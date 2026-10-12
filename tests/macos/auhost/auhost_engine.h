// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// auhost_engine —— scvb_auhost 的调度宿主模型(B 线 M13)。只用标准库 + tests/support/presence_meter.h,
// 不碰 AudioToolbox,所以本头连同 auhost_judge.h 的自测能在任何平台上编译运行(本地用 g++ 自测,
// CI 上由 scvb_auhost --self-test 跑同一份)。
//
// ## 一台「宿主」
//
// 与 A 线 tests/host/sched/SchedRig.h 同一个调度形状,只是插件换成真实的二进制 component
// (由 scvb_auhost.cpp 经回调接进来):
//   · 每条 Input lane 一个连续渲染游标 c_i;游标 = 「该样本送到总线的时刻」(从 0 起的样本数)。
//     时间线位置 = map(c_i),由场景的走带决定(LS-1 线性;LS-4 Cycle 回绕)。
//   · 每个 Output 周期(b_out 个样本):
//       ① 预取轨突发处理到 c_i ≥ C_O + L_i + b_out(块长 b_in,默认 1024);
//       ② live 轨处理 [C_O, C_O + b_out);
//       ③ 总线 = 各 lane 输出按游标求和,**只进 L、增益 0.5**(presence::kRawBusGainL/R),
//          SCVB 混音是居中的 ⇒ 判据按 L/R 能量比分得开「未平衡原声」与「SCVB 混音」;
//       ④ 处理 Output [C_O, C_O + b_out),录下它的 L/R 与总线时间线位置。
//   · 块在时间线不连续处(循环点)切开 —— 一块之内时间线恒连续,这是 Input 写环与 Output 读环都默认的前提。
//   · 预取轨比 Output 早约 L_i 跨过循环点 ⇒ 写方先换代 —— 这正是 H1 的形状(LS-4)。
//
// ## 场景(精简版,M13)
//
//   LS-1  稳态回放(无 Cycle、无定位):lane 0 live,lane 1 / 2 预取提前 4096 / 16384。
//         判据:预热后逐轨 SCVB 混音在场 ≥ 99%,Wrong = 0。
//   LS-4  Cycle 2 s + 混合提前量(H1 现场版):lane 0 live,lane 1 / 2 预取提前 4096 / 48000。
//         判据:预热后连续 4 圈,每圈每轨丢失 ≤ 1 帧(A 线 A-4 的口径),Wrong = 0。
//   取值是 A 线 LS-1(L ∈ {0,1024,4096,16384})与 LS-4(Δ ∈ {1024,4096,48000})格子里的取样,
//   圈长取 2 s(A 线三档里最短、每圈换代最多的一档)。LS-5、离线导出、「修复前 artifact 留证」按精简裁定不做。
//
// ## 预热(settle)
//
// 真实插件起来之后要先走完交接:Input 认领通道 → Output 看见这一路(connected_mask)并注入 →
// Input 收到回执后自静音(J32)。drive() 每周期看两件事,连续 kSettleStableCycles 个周期都成立才算预热完:
//   · 每条被调用的 lane 在本周期的总线区间里输出全零(已自静音),而它的源信号不是零;
//   · 宿主侧外部判据(scvb_auhost.cpp:registry 段里 Output 活跃、各 lane 的 InputSlot 活跃且
//     connected_mask 有这一位)。
// 预热完再放 kSettleMarginSamples(0.1 s,与 SchedRig 的 runScenario 同)才进判定窗。
//
// ## 不覆盖的(免得被当成全称)
//
//   · 宿主多线程:本台在一条渲染线程上串行调用各实例,不模拟宿主把不同轨放到不同线程并发渲染。
//   · 停调策略、定位、起播怪癖、live⇄预取切换都不在精简版里(那些在 A 线 SchedRig 的处理器级用例里)。
//   · lane 只用 mono;总线 R 恒为 0。
//
// 运行期文案一律 ASCII;中文只在注释里。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "support/presence_meter.h"

