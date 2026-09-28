// SPDX-License-Identifier: GPL-3.0-or-later
//
// [J146] 拖动档预览的计算核(src/core/analysis/VadPreview.h)—— 与分析流水线 S1 是不是同一份实现。
//
// 这组用例钉三件事:
//   ① `runEnergyVad` 拆成「基准(与参数无关)+ 按参数跑那一半」之后逐位不变;
//   ② 预览核在**同一份缓存**上换任意参数跑出来的后验与 S1 段,与流水线 / 现场重算逐位相同 ——
//      这就是「预览看到的 = 松手那一趟会算出来的」在 core 层的证据(host 用例 [j146] 再在真
//      FrameStore 上钉一遍 VAD 列);
//   ③ `ChannelFrames::mutationSeq()` 的记账口径:改特征的入口都 +1,只动 vadP 的入口不动 ——
//      预览缓存靠它判「还能不能用」,记错一边就是「用旧特征出预览」或「缓存永远不命中」。

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "analysis/AnalysisPipeline.h"
#include "analysis/EnergyVad.h"
#include "analysis/FrameStore.h"
#include "analysis/VadPreview.h"

namespace
{

using scvb::analysis::SegmentationParams;
using scvb::analysis::VadParams;
using scvb::analysis::VadSegment;

constexpr int kHops = 3000; // 30 s @ 10ms hop
constexpr double kHopSec = 0.01;
constexpr double kSr = 48000.0;

float lufsToKw(double lufs)
{
    return static_cast<float>(std::pow(10.0, (lufs + 0.691) / 10.0));
}

// 素材:底噪 −60;一段 14 s 的长乐句(> maxSegment 8 s,中间一个 −30 的谷 ⇒ 谷切分那条路必走);
// 一段 −40 的轻乐句(门限深一点才算有声 ⇒ 阈值有牙);一段 −15 的短乐句。
std::vector<float> makeKw()
{
    std::vector<float> kw(static_cast<std::size_t>(kHops), lufsToKw(-60.0));
    const auto fill = [&kw](int a, int b, double lufs) {
        for (int h = a; h < b; ++h)
            kw[static_cast<std::size_t>(h)] = lufsToKw(lufs);
    };
    fill(200, 1600, -12.0);
    fill(850, 880, -30.0); // 长乐句中间的谷
    fill(2000, 2300, -40.0);
    fill(2500, 2700, -15.0);
    return kw;
}

struct ParamSet
{
    VadParams vad;
    SegmentationParams seg;
};

std::vector<ParamSet> paramSweep()
{
    std::vector<ParamSet> out;
    const float depths[] = {30.0f, 20.0f, 45.0f, 8.0f};
    const float hysts[] = {6.0f, 3.0f, 12.0f};
    const int hangs[] = {250, 100, 600};
    const int pads[][2] = {{120, 200}, {20, 50}, {400, 400}};
    const int minSegs[] = {120, 50, 2000};
    const double sens[] = {50.0, 0.0, 100.0};
    for (int i = 0; i < 9; ++i)
    {
        ParamSet p;
        p.vad.thresholdDb = depths[i % 4];
        p.vad.hysteresisDb = hysts[i % 3];
        p.vad.hangoverMs = hangs[(i + 1) % 3];
        p.vad.paddingPreMs = pads[i % 3][0];
        p.vad.paddingPostMs = pads[i % 3][1];
        p.vad.minSegmentMs = minSegs[(i + 2) % 3];
        p.seg.minSegmentMs = static_cast<double>(p.vad.minSegmentMs);
        p.seg.sensitivity = sens[i % 3];
        out.push_back(p);
    }
    return out;
}

bool sameSegs(const std::vector<VadSegment>& a, const std::vector<VadSegment>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].startHop != b[i].startHop || a[i].endHop != b[i].endHop)
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("[j146] runEnergyVad ≡ computeVadBaseline + runEnergyVadOnBaseline(段与后验逐位相同)", "[j146][vad]")
{
    const auto kw = makeKw();
    const auto base = scvb::analysis::computeVadBaseline(kw.data(), kw.size());
    REQUIRE(base.l.size() == kw.size());
    for (const auto& p : paramSweep())
    {
        INFO("depth=" << p.vad.thresholdDb << " hyst=" << p.vad.hysteresisDb << " hang=" << p.vad.hangoverMs);
        std::vector<float> postA(kw.size()), postB(kw.size());
        const auto a = scvb::analysis::runEnergyVad(kw.data(), kw.size(), 7, p.vad, postA.data());
        const auto b = scvb::analysis::runEnergyVadOnBaseline(base, 7, p.vad, postB.data());
        CHECK(sameSegs(a.segments, b.segments));
        CHECK(a.guard == b.guard);
        CHECK(postA == postB);
    }
    // 空输入:两条路都回空结果、守卫 Normal。
    const auto emptyA = scvb::analysis::runEnergyVad(kw.data(), 0, 0, VadParams{});
    const auto emptyB = scvb::analysis::runEnergyVadOnBaseline(scvb::analysis::VadBaseline{}, 0, VadParams{});
    CHECK(emptyA.segments.empty());
    CHECK(emptyB.segments.empty());
    CHECK(emptyB.guard == scvb::analysis::VadGuard::Normal);
}

