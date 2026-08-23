// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

#include "analysis/PanCurve.h"

using scvb::PanCurvePoint;
using scvb::PanCurveShape;
using scvb::PanCurveSide;

namespace
{

PanCurvePoint point(float angle, float gain, PanCurveShape shape, float q, PanCurveSide side)
{
    return PanCurvePoint{angle, gain, shape, q, side};
}

} // namespace

TEST_CASE("CURVE-1 bell", "[pancurve]")
{
    // A=+6, P₀=30, Q=3(Δ=100/3≈33.33)。规格里的 63.33 / −3.33 / 96.67 是
    // P₀±Δ / P₀+2Δ 的两位小数显示;此处按精确位置(P₀±Δ、P₀+2Δ)取 u=0/±1/2 的干净值。
    const PanCurvePoint p = point(30.0f, 6.0f, PanCurveShape::bell, 3.0f, PanCurveSide::out);
    const double delta = scvb::panCurveHalfWidth(3.0f); // = 100/3
    REQUIRE(scvb::evalShape(p, 30.0) == Catch::Approx(6.0).margin(1e-4));
    REQUIRE(scvb::evalShape(p, 30.0 + delta) == Catch::Approx(3.0).margin(1e-4));
    REQUIRE(scvb::evalShape(p, 30.0 - delta) == Catch::Approx(3.0).margin(1e-4));
    REQUIRE(scvb::evalShape(p, 30.0 + 2.0 * delta) == Catch::Approx(0.375).margin(1e-4));
}

TEST_CASE("CURVE-2 shelf side=out (P0<0 → left)", "[pancurve]")
{
    // A=−6, P₀=−45, Q=2, side=out(Δ=50,P₀<0 → 向左)
    const PanCurvePoint p = point(-45.0f, -6.0f, PanCurveShape::shelf, 2.0f, PanCurveSide::out);
    REQUIRE(scvb::evalShape(p, -45.0) == Catch::Approx(-3.0).margin(1e-4));
    REQUIRE(scvb::evalShape(p, -95.0) == Catch::Approx(-5.8921).margin(1e-4));
    REQUIRE(scvb::evalShape(p, 5.0) == Catch::Approx(-0.1079).margin(1e-4));
}

TEST_CASE("CURVE-3 two bells clamp to +12", "[pancurve]")
{
    const std::vector<PanCurvePoint> pts = {
        point(0.0f, 8.0f, PanCurveShape::bell, 1.5f, PanCurveSide::out),
        point(0.0f, 8.0f, PanCurveShape::bell, 1.5f, PanCurveSide::out),
    };
    // G(0)=16 → clamp +12
    REQUIRE(scvb::evalCurve(pts, 0.0) == Catch::Approx(12.0).margin(1e-6));
}

TEST_CASE("CURVE-4 LUT vs analytic <= 0.03 dB", "[pancurve]")
{
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_real_distribution<double> angleDist(-100.0, 100.0);
    std::uniform_real_distribution<double> gainDist(-12.0, 12.0);
    std::uniform_real_distribution<double> qDist(0.5, 10.0);
    std::uniform_int_distribution<int> shapeDist(0, 2);
    std::uniform_int_distribution<int> sideDist(0, 2);
    std::uniform_real_distribution<double> panDist(-100.0, 100.0);
    // cut:q 承载 slope(dB/oct,6|12|18|24);gain 取代表性切除深度(避免极小 |A| 的病态窄斜坡)。
    std::uniform_int_distribution<int> slopeIdxDist(0, 3);
    std::uniform_int_distribution<int> cutGainIdxDist(0, 4);
    const std::array<float, 4> kSlopes = {6.0f, 12.0f, 18.0f, 24.0f};
    const std::array<float, 5> kCutGains = {-3.0f, -6.0f, -12.0f, -18.0f, -24.0f};

    double worstErr = 0.0;

    for (int iter = 0; iter < 20; ++iter)
    {
        std::vector<PanCurvePoint> pts;
        const int count = 1 + (iter % 5); // 1..5 点
        for (int i = 0; i < count; ++i)
        {
            // 首点显式注入 side=out 且 |P₀|<5 的合法组合(§7.1 确定性求值)。
            const float angle = (i == 0) ? ((iter % 2 == 0) ? 2.0f : -3.0f) : static_cast<float>(angleDist(rng));
            const PanCurveShape shape = (i == 0) ? PanCurveShape::shelf : static_cast<PanCurveShape>(shapeDist(rng));
            const PanCurveSide side = (i == 0) ? PanCurveSide::out : static_cast<PanCurveSide>(sideDist(rng));
            float gain = static_cast<float>(gainDist(rng));
            float q = static_cast<float>(qDist(rng));
            if (shape == PanCurveShape::cut)
            {
                q = kSlopes[slopeIdxDist(rng)];
                gain = kCutGains[cutGainIdxDist(rng)];
            }
            pts.push_back(PanCurvePoint{
                angle,
                gain,
                shape,
                q,
                side,
            });
        }

        scvb::PanCurveLut lut;
        lut.rebuild(pts);

        // 网格点(801 点)。
        for (int g = 0; g < scvb::kPanCurveLutSize; ++g)
        {
            const float pan = -100.0f + 200.0f * static_cast<float>(g) / static_cast<float>(scvb::kPanCurveLutSize - 1);
            const double analytic = scvb::evalCurve(pts, static_cast<double>(pan));
            const double lutVal = static_cast<double>(lut.gainDb(pan));
            const double err = std::fabs(lutVal - analytic);
            if (err > worstErr)
                worstErr = err;
            REQUIRE(err <= 0.03);
        }

        // 网格间随机点(200 点)。
        for (int r = 0; r < 200; ++r)
        {
            const double pan = panDist(rng);
            const double analytic = scvb::evalCurve(pts, pan);
            const double lutVal = static_cast<double>(lut.gainDb(static_cast<float>(pan)));
            const double err = std::fabs(lutVal - analytic);
            if (err > worstErr)
                worstErr = err;
            REQUIRE(err <= 0.03);
        }
    }

    // 打印最坏误差(CI 定位用;正常应 ≤0.03 且有明显余量)。
    std::cout << "  CURVE-4 worst LUT-vs-analytic = " << worstErr << " dB\n";

    // G(±100) 有限、无 NaN。
    const std::vector<PanCurvePoint> pts = {
        point(-40.0f, -12.0f, PanCurveShape::cut, 10.0f, PanCurveSide::out),
        point(30.0f, 6.0f, PanCurveShape::bell, 10.0f, PanCurveSide::out),
    };
    scvb::PanCurveLut lut;
    lut.rebuild(pts);
    REQUIRE(std::isfinite(static_cast<double>(lut.gainDb(-100.0f))));
    REQUIRE(std::isfinite(static_cast<double>(lut.gainDb(100.0f))));
}

