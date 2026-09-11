// SPDX-License-Identifier: GPL-3.0-or-later
#include "WebViewHost.h"

#include "BridgeBase.h"
#include "PlatformWebView.h"

#include <type_traits>
#include <utility>

// [SL-271] 品红探针。开成 1 后 HostWebView::paint 铺品红而不是 shellBackdrop —— 真机上一眼
// 能分出「白闪没了是因为我们这层 paint 真的在跑」还是「WebView2 这次恰好起得快」。
// 默认关,只经 -DSCVB_PROBE_MAGENTA=1 临时开;交付构建里它必须是 0。
#ifndef SCVB_PROBE_MAGENTA
#define SCVB_PROBE_MAGENTA 0
#endif

namespace scvb::webview
{

using WBC = juce::WebBrowserComponent;

namespace
{
juce::String fallbackTitle(const juce::String& role)
{
    // 三角色各自成句。漏掉 monitor 时它的降级面板会自称「SCVB Output」——
    // 用户开不了窗时看到的正是这张面板,标题指错插件会把排查引到另一个插件上去。
    if (role == "input")
        return "SCVB Input";
    if (role == "monitor")
        return "SCVB Monitor";
    return "SCVB Output";
}

juce::String missingRuntimeMessage()
{
    return "Microsoft Edge WebView2 Runtime was not found, so the full UI cannot load.\n"
           "Install the runtime once, then reopen this plugin window.";
}

juce::String tooOldRuntimeMessage()
{
    return "The installed Microsoft Edge WebView2 Runtime is too old for this plugin.\n"
           "Update to the Evergreen runtime, then reopen this plugin window.";
}

// 超时兜底按「最后一次导航走到哪一步」分三态给文案。这是本面板最重要的信息:
// 同一句「加载太慢」在三种完全不同的故障上都会出现,而这三种的下一步动作互不相同。
//   notStarted —— 导航事件一次都没来 ⇒ WebView2 **环境/控制器根本没建起来**。
//                 JUCE 把环境创建与控制器创建的 HRESULT 都吞了(回调里 HRESULT 形参无名,
//                 juce_WebBrowserComponent_windows.cpp),所以「没有事件」是我们唯一能拿到的
//                 信号 —— 也正因如此它很可靠:环境起来了就必然有 NavigationStarting。
//                 常见成因:user-data 目录不可写 / 被别的进程占着(宿主的插件扫描 sandbox
//                 进程与音频进程同时活着)/ 宿主策略挡掉 msedgewebview2.exe 子进程。
//   started    —— 导航开始但没走完 ⇒ 资源供给或渲染进程卡住。
//   finished   —— 页面加载完了桥仍没起来 ⇒ 前端脚本没跑到 requestInitialState。
juce::String envNotStartedMessage()
{
    return "The WebView2 environment did not start (no navigation ever began).\n"
           "This is usually the user-data folder being unwritable or already in use by another\n"
           "process, or the host blocking the msedgewebview2.exe child process.";
}

juce::String navStalledMessage()
{
    return "The page started loading but never finished.\n"
           "Click Retry, or close and reopen this plugin window.";
}

juce::String bridgeStalledMessage()
{
    return "The page loaded, but the plugin UI never finished starting up.\n"
           "Click Retry; if it keeps failing, report the diagnostics line below.";
}

juce::String bootErrorMessage()
{
    return "The plugin UI failed to start (front-end script error).\n"
           "Click Retry; if it keeps failing, report the diagnostics line below.";
}

// **当前**桥已就绪的编辑器数量(进程内)。热预算的判据只能是「此刻确有一个活着的桥」,
// 不能是「历史上曾经起来过」:
//   • WebView2 的浏览器进程组按 user-data 目录共享(见 PlatformWebView::makeUserDataFolder),
//     只要还有一个实例活着,进程组就在,新开的编辑器只是往里加一个 WebView,5s 绰绰有余;
//   • 可一旦所有编辑器都关掉,浏览器进程组会退出,再开就是**完整冷启动** —— 此时若因
//     「曾经起来过」而只给 5s,等于亲手造出「关窗再开必超时」这个新故障。
// 故用计数而非 bool:>0 才算热。
std::atomic<int>& readyBridgeCount()
{
    static std::atomic<int> count{0};
    return count;
}
} // namespace

// -----------------------------------------------------------------------------
// HostWebView —— 只为拿到 JUCE 的三个页面回调而存在的薄子类(01 §6.1 机制 3)。
// 看门狗要「收到导航事件才起算」,而 pageAboutToLoad / pageFinishedLoading /
// pageLoadHadNetworkError 是 JUCE 唯一暴露导航时序的接口,只能经继承取得。
// -----------------------------------------------------------------------------
class WebViewHost::HostWebView final : public juce::WebBrowserComponent
{
public:
    HostWebView(WebViewHost& owner, Options options) : juce::WebBrowserComponent(std::move(options)), owner_(owner) {}

