// SPDX-License-Identifier: GPL-3.0-or-later
#include "output/OutputSession.h"

namespace scvb::output
{

namespace
{
std::wstring audioFullName(u32 group, u32 channel)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kAudio, channel);
}

// 文件级空源:音频线程经 mixSource(非法 channel)访问时,不经过 magic-static 守卫(实时线程禁锁)。
ShmRingMixSource gEmptyMixSource;
} // namespace

OutputSession::OutputSession(ISegmentBackend& backend, u32 pid)
    : backend_(backend), pid_(pid), registry_(backend, groupId_), ctrl_(backend, groupId_)
{
}

OutputSession::~OutputSession()
{
    releaseSlot();
    releaseSegments();
}

OutputClaimState OutputSession::prepare(u32 sampleRate, u32 maxBlock, u64 nowMs)
{
    sampleRate_ = sampleRate;
    maxBlock_ = maxBlock;
    return openAndClaim(nowMs);
}

void OutputSession::heartbeat(u64 nowMs)
{
    if (state_.load(std::memory_order_relaxed) == OutputClaimState::kActive)
    {
        registry_.heartbeatOutput(nowMs);
    }
}

OutputClaimState OutputSession::openAndClaim(u64 nowMs)
{
    // 确保 registry 映射到本实例 group_id 所属组(setGroupId 可在首次 prepare 前改组)。
    if (registry_.group() != groupId_)
    {
        const auto gr = registry_.changeGroup(groupId_);
        if (gr == Registry::ClaimResult::kAbiMismatch)
        {
            state_.store(OutputClaimState::kAbiMismatch, std::memory_order_release);
            return state_.load(std::memory_order_relaxed);
        }
        if (gr != Registry::ClaimResult::kClaimed)
        {
            state_.store(OutputClaimState::kUnavailable, std::memory_order_release);
            return state_.load(std::memory_order_relaxed);
        }
    }
    else if (!registry_.isOpen())
    {
        const auto r = registry_.open();
        if (r == Registry::ClaimResult::kAbiMismatch)
        {
            state_.store(OutputClaimState::kAbiMismatch, std::memory_order_release);
            return state_.load(std::memory_order_relaxed);
        }
        if (r != Registry::ClaimResult::kClaimed)
        {
            state_.store(OutputClaimState::kUnavailable, std::memory_order_release);
            return state_.load(std::memory_order_relaxed);
        }
    }

    // ctrl 段:Output 主实例写广播/全局小节、消费命令环(owner 角色)。
    if (ctrl_.group() != groupId_)
    {
        const auto cr = ctrl_.changeGroup(groupId_);
        if (cr == InitResult::kAbiMismatch)
        {
            state_.store(OutputClaimState::kAbiMismatch, std::memory_order_release);
            return state_.load(std::memory_order_relaxed);
        }
        if (cr != InitResult::kOk)
        {
            state_.store(OutputClaimState::kUnavailable, std::memory_order_release);
            return state_.load(std::memory_order_relaxed);
        }
    }
    else if (!ctrl_.isOpen())
    {
        const auto cr = ctrl_.open();
        if (cr == InitResult::kAbiMismatch)
        {
            state_.store(OutputClaimState::kAbiMismatch, std::memory_order_release);
            return state_.load(std::memory_order_relaxed);
        }
        if (cr != InitResult::kOk)
        {
            state_.store(OutputClaimState::kUnavailable, std::memory_order_release);
            return state_.load(std::memory_order_relaxed);
        }
    }

    // 停摆看门狗注入源(blockCounter / write_head / connected_mask)。
    ctrl_.setBlockCounterSource([this] { return blockCounter_.load(std::memory_order_relaxed); });
    ctrl_.setWriteHeadSource([this](u32 ch) { return writeHead(ch); });
    ctrl_.setConnectedMaskSource([this] { return registry_.connectedMask(); });

    const auto cr = registry_.claimOutput(pid_, nowMs);
    if (cr == Registry::ClaimResult::kConflict)
    {
        state_.store(OutputClaimState::kObserver, std::memory_order_release);
        injectMask_.store(0, std::memory_order_release);
        return state_.load(std::memory_order_relaxed);
    }
    if (cr != Registry::ClaimResult::kClaimed)
    {
        state_.store(OutputClaimState::kUnavailable, std::memory_order_release);
        return state_.load(std::memory_order_relaxed);
    }
    state_.store(OutputClaimState::kActive, std::memory_order_release);
    attachAudioRings();
    return state_.load(std::memory_order_relaxed);
}

