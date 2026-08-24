// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB · pan 轨迹图(可复用件;T43 交付 / [J75] A 节)
// -----------------------------------------------------------------------------
// 05 J75 A 逐字落地:
//   • x = 工程时间线,**轴语义/缩放/滚动与 Tab3 泳道一致**(复用 T33 的
//     `canvas/timeline.js` 视口模型,不另写一份换算);
//   • y = pan,显示口径与 Tab1 曲线编辑器一致 —— 曲线编辑器的「角度域」就是
//     `angle ∈ −100..100`(契约 §1.17 / 05 §6.2 的刻度 −100/−50/0/+50/+100),
//     故本图 y 轴逐字取同一值域与同一组刻度(**回流⑤ 的 `θ=width%×0.6` 是
//     Width 读数的换算,与 pan 无关,不在此套用**;偏差已登记 PR 差异清单);
//   • 每轨一条曲线 = 该轨**最终打印 pan**;数据源 = §2.8 段表的 `segments[].pan`
//     (auto 段与 user_edited 段同在一张表里,即「自动+手动合成后」);
//   • **无分段覆盖的区间不画线** —— 段与段之间只要有间隙就断线,间隙是乐句之间的
//     静默还是整段没采到的空当,在这里是同一件事(数据源 = 段表本身的 activity);
//   • 立体声轨画 pan 中心线(width 张开度带 v1 不入本图,见 J75 A 的「后续增强」注);
//   • 播放头竖线 + 跟随模式(默认开;手动缩放/拖拽即脱离,「回到播放头」恢复)。
//
// **复用契约(T46 Monitor)**:本模块不认 store、不认契约事件、不认 i18n ——
// 调用方经 `getSeries()` 喂「已经算好的 15 条折线」,经 `getPalette()` 喂色板,
// 文案一律由调用方写进 DOM。Monitor 直接复用本文件,零重复实现(07 T46 卡)。
//
// 分层与降级照 05 §6.1:静态层(网格 + 15 条折线)脏标记重绘、播放头走 DOM 竖线
// 由 `canvas/playhead.js` 的 rAF 插值驱动;空闲零 rAF。超预算的降采样见
// `decimateRun()`(降级序列之前的第一道闸:同一像素列内的点先塌掉)。
//
// 层次说明:本文件在 `web/shared/` 却 import `web/output/canvas/*` —— 那四件套
// 的文件头逐字写着「复用契约(T34 / T43)」,是 T33 为本卡预先切出来的可复用件,
// 不是 Output 页面的私有实现;此处按其声明消费,不另起炉灶(T43 卡「必须复用」)。
// =============================================================================

import {
    clampViewport,
    createTimeline,
    pxPerSec,
    spanOf,
    timeToX,
    xToTime,
    zoomLabel,
    ZOOM_STEP,
    MIN_SPAN_S,
} from "../output/canvas/timeline.js";
import {
    backingScale,
    resizeCanvas,
    observeResolution,
} from "../output/canvas/hidpi.js";
import { createLayerStack } from "../output/canvas/layers.js";
import { createPlayhead } from "../output/canvas/playhead.js";
import { resolveTrackPalette, rgbaOf } from "./track-colors.js";

// =============================================================================
// 一、纯函数(无 DOM,node 可直接 import 断言)
// =============================================================================

/** pan 值域 = 曲线编辑器的角度域(契约 §1.17 `angle −100..100`)。 */
export const PAN_MIN = -100;
export const PAN_MAX = 100;

/** y 轴刻度(与 05 §6.2 曲线编辑器的角度刻度逐字同组)。 */
export const PAN_TICKS = Object.freeze([100, 50, 0, -50, -100]);

/** 两段之间「算连着」的容差(秒)。段表的 t1S/t0S 都是 2 位小数,10ms 足够。 */
export const CONTIGUOUS_EPS_S = 0.01;

/** 跟随模式:重定位后播放头落在视口的位置比(0=左缘,1=右缘)。 */
export const FOLLOW_LEAD = 0.4;

/** 跟随模式:播放头落在这条带内就不动视口(避免每帧微调抖动)。 */
export const FOLLOW_BAND_LO = 0.05;
export const FOLLOW_BAND_HI = 0.85;

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

