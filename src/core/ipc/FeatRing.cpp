// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/FeatRing.h"

#include <algorithm>

#include "ipc/CtrlPlane.h"

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
    // 不可变运行态发布(PR#51 红旗第 2 轮):构造全新状态,绝不原地 assign —— 播放中并发 re-prepare
    // 时,音频线程在途调用仍持旧状态指针(ownedState_ 保活),无悬垂、无锁、无自旋。
    auto s = std::make_unique<FeatRunState>();
    s->extractor->prepare(sampleRate, channels);
    s->maxBlockSamples = std::max(1, maxBlockSamples);
    const int ch = s->extractor->channels();
    s->shifted.assign(static_cast<std::size_t>(ch), nullptr);
    const int maxFrames = std::max(1, s->maxBlockSamples / s->extractor->hopSize() + 2);
    s->out.assign(static_cast<std::size_t>(maxFrames), analysis::FeatFrame{});

    state_.store(s.get(), std::memory_order_release);
    ownedState_.push_back(std::move(s));
}

void FeatRing::startRun(int64_t timelineSample, const std::function<void()>& betweenStores)
{
    FeatRunState* s = state_.load(std::memory_order_acquire);
    const FeatRingBinding* b = binding_.load(std::memory_order_acquire);
    if (s == nullptr || b == nullptr || !b->bound)
    {
        return;
    }

    const int64_t hopSamples = s->extractor->hopSize();
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
    s->extractor->startRun();

    const int64_t firstFullSample = static_cast<int64_t>(h0) * hopSamples;
    s->pendingSkip = (firstFullSample > timelineSample) ? (firstFullSample - timelineSample) : 0;
    s->nextHop = h0;

    // fingerprint 侧同样按 run 边界重置(04 §4.5):run 切换后滤波态已 reset,跨 run 拼出来的
    // tile 不是同一段音频,必须丢弃重起。fpTileOpen=false ⇒ 只有走到 tile 起点(hop%100==0)
    // 才会开始累加,run 中途接上的半个 tile 永不上报。
    s->fpHop = h0;
    s->fpTile.reset();
    s->fpTileIdx = 0;
    s->fpTileCount = 0;
    s->fpTileOpen = false;
}

