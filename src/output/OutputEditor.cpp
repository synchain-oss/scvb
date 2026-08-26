// SPDX-License-Identifier: GPL-3.0-or-later
#include "OutputEditor.h"

#include "AnalyzeScopeMath.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include <ScvbOutputWebData.h> // juce_add_binary_data 生成:嵌入的 web/output UI 资源

#include "BridgeArgs.h"
#include "SegmentEditService.h"
#include "UiDefaultsStore.h"
#include "engine/CurveEvaluator.h"
#include "state/SegmentEdit.h"
#include "state/StateCodec.h"

namespace scvb::output
{

namespace
{

using namespace scvb::outputbridge;

// 全轨位图(u16,bit0=ch1…bit14=ch15)。增量段事件用单轨位;snapshot/versionActive/copyVersion/undo/redo 用全轨。
constexpr std::uint16_t kAllTracksMask = 0x7FFF;

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
    : scvb::webview::WebViewHost(
          processor,
          [&] {
              scvb::webview::WebViewHost::Config config;
              config.role = "output";
              config.userDataFolderName = "scvb-output-webview";
              config.version = "0.1.0"; // 项目版本(CMake project VERSION)
              config.lang = processor.uiLanguage().toStdString();
              config.uiScale = static_cast<float>(processor.uiScalePercent()) / 100.0f;
              config.channelLimit = 15;
              // 嵌入的 web/output UI 资源(cmake/ScvbWebAssets.cmake ->
              // SCVBOutputWebAssets)。传空 Source 会让 resource provider
              // 恒 nullopt = 空白窗口 + 看门狗超时。
              config.resourceSource = {ScvbOutputWebData::namedResourceListSize, ScvbOutputWebData::originalFilenames,
                                       ScvbOutputWebData::namedResourceList, &ScvbOutputWebData::getNamedResource};
              config.augmentOptions = [this](juce::WebBrowserComponent::Options& o) { registerNativeFunctions(o); };
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
    // 系统级全局默认(跨工程,UiDefaultsStore 落盘;硬编码 false 时「不再显示」永不生效 —— T37 A-3)
    put(o, "guide_seen_global", uidefaults::guideSeenGlobal());
    put(o, "tour_seen_global", uidefaults::tourSeenGlobal());
    // §1.1 附加位:用户显式选过语言的系统级全局默认(新工程不再重复问语言)。
    put(o, "lang_chosen_global", uidefaults::langChosenGlobal());
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

    syncDawLoopRange(); // daw_loop 档:先把 range 跟到宿主循环区,再让 emitState 下发
    emitState(first);
    emitParams(first);
    if (first || (tickCount_ % 6 == 0))
        emitConn(); // ~4Hz(25Hz 6 分频)
    if (first || (tickCount_ % 25 == 0))
        emitGroups(); // 1Hz(25Hz 25 分频)
    emitMeters(); // 25Hz + 0.3dB 阈值
    emitPlayhead(); // 25Hz + diff
    if (tickCount_ % 12 == 0)
        emitCaptureProgress(); // ~2Hz(25Hz 12 分频);内部再判「仅播放中发」(§2.7)

    // 段表快照:首帧必发;sample rate 变化(含宿主 prepareToPlay 前后)或 CRVS 修订号变化(加载工程/
    // 预设后 setStateInformation 替换段真身)必重发 —— 否则旧时间/旧段表残留到下一次段编辑/undo/切版本
    // (PR#55 第7轮缺陷1 / 第8轮缺陷1)。
    const double srNow = processor_.sampleRate();
    const std::uint32_t crvsRev = processor_.crvsRevision();
    // 分析刚完成时 reason 必须是 "analyze" 而不是 "snapshot"(§2.8):web 有两处认它 ——
    // Tab4 的「参数已改、结果陈旧」基线同步,与 Tab3 的分析 diff 摘要条 + 倒计时撤条。
    // 落成 snapshot 会让这两处静默失效(分析完了标记还挂着、摘要条不出)。
    const bool analyzed = processor_.takeAnalysisDone();
    if (first || analyzed || (srNow > 0.0 && !juce::approximatelyEqual(srNow, lastSegmentsSampleRate_)) ||
        crvsRev != lastCrvsRevision_)
    {
        lastSegmentsSampleRate_ = srNow;
        lastCrvsRevision_ = crvsRev;
        emitSegments(analyzed ? "analyze" : "snapshot", kAllTracksMask);
    }

    // scvb.error:仅条件成立时发(§2.9),T29 无触发面。
}

// ============================================================================
// 事件发射
// ============================================================================
bool OutputEditor::emitIfChanged(const char* eventName, const juce::var& payload, juce::String& lastJson)
{
    const juce::String json = juce::JSON::toString(payload);
    if (json == lastJson)
    {
        return false;
    }
    // 不可见时 emitEventIfBrowserIsVisible 会把载荷丢掉 —— 此时**不推进基线**,否则这一份变化
    // 被永久吞掉:恢复可见后 json 仍等于陈旧的 lastJson,除非底层值再变一次,事件不会重发。
    // 停走带时 scvb.playhead 的载荷逐字节稳定,于是「关一次面板」= 播放头竖线再也不出现。
    if (!webView().isVisible())
    {
        return false;
    }
    lastJson = json;
    webView().emitEventIfBrowserIsVisible(eventName, payload);
    return true;
}

void OutputEditor::emitState(bool forceFull)
{
    (void)forceFull; // T29 增量子树未实现:一律 full:true 全量下发(UI 全量替换)。
    juce::var payload = buildStateSubtree(true);
    put(payload, "full", true);

    emitIfChanged(Event::State, payload, lastStateJson_);
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
    // §2.2 hostEcho:此刻车道是否正被**宿主自动化**驱动(打印器的 layer-1b listener 记的时刻)。
    // 此前恒 false —— 于是「宿主在 Read 档回写参数、把用户的手动改动盖掉」这件事在 UI 上
    // 完全不可见,用户只看到「调了没反应」(v5.1 实测 P1-D)。优先级本身是设计(J78 的
    // 优先级表),要修的是**看不见**。
    put(payload, "hostEcho", processor_.getPrinter().hostEchoActive());
    put(payload, "full", forceFull);
    put(payload, "versionActive", v);

    emitIfChanged(Event::Params, payload, lastParamsJson_);
}

void OutputEditor::emitConn()
{
    // 注:heartbeatAgeMs 自 T37 起是**活数**(每拍都变),故下面这道 diff 门在有 Input 连着
    // 时恒真 —— scvb.conn 变成稳定的 ~4Hz 全量下发。这正是 §2.3 给本事件定的频率(emitTick
    // 的 6 分频),Tab4 诊断区的「每轨 heartbeat 年龄」也要靠这个活数;不为了让 diff 门重新
    // 生效而把 age 排除出比对(那等于把诊断区冻住)。全空闲(无 Input)时载荷回到全哨兵、
    // 门重新拦住,不会有空转下发。
    //
    // web 侧后果已核过:app.js 的 scvb.conn 处理器会 requestRender() 整页重投影,于是有
    // Input 连着时常驻 ~4Hz 整页 render。render 是幂等纯投影,且正在编辑的控件有明确豁免
    // (tab-tracks.js 的 `if (local.editingCh !== ch)` 跳过该行 label;输入框的 value 只在
    // beginLabelEdit 里写、render 从不碰),故编辑态不会掉焦点、也不会被抹半截。
    // 4Hz 也只有 §2.5 meters 那条 30Hz 路径的 1/7.5。
    juce::var payload = buildConnPayload();
    emitIfChanged(Event::Conn, payload, lastConnJson_);
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
    // 数据面 = 音频线程发布的 MeterShot 线性快照(post-gain/pre-pan 轨道电平 + 替换后的总线)。
    // dB 换算刻意留在这里:[A] 只做乘加,25Hz 的 [M] 才做 32 次 log10。
    // 0.3 dB 阈值(契约 §0.4-2):任一轨/总线变化 ≥ 阈值才发,首帧必发。
    constexpr float kFloorDb = -60.0f;
    constexpr float kMeterThresholdDb = 0.3f;

    // 线性幅度 → dBFS,并钳到地板。0(静音/无数据)→ 地板,不产生 -inf。
    const auto toDb = [kFloorDb](float lin) {
        if (!(lin > 0.0f))
            return kFloorDb;
        const float db = 20.0f * std::log10(lin);
        return db < kFloorDb ? kFloorDb : db;
    };

    const scvb::output::MeterPod pod = processor_.meterSnapshot();

    std::array<float, 15> db;
    std::array<float, 15> peak;
    for (int t = 0; t < 15; ++t)
    {
        db[static_cast<std::size_t>(t)] = toDb(pod.trackRms[t]);
        peak[static_cast<std::size_t>(t)] = toDb(pod.trackPeak[t]);
    }
    const float busL = toDb(pod.busRms[0]);
    const float busR = toDb(pod.busRms[1]);
    const float busLPeak = toDb(pod.busPeak[0]);
    const float busRPeak = toDb(pod.busPeak[1]);

    bool changed = !metersEverSent_;
    for (int t = 0; t < 15 && !changed; ++t)
        changed = std::fabs(db[static_cast<std::size_t>(t)] - lastMeterDb_[static_cast<std::size_t>(t)]) >=
                      kMeterThresholdDb ||
                  std::fabs(peak[static_cast<std::size_t>(t)] - lastMeterPeak_[static_cast<std::size_t>(t)]) >=
                      kMeterThresholdDb;
    if (!changed)
        changed = std::fabs(busL - lastBusL_) >= kMeterThresholdDb ||
                  std::fabs(busR - lastBusR_) >= kMeterThresholdDb ||
                  std::fabs(busLPeak - lastBusLPeak_) >= kMeterThresholdDb ||
                  std::fabs(busRPeak - lastBusRPeak_) >= kMeterThresholdDb;

    if (!changed)
        return;

    metersEverSent_ = true;
    lastMeterDb_ = db;
    lastMeterPeak_ = peak;
    lastBusL_ = busL;
    lastBusR_ = busR;
    lastBusLPeak_ = busLPeak;
    lastBusRPeak_ = busRPeak;

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
    put(l, "peakDb", static_cast<double>(busLPeak));
    juce::var r = obj();
    put(r, "db", static_cast<double>(busR));
    put(r, "peakDb", static_cast<double>(busRPeak));
    put(bus, "l", l);
    put(bus, "r", r);

    juce::var payload = obj();
    put(payload, "tracks", tracks);
    put(payload, "bus", bus);
    webView().emitEventIfBrowserIsVisible(Event::Meters, payload);
}

bool OutputEditor::hostLoopSeconds(double& startS, double& endS) const
{
    const scvb::engine::PlayheadPod pod = processor_.playheadSnapshot();
    // JUCE 的 AudioPlayHead::LoopPoints 只有 ppqStart/ppqEnd —— 换算成秒必须有 tempo。
    // 宿主给了 cycle 却没给 bpm(或 bpm<=0)时不猜:按「无循环区」处理,UI 走 loopMissing 档。
    if ((pod.flags & scvb::engine::kPlayheadCycleValid) == 0)
        return false;
    if ((pod.flags & scvb::engine::kPlayheadTempoValid) == 0 || pod.bpm <= 0.0)
        return false;

    const double secPerBeat = 60.0 / pod.bpm;
    const double s = pod.loopStartPpq * secPerBeat;
    const double e = pod.loopEndPpq * secPerBeat;
    if (!(e > s) || !std::isfinite(s) || !std::isfinite(e))
        return false; // 空/倒置循环区不算有效

    startS = s;
    endS = e;
    return true;
}

bool OutputEditor::syncDawLoopRange()
{
    auto& rt = processor_.runtime();
    if (rt.rangeMode != 1) // 仅 daw_loop 档跟随
        return false;

    double s = 0.0;
    double e = 0.0;
    if (!hostLoopSeconds(s, e))
        return false; // 循环区暂时读不到 → 沿用上次范围(UI 据 playhead 缺字段显示「已失效」)

    if (juce::approximatelyEqual(rt.rangeStartS, s) && juce::approximatelyEqual(rt.rangeEndS, e))
        return false;

    rt.rangeStartS = s;
    rt.rangeEndS = e;
    // **不** bump config_seq:§4.3 定义的是「**广播区**任一字段变化 +1」,而 range 不在广播区里。
    // 这里是 25Hz 的自动跟随路径 —— daw_loop 档遇上 tempo 自动化时 range 每拍都在变,bump 会让
    // 广播区 25Hz 空转重写 2KB、scvb.config 也跟着 25Hz 空转下发。range 本身经 scvb.state 下推,
    // 那条链有自己的 JSON diff 门,不依赖 config_seq。
    return true;
}

void OutputEditor::emitPlayhead()
{
    // 读音频线程发布的 playheadShot_ SPSC 快照(PR#55 建议①;不直读宿主 AudioPlayHead)。
    const scvb::engine::PlayheadPod pod = processor_.playheadSnapshot();
    const bool playing = (pod.flags & scvb::engine::kPlayheadIsPlaying) != 0;
    const double timeS = pod.timeSamples >= 0 ? samplesToSeconds(pod.timeSamples, processor_.sampleRate()) : 0.0;

    // inRange(§2.6):mode=follow 恒 true;否则落在 [startS, endS)。
    const auto& rt = processor_.runtime();
    bool inRange = true;
    if (rt.rangeMode != 0)
        inRange = timeS >= rt.rangeStartS && timeS < rt.rangeEndS;

    juce::var payload = obj();
    put(payload, "timeS", timeS);
    put(payload, "isPlaying", playing);
    put(payload, "inRange", inRange);
    // loopStartS/loopEndS(§2.6):仅宿主提供且 kCycleValid + 可换算时出现;缺失即字段不存在
    // (不发哨兵值)。UI 靠「daw_loop 档 ∧ 本事件无这两字段」判定循环区已失效。
    double loopStartS = 0.0;
    double loopEndS = 0.0;
    if (hostLoopSeconds(loopStartS, loopEndS))
    {
        put(payload, "loopStartS", loopStartS);
        put(payload, "loopEndS", loopEndS);
    }

    emitIfChanged(Event::Playhead, payload, lastPlayheadJson_);
}

void OutputEditor::emitCaptureProgress()
{
    // §2.7:播放中 2Hz;非播放不发。数据源 = FrameStore 的 coverage 记账
    // (Input 写 feat 段 → OutputSession 25Hz 增量拉取 → CoverageMap)。
    // 只读观察实例(O3)覆盖率恒 0 且**这是有意的**:OutputSession::tick 对 observer 早退,
    // 不 attach feat 段也不 pullFeatures —— 采集与分析的真源归本组那个 kActive 的主 Output,
    // 观察实例不该另存一份特征真身,也不该跟主实例抢着拉同一批 hop。
    const scvb::engine::PlayheadPod pod = processor_.playheadSnapshot();
    if ((pod.flags & scvb::engine::kPlayheadIsPlaying) == 0)
    {
        return;
    }

    // 覆盖率的分母(§2.7 字段纪律):global.range;follow 档取「全时间线已分析域」——
    // 用当前播放位置作为已知时间线末端,否则分母是无穷大、覆盖率恒 0。
    const auto& rt = processor_.runtime();
    const double timeS = pod.timeSamples >= 0 ? samplesToSeconds(pod.timeSamples, processor_.sampleRate()) : 0.0;
    double startS = 0.0;
    double endS = timeS;
    if (rt.rangeMode != 0)
    {
        startS = rt.rangeStartS;
        endS = rt.rangeEndS;
    }
    if (!(endS > startS))
    {
        return; // 时间线还没走出一个 hop:没有可报的覆盖
    }

    juce::var channels = mkArray();
    bool any = false;
    for (int t = 0; t < 15; ++t)
    {
        const int ch = t + 1;
        const auto info = processor_.coverageOf(ch, startS, endS);
        const std::size_t idx = static_cast<std::size_t>(t);

        // addedRanges = 本帧相对上一帧**新增**的覆盖区间(§2.7「增量」)。用 CoverageMap 自己的
        // add/punch 做差集:全量并进去,再把上一帧已报过的打洞打掉,剩下的就是新增。
        scvb::analysis::CoverageMap added;
        for (const auto& r : info.ranges)
            added.add(r);
        for (const auto& r : lastCoverageRanges_[idx])
            added.punch(r);

        const bool pctChanged = !juce::approximatelyEqual(info.pct, lastCoveragePct_[idx]);
        if (added.empty() && !pctChanged)
        {
            continue; // §2.7:仅包含本帧有变化的轨
        }
        lastCoverageRanges_[idx] = info.ranges;
        lastCoveragePct_[idx] = info.pct;

        const double hopS = ScvbOutputAudioProcessor::featHopSeconds();
        juce::var addedArr = mkArray();
        for (const auto& r : added.ranges())
        {
            juce::var seg = obj();
            put(seg, "startS", static_cast<double>(r.begin) * hopS);
            put(seg, "endS", static_cast<double>(r.end) * hopS);
            push(addedArr, seg);
        }

        juce::var c = obj();
        put(c, "ch", ch);
        put(c, "addedRanges", addedArr);
        put(c, "coveragePct", static_cast<double>(info.pct));
        push(channels, c);
        any = true;
    }

    if (!any)
    {
        return; // 无变化不发(§0.4 值未变不发)
    }

    juce::var payload = obj();
    put(payload, "channels", channels);
    webView().emitEventIfBrowserIsVisible(Event::CaptureProgress, payload);
}

void OutputEditor::emitSegments(const juce::String& reason, std::uint16_t tracksMask)
{
    webView().emitEventIfBrowserIsVisible(Event::Segments, buildSegmentsPayload(reason, tracksMask));
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
    // 注:本字段(scvb.state.config_seq,0 起)与 ctrl 广播区的 config_seq(1 起)差一个固定偏移 ——
    // 广播区把 0 留给「本组没有 Output 在广播」这一态(见 publishConfigBroadcast)。两者都是
    // 单调计数器、只用于「变没变」的比较,不参与跨端相等判定,故不强行统一,只在此注明。
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
    // [J69/U24] 持锁快照读(复评重要②):loudnessMode/centerSlotPolicy 由 setAnalysisConfig 持锁写、
    // getStateInformation 持锁读,emitState 必须同锁读(经 analysisConfigSnapshot)消除剩余竞态。
    const auto analysisCfg = processor_.analysisConfigSnapshot();
    put(analysis, "loudness_mode", analysisCfg.first);
    put(analysis, "center_slot_policy", analysisCfg.second);
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
        const bool participate = c.participatesInAutoPan();
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
    put(ui, "master_chart_mode", processor_.masterChartMode());
    // 两位是 atomic(宿主线程的 setStateInformation 会写,本函数在消息线程 25Hz 读):
    // 陈旧一帧无害,撕裂才有害 —— 故取 atomic 而不是让 25Hz 的 emit 去抢 lifecycleMutex_。
    put(ui, "guide_seen", rt.guideSeen.load(std::memory_order_relaxed));
    put(ui, "tour_seen", rt.tourSeen.load(std::memory_order_relaxed));
    // 首启语言卡的抑制位(§1.30 setLang 被显式调用过即为真;随 PRMS 持久化)。
    put(ui, "lang_chosen", rt.langChosen.load(std::memory_order_relaxed));
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
        put(analysisRun, "progress", rt.analysisProgress.load(std::memory_order_relaxed));
    put(o, "analysis_run", analysisRun);

    return o;
}

juce::var OutputEditor::buildConnPayload() const
{
    // 数据源 = 本组 registry 的 15 条 InputSlot 实况(§2.3 字段纪律逐条对位)。
    const auto snap = processor_.connSnapshot();

    juce::var channels = mkArray();
    for (int t = 0; t < 15; ++t)
    {
        const auto& info = snap.channels[static_cast<std::size_t>(t)];
        juce::var ch = obj();
        put(ch, "slotState", static_cast<int>(info.slotState));
        put(ch, "heartbeatAgeMs", static_cast<juce::int64>(info.heartbeatAgeMs));
        // §2.3:heartbeatFresh 是 heartbeatAgeMs ≤ 2000 的派生布尔(哨兵 0xFFFFFFFF 自然为 false)。
        put(ch, "heartbeatFresh", info.heartbeatAgeMs <= static_cast<std::uint32_t>(scvb::kStaleDisplayMs));
        put(ch, "capturing", info.capturing);
        // 本次失准发作内的缺口数(非进程累计):恢复健康 1s 后归零,横幅/行内 ⚠ 随之撤下。
        // **只数真失准**(走带推进中的时间线缺口);写方停着走 suspended,见下。
        put(ch, "misalignCount", static_cast<int>(processor_.misalignCount(t + 1)));
        put(ch, "srMismatch", info.srMismatch);
        // 该轨的写方停着(宿主在无信号段挂起 Input / 用户 bypass / 轨未激活)。
        // 与 misalignCount 是**两件事**:这条是中性状态,UI 用灰蓝提示而不是红色 ⚠ ——
        // 乐句间隙里宿主挂起 Input 属于正常现象,用户自己 bypass 的更不需要警报。
        put(ch, "suspended", info.suspended);
        push(channels, ch);
    }

    juce::var o = obj();
    put(o, "channels", channels);
    put(o, "outputReadOnly", snap.readOnly);
    // generation 是 u32,和 heartbeatAgeMs 同样用 int64 承载:int 在 >2^31 时会翻负,
    // 而这是个只增不减的重初始化计数器。
    put(o, "generation", static_cast<juce::int64>(snap.generation));
    return o;
}

juce::var OutputEditor::buildSegmentsPayload(const juce::String& reason, std::uint16_t tracksMask) const
{
    const int v = processor_.versionActive();
    const scvb::state::CrvsData crvs = processor_.crvsSnapshot(); // 持锁快照(PR#55 重要1)
    const auto& vc = crvs.versions[static_cast<std::size_t>(v - 1)];
    const double sr = processor_.sampleRate();

    // 「已知时间线末端」——无末端段(t1 = 1<<40 哨兵)的右端降级取它。
    // 三级取值(①全轨非哨兵最大末端 → ②采集覆盖 → ③t0+最小非零宽度)的实现在
    // `BridgeArgs.h` 的降级链三函数里 —— 与 harness 的 HOST R4 用例共用同一份代码,
    // 用例断的就是这里真实上桥的值(v5.3 R4)。
    const std::int64_t knownEndSamples =
        scvb::output::knownTimelineEndSamples(vc, processor_.capturedExtentSeconds(), sr);
    const std::int64_t minSpanSamples =
        scvb::output::minOpenEndedSpanSamples(ScvbOutputAudioProcessor::featHopSeconds(), sr);

    juce::var channels = mkArray();
    for (int t = 0; t < 15; ++t)
    {
        if ((tracksMask & (1u << t)) == 0)
            continue; // 只含掩码内轨(PR#55 第11轮缺陷2)
        const auto& segments = vc.tracks[static_cast<std::size_t>(t)].segments;

        juce::var ch = obj();
        put(ch, "ch", t + 1);
        juce::var segArr = mkArray();
        for (std::size_t i = 0; i < segments.size(); ++i)
        {
            const auto& s = segments[i];
            juce::var seg = obj();
            put(seg, "segIdx", static_cast<int>(i));
            put(seg, "t0S", samplesToSeconds(s.t0, sr)); // 安全换算(PR#55 第6轮缺陷1)
            // 「无末端」段(setTrackManual 的常值段:t1 = 1<<40 哨兵,真末端由宿主时间线提供)
            // **在这里按语义降级**,而不是让哨兵以「2290 万秒」的伪装流到前端 —— 那个数曾被
            // durationOf 当成工程时长选中,再当作 requestWaveform 的 endS 发回来,把消息线程
            // 跑死(P0-A)。#89 在 viz 侧已按同一口径处理过(无末端只取 t0)。
            // 降级目标 = **已知时间线末端**:优先取本次快照里其余段的最大真末端,没有则回落
            // 到该段自己的 t0(段仍然存在、可点、可切,只是右端不再撒谎)。
            const bool openEnded = s.t1 >= scvb::output::kOpenEndedT1;
            // ⚠ HOST R4 用例守的是降级链三函数本身,守不到「这里还在调它」这一跳
            // (harness 编不进本 TU)—— 改动下面这一行(绕开 effectiveT1Samples 或
            // 换回裸 s.t1)必须同步 tests/host 的 HOST R4 用例,否则测试照绿而 bug 回归。
            const std::int64_t t1Effective =
                scvb::output::effectiveT1Samples(s.t0, s.t1, knownEndSamples, minSpanSamples);
            put(seg, "t1S", samplesToSeconds(t1Effective, sr));
            put(seg, "openEnded", openEnded); // §2.8:UI 据此知道右端是「到末端」而不是一个真时刻
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
    add(Fn::SetMasterChartMode, &OutputEditor::handleSetMasterChartMode);
    add(Fn::SetGuideSeen, &OutputEditor::handleSetGuideSeen);
    add(Fn::SetTourSeen, &OutputEditor::handleSetTourSeen);
    add(Fn::ConfirmPrintGuard, &OutputEditor::handleConfirmPrintGuard);
}

// ============================================================================
// 通用函数覆写(基类只维护 editor 局部值;Output 桥面下发的 ui.* 真源在 processor)
// ============================================================================
void OutputEditor::handleSetLang(const juce::Array<juce::var>& args,
                                 juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    WebViewHost::handleSetLang(args, std::move(complete)); // 归一化 {zh,en,fr} + 回执 {ok:true}
    processor_.bridgeSetUiLanguage(lang()); // §1.30:落 Output state(实际生效值经 scvb.state 回推)
    // 显式选过语言 → 同时写系统级全局默认,新工程也不再问(与 guide/tour 的 alsoGlobal 同口径)。
    // **两件都要写**:只写「选过」而不写「选的是哪个」,移除插件再加载时全局位挡住了语言
    // 起始卡、语言却回落到出厂的 en —— 用户既回不到中文也没有再选一次的入口(v5 实测 P1-6)。
    uidefaults::setLangChosenGlobal(true);
    uidefaults::setLangGlobal(lang());
}

void OutputEditor::persistUiScaleAsDefault()
{
    // §1.29「保持」= 落工程 state + 系统级全局默认(新工程沿用;05 §1.2)。
    const int percent = juce::roundToInt(uiScale() * 100.0f);
    processor_.bridgeSetUiScalePercent(percent);
    uidefaults::setUiScalePercent(percent);
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

// analyze/previewAnalyze 的作用域参数(§1.5/§1.6):"all" = 全时间线全轨;
// 对象形 {tracksMask,startS,endS} = 指定轨 × 指定范围(web 的 analyzeScope 产出)。
// follow 档取**整条已采集时间线**(capturedExtentSeconds),与播放头无关 —— v5.1 P1-F:
// 原先取「当前播放位置」,用户 Cubase 设了「播完回开头」时播放头回 0,范围恒空、分析永远受理不了。
OutputEditor::AnalyzeScope OutputEditor::parseAnalyzeScope(const ArgList& a) const
{
    AnalyzeScope s;
    const auto& rt = processor_.runtime();

    if (a.size() > 0 && a[0].isObject())
    {
        s.tracksMask = static_cast<std::uint16_t>(static_cast<int>(a[0].getProperty("tracksMask", 0)) & 0x7FFF);
        s.startS = static_cast<double>(a[0].getProperty("startS", 0.0));
        s.endS = static_cast<double>(a[0].getProperty("endS", 0.0));
        return s;
    }

    // "all" 或缺参:范围推导是纯函数(AnalyzeScopeMath.h),这里只负责取参数。
    // 抽出去的理由见那个头文件:它是 P1-F 的唯一修复点,埋在私有成员里 harness 够不着,
    // 回归用例只能绕开它 —— 改回旧写法照样绿(评审 I1)。
    s.tracksMask = 0; // 0 = 不限轨
    const AnalyzeRange r =
        analyzeAllRange(rt.rangeMode, rt.rangeStartS, rt.rangeEndS, processor_.capturedExtentSeconds());
    s.startS = r.startS;
    s.endS = r.endS;
    return s;
}

void OutputEditor::handlePreviewAnalyze(const ArgList& a, Completion c)
{
    // 纯只读 dry-run(§1.5):范围 ∩ 覆盖的轨数 + 会被保留的用户段数,不改任何数据。
    const AnalyzeScope sc = parseAnalyzeScope(a);
    const auto info = processor_.previewAnalysis(sc.tracksMask, sc.startS, sc.endS);
    juce::var o = obj();
    put(o, "intervals", info.intervals);
    put(o, "tracks", info.tracks);
    put(o, "manualKept", info.manualKept);
    c(o);
}

void OutputEditor::handleAnalyze(const ArgList& a, Completion c)
{
    // §1.6:受理回执 + 影响面,立即 resolve —— 真正的分析在**后台线程**跑,
    // 进度经 scvb.state.analysis_run 回推,结果经 scvb.segments 回推。
    //
    // 此前这里是 T29 占位:回 {ok:true, affected:{0,0,0}} 却**从不置 analysis_run.running**。
    // 而 web 拿到 ok 之后要等 running 翻真才把状态交回 state 驱动,于是它的在途标志永远挂着,
    // 「分析中」转到天荒地老也不出结果(v4 实测 P0-1)。
    if (isReadOnly())
    {
        c(observerResp());
        return;
    }

    const AnalyzeScope sc = parseAnalyzeScope(a);
    // §1.6 的第二参 opts:{clearManual:bool}。此前 a[1] 从没被读过 —— 而 web 有两个调用点
    // 专门传它(Tab3「重新识别(含手动段)」带二次确认、Tab2 单轨重新识别),用户点完确认
    // 得到的行为与普通分析逐字节相同,且无任何反馈。
    bool clearManual = false;
    if (a.size() > 1 && a[1].isObject())
    {
        strictBool(a[1].getProperty("clearManual", false), clearManual);
    }

    const auto accepted = processor_.startAnalysis(sc.tracksMask, sc.startS, sc.endS, clearManual);

    if (!accepted.ok)
    {
        juce::var o = obj();
        put(o, "ok", false);
        if (accepted.busy)
        {
            // §5.6 八值闭集 + §7 manifest:analyze 只登记了 "busy" 这一个 reason。
            put(o, "reason", "busy");
        }
        else
        {
            // §1.6 拒绝态行:range ∩ coverage = ∅ → {ok:false, affected:{0,0,0}},**不带 reason**。
            juce::var affected = obj();
            put(affected, "intervals", 0);
            put(affected, "tracks", 0);
            put(affected, "manualKept", 0);
            put(o, "affected", affected);
        }
        c(o);
        return;
    }

    lastStateJson_.clear(); // analysis_run.running 立刻回推,别等下一次 diff
    juce::var affected = obj();
    put(affected, "intervals", accepted.intervals);
    put(affected, "tracks", accepted.tracks);
    put(affected, "manualKept", accepted.manualKept);
    juce::var o = obj();
    put(o, "ok", true);
    put(o, "affected", affected);
    c(o);
}

void OutputEditor::handleCancelAnalyze(const ArgList& /*a*/, Completion c)
{
    // §1.7:进行中才可取消;取消后结果整份丢弃,不碰 CRVS。
    const bool wasRunning = processor_.analysisRunning();
    if (wasRunning)
    {
        processor_.cancelAnalysis();
        lastStateJson_.clear(); // running 立刻回推 false
    }
    juce::var o = obj();
    put(o, "ok", wasRunning);
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
    // 非 const:daw_loop 档的范围真源是宿主循环区,下面会用实测值覆盖调用方传的值。
    double startS = a.size() > 1 ? static_cast<double>(a[1]) : 0.0;
    double endS = a.size() > 2 ? static_cast<double>(a[2]) : 0.0;

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

    // daw_loop 档要求宿主此刻确实提供了可换算的循环区;没有就明确拒绝,UI 据此把该档
    // 置灰并显示「宿主未提供循环区」,而不是切过去之后拿着一个空范围假装成功(§1.8)。
    double loopStartS = 0.0;
    double loopEndS = 0.0;
    if (modeInt == 1 && !hostLoopSeconds(loopStartS, loopEndS))
    {
        juce::var o = obj();
        put(o, "ok", false);
        put(o, "reason", "noLoop");
        c(o);
        return;
    }

    auto& rt = processor_.runtime();
    // daw_loop 的范围真源是宿主循环区,不采信调用方传的 startS/endS。
    if (modeInt == 1)
    {
        startS = loopStartS;
        endS = loopEndS;
    }
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
        emitSegments("versionActive", kAllTracksMask);
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
    if (!processor_.isPrepared()) // 未 prepare → 拒绝(触发 rebuild,PR#55 第7轮缺陷2)
    {
        c(badArgResp());
        return;
    }
    const auto result = processor_.copyVersion(src, dst); // 持锁事务(PR#55 重要1)
    if (result != scvb::engine::CopyVersionResult::Ok)
    {
        c(badArgResp());
        return;
    }

    emitSegments("copyVersion", kAllTracksMask);
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

    if (a.size() < 2 || !a[1].isObject())
    {
        c(badArgResp()); // 必须传对象 patch(PR#55 第8轮缺陷2)
        return;
    }
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

    // 至少一个字段(空对象 patch → badArg,PR#55 第8轮缺陷2)。
    if (!(hasEnabled || hasLabel || hasPriority || hasLeadLock || hasLeadVolExempt || hasParticipate || hasPairId))
    {
        c(badArgResp());
        return;
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
    if (!processor_.isPrepared()) // 未 prepare → 拒绝(触发 rebuild,PR#55 第7轮缺陷2)
    {
        c(badArgResp());
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

    emitSegments("trackManual", static_cast<std::uint16_t>(1u << (ch - 1))); // 仅该轨(PR#55 第11轮缺陷2)
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

    // 缺参/非数组 → badArg,绝不静默清空曲线(PR#55 第5轮缺陷1);空数组 = 整表替换为空(显式清空)。
    if (a.size() < 1 || !a[0].isArray())
    {
        c(badArgResp());
        return;
    }

    std::vector<scvb::PanCurvePoint> points;
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

    // mode 白名单(vad_only / valley),白名单外 badArg(PR#55 第6轮缺陷2)。
    const juce::String mode = p.getProperty("mode", rt.segmentationMode).toString();
    if (!isSegmentationMode(mode))
    {
        c(badArgResp());
        return;
    }
    // sensitivity 0..100、min_segment_ms 50..500(02-dsp-spec §0.3 常量表),越界 clamp。
    const float sens =
        juce::jlimit(0.0f, 100.0f, static_cast<float>(p.getProperty("sensitivity", rt.segmentationSensitivity)));
    const int mms =
        juce::jlimit(50, 500, static_cast<int>(p.getProperty("min_segment_ms", rt.segmentationMinSegmentMs)));

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
    if (!processor_.isPrepared()) // 未 prepare → 拒绝(触发 rebuild,PR#55 第7轮缺陷2)
    {
        c(badArgResp());
        return;
    }
    const float ms = a.size() > 0 ? static_cast<float>(a[0]) : 80.0f;
    // 持锁 + 变化才重建全部曲线(transitionRampSec 烘焙进 CurveEvaluator,PR#55 第5轮缺陷2)。
    if (processor_.setTransitionRamp(ms))
        ++processor_.runtime().configSeq; // 变化才 bump(PR#55 缺陷4)
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

    // 阶段1:先校验全部枚举字段到局部量(PR#55 第4轮缺陷2)。patch 未含字段时局部量保持空串,
    // 阶段2 的 setter 依 hasLoudness/hasCenter 忽略之 —— 无需回退读 rt 现值(复评建议②,删死值回退读)。
    juce::String loudnessMode;
    juce::String centerSlotPolicy;
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

    // 阶段2:经 processor 持锁 setter 一次性应用(复评重要①:与 getStateInformation 同锁读,
    // 消除 juce::String COW 跨线程竞态;变化才 bump configSeq)。
    (void)processor_.setAnalysisConfig(loudnessMode, centerSlotPolicy, hasLoudness, hasCenter);
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
    if (sr <= 0.0) // 未 prepare(样本率未知)→ 拒绝秒↔样本换算(PR#55 第6轮缺陷1)
    {
        c(badArgResp());
        return;
    }
    if (!payload.isObject())
    {
        c(badArgResp()); // payload 必须为对象(PR#55 第11轮缺陷1)
        return;
    }

    scvb::state::SegmentEditArgs args;

    if (op == "move_boundary")
    {
        if (!payload.hasProperty("segIdx") || !payload.hasProperty("tS") || !payload.hasProperty("edge"))
        {
            c(badArgResp()); // 必填字段缺失(PR#55 第11轮缺陷1)
            return;
        }
        args.op = scvb::state::SegmentEditOp::MoveBoundary;
        args.segIdx = static_cast<int>(payload.getProperty("segIdx", 0));
        const juce::String edge = payload.getProperty("edge", juce::String()).toString();
        args.edgeIsT0 = (edge == "t0");
        if (edge != "t0" && edge != "t1")
        {
            c(badArgResp());
            return;
        }
        args.tSamples =
            static_cast<std::int64_t>(std::llround(static_cast<double>(payload.getProperty("tS", 0.0)) * sr));
    }
    else if (op == "split")
    {
        if (!payload.hasProperty("segIdx") || !payload.hasProperty("tS"))
        {
            c(badArgResp());
            return;
        }
        args.op = scvb::state::SegmentEditOp::Split;
        args.segIdx = static_cast<int>(payload.getProperty("segIdx", 0));
        args.tSamples =
            static_cast<std::int64_t>(std::llround(static_cast<double>(payload.getProperty("tS", 0.0)) * sr));
    }
    else if (op == "merge")
    {
        if (!payload.hasProperty("segIdxA") || !payload.hasProperty("segIdxB"))
        {
            c(badArgResp());
            return;
        }
        args.op = scvb::state::SegmentEditOp::Merge;
        args.segIdx = static_cast<int>(payload.getProperty("segIdxA", 0));
        args.segIdxB = static_cast<int>(payload.getProperty("segIdxB", 0));
    }
    else if (op == "set_values")
    {
        if (!payload.hasProperty("segIdx"))
        {
            c(badArgResp());
            return;
        }
        args.op = scvb::state::SegmentEditOp::SetValues;
        args.segIdx = static_cast<int>(payload.getProperty("segIdx", 0));
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
        {
            c(badArgResp());
            return;
        }
    }
    else if (op == "set_locked")
    {
        if (!payload.hasProperty("segIdx") || !payload.hasProperty("locked"))
        {
            c(badArgResp()); // locked 必填(PR#55 第11轮缺陷1)
            return;
        }
        args.op = scvb::state::SegmentEditOp::SetLocked;
        args.segIdx = static_cast<int>(payload.getProperty("segIdx", 0));
        bool lockedVal = false;
        if (!strictBool(payload.getProperty("locked", false), lockedVal))
        {
            c(badArgResp());
            return;
        }
        args.locked = lockedVal;
    }
    else
    {
        c(badArgResp()); // 未知 op
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

    emitSegments("edit", static_cast<std::uint16_t>(1u << (ch - 1))); // 仅该轨(PR#55 第11轮缺陷2)
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
    // 打洞 FrameStore 的 coverage(§1.24);clearedS = 实际清掉的总时长,供 UI 反馈。
    const double clearedS = processor_.clearCoverage(static_cast<std::uint16_t>(tracksMask & 0x7FFF), startS, endS);
    // 覆盖变了就重置 captureProgress 的增量基线,否则下一帧的差集会把「已被清掉的区间」
    // 当成仍然存在,覆盖条撤不下去。
    for (auto& r : lastCoverageRanges_)
        r.clear();
    lastCoveragePct_.fill(-1.0f); // 哨兵:与任何真实百分比都不等 → 下一帧必报

    juce::var o = obj();
    put(o, "ok", true);
    put(o, "clearedS", clearedS);
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
        emitSegments("undo", kAllTracksMask); // 全量段表(PR#55 建议①)
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
        emitSegments("redo", kAllTracksMask); // 全量段表(PR#55 建议①)
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
    // 数据源 = FrameStore(Input 写 feat 段 → Output 25Hz 增量拉取),与 §2.7 覆盖率同一份账。
    // T29 落卡时这里是**写死的全未覆盖桩**:回包形状合法、能过 isTileShape,于是泳道照常画
    // 斜纹与栅格,但每一列 covered=0 → 包络层整体 `continue` 跳过 —— 真机上就是「有斜纹、
    // 没波形」的纯黑泳道(v5 实测 P0-4)。桩的形状太像真回包,所以三个版本都没被发现。
    // 跨度上界(P0-A):内层代价虽已与覆盖同阶,但**病态请求本身**不该被受理 ——
    // 纵深防御的第二道。上界取「已采集范围」与常数的较大者留出余量,超了直接 badArg,
    // 让前端的坏时长在这里就停住,而不是变成一次几十秒的持锁计算。
    if (endS - startS > ScvbOutputAudioProcessor::kMaxRequestSpanS)
    {
        c(badArgResp());
        return;
    }
    const auto tile = processor_.waveformOf(ch, startS, endS, cols);
    juce::var minDb = mkArray();
    juce::var maxDb = mkArray();
    juce::var vad = mkArray();
    juce::var covered = mkArray();
    juce::var stale = mkArray();
    juce::var passId = mkArray();
    for (int i = 0; i < cols; ++i)
    {
        const auto k = static_cast<std::size_t>(i);
        push(minDb, tile.minDb[k]);
        push(maxDb, tile.maxDb[k]);
        push(vad, tile.vad[k]);
        push(covered, tile.covered[k]);
        // stale/passId:重分析代际标记归 T33 的段表面,波形瓦片本身不带代际(恒 0)。
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

void OutputEditor::handleSetMasterChartMode(const ArgList& a, Completion c)
{
    const juce::String mode = a.size() > 0 ? a[0].toString() : juce::String();
    if (mode != "distribution" && mode != "trajectory")
    {
        c(badArgResp());
        return;
    }
    processor_.setMasterChartMode(mode); // 写 state ui.master_chart_mode(随工程持久化)
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
    // §1.32 alsoGlobal(缺省 true):勾了「不再显示」才写系统级全局默认,承诺跨工程成立。
    // **两个参数都校验完才落任何值** —— badArg 回执与已生效的副作用不能并存。
    bool alsoGlobal = true;
    if (a.size() >= 2 && !strictBool(a[1], alsoGlobal))
    {
        c(badArgResp());
        return;
    }
    processor_.bridgeSetGuideSeen(seen); // 持 lifecycleMutex_(与 getStateInformation 同锁)
    if (alsoGlobal)
    {
        uidefaults::setGuideSeenGlobal(seen);
    }
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
    // §1.33 alsoGlobal(缺省 true):完成与「暂不」都置全局位 → 新工程不再自动询问。
    // 校验先于落值,理由同 handleSetGuideSeen。
    bool alsoGlobal = true;
    if (a.size() >= 2 && !strictBool(a[1], alsoGlobal))
    {
        c(badArgResp());
        return;
    }
    processor_.bridgeSetTourSeen(seen); // 持 lifecycleMutex_(与 getStateInformation 同锁)
    if (alsoGlobal)
    {
        uidefaults::setTourSeenGlobal(seen);
    }
    c(okResp());
}

void OutputEditor::handleConfirmPrintGuard(const ArgList& /*a*/, Completion c)
{
    processor_.runtime().printGuardPending = false; // 幂等(§1.34)
    c(okResp());
}

} // namespace scvb::output
