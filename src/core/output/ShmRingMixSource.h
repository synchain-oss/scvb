// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ShmRingMixSource —— IMixSource 的共享内存实现(01 §5.2 读环语义)。
// bind() 一次性几何快照(sample_rate / ring_frames / channels,Registry.h 几何纪律);
// read() 只读快照,绝不回读段头几何字段(宿主编排 mono⇄stereo 重建环时的撕裂防护)。
// read() 逐行实现:covered 判定 → 读中换代弃用 → 套圈弃用 → 失准计数(01 §5.2)。
//
// 跨线程发布协议(T16 DspArbiter / T23 AudioRing 同款 Snapshot 模式):
//   绑定是「不可变快照 + std::atomic<const AudioRingBinding*>」发布 —— 消息线程 bind()/unbind()
//   构造完整绑定后 release-store;音频线程每块 acquire-load 一次、整块复用同一份(无撕裂、无
//   bound==true 且 header==nullptr 的中间态)。旧绑定由本类 owned_ 保活(进程寿命);绑定内裸指针
//   指向的段由调用方(OutputSession)经 SegmentHandle 宽限期保活,音频线程块内使用、绝不跨块持有。

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

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
    // magic/abi 不符 → 发布 bound==false 的绑定(或 nullptr),read() 恒 false 且不计数。
    void bind(AudioRingHeader* header, float* data) noexcept;

    // 解绑(释放/改组路径,防悬垂):release-store nullptr,此后 read()/bound() 均空操作。
    void unbind() noexcept;

    // 音频线程:每 block acquire-load 一次不可变绑定快照,整 block 复用。
    const AudioRingBinding* acquire() const noexcept { return binding_.load(std::memory_order_acquire); }

    // IMixSource
    bool bound() const noexcept override;
    u32 channels() const noexcept override;
    u32 sampleRate() const noexcept override;
    u32 ringFrames() const noexcept override;
    bool read(int64_t t0, float* dst, int n) noexcept override;
    u32 gapCount() const noexcept override { return gapCount_.load(std::memory_order_relaxed); }
    u64 writeHead() const noexcept override;
    u64 epoch() const noexcept override;

private:
    std::atomic<const AudioRingBinding*> binding_{nullptr}; // [M] 写 / [A] 读
    std::vector<std::unique_ptr<AudioRingBinding>> owned_; // 旧绑定保活(进程寿命,T16 同款)

    // 音频线程独占(仅 read() 访问;换代/重绑由 lastBinding_ 指针变化检测,不回读成员)。
    const AudioRingBinding* lastBinding_ = nullptr; // 上次块所用绑定(变指针 → 重置代际状态)
    u64 lastEpoch_ = 0;
    int64_t validFrom_ = 0; // 本代有效数据起点(epoch 跳变后 = 当前块起点,§5.2)
    // 本代是否已成功读到过数据。未 primed 的 covered 失败 = 写方还没追到本位置(刚 attach 的空环 /
    // 起播瞬间 / 宿主先渲染 Output 再渲染 Input),是「尚未上线」而非「失准」,不得计数 ——
    // 否则所有注入轨会在同一块同时 +1(T37 三轮 A 族「五轨几乎同时报失准」)。
    bool primed_ = false;

    std::atomic<u32> gapCount_{0};
};

} // namespace scvb::output