    // -------------------------------------------------------------------------
    // [SL-271] 开窗白闪的**真正**那一层。
    //
    // SL-253 铺了三层暗色,用户仍看得见白闪,原因是三层没有一层管到这一段:
    //   ① WebViewHost::paint —— 从不执行。本组件是 WebBrowserComponent,它在构造里
    //      setOpaque(true)(juce_WebBrowserComponent.cpp:590),又铺满父组件;JUCE 画父组件
    //      前会把不透明子组件的矩形从裁剪区里剔掉(detail/juce_ComponentHelpers.h 的
    //      clipObscuredRegions:isOpaque() ⇒ excludeClipRegion)。父的 fillAll 净效果为 0。
    //   ② WebView2 的 DefaultBackgroundColor —— 要等**控制器建好**才有意义,而白闪正是
    //      控制器建好之前那一段。
    //   ③ 页面自己的底色 —— 更晚,要等页面解析。(SL-253 当时这一层是 tokens.css 的
    //      --page-backdrop;[SL-355] 之后由三份 index.html <head> 里那条内联的 html 底承担,
    //      而 [SL-377] 起 --page-backdrop 已改作**外圈色**,不再是这条链上的一环。)
    // 而这一段真正在画的是 JUCE 自己:WebBrowserComponent::paint 转给后端的 fallbackPaint,
    // WebView2 后端那份**每帧无条件** fillAll(Colours::white)
    // (juce_WebBrowserComponent_windows.cpp:492)。白的来源就是它。
    //
    // 修法:**先调基类,再把自己的底盖上去**。两次 fillAll 落在同一个 Graphics、同一次
    // paint 里,先白后 shellBackdrop(),中间不上屏 —— Windows 侧两条渲染路都是整帧画完才出:软件渲染
    // 画进 offscreenImage 再 blit(juce_Windowing_windows.cpp:4907),Direct2D 走
    // startFrame/endFrame 成对包住整次 paint(同文件 :5144-5145)。上屏的只有后盖的那一层。
    // WebViewHost::paint 保留不动:兜底面板路径下 webView_ 被 setVisible(false),
    // 那时父组件的 fillAll 才真的会画。
    //
    // ⚠ 为什么**不是**「覆写且不调基类」(PR 178 复审【重要】1 纠正的正是这一版):
    //   基类的 fallbackPaint 不只是画白 —— 它尾巴上那句
    //   `if (! hasBrowserBeenCreated()) checkWindowAssociation();`
    //   (juce_WebBrowserComponent_windows.cpp:496-497)是 WebView2 控制器的**重试泵**:
    //   createWebView() 在 peer 为空时直接 return,而我们在构造期就 goToURL(本文件的
    //   beginLoadAttempt),那一刻 peer 还没有。控制器能不能建起来,全靠此后一次次重入
    //   checkWindowAssociation。绕过基类 = 把泵拆了。
    //   ⚠ 这里**刻意不写「窗永远开不出来」**:那是前几轮流传过、而本卡实测**证伪**了的
    //   绝对说法。实测(见下方 static_assert 旁记录)是:删掉基类调用后,pluginval 全量
    //   含 GUI 三个 bundle 照样全 PASS、窗开得出来 —— JUCE 自己的 parentHierarchyChanged()
    //   / visibilityChanged() 已足够把控制器建起来。paint 里这个泵兜的是「那两条事件路径
    //   都赶在 peer 就绪之前跑完」这一类时序,pluginval 的开窗时序撞不出来,真实宿主
    //   (窗口延迟创建、宿主自己的插件扫描沙箱进程)更容易撞上。
    //   所以拆掉它是**真机上的开窗风险**,不是「必然开不出来」——而这条风险没有任何
    //   机检兜得住(缺口登记 SL-282)。
    //   而在本层**复刻**一份泵同样不行(下面这两条说的是**那个被否掉的方案**,不是现状):
    //     • 复刻件只能押在 JUCE 的私有实现细节上(基类的条件 `hasBrowserBeenCreated()`
    //       与 `visibilityChanged() == impl->checkWindowAssociation()` 都不是公开契约),
    //       `.juce-version` 一升就可能静默变成空调用;
    //     • 而 static_assert 守不到「那份复刻还在」—— 删掉它断言照旧全绿,本文件又不进
    //       任何测试目标,于是它会无声地烂掉。
    //   调基类之后没有可删的复刻件:泵连同它的条件一字未动留在 JUCE 里。
    //   ⚠ 但**别把这读成「守住 override 就守住了全部」**:上面那句
    //   `juce::WebBrowserComponent::paint(g);` 删掉照样编得过、断言照样绿,泵一样会没
    //   —— 详见 static_assert 旁的实测记录。差别在**必守面从两处减到一处、且那一处显式**:
    //   基类签名一变就编译红,不会像复刻件那样静默失效。
    //   代价只是每次 paint 多一次纯色 fillAll(paint 本就极低频)。
    //
    // -------------------------------------------------------------------------
    // [SL-355] 开窗那几帧的**分层地图**(v5.6.7 真机反馈:「先灰、然后全白、然后才出来」)。
    // 这份地图**只写在这里一份**,web 侧三个 index.html 与 smoke-embedded-resources.mjs ⑥
    // 都只留一句指路,不复述。
    //
    // 【灰】不是我方任何一层画的。三条证据:
    //   • JUCE 注册插件子窗口类时 `WNDCLASSEX wcex = {};`(juce_Windowing_windows.cpp 的
    //     WindowClassHolder)⇒ hbrBackground 为 0,系统不会拿系统色去擦背景;
    //   • 同文件 windowProc 的 `case WM_ERASEBKGND: if (hasTitleBar()) break; return 1;`
    //     ⇒ 无标题栏的窗口连默认擦除都不走,而 VST3 编辑器正是这一类:
    //     detail::PluginUtilities::getDesktopFlags 只可能给出 0 或
    //     windowRequiresSynchronousCoreGraphicsRendering,两者都不含 windowHasTitleBar;
    //   • (SL-355 当时)我方三层底色都是 shellBackdrop() = kShellBackdropArgb,那时它是暗色,
    //     画不出浅灰。⚠ [SL-370] **这第三条证据已经不成立**:kShellBackdropArgb 现在是浅色
    //     #d9cadb,和「浅灰」在肉眼上分不开。前两条(hbrBackground 为 0、WM_ERASEBKGND 不走)
    //     不依赖取值,仍然成立,所以结论没变;但从今往后**不能再拿颜色去区分**第一段是宿主的
    //     还是我方的 —— 而那正是本卡想要的结果(两边都浅 ⇒ 用户看不出交接)。
    //   ⇒ 浅灰只可能来自**宿主自己的插件窗容器** —— 它在我们的 HWND 上屏之前就在那儿。
    //   插件侧无从覆盖。**这一条只有真机能最终确认**(各 DAW 的容器底色不同)。
    //
    // 【白】分三节,前两节此前已堵,第三节是 SL-355 补的:
    //   ①-a 控制器建起来**之前** —— 就是本函数上面讲的那一层(JUCE 的 fallbackPaint
    //        每帧无条件 fillAll(Colours::white)),由本 override 先调基类再整块盖住。
    //   ①-b 控制器建好、文档还没提交 —— WebView2 的 DefaultBackgroundColor,由
    //        PlatformWebView::makeWebViewOptions 的 withBackgroundColour(shellBackdrop())
    //        铺上([SL-253];判据在 tests/webview/test_plugin_common.cpp)。
    //        ⚠ **这一层可能整层不在**:JUCE 是
    //        `webViewController->QueryInterface(controller2...)` 后 `if (controller2 != nullptr)`
    //        才 put_DefaultBackgroundColor(juce 的 WebView2::setWebViewPreferences),
    //        取不到就静默跳过;而 PlatformWebView.h 的 kMinRuntimeMajor 注释①自己就把
    //        ICoreWebView2Controller2 归为「只经 QueryInterface 取、取不到就跳过、不构成
    //        下限」的那一类。取不到时,这一节露的就是 WebView2 自己的默认白。
    //        [SL-376] 这一条从「无从观测」变成**每次加载尝试打一行**:
    //        `webview2 default background: available|UNAVAILABLE|unknown ...`
    //        (WebViewHost::logBackgroundColourSupport,判定见 PlatformWebView.h 的
    //        backgroundColourSupport 头注 —— 它证的是「运行时有没有这个接口」,**不是**
    //        「JUCE 那次 QueryInterface 真成功了」,别读过头)。
    //   ①-c 文档已提交、外链 css 还没到 —— 页面自己没有任何底色,露的是 ①-b
    //        (①-b 缺席时就是白)。tokens.css 与 base.css 各要经一次 ResourceProvider 的
    //        WebResourceRequested 回到消息线程才拿得到。[SL-355] 因此在三份 index.html 的
    //        <head> 里内联一条 `html { background-color: … }`,排在两条
    //        <link rel="stylesheet"> 之前;判据 = web-preview/tests/smoke-embedded-resources.mjs
    //        的 ⑥。
    //        ⚠ **「排在外链之前」是排序事实,不是时序保证** —— 别把它读成「已经堵住」。
    //        Chromium 对 <head> 里的 <link rel="stylesheet"> 是**渲染阻塞**的:外链的 CSSOM
    //        就绪之前文档整体不进正常绘制路径,那一段屏上仍然是视图的 base background color
    //        (即 ①-b)。所以这条内联声明**确定**兜住的只有两条路:
    //          · 外链**取不到 / 加载失败** —— 阻塞随之解除,画出来的是这条内联底色而不是白。
    //            本仓栽过三次的「web 资源没进包 ⇒ 空白窗口」正是这一类;
    //          · 外链到达之后稳态零差异(所以它无副作用)。⚠ [SL-377] **理由变了**:
    //            SL-355 当时的理由是「与 base.css 的 body 底色同值」,而现在两者是两个角色、
    //            **取值不同**(内联的 html 底 = 占位色 #d9cadb;body 底 = 外圈色 #191820)。
    //            现在成立的理由是**覆盖**而不是同值:base.css 的 `html, body { height: 100% }`
    //            让 body 盒铺满视口,深色 body 底整块盖在 html 画布底之上,稳态看不见接缝。
    //            (`--page-backdrop` 全仓只有 base.css 的 body 一个消费者。)
    //        而「正常路径上它到底缩不缩得短那段白」取决于 Blink 在阻塞期间用不用根元素样式,
    //        本机没有任何手段能验证 —— **只有真机能判**,验收步骤见 PR #234 描述。
    //   还有一节在我们的 API 之外:WebView2 runtime 自己那个宿主 HWND,在首帧合成之前由
    //   runtime 画,JUCE 不暴露它(只在 createWebView 里遍历子窗口找到后交给
    //   AccessibilityHandler::setNativeChildForComponent)。它是不是白闪的剩余来源,
    //   **只有真机能判**。
    //
    // -------------------------------------------------------------------------
    // [SL-370] v5.6.8 真机反馈:「先白然后黑然后再白,最后内容,每次打开都固定复现,
    // 冷热启动没区别」。上面那张地图没错,错的是它铺的**颜色**。
    //
    // 【定谳】成品首屏真正铺满窗口的是 `.sc-shell` 的 `--page-gradient`(浅色玻璃拟态;
    //   web/shared/base.css 是这个 token 的唯一消费者;`#card` 的宽高 = 设计盒,而窗口尺寸
    //   由 resizeToDesignBox 按同一个设计盒设,所以外壳几乎铺满窗口,只有它的圆角之外那一圈
    //   露出 --page-backdrop)。
    //   而 SL-253/271/355 一路铺的预绘底色是深色 #191820 —— 也就是说**那段黑是我们自己画的**。
    //
    // 【三段各自的归因】
    //   · 白(第一段)= 同上面【灰】那一节:宿主自己的插件窗容器,在我方 HWND 上屏之前。
    //     插件侧无从覆盖;v5.6.7 用户读成「灰」、v5.6.8 读成「白」,都是这一节(各 DAW/主题不同)。
    //   · 黑(第二段)= 我方 ①-a/①-b/①-c 三层预绘底色,取值 kShellBackdropArgb。
    //     **这是一条排除法结论,不是猜**:开窗路径上我方只有这一个深色值 —— JUCE 的
    //     fallbackPaint 画白、外壳与 body 稳态都走浅色渐变、宿主容器是浅的,窗口里能出现
    //     一整块黑的来源只剩它。(全仓求证:预绘底色的全部落点 = ⑥ 与 ⑥c 读的那几个路径,
    //     见 web-preview/tests/smoke-embedded-resources.mjs;[SL-377] 起 ⑥b 读的是外圈色,
    //     不在这条链上。)
    //   · 白(第三段)= 我方三层**盖不到**的那一节,即上面已登记的两条:①-b 缺席时
    //     (JUCE 用 QueryInterface 取 ICoreWebView2Controller2,取不到就静默跳过)露出的
    //     WebView2 默认白,以及 runtime 自己那个宿主 HWND 首帧之前的那一段。
    //     ⚠ 顺序上它必然排在黑之后:JUCE 在控制器建好的完成回调里是先 addEventHandlers()
    //     + setWebViewPreferences()(这里面才 put_DefaultBackgroundColor)、**再** Navigate
    //     (juce_WebBrowserComponent_windows.cpp 的 createWebView 完成回调),所以 ①-b 一旦
    //     在场就从控制器建好一路管到首帧;看得见白 ⇒ 这一层不在(或白来自 runtime 的 HWND)。
    //     **插件侧没有 API 能盖它**,SL-370 也没有新增手段 —— 只有真机能判。
    //
    // 【SL-370 的修法 · 第一段:颜色(兜底)】把预绘底色(tokens.css 的 --page-backdrop /
    //   三份 index.html 的 <head> 内联 / 本文件用的 kShellBackdropArgb,SL-370 当时这三处同源)
    //   **整体换成浅色**:取 `--page-gradient` 的渐变轴中点色,现值 #d9cadb(本渐变四段斜率
    //   几乎一致,取整后它与「沿轴等权均值」同为 #d9cadb;⑥c 判的是**中点色**这一条,不判均值)。
    //   黑那一段就此彻底拿掉。
    //   ⚠ 连带面:kShellBackdropArgb 同时是 FallbackPanel 的面板底色,三行标签因此从浅字
    //   改成深墨(判据 = tests/webview/test_plugin_common.cpp 的对比度断言)。
    //   ⚠ [SL-377] **上面那个「三处同源」已经拆成两个角色**:用户裁定窗口四角
    //   (= --page-backdrop,外壳圆角之外那一圈)**改回深色 #191820**,而占位色
    //   (kShellBackdropArgb + 三份 index.html 内联)保持浅色不动。SL-370 时四角跟着变浅是
    //   顺带效果、当时留给用户终验,终验结论就是这一条。
    //   机检:⑥ 保证占位色那几处彼此同值,⑥c 从 --page-gradient 现算现对「和成品可见底色
    //   是不是一个明暗」,⑥b 单独钉外圈色(对拍设计稿 body 底,并断言它 != 占位色)。
    //
    // 【SL-370 的修法 · 第二段:根本不让 WebView 上屏(主修法)】统筹裁定(#241 评论):
    //   **光换颜色盖不住第三段白**。用户机的 Runtime 是 152.0.4191.66,ICoreWebView2Controller2
    //   与 DefaultBackgroundColor 全都可用(即 ①-b 这一层确定在场、且已经是浅色),仍然固定
    //   看得见白 ⇒ 那几帧白不由我方任何一层底色决定,而是 WebView2 自己那个宿主 HWND 在合成
    //   首帧之前画的,插件侧没有 API 管得到它的颜色。
    //   于是改成管**它在不在屏上**:导航开始 → 页面「首帧已绘」之间,把 WebView 子窗口整块
    //   挪到宿主客户区之外,那块地方由 WebViewHost::paint 铺 shellBackdrop() 当占位;
    //   SL-370 当时是三条放行路(前端 __scvb__firstFrame / pageFinishedLoading / 3s 超时)
    //   谁先到算谁。判定收在 WebViewRevealGate.h(纯逻辑、有单测),**为什么是「挪走」而不是
    //   setVisible(false) 或零尺寸、为什么必须等到导航开始**,只写在那份头注一处,别在这里复述。
    //   ⇒ 目标序列:「白(宿主容器)→ 浅色占位 → 内容」。
    //   ⚠ 仍然只有真机能判:①-b 之前(控制器建好那一瞬)那几帧不在闸门覆盖范围内 ——
    //   闸门要等 NavigationStarting 才动,而那之前 WebView2 已经建好控制器。这一段是否还看得见
    //   白,20 次开关的真机验收说了算;真看得见,下一步只能往「控制器建好之前先零尺寸」走,
    //   而那条路会拆掉 SL-271 的重试泵(风险见下面【评估过、本卡没做的那条】)。
    //
    // -------------------------------------------------------------------------
    // [SL-376] v5.6.10 真机反馈:「粉色占位 → **白一瞬** → 内容」,黑已消、冷热无差。
    //   ⇒ SL-370 的两段修法都生效了,剩下的白落在**放行之后、页面首帧上屏之前**。
    //   定谳与修法(放行只认 firstFrame + 信号到后再压一拍:tick 数 ∧ 32 ms 毫秒下界)只写在
    //   WebViewRevealGate.h 头注一处,这里不复述。这里只记它对上面那张地图的影响:
    //   目标序列变成「白(宿主容器)→ 粉色占位 → 内容」,占位段从「首帧信号与 load 事件
    //   竞速的结果」变成「**正常路上**一定等到首帧已合成并上屏」。
    //   ⚠ [SL-378] 「一定」两个字只覆盖**正常路**,别读成全称:首帧信号缺席时 `onTick` 的
    //   timeout 路(RevealGate::kRevealFallbackMs,3s)照样放行 —— 那是「绝不允许永远不放行」
    //   的兜底,不是漏网之鱼。命中它的那一次开窗露的是 WebView2 宿主自己的底,形态、真机
    //   指标(放行原因里出现 `timeout`)与出路写在 WebViewRevealGate.h 头注那一节,这里不复述。
    //   `kRevealSettleMs`(32 ms)同理:它是「一个 60Hz 合成帧再加约一帧余量」的**下界近似**,
    //   **不是**对「已经上屏」的观测 —— 观测在真机,本机量不到。
    // -------------------------------------------------------------------------
    //
    // 【SL-355 评估过、当时没做的那条】「控制器建好 / 首帧到达之前先 setVisible(false)」。
    // ⚠ [SL-370] 这一段仍然成立、且**仍然是被否掉的那个方案** —— 本卡做的不是它:
    //   本卡挪的是 **bounds**(可见性一字未动、`owner.isShowing()` 恒真),而且要等
    //   **导航开始之后**才挪(那时控制器已建好,泵本来就是空调用)。下面这三条针对的是
    //   「隐藏 / 提前隐藏」,不要读成对本卡实现的描述;逐条对照见 WebViewRevealGate.h 头注。
    //   • 机制上不是死路:checkWindowAssociation 末尾那句
    //     `if (! hasBrowserBeenCreated()) createBrowser();` 不看 isShowing,
    //     而 componentVisibilityChanged 会在我们改回 setVisible(true) 时补一次 put_IsVisible。
    //   • 但风险恰好落在**开窗路径本身**:隐藏期间本 paint 不会被调用 ⇒ 上面那个重试泵
    //     整段消失;而 createWebView 在 `getPeer()` 为空时直接 return,我们又恰恰在构造期
    //     就 goToURL(见 beginLoadAttempt),那一刻还没有 peer。于是只剩
    //     parentHierarchyChanged / visibilityChanged 两条路,它们若都赶在 peer 就绪之前跑完,
    //     控制器就再也建不起来 —— 而我们还在等「首帧」才 setVisible(true),互相等死,
    //     表现是 15s 后兜底面板。
    //   • 何况 JUCE 根本没有「首帧」信号:最早只有 pageFinishedLoading(导航完成),
    //     晚于 WebView2 的首帧,拿它当开关反而把「还没出内容」那一段拉长。
    //   ⇒ 拿开窗路径上的死锁风险去换一个观感问题不划算,SL-355 因此只做了 ①-c。
    //   [SL-370] 第三条(「JUCE 根本没有首帧信号」)已经不成立:本卡在三份 index.html 的
    //   boot 脚本里补了一条 __scvb__firstFrame 上行信号(嵌套两层 rAF ⇒ 前一帧确已合成),
    //   走的是与 kBootErrorEventId 同一条 JUCE 内建通道。前两条(泵、死锁)仍成立,
    //   本卡正是靠「挪 bounds + 等导航开始」绕开它们的。
    // -------------------------------------------------------------------------
    void paint(juce::Graphics& g) override
    {
        // 基类 = fallbackPaint:WebView2 后端的 fillAll(Colours::white) + 控制器重试泵。
        // 这一句**必须在前**:它画的白由下面整块盖掉,而泵要的是「每帧都被调到」。
        juce::WebBrowserComponent::paint(g);

#if SCVB_PROBE_MAGENTA
        g.fillAll(juce::Colours::magenta); // 真机探针:见文件顶部宏注释,交付构建里不会走到
#else
        g.fillAll(scvb::webview::shellBackdrop());
#endif
    }

