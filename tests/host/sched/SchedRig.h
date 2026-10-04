// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SchedRig —— 宿主调度模拟器(A 线 A-2)。header-only,只给 scvb_host_tests 的 sched 用例用。
//
// 一台「机器」:1 个真 ScvbOutputAudioProcessor + N 个真 mono(可切 stereo)ScvbInputAudioProcessor,
// 经**真实**共享内存段通信(与 test_host_harness.cpp 的 Rig / MonoMultiRig 同一条路)。和那两台的区别是
// 本台扮演的是一个**会调度**的宿主:每个实例各有自己的渲染游标与提前量,预取轨成块超前、live 轨跟着
// Output 走,时间线经 Cycle / 定位 / 停走带变换,宿主还可能按自己的策略停调某个实例。
//
// ## 组成
//
//   · TransportMap:游标 → 时间线的**纯函数**(Cycle 回绕、定位、停 / 起走带)。所有实例共用一张。
//   · LanePlayHead:每个实例一个,带自己的连续渲染游标 c_i;时间线位置 = map(c_i)。它就是喂给该实例的
//     juce::AudioPlayHead。块在任何不连续处(循环点、定位点、停 / 起点、策略区间边沿)切开 —— 一块之内
//     时间线恒连续,这是 Input 写环与 Output 读环都默认的前提。
//   · HostScheduler(SchedRig::stepCycle,经 runCycles / runFor / runUntil / settle 驱动):每个 Output 周期
//       ① 预取轨突发处理到 c_i ≥ C_O + L_i + b_out(块长 b_in,默认 1024,可超出);
//       ② live 轨同周期先处理 Input(块长 = Output 块长);
//       ③ 断言图依赖不变式:所有送往总线的 Input 都已处理到 Output 本块要读的位置
//          (C_O + readerOffset + n_out;readerOffset 平时为 0,即「都已到 C_O + b_out」);
//       ④ 再处理 Output。
//     故意违反(violateGraphOrder)时显式标注:标注过的违反只记账,没标注的违反直接 FAIL_CHECK。
//   · 总线输入真实化:每条 lane 的 Input 输出按游标(= 连续时间)入 FIFO;Output 的输入缓冲 = 各 lane
//     FIFO 求和。lane 进总线用**宿主侧可辨声像**:只进 L、增益 0.5(presence::kRawBusGainL/R)。
//     SCVB 混音是居中的(L、R 都有),于是「未平衡原声」与「SCVB 混音」在 L/R 判据上分得开。
//     现有 Rig 给 Output 喂空缓冲,测不出直通、双路叠加,也做不了「看总线静音就停调」—— 本台补上。
//   · 节拍:pace=1 按墙钟实时(runDispatchLoopUntil,500ms / 200ms / 5s 这类墙钟门限的场景只能用它);
//     pace>1 加速(只适合纯时间线场景);pace<=0 不按节拍,每周期泵 unpacedPumpMs 毫秒(0 = 不泵,
//     [M] 冻结)。offline(on) 是另一维:对所有实例 setNonRealtime(on)。
//   · 记录:每个 Output 块的 L/R、总线输入 L/R 与时间线位置;每个 [M] 拍(本台每次泵完消息、
//     间隔 ≥ beatMinGapMs 时采一拍)的 connSnapshot()(suspended)、misalignCount、gapCount、trackPeak;
//     每条 lane 已处理的块日志;逐帧判定接 tests/support/presence_meter.h。
//
// ## 时间模型(写清楚,场景卡按这个读数)
//
//   · 游标 = 「该样本送到总线的时刻」(渲染时间,从 0 起的样本数)。lane i 的 FIFO 按游标存,所以
//     预取轨提前算出来的音频照样在正确的时刻进总线。
//   · 静态的时间线结构(Cycle 回绕)写在 map 里,预取轨会**提前**跨过循环点 —— 这正是 H1 的形状。
//   · 用户操作(定位 / 停 / 起 / 改 Cycle)发生在某个 Output 游标 a 上:生效游标 e = a(定位另加
//     staleOutBlocks × b_out),所有游标已超过 e 的 lane **冲刷**回 e(FIFO 截断、游标回退,重新渲染)。
//     即:宿主丢掉已预取的旧音频重新预取;Output 先把 k 个已在途的旧块放完再换到新位置。
//   · readerOffset = Δ:Output 报给插件的时间线 = map(C_O + Δ),总线内容仍按 C_O。模拟「Aux 位置没做
//     延迟补偿」一类读方超前(H3)。逐帧判定按**总线时间** map(C_O) 对齐 —— 那才是听者听到的位置。
//   · 起播怪癖(quirk):只改**报给插件**的时间线位置(覆盖接下来几次被调用的块),不改游标与总线内容。
//
// ## 停调策略(宿主不调用某个实例时,它的那一路怎么出声)
//
//   · noRegion(lane,[g0,g1)):时间线落在区间内时宿主**不调用**该 Input,该轨宿主输出为静音
//     (Logic 无 region)。该轨源信号在区间内也视为静音(判定期望 = 缺席)。
//   · silentRegion(lane,[g0,g1)):源信号是静音,宿主照常调用(喂零)。给 inputSilenceSuspend 用。
//   · inputSilenceSuspend(on):Cubase 式,本块源信号静音就不调用 Input(输出静音)。
//   · outputSilenceTail(T):Logic / FL 式,总线输入连续静音超过 T 个样本就不调用 Output,
//     Output 那一路原样直通总线输入。
//   · chainSuspend(T, by=in|out|both):整条链停调,**只做表征**(by=in:所有 lane 源信号静音 > T;
//     by=out:总线输入静音 > T;both:两者同时)。停调期间 Input 与 Output 都不调用,宿主原样直通。
//   · bypassInput(lane,on):宿主旁路该 Input —— 不调用,宿主把该轨源信号原样送进总线
//     (JUCE 默认的 processBlockBypassed 就是直通)。写头因此停住,Output 那边读不到这一轨;
//     而总线上这条轨的原声**是在的** —— 「判定经过真实 Output 混音路径」的自测靠的就是这个反差。
//
// ## 不覆盖的(免得被当成全称)
//
//   · 宿主多线程:本台单线程串行调用各实例(音频「线程」与 [M] 交替),不模拟真正的并发撕裂;
//     那一层由 tests/core 的并发用例与 A-3 的 oracle 负责。
//   · outputFirst 违反时,Output 先于 live 轨处理,本周期那几条 lane 的总线输入尚未产生 ⇒ 按零计
//     (busUnderflowSamples 记账),不去模拟真实宿主可能的「晚一块」反馈延迟。
//   · lane 进总线只取第 0 声道(stereo lane 的两声道内容相同),R 恒为 0。
//   · [M] 拍是本台**泵消息时**采的样,不是挂在 Output 的 Timer 上(不改 src/);两次采样间隔
//     ≥ beatMinGapMs(默认 20ms,约半拍),足够抓住持续 ≥ 1s 的失准 / 挂起状态,抓不住单拍闪烁。
//
// ## 纪律
//
//   · 不改 src/,只用公开接口:processBlock、setNonRealtime、connSnapshot、misalignCount、gapCount、
//     meterSnapshot(外加建台必需的 setGroupId / setChannelId / setBusesLayout / prepareToPlay 等)。
//   · 框架里不写 Win32 API(留给 mac 线在 POSIX 后端上直接开启)。同机独占由本二进制里
//     tests/support/exclusive_guard.h 注册的进程互斥负责(test_host_harness.cpp 与 test_host_sched.cpp
//     各引一次,头注说明了重复引入安全),本头不重复取锁。
//   · 组号取 1..7,8 是 kNoWriterGroup 保留组(test_host_harness.cpp 头注);构造时**读回**组号断言
//     (组号越界会静默回落 g1,SL-324)。默认 6:host 套件里各 rig 串行、段随实例析构,同进程无并发。
//   · 运行期文案一律 ASCII(中文字面量在本机 CP936 上会触发 C4819);中文只在注释里。

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "support/presence_meter.h"

