// SPDX-License-Identifier: GPL-3.0-or-later
// canvas.js —— S3 spike Canvas 2D 渲染(曲线编辑器 / 波形时间线 / 电平表)。
// 缩放正确性(05 §1.2 新问题):k = uiScale × devicePixelRatio;后备分辨率 = cssW×k × cssH×k;
// ctx.setTransform(k,0,0,k,0,0) 后按设计 px 绘制 → CSS zoom 与高 DPI 下不发糊。
// 分层(05 §6.1):静态底层(网格/波形,视口变化才重绘)+ 动态层(曲线/手柄/边界/播放头,rAF)。

// 把 canvas 后备存储设为 cssW×k / cssH×k,并设 transform;返回 2d 上下文。绘制一律用设计 px。
export function setupCanvas(canvas, cssW, cssH, k) {
    const bw = Math.max(1, Math.round(cssW * k));
    const bh = Math.max(1, Math.round(cssH * k));
    if (canvas.width !== bw) canvas.width = bw;
    if (canvas.height !== bh) canvas.height = bh;
    const ctx = canvas.getContext("2d");
    ctx.setTransform(k, 0, 0, k, 0, 0);
    ctx.imageSmoothingEnabled = true;
    return ctx;
}

export function currentK(uiScale) {
    const dpr = window.devicePixelRatio || 1;
    return (uiScale || 1) * dpr;
}

// ---------------------------------------------------------------------------
// 曲线编辑器(角度域增益曲线,05 §6.2)。点:{angle,gain_db,shape,q,side}。
// ---------------------------------------------------------------------------
export function curveScreenToValue(x, y, opts) {
    const angle = -100 + (x / opts.w) * 200;
    const gainDb =
        opts.gainMaxDb - (y / opts.h) * (opts.gainMaxDb - opts.gainMinDb);
    return {
        angle: clamp(angle, -100, 100),
        gain_db: clamp(gainDb, opts.gainMinDb, opts.gainMaxDb),
    };
}

export function curveValueToScreen(pt, opts) {
    const x = ((pt.angle + 100) / 200) * opts.w;
    const y =
        ((opts.gainMaxDb - pt.gain_db) / (opts.gainMaxDb - opts.gainMinDb)) *
        opts.h;
    return { x, y };
}

