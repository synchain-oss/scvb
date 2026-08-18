// SPDX-License-Identifier: GPL-3.0-or-later
#include "OutputEditor.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include "BridgeArgs.h"
#include "SegmentEditService.h"
#include "engine/CurveEvaluator.h"
#include "state/SegmentEdit.h"
#include "state/StateCodec.h"

namespace scvb::output
{

namespace
{

using namespace scvb::outputbridge;

// ---- juce::var 构造助手 ----
juce::var obj()
{
    return juce::var(new juce::DynamicObject());
}

void put(juce::var& o, const char* key, const juce::var& value)
{
    if (auto* d = o.getDynamicObject())
        d->setProperty(juce::Identifier(key), value);
}

juce::var mkArray()
{
    return juce::var(juce::Array<juce::var>());
}

void push(juce::var& arr, const juce::var& value)
{
    arr.append(value);
}

// ---- 回执助手 ----
juce::var okResp()
{
    juce::var o = obj();
    put(o, "ok", true);
    return o;
}

juce::var badArgResp()
{
    juce::var o = obj();
    put(o, "ok", false);
    put(o, "reason", "badArg");
    return o;
}

juce::var observerResp()
{
    juce::var o = obj();
    put(o, "observer", true);
    return o;
}

// ---- 枚举 → 字符串 ----
const char* rangeModeName(int mode)
{
    switch (mode)
    {
    case 1:
        return "daw_loop";
    case 2:
        return "manual";
    case 0:
    default:
        return "follow";
    }
}

const char* originName(scvb::state::SegmentOrigin o)
{
    switch (o)
    {
    case scvb::state::SegmentOrigin::UserEdited:
        return "user_edited";
    case scvb::state::SegmentOrigin::UserCreated:
        return "user_created";
    case scvb::state::SegmentOrigin::Auto:
    default:
        return "auto";
    }
}

const char* shapeName(scvb::PanCurveShape s)
{
    switch (s)
    {
    case scvb::PanCurveShape::shelf:
        return "shelf";
    case scvb::PanCurveShape::cut:
        return "cut";
    case scvb::PanCurveShape::bell:
    default:
        return "bell";
    }
}

const char* sideName(scvb::PanCurveSide s)
{
    switch (s)
    {
    case scvb::PanCurveSide::left:
        return "left";
    case scvb::PanCurveSide::right:
        return "right";
    case scvb::PanCurveSide::out:
    default:
        return "out";
    }
}

} // namespace

// ============================================================================
// 构造
// ============================================================================
OutputEditor::OutputEditor(ScvbOutputAudioProcessor& processor)
    : scvb::webview::WebViewHost(processor,
                                 [&] {
                                     scvb::webview::WebViewHost::Config config;
                                     config.role = "output";
                                     config.userDataFolderName = "scvb-output-webview";
                                     config.version = "0.1.0"; // 项目版本(CMake project VERSION)
                                     config.lang = processor.uiLanguage().toStdString();
                                     config.uiScale = static_cast<float>(processor.uiScalePercent()) / 100.0f;
                                     config.channelLimit = 15;
                                     config.resourceSource = {}; // 空 Source(web 资源嵌入归 T27b/T28)
                                     config.augmentOptions = [this](juce::WebBrowserComponent::Options& o) {
                                         registerNativeFunctions(o);
                                     };
                                     return config;
                                 }()),
      processor_(processor)
{
}

// ============================================================================
// 首帧全量快照(契约 §1.1)
// ============================================================================
juce::var OutputEditor::buildSnapshot()
{
    juce::var o = buildStateSubtree(true);

    // 快照专属字段(§1.1 语义行):session_guid / version / guide_seen_global / tour_seen_global / conn。
    put(o, "session_guid", juce::var("00000000-0000-0000-0000-000000000000")); // 会话 GUID 归 T21/T40 持久化
    juce::var version = obj();
    put(version, "plugin", "0.1.0");
    put(version, "abi", static_cast<int>(scvb::kScvbAbi));
    put(o, "version", version);
    put(o, "guide_seen_global", false); // 系统级全局默认(T31 落盘)
    put(o, "tour_seen_global", false);
    put(o, "conn", buildConnPayload());
    return o;
}

// ============================================================================
// 25Hz diff-then-emit(契约 §0.4)
// ============================================================================
void OutputEditor::emitTick()
{
    ++tickCount_;
    const bool first = firstFrame_;
    if (first)
        firstFrame_ = false;

    emitState(first);
    emitParams(first);
    if (first || (tickCount_ % 6 == 0))
        emitConn(); // ~4Hz(25Hz 6 分频)
    if (first || (tickCount_ % 25 == 0))
        emitGroups(); // 1Hz(25Hz 25 分频)
    emitMeters(); // 25Hz + 0.3dB 阈值
    emitPlayhead(); // 25Hz + diff

    if (first)
        emitSegments("snapshot", true);

    // scvb.captureProgress:仅播放中 2Hz;T29 无采集覆盖数据源,非播放不发(§2.7)。
    // scvb.error:仅条件成立时发(§2.9),T29 无触发面。
}

// ============================================================================
// 事件发射
// ============================================================================
void OutputEditor::emitState(bool forceFull)
{
    (void)forceFull; // T29 增量子树未实现:一律 full:true 全量下发(UI 全量替换)。
    juce::var payload = buildStateSubtree(true);
    put(payload, "full", true);

    const juce::String json = juce::JSON::toString(payload);
    if (json != lastStateJson_)
    {
        lastStateJson_ = json;
        webView().emitEventIfBrowserIsVisible(Event::State, payload);
    }
}

void OutputEditor::emitParams(bool forceFull)
{
    auto& apvts = processor_.getAPVTS();
    const int v = processor_.versionActive();

    // 63 个 id:全局三件 + 当前激活版本 15 轨 × 4。
    juce::Array<juce::String> ids;
    ids.add("width");
    ids.add("ms_balance");
    ids.add("lead_select");
    for (int t = 1; t <= 15; ++t)
    {
        ids.add(scvb::params::panId(v, t));
        ids.add(scvb::params::volId(v, t));
        ids.add(scvb::params::widthId(v, t));
        ids.add(scvb::params::freezeId(v, t));
    }

    juce::var values = obj();
    bool any = false;
    for (const auto& id : ids)
    {
        const float value = readParamEngineering(apvts, id); // 工程值(非归一化,PR#55 第3轮重要1)
        const auto it = lastParamsValues_.find(id);
        const bool changed = it == lastParamsValues_.end() ||
                             !juce::approximatelyEqual(static_cast<double>(it->second), static_cast<double>(value));
        if (forceFull || changed)
        {
            put(values, id.toRawUTF8(), juce::var(value));
            lastParamsValues_[id] = value;
            any = true;
        }
    }

    if (!any && !forceFull)
        return;

    juce::var payload = obj();
    put(payload, "values", values);
    put(payload, "hostEcho", false); // host echo 屏蔽→灰显归 T31/T32;桥面先恒 false
    put(payload, "full", forceFull);
    put(payload, "versionActive", v);

    const juce::String json = juce::JSON::toString(payload);
    if (json != lastParamsJson_)
    {
        lastParamsJson_ = json;
        webView().emitEventIfBrowserIsVisible(Event::Params, payload);
    }
}

void OutputEditor::emitConn()
{
    juce::var payload = buildConnPayload();
    const juce::String json = juce::JSON::toString(payload);
    if (json != lastConnJson_)
    {
        lastConnJson_ = json;
        webView().emitEventIfBrowserIsVisible(Event::Conn, payload);
    }
}

void OutputEditor::emitGroups()
{
    const std::uint8_t bitmap = processor_.probeGroupsOnline();
    if (groupsEverSent_ && bitmap == lastGroupsOnline_)
        return; // 变化才发(首帧必发)

    groupsEverSent_ = true;
    lastGroupsOnline_ = bitmap;
    juce::var payload = obj();
    put(payload, "groups_online", static_cast<int>(bitmap));
    webView().emitEventIfBrowserIsVisible(Event::Groups, payload);
}

void OutputEditor::emitMeters()
{
    // T29: no live meter source (post-gain levels wired in T32), all tracks sit at the -60 dB floor.
    // 0.3 dB threshold (contract 0.4-2): emit only when any track/bus moved >= threshold, first frame always.
    constexpr float kFloorDb = -60.0f;
    constexpr float kMeterThresholdDb = 0.3f;

    std::array<float, 15> db;
    std::array<float, 15> peak;
    db.fill(kFloorDb);
    peak.fill(kFloorDb);
    const float busL = kFloorDb;
    const float busR = kFloorDb;

    bool changed = !metersEverSent_;
    for (int t = 0; t < 15 && !changed; ++t)
        changed = std::fabs(db[static_cast<std::size_t>(t)] - lastMeterDb_[static_cast<std::size_t>(t)]) >=
                      kMeterThresholdDb ||
                  std::fabs(peak[static_cast<std::size_t>(t)] - lastMeterPeak_[static_cast<std::size_t>(t)]) >=
                      kMeterThresholdDb;
    if (!changed)
        changed = std::fabs(busL - lastBusL_) >= kMeterThresholdDb || std::fabs(busR - lastBusR_) >= kMeterThresholdDb;

    if (!changed)
        return;

    metersEverSent_ = true;
    lastMeterDb_ = db;
    lastMeterPeak_ = peak;
    lastBusL_ = busL;
    lastBusR_ = busR;

    juce::var tracks = mkArray();
    for (int t = 0; t < 15; ++t)
    {
        juce::var tr = obj();
        put(tr, "db", static_cast<double>(db[static_cast<std::size_t>(t)]));
        put(tr, "peakDb", static_cast<double>(peak[static_cast<std::size_t>(t)]));
        push(tracks, tr);
    }
    juce::var bus = obj();
    juce::var l = obj();
    put(l, "db", static_cast<double>(busL));
    put(l, "peakDb", static_cast<double>(busL));
    juce::var r = obj();
    put(r, "db", static_cast<double>(busR));
    put(r, "peakDb", static_cast<double>(busR));
    put(bus, "l", l);
    put(bus, "r", r);

    juce::var payload = obj();
    put(payload, "tracks", tracks);
    put(payload, "bus", bus);
    webView().emitEventIfBrowserIsVisible(Event::Meters, payload);
}

void OutputEditor::emitPlayhead()
{
    // 读音频线程发布的 playheadShot_ SPSC 快照(PR#55 建议①;不直读宿主 AudioPlayHead)。
    const scvb::engine::PlayheadPod pod = processor_.playheadSnapshot();
    const bool playing = (pod.flags & scvb::engine::kPlayheadIsPlaying) != 0;
    const double timeS = pod.timeSamples >= 0 ? static_cast<double>(pod.timeSamples) / processor_.sampleRate() : 0.0;

    // inRange(§2.6):mode=follow 恒 true;否则落在 [startS, endS)。
    const auto& rt = processor_.runtime();
    bool inRange = true;
    if (rt.rangeMode != 0)
        inRange = timeS >= rt.rangeStartS && timeS < rt.rangeEndS;

    juce::var payload = obj();
    put(payload, "timeS", timeS);
    put(payload, "isPlaying", playing);
    put(payload, "inRange", inRange);
    // loopStartS/loopEndS:仅宿主提供且 kCycleValid 时出现;T29 未接 loop 换算,缺失即不出现。

    const juce::String json = juce::JSON::toString(payload);
    if (json != lastPlayheadJson_)
    {
        lastPlayheadJson_ = json;
        webView().emitEventIfBrowserIsVisible(Event::Playhead, payload);
    }
}

void OutputEditor::emitCaptureProgress()
{
    // §2.7:非播放不发;T29 无覆盖增量数据源,不实现(占位,避免遗漏事件名)。
}

void OutputEditor::emitSegments(const juce::String& reason, bool allTracks)
{
    webView().emitEventIfBrowserIsVisible(Event::Segments, buildSegmentsPayload(reason, allTracks));
}

void OutputEditor::emitError(const juce::String& code, int ch, const juce::var& detail, bool active)
{
    juce::var payload = obj();
    put(payload, "code", code);
    if (ch >= 1 && ch <= 15)
        put(payload, "ch", ch);
    put(payload, "detail", detail);
    put(payload, "active", active);
    webView().emitEventIfBrowserIsVisible(Event::Error, payload);
}

// ============================================================================
// 载荷构造
// ============================================================================
juce::var OutputEditor::buildStateSubtree(bool /*full*/) const
{
    const auto& rt = processor_.runtime();
    const scvb::state::CrvsData crvs = processor_.crvsSnapshot(); // 持锁快照(PR#55 重要1)

    juce::var o = obj();
    put(o, "config_seq", static_cast<int>(rt.configSeq));
    put(o, "group_id", processor_.groupId());

    juce::var global = obj();
    put(global, "capture_enabled", processor_.captureEnabled());
    put(global, "output_enabled", processor_.outputEnabled());
    put(global, "version_active", processor_.versionActive());
    juce::var range = obj();
    put(range, "mode", rangeModeName(rt.rangeMode));
    put(range, "start_s", rt.rangeStartS);
    put(range, "end_s", rt.rangeEndS);
    put(global, "range", range);
    put(o, "global", global);

    juce::var analysis = obj();
    juce::var vad = obj();
    put(vad, "threshold_db", rt.vadThresholdDb);
    put(vad, "hysteresis_db", rt.vadHysteresisDb);
    put(vad, "hangover_ms", rt.vadHangoverMs);
    put(vad, "padding_pre_ms", rt.vadPaddingPreMs);
    put(vad, "padding_post_ms", rt.vadPaddingPostMs);
    put(analysis, "vad", vad);
    juce::var segmentation = obj();
    put(segmentation, "mode", rt.segmentationMode);
    put(segmentation, "sensitivity", rt.segmentationSensitivity);
    put(segmentation, "min_segment_ms", rt.segmentationMinSegmentMs);
    put(analysis, "segmentation", segmentation);
    put(analysis, "transition_ramp_ms", rt.transitionRampMs);
    put(analysis, "loudness_mode", rt.loudnessMode);
    put(analysis, "center_slot_policy", rt.centerSlotPolicy);
    put(o, "analysis", analysis);

    juce::var channels = mkArray();
    for (int t = 0; t < 15; ++t)
    {
        const auto& c = rt.channels[static_cast<std::size_t>(t)];
        juce::var ch = obj();
        put(ch, "enabled", c.enabled);
        put(ch, "label", c.label);
        put(ch, "source_channels", c.sourceChannels);
        // J60:未显式设置时按 mono=true / stereo=false 推导。
        const bool participate = c.participateAutoPanSet ? c.participateAutoPan : (c.sourceChannels == 1);
        put(ch, "participate_in_auto_pan", participate);
        put(ch, "priority", c.priority);
        put(ch, "lead_lock", c.leadLock);
        put(ch, "lead_vol_exempt", c.leadVolExempt);
        put(ch, "pair_id", c.pairId);
        push(channels, ch);
    }
    put(o, "channels", channels);

    juce::var versions = mkArray();
    for (int v = 0; v < 2; ++v)
    {
        const auto& vc = crvs.versions[static_cast<std::size_t>(v)];
        juce::var ver = obj();
        const juce::String name =
            vc.meta.name.empty() ? juce::String::formatted("V%d", v + 1) : juce::String::fromUTF8(vc.meta.name.c_str());
        put(ver, "name", name);
        bool empty = true;
        for (int t = 0; t < 15 && empty; ++t)
            empty = vc.tracks[static_cast<std::size_t>(t)].segments.empty();
        put(ver, "empty", empty);
        juce::var panCurve = obj();
        juce::var points = mkArray();
        for (const auto& p : vc.panCurve)
        {
            juce::var pt = obj();
            put(pt, "angle", p.angle);
            put(pt, "gain_db", p.gainDb);
            put(pt, "shape", shapeName(p.shape));
            put(pt, "q", p.q);
            put(pt, "side", sideName(p.side));
            push(points, pt);
        }
        put(panCurve, "points", points);
        put(ver, "pan_curve", panCurve);
        push(versions, ver);
    }
    put(o, "versions", versions);

    juce::var features = obj();
    put(features, "embedded", false);
    put(features, "bytes", static_cast<juce::int64>(0));
    put(o, "features", features);

    juce::var ui = obj();
    put(ui, "scale", static_cast<float>(processor_.uiScalePercent()) / 100.0f);
    put(ui, "language", processor_.uiLanguage());
    put(ui, "active_tab", rt.activeTab);
    put(ui, "guide_seen", rt.guideSeen);
    put(ui, "tour_seen", rt.tourSeen);
    put(o, "ui", ui);

    juce::var printGuard = obj();
    put(printGuard, "pending", rt.printGuardPending);
    put(o, "print_guard", printGuard);

    juce::var recapture = obj();
    put(recapture, "armed", rt.recaptureArmed);
    put(recapture, "tracksMask", static_cast<int>(rt.recaptureTracksMask));
    put(recapture, "startS", rt.recaptureStartS);
    put(recapture, "endS", rt.recaptureEndS);
    put(recapture, "autoStop", rt.recaptureAutoStop);
    put(o, "recapture", recapture);

    juce::var analysisRun = obj();
    put(analysisRun, "running", rt.analysisRunning);
    if (rt.analysisHasProgress)
        put(analysisRun, "progress", rt.analysisProgress);
    put(o, "analysis_run", analysisRun);

    return o;
}

juce::var OutputEditor::buildConnPayload() const
{
    const bool readOnly = processor_.isReadOnly();

    juce::var channels = mkArray();
    for (int t = 0; t < 15; ++t)
    {
        juce::var ch = obj();
        // T29:per-channel slotState/heartbeat 真身归 T30/T32(读本组 registry InputSlot);
        // 此处以 session 视角的最小面填充:空闲 0 / 活跃 2(仅当本实例为 active)。
        const int slotState = (!readOnly) ? 2 : 0;
        put(ch, "slotState", slotState);
        put(ch, "heartbeatAgeMs", static_cast<juce::int64>(0xFFFFFFFFu)); // 哨兵「无数据」
        put(ch, "heartbeatFresh", false); // heartbeatAgeMs=0xFFFFFFFF 哨兵 → fresh=false(§2.3 派生一致)
        put(ch, "capturing", false);
        put(ch, "misalignCount", static_cast<int>(processor_.gapCount(t + 1)));
        put(ch, "srMismatch", false);
        push(channels, ch);
    }

    juce::var o = obj();
    put(o, "channels", channels);
    put(o, "outputReadOnly", readOnly);
    put(o, "generation", 0);
    return o;
}

juce::var OutputEditor::buildSegmentsPayload(const juce::String& reason, bool allTracks) const
{
    const int v = processor_.versionActive();
    const scvb::state::CrvsData crvs = processor_.crvsSnapshot(); // 持锁快照(PR#55 重要1)
    const auto& vc = crvs.versions[static_cast<std::size_t>(v - 1)];
    const double sr = processor_.sampleRate();

    juce::var channels = mkArray();
    for (int t = 0; t < 15; ++t)
    {
        const auto& segments = vc.tracks[static_cast<std::size_t>(t)].segments;
        if (!allTracks && segments.empty())
            continue; // 增量:只含受影响(有段)轨;snapshot/versionActive 含全部轨

        juce::var ch = obj();
        put(ch, "ch", t + 1);
        juce::var segArr = mkArray();
        for (std::size_t i = 0; i < segments.size(); ++i)
        {
            const auto& s = segments[i];
            juce::var seg = obj();
            put(seg, "segIdx", static_cast<int>(i));
            put(seg, "t0S", static_cast<double>(s.t0) / sr);
            put(seg, "t1S", static_cast<double>(s.t1) / sr);
            put(seg, "pan", static_cast<double>(s.pan));
            put(seg, "volDb", static_cast<double>(s.volDb));
            put(seg, "origin", originName(scvb::state::segmentOrigin(s.flags)));
            put(seg, "locked", scvb::state::segmentLocked(s.flags));
            put(seg, "loudnessLufs", 0.0); // 段内积分响度归 T21 分析管线
            push(segArr, seg);
        }
        put(ch, "segments", segArr);
        put(ch, "stale", false);
        push(channels, ch);
    }

    juce::var diff = obj();
    put(diff, "kept", 0);
    put(diff, "changed", mkArray());
    put(diff, "added", 0);
    put(diff, "removed", 0);

    juce::var o = obj();
    put(o, "version", v);
    put(o, "reason", reason);
    put(o, "channels", channels);
    put(o, "diff", diff);
    return o;
}

// ============================================================================
// native function 注册(30 个插件专属)
// ============================================================================
void OutputEditor::registerNativeFunctions(juce::WebBrowserComponent::Options& options)
{
    using WBC = juce::WebBrowserComponent;

    auto add = [&options, this](const char* name, void (OutputEditor::*handler)(const ArgList&, Completion)) {
        options = options.withNativeFunction(juce::Identifier(name), [this, handler](const ArgList& a, Completion c) {
            (this->*handler)(a, std::move(c));
        });
    };

    add(Fn::SetCaptureEnabled, &OutputEditor::handleSetCaptureEnabled);
    add(Fn::SetOutputEnabled, &OutputEditor::handleSetOutputEnabled);
    add(Fn::SetGroupId, &OutputEditor::handleSetGroupId);
    add(Fn::PreviewAnalyze, &OutputEditor::handlePreviewAnalyze);
    add(Fn::Analyze, &OutputEditor::handleAnalyze);
    add(Fn::CancelAnalyze, &OutputEditor::handleCancelAnalyze);
    add(Fn::SetRange, &OutputEditor::handleSetRange);
    add(Fn::SetVersionActive, &OutputEditor::handleSetVersionActive);
    add(Fn::SetVersionName, &OutputEditor::handleSetVersionName);
    add(Fn::CopyVersion, &OutputEditor::handleCopyVersion);
    add(Fn::BeginParamGesture, &OutputEditor::handleBeginParamGesture);
    add(Fn::SetParam, &OutputEditor::handleSetParam);
    add(Fn::EndParamGesture, &OutputEditor::handleEndParamGesture);
    add(Fn::SetChannelConfig, &OutputEditor::handleSetChannelConfig);
    add(Fn::SetTrackManual, &OutputEditor::handleSetTrackManual);
    add(Fn::SetPanCurve, &OutputEditor::handleSetPanCurve);
    add(Fn::SetVadParams, &OutputEditor::handleSetVadParams);
    add(Fn::SetSegmentation, &OutputEditor::handleSetSegmentation);
    add(Fn::SetTransitionRamp, &OutputEditor::handleSetTransitionRamp);
    add(Fn::SetAnalysisConfig, &OutputEditor::handleSetAnalysisConfig);
    add(Fn::EditSegment, &OutputEditor::handleEditSegment);
    add(Fn::RecaptureArm, &OutputEditor::handleRecaptureArm);
    add(Fn::ClearCoverage, &OutputEditor::handleClearCoverage);
    add(Fn::Undo, &OutputEditor::handleUndo);
    add(Fn::Redo, &OutputEditor::handleRedo);
    add(Fn::RequestWaveform, &OutputEditor::handleRequestWaveform);
    add(Fn::SetActiveTab, &OutputEditor::handleSetActiveTab);
    add(Fn::SetGuideSeen, &OutputEditor::handleSetGuideSeen);
    add(Fn::SetTourSeen, &OutputEditor::handleSetTourSeen);
    add(Fn::ConfirmPrintGuard, &OutputEditor::handleConfirmPrintGuard);
}

// ============================================================================
// 只读态
// ============================================================================
bool OutputEditor::isReadOnly() const
{
    return processor_.isReadOnly();
}

// ============================================================================
// handlers
// ============================================================================
void OutputEditor::handleSetCaptureEnabled(const ArgList& a, Completion c)
{
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }
    bool on = false;
    if (a.size() < 1 || !strictBool(a[0], on))
    {
        c(badArgResp());
        return;
    }
    processor_.setCaptureEnabled(on);
    c(okResp());
}

