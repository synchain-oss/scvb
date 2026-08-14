// SPDX-License-Identifier: GPL-3.0-or-later
// bridge.js —— S3 spike 前端桥抽象(05 §1.4 API 面 + mock 数据)。
// 统一 createBridge(),两种后端自动切换:
//   • JuceBackend —— 插件内运行(检测到 window.__JUCE__),走 JUCE 原生集成(getNativeFunction/addEventListener)。
//   • MockBackend —— 纯浏览器预览 / headless 基准,内置 15 轨 × 5 分钟 mock 数据(见 mock-data.js)。
// 函数名/事件名严格对应 05 §1.4(事件统一 scvb.* 前缀)。spike 内 mock 后端逐一实现假数据版。
import { makeMockData } from "./mock-data.js";

// Output 事件名(05 §1.4;频率为真实实现目标,mock 用 timer 近似)。
export const EVT = {
    STATE: "scvb.state",
    PARAMS: "scvb.params",
    CONN: "scvb.conn",
    GROUPS: "scvb.groups",
    METERS: "scvb.meters",
    PLAYHEAD: "scvb.playhead",
    CAPTURE_PROGRESS: "scvb.captureProgress",
    SEGMENTS: "scvb.segments",
    ERROR: "scvb.error",
};

// Output 原生函数名(05 §1.4,UI → C++)。
export const FN = {
    requestInitialState: "requestInitialState",
    setCaptureEnabled: "setCaptureEnabled",
    setOutputEnabled: "setOutputEnabled",
    setGroupId: "setGroupId",
    previewAnalyze: "previewAnalyze",
    analyze: "analyze",
    cancelAnalyze: "cancelAnalyze",
    setRange: "setRange",
    setVersionActive: "setVersionActive",
    setVersionName: "setVersionName",
    copyVersion: "copyVersion",
    beginParamGesture: "beginParamGesture",
    setParam: "setParam",
    endParamGesture: "endParamGesture",
    setChannelConfig: "setChannelConfig",
    setTrackManual: "setTrackManual",
    setPanCurve: "setPanCurve",
    setVadParams: "setVadParams",
    setSegmentation: "setSegmentation",
    setTransitionRamp: "setTransitionRamp",
    editSegment: "editSegment",
    recaptureArm: "recaptureArm",
    clearCoverage: "clearCoverage",
    undo: "undo",
    redo: "redo",
    requestWaveform: "requestWaveform",
    setUiScale: "setUiScale",
    commitUiScale: "commitUiScale",
    setLang: "setLang",
    setActiveTab: "setActiveTab",
    setGuideSeen: "setGuideSeen",
    setTourSeen: "setTourSeen",
};

const DESIGN = { w: 1180, h: 780 };
const SCALE_PRESETS = [0.5, 0.65, 0.8, 1, 1.25, 1.5, 2];

function makeEmitter() {
    const map = new Map();
    return {
        on(evt, cb) {
            if (!map.has(evt)) map.set(evt, []);
            map.get(evt).push(cb);
            return () => {
                const list = map.get(evt) || [];
                const i = list.indexOf(cb);
                if (i >= 0) list.splice(i, 1);
            };
        },
        emit(evt, payload) {
            (map.get(evt) || []).forEach((cb) => cb(payload));
        },
    };
}

