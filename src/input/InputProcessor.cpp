// SPDX-License-Identifier: GPL-3.0-or-later
#include "InputProcessor.h"

#include "InputBridgeLogic.h"
#include "InputEditor.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr int kChannelIdMax = 15; // [J01+J59] 0..15,0=未分配
constexpr int kGroupIdMax = 8; // [J66] 1..8
} // namespace

ScvbInputAudioProcessor::ScvbInputAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      session_(backend_, static_cast<scvb::u32>(::GetCurrentProcessId())),
      ctrl_(backend_, scvb::input::kInputDefaultGroup)
{
    setLatencySamples(0); // ADR-002:Input 报告 latency=0
}

ScvbInputAudioProcessor::~ScvbInputAudioProcessor()
{
    stopTimer();
    const juce::ScopedLock lock(lifecycleMutex_);
    session_.release(scvb::steadyNowMs());
}

void ScvbInputAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const juce::ScopedLock lock(lifecycleMutex_);

    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    preparedMaxBlock_ = samplesPerBlock > 0 ? samplesPerBlock : 512;
    // [J57] 声道检测:prepareToPlay 依轨道布局判定 channels ∈ {1,2},运行期不变(写进 AudioRingHeader)。
    srcChannels_ = (getMainBusNumInputChannels() == 1) ? 1 : 2;
    capInterleaved_.assign(static_cast<std::size_t>(2) * static_cast<std::size_t>(preparedMaxBlock_), 0.0f);

    const auto now = scvb::steadyNowMs();
    session_.prepare(static_cast<scvb::u32>(sampleRate_), static_cast<scvb::u32>(preparedMaxBlock_),
                     static_cast<scvb::u32>(srcChannels_), now);
    rampSwitcher_.prepare(sampleRate_);

    // claim 未就绪(I6 未分配 / I2 冲突 / I0 / I1)→ 输出走直通档(人声不消失)。
    if (session_.state() != scvb::input::InputClaimState::kActive)
    {
        stageMachine_.forcePassthrough();
    }

    lastT0_ = std::numeric_limits<int64_t>::lowest();
    expectedNext_ = std::numeric_limits<int64_t>::lowest();
    prepared_ = true;
    startTimerHz(25); // [M] 25Hz:健康判定 + 心跳(4Hz 折半在 timerCallback 内)
}

void ScvbInputAudioProcessor::releaseResources()
{
    const juce::ScopedLock lock(lifecycleMutex_);
    session_.release(scvb::steadyNowMs());
    stageMachine_.forcePassthrough();
    prepared_ = false;
}

bool ScvbInputAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // 直通:输入输出声道数必须一致,支持 mono 与 stereo([J57] 立体声源)。
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
        layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
    {
        return false;
    }
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
    {
        return false;
    }
    return true;
}

void ScvbInputAudioProcessor::captureFrames(const float* const* src, int srcCh, float* dst, int n)
{
    // interleaved LR 打包;mono 单通道。[J57] 不下混、不互换。
    for (int c = 0; c < srcCh; ++c)
    {
        const float* s = src[c];
        for (int i = 0; i < n; ++i)
        {
            dst[static_cast<std::size_t>(i) * static_cast<std::size_t>(srcCh) + static_cast<std::size_t>(c)] = s[i];
        }
    }
}

void ScvbInputAudioProcessor::writeTailFromZero(const scvb::AudioRingBinding* b, const float* interleaved, int n,
                                                int64_t t0)
{
    // 跨零点块(t0<0 且 t0+n>0):写 [0, t0+n) 尾段(R3,01 §5.1 步骤 2)。
    const int skip = static_cast<int>(-t0);
    if (skip >= n)
    {
        return;
    }
    const int tail = n - skip;
    if (0 != expectedNext_)
    {
        // epoch 检测以 0 为新起点(R3)。
        scvb::AudioRing::bumpEpoch(b);
        session_.featRing().startRun(0);
    }
    lastT0_ = 0;
    expectedNext_ = tail;
    scvb::AudioRing::write(b, 0, interleaved + static_cast<std::size_t>(skip) * static_cast<std::size_t>(srcChannels_),
                           tail);
}

void ScvbInputAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    const juce::ScopedNoDenormals noDenormals;

    // 块长规划(PR#51 重要#2):采集/写环按夹取后的 captureSamples(research/01 §2.3 越界夹取,
    // Bridge #169 教训),输出级渲染按 renderSamples = 全块 —— 消除静音档大块尾段残留旧音频的泄漏。
    const int numIn = buffer.getNumSamples();
    if (numIn <= 0)
    {
        return;
    }
    const scvb::input::InputBlockPlan plan = scvb::input::planBlock(numIn, preparedMaxBlock_);
    const int n = plan.captureSamples;
    const int nRender = plan.renderSamples;

    const int srcCh = srcChannels_;
    const float* const* src = buffer.getArrayOfReadPointers();
    if (src == nullptr)
    {
        return;
    }

    // 音频线程块视图(PR#51 红旗#2,T16 Snapshot 同款):每 block acquire-load 一次不可变绑定快照
    // + 取 registry/audio/feat 三段 SegmentHandle 租约(持有期内 [M] 不解映射),整 block 复用;
    // 音频线程绝不触碰 session_ 的可变成员(claimedChannel_ 经块视图快照)。
    const scvb::input::InputSessionBlockView block = session_.acquireBlock();

    // 1) 捕获(ADR-003 v2.0 [J57]):mono 直取,stereo interleaved LR 打包,零分配(仅夹取部分)。
    if (n > 0)
    {
        captureFrames(src, srcCh, capInterleaved_.data(), n);
    }

    // 2) 时间线定位(负 timeInSamples = 倒计时/pre-roll,[J51] 有效时间线但 Input 不整块写环/写特征)。
    bool playing = false;
    bool haveT0 = false;
    int64_t t0 = 0;
    if (juce::AudioPlayHead* ph = getPlayHead())
    {
        const juce::Optional<juce::AudioPlayHead::PositionInfo> pos = ph->getPosition();
        if (pos.hasValue())
        {
            playing = pos->getIsPlaying();
            const juce::Optional<int64_t> ts = pos->getTimeInSamples();
            if (ts.hasValue())
            {
                t0 = *ts;
                haveT0 = true;
            }
        }
    }

    if (!haveT0 || t0 < 0)
    {
        if (haveT0 && t0 + n > 0)
        {
            writeTailFromZero(block.audio, capInterleaved_.data(), n, t0);
        }
        // 无/负时间线:输出走当前档(步骤 6)。
        float peak = 0.0f;
        for (int i = 0; i < n * srcCh; ++i)
        {
            peak = std::max(peak, std::abs(capInterleaved_[static_cast<std::size_t>(i)]));
        }
        meter_.store(peak, std::memory_order_relaxed);
        const scvb::u32 mode = c18Stage_.load(std::memory_order_acquire);
        rampSwitcher_.render(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), nRender,
                             mode == static_cast<scvb::u32>(scvb::input::OutputStageMode::kSilence)
                                 ? scvb::input::OutputStageMode::kSilence
                                 : scvb::input::OutputStageMode::kPassthrough);
        return;
    }

    // 3) epoch 跳变检测(契约 §2;停走带静止重写不算跳变)。
    if (t0 != expectedNext_)
    {
        if (playing || t0 != lastT0_)
        {
            scvb::AudioRing::bumpEpoch(block.audio);
            session_.featRing().startRun(t0); // run 切换:biquad 预热(T08/04 §3.2)
        }
    }
    lastT0_ = t0;
    expectedNext_ = t0 + nRender;

    // 4) 写音频环:时间线寻址,frame index = pos & (ring_frames-1);stereo interleaved LR。
    if (n > 0)
    {
        scvb::AudioRing::write(block.audio, t0, capInterleaved_.data(), n);
    }

    // 5) 特征提取(ADR-007:采集开才写特征段;K 加权与 hop 累加在播放中恒跑)。
    if (playing)
    {
        const bool armed = captureArmed_.load(std::memory_order_relaxed) != 0;
        session_.featRing().setCapturing(armed);
        session_.setCapturing(
            block.registrySlot,
            armed); // 经块视图 slot 快照(第3轮红旗) // 经块视图 channel + registry 租约(红旗#2/重要#3)
        planarPtrs_[0] = src[0];
        planarPtrs_[1] = (srcCh >= 2) ? src[1] : nullptr;
        if (n > 0)
        {
            if (n > 0)
            {
                session_.featRing().processBlock(planarPtrs_.data(), n);
            }
        }
    }
    else
    {
        session_.setCapturing(block.registrySlot, false);
    }

    float peak = 0.0f;
    for (int i = 0; i < n * srcCh; ++i)
    {
        peak = std::max(peak, std::abs(capInterleaved_[static_cast<std::size_t>(i)]));
    }
    meter_.store(peak, std::memory_order_relaxed);

    // 6) 输出级仲裁(ADR-002 v1/J12+J32):读 C18 模式字经 RampSwitcher 渲染。
    const scvb::u32 mode = c18Stage_.load(std::memory_order_acquire);
    rampSwitcher_.render(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), nRender,
                         mode == static_cast<scvb::u32>(scvb::input::OutputStageMode::kSilence)
                             ? scvb::input::OutputStageMode::kSilence
                             : scvb::input::OutputStageMode::kPassthrough);
}

