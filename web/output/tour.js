// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · 首启交互式引导 tour(J62 / 05 §2.6;T36b 交付物)
// -----------------------------------------------------------------------------
// 职责边界:
//   • 本文件只管 tour 本体:深色玻璃蒙版 + spotlight 挖亮 + 说明框 callout +
//     步骤机(全参数导览 44 步、步号连续)+「演示数据」badge。全部为 Output 页内的 overlay 层。
//   • 外壳(header / 横幅 / footer / 缩放 / 引导页 / tab 路由 / 询问步)在 web/output/app.js。
//     本文件不写 state、不触引擎、除 setTourSeen 外不调任何桥函数(§2.6 demo 注入纪律);
//     期间真实事件照常入内存但不渲染,由 app.js 的 viewStore() 切换消费面。
//
// 两段导出(与 tab-master/tab-tracks/tab-settings 同构):
//   ① 纯函数(无 DOM,node 可直接 import 断言):TOUR_STEPS / TOUR_ANCHORS /
//     shouldShowTourAsk / buildDemoStore;
//   ② createTour(opts)(DOM 接线)。模块顶层零副作用、零 document 触碰,否则
//     check-i18n / 冒烟脚本 import 即炸。
//
// 真源:
//   • 交互规则与步骤清单 = masterPlan/plan/05-ui-spec.md §2.6(组件与交互真源;步数经用户
//     2026-08-23 指令由 7 步基线扩为全参数导览 44 步,05 §2.6 待统筹勘误,deviations 登记);
//   • 视觉(蒙版/亮区/说明框)= docs/design/「SCVB 设计稿.dc.html」tour1-tour6 场景 +
//     tokens.css 的 --r-callout(设计稿 1105/1957 行 tour 卡与 spotlight 挖洞在用)、
//     --dark-modal / --dark-solid(深色玻璃 #241f2e 底)。
//   • 桥面 = 冻结契约 docs/SCVB_CONTRACT.md §1.33 setTourSeen(seen, alsoGlobal=true)。
// =============================================================================

import { FIFTEEN_TRACKS } from "../shared/mock-data.js";

// =============================================================================
// 一、纯函数与常量(无 DOM;node 侧断言面)
// =============================================================================

/** spotlight 洞的圆角(与说明框同款浮层卡圆角,05 §4 --r-callout)。 */
export const SPOT_RADIUS = 16;

/** spotlight 洞内边距(设计 px):≥8(05 §2.6「内边距 ≥8 设计 px」)。 */
export const SPOT_PAD = 10;

/** 羽化半径(设计 px):spotlight 边缘柔化。 */
export const SPOT_FEATHER = 12;

/**
 * tour 步骤清单(用户 2026-08-23 指令:7 步基线作废,扩为全参数导览 44 步,步号连续 1..N;
 * 每参数一步,含 T33 波形页控件;05 §2.6 待统筹勘误,deviations 登记)。
 *   • anchor = spotlight 目标(data-tour 键);null = 无 spotlight(居中说明框)。
 *   • 首启语言选择已移出本表(T36b 第四轮),归 web/output/lang-start.js 独立 overlay。
 *   • tab    = 该步讲解对象所在的 tab;null = 留在当前 tab。
 * 步数即 7,增删步须先改 05 §2.6,再回改本表(05 §2.6「增删步须回本表改」)。
 */
