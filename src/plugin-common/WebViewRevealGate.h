// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_graphics/juce_graphics.h>

#include <cstdint>

namespace scvb::webview
{

// -----------------------------------------------------------------------------
// [SL-370 / SL-376] 开窗遮挡闸 —— 「WebView 子窗口此刻该不该待在可视区之外」的**唯一判定处**。
//
// 【为什么需要它】把预绘底色换成成品浅色(本卡第一段)只是兜底:用户机的 WebView2 Runtime
// 是 152.0.4191.66(ICoreWebView2Controller2 / DefaultBackgroundColor 全可用),仍固定看到
// 「白 → 黑 → 白 → 内容」。也就是说白的那几帧根本不由我方任何一层底色决定 —— WebView2 的
// 宿主 HWND 一旦上屏,它自己在合成首帧之前画什么,插件侧没有任何 API 管得到。
// 于是改成**根本不让它上屏**:导航开始到「页面首帧已绘」之间,把 WebView 子窗口挪到宿主
// 客户区之外,那块地方由 WebViewHost::paint 铺占位([SL-402] 起 = 与成品外壳渐变同组色标的
// shellBackdropGradient 渐变;SL-370 当时是单色成品外壳中点色)。
//
// 【为什么挪走而不是 setVisible(false) / 零尺寸】三条,缺一不可:
//   • `setVisible(false)` 会走 JUCE 的 componentVisibilityChanged → checkWindowAssociation
//     的 else 分支,而 `Options` 里那个 `keepPageLoadedWhenBrowserIsHidden` 成员的**默认值
//     就是 false**(在 juce_WebBrowserComponent.h 里 grep 这个名字,声明处带着 `= false`)
//     ⇒ `unloadPageWhenHidden` 为真 ⇒ JUCE 会把页面 `goToURL("about:blank")` 顶掉。
//     隐藏一下就把正在加载的页面弄没了。**这一条是读实现读出来的,不是推断。**
//   • `put_IsVisible(false)`:WebView2 文档对这个属性的说法是「不可见时 WebView **停止渲染**」。
//     渲染停了之后 `requestAnimationFrame` 还发不发,**本机没条件实测**(要真 WebView2 宿主)
//     —— 但它一旦不发,就与「等 rAF 里的首帧信号」互相等死,3s 兜底会变成每次开窗都吃满。
//     **这一条是风险不是已证事实**;挪 bounds 根本不用去赌它,所以没有为它做实验的必要。
//   • 零尺寸会把视口压成 0×0,页面按 0 宽布局、放回来时整页 reflow。
//   挪走则三条都不沾:尺寸不变、`owner.isShowing()` 不变、Chromium 仍认为自己可见。
//
// ⚠ **挪走并没有把这一族风险全甩掉**(#241 复审【重要】①):Chromium 对「可视矩形为空」的
//   widget 同样会停 BeginFrame,rAF 随之停摆 —— 与被否掉的 `put_IsVisible(false)` 殊途同归,
//   而我们挪的正是被 Windows 裁到零可见面积的那个 WebView2 宿主 HWND。**本机验不了**
//   (要真 WebView2 宿主),所以这条是**已知风险,不是已排除项**。
//   命中时的形态是**静默降级,不是崩/卡**:`firstFrame` 永不到达 ⇒ 每次开窗都吃满 3s 超时
//   兜底。[SL-376] 之后 `navFinished` 不再放行,所以这条降级的代价从「早放几毫秒」变成
//   「每次开窗多按住 3 秒占位」—— 屏上仍然是「粉底 → 内容」,不会白也不会卡,但
//   **「等到首帧已绘再放」这条保证没了**,而这件事**没有任何一格判据会红**。
//   探针有两行,**必须一起读**:`WebViewHost::handleFirstFrame()` 的
//   `first-frame signal after <n> ms (still parked|already revealed)`(信号**到没到**),
//   与 `noteRevealed()` 的 `webview revealed (<reason>) after <n> ms`(**谁**放的行)。
//   只看后者会把「信号来晚了」误读成「信号没来」—— 而这两者的处置完全不同。
//   真机验收的硬指标是**前者的条数**:开 N 次窗就该有 N 行 `first-frame signal`;
//   少了才是命中本条。[SL-376] 之后还多了一条更直白的指标:放行原因里只要出现
//   `timeout`,就说明这一次开窗的首帧信号缺席(那一行会自带 `navFinished seen|not seen`
//   帮着分「页面 load 完了但信号没发」与「导航压根没走完」)。
//   本机 pluginval(真 WebView2 宿主)`--repeat 10` 实测:**Input / Output 各 10 次开窗,
//   每次都收到了 `first-frame signal`,且到达时闸门都还 `still parked`** ⇒ rAF 在挪出可视区
//   之后**仍在跑**,本条**未命中**。放行原因两侧都是 **10 firstFrame / 0 navFinished /
//   0 timeout**([SL-376] 之后 navFinished 本就不再放行)。同一次实测里「信号 → 挪回」那一拍
//   落在 **32~72 ms**(下界 32 = 当时的 kRevealSettleMs,上界 ≈ 32 + 一个 25Hz tick)—— 加下界
//   之前是 4~42 ms,最小那次比一个合成帧还短,正是复审点出的「注释比实现强」。
//   ⚠ [SL-436] 这组数是 `kRevealSettleMs = 32` 时代的**实测**,不是公式推出来的,改到 64 后
//   没有重新拿 pluginval 量过;按下面证明过的 worst_case = kRevealSettleMs + tick 公式外推,
//   应落在 **64~104 ms**,这是外推值、未经实测验证。
//   ⚠ 同一次实测也量到了**这两条回调的先后本来就不稳**:Input 上 `navFinished` 每次都比
//   首帧信号早 7–16 ms(10/10),Output 上反过来,首帧信号早约 21 ms(10 次里 8 次)。
//   SL-370 当时量到的 6 firstFrame / 4 navFinished 是同一件事的另一个样本。
//   **这正是不能拿 navFinished 放行的理由**:它赢不赢是掷骰子,而它赢的那几次露的就是白。
//   数表见 PR 描述。
//   ⚠ 这是**pluginval 宿主上的实测**,不等于所有 DAW —— 别把它读成「这条风险已经消失」,
//   验收指标(每开一次窗就该有一行 `first-frame signal`)照留。
//   真命中之后的出路不是回到隐藏(它更糟),而是「不挪 WebView、在它上面盖一层原生占位窗」;
//   **不要再退回「navFinished 也放行」**—— 那正是 SL-376 定谳掉的那段白(见下面【只认首帧】)。
//   那时再立卡,别在这里预先写死结论。
//
// 【为什么等到 onNavigationStarted 才挪】SL-271 的重试泵挂在 `HostWebView::paint` 上,而挪出
// 可视区之后 JUCE 不再画它 ⇒ 泵停。导航开始 = WebView2 控制器已经建好 —— JUCE 在
// `CreateCoreWebView2Controller` 的完成回调里是先 `addEventHandlers()` + `setWebViewPreferences()`
// 再 `Navigate`(juce_WebBrowserComponent_windows.cpp 的 createWebView,读实现核过),
// 而泵的条件正是
// `if (! hasBrowserBeenCreated())` —— 控制器建好之后它本来就是空调用。所以「控制器建好之前
// 一直可见」既保住了泵,又没有放过任何一帧白:那一段屏上是我方 paint 铺的占位渐变
// ([SL-402] 起;SL-370 当时是单色 shellBackdrop)。
//
// 【[SL-376] 只认首帧 —— 放行路从三条减到两条】
// v5.6.10 真机(用户)是「粉色占位 → **白一瞬** → 内容」:黑没了(SL-370 的颜色那一段生效),
// 白挪到了**放行之后、页面首帧上屏之前**。定谳靠的是 SL-370 自己留下的那张数表 ——
// pluginval `--repeat 10` 里有 **4/10 次是 navFinished 抢在 firstFrame 前 3–6 ms 放的行**。
// `pageFinishedLoading`(= WebView2 的 NavigationCompleted)只说明「文档下载完、load 事件发了」,
// 它**不保证任何一帧已经合成**;此刻把 WebView 挪回可视区,露出来的就是 WebView2 那个宿主
// HWND 在首帧之前画的东西(DefaultBackgroundColor 缺席时即白,见 SL-364)。
// ⚠ **「谁先到」是掷骰子,不是常数** —— 本卡复测(见上面那段实测记录)量到 Input 上
// navFinished 每次都早 7–16 ms、Output 上反过来晚约 21 ms。所以别去纠结「差几毫秒」:
// 修法是把这条路整个拿掉,而不是去调它的胜负。用户机上白每次都在,只说明那台机器上
// navFinished 稳定赢;**这一点本机复现不了,也不需要复现**。
// ⇒ `navFinished` **不再放行**,只记账(`navigationFinishedSeen()`,进超时那一行诊断);
//   放行只认 `firstFrame`([SL-429] 起 = 前端等到 **paint 记录**到达后再嵌套两层 rAF 才发),
//   外加 `timeout` 兜底与 `fallback` 顶替。
//
// 【[SL-429] 「两层 rAF ⇒ 已绘」是假的 —— 第二段白的**一条**成因(不是已证的根因)】
// SL-370 在三份 index.html 与本文件里都写过「嵌套两层 rAF ⇒ 前一帧确已合成」。**这句是错的**:
// rAF 回调跑在事件循环的渲染步里,而 Chromium 在导航后的 paint-holding 期间**照样跑渲染步、
// 却不提交任何一帧**。于是「两层 rAF 都回调过了」与「页面画过一帧」是两件事。
// 按 DOMContentLoaded 触发时,信号时刻 ≈ DCL + 两个 rAF,**与 first-paint 之间没有任何
// 约束** —— 谁先谁后随页面与机器状态而变。
//
// 本机 headless Chrome 实测(CDP 桩记信号时刻 + PerformanceObserver 的 paint 记录;
// 修前树 = 3a759a7 的 web/,修后树 = 本卡,各跑 12 轮。下表取「加载顺畅」的那些轮
// —— 本机 12 轮里有几轮页面自己就慢到 3~8 秒,那种轮次 first-paint 与 DCL 挤在一起,
// 不反映正常开窗;全量数表与两种口径都在 PR 描述里),`信号 − first-paint`:
//     页面      修前(中位数 / 区间)         修后(中位数 / 区间)
//     input     −277 ms / −369 .. −3 ms      +16 ms / +15 .. +81 ms
//     monitor     +6 ms /   −8 .. +73 ms     +17 ms / +15 .. +19 ms
//     output     +59 ms /  +35 .. +66 ms     +90 ms / +48 .. +153 ms
// ⚠ **不要把符号读成判别式**。本文件第一版写过「负 ⇒ 闪、正 ⇒ 不闪,与用户 2026-09-17
//   给的三比一完全对齐」——12 轮复测把它证伪:monitor 修前**十有八九是正的**(全量 12 轮里
//   只有 2 轮为负),整个区间 −8 .. +73 ms 与下面 `kRevealSettleMs` 同量级 ——
//   ⚠ [SL-436] 这条论证按**改后的 64 ms** 重算(原话按 32 ms 写的,下面是重新核过的数,
//   不是简单替换字面量):放行还要再压满那 64 ms ⇒ 最坏一档 −8+64=56 ms,比原来 32 ms 时的
//   −8+32=24 ms 更宽裕 ⇒ **按本模型 monitor 更不该闪**(结论没有被这次改动削弱,反而更稳),
//   可用户真机上它稳定闪。
//   ⇒ **monitor 的第二段白本卡没有解释**;这一修对它是净正向(信号只会更晚),但成因不明,
//   与观测相容 ≠ 被解释。input 那一档在 headless 上早 **200~370 ms**,比这 64 ms 的余量大
//   **3~6 倍**(改前对 32 ms 是 6~11 倍,「一个数量级」原本就是松口语,64 下更准的说法是
//   「大几倍」)—— 但**那是 headless 的量级,真宿主上小得多**,见下面【真宿主 A/B】。
//   output 是三页里唯一天然免疫的(index.html 约 490 KB,input 47 / monitor 64,
//   first-paint 稳定抢在信号之前),**碰巧**而非设计 ——
//   也正因如此,只跑 output 的页面级判据测不出本缺陷。
// 机理:信号早于首帧 ⇒ 闸门在页面**一个像素都还没画过**的时候就把 WebView 挪回可视区,
// 露出来的是 Chromium 那个 widget 在自己首帧之前铺的底(白),它盖在 ①-b
// (`put_DefaultBackgroundColor`)**上面** —— 所以把 ①-b 换成什么颜色都救不了这一段。
// ⇒ 三页的武装触发条件改成「PerformanceObserver 收到 paint 记录」再走原来的两层 rAF。
//   **走 paint 这条路时「信号晚于 first-paint」是结构保证,不是实测巧合**:paint 记录是在
//   那一帧画过之后才创建的,而观察者回调必然在记录入队之后的另一个任务里跑(本机 72 个
//   样本无一例外:回调时刻比记录的 startTime 晚 2.5~109 ms,中位数 6 ms)。所以上表修后
//   那几个 +ms 只是「回调延迟 + 两层 rAF」的大小,**它们的数值不再是判据** ——
//   monitor 那 +17 贴不贴零也就不要紧了。⚠ 这条保证**只覆盖 paint 那条路**:走下面的
//   回落或保险定时器时它不成立(那两条路本来就是为「paint 记录不来」准备的)。
//   回落(不支持 PerformanceObserver ⇒ 退回 DOMContentLoaded)与保险定时器(paint 记录
//   永不到达时仍然放行)都在页内,理由写在那里。
//   ⚠ [SL-429 第 2 轮] **别再写「2.5s 保险赶在这里的 3s 之前」** —— 两者**不共享时间原点**:
//   下面的 kRevealFallbackMs 从 onNavigationStarted(parkedAtMs_)起算,页内那 2500 ms 从
//   **那段内联脚本执行**起算,中间隔着「导航开始 → 文档送达 → <head> 解析」。真实余量是
//   `500ms − 那一段`;那一段够长时保险反而跑输,放行原因照样掉成 timeout。而且**这件事
//   从抓取包里看不出来**(`navigation started` 那行不带时间戳)⇒ SL-430,与 SL-426
//   (日志里读不出插件版本号)同族。**SL-430 的前半已随本卡落地**:首帧信号的载荷现在带
//   `paintDeltaMs`,`first-frame signal` 那行尾部会打 `(signal-firstPaint +N ms)` 或
//   `(no paint record)`(见 WebViewHost::handleFirstFrame)⇒ 用户机上的余量**可以直接读了**,
//   不用再像上面那张表一样从 A/B 差值反推;`navigation started` 补时间戳那**后半仍封存**。
//   ⚠ 那条保险要成立,**三件事缺一不可**(本卡为它连栽三轮,每一轮都是上一轮补丁的副作用):
//     · 回调**直接发信号、不绕两层 rAF** —— 保险存在的唯一理由就是「paint 记录不来」,
//       而那一档最可能的成因正是上面点名的 BeginFrame 停摆,停了 rAF 也不回调;
//     · 它**排在 try 之前** —— 排在 `po.observe()` 之后的话,`new PerformanceObserver`
//       抛错时那一行根本没执行过,**回落路压根没有保险**;
//     · **撤网落在 signal() 里**,不在武装处 —— 否则网撤在「信号还没发出去」之前,
//       而撤网后剩下的正是 BeginFrame 停摆下不回调的那两拍。
//   ⚠ 判据 = ⑦ 的 (d),但它**只守得住第一件**;后两件今天**没有判据**(统筹裁定不再往这一族
//   加正则,整族转 SL-431)。所以**别把这里读成「信号不会永不发出」** —— 那句话要三件都在
//   才成立,而只有一件被机器守着。
// 判据两格,各守一件事:web-preview/tests/smoke-embedded-resources.mjs ⑦(源码形态)与
//   smoke-first-frame-page.mjs A(**三页都跑**,量 `信号时刻 − first-paint 时刻` 的符号
//   与帧差)。⚠ 后者以前只跑 output —— 而 output 正是三页里唯一测不出本缺陷的那个。
// ⚠ 这一段是**本机 headless Chrome 的实测 + 对真机现象的解释**,不是对 WebView2 宿主窗口的
//   直接观测(本机做不了像素级观测)。**真机上的 first-paint 从来没有被量过** ——
//   抓取包只记信号时刻与放行时刻,没有 paint 时间戳(同族的诊断缺口见 SL-426)。
//   真机终验指标仍是「开窗时看不见那段白」。
//
// 【[SL-429] 真 WebView2 宿主 A/B —— 量级不能从 headless 外推】
// pluginval `--repeat 10` + DBWIN 捕获,**同一台机、同一棵树、前后相隔几分钟**:只把三份
// index.html 在 3a759a7(DCL 触发)与本卡(paint 触发)之间切换并各重编一次。
// `first-frame signal after N ms` 的中位数 [区间]:
//     插件      修前              修后              差
//     Input     796 [765, 835]    828 [806, 862]    +31 ms
//     Monitor   794 [767, 863]    811 [790, 857]    +17 ms
//     Output    838 [773, 909]    886 [858, 941]    +48 ms
// 六档全是 10/10 `firstFrame`、0 `timeout`、0 `fallback`、10/10 `still parked`。
// 两件事由它定下来:
//   ① 上面【为什么挪走】那段点名的风险(挪出可视区 ⇒ Chromium 停 BeginFrame ⇒ paint 记录
//      永不到达 ⇒ 每次开窗吃满页内 2.5s 保险)**在真宿主上没有命中** —— 判别很直接:保险是
//      2500 ms,而实测信号全部落在 1 秒以内,且放行原因一次 timeout 都没有。
//   ② **代价是 +17~48 ms,不是 headless 那个 +262 ms**。headless 上那 200 ms 是 localhost
//      http 取两条渲染阻塞 CSS 的往返;真机走 ResourceProvider 从内存里拿,这一段几乎没有。
// ⚠ **下面是推断,不是实测**(真机量不到 first-paint):`修后 − 修前 = first-paint − DCL`
//   ⇒ 本机真宿主上 first-paint 比 DCL 晚 **17~48 ms**;而修前信号 = DCL + 两层 rAF
//   ≈ DCL + 16~33 ms ⇒ **修前「信号 − first-paint」落在 ±几十毫秒之内,符号不确定** ——
//   与 headless 上 monitor 那一档同一个量级,而 `kRevealSettleMs` 就有 32 ms(⚠ [SL-436]
//   按当时的 32 算,这里的余量是 −32+32=**0**,贴着零、不宽裕;改到 64 之后是
//   −32+64=**32 ms**——是余量从贴着 0 抬到 32 ms,**不是「翻了一倍」**:0 的一倍还是 0,
//   这里翻倍的是 `kRevealSettleMs` 这个常量本身,不是这个余量)。
//   ⇒ **本机这台真宿主上,修前也不该闪**,与 SL-420「本机复现不出 Input 的白」一致,
//   [SL-436] 改到 64 后这条判断比改前更站得住,不因这次改动而削弱。
// ⇒ 口径:input 那一档是「**机制在真宿主上也站得住**(按上面的反推,修前信号可能早于首帧),
//   但**余量是否足以在用户机上致白没有被测量**」,**不是**「已定」。用户机上的 first-paint
//   同样从来没被量过(SL-426)。本修法的价值在于把这一整类可能性**结构性地消掉**,
//   代价 17~48 ms;它**没有**证明用户看到的那几段白一定就是它。
//
// 【为什么首帧信号到了还要再等一拍】两层 rAF 保证的是「已绘的那一帧已经**提交**给合成器」,
// 从提交到**上屏**还差一拍(合成器要拿到帧、Windows 要把那块位图推到桌面)。信号一到就立刻
// 挪回来,仍然可能在这一拍里露出 WebView2 的底 —— 那正是用户看到的「白一瞬」。
// 所以 `onFirstFrame()` 只**武装**,真正放行落在后面的 25Hz tick 上。
//
// ⚠ 这一拍是 **tick 数 ∧ 毫秒下界**两个条件,缺一不可 —— #247 复审【重要】② 点出的正是
//   「只数 tick」那一版名不副实:信号到达点相对 tick 相位是随机的,只等「下一个 tick」时
//   实际等待落在 (0, 40 ms] 上,**下界是 0**;本卡自己的 pluginval 数表里最小一次只有 4 ms,
//   **小于**一个 60Hz 合成帧。也就是说头注承诺的「等出那一拍」在相当一部分开窗上没兑现。
//   现在两个条件:
//     · `kRevealSettleTicks`  —— 至少再回一次消息循环(闸门状态的翻转只发生在 tick 上);
//     · `kRevealSettleMs`     —— **[SL-247] 初定 32 ms**,把上面那个 0 下界堵死。当时的取法:
//                                32 ms = 一个 60Hz 合成帧(16.7 ms)**再加约一帧余量**;它
//                                **不是**「两个合成帧」(那是 33.3 ms,#247 复审第 2 轮抓到
//                                过这句写过头)。⚠ **[SL-436] 2026-09-19 改到 64 ms**(用户
//                                拍板;monitor 页面真机上白过一次,把可容忍的合成器延迟窗口
//                                翻倍)——64 已经不是「一帧余量」那套讲法能套的数,上面这段
//                                32 的推导是**历史设计动机**,不随之改写。
//   两者都满足才放。tick 被卡住时只会更晚,不会更早(那种情形下整个界面本来就不动)。
//   ⚠ 代价上界的**通用公式**(不是凑数):`kRevealSettleTicks = 1`(本仓现值)这个前提下,
//     worst_case = kRevealSettleMs + P,P = settle tick 的周期(本仓 40 ms,即 25Hz)——这是
//     **紧界**,逼近但取不到。`kRevealSettleTicks = T > 1` 时通式是
//     worst_case = max(T·P, kRevealSettleMs + P),只有 (T−1)·P ≤ kRevealSettleMs 时才退化成
//     上式(synchain-bridge 同名头文件 SL-436 那轮复审用 T=3 的反例证过,改 T 前必须重推)。
//   代入:旧值(32)时 32+40=**72 ms**,与上面 `:55` 那组实测的上界一致;⚠ 本段原来写的是
//     「最坏约 80 ms」——**那是既有错数,不是这次改动引入的**,72 才是按公式算出来的对的数,
//     顺带订正。改到 64 后:64+40=**104 ms**,而占位段本身约 0.9 秒。
//
// 【放行路一览】firstFrame(前端 rAF 信号,**唯一的正常路**,武装后下一个 tick 生效)/
// timeout(kRevealFallbackMs,信号没来时兜底,绝不允许「永远不放行」)/
// fallback(看门狗切了兜底面板,面板自己铺满本组件,这不算「放行」)。
// navFinished **不在此列**,它只被记下来供诊断。
//
// 本类是**纯逻辑**、不碰 JUCE 组件:WebViewHost.cpp 不进任何测试目标,把判定收在这里才有
// 单测(tests/webview/test_plugin_common.cpp)。几何那一半见下面的 parkedBounds()。
// -----------------------------------------------------------------------------
class RevealGate
{
public:
    // 首帧信号没来时的兜底上界。取 3s:比 kAfterNavBudgetMs(5s)短,
    // 保证「宁可早放行看见一点白」也不会拖到看门狗兜底面板那一步。
    // ⚠ [SL-378] 这个「短」有**两个半边**,缺一不成立:
    //   · **常量关系**:由 WebViewHost.h 里那句 static_assert 在每次编译上守;
    //   · **前提**:两个预算要**同时起算** —— 看门狗那 5s 是导航事件到达时顺延出来的,
    //     而顺延只在本次加载尝试的**首次**导航事件上生效,靠 `beginLoadAttempt()` 复位
    //     `navBudgetApplied_`。这一半今天**没有用例守**(WebViewHost.cpp 不进任何测试目标),
    //     完整说明与「另立卡」的去向见 WebViewHost.h 那句 static_assert 上方的注释。
    static constexpr int kRevealFallbackMs = 3000;

