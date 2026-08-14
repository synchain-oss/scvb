// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

#include <cmath>
#include <cstddef>
#include <vector>

#include "analysis/FeatureExtractor.h"
#include "analysis/Loudness.h"
#include "dsp/KWeighting.h"

namespace
{

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

// 三个正弦叠加的确定性任意素材(非周期相对 hop 长度)。
std::vector<float> makeMix(double fs, int seconds, double f1, double f2, double f3)
{
    const std::size_t n = static_cast<std::size_t>(fs * seconds);
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / fs;
        out[i] = static_cast<float>(0.3 * std::sin(2.0 * kPi * f1 * t) + 0.2 * std::sin(2.0 * kPi * f2 * t) +
                                    0.1 * std::sin(2.0 * kPi * f3 * t));
    }
    return out;
}

// 以给定块长序列(循环)推流并收集全部帧。lens 须非空;单块推流传 {numSamples}。
std::vector<scvb::analysis::FeatFrame> runExtractor(scvb::analysis::FeatureExtractor& ex, const float* const* ch,
                                                    int numSamples, const std::vector<int>& lens)
{
    const int totalCap = numSamples / ex.hopSize() + 2;
    std::vector<scvb::analysis::FeatFrame> frames(static_cast<std::size_t>(totalCap));

    int written = 0;
    int pos = 0;
    std::size_t li = 0;
    while (pos < numSamples)
    {
        int len = lens[li % lens.size()];
        if (len > numSamples - pos)
            len = numSamples - pos;

        const float* blockCh[2] = {nullptr, nullptr};
        for (int c = 0; c < ex.channels(); ++c)
            blockCh[c] = ch[c] + pos;

        const int got = ex.processBlock(blockCh, len, frames.data() + written, totalCap - written);
        written += got;
        pos += len;
        ++li;
    }

    frames.resize(static_cast<std::size_t>(written));
    return frames;
}

void splitFrames(const std::vector<scvb::analysis::FeatFrame>& frames, std::vector<float>& kwMs,
                 std::vector<float>& peak)
{
    kwMs.resize(frames.size());
    peak.resize(frames.size());
    for (std::size_t k = 0; k < frames.size(); ++k)
    {
        kwMs[k] = frames[k].kw_ms;
        peak[k] = frames[k].peak;
    }
}

} // namespace

TEST_CASE("LOUD-1: 两 hop kw_ms={0.1,0.2} → z=0.15 / L=−8.9301", "[loudness]")
{
    const float kwMs[] = {0.1f, 0.2f};
    const float peak[] = {0.5f, 0.5f};

    const auto v = scvb::analysis::segmentLoudness(kwMs, peak, 2, scvb::analysis::LoudnessMode::kIntegrated);

    REQUIRE(v.z == Approx(0.15).margin(1e-12));
    REQUIRE(v.level == Approx(-8.9301).margin(1e-4));
}

TEST_CASE("LOUD-2: kw_ms 全 0 → floor −120.69,有限不 NaN/Inf", "[loudness]")
{
    const float kwMs[] = {0.0f, 0.0f, 0.0f};
    const float peak[] = {0.0f, 0.0f, 0.0f};

    const auto v = scvb::analysis::segmentLoudness(kwMs, peak, 3, scvb::analysis::LoudnessMode::kIntegrated);

    REQUIRE(v.z == 0.0);
    REQUIRE(v.level == Approx(-120.69).margin(0.01));
    REQUIRE(std::isfinite(v.level));
    REQUIRE(!std::isnan(v.level));
}

TEST_CASE("LOUD-3: 段乘 10^(u/10)(u=+3) → L_seg 恰好 +3.0 LU", "[loudness]")
{
    const float kwMs[] = {0.05f, 0.1f, 0.3f, 0.2f};
    const float peak[] = {0.4f, 0.4f, 0.4f, 0.4f};

    const auto base = scvb::analysis::segmentLoudness(kwMs, peak, 4, scvb::analysis::LoudnessMode::kIntegrated);

    const double gain = std::pow(10.0, 3.0 / 10.0);
    float kwMsScaled[4];
    for (int k = 0; k < 4; ++k)
        kwMsScaled[k] = static_cast<float>(static_cast<double>(kwMs[k]) * gain);

    const auto scaled = scvb::analysis::segmentLoudness(kwMsScaled, peak, 4, scvb::analysis::LoudnessMode::kIntegrated);

    REQUIRE(scaled.level - base.level == Approx(3.0).margin(1e-6));
    REQUIRE(scaled.z == Approx(base.z * gain).epsilon(1e-6));
}