export const TOUR_STEPS = Object.freeze(
    [
        { anchor: null, tab: "master" }, // 1
        { anchor: null, tab: "master" }, // 2 工作流程与优先级(居中大卡)
        { anchor: "tab1", tab: "master" }, // 3
        { anchor: "group", tab: "master" }, // 4
        { anchor: "cap", tab: "master" }, // 5
        { anchor: "an", tab: "master" }, // 6
        { anchor: "out", tab: "master" }, // 7
        { anchor: "dist", tab: "master" }, // 8
        { anchor: "width", tab: "master" }, // 9
        { anchor: "msbalance", tab: "master" }, // 10
        { anchor: "leadselect", tab: "master" }, // 11
        { anchor: "range", tab: "master" }, // 12
        { anchor: "transition", tab: "master" }, // 13
        { anchor: "curve", tab: "master" }, // 14
        { anchor: "tab2", tab: "tracks" }, // 15
        { anchor: "trackrow", tab: "tracks" }, // 16
        { anchor: "pan", tab: "tracks" }, // 17
        { anchor: "widthknob", tab: "tracks" }, // 18
        { anchor: "vollevel", tab: "tracks" }, // 19
        { anchor: "prio", tab: "tracks" }, // 20
        { anchor: "leadlock", tab: "tracks" }, // 21
        { anchor: "volexempt", tab: "tracks" }, // 22
        { anchor: "autopan", tab: "tracks" }, // 23
        { anchor: "pair", tab: "tracks" }, // 24
        { anchor: "freeze", tab: "tracks" }, // 25
        { anchor: "enable", tab: "tracks" }, // 26
        { anchor: "tab3", tab: "wave" }, // 27
        { anchor: "vad", tab: "wave" }, // 28
        { anchor: "segments", tab: "wave" }, // 29
        { anchor: "actions", tab: "wave" }, // 30
        { anchor: "lanes", tab: "wave", action: "zoomLanes" }, // 31
        { anchor: "selection", tab: "wave", action: "showDemoSelection" }, // 32
        { anchor: "wave-panel", tab: "wave" }, // 33
        { anchor: "inspector", tab: "wave", action: "showDemoSegment" }, // 34
        { anchor: "tab4", tab: "settings" }, // 35
        { anchor: "guideblock", tab: "settings", action: "expandGuide" }, // 36
        { anchor: "loudnessmode", tab: "settings" }, // 37
        { anchor: "centerslot", tab: "settings" }, // 38
        { anchor: "scale", tab: "settings" }, // 39
        { anchor: "lang", tab: "settings" }, // 40
        { anchor: "storage", tab: "settings" }, // 41
        { anchor: "diagnostic", tab: "settings" }, // 42
        { anchor: "version", tab: "settings" }, // 43
        { anchor: "review", tab: "settings" }, // 44
    ].map(Object.freeze),
);

/** 有 spotlight 的步骤锚点(步 2..N;用于 smoke 断言「末步=review、锚点连续无跳号」)。 */
export const TOUR_ANCHORS = Object.freeze(TOUR_STEPS.map((s) => s.anchor));

/**
 * 询问步是否该弹(J62 首启顺序:红字页 → 询问步 → tour;J50a 镜像)。
 * 判据与 shouldShowGuide 完全同构(契约 §1.33 语义行 + §2.6「先读全局默认判定位」):
 *   工程 ui.tour_seen === false 且 系统级全局默认 tour_seen_global === false。
 * 「不再询问」承诺跨工程成立:完成/暂不都经 setTourSeen(true, true),新工程
 * tour_seen=false 但全局位已置 → 不再自动询问。
 * @param {object} state    工程 state 子树(含 ui.tour_seen)
 * @param {object} snapshot §1.1 快照(含 tour_seen_global)
 */
export function shouldShowTourAsk(state, snapshot) {
    if (!snapshot) return false;
    const ui = (state && state.ui) || {};
    return ui.tour_seen === false && snapshot.tour_seen_global === false;
}

/**
 * 询问步的**启动兜底**判定(05 §2.6 首启顺序 + 真实世界边角)。
 * 正常路径:guide 未看 → 红字页 → 「开始使用」→ 询问步(shouldShowTourAsk);
 * 但若某会话已过红字页(ui.guide_seen=true)却没答询问步(tour 未置位),红字页不会再弹、
 * 「开始使用」无处可点,询问步就永远没有入口 —— 故渲染层在 guide 已见 + tour 未置位时
 * **直接显示询问步**,与「开始使用」处理器共用 shouldShowTourAsk 同一判定。
 * @param {object} state    工程 state 子树(含 ui.guide_seen / ui.tour_seen)
 * @param {object} snapshot §1.1 快照(含 tour_seen_global)
 */
export function shouldAutoShowTourAsk(state, snapshot) {
    const ui = (state && state.ui) || {};
    return ui.guide_seen === true && shouldShowTourAsk(state, snapshot);
}

