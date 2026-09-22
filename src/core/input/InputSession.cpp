// SPDX-License-Identifier: GPL-3.0-or-later
#include "input/InputSession.h"

#include "ipc/GroupProbe.h"

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
    const bool sameGroup = registry_.isOpen() && registry_.group() == groupId_;
    if (sameGroup && claimedChannel_.load(std::memory_order_relaxed) == channelId_)
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

    // [SL-19 复发/UI 未回滚] 换 channel 前先记住"换之前真正持有的那个 channel"——下面失败时
    // 补偿式回滚要用。只在**组没变**的前提下才有意义:组同时也在变属于理论上可能、但
    // InputProcessor 的实际调用面从不这样用(setChannelId 只改 channel;换组走 changeGroup()
    // 那条独立路径,行为由它自己的测试钉着,本卡不碰)。两者同时变时按原样处理(失败即未分配),
    // 不引入一个只覆盖一半场景的补偿。
    const u32 previousChannel = sameGroup ? claimedChannel_.load(std::memory_order_relaxed) : 0;

    // 首次/换 channel/换 group:释放旧资源 → 新 claim → 建段。
    releaseSlot();
    releaseSegments();
    state_ = InputClaimState::kUnassigned;
    if (!openAndClaim(sampleRate, maxBlock, channels, nowMs))
    {
        const InputClaimState failure = state_; // 这次请求本身的失败原因,回滚成不成功都要报它

        // [SL-19 复发/UI 未回滚] 补偿式回滚:CAS 新槽失败时,尝试把刚释放的旧槽抢回来,
        // 让会话继续在旧 channel 上正常工作,不把"转移失败"变成"连旧的也丢了"。
        // ⚠ 不是保证:回滚本身也是一次 openAndClaim,失败窗口只有两次 CAS 之间那几微秒,
        // 理论上仍可能被另一实例抢先——那种情况下退化成"确实未分配",如实反映现状,
        // 不假装拿到了什么没拿到的东西。
        // 复审 4056695565:`previousChannel != channelId_` 这半句是恒真,删掉——走到这里已经
        // 隐含它成立:previousChannel 非 0 时必然 sameGroup==true(见上面的赋值),而 sameGroup
        // 为真时若 claimedChannel_(=previousChannel)== channelId_,早在第 55 行的快路径就
        // return 了,不会走到这儿。留着这句比较容易被将来的改动误读成"这两个号真的可能相等"。
        if (previousChannel != 0)
        {
            const u32 requestedChannel = channelId_;
            channelId_ = previousChannel; // 临时改回旧目标,复用同一条 claim 路径重新抢
            const bool rolledBack = openAndClaim(sampleRate, maxBlock, channels, nowMs);
            if (rolledBack)
            {
                // 会话确实还活着(只是回到了旧 channel):心跳/采集布防等内部逻辑都该按
                // "活跃"走;但这次用户请求的那个新 channel 本身没有拿到,报给调用方的
                // 仍然是 failure,UI 的冲突提示(抖动 + 红 toast)不受回滚影响。
                state_ = InputClaimState::kActive;
                return failure;
            }
            // 复审 4056695543:这里不是"避免留着一个没握住的号"——回滚失败后 previousChannel(3)
            // 和 requestedChannel(5)都没握住,这条理由站不住。真正的原因是:channelId_ 是"配置"
            // 字段(InputProcessor 读它当"用户最近一次请求的号"),上面几行只是临时把它借用成
            // previousChannel 好复用 openAndClaim() 的代码路径——回滚失败就要把这次临时借用
            // 复原,让 channelId_ 照实落回用户真正请求的那个号(哪怕没抢到),不能让它停留在
            // 一个纯粹为了复用代码路径而借用的旧值上。
            channelId_ = requestedChannel;
        }
        state_ = failure;
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

bool InputSession::outputOnline(u64 nowMs) const
{
    const OutputSlot* os = registry_.outputSlot();
    if (os == nullptr)
    {
        return false;
    }
    // state 校验防「释放后 ≤2s 残留新鲜」:releaseOutput 把 state 置回 kSlotFree,但**不清**
    // connected_mask(清 mask 只在 claimOutput 的两条路径),故 mask 位不足以证明 Output 在场。
    return os->state.load(std::memory_order_acquire) == kSlotActive &&
           !isStaleDisplay(os->heartbeat_ms.load(std::memory_order_acquire), nowMs);
}

bool InputSession::outputClaimedButStale(u64 nowMs) const
{
    const OutputSlot* os = registry_.outputSlot();
    if (os == nullptr)
    {
        return false; // 没有 Output ⇒ 不是「死的 Output」;直通与否由 mask/state 判
    }
    if (os->state.load(std::memory_order_acquire) != kSlotActive)
    {
        return false; // 优雅退场:音频线程逐块读 state 已判掉,这里不重复否决
    }
    // 自称 kSlotActive 却心跳陈旧 = 崩溃/挂死且没走释放路径 —— 唯一必须靠 [M] 时钟才能识别的那种。
    return isStaleDisplay(os->heartbeat_ms.load(std::memory_order_acquire), nowMs);
}

bool InputSession::isHealthy(u64 nowMs) const
{
    const u32 ch = claimedChannel_.load(std::memory_order_acquire);
    if (state_ != InputClaimState::kActive || ch == 0)
    {
        return false;
    }
    if (!outputOnline(nowMs))
    {
        return false;
    }
    const OutputSlot* os = registry_.outputSlot();
    if (os == nullptr)
    {
        return false;
    }
    const u32 mask = os->connected_mask.load(std::memory_order_acquire);
    return (mask & (1u << (ch - 1))) != 0;
}

InputConnSnapshot InputSession::connSnapshot(u64 nowMs) const
{
    InputConnSnapshot s;
    const u32 ch = claimedChannel_.load(std::memory_order_acquire);

    const OutputSlot* os = registry_.outputSlot();
    if (os != nullptr)
    {
        // §4.2 outputOnline = 本组 OutputSlot 心跳新鲜 ≤2000ms;附 state 校验防「释放后 ≤2s 残留新鲜」。
        s.outputOnline = outputOnline(nowMs);
        // §4.2 maskBit 以 outputOnline 为前提(PR#54 R5):Output 心跳陈旧时 connected_mask 位
        // 仍在也不置位,否则 kActive 的 claimValue 会误判为 "active"(已死 Output 显示健康)。
        if (s.outputOnline && ch >= 1 && ch <= kMaxChannels)
        {
            const u32 mask = os->connected_mask.load(std::memory_order_acquire);
            s.maskBit = (mask & (1u << (ch - 1))) != 0;
        }
    }

    if (ch >= 1 && ch <= kMaxChannels)
    {
        if (const InputSlot* is = registry_.inputSlot(ch))
        {
            s.capturing = (is->flags.load(std::memory_order_acquire) & kFlagCapturing) != 0;
        }
    }

    // §4.2 occupiedMask:置位 = 心跳新鲜(≤2000ms)且 state≥1(含本实例自己占的位);
    // 陈旧占用(双条件可覆盖)不置位,保持「陈旧可覆盖」语义。
    for (u32 c = 1; c <= kMaxChannels; ++c)
    {
        const InputSlot* is = registry_.inputSlot(c);
        if (is == nullptr)
        {
            continue;
        }
        if (is->state.load(std::memory_order_acquire) >= kSlotClaimed &&
            !isStaleDisplay(is->heartbeat_ms.load(std::memory_order_acquire), nowMs))
        {
            s.occupiedMask = static_cast<std::uint16_t>(s.occupiedMask | (1u << (c - 1)));
        }
    }
    return s;
}

std::uint8_t InputSession::groupsOnline(u64 nowMs) const
{
    // 本组位 = 本组 OutputSlot 心跳新鲜;其余 7 组经只读探测(01 §4.5,失败组判离线不报错)。
    // channel_id=0(未分配)时本组 registry 未打开,outputSlot() 为空 → 回退对本组也做只读探测
    // (pr-agent PR#54 R2:未分配实例的本组绿点恒灭)。
    const OutputSlot* os = registry_.outputSlot();
    std::uint8_t bits = probeGroupsOnline(backend_, groupId_, nowMs, /*includeOwnGroup=*/os == nullptr);
    if (os != nullptr && os->state.load(std::memory_order_acquire) == kSlotActive &&
        !isStaleDisplay(os->heartbeat_ms.load(std::memory_order_acquire), nowMs))
    {
        bits = static_cast<std::uint8_t>(bits | (1u << (groupId_ - 1)));
    }
    return bits;
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
    v.outputSlot = Registry::outputSlotAtBase(v.registryLease.base());
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

    // [SL-482] attach 到**存活的旧段**时上面那个 initData 一次都不跑,几何会停在上一实例的值。
    // 段不消亡是常态:Output 常驻持着 audioHandles_[idx],所以只要 Output 没卸载,audio.chN
    // 这个 section 就一直在。此时 createOrOpen 返回 created=false、magic 已有效,initHeader
    // 走「attach 且 magic 就绪」分支直接 kOk —— 几何一个字节都没写。而下游校验够不到这一维:
    // InputSession 只看 audioRing_.bound()(channels∈{1,2} 恒过),openAndClaim 随后把
    // lastChannels_ 置成**本次请求**的真实值,于是此后任何 re-prepare 的快路径都判「布局没变」、
    // 再也不会触发 rebuildAudioGeometry —— 错到 Output 卸载为止。
    // 用户侧表现:mono 轨上的 Input 换成 stereo 轨上的 Input 且用同一通道号 ⇒ write() 按
    // stride=1 把 LR 交错流当连续帧写,Output 按 mono 读 ⇒ 总线上是半速交替 L/R 的撕裂噪音。
    // 修法:按值比对段头几何,与本次请求不一致就走 rebuildAudioGeometry 的同一条路
    // (写定几何 → write_head 归零 → epoch+1 → 再发布绑定快照)。
    // 按值比对而不是按 created 判分支,是因为 allowOverwrite=true 的 attach 还有第三条出路
    // (自旋 500ms 仍 magic==0 → 覆盖式重初始化,initData 会跑);按值比对对三条出路都成立,
    // 且 initData 真跑过时它恒为 no-op。
    if (ah->sample_rate != sampleRate || ah->channels != channels || ah->ring_frames != kDefaultRingFrames)
    {
        ah->sample_rate = sampleRate;
        ah->ring_frames = kDefaultRingFrames;
        ah->channels = channels;
        ah->write_head_samples.store(0, std::memory_order_release);
        ah->epoch.fetch_add(1, std::memory_order_release); // 换代:读方丢弃旧代数据(01 §4.1)
    }

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
