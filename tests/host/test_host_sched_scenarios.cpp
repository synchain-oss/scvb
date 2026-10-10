// SPDX-License-Identifier: GPL-3.0-or-later
// test_host_sched_scenarios —— 宿主调度复现场景 LS-1…15(A 线 A-4)。
//
// 处理器级复现:真 ScvbOutputAudioProcessor + 真 ScvbInputAudioProcessor、真共享内存、真 Output 混音路径
// (tests/host/sched/SchedRig.h),逐轨在场判据(tests/support/presence_meter.h)。A-3 已在读方层面
// (tests/core/test_mix_source_provenance.cpp)证实 / 钉住的几族,这里在整条混音路径上复现并量化;
// 读方层面推不出来的(看门狗、注入仲裁、墙钟门限、Input 侧的静音 / 直通仲裁)也只有这里看得见。
//
// 本文件里的用例一律打 `[.][sched]`(理由见 test_host_sched.cpp 头注):主条目 `scvb_host_tests`
// 默认不跑,ctest 条目 `scvb_host_sched_tests` 显式选中。
//
// ## 判据(三层,设计稿 §3.2)
//
//   1. 逐轨在场位图:Output 录音按时间线对齐的 1024 样本帧逐轨判 Present / Absent / Wrong / Partial,
//      并按 L/R 能量比分 SCVB 混音(Mix,居中)与未平衡原声(Raw,宿主侧只进 L)。
//   2. 理想调度差分:同一份走带脚本在「全 live、Δ=0、按图顺序」的台上再跑一遍,两边按「脚本起点之后的
//      Output 游标」逐帧对齐;理想台上 Present、被测台上不是的帧记「丢失」。只有声明过的过渡窗不计
//      (例:LS-3 宿主自己跳过渲染的那一段 —— 那里正确的输出本来就是静音)。墙钟门限类场景(LS-11/12/
//      14/15)的「理想」就是源信号本身(第一帧就该在场),不另跑一台。
//   3. 计数器 / 告警断言之前先 REQUIRE 前提成立(交接确实走完、轨确实注入、圈确实回绕、怪癖确实被
//      消费、Output 确实被停调 …),沿用 SL-254 写法。
//   另有环可用性 oracle(LS-4,见 ringOracle):按 LaneBlock / OutBlock 的处理顺序号重放各 Input 的写环,
//   判「Output 读这一帧的那一刻,环里是不是正好写着本位置的有声数据」。任何 I1 安全的读方都只能读到
//   这些帧,所以它是在场率的物理上界:oracle 说读得到、被测读方没读到的帧,缺陷在读方,不在调度。
//
// ## [!shouldfail] 约定(同 A-3)
//
//   修复前预期失败的格打 `[!shouldfail]`(Catch2:用例失败 = 通过;用例**通过**反而判红 —— 判据一旦
//   空转、或缺陷被别的改动顺手修掉,这里当场红,逼着修复卡去掉标记)。每条配一条同名「- precondition」
//   普通用例:REQUIRE 场景确实跑到,且只断言**修复前后都成立**的事实,修复卡去掉标记时它不用动。
//   同一族里只有部分格出错时按格拆开:[!shouldfail] 只收实测出错的格,其余格进普通用例 —— 否则族里
//   现在安全的格以后退化了,会被同一条 shouldfail 吞掉。
//   [A-5] E1 读方换代处理落地后去掉了标记的(现在是必须绿的普通用例):LS-3 跳跃组、LS-4、LS-6 k≥1、
//   LS-8 垃圾值、LS-12 的错音(H4,含领先离线 6 s 格)、LS-13 的两次定位 / ε<0 / 几何三条。仍留着标记的:
//   N2b 与 LS-14(H3,归 A-6)、LS-11a(H5,归 A-7)、LS-12 的句首(E2-a / E2-b,归 A-8)。LS-12 领先轨的
//   句首判据随 A-5 改成「最多丢一个写方块」(checkLs12LeadHead,设计稿 §8 第 4 条),领先实时 1 s 格挪进 E2-a。
//   下面各场景注释里描述的「修前」现象指 A-5 之前的读方,保留作证据。
//
// ## 场景结果缓存与节拍
//
//   每个场景只跑一次(函数内 static),同名的判据用例与前提用例共用结果;单独跑其中一条时按需计算。
//   跑场景的函数不断言(建台里的 REQUIRE 除外),结果全进 Run;断言都在用例里。
//   纯时间线场景用 pace=16(kFastPace,见其注释;定义见 SchedRig.h 头注);跨墙钟门限的场景(LS-11、
//   LS-12 实时格、LS-13 几何格的改布局段、LS-14、LS-15)用 pace=1;离线格 LS-10 用 pace=10、LS-12 用
//   pace=5(见各自注释)。
//   另:settle 之后统一再放 0.1 s 才进脚本(见 runScenario),交接期的总线交叉不混进判定。
//
// ## 数字
//
//   每个前提用例(以及几条普通用例)都用 WARN 打摘要(Catch2 的 WARN 不判红、恒输出)。PR 里的结论表
//   从这里抄:`scvb_host_tests "[sched]" --reporter compact`。
//
// 运行期文案一律 ASCII(本机 CP936 上中文字面量会触发 C4819);中文只在注释里。

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

// 同机独占守卫:本二进制已由 test_host_harness.cpp 引入一次;这里再引一次是为了让本文件的用例不依赖
// 「别的 TU 恰好引过」(头注:每进程至多取一次锁,重复引入安全)。只在 Windows 上有这把锁。
#if defined(_WIN32)
#include "support/exclusive_guard.h"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "sched/SchedRig.h"
#include "support/presence_meter.h"

