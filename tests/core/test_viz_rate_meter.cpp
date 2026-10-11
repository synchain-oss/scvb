// SPDX-License-Identifier: GPL-3.0-or-later
// test_viz_rate_meter —— viz 实得速率的计数与窗口汇总(B 线 M07b,src/core/diag/VizRateMeter.*)。
//
// 全部用注入的逻辑时刻驱动,不看墙钟:这些格判的是「怎么数、怎么分窗、怎么算」,与本机定时器快慢无关,
// 进门禁不会变成抖动源。真实宿主里的数字由 mac 平台文件日志带回(LUNA 会话)。
//
// 每一格钉一个落点(反向注入时只红自己那一格):
//   · 定锚拍不计数(栅栏柱)            —— 「第一窗定锚」格;
//   · 收窗边界 >= 与收窗拍的归属         —— 「窗口边界」格;
//   · 频率按窗口实长                    —— 「实长」格;
//   · 零事件 / 时钟回退 / 间隔归属       —— 各自一格;
//   · Output 侧 publish 只数发布器真的发出去的帧 —— 「OutputVizRate」格(真 VizPublisher 驱动)。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "diag/VizRateMeter.h"
#include "diag/VizRateSink.h"
#include "ipc/SegmentBackendInProcess.h"
#include "output/VizPublisher.h"

using scvb::diag::MonitorVizRate;
using scvb::diag::OutputVizRate;
using scvb::diag::VizRateMeter;

namespace
{
std::vector<std::string>& capturedLines()
{
    static std::vector<std::string> lines;
    return lines;
}

void captureSink(const char* line) noexcept
{
    try
    {
        capturedLines().emplace_back(line);
    }
    catch (...)
    {
    }
}

// 本格期间把 viz-rate 槽接到 capturedLines(),结束时摘掉(不让别的格看到残留的 sink)。
struct CaptureGuard
{
    CaptureGuard()
    {
        capturedLines().clear();
        scvb::diag::setVizRateSink(&captureSink);
    }
    ~CaptureGuard()
    {
        scvb::diag::setVizRateSink(nullptr);
        capturedLines().clear();
    }
    CaptureGuard(const CaptureGuard&) = delete;
    CaptureGuard& operator=(const CaptureGuard&) = delete;
};

// 60Hz 定时器的第 k 拍(整数毫秒,向下取整):间隔在 16 / 17 ms 之间交替,第 600 拍恰好落在 10000 ms。
std::uint64_t beat60(std::uint64_t k)
{
    return (k * 1000u) / 60u;
}

bool isPrintableAscii(const std::string& s)
{
    for (const char c : s)
    {
        if (c < 0x20 || c > 0x7e)
            return false;
    }
    return true;
}
} // namespace

TEST_CASE("[M07b] VizRateMeter: the first close only anchors; the anchor beat is not counted", "[M07b][vizrate]")
{
    VizRateMeter m({"tick"}, 10000);
    std::vector<VizRateMeter::Window> closed;
    for (std::uint64_t k = 0; k <= 1200; ++k)
    {
        const auto t = beat60(k);
        m.note(0, t);
        VizRateMeter::Window w;
        if (m.closeIfDue(t, w))
            closed.push_back(w);
    }
    REQUIRE(closed.size() == 2);

    // 第一窗:锚在第 0 拍,收在第 600 拍。601 次回调、600 个间隔 —— 计 600 个才是 60.0Hz。
    // 把定锚那一拍也算进去,就成了 601 / 10 s = 60.1Hz(栅栏柱错误)。
    CHECK(closed[0].startMs == 0);
    CHECK(closed[0].endMs == 10000);
    CHECK(closed[0].series[0].count == 600);
    CHECK(closed[0].rateDeciHz(0) == 600);
    CHECK(closed[0].series[0].hasGap);
    CHECK(closed[0].series[0].maxGapMs == 17);

    // 第二窗紧接着第一窗的收窗时刻,同样 600 个。
    CHECK(closed[1].startMs == 10000);
    CHECK(closed[1].endMs == 20000);
    CHECK(closed[1].series[0].count == 600);
    CHECK(closed[1].rateDeciHz(0) == 600);
}

