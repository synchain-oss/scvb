// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · Tab1 pan 角度域增益曲线编辑器(T34)
// -----------------------------------------------------------------------------
// 目标:与 scvb_core 同一条插值算法的可视编辑器(02 §7 / 契约 §1.17)。
// 职责边界:
//   • 本文件只管**曲线窗内部**(加点/拖拽/Q/shape/删除/a11y/side 三段选 + 手柄方向指示
//      + MS 等效增益叠加线)。窗外的 eyebrow / 图例 / X/Y 刻度 / 空态 DOM 仍是
//     index.html + tab-master.js 的既有结构,本文件不重建它们。
//   • 两段导出:**纯函数**(无 DOM,node 可直接 import 断言)+ createCurveEditor()
//     (DOM 接线)。模块顶层零副作用、零 document 触碰。
//
// 数学真源(与 src/core/analysis/PanCurve.{h,cpp} 逐字同构):
//   G(P) = clamp( Σ_k Shape_k(P), −24, +12 )   [dB]
//   半宽 Δ = 100 / Q;bell u=(P−P₀)/Δ;A·2^(−u²)
//   shelf:u 由 side 决定 —— right→(P−P₀)/Δ、left→(P₀−P)/Δ、
//          out→P₀≥0 按 right / P₀<0 按 left(对任意 P₀ 确定性求值,无未定义区)
//   shelf:A·0.5·(1+tanh(2u))
//   cut:slope 模型 —— d = 侧向距离(right→P−P₀、left→P₀−P、out→sign(P₀) 定),
//        d=max(|d|,d0)(d0=1°),u=log2(d/d0),u_b=|A|/s(q 承载 slope),
//        G=A·smoothstep(u/u_b) clamp 到 [A,0];d≤0(保留侧)→0
//   LUT = 2049 点(−100..+100,步长 ≈0.0977,奇数保证 0° 恰为格点);
//        gainDb(P)=clamp + 线性插值。
//   MS 等效增益(02 §8.4,J68 纯显示层):
//        t=ms_balance/100;g_M=1−max(t,0);g_S=1−max(−t,0);θ=(P+100)/200·(π/2)
//        g_eq(P)=10·log10(max(½·[g_M²(1+sin2θ)+g_S²(1−sin2θ)], 1e−12))
//
// 纪律:状态一律写 data-* 属性;词条一律走 web/shared/i18n.js 的 curve.* key;
// 不改 tab-master.js / canvas/meter.js 的既有逻辑;模块顶层零副作用。
// =============================================================================

// =============================================================================
// 一、纯函数(无 DOM;node 侧断言面 + 对拍脚本共用)
// =============================================================================

/** 点数上限(契约 §1.17 整表提交 ≤16 点)。 */
export const MAX_POINTS = 16;

/** Q 值域(02 §7:Q ∈ [0.5, 10])。 */
export const Q_RANGE = Object.freeze({ min: 0.5, max: 10 });

/** 加点默认 Q(05 §6.2 交互表:双击空白新增默认 q=1)。 */
export const DEFAULT_Q = 1;

/** 加点默认 shape / side(05 §6.2 + 契约 §1.17)。 */
export const DEFAULT_SHAPE = "bell";
export const DEFAULT_SIDE = "out";

/** cut 斜率档位(dB/oct;02 §7.1 修订 / 契约 §1.17)。 */
export const SLOPES = Object.freeze([6, 12, 18, 24]);
/** cut 默认斜率(dB/oct)。 */
export const DEFAULT_SLOPE = 12;

/** cut slope 模型侧向距离地板 d0 = 1.0(度)。 */
export const CUT_D0 = 1;

/** shape / side 枚举(契约 §1.17;side 默认 out,J07)。 */
export const SHAPES = Object.freeze(["bell", "shelf", "cut"]);
export const SIDES = Object.freeze(["out", "left", "right"]);

/** 角度域(契约 §1.17:angle −100..100)。 */
export const ANGLE_MIN = -100;
export const ANGLE_MAX = 100;

/** UI 编辑/显示域(05 §6.2:「y 夹显示范围(建议 ±12dB)」)。 */
export const GAIN_DB_MIN = -12;
export const GAIN_DB_MAX = 12;

/** DSP 整体 clamp 边界(02 §7.1 / PanCurve.h)。 */
export const CURVE_DB_MIN = -24;
export const CURVE_DB_MAX = 12;

/** LUT 点数(2049;PanCurve.h kPanCurveLutSize,CI #56 提密后口径)。 */
export const LUT_SIZE = 2049;

/** side=out 中心区阈值(02 §7.1:UI 在 |angle|<5 时强制改选 left/right)。 */
export const CENTER_SIDE_THRESHOLD = 5;

/** 画布逻辑尺寸(与 index.html 曲线窗 SVG viewBox 同源)。 */
export const PLOT_W = 660;
export const PLOT_H = 214;
/** 0 dB 的 y 坐标 / +12 dB 的 y 坐标(设计稿网格 y=109 / y=35)。 */
export const PLOT_ZERO_Y = 109;
export const PLOT_TOP_Y = 35;

export function clamp(lo, hi, v) {
    return v < lo ? lo : v > hi ? hi : v;
}

/** 占位符求值(与 tab-master.js 的 format 同口径;i18n 只发字典,不做模板求值)。 */
export function format(text, vals) {
    return String(text).replace(/[{](\w+)[}]/g, (m, k) =>
        Object.prototype.hasOwnProperty.call(vals, k) ? String(vals[k]) : m,
    );
}

// ---------------------------------------------------------------- DSP 数学
/** 半宽 Δ = 100/Q(02 §7.1)。 */
export function halfWidth(q) {
    return 100 / q;
}

