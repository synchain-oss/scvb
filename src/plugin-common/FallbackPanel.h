// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>

namespace scvb::webview
{
// FallbackPanel —— WebView2 运行时缺失 / 5s 看门狗超时时的原生兜底面板(01 §6.1 机制 3 后半;平台无关)。
// 固定像素布局,不随 uiScale 缩放;切兜底时宿主(WebViewHost)负责把窗口放大到 ≥ 设计盒,
// 否则小缩放档位(如 33%)会挤压/裁掉按钮。retryWebView 成功回到 WebView 时再按 uiScale 恢复。
class FallbackPanel final : public juce::Component
{
public:
    struct Options
    {
        juce::String title;
        juce::String message;
        bool showInstall = false; // 缺运行时 -> 显示「下载 WebView2 Runtime」;看门狗超时 -> 不显示
        std::function<void()> onInstall;
        std::function<void()> onRetry;
    };

    explicit FallbackPanel(Options options);
    ~FallbackPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    Options options_;
    juce::Label title_;
    juce::Label message_;
    juce::TextButton install_;
    juce::TextButton retry_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FallbackPanel)
};

} // namespace scvb::webview
