// SPDX-License-Identifier: GPL-3.0-or-later
#include "InputProcessor.h"

#include <cstdlib>

namespace
{
// Input state 布局(ADR-004「Input state 只存 channel id + UI 偏好」+ [J66] group_id)。
// { u32 abi; u32 group_id; u32 channel_id; } 共 12 字节。
inline constexpr unsigned int kStateAbi = 1;

bool parseEnvU32(const char* name, unsigned int lo, unsigned int hi, unsigned int& out)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0')
    {
        return false;
    }
    const long parsed = std::strtol(v, nullptr, 10);
    if (parsed < static_cast<long>(lo) || parsed > static_cast<long>(hi))
    {
        return false;
    }
    out = static_cast<unsigned int>(parsed);
    return true;
}
} // namespace

InputProcessor::InputProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    parseEnvU32("SCVB_GROUP", 1, scvb::kMaxGroups, envGroup_);
    parseEnvU32("SCVB_CHANNEL", 1, scvb::kMaxChannels, envChannel_);
}

InputProcessor::~InputProcessor()
{
    stopTimer();
    if (claimed_ && registry_ != nullptr)
    {
        registry_->releaseInput(channel_, pid_);
    }
}

void InputProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    maxBlock_ = samplesPerBlock;
    srcChannels_ = (getTotalNumInputChannels() >= 2) ? 2 : 1;

    capBuf_.assign(static_cast<std::size_t>(maxBlock_) * srcChannels_, 0.0f);
    stage_.prepare(sampleRate_);

    resolveConfig();
    doClaim();

    if (!isTimerRunning())
    {
        startTimerHz(25); // [M] 25Hz 健康仲裁 + ~240ms 心跳
    }
}

void InputProcessor::releaseResources()
{
    stopTimer();
}

bool InputProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
    {
        return false;
    }
    return layouts.getMainOutputChannelSet() == layouts.getMainInputChannelSet();
}

void InputProcessor::resolveConfig()
{
    const unsigned int g = (envGroup_ >= 1 && envGroup_ <= scvb::kMaxGroups)
                               ? envGroup_
                               : ((stateGroup_ >= 1 && stateGroup_ <= scvb::kMaxGroups) ? stateGroup_ : 1);
    const unsigned int c = (envChannel_ >= 1 && envChannel_ <= scvb::kMaxChannels)
                               ? envChannel_
                               : (stateChannel_ <= scvb::kMaxChannels ? stateChannel_ : 0);
    group_ = g;
    channel_ = c;
}

void InputProcessor::doClaim()
{
    pid_ = static_cast<u32>(::GetCurrentProcessId());

    // 组号变化 → 重建 IPC 对象(组号 G 是构造参数)。
    if (registry_ == nullptr || registry_->group() != group_)
    {
        registry_ = std::make_unique<scvb::Registry>(group_);
        ring_ = std::make_unique<scvb::AudioRing>(group_, channel_ == 0 ? 1 : channel_);
    }

    if (!registry_->isOpen())
    {
        const auto r = registry_->open();
        if (r != scvb::Registry::ClaimResult::kClaimed)
        {
            claimed_ = false;
            active_ = false;
            return; // abi 不符 / 映射失败
        }
    }

    // 已有环且 SR/声道未变 → 仅更新 max_block。
    if (claimed_ && ring_ != nullptr && ring_->isOpen() && channel_ != 0 &&
        ring_->header()->sample_rate == static_cast<u32>(sampleRate_) &&
        ring_->header()->channels == static_cast<u32>(srcChannels_))
    {
        if (scvb::InputSlot* slot = registry_->inputSlot(channel_))
        {
            slot->max_block = static_cast<u32>(maxBlock_);
        }
        return;
    }

    // claim(0 = 自动最低空闲 slot)。
    if (channel_ == 0)
    {
        channel_ = 0;
        for (u32 c = 1; c <= scvb::kMaxChannels; ++c)
        {
            if (registry_->claimInput(c, pid_, static_cast<u32>(sampleRate_), static_cast<u32>(maxBlock_),
                                      scvb::steadyNowMs()) == scvb::Registry::ClaimResult::kClaimed)
            {
                channel_ = c;
                break;
            }
        }
    }
    else
    {
        if (registry_->claimInput(channel_, pid_, static_cast<u32>(sampleRate_), static_cast<u32>(maxBlock_),
                                  scvb::steadyNowMs()) != scvb::Registry::ClaimResult::kClaimed)
        {
            claimed_ = false;
            active_ = false;
            return; // 冲突:不写环,输出走直通
        }
    }

    if (channel_ == 0)
    {
        claimed_ = false;
        active_ = false;
        return; // 15 slot 全占
    }

    // 环可能因 channel 变化而换对象。
    if (ring_->channel() != channel_ || ring_->group() != group_)
    {
        ring_ = std::make_unique<scvb::AudioRing>(group_, channel_);
    }
    ring_->createForInput(static_cast<u32>(sampleRate_), scvb::ringFramesFromEnv(), static_cast<u32>(srcChannels_));

    claimed_ = true;
    active_ = ring_->isOpen();
    lastT0_ = -1;
    expectedNext_ = -1;
    unhealthySince_ = 0;
    silenceCommandedAt_ = 0;
    mutePending_ = false;
}

