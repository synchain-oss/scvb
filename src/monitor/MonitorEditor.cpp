// SPDX-License-Identifier: GPL-3.0-or-later
#include "MonitorEditor.h"

#include "MonitorBridgeApi.h"
#include "MonitorProcessor.h"

#include "BridgeBase.h"

#include <utility>

namespace scvb::monitor
{
namespace
{
using WBC = juce::WebBrowserComponent;

// scvb.viz 推送间隔:与 viz 发布器的 4Hz 同频 —— 更快也没有新数据。
constexpr juce::uint64 kVizEmitIntervalMs = 250;

const char* vizStateName(ScvbMonitorAudioProcessor::VizState s)
{
    switch (s)
    {
    case ScvbMonitorAudioProcessor::VizState::kOnline:
        return "online";
    case ScvbMonitorAudioProcessor::VizState::kAbiMismatch:
        return "abiMismatch";
    case ScvbMonitorAudioProcessor::VizState::kOffline:
    default:
        return "offline";
    }
}
} // namespace

MonitorEditor::MonitorEditor(ScvbMonitorAudioProcessor& processor)
    : WebViewHost(processor,
                  [&processor, this]() {
                      scvb::webview::WebViewHost::Config c;
                      // role 取 "monitor":designBoxWindowSize / parseUiScaleArg 对非 "input" 一律
                      // 回落 Output 档位(1180×780,7 档)—— Monitor 自己的设计盒尺寸依内容定,归 T46
                      // (05 J75 节 C),本卡先借 Output 盒,机制同款。
                      c.role = "monitor";
                      c.userDataFolderName = "SCVBMonitorWV2"; // 与 Input/Output 各自独立(UDF per-plugin)
                      c.version = JucePlugin_VersionString;
                      c.lang = processor.uiLanguage();
                      c.uiScale = static_cast<float>(processor.uiScalePercent()) / 100.0f;
                      c.channelLimit = 15;
                      c.resourceSource = {}; // web/monitor 资源嵌入归 T46(与 Input/Output 现状同口径)
                      c.augmentOptions = [this](juce::WebBrowserComponent::Options& options) {
                          registerNativeFunctions(options);
                      };
                      return c;
                  }()),
      processor_(processor), pluginVersion_(JucePlugin_VersionString)
{
}

void MonitorEditor::registerNativeFunctions(juce::WebBrowserComponent::Options& options)
{
    // Monitor 只注册**一个**专属函数:组选择。没有任何写操作 —— 这是只读监视器的全部桥面写入口。
    options = options.withNativeFunction(juce::Identifier(bridge::kFnSetGroupId),
                                         [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                             handleSetGroupId(a, std::move(c));
                                         });
}

juce::var MonitorEditor::buildSnapshot()
{
    // 与 Input/Output 同款:每次 requestInitialState(含 WebView 重载)都清 diff 基线,
    // 下一个 emitTick 重发各事件首帧。
    lastStateJson_.clear();
    lastGroupsJson_.clear();
    lastVizJson_.clear();
    lastGroupsMs_ = 0;
    lastVizMs_ = 0;

    scvb::bridge::UiSeed seed;
    seed.role = "monitor";
    seed.version = pluginVersion_;
    seed.lang = lang();
    seed.uiScale = uiScale();
    seed.channelLimit = 15;
    auto snapshot = scvb::bridge::buildUiSnapshot(seed);
    if (auto* obj = snapshot.getDynamicObject())
    {
        obj->setProperty("groupId", processor_.groupId());
        obj->setProperty("viz", vizStateName(processor_.vizState()));
        obj->setProperty("groupsOnline", static_cast<int>(processor_.groupsOnline()));
    }
    return snapshot;
}

juce::var MonitorEditor::buildStatePayload() const
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("groupId", processor_.groupId());
    obj->setProperty("uiScale", uiScale());
    obj->setProperty("language", lang());
    obj->setProperty("viz", vizStateName(processor_.vizState()));
    obj->setProperty("fresh", processor_.vizFresh());
    return juce::var(obj);
}

