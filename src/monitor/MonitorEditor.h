// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MonitorEditor —— Monitor 编辑器壳(T45)。复用 plugin-common 的 WebViewHost 装配
// (含 PlatformWebView 的 per-plugin + per-instance UserDataFolder 隔离 —— Monitor 拿自己的
// 目录名,不与 Input/Output 抢)。
//
// 本卡只交壳:一个最小占位页 + 「组选择 / 在线态 / viz 帧摘要」三个事件,够跑通
// Output → viz 段 → Monitor → web 的整条链路。**真 UI(上分布图 / 下轨迹图)归 T46**,
// 资源目录约定已留好:`web/monitor/`。

#include "WebViewHost.h"

#include <juce_audio_processors/juce_audio_processors.h>

class ScvbMonitorAudioProcessor;

namespace scvb::monitor
{
class MonitorEditor final : public scvb::webview::WebViewHost
{
public:
    explicit MonitorEditor(ScvbMonitorAudioProcessor& processor);
    ~MonitorEditor() override = default;

protected:
    juce::var buildSnapshot() override;
    void emitTick() override;
    void handleSetLang(const juce::Array<juce::var>& args,
                       juce::WebBrowserComponent::NativeFunctionCompletion complete) override;
    void persistUiScaleAsDefault() override;

private:
    void registerNativeFunctions(juce::WebBrowserComponent::Options& options);
    void handleSetObservedGroup(const juce::Array<juce::var>& args,
                                juce::WebBrowserComponent::NativeFunctionCompletion complete);

    juce::var buildStatePayload() const;
    juce::var buildVizPayload() const;
    juce::var buildPlayheadPayload() const;

    // JSON 串比对后按需 emit(与 Input/Output 同款 diff-then-emit;隐藏期不推进缓存)。
    bool emitIfChanged(const char* name, const juce::var& payload, juce::String& lastJson);

    ScvbMonitorAudioProcessor& processor_;
    juce::String pluginVersion_;

    juce::String lastStateJson_;
    juce::String lastGroupsJson_;
    juce::String lastVizJson_;
    juce::String lastPlayheadJson_;
    juce::uint64 lastGroupsMs_ = 0; // 1Hz 折半
    juce::uint64 lastVizMs_ = 0; // 4Hz 折半(与发布器同频)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MonitorEditor)
};
} // namespace scvb::monitor
