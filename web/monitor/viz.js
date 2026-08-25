// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Monitor · viz 段投影(纯函数;T46 / [J75] C)
// -----------------------------------------------------------------------------
// 本文件把 **viz 段的一帧**翻译成两张图各自要的形状:
//   `scvb.viz` ──► vizSeries()     ──► trajectory-chart.js 的 getSeries()
//              ├─► vizDistRows()   ──► distribution-chart.js 的 distBarsHtml()
//              └─► vizLegendRows() ──► distribution-chart.js 的 legendItemsHtml()
// 无 DOM、无 store、无 i18n —— node 可直接 import 断言
// (`web-preview/tests/smoke-monitor.mjs`)。DOM 接线在 `./app.js`。
//
// 段的**布局与语义真源在 native 侧**(T44:`docs/contract-changes/20260825-viz-segment.md`
// / `src/core/ipc/VizPlane.h` / `tests/golden/ipc-layout.txt`);JS 侧的镜像与 parity 断言在
// `./viz-contract.js`。本文件只消费那份镜像里的常量,不再复述一遍段布局。
//
// =============================================================================
// 事件形状(= 段的 JSON 投影;samples→秒的换算在 T45 的 C++ 桥,契约 §0.2)
// =============================================================================
//   {
//     magic:"SCVB", abi:1, generation, columnCount:1024, trackCount:15, panScale:100,
//     online:bool,                          // = 段在线 **且** 帧新鲜(桥把两者与在了一起)
//     // 组回显与段级三态走 `scvb.state`:{groupId, uiScale, language, viz, fresh}
//     seq, laneRevision, publishMs, playheadEpoch, versionActive, sampleRate,
//     windowStartS, windowSpanS,            // 窗口(秒);windowSpanS === 0 ⇒ 无有效窗口
//     playheadS|null, playheadFlags, loopStartS|null, loopEndS|null,
//     onlineMask, coveredMask, stereoMask, leadMask,          // bit{ch−1}
//     colorIndex:[15], coverage:[15][32], lanes:[15][1024],   // ← 见下「按需重发」
//     trackPanNow:[15], trackVolDb:[15], trackWidthPct:[15],  // **已解码的工程量**,null = 无
//     trackLabels:[15]                                        // VizTrackLabels,桥已解码
//   }
//
// **每一块都是定长 15、下标即轨号**(与段内布局逐条对应),不是「带 ch 字段的对象数组」——
// 段本来就是定长表,投影成对象数组会凭空多出一道「ch 合不合法」的闸。
// `leadMask` T45 已落地(`VizFrame.track_lead_mask`);拿不到时一律不戴绿帽。
//
// **车道三件按需重发**:`lane_revision` 的段内注释逐字写着「读方可据此跳过重解析」。
// 稳态下 4Hz 只写 128 B 帧头、车道不重算,故桥也只在 `laneRevision` 变化的那一帧带上
// `colorIndex`/`coverage`/`lanes`,其余帧省略。缓存与合并由 `mergeVizFrame()` 做,
// 调用方持有那份缓存 —— 本文件其余部分只处理「已经合并好的一帧」。
//
// `trackVolDb` 为什么可能整块缺:旧版 Output 的段里没有 `VizTrackState`。缺了就**画空**,
// 不猜、不填 0 —— 填 0 会画出一排居中等高的幽灵柱,那是假数据。见 `DIST_REQUIRES`。
// =============================================================================

import { PAN_MAX, PAN_MIN } from "../shared/trajectory-chart.js";
import { TRACK_COLOR_COUNT } from "../shared/track-colors.js";
import {
    VIZ_ABI,
    VIZ_COLUMNS,
    VIZ_COVERAGE_WORDS,
    VIZ_FLAG_LOOP_VALID,
    VIZ_FLAG_LOOPING,
    VIZ_FLAG_PLAYING,
    VIZ_MAGIC,
    VIZ_PAN_NONE,
    VIZ_PAN_SCALE,
    VIZ_TRACK_STATE_RANGE,
    VIZ_TRACKS,
} from "./viz-contract.js";

export {
    VIZ_ABI,
    VIZ_COLUMNS,
    VIZ_COVERAGE_WORDS,
    VIZ_MAGIC,
    VIZ_PAN_NONE,
    VIZ_PAN_SCALE,
};

/** 组号 → 显示字母(契约 §0.2:g = 1..8,UI 显示 A-H)。 */
export const GROUP_LETTERS = Object.freeze([
    "A",
    "B",
    "C",
    "D",
    "E",
    "F",
    "G",
    "H",
]);

