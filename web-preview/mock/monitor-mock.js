// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Monitor 侧 mock 后端(T46;**已按 T44 段规格对表**)
// =============================================================================
// 与 `./state-driver.js` 同一份契约,壳页照旧只认这一个入口:
//
//   import { createPreviewSession } from "./mock/monitor-mock.js";
//   const session = createPreviewSession({ role: "monitor", params: location.search });
//   targetWindow.__SCVB_MOCK__ = session.mock;   // 必须早于真源页面的 app.js 求值
//   session.start();
//
// **为什么另起一份而不是往 state-driver.js 里加一个 role**:那份 driver 的每一条周期事件、
// 每一个 fixture 分支都按 Output/Input 两侧的契约写死(§2 九事件 / §4 五事件);加第三侧要在
// 十几处 `if (role === …)` 上分叉,而 Monitor 的数据面(viz 段)与那两侧**没有一个字段重合**。
// 分成两份,既有两侧一行不动(smoke-mock / smoke-input 是证据),将来段改也只改本文件。
// **数据仍同源**:世界经 `state-driver.js` 的 `buildWorld()` 造 —— 段表、轨名、立体声轨、
// 时间线全部来自 `web/shared/mock-data.js` 的同一批生成器,Monitor 看到的和 Output 看到的
// 是同一个工程,不是另编的一套假数据。
//
// =============================================================================
// 本文件是 T44 viz 段的**行为替身**,不是自拟形状
// =============================================================================
// 段规格真源 = `docs/contract-changes/20260825-viz-segment.md` + `src/core/ipc/VizPlane.h`
// (T44,PR #89);JS 侧镜像 = `web/monitor/viz-contract.js`。本文件按那份规格逐条造数,
// 包括这些**容易两边写反**的地方 —— 每一条 smoke 里都有对应断言:
//   • 车道 = 列**中心时刻点采样**(不是列内均值);
//   • 覆盖位图 = 列区间与任一分段**有交集**即置 1(**保守口径**,短于一列的分段不消失)——
//     注意与车道的判据**不同**:车道是点采样、位图是区间求交,两者刻意不一致;
//   • 位图位序 LSB 优先、每字 32 列;
//   • 车道定点 `round(clamp(pan,−100,100) × 100)`,整轨无数据 ⇒ 全 `kVizPanNone`;
//   • **断线以位图为准**:曲线求值会填补空隙,故本文件在没有分段的列上**照样填车道值**
//     (hold 上一段的 pan)—— 只有位图是 0。消费侧若误把车道当覆盖判据,断线会整个消失,
//     这正是 smoke 要抓的那个错;
//   • 窗口跨度 = `max(最大分段末端, playhead+1, 60s)` 向上取整到 **30s** 边界,上限 24h;
//   • 帧头 4Hz;车道 / 位图 / 轨色**按需重算**(`lane_revision` 变化时才随事件带上)。
//
// **样本 ↔ 秒**:段里是样本,而契约 §0.2「UI 永不见样本、只收秒」—— 换算发生在 T45 的
// C++ 桥。本文件模拟的是**桥之后**的那一层,故对外发秒;但内部仍按样本算一遍再除,
// 把换算式也跑到(见 `SAMPLES_TO_SECONDS_NOTE`)。
//
// 依赖方向:web-preview/ → web/(单向,06 §6.2)。本文件不碰 DOM、不复制任何 UI 代码。
// =============================================================================

import { buildWorld } from "./state-driver.js";
import {
    CHANNEL_COUNT,
    DEMO_DURATION_S,
    DEMO_GROUPS_ONLINE,
} from "../../web/shared/mock-data.js";
import {
    GROUPS_JSON_KEY,
    VIZ_ABI,
    VIZ_COLUMNS,
    VIZ_COVERAGE_WORDS,
    VIZ_FLAG_PLAYING,
    VIZ_MAGIC,
    VIZ_PAN_NONE,
    VIZ_PAN_SCALE,
    VIZ_TRACKS,
} from "../../web/monitor/viz-contract.js";

// -----------------------------------------------------------------------------
// 0. 常量
// -----------------------------------------------------------------------------

/** 组数(契约 §0.2:g = 1..8)。 */
const GROUP_COUNT = 8;

/** 采样率(段的 `sample_rate`;换算式跑得到就行,值本身随便取一个常见的)。 */
const SAMPLE_RATE = 48000;

/** `VizTrackLabels` 的每轨定长槽(字节;T44:`utf8[15][8]` u32 = 32 B/轨)。 */
const VIZ_LABEL_BYTES = 32;