namespace
{

namespace presence = scvb::testsupport::presence;
namespace sched = scvb::testsupport::sched;

using presence::Source;
using presence::Verdict;
using sched::SchedRig;

constexpr std::int64_t kF = presence::kFrame;
constexpr std::int64_t kRing = presence::kRingR;
constexpr std::int64_t kBig = sched::kFar;

std::int64_t sec(double s)
{
    return sched::secondsToSamples(s);
}

// 帧对齐的时间线位置(向下取到 1024 的整数倍;只用于 s >= 0)。
std::int64_t alignedSec(double s)
{
    return (sec(s) / kF) * kF;
}

double toSec(std::int64_t samples)
{
    return static_cast<double>(samples) / sched::kSchedSr;
}

// 纯时间线场景的节拍。不用 pace=0(每周期 runDispatchLoopUntil(1)):那一档每周期至少睡一次,开发中
// 整套先后量到 193 s / 210 s / 660 s(其间用例略有增减),660 s 那次块长 64 的台连交接都没走完 ——
// 推断是那一次睡眠吃系统计时器粒度(15.6 ms 时每个 1024 样本的周期都被拖成约 16 ms ≈ 实时;未单独验证)。
// pace>0 按墙钟**绝对时刻**对齐(SchedRig::paceAfterCycle):某次睡过头,后面几个周期就不睡、只每
// 10 ms 泵一次,平均速度不吃计时器粒度(改了之后 ctest 三轮相差不到 1 s)。16 倍:纯时间线场景不跨任何
// 墙钟门限(最长的一段读失败也只有约 3 s 音频 ≈ 0.19 s 墙钟,远低于 500 ms 的停滞门限)。
constexpr double kFastPace = 16.0;

// n 条 live lane。
sched::SchedCfg lanesCfg(int n, double pace)
{
    sched::SchedCfg c = sched::liveLanes(n);
    c.pace = pace;
    c.unpacedPumpMs = 1;
    return c;
}

void makePrefetch(sched::SchedCfg& c, int lane, std::int64_t lead, int blockIn = 1024)
{
    sched::LaneCfg& l = c.lanes[static_cast<std::size_t>(lane)];
    l.live = false;
    l.lead = lead;
    l.blockIn = blockIn;
}

// ===========================================================================
// 一次场景运行的全部记录
// ===========================================================================

// 一次 Input 侧采样(bridgeTickSnapshot):这一轨此刻是否「交给 Output 了」(connected_mask 本位)、
// 输出级目标档是不是直通。LS-11 / LS-15 要看 Input 那一侧的仲裁,Output 的计数器看不到它。
struct InputSample
{
    std::int64_t rel = 0;
    double wall = 0.0;
    std::vector<char> maskBit;
    std::vector<char> passthrough;
};

struct Run
{
    std::string name;
    bool settled = false;
    int lanes = 0;
    std::int64_t c0 = 0; // 脚本起点(交接走完之后)的 Output 游标;下面所有 rel 都相对它
    std::int64_t c1 = 0;
    double scriptWallMs = 0.0;
    sched::Analysis out; // Output 录音 [c0, c1) 的逐帧判定
    std::vector<sched::OutBlock> outBlocks; // 只含 c0 之后的块(clearRecording)
    std::vector<std::vector<sched::LaneBlock>> laneLogs; // 全程(含交接期:环可用性 oracle 要重放它)
    std::vector<sched::MBeat> beats; // c0 之后的 [M] 拍
    sched::InvariantStats inv;
    std::int64_t busUnderflow = 0;
    std::vector<sched::Range> outputSuspend; // 绝对游标
    std::vector<sched::Range> chainSuspend;
    std::map<std::string, std::int64_t> mark; // 脚本里记下的相对游标
    std::map<std::string, double> wall; // 脚本里记下的墙钟时刻(ms)
    std::map<std::string, std::int64_t> val; // 脚本里记下的其他数值(例:定位目标的时间线位置)
    std::vector<InputSample> inputs;
    std::function<void(SchedRig&)> sampler; // sampleInputs 的自续调度体(只在脚本运行期间被调用)
};

using Script = std::function<void(SchedRig&, Run&)>;

// 本进程里是否已有一台交接没走完。交接正常 0.3–0.7 s 走完(预算 8 s,与 SchedRig::settle 的默认值同);
// 一旦有一台走不完,多半是全局性的(组号撞车、段建不出来),后面每台都会等满预算 —— 一百多台 × 8 s 会把
// 整套拖过 ctest 上界,真正的失败原文反而来不及打出来。所以第一台失败之后,后面的台只给 1.5 s。
bool gSettleFailed = false;

// 建台 → 交接走完(settle)→ 丢掉交接期录音 → 跑脚本 → 判定。不断言:交接没走完时 settled=false,
// 由前提用例 REQUIRE。
Run runScenario(const std::string& name, const sched::SchedCfg& cfg, const Script& script,
                const presence::Thresholds& th = presence::Thresholds{})
{
    Run run;
    run.name = name;
    run.lanes = static_cast<int>(cfg.lanes.size());
    SchedRig rig(cfg);
    run.settled = rig.settle(gSettleFailed ? 1500 : 8000);
    if (!run.settled)
    {
        gSettleFailed = true;
        return run;
    }
    // settle() 只要求连续 8 个周期「读得到且总线已静音」,不保证 Output 那道 80 ms 的总线交叉已走完 ——
    // 块长 256 时 8 个周期只有 43 ms,脚本起点那几帧还在交叉里,幅度核对会判 Partial(实测:理想调度
    // 同样出现,与调度无关)。再放 0.1 s 让交叉走完,脚本从稳态起步。
    rig.runFor(sec(0.1));
    rig.clearRecording();
    run.c0 = rig.outCursor();
    const std::size_t beat0 = rig.beats().size();
    const double w0 = sched::wallMs();
    script(rig, run);
    run.scriptWallMs = sched::wallMs() - w0;
    // sampleInputs 的自续闭包按引用捕获了本函数里的 run;Run 随后按值返回、被搬进各缓存,闭包里的引用就
    // 失效了。脚本跑完立刻清掉,免得以后有人在用例里复用它(那就是悬垂引用)。
    run.sampler = nullptr;
    run.c1 = rig.outCursor();
    run.out = rig.analyzeOutput(run.c0, run.c1, th);
    run.outBlocks = rig.outBlocks();
    for (int i = 0; i < rig.laneCount(); ++i)
    {
        run.laneLogs.push_back(rig.laneLog(i));
    }
    run.beats.assign(rig.beats().begin() + static_cast<std::ptrdiff_t>(beat0), rig.beats().end());
    run.inv = rig.invariant();
    run.busUnderflow = rig.busUnderflowSamples();
    run.outputSuspend = rig.outputSuspendLog();
    run.chainSuspend = rig.chainSuspendLog();
    return run;
}

// 在相对游标 rel 处(落在 Output 块格上)记一个标记与当时的墙钟。
void markAt(SchedRig& rig, Run& run, const std::string& key, std::int64_t rel)
{
    rig.at(run.c0 + rel, [&run, key](SchedRig& r) {
        run.mark[key] = r.outCursor() - run.c0;
        run.wall[key] = sched::wallMs();
    });
}

void markNow(SchedRig& rig, Run& run, const std::string& key)
{
    run.mark[key] = rig.outCursor() - run.c0;
    run.wall[key] = sched::wallMs();
}

// 每 period 个样本(落在 Output 块格上)采一次各 Input 的 bridgeTickSnapshot,直到相对游标 untilRel。
void sampleInputs(SchedRig& rig, Run& run, std::int64_t period, std::int64_t untilRel)
{
    run.sampler = [&run, period, untilRel](SchedRig& r) {
        InputSample s;
        s.rel = r.outCursor() - run.c0;
        s.wall = sched::wallMs();
        for (int i = 0; i < r.laneCount(); ++i)
        {
            const ScvbInputAudioProcessor::BridgeTickSnapshot snap = r.input(i).bridgeTickSnapshot();
            s.maskBit.push_back(static_cast<char>(snap.conn.maskBit ? 1 : 0));
            s.passthrough.push_back(static_cast<char>(snap.passthrough ? 1 : 0));
        }
        run.inputs.push_back(std::move(s));
        if (r.outCursor() - run.c0 + period < untilRel)
        {
            r.at(r.outCursor() + period, run.sampler);
        }
    };
    rig.at(sched::kNow, run.sampler);
}

// ===========================================================================
// 逐帧统计
// ===========================================================================

struct Tally
{
    int total = 0;
    int present = 0;
    int absent = 0;
    int wrong = 0;
    int partial = 0;
    int mixPresent = 0;
    int rawPresent = 0;
    int rawAny = 0; // 来源判成 Raw 的帧(不论判定)
};

bool inWindow(const Run& r, const sched::FrameVerdict& f, std::int64_t rel0, std::int64_t rel1)
{
    const std::int64_t rel = f.cursor - r.c0;
    return rel >= rel0 && rel + kF <= rel1;
}

Tally tally(const Run& r, int lane, std::int64_t rel0 = -kBig, std::int64_t rel1 = kBig, bool skipEdge = true)
{
    Tally t;
    for (const sched::FrameVerdict& f : r.out.frames)
    {
        if (f.lane != lane || !inWindow(r, f, rel0, rel1) || (skipEdge && f.edge))
        {
            continue;
        }
        ++t.total;
        t.present += f.verdict == Verdict::Present ? 1 : 0;
        t.absent += f.verdict == Verdict::Absent ? 1 : 0;
        t.wrong += f.verdict == Verdict::Wrong ? 1 : 0;
        t.partial += f.verdict == Verdict::Partial ? 1 : 0;
        t.mixPresent += (f.verdict == Verdict::Present && f.source == Source::Mix) ? 1 : 0;
        t.rawPresent += (f.verdict == Verdict::Present && f.source == Source::Raw) ? 1 : 0;
        t.rawAny += f.source == Source::Raw ? 1 : 0;
    }
    return t;
}

std::string str(const Tally& t)
{
    std::ostringstream os;
    os << "P" << t.present << "/" << t.total << " (mix " << t.mixPresent << " raw " << t.rawPresent << ") A" << t.absent
       << " W" << t.wrong << " Pa" << t.partial << " rawAny " << t.rawAny;
    return os.str();
}

double pctOf(int a, int b)
{
    return b > 0 ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0;
}

// 窗口内不是「Mix 在场」的帧(诊断用,最多 maxN 帧):相对游标@时间线位置:判定/原因/来源。
std::string describeNonMix(const Run& r, int lane, std::int64_t rel0 = -kBig, std::int64_t rel1 = kBig, int maxN = 8)
{
    std::ostringstream os;
    int n = 0;
    for (const sched::FrameVerdict& f : r.out.frames)
    {
        if (f.lane != lane || !inWindow(r, f, rel0, rel1) || (f.verdict == Verdict::Present && f.source == Source::Mix))
        {
            continue;
        }
        if (n++ < maxN)
        {
            os << " " << (f.cursor - r.c0) << "@" << f.t << ":" << presence::toString(f.verdict) << "/"
               << presence::toString(f.why) << "/" << presence::toString(f.source) << (f.edge ? "/edge" : "");
        }
    }
    if (n > maxN)
    {
        os << " ...(+" << (n - maxN) << ")";
    }
    return os.str();
}

// 窗口内第一个满足 pred 的帧的相对游标;没有返回 -1。帧按游标递增排列(Analysis 逐段、段内逐帧)。
template<typename Pred>
std::int64_t firstFrame(const Run& r, int lane, std::int64_t rel0, std::int64_t rel1, Pred pred)
{
    std::int64_t best = -1;
    for (const sched::FrameVerdict& f : r.out.frames)
    {
        if (f.lane != lane || !inWindow(r, f, rel0, rel1) || f.edge || !pred(f))
        {
            continue;
        }
        const std::int64_t rel = f.cursor - r.c0;
        if (best < 0 || rel < best)
        {
            best = rel;
        }
    }
    return best;
}

bool isMix(const sched::FrameVerdict& f)
{
    return f.verdict == Verdict::Present && f.source == Source::Mix;
}

bool isRaw(const sched::FrameVerdict& f)
{
    return f.verdict == Verdict::Present && f.source == Source::Raw;
}

// 理想调度差分:按相对游标对齐(同一份走带脚本 ⇒ 同一游标上是同一时间线位置,对不上的帧另计)。
struct Diff
{
    int idealPresent = 0; // 理想台上 Present 的帧(分母)
    int lost = 0; // 理想 Present、被测不是
    int wrong = 0; // 被测 Wrong(不论理想是什么)
    int unmatched = 0; // 被测帧在理想台上找不到同游标同位置的帧
    std::int64_t firstLostRel = -1;
    std::int64_t lastLostRel = -1;
};

Diff diffVsIdeal(const Run& ideal, int idealLane, const Run& tested, int testedLane, std::int64_t rel0 = -kBig,
                 std::int64_t rel1 = kBig, const std::vector<sched::Range>& excused = {})
{
    std::map<std::int64_t, const sched::FrameVerdict*> at;
    for (const sched::FrameVerdict& f : ideal.out.frames)
    {
        if (f.lane == idealLane && !f.edge)
        {
            at[f.cursor - ideal.c0] = &f;
        }
    }
    Diff d;
    for (const sched::FrameVerdict& f : tested.out.frames)
    {
        if (f.lane != testedLane || !inWindow(tested, f, rel0, rel1) || f.edge)
        {
            continue;
        }
        const std::int64_t rel = f.cursor - tested.c0;
        d.wrong += f.verdict == Verdict::Wrong ? 1 : 0;
        const auto it = at.find(rel);
        if (it == at.end() || it->second->t != f.t)
        {
            ++d.unmatched;
            continue;
        }
        if (it->second->verdict != Verdict::Present)
        {
            continue;
        }
        const bool isExcused = std::any_of(excused.begin(), excused.end(),
                                           [rel](const sched::Range& x) { return x.overlaps(rel, rel + kF); });
        if (isExcused)
        {
            continue;
        }
        ++d.idealPresent;
        if (f.verdict != Verdict::Present)
        {
            ++d.lost;
            d.firstLostRel = d.firstLostRel < 0 ? rel : d.firstLostRel;
            d.lastLostRel = rel;
        }
    }
    return d;
}

std::string str(const Diff& d)
{
    std::ostringstream os;
    os << "lost " << d.lost << "/" << d.idealPresent << " wrong " << d.wrong;
    if (d.unmatched != 0)
    {
        os << " unmatched " << d.unmatched;
    }
    if (d.firstLostRel >= 0)
    {
        os << " lostRel[" << d.firstLostRel << "," << d.lastLostRel << "]";
    }
    return os.str();
}

// 相对游标 rel 处(帧起点)该轨的判定;没有这一帧返回 nullptr。
const sched::FrameVerdict* frameAt(const Run& r, int lane, std::int64_t rel)
{
    for (const sched::FrameVerdict& f : r.out.frames)
    {
        if (f.lane == lane && f.cursor - r.c0 == rel)
        {
            return &f;
        }
    }
    return nullptr;
}

// Output 报给插件的位置与总线位置不一致的块(宿主时间戳怪癖)所覆盖的相对游标区间。这些块里读方
// 交出的是**它被告知的那个位置**的数据,按总线时间判定必然对不上 —— 错位是宿主造成的,不归读方。
std::vector<sched::Range> mislabeledBlocks(const Run& r)
{
    std::vector<sched::Range> out;
    for (const sched::OutBlock& b : r.outBlocks)
    {
        if (b.reason == sched::CallReason::Called && b.tReported != b.tBus)
        {
            out.push_back(sched::Range{b.cursor - r.c0, b.cursor - r.c0 + b.n});
        }
    }
    return out;
}

// 窗口内、且不与 excused 区间重叠的 Wrong 帧数。
int wrongOutside(const Run& r, int lane, std::int64_t rel0, std::int64_t rel1, const std::vector<sched::Range>& excused)
{
    int n = 0;
    for (const sched::FrameVerdict& f : r.out.frames)
    {
        if (f.lane != lane || !inWindow(r, f, rel0, rel1) || f.edge || f.verdict != Verdict::Wrong)
        {
            continue;
        }
        const std::int64_t rel = f.cursor - r.c0;
        const bool isExcused = std::any_of(excused.begin(), excused.end(),
                                           [rel](const sched::Range& x) { return x.overlaps(rel, rel + kF); });
        n += isExcused ? 0 : 1;
    }
    return n;
}

// Output 时间线往回跳(循环回绕、回跳定位)的相对游标。
std::vector<std::int64_t> outputBackJumps(const Run& r)
{
    std::vector<std::int64_t> out;
    for (std::size_t i = 1; i < r.outBlocks.size(); ++i)
    {
        const sched::OutBlock& a = r.outBlocks[i - 1];
        const sched::OutBlock& b = r.outBlocks[i];
        if (a.playingBus && b.playingBus && b.tBus < a.tBus)
        {
            out.push_back(b.cursor - r.c0);
        }
    }
    return out;
}

// 失准:窗口 [rel0, rel1) 内各拍该轨 misalignCount 的最大值。
std::uint32_t maxMisalign(const Run& r, int lane, std::int64_t rel0 = -kBig, std::int64_t rel1 = kBig)
{
    std::uint32_t m = 0;
    for (const sched::MBeat& b : r.beats)
    {
        const std::int64_t rel = b.outCursor - r.c0;
        if (rel >= rel0 && rel < rel1)
        {
            m = std::max(m, b.lanes[static_cast<std::size_t>(lane)].misalign);
        }
    }
    return m;
}

std::uint32_t maxMisalignAll(const Run& r)
{
    std::uint32_t m = 0;
    for (int lane = 0; lane < r.lanes; ++lane)
    {
        m = std::max(m, maxMisalign(r, lane));
    }
    return m;
}

// 某条 lane 的块日志里,c0 之后时间线第一次「往回跳」(游标前进而位置后退)的那一块;没有返回 nullptr。
const sched::LaneBlock* laneFirstBackJump(const Run& r, int lane)
{
    const std::vector<sched::LaneBlock>& log = r.laneLogs[static_cast<std::size_t>(lane)];
    const sched::LaneBlock* prev = nullptr;
    for (const sched::LaneBlock& b : log)
    {
        if (b.cursor < r.c0)
        {
            prev = nullptr;
            continue;
        }
        if (prev != nullptr && b.cursor > prev->cursor && b.tBus < prev->tBus)
        {
            return &b;
        }
        prev = &b;
    }
    return nullptr;
}

// 某条 lane 在 c0 之后(按处理顺序)第一次渲染时间线位置 t 的那一块;没有返回 nullptr。定位冲刷
// 让游标往回退,laneFirstBackJump 认不出它,定位类场景用这个。
const sched::LaneBlock* laneFirstBlockAt(const Run& r, int lane, std::int64_t t, std::int64_t afterSeq = -1)
{
    for (const sched::LaneBlock& b : r.laneLogs[static_cast<std::size_t>(lane)])
    {
        if (b.cursor >= r.c0 && b.tBus == t && b.seq > afterSeq)
        {
            return &b;
        }
    }
    return nullptr;
}

// 处理顺序号 seq 之后、最先处理的那个 Output 块的相对游标(「这件事发生时 Output 走到了哪」)。
std::int64_t outRelAtSeq(const Run& r, std::int64_t seq)
{
    for (const sched::OutBlock& b : r.outBlocks)
    {
        if (b.seq > seq)
        {
            return b.cursor - r.c0;
        }
    }
    return -1;
}

int countLaneReason(const Run& r, int lane, sched::CallReason reason, std::int64_t rel0, std::int64_t rel1)
{
    int n = 0;
    for (const sched::LaneBlock& b : r.laneLogs[static_cast<std::size_t>(lane)])
    {
        const std::int64_t rel = b.cursor - r.c0;
        if (rel >= rel0 && rel < rel1 && b.reason == reason)
        {
            ++n;
        }
    }
    return n;
}

int countOutReason(const Run& r, sched::CallReason reason, std::int64_t rel0, std::int64_t rel1)
{
    int n = 0;
    for (const sched::OutBlock& b : r.outBlocks)
    {
        const std::int64_t rel = b.cursor - r.c0;
        if (rel >= rel0 && rel < rel1 && b.reason == reason)
        {
            ++n;
        }
    }
    return n;
}

// ===========================================================================
// 环可用性 oracle(LS-4)
// ===========================================================================
//
// 按处理顺序号(seq)把各 lane 的写环与 Output 的读环重放一遍:lane 块被调用 ⇒ 它报给插件的位置
// [t, t+n) 写进该 lane 的环槽(t & (R-1));Output 块被调用 ⇒ 判这一刻环槽里是不是正好写着本位置
// 的有声数据。这是 Input 写环协议(InputProcessor.cpp:291-298 按段 AudioRing::write)在样本级的
// 投影,不看 epoch / 写头 —— 那些是读方用来**推断**环槽内容的元数据,oracle 直接看内容本身。
// 只支持「报给插件的位置 == 总线位置、且 >= 0」的脚本(没有怪癖、没有负 t0、没有读方偏移);
// 超出建模范围时 supported=false,调用方 REQUIRE 它。
struct Oracle
{
    bool supported = true;
    std::vector<std::vector<char>> avail; // [lane][rel]:Output 读这个样本时环里是本位置的有声数据
};

Oracle ringOracle(const Run& r)
{
    Oracle o;
    const std::int64_t len = r.c1 - r.c0;
    const std::size_t nl = static_cast<std::size_t>(r.lanes);
    o.avail.assign(nl, std::vector<char>(static_cast<std::size_t>(std::max<std::int64_t>(len, 0)), 0));
    // 环槽里写着的位置;-1 = 没写过,-2 = 写的是静音(源信号不在放)。
    std::vector<std::vector<std::int64_t>> slot(nl, std::vector<std::int64_t>(static_cast<std::size_t>(kRing), -1));
    std::vector<std::size_t> next(nl, 0);
    const std::int64_t mask = kRing - 1;

    const auto apply = [&](std::size_t lane, const sched::LaneBlock& b) {
        if (b.reason != sched::CallReason::Called)
        {
            return;
        }
        if (b.quirked || b.tReported != b.tBus || b.tReported < 0)
        {
            o.supported = false;
            return;
        }
        for (std::int64_t i = 0; i < b.n; ++i)
        {
            slot[lane][static_cast<std::size_t>((b.tReported + i) & mask)] = b.contentActive ? b.tReported + i : -2;
        }
    };

    for (const sched::OutBlock& ob : r.outBlocks)
    {
        for (std::size_t lane = 0; lane < nl; ++lane)
        {
            const std::vector<sched::LaneBlock>& log = r.laneLogs[lane];
            while (next[lane] < log.size() && log[next[lane]].seq < ob.seq)
            {
                apply(lane, log[next[lane]]);
                ++next[lane];
            }
        }
        if (ob.reason != sched::CallReason::Called || !ob.playingReported)
        {
            continue;
        }
        if (ob.quirked || ob.tReported != ob.tBus || ob.tReported < 0)
        {
            o.supported = false;
            continue;
        }
        for (std::size_t lane = 0; lane < nl; ++lane)
        {
            for (std::int64_t i = 0; i < ob.n; ++i)
            {
                const std::int64_t rel = ob.cursor + i - r.c0;
                if (rel < 0 || rel >= len)
                {
                    continue;
                }
                const std::int64_t p = ob.tReported + i;
                o.avail[lane][static_cast<std::size_t>(rel)] =
                    static_cast<char>(slot[lane][static_cast<std::size_t>(p & mask)] == p ? 1 : 0);
            }
        }
    }
    return o;
}

// 窗口内(帧对齐)oracle 判「整帧可读」的帧数与帧总数。
std::pair<int, int> oracleFrames(const Run& r, const Oracle& o, int lane, std::int64_t rel0, std::int64_t rel1)
{
    int ok = 0;
    int total = 0;
    for (const sched::FrameVerdict& f : r.out.frames)
    {
        if (f.lane != lane || !inWindow(r, f, rel0, rel1) || f.edge)
        {
            continue;
        }
        ++total;
        const std::int64_t rel = f.cursor - r.c0;
        bool all = true;
        for (std::int64_t i = 0; i < kF && all; ++i)
        {
            all = o.avail[static_cast<std::size_t>(lane)][static_cast<std::size_t>(rel + i)] != 0;
        }
        ok += all ? 1 : 0;
    }
    return {ok, total};
}

// ===========================================================================
// LS-1 稳态提前量对照
// ===========================================================================
//
// lane 0 live;lane 1、2 预取(块长 1024)提前 L。无 Cycle、无定位(脚本起点那一次定位把各轨对齐到
// 同一位置),写方稳态领先读方 L < R:读方每块要的数据都已写好且没被套圈 ⇒ 全在场。
struct Ls1Cell
{
    std::int64_t lead = 0;
    int outBlock = 0;
    Run run;
};

Run runLs1(const std::string& name, const sched::SchedCfg& cfg, int outBlock)
{
    const std::int64_t s0 = alignedSec(2.0);
    const std::int64_t dur = outBlock == 64 ? sec(1.0) : sec(2.0);
    return runScenario(name, cfg, [s0, dur](SchedRig& rig, Run&) {
        rig.seek(sched::kNow, s0);
        rig.runFor(dur);
    });
}

// 每个 Output 块长一台理想调度(全 live、同一份脚本)。
const std::vector<Run>& ls1Ideal()
{
    static const std::vector<Run> ideal = [] {
        std::vector<Run> out;
        for (const int b : {64, 256, 1024})
        {
            sched::SchedCfg cfg = lanesCfg(3, kFastPace);
            cfg.outBlock = b;
            std::ostringstream os;
            os << "LS-1 ideal bout=" << b;
            out.push_back(runLs1(os.str(), cfg, b));
        }
        return out;
    }();
    return ideal;
}

const Run& ls1IdealFor(int outBlock)
{
    return ls1Ideal()[outBlock == 64 ? 0u : (outBlock == 256 ? 1u : 2u)];
}

const std::vector<Ls1Cell>& ls1Cells()
{
    static const std::vector<Ls1Cell> cells = [] {
        std::vector<Ls1Cell> out;
        for (const std::int64_t lead : {0, 1024, 4096, 16384})
        {
            for (const int b : {64, 256, 1024})
            {
                sched::SchedCfg cfg = lanesCfg(3, kFastPace);
                cfg.outBlock = b;
                makePrefetch(cfg, 1, lead);
                makePrefetch(cfg, 2, lead);
                std::ostringstream os;
                os << "LS-1 lead=" << lead << " bout=" << b;
                out.push_back(Ls1Cell{lead, b, runLs1(os.str(), cfg, b)});
            }
        }
        return out;
    }();
    return cells;
}

// ===========================================================================
// LS-2 起播突发
// ===========================================================================
//
// 放 1 s → 停走带 3 块 → 从 20 s 起播:预取轨在起播那一个周期里一口气渲染 lead + b_out(突发)。
struct Ls2Cell
{
    std::int64_t lead = 0;
    Run run;
};

const std::vector<Ls2Cell>& ls2Cells()
{
    static const std::vector<Ls2Cell> cells = [] {
        std::vector<Ls2Cell> out;
        for (const std::int64_t lead : {4096, 48000})
        {
            sched::SchedCfg cfg = lanesCfg(3, kFastPace);
            makePrefetch(cfg, 1, lead);
            makePrefetch(cfg, 2, lead);
            std::ostringstream os;
            os << "LS-2 lead=" << lead;
            out.push_back(Ls2Cell{lead, runScenario(os.str(), cfg, [](SchedRig& rig, Run& run) {
                                      rig.seek(sched::kNow, alignedSec(2.0));
                                      rig.runFor(sec(1.0));
                                      markNow(rig, run, "stop");
                                      rig.stop();
                                      rig.runCycles(3);
                                      markNow(rig, run, "start");
                                      rig.startFrom(alignedSec(20.0));
                                      rig.runFor(sec(2.0));
                                  })});
        }
        return out;
    }();
    return cells;
}

// ===========================================================================
// LS-3 live ⇄ 预取切换
// ===========================================================================
//
// lane 0 live、lane 2 预取提前 4096(对照);lane 1 是被切换的那条,提前量 8192。先连放 11.5 s(> R,
// 环里每个槽都写过「上一圈」的有声数据 —— 读方若错读旧槽,判据看得出来是 Wrong 而不只是 Absent),
// 然后每 1 s 切一次。
//   · 平滑组(A):Rewind → live、Drain → 预取、Drain → live、Drain → 预取、Rewind → live。
//     Rewind:超前的游标退回 Output 游标(Input 看到时间线回跳、换代,重写一遍已写过的位置);
//     Drain:游标不动(时间线连续,不换代)。
//   · 跳跃组(B):Jump → 预取(游标直接跳到 Output + 8192:宿主跳过了这一轨 [C_O, C_O+8192) 的渲染,
//     总线上这一段本来就是静音 —— 这是声明过的过渡窗,不计丢失;但读方若把这段的旧环槽当新数据读出来,
//     那是错音)、Jump → live、Jump → 预取。
struct Ls3Run
{
    Run ideal;
    Run tested;
    std::vector<std::int64_t> switches; // 相对游标
    std::vector<sched::Range> excused; // 宿主跳过渲染的过渡窗(相对游标)
};

Ls3Run runLs3(bool jumpGroup)
{
    Ls3Run out;
    const std::int64_t s0 = alignedSec(1.0);
    const std::int64_t preRoll = sec(11.5);
    struct Sw
    {
        bool live;
        sched::LiveSwitch mode;
    };
    const std::vector<Sw> plan = jumpGroup ? std::vector<Sw>{{false, sched::LiveSwitch::Jump},
                                                             {true, sched::LiveSwitch::Jump},
                                                             {false, sched::LiveSwitch::Jump}}
                                           : std::vector<Sw>{{true, sched::LiveSwitch::Rewind},
                                                             {false, sched::LiveSwitch::Drain},
                                                             {true, sched::LiveSwitch::Drain},
                                                             {false, sched::LiveSwitch::Drain},
                                                             {true, sched::LiveSwitch::Rewind}};
    const std::int64_t lead1 = 8192;
    for (const bool ideal : {true, false})
    {
        sched::SchedCfg cfg = lanesCfg(3, kFastPace);
        if (!ideal)
        {
            cfg.lanes[1].lead = lead1;
            cfg.lanes[1].live = jumpGroup; // 平滑组从预取起步(第一切是 Rewind → live),跳跃组从 live 起步
            makePrefetch(cfg, 2, 4096);
        }
        std::vector<std::int64_t> switches;
        Run r = runScenario(ideal ? "LS-3 ideal" : (jumpGroup ? "LS-3 jump" : "LS-3 smooth"), cfg,
                            [&](SchedRig& rig, Run& run) {
                                rig.seek(sched::kNow, s0);
                                rig.runFor(preRoll);
                                for (const Sw& sw : plan)
                                {
                                    switches.push_back(rig.outCursor() - run.c0);
                                    if (!ideal)
                                    {
                                        rig.setLive(1, sw.live, sw.mode);
                                    }
                                    rig.runFor(sec(1.0));
                                }
                            });
        if (ideal)
        {
            out.ideal = std::move(r);
        }
        else
        {
            out.tested = std::move(r);
            out.switches = switches;
        }
    }
    if (jumpGroup)
    {
        // Jump → 预取那两切:宿主跳过了 [切换点, 切换点 + lead) 的渲染(再加一帧对齐余量)。
        for (std::size_t i = 0; i < out.switches.size(); ++i)
        {
            if (!plan[i].live)
            {
                out.excused.push_back(sched::Range{out.switches[i], out.switches[i] + lead1 + kF});
            }
        }
    }
    return out;
}

const Ls3Run& ls3Smooth()
{
    static const Ls3Run r = runLs3(false);
    return r;
}

const Ls3Run& ls3Jump()
{
    static const Ls3Run r = runLs3(true);
    return r;
}

// ===========================================================================
// LS-4 Cycle + 混合提前量(H1)
// ===========================================================================
//
// lane 0 live(对照);lane 1、2 预取(块长 1024)提前 Δ。Cycle [B, B+len),B 帧对齐。预取轨比 Output
// 早 Δ 跨过循环点 ⇒ 写方先换代;读方此时还在上一圈的尾段 [E−Δ, E),把本代有效起点锚在自己的 t0 ≈ E−Δ
// (ShmRingMixSource.cpp:98-106),回到 B 之后 t0 < validFrom_,整圈读不到;又因 primed_ 在换代时清零,
// 失败一律不计失准(:125-137)。
struct Ls4Cell
{
    std::int64_t lead = 0;
    std::int64_t loopLen = 0;
    int laps = 0;
    Run run;
};

struct Ls4Family
{
    std::vector<Run> ideal; // 每个圈长一台(全 live)
    std::vector<Ls4Cell> cells;
};

constexpr std::int64_t kLs4LoopB = 141 * kF; // ≈ 3.008 s,帧对齐

int ls4Laps(std::int64_t loopLen)
{
    return loopLen <= sec(2.0) ? 4 : 3;
}

Run runLoop(const std::string& name, const sched::SchedCfg& cfg, std::int64_t loopB, std::int64_t loopLen, int laps)
{
    return runScenario(name, cfg, [loopB, loopLen, laps](SchedRig& rig, Run&) {
        rig.seek(sched::kNow, loopB);
        rig.cycle(loopB, loopB + loopLen);
        rig.runFor(static_cast<std::int64_t>(laps) * loopLen);
    });
}

const Ls4Family& ls4()
{
    static const Ls4Family fam = [] {
        Ls4Family f;
        const std::vector<std::int64_t> lens = {sec(2.0), sec(8.0), sec(12.0)};
        for (const std::int64_t len : lens)
        {
            std::ostringstream os;
            os << "LS-4 ideal loop=" << toSec(len) << "s";
            f.ideal.push_back(runLoop(os.str(), lanesCfg(3, kFastPace), kLs4LoopB, len, ls4Laps(len)));
        }
        for (const std::int64_t lead : {1024, 4096, 48000})
        {
            for (const std::int64_t len : lens)
            {
                sched::SchedCfg cfg = lanesCfg(3, kFastPace);
                makePrefetch(cfg, 1, lead);
                makePrefetch(cfg, 2, lead);
                std::ostringstream os;
                os << "LS-4 lead=" << lead << " loop=" << toSec(len) << "s";
                f.cells.push_back(
                    Ls4Cell{lead, len, ls4Laps(len), runLoop(os.str(), cfg, kLs4LoopB, len, ls4Laps(len))});
            }
        }
        return f;
    }();
    return fam;
}

const Run& ls4IdealFor(std::int64_t loopLen)
{
    const Ls4Family& f = ls4();
    const std::size_t i = loopLen <= sec(2.0) ? 0u : (loopLen <= sec(8.0) ? 1u : 2u);
    return f.ideal[i];
}

// 一圈 = Output 两次回绕之间(第 0 圈从脚本起点到第一次回绕)。
std::vector<sched::Range> lapsOf(const Run& r)
{
    std::vector<sched::Range> laps;
    std::int64_t begin = 0;
    for (const std::int64_t w : outputBackJumps(r))
    {
        laps.push_back(sched::Range{begin, w});
        begin = w;
    }
    laps.push_back(sched::Range{begin, r.c1 - r.c0});
    return laps;
}

// ===========================================================================
// LS-5 Cycle,各轨同一提前量(对照)
// ===========================================================================
//
// 「同一提前量」= Input 与 Output 以同一提前量处理 ⇒ 相对提前量 Δ = 0。本台的提前量都是相对 Output
// 的,所以就是全 live;另加一格「全部预取、提前 0、预取块 1024 > Output 块 256」:块粒度不同,但相对
// 提前量仍是 0(预取轨的块在循环点处切开,跨循环点的那一块与 Output 同周期处理)。
struct Ls5Cell
{
    Run run;
};

const std::vector<Ls5Cell>& ls5Cells()
{
    static const std::vector<Ls5Cell> cells = [] {
        std::vector<Ls5Cell> out;
        out.push_back(Ls5Cell{runLoop("LS-5 all-live loop=2s", lanesCfg(3, kFastPace), kLs4LoopB, sec(2.0), 4)});
        out.push_back(Ls5Cell{runLoop("LS-5 all-live loop=8s", lanesCfg(3, kFastPace), kLs4LoopB, sec(8.0), 3)});
        sched::SchedCfg cfg = lanesCfg(3, kFastPace);
        cfg.outBlock = 256;
        for (int lane = 0; lane < 3; ++lane)
        {
            makePrefetch(cfg, lane, 0);
        }
        out.push_back(
            Ls5Cell{runLoop("LS-5 all-prefetch lead=0 bin=1024 bout=256 loop=2s", cfg, kLs4LoopB, sec(2.0), 4)});
        return out;
    }();
    return cells;
}

// ===========================================================================
// LS-6 / LS-7 播放中定位(回跳 / 前跳),k 个在途旧块,写方领先
// ===========================================================================
//
// 从 12 s 放 2 s,在 14 s 处定位:回跳到 11 s(LS-6)或前跳到 20 s(LS-7)。Output 先放完 k 个在途旧块
// 再换位置;预取轨在定位那一周期就被冲刷回生效点、在新位置上重新渲染(先换代)。
//   回跳 + k ≥ 1:读方在旧位置上看到换代,锚在旧位置(≈14 s);新位置 11 s < 锚点 ⇒ 读方重新走过 14 s
//   之前一直读不到(设计稿 LS-6「读方越过原位置前一直静音」)。k = 0 时读写同块跳变,不丢。
struct SeekCell
{
    std::int64_t lead = 0;
    int stale = 0;
    Run run;
};

struct SeekFamily
{
    std::vector<Run> ideal; // 每个 k 一台(全 live,同一份脚本)
    std::vector<SeekCell> cells;
    std::int64_t from = 0;
    std::int64_t to = 0;
};

constexpr int kStaleKs[] = {0, 1, 3};

SeekFamily runSeekFamily(bool back)
{
    SeekFamily fam;
    const std::int64_t s0 = alignedSec(12.0);
    fam.from = s0 + alignedSec(2.0);
    fam.to = back ? alignedSec(11.0) : alignedSec(20.0);
    const std::int64_t to = fam.to;
    const auto script = [s0, to](int k) {
        return [s0, to, k](SchedRig& rig, Run& run) {
            rig.seek(sched::kNow, s0);
            rig.runFor(alignedSec(2.0));
            markNow(rig, run, "seek");
            rig.seek(sched::kNow, to, k);
            rig.runFor(sec(4.0));
        };
    };
    for (const int k : kStaleKs)
    {
        std::ostringstream os;
        os << (back ? "LS-6" : "LS-7") << " ideal k=" << k;
        fam.ideal.push_back(runScenario(os.str(), lanesCfg(3, kFastPace), script(k)));
    }
    for (const std::int64_t lead : {4096, 48000})
    {
        for (const int k : kStaleKs)
        {
            sched::SchedCfg cfg = lanesCfg(3, kFastPace);
            makePrefetch(cfg, 1, lead);
            makePrefetch(cfg, 2, lead);
            std::ostringstream os;
            os << (back ? "LS-6" : "LS-7") << " lead=" << lead << " k=" << k;
            fam.cells.push_back(SeekCell{lead, k, runScenario(os.str(), cfg, script(k))});
        }
    }
    return fam;
}

const SeekFamily& ls6()
{
    static const SeekFamily f = runSeekFamily(true);
    return f;
}

const SeekFamily& ls7()
{
    static const SeekFamily f = runSeekFamily(false);
    return f;
}

const Run& seekIdealFor(const SeekFamily& f, int k)
{
    for (std::size_t i = 0; i < std::size(kStaleKs); ++i)
    {
        if (kStaleKs[i] == k)
        {
            return f.ideal[i];
        }
    }
    return f.ideal.front();
}

// ===========================================================================
// LS-8 起播怪癖(H1′)
// ===========================================================================
//
// 放 1 s → 停走带 3 块 → 压入怪癖 → 从 0 起播放 3 s。lane 0、1 live,lane 2 预取提前 4096。
//   garbage(540544, k):起播头 k 块报一个离谱的大位置(Logic 论坛 66945);
//   offByOne(−179, 1868):头两块报 −179、1868(两者之差不等于块长)。
// Output 拿到垃圾值 ⇒ 读方在那一块看到换代、把本代有效起点锚在 540544(≈11.26 s),之后的真实位置
// 全在锚点之前 ⇒ 这一遍所有轨读不到,且不计失准(从没 primed 过)。
struct Ls8Cell
{
    std::string label;
    SchedRig::Target target = SchedRig::Target::Output;
    sched::Quirk quirk;
    Run run;
};

struct Ls8Family
{
    Run ideal;
    std::vector<Ls8Cell> cells;
};

sched::SchedCfg ls8Cfg()
{
    sched::SchedCfg cfg = lanesCfg(3, kFastPace);
    makePrefetch(cfg, 2, 4096);
    return cfg;
}

Run runLs8(const std::string& name, bool withQuirk, SchedRig::Target target, const sched::Quirk& q)
{
    return runScenario(name, ls8Cfg(), [withQuirk, target, q](SchedRig& rig, Run& run) {
        rig.seek(sched::kNow, alignedSec(2.0));
        rig.runFor(sec(1.0));
        rig.stop();
        rig.runCycles(3);
        if (withQuirk)
        {
            rig.quirk(target, q);
        }
        markNow(rig, run, "start");
        rig.startFrom(0);
        rig.runFor(sec(3.0));
    });
}

const Ls8Family& ls8()
{
    static const Ls8Family fam = [] {
        Ls8Family f;
        f.ideal = runLs8("LS-8 ideal", false, SchedRig::Target::Output, sched::Quirk{});
        const sched::Quirk g1 = sched::Quirk::garbage(540544, 1);
        const sched::Quirk g2 = sched::Quirk::garbage(540544, 2);
        const sched::Quirk ob = sched::Quirk::offByOne(-179, 1868);
        const std::vector<std::pair<std::string, std::pair<SchedRig::Target, sched::Quirk>>> plan = {
            {"output garbage x1", {SchedRig::Target::Output, g1}},
            {"output garbage x2", {SchedRig::Target::Output, g2}},
            {"inputs garbage x1", {SchedRig::Target::AllInputs, g1}},
            {"all garbage x1", {SchedRig::Target::All, g1}},
            {"output -179/1868", {SchedRig::Target::Output, ob}},
            {"inputs -179/1868", {SchedRig::Target::AllInputs, ob}},
            {"all -179/1868", {SchedRig::Target::All, ob}},
        };
        for (const auto& p : plan)
        {
            Ls8Cell c;
            c.label = p.first;
            c.target = p.second.first;
            c.quirk = p.second.second;
            c.run = runLs8("LS-8 " + p.first, true, c.target, c.quirk);
            f.cells.push_back(std::move(c));
        }
        return f;
    }();
    return fam;
}

// ===========================================================================
// LS-9 预取块 1024 > prepare 512(SL-523),Output 块 32
// ===========================================================================
const Run& ls9()
{
    static const Run r = [] {
        sched::SchedCfg cfg = lanesCfg(3, kFastPace);
        cfg.outBlock = 32;
        for (int lane : {1, 2})
        {
            makePrefetch(cfg, lane, 4096, 1024);
            cfg.lanes[static_cast<std::size_t>(lane)].prepareBlock = 512;
        }
        return runScenario("LS-9 bin=1024 prepare=512 bout=32", cfg, [](SchedRig& rig, Run&) {
            rig.seek(sched::kNow, alignedSec(2.0));
            rig.runFor(sec(1.0));
        });
    }();
    return r;
}

// ===========================================================================
// LS-10 离线导出对照
// ===========================================================================
//
// 全 live(离线导出是同步调度)、setNonRealtime(true)、块长 1024 / 2048 / 4096,与实时理想调度
// (块长 1024)逐帧对比。离线格按 pace=10 跑(离线宿主不按墙钟走;这里只要求它比实时快)。
struct Ls10Cell
{
    int block = 0;
    Run run;
};

struct Ls10Family
{
    Run realtime;
    std::vector<Ls10Cell> cells;
};

Run runLs10(const std::string& name, int block, bool offline)
{
    sched::SchedCfg cfg = lanesCfg(3, offline ? 10.0 : kFastPace);
    cfg.outBlock = block;
    cfg.offline = offline;
    return runScenario(name, cfg, [](SchedRig& rig, Run&) {
        rig.seek(sched::kNow, alignedSec(2.0));
        rig.runFor(sec(5.0));
    });
}

const Ls10Family& ls10()
{
    static const Ls10Family fam = [] {
        Ls10Family f;
        f.realtime = runLs10("LS-10 realtime b=1024", 1024, false);
        for (const int b : {1024, 2048, 4096})
        {
            std::ostringstream os;
            os << "LS-10 offline b=" << b;
            f.cells.push_back(Ls10Cell{b, runLs10(os.str(), b, true)});
        }
        return f;
    }();
    return fam;
}

// 按时间线位置逐帧对比两次运行(块长不同,游标块格不同,但时间线位置一一对应)。
int verdictMismatchesByT(const Run& a, const Run& b, int lane)
{
    std::map<std::int64_t, Verdict> at;
    for (const sched::FrameVerdict& f : a.out.frames)
    {
        if (f.lane == lane)
        {
            at[f.t] = f.verdict;
        }
    }
    int mism = 0;
    int matched = 0;
    for (const sched::FrameVerdict& f : b.out.frames)
    {
        if (f.lane != lane)
        {
            continue;
        }
        const auto it = at.find(f.t);
        if (it == at.end())
        {
            continue;
        }
        ++matched;
        mism += it->second != f.verdict ? 1 : 0;
    }
    return matched > 0 ? mism : -1;
}

// ===========================================================================
// LS-11a Output 被停调、Input 仍在写(H5)
// ===========================================================================
//
// 3 条 live,pace=1。放 1 s → 宿主停调 Output 7 s(Live 停用设备 / FL smart disable 一类,总线直通)→
// 恢复调用 2.5 s。停调期间 Output 的录音就是总线输入(直通):Input 若切回直通,这里听得到原声(Raw)。
// KI-6 文档写「约 5.5 s 后转未平衡原声」:看门狗 0.5 s 判停摆清 connected_mask,Input 再按 5 s 滞回
// 切直通。H5 推演:看门狗只清一拍,下一拍 evaluateChannels 又把位置回(OutputSession.cpp:528-539 /
// CtrlPlane.cpp:574-590),Input 永远等不满 5 s ⇒ 停调期间一直无声。
constexpr double kLs11SuspendS = 7.0;

const Run& ls11a()
{
    static const Run r = [] {
        return runScenario("LS-11a", lanesCfg(3, 1.0), [](SchedRig& rig, Run& run) {
            sampleInputs(rig, run, 2 * 1024, sec(1.0 + kLs11SuspendS + 2.5));
            rig.runFor(sec(1.0));
            markNow(rig, run, "suspend");
            rig.suspendOutput(true);
            rig.runFor(sec(kLs11SuspendS));
            markNow(rig, run, "resume");
            rig.suspendOutput(false);
            rig.runFor(sec(2.5));
        });
    }();
    return r;
}

// ===========================================================================
// LS-11b 总线静音即停调 Output / 整链停调(只表征)
// ===========================================================================
//
// (A) outputSilenceTail(0.5 s):总线输入连续静音 0.5 s 就不调 Output、总线直通(Logic / FL 式)。
//     各 Input 交给 Output 之后自己是静音的 ⇒ 总线必然静音 ⇒ Output 必然被停调(H2,硬约束 3 的直接后果)。
// (B) chainSuspend(0.5 s, by=In):全部轨源信号静音 0.5 s 后整条链(Input + Output)都不调用,宿主直通。
//     脚本:2 s 处起放,[3 s, 6 s) 全部轨静音,6 s 后恢复。
// 只打印周期、无声时长、恢复后的句首,只断言「场景确实跑到」。
const Run& ls11bTail()
{
    static const Run r = [] {
        return runScenario("LS-11b output-silence-tail", lanesCfg(3, 1.0), [](SchedRig& rig, Run& run) {
            sampleInputs(rig, run, 2 * 1024, sec(9.0));
            rig.runFor(sec(0.5));
            markNow(rig, run, "on");
            rig.outputSilenceTail(sec(0.5));
            rig.runFor(sec(8.0));
            rig.outputSilenceTail(-1);
        });
    }();
    return r;
}

const Run& ls11bChain()
{
    static const Run r = [] {
        return runScenario("LS-11b chain-suspend", lanesCfg(3, 1.0), [](SchedRig& rig, Run& run) {
            const std::int64_t s0 = alignedSec(2.0);
            const std::int64_t g0 = alignedSec(3.0);
            const std::int64_t g1 = alignedSec(6.0);
            for (int lane = 0; lane < rig.laneCount(); ++lane)
            {
                rig.silentRegion(lane, g0, g1);
            }
            rig.chainSuspend(sec(0.5), sched::ChainBy::In);
            sampleInputs(rig, run, 2 * 1024, (g1 - s0) + sec(2.0));
            rig.seek(sched::kNow, s0);
            run.mark["g0"] = g0 - s0;
            run.mark["g1"] = g1 - s0;
            rig.runFor((g1 - s0) + sec(2.0));
        });
    }();
    return r;
}

// ===========================================================================
// LS-12 Input 无 region(E2-a / E2-b / H4)
// ===========================================================================
//
// lane 0 live(对照);lane 2 预取提前 8192(对照);lane 1 是被测轨:同步格 live、领先格预取提前 8192,
// 时间线 [g0, g1) 内宿主不调用它(无 region)。g0 之前从 1 s 连放 11.5 s(> R + 8192:间隙末尾那 8192
// 个环槽里写着上一圈的有声数据 —— 读方若把它们当新数据读出来,判 Wrong)。
//   实时格:间隙前 0.6 s 起按 pace=1 跑(写头冻结 500 ms、[M] 一拍、5 s 滞回都是墙钟量)。
//   离线格:setNonRealtime(true),全程 pace=5(离线时间线跑在墙钟前面;6 s 间隙 ≈ 1.2 s 墙钟,稳稳跨过
//   500 ms 的停滞门限,1 s 间隙 ≈ 0.2 s 墙钟,跨不过。pace=10 时 6 s 间隙只有约 0.6 s 墙钟,贴着门限 ——
//   格的含义不能随机器快慢翻转)。
//   领先的离线格恢复时是错读(H4)还是缺席(E2-b),取决于把该轨重新放回注入集的那一拍 [M] 落在读方
//   走到 g1 之前还是之后(写方领先 8192 ≈ 34 ms 墙钟):两种结果都是缺陷,H4 的判据只看错音,确定性靠
//   实时领先格(领先 ≈ 170 ms 墙钟,远大于一拍 40 ms,恢复那一拍必然落在读方到 g1 之前)。
//   另有一格单轨(solo):间隙里 Output 一条注入轨都没有,总线原样直通 —— 只有这时 Input 切回直通的
//   原声听得见(多轨时总线被其余轨的混音整体替换,这一轨的原声被吃掉,表现为缺席)。
// 判据只看被测轨间隙之后(以及间隙尾段)的帧:第一帧就该是 Mix 在场,不该有原声,不该有错音。
struct Ls12Cell
{
    double gapS = 0.0;
    bool lead = false;
    bool offline = false;
    bool solo = false; // 只有被测这一条轨(其余轨都不存在):Output 在间隙里一条注入轨都没有
    int testLane = 1;
    std::int64_t g0Rel = 0;
    std::int64_t g1Rel = 0;
    Run run;
};

constexpr std::int64_t kLs12Lead = 8192;

Ls12Cell runLs12(double gapS, bool lead, bool offline, bool solo = false)
{
    Ls12Cell c;
    c.gapS = gapS;
    c.lead = lead;
    c.offline = offline;
    c.solo = solo;
    c.testLane = solo ? 0 : 1;
    sched::SchedCfg cfg = lanesCfg(solo ? 1 : 3, offline ? 5.0 : kFastPace);
    cfg.offline = offline;
    if (!solo)
    {
        if (lead)
        {
            makePrefetch(cfg, 1, kLs12Lead);
        }
        makePrefetch(cfg, 2, kLs12Lead);
    }
    const int testLane = c.testLane;
    const std::int64_t s0 = alignedSec(1.0);
    const std::int64_t g0 = s0 + alignedSec(11.5);
    const std::int64_t g1 = g0 + alignedSec(gapS);
    c.g0Rel = g0 - s0;
    c.g1Rel = g1 - s0;
    std::ostringstream os;
    os << "LS-12 gap=" << gapS << "s " << (lead ? "lead=8192" : "sync") << " " << (offline ? "offline" : "realtime")
       << (solo ? " solo" : "");
    c.run = runScenario(os.str(), cfg, [=](SchedRig& rig, Run& run) {
        rig.noRegion(testLane, g0, g1);
        rig.seek(sched::kNow, s0);
        markAt(rig, run, "g0", g0 - s0);
        markAt(rig, run, "g1", g1 - s0);
        if (offline)
        {
            rig.runFor((g1 - s0) + sec(2.5));
            return;
        }
        rig.runFor((g0 - s0) - sec(0.6));
        rig.setPace(1.0);
        rig.runFor(sec(0.6) + (g1 - g0) + sec(2.5));
    });
    return c;
}

const std::vector<Ls12Cell>& ls12Cells()
{
    static const std::vector<Ls12Cell> cells = [] {
        std::vector<Ls12Cell> out;
        for (const bool offline : {false, true})
        {
            for (const bool lead : {false, true})
            {
                for (const double gap : {1.0, 6.0})
                {
                    out.push_back(runLs12(gap, lead, offline));
                }
            }
        }
        // 单轨:间隙里 Output 一条注入轨都没有 ⇒ 它把总线原样直通,Input 若已切回直通,句首的原声听得见
        // (多轨时总线被其余轨的混音整体替换,这一轨的原声被吃掉,表现为缺席 —— 见上面 3 轨的格)。
        out.push_back(runLs12(6.0, false, false, true));
        return out;
    }();
    return cells;
}

struct Ls12Metrics
{
    std::int64_t headRel = -1; // 间隙后第一帧 Mix 在场的相对游标
    int headLostFrames = -1; // g1 起到第一帧 Mix 在场之间的帧数(-1 = 2.5 s 内都没有)
    int rawAfter = 0; // [g1, g1 + 2.5 s) 里来源是原声的帧
    int wrongNear = 0; // [g1 − 0.5 s, g1 + 2.5 s) 里的 Wrong 帧
    int lostAfter = 0; // [g1, g1 + 2.5 s) 里不是 Present 的帧
    Tally after;
};

Ls12Metrics ls12Metrics(const Ls12Cell& c)
{
    Ls12Metrics m;
    const std::int64_t end = c.g1Rel + sec(2.5);
    m.headRel = firstFrame(c.run, c.testLane, c.g1Rel, end, isMix);
    m.headLostFrames = m.headRel < 0 ? -1 : static_cast<int>((m.headRel - c.g1Rel) / kF);
    m.after = tally(c.run, c.testLane, c.g1Rel, end);
    m.rawAfter = m.after.rawAny;
    m.lostAfter = m.after.total - m.after.present;
    m.wrongNear = tally(c.run, c.testLane, c.g1Rel - sec(0.5), end).wrong;
    return m;
}

std::string str(const Ls12Cell& c, const Ls12Metrics& m)
{
    std::ostringstream os;
    os << c.run.name << ": headLost=" << m.headLostFrames << " frames ("
       << (m.headLostFrames < 0 ? -1.0 : 1000.0 * toSec(m.headLostFrames * kF)) << " ms) rawAfter=" << m.rawAfter
       << " wrongNear=" << m.wrongNear << " lostAfter=" << m.lostAfter << " after[" << str(m.after) << "] gapWall="
       << (c.run.wall.count("g1") != 0 && c.run.wall.count("g0") != 0 ? c.run.wall.at("g1") - c.run.wall.at("g0")
                                                                      : -1.0)
       << " ms";
    return os.str();
}

// ===========================================================================
// LS-13 对抗安全
// ===========================================================================
//
// 都先连放 11.5 s(> R),让每个环槽里都有「上一圈」的有声数据,错读才判得出 Wrong。
//   (a) near-ring:写方领先 Δ,定位跨度 ≈ R − Δ(回跳 R−Δ、R−Δ/2,前跳 R−Δ),k = Δ/1024 个在途旧块
//       (写方比读方早 Δ 到达定位点,与 A-3 的模型同形)。A-3 读方层面实测安全。
//   (b) two-seeks-across-ring:Δ = 4096、k = 4:第一次往前定位到 X + R − Δ + m,两块后又定位回来
//       (X + 2048 撤销式,或 X − 1024);读方在旧位置上看到第二次换代、锚在自己的 t0,前方 m 帧的环槽
//       已被换成下一圈的数据。A-3 读方层面实测出错。m ∈ {Δ/4, Δ/2}(见 ls13() 里的注释)。
//   (c) mismatch:全 live,定位到从没放过的 20 s,Output 那一块报 20 s + ε(读写目标不一致)。
//       ε < 0:[S+ε, S) 本代没写过,读方锚在 S+ε、写头已覆盖 ⇒ 交出旧环槽(A-3 实测出错);ε > 0 不出错。
//       判据见 mismatchSafe。另两格写方领先 4096:本台定位时预取轨被冲刷回定位点、与读方同周期跳变,
//       A-3 读方层面「领先时读方锚在旧位置、反而挡住了」那一支在这里不成立(那一支要 k ≥ 1 个在途旧块)。
//   (d) geometry:Cycle 12 s(> R)第二圈开头把 lane 1 mono → stereo 重新 prepare,0.5 s 后改回 mono。
//       Output 按 25 Hz 比对段头几何换绑(OutputSession.cpp:180-205),换绑之前读方拿旧快照的声道步长
//       解新布局的数据。圈长必须 > R 且改布局要落在第二圈前 R 之内:短圈里旧快照按 mono 步长读到的
//       恰好是**上一圈同一位置**的 mono 数据(循环每圈写的是同一批位置),内容碰巧是对的,判不出来
//       (实测 2 s 圈 0 错帧);圈长 > R 时那个环槽里最后写的是本圈后段 p + R 的数据,错读即反相。
struct Ls13Cell
{
    std::string label;
    bool expectWrongNow = false; // A-3 读方层面已实测出错的形状
    Run run;
};

Run runNearRing(std::int64_t lead, std::int64_t span, bool back)
{
    sched::SchedCfg cfg = lanesCfg(3, kFastPace);
    makePrefetch(cfg, 1, lead);
    makePrefetch(cfg, 2, lead);
    const int k = static_cast<int>(lead / 1024);
    std::ostringstream os;
    os << "LS-13 near-ring lead=" << lead << " " << (back ? "back " : "fwd ") << span;
    return runScenario(os.str(), cfg, [span, back, k](SchedRig& rig, Run& run) {
        rig.seek(sched::kNow, alignedSec(1.0));
        rig.runFor(sec(11.5));
        const std::int64_t x = rig.map().at(rig.outCursor()).t + static_cast<std::int64_t>(k) * 1024;
        markNow(rig, run, "seek");
        rig.seek(sched::kNow, back ? x - span : x + span, k);
        rig.runFor(sec(1.5));
    });
}

Run runTwoSeeks(std::int64_t lead, std::int64_t m, bool undo)
{
    sched::SchedCfg cfg = lanesCfg(3, kFastPace);
    makePrefetch(cfg, 1, lead);
    makePrefetch(cfg, 2, lead);
    const int k = static_cast<int>(lead / 1024);
    std::ostringstream os;
    os << "LS-13 two-seeks-across-ring lead=" << lead << " m=" << m
       << (undo ? " back-to=X+lead/2" : " back-to=X-lead/4");
    return runScenario(os.str(), cfg, [lead, m, undo, k](SchedRig& rig, Run& run) {
        rig.seek(sched::kNow, alignedSec(1.0));
        rig.runFor(sec(11.5));
        const std::int64_t a = rig.outCursor();
        const std::int64_t x = rig.map().at(a).t + static_cast<std::int64_t>(k) * 1024; // 写方在定位生效点上的位置
        markNow(rig, run, "seek1");
        run.val["s1"] = x + kRing - lead + m;
        run.val["s2"] = undo ? x + lead / 2 : x - lead / 4;
        rig.seek(sched::kNow, run.val["s1"], k);
        const int h = k / 2; // 写方 Δ/2 之后再定位一次
        rig.runCycles(h);
        markNow(rig, run, "seek2");
        rig.seek(sched::kNow, run.val["s2"], k);
        rig.runFor(sec(1.5));
    });
}

// 读写目标不一致的判据:Output 报 S+ε 的那一块,[S+ε, S+ε+n) 不全是本代写过的位置(ε < 0 时
// [S+ε, S) 本代没写,ε > 0 时 [S+n, S+ε+n) 写方还没写到)。read() 整块要么交出要么不交 —— I1 安全的
// 读方在这一块只能交出空(Absent)。交出东西(哪怕按总线时间判成 Present)就是把不属于该位置的环槽
// 当新数据读了出来。其余帧照常不许 Wrong。
bool mismatchSafe(const Run& r, int lane)
{
    const sched::FrameVerdict* f = frameAt(r, lane, r.mark.at("seek"));
    return f != nullptr && f->verdict == Verdict::Absent && tally(r, lane).wrong == 0;
}

Run runMismatch(std::int64_t eps, std::int64_t lead)
{
    sched::SchedCfg cfg = lanesCfg(3, kFastPace);
    if (lead > 0)
    {
        makePrefetch(cfg, 1, lead);
        makePrefetch(cfg, 2, lead);
    }
    std::ostringstream os;
    os << "LS-13 mismatch lead=" << lead << " eps=" << eps;
    return runScenario(os.str(), cfg, [eps](SchedRig& rig, Run& run) {
        rig.seek(sched::kNow, alignedSec(1.0));
        rig.runFor(sec(11.5));
        const std::int64_t s = alignedSec(20.0);
        sched::Quirk q;
        q.reported = {s + eps};
        markNow(rig, run, "seek");
        rig.seek(sched::kNow, s);
        rig.quirk(SchedRig::Target::Output, q);
        rig.runFor(sec(1.5));
    });
}

// 不核幅度:同一轨 mono 与 stereo 两种布局在 SCVB 混音里的增益本来就不同(单声道等功率声像 vs 双声道
// 声像),仪器按段取幅度中位数做参考(presence_meter.h「不覆盖的」第三条),一段里两种布局各占一部分时
// 少数那部分会被判成幅度不符 —— 那不是错读。本格要抓的是错读(相关 < 0.5 判 Wrong),与幅度无关。
presence::Thresholds geometryThresholds()
{
    presence::Thresholds th;
    th.ampTol = -1.0;
    return th;
}

constexpr std::int64_t kGeoLoopLen = 576000; // 12 s

Run runGeometry()
{
    sched::SchedCfg cfg = lanesCfg(3, kFastPace);
    return runScenario(
        "LS-13 geometry mono<->stereo mid-cycle", cfg,
        [](SchedRig& rig, Run& run) {
            rig.seek(sched::kNow, kLs4LoopB);
            rig.cycle(kLs4LoopB, kLs4LoopB + kGeoLoopLen);
            rig.runFor(kGeoLoopLen + sec(0.1));
            rig.setPace(1.0); // 25 Hz 换绑是墙钟量:旧快照窗口按实时节拍才是真实宽度
            rig.runFor(sec(0.2));
            markNow(rig, run, "toStereo");
            run.val["tStereo"] = rig.map().at(rig.outCursor()).t;
            rig.geometryRewrite(1, true);
            rig.runFor(sec(0.5));
            markNow(rig, run, "toMono");
            rig.geometryRewrite(1, false);
            rig.runFor(sec(1.0));
        },
        geometryThresholds());
}

struct Ls13Family
{
    std::vector<Ls13Cell> nearRing;
    std::vector<Ls13Cell> twoSeeks;
    std::vector<Ls13Cell> mismatch;
    Ls13Cell geometry;
};

const Ls13Family& ls13()
{
    static const Ls13Family fam = [] {
        Ls13Family f;
        for (const std::int64_t lead : {4096, 48000})
        {
            f.nearRing.push_back(Ls13Cell{"near-ring back R-lead", false, runNearRing(lead, kRing - lead, true)});
            f.nearRing.push_back(Ls13Cell{"near-ring back R-lead/2", false, runNearRing(lead, kRing - lead / 2, true)});
            f.nearRing.push_back(Ls13Cell{"near-ring fwd R-lead", false, runNearRing(lead, kRing - lead, false)});
        }
        // m = Δ/4、Δ/2:读方在旧位置上读到的那几块整块都换成了下一圈的数据(反相,判 Wrong)。m = Δ/8 时
        // 被换掉的只有半块,那一帧判成 Partial —— 仪器按帧判定的粒度够不着,不放进来(PR 里如实写)。
        for (const std::int64_t m : {1024, 2048})
        {
            for (const bool undo : {true, false})
            {
                f.twoSeeks.push_back(Ls13Cell{"two-seeks", true, runTwoSeeks(4096, m, undo)});
            }
        }
        for (const std::int64_t eps : {-4096, -1868, -179, 179, 1868})
        {
            f.mismatch.push_back(Ls13Cell{"mismatch sync", eps < 0, runMismatch(eps, 0)});
        }
        // 写方领先 4096:定位冲刷让预取轨与读方在同一周期跳变(本台没有在途旧块),领先挡不住 ——
        // A-3 读方层面「领先时读方锚在旧位置、反而挡住了」那一支对应的是 k >= 1 个在途旧块。
        for (const std::int64_t eps : {-4096, -1868})
        {
            f.mismatch.push_back(Ls13Cell{"mismatch lead", true, runMismatch(eps, 4096)});
        }
        f.geometry = Ls13Cell{"geometry", true, runGeometry()};
        return f;
    }();
    return fam;
}

int wrongAll(const Run& r)
{
    int w = 0;
    for (int lane = 0; lane < r.lanes; ++lane)
    {
        w += tally(r, lane).wrong;
    }
    return w;
}

std::string wrongByLane(const Run& r)
{
    std::ostringstream os;
    for (int lane = 0; lane < r.lanes; ++lane)
    {
        os << " l" << lane << "[" << str(tally(r, lane)) << "]";
    }
    return os.str();
}

// ===========================================================================
// LS-14 持续先算 Output / 读方偏移超前(H3)
// ===========================================================================
//
// 3 条 live,pace=1。条件在一次定位的同一周期开始 —— 定位让各轨换代、读方 primed_ 清零,于是条件
// 「从这一代一开始就成立」:读方永远领先写头、永远 primed 不了、失败一律不计失准(H3 的形状)。
// 条件持续 3 s 后撤掉,再放 2 s。
//   (a) outputFirst:Output 先于各 Input 处理(N2 的持续版);
//   (b) readerOffset 4096:Output 报给插件的位置比总线超前 4096(Aux 位置没做延迟补偿一类)。
// 另有 (c) 不经定位、播放中途才开始 outputFirst:读方此前已 primed,第二块起就计失准 —— 这是现行
// 实现「会报」的那一半,只表征(打印告警的通断),不判 H3。
struct Ls14Cell
{
    std::string label;
    bool atGenerationStart = true;
    Run run;
};

constexpr double kLs14CondS = 3.0;

Run runLs14(const std::string& label, sched::GraphViolationKind kind, bool withSeek)
{
    return runScenario("LS-14 " + label, lanesCfg(3, 1.0), [kind, withSeek](SchedRig& rig, Run& run) {
        rig.runFor(sec(0.5));
        markNow(rig, run, "on");
        if (withSeek)
        {
            rig.seek(sched::kNow, alignedSec(30.0));
        }
        rig.violateGraphOrder(
            sched::GraphViolation{kind, -1, kind == sched::GraphViolationKind::ReaderOffset ? 4096 : 0, true});
        rig.runFor(sec(kLs14CondS));
        markNow(rig, run, "off");
        rig.violateGraphOrder(sched::GraphViolation{});
        rig.runFor(sec(2.0));
    });
}

const std::vector<Ls14Cell>& ls14Cells()
{
    static const std::vector<Ls14Cell> cells = [] {
        std::vector<Ls14Cell> out;
        out.push_back(Ls14Cell{"outputFirst from a seek", true,
                               runLs14("outputFirst from a seek", sched::GraphViolationKind::OutputFirst, true)});
        out.push_back(
            Ls14Cell{"readerOffset=4096 from a seek", true,
                     runLs14("readerOffset=4096 from a seek", sched::GraphViolationKind::ReaderOffset, true)});
        out.push_back(Ls14Cell{"outputFirst mid-stream", false,
                               runLs14("outputFirst mid-stream", sched::GraphViolationKind::OutputFirst, false)});
        return out;
    }();
    return cells;
}

// 告警时序(按 [M] 拍的墙钟):条件开始后第一次 misalign>0 的延迟、告警亮起后条件持续期间熄灭的拍数、
// 条件撤掉后告警撤下的延迟。-1 = 没发生。
struct AlarmTiming
{
    double onsetMs = -1.0;
    int flickerBeats = 0;
    int beatsDuring = 0;
    double clearMs = -1.0;
};

AlarmTiming alarmTiming(const Run& r, int lane)
{
    AlarmTiming a;
    const double on = r.wall.at("on");
    const double off = r.wall.at("off");
    bool lit = false;
    for (const sched::MBeat& b : r.beats)
    {
        const bool alarm = b.lanes[static_cast<std::size_t>(lane)].misalign > 0;
        if (b.wallMs >= on && b.wallMs < off)
        {
            ++a.beatsDuring;
            if (alarm && !lit)
            {
                lit = true;
                a.onsetMs = a.onsetMs < 0.0 ? b.wallMs - on : a.onsetMs;
            }
            else if (!alarm && lit)
            {
                ++a.flickerBeats;
            }
        }
        else if (b.wallMs >= off && a.clearMs < 0.0 && !alarm)
        {
            a.clearMs = b.wallMs - off;
        }
    }
    return a;
}

std::string str(const AlarmTiming& a)
{
    std::ostringstream os;
    os << "onset " << a.onsetMs << " ms, flicker " << a.flickerBeats << "/" << a.beatsDuring << " beats, clear "
       << a.clearMs << " ms";
    return os.str();
}

// ===========================================================================
// LS-15 停走带 / 起播 + 写方领先(E2 的边界)
// ===========================================================================
//
// lane 0 live,lane 1、2 预取提前 8192;pace=1。放 1.3 s → 停走带 D(宿主照常调用各实例,报 isPlaying
// = false、位置冻结)→ 原地起播 → 放 2 s。D = 0.25 s(不到 500 ms 停滞门限)与 2 s(跨过门限:写头
// 冻结 ⇒ suspended ⇒ 该轨退出 connected_mask)。A-8 改「挂起 ≠ 掉线」只在走带**在跑**时生效,
// 停走带沿用 500 ms 规则 —— 这里钉住现行行为,A-8 之后必须不变。
struct Ls15Cell
{
    double stopS = 0.0;
    Run run;
};

const std::vector<Ls15Cell>& ls15Cells()
{
    static const std::vector<Ls15Cell> cells = [] {
        std::vector<Ls15Cell> out;
        for (const double d : {0.25, 2.0})
        {
            sched::SchedCfg cfg = lanesCfg(3, kFastPace);
            makePrefetch(cfg, 1, 8192);
            makePrefetch(cfg, 2, 8192);
            std::ostringstream os;
            os << "LS-15 stop=" << d << "s";
            out.push_back(Ls15Cell{d, runScenario(os.str(), cfg, [d](SchedRig& rig, Run& run) {
                                       rig.seek(sched::kNow, alignedSec(2.0));
                                       rig.runFor(sec(1.0));
                                       rig.setPace(1.0);
                                       sampleInputs(rig, run, 2 * 1024, rig.outCursor() - run.c0 + sec(0.3 + d + 2.0));
                                       rig.runFor(sec(0.3));
                                       markNow(rig, run, "stop");
                                       rig.stop();
                                       rig.runFor(sec(d));
                                       markNow(rig, run, "start");
                                       rig.start();
                                       rig.runFor(sec(2.0));
                                   })});
        }
        return out;
    }();
    return cells;
}

// Input 采样里,相对游标 [rel0, rel1) 内该轨 maskBit 为 0 的采样数 / 采样总数。
std::pair<int, int> maskOffSamples(const Run& r, int lane, std::int64_t rel0, std::int64_t rel1)
{
    int off = 0;
    int total = 0;
    for (const InputSample& s : r.inputs)
    {
        if (s.rel >= rel0 && s.rel < rel1)
        {
            ++total;
            off += s.maskBit[static_cast<std::size_t>(lane)] == 0 ? 1 : 0;
        }
    }
    return {off, total};
}

std::pair<int, int> passthroughSamples(const Run& r, int lane, std::int64_t rel0, std::int64_t rel1)
{
    int on = 0;
    int total = 0;
    for (const InputSample& s : r.inputs)
    {
        if (s.rel >= rel0 && s.rel < rel1)
        {
            ++total;
            on += s.passthrough[static_cast<std::size_t>(lane)] != 0 ? 1 : 0;
        }
    }
    return {on, total};
}

} // namespace

