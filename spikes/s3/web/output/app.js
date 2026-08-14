// SPDX-License-Identifier: GPL-3.0-or-later
// app.js —— S3 spike 页面主逻辑(状态机 / 控件渲染 / 曲线与波形交互 / 缩放 / 基准编排)。
// 设计盒 1180×780(05 §1.2 回流①);缩放走「固定设计盒 + CSS zoom + 10 秒确认防呆」。
// 所有 canvas 后备分辨率 = cssW×k(k=uiScale×dpr),绘制用设计 px(canvas.js)。

import { createBridge, DESIGN, SCALE_PRESETS, EVT } from "./bridge.js";
import { TRACKS, DURATION_S } from "./mock-data.js";
import * as C from "./canvas.js";
import {
    FrameSampler,
    measureStaticRedraw,
    measureMemory,
    checkSharpness,
    checkLayoutOverflow,
    memoryMonotonicGrowth,
    sleep,
} from "./bench.js";

const state = {
    uiScale: 1,
    lang: "zh",
    version: "0.1.0",
    snap: null,
    viewStart: 0,
    viewEnd: DURATION_S,
    playheadS: 0,
};
let bridge;
let els = {};
let waveLanes = [];
let curvePoints = [];
let dragCurveIndex = -1;
let metersBuf = null;

async function boot() {
    bridge = createBridge({
        previewInit: { version: "0.1.0", lang: "zh", uiScale: 1 },
    });
    state.uiScale = bridge.init.uiScale || 1;
    cacheEls();
    const snap = await bridge.requestInitialState();
    state.snap = snap;
    curvePoints = snap.versions[snap.version_active - 1].pan_curve;
    buildGlobalControls(snap);
    buildTracks(snap);
    buildWave();
    buildCurve();
    applyScale();
    wireInteractions();
    if (bridge.start) bridge.start();
    bridge.on(EVT.METERS, onMeters);
    bridge.on(EVT.PLAYHEAD, onPlayhead);
    bridge.on(EVT.PARAMS, onParams);
    drawStatic();
    drawDynamic();
    renderMeters();
    updateHud();
    if (new URLSearchParams(location.search).has("bench")) {
        runBench();
    }
}

function cacheEls() {
    const id = (n) => document.getElementById(n);
    els = {
        card: id("card"),
        root: id("vst-root"),
        tracksBody: id("tracks-body"),
        global: id("global"),
        curve: id("curve-canvas"),
        waveStatic: id("wave-static"),
        waveDynamic: id("wave-dynamic"),
        meters: id("meters-canvas"),
        hud: id("hud"),
        scale: id("scale"),
        confirm: id("scale-confirm"),
        confirmCountdown: id("scale-confirm-countdown"),
        version: id("version"),
    };
}

// ---------------------------------------------------------------------------
// 全局控件(Tab1 风格:width / ms_balance / lead_select / transition / group / version)
// ---------------------------------------------------------------------------
function buildGlobalControls(snap) {
    const g = snap.global;
    const items = [
        mkRange("width", "Width", 0, 150, g.width, "width"),
        mkRange(
            "ms_balance",
            "MS Balance",
            -100,
            100,
            g.ms_balance,
            "ms_balance",
        ),
        mkSelect(
            "lead_select",
            "Lead Select",
            ["遵循分析"].concat(snap.channels.map((c) => c.label)),
            g.lead_select,
            "lead_select",
        ),
        mkRange(
            "transition",
            "Transition",
            20,
            300,
            snap.analysis.transition_ramp_ms,
            "transition",
        ),
    ];
    const group = mkGroup(snap);
    els.global.replaceChildren(...items, group);
    els.version.textContent =
        "v" +
        snap.version_active +
        " / " +
        snap.versions.map((v) => v.name).join(" · ");
}

function mkRange(id, label, min, max, value, param) {
    const wrap = document.createElement("label");
    wrap.className = "ctl";
    wrap.dataset.glow = "";
    const span = document.createElement("span");
    span.className = "ctl-label";
    span.textContent = label;
    const input = document.createElement("input");
    input.type = "range";
    input.min = String(min);
    input.max = String(max);
    input.value = String(value);
    input.addEventListener("input", () => {
        if (
            param === "width" ||
            param === "ms_balance" ||
            param === "lead_select"
        )
            bridge.setParam(param, +input.value);
        else if (param === "transition") bridge.setTransitionRamp(+input.value);
    });
    wrap.append(span, input);
    return wrap;
}

