// SPDX-License-Identifier: GPL-3.0-or-later
// test_panlaw —— 10 §4.4.1 PANLAW-2..5 的渲染层对拍(发版验证清单 D3,SL-575)。
//
// PANLAW-1 是增益公式本身,由 test_transition.cpp 的 PAN-1 钉着;这里补的是「把声音真的混出来,
// 在输出上量」的那几格。渲染用的是 Output 实时混音循环逐样本调用的同一对原语
// (output/MixMath.h 的 mixMonoSample / mixStereoSample,调用点在 src/output/OutputProcessor.cpp
// 的逐样本混音那段):固定种子粉噪逐样本混成 stereo,再在输出上量 RMS / 能量 / 相关。
// 不含读环、参数平滑、busXfade 与宿主 —— 即 10 §4.5 说的「数学层面的端到端」,不替代
// DAW 上的 null test(清单 D1 / D2)。
//
// 每一格对应的反向注入(改 MixMath.h 一处、重编、只跑本文件)记在 SL-575 的 PR 描述里。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "analysis/PanCurve.h"
#include "output/MixMath.h"

namespace
{

constexpr int kFs = 48000;
constexpr double kPi = 3.14159265358979323846;

// 没画过 pan 曲线(G ≡ 0),与 test_mix_source.cpp 的 kNoCurve 同形。
const scvb::PanCurveXfade kNoCurve{};

// 确定性粉噪(白噪 → Paul Kellet refined pink filter;与 test_kweighting.cpp 同一滤波器,种子可选)。
std::vector<float> pinkNoise(std::uint32_t seed, int seconds)
{
    const std::size_t n = static_cast<std::size_t>(kFs) * static_cast<std::size_t>(seconds);
    std::vector<float> out(n);
    std::mt19937 gen(seed);
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
        out[i] = 0.1f * (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f);
        b6 = white * 0.115926f;
    }
    return out;
}

double dot(const std::vector<float>& a, const std::vector<float>& b)
{
    double s = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        s += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    return s;
}

// 从 b 里减掉它在 a 上的投影、再缩放到与 a 等能量:得到与 a **样本相关为 0** 的一路。
// 独立生成的两路粉噪在 10 秒里仍有百分之几的样本相关(能量集中在低频),而 PANLAW-5 的
// 「总能量不随 width 变」只对不相关的两路成立(相关的两路合到一起会相干叠加,见 5c 的注)。
std::vector<float> orthogonalTo(const std::vector<float>& a, std::vector<float> b)
{
    const double k = dot(b, a) / dot(a, a);
    for (std::size_t i = 0; i < b.size(); ++i)
        b[i] = static_cast<float>(static_cast<double>(b[i]) - k * static_cast<double>(a[i]));
    const double g = std::sqrt(dot(a, a) / dot(b, b));
    for (auto& v : b)
        v = static_cast<float>(static_cast<double>(v) * g);
    return b;
}

struct Stereo
{
    std::vector<float> l;
    std::vector<float> r;
};

// 单轨 mono 源 → Output 混音原语 → stereo(vol 0 dB、fade 1、无 pan 曲线)。
Stereo renderMono(const std::vector<float>& src, float pan, float globalWidth)
{
    Stereo out{std::vector<float>(src.size()), std::vector<float>(src.size())};
    for (std::size_t i = 0; i < src.size(); ++i)
    {
        float l = 0.0f;
        float r = 0.0f;
        scvb::output::mixMonoSample(src[i], pan, 0.0f, globalWidth, 1.0f, kNoCurve, l, r);
        out.l[i] = l;
        out.r[i] = r;
    }
    return out;
}

// 单轨 stereo 源 → Output 混音原语 → stereo(pan、每轨 width 可设;全局 width 100、vol 0 dB)。
Stereo renderStereo(const Stereo& src, float pan, float trkWidth)
{
    Stereo out{std::vector<float>(src.l.size()), std::vector<float>(src.l.size())};
    for (std::size_t i = 0; i < src.l.size(); ++i)
    {
        float l = 0.0f;
        float r = 0.0f;
        scvb::output::mixStereoSample(src.l[i], src.r[i], pan, 0.0f, trkWidth, 100.0f, 1.0f, kNoCurve, l, r);
        out.l[i] = l;
        out.r[i] = r;
    }
    return out;
}

