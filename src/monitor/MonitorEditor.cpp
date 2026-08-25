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
    // Monitor 只注册**一个**专属函数:组选择。名字不是 `setObservedGroup` —— 契约 §1.4 的那个是
    // Output 的改组(断连本组全部、要弹确认条),与「换一个组来看」是两件事(T46 提出,采纳)。
    options = options.withNativeFunction(juce::Identifier(bridge::kFnSetObservedGroup),
                                         [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                             handleSetObservedGroup(a, std::move(c));
                                         });
}

juce::var MonitorEditor::buildSnapshot()
{
    // 与 Input/Output 同款:每次 requestInitialState(含 WebView 重载)都清 diff 基线,
    // 下一个 emitTick 重发各事件首帧。
    lastStateJson_.clear();
    lastGroupsJson_.clear();
    lastVizJson_.clear();
    lastPlayheadJson_.clear();
    lastGroupsMs_ = 0;
    lastVizMs_ = 0;

    // 形状按消费侧(T46)要的来:{abi, version, groupId, groups_online, ui:{scale,language}, viz:<首帧>}。
    // 首帧带上 viz —— 否则页面要空等一整个低频发布周期(250ms)才出图。
    auto* ui = new juce::DynamicObject();
    ui->setProperty("scale", uiScale());
    ui->setProperty("language", lang());

    auto* obj = new juce::DynamicObject();
    obj->setProperty("abi", static_cast<int>(scvb::kScvbAbi));
    obj->setProperty("version", pluginVersion_);
    obj->setProperty("groupId", processor_.groupId());
    obj->setProperty("groups_online", static_cast<int>(processor_.groupsOnline()));
    obj->setProperty("ui", juce::var(ui));
    obj->setProperty("viz", buildVizPayload());
    return juce::var(obj);
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
        tr->setProperty("label", juce::String::fromUTF8(v.label[t].c_str(), static_cast<int>(v.label[t].size())));
        // 每轨当前值(分布图三件套)。哨兵 → **不带该字段**(§4.1 字段纪律:拿不到就不写,
        // 不发明 0 —— 0 是合法 pan / 合法 dB,拿它当「没有」会让图画出一条居中的假线)。
        if (!scvb::vizPanIsNone(v.panNow[t]))
        {
            tr->setProperty("panNow", scvb::vizUnpackPan(v.panNow[t]));
        }
        if (!scvb::vizPanIsNone(v.volDb[t]))
        {
            tr->setProperty("volDb", scvb::vizUnpackFixed(v.volDb[t]));
        }
        if (!scvb::vizPanIsNone(v.widthPct[t]))
        {
            tr->setProperty("widthPct", scvb::vizUnpackFixed(v.widthPct[t]));
        }
        const auto raw = v.pan[t][headCol];
        // 播放头所在列的车道采样(与 panNow 的区别:这是列中心点采样,列宽 = span/1024)。
        if (!scvb::vizPanIsNone(raw) && v.covered(t, headCol))
        {
            tr->setProperty("pan", scvb::vizUnpackPan(raw));
        }
        tracks.add(juce::var(tr));
    }
    obj->setProperty("tracks", tracks);
    return juce::var(obj);
}

juce::var MonitorEditor::buildPlayheadPayload() const
{
    // 载荷形状**逐字复用 Output 侧 scvb.playhead**(T46 的 onPlayhead 消费代码因此一行没改)。
    // Monitor 没有 range 概念,inRange 恒 true。
    const auto pod = processor_.playheadSnapshot();
    const bool playing = (pod.flags & scvb::engine::kPlayheadIsPlaying) != 0;
    const double sr = pod.sampleRate > 0.0 ? pod.sampleRate : 48000.0;
    const double timeS = pod.timeSamples >= 0 ? static_cast<double>(pod.timeSamples) / sr : 0.0;

    auto* obj = new juce::DynamicObject();
    obj->setProperty("timeS", timeS);
    obj->setProperty("isPlaying", playing);
    obj->setProperty("inRange", true);
    // loopStartS/loopEndS:仅 cycle + tempo 都有效时出现(§4.1 字段纪律:拿不到就不写)。
    if ((pod.flags & scvb::engine::kPlayheadCycleValid) != 0 && (pod.flags & scvb::engine::kPlayheadTempoValid) != 0 &&
        pod.bpm > 0.0)
    {
        obj->setProperty("loopStartS", pod.loopStartPpq * 60.0 / pod.bpm);
        obj->setProperty("loopEndS", pod.loopEndPpq * 60.0 / pod.bpm);
    }
    return juce::var(obj);
}

void MonitorEditor::emitTick()
{
    const auto now = scvb::steadyNowMs();

    // scvb.playhead:每 tick(WebViewHost 定时器 25Hz —— 宿主给到的上限就是这个)。
    // 竖线跟手靠它,不靠 viz 段里那份 4Hz 的冗余副本。
    emitIfChanged(bridge::kEvPlayhead, buildPlayheadPayload(), lastPlayheadJson_);

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

void MonitorEditor::handleSetObservedGroup(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
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
    processor_.setObservedGroup(requested);
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