// ---------------------------------------------------------------------------
// JuceBackend —— 插件内:经 JUCE 原生集成转发函数与事件。
// 原生函数调用 = 最小内联 getNativeFunction(emit __juce__invoke / 监听 __juce__complete),
// 不依赖 JUCE 官方 js helper(spike 自足;正式版 T26 再换 web/js/juce/index.js)。
// ---------------------------------------------------------------------------
function makeJuceBackend(init) {
    const em = makeEmitter();
    const backend = window.__JUCE__.backend;
    Object.values(EVT).forEach((id) =>
        backend.addEventListener(id, (payload) => em.emit(id, payload)),
    );

    // 最小 getNativeFunction(与 JUCE 官方 helper 同契约:emit __juce__invoke / 等 __juce__complete)。
    let nextId = 0;
    const pending = new Map();
    backend.addEventListener("__juce__complete", ({ promiseId, result }) => {
        if (pending.has(promiseId)) {
            pending.get(promiseId)(result);
            pending.delete(promiseId);
        }
    });
    const nf =
        (name) =>
        async (...args) =>
            new Promise((resolve) => {
                const id = nextId++;
                pending.set(id, resolve);
                backend.emitEvent("__juce__invoke", {
                    name,
                    params: args,
                    resultId: id,
                });
            });
    const passthrough =
        (name) =>
        async (...args) =>
            nf(name)(...args);

    return {
        isPreview: false,
        init,
        on: em.on,
        requestInitialState: passthrough(FN.requestInitialState),
        setCaptureEnabled: (v) => passthrough(FN.setCaptureEnabled)(v),
        setOutputEnabled: (v) => passthrough(FN.setOutputEnabled)(v),
        setGroupId: (g) => passthrough(FN.setGroupId)(g),
        previewAnalyze: (s) => passthrough(FN.previewAnalyze)(s),
        analyze: (s, o) => passthrough(FN.analyze)(s, o),
        cancelAnalyze: () => passthrough(FN.cancelAnalyze)(),
        setRange: (m, a, b) => passthrough(FN.setRange)(m, a, b),
        setVersionActive: (v) => passthrough(FN.setVersionActive)(v),
        setVersionName: (v, n) => passthrough(FN.setVersionName)(v, n),
        copyVersion: (a, b) => passthrough(FN.copyVersion)(a, b),
        beginParamGesture: (id) => passthrough(FN.beginParamGesture)(id),
        setParam: (id, v) => passthrough(FN.setParam)(id, v),
        endParamGesture: (id) => passthrough(FN.endParamGesture)(id),
        setChannelConfig: (ch, p) => passthrough(FN.setChannelConfig)(ch, p),
        setTrackManual: (ch, k, v) => passthrough(FN.setTrackManual)(ch, k, v),
        setPanCurve: (pts) => passthrough(FN.setPanCurve)(pts),
        setVadParams: (p) => passthrough(FN.setVadParams)(p),
        setSegmentation: (p) => passthrough(FN.setSegmentation)(p),
        setTransitionRamp: (ms) => passthrough(FN.setTransitionRamp)(ms),
        editSegment: (ch, op, p) => passthrough(FN.editSegment)(ch, op, p),
        recaptureArm: (m, a, b, auto) =>
            passthrough(FN.recaptureArm)(m, a, b, auto),
        clearCoverage: (m, a, b) => passthrough(FN.clearCoverage)(m, a, b),
        undo: () => passthrough(FN.undo)(),
        redo: () => passthrough(FN.redo)(),
        requestWaveform: (ch, a, b, cols) =>
            passthrough(FN.requestWaveform)(ch, a, b, cols),
        setUiScale: (f) => passthrough(FN.setUiScale)(f),
        commitUiScale: () => passthrough(FN.commitUiScale)(),
        setLang: (c) => passthrough(FN.setLang)(c),
        setActiveTab: (t) => passthrough(FN.setActiveTab)(t),
        setGuideSeen: (s, g) => passthrough(FN.setGuideSeen)(s, g),
        setTourSeen: (s, g) => passthrough(FN.setTourSeen)(s, g),
    };
}