double energy(const std::vector<float>& x)
{
    return dot(x, x);
}

double dB(double ratio)
{
    return 10.0 * std::log10(ratio);
}

// 由两声道的能量比反解有效 pan:equal-power 下 E_R / E_L = tan²θ,θ = (P+100)·π/400。
double panFromEnergies(double eL, double eR)
{
    const double theta = std::atan2(std::sqrt(eR), std::sqrt(eL));
    return theta * 400.0 / kPi - 100.0;
}

// 两路的样本相关系数。
double correlation(const std::vector<float>& a, const std::vector<float>& b)
{
    return dot(a, b) / std::sqrt(energy(a) * energy(b));
}

std::vector<float> sumOf(const Stereo& s, float sign)
{
    std::vector<float> m(s.l.size());
    for (std::size_t i = 0; i < m.size(); ++i)
        m[i] = s.l[i] + sign * s.r[i];
    return m;
}

} // namespace

TEST_CASE("PANLAW-2: mono pink noise, 9 pan values, L/R level ratio = 20*log10(cot(theta)) within 0.05 dB", "[panlaw]")
{
    const auto src = pinkNoise(0x50A1u, 10);
    for (const float p : {-75.0f, -50.0f, -25.0f, 0.0f, 25.0f, 50.0f, 75.0f})
    {
        INFO("pan = " << p);
        const Stereo out = renderMono(src, p, 100.0f);
        const double measured = dB(energy(out.l) / energy(out.r)); // = 20*log10(RMS_L / RMS_R)
        const double theta = (static_cast<double>(p) + 100.0) * kPi / 400.0;
        const double expected = 20.0 * std::log10(std::cos(theta) / std::sin(theta));
        CHECK(std::abs(measured - expected) <= 0.05);
    }
    // 两个端点 cot θ = ∞ / 0,比值没有有限的理论值:改判「另一侧比这一侧低 120 dB 以上」
    // (硬左 / 硬右:声音只在一侧)。
    for (const float p : {-100.0f, 100.0f})
    {
        INFO("pan = " << p);
        const Stereo out = renderMono(src, p, 100.0f);
        const double eNear = p < 0.0f ? energy(out.l) : energy(out.r);
        const double eFar = p < 0.0f ? energy(out.r) : energy(out.l);
        CHECK(eNear > 0.0);
        CHECK(eFar <= eNear * 1e-12);
    }
}

TEST_CASE("PANLAW-3: global width {0,50,100,150} scales P_curve=60 to P_eff 0/30/60/90 (measured)", "[panlaw]")
{
    const auto src = pinkNoise(0x50A3u, 10);
    const struct
    {
        float width;
        double pEff;
    } cases[] = {{0.0f, 0.0}, {50.0f, 30.0}, {100.0f, 60.0}, {150.0f, 90.0}};
    for (const auto& c : cases)
    {
        INFO("global width = " << c.width);
        const Stereo out = renderMono(src, 60.0f, c.width);
        CHECK(std::abs(panFromEnergies(energy(out.l), energy(out.r)) - c.pEff) <= 0.01);
    }
}

TEST_CASE("PANLAW-4: sweeping one track from -100 to +100 keeps 10*log10(zL+zR) within 0.05 dB", "[panlaw]")
{
    const auto src = pinkNoise(0x50A4u, 1);
    const double eIn = energy(src);
    double lo = 1e300;
    double hi = -1e300;
    for (int p = -100; p <= 100; p += 5)
    {
        const Stereo out = renderMono(src, static_cast<float>(p), 100.0f);
        const double level = dB((energy(out.l) + energy(out.r)) / eIn);
        lo = std::min(lo, level);
        hi = std::max(hi, level);
    }
    INFO("level range [" << lo << ", " << hi << "] dB re input");
    CHECK(hi - lo <= 0.05);
    CHECK(std::abs(hi) <= 0.05); // equal-power:总能量等于输入能量(0 dB),不是只「平」
}

