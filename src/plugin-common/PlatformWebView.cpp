// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlatformWebView.h"

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

using WBC = juce::WebBrowserComponent;

juce::WebBrowserComponent::Options PlatformWebView::makeWebViewOptions(juce::WebBrowserComponent::Options options,
                                                                       const juce::String& userDataFolderName)
{
#if JUCE_WINDOWS
    WBC::Options::WinWebView2 wv2;
    wv2 = wv2.withUserDataFolder(
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(userDataFolderName));
    options = options.withBackend(WBC::Options::Backend::webview2).withWinWebView2Options(wv2);
#else
    juce::ignoreUnused(userDataFolderName);
#endif
    return options;
}

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
