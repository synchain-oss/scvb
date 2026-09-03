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
//   ⚠ 本检查是**文本级**的,不是语义级。下面这份边界清单**必须与扫描器数量同步** ——
//   [SL-291] 复审连着三轮点名它没跟上(当时还写着「两处边界」,而扫描器已经有五个),
//   而「都说明白」是一句承诺:清单漏了哪一条,读者就会以为那一条不存在。
//     · **字符串包含判断**:三种 handler 形态靠 `includes` 认 ⇒ handler 体里写一句
//       `// teardown 在别处做` 的注释也能顶替真调用;
//     · **`braceBody`(花括号配对)**:对字符串 / 模板串里的裸括号是盲的 —— C 与 D 都吃
//       这一份,`waitFor` 体或 handler 体里出现含裸 `{`/`}` 的串,配对会跑偏 ⇒ 假红;
//     · **`parenSpan`(圆括号配对)**:同上,串里的裸 `(`/`)` 会带偏;
//     · **`bodyAfter` 的深度计数**:同样只数字符,不懂串;且无花括号体以「深度 0 的换行」
//       为**近似**上界(见该函数头注:成员链断行会假红,失效方向倒向报错);
//     · **`skipComment`**:只认 `//` 与 `/* */` 的字面形态,对**串里的** `//`(如
//       `"http://…"`)是盲的 —— 会把它当成行注释起点。
//   今天六套逐个核过:`waitFor` 体与三个 handler 体花括号/圆括号平衡、串里无裸括号、
//   收尾段无 `http://` 之类的串。要根治这几条都得上 AST,对一道防「照模板复制」的门禁
//   不划算 —— 但别以为它比实际更严。
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
//   [SL-291] **判据自己也要有判据**:每次运行**先跑一遍内置自检矩阵**(正例「合法但不
//   常见的写法不许红」+ 删除式「该红必须红」;**格数以下面 `SELFTEST` 为准,别在这里写死** ——
//   第一版写了「7 格」,补两格正例后就成了假话),不合预期就直接退出、
//   **不再往下看真文件** —— 判据坏掉时,它对真文件的任何结论都不可信。
//   矩阵内置而不是另起测试文件,是因为本门禁扫的正是 `web-preview/tests/smoke-*.mjs`:
//   样本放进那个目录会被它自己扫到(判例:扫描器入库才炸),放别处又要另接一条 CI 命令。
//   内置成运行前置,CI 与 gates 里那条既有命令自动带上它。
//
//   用法:
//     node scripts/check-smoke-hygiene.mjs [--help] [--selftest]
//       --selftest  只跑自检矩阵,不扫真文件(排查判据本身时用)
//   退出码:自检失败 / 任一断言失败 = 1,全通过 = 0。

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(
    path.dirname(fileURLToPath(import.meta.url)),
    "..",
);
const TESTS = path.join(repoRoot, "web-preview", "tests");

