// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— mock 后端(T28 交付)
// =============================================================================
// 定位(06 §6.2 硬约束 2 / 契约 §0.7):
//   本文件是 `window.__JUCE__.backend` 的**同形替身**——把冻结契约
//   docs/SCVB_CONTRACT.md 的**全部** native function 与 event 用假数据实现一遍,
//   交给 `web/shared/bridge.js` 的 `createBridge({role, mockBackend})` 接线。
//   同形 = ①`BRIDGE_FUNCTIONS[role]` 的每个名字一个方法;②一个
//   `addEventListener(evtName, cb)`。缺一个,createBridge 就在运行期 throw。
//
// 纪律(逐条对应任务卡与 06 §6.2):
//   • **零 UI 代码**:本文件只产数据与状态,不碰 DOM、不引用 web/output|input 的任何东西。
//   • **数据一律走生成器**:载荷形状全部来自 `web/shared/mock-data.js` 的 make*(),
//     本文件只做「用生成器的产物组装契约登记的容器」,不自造字段名、不自造形状。
//   • **返回形状照「返回」行**:契约 §0.8 第 5 条规定每个函数的「返回」行是
//     **可枚举的完整并集**;本文件只返回该行登记过的形状,不发明新形状
//     (含拒绝态——例如 `setGroupId` 只可能回 `{ok:true}` 或 `{observer:true}`,
//     故非法 g 走夹取而不是 badArg)。
//   • **写函数回推状态**:写入 → 改内部 state → emit 增量事件,让单向渲染链路可预演;
//     但**不实现算法**(analyze 只在 800ms 后用生成器重算段表,不做真分析)。
//   • **`mBridgeReady` 门控**(§0.6):`requestInitialState()` 之前 emit 一律丢弃。
//   • **range.mode 三值枚举**(J04):`follow | daw_loop | manual`,默认 follow;
//     v0 的「全曲 = 起止哨兵」约定已废除,本文件不得残留任何哨兵式全曲判断。
//
// 场景化拒绝(fixture 定义见 state-driver.js,`caps` 由 world 传入):
//   • `second-output`:`caps.readOnly=true` → 「返回」行登记了 `{observer:true}` 的
//     五个写函数(setGroupId / setChannelConfig / setTrackManual / editSegment /
//     clearCoverage)一律回 `{observer:true}` 且不改 state;`recaptureArm` 回
//     `reason:"readOnly"`。
//   • `channel-conflict`:Input `setChannelId(n)` 命中 `caps.occupiedMask` 的位 →
//     `{conflict:true}` + 推 `scvb.error{code:"channelConflict"}`。
//   • `stereo-mixed&loop=none`:`caps.loopAvailable=false` → `setRange("daw_loop", …)`
//     回 `{ok:false, reason:"noLoop"}`(灰模 Range 档的「宿主未提供循环区」disabled 占位)。
//
// 依赖方向:web-preview/ → web/(单向)。**web/ 永不反向依赖本文件**(06 §6.2)。
// =============================================================================

import { BRIDGE_FUNCTIONS, BRIDGE_EVENTS } from "../../web/shared/bridge.js";
import { DESIGN } from "../../web/shared/design-box.js";
import {
    CHANNEL_COUNT,
    VERSION_COUNT,
    ENUMS,
    makeError,
    makeParams,
    makeSegments,
    makeWaveformTile,
} from "../../web/shared/mock-data.js";

// -----------------------------------------------------------------------------
// 0. 小工具(纯计算,不含任何契约形状)
// -----------------------------------------------------------------------------

/**
 * 过一次 JSON —— 真桥面本来就是 JSON 传输,mock 照做:
 * 返回值与事件载荷都不是内部对象的引用,UI 改了也污染不到 mock 的 state。
 */
function clone(v) {
    return v === undefined ? undefined : JSON.parse(JSON.stringify(v));
}

function isPlainObject(v) {
    return v !== null && typeof v === "object" && !Array.isArray(v);
}

/** 深合并:普通对象递归,数组与标量整体替换(与 mock-data 的 mergeDeep 同口径)。 */
function mergeDeep(base, patch) {
    if (!isPlainObject(patch)) return base;
    for (const key of Object.keys(patch)) {
        const v = patch[key];
        if (isPlainObject(v) && isPlainObject(base[key]))
            mergeDeep(base[key], v);
        else base[key] = v;
    }
    return base;
}

function clamp(x, lo, hi) {
    return x < lo ? lo : x > hi ? hi : x;
}

function isFiniteNumber(v) {
    return typeof v === "number" && Number.isFinite(v);
}

/** 夹取成整数(契约 §0.8 第 2 条:越界一律夹取或拒绝,绝不崩溃)。 */
function clampInt(v, lo, hi, fallback) {
    if (!isFiniteNumber(v)) return fallback;
    return clamp(Math.round(v), lo, hi);
}

function round(x, n = 2) {
    const p = 10 ** n;
    return Math.round(x * p) / p;
}

/** u16 位图 → 通道号数组(bit0=ch1 … bit14=ch15,bit15 保留,契约 §0.2 第 5 条)。 */
export function channelsOfMask(mask) {
    const out = [];
    for (let ch = 1; ch <= CHANNEL_COUNT; ch++) {
        if ((mask >>> (ch - 1)) & 1) out.push(ch);
    }
    return out;
}

/** 通道号数组 → u16 位图。 */
export function maskOfChannels(list) {
    let mask = 0;
    for (const ch of list) mask |= 1 << (ch - 1);
    return mask >>> 0;
}

// -----------------------------------------------------------------------------
// 1. ParamID 面(契约 §1.12-§1.14)
// -----------------------------------------------------------------------------
//
// 可**读**的 id 全集直接取 `makeParams()` 造出来的 values 键集 —— 不在本文件里
// 重抄一遍 `v{v}_t{tt}_*` 的拼法(那就成了第二真源)。可**写**的子集按契约的
// 「不在本通道」行剔除每轨 pan/vol:它们由引擎打印头驱动,UI 只读不写。

const PAN_OR_VOL_ID = /_(pan|vol)$/;

/** 该 id 是否属于 gesture 三段式可驱动的参数面(否则一律 `{ok:false, reason:"badArg"}`)。 */
function isWritableParamId(values, id) {
    return typeof id === "string" && id in values && !PAN_OR_VOL_ID.test(id);
}