/** 窗口跨度量化边界(秒)与下限、上限 —— 逐条照 T44 变更文档「窗口跨度」。 */
const WINDOW_QUANTUM_S = 30;
const WINDOW_MIN_S = 60;
const WINDOW_MAX_S = 24 * 3600;

/** 帧头发布周期(ms):4Hz,照 T44「帧头标量 250ms」。 */
const VIZ_FRAME_PERIOD_MS = 250;

/** 车道兜底重算周期(ms):照 T44「距上次重算 ≥ 1s」四触发之一。 */
const LANE_RECALC_MS = 1000;

/**
 * 播放头频率:**25Hz**(T45 说明:WebViewHost 定时器给到的上限就是这个,Output 侧也一样)。
 * 数据源是 Monitor **自己的 AudioPlayHead** —— 与 viz 段无关,故 Output 停摆时竖线照常走,
 * 消费侧不必为「viz 陈旧」特判播放头。
 */
const PLAYHEAD_PERIOD_MS = 40;

/** groups 位图频率(§2.4 原样 1Hz,变化才发)。 */
const GROUPS_PERIOD_MS = 1000;

/**
 * 各组的轨集(演示用)。
 *
 * 三个在线组(A/B/E = `DEMO_GROUPS_ONLINE`)刻意给**不同的轨数**:切组时画面必须肉眼可辨
 * 地变一次,否则「切过去了」和「切失败了、还在看上一组」长得一模一样。
 */
const GROUP_TRACKS = Object.freeze({
    1: Object.freeze(range1(CHANNEL_COUNT)), // A:满配 15 轨
    2: Object.freeze([1, 2, 3, 4, 5, 6]), // B:6 轨小编制
    5: Object.freeze([1, 2, 7, 8, 9, 10, 11, 12, 13]), // E:9 轨,轨号不连续
});

function range1(n) {
    return Array.from({ length: n }, (_, i) => i + 1);
}

function clamp(lo, hi, v) {
    return v < lo ? lo : v > hi ? hi : v;
}

// -----------------------------------------------------------------------------
// 1. 查询参数
// -----------------------------------------------------------------------------

/** Monitor 侧的演示场景名(壳页白名单 `SCENARIO_NAMES.monitor` 引用本表,不抄第二份)。 */
export const MONITOR_SCENARIOS = Object.freeze([
    "monitor-online", // 满配:组 A,15 轨全画
    "monitor-offline", // 空态:观察的组没有 Output(attach = failed)
    "monitor-groups", // 组切换:开箱停在组 B(6 轨),可点到 A(15 轨)/ E(9 轨)
    "monitor-stalled", // 在线但陈旧:state 的 fresh=false ⇒ 琥珀横幅,**图仍显示**
    "monitor-abi", // 拒连:state 报 abiMismatch ⇒ 红横幅 + 停止读取
    "monitor-reconnect", // 重连:开箱 offline,3 秒后 Output 上线 ⇒ 自动出图
    "monitor-no-lanes", // 降级:桥没送车道 ⇒ 轨迹图走「未接通」的专门空态
    "monitor-no-tracks", // 降级:没有每轨三条标量 ⇒ 分布图画空、轨迹图照常
    "monitor-no-lead", // 降级:没有 leadMask ⇒ 柱照画、无绿帽
]);

function toSearchParams(params) {
    if (params instanceof URLSearchParams) return params;
    if (typeof params === "string") return new URLSearchParams(params);
    if (params && typeof params === "object") {
        return new URLSearchParams(
            Object.entries(params).map(([k, v]) => [k, String(v)]),
        );
    }
    return new URLSearchParams("");
}

/**
 * 解析 `?scenario=` / `?group=` / `?play=`。
 * 未知 scenario 回落 `monitor-online` 并 warn —— 与 state-driver 同款「不假装支持」。
 */
export function parseMonitorQuery(params) {
    const q = toSearchParams(params);
    const warnings = [];

    const raw = q.get("scenario");
    let scenario = "monitor-online";
    if (raw) {
        if (MONITOR_SCENARIOS.includes(raw)) scenario = raw;
        else {
            warnings.push(
                `场景 ${raw} 不在 Monitor 场景表内,已回落 ${scenario}`,
            );
        }
    }

    const rawGroup = Number(q.get("group"));
    let group = null;
    if (q.get("group") !== null) {
        if (
            Number.isInteger(rawGroup) &&
            rawGroup >= 1 &&
            rawGroup <= GROUP_COUNT
        ) {
            group = rawGroup;
        } else {
            warnings.push(`group=${q.get("group")} 非法(1..8),已按场景默认组`);
        }
    }

    const rawPlay = q.get("play");
    const play =
        rawPlay === null ? true : rawPlay !== "0" && rawPlay !== "false";

    return { scenario, group, play, warnings };
}

