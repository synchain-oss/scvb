// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/AudioRing.h"

namespace scvb
{

void AudioRing::bind(AudioRingHeader* header, float* data)
{
    if (header == nullptr || data == nullptr)
    {
        binding_.store(nullptr, std::memory_order_release);
        return;
    }

    // 构造不可变绑定:magic/abi 校验 + 几何快照(bind 时读一次,此后只读快照,绝不回读段头)。
    auto b = std::make_unique<AudioRingBinding>();
    b->header = header;
    b->data = data;
    const bool magicOk = header->magic.load(std::memory_order_acquire) == kScvbMagic;
    const bool abiOk = header->abi.load(std::memory_order_acquire) == kScvbAbi;
    const u32 sr = header->sample_rate;
    const u32 frames = header->ring_frames;
    const u32 channels = header->channels;
    const bool framesPow2 = frames != 0 && (frames & (frames - 1)) == 0;
    const bool geoOk = framesPow2 && (channels == 1 || channels == 2);
    b->geo = AudioRingGeometry{sr, frames, channels};
    b->bound = magicOk && abiOk && geoOk;

    binding_.store(b.get(), std::memory_order_release);
    owned_.push_back(std::move(b));
}

void AudioRing::write(const AudioRingBinding* b, int64_t t0, const float* interleaved, int n) noexcept
{
    if (b == nullptr || !b->bound || n <= 0)
    {
        return;
    }
    const u32 mask = b->geo.ringFrames - 1;
    const u32 ch = b->geo.channels;
    for (int i = 0; i < n; ++i)
    {
        const u32 frame = static_cast<u32>(static_cast<u64>(t0 + i)) & mask;
        for (u32 c = 0; c < ch; ++c)
        {
            b->data[static_cast<std::size_t>(frame) * ch + c] = interleaved[static_cast<std::size_t>(i) * ch + c];
        }
    }
    b->header->write_head_samples.store(static_cast<u64>(t0 + n), std::memory_order_release);
}

void AudioRing::bumpEpoch(const AudioRingBinding* b) noexcept
{
    if (b != nullptr && b->bound)
    {
        b->header->epoch.fetch_add(1, std::memory_order_release);
    }
}

void AudioRing::publishWriteHead(const AudioRingBinding* b, int64_t pos) noexcept
{
    if (b != nullptr && b->bound)
    {
        b->header->write_head_samples.store(static_cast<u64>(pos), std::memory_order_release);
    }
}

} // namespace scvb