function mkSelect(id, label, options, value, param) {
    const wrap = document.createElement("label");
    wrap.className = "ctl";
    const span = document.createElement("span");
    span.className = "ctl-label";
    span.textContent = label;
    const sel = document.createElement("select");
    options.forEach((o, i) => {
        const opt = document.createElement("option");
        opt.value = String(i);
        opt.textContent = o;
        sel.appendChild(opt);
    });
    sel.value = String(value);
    sel.addEventListener("change", () => bridge.setParam(param, +sel.value));
    wrap.append(span, sel);
    return wrap;
}

function mkGroup(snap) {
    const wrap = document.createElement("div");
    wrap.className = "ctl group";
    const span = document.createElement("span");
    span.className = "ctl-label";
    span.textContent = "Group";
    const pills = document.createElement("div");
    pills.className = "group-pills";
    for (let g = 1; g <= 8; g++) {
        const b = document.createElement("button");
        b.textContent = String.fromCharCode(64 + g);
        b.className = "pill" + (g === snap.group_id ? " on" : "");
        b.addEventListener("click", () => bridge.setGroupId(g));
        pills.appendChild(b);
    }
    wrap.append(span, pills);
    return wrap;
}

// ---------------------------------------------------------------------------
// 轨道表(Tab2 风格:14 列 × 15 行 + sticky 表头)
// ---------------------------------------------------------------------------
const COLUMNS = [
    ["st", 8],
    ["ch", 44],
    ["label", 150],
    ["pan", 30],
    ["width", 30],
    ["vol", 268],
    ["prio", 66],
    ["lead", 26],
    ["pair", 52],
    ["volx", 26],
    ["panx", 26],
    ["frP", 26],
    ["frV", 26],
    ["on", 26],
];

function buildTracks(snap) {
    for (const c of snap.channels) {
        const row = document.createElement("div");
        row.className = "track-row";
        row.dataset.glow = "";
        const st = cell("st");
        st.appendChild(dot(c));
        row.appendChild(st);

        const ch = cell("ch");
        ch.textContent =
            String(c.ch).padStart(2, "0") +
            (c.source_channels === 2 ? " ST" : "");
        row.appendChild(ch);

        const label = cell("label");
        label.textContent = c.label;
        row.appendChild(label);

        const pan = cell("pan");
        pan.appendChild(knob(c.pan));
        row.appendChild(pan);

        const width = cell("width");
        width.appendChild(knob(c.width));
        row.appendChild(width);

        const vol = cell("vol");
        vol.appendChild(volComposite(c));
        row.appendChild(vol);

        const prio = cell("prio");
        prio.appendChild(stepper(c.priority));
        row.appendChild(prio);

        const lead = cell("lead");
        lead.appendChild(toggle(c.lead));
        row.appendChild(lead);

        const pair = cell("pair");
        pair.textContent = c.pair_id ? "P" + c.pair_id : "—";
        row.appendChild(pair);

        const volx = cell("volx");
        volx.appendChild(toggle(c.lead_vol_exempt));
        row.appendChild(volx);

        const panx = cell("panx");
        panx.appendChild(toggle(c.participate_in_auto_pan));
        row.appendChild(panx);

        const frP = cell("frP");
        frP.appendChild(toggle((c.freeze & 1) !== 0));
        row.appendChild(frP);

        const frV = cell("frV");
        frV.appendChild(toggle((c.freeze & 2) !== 0));
        row.appendChild(frV);

        const on = cell("on");
        on.appendChild(toggle(c.enabled));
        row.appendChild(on);

        els.tracksBody.appendChild(row);
    }
}

