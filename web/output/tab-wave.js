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
    MIN_SPAN_S,
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
    OVERVIEW_COLS,
} from "./canvas/waveform.js";
import { nearestHit, BOUNDARY_HIT_PX } from "../shared/hit.js";
import { wheelPx, WHEEL_LINE_PX, WHEEL_PAGE_PX } from "../shared/wheel.js";
import { isEditableTarget } from "../shared/context-menu.js";
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

/**
 * 泳道行高**默认**档 34px(实景帧 1812;[J72a] C-10:TAB_ROWS 的 44 是旧口径,不取)。
 *
 * T33 Wave 4(用户新件):行高改成**运行时可变**(纵向缩放条)—— 编译期常量只剩
 * 「默认值 + 纯函数面的换算基准」。**运行期几何一律读 `local.laneH`**
 * (泳道行 CSS 高 / canvas 高 / overlay 高 / visibleLanes / recapband 子块 /
 *  角标 / 播放头 / 选区叠加层),不得再引本常量。
 */
export const LANE_H_DEFAULT = 34;

/**
 * 兼容别名(= 默认档)。纯函数断言面、T34/T43 复用面与设计稿换算注释按它引用;
 * 交互期几何请读 `local.laneH`。
 */
export const LANE_H = LANE_H_DEFAULT;

/**
 * 运行时行高夹取区间(Wave 4 纵向缩放条)。
 * 下限 22:`envelopeHalfPx` 的 `h/2−4` 在 22px 档还剩 7px 可画(再矮包络就摊成
 * 一条线,05 行 311 的 min/max 包络失去意义)。
 * ⚠ 修订轮更正:原注写的「pan(中线 12±7)与 vol(基线 22−7)两带压到 22px
 * 仍不重叠」是错的 —— 两带的实现是纯等比(panYPx/volYPx 见下),pan 值域
 * [5k,19k] 与 vol 值域 [15k,22k] 对任意 k>0 都交于 [15k,19k],34px 默认档就
 * 已相交;等比缩放不改变相对关系,所以这条既不是下限 22 的理由,也不是退化。
 * 上限 88:15 泳道 × 88 = 1320px,纵向滚动条兜住,再高单屏一条都看不全。
 */
export const LANE_H_MIN = 22;
export const LANE_H_MAX = 88;

/** 行高夹取(非法值回默认档)。 */
export function clampLaneH(h) {
    const v = num(h, LANE_H_DEFAULT);
    return Math.round(Math.min(Math.max(v, LANE_H_MIN), LANE_H_MAX));
}

/** 行高 → 滑杆行程比(0..1;纵向缩放条 thumb 与 aria 同源)。 */
export function laneHPercent(h) {
    return (clampLaneH(h) - LANE_H_MIN) / (LANE_H_MAX - LANE_H_MIN);
}

/** 滑杆行程比 → 行高(0..1 线性)。 */
export function laneHFromPercent(p) {
    const x = Math.min(Math.max(num(p, 0), 0), 1);
    return clampLaneH(LANE_H_MIN + x * (LANE_H_MAX - LANE_H_MIN));
}

// ---- 滚轮四路映射(SL-205)---------------------------------------------------
// 改前**只实装了 Ctrl 一路**(`if (!e.ctrlKey) return`),另外三路连分支都没有。
// 后果不只是「没功能」:没被 preventDefault 的滚轮会**漏给浏览器默认动作** ——
// 页面级实测,裸滚轮把泳道列竖着滚跑了 121px(用户看到视图突变,自然以为缩放坏了)。

/** Alt 纵向缩放:每格行高步进(22..88 共 66px,4px/格 ≈ 17 档)。 */
export const LANE_H_WHEEL_STEP = 4;

/**
 * 微拖之后多久内不接受「双击边界 = 合并」(ms;复审【重要】)。
 *
 * 边界拖拽是**按下-微移-释放**,连着做两次会被浏览器判成一次 dblclick —— 于是
 * 「我在细调这条边界」被读成「我要删掉这条边界」,左右两段当场合并。代价还特别大:
 * 前两笔 move_boundary 已按契约 §5.4 把命中段变成 origin=user_edited + locked=true,
 * 而 analyze(…,{clearManual:true}) 对 locked 段免疫(须先逐段解锁)。
 * 故 commitBoundDrag **真发**了 move_boundary 之后的这一小段时间里,双击的边界分支
 * 整个让路(既不合并、也不落到分割去 —— 落过去等于又加一条边界,同样不是用户要的)。
 */
export const BOUND_DRAG_MERGE_GUARD_MS = 400;

/**
 * 「刚提交过边界拖拽,这次双击不算合并」判定(纯函数,node 侧可断言)。
 * `lastCommitMs` 为 0 / 非数 = 本会话还没提交过拖拽 ⇒ 不拦。
 */
export function mergeGuardedByDrag(nowMs, lastCommitMs) {
    const t = num(lastCommitMs, 0);
    if (!(t > 0)) return false;
    const dt = num(nowMs, 0) - t;
    return dt >= 0 && dt < BOUND_DRAG_MERGE_GUARD_MS;
}

/**
 * [SL-242] 段级「恢复自动」的 scope(纯函数,node 侧可断言)。
 *
 * 用户实测(v5.6.2 终验 A7):点**一个小片段**的「恢复自动」,整个大片段被合并、
 * 全都回了自动。成因就在这里 —— 这枚钮此前发的是 `{tracksMask: 1 << (ch-1)}`,
 * **不带范围**;而 §1.6 的对象形 scope 缺省范围 = 走 `"all"` 那条推导(见
 * `AnalyzeScopeMath.h::analyzeScopeRange`),于是「整轨 × 整条时间线」的手动段
 * 一次清光。钮长在**段检查器**里、就在这一段的锁定开关下面,用户读到的当然是
 * 「把**这一段**还回自动」—— 入口的位置就是它的作用域承诺。
 *
 * 定谳后的语义(写进 PR / 确认句):**恢复自动 = 只重算选中这一段的区间**,
 * 该段的 origin 回 auto、值回引擎算出来的值;**不合并边界**,相邻段(不论 auto、
 * 手动还是锁定)一个字节都不动。相邻自动段要不要并起来是另一件事,不在本入口里做。
 *
 * `openEnded` 段(§2.8):`t1S` 只是一个**保守下界**,不是真末端 —— 那是
 * `setTrackManual` 造的「单段全时限常值」,CRVS 里 t1 是 1<<40 哨兵。对它取
 * `endS = t1S` 会把段的右半截留在手动态。故这一档**不给 endS**,让真桥按
 * `analyzeScopeRange`「给了的照用、没给的取 all 档同侧端点」推到时间线末端 ——
 * 那本来就是这个段的真实覆盖面(它就是整轨)。
 *
 * 拿不到有限的 `t0S` 时回 `null`(调用方不发请求)。**不退回轨级 scope** ——
 * 那正是本卡要修掉的行为,拿它当兜底等于把缺陷留一条后门。
 *
 * @param {number} ch 1..15
 * @param {{t0S?:number, t1S?:number, openEnded?:boolean}|null} seg §2.8 的段对象
 * @returns {{tracksMask:number, startS:number, endS?:number}|null}
 */
export function segmentRestoreScope(ch, seg) {
    const n = Number(ch);
    if (!Number.isInteger(n) || n < 1 || n > LANE_COUNT) return null;
    const s = seg || {};
    if (!Number.isFinite(s.t0S)) return null;
    const scope = { tracksMask: 1 << (n - 1), startS: s.t0S };
    if (!s.openEnded && Number.isFinite(s.t1S) && s.t1S > s.t0S) {
        scope.endS = s.t1S;
    }
    return scope;
}

/**
 * 滚轮该走哪一路(纯函数,node 侧可断言)。
 *
 * 优先级 Ctrl > Alt > Shift > 裸,**不接受组合**:同时按住只认最高的那一个。
 * 理由是可预测性 —— 组合键在不同宿主/输入设备上被吞被改的概率远高于单修饰键
 * (Shift+滚轮在 Chrome 里就会被改写成 deltaX,见 wheelPx)。
 *
 * @returns {"hzoom"|"vzoom"|"vpan"|"hpan"}
 */
export function wheelRoute(e) {
    const ev = e || {};
    if (ev.ctrlKey || ev.metaKey) return "hzoom";
    if (ev.altKey) return "vzoom";
    if (ev.shiftKey) return "vpan";
    return "hpan";
}

// 位移归一(deltaMode 分档 + 主轴取大)走 shared/wheel.js —— 与总览轨迹图**同一份**;
// 两处曾各写一份逐字同义的实现,抽走免得下次只修一边(复审建议③)。
export { wheelPx, WHEEL_LINE_PX, WHEEL_PAGE_PX };

/** 段检查器宽(稿内 877;灰模 260 → 统一 262,登记差异)。 */
export const INSPECTOR_W = 262;

/** 右缘双刻度列宽(图例帧 786;B-11:全泳道区共用一列)。 */
export const SCALE_COL_W = 44;

/**
 * ⚠ **VZOOM_GUTTER_W 已撤销(Wave 5 用户裁定②)**。
 *
 * 沿革:Wave 4 把 12px 的竖直纵向缩放条浮在泳道 canvas 最右 18px 上,造出
 * 24×74px 指针死区 → 修订轮从舞台宽让出 24px 自留槽把它挪进去。用户 preview
 * 第二轮的原话是「为什么没有贴着右侧,放在纵向滚动的进度条下面?」—— 杆缩在
 * 双刻度列**左侧**、压在泳道区内,既不贴边也占着舞台宽。
 *
 * 裁定:杆整件移到**窗口右下角的底部缩放条行内**(`.wave-hzoom__vzoomctl`,
 * 与纵向滚动条同一右缘、在其轨道**下方**),槽随之撤销 —— 舞台宽从
 * `w − 158 − 44 − 24` 回到 `w − 158 − 44`,`.wave-overlay` / `.wave-lane__static`
 * / `.wave-lane__badges` 三处右让口径一并归并到单一的 44(code-review 提的
 * badges 右让没跟着改的账,一并了结)。
 */

/** 时间标尺行高(实景帧 807)。 */
export const RULER_H = 22;

/**
 * 底部缩放/滚动条行高。
 *
 * 稿内 866 是 20px;**Wave 6 用户裁定把它抬到 44** —— 用户 preview 第三轮要求
 * 纵向缩放条「纵向放置、在纵向进度条的下方、与横向缩放条呈 90 度」,而 Wave 5
 * 把它压成横轨的唯一理由就是「20px 行高放不下竖杆」。既然形制由用户定,腾空间
 * 的代价就由布局出:底部条 44px(内高 43)= 6×22 竖轨(VZOOM_H)+ 上下各 10.5px
 * 余量,那道余量同时是**圆角账**(见 index.html `.wave-hzoom__vzoomctl` 头注)。
 * 泳道 canvas 少 24px 高,纵向滚动条兜住;`.wave-scalecol` / `.wave-selection` /
 * `.wave-dim` 三处 `bottom` 与本值同步。deviations 登记「覆盖稿内 866 的 20px」。
 */
export const BOTTOM_BAR_H = 44;

/** 底部条左空档(867;+ 左 padding 10 = HEAD_W)。 */
export const BOTTOM_HEAD_W = 148;

/**
 * 纵向缩放条轨长(**与 index.html `.wave-vzoom` 的 height 同步**)。
 *
 * 三轮沿革:Wave 4 = 泳道区右缘 12×62 竖杆;Wave 5 = 迁进 20px 底部条后被压成
 * 44×6 横轨;**Wave 6 用户裁定竖回来** —— 6×22 竖轨落在底部条最右端的
 * `.wave-hzoom__vzoomslot`(= 泳道区纵向滚动条那一列)里,与横向缩放条呈 90°。
 * 22 = 底部条内高 43 − 上下各 10.5px 余量,那道余量是**圆角账**:11px 圆点
 * thumb 走到最矮档(触轨底)时得离窗口内圆角(14)够远才不被啃 —— 26px 轨长
 * 实测就啃了一口(见 index.html `.wave-hzoom__vzoomctl` 头注的逐项算式)。
 * thumb 行程走百分比 + `margin-top:-5.5px`,**上 = 高**(与「向上拖 = 泳道变高」
 * 同向),端头不需要 PAD 常量。
 * 语义三轮不变:纵向缩放 = 泳道行高 22..88px,读数「⇕ 34」在轨左侧,
 * 方向键上/右加高、下/左减矮。
 */
export const VZOOM_H = 22;

/** 无任何时长线索时的兜底工程时长(秒;mock 假数据同为 5 分钟,J59)。 */
export const FALLBACK_DURATION_S = 300;
/**
 * 工程时长的**合理上限**(秒,24h)。
 *
 * 存在的理由不是「工程不会更长」,而是**一个坏字段不该污染整个视口模型**。
 * P0-A 那次的具体坏字段(无末端哨兵)现在已经在两处按**语义**处理掉了 —— C++ 侧
 * emitSegments 降级右端 + 上面 durationOf 跳过 openEnded 段 —— 本上限是最后一道
 * 兜底,防的是「以后又冒出别的坏字段」,不再是当前已知问题的主要防线。
 */
export const MAX_DURATION_S = 24 * 60 * 60;

/**
 * 7 滑杆定义(顺序不可重排 —— 05 §2.3 行 298-299 的 §1.18 五字段 + §1.19 两字段)。
 * 值域由设计稿默认值与行程比反推(p = (def-min)/(max-min) 与稿内 2070-2074 逐一相符):
 * -38dB→.44 / 6dB→.30 / 180ms→.36 / 120ms→.24(J23)/ 200ms→.40(J23)/
 * .62→.62 / 420ms→.28。`gb` = 灰模既有锚点(appendix B),`t` = 短标词条(A-19)。
 */