TEST_CASE("side=out with |P0|<5 is deterministic", "[pancurve]")
{
    // out(angle=+2) 应与显式 right 逐点一致;out(angle=−3) 应与显式 left 逐点一致。
    const PanCurvePoint outRight = point(2.0f, -6.0f, PanCurveShape::shelf, 2.0f, PanCurveSide::out);
    const PanCurvePoint right = point(2.0f, -6.0f, PanCurveShape::shelf, 2.0f, PanCurveSide::right);
    const PanCurvePoint outLeft = point(-3.0f, -6.0f, PanCurveShape::shelf, 2.0f, PanCurveSide::out);
    const PanCurvePoint left = point(-3.0f, -6.0f, PanCurveShape::shelf, 2.0f, PanCurveSide::left);

    for (double p : {-100.0, -50.0, 0.0, 50.0, 100.0})
    {
        REQUIRE(scvb::evalShape(outRight, p) == Catch::Approx(scvb::evalShape(right, p)).margin(1e-9));
        REQUIRE(scvb::evalShape(outLeft, p) == Catch::Approx(scvb::evalShape(left, p)).margin(1e-9));
    }
}

TEST_CASE("CURVE-5 cut slope (primary analytic assertion)", "[pancurve]")
{
    // cut 改 slope 模型:q 字段承载 slope(dB/oct);d = max(|侧向距离|, 1°),
    // u = log2(d/1°),u_b = |A|/s,G = A·smoothstep(u/u_b) 并 clamp 到 [A, 0]。
    // d≤0(保留侧)→ G=0;d<1°(切点自身及 1° 内)→ G=0。解析期望值,非实现回放。
    const double sqrt2 = std::sqrt(2.0);

    // A=−12,s=12 → u_b=1;side=right:d = P−30(P>30 为切除侧)。
    const PanCurvePoint p12r = point(30.0f, -12.0f, PanCurveShape::cut, 12.0f, PanCurveSide::right);
    REQUIRE(scvb::evalShape(p12r, 32.0) == Catch::Approx(-12.0).margin(1e-4)); // d=2°→u=1→smoothstep=1
    REQUIRE(scvb::evalShape(p12r, 30.0 + sqrt2) == Catch::Approx(-6.0).margin(1e-4)); // d=√2°→u=0.5→smoothstep=0.5
    REQUIRE(scvb::evalShape(p12r, 31.0) == Catch::Approx(0.0).margin(1e-4)); // d=1°→G=0
    REQUIRE(scvb::evalShape(p12r, 30.5) == Catch::Approx(0.0).margin(1e-4)); // d=0.5°<d0→G=0
    REQUIRE(scvb::evalShape(p12r, 20.0) == Catch::Approx(0.0).margin(1e-4)); // 保留侧(d<0)→G=0

    // side=left(镜像):d = 30−P(P<30 为切除侧)。
    const PanCurvePoint p12l = point(30.0f, -12.0f, PanCurveShape::cut, 12.0f, PanCurveSide::left);
    REQUIRE(scvb::evalShape(p12l, 28.0) == Catch::Approx(-12.0).margin(1e-4)); // d=2°
    REQUIRE(scvb::evalShape(p12l, 30.0 - sqrt2) == Catch::Approx(-6.0).margin(1e-4)); // d=√2°
    REQUIRE(scvb::evalShape(p12l, 40.0) == Catch::Approx(0.0).margin(1e-4)); // 保留侧(P>30)→G=0

    // side=out,P₀=−30(<0 → left):d = −30−P。
    const PanCurvePoint pOut = point(-30.0f, -12.0f, PanCurveShape::cut, 12.0f, PanCurveSide::out);
    REQUIRE(scvb::evalShape(pOut, -32.0) == Catch::Approx(-12.0).margin(1e-4)); // d=2°
    REQUIRE(scvb::evalShape(pOut, -20.0) == Catch::Approx(0.0).margin(1e-4)); // 保留侧(P>−30)→G=0

    // s=6 → u_b=2:满切在 d=2²=4°。
    const PanCurvePoint p6 = point(30.0f, -12.0f, PanCurveShape::cut, 6.0f, PanCurveSide::right);
    REQUIRE(scvb::evalShape(p6, 34.0) == Catch::Approx(-12.0).margin(1e-4)); // d=4°→u=2→u/u_b=1
    REQUIRE(scvb::evalShape(p6, 32.0) == Catch::Approx(-6.0).margin(1e-4)); // d=2°→u=1→u/u_b=0.5

    // s=24 → u_b=0.5:满切在 d=2^0.5=√2°。
    const PanCurvePoint p24 = point(30.0f, -12.0f, PanCurveShape::cut, 24.0f, PanCurveSide::right);
    REQUIRE(scvb::evalShape(p24, 30.0 + sqrt2) == Catch::Approx(-12.0).margin(1e-4)); // d=√2°→u=0.5→u/u_b=1
    REQUIRE(scvb::evalShape(p24, 30.0 + std::sqrt(sqrt2)) ==
            Catch::Approx(-6.0).margin(1e-4)); // d=2^0.25→u=0.25→u/u_b=0.5
}

