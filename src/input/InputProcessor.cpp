// SPDX-License-Identifier: GPL-3.0-or-later
#include "InputProcessor.h"

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
      session_(backend_, static_cast<scvb::u32>(::GetCurrentProcessId()))
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
        session_.setCapturing(block.channel, armed); // 经块视图 channel + registry 租约(红旗#2/重要#3)
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
        session_.setCapturing(block.channel, false);
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

    channelId_ = static_cast<int>(s.channelId);
    groupId_ = static_cast<int>(s.groupId);
    uiScale_ = static_cast<int>(s.uiScale);
    uiLanguage_ = juce::String::fromUTF8(s.uiLanguage.c_str(), static_cast<int>(s.uiLanguage.size()));
    session_.setChannelId(s.channelId);
    session_.setGroupId(s.groupId);
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

void ScvbInputAudioProcessor::setChannelId(int channelId)
{
    channelId = juce::jlimit(0, kChannelIdMax, channelId);
    const juce::ScopedLock lock(lifecycleMutex_);
    channelId_ = channelId;
    session_.setChannelId(static_cast<scvb::u32>(channelId));
    session_.prepare(static_cast<scvb::u32>(sampleRate_), static_cast<scvb::u32>(preparedMaxBlock_),
                     static_cast<scvb::u32>(srcChannels_), scvb::steadyNowMs());
    if (session_.state() != scvb::input::InputClaimState::kActive)
    {
        stageMachine_.forcePassthrough();
    }
}

void ScvbInputAudioProcessor::setGroupId(int groupId)
{
    groupId = juce::jlimit(1, kGroupIdMax, groupId);
    const juce::ScopedLock lock(lifecycleMutex_);
    if (groupId == groupId_)
    {
        return;
    }
    groupId_ = groupId;
    // 改组(J66):释放旧组 slot → 新组重走 claim;期间输出走直通档(01 §4.1)。
    stageMachine_.forcePassthrough();
    session_.changeGroup(static_cast<scvb::u32>(groupId), static_cast<scvb::u32>(sampleRate_),
                         static_cast<scvb::u32>(preparedMaxBlock_), static_cast<scvb::u32>(srcChannels_),
                         scvb::steadyNowMs());
}

// juce_add_plugin 的 VST3/AU wrapper 从这里实例化插件。
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ScvbInputAudioProcessor();
}
