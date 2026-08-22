// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · 泳道波形渲染(分块拉取 LRU + 静态层位图;T33 Wave 1 交付)。
// -----------------------------------------------------------------------------
// 真源:
//   • 契约 §1.27 `requestWaveform(ch, startS, endS, cols)` —— **request/response,
//     一次调用恰好一次 completion,绝不进事件流**;cols 1..4096;未覆盖列
//     `covered=0` 且 `minDb=maxDb=-160` 哨兵;`valleys[]` 秒、升序(边界吸附用);
//   • 05 §6.3:拉取式分块,块粒度 = 视口宽,LRU 缓存 **8 块/轨**;拖动平移先 blit
//     旧位图,**静止 120ms** 后取新块;每轨一条离屏位图;VAD 罩、覆盖条、stale
//     条纹画在同层(边界/手柄/选区/播放头在共享动态层,归 tab-wave.js);
//   • 05 §2.3(311):kw_ms→dB 的 min/max 包络柱;未覆盖 = 空白底纹;stale =
//     琥珀斜条纹叠加(⚠ 角标是 DOM 件,Wave 2);不同 passId 底色微差;
//   • 05 §2.3(316)+ 统筹裁定 B-07:底部覆盖条 **2px + accent 薰衣草**。
//
// 本文件零 DOM 查询:画到调用方给的 2D ctx 上;取数走注入的 request 函数。
// 颜色经 palette 注入(默认值 = tokens.css Tab3 深色泳道组的字面镜像 ——
// canvas 读不到 CSS 变量,tab-wave.js 在 mount 时用 getComputedStyle 换算真值)。
// =============================================================================

/** LRU 容量:8 块/轨(05 §6.3 逐字)。 */
export const TILE_LRU_CAP = 8;

/** 视口静止多久才取新块(ms;05 §6.3「静止 120ms 后取新块」;brief §0.8)。 */
export const IDLE_REFETCH_MS = 120;

/** 契约 §1.27:cols 上限。 */
export const MAX_COLS = 4096;

/** 契约 §1.27:未覆盖列哨兵 dB。 */
export const UNCOVERED_DB = -160;

/** 包络显示地板(dB):低于此值画最细柱(特征流值域,mock 同口径 -80)。 */
export const ENV_FLOOR_DB = -80;

/**
 * 默认调色(= tokens.css 字面镜像;键名即语义)。
 * env/vad/coverage 的出处见图谱 §12,stale 配方见图谱 A-08 统筹建议。
 */
export const DEFAULT_PALETTE = Object.freeze({
    env: "rgba(196, 190, 220, 0.5)", // --wave-env
    vad: "rgba(120, 176, 142, 0.07)", // rgba(var(--sem-green), .07) 实景档
    uncovered: "rgba(255, 255, 255, 0.05)", // 空白底纹(斜纹亮线)
    stale: "rgba(212, 176, 118, 0.22)", // rgba(var(--sem-amber), .22)
    passTint: "rgba(255, 255, 255, 0.03)", // passId 偶数轮次的底色微差
    coverage: "rgba(181, 172, 201, 0.85)", // rgba(var(--acc), .85)(B-07 accent)
});

/** 覆盖条高(px;B-07 裁定 2px)。 */
export const COVERAGE_BAR_PX = 2;

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

/** 块缓存键(同一轨内唯一;起止量化到 ms,cols 原样)。 */
export function tileKey(startS, endS, cols) {
    return `${Math.round(num(startS, 0) * 1000)}:${Math.round(
        num(endS, 0) * 1000,
    )}:${Math.trunc(num(cols, 0))}`;
}

/**
 * dB → 包络半高(px)。地板(≤ -80)画 1px 最细柱,0 dB 顶到
 * `laneH/2 − 4`(上下各留 4px 呼吸,34px 泳道时 = 13px,与设计稿
 * 「半高 4..12px」带一致)。未覆盖哨兵(-160)也会落在最细柱 —— 调用方
 * 先按 covered 位裁掉,不依赖本函数分辨。
 */
export function envelopeHalfPx(db, laneH) {
    const h = Math.max(num(laneH, 34), 8);
    const max = h / 2 - 4;
    const d = Math.min(Math.max(num(db, ENV_FLOOR_DB), ENV_FLOOR_DB), 0);
    return 1 + ((d - ENV_FLOOR_DB) / -ENV_FLOOR_DB) * (max - 1);
}

