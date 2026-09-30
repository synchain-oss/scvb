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

bool FingerprintWatch::ChannelState::test(const TileBitmap& b, std::uint32_t tile)
{
    const std::size_t w = tile / 64u;
    return w < b.size() && (b[w] & (1ull << (tile % 64u))) != 0;
}

bool FingerprintWatch::ChannelState::set(TileBitmap& b, std::uint32_t tile)
{
    if (b.empty())
    {
        b.assign(kTileBitmapWords, 0ull); // 惰性分配:第一次真用到才占那 8KB
    }
    const std::size_t w = tile / 64u;
    if (w >= b.size())
    {
        return false; // tile 越 16 位上限:打包侧本就不会发,读侧再兜一道
    }
    const std::uint64_t bit = 1ull << (tile % 64u);
    if ((b[w] & bit) != 0)
    {
        return false;
    }
    b[w] |= bit;
    return true;
}

bool FingerprintWatch::ChannelState::clear(TileBitmap& b, std::uint32_t tile)
{
    const std::size_t w = tile / 64u;
    if (w >= b.size())
    {
        return false;
    }
    const std::uint64_t bit = 1ull << (tile % 64u);
    if ((b[w] & bit) == 0)
    {
        return false;
    }
    b[w] &= ~bit;
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
        // 无基线:不比对、不计入分母、也不定谳,并且**打断滞回连续段** —— 判据是「连续
        // 3 秒不匹配」,中间隔着一个没采过的秒就不是连续的三秒。这是保守的一侧:覆盖有洞时
        // 宁可不提示,也不要靠拼接出来的「连续」去下一个可能错的结论。
        c->runLen = 0;
        return;
    }

    if (ChannelState::set(c->checked, tile))
    {
        ++c->checkedCount;
    }

    // 比对同样截断到低 48 位:上报侧丢掉的高 16 位基线侧也必须丢,否则恒不相等。
    const bool match = (baseline & kFpReportHashMask) == reported;
    if (match)
    {
        c->runLen = 0; // 一条匹配就断段 —— 暖机误报正是「一条失配后面紧跟匹配」的形状
        // 重采集或用户把上游改回去了:该 tile 的失配定谳撤销(只提示、可自愈)。
        if (ChannelState::clear(c->mismatched, tile))
        {
            --c->mismatchedCount;
        }
        return;
    }

    // 连续段推进:记下本条的 tile 号(环形覆写,只留最近 kFpHysteresisTiles 条)。
    c->pending[c->runLen % kFpHysteresisTiles] = tile;
    ++c->runLen;

    if (c->runLen >= kFpHysteresisTiles)
    {
        // 达到门槛的那一拍把整段(含此前挂起的 kFpHysteresisTiles-1 条)一并定谳;
        // 之后每多一条只有环里最新那格是新的,其余早已置位(set 幂等)。
        for (std::uint32_t i = 0; i < kFpHysteresisTiles; ++i)
        {
            if (ChannelState::set(c->mismatched, c->pending[i]))
            {
                ++c->mismatchedCount;
            }
        }
    }
}

bool FingerprintWatch::stale(std::uint32_t channel) const
{
    const ChannelState* c = stateOf(channel);
    if (c == nullptr || c->checkedCount == 0)
    {
        return false;
    }
    // >10%(严格大于),整数比较(u64 相乘,分子最大 65536 也不会溢出)。
    return static_cast<std::uint64_t>(c->mismatchedCount) * 100u >
           static_cast<std::uint64_t>(c->checkedCount) * kFpStalePercent;
}

std::uint32_t FingerprintWatch::tilesChecked(std::uint32_t channel) const
{
    const ChannelState* c = stateOf(channel);
    return c != nullptr ? c->checkedCount : 0u;
}

std::uint32_t FingerprintWatch::tilesMismatched(std::uint32_t channel) const
{
    const ChannelState* c = stateOf(channel);
    return c != nullptr ? c->mismatchedCount : 0u;
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
