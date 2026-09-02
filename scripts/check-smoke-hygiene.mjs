// SPDX-License-Identifier: GPL-3.0-or-later
// check-smoke-hygiene.mjs —— 页面级冒烟的写法纪律门禁(零依赖,Node >= 18,ESM)。
//   真源 = web-preview/tests/smoke-*page*.mjs 这一族(起无头 Chrome + CDP 的那几套)。
//
//   [SL-287] 这一族有两个**只在出事时才显形**的失效形态,而它们都不会让任何现有用例变红:
//     ① CDP 调用没有截止时间 ⇒ 响应不回来就**永远不 resolve**。SL-274 实测挂过 75 分钟
//        零输出(node 与 Chrome 都还活着)。那次还赶上 [SL-277] 拆锁前的形态(整条 gates
//        被外部目录锁包着)所以堵住了整批;拆锁后 gate 1–5 不持锁,3e 挂死只停死本轮。
//     ② 收尾只写在 happy path ⇒ 中途抛错/被信号打断时 Chrome 与临时目录全留下。
//        本机 temp 下现有近千个 `scvb-*` 残留目录就是这么攒出来的(另见 SL-289)。
//   两条都是「加一套新冒烟时很容易照着旧模板复制、而复制出来的那份照样全绿」——
//   所以要有一道机检钉住写法,而不是靠人记得。
//
//   ⚠ 本检查是**文本级**的,不是语义级,两处边界都说明白:
//     · 三种 handler 形态靠字符串包含判断 ⇒ handler 体里写一句 `// teardown 在别处做`
//       的注释也能顶替真调用;
//     · `braceBody` 的花括号配对对**字符串 / 模板串 / 注释里的裸括号是盲的** ——
//       C 与 D 都吃这一份,所以 `waitFor` 体或 handler 体里将来出现一个含裸 `{`/`}` 的
//       字符串,配对会跑偏 ⇒ 假红。今天六套的 `waitFor` 体与三个 handler 体都花括号平衡、
//       串里无裸括号(逐个核过)。
//   要根治这两条都得上 AST,对一道防「照模板复制」的门禁不划算 —— 但别以为它比实际更严。
//
//   四条断言(每条都对应一个真实踩过的形态,不是泛泛的好习惯):
//     A. `send(method, params, timeoutMs)` —— CDP 调用能带截止时间。
//     B. 有 `CDP_DEFAULT_TIMEOUT_MS` 兜底(一次性调用用),且 send 里真的用了它。
//     C. `waitFor` 内部的 evaluate 传**按剩余预算算出来的**上界(含 `ms` 与 `Date.now() - t0`)。
//        为什么必须是「按预算算」而不是一个常数:smoke-output-dist-page 里有合法跑 8s 的
//        采样 evaluate、还有内部自带 12s 上界的 `measureOff`,而它最紧的 waitFor 预算
//        也是 12s —— 「大于合法最大值」与「小于最小预算」在单一常数下无解。
//        按剩余预算取还**不改变原本能过的行为**:第一版写 `ms/2`,把「耗时落在
//        (ms/2, ms) 区间的合法调用」从过变成必红(monitor 那套余量只有 1.65 倍)。
//     D. 收尾走**所有**退出路径:有 `teardown()`,且 exit / SIGINT / SIGTERM /
//        uncaughtException / unhandledRejection 五个都挂上了**且 handler 体里真的调了它**
//        —— 只检字符串在不在的话,一个空的 `process.on("exit", () => {})` 也能全绿。
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

