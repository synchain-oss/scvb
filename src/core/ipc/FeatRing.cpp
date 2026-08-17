// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/FeatRing.h"

#include <algorithm>

namespace scvb
{

void FeatRing::bind(FeatHeader* header, FeatFrame* ring, u32 capacity) noexcept
{
    header_ = header;
    ring_ = ring;
    capacity_ = capacity;
    bound_ = (header_ != nullptr && ring_ != nullptr && capacity_ != 0);
}

void FeatRing::prepare(double sampleRate, int channels, int maxBlockSamples)
{
    extractor_.prepare(sampleRate, channels);
    const int ch = extractor_.channels();
    shifted_.assign(static_cast<std::size_t>(ch), nullptr);
    const int maxFrames = std::max(1, maxBlockSamples / extractor_.hopSize() + 2);
    out_.assign(static_cast<std::size_t>(maxFrames), analysis::FeatFrame{});
    prepared_ = true;
    pendingSkip_ = 0;
    nextHop_ = 0;
}

void FeatRing::startRun(int64_t timelineSample, const std::function<void()>& betweenStores)
{
    if (!prepared_ || !bound_ || header_ == nullptr || ring_ == nullptr)
    {
        return;
    }

    const int64_t hopSamples = extractor_.hopSize();
    // 首个完整 hop(04 §3.2 ceilDiv)。
    const uint64_t h0 = static_cast<uint64_t>((timelineSample + hopSamples - 1) / hopSamples);

    // 顺序不可倒(J33/04 §3.2):先 base 后 write。读方若插在两 store 之间只能见
    // 「新 base + 旧 write」(前跳 seek 空循环 / 回卷幂等重拉,均安全);倒置则见
    // 「旧 base + 新 write」,前跳 seek 把从未写入的陈旧槽整段拉进 FrameStore。
    header_->base_hop.store(h0, std::memory_order_release);
    if (betweenStores)
    {
        betweenStores();
    }
    header_->write_hop.store(h0, std::memory_order_release);

    // biquad 预热:run 边界 reset + 首完整 hop 预热(T08 口径,run B 首 hop 不残留 run A 滤波态)。
    extractor_.startRun();

    const int64_t firstFullSample = static_cast<int64_t>(h0) * hopSamples;
    pendingSkip_ = (firstFullSample > timelineSample) ? (firstFullSample - timelineSample) : 0;
    nextHop_ = h0;
}

int FeatRing::processBlock(const float* const* channels, int numSamples)
{
    if (!capturing_ || !prepared_ || !bound_ || header_ == nullptr || ring_ == nullptr || numSamples <= 0)
    {
        return 0; // 采集 OFF 或未就绪:静默丢弃(release 不写段、不推进 write_hop)
    }

    int offset = 0;
    if (pendingSkip_ > 0)
    {
        offset = static_cast<int>(std::min<int64_t>(pendingSkip_, static_cast<int64_t>(numSamples)));
        pendingSkip_ -= offset;
        if (pendingSkip_ > 0)
        {
            return 0; // 整块仍是 run 起始部分 hop 的尾巴,丢弃
        }
    }

    const int n = numSamples - offset;
    const int ch = extractor_.channels();
    for (int c = 0; c < ch; ++c)
    {
        shifted_[static_cast<std::size_t>(c)] = channels[c] + offset;
    }

    const int got = extractor_.processBlock(shifted_.data(), n, out_.data(), static_cast<int>(out_.size()));

    for (int i = 0; i < got; ++i)
    {
        const uint64_t hop = nextHop_;
        ring_[hop % capacity_] =
            FeatFrame{out_[static_cast<std::size_t>(i)].kw_ms, out_[static_cast<std::size_t>(i)].peak};
        ++nextHop_;
        header_->write_hop.store(nextHop_, std::memory_order_release); // 稳态:写帧后推进 write_hop
    }

    return got;
}

uint32_t pullIncremental(const FeatHeader& header, const FeatFrame* ring, u32 capacity, FeatPullState& state,
                         analysis::ChannelFrames& store, analysis::HopRange timeGate)
{
    const uint64_t b1 = header.base_hop.load(std::memory_order_acquire);
    const uint64_t w = header.write_hop.load(std::memory_order_acquire);
    const uint64_t b2 = header.base_hop.load(std::memory_order_acquire);
    if (b1 != b2)
    {
        return 0; // run 正在切换,下拍再读(seqlock 式)
    }

    if (!state.initialized || b1 != state.lastBase)
    {
        state.lastPulled = b1; // 新 run:从 base 起
        state.lastBase = b1;
        state.initialized = true;
    }

    if (w < state.lastPulled)
    {
        return 0; // 真回退异常,本拍丢弃重读
    }

    const uint64_t start = std::max(state.lastPulled, b1);
    const uint64_t wCap = std::min(w, start + kMaxBurstHops);

    uint32_t pulled = 0;
    for (uint64_t h = start; h < wCap; ++h)
    {
        if (h >= timeGate.begin && h < timeGate.end)
        {
            const FeatFrame& f = ring[h % capacity];
            store.write(h, f.kw_ms, f.peak);
            ++pulled;
        }
    }
    state.lastPulled = wCap;
    return pulled;
}

void FeatPuller::bind(u32 channel, const FeatHeader* header, const FeatFrame* ring, u32 capacity)
{
    if (channel < 1 || channel > kMaxChannels)
    {
        return;
    }
    auto& b = bindings_[channel - 1];
    b.header = header;
    b.ring = ring;
    b.capacity = capacity;
    b.bound = (header != nullptr && ring != nullptr && capacity != 0);
}

void FeatPuller::pullTick(analysis::FrameStore& store, analysis::HopRange timeGate, u32 selectedMask, u32 activeMask)
{
    for (u32 ch = 1; ch <= kMaxChannels; ++ch)
    {
        const u32 bit = 1u << (ch - 1);
        if ((activeMask & bit) == 0)
        {
            continue; // 非在线轨(connected_mask)
        }
        if (selectedMask != 0 && (selectedMask & bit) == 0)
        {
            continue; // 布防期未选中轨(04 §4.2 步骤 2)
        }
        const auto& b = bindings_[ch - 1];
        if (!b.bound)
        {
            continue;
        }
        pullIncremental(*b.header, b.ring, b.capacity, states_[ch - 1], store.channel(ch), timeGate);
    }
}

FeatPullState& FeatPuller::state(u32 channel)
{
    const u32 idx = (channel >= 1 && channel <= kMaxChannels) ? (channel - 1) : 0;
    return states_[idx];
}

void FeatPuller::reset()
{
    bindings_.fill(Binding{});
    states_.fill(FeatPullState{});
}

} // namespace scvb
