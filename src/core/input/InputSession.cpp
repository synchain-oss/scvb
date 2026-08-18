// SPDX-License-Identifier: GPL-3.0-or-later
#include "input/InputSession.h"

namespace scvb::input
{

namespace
{
std::wstring audioFullName(u32 group, u32 channel)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kAudio, channel);
}
std::wstring featFullName(u32 group, u32 channel)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kFeat, channel);
}

// 段恒按 stereo 容量创建(ring_frames × 2 float;01 §5.3 / Registry.h 几何纪律第 2 条)。
std::size_t audioSegmentSize()
{
    return sizeof(AudioRingHeader) + static_cast<std::size_t>(kDefaultRingFrames) * 2 * sizeof(float);
}
std::size_t featSegmentSize()
{
    return sizeof(FeatHeader) + static_cast<std::size_t>(kFeatCapacityHops) * sizeof(FeatFrame);
}
} // namespace

InputSession::InputSession(ISegmentBackend& backend, u32 pid)
    : backend_(backend), pid_(pid), registry_(backend, groupId_)
{
}

InputSession::~InputSession()
{
    releaseSlot();
    releaseSegments();
}

InputClaimState InputSession::prepare(u32 sampleRate, u32 maxBlock, u32 channels, u64 nowMs)
{
    if (channelId_ == 0 || channelId_ > kMaxChannels)
    {
        // I6 UNASSIGNED:不 claim、不建段、不发心跳(01 §4 I5/I6)。
        releaseSlot();
        releaseSegments();
        state_ = InputClaimState::kUnassigned;
        return state_;
    }

    // 已 active 且同 channel+group → I5:更新 slot.sample_rate/max_block、重建环头(epoch+1)。
    if (registry_.isOpen() && claimedChannel_ == channelId_ && registry_.group() == groupId_)
    {
        registry_.updateOwnedInputSlot(channelId_, pid_, sampleRate, maxBlock);
        rebuildAudioGeometry(sampleRate, channels);
        featRing_.prepare(static_cast<double>(sampleRate), static_cast<int>(channels), static_cast<int>(maxBlock));
        state_ = InputClaimState::kActive;
        return state_;
    }

    // 首次/换 channel/换 group:释放旧资源 → 新 claim → 建段。
    releaseSlot();
    releaseSegments();
    state_ = InputClaimState::kUnassigned;
    if (!openAndClaim(sampleRate, maxBlock, channels, nowMs))
    {
        return state_;
    }
    state_ = InputClaimState::kActive;
    return state_;
}

void InputSession::heartbeat(u64 nowMs)
{
    if (state_ == InputClaimState::kActive && claimedChannel_ != 0)
    {
        registry_.heartbeatInput(claimedChannel_, nowMs);
    }
}

void InputSession::setMuted(bool muted)
{
    if (claimedChannel_ == 0)
    {
        return;
    }
    InputSlot* s = registry_.inputSlot(claimedChannel_);
    if (s == nullptr)
    {
        return;
    }
    if (muted)
    {
        s->flags.fetch_or(kFlagMuted, std::memory_order_release);
    }
    else
    {
        s->flags.fetch_and(static_cast<u32>(~kFlagMuted), std::memory_order_release);
    }
}

void InputSession::setCapturing(bool capturing)
{
    if (claimedChannel_ == 0)
    {
        return;
    }
    InputSlot* s = registry_.inputSlot(claimedChannel_);
    if (s == nullptr)
    {
        return;
    }
    if (capturing)
    {
        s->flags.fetch_or(kFlagCapturing, std::memory_order_relaxed);
    }
    else
    {
        s->flags.fetch_and(static_cast<u32>(~kFlagCapturing), std::memory_order_relaxed);
    }
}

bool InputSession::isHealthy(u64 nowMs) const
{
    if (state_ != InputClaimState::kActive || claimedChannel_ == 0)
    {
        return false;
    }
    const OutputSlot* os = registry_.outputSlot();
    if (os == nullptr)
    {
        return false;
    }
    if (os->state.load(std::memory_order_acquire) != kSlotActive)
    {
        return false;
    }
    if (isStaleDisplay(os->heartbeat_ms.load(std::memory_order_acquire), nowMs))
    {
        return false;
    }
    const u32 mask = os->connected_mask.load(std::memory_order_acquire);
    return (mask & (1u << (claimedChannel_ - 1))) != 0;
}

InputClaimState InputSession::changeGroup(u32 newGroup, u32 sampleRate, u32 maxBlock, u32 channels, u64 nowMs)
{
    // 释放旧组 slot → Unmap 旧组段。
    releaseSlot();
    releaseSegments();
    state_ = InputClaimState::kUnassigned;
    claimedChannel_ = 0;

    groupId_ = (newGroup >= 1 && newGroup <= kMaxGroups) ? newGroup : kInputDefaultGroup;

    // 换新组 registry + 重走 claim(01 §4.1 改组转移)。
    if (channelId_ == 0 || channelId_ > kMaxChannels)
    {
        return InputClaimState::kUnassigned;
    }
    if (!openAndClaim(sampleRate, maxBlock, channels, nowMs))
    {
        return state_;
    }
    state_ = InputClaimState::kActive;
    return state_;
}

void InputSession::release(u64 nowMs)
{
    releaseSlot();
    releaseSegments();
    state_ = InputClaimState::kUnassigned;
    claimedChannel_ = 0;
    reap(nowMs);
}

