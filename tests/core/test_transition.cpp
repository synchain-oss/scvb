// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "dsp/PanMath.h"
#include "engine/CurveEvaluator.h"

namespace
{

constexpr double kFs = 48000.0;

// 恢复 equal-power 角度 θ = atan2(gR, gL)(θ ∈ [0, π/2])。
double angleOf(float gL, float gR)
{
    return std::atan2(static_cast<double>(gR), static_cast<double>(gL));
}

} // namespace

TEST_CASE("PAN-1 equal-power pan gains", "[transition]")
{
    float gL = 0.0f, gR = 0.0f;
    scvb::panGains(0.0f, gL, gR);
    REQUIRE(gL == Catch::Approx(0.7071).margin(1e-4));
    REQUIRE(gR == Catch::Approx(0.7071).margin(1e-4));

    scvb::panGains(-100.0f, gL, gR);
    REQUIRE(gL == Catch::Approx(1.0).margin(1e-4));
    REQUIRE(gR == Catch::Approx(0.0).margin(1e-4));

    scvb::panGains(50.0f, gL, gR);
    REQUIRE(gL == Catch::Approx(0.3827).margin(1e-4));
    REQUIRE(gR == Catch::Approx(0.9239).margin(1e-4));
}

TEST_CASE("DUALPAN-1 width=0 degenerates to single-point equal-power", "[transition]")
{
    // mono/stereo 两条路径的连续性守卫:width=0 → 双子声像重合于 panCenter。
    for (float pan = -100.0f; pan <= 100.0f; pan += 1.0f)
    {
        const scvb::DualPanGains d = scvb::dualPanGains(pan, 0.0f);
        float gL = 0.0f, gR = 0.0f;
        scvb::panGains(pan, gL, gR);
        REQUIRE(d.gL_L == gL);
        REQUIRE(d.gR_L == gR);
        REQUIRE(d.gL_R == gL);
        REQUIRE(d.gR_R == gR);
    }
}

TEST_CASE("DUALPAN-2 each sub-pan is energy-conserving", "[transition]")
{
    for (float pan = -100.0f; pan <= 100.0f; pan += 1.0f)
    {
        for (float width = 0.0f; width <= 100.0f; width += 1.0f)
        {
            const scvb::DualPanGains d = scvb::dualPanGains(pan, width);
            const double eL = static_cast<double>(d.gL_L) * d.gL_L + static_cast<double>(d.gR_L) * d.gR_L;
            const double eR = static_cast<double>(d.gL_R) * d.gL_R + static_cast<double>(d.gR_R) * d.gR_R;
            REQUIRE(std::fabs(10.0 * std::log10(eL)) <= 0.05);
            REQUIRE(std::fabs(10.0 * std::log10(eR)) <= 0.05);
        }
    }
}

TEST_CASE("DUALPAN-3 arc center and width are decoupled", "[transition]")
{
    constexpr double kPiOver400 = 3.14159265358979323846 / 400.0;

    // ① 固定 width 扫 panCenter:两侧角度差恒定(2·width·π/400)。
    const float width = 30.0f;
    for (float pan = -40.0f; pan <= 40.0f; pan += 1.0f)
    {
        const scvb::DualPanGains d = scvb::dualPanGains(pan, width);
        const double diff = angleOf(d.gL_R, d.gR_R) - angleOf(d.gL_L, d.gR_L);
        REQUIRE(diff == Catch::Approx(2.0 * static_cast<double>(width) * kPiOver400).margin(1e-4));
    }

    // ② 固定 panCenter 扫 width:弧中心不漂移(θ(panCenter) = (panCenter+100)·π/400)。
    for (float w = 0.0f; w <= 100.0f; w += 1.0f)
    {
        const scvb::DualPanGains d = scvb::dualPanGains(0.0f, w);
        const double center = (angleOf(d.gL_L, d.gR_L) + angleOf(d.gL_R, d.gR_R)) * 0.5;
        REQUIRE(center == Catch::Approx(100.0 * kPiOver400).margin(1e-4));
    }
}

