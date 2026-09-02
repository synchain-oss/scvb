// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— mock 后端冒烟(node,无 DOM)
// =============================================================================
// 跑什么:
//   ① `createBridge({role, mockBackend})` 双侧不 throw,且 backend 键集恰好 =
//      契约 §7 manifest 的函数集 + `addEventListener`(同形自检,契约 §0.7);
//   ② 六个 fixture × 双侧:§0.6 门控(`requestInitialState()` 之前零事件)、
//      首帧各类事件必发(§0.4 第 3 条)、载荷形状、`range.mode` 三值枚举覆盖;
//   ③ 场景化拒绝(只读观察态 / 无宿主循环区 / 通道占用冲突)与参数面 badArg;
//   ④ **重分析保护用户段**(§1.6/§2.8,J34):用户段与锁定段不被重算结果覆盖,
//      `clearManual:true` 时 locked 段仍免疫,`diff.kept` 计数与实际保留数一致;
//   ⑤ **params-v0 范围夹取**(§0.8 第 2 条):越界值夹取后回推,非有限值回 badArg;
//   ⑥ `?scenario=` / `?fixture=` 的回落与 warning;
//   ⑦ **[J83] `participate_in_auto_pan` 默认档**:未显式设置一律 true(含 stereo 轨),
//      显式设置经 `setChannelConfig` 仍然说了算,且 §4.3 Input 只读镜像与 Output 真源同值。
//
// 用法:node web-preview/tests/smoke-mock.mjs [仓库根绝对路径]
//   不给参数就按本脚本位置推仓库根(<repo>/web-preview/tests/ → <repo>)。
// 退出码:0 = 全绿;1 = 有断言失败(逐条打印 [FAIL])。
// =============================================================================