/** 通道数(= 段的 `track_count`)。 */
export const CHANNEL_COUNT = VIZ_TRACKS;

/**
 * 兜底时间线长度(秒)。与 `tab-master.js` 的 `CHART_FALLBACK_DURATION_S` 同值 ——
 * 窗口跨度为 0(未 prepare)时视口不该塌到 0,那样时间轴一格刻度都画不出来。
 */
export const VIZ_FALLBACK_DURATION_S = 300;

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

function clamp(lo, hi, v) {
    return v < lo ? lo : v > hi ? hi : v;
}

/** `scvb.groups` 位图 → 某组是否在线(事件缺失时传 0,绿点全灭、零报错)。 */
export function groupOnline(bitmap, g) {
    return ((Number(bitmap) || 0) >> (g - 1)) & 1 ? 1 : 0;
}

/** 轨掩码(`track_*_mask`)→ 某轨的位。`bit{ch−1}`,与段内注释逐字同序。 */
export function maskHas(mask, ch) {
    if (!Number.isInteger(ch) || ch < 1 || ch > VIZ_TRACKS) return false;
    return (((Number(mask) || 0) >>> (ch - 1)) & 1) === 1;
}

/**
 * 覆盖位图的第 i 列是否有分段。
 *
 * 位序 = **LSB 优先、每字 32 列**(`words[i/32]` 的 `bit(i%32)`),与 `VizCoverage` 的段内
 * 注释逐字同序 —— 位序是这类位图最容易两边写反的地方,且写反了图**照画**,只是断线位置
 * 整体错开 32 列的倍数,肉眼几乎看不出来。故只此一处实现,两张图共用。
 *
 * 越界 / 脏数据一律回 false(读不到覆盖 = 不画线,比画一条假线安全)。
 *
 * @param {number[]} words 单轨的 32 个 u32
 */
export function columnCovered(words, i) {
    if (!Array.isArray(words) || !Number.isInteger(i) || i < 0) return false;
    if (i >= VIZ_COLUMNS) return false;
    const w = words[i >>> 5];
    if (!Number.isFinite(w)) return false;
    return ((w >>> (i & 31)) & 1) === 1;
}

/**
 * 定点 int16 → 工程量(通用解码)。哨兵与非数一律回 **null**,不是 0 ——
 * 0 在三条标量里分别是「居中 / 0 dB / 宽度 0」,每一个都是有意义的合法值,
 * 拿它当「没有数据」会让缺数据的轨画成一根**合法的**柱。
 *
 * `VizTrackState` 的三条标量与 `VizLanes` 的车道共用同一个标度与同一个哨兵,
 * 故解码只此一处 —— 不是三处各写一份 `/100` 再各自夹取。
 */
export function fixedToUnit(v, lo, hi) {
    if (!Number.isFinite(v) || v === VIZ_PAN_NONE) return null;
    return clamp(lo, hi, v / VIZ_PAN_SCALE);
}

/** 定点 int16 → 角度域 pan(车道与 `trackPanNow` 共用)。 */
export function panOfFixed(v) {
    return fixedToUnit(v, PAN_MIN, PAN_MAX);
}

/**
 * 取每轨的某条标量(`jsonKey` = trackPanNow|trackVolDb|trackWidthPct)。
 *
 * **桥送来的已经是工程量,哨兵已折成 `null`**(T45 `buildVizPayload`:
 * `vizPanIsNone(x) ? juce::var() : vizUnpackPan(x)`),故这里**不再解定点**,
 * 只做值域夹取。定点解码在 JS 侧只剩车道一个用途(那是段里的原始 int16)。
 *
 * 整块缺失、轨号越界、或该轨是 null ⇒ null。**不能判 `!== 0`** ——
 * 0 在三条里分别是「居中 / 0 dB / 宽度 0」,每一个都是合法值。
 */
export function trackScalar(viz, jsonKey, ch) {
    const arr = viz && viz[jsonKey];
    const r = VIZ_TRACK_STATE_RANGE[jsonKey];
    if (!Array.isArray(arr) || !r) return null;
    if (!Number.isInteger(ch) || ch < 1 || ch > VIZ_TRACKS) return null;
    const v = arr[ch - 1];
    return Number.isFinite(v) ? clamp(r.lo, r.hi, v) : null;
}