/**
 * 由 FIFTEEN_TRACKS(J62 健康满配 15 轨 demo)造一份与 app.js 事件仓同形的渲染视图。
 *
 * 为什么不直接改 app.js 的 store:tour 期间真实事件必须照常入内存(store 逐字节不变),
 * 渲染层却要显示 demo —— 于是 demo 是另一份对象,app.js 的 viewStore() 在 tour
 * 激活期把它当作唯一渲染源,结束即切回真实 store。纯 UI 展示层,不触引擎、不写 state。
 *
 * @returns {object} 与 app.js store 同形:{ ready, state, snapshot, params, conn,
 *   groups, playhead, segments, coverage, errors, unknownCodes, readOnly,
 *   noTimeline, loopMissing, session }
 */
export function buildDemoStore() {
    const snap = FIFTEEN_TRACKS.snapshot;
    // 快照专属键(session_guid / version / *_global / conn)不进 state 子树(§1.1 语义行);
    // state 子树由 makeTourDemoSnapshot 产出,深冻结,渲染侧只读、零就地改写。
    const stateFields = { ...snap };
    delete stateFields.session_guid;
    delete stateFields.version;
    delete stateFields.guide_seen_global;
    delete stateFields.tour_seen_global;
    delete stateFields.conn;

    const coverage = {};
    for (const c of FIFTEEN_TRACKS.captureProgress.channels) {
        coverage[c.ch] = c.coveragePct;
    }

    return {
        ready: true,
        state: stateFields,
        snapshot: snap,
        params: {
            values: { ...FIFTEEN_TRACKS.params.values },
            hostEcho: false,
            hostEchoAt: 0,
            full: true,
            versionActive: 1,
        },
        conn: snap.conn,
        groups: FIFTEEN_TRACKS.groups.groups_online,
        playhead: { ...FIFTEEN_TRACKS.playhead },
        segments: FIFTEEN_TRACKS.segments,
        coverage,
        errors: new Map(),
        unknownCodes: [],
        readOnly: false,
        noTimeline: false,
        loopMissing: false,
        // 本会话一次性判定(不入 state chunk);demo 里引导已关、无打印态。
        session: {
            writeConfirmSeen: true,
            printDoneUntil: 0,
            wasPrinting: false,
            guideClosed: true,
            rejectedPrintingUntil: 0,
        },
    };
}

// =============================================================================
// 二、DOM 接线
// =============================================================================

/**
 * @param {{
 *   root: Document|Element,
 *   card: Element,             // #card(overlay 挂载点,position:relative)
 *   bridge: object|null,
 *   getT: () => object,        // 当前语言字典
 *   activateTab: (name, {push?:boolean}) => void,  // app.js 的 tab 路由
 *   getActiveTab: () => string,                    // 当前 data-tab
 *   requestRender: () => void,                     // 结束还原真实渲染
 * }} opts
 */
