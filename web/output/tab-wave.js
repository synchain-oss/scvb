// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · Tab3「波形与分段」—— 15 泳道 + 段检查器(T33 Wave 1 交付物:静态结构)。
// -----------------------------------------------------------------------------
// 职责边界(与 tab-tracks.js 同构):
//   • 本文件只管 **Tab3**(工具条 / 标尺 / 15 泳道 / 选区叠加层 / 底部缩放条 /
//     段检查器)。外壳在 web/output/app.js,app.js 只留装配调用与订阅转发。
//   • 两段导出:**纯函数**(无 DOM,node 可直接 import 断言)+ `createTabWave()`
//     (DOM 接线)。模块顶层零副作用、零 document 触碰。
//
// 视觉真源 = 设计稿 `docs/design/SCVB 设计稿.dc.html` 两处(图谱 §0,职责不可互换):
//   实景帧 805-873(15 泳道排布)+ 图例帧 706-803(泳道内部画法规格)。
// 交互语义与词条真源 = 05 §2.3 / §2.3a;桥面 = 冻结契约 §1.5/§1.6/§1.8/§1.18/
// §1.19/§1.22/§1.23/§1.24/§1.27 与事件 §2.6/§2.7/§2.8。
//
// **[J67]**:不存在波形⇄列表切换器;段查看/编辑唯一入口 = 泳道点选 + 段检查器。
// **契约禁项**:不新增拉取式段 API —— 段数据一律消费 `scvb.segments` 增量事件。
//
// 分波:
//   • **Wave 1(本文件现状)** = 静态结构:store → 泳道模型**只读投影**(轨头六件 /
//     空态 / 布防 badge / 菊花态 / 按钮判据),canvas 静态层(包络/VAD/未覆盖/stale/
//     passId/覆盖条,canvas/waveform.js)与共享动态层(曲线/边界,本文件画)。
//     **不接交互** —— 全部交互挂 [Wave 2] TODO 注释,一处一条。
//   • **Wave 2** = 滑杆两段式 / 四动作+确认框 / 选区拖拽+设为范围 / 边界拖拽吸附+
//     分割合并 / 点选+shift 连选 / 检查器编辑+锁定 / 缩放平移 / 播放头交互 /
//     布防 badge 三处 + footer 警告 / A2 七件。
// =============================================================================

// 可复用件四件套(T34/T43 复用面;各自文件头写明复用契约)
import {
    createTimeline,
    timeToX,
    spanOf,
    zoomLabel,
    scrollThumb,
} from "./canvas/timeline.js";
import { backingScale, resizeCanvas } from "./canvas/hidpi.js";
import { createLayerStack } from "./canvas/layers.js";
import { createPlayhead } from "./canvas/playhead.js";
import {
    createWaveformSource,
    paintWaveTile,
    MAX_COLS,
} from "./canvas/waveform.js";
// Tab2 已锤实的口径直接复用(状态灯五态 / 轨号零填充 / vol 行程映射 / 段表取轨)
import {
    CHANNEL_COUNT,
    trackStatusOf,
    statusVisual,
    segmentsOfCh,
    volPercent,
    labelPlaceholder,
    tt,
} from "./tab-tracks.js";
import { format } from "./tab-master.js";

// =============================================================================
// 一、纯函数与常量(无 DOM;node 侧断言面)
// =============================================================================

/** 泳道数 = 15(J59;契约 §0.2 第 4 条)。 */
export const LANE_COUNT = CHANNEL_COUNT;

/** 轨头列宽(设计稿 808/841/867 三处共用;底部空档 148 + padding 10 = 158)。 */
export const HEAD_W = 158;

/** 泳道行高 34px(实景帧 1812;[J72a] C-10:TAB_ROWS 的 44 是旧口径,不取)。 */
export const LANE_H = 34;

/** 段检查器宽(稿内 877;灰模 260 → 统一 262,登记差异)。 */
export const INSPECTOR_W = 262;

/** 右缘双刻度列宽(图例帧 786;B-11:全泳道区共用一列)。 */
export const SCALE_COL_W = 44;

/** 时间标尺行高(实景帧 807)。 */
export const RULER_H = 22;

/** 底部缩放/滚动条行高(866)。 */
export const BOTTOM_BAR_H = 20;

/** 底部条左空档(867;+ 左 padding 10 = HEAD_W)。 */
export const BOTTOM_HEAD_W = 148;

/** 无任何时长线索时的兜底工程时长(秒;mock 假数据同为 5 分钟,J59)。 */
export const FALLBACK_DURATION_S = 300;

/**
 * 7 滑杆定义(顺序不可重排 —— 05 §2.3 行 298-299 的 §1.18 五字段 + §1.19 两字段)。
 * 值域由设计稿默认值与行程比反推(p = (def-min)/(max-min) 与稿内 2070-2074 逐一相符):
 * -38dB→.44 / 6dB→.30 / 180ms→.36 / 120ms→.24(J23)/ 200ms→.40(J23)/
 * .62→.62 / 420ms→.28。`gb` = 灰模既有锚点(appendix B),`t` = 短标词条(A-19)。
 */
