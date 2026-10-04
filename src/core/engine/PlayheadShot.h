// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

// PlayheadShot:音频线程([A])→ 消息线程([M]) 的 playhead 快照通道唯一真源定义
// (03 §3.1 / 01 C8 / 04 §2.2)。单写(音频线程)单读(消息线程),最新值语义。
// 「单写」指**同一时刻只有一个写方**:Output 的 releaseResources 也补发一帧([SL-527]),
// 靠的是宿主不让它与 processBlock 并发;新增写点同样要满足这一条。常态下:
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
    kPlayheadTimeSigValid = 1u << 5, // [J147] 宿主给了拍号(timeSigNum/timeSigDen 有效)
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
    int32_t timeSigNum = 0; // [J147] kTimeSigValid 时有效(拍号分子)
    int32_t timeSigDen = 0; // [J147] kTimeSigValid 时有效(拍号分母)
};

// seq 版本号包裹的双缓冲快照(进程内,非 IPC)。
struct PlayheadShot
{
    std::atomic<uint32_t> seq{0}; // 写方:写前 +1(奇)+ release fence → 写 pod → 写后 +1(偶)
    PlayheadPod pod{}; // 读方:seq 前后双读,奇或不等 → 沿用上帧(不自旋)

    // 音频线程每块整体发布一次(零分配零锁)。绝不在音频线程调 setValueNotifyingHost(R6)。
    // [B 线 M04] 写侧是标准 Boehm 写法(同 CtrlPlane::writeBroadcast):奇数增量 relaxed + 紧跟一道
    // release fence。原先奇数增量是 release RMW,release 只约束它**之前**的访问,挡不住后面的 pod 写
    // 被提到奇数 seq 之前可见 —— arm64 上读方可能读到前后两次 seq 都是同一个偶数、pod 却已经半新半旧
    // (消费方是 AutomationPrinter / Monitor / UI,撕裂一帧就是一个错误的时间位置或 epoch)。
    // fence 之后的 pod 写一旦被读方读到,本 fence 与读方 read() 里的 acquire fence 同步 ⇒ 奇数 seq
    // 对读方第二次 seq 读可见 ⇒ before != after,撕裂被识别。
    // x86:fetch_add 不论 relaxed / release 都是 lock xadd(本身即全屏障),release fence 只是编译器
    // 屏障、不生成指令,所以 x86 上本来就不会撕裂,这里也不多一条指令。arm64:LDADDL 换成
    // LDADD + DMB ISH,每块一次。
    void publish(const PlayheadPod& p) noexcept
    {
        seq.fetch_add(1, std::memory_order_relaxed); // 奇数:进入临界区
        std::atomic_thread_fence(std::memory_order_release); // 挡住后面的 pod 写上浮到奇数 seq 之前
        pod = p;
        seq.fetch_add(1, std::memory_order_release); // 偶数:发布完成(release:pod 写不下沉到它之后)
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

// [J147] 宿主速度 / 拍号 / 拍位置 → 桥事件 `scvb.playhead` 的四个可选字段(契约 §2.6)。
// 纯函数,只读一份已经读出来的 pod([M] 侧调用;音频线程只管 publish,不调它)。
// 取值纪律:
//   · `valid` = bpm 与拍号**同时**可用且落在合理域内 —— 页面换算小节两样缺一不可,
//     只给一样等于没给(web 按「秒」显示并明说);
//   · `ppqValid` 另要求 `timeSamples >= 0`:`scvb.playhead.timeS` 在宿主不给时间线时
//     填的是 0.0,那时把 ppq 发出去,页面会拿它与一个假的 0 秒配对当换算锚点。
//   · 同理还要求 `timeRate`(调用方把 timeSamples 换成 timeS 用的那个采样率)> 0 且等于本帧
//     发布时的 `sampleRate`(PR #325 复审):插件停用后处理器采样率回 0,`timeS` 被填成 0.0,
//     而补发的那一帧里 ppq 仍是真实位置 —— 同一个假锚点的另一条来路。
struct HostTempo
{
    bool valid = false;
    double bpm = 0.0;
    int32_t timeSigNum = 0;
    int32_t timeSigDen = 0;
    bool ppqValid = false;
    double ppq = 0.0;
};

inline constexpr double kHostTempoMaxBpm = 999.0; // 宿主给出超过它的值按「没给」处理
inline constexpr int32_t kHostTimeSigMax = 64; // 拍号分子/分母上界(同上)

inline HostTempo hostTempoOf(const PlayheadPod& p, double timeRate) noexcept
{
    HostTempo t;
    const bool tempoOk =
        (p.flags & kPlayheadTempoValid) != 0u && std::isfinite(p.bpm) && p.bpm > 0.0 && p.bpm <= kHostTempoMaxBpm;
    const bool meterOk = (p.flags & kPlayheadTimeSigValid) != 0u && p.timeSigNum >= 1 &&
                         p.timeSigNum <= kHostTimeSigMax && p.timeSigDen >= 1 && p.timeSigDen <= kHostTimeSigMax;
    if (!tempoOk || !meterOk)
        return t;
    t.valid = true;
    t.bpm = p.bpm;
    t.timeSigNum = p.timeSigNum;
    t.timeSigDen = p.timeSigDen;
    if ((p.flags & kPlayheadMusicValid) != 0u && p.timeSamples >= 0 && std::isfinite(p.ppq) && timeRate > 0.0 &&
        timeRate == p.sampleRate)
    {
        t.ppqValid = true;
        t.ppq = p.ppq;
    }
    return t;
}

} // namespace scvb::engine