TEST_CASE("WIDTH-1 global width scales the assignment angle", "[transition]")
{
    // mono 轨 P_curve=60,width ∈ {0,50,100,150} → P_eff = 0/30/60/90。
    REQUIRE(scvb::scaleByGlobalWidth(60.0f, 0.0f) == Catch::Approx(0.0f).margin(1e-6));
    REQUIRE(scvb::scaleByGlobalWidth(60.0f, 50.0f) == Catch::Approx(30.0f).margin(1e-6));
    REQUIRE(scvb::scaleByGlobalWidth(60.0f, 100.0f) == Catch::Approx(60.0f).margin(1e-6));
    REQUIRE(scvb::scaleByGlobalWidth(60.0f, 150.0f) == Catch::Approx(90.0f).margin(1e-6));
    // width=150 且 P_curve=70 → clamp 100。
    REQUIRE(scvb::scaleByGlobalWidth(70.0f, 150.0f) == Catch::Approx(100.0f).margin(1e-6));
}

TEST_CASE("TRANS-1 equal-power along the whole ramp", "[transition]")
{
    // P −100→+100,ramp=80ms。gap=200ms 使 T_eff=80ms(0.6·gap=120ms > 80ms)。
    scvb::CurveEvaluator ev;
    ev.build(
        {
            scvb::CurveSegment{0.0, 0.05, -100.0, 0.0},
            scvb::CurveSegment{0.25, 0.30, 100.0, 0.0},
        },
        scvb::TransitionConfig{});

    const double rampCenter = (0.05 + 0.25) * 0.5; // 0.15
    const double t0 = rampCenter - 0.04; // 0.11
    const double t1 = rampCenter + 0.04; // 0.19

    const int64_t n = static_cast<int64_t>((t1 - t0) * kFs);
    for (int64_t k = 0; k <= n; ++k)
    {
        const double t = t0 + static_cast<double>(k) / kFs;
        const double p = ev.panAt(t);
        float gL = 0.0f, gR = 0.0f;
        scvb::panGains(static_cast<float>(p), gL, gR);
        const double energy = static_cast<double>(gL) * gL + static_cast<double>(gR) * gR;
        REQUIRE(std::fabs(energy - 1.0) <= 1e-6);
    }

    // 中点 P=0。
    REQUIRE(ev.panAt(rampCenter) == Catch::Approx(0.0).margin(1e-6));
}

TEST_CASE("TRANS-2 ramp is smoothstep-shaped and monotonic", "[transition]")
{
    scvb::CurveEvaluator ev;
    ev.build(
        {
            scvb::CurveSegment{0.0, 0.05, -100.0, 0.0},
            scvb::CurveSegment{0.25, 0.30, 100.0, 0.0},
        },
        scvb::TransitionConfig{});

    const double rampCenter = 0.15;
    const double t0 = rampCenter - 0.04;
    const double t1 = rampCenter + 0.04;

    const int64_t n = static_cast<int64_t>((t1 - t0) * kFs);
    double prevW = -1.0;
    double prevP = ev.panAt(t0);
    for (int64_t k = 0; k <= n; ++k)
    {
        const double t = t0 + static_cast<double>(k) / kFs;
        const double p = ev.panAt(t);
        const double w = (p + 100.0) / 200.0; // P=−100 → w=0,P=+100 → w=1
        if (k > 0)
            REQUIRE(w >= prevW); // 单调不减

        // 端点一阶差分 ≤ 1e−3(smoothstep 端导数为 0):查 ramp 头/尾各 3 个样本。
        if (k >= 1)
        {
            const double step = std::fabs(w - (prevP + 100.0) / 200.0);
            if (k <= 3 || k >= n - 2)
                REQUIRE(step <= 1e-3);
        }

        prevW = w;
        prevP = p;
    }
}

