// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <cmath>
#include <cstddef>

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
// (外壳圆角之外那一圈,用户裁定改回深色 #191820),而本文件这组常量是**占位**,两个角色
// 取值不同。别再拿它俩互相对拍 —— 判据也已按两角色重写(⑥/⑥c 钉占位那一族,⑥b 钉外圈色
// 并断言它与占位那一族不等)。
// **别在这里记「现在共有几处」**:记在注释里的数一定会漂(SL-355 就又添了一批,见下条)。
// 要找全落点就读 web-preview/tests/smoke-embedded-resources.mjs 的 **⑥ 与 ⑥c** —— 那两格逐处
// 对拍,它们读哪几个路径,占位底就落在哪几处([SL-377] 起 ⑥b 读的是**外圈色**那条,
// 不是本文件这组常量的落点)。
//
// [SL-355] 更正上面「HTML 的底色藏在两个外链 css 里」那半句:三份 index.html 的 <head> 里
// 各内联了一条根元素底色声明,排在两条 <link rel="stylesheet"> 之前。为什么必须写字面量
// 而不是 var(--page-backdrop),以及开窗那几段各自的来源与证据,只写在
// src/plugin-common/WebViewHost.cpp 的 HostWebView::paint 头注一处。
//
// [SL-370] 取值由深色 #191820 改成**浅色** —— SL-253/355 一路把这一层当「防露白的暗底」,
// 而成品首屏真正铺满窗口的是 .sc-shell 的浅色渐变(tokens.css 的 --page-gradient),于是
// 预绘的暗底自己变成了用户看见的那段黑(v5.6.8 实测「白→黑→白→内容」)。当时的取值 =
// --page-gradient 的渐变轴中点色,由那个 smoke 的 ⑥c 从渐变现算现对(⑥ 只对拍几处彼此
// 同值,对拍不出「和成品差了一整个明暗」)。
// ⚠ 本常量当时同时是 FallbackPanel 的面板底色:换浅色之后那三行标签必须是**深墨**才看得见。
//
// [SL-377] 占位那一份**不动**(仍是浅色),动的是 tokens.css 的 --page-backdrop:
// 用户裁定窗口四角(外壳圆角之外那一圈)改回深色。两者从此是两个角色,见上面 ⚠。
//
// [SL-402] **占位从单色升级为渐变**(第五代):单色占位与开窗后可见的成品底
// (.sc-shell 的 --page-gradient 渐变)在明暗上仍有一段差 —— 用户报「加载中占位没有深浅,
// 与打开后的背景有别」。修法 = 占位改画**同一组色标的线性渐变**(tokens.css 的
// --page-gradient 是真源):C++ 侧本文件的 kShellBackdropStops(下面 shellBackdropGradient()
// 按 CSS linear-gradient 的几何复刻)、三份 index.html 的 <head> 内联(现在是 linear-gradient
// 字面量,判据 ⑥)与 tokens.css 三处逐字对拍,改任一处即红。两个连带:
//   · WebView2 的 DefaultBackgroundColor 只收**纯色**(①-b 层,可能整层缺席),它取
//     shellBackdropMid()(= 色标数组沿轴 50% 的插值色,现值 #d9cadb),与渐变占位不跳阶;
//   · FallbackPanel 的面板底**从此不再与本文件同源**(SL-253 收编的那条性质由 SL-402 拆开):
//     占位已是渐变,面板底仍是一块纯色,取值留在 FallbackPanel.h 自己的 kFallbackPanelArgb
//     (注释写明它是 SL-402 当时的中点色记录,不是跨文件真源)。
//
// ⚠ 全部色标必须**完全不透明**:JUCE 的 withBackgroundColour 只接受全不透明或全透明
// (见其头注断言);ColourGradient 的停靠点同理取自同一组全不透明常量。
struct ShellBackdropStop
{
    float pos; // 沿渐变轴的比例 0..1(CSS 停靠点写的 n% / 100)
    juce::uint32 argb; // 全不透明(0xAARRGGBB)
};

// [SL-402 · 第 1 推] 渐变角度(度)—— 与色标数组并列的**命名空间级常量**。CSS
// linear-gradient 的 0deg = 向上、顺时针;tokens.css 的 --page-gradient 与三份 index.html
// 内联写的角度都要与本常量逐字对拍(⑥/⑥c 各有一条角度断言):「与成品同形」这条性质
// 包括**走向** —— 只对拍色标表时,角度漂了(157 写成 156)判据照绿,第 1 推把这个缺口钉上。
inline constexpr float kShellBackdropAngleDeg = 157.0f;

// [SL-402] 占位渐变的**色标数组**(C++ 侧单一真源)—— 逐字照抄 tokens.css
// `--page-gradient` 的四个停靠点(角度见上面的 kShellBackdropAngleDeg;改 tokens 一处,
// 这里与三份 index.html 的内联必须同批改,⑥/⑥c 会对拍出漏改的那一处)。
// pos 用 0..1 的小数(CSS 的 n% / 100)。
inline constexpr ShellBackdropStop kShellBackdropStops[] = {
    {0.00f, 0xffb5acc9}, // #b5acc9   0% ← tokens.css --page-gradient 第 1 停靠点
    {0.32f, 0xffccbfd5}, // #ccbfd5  32%
    {0.64f, 0xffe3d2e0}, // #e3d2e0  64%
    {1.00f, 0xfffde8ed}, // #fde8ed 100%
};

