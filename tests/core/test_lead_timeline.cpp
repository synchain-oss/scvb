// SPDX-License-Identifier: GPL-3.0-or-later
// test_lead_timeline —— [SL-216 / J136] 主唱进分析:lead_select 时间线记录 + 管线逐区间并入集合 C。
//
// 四层各钉各的:
//   · LeadTimeline / majorityLead / LeadRecorder / LEAD 编解码 —— 纯数据结构;
//   · runAnalysisPipeline —— 记录里选中的那一轨在它被选中的那段居中、不占槽,其余声部按剩下的轨数排槽;
//     与「把那一轨设成 lead_lock」逐位同解(同一条 C 路径,平衡也一样);没有记录且当前值为 0 ⇒ 与改动前同解。
//   · [SL-545 / J143 + J143b] 点分析那一刻的 lead_select(cfg.leadFallback)怎么进来:只有**宿主写的**记录
//     (`LeadRun::automated`)算自动化 —— 窗里一条都没有 ⇒ 整窗用它;有 ⇒ 宿主记录盖到的地方按记录,
//     其余地方用它。记录的来源怎么定(`LeadWriteOrigin`)、怎么存(LEAD minor 2)也在这里 —— 标签 [sl545]。
//   · [SL-570 / J167] 主唱切换立即生效 + 吸附:有效主唱 E(t)、切换点吸附(边界 → 停顿 → 原位)、去抖、
//     按切换点切子区间;E 全程不变时与 lead_lock 逐位同解 —— 标签 [sl570]。
// 接线(Output 播放时记录 / 分析取记录 / 随工程存取 / 当前值取参数面)在 tests/host/test_host_harness.cpp 的
// [sl216] 与 [sl545]。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "analysis/AnalysisPipeline.h"
#include "analysis/LeadTimeline.h"

using namespace scvb::analysis;