#include "InputProcessor.h"
#include "OutputProcessor.h"

namespace scvb::testsupport::sched
{

namespace presence = scvb::testsupport::presence;

inline constexpr double kSchedSr = 48000.0;
inline constexpr int kSchedDefaultGroup = 6;
// test_host_harness.cpp 的 kNoWriterGroup:恒无写方的保留组,本台绝不使用。
inline constexpr int kSchedReservedGroup = 8;
inline constexpr int kSchedMaxLanes = 15;
// 「没有边界」的哨兵。取 int64 上限的 1/4,做加减不会溢出。
inline constexpr std::int64_t kFar = std::numeric_limits<std::int64_t>::max() / 4;
// 调度操作里「就是现在」(当前 Output 游标)的哨兵。
inline constexpr std::int64_t kNow = -1;

inline std::int64_t secondsToSamples(double s, double sr = kSchedSr)
{
    return static_cast<std::int64_t>(s * sr + 0.5);
}

inline double wallMs()
{
    return juce::Time::getMillisecondCounterHiRes();
}

// 时间线半开区间 [begin, end)。
struct Range
{
    std::int64_t begin = 0;
    std::int64_t end = 0;

    bool contains(std::int64_t t) const { return t >= begin && t < end; }
    bool overlaps(std::int64_t b, std::int64_t e) const { return b < end && begin < e; }
    bool covers(std::int64_t b, std::int64_t e) const { return begin <= b && e <= end; }
};

inline bool anyContains(const std::vector<Range>& rs, std::int64_t t)
{
    return std::any_of(rs.begin(), rs.end(), [t](const Range& r) { return r.contains(t); });
}

// ===========================================================================
// TransportMap —— 游标 → (时间线位置, 是否在放, Cycle 状态) 的纯函数
// ===========================================================================

enum class TransportKind
{
    Seek, // t := pos(走带状态不变)
    Stop, // playing := false,t 停在当前值
    Start, // playing := true;hasPos 时 t := pos
    Cycle // Cycle 区间 := [pos, loopEnd);loopEnd <= pos 表示关 Cycle
};

struct TransportEvent
{
    std::int64_t at = 0; // 生效游标
    TransportKind kind = TransportKind::Seek;
    std::int64_t pos = 0;
    std::int64_t loopEnd = 0;
    bool hasPos = false;
};

struct MapPoint
{
    std::int64_t t = 0;
    bool playing = true;
    bool cycleOn = false;
    std::int64_t loopB = 0;
    std::int64_t loopE = 0;

    // 本点是否处在「走到 loopE 就回绕」的状态(在放、Cycle 有效、还没越过 loopE)。
    bool wrapping() const { return playing && cycleOn && loopB < loopE && t < loopE; }
};

class TransportMap
{
public:
    explicit TransportMap(std::int64_t startT = 0, bool playing = true)
    {
        initial_.t = startT;
        initial_.playing = playing;
        rebuild();
    }

    // 按生效游标插入(同一游标上的多个事件按加入顺序依次生效)。
    void add(const TransportEvent& ev)
    {
        const auto it = std::upper_bound(events_.begin(), events_.end(), ev.at,
                                         [](std::int64_t a, const TransportEvent& e) { return a < e.at; });
        events_.insert(it, ev);
        rebuild();
    }

    const std::vector<TransportEvent>& events() const { return events_; }

    MapPoint at(std::int64_t c) const { return advance(segs_[segIndex(c)], c); }

    // c 之后(严格大于 c)第一个不连续点的游标:下一个事件、下一次 Cycle 回绕,或时间线到达
    // cuts 里某个位置(只看当前这段线性区间 —— 回绕之后的那段等走到了再算)。没有就返回 kFar。
    std::int64_t nextBoundary(std::int64_t c, const std::vector<std::int64_t>& cuts = {}) const
    {
        const std::size_t idx = segIndex(c);
        std::int64_t best = idx + 1 < segs_.size() ? segs_[idx + 1].c0 : kFar;
        const Seg& s = segs_[idx];
        const MapPoint p = advance(s, c);
        if (!p.playing)
        {
            return best;
        }
        std::int64_t pieceEnd = kFar;
        if (s.p0.wrapping())
        {
            const std::int64_t first = s.c0 + (s.p0.loopE - s.p0.t);
            const std::int64_t len = s.p0.loopE - s.p0.loopB;
            pieceEnd = c < first ? first : first + ((c - first) / len + 1) * len;
        }
        best = std::min(best, pieceEnd);
        for (std::int64_t g : cuts)
        {
            if (g > p.t)
            {
                best = std::min(best, c + (g - p.t));
            }
        }
        return best;
    }

private:
    struct Seg
    {
        std::int64_t c0 = 0;
        MapPoint p0;
    };

    static MapPoint advance(const Seg& s, std::int64_t c)
    {
        MapPoint p = s.p0;
        const std::int64_t d = c - s.c0;
        if (!p.playing || d <= 0)
        {
            return p;
        }
        if (s.p0.wrapping())
        {
            const std::int64_t first = s.p0.loopE - s.p0.t;
            if (d < first)
            {
                p.t = s.p0.t + d;
            }
            else
            {
                p.t = s.p0.loopB + (d - first) % (s.p0.loopE - s.p0.loopB);
            }
            return p;
        }
        p.t = s.p0.t + d;
        return p;
    }

    std::size_t segIndex(std::int64_t c) const
    {
        // 最后一个 c0 <= c 的段。segs_[0].c0 恒为 0;负游标按 0 段处理。
        const auto it =
            std::upper_bound(segs_.begin(), segs_.end(), c, [](std::int64_t a, const Seg& s) { return a < s.c0; });
        return it == segs_.begin() ? 0 : static_cast<std::size_t>(std::distance(segs_.begin(), it) - 1);
    }

    void rebuild()
    {
        segs_.clear();
        segs_.push_back(Seg{0, initial_});
        for (const TransportEvent& ev : events_)
        {
            MapPoint p = advance(segs_.back(), ev.at);
            switch (ev.kind)
            {
            case TransportKind::Seek:
                p.t = ev.pos;
                break;
            case TransportKind::Stop:
                p.playing = false;
                break;
            case TransportKind::Start:
                p.playing = true;
                if (ev.hasPos)
                {
                    p.t = ev.pos;
                }
                break;
            case TransportKind::Cycle:
                p.cycleOn = ev.loopEnd > ev.pos;
                p.loopB = ev.pos;
                p.loopE = ev.loopEnd;
                break;
            }
            segs_.push_back(Seg{ev.at, p});
        }
    }

    MapPoint initial_;
    std::vector<TransportEvent> events_;
    std::vector<Seg> segs_;
};

// ===========================================================================
// 起播怪癖:覆盖「报给插件」的时间线位置
// ===========================================================================

struct Quirk
{
    std::vector<std::int64_t> reported; // 接下来每次被调用的块依次报这些位置,用完恢复正常

    // Output / Input 起播头 k 块拿到同一个离谱的大值(Logic 论坛 66945 那种 540544)。
    static Quirk garbage(std::int64_t value, int blocks)
    {
        Quirk q;
        q.reported.assign(static_cast<std::size_t>(std::max(blocks, 0)), value);
        return q;
    }
    // 预备拍:从 start(负数)起,每块报的位置只前进 step(块长不变),直到 >= 0 为止。
    static Quirk preroll(std::int64_t start, std::int64_t step)
    {
        Quirk q;
        for (std::int64_t t = start; t < 0 && step > 0; t += step)
        {
            q.reported.push_back(t);
        }
        return q;
    }
    // 头两块报 first、second(例:-179 → 1868,两者之差不等于块长)。
    static Quirk offByOne(std::int64_t first, std::int64_t second)
    {
        Quirk q;
        q.reported = {first, second};
        return q;
    }
};

// ===========================================================================
// LanePlayHead —— 每实例一个:连续渲染游标 + 报给插件的位置
// ===========================================================================

struct BlockPos
{
    std::int64_t cursor = 0;
    int n = 0;
    MapPoint bus; // 总线时间 = map(cursor):内容与判定按它对齐
    std::int64_t tReported = 0; // 报给插件的 timeInSamples(readerOffset / quirk 之后)
    bool playingReported = true;
    bool quirked = false;
};

class LanePlayHead final : public juce::AudioPlayHead
{
public:
    LanePlayHead(const TransportMap& map, double sr) : map_(map), sr_(sr) {}

