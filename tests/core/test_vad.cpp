// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "analysis/EnergyVad.h"
#include "analysis/FeatureExtractor.h"
#include "analysis/LoudnessMode.h"

namespace
{

using scvb::analysis::AnalysisSettingsStale;
using scvb::analysis::LoudnessMode;
using scvb::analysis::VadGuard;
using scvb::analysis::VadParams;
using scvb::analysis::VadResult;
using scvb::analysis::VadSegment;

constexpr double kLufsOffset = -0.691;
constexpr int kNumHops = 1000; // 10s @ 48k,10ms hop

// ℓ(LUFS)→ kw_ms(线性能量):ℓ = −0.691 + 10·log10(kw_ms)。
float lufsToEnergy(double lufs)
{
    return static_cast<float>(std::pow(10.0, (lufs - kLufsOffset) / 10.0));
}

// 以 −60 LUFS 底噪 + 若干响度区间构造 kw_ms(02 §2.8「特征域直接构造 ℓ,免音频」)。
std::vector<float> makeKwMs(const std::vector<std::pair<int, int>>& loud, double loudDb, int n = kNumHops)
{
    std::vector<float> kw(static_cast<std::size_t>(n), lufsToEnergy(-60.0));
    const float v = lufsToEnergy(loudDb);
    for (const auto& seg : loud)
    {
        for (int k = seg.first; k < seg.second && k < n; ++k)
            kw[static_cast<std::size_t>(k)] = v;
    }
    return kw;
}

// 段边界 ±1 hop(2-hop 平滑在边界处 ±1 hop,02 §2.8 VAD-1 的「±1 hop」口径)。
void checkSegment(const VadSegment& seg, int64_t start, int64_t end, int idx)
{
    INFO("segment[" << idx << "] = [" << seg.startHop << ", " << seg.endHop << ") expected [" << start << ", " << end
                    << ") ±1 hop");
    CHECK(seg.startHop >= start - 1);
    CHECK(seg.startHop <= start + 1);
    CHECK(seg.endHop >= end - 1);
    CHECK(seg.endHop <= end + 1);
}

void checkSegments(const std::vector<VadSegment>& got, const std::vector<std::pair<int64_t, int64_t>>& expected)
{
    REQUIRE(got.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        checkSegment(got[i], expected[i].first, expected[i].second, static_cast<int>(i));
}

int64_t totalDuration(const std::vector<VadSegment>& segs)
{
    int64_t total = 0;
    for (const VadSegment& s : segs)
        total += s.endHop - s.startHop;
    return total;
}

bool sameResult(const VadResult& a, const VadResult& b)
{
    if (a.guard != b.guard || a.segments.size() != b.segments.size())
        return false;
    for (std::size_t i = 0; i < a.segments.size(); ++i)
    {
        if (a.segments[i].startHop != b.segments[i].startHop || a.segments[i].endHop != b.segments[i].endHop)
            return false;
    }
    return true;
}

constexpr double kPi = 3.14159265358979323846264338327950288;

std::vector<float> makeSine(double fs, double freq, double amp, int seconds)
{
    const std::size_t n = static_cast<std::size_t>(fs * seconds);
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / fs;
        out[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * t));
    }
    return out;
}

std::vector<scvb::analysis::FeatFrame> runExtractor(scvb::analysis::FeatureExtractor& ex, const float* const* ch,
                                                    int numSamples)
{
    const int totalCap = numSamples / ex.hopSize() + 2;
    std::vector<scvb::analysis::FeatFrame> frames(static_cast<std::size_t>(totalCap));

    int written = 0;
    int pos = 0;
    while (pos < numSamples)
    {
        int len = numSamples - pos;
        const float* blockCh[2] = {nullptr, nullptr};
        for (int c = 0; c < ex.channels(); ++c)
            blockCh[c] = ch[c] + pos;
        const int got = ex.processBlock(blockCh, len, frames.data() + written, totalCap - written);
        written += got;
        pos += len;
    }
    frames.resize(static_cast<std::size_t>(written));
    return frames;
}

} // namespace

