// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 场景/fixture 驱动(T28 交付)
// =============================================================================
// 职责三件:
//   ① **组装 fixture 初始状态**(六个 fixture,数据一律经 web/shared/mock-data.js
//      的生成器 + overrides 产出,本文件不自造载荷形状);
//   ② **解析 `?fixture=` / `?scenario=` / `?loop=` / `?role=` / `?play=`**,
//      未实现的 05 §2.5 场景名一律回落 `fifteen-tracks` + console.warn(**不假装支持**);
//   ③ **驱动周期性事件**:meters 30Hz / playhead 30Hz / params 25Hz / conn 4Hz /
//      config 4Hz(变化才发)/ captureProgress 播放中 2Hz / groups 1Hz —— 频率照契约 §2/§4。
//
// 壳页(web-preview/output.html、input.html)的唯一入口:
//
//   import { createPreviewSession } from "./mock/state-driver.js";
//   const session = createPreviewSession({ role: "output", params: location.search });
//   targetWindow.__SCVB_MOCK__ = session.mock;   // 必须在真源页面的 app.js 求值之前
//   session.start();                             // 起周期事件
//   // session.info = { fixture, scenario, loop, warnings[] } —— 供导航页/角标展示
//
// 本模块**没有 import 副作用**:不自动挂 window、不自动起定时器。挂载时机由壳页掌握
// (灰模 app.js 在模块顶层就读 `window.__SCVB_MOCK__`,晚一步就接不上)。
// 想要「当前窗口一把梭」的写法,用下面的 `installMock()`。
//
// 依赖方向:web-preview/ → web/(单向,06 §6.2)。本文件不碰 DOM 结构、不复制任何 UI 代码。
// =============================================================================

import {
    createMockBackend,
    makeDefaultParams,
    maskOfChannels,
} from "./juce-bridge-mock.js";
import {
    CHANNEL_COUNT,
    DEMO_DURATION_S,
    DEMO_GROUPS_ONLINE,
    DEMO_LABELS,
    DEMO_STEREO_CHANNELS,
    FIFTEEN_TRACKS,
    METER_FLOOR_DB,
    makeCaptureProgress,
    makeError,
    makeGroups,
    makeInputSnapshot,
    makeMeters,
    makeOutputSnapshot,
    makeParams,
    makePlayhead,
    makeSegments,
    makeTourDemoSegments,
    makeTourDemoSnapshot,
} from "../../web/shared/mock-data.js";

// -----------------------------------------------------------------------------
// 0. 常量
// -----------------------------------------------------------------------------

/**
 * 六个 fixture(任务卡硬约束 3)。
 * 注意:J59 废除的那个 **10 轨口径的旧 fixture 名**不得在本仓任何位置出现 —— 连注释、
 * 连「已废除」这样的说明文字都算命中,因为任务卡的验收是一条 `grep -rn` 全仓零命中断言,
 * 把它写出来解释它自己就会把断言打红。要指代它,一律说「10 轨口径的旧 fixture 名」。
 */
export const FIXTURES = Object.freeze([
    "empty",
    "fifteen-tracks",
    "misaligned",
    "channel-conflict",
    "second-output",
    "stereo-mixed",
]);

export const DEFAULT_FIXTURE = "fifteen-tracks";

/**
 * `?scenario=`(05 §2.5 / §3 的场景名)→ 本卡 fixture 的**已实现**映射。
 * 表外的场景名一律回落 `fifteen-tracks` 并 warn「待 T31-T36 接线」——
 * 那些场景要的是 UI 侧的横幅/overlay/tour,本卡的 mock 后端给不出可验收的东西,
 * 假装支持只会让后续 agent 以为已经有了。
 */
export const SCENARIO_MAP = Object.freeze({
    empty: "empty",
    connected: "fifteen-tracks",
    misaligned: "misaligned",
    conflict: "channel-conflict",
    occupied: "channel-conflict",
    "group-switch": "second-output",
    "no-output": "fifteen-tracks",
    // T36 接线五档(Input 七态中的 no-output / passthrough / abi-mismatch /
    // sr-mismatch / group-mismatch):落在健康满配世界上,由 buildWorld 的场景覆写改 Input 快照初值。
    passthrough: "fifteen-tracks",
    "abi-mismatch": "fifteen-tracks",
    "sr-mismatch": "fifteen-tracks",
    "group-mismatch": "fifteen-tracks",
    // T31 接线两档:落在健康满配世界上,由 buildWorld 的场景覆写改快照初值
    // (print_guard.pending / ui.guide_seen),否则加载守卫与引导页在浏览器不可达。
    "print-guard": "fifteen-tracks",
    "first-run": "fifteen-tracks",
    // T33 接线:布防态落在健康满配世界上,由 buildWorld 场景覆写改快照初值
    // (state.recapture 按契约 §9.2 形状回读,Tab3 三处 badge 的数据源)
    "recapture-armed": "fifteen-tracks",
    // T36b 首启交互式引导:完整首启链(语言卡 → 红字九条 → 询问步 → tour 43 步);见 buildWorld 覆写
    "first-run-tour": "fifteen-tracks",
    // T34 曲线编辑器演示:非零 ms_balance,让 J68 叠加线(g_eq)在截图里可见
    "curve-editor": "fifteen-tracks",
});

