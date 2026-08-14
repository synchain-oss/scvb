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
constexpr double kHopMs = 10.0;

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
