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
//   • 播放头竖线 + 跟随模式(默认开;手动动**横向**视口即脱离 —— 滚轮平移 /
//     Ctrl+滚轮缩放 / 拖拽 / ←→ / ±;「回到播放头」恢复。纵向那四路不脱离)。
//
// **滚轮四路映射**(2026-08-25 用户 preview 后定稿):
//   滚轮 = 横向平移 · Ctrl+滚轮 = 横向缩放 · Shift+滚轮 = 纵向平移 · Alt+滚轮 = 纵向缩放。
//   最常用的动作(左右滑动看时间线)放在不按键的那一档。**本图的滚轮语义自此与
//   Tab3 泳道分叉**(那边裸滚轮仍是缩放,05 行 319)—— 是否统一留用户裁量,本卡不动 Tab3。
//
// **纵向缩放**(2026-08-25 用户 preview 反馈追加):y 轴不再恒等于全角度域 ——
// 视野 `{lo, hi} ⊆ −100..100` 也是一个视口,且与 x 轴**共用同一套视口几何**:
// 把 pan 折成轴坐标 `a = +100 − pan`(正好是 y 向下的方向),全域就是一条长 200 的
// 「时间线」,于是 `clampViewport / zoomAt / panBy` 一字不改地直接复用(与 x 轴同源,
// 不另写一份夹取与锚点换算)。放大上限 8×(`PAN_SPAN_MIN = 25`),纵向缩放**不动**
// 横向跟随 —— 两条轴的状态互不牵连,跟随是时间轴的事。
//
// **复用契约(T46 Monitor)**:本模块不认 store、不认契约事件、不认 i18n ——
// 调用方经 `getSeries()` 喂「已经算好的 15 条折线」,经 `getPalette()` 喂色板,
// 文案一律由调用方写进 DOM。Monitor 直接复用本文件,零重复实现(07 T46 卡)。
// 多实例/可销毁也在这份契约里:`destroy()` 退掉活得比 canvas 长的三处订阅
// (window pointerup 兜底 / 媒体查询 / ResizeObserver)。Tab1 单实例用不到它,
// Monitor 在窗口开合里反复建销毁**必须**调 —— 不调就是每开一次漏一组订阅。
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
    panBy,
    pxPerSec,
    spanOf,
    timeToX,
    xToTime,
    zoomAt,
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
import { resolveTrackPalette, rgbaOf, trackIndex } from "./track-colors.js";
import { wheelPx } from "./wheel.js";

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

/** 全角度域的跨度(纵向视口的「全长」)。 */
export const PAN_SPAN_FULL = PAN_MAX - PAN_MIN;

/** 纵向放大上限;再往里就只剩一条线的粗细,读不出「细微 pan 变化」以外的东西。 */
export const PAN_ZOOM_MAX = 8;

/** 纵向视野的最小跨度(= 全域 / 上限倍数)。 */
export const PAN_SPAN_MIN = PAN_SPAN_FULL / PAN_ZOOM_MAX;

/** 默认纵向视野 = 全域(J75 A 原样;缩放是它之上的可选态)。 */
export const PAN_VIEW_FULL = Object.freeze({ lo: PAN_MIN, hi: PAN_MAX });

/** 单步纵向缩放倍率 —— 与时间轴同一手感(两轴共用 ZOOM_STEP)。 */
export const PAN_ZOOM_STEP = ZOOM_STEP;

// 滚轮归一化(deltaMode 分档 + 主轴取大)与 Tab3 泳道**同一份**,见 shared/wheel.js。
// 两处原先各写了一份逐字同义的实现,抽走免得下次只修一边(SL-205 复审建议③)。
export { WHEEL_LINE_PX } from "./wheel.js";

/** y 轴刻度的候选步长(粗 → 细);缩放档越深取越细的那一档。 */
export const PAN_TICK_STEPS = Object.freeze([50, 25, 20, 10, 5, 2, 1]);