void OutputEditor::handleSetOutputEnabled(const ArgList& a, Completion c)
{
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }
    bool on = false;
    if (a.size() < 1 || !strictBool(a[0], on))
    {
        c(badArgResp());
        return;
    }
    processor_.setOutputEnabled(on);
    c(okResp());
}

void OutputEditor::handleSetGroupId(const ArgList& a, Completion c)
{
    const int g = a.size() > 0 ? static_cast<int>(a[0]) : 1;
    if (g < 1 || g > 8)
    {
        c(badArgResp());
        return;
    }
    processor_.setGroupId(g);
    // §1.4:改组后新组已有主 Output → 本实例进只读观察 → {observer:true}(PR#55 建议②)。
    if (processor_.isReadOnly())
    {
        c(observerResp());
        return;
    }
    c(okResp());
}

void OutputEditor::handlePreviewAnalyze(const ArgList& /*a*/, Completion c)
{
    // 纯只读 dry-run:范围∩覆盖 ∩ origin≠auto 段相交;T29 无覆盖/分析管线 → 空集合。
    juce::var o = obj();
    put(o, "intervals", 0);
    put(o, "tracks", 0);
    put(o, "manualKept", 0);
    c(o);
}

void OutputEditor::handleAnalyze(const ArgList& /*a*/, Completion c)
{
    // §1.6:受理回执 + 影响面,立即 resolve(长耗时分析绝不阻塞消息线程)。
    // T29:分析管线(FeatureExtractor/Segmentation/Reanalysis)未接线 → affected {0,0,0},不置 running。
    auto& rt = processor_.runtime();
    if (rt.analysisRunning)
    {
        juce::var o = obj();
        put(o, "ok", false);
        put(o, "reason", "busy");
        c(o);
        return;
    }
    juce::var affected = obj();
    put(affected, "intervals", 0);
    put(affected, "tracks", 0);
    put(affected, "manualKept", 0);
    juce::var o = obj();
    put(o, "ok", true);
    put(o, "affected", affected);
    c(o);
}

