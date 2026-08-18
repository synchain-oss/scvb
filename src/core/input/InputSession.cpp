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

    // 已 active 且同 channel+group:仅当 SR/声道布局变化才重建环头(epoch+1)+ 重备 extractor。
    if (registry_.isOpen() && claimedChannel_.load(std::memory_order_relaxed) == channelId_ &&
        registry_.group() == groupId_)
    {
        registry_.updateOwnedInputSlot(channelId_, pid_, sampleRate, maxBlock);
        // 纯块长变化不触碰几何/提取器(避免与音频线程并发重置;SR/布局变化属宿主停止音频的重配置)。
        if (sampleRate != lastSampleRate_ || channels != lastChannels_)
        {
            rebuildAudioGeometry(sampleRate, channels);
            featRing_.prepare(static_cast<double>(sampleRate), static_cast<int>(channels), static_cast<int>(maxBlock));
            lastSampleRate_ = sampleRate;
            lastChannels_ = channels;
        }
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
    const u32 ch = claimedChannel_.load(std::memory_order_relaxed);
    if (state_ == InputClaimState::kActive && ch != 0)
    {
        registry_.heartbeatInput(ch, nowMs);
    }
}

void InputSession::setMuted(bool muted)
{
    const u32 ch = claimedChannel_.load(std::memory_order_relaxed);
    if (ch == 0)
    {
        return;
    }
    InputSlot* s = registry_.inputSlot(ch);
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

void InputSession::setCapturing(InputSlot* slot, bool capturing)
{
    // PR#51 第3轮红旗:slot 来自 acquireBlock() 块视图快照(经 registry 租约基址 + 冻结偏移寻址),
    // 音频线程不再裸读 registry_ 可变 header_/handle_ —— 改组瞬间不构成撕裂组合。
    if (slot == nullptr)
    {
        return;
    }
    if (capturing)
    {
        slot->flags.fetch_or(kFlagCapturing, std::memory_order_relaxed);
    }
    else
    {
        slot->flags.fetch_and(static_cast<u32>(~kFlagCapturing), std::memory_order_relaxed);
    }
}

bool InputSession::isHealthy(u64 nowMs) const
{
    const u32 ch = claimedChannel_.load(std::memory_order_acquire);
    if (state_ != InputClaimState::kActive || ch == 0)
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
    return (mask & (1u << (ch - 1))) != 0;
}

InputSessionBlockView InputSession::acquireBlock() const
{
    InputSessionBlockView v;
    // 先取段租约(阻止 [M] 在块内解映射),再 acquire 绑定快照 + channel。
    v.registryLease = registry_.lease();
    v.audioLease = audioHandle_.lease();
    v.featLease = featHandle_.lease();
    v.audio = audioRing_.acquire();
    v.channel = claimedChannel_.load(std::memory_order_acquire);
    // PR#51 第3轮红旗:经租约基址 + 冻结偏移快照 InputSlot*,不读 registry_ 可变 header_。
    // 租约为空(改组已请求释放)→ base 为 null → registrySlot 为 null(setCapturing 空操作)。
    v.registrySlot = Registry::inputSlotAtBase(v.registryLease.base(), v.channel);
    return v;
}

InputClaimState InputSession::changeGroup(u32 newGroup, u32 sampleRate, u32 maxBlock, u32 channels, u64 nowMs)
{
    // 释放旧组 slot → Unmap 旧组段。
    releaseSlot();
    releaseSegments();
    state_ = InputClaimState::kUnassigned;
    claimedChannel_.store(0, std::memory_order_release);

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
    claimedChannel_.store(0, std::memory_order_release);
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
    claimedChannel_.store(channelId_, std::memory_order_release);

    if (!createSegments(sampleRate, channels))
    {
        registry_.releaseInput(channelId_, pid_);
        claimedChannel_.store(0, std::memory_order_release);
        state_ = InputClaimState::kUnavailable;
        return false;
    }
    featRing_.prepare(static_cast<double>(sampleRate), static_cast<int>(channels), static_cast<int>(maxBlock));
    lastSampleRate_ = sampleRate;
    lastChannels_ = channels;
    return true;
}

bool InputSession::createSegments(u32 sampleRate, u32 channels)
{
    if (claimedChannel_.load(std::memory_order_relaxed) == 0)
    {
        return false;
    }

    // audio.chN:几何(sample_rate/ring_frames/channels)在 initData 写定;magic 最后发布。
    SegmentView av;
    if (backend_.createOrOpen(audioFullName(groupId_, claimedChannel_.load(std::memory_order_relaxed)),
                              audioSegmentSize(), av) != InitResult::kOk)
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

    // feat.chN。失败须释放已建的 audio 段(PR#51 泄漏修复)。
    SegmentView fv;
    if (backend_.createOrOpen(featFullName(groupId_, claimedChannel_.load(std::memory_order_relaxed)),
                              featSegmentSize(), fv) != InitResult::kOk)
    {
        releaseSegments();
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
        releaseSegments();
        return false;
    }
    FeatFrame* fdata = reinterpret_cast<FeatFrame*>(fh + 1);
    featHandle_ = SegmentHandle(std::move(fv), &backend_);
    featRing_.bind(fh, fdata, kFeatCapacityHops);

    if (!audioRing_.bound() || !featRing_.bound())
    {
        releaseSegments();
        return false;
    }
    return true;
}

void InputSession::rebuildAudioGeometry(u32 sampleRate, u32 channels)
{
    const AudioRingBinding* b = audioRing_.acquire();
    if (b == nullptr || !b->bound)
    {
        return;
    }
    AudioRingHeader* ah = b->header;
    float* adata = b->data;
    // 几何(plain u32)写定 → epoch+1 → write_head 重置 → 重新发布快照(几何与视图同快照发布)。
    ah->sample_rate = sampleRate;
    ah->channels = channels;
    ah->write_head_samples.store(0, std::memory_order_release);
    ah->epoch.fetch_add(1, std::memory_order_release);
    audioRing_.bind(ah, adata);
}

void InputSession::releaseSegments()
{
    audioRing_.bind(nullptr, nullptr); // 解绑(发布 nullptr,防悬垂)
    featRing_.unbind();
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
    const u32 ch = claimedChannel_.load(std::memory_order_relaxed);
    if (ch != 0)
    {
        registry_.releaseInput(ch, pid_);
        claimedChannel_.store(0, std::memory_order_release);
    }
}

} // namespace scvb::input