import { readFileSync } from "node:fs"; // [SL-274] 封顶三处同值的源码字面量对拍
import { pathToFileURL, fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const u = (p) => pathToFileURL(join(ROOT, p)).href;

const { createBridge, BRIDGE_FUNCTIONS, BRIDGE_EVENTS, PENDING_FUNCS } =
    await import(u("web/shared/bridge.js"));
const driver = await import(u("web-preview/mock/state-driver.js"));

const FIXTURES = driver.FIXTURES;
let fail = 0;
const log = (...a) => console.log(...a);
function check(cond, msg) {
    if (!cond) {
        fail++;
        console.error("  [FAIL]", msg);
    }
    return cond;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

log("=== ① createBridge 双侧不 throw(mock 覆盖契约全集)===");
for (const role of ["output", "input"]) {
    const s = driver.createPreviewSession({ role, params: "" });
    let bridge = null;
    try {
        bridge = createBridge({ role, mockBackend: s.mock });
    } catch (e) {
        check(false, `createBridge(${role}) threw: ${e.message}`);
    }
    if (bridge) {
        const fns = BRIDGE_FUNCTIONS[role].filter(
            (n) => typeof bridge[n] === "function",
        );
        check(
            fns.length === BRIDGE_FUNCTIONS[role].length,
            `${role} 函数缺失 ${BRIDGE_FUNCTIONS[role].length - fns.length}`,
        );
        let wired = 0;
        for (const ev of BRIDGE_EVENTS[role]) {
            bridge.on(ev, () => {});
            wired++;
        }
        // backend 键集 = 契约函数 + addEventListener(同形断言)
        // + PENDING_FUNCS[role]:走冻结变更流程、native 未落地的待转正名字
        // (见 bridge.js 那张表的头注)。它**必须**留在断言里而不是被过滤掉 ——
        // 名字转正后从 PENDING_FUNCS 挪进 BRIDGE_FUNCTIONS,本行自动跟着改判,
        // 而随手自造的契约外方法照旧会把这条断言打红。
        const keys = Object.keys(s.mock).sort();
        const want = [
            ...BRIDGE_FUNCTIONS[role],
            ...PENDING_FUNCS[role],
            "addEventListener",
        ].sort();
        check(
            JSON.stringify(keys) === JSON.stringify(want),
            `${role} backend 键集与契约不等:多 [${keys.filter((k) => !want.includes(k))}] 缺 [${want.filter((k) => !keys.includes(k))}]`,
        );
        log(
            `  ${role}: 函数 ${fns.length}/${BRIDGE_FUNCTIONS[role].length}, 事件 ${wired}/${BRIDGE_EVENTS[role].length}, isPreview=${bridge.isPreview}`,
        );
    }
    s.stop();
}

log("\n=== ② 六 fixture 冒烟(门控 + 首帧 + 载荷形状)===");
const RANGE_MODES = new Set();
for (const fixture of FIXTURES) {
    for (const role of ["output", "input"]) {
        const seen = new Map();
        const s = driver.createPreviewSession({
            role,
            params: `fixture=${fixture}`,
        });
        const bridge = createBridge({ role, mockBackend: s.mock });
        for (const ev of BRIDGE_EVENTS[role]) {
            bridge.on(ev, (p) => {
                seen.set(ev, (seen.get(ev) || 0) + 1);
                seen.set(ev + ":last", p);
            });
        }
        // 门控断言:requestInitialState 之前不许有事件
        s.start();
        check(
            seen.size === 0,
            `${fixture}/${role} §0.6 门控破了(未 ready 就有事件)`,
        );

        const snap = await bridge.requestInitialState();
        await sleep(120);

        const evs = BRIDGE_EVENTS[role].filter((e) => seen.get(e) > 0);
        if (role === "output") {
            const mode = snap.global.range.mode;
            RANGE_MODES.add(mode);
            check(
                ["follow", "daw_loop", "manual"].includes(mode),
                `${fixture} range.mode 非三值枚举:${mode}`,
            );
            check(
                Array.isArray(snap.channels) && snap.channels.length === 15,
                `${fixture} channels 不是 15`,
            );
            check(snap.versions.length === 2, `${fixture} versions 不是 2`);
            check(
                snap.conn && snap.conn.channels.length === 15,
                `${fixture} conn.channels 不是 15`,
            );
            const st = seen.get("scvb.state:last");
            check(
                st && st.full === true,
                `${fixture} 首帧 state 不是 full:true`,
            );
            const seg = seen.get("scvb.segments:last");
            check(
                seg && seg.reason === "snapshot" && seg.channels.length === 15,
                `${fixture} 首帧 segments 不是全部轨 snapshot`,
            );
            const mt = seen.get("scvb.meters:last");
            check(
                mt && mt.tracks.length === 15 && mt.bus && mt.bus.l && mt.bus.r,
                `${fixture} meters 形状不合法`,
            );
            const ph = seen.get("scvb.playhead:last");
            check(
                ph &&
                    typeof ph.timeS === "number" &&
                    typeof ph.inRange === "boolean",
                `${fixture} playhead 形状不合法`,
            );
            check(
                seen.get("scvb.conn") > 0,
                `${fixture}/${role} 首帧 conn 没发(§0.4 第 3 条)`,
            );
            check(
                seen.get("scvb.groups") > 0,
                `${fixture}/${role} 首帧 groups 没发(§0.4 第 3 条)`,
            );
            const pa = seen.get("scvb.params:last");
            check(
                pa && typeof pa.values === "object",
                `${fixture} params 形状不合法`,
            );
        } else {
            check(
                typeof snap.channel_id === "number" && snap.role === "input",
                `${fixture} input 快照形状不合法`,
            );
            const st = seen.get("scvb.state:last");
            check(
                st &&
                    [
                        "unassigned",
                        "idle",
                        "active",
                        "conflict",
                        "abiMismatch",
                        "srMismatch",
                    ].includes(st.claim),
                `${fixture}/input 首帧 state.claim 非法:${st && JSON.stringify(st.claim)}`,
            );
            check(
                st && Number.isInteger(st.abi),
                `${fixture}/input 首帧 state.abi 非法:${st && JSON.stringify(st.abi)}`,
            );
            check(
                seen.get("scvb.config") > 0,
                `${fixture}/input 首帧 config 没发`,
            );
            check(seen.get("scvb.conn") > 0, `${fixture}/input 首帧 conn 没发`);
        }
        log(
            `  ${fixture.padEnd(16)} ${role.padEnd(6)} 事件 ${evs.length}/${BRIDGE_EVENTS[role].length} [${evs.map((e) => e.replace("scvb.", "") + "×" + seen.get(e)).join(" ")}]`,
        );
        s.stop();
    }
}
check(
    RANGE_MODES.has("follow") &&
        RANGE_MODES.has("daw_loop") &&
        RANGE_MODES.has("manual"),
    `六 fixture 未覆盖三值枚举,实得 ${[...RANGE_MODES]}`,
);
log(`  range.mode 覆盖:${[...RANGE_MODES].join(" / ")}`);

// -----------------------------------------------------------------------------
// 会话小工具:起周期事件 + 解门控,回调里拿 bridge / session / 事件账本
// -----------------------------------------------------------------------------
async function withSession(role, params, fn) {
    const s = driver.createPreviewSession({ role, params });
    const b = createBridge({ role, mockBackend: s.mock });
    const seen = new Map();
    for (const ev of BRIDGE_EVENTS[role]) {
        b.on(ev, (p) => {
            seen.set(ev, (seen.get(ev) || 0) + 1);
            seen.set(ev + ":last", p);
        });
    }
    s.start();
    await b.requestInitialState();
    const r = await fn(b, seen, s);
    s.stop();
    return r;
}

log("\n=== ③ 场景化拒绝行为 ===");

// second-output:写函数回 observer
await withSession("output", "fixture=second-output", async (b) => {
    const rows = [
        ["setChannelConfig", await b.setChannelConfig(1, { priority: 3 })],
        ["setTrackManual", await b.setTrackManual(1, "pan", 20)],
        [
            "editSegment",
            await b.editSegment(1, "set_locked", { segIdx: 0, locked: true }),
        ],
        ["clearCoverage", await b.clearCoverage(1, 1, 5)],
        ["setGroupId", await b.setGroupId(2)],
        ["recaptureArm", await b.recaptureArm(1, 1, 5)],
    ];
    for (const [n, r] of rows) {
        const good =
            n === "recaptureArm"
                ? r.reason === "readOnly"
                : r.observer === true;
        check(good, `second-output ${n} 未拒绝:${JSON.stringify(r)}`);
        log(`  second-output ${n.padEnd(18)} → ${JSON.stringify(r)}`);
    }
});

// stereo-mixed&loop=none:daw_loop → noLoop
await withSession("output", "fixture=stereo-mixed&loop=none", async (b) => {
    const r = await b.setRange("daw_loop", 0, 0);
    check(
        r.ok === false && r.reason === "noLoop",
        `loop=none 下 daw_loop 未回 noLoop:${JSON.stringify(r)}`,
    );
    log(`  stereo-mixed&loop=none setRange("daw_loop") → ${JSON.stringify(r)}`);
    const r2 = await b.setRange("manual", 10, 5);
    check(
        r2.reason === "badArg",
        `manual 起>=止 未回 badArg:${JSON.stringify(r2)}`,
    );
    log(`  stereo-mixed setRange("manual",10,5) → ${JSON.stringify(r2)}`);
});
// 有 loop 的 fixture:daw_loop 成功
await withSession("output", "fixture=second-output", async (b) => {
    const r = await b.setRange("daw_loop", 0, 0);
    check(r.ok === true, `second-output daw_loop 应成功:${JSON.stringify(r)}`);
    log(`  second-output setRange("daw_loop") → ${JSON.stringify(r)}`);
});

// channel-conflict:Input setChannelId 撞占位
await withSession("input", "fixture=channel-conflict", async (b) => {
    const r = await b.setChannelId(4);
    check(
        r.conflict === true,
        `channel-conflict setChannelId 未回 conflict:${JSON.stringify(r)}`,
    );
    log(`  channel-conflict setChannelId(4) → ${JSON.stringify(r)}`);
});
await withSession("input", "fixture=empty", async (b) => {
    const r = await b.setChannelId(4);
    check(
        r.ok === true,
        `empty fixture setChannelId 应成功:${JSON.stringify(r)}`,
    );
    log(`  empty setChannelId(4) → ${JSON.stringify(r)}`);
    const p = await b.remoteSetPriority(7);
    log(`  empty remoteSetPriority(7) → ${JSON.stringify(p)}`);
});

// PRINT 态硬拒绝 + 参数面 badArg + undo/redo + 波形
await withSession("output", "fixture=fifteen-tracks", async (b) => {
    await b.setOutputEnabled(true);
    const r = await b.setVersionActive(2);
    log(`  PRINT 态 setVersionActive(2) → ${JSON.stringify(r)}`);
    check(
        r.rejected === "printing" || r.ok === true,
        `setVersionActive 返回形状非法:${JSON.stringify(r)}`,
    );
    const bad = await b.setParam("v1_t01_pan", 10);
    check(
        bad.reason === "badArg",
        `轨 pan 不在 gesture 通道,应回 badArg:${JSON.stringify(bad)}`,
    );
    log(
        `  setParam("v1_t01_pan") → ${JSON.stringify(bad)}(pan/vol 不在本通道)`,
    );
    const okp = await b.setParam("v1_t01_width", 80);
    check(okp.ok === true, `setParam width 应成功:${JSON.stringify(okp)}`);
    check((await b.undo()).ok === false, "undo 应回 {ok:false}");
    check((await b.redo()).ok === false, "redo 应回 {ok:false}");
    const wf = await b.requestWaveform(1, 0, 10, 64);
    check(
        Array.isArray(wf.minDb) &&
            wf.minDb.length === 64 &&
            Array.isArray(wf.valleys),
        "requestWaveform 形状不合法",
    );
    const wfBad = await b.requestWaveform(99, 0, 10, 64);
    check(wfBad.reason === "badArg", "requestWaveform 越界应回 badArg");
    const an = await b.analyze("all");
    check(
        an.ok === true && an.affected,
        `analyze 受理回执形状:${JSON.stringify(an)}`,
    );
    log(`  analyze("all") → ${JSON.stringify(an)}`);
});

// -----------------------------------------------------------------------------
// ④ 重分析保护用户段(§1.6 / §2.8,J34)
// -----------------------------------------------------------------------------

const CH = 1;
const CH_MASK = 1 << (CH - 1);
/** 取最近一帧 scvb.segments 里某轨的段表。 */
const segsOf = (seen, ch) => {
    const f = seen.get("scvb.segments:last");
    const e = f && f.channels.find((c) => c.ch === ch);
    return e ? e.segments : [];
};
/**
 * 「逐字节保留」的比较口径:除 `segIdx` 外全字段相等。
 * `segIdx` 按 §2.8 字段纪律**每次事件后重新编号**,把它算进去等于要求契约不许重编号。
 */
const idless = (s) => JSON.stringify({ ...s, segIdx: null });

/** 造两条用户段:idx0 = user_edited+locked;idx1 = user_edited 但已解锁。 */
async function seedUserSegments(b, seen) {
    check(
        (await b.editSegment(CH, "set_values", { segIdx: 0, pan: 33.3 })).ok ===
            true,
        "前置:set_values(idx0) 应成功",
    );
    check(
        (await b.editSegment(CH, "set_values", { segIdx: 1, volDb: -6.5 }))
            .ok === true,
        "前置:set_values(idx1) 应成功",
    );
    check(
        (await b.editSegment(CH, "set_locked", { segIdx: 1, locked: false }))
            .ok === true,
        "前置:set_locked(idx1,false) 应成功",
    );
    const before = segsOf(seen, CH);
    check(before.length >= 3, `前置:ch${CH} 段数太少(${before.length})`);
    check(
        before[0].origin === "user_edited" && before[0].locked === true,
        `前置:idx0 应是 user_edited+locked,实得 ${JSON.stringify(before[0])}`,
    );
    check(
        before[1].origin === "user_edited" && before[1].locked === false,
        `前置:idx1 应是 user_edited 且未锁定,实得 ${JSON.stringify(before[1])}`,
    );
    return before;
}

log("\n=== ④ 重分析保护用户段(§1.6/§2.8,J34)===");

// clearManual:false —— 用户段 ∪ 锁定段逐字节保留
await withSession(
    "output",
    "fixture=fifteen-tracks&play=0",
    async (b, seen) => {
        const before = await seedUserSegments(b, seen);
        const wantKept = before.filter(
            (s) => s.origin !== "auto" || s.locked === true,
        ).length;

        const pre = await b.previewAnalyze({ tracksMask: CH_MASK });
        check(
            pre.manualKept === wantKept,
            `previewAnalyze.manualKept 应与真跑一致:预览 ${pre.manualKept} vs 实际 ${wantKept}`,
        );

        const r = await b.analyze(
            { tracksMask: CH_MASK },
            { clearManual: false },
        );
        check(r.ok === true, `analyze 应受理:${JSON.stringify(r)}`);
        await sleep(900);

        const frame = seen.get("scvb.segments:last");
        check(
            frame && frame.reason === "analyze",
            `重算后应收到 reason:"analyze" 帧,实得 ${frame && frame.reason}`,
        );
        const after = segsOf(seen, CH);
        check(
            after.some((s) => idless(s) === idless(before[0])),
            "clearManual:false —— 锁定的用户段应逐字节保留",
        );
        check(
            after.some((s) => idless(s) === idless(before[1])),
            "clearManual:false —— 未锁定的用户段(origin≠auto)也应逐字节保留",
        );
        check(
            frame.diff.kept === wantKept,
            `clearManual:false 的 diff.kept 应 = ${wantKept},实得 ${frame.diff.kept}`,
        );
        // 合并后:按 t0S 有序 + segIdx 0 基重编号(§2.8 字段纪律)
        check(
            after.every((s, i) => s.segIdx === i),
            "合并后 segIdx 未按 0 基重编号",
        );
        check(
            after.every((s, i) => i === 0 || after[i - 1].t0S <= s.t0S),
            "合并后段表未按 t0S 排序",
        );
        check(
            after.every((s, i) => i === 0 || after[i - 1].t1S <= s.t0S),
            "合并后仍有段在时间上重叠(保留段与新算出的 auto 段抢同一区间)",
        );
        log(
            `  clearManual:false → 段 ${before.length}→${after.length},diff.kept=${frame.diff.kept}(应 ${wantKept}),用户段原样保留`,
        );
    },
);

// clearManual:true —— origin 重置参与重算,但 locked 段仍免疫
await withSession(
    "output",
    "fixture=fifteen-tracks&play=0",
    async (b, seen) => {
        const before = await seedUserSegments(b, seen);
        const wantKept = before.filter((s) => s.locked === true).length;
        const unlockedUser = before[1];

        const r = await b.analyze(
            { tracksMask: CH_MASK },
            { clearManual: true },
        );
        check(
            r.ok === true,
            `analyze(clearManual:true) 应受理:${JSON.stringify(r)}`,
        );
        await sleep(900);

        const frame = seen.get("scvb.segments:last");
        const after = segsOf(seen, CH);
        check(
            after.some((s) => idless(s) === idless(before[0])),
            "clearManual:true —— locked 段仍应免疫(须先逐段解锁,04 §4.4)",
        );
        check(
            !after.some((s) => idless(s) === idless(unlockedUser)),
            "clearManual:true —— 未锁定的用户段应被重算结果换掉",
        );
        const sameSpan = after.find((s) => s.t0S === unlockedUser.t0S);
        check(
            sameSpan && sameSpan.origin === "auto" && sameSpan.locked === false,
            `clearManual:true —— 该位置应换成 auto 未锁定段,实得 ${JSON.stringify(sameSpan)}`,
        );
        check(
            frame.diff.kept === wantKept,
            `clearManual:true 的 diff.kept 应 = 锁定段数 ${wantKept},实得 ${frame.diff.kept}`,
        );
        check(
            wantKept ===
                before.filter((s) => s.origin !== "auto" || s.locked === true)
                    .length -
                    1,
            "clearManual:true 的保留集应恰好比 false 档少掉那条已解锁的用户段",
        );
        log(
            `  clearManual:true  → diff.kept=${frame.diff.kept}(应 ${wantKept}),locked 免疫、解锁的用户段被重算`,
        );
    },
);

// -----------------------------------------------------------------------------
// ⑤ params-v0 范围夹取(§0.8 第 2 条 / §1.13 可驱动 ParamID 全集)
// -----------------------------------------------------------------------------

log("\n=== ⑤ setParam 夹取(params-v0 范围)===");
await withSession(
    "output",
    "fixture=fifteen-tracks&play=0",
    async (b, seen) => {
        const cases = [
            ["width", 9999, 150],
            ["width", -50, 0],
            ["ms_balance", 250, 100],
            ["ms_balance", -250, -100],
            ["lead_select", 7.6, 8], // int 档:先取整再夹取
            ["lead_select", 99, 15],
            ["v1_t01_width", 9999, 100], // 每轨 width 只到 100(全局那件才到 150)
            ["v1_t01_freeze", 9, 3],
            ["v1_t01_freeze", 1.4, 1],
        ];
        for (const [id, sent, want] of cases) {
            const r = await b.setParam(id, sent);
            check(
                r.ok === true,
                `setParam(${id}, ${sent}) 应受理:${JSON.stringify(r)}`,
            );
            const got = seen.get("scvb.params:last").values[id];
            check(
                got === want,
                `setParam(${id}, ${sent}) 应夹取回推 ${want},实得 ${got}`,
            );
        }
        log(`  ${cases.length} 条越界值全部按 params-v0 范围夹取并回推`);

        for (const bad of [NaN, Infinity, "80", null, undefined]) {
            const r = await b.setParam("width", bad);
            check(
                r.ok === false && r.reason === "badArg",
                `setParam("width", ${JSON.stringify(bad)}) 应回 badArg,实得 ${JSON.stringify(r)}`,
            );
        }
        // 原型链上的键不是可写 ParamID(hasOwnProperty 口径)
        for (const id of ["constructor", "toString", "__proto__"]) {
            const r = await b.setParam(id, 1);
            check(
                r.ok === false && r.reason === "badArg",
                `setParam(${JSON.stringify(id)}) 应回 badArg,实得 ${JSON.stringify(r)}`,
            );
            const g = await b.beginParamGesture(id);
            check(
                g.ok === false && g.reason === "badArg",
                `beginParamGesture(${JSON.stringify(id)}) 应回 badArg,实得 ${JSON.stringify(g)}`,
            );
        }
        log('  非有限值 / 原型链键一律 {ok:false,reason:"badArg"}');
    },
);

// -----------------------------------------------------------------------------
// ⑥ ?scenario= / ?fixture= 回落
// -----------------------------------------------------------------------------

log("\n=== ⑥ 查询参数回落 ===");
{
    const cases = [
        ["scenario=printing", "fifteen-tracks", true],
        ["scenario=misaligned", "misaligned", false],
        // 非法 fixture 不吃掉同时给出的合法 scenario
        ["fixture=nope&scenario=misaligned", "misaligned", true],
        ["fixture=nope", "fifteen-tracks", true],
        // 原型链上的键不是场景名
        ["scenario=constructor", "fifteen-tracks", true],
    ];
    for (const [params, wantFixture, wantWarn] of cases) {
        const s = driver.createPreviewSession({ role: "output", params });
        check(
            s.info.fixture === wantFixture,
            `?${params} 应回落 ${wantFixture},实得 ${s.info.fixture}`,
        );
        check(
            s.info.warnings.length > 0 === wantWarn,
            `?${params} 的 warning 与预期不符:${JSON.stringify(s.info.warnings)}`,
        );
        log(`  ?${params.padEnd(30)} → fixture=${s.info.fixture}`);
        s.stop();
    }
}

// -----------------------------------------------------------------------------
// ⑦ [J83] participate_in_auto_pan 默认档 —— 未显式设置一律 true
// -----------------------------------------------------------------------------
//
// source_channels 来自 `getMainBusNumInputChannels()`,是**轨道总线布局**不是素材声道数
// (单声道人声放在立体声轨上就报 2)。按它推导默认档,真机上绝大多数人声轨会被判成
// 「不参与自动声像」→ AutoAssign 按「保持现值」处理 → pan 全 0 烘焙进段表(v5.1 实测 P0-B)。
// 改回 `sourceChannels === 1` 时,下面 source_channels=2 与「不给该键」两档即红。
// 变更文档:docs/contract-changes/20260826-j83-participate-default.md。

log("\n=== ⑦ [J83] participate_in_auto_pan 默认档 ===");
{
    const MD = await import(u("web/shared/mock-data.js"));

    // 空快照:15 轨 mono、谁都没设过 → 全部参与。
    const base = MD.makeOutputSnapshot();
    check(
        base.channels.length === 15 &&
            base.channels.every(
                (c) =>
                    c.source_channels === 1 &&
                    c.participate_in_auto_pan === true,
            ),
        "空快照 15 轨(mono)应全部默认参与",
    );
    log("  makeOutputSnapshot:15 轨 mono 默认 participate=true");

    // tour demo:四条 stereo 轨(DEMO_STEREO_CHANNELS)**不给** participate 键,
    // 走的正是 makeChannelConfig 的默认档 —— 改回 `source_channels === 1` 推导即红。
    const demo = MD.makeTourDemoSnapshot();
    const stereo = MD.DEMO_STEREO_CHANNELS;
    check(stereo.length > 0, "tour demo 应有 stereo 轨,否则本组断言形同虚设");
    for (const ch of stereo) {
        const c = demo.channels[ch - 1];
        check(
            c.source_channels === 2 && c.participate_in_auto_pan === true,
            `tour demo ch${ch}(stereo)应默认参与,实得 participate=${c.participate_in_auto_pan}`,
        );
    }
    check(
        demo.channels.every((c) => c.participate_in_auto_pan === true),
        "tour demo 15 轨应全部默认参与",
    );
    log(
        `  tour demo:15 轨全部 participate=true(含 stereo 轨 ${stereo.join("/")})`,
    );

    // Input 侧 §4.3 只读快照的默认档同口径(C++ 侧 buildConfigPayload 的降级路径)。
    check(
        MD.makeInputConfig().participate_in_auto_pan === true &&
            MD.makeInputSnapshot().config.participate_in_auto_pan === true,
        "Input §4.3 config 默认应参与",
    );
    log("  makeInputConfig / makeInputSnapshot 默认 participate=true");
}

// 显式设置仍然说了算 —— 默认档变了不等于用户关不掉。
// **走 mock 桥的 setChannelConfig**(§1.15 配置唯一写入点)而不是直接给生成器塞 patch:
// `mergeDeep` 对数组是**整体替换**,给 `makeTourDemoSnapshot({channels:[...]})` 传一份改好的
// 数组时 `makeChannelConfig` 一次都不会被调用 —— 那样的断言只验证「传进去的东西还在」,
// 把默认值改成任何常量它都照绿(PR #105 claude-review 建议 2)。
{
    const MD = await import(u("web/shared/mock-data.js"));
    // 轨号从 demo 数据推,不写死:写死的话 demo 轨画像一改,这条就会静默地去测一条 mono 轨
    // 而照样全绿(pr-agent 建议)。写前先确认它真是 stereo —— 本组要守的正是「stereo 轨也默认参与」。
    const ch = MD.DEMO_STEREO_CHANNELS[0];
    await withSession("output", "fixture=fifteen-tracks", async (b) => {
        const before = await b.requestInitialState();
        check(
            before.channels[ch - 1].source_channels === 2 &&
                before.channels[ch - 1].participate_in_auto_pan === true,
            `写之前 ch${ch}(stereo)应是默认参与`,
        );
        const r = await b.setChannelConfig(ch, {
            participate_in_auto_pan: false,
        });
        check(r.ok === true, `setChannelConfig 应成功:${JSON.stringify(r)}`);
        const after = await b.requestInitialState();
        check(
            after.channels[ch - 1].participate_in_auto_pan === false,
            "显式 false 必须压过默认档并回推",
        );
        check(
            after.channels.filter((c) => c.participate_in_auto_pan === false)
                .length === 1,
            "显式关闭只影响被写的那一轨",
        );
        log(
            `  经 setChannelConfig 显式关闭 ch${ch} → 只有该轨 false,其余仍 true`,
        );
    });

    // stereo-mixed fixture:Output 真源与 Input §4.3 只读镜像必须同值。
    // 只改一侧会造出真机上不可能出现的组合(见 state-driver.js 该分支的注释)。
    const outSnap = await withSession("output", "fixture=stereo-mixed", (b) =>
        b.requestInitialState(),
    );
    const inSnap = await withSession("input", "fixture=stereo-mixed", (b) =>
        b.requestInitialState(),
    );
    const mirrorCh = inSnap.channel_id;
    check(
        outSnap.channels[mirrorCh - 1].participate_in_auto_pan === false &&
            inSnap.config.participate_in_auto_pan === false,
        `stereo-mixed 两侧不同值:Output=${outSnap.channels[mirrorCh - 1].participate_in_auto_pan} ` +
            `Input=${inSnap.config.participate_in_auto_pan}`,
    );
    log(`  stereo-mixed:ch${mirrorCh} 两侧同为 false(§4.3 只读镜像自洽)`);
}

// ---------------------------------------------------------------------------
// [SL-274] diff.changed 的每一条都必须是**幅度值得一提**的改动。
//
// native 侧 `src/core/output/SegmentDiff.h::changedAtDisplayPrecision` 只登记
// 「量化到 1 位小数后不同 **且** 幅度 >= 半个显示步长」的段 —— 这一条是本卡的修复:
// 用户 v5.6.5 实测「摘要弹出全轨全段」,里头大半是贴着量化边界、幅度小到不值一提的条目
// (它们屏幕上的数字**确实变了**,滤掉是因为变化量太小,不是因为「看不见」)。
// mock 若发得出 native 发不出的东西,页面级冒烟看到的就不是真机会有的画面。
//
// **为什么断在这里、而不是在 mock 里加一段过滤**:实测两条路径(默认档 232 条 /
// 满档 1600 条),逐条 max(|dPan|,|dVol|) 的最小值分别是 0.2 / 0.1 —— 都 >= 一整档,
// 过滤代码一条都滤不掉,那是永不触发的
// 死判据,删掉它不会有任何用例变红。真正决定这件事的是 panJitter / volJitter 的量级,
// 所以把约束写成断言:哪天有人把抖动调小到能产生亚显示精度的改动,这里立刻红,
// 逼人当场决定「改抖动还是给 mock 加过滤」,而不是被一段静默过滤盖过去。
// ⚠ **两条路径都要扫**([SL-274] 复审第 2 轮【重要】):`diffFillToCap` 拿掉的正是
// `% 17` 那道抽稀,于是 diff-flood 下进表的是**另一批段** —— 它们的 panJitter/volJitter
// 落点不受默认档那批的断言覆盖。`unit()` 是种子函数、完全确定,所以「有没有一条两维
// 都低于闸门」这件事要么已经发生要么永不发生;不扫就等于不知道是哪一种。
log("");
log(
    "=== [SL-274] mock 的 diff.changed 与 native 判据同口径(默认档 + diff-flood)===",
);
{
    const md = await import(u("web/shared/mock-data.js"));
    const GATE = 0.05; // 半个显示步长(native 的 kChangeAmplitudeGate)
    for (const fill of [false, true]) {
        let seen = 0;
        let invisible = 0;
        // 报「逐条两维取大者」的最小值 —— 那才是断言真正问的那个量。
        // (不报最小 |dPan|:满档下有整批 pan 一动不动、只有 volDb 变的条目,
        //  那批完全合法 —— native 的判据本来就是两维取「或」。)
        let minGated = Infinity;
        for (const reason of ["vad", "segmentation", "analyze", "edit"]) {
            for (let v = 1; v <= 2; v++) {
                const frame = md.makeSegments(v, reason, undefined, {
                    diffFillToCap: fill,
                });
                for (const c of frame.diff.changed || []) {
                    seen++;
                    const dp = Math.abs(c.panTo - c.panFrom);
                    const dv = Math.abs(c.volDbTo - c.volDbFrom);
                    minGated = Math.min(minGated, Math.max(dp, dv));
                    if (dp < GATE && dv < GATE) invisible++;
                }
            }
        }
        const path = fill ? "diff-flood(满档)" : "默认档";
        // 下界按路径分开:默认档 8 帧 × 29 条 = 232,满档 8 帧 × 200 条 = 1600。
        // 写死「>100」对满档太松,会盖不住「封顶哪天被调小到 20」这种退化。
        const floorN = fill ? 1000 : 100;
        check(
            seen > floorN,
            `[${path}] 样本量够大(实得 ${seen} 条,应 >${floorN})`,
        );
        check(
            invisible === 0,
            `[${path}] 每条 changed 至少有一维过得了 native 的幅度闸门 ${GATE}` +
                `(实得 ${invisible} 条两维都低于闸门 —— native 不会发这种条目,` +
                "mock 也不该发:要么把 panJitter/volJitter 调回大幅度,要么给 mock 补过滤)",
        );
        log(
            `  [${path}] ${seen} 条 changed,零条两维皆亚闸门;` +
                `逐条 max(|dPan|,|dVol|) 的最小值 = ${minGated.toFixed(4)}`,
        );
    }
}

// ---------------------------------------------------------------------------
// [SL-274] `changed[]` 的封顶 **200** 是三处同值,这里给它上机器门禁。
//
// 三处:native `src/core/output/SegmentDiff.h::kMaxChangedItems`、
// web `web/output/tab-wave.js::DIFF_CHANGED_CAP`、mock `web/shared/mock-data.js` 的字面量。
// 改前只有注释在绑三者(「改一处要三处一起改」),而本仓对单一真源一向是上门禁的
// (`.juce-version`、`check-bridge-parity`、`check-*-parity`)—— 注释绑不住,人会漏。
//
// 为什么这条**必须**存在:web 侧拿 `DIFF_CHANGED_CAP` 判「这一帧顶到封顶没有」,顶到就把
// 计数渲染成「200+」(它是下界,不是总数)。三者一旦不同值,那个判断就会在错误的点翻面 ——
// native 截到 200、web 以为封顶是 500 ⇒ 屏幕上印「200」,把「至少 200 段改了」说成
// 「正好 200 段」。这正是本卡在 tab-wave.js 那段头注里说的「这一行唯一可能撒的谎」。
//
// 读源码字面量而不是 import:native 是 C++ 头文件,只能正则;三处用同一种读法,
// 免得哪天 web 侧改成动态计算而这条门禁还以为自己在对拍。
log("");
log("=== [SL-274] changed[] 封顶三处同值(native / web / mock)===");
{
    const grab = (relPath, re, what) => {
        const src = readFileSync(join(ROOT, relPath), "utf8");
        const m = src.match(re);
        check(!!m, `${what}:在 ${relPath} 里找得到那个常量`);
        return m ? Number(m[1]) : NaN;
    };
    const nativeCap = grab(
        "src/core/output/SegmentDiff.h",
        /kMaxChangedItems\s*=\s*(\d+)/,
        "native kMaxChangedItems",
    );
    const webCap = grab(
        "web/output/tab-wave.js",
        /DIFF_CHANGED_CAP\s*=\s*(\d+)/,
        "web DIFF_CHANGED_CAP",
    );
    const mockCap = grab(
        "web/shared/mock-data.js",
        /changed\.length\s*<\s*(\d+)/,
        "mock changed 封顶",
    );
    check(
        nativeCap === webCap && webCap === mockCap,
        `三处同值(native ${nativeCap} / web ${webCap} / mock ${mockCap}) —— ` +
            "任一处单独改动都会让「顶到封顶就渲染成 N+」在错误的点翻面",
    );
    // 三个值都打出来:红的时候日志要直接说清是哪一处跑偏了,别再让人回去翻源码。
    log(`  native ${nativeCap} / web ${webCap} / mock ${mockCap}`);
}

log(`\n=== 结果:${fail === 0 ? "全部通过" : fail + " 项失败"} ===`);
process.exit(fail === 0 ? 0 : 1);