TEST_CASE("VAD-1: 单唱段 core 与 padding", "[vad]")
{
    const auto kw = makeKwMs({{100, 200}}, -20.0);
    const VadParams p;
    const VadResult res = scvb::analysis::runEnergyVad(kw.data(), kw.size(), 0, p);

    // core=[1.00,2.00);最终段 [0.88,2.20)s(pre 120ms/post 200ms),±1 hop。
    REQUIRE(res.guard == VadGuard::Normal);
    checkSegments(res.segments, {{88, 220}});
}

TEST_CASE("VAD-2a: 两唱段间隙 300ms padding 重叠合并", "[vad]")
{
    const auto kw = makeKwMs({{100, 200}, {230, 330}}, -20.0);
    const VadParams p;
    const VadResult res = scvb::analysis::runEnergyVad(kw.data(), kw.size(), 0, p);

    // 300ms=30 hop ≥ hangover 25 → 两 core;padding 后重叠 → P3 合并 → 1 段。
    checkSegments(res.segments, {{88, 350}});
}

TEST_CASE("VAD-2b: 两唱段间隙 600ms 不合并", "[vad]")
{
    const auto kw = makeKwMs({{100, 200}, {260, 360}}, -20.0);
    const VadParams p;
    const VadResult res = scvb::analysis::runEnergyVad(kw.data(), kw.size(), 0, p);

    // padding 后 gap=280ms ≥ mergeGap 150ms → 2 段。
    checkSegments(res.segments, {{88, 220}, {248, 380}});
}

TEST_CASE("VAD-2c: 两唱段间隙 200ms 不出段", "[vad]")
{
    const auto kw = makeKwMs({{100, 200}, {220, 320}}, -20.0);
    const VadParams p;
    const VadResult res = scvb::analysis::runEnergyVad(kw.data(), kw.size(), 0, p);

    // 20 hop < hangover → 不出段 → 1 个 core [1.0,3.2);最终段 [0.88,3.40)s。
    REQUIRE(res.segments.size() == 1);
    checkSegment(res.segments[0], 88, 340, 0);
}

TEST_CASE("VAD-3: 50ms 尖峰 P1 丢短先于 padding", "[vad]")
{
    const auto kw = makeKwMs({{100, 105}}, -20.0); // 50ms
    const VadParams p;
    const VadResult res = scvb::analysis::runEnergyVad(kw.data(), kw.size(), 0, p);

    // core 50ms < minSegment 120ms → 丢弃;padding 之前判定(P1 先于 P2)。
    REQUIRE(res.segments.empty());
    REQUIRE(res.guard == VadGuard::Normal);
}

TEST_CASE("VAD-4: 全程静音动态范围守卫(疑似无人声)", "[vad]")
{
    std::vector<float> kw(static_cast<std::size_t>(kNumHops), lufsToEnergy(-60.0));
    const VadParams p;
    const VadResult res = scvb::analysis::runEnergyVad(kw.data(), kw.size(), 0, p);

    // A − F < 12 且 A < −50 → 空段列表 + 警告。
    REQUIRE(res.segments.empty());
    REQUIRE(res.guard == VadGuard::SilentRegion);
    REQUIRE(res.warned());
    REQUIRE(std::string(res.warningMessage()) == "该轨疑似无人声");
}

TEST_CASE("VAD-守卫-大声: 全程有声 → 单段覆盖选区", "[vad]")
{
    std::vector<float> kw(static_cast<std::size_t>(kNumHops), lufsToEnergy(-20.0));
    const VadParams p;
    const VadResult res = scvb::analysis::runEnergyVad(kw.data(), kw.size(), 0, p);

    // A > −30 → 整选区视为一个有声段 + 警告。
    REQUIRE(res.guard == VadGuard::LoudRegion);
    REQUIRE(res.segments.size() == 1);
    REQUIRE(res.segments[0].startHop == 0);
    REQUIRE(res.segments[0].endHop == kNumHops);
    REQUIRE(std::string(res.warningMessage()) == "该轨几乎全程有声");
}

TEST_CASE("VAD-守卫-窄动态: 动态范围过小 → 照常执行 + 警告", "[vad]")
{
    std::vector<float> kw(static_cast<std::size_t>(kNumHops), lufsToEnergy(-40.0));
    const VadParams p;
    const VadResult res = scvb::analysis::runEnergyVad(kw.data(), kw.size(), 0, p);

    // −50 ≤ A ≤ −30 → 照常执行 + 警告(本构造恒定 −40,无 hop 越阈值 → 空)。
    REQUIRE(res.guard == VadGuard::NarrowDynamicRange);
    REQUIRE(res.segments.empty());
    REQUIRE(std::string(res.warningMessage()) == "动态范围过小,识别可能不可靠");
}

