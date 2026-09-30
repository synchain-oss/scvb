// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 首帧竞态回归(node,无 DOM)
// =============================================================================
// 守的是 D14:driver 的周期定时器在页面调 `requestInitialState()` **之前**就已经在跑,
// 那几拍全被 §0.6 门控丢掉;若 `emitIfChanged` 在门控期照样记账,门控解除后的
// 「首帧必发」(§0.4 第 3 条)就会被自己那几拍的记账悄悄抑制 —— 实测症状是 Input 侧
// 只剩 scvb.state、Output 侧丢 conn/groups(连接 pill 与状态灯永远点不亮)。
//
// 两条路径都要绿:
//   ① 页面晚调 `requestInitialState()`(T31 接线后的常态);
//   ② 页面**根本不调**(T27b 灰模现状),靠 driver 1.5s 后兜底代调。
//
// 用法:node web-preview/tests/smoke-ready-race.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有首帧缺失。
// =============================================================================

import { pathToFileURL, fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const u = (p) => pathToFileURL(join(ROOT, p)).href;

const { createBridge, BRIDGE_EVENTS } = await import(u("web/shared/bridge.js"));
const driver = await import(u("web-preview/mock/state-driver.js"));

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fail = 0;

/** 首帧必发的状态类事件(§0.4 第 3 条),逐侧。 */
const REQUIRED = {
    output: [
        "scvb.state",
        "scvb.params",
        "scvb.conn",
        "scvb.groups",
        "scvb.segments",
    ],
    input: ["scvb.state", "scvb.conn", "scvb.config", "scvb.groups"],
};

function assertFirstFrames(label, role, seen) {
    for (const need of REQUIRED[role]) {
        if (!seen.get(need)) {
            fail++;
            console.error(`  [FAIL] ${label} 首帧缺 ${need}`);
        }
    }
}

// ① 页面晚调 requestInitialState()(iframe 装载 + 接线晚于若干定时器拍)
for (const role of ["output", "input"]) {
    const seen = new Map();
    const s = driver.createPreviewSession({
        role,
        params: "fixture=fifteen-tracks",
    });
    const b = createBridge({ role, mockBackend: s.mock });
    for (const ev of BRIDGE_EVENTS[role]) {
        b.on(ev, () => seen.set(ev, (seen.get(ev) || 0) + 1));
    }
    s.start();
    await sleep(1200);
    await b.requestInitialState();
    await sleep(60);
    console.log(
        `${role}(晚调): ${[...seen.entries()]
            .map(([k, v]) => `${k.replace("scvb.", "")}×${v}`)
            .join(" ")}`,
    );
    assertFirstFrames(`${role}(晚调)`, role, seen);
    s.stop();
}

// ② 页面完全不调(= T27b 灰模现状):driver 1.5s 后兜底代调
{
    const seen = new Map();
    const s = driver.createPreviewSession({
        role: "output",
        params: "fixture=empty",
    });
    const b = createBridge({ role: "output", mockBackend: s.mock });
    for (const ev of BRIDGE_EVENTS.output) {
        b.on(ev, () => seen.set(ev, (seen.get(ev) || 0) + 1));
    }
    s.start();
    await sleep(1800);
    console.log(
        `output(兜底代调): ${
            [...seen.entries()]
                .map(([k, v]) => `${k.replace("scvb.", "")}×${v}`)
                .join(" ") || "(无事件)"
        }`,
    );
    assertFirstFrames("output(兜底代调)", "output", seen);
    s.stop();
}

console.log(`\n=== 结果:${fail === 0 ? "全部通过" : fail + " 项失败"} ===`);
process.exit(fail === 0 ? 0 : 1);
