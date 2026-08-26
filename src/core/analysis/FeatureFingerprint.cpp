// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/FeatureFingerprint.h"

#include "analysis/FrameStore.h"
#include "ipc/CtrlPlane.h"

namespace scvb::analysis
{

bool baselineTileFingerprint(const ChannelFrames& frames, std::uint32_t tileIdx, std::uint64_t& out)
{
    const std::uint64_t begin = static_cast<std::uint64_t>(tileIdx) * kFpTileHops;
    const std::uint64_t end = begin + kFpTileHops;
    // 未被完整覆盖 = 该秒没采过(或被打洞重采),没有基线可比 —— 直接不参与。
    if (!frames.coversFully(HopRange{begin, end}))
    {
        return false;
    }

    TileFingerprint fp;
    for (std::uint64_t h = begin; h < end; ++h)
    {
        fp.pushKwDbq(frames.kwDbq(h));
    }
    out = fp.value();
    return true;
}

FingerprintWatch::ChannelState* FingerprintWatch::stateOf(std::uint32_t channel)
{
    if (channel < 1 || channel > channels_.size())
    {
        return nullptr;
    }
    return &channels_[channel - 1];
}

const FingerprintWatch::ChannelState* FingerprintWatch::stateOf(std::uint32_t channel) const
{
    if (channel < 1 || channel > channels_.size())
    {
        return nullptr;
    }
    return &channels_[channel - 1];
}

void FingerprintWatch::onReport(std::uint32_t channel, std::uint64_t packedValue, const ChannelFrames& frames)
{
    ChannelState* c = stateOf(channel);
    if (c == nullptr)
    {
        return;
    }

    const std::uint32_t tile = unpackFpReportTileIdx(packedValue);
    const std::uint64_t reported = unpackFpReportHash(packedValue);

    std::uint64_t baseline = 0;
    if (!baselineTileFingerprint(frames, tile, baseline))
    {
        // 无基线:不比对、不计入分母。**也不打断滞回连续段** —— 采集覆盖本来就可能有洞,
        // 一个没采过的秒不该把「连续 3 秒失配」的判定清零。
        return;
    }

    c->checked.insert(tile);

    // 比对同样截断到低 48 位:上报侧丢掉的高 16 位基线侧也必须丢,否则恒不相等。
    const bool match = (baseline & kFpReportHashMask) == reported;
    if (match)
    {
        c->runLen = 0;
        c->hasRun = false;
        // 重采集或用户把上游改回去了:该 tile 的失配定谳撤销(只提示、可自愈)。
        c->mismatched.erase(tile);
        return;
    }

    // 相邻 tile 才算「连续」;跳播/换段重新起算。
    c->runLen = (c->hasRun && tile == c->lastMismatchTile + 1) ? (c->runLen + 1) : 1;
    c->lastMismatchTile = tile;
    c->hasRun = true;

    if (c->runLen >= kFpHysteresisTiles)
    {
        // 达到滞回门槛的那一拍把整段(含此前挂起的 kFpHysteresisTiles-1 个)一并定谳;
        // 之后每多一个 tile 只补它自己(前面的已在集合里,insert 幂等)。
        const std::uint32_t back = kFpHysteresisTiles - 1;
        const std::uint32_t first = tile >= back ? (tile - back) : 0;
        for (std::uint32_t t = first; t <= tile; ++t)
        {
            c->mismatched.insert(t);
        }
    }
}

bool FingerprintWatch::stale(std::uint32_t channel) const
{
    const ChannelState* c = stateOf(channel);
    if (c == nullptr || c->checked.empty())
    {
        return false;
    }
    // >10%(严格大于),整数比较。
    return c->mismatched.size() * 100u > c->checked.size() * kFpStalePercent;
}

std::uint32_t FingerprintWatch::tilesChecked(std::uint32_t channel) const
{
    const ChannelState* c = stateOf(channel);
    return c != nullptr ? static_cast<std::uint32_t>(c->checked.size()) : 0u;
}

std::uint32_t FingerprintWatch::tilesMismatched(std::uint32_t channel) const
{
    const ChannelState* c = stateOf(channel);
    return c != nullptr ? static_cast<std::uint32_t>(c->mismatched.size()) : 0u;
}

void FingerprintWatch::resetChannel(std::uint32_t channel)
{
    if (ChannelState* c = stateOf(channel))
    {
        *c = ChannelState{};
    }
}

void FingerprintWatch::reset()
{
    for (auto& c : channels_)
    {
        c = ChannelState{};
    }
}

} // namespace scvb::analysis
