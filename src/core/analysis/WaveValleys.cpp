// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/WaveValleys.h"

#include <algorithm>

#include "analysis/EnergyVad.h" // frameLoudnessDb:ℓ 的唯一口径(与流水线 envDb 同一条式子)

namespace scvb::analysis
{

std::vector<double> snapValleysSeconds(const ChannelFrames& frames, double startS, double endS, double hopS,
                                       const SegmentationParams& p, std::size_t maxCount)
{
    std::vector<double> out;
    if (!(hopS > 0.0) || !(endS > startS) || maxCount == 0)
    {
        return out;
    }

    const auto toHop = [hopS](double s) {
        const double h = s / hopS;
        return h <= 0.0 ? std::uint64_t{0} : static_cast<std::uint64_t>(h);
    };
    const std::uint64_t h0 = toHop(startS);
    const std::uint64_t h1 = std::max(toHop(endS) + 1, h0 + 1);
    const std::uint64_t c0 = h0 > kSnapValleyContextHops ? h0 - kSnapValleyContextHops : 0;
    const std::uint64_t c1 = h1 + kSnapValleyContextHops;

    // 先与覆盖区求交:代价与「实际有多少数据」同阶,与请求跨度无关(waveformOf 的 P0-A 同款)。
    const std::vector<HopRange> runs = frames.coverage().intersect(HopRange{c0, c1});
    std::uint64_t total = 0;
    for (const auto& r : runs)
    {
        total += r.end - r.begin;
    }
    if (total == 0 || total > kSnapValleyMaxScanHops)
    {
        return out;
    }

    struct Found
    {
        std::uint64_t hop;
        double depthDb;
        double t; // 谷底 hop 的中心时刻 (hop+0.5)·hopS —— 窗内判定与回包用的是同一个数
    };
    std::vector<Found> found;
    std::vector<std::int16_t> kw;
    std::vector<std::int16_t> peak;
    std::vector<std::uint8_t> vad;
    std::vector<float> l;
    for (const auto& r : runs)
    {
        kw.clear();
        peak.clear();
        vad.clear();
        frames.appendRange(r, kw, peak, vad); // 按页取,每 4096 hop 才查一次索引
        l.resize(kw.size());
        for (std::size_t i = 0; i < kw.size(); ++i)
        {
            // 与 AnalysisPipeline 的 envDb 逐字同一条路:kwDbq → dequantizeKwMs(= ChannelFrames::kwMs)
            // → frameLoudnessDb。
            l[i] = frameLoudnessDb(static_cast<double>(dequantizeKwMs(kw[i])));
        }
        const auto vs = detectSnapValleys(l.data(), 0, static_cast<std::int64_t>(l.size()), p);
        for (const auto& v : vs)
        {
            const std::uint64_t hop = r.begin + static_cast<std::uint64_t>(v.hop);
            const double t = (static_cast<double>(hop) + 0.5) * hopS;
            if (t >= startS && t < endS)
            {
                found.push_back(Found{hop, v.depthDb, t});
            }
        }
    }

    if (found.size() > maxCount)
    {
        // 留最深的 maxCount 个(同深取早的),再按时间排回升序。
        std::stable_sort(found.begin(), found.end(),
                         [](const Found& a, const Found& b) { return a.depthDb > b.depthDb; });
        found.resize(maxCount);
        std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.hop < b.hop; });
    }

    out.reserve(found.size());
    for (const auto& f : found)
    {
        out.push_back(f.t);
    }
    return out;
}

} // namespace scvb::analysis
