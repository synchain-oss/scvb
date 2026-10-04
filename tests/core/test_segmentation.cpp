// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "analysis/Segmentation.h"

namespace
{

using scvb::analysis::AnalysisSegment;
using scvb::analysis::ExcludedRange;
using scvb::analysis::GlobalInterval;
using scvb::analysis::MergeParams;
using scvb::analysis::Origin;
using scvb::analysis::SegmentationParams;
using scvb::analysis::Valley;

constexpr int kSampleRate = 48000;

int64_t sec(double s)
{
    return static_cast<int64_t>(std::llround(s * kSampleRate));
}

// ℓ(dB)构造:plateauDb 平台 + 若干矩形谷(centerHop 为中心、halfWidthHops 半宽、depthDb 深)。
std::vector<float> makeLoudness(int totalHops, double plateauDb,
                                const std::vector<std::tuple<int, int, double>>& valleys)
{
    std::vector<float> l(static_cast<std::size_t>(totalHops), static_cast<float>(plateauDb));
    for (const auto& v : valleys)
    {
        const int center = std::get<0>(v);
        const int half = std::get<1>(v);
        const float depth = static_cast<float>(std::get<2>(v));
        for (int k = center - half; k < center + half; ++k)
        {
            if (k >= 0 && k < totalHops)
                l[static_cast<std::size_t>(k)] = static_cast<float>(plateauDb - depth);
        }
    }
    return l;
}

// S1:SEG-1 的谷参数(两个 8dB、200ms 宽谷 @4s/8s,平台 −20)。
std::vector<float> seg1Loudness()
{
    // 20 hop 宽谷(中心 400/800),矩形谷对称于名义 hop → rep = 400/800。
    return makeLoudness(1200, -20.0, {{400, 10, 8.0}, {800, 10, 8.0}});
}

AnalysisSegment seg(double t0, double t1, Origin o = Origin::Auto, bool locked = false, double pan = 0.0,
                    double volDb = 0.0)
{
    AnalysisSegment s;
    s.t0Samples = sec(t0);
    s.t1Samples = sec(t1);
    s.origin = o;
    s.locked = locked;
    s.pan = pan;
    s.volDb = volDb;
    return s;
}

bool segmentsEqual(const AnalysisSegment& a, const AnalysisSegment& b)
{
    return a.t0Samples == b.t0Samples && a.t1Samples == b.t1Samples && a.pan == b.pan && a.volDb == b.volDb &&
           a.origin == b.origin && a.locked == b.locked;
}

// 段列表严格按 t0 排序且互不重叠。
bool isNonOverlapping(const std::vector<AnalysisSegment>& segs)
{
    for (std::size_t i = 1; i < segs.size(); ++i)
    {
        if (segs[i - 1].t0Samples >= segs[i - 1].t1Samples)
            return false;
        if (segs[i - 1].t1Samples > segs[i].t0Samples)
            return false;
    }
    return true;
}

} // namespace

// ============================================================================
// S1 谷切分
// ============================================================================

TEST_CASE("SEG-1: 12s 双 8dB 谷 maxSegment=8 → 恰好 2 段,切在 4s", "[segmentation][valley]")
{
    const auto l = seg1Loudness();
    SegmentationParams p;
    p.maxSegmentS = 8.0;
    const auto r = scvb::analysis::splitValleys(l.data(), 0, 1200, p);

    REQUIRE(r.segments.size() == 2);
    REQUIRE(!r.noNaturalCut);
    CHECK(r.segments[0].startHop == 0);
    CHECK(r.segments[1].endHop == 1200);
    CHECK(r.segments[0].endHop == r.segments[1].startHop);
    // 切点 = 较早的 4s 谷(两谷同分 → 距中点同 → 较早者)。
    CHECK(r.segments[0].endHop == 400);
}

TEST_CASE("SEG-2: 10s 无谷平坦 → 未找到自然切点,保留 1 段", "[segmentation][valley]")
{
    const auto l = makeLoudness(1000, -20.0, {});
    SegmentationParams p;
    p.maxSegmentS = 8.0;
    const auto r = scvb::analysis::splitValleys(l.data(), 0, 1000, p);

    REQUIRE(r.segments.size() == 1);
    CHECK(r.noNaturalCut);
    CHECK(r.segments[0].startHop == 0);
    CHECK(r.segments[0].endHop == 1000);
}