    bool pageAboutToLoad(const juce::String& url) override
    {
        owner_.onNavigationStarted(url);
        return true; // 本插件只导航到 resource provider 根,不拦
    }

    void pageFinishedLoading(const juce::String& url) override { owner_.onNavigationFinished(url); }

    // [SL-214] 外链通道:页面里的 window.open() / target=_blank 走这里。
    //
    // 为什么落这一层:JUCE 的 WebView2 后端已经把 NewWindowRequested 接到本虚函数上,
    // 并且 put_Handled(true) —— 也就是说**不重写它,外链就是死的**(WebView2 不会弹
    // 自己的窗,JUCE 的默认实现又什么都不做)。重写 + launchInDefaultBrowser 就是 JUCE
    // 给的现成通道,**不需要新增桥函数**:契约函数表里没有「打开外部 URL」这一项,
    // 走这条路就不必动冻结契约。
    //
    // 只放行 http/https:外链的本质是「把一个字符串交给系统去执行」,限定协议是这类
    // 通道的基本纪律 —— file: 与自定义协议一律不接,哪怕页面是我们自己打包的资源。
    //
    // 本重写是**纯增量**,不碰 pageAboutToLoad,加载路径一个字节没动。代价是主框架导航
    // (裸 <a href>、没有 target=_blank)仍会就地把整页替换掉 —— 插件窗没有后退键,那会是
    // 死局。眼下页面里一个这样的链接都没有;真要补这道防线,得在 pageAboutToLoad 里按
    // resource provider 根做白名单,而那条**会碰加载路径**,必须单独过 gate 8 真机验证。
    void newWindowAttemptingToLoad(const juce::String& newURL) override
    {
        const auto url = newURL.trim();
        if (url.startsWithIgnoreCase("https://") || url.startsWithIgnoreCase("http://"))
        {
            owner_.logDiag("external link -> default browser: " + url);
            juce::URL(url).launchInDefaultBrowser();
        }
        else
        {
            owner_.logDiag("blocked non-http external link: " + url);
        }
    }