TEST_CASE("[M07b] VizRateMeter: a window closes exactly at its length; the closing beat's events stay in it",
          "[M07b][vizrate]")
{
    VizRateMeter m({"tick"}, 1000);
    VizRateMeter::Window w;
    CHECK_FALSE(m.closeIfDue(0, w)); // 定锚
    CHECK(m.anchored());
    m.note(0, 500);
    CHECK_FALSE(m.closeIfDue(999, w)); // 差 1 ms:不收

    m.note(0, 1000); // 收窗那一拍先记事件、后收窗:归这一窗
    REQUIRE(m.closeIfDue(1000, w)); // 恰好满一个窗长:收(>=,不是 >)
    CHECK(w.durationMs() == 1000);
    CHECK(w.series[0].count == 2);

    // 收窗之后、同一毫秒里再来的事件归下一窗(按调用先后,不按时刻比较:不重算、不漏算)。
    m.note(0, 1000);
    m.note(0, 1500);
    CHECK_FALSE(m.closeIfDue(1999, w));
    REQUIRE(m.closeIfDue(2000, w));
    CHECK(w.startMs == 1000);
    CHECK(w.series[0].count == 2);
}

TEST_CASE("[M07b] VizRateMeter: the rate divides by the real window length, not the nominal one", "[M07b][vizrate]")
{
    // 定时器被拉到 3 s 一拍:收窗那一拍本身晚到 2 s。实长 12 s、4 个事件 = 0.33Hz;
    // 按标称 10 s 算会得到 0.4Hz —— 把「被拉长」算成「没那么长」。
    VizRateMeter m({"tick"}, 10000);
    VizRateMeter::Window w;
    bool closed = false;
    for (std::uint64_t t = 0; t <= 12000 && !closed; t += 3000)
    {
        m.note(0, t);
        closed = m.closeIfDue(t, w);
    }
    REQUIRE(closed);
    CHECK(w.durationMs() == 12000);
    CHECK(w.series[0].count == 4);
    CHECK(w.rateDeciHz(0) == 3);
    CHECK(w.series[0].maxGapMs == 3000);
    CHECK(m.format("t", w) == "viz-rate t window=12.0s tick=0.3Hz tick-max=3000ms");
}

TEST_CASE("[M07b] VizRateMeter: an empty window reports 0.0Hz and no gap", "[M07b][vizrate]")
{
    VizRateMeter m({"tick", "push"}, 1000);
    VizRateMeter::Window w;
    CHECK_FALSE(m.closeIfDue(5, w));
    m.note(1, 500); // 只有 push 一路有一个事件:它也没有前驱,不产生间隔
    REQUIRE(m.closeIfDue(1005, w));
    CHECK(w.series[0].count == 0);
    CHECK(w.rateDeciHz(0) == 0);
    CHECK_FALSE(w.series[0].hasGap);
    CHECK(w.series[1].count == 1);
    CHECK_FALSE(w.series[1].hasGap);
    CHECK(m.format("x", w) == "viz-rate x window=1.0s tick=0.0Hz push=1.0Hz tick-max=- push-max=-");

    // 窗口实长为 0 的 Window(只有手搓才会出现)不做除法。
    VizRateMeter::Window zero;
    zero.series[0].count = 5;
    CHECK(zero.rateDeciHz(0) == 0);
    CHECK(zero.rateDeciHz(VizRateMeter::kMaxSeries) == 0); // 下标越界同样给 0
}