function cell(cls) {
    const d = document.createElement("div");
    d.className = "cell " + cls;
    return d;
}
function dot(c) {
    const d = document.createElement("span");
    d.className = "dot" + (c.enabled ? " active" : "");
    return d;
}
function knob(v) {
    const b = document.createElement("button");
    b.className = "knob";
    b.textContent = String(Math.round(v));
    return b;
}
function toggle(on) {
    const b = document.createElement("button");
    b.className = "tgl" + (on ? " on" : "");
    b.textContent = on ? "●" : "○";
    b.addEventListener("click", () => {
        b.classList.toggle("on");
        b.textContent = b.classList.contains("on") ? "●" : "○";
    });
    return b;
}
function stepper(v) {
    const s = document.createElement("span");
    s.className = "stepper";
    s.textContent = String(v);
    return s;
}
function volComposite(c) {
    const wrap = document.createElement("div");
    wrap.className = "vol-composite";
    const slider = document.createElement("input");
    slider.type = "range";
    slider.min = "-24";
    slider.max = "12";
    slider.step = "0.1";
    slider.value = String(c.vol);
    const meter = document.createElement("canvas");
    meter.width = 100;
    meter.height = 10;
    meter.className = "mini-meter";
    wrap.append(slider, meter);
    return wrap;
}

// ---------------------------------------------------------------------------
// 波形(15 泳道,垂直滚动)+ 15 路电平表
// ---------------------------------------------------------------------------
function buildWave() {
    waveLanes = [];
    for (let ch = 1; ch <= TRACKS; ch++) {
        waveLanes.push({ ch, data: null, cols: 1180 });
    }
}

function fetchWave() {
    const span = state.viewEnd - state.viewStart;
    for (const lane of waveLanes) {
        lane.data = null;
    }
    // 拉取式分块:逐轨 requestWaveform(视口宽 1180 列)。
    for (const lane of waveLanes) {
        bridge
            .requestWaveform(lane.ch, state.viewStart, state.viewEnd, 1180)
            .then((d) => {
                lane.data = d;
            });
    }
    // 同步等待(改为 Promise.all,先空后填,避免首帧缺失)。
    return Promise.all(
        waveLanes.map((lane) =>
            bridge
                .requestWaveform(lane.ch, state.viewStart, state.viewEnd, 1180)
                .then((d) => {
                    lane.data = d;
                }),
        ),
    );
}

function buildCurve() {
    // 曲线画布布局尺寸在 applyScale 后的 resize 里定。
}

function drawStatic() {
    const w = els.waveStatic.clientWidth || 1100;
    const h = els.waveStatic.clientHeight || 600;
    const laneH = h / TRACKS;
    const ctx = C.setupCanvas(els.waveStatic, w, h, C.currentK(state.uiScale));
    const lanes = waveLanes.map((l) => l.data || emptyWave());
    C.renderWaveformStatic(ctx, lanes, { w, h, laneH });
}

function emptyWave() {
    const n = 1180;
    return {
        minDb: new Float32Array(n).fill(-50),
        maxDb: new Float32Array(n).fill(-6),
        vad: new Uint8Array(n),
        covered: new Uint8Array(n),
        stale: new Uint8Array(n),
    };
}

function drawDynamic() {
    const w = els.waveDynamic.clientWidth || 1100;
    const h = els.waveDynamic.clientHeight || 600;
    const laneH = h / TRACKS;
    const ctx = C.setupCanvas(els.waveDynamic, w, h, C.currentK(state.uiScale));
    // 300 个分段矩形(15 泳道 × 20 段)的边界竖线 + 播放头(动态层)。
    C.renderWaveformDynamic(ctx, fakeSegments(), {
        w,
        h,
        laneH,
        viewStart: state.viewStart,
        viewEnd: state.viewEnd,
        playheadS: state.playheadS,
    });
}

function fakeSegments() {
    const segs = [];
    const perTrack = 20;
    for (let ch = 1; ch <= TRACKS; ch++) {
        for (let s = 0; s < perTrack; s++) {
            segs.push({
                ch,
                startS: s * (DURATION_S / perTrack),
                locked: s % 5 === 0,
            });
        }
    }
    return segs;
}

function renderMeters() {
    const w = els.meters.clientWidth || 1100;
    const h = els.meters.clientHeight || 36;
    const ctx = C.setupCanvas(els.meters, w, h, C.currentK(state.uiScale));
    if (!metersBuf) {
        metersBuf = new Float32Array(TRACKS);
        metersBuf.fill(-60);
    }
    const gap = 2;
    const mw = (w - gap * (TRACKS - 1)) / TRACKS;
    for (let i = 0; i < TRACKS; i++) {
        C.renderMeter(ctx, metersBuf[i], {
            w: mw,
            h: h,
            x: i * (mw + gap),
            y: 0,
        });
    }
}

