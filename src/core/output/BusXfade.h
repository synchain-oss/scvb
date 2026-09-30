// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// BusXfade —— 总线级「传入总线 ⇄ 自有混音」等功率交叉淡变(01 §5.2 过渡语义 R2)。
// 单一 ramp 状态机:θ=0 = 直通(输出=传入总线),θ=π/2 = 替换(输出=自有混音)。
// 稳态零开销(直通不写、替换直接拷贝);交叉期 cos²+sin²=1 等功率。
// 所有「混音⇄直通」的总线内容切换都经本状态机,无任何硬切路径(§5.2 R2/R3)。

namespace scvb::output
{

class BusXfade
{
public:
    static constexpr double kDefaultRampMs = 80.0;

    BusXfade() = default;

    // 采样率与 ramp 时长(可配,默认 80ms)。
    void prepare(double sampleRate, double rampMs = kDefaultRampMs);

    // 音频线程逐块渲染。out 与 input 可别名(逐样本读后写);mix 为自有混音(L/R)。
    // targetMix:true → 目标=替换(混音);false → 目标=直通。
    void render(float* const* out, const float* const* input, const float* const* mix, int n, bool targetMix);

    // 稳态判定。
    bool isSettledMix() const noexcept { return targetMix_ && theta_ >= kHalfPi; }
    bool isSettledPassthrough() const noexcept { return !targetMix_ && theta_ <= 0.0; }

    // 硬置位到直通稳态(首块/重初始化)。
    void resetToPassthrough() noexcept
    {
        targetMix_ = false;
        theta_ = 0.0;
    }

private:
    static constexpr double kHalfPi = 1.57079632679489661923;

    double sampleRate_ = 48000.0;
    double rampSamples_ = 3840.0; // 80ms @48k
    bool targetMix_ = false;
    double theta_ = 0.0; // 0=直通,π/2=替换
};

} // namespace scvb::output