// ---------------------------------------------------------------------------
// MockBackend —— 纯浏览器 / headless:内置 15 轨 × 5 分钟 mock 数据(零网络)。
// ---------------------------------------------------------------------------
function makeMockBackend(init) {
    const em = makeEmitter();
    const mock = makeMockData();
    let timers = [];

    const state = mock.state;

    function emitNow(evt, payload) {
        em.emit(evt, payload);
    }

    return {
        isPreview: true,
        init,
        on: em.on,
        // 启动模拟事件流(25Hz params / 30Hz meters+playhead / 4Hz conn / 1Hz groups),返回停止函数。
        start() {
            timers.forEach(clearInterval);
            timers = [];
            timers.push(
                setInterval(
                    () => emitNow(EVT.PARAMS, mock.paramsSnapshot()),
                    40,
                ),
            );
            timers.push(
                setInterval(
                    () => emitNow(EVT.METERS, mock.metersSnapshot()),
                    33,
                ),
            );
            timers.push(
                setInterval(
                    () => emitNow(EVT.PLAYHEAD, mock.playheadSnapshot()),
                    33,
                ),
            );
            timers.push(
                setInterval(() => emitNow(EVT.CONN, mock.connSnapshot()), 250),
            );
            timers.push(
                setInterval(
                    () => emitNow(EVT.GROUPS, mock.groupsSnapshot()),
                    1000,
                ),
            );
            return () => timers.forEach(clearInterval);
        },
        async requestInitialState() {
            return mock.initialSnapshot();
        },
        async setCaptureEnabled(on) {
            state.global.capture_enabled = !!on;
            emitNow(EVT.STATE, mock.stateSnapshot());
            return { ok: true };
        },
        async setOutputEnabled(on) {
            state.global.output_enabled = !!on;
            emitNow(EVT.STATE, mock.stateSnapshot());
            return { ok: true };
        },
        async setGroupId(g) {
            state.group_id = g;
            emitNow(EVT.STATE, mock.stateSnapshot());
            return { ok: true };
        },
        async previewAnalyze() {
            return { intervals: 40, tracks: 15, manualKept: 6 };
        },
        async analyze() {
            return {
                ok: true,
                affected: { intervals: 40, tracks: 15, manualKept: 6 },
            };
        },
        async cancelAnalyze() {
            return { ok: true };
        },
        async setRange(mode, startS, endS) {
            state.global.range = { mode, startS, endS };
            return { ok: true };
        },
        async setVersionActive(v) {
            state.version_active = v;
            emitNow(EVT.STATE, mock.stateSnapshot());
            return { ok: true };
        },
        async setVersionName(v, name) {
            state.versions[v - 1].name = String(name).slice(0, 16) || "V" + v;
            return { ok: true };
        },
        async copyVersion(src, dst) {
            return { ok: true };
        },
        async beginParamGesture(id) {
            return { ok: true };
        },
        async setParam(id, value) {
            state.params[id] = value;
            emitNow(EVT.PARAMS, mock.paramsSnapshot());
            return { ok: true };
        },
        async endParamGesture(id) {
            return { ok: true };
        },
        async setChannelConfig(ch, patch) {
            Object.assign(state.channels[ch - 1], patch);
            emitNow(EVT.STATE, mock.stateSnapshot());
            return { ok: true };
        },
        async setTrackManual(ch, panOrVol, value) {
            return { ok: true };
        },
        async setPanCurve(points) {
            state.versions[state.version_active - 1].pan_curve = points;
            emitNow(EVT.STATE, mock.stateSnapshot());
            return { ok: true };
        },
        async setVadParams(p) {
            state.analysis.vad = p;
            return { ok: true };
        },
        async setSegmentation(p) {
            state.analysis.segmentation = p;
            return { ok: true };
        },
        async setTransitionRamp(ms) {
            state.analysis.transition_ramp_ms = ms;
            return { ok: true };
        },
        async editSegment(ch, op, payload) {
            return { ok: true };
        },
        async recaptureArm(mask, startS, endS, autoStop) {
            return { armed: true, tracksMask: mask, startS, endS };
        },
        async clearCoverage() {
            return { ok: true };
        },
        async undo() {
            return { ok: true };
        },
        async redo() {
            return { ok: true };
        },
        async requestWaveform(ch, startS, endS, cols) {
            return mock.waveform(ch, startS, endS, cols);
        },
        async setUiScale(f) {
            return { ok: true, scale: Number(f) || 1 };
        },
        async commitUiScale() {
            return { ok: true };
        },
        async setLang(code) {
            state.ui.lang = code;
            return { ok: true };
        },
        async setActiveTab(tab) {
            state.ui.active_tab = tab;
            return { ok: true };
        },
        async setGuideSeen(seen, alsoGlobal) {
            state.ui.guide_seen = !!seen;
            return { ok: true };
        },
        async setTourSeen(seen, alsoGlobal) {
            state.ui.tour_seen = !!seen;
            return { ok: true };
        },
    };
}

// ---------------------------------------------------------------------------
// 工厂 —— UI 只调这一个
// ---------------------------------------------------------------------------
export function createBridge(opts = {}) {
    if (typeof window !== "undefined" && window.__JUCE__) {
        return makeJuceBackend(readInit(window.__JUCE__.initialisationData));
    }
    return makeMockBackend(readInit(opts.previewInit));
}

function readInit(src) {
    src = src || {};
    return {
        version: src.version ?? "0.1.0",
        lang: src.lang ?? "zh",
        uiScale: Number(src.uiScale) || 1,
    };
}

export { DESIGN, SCALE_PRESETS };
