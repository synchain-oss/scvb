// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Registry —— registry 段的生命周期状态机(claim / 接管 / 心跳 / generation,01 §4.0–§4.3)。
// 组内语义(ipc v1.5 [J66]):构造带 group(1..8),claim/接管/心跳/connected_mask/config_seq
// 全部作用于本组 g{G}.registry 段;跨组互不可见。所有段操作只在持 lifecycleMutex 的消息线程
// 执行([M] 或宿主生命周期回调);音频线程只拿映射后的裸指针做原子读写。
//
// 段初始化协议(created/attach/覆盖式重初始化)复用 ISegmentBackend::initHeader(T06 已实现,
// 勿重写)。双阈值([J10]):kStaleDisplayMs=2000 仅驱动 UI「失联」显示;kTakeoverMs=5000 且
// 接管前必须 pid 探活失败(双条件,保护 SPSC 单写前提)。pid 探活函数只在消息线程调用,
// 禁止音频线程。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ISegmentBackend.h"
#include "SegmentLayout.h"

namespace scvb
{

// 双阈值陈旧判定(ipc v1 [J10])。
inline constexpr u64 kStaleDisplayMs = 2000; // 仅 UI「失联」显示
inline constexpr u64 kTakeoverMs = 5000; // 接管需 ≥5000ms 且 pid 探活失败

// 心跳写入节奏:4Hz = 250ms([M] 定时器按此判定是否到写点)。
inline constexpr u64 kHeartbeatIntervalMs = 250;

// 幽灵槽接管宽限期:claim 者 CAS 后、fill 前崩溃 → state 卡 kSlotClaimed(pid=0/hb=0)。
// 合法 claim 是微秒级,1s 远大于任何在途窗口;同一槽持续该状态超过此宽限期才允许接管。
inline constexpr u64 kClaimGhostGraceMs = 1000;

// registry 段布局偏移(与 SegmentLayout.h 的 static_assert 对齐)。
inline constexpr std::size_t kRegistrySegmentSize = 4096;
inline constexpr std::size_t kInputSlotOffset = sizeof(RegistryHeader);
inline constexpr std::size_t kOutputSlotOffset = sizeof(RegistryHeader) + kMaxChannels * sizeof(InputSlot);

// 真实 pid 探活(Windows:OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)+GetExitCodeProcess)。
// 探活失败(句柄拿不到/退出码非 STILL_ACTIVE)一律判「死」——J10「pid 存活探测失败」条件。
// 只在消息线程调用,禁止音频线程(OpenProcess 是系统调用)。
bool isProcessAlive(u32 pid) noexcept;

// 显示陈旧(>2000ms,仅 UI 显示口径)。
inline bool isStaleDisplay(u64 heartbeatMs, u64 nowMs) noexcept
{
    return heartbeatMs == 0 || nowMs - heartbeatMs > kStaleDisplayMs;
}

// 接管条件之一:心跳陈旧 ≥5000ms(另一条件是 pid 探活失败,由调用方组合)。
inline bool isTakeoverStale(u64 heartbeatMs, u64 nowMs) noexcept
{
    return heartbeatMs == 0 || nowMs - heartbeatMs >= kTakeoverMs;
}

// v9:Input 通道解析策略(env > state > 已认领保持 > 自动 0)。
// 已认领的自动模式实例跨宿主 re-prepare 必须保持原 channel:若每次被 state 默认值打回 0,
// auto 路径会改抢最低空闲槽 → 实例迁槽、旧槽心跳陈旧但 state 仍 Active(幽灵槽)。
inline u32 resolveInputChannelForClaim(u32 envCh, u32 stateCh, bool claimed, u32 currentCh) noexcept
{
    if (envCh >= 1 && envCh <= kMaxChannels)
    {
        return envCh;
    }
    if (stateCh >= 1 && stateCh <= kMaxChannels)
    {
        return stateCh;
    }
    if (claimed && currentCh >= 1 && currentCh <= kMaxChannels)
    {
        return currentCh;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// AudioRingHeader 几何字段快照纪律(供 T23/T24 环读写实现引用;几何 = sample_rate / ring_frames /
// channels 三个 plain u32,非原子)。两条硬性纪律:
// 1. 几何只在 attach 时读一次,快照进本地(不可分快照对象/结构),后续每块只读本地快照,绝不每块
//    重读段头几何字段。段头几何与视图必须作为同一快照原子发布,否则宿主编排(mono⇄stereo 重建环)
//    会让读/写线程读到「新几何 + 旧视图」或「旧几何 + 新视图」的撕裂组合 → 越界写共享内存
//    (0xC0000005;S1 v5 AudioRingGeometry 快照教训)。
// 2. 段恒按 stereo 容量创建(ring_frames × 2 float),声道切换(1→2)不扩容段——mono 只用
//    ring_frames×1 的前半,stereo 用满容量;几何 channels 字段在 prepareToPlay 写定后运行期不变。
//    任何依赖「切换声道时重开更大段」的假设都违反此纪律(ring_frames 口径 = 帧数,不是样本数,
//    见 SegmentLayout.h AudioRingHeader 注释)。
// ---------------------------------------------------------------------------

class Registry
{
public:
    enum class ClaimResult
    {
        kClaimed, // 本实例取得所有权(state=2);open() 成功也返回此值(见 open 注释)
        kConflict, // 被活跃实例占用(不满足接管双条件)
        kUnavailable, // 段未打开/映射失败
        kAbiMismatch // abi 不符(拒连,J40)
    };

    // pid 探活注入源(默认真实 isProcessAlive;测试注入可控 lambda 模拟存活/死亡)。
    using ProcessProbe = std::function<bool(u32 pid)>;
    // abi 不符横幅回调(J40):参数(localAbi, remoteAbi)。消息线程触发,绝不崩溃、绝不半兼容。
    using AbiMismatchHandler = std::function<void(u32 localAbi, u32 remoteAbi)>;

    // 组号 G 为构造参数(1..8,ipc v1.5 [J66]);backend 注入以便 InProcess 测试后端替换。
    explicit Registry(ISegmentBackend& backend, u32 group, ProcessProbe probe = &isProcessAlive);
    ~Registry();

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    void setAbiMismatchHandler(AbiMismatchHandler handler) { abiMismatchHandler_ = std::move(handler); }

    // 映射 + §4.0 初始化(消息线程)。返回 kClaimed 表示「registry 已映射并完成 §4.0 初始化」
    // (不代表已 claim 某 slot,claim 由调用方随后执行)。
    ClaimResult open();

    bool isOpen() const { return header_ != nullptr; }
    u32 group() const { return group_; }
    u32 generation() const; // RegistryHeader.generation(覆盖式重初始化 +1)
    u32 abi() const; // 本段 abi(连接成功后 = kScvbAbi)
    // 最近一次 abi 不符时探测到的对端 abi(0 = 未探测到;open()/changeGroup() 失败路径记录)。
    // T30 桥 abi_remote 来源(scvb.state.abi_remote;探测不到则字段不存在)。
    u32 remoteAbi() const noexcept { return remoteAbi_; }

    RegistryHeader* header() { return header_; }
    // 返回指向共享内存的可变指针(const 方法:Registry 的 const 与共享内存内容无关)。
    InputSlot* inputSlot(u32 channel) const; // channel ∈ [1,15],非法/未打开 → nullptr
    OutputSlot* outputSlot() const;

    // 音频线程经租约基址寻址 InputSlot(不读可变 header_,PR#51 第3轮红旗):布局冻结
    // (kInputSlotOffset = sizeof(RegistryHeader)),base 由调用方 SegmentHandle 租约持有保证有效。
    static InputSlot* inputSlotAtBase(void* base, u32 channel) noexcept
    {
        if (base == nullptr || channel == 0 || channel > kMaxChannels)
        {
            return nullptr;
        }
        auto* p = static_cast<char*>(base);
        return reinterpret_cast<InputSlot*>(p + kInputSlotOffset + (channel - 1) * sizeof(InputSlot));
    }

    // 音频线程按块租约(引用计数握手释放):块首 lease(),块末析构归还;持有期内保证 registry
    // 段不解映射(消息线程 release() 会推迟到 leaseCount 归零)。release() 请求后返回空租约。
    SegmentHandle::Lease lease() const { return handle_.lease(); }

    // claim 一个 Input channel(消息线程;CAS 0→1 填字段→2;被占且满足接管双条件则接管)。
    ClaimResult claimInput(u32 channel, u32 pid, u32 sampleRate, u32 maxBlock, u64 nowMs);
    // 自动认领专用(v8/v10):只抢 Free 槽或陈旧+死 pid 接管;**不做同 pid 刷新/接管**
    // (同 pid 刷新会让复制轨道带来的同 channel 复制体抢走原实例的槽 → 双写同环)。
    ClaimResult claimInputAuto(u32 channel, u32 pid, u32 sampleRate, u32 maxBlock, u64 nowMs);
    // v10:仅刷新【本实例自己持有】的 slot 字段(采样率切换重认领),非属主调用为空操作。
    // 仅 re-prepare 且音频停摆时调用(采样率切换重认领路径);音频运行中不得调(改几何不重建环)。
    void updateOwnedInputSlot(u32 channel, u32 pid, u32 sampleRate, u32 maxBlock);
    // 释放本实例占用的 Input channel(析构/改组路径,消息线程)。
    void releaseInput(u32 channel, u32 pid);

    // Output 占用 OutputSlot(同语义,ADR-002 第二个 Output 只读观察 = 同组内语义)。
    ClaimResult claimOutput(u32 pid, u64 nowMs);
    void releaseOutput(u32 pid);

    // 心跳写(消息线程 4Hz 定时器)。
    void heartbeatInput(u32 channel, u64 nowMs);
    void heartbeatOutput(u64 nowMs);

    // connected_mask 维护(Output [M];bit{N-1} = channel N 健康;[J48] 位操作 fetch_or/fetch_and)。
    u32 connectedMask() const;
    void setConnectedMaskBit(u32 channel);
    void clearConnectedMaskBit(u32 channel);
    void clearConnectedMask(); // 停摆看门狗/时间线健康前置清空(整字 store)

    // config_seq 维护(Output [M] 单调递增 API + Input 变化检测)。
    u32 configSeq() const;
    u32 bumpConfigSeq(); // fetch_add(1) 单调递增,返回新值
    bool configChanged(u32& lastSeenSeq) const; // Input 变化检测:比较并更新 lastSeenSeq

    // 改组(J66):释放本实例持有的 slot(state→0)→ 卸载本组 registry 段 → 换新组 → 重新映射+
    // §4.0 初始化。返回新组的 open 结果;claim 由调用方随后执行(01 §4.1/§4.2 改组转移)。
    ClaimResult changeGroup(u32 newGroup);

    // 延迟释放回收([M] 心跳/轮询周期调用,文档注明调用点):对 pendingReleases_ 逐项 release(nowMs),
    // 成功(租约归零且宽限期届满)即移除并解映射。pr-agent 复审:open()/changeGroup() 时 release() 返回
    // false 的旧句柄压入 pendingReleases_,否则旧 mapping 无人再 release → 永久泄漏。
    void reapPendingReleases(u64 nowMs);
    std::size_t pendingReleaseCount() const { return pendingReleases_.size(); }

    // 本实例持有资源状态(供 changeGroup/析构与测试断言)。
    u32 ownedInputChannel() const { return ownedChannel_; }
    bool ownsOutput() const { return ownsOutput_; }
    u32 ownedPid() const { return ownedPid_; }

private:
    void releaseOwnedSlot() noexcept;
    static void fillInputSlot(InputSlot& s, u32 pid, u32 sampleRate, u32 maxBlock, u64 nowMs);

    ISegmentBackend& backend_;
    u32 group_ = 1;
    ProcessProbe probe_;
    AbiMismatchHandler abiMismatchHandler_;
    SegmentHandle handle_; // 引用计数句柄:析构/改组经 release() 握手释放(消息线程)
    std::vector<SegmentHandle> pendingReleases_; // 延迟释放(租约在途)的旧句柄,[M] reapPendingReleases 回收
    RegistryHeader* header_ = nullptr;

    // 本实例持有的资源(changeGroup/析构释放用)。
    u32 ownedChannel_ = 0; // 0 = 未持有 Input slot
    bool ownsOutput_ = false;
    u32 ownedPid_ = 0;

    // 幽灵槽本地观测时钟(消息线程):首见 kSlotClaimed+pid==0+hb==0 时记录 nowMs;state 变化即清。
    u64 ghostSeenMs_[kMaxChannels] = {};
    u64 outputGhostSeenMs_ = 0;

    u32 remoteAbi_ = 0; // 最近一次 abi 不符时探测到的对端 abi(0 = 未探测到;T30 桥来源)
};

} // namespace scvb