/** 宿主循环区(`daw_loop` 档的来源;`?loop=none` 时视为宿主根本不提供)。 */
const HOST_LOOP = Object.freeze({ startS: 24, endS: 96 });

/** `stereo-mixed` 的手动区间(第三个枚举值 `manual` 的代表档)。 */
const MANUAL_RANGE = Object.freeze({ startS: 12, endS: 96 });

/** `recapture-armed` 的布防面(轨 3/4/7/12 × 选区 78-114s;autoStop 默认 false)。 */
const RECAPTURE_DEMO = Object.freeze({
    channels: Object.freeze([3, 4, 7, 12]),
    startS: 78,
    endS: 114,
});

/** `misaligned` 的失准轨与计数(琥珀横幅① + Tab2 该轨 ⚠ 计数的数据源)。 */
const MISALIGN_COUNTS = Object.freeze({ 3: 4, 7: 1, 11: 9 });

/** `channel-conflict` 里被别的实例占住、用户又点了的通道号。 */
const CONFLICT_CHANNEL = 4;

/** 全 15 通道占用位图(u16,bit0=ch1 … bit14=ch15)。 */
const ALL_CHANNELS_MASK = maskOfChannels(
    Array.from({ length: CHANNEL_COUNT }, (_, i) => i + 1),
);

/** 事件周期(契约 §2/§4 逐类标注;ms)。 */
const PERIOD = Object.freeze({
    frame30Hz: 33, // scvb.meters / scvb.playhead
    params25Hz: 40, // scvb.params
    conn4Hz: 250, // scvb.conn / scvb.config
    capture2Hz: 500, // scvb.captureProgress(仅播放中)
    groups1Hz: 1000, // scvb.groups
});

/**
 * 兜底代调 `requestInitialState()` 的等待时长(ms)。
 * T27b 灰模把 `requestInitialState()` 留成 `[T31]` 注释桩,页面自己不会调 ——
 * 而契约 §0.6 规定调用前一个事件都不许推,于是预览页会静默停在空态。
 * driver 等这么久还没等到,就自己代调一次并留一条 console.info(**只在预览侧兜底,
 * 绝不改 web/**)。T31 接线后页面会先调,这条兜底自然不触发。
 */
const AUTO_REQUEST_INITIAL_STATE_MS = 1500;

// -----------------------------------------------------------------------------
// 1. 查询参数解析
// -----------------------------------------------------------------------------

/**
 * 自有键判定 —— 查询串是外部输入,`SCENARIO_MAP[q.get("scenario")]` 这种写法会让
 * `?scenario=constructor` 从原型链上取到一个真值,当场把它当成「已实现的场景名」。
 */
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
 * 解析预览参数。
 * @returns {{fixture:string, scenario:string|null, loop:"host"|"none"|null,
 *            play:boolean|null, role:string|null, warnings:string[]}}
 */
export function parsePreviewQuery(params) {
    const q = toSearchParams(params);
    const warnings = [];

    const rawFixture = q.get("fixture");
    const rawScenario = q.get("scenario");
    const fixtureOk = !!rawFixture && FIXTURES.includes(rawFixture);
    const scenarioFixture =
        rawScenario && hasOwn(SCENARIO_MAP, rawScenario)
            ? SCENARIO_MAP[rawScenario]
            : null;

    // 优先级:合法 fixture > 已实现 scenario 的映射 > 默认档。
    // 非法 fixture **不**吃掉同时给出的 scenario —— 拼错一个参数就把另一个参数
    // 一起丢进默认档,拿到的是「看起来像但不是你要的那档」,比直接回默认档更难发现。
    let fixture = DEFAULT_FIXTURE;
    if (fixtureOk) fixture = rawFixture;
    else if (scenarioFixture) fixture = scenarioFixture;

    if (rawFixture && !fixtureOk) {
        warnings.push(
            `fixture ${rawFixture} 不存在(六个:${FIXTURES.join(" / ")}),已回落 ${fixture}`,
        );
    }
    if (rawScenario && !scenarioFixture) {
        warnings.push(`场景 ${rawScenario} 待 T31-T36 接线`);
    }

    const rawLoop = q.get("loop");
    let loop = null;
    if (rawLoop === "none" || rawLoop === "host") {
        loop = rawLoop;
    } else if (rawLoop) {
        warnings.push(
            `loop=${rawLoop} 未知(只认 host / none),已按 fixture 默认档`,
        );
    }

    const rawPlay = q.get("play");
    const play =
        rawPlay === null ? null : rawPlay !== "0" && rawPlay !== "false";

    return {
        fixture,
        scenario: rawScenario,
        loop,
        play,
        role: q.get("role"),
        warnings,
    };
}

