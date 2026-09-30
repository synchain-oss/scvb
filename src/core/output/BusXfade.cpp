// SPDX-License-Identifier: GPL-3.0-or-later
#include "output/BusXfade.h"

#include <algorithm>
#include <cmath>

namespace scvb::output
{

void BusXfade::prepare(double sampleRate, double rampMs)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double ms = rampMs > 0.0 ? rampMs : kDefaultRampMs;
    rampSamples_ = std::max(1.0, sampleRate_ * ms / 1000.0);
    theta_ = targetMix_ ? kHalfPi : 0.0;
}

void BusXfade::render(float* const* out, const float* const* input, const float* const* mix, int n, bool targetMix)
{
    if (n <= 0 || out == nullptr || input == nullptr || mix == nullptr)
    {
        return;
    }
    targetMix_ = targetMix;
    const double step = kHalfPi / rampSamples_;
    for (int i = 0; i < n; ++i)
    {
        if (!targetMix_ && theta_ <= 0.0)
        {
            return; // 稳态直通:余下样本保持原样(零开销,输出==输入)
        }
        if (targetMix_ && theta_ >= kHalfPi)
        {
            // 稳态替换:余下样本直接拷贝自有混音。
            for (int c = 0; c < 2; ++c)
            {
                for (int j = i; j < n; ++j)
                {
                    out[c][j] = mix[c][j];
                }
            }
            return;
        }
        const float gPassthrough = static_cast<float>(std::cos(theta_));
        const float gMix = static_cast<float>(std::sin(theta_));
        for (int c = 0; c < 2; ++c)
        {
            out[c][i] = input[c][i] * gPassthrough + mix[c][i] * gMix;
        }
        if (targetMix_)
        {
            theta_ = std::min(kHalfPi, theta_ + step);
        }
        else
        {
            theta_ = std::max(0.0, theta_ - step);
        }
    }
}

} // namespace scvb::output