TEST_CASE("TRANS-3 gap=0 speed-limited T_eff and peak slope", "[transition]")
{
    // ΔP=60(两侧段足够长,0.9·len 夹限不触发)。
    scvb::CurveEvaluator ev;
    ev.build(
        {
            scvb::CurveSegment{0.0, 10.0, -30.0, 0.0},
            scvb::CurveSegment{10.0, 20.0, 30.0, 0.0},
        },
        scvb::TransitionConfig{});
    REQUIRE(ev.boundaries().size() == 1u);
    REQUIRE(ev.boundaries()[0].tEffSec == Catch::Approx(6.0).margin(1e-9));
    REQUIRE(ev.warningCount() == 0);

    // 逐样本差分峰值 ≤ 15 P/s × (1+1e−3)。
    const double t0 = 10.0 - 3.0; // 6s ramp 中点两侧各 3s
    const double t1 = 10.0 + 3.0;
    const int64_t n = static_cast<int64_t>((t1 - t0) * kFs);
    double maxDiff = 0.0;
    double prev = ev.panAt(t0);
    for (int64_t k = 1; k <= n; ++k)
    {
        const double t = t0 + static_cast<double>(k) / kFs;
        const double cur = ev.panAt(t);
        const double diff = std::fabs(cur - prev) * kFs; // P/s
        if (diff > maxDiff)
            maxDiff = diff;
        prev = cur;
    }
    REQUIRE(maxDiff <= 15.0 * (1.0 + 1e-3));
}

TEST_CASE("TRANS-3b gap=0 ΔP=120 hits 6s cap and warns", "[transition]")
{
    scvb::CurveEvaluator ev;
    ev.build(
        {
            scvb::CurveSegment{0.0, 10.0, -60.0, 0.0},
            scvb::CurveSegment{10.0, 20.0, 60.0, 0.0},
        },
        scvb::TransitionConfig{});
    REQUIRE(ev.boundaries()[0].tEffSec == Catch::Approx(6.0).margin(1e-9));
    REQUIRE(ev.boundaries()[0].warnLimit);
    REQUIRE(ev.warningCount() >= 1);
}

TEST_CASE("TRANS-4 gap=50ms clamps T_eff into the gap", "[transition]")
{
    scvb::CurveEvaluator ev;
    ev.build(
        {
            scvb::CurveSegment{0.0, 0.10, -50.0, 0.0},
            scvb::CurveSegment{0.15, 0.25, 50.0, 0.0},
        },
        scvb::TransitionConfig{});
    // T_eff = clamp(min(80, 0.6·50=30), 20, 80) = 30ms。
    REQUIRE(ev.boundaries()[0].tEffSec == Catch::Approx(0.030).margin(1e-9));

    // ramp 完全在空隙 [0.10, 0.15] 内。
    const auto& b = ev.boundaries()[0];
    REQUIRE(b.centerSec - b.tEffSec * 0.5 >= 0.10 - 1e-12);
    REQUIRE(b.centerSec + b.tEffSec * 0.5 <= 0.15 + 1e-12);
}

TEST_CASE("TRANS-5 chunked evaluation is bit-identical to whole", "[transition]")
{
    scvb::CurveEvaluator ev;
    ev.build(
        {
            scvb::CurveSegment{0.0, 0.5, -60.0, -2.0},
            scvb::CurveSegment{0.6, 1.1, 40.0, 1.5},
            scvb::CurveSegment{1.2, 1.8, 0.0, -0.5},
        },
        scvb::TransitionConfig{});

    const int64_t total = static_cast<int64_t>(1.8 * kFs);
    std::vector<double> panWhole(static_cast<size_t>(total));
    std::vector<double> volWhole(static_cast<size_t>(total));
    for (int64_t k = 0; k < total; ++k)
    {
        const double t = static_cast<double>(k) / kFs;
        panWhole[static_cast<size_t>(k)] = ev.panAt(t);
        volWhole[static_cast<size_t>(k)] = ev.volAt(t);
    }

    // 块长 [128, 480, 77, 513, 61, 127] 混合推流。
    const int64_t blockSizes[] = {128, 480, 77, 513, 61, 127};
    std::vector<double> panChunked(static_cast<size_t>(total));
    std::vector<double> volChunked(static_cast<size_t>(total));
    int64_t k = 0;
    size_t bi = 0;
    while (k < total)
    {
        const int64_t block = blockSizes[bi % 6];
        for (int64_t j = 0; j < block && k < total; ++j, ++k)
        {
            const double t = static_cast<double>(k) / kFs;
            panChunked[static_cast<size_t>(k)] = ev.panAt(t);
            volChunked[static_cast<size_t>(k)] = ev.volAt(t);
        }
        ++bi;
    }

    size_t firstMismatch = static_cast<size_t>(-1);
    for (size_t i = 0; i < static_cast<size_t>(total); ++i)
    {
        if (panWhole[i] != panChunked[i] || volWhole[i] != volChunked[i])
        {
            firstMismatch = i;
            break;
        }
    }
    REQUIRE(firstMismatch == static_cast<size_t>(-1));
}