namespace scvb::auhost
{

namespace presence = scvb::testsupport::presence;

inline constexpr double kSr = 48000.0;
inline constexpr int kLanes = 3;
inline constexpr double kBpm = 120.0;
inline constexpr std::int64_t kFar = std::numeric_limits<std::int64_t>::max() / 4;
// 预热判据要连续成立的周期数(与 SchedRig::settle 的 stableCycles 默认值相同)。
inline constexpr int kSettleStableCycles = 8;
// 预热完再放 0.1 s 才进判定窗:交接期的 80 ms 总线交叉不混进判定(SchedRig 的 runScenario 同口径)。
inline constexpr std::int64_t kSettleMarginSamples = 4800;
// lane 输出「全零」的门限:J32 自静音后 Input 输出数字静音,这里只留浮点噪声的余量。
inline constexpr float kMutedAbs = 1.0e-7f;

inline std::int64_t secToSamples(double s)
{
    return static_cast<std::int64_t>(std::llround(s * kSr));
}

inline double samplesToSec(std::int64_t n)
{
    return static_cast<double>(n) / kSr;
}

// ---------------------------------------------------------------------------
// 场景
// ---------------------------------------------------------------------------

struct LaneSpec
{
    bool live = true;
    std::int64_t lead = 0; // 预取轨的提前量(样本);live 轨恒 0
    int blockIn = 1024; // 预取块长
};

enum class Scenario
{
    Ls1,
    Ls4
};

struct ScenarioSpec
{
    Scenario id = Scenario::Ls1;
    std::string name = "ls1";
    int outBlock = 512;
    std::array<LaneSpec, kLanes> lanes{};
    std::int64_t startT = 0; // 游标 0 处的时间线位置
    bool cycle = false;
    std::int64_t loopLen = 0; // Cycle = [startT, startT + loopLen)
    std::int64_t measureSamples = 0; // LS-1:判定窗长度
    int laps = 0; // LS-4:判定的圈数
    std::int64_t settleLimit = 0; // 预热最晚到 Output 游标这里;超过 = 预热失败
    // 判据
    double minPresentFraction = 0.99; // LS-1
    int maxLostPerLap = 1; // LS-4

    int maxBlock() const
    {
        int m = outBlock;
        for (const LaneSpec& l : lanes)
        {
            m = std::max(m, l.blockIn);
        }
        return m;
    }
    std::int64_t maxLead() const
    {
        std::int64_t m = 0;
        for (const LaneSpec& l : lanes)
        {
            m = std::max(m, l.live ? std::int64_t{0} : l.lead);
        }
        return m;
    }
    // 判定窗最晚结束的 Output 游标(预热用满上限时)。
    std::int64_t outLimit() const
    {
        const std::int64_t m0 = settleLimit + kSettleMarginSamples;
        if (id == Scenario::Ls1)
        {
            return m0 + measureSamples + outBlock;
        }
        return (m0 / loopLen + 2 + laps) * loopLen + outBlock;
    }
};

inline ScenarioSpec makeLs1()
{
    ScenarioSpec s;
    s.id = Scenario::Ls1;
    s.name = "ls1";
    s.outBlock = 512;
    s.lanes[0] = LaneSpec{true, 0, 1024};
    s.lanes[1] = LaneSpec{false, 4096, 1024};
    s.lanes[2] = LaneSpec{false, 16384, 1024};
    s.startT = 93 * presence::kFrame; // ≈ 1.98 s,帧对齐(A 线 LS-1 的 alignedSec(2.0))
    s.cycle = false;
    s.measureSamples = secToSamples(5.0);
    s.settleLimit = secToSamples(12.0);
    return s;
}

inline ScenarioSpec makeLs4()
{
    ScenarioSpec s;
    s.id = Scenario::Ls4;
    s.name = "ls4";
    s.outBlock = 512;
    s.lanes[0] = LaneSpec{true, 0, 1024};
    s.lanes[1] = LaneSpec{false, 4096, 1024};
    s.lanes[2] = LaneSpec{false, 48000, 1024};
    s.startT = 141 * presence::kFrame; // ≈ 3.008 s,帧对齐(A 线 kLs4LoopB)
    s.cycle = true;
    s.loopLen = secToSamples(2.0); // 96000:每圈 93 个完整分析帧 + 尾部 768 样本(不分析)
    s.laps = 4;
    s.settleLimit = secToSamples(12.0);
    return s;
}

// ---------------------------------------------------------------------------
// 走带:游标 → 时间线(纯函数)
// ---------------------------------------------------------------------------

struct TimePoint
{
    std::int64_t t = 0;
    bool cycling = false;
    std::int64_t loopB = 0;
    std::int64_t loopE = 0;
};

inline TimePoint timelineAt(const ScenarioSpec& s, std::int64_t c)
{
    if (!s.cycle)
    {
        return TimePoint{s.startT + c, false, 0, 0};
    }
    return TimePoint{s.startT + c % s.loopLen, true, s.startT, s.startT + s.loopLen};
}

// 从游标 c 起,时间线还能连续走多少个样本(≥ 1)。
inline std::int64_t runLength(const ScenarioSpec& s, std::int64_t c)
{
    if (!s.cycle)
    {
        return kFar;
    }
    return s.loopLen - c % s.loopLen;
}

inline double samplesToBeats(double samples)
{
    return samples / kSr * (kBpm / 60.0);
}

// ---------------------------------------------------------------------------
// 引擎
// ---------------------------------------------------------------------------

struct Callbacks
{
    // Input lane:mono 进、mono 出。返回 false = 宿主侧渲染失败(整个场景按「没测成」处理)。
    std::function<bool(int lane, std::int64_t cursor, const TimePoint& tp, int n, const float* in, float* out)> lane;
    // Output:stereo 进、stereo 出。
    std::function<bool(std::int64_t cursor, const TimePoint& tp, int n, const float* inL, const float* inR, float* outL,
                       float* outR)>
        output;
    // 每个 Output 周期末尾调用一次(scvb_auhost 在这里渲染 Monitor)。可空。
    std::function<bool(std::int64_t cursor, int n)> afterOutput;
};

// lane 的一次时间线回跳(预取轨先于 Output 跨过循环点):laneCursor = 回跳块的游标,
// outCursor = 渲染它的那个周期的 Output 游标。laneCursor − outCursor = 实际提前量。
struct LaneWrap
{
    std::int64_t laneCursor = 0;
    std::int64_t outCursor = 0;
};

class Engine
{
public:
    Engine(ScenarioSpec spec, Callbacks cb) : s_(std::move(spec)), cb_(std::move(cb))
    {
        capacity_ = s_.outLimit() + s_.maxLead() + s_.outBlock + s_.maxBlock() + 1;
        outL.assign(static_cast<std::size_t>(capacity_), 0.0f);
        outR.assign(static_cast<std::size_t>(capacity_), 0.0f);
        outT.assign(static_cast<std::size_t>(capacity_), 0);
        busL.assign(static_cast<std::size_t>(capacity_), 0.0f);
        for (int l = 0; l < kLanes; ++l)
        {
            laneOut[static_cast<std::size_t>(l)].assign(static_cast<std::size_t>(capacity_), 0.0f);
        }
        const std::size_t mb = static_cast<std::size_t>(s_.maxBlock());
        inBuf_.assign(mb, 0.0f);
        zeros_.assign(mb, 0.0f);
    }

