// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Input · 首启轻量引导 mini tour([J80] / 07 T48)
// -----------------------------------------------------------------------------
// 为什么 Input 也要引导:真实使用顺序里 Input 是用户见到的**第一个** SCVB 界面
// (每条人声轨最后一格插一个),而 J62 当时把 tour 只判给了 Output —— 于是第一个界面零引导。
// [J80] 立的是轻量版:独立语言卡(web/shared/lang-start.js)→ **5 步** mini tour。
//
// 与 Output 44 步导览的关系:
//   • **画法共用**(蒙版/亮区/说明框/箭头 = web/shared/tour-paint.js),两个插件的引导
//     长一个样;
//   • **步骤机各写各的**:Output 那套带跨 tab 翻页、demo 数据注入、per-step 视图动作、
//     「演示数据」badge —— Input 单页一件都不需要,照搬只会把 400 行不走的代码搬过来。
//     本文件的步骤机 = 5 步、无 tab、无 demo 注入(轻量的本义:讲的是**真实**界面)。
//
// 职责边界(硬):
//   • 不写 state、不触引擎;**唯一桥调用 = setGuideSeen**(首启链的完成与 Skip 都置位,
//     J50a 镜像;header「?」**重看**走 start({replay:true}),结束时不调用它)。
//     该名字当前停在 bridge.js 的 PENDING_FUNCS.input(契约 §3 尚无此函数),变更说明见
//     docs/contract-changes/20260825-input-guide-seen.md;native 未落地时桥上不挂,
//     调用直接落空 —— 本会话内靠 app.js 的会话标记不重弹,**不假装写了** state。
//   • **无声音,仅视觉 + 文字**(05 §2.6)。
//   • 两段导出(与 output/tour.js 同构):纯函数(无 DOM,node 可直接 import 断言)
//     + createInputTour(opts)(DOM 接线)。模块顶层零副作用、零 document 触碰。
//
// 真源:
//   • 规格 = masterPlan/plan/adjudications.md 的 J80 行 + 05 §3 文末 J80 节;
//   • 交互规则(左键任意处下一步 / 说明框按钮例外 / Esc=Skip / ←→ / aria-live / 无声音)
//     = 05 §2.6「交互规则」同款;
//   • 第 ④ 步文案口径 = 九条硬约束第 3 条(12 §3.4 / 05 §2.4),场景化改写但用词同源:
//     「设计行为,不是 bug」「检测不到健康 Output 时(未装、未连上、对端已退出)自动切回直通」
//     「不会因为只装了一个插件就得到一条没有声音的轨道」——**禁止**「永久静音 / 哑轨」类旧表述。
// =============================================================================

import {
    TOUR_BASE_CSS,
    drawMask,
    spotRectOf,
    placeCallout,
} from "../shared/tour-paint.js";

// =============================================================================
// 一、纯函数与常量(无 DOM;node 侧断言面)
// =============================================================================

/**
 * mini tour 步骤清单([J80] 5 步基线,步号连续 1..5)。
 *   • anchor = spotlight 目标(`data-tour` 键);null = 无 spotlight(居中说明框)。
 * 增删步须先改 05 §3 的 J80 节,再回改本表。
 */
export const TOUR_IN_STEPS = Object.freeze(
    [
        { anchor: null }, // 1 欢迎:这是 SCVB 的采集端(居中卡)
        { anchor: "group" }, // 2 组选择 A–H(与总线上 Output 同组)
        { anchor: "channel" }, // 3 channel 轮带(同组内每轨一个号)
        { anchor: "pill" }, // 4 连接状态 + 「现在没声音是正常的」
        { anchor: "help" }, // 5 完整控制在 Output + 重看入口自指(J62 同款末步)
    ].map(Object.freeze),
);

/** 各步锚点(smoke 断言「首步无 spotlight、末步 = help 自指」)。 */
export const TOUR_IN_ANCHORS = Object.freeze(
    TOUR_IN_STEPS.map((s) => s.anchor),
);

/**
 * 首启链(语言卡 → mini tour)是否该起(判据与 Output 侧 shouldShowGuide / shouldShowLangStart
 * 完全同构,J50a 镜像):工程 `ui.guide_seen === false` 且 系统级全局默认 `guide_seen_global === false`。
 * 「已看过」承诺跨工程成立:置位时 setGuideSeen(true, true) 连全局位一起写,新工程
 * guide_seen=false 但全局位已置 → 不再自动弹。
 * @param {object} state    Input state 子树(含 ui.guide_seen)
 * @param {object} snapshot §3.1 快照(含 guide_seen_global)
 * @param {boolean} closedThisSession 本会话是否已走过/跳过引导
 */