// ###########################################################################
// 用例
// ###########################################################################

TEST_CASE("SCHED scenarios scaffold: a lane read one ring lap stale is judged Wrong inside the host target",
          "[.][sched]")
{
    constexpr int kFrames = 16;
    const std::int64_t len = kFrames * static_cast<std::int64_t>(presence::kFrame);
    const std::int64_t t0 = 2 * presence::kRingR;

    std::vector<float> left(static_cast<std::size_t>(len), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(len), 0.0f);
    // lane 0 正常;lane 1 的内容来自时间线 t - R(环里上一圈留下的旧数据)。
    presence::addLane(0, t0, left.data(), len, 0.7);
    presence::addLane(0, t0, right.data(), len, 0.7);
    presence::addLane(1, t0 - presence::kRingR, left.data(), len, 0.7);
    presence::addLane(1, t0 - presence::kRingR, right.data(), len, 0.7);

    const presence::CombReport rep = presence::analyze(presence::Span{left.data(), right.data(), t0, len}, {0, 1});
    REQUIRE(rep.lanes.size() == 2u);
    REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));
    CHECK(rep.lanes[0].count(presence::Verdict::Present) == kFrames);
    CHECK(rep.lanes[1].count(presence::Verdict::Wrong) == kFrames);
}

// ---------------------------------------------------------------------------
// LS-1
// ---------------------------------------------------------------------------
TEST_CASE("SCHED LS-1: steady leads L in {0,1024,4096,16384} x Output blocks {64,256,1024} stay fully present",
          "[.][sched]")
{
    const std::vector<Ls1Cell>& cells = ls1Cells();
    REQUIRE(cells.size() == 12u);
    // 理想调度全在场 —— 差分的分母可信。
    for (const Run& ideal : ls1Ideal())
    {
        INFO(ideal.name);
        REQUIRE(ideal.settled);
        for (int lane = 0; lane < 3; ++lane)
        {
            const Tally t = tally(ideal, lane);
            REQUIRE(t.total >= 45);
            REQUIRE(t.wrong == 0);
            REQUIRE(t.mixPresent == t.total);
        }
        WARN(ideal.name << ": lane0 " << str(tally(ideal, 0)) << " non-mix l0" << describeNonMix(ideal, 0));
    }
    for (const Ls1Cell& c : cells)
    {
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        const Run& ideal = ls1IdealFor(c.outBlock);
        for (int lane = 0; lane < 3; ++lane)
        {
            INFO("lane " << lane);
            const Tally t = tally(c.run, lane);
            REQUIRE(t.total >= (c.outBlock == 64 ? 45 : 90));
            const Diff d = diffVsIdeal(ideal, lane, c.run, lane);
            CHECK(d.unmatched == 0);
            CHECK(d.lost == 0);
            CHECK(t.wrong == 0);
        }
        CHECK(maxMisalignAll(c.run) == 0);
        CHECK(c.run.inv.undeclared == 0);
        WARN(c.run.name << ": lane1 " << str(tally(c.run, 1)) << " misalign " << maxMisalignAll(c.run)
                        << " | non-mix l0" << describeNonMix(c.run, 0) << " l1" << describeNonMix(c.run, 1));
    }
}