TEST_CASE("[M07b] VizRateMeter: a clock step back drops the window and re-anchors", "[M07b][vizrate]")
{
    VizRateMeter m({"tick"}, 10000);
    VizRateMeter::Window w;
    CHECK_FALSE(m.closeIfDue(5000, w)); // 锚在 5000
    m.note(0, 5100);
    m.note(0, 5200);

    // 时钟退到 4000:不能拿 4000 - 5000 的无符号差当窗长(那是一个天文数字,会当场收出一个假窗)。
    m.note(0, 4000);
    CHECK_FALSE(m.anchored());
    CHECK_FALSE(m.closeIfDue(4000, w)); // 重新定锚
    m.note(0, 5000);
    CHECK_FALSE(m.closeIfDue(13999, w));
    REQUIRE(m.closeIfDue(14000, w));
    CHECK(w.startMs == 4000);
    CHECK(w.durationMs() == 10000);
    CHECK(w.series[0].count == 1); // 回退之前的两个事件随旧窗作废;4000 那一下是定锚前的
    CHECK(w.series[0].maxGapMs == 1000); // 4000 -> 5000
    CHECK(w.clockBackSteps == 1);
    CHECK(m.format("x", w) == "viz-rate x window=10.0s tick=0.1Hz tick-max=1000ms clock-back=1");

    // 回退次数只进紧接着的那一条汇总。
    REQUIRE(m.closeIfDue(24000, w));
    CHECK(w.clockBackSteps == 0);
}

TEST_CASE("[M07b] VizRateMeter: a gap is charged to the window of its later event; restart forgets predecessors",
          "[M07b][vizrate]")
{
    VizRateMeter m({"tick"}, 1000);
    VizRateMeter::Window w;
    m.note(0, 0); // 定锚前:不计数,只当下一个事件的前驱
    CHECK_FALSE(m.closeIfDue(0, w));
    m.note(0, 100);
    m.note(0, 900);
    REQUIRE(m.closeIfDue(1000, w));
    CHECK(w.series[0].count == 2);
    CHECK(w.series[0].maxGapMs == 800);

    m.note(0, 1500); // 与上一窗的 900 相隔 600:跨窗的这一段归这一窗
    REQUIRE(m.closeIfDue(2000, w));
    CHECK(w.series[0].count == 1);
    CHECK(w.series[0].maxGapMs == 600);

    // restart(音频流重起)之后,停表前的那个事件不再是前驱:2100 -> 2200 是 100,不是 1500 -> 2200。
    m.restart();
    CHECK_FALSE(m.anchored());
    m.note(0, 2100);
    CHECK_FALSE(m.closeIfDue(2100, w));
    m.note(0, 2200);
    REQUIRE(m.closeIfDue(3100, w));
    CHECK(w.series[0].count == 1);
    CHECK(w.series[0].maxGapMs == 100);
}

TEST_CASE("[M07b] VizRateMeter: the line is fixed ASCII with integer-only numbers", "[M07b][vizrate]")
{
    VizRateMeter m({"tick", "publish"});
    CHECK(m.windowMs() == scvb::diag::kVizRateWindowMs);
    CHECK(m.seriesCount() == 2);

    VizRateMeter::Window w;
    w.startMs = 0;
    w.endMs = 10000;
    w.series[0] = {600, 17, true};
    w.series[1] = {300, 34, true};
    const auto line = m.format("output VST3#1 group=1 writer=on", w);
    CHECK(line == "viz-rate output VST3#1 group=1 writer=on window=10.0s tick=60.0Hz publish=30.0Hz tick-max=17ms "
                  "publish-max=34ms");
    CHECK(isPrintableAscii(line));

    // 窗长四舍五入到 0.1 s;频率四舍五入到 0.1Hz。
    w.endMs = 10049;
    CHECK(m.format("x", w).find(" window=10.0s ") != std::string::npos);
    w.endMs = 10050;
    CHECK(m.format("x", w).find(" window=10.1s ") != std::string::npos);
    w.endMs = 10000;
    w.series[0].count = 599; // 59.9Hz
    w.series[1].count = 1;
    CHECK(m.format("x", w).find(" tick=59.9Hz publish=0.1Hz ") != std::string::npos);

    // 名字多于 kMaxSeries 的忽略;nullptr 名字记成 "?";窗长 0 按 1 ms 处理(不除零、每拍都能收)。
    VizRateMeter many({"a", "b", nullptr, "d", "e"}, 0);
    CHECK(many.seriesCount() == VizRateMeter::kMaxSeries);
    CHECK(many.windowMs() == 1);
    VizRateMeter::Window empty;
    empty.endMs = 1;
    CHECK(many.format("p", empty) == "viz-rate p window=0.0s a=0.0Hz b=0.0Hz ?=0.0Hz d=0.0Hz a-max=- b-max=- ?-max=- "
                                     "d-max=-");
    many.note(9, 5); // 下标越界:忽略,不崩
}