void OutputSession::attachAudioRings()
{
    // 幂等:只 attach 尚未绑定且 slot 活跃的 channel(attach 失败由 [M] 25Hz 重试)。
    for (u32 ch = 1; ch <= kMaxChannels; ++ch)
    {
        const std::size_t idx = static_cast<std::size_t>(ch - 1);
        if (sources_[idx].bound())
        {
            continue;
        }
        if (audioHandles_[idx].valid())
        {
            continue; // 句柄在途(宽限期),不重复 attach
        }
        InputSlot* slot = registry_.inputSlot(ch);
        if (slot == nullptr || slot->state.load(std::memory_order_acquire) != kSlotActive)
        {
            continue;
        }

        // 只读 attach(01 §4.0):openExisting + initHeader(allowOverwrite=false),严禁写。
        SegmentView av;
        if (backend_.openExisting(audioFullName(groupId_, ch), av) != InitResult::kOk)
        {
            continue;
        }
        auto* ah = static_cast<AudioRingHeader*>(av.base);
        const auto ir = backend_.initHeader(av, &ah->magic, &ah->abi, nullptr, sizeof(AudioRingHeader),
                                            /*initData=*/{}, /*allowOverwrite=*/false);
        if (ir != InitResult::kOk)
        {
            backend_.unmap(av);
            continue;
        }
        float* adata = reinterpret_cast<float*>(ah + 1);
        audioHandles_[idx] = SegmentHandle(std::move(av), &backend_);
        sources_[idx].bind(ah, adata);
    }
}

void OutputSession::evaluateChannels(u64 nowMs)
{
    u32 inject = 0;
    for (u32 ch = 1; ch <= kMaxChannels; ++ch)
    {
        const std::size_t idx = static_cast<std::size_t>(ch - 1);
        InputSlot* slot = registry_.inputSlot(ch);
        const bool slotActive = slot != nullptr && slot->state.load(std::memory_order_acquire) == kSlotActive;
        const bool hbFresh =
            slot != nullptr && !isStaleDisplay(slot->heartbeat_ms.load(std::memory_order_acquire), nowMs);
        const bool srMatch = slot != nullptr && slot->sample_rate == sampleRate_;
        const bool bound = sources_[idx].bound();

        // 失准判定:gapCount 增长 → 记失准时刻(近 1s 内视为 CH_MISALIGNED)。
        const u32 gc = sources_[idx].gapCount();
        if (gc != lastGapCount_[idx])
        {
            misalignedSinceMs_[idx] = nowMs;
            lastGapCount_[idx] = gc;
        }
        const bool misaligned = (nowMs - misalignedSinceMs_[idx]) < kMisalignRecoverMs;

        // CH_SUSPENDED:write_head 完全停滞 ≥0.5s ∧ 心跳新鲜(宿主跳过该轨处理,不计失准)。
        bool suspended = false;
        if (hbFresh && bound)
        {
            const u64 wh = sources_[idx].writeHead();
            if (wh != lastWriteHead_[idx])
            {
                lastWriteHead_[idx] = wh;
                lastWriteHeadChangeMs_[idx] = nowMs;
            }
            else if (nowMs - lastWriteHeadChangeMs_[idx] >= kSuspendStallMs)
            {
                suspended = true;
            }
        }

        const bool online = slotActive && hbFresh && srMatch && bound && !misaligned && !suspended;
        if (online)
        {
            registry_.setConnectedMaskBit(ch);
            if (!onlinePrev_[idx])
            {
                onlineSinceMs_[idx] = nowMs; // 上线时刻(注入延迟起点)
            }
            onlinePrev_[idx] = true;

            // [J32] 注入延迟:等该轨 muted 确认位或延迟 ≥200ms(先到者)才开始注入。
            const u32 flags = slot->flags.load(std::memory_order_acquire);
            const bool muted = (flags & kFlagMuted) != 0;
            if (muted || (nowMs - onlineSinceMs_[idx] >= kInjectDelayMs))
            {
                inject |= (1u << (ch - 1));
            }
        }
        else
        {
            registry_.clearConnectedMaskBit(ch);
            onlinePrev_[idx] = false;
        }
    }
    injectMask_.store(inject, std::memory_order_release);
}

void OutputSession::refreshGlobalInfo(u64 nowMs)
{
    if (nowMs - lastGlobalInfoMs_ < 250)
    {
        return; // 每 250ms 刷新一次([J09])
    }
    lastGlobalInfoMs_ = nowMs;

    OutputGlobalInfoSnapshot s;
    s.capture_enabled = captureEnabled_.load(std::memory_order_relaxed);
    s.output_sample_rate = sampleRate_;
    s.flags = outputEnabled_.load(std::memory_order_relaxed) ? kOutputEnabled : 0;
    for (u32 i = 0; i < kMaxChannels; ++i)
    {
        s.gap_count[i] = sources_[i].gapCount();
        s.overlap_count[i] = 0; // v1:重叠取时间线正确者(后写覆盖先写),不计数(§5.3)
        s.epoch_summary[i] = sources_[i].epoch();
    }
    ctrl_.refreshGlobalInfo(s);
}

void OutputSession::consumeCommands(u64 /*nowMs*/)
{
    // 消费命令环(排空,防满环丢最旧)。op 派发(kSetPriority/kFpReport → state/analysis)归后续任务,
    // 本卡只承担「Output [M] 消费」通道职责(§3.2 C6)。
    CtrlRecord rec;
    for (u32 ch = 1; ch <= kMaxChannels; ++ch)
    {
        while (ctrl_.dequeue(ch, rec))
        {
        }
    }
}

