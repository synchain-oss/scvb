// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// InputEditor —— Input 插件编辑器(T30):WebViewHost 装配 + Input 桥专属原生函数与事件
// (docs/SCVB_CONTRACT.md §3/§4)。diff-then-emit 全部在 emitTick()(25Hz,[M]):
//   scvb.state  变化即发(首帧必发);
//   scvb.conn   ~4Hz(250ms 折半);
//   scvb.config 25Hz 轮询,config_seq 变化才发;
//   scvb.groups ~1Hz;
//   scvb.error  随 claim 态迁移边沿即时发(channelConflict/srMismatch;abi 不占 error code,§4.5)。

#include "WebViewHost.h"

#include <juce_audio_processors/juce_audio_processors.h>

class ScvbInputAudioProcessor;

namespace scvb::input
{
class InputEditor final : public scvb::webview::WebViewHost
{
public:
    explicit InputEditor(ScvbInputAudioProcessor& processor);
    ~InputEditor() override = default;

protected:
    // 首帧全量快照(§3.1 InputSnapshot;每次调用重置 diff 基线,WebView 重载安全)。
    juce::var buildSnapshot() override;
    // 25Hz diff-then-emit(机制 7;仅在 mBridgeReady 且非兜底时被 WebViewHost 调用)。
    void emitTick() override;
    // setLang:归一化后同时落 Input state(uiLanguage,params-v0 §三;§3.5)。
    void handleSetLang(const juce::Array<juce::var>& args,
                       juce::WebBrowserComponent::NativeFunctionCompletion complete) override;
    // commitUiScale 防呆确认后落 Input state(uiScale 百分数;§3.5)。
    void persistUiScaleAsDefault() override;

private:
    // 追加 Input 专属原生函数(WebViewHost 装配后调用;通用四函数已由基类注册)。
    void registerNativeFunctions(juce::WebBrowserComponent::Options& options);

    void handleSetChannelId(const juce::Array<juce::var>& args,
                            juce::WebBrowserComponent::NativeFunctionCompletion complete);
    void handleSetGroupId(const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion complete);
    void handleRemoteSetPriority(const juce::Array<juce::var>& args,
                                 juce::WebBrowserComponent::NativeFunctionCompletion complete);

    // JSON 串比对后按需 emit(DynamicObject 的 var== 是指针比较,不可用于 diff)。
    void emitIfChanged(const char* name, const juce::var& payload, juce::String& lastJson);
    // claim 态迁移边沿 → scvb.error(conflict 进出 / srMismatch 进出)。
    void emitClaimError(const juce::String& claim, const juce::String& prevClaim, int channelId, int groupId,
                        int localSr, int outputSr);

    ScvbInputAudioProcessor& processor_;
    juce::String pluginVersion_; // JucePlugin_VersionString(§3.1 version.plugin)

    // diff-then-emit 状态(buildSnapshot 快照回执时全部重置 → 重载后的新页面首帧必收各事件)。
    juce::String lastStateJson_;
    juce::String lastConnJson_;
    juce::String lastConfigJson_;
    juce::String lastGroupsJson_;
    juce::String lastErrorJson_;
    juce::uint32 lastConfigSeq_ = 0xFFFFFFFFu; // 哨兵 = 首 tick 必发 scvb.config(§0.4;其后仅 seq 变化才发)
    juce::String lastClaim_; // 快照时初始化为当前 claim(error 只发迁移边沿)
    int lastErrorChannelId_ = -1; // error 边沿键的 channel 分量(claim 同但 channel 变也重发;哨兵 -1)
    juce::uint64 lastConnMs_ = 0; // 4Hz 折半(快照回执复位 → 首 tick 必发 scvb.conn)
    juce::uint64 lastGroupsMs_ = 0; // 1Hz 折半(快照回执复位 → 首 tick 必发 scvb.groups)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InputEditor)
};
} // namespace scvb::input