/**
 * 出厂默认参数一帧:键集仍取生成器(不手写 id),只把值换成契约默认档
 * (width 100 / ms_balance 0 / lead_select 0 / 每轨 width 100、freeze 0、pan 0、vol 0 dB)。
 * 供 `empty` fixture 用——空工程不该显示 15 轨的画像值。
 */
export function makeDefaultParams(versionActive = 1) {
    const frame = makeParams({ versionActive });
    for (const id of Object.keys(frame.values)) {
        frame.values[id] = /(^|_)width$/.test(id) ? 100 : 0;
    }
    return frame;
}

// -----------------------------------------------------------------------------
// 2. 事件通道 + 内部模型
// -----------------------------------------------------------------------------

function makeContext(role, world) {
    const snapshot = clone(
        role === "output" ? world.output.snapshot : world.input.snapshot,
    );

    const model = {
        role,
        fixture: world.fixture,
        caps: clone(world.caps),
        durationS: world.durationS,
        /** §0.6:requestInitialState() 之前不推送任何事件。 */
        ready: false,
        transport: {
            timeS: world.transport.timeS,
            isPlaying: world.transport.isPlaying,
        },
        snapshot,
        // conn / config 与快照共用同一对象,写一处两处同步(契约 §1.1/§3.1 语义行:
        // 快照的 conn/config 子树与事件载荷不得各自漂移)。
        conn: snapshot.conn,
        config: role === "input" ? snapshot.config : null,
        // claim 只在 §4.1 `scvb.state` 里,**不在 §3.1 快照字段集**(A-30 对齐后仍是两个
        // 字段集:快照给 channel_id/group_id/role/conn/config/ui/version)。所以它单独存,
        // 绝不写进 model.snapshot —— 否则 requestInitialState() 会多回一个契约没登记的键。
        claim: role === "input" ? world.input.claim || "unassigned" : null,
        // §4.1 `abi_remote`:探测不到对端就**没有这个字段**(不发哨兵)。
        abiRemote: role === "input" ? world.input.abiRemote : undefined,
        params: role === "output" ? clone(world.output.params) : null,
        groupsOnline: world.groupsOnline,
        segVersion: role === "output" ? world.output.segments.version : 1,
        segByCh: new Map(),
        coveragePct: new Map(),
        errors: clone(world.errors[role] || []),
        timers: new Set(),
    };

    if (role === "output") {
        for (const entry of clone(world.output.segments.channels)) {
            model.segByCh.set(entry.ch, entry);
        }
        for (const entry of world.output.coverage || []) {
            model.coveragePct.set(entry.ch, entry.coveragePct);
        }
    }

    // ---- 事件分发(与 window.__JUCE__.backend.addEventListener 同形)-------------
    const listeners = new Map();
    const lastSent = new Map();
    const readyCbs = [];
    const allowedEvents = new Set(BRIDGE_EVENTS[role]);

    function addEventListener(name, cb) {
        if (typeof cb !== "function") {
            throw new TypeError("mockBackend.addEventListener:cb 必须是函数");
        }
        let list = listeners.get(name);
        if (!list) {
            list = [];
            listeners.set(name, list);
        }
        list.push(cb);
    }

    function emit(name, payload) {
        if (!allowedEvents.has(name)) {
            throw new RangeError(
                `mock 后端:事件名 ${JSON.stringify(name)} 不在 ${role} 侧名表内(契约 §7)`,
            );
        }
        // §0.6 门控:UI 调 requestInitialState() 之前一个事件都不发。
        if (!model.ready) return false;
        if (name === "scvb.captureProgress") {
            for (const entry of payload.channels || []) {
                model.coveragePct.set(entry.ch, entry.coveragePct);
            }
        }
        const list = listeners.get(name);
        if (!list || list.length === 0) return false;
        const frozen = clone(payload);
        for (const cb of list.slice()) cb(frozen);
        return true;
    }

    /** §0.4「值未变不发」——params/conn/groups 等周期事件走这一口。 */
    function emitIfChanged(name, payload) {
        // §0.6 门控期**不记账**。driver 的周期定时器在页面调 requestInitialState() 之前
        // 就已经在跑(灰模现状:页面根本不调,靠 driver 1.5s 后兜底代调),那几拍一个都
        // 发不出去;若照样写进 lastSent,门控解除后的「首帧必发」(§0.4 第 3 条)会被
        // 自己那几拍的记账悄悄抑制 —— 实测症状:Input 侧只剩 scvb.state,conn/config/groups
        // 三个首帧全丢,Output 侧丢 conn/groups(连接 pill 与状态灯永远点不亮)。
        if (!model.ready) return false;
        const key = JSON.stringify(payload);
        if (lastSent.get(name) === key) return false;
        lastSent.set(name, key);
        return emit(name, payload);
    }

    function later(ms, fn) {
        const id = setTimeout(() => {
            model.timers.delete(id);
            fn();
        }, ms);
        model.timers.add(id);
        return id;
    }

    function markReady() {
        if (model.ready) return;
        model.ready = true;
        // 首帧必发(§0.4 第 3 条)由 driver 负责编排;这里只广播「可以发了」。
        for (const cb of readyCbs.slice()) cb();
    }

    // ---- range / PRINT 判定(契约 §5.3 / §2.6)---------------------------------

    function loopWindow() {
        return model.caps.loopAvailable ? model.caps.loop : null;
    }

    /** 播放头是否落在 `global.range` 内。follow 恒 true(§5.3:follow 无界)。 */
    function inRangeAt(tS) {
        if (role !== "output") return true;
        const range = model.snapshot.global.range;
        if (range.mode === "follow") return true;
        if (range.mode === "daw_loop") {
            const loop = loopWindow();
            if (!loop) return true; // 宿主 loop 瞬态缺失:保持最后有效值,不回退档位
            return tS >= loop.startS && tS < loop.endS;
        }
        return tS >= range.start_s && tS < range.end_s;
    }

    /** PRINT 三与条件(§1.9/§5.6):输出 ON ∧ 播放中 ∧ 在 range 内。 */
    function isPrinting() {
        if (role !== "output") return false;
        return (
            model.snapshot.global.output_enabled === true &&
            model.transport.isPlaying === true &&
            inRangeAt(model.transport.timeS)
        );
    }

    // ---- state 回推 -----------------------------------------------------------

    /** Output/Input 快照 → `scvb.state` 全量帧(去掉快照专属键,补 `full`)。 */
    function fullStatePayload() {
        const s = clone(model.snapshot);
        if (role === "output") {
            delete s.session_guid;
            delete s.version;
            delete s.guide_seen_global;
            delete s.tour_seen_global;
            delete s.conn;
            return { full: true, ...s };
        }
        // §4.1 载荷 = 快照的部分字段 + claim(claim 不在快照里,见 model.claim 注释)。
        // `abi_remote` 探测不到时**字段不存在**(不发哨兵)。
        return {
            channel_id: s.channel_id,
            group_id: s.group_id,
            claim: model.claim,
            abi: s.version.abi,
            ...(model.abiRemote === undefined
                ? {}
                : { abi_remote: model.abiRemote }),
            ui: s.ui,
        };
    }

    /** 写内部 state 并推增量帧(Output 带 `full:false`;Input 无 full 字段)。 */
    function patchState(patch) {
        if (role === "input" && "claim" in patch) {
            model.claim = patch.claim;
            const rest = { ...patch };
            delete rest.claim;
            mergeDeep(model.snapshot, rest);
        } else {
            mergeDeep(model.snapshot, patch);
        }
        emit(
            "scvb.state",
            role === "output" ? { full: false, ...patch } : patch,
        );
    }

    // ---- 段表(§2.8)-----------------------------------------------------------

    function segmentsOf(ch) {
        const entry = model.segByCh.get(ch);
        return entry ? entry.segments : [];
    }

    /** 重排 segIdx —— 契约 §2.8:每次事件后 0 基重新编号。 */
    function renumber(list) {
        list.forEach((seg, i) => {
            seg.segIdx = i;
        });
        return list;
    }

    /**
     * 用内部段表拼一帧 `scvb.segments`。
     * 容器与字段全部照 §2.8;段对象本身一律是生成器造的(或生成器产物的就地编辑),
     * 本函数不新造段字段。
     */
    function segmentsPayload(reason, chList) {
        const channels = chList
            .map((ch) => model.segByCh.get(ch))
            .filter(Boolean)
            .map((entry) => clone(entry));
        let kept = 0;
        let total = 0;
        for (const entry of channels) {
            total += entry.segments.length;
            for (const seg of entry.segments) {
                if (seg.origin !== "auto") kept++;
            }
        }
        // 首帧快照/切版本是「全量给到」而非「改了多少」(与 mock-data 的 isFullDump 同口径)
        const isFullDump = reason === "snapshot" || reason === "versionActive";
        return {
            version: model.segVersion,
            reason,
            channels,
            diff: {
                kept,
                changed: [],
                added: isFullDump ? total : 0,
                removed: 0,
            },
        };
    }

    /**
     * 用生成器重算给定轨的段表(analyze / 版本切换 / 版本复制共用)。
     * `store:false` 用于「结果不属当前激活版本」的情形(copyVersion 到非激活版本):
     * 事件照发(载荷带 `version`),但内部显示用的段表不被换掉。
     */
    function regenerateSegments(
        version,
        reason,
        chList,
        { store = true } = {},
    ) {
        const frame = makeSegments(version, reason, chList);
        if (store) {
            for (const entry of frame.channels) {
                model.segByCh.set(entry.ch, clone(entry));
            }
            model.segVersion = version;
        }
        return frame;
    }

    function allChannels() {
        return Array.from({ length: CHANNEL_COUNT }, (_, i) => i + 1);
    }

    const ctl = {
        role,
        fixture: model.fixture,
        model,
        emit,
        emitIfChanged,
        addEventListener,
        isReady: () => model.ready,
        onReady(cb) {
            readyCbs.push(cb);
            if (model.ready) cb();
        },
        /** driver 每帧同步走带位置;mock 据此算 inRange 与 PRINT。 */
        setTransport({ timeS, isPlaying }) {
            if (isFiniteNumber(timeS)) model.transport.timeS = timeS;
            if (typeof isPlaying === "boolean")
                model.transport.isPlaying = isPlaying;
        },
        isPrinting,
        inRangeAt,
        /** §2.6 的可选字段:宿主提供 loop 才出现,缺失即字段不存在(不发哨兵)。 */
        playheadOverrides(tS) {
            const loop = loopWindow();
            const extra = { inRange: inRangeAt(tS) };
            if (loop) {
                extra.loopStartS = loop.startS;
                extra.loopEndS = loop.endS;
            }
            return extra;
        },
        fullStatePayload,
        segmentsPayload,
        connPayload: () => clone(model.conn),
        configPayload: () => clone(model.config),
        paramsFullPayload() {
            return {
                ...clone(model.params),
                full: true,
                hostEcho: false,
                versionActive: model.snapshot.global.version_active,
            };
        },
        groupsPayload: () => ({ groups_online: model.groupsOnline }),
        /** fixture 的条件类事件(§0.4 第 3 条:不发空 error,只在条件成立时发)。 */
        pendingErrors: () => clone(model.errors),
        connectedChannels() {
            if (role !== "output") return [];
            return model.conn.channels
                .map((c, i) => ({ ch: i + 1, c }))
                .filter((x) => x.c.slotState === 2 && x.c.heartbeatFresh)
                .map((x) => x.ch);
        },
        /**
         * PRINT 态的引擎打印头 —— 用当前播放位置在段表上取值,回一帧
         * `scvb.params` 稀疏 diff(`hostEcho:true`,§0.5:UI 只更新显示、绝不回写)。
         * 值未变则返回 null(driver 据此跳过本帧)。
         */
        printedParamsDiff() {
            if (role !== "output" || !isPrinting()) return null;
            const v = model.snapshot.global.version_active;
            const tS = model.transport.timeS;
            const values = {};
            for (const ch of allChannels()) {
                const seg = segmentsOf(ch).find(
                    (s) => tS >= s.t0S && tS < s.t1S,
                );
                if (!seg) continue;
                const p = `v${v}_t${String(ch).padStart(2, "0")}_`;
                if (model.params.values[`${p}pan`] !== seg.pan) {
                    values[`${p}pan`] = seg.pan;
                    model.params.values[`${p}pan`] = seg.pan;
                }
                if (model.params.values[`${p}vol`] !== seg.volDb) {
                    values[`${p}vol`] = seg.volDb;
                    model.params.values[`${p}vol`] = seg.volDb;
                }
            }
            if (Object.keys(values).length === 0) return null;
            return { values, hostEcho: true, full: false, versionActive: v };
        },
        /** 停掉所有内部定时器(node 冒烟/页面卸载用)。 */
        dispose() {
            for (const id of model.timers) clearTimeout(id);
            model.timers.clear();
        },
    };

    return {
        role,
        model,
        ctl,
        emit,
        addEventListener,
        markReady,
        patchState,
        fullStatePayload,
        segmentsPayload,
        regenerateSegments,
        segmentsOf,
        renumber,
        later,
        isPrinting,
        inRangeAt,
        allChannels,
    };
}

