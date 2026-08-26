// SPDX-License-Identifier: GPL-3.0-or-later
#include "output/OutputSession.h"

#include <limits>

namespace scvb::output
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

void OutputSession::attachFeatRings()
{
    // 与 attachAudioRings 同构的只读 attach:openExisting + initHeader(allowOverwrite=false)。
    // 幂等,失败由 [M] 25Hz 重试。特征段与音频段同生命周期(InputSession 一起建/一起放)。
    for (u32 ch = 1; ch <= kMaxChannels; ++ch)
    {
        const std::size_t idx = static_cast<std::size_t>(ch - 1);
        if (featBound_[idx] || featHandles_[idx].valid())
        {
            continue;
        }
        InputSlot* slot = registry_.inputSlot(ch);
        if (slot == nullptr || slot->state.load(std::memory_order_acquire) != kSlotActive)
        {
            continue;
        }

        SegmentView fv;
        if (backend_.openExisting(featFullName(groupId_, ch), fv) != InitResult::kOk)
        {
            continue;
        }
        auto* fh = static_cast<FeatHeader*>(fv.base);
        const auto ir = backend_.initHeader(fv, &fh->magic, &fh->abi, nullptr, sizeof(FeatHeader),
                                            /*initData=*/{}, /*allowOverwrite=*/false);
        if (ir != InitResult::kOk)
        {
            backend_.unmap(fv);
            continue;
        }

        // 几何快照纪律(04 §3 / FeatRing.h 头注):capacity 在 attach 时读一次进绑定,
        // 此后只用快照做索引模数,绝不回读段头。
        const u32 capacity = fh->capacity_hops;
        if (capacity == 0)
        {
            backend_.unmap(fv);
            continue; // 写方尚未初始化几何:下一拍重试
        }
        const FeatFrame* fdata = reinterpret_cast<const FeatFrame*>(fh + 1);
        featHandles_[idx] = SegmentHandle(std::move(fv), &backend_);
        featPuller_.bind(ch, fh, fdata, capacity);
        featBound_[idx] = true;
    }
}