/** 取某轨的轨名(`VizTrackLabels` 解码后的串);缺失 ⇒ 空串,不是 undefined。 */
export function trackLabel(viz, ch) {
    const arr = (viz && viz.trackLabels) || null;
    if (!Array.isArray(arr)) return "";
    const s = arr[ch - 1];
    return typeof s === "string" ? s : "";
}

/**
 * 这一帧 viz 能不能读,以及为什么不能。
 *
 * **段级三态(在线/离线/版本不匹配)不在这里判** —— 那是 `scvb.state` 的 `viz` + `fresh`
 * 说了算,由 native 做:命名段是引用计数存活的,只要 Monitor 自己不松手,「Output 进程
 * 退出」就永远看不出来(段还在、read 还成功)。T45 的做法是帧陈旧时**松开映射再探一次** ——
 * 这件事 UI 侧做不了,故状态从事件里来,不自己猜。
 *
 * @param {object|null} viz 最近一帧(已合并)
 * @param {{viz?:string, fresh?:boolean}} [status] 最近一帧 `scvb.state` 的 viz 面
 * @returns {{ok:boolean, reason:string, abi:number}} `reason`:
 *   `""`         —— 可读;
 *   `"shape"`    —— 帧还没到 ⇒ 静默忽略(**缺车道不算** —— 那只影响轨迹图那一半);
 *   `"magic"`    —— 段头对不上 ⇒ 整帧丢弃(读错段比读不到段危险得多);
 *   `"abi"`      —— 段比本机新 / state 报 abiMismatch ⇒ **停止读取** + 红横幅(J40);
 *   `"geometry"` —— 列数/轨数/标度与本机常量不符 ⇒ 同 abi 处理(T44 几何自检的 UI 兜底);
 *   `"offline"`  —— 该组没有 Output ⇒ **空态**,不是错误;
 *   `"stale"`    —— Output 还在、但帧不再更新 ⇒ **在线但陈旧**(琥珀横幅,**图仍显示**);
 *   `"window"`   —— 窗口跨度为 0(未 prepare)⇒ 空态。
 *
 * `offline` 与 `abi` 必须可区分 —— T44 显式设计的两条降级路径(空态 vs 拒连横幅)。
 */
export function vizAccepts(viz, status) {
    const st = status || {};
    if (st.viz === "abiMismatch") return { ok: false, reason: "abi", abi: 0 };
    if (!viz || typeof viz !== "object") {
        return { ok: false, reason: "shape", abi: 0 };
    }
    if (viz.magic !== VIZ_MAGIC) {
        return { ok: false, reason: "magic", abi: num(viz.abi, 0) };
    }
    const abi = num(viz.abi, 0);
    if (abi > VIZ_ABI) return { ok: false, reason: "abi", abi };
    // 几何自检:T44 在 attachReadOnly() 里查过一遍,UI 侧再兜一层 —— 桥送来的几何若与
    // 本机常量不符,后面的逐列循环会静默少画/多画,而不是报错。
    if (
        viz.columnCount !== undefined &&
        (viz.columnCount !== VIZ_COLUMNS ||
            viz.trackCount !== VIZ_TRACKS ||
            viz.panScale !== VIZ_PAN_SCALE)
    ) {
        return { ok: false, reason: "geometry", abi };
    }
    // 这一帧的数据本身能不能用。**判据只有窗口,不含车道** —— 车道只是轨迹图那一半的
    // 数据面,缺了应当只让轨迹图走「未接通」空态(见 `vizHasLanes`),不该把整页
    // (含分布图与图例)一起拖进空态面板。
    const usable = num(viz.windowSpanS, 0) > 0;

    // ⚠ **陈旧必须排在「离线」之前**。T45 的 `buildVizPayload` 写的是
    // `online = (vizState==online) && vizFresh()` —— 两件事被与在了一起,于是
    // 「在线但陈旧」在帧里长得和「离线」一模一样(都是 `online:false`)。
    // 只看帧就会把它判成掉线、把图清空,而 Output 其实还在跑。
    // `scvb.state` 里那两个量是分开的,以它为准。
    // **陈旧不挡出图**:数据还是上一份真数据,清掉反而更糟。
    if (st.viz === "online" && st.fresh === false && usable) {
        return { ok: true, reason: "stale", abi };
    }
    // ⚠ **离线判据必须排在「缺车道」之前**:段 attach 不上时桥送的就是一帧没有车道、
    // 没有窗口的空帧。先判 shape 会把「该组没有 Output」报成「帧还没到」——
    // 前者要显示空态面板并说清是哪个组,后者是静默忽略。
    if (st.viz === "offline" || viz.online === false) {
        return { ok: false, reason: "offline", abi };
    }
    if (!usable) return { ok: false, reason: "window", abi };
    return { ok: true, reason: "", abi };
}

