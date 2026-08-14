// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>

// PanMath:全项目唯一 pan 增益来源(ADR-010)。纯 C++17,不依赖 JUCE。
// equal-power pan + [J57] dual-pan+width;明令不用 M/S 拉宽(避极性反转)。
namespace scvb
{

// clamp 到 pan 值域 [−100, +100]
inline float clampPan(float pan)
{
    if (pan < -100.0f)
        return -100.0f;
    if (pan > 100.0f)
        return 100.0f;
    return pan;
}

// equal-power pan:θ = (P+100)·π/400 ∈ [0, π/2];gL = cos θ,gR = sin θ。
// gL² + gR² = 1(中心 −3.01 dB)。P ∈ [−100, +100]。
inline void panGains(float pan, float& gainLeft, float& gainRight)
{
    constexpr double kPiOver400 = 3.14159265358979323846 / 400.0;
    const double theta = (static_cast<double>(pan) + 100.0) * kPiOver400;
    gainLeft = static_cast<float>(std::cos(theta));
    gainRight = static_cast<float>(std::sin(theta));
}

// [J57] stereo 源双声像增益:L 源走一次 equal-power、R 源走一次 equal-power。
struct DualPanGains
{
    float gL_L = 0.0f;
    float gR_L = 0.0f;
    float gL_R = 0.0f;
    float gR_R = 0.0f;
};

// dual-pan + width:panCenter = 弧中心,width = 每轨张开度 ∈ [0,100]。
// 名义子声像 P_L = clamp(panCenter − width)、P_R = clamp(panCenter + width);
// width=0 → 双子声像重合(塌成 mono),width=100 → 源宽度原样。
// 注意:每轨 width(此处)与全局 width(scaleByGlobalWidth)是两个量,不得合并(J57)。
inline DualPanGains dualPanGains(float panCenter, float width)
{
    const float subPanLeft = clampPan(panCenter - width);
    const float subPanRight = clampPan(panCenter + width);

    DualPanGains g;
    panGains(subPanLeft, g.gL_L, g.gR_L);
    panGains(subPanRight, g.gL_R, g.gR_R);
    return g;
}

// 全局 width 几何缩放(02 §8.1 步骤 4,ADR-010):P_eff = clamp(P_nom · width/100, −100, +100)。
// 缩放的是「分配角」(几何角度);与每轨 width(dualPanGains 的张开度)正交,不可合并。
inline float scaleByGlobalWidth(float panNominal, float globalWidthPercent)
{
    return clampPan(panNominal * (globalWidthPercent / 100.0f));
}

} // namespace scvb
