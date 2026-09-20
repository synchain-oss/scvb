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

/** LUT 点数(32769;PanCurve.h kPanCurveLutSize,cut slope 窄斜坡提密后口径)。 */
export const LUT_SIZE = 32769;

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
 * 32769 点 LUT(与 PanCurveLut::rebuild 同构)。返回 Float32Array —— C++ 侧
 * m_lut 是 std::array<float,32769>,(float)clampDb(db) 的 float32 取整与 Float32Array
 * 存储天然一致。网格 pan = −100 + 200·i/32768 是二进制有理数,float/double 均精确。
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
        // [SL-450] 这份空壳也要有 abortEdit / diag:app.js 的 runHistory() 无条件调
        // abortEdit(),画布缺席时少一个方法就是 Ctrl+Z 当场抛 TypeError。
        return {
            mount() {},
            render() {},
            draw() {},
            push() {},
            abortEdit() {
                return false;
            },
            diag: () => ({
                dragging: false,
                hasPreview: false,
                pendingVersion: 0,
                activeVersion: 0,
                curveSig: "",
                crossVersionDrops: 0,
                commits: 0,
                aborts: 0,
            }),
        };
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
        // [SL-450] pointerdown 时记下的 pointerId,releasePointerCapture 的唯一实参
        // 来源(捕获是按 pointerId 发的)。
        dragPointerId: null,
        // [SL-450 复审轮 1] **这份在飞编辑属于哪一版**。原名 dragVersion、只在
        // pointerdown 写 —— 那是错的:在飞编辑**不只拖动一种**,滚轮与 Q 滑杆也会
        // 造出一份挂着 140ms 防抖的待提交抄本,而它们当初根本没记版本,于是
        // render() 那道闸(只看 dragging)对它们视而不见。泛化成 pendingVersion,
        // 三类写者一律在**推出抄本的那一刻**写它。
        pendingVersion: 0,
        shift: false,
        commitTimer: 0,
        hintTimer: 0,
        lutCache: { key: "", lut: null },
        // [SL-450] 只读诊断计数。页面级冒烟要断的是「那一次提交**没有发生**」——
        // 观测计数,不观测画面:曲线画成什么样还受 store 回显影响,分辨不出
        // 「没提交」与「提交了但回显还没到」。零写入口,与 __SCVB_OUTPUT__ 同口径。
        commits: 0,
        aborts: 0,
        // [SL-450 复审轮 1] 消费点守卫丢掉的跨版本提交次数。判据要能断「守卫**真的**
        // 开过火」,而不是「什么都没发生」—— 这两者在 V2 指纹上长得一模一样。
        crossVersionDrops: 0,
    };

    /**
     * 有没有**尚未落地**的本地编辑。
     *
     * ⚠ 三条缺一不可,而且 render() 的版本闸与 abortEdit() 的早退**必须共用这一个**
     * 函数 —— 两处各写一份条件的话,会出现「闸认为有在飞、早退认为没有」的错位:
     * 闸调了 abortEdit,abortEdit 当场早退什么也没做,而**两边都不会报错**。
     *   · dragging    —— 指针拖动在飞;
     *   · dragPoints  —— 有未提交的抄本(滚轮 / Q 滑杆改完、防抖还没到点);
     *   · commitTimer —— 防抖定时器还挂着。抄本可能已被 commit 的 finally 清掉,
     *                    而定时器仍拿着**闭包里的** next —— 只看前两条会漏掉它。
     */
    function hasPendingEdit() {
        return (
            local.dragging || local.dragPoints !== null || !!local.commitTimer
        );
    }

    /** 当前激活版本号(§1.17:setPanCurve 写的就是它,载荷里不带版本号)。 */
    function activeVersion() {
        const s = getStore().state || {};
        return (s.global && s.global.version_active) || 1;
    }

    function points() {
        const s = getStore().state || {};
        const version = (s.versions || [])[activeVersion() - 1];
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
    /**
     * @param {Array} next 整表
     * @param {number} srcVersion **这份点表是从哪一版的 `points()` 推出来的**。
     *   **必传,没有默认值** —— 默认值早晚有人搞错:同步路径传 `activeVersion()` 恒等,
     *   而延迟路径若被默认成「现在」,守卫当场失效且没有任何东西会红。
     *   新增调用点必须自己决定这个值,这正是要它显式的理由。
     */
    async function commit(next, srcVersion) {
        // [SL-450] 计数排在最前面:连「桥没接上」那条早退也算一次**提交尝试**。
        // 判据要的是「abortEdit 之后那一次提交压根没发起」,不是「发起了但没成功」。
        local.commits++;
        // =====================================================================
        // [SL-450 复审轮 1] **跨版本落地守卫 —— 最后一道,不是唯一一道**
        // ---------------------------------------------------------------------
        // §1.17 的 setPanCurve 写「当前激活版本」、载荷里**不带版本号**,所以一份从 V1
        // 推出来的点表落到 V2 上就是**整表覆盖 V2**(丢的是另一个版本的整条曲线)。
        //
        // ⚠ 闸放在**消费点**而不是各个**触发点**,这是本轮改的:第一版把闸放在
        // `render()` 里(理由是「一个钩子盖住全部触发路径」),结果它只盖住了
        // `local.dragging` 那一条 —— 而 `dragPoints` 有**三类**能跨版本在飞的写者
        // (拖动 / 滚轮 140ms 防抖 / Q 滑杆 140ms 防抖)。按触发点补,补一处漏一处;
        // 而 `commit()` 是这份抄本**唯一**落地的地方(全仓 `bridge.setPanCurve` 只此
        // 一处调用),在这里核一次,**所有写者按构造全被覆盖**,不必各补一道。
        //
        // 同步路径(addAt / deleteAt / onKeyDown / setShape / setSide / setSlope)推表
        // 与提交在**同一个 tick**,版本不可能中途变 —— 这道闸对它们恒真,零行为改变。
        //
        // ⚠⚠ **这道守卫今天没有确定性可达的测试输入,而它挡不住的那一档也真实存在** ——
        // 两句都要说清楚,别让后人以为它是主力:
        //   · **挡不住哪一档**:引擎切版本是**同步**的,而 `scvb.state` 回声**异步**。
        //     在「切换已发出、回声还没到」这一窗口里,UI 的 `activeVersion()` 仍是旧值,
        //     本守卫比出来相等、照样放行,而引擎已经在新版本上 —— 这一发就落错了版本。
        //     **任何基于 UI 版本号的判据在这一窗口里都失灵**,补第三处触发点也没用。
        //     实测证据:滚轮 / Q 滑杆两臂曾在此处把 V1 的值写进 V2,而本计数器为 0。
        //     根因在契约:§1.17 的 `setPanCurve` **不带版本号**,UI 没有办法指定目标版本。
        //     已记为已知负债,留待 [SL-447](issue #272)改 §1.17 时一并解决。
        //   · **为什么还留着**:真正关死那两路的是 `switchVersion()` 的发前中止(本地)
        //     与 `render()` 的回声中止(远端)—— 两者都**依赖 UI 状态与时序**。本守卫
        //     在唯一的落地点上再核一次,是它们失效时的最后一道。删掉它今天不会有用例变红
        //     (上面两道会先拦住),这一点如实写在这里,而不是假装它被钉住了。
        // =====================================================================
        if (srcVersion !== activeVersion()) {
            local.crossVersionDrops++;
            // 抄本已过期(它属于别的版本),丢掉。引用守卫同下面的 finally:
            // 只清「仍是当前这批」,免得把之后新起的一批误清。
            if (local.dragPoints === next) local.dragPoints = null;
            draw();
            return;
        }
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
        commit(next, activeVersion()); // 同步路径:推表与提交同一 tick
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
        commit(removePoint(cur, i), activeVersion()); // 同步路径
        local.selected = -1;
        syncToolbar();
        draw();
    }

    // ---- 中止在飞编辑([SL-450])--------------------------------------------
    /**
     * 丢掉一切**尚未提交**的本地编辑态,让下一帧 draw()/render() 直接退回 store。
     *
     * 为什么非有不可:pointerdown 把点集抄进 `local.dragPoints`,pointerup 提交的是
     * **那份抄本**。于是「按住不放 → Ctrl+Z → 松手」会走成:undo 把上一笔编辑撤了,
     * 紧接着那一记 pointerup 又把陈旧抄本整表写回去 —— **撤销当场被抹掉**。
     * 拖动期本来就没有声音变化,所以这件事在界面上察觉不到。
     * ⚠ 这一层**只能落在 web 侧**:C++ 不知道有人正按着鼠标。
     *
     * 三件事缺一不可:
     *   ① `clearTimeout(commitTimer)` —— `dragPoints` 不只被拖动写:Q 滑杆与键盘微调
     *      也写它,并挂 140ms 防抖提交。只把 `dragPoints` 置空、不停表,那个定时器
     *      照样会拿着**闭包里捕获的** next 提交(它不读 `local.dragPoints`),
     *      于是「拨完 Q 滑杆 140ms 内按 Ctrl+Z」原样复现同一个缺陷;
     *   ② `releasePointerCapture` —— 捕获不放掉,指针事件会一直被这块 canvas 吃住;
     *   ③ `dragging=false` —— 随后那记 pointerup 由它挡住(onPointerUp 首行早退),
     *      这才是「不再提交那份陈旧抄本」的落点。
     *
     * @returns {boolean} 本次是否真的中止了一段在飞拖动(纯诊断用,生产路径不看)。
     */
    function abortEdit() {
        const wasDragging = local.dragging;
        // [SL-450 复审轮 1【建议】] 无条件调用 ⇒ 每次 undo/redo 都白跑一遍
        // syncToolbar()+draw()。没有在飞的东西就直接返回。
        // 条件走 hasPendingEdit() —— 与 render() 那道版本闸**同一个函数**,见它的注释。
        if (!hasPendingEdit()) return false;
        clearTimeout(local.commitTimer); // ①
        local.commitTimer = 0;
        if (
            local.dragPointerId !== null &&
            canvas.releasePointerCapture &&
            canvas.hasPointerCapture &&
            canvas.hasPointerCapture(local.dragPointerId)
        ) {
            // ② 捕获可能已被浏览器隐式释放(pointercancel / 元素离开文档),
            // 那时再放一次会抛 NotFoundError —— 先问 hasPointerCapture。
            canvas.releasePointerCapture(local.dragPointerId);
        }
        local.dragging = false; // ③
        local.dragIndex = -1;
        local.dragPointerId = null;
        local.pendingVersion = 0;
        local.dragPoints = null;
        if (local.selected >= points().length) local.selected = -1;
        // [SL-450 复审轮 1] 计**所有真正做了事的中止**,不只拖动那一种。
        // 原本写的是 `if (wasDragging)` —— 那会让「取消一发在飞的 140ms 防抖提交」
        // 计数为 0,而那恰恰是本轮新增的两条路径要观测的东西:判据会读到 0,
        // 看起来像「什么都没发生」,与「真的没中止」分不开。早退已在函数开头挡掉
        // 空跑,所以能走到这里就一定处理了点什么。
        local.aborts++;
        syncToolbar();
        draw();
        return wasDragging;
    }

    // ---- 拖拽 -------------------------------------------------------------
    function onPointerDown(e) {
        const i = hitTest(e);
        if (i >= 0) {
            local.dragging = true;
            local.dragIndex = i;
            local.dragPoints = points().slice();
            // [SL-450] 抄本属于**哪个版本**要一起记下来:§1.17 的 setPanCurve 写的是
            // 「当前激活版本」、载荷里不带版本号,拖到一半换了版本再提交 = 把 V1 的
            // 点集整表写进 V2。判据在 render() 里(换版本不一定由本地点击发起 ——
            // `version_active` 也会经 §2.1 `scvb.state` 推过来)。
            local.dragPointerId = e.pointerId;
            local.pendingVersion = activeVersion();
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
        // [SL-450] 这一行同时是「在飞拖动已被 abortEdit() 中止」的落点:中止后
        // `dragging` 已是 false,这记松手就不会再把陈旧抄本提交上去。
        if (!local.dragging) return;
        // [SL-450] 版本要在下面把 dragVersion 清零**之前**取走 —— 这份抄本是
        // pointerdown 那一刻从这一版推出来的,消费点的守卫要拿它去核。
        const pendingVersionAtDown = local.pendingVersion;
        local.dragging = false;
        const idx = local.dragIndex;
        local.dragIndex = -1;
        local.dragPointerId = null;
        local.pendingVersion = 0;
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
        // [SL-450] **延迟路径**:这份抄本是 pointerdown 那一刻从 dragVersion 那一版推出来的,
        // 中间可能已经换过版本 —— 必须传**捕获时**的版本,不是现在的。
        commit(next, pendingVersionAtDown);
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
        // [SL-450] **延迟路径**:闭包已经捕获了 next,把**推表那一刻的版本**一起捕获。
        // 同时写进 local.pendingVersion —— render() 的版本闸靠它认出「这份在飞编辑
        // 属于旧版本」,回声一到就把还没开火的这一发取消掉。
        const srcVersion = activeVersion();
        local.pendingVersion = srcVersion;
        clearTimeout(local.commitTimer);
        local.commitTimer = setTimeout(() => {
            commit(next, srcVersion);
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
        commit(next, activeVersion()); // 同步路径(键盘微调:推表与提交同一 tick)
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
            // 紧凑纯数字标签;全称 "N dB/oct" 走 title/aria-label(a11y + hover 提示)
            const full = getT()["curve.slope.opt" + s] || s + " dB/oct";
            b.textContent = String(s);
            b.title = full;
            b.setAttribute("aria-label", full);
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
            // [SL-450] **延迟路径**:同滚轮 —— 连版本一起捕获进闭包 + 写 pendingVersion。
            const srcVersion = activeVersion();
            local.pendingVersion = srcVersion;
            clearTimeout(local.commitTimer);
            local.commitTimer = setTimeout(() => {
                commit(next, srcVersion);
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
        commit(next, activeVersion()); // 同步路径(工具条:推表与提交同一 tick)
        syncToolbar();
        draw();
        announce(announcePoint(idx, next[idx], getT()));
    }

    function setSide(s) {
        if (!SIDES.includes(s) || local.selected < 0) return;
        const cur = points();
        const idx = local.selected;
        const next = updatePoint(cur, idx, { side: s });
        commit(next, activeVersion()); // 同步路径(工具条:推表与提交同一 tick)
        syncToolbar();
        draw();
        announce(announcePoint(idx, next[idx], getT()));
    }

    function setSlope(s) {
        if (!SLOPES.includes(s) || local.selected < 0) return;
        const cur = points();
        const idx = local.selected;
        const next = updatePoint(cur, idx, { q: s });
        commit(next, activeVersion()); // 同步路径(工具条:推表与提交同一 tick)
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
        // =====================================================================
        // [SL-450] **远端换版本**:回声一到就取消**任何**在飞编辑。
        // ---------------------------------------------------------------------
        // 条件原本是 `local.dragging` —— 只盖住拖动那一条,两条 140ms 防抖路径
        // (滚轮 / Q 滑杆)它看不见。复审轮 1 放宽到 `hasPendingEdit()`。
        //
        // 为什么这一道非有不可(它不是 commit() 守卫的重复):
        // `version_active` 未必由本地点 chip 改 —— ARMED 轻确认、另一侧实例、以及
        // §2.1 `scvb.state` 下行推过来的增量,都从这一个口进。本地那一路已由
        // `app.js::switchVersion()` 在**发出切换之前**调 abortEdit 关死;
        // **远端那一路只能在这里、在回声到达的那一刻**关。
        //
        // ⚠ 如实记账:把条件**退回** `local.dragging` 今天**不会有任何用例变红**
        // (删除式 C8 实跑未红)。原因不是判据松,是那半边**造不出确定性输入**:
        // 两条防抖路径的 140ms 比 `scvb.state` 回声(约 250ms)先到,远端切换时
        // 它们**在闸能看见之前就已经开火了** —— 那一档属于下面 commit() 注释里写的
        // 残余(契约层,§1.17 不带版本号)。放宽仍然保留,两个理由:
        //   ① 它与 abortEdit() 的早退**必须共用同一组条件**(见 hasPendingEdit 注释),
        //      各写一份会出现「闸认为有在飞、早退认为没有」的错位;
        //   ② `dragPoints` 非空而 `commitTimer` 已清的那一窗(提交在途)也该被收掉。
        // 钉不住就写明钉不住,不假装它被用例守着。
        // =====================================================================
        if (hasPendingEdit() && local.pendingVersion !== activeVersion())
            abortEdit();
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
                const full = getT()["curve.slope.opt" + s] || s + " dB/oct";
                b.title = full;
                b.setAttribute("aria-label", full);
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

    return {
        mount,
        render,
        draw,
        push() {},
        // [SL-450] app.js 的 runHistory() 在发 undo/redo **之前**调它。
        abortEdit,
        /** 只读诊断快照(页面级冒烟用;零写入口,与 `__SCVB_OUTPUT__` 同口径)。 */
        diag: () => ({
            dragging: local.dragging,
            hasPreview: local.dragPoints !== null,
            pendingVersion: local.pendingVersion,
            activeVersion: activeVersion(),
            crossVersionDrops: local.crossVersionDrops,
            // [SL-450] **当前激活版本**点集的指纹。计数(commits)只说得出「有没有
            // 发起提交」,说不出「那一版的数据有没有被改掉」—— 而「拖到一半换版本」
            // 的真实后果是后者(V1 的抄本整表落进 V2)。两者是**不同的失效面**:
            // onPointerUp 里 `idx >= cur.length` 那道早退会在目标版本点数不足时
            // 顺手挡掉提交,于是只看计数的判据在那种夹具上**根本分辨不出**版本闸在不在。
            curveSig: points()
                .map((p) =>
                    [p.angle, p.gain_db, p.shape, p.q, p.side].join(":"),
                )
                .join("|"),
            commits: local.commits,
            aborts: local.aborts,
        }),
    };
}
