// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// AudioRing —— 音频环写侧封装(ipc-contract §2 冻结布局 + 01 §5.1 步骤 4)。Input 写、Output 读
// (T24);本卡只落写侧,读侧归 Output。JUCE-free,可离线单测。
//
// 几何快照纪律(Registry.h 同名纪律 + S1 v5 AudioRingGeometry 教训,硬性):
//   sample_rate / ring_frames / channels 是 plain u32(非原子),bind() 时一次性读进不可变绑定快照
//   AudioRingBinding.geo,此后 write() 只读该快照、绝不回读段头几何字段 —— 宿主编排 mono⇄stereo
//   重建环时,回读会读到「新几何 + 旧视图」的撕裂组合 → 越界写共享内存(0xC0000005)。
//   段恒按 stereo 容量创建(ring_frames × 2 float);channels=1|2,mono 只用前半、stereo 用满容量。
//
// 跨线程发布协议(T16 DspArbiter 同款 Snapshot 模式):
//   绑定是「不可变快照 + std::atomic<const AudioRingBinding*>」发布 —— 消息线程 bind() 构造完整
//   绑定后 release-store;音频线程每 block acquire-load 一次、整 block 复用同一份(无撕裂)。旧绑定
//   由本类 owned_ 保活(进程寿命,与 DspArbiter「旧快照由发布方保活」一致);绑定内裸指针指向的段
//   由调用方(InputSession)经 SegmentHandle 租约/宽限期保活,音频线程块内使用、绝不跨块持有。
//
// write() 按时间线绝对样本位置寻址:frame index = timeline_pos & (ring_frames-1),
//   stereo 为 interleaved LR(契约 v1.4 [J57])。数据先写、write_head 后发布(release,契约 §2)。

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "SegmentLayout.h"

namespace scvb
{

// 环几何快照(sample_rate / ring_frames / channels 三个 plain u32,bind 时读一次)。
struct AudioRingGeometry
{
    u32 sampleRate = 0;
    u32 ringFrames = 0; // 帧数(不是样本数;帧 = channels 个 interleaved float)
    u32 channels = 0; // 1=mono 2=stereo([J57])
};

// 不可变环绑定快照(发布后不得修改)。裸指针指向共享内存段,由调用方经 SegmentHandle 保活。
struct AudioRingBinding
{
    AudioRingHeader* header = nullptr;
    float* data = nullptr;
    AudioRingGeometry geo{};
    bool bound = false;
};

class AudioRing
{
public:
    AudioRing() = default;

    // 消息线程(持 lifecycleMutex):构造+发布绑定快照。校验 magic/abi/ring_frames 2^k/channels∈{1,2},
    // 校验失败或 header/data 为 nullptr → 发布 nullptr(bound()==false,processBlock 静默不写)。
    void bind(AudioRingHeader* header, float* data);

    // 音频线程:每 block acquire-load 一次,整 block 复用。
    const AudioRingBinding* acquire() const noexcept { return binding_.load(std::memory_order_acquire); }

    bool bound() const noexcept
    {
        const AudioRingBinding* b = acquire();
        return b != nullptr && b->bound;
    }
    AudioRingGeometry geometry() const noexcept
    {
        const AudioRingBinding* b = acquire();
        return b != nullptr ? b->geo : AudioRingGeometry{};
    }

    // 静态写(以绑定快照为参,不回读成员)。b 可为 nullptr(静默不写)。调用方保证 t0>=0。
    static void write(const AudioRingBinding* b, int64_t t0, const float* interleaved, int n) noexcept;
    // epoch 跳变(transport 定位/loop 回跳)时 +1(契约 §2)。
    static void bumpEpoch(const AudioRingBinding* b) noexcept;
    // 直接发布 write_head(供跨零点尾段写入等场景)。
    static void publishWriteHead(const AudioRingBinding* b, int64_t pos) noexcept;

private:
    std::atomic<const AudioRingBinding*> binding_{nullptr}; // 消息线程写 / 音频线程读
    std::vector<std::unique_ptr<AudioRingBinding>> owned_; // 旧绑定保活(进程寿命,T16 同款)
};

} // namespace scvb
