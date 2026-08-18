// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/AudioRing.h"

namespace scvb
{

void AudioRing::bind(AudioRingHeader* header, float* data) noexcept
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

void AudioRing::write(int64_t t0, const float* interleaved, int n) noexcept
{
    if (!bound_ || n <= 0)
    {
        return;
    }
    const u32 mask = geo_.ringFrames - 1;
    const u32 ch = geo_.channels;
    for (int i = 0; i < n; ++i)
    {
        const u32 frame = static_cast<u32>(static_cast<u64>(t0 + i)) & mask;
        for (u32 c = 0; c < ch; ++c)
        {
            data_[static_cast<std::size_t>(frame) * ch + c] = interleaved[static_cast<std::size_t>(i) * ch + c];
        }
    }
    header_->write_head_samples.store(static_cast<u64>(t0 + n), std::memory_order_release);
}

void AudioRing::bumpEpoch() noexcept
{
    if (header_ != nullptr)
    {
        header_->epoch.fetch_add(1, std::memory_order_release);
    }
}

void AudioRing::publishWriteHead(int64_t pos) noexcept
{
    if (header_ != nullptr)
    {
        header_->write_head_samples.store(static_cast<u64>(pos), std::memory_order_release);
    }
}

} // namespace scvb
