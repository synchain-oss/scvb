// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "OutputBridgeApi.h"
#include "OutputProcessor.h"
#include "WebViewHost.h"

namespace scvb::output
{

// OutputEditor —— Output 插件桥(T29,契约 docs/SCVB_CONTRACT.md §1/§2)。
// 继承 WebViewHost(T26 装配层):requestInitialState/setLang/setUiScale/commitUiScale 四个通用函数与
// mBridgeReady 门控 / 25Hz Timer 由基类承载;本类经 augmentOptions 追加其余 30 个 native function,
// 并在 emitTick 里做 9 个事件的 diff-then-emit(每类独立节流 + 首帧必发)。
class OutputEditor final : public scvb::webview::WebViewHost
{
public:
    explicit OutputEditor(ScvbOutputAudioProcessor& processor);
    ~OutputEditor() override = default;

protected:
    // 首帧全量快照(契约 §1.1)。
    juce::var buildSnapshot() override;

    // 25Hz diff-then-emit(仅 mBridgeReady 后由基类调用)。
    void emitTick() override;

    // setLang:归一化后同时落 Output state(uiLanguage,params-v0 §二;§1.30)。
    // 基类只写 editor 局部 lang_,而 §2.1 的 ui.language 取自 processor —— 不落 processor
    // 就会在下一次 state emit 把旧语言回推给 UI(T37 真机 bug A-1)。
    void handleSetLang(const juce::Array<juce::var>& args,
                       juce::WebBrowserComponent::NativeFunctionCompletion complete) override;
    // commitUiScale 防呆确认后落 Output state + 系统级全局默认(§1.29)。
    void persistUiScaleAsDefault() override;

private:
    using Completion = juce::WebBrowserComponent::NativeFunctionCompletion;
    using ArgList = juce::Array<juce::var>;

    // ---- 事件发射 ----
    void emitState(bool forceFull);
    bool emitParams(bool forceFull); // 返回:C++ 侧观察到这一帧已下发(SL-199 闩锁清位判据)
    void emitConn();
    void emitGroups();
    void emitMeters();
    void emitPlayhead();
    void emitCaptureProgress();
    // tracksMask = u16 位图(bit0=ch1…bit14=ch15),kAllTracksMask=全轨;增量事件只含掩码内轨(PR#55 第11轮缺陷2)。
    bool emitSegments(const juce::String& reason, std::uint16_t tracksMask); // 同上
    void emitError(const juce::String& code, int ch, const juce::var& detail, bool active);

    // analyze/previewAnalyze 的作用域参数(§1.5/§1.6)。
    struct AnalyzeScope
    {
        std::uint16_t tracksMask = 0; // 0 = 不限轨
        double startS = 0.0;
        double endS = 0.0;
    };
    AnalyzeScope parseAnalyzeScope(const ArgList& a) const;

    // 宿主循环区(秒)。返回 false = 宿主未提供循环区,或提供了但没给 tempo 换算不出秒
    // (JUCE 的 loopPoints 只有 ppq)。true 时 startS/endS 有效且 endS > startS。
    bool hostLoopSeconds(double& startS, double& endS) const;
    // mode=daw_loop 时把 runtime 的 range 跟到宿主循环区上;返回 true = 本拍值有变化。
    bool syncDawLoopRange();

    // 契约 §2.1 的 state 子树(快照与 scvb.state 共用,防两处漂移)。
    juce::var buildStateSubtree(bool full) const;
    // 契约 §2.3 scvb.conn 载荷。
    juce::var buildConnPayload() const;
    // 契约 §2.8 scvb.segments 载荷(reason ∈ 十值;只输出 tracksMask 掩码内轨)。
    juce::var buildSegmentsPayload(const juce::String& reason, std::uint16_t tracksMask) const;

    // ---- native function 注册 ----
    void registerNativeFunctions(juce::WebBrowserComponent::Options& options);

    // ---- 30 个插件专属 handler(消息线程;全部立即 resolve)----
    void handleSetCaptureEnabled(const ArgList& a, Completion c);
    void handleSetOutputEnabled(const ArgList& a, Completion c);
    void handleSetGroupId(const ArgList& a, Completion c);
    void handlePreviewAnalyze(const ArgList& a, Completion c);
    void handleAnalyze(const ArgList& a, Completion c);
    void handleCancelAnalyze(const ArgList& a, Completion c);
    void handleSetRange(const ArgList& a, Completion c);
    void handleSetVersionActive(const ArgList& a, Completion c);
    void handleSetVersionName(const ArgList& a, Completion c);
    void handleCopyVersion(const ArgList& a, Completion c);
    void handleBeginParamGesture(const ArgList& a, Completion c);
    void handleSetParam(const ArgList& a, Completion c);
    void handleEndParamGesture(const ArgList& a, Completion c);
    void handleSetChannelConfig(const ArgList& a, Completion c);
    void handleSetTrackManual(const ArgList& a, Completion c);
    void handleSetPanCurve(const ArgList& a, Completion c);
    void handleSetVadParams(const ArgList& a, Completion c);
    void handleSetSegmentation(const ArgList& a, Completion c);
    void handleSetTransitionRamp(const ArgList& a, Completion c);
    void handleSetAnalysisConfig(const ArgList& a, Completion c);
    void handleEditSegment(const ArgList& a, Completion c);
    void handleRecaptureArm(const ArgList& a, Completion c);
    void handleClearCoverage(const ArgList& a, Completion c);
    void handleUndo(const ArgList& a, Completion c);
    void handleRedo(const ArgList& a, Completion c);
    void handleRequestWaveform(const ArgList& a, Completion c);
    void handleSetActiveTab(const ArgList& a, Completion c);
    void handleSetMasterChartMode(const ArgList& a, Completion c);
    void handleSetGuideSeen(const ArgList& a, Completion c);
    void handleSetTourSeen(const ArgList& a, Completion c);
    void handleConfirmPrintGuard(const ArgList& a, Completion c);