namespace
{

constexpr double kSr = 48000.0;
constexpr int kHopMs = 10;
constexpr std::int64_t kHopSamples = 480;

PipelineTrackFeatures makeAlternating(int rounds, int loudHops, int quietHops, float loudKw)
{
    PipelineTrackFeatures f;
    for (int r = 0; r < rounds; ++r)
    {
        for (int i = 0; i < loudHops; ++i)
        {
            f.kwMs.push_back(loudKw);
            f.peak.push_back(std::sqrt(loudKw));
        }
        for (int i = 0; i < quietHops; ++i)
        {
            f.kwMs.push_back(1e-9f);
            f.peak.push_back(1e-5f);
        }
    }
    f.covered.assign(f.kwMs.size(), 1u);
    f.anyCovered = !f.kwMs.empty();
    return f;
}

PipelineConfig makeConfig(std::size_t numHops, int activeTracks)
{
    PipelineConfig cfg;
    cfg.sampleRate = kSr;
    cfg.hopMs = kHopMs;
    cfg.rangeStartSample = 0;
    cfg.rangeEndSample = static_cast<std::int64_t>(numHops) * kHopSamples;
    for (int t = 0; t < kPipelineTracks; ++t)
    {
        cfg.tracks[static_cast<std::size_t>(t)].enabled = (t < activeTracks);
    }
    return cfg;
}

// 三轨同时发声(同一包络,能量相同)—— 每个区间三轨都活跃,指派结果只由槽位规则决定。
// 轨 1(下标 0)优先级压到 0:优先级高 → 角度大(02 §5.3 p_target),所以最低的那条拿中心槽;
// 这样「选轨 2 当主唱」与「没选」给出的中心轨不同,两种结果可区分。
struct ThreeVoices
{
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    PipelineConfig cfg;
    ThreeVoices()
    {
        for (int t = 0; t < 3; ++t)
        {
            features[static_cast<std::size_t>(t)] = makeAlternating(4, 80, 60, 0.05f);
        }
        cfg = makeConfig(features[0].kwMs.size(), 3);
        cfg.tracks[0].priority = 0.0;
    }
};

// [SL-545] minor 1 的 LEAD 载荷(SL-216 的格式:记录 20 字节、没有 flags)。本构建的编码器只写 minor 2,
// 所以旧格式手拼。
struct V1Run
{
    std::int64_t t0;
    std::int64_t t1;
    std::uint32_t lead;
};

void putLe(std::vector<std::uint8_t>& out, std::uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i)
    {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
}

std::vector<std::uint8_t> legacyLeadChunk(const std::vector<V1Run>& runs)
{
    std::vector<std::uint8_t> out;
    putLe(out, 1, 2); // minor 1
    putLe(out, 0, 2); // reserved
    putLe(out, runs.size(), 4);
    for (const auto& r : runs)
    {
        putLe(out, static_cast<std::uint64_t>(r.t0), 8);
        putLe(out, static_cast<std::uint64_t>(r.t1), 8);
        putLe(out, r.lead, 4);
    }
    return out;
}

bool allPansAre(const std::vector<AnalysisSegment>& segs, double v)
{
    if (segs.empty())
    {
        return false;
    }
    for (const auto& s : segs)
    {
        if (std::abs(s.pan - v) > 1e-9)
        {
            return false;
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// LeadTimeline
// ---------------------------------------------------------------------------

TEST_CASE("SL216 时间线:连续同值写入合并成一段", "[analysis][lead][sl216]")
{
    LeadTimeline tl;
    for (int b = 0; b < 100; ++b)
    {
        CHECK(tl.write(b * 512, (b + 1) * 512, 3, false));
    }
    const auto runs = tl.runs();
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].t0 == 0);
    CHECK(runs[0].t1 == 100 * 512);
    CHECK(runs[0].lead == 3);
}

TEST_CASE("SL216 时间线:后写覆盖先写(中间劈开、两端保留)", "[analysis][lead][sl216]")
{
    LeadTimeline tl;
    CHECK(tl.write(0, 1000, 1, false));
    CHECK(tl.write(400, 600, 2, false)); // 劈开
    auto runs = tl.runs();
    REQUIRE(runs.size() == 3);
    CHECK((runs[0].t0 == 0 && runs[0].t1 == 400 && runs[0].lead == 1));
    CHECK((runs[1].t0 == 400 && runs[1].t1 == 600 && runs[1].lead == 2));
    CHECK((runs[2].t0 == 600 && runs[2].t1 == 1000 && runs[2].lead == 1));

    // 跨三段覆盖:左截、中删、右截。
    CHECK(tl.write(300, 700, 0, false));
    runs = tl.runs();
    REQUIRE(runs.size() == 3);
    CHECK((runs[0].t0 == 0 && runs[0].t1 == 300 && runs[0].lead == 1));
    CHECK((runs[1].t0 == 300 && runs[1].t1 == 700 && runs[1].lead == 0));
    CHECK((runs[2].t0 == 700 && runs[2].t1 == 1000 && runs[2].lead == 1));

    // 用原值盖回中间:三段合回一段(两侧相接同值合并)。
    CHECK(tl.write(300, 700, 1, false));
    runs = tl.runs();
    REQUIRE(runs.size() == 1);
    CHECK((runs[0].t0 == 0 && runs[0].t1 == 1000 && runs[0].lead == 1));
}

TEST_CASE("SL216 时间线:非法写入忽略", "[analysis][lead][sl216]")
{
    LeadTimeline tl;
    CHECK_FALSE(tl.write(100, 100, 1, false)); // 空
    CHECK_FALSE(tl.write(100, 50, 1, false)); // 倒序
    CHECK_FALSE(tl.write(-10, 50, 1, false)); // 负起点
    CHECK_FALSE(tl.write(0, 50, 16, false)); // 越界
    CHECK_FALSE(tl.write(0, 50, -1, false));
    CHECK(tl.empty());
}

TEST_CASE("SL216 时间线:runsOverlapping 取与窗相交的段(含跨左边界那段)", "[analysis][lead][sl216]")
{
    LeadTimeline tl;
    tl.write(0, 100, 1, false);
    tl.write(100, 200, 2, false);
    tl.write(200, 300, 3, false);
    const auto r = tl.runsOverlapping(150, 250);
    REQUIRE(r.size() == 2);
    CHECK(r[0].lead == 2);
    CHECK(r[1].lead == 3);
    CHECK(tl.runsOverlapping(300, 400).empty());
}

TEST_CASE("SL216 多数值:按已记录样本数取最多,平局取小,未记录不算", "[analysis][lead][sl216]")
{
    const std::vector<LeadRun> runs{{0, 300, 2}, {300, 1000, 5}};
    CHECK(majorityLead(runs, 0, 1000) == 5);
    CHECK(majorityLead(runs, 0, 400) == 2);
    CHECK(majorityLead(runs, 200, 400) == 2); // 100 vs 100 平局 → 小的
    CHECK(majorityLead(runs, 1000, 2000) == 0); // 全未记录
    // 窗里只有一小截有记录:仍按有记录的那部分定(未记录不稀释)。
    CHECK(majorityLead(runs, 900, 5000) == 5);
    // 0 是一个值,参与计数。
    const std::vector<LeadRun> withZero{{0, 600, 0}, {600, 1000, 4}};
    CHECK(majorityLead(withZero, 0, 1000) == 0);
}

TEST_CASE("SL545 多数值:整段没有记录 → 回落值;有一个已记录样本就按记录(记录的 0 也压过回落值)",
          "[analysis][lead][sl545]")
{
    const std::vector<LeadRun> runs{{0, 300, 2}, {300, 1000, 5}};
    CHECK(majorityLead(runs, 1000, 2000, 7) == 7); // 全未记录 → 回落值
    CHECK(majorityLead({}, 0, 1000, 7) == 7); // 一条记录都没有 → 回落值
    CHECK(majorityLead(runs, 1000, 1000, 7) == 7); // 空区间:没有已记录样本,同一条定义
    // 窗里只有一小截有记录:仍按记录定,回落值不参与计数(记录优先)。
    CHECK(majorityLead(runs, 900, 5000, 7) == 5);
    CHECK(majorityLead(runs, 0, 1000, 7) == 5);
    // 记录了 0 ≠ 没有记录:0 是一个值,压过回落值。
    const std::vector<LeadRun> zeros{{0, 1000, 0}};
    CHECK(majorityLead(zeros, 0, 1000, 7) == 0);
    CHECK(majorityLead(zeros, 1000, 2000, 7) == 7); // 同一份记录,窗挪到记录外 → 回落
}

TEST_CASE("SL545 automatedLeadRuns:只留宿主写的记录,顺序与字段不变", "[analysis][lead][sl545]")
{
    const std::vector<LeadRun> runs{{0, 100, 0, false}, {100, 200, 3, true}, {200, 300, 3, false}, {300, 400, 0, true}};
    const auto a = automatedLeadRuns(runs);
    REQUIRE(a.size() == 2);
    CHECK((a[0].t0 == 100 && a[0].t1 == 200 && a[0].lead == 3 && a[0].automated));
    CHECK((a[1].t0 == 300 && a[1].t1 == 400 && a[1].lead == 0 && a[1].automated));
    CHECK(automatedLeadRuns({{0, 100, 5, false}}).empty());
    CHECK(automatedLeadRuns({}).empty());
}

// ---------------------------------------------------------------------------
// [SL-570 / J167] 有效主唱 E(t) 与切换点吸附 —— 纯函数
//
// 下面几条的单位约定:1 个样本当 1 ms 读(`kMs`),吸附半径 1000 = 1 s、hop 10 = 10 ms、最短片段 150 = 150 ms ——
// 与分析里的真实换算(48 kHz 下 48000 / 480 / 7200 样本)同比例,数字好读。
// ---------------------------------------------------------------------------

namespace
{
const LeadSnapParams kMs{/*radiusSamples=*/1000, /*hopSamples=*/10, /*minPieceSamples=*/150};

std::string describePieces(const std::vector<LeadPiece>& ps)
{
    std::ostringstream o;
    for (const auto& p : ps)
    {
        o << "[" << p.t0 << "," << p.t1 << ")=" << p.lead << " ";
    }
    return o.str();
}

void checkPieces(const std::vector<LeadPiece>& got, const std::vector<LeadPiece>& want)
{
    INFO("got:  " << describePieces(got));
    INFO("want: " << describePieces(want));
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < got.size(); ++i)
    {
        CHECK(got[i].t0 == want[i].t0);
        CHECK(got[i].t1 == want[i].t1);
        CHECK(got[i].lead == want[i].lead);
    }
}

std::vector<LeadPiece> snapNoPauses(const std::vector<LeadPiece>& ps, const std::vector<std::int64_t>& bounds)
{
    return snapLeadSwitches(ps, bounds, LeadPauseLookup{}, kMs);
}

LeadPauseLookup fixedPauses(std::vector<std::int64_t> ps)
{
    return [ps](std::int64_t) { return ps; };
}
} // namespace

TEST_CASE("SL570 有效主唱:宿主记录盖到处取记录值、盖不到处取回落值,相邻同值合并,盖满窗口", "[analysis][lead][sl570]")
{
    // 没有记录 ⇒ 整窗一个片段 = 回落值(点分析那一刻的值)。
    checkPieces(effectiveLeadPieces({}, 0, 500, 2), {{0, 500, 2}});
    // 记录之间的空档、两头 ⇒ 回落值。
    checkPieces(effectiveLeadPieces({{100, 200, 3, true}, {300, 400, 5, true}}, 0, 500, 2),
                {{0, 100, 2}, {100, 200, 3}, {200, 300, 2}, {300, 400, 5}, {400, 500, 2}});
    // 记录值恰好等于回落值 ⇒ 与两侧空档合成一段(没有切换点)。
    checkPieces(effectiveLeadPieces({{100, 200, 2, true}}, 0, 500, 2), {{0, 500, 2}});
    // 记录了 0 ≠ 没有记录:0 是「无主唱」这个值,回落值 7 只补盖不到的地方。
    checkPieces(effectiveLeadPieces({{0, 100, 0, true}}, 0, 300, 7), {{0, 100, 0}, {100, 300, 7}});
    // 窗口外的部分裁掉;整条在窗口外的记录不看。
    checkPieces(effectiveLeadPieces({{0, 150, 3, true}, {600, 900, 4, true}}, 100, 500, 2),
                {{100, 150, 3}, {150, 500, 2}});
    // 空窗口 ⇒ 空。
    CHECK(effectiveLeadPieces({{0, 150, 3, true}}, 500, 500, 2).empty());
}

TEST_CASE("SL570 去抖:短于最短片段的并进前一段;第一段太短并进后一段", "[analysis][lead][sl570]")
{
    // 自动化斜线途经的中间值(各 20):旧主唱一直唱到新主唱真正开始。
    std::vector<LeadPiece> ramp{{0, 1000, 2}, {1000, 1020, 3}, {1020, 1040, 4}, {1040, 2000, 7}};
    absorbShortLeadPieces(ramp, 150);
    checkPieces(ramp, {{0, 1040, 2}, {1040, 2000, 7}});
    // 一闪而过又回来:前后同值,合回一段。
    std::vector<LeadPiece> blip{{0, 1000, 2}, {1000, 1100, 5}, {1100, 2000, 2}};
    absorbShortLeadPieces(blip, 150);
    checkPieces(blip, {{0, 2000, 2}});
    // 第一段太短:它前面没有东西,窗口一开头就换人 ⇒ 开头那一小截按换过去的值。
    std::vector<LeadPiece> head{{0, 100, 2}, {100, 2000, 7}};
    absorbShortLeadPieces(head, 150);
    checkPieces(head, {{0, 2000, 7}});
    // 最后一段太短:并进前一段。
    std::vector<LeadPiece> tail{{0, 1900, 2}, {1900, 2000, 7}};
    absorbShortLeadPieces(tail, 150);
    checkPieces(tail, {{0, 2000, 2}});
    // 零长(两个切换点吸到同一处):并掉 ⇒ 那一处之后直接是后一个值。
    std::vector<LeadPiece> zero{{0, 1000, 2}, {1000, 1000, 5}, {1000, 2000, 7}};
    absorbShortLeadPieces(zero, 150);
    checkPieces(zero, {{0, 1000, 2}, {1000, 2000, 7}});
    // 恰好等于最短片段不算短(判据是「短于」)。
    std::vector<LeadPiece> exact{{0, 1000, 2}, {1000, 1150, 5}, {1150, 2000, 7}};
    absorbShortLeadPieces(exact, 150);
    checkPieces(exact, {{0, 1000, 2}, {1000, 1150, 5}, {1150, 2000, 7}});
    // 全都短:合成一段,值取第一段的。
    std::vector<LeadPiece> allShort{{0, 50, 2}, {50, 100, 5}};
    absorbShortLeadPieces(allShort, 150);
    checkPieces(allShort, {{0, 100, 2}});
}

TEST_CASE("SL570 最近点:半径含端点,等距取早,没有就不动", "[analysis][lead][sl570]")
{
    const std::vector<std::int64_t> pts{1000, 3000, 5000};
    std::int64_t at = -1;
    CHECK(nearestWithin(pts, 3000, 1000, at));
    CHECK(at == 3000); // 恰在点上
    CHECK(nearestWithin(pts, 3400, 1000, at));
    CHECK(at == 3000);
    CHECK(nearestWithin(pts, 4000, 1000, at));
    CHECK(at == 3000); // 与 3000 / 5000 等距 ⇒ 取早
    CHECK(nearestWithin(pts, 6000, 1000, at));
    CHECK(at == 5000); // 恰在半径上:算
    at = -1;
    CHECK_FALSE(nearestWithin(pts, 6001, 1000, at));
    CHECK(at == -1); // 没有 ⇒ 不动
    CHECK_FALSE(nearestWithin({}, 10, 1000, at));
}

TEST_CASE("SL570 吸附:切换点恰在边界上 ⇒ 不动", "[analysis][lead][sl570]")
{
    checkPieces(snapNoPauses({{0, 3000, 2}, {3000, 6000, 7}}, {0, 3000, 6000}), {{0, 3000, 2}, {3000, 6000, 7}});
}

TEST_CASE("SL570 吸附:1 s 内有边界 ⇒ 吸到最近的那个(画晚 / 画早都行)", "[analysis][lead][sl570]")
{
    // 画晚 0.5 s:吸回句首 3000。
    checkPieces(snapNoPauses({{0, 3500, 2}, {3500, 8000, 7}}, {0, 3000, 8000}), {{0, 3000, 2}, {3000, 8000, 7}});
    // 画早 0.6 s:吸到 3000。
    checkPieces(snapNoPauses({{0, 2400, 2}, {2400, 8000, 7}}, {0, 3000, 8000}), {{0, 3000, 2}, {3000, 8000, 7}});
    // 两个边界都在 1 s 内:吸最近的(3900 比 3000 近)。
    checkPieces(snapNoPauses({{0, 3500, 2}, {3500, 8000, 7}}, {0, 3000, 3900, 8000}), {{0, 3900, 2}, {3900, 8000, 7}});
    // 半径含端点:离边界 0 恰好 1000 ⇒ 吸到 0,第一段变零长、并进后一段 ⇒ 整窗是新值。
    checkPieces(snapNoPauses({{0, 1000, 2}, {1000, 5000, 7}}, {0, 5000}), {{0, 5000, 7}});
}

TEST_CASE("SL570 吸附:1 s 内没有边界 ⇒ 停顿;停顿也没有 ⇒ 原位取整到 hop", "[analysis][lead][sl570]")
{
    const std::vector<std::int64_t> bounds{0, 8000};
    // 没有停顿:原位。3004 → 3000,3006 → 3010(hop = 10)。
    checkPieces(snapNoPauses({{0, 3004, 2}, {3004, 8000, 7}}, bounds), {{0, 3000, 2}, {3000, 8000, 7}});
    checkPieces(snapNoPauses({{0, 3006, 2}, {3006, 8000, 7}}, bounds), {{0, 3010, 2}, {3010, 8000, 7}});
    // 离边界超出 1 s 一个 hop:不吸边界。
    checkPieces(snapNoPauses({{0, 1010, 2}, {1010, 8000, 7}}, bounds), {{0, 1010, 2}, {1010, 8000, 7}});
    // 1 s 内有停顿:吸过去。
    checkPieces(snapLeadSwitches({{0, 3000, 2}, {3000, 8000, 7}}, bounds, fixedPauses({2500, 6000}), kMs),
                {{0, 2500, 2}, {2500, 8000, 7}});
    // 停顿在 1 s 外:原位。
    checkPieces(snapLeadSwitches({{0, 3000, 2}, {3000, 8000, 7}}, bounds, fixedPauses({1900, 4100}), kMs),
                {{0, 3000, 2}, {3000, 8000, 7}});
    // 边界优先于停顿:1 s 内有边界就不看停顿,哪怕停顿更近。
    checkPieces(snapLeadSwitches({{0, 3000, 2}, {3000, 8000, 7}}, {0, 3900, 8000}, fixedPauses({3100}), kMs),
                {{0, 3900, 2}, {3900, 8000, 7}});
}

TEST_CASE("SL570 吸附:两个切换点离得很近 —— 吸到不同边界各自保留,吸到同一边界后写的赢", "[analysis][lead][sl570]")
{
    // 各自最近的边界不同(3000 / 3200),中间那段 200 >= 150,保留。
    checkPieces(snapNoPauses({{0, 2950, 2}, {2950, 3250, 5}, {3250, 8000, 7}}, {0, 3000, 3200, 8000}),
                {{0, 3000, 2}, {3000, 3200, 5}, {3200, 8000, 7}});
    // 吸附冲突:两个都吸到 3000 ⇒ 中间那段零长、并掉 ⇒ 3000 之后直接是后一个值。
    checkPieces(snapNoPauses({{0, 2900, 2}, {2900, 3300, 5}, {3300, 8000, 7}}, {0, 3000, 8000}),
                {{0, 3000, 2}, {3000, 8000, 7}});
}

TEST_CASE("SL570 吸附:吸完被挤得太短的片段并进前一段", "[analysis][lead][sl570]")
{
    // 3700 → 停顿 4000,4400 → 停顿 4100:中间那段只剩 100 < 150 ⇒ 并进前一段,4100 之后是后一个值。
    checkPieces(
        snapLeadSwitches({{0, 3700, 2}, {3700, 4400, 5}, {4400, 10000, 7}}, {0, 10000}, fixedPauses({4000, 4100}), kMs),
        {{0, 4100, 2}, {4100, 10000, 7}});
}

TEST_CASE("SL570 吸附:极短片段在吸附前就去掉 —— 否则它的两条边各吸各的,能被撑成一整秒", "[analysis][lead][sl570]")
{
    // 2 → 5(20 长)→ 7。前一条边 2990 离边界 2000 有 990、会被吸过去;后一条边 3010 离 2000 有 1010、不吸。
    // 不先去抖:5 被撑成 [2000, 3010) 一整秒多。先去抖:5 当场并进 2,只剩一个切换点 3010,原位。
    checkPieces(snapNoPauses({{0, 2990, 2}, {2990, 3010, 5}, {3010, 8000, 7}}, {0, 2000, 8000}),
                {{0, 3010, 2}, {3010, 8000, 7}});
}

TEST_CASE("SL570 吸附:宿主记录与回落值的交界也是切换点,照样吸附", "[analysis][lead][sl570]")
{
    // 宿主记录 3 播到 3500 就停了,之后没播到 ⇒ 点分析那一刻的值 2;交界 3500 吸到句首 3000。
    const auto pieces = effectiveLeadPieces({{0, 3500, 3, true}}, 0, 8000, 2);
    checkPieces(pieces, {{0, 3500, 3}, {3500, 8000, 2}});
    checkPieces(snapNoPauses(pieces, {0, 3000, 8000}), {{0, 3000, 3}, {3000, 8000, 2}});
}

TEST_CASE("SL570 吸附:随机输入 —— 首尾不动、首尾相接、不交叉、除独段外都不短于最短片段、切点都在边界/停顿/栅格上",
          "[analysis][lead][sl570]")
{
    // 手写 LCG:std 的分布在各平台实现不同,种子相同也可能抽出不同的数。
    std::uint64_t state = 0x5CB570u;
    const auto next = [&state](std::int64_t mod) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::int64_t>((state >> 33) % static_cast<std::uint64_t>(mod));
    };
    for (int iter = 0; iter < 400; ++iter)
    {
        const std::int64_t hi = 20000 + next(20000);
        std::vector<std::int64_t> bounds{0};
        while (bounds.back() < hi)
        {
            bounds.push_back(std::min(hi, bounds.back() + 150 + next(4000))); // 区间 >= 150,与 §3.4 同
        }
        std::vector<std::int64_t> pauses;
        for (std::int64_t p = next(3000); p < hi; p += 100 + next(3000))
        {
            pauses.push_back(p);
        }
        std::vector<LeadPiece> pieces;
        for (std::int64_t t = 0; t < hi;)
        {
            const std::int64_t len = 1 + next(next(2) == 0 ? 300 : 3000); // 一半是短抖动
            const std::int64_t t1 = std::min(hi, t + len);
            int v = static_cast<int>(next(4));
            if (!pieces.empty() && v == pieces.back().lead)
            {
                v = (v + 1) % 4;
            }
            pieces.push_back(LeadPiece{t, t1, v});
            t = t1;
        }
        const auto out = snapLeadSwitches(pieces, bounds, fixedPauses(pauses), kMs);
        INFO("iter " << iter << " out: " << describePieces(out));
        REQUIRE_FALSE(out.empty());
        CHECK(out.front().t0 == 0);
        CHECK(out.back().t1 == hi);
        for (std::size_t k = 0; k < out.size(); ++k)
        {
            CHECK(out[k].t0 < out[k].t1);
            if (out.size() > 1)
            {
                CHECK(out[k].t1 - out[k].t0 >= kMs.minPieceSamples);
            }
            if (k > 0)
            {
                CHECK(out[k].t0 == out[k - 1].t1);
                CHECK(out[k].lead != out[k - 1].lead);
                const std::int64_t c = out[k].t0;
                const bool onBound = std::binary_search(bounds.begin(), bounds.end(), c);
                const bool onPause = std::binary_search(pauses.begin(), pauses.end(), c);
                CHECK((onBound || onPause || c % kMs.hopSamples == 0));
            }
        }
    }
}