// -----------------------------------------------------------------------------
// 3. Output 后端 —— 契约 §1 的 34 个函数(顺序照 §7 manifest)
// -----------------------------------------------------------------------------

function buildOutputBackend(ctx) {
    const {
        model,
        ctl,
        emit,
        markReady,
        patchState,
        segmentsPayload,
        regenerateSegments,
        segmentsOf,
        renumber,
        later,
        isPrinting,
        allChannels,
    } = ctx;

    const OK = () => ({ ok: true });
    const BAD_ARG = () => ({ ok: false, reason: "badArg" });
    const OBSERVER = () => ({ observer: true });

    /** 只读观察态(`second-output`):§5.6 的 `{observer:true}`。 */
    function readOnly() {
        return model.caps.readOnly === true;
    }

    /** 宿主未提供时间线(§5.1 `noTimeline`):采集/输出开关的唯一拒绝分支。 */
    function noTimeline() {
        return model.caps.noTimeline === true;
    }

    /** 分析 dry-run:只数 `range ∩ coverage` 内的段(§1.5,纯只读、不写 state)。 */
    function affectedOf(scope) {
        const mask =
            scope === "all" || scope === undefined || scope === null
                ? maskOfChannels(allChannels())
                : Number(scope.tracksMask) || 0;
        const chList = channelsOfMask(mask);
        let intervals = 0;
        let manualKept = 0;
        let tracks = 0;
        const startS = isFiniteNumber(scope?.startS) ? scope.startS : -Infinity;
        const endS = isFiniteNumber(scope?.endS) ? scope.endS : Infinity;
        for (const ch of chList) {
            const hit = segmentsOf(ch).filter(
                (s) => s.t1S > startS && s.t0S < endS,
            );
            if (hit.length === 0) continue;
            tracks++;
            intervals += hit.length;
            manualKept += hit.filter((s) => s.origin !== "auto").length;
        }
        return { intervals, tracks, manualKept, channels: chList };
    }

    function pushSegments(reason, chList) {
        emit("scvb.segments", segmentsPayload(reason, chList));
    }

    /** 松手档 300ms 防抖(§1.18/§1.19):停止调用后才跑「流水线」。 */
    let debounceId = null;
    function debounceAnalysisPipeline(reason) {
        if (debounceId !== null) clearTimeout(debounceId);
        debounceId = later(300, () => {
            debounceId = null;
            const v = model.snapshot.global.version_active;
            regenerateSegments(v, reason, allChannels());
            pushSegments(reason, allChannels());
        });
    }

    const backend = {
        // ---- §1.1 -------------------------------------------------------------
        requestInitialState() {
            const snap = clone(model.snapshot);
            markReady();
            return snap;
        },

        // ---- §1.2 -------------------------------------------------------------
        setCaptureEnabled(on) {
            if (noTimeline()) return { ok: false, reason: "noTimeline" };
            patchState({ global: { capture_enabled: !!on } });
            return OK();
        },

        // ---- §1.3 -------------------------------------------------------------
        setOutputEnabled(on) {
            if (noTimeline()) return { ok: false, reason: "noTimeline" };
            patchState({ global: { output_enabled: !!on } });
            return OK();
        },

        // ---- §1.4(返回行只有 {ok} | {observer:true} ⇒ 非法 g 走夹取,不发 badArg)--
        setGroupId(g) {
            if (readOnly()) return OBSERVER();
            const next = clampInt(g, 1, 8, model.snapshot.group_id);
            patchState({ group_id: next });
            return OK();
        },

        // ---- §1.5 -------------------------------------------------------------
        previewAnalyze(scope) {
            const a = affectedOf(scope);
            return {
                intervals: a.intervals,
                tracks: a.tracks,
                manualKept: a.manualKept,
            };
        },

        // ---- §1.6 -------------------------------------------------------------
        analyze(scope, opts) {
            if (model.snapshot.analysis_run.running) {
                return { ok: false, reason: "busy" };
            }
            const a = affectedOf(scope);
            const affected = {
                intervals: a.intervals,
                tracks: a.tracks,
                manualKept: a.manualKept,
            };
            // range ∩ coverage = ∅ ⇒ 受理失败(UI 侧同条件按钮 disabled)
            if (a.intervals === 0) return { ok: false, affected };

            const version = model.snapshot.global.version_active;
            const clearManual = !!(opts && opts.clearManual);
            patchState({ analysis_run: { running: true, progress: 0 } });
            // [W] 线程的秒级流水线在 mock 里退化成 4 拍进度 + 一次段表重算:
            // 本卡不实现算法(brief「mock 后端行为」),只保证事件时序与 UI 消费面真实。
            for (let step = 1; step <= 3; step++) {
                later(200 * step, () => {
                    if (!model.snapshot.analysis_run.running) return;
                    patchState({
                        analysis_run: { running: true, progress: step / 4 },
                    });
                });
            }
            later(800, () => {
                if (!model.snapshot.analysis_run.running) return;
                const frame = regenerateSegments(
                    version,
                    "analyze",
                    a.channels,
                );
                if (clearManual) {
                    // `clearManual:true` 只重置 origin,locked 段免疫(04 §4.4 / J34)。
                    for (const ch of a.channels) {
                        for (const seg of segmentsOf(ch)) {
                            if (!seg.locked) seg.origin = "auto";
                        }
                    }
                }
                // 段表取内部模型(带 clearManual 的后置状态),diff 明细沿用生成器算出的那份。
                const payload = segmentsPayload("analyze", a.channels);
                payload.diff.changed = frame.diff.changed;
                payload.diff.added = frame.diff.added;
                payload.diff.removed = frame.diff.removed;
                emit("scvb.segments", payload);
                patchState({ analysis_run: { running: false, progress: 1 } });
            });
            return { ok: true, affected };
        },

        // ---- §1.7 -------------------------------------------------------------
        cancelAnalyze() {
            if (!model.snapshot.analysis_run.running) return { ok: false };
            patchState({ analysis_run: { running: false, progress: 0 } });
            return { ok: true };
        },

        // ---- §1.8(J04 三值枚举;follow 忽略起止,无哨兵约定)---------------------
        setRange(mode, startS, endS) {
            if (!ENUMS.rangeMode.includes(mode)) return BAD_ARG();
            if (mode === "daw_loop" && !model.caps.loopAvailable) {
                return { ok: false, reason: "noLoop" };
            }
            const current = model.snapshot.global.range;
            if (mode === "manual") {
                if (
                    !isFiniteNumber(startS) ||
                    !isFiniteNumber(endS) ||
                    !(startS < endS)
                ) {
                    return BAD_ARG();
                }
                patchState({
                    global: { range: { mode, start_s: startS, end_s: endS } },
                });
                return OK();
            }
            if (mode === "daw_loop") {
                const loop = model.caps.loop;
                patchState({
                    global: {
                        range: {
                            mode,
                            start_s: loop.startS,
                            end_s: loop.endS,
                        },
                    },
                });
                return OK();
            }
            // follow:起止被忽略(C++ 不解释调用方传的值),故原样保留既有值。
            patchState({
                global: {
                    range: {
                        mode,
                        start_s: current.start_s,
                        end_s: current.end_s,
                    },
                },
            });
            return OK();
        },

        // ---- §1.9 -------------------------------------------------------------
        setVersionActive(v) {
            if (isPrinting()) return { rejected: "printing" };
            const next = clampInt(
                v,
                1,
                VERSION_COUNT,
                model.snapshot.global.version_active,
            );
            patchState({ global: { version_active: next } });
            // 切版本:全量重发 params + 全量重发 segments(§1.9 语义行)
            model.params = makeParams({ versionActive: next });
            emit("scvb.params", ctl.paramsFullPayload());
            emit(
                "scvb.segments",
                regenerateSegments(next, "versionActive", allChannels()),
            );
            return OK();
        },

        // ---- §1.10 ------------------------------------------------------------
        setVersionName(v, name) {
            const idx =
                clampInt(
                    v,
                    1,
                    VERSION_COUNT,
                    model.snapshot.global.version_active,
                ) - 1;
            const raw = typeof name === "string" ? name : "";
            const trimmed = raw.trim();
            const settled =
                trimmed === ""
                    ? `V${idx + 1}`
                    : Array.from(raw).slice(0, 16).join("");
            const versions = clone(model.snapshot.versions);
            versions[idx].name = settled;
            patchState({ versions });
            return { ok: true, name: settled };
        },

        // ---- §1.11 ------------------------------------------------------------
        copyVersion(src, dst) {
            if (isPrinting()) return { rejected: "printing" };
            if (
                !Number.isInteger(src) ||
                !Number.isInteger(dst) ||
                src < 1 ||
                dst < 1 ||
                src > VERSION_COUNT ||
                dst > VERSION_COUNT ||
                src === dst
            ) {
                return BAD_ARG();
            }
            const versions = clone(model.snapshot.versions);
            // 深拷贝曲线与 pan_curve,**不复制 name**(§1.11)
            versions[dst - 1].pan_curve = clone(versions[src - 1].pan_curve);
            versions[dst - 1].empty = versions[src - 1].empty;
            patchState({ versions });
            // 结果属 dst 版本;dst 不是当前激活版本时只发事件、不换内部显示用段表。
            const store = dst === model.snapshot.global.version_active;
            emit(
                "scvb.segments",
                regenerateSegments(dst, "copyVersion", allChannels(), {
                    store,
                }),
            );
            return OK();
        },

        // ---- §1.12 ------------------------------------------------------------
        beginParamGesture(id) {
            return isWritableParamId(model.params.values, id)
                ? OK()
                : BAD_ARG();
        },

        // ---- §1.13 ------------------------------------------------------------
        setParam(id, value) {
            if (!isWritableParamId(model.params.values, id)) return BAD_ARG();
            if (!isFiniteNumber(value)) return BAD_ARG();
            model.params.values[id] = value;
            emit("scvb.params", {
                values: { [id]: value },
                hostEcho: false,
                full: false,
                versionActive: model.snapshot.global.version_active,
            });
            return OK();
        },

        // ---- §1.14 ------------------------------------------------------------
        endParamGesture(id) {
            return isWritableParamId(model.params.values, id)
                ? OK()
                : BAD_ARG();
        },

        // ---- §1.15 ------------------------------------------------------------
        setChannelConfig(ch, patch) {
            if (readOnly()) return OBSERVER();
            if (!Number.isInteger(ch) || ch < 1 || ch > CHANNEL_COUNT) {
                return BAD_ARG();
            }
            if (!isPlainObject(patch)) return BAD_ARG();
            // 不可写字段(§1.15「不可写字段」行):source_channels 是 Input 实测值,
            // auto_pan/auto_vol 已按 J65 删除。
            for (const forbidden of [
                "source_channels",
                "auto_pan",
                "auto_vol",
            ]) {
                if (forbidden in patch) return BAD_ARG();
            }
            const next = {};
            if ("enabled" in patch) next.enabled = !!patch.enabled;
            if ("label" in patch) {
                if (typeof patch.label !== "string") return BAD_ARG();
                next.label = Array.from(patch.label).slice(0, 24).join("");
            }
            if ("priority" in patch) {
                if (!Number.isInteger(patch.priority)) return BAD_ARG();
                next.priority = clamp(patch.priority, 0, 10);
            }
            if ("lead_lock" in patch) next.lead_lock = !!patch.lead_lock;
            if ("lead_vol_exempt" in patch)
                next.lead_vol_exempt = !!patch.lead_vol_exempt;
            if ("participate_in_auto_pan" in patch)
                next.participate_in_auto_pan = !!patch.participate_in_auto_pan;
            if ("pair_id" in patch) {
                if (!Number.isInteger(patch.pair_id)) return BAD_ARG();
                next.pair_id = clamp(patch.pair_id, 0, 7);
            }
            const channels = clone(model.snapshot.channels);
            mergeDeep(channels[ch - 1], next);
            // 广播区整体版本号 +1(§1.15/§4.3 写侧纪律)
            patchState({
                channels,
                config_seq: model.snapshot.config_seq + 1,
            });
            return OK();
        },

        // ---- §1.16 ------------------------------------------------------------
        setTrackManual(ch, panOrVol, value) {
            if (readOnly()) return OBSERVER();
            if (
                !Number.isInteger(ch) ||
                ch < 1 ||
                ch > CHANNEL_COUNT ||
                (panOrVol !== "pan" && panOrVol !== "vol") ||
                !isFiniteNumber(value)
            ) {
                // 「返回」行未登记 badArg ⇒ 非法入参按 §0.8 第 2 条夹取:
                // 这里没有可夹取的语义(轨号/字段名不存在),按不改 state 的空写入处理。
                return { ok: true, replacedSegments: 0, replacedLocked: 0 };
            }
            const old = segmentsOf(ch);
            const replacedSegments = old.length;
            const replacedLocked = old.filter((s) => s.locked).length;
            // 04 §1.5 方案 A:向当前激活版本该轨写入**覆盖全时间线的单段常值**。
            // 段对象形状取生成器产物为原型(不自造字段),只改该改的几个键。
            const seed =
                old[0] ||
                makeSegments(model.segVersion, "snapshot", [ch]).channels[0]
                    .segments[0];
            if (!seed) {
                return { ok: true, replacedSegments, replacedLocked };
            }
            const proto = clone(seed);
            proto.segIdx = 0;
            proto.t0S = 0;
            proto.t1S = model.durationS;
            proto.origin = "user_edited";
            proto.locked = true;
            if (panOrVol === "pan")
                proto.pan = clamp(round(value, 1), -100, 100);
            else proto.volDb = clamp(round(value, 1), -24, 12);
            model.segByCh.set(ch, { ch, segments: [proto], stale: false });
            pushSegments("trackManual", [ch]);
            return { ok: true, replacedSegments, replacedLocked };
        },

        // ---- §1.17 ------------------------------------------------------------
        setPanCurve(points) {
            if (!Array.isArray(points) || points.length > 16) return BAD_ARG();
            for (const p of points) {
                if (
                    !isPlainObject(p) ||
                    !isFiniteNumber(p.angle) ||
                    p.angle < -100 ||
                    p.angle > 100 ||
                    !isFiniteNumber(p.gain_db) ||
                    !["bell", "shelf", "cut"].includes(p.shape) ||
                    !isFiniteNumber(p.q) ||
                    p.q <= 0 ||
                    !["out", "left", "right"].includes(p.side ?? "out")
                ) {
                    return BAD_ARG();
                }
            }
            const active = model.snapshot.global.version_active;
            const versions = clone(model.snapshot.versions);
            versions[active - 1].pan_curve = { points: clone(points) };
            versions[active - 1].empty = points.length === 0;
            patchState({ versions });
            return OK();
        },

        // ---- §1.18 ------------------------------------------------------------
        setVadParams(p) {
            const keys = [
                "threshold_db",
                "hysteresis_db",
                "hangover_ms",
                "padding_pre_ms",
                "padding_post_ms",
            ];
            if (!isPlainObject(p)) return BAD_ARG();
            for (const k of keys) {
                if (!isFiniteNumber(p[k])) return BAD_ARG();
            }
            patchState({ analysis: { vad: clone(p) } });
            debounceAnalysisPipeline("vad");
            return OK();
        },

        // ---- §1.19 ------------------------------------------------------------
        setSegmentation(p) {
            if (
                !isPlainObject(p) ||
                typeof p.mode !== "string" ||
                !isFiniteNumber(p.sensitivity) ||
                !isFiniteNumber(p.min_segment_ms)
            ) {
                return BAD_ARG();
            }
            patchState({ analysis: { segmentation: clone(p) } });
            debounceAnalysisPipeline("segmentation");
            return OK();
        },

        // ---- §1.20(越界夹取到 [20,300] 并回推夹取后的值)------------------------
        setTransitionRamp(ms) {
            const next = isFiniteNumber(ms)
                ? clamp(ms, 20, 300)
                : model.snapshot.analysis.transition_ramp_ms;
            patchState({ analysis: { transition_ramp_ms: next } });
            return OK();
        },

        // ---- §1.21 ------------------------------------------------------------
        setAnalysisConfig(patch) {
            if (!isPlainObject(patch)) return BAD_ARG();
            const next = {};
            if ("loudness_mode" in patch) {
                if (!ENUMS.analysisLoudnessMode.includes(patch.loudness_mode))
                    return BAD_ARG();
                next.loudness_mode = patch.loudness_mode;
            }
            if ("center_slot_policy" in patch) {
                if (
                    !ENUMS.analysisCenterSlotPolicy.includes(
                        patch.center_slot_policy,
                    )
                )
                    return BAD_ARG();
                next.center_slot_policy = patch.center_slot_policy;
            }
            if (Object.keys(next).length === 0) return BAD_ARG();
            patchState({ analysis: next });
            return OK();
        },

        // ---- §1.22(五 op,payload 逐 op 见 §5.4)-------------------------------
        editSegment(ch, op, payload) {
            if (readOnly()) return OBSERVER();
            if (
                !Number.isInteger(ch) ||
                ch < 1 ||
                ch > CHANNEL_COUNT ||
                !ENUMS.editSegmentOp.includes(op) ||
                !isPlainObject(payload)
            ) {
                return BAD_ARG();
            }
            const list = segmentsOf(ch);
            const inBounds = (i) =>
                Number.isInteger(i) && i >= 0 && i < list.length;

            if (op === "move_boundary") {
                const { segIdx, edge, tS } = payload;
                if (!inBounds(segIdx) || !isFiniteNumber(tS)) return BAD_ARG();
                if (edge !== "t0" && edge !== "t1") return BAD_ARG();
                const seg = list[segIdx];
                if (edge === "t0") {
                    const lo = segIdx > 0 ? list[segIdx - 1].t0S + 0.05 : 0;
                    const next = clamp(tS, lo, seg.t1S - 0.05);
                    seg.t0S = round(next, 3);
                    if (segIdx > 0) list[segIdx - 1].t1S = seg.t0S;
                } else {
                    const hi =
                        segIdx < list.length - 1
                            ? list[segIdx + 1].t1S - 0.05
                            : model.durationS;
                    const next = clamp(tS, seg.t0S + 0.05, hi);
                    seg.t1S = round(next, 3);
                    if (segIdx < list.length - 1)
                        list[segIdx + 1].t0S = seg.t1S;
                }
                seg.origin = "user_edited";
                seg.locked = true;
            } else if (op === "split") {
                const { segIdx, tS } = payload;
                if (!inBounds(segIdx) || !isFiniteNumber(tS)) return BAD_ARG();
                const seg = list[segIdx];
                if (!(tS > seg.t0S && tS < seg.t1S)) return BAD_ARG();
                // 两子段继承原值(§5.4 split 行)
                const right = clone(seg);
                right.t0S = round(tS, 3);
                seg.t1S = round(tS, 3);
                for (const s of [seg, right]) {
                    s.origin = "user_edited";
                    s.locked = true;
                }
                list.splice(segIdx + 1, 0, right);
            } else if (op === "merge") {
                const { segIdxA, segIdxB } = payload;
                if (!inBounds(segIdxA) || !inBounds(segIdxB)) return BAD_ARG();
                if (Math.abs(segIdxA - segIdxB) !== 1) {
                    return { ok: false, reason: "notAdjacent" };
                }
                const a = list[Math.min(segIdxA, segIdxB)];
                const b = list[Math.max(segIdxA, segIdxB)];
                const da = a.t1S - a.t0S;
                const db = b.t1S - b.t0S;
                const total = da + db || 1;
                // 合并值按时长加权(§5.4 merge 行)
                a.pan = round((a.pan * da + b.pan * db) / total, 1);
                a.volDb = round((a.volDb * da + b.volDb * db) / total, 1);
                a.loudnessLufs = round(
                    (a.loudnessLufs * da + b.loudnessLufs * db) / total,
                    1,
                );
                a.t1S = b.t1S;
                a.origin = "user_edited";
                a.locked = true;
                list.splice(list.indexOf(b), 1);
            } else if (op === "set_values") {
                const { segIdx, pan, volDb } = payload;
                if (!inBounds(segIdx)) return BAD_ARG();
                if (pan === undefined && volDb === undefined) return BAD_ARG();
                const seg = list[segIdx];
                if (pan !== undefined) {
                    if (!isFiniteNumber(pan)) return BAD_ARG();
                    seg.pan = clamp(round(pan, 1), -100, 100);
                }
                if (volDb !== undefined) {
                    if (!isFiniteNumber(volDb)) return BAD_ARG();
                    seg.volDb = clamp(round(volDb, 1), -24, 12);
                }
                seg.origin = "user_edited";
                seg.locked = true;
            } else {
                // set_locked:单纯切 locked,**不改 origin**(§5.4)
                const { segIdx, locked } = payload;
                if (!inBounds(segIdx) || typeof locked !== "boolean") {
                    return BAD_ARG();
                }
                list[segIdx].locked = locked;
            }

            renumber(list);
            pushSegments("edit", [ch]);
            return OK();
        },

        // ---- §1.23 ------------------------------------------------------------
        recaptureArm(tracksMask, startS, endS, autoStop) {
            const mask = Number.isInteger(tracksMask) ? tracksMask >>> 0 : 0;
            const s = isFiniteNumber(startS) ? startS : 0;
            const e = isFiniteNumber(endS) ? endS : 0;
            const echo = { tracksMask: mask, startS: s, endS: e };
            // 撤销布防(§1.23 语义行的显式约定;与 range 的哨兵无关)
            if (mask === 0 && s === 0 && e === 0) {
                patchState({
                    recapture: { armed: false, ...echo, autoStop: false },
                });
                return { armed: false, ...echo };
            }
            if (readOnly())
                return { armed: false, ...echo, reason: "readOnly" };
            if (model.caps.noTimeline)
                return { armed: false, ...echo, reason: "noTimeline" };
            if (mask === 0)
                return { armed: false, ...echo, reason: "noTracks" };
            if (!(s < e))
                return { armed: false, ...echo, reason: "noSelection" };
            patchState({
                recapture: { armed: true, ...echo, autoStop: !!autoStop },
            });
            return { armed: true, ...echo };
        },

        // ---- §1.24 ------------------------------------------------------------
        clearCoverage(tracksMask, startS, endS) {
            if (readOnly()) return OBSERVER();
            const mask = Number.isInteger(tracksMask) ? tracksMask >>> 0 : 0;
            if (
                mask === 0 ||
                !isFiniteNumber(startS) ||
                !isFiniteNumber(endS) ||
                !(startS < endS)
            ) {
                return BAD_ARG();
            }
            const chList = channelsOfMask(mask);
            const span = endS - startS;
            const pctDrop = (span / model.durationS) * 100;
            const channels = chList.map((ch) => {
                const before = model.coveragePct.get(ch) ?? 0;
                const after = round(clamp(before - pctDrop, 0, 100), 1);
                model.coveragePct.set(ch, after);
                // §2.7 的 addedRanges 只表达「新增」,清除只体现在 coveragePct 上。
                return { ch, addedRanges: [], coveragePct: after };
            });
            emit("scvb.captureProgress", { channels });
            return { ok: true, clearedS: round(span * chList.length, 2) };
        },

        // ---- §1.25 / §1.26(brief:栈空即可)------------------------------------
        undo() {
            return { ok: false };
        },
        redo() {
            return { ok: false };
        },

        // ---- §1.27(request/response,绝不进事件流)------------------------------
        requestWaveform(ch, startS, endS, cols) {
            if (
                !Number.isInteger(ch) ||
                ch < 1 ||
                ch > CHANNEL_COUNT ||
                !Number.isInteger(cols) ||
                cols < 1 ||
                cols > 4096 ||
                !isFiniteNumber(startS) ||
                !isFiniteNumber(endS) ||
                !(startS < endS)
            ) {
                return BAD_ARG();
            }
            return makeWaveformTile(ch, startS, endS, cols);
        },

        // ---- §1.28 / §1.29 ----------------------------------------------------
        setUiScale(f) {
            if (!DESIGN.output.presets.includes(f)) return BAD_ARG();
            patchState({ ui: { scale: f } });
            return OK();
        },
        commitUiScale() {
            return OK();
        },

        // ---- §1.30(未知 code 回退 zh 并回推实际值)------------------------------
        setLang(code) {
            const next = ENUMS.language.includes(code) ? code : "zh";
            patchState({ ui: { language: next } });
            return OK();
        },

        // ---- §1.31 ------------------------------------------------------------
        setActiveTab(tab) {
            if (!ENUMS.activeTab.includes(tab)) return BAD_ARG();
            patchState({ ui: { active_tab: tab } });
            return OK();
        },

        // ---- §1.32 / §1.33(alsoGlobal 只写全局默认位,不回写 state)-------------
        setGuideSeen(seen, alsoGlobal = true) {
            patchState({ ui: { guide_seen: !!seen } });
            if (alsoGlobal !== false) model.snapshot.guide_seen_global = !!seen;
            return OK();
        },
        setTourSeen(seen, alsoGlobal = true) {
            patchState({ ui: { tour_seen: !!seen } });
            if (alsoGlobal !== false) model.snapshot.tour_seen_global = !!seen;
            return OK();
        },

        // ---- §1.34(幂等)------------------------------------------------------
        confirmPrintGuard() {
            patchState({ print_guard: { pending: false } });
            return OK();
        },
    };

    return backend;
}