/**
 * 这一帧带没带车道(轨迹图的全部数据面)。
 *
 * 三种「没有线」在画面上一模一样,但**该说的话完全不同**,故必须分得开:
 *   ① 桥没送车道(旧版 Output / 桥回归)⇒ 「监视数据未接通」;
 *   ② 送了车道、但位图全 0(工程真没分析过)⇒ 「尚无分段结果」;
 *   ③ 整个组不在线 ⇒ 空态面板。
 * 本函数只回答 ①;② 由 `vizSeries().length` 回答,③ 由 `vizAccepts()` 回答。
 * 说错了会让用户去 DAW 里白找一遍。
 */
export function vizHasLanes(viz) {
    return !!viz && Array.isArray(viz.lanes) && Array.isArray(viz.coverage);
}

/** 拒读理由是不是「该显示成空态」(而不是红横幅)。 */
export function vizIsEmptyState(reason) {
    return reason === "offline" || reason === "window" || reason === "shape";
}

/** 时间线全长(秒)= 窗口末端;拿不到证据就压在兜底值上,不塌到 0。 */
export function vizDurationS(viz) {
    const end =
        num(viz && viz.windowStartS, 0) + num(viz && viz.windowSpanS, 0);
    return end > 0 ? end : VIZ_FALLBACK_DURATION_S;
}

/** 每列的秒宽 = 窗口跨度 / 列数。 */
export function vizColumnS(viz) {
    const span = num(viz && viz.windowSpanS, 0);
    return span > 0 ? span / VIZ_COLUMNS : 0;
}

/** `playhead_flags` → 三个布尔(位定义在 viz-contract.js)。 */
export function vizFlags(viz) {
    const f = Number((viz && viz.playheadFlags) || 0);
    return {
        isPlaying: (f & VIZ_FLAG_PLAYING) !== 0,
        looping: (f & VIZ_FLAG_LOOPING) !== 0,
        loopValid: (f & VIZ_FLAG_LOOP_VALID) !== 0,
    };
}

/**
 * viz 帧 → §2.6 形状的播放头事件(喂 trajectory-chart 的 `onPlayhead`)。
 *
 * 轨迹图只认 `{timeS, isPlaying}` 这一组扁平标量(与 Output 侧 §2.6 同形),故这里做一次
 * 形状归一。**没有时间线时返回 null**(`playhead_samples === −1`)—— 传 `timeS: 0` 会把
 * 竖线钉在开头,看着像「停在 0 秒」而不是「没有时间线」。
 */
export function vizPlayheadEvent(viz) {
    const t = viz && viz.playheadS;
    if (!Number.isFinite(t) || t < 0) return null;
    return { timeS: t, isPlaying: vizFlags(viz).isPlaying, inRange: true };
}

// 轨号闸不再需要单列一个函数:段里的每一块都是**定长 15**、下标即轨号,
// 越界的轨号根本没有落点(不像最初那版自拟的 `tracks[{ch,…}]` 对象数组 ——
// 那种形状里 ch 是载荷字段,才需要一道闸)。掩码侧的闸在 `maskHas` 里。

/**
 * 轨色索引 → 传给两张图的「轨号」。
 *
 * 两件消费方(`trackIndex()` / `trackColorVar()`)都按轨号取色,故这里把 `VizTrackColors`
 * 的槽位归一成 1..15 后当轨号交出去。段内注释:v1 恒 = 轨号,`0` = 未指定 —— 未指定就
 * 回落轨号(缺色号不该让这条线消失,顶多是颜色不是「指定的那个」)。
 */
export function colorChOf(viz, ch) {
    const arr = (viz && viz.colorIndex) || [];
    const idx = Number(arr[ch - 1]);
    if (Number.isInteger(idx) && idx >= 1 && idx <= TRACK_COLOR_COUNT) {
        return idx;
    }
    return ch;
}

/** 播放头落在第几列(不在窗口内 / 无时间线 ⇒ −1)。 */
export function playheadColumn(viz) {
    const colS = vizColumnS(viz);
    const t = viz && viz.playheadS;
    if (!(colS > 0) || !Number.isFinite(t) || t < 0) return -1;
    const i = Math.floor((t - num(viz.windowStartS, 0)) / colS);
    return i >= 0 && i < VIZ_COLUMNS ? i : -1;
}

