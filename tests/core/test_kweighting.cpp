// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

#include "dsp/KWeighting.h"

#ifdef SCVB_TESTS_WITH_EBUR128
#include <ebur128.h>
#endif

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

std::vector<float> makeDc(double fs, double amp, int seconds)
{
    return std::vector<float>(static_cast<std::size_t>(fs * seconds), static_cast<float>(amp));
}

int hopSize(double fs)
{
    return static_cast<int>(std::llround(fs / 100.0));
}

// 样本过 KWeighting,跳过前 skipSamples 后积分,返回 L = -0.691 + 10log10(mean(y^2))。
double measureLufs(scvb::dsp::KWeighting& kw, const std::vector<float>& samples, std::size_t skipSamples)
{
    double sumSq = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const float y = kw.process(samples[i]);
        if (i >= skipSamples)
        {
            sumSq += static_cast<double>(y) * static_cast<double>(y);
            ++count;
        }
    }
    const double mean = count > 0 ? sumSq / static_cast<double>(count) : 0.0;
    const double clamped = mean < 1e-12 ? 1e-12 : mean;
    return -0.691 + 10.0 * std::log10(clamped);
}

// 确定性粉噪(白噪 → Paul Kellet refined pink filter)。
std::vector<float> makePinkNoise(double fs, int seconds)
{
    const std::size_t n = static_cast<std::size_t>(fs * seconds);
    std::vector<float> out(n);
    std::mt19937 gen(0x5C4B7A9Eu);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    float b0 = 0.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float b3 = 0.0f;
    float b4 = 0.0f;
    float b5 = 0.0f;
    float b6 = 0.0f;
    for (std::size_t i = 0; i < n; ++i)
    {
        const float white = dist(gen);
        b0 = 0.99886f * b0 + white * 0.0555179f;
        b1 = 0.99332f * b1 + white * 0.0750759f;
        b2 = 0.96900f * b2 + white * 0.1538520f;
        b3 = 0.86650f * b3 + white * 0.3104856f;
        b4 = 0.55000f * b4 + white * 0.5329522f;
        b5 = -0.7616f * b5 - white * 0.0168980f;
        out[i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f;
        b6 = white * 0.115926f;
    }
    return out;
}

} // namespace

TEST_CASE("KW-1: 1kHz 0dBFS 校准点(997Hz 正弦)", "[kweighting]")
{
    constexpr double fs = 48000.0;
    scvb::dsp::KWeighting kw;
    kw.prepare(fs);

    const auto sine = makeSine(fs, 997.0, 1.0, 5);
    const std::size_t skip = static_cast<std::size_t>(0.1 * fs); // 跳过前 100ms
    const double lufs = measureLufs(kw, sine, skip);

    REQUIRE(lufs == Approx(-3.01).margin(0.05));
}

TEST_CASE("KW-2: makeStage1/2(48000) 匹配 BS.1770 表值", "[kweighting]")
{
    const auto s1 = scvb::dsp::makeStage1(48000.0);
    const auto s2 = scvb::dsp::makeStage2(48000.0);

    REQUIRE(s1.b0 == Approx(1.53512485958697).margin(1e-9));
    REQUIRE(s1.b1 == Approx(-2.69169618940638).margin(1e-9));
    REQUIRE(s1.b2 == Approx(1.19839281085285).margin(1e-9));
    REQUIRE(s1.a1 == Approx(-1.69065929318241).margin(1e-9));
    REQUIRE(s1.a2 == Approx(0.73248077421585).margin(1e-9));

    REQUIRE(s2.b0 == Approx(1.0).margin(1e-9));
    REQUIRE(s2.b1 == Approx(-2.0).margin(1e-9));
    REQUIRE(s2.b2 == Approx(1.0).margin(1e-9));
    REQUIRE(s2.a1 == Approx(-1.99004745483398).margin(1e-9));
    REQUIRE(s2.a2 == Approx(0.99007225036621).margin(1e-9));
}

TEST_CASE("KW-4: RLB 高通在稳态下灭直流", "[kweighting]")
{
    constexpr double fs = 48000.0;
    scvb::dsp::KWeighting kw;
    kw.prepare(fs);

    const auto dc = makeDc(fs, 0.5, 5);
    const std::size_t skip = static_cast<std::size_t>(0.5 * fs); // 跳过前 500ms
    const double lufs = measureLufs(kw, dc, skip);

    REQUIRE(lufs < -100.0);
}

#ifdef SCVB_TESTS_WITH_EBUR128
TEST_CASE("KW-3: 粉噪 30s 与 libebur128 M(400ms) 对拍", "[kweighting][reference]")
{
    const double rates[] = {44100.0, 48000.0, 88200.0, 96000.0};

    for (const double fs : rates)
    {
        INFO("fs = " << fs);
        const int h = hopSize(fs);
        const auto noise = makePinkNoise(fs, 30);
        const int numHops = static_cast<int>(noise.size()) / h;

        // 自研:每 hop 平方和(double),再按 40 hop(400ms)滑动窗口取响度。
        scvb::dsp::KWeighting kw;
        kw.prepare(fs);
        std::vector<double> hopEnergy(static_cast<std::size_t>(numHops));
        for (int k = 0; k < numHops; ++k)
        {
            double sum = 0.0;
            for (int i = 0; i < h; ++i)
            {
                const float y = kw.process(
                    noise[static_cast<std::size_t>(k) * static_cast<std::size_t>(h) + static_cast<std::size_t>(i)]);
                sum += static_cast<double>(y) * static_cast<double>(y);
            }
            hopEnergy[static_cast<std::size_t>(k)] = sum;
        }

        // libebur128:mono、M(400ms),逐 10ms 喂入并取 momentary。
        ebur128_state* st = ebur128_init(1u, static_cast<unsigned long>(fs), EBUR128_MODE_M);
        REQUIRE(st != nullptr);

        for (int k = 0; k < numHops; ++k)
        {
            const float* src = noise.data() + static_cast<std::size_t>(k) * static_cast<std::size_t>(h);
            REQUIRE(ebur128_add_frames_float(st, src, static_cast<std::size_t>(h)) == EBUR128_SUCCESS);

            double ref = 0.0;
            REQUIRE(ebur128_loudness_momentary(st, &ref) == EBUR128_SUCCESS);

            if (k >= 39) // 前 400ms 窗口未满,跳过
            {
                double total = 0.0;
                for (int j = k - 39; j <= k; ++j)
                    total += hopEnergy[static_cast<std::size_t>(j)];

                const double mean = total / static_cast<double>(40 * h);
                const double mine = -0.691 + 10.0 * std::log10(mean < 1e-12 ? 1e-12 : mean);

                REQUIRE(mine == Approx(ref).margin(0.05));
            }
        }

        ebur128_destroy(&st);
    }
}
#else
TEST_CASE("KW-3: libebur128 对拍(参考,需 SCVB_TESTS_WITH_EBUR128)", "[kweighting][reference]")
{
    SUCCEED("libebur128 reference test disabled (SCVB_TESTS_WITH_EBUR128=OFF)");
}
#endif