void OutputEditor::handleCancelAnalyze(const ArgList& /*a*/, Completion c)
{
    // T29:无进行中的分析。
    juce::var o = obj();
    put(o, "ok", false);
    c(o);
}

void OutputEditor::handleSetRange(const ArgList& a, Completion c)
{
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }
    const juce::String mode = a.size() > 0 ? a[0].toString() : juce::String("follow");
    const double startS = a.size() > 1 ? static_cast<double>(a[1]) : 0.0;
    const double endS = a.size() > 2 ? static_cast<double>(a[2]) : 0.0;

    int modeInt = 0;
    if (mode == "daw_loop")
        modeInt = 1;
    else if (mode == "manual")
        modeInt = 2;
    else if (mode != "follow")
    {
        c(badArgResp());
        return;
    }

    if (modeInt == 2 && startS >= endS)
    {
        c(badArgResp());
        return;
    }

    auto& rt = processor_.runtime();
    // 值变化才 config_seq+1(PR#55 缺陷4)。
    const bool changed =
        rt.rangeMode != modeInt || (modeInt != 0 && (rt.rangeStartS != startS || rt.rangeEndS != endS));
    rt.rangeMode = modeInt;
    if (modeInt != 0) // follow 忽略 startS/endS
    {
        rt.rangeStartS = startS;
        rt.rangeEndS = endS;
    }
    if (changed)
        ++rt.configSeq;
    c(okResp());
}

