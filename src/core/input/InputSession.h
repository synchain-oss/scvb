// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// InputSession —— Input 实例的 IPC 会话(claim/接管/心跳/段创建/改组/健康判定,01 §4.1)。
// JUCE-free,可离线单测(用 SegmentBackendInProcess 模拟多实例;跨进程真实行为归 T07b)。
//
// channel_id==0 → I6 UNASSIGNED:不 claim、不创建 audio.chN/feat.chN、不发心跳(01 §4 I5/I6;
// 03 §7.2)。这正是 [J01] 要消除的「十五个 Input 齐插连环 CAS 冲突」场景的兜底。
// 健康判定(ipc v1 [J12 联动])= 本实例 I4 ACTIVE ∧ 本组 OutputSlot 心跳新鲜(2000ms 口径)
//   ∧ connected_mask 含本 channel。改组(J66)= 释放旧组 slot → Unmap 旧组段 → 新组重走 claim。
//
// 所有段操作只发生在持 lifecycleMutex 的消息线程([M] 或宿主生命周期回调,01 §3.1 R1)。
//
// 音频线程同步模型(PR#51 红旗修复,T16 Snapshot 同款):
//   音频线程每 block 调 acquireBlock() —— acquire-load 一次不可变绑定快照(AudioRing/FeatRing 的
//   atomic<const Binding*>)+ 取 registry/audio/feat 三段 SegmentHandle 租约(引用计数,持有期内
//   [M] release() 不解映射),整 block 复用同一份视图;旧绑定由 AudioRing/FeatRing 内部 owned_ 保活
//   (进程寿命),绑定内裸指针指向的段由租约保证块内有效。音频线程绝不直接触碰 registry_/audioHandle_/
//   featHandle_ 的可变成员,也不读 claimedChannel_(经块视图 channel 快照)。

#include <atomic>
#include <cstdint>
#include <vector>

#include "ipc/AudioRing.h"
#include "ipc/FeatRing.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/Registry.h"

namespace scvb::input
{

// [J66] group_id 默认 1(UI 显示 A)。
inline constexpr u32 kInputDefaultGroup = 1;

// Input 实例的 IPC 状态(01 §4.1 状态机)。
enum class InputClaimState
{
    kUnassigned, // I6:channel_id==0,不 claim/不建段/不发心跳
    kActive, // I4:claim 成功
    kConflict, // I2:被活跃实例占用(不满足接管双条件)
    kAbiMismatch, // I1:registry abi 不符(拒连,J40)
    kUnavailable // I0:段未打开/映射失败
};

// 音频线程每 block acquire 一次的不可变视图:绑定快照 + 段租约(持有期内段不解映射)。
struct InputSessionBlockView
{
    const AudioRingBinding* audio = nullptr; // 音频环绑定(可为 null → 不写环)
    u32 channel = 0; // claimed channel 快照(0 = 未绑定)
    InputSlot* registrySlot = nullptr; // 经 registryLease 基址 + 冻结偏移快照(PR#51 第3轮红旗)
    // [SL-254] 同一条租约下的 OutputSlot(只读):非实时下 [A] 逐块读 connected_mask 判静音。
    OutputSlot* outputSlot = nullptr;
    SegmentHandle::Lease registryLease; // registry 段租约
    SegmentHandle::Lease audioLease; // audio 段租约
    SegmentHandle::Lease featLease; // feat 段租约
};

// T30 桥 scvb.conn 六字段快照([M] 采集;契约 §4.2)。IPC 可推的四字段由 connSnapshot 填,
// passthrough/passthroughPending 由输出级仲裁方(InputProcessor 的 StageSwitchStateMachine)回填。
struct InputConnSnapshot
{
    bool outputOnline = false; // 本组 OutputSlot 活跃且心跳 ≤2000ms(J66 本组语义)
    bool maskBit = false; // 本组 connected_mask 中本 channel 位
    bool capturing = false; // 本实例 InputSlot.flags bit0
    bool passthrough = true; // 当前音频路径:true=直通,false=静音转发(已接管)
    bool passthroughPending = false; // 「静音→直通」5s 滞回窗口内(J32)
    std::uint16_t occupiedMask = 0; // 本组 15 个 InputSlot 心跳新鲜占用位图(bit0=ch1;含本实例)
};

class InputSession
{
public:
    InputSession(ISegmentBackend& backend, u32 pid);
    ~InputSession();

    InputSession(const InputSession&) = delete;
    InputSession& operator=(const InputSession&) = delete;

    void setChannelId(u32 ch) noexcept { channelId_ = (ch <= kMaxChannels) ? ch : 0; }
    u32 channelId() const noexcept { return channelId_; }
    void setGroupId(u32 g) noexcept { groupId_ = (g >= 1 && g <= kMaxGroups) ? g : kInputDefaultGroup; }
    u32 groupId() const noexcept { return groupId_; }
    u32 pid() const noexcept { return pid_; }