export const SLIDERS = Object.freeze(
    [
        // prettier-ignore
        { key: "threshold", field: "threshold_db", api: "vad", gb: "wave-vad-threshold", t: "wave.sldThreshold", min: -60, max: -10, def: -38, unit: "dB", dp: 0 },
        // prettier-ignore
        { key: "hysteresis", field: "hysteresis_db", api: "vad", gb: "wave-vad-hysteresis", t: "wave.sldHysteresis", min: 0, max: 20, def: 6, unit: "dB", dp: 0 },
        // prettier-ignore
        { key: "hangover", field: "hangover_ms", api: "vad", gb: "wave-vad-hangover", t: "wave.sldHangover", min: 0, max: 500, def: 180, unit: "ms", dp: 0 },
        // prettier-ignore
        { key: "paddingpre", field: "padding_pre_ms", api: "vad", gb: "wave-vad-paddingpre", t: "wave.sldPadPre", min: 0, max: 500, def: 120, unit: "ms", dp: 0 },
        // prettier-ignore
        { key: "paddingpost", field: "padding_post_ms", api: "vad", gb: "wave-vad-paddingpost", t: "wave.sldPadPost", min: 0, max: 500, def: 200, unit: "ms", dp: 0 },
        // prettier-ignore
        { key: "sensitivity", field: "sensitivity", api: "seg", gb: "wave-seg-sensitivity", t: "wave.sldSensitivity", min: 0, max: 1, def: 0.62, unit: "", dp: 2 },
        // prettier-ignore
        { key: "minseg", field: "min_segment_ms", api: "seg", gb: "wave-seg-minlen", t: "wave.sldMinSeg", min: 0, max: 1500, def: 420, unit: "ms", dp: 0 },
    ].map(Object.freeze),
);

/**
 * VAD 参数缓存初值(**五字段整包**下发纪律的 UI 侧底账,契约 §1.18;brief §0.4)。
 * [Wave 2] 拖任何一杆都以「当前整组缓存 + 本杆新值」整包调 setVadParams,
 * 绝不只发变动字段;state 回推后整组覆盖。
 */
export const DEFAULT_VAD_PARAMS = Object.freeze({
    threshold_db: -38,
    hysteresis_db: 6,
    hangover_ms: 180,
    padding_pre_ms: 120,
    padding_post_ms: 200,
});

/** 分段参数缓存初值(契约 §1.19:{mode, sensitivity, min_segment_ms} 整包)。 */
export const DEFAULT_SEGMENTATION = Object.freeze({
    mode: "auto",
    sensitivity: 0.62,
    min_segment_ms: 420,
});

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

/** 滑杆行程百分数(0..100;CSS 变量 --p 与 aria 同源)。 */
export function sliderPercent(def, v) {
    const x = (num(v, def.def) - def.min) / (def.max - def.min);
    return (x < 0 ? 0 : x > 1 ? 1 : x) * 100;
}

/** 滑杆读数文本(mono tabular;dp 位小数 + 单位)。 */
export function fmtSliderValue(def, v) {
    const x = num(v, def.def);
    const s = def.dp > 0 ? x.toFixed(def.dp) : String(Math.round(x));
    return def.unit ? `${s} ${def.unit}` : s;
}

/** 秒 → `mm:ss.mmm`(检查器/选区读数主显,05 §2.3a 行 331 / B-12)。 */
export function fmtTimeMs(s) {
    const t = Math.max(num(s, 0), 0);
    const m = Math.floor(t / 60);
    const sec = Math.floor(t % 60);
    const ms = Math.round((t - Math.floor(t)) * 1000);
    return `${String(m).padStart(2, "0")}:${String(sec).padStart(2, "0")}.${String(ms).padStart(3, "0")}`;
}

/**
 * 工程时长推定(秒)。契约没有独立「时长」字段 —— 取 store 里能见到的最大时间线索:
 * Range 终点 / 段表最大 t1S / 播放头。全空时回落 5 分钟(mock 假数据口径)。
 */
export function durationOf(store) {
    const st = store || {};
    let d = 0;
    const range = ((st.state || {}).global || {}).range || {};
    d = Math.max(d, num(range.end_s, 0));
    for (const c of (st.segments && st.segments.channels) || []) {
        for (const seg of (c && c.segments) || []) {
            d = Math.max(d, num(seg && seg.t1S, 0));
        }
    }
    d = Math.max(d, num(st.playhead && st.playhead.timeS, 0));
    return d > 0 ? d : FALLBACK_DURATION_S;
}

/**
 * 事件仓 → 15 条泳道模型(**Tab3 轨头/行状态的唯一渲染源**;只读投影)。
 *   label / status ← §2.1 channels + §2.3 conn(口径同 Tab2 rowsFromStore);
 *   cov ← §2.7 captureProgress(app.js 落在 store.coverage[ch]);
 *   segs / stale ← §2.8 segments;low ← §2.9 lowSample(轨级 error)。
 * `picked` 恒 0:轨选是 Wave 2 交互态,不属事件仓。
 */
export function laneModelFromStore(store) {
    const st = store || {};
    const chans = (st.state && st.state.channels) || [];
    const conn = (st.conn && st.conn.channels) || [];
    const lowErr =
        st.errors && typeof st.errors.get === "function"
            ? st.errors.get("lowSample")
            : null;
    const lanes = [];
    for (let ch = 1; ch <= LANE_COUNT; ch++) {
        const cfg = chans[ch - 1] || {};
        const segCh = segmentsOfCh(st.segments, ch);
        lanes.push({
            n: ch,
            label: typeof cfg.label === "string" ? cfg.label : "",
            status: trackStatusOf(conn[ch - 1] || null),
            cov: Math.round(num((st.coverage || {})[ch], 0)),
            segs: ((segCh && segCh.segments) || []).length,
            stale: !!(segCh && segCh.stale),
            low: lowErr && lowErr.ch === ch ? 1 : 0,
            picked: 0,
        });
    }
    return lanes;
}

/** 空态判定(05 §2.3 行 318:**全部轨**无 coverage 才空)。 */
export function isLanesEmpty(store) {
    const cov = (store || {}).coverage || {};
    for (const k of Object.keys(cov)) {
        if (num(cov[k], 0) > 0) return false;
    }
    return true;
}

