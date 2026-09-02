// SPDX-License-Identifier: GPL-3.0-or-later
// check-smoke-hygiene.mjs —— 页面级冒烟的写法纪律门禁(零依赖,Node >= 18,ESM)。
//   真源 = web-preview/tests/smoke-*page*.mjs 这一族(起无头 Chrome + CDP 的那几套)。
//
//   [SL-287] 这一族有两个**只在出事时才显形**的失效形态,而它们都不会让任何现有用例变红:
//     ① CDP 调用没有截止时间 ⇒ 响应不回来就**永远不 resolve**。SL-274 实测挂过 75 分钟
//        零输出(node 与 Chrome 都还活着),而它同时占着本机 IPC 锁,整批 agent 在排队。
//     ② 收尾只写在 happy path ⇒ 中途抛错/被信号打断时 Chrome 与临时目录全留下。
//        本机 temp 下现有近千个 `scvb-*` 残留目录就是这么攒出来的(另见 SL-289)。
//   两条都是「加一套新冒烟时很容易照着旧模板复制、而复制出来的那份照样全绿」——
//   所以要有一道机检钉住写法,而不是靠人记得。
//
//   四条断言(每条都对应一个真实踩过的形态,不是泛泛的好习惯):
//     A. `send(method, params, timeoutMs)` —— CDP 调用能带截止时间。
//     B. 有 `CDP_DEFAULT_TIMEOUT_MS` 兜底(一次性调用用),且 send 里真的用了它。
//     C. `waitFor` 内部的 evaluate 传**按本次预算算出来的**上界(`Math.floor(ms / 2)`)。
//        为什么必须是「按预算算」而不是一个常数:smoke-output-dist-page 里有合法跑 8s 的
//        采样 evaluate、还有内部自带 12s 上界的 `measureOff`,而它最紧的 waitFor 预算
//        也是 12s —— 「大于合法最大值」与「小于最小预算」在单一常数下无解。
//        写成 `ms/2` 之后,谁再加一条 `waitFor(x, 1000)`,上界自动跟到 500ms,不会被悄悄突破。
//     D. 收尾走**所有**退出路径:有 `teardown()`,且 exit / SIGINT / SIGTERM /
//        uncaughtException / unhandledRejection 五个都挂上了。
//   用法:
//     node scripts/check-smoke-hygiene.mjs [--help]
//   退出码:任一断言失败 = 1,全通过 = 0。

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(
    path.dirname(fileURLToPath(import.meta.url)),
    "..",
);
const TESTS = path.join(repoRoot, "web-preview", "tests");

if (process.argv.includes("--help")) {
    console.log("用法: node scripts/check-smoke-hygiene.mjs");
    console.log("断言页面级冒烟都有 CDP 截止时间,且收尾走所有退出路径。");
    process.exit(0);
}

// 只管**起浏览器的那一族**:靠 `cdpConnect` 认,而不是靠文件名里有没有 "page"。
// 按文件名认会漏掉将来叫别的名字的新冒烟 —— 那正是这道门禁要防的「照模板复制一份」。
const files = fs
    .readdirSync(TESTS)
    .filter((f) => f.startsWith("smoke-") && f.endsWith(".mjs"))
    .map((f) => path.join(TESTS, f))
    .filter((p) => fs.readFileSync(p, "utf8").includes("function cdpConnect("));

if (files.length === 0) {
    console.error(
        "check-smoke-hygiene: 一套带 cdpConnect 的冒烟都没找到 —— 这个门禁八成失效了(路径或形态变了)。",
    );
    process.exit(1);
}

const HANDLERS = [
    '"exit"',
    '"SIGINT"',
    '"SIGTERM"',
    '"uncaughtException"',
    '"unhandledRejection"',
];

let failed = 0;
for (const p of files) {
    const s = fs.readFileSync(p, "utf8");
    const name = path.basename(p);
    const bad = [];

    if (!/send\(method,\s*params,\s*timeoutMs\)/.test(s))
        bad.push("A. `send()` 没有 timeoutMs 形参(CDP 调用无法带截止时间)");
    if (!/const CDP_DEFAULT_TIMEOUT_MS\s*=\s*\d+/.test(s))
        bad.push("B. 缺 `CDP_DEFAULT_TIMEOUT_MS` 兜底常量");
    else if (!/timeoutMs\s*\|\|\s*CDP_DEFAULT_TIMEOUT_MS/.test(s))
        bad.push("B. 定义了 `CDP_DEFAULT_TIMEOUT_MS` 但 send 里没用它");
    if (
        !/evaluate\(expr,\s*Math\.max\(\s*\d+\s*,\s*Math\.floor\(ms \/ 2\)\)\)/.test(
            s,
        )
    )
        bad.push(
            "C. `waitFor` 内部的 evaluate 没有按本次预算取上界(应传 Math.floor(ms / 2));" +
                "写死一个常数会在某些套里与「大于合法最大值」互相矛盾,见文件头",
        );
    if (!/function teardown\(\)/.test(s)) bad.push("D. 没有 `teardown()`");
    else {
        const miss = HANDLERS.filter((h) => !s.includes(h));
        if (miss.length)
            bad.push(`D. 收尾没挂满退出路径,缺:${miss.join(" / ")}`);
    }

    if (bad.length) {
        failed++;
        console.error(`  [FAIL] ${name}`);
        for (const b of bad) console.error(`         ${b}`);
    }
}

if (failed > 0) {
    console.error(
        `check-smoke-hygiene: ${failed}/${files.length} 套不合纪律。` +
            "照着旧模板复制一份新冒烟时最容易漏这几条,而漏了之后所有现有用例照样全绿。",
    );
    process.exit(1);
}
console.log(
    `check-smoke-hygiene 通过:${files.length} 套页面级冒烟都有 CDP 截止时间且收尾走所有退出路径。`,
);