// -----------------------------------------------------------------------------
// 2. 段的栅格化(本文件的核心;逐条照 T44 的降采样口径)
// -----------------------------------------------------------------------------

/**
 * 窗口跨度(秒)。
 * `max(最大分段末端, playhead+1, 60s)` 向上取整到 30s 边界,上限 24h。
 * 量化是为了让跨度在播放推进中保持稳定 —— 否则车道每帧都要重算。
 */
export function windowSpanS(maxSegEndS, playheadS) {
    const raw = Math.max(
        Number(maxSegEndS) || 0,
        (Number(playheadS) || 0) + 1,
        WINDOW_MIN_S,
    );
    const q = Math.ceil(raw / WINDOW_QUANTUM_S) * WINDOW_QUANTUM_S;
    return Math.min(q, WINDOW_MAX_S);
}

/**
 * 工程量 → 段内定点 int16(`round(v × 100)`)。
 * 三条 `VizTrackState` 标量与车道共用同一个标度,故编码也只此一处。
 * 非数 ⇒ 哨兵(该轨无数据)。
 */
export function fixedOf(v) {
    if (!Number.isFinite(v)) return VIZ_PAN_NONE;
    return Math.round(v * VIZ_PAN_SCALE);
}

/** pan → 段内定点 int16(先夹到角度域再定点)。 */
export function panToFixed(pan) {
    if (!Number.isFinite(pan)) return VIZ_PAN_NONE;
    return fixedOf(clamp(-100, 100, pan));
}

/**
 * 单轨段表 → `{lane: int16[1024], words: u32[32]}`。
 *
 * **两条判据刻意不同**(照 T44):
 *   • `lane[i]` = 列**中心时刻**的曲线求值。曲线会 hold —— 没有分段的列**照样有值**
 *     (取最近一段的 pan),整轨无分段时才全哨兵;
 *   • `words` 的第 i 位 = 列区间 `[i·colS, (i+1)·colS)` 与任一分段**有交集**即 1
 *     (保守口径:短于一列的分段不会消失)。
 *
 * 于是「车道有值 ≠ 有覆盖」——**断线只能看位图**。消费侧若拿车道当覆盖判据,断线会整个
 * 消失而图看起来完全正常,这正是 smoke ③ 要抓的那个错。
 */
export function rasterizeTrack(segments, spanS, startS = 0) {
    const lane = new Array(VIZ_COLUMNS).fill(VIZ_PAN_NONE);
    const words = new Array(VIZ_COVERAGE_WORDS).fill(0);
    const segs = (segments || [])
        .filter((s) => s && Number.isFinite(s.t0S) && s.t1S > s.t0S)
        .slice()
        .sort((a, b) => a.t0S - b.t0S);
    if (segs.length === 0 || !(spanS > 0)) return { lane, words };

    const colS = spanS / VIZ_COLUMNS;
    for (let i = 0; i < VIZ_COLUMNS; i++) {
        const c0 = startS + i * colS;
        const c1 = c0 + colS;
        // ---- 位图:与任一分段有交集(半开区间求交,端点相接不算交集)
        for (const s of segs) {
            if (s.t0S < c1 && s.t1S > c0) {
                words[i >>> 5] = (words[i >>> 5] | (1 << (i & 31))) >>> 0;
                break;
            }
        }
        // ---- 车道:列中心点采样 + hold(CurveEvaluator 的填补语义)
        const mid = c0 + colS / 2;
        let hold = null;
        for (const s of segs) {
            if (s.t0S > mid) break;
            hold = s; // 最后一个起点 ≤ mid 的段
        }
        if (hold) lane[i] = panToFixed(hold.pan);
    }
    return { lane, words };
}

/** 掩码:把轨号数组折成 `bit{ch−1}` 的 u32。 */
export function maskOf(channels) {
    let m = 0;
    for (const ch of channels || []) {
        if (Number.isInteger(ch) && ch >= 1 && ch <= VIZ_TRACKS) {
            m = (m | (1 << (ch - 1))) >>> 0;
        }
    }
    return m;
}

/**
 * 造一个组的车道/位图/掩码(只算一次,之后每帧只拼帧头)。
 * @returns {null|object} 组不在线 ⇒ null(attach 会得 failed)
 */
