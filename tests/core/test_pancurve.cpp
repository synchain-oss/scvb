// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
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
            pts.push_back(PanCurvePoint{
                angle,
                static_cast<float>(gainDist(rng)),
                shape,
                static_cast<float>(qDist(rng)),
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
            REQUIRE(std::fabs(lutVal - analytic) <= 0.03);
        }

        // 网格间随机点(200 点)。
        for (int r = 0; r < 200; ++r)
        {
            const double pan = panDist(rng);
            const double analytic = scvb::evalCurve(pts, pan);
            const double lutVal = static_cast<double>(lut.gainDb(static_cast<float>(pan)));
            REQUIRE(std::fabs(lutVal - analytic) <= 0.03);
        }
    }

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

TEST_CASE("CURVE-5 cut (primary analytic assertion)", "[pancurve]")
{
    // A=−12, P₀=−40, Q=2, side=out(P₀<0 → 向左);resDb=20·log10(2/0.7071)=9.0310。
    const PanCurvePoint p = point(-40.0f, -12.0f, PanCurveShape::cut, 2.0f, PanCurveSide::out);
    // G(−40) = A/2 + resDb = −6 + 9.0310 = +3.0310
    REQUIRE(scvb::evalShape(p, -40.0) == Catch::Approx(3.0310).margin(1e-4));
    REQUIRE(scvb::evalShape(p, -100.0) == Catch::Approx(-11.9020).margin(1e-4));
    REQUIRE(scvb::evalShape(p, 0.0) == Catch::Approx(-0.4625).margin(1e-4));
}

TEST_CASE("CURVE-6 shelf side=right (mirror)", "[pancurve]")
{
    // CURVE-2 改 side=right:镜像生效。
    const PanCurvePoint p = point(-45.0f, -6.0f, PanCurveShape::shelf, 2.0f, PanCurveSide::right);
    REQUIRE(scvb::evalShape(p, -45.0) == Catch::Approx(-3.0).margin(1e-4));
    REQUIRE(scvb::evalShape(p, -95.0) == Catch::Approx(-0.1079).margin(1e-4));
    REQUIRE(scvb::evalShape(p, 5.0) == Catch::Approx(-5.8921).margin(1e-4));
}