export function curveHitTest(points, x, y, opts) {
    const r = Math.max(12, opts.hitRadius || 12);
    let best = -1;
    let bestD = r;
    for (let i = 0; i < points.length; i++) {
        const s = curveValueToScreen(points[i], opts);
        const d = Math.hypot(s.x - x, s.y - y);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

export function renderCurveEditor(ctx, points, opts) {
    const { w, h } = opts;
    ctx.clearRect(0, 0, w, h);

    // 网格:角度刻度 -100/-50/0/+50/+100,dB 横线(±12dB 每 6dB 一条)。
    ctx.strokeStyle = "rgba(255,255,255,0.06)";
    ctx.lineWidth = 1;
    for (let a = -100; a <= 100; a += 50) {
        const x = ((a + 100) / 200) * w;
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.lineTo(x, h);
        ctx.stroke();
    }
    for (let db = -12; db <= 12; db += 6) {
        const y = ((12 - db) / 24) * h;
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(w, y);
        ctx.stroke();
    }
    // 中线 0 dB
    const y0 = ((12 - 0) / 24) * h;
    ctx.strokeStyle = "rgba(255,255,255,0.18)";
    ctx.beginPath();
    ctx.moveTo(0, y0);
    ctx.lineTo(w, y0);
    ctx.stroke();

    // 曲线(线性插值 polyline,采样 128 段)
    ctx.strokeStyle = "#9B93C4";
    ctx.lineWidth = 2;
    ctx.beginPath();
    const samples = 128;
    for (let i = 0; i <= samples; i++) {
        const angle = -100 + (i / samples) * 200;
        const gain = interpolateCurve(points, angle);
        const s = curveValueToScreen({ angle, gain_db: gain }, opts);
        if (i === 0) ctx.moveTo(s.x, s.y);
        else ctx.lineTo(s.x, s.y);
    }
    ctx.stroke();

    // 手柄(shape 区分轮廓:圆=bell,方=shelf,三角=cut)
    for (const pt of points) {
        const s = curveValueToScreen(pt, opts);
        ctx.fillStyle = "#e8e6f2";
        ctx.strokeStyle = "#9B93C4";
        ctx.lineWidth = 2;
        ctx.beginPath();
        if (pt.shape === "shelf") ctx.rect(s.x - 5, s.y - 5, 10, 10);
        else if (pt.shape === "cut") triangle(ctx, s.x, s.y);
        else ctx.arc(s.x, s.y, 5, 0, Math.PI * 2);
        ctx.fill();
        ctx.stroke();
    }
}

function triangle(ctx, x, y) {
    ctx.moveTo(x, y - 6);
    ctx.lineTo(x + 6, y + 5);
    ctx.lineTo(x - 6, y + 5);
    ctx.closePath();
}

function interpolateCurve(points, angle) {
    if (points.length === 0) return 0;
    if (angle <= points[0].angle) return points[0].gain_db;
    for (let i = 1; i < points.length; i++) {
        if (angle <= points[i].angle) {
            const a = points[i - 1];
            const b = points[i];
            const t = (angle - a.angle) / Math.max(0.001, b.angle - a.angle);
            return a.gain_db + (b.gain_db - a.gain_db) * t;
        }
    }
    return points[points.length - 1].gain_db;
}

// ---------------------------------------------------------------------------
// 波形时间线(05 §6.3):静态底层 = 网格 + 每轨波形 min/max 列 + VAD 罩 + covered + stale。
// lanes = [{minDb:Float32Array, maxDb:Float32Array, vad:Uint8Array, covered:Uint8Array, stale:Uint8Array}]
// ---------------------------------------------------------------------------
export function renderWaveformStatic(ctx, lanes, opts) {
    const { w, h, laneH } = opts;
    ctx.clearRect(0, 0, w, h);
    const cols = lanes[0].minDb.length;

    // 每泳道网格线
    ctx.strokeStyle = "rgba(255,255,255,0.08)";
    ctx.lineWidth = 1;
    for (let l = 0; l <= lanes.length; l++) {
        const y = l * laneH;
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(w, y);
        ctx.stroke();
    }

    for (let l = 0; l < lanes.length; l++) {
        const lane = lanes[l];
        const baseY = l * laneH;
        const midY = baseY + laneH / 2;
        const amp = (laneH / 2) * 0.9;

        // covered 底条
        ctx.fillStyle = "rgba(155,147,196,0.05)";
        for (let c = 0; c < cols; c++) {
            if (lane.covered[c]) {
                const x = (c / cols) * w;
                ctx.fillRect(
                    x,
                    baseY + 1,
                    Math.ceil(w / cols) + 0.5,
                    laneH - 2,
                );
            }
        }

        // stale 竖条纹
        ctx.fillStyle = "rgba(255,170,80,0.16)";
        for (let c = 0; c < cols; c++) {
            if (lane.stale[c]) {
                const x = (c / cols) * w;
                ctx.fillRect(
                    x,
                    baseY + 1,
                    Math.ceil(w / cols) + 0.5,
                    laneH - 2,
                );
            }
        }

        // 波形 min/max 竖线
        ctx.strokeStyle = "#7f7f96";
        ctx.lineWidth = 1;
        const colW = w / cols;
        for (let c = 0; c < cols; c++) {
            const x = c * colW;
            const yMax = midY - (lane.maxDb[c] / -60) * amp;
            const yMin = midY - (lane.minDb[c] / -60) * amp;
            ctx.beginPath();
            ctx.moveTo(x, yMax);
            ctx.lineTo(x, yMin);
            ctx.stroke();
        }

        // VAD 罩(阈值以上区间,淡绿)
        ctx.fillStyle = "rgba(80,200,140,0.12)";
        for (let c = 0; c < cols; c++) {
            if (lane.vad[c]) {
                const x = c * colW;
                ctx.fillRect(x, baseY + 1, colW + 0.5, laneH - 2);
            }
        }
    }
}

// 动态层:分段边界竖线 + 选区 + 播放头。
export function renderWaveformDynamic(ctx, segments, opts) {
    const { w, h, laneH, viewStart, viewEnd, playheadS } = opts;
    ctx.clearRect(0, 0, w, h);
    const span = Math.max(0.0001, viewEnd - viewStart);

    // 分段边界竖线(每段起点;locked 段用琥珀色)
    ctx.lineWidth = 1;
    for (const seg of segments) {
        const x = ((seg.startS - viewStart) / span) * w;
        if (x < 0 || x > w) continue;
        const y = (seg.ch - 1) * laneH;
        ctx.strokeStyle = seg.locked ? "#FFAA50" : "rgba(255,255,255,0.35)";
        ctx.beginPath();
        ctx.moveTo(x, y + 2);
        ctx.lineTo(x, y + laneH - 2);
        ctx.stroke();
    }

    // 播放头
    if (playheadS != null && playheadS >= viewStart && playheadS <= viewEnd) {
        const x = ((playheadS - viewStart) / span) * w;
        ctx.strokeStyle = "#9B93C4";
        ctx.lineWidth = 1.5;
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.lineTo(x, h);
        ctx.stroke();
    }
}

// ---------------------------------------------------------------------------
// 电平表弹道(-60 dB 地板 → 0,镜像 §2.0 电平合成件)。垂直条,原点在 (x,y)。
// ---------------------------------------------------------------------------
export function renderMeter(ctx, db, opts) {
    const { w, h, x = 0, y = 0 } = opts;
    ctx.clearRect(x, y, w, h);
    const frac = clamp((db + 60) / 60, 0, 1);
    const barH = h * frac;
    ctx.fillStyle = "rgba(255,255,255,0.08)";
    ctx.fillRect(x, y, w, h);
    ctx.fillStyle = frac > 0.9 ? "#e0a0a0" : "#9B93C4";
    ctx.fillRect(x, y + h - barH, w, barH);
}

function clamp(v, lo, hi) {
    return Math.max(lo, Math.min(hi, v));
}