    // [SL-376] 首帧信号到达后压住的**两个**条件,`onTick` 里必须同时满足才放行。
    // 为什么不能只有 tick 数(它的下界是 0),见头注【为什么首帧信号到了还要再等一拍】。
    static constexpr int kRevealSettleTicks = 1; // 至少再回一次消息循环
    static constexpr int kRevealSettleMs = 64; // [SL-436] 2026-09-19 由 32 改到 64,推导见上方头注

    // 一次新的加载尝试开始(构造 / retry 共用):重新武装,允许下一次导航再挪一次。
    void beginLoadAttempt() noexcept
    {
        parked_ = false;
        revealed_ = false;
        settling_ = false;
        settleTicksSeen_ = 0;
        settleAtMs_ = 0;
        navFinishedSeen_ = false;
        revealReason_ = "";
    }

    // 导航开始(WebView2 控制器已建好)。已经放行过就不再挪 —— 页面自己再导航一次时
    // 重新黑一下屏比那点白更难看。
    void onNavigationStarted(std::uint32_t nowMs) noexcept
    {
        if (revealed_ || parked_)
            return;
        parked_ = true;
        parkedAtMs_ = nowMs;
    }

    // 前端「首帧已绘」信号(__scvb__firstFrame)。**这里不放行**,只武装 —— 真正放行在后面的
    // onTick 上,要同时满足 kRevealSettleTicks 与 kRevealSettleMs,理由见头注。
    // 还没挪走时**也要记账**:记成已放行之后,后面那次 onNavigationStarted 就不会再把
    // 已经画好的页面挪走。
    //
    // 取 `nowMs` 是为了那条毫秒下界:信号到达的时刻只有调用方知道(它在消息线程上,
    // 与 25Hz tick 不同相),不传进来就只能拿「下一个 tick 的时刻」当起点,那等于把要量的
    // 那段时间自己抹掉。
    //
    // ⚠ [SL-437 复审] 前端那半边(web/*/index.html 的 signal())在 __JUCE__ 迟到时靠 2.5s
    // 保险重发一次 —— 这把「信号可能晚到」的窗口从两层 rAF 的几十毫秒量级,拉宽到了
    // 2.5 秒量级。机制(不下"没问题"的结论):
    //   · 若这条迟到的信号在 `parked_` 还是 true 时到达(常见情形):`settling_` 走正常
    //     的武装路径,后面的 25Hz tick 仍会按 kRevealSettleTicks/kRevealSettleMs 正常放行,
    //     只是**整段等待时间比平常长了 2.5s 左右**。
    //   · 若这条迟到的信号在 `kRevealFallbackMs`(3000ms,从 onNavigationStarted 起算)
    //     的超时兜底已经先放过行**之后**才到达:此时 `parked_` 已经是 false,上面
    //     `if (!parked_) { revealed_ = true; return; }` 分支接住,是**无害的空操作**——
    //     不会重复放行、不会崩溃。但那意味着这一次开窗其实是**走了兜底面板**这条路,
    //     不是「等到真的画过一帧再放行」这条本卡想保证的路;2.5s 的保险窗口与 3s 的
    //     兜底窗口余量并不宽(2.5s vs 3s,中间隔着的只是导航开始到本段脚本执行那一小段),
    //     两者谁先谁后没有判据钉着,真机上出现哪一种取决于当时的时序。
    void onFirstFrame(std::uint32_t nowMs) noexcept
    {
        if (!parked_)
        {
            revealed_ = true;
            return;
        }
        if (!settling_)
        {
            settling_ = true;
            settleTicksSeen_ = 0;
            settleAtMs_ = nowMs;
        }
    }

