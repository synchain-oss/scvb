// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB · 声像 / 音量分布图 + 轨道配色图例(可复用件;T46 提取 / [J75] B+C)
// -----------------------------------------------------------------------------
// 本文件是 T46 从 `web/output/tab-master.js` 里**原样提取**出来的两块:
//   ① `distGeometry()` —— 一根柱 / 一条张开线的几何(设计稿 L2037-2056);
//   ② `distBarsHtml()` / `legendItemsHtml()` —— 两处 innerHTML 的拼串。
// 提取的理由只有一个:**Monitor(web/monitor/)要画同一张图**,而它的数据面来自
// viz 共享段、不是 Output 的 `scvb.params`。把「几何 + 标记」抽出来,两页就只差
// 「怎么把数据凑成 rows」这一步 —— 07 T46 卡的「复用 T43 组件与 token,零重复实现」。
//
// **提取的纪律:Output 侧零行为变化。** 拼出来的 HTML 字符串与提取前**逐字节相同**
// (属性顺序、小数位数、空格位置全部照旧),故 Tab1 的既有冒烟与截图一字不改照过;
// `web-preview/tests/smoke-monitor.mjs` ① 节拿同一批入参对拍两处产物,漂了当场红。
//
// **不在本文件里的东西**(和 trajectory-chart.js 同一条复用契约):
//   • 不认 store、不认契约事件、不认 i18n —— 调用方把「已经算好的 rows」喂进来,
//     文案(ST 角标、图例 title)由调用方按字典给;
//   • 不碰 DOM —— 只返回字符串,写不写 innerHTML、比不比对旧值由调用方定。
//     (Output 侧 `if (el.innerHTML !== html)` 的省写判断留在原处,不进本件。)
//   • **不含 CSS** —— `.dist-bar` / `.dist-span` / `.chart-legend__*` 的样式各页
//     自持(两页的版面本就不同:Tab1 是四列网格里的一张卡,Monitor 是上下两块)。
//     真正不能漂的是**轨色接法** `rgba(var(--tc), a)` 与 `--tc: var(--track-color-N)`
//     这条链 —— 它由本文件拼出,两页共用,漂不了。
// =============================================================================

import { trackColorVar } from "./track-colors.js";

// =============================================================================
// 一、纯函数(无 DOM;node 侧断言面)
// =============================================================================

/** 音量行程的两端(dB;契约 / params-v0 的 `vol` 值域)。 */
export const VOL_MIN_DB = -24;
export const VOL_MAX_DB = 12;

/** [SL-280] 柱高行程的两端(块高百分比)——与上面两个值域常量同类,故并排放。 */
export const BAR_H_MIN_PCT = 8;
export const BAR_H_MAX_PCT = 88;

function clamp(lo, hi, v) {
    return v < lo ? lo : v > hi ? hi : v;
}

function round2(v) {
    return Math.round(v * 100) / 100;
}

/** 两位零填充(契约 §1.12 的 `t{t:02d}` 与轨号显示同款)。 */
export function tt2(n) {
    return String(n).padStart(2, "0");
}