    // prepare(消息线程/生命周期回调,持 lifecycleMutex):依 channel_id/group_id 走 claim 或 I6。
    // channels=1|2([J57],由宿主布局判定)。返回 claim 态。
    // 已 active 且同 channel+group → 仅当 SR/声道布局变化才重建环头(epoch+1)+ 重备 extractor。
    InputClaimState prepare(u32 sampleRate, u32 maxBlock, u32 channels, u64 nowMs);

    // [M] 4Hz 心跳(kActive 才写)。
    void heartbeat(u64 nowMs);

    // [M] muted 确认位(C19,J32):静音 ramp 完成置位、切直通前清位(fetch_or/fetch_and,[J48])。
    void setMuted(bool muted);

    // [A] capturing 位(C17):采集门控,一律 fetch_or/fetch_and([J48],禁 load-modify-store)。
    // slot 来自 acquireBlock() 的块视图快照(经 registry 租约基址 + 冻结偏移寻址,音频线程
    // 绝不裸读 registry_ 可变 header_,PR#51 第3轮红旗);调用方须持块视图租约(段保活)。
    void setCapturing(InputSlot* slot, bool capturing);

    // [M] 25Hz 健康判定([J12]):见文件头。claim 未就绪/OutputSlot 空 → false(直通)。
    bool isHealthy(u64 nowMs) const;

    // [M] 25Hz 延迟释放回收([M] 心跳/轮询周期调用):registry + audio/feat 段句柄。
    void reap(u64 nowMs);
    std::size_t pendingReleaseCount() const { return registry_.pendingReleaseCount() + pendingSegments_.size(); }

    // 改组(J66):释放旧组 slot → Unmap 旧组段 → 新组 registry 重走 claim。返回新组 claim 态;
    // 期间输出走直通档(由调用方经 StageSwitchStateMachine::forcePassthrough 保证)。
    InputClaimState changeGroup(u32 newGroup, u32 sampleRate, u32 maxBlock, u32 channels, u64 nowMs);

    // 释放(析构/releaseResources,消息线程):释放 slot + Unmap 段。registry 段保持映射(进程寿命内复用)。
    void release(u64 nowMs);

    // 音频线程每 block 调用:acquire 绑定快照 + 取段租约(见文件头同步模型)。
    InputSessionBlockView acquireBlock() const;

    // 音频线程访问(仅 active 后非空/非绑定)。
    AudioRing& audioRing() noexcept { return audioRing_; }
    FeatRing& featRing() noexcept { return featRing_; }
    u32 boundChannel() const noexcept { return claimedChannel_.load(std::memory_order_acquire); }
    InputClaimState state() const noexcept { return state_; }

    // --- T30 桥只读快照入口([M],持 lifecycleMutex;只读共享内存原子,绝不触碰音频线程成员)---
    u32 localAbi() const noexcept { return kScvbAbi; } // 本机 SCVB abi(= RegistryHeader.abi 同源)
    u32 remoteAbi() const noexcept { return registry_.remoteAbi(); } // abi 不符时探测到的对端 abi
    u32 configSeq() const { return registry_.configSeq(); } // 本组 OutputSlot.config_seq(§4.3 变化检测)
    InputConnSnapshot connSnapshot(u64 nowMs) const; // §4.2 的 IPC 四字段 + occupiedMask
    std::uint8_t groupsOnline(u64 nowMs) const; // 本组位(OutputSlot 心跳)+ 跨组只读探测(01 §4.5/J70)

private:
    bool openAndClaim(u32 sampleRate, u32 maxBlock, u32 channels, u64 nowMs);
    bool createSegments(u32 sampleRate, u32 channels);
    void rebuildAudioGeometry(u32 sampleRate, u32 channels);
    void releaseSegments();
    void releaseHandle(SegmentHandle& handle);
    void releaseSlot();

    ISegmentBackend& backend_;
    u32 pid_;
    u32 channelId_ = 0;
    u32 groupId_ = kInputDefaultGroup;
    std::atomic<u32> claimedChannel_{0}; // [M] 写 / [A] 经块视图 acquire-load
    InputClaimState state_ = InputClaimState::kUnassigned;

    // 上次 prepare 的几何(用于判断 re-prepare 是否真发生 SR/布局变化)。
    u32 lastSampleRate_ = 0;
    u32 lastChannels_ = 0;

    Registry registry_;
    AudioRing audioRing_;
    FeatRing featRing_;
    SegmentHandle audioHandle_;
    SegmentHandle featHandle_;
    std::vector<SegmentHandle> pendingSegments_; // 延迟释放(租约在途/宽限期未满)的段句柄,[M] reap 回收
};

} // namespace scvb::input