    const ScenarioSpec& spec() const { return s_; }
    std::int64_t outCursor() const { return outC_; }
    std::int64_t laneCursor(int lane) const { return laneC_[static_cast<std::size_t>(lane)]; }
    std::int64_t capacity() const { return capacity_; }
    const std::string& error() const { return error_; }

    // 宿主根本不调用这条 lane(负对照「停掉一路 Input」):总线上这一路是静音(宿主不调 = 无 region)。
    void setSkipped(int lane, bool on) { skipped_[static_cast<std::size_t>(lane)] = on; }
    bool skipped(int lane) const { return skipped_[static_cast<std::size_t>(lane)]; }

    // 上一个周期里这条 lane 在总线区间 [C_O, C_O + b_out) 上输出全零(源信号非零)。
    bool laneMutedLastCycle(int lane) const { return mutedLast_[static_cast<std::size_t>(lane)]; }

    // 一个 Output 周期。false = 某次渲染失败或容量用完(error() 说明)。
    bool stepCycle()
    {
        const std::int64_t c0 = outC_;
        const int b = s_.outBlock;
        if (c0 + b + s_.maxLead() + s_.maxBlock() >= capacity_)
        {
            error_ = "engine capacity exhausted";
            return false;
        }
        // ① 预取轨
        for (int l = 0; l < kLanes; ++l)
        {
            const LaneSpec& ls = s_.lanes[static_cast<std::size_t>(l)];
            if (ls.live)
            {
                continue;
            }
            std::int64_t& c = laneC_[static_cast<std::size_t>(l)];
            while (c < c0 + ls.lead + b)
            {
                const int n = static_cast<int>(std::min<std::int64_t>(ls.blockIn, runLength(s_, c)));
                if (!renderLane(l, c, n))
                {
                    return false;
                }
                c += n;
            }
        }
        // ② live 轨
        for (int l = 0; l < kLanes; ++l)
        {
            if (!s_.lanes[static_cast<std::size_t>(l)].live)
            {
                continue;
            }
            std::int64_t& c = laneC_[static_cast<std::size_t>(l)];
            while (c < c0 + b)
            {
                const int n = static_cast<int>(std::min<std::int64_t>(c0 + b - c, runLength(s_, c)));
                if (!renderLane(l, c, n))
                {
                    return false;
                }
                c += n;
            }
        }
        // 图依赖不变式:送往总线的每条 lane 都已处理到 Output 本块要读的位置。
        for (int l = 0; l < kLanes; ++l)
        {
            if (laneC_[static_cast<std::size_t>(l)] < c0 + b)
            {
                error_ = "graph order violated";
                return false;
            }
        }
        // ③ 总线 + 本周期的自静音观测
        for (int l = 0; l < kLanes; ++l)
        {
            bool muted = !skipped_[static_cast<std::size_t>(l)];
            const std::vector<float>& lo = laneOut[static_cast<std::size_t>(l)];
            for (std::int64_t c = c0; c < c0 + b && muted; ++c)
            {
                muted = std::fabs(lo[static_cast<std::size_t>(c)]) <= kMutedAbs;
            }
            mutedLast_[static_cast<std::size_t>(l)] = muted;
        }
        for (std::int64_t c = c0; c < c0 + b; ++c)
        {
            float sum = 0.0f;
            for (int l = 0; l < kLanes; ++l)
            {
                sum += laneOut[static_cast<std::size_t>(l)][static_cast<std::size_t>(c)];
            }
            busL[static_cast<std::size_t>(c)] = static_cast<float>(presence::kRawBusGainL) * sum;
        }
        // ④ Output(在时间线不连续处切开)
        std::int64_t c = c0;
        while (c < c0 + b)
        {
            const int n = static_cast<int>(std::min<std::int64_t>(c0 + b - c, runLength(s_, c)));
            const TimePoint tp = timelineAt(s_, c);
            if (haveOutT_ && tp.t != lastOutEndT_)
            {
                outWraps.push_back(c);
            }
            const std::size_t i = static_cast<std::size_t>(c);
            if (!cb_.output(c, tp, n, &busL[i], zeros_.data(), &outL[i], &outR[i]))
            {
                error_ = "output render failed";
                return false;
            }
            for (int k = 0; k < n; ++k)
            {
                outT[i + static_cast<std::size_t>(k)] = tp.t + k;
            }
            haveOutT_ = true;
            lastOutEndT_ = tp.t + n;
            c += n;
        }
        if (cb_.afterOutput && !cb_.afterOutput(c0, b))
        {
            error_ = "after-output render failed";
            return false;
        }
        outC_ = c0 + b;
        ++cycles_;
        return true;
    }