void FeatRing::pushFpReport(u64 value) noexcept
{
    // [A] 单生产者:只读自己的 write,acquire-load 消费者的 read。满 → 丢最新 + 计数
    // (绝不覆盖未读记录:未读的是更早、更该先给用户看的那一秒)。
    const u32 w = fpWrite_.load(std::memory_order_relaxed);
    const u32 r = fpRead_.load(std::memory_order_acquire);
    if (w - r >= kFpQueueCapacity)
    {
        fpDrop_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    fpSlots_[w % kFpQueueCapacity] = value;
    fpWrite_.store(w + 1, std::memory_order_release);
}

uint32_t FeatRing::drainFpReports(u64* out, uint32_t max) noexcept
{
    if (out == nullptr || max == 0)
    {
        return 0;
    }
    const u32 w = fpWrite_.load(std::memory_order_acquire);
    u32 r = fpRead_.load(std::memory_order_relaxed);
    uint32_t n = 0;
    while (r != w && n < max)
    {
        out[n++] = fpSlots_[r % kFpQueueCapacity];
        ++r;
    }
    fpRead_.store(r, std::memory_order_release);
    return n;
}

void FeatRing::accumulateFp(FeatRunState& s, float kwMs, bool capturing) noexcept
{
    const uint64_t hop = s.fpHop;
    ++s.fpHop;

    if (hop % analysis::kFpTileHops == 0)
    {
        // tile 起点:开一个新 tile(此前若有未收尾的半个 tile,连同它一起丢弃)。
        s.fpTile.reset();
        // u64 除法直接留在 u64 里:先窄化到 u32 再比上限,会让超长时间线的 tile 号回绕成
        // 一个合法的小号,把一条 18 小时之外的记录错报到某个真实 tile 上。
        s.fpTileIdx = hop / analysis::kFpTileHops;
        s.fpTileCount = 0;
        s.fpTileOpen = true;
    }
    if (!s.fpTileOpen)
    {
        return;
    }

    // 量化到 FrameStore 的 dBq 单位后入 FNV-1a —— 与 Output 侧基线**同一个入口**(见
    // FeatureFingerprint.h 头注「两端逐位一致的前提」)。
    s.fpTile.pushKwDbq(analysis::quantizeKwDbq(kwMs));
    if (++s.fpTileCount < analysis::kFpTileHops)
    {
        return;
    }

    s.fpTileOpen = false;
    // 采集 ON 时不上报:此刻这一秒的特征正在被写成**新基线**,拿它跟自己比毫无意义
    // (04 §4.5「采集 OFF 期间 Input 播放时……每秒上报一条 fp_report」)。
    // tile_idx 超 16 位(≈18.2 小时)不上报(04 §4.5 截断语义),避免钳制后错报到 tile 65535。
    if (capturing || s.fpTileIdx > analysis::kFpMaxTileIdx)
    {
        return;
    }
    pushFpReport(packFpReport(static_cast<u32>(s.fpTileIdx), s.fpTile.value()));
}

int FeatRing::processBlock(const float* const* channels, int numSamples)
{
    if (numSamples <= 0)
    {
        return 0;
    }
    // 采集开关只门控**写段**,不门控 K 加权提取(04 §4.5:采集 OFF 期间照跑同一条代码路径,
    // 产出的 kw 喂 fingerprint watchdog)。段的「仅采集 ON 写」冻结语义由下面的 if (capturing) 保住。
    const bool capturing = capturing_.load(std::memory_order_relaxed);
    FeatRunState* s = state_.load(std::memory_order_acquire);
    const FeatRingBinding* b = binding_.load(std::memory_order_acquire);
    if (s == nullptr || b == nullptr || !b->bound)
    {
        return 0; // 未就绪/未绑定:静默不写、不推进 write_hop
    }

    int consumed = 0;
    bool tailDropped = false;
    if (s->pendingSkip > 0)
    {
        const int skip = static_cast<int>(std::min<int64_t>(s->pendingSkip, static_cast<int64_t>(numSamples)));
        s->pendingSkip -= skip;
        consumed = skip;
        tailDropped = s->pendingSkip > 0; // 整块仍是 run 起始部分 hop 的尾巴,丢弃
    }

    int written = 0;
    if (!tailDropped)
    {
        // 大块按 maxBlockSamples 分段喂 extractor:out 只按 maxBlockSamples/hopSize+2 分配,
        // 宿主离线渲染/bounce 时块长可超 maximumExpectedSamplesPerBlock,一次性喂会越界读 out(PR#42 红旗)。
        const int ch = s->extractor->channels();
        int remaining = numSamples - consumed;
        while (remaining > 0)
        {
            const int chunk = std::min(remaining, s->maxBlockSamples);
            for (int c = 0; c < ch; ++c)
            {
                s->shifted[static_cast<std::size_t>(c)] = channels[c] + consumed;
            }

            const int got =
                s->extractor->processBlock(s->shifted.data(), chunk, s->out.data(), static_cast<int>(s->out.size()));
            // 兜底 clamp:正常分段下 got <= out.size();防御性钳制,杜绝任何越界读。
            const int nFrames = std::min(got, static_cast<int>(s->out.size()));
            for (int i = 0; i < nFrames; ++i)
            {
                const analysis::FeatFrame& f = s->out[static_cast<std::size_t>(i)];
                if (capturing)
                {
                    const uint64_t hop = s->nextHop;
                    b->ring[hop % b->capacity] = FeatFrame{f.kw_ms, f.peak};
                    ++s->nextHop;
                    b->header->write_hop.store(s->nextHop, std::memory_order_release); // 稳态:写帧后推进 write_hop
                    ++written;
                }
                accumulateFp(*s, f.kw_ms, capturing);
            }
            consumed += chunk;
            remaining -= chunk;
        }
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