if (process.argv.includes("--help")) {
    console.log("用法: node scripts/check-smoke-hygiene.mjs [--selftest]");
    console.log("断言页面级冒烟都有 CDP 截止时间,且收尾走所有退出路径。");
    console.log(
        "每次运行都先跑内置自检矩阵(正例 + 删除式);--selftest 只跑它。",
    );
    process.exit(0);
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
//
// ⚠ [SL-291] **本函数自己不设边界,边界由调用方给** —— 见 `bodyAfter`:它只在光标处
// 确实是 `{` 时才调本函数,所以「往后滑到无关块」这件事在那条路径上**由构造排除**。
// (第一版给本函数加了个 `limit` 形参,但唯一的调用方在调用前就已经确认 `{` 就在光标处,
//  那个形参因此**没有任何调用方依赖、也没有任何用例能让它变红** —— 无牙守卫比没有守卫更坏,
//  已删。D 档的真正边界是 `parenSpan` + `bodyAfter` 这一对。)
// C 档取 `waitFor` 体时仍用本函数的无边界形态:那里锚点就是函数声明,下一个 `{` 正是它的体。
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

// [SL-291] 从 idx 起找第一个 `(`,配对到它的 `)`。用来把「**这一次调用/这个循环头**」
// 框出来,给 braceBody 一个真实边界,而不是让它在整个文件里往后找。
// 与 braceBody 同样是文本级、对串/注释里的裸括号是盲的(边界说明见文件头)。
function parenSpan(src, idx) {
    const open = src.indexOf("(", idx);
    if (open < 0) return null;
    let depth = 0;
    for (let i = open; i < src.length; i++) {
        if (src[i] === "(") depth++;
        else if (src[i] === ")" && --depth === 0)
            return { open, end: i + 1, text: src.slice(open, i + 1) };
    }
    return null;
}

// [SL-291] 取「紧跟 from 之后的那个体」:有花括号就取配对块,没有花括号就取**这一条语句**
// (到**嵌套深度为 0 的那个分号**为止)。取不到 ⇒ 返回 null,调用方据此判**不覆盖**
// (失效方向倒向报错,不是放过)。
//
// ⚠ **不能裸用 `indexOf(";")`**(复审第 1 轮【重要】):无花括号循环的体常常是一个
// **带嵌套体的调用**,第一个分号在嵌套里面 ——
//     for (const sig of [...])
//         process.on(sig, () => { process.exit(130); teardown(); });
// 裸 indexOf 命中的是 `process.exit(130);` 那个分号,箭头体被**从中间截断**,
// `teardown` 落在 span 之外 ⇒ 判「没调」⇒ **假红**,而且这相对旧写法是**行为收窄**
// (旧的 `braceBody(s, forAt)` 会抓到箭头体的 `{…}` 判过)。
// 我把「行为零回归」写进过 PR 描述,那句只在今天这 6 套(全是带花括号的循环体)上成立。
//
// 另一头是**上界**:裸 indexOf 没有上界,无分号(ASI)写法下会滑到后面某处的 `;`、
// 把无关代码并进 span —— 那正是本卡要根治的**假绿**。
//
// ⚠ **光按深度不够,这一点我写错过一版**(复审第 2 轮):深度只让扫描**跳过嵌套里的
// 分号**,并**不给扫描加上界** —— `}` 让 curly 归 0 之后,它照样能一路滑到后面任意一条
// 顶层语句的 `;`:
//     for (const sig of [...])
//         process.on(sig, () => process.exit(130))      // ASI,无分号
//     if (true) { teardown(); process.on('noop', () => {}); }
//     const later = 1;                                   // ← 滑到这里才停
// span 于是把整个 `if` 块并了进来,里面既有 `teardown` 又有 `process.on(` ⇒ 判「已覆盖」。
// 而上一版注释却宣称「顺带堵住另一头」—— 正是本卡自己在治的「看着有防护、实际没做事」。
//
// 边界取「深度 0 的 `;`」**或**「深度 0 的换行」,**谁先算谁**。
//
// ⚠ 说清楚这条的**强度**(复审第 3 轮:上一版把它写成「构造性排除」,又比代码强了一档):
// ASI 的真规则不是「行尾即语句边界」,而是「下一个 token 接不下去时才补分号」。
// 深度 0 的换行处**继续同一条语句**是合法且常见的 —— 典型是成员链断行:
//     for (const sig of [...])
//         emitter
//             .on(sig, teardown);      // `emitter` 行尾 depth === 0
// 那种写法下本函数会在 `emitter` 后就收口 ⇒ span 不含 `teardown` ⇒ **假红**。
// 所以行尾只是一个**近似上界**,不是构造性排除;取它是因为**失效方向倒向报错**
// (假红看得见、有人来修),而不取会留下真正的假绿(见上面那段复现)。
// prettier 折长行多数折在括号里(depth > 0)不受影响,但成员链断行它确实会在 depth 0 折 ——
// 「折行不受影响」也只是今天恰好成立,别把它写成保证。今天六套都不是那种写法。
const NEWLINE = String.fromCharCode(10); // 换行符;写成常量避免转义歧义

// [SL-291] 跳过空白**与注释**。复审第 3 轮:上一版只跳空白,于是注释行会参与边界判定 ——
//     for (const sig of [...])
//         // 收尾见下
//         process.on(sig, teardown);
// 光标落在 `/` 上、扫到该行行尾(depth 0)就收口 ⇒ span 是那行注释 ⇒ **假红**;
// 更糟的是注释里出现一个裸 `(` 或 `{`(**本族头注本来就在讨论这些形态**),
// 深度再也回不到 0 ⇒ 扫到文件尾 ⇒ null ⇒ 同样假红。这是相对上一版的**新收窄**,不能留。
// (串/模板串里的裸括号仍是文件头已声明的盲区,不在本卡范围。)
// 若 i 处正好起一段注释,返回它之后的位置;否则原样返回 i(**不跳空白**)。
function skipComment(src, i) {
    if (src[i] === "/" && src[i + 1] === "/") {
        const nl = src.indexOf(NEWLINE, i);
        // 行注释**不吞掉那个换行**:换行是 bodyAfter 的上界,吞了就把上界也吞了。
        return nl < 0 ? src.length : nl;
    }
    if (src[i] === "/" && src[i + 1] === "*") {
        const close = src.indexOf("*/", i + 2);
        if (close < 0) return src.length;
        // 跨行的块注释**也只跳到它的第一个换行**,与行注释同档(复审第 4 轮):
        // 若整段跳过,中间那些深度 0 的换行就再也不被 `bodyAfter` 的主循环看见 ⇒
        // 上界在该形态下退化 ⇒ 假绿。两支强度必须一样,否则注释说不清它到底保证了什么。
        // (`skipTrivia` 是循环调用,停在换行处不影响它继续往前跳。)
        const nl = src.indexOf(NEWLINE, i);
        if (nl >= 0 && nl < close) return nl;
        return close + 2;
    }
    return i;
}

function skipTrivia(src, i) {
    for (;;) {
        while (i < src.length && /\s/.test(src[i])) i++;
        const j = skipComment(src, i);
        if (j === i) return i;
        i = j;
    }
}

function bodyAfter(src, from) {
    let i = skipTrivia(src, from);
    if (i >= src.length) return null;
    if (src[i] === "{") {
        // `{` **正好在光标处**才走配对 —— 这就是边界本身:不存在「往后找」的机会。
        const b = braceBody(src, i);
        return b && { end: b.end, text: b.text };
    }
    let paren = 0;
    let curly = 0;
    for (let j = i; j < src.length; j++) {
        // 注释区间整段跳过:否则注释里一个裸 `(`/`{` 就能把深度带偏,之后再也回不到 0
        // ⇒ 扫到文件尾 ⇒ 判不覆盖(假红)。与 skipTrivia 同一条理由。
        // ⚠ 这里**只跳注释、不跳空白** —— 用 skipTrivia 会把深度 0 的换行一并吞掉,
        //    而那个换行正是上面那条上界本身(自检矩阵当场逮到:ASI 那格从红变绿)。
        const afterComment = skipComment(src, j);
        if (afterComment > j) {
            j = afterComment - 1; // for 的 j++ 会补回来
            continue;
        }
        const c = src[j];
        if (c === "(") paren++;
        else if (c === ")") paren--;
        else if (c === "{") curly++;
        else if (c === "}") curly--;
        else if (c === ";" && paren === 0 && curly === 0)
            return { end: j + 1, text: src.slice(i, j + 1) };
        // 深度 0 的换行 = ASI 的语句边界,它和分号一样是这条语句的终点。
        else if (c === NEWLINE && paren === 0 && curly === 0)
            return { end: j, text: src.slice(i, j) };
    }
    return null;
}

// [SL-291] 四档检查抽成纯函数(输入源码文本 → 输出问题清单),这样**判据自己也能被测**:
// 下面的自检矩阵拿拼装出来的样本喂它,断言「该红的红、该过的过」。
// 抽之前这些检查只在真文件上跑过,而真文件恰好六套写法完全一致 —— 等于判据的大部分
// 分支从没被执行过(形态三今天一次都不会走到)。
function problemsOf(s) {
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
                // [SL-291] 先把**循环头**框出来,再取紧跟其后的体 —— 而不是从 `for (`
                // 一路找下一个 `{`。旧写法在**单行无花括号**循环下会滑到后面某个无关块:
                //     for (const sig of ["SIGINT","SIGTERM"]) process.on(sig, onSig);
                // 那时 `braceBody(s, forAt)` 抓到的是再往后的某个 `{…}`,而它里面通常
                // 恰好有 `teardown` 与 `process.on(` ⇒ **静默判绿**,实际这条路径没覆盖。
                const head = parenSpan(s, forAt);
                if (!head) continue;
                // 信号名必须在**循环头**里(它写在数组字面量 `["SIGINT","SIGTERM"]` 中),
                // 而不只是「这段里出现过」——否则体内一句提到信号名的注释就能顶替。
                if (!head.text.includes(h)) continue;
                const body = bodyAfter(s, head.end);
                if (!body) continue; // 取不到体 ⇒ 不认(倒向报错)
                // ⚠ **拼 head + body,不要裸切 `s.slice(forAt, body.end)`**(复审第 4 轮):
                // 裸切会把「循环头与体之间被跳过的那段注释」一起切进来,于是
                //     for (const sig of [...])
                //         // 见 process.on(sig, teardown)
                //         process.exit(130);          // 真体没注册也没收尾
                // 里的注释同时提供了 `teardown` 与 `process.on(` ⇒ 判「已覆盖」⇒ **假绿**。
                // 边界本来就已经算出来了,只是没用上。信号名另有 `head.text` 单独判过,
                // `teardown` 与 `process.on(` 本就该在体内,所以这是**纯收紧**。
                const span = head.text + body.text;
                // 还要求这段里真的在**注册**:光有「循环头提到信号名 + 体里调了 teardown」
                // 不代表它是 handler —— 任何调 teardown 的循环体里写一句提到 "SIGTERM"
                // 的注释就能顶替。`process.on(` 这一条几乎零成本。
                if (span.includes("teardown") && span.includes("process.on("))
                    return true;
            }
            // 形态三:`process.on(h, …)` —— 内联回调。
            //
            // [SL-291] 取**这次调用的括号跨度**,不再「从 onAt 起找下一个 `{`」:
            //   · 无花括号的箭头体 `process.on("SIGTERM", () => teardown());` 本来就没有 `{`,
            //     旧写法会滑到后面无关的块 ⇒ 假绿(而这种写法**是合法且应当判过的**);
            //   · 空体 `process.on("SIGTERM", () => {});` 的括号跨度里没有 `teardown` ⇒ 判红,
            //     与本档要防的「照模板复制后把 handler 体改空」正对上。
            // 括号跨度同时覆盖了「有花括号」与「无花括号」两种形态,不必分支。
            // 注册点用正则找(容忍 `process.on( "SIGTERM"` 这种空白),找不到 ⇒ 判红。
            // `h` 形如 `"SIGTERM"`(自带引号),引号在正则里不是元字符,直接内插即可。
            // (第一版这里写了 `h.replace(/"/g, '"')` —— 把 `"` 换成 `"`,**是个空操作**,
            //  却长得像在做转义。看着有防护、实际什么都没做,删掉。)
            // ⚠ **遍历每一处出现,不是只看首现**(复审第 1 轮)——与形态二上面那条
            // 「首现只是从锚点降级成上界、没有消失」是同一个毛病,第一版没把它套过来。
            // 触发形态:更早处有一段示例或被注释掉的注册(**本族的头注本来就在讨论这些形态**),
            // 首现钉在那里 ⇒ 括号跨度里没有 `teardown` ⇒ 判红,而真正的注册在后面、永远看不到。
            const re = new RegExp(`process\\.on\\(\\s*${h}`, "g");
            for (let m = re.exec(s); m; m = re.exec(s)) {
                const call = parenSpan(s, m.index);
                if (call && call.text.includes("teardown")) return true;
                // 跳过这一整次调用再往下找,免得在同一处原地打转。
                if (call) re.lastIndex = Math.max(re.lastIndex, call.end);
            }
            return false;
        };
        const miss = HANDLERS.filter((h) => !covered(h));
        if (miss.length)
            bad.push(
                `D. 这些退出路径没挂上、或挂了但体内没调 teardown():${miss.join(" / ")}`,
            );
    }

    return bad;
}