TEST_CASE("SEG-9: 三谷深浅交错,两遍法 prominence 筛除浅谷且不影响另两谷 depth", "[segmentation][valley]")
{
    // 12s 平台 −20,谷深 8/4/7 dB @4s/6s/8s;minDepth=6。
    const auto l = makeLoudness(1200, -20.0, {{400, 10, 8.0}, {600, 10, 4.0}, {800, 10, 7.0}});
    SegmentationParams p;
    p.sensitivity = 50.0; // minDepth = 6

    const auto valleys = scvb::analysis::detectValleys(l.data(), 0, 1200, p);
    REQUIRE(valleys.size() == 3);

    // 三谷 depth 8 / 4 / 7,侧峰边界 = 相邻局部极小,不依赖筛选结果。
    CHECK(valleys[0].hop == 400);
    CHECK(valleys[0].depthDb == Approx(8.0).margin(1e-6));
    CHECK(valleys[1].hop == 600);
    CHECK(valleys[1].depthDb == Approx(4.0).margin(1e-6));
    CHECK(valleys[2].hop == 800);
    CHECK(valleys[2].depthDb == Approx(7.0).margin(1e-6));

    // 按 depth > minDepth 筛选:4dB 谷被筛除,候选谷 = {4s, 8s}。
    std::vector<int64_t> candidateHops;
    for (const Valley& v : valleys)
    {
        if (v.depthDb > p.minDepthDb())
            candidateHops.push_back(v.hop);
    }
    REQUIRE(candidateHops.size() == 2);
    CHECK(candidateHops[0] == 400);
    CHECK(candidateHops[1] == 800);

    // 反向断言:去掉 4dB 谷后,另两谷 depth 不变(两遍法无自指)。
    const auto l2 = makeLoudness(1200, -20.0, {{400, 10, 8.0}, {800, 10, 7.0}});
    const auto valleys2 = scvb::analysis::detectValleys(l2.data(), 0, 1200, p);
    REQUIRE(valleys2.size() == 2);
    CHECK(valleys2[0].depthDb == Approx(8.0).margin(1e-6));
    CHECK(valleys2[1].depthDb == Approx(7.0).margin(1e-6));
}