TEST_CASE("TRANS-6 three short segments: overlap protection and warnings", "[transition]")
{
    // 三连 150ms 短段相接(gap=0):P = −60 / +60 / −60。
    scvb::CurveEvaluator ev;
    ev.build(
        {
            scvb::CurveSegment{0.00, 0.15, -60.0, 0.0},
            scvb::CurveSegment{0.15, 0.30, 60.0, 0.0},
            scvb::CurveSegment{0.30, 0.45, -60.0, 0.0},
        },
        scvb::TransitionConfig{});

    REQUIRE(ev.boundaries().size() == 2u);

    // 每边界 T_eff ≤ 0.9·150 = 135ms(重叠防护绑定)。
    for (const auto& b : ev.boundaries())
    {
        REQUIRE(b.tEffSec == Catch::Approx(0.135).margin(1e-9));
        REQUIRE(b.tEffSec <= 0.135 + 1e-9);
    }

    // 限速突破 → warning 计数 ≥ 1。
    REQUIRE(ev.warningCount() >= 1);
    REQUIRE(ev.boundaries()[0].warnLimit);
    REQUIRE(ev.boundaries()[1].warnLimit);

    // 相邻 ramp 不重叠。
    const auto& b0 = ev.boundaries()[0];
    const auto& b1 = ev.boundaries()[1];
    const double b0Hi = b0.centerSec + b0.tEffSec * 0.5;
    const double b1Lo = b1.centerSec - b1.tEffSec * 0.5;
    REQUIRE(b0Hi <= b1Lo + 1e-12);

    // P(t) 处处单值:全程采样,有限、在值域内、逐样本差分有界(无跳变)。
    const int64_t total = static_cast<int64_t>(0.45 * kFs);
    double prev = ev.panAt(0.0);
    for (int64_t k = 1; k < total; ++k)
    {
        const double t = static_cast<double>(k) / kFs;
        const double p = ev.panAt(t);
        REQUIRE(std::isfinite(p));
        REQUIRE(p >= -100.0 - 1e-9);
        REQUIRE(p <= 100.0 + 1e-9);
        const double diff = std::fabs(p - prev);
        REQUIRE(diff <= 0.03);
        prev = p;
    }
}

TEST_CASE("TRANS-7 zero-length segment degrades to jump (no NaN)", "[transition]")
{
    // 退化段(零长)把 T_eff 夹到 0 → 退化为跳变,不除零、不产 NaN。
    scvb::CurveEvaluator ev;
    ev.build(
        {
            scvb::CurveSegment{0.0, 0.1, -60.0, 0.0},
            scvb::CurveSegment{0.1, 0.1, 0.0, 0.0}, // 零长段
            scvb::CurveSegment{0.1, 0.2, 60.0, 0.0},
        },
        scvb::TransitionConfig{});

    REQUIRE(ev.boundaries().size() == 2u);
    REQUIRE(ev.boundaries()[0].isJump);
    REQUIRE(ev.boundaries()[1].isJump);

    for (int64_t k = 0; k <= 20000; ++k) // 0..0.2s 采样
    {
        const double t = static_cast<double>(k) / kFs;
        const double p = ev.panAt(t);
        REQUIRE(std::isfinite(p));
        REQUIRE(p >= -100.0 - 1e-9);
        REQUIRE(p <= 100.0 + 1e-9);
    }
}
