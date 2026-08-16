// SPDX-License-Identifier: GPL-3.0-or-later
#include "OutputProcessor.h"

#include <algorithm>
#include <cstdlib>

#include "LeakPool.h"

namespace
{
// 新上线 channel 置 mask 位后延迟注入的时长(J32:≥200ms 或 Input muted 位先到者)。
inline constexpr unsigned long long kInjectDelayMs = 200;

bool parseEnvGroup(unsigned int& out)
{
    const char* v = std::getenv("SCVB_GROUP");
    if (v == nullptr || *v == '\0')
    {
        return false;
    }
    const long parsed = std::strtol(v, nullptr, 10);
    if (parsed < 1 || parsed > static_cast<long>(scvb::kMaxGroups))
    {
        return false;
    }
    out = static_cast<unsigned int>(parsed);
    return true;
}
} // namespace

OutputProcessor::OutputProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    parseEnvGroup(group_);
    // v3 崩溃修复:音频线程缓冲构造定容,prepareToPlay 不再分配(§8 零堆分配)。
    accumL_.assign(static_cast<std::size_t>(scvb::kMaxHostBlockFrames), 0.0f);
    accumR_.assign(static_cast<std::size_t>(scvb::kMaxHostBlockFrames), 0.0f);
    trackBuf_.assign(static_cast<std::size_t>(scvb::kMaxHostBlockFrames) * 2, 0.0f);
}

OutputProcessor::~OutputProcessor()
{
    stopTimer();
    if (outputActive_.load(std::memory_order_relaxed) == 1 && registry_ != nullptr)
    {
        registry_->releaseOutput(pid_);
    }
    // v4 崩溃修复:registry 对象交还进程寿命池,绝不析构(见 LeakPool.h);
    // ringsRaw_ 指向的对象早已入池。配合 SegmentView 泄漏,在途 readBlock 安全。
    if (registry_)
    {
        retiredRegistries().push_back(std::move(registry_));
    }
}

void OutputProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    maxBlock_ = samplesPerBlock;

    // v3 崩溃修复:accum/trackBuf 构造定容(kMaxHostBlockFrames / *2),此处不再分配;超限块由分块循环 clamp。
    jassert(maxBlock_ <= scvb::kMaxHostBlockFrames);
    busXfade_.prepare(sampleRate_, maxBlock_);

    // 每会话重置计数(→ ctrl 导出的 gapCount 从 0 起)。
    for (u32 ch = 0; ch < scvb::kMaxChannels; ++ch)
    {
        gapCount_[ch].store(0, std::memory_order_relaxed);
        overlapCount_[ch].store(0, std::memory_order_relaxed);
        epochSummary_[ch].store(0, std::memory_order_relaxed);
        onlineSince_[ch] = 0;
    }

    pid_ = static_cast<u32>(::GetCurrentProcessId());
    if (registry_ == nullptr || registry_->group() != group_)
    {
        registry_ = std::make_unique<scvb::Registry>(group_);
    }
    if (!registry_->isOpen())
    {
        registry_->open();
    }

    if (!isTimerRunning())
    {
        startTimerHz(25); // [M] 25Hz 轮询 + ~240ms 心跳/ctrl 刷新
    }
}

void OutputProcessor::releaseResources()
{
    // P0 修复:不再 stopTimer()。宿主挂起非渲染轨插件时(Render-in-Place 等)会调 releaseResources;
    // 停轮询会让 connected_mask 停更、Input 心跳失联 → mask 塌缩、恢复后总线静音。
    // 轮询/心跳定时器跨宿主挂起周期存活(析构仍会 stopTimer + releaseOutput)。
}

bool OutputProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
    {
        return false;
    }
    return layouts.getMainOutputChannelSet() == layouts.getMainInputChannelSet();
}

void OutputProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int total = buffer.getNumSamples();
    if (total <= 0)
    {
        return;
    }

    // 时间线(R2/R3 [J51]:负 t0 是有效时间线,不混音不读环;无时间线 → 总线直通)。
    i64 t0 = 0;
    bool noTimeline = false;
    bool negT0 = false;
    if (juce::AudioPlayHead* ph = getPlayHead())
    {
        const auto pos = ph->getPosition();
        if (pos.hasValue())
        {
            const auto t = pos->getTimeInSamples();
            if (t.hasValue())
            {
                t0 = *t;
                negT0 = (t0 < 0);
            }
            else
            {
                noTimeline = true;
            }
        }
        else
        {
            noTimeline = true;
        }
    }
    else
    {
        noTimeline = true;
    }

    const bool passthrough = (outputActive_.load(std::memory_order_relaxed) == 0 || noTimeline || negT0);
    // acquire:配对 pollChannels 的 release,ring 元数据可见(仅激活路径需要)。
    const u32 usable = passthrough ? 0u : usableMask_.load(std::memory_order_acquire);

    // 分块循环(review 3 收口):oversized 块切成 ≤maxBlock_ 子块依次处理,
    // accumL_/accumR_/trackBuf_ 与 BusXfade 内部缓存保持 maxBlock_ 尺寸,processBlock 零堆分配(§8)。
    const int chunk = (maxBlock_ > 0) ? juce::jmin(maxBlock_, scvb::kMaxHostBlockFrames) : 1;
    for (int start = 0; start < total; start += chunk)
    {
        const int n = juce::jmin(chunk, total - start);
        float* outCh[2];
        outCh[0] = buffer.getWritePointer(0) + start;
        outCh[1] = buffer.getWritePointer(1) + start;

        if (passthrough)
        {
            busXfade_.renderPassthrough(outCh, n);
            continue;
        }

        const i64 ct = t0 + start;
        std::fill(accumL_.begin(), accumL_.begin() + n, 0.0f);
        std::fill(accumR_.begin(), accumR_.begin() + n, 0.0f);

        for (u32 ch = 0; ch < scvb::kMaxChannels; ++ch)
        {
            if ((usable & (1u << ch)) == 0)
            {
                continue;
            }
            scvb::AudioRing* ring = ringsRaw_[ch];
            if (ring == nullptr || !ring->isOpen())
            {
                continue;
            }
            const u32 nch = ring->channels();
            const auto st = ring->readBlock(ct, n, trackBuf_.data());
            if (st != scvb::AudioRing::ReadStatus::kOk)
            {
                gapCount_[ch].fetch_add(1, std::memory_order_relaxed); // 缺口 → 该轨该块静音 + 计数
                continue;
            }
            if (nch == 1)
            {
                for (int i = 0; i < n; ++i)
                {
                    const float v = trackBuf_[static_cast<std::size_t>(i)];
                    accumL_[static_cast<std::size_t>(i)] += v;
                    accumR_[static_cast<std::size_t>(i)] += v;
                }
            }
            else
            {
                for (int i = 0; i < n; ++i)
                {
                    accumL_[static_cast<std::size_t>(i)] += trackBuf_[static_cast<std::size_t>(i) * 2];
                    accumR_[static_cast<std::size_t>(i)] += trackBuf_[static_cast<std::size_t>(i) * 2 + 1];
                }
            }
            epochSummary_[ch].store(ring->header()->epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }

        busXfade_.render(outCh, accumL_.data(), accumR_.data(), n);
    }
}

void OutputProcessor::timerCallback()
{
    const u64 now = scvb::steadyNowMs();
    ++tick_;

    if (registry_ == nullptr || !registry_->isOpen())
    {
        return;
    }

    tryClaim(now);

    if (outputActive_.load(std::memory_order_relaxed) == 1)
    {
        // 心跳 ~240ms(4Hz)。
        if ((tick_ % 6) == 0)
        {
            registry_->heartbeatOutput(now);
        }
        pollChannels(now);
        // ctrl 全局信息小节刷新 ~240ms(J09)。
        if ((tick_ % 6) == 0)
        {
            refreshCtrl();
        }
    }
}

void OutputProcessor::tryClaim(u64 now)
{
    if (outputActive_.load(std::memory_order_relaxed) == 1)
    {
        return;
    }
    if (!observer_)
    {
        const auto r = registry_->claimOutput(pid_, now);
        if (r == scvb::Registry::ClaimResult::kClaimed)
        {
            outputActive_.store(1, std::memory_order_relaxed);
        }
        else if (r == scvb::Registry::ClaimResult::kConflict)
        {
            observer_ = true; // ADR-002:第二个 Output 只读观察
        }
        return;
    }
    // observer:1Hz 重试(主实例正常卸载 state→0 后可接管)。
    if ((tick_ % 25) == 0)
    {
        const auto r = registry_->claimOutput(pid_, now);
        if (r == scvb::Registry::ClaimResult::kClaimed)
        {
            outputActive_.store(1, std::memory_order_relaxed);
            observer_ = false;
        }
    }
}