TEST_CASE("VAD-5: 阈值重跑幂等与 s 扫描单调性", "[vad]")
{
    // (a) 幂等:同一输入 100 次逐位一致。
    const auto kw = makeKwMs({{100, 200}, {300, 500}}, -20.0);
    const VadParams p;
    const VadResult first = scvb::analysis::runEnergyVad(kw.data(), kw.size(), 0, p);
    for (int i = 0; i < 100; ++i)
    {
        const VadResult r = scvb::analysis::runEnergyVad(kw.data(), kw.size(), 0, p);
        INFO("run " << i);
        REQUIRE(sameResult(first, r));
    }

    // (b) s 从 0→100 扫描总有声时长单调不减(thresholdDb = 15 + 0.3·s,02 §2.5)。
    // 构造:quiet [0,400) @ −80,ramp [400,1000) ℓ 从 −50 → −20。
    std::vector<float> kw2(static_cast<std::size_t>(kNumHops), lufsToEnergy(-80.0));
    for (int k = 400; k < kNumHops; ++k)
    {
        const double lufs = -50.0 + static_cast<double>(k - 400) * (30.0 / 600.0);
        kw2[static_cast<std::size_t>(k)] = lufsToEnergy(lufs);
    }

    int64_t prev = -1;
    for (int s = 0; s <= 100; s += 10)
    {
        VadParams pp;
        pp.thresholdDb = 15.0f + 0.3f * static_cast<float>(s);
        const VadResult r = scvb::analysis::runEnergyVad(kw2.data(), kw2.size(), 0, pp);
        const int64_t total = totalDuration(r.segments);
        INFO("s=" << s << " thresholdDb=" << pp.thresholdDb << " total=" << total);
        if (prev >= 0)
            REQUIRE(total >= prev);
        prev = total;
    }
}

TEST_CASE("VAD-6: 稀疏轨活跃候选子集基准(P95)", "[vad]")
{
    // 10s 基座仅 [1.00,1.40)s @ −20(有效发声占比 4%),其余 −60。
    const auto kw = makeKwMs({{100, 140}}, -20.0);
    const VadParams p;
    const VadResult res = scvb::analysis::runEnergyVad(kw.data(), kw.size(), 0, p);

    // S = 唱段 hop → A = −20、T_on = −50;core=[1.00,1.40);最终 1 段 [0.88,1.60)s。
    // 全 hop P95 口径会把本例误判为「疑似无人声」输出空表 —— 子集基准防此回归。
    REQUIRE(res.guard == VadGuard::Normal);
    REQUIRE(!res.segments.empty());
    checkSegments(res.segments, {{88, 160}});
}

TEST_CASE("IVadBackend: EnergyVad 实现 v1 接口", "[vad]")
{
    using scvb::analysis::EnergyVad;
    EnergyVad vad;

    REQUIRE(std::string(vad.name()) == "energy_v1");
    REQUIRE(vad.worksFromFeatures());

    // computeFromFeatures 输出 p_raw(不截断):唱段中央 p_raw ≥ 1,底噪 p_raw ≤ 0(02 §2.7)。
    const auto kw = makeKwMs({{100, 200}}, -20.0);
    std::vector<float> raw(kw.size());
    vad.computeFromFeatures(kw.data(), kw.size(), VadParams{}, raw.data());
    REQUIRE(raw[150] >= 1.0f);
    REQUIRE(raw[50] <= 0.0f);

    // v2 流式接口:energy_v1 不实现。
    REQUIRE(!vad.streamBegin(48000.0));
    REQUIRE(vad.streamDrainPosterior(nullptr, 0) == 0);
    vad.streamProcess(nullptr, 0);
    vad.streamEnd();
}

