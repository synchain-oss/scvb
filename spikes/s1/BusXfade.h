// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// S1 spike —— Output 侧总线交叉淡变(busXfade,01 §5.2 R2 过渡语义)。
// 在「传入总线(各轨静音之和)」与「自有混音(15 环求和)」之间做 80–200ms 等功率交叉,
// 消除激活/退出/时间线无效边界的硬切;稳态等价完全替换(ADR-002 替换语义不变)。
// 交叉期断言:包络单调、无 >+1dB 突起、无超 ramp 时长空白(G-1b/S1-P12)。

#include <cmath>
#include <cstddef>
#include <vector>

#include "SegmentLayout.h"

namespace scvb
{

inline constexpr double kBusFadeSeconds = 0.120; // 120ms(80–200ms 区间内,J32)

class BusXfade
{
public:
    void prepare(double sampleRate, int maxBlock)
    {
        fadeSamples_ = static_cast<int>(sampleRate * kBusFadeSeconds);
        if (fadeSamples_ < 1)
        {
            fadeSamples_ = 1;
        }
        mixL_.assign(static_cast<std::size_t>(maxBlock > 0 ? maxBlock : 1), 0.0f);
        mixR_.assign(static_cast<std::size_t>(maxBlock > 0 ? maxBlock : 1), 0.0f);
        theta_ = 0.0f; // 冷启动默认透传(Output 尚未激活)
        targetActive_ = false;
        fading_ = false;
    }

    bool isActive() const { return targetActive_ && !fading_; }

    // 音频线程:目标 = 激活(自有混音)。buf 为总线 stereo(L/R),mixL/mixR 为自有混音(长度 n)。
    void render(float* const* buf, const float* mixL, const float* mixR, int n)
    {
        beginFade(true);
        float* L = buf[0];
        float* R = buf[1];
        if (!fading_)
        {
            // 稳态激活:完全替换,并缓存自有混音供退出方向交叉。
            for (int i = 0; i < n; ++i)
            {
                L[i] = mixL[i];
                R[i] = mixR[i];
                mixL_[static_cast<std::size_t>(i)] = mixL[i];
                mixR_[static_cast<std::size_t>(i)] = mixR[i];
            }
            return;
        }
        // 交叉:pass 与 mix 等功率(θ:0=透传,π/2=激活)。
        int i = 0;
        while (i < n && fading_)
        {
            const float gM = std::sin(theta_);
            const float gB = std::cos(theta_);
            L[i] = mixL[i] * gM + L[i] * gB;
            R[i] = mixR[i] * gM + R[i] * gB;
            mixL_[static_cast<std::size_t>(i)] = mixL[i];
            mixR_[static_cast<std::size_t>(i)] = mixR[i];
            ++i;
            advance();
        }
        if (!fading_)
        {
            for (int j = i; j < n; ++j)
            {
                L[j] = mixL[j];
                R[j] = mixR[j];
                mixL_[static_cast<std::size_t>(j)] = mixL[j];
                mixR_[static_cast<std::size_t>(j)] = mixR[j];
            }
        }
    }

    // 音频线程:目标 = 透传(Observer/无时间线/负 t0)。用缓存的自有混音交叉退出。
    void renderPassthrough(float* const* buf, int n)
    {
        beginFade(false);
        if (!fading_)
        {
            return; // 稳态透传:no-op
        }
        float* L = buf[0];
        float* R = buf[1];
        int i = 0;
        while (i < n && fading_)
        {
            const float gM = std::sin(theta_);
            const float gB = std::cos(theta_);
            L[i] = mixL_[static_cast<std::size_t>(i)] * gM + L[i] * gB;
            R[i] = mixR_[static_cast<std::size_t>(i)] * gM + R[i] * gB;
            ++i;
            advance();
        }
        // 余下样本已透传(θ=0 → gM=0,gB=1,buf 原样),无需处理。
    }

private:
    void beginFade(bool targetActive)
    {
        if (targetActive != targetActive_)
        {
            targetActive_ = targetActive;
            fading_ = true;
            fadePos_ = 0;
        }
    }

    void advance()
    {
        ++fadePos_;
        if (fadePos_ >= fadeSamples_)
        {
            fading_ = false;
            theta_ = targetActive_ ? static_cast<float>(kHalfPi) : 0.0f;
        }
        else
        {
            const float step = static_cast<float>(kHalfPi) / static_cast<float>(fadeSamples_);
            theta_ += targetActive_ ? step : -step;
            if (theta_ < 0.0f)
            {
                theta_ = 0.0f;
            }
            if (theta_ > static_cast<float>(kHalfPi))
            {
                theta_ = static_cast<float>(kHalfPi);
            }
        }
    }

    int fadeSamples_ = 1;
    int fadePos_ = 0;
    float theta_ = 0.0f; // 0=透传,π/2=激活
    bool targetActive_ = false;
    bool fading_ = false;
    std::vector<float> mixL_;
    std::vector<float> mixR_;
};

} // namespace scvb
