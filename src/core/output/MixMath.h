// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MixMath —— Output 混音的 DSP 原语(01 §5.2 + 02 §8.1/§8.4 + ADR-010)。
//   mono   = equal-power pan(全局 width 几何缩放)+ vol dB→线性 + 注入 fade;
//   stereo = dual-pan+width(名义子声像 clamp(P∓w) → 全局 width 缩放 → 各自 equal-power);
//   ms_balance = 总线 M/S 音量比(纯衰减 crossfade,g_M/g_S ∈ [0,1])。
// JUCE-free,可离线单测。ms_balance==0 必须与不施加逐样本按位相等 —— null test 前提(§5.2 [J58])。
//
// [SL-442] pan 角度域曲线 G(02 §7)在此施加,完成 §8.1 步骤 5 的后半句:
//   「(gL,gR) = panGains(P_ch,eff);子声像增益 = 10^((v + G(P_ch,eff))/20)」
// 三点冻结,改之前先读:
//   ① **叠加不替代** —— G 在 dB 域加到 v(= 引擎值层的 volDb)上,不替代 v 的任何部分;
//   ② **查在 P_eff 上** —— 即经 `scaleByGlobalWidth` 缩放**之后**的角,不是名义角;
//   ③ stereo **每个子声像各查一次**(与 §6.1 的 ĉ_L/ĉ_R 同构),不是拿弧中心查一次。
// ⚠ ② 与分析侧**故意不同轴**:§6.1 解 c_i 用的是名义 P(分析基准 width=100%),这里用 P_eff。
//    这不是 bug,别「修」成一致 —— 正是这个差异使得「改了 width 要提示重新平衡」(§6.5)成立。
//    (那条 dirty 提示当前**尚未实现**,是 §6.5 的已知缺口,已另行登记整族卡。)

#include <algorithm>
#include <cmath>

#include "analysis/PanCurve.h"
#include "dsp/PanMath.h"