    // JUCE 的 pageFinishedLoading。[SL-376] **不放行**:它只保证文档下载完,不保证任何一帧
    // 已经合成 —— 拿它放行正是 v5.6.10 那一瞬白的来源(头注【只认首帧】)。只记账,
    // 供超时那一行诊断区分「页面 load 完了但首帧信号没发」与「导航压根没走完」。
    void onNavigationFinished() noexcept { navFinishedSeen_ = true; }

    // 25Hz tick:先结算首帧信号的那一拍,再看超时兜底。差值走 uint32 → int32:
    // getMillisecondCounter 每 ~49 天回绕,直接比大小会在回绕点把「刚挪走」算成
    // 「早该放行」(或反过来永不放行)。
    void onTick(std::uint32_t nowMs) noexcept
    {
        if (!parked_)
            return;
        if (settling_)
        {
            // 首帧信号已到:tick 数与毫秒下界**同时**满足才放。**这一段必须先于超时判定**,
            // 否则信号踩着 3s 线到达时会被记成 timeout,数表里就凭空多出一次「信号缺席」。
            // 毫秒差同样走 uint32 → int32(回绕安全),与下面的超时判定同一手法。
            const bool ticksDone = (++settleTicksSeen_ >= kRevealSettleTicks);
            const bool msDone = (static_cast<std::int32_t>(nowMs - settleAtMs_) >= kRevealSettleMs);
            if (ticksDone && msDone)
                reveal("firstFrame");
            return;
        }
        if (static_cast<std::int32_t>(nowMs - parkedAtMs_) >= kRevealFallbackMs)
            reveal("timeout");
    }