// -----------------------------------------------------------------------------
// 2. fixture —— 六个初始世界(数据一律 mock-data 生成器 + overrides)
// -----------------------------------------------------------------------------

/**
 * Input 侧「已连上、正常工作」的一份快照。
 * **claim 不进快照**:§3.1 的字段集里没有它(它只在 §4.1 `scvb.state` 出现),
 * 所以 claim 由 `world.input.claim` 单独给,mock 侧也单独存 —— 混进快照就会让
 * `requestInitialState()` 多回一个契约没登记的键。
 * @param {number} ch 本实例占的通道号
 */
function connectedInputSnapshot(ch, extraConfig = {}) {
    // 远程只读摘要的 lead/pair/priority 取 FIFTEEN_TRACKS 该轨画像(主唱=lead+pair1),
    // 否则远程摘要行永远无内容,连「有内容时正常显示」都验证不到。
    const profile = FIFTEEN_TRACKS.snapshot.channels[ch - 1] || {};
    return makeInputSnapshot({
        channel_id: ch,
        group_id: 1,
        conn: {
            outputOnline: true,
            maskBit: true,
            capturing: true,
            passthrough: false, // 已接管:本轨静音转发(J12/J32)
            passthroughPending: false,
            occupiedMask: ALL_CHANNELS_MASK,
        },
        config: {
            label: DEMO_LABELS[ch - 1],
            priority: profile.priority ?? 5,
            lead_lock: !!profile.lead_lock,
            pair_id: profile.pair_id ?? 0,
            config_seq: 42,
            channelLabels: DEMO_LABELS.slice(),
            ...extraConfig,
        },
        ui: { scale: 1, language: "zh" },
    });
}

/** 空段表:容器与元素形状照生成器,只把每轨的 segments 清空(空工程没有分析产物)。 */
function emptySegmentsFrame(version = 1) {
    const base = makeSegments(version, "snapshot");
    return {
        ...base,
        channels: base.channels.map((entry) => ({ ...entry, segments: [] })),
        diff: { kept: 0, changed: [], added: 0, removed: 0 },
    };
}

/**
 * 每轨覆盖率基准(供 `clearCoverage` 按「清掉多少」往下扣)。
 * 直接取 FIFTEEN_TRACKS 的首帧覆盖(生成器口径),不另算一份;
 * `empty` fixture 没采过任何东西,基准一律 0 —— 否则清除操作会从 84-92% 往下扣,
 * 而页面上那 15 条覆盖条本来就是空的,数字与画面对不上。
 */
function demoCoverage(fixture) {
    return FIFTEEN_TRACKS.captureProgress.channels.map((c) => ({
        ch: c.ch,
        coveragePct: fixture === "empty" ? 0 : c.coveragePct,
    }));
}

/**
 * 组装一个 fixture 的初始世界。
 * @param {{role:string, fixture:string, loop:"host"|"none"|null, play:boolean|null,
 *          scenario?:string|null}} opts
 */
