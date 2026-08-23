// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <vector>

// PanCurve:pan 角度域增益曲线 G: P ∈ [−100,+100] → dB(02 §7)。
// bell/shelf/cut 三种形状,2049 点 LUT;UI 与 DSP 共用本实现(曲线同源)。
namespace scvb
{

enum class PanCurveShape : int
{
    bell = 0,
    shelf = 1,
    cut = 2
};

// [J07] shelf/cut 的方向;bell 忽略 side。
enum class PanCurveSide : int
{
    out = 0,
    left = 1,
    right = 2
};

// params-v0 v1 的 points[] 单点:{angle(=P₀), gain_db(=A), shape, q, side}。
struct PanCurvePoint
{
    float angle = 0.0f; // P₀ ∈ [−100, +100]
    float gainDb = 0.0f; // A(dB)
    PanCurveShape shape = PanCurveShape::bell;
    float q = 1.5f; // bell/shelf 为 Q ∈ [0.5, 10];cut 为 slope(dB/oct,6|12|18|24,默认 12)
    PanCurveSide side = PanCurveSide::out;
};

// LUT 点数:−100..+100,步长 ≈0.0061(32769 点,奇数保证 0° 恰为格点)。
// cut slope 的 smoothstep 斜坡在角度域极窄(A=−3,s=24 → u_b=0.125 oct,斜坡仅 ~0.09°),
// 2049 点(步长 0.0977)在该区间只有 ~1 个采样点,线性插值最坏误差可超 0.03 dB(CI 实测 0.034)。
// 提密度至 32769(步长 0.0061)后斜坡内 ~15 点,最坏误差 ≈0.010 dB(模拟实测),
// 稳过 02 §7.2 的 ≤0.03 dB 上限(留 ≥2× 余量)。
inline constexpr int kPanCurveLutSize = 32769;

// cut slope 模型的侧向距离地板:d = max(|d|, d0),d0 = 1.0(度)。
// d<d0(切点自身及 1° 内)→ G=0,斜坡自 1° 起。
inline constexpr double kPanCurveCutD0Deg = 1.0;

// 曲线整体 clamp 边界(02 §7.1)。
inline constexpr float kPanCurveMinDb = -24.0f;
inline constexpr float kPanCurveMaxDb = 12.0f;

// 半宽 Δ = 100 / Q(P 单位)。
double panCurveHalfWidth(float q);

// 单点形状求值(dB,不 clamp)。
// bell:u = (P − P₀)/Δ(side 忽略);
// shelf:u 由 side 决定 —— right → (P − P₀)/Δ、left → (P₀ − P)/Δ、
//        out → P₀≥0 按 right / P₀<0 按 left(对任意 P₀ 确定性求值,无未定义区)。
// cut:slope 模型 —— d = 侧向距离(right→P−P₀、left→P₀−P、out→sign(P₀) 定),
//      d = max(|d|, d0)(d0=1°),u = log2(d/d0),u_b = |A|/s(s=point.q),
//      G = A·smoothstep(u/u_b) 并 clamp 到 [A, 0];d≤0(保留侧)→ G=0。
double evalShape(const PanCurvePoint& point, double pan);

// 整条曲线解析求值:Σ 各点 + clamp 到 [−24, +12] dB。
double evalCurve(const std::vector<PanCurvePoint>& points, double pan);

// 2049 点 LUT:参数变化时重建(消息线程),音频线程只读;clamp + 线性插值。
class PanCurveLut
{
public:
    PanCurveLut() { m_lut.fill(0.0f); }

    void rebuild(const std::vector<PanCurvePoint>& points);

    // §7.2 gainDb:clamp + 线性插值。
    float gainDb(float pan) const;

    // 供 WebView bridge / 测试读取原始 LUT(不插值)。
    const float* data() const noexcept { return m_lut.data(); }

private:
    std::array<float, kPanCurveLutSize> m_lut;
};

} // namespace scvb
