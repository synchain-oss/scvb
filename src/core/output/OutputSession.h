// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OutputSession —— Output 实例的 IPC 会话(claim/接管/心跳/attach 环/per-channel 判定/改组,01 §4.2)。
// JUCE-free,可离线单测(用 SegmentBackendInProcess 模拟多实例;跨进程真实行为归 T07b)。
//
// 状态机:O0/O1 未映射/abi 不符(总线直通)、O2 ACTIVE(claim 本组 OutputSlot,读本组环混音)、
// O3 OBSERVER(同组 OutputSlot 已被占用 → 总线直通,25Hz 重试 claim/接管)。改组(J66)=
// 释放旧组 OutputSlot → Unmap 旧组段 → 新组 claim。
//
// per-channel 子状态(§4.2,[M] 25Hz 评估):CH_OFFLINE / CH_SR_MISMATCH / CH_SUSPENDED /
// CH_MISALIGNED / CH_ONLINE。CH_ONLINE 才置 connected_mask 位;注入经 [J32] 延迟 ——
// 置位后等该轨 muted 确认位或延迟 ≥200ms(先到者)才置 injectMask 位([A] 只读 injectMask 混音)。
//
// 所有段操作只发生在持 lifecycleMutex 的消息线程([M] 或宿主生命周期回调);音频线程只经
// mixSource()/injectMask() 拿裸指针做原子读写。

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "ipc/CtrlPlane.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/Registry.h"
#include "output/ShmRingMixSource.h"

namespace scvb::output
{

// [J66] group_id 默认 1(UI 显示 A)。
inline constexpr u32 kOutputDefaultGroup = 1;
// [J32] 新 channel 上线的注入延迟(置 connected_mask 位后 ≥200ms 再注入,或等 muted 确认位)。
inline constexpr u64 kInjectDelayMs = 200;
// [01 §4.2] 近 1s 无失准 → 重新在线(mask 位恢复)。
inline constexpr u64 kMisalignRecoverMs = 1000;
// [01 §4.2] write_head 停滞判定(CH_SUSPENDED 软启发式)。
inline constexpr u64 kSuspendStallMs = 500;

// Output 实例的 claim 态(01 §4.2)。
enum class OutputClaimState
{
    kActive, // O2:占据 OutputSlot,读环混音
    kObserver, // O3:同组 OutputSlot 已被占用(只读观察,总线直通)
    kAbiMismatch, // O1:registry/ctrl abi 不符(拒连,J40)
    kUnavailable // O0:段未打开/映射失败
};

class OutputSession
{
public:
    OutputSession(ISegmentBackend& backend, u32 pid);
    ~OutputSession();

    OutputSession(const OutputSession&) = delete;
    OutputSession& operator=(const OutputSession&) = delete;

    // prepare(消息线程/生命周期回调,持 lifecycleMutex):映射 registry + ctrl、claim OutputSlot、
    // attach 活跃 channel 的 audio 环。返回 claim 态。
    OutputClaimState prepare(u32 sampleRate, u32 maxBlock, u64 nowMs);

    // [M] 4Hz 心跳(kActive 才写)。
    void heartbeat(u64 nowMs);

    // [M] 25Hz 主循环:attach 缺失环 → per-channel 判定 → connected_mask/injectMask → 停摆看门狗
    // → ctrl 全局小节刷新(250ms)→ 命令环消费 → 延迟释放回收;O3 时重试 claim。
    void tick(u64 nowMs);

    // [M] 延迟释放回收(registry + ctrl + audio 段句柄)。
    void reap(u64 nowMs);
    std::size_t pendingReleaseCount() const { return registry_.pendingReleaseCount() + pendingSegments_.size(); }

    // 改组(J66):释放旧组 OutputSlot → Unmap 旧组段 → 换新组 → 重走 claim。返回新组 claim 态。
    OutputClaimState changeGroup(u32 newGroup, u32 sampleRate, u32 maxBlock, u64 nowMs);

    // 释放(析构/releaseResources,消息线程):释放 slot + Unmap 段。
    void release(u64 nowMs);