function onMeters(m) {
    for (let i = 0; i < TRACKS; i++) metersBuf[i] = m.tracks[i].db;
    renderMeters();
}

function onPlayhead(p) {
    state.playheadS = p.timeS;
    drawDynamic();
}

function onParams(p) {
    const w = document.querySelector('#global input[type="range"]');
    if (w) w.value = String(p.values.width);
}

// ---------------------------------------------------------------------------
// 交互:拖曲线点 / 拖波形边界 / 平移缩放
// ---------------------------------------------------------------------------
function wireInteractions() {
    wireCurveDrag();
    wireWaveDrag();
}

function wireCurveDrag() {
    const cv = els.curve;
    cv.addEventListener("pointerdown", (e) => {
        const rect = cv.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const y = e.clientY - rect.top;
        const opts = curveOpts();
        const idx = C.curveHitTest(curvePoints, x, y, opts);
        if (idx >= 0) {
            dragCurveIndex = idx;
            cv.setPointerCapture(e.pointerId);
        }
    });
    cv.addEventListener("pointermove", (e) => {
        if (dragCurveIndex < 0) return;
        const rect = cv.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const y = e.clientY - rect.top;
        const v = C.curveScreenToValue(x, y, curveOpts());
        curvePoints[dragCurveIndex].angle = v.angle;
        curvePoints[dragCurveIndex].gain_db = v.gain_db;
        drawCurve();
    });
    cv.addEventListener("pointerup", () => {
        if (dragCurveIndex >= 0) {
            bridge.setPanCurve(curvePoints);
            dragCurveIndex = -1;
        }
    });
}

function curveOpts() {
    const w = els.curve.clientWidth || 320;
    const h = els.curve.clientHeight || 200;
    return { w, h, gainMinDb: -12, gainMaxDb: 12, hitRadius: 12 };
}

function drawCurve() {
    const w = els.curve.clientWidth || 320;
    const h = els.curve.clientHeight || 200;
    const ctx = C.setupCanvas(els.curve, w, h, C.currentK(state.uiScale));
    C.renderCurveEditor(ctx, curvePoints, {
        w,
        h,
        gainMinDb: -12,
        gainMaxDb: 12,
    });
}

// 05 §6.1 降级②:平移/缩放先 blit 旧位图(不重绘静态层),静止 120ms 后异步重取重绘。
let staticRefreshTimer = 0;
function scheduleStaticRefresh() {
    clearTimeout(staticRefreshTimer);
    staticRefreshTimer = setTimeout(() => {
        fetchWave().then(drawStatic);
    }, 120);
}

function wireWaveDrag() {
    const cv = els.waveDynamic;
    let mode = "";
    let lastX = 0;
    cv.addEventListener("pointerdown", (e) => {
        const rect = cv.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const y = e.clientY - rect.top;
        lastX = x;
        mode = hitBoundary(x) >= 0 ? "boundary" : "pan";
        cv.setPointerCapture(e.pointerId);
    });
    cv.addEventListener("pointermove", (e) => {
        if (!mode) return;
        const rect = cv.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const dx =
            ((x - lastX) / (rect.width || 1)) *
            (state.viewEnd - state.viewStart);
        lastX = x;
        if (mode === "boundary") {
            // 拖动边界:这里仅重绘动态层(真实实现松手才 editSegment)。
            drawDynamic();
        } else if (mode === "pan") {
            state.viewStart -= dx;
            state.viewEnd -= dx;
            drawDynamic();
            scheduleStaticRefresh();
        }
    });
    cv.addEventListener("pointerup", () => {
        mode = "";
    });
    cv.addEventListener("wheel", (e) => {
        e.preventDefault();
        const rect = cv.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const frac = x / (rect.width || 1);
        const span = state.viewEnd - state.viewStart;
        const factor = e.deltaY > 0 ? 1.2 : 1 / 1.2;
        const newSpan = Math.max(5, Math.min(DURATION_S, span * factor));
        const center = state.viewStart + frac * span;
        state.viewStart = Math.max(0, center - newSpan * frac);
        state.viewEnd = Math.min(DURATION_S, state.viewStart + newSpan);
        state.viewStart = Math.max(0, state.viewEnd - newSpan);
        drawDynamic();
        scheduleStaticRefresh();
    });
}