/** HTML 文本转义 —— label 是**用户数据**,绝不许不转义就拼进 innerHTML。 */
export function esc(s) {
    return String(s == null ? "" : s).replace(
        /[&<>"']/g,
        (c) =>
            ({
                "&": "&amp;",
                "<": "&lt;",
                ">": "&gt;",
                '"': "&quot;",
                "'": "&#39;",
            })[c],
    );
}

/**
 * volDb → 柱高(块高百分比)。**唯一真源**:柱、0 dB 基准线、用例三处都从这里取。
 *
 * [SL-280] 线性铺满 `BAR_H_MIN_PCT..BAR_H_MAX_PCT`,**不再除 0.70**。
 * 旧式是 `归一 / 0.70 × 100` 再夹到 8..88 —— 归一值 0.616 就撞上夹子,解出来是
 * **volDb ≥ −1.824 dB 一律画成 88%**。而每轨 vol 的出厂默认就是 0 dB
 * (`tab-tracks.js` 的 `VOL_RANGE.def`),真实工程里各轨也多坐在 unity 附近,于是
 * 用户看到的是「15 根柱齐刷刷一样高」(v5.6.5 实测,用户点名)。
 * 更糟的是饱和点落在推子行程 0.616,而 J03 定「0 dB 在行程 2/3」—— 曲线**恰好在
 * 最常用的那个值之前就顶到头**。
 *
 * 现在 −24..+12 dB 在 8%..88% 上严格单调:0 dB → 61.33%,+12 → 88,−24 → 8。
 * `clamp` 保留为纯防御(`volDb` 进来前已被 clamp,夹不到)。
 */
export function barHeightPct(volDb) {
    const norm =
        (clamp(VOL_MIN_DB, VOL_MAX_DB, volDb) - VOL_MIN_DB) /
        (VOL_MAX_DB - VOL_MIN_DB);
    return clamp(
        BAR_H_MIN_PCT,
        BAR_H_MAX_PCT,
        BAR_H_MIN_PCT + norm * (BAR_H_MAX_PCT - BAR_H_MIN_PCT),
    );
}

/**
 * [SL-280] 0 dB 基准横线的高度(块高百分比)= `barHeightPct(0)` = 61.33。
 *
 * **不写死这个数**:线要落在「0 dB 那根柱的顶边」上,而柱顶由 `barHeightPct` 决定 ——
 * 两处各写一份就是本仓刚清过的那类漂移。`dist-motion` 建器把它写成 CSS 变量喂给
 * `.dist-plot`,并同时打 `has-zero-line` —— 后者让「喂不到」真的等于「不画」
 * (只靠不给缺省做不到:变量未定义时 `bottom` 声明失效 ⇒ 线退到图顶,比不画更误导)。
 */
export function zeroDbLinePct() {
    return round2(barHeightPct(0));
}

/**
 * 分布图一根柱/一条张开线的几何(设计稿 L2037-2056)。
 * 横位 x =(有效 pan+100)/200;柱高见 `barHeightPct`(−24..+12 dB **线性铺满**
 * `BAR_H_MIN_PCT..BAR_H_MAX_PCT`;[SL-280] 之前是「归一后 /0.70」,那会在 −1.82 dB 以上压平);
 * 张开半宽 =(轨 width%/100)×16,并被 x 与 100−x 夹住不出框。
 *
 * **有效 pan = 名义 pan × 全局 width/100**(PanMath::scaleByGlobalWidth,DSP 逐样本同式)。
 * 段表与参数面存的是**名义** pan,真正听到的位置要经全局「最大角度」几何缩放 —— 图上不缩放
 * 就等于画了一张听不到的图,而且拧「最大角度」时分布图纹丝不动(用户裁定 v5 P2-10)。
 * 立体声张开线同理:两个子声像一起被缩放,半宽跟着乘同一个系数。
 *
 * @param {number} [globalWidthPct=100] 全局 width(0..150);缺省 100 = 不缩放,
 *        供数据面不含该值的调用方(Monitor 的 viz 段只带每轨 width)沿用原几何。
 */
export function distGeometry(pan, volDb, widthPct, globalWidthPct = 100) {
    const g = clamp(0, 150, Number(globalWidthPct) || 0) / 100;
    const x = ((clamp(-100, 100, clamp(-100, 100, pan) * g) + 100) / 200) * 100;
    const h = barHeightPct(volDb);
    const half = Math.min((clamp(0, 100, widthPct) / 100) * 16 * g, x, 100 - x);
    return { x: round2(x), h: round2(h), half: round2(half) };
}

// =============================================================================
// 二、CSS 变量(几何 → 写入面;拼串与逐帧补间**共用这一处格式化**)
// =============================================================================
//
// 为什么单列出来:`dist-motion.js` 的 rAF 补间不重拼 innerHTML(那会每帧销毁重建
// 15 个节点,也把 CSS 过渡打断),而是给缓存好的节点逐帧写这几个自定义属性。
// 若两条路各写一份 `toFixed(2) + "%"`,几何漂了**图照画**、只是补间落点与静态落点
// 差一点点 —— 那是肉眼看不出来的错。故格式化只此一处,两条路都从这里取。

/** 变量表 → `style` 里的声明串(不带末尾分号,与拼串模板逐字对齐)。 */
function varsToStyle(vars) {
    return Object.keys(vars)
        .map((k) => `${k}:${vars[k]}`)
        .join(";");
}

/** 一根柱的 CSS 变量(横位 + 柱高)。 */
export function distBarVars(geo) {
    return { "--x": geo.x.toFixed(2) + "%", "--h": geo.h.toFixed(2) + "%" };
}

/**
 * 一条立体声张开横线的 CSS 变量(左端 + 宽 + 高度基线 + 线粗)。
 *
 * [SL-269] `--span-h` 是**线粗**,不是几何量:它只有两个取值。
 * `half` 被 `x` 与 `100 - x` 夹住(见 `distGeometry`),声像拖到硬左/硬右时它归零 ——
 * 而一个宽 0、高 1.5px、带 pill 圆角的盒子在光栅上仍会留下一小道痕(用户实测)。
 * 零宽时把高一起归零,这个元素才真的一个像素都不画。
 *
 * CSS 侧是 `height: var(--span-h)`,**不给缺省**([PR 178 复审【建议】5]:缺省值是死代码,
 * 两个写入面都写这个变量,却会在有人改本常量时静默失配)。所以这个数**只在本文件有一份**。
 * 代价说清:将来若出现第三个写入面而忘了写这个变量,那条张开线会**整条不画**
 * (var 解析不出 ⇒ 该声明失效 ⇒ height:auto,而 `.dist-span` 绝对定位、无 top、无内容
 * ⇒ 零高),**不是**画粗了一点。与「没写入面就不该画」一致,但排查时要往这条上想。
 */
export const SPAN_THICKNESS_PX = 1.5;

export function distSpanVars(geo) {
    const w = geo.half * 2;
    return {
        "--x0": (geo.x - geo.half).toFixed(2) + "%",
        "--w": w.toFixed(2) + "%",
        "--y": "calc(18px + " + geo.h.toFixed(2) + "%)",
        // 判据用 `toFixed(2)` 之后的那个数,不是 `w` 本身:CSS 拿到的是格式化后的串,
        // 「渲染出来是 0 宽」与「原始值恰好 0」不是一回事(half = 0.001 会写成 "0.00%",
        // 屏幕上照样是零宽的一道痕)。
        // ⚠ [PR 178 复审【建议】5] 那个 half = 0.001 **不来自 distGeometry**:它返回的
        // `half` 已经过 `round2`,所以在真实调用路径上这一层恒等于 `half > 0`。这条防御
        // 对着的是**手造 geo 的调用方**(含本仓用例里的 `{half: 0.001}`)—— 留着是因为
        // 本件是共享件、拼串与逐帧补间之外还可能有第三个调用方,但别把上面那句读成
        // 「真实路径上会发生」。
        "--span-h": Number(w.toFixed(2)) > 0 ? SPAN_THICKNESS_PX + "px" : "0px",
    };
}

// =============================================================================
// 三、标记拼串(仍是纯函数:进 rows,出字符串)
// =============================================================================

/**
 * 柱体 + 立体声张开横线的 innerHTML。
 *
 * 立体声那批横线**整体排在柱体之前** —— DOM 顺序即画序,横线先画才不会盖住柱顶
 * (提取前 `spans.concat(bars)` 就是这个语义,原样保留)。
 *
 * @param {{ch:number, pan:number, volDb:number, widthPct:number,
 *          stereo?:boolean, lead?:boolean}[]} rows 要画的轨(调用方已滤过)
 * @param {number} [highlightCh] 图例 hover 联动的轨号(0 = 无);非高亮轨 data-hi="0"
 * @param {number} [globalWidthPct=100] 全局「最大角度」;见 distGeometry
 * @returns {string}
 */
export function distBarsHtml(rows, highlightCh, globalWidthPct) {
    const hi = Number(highlightCh) || 0;
    // 图例 hover 的联动位:0 = 淡出(与轨迹图 DIM_ALPHA 同档,CSS 侧落地)。
    const dim = (ch) => (hi && hi !== ch ? "0" : "1");
    const spans = [];
    const bars = [];
    for (const r of rows || []) {
        const ch = Number(r && r.ch);
        if (!Number.isFinite(ch)) continue;
        const geo = distGeometry(r.pan, r.volDb, r.widthPct, globalWidthPct);
        // [J75] B:柱体与 width 横线按轨着色。`--tc` 走**变量指向变量**
        // (`--tc: var(--track-color-7)`),色值本身仍只在 tokens.css 里定义一处。
        const tc = `--tc:var(${trackColorVar(ch)});`;
        if (r.stereo) {
            spans.push(
                `<div class="dist-span" data-ch="${ch}" data-hi="${dim(ch)}" style="${tc}${varsToStyle(distSpanVars(geo))}"></div>`,
            );
        }
        bars.push(
            `<div class="dist-bar" data-lead="${r.lead ? 1 : 0}" data-ch="${ch}" data-hi="${dim(ch)}" style="${tc}${varsToStyle(distBarVars(geo))}"></div>`,
        );
    }
    return spans.concat(bars).join("");
}

/**
 * 图例 = 色点 + 轨号 + 轨名(+ 立体声 ST 角标);hover 高亮由 data-hi 承载。
 *
 * 轨号**始终在场** —— 二色觉下非相邻两轨可能同色(tokens.css 第 21 组的实测口径),
 * 色点不是唯一线索。轨名是用户数据,一律 `esc()` 后再拼。
 *
 * @param {{ch:number, label?:string, stereo?:boolean}[]} rows 图例行(调用方已排序)
 * @param {{badge?:string, highlightCh?:number}} [opts] `badge` = ST 角标文案(词条侧给)
 * @returns {string}
 */
export function legendItemsHtml(rows, opts) {
    const o = opts || {};
    const badge = o.badge || "ST";
    const hi = Number(o.highlightCh) || 0;
    return (rows || [])
        .map(
            (r) =>
                `<span class="chart-legend__item" role="listitem" data-legend-ch="${r.ch}" data-hi="${hi === r.ch ? 1 : 0}">` +
                `<span class="chart-legend__dot" style="--tc:var(${trackColorVar(r.ch)})" aria-hidden="true"></span>` +
                `${tt2(r.ch)}${r.label ? " " + esc(r.label) : ""}` +
                (r.stereo
                    ? `<span class="chart-legend__st">${esc(badge)}</span>`
                    : "") +
                `</span>`,
        )
        .join("");
}

/**
 * 事件目标 → 图例行的轨号(不在行上即 0)。
 *
 * 两页的图例都用**事件委托**挂容器而不是逐行挂:行是每帧按可见轨集重建的,
 * 逐行挂会随重建泄漏。判据只此一处,两页不会各写一份 `closest` 选择器。
 */
export function legendChOf(target) {
    if (!target || typeof target.closest !== "function") return 0;
    const row = target.closest("[data-legend-ch]");
    return row ? Number(row.getAttribute("data-legend-ch")) || 0 : 0;
}
