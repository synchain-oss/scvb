// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/LoudnessMode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scvb::analysis
{

namespace
{
constexpr double kLufsOffset = -0.691;
constexpr double kEnergyFloor = 1e-12;
constexpr double kAmplitudeFloor = 1e-6;
} // namespace

const char* loudnessModeToString(LoudnessMode mode)
{
    switch (mode)
    {
    case LoudnessMode::KIntegrated:
        return "k_integrated";
    case LoudnessMode::Rms:
        return "rms";
    case LoudnessMode::PeakDbfs:
        return "peak_dbfs";
    }
    return "k_integrated";
}

LoudnessModeParseResult parseLoudnessMode(const char* value)
{
    LoudnessModeParseResult r;
    if (value == nullptr || value[0] == '\0')
    {
        r.fellBack = true;
        r.warning = "analysis.loudness_mode: 空值回落默认 k_integrated";
        return r;
    }
    if (std::strcmp(value, "k_integrated") == 0)
        return r;
    if (std::strcmp(value, "rms") == 0)
    {
        r.mode = LoudnessMode::Rms;
        return r;
    }
    if (std::strcmp(value, "peak_dbfs") == 0)
    {
        r.mode = LoudnessMode::PeakDbfs;
        return r;
    }
    r.fellBack = true;
    r.warning = "analysis.loudness_mode: 未知值回落默认 k_integrated";
    return r;
}

LoudnessModeParseResult parseLoudnessMode(int ordinal)
{
    LoudnessModeParseResult r;
    switch (ordinal)
    {
    case 0:
        return r;
    case 1:
        r.mode = LoudnessMode::Rms;
        return r;
    case 2:
        r.mode = LoudnessMode::PeakDbfs;
        return r;
    default:
        r.fellBack = true;
        r.warning = "analysis.loudness_mode: 越界序号回落默认 k_integrated";
        return r;
    }
}

double computeLseg(const float* kwMs, std::size_t n)
{
    if (n == 0)
        return kLufsOffset + 10.0 * std::log10(kEnergyFloor);

    double sum = 0.0;
    for (std::size_t k = 0; k < n; ++k)
        sum += static_cast<double>(kwMs[k]);
    double z = sum / static_cast<double>(n);
    if (z < kEnergyFloor)
        z = kEnergyFloor;
    return kLufsOffset + 10.0 * std::log10(z);
}

double computeLoudnessMetric(LoudnessMode mode, const float* kwMs, const float* peak, std::size_t n)
{
    switch (mode)
    {
    case LoudnessMode::KIntegrated:
        return computeLseg(kwMs, n);
    case LoudnessMode::Rms: {
        if (n == 0)
            return 20.0 * std::log10(kAmplitudeFloor);
        double sumAmp = 0.0;
        for (std::size_t k = 0; k < n; ++k)
            sumAmp += std::sqrt(static_cast<double>(kwMs[k])); // kw_ms ≥ 0
        double meanAmp = sumAmp / static_cast<double>(n);
        if (meanAmp < kAmplitudeFloor)
            meanAmp = kAmplitudeFloor;
        return 20.0 * std::log10(meanAmp);
    }
    case LoudnessMode::PeakDbfs: {
        double maxPeak = 0.0;
        if (n > 0 && peak != nullptr)
        {
            for (std::size_t k = 0; k < n; ++k)
                maxPeak = std::max(maxPeak, static_cast<double>(peak[k]));
        }
        if (maxPeak < kAmplitudeFloor)
            maxPeak = kAmplitudeFloor;
        return 20.0 * std::log10(maxPeak);
    }
    }
    return computeLseg(kwMs, n);
}

SegmentLoudness computeSegmentLoudness(const AnalysisSettingsStale& settings, const float* kwMs, const float* peak,
                                       std::size_t n)
{
    SegmentLoudness r;
    r.lseg = computeLseg(kwMs, n);
    r.lmode = computeLoudnessMetric(settings.loudnessMode, kwMs, peak, n);
    return r;
}

} // namespace scvb::analysis
