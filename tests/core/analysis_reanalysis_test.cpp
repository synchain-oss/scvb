// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include "analysis/Reanalysis.h"

namespace
{

using scvb::analysis::AnalysisSegment;
using scvb::analysis::ExcludedRange;
using scvb::analysis::FeatureProvider;
using scvb::analysis::GuardParams;
using scvb::analysis::MergeParams;
using scvb::analysis::Origin;
using scvb::analysis::ReanalysisRequest;
using scvb::analysis::RecomputeFn;
using scvb::analysis::SilenceWindow;
using scvb::analysis::VersionAnalysis;

constexpr int kSampleRate = 48000;

int64_t sec(double s)
{
    return static_cast<int64_t>(std::llround(s * kSampleRate));
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

// ---- 逐字节序列化(与 Reanalysis::serializeVersion 字段顺序一致)----

void putBytes(std::vector<uint8_t>& out, const void* p, std::size_t n)
{
    const auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}

void putU32(std::vector<uint8_t>& out, uint32_t x)
{
    putBytes(out, &x, sizeof(x));
}
void putI64(std::vector<uint8_t>& out, int64_t x)
{
    putBytes(out, &x, sizeof(x));
}
void putU64(std::vector<uint8_t>& out, uint64_t x)
{
    putBytes(out, &x, sizeof(x));
}
void putF64(std::vector<uint8_t>& out, double x)
{
    putBytes(out, &x, sizeof(x));
}
void putU8(std::vector<uint8_t>& out, uint8_t x)
{
    putBytes(out, &x, sizeof(x));
}

std::vector<uint8_t> serializeSegments(const std::vector<AnalysisSegment>& segs)
{
    std::vector<uint8_t> out;
    putU32(out, static_cast<uint32_t>(segs.size()));
    for (const AnalysisSegment& s : segs)
    {
        putI64(out, s.t0Samples);
        putI64(out, s.t1Samples);
        putF64(out, s.pan);
        putF64(out, s.volDb);
        putU8(out, static_cast<uint8_t>(s.origin));
        putU8(out, s.locked ? 1u : 0u);
        putU64(out, s.captureEpoch);
        putU64(out, s.analysisEpoch);
        putF64(out, s.loudnessLufs);
        putF64(out, s.energyLinear);
    }
    return out;
}

std::vector<uint8_t> serializeExcluded(const std::vector<ExcludedRange>& ex)
{
    std::vector<uint8_t> out;
    putU32(out, static_cast<uint32_t>(ex.size()));
    for (const ExcludedRange& e : ex)
    {
        putI64(out, e.t0);
        putI64(out, e.t1);
    }
    return out;
}

// 段列表裁剪到 [lo, hi)(保留各段在区间内的部分与原值)。
std::vector<AnalysisSegment> clipSegments(const std::vector<AnalysisSegment>& segs, int64_t lo, int64_t hi)
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

int64_t overlapLength(int64_t a0, int64_t a1, int64_t b0, int64_t b1)
{
    return std::max<int64_t>(0, std::min(a1, b1) - std::max(a0, b0));
}

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

// 15 轨确定性模型([J59] 口径):每轨 6×10s 覆盖 [0,60)s。
VersionAnalysis makeModel()
{
    VersionAnalysis v;
    v.resize(15);
    for (int t = 0; t < 15; ++t)
    {
        for (int i = 0; i < 6; ++i)
        {
            AnalysisSegment s;
            s.t0Samples = sec(i * 10.0);
            s.t1Samples = sec((i + 1) * 10.0);
            s.origin = Origin::Auto;
            s.locked = false;
            s.pan = (i % 2 == 0) ? -30.0 : 30.0;
            s.volDb = static_cast<double>(t - 7) * 0.5;
            s.captureEpoch = 1u;
            s.analysisEpoch = 2u;
            s.loudnessLufs = -20.0 + static_cast<double>(t) * 0.1;
            s.energyLinear = 0.01 + static_cast<double>(t) * 0.001;
            v.segments[static_cast<std::size_t>(t)].push_back(s);
        }
    }
    return v;
}

// 静音窗口构造:numHops 个 hop 全 activeDb;silence 区间置 silentDb。
std::vector<float> makeLoudnessDb(int64_t numHops, float activeDb = -20.0f)
{
    return std::vector<float>(static_cast<std::size_t>(numHops), activeDb);
}

void setSilenceSeconds(std::vector<float>& db, double t0, double t1, float silentDb = -80.0f)
{
    const int64_t h0 = static_cast<int64_t>(std::llround(t0 * 100.0));
    const int64_t h1 = static_cast<int64_t>(std::llround(t1 * 100.0));
    for (int64_t h = h0; h < h1 && h < static_cast<int64_t>(db.size()); ++h)
        db[static_cast<std::size_t>(h)] = silentDb;
}

SilenceWindow windowOf(const std::vector<float>& db, int64_t firstHop = 0)
{
    SilenceWindow w;
    w.loudnessDb = db.data();
    w.firstHop = firstHop;
    w.numHops = static_cast<int64_t>(db.size());
    return w;
}

} // namespace

// ============================================================================
// guard 扩展
// ============================================================================

TEST_CASE("GUARD-1: 无静音 → 两侧各扩固定 1s", "[reanalysis][guard]")
{
    auto db = makeLoudnessDb(6000); // 60s,全有声(-20 > -60)。
    GuardParams p;
    int64_t o0 = 0;
    int64_t o1 = 0;
    scvb::analysis::guardExtend(windowOf(db), sec(30.0), sec(40.0), p, o0, o1);
    CHECK(o0 == sec(29.0));
    CHECK(o1 == sec(41.0));
}

TEST_CASE("GUARD-2: 静音 run 在 1s 外 → 延伸到静音帧", "[reanalysis][guard]")
{
    auto db = makeLoudnessDb(6000);
    setSilenceSeconds(db, 26.0, 28.0); // 左静音 run 结束于 28s。
    setSilenceSeconds(db, 43.0, 45.0); // 右静音 run 开始于 43s。
    GuardParams p;
    int64_t o0 = 0;
    int64_t o1 = 0;
    scvb::analysis::guardExtend(windowOf(db), sec(30.0), sec(40.0), p, o0, o1);
    CHECK(o0 == sec(28.0)); // 30−28=2s > 1s。
    CHECK(o1 == sec(43.0)); // 43−40=3s > 1s。
}

TEST_CASE("GUARD-3: 静音 run 在 1s 内 → 最小 guard 1s 生效", "[reanalysis][guard]")
{
    auto db = makeLoudnessDb(6000);
    setSilenceSeconds(db, 28.5, 29.5); // 左静音结束 29.5s,距 R 起点 0.5s < 1s。
    GuardParams p;
    int64_t o0 = 0;
    int64_t o1 = 0;
    scvb::analysis::guardExtend(windowOf(db), sec(30.0), sec(40.0), p, o0, o1);
    CHECK(o0 == sec(29.0)); // max(1s, 0.5s) = 1s。
    CHECK(o1 == sec(41.0));
}

TEST_CASE("GUARD-4: 窗口边缘截断 + 短静音 run 不触发延伸", "[reanalysis][guard]")
{
    // 窗口仅 [0,5s)(500 hop),右边界截断。
    auto db = makeLoudnessDb(500);
    GuardParams p;
    int64_t o0 = 0;
    int64_t o1 = 0;
    scvb::analysis::guardExtend(windowOf(db), sec(4.0), sec(5.0), p, o0, o1);
    CHECK(o0 == sec(3.0));
    CHECK(o1 == sec(5.0)); // 4+1=5 已到窗口右端,截断在 5s。

    // 短静音 run(200ms < 250ms)不算「静音帧」→ 不触发延伸。
    auto db2 = makeLoudnessDb(6000);
    setSilenceSeconds(db2, 27.8, 28.0); // 200ms。
    int64_t p0 = 0;
    int64_t p1 = 0;
    scvb::analysis::guardExtend(windowOf(db2), sec(30.0), sec(40.0), p, p0, p1);
    CHECK(p0 == sec(29.0));
    CHECK(p1 == sec(41.0));
}

// ============================================================================
// splice:候选截断
// ============================================================================

TEST_CASE("SPLICE-1: 候选越出 R⁺ 边界 → 截断;外侧逐字节不动", "[reanalysis][splice]")
{
    const std::vector<AnalysisSegment> old = {seg(0.0, 10.0), seg(10.0, 20.0), seg(20.0, 30.0)};
    // 候选 [5,25) 越出 R⁺=[10,20) → 截断为 [10,20)。
    const std::vector<AnalysisSegment> cands = {seg(5.0, 25.0, Origin::Auto, false, 42.0, 1.0)};

    const auto r = scvb::analysis::spliceTrack(old, cands, {}, sec(10.0), sec(20.0), MergeParams{});

    REQUIRE(r.size() == 3);
    CHECK(r[0].t0Samples == sec(0.0));
    CHECK(r[0].t1Samples == sec(10.0));
    CHECK(r[1].t0Samples == sec(10.0));
    CHECK(r[1].t1Samples == sec(20.0));
    CHECK(r[1].pan == 42.0);
    CHECK(r[2].t0Samples == sec(20.0));
    CHECK(r[2].t1Samples == sec(30.0));
    CHECK(isNonOverlapping(r));
    // 外侧半段逐字节继承原值。
    REQUIRE(serializeSegments(clipSegments(r, sec(0.0), sec(10.0))) ==
            serializeSegments(clipSegments(old, sec(0.0), sec(10.0))));
    REQUIRE(serializeSegments(clipSegments(r, sec(20.0), sec(30.0))) ==
            serializeSegments(clipSegments(old, sec(20.0), sec(30.0))));
}

// ============================================================================
// 验收①:重做 (轨3, 30–40s) 后其余数据逐字节不变
// ============================================================================

TEST_CASE("REAN-1: 重做 (轨3,30-40s) 后其余 14 轨与轨3 R⁺ 外逐字节不变", "[reanalysis][acceptance]")
{
    const auto before = makeModel();
    const auto beforeBytes = scvb::analysis::serializeVersion(before);

    ReanalysisRequest req;
    req.tracks = {3};
    req.startSample = sec(30.0);
    req.endSample = sec(40.0);

    const RecomputeFn recompute = [](uint32_t, int64_t r0, int64_t r1) {
        return std::vector<AnalysisSegment>{seg(static_cast<double>(r0) / kSampleRate,
                                                static_cast<double>(r1) / kSampleRate, Origin::Auto, false, 42.0, 1.0)};
    };

    const auto after =
        scvb::analysis::reanalyze(before, req, FeatureProvider{}, recompute, GuardParams{}, MergeParams{});

    // 其余 14 轨逐字节不变。
    for (uint32_t t = 0; t < before.numTracks(); ++t)
    {
        if (t == 3)
            continue;
        INFO("track=" << t);
        REQUIRE(serializeSegments(before.segments[t]) == serializeSegments(after.segments[t]));
        REQUIRE(serializeExcluded(before.excluded[t]) == serializeExcluded(after.excluded[t]));
    }

    // 轨3:R⁺=[29,41)s 外逐字节不变(R⁺=R 两侧各扩 1s,空特征 → 固定 guard)。
    const int64_t rPlus0 = sec(29.0);
    const int64_t rPlus1 = sec(41.0);
    REQUIRE(serializeSegments(clipSegments(before.segments[3], 0, rPlus0)) ==
            serializeSegments(clipSegments(after.segments[3], 0, rPlus0)));
    REQUIRE(serializeSegments(clipSegments(before.segments[3], rPlus1, sec(60.0))) ==
            serializeSegments(clipSegments(after.segments[3], rPlus1, sec(60.0))));

    // R⁺ 内确实变了(重算生效),排除「什么都没做」的假绿。
    REQUIRE(serializeSegments(clipSegments(before.segments[3], rPlus0, rPlus1)) !=
            serializeSegments(clipSegments(after.segments[3], rPlus0, rPlus1)));
    REQUIRE(isNonOverlapping(after.segments[3]));

    // 全模型序列化确实不同(仅轨3 变化所致)。
    REQUIRE(beforeBytes != scvb::analysis::serializeVersion(after));
}

// ============================================================================
// 验收②:manual 段在重叠重分析下保留
// ============================================================================

TEST_CASE("REAN-2: manual 段在重叠重分析下保留,候选挖洞让位", "[reanalysis][acceptance]")
{
    VersionAnalysis before;
    before.resize(1);
    before.segments[0] = {seg(0.0, 30.0), seg(30.0, 35.0), seg(35.0, 37.0, Origin::UserEdited, false, 15.0, -2.0),
                          seg(37.0, 60.0)};

    const std::vector<uint8_t> manualBytes =
        serializeSegments({seg(35.0, 37.0, Origin::UserEdited, false, 15.0, -2.0)});

    ReanalysisRequest req;
    req.tracks = {0};
    req.startSample = sec(30.0);
    req.endSample = sec(40.0);

    const RecomputeFn recompute = [](uint32_t, int64_t r0, int64_t r1) {
        return std::vector<AnalysisSegment>{seg(static_cast<double>(r0) / kSampleRate,
                                                static_cast<double>(r1) / kSampleRate, Origin::Auto, false, 42.0, 1.0)};
    };

    const auto after =
        scvb::analysis::reanalyze(before, req, FeatureProvider{}, recompute, GuardParams{}, MergeParams{});

    // manual 段 [35,37) 逐字节保留。
    REQUIRE(serializeSegments(clipSegments(after.segments[0], sec(35.0), sec(37.0))) == manualBytes);

    // 候选被挖洞:[29,35) 与 [37,41) 为 auto 候选值,不侵入 [35,37)。
    const int64_t rPlus0 = sec(29.0);
    const int64_t rPlus1 = sec(41.0);
    REQUIRE(serializeSegments(clipSegments(after.segments[0], rPlus0, sec(35.0))) ==
            serializeSegments({seg(29.0, 35.0, Origin::Auto, false, 42.0, 1.0)}));
    REQUIRE(serializeSegments(clipSegments(after.segments[0], sec(37.0), rPlus1)) ==
            serializeSegments({seg(37.0, 41.0, Origin::Auto, false, 42.0, 1.0)}));

    // R⁺ 外逐字节不变。
    REQUIRE(serializeSegments(clipSegments(after.segments[0], 0, rPlus0)) ==
            serializeSegments(clipSegments(before.segments[0], 0, rPlus0)));
    REQUIRE(serializeSegments(clipSegments(after.segments[0], rPlus1, sec(60.0))) ==
            serializeSegments(clipSegments(before.segments[0], rPlus1, sec(60.0))));
    REQUIRE(isNonOverlapping(after.segments[0]));
}

TEST_CASE("REAN-2b: locked 段与候选交集 ≤50% → 挖洞;>50% → 整条丢弃", "[reanalysis][splice]")
{
    // 挖洞:locked [35,36),候选 [35.5,40) 交集 0.5s/4.5s=11% ≤50% → 挖成 [36,40)。
    const std::vector<AnalysisSegment> old2 = {seg(0.0, 35.0), seg(35.0, 36.0, Origin::Auto, true, 30.0, 0.0),
                                               seg(36.0, 60.0)};
    const std::vector<AnalysisSegment> cands1 = {seg(35.5, 40.0)};
    const auto r2 = scvb::analysis::spliceTrack(old2, cands1, {}, sec(0.0), sec(60.0), MergeParams{});

    // locked [35,36) 保留;候选挖成 [36,40)。
    bool foundLocked = false;
    bool foundCarved = false;
    for (const AnalysisSegment& s : r2)
    {
        if (s.t0Samples == sec(35.0) && s.t1Samples == sec(36.0) && s.locked)
            foundLocked = true;
        if (s.t0Samples == sec(36.0) && s.t1Samples == sec(40.0) && s.origin == Origin::Auto)
            foundCarved = true;
    }
    CHECK(foundLocked);
    CHECK(foundCarved);
    CHECK(isNonOverlapping(r2));

    // >50%:候选 [35.1,36.9) 与 locked [35,36) 交集 0.9s/1.8s=50%…恰好不 >50% → 挖洞。
    // 换候选 [35.2,36.8) 交集 0.8s/1.6s=50% 也不 >50%。构造 >50%:候选 [34.9,36.1) 交集 1.0s/1.2s=83%。
    const std::vector<AnalysisSegment> cands2 = {seg(34.9, 36.1)};
    const auto r3 = scvb::analysis::spliceTrack(old2, cands2, {}, sec(0.0), sec(60.0), MergeParams{});
    bool carvedInto = false;
    for (const AnalysisSegment& s : r3)
    {
        if (s.origin == Origin::Auto && s.t0Samples < sec(35.0) && s.t1Samples > sec(35.0))
            carvedInto = true;
    }
    CHECK(!carvedInto); // 候选大半落在 locked 内 → 丢弃。
}

// ============================================================================
// 验收③:ExcludedRange 内候选不复活
// ============================================================================

TEST_CASE("REAN-3: ExcludedRange 内候选不复活", "[reanalysis][acceptance]")
{
    VersionAnalysis before;
    before.resize(1);
    before.segments[0] = {seg(0.0, 30.0), seg(30.0, 40.0), seg(40.0, 60.0)};
    before.excluded[0] = {{sec(36.0), sec(38.0)}};

    ReanalysisRequest req;
    req.tracks = {0};
    req.startSample = sec(30.0);
    req.endSample = sec(40.0);

    // 候选完全落在 ExcludedRange [36,38) 内。
    const RecomputeFn recompute = [](uint32_t, int64_t, int64_t) {
        return std::vector<AnalysisSegment>{seg(36.2, 37.8)};
    };

    const auto after =
        scvb::analysis::reanalyze(before, req, FeatureProvider{}, recompute, GuardParams{}, MergeParams{});

    // 被删区间内无任何段复活。
    for (const AnalysisSegment& s : after.segments[0])
    {
        INFO("segment [" << s.t0Samples << "," << s.t1Samples << ")");
        REQUIRE(overlapLength(s.t0Samples, s.t1Samples, sec(36.0), sec(38.0)) == 0);
    }
    REQUIRE(clipSegments(after.segments[0], sec(36.0), sec(38.0)).empty());
    REQUIRE(isNonOverlapping(after.segments[0]));
}

// ============================================================================
// 验收④:分析幂等(同输入连续 3 次逐字节相同)
// ============================================================================

TEST_CASE("REAN-4: 幂等 —— 同输入连续 3 次逐字节相同", "[reanalysis][acceptance]")
{
    const auto model = makeModel();
    ReanalysisRequest req;
    req.tracks = {3, 7};
    req.startSample = sec(30.0);
    req.endSample = sec(40.0);

    const RecomputeFn recompute = [](uint32_t track, int64_t r0, int64_t r1) {
        const double pan = (track == 3) ? 42.0 : -17.0;
        return std::vector<AnalysisSegment>{seg(static_cast<double>(r0) / kSampleRate,
                                                static_cast<double>(r1) / kSampleRate, Origin::Auto, false, pan, 1.0)};
    };

    const auto a = scvb::analysis::reanalyze(model, req, FeatureProvider{}, recompute, GuardParams{}, MergeParams{});
    const auto b = scvb::analysis::reanalyze(a, req, FeatureProvider{}, recompute, GuardParams{}, MergeParams{});
    const auto c = scvb::analysis::reanalyze(b, req, FeatureProvider{}, recompute, GuardParams{}, MergeParams{});

    const auto bytesA = scvb::analysis::serializeVersion(a);
    const auto bytesB = scvb::analysis::serializeVersion(b);
    const auto bytesC = scvb::analysis::serializeVersion(c);
    REQUIRE(bytesA == bytesB);
    REQUIRE(bytesB == bytesC);
}

// ============================================================================
// 三条不变式属性测试
// ============================================================================

namespace
{

std::mt19937 makeRng(int round)
{
    return std::mt19937(static_cast<unsigned>(0x5EED1234u + static_cast<unsigned>(round) * 2654435761u));
}

int randInt(std::mt19937& rng, int lo, int hi)
{
    return static_cast<int>(std::uniform_int_distribution<int>(lo, hi)(rng));
}

Origin randOrigin(std::mt19937& rng)
{
    const int x = randInt(rng, 0, 9);
    if (x < 2)
        return Origin::UserEdited;
    if (x < 4)
        return Origin::UserCreated;
    return Origin::Auto;
}

// [0, total) 完全覆盖随机分段。
std::vector<AnalysisSegment> randomSegmentation(std::mt19937& rng, int64_t total)
{
    std::vector<int64_t> bounds;
    bounds.push_back(0);
    const int n = randInt(rng, 3, 8);
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

} // namespace

TEST_CASE("REAN-5: 随机局部重分析三不变式(50 轮)", "[reanalysis][invariant]")
{
    const int64_t total = sec(30.0);
    constexpr int kNumTracks = 4;
    for (int round = 0; round < 50; ++round)
    {
        auto rng = makeRng(round);

        VersionAnalysis before;
        before.resize(kNumTracks);
        for (auto& t : before.segments)
            t = randomSegmentation(rng, total);

        const int64_t r0 = sec(randInt(rng, 6, 12));
        const int64_t r1 = sec(randInt(rng, 18, 24));
        const int64_t rPlus0 = r0 - sec(1.0); // 空特征 → 固定 1s guard。
        const int64_t rPlus1 = r1 + sec(1.0);

        // 随机 T 子集。
        std::vector<uint32_t> reqTracks;
        for (int t = 0; t < kNumTracks; ++t)
        {
            if (randInt(rng, 0, 1) == 0)
                reqTracks.push_back(static_cast<uint32_t>(t));
        }
        if (reqTracks.empty())
            reqTracks.push_back(static_cast<uint32_t>(randInt(rng, 0, kNumTracks - 1)));

        ReanalysisRequest req;
        req.tracks = reqTracks;
        req.startSample = r0;
        req.endSample = r1;

        const RecomputeFn recompute = [&](uint32_t track, int64_t rr0, int64_t rr1) {
            (void)track;
            return std::vector<AnalysisSegment>{
                seg(static_cast<double>(rr0) / kSampleRate, static_cast<double>(rr1) / kSampleRate)};
        };

        const auto after =
            scvb::analysis::reanalyze(before, req, FeatureProvider{}, recompute, GuardParams{}, MergeParams{});

        // 不变式 3:T 外轨逐字节不变。
        for (int t = 0; t < kNumTracks; ++t)
        {
            if (std::find(reqTracks.begin(), reqTracks.end(), static_cast<uint32_t>(t)) != reqTracks.end())
                continue;
            INFO("round=" << round << " track=" << t);
            REQUIRE(serializeSegments(before.segments[static_cast<std::size_t>(t)]) ==
                    serializeSegments(after.segments[static_cast<std::size_t>(t)]));
        }

        // 不变式 2:T 内 R⁺ 外逐字节不变。
        for (const uint32_t t : reqTracks)
        {
            INFO("round=" << round << " track=" << t);
            REQUIRE(serializeSegments(clipSegments(before.segments[t], 0, rPlus0)) ==
                    serializeSegments(clipSegments(after.segments[t], 0, rPlus0)));
            REQUIRE(serializeSegments(clipSegments(before.segments[t], rPlus1, total)) ==
                    serializeSegments(clipSegments(after.segments[t], rPlus1, total)));
        }

        // 不变式 1:user/locked 段不丢、值不变(覆盖保持)。
        for (int t = 0; t < kNumTracks; ++t)
        {
            for (const AnalysisSegment& s : before.segments[static_cast<std::size_t>(t)])
            {
                if (!s.isUserSegment())
                    continue;
                int64_t covered = 0;
                int64_t cursor = s.t0Samples;
                for (const AnalysisSegment& rr : after.segments[static_cast<std::size_t>(t)])
                {
                    const int64_t ov =
                        std::max<int64_t>(0, std::min(rr.t1Samples, s.t1Samples) - std::max(rr.t0Samples, cursor));
                    if (ov > 0)
                    {
                        INFO("round=" << round << " track=" << t);
                        REQUIRE(rr.pan == s.pan);
                        REQUIRE(rr.volDb == s.volDb);
                        REQUIRE(rr.origin == s.origin);
                        REQUIRE(rr.locked == s.locked);
                        covered += ov;
                        cursor = std::max(cursor, rr.t1Samples);
                    }
                }
                INFO("round=" << round << " user segment [" << s.t0Samples << "," << s.t1Samples << ")");
                REQUIRE(covered == s.t1Samples - s.t0Samples);
            }
            REQUIRE(isNonOverlapping(after.segments[static_cast<std::size_t>(t)]));
        }
    }
}

// ============================================================================
// 防御:请求区间校验(PR#45 审查【重要】#1)
// ============================================================================

TEST_CASE("SPLICE-2: R⁺ 倒置/空区间 → 原样返回旧段(逐字节不变)", "[reanalysis][splice]")
{
    const std::vector<AnalysisSegment> old = {seg(0.0, 10.0), seg(10.0, 20.0)};
    const std::vector<AnalysisSegment> cands = {seg(5.0, 15.0, Origin::Auto, false, 42.0, 1.0)};
    const std::vector<uint8_t> oldBytes = serializeSegments(old);

    // 倒置 R⁺(rPlus0 > rPlus1)。
    const auto inverted = scvb::analysis::spliceTrack(old, cands, {}, sec(20.0), sec(10.0), MergeParams{});
    REQUIRE(serializeSegments(inverted) == oldBytes);

    // 空 R⁺(rPlus0 == rPlus1)。
    const auto empty = scvb::analysis::spliceTrack(old, cands, {}, sec(10.0), sec(10.0), MergeParams{});
    REQUIRE(serializeSegments(empty) == oldBytes);
}

TEST_CASE("REAN-6: 请求区间倒置/空/负 → 原样返回(逐字节不变)", "[reanalysis][acceptance]")
{
    const auto before = makeModel();
    const std::vector<uint8_t> beforeBytes = scvb::analysis::serializeVersion(before);

    const RecomputeFn recompute = [](uint32_t, int64_t r0, int64_t r1) {
        return std::vector<AnalysisSegment>{seg(static_cast<double>(r0) / kSampleRate,
                                                static_cast<double>(r1) / kSampleRate, Origin::Auto, false, 42.0, 1.0)};
    };

    // 倒置区间(end < start)。
    {
        ReanalysisRequest req;
        req.tracks = {3};
        req.startSample = sec(40.0);
        req.endSample = sec(30.0);
        const auto after =
            scvb::analysis::reanalyze(before, req, FeatureProvider{}, recompute, GuardParams{}, MergeParams{});
        REQUIRE(scvb::analysis::serializeVersion(after) == beforeBytes);
    }

    // 空区间(end == start)。
    {
        ReanalysisRequest req;
        req.tracks = {3};
        req.startSample = sec(30.0);
        req.endSample = sec(30.0);
        const auto after =
            scvb::analysis::reanalyze(before, req, FeatureProvider{}, recompute, GuardParams{}, MergeParams{});
        REQUIRE(scvb::analysis::serializeVersion(after) == beforeBytes);
    }

    // 负起点(start < 0)。
    {
        ReanalysisRequest req;
        req.tracks = {3};
        req.startSample = -sec(1.0);
        req.endSample = sec(30.0);
        const auto after =
            scvb::analysis::reanalyze(before, req, FeatureProvider{}, recompute, GuardParams{}, MergeParams{});
        REQUIRE(scvb::analysis::serializeVersion(after) == beforeBytes);
    }
}
