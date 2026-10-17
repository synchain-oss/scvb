// SPDX-License-Identifier: GPL-3.0-or-later
#include "VizRateMeter.h"

#include <atomic>
#include <utility>

namespace scvb::diag
{

namespace
{
// v 是「十分之一」单位:37 -> "3.7"。
void appendTenths(std::string& out, std::uint64_t v)
{
    out += std::to_string(v / 10);
    out += '.';
    out += static_cast<char>('0' + static_cast<int>(v % 10));
}

std::uint32_t saturatingIncrement(std::uint32_t v) noexcept
{
    return v == 0xffffffffu ? v : v + 1u;
}

const char* orPlaceholder(const char* s) noexcept
{
    return (s != nullptr && s[0] != '\0') ? s : "?";
}
} // namespace

// -----------------------------------------------------------------------------
// VizRateMeter
// -----------------------------------------------------------------------------
std::uint64_t VizRateMeter::Window::rateDeciHz(std::size_t index) const noexcept
{
    const auto d = durationMs();
    if (d == 0 || index >= kMaxSeries)
        return 0;
    // count 个事件 / d 毫秒 = count * 1000 / d Hz;再 x10 取十分位,+d/2 四舍五入。
    return (series[index].count * 10000u + d / 2) / d;
}

VizRateMeter::VizRateMeter(std::initializer_list<const char*> names, std::uint64_t windowMs) noexcept
    : windowMs_(windowMs == 0 ? 1 : windowMs)
{
    for (const char* name : names)
    {
        if (seriesCount_ >= kMaxSeries)
            break;
        names_[seriesCount_++] = orPlaceholder(name);
    }
}

void VizRateMeter::restart() noexcept
{
    anchored_ = false;
    seenTime_ = false;
    startMs_ = 0;
    latestMs_ = 0;
    clockBack_ = 0;
    current_ = {};
    tails_ = {};
}

void VizRateMeter::advanceClock(std::uint64_t nowMs) noexcept
{
    if (seenTime_ && nowMs < latestMs_)
    {
        // 时钟回退:这一窗的时长与间隔都没法信了。整窗作废,回到未定锚;回退次数带进下一条汇总。
        const auto carried = saturatingIncrement(clockBack_);
        restart();
        clockBack_ = carried;
    }
    seenTime_ = true;
    latestMs_ = nowMs;
}

void VizRateMeter::note(std::size_t series, std::uint64_t nowMs, std::uint64_t events) noexcept
{
    if (series >= seriesCount_)
        return;
    advanceClock(nowMs);
    if (events == 0)
        return;
    auto& tail = tails_[series];
    if (anchored_)
    {
        auto& cur = current_[series];
        cur.count += events;
        if (tail.valid)
        {
            const auto gap = nowMs - tail.ms; // advanceClock 保证 nowMs >= tail.ms
            if (!cur.hasGap || gap > cur.maxGapMs)
            {
                cur.maxGapMs = gap;
                cur.hasGap = true;
            }
        }
    }
    tail.ms = nowMs;
    tail.valid = true;
}

bool VizRateMeter::closeIfDue(std::uint64_t nowMs, Window& out) noexcept
{
    advanceClock(nowMs);
    if (!anchored_)
    {
        anchored_ = true;
        startMs_ = nowMs;
        return false;
    }
    if (nowMs - startMs_ < windowMs_)
        return false;

    out.startMs = startMs_;
    out.endMs = nowMs;
    out.series = current_;
    out.clockBackSteps = clockBack_;

    current_ = {};
    clockBack_ = 0;
    startMs_ = nowMs;
    return true;
}

std::string VizRateMeter::format(const std::string& prefix, const Window& window) const
{
    std::string line = "viz-rate ";
    line += prefix;
    line += " window=";
    appendTenths(line, (window.durationMs() + 50) / 100); // 毫秒 -> 十分之一秒
    line += 's';
    for (std::size_t i = 0; i < seriesCount_; ++i)
    {
        line += ' ';
        line += names_[i];
        line += '=';
        appendTenths(line, window.rateDeciHz(i));
        line += "Hz";
    }
    for (std::size_t i = 0; i < seriesCount_; ++i)
    {
        line += ' ';
        line += names_[i];
        line += "-max=";
        if (window.series[i].hasGap)
        {
            line += std::to_string(window.series[i].maxGapMs);
            line += "ms";
        }
        else
        {
            line += '-';
        }
    }
    if (window.clockBackSteps > 0)
    {
        line += " clock-back=";
        line += std::to_string(window.clockBackSteps);
    }
    return line;
}

// -----------------------------------------------------------------------------
// 实例标签
// -----------------------------------------------------------------------------
std::string makeVizRateInstanceTag(const char* format)
{
    // 每个二进制一份(本函数编进各自链接的 scvb_core 副本里)。只给日志行编号用,不参与任何判据。
    static std::atomic<int> serial{0};
    const int n = serial.fetch_add(1, std::memory_order_relaxed) + 1;
    std::string tag = orPlaceholder(format);
    tag += '#';
    tag += std::to_string(n);
    return tag;
}

// -----------------------------------------------------------------------------
// OutputVizRate
// -----------------------------------------------------------------------------
OutputVizRate::OutputVizRate(std::string instanceTag, std::uint64_t windowMs)
    : meter_({"tick", "publish"}, windowMs), tag_(std::move(instanceTag))
{
}

void OutputVizRate::restart() noexcept
{
    meter_.restart();
    havePublishBaseline_ = false;
    lastPublishCount_ = 0;
}

bool OutputVizRate::onBeat(std::uint64_t nowMs, std::uint64_t publishCount, bool writerOpen, int group)
{
    meter_.note(kTick, nowMs);
    // restart 之后的第一拍只取基线:那一拍是定锚拍,它的事件本来就不计数。计数器倒退(不该发生:
    // 发布器的累计计数只增不减)同样按重取基线处理,不把无符号相减的大数记成发布。
    if (havePublishBaseline_ && publishCount >= lastPublishCount_)
        meter_.note(kPublish, nowMs, publishCount - lastPublishCount_);
    lastPublishCount_ = publishCount;
    havePublishBaseline_ = true;

    VizRateMeter::Window w;
    if (!meter_.closeIfDue(nowMs, w))
        return false;
    last_ = w;
    if (vizRateSink() != nullptr) // 没装日志(Windows 生产路径)时连行都不拼
    {
        std::string prefix = "output ";
        prefix += tag_;
        prefix += " group=";
        prefix += std::to_string(group);
        prefix += writerOpen ? " writer=on" : " writer=off";
        reportVizRate(meter_.format(prefix, w));
    }
    return true;
}

// -----------------------------------------------------------------------------
// MonitorVizRate
// -----------------------------------------------------------------------------
MonitorVizRate::MonitorVizRate(std::string instanceTag, std::uint64_t windowMs)
    : meter_({"tick", "poll", "fresh", "push"}, windowMs), tag_(std::move(instanceTag))
{
}

bool MonitorVizRate::closeIfDue(std::uint64_t nowMs, int group, const char* segState, bool editorOpen)
{
    VizRateMeter::Window w;
    if (!meter_.closeIfDue(nowMs, w))
        return false;
    last_ = w;
    if (vizRateSink() != nullptr)
    {
        std::string prefix = "monitor ";
        prefix += tag_;
        prefix += " group=";
        prefix += std::to_string(group);
        prefix += " seg=";
        prefix += orPlaceholder(segState);
        prefix += editorOpen ? " editor=on" : " editor=off";
        reportVizRate(meter_.format(prefix, w));
    }
    return true;
}

} // namespace scvb::diag