TEST_CASE("SL545 时间线:同值不同来源不合并;覆盖连来源一起换;runs / runsOverlapping / assign 带着来源",
          "[analysis][lead][sl545]")
{
    LeadTimeline tl;
    CHECK(tl.write(0, 100, 3, /*automated=*/false));
    CHECK(tl.write(100, 200, 3, /*automated=*/true)); // 相接同值、来源不同 → 不合并
    CHECK(tl.write(200, 300, 3, /*automated=*/true)); // 相接同值同来源 → 并进上一段
    auto runs = tl.runs();
    REQUIRE(runs.size() == 2);
    CHECK((runs[0].t0 == 0 && runs[0].t1 == 100 && !runs[0].automated));
    CHECK((runs[1].t0 == 100 && runs[1].t1 == 300 && runs[1].automated));

    // 插件写的同值盖进宿主那段中间:劈开,两端仍是宿主来源(后写覆盖先写,来源跟着值走)。
    CHECK(tl.write(150, 250, 3, /*automated=*/false));
    runs = tl.runs();
    REQUIRE(runs.size() == 4);
    CHECK((runs[1].t0 == 100 && runs[1].t1 == 150 && runs[1].automated));
    CHECK((runs[2].t0 == 150 && runs[2].t1 == 250 && !runs[2].automated));
    CHECK((runs[3].t0 == 250 && runs[3].t1 == 300 && runs[3].automated));

    // 再用插件来源盖掉 [100,150):左右都是插件来源的同值 → 三段并成一段。
    CHECK(tl.write(100, 150, 3, /*automated=*/false));
    runs = tl.runs();
    REQUIRE(runs.size() == 2);
    CHECK((runs[0].t0 == 0 && runs[0].t1 == 250 && !runs[0].automated));
    CHECK((runs[1].t0 == 250 && runs[1].t1 == 300 && runs[1].automated));

    const auto ov = tl.runsOverlapping(240, 260);
    REQUIRE(ov.size() == 2);
    CHECK_FALSE(ov[0].automated);
    CHECK(ov[1].automated);

    LeadTimeline back;
    back.assign(runs);
    const auto again = back.runs();
    REQUIRE(again.size() == runs.size());
    for (std::size_t i = 0; i < runs.size(); ++i)
    {
        CHECK(again[i].automated == runs[i].automated);
    }
}

TEST_CASE("SL545 写入来源:监听器在插件作用域外被调 = 宿主,作用域内 = 插件;作用域一开就预置成插件",
          "[analysis][lead][sl545]")
{
    LeadWriteOrigin o;
    CHECK_FALSE(o.byHost()); // 起始:还没人改过它(默认值 / 刚构造)不是自动化

    o.noteValueChanged(); // 宿主那一路:值变了、没有插件写在进行
    CHECK(o.byHost());

    {
        const LeadWriteOrigin::ScopedPluginWrite w(o, /*active=*/true);
        // 预置:插件这一笔的值还没落地、监听器还没来,音频线程此刻读到的来源已经是插件。
        CHECK_FALSE(o.byHost());
        o.noteValueChanged(); // 插件自己这一笔触发的监听器
        CHECK_FALSE(o.byHost());
        {
            const LeadWriteOrigin::ScopedPluginWrite nested(o, /*active=*/true); // 载入里再套一层
            o.noteValueChanged();
        }
        o.noteValueChanged(); // 内层退出后外层仍在:还是插件
        CHECK_FALSE(o.byHost());
    }
    CHECK_FALSE(o.byHost()); // 作用域退出不改来源:值仍是插件写的那个

    o.noteValueChanged(); // 作用域外再有人改 ⇒ 宿主
    REQUIRE(o.byHost());
    {
        // active = false(写的不是 lead_select):什么都不做 —— 不预置,监听器按宿主算。
        const LeadWriteOrigin::ScopedPluginWrite off(o, /*active=*/false);
        CHECK(o.byHost());
        o.noteValueChanged();
        CHECK(o.byHost());
    }
}

TEST_CASE("SL216 记录队列:排干进时间线;满了丢并计数", "[analysis][lead][sl216]")
{
    LeadRecorder rec;
    LeadTimeline tl;
    for (int b = 0; b < 10; ++b)
    {
        rec.record(b * 64, (b + 1) * 64, 7, /*automated=*/false);
    }
    CHECK(rec.drainInto(tl) == 10);
    REQUIRE(tl.size() == 1);
    CHECK(tl.runs()[0].t1 == 640);
    // [SL-545] 来源随块走进时间线:同值、宿主写的那几块另起一段。
    for (int b = 10; b < 15; ++b)
    {
        rec.record(b * 64, (b + 1) * 64, 7, /*automated=*/true);
    }
    CHECK(rec.drainInto(tl) == 5);
    REQUIRE(tl.size() == 2);
    CHECK_FALSE(tl.runs()[0].automated);
    CHECK((tl.runs()[1].t0 == 640 && tl.runs()[1].t1 == 960 && tl.runs()[1].automated));

    for (std::uint32_t i = 0; i < LeadRecorder::kCapacity + 5; ++i)
    {
        rec.record(0, 1, 1, false);
    }
    CHECK(rec.droppedRecords() == 5);
    CHECK(rec.discard() == LeadRecorder::kCapacity);
    CHECK(rec.drainInto(tl) == 0);
}

// ---------------------------------------------------------------------------
// LEAD 编解码
// ---------------------------------------------------------------------------

TEST_CASE("SL216 LEAD 往返逐字段一致(含 [SL-545] 来源位)", "[analysis][lead][sl216][sl545][state]")
{
    const std::vector<LeadRun> runs{{0, 48000, 0, false}, {48000, 96000, 3, true}, {200000, 300000, 15, true}};
    std::vector<std::uint8_t> bytes;
    encodeLeadChunk(runs, bytes);
    CHECK(bytes.size() == kLeadChunkHeaderBytes + 3 * kLeadChunkRecordBytes);
    std::vector<LeadRun> back;
    REQUIRE(decodeLeadChunk(bytes.data(), bytes.size(), back) == LeadDecodeStatus::Ok);
    REQUIRE(back.size() == runs.size());
    for (std::size_t i = 0; i < runs.size(); ++i)
    {
        CHECK(back[i].t0 == runs[i].t0);
        CHECK(back[i].t1 == runs[i].t1);
        CHECK(back[i].lead == runs[i].lead);
        CHECK(back[i].automated == runs[i].automated);
    }
    CHECK(bytes[0] == 2); // minor 2(本构建写的格式)
}

