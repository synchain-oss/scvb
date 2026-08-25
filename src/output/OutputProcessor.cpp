// SPDX-License-Identifier: GPL-3.0-or-later
#include "OutputProcessor.h"

#include <algorithm>
#include <cmath>

#include "OutputEditor.h"
#include "SegmentEditService.h"
#include "UiDefaultsStore.h"
#include "ipc/RegistryProbe.h"
#include "output/MixMath.h"
#include "state/StateMigration.h"

namespace
{
constexpr int kGroupIdMax = 8; // [J66] 1..8
constexpr int kVersionMax = 2; // [J59] 1..2
constexpr int kTimelineInvalidTicks = 12; // 25Hz × 0.5s(§4.2 连续无效判定)
} // namespace

ScvbOutputAudioProcessor::ScvbOutputAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      session_(backend_, static_cast<scvb::u32>(::GetCurrentProcessId())),
      vizPublisher_(backend_, 1u), // [T44] 组号随 setGroupId/prepareToPlay 校正
      apvts(*this, nullptr, "PARAMETERS", createParameterLayout())
{
    setLatencySamples(0); // ADR-002:Output 不报额外 latency(对齐靠时间线,不靠 PDC)

    // 缩放的系统级全局默认(§1.29「保持」落盘的那一档);工程 state 若带 CFGS 会在
    // setStateInformation 里覆盖它 —— 工程 > 全局默认。0 = 从未「保持」过,沿用 100。
    if (const int defaultScale = scvb::output::uidefaults::uiScalePercent(); defaultScale > 0)
    {
        uiScale_ = defaultScale;
    }

    handles_ = scvb::params::collectParamHandles(apvts);
    printer_.setShot(&playheadShot_);
    printer_.installHostEchoShield(apvts);

    // CRVS 段真身默认版本名([J05]:空值回落 V{n};加载 CRVS 时会被覆盖)。
    crvsData_.versions[0].meta.name = "V1";
    crvsData_.versions[1].meta.name = "V2";
}

ScvbOutputAudioProcessor::~ScvbOutputAudioProcessor()
{
    stopTimer();
    const juce::ScopedLock lock(lifecycleMutex_);
    session_.release(scvb::steadyNowMs());
}

juce::AudioProcessorValueTreeState::ParameterLayout ScvbOutputAudioProcessor::createParameterLayout()
{
    // T15:123 参数冻结布局(params-v0 v2.2 §一 / J59 / J65)。
    return scvb::params::makeOutputLayout();
}

void ScvbOutputAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const juce::ScopedLock lock(lifecycleMutex_);

    sampleRate_.store(sampleRate > 0.0 ? sampleRate : 48000.0, std::memory_order_relaxed); // 原子写(PR#55 第9轮)
    const double sr = sampleRate_.load(std::memory_order_relaxed);
    preparedMaxBlock_ = samplesPerBlock > 0 ? samplesPerBlock : 512;
    accumL_.assign(static_cast<std::size_t>(preparedMaxBlock_), 0.0f);
    accumR_.assign(static_cast<std::size_t>(preparedMaxBlock_), 0.0f);
    for (auto& tb : trackBuf_)
    {
        tb.assign(static_cast<std::size_t>(2) * static_cast<std::size_t>(preparedMaxBlock_), 0.0f);
    }

    handles_ = scvb::params::collectParamHandles(apvts);

    const auto now = scvb::steadyNowMs();
    session_.prepare(static_cast<scvb::u32>(sr), static_cast<scvb::u32>(preparedMaxBlock_), now);
    authority_.prepare(sr, handles_);
    busXfade_.prepare(sr);
    busXfade_.resetToPassthrough();

    // 平滑器:ms_balance g_M/g_S 与全局 width 10ms;per-channel 注入 fade 80ms(§5.2 过渡语义)。
    gMSmoother_.reset(sr, 0.010);
    gSSmoother_.reset(sr, 0.010);
    globalWidthSmoother_.reset(sr, 0.010);
    for (auto& f : channelFade_)
    {
        f.reset(sr, 0.080);
    }

    // 打印器接线(C8 setShot 已在构造完成;此处重绑车道 + 启 50Hz)。
    rebindVersion();
    printer_.startPrinting();

    lastT0Out_ = std::numeric_limits<int64_t>::lowest();
    expectedNextOut_ = std::numeric_limits<int64_t>::lowest();
    prepared_ = true;

    // [T44/J75] viz 段**不在这里建**。建/释放一律交给 [M] 每拍按 claim 态裁决(syncVizSegment)——
    // 理由见 R1:同组第二个 Output 是 kObserver([J66]「同组内只读观察」),它若也建段并 4Hz 写,
    // 两个写方会同时推同一个 seqlock,读方拿到的帧可以是两次发布的拼接(seq 偶数、内容却撕裂),
    // 而且**看起来完全正常**。此处只把段指向当前组。
    vizPublisher_.setGroup(static_cast<scvb::u32>(groupId_));

    startTimerHz(25);
}

void ScvbOutputAudioProcessor::releaseResources()
{
    const juce::ScopedLock lock(lifecycleMutex_);
    session_.release(scvb::steadyNowMs());
    vizPublisher_.release(); // [T44] viz 段与主链路同生命周期
    printer_.endAllGestures();
    prepared_ = false;
    sampleRate_.store(
        0.0, std::memory_order_relaxed); // 复位:isPrepared()/sr 守卫在 release 后回到「未 prepare」(PR#55 第10轮缺陷1)
}

void ScvbOutputAudioProcessor::rebindVersion()
{
    authority_.setVersionActive(versionActive_);
    rebuildAllCurves(); // 由 CRVS 段真身重建曲线 + 打印器重取活动版本曲线(T29)
    printer_.bindVersion(versionActive_, handles_);
}

bool ScvbOutputAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // ADR-003:Output 总线固定 stereo(读 15 环求和后替换总线)。
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() &&
           layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
}

