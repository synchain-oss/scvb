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
//   • **Wave 1** = 静态结构:store → 泳道模型**只读投影**(轨头六件 / 空态 /
//     布防 badge / 菊花态 / 按钮判据),canvas 静态层(包络/VAD/未覆盖/stale/
//     passId/覆盖条,canvas/waveform.js)与共享动态层(曲线/边界,本文件画)。
//   • **Wave 2(本文件现状)** = 全部交互:滑杆两段式(拖动 ≤50Hz 整包预览,
//     松手 300ms 防抖在 C++ 侧,UI 只显示倒计时条 + 消费 §2.8)/ 四动作 + 确认框 /
//     选区拖拽 + 设为范围 / 边界拖拽吸附 + 分割合并 / 点选 + shift 连选 /
//     检查器编辑 + 锁定(selectedSegs 每次 §2.8 事件后重绑,brief §0.7)/
//     缩放平移(静止 120ms 取新块、拖动先 blit)/ 布防 badge + footer 警告 /
//     A2 七件。中央写闸 isWriteBlocked() + 轨级死态是所有上行的唯一闸口。
// =============================================================================

// 可复用件四件套(T34/T43 复用面;各自文件头写明复用契约)
import {
    createTimeline,
    timeToX,
    xToTime,
    spanOf,
    zoomLabel,
    scrollThumb,
    ZOOM_STEP,
} from "./canvas/timeline.js";
import {
    backingScale,
    resizeCanvas,
    observeResolution,
} from "./canvas/hidpi.js";
import { createLayerStack } from "./canvas/layers.js";
import { createPlayhead } from "./canvas/playhead.js";
import {
    createWaveformSource,
    paintWaveTile,
    MAX_COLS,
    IDLE_REFETCH_MS,
    VAD_ALPHA,
} from "./canvas/waveform.js";
import { nearestHit, BOUNDARY_HIT_PX } from "../shared/hit.js";
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
import { format, outputPhase, lowSampleChannels } from "./tab-master.js";

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

/**
 * 秒 → `mm:ss.mmm`(检查器/选区读数主显,05 §2.3a 行 331 / B-12)。
 * 先整体量化到毫秒再拆位:毫秒四舍五入的进位联动到秒/分
 * (1.9996 → "00:02.000",不会出现 ".1000" 四位毫秒)。
 */
export function fmtTimeMs(s) {
    const totalMs = Math.round(Math.max(num(s, 0), 0) * 1000);
    const m = Math.floor(totalMs / 60000);
    const sec = Math.floor((totalMs % 60000) / 1000);
    const ms = totalMs % 1000;
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
    // §2.9 lowSample 是轨级 error 且会同时命中多轨 —— 与 Tab2 轨行**消费同一份**
    // (app.js 按 `lowSample#{ch}` 复合键存,这里扫成轨号集合;T33)。
    const low = lowSampleChannels(st.errors);
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
            low: low.has(ch) ? 1 : 0,
            picked: 0,
        });
    }
    return lanes;
}

/**
 * 空态判定(05 §2.3 行 318:**全部轨**无 coverage 才空)。
 * 「有无 coverage」的真源是 **scvb.state**(契约 §0.4:captureProgress 只在
 * 播放中发,首启非播放时不发,「空态由 scvb.state 承载」)—— 停播打开面板时
 * 以 state.features.bytes(§2.1:特征数据字节数)与段表判非空,coverage
 * 事件只作播放中的增量补充,不能单独当空态依据。
 */