// -----------------------------------------------------------------------------
// 4. Input 后端 —— 契约 §3 的 7 个函数
// -----------------------------------------------------------------------------

function buildInputBackend(ctx) {
    const { model, emit, markReady, patchState, later } = ctx;

    const OK = () => ({ ok: true });

    /** claim 结果 → §5.2 六态。 */
    function claimStateFor(channelId) {
        if (channelId === 0) return "unassigned";
        return model.conn.outputOnline ? "active" : "idle";
    }

    const backend = {
        // ---- §3.1 -------------------------------------------------------------
        requestInitialState() {
            const snap = clone(model.snapshot);
            markReady();
            return snap;
        },

        // ---- §3.2(返回行只有 {ok} | {conflict:true} ⇒ 非法 n 走夹取)------------
        setChannelId(n) {
            const next = clampInt(
                n,
                0,
                CHANNEL_COUNT,
                model.snapshot.channel_id,
            );
            const occupiedByOthers =
                next > 0 &&
                ((model.caps.occupiedMask >>> (next - 1)) & 1) === 1 &&
                next !== model.snapshot.channel_id;
            if (occupiedByOthers) {
                patchState({ claim: "conflict" });
                emit(
                    "scvb.error",
                    makeError("channelConflict", {
                        ch: next,
                        detail: { groupId: model.snapshot.group_id },
                    }),
                );
                return { conflict: true };
            }
            const claim = claimStateFor(next);
            patchState({ channel_id: next, claim });
            // 本实例占的位也算进 occupiedMask(§4.2 字段纪律),并同步音频路径。
            const own = next > 0 ? 1 << (next - 1) : 0;
            mergeDeep(model.conn, {
                maskBit: claim === "active",
                passthrough: claim !== "active",
                capturing: claim === "active" && model.conn.capturing,
                occupiedMask: (model.caps.occupiedMask | own) >>> 0,
            });
            emit("scvb.conn", clone(model.conn));
            return OK();
        },

        // ---- §3.3 -------------------------------------------------------------
        setGroupId(g) {
            const next = clampInt(g, 1, 8, model.snapshot.group_id);
            if (model.caps.groupConflict) {
                patchState({ claim: "conflict" });
                emit(
                    "scvb.error",
                    makeError("channelConflict", {
                        ch: model.snapshot.channel_id || 1,
                        detail: { groupId: next },
                    }),
                );
                return { conflict: true };
            }
            patchState({
                group_id: next,
                claim: claimStateFor(model.snapshot.channel_id),
            });
            return OK();
        },

        // ---- §3.4(经 ctrl 命令环投递;Input 侧只做乐观显示,以 scvb.config 回执为准)
        remoteSetPriority(n) {
            if (model.snapshot.channel_id === 0) {
                return { queued: false, reason: "unassigned" };
            }
            if (!model.conn.outputOnline) {
                return { queued: false, reason: "outputOffline" };
            }
            if (model.caps.ringFull) {
                return { queued: false, reason: "ringFull" };
            }
            const value = clampInt(n, 0, 10, model.config.priority);
            // Output [M] 消费后落 state 再经 config_seq 变化回执(mock 用 120ms 模拟往返)
            later(120, () => {
                mergeDeep(model.config, {
                    priority: value,
                    config_seq: model.config.config_seq + 1,
                });
                emit("scvb.config", clone(model.config));
            });
            return { queued: true };
        },

        // ---- §3.5 / §3.6 / §3.7 ------------------------------------------------
        setUiScale(f) {
            if (!DESIGN.input.presets.includes(f)) {
                return { ok: false, reason: "badArg" };
            }
            patchState({ ui: { scale: f } });
            return OK();
        },
        commitUiScale() {
            return OK();
        },
        setLang(code) {
            const next = ENUMS.language.includes(code) ? code : "zh";
            patchState({ ui: { language: next } });
            return OK();
        },
    };

    return backend;
}

