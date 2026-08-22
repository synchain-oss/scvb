// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · 时间轴视口模型(可复用件;T33 Wave 1 交付)。
// -----------------------------------------------------------------------------
// 职责 = 05 §6.1(671)/ §2.3(319)的「视口 ↔ 像素」换算与缩放/平移控制:
//   • 视口 `{startS, endS}` 与舞台宽(CSS px)之间的双向换算、夹取;
//   • Ctrl+滚轮「以光标为中心缩放」与拖拽平移的**纯几何**(事件接线归调用方);
//   • 缩放档位读数(A-16:底部缩放条旁的 mono 小字,如「×4.0 / 12s」)。
//
// **复用契约(T34 / T43)**:本模块零 DOM、零 Tab3 依赖 ——
//   • T43 轨迹图:时间轴直接复用(同一 `{startS,endS}` 模型);
//   • T34 曲线编辑器:角度轴同构(把「秒」读作「角度」即可,函数不含时间语义);
//   • 158px 轨头左偏**不在这里**:舞台坐标系(容器宽 − 轨头宽)由调用方建立
//     (图谱 [J72a] C-04),本模块只认「舞台宽」。
// 频率纪律(brief §0.8)由调用方遵守:档位变化触发分块重拉取(05 §6.3),
// 本模块只提供几何,不发任何请求。
// =============================================================================

/** 视口最小跨度(秒)—— 缩放上限的夹取底(1s 以下的列数已超出特征流分辨率)。 */
export const MIN_SPAN_S = 1;

/** 单步滚轮的缩放倍率(以光标为中心;Ctrl+滚轮一格)。 */
export const ZOOM_STEP = 1.25;

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

/**
 * 建一个夹取过的视口。
 * @param {number} durationS 全长(秒)
 * @param {number} [startS] 缺省 0
 * @param {number} [endS] 缺省 durationS
 * @returns {{startS:number, endS:number}}
 */
export function makeViewport(durationS, startS, endS) {
    const d = Math.max(num(durationS, 0), MIN_SPAN_S);
    return clampViewport({ startS: num(startS, 0), endS: num(endS, d) }, d);
}

/** 视口跨度(秒)。 */
export function spanOf(vp) {
    return vp.endS - vp.startS;
}

/**
 * 夹取:跨度 ∈ [MIN_SPAN_S, durationS],起止不越 [0, durationS]。
 * 跨度越界时**保持中心**收缩/扩张,再整体平移回界内(缩放到头不跳画面)。
 */
export function clampViewport(vp, durationS, minSpanS = MIN_SPAN_S) {
    const d = Math.max(num(durationS, 0), minSpanS);
    let s = num(vp && vp.startS, 0);
    let e = num(vp && vp.endS, d);
    if (e < s) [s, e] = [e, s];
    const mid = (s + e) / 2;
    const span = Math.min(Math.max(e - s, minSpanS), d);
    s = mid - span / 2;
    e = mid + span / 2;
    if (s < 0) {
        e -= s;
        s = 0;
    }
    if (e > d) {
        s -= e - d;
        e = d;
    }
    return { startS: Math.max(s, 0), endS: Math.min(e, d) };
}

/** 每秒像素(舞台宽 stageW 为 CSS px;命中测试同口径,05 §6.1)。 */
export function pxPerSec(vp, stageW) {
    const span = spanOf(vp);
    return span > 0 ? stageW / span : 0;
}

/** 时间 → 舞台 x(不夹取 —— 视口外返回负值/超宽值,调用方按需裁剪)。 */
export function timeToX(vp, stageW, tS) {
    const span = spanOf(vp);
    return span > 0 ? ((num(tS, 0) - vp.startS) / span) * stageW : 0;
}

/** 舞台 x → 时间(夹到视口内 —— 指针几何,越界即贴边)。 */
export function xToTime(vp, stageW, x) {
    if (!(stageW > 0)) return vp.startS;
    const t = vp.startS + (num(x, 0) / stageW) * spanOf(vp);
    return Math.min(Math.max(t, vp.startS), vp.endS);
}

/**
 * 以锚点时间为中心缩放(05 §2.3:Ctrl+滚轮以光标为中心)。
 * `factor > 1` = 放大(跨度变小)。锚点在缩放前后落在同一舞台 x 上。
 */
export function zoomAt(vp, durationS, anchorT, factor, minSpanS = MIN_SPAN_S) {
    const f = num(factor, 1);
    if (!(f > 0)) return clampViewport(vp, durationS, minSpanS);
    const span = spanOf(vp) / f;
    const t = Math.min(Math.max(num(anchorT, vp.startS), vp.startS), vp.endS);
    const ratio = spanOf(vp) > 0 ? (t - vp.startS) / spanOf(vp) : 0;
    return clampViewport(
        { startS: t - span * ratio, endS: t + span * (1 - ratio) },
        durationS,
        minSpanS,
    );
}

/** 平移(dS 秒;正 = 向后看)。跨度不变,贴边即停。 */
export function panBy(vp, durationS, dS) {
    const d = Math.max(num(durationS, 0), MIN_SPAN_S);
    const span = spanOf(vp);
    let s = vp.startS + num(dS, 0);
    s = Math.min(Math.max(s, 0), d - span);
    return { startS: s, endS: s + span };
}

/**
 * 缩放档位读数(A-16 统筹裁定:底部缩放条旁 mono 小字)。
 * `×{全长/跨度} / {跨度}s`,如全长 300s、视口 75s → 「×4.0 / 75s」。
 */
export function zoomLabel(vp, durationS) {
    const span = Math.max(spanOf(vp), MIN_SPAN_S);
    const d = Math.max(num(durationS, 0), span);
    const factor = d / span;
    const f = factor >= 10 ? Math.round(factor) : factor.toFixed(1);
    const s = span >= 10 ? Math.round(span) : span.toFixed(1);
    return `×${f} / ${s}s`;
}

/**
 * 底部滚动条 thumb 几何(0..1 比例;`left` + `width`,CSS 侧转百分比)。
 */
export function scrollThumb(vp, durationS) {
    const d = Math.max(num(durationS, 0), MIN_SPAN_S);
    return {
        left: Math.min(Math.max(vp.startS / d, 0), 1),
        width: Math.min(Math.max(spanOf(vp) / d, 0), 1),
    };
}

/**
 * 视口控制器 —— 持一份视口态 + 变更通知。零 DOM(事件接线在 tab-wave.js /
 * T43 各自的模块里做);Wave 2 的缩放/平移交互只需调 `zoom()`/`pan()`。
 * @param {object} opts
 * @param {number} opts.durationS
 * @param {(vp:{startS:number,endS:number}) => void} [opts.onChange]
 */
export function createTimeline(opts) {
    const o = opts || {};
    let durationS = Math.max(num(o.durationS, 0), MIN_SPAN_S);
    let vp = makeViewport(durationS);
    const onChange = typeof o.onChange === "function" ? o.onChange : () => {};

    function set(next) {
        const c = clampViewport(next, durationS);
        if (c.startS === vp.startS && c.endS === vp.endS) return;
        vp = c;
        onChange(vp);
    }

    return {
        viewport: () => vp,
        durationS: () => durationS,
        /** 全长变化(如工程时长更新):跨度尽量保持,越界重夹。 */
        setDuration(d) {
            durationS = Math.max(num(d, 0), MIN_SPAN_S);
            set(vp);
        },
        set,
        zoom: (anchorT, factor) => set(zoomAt(vp, durationS, anchorT, factor)),
        pan: (dS) => set(panBy(vp, durationS, dS)),
    };
}