float ScvbOutputAudioProcessor::readGlobalWidth() const noexcept
{
    return handles_.rawWidth != nullptr ? handles_.rawWidth->load(std::memory_order_relaxed) : 100.0f;
}

float ScvbOutputAudioProcessor::readMsBalance() const noexcept
{
    return handles_.rawMsBalance != nullptr ? handles_.rawMsBalance->load(std::memory_order_relaxed) : 0.0f;
}

void ScvbOutputAudioProcessor::publishPlayhead(const juce::AudioPlayHead::PositionInfo& pos, bool haveTime,
                                               bool playing)
{
    scvb::engine::PlayheadPod pod;
    pod.sampleRate = sampleRate_.load(std::memory_order_relaxed); // 原子读(PR#55 第9轮)
    pod.epoch = podEpoch_;
    pod.timeSamples = haveTime ? *pos.getTimeInSamples() : -1;
    if (playing)
    {
        pod.flags |= scvb::engine::kPlayheadIsPlaying;
    }
    if (pos.getIsLooping())
    {
        pod.flags |= scvb::engine::kPlayheadIsLooping;
    }
    if (const auto ppq = pos.getPpqPosition(); ppq.hasValue())
    {
        pod.ppq = *ppq;
        pod.flags |= scvb::engine::kPlayheadMusicValid;
    }
    if (const auto bpm = pos.getBpm(); bpm.hasValue())
    {
        pod.bpm = *bpm;
        pod.flags |= scvb::engine::kPlayheadTempoValid;
    }
    if (const auto loop = pos.getLoopPoints(); loop.hasValue())
    {
        pod.loopStartPpq = loop->ppqStart;
        pod.loopEndPpq = loop->ppqEnd;
        pod.flags |= scvb::engine::kPlayheadCycleValid;
    }
    playheadShot_.publish(pod);
}

void ScvbOutputAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    const juce::ScopedNoDenormals noDenormals;

    // 越界夹取(research/01 §2.3,Bridge #169 教训)。
    const int n = juce::jmin(buffer.getNumSamples(), preparedMaxBlock_);
    if (n <= 0)
    {
        return;
    }

    session_.bumpBlockCounter(); // [J52] 存活计数 → [M] 停摆看门狗(§4.2)

    // 时间线定位(§5.2 步骤 2)。
    bool playing = false;
    bool haveT0 = false;
    int64_t t0 = 0;
    juce::AudioPlayHead::PositionInfo pos = juce::AudioPlayHead::PositionInfo{};
    if (juce::AudioPlayHead* ph = getPlayHead())
    {
        const juce::Optional<juce::AudioPlayHead::PositionInfo> p = ph->getPosition();
        if (p.hasValue())
        {
            pos = *p;
            playing = pos.getIsPlaying();
            if (const juce::Optional<int64_t> ts = pos.getTimeInSamples(); ts.hasValue())
            {
                t0 = *ts;
                haveT0 = true;
            }
        }
    }

    // pod epoch(§5.2 步骤 2;停走带静止 t0 不变不算跳变)。
    if (haveT0 && t0 >= 0)
    {
        if (t0 != expectedNextOut_ && (playing || t0 != lastT0Out_))
        {
            ++podEpoch_;
        }
        lastT0Out_ = t0;
        expectedNextOut_ = t0 + n;
    }
    publishPlayhead(pos, haveT0, playing);

    const bool noTimeline = !haveT0;
    const bool negT0 = haveT0 && t0 < 0;
    const bool observer = session_.state() != scvb::output::OutputClaimState::kActive;

    if (observer || noTimeline || negT0)
    {
        // 只读/无时间线/负 t0:总线直通(不混音不读环)。
        if (noTimeline)
        {
            timelineInvalidBlocks_.fetch_add(1, std::memory_order_relaxed); // 仅「无时间线」计入(§4.2)
            timelineValid_.store(0, std::memory_order_relaxed);
        }
        else
        {
            timelineInvalidBlocks_.store(0, std::memory_order_relaxed);
            timelineValid_.store(1, std::memory_order_relaxed); // 负 t0 是有效时间线([J51])
        }
        const float* const* in = buffer.getArrayOfReadPointers();
        float* const* out = buffer.getArrayOfWritePointers();
        const float* lastMix[2] = {accumL_.data(), accumR_.data()};
        busXfade_.render(out, in, lastMix, n, /*targetMix=*/false);
        return;
    }

    timelineInvalidBlocks_.store(0, std::memory_order_relaxed);
    timelineValid_.store(1, std::memory_order_relaxed);

    const scvb::u32 inject = session_.injectMask();
    if (inject == 0)
    {
        // 无注入轨(全部离线/注入延迟中):总线直通,经同一 busXfade 状态机(无硬切)。
        const float* const* in = buffer.getArrayOfReadPointers();
        float* const* out = buffer.getArrayOfWritePointers();
        const float* lastMix[2] = {accumL_.data(), accumR_.data()};
        busXfade_.render(out, in, lastMix, n, /*targetMix=*/false);
        return;
    }

    // 仲裁 + arm 平滑(整 block 用同一份快照;engineAuthority = output_enabled)。
    const double tSec = static_cast<double>(t0) / sampleRate_.load(std::memory_order_relaxed); // 原子读(PR#55 第9轮)
    authority_.processBlock(session_.outputEnabled(), tSec);

    // 读注入 channel 的环(covered/换代/套圈判定在 ShmRingMixSource::read)。
    std::array<bool, 15> hasData{};
    std::array<scvb::u32, 15> nch{};
    for (int ch = 1; ch <= 15; ++ch)
    {
        if ((inject & (1u << (ch - 1))) == 0)
        {
            continue;
        }
        auto& src = session_.mixSource(static_cast<scvb::u32>(ch));
        if (!src.bound())
        {
            continue;
        }
        if (src.read(t0, trackBuf_[static_cast<std::size_t>(ch - 1)].data(), n))
        {
            hasData[static_cast<std::size_t>(ch - 1)] = true;
            nch[static_cast<std::size_t>(ch - 1)] = src.channels();
        }
    }

    // arm 平滑器:per-channel 注入 fade(上线/下线 80ms)+ 全局 width + ms_balance g_M/g_S(10ms)。
    for (int ch = 0; ch < 15; ++ch)
    {
        channelFade_[static_cast<std::size_t>(ch)].setTargetValue((inject & (1u << ch)) != 0 ? 1.0f : 0.0f);
    }
    globalWidthSmoother_.setTargetValue(readGlobalWidth());
    const float ms = readMsBalance();
    const float tms = ms / 100.0f;
    gMSmoother_.setTargetValue(1.0f - std::max(tms, 0.0f));
    gSSmoother_.setTargetValue(1.0f - std::max(-tms, 0.0f));

    // 逐样本:取值(平滑)→ gain/pan(mono equal-power / stereo dual-pan+width)→ 求和 → ms_balance。
    for (int i = 0; i < n; ++i)
    {
        const auto tv = authority_.nextSample();
        const float gw = globalWidthSmoother_.getNextValue();
        const float gM = gMSmoother_.getNextValue();
        const float gS = gSSmoother_.getNextValue();

        float l = 0.0f;
        float r = 0.0f;
        for (int ch = 0; ch < 15; ++ch)
        {
            const float fade = channelFade_[static_cast<std::size_t>(ch)].getNextValue();
            if (!hasData[static_cast<std::size_t>(ch)])
            {
                continue;
            }
            const auto& t = tv[static_cast<std::size_t>(ch)];
            if (nch[static_cast<std::size_t>(ch)] == 1)
            {
                scvb::output::mixMonoSample(trackBuf_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)], t.pan,
                                            t.volDb, gw, fade, l, r);
            }
            else
            {
                scvb::output::mixStereoSample(
                    trackBuf_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(2 * i)],
                    trackBuf_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(2 * i + 1)], t.pan, t.volDb,
                    t.width, gw, fade, l, r);
            }
        }
        scvb::output::applyMsGains(gM, gS, l, r); // 总线级 M/S 音量比(求和之后、替换之前,[J58])
        accumL_[static_cast<std::size_t>(i)] = l;
        accumR_[static_cast<std::size_t>(i)] = r;
    }

    // 替换总线(ADR-002):稳态=完全替换;进出瞬间经 busXfade 等功率交叉(§5.2 过渡语义)。
    const float* const* in = buffer.getArrayOfReadPointers();
    float* const* out = buffer.getArrayOfWritePointers();
    const float* mix[2] = {accumL_.data(), accumR_.data()};
    busXfade_.render(out, in, mix, n, /*targetMix=*/true);
}

