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
constexpr std::uint64_t kVizPollIntervalMs = 250; // 4Hz:与发布器同频,不多不少
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

void ScvbMonitorAudioProcessor::prepareToPlay(double /*sampleRate*/, int /*samplesPerBlock*/)
{
    // 只读监视器:不分配音频缓冲、不建段、不 claim。attach 交给 [M] 定时器(段可能还没建起来)。
    // 这里只把段指向当前组(setStateInformation 可能已经改过 groupId_)。
    vizPlane_.setGroupReadOnly(static_cast<scvb::u32>(groupId_));
    vizState_ = VizState::kOffline;
    vizFresh_ = false;
    lastAttachTryMs_ = 0;
    startTimerHz(4);
}

void ScvbMonitorAudioProcessor::releaseResources()
{
    stopTimer();
    vizPlane_.release();
    vizState_ = VizState::kOffline;
    vizFresh_ = false;
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
    setGroupId(static_cast<int>(s.groupId));
    setUiScalePercent(static_cast<int>(s.uiScale));
    uiLanguage_ = juce::String::fromUTF8(s.uiLanguage.c_str(), static_cast<int>(s.uiLanguage.size()));
}

// ---------------------------------------------------------------------------
// [M] 桥入口
// ---------------------------------------------------------------------------

bool ScvbMonitorAudioProcessor::setGroupId(int groupId)
{
    groupId = juce::jlimit(1, kGroupIdMax, groupId);
    if (groupId == groupId_)
    {
        return false;
    }
    groupId_ = groupId;
    // 只读换段:释放旧组句柄 + 指向新组,**不在这里 attach** —— 新组的 Output 可能还没上线,
    // attach 由 [M] 4Hz 重试。绝不用 changeGroup():那是写方路径,会把「新组无写方」变成建段。
    vizPlane_.setGroupReadOnly(static_cast<scvb::u32>(groupId));
    vizState_ = VizState::kOffline;
    vizFresh_ = false;
    lastVizPublishMs_ = 0;
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
    uiLanguage_ = lang; // 已由桥层 normalize({zh,en,fr})
}

// ---------------------------------------------------------------------------
// [M] 4Hz 定时器:viz attach/读 + 1Hz 跨组探测
// ---------------------------------------------------------------------------

void ScvbMonitorAudioProcessor::timerCallback()
{
    tickMessageThread(scvb::steadyNowMs());
}

void ScvbMonitorAudioProcessor::tickMessageThread(std::uint64_t now)
{
    refreshViz(now);

    if (now - lastGroupsProbeMs_ >= kGroupsProbeIntervalMs)
    {
        lastGroupsProbeMs_ = now;
        // [J70] 只读探测各组 registry(FILE_MAP_READ);本组也探测 —— Monitor 从不打开本组 registry。
        groupsOnline_ = scvb::probeGroupsOnline(backend_, static_cast<scvb::u32>(groupId_), now,
                                                /*includeOwnGroup=*/true);
    }

    vizPlane_.reapPendingReleases(now);
}

void ScvbMonitorAudioProcessor::refreshViz(std::uint64_t nowMs)
{
    if (!vizPlane_.isOpen())
    {
        if (nowMs - lastAttachTryMs_ < kVizPollIntervalMs)
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
        if (viz_->publishMs != lastVizPublishMs_)
        {
            lastVizPublishMs_ = viz_->publishMs;
            lastVizChangeMs_ = nowMs;
        }
    }
    vizFresh_ = lastVizChangeMs_ != 0 && (nowMs - lastVizChangeMs_) < kVizStaleMs;
}

// ---------------------------------------------------------------------------

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ScvbMonitorAudioProcessor();
}