void OutputSession::pullFeatures()
{
    // ChannelFrames 默认 readOnly=true(写入口静默丢弃、不记账)。采集开关就是那把闸:
    // 开 → 允许记账;关 → 回只读,已采集的覆盖保留不动(ADR-007 采集 OFF 只读语义)。
    // 不开闸的话拉取会一路成功却什么都没写进去,覆盖率恒 0。
    const bool capturing = captureEnabled_.load(std::memory_order_relaxed) != 0;
    for (u32 ch = 1; ch <= kMaxChannels; ++ch)
    {
        auto& frames = frameStore_.channel(ch);
        frames.setReadOnly(!capturing);
        // 布防时间维(§1 setCaptureEnabled):范围外的 hop 静默丢弃、不记账。
        frames.setGate(featureGate_);
    }
    if (!capturing)
    {
        return; // 采集 OFF:Input 也不写 feat 段,连拉都省了
    }

    // timeGate 与写入口的 gate 同一个范围(pullIncremental 也据它裁剪);selectedMask=0 = 不限轨
    // (轨维在 Input 侧按广播区的 enabled 位布防,不重复门控);activeMask = 本组 connected_mask,
    // 只拉在线轨(04 §3.3)。
    featPuller_.pullTick(frameStore_, featureGate_, /*selectedMask=*/0, registry_.connectedMask());
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

        // CH_SUSPENDED:write_head 完全停滞 ≥0.5s ∧ 心跳新鲜(宿主跳过该轨处理)。
        // dataAdvancing:该轨最近确有新帧写入 —— 恢复判定要用它,见下。
        // **判定必须排在失准判定之前**:停流现在是失准发作的来源之一(见下面的 stallCount_)。
        bool suspended = false;
        bool dataAdvancing = false;
        if (hbFresh && bound)
        {
            const u64 wh = sources_[idx].writeHead();
            if (wh != lastWriteHead_[idx])
            {
                lastWriteHead_[idx] = wh;
                lastWriteHeadChangeMs_[idx] = nowMs;
                starvingSeen_[idx] = false; // 写头一动,上一段停滞期的饿读证据作废
            }
            else if (nowMs - lastWriteHeadChangeMs_[idx] >= kSuspendStallMs)
            {
                suspended = true;
            }
            dataAdvancing = (nowMs - lastWriteHeadChangeMs_[idx]) < kSuspendStallMs;
        }

        // 停流记账:坐实挂起才记,且**一次挂起只记一笔**。
        // 短停(宿主在 -inf 段挂起 Input 又很快恢复)整个落在 500ms 判定窗内,永远走不到这里 ——
        // 那正是 P1-7 要消掉的误报。持续挂起则每拍刷新发作时刻:本轨退出注入集后 read() 不再被
        // 调用、缺口自然停止增长,只看「无新缺口」会把持续断流判成恢复(v4 实测 P0-2 假恢复)。
        // 开场条件之二:本拍确实有「写头没推到」的饿读。走带停止 / 宿主没在给块时写头同样
        // 冻结,但那时要么读得到数据、要么根本没在读 —— 一笔饿读都不会有,不该报。
        const u32 sf = sources_[idx].stallFailCount();
        if (sf != lastStallFail_[idx])
        {
            starvingSeen_[idx] = true;
        }
        lastStallFail_[idx] = sf;
        // 证据要**跨拍留存**,不能只看「本拍有没有饿读」:挂起坐实的同一拍本轨就退出注入集、
        // read() 随即停调,而 25Hz 的拍与音频块并不对齐 —— 只看本拍的话,挂起恰好落在
        // 「本拍没跑到块」的那一拍上就永远开不了场(实测约一半概率)。

        if (!suspended || !transportPlaying_)
        {
            stallEpisode_[idx] = false; // 写头恢复推进 / 走带停了 → 发作结束
        }
        else if (starvingSeen_[idx])
        {
            if (!stallEpisode_[idx])
            {
                ++stallCount_[idx]; // 一次发作只记一笔,不按块累加
            }
            stallEpisode_[idx] = true;
        }
        if (stallEpisode_[idx])
        {
            // 发作期间每拍刷新:退出注入集后 read() 不再被调,只看「无新缺口/无新饿读」
            // 会把持续断流判成恢复。
            misalignedSinceMs_[idx] = nowMs;
        }

        // 失准判定:gapCount 增长 → 记失准时刻(近 1s 内视为 CH_MISALIGNED)。
        const u32 gc = sources_[idx].gapCount();
        if (gc != lastGapCount_[idx])
        {
            misalignedSinceMs_[idx] = nowMs;
            lastGapCount_[idx] = gc;
        }
        const bool misaligned = (nowMs - misalignedSinceMs_[idx]) < kMisalignRecoverMs;

        // 恢复判定:必须「该轨数据真的在推进」,**不能只看「不再产生新缺口」**。
        // 反例(v4 实测 B3 假恢复):把 Input bypass 掉 → write_head 停滞 → 本轨转 suspended →
        // online=false → 退出 injectMask → processBlock 根本不再调 read() → 缺口自然不再增长。
        // 只看「无新缺口」会把这种**持续断流的静默**判成恢复,横幅在实际仍无声时撤下。
        // 所以恢复要同时满足:槽活跃 + 心跳新鲜 + 采样率一致 + 已绑定 + 未挂起 + 有新帧写入。
        const bool dataHealthy = slotActive && hbFresh && srMatch && bound && !suspended && dataAdvancing;
        if (!misaligned && dataHealthy)
        {
            // 本次失准发作结束 → 上桥的 misalignCount 归零,「路由失准」横幅与逐行 ⚠ 随之撤下
            // (进程累计值仍留在 gapCount 供 ctrl 全局小节/诊断)。
            misalignBaseline_[idx] = gc;
            stallBaseline_[idx] = stallCount_[idx];
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
    // 消费命令环(排空,防满环丢最旧)+ op 派发。
    // 此前这个循环体是空的 —— 记录被 dequeue 出来就丢掉,于是 Input 侧的 remoteSetPriority
    // 一路走到这里进 /dev/null,优先级怎么调都不生效(T37 三轮 C 族)。
    CtrlRecord rec;
    for (u32 ch = 1; ch <= kMaxChannels; ++ch)
    {
        const std::size_t idx = static_cast<std::size_t>(ch - 1);
        while (ctrl_.dequeue(ch, rec))
        {
            switch (rec.op)
            {
            case CtrlOp::kSetPriority:
                // 同一拍收到多条只留最后一条(值语义,不是增量);越界钳到 0..10。
                pendingPriority_[idx] = rec.value > 10u ? 10u : static_cast<u32>(rec.value);
                hasPendingPriority_[idx] = true;
                break;
            case CtrlOp::kFpReport:
                // 指纹上报归分析管线(04 §3.4),本层不消费,排空即可。
                break;
            case CtrlOp::kNone:
            default:
                // 未知 op:读方安全忽略(§4.0 前向兼容纪律),不报错不中断排空。
                break;
            }
        }
    }
}

bool OutputSession::takeRemotePriority(u32 channel, u32& valueOut)
{
    if (channel < 1 || channel > kMaxChannels)
    {
        return false;
    }
    const std::size_t idx = static_cast<std::size_t>(channel - 1);
    if (!hasPendingPriority_[idx])
    {
        return false;
    }
    valueOut = pendingPriority_[idx];
    hasPendingPriority_[idx] = false; // 取走即消费
    return true;
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
    attachFeatRings();
    evaluateChannels(nowMs);
    pullFeatures(); // 在 evaluateChannels 之后:activeMask 用本拍刚算出的 connected_mask

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
    resetChannelTracking(); // PR#53 第5轮:释放旧组后重置 per-channel 跟踪(旧组时序不得污染新组判定)
    // 特征真身按 channel 索引存、**没有 group 维度**:不清的话新组的 ch3 会继承旧组 ch3 的
    // CoverageMap,pullFeatures 再把新组特征并进同一张表(覆盖率与「已分析区域」都会算错)。
    // 只在**改组**清;release(同组 releaseResources/重新 prepare)不清 —— 那不该丢已采集的数据。
    frameStore_.reset();
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
    resetChannelTracking();
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

ChannelConnInfo OutputSession::channelConn(u32 channel, u64 nowMs) const
{
    ChannelConnInfo info;
    const InputSlot* slot = registry_.inputSlot(channel);
    if (slot == nullptr)
    {
        return info; // registry 未映射 / channel 非法 → 空闲 + 无数据哨兵
    }
    info.slotState = slot->state.load(std::memory_order_acquire);
    info.heartbeatAgeMs = heartbeatAgeMsOf(info.slotState, slot->heartbeat_ms.load(std::memory_order_acquire), nowMs);
    info.capturing = (slot->flags.load(std::memory_order_acquire) & kFlagCapturing) != 0;
    // 采样率只在槽活跃且两端都已 prepare 时才有可比性(0 = 未知,不报不一致)。
    const u32 slotSr = slot->sample_rate;
    info.srMismatch = info.slotState == kSlotActive && slotSr != 0 && sampleRate_ != 0 && slotSr != sampleRate_;
    return info;
}

u32 OutputSession::gapCount(u32 channel) const
{
    if (channel < 1 || channel > kMaxChannels)
    {
        return 0;
    }
    return sources_[static_cast<std::size_t>(channel - 1)].gapCount();
}

u32 OutputSession::misalignCountRecent(u32 channel) const
{
    if (channel < 1 || channel > kMaxChannels)
    {
        return 0;
    }
    const std::size_t idx = static_cast<std::size_t>(channel - 1);
    const u32 gc = sources_[idx].gapCount();
    const u32 base = misalignBaseline_[idx];
    const u32 gaps = gc > base ? gc - base : 0; // baseline 只会落后于 gc;防守式取饱和差
    // 停流笔数并进同一个数:UI 只有这一个「这条轨没在出数据」的计数位,两类故障共用它。
    const u32 stalls = stallCount_[idx] > stallBaseline_[idx] ? stallCount_[idx] - stallBaseline_[idx] : 0;
    return gaps + stalls;
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
        // 特征侧同步解绑:featPuller_ 持的是段内裸指针,句柄一放就不能再拉。
        featBound_[i] = false;
        releaseHandle(featHandles_[i]);
    }
    featPuller_.reset();
    // frameStore_ 不清:改组/重绑不该丢已采集的特征真身(它是 Output 侧的持久化权威,
    // 04 §3.1)。真要清由桥面的 clearCoverage 走打洞路径。
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

void OutputSession::resetChannelTracking() noexcept
{
    // 释放旧组/段后清零 per-channel 跟踪,防止旧组时序污染新组判定:onlinePrev_ 残留 true 会跳过
    // [J32] 200ms 注入延迟(切换瞬间立即注入);陈旧 lastWriteHeadChangeMs_ 可能误触 kSuspendStallMs 提前挂起。
    onlinePrev_.fill(false);
    onlineSinceMs_.fill(0);
    misalignedSinceMs_.fill(0);
    lastWriteHead_.fill(0);
    lastWriteHeadChangeMs_.fill(0);
    // lastGapCount_/misalignBaseline_ 对齐到**当前**累计值而不是 0:gapCount_ 是 ShmRingMixSource
    // 的进程寿命计数器,改组/重绑都不清零。填 0 会让下一拍 gc != lastGapCount_ 恒成立,
    // 把新组第一拍直接判成失准。
    for (std::size_t i = 0; i < kMaxChannels; ++i)
    {
        const u32 gc = sources_[i].gapCount();
        lastGapCount_[i] = gc;
        misalignBaseline_[i] = gc;
        stallBaseline_[i] = stallCount_[i];
        lastStallFail_[i] = sources_[i].stallFailCount();
    }
    stallEpisode_.fill(false);
    starvingSeen_.fill(false);
}

void OutputSession::releaseSlot()
{
    if (state_.load(std::memory_order_relaxed) == OutputClaimState::kActive)
    {
        registry_.releaseOutput(pid_);
    }
}

} // namespace scvb::output
