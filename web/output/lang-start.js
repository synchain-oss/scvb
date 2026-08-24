// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · 首启语言选择卡(T36b 第四轮)
// -----------------------------------------------------------------------------
// 职责边界:
//   • 首启链最前:guide overlay(红字九条页)会显示时(ui.guide_seen=false)先显示本卡,
//     用户选中语言后关闭本卡 → 露出红字九条页(已在选中语言下渲染)。
//   • 不属于 tour 步骤机:不写 state、不触引擎、不调任何桥函数;语言切换由 app.js 的
//     setLang 机制承担(契约 §1.30 setLang,沿用既有语言持久化,零新 state 键)。
//   • 两段导出(与 tour.js / tab-master.js 同构):纯函数(无 DOM,node 可直接 import 断言)
//     + createLangStart(opts)(DOM 接线)。模块顶层零副作用、零 document 触碰。
//   • 文案走 data-t(applyI18n 统一刷):标题三语常显、三按钮各用各自语言(三语值相同)。
//   • 点击契约:只有三个语言按钮可点(点击 = 切语言 + 关卡 → 红字九条页);卡片其余区域
//     与背景蒙版**不挂任何点击处理器** —— 点击一律无动作、绝不推进。「点击任意处 = 下一步」
//     的规则只从 tour 第 1 步(欢迎公告)起效,语言卡阶段不存在该规则(见 tour.js 的 click 接线)。
// =============================================================================

/** 可选语言(与 i18n.js 的 LANGS 同序;各按钮用各自语言显示)。 */
export const LANG_PICK_CODES = Object.freeze(["zh", "en", "fr"]);

/**
 * 语言选择卡是否该显示(判据与 tab-master.js 的 shouldShowGuide 完全同构,再加本会话已选标记):
 *   • 首帧未到(无快照)或本会话已关过引导页 / 已选过语言 ⇒ 不显示;
 *   • 工程 ui.guide_seen === false 且 系统级 guide_seen_global === false ⇒ 显示(首启最前)。
 * @param {object} state    工程 state 子树(含 ui.guide_seen)
 * @param {object} snapshot §1.1 快照(含 guide_seen_global)
 * @param {boolean} closedThisSession 本会话是否已关过引导页
 * @param {boolean} langChosen        本会话是否已选过语言
 */
export function shouldShowLangStart(
    state,
    snapshot,
    closedThisSession,
    langChosen,
) {
    if (closedThisSession || langChosen) return false;
    if (!snapshot) return false;
    const ui = (state && state.ui) || {};
    return ui.guide_seen === false && snapshot.guide_seen_global === false;
}

/**
 * @param {{
 *   root: Document|Element,
 *   card: Element,             // #card(overlay 挂载点,position:relative)
 *   onPick: (code:string) => void,  // 用户选中语言(app.js 切语言 + 关卡 + 落会话标记)
 * }} opts
 */
export function createLangStart(opts) {
    const root = opts.root;
    const card = opts.card;
    const onPick = opts.onPick || (() => {});

    let overlay = null;
    let panel = null;

    const STYLE_ID = "scvb-lang-start-style";
    function ensureStyle() {
        if (root.getElementById(STYLE_ID)) return;
        const style = document.createElement("style");
        style.id = STYLE_ID;
        // 玻璃风居中大卡:scrim 复用 --dark-scrim 玻璃令牌;面板复用 sc-modal 玻璃样式,
        // 仅补宽度/按钮排布;z-index 200 压在 guide overlay(100)之上、tour overlay(1000)之下。
        style.textContent = [
            ".lang-start { position: absolute; inset: 0; z-index: 200; display: flex; align-items: center; justify-content: center; background: var(--dark-scrim); -webkit-backdrop-filter: var(--dark-scrim-blur); backdrop-filter: var(--dark-scrim-blur); }",
            ".lang-start__panel { width: 400px; max-width: calc(100% - 2 * var(--sp-24)); text-align: center; }",
            ".lang-start__title { margin: 0 0 var(--sp-16); }",
            ".lang-start__btns { display: flex; flex-direction: column; gap: var(--sp-10); }",
            ".lang-start__btn { width: 100%; padding: var(--sp-8) var(--sp-11); font-size: var(--fs-125); }",
        ].join("\n");
        root.head.appendChild(style);
    }

    function el(tag, cls) {
        const n = document.createElement(tag);
        if (cls) n.className = cls;
        return n;
    }

    function buildDom() {
        overlay = el("div", "lang-start");
        overlay.setAttribute("data-gb", "lang-start");
        overlay.hidden = true;

        panel = el("div", "sc-modal lang-start__panel");
        panel.setAttribute("role", "dialog");
        panel.setAttribute("aria-modal", "true");
        panel.setAttribute("tabindex", "-1");
        const titleEl = el("p", "sc-modal__title lang-start__title");
        titleEl.id = "lang-start-title";
        titleEl.setAttribute("data-t", "lang-start.title");
        panel.setAttribute("aria-labelledby", "lang-start-title");

        // 只有这三个按钮挂 click;overlay(蒙版)与 panel(卡片)均无点击处理器,
        // 因此点卡外蒙版/卡内非按钮区一律无动作、绝不推进。
        const btns = el("div", "lang-start__btns");
        for (const code of LANG_PICK_CODES) {
            const b = el("button", "sc-btn lang-start__btn");
            b.type = "button";
            // data-lang-pick 而非 data-lang:避免命中 app.js 里 [data-lang] 的全局切语言绑定,防止双触发。
            b.setAttribute("data-lang-pick", code);
            b.setAttribute("data-t", "lang-start." + code);
            b.addEventListener("click", () => onPick(code));
            btns.appendChild(b);
        }

        panel.append(titleEl, btns);
        overlay.appendChild(panel);
        card.appendChild(overlay);
    }

    function setShown(shown) {
        if (!overlay) return;
        const was = !overlay.hidden;
        overlay.hidden = !shown;
        // 只在隐藏→显示跳变时聚焦(render 高频跑,重复 focus 会反复抢焦点)。
        if (shown && !was && panel) panel.focus({ preventScroll: true });
    }

    function mount() {
        ensureStyle();
        buildDom();
    }

    return {
        mount,
        setShown,
        isShown: () => (overlay ? !overlay.hidden : false),
    };
}