TEST_CASE("LOUD-4: 997Hz 正弦全链与 KW-1 一致(全链路)", "[loudness]")
{
    constexpr double fs = 48000.0;
    const auto sine = makeSine(fs, 997.0, 1.0, 5);
    const int n = static_cast<int>(sine.size());

    // KW-1 口径参考:全新 KWeighting,跳过前 100ms 后积分 L=−0.691+10log10(mean(y²))。
    scvb::dsp::KWeighting kw;
    kw.prepare(fs);
    const std::size_t skip = static_cast<std::size_t>(0.1 * fs);
    double refSum = 0.0;
    std::size_t refCount = 0;
    for (std::size_t i = skip; i < sine.size(); ++i)
    {
        const float y = kw.process(sine[i]);
        refSum += static_cast<double>(y) * static_cast<double>(y);
        ++refCount;
    }
    const double refMean = refSum / static_cast<double>(refCount);
    const double refLufs = -0.691 + 10.0 * std::log10(refMean < 1e-12 ? 1e-12 : refMean);

    // 全链:音频 → FeatureExtractor(§1)→ Loudness(§4)。
    scvb::analysis::FeatureExtractor ex;
    ex.prepare(fs, 1);
    const float* ch[1] = {sine.data()};
    const auto frames = runExtractor(ex, ch, n, {n});

    std::vector<float> kwMs;
    std::vector<float> peak;
    splitFrames(frames, kwMs, peak);

    const auto seg = scvb::analysis::measureSegment(kwMs.data(), peak.data(), static_cast<int>(kwMs.size()),
                                                    scvb::analysis::LoudnessMode::kIntegrated);

    REQUIRE(seg.main.level == Approx(refLufs).margin(0.05));
    REQUIRE(seg.main.level == Approx(-3.01).margin(0.05));

    // 确定性:跨块、跨 run 逐位一致(与块切分无关)。
    scvb::analysis::FeatureExtractor mixedEx;
    mixedEx.prepare(fs, 1);
    const auto mixedFrames = runExtractor(mixedEx, ch, n, {61, 127, 480, 513});
    std::vector<float> kwMsMixed;
    std::vector<float> peakMixed;
    splitFrames(mixedFrames, kwMsMixed, peakMixed);
    const auto segMixed =
        scvb::analysis::measureSegment(kwMsMixed.data(), peakMixed.data(), static_cast<int>(kwMsMixed.size()),
                                       scvb::analysis::LoudnessMode::kIntegrated);

    REQUIRE(segMixed.main.z == seg.main.z);
    REQUIRE(segMixed.main.level == seg.main.level);
    REQUIRE(segMixed.peakMax == seg.peakMax);
    REQUIRE(segMixed.momentary.p95 == seg.momentary.p95);
    REQUIRE(segMixed.momentary.max == seg.momentary.max);
    REQUIRE(segMixed.momentary.min == seg.momentary.min);

    // 跨 run:全新 extractor 复跑,同一素材 → 逐位一致。
    scvb::analysis::FeatureExtractor rerunEx;
    rerunEx.prepare(fs, 1);
    const auto rerunFrames = runExtractor(rerunEx, ch, n, {n});
    std::vector<float> kwMsRerun;
    std::vector<float> peakRerun;
    splitFrames(rerunFrames, kwMsRerun, peakRerun);
    const auto segRerun =
        scvb::analysis::measureSegment(kwMsRerun.data(), peakRerun.data(), static_cast<int>(kwMsRerun.size()),
                                       scvb::analysis::LoudnessMode::kIntegrated);

    REQUIRE(segRerun.main.z == seg.main.z);
    REQUIRE(segRerun.main.level == seg.main.level);
    REQUIRE(segRerun.peakMax == seg.peakMax);
    REQUIRE(segRerun.momentary.p95 == seg.momentary.p95);
    REQUIRE(segRerun.momentary.max == seg.momentary.max);
    REQUIRE(segRerun.momentary.min == seg.momentary.min);
}

