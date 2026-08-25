// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB · tour 蒙版/亮区/说明框的公用画法(T48 [J80] 由 web/output/tour.js 提取)
// -----------------------------------------------------------------------------
// 为什么单立一件:Output 的 44 步全参数导览(T36b)与 Input 的 5 步 mini tour([J80] T48)
// 是两套**步骤机**,但「暗蒙版 + 挖亮圆角洞 + 四向避让的说明框」是同一套**画法**。
// 画法留两份就会各自漂移(羽化半径、内边距、避让间距、箭头方位),两个插件的引导看起来
// 不像同一个产品 —— 05 §2.6 的组件表本来就是一份规格。故把**无状态的几何与绘制**提到
// shared,步骤机/文案/锚点/桥调用留在各自页内。
//
// 职责边界(硬):
//   • 本文件不建 DOM、不挂事件、不读 i18n、不调桥、不写 state;只对调用方传进来的
//     canvas / callout / arrow 元素做「量 + 画 + 摆」。模块顶层零副作用、零 document 触碰
//     (check-i18n / 冒烟脚本 import 即跑,顶层碰 document 会当场炸)。
//   • 颜色一律读 tokens(--void / --r-callout / --dark-* ),零裸 hex。
//
// 真源:
//   • 组件与交互规格 = masterPlan/plan/05-ui-spec.md §2.6(蒙版/spotlight/说明框三行);
//   • 视觉 = docs/design/「SCVB 设计稿.dc.html」tour1-tour6 场景 + tokens.css。
// =============================================================================

/** spotlight 洞的圆角(与说明框同款浮层卡圆角,05 §4 --r-callout)。 */
export const SPOT_RADIUS = 16;

/** spotlight 洞内边距(设计 px):≥8(05 §2.6「内边距 ≥8 设计 px」)。 */
export const SPOT_PAD = 10;

/** 羽化半径(设计 px):spotlight 边缘柔化。 */
export const SPOT_FEATHER = 12;

/**
 * tour overlay 的基础样式规则(蒙版 / 说明框 / 步骤指示 / 按钮行 / 箭头)。
 * 调用方 `ensureStyle()` 里 `[...TOUR_BASE_CSS, ...页内专有规则].join("\n")`。
 * 选择器一律以 `.tour-` 开头且互不重复 —— 与页内追加规则拼接时顺序无关。
 */
export const TOUR_BASE_CSS = Object.freeze([
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
]);

/** 蒙版底色:读 `--void` token 并压到 0.72 透明度(读不到就用 token 默认值兜底)。 */
export function maskFill(card) {
    let v = "14,10,22";
    try {
        const c = getComputedStyle(card).getPropertyValue("--void");
        if (c && c.trim()) v = c.trim();
    } catch {
        /* 兜底 = --void 默认值 */
    }
    return "rgba(" + v + ", 0.72)";
}

/** 圆角矩形路径(半径按短边收敛,避免小目标画出花瓣形)。 */
export function roundRectPath(ctx, x, y, w, h, r) {
    const rr = Math.min(r, w / 2, h / 2);
    ctx.beginPath();
    ctx.moveTo(x + rr, y);
    ctx.arcTo(x + w, y, x + w, y + h, rr);
    ctx.arcTo(x + w, y + h, x, y + h, rr);
    ctx.arcTo(x, y + h, x, y, rr);
    ctx.arcTo(x, y, x + w, y, rr);
    ctx.closePath();
}

/**
 * 画一帧蒙版:整卡铺暗色,再用 destination-out + shadowBlur 挖出羽化的圆角亮区
 * (05 §2.6「实现建议 SVG mask 或 canvas 挖洞」)。
 * @param {HTMLCanvasElement} mask 蒙版 canvas
 * @param {Element} card 挂载卡(尺寸基准)
 * @param {{x:number,y:number,w:number,h:number}|null} spotLocal 亮区(卡内坐标);null = 无亮区
 */
export function drawMask(mask, card, spotLocal) {
    const dpr = (typeof window !== "undefined" && window.devicePixelRatio) || 1;
    const w = card.clientWidth;
    const h = card.clientHeight;
    mask.width = Math.round(w * dpr);
    mask.height = Math.round(h * dpr);
    const ctx = mask.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    ctx.fillStyle = maskFill(card);
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

/**
 * 量 `[data-tour="<anchor>"]` 的亮区矩形(卡内坐标,已含 SPOT_PAD 外扩)。
 * 目标在滚动区不可见时先最小滚动到可见(05 §2.6)。
 * @returns {{x:number,y:number,w:number,h:number}|null} 锚点不存在时 null(= 无亮区)
 */
export function spotRectOf(root, anchor, mask) {
    if (!anchor) return null;
    const target = root.querySelector('[data-tour="' + anchor + '"]');
    if (!target) return null;
    try {
        target.scrollIntoView({ block: "nearest", inline: "nearest" });
    } catch {
        /* 部分 WebView 不支持 options 参数,忽略即可 */
    }
    // 以蒙版 canvas 自身的包围盒为坐标基准(与 drawMask 的 clientWidth 同源),
    // 避免卡片若有 1px 描边时 border-box 与 padding-box 差 2px 导致亮区偏移。
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

/**
 * 说明框四向避让亮区、clamp 在设计盒内,并把箭头摆到朝向亮区的那条边上。
 * 无亮区(首步居中卡)时居中且隐藏箭头。
 * @param {Element} callout 说明框
 * @param {Element} arrowEl 箭头
 * @param {Element} card 挂载卡
 * @param {{x:number,y:number,w:number,h:number}|null} spotLocal 亮区(卡内坐标)
 */
export function placeCallout(callout, arrowEl, card, spotLocal) {
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
