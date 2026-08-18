// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace scvb::webview
{
// PlatformWebView —— 平台 WebView 分支集中地(01 §9;01 §6.1 机制 1/2 与机制 3 前半)。
//   Windows:WebView2(显式后端选择 + 可写 user-data 目录 + 静态 loader 运行时探测);
//   macOS/Linux:WKWebView / WebKitGTK(JUCE 内建,无需探测,恒可用)。
class PlatformWebView
{
public:
    // 把 Windows 专属的 WebView2 分支应用到 options(机制 1/2):
    //   显式选 webview2 后端(否则 JUCE 回退旧 IE ActiveX 控件,不支持 resource provider /
    //   native 集成,把 https://juce.backend/ 当真实网址导航 → "无法打开此页");
    //   user-data 目录指向临时目录下以 userDataFolderName 命名的子目录(DAW 安装目录只读会
    //   导致 WebView2 初始化失败,必须给可写目录)。
    // 非 Windows 原样返回(走系统默认后端:WKWebView / WebKitGTK)。
    static juce::WebBrowserComponent::Options makeWebViewOptions(juce::WebBrowserComponent::Options options,
                                                                 const juce::String& userDataFolderName);

    // WebView 运行时可用性探测(机制 3 前半):
    //   Windows = WebView2 Evergreen Runtime(GetAvailableCoreWebView2BrowserVersionString);
    //   非 Windows = 恒 true(系统 WebView 恒可用)。
    static bool runtimeAvailable();
};

} // namespace scvb::webview