// ---------------------------------------------------------------------------
// LS-2
// ---------------------------------------------------------------------------
TEST_CASE("SCHED LS-2: a start burst of the prefetched lanes neither raises misalign nor leaves the bus silent",
          "[.][sched]")
{
    for (const Ls2Cell& c : ls2Cells())
    {
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        const std::int64_t start = c.run.mark.at("start");
        for (int lane = 0; lane < 3; ++lane)
        {
            INFO("lane " << lane);
            const Tally before = tally(c.run, lane, 0, c.run.mark.at("stop"));
            const Tally after = tally(c.run, lane, start, kBig);
            REQUIRE(before.total >= 40);
            REQUIRE(after.total >= 85);
            CHECK(before.mixPresent == before.total);
            CHECK(after.mixPresent == after.total); // 起播第一帧起就在场(不是全零)
            CHECK(after.wrong == 0);
        }
        CHECK(maxMisalignAll(c.run) == 0); // 突发不误报
        WARN(c.run.name << ": after start lane1 " << str(tally(c.run, 1, start, kBig)) << " misalign "
                        << maxMisalignAll(c.run));
    }
}

// ---------------------------------------------------------------------------
// LS-3
// ---------------------------------------------------------------------------
TEST_CASE("SCHED LS-3: smooth live/prefetch switches (rewind, drain) lose at most one frame each and read no stale "
          "audio",
          "[.][sched]")
{
    const Ls3Run& r = ls3Smooth();
    REQUIRE(r.ideal.settled);
    REQUIRE(r.tested.settled);
    REQUIRE(r.switches.size() == 5u);
    const std::int64_t w0 = r.switches.front() - sec(1.0);
    for (int lane = 0; lane < 3; ++lane)
    {
        INFO("lane " << lane);
        const Diff d = diffVsIdeal(r.ideal, lane, r.tested, lane, w0, kBig);
        REQUIRE(d.idealPresent >= 250);
        CHECK(d.unmatched == 0);
        CHECK(d.wrong == 0);
        CHECK(d.lost <= (lane == 1 ? static_cast<int>(r.switches.size()) : 0));
        WARN("LS-3 smooth lane " << lane << ": " << str(d));
    }
    CHECK(maxMisalignAll(r.tested) == 0);
}