    std::int64_t cursor() const { return cursor_; }
    void setCursor(std::int64_t c) { cursor_ = c; }
    std::int64_t readerOffset() const { return readerOffset_; }
    void setReaderOffset(std::int64_t d) { readerOffset_ = d; }
    void pushQuirk(const Quirk& q) { quirks_.insert(quirks_.end(), q.reported.begin(), q.reported.end()); }
    std::size_t pendingQuirks() const { return quirks_.size(); }

    // 从当前游标起,不越过任何不连续点的最大块长(<= limit)。cuts 是总线时间线上的额外切点。
    int maxBlock(int limit, const std::vector<std::int64_t>& cuts = {}) const
    {
        std::int64_t nb = map_.nextBoundary(cursor_, cuts) - cursor_;
        if (readerOffset_ != 0)
        {
            nb = std::min(nb, map_.nextBoundary(cursor_ + readerOffset_) - (cursor_ + readerOffset_));
        }
        return static_cast<int>(std::max<std::int64_t>(1, std::min<std::int64_t>(limit, nb)));
    }

    // 定下本块的位置(还不消耗 quirk)。
    BlockPos plan(int n) const
    {
        BlockPos bp;
        bp.cursor = cursor_;
        bp.n = n;
        bp.bus = map_.at(cursor_);
        const MapPoint rep = readerOffset_ != 0 ? map_.at(cursor_ + readerOffset_) : bp.bus;
        bp.tReported = rep.t;
        bp.playingReported = rep.playing;
        return bp;
    }

    // 本块确实要调用插件:套上 quirk(若有)并把位置发布给 getPosition()。
    void arm(BlockPos& bp)
    {
        if (!quirks_.empty())
        {
            bp.tReported = quirks_.front();
            bp.quirked = true;
            quirks_.pop_front();
        }
        const MapPoint loop = readerOffset_ != 0 ? map_.at(cursor_ + readerOffset_) : bp.bus;
        armed_ = bp;
        armedLoop_ = loop;
    }

    void advance(int n) { cursor_ += n; }

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setTimeInSamples(armed_.tReported);
        p.setTimeInSeconds(static_cast<double>(armed_.tReported) / sr_);
        p.setIsPlaying(armed_.playingReported);
        p.setBpm(bpm_);
        p.setPpqPosition(static_cast<double>(armed_.tReported) / sr_ * (bpm_ / 60.0));
        const bool looping = armedLoop_.cycleOn && armedLoop_.loopB < armedLoop_.loopE;
        p.setIsLooping(looping);
        if (looping)
        {
            LoopPoints lp;
            lp.ppqStart = static_cast<double>(armedLoop_.loopB) / sr_ * (bpm_ / 60.0);
            lp.ppqEnd = static_cast<double>(armedLoop_.loopE) / sr_ * (bpm_ / 60.0);
            p.setLoopPoints(lp);
        }
        return p;
    }

private:
    const TransportMap& map_;
    double sr_ = kSchedSr;
    double bpm_ = 120.0;
    std::int64_t cursor_ = 0;
    std::int64_t readerOffset_ = 0;
    std::deque<std::int64_t> quirks_;
    BlockPos armed_;
    MapPoint armedLoop_;
};

// ===========================================================================
// 配置
// ===========================================================================

struct LaneCfg
{
    bool live = true; // live:跟 Output 同周期、同块长;否则为预取轨
    std::int64_t lead = 0; // 预取提前量 L_i(样本;live 时忽略)
    int blockIn = 1024; // 预取块长 b_in
    int prepareBlock = 0; // prepareToPlay 的块长;0 = max(blockIn, outBlock)
    bool stereo = false;
    bool toBus = true; // 该轨输出是否送进 Output 所在总线(H3「人声轨没送进总线」置 false)
    double amp = presence::kDefaultLaneAmp;
    std::vector<Range> noRegion; // 时间线区间:宿主不调用、源信号视为静音
    std::vector<Range> silent; // 时间线区间:源信号静音、宿主照常调用(喂零)
};

enum class ChainBy
{
    In,
    Out,
    Both
};

struct SchedCfg
{
    std::vector<LaneCfg> lanes = std::vector<LaneCfg>(3);
    int group = kSchedDefaultGroup;
    double sr = kSchedSr;
    int outBlock = 1024; // b_out
    int outPrepareBlock = 0; // 0 = outBlock
    std::int64_t startT = 0;
    bool startPlaying = true;
    double pace = 4.0; // 1 = 实时;>1 加速;<=0 不按节拍
    int unpacedPumpMs = 1; // pace<=0 时每周期泵几毫秒(0 = 不泵)
    bool offline = false; // 建台时就 setNonRealtime(true)
    double beatMinGapMs = 20.0;
    bool failOnUndeclared = true; // 未标注的图依赖违反直接 FAIL_CHECK
};

inline SchedCfg liveLanes(int n)
{
    SchedCfg c;
    c.lanes.assign(static_cast<std::size_t>(n), LaneCfg{});
    return c;
}

// ===========================================================================
// 记录
// ===========================================================================

enum class CallReason
{
    Called,
    NoRegion, // 宿主不调用,输出静音
    Bypassed, // 宿主旁路,源信号直通
    ChainSuspended, // 整链停调,源信号直通
    SilenceSuspended, // 源信号静音被停调,输出静音
    OutputSilenceTail // (只用于 Output)总线静音尾巴后停调,总线输入直通
};

inline const char* toString(CallReason r)
{
    switch (r)
    {
    case CallReason::Called:
        return "called";
    case CallReason::NoRegion:
        return "noRegion";
    case CallReason::Bypassed:
        return "bypassed";
    case CallReason::ChainSuspended:
        return "chainSuspended";
    case CallReason::SilenceSuspended:
        return "silenceSuspended";
    case CallReason::OutputSilenceTail:
        return "outputSilenceTail";
    }
    return "?";
}

struct LaneBlock
{
    std::int64_t cursor = 0;
    int n = 0;
    std::int64_t tBus = 0;
    std::int64_t tReported = 0;
    bool playing = true;
    bool live = true;
    bool contentActive = false;
    bool quirked = false;
    CallReason reason = CallReason::Called;
};

struct LaneInterval
{
    std::int64_t cursor0 = 0;
    std::int64_t cursor1 = 0;
    std::int64_t t0 = 0; // 报给插件的位置
    std::int64_t t1 = 0;
    CallReason reason = CallReason::Called;
};

struct OutBlock
{
    std::int64_t cursor = 0;
    int n = 0;
    std::int64_t tBus = 0;
    bool playingBus = true;
    std::int64_t tReported = 0;
    bool playingReported = true;
    bool quirked = false;
    CallReason reason = CallReason::Called;
    bool violation = false; // 本周期图依赖不变式不成立
    bool declared = false; // ……且是标注过的故意违反
};

struct LaneBeat
{
    std::uint32_t slotState = 0;
    std::uint32_t heartbeatAgeMs = 0;
    bool suspended = false;
    std::uint32_t misalign = 0;
    std::uint32_t gap = 0;
    float trackPeak = 0.0f;
};

