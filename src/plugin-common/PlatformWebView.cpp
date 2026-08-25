// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlatformWebView.h"

#include <atomic>

#if JUCE_WINDOWS
// 前置声明免引 <windows.h>(与 PlatformWebViewRuntime.cpp 对 loader 的处理同一手法:
// 这个 TU 还要能离线单测,不该把整个 Win32 头拖进来)。
extern "C" unsigned long __stdcall GetCurrentProcessId();
#endif

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

// 本进程的标识,用于 user-data 目录名。Windows 上取真 PID —— 用户在诊断行里看到的数字
// 能直接和任务管理器对上,这是排查「另一个宿主进程占着同一个 UDF」的关键;其它平台不涉
// WebView2,给 0 即可。
int currentProcessId()
{
#if JUCE_WINDOWS
    return static_cast<int>(GetCurrentProcessId());
#else
    return 0;
#endif
}
} // namespace

juce::File PlatformWebView::userDataFolderRoot()
{
#if JUCE_WINDOWS
    // %LOCALAPPDATA%\Synchain\SCVB\WebView2。
    //
    // **不用 File::tempDirectory**:Storage Sense / 磁盘清理会在会话进行中扫 %TEMP%,把正在
    // 用的 user-data 目录删掉,WebView2 就地崩或拒绝重建 —— 这类故障极难复现、更难归因;
    // 部分企业策略还把 %TEMP% 设成受限位置。
    //
    // **也不用 userApplicationDataDirectory**:JUCE 把它映射到 CSIDL_APPDATA = **Roaming**
    // (juce_Files_windows.cpp),而这里要放的是浏览器缓存 —— 域环境下漫游配置文件会在
    // 登录/注销时同步整个 Roaming,几百 MB 的 WebView2 缓存会拖垮登录、或直接撞上配额被拒。
    // windowsLocalAppData = CSIDL_LOCAL_APPDATA 才是本机缓存该待的地方。
    return juce::File::getSpecialLocation(juce::File::windowsLocalAppData)
        .getChildFile("Synchain")
        .getChildFile("SCVB")
        .getChildFile("WebView2");
#else
    return juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SCVB-WebView");
#endif
}

juce::File PlatformWebView::makeUserDataFolder(const juce::String& userDataFolderName)
{
    // 唯一性必须**跨进程**成立,不能只靠进程内计数器。
    //
    // 原实现只加了进程内自增后缀,于是同一台机器上不同进程拿到的是同一个名字(都是 "_0")——
    // 而宿主远不止一个进程:Cubase / Live 等把插件扫描放在独立的 sandbox 进程里,音频进程与
    // 扫描进程会同时活着;用户还可能同时开两个 DAW。WebView2 不支持两个进程共用一个 user-data
    // 目录,第二个会拿到 HRESULT_FROM_WIN32(ERROR_INVALID_STATE) 之类的失败 —— 而 JUCE 把这个
    // HRESULT 整个吞掉(juce_WebBrowserComponent_windows.cpp 的环境创建回调 HRESULT 形参无名),
    // 失败形态就是「窗口空白 + 看门狗超时 + 重试无效」,与本卡症状完全一致。
    // 故名字里带上 PID。
    const auto pid = currentProcessId();
    const juce::String uniqueName =
        userDataFolderName + "_p" + juce::String(pid) + "_" + juce::String(instanceCounter().fetch_add(1));
    return userDataFolderRoot().getChildFile(uniqueName);
}

juce::String PlatformWebView::probeUserDataFolder(const juce::File& folder)
{
    // 建目录 + 落一个探针文件再删。WebView2 只有在环境创建时才会碰这个目录,而那一步的失败被
    // JUCE 吞掉,所以「目录到底写不写得进」必须我们自己先测一次,否则诊断面板只能说「超时」。
    const auto result = folder.createDirectory();
    if (result.failed())
        return "user-data folder not creatable: " + result.getErrorMessage();

    const auto probe = folder.getChildFile(".scvb-write-probe");
    if (!probe.replaceWithText("ok"))
        return "user-data folder not writable: " + folder.getFullPathName();
    probe.deleteFile();
    return {};
}

juce::WebBrowserComponent::Options PlatformWebView::makeWebViewOptions(juce::WebBrowserComponent::Options options,
                                                                       const juce::File& userDataFolder)
{
#if JUCE_WINDOWS
    WBC::Options::WinWebView2 wv2;
    wv2 = wv2.withUserDataFolder(userDataFolder);
    options = options.withBackend(WBC::Options::Backend::webview2).withWinWebView2Options(wv2);
#else
    juce::ignoreUnused(userDataFolder);
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
