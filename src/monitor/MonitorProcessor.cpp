// SPDX-License-Identifier: GPL-3.0-or-later
#include "MonitorProcessor.h"

// SCVB_MONITOR_HEADLESS:离线单测(scvb_monitor_tests)专用 —— 不实例化 WebView2 编辑器。
// 与 tests/CMakeLists.txt 把 WebViewHost.cpp / PlatformWebViewRuntime.cpp 排除在单测之外
// 同源(真 WebView2 由 gate 8 的真机 GUI pluginval 验)。生产构建从不定义本宏。
#if !SCVB_MONITOR_HEADLESS
#include "MonitorEditor.h"
#endif

#include "ipc/GroupProbe.h"
#include "state/InputStateCodec.h"

namespace
{
constexpr int kGroupIdMax = 8; // [J66] 1..8(A-H)
constexpr int kUiScaleMinPercent = 50;
constexpr int kUiScaleMaxPercent = 200;
constexpr std::uint64_t kGroupsProbeIntervalMs = 1000; // 1Hz 跨组探测(J70)
// 段**不存在**时的 attach 重试节流。这一条与读帧无关(读帧由定时器每拍做),
// 它挡的是「Output 还没起来」时每拍一次 open+unmap —— 4Hz 重试足够,自愈延迟无人感知。
constexpr std::uint64_t kVizAttachRetryMs = 250;
// [M] 轮询频率(SL-192)。**发布器仍是 4Hz(冻结契约口径,一个字节没动)**,改的是读方
// 的采样率。曾经这里也是 4Hz,注释写着「与发布器同频,不多不少」—— 那句话漏了一件事:
// 两个 4Hz 时钟**互不同步**。同频异相的采样会周期性地把某一帧整个跳过(读到的还是上一帧),
// 于是 UI 实得的更新率并不是 4Hz,而是 4Hz 上下漂、偶尔隔 500ms 才动一次;叠上编辑器侧
// 那一层同款的 250ms 闸(见 MonitorEditor.cpp),用户看到的就是「一秒钟刷新一两次」。
//
// 20Hz(50ms)= 发布周期的 1/5:任何一帧最迟 50ms 内被读到,再不会整帧跳过,检测延迟
// 从 ≤250ms 降到 ≤50ms。代价是 `VizPlane::read()` 从 4Hz 提到 20Hz —— 那是一次 ~16k 次
// relaxed 原子读的定长循环(15 轨 × 1024 车道),几十微秒量级,跑在 [M] 上;
// 相对「图慢半拍」这个真实体感,这点开销买得值。
constexpr int kVizPollHz = 20;
} // namespace

ScvbMonitorAudioProcessor::ScvbMonitorAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      vizPlane_(backend_, 1u), viz_(std::make_unique<scvb::VizSnapshot>())
{
    setLatencySamples(0); // 纯直通:不报任何 latency
    // 参数面:一个都不建。Output 的 123 冻结参数与本插件无关(J75:Monitor 0 自动化参数)。
    jassert(getParameters().isEmpty());
}

ScvbMonitorAudioProcessor::~ScvbMonitorAudioProcessor()
{
    stopTimer();
}

void ScvbMonitorAudioProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    // 只读监视器:不分配音频缓冲、不建段、不 claim。attach 交给 [M] 定时器(段可能还没建起来)。
    // 这里只把段指向当前组(setStateInformation 可能已经改过 groupId_)。
    vizPlane_.setGroupReadOnly(static_cast<scvb::u32>(groupId_));
    vizState_ = VizState::kOffline;
    vizFresh_ = false;
    sawVizFrame_ = false;
    lastVizChangeMs_ = 0;
    lastAttachTryMs_ = 0;
    startTimerHz(kVizPollHz);
}

void ScvbMonitorAudioProcessor::releaseResources()
{
    stopTimer(); // 先停表:此后不再有 [M] 读,锁内只剩解映射
    const juce::ScopedLock lock(lifecycleMutex_);
    vizPlane_.release();
    vizState_ = VizState::kOffline;
    vizFresh_ = false;
    sawVizFrame_ = false;
}