void ScvbInputAudioProcessor::timerCallback()
{
    const auto now = scvb::steadyNowMs();
    const juce::ScopedLock lock(lifecycleMutex_);

    // 4Hz 心跳(250ms 折半在 25Hz 定时器内)。
    if (now - lastHeartbeatMs_ >= scvb::kHeartbeatIntervalMs)
    {
        lastHeartbeatMs_ = now;
        session_.heartbeat(now);
        session_.reap(now);
        ctrl_.reapPendingReleases(now); // T30:命令环段延迟释放回收([M] 4Hz,与 session_.reap 同点)
    }

    // 健康判定 → C18 模式字([M] 25Hz 写)。
    const bool healthy = session_.isHealthy(now);
    const scvb::input::OutputStageMode target = stageMachine_.evaluate(healthy, now);
    c18Stage_.store(static_cast<scvb::u32>(target), std::memory_order_release);

    // muted 确认位(C19,J32):健康(静音)置位、切直通前清位。
    // 精确的「ramp 完成」时点由 [A] 经 RampSwitcher 判定;此处 [M] 以目标档近似,
    // 80ms ramp 窗口被 Output 侧 ≥200ms 注入延迟覆盖(J32)。
    session_.setMuted(target == scvb::input::OutputStageMode::kSilence);
}

void ScvbInputAudioProcessor::setCurrentProgram(int /*index*/) {}

const juce::String ScvbInputAudioProcessor::getProgramName(int /*index*/)
{
    return {};
}

void ScvbInputAudioProcessor::changeProgramName(int /*index*/, const juce::String& /*newName*/) {}

void ScvbInputAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const juce::ScopedLock lock(lifecycleMutex_);

    // 【PR#51 重要#2】曾拒载更高 abi 的 state:保存时原样回写宿主传入的原字节(preservedOriginal
    // 语义,T19 同款),绝不把高版本 blob 覆盖成当前版数据。
    if (stateAbiMismatch_ && !preservedStateBlob_.empty())
    {
        destData.append(preservedStateBlob_.data(), preservedStateBlob_.size());
        return;
    }

    scvb::state::InputState s;
    s.channelId = static_cast<scvb::u32>(channelId_);
    s.groupId = static_cast<scvb::u32>(groupId_);
    s.uiScale = static_cast<scvb::u32>(uiScale_);
    s.uiLanguage = uiLanguage_.toStdString();

    std::vector<std::uint8_t> payload;
    if (!scvb::state::encodeInputState(s, payload))
    {
        return;
    }
    scvb::state::StateChunks chunks;
    chunks.abi = scvb::state::kCurrentAbi;
    chunks.set(scvb::state::kFourccCfgs, std::move(payload));
    std::vector<std::uint8_t> blob;
    if (!scvb::state::encodeContainer(chunks, blob))
    {
        return;
    }
    destData.append(blob.data(), blob.size());
}

void ScvbInputAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
    {
        return;
    }
    scvb::state::StateChunks chunks;
    if (scvb::state::decodeContainer(static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(sizeInBytes),
                                     chunks) != scvb::state::DecodeStatus::Ok)
    {
        return; // 不可信字节:解码失败 → 拒载(不崩溃、不半填充)
    }
    const juce::ScopedLock lock(lifecycleMutex_);

    // 【PR#51 红旗#1】冻结契约(CLAUDE.md §7.3 / STATE_SCHEMA):读到高版本 abi → 拒载并提示升级,
    // 绝不静默丢数据;同时保留原字节供 getStateInformation 原样回写(PR#51 重要#2,preservedOriginal)。
    if (scvb::state::decideInputStateAbi(chunks.abi) == scvb::state::InputStateAbiDecision::RejectNewer)
    {
        stateAbiMismatch_ = true;
        stateAbiSeen_ = chunks.abi;
        preservedStateBlob_.assign(static_cast<const std::uint8_t*>(data),
                                   static_cast<const std::uint8_t*>(data) + sizeInBytes);
        DBG("SCVB Input: state abi " << chunks.abi << " > current " << scvb::state::kCurrentAbi
                                     << "; refusing load (upgrade required)");
        return;
    }
    stateAbiMismatch_ = false;
    preservedStateBlob_.clear();

    const scvb::state::Chunk* cfg = chunks.find(scvb::state::kFourccCfgs);
    if (cfg == nullptr)
    {
        return;
    }
    scvb::state::InputState s;
    if (!scvb::state::decodeInputState(cfg->payload.data(), cfg->payload.size(), s))
    {
        return; // 范围校验失败 → 拒载(CLAUDE.md §7.3)
    }

    const int oldGroupId = groupId_;
    channelId_ = static_cast<int>(s.channelId);
    groupId_ = static_cast<int>(s.groupId);
    uiScale_ = static_cast<int>(s.uiScale);
    uiLanguage_ = juce::String::fromUTF8(s.uiLanguage.c_str(), static_cast<int>(s.uiLanguage.size()));
    session_.setChannelId(s.channelId);
    session_.setGroupId(s.groupId);
    // T30 PR#54 复审【重要】1 + R9:ctrl_ 命令环段组必须与 state 的 group_id 同组 —— 否则
    // remoteSetPriority 把记录投进旧组命令环、srMismatch 推导读旧组 SR。channel_id=0 释放段;
    // 否则换组失败回退旧组、保持 session/ctrl 一致(避免「session 新组 + ctrl 未开/旧组」错位)。
    if (s.channelId == 0)
    {
        ctrl_.release();
    }
    else if (ctrl_.group() != s.groupId)
    {
        if (ctrl_.changeGroup(s.groupId) != scvb::InitResult::kOk)
        {
            ctrl_.changeGroup(static_cast<scvb::u32>(oldGroupId)); // 尽力回退旧组段
            groupId_ = oldGroupId;
            session_.setGroupId(static_cast<scvb::u32>(oldGroupId));
        }
    }
    // 绑定时序(03 §7.2):setStateInformation 后 claim;样本率等 prepareToPlay 提供。
    // 若已 prepare,立即 re-claim;否则由下一次 prepareToPlay 走 claim。
    if (prepared_)
    {
        session_.prepare(static_cast<scvb::u32>(sampleRate_), static_cast<scvb::u32>(preparedMaxBlock_),
                         static_cast<scvb::u32>(srcChannels_), scvb::steadyNowMs());
        if (session_.state() != scvb::input::InputClaimState::kActive)
        {
            stageMachine_.forcePassthrough();
        }
    }
}

scvb::input::InputClaimState ScvbInputAudioProcessor::setChannelId(int channelId)
{
    channelId = juce::jlimit(0, kChannelIdMax, channelId);
    const juce::ScopedLock lock(lifecycleMutex_);
    channelId_ = channelId;
    session_.setChannelId(static_cast<scvb::u32>(channelId));
    if (channelId == 0)
    {
        ctrl_.release(); // channel_id=0 不 claim 任何段(T23 口径):命令环段随释放(PR#54 R9)
    }
    session_.prepare(static_cast<scvb::u32>(sampleRate_), static_cast<scvb::u32>(preparedMaxBlock_),
                     static_cast<scvb::u32>(srcChannels_), scvb::steadyNowMs());
    if (session_.state() != scvb::input::InputClaimState::kActive)
    {
        stageMachine_.forcePassthrough();
    }
    return session_.state(); // T30 桥:{conflict:true} ⇔ kConflict,其余 {ok:true}
}