// ---------------------------------------------------------------------------
// [SL-291] **门禁自检**:每次运行都先跑一遍,不合预期就直接退出 —— 零自我豁免。
//
// 为什么内置而不是另起一个测试文件:这道门禁扫的正是 `web-preview/tests/smoke-*.mjs`,
// 把样本放进那个目录会被它自己扫到(「扫描器入库才炸」);而放在别处又需要另外接一条
// CI/gates 命令才跑得到。**内置成运行前置**则两边都不必改:CI 与 gates 里那条既有的
// `node scripts/check-smoke-hygiene.mjs` 自动带上它。
//
// 样本一律**拼装**而不是写成整份文件:只保留被测的那几行形态,其余用最小骨架凑齐
// A/B/C 三档,免得一个无关档位失败把 D 档的结论污染掉。
const NL = String.fromCharCode(10); // 换行符,避免在生成器里转义反斜杠
const SKELETON = [
    "function send(method, params, timeoutMs) {}",
    "const CDP_DEFAULT_TIMEOUT_MS = 20000;",
    "const x = timeoutMs || CDP_DEFAULT_TIMEOUT_MS;",
    "async function waitFor(expr, ms) {",
    "  const t0 = Date.now();",
    "  await evaluate(expr, Math.max(0, ms - (Date.now() - t0)));",
    "}",
    "function teardown() {}",
].join(NL);

