// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

// PlayheadShot:音频线程([A])→ 消息线程([M]) 的 playhead 快照通道唯一真源定义
// (03 §3.1 / 01 C8 / 04 §2.2)。单写(音频线程)单读(消息线程),最新值语义:
// 写方每块整体发布一次,零分配零锁;读方 seq 奇偶协议双读,撕裂即沿用上帧(不自旋)。
// 纯 C++17,无 JUCE(ADR-011:core 不得依赖 JUCE)。
// 取舍:pod 为普通字段(标准 seqlock,Linux 内核 / JUCE 同款),严格 C++ 内存模型下并发读写属
// 数据竞争,但 seq 奇偶配对 + acquire/release 成对 + fence 收边在实践上安全;撕裂读返回 false 沿用上帧。
namespace scvb::engine
{

// 快照 flags 位定义(03 §3.1)。
enum PlayheadFlag : uint32_t
{
    kPlayheadIsPlaying = 1u << 0,
    kPlayheadIsLooping = 1u << 1,
    kPlayheadCycleValid = 1u << 2,
    kPlayheadTempoValid = 1u << 3,
    kPlayheadMusicValid = 1u << 4,
};

// POD 载荷,一次性整体发布。字段并集已含 04 的 range 跟随 / tempo 采样点表需求。
struct PlayheadPod
{
    int64_t timeSamples = -1; // 本 block 起始 timeline 位置(规范保证 always valid)
    double ppq = 0.0; // kProjectTimeMusicValid 时有效
    double bpm = 0.0; // kTempoValid 时有效
    double loopStartPpq = 0.0; // kCycleValid 时有效
    double loopEndPpq = 0.0;
    double sampleRate = 48000.0;
    uint32_t epoch = 0; // 时间线跳变代数(Output [A] 在 processBlock 按跳变检测递增)
    uint32_t flags = 0; // PlayheadFlag 位组合
};

// seq 版本号包裹的双缓冲快照(进程内,非 IPC)。
struct PlayheadShot
{
    std::atomic<uint32_t> seq{0}; // 写方:写前 +1(奇)→ 写 pod → 写后 +1(偶)
    PlayheadPod pod{}; // 读方:seq 前后双读,奇或不等 → 沿用上帧(不自旋)

    // 音频线程每块整体发布一次(零分配零锁)。绝不在音频线程调 setValueNotifyingHost(R6)。
    void publish(const PlayheadPod& p) noexcept
    {
        seq.fetch_add(1, std::memory_order_release); // 奇数:进入临界区
        pod = p;
        seq.fetch_add(1, std::memory_order_release); // 偶数:发布完成
    }

    // 读方:返回 false = 本次读撕裂(写者正在写或读期间更新),调用方沿用上帧。
    bool read(PlayheadPod& out) const noexcept
    {
        const uint32_t before = seq.load(std::memory_order_acquire);
        if ((before & 1u) != 0u)
            return false; // 写者正在写

        out = pod;

        // 保证 pod 的读取不被移到第二次 seq 读之后(seqlock 读边界)。
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t after = seq.load(std::memory_order_relaxed);
        return after == before; // 读期间被更新 → 撕裂
    }
};

static_assert(std::atomic<uint32_t>::is_always_lock_free, "PlayheadShot.seq 必须无锁(CLAUDE.md §8)");

} // namespace scvb::engine