/** 选步长的判据:视野里至少这么多格,否则换更细的一档。 */
export const PAN_TICK_MIN_COUNT = 4;

/**
 * pan ↔ 轴坐标。`a = +100 − pan` 把角度域折成 `[0, 200]` 的一条「时间线」,
 * 方向与 y 向下一致,于是 timeline.js 的视口几何可以原样复用(见文件头注)。
 */
function viewToAxis(view) {
    const v = view || PAN_VIEW_FULL;
    return {
        startS: PAN_MAX - num(v.hi, PAN_MAX),
        endS: PAN_MAX - num(v.lo, PAN_MIN),
    };
}

function axisToView(a) {
    return { lo: PAN_MAX - a.endS, hi: PAN_MAX - a.startS };
}

/** 夹取纵向视野:跨度 ∈ [PAN_SPAN_MIN, 全域],且不越出 −100..100。 */
export function clampPanView(view) {
    return axisToView(
        clampViewport(viewToAxis(view), PAN_SPAN_FULL, PAN_SPAN_MIN),
    );
}

/** 以某个 pan 值为锚点纵向缩放(`factor > 1` = 放大);锚点缩放前后落在同一 y 上。 */
export function zoomPanView(view, anchorPan, factor) {
    const a = viewToAxis(clampPanView(view));
    return axisToView(
        zoomAt(
            a,
            PAN_SPAN_FULL,
            PAN_MAX - num(anchorPan, 0),
            num(factor, 1),
            PAN_SPAN_MIN,
        ),
    );
}

/** 纵向平移(`dPan > 0` = 视野向 +100 移);跨度不变,贴到 ±100 即停。 */
export function panPanView(view, dPan) {
    const a = viewToAxis(clampPanView(view));
    // 轴坐标与 pan 反向 ⇒ 「视野向 +100 移」在轴上是**减**。
    return axisToView(panBy(a, PAN_SPAN_FULL, -num(dPan, 0)));
}

/** 纵向视野是不是默认的全域(reset 按钮与读数后缀都看它)。 */
export function isPanViewFull(view) {
    const v = clampPanView(view);
    return v.hi - v.lo >= PAN_SPAN_FULL - 1e-6;
}

/** 纵向缩放倍数(全域 = 1)。 */
export function panZoomFactor(view) {
    const v = clampPanView(view);
    return PAN_SPAN_FULL / Math.max(v.hi - v.lo, PAN_SPAN_MIN);
}

/**
 * pan → 舞台 y(CSS px)。**+100(右)在顶、−100(左)在底** ——
 * 与分布图「横位 = 声像、左在左」是两套轴,故 y 轴刻度必须带 L/R 字样(词条侧承担)。
 *
 * `view` 缺省 = 全域,故 T43 首版的两参调用逐字不变。**不按 view 夹取** ——
 * 纵向放大后落在视野外的点要算得出界外的 y,由调用方裁剪(画布侧走 clip),
 * 夹到边界会把出界的线压成一条贴边的假水平线。
 */
export function panToY(pan, plotH, view) {
    const v = view || PAN_VIEW_FULL;
    const lo = num(v.lo, PAN_MIN);
    const hi = num(v.hi, PAN_MAX);
    const span = hi - lo;
    const p = clamp(PAN_MIN, PAN_MAX, num(pan, 0));
    return span > 0 ? ((hi - p) / span) * num(plotH, 0) : 0;
}

/** 舞台 y(折线区内,0 = 顶)→ pan;夹到视野内(指针几何,越界即贴边)。 */
export function yToPan(y, plotH, view) {
    const v = view || PAN_VIEW_FULL;
    const lo = num(v.lo, PAN_MIN);
    const hi = num(v.hi, PAN_MAX);
    const h = num(plotH, 0);
    if (!(h > 0)) return (lo + hi) / 2;
    return clamp(lo, hi, hi - (num(y, 0) / h) * (hi - lo));
}