    bool pageLoadHadNetworkError(const juce::String& errorInfo) override
    {
        owner_.onNavigationError(errorInfo);
        return false; // 不显示 WebView2 内建错误页 —— 兜底面板给的是可操作信息,内建页不是
    }

    // [SL-271] 删除式判据。paint 的 override 一旦被删掉,`&HostWebView::paint` 会经名字查找
    // 落到基类,decltype 变成 void (juce::WebBrowserComponent::*)(juce::Graphics&),本断言
    // 当场编译红(gate 4 / CI 都会拦下)。WebViewHost.cpp 不进任何测试目标(它只在
    // scvb_plugin_common 这个 INTERFACE 库里,随插件 target 编译),运行期用例够不着这一层,
    // 编译期断言是唯一守得住的判据 —— 与 SL-263 同族。
    // 它守得住的**范围**:只有「paint 由本类覆写」这一件事。函数体里那句
    // `juce::WebBrowserComponent::paint(g);` 它**守不到** —— 删掉照样编得过、断言照样绿、
    // 白底照样盖着,而重试泵就跟着没了。这不是推测:本卡真跑过这条注入(删该行 → 重建 →
    // gate 8 等价的 pluginval 全量含 GUI,strict 5),**三个 bundle 全 PASS、窗照常开得出来**
    // ⇒ 那一行**无机检覆盖,只有真机兜得住**(缺口登记在 SL-282)。顺带纠正一个流传过的
    // 说法:「泵拆了窗就开不出来」不成立 —— pluginval 环境下 JUCE 自己的
    // parentHierarchyChanged() / visibilityChanged() 已足够把控制器建起来,paint 里这个泵
    // 是**重试兜底**,不是唯一通路。
    // 相对上一版(在本层复刻一份泵)的收益因此不在「守得住」,而在**必守面**:
    // 从两处减到一处,且剩下那处是显式基类调用,签名一变就编译红,不会静默失效。
    static_assert(std::is_same_v<decltype(&HostWebView::paint), void (HostWebView::*)(juce::Graphics&)>,
                  "[SL-271] HostWebView::paint 必须由本类覆写:少了它,JUCE WebView2 后端的 "
                  "fallbackPaint 会每帧 fillAll(Colours::white),开窗白闪回归。");

private:
    WebViewHost& owner_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostWebView)
};

WebViewHost::WebViewHost(juce::AudioProcessor& processor, Config config)
    : juce::AudioProcessorEditor(&processor), config_(std::move(config)), provider_(config_.resourceSource),
      uiScale_(bridge::clampUiScale(config_.uiScale)), lang_(config_.lang)
{
    // 声明本组件完全不透明:JUCE 因此不会去画它下面的东西(宿主给的编辑器容器)。
    // [SL-271] 更正 SL-253 时的说法 —— 这一句挡不住开窗白闪,白闪那一段本 paint 根本不执行;
    // 见 HostWebView::paint 的注释。它管的是兜底面板路径下这块底。
    setOpaque(true);

    // UDF 必须在装 Options 之前定好,并当场探一次可写性:WebView2 自己碰这个目录时的失败被
    // JUCE 吞掉(环境创建回调的 HRESULT 形参无名),等到那时候就只剩「超时」两个字了。
    userDataFolder_ = PlatformWebView::makeUserDataFolder(config_.userDataFolderName);
    userDataFolderIssue_ = PlatformWebView::probeUserDataFolder(userDataFolder_);
    if (userDataFolderIssue_.isNotEmpty())
        logDiag("user-data folder problem: " + userDataFolderIssue_);

    webView_ = std::make_unique<HostWebView>(*this, makeOptions());
    addAndMakeVisible(*webView_);

    // 真 WebView2 实例化与 runtimeInfo 探测无法离线单测(需 WebView2 loader + 真机),
    // 属硬前置:T29/T30 接 InputEditor/OutputEditor 后经 gate 8 真机 GUI pluginval 验证。
    setResizable(false, false); // 仅经缩放档位下拉(setUiScale)编程改尺寸,不开自由拖角。
    resizeToDesignBox(uiScale_);

    beginLoadAttempt();
}