export function buildWorld(opts = {}) {
    const fixture = FIXTURES.includes(opts.fixture)
        ? opts.fixture
        : DEFAULT_FIXTURE;

    // ---- 默认值(健康满配 15 轨)-------------------------------------------------
    const caps = {
        readOnly: false,
        loopAvailable: true,
        loop: { ...HOST_LOOP },
        occupiedMask: ALL_CHANNELS_MASK,
        groupConflict: false,
        ringFull: false,
        noTimeline: false,
    };
    const errors = { output: [], input: [] };
    let transport = { timeS: 42, isPlaying: true };
    let groupsOnline = DEMO_GROUPS_ONLINE;
    let outputSnapshot = null;
    let outputParams = null;
    let outputSegments = null;
    let inputSnapshot = null;
    let inputClaim = "active"; // §5.2 六态;不属 §3.1 快照字段集,单独给
    let inputAbiRemote; // §4.1 abi_remote;探测不到对端 = undefined(字段不存在)

    if (fixture === "empty") {
        // 0 轨连接、无 coverage、guide_seen=true(不弹引导)、range 默认 follow。
        outputSnapshot = makeOutputSnapshot({
            ui: { guide_seen: true },
            guide_seen_global: true,
        });
        outputParams = makeDefaultParams(1);
        outputSegments = emptySegmentsFrame(1);
        inputSnapshot = makeInputSnapshot(); // channel_id=0(未分配)/ Output 离线 / 直通
        inputClaim = "unassigned"; // 引导态,非错误(§5.2)
        caps.loopAvailable = false; // 没有时间线内容,宿主循环区也无从谈起
        caps.occupiedMask = 0;
        transport = { timeS: 0, isPlaying: false };
        groupsOnline = makeGroups(0b00000001).groups_online; // 只有本组 A 在线
    } else if (fixture === "misaligned") {
        // fifteen-tracks 基础上给 2-3 轨注入 misalignCount>0。
        // 失准**没有独立 error code** —— 琥珀横幅①由 UI 直接按 conn 渲染(05 §2.0),
        // 所以这里只改 conn,不发 scvb.error。
        const base = makeTourDemoSnapshot();
        const channels = base.conn.channels.map((c, i) => {
            const n = MISALIGN_COUNTS[i + 1];
            return n ? { ...c, misalignCount: n } : c;
        });
        outputSnapshot = makeTourDemoSnapshot({
            global: { range: { mode: "follow" } },
            conn: { channels },
        });
        outputParams = makeParams({ versionActive: 1 });
        outputSegments = makeTourDemoSegments(1, "snapshot");
        inputSnapshot = connectedInputSnapshot(1);
    } else if (fixture === "channel-conflict") {
        // Input 侧场景:claim=conflict + channelConflict error + occupiedMask 含目标位。
        // Output 侧**不受影响**(照 fifteen-tracks 健康档)。
        outputSnapshot = makeTourDemoSnapshot({
            global: { range: { mode: "follow" } },
        });
        outputParams = makeParams({ versionActive: 1 });
        outputSegments = makeTourDemoSegments(1, "snapshot");
        inputClaim = "conflict";
        inputSnapshot = makeInputSnapshot({
            channel_id: 0, // CAS 失败 ⇒ 没占住任何 slot
            conn: {
                outputOnline: true,
                maskBit: false,
                passthrough: true, // 拒连不影响音频直通
                occupiedMask: ALL_CHANNELS_MASK,
            },
            config: {
                config_seq: 42,
                channelLabels: DEMO_LABELS.slice(),
            },
        });
        errors.input.push(
            makeError("channelConflict", {
                ch: CONFLICT_CHANNEL,
                detail: { groupId: 1 },
            }),
        );
    } else if (fixture === "second-output") {
        // 只读观察 + secondOutput 横幅②;range 取 daw_loop(宿主提供 loop)= daw_loop 代表档。
        outputSnapshot = makeTourDemoSnapshot({
            global: {
                range: {
                    mode: "daw_loop",
                    start_s: HOST_LOOP.startS,
                    end_s: HOST_LOOP.endS,
                },
            },
            conn: { outputReadOnly: true },
        });
        outputParams = makeParams({ versionActive: 1 });
        outputSegments = makeTourDemoSegments(1, "snapshot");
        inputSnapshot = connectedInputSnapshot(1); // Input 视角正常
        caps.readOnly = true;
        errors.output.push(
            makeError("secondOutput", { detail: { groupId: 1 } }),
        );
    } else if (fixture === "stereo-mixed") {
        // mono+stereo 混存:四条 stereo 轨(生成器口径 DEMO_STEREO_CHANNELS)带 ST 标、
        // participate_in_auto_pan 默认 false、每轨 width 旋钮可用;range 取 manual
        // (与 empty/fifteen-tracks 的 follow、second-output 的 daw_loop 一起凑满三值枚举)。
        outputSnapshot = makeTourDemoSnapshot({
            global: {
                range: {
                    mode: "manual",
                    start_s: MANUAL_RANGE.startS,
                    end_s: MANUAL_RANGE.endS,
                },
            },
        });
        outputParams = makeParams({ versionActive: 1 });
        outputSegments = makeTourDemoSegments(1, "snapshot");
        const stereoCh = DEMO_STEREO_CHANNELS[0];
        inputSnapshot = connectedInputSnapshot(stereoCh, {
            source_channels: 2,
            participate_in_auto_pan: false, // J60:stereo 默认不参与自动声像
        });
    } else {
        // fifteen-tracks:15 轨全连、4 stereo、覆盖/段表齐全;**range.mode=follow 默认档代表**。
        outputSnapshot = makeTourDemoSnapshot({
            global: { range: { mode: "follow" } },
        });
        outputParams = makeParams({ versionActive: 1 });
        outputSegments = makeTourDemoSegments(1, "snapshot");
        inputSnapshot = connectedInputSnapshot(1);
    }

    // ---- 场景覆写(SCENARIO_MAP 已把这两个名字映射到 fifteen-tracks)-----------
    // 只改快照初值,不动周期事件与函数语义;字段形状照 mock-data 生成器原样。
    if (opts.scenario === "print-guard" && outputSnapshot) {
        // 05 §2.0 横幅⑦:工程刚加载、上次退出时输出仍为 ON ⇒ 守卫待确认。
        // 走带停在 0(守卫场景=刚打开工程,确认前只允许 ARMED)。
        outputSnapshot = {
            ...outputSnapshot,
            print_guard: { ...outputSnapshot.print_guard, pending: true },
        };
        transport = { timeS: 0, isPlaying: false };
    }
    if (opts.scenario === "recapture-armed" && outputSnapshot) {
        // 05 §2.3「重采集选区」行:armed 后三处 badge + footer 警告的可验收世界。
        // 快照直接带 armed 态(= 用户在上一拍点过布防;切 tab/重开面板靠
        // scvb.state.recapture 恢复显示,契约 §9.2:只读回读**无 reason**)。
        // **输出开关同时 ON**:B-04 的 footer 警告判据是「输出开关 ON 或布防期
        // 被打开」—— 基线 output_enabled:false 时这条警告按判据就**不该**挂,
        // 场景开箱跑不通验收锚④(对抗校验 minor)。故本场景连输出一起摆开。
        outputSnapshot = {
            ...outputSnapshot,
            global: { ...outputSnapshot.global, output_enabled: true },
            recapture: {
                armed: true,
                tracksMask: maskOfChannels(RECAPTURE_DEMO.channels.slice()),
                startS: RECAPTURE_DEMO.startS,
                endS: RECAPTURE_DEMO.endS,
                autoStop: false,
            },
        };
    }
    if (opts.scenario === "first-run" && outputSnapshot) {
        // 05 §2.5 first-run:两级 guide_seen 全 false ⇒ 引导页 overlay 弹出。
        outputSnapshot = {
            ...outputSnapshot,
            ui: { ...outputSnapshot.ui, guide_seen: false },
            guide_seen_global: false,
        };
    }
    if (opts.scenario === "first-run-tour" && outputSnapshot) {
        // 05 §2.6 首启顺序固定 = 语言卡 → 红字九条页 → 询问步 → tour;场景必须复现完整链条,
        // 故两级 guide_seen 与 tour_seen 全 false(与 first-run 同款 + 显式 tour 位):
        // 语言卡弹出 → 选语言 → 红字页「开始使用」→ 询问步「开始引导」→ 43 步 → 完成落设置页。
        outputSnapshot = {
            ...outputSnapshot,
            ui: { ...outputSnapshot.ui, guide_seen: false, tour_seen: false },
            guide_seen_global: false,
            tour_seen_global: false,
        };
    }
    if (opts.scenario === "curve-editor" && outputParams) {
        // T34:MS 等效增益叠加线演示 —— 非零 ms_balance 使 g_eq 曲线偏离 0 dB 可见。
        outputParams = {
            ...outputParams,
            values: { ...outputParams.values, ms_balance: 42 },
        };
    }

    // ---- Input 七态场景覆写(T36;只改 Input 快照初值,不动周期事件与函数语义)----
    // 字段形状照 mock-data 生成器原样;claim/abi_remote 不属 §3.1 快照字段集,单独给。
    if (opts.scenario === "no-output") {
        // 灰「Output 未运行」:通道已选但本组 Output 离线;groups_online 全 0 也不报错(J70)。
        inputSnapshot = makeInputSnapshot({
            channel_id: 1,
            group_id: 1,
            conn: {
                outputOnline: false,
                maskBit: false,
                passthrough: true,
                occupiedMask: 1, // 本实例占 ch1
            },
            // Output 离线 → 无广播区 → channelLabels 留空(与「通道表为空」语义一致)
            config: { config_seq: 42 },
        });
        inputClaim = "idle";
        groupsOnline = 0;
        caps.occupiedMask = 0; // 本组无其它 Input,只有本实例占 ch1
    } else if (opts.scenario === "passthrough") {
        // 直通副文案:Output 在线但本 channel 未被健康读取 → 等待 Output + 直通中。
        inputSnapshot = makeInputSnapshot({
            channel_id: 1,
            group_id: 1,
            conn: {
                outputOnline: true,
                maskBit: false,
                passthrough: true,
                passthroughPending: false,
                occupiedMask: ALL_CHANNELS_MASK,
            },
            config: { config_seq: 42, channelLabels: DEMO_LABELS.slice() },
        });
        inputClaim = "idle";
    } else if (opts.scenario === "abi-mismatch") {
        // 红 pill「版本不匹配」+ banner.abiMismatch(两端 abi 数字)+ 直通副文案照常。
        inputSnapshot = makeInputSnapshot({
            channel_id: 1,
            group_id: 1,
            conn: {
                outputOnline: true,
                maskBit: false,
                passthrough: true,
                occupiedMask: ALL_CHANNELS_MASK,
            },
            config: { config_seq: 42, channelLabels: DEMO_LABELS.slice() },
        });
        inputClaim = "abiMismatch";
        inputAbiRemote = 2; // 对端 Output abi(本机 = LOCAL_ABI = 1)
    } else if (opts.scenario === "sr-mismatch") {
        // 红 pill「采样率不一致」+ 直通副文案照常。
        inputSnapshot = makeInputSnapshot({
            channel_id: 1,
            group_id: 1,
            conn: {
                outputOnline: true,
                maskBit: false,
                passthrough: true,
                occupiedMask: ALL_CHANNELS_MASK,
            },
            config: { config_seq: 42, channelLabels: DEMO_LABELS.slice() },
        });
        inputClaim = "srMismatch";
        errors.input.push(
            makeError("srMismatch", {
                ch: 1,
                detail: { inputSr: 44100, outputSr: 48000 },
            }),
        );
    } else if (opts.scenario === "group-mismatch") {
        // 本组(B)无 Output 但异组(A)在线 → pill「等待 Output · 组 B」+ group.noOutput。
        inputSnapshot = makeInputSnapshot({
            channel_id: 1,
            group_id: 2, // 非默认组 B
            conn: {
                outputOnline: false,
                maskBit: false,
                passthrough: true,
                occupiedMask: 1, // 本实例占 ch1
            },
            // 本组无 Output → 无广播区 → channelLabels 留空(与「通道表为空」语义一致)
            config: { config_seq: 42 },
        });
        inputClaim = "idle";
        groupsOnline = 0b00000001; // 只有组 A 在线(异组),本组 B 无 Output
        caps.occupiedMask = 0; // 本组(B)无其它 Input
    }

    // ---- 查询参数覆写 ----------------------------------------------------------
    if (opts.loop === "none") caps.loopAvailable = false;
    if (opts.loop === "host") caps.loopAvailable = true;
    if (typeof opts.play === "boolean") transport.isPlaying = opts.play;

    // 本实例已占的通道从「他人占用」位图剔除(§4.2 含自己的位;否则释放后重选原通道
    // 会被误判为他占 → conflict)。channel_id=0(未分配)时无需剔除。
    if (inputSnapshot && inputSnapshot.channel_id >= 1) {
        caps.occupiedMask &= ~(1 << (inputSnapshot.channel_id - 1));
    }

    return {
        fixture,
        durationS: DEMO_DURATION_S,
        caps,
        transport,
        groupsOnline,
        errors,
        output: {
            snapshot: outputSnapshot,
            params: outputParams,
            segments: outputSegments,
            coverage: demoCoverage(fixture),
        },
        input: {
            snapshot: inputSnapshot,
            claim: inputClaim,
            abiRemote: inputAbiRemote,
        },
    };
}