struct MBeat
{
    double wallMs = 0.0;
    std::int64_t outCursor = 0;
    std::vector<LaneBeat> lanes;
};

struct InvariantHit
{
    std::int64_t cycle = 0;
    std::int64_t outCursor = 0;
    int lane = 0;
    std::int64_t laneCursor = 0;
    std::int64_t required = 0;
    bool declared = false;
};

struct InvariantStats
{
    std::int64_t checked = 0; // 检查过的周期数
    std::int64_t declared = 0; // 不成立且已标注的周期数
    std::int64_t undeclared = 0; // 不成立且未标注的周期数
    std::vector<InvariantHit> hits;

    int hitsFor(int lane, bool declaredOnly) const
    {
        return static_cast<int>(std::count_if(hits.begin(), hits.end(), [lane, declaredOnly](const InvariantHit& h) {
            return h.lane == lane && (!declaredOnly || h.declared);
        }));
    }
};

enum class GraphViolationKind
{
    None,
    OutputFirst, // 本周期 Output 先于 live 轨处理
    ReaderOffset // Output 报给插件的位置超前 offset 个样本(读方超前,H3 形状)
};

struct GraphViolation
{
    GraphViolationKind kind = GraphViolationKind::None;
    int cycles = -1; // 持续几个 Output 周期;<0 = 一直
    std::int64_t offset = 0; // ReaderOffset 用
    bool annotate = true; // false = 不标注(只给自测用:证明未标注的违反会被抓到)
};

enum class LiveSwitch
{
    Rewind, // 游标超过新目标就退回目标(丢掉已预取的音频,Input 看到时间线回跳);落后则成块追上
    Drain, // 游标绝不移动:超前就空等 Output 追上,落后就成块追上(时间线连续)
    Jump // 游标直接设到新目标(前跳留下的总线空档按静音)
};

// ===========================================================================
// 逐帧判定
// ===========================================================================

struct FrameVerdict
{
    int span = 0;
    std::int64_t t = 0; // 帧起点(总线时间线)
    std::int64_t cursor = 0; // 帧起点对应的 Output 游标
    int lane = 0;
    presence::Verdict verdict = presence::Verdict::Absent;
    presence::Why why = presence::Why::LowEnergy;
    presence::Source source = presence::Source::None;
    bool expectActive = true;
    bool edge = false; // 帧跨在该轨源信号「有 / 无」的边沿上(判定照做,严格断言请跳过)
};

class Analysis
{
public:
    std::vector<FrameVerdict> frames;
    int spans = 0;
    int dirtyFrames = 0; // 有宽带泄漏(帧内硬切)的帧
    int controlNonAbsent = 0; // 零值对照频点不是 Absent 的帧

    template<typename Pred>
    int countIf(int lane, std::int64_t c0, std::int64_t c1, bool skipEdge, Pred pred) const
    {
        return static_cast<int>(std::count_if(frames.begin(), frames.end(), [&](const FrameVerdict& f) {
            return f.lane == lane && f.cursor >= c0 && f.cursor + presence::kFrame <= c1 && !(skipEdge && f.edge) &&
                   pred(f);
        }));
    }

    int total(int lane, std::int64_t c0 = -kFar, std::int64_t c1 = kFar, bool skipEdge = true) const
    {
        return countIf(lane, c0, c1, skipEdge, [](const FrameVerdict&) { return true; });
    }
    int count(int lane, presence::Verdict v, std::int64_t c0 = -kFar, std::int64_t c1 = kFar,
              bool skipEdge = true) const
    {
        return countIf(lane, c0, c1, skipEdge, [v](const FrameVerdict& f) { return f.verdict == v; });
    }
    int countSource(int lane, presence::Verdict v, presence::Source s, std::int64_t c0 = -kFar, std::int64_t c1 = kFar,
                    bool skipEdge = true) const
    {
        return countIf(lane, c0, c1, skipEdge,
                       [v, s](const FrameVerdict& f) { return f.verdict == v && f.source == s; });
    }

    // 逐帧判定日志(确定性对拍用):只含总线时间线位置与判定,不含游标与墙钟。
    std::string log() const
    {
        std::ostringstream os;
        for (const FrameVerdict& f : frames)
        {
            os << 's' << f.span << " t" << f.t << " l" << f.lane << ' ' << presence::toString(f.verdict) << '/'
               << presence::toString(f.source) << (f.edge ? " edge" : "") << '\n';
        }
        return os.str();
    }
};

// ===========================================================================
// SchedRig
// ===========================================================================

class SchedRig
{
public:
    explicit SchedRig(SchedCfg config)
        : cfg_(std::move(config)), map_(cfg_.startT, cfg_.startPlaying), outHead_(map_, cfg_.sr)
    {
        REQUIRE(!cfg_.lanes.empty());
        REQUIRE(static_cast<int>(cfg_.lanes.size()) <= std::min(kSchedMaxLanes, presence::kMaxLanes));
        // 组号 1..7;8 是保留组。越界会被 setGroupId 静默回落 g1,所以先自己挡一道,再读回断言。
        REQUIRE(cfg_.group >= 1);
        REQUIRE(cfg_.group < kSchedReservedGroup);
        REQUIRE(cfg_.outBlock > 0);

        out_.setGroupId(cfg_.group);
        REQUIRE(out_.groupId() == cfg_.group); // [SL-324] 读回断言
        out_.setPlayHead(&outHead_);
        out_.setNonRealtime(cfg_.offline);
        out_.prepareToPlay(cfg_.sr, cfg_.outPrepareBlock > 0 ? cfg_.outPrepareBlock : cfg_.outBlock);
        outBuf_.setSize(2, std::max(cfg_.outBlock, 1));

        for (std::size_t i = 0; i < cfg_.lanes.size(); ++i)
        {
            auto lane = std::make_unique<Lane>(map_, cfg_.sr);
            lane->cfg = cfg_.lanes[i];
            lane->live = lane->cfg.live;
            lane->stereo = lane->cfg.stereo;
            lane->proc = std::make_unique<ScvbInputAudioProcessor>();
            applyLayout(*lane->proc, lane->stereo);
            lane->proc->setGroupId(cfg_.group);
            lane->proc->setChannelId(static_cast<int>(i) + 1);
            lane->proc->setPlayHead(&lane->head);
            lane->proc->setNonRealtime(cfg_.offline);
            lane->proc->prepareToPlay(cfg_.sr, lanePrepareBlock(*lane));
            lanes_.push_back(std::move(lane));
        }
        // [SL-324] Input 侧读回:bridgeTickSnapshot 会懒打开 ctrl 段,所以放在全部 prepare 之后统一读
        //(与 MonoMultiRig 同口径)。
        for (auto& lane : lanes_)
        {
            REQUIRE(lane->proc->bridgeTickSnapshot().groupId == cfg_.group);
        }
        resetPace();
    }

    ~SchedRig()
    {
        out_.releaseResources();
        for (auto& lane : lanes_)
        {
            lane->proc->releaseResources();
        }
    }

    SchedRig(const SchedRig&) = delete;
    SchedRig& operator=(const SchedRig&) = delete;

    // ---- 访问 ----------------------------------------------------------------