TEST_CASE("PANLAW-5a: per-track width=100 at pan 0 reproduces the source L/R (no swap, no blend)", "[panlaw]")
{
    Stereo src{pinkNoise(0x50B1u, 10), {}};
    src.r = orthogonalTo(src.l, pinkNoise(0x50B2u, 10));
    const Stereo out = renderStereo(src, 0.0f, 100.0f);
    float maxErr = 0.0f;
    for (std::size_t i = 0; i < src.l.size(); ++i)
    {
        maxErr = std::max(maxErr, std::abs(out.l[i] - src.l[i]));
        maxErr = std::max(maxErr, std::abs(out.r[i] - src.r[i]));
    }
    INFO("max |out - src| = " << maxErr);
    CHECK(maxErr <= 1e-6f);
    CHECK(std::abs(correlation(out.l, out.r) - correlation(src.l, src.r)) <= 1e-4);
}

TEST_CASE("PANLAW-5b: per-track width=0 collapses to mono (L == R sample for sample)", "[panlaw]")
{
    Stereo src{pinkNoise(0x50B1u, 10), {}};
    src.r = orthogonalTo(src.l, pinkNoise(0x50B2u, 10));
    const Stereo out = renderStereo(src, 0.0f, 0.0f);
    std::size_t differ = 0;
    for (std::size_t i = 0; i < out.l.size(); ++i)
        differ += (out.l[i] != out.r[i]) ? 1u : 0u;
    CHECK(differ == 0u);
}

TEST_CASE("PANLAW-5c: per-track width=50 sits between 0 and 100 and keeps total energy within 0.05 dB of width=100",
          "[panlaw]")
{
    // 两路源样本相关为 0(orthogonalTo)。dual-pan 的两个子声像各自等功率,所以不相关的两路合进来
    // 总能量与 width 无关;相关的两路则会相干叠加 —— 例如 L == R 的源在 width=0 时比 width=100
    // 响 3 dB,那是物理,不是缺陷,所以这一格只对不相关的源成立。
    Stereo src{pinkNoise(0x50B1u, 10), {}};
    src.r = orthogonalTo(src.l, pinkNoise(0x50B2u, 10));
    const Stereo w0 = renderStereo(src, 0.0f, 0.0f);
    const Stereo w50 = renderStereo(src, 0.0f, 50.0f);
    const Stereo w100 = renderStereo(src, 0.0f, 100.0f);

    const double e50 = energy(w50.l) + energy(w50.r);
    const double e100 = energy(w100.l) + energy(w100.r);
    INFO("E(50) / E(100) = " << dB(e50 / e100) << " dB");
    CHECK(std::abs(dB(e50 / e100)) <= 0.05);

    // 「介于两者」:输出两声道的相关,width=0 为 1(塌成 mono),width=100 为源的 0,
    // width=50 应落在两者之间(理论值 sin(pi/4) ≈ 0.707),而不是贴在任一端。
    const double c0 = correlation(w0.l, w0.r);
    const double c50 = correlation(w50.l, w50.r);
    const double c100 = correlation(w100.l, w100.r);
    INFO("corr(L,R): width 0 = " << c0 << ", 50 = " << c50 << ", 100 = " << c100);
    CHECK(c50 > c100 + 0.1);
    CHECK(c50 < c0 - 0.1);
}

TEST_CASE("PANLAW-5d: no polarity inversion at any per-track width (E(L-R) <= E(L+R))", "[panlaw]")
{
    // 源取真实立体声那种「有公共中心成分」的一对:R = 0.6·L + 0.8·N(N 与 L 不相关、等能量),
    // 源相关 0.6。M/S 拉宽(ADR-010 明令避免的做法)在这种素材上会让 S 超过 M;dual-pan 不会。
    // 不用 5a–5c 那对不相关的源:它在 width=100 时 E(L-R) 与 E(L+R) 理论上**恰好相等**,
    // 浮点舍入往哪边偏都行,判据在那一点上分不出对错。
    const auto l = pinkNoise(0x50C1u, 10);
    const auto n = orthogonalTo(l, pinkNoise(0x50C2u, 10));
    Stereo src{l, std::vector<float>(l.size())};
    for (std::size_t i = 0; i < l.size(); ++i)
        src.r[i] = 0.6f * l[i] + 0.8f * n[i];

    for (const float w : {0.0f, 50.0f, 100.0f})
    {
        INFO("per-track width = " << w);
        const Stereo out = renderStereo(src, 0.0f, w);
        const double eMid = energy(sumOf(out, 1.0f));
        const double eSide = energy(sumOf(out, -1.0f));
        INFO("E(L+R) = " << eMid << ", E(L-R) = " << eSide);
        CHECK(eSide <= eMid);
    }
}