TEST_CASE("SCHED LS-3: jumping a lane's cursor ahead (host skips its render) reads no stale ring audio", "[.][sched]")
{
    const Ls3Run& r = ls3Jump();
    for (int lane = 0; lane < 3; ++lane)
    {
        INFO("lane " << lane);
        const Diff d = diffVsIdeal(r.ideal, lane, r.tested, lane, r.switches.front() - sec(1.0), kBig, r.excused);
        CHECK(d.wrong == 0);
        CHECK(d.lost <= (lane == 1 ? static_cast<int>(r.switches.size()) : 0));
    }
}

TEST_CASE("SCHED LS-3: jumping a lane's cursor ahead (host skips its render) reads no stale ring audio - "
          "precondition",
          "[.][sched]")
{
    const Ls3Run& r = ls3Jump();
    REQUIRE(r.ideal.settled);
    REQUIRE(r.tested.settled);
    REQUIRE(r.switches.size() == 3u);
    REQUIRE(r.excused.size() == 2u);
    // 切换前 11.5 s 连放 > R:环里每个槽都写过上一圈的有声数据。
    REQUIRE(r.switches.front() >= kRing + 8192);
    // lane 1 在第一切之前确实被注入、在场(否则「错读旧槽」无从谈起)。
    const Tally before = tally(r.tested, 1, r.switches.front() - sec(1.0), r.switches.front());
    REQUIRE(before.total >= 40);
    REQUIRE(before.mixPresent == before.total);
    // 第一切确实让 lane 1 的游标往前跳了一个提前量(块日志里出现一处游标缺口)。
    bool sawJump = false;
    const std::vector<sched::LaneBlock>& log = r.tested.laneLogs[1];
    for (std::size_t i = 1; i < log.size(); ++i)
    {
        sawJump = sawJump || (log[i].cursor - (log[i - 1].cursor + log[i - 1].n) == 8192);
    }
    REQUIRE(sawJump);
    // 理想台在切换段全在场(差分的分母可信)。
    const Tally ideal = tally(r.ideal, 1, r.switches.front(), kBig);
    REQUIRE(ideal.total >= 130);
    REQUIRE(ideal.mixPresent == ideal.total);
    for (int lane = 0; lane < 3; ++lane)
    {
        const Diff d = diffVsIdeal(r.ideal, lane, r.tested, lane, r.switches.front() - sec(1.0), kBig, r.excused);
        WARN("LS-3 jump lane " << lane << ": " << str(d) << " | tested "
                               << str(tally(r.tested, lane, r.switches.front(), kBig)));
    }
}