bool ScvbMonitorAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // 直通插件:mono 或 stereo,进出必须一致(与 Input 同款宽松口径)。
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
    {
        return false;
    }
    return out == layouts.getMainInputChannelSet();
}

void ScvbMonitorAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    // 音频直通铁律:**一个样本都不动**,输出与输入逐样本按位相等(T42 旁路口径)。
    //
    // 这里刻意什么都不做 —— 不清 buffer、不写增益、不 copy。JUCE 的 in-place buffer 已经就是
    // 宿主的输入;任何写操作(哪怕 ×1.0f)都可能因浮点路径改变位模式,破坏 nulltest 的按位相等。
    //
    // 也绝不在此触碰任何共享段:viz 段的读取一律在 [M] 定时器([J75] RT 零写入/零读取铁律)。
    // 声道数不匹配的情形已被 isBusesLayoutSupported 挡在外面,无需补清尾部声道。
    juce::ignoreUnused(buffer);

    // 唯一的例外动作:把宿主 transport 快照发布给 [M](进程内 SPSC seqlock,**不是共享内存**)。
    // 零分配、零锁、不碰 buffer —— 逐样本按位相等不受影响(nulltest 用例覆盖)。
    // 为什么不用 viz 段里的 playhead:那份是 4Hz 的冗余副本,竖线会一顿一顿;
    // Monitor 与 Output 同处一个宿主,看到的是同一条 transport,直接读最准也最跟手。
    publishPlayhead();
}

void ScvbMonitorAudioProcessor::publishPlayhead()
{
    scvb::engine::PlayheadPod pod;
    pod.sampleRate = sampleRate_;
    if (juce::AudioPlayHead* ph = getPlayHead())
    {
        if (const auto p = ph->getPosition(); p.hasValue())
        {
            pod.timeSamples = p->getTimeInSamples().hasValue() ? *p->getTimeInSamples() : -1;
            if (p->getIsPlaying())
            {
                pod.flags |= scvb::engine::kPlayheadIsPlaying;
            }
            if (p->getIsLooping())
            {
                pod.flags |= scvb::engine::kPlayheadIsLooping;
            }
            if (const auto bpm = p->getBpm(); bpm.hasValue())
            {
                pod.bpm = *bpm;
                pod.flags |= scvb::engine::kPlayheadTempoValid;
            }
            if (const auto loop = p->getLoopPoints(); loop.hasValue())
            {
                pod.loopStartPpq = loop->ppqStart;
                pod.loopEndPpq = loop->ppqEnd;
                pod.flags |= scvb::engine::kPlayheadCycleValid;
            }
        }
    }
    playheadShot_.publish(pod);
}

scvb::engine::PlayheadPod ScvbMonitorAudioProcessor::playheadSnapshot() const
{
    scvb::engine::PlayheadPod pod;
    playheadShot_.read(pod); // 撕裂读返回全零 pod;调用方按 timeSamples < 0 过滤
    return pod;
}

void ScvbMonitorAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // bypass 与非 bypass 对本插件完全等价(两条路径都是纯直通)。
    processBlock(buffer, midiMessages);
}

juce::AudioProcessorEditor* ScvbMonitorAudioProcessor::createEditor()
{
#if SCVB_MONITOR_HEADLESS
    return nullptr;
#else
    return new scvb::monitor::MonitorEditor(*this);
#endif
}

void ScvbMonitorAudioProcessor::setCurrentProgram(int /*index*/) {}

const juce::String ScvbMonitorAudioProcessor::getProgramName(int /*index*/)
{
    return {};
}

void ScvbMonitorAudioProcessor::changeProgramName(int /*index*/, const juce::String& /*newName*/) {}

// ---------------------------------------------------------------------------
// state(非自动化;复用 Input 的 CFGS payload 布局,state schema 零新增)
// ---------------------------------------------------------------------------

void ScvbMonitorAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    // 复用 scvb::state::InputState 的 payload 布局(channel_id / group_id / ui{scale,language})——
    // Monitor 不认领任何 channel,channelId 恒 0 且不参与语义。这样既不新增 state schema,
    // 也不触碰 docs/STATE_SCHEMA.md 本体(见 docs/contract-changes/20260825-monitor-target.md)。
    scvb::state::InputState s;
    s.channelId = 0;
    s.groupId = static_cast<scvb::u32>(groupId_);
    s.uiScale = static_cast<scvb::u32>(uiScalePercent_);
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

void ScvbMonitorAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
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
    // 宿主可从任意线程恢复 state;下面写 groupId_/uiScale/uiLanguage_,而 [M] 同时在读它们。
    const juce::ScopedLock lock(lifecycleMutex_);
    // 高版本 abi:拒载(CLAUDE.md §7.3)。Monitor 是只读观察器,state 只影响「看哪一组」,
    // 无需 preservedOriginal 回写 —— 丢的只是一个视图偏好,不是用户数据。
    if (scvb::state::decideInputStateAbi(chunks.abi) == scvb::state::InputStateAbiDecision::RejectNewer)
    {
        DBG("SCVB Monitor: state abi " << chunks.abi << " > current " << scvb::state::kCurrentAbi
                                       << "; refusing load (upgrade required)");
        return;
    }

    const scvb::state::Chunk* cfg = chunks.find(scvb::state::kFourccCfgs);
    if (cfg == nullptr)
    {
        return;
    }
    scvb::state::InputState s;
    if (!scvb::state::decodeInputState(cfg->payload.data(), cfg->payload.size(), s))
    {
        return; // 范围校验失败 → 拒载
    }
    // setObservedGroup 自己取锁(juce::CriticalSection 可重入,与 Output 同款)。
    setObservedGroup(static_cast<int>(s.groupId));
    setUiScalePercent(static_cast<int>(s.uiScale));
    // 归一化到 {zh,en,fr}(§1.30):codec 只限长度不限取值 —— 工程里塞了别的串会原样进
    // WebView 首帧 seed。与 setUiLanguage() 同一条路,免得两个入口口径不同。
    setUiLanguage(juce::String::fromUTF8(s.uiLanguage.c_str(), static_cast<int>(s.uiLanguage.size())));
}

// ---------------------------------------------------------------------------
// [M] 桥入口
// ---------------------------------------------------------------------------

bool ScvbMonitorAudioProcessor::setObservedGroup(int groupId)
{
    groupId = juce::jlimit(1, kGroupIdMax, groupId);
    const juce::ScopedLock lock(lifecycleMutex_);
    if (groupId == groupId_)
    {
        return false;
    }
    groupId_ = groupId;
    // 只读换段:释放旧组句柄 + 指向新组,**不在这里 attach** —— 新组的 Output 可能还没上线,
    // attach 由 [M] 定时器重试(段不存在时 4Hz 节流)。绝不用 changeGroup():那是写方路径,会把「新组无写方」变成建段。
    vizPlane_.setGroupReadOnly(static_cast<scvb::u32>(groupId));
    vizState_ = VizState::kOffline;
    vizFresh_ = false;
    sawVizFrame_ = false;
    lastVizPublishMs_ = 0;
    lastVizChangeMs_ = 0;
    lastAttachTryMs_ = 0;
    *viz_ = scvb::VizSnapshot{}; // 清空上一组的车道,避免换组瞬间画出别人的曲线
    return true;
}

void ScvbMonitorAudioProcessor::setUiScalePercent(int percent)
{
    uiScalePercent_ = juce::jlimit(kUiScaleMinPercent, kUiScaleMaxPercent, percent);
}

void ScvbMonitorAudioProcessor::setUiLanguage(const juce::String& lang)
{
    // 自己归一化,不假设调用方做过(§1.30:未知 code 回退 zh)。桥入口已经 normalize 过一次,
    // 但 setStateInformation 那条路的字节来自工程文件 —— 不可信。
    // 这里不调 scvb::bridge::normalizeLang:那会把 plugin-common 拖进离线单测目标
    // (scvb_monitor_tests 刻意只编 MonitorProcessor.cpp),口径三行写死更省。
    uiLanguage_ = (lang == "en" || lang == "fr") ? lang : juce::String("zh");
}

