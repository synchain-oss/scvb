// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// 裸 Alt 的「菜单模式」抑制(SL-227,用户 v5.5 复测)
// =============================================================================
// 症状:泳道页里 **Alt+滚轮(纵向缩放)用过之后,Ctrl+滚轮(横向缩放)概率性失效**。
// 用户自己的假设是「Alt 把焦点切走了」—— 方向可信,机理是 Windows 的老规矩:
// **单独按下再松开 Alt** 会激活窗口的菜单栏、进入「菜单模式」。宿主里承载 UI 的是
// WebView2,菜单模式一起,焦点就从 web 内容上挪走了;此后的滚轮消息不再路由给页面,
// 于是「Ctrl 横缩放没反应」。而 Alt+滚轮的手势天然以**裸 Alt 的 keyup 收尾**
// (按住 Alt → 滚 → 松开 Alt),正好每次都踩这一下 —— 「概率性」则取决于松开 Alt 时
// 有没有别的键还按着。
//
// 修:在文档级把**裸 Alt** 的默认动作拦掉(keydown 与 keyup 都要,菜单模式是 keyup
// 触发的,但 keydown 不拦某些宿主会先高亮菜单)。
//
// **只拦裸 Alt**,判据严格(见 shouldSuppressAltDefault):
//   · `e.key === "Alt"` —— 组合键里的 Alt(Alt+F 之类)`key` 是那个字母,不受影响;
//   · 不带 ctrl/shift/meta —— Ctrl+Alt 这类组合是宿主/系统的地盘,一律放行;
//   · 不拦 `Alt+F4`、`Alt+Tab`:前者 `key === "F4"`,后者根本到不了页面。
// 插件窗里本来就没有可供 Alt 访问的菜单栏,拦掉它不损失任何用户能做的事。
//
// ⚠ **真机验证项(无头验不了)**:菜单模式是 Windows + WebView2 的行为,无头浏览器
// 里既没有菜单栏、页面也恒有焦点,这段代码的**效果**在无头环境观察不到。
// 无头能验的只有「拦谁不拦谁」那一层(shouldSuppressAltDefault 的判据),已进冒烟。
// 真机复验口径(与 SL-221 同一档,记进 v5.6 清单):
//   ① Tab3 泳道里 Alt+滚轮改行高 → 松开 Alt → 立刻 Ctrl+滚轮,横向缩放必须生效;
//   ② 连做 10 次,一次都不许失效(原症状是概率性的,单次通过不算数);
//   ③ 期间宿主自身的 Alt 快捷键(若该 DAW 有)行为不变 —— 本抑制只吃裸 Alt。
// =============================================================================

/**
 * 这一记按键要不要拦掉默认动作(纯函数,node 侧可断言)。
 *
 * @param {KeyboardEvent|object} e 只读 key / ctrlKey / shiftKey / metaKey
 * @returns {boolean} true = 裸 Alt,拦;false = 其余一律放行
 */
export function shouldSuppressAltDefault(e) {
    const ev = e || {};
    if (ev.key !== "Alt") return false;
    // 组合键一律放行:那是宿主/系统的地盘,插件不该替它们做决定
    return !ev.ctrlKey && !ev.shiftKey && !ev.metaKey;
}

/**
 * 抑制裸 Alt 的菜单模式。
 *
 * 挂**捕获阶段**:菜单模式由宿主/浏览器在默认动作阶段起,越早拦下越稳;
 * 捕获阶段也保证不会被页面里某个 stopPropagation 的控件挡掉。
 *
 * @param {Document} doc 目标文档(默认 globalThis.document)
 * @returns {() => void} 解绑函数(测试用;生产挂上就不摘)
 */
export function suppressBareAltMenu(doc) {
    const d = doc || (typeof document !== "undefined" ? document : null);
    if (!d || typeof d.addEventListener !== "function") return () => {};
    const onKey = (e) => {
        if (shouldSuppressAltDefault(e)) e.preventDefault();
    };
    d.addEventListener("keydown", onKey, true);
    d.addEventListener("keyup", onKey, true);
    return () => {
        d.removeEventListener("keydown", onKey, true);
        d.removeEventListener("keyup", onKey, true);
    };
}