// ---------------------------------------------------------------------------
// LS-4(H1)
// ---------------------------------------------------------------------------
TEST_CASE("SCHED LS-4 (H1): with Cycle and mixed leads every lap of a led lane loses at most one frame and reads no "
          "stale audio",
          "[.][sched]")
{
    for (const Ls4Cell& c : ls4().cells)
    {
        INFO(c.run.name);
        const Run& ideal = ls4IdealFor(c.loopLen);
        for (const sched::Range& lap : lapsOf(c.run))
        {
            for (int lane : {1, 2})
            {
                INFO("lane " << lane << " lap [" << lap.begin << "," << lap.end << ")");
                const Diff d = diffVsIdeal(ideal, lane, c.run, lane, lap.begin, lap.end);
                CHECK(d.lost <= 1);
                CHECK(d.wrong == 0);
            }
        }
    }
}

TEST_CASE("SCHED LS-4 (H1): with Cycle and mixed leads every lap of a led lane loses at most one frame and reads no "
          "stale audio - precondition",
          "[.][sched]")
{
    const Ls4Family& fam = ls4();
    REQUIRE(fam.ideal.size() == 3u);
    REQUIRE(fam.cells.size() == 9u);
    for (const Run& ideal : fam.ideal)
    {
        INFO(ideal.name);
        REQUIRE(ideal.settled);
        for (int lane = 0; lane < 3; ++lane)
        {
            const Tally t = tally(ideal, lane);
            REQUIRE(t.total >= 360);
            REQUIRE(t.mixPresent == t.total); // 理想调度每圈全在场:差分的分母可信
        }
    }
    for (const Ls4Cell& c : fam.cells)
    {
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        const std::vector<sched::Range> laps = lapsOf(c.run);
        REQUIRE(static_cast<int>(laps.size()) == c.laps); // 圈确实回绕了 laps-1 次
        const Run& ideal = ls4IdealFor(c.loopLen);
        const Oracle o = ringOracle(c.run);
        REQUIRE(o.supported);
        std::ostringstream os;
        os << c.run.name << ":";
        for (int lane : {1, 2})
        {
            INFO("lane " << lane);
            // 写方确实比读方早约 Δ 跨过循环点(预取轨先回绕)。
            const sched::LaneBlock* wrap = laneFirstBackJump(c.run, lane);
            REQUIRE(wrap != nullptr);
            REQUIRE(wrap->tBus == kLs4LoopB);
            const std::int64_t outAtWrap = outRelAtSeq(c.run, wrap->seq);
            const std::int64_t leadObserved = laps[1].begin - outAtWrap;
            REQUIRE(leadObserved >= c.lead);
            REQUIRE(leadObserved <= c.lead + 2048);
            // 第 0 圈写方回绕之前那一段在场:轨确实注入、读方确实读得到这条轨。
            const Tally head = tally(c.run, lane, 0, outAtWrap);
            REQUIRE(head.total >= 30);
            REQUIRE(head.mixPresent == head.total);
            os << " lane" << lane << " leadObserved=" << leadObserved;
            for (std::size_t li = 0; li < laps.size(); ++li)
            {
                const sched::Range& lap = laps[li];
                INFO("lap " << li);
                // 环可用性:每圈(除换圈处至多一帧)Output 读的那一刻环里都写着本位置的有声数据 ——
                // 读不到是读方的缺陷,不是调度把数据弄没了。修复前后都成立。
                const std::pair<int, int> av = oracleFrames(c.run, o, lane, lap.begin, lap.end);
                REQUIRE(av.second >= 90);
                REQUIRE(av.first >= av.second - 1);
                const Diff d = diffVsIdeal(ideal, lane, c.run, lane, lap.begin, lap.end);
                REQUIRE(d.unmatched == 0);
                const Tally t = tally(c.run, lane, lap.begin, lap.end);
                os << " | lap" << li << " present " << pctOf(t.present, t.total) << "% (" << t.present << "/" << t.total
                   << ") oracle " << av.first << "/" << av.second << " " << str(d);
            }
        }
        // 对照轨(live)每圈全在场。
        const Diff ctl = diffVsIdeal(ideal, 0, c.run, 0);
        REQUIRE(ctl.lost == 0);
        os << " | lane0 " << str(ctl) << " | maxMisalign " << maxMisalignAll(c.run);
        WARN(os.str());
    }
}

// ---------------------------------------------------------------------------
// LS-5
// ---------------------------------------------------------------------------
TEST_CASE("SCHED LS-5: Cycle with the same lead on every instance (relative lead 0) stays fully present every lap",
          "[.][sched]")
{
    for (const Ls5Cell& c : ls5Cells())
    {
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        const std::vector<sched::Range> laps = lapsOf(c.run);
        REQUIRE(laps.size() >= 3u);
        for (const sched::Range& lap : laps)
        {
            for (int lane = 0; lane < 3; ++lane)
            {
                const Tally t = tally(c.run, lane, lap.begin, lap.end);
                REQUIRE(t.total >= 60);
                CHECK(t.mixPresent == t.total);
                CHECK(t.wrong == 0);
            }
        }
        CHECK(maxMisalignAll(c.run) == 0);
        WARN(c.run.name << ": laps " << laps.size() << " lane1 " << str(tally(c.run, 1)));
    }
}

// ---------------------------------------------------------------------------
// LS-6
// ---------------------------------------------------------------------------
TEST_CASE("SCHED LS-6: a back seek in the same block as the writer's (k=0 stale blocks) loses at most one frame",
          "[.][sched]")
{
    const SeekFamily& f = ls6();
    for (const SeekCell& c : f.cells)
    {
        if (c.stale != 0)
        {
            continue;
        }
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        for (int lane : {1, 2})
        {
            const Diff d = diffVsIdeal(seekIdealFor(f, 0), lane, c.run, lane);
            REQUIRE(d.idealPresent >= 250);
            CHECK(d.lost <= 1);
            CHECK(d.wrong == 0);
            WARN(c.run.name << " lane " << lane << ": " << str(d));
        }
    }
}

TEST_CASE("SCHED LS-6: a back seek with stale Output blocks and a leading writer loses at most one frame", "[.][sched]")
{
    const SeekFamily& f = ls6();
    for (const SeekCell& c : f.cells)
    {
        if (c.stale == 0)
        {
            continue;
        }
        INFO(c.run.name);
        for (int lane : {1, 2})
        {
            const Diff d = diffVsIdeal(seekIdealFor(f, c.stale), lane, c.run, lane);
            CHECK(d.lost <= 1);
            CHECK(d.wrong == 0);
        }
    }
}

TEST_CASE("SCHED LS-6: a back seek with stale Output blocks and a leading writer loses at most one frame - "
          "precondition",
          "[.][sched]")
{
    const SeekFamily& f = ls6();
    REQUIRE(f.cells.size() == 6u);
    for (const Run& ideal : f.ideal)
    {
        INFO(ideal.name);
        REQUIRE(ideal.settled);
        for (int lane = 0; lane < 3; ++lane)
        {
            const Tally t = tally(ideal, lane);
            REQUIRE(t.total >= 270);
            REQUIRE(t.mixPresent == t.total);
        }
    }
    for (const SeekCell& c : f.cells)
    {
        if (c.stale == 0)
        {
            continue;
        }
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        const std::int64_t seek = c.run.mark.at("seek");
        // Output 先放完 k 个旧块,再落到新位置(块表上那一次回跳正好在 seek + k·b_out)。
        const std::vector<std::int64_t> jumps = outputBackJumps(c.run);
        REQUIRE(jumps.size() == 1u);
        REQUIRE(jumps.front() == seek + static_cast<std::int64_t>(c.stale) * 1024);
        std::ostringstream os;
        os << c.run.name << ":";
        for (int lane : {1, 2})
        {
            // 定位前在场;写方在读方之前就已在新位置上重新渲染(先换代)。
            const Tally before = tally(c.run, lane, 0, seek);
            REQUIRE(before.mixPresent == before.total);
            REQUIRE(before.total >= 90);
            const sched::LaneBlock* first = laneFirstBlockAt(c.run, lane, f.to);
            REQUIRE(first != nullptr);
            REQUIRE(outRelAtSeq(c.run, first->seq) <= seek); // 写方在定位那一周期就换了代
            const Diff d = diffVsIdeal(seekIdealFor(f, c.stale), lane, c.run, lane);
            REQUIRE(d.unmatched == 0);
            os << " lane" << lane << " " << str(d) << " (" << 1000.0 * toSec(d.lost * kF) << " ms)";
        }
        os << " maxMisalign " << maxMisalignAll(c.run);
        WARN(os.str());
    }
}

// ---------------------------------------------------------------------------
// LS-7
// ---------------------------------------------------------------------------
TEST_CASE("SCHED LS-7: a forward seek with a leading writer reads no stale audio and is present right after the jump",
          "[.][sched]")
{
    const SeekFamily& f = ls7();
    REQUIRE(f.cells.size() == 6u);
    for (const SeekCell& c : f.cells)
    {
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        const std::int64_t seek = c.run.mark.at("seek");
        const std::int64_t jumpRel = seek + static_cast<std::int64_t>(c.stale) * 1024;
        for (int lane : {1, 2})
        {
            INFO("lane " << lane);
            const Diff all = diffVsIdeal(seekIdealFor(f, c.stale), lane, c.run, lane);
            const Diff afterJump = diffVsIdeal(seekIdealFor(f, c.stale), lane, c.run, lane, jumpRel, kBig);
            REQUIRE(afterJump.idealPresent >= 180);
            CHECK(all.wrong == 0);
            CHECK(afterJump.lost <= 1);
            CHECK(all.lost <= c.stale + 1);
            WARN(c.run.name << " lane " << lane << ": " << str(all));
        }
    }
}

// ---------------------------------------------------------------------------
// LS-8(H1′)
// ---------------------------------------------------------------------------
namespace
{
// A-3 读方层面与本台实测出错的格:Output 拿到垃圾值(只 Output / 两者都拿)。
bool ls8KnownBad(const Ls8Cell& c)
{
    const bool garbage = !c.quirk.reported.empty() && c.quirk.reported.front() == 540544;
    return garbage && (c.target == SchedRig::Target::Output || c.target == SchedRig::Target::All);
}

// 判据:起播后相对理想调度丢失 <= 2 帧;Wrong 帧只许落在宿主给错时间戳的那几块里(那里读方交出的是
// 它被告知的位置的数据,见 mislabeledBlocks),其余位置一帧都不许错。
void checkLs8Start(const Ls8Family& f, const Ls8Cell& c)
{
    INFO(c.run.name);
    const std::int64_t start = c.run.mark.at("start");
    const std::vector<sched::Range> mislabeled = mislabeledBlocks(c.run);
    for (int lane = 0; lane < 3; ++lane)
    {
        INFO("lane " << lane);
        const Diff d = diffVsIdeal(f.ideal, lane, c.run, lane, start, kBig);
        CHECK(d.lost <= 2);
        CHECK(wrongOutside(c.run, lane, start, kBig, mislabeled) == 0);
    }
}
} // namespace

TEST_CASE("SCHED LS-8: start quirks that leave the Output's first position sane lose at most two frames", "[.][sched]")
{
    const Ls8Family& f = ls8();
    REQUIRE(f.ideal.settled);
    int n = 0;
    for (const Ls8Cell& c : f.cells)
    {
        if (ls8KnownBad(c))
        {
            continue;
        }
        ++n;
        REQUIRE(c.run.settled);
        checkLs8Start(f, c);
        for (int lane = 0; lane < 3; ++lane)
        {
            WARN(c.run.name << " lane " << lane << ": "
                            << str(diffVsIdeal(f.ideal, lane, c.run, lane, c.run.mark.at("start"), kBig)));
        }
    }
    CHECK(n == 4);
}

TEST_CASE("SCHED LS-8 (H1'): a garbage start position on the Output loses at most two frames", "[.][sched]")
{
    const Ls8Family& f = ls8();
    for (const Ls8Cell& c : f.cells)
    {
        if (ls8KnownBad(c))
        {
            checkLs8Start(f, c);
        }
    }
}

TEST_CASE("SCHED LS-8 (H1'): a garbage start position on the Output loses at most two frames - precondition",
          "[.][sched]")
{
    const Ls8Family& f = ls8();
    REQUIRE(f.ideal.settled);
    REQUIRE(f.cells.size() == 7u);
    const std::int64_t idealStart = f.ideal.mark.at("start");
    for (int lane = 0; lane < 3; ++lane)
    {
        const Tally t = tally(f.ideal, lane, idealStart, kBig);
        REQUIRE(t.total >= 135);
        REQUIRE(t.mixPresent == t.total);
    }
    int bad = 0;
    for (const Ls8Cell& c : f.cells)
    {
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        const std::int64_t start = c.run.mark.at("start");
        // 停走带之前在场(轨确实注入)。
        for (int lane = 0; lane < 3; ++lane)
        {
            const Tally before = tally(c.run, lane, 0, start);
            REQUIRE(before.mixPresent == before.total);
        }
        // 怪癖确实在起播头几块被消费:目标实例报出了怪癖位置。
        const std::int64_t want = c.quirk.reported.front();
        bool outQuirked = false;
        for (const sched::OutBlock& b : c.run.outBlocks)
        {
            outQuirked = outQuirked || (b.quirked && b.tReported == want && b.cursor - c.run.c0 >= start);
        }
        bool laneQuirked = false;
        for (const sched::LaneBlock& b : c.run.laneLogs[0])
        {
            laneQuirked = laneQuirked || (b.quirked && b.tReported == want && b.cursor - c.run.c0 >= start);
        }
        const bool wantOut = c.target == SchedRig::Target::Output || c.target == SchedRig::Target::All;
        const bool wantLane = c.target == SchedRig::Target::AllInputs || c.target == SchedRig::Target::All;
        REQUIRE(outQuirked == wantOut);
        REQUIRE(laneQuirked == wantLane);
        bad += ls8KnownBad(c) ? 1 : 0;
        std::ostringstream os;
        os << c.run.name << ":";
        const std::vector<sched::Range> mislabeled = mislabeledBlocks(c.run);
        for (int lane = 0; lane < 3; ++lane)
        {
            os << " lane" << lane << " " << str(diffVsIdeal(f.ideal, lane, c.run, lane, start, kBig))
               << " wrongOutsideMislabeled " << wrongOutside(c.run, lane, start, kBig, mislabeled);
        }
        os << " mislabeledOutputBlocks " << mislabeled.size() << " maxMisalign " << maxMisalignAll(c.run);
        WARN(os.str());
    }
    REQUIRE(bad == 3);
}

