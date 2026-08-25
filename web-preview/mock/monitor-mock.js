// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Monitor 侧 mock 后端(T46;**待与 T44/T45 接口说明对表**)
// =============================================================================
// 与 `./state-driver.js` 同一份契约,壳页照旧只认这一个入口:
//
//   import { createPreviewSession } from "./mock/monitor-mock.js";
//   const session = createPreviewSession({ role: "monitor", params: location.search });
//   targetWindow.__SCVB_MOCK__ = session.mock;   // 必须早于真源页面的 app.js 求值
//   session.start();
//
// **为什么另起一份而不是往 state-driver.js 里加一个 role**:那份 driver 的每一条
// 周期事件、每一个 fixture 分支都按 Output/Input 两侧的契约写死(§2 九事件 / §4 五事件);
// 加第三侧要在十几处 `if (role === …)` 上分叉,而 Monitor 的数据面(viz 段)与那两侧
// **没有一个字段重合**。分成两份,既有两侧一行不动(smoke-mock / smoke-input 是证据),
// 将来 viz 段定稿要改的也只有本文件。
// **数据仍同源**:世界经 `state-driver.js` 的 `buildWorld()` 造 —— 段表、轨名、
// 立体声轨、时间线长度全部来自 `web/shared/mock-data.js` 的同一批生成器,Monitor 看到的
// 和 Output 看到的是同一个工程,不是另编的一套假数据。
//
// **viz 事件形状**的定义与「对表时要向 T44 要到的三条」写在 `web/monitor/viz.js` 的
// 文件头(消费侧),本文件只按那份说明造数,不在两处各写一份说明。
//
// 依赖方向:web-preview/ → web/(单向,06 §6.2)。本文件不碰 DOM、不复制任何 UI 代码。
// =============================================================================

import { buildWorld } from "./state-driver.js";
import {
    CHANNEL_COUNT,
    DEMO_DURATION_S,
    DEMO_GROUPS_ONLINE,
    makeGroups,
    makePlayhead,
} from "../../web/shared/mock-data.js";
import { VIZ_ABI, VIZ_MAGIC } from "../../web/monitor/viz.js";

// -----------------------------------------------------------------------------
// 0. 常量
// -----------------------------------------------------------------------------

/** 组数(契约 §0.2:g = 1..8)。 */
const GROUP_COUNT = 8;

/**
 * 降采样格数。600 格 × 5 分钟 = 每格 0.5s —— 比最细的分段(min_segment_ms 420ms)
 * 略粗一档:再细就是把段表原样搬进段里(那样降采样就没意义了),再粗则短句会整个
 * 落进一格、断线看不出来。真值由 T44 定,此处的口径供对表时对齐量级。
 */
const SLOT_COUNT = 600;

/** viz 发布频率(低频;J75 C「Output **消息线程**低频发布」)。 */
const VIZ_PERIOD_MS = 250;

/** 播放头频率(§2.6 原样 30Hz)。 */
const PLAYHEAD_PERIOD_MS = 33;

/** groups 位图频率(§2.4 原样 1Hz,变化才发)。 */
const GROUPS_PERIOD_MS = 1000;

/**
 * 各组的轨集(演示用)。
 *
 * 三个在线组(A/B/E = `DEMO_GROUPS_ONLINE`)刻意给**不同的轨数**:切组时画面必须
 * 肉眼可辨地变一次,否则「切组」这条验收路径看起来永远是通过的 —— 画面没变化时,
 * 「切过去了」和「切失败了、还在看上一组」长得一模一样。
 */
const GROUP_TRACKS = Object.freeze({
    1: Object.freeze(range1(CHANNEL_COUNT)), // A:满配 15 轨
    2: Object.freeze([1, 2, 3, 4, 5, 6]), // B:6 轨小编制
    5: Object.freeze([1, 2, 7, 8, 9, 10, 11, 12, 13]), // E:9 轨,轨号不连续
});

function range1(n) {
    return Array.from({ length: n }, (_, i) => i + 1);
}

// -----------------------------------------------------------------------------
// 1. 查询参数
// -----------------------------------------------------------------------------

/** Monitor 侧的演示场景名(壳页白名单 `SCENARIO_NAMES.monitor` 与本表必须同改)。 */
export const MONITOR_SCENARIOS = Object.freeze([
    "monitor-online", // 满配:组 A,15 轨全画
    "monitor-offline", // 空态:观察的组没有 Output 在线
    "monitor-groups", // 组切换:开箱停在组 B(6 轨),可点到 A(15 轨)/ E(9 轨)
    "monitor-stalled", // 停更:viz 的 seq 冻住 ⇒ 琥珀横幅
]);