// -----------------------------------------------------------------------------
// 3. 周期事件驱动
// -----------------------------------------------------------------------------

function allChannels() {
    return Array.from({ length: CHANNEL_COUNT }, (_, i) => i + 1);
}

/** 全轨静音的一帧 meters(未连接/停止播放:契约 §2.5 地板 -60 dB)。 */
function floorMeters(tS) {
    const floor = { db: METER_FLOOR_DB, peakDb: METER_FLOOR_DB };
    return makeMeters(tS, {
        tracks: allChannels().map(() => ({ ...floor })),
        bus: { l: { ...floor }, r: { ...floor } },
    });
}

function makeDriver(ctl, world) {
    const timers = [];
    const frameLoops = [];
    let running = false;
    let autoReqId = null;
    let tS = world.transport.timeS;

    /**
     * 30Hz 档专用的帧循环。
     * **为什么不用 `setInterval(33)`**:Windows 的默认定时器分辨率是 ~15.6ms,
     * `setInterval(33)` 会被向上取整到 ~46ms —— 实测(node 22 / Win11)只有 21.4Hz,
     * 契约 §2.5/§2.6 的 30Hz 直接不达标。浏览器前台页面有 rAF(通常 60Hz vsync),
     * 用「rAF + 时间累加器」既能稳稳落在 30Hz,又能在标签页隐藏时自动停(省电,
     * 且隐藏期间本来也没人看电平)。node 无 rAF 时回落 setInterval,精度受平台限制。
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
        const id = setInterval(fn, periodMs);
        timers.push(id);
    }

    function everyOutput() {
        // 30Hz:走带 + meters + playhead
        startFrameLoop(PERIOD.frame30Hz, () => {
            const playing = ctl.model.transport.isPlaying;
            if (playing) {
                tS += PERIOD.frame30Hz / 1000;
                if (tS >= world.durationS) tS -= world.durationS;
            }
            ctl.setTransport({ timeS: tS, isPlaying: playing });
            const connected = ctl.connectedChannels();
            ctl.emit(
                "scvb.meters",
                playing && connected.length > 0
                    ? makeMeters(tS)
                    : floorMeters(tS),
            );
            ctl.emit(
                "scvb.playhead",
                makePlayhead(tS, {
                    isPlaying: playing,
                    ...ctl.playheadOverrides(tS),
                }),
            );
        });

        // 25Hz:params —— 只有 PRINT 态的引擎打印头会动值,且**值未变不发**(§0.4/§0.5)
        timers.push(
            setInterval(() => {
                const diff = ctl.printedParamsDiff();
                if (diff) ctl.emit("scvb.params", diff);
            }, PERIOD.params25Hz),
        );

        // ~4Hz:conn(diff-then-emit)
        timers.push(
            setInterval(() => {
                ctl.emitIfChanged("scvb.conn", ctl.connPayload());
            }, PERIOD.conn4Hz),
        );

        // 播放中 2Hz:captureProgress(非播放不发;本帧无新增覆盖的轨不进 channels)
        timers.push(
            setInterval(() => {
                const s = ctl.model;
                if (!s.transport.isPlaying) return;
                if (!s.snapshot.global.capture_enabled) return;
                const connected = ctl.connectedChannels();
                if (connected.length === 0) return;
                const frame = makeCaptureProgress(tS, connected);
                if (frame.channels.length === 0) return;
                ctl.emit("scvb.captureProgress", frame);
            }, PERIOD.capture2Hz),
        );

        // 1Hz:groups(变化才发)
        timers.push(
            setInterval(() => {
                ctl.emitIfChanged("scvb.groups", ctl.groupsPayload());
            }, PERIOD.groups1Hz),
        );
    }

    function everyInput() {
        // ~4Hz:conn + config(config 契约写 25Hz 轮询、节流到变化才发,
        // mock 侧以 4Hz 轮询做同一件事 —— 变化才发的行为等价,少烧 21 次/秒空转)
        timers.push(
            setInterval(() => {
                ctl.emitIfChanged("scvb.conn", ctl.connPayload());
                ctl.emitIfChanged("scvb.config", ctl.configPayload());
            }, PERIOD.conn4Hz),
        );
        timers.push(
            setInterval(() => {
                ctl.emitIfChanged("scvb.groups", ctl.groupsPayload());
            }, PERIOD.groups1Hz),
        );
    }

    /** §0.4 第 3 条:mBridgeReady 后状态类各必发一次;条件类只在条件成立时发。 */
    function firstFrames() {
        ctl.emit("scvb.state", ctl.fullStatePayload());
        if (ctl.role === "output") {
            ctl.emitIfChanged("scvb.params", ctl.paramsFullPayload());
            ctl.emitIfChanged("scvb.conn", ctl.connPayload());
            ctl.emitIfChanged("scvb.groups", ctl.groupsPayload());
            ctl.emit("scvb.meters", floorMeters(tS));
            ctl.emit(
                "scvb.playhead",
                makePlayhead(tS, {
                    isPlaying: ctl.model.transport.isPlaying,
                    ...ctl.playheadOverrides(tS),
                }),
            );
            ctl.emit(
                "scvb.segments",
                ctl.segmentsPayload("snapshot", allChannels()),
            );
        } else {
            ctl.emitIfChanged("scvb.conn", ctl.connPayload());
            ctl.emitIfChanged("scvb.config", ctl.configPayload());
            ctl.emitIfChanged("scvb.groups", ctl.groupsPayload());
        }
        for (const err of ctl.pendingErrors()) ctl.emit("scvb.error", err);
    }

    return {
        get timeS() {
            return tS;
        },
        start(backend) {
            if (running) return;
            running = true;
            ctl.onReady(firstFrames);
            if (ctl.role === "output") everyOutput();
            else everyInput();
            // 兜底代调 requestInitialState()(见常量处注释)
            autoReqId = setTimeout(() => {
                autoReqId = null;
                if (ctl.isReady()) return;
                console.info(
                    "SCVB web-preview:页面未调用 requestInitialState()" +
                        "(T27b 灰模把它留成注释桩,属正常),driver 代调一次以开启事件流。",
                );
                backend.requestInitialState();
            }, AUTO_REQUEST_INITIAL_STATE_MS);
        },
        stop() {
            running = false;
            for (const cancel of frameLoops) cancel();
            frameLoops.length = 0;
            for (const id of timers) clearInterval(id);
            timers.length = 0;
            if (autoReqId !== null) clearTimeout(autoReqId);
            autoReqId = null;
            ctl.dispose();
        },
    };
}

