// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/PanCurve.h"

#include <algorithm>
#include <cmath>

namespace scvb
{

namespace
{

double clampDb(double db)
{
    if (db < static_cast<double>(kPanCurveMinDb))
        return static_cast<double>(kPanCurveMinDb);
    if (db > static_cast<double>(kPanCurveMaxDb))
        return static_cast<double>(kPanCurveMaxDb);
    return db;
}

// shelf/cut 的 u 取向(§7.1)。bell 不经过此函数(忽略 side)。
double uForSide(const PanCurvePoint& point, double pan)
{
    const double delta = panCurveHalfWidth(point.q);
    switch (point.side)
    {
    case PanCurveSide::left:
        return (static_cast<double>(point.angle) - pan) / delta;
    case PanCurveSide::right:
        return (pan - static_cast<double>(point.angle)) / delta;
    case PanCurveSide::out:
    default:
        // out:背离中心 —— P₀≥0 按 right、P₀<0 按 left。对任意 P₀ 确定性求值,无未定义区。
        if (point.angle >= 0.0f)
            return (pan - static_cast<double>(point.angle)) / delta;
        return (static_cast<double>(point.angle) - pan) / delta;
    }
}

double shelfValue(const PanCurvePoint& point, double pan)
{
    const double u = uForSide(point, pan);
    return static_cast<double>(point.gainDb) * 0.5 * (1.0 + std::tanh(2.0 * u));
}

double bellValue(const PanCurvePoint& point, double pan)
{
    const double delta = panCurveHalfWidth(point.q);
    const double u = (pan - static_cast<double>(point.angle)) / delta;
    return static_cast<double>(point.gainDb) * std::exp2(-(u * u));
}

// resDb = clamp(20·log10(Q/0.7071), 0, 12)。注意 0.7071 为规格字面常量(非 √2/2)。
double resonanceDb(const PanCurvePoint& point)
{
    const double db = 20.0 * std::log10(static_cast<double>(point.q) / 0.7071);
    if (db < 0.0)
        return 0.0;
    if (db > 12.0)
        return 12.0;
    return db;
}

double cutValue(const PanCurvePoint& point, double pan)
{
    const double base = shelfValue(point, pan);
    const double delta = panCurveHalfWidth(point.q);
    const double r = 4.0 * (pan - static_cast<double>(point.angle)) / delta;
    return base + resonanceDb(point) * std::exp2(-(r * r));
}

} // namespace

double panCurveHalfWidth(float q)
{
    return 100.0 / static_cast<double>(q);
}

double evalShape(const PanCurvePoint& point, double pan)
{
    switch (point.shape)
    {
    case PanCurveShape::shelf:
        return shelfValue(point, pan);
    case PanCurveShape::cut:
        return cutValue(point, pan);
    case PanCurveShape::bell:
    default:
        return bellValue(point, pan);
    }
}

double evalCurve(const std::vector<PanCurvePoint>& points, double pan)
{
    double db = 0.0;
    for (const PanCurvePoint& point : points)
        db += evalShape(point, pan);
    return clampDb(db);
}

void PanCurveLut::rebuild(const std::vector<PanCurvePoint>& points)
{
    for (int i = 0; i < kPanCurveLutSize; ++i)
    {
        const float pan = -100.0f + 200.0f * static_cast<float>(i) / static_cast<float>(kPanCurveLutSize - 1);
        double db = 0.0;
        for (const PanCurvePoint& point : points)
            db += evalShape(point, static_cast<double>(pan));
        m_lut[static_cast<size_t>(i)] = static_cast<float>(clampDb(db));
    }
}

float PanCurveLut::gainDb(float pan) const
{
    const float clamped = std::clamp(pan, -100.0f, 100.0f);
    const float x =
        clamped * static_cast<float>(kPanCurveLutSize - 1) / 200.0f + static_cast<float>(kPanCurveLutSize - 1) / 2.0f;
    const int i = static_cast<int>(x);
    const float frac = x - static_cast<float>(i);
    const int i0 = std::max(0, std::min(i, kPanCurveLutSize - 1));
    const int i1 = std::min(i0 + 1, kPanCurveLutSize - 1);
    const float a = m_lut[static_cast<size_t>(i0)];
    const float b = m_lut[static_cast<size_t>(i1)];
    return a + (b - a) * frac;
}

} // namespace scvb