TEST_CASE("LOUD-5: 三档口径解析向量(J69)", "[loudness]")
{
    const float kwMs[] = {0.01f, 0.09f};
    const float peak[] = {0.2f, 0.5f};

    const auto integrated = scvb::analysis::segmentLoudness(kwMs, peak, 2, scvb::analysis::LoudnessMode::kIntegrated);
    const auto rms = scvb::analysis::segmentLoudness(kwMs, peak, 2, scvb::analysis::LoudnessMode::kRms);
    const auto peakDb = scvb::analysis::segmentLoudness(kwMs, peak, 2, scvb::analysis::LoudnessMode::kPeakDbfs);

    REQUIRE(integrated.z == Approx(0.05).margin(1e-12));
    REQUIRE(integrated.level == Approx(-13.7013).margin(1e-4));
    REQUIRE(rms.level == Approx(-13.9794).margin(1e-4));
    REQUIRE(peakDb.level == Approx(-6.0206).margin(1e-4));

    // 三档 z 恒 >0、无 NaN。
    REQUIRE(integrated.z > 0.0);
    REQUIRE(rms.z > 0.0);
    REQUIRE(peakDb.z > 0.0);
    REQUIRE(std::isfinite(rms.level));
    REQUIRE(std::isfinite(peakDb.level));

    // 同 mode 重跑 100 次逐位一致(纯函数确定性)。
    for (int i = 0; i < 100; ++i)
    {
        const auto a = scvb::analysis::segmentLoudness(kwMs, peak, 2, scvb::analysis::LoudnessMode::kIntegrated);
        const auto b = scvb::analysis::segmentLoudness(kwMs, peak, 2, scvb::analysis::LoudnessMode::kRms);
        const auto c = scvb::analysis::segmentLoudness(kwMs, peak, 2, scvb::analysis::LoudnessMode::kPeakDbfs);
        REQUIRE(a.z == integrated.z);
        REQUIRE(a.level == integrated.level);
        REQUIRE(b.level == rms.level);
        REQUIRE(c.level == peakDb.level);
    }

    // 有动态 ⇒ rms − k_integrated = −0.278 dB < +0.691(Jensen 方向判别格)。
    REQUIRE(rms.level - integrated.level == Approx(-0.278).margin(1e-3));

    // 无动态(全 hop 等值)⇒ rms 恰比 k_integrated 高 0.691 dB(仅差 BS.1770 偏移)。
    const float flat[] = {0.04f, 0.04f, 0.04f};
    const float flatPeak[] = {0.5f, 0.5f, 0.5f};
    const auto flatInt = scvb::analysis::segmentLoudness(flat, flatPeak, 3, scvb::analysis::LoudnessMode::kIntegrated);
    const auto flatRms = scvb::analysis::segmentLoudness(flat, flatPeak, 3, scvb::analysis::LoudnessMode::kRms);
    REQUIRE(flatRms.level - flatInt.level == Approx(0.691).margin(1e-3));
}

TEST_CASE("LOUD-attached: 附带指标 peakMax 与 400ms 滑窗 p95/max/min", "[loudness]")
{
    constexpr int n = 100;
    std::vector<float> kwMs(static_cast<std::size_t>(n));
    std::vector<float> peak(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k)
    {
        // 确定性非平凡序列:逐 hop 能量单调 + 抖动,peak 单调增。
        kwMs[static_cast<std::size_t>(k)] = static_cast<float>(0.01 + 0.001 * k);
        peak[static_cast<std::size_t>(k)] = static_cast<float>(0.1 + 0.005 * k);
    }

    const auto seg =
        scvb::analysis::measureSegment(kwMs.data(), peak.data(), n, scvb::analysis::LoudnessMode::kIntegrated);

    REQUIRE(seg.peakMax == Approx(peak[static_cast<std::size_t>(n - 1)]).margin(1e-12));
    REQUIRE(seg.momentary.max >= seg.momentary.min);
    REQUIRE(seg.momentary.p95 <= seg.momentary.max);
    REQUIRE(seg.momentary.p95 >= seg.momentary.min);

    // 滑窗口径恒 K 加权:附带指标不随 mode 变化。
    const auto segRms = scvb::analysis::measureSegment(kwMs.data(), peak.data(), n, scvb::analysis::LoudnessMode::kRms);
    REQUIRE(segRms.momentary.p95 == seg.momentary.p95);
    REQUIRE(segRms.momentary.max == seg.momentary.max);
    REQUIRE(segRms.momentary.min == seg.momentary.min);
    REQUIRE(segRms.peakMax == seg.peakMax);
}