TEST_CASE("CURVE-6 shelf side=right (mirror)", "[pancurve]")
{
    // CURVE-2 改 side=right:镜像生效。
    const PanCurvePoint p = point(-45.0f, -6.0f, PanCurveShape::shelf, 2.0f, PanCurveSide::right);
    REQUIRE(scvb::evalShape(p, -45.0) == Catch::Approx(-3.0).margin(1e-4));
    REQUIRE(scvb::evalShape(p, -95.0) == Catch::Approx(-0.1079).margin(1e-4));
    REQUIRE(scvb::evalShape(p, 5.0) == Catch::Approx(-5.8921).margin(1e-4));
}

TEST_CASE("CURVE-7 cut slope LUT worst-case <= 0.015 dB", "[pancurve]")
{
    // 确定性最坏误差断言:cut slope 斜坡在角度域极窄,线性插值最坏误差 ≤0.015 dB
    // (0.03 dB 测试上限的 2× 余量)。覆盖 A∈{-3..-24} × s∈{6,12,18,24} × side 三值 ×
    // P0 代表性扫描(含模拟实测最坏相位),斜坡区 ±2° @ 0.001° 细扫。
    const std::array<float, 5> kCutGains = {-3.0f, -6.0f, -12.0f, -18.0f, -24.0f};
    const std::array<float, 4> kSlopes = {6.0f, 12.0f, 18.0f, 24.0f};
    const std::array<PanCurveSide, 3> kSides = {PanCurveSide::out, PanCurveSide::left, PanCurveSide::right};
    const std::array<float, 6> kP0 = {-64.5f, -33.9f, -3.0f, 2.0f, 30.0f, 45.0f};

    double worst = 0.0;
    PanCurvePoint worstPt;
    double worstPan = 0.0;

    for (const float gain : kCutGains)
        for (const float slope : kSlopes)
            for (const PanCurveSide side : kSides)
                for (const float p0 : kP0)
                {
                    const PanCurvePoint pt = point(p0, gain, PanCurveShape::cut, slope, side);
                    scvb::PanCurveLut lut;
                    lut.rebuild({pt});
                    for (double pan = static_cast<double>(p0) - 2.0; pan <= static_cast<double>(p0) + 2.0; pan += 0.001)
                    {
                        const double err = std::fabs(static_cast<double>(lut.gainDb(static_cast<float>(pan))) -
                                                     scvb::evalShape(pt, pan));
                        if (err > worst)
                        {
                            worst = err;
                            worstPt = pt;
                            worstPan = pan;
                        }
                    }
                }

    std::cout << "  CURVE-7 worst LUT error = " << worst << " dB @ P0=" << worstPt.angle << ", A=" << worstPt.gainDb
              << ", s=" << worstPt.q << ", side=" << static_cast<int>(worstPt.side) << ", pan=" << worstPan << "\n";
    REQUIRE(worst <= 0.015);
}
