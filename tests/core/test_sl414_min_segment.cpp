// SPDX-License-Identifier: GPL-3.0-or-later
// test_sl414_min_segment —— [SL-414] MIN SEG 对段表的兜底(统筹裁定「候选 D」+ #261 首轮改
// 「时间相接」、落点定 OutputProcessor::applyAnalysisSegments)。
//
// 定谳(scvb-sl414 工作树 build-sl414\sl414-verdict.md,不在仓内;四问结论已复制进 PR 描述):
//   min_segment_ms 只在 S0(VAD core 丢短)与 S1(谷切分)被消费;S2 全局区间划分与
//   回写层零消费 —— 用户看到的 0.21s 段 = 两轨 VAD 段交叠 210ms(> minGlobalInterval
//   150ms)被切成独立区间,回写层按区间给每轨落了一段。
//
// 修法真源 = masterPlan 02 §3.4 步骤 5(commit 8829bf4):兜底**不在管线里**,落点 =
// OutputProcessor::applyAnalysisSegments 对新段做完 clash 过滤之后、写回窗裁剪之前,对该轨
// 幸存新段跑 mergeShortAutoSegments(相接口径;用户段/锁定段永不参与;被 clash 丢掉的段
// 留下的空档由「相接」兜住 —— 用户段天然是屏障,#261 首轮 Claude-A【重要】②)。
//
// 本文件:纯函数直测(相接/间隙/相等边界/用户段)+ 管线区间产物上的显式兜底格
// (生产落点在 applyAnalysisSegments,真 startAnalysis 路径由 tests/host 的
// HOST SL414 三格走)。夹具口径:VAD padding 置 0,让「交叠 = A 终点 − B 起点」
// 恰好落在整 hop 上(210ms = 21 hop);真机默认 padding 只会放大跨轨交叠,不影响方向。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "analysis/AnalysisPipeline.h"
#include "analysis/EnergyVad.h"
#include "analysis/Segmentation.h"

using namespace scvb::analysis;

namespace
{

constexpr double kSr = 48000.0;
constexpr int kHopMs = 10;
constexpr std::int64_t kHopSamples = 480; // 10ms @ 48k

// 一轨素材:[startHop,endHop) 为 kw=0.05 的有声区,其余 1e-9;全 hop covered。
PipelineTrackFeatures trackWithLoud(int startHop, int endHop, int totalHops)
{
    PipelineTrackFeatures f;
    f.kwMs.assign(static_cast<std::size_t>(totalHops), 1e-9f);
    f.peak.assign(static_cast<std::size_t>(totalHops), 1e-5f);
    for (int i = startHop; i < endHop; ++i)
    {
        f.kwMs[static_cast<std::size_t>(i)] = 0.05f;
        f.peak[static_cast<std::size_t>(i)] = std::sqrt(0.05f);
    }
    f.covered.assign(f.kwMs.size(), 1u);
    f.anyCovered = true;
    return f;
}

// 在既有轨素材上追加一段有声区(与已有段间隔 ≥ mergeGap ⇒ VAD 出两个独立段)。
void addLoud(PipelineTrackFeatures& f, int startHop, int endHop)
{
    for (int i = startHop; i < endHop; ++i)
    {
        f.kwMs[static_cast<std::size_t>(i)] = 0.05f;
        f.peak[static_cast<std::size_t>(i)] = std::sqrt(0.05f);
    }
}

PipelineConfig sl414Config(std::size_t numHops, int activeTracks, int minSegmentMs)
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
    cfg.vad.minSegmentMs = minSegmentMs;
    cfg.segmentation.minSegmentMs = static_cast<double>(minSegmentMs);
    cfg.vad.paddingPreMs = 0; // 见文件头注:交叠算术要落在整 hop 上
    cfg.vad.paddingPostMs = 0;
    return cfg;
}

void dumpPipelineTables(const char* tag, const PipelineResult& r)
{
    for (int t = 0; t < kPipelineTracks; ++t)
    {
        const auto& segs = r.segments[static_cast<std::size_t>(t)];
        if (segs.empty())
        {
            continue;
        }
        std::string line = std::string("SL414 ") + tag + " track " + std::to_string(t) + " (" +
                           std::to_string(segs.size()) + " segs):";
        for (const auto& s : segs)
        {
            line += " [" + std::to_string(static_cast<double>(s.t0Samples) / kSr) + "s," +
                    std::to_string(static_cast<double>(s.t1Samples) / kSr) + "s|" +
                    std::to_string(static_cast<double>(s.length()) / kSr * 1000.0) + "ms]";
        }
        UNSCOPED_INFO(line);
    }
}