void OutputEditor::handleSetVersionActive(const ArgList& a, Completion c)
{
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }
    const int v = a.size() > 0 ? static_cast<int>(a[0]) : 1;
    if (v < 1 || v > 2)
    {
        c(badArgResp());
        return;
    }
    const int old = processor_.versionActive();
    processor_.setVersionActive(v);
    if (v != old)
    {
        ++processor_.runtime().configSeq; // 配置变更(PR#55 缺陷4)
        // §1.9/§2.2:切版本后全量重发 segments + state + params(PR#55 重要2)。
        emitSegments("versionActive", true);
        lastStateJson_.clear();
        lastParamsJson_.clear();
        lastParamsValues_.clear();
        emitParams(true); // full:true 全量重发 params
    }
    c(okResp());
}

void OutputEditor::handleSetVersionName(const ArgList& a, Completion c)
{
    const int v = a.size() > 0 ? static_cast<int>(a[0]) : 0;
    const juce::String name = a.size() > 1 ? a[1].toString() : juce::String();

    if (isReadOnly())
    {
        c(observerResp());
        return;
    }

    juce::String effective;
    const auto result = processor_.setVersionName(v, name, effective); // 持锁事务(PR#55 重要1)
    if (result == scvb::engine::SetNameResult::InvalidIndex)
    {
        c(badArgResp());
        return;
    }

    juce::var o = obj();
    put(o, "ok", true);
    put(o, "name", effective);
    c(o);
}

