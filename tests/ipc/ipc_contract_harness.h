// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ipc_contract_harness.h —— T07b L1 双进程 IPC 契约测试的共享 harness。
//
// 生产代码(T06/T07/T20)只冻结了 AudioRingHeader 的布局(ipc-contract v1.5 §2),环的实际
// 写/读时序(01 §5.1/§5.2)归 T23/T24,尚未落地。本 harness 直接按冻结布局 + 01 §5.1/§5.2
// 伪代码实现「时间线寻址 + epoch 换代 + 读后 write_head 复查 + gap 计数」的最简环读写,
// 供 tests/tools/scvb_ipc_peer 与 tests/ipc/test_ipc_contract.cpp 共用——两侧跑的是同一套
// 语义,验证的正是跨进程共享内存 + 生命周期,而不是同进程模拟(J19)。
//
// 关键口径(与 SegmentLayout.h AudioRingHeader 注释逐字一致):
//   ring_frames = 环内「帧」数(帧 = 一个时间线样本位置 = channels 个 interleaved float)。
//   样本数 = ring_frames * channels;float 容量 = ring_frames * channels;
//   帧地址 = ((timeline_pos & (ring_frames-1)) * channels + c)。段恒按 stereo 容量创建。

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

#include "ipc/SegmentLayout.h"

namespace scvb::ipctest
{

// ---- 音频环几何快照(attach 时读一次,之后不回读段头;01 §5.1/Registry.h 几何快照纪律)----
struct AudioGeometry
{
    u32 sampleRate = 48000;
    u32 ringFrames = kDefaultRingFrames; // 帧数(2^k),不是样本数
    u32 channels = 1; // 1=mono 2=stereo
};

// 段恒按 stereo 容量创建(ring_frames × 2 float),声道切换(1→2)不扩容段。
inline std::size_t audioSegmentBytes(u32 ringFrames)
{
    return sizeof(AudioRingHeader) + static_cast<std::size_t>(ringFrames) * 2 * sizeof(float);
}

inline float* ringData(AudioRingHeader* h)
{
    return reinterpret_cast<float*>(h + 1);
}

inline const float* ringData(const AudioRingHeader* h)
{
    return reinterpret_cast<const float*>(h + 1);
}

// 几何字段写回(initHeader 的 initData 回调:magic 发布前落盘几何)。
inline void initAudioHeader(AudioRingHeader* h, const AudioGeometry& g)
{
    h->sample_rate = g.sampleRate;
    h->ring_frames = g.ringFrames;
    h->channels = g.channels;
    h->write_head_samples.store(0, std::memory_order_relaxed);
    h->epoch.store(0, std::memory_order_relaxed);
}

// ---- 确定性斜坡(跨进程同一函数,逐样本可对拍)----
// 值域为整数浮点(精确可表示,无舍入差);stereo 时 L 正斜坡 / R 负斜坡(互换即可见,IPC-19)。
// gen 使「代」可区分(IPC-4 跨代样本判据):旧代/新代在同一时间线位置产生不同值,读到陈旧槽即错位。
inline float sampleAt(u64 pos, u32 ch, u32 channels, u64 gen)
{
    const u64 v = (pos + gen * 0x9E3779B97F4A7C15ull + static_cast<u64>(ch) * 0x9E37u) & 1023u;
    float f = static_cast<float>(static_cast<int>(v));
    if (channels == 2 && ch == 1)
    {
        f = -f;
    }
    return f;
}

// ---- 时间线模型(写/读两侧共用,保证期望值与实际值同构)----
// seekAt < 0 表示无 seek;否则第 seekAt 块(0 基)起 t0 从 seekTo 重新前进,且 generation 升为 2。
struct TimelineModel
{
    int64_t startT0 = 0;
    int64_t seekAt = -1;
    int64_t seekTo = 0;

    int64_t t0Of(int64_t blockIndex, int blocksize) const
    {
        if (seekAt >= 0 && blockIndex >= seekAt)
        {
            return seekTo + (blockIndex - seekAt) * blocksize;
        }
        return startT0 + blockIndex * blocksize;
    }