bool hasSegmentShorterThan(const std::vector<AnalysisSegment>& segs, std::int64_t lenSamples)
{
    for (const auto& s : segs)
    {
        if (s.length() < lenSamples)
        {
            return true;
        }
    }
    return false;
}

std::int64_t minSegmentLength(const std::vector<AnalysisSegment>& segs)
{
    std::int64_t m = -1;
    for (const auto& s : segs)
    {
        if (m < 0 || s.length() < m)
        {
            m = s.length();
        }
    }
    return m;
}

AnalysisSegment segAt(std::int64_t t0Samples, std::int64_t t1Samples, double pan, double volDb = 0.0,
                      Origin origin = Origin::Auto, bool locked = false)
{
    AnalysisSegment s;
    s.t0Samples = t0Samples;
    s.t1Samples = t1Samples;
    s.pan = pan;
    s.volDb = volDb;
    s.origin = origin;
    s.locked = locked;
    return s;
}

} // namespace

// ===========================================================================
// ① 管线区间产物 + 显式兜底:两轨交叠 210ms + MIN SEG=2000 → 段表无 < MIN SEG 的段
// ===========================================================================
// 几何与定谳 §2.2 逐字同形:轨 0 有声 [0,800) hop、轨 1 有声 [780,1579) hop;core 终点
// 多一 hop(§2.2 预平滑)⇒ 交叠 = [780,801) = 21 hop = 210ms,切出独立区间,回写后
// 两轨各有一段 210ms。**生产落点在 applyAnalysisSegments(clash 过滤后、裁剪前)**,
// 管线本身不做 —— 本格对管线输出显式调 mergeShortAutoSegments 断言兜底语义;
// 真 startAnalysis 路径由 tests/host 的 HOST SL414 三格走。
// 轨 0:短段与前段**相接**(prev.t1 == 780 == t0)⇒ 并入前一段 ⇒ [0,8.010) pan 0;
// 轨 1:短段无前段 ⇒ 并入后一段(next.t0 == 801 == t1)⇒ [7.800,15.790) pan 0。
// **删除式**(生产侧):去掉 applyAnalysisSegments 里的调用 ⇒ HOST SL414 (a) 红;
// 本格的显式调用不经过那条路,删除生产调用时本格照绿 —— 红在 HOST 格,别在这里找。
TEST_CASE("[SL414] 管线产物显式兜底:两轨交叠 210ms → 段表无 < MIN SEG 的 auto 段", "[analysis][pipeline][SL414]")
{
    constexpr int kTotalHops = 1579;
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = trackWithLoud(0, 800, kTotalHops);
    features[1] = trackWithLoud(780, 1579, kTotalHops);

    const auto res = runAnalysisPipeline(features, sl414Config(kTotalHops, 2, 2000));
    dumpPipelineTables("overlap210-raw", res);

    // 区间图不动:兜底只收敛段表,不为指派重新造区间(裁定 D 的边界)。
    REQUIRE(res.tracksTouched == 2);
    REQUIRE(res.intervals == 3);

    auto s0 = res.segments[static_cast<std::size_t>(0)];
    auto s1 = res.segments[static_cast<std::size_t>(1)];
    // 前提(修前复现态):管线输出里两轨各有一条 210ms 短段。
    REQUIRE(s0.size() == 2);
    REQUIRE(s1.size() == 2);
    CHECK(hasSegmentShorterThan(s0, 2000 * 48));
    CHECK(hasSegmentShorterThan(s1, 2000 * 48));

    mergeShortAutoSegments(s0, 2000.0, kSr);
    mergeShortAutoSegments(s1, 2000.0, kSr);
    dumpPipelineTables("overlap210-merged", res);

    // 核心判据:兜底后段表里没有 < 2000ms 的 auto 段。
    CHECK_FALSE(hasSegmentShorterThan(s0, 2000 * 48));
    CHECK_FALSE(hasSegmentShorterThan(s1, 2000 * 48));

    // 轨 0:短段并入前一段(相接)⇒ 恰一段 [0, 8.010s),值取前段(pan 0)。
    REQUIRE(s0.size() == 1);
    CHECK(s0[0].t0Samples == 0);
    CHECK(s0[0].t1Samples == 801 * kHopSamples);
    CHECK(s0[0].pan == Catch::Approx(0.0).margin(1e-9));

    // 轨 1:短段无前段 ⇒ 并入后一段(相接)⇒ 恰一段 [7.800s, 15.790s),值取后一段(pan 0)。
    REQUIRE(s1.size() == 1);
    CHECK(s1[0].t0Samples == 780 * kHopSamples);
    CHECK(s1[0].t1Samples == 1579 * kHopSamples);
    CHECK(s1[0].pan == Catch::Approx(0.0).margin(1e-9));
}