function clamp(lo, hi, v) {
    return v < lo ? lo : v > hi ? hi : v;
}

/**
 * pan → 舞台 y(CSS px)。**+100(右)在顶、−100(左)在底** ——
 * 与分布图「横位 = 声像、左在左」是两套轴,故 y 轴刻度必须带 L/R 字样(词条侧承担)。
 */
export function panToY(pan, plotH) {
    const p = clamp(PAN_MIN, PAN_MAX, num(pan, 0));
    return ((PAN_MAX - p) / (PAN_MAX - PAN_MIN)) * num(plotH, 0);
}

/**
 * 段表 → 折线段组(**断线的唯一判据在这里**)。
 *
 * 每段是一条**水平线**(段内 pan 恒定 = 该段最终打印值);相邻两段首尾相接时用
 * 竖直连接线连起来(引擎按 ramp 过渡,80ms 在任何缩放档下都不足 1px,故画成竖直
 * 台阶而不是斜线 —— 画斜线会在放到最大时变成一条**假的**渐变轨迹);中间只要有
 * 间隙就**另起一段**,即 J75 A 的「无分段覆盖的区间不画线」。
 *
 * @param {{t0S:number, t1S:number, pan:number}[]} segments 单轨段表(§2.8)
 * @returns {{tS:number, pan:number}[][]} 折线段组;每组 ≥2 点,组间断开
 */
export function runsOfSegments(segments) {
    const segs = (Array.isArray(segments) ? segments : [])
        .filter(
            (s) =>
                s &&
                Number.isFinite(s.t0S) &&
                Number.isFinite(s.t1S) &&
                s.t1S > s.t0S,
        )
        .slice()
        .sort((a, b) => a.t0S - b.t0S);
    const runs = [];
    let cur = null;
    let prevEnd = 0;
    for (const s of segs) {
        const pan = clamp(PAN_MIN, PAN_MAX, num(s.pan, 0));
        if (!cur || s.t0S - prevEnd > CONTIGUOUS_EPS_S) {
            cur = [];
            runs.push(cur);
        }
        cur.push({ tS: s.t0S, pan }, { tS: s.t1S, pan });
        prevEnd = s.t1S;
    }
    return runs;
}

/**
 * 同一像素列内的点先塌掉(降采样;05 §6.1「必要时降采样」的第一道闸)。
 *
 * 塌掉的条件是「同一列 **且** 与前后两点 pan 都相同」——**三点都要看**:
 * 只比前一点的话,`(t,p) (t,p) (t,q)` 这种台阶的**拐点前一点**会被吃掉,折线就从
 * 水平+竖直的台阶变成一条斜线,即凭空画出一段并不存在的渐变轨迹。
 * 段内首尾两点 pan 相同,故一条被压到 1px 宽的段会退化成两点(仍画得出 1px 竖痕),
 * 而不是消失。
 *
 * @param {{tS:number, pan:number}[]} points 单个折线段
 * @param {(tS:number)=>number} toX 时间 → 舞台 x
 */
export function decimateRun(points, toX) {
    const pts = Array.isArray(points) ? points : [];
    if (pts.length <= 2) return pts.slice();
    const out = [pts[0]];
    let lastCol = Math.round(toX(pts[0].tS));
    for (let i = 1; i < pts.length - 1; i++) {
        const col = Math.round(toX(pts[i].tS));
        const prev = out[out.length - 1];
        if (
            col === lastCol &&
            pts[i].pan === prev.pan &&
            pts[i].pan === pts[i + 1].pan
        ) {
            continue;
        }
        out.push(pts[i]);
        lastCol = col;
    }
    out.push(pts[pts.length - 1]);
    return out;
}

/** 折线段与视口有无交集(视口外整段跳过,不进 path)。 */
export function runIntersects(points, vp) {
    if (!Array.isArray(points) || points.length === 0) return false;
    return points[points.length - 1].tS >= vp.startS && points[0].tS <= vp.endS;
}

/**
 * 跟随模式的视口推进。播放头落在 [FOLLOW_BAND_LO, FOLLOW_BAND_HI] 带内不动视口;
 * 越带就把它重新摆到 FOLLOW_LEAD 处(向前翻页的手感,不是逐帧居中的漂移)。
 * @returns {{startS:number,endS:number}} 夹取后的新视口(与旧视口相等即调用方自会去重)
 */