TEST_CASE("J69-1: loudness_mode 三档 golden 向量可区分且确定", "[loudness][mode]")
{
    // 02 §4.3 LOUD-5 向量。
    const std::vector<float> kw = {0.01f, 0.09f};
    const std::vector<float> peak = {0.2f, 0.5f};

    const double ki = scvb::analysis::computeLoudnessMetric(LoudnessMode::KIntegrated, kw.data(), peak.data(), 2);
    const double rms = scvb::analysis::computeLoudnessMetric(LoudnessMode::Rms, kw.data(), peak.data(), 2);
    const double pk = scvb::analysis::computeLoudnessMetric(LoudnessMode::PeakDbfs, kw.data(), peak.data(), 2);

    REQUIRE(ki == Approx(-13.7013).margin(1e-4));
    REQUIRE(rms == Approx(-13.9794).margin(1e-4));
    REQUIRE(pk == Approx(-6.0206).margin(1e-4));

    // 同输入不同 mode 可区分。
    REQUIRE(ki != rms);
    REQUIRE(rms != pk);
    REQUIRE(ki != pk);
    REQUIRE(rms < ki); // 有动态 ⇒ RMS 低于能量口径(Jensen)
    REQUIRE(pk > ki);

    // 确定性:同 mode 重跑 100 次逐位一致。
    for (int i = 0; i < 100; ++i)
    {
        REQUIRE(scvb::analysis::computeLoudnessMetric(LoudnessMode::KIntegrated, kw.data(), peak.data(), 2) == ki);
        REQUIRE(scvb::analysis::computeLoudnessMetric(LoudnessMode::Rms, kw.data(), peak.data(), 2) == rms);
        REQUIRE(scvb::analysis::computeLoudnessMetric(LoudnessMode::PeakDbfs, kw.data(), peak.data(), 2) == pk);
    }

    // 恒等断言(02 §4.3):全 hop 等值(无动态)时 rms 恰比 k_integrated 高 0.691 dB;
    // 本例有动态 ⇒ rms − k_integrated = −0.278 dB。
    const std::vector<float> flat = {0.05f, 0.05f};
    const double flatKi = scvb::analysis::computeLoudnessMetric(LoudnessMode::KIntegrated, flat.data(), nullptr, 2);
    const double flatRms = scvb::analysis::computeLoudnessMetric(LoudnessMode::Rms, flat.data(), nullptr, 2);
    REQUIRE(flatRms - flatKi == Approx(0.691).margin(1e-4));
    REQUIRE(rms - ki == Approx(-0.278).margin(1e-3));
}

TEST_CASE("J69-2: L_seg 主口径在任意 mode 下逐位不变(反向断言)", "[loudness][mode]")
{
    // LOUD-1 / LOUD-2(02 §4.2)。
    const std::vector<float> kw1 = {0.1f, 0.2f};
    const std::vector<float> kw2 = {0.0f, 0.0f};

    const double l1 = scvb::analysis::computeLseg(kw1.data(), 2);
    const double l2 = scvb::analysis::computeLseg(kw2.data(), 2);
    REQUIRE(l1 == Approx(-8.9301).margin(1e-4));
    REQUIRE(l2 == Approx(-120.691).margin(0.01));

    // LOUD-3:任一段乘 10^(3/10) → 恰好 +3.0 LU。
    constexpr float kGain = 1.9952623f; // 10^(3/10)
    const std::vector<float> kw1g = {0.1f * kGain, 0.2f * kGain};
    REQUIRE(scvb::analysis::computeLseg(kw1g.data(), 2) - l1 == Approx(3.0).margin(1e-4));

    // 反向断言:切换 mode 后主口径逐位不变;KIntegrated 档 == computeLseg。
    const float peak[2] = {0.5f, 0.5f};
    const LoudnessMode modes[3] = {LoudnessMode::KIntegrated, LoudnessMode::Rms, LoudnessMode::PeakDbfs};
    for (const LoudnessMode m : modes)
    {
        AnalysisSettingsStale settings;
        settings.loudnessMode = m;
        const auto r = scvb::analysis::computeSegmentLoudness(settings, kw1.data(), peak, 2);

        INFO("mode=" << scvb::analysis::loudnessModeToString(m));
        REQUIRE(r.lseg == l1); // 主口径逐位不变
        REQUIRE(scvb::analysis::computeLseg(kw1.data(), 2) == l1);
        REQUIRE(scvb::analysis::computeLoudnessMetric(LoudnessMode::KIntegrated, kw1.data(), peak, 2) == l1);
    }

    // LOUD-4(全链路):997Hz 正弦 → 特征 → computeLseg ≈ −3.01,且不随 mode 变化。
    constexpr double fs = 48000.0;
    const auto sine = makeSine(fs, 997.0, 1.0, 5);
    const int n = static_cast<int>(sine.size());
    scvb::analysis::FeatureExtractor ex;
    ex.prepare(fs, 1);
    const float* ch[1] = {sine.data()};
    const auto frames = runExtractor(ex, ch, n);

    std::vector<float> kwFrames;
    kwFrames.reserve(frames.size());
    for (const auto& f : frames)
        kwFrames.push_back(f.kw_ms);
    const double l4 = scvb::analysis::computeLseg(kwFrames.data(), kwFrames.size());
    REQUIRE(l4 == Approx(-3.01).margin(0.05));
    for (const LoudnessMode m : modes)
    {
        AnalysisSettingsStale settings;
        settings.loudnessMode = m;
        REQUIRE(scvb::analysis::computeSegmentLoudness(settings, kwFrames.data(), nullptr, kwFrames.size()).lseg == l4);
    }
}

