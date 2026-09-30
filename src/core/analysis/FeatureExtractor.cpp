// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/FeatureExtractor.h"

#include <algorithm>
#include <cmath>

namespace scvb::analysis
{

void FeatureExtractor::prepare(double sampleRate, int channels)
{
    if (channels < 1)
        channels = 1;

    sampleRate_ = sampleRate;
    channels_ = channels;
    hopSize_ = static_cast<int>(std::llround(sampleRate / 100.0));
    if (hopSize_ < 1)
        hopSize_ = 1;

    kw_.assign(static_cast<std::size_t>(channels_), dsp::KWeighting{});
    for (auto& k : kw_)
        k.prepare(sampleRate);

    sumSq_.assign(static_cast<std::size_t>(channels_), 0.0);
    preheatBuf_.assign(static_cast<std::size_t>(hopSize_) * static_cast<std::size_t>(channels_), 0.0f);

    startRun();
}

void FeatureExtractor::startRun()
{
    for (auto& k : kw_)
        k.reset();

    resetAccumulator();
    preheatFill_ = 0;
    preheating_ = true;
}

void FeatureExtractor::resetAccumulator()
{
    std::fill(sumSq_.begin(), sumSq_.end(), 0.0);
    peak_ = 0.0f;
    count_ = 0;
}

FeatFrame FeatureExtractor::makeFrame() const
{
    // 每通道独立算 mean-square(与 mono 口径逐位一致)后再按能量域相加,
    // 使 stereo 结果与「两路分别算再相加」逐位一致(02-dsp-spec §1.5 FEAT-4)。
    float kw = 0.0f;
    for (int c = 0; c < channels_; ++c)
        kw += static_cast<float>(sumSq_[static_cast<std::size_t>(c)] / static_cast<double>(hopSize_));

    return FeatFrame{kw, peak_};
}

int FeatureExtractor::processBlock(const float* const* channels, int numSamples, FeatFrame* out, int maxFrames)
{
    int emitted = 0;
    int n = 0;

    while (n < numSamples)
    {
        if (preheating_)
        {
            // 缓冲 run 首个完整 hop 的样本(可跨块)。
            while (n < numSamples && preheatFill_ < hopSize_)
            {
                for (int c = 0; c < channels_; ++c)
                {
                    preheatBuf_[static_cast<std::size_t>(c) * static_cast<std::size_t>(hopSize_) +
                                static_cast<std::size_t>(preheatFill_)] = channels[c][n];
                }
                ++preheatFill_;
                ++n;
            }
            if (preheatFill_ < hopSize_)
                break;

            // 暖机:整体过一遍滤波器并丢弃输出。
            for (int s = 0; s < hopSize_; ++s)
            {
                for (int c = 0; c < channels_; ++c)
                {
                    (void)kw_[static_cast<std::size_t>(c)].process(
                        preheatBuf_[static_cast<std::size_t>(c) * static_cast<std::size_t>(hopSize_) +
                                    static_cast<std::size_t>(s)]);
                }
            }

            // 重置累加器后正式处理同一 hop(h0 即本 run 的首个输出 hop)。
            resetAccumulator();
            for (int s = 0; s < hopSize_; ++s)
            {
                for (int c = 0; c < channels_; ++c)
                {
                    const float x = preheatBuf_[static_cast<std::size_t>(c) * static_cast<std::size_t>(hopSize_) +
                                                static_cast<std::size_t>(s)];
                    const float y = kw_[static_cast<std::size_t>(c)].process(x);
                    sumSq_[static_cast<std::size_t>(c)] += static_cast<double>(y) * static_cast<double>(y);

                    const float ax = std::fabs(x);
                    if (ax > peak_)
                        peak_ = ax;
                }
            }

            if (out != nullptr && emitted < maxFrames)
                out[emitted] = makeFrame();
            ++emitted;

            resetAccumulator(); // h0 已发布,清空累加器供后续 hop 使用
            preheatFill_ = 0;
            preheating_ = false;
            continue;
        }

        // 常规逐样本处理:逐样本对各通道 K-weighting,能量按通道独立累加。
        for (int c = 0; c < channels_; ++c)
        {
            const float x = channels[c][n];
            const float y = kw_[static_cast<std::size_t>(c)].process(x);
            sumSq_[static_cast<std::size_t>(c)] += static_cast<double>(y) * static_cast<double>(y);

            const float ax = std::fabs(x);
            if (ax > peak_)
                peak_ = ax;
        }
        ++count_;
        ++n;

        if (count_ == hopSize_)
        {
            if (out != nullptr && emitted < maxFrames)
                out[emitted] = makeFrame();
            ++emitted;
            resetAccumulator();
        }
    }

    return emitted;
}

} // namespace scvb::analysis
