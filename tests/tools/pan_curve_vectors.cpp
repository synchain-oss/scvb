// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// pan_curve_vectors.cpp —— T34 对拍离线工具。
// -----------------------------------------------------------------------------
// 打印 JS↔C++ 对拍点集的 2049 点 LUT 与采样插值,供 scripts/check-curve-parity.mjs
// 生成 scripts/curve-parity-golden.json(C++ 侧真源)。本工具零外部依赖,只链接
// scvb_core 的 PanCurve.{h,cpp}(02 §7 / PanCurve.h),不链接 JUCE / Catch2。
//
// **点集必须与 scripts/check-curve-parity.mjs 的 PARITY_SETS 逐字节一致**(两处同源,
// 漂移会被对拍脚本以 ≤0.01 dB 判据打红)。覆盖:side 三值、side=out 且 |P0|<5
// 的确定性回退点(CURVE-1..6 同源向量)。
// =============================================================================

#include "analysis/PanCurve.h"

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using scvb::PanCurveLut;
using scvb::PanCurvePoint;
using scvb::PanCurveShape;
using scvb::PanCurveSide;

namespace
{

PanCurvePoint P(float angle, float gain, PanCurveShape shape, float q, PanCurveSide side)
{
    PanCurvePoint p;
    p.angle = angle;
    p.gainDb = gain;
    p.shape = shape;
    p.q = q;
    p.side = side;
    return p;
}

struct Set
{
    const char* id;
    std::vector<PanCurvePoint> points;
};

std::vector<Set> paritySets()
{
    return {
        // CURVE-1:bell A=+6, P0=30, Q=3(02 §7.3)
        {"curve-1", {P(30.0f, 6.0f, PanCurveShape::bell, 3.0f, PanCurveSide::out)}},
        // CURVE-2:shelf A=−6, P0=−45, Q=2, side=out(P0<0 → left)
        {"curve-2", {P(-45.0f, -6.0f, PanCurveShape::shelf, 2.0f, PanCurveSide::out)}},
        // CURVE-3:两个 bell A=+8 同 P0=0 → clamp +12
        {"curve-3",
         {P(0.0f, 8.0f, PanCurveShape::bell, 1.5f, PanCurveSide::out),
          P(0.0f, 8.0f, PanCurveShape::bell, 1.5f, PanCurveSide::out)}},
        // CURVE-5:cut A=−12, P0=−40, Q=2, side=out(谐振)
        {"curve-4", {P(-40.0f, -12.0f, PanCurveShape::cut, 2.0f, PanCurveSide::out)}},
        // CURVE-6:CURVE-2 改 side=right(镜像)
        {"curve-5", {P(-45.0f, -6.0f, PanCurveShape::shelf, 2.0f, PanCurveSide::right)}},
        // side=out 且 |P0|<5(确定性回退:angle≥0 → right)
        {"out-center-right", {P(2.0f, -6.0f, PanCurveShape::shelf, 2.0f, PanCurveSide::out)}},
        // side=out 且 |P0|<5(确定性回退:angle<0 → left)+ cut 谐振
        {"out-center-left", {P(-3.0f, -8.0f, PanCurveShape::cut, 3.0f, PanCurveSide::out)}},
        // 混合:三 shape × 三 side 全覆盖
        {"mixed",
         {P(-70.0f, -4.0f, PanCurveShape::shelf, 1.0f, PanCurveSide::left),
          P(-30.0f, 3.0f, PanCurveShape::bell, 2.0f, PanCurveSide::out),
          P(-8.0f, -2.0f, PanCurveShape::cut, 2.5f, PanCurveSide::out),
          P(15.0f, 2.5f, PanCurveShape::bell, 1.5f, PanCurveSide::out),
          P(55.0f, -3.0f, PanCurveShape::shelf, 0.8f, PanCurveSide::right),
          P(85.0f, 1.5f, PanCurveShape::cut, 4.0f, PanCurveSide::right)}},
    };
}

const char* shapeName(PanCurveShape s)
{
    switch (s)
    {
    case PanCurveShape::shelf:
        return "shelf";
    case PanCurveShape::cut:
        return "cut";
    case PanCurveShape::bell:
    default:
        return "bell";
    }
}

const char* sideName(PanCurveSide s)
{
    switch (s)
    {
    case PanCurveSide::left:
        return "left";
    case PanCurveSide::right:
        return "right";
    case PanCurveSide::out:
    default:
        return "out";
    }
}

} // namespace

int main()
{
    constexpr int kSamples = 401; // −100..+100 step 0.5(网格点 + 网格间点,验证插值)

    std::cout << std::setprecision(9);
    const std::vector<Set> all = paritySets();

    std::cout << "{\n  \"lutSize\": " << scvb::kPanCurveLutSize << ",\n  \"sets\": [\n";
    for (std::size_t si = 0; si < all.size(); ++si)
    {
        const Set& s = all[si];
        PanCurveLut lut;
        lut.rebuild(s.points);

        std::cout << "    {\n      \"id\": \"" << s.id << "\",\n      \"points\": [";
        for (std::size_t pi = 0; pi < s.points.size(); ++pi)
        {
            const PanCurvePoint& pt = s.points[pi];
            if (pi != 0)
                std::cout << ",";
            std::cout << "{\"angle\":" << pt.angle << ",\"gain_db\":" << pt.gainDb
                      << ",\"shape\":\"" << shapeName(pt.shape) << "\",\"q\":" << pt.q
                      << ",\"side\":\"" << sideName(pt.side) << "\"}";
        }
        std::cout << "],\n      \"lut\": [";
        for (int i = 0; i < scvb::kPanCurveLutSize; ++i)
        {
            if (i != 0)
                std::cout << ",";
            std::cout << lut.data()[i];
        }
        std::cout << "],\n      \"samples\": [";
        for (int i = 0; i < kSamples; ++i)
        {
            const float pan = -100.0f + 200.0f * static_cast<float>(i) / static_cast<float>(kSamples - 1);
            if (i != 0)
                std::cout << ",";
            std::cout << "[" << pan << "," << lut.gainDb(pan) << "]";
        }
        std::cout << "]\n    }";
        if (si + 1 < all.size())
            std::cout << ",";
        std::cout << "\n";
    }
    std::cout << "  ]\n}\n";
    return 0;
}