/**
 * 单轨 LRU(Map 的插入序即访问序;get 时重插保鲜)。
 * 存的值可以是 tile 本体或在途 Promise(去重:同键并发只发一次请求)。
 */
export function createTileCache(cap = TILE_LRU_CAP) {
    const map = new Map();
    return {
        get(key) {
            if (!map.has(key)) return undefined;
            const v = map.get(key);
            map.delete(key);
            map.set(key, v);
            return v;
        },
        set(key, value) {
            if (map.has(key)) map.delete(key);
            map.set(key, value);
            while (map.size > cap) {
                map.delete(map.keys().next().value);
            }
        },
        delete: (key) => map.delete(key),
        clear: () => map.clear(),
        size: () => map.size,
    };
}

/**
 * 拉取源:按 (ch, 视口, cols) 取块,LRU 8 块/轨 + 在途去重。
 * @param {object} opts
 * @param {(ch:number, startS:number, endS:number, cols:number) => Promise<object>} opts.request
 *        契约 §1.27 的桥函数(tab-wave 里包一层 bridge 容错)
 * @returns {{getTile:(ch,startS,endS,cols)=>Promise<object|null>,
 *            peek:(ch,startS,endS,cols)=>object|null,
 *            invalidate:(ch?:number)=>void}}
 */
export function createWaveformSource(opts) {
    const request = (opts || {}).request;
    /** ch → LRU。 */
    const perCh = new Map();

    function cacheOf(ch) {
        if (!perCh.has(ch)) perCh.set(ch, createTileCache());
        return perCh.get(ch);
    }

    async function getTile(ch, startS, endS, cols) {
        if (typeof request !== "function") return null;
        const n = Math.min(Math.max(Math.trunc(num(cols, 0)), 1), MAX_COLS);
        if (!(num(endS, 0) > num(startS, 0))) return null;
        const cache = cacheOf(ch);
        const key = tileKey(startS, endS, n);
        const hit = cache.get(key);
        if (hit) return hit.then ? hit : hit;
        const p = Promise.resolve()
            .then(() => request(ch, startS, endS, n))
            .then((tile) => {
                // 契约 §5.5 风格的拒绝载荷({reason}/{observer})一律当无数据
                if (!tile || !Array.isArray(tile.minDb)) {
                    cache.delete(key);
                    return null;
                }
                cache.set(key, tile);
                return tile;
            })
            .catch(() => {
                cache.delete(key);
                return null;
            });
        cache.set(key, p);
        return p;
    }

    return {
        getTile,
        /** 只查缓存(在途 Promise 不算命中)——渲染帧禁止 await。 */
        peek(ch, startS, endS, cols) {
            const cache = perCh.get(ch);
            if (!cache) return null;
            const v = cache.get(tileKey(startS, endS, cols));
            return v && !v.then ? v : null;
        },
        /** 数据失效(clearCoverage / 重采集完成后调;缺 ch = 全轨)。 */
        invalidate(ch) {
            if (ch == null) perCh.clear();
            else perCh.delete(ch);
        },
    };
}

// -----------------------------------------------------------------------------
// 静态层画笔 —— 一块 tile → 一条泳道的静态位图(在 ctx 上按 CSS px 作画,
// hidpi.js 已 setTransform)。层序(图谱 §12,DOM 序即层序):
// 未覆盖底纹 → passId 微差 → VAD 罩 → 包络柱 → stale 斜条纹 → 覆盖条。
// -----------------------------------------------------------------------------

/** 逐列位数组 → [start, end) 连续段表(减少 fillRect 次数的预算意识,05 §6.3)。 */
export function runsOf(flags, predicate) {
    const runs = [];
    let s = -1;
    const n = flags ? flags.length : 0;
    for (let i = 0; i <= n; i++) {
        const on = i < n && predicate(flags[i], i);
        if (on && s < 0) s = i;
        if (!on && s >= 0) {
            runs.push([s, i]);
            s = -1;
        }
    }
    return runs;
}

