// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB · 15 色轨道调色板的读取面(T43 / [J75] B 节;可复用件)
// -----------------------------------------------------------------------------
// 色值真源 = `web/shared/tokens.css` 第 21 组 `--track-color-1..15`(构造法、
// 可辨性实测数与「顺序即轨号,永不重排」的纪律都写在那里)。本文件只做两件事:
//   ① 给 DOM 侧一个 `rgba(var(--track-color-N), a)` 的拼串口(色值仍在 CSS 里,
//      切主题/改色只改 tokens.css 一处);
//   ② 给 **canvas 侧**一条读取通道 —— 2D 上下文的 fillStyle 不认 `var()`,
//      必须拿到实数分量。`resolveTrackPalette(el)` 从计算样式里把 15 组分量读回来。
//
// `FALLBACK_TRACK_COLORS` 是 tokens.css 的**镜像副本**,只在两种场合生效:
// node 侧单测(无 CSSOM)与计算样式取不到值的降级路径。**两处必须同改** ——
// 与 mock-data.js 的 `DEMO_LABELS` ↔ i18n `demo.ch*` 是同一类纪律。
// 镜像漂了不会静默:`web-preview/tests/smoke-t43-chart.mjs` 逐条比对 tokens.css
// 的声明文本与本表,不一致即 smoke 红。
// =============================================================================

/** 轨道数(J59;与 mock-data.js 的 CHANNEL_COUNT 同值,此处不 import 以免依赖倒挂)。 */
export const TRACK_COLOR_COUNT = 15;

/** tokens.css 第 21 组的镜像(`"r, g, b"` 分量串,下标 0 = 轨 1)。 */
export const FALLBACK_TRACK_COLORS = Object.freeze([
    "111, 63, 115",
    "44, 130, 94",
    "140, 33, 88",
    "144, 107, 33",
    "19, 97, 141",
    "170, 95, 83",
    "72, 69, 143",
    "191, 78, 119",
    "98, 100, 16",
    "148, 100, 157",
    "21, 96, 53",
    "105, 94, 173",
    "120, 63, 8",
    "45, 121, 198",
    "143, 69, 73",
]);

/** 轨号 → CSS 变量名(越界按 15 取模落回 1..15,永不返回空串)。 */
export function trackColorVar(ch) {
    return "--track-color-" + trackIndex(ch);
}

/**
 * 轨号 → DOM 侧色串 `rgba(var(--track-color-N), a)`。
 * alpha 省略即 `var(--track-color-N)` 的实心形式 `rgb(var(...))`。
 */
export function trackColorCss(ch, alpha) {
    const v = `var(${trackColorVar(ch)})`;
    return Number.isFinite(alpha) ? `rgba(${v}, ${alpha})` : `rgb(${v})`;
}

/**
 * 轨号 → 1..15 的下标。非法/非正一律回 1;超出 15 的按 15 取模绕回。
 * 契约 §0.2 里 ch 恒为 1..15,越界只可能来自脏数据 —— 这里保证**永远返回一个
 * 合法下标**,让消费侧不必到处判空(拿不到色比拿错色更难查)。
 */
export function trackIndex(ch) {
    const n = Math.round(Number(ch));
    if (!Number.isFinite(n) || n < 1) return 1;
    return n > TRACK_COLOR_COUNT ? ((n - 1) % TRACK_COLOR_COUNT) + 1 : n;
}

/** 分量串 + alpha → canvas 认得的字面色串(`rgba(r, g, b, a)`)。 */
export function rgbaOf(triplet, alpha) {
    return Number.isFinite(alpha)
        ? `rgba(${triplet}, ${alpha})`
        : `rgb(${triplet})`;
}

/**
 * 从计算样式把 15 组分量读回来(canvas 用)。
 *
 * 为什么每次重绘都可以调:`getComputedStyle` 有布局同步成本,但本函数在每次**静态层
 * 重绘**时至多调一次(15 次属性读),而静态层只在数据/视口变化时重绘(05 §6.1 分层),
 * 不在 rAF 逐帧路径上。调用方仍可自行缓存 —— 色板只在切主题时会变。
 *
 * @param {Element|null} el 取样节点(:root 上的变量对任意后代都可见,传 canvas 即可)
 * @returns {string[]} 15 个 `"r, g, b"` 分量串;任一项取不到即整体回退镜像表
 */
export function resolveTrackPalette(el) {
    if (
        !el ||
        typeof globalThis.getComputedStyle !== "function" ||
        typeof el.ownerDocument === "undefined"
    ) {
        return FALLBACK_TRACK_COLORS.slice();
    }
    let cs;
    try {
        cs = globalThis.getComputedStyle(el);
    } catch {
        return FALLBACK_TRACK_COLORS.slice();
    }
    if (!cs || typeof cs.getPropertyValue !== "function") {
        return FALLBACK_TRACK_COLORS.slice();
    }
    const out = [];
    for (let ch = 1; ch <= TRACK_COLOR_COUNT; ch++) {
        const v = String(cs.getPropertyValue(trackColorVar(ch)) || "").trim();
        // 分量形式必须是三段数字;取不到(变量未加载/被覆盖成别的形态)即整体回退,
        // 不做逐项混搭 —— 半张镜像半张计算值只会让「色板对不上」更难查。
        if (!/^\d{1,3}\s*,\s*\d{1,3}\s*,\s*\d{1,3}$/.test(v)) {
            return FALLBACK_TRACK_COLORS.slice();
        }
        out.push(v);
    }
    return out;
}
