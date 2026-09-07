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
// 客户区之外,那块地方由 WebViewHost::paint 用 shellBackdrop()(成品外壳中点色)铺占位。
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
//   0 timeout**([SL-376] 之后 navFinished 本就不再放行)。
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
// 一直可见」既保住了泵,又没有放过任何一帧白:那一段屏上是我方 paint 的 shellBackdrop()。
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
//   放行只认 `firstFrame`(前端在 DOMContentLoaded 后嵌套两层 rAF 才发,⇒ 前一帧确已合成),
//   外加 `timeout` 兜底与 `fallback` 顶替。
//
// 【为什么首帧信号到了还要再等一拍】两层 rAF 保证的是「前一帧已经**提交**给合成器」,
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
//     · `kRevealSettleMs`     —— 至少 32 ms,即 60Hz 上两个合成帧,把上面那个 0 下界堵死。
//   两者都满足才放。tick 被卡住时只会更晚,不会更早(那种情形下整个界面本来就不动)。
//   代价上界:tick 粒度 40 ms + 下界 32 ms ⇒ 最坏约 80 ms,而占位段本身约 0.9 秒。
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
    static constexpr int kRevealFallbackMs = 3000;

    // [SL-376] 首帧信号到达后压住的**两个**条件,`onTick` 里必须同时满足才放行。
    // 为什么不能只有 tick 数(它的下界是 0),见头注【为什么首帧信号到了还要再等一拍】。
    static constexpr int kRevealSettleTicks = 1; // 至少再回一次消息循环
    static constexpr int kRevealSettleMs = 32; // 至少两个 60Hz 合成帧

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
// 本机 pluginval 实测的占位时长(revealed 的绝对 after 值):Input 827~978 ms、
// Output 943~1115 ms([SL-376] 复测;SL-370 当时是 895~1220 ms),肉眼足够看清。
// 宽度为 0(还没 resizeToDesignBox)时退一步用 1,保证平移量恒为正、不会原地不动。
inline juce::Rectangle<int> parkedBounds(juce::Rectangle<int> visible) noexcept
{
    const int step = juce::jmax(1, visible.getWidth());
    return visible.withX(visible.getX() + step * 2);
}

} // namespace scvb::webview