function buildGroup(world, groupId) {
    const tracks = GROUP_TRACKS[groupId] || null;
    if (!tracks) return null;

    const segChannels = (world.output.segments || {}).channels || [];
    const cfgChannels = (world.output.snapshot || {}).channels || [];

    let maxEnd = 0;
    for (const ch of tracks) {
        const entry = segChannels.find((c) => c.ch === ch);
        for (const s of (entry && entry.segments) || []) {
            if (s && s.t1S > maxEnd) maxEnd = s.t1S;
        }
    }
    const spanS = windowSpanS(maxEnd, 0);

    // 段里的三张表恒为**满 15 轨**(定长数组),未启用的轨是全哨兵 + 位图全 0 ——
    // 这正是段布局的形状,mock 不许偷偷只给 N 轨。
    const lanes = [];
    const coverage = [];
    const colorIndex = [];
    const stereo = [];
    const covered = [];
    const lead = [];
    const trackMeta = [];
    for (let ch = 1; ch <= VIZ_TRACKS; ch++) {
        colorIndex.push(ch); // v1 恒 = 轨号(T44 段内注释)
        if (!tracks.includes(ch)) {
            lanes.push(new Array(VIZ_COLUMNS).fill(VIZ_PAN_NONE));
            coverage.push(new Array(VIZ_COVERAGE_WORDS).fill(0));
            continue;
        }
        const entry = segChannels.find((c) => c.ch === ch);
        const segs = (entry && entry.segments) || [];
        const { lane, words } = rasterizeTrack(segs, spanS, 0);
        lanes.push(lane);
        coverage.push(words);
        if (words.some((w) => w !== 0)) covered.push(ch);
        const cfg = cfgChannels[ch - 1] || {};
        if (cfg.source_channels === 2) stereo.push(ch);
        if (cfg.lead_lock) lead.push(ch);
        trackMeta.push({ ch, cfg, segs });
    }

    // `VizTrackLabels`:每轨 32 字节 UTF-8 定长槽,超长按 UTF-8 边界截断
    // (T44 承诺「不会切出半个汉字」)。这里按同一条规则截,好让 preview 里
    // 的长轨名与真段一致 —— 中文一字 3 字节,32 字节 ≈ 10 个汉字。
    const labels = [];
    for (let ch = 1; ch <= VIZ_TRACKS; ch++) {
        const cfg = cfgChannels[ch - 1] || {};
        labels.push(truncateUtf8(String(cfg.label || ""), VIZ_LABEL_BYTES));
    }

    return {
        tracks,
        spanS,
        lanes,
        coverage,
        colorIndex,
        labels,
        onlineMask: maskOf(tracks),
        coveredMask: maskOf(covered),
        stereoMask: maskOf(stereo),
        leadMask: maskOf(lead),
        trackMeta,
    };
}

/**
 * 按 UTF-8 字节数截断,**不切出半个字符**。
 * `TextEncoder` 在 node ≥ 11 与所有浏览器里都是全局的;逐字符累加字节数,
 * 超了就停 —— 比先编码再按字节切回来简单,也天然不会产生半个代理对。
 */
export function truncateUtf8(s, maxBytes) {
    const enc = new TextEncoder();
    if (enc.encode(s).length <= maxBytes) return s;
    let out = "";
    let n = 0;
    for (const chpt of s) {
        const w = enc.encode(chpt).length;
        if (n + w > maxBytes) break;
        out += chpt;
        n += w;
    }
    return out;
}

/** 段表里覆盖住 `tS` 的那一段;没有就取最近一段(引擎在段间 hold)。 */
function segmentAt(segs, tS) {
    let hold = null;
    for (const s of segs || []) {
        if (s.t0S <= tS && tS < s.t1S) return s;
        if (s.t1S <= tS && (!hold || s.t1S > hold.t1S)) hold = s;
    }
    return hold;
}

// -----------------------------------------------------------------------------
// 3. mock 后端
// -----------------------------------------------------------------------------