TEST_CASE("SL545 LEAD minor 1(SL-216 的旧格式,没有来源位)照读,一律读成插件写的", "[analysis][lead][sl545][state]")
{
    const auto legacy = legacyLeadChunk({{0, 100, 1}, {100, 200, 2}});
    REQUIRE(legacy.size() == kLeadChunkHeaderBytes + 2 * kLeadChunkRecordBytesMinor1);
    std::vector<LeadRun> out;
    REQUIRE(decodeLeadChunk(legacy.data(), legacy.size(), out) == LeadDecodeStatus::Ok);
    REQUIRE(out.size() == 2);
    CHECK((out[0].t0 == 0 && out[0].t1 == 100 && out[0].lead == 1 && !out[0].automated));
    CHECK((out[1].t0 == 100 && out[1].t1 == 200 && out[1].lead == 2 && !out[1].automated));

    // 记录长度按 minor 走:minor 1 的头配 24 字节记录、minor 2 的头配 20 字节记录,都是长度不符。
    std::vector<std::uint8_t> v2;
    encodeLeadChunk({{0, 100, 1, false}, {100, 200, 2, true}}, v2);
    auto v2AsV1 = v2;
    v2AsV1[0] = 1;
    CHECK(decodeLeadChunk(v2AsV1.data(), v2AsV1.size(), out) == LeadDecodeStatus::Malformed);
    auto v1AsV2 = legacy;
    v1AsV2[0] = 2;
    CHECK(decodeLeadChunk(v1AsV2.data(), v1AsV2.size(), out) == LeadDecodeStatus::Malformed);
}

TEST_CASE("SL216 LEAD 坏块整块不用;更高 minor 单列", "[analysis][lead][sl216][state]")
{
    const std::vector<LeadRun> runs{{0, 100, 1}, {100, 200, 2}};
    std::vector<std::uint8_t> good;
    encodeLeadChunk(runs, good);
    std::vector<LeadRun> out;

    SECTION("长度与条数不符")
    {
        auto b = good;
        b.pop_back();
        CHECK(decodeLeadChunk(b.data(), b.size(), out) == LeadDecodeStatus::Malformed);
    }
    SECTION("值越界")
    {
        auto b = good;
        b[kLeadChunkHeaderBytes + kLeadChunkRecordBytes + 16] = 16; // 第二条 lead = 16
        CHECK(decodeLeadChunk(b.data(), b.size(), out) == LeadDecodeStatus::Malformed);
    }
    SECTION("重叠/倒序")
    {
        auto b = good;
        b[kLeadChunkHeaderBytes + kLeadChunkRecordBytes] = 50; // 第二条 t0 = 50 < 前一条 t1
        CHECK(decodeLeadChunk(b.data(), b.size(), out) == LeadDecodeStatus::Malformed);
    }
    SECTION("flags 里有未定义的位([SL-545])")
    {
        auto b = good;
        b[kLeadChunkHeaderBytes + 20] = 2; // 第一条 flags = 2
        CHECK(decodeLeadChunk(b.data(), b.size(), out) == LeadDecodeStatus::Malformed);
    }
    SECTION("更高 minor")
    {
        auto b = good;
        b[0] = static_cast<std::uint8_t>(kLeadChunkMinor + 1);
        CHECK(decodeLeadChunk(b.data(), b.size(), out) == LeadDecodeStatus::NewerMinor);
    }
    SECTION("短于头")
    {
        CHECK(decodeLeadChunk(good.data(), 4, out) == LeadDecodeStatus::Malformed);
    }
    CHECK(out.empty());
}

// ---------------------------------------------------------------------------
// 管线:主唱进槽位/平衡计算
// ---------------------------------------------------------------------------

TEST_CASE("SL216 管线:没有记录 → 优先级最低的轨 1 拿中心(前提:可区分)", "[analysis][lead][sl216][pipeline]")
{
    ThreeVoices v;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    CHECK(allPansAre(res.segments[0], 0.0));
    CHECK_FALSE(allPansAre(res.segments[1], 0.0));
}

TEST_CASE("SL216 管线:宿主记录选中轨 2 → 轨 2 居中,另两轨在两侧对称排开", "[analysis][lead][sl216][pipeline]")
{
    ThreeVoices v;
    // [SL-545 / J143b] 只有宿主写的记录(automated)进分析;当前值留 0,轨 2 只能来自记录。
    v.cfg.leadRuns = {{v.cfg.rangeStartSample, v.cfg.rangeEndSample, 2, true}};
    const auto res = runAnalysisPipeline(v.features, v.cfg);

    CHECK(allPansAre(res.segments[1], 0.0)); // 主唱:每一段都在正中
    REQUIRE(res.segments[0].size() == res.segments[2].size());
    REQUIRE_FALSE(res.segments[0].empty());
    for (std::size_t i = 0; i < res.segments[0].size(); ++i)
    {
        const double p0 = res.segments[0][i].pan;
        const double p2 = res.segments[2][i].pan;
        // 两条自由轨 → 偶数分支 K=1 → 槽 {−60,+60}(02 §5.2):不再有人占中心,且左右对称。
        CHECK(std::abs(std::abs(p0) - 60.0) < 1e-9);
        CHECK(std::abs(p0 + p2) < 1e-9);
    }
}

TEST_CASE("SL216 管线:lead_select 选中与 lead_lock 走同一条 C 路径(pan 与 vol 逐位同解)",
          "[analysis][lead][sl216][pipeline]")
{
    // 三轨能量不等:平衡要真的出 u,才验得到「平衡也把主唱当居中轨算」。
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = makeAlternating(4, 80, 60, 0.02f);
    features[1] = makeAlternating(4, 80, 60, 0.05f);
    features[2] = makeAlternating(4, 80, 60, 0.09f);
    // 主唱挑一条**没有主唱时不在中心**的轨,否则「选中」与「锁定」两边都是它本来的居中排布,
    // 逐位相等不说明任何事(本条第一版选了轨 3,而轨 3 本来就在中心 —— 删掉管线那一行照样绿)。
    const auto base = runAnalysisPipeline(features, makeConfig(features[0].kwMs.size(), 3));
    int leadIdx = -1;
    for (int t = 0; t < 3 && leadIdx < 0; ++t)
    {
        if (!allPansAre(base.segments[static_cast<std::size_t>(t)], 0.0))
        {
            leadIdx = t;
        }
    }
    REQUIRE(leadIdx >= 0);
    auto cfgSelect = makeConfig(features[0].kwMs.size(), 3);
    // [SL-545 / J143b] 宿主写的记录;当前值留 0,选中只能来自记录。
    cfgSelect.leadRuns = {{cfgSelect.rangeStartSample, cfgSelect.rangeEndSample, leadIdx + 1, true}};
    auto cfgLock = makeConfig(features[0].kwMs.size(), 3);
    cfgLock.tracks[static_cast<std::size_t>(leadIdx)].leadLock = true;

    const auto a = runAnalysisPipeline(features, cfgSelect);
    const auto b = runAnalysisPipeline(features, cfgLock);
    bool anyU = false;
    for (int t = 0; t < 3; ++t)
    {
        const auto& sa = a.segments[static_cast<std::size_t>(t)];
        const auto& sb = b.segments[static_cast<std::size_t>(t)];
        REQUIRE(sa.size() == sb.size());
        for (std::size_t i = 0; i < sa.size(); ++i)
        {
            CHECK(sa[i].t0Samples == sb[i].t0Samples);
            CHECK(sa[i].t1Samples == sb[i].t1Samples);
            CHECK(sa[i].pan == sb[i].pan);
            CHECK(sa[i].volDb == sb[i].volDb);
            anyU = anyU || std::abs(sa[i].volDb) > 1e-6;
        }
    }
    CHECK(allPansAre(a.segments[static_cast<std::size_t>(leadIdx)], 0.0));
    CHECK(anyU); // 前提:平衡确实动了音量,上面的 vol 逐位相等不是「两边都是 0」
}

TEST_CASE("SL216 管线:记录全是 0 → 与没有记录逐位同解", "[analysis][lead][sl216][pipeline]")
{
    ThreeVoices v;
    const auto base = runAnalysisPipeline(v.features, v.cfg);
    v.cfg.leadRuns = {{v.cfg.rangeStartSample, v.cfg.rangeEndSample, 0, true}}; // 宿主记录了 0 = 无主唱
    const auto zero = runAnalysisPipeline(v.features, v.cfg);
    for (int t = 0; t < 3; ++t)
    {
        const auto& sa = base.segments[static_cast<std::size_t>(t)];
        const auto& sb = zero.segments[static_cast<std::size_t>(t)];
        REQUIRE(sa.size() == sb.size());
        for (std::size_t i = 0; i < sa.size(); ++i)
        {
            CHECK(sa[i].pan == sb[i].pan);
            CHECK(sa[i].volDb == sb[i].volDb);
        }
    }
}

TEST_CASE("SL216 管线:逐区间换主唱 —— 前半轨 1、后半轨 3", "[analysis][lead][sl216][pipeline]")
{
    ThreeVoices v;
    v.cfg.tracks[0].priority = 5.0; // 本条不需要「默认中心」的前提
    // 4 轮 × 140 hop:前两轮与后两轮的分界恰在第 2 轮静音之后(280 hop)。
    const std::int64_t half = 280 * kHopSamples;
    v.cfg.leadRuns = {{0, half, 1, true}, {half, v.cfg.rangeEndSample, 3, true}}; // 宿主自动化:前半 1、后半 3
    const auto res = runAnalysisPipeline(v.features, v.cfg);

    int firstHalfChecked = 0;
    int secondHalfChecked = 0;
    for (const auto& s : res.segments[0])
    {
        if (s.t1Samples <= half)
        {
            CHECK(s.pan == 0.0);
            ++firstHalfChecked;
        }
        else if (s.t0Samples >= half)
        {
            CHECK(s.pan != 0.0);
        }
    }
    for (const auto& s : res.segments[2])
    {
        if (s.t0Samples >= half)
        {
            CHECK(s.pan == 0.0);
            ++secondHalfChecked;
        }
        else if (s.t1Samples <= half)
        {
            CHECK(s.pan != 0.0);
        }
    }
    CHECK(firstHalfChecked > 0);
    CHECK(secondHalfChecked > 0);
}

