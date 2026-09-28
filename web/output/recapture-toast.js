// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output —— toast③「已重采集 X.Xs,建议重分析该范围」的记账(J125)。
// -----------------------------------------------------------------------------
// 05 §2.0 组件表 / §2.3「重采集选区」行:重采集 OFF 后弹 toast,带「立即重分析」钮。
//
// **信号从哪来(不新增桥事件、不新增 state 字段)**:
//   · 「重采集结束」= §2.1 `scvb.state.recapture.armed` 从 true 变 false。手动关开关、
//     「播完自动停」越过右边界、以及任何拒绝态(§1.23:四条拒绝路径都先撤防)都落到这一跳。
//   · 「X.Xs」= 布防期间**播放头真走过的、落在布防选区里的时长**(按 §2.6 `scvb.playhead`
//     相邻两帧的 timeS 累计,区间取并集,不重复计循环重录)。只在「01 采集」开着时计 ——
//     布防期采集被关掉(J92a 互斥或用户接管)时引擎一个 hop 都不写,这段不算。
//   · 为什么不用 §2.7 `captureProgress.addedRanges`:它是**覆盖**的增量,而重采集改写的
//     多半是**已经覆盖过**的区间 —— 覆盖不变 ⇒ addedRanges 为空 ⇒ 一律报 0.0s。
//     它还只报 `global.range` 以内,布防选区在 range 外时整段看不见(J87 门控不看 range)。
//
// **口径的边界(写明,别读大)**:这是 UI 按播放头推的**估计**,不是引擎的写入回执。
//   · 编辑器关着(WebView 不可见)那段时间收不到 §2.6,那段不计;重开编辑器后本会话的记账
//     从零开始(纯前端会话态,不入 state、不落盘)。
//   · 两帧间隔超过 `MAX_STEP_S` 视为跳播(seek),该步不计。
//   · 轨维不参与计时:秒数是「时间轴上重采了多长」,不是「轨 × 秒」。
//
// 纯函数,node 侧直接断言;app.js 只负责把事件喂进来、把结果投到 DOM。
// =============================================================================

/** 相邻两帧 timeS 之差的上限(秒);超过即视为跳播。§2.6 是 30Hz,正常一步 ≈ 0.033s。 */
export const MAX_STEP_S = 1;

/** 秒数四舍五入到 0.1 后仍为 0 ⇒ 不弹(「已重采集 0.0s」没有信息量)。 */
export const MIN_REPORT_S = 0.05;

/** 空记账。`intervals` 是已合并、按起点升序的 `[s0, s1]` 列表。 */
export function recapTrackerInit() {
    return {
        armed: false,
        mask: 0,
        startS: 0,
        endS: 0,
        capMask: 0,
        intervals: [],
    };
}

function num(v, d) {
    const x = Number(v);
    return Number.isFinite(x) ? x : d;
}

/** 把 `[a, b]` 并进已合并的区间表(返回新表,不改入参)。 */
export function addInterval(list, a, b) {
    if (!(b > a)) return list.slice();
    const out = [];
    let s0 = a;
    let s1 = b;
    let placed = false;
    for (const [x0, x1] of list) {
        if (x1 < s0) {
            out.push([x0, x1]);
        } else if (x0 > s1) {
            if (!placed) {
                out.push([s0, s1]);
                placed = true;
            }
            out.push([x0, x1]);
        } else {
            s0 = Math.min(s0, x0);
            s1 = Math.max(s1, x1);
        }
    }
    if (!placed) out.push([s0, s1]);
    return out;
}

/** 区间表的总长(秒)。 */
export function totalSeconds(list) {
    let t = 0;
    for (const [a, b] of list) t += b - a;
    return t;
}