// [SL-402] 占位渐变沿轴 **50% 处的插值色**(现值 #d9cadb)—— 只喂给收**纯色**的两处:
// WebView2 的 DefaultBackgroundColor(①-b 层,PlatformWebView::makeWebViewOptions)与
// 诊断行;也是 shellBackdropGradient() 空矩形守卫的落点(见下)。与 smoke ⑥b/⑥c 的
// 中点公式同一条(50% 落在 [pos_i, pos_{i+1}] 段内线性插值)。
// FallbackPanel **不**走这里(见上,SL-402 起面板底不再与占位同源)。
// ⚠ 定义必须在 shellBackdropGradient() **之前**(它的空矩形守卫要调本函数)。
inline juce::Colour shellBackdropMid()
{
    constexpr auto count = sizeof(kShellBackdropStops) / sizeof(kShellBackdropStops[0]);
    for (std::size_t i = 0; i + 1 < count; ++i)
    {
        const auto& a = kShellBackdropStops[i];
        const auto& b = kShellBackdropStops[i + 1];
        // [第 1 推] 命中条件带 `b.pos > a.pos`:相邻停靠点写成同一 pos(CSS 里合法的硬边界
        // 写法)时,b.pos - a.pos == 0 ⇒ t 为 inf/NaN。宁可落到末尾的 fail-closed 首色,
        // 也不算出一个 NaN(裁定:两句守卫之一,不加判据、不改现值)。
        if (a.pos <= 0.5f && 0.5f <= b.pos && b.pos > a.pos)
        {
            const float t = (0.5f - a.pos) / (b.pos - a.pos);
            const auto mix = [t](juce::uint32 ca, juce::uint32 cb, int shift) -> juce::uint8 {
                const auto va = static_cast<float>((ca >> shift) & 0xffu);
                const auto vb = static_cast<float>((cb >> shift) & 0xffu);
                return static_cast<juce::uint8>(juce::roundToInt(va + t * (vb - va)));
            };
            // ⚠ juce::Colour 四参构造是 (red, green, blue, alpha) —— alpha 在**最后**;
            // alpha 通道同样插值:四个停靠点全不透明 ⇒ 结果恒 0xff,顺带守住「全不透明」前提。
            return juce::Colour(mix(a.argb, b.argb, 16), mix(a.argb, b.argb, 8), mix(a.argb, b.argb, 0),
                                mix(a.argb, b.argb, 24));
        }
    }
    return juce::Colour(kShellBackdropStops[0].argb); // 解析不出段时 fail-closed 取首色
}

// 占位渐变(在 `area` 上复刻 CSS `linear-gradient(157deg, …)` 的几何;角度真源 =
// 上面的 kShellBackdropAngleDeg):
//   · 0deg 指向上、顺时针,方向向量(x 右,y 下)= (sin θ, −cos θ);
//   · 渐变线过矩形中心,线长 = |W·sinθ| + |H·cosθ|,首末停靠点各落在线的两端。
// 这三条是 CSS 规范对 linear-gradient 的定义,不是近似;JUCE 的 ColourGradient 沿 p1→p2
// 插值、两端外侧延展首末色,与 CSS 的行为同形。smoke 的 ⑥/⑥c 只对拍**色标表与角度**,
// 几何两侧各自照规范实现,不互相对拍。
inline juce::ColourGradient shellBackdropGradient(juce::Rectangle<float> area)
{
    if (area.isEmpty())
    {
        // [第 1 推] 空矩形:len == 0 ⇒ 渐变线两端重合,ColourGradient 在 p1==p2 上是
        // 未定义行为面。走中点色的一像素垂直渐变(两端同色 ⇒ 视觉等同单色)。现行两个
        // 调用点都来自 paint() 的 getLocalBounds(),不会是空盒 —— 纯防御面(裁定:不加
        // 判据、不改现值)。⚠ 本仓 JUCE(.juce-version)的 vertical() 工厂签名是
        // (colour1, y1, colour2, y2) 四参,读 juce_ColourGradient.h:106 核过。
        const auto mid = shellBackdropMid();
        return juce::ColourGradient::vertical(mid, 0.0f, mid, 1.0f);
    }
    const auto rad = juce::degreesToRadians(kShellBackdropAngleDeg);
    const auto sinA = std::sin(rad);
    const auto cosA = std::cos(rad);
    const auto len = std::abs(area.getWidth() * sinA) + std::abs(area.getHeight() * cosA);
    const auto centre = area.getCentre();
    const juce::Point<float> dir{sinA, -cosA};
    const auto start = centre - dir * (len * 0.5f);
    const auto end = centre + dir * (len * 0.5f);

    constexpr auto count = sizeof(kShellBackdropStops) / sizeof(kShellBackdropStops[0]);
    juce::ColourGradient gradient{juce::Colour(kShellBackdropStops[0].argb),
                                  start.x,
                                  start.y,
                                  juce::Colour(kShellBackdropStops[count - 1].argb),
                                  end.x,
                                  end.y,
                                  false};
    for (std::size_t i = 1; i + 1 < count; ++i)
        gradient.addColour(kShellBackdropStops[i].pos, juce::Colour(kShellBackdropStops[i].argb));
    return gradient;
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
    // 【为什么需要判】makeWebViewOptions 里的 withBackgroundColour(shellBackdropMid()) 最终落到
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