void InputSession::reap(u64 nowMs)
{
    registry_.reapPendingReleases(nowMs);
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

bool InputSession::openAndClaim(u32 sampleRate, u32 maxBlock, u32 channels, u64 nowMs)
{
    // 确保 registry 映射到本实例 group_id 所属组(setGroupId 可在首次 prepare 前改组,而
    // registry_ 构造时用的是初始默认组 —— 组不匹配时必须经 changeGroup 换组再 claim)。
    if (registry_.group() != groupId_)
    {
        const auto gr = registry_.changeGroup(groupId_);
        if (gr == Registry::ClaimResult::kAbiMismatch)
        {
            state_ = InputClaimState::kAbiMismatch;
            return false;
        }
        if (gr != Registry::ClaimResult::kClaimed)
        {
            state_ = InputClaimState::kUnavailable;
            return false;
        }
    }
    else if (!registry_.isOpen())
    {
        const auto r = registry_.open();
        if (r == Registry::ClaimResult::kAbiMismatch)
        {
            state_ = InputClaimState::kAbiMismatch;
            return false;
        }
        if (r != Registry::ClaimResult::kClaimed)
        {
            state_ = InputClaimState::kUnavailable;
            return false;
        }
    }

    const auto cr = registry_.claimInput(channelId_, pid_, sampleRate, maxBlock, nowMs);
    if (cr == Registry::ClaimResult::kConflict)
    {
        state_ = InputClaimState::kConflict;
        return false;
    }
    if (cr != Registry::ClaimResult::kClaimed)
    {
        state_ = InputClaimState::kUnavailable;
        return false;
    }
    claimedChannel_ = channelId_;

    if (!createSegments(sampleRate, channels))
    {
        registry_.releaseInput(claimedChannel_, pid_);
        claimedChannel_ = 0;
        state_ = InputClaimState::kUnavailable;
        return false;
    }
    featRing_.prepare(static_cast<double>(sampleRate), static_cast<int>(channels), static_cast<int>(maxBlock));
    return true;
}

bool InputSession::createSegments(u32 sampleRate, u32 channels)
{
    if (claimedChannel_ == 0)
    {
        return false;
    }

    // audio.chN:几何(sample_rate/ring_frames/channels)在 initData 写定;magic 最后发布。
    SegmentView av;
    if (backend_.createOrOpen(audioFullName(groupId_, claimedChannel_), audioSegmentSize(), av) != InitResult::kOk)
    {
        return false;
    }
    auto* ah = static_cast<AudioRingHeader*>(av.base);
    const auto air = backend_.initHeader(
        av, &ah->magic, &ah->abi, nullptr, sizeof(AudioRingHeader),
        [&] {
            ah->sample_rate = sampleRate;
            ah->ring_frames = kDefaultRingFrames;
            ah->channels = channels;
            ah->write_head_samples.store(0, std::memory_order_release);
            ah->epoch.fetch_add(1, std::memory_order_release); // 换代:读方丢弃旧代数据(01 §4.1)
        },
        /*allowOverwrite=*/true);
    if (air != InitResult::kOk)
    {
        backend_.unmap(av);
        return false;
    }
    float* adata = reinterpret_cast<float*>(ah + 1);
    audioHandle_ = SegmentHandle(std::move(av), &backend_);
    audioRing_.bind(ah, adata);

    // feat.chN。
    SegmentView fv;
    if (backend_.createOrOpen(featFullName(groupId_, claimedChannel_), featSegmentSize(), fv) != InitResult::kOk)
    {
        return false;
    }
    auto* fh = static_cast<FeatHeader*>(fv.base);
    const auto fir = backend_.initHeader(
        fv, &fh->magic, &fh->abi, nullptr, sizeof(FeatHeader),
        [&] {
            fh->hop_ms = kFeatHopMs;
            fh->capacity_hops = kFeatCapacityHops;
            fh->base_hop.store(0, std::memory_order_release);
            fh->write_hop.store(0, std::memory_order_release);
        },
        /*allowOverwrite=*/true);
    if (fir != InitResult::kOk)
    {
        backend_.unmap(fv);
        return false;
    }
    FeatFrame* fdata = reinterpret_cast<FeatFrame*>(fh + 1);
    featHandle_ = SegmentHandle(std::move(fv), &backend_);
    featRing_.bind(fh, fdata, kFeatCapacityHops);

    return audioRing_.bound() && featRing_.bound();
}

void InputSession::rebuildAudioGeometry(u32 sampleRate, u32 channels)
{
    AudioRingHeader* ah = audioRing_.header();
    if (ah == nullptr)
    {
        return;
    }
    // 几何(plain u32)写定 → epoch+1 → write_head 重置 → re-bind 快照(几何与视图同快照发布)。
    ah->sample_rate = sampleRate;
    ah->channels = channels;
    ah->write_head_samples.store(0, std::memory_order_release);
    ah->epoch.fetch_add(1, std::memory_order_release);
    audioRing_.bind(ah, audioRing_.data());
}

void InputSession::releaseSegments()
{
    audioRing_.bind(nullptr, nullptr); // 解绑(防悬垂)
    featRing_.bind(nullptr, nullptr, 0);
    // 段视图经 SegmentHandle 释放(消息线程;租约归零且宽限期届满后 unmap)。
    releaseHandle(audioHandle_);
    releaseHandle(featHandle_);
}

void InputSession::releaseHandle(SegmentHandle& handle)
{
    if (!handle.valid())
    {
        return;
    }
    // 首次 release 记录宽限期起始(返回 false),压入 pendingSegments_ 由 [M] reap 在宽限期届满后解映射。
    if (!handle.release(steadyNowMs()))
    {
        pendingSegments_.push_back(std::move(handle));
    }
}

void InputSession::releaseSlot()
{
    if (claimedChannel_ != 0)
    {
        registry_.releaseInput(claimedChannel_, pid_);
        claimedChannel_ = 0;
    }
}

} // namespace scvb::input