// -----------------------------------------------------------------------------
// 5. 出厂 + 结构化 parity 自检
// -----------------------------------------------------------------------------

/**
 * 结构化自检(契约 §0.7 的运行期兜底,比 createBridge 的校名早一步):
 * 实现集合与 `BRIDGE_FUNCTIONS[role]` **双向**相等 —— 少一个是漏实现,
 * 多一个说明写手自造了契约外的名字(§0.1 第 4 条:不得出现语义重复的双入口)。
 */
function assertParity(role, backend) {
    const want = BRIDGE_FUNCTIONS[role];
    const have = Object.keys(backend).filter((k) => k !== "addEventListener");
    const missing = want.filter((n) => typeof backend[n] !== "function");
    const extra = have.filter((n) => !want.includes(n));
    if (missing.length > 0 || extra.length > 0) {
        throw new Error(
            `juce-bridge-mock:${role} 侧与契约 §7 manifest 不等 —— ` +
                `缺 ${missing.length} 个 [${missing.join(", ")}];` +
                `多 ${extra.length} 个 [${extra.join(", ")}]`,
        );
    }
}

/**
 * 造一个 mock 后端。
 *
 * @param {{role:"output"|"input", world:object}} options
 *   world 由 `state-driver.js` 的 `buildWorld()` 组装(fixture 初始状态 + caps 场景开关)。
 * @returns {{backend:object, ctl:object}}
 *   backend —— 直接喂给 `createBridge({role, mockBackend})`(= window.__SCVB_MOCK__);
 *   ctl     —— 给 driver 用的控制面(emit / 首帧编排 / 走带同步),**不挂在 backend 上**,
 *              以保证 backend 的键集恰好是「契约函数 + addEventListener」。
 */
export function createMockBackend(options = {}) {
    const { role, world } = options;
    if (role !== "output" && role !== "input") {
        throw new TypeError(
            `createMockBackend:role 必须是 "output" 或 "input",实得 ${JSON.stringify(role)}`,
        );
    }
    if (!world || !world.caps) {
        throw new TypeError(
            "createMockBackend:缺少 world(由 state-driver.js 的 buildWorld() 组装)",
        );
    }
    const ctx = makeContext(role, world);
    const backend =
        role === "output" ? buildOutputBackend(ctx) : buildInputBackend(ctx);
    backend.addEventListener = ctx.addEventListener;
    assertParity(role, backend);
    return { backend, ctl: ctx.ctl };
}
