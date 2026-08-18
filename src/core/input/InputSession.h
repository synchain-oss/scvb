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
// 所有段操作只发生在持 lifecycleMutex 的消息线程([M] 或宿主生命周期回调,01 §3.1 R1);
// 音频线程只经 audioRing()/featRing() 拿裸指针做原子读写。

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
    // 已 active 且同 channel+group → I5:更新 slot.sample_rate/max_block、重建环头(epoch+1)。
    InputClaimState prepare(u32 sampleRate, u32 maxBlock, u32 channels, u64 nowMs);

    // [M] 4Hz 心跳(kActive 才写)。
    void heartbeat(u64 nowMs);

    // [M] muted 确认位(C19,J32):静音 ramp 完成置位、切直通前清位(fetch_or/fetch_and,[J48])。
    void setMuted(bool muted);

    // [A] capturing 位(C17):采集门控,一律 fetch_or/fetch_and([J48],禁 load-modify-store)。
    void setCapturing(bool capturing);

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

    // 音频线程访问(仅 active 后非空/非绑定)。
    AudioRing& audioRing() noexcept { return audioRing_; }
    FeatRing& featRing() noexcept { return featRing_; }
    u32 boundChannel() const noexcept { return claimedChannel_; }
    InputClaimState state() const noexcept { return state_; }

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
    u32 claimedChannel_ = 0;
    InputClaimState state_ = InputClaimState::kUnassigned;

    Registry registry_;
    AudioRing audioRing_;
    FeatRing featRing_;
    SegmentHandle audioHandle_;
    SegmentHandle featHandle_;
    std::vector<SegmentHandle> pendingSegments_; // 延迟释放(租约在途/宽限期未满)的段句柄,[M] reap 回收
};

} // namespace scvb::input
