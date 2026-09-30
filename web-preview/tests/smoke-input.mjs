// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Input 单页冒烟(node,无 DOM)
// =============================================================================
// 跑什么(T36 Input 七态 + 交互):
//   ① ?scenario=occupied|no-output|connected|passthrough|abi-mismatch|sr-mismatch|
//      group-mismatch 七态:scvb.state.claim / scvb.conn / scvb.config /
//      scvb.groups 载荷形状与语义正确(abi-mismatch 带 abi_remote);
//   ② setChannelId 冲突(occupied)→ {conflict:true} + channelConflict error;
//      释放(n=0)/ 成功(n 空闲)→ {ok:true} 且 claim 随 outputOnline 派生;
//   ③ setGroupId 冲突(新组同 channel 被占)→ {conflict:true};成功 → {ok:true};
//   ⑤ sourceKind 口径:source_channels 0/undefined→unmeasured,1→mono,2→stereo。
//   ④ remoteSetPriority:未分配 → {queued:false,reason:"unassigned"},
//      Output 离线 → {queued:false,reason:"outputOffline"},在线 → {queued:true}。
//   ⑥ [SL-463 / J156] 非冲突失败回执(契约 §3.2/§3.3):claim-unavailable / claim-abi-mismatch
//      两个场景下 setChannelId / setGroupId 回 {ok:false, reason},不回 {ok:true};
//      状态按真桥的形状走(换通道回滚 ⇒ 原通道不变;首次选通道 ⇒ channel_id 0)。
//
// 用法:node web-preview/tests/smoke-input.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有断言失败(逐条打印 [FAIL])。
// =============================================================================