void OutputEditor::handleCopyVersion(const ArgList& a, Completion c)
{
    const int src = a.size() > 0 ? static_cast<int>(a[0]) : 0;
    const int dst = a.size() > 1 ? static_cast<int>(a[1]) : 0;
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }
    const auto result = processor_.copyVersion(src, dst); // 持锁事务(PR#55 重要1)
    if (result != scvb::engine::CopyVersionResult::Ok)
    {
        c(badArgResp());
        return;
    }

    emitSegments("copyVersion", true);
    c(okResp());
}

void OutputEditor::handleBeginParamGesture(const ArgList& a, Completion c)
{
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }
    const juce::String id = a.size() > 0 ? a[0].toString() : juce::String();
    if (!isGestureParam(id, processor_.versionActive())) // 白名单外 badArg(PR#55 第3轮重要2)
    {
        c(badArgResp());
        return;
    }
    if (auto* p = processor_.getAPVTS().getParameter(id))
    {
        p->beginChangeGesture();
        c(okResp());
        return;
    }
    c(badArgResp());
}

void OutputEditor::handleSetParam(const ArgList& a, Completion c)
{
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }
    const juce::String id = a.size() > 0 ? a[0].toString() : juce::String();
    if (!isGestureParam(id, processor_.versionActive())) // 白名单外 badArg(PR#55 第3轮重要2)
    {
        c(badArgResp());
        return;
    }
    const float value = a.size() > 1 ? static_cast<float>(a[1]) : 0.0f;
    if (auto* p = processor_.getAPVTS().getParameter(id))
    {
        p->setValueNotifyingHost(p->convertTo0to1(value)); // 工程值 → 归一化(§1.13)
        c(okResp());
        return;
    }
    c(badArgResp());
}

void OutputEditor::handleEndParamGesture(const ArgList& a, Completion c)
{
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }
    const juce::String id = a.size() > 0 ? a[0].toString() : juce::String();
    if (!isGestureParam(id, processor_.versionActive())) // 白名单外 badArg(PR#55 第3轮重要2)
    {
        c(badArgResp());
        return;
    }
    if (auto* p = processor_.getAPVTS().getParameter(id))
    {
        p->endChangeGesture();
        c(okResp());
        return;
    }
    c(badArgResp());
}