// ---------------------------------------------------------------------------
// LS-9
// ---------------------------------------------------------------------------
TEST_CASE("SCHED LS-9: prefetch blocks of 1024 over a 512 prepare with Output blocks of 32 stay fully present (SL-523)",
          "[.][sched]")
{
    const Run& r = ls9();
    REQUIRE(r.settled);
    for (int lane = 0; lane < 3; ++lane)
    {
        INFO("lane " << lane);
        const Tally t = tally(r, lane);
        REQUIRE(t.total >= 45);
        CHECK(t.mixPresent == t.total);
        CHECK(t.wrong == 0);
    }
    CHECK(maxMisalignAll(r) == 0);
    CHECK(r.inv.undeclared == 0);
    WARN(r.name << ": lane1 " << str(tally(r, 1)));
}

// ---------------------------------------------------------------------------
// LS-10
// ---------------------------------------------------------------------------
TEST_CASE("SCHED LS-10: offline export with blocks 1024-4096 matches the realtime ideal schedule frame by frame",
          "[.][sched]")
{
    const Ls10Family& f = ls10();
    REQUIRE(f.realtime.settled);
    for (const Ls10Cell& c : f.cells)
    {
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        for (int lane = 0; lane < 3; ++lane)
        {
            INFO("lane " << lane);
            const Tally t = tally(c.run, lane);
            REQUIRE(t.total >= 230);
            CHECK(t.mixPresent == t.total);
            CHECK(verdictMismatchesByT(f.realtime, c.run, lane) == 0);
        }
        CHECK(maxMisalignAll(c.run) == 0);
        WARN(c.run.name << ": lane1 " << str(tally(c.run, 1)) << " wall " << c.run.scriptWallMs << " ms");
    }
}

// ---------------------------------------------------------------------------
// LS-11a(H5)
// ---------------------------------------------------------------------------
TEST_CASE("SCHED LS-11a (H5): with the Output not called the vocals fall back to raw passthrough after 5.5 +/- 0.5 s "
          "and rejoin lane by lane",
          "[.][sched][!shouldfail]")
{
    const Run& r = ls11a();
    const std::int64_t susp = r.mark.at("suspend");
    const std::int64_t resume = r.mark.at("resume");
    for (int lane = 0; lane < 3; ++lane)
    {
        INFO("lane " << lane);
        const std::int64_t firstRaw = firstFrame(r, lane, susp, resume, isRaw);
        CHECK(firstRaw >= susp + sec(5.0));
        CHECK(firstRaw <= susp + sec(6.0));
        if (firstRaw >= 0)
        {
            const Tally raw = tally(r, lane, firstRaw + 4 * kF, resume);
            CHECK(raw.rawPresent >= raw.total - 2);
        }
        const std::int64_t back = firstFrame(r, lane, resume, kBig, isMix);
        CHECK(back >= 0);
        CHECK(back <= resume + sec(2.0));
    }
}

TEST_CASE("SCHED LS-11a (H5): with the Output not called the vocals fall back to raw passthrough after 5.5 +/- 0.5 s "
          "and rejoin lane by lane - precondition",
          "[.][sched]")
{
    const Run& r = ls11a();
    REQUIRE(r.settled);
    const std::int64_t susp = r.mark.at("suspend");
    const std::int64_t resume = r.mark.at("resume");
    // 停调前在场;停调确实持续了 >= 6.5 s 墙钟(看门狗 0.5 s 与 5 s 滞回都跨得过)。
    REQUIRE(resume - susp >= sec(kLs11SuspendS) - 1024);
    REQUIRE(r.wall.at("resume") - r.wall.at("suspend") >= 6500.0);
    for (int lane = 0; lane < 3; ++lane)
    {
        const Tally before = tally(r, lane, sec(0.2), susp);
        REQUIRE(before.total >= 30);
        REQUIRE(before.mixPresent == before.total);
    }
    // 停调期间 Output 一块都没被调用,各 Input 照常被调用、源信号在放(写头在推进)。
    const int outCalled = countOutReason(r, sched::CallReason::Called, susp, resume);
    const int outSuspended = countOutReason(r, sched::CallReason::HostSuspended, susp, resume);
    REQUIRE(outCalled == 0);
    REQUIRE(outSuspended >= 320);
    for (int lane = 0; lane < 3; ++lane)
    {
        REQUIRE(countLaneReason(r, lane, sched::CallReason::Called, susp, resume) >= 320);
    }
    REQUIRE(r.inputs.size() >= 100u);
    std::ostringstream os;
    os << "LS-11a: suspended " << toSec(resume - susp) << " s (wall " << r.wall.at("resume") - r.wall.at("suspend")
       << " ms)";
    for (int lane = 0; lane < 3; ++lane)
    {
        const std::pair<int, int> m = maskOffSamples(r, lane, susp, resume);
        const std::pair<int, int> p = passthroughSamples(r, lane, susp, resume);
        const Tally during = tally(r, lane, susp, resume);
        os << " | lane" << lane << " during[" << str(during) << "] input maskBit off " << m.first << "/" << m.second
           << " passthrough " << p.first << "/" << p.second << " firstRaw " << firstFrame(r, lane, susp, resume, isRaw)
           << " firstMixAfterResume " << firstFrame(r, lane, resume, kBig, isMix) - resume;
    }
    WARN(os.str());
}

// ---------------------------------------------------------------------------
// LS-11b(只表征)
// ---------------------------------------------------------------------------
TEST_CASE("SCHED LS-11b: bus-silence Output suspension and whole-chain suspension are characterised", "[.][sched]")
{
    {
        const Run& r = ls11bTail();
        REQUIRE(r.settled);
        const std::int64_t on = r.mark.at("on");
        // 场景确实跑到:Output 至少被停调过一回,停调期间各 Input 照常被调用。
        REQUIRE_FALSE(r.outputSuspend.empty());
        REQUIRE(countOutReason(r, sched::CallReason::OutputSilenceTail, on, kBig) > 0);
        REQUIRE(countLaneReason(r, 0, sched::CallReason::Called, on, kBig) > 300);
        std::ostringstream os;
        os << "LS-11b tail: episodes " << r.outputSuspend.size();
        for (const sched::Range& e : r.outputSuspend)
        {
            os << " [" << toSec(e.begin - r.c0) << "s +" << toSec(e.end - e.begin) << "s]";
        }
        for (int s = 0; s < 8; ++s)
        {
            const std::int64_t a = on + sec(static_cast<double>(s));
            const Tally t = tally(r, 0, a, a + sec(1.0));
            os << " | " << s << "s:mix" << t.mixPresent << "/raw" << t.rawPresent << "/A" << t.absent;
        }
        const std::pair<int, int> m = maskOffSamples(r, 0, on, kBig);
        const std::pair<int, int> p = passthroughSamples(r, 0, on, kBig);
        os << " | lane0 maskBit off " << m.first << "/" << m.second << " passthrough " << p.first << "/" << p.second;
        WARN(os.str());
    }
    {
        const Run& r = ls11bChain();
        REQUIRE(r.settled);
        const std::int64_t g0 = r.mark.at("g0");
        const std::int64_t g1 = r.mark.at("g1");
        REQUIRE_FALSE(r.chainSuspend.empty());
        REQUIRE(countOutReason(r, sched::CallReason::ChainSuspended, g0, g1) > 0);
        REQUIRE(countLaneReason(r, 0, sched::CallReason::ChainSuspended, g0, g1) > 0);
        std::ostringstream os;
        os << "LS-11b chain: episodes " << r.chainSuspend.size();
        for (const sched::Range& e : r.chainSuspend)
        {
            os << " [" << toSec(e.begin - r.c0) << "s +" << toSec(e.end - e.begin) << "s]";
        }
        for (int lane = 0; lane < 3; ++lane)
        {
            const std::int64_t head = firstFrame(r, lane, g1, kBig, isMix);
            const Tally after = tally(r, lane, g1, g1 + sec(1.0));
            os << " | lane" << lane << " headLost " << (head < 0 ? -1 : (head - g1) / kF) << " frames after1s["
               << str(after) << "]";
        }
        WARN(os.str());
    }
}

// ---------------------------------------------------------------------------
// LS-12(E2-a / E2-b / H4)
// ---------------------------------------------------------------------------
namespace
{
// 按格分组(每组的归属都是实测确定的;墙钟门限两侧的余量由前提用例 REQUIRE):
//   · syncRealtime:同步实时(含单轨)—— 写头冻结 ≥ 500 ms ⇒ 挂起、退出注入集,恢复时句首缺席(E2-a);
//   · syncOfflineLong:同步离线 6 s(墙钟约 1.2 s)—— 同上,按倍速放大(E2-b);
//   · syncOfflineShort:同步离线 1 s(墙钟约 0.2 s,跨不过门限)—— 一直在注入集里,实测无损;
//   · leadRealtimeShort:领先实时 1 s —— Input 还在静音档、muted 位一直在,恢复那一拍即注入,必然落在读方到
//     g1 之前;但间隙 ≥ 500 ms 期间它被判挂起、退出了注入集(E2-a),读方断读之后重新接上;
//   · leadOfflineShort:领先离线 1 s —— 跨不过门限、一直在注入集里;
//     这两格在 A-5 之前读方锚在自己的 t0,把 [t0, g1) 的旧环槽当新数据读出(H4),句首「在场」是连旧带新一起读的;
//   · leadRealtimeLong:领先实时 6 s —— 错读同上(必然);但间隙 > 5.5 s 时 Input 已切直通,重新注入要等
//     muted 握手(最多两拍),80 ms 淡入可能压到 g1 上:句首丢 0–2 帧取决于拍点相位(E2-a 那一族);
//   · leadOfflineLong:领先离线 6 s —— 错读还是缺席取决于恢复那一拍落在读方到 g1 之前还是之后(写方领先
//     ≈ 34 ms 墙钟,与 [M] 一拍同量级),两种结果都是缺陷。
// [A-5] 领先四格的错音都并进 H4 那条(已翻转);句首改按领先轨判据 checkLs12LeadHead(见那里的注释)。
enum class Ls12Group
{
    SyncRealtime,
    SyncOfflineLong,
    SyncOfflineShort,
    LeadRealtimeShort,
    LeadOfflineShort,
    LeadRealtimeLong,
    LeadOfflineLong
};

Ls12Group ls12Group(const Ls12Cell& c)
{
    const bool longGap = c.gapS >= 2.0;
    if (!c.lead)
    {
        return !c.offline ? Ls12Group::SyncRealtime
                          : (longGap ? Ls12Group::SyncOfflineLong : Ls12Group::SyncOfflineShort);
    }
    if (!longGap)
    {
        return c.offline ? Ls12Group::LeadOfflineShort : Ls12Group::LeadRealtimeShort;
    }
    return c.offline ? Ls12Group::LeadOfflineLong : Ls12Group::LeadRealtimeLong;
}

double ls12GapWall(const Ls12Cell& c)
{
    return c.run.wall.at("g1") - c.run.wall.at("g0");
}

// 格的归属依赖墙钟门限(500 ms 停滞判定)落在哪一侧:跨不过门限的离线格墙钟必须 < 400 ms,该跨过的
// 必须 >= 600 ms,实时格必须按实时走。机器一慢、某格翻到门限另一侧时,这里先红,而不是在判据用例里
// 红得像一次回归。普通判据用例与前提用例调它;`[!shouldfail]` 的判据用例**不调** —— 在那里 REQUIRE 失败会被
// 当成「预期失败」吞掉,前提没成立反而看不出来。
void requireLs12WallSide(const Ls12Cell& c)
{
    INFO(c.run.name << " gapWall " << ls12GapWall(c) << " ms");
    if (!c.offline)
    {
        REQUIRE(ls12GapWall(c) >= 0.8 * 1000.0 * c.gapS);
    }
    else if (c.gapS < 2.0)
    {
        REQUIRE(ls12GapWall(c) < 400.0);
    }
    else
    {
        REQUIRE(ls12GapWall(c) >= 600.0);
    }
}

// 句首判据:恢复后第一帧就是 Mix 在场,没有原声过渡,之后也不丢。
void checkLs12Head(const Ls12Cell& c)
{
    const Ls12Metrics m = ls12Metrics(c);
    INFO(str(c, m));
    CHECK(m.headLostFrames == 0);
    CHECK(m.rawAfter == 0);
    CHECK(m.lostAfter == 0);
}

// 领先轨的句首判据(A-5 起)。写方领先时从间隙恢复,读方本地不知道写方新一代从哪一帧开始写(b_new):
// 新一代第一段发布之后,写头之下哪一段是本代写的、哪一段是间隙里没写过的旧环槽,单看 epoch 与写头分不出来,
// 读方只能锚在确认过的写头上(设计稿 §3.3 保守规则 / 残余风险 R4、§8 第 4 条)—— 句首最多丢一个写方块
// (本台预取块 1024 样本 = 一个分析帧),不出原声、之后不再丢。A-5 之前领先短间隙两格的句首「在场」,
// 是因为读方锚在自己的 t0、连同 [t0, g1) 的旧环槽一起读了(那就是 H4 的错音)。
void checkLs12LeadHead(const Ls12Cell& c)
{
    const Ls12Metrics m = ls12Metrics(c);
    INFO(str(c, m));
    CHECK(m.headLostFrames >= 0);
    CHECK(m.headLostFrames <= 1);
    CHECK(m.rawAfter == 0);
    CHECK(m.lostAfter <= 1);
}

// 全部判据:句首 + 不出现错音(间隙尾段与恢复之后)。
void checkLs12(const Ls12Cell& c)
{
    checkLs12Head(c);
    const Ls12Metrics m = ls12Metrics(c);
    INFO(str(c, m));
    CHECK(m.wrongNear == 0);
}

int ls12Count(Ls12Group g)
{
    int n = 0;
    for (const Ls12Cell& c : ls12Cells())
    {
        n += ls12Group(c) == g ? 1 : 0;
    }
    return n;
}

void ls12Precondition(const Ls12Cell& c)
{
    INFO(c.run.name);
    REQUIRE(c.run.settled);
    REQUIRE(c.run.mark.at("g0") == c.g0Rel);
    REQUIRE(c.run.mark.at("g1") == c.g1Rel);
    // 间隙前连放 > R + 提前量:间隙尾段的环槽里写着上一圈的有声数据。
    REQUIRE(c.g0Rel >= kRing + kLs12Lead + kF);
    // 被测轨间隙前在场(确实注入),间隙内宿主一块都没调用它,间隙后照常调用。
    const int lane = c.testLane;
    const Tally before = tally(c.run, lane, c.g0Rel - sec(1.0), c.g0Rel);
    REQUIRE(before.total >= 40);
    REQUIRE(before.mixPresent == before.total);
    REQUIRE(countLaneReason(c.run, lane, sched::CallReason::Called, c.g0Rel, c.g1Rel) == 0);
    REQUIRE(countLaneReason(c.run, lane, sched::CallReason::NoRegion, -kBig, kBig) > 0);
    REQUIRE(countLaneReason(c.run, lane, sched::CallReason::Called, c.g1Rel, kBig) > 50);
    // 对照轨(不受间隙影响)全程在场。
    if (!c.solo)
    {
        for (int other : {0, 2})
        {
            const Tally t = tally(c.run, other, c.g0Rel - sec(1.0), kBig);
            REQUIRE(t.mixPresent == t.total);
        }
    }
    requireLs12WallSide(c);
    WARN(str(c, ls12Metrics(c)));
}
} // namespace

TEST_CASE("SCHED LS-12: lanes that stay injected through a no-region gap are present right after it (a leading "
          "lane at most one writer block late)",
          "[.][sched]")
{
    // 同步离线 1 s(全部判据:第一帧就在场)与领先离线 1 s(领先轨句首判据;错音归 H4 那条)。
    // [A-5] 领先实时 1 s 原先也在这里(只看句首),现挪进 E2-a 那条:它在间隙里被判挂起、退出注入集,
    // 重新接上时多丢的那几帧归 E2-a(A-8);只剩一个写方块的那部分是领先轨固有的(checkLs12LeadHead)。
    REQUIRE(ls12Count(Ls12Group::SyncOfflineShort) == 1);
    REQUIRE(ls12Count(Ls12Group::LeadOfflineShort) == 1);
    for (const Ls12Cell& c : ls12Cells())
    {
        const Ls12Group g = ls12Group(c);
        if (g != Ls12Group::SyncOfflineShort && g != Ls12Group::LeadOfflineShort)
        {
            continue;
        }
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        requireLs12WallSide(c);
        if (g == Ls12Group::SyncOfflineShort)
        {
            checkLs12(c);
        }
        else
        {
            checkLs12LeadHead(c);
        }
    }
}