function hitBoundary(x) {
    const w = els.waveDynamic.clientWidth || 1100;
    const span = state.viewEnd - state.viewStart;
    const segs = fakeSegments();
    for (const seg of segs) {
        const sx = ((seg.startS - state.viewStart) / span) * w;
        if (Math.abs(sx - x) < 6) return sx;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// 缩放(固定设计盒 + CSS zoom + 10 秒确认防呆)
// ---------------------------------------------------------------------------
let confirmTimer = 0;
let confirmPrev = 1;

function applyScale() {
    els.card.style.zoom = String(state.uiScale);
    els.card.style.width = DESIGN.w + "px";
    els.card.style.height = DESIGN.h + "px";
    els.scale.value = String(state.uiScale);
    drawStatic();
    drawDynamic();
    drawCurve();
}

function setScaleSilent(f) {
    state.uiScale = nearestPreset(f);
    applyScale();
}

function nearestPreset(f) {
    let best = SCALE_PRESETS[0];
    let bd = Infinity;
    for (const p of SCALE_PRESETS) {
        const d = Math.abs(p - f);
        if (d < bd) {
            bd = d;
            best = p;
        }
    }
    return best;
}

function requestScaleChange(f) {
    confirmPrev = state.uiScale;
    state.uiScale = nearestPreset(f);
    applyScale();
    bridge.setUiScale(state.uiScale);
    showConfirm();
}

function showConfirm() {
    clearTimeout(confirmTimer);
    els.confirm.style.display = "flex";
    let remaining = 10;
    els.confirmCountdown.textContent = String(remaining);
    confirmTimer = setTimeout(revertScale, 10000);
}

function keepScale() {
    clearTimeout(confirmTimer);
    els.confirm.style.display = "none";
    bridge.commitUiScale();
}

function revertScale() {
    clearTimeout(confirmTimer);
    els.confirm.style.display = "none";
    state.uiScale = confirmPrev;
    applyScale();
    bridge.setUiScale(confirmPrev);
}

// ---------------------------------------------------------------------------
// FPS HUD(spike 专用角标):rAF 实测帧率,角标显示。
// ---------------------------------------------------------------------------
let hudFps = 60;
function updateHud() {
    let last = performance.now();
    const loop = (t) => {
        const d = t - last;
        last = t;
        if (d > 0) hudFps = hudFps * 0.9 + (1000 / d) * 0.1;
        els.hud.textContent = "FPS " + hudFps.toFixed(1);
        requestAnimationFrame(loop);
    };
    requestAnimationFrame(loop);
}

// ---------------------------------------------------------------------------
// 基准编排(?bench=1):脚本化交互 + 静态层重绘 + 内存 + 锐度/溢出自检。
// ---------------------------------------------------------------------------
async function runBench() {
    await fetchWave();
    drawStatic();
    const result = {
        env: collectEnv(),
        fps: {},
        staticRedraw: null,
        memory: null,
        sharpness: [],
        layout: [],
        timer: null,
    };
    const mem = [];

    // 静态层单帧重绘(验收②:15 泳道 × 5 分钟 < 12ms)
    result.staticRedraw = measureStaticRedraw(drawStatic, 40);

    // 让 40× 静态重绘产生的 GC 压力沉降,再测交互相位,避免把一次性 GC 长帧记进交互判据。
    await sleep(500);

    // 交互相位:拖曲线点 / 拖边界 / 平移缩放(验收①)
    result.fps.dragCurve = await phase(2000, () => scriptedCurveDrag());
    result.fps.dragBoundary = await phase(2000, () => scriptedBoundaryDrag());
    result.fps.panZoom = await phase(2000, () => scriptedPanZoom());

    // 锐度自检(验收⑤):各缩放档 × dpr 下后备分辨率 == cssW×k
    for (const z of SCALE_PRESETS) {
        setScaleSilent(z);
        const w = els.waveStatic.clientWidth || 1100;
        const h = els.waveStatic.clientHeight || 600;
        const k = C.currentK(state.uiScale);
        C.setupCanvas(els.waveStatic, w, h, k);
        result.sharpness.push(checkSharpness(els.waveStatic, w, h, k));
    }

    // 布局溢出自检(验收⑥):各档无横向溢出、无控件裁切;最小档 0.5 必测
    for (const z of SCALE_PRESETS) {
        setScaleSilent(z);
        result.layout.push({
            zoom: z,
            ...checkLayoutOverflow(els.card, allControls()),
        });
    }

    // 内存(验收③ 的浏览器侧近似):持续交互采样,判单调增长
    mem.push(measureMemory());
    for (let i = 0; i < 12; i++) {
        scriptedPanZoom();
        await sleep(250);
        mem.push(measureMemory());
    }
    result.memory = {
        start: mem[0],
        end: mem[mem.length - 1],
        samples: mem,
        ...memoryMonotonicGrowth(mem),
    };

    setScaleSilent(1);
    result.ok =
        result.fps.dragCurve.maxMs <= 32 &&
        result.fps.dragBoundary.maxMs <= 32 &&
        result.fps.panZoom.maxMs <= 32 &&
        result.staticRedraw.avgMs < 12 &&
        result.layout.every((z) => !z.overflowX && z.clipped === 0) &&
        result.sharpness.every((z) => z.ok);

    window.__benchResult = result;
    console.log("BENCH_RESULT " + JSON.stringify(result));
    els.hud.textContent =
        "BENCH DONE " +
        JSON.stringify({
            ok: result.ok,
            staticMs: result.staticRedraw.avgMs,
            maxFrameMs: Math.max(
                result.fps.dragCurve.maxMs,
                result.fps.dragBoundary.maxMs,
                result.fps.panZoom.maxMs,
            ),
        });
}

function collectEnv() {
    return {
        ua: navigator.userAgent,
        dpr: window.devicePixelRatio || 1,
        design: DESIGN.w + "x" + DESIGN.h,
        zoomPresets: SCALE_PRESETS,
        hardwareConcurrency: navigator.hardwareConcurrency || null,
        memoryGB: navigator.deviceMemory || null,
    };
}

async function phase(durationMs, work) {
    const sampler = new FrameSampler();
    sampler.start();
    const t0 = performance.now();
    while (performance.now() - t0 < durationMs) {
        work();
        await new Promise((r) => requestAnimationFrame(r));
    }
    return sampler.stop();
}

function scriptedCurveDrag() {
    const opts = curveOpts();
    const t = (performance.now() / 1000) % 1;
    if (curvePoints.length) {
        const i = dragCurveIndex >= 0 ? dragCurveIndex : 0;
        curvePoints[i].angle = -100 + t * 200;
        curvePoints[i].gain_db = Math.sin(t * Math.PI * 2) * 10;
        drawCurve();
    }
}

function scriptedBoundaryDrag() {
    state.playheadS = (performance.now() / 1000) % DURATION_S;
    drawDynamic();
}

function scriptedPanZoom() {
    const span = state.viewEnd - state.viewStart;
    const factor = Math.sin(performance.now() / 400) > 0 ? 1.01 : 0.99;
    const newSpan = Math.max(5, Math.min(DURATION_S, span * factor));
    const center = (state.viewStart + state.viewEnd) / 2;
    state.viewStart = Math.max(0, center - newSpan / 2);
    state.viewEnd = Math.min(DURATION_S, state.viewStart + newSpan);
    drawDynamic();
    scheduleStaticRefresh();
}

function allControls() {
    return Array.from(
        els.card.querySelectorAll("input, select, button, canvas"),
    );
}

window.addEventListener("DOMContentLoaded", boot);
window.__SCVB_UI = {
    setZoom: setScaleSilent,
    getZoom: () => state.uiScale,
    redrawStatic: drawStatic,
    drawCurve,
    drawDynamic,
    getCard: () => els.card,
    getCurveCanvas: () => els.curve,
    getWaveStaticCanvas: () => els.waveStatic,
    allControls,
    SCALE_PRESETS,
    DESIGN,
};