/**
 * 视野内的 y 轴刻度(从上往下,即 pan 降序)。
 *
 * 全域时逐字回到 `PAN_TICKS`(±100/±50/0);放大后按 `PAN_TICK_STEPS` 自适应细分,
 * 取「视野里够 PAN_TICK_MIN_COUNT 格」的**最粗**一档 —— 细分只在放大后发生,
 * 全域那一组刻度与曲线编辑器同源的口径不会被这条规则改掉。
 */
export function panTicksIn(view) {
    const v = clampPanView(view);
    const span = v.hi - v.lo;
    let step = PAN_TICK_STEPS[PAN_TICK_STEPS.length - 1];
    for (const s of PAN_TICK_STEPS) {
        if (span / s >= PAN_TICK_MIN_COUNT) {
            step = s;
            break;
        }
    }
    const out = [];
    for (
        let p = Math.floor(v.hi / step) * step;
        p >= v.lo - 1e-9 && out.length < 64;
        p -= step
    ) {
        out.push(Math.round(p * 1000) / 1000);
    }
    return out;
}

/**
 * 刻度数字(**不含** L/R/C 方位词 —— 那是文案,由调用方按 i18n 补,
 * 见本文件的「复用契约」)。负号用 U+2212,与词条里的 `−50` 同一个字符。
 */
export function panTickText(pan) {
    const p = Math.round(num(pan, 0) * 100) / 100;
    if (p === 0) return "0";
    return (p > 0 ? "+" : "−") + Math.abs(p);
}

/**
 * 缩放读数的纵向后缀。全域时为空串 —— 没缩放就不该在读数里多出一截,
 * 而一旦缩放,`aria-live` 的读数节点顺带把纵向档位播报出去(无需另设 live 区)。
 */
export function panZoomLabel(view) {
    const f = panZoomFactor(view);
    if (f <= 1 + 1e-6) return "";
    return ` · Y ×${f >= 10 ? Math.round(f) : f.toFixed(1)}`;
}