TEST_CASE("[j146] 预览核与流水线 S1 同一份实现:同一份缓存换 9 组参数,后验 / S1 段逐位相同", "[j146][vad]")
{
    const auto kw = makeKw();
    auto cache = scvb::analysis::buildVadPreviewTrackCache(kw.data(), kw.size());
    REQUIRE(cache.hops() == kw.size());
    REQUIRE(cache.envDb.size() == kw.size());

    bool sawValleyCut = false; // 非空过:谷切分那条路至少走过一次
    std::set<std::vector<std::pair<std::int64_t, std::int64_t>>> distinct; // 非空过:参数真的改变了结果
    for (const auto& p : paramSweep())
    {
        INFO("depth=" << p.vad.thresholdDb << " sens=" << p.seg.sensitivity << " minSeg=" << p.vad.minSegmentMs);

        // 流水线(松手那一趟走的就是它):单轨、整窗、全覆盖。
        std::array<scvb::analysis::PipelineTrackFeatures, scvb::analysis::kPipelineTracks> feats{};
        feats[0].kwMs = kw;
        feats[0].peak.assign(kw.size(), 0.5f);
        feats[0].covered.assign(kw.size(), 1u);
        feats[0].anyCovered = true;
        scvb::analysis::PipelineConfig cfg;
        cfg.sampleRate = kSr;
        cfg.rangeStartSample = 0;
        cfg.rangeEndSample = static_cast<std::int64_t>(kHops) * 480;
        cfg.vad = p.vad;
        cfg.segmentation = p.seg;
        const auto pipe = scvb::analysis::runAnalysisPipeline(feats, cfg);
        REQUIRE(pipe.vadPosterior[0].size() == kw.size());

        std::vector<float> post(kw.size());
        const auto spans = scvb::analysis::runVadPreviewTrack(cache, 0, p.vad, p.seg, kHopSec, post.data());
        CHECK(post == pipe.vadPosterior[0]); // ★ 后验:VAD 着色的数据源

        // S1 段:现场从原始 kw 重算(不带缓存)——流水线里就是这两步。
        const auto vr = scvb::analysis::runEnergyVad(kw.data(), kw.size(), 0, p.vad);
        std::vector<float> freshEnv;
        const auto s1 = scvb::analysis::splitLongVadSegments(vr.segments, 0, p.seg, kHopSec, kw.data(), kw.size(),
                                                             freshEnv, nullptr);
        CHECK(sameSegs(spans, s1)); // ★ S1 段:预览虚影的数据源

        if (s1.size() > vr.segments.size())
            sawValleyCut = true;
        std::vector<std::pair<std::int64_t, std::int64_t>> key;
        for (const auto& s : spans)
            key.emplace_back(s.startHop, s.endHop);
        distinct.insert(key);
    }
    CHECK(sawValleyCut);
    CHECK(distinct.size() >= 3);
}

TEST_CASE("[j146] 预览缓存与参数无关:同一组参数前后两次结果相同(中间换过别的参数)", "[j146][vad]")
{
    const auto kw = makeKw();
    auto cache = scvb::analysis::buildVadPreviewTrackCache(kw.data(), kw.size());
    const auto sweep = paramSweep();
    std::vector<float> p1(kw.size()), p2(kw.size()), p3(kw.size());
    const auto a = scvb::analysis::runVadPreviewTrack(cache, 0, sweep[0].vad, sweep[0].seg, kHopSec, p1.data());
    const auto b = scvb::analysis::runVadPreviewTrack(cache, 0, sweep[3].vad, sweep[3].seg, kHopSec, p2.data());
    const auto c = scvb::analysis::runVadPreviewTrack(cache, 0, sweep[0].vad, sweep[0].seg, kHopSec, p3.data());
    CHECK_FALSE(sameSegs(a, b)); // 前置:中间那组真的不一样
    CHECK(sameSegs(a, c));
    CHECK(p1 == p3);
    // 空缓存:回空、不读后验缓冲
    scvb::analysis::VadPreviewTrackCache empty;
    CHECK(scvb::analysis::runVadPreviewTrack(empty, 0, sweep[0].vad, sweep[0].seg, kHopSec, nullptr).empty());
}

