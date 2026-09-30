// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// 滚轮归一化(SL-205 复审建议③:Tab3 泳道与总览轨迹图共用一份)
// =============================================================================
// 两处消费方原先各写了一份**逐字同义**的实现(tab-wave.js 的 wheelPx /
// trajectory-chart.js 的 wheelDelta),连注释里踩过的坑都一样。抽到这里,免得
// 下次只有一边被修 —— 这类「两份同义实现」正是本仓吃过亏的那一类。
//
// 归一化要做两件,少一件就有整路失灵:
//   ① **轴**:取 |deltaX| 与 |deltaY| 中较大的那个(带原符号)。鼠标滚轮给 deltaY;
//      触控板横向双指给 deltaX;而 **Shift+滚轮在 Chromium 系会被改写成横向滚动**
//      —— 值落到 deltaX、deltaY 归零,Firefox 则仍走 deltaY。取主轴对三种来源都成立,
//      不必逐浏览器分支。只读 deltaY 的话「Shift 那一路」永远拿到 0(SL-205 用户报的②)。
//   ② **单位**:deltaMode 可能是像素(0)/ 行(1)/ 页(2)。不折成 px 的话,同一次滚动
//      在不同浏览器与鼠标驱动上跨度差两个数量级 —— 平移会从「挪一点」变成「跳一屏」。
// =============================================================================

/** deltaMode=1(行)一行的像素当量(Chrome 桌面档约 16px/行)。 */
export const WHEEL_LINE_PX = 16;

/** deltaMode=2(页)一页的像素当量兜底值;调用方能给出真实视口宽/高时应传进来。 */
export const WHEEL_PAGE_PX = 400;

/**
 * 滚轮位移归一到**像素**(带符号,正 = 向下/向右)。
 *
 * @param {WheelEvent|object} e 滚轮事件(只读 deltaX/deltaY/deltaMode,便于 node 侧构造)
 * @param {number} [pagePx] deltaMode=2 时一页折算多少像素。轨迹图传自己的舞台宽
 *        (一页 = 一屏),泳道页没有天然的「一页」概念,用常量档 WHEEL_PAGE_PX。
 * @returns {number} 像素位移;缺参/非数一律回 0(调用方据此早退,不做零位移的空动作)
 */
export function wheelPx(e, pagePx) {
    const ev = e || {};
    const dx = Number.isFinite(ev.deltaX) ? ev.deltaX : 0;
    const dy = Number.isFinite(ev.deltaY) ? ev.deltaY : 0;
    const raw = Math.abs(dx) > Math.abs(dy) ? dx : dy;
    if (ev.deltaMode === 1) return raw * WHEEL_LINE_PX;
    if (ev.deltaMode === 2) {
        const page =
            Number.isFinite(pagePx) && pagePx > 0 ? pagePx : WHEEL_PAGE_PX;
        return raw * page;
    }
    return raw;
}
