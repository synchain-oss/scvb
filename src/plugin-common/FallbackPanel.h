// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>

namespace scvb::webview
{
// [SL-402] 兜底面板的底色 —— **独立的常量,不再与开窗占位同源**(SL-253 收编、SL-370 沿用
// 的「面板底 = 占位色」那条性质由本卡拆开:占位在 SL-402 已是 tokens.css --page-gradient
// 同一组色标的**渐变**(PlatformWebView.h 的 kShellBackdropStops),而本面板的底只能是一块
// 纯色)。取值 = SL-402 当时占位渐变沿轴 50% 的中点色 #d9cadb(开窗切到兜底面板时与那块
// 渐变的中间亮度不跳阶),但那只是**当时的取值记录,不是跨文件真源**:tokens 的渐变日后
// 改了,这里既不会跟着变、也不会有判据红 —— 面板可读性由
// tests/webview/test_plugin_common.cpp 的 "FallbackPanel label colours stay readable …"
// 对比度断言独立把守(它读的就是本常量,不是占位那一族)。
inline constexpr juce::uint32 kFallbackPanelArgb = 0xffd9cadb;

// FallbackPanel —— WebView2 运行时缺失/过旧、看门狗超时、前端 boot 失败时的原生兜底面板
// (01 §6.1 机制 3 后半;平台无关)。
// 固定像素布局,不随 uiScale 缩放;切兜底时宿主(WebViewHost)负责把窗口放大到 ≥ 设计盒,
// 否则小缩放档位(如 33%)会挤压/裁掉按钮。retryWebView 成功回到 WebView 时再按 uiScale 恢复。
class FallbackPanel final : public juce::Component
{
public:
    struct Options
    {
        juce::String title;
        juce::String message;
        // 诊断行(可空):已等待时长 / 最后导航状态 / WebView2 Runtime 版本 / 前端错误摘要。
        // 小字等宽显示;同一份文本由 WebViewHost 同步写日志通道,用户回报问题时二者取其一即可。
        juce::String details;
        bool showInstall = false; // 缺运行时 / 运行时过旧 -> 显示「下载 WebView2 Runtime」;其余 -> 不显示
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
    juce::Label details_;
    juce::TextButton install_;
    juce::TextButton retry_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FallbackPanel)
};

} // namespace scvb::webview
