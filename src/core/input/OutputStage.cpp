// SPDX-License-Identifier: GPL-3.0-or-later
#include "input/OutputStage.h"

#include <algorithm>
#include <cmath>

namespace scvb::input
{

namespace
{
constexpr double kHalfPi = 1.57079632679489661923;
}

void SilenceStage::render(float* const* ch, int numChannels, int numSamples)
{
    for (int c = 0; c < numChannels; ++c)
    {
        if (ch[c] != nullptr)
        {
            std::fill(ch[c], ch[c] + numSamples, 0.0f);
        }
    }
}

void RampSwitcher::prepare(double sampleRate, double rampMs)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double ms = rampMs > 0.0 ? rampMs : kStageRampMs;
    rampSamples_ = std::max(1.0, sampleRate_ * ms / 1000.0);
    theta_ = (target_ == OutputStageMode::kPassthrough) ? 0.0 : kHalfPi;
}

void RampSwitcher::render(float* const* ch, int numChannels, int numSamples, OutputStageMode target)
{
    if (numSamples <= 0)
    {
        return;
    }
    target_ = target;
    const double step = kHalfPi / rampSamples_;
    for (int i = 0; i < numSamples; ++i)
    {
        if (target_ == OutputStageMode::kPassthrough && theta_ <= 0.0)
        {
            return; // 稳态直通:余下样本保持原样(输出==输入,逐样本一致)
        }
        if (target_ == OutputStageMode::kSilence && theta_ >= kHalfPi)
        {
            // 稳态静音:余下样本清零。
            for (int c = 0; c < numChannels; ++c)
            {
                if (ch[c] != nullptr)
                {
                    for (int j = i; j < numSamples; ++j)
                    {
                        ch[c][j] = 0.0f;
                    }
                }
            }
            return;
        }
        // ramp 中:等功率交叉,输出 = 源 × cos(θ)(静音源为零)。
        const float gain = static_cast<float>(std::cos(theta_));
        for (int c = 0; c < numChannels; ++c)
        {
            if (ch[c] != nullptr)
            {
                ch[c][i] *= gain;
            }
        }
        if (target_ == OutputStageMode::kSilence)
        {
            theta_ = std::min(kHalfPi, theta_ + step);
        }
        else
        {
            theta_ = std::max(0.0, theta_ - step);
        }
    }
}

StageSwitchStateMachine::StageSwitchStateMachine(double hysteresisMs)
    : hysteresisMs_(hysteresisMs > 0.0 ? hysteresisMs : kPassthroughHysteresisMs)
{
}

OutputStageMode StageSwitchStateMachine::evaluate(bool healthy, std::uint64_t nowMs) noexcept
{
    if (healthy)
    {
        // 直通→静音:确认健康后立即(无滞回,J32)。
        target_ = OutputStageMode::kSilence;
        haveUnhealthySince_ = false;
        unhealthySinceMs_ = 0;
        return target_;
    }

    if (target_ == OutputStageMode::kSilence)
    {
        // 静音→直通:5s 滞回只作用于这个方向(J12/J32)。
        if (!haveUnhealthySince_)
        {
            haveUnhealthySince_ = true;
            unhealthySinceMs_ = nowMs;
        }
        const std::uint64_t elapsed = (nowMs >= unhealthySinceMs_) ? (nowMs - unhealthySinceMs_) : 0;
        if (elapsed >= static_cast<std::uint64_t>(hysteresisMs_))
        {
            target_ = OutputStageMode::kPassthrough;
        }
    }
    // target_ == passthrough:保持直通(不健康时不再评估)。
    return target_;
}

void StageSwitchStateMachine::forcePassthrough() noexcept
{
    target_ = OutputStageMode::kPassthrough;
    haveUnhealthySince_ = false;
    unhealthySinceMs_ = 0;
}

} // namespace scvb::input