TEST_CASE("SL216 管线:选中的轨在该区间不活跃 → 不影响其余轨", "[analysis][lead][sl216][pipeline]")
{
    ThreeVoices v;
    const auto base = runAnalysisPipeline(v.features, v.cfg);
    v.cfg.leadRuns = {{v.cfg.rangeStartSample, v.cfg.rangeEndSample, 9, true}}; // 轨 9 没有素材
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    for (int t = 0; t < 3; ++t)
    {
        const auto& sa = base.segments[static_cast<std::size_t>(t)];
        const auto& sb = res.segments[static_cast<std::size_t>(t)];
        REQUIRE(sa.size() == sb.size());
        for (std::size_t i = 0; i < sa.size(); ++i)
        {
            CHECK(sa[i].pan == sb[i].pan);
        }
    }
    CHECK(res.segments[8].empty());
}

// ---------------------------------------------------------------------------
// [SL-545 / J143 + J143b] 点分析那一刻的 lead_select(cfg.leadFallback)
//
// J143b(统筹 2026-09-28,取代 J143a「记录里 ≥ 2 个不同的值才算自动化」):自动化证据按**写入来源**判 ——
// 只有宿主写进来的值(`LeadRun::automated`)算。窗里一条宿主记录都没有 ⇒ 整窗用当前值;有 ⇒ 宿主记录
// 盖到的地方按记录,盖不到的地方用当前值。插件自己写的记录(界面 / 撤销 / 载入 / 采集时从没改过)
// 一律不看。任务单点名的四格:① ② ③ ④ 标在标题里。
// 本机台只有三条轨有素材(ThreeVoices),所以「当前值」一律取 1..3 里的某一轨 —— 选一条没有素材的轨
// 当当前值,「它居中」在段表里看不出来,与「没有主唱」无法区分。
// ---------------------------------------------------------------------------

namespace
{
// 各段的分界都切在两轮之间的**静音缝**里,这样没有哪个段、哪个区间横跨分界 —— 分界落在一句中间时怎么切
// 是另一件事([SL-570] 那几条),这里只钉「整段是某个值 / 整段没有记录」的区间。[SL-570] 起分界本身是切换点、
// 会吸附到 1 s 内最近的区间边界 —— 落在静音缝里的分界吸到的正是缝的两端之一,缝里没有活跃轨,结果不变。
//   ThreeVoices = 4 轮 × (80 hop 发声 + 60 hop 静音):第 1、2 轮之间的缝约为 hop 240..268(段含前后 padding),
//   第 2、3 轮之间约为 hop 380..408。前提由 noSegmentStraddles 当场核,不靠这段注释。
constexpr std::int64_t kCutA = 254 * kHopSamples;
constexpr std::int64_t kCutB = 394 * kHopSamples;

bool noSegmentStraddles(const PipelineResult& res, std::int64_t cut)
{
    for (int t = 0; t < 3; ++t)
    {
        for (const auto& s : res.segments[static_cast<std::size_t>(t)])
        {
            if (s.t0Samples < cut && s.t1Samples > cut)
            {
                return false;
            }
        }
    }
    return true;
}

// [t0, t1) 里完整落进去的段:pan 全是 0 ⇒ 返回段数(> 0);有一段不是 0 或一段都没有 ⇒ 0 / −1。
// 用段数而不是 bool,让调用处能同时断言「确实查到了段」。
int centeredCountIn(const std::vector<AnalysisSegment>& segs, std::int64_t t0, std::int64_t t1)
{
    int n = 0;
    for (const auto& s : segs)
    {
        if (s.t0Samples >= t0 && s.t1Samples <= t1)
        {
            if (s.pan != 0.0)
            {
                return -1;
            }
            ++n;
        }
    }
    return n;
}

// [t0, t1) 里完整落进去的段,有几段离开了正中。
int offCenterCountIn(const std::vector<AnalysisSegment>& segs, std::int64_t t0, std::int64_t t1)
{
    int n = 0;
    for (const auto& s : segs)
    {
        if (s.t0Samples >= t0 && s.t1Samples <= t1 && s.pan != 0.0)
        {
            ++n;
        }
    }
    return n;
}

void checkSameLayout(const PipelineResult& a, const PipelineResult& b)
{
    for (int t = 0; t < 3; ++t)
    {
        const auto& sa = a.segments[static_cast<std::size_t>(t)];
        const auto& sb = b.segments[static_cast<std::size_t>(t)];
        REQUIRE(sa.size() == sb.size());
        for (std::size_t i = 0; i < sa.size(); ++i)
        {
            INFO("track " << (t + 1) << " seg " << i);
            CHECK(sa[i].t0Samples == sb[i].t0Samples);
            CHECK(sa[i].pan == sb[i].pan);
            CHECK(sa[i].volDb == sb[i].volDb);
        }
    }
}
} // namespace

TEST_CASE(
    "SL545 管线:记录全是 0(采集,插件写的)、当前值 = 3 → 轨 3 居中,另两轨在两侧对称排开(采集后改了 Lead Select 不重播)",
    "[analysis][lead][sl545][pipeline]")
{
    // 用户那条路:采集时 Lead Select = 0(采集就是走带播放,于是整窗记下的都是 0 —— 采集期间没人改过它,
    // 来源 = 插件),采完改成 3,不重播直接点分析。没有宿主写的记录 ⇒ 整窗按当前值 3。
    ThreeVoices v;
    REQUIRE_FALSE(allPansAre(runAnalysisPipeline(v.features, v.cfg).segments[2], 0.0)); // 前提:轨 3 本来不居中
    v.cfg.leadRuns = {{v.cfg.rangeStartSample, v.cfg.rangeEndSample, 0, false}};
    v.cfg.leadFallback = 3;
    const auto res = runAnalysisPipeline(v.features, v.cfg);

    CHECK(allPansAre(res.segments[2], 0.0)); // ★
    REQUIRE(res.segments[0].size() == res.segments[1].size());
    REQUIRE_FALSE(res.segments[0].empty());
    for (std::size_t i = 0; i < res.segments[0].size(); ++i)
    {
        const double p0 = res.segments[0][i].pan;
        const double p1 = res.segments[1][i].pan;
        // 两条自由轨 → 偶数分支 K=1 → 槽 {−60,+60}(02 §5.2):中心让给主唱,其余左右对称。
        CHECK(std::abs(std::abs(p0) - 60.0) < 1e-9);
        CHECK(std::abs(p0 + p1) < 1e-9);
    }
}

TEST_CASE("SL545 管线:J143b ① 记录全是 0、界面改成 3 后试听一段(B 段记成 3)、当前值 = 3 → 整窗轨 3 居中",
          "[analysis][lead][sl545][pipeline]")
{
    // #317 复审【重要】1 的情形:采集把整窗记成 0;用户在**插件界面**把 Lead Select 改成 3,试听了 B 段
    // (B 段于是记成 3),再点分析。三段记录都是插件写的 ⇒ 没有自动化证据 ⇒ 整窗按当前值 3,与「没有记录、
    // 当前值 3」逐位同解。J143a 按「窗里有 0 和 3 两个值」把它判成自动化:只有 B 段轨 3 居中,A、C 按 0。
    ThreeVoices v;
    v.cfg.leadRuns = {{0, kCutA, 0, false}, {kCutA, kCutB, 3, false}, {kCutB, v.cfg.rangeEndSample, 0, false}};
    v.cfg.leadFallback = 3;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    REQUIRE(noSegmentStraddles(res, kCutA));
    REQUIRE(noSegmentStraddles(res, kCutB));

    // ★ 不只是试听过的 B 段:A、C 段轨 3 也居中。
    CHECK(centeredCountIn(res.segments[2], 0, kCutA) > 0);
    CHECK(centeredCountIn(res.segments[2], kCutA, kCutB) > 0);
    CHECK(centeredCountIn(res.segments[2], kCutB, v.cfg.rangeEndSample) > 0);
    ThreeVoices none;
    none.cfg.leadFallback = 3;
    checkSameLayout(res, runAnalysisPipeline(none.features, none.cfg));
}

TEST_CASE("SL545 管线:J143b ② 宿主记录 A 段 0、B 段 3,C 段没有记录,当前值 = 2 → A 无主唱、B 轨 3、C 轨 2",
          "[analysis][lead][sl545][pipeline]")
{
    // 真自动化(宿主写进来的 0 和 3)⇒ 宿主记录盖到的地方按记录,当前值只补盖不到的 C 段。
    // 刻意自动化成 0 的 A 段(合唱句 / 无主唱)不会被当前值盖掉。
    ThreeVoices v;
    v.cfg.leadRuns = {{0, kCutA, 0, true}, {kCutA, kCutB, 3, true}};
    v.cfg.leadFallback = 2;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    REQUIRE(noSegmentStraddles(res, kCutA));
    REQUIRE(noSegmentStraddles(res, kCutB));
    const std::int64_t end = v.cfg.rangeEndSample;

    // A:宿主记录了 0 ⇒ 无主唱 ⇒ 优先级最低的轨 1 拿中心(与「没有记录」那条同一个前提),轨 2 不居中。
    CHECK(centeredCountIn(res.segments[0], 0, kCutA) > 0);
    CHECK(offCenterCountIn(res.segments[1], 0, kCutA) > 0);
    // B:宿主记录了 3 ⇒ 轨 3 居中;当前值 2 压不过记录。
    CHECK(centeredCountIn(res.segments[2], kCutA, kCutB) > 0);
    CHECK(offCenterCountIn(res.segments[1], kCutA, kCutB) > 0);
    // C:没有记录 ⇒ 当前值 2。
    CHECK(centeredCountIn(res.segments[1], kCutB, end) > 0);
    CHECK(offCenterCountIn(res.segments[0], kCutB, end) > 0);
    CHECK(offCenterCountIn(res.segments[2], kCutB, end) > 0);
}

TEST_CASE(
    "SL545 管线:J143b ③ 宿主记录 A 段 3、之后界面改成 2 试听 B 段、C 段是采集时的 0,当前值 = 2 → A 轨 3、B 与 C 轨 2",
    "[analysis][lead][sl545][pipeline]")
{
    // 混合:宿主自动化在 A 段写了 3(有自动化证据);用户随后在插件界面改成 2、试听了 B 段;C 段还是采集时
    // 记下的 0(插件)。宿主记录盖到的 A 段按记录;B、C 两段只有插件写的记录 ⇒ 不看,取当前值 2。
    // J143a(窗里 {3, 2, 0} ≥ 2 个值 ⇒ 全按记录)会让 C 段按 0 无主唱 —— C 段就是这一格的判别点。
    ThreeVoices v;
    v.cfg.leadRuns = {{0, kCutA, 3, true}, {kCutA, kCutB, 2, false}, {kCutB, v.cfg.rangeEndSample, 0, false}};
    v.cfg.leadFallback = 2;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    REQUIRE(noSegmentStraddles(res, kCutA));
    REQUIRE(noSegmentStraddles(res, kCutB));
    const std::int64_t end = v.cfg.rangeEndSample;

    // A:宿主记录 3 ⇒ 轨 3 居中,当前值 2 压不过它。
    CHECK(centeredCountIn(res.segments[2], 0, kCutA) > 0);
    CHECK(offCenterCountIn(res.segments[1], 0, kCutA) > 0);
    // B:插件写的 2 不看、当前值也是 2 ⇒ 轨 2 居中。
    CHECK(centeredCountIn(res.segments[1], kCutA, kCutB) > 0);
    CHECK(offCenterCountIn(res.segments[2], kCutA, kCutB) > 0);
    // C:★ 插件写的 0 不看 ⇒ 当前值 2,轨 2 居中;轨 1(无主唱时拿中心的那条)离开正中。
    CHECK(centeredCountIn(res.segments[1], kCutB, end) > 0);
    CHECK(offCenterCountIn(res.segments[0], kCutB, end) > 0);
    CHECK(offCenterCountIn(res.segments[2], kCutB, end) > 0);
}