    ScvbOutputAudioProcessor& output() { return out_; }
    ScvbInputAudioProcessor& input(int lane) { return *laneAt(lane).proc; }
    int laneCount() const { return static_cast<int>(lanes_.size()); }
    int channelOf(int lane) const { return lane + 1; }
    std::int64_t outCursor() const { return outHead_.cursor(); }
    std::int64_t laneCursor(int lane) const { return laneAt(lane).head.cursor(); }
    bool laneLive(int lane) const { return laneAt(lane).live; }
    std::int64_t cycles() const { return cycleIndex_; }
    double sr() const { return cfg_.sr; }
    const SchedCfg& cfg() const { return cfg_; }
    const TransportMap& map() const { return map_; }
    const InvariantStats& invariant() const { return inv_; }
    const std::vector<OutBlock>& outBlocks() const { return outBlocks_; }
    const std::vector<LaneBlock>& laneLog(int lane) const { return laneAt(lane).log; }
    const std::vector<MBeat>& beats() const { return beats_; }
    std::int64_t busUnderflowSamples() const { return busUnderflow_; }
    std::int64_t recordBase() const { return recBase_; }
    const std::vector<float>& recOutL() const { return recOutL_; }
    const std::vector<float>& recOutR() const { return recOutR_; }
    const std::vector<float>& recBusL() const { return recBusL_; }
    const std::vector<float>& recBusR() const { return recBusR_; }
    const std::vector<Range>& outputSuspendLog() const { return outputSuspendLog_; }
    const std::vector<Range>& chainSuspendLog() const { return chainSuspendLog_; }

    // 已处理区间(按报给插件的位置连续、原因相同合并)。
    std::vector<LaneInterval> processedIntervals(int lane) const
    {
        std::vector<LaneInterval> out;
        for (const LaneBlock& b : laneAt(lane).log)
        {
            if (!out.empty() && out.back().cursor1 == b.cursor && out.back().reason == b.reason &&
                out.back().t1 == b.tReported)
            {
                out.back().cursor1 += b.n;
                out.back().t1 += b.n;
                continue;
            }
            out.push_back(LaneInterval{b.cursor, b.cursor + b.n, b.tReported, b.tReported + b.n, b.reason});
        }
        return out;
    }

    // ---- 节拍 ----------------------------------------------------------------

    void setPace(double pace)
    {
        cfg_.pace = pace;
        resetPace();
    }
    void setUnpacedPumpMs(int ms) { cfg_.unpacedPumpMs = ms; }
    // false = 完全不泵消息([M] 冻结)。冷启动阶段(Input 直通、Output 未注入)用。
    void setPumping(bool on)
    {
        pumping_ = on;
        resetPace();
    }
    void setFailOnUndeclared(bool on) { cfg_.failOnUndeclared = on; }

    // 不处理任何音频,只跑消息循环 ms 毫秒(宿主整体没在给块)。
    void idle(int ms)
    {
        pump(ms);
        resetPace();
    }

    // ---- 运行 ----------------------------------------------------------------

    void runCycles(int n)
    {
        resetPace();
        for (int i = 0; i < n; ++i)
        {
            stepCycle();
        }
    }

    void runFor(std::int64_t samples) { runUntil(outCursor() + samples); }

    void runUntil(std::int64_t outCursorTarget)
    {
        resetPace();
        while (outCursor() < outCursorTarget)
        {
            stepCycle();
        }
    }

    // 等交接走完:所有源信号在放的 lane 都在 Output 那边读得到(trackPeak > 0),且送总线的 lane 都已
    // 自静音(本块总线输入里该轨为零),连续 stableCycles 个周期。只等不断言 —— 等不到返回 false,
    // 由调用方 REQUIRE。
    bool settle(int maxWallMs = 8000, int stableCycles = 8)
    {
        setPumping(true);
        const double start = wallMs();
        int stable = 0;
        while (wallMs() - start < static_cast<double>(maxWallMs))
        {
            stepCycle();
            stable = lastCycleSettled_ ? stable + 1 : 0;
            if (stable >= stableCycles)
            {
                return true;
            }
        }
        return false;
    }

    // ---- 事件 ----------------------------------------------------------------

    // 在 Output 游标到达 at 的那个周期开头执行 fn(at == kNow:立刻)。
    void at(std::int64_t atCursor, std::function<void(SchedRig&)> fn)
    {
        if (atCursor == kNow || atCursor <= outCursor())
        {
            fn(*this);
            return;
        }
        const auto it =
            std::upper_bound(ops_.begin(), ops_.end(), atCursor, [](std::int64_t a, const Op& o) { return a < o.at; });
        ops_.insert(it, Op{atCursor, std::move(fn)});
    }

    // 用户在 Output 游标 at 处定位到 S;Output 先放完 staleOutBlocks 个在途旧块再换位置。
    void seek(std::int64_t atCursor, std::int64_t s, int staleOutBlocks = 0)
    {
        at(atCursor, [s, staleOutBlocks](SchedRig& r) {
            TransportEvent ev;
            ev.kind = TransportKind::Seek;
            ev.pos = s;
            r.applyTransport(ev, static_cast<std::int64_t>(staleOutBlocks) * r.cfg_.outBlock);
        });
    }

    void stop(std::int64_t atCursor = kNow)
    {
        at(atCursor, [](SchedRig& r) {
            TransportEvent ev;
            ev.kind = TransportKind::Stop;
            r.applyTransport(ev, 0);
        });
    }

    void start(std::int64_t atCursor = kNow)
    {
        at(atCursor, [](SchedRig& r) {
            TransportEvent ev;
            ev.kind = TransportKind::Start;
            r.applyTransport(ev, 0);
        });
    }

    void startFrom(std::int64_t s, std::int64_t atCursor = kNow)
    {
        at(atCursor, [s](SchedRig& r) {
            TransportEvent ev;
            ev.kind = TransportKind::Start;
            ev.pos = s;
            ev.hasPos = true;
            r.applyTransport(ev, 0);
        });
    }

    // Cycle [b, e);e <= b 等于关 Cycle。
    void cycle(std::int64_t b, std::int64_t e, std::int64_t atCursor = kNow)
    {
        at(atCursor, [b, e](SchedRig& r) {
            TransportEvent ev;
            ev.kind = TransportKind::Cycle;
            ev.pos = b;
            ev.loopEnd = e;
            r.applyTransport(ev, 0);
        });
    }
    void cycleOff(std::int64_t atCursor = kNow) { cycle(0, 0, atCursor); }

    void setLive(int lane, bool on, LiveSwitch mode)
    {
        Lane& l = laneAt(lane);
        l.live = on;
        const std::int64_t target = on ? outCursor() : outCursor() + l.cfg.lead;
        const std::int64_t c = l.head.cursor();
        if ((mode == LiveSwitch::Rewind && c > target) || mode == LiveSwitch::Jump)
        {
            moveLaneCursor(l, target);
        }
    }

    void offline(bool on)
    {
        out_.setNonRealtime(on);
        for (auto& lane : lanes_)
        {
            lane->proc->setNonRealtime(on);
        }
    }

    void bypassInput(int lane, bool on) { laneAt(lane).bypassed = on; }

    enum class Target
    {
        Output,
        Lane,
        AllInputs,
        All
    };
    void quirk(Target target, const Quirk& q, int lane = 0)
    {
        if (target == Target::Output || target == Target::All)
        {
            outHead_.pushQuirk(q);
        }
        if (target == Target::Lane)
        {
            laneAt(lane).head.pushQuirk(q);
        }
        if (target == Target::AllInputs || target == Target::All)
        {
            for (auto& l : lanes_)
            {
                l->head.pushQuirk(q);
            }
        }
    }

    // 宿主改该轨布局:release → setBusesLayout → prepareToPlay(循环中途 mono⇄stereo 重新 prepare)。
    void geometryRewrite(int lane, bool stereo)
    {
        Lane& l = laneAt(lane);
        l.proc->releaseResources();
        applyLayout(*l.proc, stereo);
        l.stereo = stereo;
        l.proc->prepareToPlay(cfg_.sr, lanePrepareBlock(l));
    }

    void violateGraphOrder(const GraphViolation& v)
    {
        violation_ = v;
        violationLeft_ = v.cycles;
        outHead_.setReaderOffset(v.kind == GraphViolationKind::ReaderOffset ? v.offset : 0);
    }

    // ---- 停调策略 ------------------------------------------------------------