TEST_CASE("[j146] ChannelFrames::mutationSeq:改特征的入口都 +1,只动 vadP 的入口不动", "[j146][framestore]")
{
    scvb::analysis::ChannelFrames f;
    f.setReadOnly(false);
    auto seq = f.mutationSeq();
    const auto bumped = [&f, &seq]() {
        const bool b = f.mutationSeq() != seq;
        seq = f.mutationSeq();
        return b;
    };

    f.write(10, 0.1f, 0.2f);
    CHECK(bumped()); // write(真写进去)

    const float post[4] = {0.9f, 0.9f, 0.9f, 0.9f};
    f.setVadPosteriorRange(scvb::analysis::HopRange{10, 11}, post, 1);
    CHECK_FALSE(bumped()); // 只动 vadP
    f.setVadP(10, 200);
    CHECK_FALSE(bumped()); // 只动 vadP

    f.setReadOnly(true);
    f.write(11, 0.1f, 0.2f);
    CHECK_FALSE(bumped()); // 被 readOnly 丢弃的不算
    f.setReadOnly(false);
    f.setGate(scvb::analysis::HopRange{0, 5});
    f.write(12, 0.1f, 0.2f);
    CHECK_FALSE(bumped()); // 被 gate 丢弃的不算
    f.setGate(scvb::analysis::HopRange{0, 1000});

    f.restoreHop(20, -3000, -600, 0);
    CHECK(bumped());
    f.addCoverage(scvb::analysis::HopRange{20, 21});
    CHECK(bumped());
    f.invalidate(scvb::analysis::HopRange{20, 21});
    CHECK(bumped());
    f.reset();
    CHECK(bumped());
}

// 隐藏用例([.][j146cost]):拖动档预览的单次代价实测面(契约 §1.18「目标 <50ms」)。ctest 默认不跑 ——
// 门禁里不该有按墙钟判红的断言(与 test_viz_publish_cost.cpp 的 [vizcost] 同一纪律)。
// 手工:`scvb_tests.exe "[j146cost]"`,读 WARN 行。量的是计算核(VAD + S1 + 后验),不含 FrameStore 取样
// (那一段只在建缓存时付一次,量级与松手那一趟 startAnalysis 的取样相同)。
TEST_CASE("[j146] 预览单次代价:15 轨 × 4 分钟 / 60 分钟,建缓存一次 vs 命中缓存每次 vs 不缓存每次", "[.][j146cost]")
{
    constexpr int kTracks = 15;
    for (const int minutes : {4, 60})
    {
        const int kN = minutes * 6000; // @ 10ms
        std::vector<std::vector<float>> kws(static_cast<std::size_t>(kTracks));
        for (int t = 0; t < kTracks; ++t)
        {
            auto& kw = kws[static_cast<std::size_t>(t)];
            kw.assign(static_cast<std::size_t>(kN), lufsToKw(-60.0));
            // 每轨:唱 9 s(中间一个谷)、停 2 s,响度逐轨错开
            for (int h = 0; h < kN; ++h)
            {
                const int ph = (h + t * 37) % 1100;
                if (ph < 900)
                    kw[static_cast<std::size_t>(h)] = lufsToKw((ph > 430 && ph < 450) ? -32.0 : -12.0 - (t % 5) * 3.0);
            }
        }
        using clk = std::chrono::steady_clock;
        const auto ms = [](clk::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };

        const auto t0 = clk::now();
        std::vector<scvb::analysis::VadPreviewTrackCache> caches;
        for (const auto& kw : kws)
            caches.push_back(scvb::analysis::buildVadPreviewTrackCache(kw.data(), kw.size()));
        const auto t1 = clk::now();

        const auto sweep = paramSweep();
        std::vector<float> post(static_cast<std::size_t>(kN));
        std::size_t sink = 0;
        constexpr int kReps = 20;
        for (int r = 0; r < kReps; ++r)
        {
            const auto& p = sweep[static_cast<std::size_t>(r) % sweep.size()];
            for (auto& c : caches)
                sink += scvb::analysis::runVadPreviewTrack(c, 0, p.vad, p.seg, kHopSec, post.data()).size();
        }
        const auto t2 = clk::now();
        for (int r = 0; r < kReps; ++r)
        {
            const auto& p = sweep[static_cast<std::size_t>(r) % sweep.size()];
            for (const auto& kw : kws)
            {
                const auto vr = scvb::analysis::runEnergyVad(kw.data(), kw.size(), 0, p.vad, post.data());
                std::vector<float> env;
                sink += scvb::analysis::splitLongVadSegments(vr.segments, 0, p.seg, kHopSec, kw.data(), kw.size(), env,
                                                             nullptr)
                            .size();
            }
        }
        const auto t3 = clk::now();
        WARN("15 tracks x " << minutes << " min: build cache once = " << ms(t1 - t0)
                            << " ms; cached per call = " << ms(t2 - t1) / kReps
                            << " ms; uncached per call = " << ms(t3 - t2) / kReps << " ms (spans " << sink << ")");
        CHECK(sink > 0);
    }
}
