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
    // Monitor 只注册**一个**专属函数:组选择。名字不是 `setGroupId` —— 契约 §1.4 的那个是
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
    sentLanes_ = false; // 快照已带车道,但基线复位让下一 tick 也必带一次(WebView 重载安全)
    lastSentLaneRevision_ = 0;
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
    // 首帧 viz **必带车道** —— 否则页面要空等一整个 4Hz 周期才出图。
    obj->setProperty("viz", buildVizPayload(/*includeLanes=*/true));
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

juce::var MonitorEditor::buildVizPayload(bool includeLanes) const
{
    // 载荷形状 = T46 的 `web/monitor/viz-contract.js` 镜像表(`VIZ_FIELDS` 的 json 列)。
    // 两条硬口径:
    //   ① **UI 永不见样本**(契约 §0.2 第 3 条)—— 时间量在这里就换成秒;
    //   ② 每轨数据一律**定长 15 的平行数组、下标即轨号**,与段内定长表同形。
    //      不用 [{ch,…}] 对象数组:段本来就是定长表,包成对象反而凭空造出「ch 合不合法」的问题。
    const auto& v = processor_.vizSnapshot();
    const bool online = processor_.vizState() == ScvbMonitorAudioProcessor::VizState::kOnline;

    auto* obj = new juce::DynamicObject();
    obj->setProperty("magic", "SCVB");
    obj->setProperty("abi", static_cast<int>(scvb::kScvbAbi));
    obj->setProperty("generation", static_cast<int>(v.generation));
    obj->setProperty("columnCount", static_cast<int>(scvb::kVizColumns));
    obj->setProperty("trackCount", static_cast<int>(scvb::kMaxChannels));
    obj->setProperty("panScale", static_cast<int>(scvb::kVizPanScale));

    // `online` = **段已 attach 且可读**;`fresh` = **帧还在更新**。这是两件事,分别给。
    // 曾经把它们与在一起 —— 于是「写方停摆」在载荷上与「真掉线」完全同形,消费侧只能靠
    // 「先看哪个字段」的判据顺序把它们分开,而顺序写反是「看起来完全正常」的错(T46 实测踩到:
    // Output 明明还在跑,页面把整张图清空了)。现在两个字段各说各的事实,不需要任何顺序约定:
    //   online=true,  fresh=true  ⇒ 在线且在更新
    //   online=true,  fresh=false ⇒ **在线但停更**(Output 还在,只是不发帧)⇒ 横幅,**别清图**
    //   online=false              ⇒ 掉线 / 未接通 ⇒ 空态
    obj->setProperty("online", online);
    obj->setProperty("fresh", processor_.vizFresh());
    // 帧自带组号:换组后若有在途帧,消费侧可据此丢弃 —— 不必依赖「事件一定按序到达」这条假设。
    obj->setProperty("groupId", processor_.groupId());
    obj->setProperty("seq", static_cast<int>(v.seq));
    obj->setProperty("publishMs", static_cast<juce::int64>(v.publishMs));
    obj->setProperty("sampleRate", static_cast<int>(v.sampleRate));
    obj->setProperty("versionActive", static_cast<int>(v.versionActive));
    obj->setProperty("playheadEpoch", static_cast<int>(v.playheadEpoch));
    obj->setProperty("playheadFlags", static_cast<int>(v.playheadFlags));
    obj->setProperty("onlineMask", static_cast<int>(v.onlineMask));
    obj->setProperty("coveredMask", static_cast<int>(v.coveredMask));
    obj->setProperty("stereoMask", static_cast<int>(v.stereoMask));
    obj->setProperty("leadMask", static_cast<int>(v.leadMask));
    obj->setProperty("laneRevision", static_cast<int>(v.laneRevision));

    // samples → 秒。`sample_rate == 0`(未 prepare)时**一律给 null/0,绝不做除法** ——
    // 除零得 NaN,而 NaN 在画布上是「什么都不画」,与「没有数据」长得一模一样,查起来极痛苦。
    const double sr = static_cast<double>(v.sampleRate);
    const bool haveSr = sr > 0.0;
    obj->setProperty("windowStartS", haveSr ? static_cast<double>(v.windowStartSamples) / sr : 0.0);
    obj->setProperty("windowSpanS", haveSr ? static_cast<double>(v.windowSpanSamples) / sr : 0.0);
    // playhead_samples < 0 = 无时间线 ⇒ **null 而不是 0**:给 0 会把竖线钉在开头,
    // 看着像「停在 0 秒」,和「没有时间线」是两件完全不同的事。
    obj->setProperty("playheadS", (haveSr && v.playheadSamples >= 0)
                                      ? juce::var(static_cast<double>(v.playheadSamples) / sr)
                                      : juce::var());
    const bool loopValid =
        (v.playheadFlags & scvb::kVizLoopValid) != 0u && haveSr && v.loopStartSamples >= 0 && v.loopEndSamples >= 0;
    obj->setProperty("loopStartS", loopValid ? juce::var(static_cast<double>(v.loopStartSamples) / sr) : juce::var());
    obj->setProperty("loopEndS", loopValid ? juce::var(static_cast<double>(v.loopEndSamples) / sr) : juce::var());

    // 每轨标量:定长 15,下标即轨号。哨兵 → **null**(不是 0)——
    // 0 在三条里分别是「居中 / 0 dB / 宽度 0」,每一个都是合法值,拿它当「没数据」会画出合法的假柱。
    juce::Array<juce::var> colorIndex;
    juce::Array<juce::var> panNow;
    juce::Array<juce::var> volDb;
    juce::Array<juce::var> widthPct;
    juce::Array<juce::var> labels;
    for (scvb::u32 t = 0; t < scvb::kMaxChannels; ++t)
    {
        colorIndex.add(static_cast<int>(v.trackColor[t]));
        panNow.add(scvb::vizPanIsNone(v.panNow[t]) ? juce::var() : juce::var(scvb::vizUnpackPan(v.panNow[t])));
        volDb.add(scvb::vizPanIsNone(v.volDb[t]) ? juce::var() : juce::var(scvb::vizUnpackFixed(v.volDb[t])));
        widthPct.add(scvb::vizPanIsNone(v.widthPct[t]) ? juce::var() : juce::var(scvb::vizUnpackFixed(v.widthPct[t])));
        labels.add(juce::String::fromUTF8(v.label[t].c_str(), static_cast<int>(v.label[t].size())));
    }
    obj->setProperty("colorIndex", colorIndex);
    obj->setProperty("trackPanNow", panNow);
    obj->setProperty("trackVolDb", volDb);
    obj->setProperty("trackWidthPct", widthPct);
    obj->setProperty("trackLabels", labels);

    // 车道 + 位图:只在 lane_revision 变化(或首帧/换组)时带。稳态帧只有上面那些标量 ——
    // 15×1024 个数逐帧走 juce::var 会在 4Hz 上白烧消息线程。
    // 读方约定(T46 已实现):revision 或 groupId 对不上时**宁可当作没有车道**,
    // 也不拿旧车道配新帧头 —— 那会得到一张「时间轴新、线旧」而看起来完全正常的图。
    if (includeLanes)
    {
        juce::Array<juce::var> lanes;
        juce::Array<juce::var> coverage;
        for (scvb::u32 t = 0; t < scvb::kMaxChannels; ++t)
        {
            juce::Array<juce::var> lane;
            lane.ensureStorageAllocated(static_cast<int>(scvb::kVizColumns));
            for (scvb::u32 i = 0; i < scvb::kVizColumns; ++i)
            {
                lane.add(static_cast<int>(v.pan[t][i])); // 原始 int16 定点(含哨兵),解码归读侧
            }
            lanes.add(lane);

            juce::Array<juce::var> words;
            words.ensureStorageAllocated(static_cast<int>(scvb::kVizCoverageWords));
            for (scvb::u32 w = 0; w < scvb::kVizCoverageWords; ++w)
            {
                // 位序 LSB 优先(列 i → words[i>>>5] 的 bit(i&31));JS 侧务必 `>>> 0` 折回无符号。
                words.add(static_cast<juce::int64>(v.coverage[t][w]));
            }
            coverage.add(words);
        }
        obj->setProperty("lanes", lanes);
        obj->setProperty("coverage", coverage);
    }
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
        // 车道只在 revision 变化(或从未送过)时带 —— 稳态帧就是一小把标量。
        const auto rev = processor_.vizSnapshot().laneRevision;
        const bool needLanes = !sentLanes_ || rev != lastSentLaneRevision_;
        if (emitIfChanged(bridge::kEvViz, buildVizPayload(needLanes), lastVizJson_) && needLanes)
        {
            // 只在**事件真的发出去**之后才推进基线:编辑器隐藏时事件被丢弃,
            // 若此时推进,恢复可见后就再也不会补发车道了(PR#54 R4 同款教训)。
            sentLanes_ = true;
            lastSentLaneRevision_ = rev;
        }
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