// -----------------------------------------------------------------------------
// 4. 对外入口
// -----------------------------------------------------------------------------

/** 壳页没显式给 role 时的兜底嗅探(顺序:URL ?role= → <html data-scvb-role> → 文件名)。 */
function sniffRole(parsed) {
    if (parsed.role === "output" || parsed.role === "input") return parsed.role;
    if (typeof document !== "undefined" && document.documentElement) {
        const attr = document.documentElement.getAttribute("data-scvb-role");
        if (attr === "output" || attr === "input") return attr;
    }
    if (typeof location !== "undefined" && /input/i.test(location.pathname)) {
        return "input";
    }
    return "output";
}

/**
 * 造一次预览会话 —— **壳页的唯一入口**。
 *
 * @param {{role?:"output"|"input", params?:string|URLSearchParams|object,
 *          fixture?:string, loop?:"host"|"none", play?:boolean}} opts
 * @returns {{mock:object, ctl:object, world:object, start:Function, stop:Function,
 *            info:{role:string, fixture:string, scenario:string|null,
 *                  loop:"host"|"none", warnings:string[]}}}
 *   `mock` = 直接挂到目标窗口的 `window.__SCVB_MOCK__`(= createBridge 的 mockBackend)。
 */
export function createPreviewSession(opts = {}) {
    const parsed = parsePreviewQuery(
        opts.params !== undefined
            ? opts.params
            : typeof location !== "undefined"
              ? location.search
              : "",
    );
    const role =
        opts.role === "output" || opts.role === "input"
            ? opts.role
            : sniffRole(parsed);

    const warnings = parsed.warnings.slice();
    let fixture = parsed.fixture;
    if (opts.fixture) {
        if (FIXTURES.includes(opts.fixture)) fixture = opts.fixture;
        else warnings.push(`fixture ${opts.fixture} 不存在,已回落 ${fixture}`);
    }
    const loop = opts.loop ?? parsed.loop;
    const play = typeof opts.play === "boolean" ? opts.play : parsed.play;

    const world = buildWorld({
        role,
        fixture,
        loop,
        play,
        scenario: parsed.scenario,
    });
    const { backend, ctl } = createMockBackend({ role, world });
    const driver = makeDriver(ctl, world);

    for (const w of warnings) console.warn(`SCVB web-preview:${w}`);

    return {
        mock: backend,
        ctl,
        world,
        info: {
            role,
            fixture,
            scenario: parsed.scenario,
            // "host" = 宿主提供循环区(常态)/ "none" = 不提供(`?loop=none` 或 empty 空态)。
            // 两个值都在壳页工具条的白名单里(shell.js `LOOP_VALUES`),不会显示成 unknown。
            loop: world.caps.loopAvailable ? "host" : "none",
            warnings,
        },
        start: () => driver.start(backend),
        stop: () => driver.stop(),
    };
}

/**
 * 便捷版:造会话 + 挂 `window.__SCVB_MOCK__` + 起周期事件。
 * 同源 iframe 场景传 `targetWindow`(真源页面所在的那个 window)。
 */
export function installMock(opts = {}) {
    const session = createPreviewSession(opts);
    const win =
        opts.targetWindow || (typeof window !== "undefined" ? window : null);
    if (!win) {
        throw new Error(
            "installMock:没有可挂载的 window(node 环境请直接用 createPreviewSession)",
        );
    }
    win.__SCVB_MOCK__ = session.mock;
    session.start();
    return session;
}
