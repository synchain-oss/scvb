// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/CurveEvaluator.h"

#include <algorithm>
#include <cmath>

namespace scvb
{

namespace
{

// §8.2 clamp 下界 20ms(固定,非配置项)。
constexpr double kMinRampSec = 0.020;
// 微小变化捷径阈值(§8.2 固定):|ΔP| < 5 且 |Δv| < 0.5 dB。
constexpr double kTinyPanDelta = 5.0;
constexpr double kTinyVolDeltaDb = 0.5;

double clampd(double x, double lo, double hi)
{
    if (x < lo)
        return lo;
    if (x > hi)
        return hi;
    return x;
}

// smoothstep:3w²−2w³,端点导数为 0。
double smoothstep(double x)
{
    const double w = clampd(x, 0.0, 1.0);
    return w * w * (3.0 - 2.0 * w);
}

} // namespace

void CurveEvaluator::build(const std::vector<CurveSegment>& segments, const TransitionConfig& config)
{
    m_segments = segments;
    m_boundaries.clear();
    m_warningCount = 0;

    const size_t n = m_segments.size();
    if (n < 2)
        return; // 0/1 段:无边界;求值恒为该段值(或 0)

    m_boundaries.reserve(n - 1);
    for (size_t i = 0; i + 1 < n; ++i)
    {
        const CurveSegment& a = m_segments[i];
        const CurveSegment& b = m_segments[i + 1];

        const double gap = b.startSec - a.endSec; // ≥ 0
        const double dPan = b.pan - a.pan;
        const double dVol = b.volDb - a.volDb;
        const double center = (gap > 0.0) ? (a.endSec + b.startSec) * 0.5 : a.endSec;

        Boundary bound;
        bound.centerSec = center;

        // 微小变化捷径:在 b 处直接跳变(不可闻,少写自动化点)。
        if (std::fabs(dPan) < kTinyPanDelta && std::fabs(dVol) < kTinyVolDeltaDb)
        {
            bound.isJump = true;
            bound.tEffSec = 0.0;
            m_boundaries.push_back(bound);
            continue;
        }

        // T_eff(§8.2)。
        double tEff;
        if (gap > 0.0)
        {
            tEff = clampd(std::min(config.transitionRampSec, 0.6 * gap), kMinRampSec, config.transitionRampSec);
        }
        else
        {
            const double tPan = std::fabs(dPan) / config.ratePanPerSec;
            const double tGain = std::fabs(dVol) / config.rateGainDbPerSec;
            tEff = clampd(1.5 * std::max(tPan, tGain), config.transitionRampSec, config.maxRampSec);
        }

        // 相邻 ramp 重叠防护:每段至多被两侧 ramp 各占 0.45·len,保证 P(t) 处处单值。
        const double leftLen = a.endSec - a.startSec;
        const double rightLen = b.endSec - b.startSec;
        tEff = std::min(tEff, std::min(config.overlapRatio * leftLen, config.overlapRatio * rightLen));

        bound.tEffSec = tEff;

        // 超限速 warning(仅 gap=0 的可闻路径;0.9·len 夹限与 6s cap 共用同一计数,cap 不静默)。
        if (gap <= 0.0)
        {
            const double peakPanRate = 1.5 * std::fabs(dPan) / tEff;
            const double peakGainRate = 1.5 * std::fabs(dVol) / tEff;
            if (peakPanRate > config.ratePanPerSec || peakGainRate > config.rateGainDbPerSec)
            {
                bound.warnLimit = true;
                ++m_warningCount;
            }
        }

        m_boundaries.push_back(bound);
    }
}

double CurveEvaluator::panAt(double tSec) const
{
    return valueAt(tSec, true);
}

double CurveEvaluator::volAt(double tSec) const
{
    return valueAt(tSec, false);
}

double CurveEvaluator::valueAt(double tSec, bool pan) const
{
    const size_t n = m_segments.size();
    if (n == 0)
        return 0.0;
    if (n == 1)
        return pan ? m_segments[0].pan : m_segments[0].volDb;

    // 首段之前回填首段值;末段之后保持末段值(§8.2)。
    if (tSec < m_segments[0].startSec)
        return pan ? m_segments[0].pan : m_segments[0].volDb;
    if (tSec >= m_segments[n - 1].endSec)
        return pan ? m_segments[n - 1].pan : m_segments[n - 1].volDb;

    const auto segValue = [&](const CurveSegment& s) { return pan ? s.pan : s.volDb; };

    // ramp 优先(非重叠,至多命中一个)。
    for (size_t i = 0; i + 1 < n; ++i)
    {
        const Boundary& b = m_boundaries[i];
        if (b.isJump)
            continue;
        const double lo = b.centerSec - b.tEffSec * 0.5;
        const double hi = b.centerSec + b.tEffSec * 0.5;
        if (tSec >= lo && tSec <= hi)
        {
            const double w = smoothstep((tSec - lo) / b.tEffSec);
            const double from = segValue(m_segments[i]);
            const double to = segValue(m_segments[i + 1]);
            return from + (to - from) * w;
        }
    }

    // 段内。
    for (size_t i = 0; i < n; ++i)
    {
        if (tSec >= m_segments[i].startSec && tSec < m_segments[i].endSec)
            return segValue(m_segments[i]);
    }

    // 段间空隙:ramp 之前保持前段值,ramp 之后(过渡已完成)取后段值;
    // 微小跳变在 b 处切到后段值。
    for (size_t i = 0; i + 1 < n; ++i)
    {
        if (tSec >= m_segments[i].endSec && tSec < m_segments[i + 1].startSec)
        {
            const Boundary& b = m_boundaries[i];
            if (b.isJump)
                return (tSec >= b.centerSec) ? segValue(m_segments[i + 1]) : segValue(m_segments[i]);
            const double lo = b.centerSec - b.tEffSec * 0.5;
            return (tSec < lo) ? segValue(m_segments[i]) : segValue(m_segments[i + 1]);
        }
    }

    return segValue(m_segments[n - 1]); // 防御性兜底
}

} // namespace scvb
