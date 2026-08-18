// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// AudioRing —— 音频环写侧封装(ipc-contract §2 冻结布局 + 01 §5.1 步骤 4)。Input 写、Output 读
// (T24);本卡只落写侧,读侧归 Output。JUCE-free,可离线单测。
//
// 几何快照纪律(Registry.h 同名纪律 + S1 v5 AudioRingGeometry 教训,硬性):
//   sample_rate / ring_frames / channels 是 plain u32(非原子),bind() 时一次性读进本地快照 geo_,
//   此后 write() 只读快照、绝不回读段头几何字段 —— 宿主编排 mono⇄stereo 重建环时,回读会读到
//   「新几何 + 旧视图」的撕裂组合 → 越界写共享内存(0xC0000005)。同一次 prepare 内 channels 恒定,
//   processBlock 只读 geo_.channels([J57] 验收③)。
//   段恒按 stereo 容量创建(ring_frames × 2 float);channels=1|2,mono 只用前半、stereo 用满容量
//   (01 §5.3 / Registry.h 几何纪律第 2 条)。
//
// write() 按时间线绝对样本位置寻址:frame index = timeline_pos & (ring_frames-1),
//   stereo 为 interleaved LR(契约 v1.4 [J57])。数据先写、write_head 后发布(release,契约 §2)。

#include <cstdint>

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

class AudioRing
{
public:
    AudioRing() = default;

    // 绑定段头与环数据;一次性快照几何(见文件头纪律)。channels ∉ {1,2} 或 ring_frames 非 2^k →
    // 拒绝绑定(bound()==false,processBlock 静默不写)。header/data 为 nullptr → 解绑。
    void bind(AudioRingHeader* header, float* data) noexcept;

    bool bound() const noexcept { return bound_; }
    AudioRingHeader* header() noexcept { return header_; }
    float* data() noexcept { return data_; }
    AudioRingGeometry geometry() const noexcept { return geo_; }

    // 写 n 帧 interleaved 数据到时间线 t0;数据先写、write_head=t0+n 后发布(契约 §2)。
    // 调用方保证 t0>=0(01 §5.1 步骤 2 负时间线早退)。n<=0 或未绑定 → 空操作。
    void write(int64_t t0, const float* interleaved, int n) noexcept;

    // epoch 跳变(transport 定位/loop 回跳)时 +1(契约 §2)。
    void bumpEpoch() noexcept;

    // 直接发布 write_head(供跨零点尾段写入等场景)。
    void publishWriteHead(int64_t pos) noexcept;

private:
    AudioRingHeader* header_ = nullptr;
    float* data_ = nullptr;
    AudioRingGeometry geo_{};
    bool bound_ = false;
};

} // namespace scvb
