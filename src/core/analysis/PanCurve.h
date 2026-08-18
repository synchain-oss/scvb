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
    float q = 1.5f; // Q ∈ [0.5, 10]
    PanCurveSide side = PanCurveSide::out;
};

// LUT 点数:−100..+100,步长 ≈0.0977(2049 点,奇数保证 0° 恰为格点)。
// 801 点(步长 0.25)的线性插值误差在部分机器上可超 0.03 dB(CI 实测 0.0567),
// 提密度至 2049 后误差 ≈0.009 dB,全平台稳过 02 §7.2 的 ≤0.03 dB 上限。
inline constexpr int kPanCurveLutSize = 2049;

// 曲线整体 clamp 边界(02 §7.1)。
inline constexpr float kPanCurveMinDb = -24.0f;
inline constexpr float kPanCurveMaxDb = 12.0f;

// 半宽 Δ = 100 / Q(P 单位)。
double panCurveHalfWidth(float q);

// 单点形状求值(dB,不 clamp)。
// bell:u = (P − P₀)/Δ(side 忽略);
// shelf/cut:u 由 side 决定 —— right → (P − P₀)/Δ、left → (P₀ − P)/Δ、
//           out → P₀≥0 按 right / P₀<0 按 left(对任意 P₀ 确定性求值,无未定义区)。
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