void ScvbOutputAudioProcessor::renderBypassedUnity(juce::AudioBuffer<float>& buffer, int n, int64_t t0)
{
    // §5.4:按时间线读 15 环做 unity 求和(mono 居中复制到 L/R、stereo L→L/R→R;不 gain/pan/width)。
    // PR#53 I1:无注入轨或无可读源 → 直通(不改 buffer),绝不把总线替换成静音。
    // PR#53 复审:早退发生在清零之前 —— 无数据时不清 accum,保留 lastMix 供后续「混音→直通」等功率交叉。
    const scvb::u32 inject = session_.injectMask();
    if (inject == 0)
    {
        return; // 总线上只挂 Output / 尚无 Input:直通
    }

    // 第一遍:读注入源到 trackBuf_,只判定「是否有可读数据」,不触碰 accum。
    std::array<bool, 15> hasData{};
    std::array<scvb::u32, 15> srcCh{};
    bool anyData = false;
    for (int ch = 1; ch <= 15; ++ch)
    {
        if ((inject & (1u << (ch - 1))) == 0)
        {
            continue;
        }
        auto& src = session_.mixSource(static_cast<scvb::u32>(ch));
        if (!src.bound())
        {
            continue;
        }
        if (!src.read(t0, trackBuf_[static_cast<std::size_t>(ch - 1)].data(), n))
        {
            continue; // 缺口 → 该轨该块静音(失准计数已在 read 内)
        }
        hasData[static_cast<std::size_t>(ch - 1)] = true;
        srcCh[static_cast<std::size_t>(ch - 1)] = src.channels();
        anyData = true;
    }

    if (!anyData)
    {
        return; // 有注入轨但均无可读数据(缺口):直通,不清 accum(保留 lastMix)
    }

    // 确认有数据才清零并求和。
    std::fill(accumL_.begin(), accumL_.begin() + n, 0.0f);
    std::fill(accumR_.begin(), accumR_.begin() + n, 0.0f);
    for (int ch = 0; ch < 15; ++ch)
    {
        if (!hasData[static_cast<std::size_t>(ch)])
        {
            continue;
        }
        const scvb::u32 chCount = srcCh[static_cast<std::size_t>(ch)];
        for (int i = 0; i < n; ++i)
        {
            if (chCount == 1)
            {
                const float s = trackBuf_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)];
                accumL_[static_cast<std::size_t>(i)] += s;
                accumR_[static_cast<std::size_t>(i)] += s;
            }
            else
            {
                accumL_[static_cast<std::size_t>(i)] +=
                    trackBuf_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(2 * i)];
                accumR_[static_cast<std::size_t>(i)] +=
                    trackBuf_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(2 * i + 1)];
            }
        }
    }

    auto* outL = buffer.getWritePointer(0);
    auto* outR = buffer.getWritePointer(1);
    for (int i = 0; i < n; ++i)
    {
        outL[i] = accumL_[static_cast<std::size_t>(i)];
        outR[i] = accumR_[static_cast<std::size_t>(i)];
    }
}

void ScvbOutputAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer,
                                                    juce::MidiBuffer& /*midiMessages*/)
{
    const juce::ScopedNoDenormals noDenormals;
    const int n = juce::jmin(buffer.getNumSamples(), preparedMaxBlock_);
    if (n <= 0)
    {
        return;
    }
    session_.bumpBlockCounter(); // [J52] 存活计数 → [M] 停摆看门狗(§4.2);bypass 期间同样推进

    int64_t t0 = 0;
    bool haveT0 = false;
    if (juce::AudioPlayHead* ph = getPlayHead())
    {
        const juce::Optional<juce::AudioPlayHead::PositionInfo> p = ph->getPosition();
        if (p.hasValue())
        {
            if (const juce::Optional<int64_t> ts = p->getTimeInSamples(); ts.hasValue() && *ts >= 0)
            {
                t0 = *ts;
                haveT0 = true;
            }
        }
    }
    if (!haveT0)
    {
        return; // 无时间线:直通(不替换)
    }
    renderBypassedUnity(buffer, n, t0);
}

void ScvbOutputAudioProcessor::timerCallback()
{
    const auto now = scvb::steadyNowMs();
    const juce::ScopedLock lock(lifecycleMutex_);

    // 4Hz 心跳(250ms 折半在 25Hz 定时器内)。
    if (now - lastHeartbeatMs_ >= scvb::kHeartbeatIntervalMs)
    {
        lastHeartbeatMs_ = now;
        session_.heartbeat(now);
    }

    session_.tick(now);

    // 时间线健康前置(§4.2 [J51]):连续无时间线 ≥0.5s → 清 mask(Inputs 走 J12 直通)。
    if (timelineValid_.load(std::memory_order_relaxed) == 0)
    {
        if (++timelineInvalidTicks_ >= kTimelineInvalidTicks)
        {
            session_.forceClearMask();
            timelineInvalidTicks_ = kTimelineInvalidTicks; // 钳住,避免重复清
            // [J51] 诊断:上报连续无时间线期间累计的无效块数(timelineInvalidBlocks_ 接线落点)。
            DBG("SCVB Output: timeline invalid ≥0.5s, clearing inject mask ("
                << timelineInvalidBlocks_.load(std::memory_order_relaxed) << " invalid blocks)");
        }
    }
    else
    {
        timelineInvalidTicks_ = 0;
    }

    // 打印器模式:输出开关 ON → 引擎权威;OFF → follow host。T24 无分析曲线,Armed 与 Print 等效。
    printer_.setMode(outputEnabled_ ? scvb::engine::AuthorityMode::Armed : scvb::engine::AuthorityMode::Follow);

    // [T44/J75] viz 段:先按 claim 态裁决建/释放(唯一写方 = kActive 的那个 Output),再发布。
    syncVizSegment();
    publishVizFrame(now);
}

void ScvbOutputAudioProcessor::syncVizSegment()
{
    // viz 段的唯一写方 = **本组 claim 到 OutputSlot 的那一个** Output([J66] 同组内只读观察)。
    // claim 态会在运行中翻转(接管 / 让位 / 改组 / 段不可用),所以这条判定必须**每拍**做,
    // 不能只在 prepareToPlay 做一次 —— 否则「启动时是 observer、后来接管成功」的实例永远不发布,
    // 而「启动时 active、后来让位」的实例会与新主实例一起写同一个 seqlock。
    const bool shouldWrite = prepared_ && session_.state() == scvb::output::OutputClaimState::kActive;
    if (shouldWrite && !vizPublisher_.isOpen())
    {
        vizPublisher_.open(); // 失败(权限/内存)不影响主链路 —— Monitor 看到空态即可,下拍再试
    }
    else if (!shouldWrite && vizPublisher_.isOpen())
    {
        vizPublisher_.release(); // 让位 / 改组 / 未 prepare:立刻松手,不留第二个写方
    }
}

void ScvbOutputAudioProcessor::publishVizFrame(std::uint64_t nowMs)
{
    // 调用方已持 lifecycleMutex_(timerCallback):可直接读 crvsData_,免去 crvsSnapshot() 的深拷贝。
    // 双保险:syncVizSegment 已按 claim 态裁决过,这里再判一次。
    if (!vizPublisher_.isOpen())
    {
        return;
    }
    // 4Hz 闸门**前置**:下面要采 15 个轨名(每个一次 toStdString 堆分配)+ 曲线指针,
    // 而 tick() 只在 250ms 边界真的用得上 —— 25Hz 全采是白烧消息线程。
    if (!vizPublisher_.due(nowMs))
    {
        return;
    }

    scvb::output::VizPublishInput in;
    in.crvs = &crvsData_;
    in.curves = authority_.activeCurves();
    in.versionActive = static_cast<scvb::u32>(versionActive_);
    in.crvsRevision = crvsRevision_.load(std::memory_order_acquire);
    in.sampleRate = sampleRate_.load(std::memory_order_relaxed);
    in.playhead = playheadSnapshot();

    const int v = juce::jlimit(1, kVersionMax, versionActive_);
    // [N1] metaRevision **只哈希轨名**。width 走帧头段、每帧都刷,与 writeLanes 无关 ——
    // 把它掺进来会让「width 被自动化」变成 needLanes 恒真,每秒 15360 次曲线求值。
    // FNV-1a 64 位:碰撞概率可忽略,不再靠时间兜底兜住碰撞(见 kLaneRefreshMaxMs)。
    std::uint64_t meta = 1469598103934665603ull ^ static_cast<std::uint64_t>(v);
    const auto fnv = [&meta](unsigned char byte) { meta = (meta ^ byte) * 1099511628211ull; };
    for (int ch = 0; ch < 15; ++ch)
    {
        const auto& c = runtime_.channels[static_cast<std::size_t>(ch)];
        if (c.enabled)
        {
            in.enabledMask |= (1u << ch);
        }
        if (c.sourceChannels == 2)
        {
            in.stereoMask |= (1u << ch);
        }
        if (c.leadLock)
        {
            in.leadMask |= (1u << ch);
        }
        auto& label = in.label[static_cast<std::size_t>(ch)];
        label = c.label.toStdString();
        // 每轨 width:活动版本的参数 raw atomic(engineering 0..100)。句柄未就绪 → NaN → 段内哨兵。
        const auto* raw = handles_.rawTrkW[v - 1][ch];
        in.widthPct[static_cast<std::size_t>(ch)] =
            raw != nullptr ? raw->load(std::memory_order_relaxed) : std::numeric_limits<float>::quiet_NaN();
        for (const char byte : label)
        {
            fnv(static_cast<unsigned char>(byte));
        }
        fnv(0); // 分隔符:防「AB|C」与「A|BC」哈希相同
    }
    // 64 位哈希折成 32 位存进 metaRevision(只用于相等比较,折叠不降低碰撞抗性到可担忧的程度)。
    in.metaRevision = static_cast<scvb::u32>(meta ^ (meta >> 32));
    vizPublisher_.tick(nowMs, in);
}