// 起算一次加载尝试(构造 / retry 共用):先探运行时三态,再决定加载还是直接兜底。
// 「没装」「太旧」两条都不进看门狗 —— 用户动作是装/升级 Runtime,干等一个预算窗口毫无意义。
void WebViewHost::beginLoadAttempt()
{
    releaseReadyBridge(); // 重试时本实例的旧桥不再算数(计数只统计活着的桥)
    bridgeReady_ = false;
    navState_ = NavState::notStarted;
    navDetail_ = {};
    bootError_ = {};
    navBudgetApplied_ = false;
    revealGate_.beginLoadAttempt(); // [SL-370] 重新武装:下一次导航开始时再挪一次
    revealLogged_ = false;
    startMs_ = juce::Time::getMillisecondCounter(); // 先落起点:兜底面板的「已等待」也从这里算
    runtime_ = PlatformWebView::runtimeInfo();

    if (runtime_.status == PlatformWebView::RuntimeStatus::missing)
    {
        webView_->setVisible(false);
        showFallback(FallbackReason::MissingRuntime);
        return;
    }
    if (runtime_.status == PlatformWebView::RuntimeStatus::tooOld)
    {
        webView_->setVisible(false);
        showFallback(FallbackReason::RuntimeTooOld);
        return;
    }

    // 冷/热预算见 WebViewHost.h 的三个常量注释;导航事件到达后再按 kAfterNavBudgetMs 顺延。
    // 热的判据 = 此刻另有一个桥活着(⇒ 共享的浏览器进程组必然在跑),不是「曾经起来过」。
    const bool warm = readyBridgeCount().load() > 0;
    deadlineMs_ = startMs_ + static_cast<juce::uint32>(warm ? kWarmLoadBudgetMs : kColdLoadBudgetMs);
    logDiag(juce::String(warm ? "warm" : "cold") + " start, budget " +
            juce::String(warm ? kWarmLoadBudgetMs : kColdLoadBudgetMs) + " ms, udf " +
            userDataFolder_.getFullPathName());

    logBackgroundColourSupport();

    // 必须在任何 emit 之前完成首个 goToURL(前端脚本随后加载并注册监听)。
    webView_->setVisible(true);
    webView_->goToURL(entryUrl());
    if (!isTimerRunning())
        startTimerHz(25);
}

// 首页 URL = <provider root>/<role>/index.html,**不是**裸的 provider root。
//
// 这一条不是风格问题,它决定 ES module 的身份。浏览器按**URL**认模块:同一个文件被两个
// 不同 URL 取到,就会被实例化两次,两份各持一套模块级状态。而 ResourceProvider 按
// basename 扁平反查,同一个文件在多个 URL 下都取得到 —— 于是「服务路径」与「磁盘路径」
// 一旦不一致,就会凭空多出模块副本。
//
// 从根目录进入时正是不一致的:index.html 在 /,于是 web/output/tab-wave.js 的
// `./canvas/timeline.js` 落在 /canvas/timeline.js;而 web/shared/trajectory-chart.js
// 在 /shared/,它按磁盘写的 `../output/canvas/timeline.js` 落在 /output/canvas/timeline.js。
// 同一个 timeline.js,两个 URL,两份 playhead 状态 —— Tab1 与 Tab3 的播放头因此对不上
// (fieldfix-r3 实证)。
//
// 改从 /<role>/index.html 进入后,服务 URL 空间与 web/ 的磁盘布局逐段对齐:每个相对
// 引用解析出的路径都与浏览器预览(按真实目录服务)完全相同,模块身份天然唯一。
// 这也让「预览里能跑 = 插件里能跑」重新成立 —— 之前那条差异正是本类故障的温床。
juce::String WebViewHost::entryUrl() const
{
    return WBC::getResourceProviderRoot() + config_.role + "/index.html";
}

WebViewHost::~WebViewHost()
{
    stopTimer();
    releaseReadyBridge(); // 编辑器关闭 -> 本实例不再为「热启动」判定背书
}

// 计数只统计**活着**的桥:重试与析构都要还回去,否则关掉全部编辑器后计数仍 >0,
// 下一次开窗会按热预算跑一次真冷启动 —— 那正是 bot 指出的「关窗再开必超时」。
void WebViewHost::releaseReadyBridge()
{
    if (countedAsReady_)
    {
        countedAsReady_ = false;
        readyBridgeCount().fetch_sub(1);
    }
}

juce::WebBrowserComponent& WebViewHost::webView()
{
    return *webView_;
}

void WebViewHost::paint(juce::Graphics& g)
{
    // 见头文件注:webView_ **落在本组件里**时本函数净效果为 0(不透明子组件已把这块从裁剪区
    // 剔掉),控制器建好之前那一段由 HostWebView::paint 接住。本函数真正会画的有两条路:
    //   • 兜底面板路径 —— 那时 webView_ 被 setVisible(false);
    //   • [SL-370] 遮挡期 —— webView_ 被挪到可视区之外(见 WebViewRevealGate.h),
    //     整个窗口这时露的就是这一层。**它就是用户在「内容出来之前」看到的那块占位底色**,
    //     取值 = shellBackdrop() = 成品外壳渐变的中点色,所以从占位切到内容不跳阶。
    g.fillAll(scvb::webview::shellBackdrop());
}

void WebViewHost::resized()
{
    // [SL-370] WebView 的落点由遮挡闸决定:遮挡期间整块挪到可视区之外(尺寸不变),
    // 那块地方由上面的 paint 铺 shellBackdrop() 当占位。几何与理由见 WebViewRevealGate.h。
    if (webView_ != nullptr)
        webView_->setBounds(revealGate_.parked() ? parkedBounds(getLocalBounds()) : getLocalBounds());
    if (fallback_ != nullptr)
        fallback_->setBounds(getLocalBounds());
}

// 把闸门的判定落到组件几何上。**只经这一个函数改 webView_ 的 bounds**,别在各个触发点
// 各写一次 setBounds —— 那样闸门就有了第二个真源。
void WebViewHost::applyRevealGate()
{
    if (webView_ == nullptr)
        return;
    resized();
    repaint(); // 挪走的那一刻要让占位底色立刻补上,别等下一次自然重绘
}

// [SL-376 / SL-364] 「DefaultBackgroundColor 这一层在不在」的诊断行。
//
// 判定与它证到哪一步(以及为什么插件侧做不到直接观测)只写在
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
// 文案一律 ASCII:运行期字面量走 printf 族拼接时,含非 ASCII 的相邻窄字面量会触发 MSVC C4819。
void WebViewHost::logBackgroundColourSupport() const
{
    using Support = PlatformWebView::BackgroundColourSupport;
    const auto support = PlatformWebView::backgroundColourSupport(runtime_);
    const juce::String version = runtime_.version.isNotEmpty() ? runtime_.version : juce::String("unknown");
    const juce::String argb =
        juce::String::toHexString(static_cast<int>(scvb::webview::kShellBackdropArgb)).paddedLeft('0', 8);

    const juce::String floor = juce::String(PlatformWebView::kBackgroundColourMinRuntimeMajor);
    if (support == Support::available)
        logDiag("webview2 default background: available -- ICoreWebView2Controller2 inferred present "
                "(from runtime " +
                version + " >= major " + floor + ", not directly observed), JUCE puts argb " + argb);
    else if (support == Support::unavailable)
        logDiag("webview2 default background: UNAVAILABLE -- ICoreWebView2Controller2 inferred absent "
                "(from runtime " +
                version + " < major " + floor + ", not directly observed), JUCE drops argb " + argb + " silently");
    else if (runtime_.status == PlatformWebView::RuntimeStatus::missing)
        // 当前调用点(beginLoadAttempt)在 missing 时已提前 return,走不到这里 —— 但把它写对
        // 是为了将来挪调用点的人(#247 复审【建议】⑤):否则这条会打成
        // "runtime version unknown not parsable",把原因指错。
        logDiag("webview2 default background: unknown (no WebView2 runtime detected)");
    else
        logDiag("webview2 default background: unknown (runtime version " + version + " not parsable)");
}

