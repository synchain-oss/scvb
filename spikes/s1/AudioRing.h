// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// S1 spike —— 音频环(时间线寻址写读 + epoch + 读后 write_head 复查)。单写单读 SPSC;
// 写侧 = Input 音频线程,读侧 = Output 音频线程(同组同 channel 一一对应)。
// 寻址口径(一次性定死,与 SegmentLayout.h 的 ring_frames 注释互为镜像,读写两侧必须一致):
//   float 偏移 = ((timeline_pos & (ring_frames-1)) * channels + c)。
//   ring_frames = 帧数(不是样本数、不是帧对数)。
//
// v5 修复:几何与视图以「不可分快照」(AudioRingGeometry)经单一 atomic 指针发布。
// 宿主编排(实测 Cubase 14/15 的 freeze/RIP/导出)会反复以 mono⇄stereo 重建环;
// 旧实现先换视图再改几何,音频线程可读到「新(小)视图 + 旧(大)声道数」组合,
// 越界写入 → 0xC0000005(转储寄存器实锤:故障地址 = 2MB mono 视图基址 + 2MB + 4KB)。
// 现在写/读线程一次 load 拿到自洽的 {view, data, frames, channels},旧快照保活。

#include <atomic>
#include <cstddef>
#include <vector>

#include "SegmentBackendWin32.h"
#include "SegmentLayout.h"

namespace scvb
{

// ring_frames 建议值(01 §5.3):1<<19 = 524288 帧,mono 2MB / stereo 4MB,时长 ≈10.9s @48k。
// 最终值待 S1 实测回填(验收 9);G-5 套圈测试用 SCVB_RING_FRAMES 环境变量缩到 1<<13。
inline constexpr u32 kDefaultRingFrames = 1u << 19;

// 从环境变量读取 ring_frames 覆盖(测试用,须为 2^k;未设/非法则回落默认)。
u32 ringFramesFromEnv();

// 几何 + 视图不可分快照(v5)。对象与视图同寿命:几何对象入 retired 池永不释放,
// 视图按 v4 泄漏策略(SegmentView::reset 不解映射)进程寿命存活。
struct AudioRingGeometry
{
    SegmentView view; // 拥有映射
    AudioRingHeader* header = nullptr;
    float* data = nullptr;
    u32 frames = 0;
    u32 channels = 0;
};

class AudioRing
{
public:
    // 组号 G 与 channel(1..15)为构造参数(ipc v1.5 [J66])。
    AudioRing(u32 group, u32 channel) : group_(group), channel_(channel) {}
    ~AudioRing();

    AudioRing(const AudioRing&) = delete;
    AudioRing& operator=(const AudioRing&) = delete;

    // Input 侧(消息线程):创建/覆盖式重初始化本 channel 的 audio 段。
    // 段尺寸 = sizeof(AudioRingHeader) + ringFrames*channels*sizeof(float)。
    InitResult createForInput(u32 sampleRate, u32 ringFrames, u32 channels);

    // Output 侧(消息线程):打开已存在的 audio 段(不存在 → kFailed)。
    InitResult openForOutput();

    bool isOpen() const { return geom_.load(std::memory_order_relaxed) != nullptr; }

    u32 group() const { return group_; }
    u32 channel() const { return channel_; }
    AudioRingHeader* header(); // 当前快照头(无几何 → nullptr)
    float* data(); // 当前快照数据指针(无几何 → nullptr)
    u32 ringFrames() const;
    u32 channels() const;
    u32 mask() const;

    // 音频线程写(t0>=0)。interleavedSrc 含 frames*channels 个 float(mono 原样、stereo interleaved LR)。
    void writeBlock(i64 t0, const float* interleavedSrc, int frames);

    // 音频线程:时间线跳变(定位/loop 回跳)时 epoch+1(读方据此丢弃跨代数据)。
    void bumpEpoch();

    enum class ReadStatus
    {
        kOk, // 数据有效,已填入 dst
        kGap // 缺口:区间未覆盖 / 读中换代 / 被写方套圈(调用方应静音 + gapCount+1)
    };

    // 音频线程读(t0>=0)。dst 含 frames*channels 个 float(mono 一列、stereo interleaved LR)。
    // 读后复查 write_head/epoch:w2 > t0+ring_frames 或 e2 != e1 → kGap(防 render-ahead 套圈撕裂)。
    ReadStatus readBlock(i64 t0, int frames, float* interleavedDst);

    // 失败路径:发布空快照 → isOpen()==false,关断音频线程写读(杜绝悬挂/越界)。
    void invalidate();

private:
    u32 group_ = 1;
    u32 channel_ = 1;
    std::atomic<AudioRingGeometry*> geom_{nullptr}; // 单一发布点(v5)
    std::vector<AudioRingGeometry*> retiredGeom_; // 旧快照保活(环对象入泄漏池,随其存活)

    // 读侧状态(仅 Output 音频线程访问,单读者无需原子)。
    i64 lastEpoch_ = -1; // 上次看到的 epoch(-1 = 尚未读过)
    i64 validFrom_ = 0; // 本代有效数据起始时间线位置(epoch 跳变后 = 当前块起点)

    // v6 诊断:ringReject 日志节流时间戳(消息线程)。
    u64 lastRejectLogMs_ = 0;
};

} // namespace scvb