    u64 genOf(int64_t blockIndex) const { return (seekAt >= 0 && blockIndex >= seekAt) ? 2 : 1; }
};

// ---- 写侧状态(01 §5.1)----
struct RingWriterState
{
    bool hasT0 = false;
    int64_t lastT0 = 0;
    int64_t expectedNext = 0;
};

// 写一整块(数据先写、write_head release 后发布)。discontinuity 时 epoch+1(停走带静止重写不算)。
// interleaved 长度 = n * channels。
inline void ringWriteBlock(AudioRingHeader* h, const AudioGeometry& g, int64_t t0, int n, const float* interleaved,
                           RingWriterState& st)
{
    if (st.hasT0 && t0 != st.expectedNext && t0 != st.lastT0)
    {
        h->epoch.fetch_add(1, std::memory_order_release);
    }
    st.lastT0 = t0;
    st.expectedNext = t0 + n;
    st.hasT0 = true;

    float* ring = ringData(h);
    const u32 mask = g.ringFrames - 1;
    for (int i = 0; i < n; ++i)
    {
        const u64 pos = static_cast<u64>(t0 + i);
        const u32 idx = static_cast<u32>(pos & mask) * g.channels;
        for (u32 c = 0; c < g.channels; ++c)
        {
            ring[idx + c] = interleaved[static_cast<std::size_t>(i) * g.channels + c];
        }
    }
    h->write_head_samples.store(static_cast<u64>(t0 + n), std::memory_order_release);
}

// ---- 读侧状态(01 §5.2)----
struct RingReaderState
{
    bool hasEpoch = false;
    u64 lastEpoch = 0;
    int64_t validFrom = 0;
    u64 gapCount = 0;
};

// 读一整块:覆盖判定 + 拷贝 + 读后 write_head 复查(套圈/换代弃块)。返回本块是否计入 gap。
// outInterleaved 长度 = n * channels;gap 时填 0(该块静音)。
// afterCopy 仅供测试注入「拷贝期间写方套圈/换代」的交错(与 FeatRing::startRun 的 betweenStores 同法)。
inline u32 ringReadBlock(const AudioRingHeader* h, const AudioGeometry& g, int64_t t0, int n, float* outInterleaved,
                         RingReaderState& st, const std::function<void()>& afterCopy = {})
{
    const u64 e1 = h->epoch.load(std::memory_order_acquire);
    const u64 w = h->write_head_samples.load(std::memory_order_acquire);
    if (!st.hasEpoch || e1 != st.lastEpoch)
    {
        st.lastEpoch = e1;
        st.hasEpoch = true;
        st.validFrom = t0; // 换代:本代有效数据从当前块起点起算
    }

    const bool covered = (t0 >= st.validFrom) && (w >= static_cast<u64>(t0 + n)) &&
                         ((w - static_cast<u64>(t0)) <= static_cast<u64>(g.ringFrames));
    if (!covered)
    {
        std::memset(outInterleaved, 0, static_cast<std::size_t>(n) * g.channels * sizeof(float));
        ++st.gapCount;
        return 1;
    }

    const float* ring = ringData(h);
    const u32 mask = g.ringFrames - 1;
    for (int i = 0; i < n; ++i)
    {
        const u64 pos = static_cast<u64>(t0 + i);
        const u32 idx = static_cast<u32>(pos & mask) * g.channels;
        for (u32 c = 0; c < g.channels; ++c)
        {
            outInterleaved[static_cast<std::size_t>(i) * g.channels + c] = ring[idx + c];
        }
    }

    if (afterCopy)
    {
        afterCopy(); // 测试注入:模拟拷贝期间写方推进(套圈)或换代
    }
    const u64 e2 = h->epoch.load(std::memory_order_acquire);
    const u64 w2 = h->write_head_samples.load(std::memory_order_acquire);
    if (e2 != e1 || w2 > static_cast<u64>(t0) + static_cast<u64>(g.ringFrames))
    {
        // 读中换代 / 被写方套圈:整块弃用 + gap(防静默撕裂读,01 §5.2)
        std::memset(outInterleaved, 0, static_cast<std::size_t>(n) * g.channels * sizeof(float));
        ++st.gapCount;
        return 1;
    }
    return 0;
}

// FNV-1a 64(对精确 float 位模式哈希,逐样本一致判据;碰撞概率可忽略)。
inline u64 fnv1a64(const float* data, std::size_t nfloats, u64 h = 0xcbf29ce484222325ull)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < nfloats * sizeof(float); ++i)
    {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

// 为一块生成 interleaved 期望/写入样本。
inline void fillBlockSamples(float* out, int64_t t0, int n, const AudioGeometry& g, u64 gen)
{
    for (int i = 0; i < n; ++i)
    {
        const u64 pos = static_cast<u64>(t0 + i);
        for (u32 c = 0; c < g.channels; ++c)
        {
            out[static_cast<std::size_t>(i) * g.channels + c] = sampleAt(pos, c, g.channels, gen);
        }
    }
}

// 段名参数化断言(IPC-1 [J66]):G=1/G=8 各断言一次全名,无组号常量不复存。
inline bool segmentNamesAreParametric()
{
    return segmentLogicalName(1, SegmentKind::kRegistry) == L"SynchainSCVB.v1.g1.registry" &&
           segmentLogicalName(8, SegmentKind::kAudio, 15) == L"SynchainSCVB.v1.g8.audio.ch15" &&
           segmentLogicalName(8, SegmentKind::kFeat, 15) == L"SynchainSCVB.v1.g8.feat.ch15" &&
           segmentLogicalName(8, SegmentKind::kCtrl) == L"SynchainSCVB.v1.g8.ctrl";
}

} // namespace scvb::ipctest