TEST_CASE("SL545 管线:J143b 宿主记录恒为轨 3、当前值 = 0 → 仍按记录轨 3 居中(只有一个值的自动化也算)",
          "[analysis][lead][sl545][pipeline]")
{
    // 自动化整首都是 3(或只在已播过的部分是 3),停带后宿主 / 用户把旋钮放回 0:宿主写的记录就是证据,按它。
    // J143a 按「只有一个值 ⇒ 不算自动化」取当前值 0 —— 这一格与下面「插件写的记录恒为轨 3」只差来源位。
    ThreeVoices v;
    v.cfg.leadRuns = {{v.cfg.rangeStartSample, v.cfg.rangeEndSample, 3, true}};
    v.cfg.leadFallback = 0;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    CHECK(allPansAre(res.segments[2], 0.0)); // ★
    CHECK_FALSE(allPansAre(res.segments[0], 0.0)); // 轨 1 不再拿中心
}

TEST_CASE("SL545 管线:插件写的记录恒为轨 3、当前值 = 2 → 轨 2 居中、轨 3 不居中(用户最后一次的设定就是意图)",
          "[analysis][lead][sl545][pipeline]")
{
    // 在插件界面选 3、播一遍,播完改成 2、不重播:插件写的记录不看 ⇒ 按 2。
    ThreeVoices v;
    v.cfg.leadRuns = {{v.cfg.rangeStartSample, v.cfg.rangeEndSample, 3, false}};
    v.cfg.leadFallback = 2;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    CHECK(allPansAre(res.segments[1], 0.0)); // ★ 轨 2
    CHECK_FALSE(allPansAre(res.segments[2], 0.0)); // 记录里的 3 没有说了算
}

TEST_CASE("SL545 管线:插件写的记录恒为轨 3、当前值 = 0 → 无主唱,与没有记录逐位同解",
          "[analysis][lead][sl545][pipeline]")
{
    // 在插件界面选 3、播一遍,之后把 Lead Select 改回 0(不要主唱):按 0。
    ThreeVoices none;
    const auto base = runAnalysisPipeline(none.features, none.cfg);
    ThreeVoices v;
    v.cfg.leadRuns = {{v.cfg.rangeStartSample, v.cfg.rangeEndSample, 3, false}};
    v.cfg.leadFallback = 0;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    CHECK_FALSE(allPansAre(res.segments[2], 0.0)); // 前提:轨 3 在「没有主唱」时本来不居中
    checkSameLayout(res, base);
}

TEST_CASE("SL545 管线:没有记录(旧工程)、当前值 = 2 → 与 lead_lock 轨 2 逐位同解", "[analysis][lead][sl545][pipeline]")
{
    // SL-216 之前存的工程:特征在、LEAD 块不在 ⇒ 窗里没有宿主记录 ⇒ 整窗用当前值。
    // 与 lead_lock 逐位同解 = 当前值走的就是记录那条 C 路径(恒居中、不占槽、平衡把它当居中轨),不是另一套近似。
    ThreeVoices v;
    REQUIRE(v.cfg.leadRuns.empty());
    v.cfg.leadFallback = 2;
    ThreeVoices lock;
    lock.cfg.tracks[1].leadLock = true;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    CHECK(allPansAre(res.segments[1], 0.0));
    checkSameLayout(res, runAnalysisPipeline(lock.features, lock.cfg));
}

TEST_CASE("SL545 管线:J143b ④ minor 1 的 LEAD 块(没有来源位)读成插件写的 → 不算自动化,整窗按当前值",
          "[analysis][lead][sl545][pipeline][state]")
{
    // minor 1 是 SL-216 写的格式,那时不分来源。本卡的决定:一律读成 automated = false(不算自动化证据)——
    // 这一档从未随正式版发出,只有开发期存过的工程会带着它;把它当证据的话,开发期采集时记下的 0
    // 会压住用户选的主唱,正是 J143b 要修的那个坑。
    const auto legacy = legacyLeadChunk({{0, kCutB, 3}});
    std::vector<LeadRun> runs;
    REQUIRE(decodeLeadChunk(legacy.data(), legacy.size(), runs) == LeadDecodeStatus::Ok);
    REQUIRE(runs.size() == 1);
    REQUIRE_FALSE(runs[0].automated);

    ThreeVoices v;
    v.cfg.leadRuns = runs;
    v.cfg.leadFallback = 2;
    ThreeVoices none;
    none.cfg.leadFallback = 2;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    CHECK(allPansAre(res.segments[1], 0.0)); // 当前值 2
    checkSameLayout(res, runAnalysisPipeline(none.features, none.cfg));
}

// ---------------------------------------------------------------------------
// [SL-570 / J167] 管线:按有效主唱的切换点把区间切开(吸附后),子区间各按各的主唱
//
// 素材一律手搭(`makeShaped`):一条轨在给定的 hop 范围里发声(kw 恒定),其余静音;`dips` 里的 hop 压低 20 dB ——
// VAD 的开段门槛在活跃基准下 30 dB,压低 20 dB 不断段(用例里当场核「段数没变」),但合起来的包络上是一个
// 过得了 minDepth(灵敏度 50 ⇒ 6 dB)的谷,即「大家一起换气」。
// 所有「句首 / 边界」都从一次**没有主唱记录**的基线分析里读出来,不按 VAD 的 padding / hangover 手算。
// ---------------------------------------------------------------------------

