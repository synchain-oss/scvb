// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/VadPreview.h"

#include "analysis/AnalysisPipeline.h" // envelopeDbInto / splitLongVadSegments(与流水线同一份)

namespace scvb::analysis
{

VadPreviewTrackCache buildVadPreviewTrackCache(const float* kwMs, std::size_t n)
{
    VadPreviewTrackCache c;
    if (kwMs == nullptr || n == 0)
    {
        return c;
    }
    c.base = computeVadBaseline(kwMs, n);
    envelopeDbInto(kwMs, n, c.envDb);
    return c;
}

std::vector<VadSegment> runVadPreviewTrack(VadPreviewTrackCache& c, std::int64_t firstHop, const VadParams& vad,
                                           const SegmentationParams& seg, double hopSec, float* posteriorOut)
{
    if (c.hops() == 0)
    {
        return {};
    }
    const VadResult r = runEnergyVadOnBaseline(c.base, firstHop, vad, posteriorOut);
    // kwMs 传 nullptr:包络已在缓存里建好(envDb 非空),helper 不会再去读原始 kw。
    return splitLongVadSegments(r.segments, firstHop, seg, hopSec, nullptr, c.hops(), c.envDb, nullptr);
}

} // namespace scvb::analysis
