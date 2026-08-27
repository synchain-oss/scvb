// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// 原生右键菜单抑制(SL-207,用户 v5.4 实测拍板 2026-08-27)
// =============================================================================
// 插件窗里冒出 WebView 的浏览器菜单(「重新加载」「另存为」「查看网页源代码」)
// 是穿帮 —— 用户看到的应该是一件 VST3,不是一个网页。
//
// **为什么落在 JS 这一层**(卡里问「查哪层干净」):WebView2 确实有
// `ICoreWebView2Settings::put_AreDefaultContextMenusEnabled`,但 **JUCE 8 没有把它
// 暴露出来** —— `juce_WebBrowserComponent_windows.cpp` 只设了 AreDevToolsEnabled /
// IsStatusBarEnabled / IsBuiltInErrorPageEnabled / UserAgent 四项,
// `WinWebView2BackendOptions` 也没有对应开关,拿不到底层 ICoreWebView2Settings。
// 走 C++ 就得改 vendored 的 JUCE —— 那是本仓不接受的依赖分叉。JS 这一层反而更干净:
// 一个监听器,两个插件通用,web-preview 里也能照样验。
//
// **可编辑控件例外**:输入框上的右键菜单是「剪切/复制/粘贴」的唯一入口(版本改名
// 那个字段就要用),一刀切会把粘贴堵死。故只在非可编辑目标上拦。
// =============================================================================

/**
 * 放行选择器 —— 只挑**真的能编辑文本**的那些(复审建议②)。
 *
 * 裸 `input` 不分 type 是错的:`<input type="range">` 是滑杆(本仓 Tab3 的参数条就是),
 * `checkbox` / `radio` / `button` / `color` 同理 —— 这些控件上的右键菜单没有任何
 * 「粘贴」可言,放行它们等于在插件里露出浏览器菜单,正是本卡要治的那件事。
 * 故改成**白名单**:只放文本族的 input,加 textarea 与 select
 * (select 的右键菜单归宿主系统,拦了反而怪)。
 */
const EDITABLE_SELECTOR = [
    'input[type="text"]',
    'input[type="search"]',
    'input[type="url"]',
    'input[type="tel"]',
    'input[type="email"]',
    'input[type="password"]',
    'input[type="number"]',
    "input:not([type])", // 缺省 type 即 text
    "textarea",
    "select",
    '[contenteditable=""]',
    '[contenteditable="true"]',
    '[contenteditable="plaintext-only"]',
].join(", ");

/**
 * 目标是不是可编辑控件(右键要放行的那一类)。
 *
 * 用 closest() 而不是只看 target 自己:contenteditable 容器里的行内元素
 * (`<b>` / `<span>`)才是真正的事件目标,只比对 tagName 会漏。
 * `contenteditable="false"` **不放行**(复审建议②):那是显式声明「这块不可编辑」,
 * 而裸 `[contenteditable]` 属性选择器会把它一起放过去。
 */
export function isEditableTarget(el) {
    if (!el || typeof el.closest !== "function") return false;
    return !!el.closest(EDITABLE_SELECTOR);
}

/**
 * 抑制原生右键菜单(可编辑控件除外)。
 *
 * @param {Document} doc 目标文档(默认 globalThis.document)
 * @returns {() => void} 解绑函数(测试用;生产挂上就不摘)
 */
export function disableNativeContextMenu(doc) {
    const d = doc || (typeof document !== "undefined" ? document : null);
    if (!d || typeof d.addEventListener !== "function") return () => {};
    const onMenu = (e) => {
        if (isEditableTarget(e.target)) return;
        e.preventDefault();
    };
    d.addEventListener("contextmenu", onMenu);
    return () => d.removeEventListener("contextmenu", onMenu);
}