scvb::input::InputClaimState ScvbInputAudioProcessor::setGroupId(int groupId)
{
    groupId = juce::jlimit(1, kGroupIdMax, groupId);
    const juce::ScopedLock lock(lifecycleMutex_);
    if (groupId == groupId_)
    {
        return session_.state(); // 同组 no-op(§3.3:{ok:true})
    }
    const scvb::u32 newGroup = static_cast<scvb::u32>(groupId);
    const scvb::u32 oldGroup = static_cast<scvb::u32>(groupId_);
    // T30:命令环 ctrl 段随组走(per-组各一份,ipc v1.5 [J66];与 Registry::changeGroup 同构)。
    // PR#54 R9 + R10:有通道时先换 ctrl 段,失败回退旧组、不切 session —— 避免「session 新组 +
    // ctrl 未开/旧组」错位;channel_id=0(未分配)不建/不换 ctrl 段(保持「channel_id=0 不建段」
    // 口径),ctrl 段留到首次 setChannelId 的 ensureCtrlOpen 懒开(懒开按当前组对齐)。
    if (channelId_ != 0)
    {
        if (ctrl_.changeGroup(newGroup) != scvb::InitResult::kOk)
        {
            ctrl_.changeGroup(oldGroup); // 尽力回退旧组段(通常成功;双重失败则 ctrl 未开,ensureCtrlOpen 重试)
            return session_.state(); // 不切 session group,保持旧组一致态
        }
    }
    groupId_ = groupId;
    // 改组(J66):释放旧组 slot → 新组重走 claim;期间输出走直通档(01 §4.1)。
    stageMachine_.forcePassthrough();
    session_.changeGroup(newGroup, static_cast<scvb::u32>(sampleRate_), static_cast<scvb::u32>(preparedMaxBlock_),
                         static_cast<scvb::u32>(srcChannels_), scvb::steadyNowMs());
    return session_.state(); // T30 桥:{conflict:true} ⇔ 新组同 channel 被占(I2)
}

juce::AudioProcessorEditor* ScvbInputAudioProcessor::createEditor()
{
    return new scvb::input::InputEditor(*this);
}

// ---------------------------------------------------------------------------
// T30 Input 桥接入面([M] 编辑器线程;除注明外持 lifecycleMutex_)
// ---------------------------------------------------------------------------
void ScvbInputAudioProcessor::ensureCtrlOpen()
{
    // 调用方已持 lifecycleMutex_。channel_id=0(未分配)不建/不开命令环段(T23「channel_id=0 无痕迹/
    // 不建段」口径);释放回到 0 时由 setChannelId/setStateInformation 调 ctrl_.release() 释放(PR#54 R9)。
    if (channelId_ == 0)
    {
        return;
    }
    // PR#54 R10:release 只清 base_ 不改 group_;channel_id=0 + 非默认组加载后再分配通道时 group_
    // 可能残留旧组 —— 懒开前先按当前 groupId_ 换组对齐,否则 remoteSetPriority/srMismatch 落到错组。
    if (ctrl_.group() != static_cast<scvb::u32>(groupId_))
    {
        ctrl_.changeGroup(static_cast<scvb::u32>(groupId_));
    }
    // Input 是本组 ctrl 段的合法创建/覆盖者(CtrlPlane::open 注释)。
    if (!ctrl_.isOpen())
    {
        ctrl_.open();
    }
}

ScvbInputAudioProcessor::BridgeTickSnapshot ScvbInputAudioProcessor::bridgeTickSnapshot()
{
    const juce::ScopedLock lock(lifecycleMutex_);
    const auto now = scvb::steadyNowMs();
    BridgeTickSnapshot s;
    s.channelId = channelId_;
    s.groupId = groupId_;
    s.claimState = session_.state();
    s.conn = session_.connSnapshot(now);
    s.healthy = session_.isHealthy(now);
    s.passthrough = stageMachine_.target() == scvb::input::OutputStageMode::kPassthrough;
    s.sampleRate = sampleRate_;
    s.sourceChannels = srcChannels_;
    ensureCtrlOpen();
    s.globalInfo = ctrl_.readGlobalInfo();
    s.configSeq = session_.configSeq();
    s.localAbi = session_.localAbi();
    s.remoteAbi = session_.remoteAbi();
    return s;
}