export const SLIDERS = Object.freeze(
    [
        // prettier-ignore
        { key: "threshold", field: "threshold_db", api: "vad", gb: "wave-vad-threshold", t: "wave.sldThreshold", tip: "wave.tipThreshold", min: -60, max: -10, def: -38, unit: "dB", dp: 0 },
        // prettier-ignore
        { key: "hysteresis", field: "hysteresis_db", api: "vad", gb: "wave-vad-hysteresis", t: "wave.sldHysteresis", tip: "wave.tipHysteresis", min: 0, max: 20, def: 6, unit: "dB", dp: 0 },
        // prettier-ignore
        { key: "hangover", field: "hangover_ms", api: "vad", gb: "wave-vad-hangover", t: "wave.sldHangover", tip: "wave.tipHold", min: 0, max: 500, def: 180, unit: "ms", dp: 0 },
        // prettier-ignore
        { key: "paddingpre", field: "padding_pre_ms", api: "vad", gb: "wave-vad-paddingpre", t: "wave.sldPadPre", tip: "wave.tipPadPre", min: 0, max: 500, def: 120, unit: "ms", dp: 0 },
        // prettier-ignore
        { key: "paddingpost", field: "padding_post_ms", api: "vad", gb: "wave-vad-paddingpost", t: "wave.sldPadPost", tip: "wave.tipPadPost", min: 0, max: 500, def: 200, unit: "ms", dp: 0 },
        // prettier-ignore
        { key: "sensitivity", field: "sensitivity", api: "seg", gb: "wave-seg-sensitivity", t: "wave.sldSensitivity", tip: "wave.tipSensitivity", min: 0, max: 1, def: 0.62, unit: "", dp: 2 },
        // prettier-ignore
        { key: "minseg", field: "min_segment_ms", api: "seg", gb: "wave-seg-minlen", t: "wave.sldMinSeg", tip: "wave.tipMinSeg", min: 0, max: 1500, def: 420, unit: "ms", dp: 0 },
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
 * 横向缩放条的行程 ↔ 倍率换算(T33 Wave 4 用户新件)。
 * 倍率 = 全长 / 视口跨度 ∈ [1, 全长/MIN_SPAN_S];行程取**对数刻度** ——
 * 线性刻度下 300s 工程的前 1% 行程就把跨度从 300s 掐到 3s,后 99% 全在 1×~1.03×
 * 之间空转。对数刻度让每一段等长行程都对应等比例的跨度变化(与 Ctrl+滚轮的
 * 定比 ZOOM_STEP 同构)。
 */
export function zoomMaxFactor(durationS) {
    return Math.max(num(durationS, 0) / MIN_SPAN_S, 1);
}

/** 行程比(0..1)→ 缩放倍率。 */
export function zoomFactorFromPercent(p, durationS) {
    const fMax = zoomMaxFactor(durationS);
    if (!(fMax > 1)) return 1;
    const x = Math.min(Math.max(num(p, 0), 0), 1);
    return Math.pow(fMax, x);
}

/** 缩放倍率 → 行程比(0..1)。 */
export function zoomPercentOfFactor(factor, durationS) {
    const fMax = zoomMaxFactor(durationS);
    if (!(fMax > 1)) return 0;
    const f = Math.min(Math.max(num(factor, 1), 1), fMax);
    return Math.log(f) / Math.log(fMax);
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
 *
 * **播放头只抬高、不压低**(code-review finding 5):空态(无段 + follow 档
 * `range.end_s=0`)下一开始播放,`playhead.timeS≈0.03` 是**唯一非零线索**,
 * 老写法把它当成工程时长 ⇒ `d≈0.03` → 视口被夹到 `MIN_SPAN_S`,标尺/缩放
 * 读数/滚动条整体塌成 1 秒再随播放慢慢爬回来。播放头是「已经放到这儿了,
 * 工程至少这么长」的**下界证据**,不是长度本身 —— 故:有别的长度证据时它
 * 只参与取大;没有时下限压在 FALLBACK 上(播放头越过 5 分钟仍能把估计抬上去)。
 */
/**
 * 段的**有效右端**(秒)。`openEnded` 段(§2.8:`setTrackManual` 的单段全时限常值,
 * CRVS 里 t1 = 1<<40 哨兵)表达的是「一直到时间线末端」,不是一个真时刻 —— 对它取
 * `+Infinity` 才能让「包含 / 相交 / 重叠」这些判断得到正确答案。
 *
 * 为什么必须在前端也处理:C++ 侧把 `t1S` 降级成了「已知时间线末端」,那是个**保守下界**
 * (保证段不坍缩、可点可切),不是真末端。若前端拿它当真末端用,播放头走过那个点之后
 * 就会判成「不在任何段内」—— 手动/冻结轨恰恰是最常被点的那批(v5.3 R4)。
 */
export function segEndS(seg) {
    if (seg && seg.openEnded === true) return Infinity;
    return num(seg && seg.t1S, 0);
}

export function durationOf(store) {
    const st = store || {};
    let d = 0;
    const range = ((st.state || {}).global || {}).range || {};
    d = Math.max(d, num(range.end_s, 0));
    for (const c of (st.segments && st.segments.channels) || []) {
        for (const seg of (c && c.segments) || []) {
            // 「无末端」段(§2.8 `openEnded`)的右端不是真时刻,**不参与工程时长推定** ——
            // 它表达的是「一直到时间线末端」,拿它当长度证据是循环论证。C++ 侧已把 t1S
            // 降级成已知末端,这里再挡一道:两侧都按语义处理,不靠数值大小猜。
            if (seg && seg.openEnded === true) continue;
            d = Math.max(d, segEndS(seg));
        }
    }
    const ph = num(st.playhead && st.playhead.timeS, 0);
    // 夹上限:单个坏字段(无末端哨兵)不得污染整个视口模型,见 MAX_DURATION_S。
    return Math.min(
        Math.max(d > 0 ? d : FALLBACK_DURATION_S, ph),
        MAX_DURATION_S,
    );
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

/**
 * 「重采集选区」开关的五态(SL-193,用户 v5.4 实测拍板 2026-08-27)。
 *
 * 四态与 Tab1 的 `captureVisual` 同族,第五态 `needcap` 是 Tab3 特有的。判据逐条:
 *
 *   off        `state.recapture.armed` 为假 —— 没布防。
 *   needcap    布防了,但 `global.capture_enabled` 是关的。**这一档必须单列**:布防只是标记
 *              「哪一块失效、要重录」,真正决定特征写不写得进去的闸是 01 采集。所以「布防了
 *              但没开采集」= 播多久都一个 hop 都不会写 —— 报「已布防·等待播放」就是**说谎**,
 *              用户等到天荒地老也等不到「采集中」。
 *              **[J87] 之后这一档变罕见,但没有消失**:布防现在会自动打开 01 采集(裁定①),
 *              正常路径进不来;但用户可以在布防期间手动把 01 采集关掉,而引擎侧**不会**因此
 *              撤防(04 §4.2 ④ 的「采集 OFF 即解除布防」未实装,理由见 J87 的 PR 描述)——
 *              于是这一档正是那个状态的如实显示。
 *   armed      布防 ∧ 采集 ON ∧ 未播放 —— 真的只差按播放。
 *   capturing  布防 ∧ 采集 ON ∧ 播放中 ∧ 播放头落在布防区间 `[startS, endS)` 内 ——
 *              此刻特征确实在写。
 *   outside    在播,但没播到该采的地方。
 *
 * **[J87] `global.range`(§2.6 `inRange`)不再参与判定。** 引擎侧布防期间的记账门控是
 * 「工作选区 × 选中轨掩码」,`global.range` 被整个换掉(04 §4.2 ①)—— 选区在 range 之外时
 * 特征照写不误,而 `inRange` 会是 false。旧写法会在那种情形报「已离开重采集区」,
 * 屏幕上说没在采、盘上却正在写,是**说反了**。判位置只看布防区间这一条。
 *
 * 纯函数:node 侧直接断言,不需要 DOM(与 tab-master 的 captureVisual 同口径)。
 *
 * @param {object} state    `scvb.state`(读 recapture 与 global.capture_enabled)
 * @param {object} playhead `scvb.playhead`(读 isPlaying / timeS)
 * @returns {"off"|"needcap"|"armed"|"capturing"|"outside"}
 */
export function recaptureVisual(state, playhead) {
    const st = state || {};
    const rec = st.recapture || null;
    if (!rec || !rec.armed) return "off";
    if (!((st.global || {}).capture_enabled === true)) return "needcap";
    if (!playhead || !playhead.isPlaying) return "armed";
    // [J87] 不看 playhead.inRange —— 它算的是 global.range,而布防期的门控已经不是它了。
    const t = num(playhead.timeS, NaN);
    const s0 = num(rec.startS, 0);
    const s1 = num(rec.endS, 0);
    return Number.isFinite(t) && t >= s0 && t < s1 ? "capturing" : "outside";
}

/**
 * 「重采集选区」开关的不可用原因(SL-193;null = 可用)。返回值直接是**词条 key**,
 * 既喂 disabled 判据也喂 tooltip —— 一个真源,不会出现「灰着但说得出理由」和
 * 「说不出理由但灰着」两张皮。
 *
 * `armed=true` 恒可用:开关必须关得掉。撤防走 `recaptureArm(0,0,0)`,不吃 scope,
 * 所以选区被改掉/清掉之后照样能关(否则开关会永久卡在 ON)。
 */
export function recaptureBlockReason(o) {
    const s = o || {};
    if (s.blocked) return "wave.armReason.readOnly";
    if (s.armed) return null;
    if (!s.picked) return "wave.armReason.noTracks";
    if (!s.hasSel) return "wave.armReason.noSelection";
    return null;
}

/**
 * 「重分析选区」的不可用原因(SL-193;null = 可用)。同上:判据与 tooltip 同一真源。
 *
 * 【本函数就是「重分析一直是灰的」的定谳落点】改前的判据是
 *     `!!scope && !blocked && hasData && previewHit`,其中
 *     `previewHit = local.preview.intervals > 0`。
 * 两处错:
 *   ① **口径接错**。§1.6 的拒绝态逐字是「`range ∩ coverage = ∅`」,而 §1.5 dry-run 里
 *      承载这个量的是 **`tracks`**(范围内有覆盖的轨数),不是 `intervals`
 *      (会被重画的**已有段**数)。刚采完还没分析过的素材:覆盖满满当当、段表空空如也
 *      ⇒ intervals=0 ⇒ 首次分析永远点不动 —— 鸡生蛋。同一个坑 Tab1 踩过并已绕开
 *      (tab-master.js `analyzeNoData` 的行注:「只看段表会鸡生蛋(首采未析永远禁用)」),
 *      Tab3 这一份把它又踩了一遍。
 *   ② **真机上 `intervals` 恒为 0**。native 的 `previewAnalysis()` 只填 tracks 与
 *      manualKept,`intervals` 一路留在默认值 0(本卡一并修);mock 的 `affectedOf()`
 *      却算得好好的 —— 于是 web-preview 里怎么点怎么对,装进宿主就恒灰。
 * 合起来:真机上「有选区 + 有数据」时按钮也**必然**灰,而且没有任何原因可看。
 *
 * 改后判据取 `previewTracks`:null = dry-run 回包还在路上(先放行,到达再收敛),
 * 0 = 范围与覆盖真的没交集(§1.6 的拒绝态,与 analyze 的返回口径对齐)。
 */
export function reanalyzeBlockReason(o) {
    const s = o || {};
    if (s.blocked) return "wave.armReason.readOnly";
    if (!s.picked) return "wave.armReason.noTracks";
    if (!s.hasSel) return "wave.armReason.noSelection";
    // 全局空态与「本选区无覆盖」共用同一句(「当前范围内无采集数据——调整范围或先采集」):
    // 对用户而言要做的下一步逐字相同,分成两句只是让人多读一行。
    if (!s.hasData) return "master.step2.desc.noData";
    if (s.previewTracks === 0) return "master.step2.desc.noData";
    return null;
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

/**
 * pan 值 → 泳道内 y(px;实景帧 1819:中线 12 ± 7,pan ∈ -100..100)。
 * `laneH` 缺省 = 默认档 34;行高变化时**整套纵向几何按比例缩放**(Wave 4 纵向
 * 缩放条)—— 等比缩放不改变两带的相对关系(修订轮更正:两带值域本来就相交,
 * pan [5k,19k] ∩ vol [15k,22k] = [15k,19k],34px 默认档亦然;真正把两条线分开的
 * 是**颜色与线宽**(pan = accent 薰衣草 / vol = 白,05 行 310),不是值域)。
 */
export function panYPx(pan, laneH) {
    const p = Math.min(Math.max(num(pan, 0), -100), 100);
    return (12 + (p / 100) * 7) * (clampLaneH(laneH) / LANE_H_DEFAULT);
}

/** volDb → 泳道内 y(px;实景帧 1821:基线 22 − 行程比 × 7;行程比同 Tab2 卡箍)。 */
export function volYPx(volDb, laneH) {
    return (
        (22 - (volPercent(volDb) / 100) * 7) *
        (clampLaneH(laneH) / LANE_H_DEFAULT)
    );
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
 * **选中段高亮**(Wave 5 用户裁定③:「选择了片段之后,那个片段最好有一个高亮之类
 * 的效果,不然不清楚选中的是哪个」)。
 *
 * 此前点段只开检查器,泳道上零指示 —— 多选(shift 连选两段)时更是完全看不出选中
 * 的是哪两段。这里补一层**薰衣草半透明底 + 四边实框**:
 *   · 底 α .22 —— 压在包络柱与 VAD 顶带上能读出「这一段被框住了」,又不至于把
 *     粉白波形染成紫(与裁定⑥「不能盖住波形」同一条底线)。
 *     ⚠ 亲验后从 .16 提到 .22:点段**同时**会勾选该轨(05 行 288 的整行点选语义),
 *     行底本身已是 `rgba(acc,.16)` 的淡薰衣草,底对底的差被吃掉大半;
 *   · **四边都画**(不只上下):同一笔账 —— 只靠底色差在已选中的行里读不出来,
 *     真正让它「一眼是个框」的是那圈 2px 亮边。竖边压在段边界线上是**故意**的:
 *     选中段的两端本来就是它的边界;亮薰衣草 2px 与 auto 白虚 / 手动白实
 *     (1 / 1.6px)在色相与粗细上双重两分,不会读混。
 * 层序:画在共享动态层**最底**(曲线/边界之前),曲线永远压在高亮之上。
 * 与选区压暗(窗级 DOM)/ 布防斜纹(滚动层 DOM)分属不同层,互不遮挡。
 */
const SEG_SEL_FILL = "rgba(181,172,201,.22)";
const SEG_SEL_EDGE = "rgba(214,208,235,.95)";
const SEG_SEL_EDGE_W = 2;

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
            if (mid >= s.t0S && mid < segEndS(s)) {
                best = i;
                break;
            }
            const ov =
                Math.min(segEndS(s), segEndS(k)) - Math.max(s.t0S, k.t0S);
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
            if (!s || !(segEndS(s) > s0 && s.t0S < s1)) continue;
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
// 注:本函数返回的是**模板字符串**,里面的 HTML 注释同样进 fetch_fonts.py 的字符扫描
// (js_strings 取的是字面量,分不清哪段是注释)—— 模板里的说明要短,长说明写在函数外的
// JS 注释里。SL-177 的 ⚠ 角标语义:该轨上游音频与已采集特征不一致,建议重新采集
// (04 §4.5 fingerprint watchdog);只提示,不自动失效、不阻断任何操作。
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
        <!-- 「数据可能已过期」⚠ 角标(04 §4.5;整句 tooltip 走 wave.staleTrack) -->
        <span class="wave-lane__staledot" data-gb="${gb("stale")}" hidden>⚠</span>
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
     * 检查器数字框的「最后渲染进框的解析值」账(input → {last})。
     * `wireNumInput` 的零位移去重基准,写入点只有两处:render 把模型值写进框时
     * (`setNumBox`)、以及 commit 真把值送上行时。见 wireNumInput 头注。
     */
    const numBoxes = new Map();

    /**
     * 把模型值渲染进数字框,并同步去重基准。
     * **聚焦中的框不覆写**(否则用户正在打的字会被每帧 render 抹掉),基准也
     * 一并按兵不动 —— 那正是「用户开始编辑之前框里是什么」,即零位移的参照。
     */
    function setNumBox(input, v, dp) {
        if (!input) return;
        if (root.activeElement === input) return;
        input.value = fmtSigned(v, dp);
        const q = 10 ** dp;
        const st = numBoxes.get(input);
        if (st) st.last = Math.round(num(v, 0) * q) / q;
    }
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
        lastDict: null, // 上一帧的词典对象(恒等比较 = 切语言检测,见 render)
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
        /**
         * 段检查器面板开关(Wave 5 用户裁定④,**覆盖 C-11 的条件渲染**)。
         * 默认**开**:关掉的话点段就没有任何面板反应,J67「段编辑唯一入口」会
         * 变得不可发现;开着时无选中段出空态句,点不同段只换内容、不改宽度。
         * **持久化落点**:契约 §7 manifest 里没有 UI 偏好的写面(`ui.*` 只有
         * scale/language/active_tab/guide_seen/tour_seen),桥面零新增是硬纪律 →
         * 只存本进程内存,切 tab 保持、重开面板回默认开。deviations 登记。
         */
        inspectorOpen: true,
        reidentifyCounts: null, // {k,l}:重新识别确认框正文的定格计数(切语言补填用)
        boundDrag: null, // {ch, segIdx, tS, snapped, minS, maxS}
        // [SL-230] 检查器「恢复自动」展开确认的**是哪一段**("" = 没展开)。
        // 不做成裸布尔(#148 复审【建议】①):那样它不跟选中段绑定 —— 在段 A 上点开确认、
        // 不取消直接去点段 B,面板一进去就停在「取消 / 继续」上,用户没点过却已经在问他。
        // 存段身份、按身份比对,选中段一变自然失配,不依赖别处记得复位。
        inspRestoreAsk: "", // `${ch}:${segIdx}`
        inspRestoreBusy: false, // 「继续」在途(#148 复审【建议】②:快速双击会白跑两趟秒级分析)
        boundCommitAt: 0, // 上次**真发** move_boundary 的时刻(双击合并的让路判据)
        sliderDrag: null, // 正在拖的滑杆(els.sliders 元素)
        autostopUser: false, // 「区域外自动停止」是用户手勾的(撤防不复位它)
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
        vpIdleTimer: 0, // 视口静止 120ms 才取新块的计时(§6.3)
        panDrag: null, // 空白拖拽平移 {lastX}
        thumbDrag: null, // 底部条拖拽 {startX, vp0}
        pendingDown: null, // 舞台按下未定性(点选 vs 平移)
        knobDrag: null, // 检查器 PAN 旋钮拖拽 {startY, startVal, val}
        vslDrag: null, // 检查器 VOL 滑杆拖拽 {val}
        echo: {}, // 检查器拖拽乐观值 {pan?, vol?}(segments 事件后失效)
        canvasVp: new Map(), // ch → 画布上现在这幅对应的视口(null = 不可信)
        laneH: LANE_H_DEFAULT, // 运行时泳道行高(Wave 4 纵向缩放条;几何唯一真源)
        hzoomDrag: null, // 横向缩放条拖拽 {rect}
        vzoomDrag: null, // 纵向缩放条拖拽 {rect}
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

    /**
     * 泳道是否在前台。两个条件都要:
     *   ① Tab3 是当前激活页(四面板同在 DOM,`#content[data-tab]` 切换);
     *   ② Tab3 内部停在泳道视图 —— [T41] 建议表打开时 `data-tab` **仍是** `wave`,
     *      泳道只是被 CSS 藏起来。少了这一条,建议表开着时 rAF 插值层照常空转、
     *      往 0 宽 canvas 上画(画面不会错 —— 切回来 ResizeObserver 会补 —— 纯属白烧帧,
     *      而且那些帧时喂进 governor 会误触降级序列)。
     */
    function isPanelActive() {
        const panel = els.panel;
        if (!panel || typeof panel.closest !== "function") return true;
        const section = panel.closest('[data-tab-panel="wave"]') || panel;
        if (
            typeof section.getAttribute === "function" &&
            section.getAttribute("data-view") === "suggest"
        ) {
            return false;
        }
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
            refreshPlayheadGeometry();
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

    /**
     * 舞台几何变了(视口平移/缩放、容器尺寸变化)⇒ 竖线的**像素位**要跟着重算,
     * 尽管 `tS` 一点没动。播放中由 playhead 自己的 rAF 下一帧顺手改正;**停播时
     * 循环已自停**(§6.1 空闲零 rAF),没有任何东西会去搬它 —— 暂停态拖视口会看到
     * 波形从静止的竖线底下滑走,且契约 §0.4「值未变不发」+ app.js 的空闲去重下,
     * 下一次 §2.6 事件可能永远不来(PR#64 评审【重要】3)。
     *
     * `playhead.refresh()` 对首帧事件前(内部 ev==null)是空操作,「首帧事件前不写
     * 入、竖线维持 HTML 初始 hidden」的既有纪律不受影响;也不碰降级节流账、不起 rAF。
     *
     * **不加 `if (!playhead.running())` 前置**:播放中多写这一次是幂等的
     * (下一帧 rAF 本来也要写同一个值),省不下什么;而「循环在不在跑」并不等于
     * 「停播没停播」—— 宿主/mock 停住时若仍按 30Hz 重发**位置有微抖**的事件,
     * `push()` 的空闲去重不成立、rAF 常驻,加了前置反而恰好在最像 bug 的那一档
     * 上把补写跳掉(web-preview 实测就是这一档)。
     */
    function refreshPlayheadGeometry() {
        playhead.refresh();
    }

    /**
     * 舞台宽(CSS px)= 泳道容器宽 − 轨头 158 − 右缘刻度列 44(C-04 舞台坐标系)。
     * Wave 5 裁定②把纵向缩放条搬去底部条右端后,它在泳道区内的 24px 自留槽撤销
     * (见 VZOOM_GUTTER_W 头注的撤销说明)—— 舞台宽拿回那 24px。
     */
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
        // Wave 6:纵向缩放条要与纵向滚动条**同列**(用户裁定「在纵向进度条的下方」),
        // 所以这笔滚动条宽账它也要用 —— 但方向相反:不是右让,是把底部条右端那只槽
        // 撑成滚动条那么宽,竖轨在槽内居中即与滚动条同心。槽宽兜底 16px 写在 CSS 的
        // `max()` 里(9px 窄滚动条下 11px 圆点会被窗口圆角啃,见 index.html 头注)。
        if (els.hzoomRow) {
            els.hzoomRow.style.setProperty("--wave-gutter", sb + "px");
        }
    }

    /**
     * 泳道行高落地(Wave 4 纵向缩放条的唯一写入点)。
     * 行高是**几何真源**:CSS 变量 `--lane-h` 驱动泳道行/舞台高,滚动层内容高
     * (共享动态层 + 播放头)按 15 × 行高改写,canvas 后备存储由下一帧的
     * `resizeCanvas` 按新高重建 —— 故这里必须把两层都标脏(全 canvas 重绘)。
     */
    function applyLaneH(h, rerender) {
        const v = clampLaneH(h);
        local.laneH = v;
        if (els.lanes) els.lanes.style.setProperty("--lane-h", v + "px");
        const contentH = LANE_COUNT * v;
        if (els.overlay) els.overlay.style.height = contentH + "px";
        if (els.playhead) els.playhead.style.height = contentH + "px";
        local.canvasVp.clear();
        local.staticDirty = true;
        local.overlayDirty = true;
        if (els.vzoom) {
            attr(els.vzoom, "aria-valuenow", v);
            // 读数:裸 22..88 播报成「88」读不出单位 → aria-valuetext 带单位供 AT;
            // Wave 5 起可见读数「⇕ 34」也就位(横向杆的「×N」一直有,这条以前
            // 只有 tooltip)。悬停 tooltip(词条 · 行高)在 render() 里写 ——
            // 那里才跟得上切语言。
            attr(els.vzoom, "aria-valuetext", v + "px");
            // Wave 6:竖轨 + 圆点 thumb(`margin-top:-5.5px` 居中),行程比
            // **下→上 = 矮→高**,故 top = (1 − p);与拖拽向上 = 变高同向。
            if (els.vzoomThumb) {
                els.vzoomThumb.style.top = (1 - laneHPercent(v)) * 100 + "%";
            }
            if (els.vzoomVal) text(els.vzoomVal, "⇕ " + v);
        }
        if (rerender !== false) {
            schedulePaint();
            requestRender();
        }
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

    /**
     * 指针捕获(全页唯一口径)。`setPointerCapture` 在 pointerId 不是**活动指针**时
     * 抛 `NotFoundError` —— 触控笔切换、指针已抬起、以及无头脚本合成的 PointerEvent
     * 都会命中。两条缩放条本来各写了一份 try/catch;泳道 / 标尺 / 选区手柄 / 底部条 /
     * 检查器旋钮与滑杆这些点上没写,而那几处的 `setPointerCapture` 恰好排在
     * `local.xxxDrag = …` **之前** —— 一抛就把整个手势的建态语句甩掉,按下之后所有
     * pointermove 都当悬停走,失败形态是「按住拖动完全没反应且零报错」。收成一个 helper,
     * 捕获拿不到就退化成不捕获(窗级/元素级的松手兜底照旧收尾)。
     */
    function capturePointer(el, e) {
        if (!el || typeof el.setPointerCapture !== "function") return;
        try {
            el.setPointerCapture(e.pointerId);
        } catch {
            /* 非活动指针:不捕获也能跑,松手兜底见各自 up 处 */
        }
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
            local.vslDrag ||
            // 两条缩放条(评审【建议】6):拖它们同样是一路 rAF 重绘,漏进来
            // 会让帧时账在缩放拖拽期收不到样本 —— §6.1 的三档降级序列对
            // 「15 泳道连续换倍率」这个最重的路径恰好失效
            local.hzoomDrag ||
            local.vzoomDrag
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
        // **零改动闸**(评审【重要】4;与边界拖拽 / 检查器输入框的零位移闸同族):
        // pointerup 无条件走这里,原地点一下杆(值落回同一量化档)也会整包下发
        // setVadParams/setSegmentation —— C++/mock 侧 300ms 防抖照跑整条流水线,
        // 回来一帧空 diff 让 A-02 变更列表白闪一下。本次手势一个值都没改过就什么
        // 都不发,倒计时条也不挂。`sliderDirty` 由四条写入路径(按下 / 移动 /
        // 松手补值 / 方向键)在 setSliderValue 真的改了值时置位。
        //
        // ⚠ 脏位**按杆记账**(`s.dirty`),不是全页一个共享位:键盘档的松手是
        // 250ms 后异步触发的,共享位下「这 250ms 内在另一根杆上按一下」会把
        // 前一根杆的脏位清掉 ⇒ 那一手势的整包下发与倒计时条被整条吞掉
        // (两位校验员各自实测到,api 不同组也会互吃)。
        const dirty = !!s.dirty;
        s.dirty = false;
        // clearTimeout 必须排在 `!dirty` 早退**之后**:节流尾包是跨杆共享的
        // 单例,零改动的那一次释放去清它,会把别的杆已排队的最后一档值丢掉
        // 且不补发 —— 修复反而制造新的丢包路径。
        if (!dirty) return;
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
            ch: Math.floor(y / local.laneH) + 1,
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

    // ---- tour 视图层增强(T36b 第四轮:步 28/29 可视化)----------------------
    // 只动渲染/demo 视图:改行高、改视口、写本地工作选区,不写 state、不触桥。
    // 进入步 28/29 由 tour.js 的 per-step 动作钩子调用;退出该步或 tour 结束
    // 经 resetWaveView() 还原(恢复缩放 + 清选区)。
    let tourView = null; // {laneH, vp} 进入增强前的视图态(还原用)

    function zoomLanes() {
        if (!tourView) {
            tourView = { laneH: local.laneH, vp: timeline.viewport() };
        }
        applyLaneH(LANE_H_MAX); // 放大泳道:看清每条泳道 pan/vol 阶梯曲线的档位
        clearSelection();
    }

    function showDemoSelection() {
        if (!tourView) {
            tourView = { laneH: local.laneH, vp: timeline.viewport() };
        }
        const d = timeline.durationS();
        // 横向缩到 0..8s:默认全长视口下 2–4s 选区会被压成看不清的缝,必须放大。
        timeline.set({ startS: 0, endS: Math.min(8, d) });
        setSelection(2, Math.min(4, d));
    }

    function showDemoSegment() {
        // 选第一个有段的轨的第一段(每段都带 pan/vol 数据),让检查器立即有可编辑内容。
        const chans = (getStore().segments || {}).channels || [];
        for (const c of chans) {
            if (c && c.segments && c.segments.length) {
                selectSegment(c.ch, 0);
                return;
            }
        }
    }

    function resetWaveView() {
        if (tourView) {
            applyLaneH(tourView.laneH);
            timeline.set(tourView.vp);
            clearSelection();
            tourView = null;
        }
        clearSegSelection(); // 段检查器示例选中(步 33)还原;幂等
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
        // 选中态现在有可见落点(共享动态层的高亮,裁定③)→ 必须标脏重画,
        // 否则 render() 末尾的 schedulePaint 会因两个脏位全空而空转
        local.overlayDirty = true;
        requestRender();
    }

    function clearSegSelection() {
        if (!local.selectedSegs.length && !local.selectedCh) return;
        local.selectedCh = 0;
        local.selectedSegs = [];
        local.echo = {};
        local.overlayDirty = true; // 同上:高亮要擦掉
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

    /**
     * 「重采集选区」开关(SL-193,用户 v5.4 实测拍板 2026-08-27)。
     *
     * 原先是一次性按钮:按下去只在别处冒一条 badge,开关本身没有任何持续状态,
     * 用户「根本不知道是否有正确开始采集」。改成开关后两个方向都要有:
     *   ON  → recaptureArm(mask, startS, endS, autoStop)(§1.23 布防)
     *   OFF → recaptureArm(0, 0, 0)(§1.23 语义行的**撤防**约定:返回 {armed:false}
     *         且不带 reason —— 契约本来就有这条路,不新增桥函数)
     *
     * **零乐观值**:本函数一个字都不写 local 状态,开关的 data-on / aria-checked
     * 一律等 `scvb.state.recapture.armed` 经 25Hz 事件回读后由 render() 落笔
     * (§1.23「UI 以返回值而非乐观假设点亮」)。撤防同理 —— 点完到状态翻转之间
     * 那 ≤40ms 里开关保持旧态,好过先翻再被回读打回去。
     */
    async function toggleRecapture() {
        if (btnDisabled(els.btnRecapture) || isWriteBlocked()) return;
        const rec = (getStore().state || {}).recapture || null;
        if (rec && rec.armed) {
            // 撤防不需要 scope:选区早已被改掉/清掉时也必须关得掉,否则开关会卡在 ON。
            await call("recaptureArm", 0, 0, 0);
            setToolbarNote(null);
            requestRender();
            return;
        }
        const scope = currentScope();
        if (!scope) return;
        const autoStop = !!(els.autostop && els.autostop.checked);
        // armed:false 按 §5.5 四值 reason 出行内说明、不点亮
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
        // 计数只在开框这一刻定格,填文本交给 render(见 renderReidentifyBody)。
        local.reidentifyCounts = { k: counts.marks, l: counts.locked };
        show(els.confirmReidentify, true);
        renderReidentifyBody();
    }

    /**
     * 重新识别确认框正文填充(code-review finding 6)。
     *
     * 正文节点带 `data-t="wave.reidentifyConfirm"`,而 `applyI18n` 会把整串
     * textContent 换成词条**原文**(含 `{k}` / `{l}` 占位符)—— 老写法只在开框
     * 那一次做格式化,框开着切语言就当场退回「将清除 {k} 个…{l} 个…」。
     * `data-t` 不能删(check-i18n 靠它盘点覆盖,也是未注入词典时的兜底原文),
     * 所以走另一半:**框可见就在 render 里补填**。refreshI18n = applyI18n +
     * render(),补填必然排在整串覆盖之后。
     */
    function renderReidentifyBody() {
        const box = els.confirmReidentify;
        if (!box || box.hidden || !local.reidentifyCounts) return;
        const body = box.querySelector("[data-t]");
        if (!body) return;
        const txt = fmtKey("wave.reidentifyConfirm", local.reidentifyCounts);
        if (body.textContent !== txt) body.textContent = txt;
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
            // 契约 §1.24:清除后 UI 须重新 requestWaveform —— 块缓存整轨失效。
            // **硬删**(keepStale:false):数据真被删了,拿旧块给过渡帧垫底
            // 等于画一段已经不存在的波形。
            for (const ch of local.lanePick) {
                waveSource.invalidate(ch, null, { keepStale: false });
            }
            local.canvasVp.clear();
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
        // 缩放拖拽条两枚(Wave 4 用户新件)
        els.hzoomRow = $("wave-hzoom"); // 底部条本体:承 `--wave-gutter`(竖轨对列)
        els.hzoomBar = $("wave-hzoom-bar");
        els.hzoomThumb = $("wave-hzoom-thumb");
        els.hzoomVal = $("wave-hzoom-value");
        els.vzoom = $("wave-vzoom");
        els.vzoomThumb = $("wave-vzoom-thumb");
        els.vzoomVal = $("wave-vzoom-value");
        els.rangeline = $("wave-rangeline");
        els.hint = $("wave-trackpickhint");
        els.chip = $("wave-selchip");
        els.recapRow = $("wave-recapture-row");
        els.recapBadge = $("wave-recapture-badge");
        els.applying = $("wave-applying-spinner");
        els.applyBtn = $("wave-btn-applysegments");
        els.btnRecapture = $("wave-btn-recapture");
        // [SL-193] 开关外壳:五态标签与状态点挂在它的 data-recap 上(CSS 派生)。
        els.recapToggle = $("wave-recapture-toggle");
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
        // 「这一勾是不是用户自己点的」——撤防复位时据此区分:state 回显写进来的
        // 勾要清掉(否则下一次布防悄悄复用旧 autoStop),用户手动勾的意图要留着。
        if (els.autostop) {
            els.autostop.addEventListener("change", () => {
                local.autostopUser = !!els.autostop.checked;
            });
        }
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
        els.inspToggle = $("wave-btn-inspector");
        els.inspClose = $("inspector-close");
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
        // [SL-230] 检查器里的「恢复自动」入口(四件:行 / 说明 / 触发钮 / 取消 + 继续)
        els.inspRestore = $("inspector-restore");
        els.inspRestoreText = $("inspector-restore-text");
        els.inspRestoreBtn = $("inspector-restore-btn");
        els.inspRestoreCancel = $("inspector-restore-cancel");
        els.inspRestoreOk = $("inspector-restore-ok");

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
                stale: gb("stale"),
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
                box, // 悬停说明挂整件(短标 + 值 + 轨都能触发,26px 短标太小)
                val: $(def.gb + "-val"),
                track: box ? box.querySelector(".wave-slider__track") : null,
            };
        });
        // 共享动态层与播放头的纵向覆盖高度 = 15 × 行高(滚动层内容高;
        // 行高可变 ⇒ 统一走 applyLaneH,它同时写 CSS 变量与两层高度)
        applyLaneH(local.laneH, false);

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
                // 舞台宽变了 ⇒ 停播态的竖线像素位同样要补算(与 timeline.onChange
                // 同因;见 refreshPlayheadGeometry 头注)
                refreshPlayheadGeometry();
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
        mountZoomBars();
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
            // 波形本体「粉 + 白」两色(Wave 4 用户裁定;--wave-env 是稿内原值存档)
            if (v("--wave-env-pink")) pal.env = v("--wave-env-pink");
            if (v("--wave-env-core")) pal.envCore = v("--wave-env-core");
            // 透明度**从 waveform.js import**(本函数只换 rgb 真值)—— 两处各写
            // 一份字面 alpha 会静默失同步(对抗校验 major);顶缘线无 alpha 纪律,
            // 整条 rgba 走 --wave-vad-edge。
            if (v("--wave-vad")) {
                pal.vad = `rgba(${v("--wave-vad")}, ${VAD_ALPHA})`;
            }
            if (v("--wave-vad-edge")) pal.vadEdge = v("--wave-vad-edge");
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
                s.dirty = false; // 本次手势的「改过值没有」账从零开始(按杆记)
                capturePointer(s.track, e);
                const v = sliderValueFromEvent(s, e);
                if (v !== null && setSliderValue(s, v)) {
                    s.dirty = true;
                    sendParamsThrottled(s.def.api);
                }
                ensureTicker();
            });
            s.track.addEventListener("pointermove", (e) => {
                if (local.sliderDrag !== s) return;
                const v = sliderValueFromEvent(s, e);
                if (v !== null && setSliderValue(s, v)) {
                    s.dirty = true;
                    sendParamsThrottled(s.def.api);
                }
            });
            const up = (e) => {
                if (local.sliderDrag !== s) return;
                local.sliderDrag = null;
                const v = e ? sliderValueFromEvent(s, e) : null;
                if (v !== null && setSliderValue(s, v)) {
                    s.dirty = true;
                }
                releaseSlider(s);
            };
            s.track.addEventListener("pointerup", up);
            s.track.addEventListener("pointercancel", up);
            // 窗级兜底(与两条缩放条同口径):setPointerCapture 抛错 / 指针在窗外
            // 释放时松手事件不回到杆上,拖拽态会卡住 —— 此后任何一次经过本杆的
            // 悬停都会当成拖动直接改值。窗级这一道拿不到杆内坐标,故只收尾不取值。
            if (typeof window !== "undefined") {
                window.addEventListener("pointerup", () => up(null));
                window.addEventListener("pointercancel", () => up(null));
            }
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
                if (setSliderValue(s, v)) {
                    s.dirty = true;
                    sendParamsThrottled(s.def.api);
                }
                if (local.sliderKeyTimer) clearTimeout(local.sliderKeyTimer);
                local.sliderKeyTimer = setTimeout(() => {
                    local.sliderKeyTimer = 0;
                    releaseSlider(s);
                }, SLIDER_KEY_RELEASE_MS);
            });
        }

        // 四动作 + 抑制期显式应用 + 合并(判据在 render,入口再复检)
        // [SL-193] 重采集是 role="switch" 的自绘开关(不是 <button>),原生的
        // 「空格/回车 = 点击」不会自动来 —— 键盘可达要自己接,否则 tabindex 给了
        // 焦点却按不动。口径与 Tab1 的 onActivate 一致(空格与回车都算,阻默认滚动)。
        if (els.btnRecapture) {
            els.btnRecapture.addEventListener("click", toggleRecapture);
            els.btnRecapture.addEventListener("keydown", (e) => {
                if (e.key === " " || e.key === "Enter") {
                    e.preventDefault();
                    toggleRecapture();
                }
            });
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
                // 可拖窗 = 左段起点 +50ms .. 右段终点 −50ms(两侧各留最短段)。
                // 相邻两段**总宽 <100ms** 时 minS > maxS,窗反转:updateBoundDrag 的
                // clamp `min(max(t,minS),maxS)` 恒取 maxS,手柄钉在左段内部一动不动,
                // 释放时又必然 !== origT ⇒ 照发 move_boundary,把边界推到左段里去
                // (PR#64 评审【重要】5)。窗不成立就**不建 boundDrag**,让这一按
                // 落到②的点选/平移路径 —— 这类段只能用分割/合并处理。
                const minS = j < 0 ? 0 : num(segs[j] && segs[j].t0S, 0) + 0.05;
                // 右限走 segEndS:split 保留哨兵(SegmentEdit.cpp 的 b.t1 原样继承),
                // 尾段 openEnded 时 t1S 只是保守下界 —— 在已知末端之外分割后,裸 t1S
                // 会把窗翻到边界左边(手柄一按左跳且推不回,松手照发 move_boundary)。
                // openEnded(+Infinity)/缺段(0)都回落时间线末端。
                const nextEnd = j < 0 ? 0 : segEndS(segs[j + 1]);
                const maxS =
                    j < 0
                        ? 0
                        : (Number.isFinite(nextEnd) && nextEnd > 0
                              ? nextEnd
                              : timeline.durationS()) - 0.05;
                if (j >= 0 && minS < maxS) {
                    e.preventDefault();
                    capturePointer(els.lanes, e);
                    local.boundDrag = {
                        ch: p.ch,
                        segIdx: j + 1, // 边界 j = 段 j+1 的 t0(edge:"t0")
                        tS: bounds[j].tS,
                        origT: bounds[j].tS, // 原位(拖动期动态层跳过原线画预览线)
                        snapped: false,
                        minS,
                        maxS,
                    };
                    local.overlayDirty = true;
                    schedulePaint();
                    ensureTicker();
                    return;
                }
            }
            // ② 其余:按下未定性 —— 位移 >3px 转空白平移(05 行 319),
            //    原地抬起转舞台点选(段 → 检查器;空白 → 勾选该轨)
            capturePointer(els.lanes, e);
            local.pendingDown = {
                x: p.x,
                ch: p.ch,
                shift: e.shiftKey,
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
            // 走统一出口:就地置 null 会漏掉 showBoundHandle(false),9×26 手柄
            // 留在屏上、.wave-lanes 的 ew-resize 光标也不复位(三条出口口径
            // 不一致的那条就是这里)。
            if (local.boundDrag) cancelBoundDrag();
            local.panDrag = null;
            local.pendingDown = null;
            local.overlayDirty = true;
            schedulePaint();
        });
        els.lanes.addEventListener("pointerleave", () => {
            if (!local.boundDrag) showBoundHandle(false);
        });

        // 双击两义(05 行 313):
        //   · 双击**段间边界线**(±BOUNDARY_HIT_PX)= 删掉这条边界 = 合并左右两段
        //     ([SL-207] 用户 v5.4 实测拍板 2026-08-27);
        //   · 双击**段内**(离边界够远)= 在此分割(split 两子段继承原值,§5.4)。
        //
        // 边界判定**必须排在分割前面**:边界两侧各 6px 落在左右两段内部,先跑
        // findIndex 的话双击边界会被判成「在段内某处」而去分割 —— 用户想删边界,
        // 结果又多出一条边界,正好相反。
        //
        // 命中口径与拖拽边界**逐字复用同一套**(boundariesOf → timeToX → nearestHit
        // ±BOUNDARY_HIT_PX):手柄摸得到的地方,双击就删得掉,不另立第二套判定。
        els.lanes.addEventListener("dblclick", (e) => {
            // 刚提交过边界拖拽 ⇒ 这一下多半是「连着微调同一条边界两次」被浏览器判成的
            // dblclick,不是任何一种双击意图。**整个处理器让路**(复审终轮①):
            // 原先只在边界分支里挡,`j < 0` 那一档会漏出去 —— 把边界拖走之后落点常常
            // 已经出了 ±6px,于是那一下照发**分割**,等于用户想细调却多出一条边界。
            // 放到开头既堵死这一档,也比「每个分支各挡一次」简单。
            if (mergeGuardedByDrag(Date.now(), local.boundCommitAt)) return;
            const p = lanesPoint(e);
            if (!p || p.ch < 1 || p.ch > LANE_COUNT) return;
            const stageW = stageWidth();
            if (!(p.x >= 0 && p.x <= stageW)) return;
            if (isWriteBlocked() || isLaneDead(p.ch)) return;
            const vp = timeline.viewport();
            const t = xToTime(vp, stageW, p.x);
            const segCh = segmentsOfCh(getStore().segments, p.ch);
            const segs = (segCh && segCh.segments) || [];

            // ① 边界:boundariesOf 的第 j 条 = segs[j] 与 segs[j+1] 之间那条
            const xs = boundariesOf(segCh).map((b) =>
                timeToX(vp, stageW, b.tS),
            );
            const j = nearestHit(p.x, xs, BOUNDARY_HIT_PX);
            if (j >= 0 && segs[j] && segs[j + 1]) {
                // 走既有 merge 桥函数与既有口径:editSegment(§1.16)本就进撤销栈,
                // 与工具条「合并选中两段」逐字同一条路,不另加确认框 —— 误删一条
                // 边界的代价是一次 Ctrl+Z,弹框反而挡住连续修边界的手感。
                sendEdit(p.ch, "merge", { segIdxA: j, segIdxB: j + 1 });
                return;
            }

            // ② 段内:在此分割
            const idx = segs.findIndex((s) => t >= s.t0S && t < segEndS(s));
            if (idx < 0) return;
            const s = segs[idx];
            if (!(t > s.t0S + 0.05 && t < segEndS(s) - 0.05)) return;
            sendEdit(p.ch, "split", {
                segIdx: idx,
                tS: Math.round(t * 1000) / 1000,
            });
        });

        // 滚轮四路映射(05 行 319;[SL-205] 用户 v5.4 实测拍板 2026-08-27)。
        //
        //   Ctrl  → 横向缩放(以光标为锚)      Alt   → 纵向缩放(泳道行高)
        //   Shift → 纵向平移                    裸    → 横向平移
        //
        // **改前只实装了 Ctrl 一路**(`if (!e.ctrlKey) return`),另外三路连分支
        // 都没有;而且没被 preventDefault 的滚轮会**漏给浏览器默认动作** ——
        // 页面级实测:裸滚轮把泳道列竖着滚跑了 121px。用户报的三条(Alt 纵缩放
        // 没有、Shift 纵向平移没有、裸滚轮横向平移没有)由此而来。
        //
        // **挂整个 Tab3 面板**,不是只挂泳道窗:Ctrl+滚轮是浏览器的页面缩放手势,
        // 没被 preventDefault 的地方(工具条、检查器、窗外留白)会让**整页**跟着
        // 缩放 —— 所有卡片一起变、看着就是闪一下(用户实测「放大缩小的时候全部
        // 卡片都会变白」)。面板内一律拦下:落在泳道窗内的按视口缩放,落在别处的
        // 只拦截、不缩放(用户显然不是想缩放浏览器)。
        // 另外三路则**只在泳道窗内**接管并 preventDefault —— 窗外(检查器、参数
        // 区)的普通滚轮必须照旧交给浏览器,否则那些地方就滚不动了。
        // [SL-205 增补,用户实测 2026-08-27]「需要点一下,shift 滚轮和纯滚轮才有作用」。
        //
        // **页面层查下来没有任何焦点前置条件**:CDP 可信事件实测(切到本页之后**不曾在
        // 泳道里点过一下**)四路全部生效 —— 见 web-preview/tests/wheel-route-probe.mjs。
        // 也就是说这一条不在 web 这一层,在**宿主 / WebView2 的窗口焦点**那一层:
        // WebViewHost 至今没有任何焦点处理(`grep -n "KeyboardFocus\|focus" 那个文件` 零命中),
        // WebView2 子窗口要等用户点一下才拿到焦点,在那之前滚轮消息不一定路由过来。
        //
        // 能在 web 侧做的就这一件:**指针进入泳道窗时把焦点收进来**。三道闸防止它变成
        // 抢焦点:①只在泳道窗上挂(不是整页);②本文档已经有焦点就不动;③焦点正停在
        // 可编辑控件上时一律不动(别把人正在打的字打断)。
        // ⚠ **本条只能在真 DAW 里验**:无头浏览器里页面恒有焦点,这段代码根本不会执行。
        //
        // [统筹裁定 2026-08-28,#123 终轮] **保留 pointerenter 方案,不改「首个 wheel 事件
        // 按需取焦」**:两案在无头环境都无法验证(见上),差异只能真机分辨;而按需取焦有
        // 「第一记滚轮先被焦点路由吃掉、用户要滚两次」的先验风险,pointerenter 没有。
        // 若 v5.6 真机复验(SL-221)证明本方案仍需先点一下,再连同 C++ 焦点策略一起换案。
        if (els.window) {
            els.window.addEventListener("pointerenter", () => {
                if (typeof document === "undefined") return;
                if (document.hasFocus && document.hasFocus()) return;
                // 复用 context-menu.js 的白名单判定(复审终轮②):这里原先是全仓
                // **第三份**可编辑判定,而且是收窄之前的裸 `input` 版 —— 会把
                // `type=range`(Tab3 参数条就是滑杆)也当成「正在编辑」而放弃收拢焦点。
                if (isEditableTarget(document.activeElement)) return;
                if (typeof window !== "undefined" && window.focus)
                    window.focus();
            });
        }

        const wheelHost = els.panel || els.window || els.lanes;
        wheelHost.addEventListener(
            "wheel",
            (e) => {
                const route = wheelRoute(e);
                const inWindow =
                    els.window &&
                    typeof els.window.contains === "function" &&
                    els.window.contains(e.target);
                if (route === "hzoom") {
                    e.preventDefault(); // 先拦下页面缩放,再决定要不要动视口
                    if (!inWindow) return;
                    const stageW = stageWidth();
                    if (!(stageW > 0)) return;
                    const x = stageXFromClient(e.clientX);
                    const vp = timeline.viewport();
                    const anchorT = xToTime(
                        vp,
                        stageW,
                        Math.min(Math.max(x, 0), stageW),
                    );
                    // 取向按**归一后**的位移定,不再直接看 deltaY:行模式与
                    // Shift 改写都可能让 deltaY 是 0(见 shared/wheel.js)。
                    // **零位移早退**(复审建议①):与下面三路同一口径 —— 归一后为 0
                    // 说明这一发根本没位移,再往下走会按 `< 0 ? 放大 : 缩小` 白缩一格。
                    const dz = wheelPx(e);
                    if (!dz) return;
                    timeline.zoom(anchorT, dz < 0 ? ZOOM_STEP : 1 / ZOOM_STEP);
                    return;
                }
                if (!inWindow) return;
                e.preventDefault();
                const px = wheelPx(e);
                if (!px) return;
                if (route === "vzoom") {
                    // 上滚(px<0)= 放大 = 行变高,与纵向缩放条「上 = 高」同向
                    applyLaneH(
                        local.laneH + (px < 0 ? 1 : -1) * LANE_H_WHEEL_STEP,
                    );
                    return;
                }
                if (route === "vpan") {
                    // 泳道列自己就是纵向滚动容器(lanesPoint 读的也是它的 scrollTop)
                    if (els.lanes) els.lanes.scrollTop += px;
                    return;
                }
                // 裸滚轮:横向平移。像素 → 秒按当前视口的 px/s 折算,
                // 于是「滚一格挪多远」在任何缩放档下观感一致。
                const stageW = stageWidth();
                const span = spanOf(timeline.viewport());
                if (!(stageW > 0) || !(span > 0)) return;
                timeline.pan((px * span) / stageW);
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
        const idx = segs.findIndex((s) => t >= s.t0S && t < segEndS(s));
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

    /**
     * 手柄纵向落点(泳道内垂直居中,B-10)。26px 是手柄高;行高压到 22..25px 档时
     * 手柄比泳道高,居中后上下各溢出 (26−h)/2 —— 溢出到相邻泳道无碍(z-index:4
     * 浮标)。**只有第一条轨要夹**:ch=1 且 h<26 时居中会算出负 top(22px 档 =
     * −2px),被 `.wave-lanes` 的滚动裁剪切掉,手柄顶部缺一角 —— 这里下夹到 0。
     */
    function bhandleTop(ch) {
        const h = local.laneH;
        return Math.max((ch - 1) * h + (h - 26) / 2, 0) + "px";
    }

    /**
     * 手柄可发现性(Wave 4 用户反馈⑦:「这个把手是干嘛的」= 可发现性缺陷)。
     * title 走词条 —— 常态说清「分段边界 + 拖动/Alt/双击分割/Delete 合并」,
     * 吸附命中(金色)态换成「已吸附到能量谷」,让金色自解释。
     *
     * ⚠ **只写 title,不写 aria-label**(修订轮更正):手柄节点在 index.html 上
     * 带 `aria-hidden="true"`(纯视觉浮标,不进 tab 序),整个子树被移出无障碍树
     * —— 往上面写 aria-label 是死代码,「AT 悬停可读」不成立。摘掉 aria-hidden
     * 也不解决:非交互 div 上的 aria-label 本就没有稳定的播报路径。边界编辑的
     * 无障碍入口在段检查器(有名的按钮/输入),不在这枚浮标上。
     */
    function setBoundHandleTip(snapped) {
        const t = getT();
        const s = snapped
            ? t["wave.boundarySnapTip"] || ""
            : t["wave.boundaryHandleTip"] || "";
        setTitle(els.bhandle, s);
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
        els.bhandle.style.top = bhandleTop(p.ch);
        attr(els.bhandle, "data-manual", bounds[j].manual ? 1 : 0);
        attr(els.bhandle, "data-snap", 0);
        setBoundHandleTip(false);
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
            els.bhandle.style.top = bhandleTop(d.ch);
            attr(els.bhandle, "data-snap", d.snapped ? 1 : 0);
            setBoundHandleTip(d.snapped);
            show(els.bhandle, true);
        }
        local.overlayDirty = true;
        schedulePaint();
    }

    /**
     * 边界拖拽**作废**(不提交)。用于段表在拖拽期间被换掉的情形:
     * `boundDrag` 在 pointerdown 那一刻把 `segIdx: j+1` 锁死,而契约 §2.8
     * 「`segIdx` 每次事件后重新编号」—— 拖拽期间另一轨分析完成 / 有人撤销 /
     * 切版本,本轨段表整条替换后旧下标要么指向别的段、要么越界拿 `badArg`
     * (PR#64 评审【重要】2)。
     *
     * 为什么是**取消**而不是按 {t0S,t1S} 重绑(同 `rebindSegKeys`):重绑只解决
     * 「这一段还在」的情形,而段表换掉时被拖的那条边界本身可能已经不存在
     * (合并/清除/切版本),此时没有任何正确的 segIdx 可写;且用户按下时看到的
     * 那张图已经不是现在这张,把手势结果套到新表上属于替用户做决定。取消是
     * 唯一无歧义的处置,并且与「释放才发」纪律不冲突 —— 什么都没发过。
     */
    function cancelBoundDrag() {
        if (!local.boundDrag) return;
        local.boundDrag = null;
        showBoundHandle(false);
        if (els.bhandle) attr(els.bhandle, "data-snap", 0);
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
        // 时间戳**只在真发了 move_boundary 时**打(上面那条零位移早退不算):双击的
        // 边界分支据此让路,免得「连着微调两次同一条边界」被浏览器判成 dblclick,
        // 把这条边界删掉(复审【重要】;判据见 mergeGuardedByDrag)。
        local.boundCommitAt = Date.now();
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
                capturePointer(els.rulerScale, e);
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
                capturePointer(h, e);
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
            // 窗级兜底(与滑杆 / 两条缩放条同口径,pr-agent):capturePointer 抛错
            // 或指针在窗外释放时松手事件不回到手柄上,selDrag 会卡在该侧 —— 此后
            // 任何一次经过手柄的悬停都当成拖动改选区。窗级这道只收尾不取值。
            if (typeof window !== "undefined") {
                window.addEventListener("pointerup", up);
                window.addEventListener("pointercancel", up);
            }
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
            capturePointer(els.thumb, e);
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

    // ------------------------ mount:缩放拖拽条两枚(T33 Wave 4 用户新件)------
    /**
     * 横向缩放条:拖动改视口跨度,**以视口中心为锚**(与 Ctrl+滚轮同一
     * `timeline.zoom(anchorT, factor)` API,只是锚点从光标换成中心)。
     * 底部滚动条(平移)不动,两件形制两分(见 index.html 的 CSS 注释)。
     * 纵向缩放条:拖动改 `local.laneH`(22..88px)→ applyLaneH 全 canvas 重绘。
     * 两条都键盘可达(方向键步进)+ role=slider/aria-valuenow(render 里同步)。
     */
    function mountZoomBars() {
        // ---- 横向:行程比 p ∈ [0,1] ↔ 倍率(对数刻度,zoomFactorFromPercent)
        const applyHZoomPercent = (p) => {
            const d = timeline.durationS();
            const vp = timeline.viewport();
            const anchorT = (vp.startS + vp.endS) / 2; // 视口中心为锚
            const nextSpan = d / zoomFactorFromPercent(p, d);
            const cur = spanOf(vp);
            if (!(nextSpan > 0) || !(cur > 0)) return;
            timeline.zoom(anchorT, cur / nextSpan);
            requestRender();
        };
        const hzoomPercentFromEvent = (e) => {
            const rect = els.hzoomBar.getBoundingClientRect();
            if (!(rect.width > 0)) return null;
            return Math.min(
                Math.max((e.clientX - rect.left) / rect.width, 0),
                1,
            );
        };
        if (els.hzoomBar) {
            els.hzoomBar.addEventListener("pointerdown", (e) => {
                if (e.button !== 0) return;
                e.preventDefault();
                capturePointer(els.hzoomBar, e);
                local.hzoomDrag = true;
                const p = hzoomPercentFromEvent(e);
                if (p !== null) applyHZoomPercent(p);
                ensureTicker();
            });
            els.hzoomBar.addEventListener("pointermove", (e) => {
                if (!local.hzoomDrag) return;
                // 兜底:捕获没拿到时 pointerup 会落在别处,拖拽态清不掉 ——
                // 键已松(buttons=0)就当场收尾,否则此后任何一次悬停经过
                // 本杆都会直接跳缩放
                if (e.buttons === 0) {
                    local.hzoomDrag = null;
                    return;
                }
                const p = hzoomPercentFromEvent(e);
                if (p !== null) applyHZoomPercent(p);
            });
            const hup = () => {
                local.hzoomDrag = null;
            };
            els.hzoomBar.addEventListener("pointerup", hup);
            els.hzoomBar.addEventListener("pointercancel", hup);
            // setPointerCapture 抛错(触控笔切换 / pointerId 已失效 / 无头合成
            // 事件)时松手事件不会回到杆上,所以窗级再收一道
            if (typeof window !== "undefined") {
                window.addEventListener("pointerup", hup);
                window.addEventListener("pointercancel", hup);
            }
            // 键盘(role=slider 全套):方向键 = 一格 ZOOM_STEP(与 Ctrl+滚轮
            // 一格同量);PageUp/PageDown = 四格大步;Home/End = 全览 / 最大倍率
            els.hzoomBar.addEventListener("keydown", (e) => {
                const vp = timeline.viewport();
                if (e.key === "Home" || e.key === "End") {
                    e.preventDefault();
                    applyHZoomPercent(e.key === "Home" ? 0 : 1);
                    return;
                }
                const big =
                    e.key === "PageUp" ? 1 : e.key === "PageDown" ? -1 : 0;
                const dir =
                    big ||
                    (e.key === "ArrowRight" || e.key === "ArrowUp"
                        ? 1
                        : e.key === "ArrowLeft" || e.key === "ArrowDown"
                          ? -1
                          : 0);
                if (!dir) return;
                e.preventDefault();
                const step = Math.pow(ZOOM_STEP, big ? 4 : 1);
                timeline.zoom(
                    (vp.startS + vp.endS) / 2,
                    dir > 0 ? step : 1 / step,
                );
                requestRender();
            });
        }

        // ---- 纵向:轨**底** = 最矮行,轨**顶** = 最高行(Wave 6 竖回来后取值改读
        //      clientY —— 向上拖 = 泳道变高,与竖轨的空间直觉同向)
        const vzoomHeightFromEvent = (e) => {
            const rect = els.vzoom.getBoundingClientRect();
            if (!(rect.height > 0)) return null;
            return laneHFromPercent(1 - (e.clientY - rect.top) / rect.height);
        };
        if (els.vzoom) {
            els.vzoom.addEventListener("pointerdown", (e) => {
                if (e.button !== 0) return;
                e.preventDefault();
                capturePointer(els.vzoom, e);
                local.vzoomDrag = true;
                const h = vzoomHeightFromEvent(e);
                if (h !== null) applyLaneH(h);
                ensureTicker(); // 同横向杆:拖拽期喂帧时账(interactionActive 已含)
            });
            els.vzoom.addEventListener("pointermove", (e) => {
                if (!local.vzoomDrag) return;
                if (e.buttons === 0) {
                    // 同横向杆:捕获失败时的兜底收尾(见上)
                    local.vzoomDrag = null;
                    return;
                }
                const h = vzoomHeightFromEvent(e);
                if (h !== null && h !== local.laneH) applyLaneH(h);
            });
            const vup = () => {
                local.vzoomDrag = null;
            };
            els.vzoom.addEventListener("pointerup", vup);
            els.vzoom.addEventListener("pointercancel", vup);
            if (typeof window !== "undefined") {
                window.addEventListener("pointerup", vup);
                window.addEventListener("pointercancel", vup);
            }
            // 键盘(role=slider 全套):上/右 = 加高一步,下/左 = 减矮一步
            // (2px 一步,22..88 共 33 步);PageUp/PageDown = 10px 大步;
            // Home/End = 最矮/最高
            els.vzoom.addEventListener("keydown", (e) => {
                if (e.key === "Home" || e.key === "End") {
                    e.preventDefault();
                    applyLaneH(e.key === "Home" ? LANE_H_MIN : LANE_H_MAX);
                    return;
                }
                const big =
                    e.key === "PageUp" ? 1 : e.key === "PageDown" ? -1 : 0;
                const dir =
                    big ||
                    (e.key === "ArrowUp" || e.key === "ArrowRight"
                        ? 1
                        : e.key === "ArrowDown" || e.key === "ArrowLeft"
                          ? -1
                          : 0);
                if (!dir) return;
                e.preventDefault();
                applyLaneH(local.laneH + dir * (big ? 10 : 2));
            });
        }
    }

    // ------------------------------------------------ mount:段检查器(§17)
    function mountInspector() {
        /**
         * 面板开关(裁定④):工具条那枚 aria-pressed 按钮与标题栏 ✕ 同一个本地态。
         * 开关一按,左栏宽度就变 → 舞台宽变 → canvas 后备存储要按新宽重建,
         * 两层都标脏(与 applyLaneH 同一笔账);blit 老底也清 —— 那幅是旧舞台宽的
         * 位图,drawImage 上去会横向拉伸。
         */
        const setInspectorOpen = (on) => {
            const v = !!on;
            if (local.inspectorOpen === v) return;
            local.inspectorOpen = v;
            local.canvasVp.clear();
            local.staticDirty = true;
            local.overlayDirty = true;
            schedulePaint();
            requestRender();
        };
        if (els.inspToggle) {
            els.inspToggle.addEventListener("click", () => {
                setInspectorOpen(!local.inspectorOpen);
            });
        }
        if (els.inspClose) {
            els.inspClose.addEventListener("click", () => {
                setInspectorOpen(false);
            });
        }

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
                capturePointer(els.inspPanKnob, e);
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
                capturePointer(els.inspVolSlider, e);
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
            // [SL-230]「恢复自动」三枚钮:两态就地切换,确认后走轨级 clearManual。
            const askRestore = (on) => {
                const cur = on ? currentSeg() : null;
                local.inspRestoreAsk = cur ? `${cur.ch}:${cur.idx}` : "";
                requestRender();
            };
            if (els.inspRestoreBtn) {
                els.inspRestoreBtn.addEventListener("click", () =>
                    askRestore(true),
                );
            }
            if (els.inspRestoreCancel) {
                els.inspRestoreCancel.addEventListener("click", () =>
                    askRestore(false),
                );
            }
            if (els.inspRestoreOk) {
                els.inspRestoreOk.addEventListener("click", async () => {
                    // 在途去重(#148 复审【建议】②):钮要等下一次 render 才隐,快速双击
                    // 能发出两次 analyze —— 第二次基本是空操作,但白跑一趟秒级离线分析。
                    // 口径同轨道页那份的 local.reidentifying(try/finally 兜住抛错)。
                    if (local.inspRestoreBusy) return;
                    const cur = currentSeg();
                    local.inspRestoreAsk = "";
                    if (!cur || isWriteBlocked() || isLaneDead(cur.ch)) {
                        return requestRender();
                    }
                    // [SL-242] 作用面 = **选中这一段的区间**,不是整轨。
                    // §1.6 的 scope 本来就是 `{tracksMask, startS?, endS?}`,带上范围
                    // 是照契约用它,不是扩契约(真桥那一侧 analyzeScopeRange 早就认
                    // 「两头都给 ⇒ 逐字照用」)。判据与 openEnded 那档见
                    // segmentRestoreScope 的头注。
                    const scope = segmentRestoreScope(cur.ch, cur.seg);
                    if (!scope) return requestRender(); // 段没有可用的 t0S:不发轨级请求
                    local.inspRestoreBusy = true;
                    try {
                        await call("analyze", scope, { clearManual: true });
                    } finally {
                        local.inspRestoreBusy = false;
                    }
                    requestRender();
                });
            }
            els.inspLock.addEventListener("keydown", (e) => {
                if (e.key === " " || e.key === "Enter") {
                    e.preventDefault();
                    flip();
                }
            });
        }
    }

    /**
     * 检查器数字输入通用接线:Enter/失焦提交,方向键步进后同径提交。
     *
     * **零位移不发**(code-review finding 3,严重;与本分支 `commitBoundDrag`
     * 的「零位移不发」同一族):`set_values` 的后置是 `origin=user_edited` +
     * `locked=true`(契约 §5.4),而重新识别对 `locked` 段免疫 —— 老写法 blur
     * 无条件 commit,于是「Tab 键掠过 PAN/VOL 框」或「点另一个段(触发 blur)」
     * 就能把一个 auto 段永久变成 edited+locked:用户零可见变化,却不可逆。
     * 这里记住**最后一次渲染进框的解析值**(`numBoxes` 的 `last`,由
     * `setNumBox` 在 render 写值时同步),解析值与它相等就一个字节都不发。
     *
     * **Enter 不再发两次**(finding 4):`commit(); input.blur();` 里 blur 事件
     * 会再进一次 commit ⇒ 两次 `set_values`、两条撤销栈。commit 成功后把 `last`
     * 推到刚送出的值(段表要等 §2.8 回推才更新,不能拿模型值当去重基准),
     * 紧接着那次 blur 的 commit 就落进上面的零位移早退。
     */
    function wireNumInput(input, spec) {
        numBoxes.set(input, { last: null });
        const commit = () => {
            const cur = canEditSelected();
            if (!cur) return;
            const v = parseFloat(String(input.value).replace(",", "."));
            if (!Number.isFinite(v)) {
                requestRender(); // 回显当前值,丢弃非法输入
                return;
            }
            const q = 10 ** spec.dp;
            const q1 =
                Math.round(Math.min(Math.max(v, spec.min), spec.max) * q) / q;
            const st = numBoxes.get(input);
            if (st && st.last !== null && st.last === q1) {
                // 值没动:Tab 掠过 / 点走别处 / Enter 后的那次 blur ⇒ 不上行
                if (input.value !== fmtSigned(q1, spec.dp)) requestRender();
                return;
            }
            if (st) st.last = q1;
            spec.commit(q1);
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
            // 焦点在按钮/自定义控件上时,页级快捷键不吞键(评审【建议】7):
            // Backspace 在这些角色上是浏览器/AT 的既有语义(且 Delete 也不该在
            // 「四动作」按钮上被改写成合并),空格/回车激活按钮之后紧接着的一次
            // Backspace 会莫名合并掉两段。工具条按钮与 role=slider 全部排除。
            const role = a && a.getAttribute && a.getAttribute("role");
            const onControl =
                a &&
                (a.tagName === "BUTTON" ||
                    a.tagName === "SELECT" ||
                    role === "button" ||
                    role === "slider" ||
                    role === "switch" ||
                    role === "checkbox");
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
            if (
                (e.key === "Delete" || e.key === "Backspace") &&
                !inField &&
                !onControl
            ) {
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

        // 词典换了 = 段角标里烤进 aria-label/title 的 `wave.lockBadge` 过期了
        // (code-review finding 7)。角标 DOM 只在 marksDirty 时重建,而切语言走的是
        // applyI18n + render(),既不动视口也不来 segments 事件 —— 不在这里置位,
        // 锁角标的 tooltip/朗读文本就一直停在挂载时那门语言。i18n.dict() 每门语言
        // 返回同一个常量对象,恒等比较即「语言变了」的准信号。
        if (local.lastDict !== t) {
            local.lastDict = t;
            local.marksDirty = true;
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
            // 04 §4.5:该轨上游音频与已采集特征不一致 → ⚠ 角标 + 整句 tooltip。
            // 数据来自 §2.8 segments.channels[].stale(laneModelFromStore 已投影)。
            show(n.stale, !!lane.stale);
            setTitle(n.stale, lane.stale ? t["wave.staleTrack"] || "" : "");
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
            // 悬停说明:七个短标是 mono 微标(THRESHOLD/HOLD/…),不看说明猜不出
            // 各自管什么 —— 用户 preview 就是逐个问过来的。写在 render 里跟得上
            // 切语言;`{d}` 由 02 §0.3 的默认值填,让人知道出厂档在哪。
            const tip = t[s.def.tip];
            if (s.box && tip) {
                const s2 = format(tip, { d: fmtSliderValue(s.def, s.def.def) });
                if (s.box.getAttribute("title") !== s2) {
                    attr(s.box, "title", s2);
                }
            }
        }

        // ---- 四件判据(C-07:各自的 05 判据,行 300-303)
        //
        // [SL-193] 重采集与重分析两件的判据改由 `recaptureBlockReason` /
        // `reanalyzeBlockReason` 出**词条 key**:同一个返回值既决定灰不灰,也就是
        // tooltip 的内容 —— 用户拍板「别让用户干瞪灰按钮」,而判据与理由分两处写的话,
        // 迟早分叉成「灰着但没理由」。重分析那条的定谳(intervals vs tracks + native
        // 恒 0)写在 `reanalyzeBlockReason` 的头注里。
        const scope = currentScope();
        const blocked = isWriteBlocked();
        const hasData = !isLanesEmpty(store);
        // §1.5 dry-run 的**轨数**(不是区段数)= 「选区 ∩ coverage」的承载量。
        // null = 回包还在路上 ⇒ 先放行,到达后收敛(analyze 对空交集也会以
        // {ok:false} 拒绝,双保险)。
        const previewTracks = local.preview
            ? num(local.preview.tracks, 0)
            : null;
        const recArmed = !!((store.state || {}).recapture || {}).armed;
        const recapWhy = recaptureBlockReason({
            blocked,
            armed: recArmed,
            picked: local.lanePick.length > 0,
            hasSel: !!scope,
        });
        const reanWhy = reanalyzeBlockReason({
            blocked,
            picked: local.lanePick.length > 0,
            hasSel: !!scope,
            hasData,
            previewTracks,
        });
        setBtnEnabled(els.btnRecapture, !recapWhy);
        setBtnEnabled(els.btnReanalyze, !reanWhy);
        // tooltip 不走 applyI18n(它只刷 data-t / data-t-aria),故每次渲染按当前
        // 字典重写;可用时 setTitle("") 会把 title **移除**,不留空气泡。
        setTitle(els.btnRecapture, recapWhy ? t[recapWhy] || "" : "");
        setTitle(els.btnReanalyze, reanWhy ? t[reanWhy] || "" : "");
        setBtnEnabled(
            els.btnReidentify,
            hasNonAutoSegments(store.segments) && !blocked,
        );
        setBtnEnabled(els.btnClear, !!scope && !blocked && hasData);

        // ---- [SL-193] 重采集开关的态:ON/OFF 与五态标签全部由真事件回读派生,
        //      零乐观值(§1.23「UI 以返回值而非乐观假设点亮」)。
        //      派生方向单向:state.recapture.armed ⇒ data-on + aria-checked,
        //      **一处落笔**;CSS 侧不得另写一份「开」的视觉(见 index.html 那段行注)。
        //      三处写入都走 attr()(值没变就不碰 DOM)—— 本页 render 是 rAF 合帧的,
        //      无条件 setAttribute 会把开关每帧标脏一次。
        attr(els.btnRecapture, "data-on", recArmed ? "1" : "0");
        attr(els.btnRecapture, "aria-checked", recArmed ? "true" : "false");
        attr(
            els.recapToggle,
            "data-recap",
            recaptureVisual(store.state, store.playhead),
        );

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
        } else if (
            els.autostop &&
            els.autostop.checked &&
            !local.autostopUser
        ) {
            // 撤防后 rec 变 null,勾选框原来会**留着上一轮的态** ⇒ 下一次布防
            // 悄悄复用旧的 autoStop(pr-agent)。这里只复位「不是用户自己勾的」
            // 那一种(= 上一轮由 state 回显写进来的),用户手动勾的意图要留着。
            els.autostop.checked = false;
        }
        // 布防受覆盖区域斜条纹(05 行 300;A-08 同族配方,滚动层内)
        if (armed && stageW > 0) {
            const rx0 = Math.max(timeToX(vp, stageW, num(rec.startS, 0)), 0);
            const rx1 = Math.min(timeToX(vp, stageW, num(rec.endS, 0)), stageW);
            const runs = maskRuns(rec.tracksMask);
            if (rx1 > rx0 && runs.length && els.recapband) {
                els.recapband.style.left = HEAD_W + rx0 + "px";
                els.recapband.style.width = rx1 - rx0 + "px";
                els.recapband.style.height = LANE_COUNT * local.laneH + "px";
                // 只在**布防轨**上画条纹:容器仍是满高(供 top 定位),条纹落在
                // 每段相邻布防轨的子块里(签名比对,避免逐帧重建 innerHTML)
                // 签名带上行高:纵向缩放改行高后子块的 top/height 必须重建
                const sig =
                    local.laneH +
                    ":" +
                    runs.map((r) => r.ch0 + "x" + r.count).join(",");
                if (els.recapband.getAttribute("data-runs") !== sig) {
                    els.recapband.setAttribute("data-runs", sig);
                    els.recapband.innerHTML = runs
                        .map(
                            (r) =>
                                `<span class="wave-recapband__seg" style="top:${(r.ch0 - 1) * local.laneH}px;height:${r.count * local.laneH}px"></span>`,
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
        const durS = timeline.durationS();
        text(els.zoom, zoomLabel(vp, durS));
        if (els.thumb) {
            const th = scrollThumb(vp, durS);
            els.thumb.style.left = th.left * 100 + "%";
            els.thumb.style.width = th.width * 100 + "%";
        }
        // ---- 横向缩放条(Wave 4):thumb 行程 + ×N 读数 + aria 三者同源于当前倍率
        if (els.hzoomBar) {
            const factor = durS / Math.max(spanOf(vp), MIN_SPAN_S);
            const p = zoomPercentOfFactor(factor, durS);
            if (els.hzoomThumb) els.hzoomThumb.style.left = p * 100 + "%";
            text(
                els.hzoomVal,
                "×" + (factor >= 10 ? Math.round(factor) : factor.toFixed(1)),
            );
            attr(els.hzoomBar, "aria-valuemin", 1);
            attr(
                els.hzoomBar,
                "aria-valuemax",
                Math.round(zoomMaxFactor(durS)),
            );
            attr(els.hzoomBar, "aria-valuenow", Math.round(factor * 10) / 10);
            // aria-valuetext:aria-valuenow 是**线性**的 1..300,而 thumb 行程与
            // 拖拽换算是**对数**的(×23 时 thumb 在 55%,线性读数只有 7.7%),
            // 两者不同源。屏幕阅读器念 valuetext 优先 —— 直接复用可见读数
            //(「×4.0 / 75s」),播报与眼见/行程三者同源。
            attr(els.hzoomBar, "aria-valuetext", zoomLabel(vp, durS));
        }
        // 纵向缩放条的悬停 tooltip:「纵向缩放(…)· 34px」。写在 render 里而不是
        // applyLaneH 里 —— 切语言走的是 applyI18n + render(),applyLaneH 不重跑,
        // 在那边拼的 title 会一直停在挂载时那门语言(而挂载时词典还没注入,
        // 拼出来是个孤零零的「 · 34px」)。
        if (els.vzoom) {
            setTitle(
                els.vzoom,
                (getT()["wave.vZoomBar"] || "") + " · " + local.laneH + "px",
            );
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
        // 确认框正文的占位符补填(applyI18n 会把带 data-t 的整串换回原文)
        renderReidentifyBody();

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
        // a11y:手柄 role="slider" 需 aria-valuenow/min/max(时间秒;选区几何唯一真源)。
        // 选区步(tour 28/29 可视化)会让手柄进入可见态,axe 的 aria-required-attr 必查这里。
        const durS = timeline.durationS();
        attr(els.selHandleL, "aria-valuemin", 0);
        attr(els.selHandleR, "aria-valuemin", 0);
        attr(els.selHandleL, "aria-valuemax", durS);
        attr(els.selHandleR, "aria-valuemax", durS);
        attr(els.selHandleL, "aria-valuenow", sel.startS);
        attr(els.selHandleR, "aria-valuenow", sel.endS);
        setBtnEnabled(els.setrange, !isWriteBlocked());
        const contentH = LANE_COUNT * local.laneH;
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
     * 段检查器投影(§17):pan/vol 显示乐观回声优先(local.echo,§2.8 事件后失效);
     * 锁定 toggle / origin 角标 / B-12 mm:ss.mmm 主显 / A-18 跟随宿主提示
     * (输出开关 OFF 即显,05 §2.3a 350)。
     *
     * **显隐口径(Wave 5 裁定④,覆盖 C-11)**:面板显隐**只**由本地开关
     * `local.inspectorOpen` 决定,与「有没有选中段」彻底解耦 —— 后者只切
     * `data-empty`(面板宽不变,所以点不同段零布局抖动)。
     */
    function renderInspector(store) {
        const cur = currentSeg();
        show(els.inspector, local.inspectorOpen);
        attr(
            els.inspToggle,
            "aria-pressed",
            local.inspectorOpen ? "true" : "false",
        );
        attr(els.inspector, "data-empty", cur ? 0 : 1);
        if (!cur) {
            // 空态也要复位标题行的 origin 角标(Wave 5 /code-review minor):
            // `[data-empty="1"]` 的 CSS 盘点里没有 .inspector-head,面板改常驻后
            // 它是**可见**的 —— 不清就会挂着上一个段的 E/C,与正下方「点选泳道内的段
            // 以编辑」同框。触发路径不止 Escape:§2.8 段表重编号后 rebindSegKeys
            // 失效 → selectedCh=0,走的也是这条早退。
            show(els.inspOrigin, false);
            // [SL-230] 空态同样要收起「恢复自动」——与上面 origin 角标同一个理由:
            // 面板改常驻之后它是可见的,不收就挂着上一个段的入口。
            renderInspectorRestore(0, -1, null, false);
            return;
        }
        const seg = cur.seg;
        show(els.inspNote, !((store.state || {}).global || {}).output_enabled);
        if (seg.origin === "user_edited" || seg.origin === "user_created") {
            text(els.inspOrigin, seg.origin === "user_edited" ? "E" : "C");
            show(els.inspOrigin, true);
        } else {
            show(els.inspOrigin, false);
        }
        text(els.inspStart, fmtTimeMs(seg.t0S));
        // openEnded 段:t1S 是保守下界不是真末端,显示词条而不是一个会误导的时间值
        text(
            els.inspEnd,
            seg.openEnded ? fmtKey("wave.segOpenEnd") : fmtTimeMs(seg.t1S),
        );
        // openEnded 时长只知下界,前缀 ≥(纯符号,不进词典)
        text(
            els.inspLen,
            `${seg.openEnded ? "≥" : ""}${(num(seg.t1S, 0) - num(seg.t0S, 0)).toFixed(2)}s`,
        );
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
            setNumBox(els.inspPanInput, pan, 1);
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
            setNumBox(els.inspVolInput, vol, 1);
            els.inspVolInput.disabled = !editable;
        }
        if (els.inspLock) {
            attr(els.inspLock, "data-on", seg.locked ? 1 : 0);
            attr(els.inspLock, "aria-checked", seg.locked ? "true" : "false");
            attr(els.inspLock, "aria-disabled", editable ? "false" : "true");
        }
        // [SL-230]「恢复自动」:选中的这一段是手动来的(origin≠auto)才出 ——
        // auto 段本来就在自动态,给它一个「恢复自动」是废钮。
        // [SL-242] 作用面是**这一段**(scope 带上该段区间,见 segmentRestoreScope),
        // 不再是整轨;确认句同步换成段口径的 wave.restoreSegConfirm。
        renderInspectorRestore(cur.ch, cur.idx, seg, editable);
    }

    /**
     * 检查器「恢复自动」两态(SL-230 的手势;SL-242 起作用域与文案都是**段级**)。
     *
     * 与轨道页那份是同一套手势(按钮 → 就地展开「取消 / 继续」),但**不是同一件事**:
     * 轨道页那条(`doReidentify`)清的是整轨,它长在轨道行上、确认句说「轨 {n}」;
     * 这一条只重算选中的这一段。两处曾共用 tracks.* 那两条词条,于是文案说轨、
     * 位置说段 —— 用户点完发现整轨都回了自动(终验 A7),那正是 SL-242。
     */
    function renderInspectorRestore(ch, idx, seg, editable) {
        if (!els.inspRestore) return;
        const t = getT();
        const manual = !!seg && seg.origin && seg.origin !== "auto";
        const on = manual && editable;
        show(els.inspRestore, on);
        if (!on) {
            local.inspRestoreAsk = "";
            return;
        }
        // **锁定段:只说不做**。契约 §1.6 的 clearManual 对 locked 段免疫(「须先逐段
        // 解锁」),而 split / move_boundary / set_values 的后置(§5.4)恰恰会把命中段
        // 置成 origin=user_edited **且 locked=true** —— 也就是说用户最常遇到的手动段
        // 正是锁着的。给它一枚点了什么都不会发生的钮,就是又造一个「点了没反应」。
        // 解锁开关就在本行正上方,把话说清就够了。
        if (seg.locked) {
            local.inspRestoreAsk = "";
            text(els.inspRestoreText, t["tracks.restoreAutoLocked"] || "");
            show(els.inspRestoreBtn, false);
            show(els.inspRestoreCancel, false);
            show(els.inspRestoreOk, false);
            return;
        }
        // 按**段身份**比对:选中段一变,展开态自然失配(不靠别处记得复位)。
        // 身份取 `currentSeg()` 那份下标(与 askRestore 写入时同源),不取载荷里的
        // `segIdx` —— 两者按 §2.8 应当相等,但比对键没必要押在那条不变式上。
        const asking = local.inspRestoreAsk === `${ch}:${idx}`;
        // [SL-242] 段口径的两条词条:提示句说「本段」,确认句说「只重算该段、其余段
        // 保持不变」。不再借轨道页的 tracks.restoreAutoHint / tracks.reidentifyConfirm
        // —— 那两条说的是整轨,而这枚钮只动这一段,借来就是在文案上撒谎。
        text(
            els.inspRestoreText,
            asking
                ? t["wave.restoreSegConfirm"] || ""
                : t["wave.restoreSegHint"] || "",
        );
        show(els.inspRestoreBtn, !asking);
        show(els.inspRestoreCancel, asking);
        show(els.inspRestoreOk, asking);
        if (!asking) text(els.inspRestoreBtn, t["tracks.restoreAuto"] || "");
    }

    /**
     * 时间标尺重排。
     *
     * **data-sig 必须包含刻度的横向落点**(code-review finding 2):老 sig 只有
     * 「标签串 + span」,而刻度位置是 `(tS − vp.startS)/span` 的函数 —— 平移量
     * 小于一个刻度步长时标签集与 span 都不变,sig 相等就早退不重排,可波形/
     * 播放头/选区/边界全在动,标尺最多冻结一整格才「跳」一次。这里直接把算好
     * 的 `left`(即最终写进 style 的那两位小数)编进 sig:与渲染输出**逐字同源**,
     * 一像素都没动时照旧早退,动了就必排。
     */
    function renderRuler(vp) {
        const scale = els.rulerScale;
        if (!scale) return;
        const ticks = rulerTicks(vp);
        const span = spanOf(vp);
        const parts = ticks.map((k, i) => {
            const left = (((k.tS - vp.startS) / span) * 100).toFixed(2);
            const last = i === ticks.length - 1 && +left > 90 ? 1 : 0;
            return { label: k.label, left, last };
        });
        const sig = parts.map((p) => `${p.label}@${p.left}${p.last}`).join("|");
        if (scale.getAttribute("data-sig") === sig) return;
        scale.setAttribute("data-sig", sig);
        scale.innerHTML = parts
            .map(
                (p) =>
                    `<span class="wave-ruler__tick" style="left:${p.left}%"${p.last ? ' data-last="1"' : ""}>${p.label}</span>`,
            )
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
            const y0 = (ch - 1) * local.laneH;
            if (y0 + local.laneH >= top && y0 <= bottom) out.push(ch);
        }
        return out;
    }

    /**
     * 静态层:每条可见泳道一张 canvas。稳态直接画命中当前视口的那块 tile;
     * 视口在动时按下面的「单源」口径画降级底,静止 120ms 后取新块补齐。
     *
     * **过渡帧口径(五轮才收敛,每一轮都是用户 preview 报回来的)**:
     *   ① `ctx.scale(spanOld/spanNew, 1)` 重跑矢量画笔 —— 45° 斜纹被非等比
     *      scale 剪成缓坡、`lineWidth` 横向拉粗;且 `paintWaveTile` 首行的
     *      `clearRect` 是在**变换后**坐标里清,画布其余部分留着上一帧残迹;
     *   ② 「只 translate 不 scale,span 一变即不 blit」—— 缩放期波形整幅停在旧
     *      视口比例上,而标尺/曲线/边界/播放头每帧跟新视口走,层间时间轴错位;
     *   ③ `drawImage` 位图拉伸 —— 放大糊(光栅升采样)、缩小两侧露白;
     *   ④ 多块按时间映射重画 + 空隙裁剪 —— 不叠 α 也不露白了,但块与块**观感
     *      不同**(各块是不同时刻/不同档位的快照,列宽差一倍则外柱这个「列内
     *      max」就差一截),拼出一片深浅不一的补丁;给垫底件压暗反而更糟 ——
     *      唯一那块真数据成了亮度台阶;
     *   ⑤ **单源画满整幅**(现行):只挑一个源 —— 优先能完整盖住视口、列宽最接近
     *      1px 的缓存块(放大时上一档正好符合 ⇒ 全程清晰),没有就整幅交给全曲
     *      概览块(它跨 [0,durS],同样要过盖满校验)。补丁与 α 叠加在几何上
     *      不可能出现;代价是缩小/平移期整幅降一档粗(均匀,如地图低清瓦片)。
     * 一个源都挑不出来时**留上一帧原样当降级底**(不清、不拉伸):它是完整正确
     * 的一幅,只是比标尺晚一档,比逐帧闪空好得多。取数**回 null**(无数据/被拒)
     * 才清 —— 降级底有界,不会把一幅错档的图永久留在屏上。
     *
     * ⚠ **取数闸盖住缩放,不只是平移**(Wave 4 修订轮的红线修):契约
     * §1.27(行 383)与 brief §0.8 都写「静止 120ms 后取新块」,没有分平移/
     * 缩放两档。缩放时**每一帧跨度都不同 = 每一帧都是新键**,LRU 8 块/轨与
     * 在途去重**全部失效**(旧块反被逐出),所以这里必须与平移档共用同一个
     * `moving` 闸:视口在动就一律不取,交给 `vpIdleTimer`(IDLE_REFETCH_MS
     * = 120)标脏后补一轮。之前只在平移档设闸,实测拖 40 帧缩放条 = 560 次
     * `requestWaveform`(14 条可见泳道 × 每帧一次),真机每次还要在 [M] 受理
     * 并降采样 6 个 cols 长数组,brief §4.5「拖动平移/缩放无 >32ms 长帧」
     * 不可能守住。代价 = 缩放期波形停在上一档(降级底),手停 120ms 内补齐。
     */
    function paintStaticLanes() {
        const vp = timeline.viewport();
        const w = stageWidth();
        if (!(w > 0)) return;
        const k = backingK();
        const laneH = local.laneH;
        const cols = Math.min(Math.max(Math.round(w), 1), MAX_COLS);
        const span = spanOf(vp);
        const durS = timeline.durationS();
        const moving = !!local.vpIdleTimer;
        const pal = local.palette || undefined;
        for (const ch of visibleLanes()) {
            const n = local.lanes.get(ch);
            if (!n || !n.canvas) continue;
            const ctx = resizeCanvas(n.canvas, w, laneH, k);
            if (!ctx) continue;
            // 概览块只在**视口静止**时补(动的时候一律不取数,§1.27「静止 120ms
            // 才取新块」);ensureOverview 自带节流,可以每帧无脑调。
            if (!moving && durS > 0) {
                waveSource.ensureOverview(ch, 0, durS, OVERVIEW_COLS);
            }
            const tile = waveSource.peek(ch, vp.startS, vp.endS, cols);
            if (tile) {
                paintWaveTile(ctx, tile, w, laneH, pal);
                local.canvasVp.set(ch, { startS: vp.startS, endS: vp.endS });
                continue;
            }
            const drawn = local.canvasVp.get(ch);
            // 画布上现在这幅**就是当前视口**(cols 变了 1px 之类的键失配)——
            // 别清,留着它等新块;不然任何一次宽度抖动都会把整屏闪空
            const sameVp =
                !!drawn && drawn.startS === vp.startS && drawn.endS === vp.endS;
            // 过渡帧的拼接序:LRU 里相交的块(窄→宽、新鲜优先)在前,
            // **全曲概览块垫最后**。概览必然覆盖整个视口,所以「清空后补不满」
            // 在几何上不再可能 —— 那正是用户实测「运动时波形显示不全 + 闪烁」
            // 的成因(补不满留白 / 一块都不相交时又整幅留着,两态逐帧交替)。
            //
            // ⚠ 概览只在**前面确实没盖满**时才动用(见下面 `ov` 的用法):它跨整
            // 首曲子,每帧每轨都拉进来画的话代价可观 —— 实测超 32ms 的长帧
            // 3 → 13(brief §4.5 红线)。多数帧其实盖得满,不该为兜底付这笔钱。
            // 过渡帧**只用一个源**画满整幅 —— 这是四轮反复后的结论。
            //
            // 弯路记档(每一条都是用户 preview 报回来的):
            //   ① 「重跑画笔 + ctx.scale」—— 45° 斜纹被剪成缓坡、线宽被拉粗;
            //   ② 「drawImage 位图拉伸」—— 放大糊、缩小两侧露白;
            //   ③ 「多块叠着画」—— 波形半透明,重叠区 α 叠到 0.9 几乎纯白;
            //   ④ 「多块空隙拼接」—— 不叠了、也不露白了,但**块与块观感不同**:
            //      各块是不同时刻/不同档位的快照,列宽差一倍观感就差一截(外柱
            //      取列内 max,列越粗柱子铺得越满越高)。拼在一起就是一片深浅
            //      不一的补丁 —— 用户实测「反复缩放几次后出现亮块暗块,
            //      **动画一停马上消失**」。给垫底件压暗反而更糟:唯一那块真数据
            //      成了亮度台阶(实测各段 39/41/39/39/…/**81**/42/43,静止后
            //      74/83/82/78/…/88/82/88 —— 81 那条就是用户圈出来的亮块)。
            //
            // 所以不再拼:挑**一个**源画满整幅。优先用能完整盖住当前视口、且
            // 列宽最接近 1px(最像稳态)的缓存块 —— 放大时上一档那块正好符合,
            // 于是缩放全程清晰;盖不住(缩小/平移)就整幅交给全曲概览块,它必然
            // 覆盖,画面是均匀的一档粗,像地图平移时的低清瓦片,不会花。
            // 单源 ⇒ 补丁在几何上不可能出现,也不需要空隙裁剪与压暗调色板。
            const near = moving
                ? waveSource.peekOverlapping(ch, vp.startS, vp.endS)
                : [];
            const ov = moving ? waveSource.peekOverview(ch) : null;
            let src = null;
            if (moving) {
                const EPS = 1e-6;
                let bestErr = Infinity;
                for (const b of near) {
                    // 只认能盖满整幅的块:盖不满就得拼,拼就会花
                    if (!(b.startS <= vp.startS + EPS)) continue;
                    if (!(b.endS >= vp.endS - EPS)) continue;
                    const cols = ((b.tile || {}).minDb || []).length;
                    if (!cols || !(span > 0)) continue;
                    const colW = ((b.endS - b.startS) / cols / span) * w;
                    if (!(colW > 0)) continue;
                    const err = Math.abs(Math.log(colW)); // 离 1px 的对数距离
                    if (err < bestErr) {
                        bestErr = err;
                        src = b;
                    }
                }
                // 概览兜底同样要过「盖满整幅」这一关(PR#64 评审【重要】)。
                // 它跨 `[0, durS]`,正常必然覆盖 —— 但 durS 是从 timeline 读的,
                // 曲长回填/切歌的那一两帧里,视口可能已经越出手上这份概览的范围。
                // 不校验就会画出一块只盖住半幅的图、另半幅留白 —— 正是本波一路
                // 在治的那个症。盖不住就让 src 保持 null,退回「留上一帧当降级底」
                // (有界:取到新块即纠正)。
                if (
                    !src &&
                    ov &&
                    ov.startS <= vp.startS + EPS &&
                    ov.endS >= vp.endS - EPS
                ) {
                    src = ov;
                }
            }
            if (!sameVp && src) {
                ctx.clearRect(0, 0, w, laneH);
                local.canvasVp.set(ch, null); // 画布内容不再对应任何完整视口
                paintWaveTile(ctx, src.tile, w, laneH, pal, {
                    clear: false,
                    tileStartS: src.startS,
                    tileEndS: src.endS,
                    viewStartS: vp.startS,
                    viewEndS: vp.endS,
                }); // 视口在动就**不取新块**(契约 §1.27 行 383 / brief §0.8:静止
                // 120ms 后才取)。缩放期每帧跨度都不同 = 每帧都是新键,LRU 与
                // 在途去重全失效,不设闸就是每帧 × 每条可见泳道一次桥调用。
                continue;
            }
            if (moving) continue; // 没有老底可搬(首绘)——留白等 vpIdleTimer
            // 取数在途:resolve 后补一次静态重绘(契约 §1.27 一次调用一次
            // resolve;peek 不 await —— 渲染帧禁止阻塞)。
            const reqStartS = vp.startS;
            const reqEndS = vp.endS;
            waveSource.getTile(ch, reqStartS, reqEndS, cols).then((got) => {
                if (!got) {
                    // 无数据/被拒:降级底到此为止 —— 清掉,别把错档的图留在屏上。
                    // ⚠ 两条纪律:
                    //  ① **迟到的 null 不得擦掉更新的一帧** —— 画布上现在这幅
                    //     若已对应别的视口(期间缓存命中画对了),本次 null 弃掉;
                    //  ② 宽/行高/k **当场重读**,不用发起帧的闭包值:舞台宽在途
                    //     中变过时,按旧宽 resizeCanvas 会把后备存储也建错。
                    const drawn = local.canvasVp.get(ch);
                    if (
                        drawn &&
                        (drawn.startS !== reqStartS || drawn.endS !== reqEndS)
                    ) {
                        return;
                    }
                    const cv = (local.lanes.get(ch) || {}).canvas;
                    const w2 = stageWidth();
                    const c2 =
                        cv && w2 > 0
                            ? resizeCanvas(cv, w2, local.laneH, backingK())
                            : null;
                    if (c2) c2.clearRect(0, 0, w2, local.laneH);
                    local.canvasVp.set(ch, null);
                    return;
                }
                local.staticDirty = true;
                schedulePaint();
            });
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
        const laneH = local.laneH;
        const h = LANE_COUNT * laneH;
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
            const y0 = (ch - 1) * laneH;

            // ⓪ 选中段高亮(裁定③;见 SEG_SEL_* 头注)。画在曲线/边界**之前**,
            //    并且刻意排在「曲线可见 toggle 灭 → continue」的**上面** ——
            //    曲线关掉了也得看得出选中的是哪一段(选中态属身份指示,不属曲线)。
            if (local.selectedCh === ch && local.selectedSegs.length) {
                for (const k of local.selectedSegs) {
                    const s = segs[k.idx];
                    if (!s) continue;
                    const sx0 = timeToX(vp, w, num(s.t0S, 0));
                    const sSegE = segEndS(s);
                    const sx1 = timeToX(
                        vp,
                        w,
                        Number.isFinite(sSegE) ? sSegE : vp.t1,
                    );
                    if (sx1 < 0 || sx0 > w) continue;
                    const cx0 = Math.max(sx0, 0);
                    const cw = Math.min(sx1, w) - cx0;
                    if (!(cw > 0)) continue;
                    const ew = SEG_SEL_EDGE_W;
                    ctx.fillStyle = SEG_SEL_FILL;
                    ctx.fillRect(cx0, y0, cw, laneH);
                    ctx.fillStyle = SEG_SEL_EDGE;
                    ctx.fillRect(cx0, y0, cw, ew); // 上
                    ctx.fillRect(cx0, y0 + laneH - ew, cw, ew); // 下
                    // 左右两条只在**段端真的在视口内**时画:段被视口切掉的那一侧
                    // 画上去就成了假边界(读成「段在这里结束」)
                    if (sx0 >= 0) ctx.fillRect(cx0, y0, ew, laneH);
                    if (sx1 <= w) {
                        ctx.fillRect(cx0 + cw - ew, y0, ew, laneH);
                    }
                }
            }

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
            drawStepCurve(ctx, segs, vp, w, y0, rampPx, "pan", laneH);
            drawStepCurve(ctx, segs, vp, w, y0, rampPx, "vol", laneH);

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
                ctx.lineTo(x, y0 + laneH);
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
                const y0 = (d.ch - 1) * laneH;
                ctx.beginPath();
                ctx.setLineDash([]);
                ctx.strokeStyle = d.snapped
                    ? "rgba(226,196,136,.95)"
                    : "rgba(255,255,255,.85)";
                ctx.lineWidth = d.snapped ? 2 : 1.6;
                ctx.moveTo(x, y0);
                ctx.lineTo(x, y0 + laneH);
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
    function drawStepCurve(ctx, segs, vp, w, laneY, rampPx, dim, laneH) {
        const yOf = (seg) =>
            laneY +
            (dim === "pan" ? panYPx(seg.pan, laneH) : volYPx(seg.volDb, laneH));
        for (let i = 0; i < segs.length; i++) {
            const seg = segs[i];
            if (!seg) continue;
            const manual = seg.origin && seg.origin !== "auto";
            const x0 = timeToX(vp, w, num(seg.t0S, 0));
            // openEnded 段:t1S 只是保守下界,画到视口末端而不是下界处截断
            const segE = segEndS(seg);
            const x1 = timeToX(vp, w, Number.isFinite(segE) ? segE : vp.t1);
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
        // 进行中的边界拖拽同样持有旧 segIdx —— 与选中态一并处理,否则释放时
        // move_boundary 会指向重编号后的错段或越界 badArg(评审【重要】2)。
        // **只掐被本次事件重编号的那一轨**:§2.8 与 applySegmentsEvent 都是按 ch
        // 整条替换,未列入 channels 的轨编号一个都没动,一律取消属于误伤(拖到
        // 一半手柄归位、无任何提示,手势得重来)。全量类 reason 没有 channels
        // 语义(整表替换),那才该无条件取消。
        const segAll =
            !seg ||
            !Array.isArray(seg.channels) ||
            seg.reason === "snapshot" ||
            seg.reason === "versionActive" ||
            seg.reason === "copyVersion";
        if (
            local.boundDrag &&
            (segAll ||
                seg.channels.some((c) => c && c.ch === local.boundDrag.ch))
        ) {
            cancelBoundDrag();
        }
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

    /**
     * §2.7(播放中 2Hz):覆盖条延伸 → 该轨块缓存失效 + 静态层脏。
     * **只失效与 `addedRanges` 相交的块**:2Hz 增量事件通常只新增很小一段,
     * 整轨清会把 8 块 LRU 全丢 ⇒ 采集中反复整轨重取(pr-agent)。载荷没带
     * `addedRanges` 时退回整轨清(语义 =「这轨变了但不知道哪变了」)。
     */
    function onCaptureProgress(cp) {
        for (const c of (cp && cp.channels) || []) {
            if (!c || !c.ch) continue;
            const added = Array.isArray(c.addedRanges) ? c.addedRanges : null;
            waveSource.invalidate(c.ch, added && added.length ? added : null);
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
        // tour 视图层增强(T36b 第四轮:步 28 放大泳道 / 步 29 示例选区;只动渲染,不写 state)
        zoomLanes,
        showDemoSelection,
        showDemoSegment,
        resetWaveView,
        // 布防 badge 三处的共用跳转口径(05 行 300 ①;Tab1/Tab2 badge 经
        // app.js 切到本页后调用,定位选区 + 勾选目标轨)
        locateRecapture,
    };
}
