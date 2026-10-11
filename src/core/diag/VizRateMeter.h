// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// VizRateMeter —— viz 链路「实得速率」的进程内计数与窗口汇总(B 线 M07b)。
//
// 【为什么要有】M12a-fix(#378)在 mac CI 的测试进程里量到:JUCE 高优先级线程 wait(16) 的实得拍距
// p50 36.8 ms / p90 90.4 ms —— 定时器在某些进程环境下会被系统拉长。viz 链路的三级全挂在 JUCE Timer 上:
//   · Output:`vizTimer_`(60Hz)驱动 VizPublisher,25 ms 闸门分频 ⇒ 标称 30Hz 发布;
//   · Monitor:Processor 的 60Hz Timer 每拍读一次 viz 段;
//   · Monitor 编辑器:WebViewHost 的 25Hz tick 把 scvb.viz 推给页面(重复帧由 JSON 比对挡掉)。
// 真实前台宿主(LUNA)里定时器是否也被拉长,CI 推不出来,只能在宿主进程里数。本文件就是数数的地方:
// 每个窗口(默认 10 s)汇总一次各路事件的实得频率与最大间隔,装了平台文件日志时写一行
// (mac:~/Library/Logs/Synchain/SCVB/<role>.log;Windows 不落盘,见 VizRateSink.h)。
//
// 【不进任何共享内存段】计数全是进程内状态;VizPlane、段布局、abi、桥契约一个字节都不动。
//
// 【窗口口径】
//   · 一个窗口 = 相邻两次「收窗」之间 note() 进来的事件(按调用先后,不按时刻比较 —— 收窗那一拍
//     之后同一毫秒里再来的事件归下一窗,不会两头都算或两头都丢)。
//   · restart 之后第一次 closeIfDue() **只定锚、不收窗**;定锚之前 note() 进来的事件不计数,只记下
//     时刻供算间隔 —— 否则第一窗会多数一个(N 个间隔数成 N+1 个事件,60Hz 记成 60.1Hz)。
//   · 频率 = 事件数 / 窗口**实长**(本次收窗时刻 − 上次收窗时刻),**不用标称长度**:定时器被拉长时
//     收窗那一拍本身也晚到,按 10 s 算会把频率算高。
//   · 最大间隔:同一路相邻两次事件的时刻差,记在后一次事件所在的窗口(跨窗的那一段也算这一窗)。
//     restart 之后的第一个事件没有前驱,不产生间隔;一窗里一个间隔都没有时日志写 `-`。
//   · 时钟回退(本次时刻早于见过的最新时刻):整窗作废、清掉间隔前驱、回到未定锚,并把回退次数
//     带进下一条汇总(`clock-back=N`)—— 不让无符号相减算出一个天文数字的窗口。生产时钟是
//     steadyNowMs()(单调),这一支只为不可信输入兜底。
//
// 【线程】不加锁;除 format() 与 OutputVizRate / MonitorVizRate 收窗时拼行之外不分配。调用方保证只在
// 一个线程上用一个实例:生产 = 消息线程上的定时器回调(以及 Monitor 编辑器的 25Hz tick),都持着各自
// Processor 的 lifecycleMutex_。**音频线程一律不碰**(processBlock 里没有任何一处调用)。
//
// 文案一律 ASCII(运行期字面量含非 ASCII 会在 CP936 机器上触发 MSVC C4819);中文只在注释里。

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>

#include "VizRateSink.h"

namespace scvb::diag
{

// 汇总窗口的标称长度(毫秒)。改它只改「多久写一行」,不改任何判据。
inline constexpr std::uint64_t kVizRateWindowMs = 10000;

class VizRateMeter
{
public:
    static constexpr std::size_t kMaxSeries = 4;

    struct Series
    {
        std::uint64_t count = 0; // 本窗事件数
        std::uint64_t maxGapMs = 0; // 本窗最大间隔(hasGap 为假时无意义)
        bool hasGap = false;
    };