    void noRegion(int lane, std::int64_t g0, std::int64_t g1) { laneAt(lane).cfg.noRegion.push_back(Range{g0, g1}); }
    void silentRegion(int lane, std::int64_t g0, std::int64_t g1) { laneAt(lane).cfg.silent.push_back(Range{g0, g1}); }
    void inputSilenceSuspend(bool on) { inputSilenceSuspend_ = on; }
    void outputSilenceTail(std::int64_t tSamples) { outputSilenceTail_ = tSamples; }
    void chainSuspend(std::int64_t tSamples, ChainBy by)
    {
        chainSuspendT_ = tSamples;
        chainBy_ = by;
    }

    // ---- 记录与判定 ----------------------------------------------------------

    // 丢掉此前的 Output / 总线录音(场景很长时省内存);块表与 [M] 拍保留。
    void clearRecording()
    {
        recBase_ = outCursor();
        recOutL_.clear();
        recOutR_.clear();
        recBusL_.clear();
        recBusR_.clear();
        outBlocks_.erase(std::remove_if(outBlocks_.begin(), outBlocks_.end(),
                                        [this](const OutBlock& b) { return b.cursor < recBase_; }),
                         outBlocks_.end());
    }

    // 该轨源信号在时间线 [t0, t1) 上是否全程在放 / 部分在放。
    bool contentActiveOver(int lane, std::int64_t t0, std::int64_t t1, bool* edge = nullptr) const
    {
        const Lane& l = laneAt(lane);
        bool covered = false;
        bool touched = false;
        for (const std::vector<Range>* rs : {&l.cfg.noRegion, &l.cfg.silent})
        {
            for (const Range& r : *rs)
            {
                covered = covered || r.covers(t0, t1);
                touched = touched || r.overlaps(t0, t1);
            }
        }
        if (edge != nullptr)
        {
            *edge = touched && !covered;
        }
        return !covered;
    }

    // 对 Output 录音([c0, c1) 游标窗)做逐帧判定。总线时间线连续的块拼成一段,逐段分析。
    Analysis analyzeOutput(std::int64_t c0 = -kFar, std::int64_t c1 = kFar,
                           const presence::Thresholds& th = presence::Thresholds{}) const
    {
        return analyze(recOutL_, recOutR_, c0, c1, th);
    }

    // 同样的判定做在 Output 的**输入**(宿主总线)上 —— 宿主侧看到的「这轨在不在」。
    Analysis analyzeBus(std::int64_t c0 = -kFar, std::int64_t c1 = kFar,
                        const presence::Thresholds& th = presence::Thresholds{}) const
    {
        return analyze(recBusL_, recBusR_, c0, c1, th);
    }

    // 录音窗内 L / R 的均方(粗看声像用)。
    std::pair<double, double> outMeanSquare(std::int64_t c0, std::int64_t c1) const
    {
        double l = 0.0;
        double r = 0.0;
        std::int64_t n = 0;
        for (std::int64_t c = std::max(c0, recBase_); c < std::min(c1, recBase_ + recLength()); ++c)
        {
            const std::size_t i = static_cast<std::size_t>(c - recBase_);
            l += static_cast<double>(recOutL_[i]) * recOutL_[i];
            r += static_cast<double>(recOutR_[i]) * recOutR_[i];
            ++n;
        }
        return n > 0 ? std::make_pair(l / static_cast<double>(n), r / static_cast<double>(n))
                     : std::make_pair(0.0, 0.0);
    }

    std::int64_t recLength() const { return static_cast<std::int64_t>(recOutL_.size()); }

private:
    struct LaneFifo
    {
        std::int64_t base = 0;
        std::vector<float> data;

