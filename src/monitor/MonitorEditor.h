// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MonitorEditor —— Monitor 编辑器壳(T45)。复用 plugin-common 的 WebViewHost 装配
// (含 PlatformWebView 的 per-plugin + per-instance UserDataFolder 隔离 —— Monitor 拿自己的
// 目录名,不与 Input/Output 抢)。
//
// 本卡只交壳:一个最小占位页 + 「组选择 / 在线态 / viz 帧摘要」三个事件,够跑通
// Output → viz 段 → Monitor → web 的整条链路。**真 UI(上分布图 / 下轨迹图)归 T46**,
// 资源目录约定已留好:`web/monitor/`(见该目录 README —— 页面实现归 T46/#90,
// T45 早期的占位页已删除,免得两个 PR 各带一份同名文件在合并时互撞)。

#include "WebViewHost.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>

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
    // includeLanes=false 时只带标量帧头(稳态);车道/位图只在 lane_revision 变化时带。
    juce::var buildVizPayload(bool includeLanes) const;
    juce::var buildPlayheadPayload() const;

    // JSON 串比对后按需 emit(与 Input/Output 同款 diff-then-emit;隐藏期不推进缓存)。
    bool emitIfChanged(const char* name, const juce::var& payload, juce::String& lastJson);

    ScvbMonitorAudioProcessor& processor_;
    juce::String pluginVersion_;

    juce::String lastStateJson_;
    juce::String lastGroupsJson_;
    juce::String lastVizJson_;
    std::uint32_t lastSentLaneRevision_ = 0; // 已随事件送出过车道的 revision
    bool sentLanes_ = false; // 是否送出过任何一帧车道(首帧必带)
    juce::String lastPlayheadJson_;
    juce::uint64 lastGroupsMs_ = 0; // 1Hz 折半
    juce::uint64 lastVizMs_ = 0; // 4Hz 折半(与发布器同频)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MonitorEditor)
};
} // namespace scvb::monitor