export function shouldShowInputGuide(state, snapshot, closedThisSession) {
    if (closedThisSession) return false;
    if (!snapshot) return false; // 首帧未到:契约 §0.6 门控,不抢在快照前弹
    const ui = (state && state.ui) || {};
    return ui.guide_seen === false && snapshot.guide_seen_global === false;
}

// =============================================================================
// 二、DOM 接线
// =============================================================================

/**
 * @param {{
 *   root: Document|Element,
 *   card: Element,             // #ipt-shell(overlay 挂载点,position:relative)
 *   bridge: object|null,
 *   getT: () => object,        // 当前语言字典
 *   onEnd?: (completed:boolean) => void,  // 结束回调(app.js 落会话标记 + 重渲染)
 * }} opts
 */
export function createInputTour(opts) {
    const root = opts.root;
    const card = opts.card;
    const bridge = opts.bridge || null;
    const getT = opts.getT || (() => ({}));
    const onEnd = opts.onEnd || (() => {});

    async function call(name, ...args) {
        if (!bridge || typeof bridge[name] !== "function") return null;
        try {
            return await bridge[name](...args);
        } catch (e) {
            console.warn(
                "SCVB tour-in:bridge." + name + "() 调用失败 —— " + e.message,
            );
            return null;
        }
    }

    // ---------------------------------------------------------------- 状态
    const N = TOUR_IN_STEPS.length;
    let active = false;
    let step = 1; // 1..N
    let persistOnEnd = true; // 本次是否首启链(= 结束时写已读位);重看置 false

    // ---------------------------------------------------------------- 组件样式
    // 全部走共用画法的规则表(零页内专有规则:Input 的 mini tour 没有工作流程大卡这类特殊形态步)。
    const STYLE_ID = "scvb-tour-in-style";
    function ensureStyle() {
        if (root.getElementById(STYLE_ID)) return;
        const style = document.createElement("style");
        style.id = STYLE_ID;
        style.textContent = TOUR_BASE_CSS.join("\n");
        root.head.appendChild(style);
    }

    // ---------------------------------------------------------------- DOM
    let overlay = null;
    let mask = null;
    let callout = null;
    let titleEl = null;
    let bodyEl = null;
    let hintEl = null;
    let stepEl = null;
    let dotsEl = null;
    let skipEl = null;
    let prevEl = null;
    let nextEl = null;
    let arrowEl = null;

    function el(tag, cls) {
        const n = document.createElement(tag);
        if (cls) n.className = cls;
        return n;
    }

    function buildDom() {
        overlay = el("div", "tour-overlay");
        overlay.setAttribute("data-tour-overlay", "");
        overlay.hidden = true;

        mask = el("canvas", "tour-mask");
        mask.setAttribute("aria-hidden", "true");

        callout = el("div", "tour-callout");
        callout.setAttribute("role", "dialog");
        callout.setAttribute("tabindex", "-1");

        arrowEl = el("div", "tour-callout__arrow");
        arrowEl.setAttribute("aria-hidden", "true");

        const text = el("div", "tour-callout__text");
        text.setAttribute("aria-live", "polite");
        titleEl = el("p", "tour-callout__title");
        bodyEl = el("p", "tour-callout__body");
        hintEl = el("p", "tour-callout__hint");
        hintEl.hidden = true;
        text.append(titleEl, bodyEl, hintEl);

        const foot = el("div", "tour-callout__foot");
        const indicator = el("div", "tour-callout__indicator");
        indicator.setAttribute("aria-hidden", "true");
        stepEl = el("span", "tour-callout__step");
        dotsEl = el("span", "tour-callout__dots");
        indicator.append(stepEl, dotsEl);

        const actions = el("div", "tour-callout__actions");
        skipEl = el("button", "sc-btn sc-btn--dark");
        skipEl.type = "button";
        skipEl.setAttribute("data-tour-btn", "skip");
        prevEl = el("button", "sc-btn sc-btn--dark");
        prevEl.type = "button";
        prevEl.setAttribute("data-tour-btn", "prev");
        nextEl = el("button", "sc-btn sc-btn--cta");
        nextEl.type = "button";
        nextEl.setAttribute("data-tour-btn", "next");
        actions.append(skipEl, prevEl, nextEl);
        foot.append(indicator, actions);

        callout.append(arrowEl, text, foot);
        overlay.append(mask, callout);
        card.appendChild(overlay);
    }

    // ---------------------------------------------------------------- 步骤机
    function updateIndicator() {
        stepEl.textContent = step + "/" + N;
        const frag = document.createDocumentFragment();
        for (let i = 1; i <= N; i++) {
            const d = el("span", "tour-callout__dot");
            if (i === step) d.setAttribute("data-current", "1");
            frag.appendChild(d);
        }
        dotsEl.replaceChildren(frag);
    }

    function updateText() {
        const t = getT() || {};
        const titleKey = "tour-in.step" + step + ".title";
        const bodyKey = "tour-in.step" + step + ".body";
        titleEl.textContent =
            Object.prototype.hasOwnProperty.call(t, titleKey) && t[titleKey]
                ? t[titleKey]
                : titleKey;
        bodyEl.textContent =
            Object.prototype.hasOwnProperty.call(t, bodyKey) && t[bodyKey]
                ? t[bodyKey]
                : bodyKey;
        callout.setAttribute("aria-label", titleEl.textContent);

        // 首步正文含交互说明(05 §2.6 步骤 1;与 Output 共用同一条词条)。
        if (step === 1) {
            hintEl.hidden = false;
            hintEl.textContent = t["tour.clickAnywhere"] || "";
        } else {
            hintEl.hidden = true;
            hintEl.textContent = "";
        }

        skipEl.textContent = t["tour.skip"] || "Skip";
        prevEl.textContent = t["tour.prev"] || "Back";
        nextEl.textContent =
            step === N ? t["tour.done"] || "Done" : t["tour.next"] || "Next";

        // 首步无「上一步」可回(步号连续,1 为起点)。
        prevEl.disabled = step === 1;
        prevEl.setAttribute("aria-disabled", String(step === 1));
    }

    function showStep(i) {
        step = i;
        updateIndicator();
        updateText();
        // 双 rAF:等说明框换文后的布局(高度变化)与 scrollIntoView 落定再量几何。
        requestAnimationFrame(() => {
            requestAnimationFrame(() => {
                const spotLocal = spotRectOf(
                    root,
                    TOUR_IN_STEPS[step - 1].anchor,
                    mask,
                );
                drawMask(mask, card, spotLocal);
                placeCallout(callout, arrowEl, card, spotLocal);
            });
        });
    }

    // ---------------------------------------------------------------- 生命周期
    /**
     * @param {{replay?:boolean}} [o] `replay:true` = header「?」重看,结束时**不**写已读位
     *   (契约变更说明 20260825-input-guide-seen §四:本函数只是首启的写入口)。
     *   起步条件与首启完全一样 —— 重看不看 `guide_seen`,已置位也能再开。
     */
    function start(o) {
        if (active) return;
        active = true;
        persistOnEnd = !(o && o.replay);
        step = 1;
        overlay.hidden = false;
        if (callout) callout.focus({ preventScroll: true });
        showStep(1);
    }

    function endTour(completed) {
        if (!active) return;
        // 首启链里完成与 Skip 都置位(J50a 镜像:同步写系统级全局默认位);唯一桥调用。
        // 重看路径不置位 —— 已读位由首启那一次负责,重看只是再看一遍。
        if (persistOnEnd) call("setGuideSeen", true, true);
        active = false;
        overlay.hidden = true;
        onEnd(!!completed);
    }

    function next() {
        if (!active) return;
        if (step >= N) {
            endTour(true);
            return;
        }
        showStep(step + 1);
    }

    function prev() {
        if (!active || step <= 1) return;
        showStep(step - 1);
    }

    function skip() {
        endTour(false);
    }

    // ---------------------------------------------------------------- 事件接线
    function wire() {
        // 左键点击任意处 = 下一步(唯一例外:说明框自身按钮,各自动作)。右键/滚轮不触发 click。
        root.addEventListener(
            "click",
            (e) => {
                if (!active) return;
                if (e.button !== 0) return; // 仅左键推进
                const tgt = e.target instanceof Element ? e.target : null;
                if (tgt && tgt.closest("[data-tour-btn]")) return; // 按钮行各自动作
                next();
            },
            true,
        );

        // Esc=Skip,←/→=上一步/下一步(05 §2.6 键盘与 a11y)。
        root.addEventListener(
            "keydown",
            (e) => {
                if (!active) return;
                if (e.key === "Escape") {
                    e.preventDefault();
                    skip();
                } else if (e.key === "ArrowRight") {
                    e.preventDefault();
                    next();
                } else if (e.key === "ArrowLeft") {
                    e.preventDefault();
                    prev();
                }
            },
            true,
        );

        skipEl.addEventListener("click", skip);
        prevEl.addEventListener("click", prev);
        nextEl.addEventListener("click", next);
    }

    // ---------------------------------------------------------------- mount
    function mount() {
        ensureStyle();
        buildDom();
        wire();
    }

    return {
        mount,
        start,
        isActive: () => active,
        step: () => step,
    };
}