/**
 * 车道 + 覆盖位图 → 轨迹图的折线段组。
 *
 * **断线判据在这里**,且与 Tab1 的 `runsOfSegments()` 是同一条语义的两种表达:那边看的是
 * 「段表里有没有段」,这边看的是「覆盖位图这一列是不是 1」—— 两者都是 J75 A 的
 * 「无分段覆盖的区间不画线」。T44 的段内注释把这条钉得更死:**断线渲染以位图为准**,
 * 因为 `CurveEvaluator` 会填补空隙(hold/外推),车道值在没有分段的地方**也是有值的** ——
 * 只看车道会画出一条根本不存在的连续轨迹。
 *
 * 连续为 1 的列聚成一条折线,中间只要有一列是 0 就另起一条。每列画成一个**水平台阶**
 * (`[t, t+colS]` 两点同 pan),列与列之间用竖直连接线 —— 与 Tab1 侧逐字同款:引擎按 ramp
 * 过渡,80ms 在任何缩放档下都不足 1px,画斜线会在放到最大时变成一条**假的**渐变轨迹。
 *
 * 哨兵列(`kVizPanNone`)即使位图置了 1 也**当作没数据**:两者矛盾时以「没数据」为准,
 * 宁可少画一段,也不画一条 −327.68 的线。
 *
 * @param {object} viz 已合并的一帧(调用方已过 `vizAccepts`)
 * @returns {{ch:number, stereo:boolean, runs:{tS:number,pan:number}[][]}[]}
 */
export function vizSeries(viz) {
    const out = [];
    const colS = vizColumnS(viz);
    if (!(colS > 0)) return out;
    const t0 = num(viz && viz.windowStartS, 0);
    const lanes = (viz && viz.lanes) || [];
    const coverage = (viz && viz.coverage) || [];
    for (let ch = 1; ch <= VIZ_TRACKS; ch++) {
        // `track_online_mask` = 该轨启用。未启用的轨在段里是全哨兵 + 位图全 0,
        // 走下面的循环也画不出东西 —— 但显式跳过更省 1024 次判断,语义也更直白。
        if (viz.onlineMask !== undefined && !maskHas(viz.onlineMask, ch)) {
            continue;
        }
        const lane = lanes[ch - 1];
        const words = coverage[ch - 1];
        if (!Array.isArray(lane) || !Array.isArray(words)) continue;
        const runs = [];
        let cur = null;
        const n = Math.min(VIZ_COLUMNS, lane.length);
        for (let i = 0; i < n; i++) {
            const pan = columnCovered(words, i) ? panOfFixed(lane[i]) : null;
            if (pan === null) {
                cur = null; // 断口:下一列有覆盖时另起一条
                continue;
            }
            if (!cur) {
                cur = [];
                runs.push(cur);
            }
            cur.push(
                { tS: t0 + i * colS, pan },
                { tS: t0 + (i + 1) * colS, pan },
            );
        }
        if (runs.length === 0) continue;
        out.push({
            ch: colorChOf(viz, ch),
            stereo: maskHas(viz.stereoMask, ch),
            runs,
        });
    }
    // 按轨号升序:色板按轨号固定映射,顺序不许乱(与 Tab1 的 trajectorySeries 同款)
    return out.sort((a, b) => a.ch - b.ch);
}

/**
 * viz → 分布图的 rows(喂 `distBarsHtml`)。
 *
 * 数据源 = `VizTrackState` 的三条定点标量(`trackPanNow` / `trackVolDb` /
 * `trackWidthPct`)+ `VizTrackLabels` + 三张掩码。`trackVolDb` **整块**拿不到
 * (旧 Output / 桥没投影)⇒ 返回空数组:分布图画空,不猜、不填 0
 * (见 viz-contract.js 的 `DIST_REQUIRES`)。
 *
 * 逐轨闸:在 `onlineMask` 里 ∧ `trackVolDb` 该轨不是哨兵。两条缺一不画 ——
 * 与 Tab1「空闲轨无参数值,vol=0 会被画成幽灵柱」是同一条纪律的不同判据面。
 *
 * **横位的取值优先级**照 `PAN_NOW_PRIORITY`:`trackPanNow`(播放头**精确时刻**的曲线
 * 求值)优先,它是哨兵时才回落到播放头所在**列**的车道点采样(列中心,与精确时刻差
 * 半列),都拿不到才 0。反过来写会让分布图的柱与轨迹图的播放头在放大档下对不上,
 * 而那是「看起来完全正常」的那类错。
 */
