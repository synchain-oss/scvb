// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlatformWebView.h"

#include <atomic>

#if JUCE_WINDOWS
// 前置声明免引 <windows.h>(与 PlatformWebViewRuntime.cpp 对 loader 的处理同一手法:
// 这个 TU 还要能离线单测,不该把整个 Win32 头拖进来)。
extern "C" unsigned long __stdcall GetCurrentProcessId();
#else
#include <unistd.h> // getpid
#endif

namespace scvb::webview
{

using WBC = juce::WebBrowserComponent;

namespace
{
// 本进程 PID。**只进诊断行,不再进目录名**(理由见 makeUserDataFolder)。用户在诊断行里
// 看到的数字能直接和任务管理器对上,排查「谁占着 WebView2」时有用。
// [B 线 M07] 非 Windows 由恒 0 改为 getpid():mac 上同样要能和活动监视器对上
// (进程外 AU 时插件跑在 AUHostingService 里,PID 就是区分「进程内 / 进程外」的那一项)。
int currentProcessId()
{
#if JUCE_WINDOWS
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
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
    // [B 线 M07] 空 File = 本平台不用 UDF。WKWebView 用默认的 WKWebsiteDataStore,从不读这个目录;
    // 此前这里返回 tempDirectory/SCVB-WebView(mac 上 = ~/Library/Caches/<宿主可执行名>/),
    // 构造期还会在里面建目录、写探针 —— 只是在用户的 Caches 里留一个没人用的目录。
    return {};
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
    //
    // [B 线 M07] 根为空(本平台不用 UDF)时返回空 File,**不**往下拼:juce 的
    // File().getChildFile("x") 会把空路径补成分隔符,得到文件系统根下的 "/x"。
    const auto root = userDataFolderRoot();
    if (root == juce::File())
        return {};
    return root.getChildFile(userDataFolderName);
}

juce::String PlatformWebView::probeUserDataFolder(const juce::File& folder)
{
    // [B 线 M07] 空 File = 本平台不用 UDF(见 userDataFolderRoot):不建目录、不写探针,也不算问题。
    // Windows 上 makeUserDataFolder 恒非空,走不到这一支。
    if (folder == juce::File())
        return {};

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
    // 这一层什么都不挡,露的是窗口的白。
    // [SL-370] 这个值是**浅色**(= 成品外壳渐变的中点色),不再是 SL-253 当时的暗底;
    // 换句话说这一层也是「渲染阻塞那一段屏上到底是什么颜色」的决定者(见
    // WebViewHost.cpp 的 HostWebView::paint 头注 ①-b / ①-c 两节)。
    // [SL-402] 占位升成**渐变**之后,这一层仍只能收**纯色**(WebView2 的
    // DefaultBackgroundColor 没有渐变形态)⇒ 取占位渐变沿轴 50% 的插值色
    // shellBackdropMid(),与渐变占位不跳阶。真源仍是 PlatformWebView.h 的
    // kShellBackdropStops(⑥/⑥c 对拍 tokens.css 的 --page-gradient)。
    wv2 = wv2.withBackgroundColour(shellBackdropMid());
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

PlatformWebView::BackgroundColourSupport PlatformWebView::backgroundColourSupport(const RuntimeInfo& info)
{
    // 没探到运行时 = 这一层无从谈起(这条路上根本不会去建控制器,调用方直接切兜底面板)。
    if (info.status == RuntimeStatus::missing)
        return BackgroundColourSupport::unknown;

    // 版本串解析不出:**不猜**。runtimeInfo() 在这种情况下刻意放行走正常加载路径
    // (宁可让看门狗兜底也不误判 tooOld),这里同样不许拿一个猜来的结论顶替「不知道」。
    const int major = majorVersionOf(info.version);
    if (major < 0)
        return BackgroundColourSupport::unknown;

    return major >= kBackgroundColourMinRuntimeMajor ? BackgroundColourSupport::available
                                                     : BackgroundColourSupport::unavailable;
}

// -----------------------------------------------------------------------------
// [B 线 M07] 按引擎分支的用户可见文案与诊断行。
// WebView2 那一支**逐字**搬自 WebViewHost.cpp(M07 之前的 missingRuntimeMessage / tooOldRuntimeMessage /
// envNotStartedMessage / logBackgroundColourSupport / buildDiagnostics),由 test_plugin_common.cpp 的
// [M07] 钉字格逐字对拍 —— 改这几句就是改 Windows 用户看到的字,那几格会红。
// 文案一律 ASCII:运行期字面量含非 ASCII 会在 CP936 机器上触发 MSVC C4819(判例 cp936-chinese-source-c4819)。
// -----------------------------------------------------------------------------
juce::String PlatformWebView::missingRuntimeMessage(Engine engine)
{
    if (engine == Engine::webView2)
        return "Microsoft Edge WebView2 Runtime was not found, so the full UI cannot load.\n"
               "Install the runtime once, then reopen this plugin window.";
    return "The system web view (WebKit) is not available, so the full UI cannot load.\n"
           "Update macOS, then reopen this plugin window.";
}

juce::String PlatformWebView::tooOldRuntimeMessage(Engine engine)
{
    if (engine == Engine::webView2)
        return "The installed Microsoft Edge WebView2 Runtime is too old for this plugin.\n"
               "Update to the Evergreen runtime, then reopen this plugin window.";
    return "The system web view (WebKit) is too old for this plugin.\n"
           "Update macOS, then reopen this plugin window.";
}

juce::String PlatformWebView::envNotStartedMessage(Engine engine)
{
    if (engine == Engine::webView2)
        return "The WebView2 environment did not start (no navigation ever began).\n"
               "This is usually the user-data folder being unwritable or already in use by another\n"
               "process, or the host blocking the msedgewebview2.exe child process.";
    // 系统 WebKit:没有 user-data 目录这一环(userDataFolderRoot() 为空),剩下的常见成因是宿主挡了
    // WebKit 的网页内容子进程(com.apple.WebKit.WebContent),或系统内存吃紧。
    return "The system web view (WebKit) did not start (no navigation ever began).\n"
           "This is usually the host blocking the WebKit web content process, or the\n"
           "system running low on memory. Click Retry, or close and reopen this plugin window.";
}

juce::String PlatformWebView::runtimeDiagnosticsField(const RuntimeInfo& info, Engine engine)
{
    if (engine == Engine::webView2)
        return "WebView2 " + (info.version.isNotEmpty() ? info.version : juce::String("not found"));
    // 系统 WebKit 的版本跟着 macOS / Safari 走,runtimeInfo() 不探测(version 恒空)。
    // 写成「not found」会把一台正常的机器误报成缺运行时(M07 之前 mac 上就是这样)。
    return "WebKit (system)";
}

// [SL-376 / SL-364] 判定与它证到哪一步(以及为什么插件侧做不到直接观测)只写在
// PlatformWebView.h 的 backgroundColourSupport() 头注一处,这里不复述。
// 四条形态,措辞互不相同,便于在 DebugView / 宿主日志里直接 grep:
//   available   —— 正常;这一层**按版本推断**在,控制器建好到首帧之间铺的是我方 argb。
//   UNAVAILABLE —— SL-364 命中;那一段露的是 WebView2 默认白。**本卡不修**(遮挡闸已经让
//                  那一段不上屏),但要如实说出来,别再让下一个人从零查一遍。
//   unknown ×2  —— 版本串没解析出来 / 压根没探到运行时;两种原因分开写,不猜。
// ⚠ 措辞用 `inferred present|absent (from runtime ..., not directly observed)` 而不是
//   `present|absent`(#247 复审【建议】3,统筹裁定按「inferred from runtime >= 87」落地):
//   这一行是**按运行时主版本推断**出来的,不是对 JUCE 那次 QueryInterface 的直接观测。
//   用户会把 DebugView 片段整段贴回来,而贴回来的人多半不会同时读 PlatformWebView.h 的
//   头注 —— 所以「这是推断」必须写在**行里**,不能只写在注释里。
// [B 线 M07] 这一层是 WebView2 专有的(DefaultBackgroundColor / ICoreWebView2Controller2),系统 WebKit
// 返回空串 = 不出这一行;M07 之前 mac 上会打出「unknown (runtime version unknown not parsable)」。
juce::String PlatformWebView::backgroundColourDiagnostics(const RuntimeInfo& info, Engine engine)
{
    if (engine != Engine::webView2)
        return {};

    using Support = BackgroundColourSupport;
    const auto support = backgroundColourSupport(info);
    const juce::String version = info.version.isNotEmpty() ? info.version : juce::String("unknown");
    const juce::String argb = juce::String::toHexString(static_cast<int>(scvb::webview::shellBackdropMid().getARGB()))
                                  .paddedLeft('0', 8); // [SL-402] DefaultBackgroundColor 仍收纯色:占位渐变的轴中点色

    const juce::String floor = juce::String(kBackgroundColourMinRuntimeMajor);
    if (support == Support::available)
        return "webview2 default background: available -- ICoreWebView2Controller2 inferred present "
               "(from runtime " +
               version + " >= major " + floor + ", not directly observed), JUCE puts argb " + argb;
    if (support == Support::unavailable)
        return "webview2 default background: UNAVAILABLE -- ICoreWebView2Controller2 inferred absent "
               "(from runtime " +
               version + " < major " + floor + ", not directly observed), JUCE drops argb " + argb + " silently";
    if (info.status == RuntimeStatus::missing)
        // 当前调用点(beginLoadAttempt)在 missing 时已提前 return,走不到这里 —— 但把它写对
        // 是为了将来挪调用点的人(#247 复审【建议】⑤):否则这条会打成
        // "runtime version unknown not parsable",把原因指错。
        return "webview2 default background: unknown (no WebView2 runtime detected)";
    return "webview2 default background: unknown (runtime version " + version + " not parsable)";
}

juce::String PlatformWebView::userDataFolderDisplay(const juce::File& folder)
{
    // 空 File = 本平台不用 UDF(userDataFolderRoot() 的非 Windows 分支)。WebView2 上恒非空,原样出路径。
    if (folder == juce::File())
        return "(not used)";
    return folder.getFullPathName();
}

} // namespace scvb::webview