// 放行诊断行。**读表的人要知道的三件事**,都写在这里一处:
//   · `reason` 有**三**种(#241 复审时是四种,[SL-376] 拿掉了 `navFinished` 那一种):
//     `firstFrame` = 正常路;`timeout` = 首帧信号没来、3s 兜底;`fallback` = 遮挡期内被兜底
//     面板顶掉(RevealGate::onFallbackShown)。**`fallback` 不是「放行」**,是被面板顶掉;
//     真机数表时把它单独归一类,别塞进放行路里。
//     ⚠ `fallback` **实际只由前端 boot 失败那一条路打得出来**(它能在 3s 之内到)——
//     看门狗那条路上预算恒大于 kRevealFallbackMs,闸门早已自己记了 `timeout`;
//     运行时缺失那条路上闸门从未 parked。逐条见 showFallback() 处的注释。
//     所以**表里没有 `fallback` 是正常的**,别据此去查接线。
//   · [SL-376] `timeout` 这条**必须当异常读**:正常开窗一次都不该出现它。所以它自带后缀
//     `no first-frame signal before the 3s deadline (navFinished seen|not seen)` ——
//     `seen` = 页面 load 完了但前端没发信号(查前端 boot / rAF 那一段),`not seen` =
//     导航压根没走完(查 WebView2 环境与网络那一段)。两者的排查方向完全不同,
//     别只看 `timeout` 三个字。
//     ⚠ 措辞是「3s 线之前没来」而**不是**「从没来过」(#247 复审【建议】2):信号落在
//     「超时那个 tick 已经放行、下一个 tick 之前」这段 <40 ms 的窗口里时,它随后仍会到,
//     并自己打一行 `first-frame signal ... (already revealed)`。数表上把这两行对齐读,
//     就能把这一档与真正的「信号缺席」分开。
//   · `after N ms` 一律从 **startMs_**(本次加载尝试的起点)算,而 kRevealFallbackMs 的 3s
//     是从**导航开始**算的 —— 所以 `timeout` 那条打出来会是「3000 + 导航前耗时」而不是 3000。
//     两个起点不同是有意的:这一行是给人看「从点开窗口算起等了多久」。
void WebViewHost::noteRevealed()
{
    // `lastRevealReason()` 为空 = 这一轮从来没挪走过(例如运行时缺失直接切了兜底面板),
    // 那就没有「放行」这回事,别写一行原因是空串的诊断。
    if (revealLogged_ || revealGate_.parked() || juce::String(revealGate_.lastRevealReason()).isEmpty())
        return;
    revealLogged_ = true;
    const juce::String reason(revealGate_.lastRevealReason());
    juce::String line = "webview revealed (" + reason + ") after " +
                        juce::String(static_cast<int>(juce::Time::getMillisecondCounter() - startMs_)) + " ms";
    if (reason == "timeout")
        line << " -- no first-frame signal before the 3s deadline (navFinished "
             << (revealGate_.navigationFinishedSeen() ? "seen" : "not seen") << ")";
    logDiag(line);
}

// -----------------------------------------------------------------------------
// 缩放(机制 9):固定设计盒 × CSS zoom + setSize 同步
// -----------------------------------------------------------------------------
void WebViewHost::resizeToDesignBox(float scale)
{
    const auto r = bridge::designBoxWindowSize(config_.role, scale);
    setSize(r.width, r.height);
}

void WebViewHost::setUiScale(float scale)
{
    uiScale_ = bridge::clampUiScale(scale);
    resizeToDesignBox(uiScale_);
}

void WebViewHost::commitUiScale()
{
    persistUiScaleAsDefault();
}

void WebViewHost::persistUiScaleAsDefault()
{
    // 默认空实现:宿主侧全局默认落盘由插件 Processor 承担(T29/T30 或后续)。
}

juce::var WebViewHost::buildSnapshot()
{
    bridge::UiSeed seed;
    seed.role = config_.role;
    seed.version = config_.version;
    seed.lang = lang_;
    seed.uiScale = uiScale_;
    seed.channelLimit = config_.channelLimit;
    return bridge::buildUiSnapshot(seed);
}

// -----------------------------------------------------------------------------
// 兜底面板(机制 3 后半)
// -----------------------------------------------------------------------------
juce::String WebViewHost::buildDiagnostics() const
{
    const char* nav = "notStarted";
    switch (navState_)
    {
    case NavState::started:
        nav = "started";
        break;
    case NavState::finished:
        nav = "finished";
        break;
    case NavState::networkError:
        nav = "networkError";
        break;
    case NavState::notStarted:
    default:
        break;
    }

    juce::String d;
    d << "waited " << juce::String(static_cast<int>(juce::Time::getMillisecondCounter() - startMs_)) << " ms"
      << "  |  nav " << nav;
    if (navDetail_.isNotEmpty())
        d << " (" << navDetail_ << ")";
    d << "  |  WebView2 " << (runtime_.version.isNotEmpty() ? runtime_.version : juce::String("not found"));
    d << "  |  host " << juce::File::getSpecialLocation(juce::File::hostApplicationPath).getFileName() << " pid "
      << juce::String(PlatformWebView::processId());
    // UDF 是「环境没起来」这一路的头号嫌疑,必须原样显示:用户把这一行发回来,就能直接看出
    // 目录在哪、写不写得进、以及(名字里的 PID)是不是被另一个宿主进程占着。
    d << "\n" << "udf " << userDataFolder_.getFullPathName();
    if (userDataFolderIssue_.isNotEmpty())
        d << "  [" << userDataFolderIssue_ << "]";
    if (bootError_.isNotEmpty())
        d << "\n" << bootError_;
    return d;
}

void WebViewHost::logDiag(const juce::String& line) const
{
    // 既有日志通道:juce::Logger。DBG(= outputDebugString)在 Release 里被编掉,而兜底面板
    // 恰恰只在用户的 Release 包上出现 —— writeToLog 在无 logger 时也会落 outputDebugString
    // (DebugView 可见),宿主设了 logger 则进宿主日志。诊断不能只活在 Debug 构建里。
    juce::Logger::writeToLog("SCVB " + config_.role + ": " + line);
}

void WebViewHost::showFallback(FallbackReason reason)
{
    if (fallback_ != nullptr)
        return;
    // [SL-370] 面板自己铺满本组件,闸门不该再按住 WebView 的位置(否则 retry 回来时
    // bounds 还停在可视区外,而那条路上不一定再有导航事件把它推回来)。
    // [SL-376] `noteRevealed()` 必须**在这里**调(#247 复审【建议】③):拿掉 navFinished
    // 那条路之后,唯一还会调它的地方只剩 handleFirstFrame(),而「走到兜底面板」的典型场景
    // 恰恰是首帧信号根本不会来 ⇒ `webview revealed (fallback)` 这一行会从此消失,
    // 而 noteRevealed() 的头注仍把 fallback 列成读表的人会看到的三种 reason 之一。
    // 这里也本来就是更自然的落点:面板一切,就该当场记下闸门是被谁顶掉的。
    //
    // ⚠ **但它真正打得出来的只有一条路**(#247 复审第 2 轮:三个调用点各走一遍状态):
    //   · missing / tooOld —— 在 beginLoadAttempt 里,还没导航 ⇒ 闸门从未 parked,
    //     reveal() 因 `!parked_` 提前返回、reason 留空 ⇒ **不打**(也不该打);
    //   · LoadTimeout —— 看门狗预算(热 5s / 冷 15s,导航后还按 kAfterNavBudgetMs=5s 顺延)
    //     **恒大于** 闸门的 kRevealFallbackMs=3s,所以闸门早就自己按 timeout 放行并写过行了
    //     ⇒ revealLogged_ 为真,**不打**(那次开窗的表里是一行 timeout,信息没丢);
    //   · BootError —— 前端 boot 挂了,可以在 3s 之内到 ⇒ **只有这一条打得出 fallback**。
    //   读表的人据此就不会因为「看不到 fallback」去查接线。
    revealGate_.onFallbackShown();
    noteRevealed();
    webView_->setVisible(false);

    const bool missing = (reason == FallbackReason::MissingRuntime);
    const bool tooOld = (reason == FallbackReason::RuntimeTooOld);

    juce::String message;
    const char* tag = "loadTimeout";
    if (missing)
    {
        message = missingRuntimeMessage();
        tag = "missingRuntime";
    }
    else if (tooOld)
    {
        message = tooOldRuntimeMessage();
        tag = "runtimeTooOld";
    }
    else if (reason == FallbackReason::BootError)
    {
        message = bootErrorMessage();
        tag = "bootError";
    }
    else if (navState_ == NavState::notStarted)
    {
        // 导航一次都没开始 = WebView2 环境/控制器没建起来(见文件头三态注释)。
        message = envNotStartedMessage();
        tag = "envNotStarted";
    }
    else if (navState_ == NavState::finished)
    {
        message = bridgeStalledMessage();
        tag = "bridgeStalled";
    }
    else
    {
        message = navStalledMessage();
        tag = "navStalled";
    }

    const auto details = buildDiagnostics();
    logDiag(juce::String("fallback ") + tag + " — " + details.replaceCharacter('\n', ' '));

    FallbackPanel::Options options;
    options.title = fallbackTitle(config_.role);
    options.message = message;
    options.details = details;
    options.showInstall = missing || tooOld; // 两条的用户动作都是装/升级 Evergreen Runtime
    options.onInstall = [] { juce::URL(PlatformWebView::runtimeDownloadUrl()).launchInDefaultBrowser(); };
    // 延后到消息线程执行:FallbackPanel 的 retry onClick 内同步 reset 会销毁正执行回调的按钮
    // (use-after-free)。SafePointer 兜底 WebViewHost 先于回调被销毁的情况。
    options.onRetry = [safeThis = juce::Component::SafePointer<WebViewHost>(this)] {
        juce::MessageManager::callAsync([safeThis] {
            if (safeThis != nullptr)
                safeThis->retryWebView();
        });
    };

    fallback_ = std::make_unique<FallbackPanel>(std::move(options));
    addAndMakeVisible(*fallback_);

    // 兜底面板固定像素布局(不随 uiScale 缩放)。小缩放档位下窗口过小会挤压/裁掉按钮,
    // 故切兜底时放大到至少设计盒尺寸;更大的缩放窗口保持不变。retryWebView 回到 WebView 时按 uiScale 恢复。
    const auto design = bridge::designBoxWindowSize(config_.role, 1.0f);
    if (getWidth() < design.width || getHeight() < design.height)
        setSize(juce::jmax(getWidth(), design.width), juce::jmax(getHeight(), design.height));
    resized();
}