export function isLanesEmpty(store) {
    const st = store || {};
    if (num(((st.state || {}).features || {}).bytes, 0) > 0) return false;
    for (const c of (st.segments && st.segments.channels) || []) {
        if (((c && c.segments) || []).length > 0) return false;
    }
    const cov = st.coverage || {};
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

/** 阶梯曲线线宽(B-09:放大帧 2.4/3 不直接搬,实景取 1/1.6 保 +0.6px 差)。 */
const CURVE_W_AUTO = 1;
const CURVE_W_MANUAL = 1.6;
/**
 * 曲线深底描边:压在同色系包络柱上仍能读出两条线(Wave 3 视觉修)。
 * 描边宽 = 本色线宽 + `CURVE_HALO_W`。**深底不得压过本色**:+1.4/α.88 时
 * auto 档(lw=1)成 2.4px 近黑带里一根 1px 芯丝,墨量 2.4:1,05 行 310 的
 * 「薰衣草 / 白」两色在实景里读成近黑发丝(对抗校验 minor)→ 收到 +0.8/α.72,
 * 每侧只露 0.4px 垫底。
 */
const CURVE_HALO = "rgba(18,15,28,.72)";
const CURVE_HALO_W = 0.8;

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

// -----------------------------------------------------------------------------
// Wave 2 交互纯函数(node 侧断言面;频率/几何/重绑纪律都钉在这里)
// -----------------------------------------------------------------------------

/** 拖动期参数下发节流周期(ms;40Hz ≤ brief §0.8 的 50Hz 上限)。 */
export const PARAM_THROTTLE_MS = 25;

/** previewAnalyze 节流(ms;契约 §1.5「UI 侧节流调用」)。 */
export const PREVIEW_THROTTLE_MS = 250;

/** diff 变更列表自动收起(ms;A-02 一次性反馈组件)。 */
export const DIFF_HIDE_MS = 6000;

/** 边界吸附捕获半径(CSS px;A-14:命中谷点加亮 + 轻微磁吸)。 */
export const SNAP_PX = 6;

/** 滑杆键盘档的「视为松手」静默窗(ms;之后走与 pointerup 相同的释放路径)。 */
export const SLIDER_KEY_RELEASE_MS = 250;

/**
 * 泳道点选(05 §2.3 行 288 语义,**照抄稿内 pick(),不自行发明**):
 * 普通点击 = toggle;shift = 以**上一次点选轨**(数组尾)为锚点连选,
 * 并集追加、不清空既有选择。`cur` 保持**点击顺序数组**(锚点语义依赖它)。
 */
export function applyPick(cur, ch, shiftKey) {
    const list = Array.isArray(cur) ? cur : [];
    const has = list.indexOf(ch) >= 0;
    if (shiftKey && list.length) {
        const last = list[list.length - 1];
        const a = Math.min(last, ch);
        const b = Math.max(last, ch);
        const add = [];
        for (let k = a; k <= b; k++) {
            if (list.indexOf(k) < 0) add.push(k);
        }
        return list.concat(add);
    }
    return has ? list.filter((x) => x !== ch) : list.concat([ch]);
}

/**
 * u16 位图 → **相邻轨合并后的纵向条带**([{ch0, count}],ch0 为 1-based 起轨)。
 * 布防斜条纹只该盖 `{布防轨} × {选区}` 的交集(05 行 300)—— 一条覆盖 15 泳道的
 * 满高带会让用户以为全部轨都要被重采集(无头 QA 实拍抓到)。相邻轨并成一条,少建 DOM。
 */
export function maskRuns(mask) {
    const m = Math.trunc(num(mask, 0));
    const runs = [];
    let cur = null;
    for (let ch = 1; ch <= LANE_COUNT; ch++) {
        if (m & (1 << (ch - 1))) {
            if (cur) cur.count += 1;
            else runs.push((cur = { ch0: ch, count: 1 }));
        } else {
            cur = null;
        }
    }
    return runs;
}

/** 点选轨表 → u16 位图(契约 §0.2:bit0=ch1 … bit14=ch15)。 */
export function maskOfPicked(picked) {
    let mask = 0;
    for (const ch of picked || []) {
        if (ch >= 1 && ch <= LANE_COUNT) mask |= 1 << (ch - 1);
    }
    return mask >>> 0;
}

/**
 * 选中段重绑(brief §0.7 / 契约 §2.8 字段纪律:`segIdx` 每次事件后重新编号,
 * UI 选中态**必须按事件重绑或失效**,不得跨事件持有旧下标)。
 * `keys` = 上一帧的 `{t0S,t1S}` 时间锚;在新段表里先找**中点包含**,退而求
 * **最大重叠**;找不到即失效(掉出返回表);重复命中去重。
 */
export function rebindSegKeys(keys, segs) {
    const list = Array.isArray(segs) ? segs : [];
    const out = [];
    for (const k of keys || []) {
        if (!k) continue;
        const mid = (num(k.t0S, 0) + num(k.t1S, 0)) / 2;
        let best = -1;
        let bestOv = 0;
        for (let i = 0; i < list.length; i++) {
            const s = list[i];
            if (!s) continue;
            if (mid >= s.t0S && mid < s.t1S) {
                best = i;
                break;
            }
            const ov = Math.min(s.t1S, k.t1S) - Math.max(s.t0S, k.t0S);
            if (ov > bestOv) {
                bestOv = ov;
                best = i;
            }
        }
        if (best >= 0 && !out.some((o) => o.idx === best)) {
            out.push({ idx: best, t0S: list[best].t0S, t1S: list[best].t1S });
        }
    }
    return out;
}

/**
 * 边界吸附(05 行 313 / 契约 §5.4:吸附最近能量谷,Alt 关吸附;
 * 谷点表 = `requestWaveform.valleys[]`,秒、升序)。捕获半径按当前
 * px/秒换算成秒域(SNAP_PX 是 CSS px 口径)。
 */
export function snapBoundary(tS, valleys, pxPerSecond, altKey) {
    const t = num(tS, 0);
    if (altKey || !Array.isArray(valleys) || !(pxPerSecond > 0)) {
        return { tS: t, snapped: false };
    }
    const r = SNAP_PX / pxPerSecond;
    let best = null;
    for (const v of valleys) {
        const d = Math.abs(num(v, NaN) - t);
        if (d <= r && (best === null || d < Math.abs(best - t))) best = v;
    }
    return best === null
        ? { tS: t, snapped: false }
        : { tS: best, snapped: true };
}

/**
 * scope 内的段计数(mask × [startS,endS) 相交):
 * `marks` = 未锁定的 origin≠auto 段(重新识别确认框的 {k});
 * `locked` = 锁定段({l},clearManual 免疫面);`overlap` = 相交段总数
 * (05 行 300「将覆盖 {k} 段已有数据」)。
 */
export function countsInScope(segments, mask, startS, endS) {
    const s0 = num(startS, -Infinity);
    const s1 = num(endS, Infinity);
    let marks = 0;
    let locked = 0;
    let overlap = 0;
    for (const c of (segments && segments.channels) || []) {
        if (!c || !((mask >>> (c.ch - 1)) & 1)) continue;
        for (const s of c.segments || []) {
            if (!s || !(s.t1S > s0 && s.t0S < s1)) continue;
            overlap++;
            if (s.locked) locked++;
            else if (s.origin && s.origin !== "auto") marks++;
        }
    }
    return { marks, locked, overlap };
}

/** 带符号数值文本(检查器输入框:「+2」「-14.2」;dp 位小数,-0 归零)。 */
export function fmtSigned(v, dp) {
    const p = 10 ** (dp || 0);
    let x = Math.round(num(v, 0) * p) / p;
    if (Object.is(x, -0)) x = 0;
    const s = dp > 0 ? x.toFixed(dp) : String(x);
    return x > 0 ? `+${s}` : s;
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
     * 页面内一次性状态(不属 state chunk)。
     * `lanePick` 保持**点击顺序数组**(shift 锚点语义依赖它,05 行 288);
     * `selectedSegs` 存 `{idx,t0S,t1S}` 时间锚,**每次 segments 事件后经
     * rebindSegKeys 重绑或失效**(brief §0.7),editSegment 调用时取当帧 idx。
     */
    const local = {
        vadParams: { ...DEFAULT_VAD_PARAMS }, // §1.18 五字段整包缓存
        segmentation: { ...DEFAULT_SEGMENTATION }, // §1.19 整包缓存
        staticDirty: true, // 静态层(波形位图)需重绘
        overlayDirty: true, // 共享动态层(曲线/边界)需重绘
        marksDirty: true, // 段角标 DOM 需重建(舞台量不到宽时保留脏位)
        repaintQueued: false,
        lastUiScale: NaN, // ui.scale 档位账(变化 = 后备存储重建,05 §6.1)
        gutterPx: -1, // 泳道区纵向滚动条宽账(标尺/刻度列对基用)
        lanes: new Map(), // ch → 节点缓存(15 行 × 事件频率下不逐帧 querySelector)
        // ---- Wave 2 交互态 ----
        lanePick: [], // 点击顺序数组(锚点 = 尾元素)
        selection: null, // 工作选区 {startS,endS}(独立对象,不与 Range 同步)
        selDrag: null, // "L" | "R" | "create"
        selectedCh: 0, // 段选中所在轨(0 = 无)
        selectedSegs: [], // [{idx,t0S,t1S}] ≤2(两段 = 合并候选)
        boundDrag: null, // {ch, segIdx, tS, snapped, minS, maxS}
        sliderDrag: null, // 正在拖的滑杆(els.sliders 元素)
        sliderKeyTimer: 0, // 键盘档「视为松手」计时
        lastParamSend: 0, // ≤50Hz 节流账(Date.now 系)
        paramTimer: 0, // 节流尾包计时器
        countdownApi: null, // "vad"|"segmentation":倒计时条挂起中(A-01)
        countdownTimer: 0, // 倒计时兜底自撤(抑制期开始后 §2.8 不来时不挂死)
        diff: null, // 最近一次 diff 摘要(A-02)
        diffTimer: 0,
        toolbarNote: null, // {key, vals}:布防拒绝/notAdjacent/清除回执行内反馈
        toolbarNoteTimer: 0,
        preview: null, // previewAnalyze 结果缓存(A-07)
        previewTimer: 0,
        rangeTip: null, // {x,y}:「设为范围」一次性提示(§7)
        lastVpChange: 0, // 视口最近变化时刻(静止 120ms 才取新块,§6.3)
        vpIdleTimer: 0,
        panDrag: null, // 空白拖拽平移 {lastX}
        thumbDrag: null, // 底部条拖拽 {startX, vp0}
        pendingDown: null, // 舞台按下未定性(点选 vs 平移)
        knobDrag: null, // 检查器 PAN 旋钮拖拽 {startY, startVal, val}
        vslDrag: null, // 检查器 VOL 滑杆拖拽 {val}
        echo: {}, // 检查器拖拽乐观值 {pan?, vol?}(segments 事件后失效)
        lastPaint: new Map(), // ch → {tile,startS,endS}(拖动期 blit 的老底)
        tickerRaf: 0, // 交互/播放期帧时账 rAF(喂 layers.governor)
        palette: null, // mount 时 getComputedStyle 换算的 canvas 调色
        playheadEv: null, // 最近一次 §2.6 载荷(非前台期间只存不驱动)
        playheadHeld: false, // 非前台把播放头/帧时账挂起过 ⇒ 切回时补起帧
    };

    /**
     * 中央写闸(全部上行的唯一闸口;口径同 Tab2:只读观察态挡;hostEcho 不挡,
     * noTimeline 只挡采集/输出开关不挡本页编辑面)。
     */
    function isWriteBlocked() {
        return !!getStore().readOnly;
    }

    /** 轨级死态(采样率不一致 ⇒ 该轨编辑面整体 disabled;口径同 Tab2 isRowDead)。 */
    function isLaneDead(ch) {
        const cc = ((getStore().conn || {}).channels || [])[ch - 1] || null;
        return trackStatusOf(cc) === "srErr";
    }

    /** Tab3 是否为当前激活页(四面板同在 DOM,#content[data-tab] 切换)。 */
    function isPanelActive() {
        const panel = els.panel;
        if (!panel || typeof panel.closest !== "function") return true;
        const host = panel.closest("[data-tab]");
        return !host || host.getAttribute("data-tab") === "wave";
    }

    // 视口模型(可复用件;缩放/平移只改这一处)。视口变化 → 三层标脏 +
    // 「静止 120ms 才取新块」计时(05 §6.3)+ 标尺/读数/选区几何重投影。
    const timeline = createTimeline({
        durationS: FALLBACK_DURATION_S,
        onChange: () => {
            local.staticDirty = true;
            local.overlayDirty = true;
            local.marksDirty = true;
            local.lastVpChange = Date.now();
            if (local.vpIdleTimer) clearTimeout(local.vpIdleTimer);
            local.vpIdleTimer = setTimeout(() => {
                local.vpIdleTimer = 0;
                local.staticDirty = true;
                schedulePaint();
            }, IDLE_REFETCH_MS);
            schedulePaint();
            requestRender();
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

    // 播放头(rAF 插值;降级档位由 layers 的帧时账供给)。
    // 暂停/停播**不隐藏**:05 行 315 只说「竖线,rAF 插值平滑」,无显隐条件;
    // playhead.js 的插值契约「停住则原地」—— 暂停位置是要保住的静态视觉。
    // 首帧 §2.6 事件到达前 playhead.js 不调 apply,竖线维持 HTML 初始 hidden。
    const playhead = createPlayhead({
        degradeLevel: () => layers.governor.level(),
        apply: (tS) => {
            const el = els.playhead;
            if (!el) return;
            const stageW = stageWidth();
            const vp = timeline.viewport();
            const x = timeToX(vp, stageW, tS);
            const visible = stageW > 0 && x >= 0 && x <= stageW;
            if (el.hidden === visible) el.hidden = !visible;
            if (visible) el.style.left = HEAD_W + x + "px";
        },
    });

    /** 舞台宽(CSS px)= 泳道容器宽 − 轨头 158 − 刻度列 44(C-04 舞台坐标系)。 */
    function stageWidth() {
        const w = els.lanes ? els.lanes.clientWidth : 0;
        return Math.max(w - HEAD_W - SCALE_COL_W, 0);
    }

    /**
     * 标尺/刻度列与舞台的横向对基。舞台坐标系基于泳道容器 clientWidth(不含
     * 纵向滚动条),而标尺与右缘刻度列在窗级布局(含滚动条)—— 15×34 内容出
     * 滚动条时两套基准差恒等于滚动条宽,时间刻度会相对波形整体右漂。
     * 这里把滚动条宽同步成标尺右让/刻度列右偏,让三者共用舞台坐标系。
     */
    function syncScrollGutter() {
        if (!els.lanes) return;
        const sb = Math.max(els.lanes.offsetWidth - els.lanes.clientWidth, 0);
        if (local.gutterPx === sb) return;
        local.gutterPx = sb;
        if (els.rulerScale) {
            els.rulerScale.style.marginRight = SCALE_COL_W + sb + "px";
        }
        if (els.scalecol) els.scalecol.style.right = sb + "px";
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

    // ------------------------------------------------------------ Wave 2 底座
    /**
     * 外壳重渲染请求(拖动 50Hz 事件流只折成每帧一次 render)。
     * **合帧在外壳里做**(T33:app.js 的 requestRender 是全页唯一的 rAF 合帧点)——
     * 这里再排一层 rAF 会把本地态的可见反馈整整推迟两帧(拖动手感肉眼可辨)。
     */
    function requestRender() {
        if (typeof opts.onLocalChange === "function") opts.onLocalChange();
    }

    function nowMs() {
        return Date.now();
    }

    /** data-disabled + aria-disabled 双写(按钮判据的唯一落点)。 */
    function setBtnEnabled(btn, on) {
        attr(btn, "data-disabled", on ? 0 : 1);
        attr(btn, "aria-disabled", on ? "false" : "true");
    }

    function btnDisabled(btn) {
        return !btn || btn.getAttribute("data-disabled") === "1";
    }

    /** J47 抑制判定:**只有** PRINT 态或分析进行中(brief §0.5)。 */
    function isSuppressed(store) {
        const st = store || getStore();
        const running = !!(
            (st.state || {}).analysis_run && st.state.analysis_run.running
        );
        return running || outputPhase(st.state, st.playhead) === "print";
    }

    /** 工具条行内反馈(布防拒绝 §5.5 / notAdjacent / 清除回执;5s 自撤)。 */
    function setToolbarNote(key, vals) {
        if (local.toolbarNoteTimer) clearTimeout(local.toolbarNoteTimer);
        local.toolbarNoteTimer = 0;
        local.toolbarNote = key ? { key, vals: vals || {} } : null;
        if (key) {
            local.toolbarNoteTimer = setTimeout(() => {
                local.toolbarNoteTimer = 0;
                local.toolbarNote = null;
                requestRender();
            }, 5000);
        }
        requestRender();
    }

    /** 当前动作 scope(四钮共用:勾选轨 × 工作选区;无 = null)。 */
    function currentScope() {
        const sel = local.selection;
        if (!local.lanePick.length || !sel || !(sel.startS < sel.endS)) {
            return null;
        }
        return {
            tracksMask: maskOfPicked(local.lanePick),
            startS: sel.startS,
            endS: sel.endS,
        };
    }

    // ---- 交互期帧时账(brief 任务 9:rAF 帧时喂 layers.governor,交互期
    //      降级序列可触发;空闲零 rAF —— 无交互且未播放时循环自停)
    function interactionActive() {
        return !!(
            local.sliderDrag ||
            local.selDrag ||
            local.boundDrag ||
            local.panDrag ||
            local.thumbDrag ||
            local.knobDrag ||
            local.vslDrag
        );
    }

    function ensureTicker() {
        if (local.tickerRaf || typeof requestAnimationFrame !== "function") {
            return;
        }
        // 非前台不起帧时账:Tab3 不在前台时本页一帧都不画,量到的帧时不是本页的
        // 成本,喂进 governor 只会误触降级序列(§6.1)。切回由 resumePlayhead()/
        // 交互入口补起。
        if (!isPanelActive()) {
            local.playheadHeld = true;
            return;
        }
        let last = 0;
        const loop = (ts) => {
            local.tickerRaf = 0;
            if (last > 0) layers.governor.push(ts - last);
            last = ts;
            const p = getStore().playhead;
            if (!isPanelActive()) {
                local.playheadHeld = true;
                return;
            }
            if (interactionActive() || (p && p.isPlaying)) {
                local.tickerRaf = requestAnimationFrame(loop);
            }
        };
        local.tickerRaf = requestAnimationFrame(loop);
    }

    // ---- 7 滑杆两段式(§1.18/§1.19;brief §0.4/§0.5)-------------------------
    /** 拖动档下发:**整包**(五字段 / 三字段),≤50Hz 节流,尾包补发。 */
    function sendParams(api) {
        if (api === "vad") {
            call("setVadParams", { ...local.vadParams });
        } else {
            call("setSegmentation", { ...local.segmentation });
        }
    }

    function sendParamsThrottled(api) {
        const t = nowMs();
        if (t - local.lastParamSend >= PARAM_THROTTLE_MS) {
            local.lastParamSend = t;
            sendParams(api);
            return;
        }
        if (local.paramTimer) return; // 尾包已排
        local.paramTimer = setTimeout(
            () => {
                local.paramTimer = 0;
                local.lastParamSend = nowMs();
                sendParams(api);
            },
            PARAM_THROTTLE_MS - (t - local.lastParamSend),
        );
    }

    /** 值写入本地整包缓存 + 就地刷新该杆视觉(拖动期不等整页 render)。 */
    function setSliderValue(s, v) {
        const src = s.def.api === "vad" ? local.vadParams : local.segmentation;
        if (src[s.def.field] === v) return false;
        src[s.def.field] = v;
        text(s.val, fmtSliderValue(s.def, v));
        if (s.track) {
            s.track.style.setProperty("--p", String(sliderPercent(s.def, v)));
            attr(s.track, "aria-valuenow", v);
        }
        return true;
    }

    function sliderValueFromEvent(s, e) {
        const rect = s.track.getBoundingClientRect();
        if (!(rect.width > 0)) return null;
        let p = (e.clientX - rect.left) / rect.width;
        p = p < 0 ? 0 : p > 1 ? 1 : p;
        const raw = s.def.min + p * (s.def.max - s.def.min);
        const q = 10 ** s.def.dp;
        return Math.round(raw * q) / q;
    }

    /**
     * 松手档(A-01):UI **不自建定时器去调 analyze** —— 300ms 防抖在 C++/mock
     * 侧;这里只补发尾值 + 显示「300ms 后应用…」倒计时条,等 §2.8 事件收尾。
     * 抑制期(J47)不显示倒计时 —— render 会亮出「应用到分段」显式按钮。
     */
    function releaseSlider(s) {
        if (local.paramTimer) {
            clearTimeout(local.paramTimer);
            local.paramTimer = 0;
        }
        local.lastParamSend = nowMs();
        sendParams(s.def.api);
        armCountdown(isSuppressed() ? null : s.def.api);
        requestRender();
    }

    /**
     * 倒计时条挂起/撤下(A-01)。正常收尾 = §2.8 事件到达(onSegments 清);
     * 兜底 2s 自撤:松手后才进入抑制期(J47)时 C++/mock 侧不跑流水线、
     * 事件不来,条不能挂死。
     */
    function armCountdown(api) {
        if (local.countdownTimer) clearTimeout(local.countdownTimer);
        local.countdownTimer = 0;
        local.countdownApi = api;
        if (api) {
            local.countdownTimer = setTimeout(() => {
                local.countdownTimer = 0;
                local.countdownApi = null;
                requestRender();
            }, 2000);
        }
    }

    /** diff 变更列表(A-02:一次性反馈,DIFF_HIDE_MS 后自动收起)。 */
    function setDiff(diff) {
        if (local.diffTimer) clearTimeout(local.diffTimer);
        local.diffTimer = 0;
        local.diff = diff || null;
        if (local.diff) {
            local.diffTimer = setTimeout(() => {
                local.diffTimer = 0;
                local.diff = null;
                requestRender();
            }, DIFF_HIDE_MS);
        }
    }

    // ---- previewAnalyze 节流(§1.5:纯只读 dry-run;A-07)---------------------
    function schedulePreview() {
        if (local.previewTimer) return;
        local.previewTimer = setTimeout(async () => {
            local.previewTimer = 0;
            const scope = currentScope();
            if (!scope) {
                if (local.preview) {
                    local.preview = null;
                    requestRender();
                }
                return;
            }
            const r = await call("previewAnalyze", scope);
            local.preview =
                r && Number.isFinite(r.intervals)
                    ? {
                          intervals: r.intervals,
                          tracks: r.tracks,
                          manualKept: r.manualKept,
                      }
                    : null;
            requestRender();
        }, PREVIEW_THROTTLE_MS);
    }

    // ---- 舞台坐标(C-04:舞台坐标系 = 容器宽 − 158 − 44;zoom 用比例抵消)----
    function stageXFromClient(clientX) {
        if (!els.lanes) return NaN;
        const rect = els.lanes.getBoundingClientRect();
        if (!(rect.width > 0)) return NaN;
        const sx = els.lanes.offsetWidth / rect.width;
        return (clientX - rect.left) * sx - HEAD_W;
    }

    function lanesPoint(e) {
        if (!els.lanes) return null;
        const rect = els.lanes.getBoundingClientRect();
        if (!(rect.width > 0) || !(rect.height > 0)) return null;
        const sx = els.lanes.offsetWidth / rect.width;
        const sy = els.lanes.offsetHeight / rect.height;
        const y = (e.clientY - rect.top) * sy + els.lanes.scrollTop;
        return {
            x: (e.clientX - rect.left) * sx - HEAD_W,
            y,
            ch: Math.floor(y / LANE_H) + 1,
        };
    }

    // ---- 工作选区(05 行 314;独立对象,不与 Range 双向同步)------------------
    function setSelection(startS, endS) {
        const d = timeline.durationS();
        const s0 = Math.min(Math.max(num(startS, 0), 0), d);
        const s1 = Math.min(Math.max(num(endS, 0), 0), d);
        local.selection = { startS: Math.min(s0, s1), endS: Math.max(s0, s1) };
        schedulePreview();
        requestRender();
    }

    function clearSelection() {
        if (!local.selection) return;
        local.selection = null;
        schedulePreview();
        requestRender();
    }

    // ---- 段选中(J67 唯一入口:泳道点选 + 段检查器)---------------------------
    function pickLane(ch, shiftKey) {
        local.lanePick = applyPick(local.lanePick, ch, shiftKey);
        schedulePreview();
        requestRender();
    }

    function selectSegment(ch, idx, extend) {
        const segCh = segmentsOfCh(getStore().segments, ch);
        const seg = ((segCh && segCh.segments) || [])[idx];
        if (!seg) return;
        const key = { idx, t0S: seg.t0S, t1S: seg.t1S };
        if (
            extend &&
            local.selectedCh === ch &&
            local.selectedSegs.length === 1 &&
            local.selectedSegs[0].idx !== idx
        ) {
            local.selectedSegs = [local.selectedSegs[0], key];
        } else {
            local.selectedCh = ch;
            local.selectedSegs = [key];
        }
        local.echo = {};
        requestRender();
    }

    function clearSegSelection() {
        if (!local.selectedSegs.length && !local.selectedCh) return;
        local.selectedCh = 0;
        local.selectedSegs = [];
        local.echo = {};
        requestRender();
    }

    /** 当前选中段(检查器/编辑的读取面;idx 已按最近一次事件重绑)。 */
    function currentSeg() {
        if (!local.selectedCh || !local.selectedSegs.length) return null;
        const segCh = segmentsOfCh(getStore().segments, local.selectedCh);
        const seg = ((segCh && segCh.segments) || [])[
            local.selectedSegs[0].idx
        ];
        return seg
            ? { ch: local.selectedCh, idx: local.selectedSegs[0].idx, seg }
            : null;
    }

    function canEditSelected() {
        const cur = currentSeg();
        return cur && !isWriteBlocked() && !isLaneDead(cur.ch) ? cur : null;
    }

    /** editSegment 统一出口(释放才发的口径由调用点保证;失败撤乐观值)。 */
    async function sendEdit(ch, op, payload) {
        const res = await call("editSegment", ch, op, payload);
        if (!(res && res.ok === true)) {
            local.echo = {};
            if (res && res.reason === "notAdjacent") {
                setToolbarNote("wave.notAdjacent");
            }
            requestRender();
        }
        return res;
    }

    // ---- 四动作 + 确认框(C-07 各钮各自判据;A-04/A-05)----------------------
    function scopeOrAll() {
        return currentScope() || "all";
    }

    async function doRecapture() {
        if (btnDisabled(els.btnRecapture) || isWriteBlocked()) return;
        const scope = currentScope();
        if (!scope) return;
        const autoStop = !!(els.autostop && els.autostop.checked);
        // §1.23:UI 以**返回值**点亮 badge,不乐观假设;armed:false 按 §5.5
        // 四值 reason 出行内说明、不点亮
        const res = await call(
            "recaptureArm",
            scope.tracksMask,
            scope.startS,
            scope.endS,
            autoStop,
        );
        if (res && res.armed) setToolbarNote(null);
        else if (res && res.reason) {
            setToolbarNote("wave.armReason." + res.reason);
        }
        requestRender();
    }

    async function doReanalyze() {
        if (btnDisabled(els.btnReanalyze) || isWriteBlocked()) return;
        const scope = currentScope();
        if (!scope) return;
        await call("analyze", scope); // 受理回执;结果经 §2.8 回推,运行态经 §2.1
        requestRender();
    }

    function openReidentifyConfirm() {
        if (btnDisabled(els.btnReidentify) || isWriteBlocked()) return;
        const scope = currentScope();
        const store = getStore();
        const counts = countsInScope(
            store.segments,
            scope ? scope.tracksMask : maskOfPicked(allLanes()),
            scope ? scope.startS : -Infinity,
            scope ? scope.endS : Infinity,
        );
        const body = els.confirmReidentify
            ? els.confirmReidentify.querySelector("[data-t]")
            : null;
        if (body) {
            body.textContent = fmtKey("wave.reidentifyConfirm", {
                k: counts.marks,
                l: counts.locked,
            });
        }
        show(els.confirmReidentify, true);
    }

    async function doReidentify() {
        show(els.confirmReidentify, false);
        if (isWriteBlocked()) return;
        await call("analyze", scopeOrAll(), { clearManual: true });
        requestRender();
    }

    function openClearConfirm() {
        if (btnDisabled(els.btnClear) || isWriteBlocked()) return;
        show(els.confirmClear, true);
    }

    async function doClearCoverage() {
        show(els.confirmClear, false);
        const scope = currentScope();
        if (!scope || isWriteBlocked()) return;
        const res = await call(
            "clearCoverage",
            scope.tracksMask,
            scope.startS,
            scope.endS,
        );
        if (res && res.ok) {
            // 契约 §1.24:清除后 UI 须重新 requestWaveform —— 块缓存整轨失效
            for (const ch of local.lanePick) waveSource.invalidate(ch);
            local.lastPaint.clear();
            local.staticDirty = true;
            setToolbarNote("wave.clearedCoverage", { s: res.clearedS });
            schedulePreview();
            schedulePaint();
        }
        requestRender();
    }

    async function doApplySegments() {
        // A-03:抑制期显式应用 = analyze(scope)(J47);分析进行中回 busy,忽略
        if (isWriteBlocked()) return;
        await call("analyze", scopeOrAll());
        requestRender();
    }

    async function doMerge() {
        if (local.selectedSegs.length !== 2) return;
        const cur = canEditSelected();
        if (!cur) return;
        const [a, b] = local.selectedSegs;
        await sendEdit(cur.ch, "merge", {
            segIdxA: Math.min(a.idx, b.idx),
            segIdxB: Math.max(a.idx, b.idx),
        });
    }

    async function doSetRange() {
        const sel = local.selection;
        if (!sel || !(sel.startS < sel.endS) || isWriteBlocked()) return;
        // 契约 §1.8 末句:Tab3「设为范围」以 mode="manual" 调用
        const res = await call("setRange", "manual", sel.startS, sel.endS);
        if (res && res.ok) {
            local.rangeTip = {
                x: fmtTimeMs(sel.startS),
                y: fmtTimeMs(sel.endS),
            };
        }
        requestRender();
    }

    function allLanes() {
        const out = [];
        for (let ch = 1; ch <= LANE_COUNT; ch++) out.push(ch);
        return out;
    }

    /** 布防 badge 点击 / 外部跳转:选区与勾选轨定位到布防面(05 行 300 ①)。 */
    function locateRecapture() {
        const rec = (getStore().state || {}).recapture || null;
        if (!rec || !rec.armed) return;
        const s0 = num(rec.startS, 0);
        const s1 = num(rec.endS, 0);
        if (s1 > s0) {
            setSelection(s0, s1);
            const vp = timeline.viewport();
            if (s0 < vp.startS || s1 > vp.endS) {
                const pad = (s1 - s0) * 0.25 + 1;
                timeline.set({ startS: s0 - pad, endS: s1 + pad });
            }
        }
        const mask = Math.trunc(num(rec.tracksMask, 0));
        const picked = [];
        for (let ch = 1; ch <= LANE_COUNT; ch++) {
            if (mask & (1 << (ch - 1))) picked.push(ch);
        }
        if (picked.length) local.lanePick = picked;
        schedulePreview();
        requestRender();
    }

    // ---------------------------------------------------------------- mount
    function mount() {
        els.panel = $("tab-wave");
        els.window = $("wave-window");
        els.lanes = $("wave-lanes");
        els.rulerScale = $("wave-ruler-scale");
        els.scalecol = $("wave-scalecol");
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
        els.btnMerge = $("wave-btn-mergesegs");
        els.armNote = $("wave-arm-note");
        els.countdown = $("wave-debounce-countdown");
        els.diff = $("wave-diff-list");
        els.diffKept = els.diff ? els.diff.querySelector("[data-t]") : null;
        els.diffItems = $("wave-diff-list-items");
        els.previewLine = $("wave-reanalyze-preview");
        els.rangetip = $("wave-selection-setrange-tip");
        els.rangetipText = els.rangetip
            ? els.rangetip.querySelector("[data-t]")
            : null;
        els.rangetipClose = $("wave-selection-setrange-tip-close");
        els.recapOverlap = $("wave-recapture-overlap");
        const autostopBox = $("wave-chk-autostop");
        els.autostop = autostopBox ? autostopBox.querySelector("input") : null;
        els.confirmReidentify = $("wave-confirm-reidentify");
        els.confirmReidentifyCancel = $("wave-confirm-reidentify-cancel");
        els.confirmReidentifyOk = $("wave-confirm-reidentify-ok");
        els.confirmClear = $("wave-confirm-clearcoverage");
        els.confirmClearCancel = $("wave-confirm-clearcoverage-cancel");
        els.confirmClearOk = $("wave-confirm-clearcoverage-ok");
        els.chipText = $("wave-selchip-text");
        els.chipRange = $("wave-selchip-range");
        els.chipList = $("wave-selchip-list");
        els.chipClear = $("wave-selchip-clear");
        els.selection = $("wave-selection-handles");
        els.selEdgeL = $("wave-selection-edge-left");
        els.selEdgeR = $("wave-selection-edge-right");
        els.selHandleL = $("wave-selection-handle-left");
        els.selHandleR = $("wave-selection-handle-right");
        els.selReadL = $("wave-selection-read-left");
        els.selReadR = $("wave-selection-read-right");
        els.setrange = $("wave-selection-setrange");
        els.dimL = $("wave-selection-dim-left");
        els.dimR = $("wave-selection-dim-right");
        els.selband = $("wave-selband");
        els.recapband = $("wave-recapband");
        els.bhandle = $("wave-boundary-handle");
        els.hscroll = $("wave-hscroll");
        els.emptyCta = $("wave-empty-cta");
        els.inspector = $("segment-inspector");
        els.inspNote = $("inspector-followhost-note");
        els.inspOrigin = $("inspector-origin-value");
        els.inspStart = $("inspector-time-start");
        els.inspEnd = $("inspector-time-end");
        els.inspLen = $("inspector-time-len");
        els.inspLoud = $("inspector-loudness-val");
        els.inspPanKnob = $("inspector-pan-knob");
        els.inspPanInput = $("inspector-pan-input");
        els.inspVolSlider = $("inspector-vol-slider");
        els.inspVolInput = $("inspector-vol-input");
        els.inspLock = $("inspector-locked-toggle");

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
                head: gb("head"),
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
            // 尺寸变化走整个 render():除 canvas 重绘外,还要补两件几何账 ——
            // ① 滚动条对基(syncScrollGutter);② 非激活期(display:none,
            // 量不到宽)攒下的 marksDirty,在切回本页尺寸恢复时重建角标。
            new ResizeObserver(() => {
                local.staticDirty = true;
                local.overlayDirty = true;
                render();
            }).observe(els.lanes);
        }
        // 后备存储重建闭环(05 §6.1:「dpr 变化(matchMedia('(resolution)'))
        // 触发重建」):拖到另一块屏/改系统缩放 → 全 canvas 按新 k 重绘;
        // 非激活期只落脏位(schedulePaint 早退),切回时补绘。
        observeResolution(() => {
            local.staticDirty = true;
            local.overlayDirty = true;
            schedulePaint();
        });

        // canvas 调色接真值(05 §6.1 收尾账):tokens.css → paintWaveTile palette
        local.palette = computedPalette();

        mountToolbar();
        mountLanesPointer();
        mountSelection();
        mountBottomBar();
        mountInspector();
        mountKeyboard();
        render();
    }

    /** tokens.css 真值 → canvas 调色(读不到就回 waveform.js 字面镜像)。 */
    function computedPalette() {
        try {
            if (
                typeof getComputedStyle !== "function" ||
                !root.documentElement
            ) {
                return null;
            }
            const cs = getComputedStyle(root.documentElement);
            const v = (n) => String(cs.getPropertyValue(n) || "").trim();
            const pal = {};
            if (v("--wave-env")) pal.env = v("--wave-env");
            // 透明度**从 waveform.js import**(本函数只换 rgb 真值)—— 两处各写
            // 一份字面 alpha 会静默失同步(对抗校验 major);envCore / vadEdge
            // 无对应 token,留给字面镜像走 spread 兜底。
            if (v("--sem-green")) {
                pal.vad = `rgba(${v("--sem-green")}, ${VAD_ALPHA})`;
            }
            if (v("--sem-amber")) pal.stale = `rgba(${v("--sem-amber")}, 0.22)`;
            if (v("--acc")) pal.coverage = `rgba(${v("--acc")}, 0.85)`;
            if (v("--wh")) {
                pal.uncovered = `rgba(${v("--wh")}, 0.05)`;
                pal.passTint = `rgba(${v("--wh")}, 0.03)`;
            }
            return Object.keys(pal).length ? pal : null;
        } catch {
            return null;
        }
    }

    // ------------------------------------------------------- mount:工具条接线
    function mountToolbar() {
        // 7 滑杆两段式:拖动档 ≤50Hz 整包预览;松手档只显示倒计时条,
        // **300ms 防抖在 C++ 侧,UI 不自建定时器去 analyze**(brief §0.5)
        for (const s of els.sliders || []) {
            if (!s.track) continue;
            s.track.addEventListener("pointerdown", (e) => {
                if (e.button !== 0 || isWriteBlocked()) return;
                e.preventDefault();
                local.sliderDrag = s;
                if (s.track.setPointerCapture) {
                    s.track.setPointerCapture(e.pointerId);
                }
                const v = sliderValueFromEvent(s, e);
                if (v !== null && setSliderValue(s, v)) {
                    sendParamsThrottled(s.def.api);
                }
                ensureTicker();
            });
            s.track.addEventListener("pointermove", (e) => {
                if (local.sliderDrag !== s) return;
                const v = sliderValueFromEvent(s, e);
                if (v !== null && setSliderValue(s, v)) {
                    sendParamsThrottled(s.def.api);
                }
            });
            const up = (e) => {
                if (local.sliderDrag !== s) return;
                local.sliderDrag = null;
                const v = sliderValueFromEvent(s, e);
                if (v !== null) setSliderValue(s, v);
                releaseSlider(s);
            };
            s.track.addEventListener("pointerup", up);
            s.track.addEventListener("pointercancel", up);
            // 键盘可达:方向键步进(dp0 → 1,dp2 → 0.01),静默 250ms 视为松手
            s.track.addEventListener("keydown", (e) => {
                const dir =
                    e.key === "ArrowRight" || e.key === "ArrowUp"
                        ? 1
                        : e.key === "ArrowLeft" || e.key === "ArrowDown"
                          ? -1
                          : 0;
                if (!dir || isWriteBlocked()) return;
                e.preventDefault();
                const step = s.def.dp > 0 ? 0.01 : 1;
                const src =
                    s.def.api === "vad" ? local.vadParams : local.segmentation;
                const cur = num(src[s.def.field], s.def.def);
                const q = 10 ** s.def.dp;
                const v =
                    Math.round(
                        Math.min(
                            Math.max(cur + dir * step, s.def.min),
                            s.def.max,
                        ) * q,
                    ) / q;
                if (setSliderValue(s, v)) sendParamsThrottled(s.def.api);
                if (local.sliderKeyTimer) clearTimeout(local.sliderKeyTimer);
                local.sliderKeyTimer = setTimeout(() => {
                    local.sliderKeyTimer = 0;
                    releaseSlider(s);
                }, SLIDER_KEY_RELEASE_MS);
            });
        }

        // 四动作 + 抑制期显式应用 + 合并(判据在 render,入口再复检)
        if (els.btnRecapture) {
            els.btnRecapture.addEventListener("click", doRecapture);
        }
        if (els.btnReanalyze) {
            els.btnReanalyze.addEventListener("click", doReanalyze);
        }
        if (els.btnReidentify) {
            els.btnReidentify.addEventListener("click", openReidentifyConfirm);
        }
        if (els.btnClear) {
            els.btnClear.addEventListener("click", openClearConfirm);
        }
        if (els.applyBtn) {
            els.applyBtn.addEventListener("click", () => {
                if (!btnDisabled(els.applyBtn)) doApplySegments();
            });
        }
        if (els.btnMerge) els.btnMerge.addEventListener("click", doMerge);

        // 二次确认框(A-04/A-05 深色玻璃)
        if (els.confirmReidentifyCancel) {
            els.confirmReidentifyCancel.addEventListener("click", () =>
                show(els.confirmReidentify, false),
            );
        }
        if (els.confirmReidentifyOk) {
            els.confirmReidentifyOk.addEventListener("click", doReidentify);
        }
        if (els.confirmClearCancel) {
            els.confirmClearCancel.addEventListener("click", () =>
                show(els.confirmClear, false),
            );
        }
        if (els.confirmClearOk) {
            els.confirmClearOk.addEventListener("click", doClearCoverage);
        }

        // 选区 chip 清除 ✕ / 「设为范围」提示条关闭 / 布防 badge 定位 / 空态 CTA
        if (els.chipClear) {
            els.chipClear.addEventListener("click", () => {
                local.lanePick = [];
                schedulePreview();
                requestRender();
            });
        }
        if (els.rangetipClose) {
            els.rangetipClose.addEventListener("click", () => {
                local.rangeTip = null;
                requestRender();
            });
        }
        if (els.recapBadge) {
            els.recapBadge.addEventListener("click", locateRecapture);
        }
        if (els.emptyCta) {
            els.emptyCta.addEventListener("click", () => {
                if (typeof opts.gotoTab === "function") opts.gotoTab("master");
            });
        }
    }

    // --------------------------------------------- mount:泳道区指针(舞台面)
    function mountLanesPointer() {
        if (!els.lanes || typeof els.lanes.addEventListener !== "function") {
            return;
        }
        // 轨头:整行点选语义落在轨头面(05 行 288);舞台面点选走 pointerup
        // 无位移路径(与平移拖拽在同一手势里定性)
        for (let ch = 1; ch <= LANE_COUNT; ch++) {
            const n = local.lanes.get(ch);
            if (!n) continue;
            if (n.head) {
                n.head.addEventListener("click", (e) => {
                    if (e.target && e.target.closest("button")) return;
                    pickLane(ch, e.shiftKey);
                });
            }
            if (n.check) {
                n.check.addEventListener("keydown", (e) => {
                    if (e.key === " " || e.key === "Enter") {
                        e.preventDefault();
                        pickLane(ch, e.shiftKey);
                    }
                });
            }
            if (n.eye) {
                n.eye.addEventListener("click", (e) => {
                    e.stopPropagation();
                    const on = n.eye.getAttribute("aria-pressed") !== "false";
                    n.eye.setAttribute("aria-pressed", on ? "false" : "true");
                    local.overlayDirty = true;
                    schedulePaint();
                });
            }
        }

        els.lanes.addEventListener("pointerdown", (e) => {
            if (e.button !== 0) return;
            const p = lanesPoint(e);
            if (!p || p.ch < 1 || p.ch > LANE_COUNT) return;
            const stageW = stageWidth();
            if (!(p.x >= 0 && p.x <= stageW)) return;
            const vp = timeline.viewport();
            // ① 边界命中(±6 CSS px,05 §6.3)→ 拖拽移动边界(释放才发)
            if (!isWriteBlocked() && !isLaneDead(p.ch)) {
                const segCh = segmentsOfCh(getStore().segments, p.ch);
                const segs = (segCh && segCh.segments) || [];
                const bounds = boundariesOf(segCh);
                const xs = bounds.map((b) => timeToX(vp, stageW, b.tS));
                const j = nearestHit(p.x, xs, BOUNDARY_HIT_PX);
                if (j >= 0) {
                    e.preventDefault();
                    if (els.lanes.setPointerCapture) {
                        els.lanes.setPointerCapture(e.pointerId);
                    }
                    local.boundDrag = {
                        ch: p.ch,
                        segIdx: j + 1, // 边界 j = 段 j+1 的 t0(edge:"t0")
                        tS: bounds[j].tS,
                        origT: bounds[j].tS, // 原位(拖动期动态层跳过原线画预览线)
                        snapped: false,
                        minS: num(segs[j] && segs[j].t0S, 0) + 0.05,
                        maxS:
                            num(
                                segs[j + 1] && segs[j + 1].t1S,
                                timeline.durationS(),
                            ) - 0.05,
                    };
                    local.overlayDirty = true;
                    schedulePaint();
                    ensureTicker();
                    return;
                }
            }
            // ② 其余:按下未定性 —— 位移 >3px 转空白平移(05 行 319),
            //    原地抬起转舞台点选(段 → 检查器;空白 → 勾选该轨)
            if (els.lanes.setPointerCapture) {
                els.lanes.setPointerCapture(e.pointerId);
            }
            local.pendingDown = {
                x: p.x,
                ch: p.ch,
                shift: e.shiftKey,
                moved: 0,
            };
        });

        els.lanes.addEventListener("pointermove", (e) => {
            if (local.boundDrag) {
                updateBoundDrag(e);
                return;
            }
            const p = lanesPoint(e);
            if (!p) return;
            if (local.pendingDown && !local.panDrag) {
                if (Math.abs(p.x - local.pendingDown.x) > 3) {
                    local.panDrag = { lastX: local.pendingDown.x };
                    local.pendingDown = null;
                    ensureTicker();
                }
            }
            if (local.panDrag) {
                const stageW = stageWidth();
                if (stageW > 0) {
                    const vp = timeline.viewport();
                    const dS =
                        ((local.panDrag.lastX - p.x) * spanOf(vp)) / stageW;
                    local.panDrag.lastX = p.x;
                    timeline.pan(dS);
                }
                return;
            }
            updateBoundHover(p);
        });

        const lanesUp = (e) => {
            if (local.boundDrag) {
                commitBoundDrag();
                return;
            }
            if (local.panDrag) {
                local.panDrag = null;
                return;
            }
            const pend = local.pendingDown;
            local.pendingDown = null;
            if (!pend) return;
            const p = lanesPoint(e);
            if (!p || p.ch !== pend.ch) return;
            stageClick(pend.ch, p.x, e);
        };
        els.lanes.addEventListener("pointerup", lanesUp);
        els.lanes.addEventListener("pointercancel", () => {
            local.boundDrag = null;
            local.panDrag = null;
            local.pendingDown = null;
            local.overlayDirty = true;
            schedulePaint();
        });
        els.lanes.addEventListener("pointerleave", () => {
            if (!local.boundDrag) showBoundHandle(false);
        });

        // 双击段内 = 在此分割(05 行 313;split 两子段继承原值,§5.4)
        els.lanes.addEventListener("dblclick", (e) => {
            const p = lanesPoint(e);
            if (!p || p.ch < 1 || p.ch > LANE_COUNT) return;
            const stageW = stageWidth();
            if (!(p.x >= 0 && p.x <= stageW)) return;
            if (isWriteBlocked() || isLaneDead(p.ch)) return;
            const vp = timeline.viewport();
            const t = xToTime(vp, stageW, p.x);
            const segCh = segmentsOfCh(getStore().segments, p.ch);
            const segs = (segCh && segCh.segments) || [];
            const idx = segs.findIndex((s) => t >= s.t0S && t < s.t1S);
            if (idx < 0) return;
            const s = segs[idx];
            if (!(t > s.t0S + 0.05 && t < s.t1S - 0.05)) return;
            sendEdit(p.ch, "split", {
                segIdx: idx,
                tS: Math.round(t * 1000) / 1000,
            });
        });

        // Ctrl+滚轮:以光标为中心缩放(05 行 319;挂窗级,标尺上也生效)
        const wheelHost = els.window || els.lanes;
        wheelHost.addEventListener(
            "wheel",
            (e) => {
                if (!e.ctrlKey) return;
                e.preventDefault();
                const stageW = stageWidth();
                if (!(stageW > 0)) return;
                const x = stageXFromClient(e.clientX);
                const vp = timeline.viewport();
                const anchorT = xToTime(
                    vp,
                    stageW,
                    Math.min(Math.max(x, 0), stageW),
                );
                timeline.zoom(
                    anchorT,
                    e.deltaY < 0 ? ZOOM_STEP : 1 / ZOOM_STEP,
                );
            },
            { passive: false },
        );
    }

    /** 舞台点选:段 → 选中 + 检查器(J67);空白 → 整行点选语义(05 行 288)。 */
    function stageClick(ch, x, e) {
        const stageW = stageWidth();
        if (!(stageW > 0)) return;
        const vp = timeline.viewport();
        const t = xToTime(vp, stageW, x);
        const segCh = segmentsOfCh(getStore().segments, ch);
        const segs = (segCh && segCh.segments) || [];
        const idx = segs.findIndex((s) => t >= s.t0S && t < s.t1S);
        if (idx >= 0) {
            selectSegment(ch, idx, e.ctrlKey || e.metaKey || e.shiftKey);
            // 点选段附带勾选该轨(只增不减 —— 再点段不该把轨取消勾选)
            if (local.lanePick.indexOf(ch) < 0) pickLane(ch, false);
        } else {
            clearSegSelection();
            pickLane(ch, e.shiftKey);
        }
    }

    // ---- 边界 hover 手柄(图例帧 767-772 的 9×26 两级权重;视觉浮标)---------
    function showBoundHandle(on) {
        show(els.bhandle, !!on);
        if (!on && els.lanes) els.lanes.style.cursor = "";
    }

    function updateBoundHover(p) {
        if (!els.bhandle || p.ch < 1 || p.ch > LANE_COUNT) {
            showBoundHandle(false);
            return;
        }
        const stageW = stageWidth();
        if (!(p.x >= 0 && p.x <= stageW)) {
            showBoundHandle(false);
            return;
        }
        const vp = timeline.viewport();
        const segCh = segmentsOfCh(getStore().segments, p.ch);
        const bounds = boundariesOf(segCh);
        const xs = bounds.map((b) => timeToX(vp, stageW, b.tS));
        const j = nearestHit(p.x, xs, BOUNDARY_HIT_PX);
        if (j < 0) {
            showBoundHandle(false);
            return;
        }
        els.bhandle.style.left = HEAD_W + xs[j] + "px";
        els.bhandle.style.top = (p.ch - 1) * LANE_H + (LANE_H - 26) / 2 + "px";
        attr(els.bhandle, "data-manual", bounds[j].manual ? 1 : 0);
        setTitle(els.bhandle, getT()["wave.boundaryHandleTip"] || "");
        show(els.bhandle, true);
        if (els.lanes) els.lanes.style.cursor = "ew-resize";
    }

    function updateBoundDrag(e) {
        const d = local.boundDrag;
        const stageW = stageWidth();
        if (!d || !(stageW > 0)) return;
        const vp = timeline.viewport();
        const x = stageXFromClient(e.clientX);
        let t = xToTime(vp, stageW, Math.min(Math.max(x, 0), stageW));
        // 吸附能量谷(valleys[] 来自当前块;Alt 关吸附,A-14)
        const cols = Math.min(Math.max(Math.round(stageW), 1), MAX_COLS);
        const tile = waveSource.peek(d.ch, vp.startS, vp.endS, cols);
        const pps = stageW / spanOf(vp);
        const snap = snapBoundary(t, tile && tile.valleys, pps, e.altKey);
        t = Math.min(Math.max(snap.tS, d.minS), d.maxS);
        if (t === d.tS && snap.snapped === d.snapped) return;
        d.tS = t;
        d.snapped = snap.snapped;
        // 9×26 手柄跟手(吸附命中态经 data-manual 之外的 data-snap 高亮,A-14)
        if (els.bhandle) {
            els.bhandle.style.left = HEAD_W + timeToX(vp, stageW, t) + "px";
            els.bhandle.style.top =
                (d.ch - 1) * LANE_H + (LANE_H - 26) / 2 + "px";
            attr(els.bhandle, "data-snap", d.snapped ? 1 : 0);
            show(els.bhandle, true);
        }
        local.overlayDirty = true;
        schedulePaint();
    }

    /** 边界拖拽提交:**释放才发**(契约 §1.22 线程栏;拖动中纯本地重绘)。 */
    function commitBoundDrag() {
        const d = local.boundDrag;
        local.boundDrag = null;
        showBoundHandle(false);
        if (els.bhandle) attr(els.bhandle, "data-snap", 0);
        local.overlayDirty = true;
        schedulePaint();
        if (!d) return;
        // **零位移不发**(同 tab-tracks 拖拽路径的空转短路):边界 ±6px 命中区在
        // 15 泳道密集边界上一点就中,「点一下不拖」若照发 move_boundary,契约 §5.4
        // 的后置会把该 auto 段变成 origin=user_edited + locked=true 并压入撤销栈 ——
        // 而 analyze(scope,{clearManual:true}) 对 locked 段免疫(须先逐段解锁),
        // 于是一次误点就把这段永久钉死,用户没有任何可见反馈。按**下发口径**
        // (ms 量化后)比原位,子毫秒抖动同样算零位移。
        const tS = Math.round(d.tS * 1000) / 1000;
        if (tS === Math.round(d.origT * 1000) / 1000) return;
        sendEdit(d.ch, "move_boundary", {
            segIdx: d.segIdx,
            edge: "t0",
            tS,
        });
    }

    // ------------------------------------------------ mount:工作选区(§10)
    function mountSelection() {
        // 标尺拖出选区(选区件坐落在标尺行的设计几何;05 未给创建手势,
        // 取 DAW 惯例「时间标尺上拖拽框选」,deviations 登记)
        if (els.rulerScale) {
            els.rulerScale.addEventListener("pointerdown", (e) => {
                if (e.button !== 0) return;
                const stageW = stageWidth();
                if (!(stageW > 0)) return;
                e.preventDefault();
                if (els.rulerScale.setPointerCapture) {
                    els.rulerScale.setPointerCapture(e.pointerId);
                }
                const vp = timeline.viewport();
                const t = xToTime(
                    vp,
                    stageW,
                    Math.min(Math.max(stageXFromClient(e.clientX), 0), stageW),
                );
                local.selDrag = "create";
                local.selAnchorT = t;
                setSelection(t, t);
                ensureTicker();
            });
            els.rulerScale.addEventListener("pointermove", (e) => {
                if (local.selDrag !== "create") return;
                const stageW = stageWidth();
                if (!(stageW > 0)) return;
                const vp = timeline.viewport();
                const t = xToTime(
                    vp,
                    stageW,
                    Math.min(Math.max(stageXFromClient(e.clientX), 0), stageW),
                );
                setSelection(local.selAnchorT, t);
            });
            const rulerUp = () => {
                if (local.selDrag !== "create") return;
                local.selDrag = null;
                const sel = local.selection;
                // 原地点击(跨度 <0.1s)= 清除选区
                if (sel && sel.endS - sel.startS < 0.1) clearSelection();
                else requestRender();
            };
            els.rulerScale.addEventListener("pointerup", rulerUp);
            els.rulerScale.addEventListener("pointercancel", rulerUp);
        }

        // 两端手柄拖拽(9×26,B-01;双读数展开 + 边线发光由 render 投影)
        for (const side of ["L", "R"]) {
            const h = side === "L" ? els.selHandleL : els.selHandleR;
            if (!h) continue;
            h.addEventListener("pointerdown", (e) => {
                if (e.button !== 0 || !local.selection) return;
                e.preventDefault();
                e.stopPropagation();
                if (h.setPointerCapture) h.setPointerCapture(e.pointerId);
                local.selDrag = side;
                requestRender();
                ensureTicker();
            });
            h.addEventListener("pointermove", (e) => {
                if (local.selDrag !== side || !local.selection) return;
                const stageW = stageWidth();
                if (!(stageW > 0)) return;
                const vp = timeline.viewport();
                const t = xToTime(
                    vp,
                    stageW,
                    Math.min(Math.max(stageXFromClient(e.clientX), 0), stageW),
                );
                const sel = local.selection;
                if (side === "L") {
                    setSelection(Math.min(t, sel.endS - 0.05), sel.endS);
                } else {
                    setSelection(sel.startS, Math.max(t, sel.startS + 0.05));
                }
            });
            const up = () => {
                if (local.selDrag !== side) return;
                local.selDrag = null;
                schedulePreview();
                requestRender();
            };
            h.addEventListener("pointerup", up);
            h.addEventListener("pointercancel", up);
            // 键盘可达:方向键微调该侧(1% 视口跨度)
            h.addEventListener("keydown", (e) => {
                const dir =
                    e.key === "ArrowRight" ? 1 : e.key === "ArrowLeft" ? -1 : 0;
                if (!dir || !local.selection) return;
                e.preventDefault();
                const step = spanOf(timeline.viewport()) / 100;
                const sel = local.selection;
                if (side === "L") {
                    setSelection(
                        Math.min(sel.startS + dir * step, sel.endS - 0.05),
                        sel.endS,
                    );
                } else {
                    setSelection(
                        sel.startS,
                        Math.max(sel.endS + dir * step, sel.startS + 0.05),
                    );
                }
            });
        }

        if (els.setrange) {
            els.setrange.addEventListener("click", () => {
                if (!btnDisabled(els.setrange)) doSetRange();
            });
        }
    }

    // -------------------------------------------- mount:底部缩放/滚动条(§14)
    function mountBottomBar() {
        if (!els.thumb || !els.hscroll) return;
        els.thumb.addEventListener("pointerdown", (e) => {
            if (e.button !== 0) return;
            e.preventDefault();
            e.stopPropagation();
            if (els.thumb.setPointerCapture) {
                els.thumb.setPointerCapture(e.pointerId);
            }
            local.thumbDrag = { startX: e.clientX, vp0: timeline.viewport() };
            ensureTicker();
        });
        els.thumb.addEventListener("pointermove", (e) => {
            const d = local.thumbDrag;
            if (!d) return;
            const rect = els.hscroll.getBoundingClientRect();
            if (!(rect.width > 0)) return;
            const frac = (e.clientX - d.startX) / rect.width;
            const dS = frac * timeline.durationS();
            timeline.set({
                startS: d.vp0.startS + dS,
                endS: d.vp0.endS + dS,
            });
        });
        const up = () => {
            local.thumbDrag = null;
        };
        els.thumb.addEventListener("pointerup", up);
        els.thumb.addEventListener("pointercancel", up);
        // 轨道空白点击:视口中心跳到该处(跨度不变)
        els.hscroll.addEventListener("pointerdown", (e) => {
            if (e.button !== 0 || e.target === els.thumb) return;
            const rect = els.hscroll.getBoundingClientRect();
            if (!(rect.width > 0)) return;
            const frac = (e.clientX - rect.left) / rect.width;
            const vp = timeline.viewport();
            const half = spanOf(vp) / 2;
            const mid = frac * timeline.durationS();
            timeline.set({ startS: mid - half, endS: mid + half });
        });
    }

    // ------------------------------------------------ mount:段检查器(§17)
    function mountInspector() {
        // PAN:数字输入(step 1 / shift 0.1,05 §2.3a)——提交在 Enter/失焦
        if (els.inspPanInput) {
            wireNumInput(els.inspPanInput, {
                min: -100,
                max: 100,
                step: 1,
                shiftStep: 0.1,
                dp: 1,
                commit: (v) => commitSegValues({ pan: v }),
            });
        }
        // VOL:数字输入(step 0.1)
        if (els.inspVolInput) {
            wireNumInput(els.inspVolInput, {
                min: -24,
                max: 12,
                step: 0.1,
                shiftStep: 0.1,
                dp: 1,
                commit: (v) => commitSegValues({ volDb: v }),
            });
        }
        // PAN 微型旋钮:垂直拖拽,**释放才发**(乐观值走 local.echo)
        if (els.inspPanKnob) {
            els.inspPanKnob.addEventListener("pointerdown", (e) => {
                const cur = canEditSelected();
                if (e.button !== 0 || !cur) return;
                e.preventDefault();
                if (els.inspPanKnob.setPointerCapture) {
                    els.inspPanKnob.setPointerCapture(e.pointerId);
                }
                local.knobDrag = {
                    startY: e.clientY,
                    startVal: num(local.echo.pan, num(cur.seg.pan, 0)),
                };
                ensureTicker();
            });
            els.inspPanKnob.addEventListener("pointermove", (e) => {
                const d = local.knobDrag;
                if (!d) return;
                const v = Math.min(
                    Math.max(d.startVal + (d.startY - e.clientY) * 0.8, -100),
                    100,
                );
                local.echo.pan = Math.round(v * 10) / 10;
                requestRender();
            });
            const up = () => {
                if (!local.knobDrag) return;
                local.knobDrag = null;
                if (Number.isFinite(local.echo.pan)) {
                    commitSegValues({ pan: local.echo.pan });
                }
            };
            els.inspPanKnob.addEventListener("pointerup", up);
            els.inspPanKnob.addEventListener("pointercancel", up);
        }
        // VOL 微型滑杆:水平拖拽,释放才发
        if (els.inspVolSlider) {
            const valFrom = (e) => {
                const rect = els.inspVolSlider.getBoundingClientRect();
                if (!(rect.width > 0)) return null;
                let p = (e.clientX - rect.left) / rect.width;
                p = p < 0 ? 0 : p > 1 ? 1 : p;
                return Math.round((-24 + p * 36) * 10) / 10;
            };
            els.inspVolSlider.addEventListener("pointerdown", (e) => {
                const cur = canEditSelected();
                if (e.button !== 0 || !cur) return;
                e.preventDefault();
                if (els.inspVolSlider.setPointerCapture) {
                    els.inspVolSlider.setPointerCapture(e.pointerId);
                }
                local.vslDrag = {};
                const v = valFrom(e);
                if (v !== null) {
                    local.echo.vol = v;
                    requestRender();
                }
                ensureTicker();
            });
            els.inspVolSlider.addEventListener("pointermove", (e) => {
                if (!local.vslDrag) return;
                const v = valFrom(e);
                if (v !== null) {
                    local.echo.vol = v;
                    requestRender();
                }
            });
            const up = () => {
                if (!local.vslDrag) return;
                local.vslDrag = null;
                if (Number.isFinite(local.echo.vol)) {
                    commitSegValues({ volDb: local.echo.vol });
                }
            };
            els.inspVolSlider.addEventListener("pointerup", up);
            els.inspVolSlider.addEventListener("pointercancel", up);
        }
        // 锁定 toggle(set_locked;**不改 origin**,§5.4)
        if (els.inspLock) {
            const flip = () => {
                const cur = canEditSelected();
                if (!cur) return;
                sendEdit(cur.ch, "set_locked", {
                    segIdx: cur.idx,
                    locked: !cur.seg.locked,
                });
            };
            els.inspLock.addEventListener("click", flip);
            els.inspLock.addEventListener("keydown", (e) => {
                if (e.key === " " || e.key === "Enter") {
                    e.preventDefault();
                    flip();
                }
            });
        }
    }

    /** 检查器数字输入通用接线:Enter/失焦提交,方向键步进后同径提交。 */
    function wireNumInput(input, spec) {
        const commit = () => {
            const cur = canEditSelected();
            if (!cur) return;
            const v = parseFloat(String(input.value).replace(",", "."));
            if (!Number.isFinite(v)) {
                requestRender(); // 回显当前值,丢弃非法输入
                return;
            }
            const q = 10 ** spec.dp;
            spec.commit(
                Math.round(Math.min(Math.max(v, spec.min), spec.max) * q) / q,
            );
        };
        input.addEventListener("keydown", (e) => {
            if (e.key === "Enter") {
                e.preventDefault();
                commit();
                input.blur();
                return;
            }
            const dir =
                e.key === "ArrowUp" ? 1 : e.key === "ArrowDown" ? -1 : 0;
            if (!dir) return;
            e.preventDefault();
            const step = e.shiftKey ? spec.shiftStep : spec.step;
            const base = parseFloat(String(input.value).replace(",", "."));
            const v = Number.isFinite(base) ? base + dir * step : 0;
            const q = 10 ** spec.dp;
            input.value = fmtSigned(
                Math.min(Math.max(v, spec.min), spec.max),
                spec.dp,
            );
            void q;
        });
        input.addEventListener("blur", commit);
    }

    async function commitSegValues(patch) {
        const cur = canEditSelected();
        if (!cur) return;
        await sendEdit(cur.ch, "set_values", { segIdx: cur.idx, ...patch });
    }

    // ------------------------------------------------- mount:键盘(页级)
    function mountKeyboard() {
        if (!root || typeof root.addEventListener !== "function") return;
        root.addEventListener("keydown", (e) => {
            if (!isPanelActive()) return;
            const a = root.activeElement;
            const inField =
                a &&
                (a.tagName === "INPUT" ||
                    a.tagName === "TEXTAREA" ||
                    a.isContentEditable);
            if (e.key === "Escape") {
                if (
                    (els.confirmReidentify && !els.confirmReidentify.hidden) ||
                    (els.confirmClear && !els.confirmClear.hidden)
                ) {
                    show(els.confirmReidentify, false);
                    show(els.confirmClear, false);
                } else if (local.selectedSegs.length) {
                    clearSegSelection();
                } else if (local.selection) {
                    clearSelection();
                }
                return;
            }
            // 选中相邻两段 Delete = 合并(05 行 313;notAdjacent 行内反馈)
            if ((e.key === "Delete" || e.key === "Backspace") && !inField) {
                if (local.selectedSegs.length === 2) {
                    e.preventDefault();
                    doMerge();
                }
            }
        });
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
        // 切 tab 由 app.js 的 activateTab 补一次整页 render —— 播放头插值与帧时账
        // 的「按需起帧」配对不变式接在这里(见 onPlayhead / ensureTicker)。
        resumePlayhead();
        timeline.setDuration(durationOf(store));
        const vp = timeline.viewport();
        const stageW = stageWidth();
        syncScrollGutter();

        // ui.scale 档位变化 = 后备存储 k 变(05 §6.1)→ 全 canvas 标脏重建;
        // dpr 侧的同款触发走 mount 里的 observeResolution。
        const uiScale = num(((store.state || {}).ui || {}).scale, 1);
        if (local.lastUiScale !== uiScale) {
            local.lastUiScale = uiScale;
            local.staticDirty = true;
            local.overlayDirty = true;
        }

        // ---- 空态(data-empty 驱动泳道/空态互斥;标尺与底部条仍在,§15)
        attr(els.window, "data-empty", isLanesEmpty(store) ? 1 : 0);

        // ---- 轨头六件(15 行)
        const lanes = laneModelFromStore(store);
        for (const lane of lanes) {
            const n = local.lanes.get(lane.n);
            if (!n) continue;
            attr(n.row, "data-status", lane.status);
            // 点选态(05 行 288:整行高亮 + 复选框勾选;lanePick 是交互态真源)
            const on = local.lanePick.indexOf(lane.n) >= 0 ? 1 : 0;
            attr(n.row, "data-on", on);
            if (n.check) attr(n.check, "aria-checked", on ? "true" : "false");
            const vis = statusVisual(lane.status);
            attr(n.light, "data-tone", vis.tone);
            attr(n.light, "data-pulse", vis.pulse ? 1 : 0);
            setTitle(n.light, t[vis.key] || "");
            const labelText = lane.label || labelPlaceholder(lane.n, t);
            text(n.label, labelText);
            // 158px 轨头装六件,长轨名仍可能省略 —— 全文走 title 兜住(covseg 同理)
            setTitle(n.label, labelText);
            const covSeg = fmtKey("wave.covSeg", {
                p: lane.cov,
                n: lane.segs,
            });
            text(n.covseg, covSeg);
            setTitle(n.covseg, covSeg);
            show(n.low, !!lane.low);
            setTitle(n.low, lane.low ? t["lowSample.full"] || "" : "");
            if (n.check) {
                n.check.setAttribute(
                    "aria-label",
                    fmtKey("wave.pickTrack", { n: tt(lane.n) }),
                );
            }
        }

        // ---- 工具条:滑杆读数(本地整包缓存 = §1.18 五字段 / §1.19 整包底账)。
        //      state.analysis 整组回读(契约 §2.1;mock 默认值已统一 05 口径,
        //      02 旧口径差异登记 deviations 供复核):拖动/键盘档进行中乐观值
        //      优先,静默后以 state 整包覆盖 —— 切 tab/重开面板不丢参数。
        if (!local.sliderDrag && !local.sliderKeyTimer) {
            const ana = (store.state || {}).analysis || {};
            syncParamGroup(local.vadParams, ana.vad);
            syncParamGroup(local.segmentation, ana.segmentation);
        }
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

        // ---- 四钮判据(C-07:各钮各自的 05 判据,行 300-303)
        const scope = currentScope();
        const blocked = isWriteBlocked();
        const hasData = !isLanesEmpty(store);
        // 重分析:「选区 ∩ coverage = ∅ 时 disabled」—— 判据取 §1.5 dry-run 的
        // intervals(mock affectedOf 已按 coverage 口径);回包在途时先放行,
        // 到达后收敛(analyze 对空交集也会以 {ok:false} 拒绝,双保险)。
        const previewHit = local.preview ? local.preview.intervals > 0 : true;
        setBtnEnabled(els.btnRecapture, !!scope && !blocked);
        setBtnEnabled(
            els.btnReanalyze,
            !!scope && !blocked && hasData && previewHit,
        );
        setBtnEnabled(
            els.btnReidentify,
            hasNonAutoSegments(store.segments) && !blocked,
        );
        setBtnEnabled(els.btnClear, !!scope && !blocked && hasData);

        // ---- 轨选提示 ↔ 选区 chip 互斥切换(05 行 288;B-02/B-03 裁定:
        //      词条只含 {n} 轨句,范围 mono 串独立置于轨号串前,轨号 · 分隔)
        const picked = local.lanePick;
        show(els.hint, picked.length === 0);
        show(els.chip, picked.length > 0);
        if (picked.length) {
            text(els.chipText, fmtKey("wave.selChip", { n: picked.length }));
            const sel = local.selection;
            text(
                els.chipRange,
                sel && sel.endS > sel.startS
                    ? `${fmtTimeMs(sel.startS)}–${fmtTimeMs(sel.endS)}`
                    : "",
            );
            text(
                els.chipList,
                [...picked]
                    .sort((a, b) => a - b)
                    .map((c) => tt(c))
                    .join(" · "),
            );
        }

        // ---- 「正在应用…」菊花(契约 §2.2:analysis_run.running 驱动)
        const running = !!(
            (store.state || {}).analysis_run && store.state.analysis_run.running
        );
        show(els.applying, running);

        // ---- 三段式①「300ms 后应用…」倒计时条(A-01):松手挂起、§2.8 收尾;
        //      菊花亮起后让位(三段式②)
        show(els.countdown, !!local.countdownApi && !running);

        // ---- 「应用到分段」抑制期按钮(A-03/J47:**只有** PRINT 态或分析进行中)
        const suppressed = isSuppressed(store);
        show(els.applyBtn, suppressed);
        if (suppressed) setBtnEnabled(els.applyBtn, !running && !blocked);

        // ---- 工具条合并入口(05 行 313:选中相邻两段;Delete 同径)
        show(els.btnMerge, local.selectedSegs.length === 2);
        if (local.selectedSegs.length === 2) {
            setBtnEnabled(
                els.btnMerge,
                !blocked && !isLaneDead(local.selectedCh),
            );
        }

        // ---- 行内反馈(§5.5 armReason 四值 / notAdjacent / 清除回执)
        if (local.toolbarNote) {
            text(
                els.armNote,
                fmtKey(local.toolbarNote.key, local.toolbarNote.vals),
            );
            show(els.armNote, true);
        } else {
            show(els.armNote, false);
        }

        // ---- A-07 重分析影响预览行(§1.5 节流 dry-run;句式 = master.step2.desc)
        if (scope && local.preview) {
            text(
                els.previewLine,
                fmtKey("master.step2.desc", {
                    n: local.preview.intervals,
                    m: local.preview.tracks,
                    k: local.preview.manualKept,
                }),
            );
            show(els.previewLine, true);
        } else {
            show(els.previewLine, false);
        }

        // ---- A-02 diff 变更列表(首行 wave.diffKept;一次性反馈自动收起)
        if (local.diff) {
            if (els.diffKept) {
                text(
                    els.diffKept,
                    fmtKey("wave.diffKept", { k: num(local.diff.kept, 0) }),
                );
            }
            renderDiffItems(local.diff);
            show(els.diff, true);
        } else {
            show(els.diff, false);
        }

        // ---- 「设为范围」一次性提示条(§7:wave.setRangeTip,✕ 关闭)
        if (local.rangeTip) {
            if (els.rangetipText) {
                text(
                    els.rangetipText,
                    fmtKey("wave.setRangeTip", local.rangeTip),
                );
            }
            show(els.rangetip, true);
        } else {
            show(els.rangetip, false);
        }

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
            // 05 行 300:「将覆盖 {k} 段已有数据」(布防面内相交段计数)
            const counts = countsInScope(
                store.segments,
                mask,
                num(rec.startS, 0),
                num(rec.endS, 0),
            );
            if (counts.overlap > 0) {
                text(
                    els.recapOverlap,
                    fmtKey("wave.recaptureOverlap", { k: counts.overlap }),
                );
            }
            show(els.recapOverlap, counts.overlap > 0);
            // 布防勾选回显(scvb.state.recapture.autoStop;切 tab/重开不丢)
            if (els.autostop) els.autostop.checked = !!rec.autoStop;
        }
        // 布防受覆盖区域斜条纹(05 行 300;A-08 同族配方,滚动层内)
        if (armed && stageW > 0) {
            const rx0 = Math.max(timeToX(vp, stageW, num(rec.startS, 0)), 0);
            const rx1 = Math.min(timeToX(vp, stageW, num(rec.endS, 0)), stageW);
            const runs = maskRuns(rec.tracksMask);
            if (rx1 > rx0 && runs.length && els.recapband) {
                els.recapband.style.left = HEAD_W + rx0 + "px";
                els.recapband.style.width = rx1 - rx0 + "px";
                els.recapband.style.height = LANE_COUNT * LANE_H + "px";
                // 只在**布防轨**上画条纹:容器仍是满高(供 top 定位),条纹落在
                // 每段相邻布防轨的子块里(签名比对,避免逐帧重建 innerHTML)
                const sig = runs.map((r) => r.ch0 + "x" + r.count).join(",");
                if (els.recapband.getAttribute("data-runs") !== sig) {
                    els.recapband.setAttribute("data-runs", sig);
                    els.recapband.innerHTML = runs
                        .map(
                            (r) =>
                                `<span class="wave-recapband__seg" style="top:${(r.ch0 - 1) * LANE_H}px;height:${r.count * LANE_H}px"></span>`,
                        )
                        .join("");
                }
                show(els.recapband, true);
            } else {
                show(els.recapband, false);
            }
        } else {
            show(els.recapband, false);
        }

        // ---- Range 只读细线(05 行 290/314;A-10:follow 不画)
        const range = ((store.state || {}).global || {}).range || {};
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

        // ---- 段角标 DOM(segments 事件后标脏才重建,避免逐拍重建 innerHTML)。
        //      stageW=0(非激活面板 display:none)时保留脏位不消费 —— 口径同
        //      schedulePaint 的 isPanelActive 早退;否则 timeToX 全 0 会把
        //      NaN% 拼进 style 并让越界过滤失效。切回本页时 ResizeObserver
        //      的 render() 补建。
        if (local.marksDirty && stageW > 0) {
            local.marksDirty = false;
            renderSegMarks(store, vp, stageW);
        }

        // ---- 工作选区叠加层 + 选区外压暗(§10 / A-09)与段检查器(§17)
        renderSelection(vp, stageW);
        renderInspector(store);

        schedulePaint();
    }

    /** state.analysis 整组回读进本地整包缓存(字段级校验,mode 是字符串)。 */
    function syncParamGroup(dst, src) {
        if (!src) return;
        for (const k of Object.keys(dst)) {
            const v = src[k];
            if (k === "mode") {
                if (typeof v === "string" && v) dst[k] = v;
            } else if (Number.isFinite(v)) {
                dst[k] = v;
            }
        }
    }

    /** A-02 条目区:changed 明细 + added/removed 摘要(mono 数值行)。 */
    function renderDiffItems(diff) {
        if (!els.diffItems) return;
        let html = "";
        for (const c of diff.changed || []) {
            if (!c) continue;
            html += `<li>${esc(
                fmtKey("wave.diffItem", {
                    ch: tt(num(c.ch, 0)),
                    i: num(c.segIdx, 0) + 1,
                    pf: fmtSigned(c.panFrom, 1),
                    pt: fmtSigned(c.panTo, 1),
                    vf: fmtSigned(c.volDbFrom, 1),
                    vt: fmtSigned(c.volDbTo, 1),
                }),
            )}</li>`;
        }
        const a = num(diff.added, 0);
        const r = num(diff.removed, 0);
        if (a > 0 || r > 0) {
            html += `<li>${esc(
                fmtKey("wave.diffAddedRemoved", { a, r }),
            )}</li>`;
        }
        if (els.diffItems.innerHTML !== html) els.diffItems.innerHTML = html;
    }

    /**
     * 工作选区投影(§10):窗级叠加层 left/width 按舞台坐标写(C-04);
     * 拖动态 data-drag 驱动边线发光/手柄高亮/读数展开/「设为范围」淡出;
     * 选区带浅琥珀底(滚动层内)+ 选区外两片压暗(A-09)。
     */
    function renderSelection(vp, stageW) {
        const sel = local.selection;
        const hideAll = () => {
            show(els.selection, false);
            show(els.selband, false);
            show(els.dimL, false);
            show(els.dimR, false);
        };
        if (!sel || !(stageW > 0) || !(sel.endS > sel.startS)) {
            hideAll();
            return;
        }
        const cx0 = Math.max(timeToX(vp, stageW, sel.startS), 0);
        const cx1 = Math.min(timeToX(vp, stageW, sel.endS), stageW);
        if (!(cx1 > cx0)) {
            hideAll();
            return;
        }
        const dragSide =
            local.selDrag === "L" || local.selDrag === "R"
                ? local.selDrag
                : local.selDrag === "create"
                  ? "R"
                  : null;
        if (els.selection) {
            els.selection.style.left = HEAD_W + cx0 + "px";
            els.selection.style.width = cx1 - cx0 + "px";
            attr(els.selection, "data-drag", dragSide ? 1 : 0);
            show(els.selection, true);
        }
        attr(els.selEdgeL, "data-drag", dragSide === "L" ? 1 : 0);
        attr(els.selEdgeR, "data-drag", dragSide === "R" ? 1 : 0);
        attr(els.selHandleL, "data-drag", dragSide === "L" ? 1 : 0);
        attr(els.selHandleR, "data-drag", dragSide === "R" ? 1 : 0);
        attr(els.selReadL, "data-drag", dragSide === "L" ? 1 : 0);
        attr(els.selReadR, "data-drag", dragSide === "R" ? 1 : 0);
        // 双读数(05 行 314 口径,B-12:主显 mm:ss.mmm;v1 无 tempo 表,
        // 拖动侧不另拼小节值,展开态 = 高亮配色)
        text(els.selReadL, fmtTimeMs(sel.startS));
        text(els.selReadR, fmtTimeMs(sel.endS));
        setBtnEnabled(els.setrange, !isWriteBlocked());
        const contentH = LANE_COUNT * LANE_H;
        if (els.selband) {
            els.selband.style.left = HEAD_W + cx0 + "px";
            els.selband.style.width = cx1 - cx0 + "px";
            els.selband.style.height = contentH + "px";
            show(els.selband, true);
        }
        if (els.dimL) {
            if (cx0 > 0) {
                els.dimL.style.left = HEAD_W + "px";
                els.dimL.style.width = cx0 + "px";
                show(els.dimL, true);
            } else {
                show(els.dimL, false);
            }
        }
        if (els.dimR) {
            if (cx1 < stageW) {
                els.dimR.style.left = HEAD_W + cx1 + "px";
                els.dimR.style.width = stageW - cx1 + "px";
                show(els.dimR, true);
            } else {
                show(els.dimR, false);
            }
        }
    }

    /**
     * 段检查器投影(§17):点选段展开(J67 唯一入口);pan/vol 显示乐观回声
     * 优先(local.echo,§2.8 事件后失效);锁定 toggle / origin 角标 / B-12
     * mm:ss.mmm 主显 / A-18 跟随宿主提示(输出开关 OFF 即显,05 §2.3a 350)。
     */
    function renderInspector(store) {
        const cur = currentSeg();
        show(els.inspector, !!cur);
        if (!cur) return;
        const seg = cur.seg;
        show(els.inspNote, !((store.state || {}).global || {}).output_enabled);
        if (seg.origin === "user_edited" || seg.origin === "user_created") {
            text(els.inspOrigin, seg.origin === "user_edited" ? "E" : "C");
            show(els.inspOrigin, true);
        } else {
            show(els.inspOrigin, false);
        }
        text(els.inspStart, fmtTimeMs(seg.t0S));
        text(els.inspEnd, fmtTimeMs(seg.t1S));
        text(els.inspLen, `${(num(seg.t1S, 0) - num(seg.t0S, 0)).toFixed(2)}s`);
        text(
            els.inspLoud,
            Number.isFinite(seg.loudnessLufs)
                ? seg.loudnessLufs.toFixed(1)
                : "--",
        );
        const editable = !isWriteBlocked() && !isLaneDead(cur.ch);
        const pan = num(local.echo.pan, num(seg.pan, 0));
        const vol = num(local.echo.vol, num(seg.volDb, 0));
        if (els.inspPanKnob) {
            els.inspPanKnob.style.setProperty(
                "--ang",
                `${((pan / 100) * 140).toFixed(1)}deg`,
            );
            attr(els.inspPanKnob, "aria-valuenow", Math.round(pan));
            attr(els.inspPanKnob, "aria-disabled", editable ? "false" : "true");
        }
        if (els.inspPanInput) {
            if (root.activeElement !== els.inspPanInput) {
                els.inspPanInput.value = fmtSigned(pan, 1);
            }
            els.inspPanInput.disabled = !editable;
        }
        if (els.inspVolSlider) {
            els.inspVolSlider.style.setProperty("--p", String(volPercent(vol)));
            attr(els.inspVolSlider, "aria-valuenow", vol);
            attr(
                els.inspVolSlider,
                "aria-disabled",
                editable ? "false" : "true",
            );
        }
        if (els.inspVolInput) {
            if (root.activeElement !== els.inspVolInput) {
                els.inspVolInput.value = fmtSigned(vol, 1);
            }
            els.inspVolInput.disabled = !editable;
        }
        if (els.inspLock) {
            attr(els.inspLock, "data-on", seg.locked ? 1 : 0);
            attr(els.inspLock, "aria-checked", seg.locked ? "true" : "false");
            attr(els.inspLock, "aria-disabled", editable ? "false" : "true");
        }
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

    /**
     * 段角标(E/C + 锁定)重建;词条 wave.lockBadge 随语言切换(render 会再进来)。
     * **同一段起点的角标合成一组**(flex 行,只画一次):图例帧 774-779 的口径是
     * 「E/C 薰衣草实心 chip」与「另挂的琥珀锁形小标」两件并列,不是一枚大胶囊。
     * 锁标在 34px 实景泳道里只留 stroke 锁形,「锁定」词条走 aria-label(deviations)。
     */
    function renderSegMarks(store, vp, stageW) {
        const t = getT();
        const tip = esc(t["wave.lockBadge"] || "");
        const lockHtml = `<span class="wave-seg-lock" role="img" aria-label="${tip}" title="${tip}"><svg width="6" height="6" viewBox="0 0 12 12" aria-hidden="true"><rect x="2.4" y="5.2" width="7.2" height="4.6" rx="1.3" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M4 5.2V3.9a2 2 0 0 1 4 0v1.3" fill="none" stroke="currentColor" stroke-width="1.5"/></svg></span>`;
        for (let ch = 1; ch <= LANE_COUNT; ch++) {
            const n = local.lanes.get(ch);
            if (!n || !n.badges) continue;
            const groups = new Map();
            for (const m of segMarksOf(segmentsOfCh(store.segments, ch))) {
                const g = groups.get(m.tS) || [];
                g.push(m.kind);
                groups.set(m.tS, g);
            }
            let html = "";
            for (const [tS, kinds] of groups) {
                const x = timeToX(vp, stageW, tS);
                if (x < 0 || x > stageW) continue;
                const left = ((x / stageW) * 100).toFixed(2);
                let inner = "";
                for (const kind of kinds) {
                    inner +=
                        kind === "lock"
                            ? lockHtml
                            : `<span class="wave-seg-badge">${kind}</span>`;
                }
                html += `<span class="wave-seg-marks" style="left:calc(${left}% + 2px)">${inner}</span>`;
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

    /**
     * 静态层:每条可见泳道一块 tile → 一张位图(缓存命中直画,未命中取到再补)。
     * 拖动/缩放进行中(vpIdleTimer 挂起 = 视口未静止 120ms)缓存未命中时
     * **不发新请求**,把最近一次画过的块按新视口横向重映射先顶上(05 §6.3
     * 「拖动平移先 blit 旧位图,静止 120ms 后取新块」);静止后 vpIdleTimer
     * 标脏再走取数分支。
     */
    function paintStaticLanes() {
        const vp = timeline.viewport();
        const w = stageWidth();
        if (!(w > 0)) return;
        const k = backingK();
        const cols = Math.min(Math.max(Math.round(w), 1), MAX_COLS);
        const moving = !!local.vpIdleTimer;
        const pal = local.palette || undefined;
        for (const ch of visibleLanes()) {
            const n = local.lanes.get(ch);
            if (!n || !n.canvas) continue;
            const ctx = resizeCanvas(n.canvas, w, LANE_H, k);
            if (!ctx) continue;
            const tile = waveSource.peek(ch, vp.startS, vp.endS, cols);
            if (tile) {
                paintWaveTile(ctx, tile, w, LANE_H, pal);
                local.lastPaint.set(ch, {
                    tile,
                    startS: vp.startS,
                    endS: vp.endS,
                });
            } else if (moving) {
                const last = local.lastPaint.get(ch);
                if (last && last.endS > last.startS) {
                    const span = spanOf(vp);
                    ctx.save();
                    ctx.translate(((last.startS - vp.startS) / span) * w, 0);
                    ctx.scale((last.endS - last.startS) / span, 1);
                    paintWaveTile(ctx, last.tile, w, LANE_H, pal);
                    ctx.restore();
                }
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

            // ② 段边界(auto 白虚线 5 5 / 手动白实线;图例帧 763-764)。
            //    拖拽中的那条跳过原位 —— 预览线在循环外统一画。
            const drag = local.boundDrag;
            for (const b of boundariesOf(segCh)) {
                if (drag && drag.ch === ch && b.tS === drag.origT) continue;
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

        // ③ 边界拖拽预览线(释放才发 §1.22:拖动期纯本地;吸附命中谷点时
        //    琥珀加亮 = A-14 的「竖线加亮」反馈,Alt 关吸附则维持白亮线)
        const d = local.boundDrag;
        if (d) {
            const x = timeToX(vp, w, d.tS);
            if (x >= 0 && x <= w) {
                const y0 = (d.ch - 1) * LANE_H;
                ctx.beginPath();
                ctx.setLineDash([]);
                ctx.strokeStyle = d.snapped
                    ? "rgba(226,196,136,.95)"
                    : "rgba(255,255,255,.85)";
                ctx.lineWidth = d.snapped ? 2 : 1.6;
                ctx.moveTo(x, y0);
                ctx.lineTo(x, y0 + LANE_H);
                ctx.stroke();
            }
        }
    }

    /**
     * 单维阶梯曲线:逐段水平线 + 相邻段值差处以边界为中心的对称斜坡。
     * 每段先描一道深底 halo(线宽 +CURVE_HALO_W)再上本色 —— pan 的薰衣草与包络柱
     * 同色系、且恰好压在泳道中线上,不垫深底就读不出来(Wave 2 亲验第 3 条)。
     * 两级权重(05 行 310)靠**线宽 1/1.6(B-09 的 +0.6px 差)+ 透明度**双轨保留,
     * auto 档透明度从 .5/.42 提到 .82/.72(深底档,见 deviations §R)。
     */
    function drawStepCurve(ctx, segs, vp, w, laneY, rampPx, dim) {
        const yOf = (seg) =>
            laneY + (dim === "pan" ? panYPx(seg.pan) : volYPx(seg.volDb));
        for (let i = 0; i < segs.length; i++) {
            const seg = segs[i];
            if (!seg) continue;
            const manual = seg.origin && seg.origin !== "auto";
            const x0 = timeToX(vp, w, num(seg.t0S, 0));
            const x1 = timeToX(vp, w, num(seg.t1S, 0));
            if (x1 < 0 || x0 > w) continue;
            const y = yOf(seg);
            const half = rampPx / 2;
            const prev = segs[i - 1];
            const next = segs[i + 1];
            const path = new Path2D();
            // 段头:有上一段且值不同 → 从斜坡终点起画(斜坡由上一段迭代画上半)
            path.moveTo(Math.max(x0 + (prev ? half : 0), 0), y);
            path.lineTo(Math.min(x1 - (next ? half : 0), w), y);
            // 边界斜坡(跨在边界两侧,以边界为中心对称;绝不垂直跳变)
            if (next) {
                path.moveTo(x1 - half, y);
                path.lineTo(x1 + half, yOf(next));
            }
            const lw = manual ? CURVE_W_MANUAL : CURVE_W_AUTO;
            ctx.strokeStyle = CURVE_HALO;
            ctx.lineWidth = lw + CURVE_HALO_W;
            ctx.stroke(path);
            // 色相锁死 05 行 310 / 图谱 §12 ③ 的两色:pan = accent 薰衣草
            // rgba(181,172,201)、vol = 白;两级权重只走线宽(B-09)与 alpha,
            // **不改 rgb**(本波曾顺手换过一档手动色相,tokens.css 零命中,已回退)。
            ctx.strokeStyle =
                dim === "pan"
                    ? manual
                        ? "rgba(181,172,201,.98)"
                        : "rgba(181,172,201,.82)"
                    : manual
                      ? "rgba(255,255,255,.95)"
                      : "rgba(255,255,255,.72)";
            ctx.lineWidth = lw;
            ctx.stroke(path);
        }
    }

    // ------------------------------------------------------------ 事件入口
    /**
     * §2.8:段表/曲线/角标脏 + **选中态重绑**(brief §0.7 / 契约字段纪律:
     * `segIdx` 每次事件后重新编号 —— 选中段按 {t0S,t1S} 时间锚在新表里重绑,
     * 找不到即失效,绝不跨事件持有旧下标)。检查器乐观回声(echo)同拍让位。
     * A-01→A-02 收尾:reason vad/segmentation(松手流水线)与 analyze(重分析)
     * 撤下倒计时条并弹 diff 变更列表。
     */
    function onSegments(seg) {
        local.overlayDirty = true;
        local.marksDirty = true;
        if (local.selectedCh && local.selectedSegs.length) {
            const segCh = segmentsOfCh(getStore().segments, local.selectedCh);
            local.selectedSegs = rebindSegKeys(
                local.selectedSegs,
                (segCh && segCh.segments) || [],
            );
            if (!local.selectedSegs.length) local.selectedCh = 0;
        }
        local.echo = {};
        const reason = seg && seg.reason;
        if (
            reason === "vad" ||
            reason === "segmentation" ||
            reason === "analyze"
        ) {
            armCountdown(null);
            setDiff(seg.diff || null);
        }
        schedulePaint();
        requestRender();
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
        local.playheadEv = p || null;
        // 「只投影当前激活 tab」在 rAF 侧同样成立(对抗校验 major):Tab3 不在前台
        // 时舞台 display:none ⇒ stageWidth()=0,插值循环每帧只是把竖线反复置
        // hidden;帧时账也没有可测的帧。事件照存不照画,切回本页由 render() 的
        // resumePlayhead() 补一次 push + 起帧。
        if (!isPanelActive()) {
            local.playheadHeld = true;
            if (playhead.running()) playhead.stop();
            return;
        }
        playhead.push(p || null);
        const st = getStore();
        const capturing = !!((st.state || {}).global || {}).capture_enabled;
        show(els.playheadCap, !!(p && p.isPlaying) && capturing);
        // 播放期帧时账开跑(空闲零 rAF:停播且无交互时循环自停)
        if (p && p.isPlaying) ensureTicker();
    }

    /**
     * 切回本页:把非前台期间挂起的播放头/帧时账接回来(只在**挂起过**时补一次
     * —— 每帧无脑重 push 会把插值基线 evAtMs 每帧归零,播放头反而冻住)。
     */
    function resumePlayhead() {
        if (!local.playheadHeld || !isPanelActive()) return;
        local.playheadHeld = false;
        onPlayhead(local.playheadEv);
    }

    return {
        mount,
        render,
        onSegments,
        onCaptureProgress,
        onPlayhead,
        // 布防 badge 三处的共用跳转口径(05 行 300 ①;Tab1/Tab2 badge 经
        // app.js 切到本页后调用,定位选区 + 勾选目标轨)
        locateRecapture,
    };
}