TEST_CASE("SCHED LS-12 (E2-a): a realtime lane suspended over a no-region gap is present on the first frame after "
          "it, with no raw transition",
          "[.][sched][!shouldfail]")
{
    for (const Ls12Cell& c : ls12Cells())
    {
        if (ls12Group(c) == Ls12Group::SyncRealtime)
        {
            checkLs12(c);
        }
        else if (ls12Group(c) == Ls12Group::LeadRealtimeLong || ls12Group(c) == Ls12Group::LeadRealtimeShort)
        {
            // 错音归 H4 那条;句首按领先轨判据(最多丢一个写方块,见 checkLs12LeadHead)。⚠ 领先实时 6 s 格的
            // 句首另有 0–2 帧取决于拍点相位(见 Ls12Group 注释);领先实时 1 s 格 A-5 实测丢 4 帧,多出来的是
            // 挂起退出注入集、重新接上那几拍(E2-a)。同组的同步实时格每次都红,所以它们混在这里不影响本条现在的
            // 结论;修复卡摘掉本条标记时,要单独确认这两格也过了(看 WARN 摘要里它们那两行),不要只看整条用例绿了没有。
            checkLs12LeadHead(c);
        }
    }
}

TEST_CASE("SCHED LS-12 (H4): a leading lane resuming after a no-region gap reads no stale ring audio", "[.][sched]")
{
    // [A-5] 翻转:读方不再把恢复点之前的旧环槽当新数据(设计稿 §3.3 保守规则)。领先离线 6 s 那一格的
    // 错音判据也并到这里 —— 它原先和句首判据捆在下面那条 [!shouldfail] 里;句首归 E2-b(A-8),留在那条。
    int n = 0;
    for (const Ls12Cell& c : ls12Cells())
    {
        const Ls12Group g = ls12Group(c);
        if (g == Ls12Group::LeadRealtimeShort || g == Ls12Group::LeadOfflineShort || g == Ls12Group::LeadRealtimeLong ||
            g == Ls12Group::LeadOfflineLong)
        {
            ++n;
            const Ls12Metrics m = ls12Metrics(c);
            INFO(str(c, m));
            CHECK(m.wrongNear == 0);
        }
    }
    CHECK(n == 4);
}

TEST_CASE("SCHED LS-12 (E2-b): an offline export loses nothing after a long no-region gap", "[.][sched][!shouldfail]")
{
    for (const Ls12Cell& c : ls12Cells())
    {
        if (ls12Group(c) == Ls12Group::SyncOfflineLong)
        {
            checkLs12(c);
        }
    }
}

TEST_CASE("SCHED LS-12 (E2-b): a leading offline lane after a long gap does not miss its head",
          "[.][sched][!shouldfail]")
{
    // A-4 时这条连错音带句首一起判(H4 / E2-b 二者必居其一);[A-5] 修掉了错音(并进上面 H4 那条),
    // 这里只留句首 —— 归 E2-b(A-8),按领先轨判据(最多丢一个写方块,见 checkLs12LeadHead)。
    for (const Ls12Cell& c : ls12Cells())
    {
        if (ls12Group(c) == Ls12Group::LeadOfflineLong)
        {
            checkLs12LeadHead(c);
        }
    }
}

TEST_CASE("SCHED LS-12: no-region gaps (E2-a / E2-b / H4) - precondition", "[.][sched]")
{
    const std::vector<Ls12Cell>& cells = ls12Cells();
    REQUIRE(cells.size() == 9u);
    REQUIRE(ls12Count(Ls12Group::SyncRealtime) == 3);
    REQUIRE(ls12Count(Ls12Group::SyncOfflineLong) == 1);
    REQUIRE(ls12Count(Ls12Group::SyncOfflineShort) == 1);
    REQUIRE(ls12Count(Ls12Group::LeadRealtimeShort) == 1);
    REQUIRE(ls12Count(Ls12Group::LeadOfflineShort) == 1);
    REQUIRE(ls12Count(Ls12Group::LeadRealtimeLong) == 1);
    REQUIRE(ls12Count(Ls12Group::LeadOfflineLong) == 1);
    for (const Ls12Cell& c : cells)
    {
        ls12Precondition(c);
    }
}

// ---------------------------------------------------------------------------
// LS-13
// ---------------------------------------------------------------------------
TEST_CASE("SCHED LS-13: near-ring seeks with a leading writer read no stale audio", "[.][sched]")
{
    const Ls13Family& f = ls13();
    REQUIRE(f.nearRing.size() == 6u);
    for (const Ls13Cell& c : f.nearRing)
    {
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        REQUIRE(c.run.mark.at("seek") >= kRing);
        CHECK(wrongAll(c.run) == 0);
        WARN(c.run.name << ":" << wrongByLane(c.run));
    }
}

TEST_CASE("SCHED LS-13: two seeks within the lead, the first nearly a ring forward, read no stale audio", "[.][sched]")
{
    for (const Ls13Cell& c : ls13().twoSeeks)
    {
        INFO(c.run.name);
        CHECK(wrongAll(c.run) == 0);
    }
}

TEST_CASE("SCHED LS-13: two seeks within the lead, the first nearly a ring forward, read no stale audio - "
          "precondition",
          "[.][sched]")
{
    const Ls13Family& f = ls13();
    REQUIRE(f.twoSeeks.size() == 4u);
    for (const Ls13Cell& c : f.twoSeeks)
    {
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        REQUIRE(c.run.mark.at("seek1") >= kRing);
        // 写方在读方自己跳变之前换了两次代:预取轨先在 S1、再在 S2 上渲染,两次都发生在 Output 还在旧位置上
        // 放在途旧块的时候(读方自己的第一次跳变在 seek1 + k·b_out)。
        const std::int64_t readerJump = c.run.mark.at("seek1") + 4 * 1024;
        for (int lane : {1, 2})
        {
            const sched::LaneBlock* w1 = laneFirstBlockAt(c.run, lane, c.run.val.at("s1"));
            REQUIRE(w1 != nullptr);
            const sched::LaneBlock* w2 = laneFirstBlockAt(c.run, lane, c.run.val.at("s2"), w1->seq);
            REQUIRE(w2 != nullptr);
            REQUIRE(w1->seq < w2->seq);
            REQUIRE(outRelAtSeq(c.run, w2->seq) < readerJump);
        }
        WARN(c.run.name << ":" << wrongByLane(c.run));
    }
}

TEST_CASE("SCHED LS-13: reader/writer target mismatch reads no stale audio where the current reader is safe",
          "[.][sched]")
{
    int n = 0;
    for (const Ls13Cell& c : ls13().mismatch)
    {
        if (c.expectWrongNow)
        {
            continue;
        }
        ++n;
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        for (int lane = 0; lane < 3; ++lane)
        {
            INFO("lane " << lane << describeNonMix(c.run, lane));
            CHECK(mismatchSafe(c.run, lane));
        }
        WARN(c.run.name << ":" << wrongByLane(c.run));
    }
    CHECK(n == 2);
}

TEST_CASE("SCHED LS-13: reader/writer target mismatch with the Output target before the writer's reads no stale "
          "audio",
          "[.][sched]")
{
    for (const Ls13Cell& c : ls13().mismatch)
    {
        if (c.expectWrongNow)
        {
            for (int lane = 0; lane < 3; ++lane)
            {
                INFO(c.run.name << " lane " << lane << describeNonMix(c.run, lane));
                CHECK(mismatchSafe(c.run, lane));
            }
        }
    }
}

TEST_CASE("SCHED LS-13: reader/writer target mismatch with the Output target before the writer's reads no stale "
          "audio - precondition",
          "[.][sched]")
{
    int n = 0;
    for (const Ls13Cell& c : ls13().mismatch)
    {
        if (!c.expectWrongNow)
        {
            continue;
        }
        ++n;
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        REQUIRE(c.run.mark.at("seek") >= kRing);
        // Output 那一块确实报了 S+ε(怪癖被消费),之后回到 S 起的正常位置。
        bool quirked = false;
        for (const sched::OutBlock& b : c.run.outBlocks)
        {
            quirked = quirked || (b.quirked && b.tBus == alignedSec(20.0));
        }
        REQUIRE(quirked);
        // [S+ε, S) 的环槽里确实是上一圈的有声数据(S − R + ε 落在先前连放过的那一段里)。
        REQUIRE(alignedSec(20.0) - 4096 - kRing >= alignedSec(1.0));
        REQUIRE(alignedSec(20.0) - kRing < alignedSec(1.0) + sec(11.5));
        std::ostringstream os;
        os << c.run.name << ":" << wrongByLane(c.run) << " | seek frame";
        for (int lane = 0; lane < 3; ++lane)
        {
            const sched::FrameVerdict* fr = frameAt(c.run, lane, c.run.mark.at("seek"));
            os << " l" << lane << "="
               << (fr == nullptr ? "none"
                                 : std::string(presence::toString(fr->verdict)) + "/" + presence::toString(fr->why));
        }
        WARN(os.str());
    }
    REQUIRE(n == 5);
}

TEST_CASE("SCHED LS-13: re-preparing a lane mono<->stereo mid-cycle reads no stale or mis-strided audio", "[.][sched]")
{
    const Ls13Cell& c = ls13().geometry;
    INFO(c.run.name);
    CHECK(wrongAll(c.run) == 0);
}

TEST_CASE("SCHED LS-13: re-preparing a lane mono<->stereo mid-cycle reads no stale or mis-strided audio - "
          "precondition",
          "[.][sched]")
{
    const Ls13Cell& c = ls13().geometry;
    INFO(c.run.name);
    REQUIRE(c.run.settled);
    REQUIRE(c.run.mark.count("toStereo") == 1u);
    REQUIRE(c.run.mark.count("toMono") == 1u);
    // 改布局落在第二圈、且在圈首之后不到 R:那个环槽里最后写的是本圈后段 p + R 的 mono 数据(上一圈写的),
    // 旧快照错读即判得出来。
    REQUIRE(lapsOf(c.run).size() == 2u);
    REQUIRE(c.run.mark.at("toStereo") > lapsOf(c.run)[1].begin);
    REQUIRE(c.run.val.at("tStereo") + sec(0.6) + kRing < kLs4LoopB + kGeoLoopLen);
    // 改布局前后被测轨都在场过(轨确实注入、改完确实接回)。
    INFO("lane1 before toStereo:" << describeNonMix(c.run, 1, 0, c.run.mark.at("toStereo"), 30));
    const Tally before = tally(c.run, 1, 0, c.run.mark.at("toStereo"));
    REQUIRE(before.total >= 500);
    REQUIRE(before.mixPresent == before.total);
    const Tally tail = tally(c.run, 1, c.run.c1 - c.run.c0 - sec(0.5), kBig);
    REQUIRE(tail.total >= 20);
    REQUIRE(tail.mixPresent == tail.total);
    std::ostringstream os;
    os << c.run.name << ":" << wrongByLane(c.run) << " | lane1 around toStereo["
       << str(tally(c.run, 1, c.run.mark.at("toStereo"), c.run.mark.at("toStereo") + sec(1.0))) << "] around toMono["
       << str(tally(c.run, 1, c.run.mark.at("toMono"), c.run.mark.at("toMono") + sec(1.0))) << "]";
    WARN(os.str());
}

// ---------------------------------------------------------------------------
// LS-14(H3)
// ---------------------------------------------------------------------------
TEST_CASE("SCHED LS-14 (H3): a reader that stays ahead of the writers from a generation start raises misalign within "
          "0.5 s, steadily, and clears within 1.5 s",
          "[.][sched][!shouldfail]")
{
    for (const Ls14Cell& c : ls14Cells())
    {
        if (!c.atGenerationStart)
        {
            continue;
        }
        for (int lane = 0; lane < 3; ++lane)
        {
            const AlarmTiming a = alarmTiming(c.run, lane);
            INFO(c.run.name << " lane " << lane << ": " << str(a));
            CHECK(a.onsetMs >= 0.0);
            CHECK(a.onsetMs <= 500.0);
            CHECK(a.flickerBeats == 0);
            CHECK(a.clearMs >= 0.0);
            CHECK(a.clearMs <= 1500.0);
        }
    }
}

TEST_CASE("SCHED LS-14 (H3): a reader that stays ahead of the writers from a generation start raises misalign within "
          "0.5 s, steadily, and clears within 1.5 s - precondition",
          "[.][sched]")
{
    const std::vector<Ls14Cell>& cells = ls14Cells();
    REQUIRE(cells.size() == 3u);
    for (const Ls14Cell& c : cells)
    {
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        const std::int64_t on = c.run.mark.at("on");
        const std::int64_t off = c.run.mark.at("off");
        REQUIRE(off - on >= sec(kLs14CondS) - 1024);
        REQUIRE(c.run.wall.at("off") - c.run.wall.at("on") >= 0.8 * 1000.0 * kLs14CondS);
        // 条件确实在每个周期都成立(标注过的图依赖违反),一个未标注的都没有。
        REQUIRE(c.run.inv.declared >= (off - on) / 1024 - 1);
        REQUIRE(c.run.inv.undeclared == 0);
        std::ostringstream os;
        os << c.run.name << ":";
        for (int lane = 0; lane < 3; ++lane)
        {
            // 真实状态:条件持续期间该轨在 Output 上读不到(Absent);撤掉之后 0.5 s 起恢复在场。
            const Tally during = tally(c.run, lane, on + sec(0.1), off);
            // 中途才开始的那一格:条件期间该轨被失准踢出注入集,撤掉条件时可能正处在 1 s 的失准恢复窗里,
            // 从 off + 1.5 s 起看恢复。
            const Tally after = tally(c.run, lane, off + (c.atGenerationStart ? sec(0.5) : sec(1.5)), kBig);
            REQUIRE(during.total >= 100);
            REQUIRE(after.total >= 20);
            if (c.atGenerationStart)
            {
                REQUIRE(during.absent == during.total);
            }
            REQUIRE(after.mixPresent == after.total);
            os << " | lane" << lane << " during[" << str(during) << "] alarm " << str(alarmTiming(c.run, lane))
               << " maxMisalign " << maxMisalign(c.run, lane, on, off);
        }
        WARN(os.str());
    }
}

// ---------------------------------------------------------------------------
// LS-15(守住 E2 的边界)
// ---------------------------------------------------------------------------
TEST_CASE("SCHED LS-15: stopping and restarting the transport with leading writers keeps the current behaviour",
          "[.][sched]")
{
    const std::vector<Ls15Cell>& cells = ls15Cells();
    REQUIRE(cells.size() == 2u);
    for (const Ls15Cell& c : cells)
    {
        INFO(c.run.name);
        REQUIRE(c.run.settled);
        const std::int64_t stop = c.run.mark.at("stop");
        const std::int64_t start = c.run.mark.at("start");
        const double stopWall = c.run.wall.at("start") - c.run.wall.at("stop");
        INFO("stop wall " << stopWall << " ms");
        REQUIRE(stopWall >= 0.8 * 1000.0 * c.stopS);
        // 短停格成立的前提是停走带墙钟跨不过 500 ms 的停滞门限:机器一慢翻到另一侧时这里先红。
        if (c.stopS < 1.0)
        {
            REQUIRE(stopWall < 400.0);
        }
        REQUIRE(c.run.inputs.size() >= 20u);
        const bool longStop = c.stopS >= 1.0;
        std::ostringstream os;
        os << c.run.name << ":";
        for (int lane = 0; lane < 3; ++lane)
        {
            INFO("lane " << lane);
            const Tally before = tally(c.run, lane, stop - sec(0.3), stop);
            REQUIRE(before.mixPresent == before.total);
            const std::int64_t head = firstFrame(c.run, lane, start, kBig, isMix);
            REQUIRE(head >= 0);
            const int headLost = static_cast<int>((head - start) / kF);
            const Tally after = tally(c.run, lane, start, kBig);
            CHECK(after.wrong == 0);
            CHECK(after.rawAny == 0); // 起播不出原声
            // 短停(< 500 ms 停滞门限):第一帧起在场;长停:该轨在停走带期间按 500 ms 规则退出
            // connected_mask,起播后约一拍 [M] 内接回(含 80 ms 淡入)。
            CHECK(headLost <= (longStop ? 12 : 1));
            const Tally settledAfter = tally(c.run, lane, head + 6 * kF, kBig);
            CHECK(settledAfter.mixPresent == settledAfter.total);
            const std::pair<int, int> m = maskOffSamples(c.run, lane, stop + sec(0.6), start);
            if (longStop)
            {
                CHECK(m.first > 0); // 停走带 >= 0.5 s:写头冻结 ⇒ 退出 connected_mask(现行 500 ms 规则)
                const std::pair<int, int> back = maskOffSamples(c.run, lane, start + sec(1.0), kBig);
                CHECK(back.first == 0);
            }
            else
            {
                const std::pair<int, int> whole = maskOffSamples(c.run, lane, stop, kBig);
                CHECK(whole.first == 0);
            }
            os << " | lane" << lane << " headLost " << headLost << " after[" << str(after) << "] maskOff(stop) "
               << m.first << "/" << m.second;
        }
        CHECK(maxMisalignAll(c.run) == 0);
        WARN(os.str());
    }
}
