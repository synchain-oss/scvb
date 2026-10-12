// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// auhost_judge —— scvb_auhost 的逐轨在场判定 + 判据 + 自测(B 线 M13)。只用标准库 +
// tests/support/presence_meter.h(A 线 A-1 的尺子),与 auhost_engine.h 一样可在任何平台上编译。
//
// ## 判定
//
// Output 录音按判定窗(auhost_engine.h 的 Window:LS-1 一个窗,LS-4 每圈一个窗)逐窗交给
// presence::analyze:按时间线对齐的 1024 样本帧,逐轨判 Present / Absent / Wrong / Partial,并按 L/R
// 能量比分 SCVB 混音(Mix,居中)与未平衡原声(Raw,宿主侧只进 L)。**只有 Present 且来源是 Mix
// 才算在场**:Raw 在场说明这一路没交给 Output(Input 还在直通或双路叠加),同样算丢失。
//
// ## 判据
//
//   LS-1  每轨:帧数 ≥ 判定窗应有帧数 − 2;Wrong = 0;Mix 在场 ≥ ceil(99% × 帧数)。
//   LS-4  恰好 laps 圈;每圈每轨:帧数 ≥ 90(2 s 圈应有 93);Wrong = 0;丢失(帧数 − Mix 在场)≤ 1。
//   进程(processCheck):registry 里各实例的 pid 都 > 0;进程内 ⇒ 全等于宿主 pid,进程外 ⇒ 全不等于宿主 pid,
//   且每个都是活着的系统进程(可执行文件路径以 /System/ 开头)。
//   pid 由 scvb_auhost.cpp 从插件建的 registry 段里读出来,判定只在这里。
//
// ## 自测(selfTest,scvb_auhost --self-test)
//
// 「尺子 + 宿主模型」自己不判红,后面全部假绿。所以用一对假插件(FakeScvb:Input 按时间线写环、
// 交接后自静音;Output 按自己的时间线读环、居中混音)把 auhost_engine.h 的整条调度与本头的判据跑一遍:
// 理想假插件两个场景都必须过;每种故障只动一处,必须红,而且红在设计接住它的那一轨 / 那一条判据上:
//   F1 宿主不调 lane 1                  ⇒ LS-1 lane 1 不在场
//   F2 Output 读 lane 2 错位 512 样本   ⇒ LS-1 lane 2 Wrong / 不在场
//   F3 读方 H1:循环后领先轨整圈读不到 ⇒ LS-4 lane 1 / 2 每圈丢失 > 1,lane 0 与 LS-1 照绿
//   F4 lane 0 从没交给 Output(Input 一直直通)⇒ LS-1 lane 0 只有原声,不算在场
//   F5 Input 按渲染游标而不是时间线写环(HostCallbacks 没生效的形状)⇒ LS-4 全轨丢失
//   F6 lane 0 有 2 帧读成上一圈环(错 R 样本,反相)⇒ LS-1 lane 0 只红在 Wrong = 0 这一条
//      (2/234 帧,在场率仍 ≥ 99%:这一格专门给「Wrong = 0」那条判据配牙)
//   F7 LS-4 每圈 lane 1 有 1 帧读成上一圈环 ⇒ 只红在 LS-4 的 Wrong = 0(每圈丢 1 帧,仍在 ≤ 1 的容差内)
// 进程判据 processCheck 另有 10 格(三格必须过、七格必须红),在 selfTest 末尾。
// 自测用例全部跑完再汇总(一格红不影响其余格的判断)。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "auhost_engine.h"
#include "support/presence_meter.h"

