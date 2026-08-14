// SPDX-License-Identifier: GPL-3.0-or-later
// bench.js —— S3 spike 自带测量(帧时分布 / 静态层重绘 / 内存 / 缩放锐度 / 布局溢出)。
// ?bench=1 时自动跑一轮脚本化交互并产出 JSON(写 window.__benchResult + console.log("BENCH_RESULT ...")),
// 供 headless Edge(CDP)与 DevTools 抽取。判据阈值以 07 T04 验收 1-7 为准。

const LONG_FRAME_MS = 32; // 验收①:交互期间无 >32ms 长帧
const DROP_FRAME_MS = 20; // 05 §6.1 降级触发:连续 >20ms

// 滚动平均帧采样器。start() 起 rAF,stop() 停并返回统计。
export class FrameSampler {
    constructor() {
        this.samples = [];
        this._raf = 0;
        this._last = 0;
    }
    start() {
        this.samples.length = 0;
        this._last = performance.now();
        const loop = (t) => {
            const d = t - this._last;
            this._last = t;
            if (d > 0) this.samples.push(d);
            this._raf = requestAnimationFrame(loop);
        };
        this._raf = requestAnimationFrame(loop);
    }
    stop() {
        cancelAnimationFrame(this._raf);
        return this.stats();
    }
    stats() {
        const s = this.samples;
        if (s.length === 0) return null;
        const sorted = s.slice().sort((a, b) => a - b);
        const sum = s.reduce((a, b) => a + b, 0);
        const q = (p) =>
            sorted[
                Math.min(
                    sorted.length - 1,
                    Math.round((p / 100) * (sorted.length - 1)),
                )
            ];
        const longFrames = s.filter((d) => d > LONG_FRAME_MS).length;
        const dropFrames = s.filter((d) => d > DROP_FRAME_MS).length;
        return {
            frames: s.length,
            avgMs: sum / s.length,
            p50Ms: q(50),
            p95Ms: q(95),
            p99Ms: q(99),
            maxMs: sorted[sorted.length - 1],
            fps: 1000 / (sum / s.length),
            longFramesOver32ms: longFrames,
            dropFramesOver20ms: dropFrames,
            longFrameRatePct: (100 * longFrames) / s.length,
        };
    }
}

// 单帧静态层重绘计时:fn() 同步重绘一次;跑 N 次取 min/avg/max。
export function measureStaticRedraw(fn, n = 30) {
    const times = [];
    for (let i = 0; i < n; i++) {
        const t0 = performance.now();
        fn();
        const t1 = performance.now();
        times.push(t1 - t0);
    }
    times.sort((a, b) => a - b);
    return {
        runs: n,
        minMs: times[0],
        maxMs: times[times.length - 1],
        avgMs: times.reduce((a, b) => a + b, 0) / n,
        p95Ms: times[
            Math.min(times.length - 1, Math.round(0.95 * (times.length - 1)))
        ],
    };
}

export function measureMemory() {
    if (!window.performance || !window.performance.memory) return null;
    const m = window.performance.memory;
    return {
        usedJSHeapMB: +(m.usedJSHeapSize / 1048576).toFixed(2),
        totalJSHeapMB: +(m.totalJSHeapSize / 1048576).toFixed(2),
    };
}

// 缩放锐度自检(05 §1.2 / §6.1):后备分辨率 == cssW×k && transform == k。
export function checkSharpness(canvas, cssW, cssH, k) {
    const expectedW = Math.max(1, Math.round(cssW * k));
    const expectedH = Math.max(1, Math.round(cssH * k));
    const ok = canvas.width === expectedW && canvas.height === expectedH;
    return {
        ok,
        zoom: k / (window.devicePixelRatio || 1),
        dpr: window.devicePixelRatio || 1,
        k,
        canvasW: canvas.width,
        canvasH: canvas.height,
        expectedW,
        expectedH,
    };
}

// 布局溢出自检(验收⑥):无横向溢出(scrollWidth<=clientWidth)且无横向控件裁切。
// 垂直滚动是预期(15 泳道/15 轨行),不计为裁切;只判 left/right 越界。
export function checkLayoutOverflow(card, controls) {
    const cr = card.getBoundingClientRect();
    let clipped = 0;
    const cw = card.clientWidth;
    const overflowX = card.scrollWidth > cw + 1;
    for (const el of controls) {
        const r = el.getBoundingClientRect();
        if (
            r.width > 0 &&
            (r.left < cr.left - 0.5 || r.right > cr.right + 0.5)
        ) {
            clipped++;
        }
    }
    return {
        overflowX,
        clipped,
        cardClientW: cw,
        cardScrollW: card.scrollWidth,
    };
}

// 内存单调增长判据:线性回归斜率(最后 40% 采样 vs 首 40% 采样均值差)。
export function memoryMonotonicGrowth(samples) {
    if (samples.length < 4)
        return { growing: false, slopeMBPerMin: 0, deltaMB: 0 };
    const n = samples.length;
    const head = samples.slice(0, Math.ceil(n / 3));
    const tail = samples.slice(-Math.ceil(n / 3));
    const hm = head.reduce((a, b) => a + b.usedJSHeapMB, 0) / head.length;
    const tm = tail.reduce((a, b) => a + b.usedJSHeapMB, 0) / tail.length;
    return {
        growing: tm - hm > 4,
        slopeMBPerMin: +(tm - hm).toFixed(2),
        deltaMB: +(tm - hm).toFixed(2),
    };
}

// 睡眠(异步)
export function sleep(ms) {
    return new Promise((r) => setTimeout(r, ms));
}