export function followViewport(vp, durationS, tS) {
    const span = spanOf(vp);
    const t = num(tS, 0);
    const p = span > 0 ? (t - vp.startS) / span : 0;
    if (p >= FOLLOW_BAND_LO && p <= FOLLOW_BAND_HI) return vp;
    return clampViewport(
        { startS: t - span * FOLLOW_LEAD, endS: t + span * (1 - FOLLOW_LEAD) },
        durationS,
    );
}

/** 两视口是否逐字相同(跟随推进的去重判据)。 */
export function sameViewport(a, b) {
    return !!a && !!b && a.startS === b.startS && a.endS === b.endS;
}

/** `mm:ss`(时间轴刻度标签;与契约的秒口径同源,只是显示层)。 */
export function mmss(tS) {
    const t = Math.max(0, Math.round(num(tS, 0)));
    const m = Math.floor(t / 60);
    const s = t % 60;
    return m + ":" + String(s).padStart(2, "0");
}

/**
 * 时间轴刻度步长(秒)—— 取 1/2/5 × 10ⁿ 中「屏上间距 ≥ 64px」的最小档。
 * 与 Tab3 标尺同族的「nice number」口径;此处不引 Tab3 的小节换算(本图无 tempo 面)。
 */
export function tickStepS(spanS, stageW) {
    const target = 64; // 两条刻度线之间的最小屏距(CSS px)
    const perSec = stageW > 0 ? stageW / Math.max(spanS, MIN_SPAN_S) : 0;
    if (!(perSec > 0)) return 60;
    const raw = target / perSec;
    const pow = Math.pow(10, Math.floor(Math.log10(Math.max(raw, 1e-6))));
    for (const m of [1, 2, 5, 10]) {
        if (pow * m >= raw) return pow * m;
    }
    return pow * 10;
}

/** 视口内的刻度时刻(升序;含起点对齐)。 */
export function ticksIn(vp, stageW) {
    const step = tickStepS(spanOf(vp), stageW);
    const out = [];
    let t = Math.ceil(vp.startS / step) * step;
    for (let i = 0; i < 256 && t <= vp.endS; i++) {
        out.push(Math.round(t * 1000) / 1000);
        t += step;
    }
    return out;
}

// =============================================================================
// 二、DOM 接线
// =============================================================================

/** 画布配色的兜底(计算样式取不到时用;与 tokens.css 同值)。 */
const STYLE_FALLBACK = {
    grid: "rgba(255, 255, 255, 0.2)",
    axis: "rgba(255, 255, 255, 0.45)",
    base: "rgba(255, 255, 255, 0.35)",
    text: "#7a7a84",
    mono: '"IBM Plex Mono", "Noto Sans SC", ui-monospace, monospace',
};

/** 非高亮轨在 hover 联动时的淡出档(纯展示,05 J75 B「hover 联动高亮」)。 */
export const DIM_ALPHA = 0.16;

/** 常态线宽 / 高亮线宽(CSS px)。 */
export const LINE_W = 1.6;
export const LINE_W_HI = 2.4;

/** 底轴留白(时间刻度文字行)与上下内边距(CSS px)。 */
const AXIS_H = 14;
const PAD_TOP = 6;

/** 距画布左右缘多近的刻度标签改靠边对齐(半个「0:00」宽,CSS px)。 */
const EDGE_LABEL_PAD = 18;

function readStyles(el) {
    if (!el || typeof globalThis.getComputedStyle !== "function") {
        return { ...STYLE_FALLBACK };
    }
    let cs;
    try {
        cs = globalThis.getComputedStyle(el);
    } catch {
        return { ...STYLE_FALLBACK };
    }
    const pick = (name, dflt) => {
        const v = cs && cs.getPropertyValue ? cs.getPropertyValue(name) : "";
        const s = String(v || "").trim();
        return s || dflt;
    };
    return {
        grid: pick("--w-20", STYLE_FALLBACK.grid),
        axis: pick("--w-45", STYLE_FALLBACK.axis),
        base: pick("--w-35", STYLE_FALLBACK.base),
        text: pick("--txt-4", STYLE_FALLBACK.text),
        mono: pick("--ff-mono", STYLE_FALLBACK.mono),
    };
}