/**
 * 段表 → 折线段组(**断线的唯一判据在这里**)。
 *
 * 每段是一条**水平线**(段内 pan 恒定 = 该段最终打印值);相邻两段首尾相接时用
 * 竖直连接线连起来(引擎按 ramp 过渡,80ms 在任何缩放档下都不足 1px,故画成竖直
 * 台阶而不是斜线 —— 画斜线会在放到最大时变成一条**假的**渐变轨迹);中间只要有
 * 间隙就**另起一段**,即 J75 A 的「无分段覆盖的区间不画线」。
 *
 * 输入按 `t0S` 升序整理,并对**重叠段**夹取(§2.8 的段互不重叠,但脏数据里时间会
 * 倒流):重叠部分归先到的那一段,被完全吞掉的段整段跳过 —— 折线因此**恒为时间
 * 单调递增**,不会出现往回走的负宽台阶。
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
        // **重叠段夹到前一段的末尾**(段表已按 t0S 升序)。§2.8 的段互不重叠,
        // 但脏数据里时间会倒流:t0S < prevEnd 时若原样入点,折线就会往回走一段 ——
        // 画出负宽的台阶,且后面的 decimateRun 按「同一像素列」判据也跟着乱。
        // 语义取「先到的段占住那段时间,交接发生在它的末尾」。
        const t0 = Math.max(s.t0S, prevEnd);
        // 完全被前一段吞掉(t1S ≤ prevEnd):没有任何自己的时间,整段跳过。
        // 注意**不能**因此断线 —— 它压根没占住时间,不该在图上留下一个缺口。
        if (s.t1S <= t0) continue;
        if (!cur || t0 - prevEnd > CONTIGUOUS_EPS_S) {
            cur = [];
            runs.push(cur);
        }
        cur.push({ tS: t0, pan }, { tS: s.t1S, pan });
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
    mono: '"SCVB Mono", "Noto Sans SC", ui-monospace, monospace',
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
 * @param {HTMLElement} [opts.resetPanBtn] 「纵向复位」按钮(纵向非全域时才显示)
 * @param {HTMLElement} [opts.zoomEl] 缩放档位读数节点(`×4.0 / 75s`,aria-live 由 HTML 侧给)
 * @param {() => {ch:number, stereo:boolean, runs:{tS:number,pan:number}[][]}[]} opts.getSeries
 * @param {() => number} opts.getDurationS 工程时间线全长(秒)
 * @param {() => number} [opts.getUiScale] 05 §6.1 的 `k = uiScale × dpr`
 * @param {() => boolean} [opts.isVisible] 本图当前是否可见(不可见时不重绘、不起 rAF)
 * @param {() => void} [opts.onFollowChange] 跟随态变化(调用方据此刷按钮/词条)
 * @param {(ticks:{pan:number, y:number, side:string}[]) => void} [opts.onPanAxis]
 *   y 轴刻度变了(缩放/平移/改尺寸)。`y` = 相对画布顶的 CSS px,可直接定位刻度列;
 *   `side` = `"R" | "C" | "L" | ""`,**只在 +100 / 0 / −100 上给**(放大后看不见 +100
 *   就不该有 R —— 给别的刻度挂方位词等于骗人)。文案由调用方按 i18n 拼,本模块不认词条。
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
    const onPanAxis =
        typeof o.onPanAxis === "function" ? o.onPanAxis : () => {};

    const local = {
        following: true, // J75 A:跟随模式**默认开**
        panView: { lo: PAN_MIN, hi: PAN_MAX }, // 纵向视野;默认全域
        panAxisKey: "", // 上一次推给调用方的刻度签名(不变就不重写 DOM)
        // 最近一帧 §2.6 事件。不在前台时插值循环要**停掉**(空闲零 rAF,05 §6.1),
        // 而停掉就丢了位置 —— 故自留一份,`invalidate()` 回到前台时按它补一帧。
        playheadEv: null,
        // 「欠着一次静态层重绘」。离场期间的重绘诉求兑现不了,而它们**丢**在三处:
        //   ① `invalidate()` 不可见时早退 —— 连脏都没标上;
        //   ② `layers.tick` 先清 staticDirty 再调 drawStatic,而不可见时画不成 ——
        //      那一帧的脏位被吃掉了(与 Tab1 侧 trajDirty 同款的坑);
        //   ③ `suspend()` 停掉 rAF 循环 —— 还没兑现的 staticDirty 就此没人来跑。
        // 三处同因(不可见时画不成),故记在同一个欠账上,由 `resume()` 统一还:
        // 回到前台补一次重绘。**不**在不可见时回头调 invalidateStatic —— 那会让
        // tick 一直返回「还要下一帧」,rAF 在后台空转,违背 05 §6.1「空闲零 rAF」。
        staticPending: false,
        highlight: 0, // 图例 hover 联动的轨号(0 = 无)
        palette: resolveTrackPalette(canvas),
        styles: readStyles(canvas),
        drag: null, // {pointerId, x0, startS0}
        // 活得比 canvas 长的订阅的退订函数(window 事件 / 媒体查询 / ResizeObserver)。
        // 挂在 canvas 与两枚按钮上的那些不进这里 —— 节点一走它们跟着走。
        teardown: [],
        stageW: 0,
        stageH: 0,
        // 已拆除。**拆完之后一切写入口一律早退** —— 否则一次迟到的 `invalidate()`
        // (Monitor 在 pagehide 里拆图,而事件回调可能已经在队列上了)会重新
        // `layers.invalidateStatic()`,把 rAF 循环重新起来:实例复活、闭包连着 15 条
        // 折线的数据面一起钉在内存里,destroy() 想防的正是这个。
        // 由 T46(destroy 的首个消费者)的冒烟抓到并钉住:
        // `smoke-monitor.mjs` ⑧「destroy 之后 invalidate 也不再画」。
        destroyed: false,
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
        // `tick()` **先清 staticDirty 再调本回调** —— 不可见时直接不画的话,这一帧
        // 的重绘诉求就被那次清除吃掉了。记进欠账,回到前台由 resume() 还(见
        // `local.staticPending` 头注;此处不回头标脏,否则 rAF 在后台空转)。
        drawStatic: (frame) => {
            if (!isVisible()) {
                local.staticPending = true;
                return;
            }
            paintStatic(frame);
        },
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
        const pv = local.panView;

        // ---- 网格:pan 横线(0 线加重,与曲线编辑器的零轴同款)+ 时间竖线。
        // 全域时这组刻度就是 PAN_TICKS(±100/±50/0);放大后自适应细分。
        const panTicks = panTicksIn(pv);
        ctx.lineWidth = 1;
        for (const p of panTicks) {
            const y = Math.round(PAD_TOP + panToY(p, H, pv)) + 0.5;
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
        // 纵向放大后视野外的点算得出界外的 y(panToY 有意不夹取),这里裁到折线区 ——
        // 不裁的话出界的线会画到底部时间标签行和顶部留白上,看着像渲染坏了。
        ctx.save();
        ctx.beginPath();
        ctx.rect(0, PAD_TOP, local.stageW, H);
        ctx.clip();
        for (const s of ordered) {
            const on = !hi || s.ch === hi;
            // 取色**必须**走 trackIndex():它保证恒返回 1..15 的合法下标。
            // 自己算 `(ch-1) % len` 在 ch < 1 时会得到负下标(ch=0 → −1),
            // 取出 undefined → `rgba(undefined, .95)` → canvas 对非法 strokeStyle
            // **静默忽略**赋值,于是这一轨沿用上一轨的颜色 —— 不报错,只是画错。
            const rgb = local.palette[trackIndex(s.ch) - 1];
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
                    const y = PAD_TOP + panToY(pts[i].pan, H, pv);
                    if (i === 0) ctx.moveTo(x, y);
                    else ctx.lineTo(x, y);
                }
                ctx.stroke();
            }
        }
        ctx.restore();

        emitPanAxis(panTicks, H);
    }

    /**
     * y 轴刻度推给调用方(它拿 i18n 拼文案、写自己的刻度列)。
     * 签名不变就不推 —— 本函数在每次静态层重绘里跑,而重绘的常见触发是数据变化,
     * 那时刻度往往一格没动,推过去只会让调用方白重建一遍 DOM。
     */
    function emitPanAxis(panTicks, H) {
        const ticks = panTicks.map((p) => ({
            pan: p,
            y: PAD_TOP + panToY(p, H, local.panView),
            side:
                p === PAN_MAX ? "R" : p === PAN_MIN ? "L" : p === 0 ? "C" : "",
        }));
        const key = ticks.map((t) => `${t.pan}@${Math.round(t.y)}`).join("|");
        if (key === local.panAxisKey) return;
        local.panAxisKey = key;
        onPanAxis(ticks);
    }

    function paintZoomLabel() {
        if (o.zoomEl) {
            const v =
                zoomLabel(timeline.viewport(), timeline.durationS()) +
                panZoomLabel(local.panView);
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

    // ------------------------------------------------------------ 纵向视野
    /**
     * 写纵向视野(唯一入口)。**不碰跟随态** —— 纵向缩放是 y 轴的事,
     * 横向跟随该继续跟随(用户反馈里「加个纵向缩放」不含「顺手停掉自动滚动」)。
     */
    function setPanView(next) {
        const v = clampPanView(next);
        const cur = local.panView;
        if (v.lo === cur.lo && v.hi === cur.hi) return;
        local.panView = v;
        if (o.resetPanBtn) o.resetPanBtn.hidden = isPanViewFull(v);
        layers.invalidateStatic();
        paintZoomLabel();
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

    /** 同 `stageX`,但换算到**折线区**内的 y(0 = 折线区顶,已扣掉 PAD_TOP)。 */
    function stageY(clientY) {
        if (!canvas || typeof canvas.getBoundingClientRect !== "function") {
            return 0;
        }
        const r = canvas.getBoundingClientRect();
        const scale = r.height > 0 ? local.stageH / r.height : 1;
        return (clientY - r.top) * scale - PAD_TOP;
    }

    /**
     * 滚轮增量 → **CSS px**(轴与单位的归一化口径见 shared/wheel.js)。
     * 本图的「一页」= 自己的舞台宽(一屏),故把 stageW 传进去;泳道页没有天然的
     * 「一页」概念,用那边的常量档。
     */
    function wheelDelta(e) {
        return wheelPx(e, Math.max(local.stageW, 1));
    }

    function wire() {
        if (!canvas) return;
        // 滚轮四路映射(2026-08-25 用户 preview 后定稿):
        //
        //   滚轮        横向平移(左右滑动)        Ctrl+滚轮   横向缩放(中心跟光标 x)
        //   Shift+滚轮  纵向平移(上下滑动)        Alt+滚轮    纵向缩放(中心跟光标 y)
        //
        // 「裸滚轮 = 平移、Ctrl+滚轮 = 缩放」把最常用的动作放在不按键的那一档。
        // [SL-205 2026-08-27] 这里原先记着「本图语义自此与 Tab3 泳道分叉,是否统一留
        // 用户后续裁量」—— **已经统一了**:Tab3 那边补齐四路后与本图逐路同义
        // (裸=横向平移 / Shift=纵向平移 / Alt=纵向缩放 / Ctrl=横向缩放),
        // 归一化也共用了 shared/wheel.js。两处若要再改,请一起改。
        //
        // 四路**一律 preventDefault**:裸滚轮不拦会连带滚动祖先容器;Ctrl+滚轮不拦
        // 会触发浏览器页面缩放(WebView 里同理);Alt+滚轮在部分平台有默认的历史
        // 前进/后退语义。故监听器必须 `{ passive: false }`,否则 preventDefault 无效。
        canvas.addEventListener(
            "wheel",
            (e) => {
                e.preventDefault();
                if (!(local.stageW > 0)) return;
                const d = wheelDelta(e);
                if (d === 0) return;

                // ---- Ctrl+滚轮 = 横向缩放(以光标 x 为锚)
                if (e.ctrlKey) {
                    const vp = timeline.viewport();
                    const anchorT = xToTime(
                        vp,
                        local.stageW,
                        clamp(0, local.stageW, stageX(e.clientX)),
                    );
                    breakFollow();
                    timeline.zoom(anchorT, d < 0 ? ZOOM_STEP : 1 / ZOOM_STEP);
                    return;
                }

                // ---- Alt+滚轮 = 纵向缩放(以光标 y 为锚)
                // 不 breakFollow:纵向的事不动横向跟随(两条轴的状态互不牵连)。
                if (e.altKey) {
                    setPanView(
                        zoomPanView(
                            local.panView,
                            yToPan(stageY(e.clientY), plotH(), local.panView),
                            d < 0 ? PAN_ZOOM_STEP : 1 / PAN_ZOOM_STEP,
                        ),
                    );
                    return;
                }

                // ---- Shift+滚轮 = 纵向平移。按 px→pan 的实比例走(不是固定档位),
                // 触控板与鼠标滚轮因此手感一致。向下滚 ⇒ 视野向 −100 走。
                if (e.shiftKey) {
                    const H = plotH();
                    if (!(H > 0)) return;
                    const pv = local.panView;
                    setPanView(panPanView(pv, -(d / H) * (pv.hi - pv.lo)));
                    return;
                }

                // ---- 裸滚轮 = 横向平移。同样按 px→秒 的实比例走;触控板横向 deltaX
                // 已在 wheelDelta 里归到同一路,横向滑动与滚轮走的是同一条。
                const vp = timeline.viewport();
                const per = pxPerSec(vp, local.stageW);
                if (!(per > 0)) return;
                breakFollow();
                timeline.pan(d / per);
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
                y0: stageY(e.clientY),
                startS0: timeline.viewport().startS,
                panView0: local.panView,
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
            // 拖拽的**纵向分量**同样生效(不另设 Shift+拖拽:多一个手势就多一份要记的
            // 规矩)。全域档下 panPanView 的夹取让它自然是个空操作 —— 纵向没放大时
            // 斜着拖与横着拖手感一致,放大后才拖得动。
            const H = plotH();
            if (H > 0) {
                const v0 = d.panView0;
                setPanView(
                    panPanView(
                        v0,
                        ((stageY(e.clientY) - d.y0) / H) * (v0.hi - v0.lo),
                    ),
                );
            }
        });
        const endDrag = () => {
            local.drag = null;
            canvas.style.cursor = "";
        };
        canvas.addEventListener("pointerup", endDrag);
        canvas.addEventListener("pointercancel", endDrag);
        if (typeof window !== "undefined") {
            window.addEventListener("pointerup", endDrag);
            // 挂在 window 上 ⇒ 活得比 canvas 长,必须留退订口(见 destroy)
            local.teardown.push(() =>
                window.removeEventListener("pointerup", endDrag),
            );
        }

        // 键盘等价物(a11y:画布 role=application + tabindex)。
        //   ← →            横向平移        + −      横向缩放
        //   ↑ ↓            纵向平移        Shift+↑↓ 纵向缩放      0  纵向复位
        // 纵向那三档挑上下键,是因为「上下 = y 轴」不用记;而 `+`/`−` 上没有可用的
        // Shift 位:Shift+`=` 打出来的**就是** `+`,两者在 `e.key` 上分不开。
        canvas.addEventListener("keydown", (e) => {
            const vp = timeline.viewport();
            const step = spanOf(vp) * 0.15;
            const pv = local.panView;
            let handled = true;
            if (e.key === "ArrowLeft") {
                breakFollow();
                timeline.pan(-step);
            } else if (e.key === "ArrowRight") {
                breakFollow();
                timeline.pan(step);
            } else if (e.key === "ArrowUp" || e.key === "ArrowDown") {
                const up = e.key === "ArrowUp";
                if (e.shiftKey) {
                    setPanView(
                        zoomPanView(
                            pv,
                            (pv.lo + pv.hi) / 2,
                            up ? PAN_ZOOM_STEP : 1 / PAN_ZOOM_STEP,
                        ),
                    );
                } else {
                    const d = (pv.hi - pv.lo) * 0.15;
                    setPanView(panPanView(pv, up ? d : -d));
                }
            } else if (e.key === "0") {
                setPanView(PAN_VIEW_FULL);
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

        // 「纵向复位」:只回 y 轴全域,**不碰**横向视口与跟随态(两轴各管各的)。
        if (o.resetPanBtn) {
            o.resetPanBtn.addEventListener("click", () =>
                setPanView(PAN_VIEW_FULL),
            );
            o.resetPanBtn.hidden = true;
        }

        // 媒体查询监听:`observeResolution` 自带取消函数,收进 teardown。
        local.teardown.push(observeResolution(() => layers.invalidateStatic()));
        if (typeof ResizeObserver === "function" && canvas.parentElement) {
            const ro = new ResizeObserver(() => {
                if (measure()) {
                    layers.invalidateStatic();
                    playhead.refresh();
                }
            });
            ro.observe(canvas.parentElement);
            local.teardown.push(() => ro.disconnect());
        }
    }

    function nowMs() {
        return typeof performance !== "undefined" ? performance.now() : 0;
    }

    /**
     * 前台时把两条循环重新起上(离场时 `onPlayhead` / `suspend` 停过它们)。
     *
     * 除了按自留的最近一帧补播放头,还负责**还清离场期欠下的那次静态层重绘**
     * (三个丢法见 `local.staticPending` 的头注)。只在欠着时才画、不是每帧无脑
     * 重画 —— 脏位纪律(05 §6.1、smoke ⑦「静态层重绘走脏位」)照旧。
     */
    function resume() {
        if (local.destroyed || !isVisible()) return;
        if (local.staticPending) {
            local.staticPending = false;
            layers.invalidateStatic();
        }
        if (!local.playheadEv) return;
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
            // 拆完之后一律不干活(理由见 local.destroyed 的头注)。**连脏位都不留** ——
            // 留了也没人来还,而它连着的是一个已经不该存在的实例。
            if (local.destroyed) return;
            if (!isVisible()) {
                // 画不成,但**诉求要留着** —— 直接早退等于把这次重绘丢了,
                // 切回前台会看到一张过期的图(见 local.staticPending 头注)。
                local.staticPending = true;
                return;
            }
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
            if (local.destroyed) return;
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
            if (local.destroyed) return;
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
            if (local.destroyed) return;
            const v = Number(ch) || 0;
            if (local.highlight === v) return;
            local.highlight = v;
            layers.invalidateStatic();
        },
        following: () => local.following,
        viewport: () => timeline.viewport(),
        /** 当前纵向视野(`{lo, hi} ⊆ −100..100`)。 */
        panView: () => local.panView,
        /** 纵向复位到全域(与「纵向复位」按钮同一条路;横向不受影响)。 */
        resetPanView: () => setPanView(PAN_VIEW_FULL),
        /** 视图被切走时收手:两条自持循环一起停、藏竖线(空闲零 rAF,05 §6.1)。 */
        suspend() {
            layers.stop();
            playhead.stop();
            // 停循环会把「还没兑现的 staticDirty」一并搁置 —— 记进欠账,
            // 由 resume() 补画(否则切回来是一张过期的图)。
            local.staticPending = true;
            if (o.playheadEl && !o.playheadEl.hidden)
                o.playheadEl.hidden = true;
        },
        /**
         * 彻底拆除(**幂等**:重复调用一步不走)。
         *
         * `suspend()` 只是收手,实例还活着、还能 `resume()`;`destroy()` 是**不再用了**:
         * 停两条循环,并退掉那些**活得比 canvas 长**的订阅 —— window 的 pointerup 兜底、
         * `observeResolution` 的媒体查询、父盒的 ResizeObserver。挂在 canvas 与两枚按钮
         * 上的监听不在此列(节点一走它们跟着走)。
         *
         * 本卡(Tab1)是单实例、与页面同生命周期,调用面上**用不到**它;导出是为
         * **T46 Monitor**:那张卡要在窗口开合里反复建/销毁本图,没有拆除口就是每开一次
         * 漏一组订阅 —— window 上那条尤其致命,它会连着整个闭包(含 15 条折线的数据面)
         * 一起钉在内存里。复用契约既然写了「Monitor 直接复用本文件」,拆除口就得在。
         */
        destroy() {
            local.destroyed = true; // 先置位:此后一切写入口早退,不会有人再起 rAF
            layers.stop();
            playhead.stop();
            const fns = local.teardown.splice(0); // 先清空再执行 ⇒ 天然幂等
            for (const off of fns) {
                if (typeof off !== "function") continue;
                try {
                    off();
                } catch {
                    /* 退订失败不该拦住后面几条(拆到一半更难查) */
                }
            }
        },
        /** 测试面:允许 node 侧直接驱动视口(不经指针事件)。 */
        timeline,
    };
}