/** 「重新识别」判据(05 行 302:无 origin≠auto 段时 disabled)。 */
export function hasNonAutoSegments(segments) {
    for (const c of (segments && segments.channels) || []) {
        for (const s of (c && c.segments) || []) {
            if (s && s.origin && s.origin !== "auto") return true;
        }
    }
    return false;
}

/**
 * 某轨的段边界表(泳道内竖线;05 行 310/313)。
 * 第 i 段(i≥1)的 t0S 即一条边界;两侧任一段 origin≠auto → 实线(manual),
 * 否则虚线(auto)。
 */
export function boundariesOf(segChannel) {
    const segs = (segChannel && segChannel.segments) || [];
    const out = [];
    for (let i = 1; i < segs.length; i++) {
        const a = segs[i - 1];
        const b = segs[i];
        if (!b) continue;
        out.push({
            tS: num(b.t0S, 0),
            manual:
                (a && a.origin && a.origin !== "auto") ||
                (b.origin && b.origin !== "auto")
                    ? 1
                    : 0,
        });
    }
    return out;
}

/**
 * 某轨的段角标表(E/C 薰衣草实心 chip + 锁定小标;05 行 310)。
 * auto 段无角标;锁定标独立于 origin(set_locked 不改 origin,契约 §5.4)。
 * 位置取段起点(角标画在段头,图例帧 774-779 的 @34.5%/@59.5% 即段头偏右)。
 */
export function segMarksOf(segChannel) {
    const out = [];
    for (const s of (segChannel && segChannel.segments) || []) {
        if (!s) continue;
        const tS = num(s.t0S, 0);
        if (s.origin === "user_edited") out.push({ kind: "E", tS });
        else if (s.origin === "user_created") out.push({ kind: "C", tS });
        if (s.locked) out.push({ kind: "lock", tS });
    }
    return out;
}

/** pan 值 → 泳道内 y(px;实景帧 1819:中线 12 ± 7,pan ∈ -100..100)。 */
export function panYPx(pan) {
    const p = Math.min(Math.max(num(pan, 0), -100), 100);
    return 12 + (p / 100) * 7;
}

/** volDb → 泳道内 y(px;实景帧 1821:基线 22 − 行程比 × 7;行程比同 Tab2 卡箍)。 */
export function volYPx(volDb) {
    return 22 - (volPercent(volDb) / 100) * 7;
}

/**
 * 标尺刻度(mm:ss;稿内是静态小节号,tempo 表 v1 不可得 → 取时间刻度,
 * B-12 的 mm:ss.mmm 主显口径同源)。步长取「≤10 枚刻度」的最小档。
 */
export const RULER_STEPS_S = Object.freeze([
    1, 2, 5, 10, 15, 30, 60, 120, 300, 600,
]);

export function rulerTicks(vp) {
    const span = spanOf(vp);
    if (!(span > 0)) return [];
    let step = RULER_STEPS_S[RULER_STEPS_S.length - 1];
    for (const s of RULER_STEPS_S) {
        if (span / s <= 10) {
            step = s;
            break;
        }
    }
    const out = [];
    const first = Math.ceil(vp.startS / step) * step;
    for (let t = first; t <= vp.endS + 1e-9; t += step) {
        const m = Math.floor(t / 60);
        const sec = Math.round(t % 60);
        out.push({
            tS: t,
            label: `${String(m).padStart(2, "0")}:${String(sec).padStart(2, "0")}`,
        });
    }
    return out;
}

// =============================================================================
// 二、模板(泳道 15 行;纯字符串拼装,node 可断言锚点)
// =============================================================================

