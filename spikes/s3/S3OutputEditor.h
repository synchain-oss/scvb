// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// S3 spike —— Output WebView2 编辑器(最小插件壳的 UI 载体)。
//   Bridge 的 WebView2 装配全套搬运(01 §6.1 的 9 条机制):固定设计盒 1180×780 + CSS zoom +
//   resource provider 嵌 spike 页 + 原生函数桥(05 §1.4 API 面)+ 5s 看门狗 + FallbackPanel 兜底。
//   25Hz diff-then-emit:scvb.meters(15 路 + 总线)/ scvb.playhead / scvb.state(含 50Hz/25Hz Timer 实测频率)。

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <atomic>
#include <memory>
#include <optional>

#include "S3OutputProcessor.h"

class S3OutputEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit S3OutputEditor(S3OutputProcessor& processor);
    ~S3OutputEditor() override;

    void resized() override;

private:
    void timerCallback() override;

    juce::WebBrowserComponent::Options makeOptions();
    std::optional<juce::WebBrowserComponent::Resource> provideResource(const juce::String& url) const;
    static const char* mimeForExtension(const juce::String& ext);

    juce::var buildSnapshot() const;
    juce::var buildWaveform(int ch, double startS, double endS, int cols) const;

    void handleRequestInitialState(const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void handleRequestWaveform(const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void handleSetUiScale(const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void handleCommitUiScale(const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);

    static bool webView2RuntimeAvailable();
    void showFallback();

    S3OutputProcessor& processor_;

    juce::WebBrowserComponent webView_;
    std::unique_ptr<juce::Component> fallback_;

    std::atomic<bool> bridgeReady_{false};
    juce::uint32 startMs_ = 0;

    double meterPhase_ = 0.0;
    double playheadS_ = 0.0;
    double lastTimerTick_ = 0.0;
    float timerHz25_ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(S3OutputEditor)
};
