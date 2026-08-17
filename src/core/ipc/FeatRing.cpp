// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/FeatRing.h"

#include <algorithm>

namespace scvb
{

void FeatRing::bind(FeatHeader* header, FeatFrame* ring, u32 mappedCapacity) noexcept
{
    header_ = header;
    ring_ = ring;
    // 【几何快照纪律(04 §3 / PR#36)】hop_ms/capacity_hops 是 plain u32,attach 时一次性读进本地,
    // 此后每拍只读快照绝不回读段头;特征段恒按固定容量创建、无运行期扩容。mappedCapacity 必须 >=
    // 声明的 capacity_hops,否则拒绝绑定,保住「几何 ≤ 实际映射」不变式(v1..v5 越界类事故)。
    const bool magicOk = (header_ != nullptr) && header_->magic.load(std::memory_order_acquire) == kScvbMagic;
    const bool abiOk = (header_ != nullptr) && header_->abi.load(std::memory_order_acquire) == kScvbAbi;
    const u32 declared = (header_ != nullptr) ? header_->capacity_hops : 0;
    capacityHops_ = declared;
    hopMs_ = (header_ != nullptr) ? header_->hop_ms : 0;
    bound_ = (magicOk && abiOk && ring_ != nullptr && declared != 0 && mappedCapacity >= declared);
}

void FeatRing::prepare(double sampleRate, int channels, int maxBlockSamples)
{
    extractor_.prepare(sampleRate, channels);
    maxBlockSamples_ = std::max(1, maxBlockSamples);
    const int ch = extractor_.channels();
    shifted_.assign(static_cast<std::size_t>(ch), nullptr);
    const int maxFrames = std::max(1, maxBlockSamples_ / extractor_.hopSize() + 2);
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

    int consumed = 0;
    if (pendingSkip_ > 0)
    {
        const int skip = static_cast<int>(std::min<int64_t>(pendingSkip_, static_cast<int64_t>(numSamples)));
        pendingSkip_ -= skip;
        consumed = skip;
        if (pendingSkip_ > 0)
        {
            return 0; // 整块仍是 run 起始部分 hop 的尾巴,丢弃
        }
    }

    // 大块按 maxBlockSamples_ 分段喂 extractor:out_ 只按 maxBlockSamples_/hopSize+2 分配,
    // 宿主离线渲染/bounce 时块长可超 maximumExpectedSamplesPerBlock,一次性喂会越界读 out_(PR#42 红旗)。
    const int ch = extractor_.channels();
    int written = 0;
    int remaining = numSamples - consumed;
    while (remaining > 0)
    {
        const int chunk = std::min(remaining, maxBlockSamples_);
        for (int c = 0; c < ch; ++c)
        {
            shifted_[static_cast<std::size_t>(c)] = channels[c] + consumed;
        }

        const int got = extractor_.processBlock(shifted_.data(), chunk, out_.data(), static_cast<int>(out_.size()));
        // 兜底 clamp:正常分段下 got <= out_.size();防御性钳制,杜绝任何越界读。
        const int nFrames = std::min(got, static_cast<int>(out_.size()));
        for (int i = 0; i < nFrames; ++i)
        {
            const uint64_t hop = nextHop_;
            ring_[hop % capacityHops_] =
                FeatFrame{out_[static_cast<std::size_t>(i)].kw_ms, out_[static_cast<std::size_t>(i)].peak};
            ++nextHop_;
            header_->write_hop.store(nextHop_, std::memory_order_release); // 稳态:写帧后推进 write_hop
        }
        written += nFrames;
        consumed += chunk;
        remaining -= chunk;
    }

    return written;
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
        // run 切换(含首次):重置后本拍 return 0(确认拍,下拍再读)。若 run 切换(回卷 seek)落在
        // base.store 与 write.store 之间,读方见「新 base + 旧 write」,直接拉会把旧 run 槽按新 run
        // 记账 —— 确认拍避免(J33/PR#42)。
        state.lastPulled = b1;
        state.lastBase = b1;
        state.initialized = true;
        return 0;
    }

    if (w < state.lastPulled)
    {
        // 真回退(含同 base 重开:stop→rewind→start 时 base 不变):重置到 b1,不得保留陈旧 lastPulled,
        // 否则新 run 前缀 [b1, 旧lastPulled) 会被跳过不拉(PR#42)。
        state.lastPulled = b1;
        return 0;
    }

    // 环回绕守卫:读方落后超过容量(停滞 > capacity hop),则 [lastPulled, w-capacity) 的槽已被写方
    // 覆盖(h%capacity 已装下后 hop 的数据),继续拉会「把已覆盖槽按旧 hop 号入库」→ 重同步,如实记洞。
    if (w > state.lastPulled + capacity)
    {
        state.lastPulled = b1;
        return 0;
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

void FeatPuller::bind(u32 channel, const FeatHeader* header, const FeatFrame* ring, u32 mappedCapacity)
{
    if (channel < 1 || channel > kMaxChannels)
    {
        return;
    }
    auto& b = bindings_[channel - 1];
    b.header = header;
    b.ring = ring;
    // 【几何快照纪律(04 §3 / PR#36)】attach 时读一次 capacity_hops 进快照,此后每拍只读快照;
    // mappedCapacity 必须 >= 声明的 capacity_hops,否则拒绝绑定(越界类事故)。magic/abi 不符 → 拒绑。
    const bool magicOk = (header != nullptr) && header->magic.load(std::memory_order_acquire) == kScvbMagic;
    const bool abiOk = (header != nullptr) && header->abi.load(std::memory_order_acquire) == kScvbAbi;
    const u32 declared = (header != nullptr) ? header->capacity_hops : 0;
    b.capacity = declared;
    b.bound = (magicOk && abiOk && ring != nullptr && declared != 0 && mappedCapacity >= declared);
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
