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
// 本进程 PID。**只进诊断行,不再进目录名**(理由见 makeUserDataFolder)。用户在诊断行里
// 看到的数字能直接和任务管理器对上,排查「谁占着 WebView2」时有用。
int currentProcessId()
{
#if JUCE_WINDOWS
    return static_cast<int>(GetCurrentProcessId());
#else
    return 0;
#endif
}
} // namespace

int PlatformWebView::processId()
{
    return currentProcessId();
}

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
    // **每个插件一个固定目录(Input 一个 / Output 一个),不再每实例一个。**
    //
    // WebView2 的浏览器进程组是**按 user-data 目录**共享的:同一个目录 ⇒ 复用同一组
    // msedgewebview2.exe(浏览器 + 渲染 + GPU);不同目录 ⇒ 各起一整套。之前那版给每个
    // 实例都拼一个唯一后缀,后果有两条,都很实在:
    //   ① **进程组永不复用**,于是「本进程已经起过桥所以这次算热启动」的判定形同虚设 ——
    //      第二次开窗其实还是完整冷启动,却按热预算(5s)计时,反而更容易误判超时。
    //      (本机实测曾看到 19 个 msedgewebview2.exe 同时在跑,就是这个后果。)
    //   ② 每开一个编辑器就多一整套浏览器进程,内存与句柄线性膨胀。
    //
    // 共用一个目录是 WebView2 的正常用法:同进程内多个 WebView2 实例共用 UDF 受支持;
    // 跨进程共用也受支持,**前提是环境选项一致** —— 失败条件是选项不一致
    // (HRESULT_FROM_WIN32(ERROR_INVALID_STATE):「与共享浏览器进程中正在运行的 WebView
    // 选项不匹配」),而不是「来自另一个进程」。本插件的环境选项是常量(JUCE 默认 + 固定
    // UDF),两个宿主进程拿到的完全一致,故可安全共享。
    //
    // 万一仍然创建失败(企业策略、目录被锁等),现在不会再表现成一句「加载太慢」:构造期的
    // 可写性探针 + 「导航事件从未到达 ⇒ 环境没起来」三态面板会把它如实说出来。
    return userDataFolderRoot().getChildFile(userDataFolderName);
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
    // [SL-253] WebView2 在**任何** web 内容之下铺的那一层。不设的话它是默认构造的
    // juce::Colour = ARGB 0x00000000 = **全透明**,JUCE 会把这个值原样 put 进
    // put_DefaultBackgroundColor —— 于是从控制器建好到 tokens.css/base.css 解析完为止,
    // 这一层什么都不挡,露的是窗口的白。铺上暗色即可覆盖整段。
    wv2 = wv2.withBackgroundColour(shellBackdrop());
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
