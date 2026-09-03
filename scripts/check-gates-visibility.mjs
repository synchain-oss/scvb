// SPDX-License-Identifier: GPL-3.0-or-later
// check-gates-visibility.mjs —— 「降级路径必须在汇总里显形」的接线门禁(零依赖,Node >= 18,ESM)。
//   真源 = scripts/gates.ps1 的 Gate 3e。
//
// [SL-297] 为什么要有这条:本仓已经栽过一次「降级了,但摘要里看不见」——
//   页面级冒烟在 CDP 握手失败时退 2,而 gates 把 2 一律读成「缺可选外部依赖」⇒ 打 [SKIP]
//   ⇒ **照算 PASS**。于是在一台**装着 Chrome** 的机器上,一次瞬时超时就让整套判据无声消失,
//   汇总行还写着全 PASS(SL-293 的多轮 gates 里撞到三次,每次掉的套件还不一样)。
//   本卡把它拆成 rc=3 + `[FLAKY-SKIP]`,而**那个修复本身也需要有人守着**:
//   只要有人把汇总标签里那段插值删掉,`[FLAKY-SKIP]` 就退回「只在滚屏里出现一行」,
//   洞原样复现,而所有现有用例照绿。
//
// ⚠ 判据形态:**钉接线,不钉常量**。只断「文件里出现过 FLAKY-SKIP 字样」是没牙的 ——
//   那行 `Write-Host` 留着、而计数没进 `$smokeLabel`,正是本卡要防的那个形态。
//   所以三条一起断:①有 rc=3 分支;②该分支给**独立计数器**自增;③该计数器被拼进汇总标签。
//
// ⚠ 边界(与 check-smoke-hygiene 同款,写明免得以为它更严):本检查是**文本级**的,
//   不解析 PowerShell。变量若改名、或标签改用别的拼接写法(如 `-join`),会**假红**而不是漏判
//   —— 方向是安全侧:假红逼人回来看这条注释,漏判才会让洞悄悄回来。
//   它也**不验证运行时真打出了那行**;那由 SL-297 的删除式实跑覆盖(注入 CDP 握手失败 ⇒
//   汇总必须出现计数),本检查只保证「接线还在」。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const GATES = path.join(ROOT, "scripts", "gates.ps1");
const errors = [];
const fail = (m) => errors.push(m);

if (!fs.existsSync(GATES)) {
    console.error("[FAIL] 找不到 scripts/gates.ps1");
    process.exit(1);
}
const src = fs.readFileSync(GATES, "utf8");

// ① rc=3 分支存在,且打的是 FLAKY-SKIP(不是复用 SKIP —— 那正是被修掉的那个形态)
const rc3 = /\$rc\s+-eq\s+3\b/.test(src);
if (!rc3)
    fail(
        "gates.ps1 的 Gate 3e 没有 `$rc -eq 3` 分支 —— 浏览器在但没连上会退回被当成缺依赖",
    );
if (!/\[FLAKY-SKIP\]/.test(src))
    fail(
        "gates.ps1 里没有 `[FLAKY-SKIP]` 标记 —— 「没跑成」会和「跳过」长得一样",
    );

// ② 该分支给一个**独立**计数器自增(与 $smokeSkipped 分开:合用一个计数器 = 两类又混回去了)
if (!/\$smokeFlaky\s*\+\+/.test(src))
    fail("`$smokeFlaky++` 不在 —— rc=3 没有独立计数,汇总无从显形");
if (!/\$smokeFlaky\s*=\s*0/.test(src)) fail("`$smokeFlaky` 没有初始化");

// ③ **接线**:计数器要被拼进汇总标签。这一条是本检查的核心 ——
//    上面两条全绿而这一条红,就是「打了标记却没进摘要」,即本卡要堵的洞本身。
const wired =
    /\$smokeLabel\s*=\s*[^\n]*\$smokeFlaky|\$smokeFlaky[^\n]*\$smokeLabel/.test(
        src,
    ) || /-f\s+\$smokeLabel\s*,\s*\$smokeFlaky/.test(src);
if (!wired)
    fail(
        "`$smokeFlaky` 没有被拼进 `$smokeLabel` —— [FLAKY-SKIP] 只会出现在滚屏里," +
            "而跑完 gates 的人看的是汇总表:这正是 SL-297 要堵的那个洞原样复现。",
    );

// ④ 顺带守住 rc=2 那一档没被误改成 FLAKY(两类必须仍然分开)
if (!/\$rc\s+-eq\s+2\b/.test(src))
    fail("gates.ps1 丢了 `$rc -eq 2` 分支(真缺依赖那一档)");

if (errors.length) {
    console.error("check-gates-visibility 失败(" + errors.length + " 项):");
    for (const e of errors) console.error("  [FAIL] " + e);
    console.error(
        "  真源 = scripts/gates.ps1 的 Gate 3e;口径见本文件头注与 SL-297。",
    );
    process.exit(1);
}
console.log(
    "check-gates-visibility 通过:Gate 3e 的 rc=2 / rc=3 两档俱在,且 FLAKY 计数已接进汇总标签。",
);
process.exit(0);