void OutputSession::tick(u64 nowMs)
{
    if (state_.load(std::memory_order_relaxed) == OutputClaimState::kObserver)
    {
        // O3:25Hz 重试 claim / 接管(主实例卸载或心跳陈旧 + pid 探活失败)。
        const auto cr = registry_.claimOutput(pid_, nowMs);
        if (cr == Registry::ClaimResult::kClaimed)
        {
            state_.store(OutputClaimState::kActive, std::memory_order_release);
            attachAudioRings();
        }
        else if (cr == Registry::ClaimResult::kAbiMismatch)
        {
            state_.store(OutputClaimState::kAbiMismatch, std::memory_order_release);
        }
        // PR#53 缺陷2:observer 分支也要 reap —— changeGroup 改到被占 group 时压入 pendingReleases_/
        // pendingSegments_ 的句柄须在宽限期届满后回收,否则反复改组积累共享内存映射直到插件销毁。
        reap(nowMs);
        return; // observer:不写 registry/ctrl、不消费 cmd、不注入(§4.2 O3)
    }

    if (state_.load(std::memory_order_relaxed) != OutputClaimState::kActive)
    {
        return;
    }

    attachAudioRings();
    evaluateChannels(nowMs);

    // 停摆看门狗(§4.2 [J52]):自身 blockCounter 停滞 + 在线轨 write_head 推进 → 清 mask。
    const WatchdogResult wr = ctrl_.tickWatchdog(nowMs);
    if (wr.action == WatchdogAction::kClearMask)
    {
        registry_.clearConnectedMask();
        injectMask_.store(0, std::memory_order_release);
    }
    else if (wr.action == WatchdogAction::kReacquireBit)
    {
        registry_.setConnectedMaskBit(wr.channel);
    }

    refreshGlobalInfo(nowMs);
    consumeCommands(nowMs);
    reap(nowMs);
}

OutputClaimState OutputSession::changeGroup(u32 newGroup, u32 sampleRate, u32 maxBlock, u64 nowMs)
{
    // 释放旧组 OutputSlot → Unmap 旧组段。
    releaseSlot();
    releaseSegments();
    injectMask_.store(0, std::memory_order_release);
    state_.store(OutputClaimState::kUnavailable, std::memory_order_release);

    groupId_ = (newGroup >= 1 && newGroup <= kMaxGroups) ? newGroup : kOutputDefaultGroup;
    sampleRate_ = sampleRate;
    maxBlock_ = maxBlock;

    // 换新组 registry + ctrl + 重走 claim(01 §4.2 改组转移)。
    return openAndClaim(nowMs);
}

void OutputSession::release(u64 nowMs)
{
    releaseSlot();
    releaseSegments();
    injectMask_.store(0, std::memory_order_release);
    state_.store(OutputClaimState::kUnavailable, std::memory_order_release);
    reap(nowMs);
}

void OutputSession::reap(u64 nowMs)
{
    registry_.reapPendingReleases(nowMs);
    ctrl_.reapPendingReleases(nowMs);
    for (auto it = pendingSegments_.begin(); it != pendingSegments_.end();)
    {
        if (it->release(nowMs))
        {
            it = pendingSegments_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

ShmRingMixSource& OutputSession::mixSource(u32 channel)
{
    return (channel >= 1 && channel <= kMaxChannels) ? sources_[static_cast<std::size_t>(channel - 1)]
                                                     : gEmptyMixSource;
}

const ShmRingMixSource& OutputSession::mixSource(u32 channel) const
{
    return (channel >= 1 && channel <= kMaxChannels) ? sources_[static_cast<std::size_t>(channel - 1)]
                                                     : gEmptyMixSource;
}

u32 OutputSession::gapCount(u32 channel) const
{
    if (channel < 1 || channel > kMaxChannels)
    {
        return 0;
    }
    return sources_[static_cast<std::size_t>(channel - 1)].gapCount();
}

u64 OutputSession::writeHead(u32 channel) const
{
    if (channel < 1 || channel > kMaxChannels)
    {
        return 0;
    }
    return sources_[static_cast<std::size_t>(channel - 1)].writeHead();
}

u64 OutputSession::epoch(u32 channel) const
{
    if (channel < 1 || channel > kMaxChannels)
    {
        return 0;
    }
    return sources_[static_cast<std::size_t>(channel - 1)].epoch();
}

void OutputSession::forceClearMask() noexcept
{
    registry_.clearConnectedMask();
    injectMask_.store(0, std::memory_order_release);
}

void OutputSession::forceSetConnectedMaskBit(u32 channel)
{
    registry_.setConnectedMaskBit(channel);
}

void OutputSession::releaseSegments()
{
    for (u32 i = 0; i < kMaxChannels; ++i)
    {
        sources_[i].unbind();
        releaseHandle(audioHandles_[i]);
    }
}

void OutputSession::releaseHandle(SegmentHandle& handle)
{
    if (!handle.valid())
    {
        return;
    }
    if (!handle.release(steadyNowMs()))
    {
        pendingSegments_.push_back(std::move(handle));
    }
}

void OutputSession::releaseSlot()
{
    if (state_.load(std::memory_order_relaxed) == OutputClaimState::kActive)
    {
        registry_.releaseOutput(pid_);
    }
}

} // namespace scvb::output