function hasOwn(obj, key) {
    return Object.prototype.hasOwnProperty.call(obj, key);
}

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
        else
            warnings.push(
                `场景 ${raw} 不在 Monitor 场景表内,已回落 ${scenario}`,
            );
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
// 2. 世界 —— 把 Output 的段表栅格化成 viz 段的形状
// -----------------------------------------------------------------------------

/**
 * 段表 → 一轨的 `{pan, activity}`。
 *
 * 逐格取**格中心**落在哪一段里:段覆盖住格中心 ⇒ 该格 activity 位置 1、pan 取段值;
 * 没有任何段覆盖 ⇒ 位留 0(消费侧据此断线,J75 A)。取格中心而不是「与格有交集」,
 * 是因为后者会把每个段的两端各多染一格 —— 两段之间不足一格的缝隙会被抹平,
 * 断线跟着消失,而断线正是这张图要看的东西。
 *
 * activity 是 **u32 位图**(LSB 起、每字 32 格),与 `web/monitor/viz.js` 的
 * `slotActive()` 逐字同序。
 */
export function rasterizeChannel(segments, slotCount, slotS) {
    const pan = new Array(slotCount).fill(0);
    const activity = new Array(Math.ceil(slotCount / 32)).fill(0);
    const segs = (segments || [])
        .filter((s) => s && Number.isFinite(s.t0S) && s.t1S > s.t0S)
        .slice()
        .sort((a, b) => a.t0S - b.t0S);
    let si = 0;
    for (let i = 0; i < slotCount; i++) {
        const t = (i + 0.5) * slotS;
        while (si < segs.length && segs[si].t1S <= t) si++;
        const seg = segs[si];
        if (!seg || seg.t0S > t) continue;
        pan[i] = seg.pan;
        // `|= 1 << bit` 在 bit === 31 时得到负数(JS 位运算走 int32)。>>> 0 折回
        // 无符号 —— 载荷里出现一个负数「u32」会让 C++ 侧对表时白查半天。
        activity[i >>> 5] = (activity[i >>> 5] | (1 << (i & 31))) >>> 0;
    }
    return { pan, activity };
}

/** 段表里覆盖住 `tS` 的那一段(分布图的「实时」值取它;没有就返回 null)。 */
function segmentAt(segments, tS) {
    for (const s of segments || []) {
        if (s && s.t0S <= tS && tS < s.t1S) return s;
    }
    return null;
}

/**
 * 造一个组的世界:轨集 + 每轨的栅格 + 画像。
 * 栅格只算一次(15 轨 × 600 格),之后每一帧 viz 只是把它和当前播放头拼起来。
 */
function buildGroup(world, groupId) {
    const tracks = GROUP_TRACKS[groupId] || null;
    if (!tracks) return { online: false, channels: [] };
    const durationS = world.durationS || DEMO_DURATION_S;
    const slotS = durationS / SLOT_COUNT;
    const segChannels = (world.output.segments || {}).channels || [];
    const cfgChannels = (world.output.snapshot || {}).channels || [];

    const channels = tracks.map((ch) => {
        const entry = segChannels.find((c) => c.ch === ch);
        const cfg = cfgChannels[ch - 1] || {};
        const segs = (entry && entry.segments) || [];
        const grid = rasterizeChannel(segs, SLOT_COUNT, slotS);
        return {
            ch,
            label: String(cfg.label || ""),
            stereo: cfg.source_channels === 2,
            // 轨色索引默认 = 轨号(J75 C 的「轨色索引」字段;独立成字段的理由见 viz.js)
            colorIndex: ch,
            lead: !!cfg.lead_lock,
            segs,
            grid,
        };
    });
    return { online: true, channels, slotS, durationS };
}

// -----------------------------------------------------------------------------
// 3. mock 后端
// -----------------------------------------------------------------------------

/**
 * @param {{scenario:string, group:number|null, play:boolean}} parsed
 */
