// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <atomic>
#include <functional>
#include <memory>

#include "FallbackPanel.h"
#include "ResourceProvider.h"

namespace scvb::webview
{
// WebViewHost —— 两插件共用的 WebView2 编辑器装配基类(01 §6.1 机制 3/5/6/7/8/9 的中枢)。
// T29/T30 的 InputEditor / OutputEditor 继承本类,经 Config::augmentOptions 追加各自的
// 原生函数 / 首帧 seed,并覆写 buildSnapshot() / emitTick() 补充 diff-then-emit。
class WebViewHost : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    struct Config
    {
        juce::String role; // Init::Role:"input" | "output"
        juce::String userDataFolderName; // WebView2 user-data 目录名(Input/Output 各一)
        juce::String version = "0.1.0"; // 首帧 version seed(插件侧传 JucePlugin_VersionString)
        juce::String lang = "zh";
        float uiScale = 1.0f;
        int channelLimit = 15; // [J59]
        ResourceProvider::Source resourceSource; // 插件自己的 BinaryData::*;暂无可嵌资源则传空 Source
        // T29/T30 在此追加插件专属的原生函数/首帧 seed(在通用装配之后调用)。空 = 只用通用装配。
        std::function<void(juce::WebBrowserComponent::Options&)> augmentOptions;
    };

    WebViewHost(juce::AudioProcessor& processor, Config config);
    ~WebViewHost() override;

    void resized() override;

    // 缩放(机制 9):uiScale 实时预览(不落盘);commitUiScale 防呆确认后落盘全局默认。
    float uiScale() const { return uiScale_; }
    const juce::String& lang() const { return lang_; } // T30:scvb.state ui.language / 落 state 用
    void setUiScale(float scale); // clamp + setSize(DESIGN×scale) 预览
    void commitUiScale(); // 落盘(子类覆写 persistUiScaleAsDefault)

    juce::WebBrowserComponent& webView() { return webView_; }

protected:
    // 子类覆写以落盘全局默认(宿主侧持久化由插件 Processor 实现;默认空实现)。
    virtual void persistUiScaleAsDefault();

    // 子类覆写以提供首帧全量快照(requestInitialState 回执)。
    virtual juce::var buildSnapshot();

    // 25Hz diff-then-emit(机制 7):先跑看门狗/就绪门控,再调用子类的 emitTick()。
    void timerCallback() override;

    // 子类在 timer 里追加自己的 diff-then-emit(仅在 mBridgeReady 且非兜底时被调用)。
    virtual void emitTick();

    // 通用原生函数 handler(两插件共用;子类可复用/覆写)。
    void handleRequestInitialState(const juce::Array<juce::var>& args,
                                   juce::WebBrowserComponent::NativeFunctionCompletion complete);
    virtual void handleSetLang(const juce::Array<juce::var>& args,
                               juce::WebBrowserComponent::NativeFunctionCompletion complete);
    void handleSetUiScale(const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion complete);
    void handleCommitUiScale(const juce::Array<juce::var>& args,
                             juce::WebBrowserComponent::NativeFunctionCompletion complete);

private:
    enum class FallbackReason
    {
        MissingRuntime,
        LoadTimeout
    };

    juce::WebBrowserComponent::Options makeOptions(); // 装配通用 Options + 子类 augmentOptions
    void showFallback(FallbackReason reason);
    void retryWebView();
    void resizeToDesignBox(float scale);

    Config config_;
    ResourceProvider provider_;
    float uiScale_;
    juce::String lang_;

    std::atomic<bool> bridgeReady_{false};
    juce::uint32 startMs_ = 0;

    juce::WebBrowserComponent webView_;
    std::unique_ptr<FallbackPanel> fallback_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WebViewHost)
};

} // namespace scvb::webview
