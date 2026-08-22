// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB · 命中测试工具(可复用件;T33 Wave 1 交付)。
// -----------------------------------------------------------------------------
// 口径(两条,出处不同,不得混用 —— 图谱 §12 ④ 注):
//   • 一般控件:命中半径 **≥12 设计 px**(05 §6.1,671);
//   • 段边界竖线:**±6 设计 px**(05 §6.3,696 —— 泳道里边界更密,半径大了会串);
//   • 小靶件(选区 chip ✕、9×26 手柄等):最小缩放 0.5 档下命中区仍须
//     **≥24×24 物理 px**(RE-06)—— 用透明扩展区补足,视觉尺寸不变。
// 命中测试一律用 **CSS 坐标**(getBoundingClientRect + zoom 校正,05 §6.1);
// 本模块只做几何,不碰 DOM。复用方:T33 泳道/选区、T34 曲线手柄、T43。
// =============================================================================

/** 一般控件命中半径(设计 px;05 §6.1)。 */
export const HIT_RADIUS_PX = 12;

/** 段边界竖线命中半径(设计 px;05 §6.3)。 */
export const BOUNDARY_HIT_PX = 6;

/** 命中区物理下限(px;RE-06)。 */
export const MIN_TARGET_PHYS_PX = 24;

/** 需要按其校验的最小缩放档(05 §1.2 档位表下限)。 */
export const MIN_UI_SCALE = 0.5;

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

/** 一维命中:|x − target| ≤ r。 */
export function withinRadius(x, target, r = HIT_RADIUS_PX) {
    return Math.abs(num(x, NaN) - num(target, NaN)) <= r;
}

/** 边界竖线命中(±6 设计 px 专用口径)。 */
export function hitsBoundary(x, boundaryX) {
    return withinRadius(x, boundaryX, BOUNDARY_HIT_PX);
}

/**
 * 升序竖线表里找命中的下标(边界拖拽/吸附共用)。
 * 命中多条时取**最近**的一条;无命中返回 -1。
 * @param {number} x 指针 x(CSS px)
 * @param {number[]} xs 升序竖线位置表
 * @param {number} [r] 半径(缺省 = 边界口径 ±6)
 */
export function nearestHit(x, xs, r = BOUNDARY_HIT_PX) {
    let best = -1;
    let bestD = Infinity;
    for (let i = 0; i < (xs ? xs.length : 0); i++) {
        const d = Math.abs(num(xs[i], NaN) - num(x, NaN));
        if (d <= r && d < bestD) {
            best = i;
            bestD = d;
        }
    }
    return best;
}

/**
 * 透明扩展区(每侧,设计 px):让 `visualPx` 的靶件在 `uiScale` 档下
 * 物理命中区达到 ≥24px。已够大返回 0。
 *   例:15px 的 ✕ 在 0.5 档 → 物理 7.5px → 每侧补 (24/0.5 − 15)/2 = 16.5 设计 px。
 * @param {number} visualPx 视觉尺寸(设计 px)
 * @param {number} [uiScale] 缺省按最小档 0.5 校验(RE-06 口径)
 */
export function hitExpansionPx(visualPx, uiScale = MIN_UI_SCALE) {
    const s = num(uiScale, MIN_UI_SCALE);
    const need = MIN_TARGET_PHYS_PX / (s > 0 ? s : MIN_UI_SCALE);
    const v = num(visualPx, 0);
    return v >= need ? 0 : (need - v) / 2;
}

/**
 * 事件坐标 → 元素内 CSS 坐标(zoom 校正;05 §6.1「命中测试用 CSS 坐标」)。
 * `rect` = getBoundingClientRect() 结果(物理经浏览器换算后的 CSS 视口坐标);
 * `zoom` = 设计盒缩放(#card 的 zoom / transform 系数),rect 已被它放大,除回去
 * 得到 1× 设计坐标。纯几何 —— DOM 读取在调用方。
 */
export function eventToLocal(clientX, clientY, rect, zoom = 1) {
    const z = num(zoom, 1) || 1;
    return {
        x: (num(clientX, 0) - num(rect && rect.left, 0)) / z,
        y: (num(clientY, 0) - num(rect && rect.top, 0)) / z,
    };
}