namespace
{
constexpr std::size_t kShapedHops = 800; // 8 s;单段最长约 7.6 s,不触发 §3.2 超长段谷切分(8 s)

PipelineTrackFeatures makeShaped(const std::vector<std::pair<int, int>>& loud, float loudKw,
                                 const std::vector<std::pair<int, int>>& dips = {})
{
    PipelineTrackFeatures f;
    f.kwMs.assign(kShapedHops, 1e-9f);
    f.peak.assign(kShapedHops, 1e-5f);
    for (const auto& [a, b] : loud)
    {
        for (int h = a; h < b; ++h)
        {
            f.kwMs[static_cast<std::size_t>(h)] = loudKw;
            f.peak[static_cast<std::size_t>(h)] = std::sqrt(loudKw);
        }
    }
    for (const auto& [a, b] : dips)
    {
        for (int h = a; h < b; ++h)
        {
            f.kwMs[static_cast<std::size_t>(h)] *= 0.01f;
            f.peak[static_cast<std::size_t>(h)] *= 0.1f;
        }
    }
    f.covered.assign(kShapedHops, 1u);
    f.anyCovered = true;
    return f;
}

// 轨 t 上盖住样本 at 的那一段;没有 ⇒ nullptr。
const AnalysisSegment* segAt(const PipelineResult& r, int t, std::int64_t at)
{
    for (const auto& s : r.segments[static_cast<std::size_t>(t)])
    {
        if (s.t0Samples <= at && at < s.t1Samples)
        {
            return &s;
        }
    }
    return nullptr;
}

// 基线分析里所有轨的段边界(= 全局区间边界的超集:同活跃集合合并前的那些点也在里面)。
std::vector<std::int64_t> allSegmentEdges(const PipelineResult& r)
{
    std::vector<std::int64_t> out;
    for (const auto& segs : r.segments)
    {
        for (const auto& s : segs)
        {
            out.push_back(s.t0Samples);
            out.push_back(s.t1Samples);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// 前提:`at` 前后 1 s 内没有任何段边界(否则吸附会先吸边界,这一格测的就不是它要测的那一档)。
bool noEdgeWithinOneSecond(const PipelineResult& r, std::int64_t at)
{
    const std::int64_t radius = static_cast<std::int64_t>(std::llround(kLeadSnapRadiusS * kSr));
    for (const std::int64_t e : allSegmentEdges(r))
    {
        if (std::llabs(e - at) <= radius)
        {
            return false;
        }
    }
    return true;
}

bool sameSegments(const PipelineResult& a, const PipelineResult& b)
{
    for (int t = 0; t < kPipelineTracks; ++t)
    {
        const auto& sa = a.segments[static_cast<std::size_t>(t)];
        const auto& sb = b.segments[static_cast<std::size_t>(t)];
        if (sa.size() != sb.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < sa.size(); ++i)
        {
            if (sa[i].t0Samples != sb[i].t0Samples || sa[i].t1Samples != sb[i].t1Samples)
            {
                return false;
            }
        }
    }
    return true;
}

// 各轨段起点的并集有几个。没有切换点时每一段都始于某个全局区间的起点(相邻同值合并只会少不会多)⇒ 不超过全局区间数;
// 在没有切换点的区间里多切一刀,这个数就会超出。与下面的逐位比互补:逐位比的对照组(lead_lock)走的也是同一段切子区间的
// 代码,两边一起多切时逐位比照样相等 —— 这一条不经过那段代码。
std::size_t distinctSegmentStarts(const PipelineResult& r)
{
    std::vector<std::int64_t> starts;
    for (const auto& segs : r.segments)
    {
        for (const auto& s : segs)
        {
            starts.push_back(s.t0Samples);
        }
    }
    std::sort(starts.begin(), starts.end());
    return static_cast<std::size_t>(std::unique(starts.begin(), starts.end()) - starts.begin());
}

// 逐位比:每轨段数、每段 t0/t1/pan/volDb,以及区间数、平衡回退级、警告。
void checkBitIdentical(const PipelineResult& a, const PipelineResult& b)
{
    CHECK(a.intervals == b.intervals);
    CHECK(a.maxFallbackLevel == b.maxFallbackLevel);
    CHECK(a.warnings == b.warnings);
    for (int t = 0; t < kPipelineTracks; ++t)
    {
        const auto& sa = a.segments[static_cast<std::size_t>(t)];
        const auto& sb = b.segments[static_cast<std::size_t>(t)];
        INFO("track " << (t + 1));
        REQUIRE(sa.size() == sb.size());
        for (std::size_t i = 0; i < sa.size(); ++i)
        {
            INFO("seg " << i);
            CHECK(sa[i].t0Samples == sb[i].t0Samples);
            CHECK(sa[i].t1Samples == sb[i].t1Samples);
            CHECK(sa[i].pan == sb[i].pan);
            CHECK(sa[i].volDb == sb[i].volDb);
        }
    }
}
} // namespace

TEST_CASE("SL570 切子区间:没有切换点的区间原样成为一个子区间;切换点落在区间中间才切;恰在边界上不多切",
          "[analysis][lead][sl570]")
{
    const std::vector<GlobalInterval> iv{{0, 1000, {0, 1}}, {1000, 3000, {0}}, {3000, 4000, {1}}};
    // 一个片段(E 全程不变):逐个原样,主唱 = 那个值。
    auto w = splitIntervalsAtLeadSwitches(iv, {{0, 4000, 5}}, 0);
    REQUIRE(w.size() == 3);
    for (std::size_t i = 0; i < 3; ++i)
    {
        CHECK(w[i].interval == i);
        CHECK(w[i].t0 == iv[i].t0);
        CHECK(w[i].t1 == iv[i].t1);
        CHECK(w[i].lead == 5);
    }
    // 切换点 2000 落在第 2 个区间中间 ⇒ 它切成两段;切换点 3000 恰在边界 ⇒ 不多切。
    w = splitIntervalsAtLeadSwitches(iv, {{0, 2000, 1}, {2000, 3000, 2}, {3000, 4000, 3}}, 0);
    REQUIRE(w.size() == 4);
    CHECK((w[0].interval == 0 && w[0].t0 == 0 && w[0].t1 == 1000 && w[0].lead == 1));
    CHECK((w[1].interval == 1 && w[1].t0 == 1000 && w[1].t1 == 2000 && w[1].lead == 1));
    CHECK((w[2].interval == 1 && w[2].t0 == 2000 && w[2].t1 == 3000 && w[2].lead == 2));
    CHECK((w[3].interval == 2 && w[3].t0 == 3000 && w[3].t1 == 4000 && w[3].lead == 3));
    // 没给片段(调用方违约):每个区间原样,按回落值。
    w = splitIntervalsAtLeadSwitches(iv, {}, 7);
    REQUIRE(w.size() == 3);
    CHECK((w[1].t0 == 1000 && w[1].t1 == 3000 && w[1].lead == 7));
    // 片段中间有空档(调用方违约):空档那一截按回落值,只到下一个片段起点为止,之后照常按片段。
    w = splitIntervalsAtLeadSwitches(iv, {{0, 500, 1}, {1500, 4000, 3}}, 7);
    REQUIRE(w.size() == 5);
    CHECK((w[0].interval == 0 && w[0].t0 == 0 && w[0].t1 == 500 && w[0].lead == 1));
    CHECK((w[1].interval == 0 && w[1].t0 == 500 && w[1].t1 == 1000 && w[1].lead == 7));
    CHECK((w[2].interval == 1 && w[2].t0 == 1000 && w[2].t1 == 1500 && w[2].lead == 7));
    CHECK((w[3].interval == 1 && w[3].t0 == 1500 && w[3].t1 == 3000 && w[3].lead == 3)); // ★ 没被回落值吞掉
    CHECK((w[4].interval == 2 && w[4].t0 == 3000 && w[4].t1 == 4000 && w[4].lead == 3));
}

TEST_CASE("SL570 管线:一个区间里两人交替 —— 轨 1、轨 2、轨 1 各自那段居中,在换人处原位切开",
          "[analysis][lead][sl570][pipeline]")
{
    // 三轨一起唱满 7 s(一个全局区间,没有停顿)。自动化:轨 1 → 2.5 s 起轨 2 → 4.5 s 起回到轨 1。
    // J143 的口径下整个区间取多数值 = 轨 1(占 5 s 多),轨 2 一段都不居中 —— 用户说的「两人交替只有一个人赢」。
    std::array<PipelineTrackFeatures, kPipelineTracks> f;
    for (int t = 0; t < 3; ++t)
    {
        f[static_cast<std::size_t>(t)] = makeShaped({{50, 750}}, 0.05f);
    }
    PipelineConfig cfg = makeConfig(kShapedHops, 3);
    const auto base = runAnalysisPipeline(f, cfg);
    REQUIRE(base.intervals == 1); // 前提:一个区间
    const std::int64_t c1 = 250 * kHopSamples;
    const std::int64_t c2 = 450 * kHopSamples;
    REQUIRE(noEdgeWithinOneSecond(base, c1));
    REQUIRE(noEdgeWithinOneSecond(base, c2));

    cfg.leadRuns = {{0, c1, 1, true}, {c1, c2, 2, true}, {c2, cfg.rangeEndSample, 1, true}};
    const auto res = runAnalysisPipeline(f, cfg);
    const auto* a1 = segAt(res, 0, c1 - 1);
    const auto* a2 = segAt(res, 0, c1);
    const auto* a3 = segAt(res, 0, c2);
    const auto* b2 = segAt(res, 1, c1);
    REQUIRE((a1 && a2 && a3 && b2));
    // ★ 轨 2 在自己那段居中,段就切在换人处。
    CHECK(b2->pan == 0.0);
    CHECK(b2->t0Samples == c1);
    CHECK(b2->t1Samples == c2);
    // 轨 1 前后两段居中、中间那段让出中心。
    CHECK(a1->pan == 0.0);
    CHECK(a1->t1Samples == c1);
    CHECK(a2->pan != 0.0);
    CHECK(a3->pan == 0.0);
    CHECK(a3->t0Samples == c2);
    CHECK(res.intervals == base.intervals); // 子区间不计入全局区间数
}

TEST_CASE("SL570 管线:换人点画晚 0.5 s ⇒ 吸回句首,整句按新主唱", "[analysis][lead][sl570][pipeline]")
{
    // 轨 1 唱 0.5–2.5 s,轨 2 唱 4.5–7.5 s,轨 3 从头伴唱到尾。用户把「换成轨 2」画在轨 2 开口之后 0.5 s。
    std::array<PipelineTrackFeatures, kPipelineTracks> f;
    f[0] = makeShaped({{50, 250}}, 0.05f);
    f[1] = makeShaped({{450, 750}}, 0.05f);
    f[2] = makeShaped({{50, 750}}, 0.05f);
    PipelineConfig cfg = makeConfig(kShapedHops, 3);
    const auto base = runAnalysisPipeline(f, cfg);
    REQUIRE_FALSE(base.segments[1].empty());
    const std::int64_t sentence = base.segments[1].front().t0Samples; // 轨 2 的句首 = 一条全局区间边界
    const std::int64_t late = sentence + 50 * kHopSamples; // 画晚 0.5 s
    for (const std::int64_t e : allSegmentEdges(base))
    {
        INFO("edge " << e);
        CHECK((e == sentence || std::llabs(e - late) > late - sentence)); // 前提:句首是离画点最近的边界
    }
    REQUIRE(noEdgeWithinOneSecond(base, late + 60 * kHopSamples)); // 前提:句中没有别的边界

    cfg.leadRuns = {{0, late, 1, true}, {late, cfg.rangeEndSample, 2, true}};
    const auto res = runAnalysisPipeline(f, cfg);
    const auto* s = segAt(res, 1, sentence);
    REQUIRE(s != nullptr);
    CHECK(s->pan == 0.0); // ★ 句首那 0.5 s 也是轨 2 居中
    CHECK(s->t0Samples == sentence);
    CHECK(s->t1Samples > late); // 句中画点处没有另切一刀
}

TEST_CASE("SL570 管线:换人点画在最后一句结束前 0.5 s ⇒ 吸到句尾,这一句整句仍按原主唱",
          "[analysis][lead][sl570][pipeline]")
{
    // 窗里最后一个全局区间的终点也是段边界(最后一句的句尾),同样参与吸附。
    std::array<PipelineTrackFeatures, kPipelineTracks> f;
    for (int t = 0; t < 3; ++t)
    {
        f[static_cast<std::size_t>(t)] = makeShaped({{50, 750}}, 0.05f);
    }
    PipelineConfig cfg = makeConfig(kShapedHops, 3);
    const auto base = runAnalysisPipeline(f, cfg);
    REQUIRE(base.intervals == 1);
    REQUIRE_FALSE(base.segments[0].empty());
    const std::int64_t tail = base.segments[0].back().t1Samples; // 句尾
    const std::int64_t early = tail - 50 * kHopSamples; // 画早 0.5 s
    REQUIRE(noEdgeWithinOneSecond(base, early - 60 * kHopSamples)); // 前提:句中没有别的边界
    cfg.leadRuns = {{0, early, 1, true}, {early, cfg.rangeEndSample, 3, true}};
    const auto res = runAnalysisPipeline(f, cfg);
    const auto* s = segAt(res, 0, early);
    REQUIRE(s != nullptr);
    CHECK(s->pan == 0.0); // ★ 句尾那 0.5 s 仍是轨 1 居中
    CHECK(s->t1Samples == tail);
}

TEST_CASE("SL570 管线:长区间中段换人、附近没有边界也没有停顿 ⇒ 在原位(取整到 hop)切开",
          "[analysis][lead][sl570][pipeline]")
{
    std::array<PipelineTrackFeatures, kPipelineTracks> f;
    for (int t = 0; t < 3; ++t)
    {
        f[static_cast<std::size_t>(t)] = makeShaped({{50, 750}}, 0.05f);
    }
    PipelineConfig cfg = makeConfig(kShapedHops, 3);
    const auto base = runAnalysisPipeline(f, cfg);
    const std::int64_t cut = 400 * kHopSamples;
    REQUIRE(noEdgeWithinOneSecond(base, cut));
    // 换人时刻落在 hop 中间(宿主按块写,块边界不一定对齐 hop)。
    cfg.leadRuns = {{0, cut + 123, 1, true}, {cut + 123, cfg.rangeEndSample, 3, true}};
    const auto res = runAnalysisPipeline(f, cfg);
    const auto* before = segAt(res, 0, cut - 1);
    const auto* after = segAt(res, 2, cut);
    REQUIRE((before && after));
    CHECK(before->pan == 0.0);
    CHECK(before->t1Samples == cut); // ★ 取整到 hop 栅格
    CHECK(after->pan == 0.0);
    CHECK(after->t0Samples == cut);
}

TEST_CASE("SL570 管线:1 s 内没有边界但有大家一起换气的停顿 ⇒ 切在停顿处", "[analysis][lead][sl570][pipeline]")
{
    std::array<PipelineTrackFeatures, kPipelineTracks> f;
    for (int t = 0; t < 3; ++t)
    {
        f[static_cast<std::size_t>(t)] = makeShaped({{50, 750}}, 0.05f, {{330, 340}});
    }
    PipelineConfig cfg = makeConfig(kShapedHops, 3);
    const auto base = runAnalysisPipeline(f, cfg);
    REQUIRE(base.intervals == 1); // 前提:压低 20 dB 没有断段
    const std::int64_t drawn = 400 * kHopSamples; // 画在停顿之后 0.6–0.7 s
    REQUIRE(noEdgeWithinOneSecond(base, drawn));
    cfg.leadRuns = {{0, drawn, 1, true}, {drawn, cfg.rangeEndSample, 3, true}};
    const auto res = runAnalysisPipeline(f, cfg);
    const auto* after = segAt(res, 2, drawn);
    REQUIRE(after != nullptr);
    CHECK(after->pan == 0.0);
    INFO("cut at hop " << after->t0Samples / kHopSamples);
    CHECK(after->t0Samples >= 330 * kHopSamples); // ★ 切在停顿里,不在画点
    CHECK(after->t0Samples < 340 * kHopSamples);
}

TEST_CASE("SL570 管线:只有一条轨歇着、别的轨还在唱 ⇒ 那里不算停顿,原位切", "[analysis][lead][sl570][pipeline]")
{
    // 与上一条同一个画点,但只有轨 1 在 330–340 压低:三轨合起来只低 1.7 dB,过不了 minDepth(6 dB)。
    // 切换点处每条活跃轨的 pan 都可能变,只有大家一起歇的地方才换得不显眼。
    std::array<PipelineTrackFeatures, kPipelineTracks> f;
    f[0] = makeShaped({{50, 750}}, 0.05f, {{330, 340}});
    f[1] = makeShaped({{50, 750}}, 0.05f);
    f[2] = makeShaped({{50, 750}}, 0.05f);
    PipelineConfig cfg = makeConfig(kShapedHops, 3);
    const auto base = runAnalysisPipeline(f, cfg);
    REQUIRE(base.intervals == 1);
    const std::int64_t drawn = 400 * kHopSamples;
    REQUIRE(noEdgeWithinOneSecond(base, drawn));
    cfg.leadRuns = {{0, drawn, 1, true}, {drawn, cfg.rangeEndSample, 3, true}};
    const auto res = runAnalysisPipeline(f, cfg);
    const auto* after = segAt(res, 2, drawn);
    REQUIRE(after != nullptr);
    CHECK(after->pan == 0.0);
    CHECK(after->t0Samples == drawn); // ★ 原位
}

TEST_CASE("SL570 管线:子区间的平衡按子区间自己的能量算", "[analysis][lead][sl570][pipeline]")
{
    // 宿主把 Lead Select 从 0 拨到 9(一条没有素材的轨):两段都是「活跃轨里没有主唱」,指派结构一模一样,
    // 唯一的差别是能量 —— 轨 3 前半轻、后半重。子区间的 z 取子区间自己的 ⇒ 轨 3 前后两段的音量修正不同;
    // 若 z 仍取整个全局区间,两段的输入完全相同,解也相同,相邻同值合并后轨 3 只剩一段。
    std::array<PipelineTrackFeatures, kPipelineTracks> f;
    f[0] = makeShaped({{50, 750}}, 0.05f);
    f[1] = makeShaped({{50, 750}}, 0.05f);
    f[2] = makeShaped({{50, 400}}, 0.01f);
    for (int h = 400; h < 750; ++h)
    {
        f[2].kwMs[static_cast<std::size_t>(h)] = 0.2f;
        f[2].peak[static_cast<std::size_t>(h)] = std::sqrt(0.2f);
    }
    PipelineConfig cfg = makeConfig(kShapedHops, 3);
    cfg.tracks[0].priority = 0.0; // 优先级最低的轨 1 拿中心,轨 2、3 分坐两侧 —— 左右平衡要靠音量修正拉平
    const auto base = runAnalysisPipeline(f, cfg);
    REQUIRE(base.intervals == 1);
    const std::int64_t cut = 400 * kHopSamples;
    REQUIRE(noEdgeWithinOneSecond(base, cut));
    cfg.leadRuns = {{0, cut, 0, true}, {cut, cfg.rangeEndSample, 9, true}};
    const auto res = runAnalysisPipeline(f, cfg);
    const auto* first = segAt(res, 2, cut - 1);
    const auto* second = segAt(res, 2, cut);
    REQUIRE((first && second));
    INFO("vol " << first->volDb << " → " << second->volDb);
    CHECK(first != second); // ★ 在切换点分成两段
    CHECK(second->volDb < first->volDb); // 后半更响 ⇒ 修正更往下
}

TEST_CASE("SL570 管线:紧贴区间起点(< 150 ms)的停顿不拿来吸 —— 切出来的那一小截比最短主唱片段还短",
          "[analysis][lead][sl570][pipeline]")
{
    // 轨 1、2 唱满;轨 3 在 3 s 处加入(它的段起点 = 一条全局区间边界 edge)。轨 1、2 在 edge 之后 30–90 ms
    // 一起压低(停顿谷约在 edge + 60 ms)。换人画在 edge + 1.03 s:离 edge 超过 1 s(不吸边界),离那个停顿不到 1 s。
    std::array<PipelineTrackFeatures, kPipelineTracks> f;
    f[0] = makeShaped({{50, 750}}, 0.05f);
    f[1] = makeShaped({{50, 750}}, 0.05f);
    f[2] = makeShaped({{300, 750}}, 0.05f);
    PipelineConfig cfg = makeConfig(kShapedHops, 3);
    const auto base = runAnalysisPipeline(f, cfg);
    REQUIRE_FALSE(base.segments[2].empty());
    const std::int64_t edge = base.segments[2].front().t0Samples;
    const int edgeHop = static_cast<int>(edge / kHopSamples);
    REQUIRE(edgeHop + 9 < 300); // 前提:停顿落在轨 3 开口之前(否则轨 3 自己把谷填平了)
    f[0] = makeShaped({{50, 750}}, 0.05f, {{edgeHop + 3, edgeHop + 9}});
    f[1] = makeShaped({{50, 750}}, 0.05f, {{edgeHop + 3, edgeHop + 9}});
    const auto dipped = runAnalysisPipeline(f, cfg);
    REQUIRE(sameSegments(dipped, base)); // 前提:停顿没有改变任何段
    const std::int64_t drawn = edge + 103 * kHopSamples;
    REQUIRE(noEdgeWithinOneSecond(base, drawn)); // 前提:不吸 edge(103 hop > 1 s),另一侧也没有边界

    cfg.leadRuns = {{0, drawn, 1, true}, {drawn, cfg.rangeEndSample, 3, true}};
    const auto res = runAnalysisPipeline(f, cfg);
    const auto* after = segAt(res, 2, drawn);
    REQUIRE(after != nullptr);
    CHECK(after->pan == 0.0);
    INFO("cut at hop " << after->t0Samples / kHopSamples << ", edge hop " << edgeHop);
    CHECK(after->t0Samples == drawn); // ★ 原位;没有吸到 edge + 60 ms 那个停顿
}

TEST_CASE("SL570 管线:有效主唱全程不变 ⇒ 与「那一轨设 lead_lock」逐位同解(宿主记录 + 回落值拼成同一个值也一样)",
          "[analysis][lead][sl570][pipeline]")
{
    // 三个全局区间、活跃集合各不同,且轨 3 的响度一路爬升 —— 切开任何一个区间,两半的 z 不同、平衡给的 u 也不同,
    // 段表就会多出切点:所以「没有切换点就不切」在这里是看得见的。轨 4 从头唱到尾,让每个区间除主唱外至少还有
    // 一条自由轨、多数区间有两条(只有一条自由轨时平衡不出 u)。
    std::array<PipelineTrackFeatures, kPipelineTracks> f;
    f[0] = makeShaped({{50, 250}}, 0.02f);
    f[1] = makeShaped({{450, 750}}, 0.05f);
    f[2] = makeShaped({{50, 750}}, 0.02f);
    f[3] = makeShaped({{50, 750}}, 0.035f);
    for (int h = 50; h < 750; ++h)
    {
        const float kw = 0.02f + 0.07f * static_cast<float>(h - 50) / 700.0f;
        f[2].kwMs[static_cast<std::size_t>(h)] = kw;
        f[2].peak[static_cast<std::size_t>(h)] = std::sqrt(kw);
    }
    PipelineConfig lock = makeConfig(kShapedHops, 4);
    lock.tracks[2].leadLock = true;
    const auto oracle = runAnalysisPipeline(f, lock);
    REQUIRE(oracle.intervals >= 3); // 前提:多个区间
    bool anyU = false;
    for (const auto& segs : oracle.segments)
    {
        for (const auto& s : segs)
        {
            anyU = anyU || std::abs(s.volDb) > 1e-6;
        }
    }
    REQUIRE(anyU); // 前提:平衡真的出了 u,volDb 的逐位相等不是「两边都是 0」

    const std::int64_t end = lock.rangeEndSample;
    const std::int64_t q = 200 * kHopSamples + 77; // 记录的分界不对齐 hop、也不对齐任何段边界
    SECTION("没有记录,当前值 = 3")
    {
        PipelineConfig cfg = makeConfig(kShapedHops, 4);
        cfg.leadFallback = 3;
        const auto res = runAnalysisPipeline(f, cfg);
        checkBitIdentical(res, oracle);
        CHECK(distinctSegmentStarts(res) <= static_cast<std::size_t>(res.intervals)); // ★ 没有多切
    }
    SECTION("宿主记录前半是 3、后半没播到,当前值 = 3")
    {
        PipelineConfig cfg = makeConfig(kShapedHops, 4);
        cfg.leadRuns = {{0, q, 3, true}};
        cfg.leadFallback = 3;
        const auto res = runAnalysisPipeline(f, cfg);
        checkBitIdentical(res, oracle);
        CHECK(distinctSegmentStarts(res) <= static_cast<std::size_t>(res.intervals)); // ★ 记录与回落值的交界不是切换点
    }
    SECTION("宿主记录 3、中间夹一段插件写的 1(不算自动化),当前值 = 3")
    {
        PipelineConfig cfg = makeConfig(kShapedHops, 4);
        cfg.leadRuns = {{0, q, 3, true}, {q, 2 * q, 1, false}, {2 * q, end, 3, true}};
        cfg.leadFallback = 3;
        const auto res = runAnalysisPipeline(f, cfg);
        checkBitIdentical(res, oracle);
        CHECK(distinctSegmentStarts(res) <= static_cast<std::size_t>(res.intervals));
    }
    SECTION("宿主记录全程 3,当前值 = 0")
    {
        PipelineConfig cfg = makeConfig(kShapedHops, 4);
        cfg.leadRuns = {{0, end, 3, true}};
        cfg.leadFallback = 0;
        const auto res = runAnalysisPipeline(f, cfg);
        checkBitIdentical(res, oracle);
        CHECK(distinctSegmentStarts(res) <= static_cast<std::size_t>(res.intervals));
    }
}
