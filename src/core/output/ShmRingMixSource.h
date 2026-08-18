// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ShmRingMixSource —— IMixSource 的共享内存实现(01 §5.2 读环语义)。
// bind() 一次性几何快照(sample_rate / ring_frames / channels,Registry.h 几何纪律);
// read() 只读快照,绝不回读段头几何字段(宿主编排 mono⇄stereo 重建环时的撕裂防护)。
// read() 逐行实现:covered 判定 → 读中换代弃用 → 套圈弃用 → 失准计数(01 §5.2)。

#include <atomic>
#include <cstdint>

#include "output/IMixSource.h"
#include "ipc/AudioRing.h"
#include "ipc/SegmentLayout.h"

namespace scvb::output
{

class ShmRingMixSource final : public IMixSource
{
public:
    ShmRingMixSource() = default;

    // 绑定段头与环数据;一次性快照几何。channels ∉ {1,2} 或 ring_frames 非 2^k 或
    // magic/abi 不符 → 拒绝绑定(bound()==false,read() 恒 false 且不计数)。
    void bind(AudioRingHeader* header, float* data) noexcept;

    // 解绑(释放/改组路径,防悬垂)。
    void unbind() noexcept;

    // 换代后本代有效数据起点重置(epoch 跳变时由 read() 内部维护;SR 重建时外部调用)。
    void resetTimeline() noexcept;

    // IMixSource
    bool bound() const noexcept override { return bound_; }
    u32 channels() const noexcept override { return geo_.channels; }
    u32 sampleRate() const noexcept override { return geo_.sampleRate; }
    u32 ringFrames() const noexcept override { return geo_.ringFrames; }
    bool read(int64_t t0, float* dst, int n) noexcept override;
    u32 gapCount() const noexcept override { return gapCount_.load(std::memory_order_relaxed); }
    u64 writeHead() const noexcept override;
    u64 epoch() const noexcept override;

private:
    AudioRingHeader* header_ = nullptr;
    float* data_ = nullptr;
    AudioRingGeometry geo_{};
    bool bound_ = false;

    // 音频线程独占(仅 read() 访问)。
    u64 lastEpoch_ = 0;
    int64_t validFrom_ = 0; // 本代有效数据起点(epoch 跳变后 = 当前块起点,§5.2)

    std::atomic<u32> gapCount_{0};
};

} // namespace scvb::output