void ScvbOutputAudioProcessor::setCurrentProgram(int /*index*/) {}

const juce::String ScvbOutputAudioProcessor::getProgramName(int /*index*/)
{
    return {};
}

void ScvbOutputAudioProcessor::changeProgramName(int /*index*/, const juce::String& /*newName*/) {}

void ScvbOutputAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const juce::ScopedLock lock(lifecycleMutex_);

    // PR#53 R1:拒载更高 abi 后原样回写宿主原始字节(preservedOriginal 语义,Input PR#51 重要#2 同款),
    // 绝不把高版本 blob 覆盖成当前版数据。
    if (stateAbiMismatch_ && !preservedStateBlob_.empty())
    {
        destData.append(preservedStateBlob_.data(), preservedStateBlob_.size());
        return;
    }

    // 从上次成功加载的容器重建:原位替换 PRMS/CFGS,其余 chunk(FEAT/CRVS/未知 fourcc)原样回写(T19)。
    scvb::state::StateChunks chunks = loadedChunks_;
    chunks.abi = scvb::state::kCurrentAbi;

    // PRMS:123 参数(ValueTree XML 二进制,host 自动化面)。
    const auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    juce::MemoryBlock paramsBlock;
    copyXmlToBinary(*xml, paramsBlock);
    const auto* pdata = static_cast<const std::uint8_t*>(paramsBlock.getData());
    chunks.set(scvb::state::kFourccPrms, std::vector<std::uint8_t>(pdata, pdata + paramsBlock.getSize()));

    // CFGS:Output 配置(group_id / 采集 / 输出 / 版本 / ui,[J66] 最小 T24 子集)。
    scvb::state::OutputState s;
    s.groupId = static_cast<scvb::u32>(groupId_);
    s.captureEnabled = captureEnabled_ ? 1u : 0u;
    s.outputEnabled = outputEnabled_ ? 1u : 0u;
    s.versionActive = static_cast<scvb::u32>(versionActive_);
    s.uiScale = static_cast<scvb::u32>(uiScale_);
    s.uiLanguage = uiLanguage_.toStdString();
    // 首启已读位随工程走(STATE_SCHEMA §一 ui 组):不落盘则重开工程红字页/tour 每次重放(T37 A-3)。
    s.uiGuideSeen = runtime_.guideSeen ? 1u : 0u;
    s.uiTourSeen = runtime_.tourSeen ? 1u : 0u;
    std::vector<std::uint8_t> cfg;
    if (!scvb::state::encodeOutputState(s, cfg))
    {
        return;
    }
    chunks.set(scvb::state::kFourccCfgs, std::move(cfg));

    // [J75] T43:ui.master_chart_mode 独立 UICF chunk(恒写 4 字节 u32,0=distribution | 1=trajectory)。
    std::vector<std::uint8_t> uicf;
    const std::uint32_t chartMode = (masterChartMode_ == "trajectory") ? scvb::state::kMasterChartModeTrajectory
                                                                       : scvb::state::kMasterChartModeDistribution;
    if (!scvb::state::encodeUiConfig(chartMode, uicf))
    {
        return;
    }
    chunks.set(scvb::state::kFourccUiConfig, std::move(uicf));

    // CRVS:段真身(版本名/段表/pan_curve)从 live crvsData_ 编码(T29;覆盖 loadedChunks_ 的旧 CRVS)。
    std::vector<std::uint8_t> crvs;
    if (scvb::state::encodeCrvs(crvsData_, crvs))
        chunks.set(scvb::state::kFourccCrvs, std::move(crvs));

    std::vector<std::uint8_t> blob;
    if (!scvb::state::encodeContainer(chunks, blob))
    {
        return;
    }
    destData.append(blob.data(), blob.size());
}

void ScvbOutputAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
    {
        return;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const std::size_t size = static_cast<std::size_t>(sizeInBytes);

    // PR#53 R1:经 loadState 做 abi 判读 + 迁移(高 abi 拒载、低 abi 走迁移链;未知 chunk 原样保留)。
    scvb::state::StateChunks chunks;
    const scvb::state::StateLoadResult res = scvb::state::loadState(bytes, size, chunks);

    const juce::ScopedLock lock(lifecycleMutex_);

    // 冻结契约(CLAUDE.md §7.3 / STATE_SCHEMA):读到高版本 abi → 拒载并提示升级,绝不静默降级;
    // 同时保留原字节供 getStateInformation 原样回写(Input PR#51 红旗#1/重要#2 同款)。
    if (res.status == scvb::state::StateLoadStatus::RejectedNewer)
    {
        scvb::state::StateHeader hdr;
        stateAbiMismatch_ = true;
        stateAbiSeen_ = scvb::state::parseHeader(bytes, size, hdr) ? hdr.abi : 0u;
        preservedStateBlob_.assign(bytes, bytes + size);
        DBG("SCVB Output: state abi " << stateAbiSeen_ << " > current " << scvb::state::kCurrentAbi
                                      << "; refusing load (upgrade required)");
        return;
    }
    if (res.status != scvb::state::StateLoadStatus::Ok && res.status != scvb::state::StateLoadStatus::Migrated)
    {
        return; // Corrupt:不可信字节,拒载(不崩溃、不半填充)
    }

    stateAbiMismatch_ = false;
    preservedStateBlob_.clear();
    loadedChunks_ = chunks; // 保真 FEAT/CRVS/未知 fourcc 供 save 原样回写(T19 未知 fourcc 回写纪律)

    // PRMS:123 参数(宿主自动化面)。
    if (const scvb::state::Chunk* prms = chunks.find(scvb::state::kFourccPrms); prms != nullptr)
    {
        std::unique_ptr<juce::XmlElement> xml(
            getXmlFromBinary(prms->payload.data(), static_cast<int>(prms->payload.size())));
        if (xml != nullptr)
        {
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
            handles_ = scvb::params::collectParamHandles(apvts);
        }
    }

    const scvb::state::Chunk* cfg = chunks.find(scvb::state::kFourccCfgs);
    if (cfg == nullptr)
    {
        return;
    }
    scvb::state::OutputState s;
    if (!scvb::state::decodeOutputState(cfg->payload.data(), cfg->payload.size(), s))
    {
        return; // 范围校验失败 → 拒载(CLAUDE.md §7.3)
    }

    groupId_ = static_cast<int>(s.groupId);
    captureEnabled_ = s.captureEnabled != 0;
    outputEnabled_ = s.outputEnabled != 0;
    versionActive_ = static_cast<int>(s.versionActive);
    uiScale_ = static_cast<int>(s.uiScale);
    uiLanguage_ = juce::String::fromUTF8(s.uiLanguage.c_str(), static_cast<int>(s.uiLanguage.size()));

    // [J75] T43:ui.master_chart_mode 从独立 UICF chunk 读;缺失/长度非法/未知值均回落默认(§7.3)。
    std::uint32_t chartMode = scvb::state::kMasterChartModeDistribution;
    if (const scvb::state::Chunk* uicf = chunks.find(scvb::state::kFourccUiConfig); uicf != nullptr)
    {
        (void)scvb::state::decodeUiConfig(uicf->payload.data(), uicf->payload.size(), chartMode);
    }
    masterChartMode_ = (chartMode == scvb::state::kMasterChartModeTrajectory) ? juce::String("trajectory")
                                                                              : juce::String("distribution");

    // 首启已读位是**工程位**;跨工程的「不再显示」由快照的 *_global 单独承载(§1.32/§1.33),
    // 两者不在此合流 —— 合流会把全局位写进从没看过引导的工程的 state。
    runtime_.guideSeen = s.uiGuideSeen != 0;
    runtime_.tourSeen = s.uiTourSeen != 0;
    session_.setCaptureEnabled(captureEnabled_);
    session_.setOutputEnabled(outputEnabled_);

    // CRVS:段真身解码(版本名/段表/pan_curve)。成功 → 替换;无 chunk 或解码失败 → 重置全新默认
    // (不残留旧编辑写回,PR#55 第10轮缺陷2)。无论结果都 +修订号刷新段表(PR#55 第8轮缺陷1)。
    bool crvsLoaded = false;
    if (const scvb::state::Chunk* crvs = chunks.find(scvb::state::kFourccCrvs); crvs != nullptr)
    {
        scvb::state::CrvsData decoded;
        if (scvb::state::decodeCrvs(crvs->payload.data(), crvs->payload.size(), decoded))
        {
            crvsData_ = std::move(decoded);
            crvsLoaded = true;
        }
    }
    if (!crvsLoaded)
    {
        crvsData_ = scvb::state::CrvsData{};
        crvsData_.versions[0].meta.name = "V1"; // 默认版本名([J05])
        crvsData_.versions[1].meta.name = "V2";
    }
    crvsRevision_.fetch_add(1, std::memory_order_release);

    // 加载 state 后 CRVS 已整体替换 → 清空 UndoManager,否则 undo() 会恢复加载前的旧 CRVS 快照,
    // 静默丢弃刚加载的段数据(PR#55 第12轮;关闭 #48 tech-debt「fromState 清 undo」的桥面同款)。
    // 桥的 UndoManager 只含 CRVS 事务(editSegment/setVersionName/copyVersion/setTrackManual/setPanCurve,
    // 均写 crvsData_),无其它事务类别 → 全清口径安全(在 lifecycleMutex_ 内)。
    authority_.undoManager().clearUndoHistory();

    // 绑定时序(03 §7.2):setStateInformation 后 claim;样本率等 prepareToPlay 提供。
    if (prepared_)
    {
        // PR#53 缺陷1:已 prepared 加载到不同 group 的 state → 走 changeGroup(释放旧 OutputSlot 与旧环
        // 绑定 → 新组 claim 后 attach 新环);直接 prepare() 会让旧 group OutputSlot 保持 active 且已绑
        // sources 继续读旧环。group 未变则 prepare() 幂等 re-claim。
        const scvb::u32 newGroup = static_cast<scvb::u32>(groupId_);
        if (session_.groupId() != newGroup)
        {
            session_.changeGroup(newGroup, static_cast<scvb::u32>(sampleRate_.load(std::memory_order_relaxed)),
                                 static_cast<scvb::u32>(preparedMaxBlock_), scvb::steadyNowMs());
        }
        else
        {
            session_.prepare(static_cast<scvb::u32>(sampleRate_.load(std::memory_order_relaxed)),
                             static_cast<scvb::u32>(preparedMaxBlock_), scvb::steadyNowMs());
        }
        rebindVersion();
    }
    else
    {
        session_.setGroupId(static_cast<scvb::u32>(groupId_));
    }
}