/** 单点形状求值(dB,不 clamp)。与 PanCurve.cpp bellValue/shelfValue/cutValue 同构。 */
export function evalShape(pt, pan) {
    if (pt.shape === "shelf" || pt.shape === "cut") {
        // 侧向距离 d(带号,切除侧为正):right→(P−P₀)、left→(P₀−P)、out→sign(P₀)。
        let d;
        if (pt.side === "left") d = pt.angle - pan;
        else if (pt.side === "right") d = pan - pt.angle;
        else d = pt.angle >= 0 ? pan - pt.angle : pt.angle - pan;
        if (pt.shape === "shelf") {
            const u = d / halfWidth(pt.q);
            return pt.gain_db * 0.5 * (1 + Math.tanh(2 * u));
        }
        // cut:slope 模型(q 承载 slope dB/oct);无谐振凸起 R。
        if (d <= 0) return 0; // 保留侧:只切该侧,切点自身不受影响
        const a = pt.gain_db; // A(cut 恒 ≤0)
        const s = pt.q; // slope(dB/oct)
        if (!(s > 0)) return 0;
        const dd = Math.max(d, CUT_D0);
        const u = Math.log2(dd / CUT_D0);
        const ub = Math.abs(a) / s;
        if (!(ub > 0)) return 0; // A=0 → 无切除(避免 0/0 = NaN)
        const w = clamp(0, 1, u / ub);
        const ss = w * w * (3 - 2 * w);
        const lo = Math.min(a, 0);
        const hi = Math.max(a, 0);
        return clamp(lo, hi, a * ss); // clamp 到 [A, 0]
    }
    const u = (pan - pt.angle) / halfWidth(pt.q);
    return pt.gain_db * Math.pow(2, -(u * u));
}

function clampDb(db) {
    return db < CURVE_DB_MIN
        ? CURVE_DB_MIN
        : db > CURVE_DB_MAX
          ? CURVE_DB_MAX
          : db;
}

/** 整条曲线解析求值:Σ 各点 + clamp 到 [−24, +12] dB(02 §7.1)。 */
export function evalCurve(points, pan) {
    let db = 0;
    for (const pt of points) db += evalShape(pt, pan);
    return clampDb(db);
}

/**
 * 2049 点 LUT(与 PanCurveLut::rebuild 同构)。返回 Float32Array —— C++ 侧
 * m_lut 是 std::array<float,2049>,(float)clampDb(db) 的 float32 取整与 Float32Array
 * 存储天然一致。网格 pan = −100 + 200·i/2048 是 256 分母的二进制有理数,float/double 均精确。
 */
export function buildLut(points) {
    const lut = new Float32Array(LUT_SIZE);
    for (let i = 0; i < LUT_SIZE; i++) {
        const pan = -100 + (200 * i) / (LUT_SIZE - 1);
        let db = 0;
        for (const pt of points) db += evalShape(pt, pan);
        lut[i] = clampDb(db);
    }
    return lut;
}

/** clamp + 线性插值(与 PanCurveLut::gainDb 同构)。 */
export function lutGainDb(lut, pan) {
    const clamped = clamp(-100, 100, Number.isFinite(pan) ? pan : 0);
    const x = (clamped * (LUT_SIZE - 1)) / 200 + (LUT_SIZE - 1) / 2;
    const i = Math.floor(x);
    const frac = x - i;
    const i0 = Math.max(0, Math.min(i, LUT_SIZE - 1));
    const i1 = Math.min(i0 + 1, LUT_SIZE - 1);
    return lut[i0] + (lut[i1] - lut[i0]) * frac;
}

/**
 * MS Balance 等效增益曲线 g_eq(P)(02 §8.4,J68;纯显示层,不写入任何数据)。
 * @returns {number} dB
 */
export function eqGainDb(msBalance, pan) {
    const t = clamp(-100, 100, Number(msBalance) || 0) / 100;
    const gM = 1 - Math.max(t, 0);
    const gS = 1 - Math.max(-t, 0);
    const th = ((clamp(-100, 100, pan) + 100) / 200) * (Math.PI / 2);
    const sin2 = Math.sin(2 * th);
    const energy = 0.5 * (gM * gM * (1 + sin2) + gS * gS * (1 - sin2));
    return 10 * Math.log10(Math.max(energy, 1e-12));
}

// ---------------------------------------------------------------- side 逻辑
/**
 * shelf/cut 手柄/求值方向的**有效 side**(02 §7.1)。bell 忽略 side;
 * out 的方向恒由 sign(angle) 决定(angle≥0→right、angle<0→left)。
 */
export function resolveSide(pt) {
    if (pt.shape !== "shelf" && pt.shape !== "cut") return "out";
    if (pt.side === "left" || pt.side === "right") return pt.side;
    return pt.angle >= 0 ? "right" : "left";
}

/** 是否需要「中心点请选择方向」浮条(05 §6.2 R2)。 */
export function needsSideHint(pt) {
    return (
        (pt.shape === "shelf" || pt.shape === "cut") &&
        pt.side === "out" &&
        Math.abs(pt.angle) < CENTER_SIDE_THRESHOLD
    );
}

// ---------------------------------------------------------------- 点操作(纯函数)
/** 夹取/回填默认值,产出可提交的点(不改入参)。 */
export function normalizePoint(pt) {
    const p = pt || {};
    const shape = SHAPES.includes(p.shape) ? p.shape : DEFAULT_SHAPE;
    const qRaw = Number.isFinite(p.q)
        ? p.q
        : shape === "cut"
          ? DEFAULT_SLOPE
          : DEFAULT_Q;
    const q =
        shape === "cut"
            ? qRaw // cut:q 承载 slope(dB/oct),不受 Q 值域夹取
            : clamp(Q_RANGE.min, Q_RANGE.max, qRaw);
    return {
        angle: clamp(
            ANGLE_MIN,
            ANGLE_MAX,
            Number.isFinite(p.angle) ? p.angle : 0,
        ),
        gain_db: clamp(
            GAIN_DB_MIN,
            GAIN_DB_MAX,
            Number.isFinite(p.gain_db) ? p.gain_db : 0,
        ),
        shape,
        q,
        side: SIDES.includes(p.side) ? p.side : DEFAULT_SIDE,
    };
}