    struct Window
    {
        std::uint64_t startMs = 0;
        std::uint64_t endMs = 0;
        std::array<Series, kMaxSeries> series{};
        std::uint32_t clockBackSteps = 0; // 上一次收窗(或 restart)以来的时钟回退次数

        std::uint64_t durationMs() const noexcept { return endMs - startMs; }
        // 频率 x10(Hz 的十分位,四舍五入);窗口实长为 0 或下标越界时为 0。
        std::uint64_t rateDeciHz(std::size_t index) const noexcept;
    };

    // names:1..kMaxSeries 个 ASCII 名字(日志行里的键;多出的忽略,nullptr 记成 "?")。
    // 名字指针要活得比本对象久(实际用法全是字符串字面量)。windowMs = 0 按 1 处理。
    VizRateMeter(std::initializer_list<const char*> names, std::uint64_t windowMs = kVizRateWindowMs) noexcept;

    // 作废当前窗与全部间隔前驱,回到未定锚(下一次 closeIfDue 定锚)。
    void restart() noexcept;

    // 记 events 个事件(同一时刻)。下标越界的调用忽略;events = 0 只推进时钟、不记任何东西。
    void note(std::size_t series, std::uint64_t nowMs, std::uint64_t events = 1) noexcept;

    // 到点就收窗:返回 true 并把刚结束的窗写进 out,下一窗从 nowMs 起。未定锚时只定锚、返回 false。
    bool closeIfDue(std::uint64_t nowMs, Window& out) noexcept;

    // "viz-rate <prefix> window=10.0s <a>=60.0Hz <b>=30.0Hz <a>-max=17ms <b>-max=34ms[ clock-back=N]"
    // 数字一律按整数拼(不经 printf 的 %f:宿主进程改过 LC_NUMERIC 时小数点会变成逗号)。
    std::string format(const std::string& prefix, const Window& window) const;

    std::uint64_t windowMs() const noexcept { return windowMs_; }
    std::size_t seriesCount() const noexcept { return seriesCount_; }
    bool anchored() const noexcept { return anchored_; }

private:
    // 时钟回退检测;回退时整窗作废并计数。
    void advanceClock(std::uint64_t nowMs) noexcept;

    struct Tail
    {
        std::uint64_t ms = 0;
        bool valid = false;
    };

    std::array<const char*, kMaxSeries> names_{};
    std::size_t seriesCount_ = 0;
    std::uint64_t windowMs_ = kVizRateWindowMs;

    bool anchored_ = false;
    bool seenTime_ = false;
    std::uint64_t startMs_ = 0;
    std::uint64_t latestMs_ = 0;
    std::uint32_t clockBack_ = 0;
    std::array<Series, kMaxSeries> current_{};
    std::array<Tail, kMaxSeries> tails_{};
};

// 实例标签:"<format>#<n>",n 为本二进制里的第几个实例(从 1 起;一个插件二进制只有一种角色)。
// format 由 Processor 传 juce::AudioProcessor::getWrapperTypeDescription(wrapperType)("VST3" / "AU" …),
// 这样同一宿主里同时开着 AU 与 VST3 时两份日志行分得开。nullptr / 空串记成 "?"。
std::string makeVizRateInstanceTag(const char* format);

// Output 侧:每个 vizTimer_ 拍子在 publishVizFrame 之后调一次 onBeat。
//   tick    = 定时器回调本身(看定时器有没有被拉长 / 合并);
//   publish = VizPublisher 真的发出去的帧(publishCount() 这一拍比上一拍涨了多少)。
// 发布计数取发布器自己的累计值,而不是在回调里另数一份:只有 tick() 返回 true 的那一拍才算,
// 闸门挡掉的拍子、段没开(本实例不是该组的 viz 写方)的拍子都不算。
class OutputVizRate
{
public:
    static constexpr std::size_t kTick = 0;
    static constexpr std::size_t kPublish = 1;