// ===========================================================================
// ② 三轨间隙几何(裁定 1 的「相接」主格):短段与前段**不相接**、与后段相接
// ===========================================================================
// A(轨 0)有声 [0,1000) 与 [5980,6500);B(轨 1)有声 [1000,6000)。区间切分:
//   [0,1000){A} + [1000,1001){A,B}(10ms < 150ms → 步骤 3 吸收,平局并入前侧)
//   ⇒ [0,1001){A};[1001,5980){B};[5980,6001){A,B}(210ms ≥ 150 → 独立区间);
//   [6001,6501){A}。
// A 的管线产物:[0,1001) + [5980,6001)(210ms 短)+ [6001,6501)。短段的
//   prev.t1 = 1001 ≠ 5980(**不相接** —— 中间是 A 不活跃的 49.79s 静音间隙);
//   next.t0 = 6001 == t1(**相接**)⇒ 并入**后**段。
// ⇒ A 兜底后 = [0,1001) + [5980,6501),**首段 t1 不变**(仍 10.01s)。
// **删除式**(裁定 1):去掉相接判据(改回「有前段就并前段」)⇒ 短段被并进前段,
//   首段 t1 变成 6001 hop ⇒ 本格红在「首段 t1 == 1001 hop」那条断言上,
//   且 A 的表会把 49.79s 的静音间隙盖成一段不存在的覆盖。
TEST_CASE("[SL414] 相接:三轨间隙几何,短段与前段不相接 ⇒ 并入后段,首段 t1 不变", "[analysis][pipeline][SL414]")
{
    constexpr int kTotalHops = 6501;
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = trackWithLoud(0, 1000, kTotalHops);
    addLoud(features[0], 5980, 6500); // A 的第二段:与第一段隔 ~49.8s 静音
    features[1] = trackWithLoud(1000, 6000, kTotalHops);

    const auto res = runAnalysisPipeline(features, sl414Config(kTotalHops, 2, 2000));
    dumpPipelineTables("gap3-raw", res);

    REQUIRE(res.tracksTouched == 2);
    auto s0 = res.segments[static_cast<std::size_t>(0)];
    REQUIRE(s0.size() == 3); // 前提:[0,1001) + [5980,6001)(210ms 短)+ [6001,6501)
    CHECK(hasSegmentShorterThan(s0, 2000 * 48));

    mergeShortAutoSegments(s0, 2000.0, kSr);
    dumpPipelineTables("gap3-merged", res);

    REQUIRE(s0.size() == 2);
    // 首段 t1 不变(相接判据挡住了「隔着间隙并进前段」)—— 删除式落点。
    CHECK(s0[0].t0Samples == 0);
    CHECK(s0[0].t1Samples == 1001 * kHopSamples);
    // 短段并入后段:后段 t0 提前到短段起点,t1 不动。
    CHECK(s0[1].t0Samples == 5980 * kHopSamples);
    CHECK(s0[1].t1Samples == 6501 * kHopSamples);
    CHECK_FALSE(hasSegmentShorterThan(s0, 2000 * 48));
}

// ===========================================================================
// ③ 对照:交叠 0ms → 无短段(短段成因 = 跨轨交叠切出的独立区间)
// ===========================================================================
TEST_CASE("[SL414] 对照:交叠 0ms → 无短段(短段成因 = 跨轨交叠切出的独立区间)", "[analysis][pipeline][SL414]")
{
    constexpr int kTotalHops = 1579;
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = trackWithLoud(0, 800, kTotalHops);
    features[1] = trackWithLoud(801, 1579, kTotalHops); // 801 = 轨 0 量化后终点:恰好相接,无交叠

    const auto res = runAnalysisPipeline(features, sl414Config(kTotalHops, 2, 2000));
    dumpPipelineTables("overlap0", res);

    REQUIRE(res.tracksTouched == 2);
    REQUIRE(res.intervals == 2);
    CHECK(minSegmentLength(res.segments[0]) == 801 * kHopSamples); // 8.010s
    CHECK(minSegmentLength(res.segments[1]) == 778 * kHopSamples); // 7.780s
    CHECK_FALSE(hasSegmentShorterThan(res.segments[0], 2000 * 48));
    CHECK_FALSE(hasSegmentShorterThan(res.segments[1], 2000 * 48));
}