/** 加点(默认 bell / q=1 / gain 取双击 y);达 16 点上限返回 null。纯函数,不改入参。 */
export function addPoint(points, angle, db) {
    const list = points || [];
    if (list.length >= MAX_POINTS) return null;
    const next = list.slice();
    next.push(
        normalizePoint({
            angle,
            gain_db: db,
            shape: DEFAULT_SHAPE,
            q: DEFAULT_Q,
            side: DEFAULT_SIDE,
        }),
    );
    next.sort((a, b) => a.angle - b.angle);
    return next;
}

/** 删点(下标越界则原样返回新数组)。纯函数。 */
export function removePoint(points, index) {
    const list = points || [];
    if (!Number.isInteger(index) || index < 0 || index >= list.length)
        return list.slice();
    const next = list.slice();
    next.splice(index, 1);
    return next;
}

/** 改点(整字段 patch 后重新 normalize)。纯函数。 */
export function updatePoint(points, index, patch) {
    const list = points || [];
    if (!Number.isInteger(index) || index < 0 || index >= list.length)
        return list.slice();
    const next = list.slice();
    next[index] = normalizePoint({ ...next[index], ...(patch || {}) });
    return next;
}

/** 拖拽改 angle/gain(夹取;shape/q/side 保持)。纯函数。 */
export function movePointTo(points, index, angle, db) {
    const list = points || [];
    if (!Number.isInteger(index) || index < 0 || index >= list.length)
        return list.slice();
    const next = list.slice();
    next[index] = normalizePoint({ ...next[index], angle, gain_db: db });
    return next;
}

// ---------------------------------------------------------------- 几何(画布逻辑坐标)
/** angle → x(逻辑 660)。 */
export function angleToX(angle) {
    return ((clamp(ANGLE_MIN, ANGLE_MAX, angle) + 100) / 200) * PLOT_W;
}

/** x → angle。 */
export function xToAngle(x) {
    return (x / PLOT_W) * 200 - 100;
}

/** dB → y(逻辑 214;y=109 为 0 dB,+12 在 y=35)。 */
export function dbToY(db) {
    return (
        PLOT_ZERO_Y -
        clamp(GAIN_DB_MIN, GAIN_DB_MAX, db) *
            ((PLOT_ZERO_Y - PLOT_TOP_Y) / GAIN_DB_MAX)
    );
}

/** y → dB。 */
export function yToDb(y) {
    return clamp(
        GAIN_DB_MIN,
        GAIN_DB_MAX,
        (PLOT_ZERO_Y - y) * (GAIN_DB_MAX / (PLOT_ZERO_Y - PLOT_TOP_Y)),
    );
}

// ---------------------------------------------------------------- 读数/播报
/** 一位小数并去尾 .0(角度:整数形 −35、半度形 2.5)。 */
export function trim1(v) {
    const r = Math.round(v * 10) / 10;
    return Number.isInteger(r) ? String(r) : r.toFixed(1);
}

/** 带号 dB(正 +、负 U+2212,零不带号,一位小数)。 */
export function signedDb(v) {
    const r = Math.round(v * 10) / 10;
    if (r === 0) return "0.0";
    return (r > 0 ? "+" : "−") + Math.abs(r).toFixed(1);
}

/** Q 读数(一位小数,mono 显示)。 */
export function qLabel(q) {
    return (Math.round(q * 10) / 10).toFixed(1);
}

/** 角度读数(aria-live 用;负号统一 U+2212)。 */
export function angleLabel(a) {
    const v = Math.round(a * 10) / 10;
    return (v < 0 ? "−" : "") + trim1(Math.abs(v));
}

/**
 * 选中点状态播报文本(05 §6.2 a11y:「点 2:角度 -35,+2.5 dB,bell,Q 1.4;
 * shelf/cut 时追加方向」)。纯函数,取 i18n 字典 t 与 1 基序号。
 */
export function announcePoint(index, pt, t) {
    const vals = {
        n: index + 1,
        angle: angleLabel(pt.angle),
        gain: signedDb(pt.gain_db),
        shape: t["curve.shape." + pt.shape] || pt.shape,
        q: qLabel(pt.q),
    };
    const needsDir = pt.shape === "shelf" || pt.shape === "cut";
    const key = needsDir ? "curve.announcePointDir" : "curve.announcePoint";
    if (needsDir)
        vals.side = t["curve.side." + resolveSide(pt)] || resolveSide(pt);
    return format(t[key] || key, vals);
}

// =============================================================================
// 二、DOM 接线
// =============================================================================

/**
 * @param {{
 *   canvas: HTMLCanvasElement,
 *   root: Document|Element,
 *   bridge: object|null,
 *   getStore: () => object,
 *   getT: () => object,
 *   onLocalChange: () => void
 * }} opts
 *   canvas —— index.html 的 [data-gb="master-pancurve-canvas"] 锚点(本卡接管);
 *   getStore() —— app.js 事件仓(读 state.versions[active].pan_curve 与
 *                  params.values.ms_balance);getT() —— 当前语言字典。
 */