void WebViewHost::retryWebView()
{
    fallback_.reset();
    resizeToDesignBox(uiScale_);
    beginLoadAttempt(); // 重探运行时三态 + 重置看门狗;若又是 missing/tooOld 会就地再切兜底
    resized();
}

// -----------------------------------------------------------------------------
// 加载时序回调(message 线程,来自 HostWebView)
// -----------------------------------------------------------------------------
void WebViewHost::onNavigationStarted(const juce::String& url)
{
    navState_ = NavState::started;
    navDetail_ = {};

    // 「收到首个导航事件才重新起算」:导航开始证明 WebView2 环境已就绪,页面加载该有自己的
    // 完整额度。只顺延一次(navBudgetApplied_),且只许延后不许提前 —— 否则冷启动刚用掉的
    // 时间会被这里抹掉,或者一个反复导航的页面能把看门狗无限推后。
    if (!navBudgetApplied_)
    {
        navBudgetApplied_ = true;
        const auto extended = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(kAfterNavBudgetMs);
        if (static_cast<juce::int32>(extended - deadlineMs_) > 0) // 回绕安全的「更晚吗」
            deadlineMs_ = extended;
    }

    // [SL-370] 导航开始 = WebView2 控制器已建好 ⇒ SL-271 那个挂在 paint 上的重试泵已是空调用,
    // 此刻才可以把 WebView 挪出可视区(挪走之后 JUCE 就不再画它,泵也就不再被驱动)。
    revealGate_.onNavigationStarted(juce::Time::getMillisecondCounter());
    applyRevealGate();

    logDiag("navigation started: " + url);
}

void WebViewHost::onNavigationFinished(const juce::String& url)
{
    // 页面已下载完 ≠ 桥已就绪:前端还要跑模块图并调 requestInitialState。看门狗继续跑,
    // 但状态记下来 —— 「finished 却超时」精确指向前端 boot 失败,与「压根没导航」判然不同。
    navState_ = NavState::finished;
    // [SL-376] **这一条不再放行**,只记账。理由:pageFinishedLoading 只说明文档下载完、load
    // 事件发了,不保证任何一帧已经合成;SL-370 的 pluginval 数表里它有 4/10 次抢在首帧信号前
    // 3–6 ms 放行,放回来露出的就是 WebView2 宿主 HWND 首帧之前的底 —— 用户 v5.6.10 看到的
    // 「粉 → 白一瞬 → 内容」。完整定谳只写在 WebViewRevealGate.h 头注一处,别在这里复述。
    // 记下的这一位只进 noteRevealed() 的 timeout 那一行,帮着分「页面 load 完了但信号没发」
    // 与「导航压根没走完」。原先跟在后面的 applyRevealGate() / noteRevealed() 一并删掉:
    // 本函数已经不改闸门状态,那两句无论闸门开着还是关着都是空调用(还 parked ⇒ 位置本就在
    // 停车位、放行行不写;已放行 ⇒ 位置本就在原位、放行行早写过),留着只会让读者以为
    // 这里还有一条放行路。
    revealGate_.onNavigationFinished();
    logDiag("navigation finished: " + url);
}

void WebViewHost::onNavigationError(const juce::String& errorInfo)
{
    navState_ = NavState::networkError;
    navDetail_ = errorInfo.substring(0, 200);
    logDiag("navigation error: " + navDetail_);

    // **这里刻意不切兜底面板**,由看门狗统一裁决。
    //
    // 原因是「重试自锁」:点 Retry -> retryWebView 先 reset 掉面板、再 goToURL,而这个
    // goToURL 会**中止**上一次仍在飞的导航;被中止的那次随后异步回调过来报错,此时
    // fallback_ 恰好是 nullptr、bridgeReady_ 也还是 false —— 于是上一次的错误把刚开始的
    // 这一次就地判死,面板瞬间又贴回来。用户看到的就是「点重试没反应」。
    //
    // 不用「按错误码放行」来修:JUCE 只帮忙过滤了 OPERATION_CANCELED
    // (juce_WebBrowserComponent_windows.cpp 里把它当成功、走 pageFinishedLoading),
    // CONNECTION_ABORTED(错误码 9)照样漏下来 —— JUCE 自己的注释都写着「code 9 往往可以
    // 安全忽略」。与其维护一张「哪些码不算数」的名单,不如根本不让任何单次导航错误拥有
    // 直接判死的权力:真失败会在预算耗尽时照常进兜底面板,诊断行里带着这里记下的错误码;
    // 而中止、瞬时错误则被后续的 navigationStarted / finished 自然覆盖掉。
    // 代价只是「必然失败的情形要多等一个预算窗口」,换来的是重试真的能用。
}

// 前端 boot 失败上报(机制 3 补强)。
//
// 【为什么不是契约桥函数】契约 §7 manifest 已冻结(Output 34 函数 / 9 事件,Input 7 / 5),
// 且那些名字全部要求桥已就绪;而这里要报的恰恰是「桥还没起来」。故走 JUCE **内建**的上行
// 通道:window.__JUCE__.postMessage({eventId, payload}) ←→ C++ Options::withEventListener。
// 该通道由 JUCE 在文档创建前注入,不经 bridge.js、不占用 §7 名表、不参与 check-bridge-parity,
// 名字加 __scvb__ 前缀(照 JUCE 自己的 __juce__ 惯例)标明它是诊断面而非契约面。
void WebViewHost::handleBootError(const juce::var& payload)
{
    const auto stage = payload.getProperty("stage", juce::var("unknown")).toString();
    const auto detail = payload.getProperty("detail", juce::var("")).toString();

    bootError_ = "boot " + stage + ": " + detail.substring(0, 300);
    logDiag("front-end " + bootError_);

    if (bridgeReady_ || fallback_ != nullptr)
        return; // 桥已起来后的运行期错误归 UI 自己处理,不砸掉一个能用的界面
    showFallback(FallbackReason::BootError);
}