void ScvbOutputAudioProcessor::setGroupId(int groupId)
{
    groupId = juce::jlimit(1, kGroupIdMax, groupId);
    const juce::ScopedLock lock(lifecycleMutex_);
    if (groupId == groupId_)
    {
        return;
    }
    groupId_ = groupId;
    session_.setGroupId(static_cast<scvb::u32>(groupId));
    // 改组(J66):释放旧组 OutputSlot → 新组 claim(期间输出直通,§4.2)。
    if (prepared_)
    {
        session_.changeGroup(static_cast<scvb::u32>(groupId),
                             static_cast<scvb::u32>(sampleRate_.load(std::memory_order_relaxed)),
                             static_cast<scvb::u32>(preparedMaxBlock_), scvb::steadyNowMs());
        // [T44] viz 段随组切换:**只换指向,不建段** —— 新组里本实例可能是 kObserver,
        // 建不建由下一拍的 syncVizSegment() 按 claim 态裁决([J66])。
        vizPublisher_.setGroup(static_cast<scvb::u32>(groupId));
    }
}

void ScvbOutputAudioProcessor::setCaptureEnabled(bool on)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    captureEnabled_ = on;
    session_.setCaptureEnabled(on);
}

void ScvbOutputAudioProcessor::setOutputEnabled(bool on)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    outputEnabled_ = on;
    session_.setOutputEnabled(on);
}

void ScvbOutputAudioProcessor::setVersionActive(int version)
{
    version = juce::jlimit(1, kVersionMax, version);
    const juce::ScopedLock lock(lifecycleMutex_);
    versionActive_ = version;
    if (prepared_)
    {
        rebindVersion();
    }
}

void ScvbOutputAudioProcessor::setMasterChartMode(const juce::String& mode)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    // [J75] T43:仅 "trajectory" 是合法非默认档,其余(含未知值)一律回落默认 "distribution"。
    masterChartMode_ = (mode == "trajectory") ? juce::String("trajectory") : juce::String("distribution");
}

ScvbOutputAudioProcessor::ConnSnapshot ScvbOutputAudioProcessor::connSnapshot()
{
    const auto now = scvb::steadyNowMs(); // 取锁前采样,与 timerCallback 同一时钟口径
    const juce::ScopedLock lock(lifecycleMutex_);
    ConnSnapshot out;
    for (int t = 0; t < 15; ++t)
    {
        out.channels[static_cast<std::size_t>(t)] = session_.channelConn(static_cast<scvb::u32>(t + 1), now);
    }
    out.generation = session_.registryGeneration();
    out.readOnly = session_.state() == scvb::output::OutputClaimState::kObserver;
    return out;
}

void ScvbOutputAudioProcessor::bridgeSetUiLanguage(const juce::String& lang)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    uiLanguage_ = lang; // 已由桥层 normalize({zh,en,fr});getStateInformation 持久化
}

void ScvbOutputAudioProcessor::bridgeSetUiScalePercent(int percent)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    uiScale_ = juce::jlimit(33, 300, percent); // 0.33..3.0 × 100(params-v0 §二 uiScale)
}

juce::AudioProcessorEditor* ScvbOutputAudioProcessor::createEditor()
{
    return new scvb::output::OutputEditor(*this);
}

void ScvbOutputAudioProcessor::rebuildAllCurves()
{
    const double sr = sampleRate_.load(std::memory_order_relaxed); // 原子读(PR#55 第9轮)
    if (sr <= 0.0)
        return; // 未 prepare → 不构建不发布(防 NaN/inf CurveSegment,PR#55 第7轮缺陷2)

    for (int v = 1; v <= scvb::state::kNumVersions; ++v)
    {
        for (int t = 0; t < scvb::state::kNumTracks; ++t)
        {
            const auto& src =
                crvsData_.versions[static_cast<std::size_t>(v - 1)].tracks[static_cast<std::size_t>(t)].segments;
            if (src.empty())
            {
                authority_.setCurve(v, t, nullptr);
                continue;
            }

            std::vector<scvb::CurveSegment> curveSegments;
            curveSegments.reserve(src.size());
            for (const auto& s : src)
            {
                scvb::CurveSegment cs;
                cs.startSec = static_cast<double>(s.t0) / sr;
                cs.endSec = static_cast<double>(s.t1) / sr;
                cs.pan = static_cast<double>(s.pan);
                cs.volDb = static_cast<double>(s.volDb);
                curveSegments.push_back(cs);
            }

            scvb::TransitionConfig cfg;
            cfg.transitionRampSec = static_cast<double>(runtime_.transitionRampMs) / 1000.0;
            scvb::CurveEvaluator ev;
            ev.build(curveSegments, cfg);
            authority_.setCurve(v, t, &ev); // 不可变契约:setCurve 深拷贝
        }
    }
    // 打印器重取活动版本曲线(编辑后打印头立即反映新曲线真身,ADR-005)。
    printer_.setCurves(authority_.activeCurves());
}

std::uint8_t ScvbOutputAudioProcessor::probeGroupsOnline()
{
    std::uint8_t bitmap = 0;

    // 本组:直接读已映射的本组 OutputSlot 判定(state==active → 本组在线;observer → 同组已有主 Output)。
    const auto ownState = session_.state();
    if (ownState == scvb::output::OutputClaimState::kActive || ownState == scvb::output::OutputClaimState::kObserver)
        bitmap |= static_cast<std::uint8_t>(1u << (groupId_ - 1));

    // 异组:只读探测(契约 §2.4;失败=0 位,不重试不报错)。尺寸校验用 VirtualQuery(PR#55 缺陷1)。
    const scvb::u64 now = scvb::steadyNowMs();
    for (int g = 1; g <= scvb::kMaxGroups; ++g)
    {
        if (g == groupId_)
            continue; // 本组已判定
        if (scvb::probeRegistryGroupOnline(static_cast<scvb::u32>(g), now))
            bitmap |= static_cast<std::uint8_t>(1u << (g - 1));
    }
    return bitmap;
}

scvb::state::CrvsData ScvbOutputAudioProcessor::crvsSnapshot()
{
    const juce::ScopedLock lock(lifecycleMutex_);
    return crvsData_;
}

