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
 * **文本族**放行选择器 —— 只挑**真的能编辑文本**的那些(复审建议②)。
 *
 * 裸 `input` 不分 type 是错的:`<input type="range">` 是滑杆(本仓 Tab3 的参数条就是),
 * `checkbox` / `radio` / `button` / `color` 同理 —— 这些控件上的右键菜单没有任何
 * 「粘贴」可言,放行它们等于在插件里露出浏览器菜单,正是本卡要治的那件事。
 * 故改成**白名单**:只放文本族的 input,加 textarea 与 contenteditable。
 *
 * ⚠ **`select` 不在这一份里**([SL-450]):下拉没有任何「文本撤销」语义。右键那一侧
 * 仍放行它,理由与文本无关 —— 见下面 `EDITABLE_SELECTOR`。文本族的**单一真源**就是
 * 这一份常量,两个消费面都从它派生,别再抄第二份。
 */
export const EDITABLE_TEXT_SELECTOR = [
    'input[type="text"]',
    'input[type="search"]',
    'input[type="url"]',
    'input[type="tel"]',
    'input[type="email"]',
    'input[type="password"]',
    'input[type="number"]',
    "input:not([type])", // 缺省 type 即 text
    "textarea",
    '[contenteditable=""]',
    '[contenteditable="true"]',
    '[contenteditable="plaintext-only"]',
].join(", ");

/**
 * **右键菜单**放行选择器 = 文本族 + `select`。
 *
 * 多出来的这一条是右键这一侧**独有**的理由:下拉的右键菜单归宿主系统,拦了反而怪。
 * 它与「这里能不能撤销文本」无关,所以 Ctrl+Z 那一侧不用本常量、用
 * `EDITABLE_TEXT_SELECTOR`([SL-450] 统筹裁定)。**两处用途正当不同,不是其中一处写错。**
 */
export const EDITABLE_SELECTOR = EDITABLE_TEXT_SELECTOR + ", select";

/**
 * 目标是不是**可编辑文本**控件(Ctrl+Z 要放行的那一类,[SL-450])。
 *
 * 用 closest() 而不是只看 target 自己:contenteditable 容器里的行内元素
 * (`<b>` / `<span>`)才是真正的事件目标,只比对 tagName 会漏 —— 而「只比 tagName」
 * 正是本函数在 Ctrl+Z 那一侧要替掉的东西:`a.tagName === "INPUT"` 把 Q 滑杆
 * (`type=range`)与两个勾选框一起当成了文本框,于是焦点停在滑杆上按 Ctrl+Z
 * 不被拦截、直接漏给宿主撤销栈,到不了插件。
 * `contenteditable="false"` **不放行**(复审建议②):那是显式声明「这块不可编辑」,
 * 而裸 `[contenteditable]` 属性选择器会把它一起放过去。
 */
export function isEditableTextTarget(el) {
    if (!el || typeof el.closest !== "function") return false;
    return !!el.closest(EDITABLE_TEXT_SELECTOR);
}

/**
 * 目标是不是可编辑控件(右键要放行的那一类:文本族 + select)。
 *
 * 判定面与 [SL-207] 落地时**逐字相同** —— [SL-450] 只把文本族拆成单独常量给 Ctrl+Z
 * 复用,本函数放行的元素集合一个都没有增减(泳道 pointerenter 收焦点那个消费点同此)。
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