// 从 idx 起找第一个 `{`,配对到它的 `}`。返回 `{ open, end, text }` —— **把 open 一起带回来**,
// 调用处就不必再拿 text 去全文 `indexOf` 找它在哪(复审指出:那是一次无锚再搜索,
// 文件里更早处若有一段逐字相同的小块,会返回更早那处 ⇒ 切出空串 ⇒ 假红)。
// C 与 D 共用。
function braceBody(src, idx) {
    const open = src.indexOf("{", idx);
    if (open < 0) return null;
    let depth = 0;
    for (let i = open; i < src.length; i++) {
        if (src[i] === "{") depth++;
        else if (src[i] === "}" && --depth === 0)
            return { open, end: i + 1, text: src.slice(open, i + 1) };
    }
    return null;
}

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
    // C 只认「第二实参里同时出现 `ms` 与 `Date.now() - t0`」,**不认形参名、不认换行**:
    // 耦合到那两样的话,把 `expr` 改名成 `expression`、或 prettier 把那行折行,都会让一份
    // 语义完全正确的冒烟变红,而失败文案说的却是「没按剩余预算取上界」—— 把人带错方向。
    // (第一版这句注释就写在这儿,而下面那行正则里 `expr` 仍是逐字的 —— 注释与代码反着来,
    //  复审逮到了。现在形参名放开成任意标识符,这句话才是真的。)
    // ⚠ 锚点必须钉在 **`waitFor` 的函数体**里,不能对整个文件 `match`(复审第三轮):
    // 放开形参名之后,全文件第一处 `evaluate(<标识符>,` 逐字都是**声明**
    // (`async function evaluate(expression, timeoutMs) {`),不是 waitFor 里那次调用。
    // 改前的逐字 `expr` 之所以没事,是因为 `\s*expr\s*,` 匹配不上 `expression,` ——
    // 声明是被**构造性地排除**的;放宽之后那层保护没了,今天不红只是因为
    // `{0,160}?` 够不着声明体里的 `);`(实测余量仅约 45 字符)。一旦越过去,捕获段会含
    // `timeoutMs`(里面有子串 `ms`!)却不含 `Date.now() - t0` ⇒ 判红,而文案说
    // 「waitFor 没按剩余预算取上界」—— 正是上面那三行注释声称已经消灭的「把人带错方向」。
    const wfAt = s.search(/async function waitFor\s*\(/);
    const wfBody = (wfAt >= 0 && braceBody(s, wfAt)?.text) || "";
    const evalArg = wfBody.match(
        /evaluate\(\s*[A-Za-z_$][\w$]*\s*,([\s\S]{0,160}?)\)\s*;/,
    );
    // 「没找到 waitFor」与「找到了但上界不对」必须分开报(复审指出):
    // 锚点钉到 `async function waitFor(` 之后,耦合从「字符数」换成了**声明的逐字形态** ——
    // 一份把 waitFor 写成 `const waitFor = async (expr, ms = 15000) => {` 的冒烟**语义完全正确**,
    // 却会因为 `wfAt < 0` 落进同一条文案,被告知「没按剩余预算取上界」,指着一个不存在的问题。
    // 失效方向是假红不是假绿,但文案指错方向本身就是本卡一路在治的东西。
    if (wfAt < 0)
        bad.push(
            "C. 没找到 `async function waitFor(` —— 本检查按这个逐字形态锚定 waitFor 的体;" +
                "若改成箭头函数或改了名,请同步改这里的锚点(不是上界写错了)",
        );
    else if (
        !evalArg ||
        !evalArg[1].includes("ms") ||
        !/Date\.now\(\)\s*-\s*t0/.test(evalArg[1])
    )
        bad.push(
            "C. `waitFor` 内部的 evaluate 没有按**剩余预算**取上界" +
                "(第二实参里应同时出现 `ms` 与 `Date.now() - t0`)。" +
                "写死常数会把「耗时落在上界与预算之间的合法调用」从过变成必红,见文件头",
        );
    if (!/function teardown\(\)/.test(s)) bad.push("D. 没有 `teardown()`");
    else {
        // 只检字符串在不在是不够的 —— 那样下面这份会全绿通过:
        //     process.on("exit", () => {});          // 挂了,但没收尾
        // 而这一族真正会发生的漂移正是「照模板复制后把 handler 体改空/改错」,
        // 不是「把 handler 整个删掉」。
        //
        // 也**不能只看「注册点附近有没有 teardown」**:第一版就是这么写的,结果把
        // `process.on("exit", teardown)` 改成 `process.on("exit", () => {})` 之后照样全绿 ——
        // 因为紧跟其后的信号循环体里有 `teardown`,落进了那个窗口。邻近度不是包含关系。
        // 改成**花括号配对取出真正的体**再看里面有没有 `teardown`。
        const covered = (h) => {
            // 形态一:`process.on("exit", teardown)` —— 直接把函数名当回调传进去。
            if (
                new RegExp(
                    `process\\.on\\(\\s*${h}\\s*,\\s*teardown\\s*\\)`,
                ).test(s)
            )
                return true;
            // 形态二:`for (const x of [… h …]) { … teardown() … }` —— 名字在数组字面量里。
            // ⚠ 锚点钉在 `for (` 上,并要求**同一个体**里同时出现信号名与 `teardown`。
            // 上一版写的是 `forAt >= 0 && bodyAfter(at)…`,复审指出两个毛病:
            //   · `forAt >= 0` **恒真**(这些文件在信号名之前有几十个 `for (`),等于没写 ——
            //     与本卡前面修掉的那条恒真式守卫同族,只是这次藏在合取式里;
            //   · `at` 是**全文件首次出现**该字符串,今天恰好落在数组字面量上才对。哪天有人
            //     在头注里写一句带引号的 `"SIGTERM"`(本族注释本来就在讨论它),锚点就会跳到
            //     那条注释,`bodyAfter` 取到随便哪个后续块 —— 判定从此随机,而且是静默的。
            // 取值范围是「`for (` 到它的体结束」这**一整段**,不是只取体:
            // 信号名在 `for (const sig of ["SIGINT", "SIGTERM"])` 的**数组头**里,不在体内。
            // (复审给的补丁写的是 `bodyAfter(forAt).includes(h)`,我照抄之后六套全红 ——
            //  诊断对、补丁形态不对,验一遍才发现。)
            // 遍历 h 的**每一处**出现,不是只看首现:上一版用 `s.indexOf(h)` 当搜索上界,
            // 首现只是从「锚点」降级成了「上界」、没有消失(复审指出)。方向那时已从
            // 静默假绿变成假红,但注释宣称的「不再依赖首现」当时还不成立 —— 现在成立了。
            for (let at = s.indexOf(h); at >= 0; at = s.indexOf(h, at + 1)) {
                const forAt = s.lastIndexOf("for (", at);
                if (forAt < 0) continue;
                const b = braceBody(s, forAt);
                if (!b) continue;
                // 用 open/end 精确切,不再拿 text 去全文 `indexOf` 找位置 ——
                // 那是一次无锚再搜索,更早处若有逐字相同的小块会切出空串 ⇒ 假红。
                const span = s.slice(forAt, b.end);
                // 还要求这段里真的在**注册**:光有「循环头提到信号名 + 体里调了 teardown」
                // 不代表它是 handler —— 任何调 teardown 的循环体里写一句提到 "SIGTERM"
                // 的注释就能顶替。`process.on(` 这一条几乎零成本。
                if (
                    span.includes(h) &&
                    span.includes("teardown") &&
                    span.includes("process.on(")
                )
                    return true;
            }
            // 形态三:`process.on(h, (…) => { … teardown() … })` —— 内联回调体。
            const onAt = s.indexOf(`process.on(${h}`);
            return (
                onAt >= 0 &&
                (braceBody(s, onAt)?.text || "").includes("teardown")
            );
        };
        const miss = HANDLERS.filter((h) => !covered(h));
        if (miss.length)
            bad.push(
                `D. 这些退出路径没挂上、或挂了但体内没调 teardown():${miss.join(" / ")}`,
            );
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