// 六套真文件今天的写法(形态一 + 两个带花括号的 for 循环)—— 正例基线。
const REAL_SHAPE = [
    'process.on("exit", teardown);',
    'for (const sig of ["SIGINT", "SIGTERM"]) {',
    "    process.on(sig, () => {",
    "        teardown();",
    "        process.exit(130);",
    "    });",
    "}",
    'for (const ev of ["uncaughtException", "unhandledRejection"]) {',
    "    process.on(ev, (e) => {",
    "        teardown();",
    "        process.exit(1);",
    "    });",
    "}",
].join(NL);

// 每格:[样本名, 源码, 期望「D 档是否报问题」]。
// **正反都要有**:只有删除式(该红的红)会做出一碰就红的过紧判据,
// 反向用例(合法但不常见的写法**不该红**)才是它的对侧约束。
const SELFTEST = [
    // ---- 正例:不该红 --------------------------------------------------------
    ["真文件今天的写法", REAL_SHAPE, false],
    [
        "单行无花括号循环(合法,应判过)",
        [
            'process.on("exit", teardown);',
            'for (const sig of ["SIGINT", "SIGTERM"]) process.on(sig, teardown);',
            'for (const ev of ["uncaughtException", "unhandledRejection"]) process.on(ev, teardown);',
        ].join(NL),
        false,
    ],
    [
        "无花括号箭头体(合法,应判过)",
        [
            'process.on("exit", teardown);',
            'process.on("SIGINT", () => teardown());',
            'process.on("SIGTERM", () => teardown());',
            'process.on("uncaughtException", () => teardown());',
            'process.on("unhandledRejection", () => teardown());',
        ].join(NL),
        false,
    ],
    [
        "注册点带多余空白(合法,应判过)",
        [
            'process.on( "exit" , teardown );',
            'process.on( "SIGINT", () => teardown());',
            'process.on( "SIGTERM", () => teardown());',
            'process.on( "uncaughtException", () => teardown());',
            'process.on( "unhandledRejection", () => teardown());',
        ].join(NL),
        false,
    ],
    [
        "无花括号循环 + 嵌套体里 teardown 不是第一条语句(合法,应判过)",
        // ⚠ 这一格补的是第一版自检矩阵的**盲区**(复审第 1 轮指出):
        // 上面那格「单行无花括号循环」用的是最简形态 `process.on(sig, teardown)`,
        // `teardown` 就在第一个分号之前,所以**永远踩不到「分号切一半」**那条路径。
        // 这里让 handler 体带嵌套且 `teardown()` 排在 `process.exit()` 之后 ——
        // 裸 `indexOf(";")` 会在 `process.exit(130);` 处截断 ⇒ 假红。
        [
            'process.on("exit", teardown);',
            'for (const sig of ["SIGINT", "SIGTERM"])',
            "    process.on(sig, () => {",
            "        process.exit(130);",
            "        teardown();",
            "    });",
            'for (const ev of ["uncaughtException", "unhandledRejection"]) process.on(ev, teardown);',
        ].join(NL),
        false,
    ],
    [
        "更早处有被注释掉的反面示例(合法,应判过)",
        // 形态三只看首现时,锚点会钉在注释里那处 ⇒ 判红。本族头注**本来就在讨论这些形态**,
        // 所以这不是假想:遍历每一处出现才对。
        [
            '// 反面教材:别写成 process.on("SIGINT", () => {});',
            '// 也别写成 process.on("SIGTERM", () => {});',
            'process.on("exit", teardown);',
            'process.on("SIGINT", () => teardown());',
            'process.on("SIGTERM", () => teardown());',
            'process.on("uncaughtException", () => teardown());',
            'process.on("unhandledRejection", () => teardown());',
        ].join(NL),
        false,
    ],
    [
        "注册前有行注释(合法,应判过)",
        // 复审第 3 轮:上一版只跳空白不跳注释,光标落在 `/` 上、扫到行尾就收口 ⇒ 假红。
        [
            'process.on("exit", teardown);',
            'for (const sig of ["SIGINT", "SIGTERM"])',
            "    // 收尾见下",
            "    process.on(sig, teardown);",
            'for (const ev of ["uncaughtException", "unhandledRejection"]) process.on(ev, teardown);',
        ].join(NL),
        false,
    ],
    [
        "注释里有裸括号(合法,应判过)",
        // 更糟的那种:注释里一个裸 `(`/`{` 会把深度带偏、再也回不到 0 ⇒ 扫到文件尾 ⇒ 假红。
        // **本族头注本来就在讨论这些形态**,所以这不是假想。
        [
            'process.on("exit", teardown);',
            'for (const sig of ["SIGINT", "SIGTERM"])',
            "    // 别写成 process.on(sig, () => {}) 这种空体 {",
            "    process.on(sig, teardown);",
            'for (const ev of ["uncaughtException", "unhandledRejection"]) process.on(ev, teardown);',
        ].join(NL),
        false,
    ],
    // ---- 删除式:必须红 ------------------------------------------------------
    [
        "循环头与体之间的注释里写着注册,真体不收尾(复审第 4 轮)",
        // 裸切 `s.slice(forAt, body.end)` 会把这行注释一起切进 span ——
        // 注释同时提供 `teardown` 与 `process.on(` ⇒ 判「已覆盖」⇒ 假绿。
        // 拼 `head.text + body.text` 之后注释落在两段之外,判红。
        [
            'process.on("exit", teardown);',
            'for (const sig of ["SIGINT", "SIGTERM"])',
            "    // 见 process.on(sig, teardown)",
            "    process.exit(130);",
            'for (const ev of ["uncaughtException", "unhandledRejection"]) {',
            "    process.on(ev, () => {",
            "        teardown();",
            "    });",
            "}",
        ].join(NL),
        true,
    ],
    [
        "ASI 无分号 + 后面有顶层语句(本卡第 2 轮才堵住的那个假绿)",
        // 复审第 2 轮给的复现:体没有分号收尾,深度归零后扫描会一路滑到 `const later = 1;`,
        // 把中间那个 `if` 块并进 span —— 里面既有 teardown 又有 process.on( ⇒ 判「已覆盖」。
        // 只按深度找分号挡不住它,必须**同时以深度 0 的换行为界**。
        [
            'process.on("exit", teardown);',
            'for (const sig of ["SIGINT", "SIGTERM"])',
            "    process.on(sig, () => process.exit(130))",
            "if (true) { teardown(); process.on('noop', () => {}); }",
            "const later = 1;",
            'for (const ev of ["uncaughtException", "unhandledRejection"]) {',
            "    process.on(ev, () => {",
            "        teardown();",
            "    });",
            "}",
        ].join(NL),
        true,
    ],
    [
        "单行无花括号循环但**没调 teardown**(本卡要抓的那个洞)",
        // ⚠ 这一格必须**只让 sig 那条路径可疑**:其余四条退出路径都用最稳的写法覆盖住,
        // 否则它可能因为**别的**路径没覆盖而变红 —— 那样注入回旧写法时它照样红,
        // 这一格就检不出本卡要防的那个假绿(「注入要精准:核红在不在设计接住它的那条断言上」)。
        [
            'process.on("exit", teardown);',
            // 只有 SIGINT/SIGTERM 走「单行无花括号且体内不调 teardown」;
            // 紧随其后是一个**含 teardown 与 process.on( 的无关块** ——
            // 无边界的 braceBody 正是滑到这里、把它当成循环体而判绿的。
            'for (const sig of ["SIGINT", "SIGTERM"]) process.on(sig, () => process.exit(130));',
            "if (true) {",
            "    teardown();",
            "    process.on('noop', () => {});",
            "}",
            // 这两条用**带花括号**的循环稳稳覆盖住,不参与本格的判定。
            'for (const ev of ["uncaughtException", "unhandledRejection"]) {',
            "    process.on(ev, () => {",
            "        teardown();",
            "    });",
            "}",
        ].join(NL),
        true,
    ],
    [
        "handler 体被改空",
        [
            'process.on("exit", teardown);',
            'for (const sig of ["SIGINT", "SIGTERM"]) {',
            "    process.on(sig, () => {});",
            "}",
            "function later() { teardown(); }",
            'for (const ev of ["uncaughtException", "unhandledRejection"]) process.on(ev, teardown);',
        ].join(NL),
        true,
    ],
    [
        "整条退出路径没挂",
        [
            'process.on("exit", teardown);',
            'for (const sig of ["SIGINT"]) process.on(sig, teardown);',
            'for (const ev of ["uncaughtException", "unhandledRejection"]) process.on(ev, teardown);',
        ].join(NL),
        true,
    ],
];

