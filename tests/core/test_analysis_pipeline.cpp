// SPDX-License-Identifier: GPL-3.0-or-later
// test_analysis_pipeline —— AnalysisPipeline 编排层单测(离线、合成特征)。
// 分析全链此前从未接线(handleAnalyze 是 T29 占位),v4 实测 P0-1「分析中卡死」即由此而来。
// 本文件只压编排层:喂合成的 kw 序列,断言 VAD → 谷切分 → 全局区间 → 指派 能真的出段。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "analysis/AnalysisPipeline.h"

using namespace scvb::analysis;

namespace
{

constexpr double kSr = 48000.0;
constexpr int kHopMs = 10;

// 造一条「有声/静音交替」的 kw_ms 序列(线性 K 加权均方)。
// loudMs/quietMs 各若干轮;有声段 kw = loudKw,静音段 kw ≈ 0。
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
    cfg.rangeEndSample = static_cast<std::int64_t>(numHops) * static_cast<std::int64_t>(kHopMs * kSr / 1000.0);
    for (int t = 0; t < kPipelineTracks; ++t)
    {
        cfg.tracks[static_cast<std::size_t>(t)].enabled = (t < activeTracks);
    }
    return cfg;
}

} // namespace

TEST_CASE("PIPE-1 单轨有声/静音交替 → 切出多段", "[analysis][pipeline][t37]")
{
    // 每轮 80 hop 有声(0.8s)+ 60 hop 静音(0.6s),共 5 轮。
    auto feat = makeAlternating(5, 80, 60, 0.05f);
    const std::size_t n = feat.kwMs.size();

    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = feat;

    const auto cfg = makeConfig(n, 1);
    const auto res = runAnalysisPipeline(features, cfg);

    CHECK_FALSE(res.cancelled);
    CHECK(res.tracksTouched == 1);
    CHECK(res.intervals > 0);
    REQUIRE_FALSE(res.segments[0].empty()); // ← 核心:真的出段了

    for (std::size_t i = 0; i < res.segments[0].size(); ++i)
    {
        const auto& s = res.segments[0][i];
        CHECK(s.t1Samples > s.t0Samples);
        CHECK(s.pan >= -100.0);
        CHECK(s.pan <= 100.0);
        if (i > 0)
        {
            CHECK(s.t0Samples >= res.segments[0][i - 1].t1Samples);
        }
    }
}

TEST_CASE("PIPE-2 多轨 → 全局区间 + 指派,各轨都拿到 pan", "[analysis][pipeline][t37]")
{
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    // 三轨错开发声,保证全局区间里活跃集合会变化。
    features[0] = makeAlternating(4, 100, 60, 0.05f);
    features[1] = makeAlternating(4, 60, 100, 0.04f);
    features[2] = makeAlternating(4, 80, 80, 0.03f);
    const std::size_t n = features[0].kwMs.size();

    auto cfg = makeConfig(n, 3);
    const auto res = runAnalysisPipeline(features, cfg);

    CHECK(res.tracksTouched == 3);
    CHECK(res.intervals > 0);
    for (int t = 0; t < 3; ++t)
    {
        CHECK_FALSE(res.segments[static_cast<std::size_t>(t)].empty());
    }

    // 同一时刻多轨同时发声时,pan 不应全挤在正中(§5 指派的意义所在)。
    bool anyNonZeroPan = false;
    for (int t = 0; t < 3 && !anyNonZeroPan; ++t)
    {
        for (const auto& s : res.segments[static_cast<std::size_t>(t)])
        {
            if (std::abs(s.pan) > 1.0)
            {
                anyNonZeroPan = true;
                break;
            }
        }
    }
    CHECK(anyNonZeroPan);
}

TEST_CASE("PIPE-3 关掉的轨不产段;无覆盖的轨不产段", "[analysis][pipeline][t37]")
{
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = makeAlternating(4, 80, 60, 0.05f);
    features[1] = makeAlternating(4, 80, 60, 0.05f);
    const std::size_t n = features[0].kwMs.size();

    auto cfg = makeConfig(n, 2);
    cfg.tracks[1].enabled = false; // 轨 2 被关

    const auto res = runAnalysisPipeline(features, cfg);
    CHECK_FALSE(res.segments[0].empty());
    CHECK(res.segments[1].empty()); // 关掉的轨:一段都不该有

    // 轨 3 从未有覆盖 → 同样不产段。
    CHECK(res.segments[2].empty());
}

TEST_CASE("PIPE-4 取消:立即返回且标记 cancelled", "[analysis][pipeline][t37]")
{
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = makeAlternating(20, 80, 60, 0.05f);
    const std::size_t n = features[0].kwMs.size();
    const auto cfg = makeConfig(n, 1);

    const auto res = runAnalysisPipeline(features, cfg, {}, [] { return true; });
    CHECK(res.cancelled);
}