TEST_CASE("[M07b] instance tags number the instances of one binary", "[M07b][vizrate]")
{
    const auto a = scvb::diag::makeVizRateInstanceTag("VST3");
    const auto b = scvb::diag::makeVizRateInstanceTag("AU");
    const auto q = scvb::diag::makeVizRateInstanceTag(nullptr);
    REQUIRE(a.rfind("VST3#", 0) == 0);
    REQUIRE(b.rfind("AU#", 0) == 0);
    REQUIRE(q.rfind("?#", 0) == 0);
    const auto na = std::stoi(a.substr(5));
    CHECK(std::stoi(b.substr(3)) == na + 1);
    CHECK(std::stoi(q.substr(2)) == na + 2);
}

TEST_CASE("[M07b] viz-rate sink: no sink is a no-op; an installed sink gets the line verbatim", "[M07b][vizrate]")
{
    scvb::diag::setVizRateSink(nullptr);
    scvb::diag::reportVizRate("dropped on the floor"); // 不崩、不留痕
    {
        CaptureGuard capture;
        CHECK(scvb::diag::vizRateSink() == &captureSink);
        scvb::diag::reportVizRate("viz-rate x window=1.0s");
        REQUIRE(capturedLines().size() == 1);
        CHECK(capturedLines()[0] == "viz-rate x window=1.0s");
    }
    CHECK(scvb::diag::vizRateSink() == nullptr);
}

namespace
{
constexpr double kSr = 48000.0;

struct PublishRig
{
    scvb::SegmentBackendInProcess backend;
    scvb::output::VizPublisher pub{backend, 3};
    scvb::output::VizPublishInput in;

    explicit PublishRig(bool open)
    {
        in.sampleRate = kSr;
        in.versionActive = 1;
        if (open)
            REQUIRE(pub.open() == scvb::InitResult::kOk);
    }

    // OutputProcessor 的 vizTimer_ 一拍:publishVizFrame(isOpen + due 闸门 + tick)之后 onBeat。
    bool beat(OutputVizRate& rate, std::uint64_t t)
    {
        if (pub.isOpen() && pub.due(t))
            pub.tick(t, in);
        return rate.onBeat(t, pub.publishCount(), pub.isOpen(), 3);
    }
};
} // namespace