// ===========================================================================
// ③ 对照:交叠 2500ms → 区间照切(3 条),段长 ≥ MIN SEG(兜底无可并对象)
// ===========================================================================
TEST_CASE("[SL414] 对照:交叠 2500ms → 照样切区间,段长 ≥ MIN SEG(是否违限只取决于交叠长)", "[analysis][pipeline][SL414]")
{
    constexpr int kTotalHops = 1579;
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = trackWithLoud(0, 800, kTotalHops);
    features[1] = trackWithLoud(551, 1579, kTotalHops); // 交叠 [551,801) = 250 hop = 2.5s

    const auto res = runAnalysisPipeline(features, sl414Config(kTotalHops, 2, 2000));
    dumpPipelineTables("overlap2500", res);

    REQUIRE(res.tracksTouched == 2);
    REQUIRE(res.intervals == 3); // 机制照常工作:交叠区间是独立区间
    // 轨 0:[0,5.51) + [5.51,8.01);轨 1:[5.51,8.01) + [8.01,15.79)。没有一条 < 2000ms,
    // 兜底无可并对象 —— 与修前逐字节同形(参数本就该约束它们)。
    CHECK(minSegmentLength(res.segments[0]) == 2500 * 48);
    CHECK(minSegmentLength(res.segments[1]) == 2500 * 48);
    CHECK_FALSE(hasSegmentShorterThan(res.segments[0], 2000 * 48));
    CHECK_FALSE(hasSegmentShorterThan(res.segments[1], 2000 * 48));
}

// ===========================================================================
// ③ 对照:交叠 130ms(< minGlobalInterval=150ms)→ 被步骤 3 吸收,无短段
// ===========================================================================
// 钉住:S2 唯一的短段门槛是 150ms 常量(02 §0.3 minGlobalInterval),与 min_segment_ms 无关;
// 吸收方保留自己的活跃集合(现行机制),被吸收交叠里**另一轨**的表覆盖悄悄短了一截。
TEST_CASE("[SL414] 对照:交叠 130ms < minGlobalInterval → 被步骤 3 吸收,无短段", "[analysis][pipeline][SL414]")
{
    constexpr int kTotalHops = 1579;
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = trackWithLoud(0, 800, kTotalHops);
    features[1] = trackWithLoud(788, 1579, kTotalHops); // 交叠 [788,801) = 130ms(量化后)< 150ms

    const auto res = runAnalysisPipeline(features, sl414Config(kTotalHops, 2, 2000));
    dumpPipelineTables("overlap130", res);

    REQUIRE(res.tracksTouched == 2);
    REQUIRE(res.intervals == 2); // 130ms < 150ms ⇒ 没有独立区间
    CHECK_FALSE(hasSegmentShorterThan(res.segments[0], 2000 * 48));
    CHECK_FALSE(hasSegmentShorterThan(res.segments[1], 2000 * 48));
    // 轨 1 的表从 8.010s 才开始:它在交叠里的活跃被步骤 3 的「吸收方保留集合」丢掉了。
    CHECK(res.segments[1].front().t0Samples == 801 * kHopSamples);
}

// ===========================================================================
// ④ 参数到达段表:同一张管线产物,MIN SEG=50 保留 220ms 段、MIN SEG=2000 并掉
// ===========================================================================
// 定谳时代(修前)本格断的是「50 与 2000 段表逐字节相同」(区间/回写层零消费的证据);
// 兜底落 applyAnalysisSegments 之后,管线上这一点对**两张档位的输入完全相同**(管线仍
// 零消费 min_segment_ms),分歧由兜底产生:220ms 在 50 档合法保留、在 2000 档被并掉。
TEST_CASE("[SL414] 同一管线产物:MIN SEG=50 保留短段、2000 并掉(参数在兜底层到达段表)", "[analysis][pipeline][SL414]")
{
    constexpr int kTotalHops = 1579;
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = trackWithLoud(0, 800, kTotalHops);
    features[1] = trackWithLoud(779, 1579, kTotalHops); // 交叠 [779,801) = 220ms

    const auto res = runAnalysisPipeline(features, sl414Config(kTotalHops, 2, 50));
    dumpPipelineTables("minseg-raw", res);

    for (const int t : {0, 1})
    {
        auto lo = res.segments[static_cast<std::size_t>(t)];
        auto hi = res.segments[static_cast<std::size_t>(t)];
        // 前提:管线产物(两档同源)里有 220ms 短段。
        REQUIRE(lo.size() == 2);
        CHECK(hasSegmentShorterThan(lo, 2000 * 48));

        mergeShortAutoSegments(lo, 50.0, kSr);
        mergeShortAutoSegments(hi, 2000.0, kSr);

        // MIN SEG=50:220ms 段合法保留(两段,含 220ms 那条)。
        REQUIRE(lo.size() == 2);
        CHECK_FALSE(hasSegmentShorterThan(lo, 50 * 48));
        CHECK(hasSegmentShorterThan(lo, 2000 * 48)); // 220ms < 2000ms:它在,而且合法
        // MIN SEG=2000:并入后恰一段,无 < 2000ms 的段。
        CHECK_FALSE(hasSegmentShorterThan(hi, 2000 * 48));
        REQUIRE(hi.size() == 1);
    }
}