// ---------------------------------------------------------------------------
// [SL-382] sensitivity → 段数 的**单调性**(用户 v5.6.11 实测 B14「灵敏度好像没用」)。
//
// 为什么要造三层谷树、而不是一条 10s 里放几个深浅不同的谷:`splitValleys` 的递归在
// `seg.t1 - seg.t0 <= maxHops` 处**无条件停**(§3.2 步骤 4 第一行)。所以一段 10s 素材
// 无论灵敏度多高都只切一刀 —— 切完两半都 ≤8s,浅谷再多也轮不到。段数要真的随灵敏度变,
// 必须让**每一层切完之后的半段仍然 >maxSegment**,即:
//   64s ──12dB──> 32s ×2 ──7dB──> 16s ×4 ──4dB──> 8s ×8(恰好 = maxHops,停)
// 三档 minDepth = 6·2^((50−s)/50) 各自吃到第几层,段数就停在第几层:
//   s=5  → 11.20:只有 12dB 过线 → 2 段(32s 半段里找不到自然切点 ⇒ noNaturalCut)
//   s=50 → 6.00 :12/7dB 过线   → 4 段(16s 四分段里 4dB 谷不过线 ⇒ 仍 noNaturalCut)
//   s=95 → 3.22 :全部过线      → 8 段(每片恰好 8s = maxHops,递归无条件停 ⇒ 无 noNaturalCut)
// 谷宽固定 20 hop(200ms),与 SEG-1/SEG-9 同款:5 hop 移动平均后谷底仍保住全深。
//
// ⚠ 本用例喂的是 **ℓ(dB)**,与 `Segmentation.h` 的入参契约一致。生产侧曾经喂**线性
//   kw**,那时 depth 落在 1e-2 量级,而本用例三档里**最小**的 minDepth 也有 3.22
//   (映射在 s=100 处的下确界是 3.0,同样够不到)⇒ 三档全是 1 段;
//   钉住那一跳的是 `test_analysis_pipeline.cpp` 的 `[SL382]` 流水线格,不是这里。
// ---------------------------------------------------------------------------
TEST_CASE("[SL382] sensitivity 5/50/95 → 段数 2/4/8 严格单调", "[segmentation][valley][SL382]")
{
    // 64s 三层谷树:深 12dB @32s;7dB @16s/48s;4dB @8s/24s/40s/56s。
    const auto l = makeLoudness(6400, -20.0,
                                {{3200, 10, 12.0},
                                 {1600, 10, 7.0},
                                 {4800, 10, 7.0},
                                 {800, 10, 4.0},
                                 {2400, 10, 4.0},
                                 {4000, 10, 4.0},
                                 {5600, 10, 4.0}});

    struct Expect
    {
        double sensitivity;
        std::size_t segments;
        bool noNaturalCut;
    };
    // 段数与 noNaturalCut 都写死:只断「单调」的话,三档一起退化成 1 段(= 生产侧
    // 喂线性 kw 的那个形态)照样满足「非严格单调」,那种绿是假的。
    const Expect grid[] = {{5.0, 2u, true}, {50.0, 4u, true}, {95.0, 8u, false}};

    std::vector<std::size_t> counts;
    for (const Expect& e : grid)
    {
        SegmentationParams p;
        p.maxSegmentS = 8.0;
        p.sensitivity = e.sensitivity;
        INFO("sensitivity=" << e.sensitivity << " minDepth=" << p.minDepthDb());
        const auto r = scvb::analysis::splitValleys(l.data(), 0, 6400, p);
        CHECK(r.segments.size() == e.segments);
        CHECK(r.noNaturalCut == e.noNaturalCut);
        counts.push_back(r.segments.size());
    }

    // 严格单调递增(灵敏度越高越容易切,02 §3.2 逐字「越灵敏越容易切」)。
    REQUIRE(counts.size() == 3u);
    CHECK(counts[0] < counts[1]);
    CHECK(counts[1] < counts[2]);
}

// ============================================================================
// S2 全局区间划分
// ============================================================================

TEST_CASE("SEG-3: 两轨 [0,4)、[2,6) → 三区间 {1}/{1,2}/{2}", "[segmentation][interval]")
{
    const std::vector<std::vector<AnalysisSegment>> tracks = {
        {seg(0.0, 4.0)},
        {seg(2.0, 6.0)},
    };
    const auto iv = scvb::analysis::buildGlobalIntervals(tracks, kSampleRate, 150.0);

    REQUIRE(iv.size() == 3);
    CHECK(iv[0].t0 == sec(0.0));
    CHECK(iv[0].t1 == sec(2.0));
    CHECK(iv[0].tracks == std::vector<int>{0});
    CHECK(iv[1].t0 == sec(2.0));
    CHECK(iv[1].t1 == sec(4.0));
    CHECK(iv[1].tracks == std::vector<int>({0, 1}));
    CHECK(iv[2].t0 == sec(4.0));
    CHECK(iv[2].t1 == sec(6.0));
    CHECK(iv[2].tracks == std::vector<int>{1});
}

TEST_CASE("SEG-4: 100ms 碎片 → 对称差并入,区间数不变多", "[segmentation][interval]")
{
    // 在 SEG-3 基础上加一条 100ms 碎片轨 [4.0,4.1),制造碎片 [4,4.1)。
    const std::vector<std::vector<AnalysisSegment>> tracks = {
        {seg(0.0, 4.0)},
        {seg(2.0, 6.0)},
        {seg(4.0, 4.1)},
    };
    const auto iv = scvb::analysis::buildGlobalIntervals(tracks, kSampleRate, 150.0);

    // 碎片 {1,2} 与右邻 {1} 对称差(1)小于与左邻 {0,1} 的对称差(2)→ 并入右侧;区间数仍 3。
    REQUIRE(iv.size() == 3);
    CHECK(iv[0].t0 == sec(0.0));
    CHECK(iv[0].t1 == sec(2.0));
    CHECK(iv[0].tracks == std::vector<int>{0});
    CHECK(iv[1].t0 == sec(2.0));
    CHECK(iv[1].t1 == sec(4.0));
    CHECK(iv[1].tracks == std::vector<int>({0, 1}));
    CHECK(iv[2].t0 == sec(4.0));
    CHECK(iv[2].t1 == sec(6.0));
    CHECK(iv[2].tracks == std::vector<int>{1});
}