export function createCurveEditor(opts) {
    const o = opts || {};
    const canvas = o.canvas;
    const root = o.root || (canvas && canvas.ownerDocument) || null;
    const bridge = o.bridge;
    const getStore = o.getStore || (() => ({}));
    const getT = o.getT || (() => ({}));
    const onLocalChange =
        typeof o.onLocalChange === "function" ? o.onLocalChange : () => {};

    if (!canvas || typeof canvas.getContext !== "function") {
        return { mount() {}, render() {}, draw() {}, push() {} };
    }

    const ctx = canvas.getContext("2d");
    const card =
        root && root.querySelector
            ? root.querySelector('[data-gb="master-pancurve"]')
            : null;
    // 隐藏 T31 的 SVG 网格/折线占位层:曲线 + 网格一律由本 canvas 接管(窗底 .sc-dark 透出)。
    const svgLayer = card ? card.querySelector("svg") : null;

    const local = {
        selected: -1,
        dragging: false,
        dragIndex: -1,
        dragPoints: null,
        shift: false,
        commitTimer: 0,
        hintTimer: 0,
        lutCache: { key: "", lut: null },
    };

    function points() {
        const s = getStore().state || {};
        const v = (s.global && s.global.version_active) || 1;
        const version = (s.versions || [])[v - 1];
        return (version && version.pan_curve && version.pan_curve.points) || [];
    }

    function msBalance() {
        const prm = getStore().params || {};
        const v = Number(prm.values && prm.values.ms_balance);
        return Number.isFinite(v) ? v : 0;
    }

    // ---- 画布尺寸 + 缩放(05 §6.1:canvas.width=cssW·k,k=uiScale·dpr)----------
    let sx = 1;
    let sy = 1;
    function resize() {
        const rect = canvas.getBoundingClientRect();
        const dpr =
            (typeof window !== "undefined" && window.devicePixelRatio) || 1;
        const w = Math.max(1, Math.round(rect.width * dpr));
        const h = Math.max(1, Math.round(rect.height * dpr));
        if (canvas.width !== w) canvas.width = w;
        if (canvas.height !== h) canvas.height = h;
        sx = w / PLOT_W;
        sy = h / PLOT_H;
        draw();
    }

    // ---- CSS token 读取(零裸 hex;token 真源 = web/shared/tokens.css)----------
    function cssVar(name, fallback) {
        if (!card || typeof getComputedStyle !== "function") return fallback;
        const v = getComputedStyle(card).getPropertyValue(name);
        return v && v.trim() ? v.trim() : fallback;
    }
    function colors() {
        const acc = cssVar("--acc", "181, 172, 201");
        return {
            grid: cssVar("--dark-grid", "rgba(255,255,255,0.08)"),
            axis: cssVar("--dark-grid-axis", "rgba(255,255,255,0.2)"),
            accent: cssVar("--acc-strong", "rgba(181,172,201,0.95)"),
            handle: cssVar("--txt-dark-1", "rgba(237,234,244,0.95)"),
            overlay: cssVar("--txt-dark-1", "rgba(237,234,244,0.72)"),
            fill: "rgba(" + acc.trim() + ", 0.12)",
        };
    }

    // ---- 绘制 -------------------------------------------------------------
    function drawGrid(c) {
        ctx.strokeStyle = c.grid;
        ctx.lineWidth = 1;
        ctx.beginPath();
        for (const y of [PLOT_TOP_Y, 72, PLOT_ZERO_Y, 146, 183]) {
            ctx.moveTo(0, y);
            ctx.lineTo(PLOT_W, y);
        }
        for (const x of [0, 165, 330, 495, 660]) {
            ctx.moveTo(x, 0);
            ctx.lineTo(x, PLOT_H);
        }
        ctx.stroke();
        ctx.strokeStyle = c.axis;
        ctx.beginPath();
        ctx.moveTo(0, PLOT_ZERO_Y);
        ctx.lineTo(PLOT_W, PLOT_ZERO_Y);
        ctx.moveTo(330, 0);
        ctx.lineTo(330, PLOT_H);
        ctx.stroke();
    }

    /** 采样 661 列画一条 dB 曲线(主曲线走 LUT 插值,叠加线走解析 g_eq)。 */
    function traceCurve(getDb) {
        ctx.beginPath();
        for (let px = 0; px <= PLOT_W; px++) {
            const pan = xToAngle(px);
            const y = dbToY(getDb(pan));
            if (px === 0) ctx.moveTo(px, y);
            else ctx.lineTo(px, y);
        }
    }

    function draw() {
        if (!ctx) return;
        ctx.setTransform(sx, 0, 0, sy, 0, 0);
        ctx.clearRect(0, 0, PLOT_W, PLOT_H);
        const c = colors();
        const pts = local.dragPoints || points();
        // LUT 缓存:仅点集变化才重建;MS 叠加线拖动时主曲线 LUT 复用,不逐事件重建
        const lutKey = pts
            .map(
                (p) =>
                    p.angle +
                    "," +
                    p.gain_db +
                    "," +
                    p.shape +
                    "," +
                    p.q +
                    "," +
                    p.side,
            )
            .join("|");
        let lut;
        if (local.lutCache.key === lutKey && local.lutCache.lut) {
            lut = local.lutCache.lut;
        } else {
            lut = buildLut(pts);
            local.lutCache.key = lutKey;
            local.lutCache.lut = lut;
        }
        drawGrid(c);

        // 主曲线 G(P):accent 2px + 半透明填充(填充到窗底 −12 dB 线)
        traceCurve((pan) => lutGainDb(lut, pan));
        ctx.strokeStyle = c.accent;
        ctx.lineWidth = 2;
        ctx.stroke();
        ctx.lineTo(PLOT_W, PLOT_H);
        ctx.lineTo(0, PLOT_H);
        ctx.closePath();
        ctx.fillStyle = c.fill;
        ctx.fill();

        // MS 等效增益叠加线(恰为一条;J68/J71④ 纯显示层,不写任何数据)
        const ms = msBalance();
        ctx.strokeStyle = c.overlay;
        ctx.lineWidth = 1.6;
        ctx.setLineDash([5, 4]);
        traceCurve((pan) => eqGainDb(ms, pan));
        ctx.stroke();
        ctx.setLineDash([]);

        // 手柄(圆=bell、方=shelf、三角=cut;shelf/cut 追加方向指示)
        pts.forEach((pt, i) => drawHandle(pt, i, c));
    }

    function drawHandle(pt, i, c) {
        const x = angleToX(pt.angle);
        const y = dbToY(pt.gain_db);
        const sel = i === local.selected;
        const size = sel ? 6.5 : 5;
        ctx.save();
        ctx.shadowColor = "rgba(181,172,201,0.8)";
        ctx.shadowBlur = sel ? 12 : 8;
        ctx.fillStyle = c.handle;
        ctx.strokeStyle = c.accent;
        ctx.lineWidth = sel ? 1.5 : 1;
        ctx.beginPath();
        if (pt.shape === "cut") {
            ctx.moveTo(x, y - size);
            ctx.lineTo(x - size, y + size * 0.8);
            ctx.lineTo(x + size, y + size * 0.8);
            ctx.closePath();
        } else if (pt.shape === "shelf") {
            ctx.rect(x - size, y - size, size * 2, size * 2);
        } else {
            ctx.arc(x, y, size, 0, Math.PI * 2);
        }
        ctx.fill();
        ctx.stroke();

        // shelf/cut 方向指示(三角朝有效 side)
        if (pt.shape === "shelf" || pt.shape === "cut") {
            const dir = resolveSide(pt) === "left" ? -1 : 1;
            const ax = x + dir * (size + 6);
            ctx.shadowBlur = 0;
            ctx.fillStyle = c.overlay;
            ctx.beginPath();
            ctx.moveTo(ax, y);
            ctx.lineTo(ax - dir * 5, y - 3.5);
            ctx.lineTo(ax - dir * 5, y + 3.5);
            ctx.closePath();
            ctx.fill();
        }
        ctx.restore();
    }

    // ---- 命中测试(命中半径 ≥12 设计 px,CSS 坐标换算)------------------------
    function logicalFromEvent(e) {
        const rect = canvas.getBoundingClientRect();
        return {
            x: ((e.clientX - rect.left) / rect.width) * PLOT_W,
            y: ((e.clientY - rect.top) / rect.height) * PLOT_H,
        };
    }
    function hitTest(e) {
        const lp = logicalFromEvent(e);
        const rect = canvas.getBoundingClientRect();
        const rx = 12 * (PLOT_W / rect.width);
        const ry = 12 * (PLOT_H / rect.height);
        let best = -1;
        let bestD = Infinity;
        points().forEach((pt, i) => {
            const dx = angleToX(pt.angle) - lp.x;
            const dy = dbToY(pt.gain_db) - lp.y;
            const d = (dx / rx) * (dx / rx) + (dy / ry) * (dy / ry);
            if (d < 1 && d < bestD) {
                bestD = d;
                best = i;
            }
        });
        return best;
    }

    // ---- 上行提交(契约 §1.17:pointerup/工具条变更后整表提交)---------------
    async function commit(next) {
        if (!bridge || typeof bridge.setPanCurve !== "function") {
            local.dragPoints = null;
            return;
        }
        try {
            await bridge.setPanCurve(next);
        } catch (e) {
            console.warn(
                "SCVB curve-editor:setPanCurve() 调用失败 —— " + e.message,
            );
        } finally {
            // echo 之后才清本地待提交态:避免 commit 与回显之间的窗口里
            // render()/draw() 退回旧 store 造成「曲线一跳一跳」。只清「仍是当前这批」。
            if (local.dragPoints === next) local.dragPoints = null;
        }
    }

    // ---- 浮条 + aria-live -------------------------------------------------
    let _hintEl = null;
    let _liveEl = null;
    function hintEl() {
        if (!_hintEl && card && typeof document !== "undefined") {
            _hintEl = document.createElement("div");
            _hintEl.className = "curve-toast";
            _hintEl.setAttribute("role", "status");
            _hintEl.hidden = true;
            card.appendChild(_hintEl);
        }
        return _hintEl;
    }
    function liveEl() {
        if (!_liveEl && card && typeof document !== "undefined") {
            _liveEl = document.createElement("div");
            _liveEl.className = "sr-only";
            _liveEl.setAttribute("aria-live", "polite");
            _liveEl.setAttribute("aria-atomic", "true");
            card.appendChild(_liveEl);
        }
        return _liveEl;
    }
    function showHint(text) {
        const el = hintEl();
        if (!el) return;
        el.textContent = text;
        el.hidden = false;
        clearTimeout(local.hintTimer);
        local.hintTimer = setTimeout(() => {
            el.hidden = true;
        }, 2600);
    }
    function announce(text) {
        const el = liveEl();
        if (!el) return;
        el.textContent = "";
        if (typeof requestAnimationFrame === "function") {
            requestAnimationFrame(() => {
                el.textContent = text;
            });
        } else {
            el.textContent = text;
        }
    }

    function select(i) {
        local.selected = i;
        syncToolbar();
        if (i >= 0) announce(announcePoint(i, points()[i], getT()));
        draw();
    }

    // ---- 加点/删点 ---------------------------------------------------------
    function addAt(lp) {
        const cur = points();
        if (cur.length >= MAX_POINTS) {
            showHint(getT()["curve.maxPoints"] || "curve.maxPoints");
            return;
        }
        const angle = xToAngle(lp.x);
        const db = yToDb(lp.y);
        // 预先归一化出「即将插入的点」,addPoint 内部对同一组入参做同样归一化,
        // 故 next 里必有一项与 newPt 逐字段相等;据此回找真实下标(按 angle 排序后
        // 新点未必在末尾,旧写法 next.length-1 会选中最右旧点,让工具条/方向键/aria 落错)。
        const newPt = normalizePoint({
            angle,
            gain_db: db,
            shape: DEFAULT_SHAPE,
            q: DEFAULT_Q,
            side: DEFAULT_SIDE,
        });
        const next = addPoint(cur, newPt.angle, newPt.gain_db);
        if (!next) return;
        commit(next);
        const idx = next.findIndex(
            (p) =>
                p.angle === newPt.angle &&
                p.gain_db === newPt.gain_db &&
                p.shape === newPt.shape &&
                p.q === newPt.q &&
                p.side === newPt.side,
        );
        local.selected = idx >= 0 ? idx : 0;
        syncToolbar();
        if (local.selected >= 0 && local.selected < next.length) {
            announce(
                announcePoint(local.selected, next[local.selected], getT()),
            );
        }
        draw();
    }

    function deleteAt(i) {
        const cur = points();
        if (i < 0 || i >= cur.length) return;
        commit(removePoint(cur, i));
        local.selected = -1;
        syncToolbar();
        draw();
    }

    // ---- 拖拽 -------------------------------------------------------------
    function onPointerDown(e) {
        const i = hitTest(e);
        if (i >= 0) {
            local.dragging = true;
            local.dragIndex = i;
            local.dragPoints = points().slice();
            select(i);
            if (canvas.setPointerCapture) canvas.setPointerCapture(e.pointerId);
            e.preventDefault();
        } else {
            select(-1);
        }
    }

    function onPointerMove(e) {
        if (!local.dragging || local.dragIndex < 0) return;
        const lp = logicalFromEvent(e);
        let angle = clamp(ANGLE_MIN, ANGLE_MAX, xToAngle(lp.x));
        let db = yToDb(lp.y);
        if (local.shift) {
            angle = Math.round(angle * 2) / 2;
            db = Math.round(db * 10) / 10;
        }
        const next = movePointTo(points(), local.dragIndex, angle, db);
        local.dragPoints = next;
        draw();
    }

    function onPointerUp() {
        if (!local.dragging) return;
        local.dragging = false;
        const idx = local.dragIndex;
        local.dragIndex = -1;
        const cur = points();
        if (idx < 0 || idx >= cur.length) {
            local.dragPoints = null;
            draw();
            return;
        }
        const target = local.dragPoints ? local.dragPoints[idx] : cur[idx];
        const next = (local.dragPoints || cur).slice();
        // side=out 且 |angle|<5 → 自动改选 left/right 并浮条提示(05 §6.2 R2)
        if (needsSideHint(target)) {
            next[idx] = { ...target, side: resolveSide(target) };
            showHint(getT()["curve.centerSide"] || "curve.centerSide");
        }
        // 使 commit() finally 的引用守卫(local.dragPoints === next)成立:next 是
        // .slice() 出的新数组,必须先把 dragPoints 重新指向它,否则 finally 永远不清,
        // 拖后 draw() 一直读旧数组(切版本不刷新 / side 改向显示错 / 键盘微调不可见)。
        local.dragPoints = next;
        commit(next);
        local.selected = idx;
        syncToolbar();
        if (idx >= 0) announce(announcePoint(idx, next[idx], getT()));
        draw();
    }

    // ---- 滚轮:bell/shelf 调 Q,cut 步进斜率档 ----------------------------------
    function onWheel(e) {
        if (local.selected >= points().length) local.selected = -1;
        if (local.selected < 0) return;
        e.preventDefault();
        const cur = points();
        const idx = local.selected;
        const pt = cur[idx];
        let next;
        if (pt.shape === "cut") {
            const dir = e.deltaY < 0 ? 1 : -1;
            const i = SLOPES.indexOf(Math.round(pt.q));
            const ni = clamp(
                0,
                SLOPES.length - 1,
                i < 0 ? (dir > 0 ? 0 : SLOPES.length - 1) : i + dir,
            );
            next = updatePoint(cur, idx, { q: SLOPES[ni] });
        } else {
            const factor = e.deltaY < 0 ? 1.12 : 1 / 1.12;
            const q = clamp(Q_RANGE.min, Q_RANGE.max, pt.q * factor);
            next = updatePoint(cur, idx, { q });
        }
        local.dragPoints = next;
        syncToolbar();
        draw();
        clearTimeout(local.commitTimer);
        local.commitTimer = setTimeout(() => {
            commit(next);
        }, 140);
    }

    // ---- 键盘 -------------------------------------------------------------
    function onKeyDown(e) {
        const cur = points();
        const n = cur.length;

        // Esc:取消选中(键盘可退出编辑态)
        if (e.key === "Escape") {
            if (local.selected >= 0) {
                local.selected = -1;
                syncToolbar();
                draw();
            }
            return;
        }

        // Enter/Space:加点到默认位(角度 0、0 dB)。pan 曲线是角度域,无「播放位置」维度,
        // 故取中性默认位;达 16 点上限时浮条提示。
        if (e.key === "Enter" || e.key === " ") {
            e.preventDefault();
            if (n >= MAX_POINTS) {
                showHint(getT()["curve.maxPoints"] || "curve.maxPoints");
                return;
            }
            addAt({ x: PLOT_W / 2, y: PLOT_ZERO_Y });
            return;
        }

        // Shift+←/→:在点间移动选中(循环;无选中时从首/末进入)。
        // 普通方向键按 05 §6.2「方向键微调」保留给选中点微调,故导航走 Shift 修饰。
        if (e.shiftKey && (e.key === "ArrowLeft" || e.key === "ArrowRight")) {
            e.preventDefault();
            if (n === 0) return;
            let idx = local.selected;
            if (idx < 0) idx = e.key === "ArrowRight" ? 0 : n - 1;
            else
                idx =
                    e.key === "ArrowRight" ? (idx + 1) % n : (idx - 1 + n) % n;
            select(idx);
            return;
        }

        // 无选中时的普通方向键:选中首个点(键盘选点入口)
        if (local.selected < 0) {
            if (n === 0) return;
            if (e.key.startsWith("Arrow")) {
                e.preventDefault();
                select(0);
            }
            return;
        }

        // selected 越界防御(版本切换/删除后 store 可能先于 render 变化)
        if (local.selected >= n) {
            local.selected = -1;
            return;
        }

        const idx = local.selected;
        const pt = cur[idx];
        let angle = pt.angle;
        let db = pt.gain_db;
        if (e.key === "ArrowLeft") angle -= 0.5;
        else if (e.key === "ArrowRight") angle += 0.5;
        else if (e.key === "ArrowUp") db += 0.1;
        else if (e.key === "ArrowDown") db -= 0.1;
        else if (e.key === "Delete" || e.key === "Backspace") {
            e.preventDefault();
            deleteAt(idx);
            return;
        } else {
            return;
        }
        e.preventDefault();
        const next = movePointTo(cur, idx, angle, db);
        commit(next);
        local.selected = idx;
        syncToolbar();
        draw();
        announce(announcePoint(idx, next[idx], getT()));
    }

    // ---- 工具条(深色玻璃,选中点弹出)---------------------------------------
    let _toolbar = null;
    function buildToolbar() {
        if (!card || typeof document === "undefined") return null;
        const bar = document.createElement("div");
        bar.className = "curve-toolbar";
        bar.setAttribute("data-gb", "master-pancurve-toolbar");
        bar.setAttribute("role", "toolbar");
        bar.hidden = true;

        const shapeGroup = document.createElement("div");
        shapeGroup.className = "curve-toolbar__group";
        shapeGroup.setAttribute("role", "group");
        shapeGroup.setAttribute("aria-label", "shape");
        for (const s of SHAPES) {
            const b = document.createElement("button");
            b.type = "button";
            b.className = "curve-toolbar__opt";
            b.setAttribute("data-curve-shape", s);
            b.textContent = getT()["curve.shape." + s] || s;
            b.addEventListener("click", () => setShape(s));
            shapeGroup.appendChild(b);
        }
        bar.appendChild(shapeGroup);

        const sideGroup = document.createElement("div");
        sideGroup.className = "curve-toolbar__group";
        sideGroup.setAttribute("role", "group");
        sideGroup.setAttribute("aria-label", "side");
        sideGroup.setAttribute("data-curve-side-group", "1");
        for (const s of SIDES) {
            const b = document.createElement("button");
            b.type = "button";
            b.className = "curve-toolbar__opt";
            b.setAttribute("data-curve-side", s);
            b.textContent = getT()["curve.side." + s] || s;
            b.addEventListener("click", () => setSide(s));
            sideGroup.appendChild(b);
        }
        const sideInfo = document.createElement("span");
        sideInfo.className = "curve-toolbar__info";
        sideInfo.setAttribute("data-curve-side-info", "1");
        sideInfo.textContent = "ⓘ";
        sideInfo.title = getT()["curve.sideTooltip"] || "";
        sideInfo.setAttribute("aria-label", getT()["curve.sideTooltip"] || "");
        sideGroup.appendChild(sideInfo);
        bar.appendChild(sideGroup);

        const slopeGroup = document.createElement("div");
        slopeGroup.className = "curve-toolbar__group curve-toolbar__slope";
        slopeGroup.setAttribute("role", "group");
        slopeGroup.setAttribute(
            "aria-label",
            getT()["curve.slopeLabel"] || "slope",
        );
        slopeGroup.setAttribute("data-curve-slope-group", "1");
        const slopeLabelEl = document.createElement("span");
        slopeLabelEl.className = "curve-toolbar__slope-label";
        slopeLabelEl.textContent = getT()["curve.slopeLabel"] || "slope";
        slopeGroup.appendChild(slopeLabelEl);
        for (const s of SLOPES) {
            const b = document.createElement("button");
            b.type = "button";
            b.className = "curve-toolbar__opt";
            b.setAttribute("data-curve-slope", s);
            b.textContent = getT()["curve.slope.opt" + s] || s + " dB/oct";
            b.addEventListener("click", () => setSlope(s));
            slopeGroup.appendChild(b);
        }
        bar.appendChild(slopeGroup);

        const qWrap = document.createElement("div");
        qWrap.className = "curve-toolbar__q";
        const qLabelEl = document.createElement("span");
        qLabelEl.className = "curve-toolbar__q-label";
        qLabelEl.textContent = getT()["curve.qLabel"] || "Q";
        const qSlider = document.createElement("input");
        qSlider.type = "range";
        qSlider.min = String(Q_RANGE.min);
        qSlider.max = String(Q_RANGE.max);
        qSlider.step = "0.1";
        qSlider.setAttribute("data-curve-q", "1");
        qSlider.setAttribute("aria-label", getT()["curve.qLabel"] || "Q");
        qSlider.addEventListener("input", () => {
            const v = Number(qSlider.value);
            if (!Number.isFinite(v) || local.selected < 0) return;
            const cur = points();
            if (local.selected >= cur.length) return;
            const next = updatePoint(cur, local.selected, { q: v });
            local.dragPoints = next;
            // ① 立即写 qRead(不等 syncToolbar、不等 store 回显)
            if (_toolbar && _toolbar.qRead) {
                _toolbar.qRead.textContent = qLabel(v);
            }
            syncToolbar();
            draw();
            clearTimeout(local.commitTimer);
            local.commitTimer = setTimeout(() => {
                commit(next);
            }, 140);
        });
        const qRead = document.createElement("span");
        qRead.className = "curve-toolbar__q-read sc-num";
        qRead.setAttribute("data-curve-q-read", "1");
        qWrap.append(qLabelEl, qSlider, qRead);
        bar.appendChild(qWrap);

        const del = document.createElement("button");
        del.type = "button";
        del.className = "curve-toolbar__delete";
        del.setAttribute("aria-label", getT()["curve.deleteLabel"] || "Delete");
        del.innerHTML =
            '<svg width="12" height="12" viewBox="0 0 14 14" fill="none" stroke="rgba(216,140,140,.9)" stroke-width="1.6" stroke-linecap="round" aria-hidden="true"><path d="M3 4h8M5.5 4V2.8h3V4M4.4 4l.5 7.2h4.2L9.6 4"></path></svg>';
        del.addEventListener("click", () => {
            if (local.selected >= 0) deleteAt(local.selected);
        });
        bar.appendChild(del);

        card.appendChild(bar);
        _toolbar = {
            bar,
            shapeGroup,
            sideGroup,
            slopeGroup,
            sideInfo,
            qWrap,
            qSlider,
            qRead,
        };
        return _toolbar;
    }

    function setShape(s) {
        if (!SHAPES.includes(s) || local.selected < 0) return;
        const cur = points();
        const idx = local.selected;
        const pt = cur[idx];
        const patch = { shape: s };
        // shape 切换时回填默认档:→cut 用默认斜率 12;→bell/shelf 用默认 Q 1
        if (s === "cut" && !SLOPES.includes(Math.round(pt.q)))
            patch.q = DEFAULT_SLOPE;
        else if (s !== "cut" && (pt.q < Q_RANGE.min || pt.q > Q_RANGE.max))
            patch.q = DEFAULT_Q;
        const next = updatePoint(cur, idx, patch);
        // shape→shelf/cut 且 side=out 且 |angle|<5 → 自动改选方向 + 浮条
        if (needsSideHint(next[idx])) {
            next[idx] = { ...next[idx], side: resolveSide(next[idx]) };
            showHint(getT()["curve.centerSide"] || "curve.centerSide");
        }
        commit(next);
        syncToolbar();
        draw();
        announce(announcePoint(idx, next[idx], getT()));
    }

    function setSide(s) {
        if (!SIDES.includes(s) || local.selected < 0) return;
        const cur = points();
        const idx = local.selected;
        const next = updatePoint(cur, idx, { side: s });
        commit(next);
        syncToolbar();
        draw();
        announce(announcePoint(idx, next[idx], getT()));
    }

    function setSlope(s) {
        if (!SLOPES.includes(s) || local.selected < 0) return;
        const cur = points();
        const idx = local.selected;
        const next = updatePoint(cur, idx, { q: s });
        commit(next);
        syncToolbar();
        draw();
        announce(announcePoint(idx, next[idx], getT()));
    }

    function syncToolbar() {
        const tb = _toolbar || buildToolbar();
        if (!tb) return;
        const idx = local.selected;
        tb.bar.hidden = idx < 0;
        if (idx < 0) return;
        const cur = local.dragPoints || points();
        const pt = cur[idx];
        if (!pt) return;
        for (const b of tb.bar.querySelectorAll("[data-curve-shape]")) {
            const on = b.getAttribute("data-curve-shape") === pt.shape;
            b.setAttribute("data-active", on ? "1" : "0");
            b.setAttribute("aria-pressed", on ? "true" : "false");
        }
        const directed = pt.shape === "shelf" || pt.shape === "cut";
        tb.sideGroup.hidden = !directed;
        for (const b of tb.bar.querySelectorAll("[data-curve-side]")) {
            const on = b.getAttribute("data-curve-side") === pt.side;
            b.setAttribute("data-active", on ? "1" : "0");
            b.setAttribute("aria-pressed", on ? "true" : "false");
        }
        // cut → 斜率分段钮;bell/shelf → Q 滑杆(二者互斥)
        const isCut = pt.shape === "cut";
        if (tb.slopeGroup) tb.slopeGroup.hidden = !isCut;
        if (tb.qWrap) tb.qWrap.hidden = isCut;
        if (isCut) {
            for (const b of tb.bar.querySelectorAll("[data-curve-slope]")) {
                const on =
                    Number(b.getAttribute("data-curve-slope")) ===
                    Math.round(pt.q);
                b.setAttribute("data-active", on ? "1" : "0");
                b.setAttribute("aria-pressed", on ? "true" : "false");
            }
        } else {
            tb.qSlider.value = String(Math.round(pt.q * 10) / 10);
            tb.qRead.textContent = qLabel(pt.q);
        }
        // 定位:工具条沿选中点附近,夹在窗内(设计稿 L508 深色玻璃浮条)
        const xPct = ((angleToX(pt.angle) - 150) / PLOT_W) * 100;
        tb.bar.style.left = Math.max(2, Math.min(78, xPct)).toFixed(1) + "%";
    }

    // ---- 挂载 -------------------------------------------------------------
    function mount() {
        if (svgLayer) svgLayer.style.display = "none";
        canvas.removeAttribute("aria-hidden");
        canvas.setAttribute("role", "application");
        canvas.setAttribute("tabindex", "0");
        canvas.setAttribute(
            "aria-label",
            getT()["curve.canvasLabel"] || "curve.canvasLabel",
        );
        canvas.addEventListener("pointerdown", onPointerDown);
        canvas.addEventListener("pointermove", onPointerMove);
        canvas.addEventListener("pointerup", onPointerUp);
        canvas.addEventListener("pointercancel", onPointerUp);
        canvas.addEventListener("dblclick", (e) => {
            const i = hitTest(e);
            if (i >= 0) deleteAt(i);
            else addAt(logicalFromEvent(e));
        });
        canvas.addEventListener("wheel", onWheel, { passive: false });
        canvas.addEventListener("keydown", onKeyDown);
        if (typeof window !== "undefined") {
            window.addEventListener("keydown", trackShiftDown);
            window.addEventListener("keyup", trackShiftUp);
        }
        if (typeof ResizeObserver !== "undefined" && card) {
            new ResizeObserver(resize).observe(card);
        }
        resize();
        buildToolbar();
        syncToolbar();
        draw();
    }

    function trackShiftDown(e) {
        if (e.key === "Shift") local.shift = true;
    }
    function trackShiftUp(e) {
        if (e.key === "Shift") local.shift = false;
    }

    // ---- 对外 render(app.js 每次 render() 时调;读 store 后重绘)-------------
    function render() {
        const cur = points();
        if (local.selected >= cur.length) {
            local.selected = -1;
            syncToolbar();
        }
        draw();
        if (_toolbar) {
            for (const b of _toolbar.bar.querySelectorAll(
                "[data-curve-shape]",
            )) {
                const s = b.getAttribute("data-curve-shape");
                b.textContent = getT()["curve.shape." + s] || s;
            }
            for (const b of _toolbar.bar.querySelectorAll(
                "[data-curve-side]",
            )) {
                const s = b.getAttribute("data-curve-side");
                b.textContent = getT()["curve.side." + s] || s;
            }
            const ql = _toolbar.bar.querySelector(".curve-toolbar__q-label");
            if (ql) ql.textContent = getT()["curve.qLabel"] || "Q";
            const sl = _toolbar.bar.querySelector(
                ".curve-toolbar__slope-label",
            );
            if (sl) sl.textContent = getT()["curve.slopeLabel"] || "slope";
            for (const b of _toolbar.bar.querySelectorAll(
                "[data-curve-slope]",
            )) {
                const s = b.getAttribute("data-curve-slope");
                b.textContent = getT()["curve.slope.opt" + s] || s + " dB/oct";
            }
            const si = _toolbar.bar.querySelector("[data-curve-side-info]");
            if (si) {
                const tip = getT()["curve.sideTooltip"] || "";
                si.title = tip;
                si.setAttribute("aria-label", tip);
            }
            const del = _toolbar.bar.querySelector(".curve-toolbar__delete");
            if (del)
                del.setAttribute(
                    "aria-label",
                    getT()["curve.deleteLabel"] || "Delete",
                );
        }
    }

    return { mount, render, draw, push() {} };
}