function createMonitorBackend(parsed) {
    const world = buildWorld({ role: "output", fixture: "fifteen-tracks" });

    let groupsOnline = DEMO_GROUPS_ONLINE; // A/B/E
    let observed = 1;
    if (parsed.scenario === "monitor-groups") observed = 2; // 开箱停在 B(6 轨)
    if (parsed.scenario === "monitor-offline") observed = 3; // C:没有 Output
    if (parsed.scenario === "monitor-reconnect") observed = 1; // A,但先假装还没起来
    if (parsed.group) observed = parsed.group;

    const groupCache = new Map();
    const groupOf = (g) => {
        if (!groupCache.has(g)) groupCache.set(g, buildGroup(world, g));
        return groupCache.get(g);
    };

    const state = {
        observed,
        tS: 42,
        isPlaying: parsed.play,
        seq: 2, // 段里 seq 偶 = 稳定;桥只在稳定帧上投影,故对外恒为偶数
        generation: 1,
        laneRevision: 1,
        publishMs: 1000,
        // `scvb.state` 的 viz 面:三态 + **独立的** fresh(T45 `buildStatePayload`)。
        // 「在线但陈旧」是真实存在的一档 —— Output 还在跑、只是不再发帧。
        vizState:
            parsed.scenario === "monitor-abi"
                ? "abiMismatch"
                : parsed.scenario === "monitor-offline" ||
                    parsed.scenario === "monitor-reconnect"
                  ? "offline"
                  : "online",
        fresh: parsed.scenario !== "monitor-stalled",
        abi: VIZ_ABI,
        // `monitor-reconnect`:先起不来,由 driver 在 3 秒后翻成 true
        outputUp: parsed.scenario !== "monitor-reconnect",
        // `monitor-no-lanes`:桥不送车道两件 ⇒ 轨迹图走「未接通」的专门空态
        withLanes: parsed.scenario !== "monitor-no-lanes",
        // `monitor-no-tracks`:桥没送每轨三条标量(旧版 Output 的形态)⇒ 分布图画空
        withTracks: parsed.scenario !== "monitor-no-tracks",
        // `monitor-no-lead`:有三条标量但没有 leadMask ⇒ 柱照画、一律不戴绿帽
        withLead: parsed.scenario !== "monitor-no-lead",
        scale: 1,
        language: "zh",
        committedScale: 1,
        lastLaneRecalcMs: 0,
        lastSentLaneRevision: new Map(), // groupId → 已随事件发过的 laneRevision
    };

    const listeners = new Map();
    let ready = false;
    let lastStateJson = "";
    const onReadyQueue = [];

    function emit(name, payload) {
        for (const cb of listeners.get(name) || []) cb(payload);
    }

    /** `scvb.state`(T45 `buildStatePayload`):组 / 缩放 / 语言 / viz 三态 / fresh。 */
    function statePayload() {
        return {
            groupId: state.observed,
            uiScale: state.scale,
            language: state.language,
            viz: state.vizState,
            fresh: state.fresh,
        };
    }

    /**
     * 造一帧 viz。
     *
     * @param {boolean} [withLanes] 是否带车道三件。缺省按 `lane_revision` 决定:
     *   只有「这一组的这个 revision 还没发过」时才带 —— 稳态下 4Hz 只发 128 B 帧头,
     *   与 T44 的「稳态下只写帧头」逐条对应。
     */
    function vizFrame(groupId, tS, withLanes) {
        const g = state.outputUp ? groupOf(groupId) : null;

        // 帧里的 `online` 与 `fresh` 是**两件事、各说各的**(T45 `decae38` 把它们拆开了):
        //   `online` = 段已 attach 且可读;`fresh` = 帧还在更新。
        // 拆开之前是一个与在一起的布尔 —— 于是「写方停摆」与「真掉线」在载荷上完全同形,
        // 消费侧只能靠判据顺序分开。mock 必须跟着拆:形状不同形,这边测出来的是另一个
        // 接口的行为,而 DAW 里才发现的那种错正是这么来的。
        const online = state.vizState === "online" && !!g;

        const base = {
            magic: VIZ_MAGIC,
            abi: state.abi,
            generation: state.generation,
            columnCount: VIZ_COLUMNS,
            trackCount: VIZ_TRACKS,
            panScale: VIZ_PAN_SCALE,
            online,
            fresh: state.fresh,
            // 帧自带组号:换组后消费侧据此丢在途帧,不必依赖「事件一定按序到达」。
            groupId,
            seq: state.seq,
            publishMs: state.publishMs,
            playheadEpoch: 1,
            versionActive: 1,
            sampleRate: SAMPLE_RATE,
        };

        if (!g) {
            // 段读不到 —— 窗口/掩码/车道一律为空,而**事件照发**:
            // 「没有事件」在 UI 侧分不清是离线还是桥断了。
            return {
                ...base,
                windowStartS: 0,
                windowSpanS: 0,
                playheadS: null,
                playheadFlags: 0,
                loopStartS: null,
                loopEndS: null,
                onlineMask: 0,
                coveredMask: 0,
                stereoMask: 0,
                leadMask: 0,
                laneRevision: state.laneRevision,
            };
        }

        // ---- 样本 → 秒:段里是样本,桥换算成秒(契约 §0.2)。这里按样本算一遍再除,
        // 把换算式真的跑到(`sample_rate === 0` 的除零分支由 windowSpanS>0 保证不进)。
        const windowStartSamples = 0;
        const windowSpanSamples = Math.round(g.spanS * SAMPLE_RATE);
        const playheadSamples = Math.round(tS * SAMPLE_RATE);

        // 本 mock 只演 playing 这一位。`kVizLooping` / `kVizLoopValid` 与两个 loop 端点
        // 段里有、但**两张图都不消费**(轨迹图不画循环区)—— 造一份没人读的数据只会让
        // 「对表时以为已经验过」。等哪张图真要用它了再补,那时才有可断言的行为。
        const flags = state.isPlaying ? VIZ_FLAG_PLAYING : 0;

        const frame = {
            ...base,
            windowStartS: windowStartSamples / SAMPLE_RATE,
            windowSpanS: windowSpanSamples / SAMPLE_RATE,
            playheadS: playheadSamples / SAMPLE_RATE,
            playheadFlags: flags,
            loopStartS: null,
            loopEndS: null,
            onlineMask: g.onlineMask,
            coveredMask: g.coveredMask,
            stereoMask: g.stereoMask,
            // `track_lead_mask` 仍待 T44 确认(viz-contract.js 的 VIZ_PENDING_FIELDS)。
            // `monitor-no-lead` 之外的场景先按「已经有了」造,好让柱顶绿帽在 preview
            // 里可验收;没有它时的行为由 `monitor-no-tracks` 之外的那条断言管。
            leadMask: state.withLead ? g.leadMask : undefined,
            laneRevision: state.laneRevision,
        };

        const need =
            withLanes === undefined
                ? state.lastSentLaneRevision.get(groupId) !== state.laneRevision
                : !!withLanes;
        if (need && state.withLanes) {
            state.lastSentLaneRevision.set(groupId, state.laneRevision);
            frame.colorIndex = g.colorIndex;
            frame.coverage = g.coverage;
            frame.lanes = g.lanes;
        }

        // ---- `VizTrackState`(三条定点标量)+ `VizTrackLabels`。
        // **每帧都发**:它们是「当前值」,随播放头逐帧变,不进 `lane_revision` 的
        // 按需重发(那条管的是 15×1024 的车道)。三条各 15 项 = 45 个数,便宜。
        //
        // `panNow` 按 **T44 对表信定的口径**:播放头**所在时刻**的曲线求值,
        // **不是** lane 在播放头列的采样(列中心,差半列)。mock 这里用「覆盖住 tS
        // 的那一段的 pan」当作精确时刻求值 —— 段内 pan 恒定,与引擎 CurveEvaluator
        // 在段内的取值一致。
        if (state.withTracks) {
            const panNow = [];
            const volDb = [];
            const widthPct = [];
            for (let ch = 1; ch <= VIZ_TRACKS; ch++) {
                const meta = g.trackMeta.find((m) => m.ch === ch);
                if (!meta) {
                    // 未启用的轨:段里是哨兵,**桥把哨兵折成 null**(T45 逐条照做)——
                    // 不是 0:0 在三条里分别是「居中 / 0 dB / 宽度 0」,都是合法值。
                    panNow.push(null);
                    volDb.push(null);
                    widthPct.push(null);
                    continue;
                }
                const seg = segmentAt(meta.segs, tS);
                panNow.push(seg ? seg.pan : null);
                volDb.push(seg ? seg.volDb : null);
                widthPct.push(meta.cfg.source_channels === 2 ? 82 : 100);
            }
            frame.trackPanNow = panNow;
            frame.trackVolDb = volDb;
            frame.trackWidthPct = widthPct;
            frame.trackLabels = g.labels;
        }
        return frame;
    }

    const backend = {
        addEventListener(name, cb) {
            if (!listeners.has(name)) listeners.set(name, []);
            listeners.get(name).push(cb);
        },

        /** §0.6 同款门控:调用前一个事件都不推。 */
        async requestInitialState() {
            ready = true;
            const snap = {
                abi: state.abi,
                version: "0.1.0",
                groupId: state.observed,
                groups_online: groupsOnline,
                ui: { scale: state.scale, language: state.language },
                // 首帧 viz 随快照给(**必带车道**)—— 与契约 §0.4「状态类各必发一次」同精神:
                // 没有它,页面要空等一整个 4Hz 周期才出图。
                viz: vizFrame(state.observed, state.tS, true),
            };
            for (const fn of onReadyQueue.splice(0)) fn();
            return snap;
        },

        /** 只读换观察对象:不 claim、不写 registry(J75 C)。 */
        async setObservedGroup(g) {
            if (!Number.isInteger(g) || g < 1 || g > GROUP_COUNT) {
                return { ok: false, reason: "badArg" };
            }
            state.observed = g;
            state.seq += 2; // 偶数保持偶数(奇数 = 写入中,不该被读方看到)
            // 组回显走 `scvb.state`(帧里那个 `groupId` 只用于丢在途帧,不当回显)
            emit("scvb.state", statePayload());
            // 换组 = 换段 = 另一份车道,故这一帧**必带**车道三件。
            emit("scvb.viz", vizFrame(g, state.tS, true));
            return { ok: true };
        },

        async setUiScale(f) {
            // 档位表由 UI 侧给(web/monitor/monitor-box.js);后端只夹合法区间。
            if (!Number.isFinite(f) || f <= 0 || f > 4) {
                return { ok: false, reason: "badArg" };
            }
            state.scale = f;
            return { ok: true };
        },

        async commitUiScale() {
            state.committedScale = state.scale;
            return { ok: true };
        },

        async setLang(code) {
            if (!["zh", "en", "fr"].includes(code)) {
                return { ok: false, reason: "badArg" };
            }
            state.language = code;
            return { ok: true };
        },
    };

    const ctl = {
        state,
        world,
        emit,
        vizFrame,
        groupOf,
        isReady: () => ready,
        onReady: (fn) => (ready ? fn() : onReadyQueue.push(fn)),
        // ⚠ T45 的 `scvb.groups` 用 **`online`** 这个键,不是 Output 侧的 `groups_online`
        // (真源 = MonitorEditor.cpp;常量在 viz-contract.js 的 GROUPS_JSON_KEY)。
        // 读错的后果是绿点永远不亮、而页面一切正常零报错 —— 故两侧都从那个常量取。
        groupsPayload: () => ({ [GROUPS_JSON_KEY]: groupsOnline }),
        statePayload,
        /** `scvb.state` 变化才发(§0.4「值未变不发」同款)。 */
        emitIfStateChanged() {
            const json = JSON.stringify(statePayload());
            if (json === lastStateJson) return;
            lastStateJson = json;
            emit("scvb.state", statePayload());
        },
        setGroupsOnline(bitmap) {
            groupsOnline = bitmap;
        },
        /** 测试面:让 Output「上线」(重连场景;driver 也用它)。 */
        bringOutputUp() {
            if (state.outputUp) return;
            state.outputUp = true;
            state.vizState = "online";
            state.fresh = true;
            state.generation += 1; // 覆盖式重初始化 +1(段被重建)
            state.laneRevision += 1; // 新段 = 新车道,读方必须重解析
            state.seq += 2;
            emit("scvb.state", statePayload());
            emit("scvb.viz", ctl.vizFrame(state.observed, state.tS, true));
        },
    };

    return { backend, ctl };
}

