// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

// MeterShot —— 音频线程([A])→ 消息线程([M]) 的电平快照通道(契约 §2.5 scvb.meters 的数据面)。
// 协议与 engine::PlayheadShot 逐条同款:单写单读、seq 奇偶双读、撕裂即沿用上帧(不自旋)。
// 纯 C++17,无 JUCE(ADR-011:core 不得依赖 JUCE)。
//
// 【为什么发线性值而不是 dB】dB 换算是 32 次 log10;放在 [A] 是白白占实时预算,而 [M] 只有 25Hz。
// 故 [A] 只做加法/乘法/取绝对值(块内一遍扫描),log10 一律留给 emitMeters。
//
// 【口径】per-track 电平 = **post-gain / pre-pan**:源样本 × 该轨 volDb 线性增益。pan 只在 L/R
// 之间重新分配能量,不改变「这一轨有多响」,所以轨道液柱不吃 pan;总线 L/R 则取求和替换后的
// accum(即真正送出去的信号)。增益取本块起点的仲裁目标值(逐样本平滑的差异对电平表不可见)。
namespace scvb::output
{

inline constexpr int kMeterTracks = 15;

// POD 载荷,一次性整体发布。全部为**线性幅度**(非 dB),0 = 静音。
struct MeterPod
{
    float trackRms[kMeterTracks] = {}; // 块内 RMS(post-gain)
    float trackPeak[kMeterTracks] = {}; // 块内峰值(post-gain)
    float busRms[2] = {}; // 总线 L/R 块内 RMS(替换总线后的实际输出)
    float busPeak[2] = {}; // 总线 L/R 块内峰值
};

struct MeterShot
{
    std::atomic<std::uint32_t> seq{0}; // 写方:写前 +1(奇)→ 写 pod → 写后 +1(偶)
    MeterPod pod{}; // 读方:seq 前后双读,奇或不等 → 沿用上帧(不自旋)

    // 音频线程每块整体发布一次(零分配零锁)。
    void publish(const MeterPod& p) noexcept
    {
        seq.fetch_add(1, std::memory_order_release); // 奇数:进入临界区
        pod = p;
        seq.fetch_add(1, std::memory_order_release); // 偶数:发布完成
    }

    // 读方:返回 false = 本次读撕裂(写者正在写或读期间更新),调用方沿用上帧。
    bool read(MeterPod& out) const noexcept
    {
        const std::uint32_t before = seq.load(std::memory_order_acquire);
        if ((before & 1u) != 0u)
        {
            return false; // 写者正在写
        }

        out = pod;

        // 保证 pod 的读取不被移到第二次 seq 读之后(seqlock 读边界)。
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint32_t after = seq.load(std::memory_order_relaxed);
        return after == before; // 读期间被更新 → 撕裂
    }
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "MeterShot.seq 必须无锁(CLAUDE.md §8)");

} // namespace scvb::output