// ---------------------------------------------------------------------------
// [M] 20Hz 定时器(SL-192;发布器仍 4Hz):viz attach/读 + 1Hz 跨组探测
// ---------------------------------------------------------------------------

void ScvbMonitorAudioProcessor::timerCallback()
{
    tickMessageThread(scvb::steadyNowMs());
}

void ScvbMonitorAudioProcessor::tickMessageThread(std::uint64_t now)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    refreshViz(now);

    if (now - lastGroupsProbeMs_ >= kGroupsProbeIntervalMs)
    {
        lastGroupsProbeMs_ = now;
        // [J70] 只读探测各组 registry(FILE_MAP_READ);本组也探测 —— Monitor 从不打开本组 registry。
        groupsOnline_ = scvb::probeGroupsOnline(backend_, static_cast<scvb::u32>(groupId_), now,
                                                /*includeOwnGroup=*/true);
    }
}

void ScvbMonitorAudioProcessor::refreshViz(std::uint64_t nowMs)
{
    // ① 已 attach 但帧陈旧:写方要么只是停摆、要么进程已经没了。**分辨这两者只有一个办法 ——
    // 松开自己的映射再探一次**。命名段的存活是引用计数的:只要 Monitor 自己还抱着映射,
    // 段就不会消失,于是「写方进程退出」永远看不出来,UI 会一直显示一份僵尸数据。
    // 松手后:段真没了 → attach 失败 → 空态;写方还在(只是没发新帧)→ attach 成功 → 维持在线但陈旧。
    // 注意**不重置** sawVizFrame_/lastVizPublishMs_ —— 重新读到同一个 publish_ms 不算新帧,
    // 否则每次探测都会把自己「刷新」一遍,永远判不出陈旧。
    if (vizPlane_.isOpen() && sawVizFrame_ && !vizFresh_ && nowMs - lastStaleProbeMs_ >= kVizStaleMs)
    {
        lastStaleProbeMs_ = nowMs;
        vizPlane_.release();
        lastAttachTryMs_ = 0; // 同一拍内立刻重试
    }

    if (!vizPlane_.isOpen())
    {
        if (nowMs - lastAttachTryMs_ < kVizAttachRetryMs)
        {
            return;
        }
        lastAttachTryMs_ = nowMs;
        switch (vizPlane_.attachReadOnly())
        {
        case scvb::InitResult::kOk:
            vizState_ = VizState::kOnline;
            break;
        case scvb::InitResult::kAbiMismatch:
            // 拒连(J40):绝不半兼容读。下一拍仍会重试 —— 对端升级后自愈,而 attach 失败的
            // 每一拍只是一次 open+unmap,不映射、不读段内容,代价可忽略。
            vizState_ = VizState::kAbiMismatch;
            vizFresh_ = false;
            return;
        case scvb::InitResult::kFailed:
        default:
            vizState_ = VizState::kOffline; // 段不存在 = Output 未在该组上线:空态,下拍再试
            vizFresh_ = false;
            return;
        }
    }

    // 一致性读;撕裂(连续 8 次)则沿用上帧,不清空、不闪烁。
    if (vizPlane_.read(*viz_))
    {
        // 首帧必算「有新帧」:publish_ms 完全可以合法地是 0(写方用的是自己的时钟基准),
        // 光比 publish_ms 会把首帧当成「没动过」,于是永远判陈旧。
        if (!sawVizFrame_ || viz_->publishMs != lastVizPublishMs_)
        {
            sawVizFrame_ = true;
            lastVizPublishMs_ = viz_->publishMs;
            lastVizChangeMs_ = nowMs;
        }
    }
    vizFresh_ = sawVizFrame_ && (nowMs - lastVizChangeMs_) < kVizStaleMs;
}

// ---------------------------------------------------------------------------

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ScvbMonitorAudioProcessor();
}
