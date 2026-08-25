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
//   ⑤ **params-v0 范围夹取**(§0.8 第 2 条):越界值夹取后回推,非有限值回 badArg。
//
// 用法:node web-preview/tests/smoke-mock.mjs [仓库根绝对路径]
//   不给参数就按本脚本位置推仓库根(<repo>/web-preview/tests/ → <repo>)。
// 退出码:0 = 全绿;1 = 有断言失败(逐条打印 [FAIL])。
// =============================================================================

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

log(`\n=== 结果:${fail === 0 ? "全部通过" : fail + " 项失败"} ===`);
process.exit(fail === 0 ? 0 : 1);
