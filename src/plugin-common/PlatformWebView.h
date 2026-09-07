// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace scvb::webview
{
// [SL-253] 开窗底色的**唯一真源**。用户实测:插件窗口一开有一瞬间全白,然后才画出 UI。
// 成因是开窗路径上**三层都没有不透明底色**(编辑器组件无 paint、WebView2 的
// DefaultBackgroundColor 默认全透明、HTML 的底色藏在两个外链 css 里),白的是窗口本身。
//
// SL-253 当时取值与 `web/shared/tokens.css` 的 `--page-backdrop` 对齐 —— 那条注释原话就是
// 「仅防露白」,只是它来得太晚;当时仓里这个底色有**两个**字面量(tokens 与 FallbackPanel
// 各写一个),收成本常量之后 C++ 侧不再各写各的。
// ⚠ [SL-377] **「与 --page-backdrop 对齐」这句已经不成立**:那个变量现在是**外圈色**
// (外壳圆角之外那一圈,用户裁定改回深色 #191820),而本常量是**占位色**,两个角色取值不同。
// 别再拿它俩互相对拍 —— 判据也已按两角色重写(⑥/⑥c 钉占位色,⑥b 钉外圈色并断言两者不等)。
// **别在这里记「现在共有几处」**:记在注释里的数一定会漂(SL-355 就又添了一批,见下条)。
// 要找全落点就读 web-preview/tests/smoke-embedded-resources.mjs 的 **⑥ 与 ⑥c** —— 那两格逐处
// 对拍,它们读哪几个路径,占位底色就落在哪几处([SL-377] 起 ⑥b 读的是**外圈色**那条,
// 不是本常量的落点)。
// ⚠ 必须**完全不透明**:JUCE 的 withBackgroundColour 只接受全不透明或全透明(见其头注断言)。
//
// [SL-355] 更正上面「HTML 的底色藏在两个外链 css 里」那半句:现在三份 index.html 的
// <head> 里各内联了一条 `html { background-color: … }`,排在两条 <link rel="stylesheet">
// 之前,取值与本常量的低 24 位逐字相同(判据 = web-preview/tests/smoke-embedded-resources.mjs
// 的 ⑥,改一边不改另一边即红)。为什么必须写字面量而不是 var(--page-backdrop),以及
// 开窗那几段各自的来源与证据,只写在 src/plugin-common/WebViewHost.cpp 的
// HostWebView::paint 头注一处。
//
// [SL-370] 取值由深色 #191820 改成**浅色** —— SL-253/355 一路把这一层当「防露白的暗底」,
// 而成品首屏真正铺满窗口的是 .sc-shell 的浅色渐变(tokens.css 的 --page-gradient),于是
// 预绘的暗底自己变成了用户看见的那段黑(v5.6.8 实测「白→黑→白→内容」)。现在的取值 =
// --page-gradient 的渐变轴中点色,由上面那个 smoke 的 ⑥c 从渐变现算现对(⑥ 只对拍
// 几处彼此同值,对拍不出「和成品差了一整个明暗」)。
// ⚠ 本常量同时是 FallbackPanel 的面板底色:换浅色之后那三行标签必须是**深墨**才看得见,
// 判据 = tests/webview/test_plugin_common.cpp 的对比度断言。
//
// [SL-377] 本常量**不动**(仍是浅色占位),动的是 tokens.css 的 --page-backdrop:
// 用户裁定窗口四角(外壳圆角之外那一圈)改回深色。两者从此是两个角色,见上面 ⚠。
inline constexpr juce::uint32 kShellBackdropArgb = 0xffd9cadb;
inline juce::Colour shellBackdrop() noexcept
{
    return juce::Colour(kShellBackdropArgb);
}

// PlatformWebView —— 平台 WebView 分支集中地(01 §9;01 §6.1 机制 1/2 与机制 3 前半)。
//   Windows:WebView2(显式后端选择 + 可写 user-data 目录 + 静态 loader 运行时探测);
//   macOS/Linux:WKWebView / WebKitGTK(JUCE 内建,无需探测,恒可用)。
class PlatformWebView
{
public:
    // 把 Windows 专属的 WebView2 分支应用到 options(机制 1/2):
    //   显式选 webview2 后端(否则 JUCE 回退旧 IE ActiveX 控件,不支持 resource provider /
    //   native 集成,把 https://juce.backend/ 当真实网址导航 → "无法打开此页");
    //   user-data 目录用调用方经 makeUserDataFolder 取到的那一个(DAW 安装目录只读会导致
    //   WebView2 初始化失败,必须给可写目录)。目录由调用方持有,才进得了诊断面板。
    // 非 Windows 原样返回(走系统默认后端:WKWebView / WebKitGTK)。
    static juce::WebBrowserComponent::Options makeWebViewOptions(juce::WebBrowserComponent::Options options,
                                                                 const juce::File& userDataFolder);

    // user-data 目录的父目录(Windows = %LOCALAPPDATA%\Synchain\SCVB\WebView2)。
    static juce::File userDataFolderRoot();

    // 本插件(**不是本实例**)的 user-data 目录 = userDataFolderRoot()/userDataFolderName。
    // WebView2 的浏览器进程组按 UDF 共享 —— 固定目录才能复用进程组,这也是「热启动」判定
    // 得以成立的前提。完整理由见 .cpp 实现处。
    static juce::File makeUserDataFolder(const juce::String& userDataFolderName);

    // 本进程 PID(只进诊断行,便于与任务管理器对照)。非 Windows 返回 0。
    static int processId();

    // 建目录 + 写一个探针文件再删。返回空串 = 可写;否则是可直接进诊断面板的人话原因。
    // WebView2 自己碰这个目录时的失败被 JUCE 吞掉,所以必须我们先测一次。
    static juce::String probeUserDataFolder(const juce::File& folder);

    // 运行时三态(机制 3 前半)。「装了但太旧」必须与「没装」分开:两者的用户动作都是装
    // Evergreen Runtime,但太旧的机器上 WebView2 能创建、只是缺 JUCE 用到的接口,坐等看门狗
    // 超时会把它误报成「加载慢」,让用户白等一个完整预算窗口。
    enum class RuntimeStatus
    {
        ok,
        missing, // loader 报告本机没有任何 WebView2 Runtime
        tooOld // 有 Runtime,但主版本低于 kMinRuntimeMajor
    };

    struct RuntimeInfo
    {
        RuntimeStatus status = RuntimeStatus::missing;
        juce::String version; // loader 原样返回的版本串;missing 时为空
    };

    // WebView2 Runtime 主版本下限 = max(JUCE API 下限, 前端语法下限) = 86。
    //
    // ① **JUCE API 下限 = 86**。JUCE 8.0.8(仓库根 .juce-version)的
    //    juce_WebBrowserComponent_windows.cpp 硬依赖的最高 WebView2 接口是首发 GA 契约 ——
    //    ICoreWebView2 的 AddWebResourceRequestedFilter / AddScriptToExecuteOnDocumentCreated /
    //    WebMessageReceived(resource provider + native 集成的全部底座)与
    //    ICoreWebView2Environment::CreateCoreWebView2Controller,均随 WebView2 SDK 1.0.622.22 /
    //    Runtime **Edge 86** 首发。更高的 ICoreWebView2Controller2(默认背景色)与
    //    ICoreWebView2Settings2(UserAgent)JUCE 只经 QueryInterface 取、取不到就跳过
    //    (同文件 setWebViewPreferences),不构成下限。
    //
    // ② **前端语法下限 = 80**。全量扫 web/(T27-T36b 全部页面 + canvas + shared)后,用到的
    //    最新语法只有空值合并 ?? (ES2020 = Chromium 80)与 Array.prototype.flatMap
    //    (ES2019 = Chromium 69);**没有**可选链 ?.、顶层 await、类私有字段、static 初始化块、
    //    ??= / ||=、.at() / replaceAll / Object.hasOwn、正则 lookbehind。
    //    这条由 web-preview/tests/smoke-frontend-syntax-floor.mjs 持续把关 —— 前端一旦用了
    //    更新的语法,那套会红,提醒同批抬高本常量;否则旧 Runtime 上会是**整页 SyntaxError**
    //    (脚本一行都不执行、桥永不就绪),表现与「加载超时」一模一样,极难归因。
    //
    // 取二者较大值 86。低于它,resource provider / native 集成必然接不上,再等也不会好。
    static constexpr int kMinRuntimeMajor = 86;

    // Evergreen Runtime 引导下载页(missing / tooOld 两条分支共用)。
    static const char* runtimeDownloadUrl();

    // 运行时探测(Windows 走 WebView2 loader 的 GetAvailableCoreWebView2BrowserVersionString;
    // 非 Windows 恒 ok 且 version 为空)。
    static RuntimeInfo runtimeInfo();

    // 兼容旧调用点:等价于 runtimeInfo().status == ok。
    static bool runtimeAvailable();

    // 版本串 → 主版本号;解析不出返回 -1。纯函数(不碰 loader),便于离线单测。
    // loader 可能返回 "137.0.3296.83" 或带通道后缀的 "137.0.3296.83 dev",只取首段数字。
    static int majorVersionOf(const juce::String& version);

    // -------------------------------------------------------------------------
    // [SL-376 / SL-364] `DefaultBackgroundColor` 这一层到底在不在。
    //
    // 【为什么需要判】makeWebViewOptions 里的 withBackgroundColour(shellBackdrop()) 最终落到
    // JUCE 的 `WebView2::setWebViewPreferences`:它先
    // `webViewController->QueryInterface(ICoreWebView2Controller2)`,**取不到就静默跳过**
    // put_DefaultBackgroundColor(juce_WebBrowserComponent_windows.cpp,读实现核过 ——
    // 那个 `if (controller2 != nullptr)` 没有 else、没有日志、HRESULT 也不看)。
    // 于是「我方在控制器建好到首帧之间铺没铺上底色」这件事在真机上**完全不可观测**,
    // SL-364 就卡在这里。本函数把它变成一行可抓的诊断。
    //
    // 【它证到哪一步 —— 别读过头】它判的是**运行时有没有这个接口**,不是「JUCE 那次
    // QueryInterface 真的成功了」,更不是「那一帧屏上真是这个颜色」。插件侧拿不到 JUCE 私有的
    // controller,做不到直接观测;接口在场是 QueryInterface 成功的**必要条件**,而 IID 一旦
    // 随 SDK 发布就不再变,所以「运行时够新 ⇒ 接口在」这一步成立,反向不成立。
    //
    // 【纯函数】只吃 RuntimeInfo,便于离线单测(真 loader 与真 WebView2 都够不着)。
    enum class BackgroundColourSupport
    {
        available, // 运行时够新 ⇒ ICoreWebView2Controller2 在 ⇒ JUCE 那句不会静默跳过
        unavailable, // 运行时太旧 ⇒ 接口不在 ⇒ 这一层整层缺席,控制器建好到首帧之间露的是白
        unknown // 没探到运行时 / 版本串解析不出 ⇒ 不猜,如实说不知道
    };

    // ICoreWebView2Controller2(即 DefaultBackgroundColor)的运行时主版本下限。
    //
    // ⚠ **这个数字是本条判定里唯一没有机检、也无法离线核实的一环**:它来自该接口首发的
    // WebView2 SDK 1.0.774.44 所对应的 Edge 通道(87),而仓里没有任何东西能把这条映射钉住。
    // 影响面被两头夹得很小:kMinRuntimeMajor = 86 已经把更低的运行时挡在兜底面板之后,
    // 而 Evergreen Runtime 会自动升级,现实中不存在停在 86/87 这一档的机器。判错也只影响
    // 这一行诊断的措辞,不改变任何行为。
    static constexpr int kBackgroundColourMinRuntimeMajor = 87;

    static BackgroundColourSupport backgroundColourSupport(const RuntimeInfo& info);
};

} // namespace scvb::webview