void OutputEditor::handleSetChannelConfig(const ArgList& a, Completion c)
{
    const int ch = a.size() > 0 ? static_cast<int>(a[0]) : 0;
    if (ch < 1 || ch > 15)
    {
        c(badArgResp());
        return;
    }
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }

    const auto& cur = processor_.runtime().channels[static_cast<std::size_t>(ch - 1)];

    // 阶段1:先对全部 patch 字段做完整校验到局部临时量,绝不触碰 channel(PR#55 第4轮缺陷2)。
    bool hasEnabled = false;
    bool enabled = false;
    bool hasLabel = false;
    juce::String label;
    bool hasPriority = false;
    int priority = 0;
    bool hasLeadLock = false;
    bool leadLock = false;
    bool hasLeadVolExempt = false;
    bool leadVolExempt = false;
    bool hasParticipate = false;
    bool participate = false;
    bool hasPairId = false;
    int pairId = 0;

    if (a.size() > 1 && a[1].isObject())
    {
        const juce::var patch = a[1];
        if (patch.hasProperty("source_channels") || patch.hasProperty("auto_pan") || patch.hasProperty("auto_vol"))
        {
            c(badArgResp()); // 只读/已删字段不可写(§1.15)
            return;
        }
        if (patch.hasProperty("enabled"))
        {
            if (!strictBool(patch.getProperty("enabled", cur.enabled), enabled))
            {
                c(badArgResp());
                return;
            }
            hasEnabled = true;
        }
        if (patch.hasProperty("label"))
        {
            label = patch.getProperty("label", juce::String()).toString().substring(0, 24);
            hasLabel = true;
        }
        if (patch.hasProperty("priority"))
        {
            priority = juce::jlimit(0, 10, static_cast<int>(patch.getProperty("priority", cur.priority)));
            hasPriority = true;
        }
        if (patch.hasProperty("lead_lock"))
        {
            if (!strictBool(patch.getProperty("lead_lock", cur.leadLock), leadLock))
            {
                c(badArgResp());
                return;
            }
            hasLeadLock = true;
        }
        if (patch.hasProperty("lead_vol_exempt"))
        {
            if (!strictBool(patch.getProperty("lead_vol_exempt", cur.leadVolExempt), leadVolExempt))
            {
                c(badArgResp());
                return;
            }
            hasLeadVolExempt = true;
        }
        if (patch.hasProperty("participate_in_auto_pan"))
        {
            if (!strictBool(patch.getProperty("participate_in_auto_pan", false), participate))
            {
                c(badArgResp());
                return;
            }
            hasParticipate = true;
        }
        if (patch.hasProperty("pair_id"))
        {
            pairId = juce::jlimit(0, 7, static_cast<int>(patch.getProperty("pair_id", cur.pairId)));
            hasPairId = true;
        }
    }

    // 阶段2:全部通过后一次性应用,变化才 bump config_seq。
    auto& channel = processor_.runtime().channels[static_cast<std::size_t>(ch - 1)];
    bool changed = false;
    if (hasEnabled)
    {
        changed |= enabled != channel.enabled;
        channel.enabled = enabled;
    }
    if (hasLabel)
    {
        changed |= label != channel.label;
        channel.label = label;
    }
    if (hasPriority)
    {
        changed |= priority != channel.priority;
        channel.priority = priority;
    }
    if (hasLeadLock)
    {
        changed |= leadLock != channel.leadLock;
        channel.leadLock = leadLock;
    }
    if (hasLeadVolExempt)
    {
        changed |= leadVolExempt != channel.leadVolExempt;
        channel.leadVolExempt = leadVolExempt;
    }
    if (hasParticipate)
    {
        changed |= participate != channel.participateAutoPan;
        channel.participateAutoPan = participate;
        channel.participateAutoPanSet = true;
    }
    if (hasPairId)
    {
        changed |= pairId != channel.pairId;
        channel.pairId = pairId;
    }

    if (changed)
        ++processor_.runtime().configSeq; // 广播区整体版本号,值变化才 bump(PR#55 缺陷4)
    c(okResp());
}

void OutputEditor::handleSetTrackManual(const ArgList& a, Completion c)
{
    const int ch = a.size() > 0 ? static_cast<int>(a[0]) : 0;
    const juce::String panOrVol = a.size() > 1 ? a[1].toString() : juce::String();
    const float value = a.size() > 2 ? static_cast<float>(a[2]) : 0.0f;
    if (ch < 1 || ch > 15 || (panOrVol != "pan" && panOrVol != "vol"))
    {
        c(badArgResp());
        return;
    }
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }

    int replacedSegments = 0;
    int replacedLocked = 0;
    // 持锁事务 + 如实统计替换前段数/锁定段数(PR#55 重要1/建议⑤)。
    if (!processor_.setTrackManual(ch, panOrVol == "pan", value, replacedSegments, replacedLocked))
    {
        c(badArgResp());
        return;
    }

    emitSegments("trackManual", false);
    juce::var o = obj();
    put(o, "ok", true);
    put(o, "replacedSegments", replacedSegments);
    put(o, "replacedLocked", replacedLocked);
    c(o);
}

void OutputEditor::handleSetPanCurve(const ArgList& a, Completion c)
{
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }

    std::vector<scvb::PanCurvePoint> points;
    if (a.size() > 0 && a[0].isArray())
    {
        const auto* arr = a[0].getArray();
        if (arr == nullptr || arr->size() > 16)
        {
            c(badArgResp());
            return;
        }
        for (const auto& item : *arr)
        {
            if (!item.isObject())
            {
                c(badArgResp());
                return;
            }
            scvb::PanCurvePoint p;
            p.angle = static_cast<float>(item.getProperty("angle", 0.0));
            p.gainDb = static_cast<float>(item.getProperty("gain_db", 0.0));
            p.q = static_cast<float>(item.getProperty("q", 1.5));
            const juce::String shape = item.getProperty("shape", juce::String()).toString();
            const juce::String side = item.getProperty("side", juce::String("out")).toString();
            if (shape == "shelf")
                p.shape = scvb::PanCurveShape::shelf;
            else if (shape == "cut")
                p.shape = scvb::PanCurveShape::cut;
            else if (shape != "bell")
            {
                c(badArgResp());
                return;
            }
            if (side == "left")
                p.side = scvb::PanCurveSide::left;
            else if (side == "right")
                p.side = scvb::PanCurveSide::right;
            else if (side != "out")
            {
                c(badArgResp());
                return;
            }
            if (p.q <= 0.0f || p.angle < -100.0f || p.angle > 100.0f)
            {
                c(badArgResp());
                return;
            }
            points.push_back(p);
        }
    }

    processor_.setPanCurve(processor_.versionActive(), points); // 持锁事务(PR#55 重要1)
    lastStateJson_.clear(); // pan_curve 经 scvb.state 回推
    c(okResp());
}

void OutputEditor::handleSetVadParams(const ArgList& a, Completion c)
{
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }
    if (a.size() < 1 || !a[0].isObject())
    {
        c(badArgResp());
        return;
    }
    const juce::var p = a[0];
    auto& rt = processor_.runtime();
    const float t = static_cast<float>(p.getProperty("threshold_db", rt.vadThresholdDb));
    const float h = static_cast<float>(p.getProperty("hysteresis_db", rt.vadHysteresisDb));
    const int hm = static_cast<int>(p.getProperty("hangover_ms", rt.vadHangoverMs));
    const int pp = static_cast<int>(p.getProperty("padding_pre_ms", rt.vadPaddingPreMs));
    const int po = static_cast<int>(p.getProperty("padding_post_ms", rt.vadPaddingPostMs));
    const bool changed = t != rt.vadThresholdDb || h != rt.vadHysteresisDb || hm != rt.vadHangoverMs ||
                         pp != rt.vadPaddingPreMs || po != rt.vadPaddingPostMs;
    rt.vadThresholdDb = t;
    rt.vadHysteresisDb = h;
    rt.vadHangoverMs = hm;
    rt.vadPaddingPreMs = pp;
    rt.vadPaddingPostMs = po;
    if (changed)
        ++rt.configSeq; // PR#55 缺陷4
    c(okResp());
}