    std::int64_t cycles() const { return cycles_; }

    // 录音(按 Output 游标下标)。
    std::vector<float> outL;
    std::vector<float> outR;
    std::vector<std::int64_t> outT; // 该样本所在的总线时间线位置
    std::vector<float> busL; // Output 的输入 L(R 恒为 0)
    std::array<std::vector<float>, kLanes> laneOut; // 各 lane 的宿主输出(按游标)
    std::array<std::vector<LaneWrap>, kLanes> laneWraps;
    std::vector<std::int64_t> outWraps; // Output 时间线回跳处的游标

private:
    bool renderLane(int l, std::int64_t c, int n)
    {
        const std::size_t li = static_cast<std::size_t>(l);
        const TimePoint tp = timelineAt(s_, c);
        if (haveLaneT_[li] && tp.t != lastLaneEndT_[li])
        {
            laneWraps[li].push_back(LaneWrap{c, outC_});
        }
        haveLaneT_[li] = true;
        lastLaneEndT_[li] = tp.t + n;
        float* out = &laneOut[li][static_cast<std::size_t>(c)];
        if (skipped_[li])
        {
            std::fill(out, out + n, 0.0f);
            return true;
        }
        std::fill(inBuf_.begin(), inBuf_.begin() + n, 0.0f);
        presence::addLane(l, tp.t, inBuf_.data(), n);
        if (!cb_.lane(l, c, tp, n, inBuf_.data(), out))
        {
            error_ = "lane " + std::to_string(l) + " render failed";
            return false;
        }
        return true;
    }