        std::int64_t end() const { return base + static_cast<std::int64_t>(data.size()); }
        void write(std::int64_t c, const float* src, int n)
        {
            if (c < base)
            {
                const std::int64_t skip = base - c;
                if (skip >= n)
                {
                    return;
                }
                src += skip;
                n -= static_cast<int>(skip);
                c = base;
            }
            truncate(c);
            if (c > end())
            {
                data.resize(static_cast<std::size_t>(c - base), 0.0f); // 前跳留下的空档 = 静音
            }
            data.insert(data.end(), src, src + n);
        }
        // 读一个样本;还没产出 ⇒ ok=false(调用方按零计)。
        float read(std::int64_t c, bool& ok) const
        {
            ok = c >= base && c < end();
            return ok ? data[static_cast<std::size_t>(c - base)] : 0.0f;
        }
        void truncate(std::int64_t c)
        {
            if (c < end())
            {
                data.resize(static_cast<std::size_t>(std::max<std::int64_t>(c - base, 0)));
            }
        }
        void dropBefore(std::int64_t c)
        {
            if (c <= base)
            {
                return;
            }
            const std::int64_t k = std::min<std::int64_t>(c - base, static_cast<std::int64_t>(data.size()));
            data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(k));
            base += k;
        }
    };

    struct Lane
    {
        Lane(const TransportMap& map, double sr) : head(map, sr) {}

        LaneCfg cfg;
        LanePlayHead head;
        std::unique_ptr<ScvbInputAudioProcessor> proc;
        bool live = true;
        bool stereo = false;
        bool bypassed = false;
        LaneFifo fifo;
        juce::AudioBuffer<float> buf;
        std::vector<LaneBlock> log;
    };

    struct Op
    {
        std::int64_t at = 0;
        std::function<void(SchedRig&)> fn;
    };

    static void applyLayout(ScvbInputAudioProcessor& p, bool stereo)
    {
        const juce::AudioChannelSet set = stereo ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono();
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add(set);
        layout.outputBuses.add(set);
        REQUIRE(p.setBusesLayout(layout));
    }

    int lanePrepareBlock(const Lane& l) const
    {
        return l.cfg.prepareBlock > 0 ? l.cfg.prepareBlock : std::max(l.cfg.blockIn, cfg_.outBlock);
    }

    Lane& laneAt(int lane)
    {
        REQUIRE(lane >= 0);
        REQUIRE(lane < laneCount());
        return *lanes_[static_cast<std::size_t>(lane)];
    }
    const Lane& laneAt(int lane) const
    {
        REQUIRE(lane >= 0);
        REQUIRE(lane < laneCount());
        return *lanes_[static_cast<std::size_t>(lane)];
    }

    void moveLaneCursor(Lane& l, std::int64_t target)
    {
        if (target < l.head.cursor())
        {
            l.fifo.truncate(target); // 已预取、还没进总线的音频作废
        }
        l.head.setCursor(target);
    }

    // 用户操作:在当前 Output 游标(加 delay)处生效;超过生效点的 lane 冲刷回生效点重新渲染。
    void applyTransport(TransportEvent ev, std::int64_t delay)
    {
        ev.at = outCursor() + delay;
        map_.add(ev);
        for (auto& lane : lanes_)
        {
            if (lane->head.cursor() > ev.at)
            {
                moveLaneCursor(*lane, ev.at);
            }
        }
    }

    void applyDueOps()
    {
        while (!ops_.empty() && ops_.front().at <= outCursor())
        {
            Op op = std::move(ops_.front());
            ops_.erase(ops_.begin());
            op.fn(*this);
        }
    }

    std::vector<std::int64_t> laneCuts(const Lane& l) const
    {
        std::vector<std::int64_t> cuts;
        for (const std::vector<Range>* rs : {&l.cfg.noRegion, &l.cfg.silent})
        {
            for (const Range& r : *rs)
            {
                cuts.push_back(r.begin);
                cuts.push_back(r.end);
            }
        }
        return cuts;
    }

    // 该轨源信号在 map 点 p 处是否在放(块已在所有边沿处切开,看块首即可)。
    static bool contentActiveAt(const Lane& l, const MapPoint& p)
    {
        return p.playing && !anyContains(l.cfg.noRegion, p.t) && !anyContains(l.cfg.silent, p.t);
    }

    void processLaneBlock(Lane& l, int laneIdx, int n)
    {
        BlockPos bp = l.head.plan(n);
        const bool active = contentActiveAt(l, bp.bus);
        const int nch = l.stereo ? 2 : 1;
        l.buf.setSize(nch, n, false, false, true);
        if (active)
        {
            const presence::Tone tone = presence::laneTone(laneIdx, l.cfg.amp);
            for (int ch = 0; ch < nch; ++ch)
            {
                float* d = l.buf.getWritePointer(ch);
                for (int i = 0; i < n; ++i)
                {
                    d[i] = static_cast<float>(presence::sampleAt(tone, bp.bus.t + i));
                }
            }
        }
        else
        {
            l.buf.clear();
        }

        CallReason reason = CallReason::Called;
        if (l.bypassed)
        {
            reason = CallReason::Bypassed; // 宿主直通源信号
        }
        else if (anyContains(l.cfg.noRegion, bp.bus.t))
        {
            reason = CallReason::NoRegion;
            l.buf.clear();
        }
        else if (chainSuspended_)
        {
            reason = CallReason::ChainSuspended; // 宿主直通源信号
        }
        else if (inputSilenceSuspend_ && !active)
        {
            reason = CallReason::SilenceSuspended; // 源信号本来就是零
        }

        if (reason == CallReason::Called)
        {
            l.head.arm(bp);
            l.proc->processBlock(l.buf, midi_);
        }
        l.fifo.write(bp.cursor, l.buf.getReadPointer(0), n);
        l.log.push_back(
            LaneBlock{bp.cursor, n, bp.bus.t, bp.tReported, bp.bus.playing, l.live, active, bp.quirked, reason});
        l.head.advance(n);
    }

    // 把 lane 推进到游标 >= target(不截在 target 上:预取块整块处理,可超出)。
    void advanceLane(int laneIdx, std::int64_t target)
    {
        Lane& l = *lanes_[static_cast<std::size_t>(laneIdx)];
        const int limit = l.live ? cfg_.outBlock : l.cfg.blockIn;
        const std::vector<std::int64_t> cuts = laneCuts(l);
        while (l.head.cursor() < target)
        {
            processLaneBlock(l, laneIdx, l.head.maxBlock(limit, cuts));
        }
    }

    void checkInvariant(int nOut, bool declaredCycle, OutBlock& ob)
    {
        ++inv_.checked;
        const std::int64_t required = outCursor() + outHead_.readerOffset() + nOut;
        bool any = false;
        for (std::size_t i = 0; i < lanes_.size(); ++i)
        {
            const Lane& l = *lanes_[i];
            if (!l.cfg.toBus || l.head.cursor() >= required)
            {
                continue;
            }
            any = true;
            inv_.hits.push_back(
                InvariantHit{cycleIndex_, outCursor(), static_cast<int>(i), l.head.cursor(), required, declaredCycle});
        }
        if (!any)
        {
            return;
        }
        ob.violation = true;
        ob.declared = declaredCycle;
        if (declaredCycle)
        {
            ++inv_.declared;
            return;
        }
        ++inv_.undeclared;
        if (cfg_.failOnUndeclared)
        {
            FAIL_CHECK("SchedRig: graph-order invariant broken without annotation at cycle "
                       << cycleIndex_ << " (outCursor " << outCursor() << ", required lane cursor " << required << ")");
        }
    }

    void processOutput(int nOut, OutBlock& ob)
    {
        BlockPos bp = outHead_.plan(nOut);
        outBuf_.setSize(2, nOut, false, false, true);
        outBuf_.clear();
        float* l = outBuf_.getWritePointer(0);
        float* r = outBuf_.getWritePointer(1);
        bool busSilent = true;
        for (std::size_t li = 0; li < lanes_.size(); ++li)
        {
            Lane& lane = *lanes_[li];
            lastBusNonZero_[li] = false;
            if (!lane.cfg.toBus)
            {
                continue;
            }
            for (int i = 0; i < nOut; ++i)
            {
                bool ok = false;
                const float v = lane.fifo.read(bp.cursor + i, ok);
                if (!ok)
                {
                    ++busUnderflow_;
                    continue;
                }
                l[i] += static_cast<float>(presence::kRawBusGainL) * v;
                r[i] += static_cast<float>(presence::kRawBusGainR) * v;
                if (v != 0.0f)
                {
                    lastBusNonZero_[li] = true;
                    busSilent = false;
                }
            }
        }
        recBusL_.insert(recBusL_.end(), l, l + nOut);
        recBusR_.insert(recBusR_.end(), r, r + nOut);

        busSilentRun_ = busSilent ? busSilentRun_ + nOut : 0;
        CallReason reason = CallReason::Called;
        if (chainSuspended_)
        {
            reason = CallReason::ChainSuspended;
        }
        else if (outputSilenceTail_ >= 0 && busSilentRun_ > outputSilenceTail_)
        {
            reason = CallReason::OutputSilenceTail;
        }
        noteEpisode(outputSuspendLog_, reason == CallReason::OutputSilenceTail, bp.cursor, nOut);

        if (reason == CallReason::Called)
        {
            outHead_.arm(bp);
            out_.processBlock(outBuf_, midi_);
        }
        recOutL_.insert(recOutL_.end(), outBuf_.getReadPointer(0), outBuf_.getReadPointer(0) + nOut);
        recOutR_.insert(recOutR_.end(), outBuf_.getReadPointer(1), outBuf_.getReadPointer(1) + nOut);

        ob.cursor = bp.cursor;
        ob.n = nOut;
        ob.tBus = bp.bus.t;
        ob.playingBus = bp.bus.playing;
        ob.tReported = bp.tReported;
        ob.playingReported = bp.playingReported;
        ob.quirked = bp.quirked;
        ob.reason = reason;

        // settle 判据:源信号在放的 lane 都读得到,送总线的 lane 都已自静音。
        const scvb::output::MeterPod pod = out_.meterSnapshot();
        bool settled = reason == CallReason::Called && bp.bus.playing;
        for (std::size_t li = 0; li < lanes_.size() && settled; ++li)
        {
            const Lane& lane = *lanes_[li];
            if (!contentActiveAt(lane, bp.bus))
            {
                continue;
            }
            settled = pod.trackPeak[li] > 0.0f && !(lane.cfg.toBus && lastBusNonZero_[li]);
        }
        lastCycleSettled_ = settled;
    }

    static void noteEpisode(std::vector<Range>& log, bool on, std::int64_t cursor, int n)
    {
        if (!on)
        {
            return;
        }
        if (!log.empty() && log.back().end == cursor)
        {
            log.back().end += n;
            return;
        }
        log.push_back(Range{cursor, cursor + n});
    }

    void updateChainSuspension(int nOut)
    {
        if (chainSuspendT_ < 0)
        {
            chainSuspended_ = false;
            return;
        }
        const MapPoint p = map_.at(outCursor());
        const bool allSilent = std::none_of(lanes_.begin(), lanes_.end(),
                                            [&p](const std::unique_ptr<Lane>& l) { return contentActiveAt(*l, p); });
        inSilentRun_ = allSilent ? inSilentRun_ + nOut : 0;
        const bool byIn = inSilentRun_ > chainSuspendT_;
        const bool byOut = busSilentRun_ > chainSuspendT_;
        chainSuspended_ = chainBy_ == ChainBy::In ? byIn : (chainBy_ == ChainBy::Out ? byOut : (byIn && byOut));
        noteEpisode(chainSuspendLog_, chainSuspended_, outCursor(), nOut);
    }

    // 一个 Output 周期(HostScheduler)。
    void stepCycle()
    {
        applyDueOps();
        const bool violating = violation_.kind != GraphViolationKind::None && violationLeft_ != 0;
        const bool outFirst = violating && violation_.kind == GraphViolationKind::OutputFirst;
        const bool declaredCycle = violating && violation_.annotate;

        const int nOut = outHead_.maxBlock(cfg_.outBlock);
        updateChainSuspension(nOut);

        // ① 预取轨突发处理到 c_i >= C_O + L_i + b_out。
        for (std::size_t i = 0; i < lanes_.size(); ++i)
        {
            if (!lanes_[i]->live)
            {
                advanceLane(static_cast<int>(i), outCursor() + lanes_[i]->cfg.lead + cfg_.outBlock);
            }
        }
        // ② live 轨同周期先处理 Input(outputFirst 违反时挪到 Output 之后)。
        if (!outFirst)
        {
            advanceLiveLanes(outCursor() + nOut);
        }
        // ③ 图依赖不变式。
        OutBlock ob;
        checkInvariant(nOut, declaredCycle, ob);
        // ④ Output。
        processOutput(nOut, ob);
        if (outFirst)
        {
            advanceLiveLanes(outCursor() + nOut);
        }
        outBlocks_.push_back(ob);
        outHead_.advance(nOut);
        ++cycleIndex_;

        if (violating && violationLeft_ > 0 && --violationLeft_ == 0)
        {
            violateGraphOrder(GraphViolation{});
        }
        for (auto& lane : lanes_)
        {
            lane->fifo.dropBefore(outCursor());
        }
        paceAfterCycle();
    }

    void advanceLiveLanes(std::int64_t target)
    {
        for (std::size_t i = 0; i < lanes_.size(); ++i)
        {
            if (lanes_[i]->live)
            {
                advanceLane(static_cast<int>(i), target);
            }
        }
    }

    void resetPace()
    {
        paceWall0_ = wallMs();
        paceCursor0_ = outCursor();
    }

    void pump(int ms)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
        lastPumpWall_ = wallMs();
        sampleBeat();
    }

    void paceAfterCycle()
    {
        if (!pumping_)
        {
            return;
        }
        if (cfg_.pace > 0.0)
        {
            const double target =
                paceWall0_ + static_cast<double>(outCursor() - paceCursor0_) * 1000.0 / (cfg_.sr * cfg_.pace);
            const double now = wallMs();
            const double ahead = target - now;
            if (ahead >= 1.0)
            {
                pump(static_cast<int>(ahead));
            }
            else if (now - lastPumpWall_ >= 10.0)
            {
                pump(0); // 跟不上节拍时也别让 [M] 饿着
            }
            return;
        }
        if (cfg_.unpacedPumpMs > 0)
        {
            pump(cfg_.unpacedPumpMs);
        }
    }

    void sampleBeat()
    {
        const double now = wallMs();
        if (!beats_.empty() && now - beats_.back().wallMs < cfg_.beatMinGapMs)
        {
            return;
        }
        MBeat b;
        b.wallMs = now;
        b.outCursor = outCursor();
        const auto snap = out_.connSnapshot();
        const scvb::output::MeterPod pod = out_.meterSnapshot();
        for (int i = 0; i < laneCount(); ++i)
        {
            const auto& c = snap.channels[static_cast<std::size_t>(i)];
            LaneBeat lb;
            lb.slotState = c.slotState;
            lb.heartbeatAgeMs = c.heartbeatAgeMs;
            lb.suspended = c.suspended;
            lb.misalign = out_.misalignCount(channelOf(i));
            lb.gap = out_.gapCount(channelOf(i));
            lb.trackPeak = pod.trackPeak[static_cast<std::size_t>(i)];
            b.lanes.push_back(lb);
        }
        beats_.push_back(std::move(b));
    }

    Analysis analyze(const std::vector<float>& recL, const std::vector<float>& recR, std::int64_t c0, std::int64_t c1,
                     const presence::Thresholds& th) const
    {
        Analysis a;
        std::vector<int> ids;
        for (int i = 0; i < laneCount(); ++i)
        {
            ids.push_back(i);
        }
        const std::int64_t recEnd = recBase_ + recLength();
        std::size_t i = 0;
        while (i < outBlocks_.size())
        {
            // 一段 = 总线时间线连续、在放、游标连续的相邻块。
            const OutBlock& first = outBlocks_[i];
            std::size_t j = i + 1;
            std::int64_t cEnd = first.cursor + first.n;
            std::int64_t tEnd = first.tBus + first.n;
            while (first.playingBus && j < outBlocks_.size() && outBlocks_[j].playingBus &&
                   outBlocks_[j].cursor == cEnd && outBlocks_[j].tBus == tEnd)
            {
                cEnd += outBlocks_[j].n;
                tEnd += outBlocks_[j].n;
                ++j;
            }
            const std::int64_t s = std::max({first.cursor, c0, recBase_});
            const std::int64_t e = std::min({cEnd, c1, recEnd});
            if (first.playingBus && e - s >= presence::kFrame)
            {
                const std::int64_t tStart = first.tBus + (s - first.cursor);
                const std::size_t off = static_cast<std::size_t>(s - recBase_);
                const presence::Span span{recL.data() + off, recR.data() + off, tStart, e - s};
                const presence::ExpectFn expect = [this](int lane, std::int64_t fs) {
                    return contentActiveOver(lane, fs, fs + presence::kFrame);
                };
                const presence::CombReport rep = presence::analyze(span, ids, expect, th);
                a.dirtyFrames += rep.dirtyFrames(th);
                a.controlNonAbsent += rep.controlNonAbsent(th);
                for (const presence::LaneReport& lr : rep.lanes)
                {
                    for (std::size_t k = 0; k < rep.frameStarts.size(); ++k)
                    {
                        FrameVerdict f;
                        f.span = a.spans;
                        f.t = rep.frameStarts[k];
                        f.cursor = s + (f.t - tStart);
                        f.lane = lr.lane;
                        f.verdict = lr.judged[k].verdict;
                        f.why = lr.judged[k].why;
                        f.source = lr.sources[k];
                        f.expectActive = lr.frames[k].expectActive;
                        bool edge = false;
                        contentActiveOver(lr.lane, f.t, f.t + presence::kFrame, &edge);
                        f.edge = edge;
                        a.frames.push_back(f);
                    }
                }
                ++a.spans;
            }
            i = j;
        }
        return a;
    }

    // 成员顺序即构造顺序:JUCE 初始化最先,map 先于各 playhead。
    juce::ScopedJuceInitialiser_GUI juceInit_;
    SchedCfg cfg_;
    TransportMap map_;
    LanePlayHead outHead_;
    ScvbOutputAudioProcessor out_;
    std::vector<std::unique_ptr<Lane>> lanes_;
    juce::AudioBuffer<float> outBuf_;
    juce::MidiBuffer midi_;

    std::vector<Op> ops_;
    GraphViolation violation_;
    int violationLeft_ = 0;
    InvariantStats inv_;
    std::int64_t cycleIndex_ = 0;

    bool inputSilenceSuspend_ = false;
    std::int64_t outputSilenceTail_ = -1;
    std::int64_t chainSuspendT_ = -1;
    ChainBy chainBy_ = ChainBy::Both;
    bool chainSuspended_ = false;
    std::int64_t busSilentRun_ = 0;
    std::int64_t inSilentRun_ = 0;
    std::vector<Range> outputSuspendLog_;
    std::vector<Range> chainSuspendLog_;

    bool pumping_ = true;
    double paceWall0_ = 0.0;
    std::int64_t paceCursor0_ = 0;
    double lastPumpWall_ = 0.0;

    std::array<bool, kSchedMaxLanes> lastBusNonZero_{};
    bool lastCycleSettled_ = false;
    std::int64_t busUnderflow_ = 0;

    std::int64_t recBase_ = 0;
    std::vector<float> recOutL_;
    std::vector<float> recOutR_;
    std::vector<float> recBusL_;
    std::vector<float> recBusR_;
    std::vector<OutBlock> outBlocks_;
    std::vector<MBeat> beats_;
};

} // namespace scvb::testsupport::sched