namespace scvb::auhost
{

struct LaneStats
{
    int frames = 0;
    int mixPresent = 0;
    int rawPresent = 0;
    int absent = 0;
    int wrong = 0;
    int partial = 0;
    int lost() const { return frames - mixPresent; }
    void add(const LaneStats& o)
    {
        frames += o.frames;
        mixPresent += o.mixPresent;
        rawPresent += o.rawPresent;
        absent += o.absent;
        wrong += o.wrong;
        partial += o.partial;
    }
};

struct FrameRow
{
    int window = 0;
    std::int64_t frameStart = 0; // 时间线位置
    int lane = 0;
    presence::Verdict verdict = presence::Verdict::Absent;
    presence::Why why = presence::Why::LowEnergy;
    presence::Source source = presence::Source::None;
    double energyDb = 0.0;
    double rho = 0.0;
};

struct WindowResult
{
    Window w;
    std::int64_t t0 = 0; // 窗首的总线时间线位置
    bool contiguous = true;
    int dirtyFrames = 0;
    std::array<LaneStats, kLanes> lanes{};
};

struct Judgement
{
    bool measured = false; // 判定窗都在录音范围里、时间线连续
    bool pass = false;
    std::array<bool, kLanes> lanePass{};
    std::array<LaneStats, kLanes> total{};
    std::vector<WindowResult> windows;
    std::vector<FrameRow> rows;
    std::vector<std::string> failures;
};

inline WindowResult analyzeWindow(const Engine& e, const Window& w, int index, std::vector<FrameRow>* rows)
{
    WindowResult r;
    r.w = w;
    const std::size_t c0 = static_cast<std::size_t>(w.c0);
    const std::int64_t len = w.c1 - w.c0;
    r.t0 = e.outT[c0];
    for (std::int64_t k = 1; k < len; ++k)
    {
        if (e.outT[c0 + static_cast<std::size_t>(k)] != r.t0 + k)
        {
            r.contiguous = false;
            return r;
        }
    }
    const presence::Span span{&e.outL[c0], &e.outR[c0], r.t0, len};
    const presence::Thresholds th;
    const presence::CombReport rep = presence::analyze(span, {0, 1, 2}, {}, th);
    r.dirtyFrames = rep.dirtyFrames(th);
    for (const presence::LaneReport& lr : rep.lanes)
    {
        LaneStats& st = r.lanes[static_cast<std::size_t>(lr.lane)];
        for (std::size_t i = 0; i < lr.judged.size(); ++i)
        {
            const presence::Judged& j = lr.judged[i];
            const presence::Source src = lr.sources[i];
            ++st.frames;
            switch (j.verdict)
            {
            case presence::Verdict::Present:
                if (src == presence::Source::Mix)
                {
                    ++st.mixPresent;
                }
                else
                {
                    ++st.rawPresent;
                }
                break;
            case presence::Verdict::Absent:
                ++st.absent;
                break;
            case presence::Verdict::Wrong:
                ++st.wrong;
                break;
            case presence::Verdict::Partial:
                ++st.partial;
                break;
            }
            if (rows != nullptr)
            {
                rows->push_back(FrameRow{index, lr.frames[i].frameStart, lr.lane, j.verdict, j.why, src,
                                         presence::msToDbfs(lr.frames[i].energy), lr.frames[i].rho});
            }
        }
    }
    return r;
}

inline Judgement judge(const Engine& e, const DriveResult& d, bool keepRows)
{
    Judgement jd;
    const ScenarioSpec& s = e.spec();
    if (!d.ok || d.windows.empty())
    {
        jd.failures.push_back("not measured: " + (d.error.empty() ? std::string("no windows") : d.error));
        return jd;
    }
    jd.measured = true;
    for (std::size_t wi = 0; wi < d.windows.size(); ++wi)
    {
        const Window& w = d.windows[wi];
        if (w.c0 < 0 || w.c1 > e.outCursor() || w.c1 <= w.c0)
        {
            jd.measured = false;
            jd.failures.push_back("window " + std::to_string(wi) + " outside the recording");
            continue;
        }
        WindowResult wr = analyzeWindow(e, w, static_cast<int>(wi), keepRows ? &jd.rows : nullptr);
        if (!wr.contiguous)
        {
            jd.measured = false;
            jd.failures.push_back("window " + std::to_string(wi) + " is not timeline-contiguous");
        }
        for (int l = 0; l < kLanes; ++l)
        {
            jd.total[static_cast<std::size_t>(l)].add(wr.lanes[static_cast<std::size_t>(l)]);
        }
        jd.windows.push_back(wr);
    }
    if (!jd.measured)
    {
        return jd;
    }
    jd.lanePass.fill(true);
    auto fail = [&jd](int lane, const std::string& why) {
        jd.lanePass[static_cast<std::size_t>(lane)] = false;
        jd.failures.push_back("lane " + std::to_string(lane) + ": " + why);
    };
    if (s.id == Scenario::Ls1)
    {
        const int expectFrames = static_cast<int>(s.measureSamples / presence::kFrame);
        for (int l = 0; l < kLanes; ++l)
        {
            const LaneStats& st = jd.total[static_cast<std::size_t>(l)];
            const int need = static_cast<int>(std::ceil(s.minPresentFraction * st.frames));
            if (st.frames < expectFrames - 2)
            {
                fail(l, "only " + std::to_string(st.frames) +
                            " frames analysed (expected >= " + std::to_string(expectFrames - 2) + ")");
            }
            if (st.wrong != 0)
            {
                fail(l, std::to_string(st.wrong) + " Wrong frames");
            }
            if (st.mixPresent < need)
            {
                fail(l, "mix-present " + std::to_string(st.mixPresent) + "/" + std::to_string(st.frames) + " < " +
                            std::to_string(need) + " (99%)");
            }
        }
    }
    else
    {
        if (static_cast<int>(jd.windows.size()) != s.laps)
        {
            jd.failures.push_back("measured " + std::to_string(jd.windows.size()) + " laps, expected " +
                                  std::to_string(s.laps));
            jd.lanePass.fill(false);
        }
        for (std::size_t wi = 0; wi < jd.windows.size(); ++wi)
        {
            for (int l = 0; l < kLanes; ++l)
            {
                const LaneStats& st = jd.windows[wi].lanes[static_cast<std::size_t>(l)];
                const std::string where = "lap " + std::to_string(wi) + ": ";
                if (st.frames < 90)
                {
                    fail(l, where + "only " + std::to_string(st.frames) + " frames analysed (expected >= 90)");
                }
                if (st.wrong != 0)
                {
                    fail(l, where + std::to_string(st.wrong) + " Wrong frames");
                }
                if (st.lost() > s.maxLostPerLap)
                {
                    fail(l, where + "lost " + std::to_string(st.lost()) + "/" + std::to_string(st.frames) +
                                " frames (> " + std::to_string(s.maxLostPerLap) + ")");
                }
            }
        }
    }
    jd.pass = jd.failures.empty();
    return jd;
}

// 进程判据。pids = registry 里 Output 与各 Input 写下的 pid(0 = 没读到);paths[i] = pids[i] 那个进程此刻的
// 可执行文件路径(进程已经不在、或读不到 = 空串)。
// 进程外时每个 pid 还必须是一个**活着的系统进程**(路径以 /System/ 开头;AUHostingService 在
// /System/Library/Frameworks/AudioToolbox.framework/XPCServices/ 下):registry 段若是之前某一格残留的旧段,
// 里面的旧 pid 本来就 ≠ 宿主 pid,只看「≠ 宿主 pid」会在没有新证据时判过。不要求各实例同在一个进程:
// 实例怎么分进程由系统决定,不是 API 的承诺(实测都在同一个 AUHostingService 里,数字记在 registry.distinct_pids)。
// 进程内时 pid 等于宿主 pid 本身就是新证据(旧段里的 pid 不可能是本进程),路径不另核。
inline bool processCheck(bool outOfProcess, long long hostPid, const std::vector<long long>& pids,
                         const std::vector<std::string>& paths)
{
    if (pids.empty() || paths.size() != pids.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < pids.size(); ++i)
    {
        const long long p = pids[i];
        if (p <= 0)
        {
            return false;
        }
        if (outOfProcess ? (p == hostPid || paths[i].rfind("/System/", 0) != 0) : p != hostPid)
        {
            return false;
        }
    }
    return true;
}

inline std::string percent(int a, int b)
{
    if (b <= 0)
    {
        return "-";
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f%%", 100.0 * a / b);
    return buf;
}

inline std::string statsCells(const LaneStats& st)
{
    std::ostringstream os;
    os << st.frames << " | " << st.mixPresent << " (" << percent(st.mixPresent, st.frames) << ") | " << st.rawPresent
       << " | " << st.absent << " | " << st.partial << " | " << st.wrong;
    return os.str();
}

// markdown 小结(scvb_auhost 拼进 job summary)。
inline std::string judgementMarkdown(const Engine& e, const DriveResult& d, const Judgement& jd)
{
    const ScenarioSpec& s = e.spec();
    std::ostringstream os;
    os << "| lane | role | frames | mix present | raw present | absent | partial | wrong | verdict |\n"
       << "|---|---|---|---|---|---|---|---|---|\n";
    for (int l = 0; l < kLanes; ++l)
    {
        const LaneSpec& ls = s.lanes[static_cast<std::size_t>(l)];
        os << "| " << l << " | " << (ls.live ? std::string("live") : "prefetch lead " + std::to_string(ls.lead))
           << (e.skipped(l) ? " (host never calls it)" : "") << " | "
           << statsCells(jd.total[static_cast<std::size_t>(l)]) << " | "
           << (jd.measured ? (jd.lanePass[static_cast<std::size_t>(l)] ? "pass" : "FAIL") : "-") << " |\n";
    }
    if (s.id == Scenario::Ls4 && !jd.windows.empty())
    {
        os << "\nper lap (lost frames = frames - mix present):\n\n| lap | timeline start | lane 0 | lane 1 | lane 2 |\n"
           << "|---|---|---|---|---|\n";
        for (std::size_t wi = 0; wi < jd.windows.size(); ++wi)
        {
            const WindowResult& w = jd.windows[wi];
            os << "| " << wi << " | " << w.t0;
            for (int l = 0; l < kLanes; ++l)
            {
                const LaneStats& st = w.lanes[static_cast<std::size_t>(l)];
                os << " | lost " << st.lost() << "/" << st.frames
                   << (st.wrong ? " WRONG " + std::to_string(st.wrong) : "");
            }
            os << " |\n";
        }
    }
    os << "\nsettled at " << samplesToSec(d.settledCursor) << " s of audio; measured windows: " << d.windows.size()
       << "; output cycles: " << e.cycles() << "\n";
    if (!jd.failures.empty())
    {
        os << "\nfailures:\n\n";
        for (const std::string& f : jd.failures)
        {
            os << "- " << f << "\n";
        }
    }
    return os.str();
}

inline std::string rowsCsv(const Judgement& jd)
{
    std::ostringstream os;
    os << "window,frame_start,lane,verdict,why,source,energy_db,rho\n";
    for (const FrameRow& r : jd.rows)
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.1f,%.3f", r.energyDb, r.rho);
        os << r.window << ',' << r.frameStart << ',' << r.lane << ',' << presence::toString(r.verdict) << ','
           << presence::toString(r.why) << ',' << presence::toString(r.source) << ',' << buf << '\n';
    }
    return os.str();
}

// 实测提前量:每条预取 lane 在判定窗里各次回跳的 laneCursor − outCursor 的最小 / 最大值(LS-4)。
struct LeadObserved
{
    bool any = false;
    std::int64_t min = 0;
    std::int64_t max = 0;
};

inline LeadObserved observedLead(const Engine& e, const DriveResult& d, int lane)
{
    LeadObserved o;
    if (d.windows.empty())
    {
        return o;
    }
    for (const LaneWrap& w : e.laneWraps[static_cast<std::size_t>(lane)])
    {
        if (w.laneCursor < d.windows.front().c0 || w.laneCursor >= d.windows.back().c1)
        {
            continue;
        }
        const std::int64_t lead = w.laneCursor - w.outCursor;
        o.min = o.any ? std::min(o.min, lead) : lead;
        o.max = o.any ? std::max(o.max, lead) : lead;
        o.any = true;
    }
    return o;
}

// ===========================================================================
// 自测:假插件 + 故障注入
// ===========================================================================

namespace fake
{

enum class Fault
{
    None,
    SkipLane1, // F1
    MisreadLane2, // F2
    H1LeadLanes, // F3
    RawLane0, // F4
    CursorTimeline, // F5
    WrongBurstLane0, // F6
    WrongFrameEveryLap // F7
};

// 一组假 SCVB:Input 按时间线写环(帧位置 & (R−1)),Output 按自己的时间线读环并居中混音。
// 交接:Output 渲染过 kHandoverCycles 个周期之后,Input 自静音、Output 改出「混音 + 总线」
// (总线上只剩没交接的那几路原声 —— 与真 Output 一样,没交给它的轨不能被吞掉);此前 Output 直通总线。
class FakeScvb
{
public:
    static constexpr int kHandoverCycles = 40;
    // F6 那 2 帧的时间线起点:LS-1 判定窗(约 0.6–5.6 s 音频)里,帧对齐。
    static constexpr std::int64_t kBurstT0 = (93 + 200) * presence::kFrame;
    // F7 那 1 帧:LS-4 循环 [141, 141 + 93.75) 帧里的第 40 帧,每圈都读错一次。
    static constexpr std::int64_t kLapBurstT0 = (141 + 40) * presence::kFrame;
    static constexpr float kMixGain = 0.5f; // 居中:L = R = kMixGain · x