namespace scvb::output
{

// dB → 线性增益。
inline float dbToLinear(float db) noexcept
{
    return std::pow(10.0f, db / 20.0f);
}

// 总线 M/S 音量比(02 §8.4):M=(L+R)/2、S=(L−R)/2;L'=g_M·M+g_S·S、R'=g_M·M−g_S·S。
// g_M==1 且 g_S==1 → 矩阵恒等,短路(逐位透传,不产生浮点舍入)—— null test 前提。
inline void applyMsGains(float gM, float gS, float& l, float& r) noexcept
{
    if (gM == 1.0f && gS == 1.0f)
    {
        return;
    }
    const float m = 0.5f * (l + r);
    const float s = 0.5f * (l - r);
    l = gM * m + gS * s;
    r = gM * m - gS * s;
}

// 从 ms_balance 参数值导出 g_M/g_S 后施加(02 §8.4):t = ms_balance/100 ∈ [-1,+1];
// g_M = 1-max(t,0)、g_S = 1-max(-t,0)(纯衰减 crossfade)。ms_balance==0 → 短路。
inline void applyMsBalance(float msBalance, float& l, float& r) noexcept
{
    if (msBalance == 0.0f)
    {
        return; // null test 前提:逐样本按位相等(矩阵恒等短路)
    }
    const float t = msBalance / 100.0f;
    applyMsGains(1.0f - std::max(t, 0.0f), 1.0f - std::max(-t, 0.0f), l, r);
}

// G 的**线性**增益因子 = 10^(G(P_eff)/20)。
//
// ⚠ 为什么不写成规格字面的 10^((v + G)/20),而拆成 10^(v/20) · 10^(G/20):
// 两者在实数域相等,但**浮点下不等**,而这里有一条硬约束 —— 没画过曲线的工程接上 G 之后
// 必须**逐位**不变。G≡0 时本函数回**恰好** 1.0f,乘上去是按位恒等;写成 dB 域相加的话,
// stereo 那边为了给两个子声像各自的 G 就必须把 `(a+b)·vg` 拆成 `a·vgL + b·vgR`,
// 而 `(a+b)·c` 与 `a·c + b·c` 的舍入结果不同 —— null test 当场就破了。
// 口径与本文件 `applyMsGains` 的 g_M==g_S==1 短路一致:恒等变换必须按位透传。
//
// 顺带:G≡0 时走的是 `db == 0.0f` 那条早退,**不调 std::pow** —— 这是音频线程的逐样本
// 逐轨内循环。没画曲线的工程(绝大多数)因此只多付查表那一下:
// **mono 每样本每轨 1 次、stereo 2 次**(两个子声像各查各的);淡入窗口内各再多一次。
//
// 实测(15 轨 × 20 秒音频,/O2,占一颗核):
//   mono   无曲线 0.446% → 只查表不 pow 0.534% → 查表+pow 0.988% → 窗口内 1.186%
//   stereo 无曲线 1.034% → 只查表不 pow 1.156% → 查表+pow 2.087% → 窗口内 2.422%
// ⇒ 画了曲线时新增约 1 个百分点的一颗核,其中 **约 84% 是 std::pow,查表只占约 16%**。
// ⚠ 别为此把 `dbToLinear` 的 pow 换成 exp2 近似:那会同时改变**基线路径**的舍入,
//    而「空曲线逐位不变」(SL442-MIX-1,无容差)正是靠基线路径逐字不变成立的。
inline float panCurveLinearGain(const scvb::PanCurveXfade& curve, float panEff) noexcept
{
    const float db = panCurveGainDb(curve, panEff);
    return (db == 0.0f) ? 1.0f : dbToLinear(db);
}

// mono 单样本混入(01 §5.2 + 02 §8.1):equal-power pan(sin/cos,ADR-010)+ vol + G + 注入 fade。
inline void mixMonoSample(float sample, float pan, float volDb, float globalWidth, float fade,
                          const scvb::PanCurveXfade& curve, float& l, float& r) noexcept
{
    if (fade <= 0.0f)
    {
        return;
    }
    // ★ G 查在 P_eff(全局 width 缩放**之后**的角)上,不是 pan —— §8.1 步骤 5。
    const float panEff = scaleByGlobalWidth(pan, globalWidth);
    float gL = 0.0f;
    float gR = 0.0f;
    panGains(panEff, gL, gR);
    const float g = dbToLinear(volDb) * panCurveLinearGain(curve, panEff) * fade;
    l += sample * gL * g;
    r += sample * gR * g;
}

// stereo 单样本混入(01 §5.2 + 02 §8.1):dual-pan + 每轨 width。
// 求值链 = 名义子声像 clamp(P∓w_t) → 全局 width 几何缩放 → 每子声像独立 equal-power。
// 全局 width=100 时与 PanMath::dualPanGains(pan, trkWidth)(T14 交付)完全等价。
inline void mixStereoSample(float sL, float sR, float pan, float volDb, float trkWidth, float globalWidth, float fade,
                            const scvb::PanCurveXfade& curve, float& l, float& r) noexcept
{
    if (fade <= 0.0f)
    {
        return;
    }
    const float subLNom = clampPan(pan - trkWidth);
    const float subRNom = clampPan(pan + trkWidth);
    const float subL = scaleByGlobalWidth(subLNom, globalWidth);
    const float subR = scaleByGlobalWidth(subRNom, globalWidth);
    float gL_L = 0.0f;
    float gR_L = 0.0f;
    float gL_R = 0.0f;
    float gR_R = 0.0f;
    panGains(subL, gL_L, gR_L);
    panGains(subR, gL_R, gR_R);

    // ★ 两个子声像**各查一次** G(与 §6.1 的 ĉ_L/ĉ_R 同构),且查在缩放后的 subL/subR 上。
    // 拿弧中心查一次再共用是错的:张开度一大,两个子声像会落在曲线上差别很大的两处,
    // 共用等于把一侧的增益按到另一侧头上。
    // 折进各自的 pan 增益(而不是折进 vg):G≡0 时两个因子**恰为 1.0f**,乘法按位恒等,
    // 下面那两行求和式的结构与接 G 之前逐字不变 ⇒ null test 成立(见 panCurveLinearGain 的注)。
    const float cL = panCurveLinearGain(curve, subL);
    const float cR = panCurveLinearGain(curve, subR);
    gL_L *= cL;
    gR_L *= cL;
    gL_R *= cR;
    gR_R *= cR;

    const float vg = dbToLinear(volDb) * fade;
    l += (sL * gL_L + sR * gL_R) * vg;
    r += (sL * gR_L + sR * gR_R) * vg;
}

} // namespace scvb::output