void OutputEditor::handleSetSegmentation(const ArgList& a, Completion c)
{
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }
    if (a.size() < 1 || !a[0].isObject())
    {
        c(badArgResp());
        return;
    }
    const juce::var p = a[0];
    auto& rt = processor_.runtime();
    const juce::String mode = p.getProperty("mode", rt.segmentationMode).toString();
    const float sens = static_cast<float>(p.getProperty("sensitivity", rt.segmentationSensitivity));
    const int mms = static_cast<int>(p.getProperty("min_segment_ms", rt.segmentationMinSegmentMs));
    const bool changed =
        mode != rt.segmentationMode || sens != rt.segmentationSensitivity || mms != rt.segmentationMinSegmentMs;
    rt.segmentationMode = mode;
    rt.segmentationSensitivity = sens;
    rt.segmentationMinSegmentMs = mms;
    if (changed)
        ++rt.configSeq; // PR#55 缺陷4
    c(okResp());
}

void OutputEditor::handleSetTransitionRamp(const ArgList& a, Completion c)
{
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }
    const float ms = a.size() > 0 ? static_cast<float>(a[0]) : 80.0f;
    const float clamped = juce::jlimit(20.0f, 300.0f, ms);
    if (processor_.runtime().transitionRampMs != clamped)
    {
        processor_.runtime().transitionRampMs = clamped;
        ++processor_.runtime().configSeq; // PR#55 缺陷4
    }
    c(okResp());
}

void OutputEditor::handleSetAnalysisConfig(const ArgList& a, Completion c)
{
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }
    if (a.size() < 1 || !a[0].isObject())
    {
        c(badArgResp());
        return;
    }
    const juce::var patch = a[0];
    auto& rt = processor_.runtime();

    // 阶段1:先校验全部枚举字段到局部量(PR#55 第4轮缺陷2)。
    juce::String loudnessMode = rt.loudnessMode;
    juce::String centerSlotPolicy = rt.centerSlotPolicy;
    bool hasLoudness = false;
    bool hasCenter = false;
    if (patch.hasProperty("loudness_mode"))
    {
        loudnessMode = patch.getProperty("loudness_mode", juce::String()).toString();
        if (loudnessMode != "kw_integrated" && loudnessMode != "rms" && loudnessMode != "peak_dbfs")
        {
            c(badArgResp());
            return;
        }
        hasLoudness = true;
    }
    if (patch.hasProperty("center_slot_policy"))
    {
        centerSlotPolicy = patch.getProperty("center_slot_policy", juce::String()).toString();
        if (centerSlotPolicy != "priority_queue" && centerSlotPolicy != "lead_exclusive" &&
            centerSlotPolicy != "even_spread")
        {
            c(badArgResp());
            return;
        }
        hasCenter = true;
    }

    // 阶段2:全部通过后一次性应用,变化才 bump。
    bool changed = false;
    if (hasLoudness)
    {
        changed |= loudnessMode != rt.loudnessMode;
        rt.loudnessMode = loudnessMode;
    }
    if (hasCenter)
    {
        changed |= centerSlotPolicy != rt.centerSlotPolicy;
        rt.centerSlotPolicy = centerSlotPolicy;
    }
    if (changed)
        ++rt.configSeq; // PR#55 缺陷4
    c(okResp());
}

void OutputEditor::handleEditSegment(const ArgList& a, Completion c)
{
    const int ch = a.size() > 0 ? static_cast<int>(a[0]) : 0;
    const juce::String op = a.size() > 1 ? a[1].toString() : juce::String();
    const juce::var payload = a.size() > 2 ? a[2] : juce::var();

    if (ch < 1 || ch > 15)
    {
        c(badArgResp());
        return;
    }
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }

    const double sr = processor_.sampleRate();
    scvb::state::SegmentEditArgs args;
    bool valid = true;

    if (op == "move_boundary")
    {
        args.op = scvb::state::SegmentEditOp::MoveBoundary;
        args.segIdx = payload.isObject() ? static_cast<int>(payload.getProperty("segIdx", -1)) : -1;
        const juce::String edge =
            payload.isObject() ? payload.getProperty("edge", juce::String()).toString() : juce::String();
        args.edgeIsT0 = (edge == "t0");
        if (edge != "t0" && edge != "t1")
            valid = false;
        args.tSamples =
            payload.isObject()
                ? static_cast<std::int64_t>(std::llround(static_cast<double>(payload.getProperty("tS", 0.0)) * sr))
                : 0;
    }
    else if (op == "split")
    {
        args.op = scvb::state::SegmentEditOp::Split;
        args.segIdx = payload.isObject() ? static_cast<int>(payload.getProperty("segIdx", -1)) : -1;
        args.tSamples =
            payload.isObject()
                ? static_cast<std::int64_t>(std::llround(static_cast<double>(payload.getProperty("tS", 0.0)) * sr))
                : 0;
    }
    else if (op == "merge")
    {
        args.op = scvb::state::SegmentEditOp::Merge;
        args.segIdx = payload.isObject() ? static_cast<int>(payload.getProperty("segIdxA", -1)) : -1;
        args.segIdxB = payload.isObject() ? static_cast<int>(payload.getProperty("segIdxB", -1)) : -1;
    }
    else if (op == "set_values")
    {
        args.op = scvb::state::SegmentEditOp::SetValues;
        args.segIdx = payload.isObject() ? static_cast<int>(payload.getProperty("segIdx", -1)) : -1;
        if (payload.hasProperty("pan"))
        {
            args.hasPan = true;
            args.pan = juce::jlimit(-100.0f, 100.0f, static_cast<float>(payload.getProperty("pan", 0.0)));
        }
        if (payload.hasProperty("volDb"))
        {
            args.hasVol = true;
            args.volDb = juce::jlimit(-24.0f, 12.0f, static_cast<float>(payload.getProperty("volDb", 0.0)));
        }
        if (!args.hasPan && !args.hasVol)
            valid = false;
    }
    else if (op == "set_locked")
    {
        args.op = scvb::state::SegmentEditOp::SetLocked;
        args.segIdx = payload.isObject() ? static_cast<int>(payload.getProperty("segIdx", -1)) : -1;
        bool lockedVal = false;
        if (!payload.isObject() || !strictBool(payload.getProperty("locked", false), lockedVal))
            valid = false;
        args.locked = lockedVal;
    }
    else
    {
        valid = false;
    }

    if (!valid)
    {
        c(badArgResp());
        return;
    }

    const int track = ch - 1;
    // 持锁 + 结果门控事务(PR#55 重要1/缺陷3)。
    const scvb::state::SegmentEditResult result = processor_.editSegment(track, args);

    if (result != scvb::state::SegmentEditResult::Ok)
    {
        if (result == scvb::state::SegmentEditResult::NotAdjacent)
        {
            juce::var o = obj();
            put(o, "ok", false);
            put(o, "reason", "notAdjacent");
            c(o);
        }
        else
        {
            c(badArgResp());
        }
        return;
    }

    emitSegments("edit", false);
    c(okResp());
}