TEST_CASE("J69-3: 未知/越界 mode 回落默认档并记 warning", "[loudness][mode]")
{
    // 合法串。
    REQUIRE(scvb::analysis::parseLoudnessMode("k_integrated").mode == LoudnessMode::KIntegrated);
    REQUIRE(!scvb::analysis::parseLoudnessMode("k_integrated").fellBack);
    REQUIRE(scvb::analysis::parseLoudnessMode("rms").mode == LoudnessMode::Rms);
    REQUIRE(!scvb::analysis::parseLoudnessMode("rms").fellBack);
    REQUIRE(scvb::analysis::parseLoudnessMode("peak_dbfs").mode == LoudnessMode::PeakDbfs);
    REQUIRE(!scvb::analysis::parseLoudnessMode("peak_dbfs").fellBack);

    // 未知串 → 回落默认 + warning。
    {
        const auto r = scvb::analysis::parseLoudnessMode("lufs_s");
        REQUIRE(r.fellBack);
        REQUIRE(r.mode == LoudnessMode::KIntegrated);
        REQUIRE(r.warning != nullptr);
    }
    {
        const auto r = scvb::analysis::parseLoudnessMode("");
        REQUIRE(r.fellBack);
        REQUIRE(r.mode == LoudnessMode::KIntegrated);
        REQUIRE(r.warning != nullptr);
    }
    {
        const auto r = scvb::analysis::parseLoudnessMode(nullptr);
        REQUIRE(r.fellBack);
        REQUIRE(r.mode == LoudnessMode::KIntegrated);
        REQUIRE(r.warning != nullptr);
    }

    // 越界 int → 回落默认 + warning。
    REQUIRE(scvb::analysis::parseLoudnessMode(5).fellBack);
    REQUIRE(scvb::analysis::parseLoudnessMode(5).mode == LoudnessMode::KIntegrated);
    REQUIRE(scvb::analysis::parseLoudnessMode(-1).fellBack);
    REQUIRE(scvb::analysis::parseLoudnessMode(-1).mode == LoudnessMode::KIntegrated);

    // 回落不崩、不静默改主口径:回落档 == 默认档,computeLseg 不变。
    const std::vector<float> kw = {0.1f, 0.2f};
    const double l = scvb::analysis::computeLseg(kw.data(), 2);
    const auto fallback = scvb::analysis::parseLoudnessMode("bogus");
    REQUIRE(scvb::analysis::computeLoudnessMetric(fallback.mode, kw.data(), nullptr, 2) == l);
}

TEST_CASE("J69-4: 改 mode 触发全局 stale 标志", "[loudness][mode]")
{
    AnalysisSettingsStale s;
    REQUIRE(!s.stale()); // 初始 current == applied

    s.loudnessMode = LoudnessMode::Rms;
    REQUIRE(s.stale()); // 改 mode → stale

    s.markApplied(); // 全量分析完成
    REQUIRE(!s.stale());

    s.loudnessMode = LoudnessMode::PeakDbfs;
    REQUIRE(s.stale());

    s.markApplied();
    REQUIRE(!s.stale());
}