/** 45° 斜条纹填充(3px 线 / 8px 周期;A-08 统筹建议配方,stale 与未覆盖共用画法)。 */
function paintStripes(ctx, x, y, w, h, color, period, lineW) {
    ctx.save();
    ctx.beginPath();
    ctx.rect(x, y, w, h);
    ctx.clip();
    ctx.strokeStyle = color;
    ctx.lineWidth = lineW;
    ctx.beginPath();
    for (let d = x - h; d < x + w + h; d += period) {
        ctx.moveTo(d, y + h);
        ctx.lineTo(d + h, y);
    }
    ctx.stroke();
    ctx.restore();
}

/**
 * 画一块静态层。tile 为契约 §1.27 形状(六个 cols 长数组);w/h = 舞台 CSS px。
 * @param {CanvasRenderingContext2D} ctx
 * @param {object} tile
 * @param {number} w
 * @param {number} h
 * @param {object} [palette] 覆盖 DEFAULT_PALETTE 的键
 */
export function paintWaveTile(ctx, tile, w, h, palette) {
    if (!ctx || !tile || !Array.isArray(tile.minDb)) return;
    const pal = { ...DEFAULT_PALETTE, ...(palette || {}) };
    const cols = tile.minDb.length;
    if (!cols || !(w > 0) || !(h > 0)) return;
    const colW = w / cols;
    const mid = h / 2;
    const x0 = (i) => i * colW;

    ctx.clearRect(0, 0, w, h);

    // ① 未覆盖 = 空白底纹(细斜纹,不是纯掏空 —— [J72a] C-03)
    for (const [a, b] of runsOf(tile.covered, (v) => !v)) {
        paintStripes(ctx, x0(a), 0, (b - a) * colW, h, pal.uncovered, 7, 1);
    }

    // ② passId 底色微差:偶数轮次整段加一层极淡白(不同采集轮次可辨即可)
    if (Array.isArray(tile.passId)) {
        ctx.fillStyle = pal.passTint;
        for (const [a, b] of runsOf(
            tile.passId,
            (v, i) => tile.covered[i] && v > 0 && v % 2 === 0,
        )) {
            ctx.fillRect(x0(a), 0, (b - a) * colW, h);
        }
    }

    // ③ VAD 绿罩(满高;阈值拖动重取重绘,05 §2.3 行 312)
    if (Array.isArray(tile.vad)) {
        ctx.fillStyle = pal.vad;
        for (const [a, b] of runsOf(
            tile.vad,
            (v, i) => tile.covered[i] && v > 0,
        )) {
            ctx.fillRect(x0(a), 0, (b - a) * colW, h);
        }
    }

    // ④ min/max 包络柱(05 §2.3 行 311):外柱 = max 半透明,内柱 = min 提亮,
    //    纵向对称于泳道中线(实景帧的居中几何,图谱 §12 ①)
    ctx.fillStyle = pal.env;
    for (let i = 0; i < cols; i++) {
        if (!tile.covered[i]) continue;
        const hi = envelopeHalfPx(tile.maxDb[i], h);
        ctx.fillRect(x0(i), mid - hi, Math.max(colW - 1, 0.5), hi * 2);
    }
    ctx.globalAlpha = 0.55;
    for (let i = 0; i < cols; i++) {
        if (!tile.covered[i]) continue;
        const lo = envelopeHalfPx(tile.minDb[i], h);
        ctx.fillRect(x0(i), mid - lo, Math.max(colW - 1, 0.5), lo * 2);
    }
    ctx.globalAlpha = 1;

    // ⑤ stale = 琥珀斜条纹叠加(05 §2.3 行 311;⚠ 角标是 DOM 件,Wave 2)
    if (Array.isArray(tile.stale)) {
        for (const [a, b] of runsOf(
            tile.stale,
            (v, i) => tile.covered[i] && v > 0,
        )) {
            paintStripes(ctx, x0(a), 0, (b - a) * colW, h, pal.stale, 8, 3);
        }
    }

    // ⑥ 底部 2px 覆盖条(已覆盖 = accent 薰衣草;B-07 裁定)
    ctx.fillStyle = pal.coverage;
    for (const [a, b] of runsOf(tile.covered, (v) => !!v)) {
        ctx.fillRect(
            x0(a),
            h - COVERAGE_BAR_PX,
            (b - a) * colW,
            COVERAGE_BAR_PX,
        );
    }
}