void OutputEditor::handleRecaptureArm(const ArgList& a, Completion c)
{
    const int tracksMask = a.size() > 0 ? static_cast<int>(a[0]) : 0;
    const double startS = a.size() > 1 ? static_cast<double>(a[1]) : 0.0;
    const double endS = a.size() > 2 ? static_cast<double>(a[2]) : 0.0;
    bool autoStop = false;
    if (a.size() > 3 && !strictBool(a[3], autoStop))
    {
        c(badArgResp());
        return;
    }

    auto& rt = processor_.runtime();
    juce::var o = obj();

    // tracksMask=0 且 startS=endS=0 → 撤销布防。
    if (tracksMask == 0 && startS == 0.0 && endS == 0.0)
    {
        rt.recaptureArmed = false;
        rt.recaptureTracksMask = 0;
        put(o, "armed", false);
        put(o, "tracksMask", 0);
        put(o, "startS", 0.0);
        put(o, "endS", 0.0);
        c(o);
        return;
    }

    const char* reason = nullptr;
    if (tracksMask == 0)
        reason = "noTracks";
    else if (startS >= endS)
        reason = "noSelection";
    else if (isReadOnly())
        reason = "readOnly";

    if (reason != nullptr)
    {
        rt.recaptureArmed = false;
        put(o, "armed", false);
        put(o, "tracksMask", tracksMask);
        put(o, "startS", startS);
        put(o, "endS", endS);
        put(o, "reason", reason);
        c(o);
        return;
    }

    rt.recaptureArmed = true;
    rt.recaptureTracksMask = static_cast<std::uint16_t>(tracksMask & 0x7FFF);
    rt.recaptureStartS = startS;
    rt.recaptureEndS = endS;
    rt.recaptureAutoStop = autoStop;
    put(o, "armed", true);
    put(o, "tracksMask", tracksMask);
    put(o, "startS", startS);
    put(o, "endS", endS);
    c(o);
}

void OutputEditor::handleClearCoverage(const ArgList& a, Completion c)
{
    const int tracksMask = a.size() > 0 ? static_cast<int>(a[0]) : 0;
    const double startS = a.size() > 1 ? static_cast<double>(a[1]) : 0.0;
    const double endS = a.size() > 2 ? static_cast<double>(a[2]) : 0.0;
    if (tracksMask == 0 || startS >= endS)
    {
        c(badArgResp());
        return;
    }
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }
    // T29 无采集特征数据源(FrameStore 接线归 T21/T33)→ clearedS=0。
    juce::var o = obj();
    put(o, "ok", true);
    put(o, "clearedS", 0.0);
    c(o);
}

void OutputEditor::handleUndo(const ArgList& /*a*/, Completion c)
{
    if (isReadOnly()) // observer 只读实例不得改本地 CRVS/UndoManager(PR#55 第4轮缺陷1)
    {
        c(observerResp());
        return;
    }
    const bool ok = processor_.undo(); // 持锁(PR#55 重要1)
    if (ok)
        emitSegments("undo", true); // 全量段表(PR#55 建议①)
    juce::var o = obj();
    put(o, "ok", ok);
    c(o);
}

void OutputEditor::handleRedo(const ArgList& /*a*/, Completion c)
{
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }
    const bool ok = processor_.redo(); // 持锁(PR#55 重要1)
    if (ok)
        emitSegments("redo", true); // 全量段表(PR#55 建议①)
    juce::var o = obj();
    put(o, "ok", ok);
    c(o);
}

void OutputEditor::handleRequestWaveform(const ArgList& a, Completion c)
{
    const int ch = a.size() > 0 ? static_cast<int>(a[0]) : 0;
    const double startS = a.size() > 1 ? static_cast<double>(a[1]) : 0.0;
    const double endS = a.size() > 2 ? static_cast<double>(a[2]) : 0.0;
    const int cols = a.size() > 3 ? static_cast<int>(a[3]) : 0;
    if (ch < 1 || ch > 15 || cols < 1 || cols > 4096 || startS >= endS)
    {
        c(badArgResp());
        return;
    }

    // §1.27:拉取式 request/response,一次调用一次 resolve(绝不进事件流)。
    // T29 无特征数据源(FrameStore/feat 环接线归 T21/T33)→ 未覆盖列:covered=0,minDb=maxDb=-160 哨兵。
    juce::var minDb = mkArray();
    juce::var maxDb = mkArray();
    juce::var vad = mkArray();
    juce::var covered = mkArray();
    juce::var stale = mkArray();
    juce::var passId = mkArray();
    for (int i = 0; i < cols; ++i)
    {
        push(minDb, -160.0);
        push(maxDb, -160.0);
        push(vad, 0);
        push(covered, 0);
        push(stale, 0);
        push(passId, 0);
    }
    juce::var valleys = mkArray();

    juce::var o = obj();
    put(o, "minDb", minDb);
    put(o, "maxDb", maxDb);
    put(o, "vad", vad);
    put(o, "covered", covered);
    put(o, "stale", stale);
    put(o, "passId", passId);
    put(o, "valleys", valleys);
    c(o);
}

void OutputEditor::handleSetActiveTab(const ArgList& a, Completion c)
{
    const juce::String tab = a.size() > 0 ? a[0].toString() : juce::String();
    if (tab != "master" && tab != "tracks" && tab != "wave" && tab != "settings")
    {
        c(badArgResp());
        return;
    }
    processor_.runtime().activeTab = tab;
    c(okResp());
}

void OutputEditor::handleSetGuideSeen(const ArgList& a, Completion c)
{
    bool seen = false;
    if (a.size() < 1 || !strictBool(a[0], seen))
    {
        c(badArgResp());
        return;
    }
    processor_.runtime().guideSeen = seen;
    // alsoGlobal 落盘全局默认归 T31(commitUiScale 通道)。
    c(okResp());
}

void OutputEditor::handleSetTourSeen(const ArgList& a, Completion c)
{
    bool seen = false;
    if (a.size() < 1 || !strictBool(a[0], seen))
    {
        c(badArgResp());
        return;
    }
    processor_.runtime().tourSeen = seen;
    c(okResp());
}

void OutputEditor::handleConfirmPrintGuard(const ArgList& /*a*/, Completion c)
{
    processor_.runtime().printGuardPending = false; // 幂等(§1.34)
    c(okResp());
}

} // namespace scvb::output