export function vizDistRows(viz) {
    if (!Array.isArray(viz && viz.trackVolDb)) return [];
    const at = playheadColumn(viz);
    const out = [];
    for (let ch = 1; ch <= VIZ_TRACKS; ch++) {
        if (viz.onlineMask !== undefined && !maskHas(viz.onlineMask, ch)) {
            continue;
        }
        const volDb = trackScalar(viz, "trackVolDb", ch);
        if (volDb === null) continue;
        const scalarPan = trackScalar(viz, "trackPanNow", ch);
        const fromLane =
            at >= 0 ? panOfFixed(((viz.lanes || [])[ch - 1] || [])[at]) : null;
        const width = trackScalar(viz, "trackWidthPct", ch);
        out.push({
            ch: colorChOf(viz, ch),
            pan: scalarPan !== null ? scalarPan : num(fromLane, 0),
            volDb,
            // width 缺失回落 100(= 不张开)。mono 轨本来就是 100,而 stereo 轨
            // 缺 width 时画一条零宽横线比画一条瞎猜宽度的横线诚实。
            widthPct: width !== null ? width : 100,
            stereo: maskHas(viz.stereoMask, ch),
            // `leadMask` 仍待 T44 确认(viz-contract.js 的 VIZ_PENDING_FIELDS)。
            // 拿不到就一律不戴绿帽 —— 少一顶帽子是「少了个信息」,猜错是「标错了主唱」。
            lead: maskHas(viz.leadMask, ch),
        });
    }
    return out.sort((a, b) => a.ch - b.ch);
}

/**
 * viz → 图例行(喂 `legendItemsHtml`)。
 *
 * 行集 = **两张图里任意一张真的画了的那些轨**的并集。Tab1 的图例跟着当前视图走
 * (两视图二选一);Monitor 两张图**同屏**,跟着其中一张就必然出现「图例里有它、
 * 另一张图上找不到它」或反之。取并集才让「图例里有它 ⇒ 屏幕上找得到它」成立。
 *
 * `trackLabels` 整块拿不到时只显示两位轨号(`legendItemsHtml` 对空 label 的既有处理),
 * 而不是显示 `undefined`。
 */
export function vizLegendRows(viz) {
    const drawn = new Set(vizSeries(viz).map((s) => s.ch));
    for (const r of vizDistRows(viz)) drawn.add(r.ch);
    const out = [];
    for (let ch = 1; ch <= VIZ_TRACKS; ch++) {
        const cch = colorChOf(viz, ch);
        if (!drawn.has(cch)) continue;
        out.push({
            ch: cch,
            label: trackLabel(viz, ch),
            stereo: maskHas(viz && viz.stereoMask, ch),
        });
    }
    return out.sort((a, b) => a.ch - b.ch);
}

/**
 * 车道三件的**按需重发**合并(`lane_revision` 语义)。
 *
 * 稳态下 4Hz 只写 128 B 帧头、车道不重算,桥因此只在 `laneRevision` 变化的那一帧带上
 * `colorIndex` / `coverage` / `lanes`。本函数把新帧头与上一份车道拼成完整的一帧。
 *
 * 四种情形:
 *   ① 新帧自带车道 ⇒ 原样用(并由调用方存成新缓存);
 *   ② 没缓存 ⇒ 原样返回(消费侧会因缺 lanes 落到 `shape` 空态,等下一次重算);
 *   ③ 有缓存且 `laneRevision` 一致 ⇒ 沿用缓存(稳态,最常见);
 *   ④ 有缓存但 `laneRevision` 或 `groupId` **不一致** ⇒ 车道已经换了内容而桥没发过来,
 *      **宁可当作没有车道**也不拿旧车道配新帧头 —— 那会画出一张「时间轴是新的、线是旧的」
 *      的图,而它看起来完全正常。换组同理:groupId 变了就是另一个数据面。
 *
 * @param {object|null} prev 上一份完整帧(缓存)
 * @param {object} next 新到达的帧
 * @returns {object} 合并后的帧
 */
export function mergeVizFrame(prev, next) {
    if (!next || typeof next !== "object") return next;
    if (Array.isArray(next.lanes) && Array.isArray(next.coverage)) return next;
    if (!prev || !Array.isArray(prev.lanes)) return next;
    if (prev.laneRevision !== next.laneRevision) return next;
    if (prev.groupId !== next.groupId) return next;
    return {
        ...next,
        colorIndex: prev.colorIndex,
        coverage: prev.coverage,
        lanes: prev.lanes,
    };
}