    // 切兜底面板:面板自己铺满本组件,闸门不该再按住 WebView 的位置。
    void onFallbackShown() noexcept { reveal("fallback"); }

    bool parked() const noexcept { return parked_; }
    // 最近一次放行的原因(诊断行用;从未放行过是空串)。
    const char* lastRevealReason() const noexcept { return revealReason_; }
    // [SL-376] pageFinishedLoading 到过没有。**与放行无关**,只进 timeout 那一行诊断。
    bool navigationFinishedSeen() const noexcept { return navFinishedSeen_; }

private:
    void reveal(const char* why) noexcept
    {
        revealed_ = true;
        settling_ = false;
        if (!parked_)
            return;
        parked_ = false;
        revealReason_ = why;
    }

    bool parked_ = false;
    bool revealed_ = false;
    bool settling_ = false; // [SL-376] 首帧信号已到、正在压那一拍
    bool navFinishedSeen_ = false;
    int settleTicksSeen_ = 0;
    std::uint32_t settleAtMs_ = 0; // [SL-376] 首帧信号到达的时刻(毫秒下界的起点)
    std::uint32_t parkedAtMs_ = 0;
    const char* revealReason_ = "";
};

// 遮挡期间 WebView 子窗口该占的矩形:**尺寸一字不改**(页面不 reflow、Chromium 仍按真实
// 视口出帧),整块平移到本组件右侧一个窗口宽之外。JUCE 会把它换算成宿主 HWND 客户区坐标喂给
// ICoreWebView2Controller::put_Bounds,而子窗口恒被 Windows 裁到父窗口客户区内 ⇒ 屏上看不见。
//
// ⚠ 上面这句依赖一条**没有任何用例守着**的前提,写在这里免得下一个人排查空转时白走一圈
// (#241 复审):**JUCE 必须把一次「只改 x、宽高不变」的 setBounds 也转发到 put_Bounds**。
// 读实现核过,成立:`ComponentMovementWatcher::componentMovedOrResized` 会按
// **顶层坐标**重算 `wasMoved`(纯位移 ⇒ 位置变了 ⇒ 为真),然后调虚函数;而 WebView2 后端
// 那份覆写(juce_WebBrowserComponent_windows.cpp)**两个标志都不看**,一律
// `setControlBounds(peer->getAreaCoveredBy(owner))`。
// 为什么值得写下来:这一环真断了的话,`parked_` 照样翻、诊断行照样打、C++ 用例照样绿,
// 而 WebView 从头到尾没挪过 —— 主修法完全空转且**一格都不会红**。
// 统筹裁定(#241 15:49 ②)**不在本卡补判据**,改由真机项兜:验收按**正向**指标判 ——
// 「开窗应看见一段约 1 秒的浅紫占位,再切到内容」;**仍见白/黑就说明位移没生效**。
// 不按「没有黑」判,因为闸门空转时(底色已改浅)那条照样满足。
// 本机 pluginval 实测的占位时长(revealed 的绝对 after 值):Input 925~1179 ms、
// Output 895~1165 ms,肉眼足够看清。
// ⚠ 这是**加了 kRevealSettleMs 下界之后**用最终代码重跑的数(#247 复审第 2 轮点名要重测:
//   下界改的正是被测的那一段,拿旧数当「实测」就是假句)。加下界之前是 Input 827~978 /
//   Output 943~1115 ms,SL-370 当时是 895~1220 ms —— 三组量级相同,都是约 1 秒。
// 宽度为 0(还没 resizeToDesignBox)时退一步用 1,保证平移量恒为正、不会原地不动。
inline juce::Rectangle<int> parkedBounds(juce::Rectangle<int> visible) noexcept
{
    const int step = juce::jmax(1, visible.getWidth());
    return visible.withX(visible.getX() + step * 2);
}

} // namespace scvb::webview
