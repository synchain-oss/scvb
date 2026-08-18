// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MixMath —— Output 混音的 DSP 原语(01 §5.2 + 02 §8.1/§8.4 + ADR-010)。
//   mono   = equal-power pan(全局 width 几何缩放)+ vol dB→线性 + 注入 fade;
//   stereo = dual-pan+width(名义子声像 clamp(P∓w) → 全局 width 缩放 → 各自 equal-power);
//   ms_balance = 总线 M/S 音量比(纯衰减 crossfade,g_M/g_S ∈ [0,1])。
// JUCE-free,可离线单测。ms_balance==0 必须与不施加逐样本按位相等 —— null test 前提(§5.2 [J58])。

#include <algorithm>
#include <cmath>

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

// mono 单样本混入(01 §5.2):equal-power pan(sin/cos,ADR-010)+ vol + 注入 fade。
inline void mixMonoSample(float sample, float pan, float volDb, float globalWidth, float fade, float& l,
                          float& r) noexcept
{
    if (fade <= 0.0f)
    {
        return;
    }
    float gL = 0.0f;
    float gR = 0.0f;
    panGains(scaleByGlobalWidth(pan, globalWidth), gL, gR);
    const float g = dbToLinear(volDb) * fade;
    l += sample * gL * g;
    r += sample * gR * g;
}

// stereo 单样本混入(01 §5.2 + 02 §8.1):dual-pan + 每轨 width。
// 求值链 = 名义子声像 clamp(P∓w_t) → 全局 width 几何缩放 → 每子声像独立 equal-power。
// 全局 width=100 时与 PanMath::dualPanGains(pan, trkWidth)(T14 交付)完全等价。
inline void mixStereoSample(float sL, float sR, float pan, float volDb, float trkWidth, float globalWidth, float fade,
                            float& l, float& r) noexcept
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
    const float vg = dbToLinear(volDb) * fade;
    l += (sL * gL_L + sR * gL_R) * vg;
    r += (sL * gR_L + sR * gR_R) * vg;
}

} // namespace scvb::output
