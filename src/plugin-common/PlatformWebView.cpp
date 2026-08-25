// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlatformWebView.h"

#include <atomic>

namespace scvb::webview
{

using WBC = juce::WebBrowserComponent;

namespace
{
// 每实例唯一后缀(进程内递增)。多个 Input 插件实例在同一宿主进程内共享进程;若都沿用同一
// 固定 user-data 目录名,WebView2 初始化会因同名 folder 抢占而失败,故给每个实例追加唯一后缀。
std::atomic<int>& instanceCounter()
{
    static std::atomic<int> counter{0};
    return counter;
}
} // namespace

juce::WebBrowserComponent::Options PlatformWebView::makeWebViewOptions(juce::WebBrowserComponent::Options options,
                                                                       const juce::String& userDataFolderName)
{
#if JUCE_WINDOWS
    const juce::String uniqueName = userDataFolderName + "_" + juce::String(instanceCounter().fetch_add(1));
    WBC::Options::WinWebView2 wv2;
    wv2 = wv2.withUserDataFolder(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(uniqueName));
    options = options.withBackend(WBC::Options::Backend::webview2).withWinWebView2Options(wv2);
#else
    juce::ignoreUnused(userDataFolderName);
#endif
    return options;
}

const char* PlatformWebView::runtimeDownloadUrl()
{
    return "https://go.microsoft.com/fwlink/p/?LinkId=2124703";
}

int PlatformWebView::majorVersionOf(const juce::String& version)
{
    const auto head = version.trim().upToFirstOccurrenceOf(".", false, false).trim();
    if (head.isEmpty() || !head.containsOnly("0123456789"))
        return -1;
    return head.getIntValue();
}

} // namespace scvb::webview