    // 音频线程访问([A])。channel ∈ [1,15];非法返回未绑定源。
    ShmRingMixSource& mixSource(u32 channel);
    const ShmRingMixSource& mixSource(u32 channel) const;
    u32 injectMask() const noexcept { return injectMask_.load(std::memory_order_acquire); }

    // 状态访问。
    void setCaptureEnabled(bool on) noexcept { captureEnabled_.store(on ? 1u : 0u, std::memory_order_relaxed); }
    void setOutputEnabled(bool on) noexcept { outputEnabled_.store(on ? 1u : 0u, std::memory_order_relaxed); }
    bool captureEnabled() const noexcept { return captureEnabled_.load(std::memory_order_relaxed) != 0; }
    bool outputEnabled() const noexcept { return outputEnabled_.load(std::memory_order_relaxed) != 0; }
    void setGroupId(u32 g) noexcept { groupId_ = (g >= 1 && g <= kMaxGroups) ? g : kOutputDefaultGroup; }
    u32 groupId() const noexcept { return groupId_; }
    u32 pid() const noexcept { return pid_; }
    u32 sampleRate() const noexcept { return sampleRate_; }
    OutputClaimState state() const noexcept { return state_.load(std::memory_order_acquire); }

    // [M] 聚合用([J09] 全局小节 / 看门狗)。
    u32 gapCount(u32 channel) const;
    u64 writeHead(u32 channel) const;
    u64 epoch(u32 channel) const;

    // [A] 停摆看门狗存活计数([J52]):processBlock 首行 fetch_add。
    void bumpBlockCounter() noexcept { blockCounter_.fetch_add(1, std::memory_order_relaxed); }

    // 时间线健康前置(§4.2 [J51]):[M] 判定连续无时间线 ≥0.5s → 清空 mask(Inputs 走 J12 直通)。
    void forceClearMask() noexcept;
    void forceSetConnectedMaskBit(u32 channel);

private:
    OutputClaimState openAndClaim(u64 nowMs);
    void attachAudioRings();
    void releaseSegments();
    void releaseHandle(SegmentHandle& handle);
    void releaseSlot();
    void evaluateChannels(u64 nowMs);
    void refreshGlobalInfo(u64 nowMs);
    void consumeCommands(u64 nowMs);

    ISegmentBackend& backend_;
    u32 pid_;
    u32 groupId_ = kOutputDefaultGroup;
    u32 sampleRate_ = 0;
    u32 maxBlock_ = 0;
    // claim 态([M] 写 / [A] 经 state() acquire-load;processBlock 据此判 observer)。
    std::atomic<OutputClaimState> state_{OutputClaimState::kUnavailable};
    static_assert(decltype(state_)::is_always_lock_free, "OutputSession::state_ 必须 lock-free(§8 实时线程纪律)");

    Registry registry_;
    CtrlPlane ctrl_;
    std::array<ShmRingMixSource, kMaxChannels> sources_{}; // index = channel-1
    std::array<SegmentHandle, kMaxChannels> audioHandles_{};
    std::vector<SegmentHandle> pendingSegments_;

    // [A] 只读的注入掩码(bit{N-1} = channel N 可注入混音);[M] 25Hz 写。
    std::atomic<u32> injectMask_{0};
    // [A] fetch_add 的存活计数;[M] 读(停摆看门狗)。
    std::atomic<u64> blockCounter_{0};

    // 配置态([M] 写 / [A] 或全局小节读)。
    std::atomic<u32> captureEnabled_{0};
    std::atomic<u32> outputEnabled_{1};

    // [M] per-channel 状态。
    std::array<bool, kMaxChannels> onlinePrev_{};
    std::array<u64, kMaxChannels> onlineSinceMs_{};
    std::array<u64, kMaxChannels> misalignedSinceMs_{};
    std::array<u32, kMaxChannels> lastGapCount_{};
    std::array<u64, kMaxChannels> lastWriteHead_{};
    std::array<u64, kMaxChannels> lastWriteHeadChangeMs_{};

    u64 lastGlobalInfoMs_ = 0;
};

} // namespace scvb::output
