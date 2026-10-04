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
// 本模块只做几何,不碰 DOM(唯一例外:文件末尾 zoomRectMode 的一次性探针,M09)。
// 复用方:T33 泳道/选区、T34 曲线手柄、T43。
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

// =============================================================================
// [M09] CSS zoom 下 getBoundingClientRect 的语义探测
// -----------------------------------------------------------------------------
// 上面的 eventToLocal,以及全仓「rect 折回设计坐标」的那一族命中测试(曲线编辑器的
// logicalFromEvent、轨迹图的 stageX、泳道坐标……)都按**新语义**写:rect 是缩放后
// 的视觉尺寸,left/top 是视口坐标,与事件的 clientX/Y 同一坐标系。Chromium 128+
// (WebView2 Evergreen)与 WebKit 26.4+ 如此。
//
// WebKit 26.4 之前(macOS 的系统 WKWebView;Sonoma 以下的系统拿不到 26.4)是
// **旧语义**:rect 按元素的有效 zoom 除回去,宽高与位置都不含缩放 —— 而事件坐标仍是
// 视口坐标,于是 zoom≠1 时命中整体偏。
//
// 旧语义下**降级、不换算**(design-lineB §3.5):外壳缩放锁在 1(见 shell-fit.js)。
// zoom=1 时两种语义逐字相同,命中测试按构造就是对的。不做换算的理由:旧 WebKit 下
// 事件坐标与 rect 的确切关系在 CI 上验证不了,盲做换算没法保证对。
//
// 判据只认**量到的**语义,不读 UA、不读版本号:一个 zoom:2、宽 100px 的不可见探针,
//   新语义:rect 宽 = 200(含 zoom),offsetWidth = 100;
//   旧语义:rect 宽 = 100(不含 zoom),offsetWidth = 100。
// offset* 在两种语义下都不含 zoom(CSS Viewport 规范的口径;Chromium 实测 rect 200 /
// offsetWidth 100),所以「rect 宽 ÷ offsetWidth」≈ 探针 zoom 即新语义、≈ 1 即旧语义。
// =============================================================================

/** rect 含 zoom(新语义;Chromium 128+ / WebKit 26.4+)。 */
export const ZOOM_RECT_STANDARD = "standard";
/** rect 不含 zoom(旧语义;WebKit 26.4 之前)。 */
export const ZOOM_RECT_LEGACY = "legacy";
/** 量不出来(没有 DOM / 探针没布局出尺寸)。调用方按**新语义**处理:不改任何行为。 */
export const ZOOM_RECT_UNKNOWN = "unknown";

/** 探针的 zoom 与宽度(px)。 */
export const ZOOM_PROBE_FACTOR = 2;
export const ZOOM_PROBE_PX = 100;

/**
 * 纯判定(node 可直接断言):探针的 rect 宽与 offsetWidth → 语义。
 *
 * 比值离探针 zoom 不到 0.25 判新语义,离 1 不到 0.1 判旧语义;两头都不沾(含 0 / NaN)
 * 判 unknown。阈值只为吸收亚像素舍入,两档中间留着大片空档,不会把一种判成另一种。
 */
export function classifyZoomRect(rectWidth, offsetWidth) {
    const rw = num(rectWidth, NaN);
    const ow = num(offsetWidth, NaN);
    if (!(rw > 0) || !(ow > 0)) return ZOOM_RECT_UNKNOWN;
    const k = rw / ow;
    if (Math.abs(k - ZOOM_PROBE_FACTOR) < 0.25) return ZOOM_RECT_STANDARD;
    if (Math.abs(k - 1) < 0.1) return ZOOM_RECT_LEGACY;
    return ZOOM_RECT_UNKNOWN;
}

/** 按文档缓存(一个文档的语义在它的生命期里不会变;WeakMap 不留住已拆的文档)。 */
const zoomModeCache = new WeakMap();

/**
 * 当前文档里 CSS zoom 下 getBoundingClientRect 的语义(结果按文档缓存)。
 *
 * 探针挂在 body(没有 body 时挂 documentElement)下,量完当场摘掉;它不可见、不收
 * 指针、不进无障碍树。unknown **不缓存**:下一次调用会再量一次。
 *
 * @param {Document} [doc] 注入用(默认全局 document)
 * @returns {"standard"|"legacy"|"unknown"}
 */
export function zoomRectMode(doc) {
    const d = doc || (typeof document !== "undefined" ? document : null);
    if (!d || typeof d.createElement !== "function") return ZOOM_RECT_UNKNOWN;
    if (zoomModeCache.has(d)) return zoomModeCache.get(d);
    const host = d.body || d.documentElement;
    if (!host || typeof host.appendChild !== "function") {
        return ZOOM_RECT_UNKNOWN;
    }
    const probe = d.createElement("div");
    probe.setAttribute("aria-hidden", "true");
    probe.style.cssText =
        "position:absolute;left:0;top:0;height:1px;margin:0;padding:0;" +
        "border:0;visibility:hidden;pointer-events:none;" +
        `width:${ZOOM_PROBE_PX}px;zoom:${ZOOM_PROBE_FACTOR}`;
    host.appendChild(probe);
    let mode = ZOOM_RECT_UNKNOWN;
    try {
        mode = classifyZoomRect(
            probe.getBoundingClientRect().width,
            probe.offsetWidth,
        );
    } finally {
        host.removeChild(probe);
    }
    if (mode !== ZOOM_RECT_UNKNOWN) zoomModeCache.set(d, mode);
    return mode;
}
