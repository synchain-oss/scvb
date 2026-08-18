// SPDX-License-Identifier: GPL-3.0-or-later
#include "output/ShmRingMixSource.h"

namespace scvb::output
{

void ShmRingMixSource::bind(AudioRingHeader* header, float* data) noexcept
{
    header_ = header;
    data_ = data;
    bound_ = false;
    geo_ = AudioRingGeometry{};
    if (header_ == nullptr || data_ == nullptr)
    {
        return;
    }

    // magic/abi 校验 + 几何快照(bind 时读一次,此后只读快照,绝不回读段头几何)。
    const bool magicOk = header_->magic.load(std::memory_order_acquire) == kScvbMagic;
    const bool abiOk = header_->abi.load(std::memory_order_acquire) == kScvbAbi;
    const u32 sr = header_->sample_rate;
    const u32 frames = header_->ring_frames;
    const u32 channels = header_->channels;
    const bool framesPow2 = frames != 0 && (frames & (frames - 1)) == 0;
    if (!magicOk || !abiOk || !framesPow2 || (channels != 1 && channels != 2))
    {
        return;
    }
    geo_ = AudioRingGeometry{sr, frames, channels};
    bound_ = true;
}

void ShmRingMixSource::unbind() noexcept
{
    header_ = nullptr;
    data_ = nullptr;
    bound_ = false;
    geo_ = AudioRingGeometry{};
    lastEpoch_ = 0;
    validFrom_ = 0;
}

void ShmRingMixSource::resetTimeline() noexcept
{
    lastEpoch_ = 0;
    validFrom_ = 0;
}

u64 ShmRingMixSource::writeHead() const noexcept
{
    return header_ != nullptr ? header_->write_head_samples.load(std::memory_order_acquire) : 0;
}

u64 ShmRingMixSource::epoch() const noexcept
{
    return header_ != nullptr ? header_->epoch.load(std::memory_order_acquire) : 0;
}

bool ShmRingMixSource::read(int64_t t0, float* dst, int n) noexcept
{
    if (!bound_ || dst == nullptr || n <= 0 || t0 < 0)
    {
        return false;
    }

    const u64 e1 = header_->epoch.load(std::memory_order_acquire);
    const u64 w = header_->write_head_samples.load(std::memory_order_acquire);
    if (e1 != lastEpoch_)
    {
        // epoch 跳变:本代有效数据从此刻起算(§5.2)。
        lastEpoch_ = e1;
        validFrom_ = t0;
    }

    // covered 判定(§5.2):区间在有效代内、写头覆盖整块、且未超环距(过旧数据已被覆盖)。
    const u64 t0u = static_cast<u64>(t0);
    const bool covered = (t0 >= validFrom_) && (w >= t0u + static_cast<u64>(n)) && (w - t0u <= geo_.ringFrames);
    if (!covered)
    {
        gapCount_.fetch_add(1, std::memory_order_relaxed); // 缺口→该轨该块静音+失准计数(ADR-002)
        return false;
    }

    // 读样本(重叠语义:同一时间线位置后写覆盖先写 → 按地址读到的即「时间线正确者」)。
    // [J57] 按环头 channels(1|2)解码:stereo = interleaved LR(契约 v1.4)。
    const u32 mask = geo_.ringFrames - 1;
    const u32 nch = geo_.channels;
    for (int i = 0; i < n; ++i)
    {
        const u32 frame = static_cast<u32>(static_cast<u64>(t0 + i)) & mask;
        for (u32 c = 0; c < nch; ++c)
        {
            dst[static_cast<std::size_t>(i) * nch + c] = data_[static_cast<std::size_t>(frame) * nch + c];
        }
    }

    // 读中换代或写方套圈(R1):整块弃用(防 render-ahead 超环距的静默撕裂读)。
    const u64 e2 = header_->epoch.load(std::memory_order_acquire);
    const u64 w2 = header_->write_head_samples.load(std::memory_order_acquire);
    if (e2 != e1 || w2 > t0u + geo_.ringFrames)
    {
        gapCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

} // namespace scvb::output