// -----------------------------------------------------------------------------
// 4. 周期事件
// -----------------------------------------------------------------------------

function makeDriver(ctl) {
    const timers = [];
    const frameLoops = [];
    let running = false;
    let autoReqId = null;

    /**
     * 30Hz 档专用的帧循环 —— 理由同 state-driver.js:Windows 的定时器分辨率 ~15.6ms 会把
     * `setInterval(33)` 抬到 ~46ms(实测 21.4Hz),达不到 §2.6 的 30Hz。浏览器用
     * rAF + 时间累加器;node 无 rAF 时回落 setInterval。
     */
    function startFrameLoop(periodMs, fn) {
        if (typeof requestAnimationFrame === "function") {
            let last = 0;
            let handle = 0;
            let alive = true;
            const step = (now) => {
                if (!alive) return;
                handle = requestAnimationFrame(step);
                if (now - last < periodMs) return;
                last = now;
                fn();
            };
            handle = requestAnimationFrame(step);
            frameLoops.push(() => {
                alive = false;
                if (typeof cancelAnimationFrame === "function") {
                    cancelAnimationFrame(handle);
                }
            });
            return;
        }
        timers.push(setInterval(fn, periodMs));
    }

    return {
        start(backend) {
            if (running) return;
            running = true;

            ctl.onReady(() => {
                ctl.emit("scvb.state", ctl.statePayload());
                ctl.emit("scvb.groups", ctl.groupsPayload());
            });

            // 25Hz:播放头(§2.6 载荷原样,与 Output 侧同形)
            startFrameLoop(PLAYHEAD_PERIOD_MS, () => {
                const s = ctl.state;
                if (s.isPlaying) {
                    s.tS += PLAYHEAD_PERIOD_MS / 1000;
                    if (s.tS >= DEMO_DURATION_S) s.tS -= DEMO_DURATION_S;
                }
                ctl.emit("scvb.playhead", {
                    timeS: Math.round(s.tS * 1000) / 1000,
                    isPlaying: s.isPlaying,
                    inRange: true,
                });
            });

            // 4Hz:viz 帧头(T44「帧头标量 250ms」)。
            // `monitor-stalled` 下 publishMs 冻住 —— 事件照发,停摆的是时刻。
            timers.push(
                setInterval(() => {
                    const s = ctl.state;
                    s.publishMs += VIZ_FRAME_PERIOD_MS;
                    s.seq += 2;
                    // 车道兜底重算(T44 四触发之一:距上次 ≥1s)
                    if (s.publishMs - s.lastLaneRecalcMs >= LANE_RECALC_MS) {
                        s.lastLaneRecalcMs = s.publishMs;
                        // 本 mock 的段表是静态的,内容没变 ⇒ **lane_revision 不 +1**。
                        // T44 的语义是「只在重算车道时 +1」,而重算出同样的内容也不该
                        // 让读方白重解析一次 15×1024 —— 这条正是稳态省流的来源。
                    }
                    ctl.emit("scvb.viz", ctl.vizFrame(s.observed, s.tS));
                }, VIZ_FRAME_PERIOD_MS),
            );

            // 1Hz:组在线位图 + scvb.state(都是变化才发)
            timers.push(
                setInterval(() => {
                    ctl.emit("scvb.groups", ctl.groupsPayload());
                    ctl.emitIfStateChanged();
                }, GROUPS_PERIOD_MS),
            );

            // 重连场景:3 秒后 Output 上线(段被创建,generation +1)
            if (!ctl.state.outputUp) {
                timers.push(setTimeout(() => ctl.bringOutputUp(), 3000));
            }

            // 兜底代调 requestInitialState()(与 state-driver 同款:页面若没调,
            // 契约门控会让事件流永不开始,预览页静默停在空态)
            autoReqId = setTimeout(() => {
                autoReqId = null;
                if (ctl.isReady()) return;
                console.info(
                    "SCVB web-preview:Monitor 页面未调用 requestInitialState(),driver 代调一次。",
                );
                backend.requestInitialState();
            }, 1500);
        },
        stop() {
            running = false;
            for (const cancel of frameLoops) cancel();
            frameLoops.length = 0;
            for (const id of timers) clearInterval(id);
            timers.length = 0;
            if (autoReqId !== null) clearTimeout(autoReqId);
            autoReqId = null;
        },
    };
}