std::uint8_t ScvbInputAudioProcessor::bridgeGroupsOnline()
{
    const juce::ScopedLock lock(lifecycleMutex_);
    return session_.groupsOnline(scvb::steadyNowMs());
}

ScvbInputAudioProcessor::PriorityResult ScvbInputAudioProcessor::bridgeRemoteSetPriority(int n)
{
    n = juce::jlimit(0, 10, n);
    const juce::ScopedLock lock(lifecycleMutex_);
    ensureCtrlOpen();
    const auto now = scvb::steadyNowMs();
    PriorityResult r;

    const scvb::u32 ch = static_cast<scvb::u32>(channelId_);
    const bool outputOnline = session_.connSnapshot(now).outputOnline;
    const bool active = session_.state() == scvb::input::InputClaimState::kActive;
    const bool ringFull = ctrl_.isRingFull(ch);
    // §3.4/§5.6 拒绝判定(真源 = bridge::priorityRejection 纯函数)。判定顺序:unassigned >
    // outputOffline > 非活跃 > ringFull。非活跃实例(conflict/abiMismatch/unavailable)不是
    // channel 持有者,SPSC 纪律禁止其向共享命令环 enqueue —— 否则两个实例成双生产者竞写
    // write_pos/覆盖最旧,损坏环或向不拥有的 channel 注入控制命令(PR#54 R3)。reason 取
    // §5.6 闭集内最近似的 "unassigned"(未持有 slot;channelConflict 是 §5.1 errorCode 而非
    // reason,闭集内无对应项,故不新造 reason,保持 §3.4 remoteSetPriority 返回形状冻结)。
    const auto reject = scvb::input::bridge::priorityRejection(channelId_, outputOnline, ringFull, active);

    // 满环仍投递(写方覆盖最旧 + 溢出计数,§6/10 IPC-13);其余拒绝态一律不投递。
    if (reject == scvb::input::bridge::PriorityReject::kRingFull ||
        reject == scvb::input::bridge::PriorityReject::kNone)
    {
        // PR#54 R10:enqueue 返回 false = 命令未写入(ctrl 段未打开/打开失败,如 abi 不符的残留损坏
        // 段)。不得再回 queued:true,否则 UI 误以为成功。reason 取 §5.6 八值闭集内最近似的 busy
        // (临时可重试;其余七值语义均不匹配:badArg/noTimeline/noLoop/notAdjacent 属其它函数、
        // ringFull/outputOffline/unassigned 已有专属判定),不新造 reason。
        if (!ctrl_.enqueue(ch, scvb::CtrlOp::kSetPriority, static_cast<scvb::u64>(n)))
        {
            r.reason = "busy";
            return r;
        }
    }

    if (reject != scvb::input::bridge::PriorityReject::kNone)
    {
        r.reason = scvb::input::bridge::priorityRejectReason(reject);
        return r;
    }
    r.queued = true;
    return r;
}

void ScvbInputAudioProcessor::bridgeSetUiLanguage(const juce::String& lang)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    uiLanguage_ = lang; // 已由桥层 normalize({zh,en,fr});getStateInformation 持久化
}

void ScvbInputAudioProcessor::bridgeSetUiScalePercent(int percent)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    uiScale_ = juce::jlimit(33, 300, percent); // 0.33..3.0 × 100(params-v0 §三 uiScale)
}

int ScvbInputAudioProcessor::bridgeUiScalePercent() const
{
    const juce::ScopedLock lock(lifecycleMutex_);
    return uiScale_;
}

juce::String ScvbInputAudioProcessor::bridgeUiLanguage() const
{
    const juce::ScopedLock lock(lifecycleMutex_);
    return uiLanguage_;
}

// juce_add_plugin 的 VST3/AU wrapper 从这里实例化插件。
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ScvbInputAudioProcessor();
}