void OutputProcessor::pollChannels(u64 now)
{
    u32 connected = 0;
    u32 usable = 0;
    for (u32 ch = 0; ch < scvb::kMaxChannels; ++ch)
    {
        scvb::InputSlot* slot = registry_->inputSlot(ch + 1);
        bool online = false;
        if (slot != nullptr && slot->state.load(std::memory_order_acquire) == scvb::kSlotActive)
        {
            const bool hbFresh = !scvb::isStaleDisplay(slot->heartbeat_ms.load(std::memory_order_acquire), now);
            const bool srMatch = slot->sample_rate == static_cast<u32>(sampleRate_);
            online = hbFresh && srMatch;
            if (online)
            {
                connected |= (1u << ch);
                if (ringsRaw_[ch] == nullptr)
                {
                    // v4:对象创建后立即交还泄漏池(永不析构),仅发布裸指针。
                    auto ring = std::make_unique<scvb::AudioRing>(group_, ch + 1);
                    ringsRaw_[ch] = ring.get();
                    retiredRings().push_back(std::move(ring));
                }
                if (!ringsRaw_[ch]->isOpen())
                {
                    ringsRaw_[ch]->openForOutput();
                }
                if (onlineSince_[ch] == 0)
                {
                    onlineSince_[ch] = now;
                }
                // J32:置 mask 位后延迟 ≥200ms 再注入,或等 Input muted 确认位(先到者)。
                const bool muted = (slot->flags.load(std::memory_order_acquire) & scvb::kFlagMuted) != 0;
                const bool delayElapsed = (now - onlineSince_[ch] >= kInjectDelayMs);
                if (muted || delayElapsed)
                {
                    usable |= (1u << ch);
                }
            }
            else
            {
                onlineSince_[ch] = 0;
            }
        }
        else
        {
            onlineSince_[ch] = 0;
        }
    }
    usableMask_.store(usable, std::memory_order_release); // release:发布 ringsRaw_[ch] 元数据给 [A]
    // 写 connected_mask(Input 健康判定依据;SR 不符/离线/心跳陈旧 → mask bit=0)。
    scvb::OutputSlot* out = registry_->outputSlot();
    if (out != nullptr)
    {
        out->connected_mask.store(connected, std::memory_order_release);
    }
}

void OutputProcessor::refreshCtrl()
{
    if (!ctrlOpen_)
    {
        const auto r = scvb::SegmentBackendWin32::map(scvb::segmentCtrlName(group_), 16384, ctrlView_);
        if (r != scvb::InitResult::kOk)
        {
            return;
        }
        ctrl_ = static_cast<scvb::OutputGlobalInfo*>(ctrlView_.base);
        ctrlOpen_ = true;
    }
    if (ctrl_ == nullptr)
    {
        return;
    }
    ctrl_->capture_enabled.store(0, std::memory_order_relaxed); // spike 无特征段,采集态恒 0
    ctrl_->output_sample_rate.store(static_cast<u32>(sampleRate_), std::memory_order_relaxed);
    ctrl_->flags.store(scvb::kOutputEnabled, std::memory_order_relaxed);
    for (u32 ch = 0; ch < scvb::kMaxChannels; ++ch)
    {
        ctrl_->gap_count[ch].store(gapCount_[ch].load(std::memory_order_relaxed), std::memory_order_relaxed);
        ctrl_->overlap_count[ch].store(overlapCount_[ch].load(std::memory_order_relaxed), std::memory_order_relaxed);
        ctrl_->epoch_summary[ch].store(epochSummary_[ch].load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
}

void OutputProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const u32 data[2] = {1, group_};
    destData.setSize(sizeof(data));
    destData.copyFrom(data, 0, sizeof(data));
}

void OutputProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes < static_cast<int>(sizeof(u32) * 2))
    {
        return;
    }
    const auto* p = static_cast<const u32*>(data);
    if (p[0] != 1)
    {
        return;
    }
    if (p[1] >= 1 && p[1] <= scvb::kMaxGroups)
    {
        group_ = p[1];
    }
}

void OutputProcessor::setCurrentProgram(int) {}
const juce::String OutputProcessor::getProgramName(int)
{
    return {};
}
void OutputProcessor::changeProgramName(int, const juce::String&) {}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OutputProcessor();
}
