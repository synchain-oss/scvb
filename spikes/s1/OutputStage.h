// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// S1 spike —— 输出级双档(IOutputStage)+ RampSwitcher(ADR-002 v1 / J12+J32)。
//   SilenceStage    :健康连接时输出静音(buf.clear,保住 DAW 依赖图排序)
//   PassthroughStage:未检测到健康 Output 时直通(保持 buf 原样)
// 切换经 RampSwitcher 80ms 等功率交叉 ramp;5s 滞回由 Input 的 [M] 25Hz 健康仲裁施加(见 InputProcessor),
// 不在本文件(本文件只做「读模式字 + 平滑过渡」)。
// 全部操作 planar float**,与 JUCE AudioBuffer::getArrayOfWritePointers() 对齐;零堆分配。

#include <atomic>
#include <cmath>
#include <cstddef>

#include "SegmentLayout.h"

namespace scvb
{

inline constexpr double kRampSeconds = 0.080; // 80ms(ADR-010/J12)
inline constexpr u64 kHysteresisMs = 5000; // 5s 滞回(仅作用于 静音→直通 方向,J12/J32)

// 等功率交叉曲线(单信号对静音的交叉退化为对 passthrough 信号的增益 ramp)。
inline float equalPowerFadeIn(float p) // 0→1,sin 曲线
{
    return std::sin(static_cast<float>(kHalfPi) * p);
}
inline float equalPowerFadeOut(float p) // 1→0,cos 曲线
{
    return std::cos(static_cast<float>(kHalfPi) * p);
}

class IOutputStage
{
public:
    virtual ~IOutputStage() = default;
    virtual void render(float* const* channels, int numChannels, int numSamples) = 0;
};

// 健康:清空所有声道(输出静音)。
class SilenceStage final : public IOutputStage
{
public:
    void render(float* const* channels, int numChannels, int numSamples) override
    {
        for (int c = 0; c < numChannels; ++c)
        {
            float* ch = channels[c];
            for (int i = 0; i < numSamples; ++i)
            {
                ch[i] = 0.0f;
            }
        }
    }
};

// 不健康:保持 buffer 原样(源通道直通,[J57] 不下混)。
class PassthroughStage final : public IOutputStage
{
public:
    void render(float* const*, int, int) override
    {
        // 直通 = 不改写 buffer。
    }
};

// C18 模式字 atomic<u32> stage([M] 25Hz 写 / [A] 每 block 读)+ 80ms 等功率 ramp。
// 0 = SilenceStage,1 = PassthroughStage。
class RampSwitcher
{
public:
    RampSwitcher() = default;

    void prepare(double sampleRate)
    {
        rampSamples_ = static_cast<int>(sampleRate * kRampSeconds);
        if (rampSamples_ < 1)
        {
            rampSamples_ = 1;
        }
        // 冷启动默认:未确认健康 → 直通(ADR-002 v1,无健康 Output 时直通)。
        currentPassthrough_ = true;
        ramping_ = false;
    }

    // [M] 25Hz 健康仲裁写入模式字(0=静音,1=直通)。
    void setTargetPassthrough(bool passthrough) { stage_.store(passthrough ? 1u : 0u, std::memory_order_relaxed); }
    bool targetPassthrough() const { return stage_.load(std::memory_order_relaxed) != 0; }
    bool isPassthrough() const { return currentPassthrough_ && !ramping_; }

    // [A] 每 block:按模式字渲染,自动 80ms 等功率 ramp。
    void render(float* const* channels, int numChannels, int numSamples)
    {
        const bool target = (stage_.load(std::memory_order_relaxed) != 0);
        if (target != currentPassthrough_)
        {
            if (!ramping_ || rampTargetPassthrough_ != target)
            {
                ramping_ = true;
                rampTargetPassthrough_ = target;
                rampPos_ = 0;
            }
        }
        if (!ramping_)
        {
            if (!currentPassthrough_)
            {
                clear(channels, numChannels, numSamples);
            }
            return; // 稳态直通:no-op
        }

        int i = 0;
        while (i < numSamples && ramping_)
        {
            const float p = static_cast<float>(rampPos_) / static_cast<float>(rampSamples_);
            const float g = rampTargetPassthrough_ ? equalPowerFadeIn(p) : equalPowerFadeOut(p);
            for (int c = 0; c < numChannels; ++c)
            {
                channels[c][i] *= g;
            }
            ++i;
            if (++rampPos_ >= rampSamples_)
            {
                ramping_ = false;
                currentPassthrough_ = rampTargetPassthrough_;
            }
        }
        // 余下样本按稳态增益补齐(fade-in 完成 → 直通,保持原样;fade-out 完成 → 静音,清零)。
        if (!currentPassthrough_)
        {
            for (int c = 0; c < numChannels; ++c)
            {
                for (int j = i; j < numSamples; ++j)
                {
                    channels[c][j] = 0.0f;
                }
            }
        }
    }

private:
    static void clear(float* const* channels, int numChannels, int numSamples)
    {
        for (int c = 0; c < numChannels; ++c)
        {
            float* ch = channels[c];
            for (int i = 0; i < numSamples; ++i)
            {
                ch[i] = 0.0f;
            }
        }
    }

    std::atomic<u32> stage_{1}; // C18 模式字,默认直通
    int rampSamples_ = 1;
    int rampPos_ = 0;
    bool currentPassthrough_ = true;
    bool ramping_ = false;
    bool rampTargetPassthrough_ = true;
};

} // namespace scvb
