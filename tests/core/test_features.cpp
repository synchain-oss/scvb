// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "analysis/FeatureExtractor.h"

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

} // namespace

TEST_CASE("FEAT-1: 正弦稳态特征(peak 与逐 hop 相等)", "[features]")
{
    constexpr double fs = 48000.0;
    const int h = 480;
    const auto sine = makeSine(fs, 1000.0, 0.5, 1);
    const int n = static_cast<int>(sine.size());

    scvb::analysis::FeatureExtractor ex;
    ex.prepare(fs, 1);
    const float* ch[1] = {sine.data()};
    const auto frames = runExtractor(ex, ch, n, {n});

    REQUIRE(frames.size() == static_cast<std::size_t>(n / h)); // 1s@48k = 100 hop

    for (const auto& f : frames)
        REQUIRE(f.peak == Approx(0.5f).margin(1e-6f));

    // 稳态后逐 hop 相等(每个 hop = 10 个整周期;跳过前 10 hop ≈ 100ms 的暖机尾巴,
    // 38Hz RLB 高通暂态收敛较慢,见 02-dsp-spec §1.4)。
    for (std::size_t k = 10; k + 1 < frames.size(); ++k)
    {
        INFO("hop " << k);
        REQUIRE(frames[k].kw_ms == Approx(frames[k + 1].kw_ms).margin(1e-6f));
    }
}

TEST_CASE("FEAT-2: 累加器跨块逐位一致", "[features]")
{
    constexpr double fs = 48000.0;
    const auto sig = makeMix(fs, 1, 997.0, 3171.0, 777.0);
    const int n = static_cast<int>(sig.size());
    const float* ch[1] = {sig.data()};

    scvb::analysis::FeatureExtractor oneShotEx;
    oneShotEx.prepare(fs, 1);
    const auto oneShot = runExtractor(oneShotEx, ch, n, {n});

    scvb::analysis::FeatureExtractor mixedEx;
    mixedEx.prepare(fs, 1);
    const auto mixed = runExtractor(mixedEx, ch, n, {61, 127, 480, 513});

    REQUIRE(oneShot.size() == mixed.size());
    for (std::size_t k = 0; k < oneShot.size(); ++k)
    {
        INFO("hop " << k);
        REQUIRE(oneShot[k].kw_ms == mixed[k].kw_ms); // 逐位
        REQUIRE(oneShot[k].peak == mixed[k].peak);
    }
}

TEST_CASE("FEAT-3: 跨 run 边界可复现", "[features]")
{
    constexpr double fs = 48000.0;
    const int h = 480;
    const auto sig = makeMix(fs, 5, 997.0, 3171.0, 777.0);
    const int n = static_cast<int>(sig.size());
    const int k1 = 100; // run A 采 hop 0..99,run B 从 hop 100 起
    const int k1Sample = k1 * h;

    // 参考:仅从 k1 单独推流(全新 extractor,预热 k1)。
    scvb::analysis::FeatureExtractor ref;
    ref.prepare(fs, 1);
    const float* chB[1] = {sig.data() + k1Sample};
    const auto refFrames = runExtractor(ref, chB, n - k1Sample, {n - k1Sample});

    // run B 口径 A:先跑 run A 再 startRun,块长 [480]。
    scvb::analysis::FeatureExtractor exA;
    exA.prepare(fs, 1);
    const float* chA[1] = {sig.data()};
    (void)runExtractor(exA, chA, k1Sample, {k1Sample}); // run A:hops 0..99,丢弃
    exA.startRun();
    const auto runB_A = runExtractor(exA, chB, n - k1Sample, {480});

    // run B 口径 B:块长 [61,127,513]。
    scvb::analysis::FeatureExtractor exB;
    exB.prepare(fs, 1);
    (void)runExtractor(exB, chA, k1Sample, {k1Sample});
    exB.startRun();
    const auto runB_B = runExtractor(exB, chB, n - k1Sample, {61, 127, 513});

    REQUIRE(refFrames.size() == runB_A.size());
    REQUIRE(refFrames.size() == runB_B.size());
    for (std::size_t k = 0; k < refFrames.size(); ++k)
    {
        INFO("hop " << k);
        REQUIRE(runB_A[k].kw_ms == refFrames[k].kw_ms); // run B 首 hop 无 run A 残留
        REQUIRE(runB_A[k].peak == refFrames[k].peak);
        REQUIRE(runB_B[k].kw_ms == refFrames[k].kw_ms);
        REQUIRE(runB_B[k].peak == refFrames[k].peak);
    }
}

TEST_CASE("FEAT-4: stereo 多通道能量求和(J57)", "[features]")
{
    constexpr double fs = 48000.0;

    // (a) L=R=幅度 0.5 的 1kHz 正弦 → kw_ms 为单通道 2 倍(+3.01dB,能量域求和),peak 取最大。
    SECTION("L==R 时能量域 2 倍")
    {
        const auto sine = makeSine(fs, 1000.0, 0.5, 1);
        const int n = static_cast<int>(sine.size());

        scvb::analysis::FeatureExtractor mono;
        mono.prepare(fs, 1);
        const float* mch[1] = {sine.data()};
        const auto monoFrames = runExtractor(mono, mch, n, {n});

        scvb::analysis::FeatureExtractor stereo;
        stereo.prepare(fs, 2);
        const float* sch[2] = {sine.data(), sine.data()};
        const auto stereoFrames = runExtractor(stereo, sch, n, {n});

        REQUIRE(monoFrames.size() == stereoFrames.size());
        for (std::size_t k = 0; k < monoFrames.size(); ++k)
        {
            INFO("hop " << k);
            REQUIRE(stereoFrames[k].kw_ms == Approx(2.0f * monoFrames[k].kw_ms).epsilon(1e-6f));
            REQUIRE(stereoFrames[k].peak == Approx(0.5f).margin(1e-6f));
        }
    }

    // (b) L/R 内容不同 → 与「两路分别算再相加」逐位一致。
    SECTION("L!=R 时与两路分别算再相加逐位一致")
    {
        const auto l = makeMix(fs, 1, 997.0, 3171.0, 777.0);
        const auto r = makeMix(fs, 1, 1201.0, 2887.0, 543.0);
        const int n = static_cast<int>(l.size());

        scvb::analysis::FeatureExtractor monoL;
        monoL.prepare(fs, 1);
        const float* lch[1] = {l.data()};
        const auto lf = runExtractor(monoL, lch, n, {n});

        scvb::analysis::FeatureExtractor monoR;
        monoR.prepare(fs, 1);
        const float* rch[1] = {r.data()};
        const auto rf = runExtractor(monoR, rch, n, {n});

        scvb::analysis::FeatureExtractor stereo;
        stereo.prepare(fs, 2);
        const float* sch[2] = {l.data(), r.data()};
        const auto sf = runExtractor(stereo, sch, n, {n});

        REQUIRE(lf.size() == sf.size());
        for (std::size_t k = 0; k < lf.size(); ++k)
        {
            INFO("hop " << k);
            REQUIRE(sf[k].kw_ms == lf[k].kw_ms + rf[k].kw_ms); // 逐位
            REQUIRE(sf[k].peak == std::max(lf[k].peak, rf[k].peak));
        }
    }
}