void InputProcessor::captureInterleaved(const juce::AudioBuffer<float>& buffer, int offset, int numSamples)
{
    if (srcChannels_ == 1)
    {
        const float* in = buffer.getReadPointer(0) + offset;
        for (int i = 0; i < numSamples; ++i)
        {
            capBuf_[static_cast<std::size_t>(i)] = in[i];
        }
    }
    else
    {
        const float* l = buffer.getReadPointer(0) + offset;
        const float* r = buffer.getReadPointer(1) + offset;
        for (int i = 0; i < numSamples; ++i)
        {
            capBuf_[static_cast<std::size_t>(i) * 2] = l[i];
            capBuf_[static_cast<std::size_t>(i) * 2 + 1] = r[i];
        }
    }
}

void InputProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int total = buffer.getNumSamples();
    if (total <= 0)
    {
        return;
    }
    const int nch = juce::jmin(buffer.getNumChannels(), srcChannels_);

    // 时间线定位(R1/R3:负 t0 = 倒计时/pre-roll,有效时间线但不整块写环,防负索引越界)。
    i64 t0 = 0;
    bool haveTimeline = false;
    bool playing = false;
    if (juce::AudioPlayHead* ph = getPlayHead())
    {
        const auto pos = ph->getPosition();
        if (pos.hasValue())
        {
            const auto t = pos->getTimeInSamples();
            if (t.hasValue())
            {
                haveTimeline = true;
                t0 = *t;
                playing = pos->getIsPlaying();
            }
        }
    }

    const bool canWrite = claimed_ && ring_ != nullptr && ring_->isOpen();

    // block 级 epoch 跳变检测(§5.1 步骤 3):停走带静止重写不算跳变。
    if (canWrite && haveTimeline)
    {
        if (t0 >= 0)
        {
            if (t0 != expectedNext_ && (playing || t0 != lastT0_))
            {
                ring_->bumpEpoch();
            }
        }
        else if (t0 + total > 0 && 0 != expectedNext_)
        {
            ring_->bumpEpoch(); // 跨零点:以 0 为新起点
        }
    }

    // 分块循环(review 3 收口):oversized 块切成 ≤maxBlock_ 子块依次处理,
    // capBuf_ 保持 maxBlock_ 尺寸,processBlock 零堆分配(§8 实时线程规则)。
    const int chunk = (maxBlock_ > 0) ? maxBlock_ : 1;
    for (int start = 0; start < total; start += chunk)
    {
        const int n = juce::jmin(chunk, total - start);
        const i64 ct = t0 + start;

        if (canWrite && haveTimeline)
        {
            if (ct >= 0)
            {
                captureInterleaved(buffer, start, n);
                ring_->writeBlock(ct, capBuf_.data(), n);
            }
            else if (ct < 0 && ct + n > 0)
            {
                // 跨零点子块:只写 [0, ct+n) 尾段(G-6 / S1-P18)。
                captureInterleaved(buffer, start, n);
                const int from = static_cast<int>(-ct);
                const int tail = n - from;
                ring_->writeBlock(0, capBuf_.data() + static_cast<std::size_t>(from) * srcChannels_, tail);
            }
            // 纯负 t0 子块:不写环(§5.1 步骤 2)。
        }

        // 输出级仲裁(§5.1 步骤 6):对子块渲染,SilenceStage ⇄ PassthroughStage 80ms 等功率 ramp。
        float* ch[2];
        for (int c = 0; c < nch; ++c)
        {
            ch[c] = buffer.getWritePointer(c) + start;
        }
        stage_.render(ch, nch, n);
    }

    if (haveTimeline)
    {
        lastT0_ = t0;
        expectedNext_ = t0 + total;
    }
}