scvb::engine::PlayheadPod ScvbOutputAudioProcessor::playheadSnapshot() const
{
    scvb::engine::PlayheadPod pod{};
    // read 失败(写者正在写/读期间更新)时 pod 未被子写,保持零值 —— 是「撕裂混合值」,非「沿用上帧」;
    // 下一 25Hz tick 重读自愈(PR#55 建议④注释修正)。
    playheadShot_.read(pod);
    return pod;
}

scvb::engine::SetNameResult ScvbOutputAudioProcessor::setVersionName(int version, const juce::String& name,
                                                                     juce::String& effectiveOut)
{
    const juce::ScopedLock lock(lifecycleMutex_);

    std::string eff;
    const scvb::engine::SetNameResult result = scvb::engine::normalizeVersionName(version, name.toStdString(), eff);
    effectiveOut = juce::String::fromUTF8(eff.c_str());
    if (result == scvb::engine::SetNameResult::InvalidIndex)
        return result;
    if (crvsData_.versions[static_cast<std::size_t>(version - 1)].meta.name == eff)
        return result; // 未变,不产生空撤销事务

    scvb::output::commitCrvsTransaction(
        authority_.undoManager(), crvsData_, "Rename V" + juce::String(version),
        [&] { crvsData_.versions[static_cast<std::size_t>(version - 1)].meta.name = eff; },
        [] {}); // 改名不影响曲线,不 rebuild
    return result;
}

scvb::engine::CopyVersionResult ScvbOutputAudioProcessor::copyVersion(int src, int dst)
{
    const juce::ScopedLock lock(lifecycleMutex_);

    if (src < 1 || src > 2 || dst < 1 || dst > 2 || src == dst)
        return scvb::engine::CopyVersionResult::InvalidIndex;

    scvb::output::commitCrvsTransaction(
        authority_.undoManager(), crvsData_, "Copy V" + juce::String(src) + " -> V" + juce::String(dst),
        [&] {
            auto& d = crvsData_.versions[static_cast<std::size_t>(dst - 1)];
            const auto& s = crvsData_.versions[static_cast<std::size_t>(src - 1)];
            d.tracks = s.tracks; // 深拷贝段表(含 origin/locked)
            d.panCurve = s.panCurve;
            d.meta.copiedFrom = src; // name 不复制(保留目标名,J05)
            d.meta.copiedAtMs = static_cast<std::int64_t>(juce::Time::getCurrentTime().toMilliseconds());
        },
        [this] { rebuildAllCurves(); });
    return scvb::engine::CopyVersionResult::Ok;
}

scvb::state::SegmentEditResult ScvbOutputAudioProcessor::editSegment(int track,
                                                                     const scvb::state::SegmentEditArgs& args)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    // 结果门控:仅 Ok 才压入 undo 事务并 rebuild(PR#55 缺陷3)。
    return scvb::output::editSegmentTransactional(authority_.undoManager(), crvsData_, versionActive_, track, args,
                                                  [this] { rebuildAllCurves(); });
}

bool ScvbOutputAudioProcessor::setTrackManual(int ch, bool isPan, float value, int& replacedSegments,
                                              int& replacedLocked)
{
    const juce::ScopedLock lock(lifecycleMutex_);

    if (ch < 1 || ch > 15)
        return false;

    auto& track =
        crvsData_.versions[static_cast<std::size_t>(versionActive_ - 1)].tracks[static_cast<std::size_t>(ch - 1)];
    replacedSegments = static_cast<int>(track.segments.size());
    replacedLocked = 0;
    for (const auto& s : track.segments)
        if (scvb::state::segmentLocked(s.flags))
            ++replacedLocked; // 如实统计锁定段(PR#55 建议⑤)

    scvb::state::Segment seg;
    seg.t0 = 0;
    seg.t1 = static_cast<std::int64_t>(1) << 40; // 覆盖全时间线近似(真末端由宿主时间线提供)
    if (isPan)
    {
        seg.pan = juce::jlimit(-100.0f, 100.0f, value);
        seg.volDb = 0.0f;
    }
    else
    {
        seg.pan = 0.0f;
        seg.volDb = juce::jlimit(-24.0f, 12.0f, value);
    }
    seg.flags = scvb::state::makeSegmentFlags(scvb::state::SegmentOrigin::UserEdited, false);

    scvb::output::commitCrvsTransaction(
        authority_.undoManager(), crvsData_, "Track manual ch" + juce::String(ch),
        [&] { track.segments.assign(1, seg); }, [this] { rebuildAllCurves(); });
    return true;
}

void ScvbOutputAudioProcessor::setPanCurve(int version, const std::vector<scvb::PanCurvePoint>& points)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    if (version < 1 || version > scvb::state::kNumVersions)
        return; // 越界拒绝(public API 防 UB,PR#55 建议②)
    scvb::output::commitCrvsTransaction(
        authority_.undoManager(), crvsData_, "Set pan curve",
        [&] { crvsData_.versions[static_cast<std::size_t>(version - 1)].panCurve = points; },
        [] {}); // pan_curve 不参与 CurveEvaluator,不 rebuild
}

bool ScvbOutputAudioProcessor::setTransitionRamp(float ms)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    const float clamped = juce::jlimit(20.0f, 300.0f, ms);
    if (runtime_.transitionRampMs == clamped)
        return false; // 未变,不重建

    runtime_.transitionRampMs = clamped;
    rebuildAllCurves(); // 新 ramp 立即烘焙进 CurveEvaluator 并重新发布(PR#55 第5轮缺陷2)
    return true;
}

bool ScvbOutputAudioProcessor::undo()
{
    const juce::ScopedLock lock(lifecycleMutex_);
    return authority_.undoManager().undo();
}

bool ScvbOutputAudioProcessor::redo()
{
    const juce::ScopedLock lock(lifecycleMutex_);
    return authority_.undoManager().redo();
}

// juce_add_plugin 的 VST3/AU wrapper 从这里实例化插件。
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ScvbOutputAudioProcessor();
}