/**
 * 每一帧 §2.1 之后调(传深合并后的整份 state)。
 *
 * @returns {{tracker:object, done:null|{seconds:number, tracksMask:number, startS:number, endS:number, intervals:number[][]}}}
 *   `done` 非空 = 这一帧正是「布防 → 撤防」那一跳,且这次重采集确实采到了东西:
 *   `seconds` = 并集总长(`intervals` 即该并集);`startS/endS` = 采到的区间的外包络;`tracksMask` = 采到东西时
 *   处于布防的轨(中途改选区/改勾选再布防时取并)。「立即重分析」就按这三个值调 §1.6。
 */
export function recapOnState(tracker, state) {
    const rec = (state || {}).recapture || null;
    const armedNow = !!(rec && rec.armed);
    const t = tracker || recapTrackerInit();
    if (armedNow) {
        const next = {
            ...t,
            armed: true,
            mask: Math.trunc(num(rec.tracksMask, 0)),
            startS: num(rec.startS, 0),
            endS: num(rec.endS, 0),
        };
        // 新一次布防(false → true):上一轮的账清零。中途改选区的再布防(true → true)不清。
        if (!t.armed) {
            next.capMask = 0;
            next.intervals = [];
        }
        return { tracker: next, done: null };
    }
    if (!t.armed) return { tracker: t, done: null };
    // true → false:这一次重采集结束了。
    const seconds = totalSeconds(t.intervals);
    const done =
        seconds >= MIN_REPORT_S && t.intervals.length > 0 && t.capMask !== 0
            ? doneOf(t.intervals, t.capMask)
            : null;
    return { tracker: recapTrackerInit(), done };
}

function doneOf(intervals, mask) {
    return {
        seconds: totalSeconds(intervals),
        tracksMask: mask,
        startS: intervals[0][0],
        endS: intervals[intervals.length - 1][1],
        intervals,
    };
}

/**
 * 新一次「重采集结束」到来时,上一条 toast 若还开着(用户既没关也没点重分析)⇒ **并进去**,
 * 不是顶掉:顶掉的话上一段重采集的「建议重分析」就无声丢了。秒数取两段区间的并集
 * (同一段循环重采两次不重复计),范围取外包络,轨取并。
 */
export function mergeRecapDone(prev, next) {
    if (!next) return prev || null;
    if (!prev) return next;
    let list = (prev.intervals || []).slice();
    for (const [a, b] of next.intervals || []) list = addInterval(list, a, b);
    if (list.length === 0) return next;
    return doneOf(list, (prev.tracksMask | next.tracksMask) >>> 0);
}

/**
 * 每一帧 §2.6 调(`prev` = 覆写前的上一帧,`cur` = 本帧;state 取当前深合并结果)。
 *
 * 计入条件(逐条):布防中 ∧ 01 采集开着 ∧ 两帧里至少一帧在播(起播那一步与停播那一步
 * 播放头都真的走过)∧ 0 < Δt ≤ MAX_STEP_S。计入的是 `[prev.timeS, cur.timeS]` 与布防
 * 选区 `[startS, endS]` 的交集。
 */
export function recapOnPlayhead(tracker, state, prev, cur) {
    const t = tracker || recapTrackerInit();
    if (!t.armed || !prev || !cur) return t;
    if (!(((state || {}).global || {}).capture_enabled === true)) return t;
    if (!(prev.isPlaying === true || cur.isPlaying === true)) return t;
    const a = num(prev.timeS, NaN);
    const b = num(cur.timeS, NaN);
    if (!Number.isFinite(a) || !Number.isFinite(b)) return t;
    const dt = b - a;
    if (!(dt > 0) || dt > MAX_STEP_S) return t;
    const s0 = Math.max(a, t.startS);
    const s1 = Math.min(b, t.endS);
    if (!(s1 > s0)) return t;
    return {
        ...t,
        capMask: t.capMask | t.mask,
        intervals: addInterval(t.intervals, s0, s1),
    };
}

/** toast 里的「X.X」:保留一位小数。 */
export function fmtRecapSeconds(s) {
    return (Math.round(num(s, 0) * 10) / 10).toFixed(1);
}