function createMonitorBackend(parsed) {
    const world = buildWorld({ role: "output", fixture: "fifteen-tracks" });
    const durationS = world.durationS || DEMO_DURATION_S;

    // 场景 → 初始观察组与在线位图
    let groupsOnline = DEMO_GROUPS_ONLINE; // A/B/E
    let observed = 1;
    if (parsed.scenario === "monitor-groups") observed = 2; // 开箱停在 B(6 轨)
    if (parsed.scenario === "monitor-offline") observed = 3; // C:没有 Output
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
        seq: 0,
        // `monitor-stalled` 场景:seq 冻在这里不动 ⇒ 消费侧 3 秒后挂琥珀横幅
        frozen: parsed.scenario === "monitor-stalled",
        scale: 1,
        language: "zh",
        committedScale: 1,
    };

    const listeners = new Map();
    let ready = false;
    const onReadyQueue = [];

    function emit(name, payload) {
        for (const cb of listeners.get(name) || []) cb(payload);
    }

    /**
     * 造一帧 viz。
     *
     * 组不在线时**仍然发一帧**(`online:false`)—— 「没有事件」与「有事件说没在线」
     * 在 UI 侧是两回事:前者分不清是离线还是桥断了,后者能当场落到空态。
     */
    function vizFrame(groupId, tS) {
        const g = groupOf(groupId);
        const base = {
            magic: VIZ_MAGIC,
            abi: VIZ_ABI,
            seq: state.seq,
            groupId,
            durationS,
            slotCount: SLOT_COUNT,
            slotS: durationS / SLOT_COUNT,
            playheadS: Math.round(tS * 1000) / 1000,
        };
        if (!g.online) return { ...base, online: false, channels: [] };
        return {
            ...base,
            online: true,
            channels: g.channels.map((c) => {
                // 分布图的「实时」三值取**播放头所在那一段**的打印值;播放头落在段间
                // 空当时保留最近一次的段值(引擎在段间走 ramp,不会瞬间归零)。
                const seg = segmentAt(c.segs, tS) || lastSegBefore(c.segs, tS);
                return {
                    ch: c.ch,
                    label: c.label,
                    stereo: c.stereo,
                    colorIndex: c.colorIndex,
                    lead: c.lead,
                    panNow: seg ? seg.pan : 0,
                    volDb: seg ? seg.volDb : -24,
                    widthPct: c.stereo ? 82 : 100,
                    pan: c.grid.pan,
                    activity: c.grid.activity,
                };
            }),
        };
    }

    function lastSegBefore(segs, tS) {
        let best = null;
        for (const s of segs || []) {
            if (s.t1S <= tS && (!best || s.t1S > best.t1S)) best = s;
        }
        return best;
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
                abi: VIZ_ABI,
                version: "0.1.0",
                groupId: state.observed,
                groups_online: groupsOnline,
                ui: { scale: state.scale, language: state.language },
                // 首帧 viz 随快照一起给 —— 与契约 §0.4「状态类各必发一次」同精神:
                // 没有它,页面要空等一整个发布周期才出图。
                viz: vizFrame(state.observed, state.tS),
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
            state.seq += 1;
            emit("scvb.viz", vizFrame(g, state.tS));
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
        groupsPayload: () => makeGroups(groupsOnline),
        setGroupsOnline(bitmap) {
            groupsOnline = bitmap;
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
     * 30Hz 档专用的帧循环 —— 理由同 state-driver.js:Windows 的定时器分辨率
     * ~15.6ms 会把 `setInterval(33)` 抬到 ~46ms(实测 21.4Hz),达不到 §2.6 的 30Hz。
     * 浏览器用 rAF + 时间累加器;node 无 rAF 时回落 setInterval。
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
                ctl.emit("scvb.groups", ctl.groupsPayload());
                ctl.emit(
                    "scvb.viz",
                    ctl.vizFrame(ctl.state.observed, ctl.state.tS),
                );
            });

            // 30Hz:播放头(§2.6 载荷原样)
            startFrameLoop(PLAYHEAD_PERIOD_MS, () => {
                const s = ctl.state;
                if (s.isPlaying) {
                    s.tS += PLAYHEAD_PERIOD_MS / 1000;
                    if (s.tS >= ctl.world.durationS)
                        s.tS -= ctl.world.durationS;
                }
                ctl.emit(
                    "scvb.playhead",
                    makePlayhead(s.tS, { isPlaying: s.isPlaying }),
                );
            });

            // 4Hz:viz 低频快照(J75 C)。`monitor-stalled` 场景下 seq 不前进,
            // 消费侧据此在 3 秒后挂琥珀横幅 —— 事件照发,冻住的是序号。
            timers.push(
                setInterval(() => {
                    const s = ctl.state;
                    if (!s.frozen) s.seq += 1;
                    ctl.emit("scvb.viz", ctl.vizFrame(s.observed, s.tS));
                }, VIZ_PERIOD_MS),
            );

            // 1Hz:组在线位图
            timers.push(
                setInterval(() => {
                    ctl.emit("scvb.groups", ctl.groupsPayload());
                }, GROUPS_PERIOD_MS),
            );

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
        } else
            warnings.push(
                `场景 ${opts.scenario} 不存在,已按 ${parsed.scenario}`,
            );
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