// [SL-370] 前端「首帧已绘」上报。通道与 kBootErrorEventId 同一条(JUCE 内建
// __JUCE__.postMessage,不经 bridge.js、不占契约 §7 名表),理由见 handleBootError 的注释。
// 载荷不看:这条信号只有「到了」这一个信息量,前端也只发一次。
void WebViewHost::handleFirstFrame()
{
    // [SL-370] **先记「信号到了」,再谈放行** —— 这是两件必须分开数的事:信号可能在 3s 兜底
    // 或兜底面板之后才姗姗来迟,那时 noteRevealed() 一个字都不写;只看放行原因就会把
    // 「信号来晚了」误读成「信号没来」,而后者正是本卡唯一那条静默降级
    // (挪出可视区 ⇒ 合成器停 BeginFrame ⇒ rAF 停 ⇒ 信号永不到达,见 WebViewRevealGate.h)。
    // 真机验收数的就是这一行与下面那行放行行的**条数比**。
    logDiag(juce::String("first-frame signal after ") +
            juce::String(static_cast<int>(juce::Time::getMillisecondCounter() - startMs_)) + " ms" +
            (revealGate_.parked() ? " (still parked)" : " (already revealed)"));
    // [SL-376] onFirstFrame() **只武装,不放行** —— 真正挪回可视区在后面的 25Hz tick 上
    // (kRevealSettleTicks ∧ kRevealSettleMs,理由见 WebViewRevealGate.h 头注)。所以下面两句
    // 在**正常那条路**上是空调用,留着是为了「闸门状态一变就落地」这条不变式只有
    // applyRevealGate 一个出口。信号在兜底面板之后才到时它们同样无副作用:那时
    // showFallback() 已经写过放行行(revealLogged_ 为真),noteRevealed() 直接返回。
    // 传 nowMs 是因为毫秒下界要从**信号到达那一刻**起算,不是从下一个 tick 起算。
    revealGate_.onFirstFrame(juce::Time::getMillisecondCounter());
    applyRevealGate();
    noteRevealed();
}

// -----------------------------------------------------------------------------
// WebView 装配(机制 1/2/4/5/6)
// -----------------------------------------------------------------------------
juce::WebBrowserComponent::Options WebViewHost::makeOptions()
{
    auto options = PlatformWebView::makeWebViewOptions(WBC::Options{}, userDataFolder_);

    options = options.withNativeIntegrationEnabled().withResourceProvider(
        [this](const juce::String& url) { return provider_.provide(url); },
        juce::URL(WBC::getResourceProviderRoot()).getOrigin());

    // 首帧同步 seed(机制 5):version / 角色 / channel 上限 / lang / uiScale(键值对与 buildSnapshot 同源)。
    for (const auto& seedPair :
         bridge::buildUiSeedPairs({config_.role, config_.version, lang_, uiScale_, config_.channelLimit}))
        options = options.withInitialisationData(seedPair.first, seedPair.second);

    // JS -> C++(机制 6):通用缩放/语言/首帧;插件专属函数由 augmentOptions 追加。
    options = options
                  .withNativeFunction(juce::Identifier(bridge::Fn::RequestInitialState),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleRequestInitialState(a, std::move(c));
                                      })
                  .withNativeFunction(juce::Identifier(bridge::Fn::SetLang),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleSetLang(a, std::move(c));
                                      })
                  .withNativeFunction(juce::Identifier(bridge::Fn::SetUiScale),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleSetUiScale(a, std::move(c));
                                      })
                  .withNativeFunction(juce::Identifier(bridge::Fn::CommitUiScale),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleCommitUiScale(a, std::move(c));
                                      });

    // 诊断面(非契约):前端 boot 失败上行。理由与通道选择见 handleBootError 的注释。
    options = options.withEventListener(juce::Identifier(kBootErrorEventId),
                                        [this](const juce::var& payload) { handleBootError(payload); });

    // [SL-370] 时序面(非契约):前端「首帧已绘」上行 —— 遮挡闸的第一条放行路。
    options = options.withEventListener(juce::Identifier(kFirstFrameEventId),
                                        [this](const juce::var&) { handleFirstFrame(); });

    if (config_.augmentOptions)
        config_.augmentOptions(options);

    return options;
}

// -----------------------------------------------------------------------------
// 原生函数处理(message 线程)
// -----------------------------------------------------------------------------
void WebViewHost::handleRequestInitialState(const juce::Array<juce::var>&, WBC::NativeFunctionCompletion complete)
{
    bridgeReady_ = true; // 前端确认就绪 -> 此后 timer 才允许 emit(机制 8)
    // 本实例的桥活着 -> 共享的 WebView2 进程组必在跑 -> 其它编辑器可按热预算起看门狗。
    // requestInitialState 可能被同一页面多次调用(页面重载后要重来一次),故用标志防重复计数。
    if (!countedAsReady_)
    {
        countedAsReady_ = true;
        readyBridgeCount().fetch_add(1);
    }
    logDiag("bridge ready after " + juce::String(static_cast<int>(juce::Time::getMillisecondCounter() - startMs_)) +
            " ms");
    complete(buildSnapshot());
}

void WebViewHost::handleSetLang(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
{
    const juce::String code = args.size() > 0 ? args[0].toString() : juce::String("zh");
    lang_ = bridge::normalizeLang(code); // §1.30:仅 {zh,en,fr},未知回退 zh;实际值经 buildSnapshot 回推 scvb.state
    complete(bridge::okResponse());
}

void WebViewHost::handleSetUiScale(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
{
    float scale = uiScale_;
    if (!bridge::parseUiScaleArg(args, config_.role, scale))
    {
        complete(bridge::badArgResponse()); // §1.28:非法参数(缺参/非数值/不在档位表)→ badArg
        return;
    }
    setUiScale(scale); // 只实时预览、不落盘(机制 9 前半)
    complete(bridge::okResponse()); // §1.28:{ok:true}
}

void WebViewHost::handleCommitUiScale(const juce::Array<juce::var>&, WBC::NativeFunctionCompletion complete)
{
    commitUiScale(); // 防呆确认「保持」后落盘(机制 9 后半)
    complete(bridge::okResponse()); // §1.29:{ok:true}
}

// -----------------------------------------------------------------------------
// 25Hz Timer(message 线程):看门狗 + 就绪门控 + diff-then-emit(机制 3/7/8)
// -----------------------------------------------------------------------------
void WebViewHost::timerCallback()
{
    // WebView2 看门狗:预算由 beginLoadAttempt 定(冷 15s / 热 5s),首个导航事件到达后按
    // kAfterNavBudgetMs 顺延。超时判定加载失败切兜底(可重试/重开窗口),文案不误报「运行时缺失」。
    // 比较走 uint32 差值再转 int32:getMillisecondCounter 每 ~49 天回绕一次,直接比大小会在
    // 回绕点把「还没到点」算成「早就超时」,把好好的窗口砸成兜底面板。
    // (这四行说的是**下面第二个** if;紧跟的第一个 if 是 [SL-370] 遮挡闸,方向相反 ——
    //  这里到点=把界面放出来,那里到点=换成兜底面板。两段不要读串。)
    // [SL-370/SL-376] 遮挡闸在这个 tick 上做**两件**事,都收在 onTick 里:
    //   · 首帧信号已到时结算那一拍(kRevealSettleTicks)——「信号 = 帧已提交」,提交到上屏
    //     还差一拍,所以放行落在这里而不是 handleFirstFrame 里;
    //   · 信号没来时到点强制放行(kRevealFallbackMs),绝不允许出现「永远挪在外面」——
    //     那会是一块彻底不动的粉色板,比白闪坏得多。
    // 正因为**放行现在只可能发生在这个 tick 上**,这个 if 是唯一会把闸门从 parked 翻过来的
    // 地方(fallback 那条除外);别把它读成「只是超时兜底」。
    if (revealGate_.parked())
    {
        revealGate_.onTick(juce::Time::getMillisecondCounter());
        if (!revealGate_.parked())
        {
            applyRevealGate();
            noteRevealed();
        }
    }

    if (!bridgeReady_ && fallback_ == nullptr &&
        static_cast<juce::int32>(juce::Time::getMillisecondCounter() - deadlineMs_) > 0)
    {
        showFallback(FallbackReason::LoadTimeout);
        return;
    }
    if (!bridgeReady_)
        return;

    emitTick();
}

void WebViewHost::emitTick()
{
    // 默认空实现;T29/T30 子类在此追加各事件类别的 diff-then-emit(机制 7)。
}

} // namespace scvb::webview