import { pathToFileURL, fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";
import { awaitSeen } from "./lib/await-state.mjs";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const u = (p) => pathToFileURL(join(ROOT, p)).href;

const { createBridge } = await import(u("web/shared/bridge.js"));
const { sourceKind } = await import(u("web/shared/source-kind.js"));
const driver = await import(u("web-preview/mock/state-driver.js"));
const mock = await import(u("web-preview/mock/juce-bridge-mock.js"));

let fail = 0;
const log = (...a) => console.log(...a);
function check(cond, msg) {
    if (!cond) {
        fail++;
        console.error("  [FAIL]", msg);
    }
    return cond;
}

/** 起 Input 会话 + 事件账本;回调里拿 bridge / seen / session。 */
async function withInput(params, fn) {
    const s = driver.createPreviewSession({ role: "input", params });
    const b = createBridge({ role: "input", mockBackend: s.mock });
    const seen = new Map();
    for (const ev of [
        "scvb.state",
        "scvb.conn",
        "scvb.config",
        "scvb.groups",
        "scvb.error",
    ]) {
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

log("=== ① Input 七态(scenario → claim / conn / config / groups)===");

const SEVEN = [
    ["connected", "active", true, false],
    ["no-output", "idle", false, true],
    ["passthrough", "idle", true, true],
    ["abi-mismatch", "abiMismatch", true, true],
    ["sr-mismatch", "srMismatch", true, true],
    ["group-mismatch", "idle", false, true],
    ["occupied", "conflict", true, true],
];
for (const [scenario, wantClaim, wantOutputOnline, wantPassthrough] of SEVEN) {
    await withInput("scenario=" + scenario, async (b, seen) => {
        const st = seen.get("scvb.state:last");
        const conn = seen.get("scvb.conn:last");
        check(
            st && st.claim === wantClaim,
            `${scenario}:claim 应 ${wantClaim},实得 ${st && st.claim}`,
        );
        check(
            conn && conn.outputOnline === wantOutputOnline,
            `${scenario}:conn.outputOnline 应 ${wantOutputOnline},实得 ${conn && conn.outputOnline}`,
        );
        check(
            conn && conn.passthrough === wantPassthrough,
            `${scenario}:conn.passthrough 应 ${wantPassthrough},实得 ${conn && conn.passthrough}`,
        );
        check(
            seen.get("scvb.config:last") != null,
            `${scenario}:首帧 scvb.config 缺失`,
        );
        check(
            seen.get("scvb.groups:last") != null,
            `${scenario}:首帧 scvb.groups 缺失`,
        );
        log(
            `  ${scenario.padEnd(14)} claim=${st && st.claim} outputOnline=${conn && conn.outputOnline} passthrough=${conn && conn.passthrough} groups=${(seen.get("scvb.groups:last") || {}).groups_online}`,
        );
    });
}

log("\n=== ①b abi-mismatch 两端 abi 数字(scvb.state.abi / abi_remote)===");
await withInput("scenario=abi-mismatch", async (b, seen) => {
    const st = seen.get("scvb.state:last");
    check(
        st && Number.isInteger(st.abi) && Number.isInteger(st.abi_remote),
        `abi-mismatch 应带 abi/abi_remote 整数: ${JSON.stringify(st && { abi: st.abi, abi_remote: st.abi_remote })}`,
    );
    check(
        st && st.abi !== st.abi_remote,
        `abi-mismatch 两端 abi 应不同: abi=${st && st.abi} abi_remote=${st && st.abi_remote}`,
    );
    log(`  abi=${st.abi} abi_remote=${st.abi_remote}(本机 ≠ 对端)`);
});

log("\n=== ①c group-mismatch:本组无 Output、异组在线、非默认组===");
await withInput("scenario=group-mismatch", async (b, seen) => {
    const st = seen.get("scvb.state:last");
    const groups = seen.get("scvb.groups:last");
    check(
        st && st.group_id === 2,
        `group-mismatch group_id 应 2,实得 ${st && st.group_id}`,
    );
    check(
        groups && groups.groups_online === 1,
        `group-mismatch groups_online 应只有组 A(bit0),实得 ${groups && groups.groups_online}`,
    );
    log(
        `  group_id=${st.group_id} groups_online=${groups.groups_online}(组 A 在线,本组 B 无 Output)`,
    );
});

log("\n=== ② setChannelId 冲突 / 成功 / 释放 ===");
await withInput("scenario=occupied", async (b, seen) => {
    const r = await b.setChannelId(4);
    check(
        r.conflict === true,
        `occupied setChannelId(4) 应 conflict,实得 ${JSON.stringify(r)}`,
    );
    check(
        seen.get("scvb.error:last") &&
            seen.get("scvb.error:last").code === "channelConflict",
        "occupied 冲突应推 scvb.error{channelConflict}",
    );
    log(`  setChannelId(4) → ${JSON.stringify(r)}`);
});
await withInput("scenario=no-output", async (b) => {
    // no-output 场景 ch1 已被本实例占(channel_id=1),释放后再选新通道应成功
    const rel = await b.setChannelId(0);
    check(
        rel.ok === true,
        `setChannelId(0) 释放应 {ok:true},实得 ${JSON.stringify(rel)}`,
    );
    const r = await b.setChannelId(3);
    check(
        r.ok === true,
        `释放后 setChannelId(3) 应成功,实得 ${JSON.stringify(r)}`,
    );
    log(
        `  setChannelId(0) → ${JSON.stringify(rel)}; setChannelId(3) → ${JSON.stringify(r)}`,
    );
});
// 释放 → 重选原通道 往返(§3.2;occupiedMask 位变化正确)
await withInput("scenario=connected", async (b, seen) => {
    const rel = await b.setChannelId(0);
    check(rel.ok === true, `释放应 {ok:true},实得 ${JSON.stringify(rel)}`);
    const afterRelease = seen.get("scvb.conn:last");
    check(
        afterRelease && ((afterRelease.occupiedMask >>> 0) & 1) === 0,
        `释放后 ch1 位应清,实得 ${afterRelease && afterRelease.occupiedMask}`,
    );
    const r = await b.setChannelId(1);
    check(
        r.ok === true,
        `释放后重选原通道 ch1 应成功,实得 ${JSON.stringify(r)}`,
    );
    const afterClaim = seen.get("scvb.conn:last");
    check(
        afterClaim && ((afterClaim.occupiedMask >>> 0) & 1) === 1,
        `重选后 ch1 位应置,实得 ${afterClaim && afterClaim.occupiedMask}`,
    );
    log(
        `  释放→重选 ch1: ${JSON.stringify(rel)} → ${JSON.stringify(r)}, occupiedMask=${afterClaim && afterClaim.occupiedMask}`,
    );
});

log("\n=== ③ setGroupId 成功 / 冲突 ===");
await withInput("scenario=connected", async (b, seen) => {
    const r = await b.setGroupId(2);
    check(
        r.ok === true,
        `setGroupId(2) 应 {ok:true},实得 ${JSON.stringify(r)}`,
    );
    // [SL-357] 状态回声默认异步 —— 等那一帧再读,别在写回执那一刻读(读到的是旧值)。
    const st = await awaitSeen(
        seen,
        "scvb.state",
        (v) => v.group_id === 2,
        "切组后的 scvb.state",
    );
    check(
        st && st.group_id === 2,
        `切组后 group_id 应 2,实得 ${st && st.group_id}`,
    );
    check(
        st && st.claim === "active",
        `connected(maskBit=1) 切组后 claim 应 active,实得 ${st && st.claim}`,
    );
    log(`  setGroupId(2) → ${JSON.stringify(r)}`);
});
// claimStateFor(§5.2):active 需 outputOnline && maskBit;maskBit=0 → idle
await withInput("scenario=passthrough", async (b, seen) => {
    const r = await b.setGroupId(2);
    check(
        r.ok === true,
        `passthrough setGroupId(2) 应 {ok:true},实得 ${JSON.stringify(r)}`,
    );
    const st = seen.get("scvb.state:last");
    check(
        st && st.claim === "idle",
        `passthrough(maskBit=0) 切组后 claim 应 idle,实得 ${st && st.claim}`,
    );
    log(`  passthrough setGroupId(2) → claim=${st && st.claim}`);
});
// 新组同 channel 被占(groupConflict cap)→ 冲突
{
    const world = driver.buildWorld({
        role: "input",
        fixture: "fifteen-tracks",
    });
    world.caps.groupConflict = true;
    const { backend } = mock.createMockBackend({ role: "input", world });
    const b = createBridge({ role: "input", mockBackend: backend });
    const seen = new Map();
    b.on("scvb.error", (p) => seen.set("scvb.error:last", p));
    await b.requestInitialState(); // §0.6 门控:先解 ready,后续 error 事件才会派发
    const r = await b.setGroupId(2);
    check(
        r.conflict === true,
        `groupConflict 下 setGroupId(2) 应 {conflict:true},实得 ${JSON.stringify(r)}`,
    );
    check(
        seen.get("scvb.error:last") &&
            seen.get("scvb.error:last").code === "channelConflict",
        "group 冲突应推 scvb.error{channelConflict}",
    );
    log(`  groupConflict setGroupId(2) → ${JSON.stringify(r)}`);
}

log("\n=== ④ remoteSetPriority 拒绝 / 成功 ===");
await withInput("scenario=no-output", async (b) => {
    // channel_id=1(已选)但 Output 离线
    const r = await b.remoteSetPriority(7);
    check(
        r.queued === false && r.reason === "outputOffline",
        `Output 离线 remoteSetPriority 应 outputOffline,实得 ${JSON.stringify(r)}`,
    );
    log(`  outputOffline → ${JSON.stringify(r)}`);
});
// 未分配(channel_id=0)用 empty fixture
await withInput("fixture=empty", async (b) => {
    const r = await b.remoteSetPriority(7);
    check(
        r.queued === false && r.reason === "unassigned",
        `未分配 remoteSetPriority 应 unassigned,实得 ${JSON.stringify(r)}`,
    );
    log(`  unassigned → ${JSON.stringify(r)}`);
});
await withInput("scenario=connected", async (b) => {
    const r = await b.remoteSetPriority(7);
    check(
        r.queued === true,
        `在线 remoteSetPriority 应 queued:true,实得 ${JSON.stringify(r)}`,
    );
    log(`  connected → ${JSON.stringify(r)}`);
});

log("\n=== ⑤ sourceKind 口径(source_channels 1/2/0/undefined)===");
const SK_CASES = [
    [1, "mono"],
    [2, "stereo"],
    [0, "unmeasured"],
    [undefined, "unmeasured"],
];
for (const [sc, want] of SK_CASES) {
    const got = sourceKind(sc);
    check(got === want, `sourceKind(${String(sc)}) 应 ${want},实得 ${got}`);
    log(`  sourceKind(${String(sc)}) = ${got}`);
}

log("\n=== ⑥ [SL-463] 非冲突失败回执:{ok:false, reason} ===");
// claim-unavailable:开箱已接管 ch2。换到 ch5 ⇒ 回执 unavailable,会话回滚 ⇒ 仍是 ch2 / active。
await withInput("scenario=claim-unavailable", async (b, seen) => {
    const st0 = seen.get("scvb.state:last");
    check(
        st0 && st0.channel_id === 2 && st0.claim === "active",
        `claim-unavailable 开箱应 ch2 / active,实得 ${JSON.stringify(st0 && { ch: st0.channel_id, claim: st0.claim })}`,
    );
    const r = await b.setChannelId(5);
    check(
        r &&
            r.ok === false &&
            r.reason === "unavailable" &&
            r.conflict === undefined,
        `claim-unavailable setChannelId(5) 应 {ok:false,reason:"unavailable"},实得 ${JSON.stringify(r)}`,
    );
    const again = await b.setChannelId(2); // 重选当前通道 = 真桥快路径,不重建段 ⇒ 成功
    check(
        again && again.ok === true,
        `claim-unavailable 重选当前 ch2 应 {ok:true},实得 ${JSON.stringify(again)}`,
    );
    const g = await b.setGroupId(2);
    check(
        g && g.ok === false && g.reason === "unavailable",
        `claim-unavailable setGroupId(2) 应 {ok:false,reason:"unavailable"},实得 ${JSON.stringify(g)}`,
    );
    const st = await awaitSeen(
        seen,
        "scvb.state",
        (v) => v.group_id === 2,
        "切组失败后的 scvb.state",
    );
    check(
        st && st.channel_id === 0 && st.claim === "idle",
        `新组建段失败后应 channel_id 0 / claim idle(§5.2 ② 支),实得 ${JSON.stringify(st && { ch: st.channel_id, claim: st.claim })}`,
    );
    log(
        `  setChannelId(5) → ${JSON.stringify(r)};setGroupId(2) → ${JSON.stringify(g)}`,
    );
});
// claim-abi-mismatch:开箱未分配。第一次点卡 ⇒ 回执 abiMismatch,claim 随之 abiMismatch 且带 abi_remote。
await withInput("scenario=claim-abi-mismatch", async (b, seen) => {
    const r = await b.setChannelId(4);
    check(
        r && r.ok === false && r.reason === "abiMismatch",
        `claim-abi-mismatch setChannelId(4) 应 {ok:false,reason:"abiMismatch"},实得 ${JSON.stringify(r)}`,
    );
    const st = await awaitSeen(
        seen,
        "scvb.state",
        (v) => v.claim === "abiMismatch",
        "abi 不符后的 scvb.state",
    );
    check(
        st &&
            st.channel_id === 0 &&
            Number.isInteger(st.abi_remote) &&
            st.abi_remote !== st.abi,
        `abi 不符后应 channel_id 0 且带不同的 abi_remote,实得 ${JSON.stringify(st && { ch: st.channel_id, abi: st.abi, abi_remote: st.abi_remote })}`,
    );
    const rel = await b.setChannelId(0); // 释放不 claim 任何段 ⇒ 真桥不会失败
    check(
        rel && rel.ok === true,
        `claim-abi-mismatch setChannelId(0) 应 {ok:true},实得 ${JSON.stringify(rel)}`,
    );
    log(
        `  setChannelId(4) → ${JSON.stringify(r)};setChannelId(0) → ${JSON.stringify(rel)}`,
    );
});

log(`\n=== 结果:${fail === 0 ? "全部通过" : fail + " 项失败"} ===`);
process.exit(fail === 0 ? 0 : 1);
