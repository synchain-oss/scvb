// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— toast③ 记账(`web/output/recapture-toast.js`)冒烟(J125)
// =============================================================================
// 跑什么(纯函数,node 侧;「算出来的东西真进了 DOM、钮真调了 analyze」在页面级那一套:
// smoke-output-stale-page.mjs ⑧):
//   ① 布防 → 播放穿过选区 → 撤防:秒数 = 播放头在选区里走过的时长,范围 = 采到的外包络;
//   ② 只计选区内那一截(起播在选区左边、停在右边之外);
//   ③ 01 采集关着的那几步不计(J92a 互斥 / 用户接管:引擎一个 hop 都不写);
//   ④ 跳播(Δt > MAX_STEP_S)与倒退不计;
//   ⑤ 同一段循环重录两遍按并集计,不翻倍;
//   ⑥ 布防后没播过就撤防 ⇒ 不弹(0.0s 没有信息量);
//   ⑦ 中途改选区的再布防(true → true)不清账;新一次布防(false → true)才清;
//   ⑧ 还开着的那条与新到的一条合并,不顶掉;
//   ⑨ 起播那一步(上一帧停着)与停播那一步(本帧停着)都计 —— 播放头真的走过了。
//
// 用法:node web-preview/tests/smoke-recapture-toast.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有断言失败(逐条打印 [FAIL])。
// =============================================================================

