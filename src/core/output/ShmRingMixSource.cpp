// SPDX-License-Identifier: GPL-3.0-or-later
#include "output/ShmRingMixSource.h"

namespace scvb::output
{

void ShmRingMixSource::bind(AudioRingHeader* header, float* data) noexcept
{
    if (header == nullptr || data == nullptr)
    {
        binding_.store(nullptr, std::memory_order_release);
        return;
    }

    // 构造不可变绑定:magic/abi 校验 + 几何快照(bind 时读一次,此后只读快照,绝不回读段头几何)。
    auto b = std::make_unique<AudioRingBinding>();
    b->header = header;
    b->data = data;
    const bool magicOk = header->magic.load(std::memory_order_acquire) == kScvbMagic;
    const bool abiOk = header->abi.load(std::memory_order_acquire) == kScvbAbi;
    const u32 sr = header->sample_rate;
    const u32 frames = header->ring_frames;
    const u32 channels = header->channels;
    const bool framesPow2 = frames != 0 && (frames & (frames - 1)) == 0;
    b->geo = AudioRingGeometry{sr, frames, channels};
    b->bound = magicOk && abiOk && framesPow2 && (channels == 1 || channels == 2);

    binding_.store(b.get(), std::memory_order_release);
    owned_.push_back(std::move(b));
}

void ShmRingMixSource::unbind() noexcept
{
    // 发布 nullptr(防悬垂);不触碰音频线程独占的 lastBinding_/lastEpoch_/validFrom_
    // (由 read() 经绑定指针变化自行重置,避免 [M]/[A] 数据竞争)。
    binding_.store(nullptr, std::memory_order_release);
}

bool ShmRingMixSource::bound() const noexcept
{
    const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
    return b != nullptr && b->bound;
}

u32 ShmRingMixSource::channels() const noexcept
{
    const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
    return b != nullptr ? b->geo.channels : 0;
}

u32 ShmRingMixSource::sampleRate() const noexcept
{
    const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
    return b != nullptr ? b->geo.sampleRate : 0;
}

u32 ShmRingMixSource::ringFrames() const noexcept
{
    const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
    return b != nullptr ? b->geo.ringFrames : 0;
}

u64 ShmRingMixSource::writeHead() const noexcept
{
    const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
    return (b != nullptr && b->bound) ? b->header->write_head_samples.load(std::memory_order_acquire) : 0;
}

u64 ShmRingMixSource::epoch() const noexcept
{
    const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
    return (b != nullptr && b->bound) ? b->header->epoch.load(std::memory_order_acquire) : 0;
}

bool ShmRingMixSource::read(int64_t t0, float* dst, int n) noexcept
{
    // 硬约束:本方法 acquire 的绑定裸指针只在「本块」内有效 —— 底层共享内存段由 OutputSession 的
    // SegmentHandle 500ms 宽限期(kReleaseGraceMs > 单块 wall-clock)保活,不得跨块持有裸指针(S1 验收)。
    const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
    if (b == nullptr || !b->bound || dst == nullptr || n <= 0 || t0 < 0)
    {
        return false;
    }

    // 绑定指针变化(重绑/换代)→ 重置代际状态(等效旧 resetTimeline,但只发生在音频线程)。
    if (b != lastBinding_)
    {
        lastBinding_ = b;
        lastEpoch_ = 0;
        validFrom_ = 0;
    }

    const u64 e1 = b->header->epoch.load(std::memory_order_acquire);
    const u64 w = b->header->write_head_samples.load(std::memory_order_acquire);
    if (e1 != lastEpoch_)
    {
        // epoch 跳变:本代有效数据从此刻起算(§5.2)。
        lastEpoch_ = e1;
        validFrom_ = t0;
    }

    // covered 判定(§5.2):区间在有效代内、写头覆盖整块、且未超环距(过旧数据已被覆盖)。
    const u64 t0u = static_cast<u64>(t0);
    const bool covered = (t0 >= validFrom_) && (w >= t0u + static_cast<u64>(n)) && (w - t0u <= b->geo.ringFrames);
    if (!covered)
    {
        gapCount_.fetch_add(1, std::memory_order_relaxed); // 缺口→该轨该块静音+失准计数(ADR-002)
        return false;
    }

    // 读样本(重叠语义:同一时间线位置后写覆盖先写 → 按地址读到的即「时间线正确者」)。
    // [J57] 按环头 channels(1|2)解码:stereo = interleaved LR(契约 v1.4)。
    const u32 mask = b->geo.ringFrames - 1;
    const u32 nch = b->geo.channels;
    for (int i = 0; i < n; ++i)
    {
        const u32 frame = static_cast<u32>(static_cast<u64>(t0 + i)) & mask;
        for (u32 c = 0; c < nch; ++c)
        {
            dst[static_cast<std::size_t>(i) * nch + c] = b->data[static_cast<std::size_t>(frame) * nch + c];
        }
    }

    // 读中换代或写方套圈(R1):整块弃用(防 render-ahead 超环距的静默撕裂读)。
    const u64 e2 = b->header->epoch.load(std::memory_order_acquire);
    const u64 w2 = b->header->write_head_samples.load(std::memory_order_acquire);
    if (e2 != e1 || w2 > t0u + b->geo.ringFrames)
    {
        gapCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

} // namespace scvb::output
