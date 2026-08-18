// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlatformWebView.h"

// 运行时探测单独成文件:本 TU 引用 WebView2 静态 loader(GetAvailableCoreWebView2BrowserVersionString),
// 需链接 WebView2 loader(仅插件 target 经 NEEDS_WEBVIEW2 链接)。拆开后 PlatformWebView.cpp 的
// makeWebViewOptions 可离线单测,无需 loader。真机探测路径在 T29/T30 接编辑器后验证。

#if JUCE_WINDOWS
// WebView2 静态 loader(JUCE NEEDS_WEBVIEW2 + 静态链接)导出;ole32 提供 CoTaskMemFree。
// 直接前置声明,免引 <WebView2.h>/<windows.h>(避开 include 路径与宏污染)。
extern "C" {
long __stdcall GetAvailableCoreWebView2BrowserVersionString(const wchar_t* browserExecutableFolder,
                                                            wchar_t** versionInfo);
void __stdcall CoTaskMemFree(void* pv);
}
#endif

namespace scvb::webview
{

bool PlatformWebView::runtimeAvailable()
{
#if JUCE_WINDOWS
    wchar_t* version = nullptr;
    const long hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    const bool ok = hr >= 0 && version != nullptr && version[0] != L'\0';
    if (version != nullptr)
        CoTaskMemFree(version);
    return ok;
#else
    return true; // macOS(WKWebView)/ Linux(WebKitGTK):系统 WebView 恒可用
#endif
}

} // namespace scvb::webview
