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

// smoothstep:w²(3−2w) 在 [0,1] 内、外扩 0/1。
double smoothstep01(double x)
{
    const double w = std::clamp(x, 0.0, 1.0);
    return w * w * (3.0 - 2.0 * w);
}

// cut 的侧向距离 d(带号,切除侧为正):right → (P−P₀)、left → (P₀−P)、
// out → P₀≥0 按 right / P₀<0 按 left(与现 u 逻辑一致)。保留侧 d≤0。
double cutSideDistance(const PanCurvePoint& point, double pan)
{
    switch (point.side)
    {
    case PanCurveSide::left:
        return static_cast<double>(point.angle) - pan;
    case PanCurveSide::right:
        return pan - static_cast<double>(point.angle);
    case PanCurveSide::out:
    default:
        if (point.angle >= 0.0f)
            return pan - static_cast<double>(point.angle);
        return static_cast<double>(point.angle) - pan;
    }
}

// cut:slope 模型(02 §7.1 修订)—— q 字段承载 slope(dB/oct);无谐振凸起 R。
//   d = max(|d|, d0)(d0=1°),u = log2(d/d0),u_b = |A|/s,
//   G = A·smoothstep(u/u_b),clamp 到 [A, 0];d≤0(保留侧)→ G=0。
double cutValue(const PanCurvePoint& point, double pan)
{
    const double a = static_cast<double>(point.gainDb); // A(cut 恒 ≤0)
    const double s = static_cast<double>(point.q); // slope(dB/oct)
    if (!(s > 0.0))
        return 0.0; // 防御:非法 slope(契约 q>0)
    const double dRaw = cutSideDistance(point, pan);
    if (dRaw <= 0.0)
        return 0.0; // 保留侧:只切该侧,切点自身不受影响
    const double d = std::max(dRaw, kPanCurveCutD0Deg);
    const double u = std::log2(d / kPanCurveCutD0Deg);
    const double ub = std::fabs(a) / s;
    if (!(ub > 0.0))
        return 0.0; // A=0 → 无切除(避免 0/0 = NaN)
    const double g = a * smoothstep01(u / ub);
    return std::clamp(g, std::min(a, 0.0), std::max(a, 0.0)); // clamp 到 [A, 0]
}

} // namespace

bool isPanCurvePointUsable(const PanCurvePoint& point)
{
    // 顺序有意:**先 isfinite,再比大小**。反过来没用 —— NaN 的所有比较都是 false,
    // 范围判定会把它当成「在范围内」放行(桥面原有的那三条比较正是这么漏的)。
    if (!std::isfinite(point.angle) || !std::isfinite(point.gainDb) || !std::isfinite(point.q))
        return false;
    if (point.angle < kPanCurvePointAngleMin || point.angle > kPanCurvePointAngleMax)
        return false;
    if (!(point.q > 0.0f)) // 写成 !(>0) 而非 <=0:对 NaN 两者不等价(上面已挡,这里是第二道)
        return false;
    // gainDb 只要求有限(宪法未声明每点值域,理由见头文件)。有限的极端值不会产出 NaN:
    // 大 |A| 经 clampDb 收进 [-24,+12],小 |A| 无害 —— 由 SL442-NAN-3 的极端有限值用例钉住。
    return true;
}

bool arePanCurvePointsUsable(const std::vector<PanCurvePoint>& points)
{
    for (const PanCurvePoint& p : points)
    {
        if (!isPanCurvePointUsable(p))
            return false;
    }
    return true;
}

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
    // ⚠ [SL-442] **`std::clamp` 挡不住 NaN**:`v < lo` 与 `hi < v` 对 NaN 都是 false,
    //   于是 NaN 被原样返回。读到这一行的人会以为 `clamped`(以及下面的 `x`)从此有界 ——
    //   **它没有。** 这与本文件 `clampDb` 是**同一个坑,在同一个文件里的第二次**。
    const float clamped = std::clamp(pan, -100.0f, 100.0f);
    const float x =
        clamped * static_cast<float>(kPanCurveLutSize - 1) / 200.0f + static_cast<float>(kPanCurveLutSize - 1) / 2.0f;
    // ⚠ NaN 输入下 `x` 是 NaN,`static_cast<int>(NaN)` 是 UB(产出一个垃圾 int)。
    //   下面 `i0` 的钳制把它收回合法区间 ⇒ **不会越界访问**;输出经 `frac` 退化为 NaN。
    //   —— 危害等级是「输出 NaN」,不是「内存损坏」。
    // [SL-442] `pan` 的来源:音频线程侧来自 DspArbiter 的 pan 平滑器(曲线值 / host 参数,
    //   再经 scaleByGlobalWidth),**不来自 pan_curve 的点数据** —— 点数据只决定表的内容,
    //   不决定索引。「这里取不到 NaN」整个建立在这条来源上;来源一变,这几行要重新看。
    const int i = static_cast<int>(x);
    const float frac = x - static_cast<float>(i);
    const int i0 = std::max(0, std::min(i, kPanCurveLutSize - 1));
    const int i1 = std::min(i0 + 1, kPanCurveLutSize - 1);
    const float a = m_lut[static_cast<size_t>(i0)];
    const float b = m_lut[static_cast<size_t>(i1)];
    return a + (b - a) * frac;
}

} // namespace scvb