import { pathToFileURL, fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const u = (p) => pathToFileURL(join(ROOT, p)).href;
const R = await import(u("web/output/recapture-toast.js"));

let fail = 0;
const log = (...a) => console.log(...a);
function check(cond, msg) {
    if (!cond) {
        fail++;
        console.error("  [FAIL]", msg);
    }
    return cond;
}
const near = (a, b, msg, tol = 1e-9) =>
    check(
        Number.isFinite(a) && Math.abs(a - b) <= tol,
        `${msg}: 实得 ${a},期望 ${b}`,
    );

const armedState = (mask, s0, s1, cap = true) => ({
    global: { capture_enabled: cap },
    recapture: { armed: true, tracksMask: mask, startS: s0, endS: s1 },
});
const disarmedState = () => ({
    global: { capture_enabled: false },
    recapture: { armed: false, tracksMask: 0, startS: 0, endS: 0 },
});
const ph = (timeS, isPlaying = true) => ({ timeS, isPlaying, inRange: true });

/** 按 30Hz 从 t0 播到 t1(含起播前一帧停着的那一帧),返回走完后的 tracker 与最后一帧。 */
function play(tracker, state, t0, t1, prev0) {
    let prev = prev0 || ph(t0, false);
    let tr = tracker;
    const step = 1 / 30;
    for (let t = t0 + step; t < t1 + 1e-9; t += step) {
        const cur = ph(Math.min(t, t1), true);
        tr = R.recapOnPlayhead(tr, state, prev, cur);
        prev = cur;
    }
    return { tr, prev };
}

function session(mask, s0, s1, t0, t1, cap = true) {
    let tr = R.recapTrackerInit();
    const st = armedState(mask, s0, s1, cap);
    tr = R.recapOnState(tr, st).tracker;
    const r = play(tr, st, t0, t1);
    return R.recapOnState(r.tr, disarmedState());
}

// =============================================================================
log("=== ① 布防 → 播放穿过选区 → 撤防 ===");
{
    const { done } = session(0b1100, 10, 20, 12, 15.5);
    if (check(done, "① 撤防那一跳产出 done")) {
        near(done.seconds, 3.5, "① 秒数 = 12 → 15.5 走过的时长", 1e-6);
        near(done.startS, 12, "① 范围起点 = 采到的最早时刻", 1e-6);
        near(done.endS, 15.5, "① 范围终点 = 采到的最晚时刻", 1e-6);
        check(done.tracksMask === 0b1100, "① 轨掩码 = 布防掩码");
        check(R.fmtRecapSeconds(done.seconds) === "3.5", "① 显示为 3.5");
    }
}

log("=== ② 只计选区内那一截 ===");
{
    const { done } = session(1, 10, 20, 8, 23);
    if (check(done, "② done")) {
        near(done.seconds, 10, "② 8 → 23 播过去,只计 [10, 20]", 1e-6);
        near(done.startS, 10, "② 起点夹到选区左边", 1e-6);
        near(done.endS, 20, "② 终点夹到选区右边", 1e-6);
    }
}

log("=== ③ 01 采集关着不计 ===");
{
    const { done } = session(1, 10, 20, 12, 18, false);
    check(done === null, "③ 布防但采集全程关着 ⇒ 不弹");
    // 前半开、后半关:只计前半
    let tr = R.recapOnState(
        R.recapTrackerInit(),
        armedState(1, 0, 100),
    ).tracker;
    let r = play(tr, armedState(1, 0, 100, true), 10, 12);
    r = play(r.tr, armedState(1, 0, 100, false), 12, 20, r.prev);
    const d2 = R.recapOnState(r.tr, disarmedState()).done;
    if (check(d2, "③ 前半开采集 ⇒ 有 done")) {
        near(d2.seconds, 2, "③ 只计采集开着的那 2 秒", 1e-6);
    }
}

log("=== ④ 跳播与倒退不计 ===");
{
    let tr = R.recapOnState(
        R.recapTrackerInit(),
        armedState(1, 0, 100),
    ).tracker;
    const st = armedState(1, 0, 100);
    tr = R.recapOnPlayhead(tr, st, ph(10), ph(10 + R.MAX_STEP_S + 0.5));
    tr = R.recapOnPlayhead(tr, st, ph(40), ph(39));
    check(R.totalSeconds(tr.intervals) === 0, "④ 跳播一步与倒退一步都不入账");
    tr = R.recapOnPlayhead(tr, st, ph(40), ph(40 + R.MAX_STEP_S));
    near(
        R.totalSeconds(tr.intervals),
        R.MAX_STEP_S,
        "④ 恰好等于上限的一步照计",
        1e-9,
    );
}

log("=== ⑤ 循环重录按并集计 ===");
{
    const st = armedState(1, 10, 20);
    let tr = R.recapOnState(R.recapTrackerInit(), st).tracker;
    let r = play(tr, st, 11, 14);
    r = play(r.tr, st, 11, 14); // 回到 11 再录一遍(中间那一步是倒退,不计)
    const d = R.recapOnState(r.tr, disarmedState()).done;
    if (check(d, "⑤ done"))
        near(d.seconds, 3, "⑤ 两遍同一段 = 3s 不是 6s", 1e-6);
}

log("=== ⑥ 没播过就撤防 ⇒ 不弹 ===");
{
    let tr = R.recapOnState(
        R.recapTrackerInit(),
        armedState(1, 10, 20),
    ).tracker;
    check(R.recapOnState(tr, disarmedState()).done === null, "⑥ 0 秒不弹");
    // 只播了不到 MIN_REPORT_S
    const { done } = session(1, 10, 20, 12, 12 + 1 / 30);
    check(done === null, "⑥ 一帧(0.033s,显示为 0.0)不弹");
    check(
        R.recapOnState(R.recapTrackerInit(), disarmedState()).done === null,
        "⑥ 从未布防 ⇒ 撤防帧不产出",
    );
}

log("=== ⑦ 再布防不清账,新布防才清 ===");
{
    const stA = armedState(0b01, 10, 20);
    let tr = R.recapOnState(R.recapTrackerInit(), stA).tracker;
    let r = play(tr, stA, 12, 14);
    const stB = armedState(0b10, 30, 40); // 中途改选区 + 改勾选(armed 一直为 true)
    tr = R.recapOnState(r.tr, stB).tracker;
    near(R.totalSeconds(tr.intervals), 2, "⑦ true → true 保留前一段的账", 1e-6);
    r = play(tr, stB, 31, 32);
    const d = R.recapOnState(r.tr, disarmedState()).done;
    if (check(d, "⑦ done")) {
        near(d.seconds, 3, "⑦ 两段合计 3s", 1e-6);
        near(d.startS, 12, "⑦ 外包络起点", 1e-6);
        near(d.endS, 32, "⑦ 外包络终点", 1e-6);
        check(d.tracksMask === 0b11, "⑦ 两次布防的轨取并");
    }
    // false → true 的新一次布防:上一轮残账必须清(用一个没撤防就被重置的 tracker 模拟)
    let t2 = R.recapOnState(R.recapTrackerInit(), stA).tracker;
    t2 = play(t2, stA, 12, 14).tr;
    t2 = { ...t2, armed: false }; // 上一帧已撤防(recapOnState 已产出过 done)
    t2 = R.recapOnState(t2, stA).tracker;
    check(t2.intervals.length === 0, "⑦ false → true 清账");
}

log("=== ⑧ 合并,不顶掉 ===");
{
    const a = session(0b01, 0, 100, 10, 12).done;
    const b = session(0b10, 0, 100, 11, 15).done;
    const m = R.mergeRecapDone(a, b);
    if (check(m, "⑧ 合并结果非空")) {
        near(m.seconds, 5, "⑧ [10,12] ∪ [11,15] = 5s", 1e-6);
        near(m.startS, 10, "⑧ 起点", 1e-6);
        near(m.endS, 15, "⑧ 终点", 1e-6);
        check(m.tracksMask === 0b11, "⑧ 轨取并");
    }
    check(R.mergeRecapDone(null, b) === b, "⑧ 前面没有开着的 ⇒ 就是新来的");
    check(R.mergeRecapDone(a, null) === a, "⑧ 新来的为空 ⇒ 原样保留");
}

log("=== ⑨ 起播与停播两步都计 ===");
{
    const st = armedState(1, 0, 100);
    let tr = R.recapOnState(R.recapTrackerInit(), st).tracker;
    tr = R.recapOnPlayhead(tr, st, ph(10, false), ph(10.5, true));
    near(
        R.totalSeconds(tr.intervals),
        0.5,
        "⑨ 上一帧停着、本帧在播 ⇒ 计",
        1e-9,
    );
    tr = R.recapOnPlayhead(tr, st, ph(10.5, true), ph(10.8, false));
    near(
        R.totalSeconds(tr.intervals),
        0.8,
        "⑨ 上一帧在播、本帧停着 ⇒ 计",
        1e-9,
    );
    tr = R.recapOnPlayhead(tr, st, ph(10.8, false), ph(11.8, false));
    near(
        R.totalSeconds(tr.intervals),
        0.8,
        "⑨ 两帧都停着(停走时移动了播放头)⇒ 不计",
        1e-9,
    );
}

// =============================================================================
if (fail) {
    console.error(`\n失败 ${fail} 条 ❌`);
    process.exit(1);
}
console.log("\n全部通过 ✅");