// ===========================================================================
// ⑤ 纯函数直测(mergeShortAutoSegments,Segmentation.h/.cpp)
// ===========================================================================

// 无前段 ⇒ 并入**相接**的后一段(提前后一段 t0),**值取被并入的那一段**(后一段的
// pan/vol 保留,短段自己的 pan/vol 被丢弃 —— 交叠区间里解出的分槽值不做二次平衡)。
TEST_CASE("[SL414] 纯函数:首段并入相接的后一段,值取后一段", "[analysis][pipeline][SL414]")
{
    std::vector<AnalysisSegment> segs;
    segs.push_back(segAt(0, 480, -60.0, 2.0)); // 10ms,短
    segs.push_back(segAt(480, 96000, 0.0, -3.5)); // 2000ms,相接(next.t0 == 480 == t1)

    mergeShortAutoSegments(segs, 2000.0, kSr);

    REQUIRE(segs.size() == 1);
    CHECK(segs[0].t0Samples == 0); // t0 提前到短段起点
    CHECK(segs[0].t1Samples == 96000);
    CHECK(segs[0].pan == Catch::Approx(0.0)); // 值取后一段
    CHECK(segs[0].volDb == Catch::Approx(-3.5));
}

// 优先并入**相接**的前一段(延长前一段 t1);连续短段在同一轮里逐个被吸进前侧。
TEST_CASE("[SL414] 纯函数:短段并入相接的前一段,连续短段循环并入", "[analysis][pipeline][SL414]")
{
    std::vector<AnalysisSegment> segs;
    segs.push_back(segAt(0, 96000, 1.0, -1.0)); // 2000ms
    segs.push_back(segAt(96000, 97000, -60.0, 2.0)); // 10ms,短,相接
    segs.push_back(segAt(97000, 97500, 60.0, 3.0)); // 5ms,短,相接(连续)
    segs.push_back(segAt(97500, 288000, -1.0, -2.0)); // 2000ms+

    mergeShortAutoSegments(segs, 2000.0, kSr);

    REQUIRE(segs.size() == 2);
    CHECK(segs[0].t0Samples == 0);
    CHECK(segs[0].t1Samples == 97500); // 两条短段依次延长前一段的 t1
    CHECK(segs[0].pan == Catch::Approx(1.0)); // 值取前一段,不重算
    CHECK(segs[0].volDb == Catch::Approx(-1.0));
    CHECK(segs[1].t0Samples == 97500);
    CHECK(segs[1].t1Samples == 288000);
    CHECK(segs[1].pan == Catch::Approx(-1.0));
}

// [第 1 推·裁定 1] **带间隙不并**:短段与相邻段时间上不相接(隔着静音间隙)⇒ 原地保留。
// 「表内相邻」不等于「相接」—— 并进去会把静音间隙盖成一段不存在的覆盖。
TEST_CASE("[SL414] 纯函数:带间隙(不相接)⇒ 原地保留", "[analysis][pipeline][SL414]")
{
    std::vector<AnalysisSegment> segs;
    segs.push_back(segAt(0, 48000, 1.0, -1.0)); // 1000ms,短(< 2000ms)
    segs.push_back(segAt(96000, 97000, -60.0, 2.0)); // 10ms,短;与前者隔 1s 间隙
    // 间隙之后再来一条长段,钉住「后段也不吸收不相接的短段」。
    segs.push_back(segAt(192000, 288000, -1.0, -2.0)); // 2000ms,与短段隔 2s 间隙

    mergeShortAutoSegments(segs, 2000.0, kSr);

    REQUIRE(segs.size() == 3); // 三段原地保留:两条短段都没有相接的邻居
    CHECK(segs[0].t1Samples == 48000); // 首段 t1 不被拉长
    CHECK(segs[1].t0Samples == 96000);
    CHECK(segs[1].t1Samples == 97000);
    CHECK(segs[2].t0Samples == 192000);
}