TEST_CASE("[M07b] OutputVizRate counts the frames the real VizPublisher actually published", "[M07b][vizrate]")
{
    CaptureGuard capture;
    constexpr std::uint64_t kT0 = 1000;

    SECTION("60Hz driver, 25 ms gate: tick 60Hz, publish 30Hz")
    {
        PublishRig rig(true);
        OutputVizRate rate("VST3#7", 10000);
        int closed = 0;
        for (std::uint64_t k = 0; k <= 600; ++k)
            closed += rig.beat(rate, kT0 + beat60(k)) ? 1 : 0;
        REQUIRE(closed == 1);
        // 发布器在第 0 拍(首帧)与此后每个偶数拍发帧;第 0 拍是定锚拍,不计。
        CHECK(rig.pub.publishCount() == 301);
        CHECK(rate.lastWindow().series[OutputVizRate::kTick].count == 600);
        CHECK(rate.lastWindow().series[OutputVizRate::kPublish].count == 300);
        REQUIRE(capturedLines().size() == 1);
        CHECK(capturedLines()[0] == "viz-rate output VST3#7 group=3 writer=on window=10.0s tick=60.0Hz publish=30.0Hz "
                                    "tick-max=17ms publish-max=34ms");
    }

    SECTION("timer stretched to 36 ms: every beat clears the gate, both rates fall to 27.8Hz")
    {
        PublishRig rig(true);
        OutputVizRate rate("AU#1", 10000);
        int closed = 0;
        for (std::uint64_t k = 0; k <= 278; ++k)
            closed += rig.beat(rate, kT0 + 36 * k) ? 1 : 0;
        REQUIRE(closed == 1);
        CHECK(rate.lastWindow().durationMs() == 10008);
        REQUIRE(capturedLines().size() == 1);
        CHECK(capturedLines()[0] == "viz-rate output AU#1 group=3 writer=on window=10.0s tick=27.8Hz publish=27.8Hz "
                                    "tick-max=36ms publish-max=36ms");
    }

    SECTION("not the viz writer (segment never opened): publish 0, writer=off")
    {
        PublishRig rig(false);
        OutputVizRate rate("VST3#2", 10000);
        for (std::uint64_t k = 0; k <= 600; ++k)
            rig.beat(rate, kT0 + beat60(k));
        CHECK(rig.pub.publishCount() == 0);
        REQUIRE(capturedLines().size() == 1);
        CHECK(capturedLines()[0] == "viz-rate output VST3#2 group=3 writer=off window=10.0s tick=60.0Hz publish=0.0Hz "
                                    "tick-max=17ms publish-max=-");
    }

    SECTION("restart (prepareToPlay again): the stopped stretch is not a beat gap")
    {
        PublishRig rig(true);
        OutputVizRate rate("VST3#3", 10000);
        for (std::uint64_t k = 0; k <= 300; ++k) // 5 s 后宿主停了音频流
            rig.beat(rate, kT0 + beat60(k));
        CHECK(capturedLines().empty());

        rate.restart(); // 10 s 后重新 prepareToPlay
        const std::uint64_t resume = kT0 + 15000;
        int closed = 0;
        for (std::uint64_t k = 0; k <= 600; ++k)
            closed += rig.beat(rate, resume + beat60(k)) ? 1 : 0;
        REQUIRE(closed == 1);
        CHECK(rate.lastWindow().startMs == resume);
        REQUIRE(capturedLines().size() == 1);
        // 停表那 10 s 不进 tick-max;publish 计数从重起后的基线算(重起那一拍是定锚拍)。
        CHECK(capturedLines()[0] == "viz-rate output VST3#3 group=3 writer=on window=10.0s tick=60.0Hz publish=30.0Hz "
                                    "tick-max=17ms publish-max=34ms");
    }

    SECTION("no sink installed: windows still close, nothing is formatted or reported")
    {
        scvb::diag::setVizRateSink(nullptr);
        PublishRig rig(true);
        OutputVizRate rate("VST3#4", 10000);
        int closed = 0;
        for (std::uint64_t k = 0; k <= 600; ++k)
            closed += rig.beat(rate, kT0 + beat60(k)) ? 1 : 0;
        CHECK(closed == 1);
        CHECK(capturedLines().empty());
    }
}

TEST_CASE("[M07b] MonitorVizRate names the group, segment state and editor flag", "[M07b][vizrate]")
{
    CaptureGuard capture;
    MonitorVizRate rate("AU#2", 1000);
    rate.onTick(0);
    CHECK_FALSE(rate.closeIfDue(0, 2, "online", true)); // 定锚
    for (std::uint64_t t = 50; t <= 1000; t += 50)
    {
        if (t % 100 == 0)
        {
            rate.onTick(t);
            rate.onPoll(t);
        }
        if (t % 200 == 0)
            rate.onFresh(t);
        if (t % 250 == 0)
            rate.onPush(t);
        rate.closeIfDue(t, 2, "online", true);
    }
    REQUIRE(capturedLines().size() == 1);
    CHECK(capturedLines()[0] ==
          "viz-rate monitor AU#2 group=2 seg=online editor=on window=1.0s tick=10.0Hz poll=10.0Hz "
          "fresh=5.0Hz push=4.0Hz tick-max=100ms poll-max=100ms fresh-max=200ms push-max=250ms");
    CHECK(rate.lastWindow().series[MonitorVizRate::kPush].count == 4);

    // 段状态缺省 / 编辑器没开。
    for (std::uint64_t t = 1100; t <= 2000; t += 100)
    {
        rate.onTick(t);
        rate.closeIfDue(t, 5, nullptr, false);
    }
    REQUIRE(capturedLines().size() == 2);
    CHECK(capturedLines()[1].rfind("viz-rate monitor AU#2 group=5 seg=? editor=off window=1.0s tick=10.0Hz poll=0.0Hz",
                                   0) == 0);
}