    ScenarioSpec s_;
    Callbacks cb_;
    std::int64_t capacity_ = 0;
    std::int64_t outC_ = 0;
    std::int64_t cycles_ = 0;
    std::array<std::int64_t, kLanes> laneC_{};
    std::array<bool, kLanes> skipped_{};
    std::array<bool, kLanes> mutedLast_{};
    std::array<bool, kLanes> haveLaneT_{};
    std::array<std::int64_t, kLanes> lastLaneEndT_{};
    bool haveOutT_ = false;
    std::int64_t lastOutEndT_ = 0;
    std::vector<float> inBuf_;
    std::vector<float> zeros_;
    std::string error_;
};

// ---------------------------------------------------------------------------
// 驱动:预热 → 判定窗
// ---------------------------------------------------------------------------

struct DriveHooks
{
    // 每个 Output 周期之后调用(scvb_auhost:按墙钟节拍等待并泵消息)。可空。
    std::function<void(std::int64_t outCursorAfter)> pace;
    // 宿主侧的外部预热判据(scvb_auhost:registry 段)。可空 = 只看 lane 自静音。
    std::function<bool()> externalSettled;
    // 预热时要求哪些 lane 已自静音(宿主不调用的 lane 恒不要求)。只有自测的 F4(某轨从没交给 Output)
    // 会关掉一位 —— 否则那一格停在「预热失败」,测不到判据本身。
    std::array<bool, kLanes> requireMuted{{true, true, true}};
    // 宿主要求提前收工(scvb_auhost:墙钟总时限到了)。可空。
    std::function<bool()> shouldStop;
};

// 判定窗:一组 Output 游标区间,每个区间内总线时间线连续(LS-1 一个,LS-4 每圈一个)。
struct Window
{
    std::int64_t c0 = 0;
    std::int64_t c1 = 0;
};

struct DriveResult
{
    bool ok = false; // 渲染全部成功且预热完成
    bool settled = false;
    std::int64_t settledCursor = -1; // 预热判据第 kSettleStableCycles 次连续成立时的 Output 游标
    std::int64_t endCursor = 0;
    std::vector<Window> windows;
    std::string error;
};

inline std::vector<Window> measureWindows(const ScenarioSpec& s, std::int64_t settledCursor)
{
    std::vector<Window> w;
    const std::int64_t m0 = settledCursor + kSettleMarginSamples;
    if (s.id == Scenario::Ls1)
    {
        w.push_back(Window{m0, m0 + s.measureSamples});
        return w;
    }
    // LS-4:预热 + 0.1 s 之后的第一个循环点起,连续 laps 圈。圈界 = Output 时间线回跳处 = loopLen 的整数倍。
    const std::int64_t k0 = (m0 + s.loopLen - 1) / s.loopLen;
    for (int k = 0; k < s.laps; ++k)
    {
        w.push_back(Window{(k0 + k) * s.loopLen, (k0 + k + 1) * s.loopLen});
    }
    return w;
}

inline DriveResult drive(Engine& e, const DriveHooks& hooks)
{
    DriveResult r;
    const ScenarioSpec& s = e.spec();
    int stable = 0;
    while (true)
    {
        if (hooks.shouldStop && hooks.shouldStop())
        {
            r.error = "stopped by the host before settling (wall-clock limit)";
            r.endCursor = e.outCursor();
            return r;
        }
        if (!e.stepCycle())
        {
            r.error = e.error();
            r.endCursor = e.outCursor();
            return r;
        }
        if (hooks.pace)
        {
            hooks.pace(e.outCursor());
        }
        bool ok = true;
        bool anyCalled = false;
        for (int l = 0; l < kLanes; ++l)
        {
            if (e.skipped(l) || !hooks.requireMuted[static_cast<std::size_t>(l)])
            {
                continue;
            }
            anyCalled = true;
            ok = ok && e.laneMutedLastCycle(l);
        }
        ok = ok && anyCalled && (!hooks.externalSettled || hooks.externalSettled());
        stable = ok ? stable + 1 : 0;
        if (stable >= kSettleStableCycles)
        {
            r.settled = true;
            r.settledCursor = e.outCursor();
            break;
        }
        if (e.outCursor() >= s.settleLimit)
        {
            r.error = "not settled within " + std::to_string(samplesToSec(s.settleLimit)) +
                      " s of audio (lanes never all self-muted / registry never showed them injected)";
            r.endCursor = e.outCursor();
            return r;
        }
    }
    r.windows = measureWindows(s, r.settledCursor);
    const std::int64_t end = r.windows.back().c1;
    while (e.outCursor() < end)
    {
        if (hooks.shouldStop && hooks.shouldStop())
        {
            r.error = "stopped by the host inside the measured windows (wall-clock limit)";
            r.endCursor = e.outCursor();
            return r;
        }
        if (!e.stepCycle())
        {
            r.error = e.error();
            r.endCursor = e.outCursor();
            return r;
        }
        if (hooks.pace)
        {
            hooks.pace(e.outCursor());
        }
    }
    r.endCursor = e.outCursor();
    r.ok = true;
    return r;
}

} // namespace scvb::auhost
