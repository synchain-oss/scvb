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
        // [B 线 M04] 换代之后、写新一代数据之前的 release fence。调用方(InputProcessor 音频线程)
        // 紧接着就在同一线程里 write() 新一代样本;release RMW 只约束它**之前**的访问,挡不住这些
        // 样本写被提到 epoch+1 之前可见 —— arm64 上读方可能前后两次都读到旧 epoch(e1 == e2),
        // 样本却已是新一代,把跨代数据当本代收下。fence 让「先换代、后写数据」在写侧成立;读侧在
        // 样本循环之后、e2 之前的 acquire fence 归 A 线(ShmRingMixSource,A-5),两侧配齐才完整。
        // x86:release fence 只是编译器屏障,不生成指令(lock xadd 本身已是全屏障,x86 上原本就不撕裂)。
        // arm64:每次换代一条 DMB ISH;换代只在定位 / 循环回跳时发生,不在逐块路径上。
        std::atomic_thread_fence(std::memory_order_release);
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