{
    let selfBad = 0;
    for (const [label, shape, wantBad] of SELFTEST) {
        const problems = problemsOf(`${SKELETON}
${shape}
`);
        const dProblems = problems.filter((b) => b.startsWith("D."));
        // 只看 D 档:A/B/C 由骨架保证,若它们红了说明骨架该同步更新,单独报出来。
        const nonD = problems.filter((b) => !b.startsWith("D."));
        if (nonD.length) {
            selfBad++;
            console.error(`  [自检骨架失效] ${label}:${nonD.join(" | ")}`);
        }
        if (dProblems.length > 0 !== wantBad) {
            selfBad++;
            console.error(
                `  [自检失败] ${label}:期望 D 档${wantBad ? "报问题" : "不报问题"},` +
                    `实得 ${dProblems.length ? dProblems.join(" | ") : "无问题"}`,
            );
        }
    }
    if (selfBad > 0) {
        console.error(
            `check-smoke-hygiene: **门禁自检未通过**(${selfBad} 项)—— 判据本身坏了,` +
                "此时对真文件的任何结论都不可信,故不再往下跑。",
        );
        process.exit(1);
    }
}

if (process.argv.includes("--selftest")) {
    console.log(
        `check-smoke-hygiene 自检通过:${SELFTEST.length} 格(含删除式与反向用例)。`,
    );
    process.exit(0);
}