void InputProcessor::timerCallback()
{
    const u64 now = scvb::steadyNowMs();
    ++tick_;

    // 心跳 ~240ms(4Hz;25Hz 定时器每 6 tick 一次)。
    if ((tick_ % 6) == 0 && claimed_ && registry_ != nullptr)
    {
        registry_->heartbeatInput(channel_, now);
    }

    pollHealth(now);
}

bool InputProcessor::isHealthy(u64 now) const
{
    if (registry_ == nullptr || !registry_->isOpen())
    {
        return false;
    }
    scvb::OutputSlot* out = registry_->outputSlot();
    if (out == nullptr || out->state.load(std::memory_order_acquire) != scvb::kSlotActive)
    {
        return false;
    }
    const bool hbFresh = !scvb::isStaleDisplay(out->heartbeat_ms.load(std::memory_order_acquire), now);
    const u32 mask = out->connected_mask.load(std::memory_order_acquire);
    const bool maskHas = channel_ >= 1 && (mask & (1u << (channel_ - 1))) != 0;
    return hbFresh && maskHas; // [J12 联动]:心跳 ≤2000ms 且 connected_mask 含本 channel
}

void InputProcessor::pollHealth(u64 now)
{
    if (!claimed_ || registry_ == nullptr || !registry_->isOpen())
    {
        stage_.setTargetPassthrough(true);
        return;
    }

    const bool healthy = isHealthy(now);
    if (healthy)
    {
        unhealthySince_ = 0;
        if (stage_.targetPassthrough())
        {
            // 直通 → 静音:确认健康后立即 80ms ramp(J32,此方向无滞回)。
            setMutedFlag(false);
            stage_.setTargetPassthrough(false);
            silenceCommandedAt_ = now;
            mutePending_ = true;
        }
    }
    else
    {
        // 静音 → 直通:5s 滞回(J12/J32)。
        if (unhealthySince_ == 0)
        {
            unhealthySince_ = now;
        }
        else if (now - unhealthySince_ >= scvb::kHysteresisMs)
        {
            if (!stage_.targetPassthrough())
            {
                setMutedFlag(false);
                stage_.setTargetPassthrough(true);
                mutePending_ = false;
            }
        }
    }

    // muted 确认位:静音 ramp(80ms)完成后置位(§4.4-b)。用 100ms 固定延迟近似。
    if (mutePending_ && !stage_.targetPassthrough() && now - silenceCommandedAt_ >= 100)
    {
        setMutedFlag(true);
        mutePending_ = false;
    }
}

void InputProcessor::setMutedFlag(bool muted)
{
    if (registry_ == nullptr || !registry_->isOpen() || channel_ == 0)
    {
        return;
    }
    scvb::InputSlot* slot = registry_->inputSlot(channel_);
    if (slot == nullptr)
    {
        return;
    }
    if (muted)
    {
        slot->flags.fetch_or(scvb::kFlagMuted, std::memory_order_release);
    }
    else
    {
        slot->flags.fetch_and(~scvb::kFlagMuted, std::memory_order_release);
    }
}

void InputProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const u32 data[3] = {kStateAbi, group_, channel_};
    destData.setSize(sizeof(data));
    destData.copyFrom(data, 0, sizeof(data));
}

void InputProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes < static_cast<int>(sizeof(u32) * 3))
    {
        return;
    }
    const auto* p = static_cast<const u32*>(data);
    const u32 abi = p[0];
    const u32 group = p[1];
    const u32 channel = p[2];
    if (abi != kStateAbi)
    {
        return; // 拒载高版本(J40 语义;spike 只认 abi=1)
    }
    if (group >= 1 && group <= scvb::kMaxGroups)
    {
        stateGroup_ = group;
    }
    if (channel <= scvb::kMaxChannels)
    {
        stateChannel_ = channel;
    }
}

void InputProcessor::setCurrentProgram(int) {}
const juce::String InputProcessor::getProgramName(int)
{
    return {};
}
void InputProcessor::changeProgramName(int, const juce::String&) {}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new InputProcessor();
}