// [第 1 推·裁定 1] **恰好等于 MIN SEG 保留**:「< minSegmentMs」是严格小于,等于不算短。
TEST_CASE("[SL414] 纯函数:恰好等于 MIN SEG 的段保留", "[analysis][pipeline][SL414]")
{
    std::vector<AnalysisSegment> segs;
    segs.push_back(segAt(0, 96000, 1.0, -1.0)); // 恰好 2000ms
    segs.push_back(segAt(96000, 192000, -1.0, -2.0)); // 恰好 2000ms,相接

    mergeShortAutoSegments(segs, 2000.0, kSr);

    REQUIRE(segs.size() == 2); // 两段都 = MIN SEG ⇒ 都保留,不并
    CHECK(segs[0].t1Samples == 96000);
    CHECK(segs[1].t0Samples == 96000);
}

// 整轨只剩一段时保留 —— S0 只保证 **core** ≥ min,这一段仍可能短:窗边裁剪([SL-399 R8]),
// 或「邻段因与用户段/锁定段 clash 而整条落选」后剩下的孤段(后者与裁剪无关,**整条时间线
// 重分析时同样会出**)。没有可并入的对象 ⇒ 与「两侧都不相接」同一档,原样保留。
TEST_CASE("[SL414] 纯函数:整轨只剩一段时保留", "[analysis][pipeline][SL414]")
{
    std::vector<AnalysisSegment> segs;
    segs.push_back(segAt(480, 480 + 480, 0.0)); // 10ms,短,且是唯一一段

    mergeShortAutoSegments(segs, 2000.0, kSr);

    REQUIRE(segs.size() == 1);
    CHECK(segs[0].t0Samples == 480);
    CHECK(segs[0].t1Samples == 960);
}

// 用户段(origin != Auto)/锁定段永不参与:短用户段自己不被并;用户段也不吸收别人。
TEST_CASE("[SL414] 纯函数:用户段 / 锁定段永不参与", "[analysis][pipeline][SL414]")
{
    // 短用户段:不并,邻段也不动。
    std::vector<AnalysisSegment> user;
    user.push_back(segAt(0, 480, 5.0, 0.0, Origin::UserEdited)); // 10ms,用户段
    user.push_back(segAt(480, 96000, 0.0)); // 2000ms auto,相接
    mergeShortAutoSegments(user, 2000.0, kSr);
    REQUIRE(user.size() == 2);
    CHECK(user[0].t0Samples == 0);
    CHECK(user[0].t1Samples == 480); // 用户段原样
    CHECK(user[1].t0Samples == 480); // auto 段没被拉长
    CHECK(user[1].t1Samples == 96000);

    // 锁定的 auto 段同样不参与。
    std::vector<AnalysisSegment> lockedCase;
    lockedCase.push_back(segAt(0, 480, 5.0, 0.0, Origin::Auto, true)); // 10ms,锁定
    lockedCase.push_back(segAt(480, 96000, 0.0));
    mergeShortAutoSegments(lockedCase, 2000.0, kSr);
    REQUIRE(lockedCase.size() == 2);
    CHECK(lockedCase[0].t0Samples == 0);
    CHECK(lockedCase[0].t1Samples == 480);

    // 短 auto 段夹在两条用户段之间:时间上相接但两侧都不是 auto ⇒ 原地保留
    // (用户段屏障;与生产侧「clash 过滤丢掉与用户段重叠的新段」留下的空档同形)。
    std::vector<AnalysisSegment> sandwich;
    sandwich.push_back(segAt(0, 96000, 0.0, 0.0, Origin::UserCreated)); // 用户段
    sandwich.push_back(segAt(96000, 96480, -60.0)); // 10ms,短 auto,两侧都是用户段
    sandwich.push_back(segAt(96480, 192000, 1.0, 0.0, Origin::UserEdited)); // 用户段
    mergeShortAutoSegments(sandwich, 2000.0, kSr);
    REQUIRE(sandwich.size() == 3);
    CHECK(sandwich[1].t0Samples == 96000);
    CHECK(sandwich[1].t1Samples == 96480);
    CHECK(sandwich[1].pan == Catch::Approx(-60.0));
}
