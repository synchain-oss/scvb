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
    //   user-data 目录用调用方经 makeUserDataFolder 取到的那一个(DAW 安装目录只读会导致
    //   WebView2 初始化失败,必须给可写目录)。目录由调用方持有,才进得了诊断面板。
    // 非 Windows 原样返回(走系统默认后端:WKWebView / WebKitGTK)。
    static juce::WebBrowserComponent::Options makeWebViewOptions(juce::WebBrowserComponent::Options options,
                                                                 const juce::File& userDataFolder);

    // user-data 目录的父目录(Windows = %LOCALAPPDATA%\Synchain\SCVB\WebView2)。
    static juce::File userDataFolderRoot();

    // 本实例专属的 user-data 目录:名字带 **PID** + 进程内序号。跨进程唯一是硬要求 ——
    // 宿主的插件扫描 sandbox 进程与音频进程会同时活着,共用一个 UDF 会让 WebView2 环境创建
    // 失败,而 JUCE 把那个 HRESULT 吞掉,症状正是「空白窗口 + 超时 + 重试无效」。
    static juce::File makeUserDataFolder(const juce::String& userDataFolderName);

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
};

} // namespace scvb::webview