    // 只读观察态判定(OutputSession kObserver)。
    bool isReadOnly() const;

    ScvbOutputAudioProcessor& processor_;

    // diff-then-emit 的统一出口:**不可见时不推进缓存**。裸写法(先 lastJson=json 再
    // emitEventIfBrowserIsVisible)会把隐藏期的那一份变化吞掉 —— 载荷被丢,基线却已前移,
    // 恢复可见后 json==lastJson 于是永不重发。停走带时 scvb.playhead 载荷逐字节不变,
    // 这一吞就是永久的:playhead.js 内部 ev 恒 null,竖线维持 HTML 初始 hidden,泳道上
    // **既没有播放头也没有帧驱动**(v5 实测 P0-4 的后半)。Input(advanceEmitCache,PR#54 R4)
    // 与 Monitor(emitIfChanged)早已这样做,只有 Output 四处漏了。
    bool emitIfChanged(const char* eventName, const juce::var& payload, juce::String& lastJson);

    // ---- diff-then-emit 状态(消息线程独占)----
    bool firstFrame_ = true; // mBridgeReady 后首帧必发各事件(§0.4)
    // [SL-199] 上一拍 webview 是否可见:false→true 的边沿上补发 **scvb.params 全量 +
    // scvb.segments 全量快照** —— 两者的第二层基线(lastParamsValues_ / lastSegments* 三件)
    // 都在「发之前」就推进了,隐藏期被 emitEventIfBrowserIsVisible 丢掉的那一帧因此永不重发。
    // 初值 false:首帧本来就走全量,边沿不会多发一帧。
    bool wasVisible_ = false;
    bool wasSegVisible_ = false; // 同上,segments 一路独立记账(两条路的 settle 时机不同)
    bool pendingParamsFull_ = false; // 闩锁:置位后每拍强制全量,直到确认发出去才清
    bool pendingSegmentsFull_ = false; // 同上(scvb.segments 的 reason:"snapshot" 全量)
    // analyzed 闩锁位:takeAnalysisDone() 取走即清,隐藏期分析完成的话这一位会被消费掉,
    // 恢复可见只补 snapshot —— Tab4 陈旧基线同步与 Tab3 diff 摘要条只认 reason="analyze",
    // 会静默失效。发出去了才清(#119 复审顺带记账)。
    bool pendingAnalyzed_ = false;
    int tickCount_ = 0; // 25Hz 计数器(分频 conn ~4Hz / groups 1Hz / captureProgress 2Hz)
    double lastSegmentsSampleRate_ = 0.0; // 段表快照上次换算所用 sampleRate(变化即重发,PR#55 第7轮缺陷1)
    std::uint32_t lastCrvsRevision_ = 0; // CRVS 修订号检测(加载工程/预设后重发段表,PR#55 第8轮缺陷1)
    // 15 轨 stale 位图(bit{N-1});翻位即重发段表 —— fingerprint watchdog(04 §4.5)的判定不跟
    // 任何段编辑同步发生,不在这里检测的话「数据可能过期」要等到下一次段编辑/切版本才出得来。
    std::uint16_t lastStaleMask_ = 0;
    juce::String lastStateJson_;
    juce::String lastParamsJson_;
    juce::String lastConnJson_;
    juce::String lastPlayheadJson_;
    std::uint8_t lastGroupsOnline_ = 0;
    bool groupsEverSent_ = false; // scvb.groups 首帧必发(§0.4;独立于位图值)
    std::map<juce::String, float> lastParamsValues_; // scvb.params 稀疏 diff 缓存
    std::array<float, 15> lastMeterDb_{}; // meters 0.3dB 阈值(§0.4)
    std::array<float, 15> lastMeterPeak_{};
    float lastBusL_ = -1000.0f;
    float lastBusR_ = -1000.0f;
    float lastBusLPeak_ = -1000.0f;
    float lastBusRPeak_ = -1000.0f;
    bool metersEverSent_ = false;
    // §2.7 captureProgress 的增量基线:上一帧已报过的覆盖区间与覆盖率(index = ch-1)。
    std::array<std::vector<scvb::analysis::HopRange>, 15> lastCoverageRanges_{};
    std::array<float, 15> lastCoveragePct_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OutputEditor)
};

} // namespace scvb::output
