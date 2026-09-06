// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_graphics/juce_graphics.h>

#include <cstdint>

namespace scvb::webview
{

// -----------------------------------------------------------------------------
// [SL-370] 开窗遮挡闸 —— 「WebView 子窗口此刻该不该待在可视区之外」的**唯一判定处**。
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
//     的 else 分支,而 `Options::keepPageLoadedWhenBrowserIsHidden` **默认 false**
//     (juce_WebBrowserComponent.h:432)⇒ `unloadPageWhenHidden` 为真 ⇒ JUCE 会把页面
//     `goToURL("about:blank")` 顶掉。隐藏一下就把正在加载的页面弄没了。
//   • `put_IsVisible(false)` 让 Chromium 把页面判成不可见,`requestAnimationFrame` 随之停摆
//     —— 而我们等的正是前端在 rAF 里发的「首帧已绘」信号,会互相等死。
//   • 零尺寸会把视口压成 0×0,页面按 0 宽布局、放回来时整页 reflow。
//   挪走则三条都不沾:尺寸不变、`owner.isShowing()` 不变、Chromium 仍认为自己可见。
//
// 【为什么等到 onNavigationStarted 才挪】SL-271 的重试泵挂在 `HostWebView::paint` 上,而挪出
// 可视区之后 JUCE 不再画它 ⇒ 泵停。导航开始 = WebView2 控制器已经建好(JUCE 是在控制器创建
// 完成回调里先 setWebViewPreferences 再 Navigate),而泵的条件正是
// `if (! hasBrowserBeenCreated())` —— 控制器建好之后它本来就是空调用。所以「控制器建好之前
// 一直可见」既保住了泵,又没有放过任何一帧白:那一段屏上是我方 paint 的 shellBackdrop()。
//
// 【三条放行路,谁先到算谁】firstFrame(前端 rAF 信号)/ navFinished(JUCE 的
// pageFinishedLoading,信号丢了也不会卡住)/ timeout(kRevealFallbackMs,前两条都没来时兜底,
// 绝不允许「永远不放行」)。
//
// 本类是**纯逻辑**、不碰 JUCE 组件:WebViewHost.cpp 不进任何测试目标,把判定收在这里才有
// 单测(tests/webview/test_plugin_common.cpp)。几何那一半见下面的 parkedBounds()。
// -----------------------------------------------------------------------------
class RevealGate
{
public:
    // 前两条放行路都没来时的兜底上界。取 3s:比 kAfterNavBudgetMs(5s)短,
    // 保证「宁可早放行看见一点白」也不会拖到看门狗兜底面板那一步。
    static constexpr int kRevealFallbackMs = 3000;

    // 一次新的加载尝试开始(构造 / retry 共用):重新武装,允许下一次导航再挪一次。
    void beginLoadAttempt() noexcept
    {
        parked_ = false;
        revealed_ = false;
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

    // 前端「首帧已绘」信号(__scvb__firstFrame)。**即使还没挪走也要记账**:
    // 记成已放行之后,后面那次 onNavigationStarted 就不会再把已经画好的页面挪走。
    void onFirstFrame() noexcept { reveal("firstFrame"); }

    // JUCE 的 pageFinishedLoading。信号那条路断了(前端脚本整体没跑起来)时由它兜。
    void onNavigationFinished() noexcept { reveal("navFinished"); }

    // 25Hz tick 的超时兜底。差值走 uint32 → int32:getMillisecondCounter 每 ~49 天回绕,
    // 直接比大小会在回绕点把「刚挪走」算成「早该放行」(或反过来永不放行)。
    void onTick(std::uint32_t nowMs) noexcept
    {
        if (!parked_)
            return;
        if (static_cast<std::int32_t>(nowMs - parkedAtMs_) >= kRevealFallbackMs)
            reveal("timeout");
    }

    // 切兜底面板:面板自己铺满本组件,闸门不该再按住 WebView 的位置。
    void onFallbackShown() noexcept { reveal("fallback"); }

    bool parked() const noexcept { return parked_; }
    // 最近一次放行的原因(诊断行用;从未放行过是空串)。
    const char* lastRevealReason() const noexcept { return revealReason_; }

private:
    void reveal(const char* why) noexcept
    {
        revealed_ = true;
        if (!parked_)
            return;
        parked_ = false;
        revealReason_ = why;
    }

    bool parked_ = false;
    bool revealed_ = false;
    std::uint32_t parkedAtMs_ = 0;
    const char* revealReason_ = "";
};

// 遮挡期间 WebView 子窗口该占的矩形:**尺寸一字不改**(页面不 reflow、Chromium 仍按真实
// 视口出帧),整块平移到本组件右侧一个窗口宽之外。JUCE 会把它换算成宿主 HWND 客户区坐标喂给
// ICoreWebView2Controller::put_Bounds,而子窗口恒被 Windows 裁到父窗口客户区内 ⇒ 屏上看不见。
// 宽度为 0(还没 resizeToDesignBox)时退一步用 1,保证平移量恒为正、不会原地不动。
inline juce::Rectangle<int> parkedBounds(juce::Rectangle<int> visible) noexcept
{
    const int step = juce::jmax(1, visible.getWidth());
    return visible.withX(visible.getX() + step * 2);
}

} // namespace scvb::webview