    explicit FakeScvb(Fault f) : fault_(f)
    {
        for (auto& r : ring_)
        {
            r.assign(static_cast<std::size_t>(presence::kRingR), 0.0f);
        }
        for (auto& r : ringT_)
        {
            r.assign(static_cast<std::size_t>(presence::kRingR), -1);
        }
    }

    bool lane(int l, std::int64_t cursor, const TimePoint& tp, int n, const float* in, float* out)
    {
        const std::size_t li = static_cast<std::size_t>(l);
        // F5:时间线没传进插件(HostCallbacks 没生效),插件只能拿渲染游标 + 各实例独立的偏移当位置。
        const std::int64_t base = fault_ == Fault::CursorTimeline ? cursor + 1000000 * (l + 1) : tp.t;
        for (int i = 0; i < n; ++i)
        {
            const std::int64_t t = base + i;
            const std::size_t idx = static_cast<std::size_t>(t & (presence::kRingR - 1));
            ring_[li][idx] = in[i];
            ringT_[li][idx] = t;
            out[i] = handedOver(l) ? 0.0f : in[i];
        }
        return true;
    }

    bool output(const TimePoint& tp, int n, const float* inL, const float* inR, float* outL, float* outR)
    {
        if (haveOut_ && tp.t != lastOutEnd_)
        {
            ++outLap_;
        }
        haveOut_ = true;
        lastOutEnd_ = tp.t + n;
        if (outCycles_ < kHandoverCycles)
        {
            std::copy(inL, inL + n, outL);
            std::copy(inR, inR + n, outR);
            return true;
        }
        for (int i = 0; i < n; ++i)
        {
            float mix = 0.0f;
            for (int l = 0; l < kLanes; ++l)
            {
                if (!handedOver(l))
                {
                    continue; // 没交接的轨:原声留在总线上,下面原样加回
                }
                if (fault_ == Fault::H1LeadLanes && l != 0 && outLap_ > 0)
                {
                    continue; // 修前 H1 的形状:写方领先、先换代 ⇒ 读方回到 B 之后整圈读不到
                }
                std::int64_t t = tp.t + i;
                if (fault_ == Fault::MisreadLane2 && l == 2)
                {
                    t -= 512;
                }
                const bool aliased = (fault_ == Fault::WrongBurstLane0 && l == 0 && t >= kBurstT0 &&
                                      t < kBurstT0 + 2 * presence::kFrame) ||
                                     (fault_ == Fault::WrongFrameEveryLap && l == 1 && t >= kLapBurstT0 &&
                                      t < kLapBurstT0 + presence::kFrame);
                if (aliased)
                {
                    // 读到的是环里早一圈(t − R)的内容:载波反相,与期望的相关 ≈ −1。
                    mix += static_cast<float>(presence::sampleAt(presence::laneTone(l), t - presence::kRingR));
                    continue;
                }
                const std::size_t idx = static_cast<std::size_t>(t & (presence::kRingR - 1));
                if (ringT_[static_cast<std::size_t>(l)][idx] == t)
                {
                    mix += ring_[static_cast<std::size_t>(l)][idx];
                }
            }
            outL[i] = kMixGain * mix + inL[i];
            outR[i] = kMixGain * mix + inR[i];
        }
        return true;
    }