export function createTour(opts) {
    const root = opts.root;
    const card = opts.card;
    const bridge = opts.bridge || null;
    const getT = opts.getT || (() => ({}));
    const activateTab = opts.activateTab || (() => {});
    const getActiveTab = opts.getActiveTab || (() => "master");
    const requestRender = opts.requestRender || (() => {});
    const expandGuide = opts.expandGuide || (() => {}); // 步 36 自动展开「查看全部九条」
    const zoomLanes = opts.zoomLanes || (() => {}); // 步 31 放大泳道(纯视图层)
    const showDemoSelection = opts.showDemoSelection || (() => {}); // 步 32 示例选区(纯视图层)
    const showDemoSegment = opts.showDemoSegment || (() => {}); // 步 34 段检查器示例选中(纯视图层)
    const resetWaveView = opts.resetWaveView || (() => {}); // 退出 31/32/34 或 tour 结束还原

    async function call(name, ...args) {
        if (!bridge || typeof bridge[name] !== "function") return null;
        try {
            return await bridge[name](...args);
        } catch (e) {
            console.warn(
                "SCVB tour:bridge." + name + "() 调用失败 —— " + e.message,
            );
            return null;
        }
    }

    // ---------------------------------------------------------------- 状态
    const N = TOUR_STEPS.length;
    let active = false;
    let step = 1; // 1..N
    let preTourTab = "master"; // 进入 tour 前的 tab(Skip 还原用)
    let demoStore = null;

    // ---------------------------------------------------------------- 组件样式
    // 全部颜色走 tokens(零裸 hex);--r-callout / --dark-modal / --dark-solid 同族于
    // 设计稿 tour 说明卡与 spotlight 挖洞(05 §4「深色雕刻窗与深色浮层」)。
    const STYLE_ID = "scvb-tour-style";
    function ensureStyle() {
        if (root.getElementById(STYLE_ID)) return;
        const style = document.createElement("style");
        style.id = STYLE_ID;
        style.textContent = [
            ".tour-overlay { position: absolute; inset: 0; z-index: 1000; pointer-events: auto; }",
            ".tour-mask { position: absolute; inset: 0; width: 100%; height: 100%; display: block; }",
            ".tour-callout { position: absolute; z-index: 1; box-sizing: border-box; width: 360px;",
            "  padding: var(--sp-16) var(--sp-18) var(--sp-14); border-radius: var(--r-callout);",
            "  background: linear-gradient(158deg, rgba(34, 31, 44, 0.72), rgba(20, 17, 28, 0.8));",
            "  -webkit-backdrop-filter: blur(16px) saturate(140%);",
            "  backdrop-filter: blur(16px) saturate(140%);",
            "  border: 1px solid rgba(var(--wh), 0.16); box-shadow: var(--dark-modal-shadow);",
            "  color: var(--txt-dark-2); outline: none; }",
            ".tour-callout__title { margin: 0 0 var(--sp-6); font-family: var(--ff-grotesk);",
            "  font-weight: 600; font-size: var(--fs-160); color: var(--txt-dark-1); }",
            ".tour-callout__body { font-size: var(--fs-125); line-height: 1.55;",
            "  color: var(--txt-dark-2); text-wrap: pretty; }",
            ".tour-callout__hint { margin-top: var(--sp-8); font-family: var(--ff-mono);",
            "  font-size: var(--fs-105); letter-spacing: var(--mono-ls-pill); color: var(--txt-dark-4); }",
            ".tour-callout__workflow { margin-top: var(--sp-12); display: flex; flex-direction: column; gap: var(--sp-8); }",
            ".tour-flow { display: flex; flex-wrap: wrap; align-items: center; gap: var(--sp-6); }",
            ".tour-flow__node { padding: var(--sp-6) var(--sp-10); border-radius: var(--r-pill); background: rgba(var(--wh), 0.12); border: 1px solid rgba(var(--wh), 0.2); font-family: var(--ff-sans); font-size: var(--fs-110); color: var(--txt-dark-2); white-space: nowrap; }",
            ".tour-flow__arrow { color: var(--txt-dark-4); font-family: var(--ff-mono); }",
            ".tour-flow__priority { padding: var(--sp-8) var(--sp-10); border-radius: var(--r-md); background: rgba(var(--wh), 0.08); border: 1px solid rgba(var(--wh), 0.16); font-size: var(--fs-115); color: var(--txt-dark-3); }",
            ".tour-callout__foot { display: flex; flex-direction: column; align-items: stretch;",
            "  gap: var(--sp-8); margin-top: var(--sp-12); }",
            ".tour-callout__indicator { display: flex; align-items: center; gap: var(--sp-10); flex: none; }",
            ".tour-callout__step { font-family: var(--ff-mono); font-size: var(--fs-105);",
            "  letter-spacing: var(--mono-ls); color: var(--txt-dark-3); font-variant-numeric: tabular-nums; }",
            ".tour-callout__dots { display: inline-flex; gap: 3px; }",
            ".tour-callout__dot { width: 4px; height: 4px; border-radius: 50%; background: rgba(var(--wh), 0.22); }",
            '.tour-callout__dot[data-current="1"] { background: var(--acc-cta); }',
            ".tour-callout__actions { display: flex; justify-content: flex-end; gap: var(--sp-8); flex: none; }",
            ".tour-callout__arrow { position: absolute; width: 12px; height: 12px;",
            "  background: var(--dark-solid); transform: rotate(45deg); pointer-events: none; }",
        ].join("\n");
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
    let workflowEl = null;
    let badge = null;

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

        workflowEl = el("div", "tour-callout__workflow");
        workflowEl.hidden = true;

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

        callout.append(arrowEl, text, workflowEl, foot);
        overlay.append(mask, callout);
        card.appendChild(overlay);

        badge = root.querySelector('[data-gb="header-demo-chip"]');
    }

    // ---------------------------------------------------------------- 蒙版绘制
    // canvas destination-out 挖洞 + shadowBlur 羽化(05 §2.6「实现建议 SVG mask 或
    // canvas 挖洞」);mask 填充色读 --void token,零硬编码颜色。
    function maskFill() {
        let v = "14,10,22";
        try {
            const c = getComputedStyle(card).getPropertyValue("--void");
            if (c && c.trim()) v = c.trim();
        } catch {
            /* 兜底 = --void 默认值 */
        }
        return "rgba(" + v + ", 0.72)";
    }

    function roundRectPath(ctx, x, y, w, h, r) {
        const rr = Math.min(r, w / 2, h / 2);
        ctx.beginPath();
        ctx.moveTo(x + rr, y);
        ctx.arcTo(x + w, y, x + w, y + h, rr);
        ctx.arcTo(x + w, y + h, x, y + h, rr);
        ctx.arcTo(x, y + h, x, y, rr);
        ctx.arcTo(x, y, x + w, y, rr);
        ctx.closePath();
    }

    function drawMask(spotLocal) {
        const dpr =
            (typeof window !== "undefined" && window.devicePixelRatio) || 1;
        const w = card.clientWidth;
        const h = card.clientHeight;
        mask.width = Math.round(w * dpr);
        mask.height = Math.round(h * dpr);
        const ctx = mask.getContext("2d");
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.clearRect(0, 0, w, h);

        ctx.fillStyle = maskFill();
        ctx.fillRect(0, 0, w, h);

        if (spotLocal && spotLocal.w > 0 && spotLocal.h > 0) {
            ctx.save();
            ctx.globalCompositeOperation = "destination-out";
            ctx.shadowColor = "rgba(0, 0, 0, 1)";
            ctx.shadowBlur = SPOT_FEATHER;
            ctx.beginPath();
            roundRectPath(
                ctx,
                spotLocal.x,
                spotLocal.y,
                spotLocal.w,
                spotLocal.h,
                SPOT_RADIUS,
            );
            ctx.fill();
            ctx.fill(); // 二画增强洞心全透,羽化只留在边缘
            ctx.restore();
        }
    }

    // ---------------------------------------------------------------- spotlight
    function computeSpot() {
        const cfg = TOUR_STEPS[step - 1];
        if (!cfg.anchor) return null;
        const target = root.querySelector('[data-tour="' + cfg.anchor + '"]');
        if (!target) return null;
        // 目标在滚动区不可见先滚动到可见(05 §2.6);block/inline=nearest 只做最小滚动。
        try {
            target.scrollIntoView({ block: "nearest", inline: "nearest" });
        } catch {
            /* 部分 WebView 不支持 options 参数,忽略即可 */
        }
        // 以蒙版 canvas 自身的包围盒为坐标基准(与 drawMask 的 clientWidth 同源),
        // 避免 #card 若有 1px 描边时 border-box 与 padding-box 差 2px 导致亮区偏移。
        const cr = mask.getBoundingClientRect();
        const tr = target.getBoundingClientRect();
        return {
            x: tr.left - cr.left - SPOT_PAD,
            y: tr.top - cr.top - SPOT_PAD,
            w: tr.width + SPOT_PAD * 2,
            h: tr.height + SPOT_PAD * 2,
        };
    }

    function clamp(v, lo, hi) {
        return v < lo ? lo : v > hi ? hi : v;
    }

    /** 说明框四向避让亮区、clamp 设计盒内;返回 callout 左上角与箭头方位。 */
    function placeCallout(spotLocal) {
        const cw = callout.offsetWidth;
        const ch = callout.offsetHeight;
        const W = card.clientWidth;
        const H = card.clientHeight;
        const margin = 12;
        const gap = 14;

        let x;
        let y;
        let edge = "none";
        let off = 0; // 箭头沿边缘的偏移(距 callout 左上角)

        if (!spotLocal) {
            x = (W - cw) / 2;
            y = (H - ch) / 2;
        } else {
            const sx = spotLocal.x;
            const sy = spotLocal.y;
            const sw = spotLocal.w;
            const sh = spotLocal.h;
            const cx = sx + sw / 2;
            const cy = sy + sh / 2;

            const fitsBelow = sy + sh + gap + ch <= H - margin;
            const fitsAbove = sy - gap - ch >= margin;
            const fitsRight = sx + sw + gap + cw <= W - margin;
            const fitsLeft = sx - gap - cw >= margin;

            if (fitsBelow) {
                edge = "top";
                x = clamp(cx - cw / 2, margin, W - margin - cw);
                y = sy + sh + gap;
                off = clamp(cx - x, 18, cw - 18);
            } else if (fitsAbove) {
                edge = "bottom";
                x = clamp(cx - cw / 2, margin, W - margin - cw);
                y = sy - gap - ch;
                off = clamp(cx - x, 18, cw - 18);
            } else if (fitsRight) {
                edge = "left";
                y = clamp(cy - ch / 2, margin, H - margin - ch);
                x = sx + sw + gap;
                off = clamp(cy - y, 18, ch - 18);
            } else if (fitsLeft) {
                edge = "right";
                y = clamp(cy - ch / 2, margin, H - margin - ch);
                x = sx - gap - cw;
                off = clamp(cy - y, 18, ch - 18);
            } else {
                // 四向都放不下(极小视口):贴目标右下,箭头朝上
                edge = "top";
                x = clamp(sx + sw + gap, margin, W - margin - cw);
                y = clamp(sy + sh + gap, margin, H - margin - ch);
                off = 18;
            }
        }

        callout.style.left = Math.round(x) + "px";
        callout.style.top = Math.round(y) + "px";

        const AW = 12;
        if (edge === "none") {
            arrowEl.hidden = true;
        } else {
            arrowEl.hidden = false;
            if (edge === "top") {
                arrowEl.style.left = Math.round(off - AW / 2) + "px";
                arrowEl.style.top = Math.round(-AW / 2) + "px";
            } else if (edge === "bottom") {
                arrowEl.style.left = Math.round(off - AW / 2) + "px";
                arrowEl.style.top = Math.round(ch - AW / 2) + "px";
            } else if (edge === "left") {
                arrowEl.style.left = Math.round(-AW / 2) + "px";
                arrowEl.style.top = Math.round(off - AW / 2) + "px";
            } else {
                arrowEl.style.left = Math.round(cw - AW / 2) + "px";
                arrowEl.style.top = Math.round(off - AW / 2) + "px";
            }
        }
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

    /** 步 2 工作流程图:五个节点箭头相连 + 优先级行(词条 workflow.*,三语)。 */
    function renderWorkflow(t) {
        const nodes = [
            "workflow.capture",
            "workflow.analyze",
            "workflow.tweak",
            "workflow.write",
            "workflow.manual",
        ];
        const row = el("div", "tour-flow");
        nodes.forEach((key, i) => {
            if (i > 0) {
                const arr = el("span", "tour-flow__arrow");
                arr.textContent = "→";
                row.appendChild(arr);
            }
            const node = el("span", "tour-flow__node");
            node.textContent = t[key] || key;
            row.appendChild(node);
        });
        const prio = el("div", "tour-flow__priority");
        prio.textContent = t["workflow.priority"] || "";
        workflowEl.replaceChildren(row, prio);
    }

    function updateText() {
        const t = getT() || {};
        const titleKey = "tour.step" + step + ".title";
        const bodyKey = "tour.step" + step + ".body";
        titleEl.textContent =
            Object.prototype.hasOwnProperty.call(t, titleKey) && t[titleKey]
                ? t[titleKey]
                : titleKey;
        bodyEl.textContent =
            Object.prototype.hasOwnProperty.call(t, bodyKey) && t[bodyKey]
                ? t[bodyKey]
                : bodyKey;
        callout.setAttribute("aria-label", titleEl.textContent);

        // 首步正文含交互说明 tour.clickAnywhere(05 §2.6 步骤 1)。
        if (step === 1) {
            hintEl.hidden = false;
            hintEl.textContent = t["tour.clickAnywhere"] || "";
        } else {
            hintEl.hidden = true;
            hintEl.textContent = "";
        }

        // 步 2 工作流程图(节点 + 优先级行;居中大卡)
        if (step === 2) {
            workflowEl.hidden = false;
            renderWorkflow(t);
        } else {
            workflowEl.hidden = true;
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
        // 退出 wave 视图增强步(31 放大泳道 / 32 示例选区 / 34 示例选中段)时还原。
        const leaving = TOUR_STEPS[step - 1];
        if (
            leaving &&
            (leaving.action === "zoomLanes" ||
                leaving.action === "showDemoSelection" ||
                leaving.action === "showDemoSegment")
        ) {
            resetWaveView();
        }
        step = i;
        const cfg = TOUR_STEPS[step - 1];
        // 跨 tab 自动翻页:tour 期间 tab 切换为 UI 本地态,不经 setActiveTab 写 state(§2.6)。
        if (cfg.tab) activateTab(cfg.tab, { push: false });
        // per-step 动作钩子(步 31 放大泳道 / 步 32 示例选区 / 步 34 示例选中段 / 步 36 自动展开九条)
        if (cfg.action === "expandGuide") expandGuide();
        else if (cfg.action === "zoomLanes") zoomLanes();
        else if (cfg.action === "showDemoSelection") showDemoSelection();
        else if (cfg.action === "showDemoSegment") showDemoSegment();
        updateIndicator();
        updateText();
        // 双 rAF:等 tab 切换(display 变化)与 scrollIntoView 后布局落定再量几何。
        requestAnimationFrame(() => {
            requestAnimationFrame(() => {
                const spotLocal = computeSpot();
                drawMask(spotLocal);
                placeCallout(spotLocal);
            });
        });
    }

    // ---------------------------------------------------------------- 生命周期
    function start() {
        if (active) return;
        preTourTab = getActiveTab();
        demoStore = buildDemoStore();
        active = true;
        step = 1;
        overlay.hidden = false;
        if (badge) {
            badge.hidden = false;
            // 提升到蒙版之上:demo badge 是「tour 激活期常显」的例外件,不得被蒙版压暗。
            badge.style.position = "relative";
            badge.style.zIndex = "1001";
        }
        if (callout) callout.focus({ preventScroll: true });
        showStep(1);
    }

    function endTour(completed) {
        if (!active) return;
        resetWaveView(); // 还原缩放 + 清选区/段选中(步 30/31/33 视图增强)
        // 完成与 Skip 都置位(§2.6:两者都经 setTourSeen(true, true));唯一桥调用。
        call("setTourSeen", true, true);
        active = false;
        demoStore = null;
        overlay.hidden = true;
        if (badge) {
            badge.hidden = true;
            badge.style.position = "";
            badge.style.zIndex = "";
        }
        // 结束按结束位置同步一次(§2.6):完成=设置 tab;Skip=还原进入前 tab。
        const target = completed ? getActiveTab() : preTourTab;
        activateTab(target, { push: true });
        requestRender();
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
                // 蒙版 canvas 在最上拦截指针,目标元素本身收不到事件 → 按坐标命中目标。
                const hitAt = (sel) => {
                    const els = root.querySelectorAll(sel);
                    for (const el of els) {
                        const r = el.getBoundingClientRect();
                        if (
                            e.clientX >= r.left &&
                            e.clientX <= r.right &&
                            e.clientY >= r.top &&
                            e.clientY <= r.bottom
                        )
                            return el;
                    }
                    return null;
                };
                const cfg = TOUR_STEPS[step - 1];
                next();
            },
            true,
        );

        // Esc=Skip,←/→=上一步/下一步(§2.6 键盘与 a11y)。
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
        demoStore: () => demoStore,
        step: () => step,
    };
}