// ============================================================================
// S3 manual 合并
// ============================================================================

TEST_CASE("SEG-5: UserEdited 占位段与候选重叠 80% → 丢弃候选,占位不动", "[segmentation][merge]")
{
    // 旧段含 UserEdited [10,12)s;R=[8,15),guard=1s → R⁺=[7,16)。
    const std::vector<AnalysisSegment> old = {seg(10.0, 12.0, Origin::UserEdited, false, 20.0, -3.0)};
    const std::vector<AnalysisSegment> cands = {seg(9.8, 12.3), seg(13.0, 14.0)};

    const auto r = scvb::analysis::mergeTrackSegments(old, cands, {}, sec(7.0), sec(16.0), MergeParams{});

    // 候选1 与占位交集 [10,12)=2.0s 占自身 2.5s 的 80%>50% → 丢;候选2 保留。
    REQUIRE(r.size() == 2);
    REQUIRE(r[0].t0Samples == sec(10.0));
    REQUIRE(r[0].t1Samples == sec(12.0));
    REQUIRE(r[0].origin == Origin::UserEdited);
    REQUIRE(r[0].pan == 20.0);
    REQUIRE(r[0].volDb == -3.0);
    REQUIRE(r[1].t0Samples == sec(13.0));
    REQUIRE(r[1].t1Samples == sec(14.0));
    REQUIRE(r[1].origin == Origin::Auto);
    REQUIRE(isNonOverlapping(r));
}

TEST_CASE("SEG-6: ExcludedRange [5,6) × 候选 [5.1,5.9) → 不复活", "[segmentation][merge]")
{
    const std::vector<AnalysisSegment> cands = {seg(5.1, 5.9)};
    const std::vector<ExcludedRange> excl = {{sec(5.0), sec(6.0)}};

    const auto r = scvb::analysis::mergeTrackSegments({}, cands, excl, sec(0.0), sec(10.0), MergeParams{});

    REQUIRE(r.empty());
}

TEST_CASE("SEG-6b: ExcludedRange [5,6) × 候选 [4,8) → 重叠 25% ≤50% → 保留(分母=候选自身长度)", "[segmentation][merge]")
{
    const std::vector<AnalysisSegment> cands = {seg(4.0, 8.0)};
    const std::vector<ExcludedRange> excl = {{sec(5.0), sec(6.0)}};

    const auto r = scvb::analysis::mergeTrackSegments({}, cands, excl, sec(0.0), sec(10.0), MergeParams{});

    // 重叠 1s = 候选自身 4s 的 25% ≤50% → 保留(按 ExcludedRange 长度口径会误判 100% 而误丢)。
    REQUIRE(r.size() == 1);
    REQUIRE(r[0].t0Samples == sec(4.0));
    REQUIRE(r[0].t1Samples == sec(8.0));
    REQUIRE(isNonOverlapping(r));
}

TEST_CASE("SEG-8: locked 占位与候选交集 20% ≤50% → 挖洞,全轨互不重叠", "[segmentation][merge]")
{
    // 占位 [10,12)(locked);候选 [11.5,14.0)(交集 0.5s 占自身 20%)→ 挖洞 → [12,14)。
    const std::vector<AnalysisSegment> old = {seg(10.0, 12.0, Origin::Auto, true, 30.0, 0.0)};
    const std::vector<AnalysisSegment> cands = {seg(11.5, 14.0)};

    const auto r = scvb::analysis::mergeTrackSegments(old, cands, {}, sec(0.0), sec(20.0), MergeParams{});

    REQUIRE(r.size() == 2);
    REQUIRE(r[0].t0Samples == sec(10.0));
    REQUIRE(r[0].t1Samples == sec(12.0));
    REQUIRE(r[0].locked);
    REQUIRE(r[1].t0Samples == sec(12.0));
    REQUIRE(r[1].t1Samples == sec(14.0));
    REQUIRE(r[1].origin == Origin::Auto);
    REQUIRE(!r[1].locked);
    REQUIRE(isNonOverlapping(r));
}

