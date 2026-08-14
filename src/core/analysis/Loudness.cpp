// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/Loudness.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace scvb::analysis
{

namespace
{
constexpr double kLoudnessOffset = -0.691; // BS.1770(02-dsp-spec §0.2/§4.1)
constexpr double kEnergyFloor = 1e-12; // 能量域下限(≈−120 LUFS)
constexpr double kAmplitudeFloor = 1e-6; // 幅度域下限(rms / peak_dbfs,≈−120 dBFS)

double energyToLoudness(double z)
{
    return kLoudnessOffset + 10.0 * std::log10(std::max(z, kEnergyFloor));
}

double amplitudeToLevel(double a)
{
    return 20.0 * std::log10(std::max(a, kAmplitudeFloor));
}

// 空段兜底:主指标落到各自 floor,有限、无 NaN/Inf(LOUD-2 语义)。
LoudnessValue silentValue(LoudnessMode mode)
{
    LoudnessValue out;
    switch (mode)
    {
    case LoudnessMode::kIntegrated:
        out.z = 0.0;
        out.level = energyToLoudness(0.0);
        break;
    case LoudnessMode::kRms:
        out.level = amplitudeToLevel(0.0);
        out.z = std::pow(10.0, out.level / 10.0);
        break;
    case LoudnessMode::kPeakDbfs:
        out.level = amplitudeToLevel(0.0);
        out.z = std::pow(10.0, out.level / 10.0);
        break;
    }
    return out;
}
} // namespace

LoudnessValue segmentLoudness(const float* kwMs, const float* peak, int numHops, LoudnessMode mode)
{
    if (numHops <= 0)
        return silentValue(mode);

    LoudnessValue out;

    switch (mode)
    {
    case LoudnessMode::kIntegrated: {
        // 线性能量域:K-weighted mean-square 的段内算术平均(ungated,窗长=段长)。
        double sum = 0.0;
        for (int k = 0; k < numHops; ++k)
            sum += static_cast<double>(kwMs[k]);
        out.z = sum / static_cast<double>(numHops);
        out.level = energyToLoudness(out.z);
        break;
    }
    case LoudnessMode::kRms: {
        // 幅度域算术平均(逐 hop RMS 幅度取均值,仍带 K 加权)。
        double sum = 0.0;
        for (int k = 0; k < numHops; ++k)
            sum += std::sqrt(static_cast<double>(kwMs[k]));
        const double meanRms = sum / static_cast<double>(numHops);
        out.level = amplitudeToLevel(meanRms);
        out.z = std::pow(10.0, out.level / 10.0);
        break;
    }
    case LoudnessMode::kPeakDbfs: {
        double pk = 0.0;
        for (int k = 0; k < numHops; ++k)
            pk = std::max(pk, static_cast<double>(peak[k]));
        out.level = amplitudeToLevel(pk);
        out.z = std::pow(10.0, out.level / 10.0);
        break;
    }
    }

    return out;
}

SegmentLoudness measureSegment(const float* kwMs, const float* peak, int numHops, LoudnessMode mode)
{
    SegmentLoudness out;
    out.main = segmentLoudness(kwMs, peak, numHops, mode);

    if (numHops <= 0)
        return out;

    // peakMax:段内未加权样本峰值最大值(特征域,max_k peak[k])。
    double pk = 0.0;
    for (int k = 0; k < numHops; ++k)
        pk = std::max(pk, static_cast<double>(peak[k]));
    out.peakMax = pk;

    // momentary:400ms=40hop 滑窗 K 加权响度(恒 K 加权口径,不随 mode 变化)。
    // 段长不足 400ms 时窗口退化为整段(窗长 = min(40, N))。
    const int window = std::min(kMomentaryWindowHops, numHops);
    const int numWindows = numHops - window + 1;

    double windowSum = 0.0;
    for (int k = 0; k < window; ++k)
        windowSum += static_cast<double>(kwMs[k]);

    std::vector<double> levels(static_cast<std::size_t>(numWindows));
    double maxLevel = -1e300;
    double minLevel = 1e300;
    for (int i = 0; i < numWindows; ++i)
    {
        if (i > 0)
        {
            windowSum -= static_cast<double>(kwMs[i - 1]);
            windowSum += static_cast<double>(kwMs[i - 1 + window]);
        }
        const double mean = windowSum / static_cast<double>(window);
        const double level = energyToLoudness(mean);
        levels[static_cast<std::size_t>(i)] = level;
        maxLevel = std::max(maxLevel, level);
        minLevel = std::min(minLevel, level);
    }

    out.momentary.max = maxLevel;
    out.momentary.min = minLevel;
    out.momentary.p95 = maxLevel;

    // p95:对滑窗响度排序后线性插值(确定性;n==1 时即该唯一值)。
    std::sort(levels.begin(), levels.end());
    const double pos = 0.95 * static_cast<double>(levels.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(pos));
    const auto hi = static_cast<std::size_t>(std::ceil(pos));
    out.momentary.p95 = levels[lo] + (pos - static_cast<double>(lo)) * (levels[hi] - levels[lo]);

    return out;
}

} // namespace scvb::analysis