// 只管**起浏览器的那一族**:靠 `cdpConnect` 认,而不是靠文件名里有没有 "page"。
// 按文件名认会漏掉将来叫别的名字的新冒烟 —— 那正是这道门禁要防的「照模板复制一份」。
//
// ⚠ **必须放在自检之后**(复审第 1 轮):这一块原先在模块顶层无条件执行,于是
//   · `--selftest` 实际仍会扫真文件目录,与它自己声明的「不扫真文件」反着来;
//   · 目录改名 / 一套都找不到时,先在这里 `exit(1)`,**自检矩阵一格都跑不到** ——
//     而「排查判据本身」恰恰是判据可能同时坏掉的场景,那时伸手去拿的开关却拒绝启动;
//   · `TESTS` 不存在时 `readdirSync` 直接抛 ENOENT,连那句话都没有。
// 自检矩阵不依赖 `files`,下移即可,与文件头声明的次序(先自检、不过就不再看真文件)对齐。
let entries;
try {
    entries = fs.readdirSync(TESTS);
} catch (e) {
    // 目录不在时 `readdirSync` 抛 ENOENT,裸栈对读者毫无信息量;这里换成说得清的一句。
    // 与下面「一套都没找到」同一口径:门禁**够不到被测面**时判红,不是静默放过。
    console.error(
        `check-smoke-hygiene: 读不到 ${TESTS}(${e.code || e.message})—— ` +
            "这个门禁八成失效了(路径变了?)。",
    );
    process.exit(1);
}
const files = entries
    .filter((f) => f.startsWith("smoke-") && f.endsWith(".mjs"))
    .map((f) => path.join(TESTS, f))
    .filter((p) => fs.readFileSync(p, "utf8").includes("function cdpConnect("));

if (files.length === 0) {
    console.error(
        "check-smoke-hygiene: 一套带 cdpConnect 的冒烟都没找到 —— 这个门禁八成失效了(路径或形态变了)。",
    );
    process.exit(1);
}

let failed = 0;
for (const p of files) {
    const s = fs.readFileSync(p, "utf8");
    const name = path.basename(p);
    const bad = problemsOf(s);
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