// ============================================================================
// 三条不变式属性测试(SEG-7)
// ============================================================================

namespace
{

// 确定性 PRNG(固定种子,100 轮可复现)。
std::mt19937 makeRng(int round)
{
    return std::mt19937(static_cast<unsigned>(0x5EED1234u + static_cast<unsigned>(round) * 2654435761u));
}

int randInt(std::mt19937& rng, int lo, int hi)
{
    return static_cast<int>(std::uniform_int_distribution<int>(lo, hi)(rng));
}

// 随机 origin / locked。
Origin randOrigin(std::mt19937& rng)
{
    const int x = randInt(rng, 0, 9);
    if (x < 2)
        return Origin::UserEdited;
    if (x < 4)
        return Origin::UserCreated;
    return Origin::Auto;
}

// 生成 [0, total) 的随机完全覆盖分段(非重叠、按 t0 排序、含随机 origin/locked/pan/vol)。
std::vector<AnalysisSegment> randomSegmentation(std::mt19937& rng, int64_t total)
{
    std::vector<int64_t> bounds;
    bounds.push_back(0);
    const int n = randInt(rng, 3, 9);
    for (int i = 0; i < n; ++i)
        bounds.push_back(static_cast<int64_t>(randInt(rng, 1, static_cast<int>(total - 1))));
    bounds.push_back(total);
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    std::vector<AnalysisSegment> segs;
    for (std::size_t i = 0; i + 1 < bounds.size(); ++i)
    {
        AnalysisSegment s;
        s.t0Samples = bounds[i];
        s.t1Samples = bounds[i + 1];
        s.origin = randOrigin(rng);
        s.locked = (randInt(rng, 0, 9) == 0);
        s.pan = static_cast<double>(randInt(rng, -80, 80));
        s.volDb = static_cast<double>(randInt(rng, -12, 6));
        segs.push_back(s);
    }
    return segs;
}

// 段列表裁剪到 [lo, hi)(保留各段在区间内的部分与原值)。
std::vector<AnalysisSegment> clipRange(const std::vector<AnalysisSegment>& segs, int64_t lo, int64_t hi)
{
    std::vector<AnalysisSegment> out;
    for (const AnalysisSegment& s : segs)
    {
        const int64_t t0 = std::max(s.t0Samples, lo);
        const int64_t t1 = std::min(s.t1Samples, hi);
        if (t1 <= t0)
            continue;
        AnalysisSegment c = s;
        c.t0Samples = t0;
        c.t1Samples = t1;
        out.push_back(c);
    }
    return out;
}

bool sameList(const std::vector<AnalysisSegment>& a, const std::vector<AnalysisSegment>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (!segmentsEqual(a[i], b[i]))
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("SEG-7 不变式1: 重分析后 user/locked 段不丢且边界值不变(100 轮)", "[segmentation][invariant]")
{
    const int64_t total = sec(30.0);
    for (int round = 0; round < 100; ++round)
    {
        auto rng = makeRng(round);
        const auto old = randomSegmentation(rng, total);

        // 随机 R⁺(guard 扩展后)。
        const int64_t r0 = sec(randInt(rng, 2, 12));
        const int64_t r1 = sec(randInt(rng, 18, 28));
        const int64_t rPlus0 = r0 - sec(1.0);
        const int64_t rPlus1 = r1 + sec(1.0);

        // 随机候选(在 R⁺ 内)与随机 ExcludedRange。
        std::vector<AnalysisSegment> cands;
        int64_t c0 = rPlus0;
        for (int i = 0; i < randInt(rng, 1, 4); ++i)
        {
            const int64_t len = sec(randInt(rng, 1, 3));
            const int64_t c1 = std::min(rPlus1, c0 + len);
            if (c1 > c0)
            {
                cands.push_back(seg(static_cast<double>(c0) / kSampleRate, static_cast<double>(c1) / kSampleRate));
                c0 = c1 + sec(0.5);
            }
        }
        std::vector<ExcludedRange> excl;
        if (randInt(rng, 0, 2) == 0)
            excl.push_back({rPlus0 + sec(1.0), rPlus0 + sec(3.0)});

        const auto result = scvb::analysis::mergeTrackSegments(old, cands, excl, rPlus0, rPlus1, MergeParams{});

        // 每个 user/locked 旧段:覆盖不丢、值不变。
        for (const AnalysisSegment& s : old)
        {
            if (!s.isUserSegment())
                continue;
            int64_t covered = 0;
            int64_t cursor = s.t0Samples;
            for (const AnalysisSegment& r : result)
            {
                const int64_t ov =
                    std::max<int64_t>(0, std::min(r.t1Samples, s.t1Samples) - std::max(r.t0Samples, cursor));
                if (ov > 0)
                {
                    INFO("round=" << round);
                    // 重叠 result 段必须是 s 本身(同值,不覆盖)。
                    REQUIRE(r.pan == s.pan);
                    REQUIRE(r.volDb == s.volDb);
                    REQUIRE(r.origin == s.origin);
                    REQUIRE(r.locked == s.locked);
                    covered += ov;
                    cursor = std::max(cursor, r.t1Samples);
                }
            }
            INFO("round=" << round << " user segment [" << s.t0Samples << "," << s.t1Samples << ")");
            REQUIRE(covered == s.t1Samples - s.t0Samples);
        }
        REQUIRE(isNonOverlapping(result));
    }
}

TEST_CASE("SEG-7 不变式2: R⁺ 外字节不动,guard 带不产生新 pan/vol 值(100 轮)", "[segmentation][invariant]")
{
    const int64_t total = sec(30.0);
    for (int round = 0; round < 100; ++round)
    {
        auto rng = makeRng(round);
        const auto old = randomSegmentation(rng, total);

        const int64_t r0 = sec(randInt(rng, 2, 12));
        const int64_t r1 = sec(randInt(rng, 18, 28));
        const int64_t rPlus0 = r0 - sec(1.0);
        const int64_t rPlus1 = r1 + sec(1.0);

        std::vector<AnalysisSegment> cands;
        int64_t c0 = rPlus0;
        for (int i = 0; i < randInt(rng, 1, 4); ++i)
        {
            const int64_t len = sec(randInt(rng, 1, 3));
            const int64_t c1 = std::min(rPlus1, c0 + len);
            if (c1 > c0)
            {
                cands.push_back(seg(static_cast<double>(c0) / kSampleRate, static_cast<double>(c1) / kSampleRate));
                c0 = c1 + sec(0.5);
            }
        }

        const auto result = scvb::analysis::mergeTrackSegments(old, cands, {}, rPlus0, rPlus1, MergeParams{});

        // 强不变式:R⁺ 外(左/右两侧)段划分与值逐位一致。
        INFO("round=" << round);
        REQUIRE(sameList(clipRange(old, 0, rPlus0), clipRange(result, 0, rPlus0)));
        REQUIRE(sameList(clipRange(old, rPlus1, total), clipRange(result, rPlus1, total)));

        // 弱不变式:guard 带(R⁺R)内不产生新的 pan/vol 值 —— 每段 pan/vol 要么继承自旧段,要么是候选默认(0,0)。
        const int64_t gLo = rPlus0;
        const int64_t gHi = r0;
        for (const AnalysisSegment& r : result)
        {
            if (r.t1Samples <= gLo || r.t0Samples >= gHi)
                continue;
            const bool inherited = [&]() {
                for (const AnalysisSegment& o : old)
                {
                    if (o.pan == r.pan && o.volDb == r.volDb)
                        return true;
                }
                return false;
            }();
            const bool isDefault = (r.pan == 0.0 && r.volDb == 0.0);
            INFO("round=" << round << " guard segment [" << r.t0Samples << "," << r.t1Samples << ") pan=" << r.pan
                          << " vol=" << r.volDb);
            REQUIRE((inherited || isDefault));
        }
        REQUIRE(isNonOverlapping(result));
    }
}

TEST_CASE("SEG-7 不变式3: T 外轨段与曲线值一致(100 轮)", "[segmentation][invariant]")
{
    const int64_t total = sec(30.0);
    constexpr int kNumTracks = 4;
    for (int round = 0; round < 100; ++round)
    {
        auto rng = makeRng(round);
        std::vector<std::vector<AnalysisSegment>> tracks(kNumTracks);
        for (auto& t : tracks)
            t = randomSegmentation(rng, total);

        const int64_t rPlus0 = sec(5.0);
        const int64_t rPlus1 = sec(25.0);

        // 随机 T 子集(至少 1 轨,至多 kNumTracks-1 轨)。
        const int inT = randInt(rng, 0, kNumTracks - 1);
        std::vector<bool> isT(kNumTracks, false);
        for (int t = 0; t < kNumTracks; ++t)
            isT[static_cast<std::size_t>(t)] = (t == inT || randInt(rng, 0, 1) == 0);

        // 只对 T 内轨跑 merge;T 外轨一个字节不动。
        std::vector<std::vector<AnalysisSegment>> result = tracks;
        for (int t = 0; t < kNumTracks; ++t)
        {
            if (!isT[static_cast<std::size_t>(t)])
                continue;
            std::vector<AnalysisSegment> cands;
            int64_t c0 = rPlus0;
            for (int i = 0; i < randInt(rng, 1, 3); ++i)
            {
                const int64_t c1 = std::min(rPlus1, c0 + sec(randInt(rng, 1, 3)));
                if (c1 > c0)
                {
                    cands.push_back(seg(static_cast<double>(c0) / kSampleRate, static_cast<double>(c1) / kSampleRate));
                    c0 = c1 + sec(0.5);
                }
            }
            result[static_cast<std::size_t>(t)] = scvb::analysis::mergeTrackSegments(
                tracks[static_cast<std::size_t>(t)], cands, {}, rPlus0, rPlus1, MergeParams{});
        }

        for (int t = 0; t < kNumTracks; ++t)
        {
            if (!isT[static_cast<std::size_t>(t)])
            {
                INFO("round=" << round << " track=" << t);
                REQUIRE(sameList(tracks[static_cast<std::size_t>(t)], result[static_cast<std::size_t>(t)]));
            }
        }
    }
}

// ===========================================================================
// [J145] 边界拖拽吸附谷(`detectSnapValleys`,契约 §1.27 `requestWaveform.valleys[]` 的算法面)。
// 与 detectValleys 共用平滑 / 谷底判定 / minDepth 门槛,只有 depth 的侧峰边界不同(见 Segmentation.h)。
// ===========================================================================

TEST_CASE("[J145] 吸附谷:矩形谷落在谷中心,depth = 平台 − 谷底", "[segmentation][valley][J145]")
{
    // 12s 平台 −20,一个 20 dB、300ms 宽的矩形谷 @4s(hop 385..414)。
    const auto l = makeLoudness(1200, -20.0, {{400, 15, 20.0}});
    SegmentationParams p;
    p.sensitivity = 50.0; // minDepth = 6

    const auto v = scvb::analysis::detectSnapValleys(l.data(), 0, 1200, p);
    REQUIRE(v.size() == 1);
    // 平滑后谷底平坦区 = [387,412],中心四舍五入 = 400(与 detectValleys 同一条 rep 口径)。
    CHECK(v[0].hop == 400);
    CHECK(v[0].depthDb == Approx(20.0).margin(1e-6));
}

TEST_CASE("[J145] 吸附谷:浅于 minDepth 的谷不产出,门槛随灵敏度走", "[segmentation][valley][J145]")
{
    // 4 dB 谷:minDepth(s=50)=6 ⇒ 不产出;minDepth(s=100)=3 ⇒ 产出。门槛与 S1 候选谷同一条。
    const auto l = makeLoudness(1200, -20.0, {{600, 15, 4.0}});
    SegmentationParams p;
    p.sensitivity = 50.0;
    CHECK(scvb::analysis::detectSnapValleys(l.data(), 0, 1200, p).empty());
    p.sensitivity = 100.0;
    const auto v = scvb::analysis::detectSnapValleys(l.data(), 0, 1200, p);
    REQUIRE(v.size() == 1);
    CHECK(v[0].hop == 600);
}

TEST_CASE("[J145] 吸附谷:贴段端的低平区不算谷(该侧没有东西可爬)", "[segmentation][valley][J145]")
{
    // [0,100) 低平 −60,其后平台 −20:detectValleys 会把段首平坦区当局部极小(depth=0),
    // 吸附这边同样 depth=0 ⇒ 不产出。覆盖段从一段静音开始是常态,不能在覆盖起点凭空吸一下。
    std::vector<float> l(400, -20.0f);
    for (int k = 0; k < 100; ++k)
        l[static_cast<std::size_t>(k)] = -60.0f;
    SegmentationParams p;
    CHECK(scvb::analysis::detectSnapValleys(l.data(), 0, 400, p).empty());
}

// 为什么吸附不能照搬 detectValleys 的 depth(见 Segmentation.h 头注):两句之间是**带噪底噪**、
// 不是数字静音时,每个噪声小起伏都是一个局部极小,「相邻极小」就在几十毫秒外 ⇒ 按 detectValleys
// 算 depth 只有零点几 dB,整段间隙一个候选都没有。同一份 ℓ 两边各跑一次,把这件事钉成对照。
//
// 素材:−20 dB 句子 [0,300) · 底噪 −60±1.5 dB(固定种子)[300,500) · 句子 [500,800);
// 间隙里在 hop 418..422 埋一个 −62.5 dB 的小坑,让「间隙最低点」有确定位置(平滑后唯一最低 = 420)。
// 小坑比周围噪声只低约 2~3 dB ⇒ 它的「相邻极小口径」depth 仍过不了 6 dB 门槛,不会替前者蒙混过关。
TEST_CASE("[J145] 带噪间隙:detectValleys 零候选,detectSnapValleys 恰一个谷在间隙最低点", "[segmentation][valley][J145]")
{
    std::vector<float> l(800, -20.0f);
    std::mt19937 rng(0x5CB1452u);
    std::uniform_real_distribution<float> noise(-1.5f, 1.5f);
    for (int k = 300; k < 500; ++k)
        l[static_cast<std::size_t>(k)] = -60.0f + noise(rng);
    for (int k = 418; k <= 422; ++k)
        l[static_cast<std::size_t>(k)] = -62.5f;
    SegmentationParams p;
    p.sensitivity = 50.0; // minDepth = 6

    // 对照:相邻极小口径下,整条 ℓ 没有一个谷过门槛(间隙两侧的平台是平的,不产生极小)。
    int legacyCandidates = 0;
    for (const Valley& v : scvb::analysis::detectValleys(l.data(), 0, 800, p))
    {
        if (v.depthDb > p.minDepthDb())
            ++legacyCandidates;
    }
    CHECK(legacyCandidates == 0);

    const auto snap = scvb::analysis::detectSnapValleys(l.data(), 0, 800, p);
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].hop == 420);
    // depth ≈ 句子 − 坑底(−20 − (−62.5) = 42.5);平台是平的,平滑不改它。
    CHECK(snap[0].depthDb == Approx(42.5).margin(0.5));
}

TEST_CASE("[J145] 吸附谷:两个间隙各一个谷,按 hop 升序", "[segmentation][valley][J145]")
{
    const auto l = makeLoudness(1200, -20.0, {{300, 20, 30.0}, {800, 10, 12.0}});
    SegmentationParams p;
    const auto v = scvb::analysis::detectSnapValleys(l.data(), 0, 1200, p);
    REQUIRE(v.size() == 2);
    CHECK(v[0].hop == 300);
    CHECK(v[1].hop == 800);
    CHECK(v[0].depthDb == Approx(30.0).margin(1e-6));
    CHECK(v[1].depthDb == Approx(12.0).margin(1e-6));
}