/** HTML 转义(label 是用户数据,绝不拼进 innerHTML 不转义 —— 口径同 Tab2)。 */
function esc(s) {
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
 * 一条泳道(实景帧 839-861 + 轨头六件补齐口径 B-08)。
 * 状态一律 data-*(data-status / data-on),几何走 CSS;
 * 舞台两层:静态位图 canvas(waveform.js 画)+ 角标 DOM 层(段 E/C/锁定)。
 * 曲线与边界画在 .wave-lanes 级的共享动态层(§6.3),不逐泳道建 canvas。
 */
export function waveLaneHtml(ch) {
    const gb = (suffix) => `wave-lane-${ch}${suffix ? "-" + suffix : ""}`;
    return `
    <div class="wave-lane" data-gb="${gb("")}" data-ch="${ch}" data-on="0" data-status="idle">
      <div class="wave-lane__head" data-gb="${gb("head")}">
        <!-- [Wave 2] 复选框/整行点选 = 勾选该轨;shift = 以上一次点选轨为锚点连选
             (05 行 288 语义照抄稿内 pick(),不自行发明;lanePick 保持点击顺序数组) -->
        <span class="wave-lane__check" role="checkbox" aria-checked="false"
              tabindex="0" data-gb="${gb("checkbox")}">
          <svg width="8" height="8" viewBox="0 0 8 8" aria-hidden="true">
            <path d="M1.5 4.2 3.2 6 6.5 2" fill="none" stroke="#2a2438"
                  stroke-width="1.8" stroke-linecap="round"/>
          </svg>
        </span>
        <!-- 状态灯:统一 Tab2 的 8×8 .sc-dot + active livePulse(B-06 裁定) -->
        <span class="sc-dot wave-lane__light" data-tone="gray" data-pulse="0"
              data-gb="${gb("light")}"></span>
        <span class="wave-lane__label" data-gb="${gb("label")}"></span>
        <!-- 「样本不足」黄标:158px 内压成琥珀点 + tooltip(05 行 309;C-05 补件) -->
        <span class="wave-lane__lowdot" data-gb="${gb("lowsample")}" hidden></span>
        <span class="wave-lane__covseg" data-gb="${gb("covseg")}"></span>
        <!-- [Wave 2] 曲线可见 toggle(B-08:压成眼睛图标钮;防遮挡,05 行 309) -->
        <button class="wave-lane__eye" type="button" aria-pressed="true"
                data-t-aria="wave.curveVisible" data-gb="${gb("curvevisible")}">
          <svg width="14" height="14" viewBox="0 0 14 14" aria-hidden="true">
            <path d="M1.5 7C3 4.2 5 2.8 7 2.8S11 4.2 12.5 7C11 9.8 9 11.2 7 11.2S3 9.8 1.5 7Z"
                  fill="none" stroke="currentColor" stroke-width="1.4"/>
            <circle cx="7" cy="7" r="1.8" fill="none" stroke="currentColor"
                    stroke-width="1.4"/>
          </svg>
        </button>
      </div>
      <div class="wave-lane__stage" data-gb="${gb("stage")}">
        <!-- 静态层位图:包络柱 / VAD 罩 / 未覆盖底纹 / stale 斜条纹 / passId 微差 /
             2px 覆盖条(canvas/waveform.js;数据 = 契约 §1.27 requestWaveform) -->
        <canvas class="wave-lane__static" data-gb="${gb("static")}"></canvas>
        <!-- 段角标 DOM 层(E/C/锁定;render() 按 scvb.segments 重建) -->
        <div class="wave-lane__badges" data-gb="${gb("badges")}"></div>
        <!-- [Wave 2] 边界拖拽手柄(图例帧 767-772 的 9×26 两级权重)/ 双击分割 /
             相邻两段 Delete 合并 → bridge.editSegment(释放才发,§1.22/§5.4;
             吸附 requestWaveform.valleys[],Alt 关吸附,tooltip = wave.boundaryHandleTip) -->
      </div>
    </div>`;
}

// =============================================================================
// 三、DOM 接线(createTabWave)
// =============================================================================

/**
 * @param {object} opts
 * @param {Document|Element} opts.root  查询根(app.js 传 document)
 * @param {object|null} opts.bridge     createBridge() 结果 —— 上行只经它(Wave 2)
 * @param {() => object} opts.getStore  事件仓(**唯一渲染源**)
 * @param {() => object} opts.getT      当前语言字典
 * @param {() => void} [opts.onLocalChange] 本地态改变后请求外壳重渲染(Wave 2 用)
 */
export function createTabWave(opts) {
    const root = opts.root;
    const getStore = opts.getStore || (() => ({}));
    const getT = opts.getT || (() => ({}));
    const bridge = opts.bridge || null;

    async function call(name, ...args) {
        if (!bridge || typeof bridge[name] !== "function") return null;
        try {
            return await bridge[name](...args);
        } catch (e) {
            console.warn(`SCVB Tab3:bridge.${name}() 调用失败 —— ${e.message}`);
            return null;
        }
    }

    const $ = (gb) => root.querySelector(`[data-gb="${gb}"]`);

    const els = {};
    /**
     * 页面内一次性状态(不属 state chunk)。Wave 1 只有渲染脏标记与参数缓存;
     * [Wave 2] 将加入:lanePick(点击顺序数组,shift 锚点语义依赖它)/ selection
     * {startS,endS} / selectedSeg {ch,segIdx}(**每次 segments 事件后重绑或失效**,
     * brief §0.7)/ 拖拽态 / 确认框态。
     */
    const local = {
        vadParams: { ...DEFAULT_VAD_PARAMS }, // §1.18 五字段整包缓存
        segmentation: { ...DEFAULT_SEGMENTATION }, // §1.19 整包缓存
        staticDirty: true, // 静态层(波形位图)需重绘
        overlayDirty: true, // 共享动态层(曲线/边界)需重绘
        marksDirty: true, // 段角标 DOM 需重建
        repaintQueued: false,
        lanes: new Map(), // ch → 节点缓存(15 行 × 事件频率下不逐帧 querySelector)
    };

    /** 中央写闸(Wave 2 全部上行的唯一闸口;口径同 Tab2:只读观察态挡)。 */
    function isWriteBlocked() {
        return !!getStore().readOnly;
    }
    void isWriteBlocked; // [Wave 2] 交互接线后启用;先行定义钉住口径

    /** Tab3 是否为当前激活页(四面板同在 DOM,#content[data-tab] 切换)。 */
    function isPanelActive() {
        const panel = els.panel;
        if (!panel || typeof panel.closest !== "function") return true;
        const host = panel.closest("[data-tab]");
        return !host || host.getAttribute("data-tab") === "wave";
    }

    // 视口模型(可复用件;Wave 1 恒全览,Wave 2 的缩放/平移只改这一处)
    const timeline = createTimeline({
        durationS: FALLBACK_DURATION_S,
        onChange: () => {
            local.staticDirty = true;
            local.overlayDirty = true;
            local.marksDirty = true;
            schedulePaint();
        },
    });

    // 分块拉取源(LRU 8 块/轨;契约 §1.27 一次调用一次 resolve)
    const waveSource = createWaveformSource({
        request: (ch, s0, s1, cols) =>
            call("requestWaveform", ch, s0, s1, cols),
    });

    // 分层骨架:静态位图重绘走脏标记,动态层 Wave 1 无逐帧诉求(空闲零 rAF)
    const layers = createLayerStack({
        drawStatic: () => {
            paintStaticLanes();
            paintOverlay();
        },
        drawDynamic: () => false,
    });

    // 播放头(rAF 插值;降级档位由 layers 的帧时账供给)
    const playhead = createPlayhead({
        degradeLevel: () => layers.governor.level(),
        apply: (tS, playing) => {
            const el = els.playhead;
            if (!el) return;
            const stageW = stageWidth();
            const vp = timeline.viewport();
            const x = timeToX(vp, stageW, tS);
            const visible = playing && x >= 0 && x <= stageW;
            if (el.hidden === visible) el.hidden = !visible;
            if (visible) el.style.left = HEAD_W + x + "px";
        },
    });

    /** 舞台宽(CSS px)= 泳道容器宽 − 轨头 158 − 刻度列 44(C-04 舞台坐标系)。 */
    function stageWidth() {
        const w = els.lanes ? els.lanes.clientWidth : 0;
        return Math.max(w - HEAD_W - SCALE_COL_W, 0);
    }

    /** 后备存储倍率(05 §6.1:k = uiScale × dpr)。 */
    function backingK() {
        const ui = ((getStore().state || {}).ui || {}).scale;
        const dpr = typeof devicePixelRatio === "number" ? devicePixelRatio : 1;
        return backingScale(num(ui, 1), dpr);
    }

    // ---------------------------------------------------------------- 小工具
    function attr(node, name, value) {
        if (!node) return;
        const v = String(value);
        if (node.getAttribute(name) !== v) node.setAttribute(name, v);
    }

    function text(node, value) {
        if (node && node.textContent !== value) node.textContent = value;
    }

    function show(node, on) {
        if (node && node.hidden === !!on) node.hidden = !on;
    }

    function setTitle(node, value) {
        if (!node) return;
        if (value) attr(node, "title", value);
        else node.removeAttribute("title");
    }

    /** 词条占位符求值(i18n.js 只发字典;缺 key 时保底空串,不渲染裸模板)。 */
    function fmtKey(key, vals) {
        const t = getT();
        const raw = t && t[key];
        return typeof raw === "string" ? format(raw, vals || {}) : "";
    }

    // ---------------------------------------------------------------- mount
    function mount() {
        els.panel = $("tab-wave");
        els.window = $("wave-window");
        els.lanes = $("wave-lanes");
        els.rulerScale = $("wave-ruler-scale");
        els.overlay = $("wave-overlay");
        els.playhead = $("wave-playhead");
        els.playheadCap = $("wave-playhead-cap");
        els.zoom = $("wave-zoom-readout");
        els.thumb = $("wave-hscroll-thumb");
        els.rangeline = $("wave-rangeline");
        els.hint = $("wave-trackpickhint");
        els.chip = $("wave-selchip");
        els.recapRow = $("wave-recapture-row");
        els.recapBadge = $("wave-recapture-badge");
        els.applying = $("wave-applying-spinner");
        els.applyBtn = $("wave-btn-applysegments");
        els.btnRecapture = $("wave-btn-recapture");
        els.btnReanalyze = $("wave-btn-reanalyze");
        els.btnReidentify = $("wave-btn-reidentify");
        els.btnClear = $("wave-btn-clearcoverage");

        // 15 泳道生成(afterbegin:让静态占位的 overlay/selband/playhead 留在
        // 后面的 DOM 序,绝对定位层叠在泳道之上)
        if (els.lanes && !root.querySelector('[data-gb="wave-lane-1"]')) {
            let html = "";
            for (let ch = 1; ch <= LANE_COUNT; ch++) html += waveLaneHtml(ch);
            els.lanes.insertAdjacentHTML("afterbegin", html);
        }
        for (let ch = 1; ch <= LANE_COUNT; ch++) {
            const gb = (s) => $(`wave-lane-${ch}${s ? "-" + s : ""}`);
            local.lanes.set(ch, {
                row: gb(""),
                light: gb("light"),
                label: gb("label"),
                low: gb("lowsample"),
                covseg: gb("covseg"),
                check: gb("checkbox"),
                eye: gb("curvevisible"),
                canvas: gb("static"),
                badges: gb("badges"),
            });
        }
        // 滑杆节点缓存(值文本 + 行程 --p + aria)
        els.sliders = SLIDERS.map((def) => {
            const box = $(def.gb);
            return {
                def,
                val: $(def.gb + "-val"),
                track: box ? box.querySelector(".wave-slider__track") : null,
            };
        });
        // 共享动态层与播放头的纵向覆盖高度 = 15 × 34(滚动层内容高)
        const contentH = LANE_COUNT * LANE_H;
        if (els.overlay) els.overlay.style.height = contentH + "px";
        if (els.playhead) els.playhead.style.height = contentH + "px";

        // 泳道区滚动/尺寸变化 → 按可见集重绘静态层(brief §0.13;passive 只标脏)
        if (els.lanes && typeof els.lanes.addEventListener === "function") {
            els.lanes.addEventListener(
                "scroll",
                () => {
                    local.staticDirty = true;
                    schedulePaint();
                },
                { passive: true },
            );
        }
        if (typeof ResizeObserver === "function" && els.lanes) {
            new ResizeObserver(() => {
                local.staticDirty = true;
                local.overlayDirty = true;
                schedulePaint();
            }).observe(els.lanes);
        }

        // [Wave 2] 交互接线全部落在这里,一处一条(Wave 1 零监听器):
        //   TODO(T33 Wave 2) 泳道点选 + shift 连选(05 行 288;稿内 pick() 照抄)
        //   TODO(T33 Wave 2) 7 滑杆两段式(拖动 ≤50Hz setVadParams/setSegmentation
        //        整包预览;松手只显示倒计时条,300ms 防抖在 C++ 侧)
        //   TODO(T33 Wave 2) 四动作按钮 + 二次确认框(recaptureArm / analyze /
        //        analyze{clearManual} / clearCoverage;逐钮 disabled 判据 C-07)
        //   TODO(T33 Wave 2) 选区拖拽 + 双读数 + 设为范围(setRange("manual",…);
        //        选区退化 startS>=endS 时禁用按钮)+ 选区外压暗几何
        //   TODO(T33 Wave 2) 边界拖拽吸附(valleys[] + Alt)/ 双击分割 / Delete 合并
        //        (editSegment 五 op,释放才发,全入撤销栈;merge 处理 notAdjacent)
        //   TODO(T33 Wave 2) 检查器 Pan/Vol 编辑 + 锁定 toggle(set_values /
        //        set_locked;selectedSeg 每帧 segments 事件后重绑)
        //   TODO(T33 Wave 2) Ctrl+滚轮缩放 / 拖底部条与空白平移(timeline.zoom/pan;
        //        拖动期先 blit,静止 120ms 取新块)
        //   TODO(T33 Wave 2) 布防 badge 点击跳转定位选区;footer 琥珀警告接线(B-04)
        //   TODO(T33 Wave 2) 空态 CTA 跳 Tab1;曲线可见 eye 钮;标尺键盘可达性
        render();
    }

    // ---------------------------------------------------------------- 渲染
    /** 重绘调度:合帧一次(多事件同拍到达时只画一遍)。 */
    function schedulePaint() {
        if (local.repaintQueued) return;
        local.repaintQueued = true;
        const run = () => {
            local.repaintQueued = false;
            if (!isPanelActive()) return; // 非前台不烧 canvas;切回时 render 再标脏
            if (local.staticDirty || local.overlayDirty) {
                local.staticDirty = false;
                local.overlayDirty = false;
                layers.invalidateStatic();
            }
        };
        if (typeof requestAnimationFrame === "function") {
            requestAnimationFrame(run);
        } else {
            run();
        }
    }

    /** store → 全部只读投影(外壳每次 render() 都调;必须廉价、可重入)。 */
    function render() {
        const store = getStore();
        const t = getT();
        timeline.setDuration(durationOf(store));
        const vp = timeline.viewport();

        // ---- 空态(data-empty 驱动泳道/空态互斥;标尺与底部条仍在,§15)
        attr(els.window, "data-empty", isLanesEmpty(store) ? 1 : 0);

        // ---- 轨头六件(15 行)
        const lanes = laneModelFromStore(store);
        for (const lane of lanes) {
            const n = local.lanes.get(lane.n);
            if (!n) continue;
            attr(n.row, "data-status", lane.status);
            const vis = statusVisual(lane.status);
            attr(n.light, "data-tone", vis.tone);
            attr(n.light, "data-pulse", vis.pulse ? 1 : 0);
            setTitle(n.light, t[vis.key] || "");
            text(n.label, lane.label || labelPlaceholder(lane.n, t));
            text(
                n.covseg,
                fmtKey("wave.covSeg", { p: lane.cov, n: lane.segs }),
            );
            show(n.low, !!lane.low);
            setTitle(n.low, lane.low ? t["lowSample.full"] || "" : "");
            if (n.check) {
                n.check.setAttribute(
                    "aria-label",
                    fmtKey("wave.pickTrack", { n: tt(lane.n) }),
                );
            }
        }

        // ---- 工具条:滑杆读数(本地整包缓存;初值 = 设计稿/J23 默认)。
        //      参数真身在 state.analysis(契约 §1.1/§2.1),但 mock/02 的默认值
        //      口径(threshold_db 正值、sensitivity 0..100)与 05/设计稿的滑杆
        //      值域(-60..-10 dB、0..1)相悖 —— Wave 1 不回读 state,静态显示
        //      设计默认;TODO(T33 Wave 2)与统筹裁定两套口径后再接
        //      state.analysis 整组回读 + 拖动乐观回声让位(deviations 已登记)。
        for (const s of els.sliders || []) {
            const src =
                s.def.api === "vad" ? local.vadParams : local.segmentation;
            const v = num(src[s.def.field], s.def.def);
            text(s.val, fmtSliderValue(s.def, v));
            if (s.track) {
                s.track.style.setProperty(
                    "--p",
                    String(sliderPercent(s.def, v)),
                );
                attr(s.track, "aria-valuenow", v);
            }
        }

        // ---- 四钮判据(C-07:各钮各自的 05 判据;Wave 1 无选区/轨选 → 依赖
        //      选区的三钮保持 disabled,重新识别按段表投影)
        const reidentifyOn = hasNonAutoSegments(store.segments);
        attr(els.btnReidentify, "data-disabled", reidentifyOn ? 0 : 1);
        attr(
            els.btnReidentify,
            "aria-disabled",
            reidentifyOn ? "false" : "true",
        );
        for (const btn of [els.btnRecapture, els.btnReanalyze, els.btnClear]) {
            attr(btn, "data-disabled", 1);
            attr(btn, "aria-disabled", "true");
        }

        // ---- 轨选提示 / 选区 chip(Wave 1 无轨选:恒显提示、chip 隐)
        show(els.hint, true);
        show(els.chip, false);

        // ---- 「正在应用…」菊花(契约 §2.2:analysis_run.running 驱动)
        const running = !!(
            (store.state || {}).analysis_run && store.state.analysis_run.running
        );
        show(els.applying, running);

        // ---- 「应用到分段」抑制期按钮(J47:只有 PRINT 态或分析进行中;
        //      Wave 1 先按 running 投影,PRINT 态并入 Wave 2 的 outputPhase 接线)
        show(els.applyBtn, running);

        // ---- 重采集布防行(scvb.state.recapture:{armed,tracksMask,startS,endS,
        //      autoStop},无 reason;以事件回读恢复显示,切 tab/重开面板不丢)
        const rec = (store.state || {}).recapture || null;
        const armed = !!(rec && rec.armed);
        show(els.recapRow, armed);
        if (armed) {
            let cnt = 0;
            const mask = Math.trunc(num(rec.tracksMask, 0));
            for (let b = 0; b < LANE_COUNT; b++) if (mask & (1 << b)) cnt++;
            text(
                els.recapBadge,
                fmtKey("wave.recaptureArmed", {
                    x: fmtTimeMs(num(rec.startS, 0)),
                    y: fmtTimeMs(num(rec.endS, 0)),
                    n: cnt,
                }),
            );
        }

        // ---- Range 只读细线(05 行 290/314;A-10:follow 不画)
        const range = ((store.state || {}).global || {}).range || {};
        const stageW = stageWidth();
        if (
            (range.mode === "daw_loop" || range.mode === "manual") &&
            stageW > 0
        ) {
            const x0 = Math.max(timeToX(vp, stageW, num(range.start_s, 0)), 0);
            const x1 = Math.min(
                timeToX(vp, stageW, num(range.end_s, 0)),
                stageW,
            );
            if (x1 > x0) {
                show(els.rangeline, true);
                attr(els.rangeline, "data-mode", range.mode);
                els.rangeline.style.left = HEAD_W + x0 + "px";
                els.rangeline.style.width = x1 - x0 + "px";
            } else {
                show(els.rangeline, false);
            }
        } else {
            show(els.rangeline, false);
        }

        // ---- 标尺 + 缩放读数 + 底部 thumb(视口投影)
        renderRuler(vp);
        text(els.zoom, zoomLabel(vp, timeline.durationS()));
        if (els.thumb) {
            const th = scrollThumb(vp, timeline.durationS());
            els.thumb.style.left = th.left * 100 + "%";
            els.thumb.style.width = th.width * 100 + "%";
        }

        // ---- 段角标 DOM(segments 事件后标脏才重建,避免逐拍重建 innerHTML)
        if (local.marksDirty) {
            local.marksDirty = false;
            renderSegMarks(store, vp, stageW);
        }

        schedulePaint();
    }

    function renderRuler(vp) {
        const scale = els.rulerScale;
        if (!scale) return;
        const ticks = rulerTicks(vp);
        const span = spanOf(vp);
        const sig = ticks.map((k) => k.label).join("|") + "@" + span;
        if (scale.getAttribute("data-sig") === sig) return;
        scale.setAttribute("data-sig", sig);
        scale.innerHTML = ticks
            .map((k, i) => {
                const left = ((k.tS - vp.startS) / span) * 100;
                const last = i === ticks.length - 1 && left > 90 ? 1 : 0;
                return `<span class="wave-ruler__tick" style="left:${left.toFixed(2)}%"${last ? ' data-last="1"' : ""}>${k.label}</span>`;
            })
            .join("");
    }

    /** 段角标(E/C/锁定)重建;词条 wave.lockBadge 随语言切换(render 会再进来)。 */
    function renderSegMarks(store, vp, stageW) {
        const t = getT();
        for (let ch = 1; ch <= LANE_COUNT; ch++) {
            const n = local.lanes.get(ch);
            if (!n || !n.badges) continue;
            const marks = segMarksOf(segmentsOfCh(store.segments, ch));
            let html = "";
            for (const m of marks) {
                const x = timeToX(vp, stageW, m.tS);
                if (x < 0 || x > stageW) continue;
                const left = ((x / stageW) * 100).toFixed(2);
                if (m.kind === "lock") {
                    html += `<span class="wave-seg-lock" style="left:calc(${left}% + 26px)"><svg width="9" height="9" viewBox="0 0 12 12" aria-hidden="true"><rect x="2.4" y="5.2" width="7.2" height="4.6" rx="1.3" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M4 5.2V3.9a2 2 0 0 1 4 0v1.3" fill="none" stroke="currentColor" stroke-width="1.5"/></svg>${esc(t["wave.lockBadge"] || "")}</span>`;
                } else {
                    html += `<span class="wave-seg-badge" style="left:calc(${left}% + 9px)">${m.kind}</span>`;
                }
            }
            if (n.badges.innerHTML !== html) n.badges.innerHTML = html;
        }
    }

    // ------------------------------------------------------------ canvas 绘制
    /** 可见泳道集(垂直滚动下同屏可见更少 —— 按可见集渲染,05 §6.3)。 */
    function visibleLanes() {
        const lanesEl = els.lanes;
        if (!lanesEl || !lanesEl.clientHeight) return [];
        const top = lanesEl.scrollTop;
        const bottom = top + lanesEl.clientHeight;
        const out = [];
        for (let ch = 1; ch <= LANE_COUNT; ch++) {
            const y0 = (ch - 1) * LANE_H;
            if (y0 + LANE_H >= top && y0 <= bottom) out.push(ch);
        }
        return out;
    }

    /** 静态层:每条可见泳道一块 tile → 一张位图(缓存命中直画,未命中取到再补)。 */
    function paintStaticLanes() {
        const vp = timeline.viewport();
        const w = stageWidth();
        if (!(w > 0)) return;
        const k = backingK();
        const cols = Math.min(Math.max(Math.round(w), 1), MAX_COLS);
        for (const ch of visibleLanes()) {
            const n = local.lanes.get(ch);
            if (!n || !n.canvas) continue;
            const ctx = resizeCanvas(n.canvas, w, LANE_H, k);
            if (!ctx) continue;
            const tile = waveSource.peek(ch, vp.startS, vp.endS, cols);
            if (tile) {
                paintWaveTile(ctx, tile, w, LANE_H);
            } else {
                // 取数在途:resolve 后补一次静态重绘(契约 §1.27 一次调用一次
                // resolve;peek 不 await —— 渲染帧禁止阻塞)
                waveSource.getTile(ch, vp.startS, vp.endS, cols).then((got) => {
                    if (!got) return;
                    local.staticDirty = true;
                    schedulePaint();
                });
            }
        }
    }

    /**
     * 共享动态层(§6.3:整个泳道区一张覆盖 canvas):pan/vol 阶梯曲线 + 段边界。
     * 段内水平、边界以 transition_ramp_ms 为宽画**以边界为中心对称**的斜坡,
     * 绝不垂直跳变(图例帧 758-765 的关键几何);auto/手动两级权重线宽 1 / 1.6
     * (B-09:+0.6px 差保留,不照搬放大帧的 2.4/3)。
     */
    function paintOverlay() {
        const canvas = els.overlay;
        if (!canvas) return;
        const w = stageWidth();
        const h = LANE_COUNT * LANE_H;
        if (!(w > 0)) return;
        const ctx = resizeCanvas(canvas, w, h, backingK());
        if (!ctx) return;
        ctx.clearRect(0, 0, w, h);
        const store = getStore();
        const vp = timeline.viewport();
        const rampMs = num(
            ((store.state || {}).analysis || {}).transition_ramp_ms,
            80, // 契约 §1.20 默认 80(20..300);字段在 state.analysis 面
        );
        const rampPx = Math.max((rampMs / 1000) * (w / spanOf(vp)), 1);

        for (const ch of visibleLanes()) {
            const segCh = segmentsOfCh(store.segments, ch);
            const segs = (segCh && segCh.segments) || [];
            if (!segs.length) continue;
            const y0 = (ch - 1) * LANE_H;

            // 曲线可见 toggle(眼睛钮)灭 → 本轨曲线与边界都不画(防遮挡语义)
            const eye = local.lanes.get(ch);
            if (
                eye &&
                eye.eye &&
                eye.eye.getAttribute("aria-pressed") === "false"
            ) {
                continue;
            }

            // ① 两条阶梯曲线(pan = accent 薰衣草 / vol = 白,05 行 310)
            drawStepCurve(ctx, segs, vp, w, y0, rampPx, "pan");
            drawStepCurve(ctx, segs, vp, w, y0, rampPx, "vol");

            // ② 段边界(auto 白虚线 5 5 / 手动白实线;图例帧 763-764)
            for (const b of boundariesOf(segCh)) {
                const x = timeToX(vp, w, b.tS);
                if (x < 0 || x > w) continue;
                ctx.beginPath();
                ctx.setLineDash(b.manual ? [] : [5, 5]);
                ctx.strokeStyle = b.manual
                    ? "rgba(255,255,255,.5)"
                    : "rgba(255,255,255,.3)";
                ctx.lineWidth = 1;
                ctx.moveTo(x, y0);
                ctx.lineTo(x, y0 + LANE_H);
                ctx.stroke();
            }
            ctx.setLineDash([]);
        }
    }

    /** 单维阶梯曲线:逐段水平线 + 相邻段值差处以边界为中心的对称斜坡。 */
    function drawStepCurve(ctx, segs, vp, w, laneY, rampPx, dim) {
        const yOf = (seg) =>
            laneY + (dim === "pan" ? panYPx(seg.pan) : volYPx(seg.volDb));
        for (let i = 0; i < segs.length; i++) {
            const seg = segs[i];
            if (!seg) continue;
            const manual = seg.origin && seg.origin !== "auto";
            // 两级权重(05 行 310):auto 半透明细线,手动实线加重 +0.6px
            ctx.strokeStyle =
                dim === "pan"
                    ? manual
                        ? "rgba(181,172,201,.98)"
                        : "rgba(181,172,201,.5)"
                    : manual
                      ? "rgba(255,255,255,.92)"
                      : "rgba(255,255,255,.42)";
            ctx.lineWidth = manual ? 1.6 : 1;
            const x0 = timeToX(vp, w, num(seg.t0S, 0));
            const x1 = timeToX(vp, w, num(seg.t1S, 0));
            if (x1 < 0 || x0 > w) continue;
            const y = yOf(seg);
            const half = rampPx / 2;
            const prev = segs[i - 1];
            const next = segs[i + 1];
            ctx.beginPath();
            // 段头:有上一段且值不同 → 从斜坡终点起画(斜坡由上一段迭代画上半)
            ctx.moveTo(Math.max(x0 + (prev ? half : 0), 0), y);
            ctx.lineTo(Math.min(x1 - (next ? half : 0), w), y);
            ctx.stroke();
            // 边界斜坡(跨在边界两侧,以边界为中心对称;绝不垂直跳变)
            if (next) {
                const yNext = yOf(next);
                ctx.beginPath();
                ctx.moveTo(x1 - half, y);
                ctx.lineTo(x1 + half, yNext);
                ctx.stroke();
            }
        }
    }

    // ------------------------------------------------------------ 事件入口
    /** §2.8:段表/曲线/角标脏;selectedSeg 重绑归 Wave 2(brief §0.7)。 */
    function onSegments() {
        local.overlayDirty = true;
        local.marksDirty = true;
        schedulePaint();
    }

    /** §2.7(播放中 2Hz):覆盖条延伸 → 该轨块缓存失效 + 静态层脏。 */
    function onCaptureProgress(cp) {
        for (const c of (cp && cp.channels) || []) {
            if (c && c.ch) waveSource.invalidate(c.ch);
        }
        local.staticDirty = true;
        schedulePaint();
    }

    /** §2.6(30Hz):播放头 rAF 插值;采集中头部绿色进度点(A-15)。 */
    function onPlayhead(p) {
        playhead.push(p || null);
        const st = getStore();
        const capturing = !!((st.state || {}).global || {}).capture_enabled;
        show(els.playheadCap, !!(p && p.isPlaying) && capturing);
    }

    return { mount, render, onSegments, onCaptureProgress, onPlayhead };
}