/**
 * 轨迹图。
 *
 * @param {object} opts
 * @param {HTMLCanvasElement} opts.canvas 静态层画布(网格 + 15 条折线)
 * @param {HTMLElement} [opts.playheadEl] 播放头竖线(DOM,与 Tab3 同款:只写 left/hidden)
 * @param {HTMLElement} [opts.followBtn] 「回到播放头」按钮(脱离跟随时才显示)
 * @param {HTMLElement} [opts.zoomEl] 缩放档位读数节点(`×4.0 / 75s`,aria-live 由 HTML 侧给)
 * @param {() => {ch:number, stereo:boolean, runs:{tS:number,pan:number}[][]}[]} opts.getSeries
 * @param {() => number} opts.getDurationS 工程时间线全长(秒)
 * @param {() => number} [opts.getUiScale] 05 §6.1 的 `k = uiScale × dpr`
 * @param {() => boolean} [opts.isVisible] 本图当前是否可见(不可见时不重绘、不起 rAF)
 * @param {() => void} [opts.onFollowChange] 跟随态变化(调用方据此刷按钮/词条)
 */
export function createTrajectoryChart(opts) {
    const o = opts || {};
    const canvas = o.canvas || null;
    const getSeries =
        typeof o.getSeries === "function" ? o.getSeries : () => [];
    const getDurationS =
        typeof o.getDurationS === "function" ? o.getDurationS : () => 0;
    const getUiScale =
        typeof o.getUiScale === "function" ? o.getUiScale : () => 1;
    const isVisible =
        typeof o.isVisible === "function" ? o.isVisible : () => true;
    const onFollowChange =
        typeof o.onFollowChange === "function" ? o.onFollowChange : () => {};

    const local = {
        following: true, // J75 A:跟随模式**默认开**
        // 最近一帧 §2.6 事件。不在前台时插值循环要**停掉**(空闲零 rAF,05 §6.1),
        // 而停掉就丢了位置 —— 故自留一份,`invalidate()` 回到前台时按它补一帧。
        playheadEv: null,
        highlight: 0, // 图例 hover 联动的轨号(0 = 无)
        palette: resolveTrackPalette(canvas),
        styles: readStyles(canvas),
        drag: null, // {pointerId, x0, startS0}
        stageW: 0,
        stageH: 0,
    };

    const timeline = createTimeline({
        durationS: Math.max(getDurationS(), MIN_SPAN_S),
        onChange: () => {
            layers.invalidateStatic();
            playhead.refresh();
            paintZoomLabel();
        },
    });

    const layers = createLayerStack({
        drawStatic: (frame) => paintStatic(frame),
        drawDynamic: () => false, // 播放头走 DOM 竖线;静态层之外无逐帧诉求(空闲零 rAF)
    });

    const playhead = createPlayhead({
        degradeLevel: () => layers.governor.level(),
        apply: (tS) => {
            const el = o.playheadEl;
            if (!el) return;
            const x = timeToX(timeline.viewport(), local.stageW, tS);
            const visible = local.stageW > 0 && x >= 0 && x <= local.stageW;
            if (el.hidden === visible) el.hidden = !visible;
            if (visible) el.style.left = x + "px";
        },
    });

    // ------------------------------------------------------------------ 几何
    function measure() {
        if (!canvas || !canvas.parentElement) return false;
        const host = canvas.parentElement;
        const w = host.clientWidth;
        const h = host.clientHeight;
        if (!(w > 0) || !(h > 0)) return false;
        const changed = w !== local.stageW || h !== local.stageH;
        local.stageW = w;
        local.stageH = h;
        return changed;
    }

    /** 折线区高(总高 − 底轴文字行 − 顶部留白)。 */
    function plotH() {
        return Math.max(local.stageH - AXIS_H - PAD_TOP, 1);
    }

    // ------------------------------------------------------------------ 绘制
    function paintStatic() {
        if (!canvas || !isVisible()) return;
        measure();
        const k = backingScale(num(getUiScale(), 1), dpr());
        const ctx = resizeCanvas(canvas, local.stageW, local.stageH, k);
        if (!ctx) return;
        ctx.clearRect(0, 0, local.stageW, local.stageH);

        const vp = timeline.viewport();
        const H = plotH();
        const st = local.styles;

        // ---- 网格:pan 五档横线(0 线加重,与曲线编辑器的零轴同款)+ 时间竖线
        ctx.lineWidth = 1;
        for (const p of PAN_TICKS) {
            const y = Math.round(PAD_TOP + panToY(p, H)) + 0.5;
            ctx.strokeStyle = p === 0 ? st.axis : st.grid;
            ctx.beginPath();
            ctx.moveTo(0, y);
            ctx.lineTo(local.stageW, y);
            ctx.stroke();
        }
        ctx.strokeStyle = st.grid;
        ctx.fillStyle = st.text;
        ctx.font = `9px ${st.mono}`;
        ctx.textAlign = "center";
        ctx.textBaseline = "top";
        for (const t of ticksIn(vp, local.stageW)) {
            const x = Math.round(timeToX(vp, local.stageW, t)) + 0.5;
            ctx.beginPath();
            ctx.moveTo(x, PAD_TOP);
            ctx.lineTo(x, PAD_TOP + H);
            ctx.stroke();
            // 贴边的刻度改成靠边对齐:居中会让首末标签被画布裁掉一半
            // (「5:00」显示成「5:」),看着像渲染坏了。
            ctx.textAlign =
                x < EDGE_LABEL_PAD
                    ? "left"
                    : x > local.stageW - EDGE_LABEL_PAD
                      ? "right"
                      : "center";
            ctx.fillText(mmss(t), x, PAD_TOP + H + 2);
        }
        ctx.textAlign = "center";
        // 底基线(与分布图 .dist-plot__base 同角色)
        ctx.strokeStyle = st.base;
        ctx.beginPath();
        const by = Math.round(PAD_TOP + H) + 0.5;
        ctx.moveTo(0, by);
        ctx.lineTo(local.stageW, by);
        ctx.stroke();

        // ---- 15 条折线。高亮轨最后画(压在最上面),其余按轨号顺序。
        const series = getSeries() || [];
        const hi = local.highlight;
        const toX = (tS) => timeToX(vp, local.stageW, tS);
        const ordered = hi
            ? series
                  .filter((s) => s.ch !== hi)
                  .concat(series.filter((s) => s.ch === hi))
            : series;
        ctx.lineJoin = "round";
        ctx.lineCap = "butt";
        for (const s of ordered) {
            const on = !hi || s.ch === hi;
            const rgb = local.palette[(s.ch - 1) % local.palette.length];
            ctx.strokeStyle = rgbaOf(rgb, on ? 0.95 : DIM_ALPHA);
            ctx.lineWidth = hi && s.ch === hi ? LINE_W_HI : LINE_W;
            // 立体声轨 = pan **中心线**(J75 A;width 张开度带 v1 不入本图)——
            // 视觉上不改笔形(改成虚线会与「断线」撞语义),身份由图例的 ST 角标承担。
            for (const run of s.runs || []) {
                if (!runIntersects(run, vp)) continue;
                const pts = decimateRun(run, toX);
                ctx.beginPath();
                for (let i = 0; i < pts.length; i++) {
                    const x = toX(pts[i].tS);
                    const y = PAD_TOP + panToY(pts[i].pan, H);
                    if (i === 0) ctx.moveTo(x, y);
                    else ctx.lineTo(x, y);
                }
                ctx.stroke();
            }
        }
    }

    function paintZoomLabel() {
        if (o.zoomEl) {
            const v = zoomLabel(timeline.viewport(), timeline.durationS());
            if (o.zoomEl.textContent !== v) o.zoomEl.textContent = v;
        }
    }

    function dpr() {
        return typeof devicePixelRatio === "number" ? devicePixelRatio : 1;
    }

    // -------------------------------------------------------------- 跟随模式
    function setFollowing(on) {
        const next = !!on;
        if (local.following === next) return;
        local.following = next;
        if (o.followBtn) o.followBtn.hidden = next;
        onFollowChange();
    }

    /** 手动缩放/拖拽 ⇒ 脱离跟随(J75 A);跟随中的**程序性**推进不走这里。 */
    function breakFollow() {
        setFollowing(false);
    }

    function advanceFollow(tS) {
        if (!local.following) return;
        const vp = timeline.viewport();
        const next = followViewport(vp, timeline.durationS(), tS);
        if (!sameViewport(vp, next)) timeline.set(next);
    }

    // ---------------------------------------------------------------- 事件面
    function stageX(clientX) {
        if (!canvas || typeof canvas.getBoundingClientRect !== "function") {
            return 0;
        }
        const r = canvas.getBoundingClientRect();
        // 命中测试走 CSS 坐标(05 §6.1):CSS zoom 下 rect 已是缩放后的尺寸,
        // 按「rect 宽 ↔ 舞台宽」的比例折回,不去读 zoom 档位。
        const scale = r.width > 0 ? local.stageW / r.width : 1;
        return (clientX - r.left) * scale;
    }

    function wire() {
        if (!canvas) return;
        // 缩放 = 时间轴滚轮(J75 A 逐字)。Tab3 的手势是 **Ctrl+滚轮**(05 行 319),
        // 两者在此并存:落在画布上的裸滚轮与 Ctrl+滚轮都缩放,且都 preventDefault ——
        // 本图嵌在 Tab1 网格里,裸滚轮若不拦会连带滚动祖先容器/触发页面缩放。
        canvas.addEventListener(
            "wheel",
            (e) => {
                e.preventDefault();
                if (!(local.stageW > 0)) return;
                const vp = timeline.viewport();
                const anchorT = xToTime(
                    vp,
                    local.stageW,
                    clamp(0, local.stageW, stageX(e.clientX)),
                );
                breakFollow();
                timeline.zoom(
                    anchorT,
                    e.deltaY < 0 ? ZOOM_STEP : 1 / ZOOM_STEP,
                );
            },
            { passive: false },
        );

        // 平移 = 拖拽(J75 A)。指针捕获照 Tab3 同款:拿不到捕获时用 window 上的
        // pointerup 兜底,否则拖出画布松手会把拖拽态永久卡住。
        canvas.addEventListener("pointerdown", (e) => {
            if (e.button !== 0 || !(local.stageW > 0)) return;
            local.drag = {
                id: e.pointerId,
                x0: stageX(e.clientX),
                startS0: timeline.viewport().startS,
            };
            breakFollow();
            if (typeof canvas.setPointerCapture === "function") {
                try {
                    canvas.setPointerCapture(e.pointerId);
                } catch {
                    /* 捕获拿不到不影响拖拽本身(下面有 window 兜底) */
                }
            }
            canvas.style.cursor = "grabbing";
        });
        canvas.addEventListener("pointermove", (e) => {
            const d = local.drag;
            if (!d || d.id !== e.pointerId) return;
            const vp = timeline.viewport();
            const per = pxPerSec(vp, local.stageW);
            if (!(per > 0)) return;
            const dS = (d.x0 - stageX(e.clientX)) / per;
            timeline.set(
                clampViewport(
                    {
                        startS: d.startS0 + dS,
                        endS: d.startS0 + dS + spanOf(vp),
                    },
                    timeline.durationS(),
                ),
            );
        });
        const endDrag = () => {
            local.drag = null;
            canvas.style.cursor = "";
        };
        canvas.addEventListener("pointerup", endDrag);
        canvas.addEventListener("pointercancel", endDrag);
        if (typeof window !== "undefined") {
            window.addEventListener("pointerup", endDrag);
        }

        // 键盘等价物(a11y:画布 role=application + tabindex);方向键平移、± 缩放。
        canvas.addEventListener("keydown", (e) => {
            const vp = timeline.viewport();
            const step = spanOf(vp) * 0.15;
            let handled = true;
            if (e.key === "ArrowLeft") {
                breakFollow();
                timeline.pan(-step);
            } else if (e.key === "ArrowRight") {
                breakFollow();
                timeline.pan(step);
            } else if (e.key === "+" || e.key === "=") {
                breakFollow();
                timeline.zoom((vp.startS + vp.endS) / 2, ZOOM_STEP);
            } else if (e.key === "-" || e.key === "_") {
                breakFollow();
                timeline.zoom((vp.startS + vp.endS) / 2, 1 / ZOOM_STEP);
            } else handled = false;
            if (handled) e.preventDefault();
        });

        if (o.followBtn) {
            o.followBtn.addEventListener("click", () => {
                setFollowing(true);
                advanceFollow(playhead.valueAt(nowMs()));
                // 回到跟随的那一下要**立刻**把播放头摆回带内,而不是等下一个
                // §2.6 事件 —— 停播时那个事件可能永不到来(契约 §0.4「值未变不发」)。
                playhead.refresh();
            });
            o.followBtn.hidden = true;
        }

        observeResolution(() => layers.invalidateStatic());
        if (typeof ResizeObserver === "function" && canvas.parentElement) {
            new ResizeObserver(() => {
                if (measure()) {
                    layers.invalidateStatic();
                    playhead.refresh();
                }
            }).observe(canvas.parentElement);
        }
    }

    function nowMs() {
        return typeof performance !== "undefined" ? performance.now() : 0;
    }

    /** 前台时按自留的最近一帧把插值循环重新起上(离场时 onPlayhead 停过它)。 */
    function resume() {
        if (!isVisible() || !local.playheadEv) return;
        playhead.push(local.playheadEv);
    }

    // ------------------------------------------------------------------ 出口
    wire();
    measure();
    paintZoomLabel();

    return {
        /**
         * 数据/可见性变化后请求一次静态层重绘(幂等;不可见时不起 rAF)。
         * 兼「回到前台」的补帧口:插值循环在离场时被 `onPlayhead` 停掉了,
         * 这里按自留的最近一帧把它重新起上(与 Tab3 的 resumePlayhead 同一配对不变式)。
         */
        invalidate() {
            if (!isVisible()) return;
            local.palette = resolveTrackPalette(canvas);
            local.styles = readStyles(canvas);
            measure();
            layers.invalidateStatic();
            resume();
            playhead.refresh();
            paintZoomLabel();
        },
        /**
         * 回到前台的**按需起帧**口(与 `onPlayhead` 的停帧配对)。
         * 幂等且便宜:`playhead.push` 对「与上一帧逐字相同的停住事件」不起 rAF,
         * 故调用方每帧无脑调也不会烧循环。
         */
        resume,
        /** 工程时长变化(段表/播放头把时间线推长了)。 */
        setDuration(d) {
            if (Math.abs(timeline.durationS() - num(d, 0)) < 1e-6) return;
            timeline.setDuration(d);
            layers.invalidateStatic();
            paintZoomLabel();
        },
        /**
         * §2.6 播放头事件入口(30Hz);跟随开启时顺带推进视口。
         *
         * **不在前台就停帧并早退**(05 §6.1 空闲零 rAF):否则停在分布档、或整个
         * Tab1 都不是当前页时,30Hz 事件流仍会每显示帧给一个 display:none 的竖线
         * 写 left —— 这正是 Tab3 收尾时抓到的那一类空转(smoke-tab3 ⑨ 同款不变式)。
         */
        onPlayhead(ev) {
            local.playheadEv = ev || null;
            if (!isVisible()) {
                playhead.stop();
                return;
            }
            playhead.push(ev);
            if (ev && ev.isPlaying) advanceFollow(num(ev.timeS, 0));
        },
        /** 图例 hover 联动(0 = 取消高亮)。纯展示,无写操作(J75 B)。 */
        setHighlight(ch) {
            const v = Number(ch) || 0;
            if (local.highlight === v) return;
            local.highlight = v;
            layers.invalidateStatic();
        },
        following: () => local.following,
        viewport: () => timeline.viewport(),
        /** 视图被切走时收手:两条自持循环一起停、藏竖线(空闲零 rAF,05 §6.1)。 */
        suspend() {
            layers.stop();
            playhead.stop();
            if (o.playheadEl && !o.playheadEl.hidden)
                o.playheadEl.hidden = true;
        },
        /** 测试面:允许 node 侧直接驱动视口(不经指针事件)。 */
        timeline,
    };
}
