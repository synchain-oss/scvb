// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/FeatRing.h"

#include <algorithm>

namespace scvb
{

void FeatRing::bind(FeatHeader* header, FeatFrame* ring, u32 mappedCapacity)
{
    // 构造不可变绑定快照并 release 发布(【几何快照纪律】hop_ms/capacity_hops 是 plain u32,attach
    // 时一次性读进快照,此后 startRun/processBlock 只读快照绝不回读段头;mappedCapacity 必须 >= 声明
    // 的 capacity_hops,否则拒绝绑定,保住「几何 ≤ 实际映射」不变式)。
    auto b = std::make_unique<FeatRingBinding>();
    b->header = header;
    b->ring = ring;
    const bool magicOk = (header != nullptr) && header->magic.load(std::memory_order_acquire) == kScvbMagic;
    const bool abiOk = (header != nullptr) && header->abi.load(std::memory_order_acquire) == kScvbAbi;
    const u32 declared = (header != nullptr) ? header->capacity_hops : 0;
    b->capacity = declared;
    b->hopMs = (header != nullptr) ? header->hop_ms : 0;
    b->bound = (magicOk && abiOk && ring != nullptr && declared != 0 && mappedCapacity >= declared);

    binding_.store(b.get(), std::memory_order_release);
    owned_.push_back(std::move(b));
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
    if (!prepared_)
    {
        return;
    }
    const FeatRingBinding* b = binding_.load(std::memory_order_acquire);
    if (b == nullptr || !b->bound)
    {
        return;
    }

    const int64_t hopSamples = extractor_.hopSize();
    // 首个完整 hop(04 §3.2 ceilDiv)。
    const uint64_t h0 = static_cast<uint64_t>((timelineSample + hopSamples - 1) / hopSamples);

    // 顺序不可倒(J33/04 §3.2):先 base 后 write。读方若插在两 store 之间只能见
    // 「新 base + 旧 write」(前跳 seek 空循环 / 回卷幂等重拉,均安全);倒置则见
    // 「旧 base + 新 write」,前跳 seek 把从未写入的陈旧槽整段拉进 FrameStore。
    b->header->base_hop.store(h0, std::memory_order_release);
    if (betweenStores)
    {
        betweenStores();
    }
    b->header->write_hop.store(h0, std::memory_order_release);

    // biquad 预热:run 边界 reset + 首完整 hop 预热(T08 口径,run B 首 hop 不残留 run A 滤波态)。
    extractor_.startRun();

    const int64_t firstFullSample = static_cast<int64_t>(h0) * hopSamples;
    pendingSkip_ = (firstFullSample > timelineSample) ? (firstFullSample - timelineSample) : 0;
    nextHop_ = h0;
}

int FeatRing::processBlock(const float* const* channels, int numSamples)
{
    if (!capturing_ || !prepared_ || numSamples <= 0)
    {
        return 0; // 采集 OFF 或未就绪:静默丢弃(release 不写段、不推进 write_hop)
    }
    const FeatRingBinding* b = binding_.load(std::memory_order_acquire);
    if (b == nullptr || !b->bound)
    {
        return 0; // 未绑定:静默不写、不推进 write_hop
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
            b->ring[hop % b->capacity] =
                FeatFrame{out_[static_cast<std::size_t>(i)].kw_ms, out_[static_cast<std::size_t>(i)].peak};
            ++nextHop_;
            b->header->write_hop.store(nextHop_, std::memory_order_release); // 稳态:写帧后推进 write_hop
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

    // 环回绕守卫(PR#42 复审 R1):读方落后 >= capacity 时,[lastPulled, w-capacity) 的槽已被写方覆盖
    // (h%capacity 已装下后 hop 的数据)。跳前到可恢复前沿 w-capacity(跳过的区间如实记洞)后继续拉最新
    // 窗口;绝不 return 0,否则该 channel 在本 run 剩余时间永久停更(静默漏拉/假覆盖)。用 >= 兜住
    // w==lastPulled+capacity 的覆写竞态(off-by-one);w>capacity 无下溢,边界回落 b1。
    if (w >= state.lastPulled + capacity)
    {
        state.lastPulled = (w > capacity) ? (w - capacity) : b1;
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