    void endCycle() { ++outCycles_; }

private:
    // F4:lane 0 从没交给 Output(Input 一直直通,Output 不混它)。
    bool handedOver(int l) const { return outCycles_ >= kHandoverCycles && !(fault_ == Fault::RawLane0 && l == 0); }

    Fault fault_;
    std::array<std::vector<float>, kLanes> ring_;
    std::array<std::vector<std::int64_t>, kLanes> ringT_;
    std::int64_t outCycles_ = 0;
    bool haveOut_ = false;
    std::int64_t lastOutEnd_ = 0;
    int outLap_ = 0;
};

struct FakeRun
{
    DriveResult drive;
    Judgement jd;
    std::array<LeadObserved, kLanes> lead{};
};

inline FakeRun runFake(const ScenarioSpec& spec, Fault f)
{
    auto fs = std::make_shared<FakeScvb>(f);
    Callbacks cb;
    cb.lane = [fs](int l, std::int64_t c, const TimePoint& tp, int n, const float* in, float* out) {
        return fs->lane(l, c, tp, n, in, out);
    };
    cb.output = [fs](std::int64_t, const TimePoint& tp, int n, const float* inL, const float* inR, float* outL,
                     float* outR) { return fs->output(tp, n, inL, inR, outL, outR); };
    cb.afterOutput = [fs](std::int64_t, int) {
        fs->endCycle();
        return true;
    };
    Engine e(spec, cb);
    DriveHooks hooks;
    if (f == Fault::SkipLane1)
    {
        e.setSkipped(1, true);
    }
    if (f == Fault::RawLane0)
    {
        hooks.requireMuted[0] = false;
    }
    FakeRun r;
    r.drive = drive(e, hooks);
    r.jd = judge(e, r.drive, false);
    for (int l = 0; l < kLanes; ++l)
    {
        r.lead[static_cast<std::size_t>(l)] = observedLead(e, r.drive, l);
    }
    return r;
}

} // namespace fake

// 自测入口:0 = 全过;1 = 有格不符(逐格打印)。
inline int selfTest()
{
    using fake::Fault;
    int failed = 0;
    int cases = 0;
    auto check = [&](bool cond, const std::string& what) {
        ++cases;
        if (!cond)
        {
            ++failed;
            std::printf("[self-test] FAIL: %s\n", what.c_str());
        }
        else
        {
            std::printf("[self-test] ok:   %s\n", what.c_str());
        }
    };
    const ScenarioSpec ls1 = makeLs1();
    const ScenarioSpec ls4 = makeLs4();

    // 理想:两个场景都过,而且每轨 100% 混音在场(假插件是理想读方)。
    {
        const fake::FakeRun r = fake::runFake(ls1, Fault::None);
        check(r.drive.ok && r.jd.measured, "ideal LS-1 is measured");
        check(r.jd.pass, "ideal LS-1 passes");
        for (int l = 0; l < kLanes; ++l)
        {
            const LaneStats& st = r.jd.total[static_cast<std::size_t>(l)];
            check(st.frames >= 230 && st.mixPresent == st.frames,
                  "ideal LS-1 lane " + std::to_string(l) + " fully mix-present (" + std::to_string(st.mixPresent) +
                      "/" + std::to_string(st.frames) + ")");
        }
    }
    {
        const fake::FakeRun r = fake::runFake(ls4, Fault::None);
        check(r.drive.ok && r.jd.measured, "ideal LS-4 is measured");
        check(r.jd.pass, "ideal LS-4 passes");
        check(static_cast<int>(r.jd.windows.size()) == ls4.laps, "ideal LS-4 measures exactly 4 laps");
        for (int l = 0; l < kLanes; ++l)
        {
            const LaneStats& st = r.jd.total[static_cast<std::size_t>(l)];
            check(st.frames == 93 * ls4.laps && st.mixPresent == st.frames,
                  "ideal LS-4 lane " + std::to_string(l) + " fully mix-present, 93 frames per lap (" +
                      std::to_string(st.mixPresent) + "/" + std::to_string(st.frames) + ")");
        }
        // 预取轨确实比 Output 早至少一个提前量跨过循环点(H1 的形状真的出现了)。
        for (int l = 1; l < kLanes; ++l)
        {
            const LeadObserved& o = r.lead[static_cast<std::size_t>(l)];
            const std::int64_t lead = ls4.lanes[static_cast<std::size_t>(l)].lead;
            check(o.any && o.min >= lead && o.max < lead + ls4.outBlock,
                  "ideal LS-4 lane " + std::to_string(l) + " wraps ahead of the Output by [lead, lead + b_out)");
        }
    }
    // F1:宿主不调 lane 1。
    {
        const fake::FakeRun r = fake::runFake(ls1, Fault::SkipLane1);
        check(r.jd.measured && !r.jd.pass, "F1 skip lane 1: LS-1 fails");
        check(!r.jd.lanePass[1] && r.jd.lanePass[0] && r.jd.lanePass[2], "F1 skip lane 1: only lane 1 fails");
        check(r.jd.total[1].mixPresent == 0, "F1 skip lane 1: lane 1 never mix-present");
    }
    // F2:Output 读 lane 2 错位 512 样本。
    {
        const fake::FakeRun r = fake::runFake(ls1, Fault::MisreadLane2);
        check(r.jd.measured && !r.jd.pass, "F2 misread lane 2: LS-1 fails");
        check(!r.jd.lanePass[2] && r.jd.lanePass[0] && r.jd.lanePass[1], "F2 misread lane 2: only lane 2 fails");
        check(r.jd.total[2].mixPresent == 0, "F2 misread lane 2: lane 2 never mix-present");
    }
    // F3:读方 H1。LS-4 领先轨红、live 轨绿;LS-1(没有循环)照绿。
    {
        const fake::FakeRun r = fake::runFake(ls4, Fault::H1LeadLanes);
        check(r.jd.measured && !r.jd.pass, "F3 reader H1: LS-4 fails");
        check(r.jd.lanePass[0] && !r.jd.lanePass[1] && !r.jd.lanePass[2],
              "F3 reader H1: lanes 1 and 2 fail, lane 0 passes");
        const fake::FakeRun r1 = fake::runFake(ls1, Fault::H1LeadLanes);
        check(r1.jd.pass, "F3 reader H1: LS-1 (no cycle) still passes");
    }
    // F4:交接后 lane 0 的原声仍出现在 Output 上(没交给 Output)。
    {
        const fake::FakeRun r = fake::runFake(ls1, Fault::RawLane0);
        check(r.jd.measured && !r.jd.pass, "F4 raw lane 0: LS-1 fails");
        check(!r.jd.lanePass[0] && r.jd.lanePass[1] && r.jd.lanePass[2], "F4 raw lane 0: only lane 0 fails");
        check(r.jd.total[0].rawPresent > 0 && r.jd.total[0].mixPresent == 0,
              "F4 raw lane 0: lane 0 is heard as raw, and raw is not counted as present");
    }
    // F5:插件拿渲染游标当时间线(HostCallbacks 没生效)。
    {
        const fake::FakeRun r = fake::runFake(ls4, Fault::CursorTimeline);
        check(r.jd.measured && !r.jd.pass, "F5 cursor-as-timeline: LS-4 fails");
        check(!r.jd.lanePass[0] && !r.jd.lanePass[1] && !r.jd.lanePass[2], "F5 cursor-as-timeline: every lane fails");
    }
    // F6:lane 0 有 2 帧错音,在场率仍够 ⇒ 只有 Wrong = 0 那条判据接得住。
    {
        const fake::FakeRun r = fake::runFake(ls1, Fault::WrongBurstLane0);
        const LaneStats& st = r.jd.total[0];
        check(r.jd.measured && !r.jd.pass, "F6 2-frame ring-aliased read on lane 0: LS-1 fails");
        check(!r.jd.lanePass[0] && r.jd.lanePass[1] && r.jd.lanePass[2], "F6: only lane 0 fails");
        check(st.wrong == 2, "F6: exactly the 2 aliased frames are Wrong (" + std::to_string(st.wrong) + ")");
        check(st.mixPresent >= static_cast<int>(std::ceil(ls1.minPresentFraction * st.frames)),
              "F6: lane 0 presence alone would still pass (" + std::to_string(st.mixPresent) + "/" +
                  std::to_string(st.frames) + "), so the red comes from the Wrong = 0 criterion");
    }
    // F7:LS-4 每圈 lane 1 错 1 帧 ⇒ 每圈丢失仍 ≤ 1,只有 Wrong = 0 接得住。
    {
        const fake::FakeRun r = fake::runFake(ls4, Fault::WrongFrameEveryLap);
        check(r.jd.measured && !r.jd.pass, "F7 one ring-aliased frame per lap on lane 1: LS-4 fails");
        check(!r.jd.lanePass[1] && r.jd.lanePass[0] && r.jd.lanePass[2], "F7: only lane 1 fails");
        bool withinLost = !r.jd.windows.empty();
        for (const WindowResult& w : r.jd.windows)
        {
            withinLost = withinLost && w.lanes[1].wrong == 1 && w.lanes[1].lost() <= ls4.maxLostPerLap;
        }
        check(withinLost, "F7: every lap has exactly 1 Wrong frame on lane 1 and stays within the lost <= 1 allowance");
    }
    // 进程判据:进程外标志被静默忽略(实例其实在宿主进程里)必须红;反过来也一样;没读到 pid 必须红;
    // 进程外时 pid 不是活着的系统进程(残留旧段里的 pid、或别的程序)必须红。
    {
        const long long host = 100;
        const std::string sys =
            "/System/Library/Frameworks/AudioToolbox.framework/XPCServices/AUHostingServiceXPC_arrow.xpc/Contents/"
            "MacOS/AUHostingServiceXPC_arrow";
        const std::string self = "/opt/build-auhost/scvb_auhost"; // 宿主自己:不在 /System 下
        check(processCheck(false, host, {100, 100, 100, 100}, {self, self, self, self}),
              "process: in-process, all pids are the host pid -> ok");
        check(processCheck(true, host, {200, 200, 200, 200}, {sys, sys, sys, sys}),
              "process: out-of-process, every pid is a live system service, none is the host pid -> ok");
        check(processCheck(true, host, {200, 201, 200, 201}, {sys, sys, sys, sys}),
              "process: out-of-process across two system service processes -> ok (one process is not required)");
        check(!processCheck(true, host, {100, 100, 100, 100}, {self, self, self, self}),
              "process: out-of-process flag ignored (instances in the host process) -> fails");
        check(!processCheck(true, host, {200, 200, 100, 200}, {sys, sys, self, sys}),
              "process: one OOP instance in the host process -> fails");
        check(!processCheck(false, host, {100, 200, 100, 100}, {self, sys, self, self}),
              "process: in-process run with a foreign pid -> fails");
        // pid 0 = 没读到:单凭这一条就红,不靠路径(这里故意给它一个系统路径)。
        check(!processCheck(true, host, {200, 0, 200, 200}, {sys, sys, sys, sys}),
              "process: a pid that was never written -> fails");
        check(!processCheck(false, host, {}, {}), "process: no pids at all -> fails");
        check(!processCheck(true, host, {300, 300, 300, 300}, {"", "", "", ""}),
              "process: out-of-process pids from a leftover registry (process no longer alive) -> fails");
        const std::string other = "/usr/local/bin/other";
        check(!processCheck(true, host, {400, 400, 400, 400}, {other, other, other, other}),
              "process: out-of-process pid is a live non-system process -> fails");
    }
    std::printf("[self-test] %d cases, %d failed\n", cases, failed);
    return failed == 0 ? 0 : 1;
}

} // namespace scvb::auhost