    explicit OutputVizRate(std::string instanceTag, std::uint64_t windowMs = kVizRateWindowMs);

    // 音频流(重)起时调(prepareToPlay,持 lifecycleMutex_):上一段的残窗作废,停表期间不算进拍距。
    void restart() noexcept;

    // publishCount = VizPublisher::publishCount()(累计值,本拍发布之后读);writerOpen = 发布器的段开着;
    // group = 当前组号(只进日志行)。收窗时若装了 sink 就交一行。返回 true = 这一拍收了一个窗。
    // ⚠ writer= / group= 只反映**收窗那一拍**的状态:窗口中途接管或让出写方(例如同组第一个 Output
    // 被移除、本实例接过 viz 段)时,那一行的 publish 落在 0 与 30Hz 之间 —— 那不是定时器被拉长,
    // 判定时器本身看 tick / tick-max。
    bool onBeat(std::uint64_t nowMs, std::uint64_t publishCount, bool writerOpen, int group);

    const VizRateMeter& meter() const noexcept { return meter_; }
    // 最近一次收的窗(测试内省;一窗都没收过时全零)。
    const VizRateMeter::Window& lastWindow() const noexcept { return last_; }
    const std::string& instanceTag() const noexcept { return tag_; }

private:
    VizRateMeter meter_;
    std::string tag_;
    VizRateMeter::Window last_{};
    std::uint64_t lastPublishCount_ = 0;
    bool havePublishBaseline_ = false;
};

// Monitor 侧:Processor 的 60Hz 定时器与编辑器的 25Hz tick 各在自己的点上调。
//   tick  = Processor 定时器回调本身(tickMessageThread);
//   poll  = 真的去读了一次 viz 段(VizPlane::read;段没 attach 的拍子不算);
//   fresh = 读到了新帧(publishMs 变了,或换组 / 重连后的第一帧);
//   push  = 编辑器真的把 scvb.viz 推给了页面(emitIfChanged 返回 true;重复帧、隐藏期不算)。
class MonitorVizRate
{
public:
    static constexpr std::size_t kTick = 0;
    static constexpr std::size_t kPoll = 1;
    static constexpr std::size_t kFresh = 2;
    static constexpr std::size_t kPush = 3;

    explicit MonitorVizRate(std::string instanceTag, std::uint64_t windowMs = kVizRateWindowMs);

    // prepareToPlay / 换组时调(都持 lifecycleMutex_):残窗作废。
    void restart() noexcept { meter_.restart(); }

    void onTick(std::uint64_t nowMs) noexcept { meter_.note(kTick, nowMs); }
    void onPoll(std::uint64_t nowMs) noexcept { meter_.note(kPoll, nowMs); }
    void onFresh(std::uint64_t nowMs) noexcept { meter_.note(kFresh, nowMs); }
    void onPush(std::uint64_t nowMs) noexcept { meter_.note(kPush, nowMs); }

    // tickMessageThread 末尾调。segState = "online" / "offline" / "abiMismatch";editorOpen = 编辑器开着
    // (push 为 0 时据此分辨「没开窗」与「开着但推不出去」)。收窗时若装了 sink 就交一行。
    // ⚠ 与 Output 侧同理:seg= / editor= 只反映收窗那一拍;窗口中途上线 / 掉线 / 开关编辑器时,
    // 那一行的 poll / fresh / push 落在两档之间,不要读成定时器被拉长(看 tick / tick-max)。
    // 换组不在此列:换组会 restart,一窗只算一个组。
    bool closeIfDue(std::uint64_t nowMs, int group, const char* segState, bool editorOpen);

    const VizRateMeter& meter() const noexcept { return meter_; }
    const VizRateMeter::Window& lastWindow() const noexcept { return last_; }
    const std::string& instanceTag() const noexcept { return tag_; }

private:
    VizRateMeter meter_;
    std::string tag_;
    VizRateMeter::Window last_{};
};

} // namespace scvb::diag