// -----------------------------------------------------------------------------
// 5. 对外入口(与 state-driver.js 同一契约,壳页两边通用)
// -----------------------------------------------------------------------------

/**
 * 造一次 Monitor 预览会话 —— **壳页的唯一入口**。
 *
 * @param {{params?:string|URLSearchParams|object, scenario?:string, group?:number}} opts
 * @returns {{mock:object, ctl:object, world:object, start:Function, stop:Function,
 *            info:{role:string, scenario:string, group:number, warnings:string[]}}}
 */
export function createPreviewSession(opts = {}) {
    const parsed = parseMonitorQuery(
        opts.params !== undefined
            ? opts.params
            : typeof location !== "undefined"
              ? location.search
              : "",
    );
    const warnings = parsed.warnings.slice();
    if (opts.scenario) {
        if (MONITOR_SCENARIOS.includes(opts.scenario)) {
            parsed.scenario = opts.scenario;
        } else {
            warnings.push(
                `场景 ${opts.scenario} 不存在,已按 ${parsed.scenario}`,
            );
        }
    }
    if (Number.isInteger(opts.group)) parsed.group = opts.group;

    const { backend, ctl } = createMonitorBackend(parsed);
    const driver = makeDriver(ctl);

    for (const w of warnings) console.warn(`SCVB web-preview:${w}`);

    return {
        mock: backend,
        ctl,
        world: ctl.world,
        info: {
            role: "monitor",
            scenario: parsed.scenario,
            group: ctl.state.observed,
            warnings,
        },
        start: () => driver.start(backend),
        stop: () => driver.stop(),
    };
}