juce::var MonitorEditor::buildVizPayload() const
{
    const auto& v = processor_.vizSnapshot();
    const bool online = processor_.vizState() == ScvbMonitorAudioProcessor::VizState::kOnline;

    auto* obj = new juce::DynamicObject();
    obj->setProperty("online", online && processor_.vizFresh());
    obj->setProperty("sampleRate", static_cast<int>(v.sampleRate));
    obj->setProperty("windowStart", static_cast<juce::int64>(v.windowStartSamples));
    obj->setProperty("windowSpan", static_cast<juce::int64>(v.windowSpanSamples));
    obj->setProperty("playhead", static_cast<juce::int64>(v.playheadSamples));
    obj->setProperty("playing", (v.playheadFlags & scvb::kVizPlaying) != 0u);
    obj->setProperty("looping", (v.playheadFlags & scvb::kVizLooping) != 0u);
    if ((v.playheadFlags & scvb::kVizLoopValid) != 0u)
    {
        obj->setProperty("loopStart", static_cast<juce::int64>(v.loopStartSamples));
        obj->setProperty("loopEnd", static_cast<juce::int64>(v.loopEndSamples));
    }
    obj->setProperty("versionActive", static_cast<int>(v.versionActive));
    obj->setProperty("laneRevision", static_cast<int>(v.laneRevision));
    obj->setProperty("columns", static_cast<int>(scvb::kVizColumns));

    // T45 壳层只送**每轨摘要**(轨号 / 色位 / 在线 / 有分段 / 立体声 / 播放头处的 pan)。
    // 15×1024 车道的全量传输口径归 T46 —— 逐帧丢 15360 个数走 juce::var 会在 4Hz 上白烧
    // 消息线程;T46 按需求定(建议 base64 定长块 + laneRevision 变化才重传,见 PR 说明)。
    const bool haveWindow = v.windowSpanSamples > 0;
    const double colSamples =
        haveWindow ? static_cast<double>(v.windowSpanSamples) / static_cast<double>(scvb::kVizColumns) : 0.0;
    scvb::u32 headCol = 0;
    if (haveWindow && v.playheadSamples > 0)
    {
        const double c =
            (static_cast<double>(v.playheadSamples) - static_cast<double>(v.windowStartSamples)) / colSamples;
        headCol = c <= 0.0 ? 0u
                           : (c >= static_cast<double>(scvb::kVizColumns - 1) ? scvb::kVizColumns - 1
                                                                              : static_cast<scvb::u32>(c));
    }

    juce::Array<juce::var> tracks;
    for (scvb::u32 t = 0; t < scvb::kMaxChannels; ++t)
    {
        auto* tr = new juce::DynamicObject();
        tr->setProperty("ch", static_cast<int>(t + 1));
        tr->setProperty("color", static_cast<int>(v.trackColor[t]));
        tr->setProperty("enabled", (v.onlineMask & (1u << t)) != 0u);
        tr->setProperty("hasSegments", (v.coveredMask & (1u << t)) != 0u);
        tr->setProperty("stereo", (v.stereoMask & (1u << t)) != 0u);
        const auto raw = v.pan[t][headCol];
        // 哨兵/未覆盖 → 不带 pan 字段(§4.1 字段纪律:拿不到就不写,不发明 0)。
        if (!scvb::vizPanIsNone(raw) && v.covered(t, headCol))
        {
            tr->setProperty("pan", scvb::vizUnpackPan(raw));
        }
        tracks.add(juce::var(tr));
    }
    obj->setProperty("tracks", tracks);
    return juce::var(obj);
}

void MonitorEditor::emitTick()
{
    const auto now = scvb::steadyNowMs();

    // scvb.state:变化即发(首帧必发)。
    emitIfChanged(bridge::kEvState, buildStatePayload(), lastStateJson_);

    // scvb.groups:1Hz(J70 只读探测位图)。
    if (now - lastGroupsMs_ >= 1000)
    {
        lastGroupsMs_ = now;
        auto* obj = new juce::DynamicObject();
        obj->setProperty("online", static_cast<int>(processor_.groupsOnline()));
        emitIfChanged(bridge::kEvGroups, juce::var(obj), lastGroupsJson_);
    }

    // scvb.viz:4Hz(与发布器同频;更快没有新数据)。
    if (now - lastVizMs_ >= kVizEmitIntervalMs)
    {
        lastVizMs_ = now;
        emitIfChanged(bridge::kEvViz, buildVizPayload(), lastVizJson_);
    }
}

bool MonitorEditor::emitIfChanged(const char* name, const juce::var& payload, juce::String& lastJson)
{
    const juce::String json = juce::JSON::toString(payload);
    // 隐藏(关闭/最小化)时事件会被丢弃 —— 此时**不推进缓存**,否则隐藏期的变化被吞,
    // 恢复可见后不再重发(与 Input 的 advanceEmitCache 同口径,PR#54 R4)。
    if (!webView().isVisible())
    {
        return false;
    }
    if (json == lastJson)
    {
        return false;
    }
    lastJson = json;
    webView().emitEventIfBrowserIsVisible(juce::Identifier(name), payload);
    return true;
}

void MonitorEditor::handleSetGroupId(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
{
    if (args.size() < 1 || !(args[0].isInt() || args[0].isDouble()))
    {
        auto* err = new juce::DynamicObject();
        err->setProperty("ok", false);
        err->setProperty("reason", "badArg");
        complete(juce::var(err));
        return;
    }
    const int requested = static_cast<int>(args[0]);
    if (requested < 1 || requested > 8)
    {
        auto* err = new juce::DynamicObject();
        err->setProperty("ok", false);
        err->setProperty("reason", "badArg");
        complete(juce::var(err));
        return;
    }
    processor_.setGroupId(requested);
    auto* ok = new juce::DynamicObject();
    ok->setProperty("ok", true);
    ok->setProperty("groupId", processor_.groupId()); // 实际生效值经回执 + scvb.state 双回推
    complete(juce::var(ok));
}

void MonitorEditor::handleSetLang(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
{
    WebViewHost::handleSetLang(args, std::move(complete)); // 归一化 {zh,en,fr} + 回执
    processor_.setUiLanguage(lang());
}

void MonitorEditor::persistUiScaleAsDefault()
{
    processor_.setUiScalePercent(juce::roundToInt(uiScale() * 100.0f));
}

} // namespace scvb::monitor
