// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output —— A-02 diff 摘要的折叠与泳道保底,**页面级**冒烟(无头 Chrome + CDP;SL-274)
// -----------------------------------------------------------------------------
// 用户 v5.6.5 实测:拖参数松手应用后「diff 摘要弹出全轨全段列表」,且「泳道完全消失,
// 过一会才出现」。根因**不在数据面** —— `applySegmentsEvent` 从不清空段表,native 也只在
// 重分段**完成**时才发那一帧。是**布局**:
//
//     .wave-toolbar { flex: none; }            <- 不可压缩,高度随内容长
//     .wave-window  { flex: 1; min-height: 0 } <- 可被压到 0
//
// diff 明细当时是逐条 <li> 平铺、无折叠无封顶的(native 封顶 200 条),一次全量重分段
// 把工具条撑到整屏,泳道窗被整块挤没;6s 后 `DIFF_HIDE_MS` 自动收起,泳道才回来 ——
// 「过一会才出现」逐字对上。本套的反向注入实测(去掉明细封顶)复现了它:明细 29 条平铺 ⇒
// `.wave-toolbar` 614px > 面板 555px ⇒ `wave-lanes` 的 `clientHeight` 与可视高**双双为 0**。
//
// **为什么必须页面级**:上面每一个数都来自**布局引擎**。node 侧那套(smoke-tab3-
// interactions.mjs)断的是纯函数与源码字面,`clientHeight` 在无 DOM 环境里恒 0 ——
// 这条缺陷从定义上就落在它够不着的地方。判例逐字见 smoke-output-dist-page.mjs 头注
// 的「写入面的断言证明不了渲染面」。
//
// 五条判据,**各自配一条删之即红的通路**:
//   (1) 默认折叠:`<details>` 的 open 为假、而明细条目**已在 DOM 里**(不是没数据)。
//       删掉折叠(默认展开)=> 红。
//   (2) 展开封顶:展开后 `.wave-diff__items` 自己滚(scrollHeight > clientHeight)
//       且 clientHeight <= max-height。删掉 `max-height` => 明细整列铺开 => 红。
//   (3) 工具条预算(本卡真正的不变量):折叠态与展开态都要满足
//       `工具条高 + 泳道窗高 <= 面板高`,且泳道矩形与面板可视矩形真的有交集;
//       再加一条**两态自比**:折叠态工具条必须矮于展开态(折叠真的省下了高度)。
//       去掉折叠或去掉明细封顶 => 29 条明细平铺、工具条涨到 614px(> 面板 555px)=> 红。
//       两个数都要量:面板是 overflow:hidden,**盒子还在**不等于**看得见**。
//       (为什么不是「压窄视口 + min-height 兜底」——那两条实测都不承力,
//        原因逐字写在判据 (3) 那一段的注释里。)
//   (4) 自动收起分两档,三条都要断:
//       (4a) 展开态跨过折叠档 `DIFF_HIDE_MS` 仍在屏上 —— 两档合并回一档 => 红;
//       (4b) 收回折叠后按短档撤下 —— 长档把自动收起整条废掉 => 红;
//       (4c) 展开态跨过 `DIFF_HIDE_OPEN_MS` 也要撤下 —— 展开态改回「不起表」
//            (第一版形态,会让这块永远挂在屏上而它没有关闭钮)=> 红。
//   (5) 顶到 `changed[]` 封顶(200)时,折叠头印的是**下界**「200+」而不是「200」。
//       常态素材只出 29 条,这个分支一条用例都到不了 —— 故本条另开一次装载,走
//       `?scenario=diff-flood`(mock 的 `diffFillToCap`)把 changed 抽满。
//       删掉 tab-wave.js 里 `nChanged >= DIFF_CHANGED_CAP` 那个三元 => 红。
//       (三处 200 是否同值由 smoke-mock.mjs 的三方对拍守 —— native 走正则、
//        两侧 JS 走 import 比运行时真值 —— 不在本套。)
//
// ⏱ 本套**跑得慢**(本机实测 ~60s),这是判据本身要求的:(4) 那三条要真的等过
// `DIFF_HIDE_MS`(6s)两次与 `DIFF_HIDE_OPEN_MS`(30s)一次 —— 自动收起的时长是被测
// 行为,拿假计时器替掉就等于不测它。慢是买来的确定性,别为了提速把等待砍掉。
//
// 用法:node web-preview/tests/smoke-seg-diff-fold-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;**2 = 环境里没有 Chrome/Edge**(口径同
//   smoke-monitor-page.mjs 与 CLAUDE.md 的可选依赖档:缺席不判红,但也绝不算通过)。
//
// CDP 那 30 行与 smoke-seg-restore-page.mjs / smoke-output-dist-page.mjs 同源
// (node 内置 fetch + WebSocket,零依赖 —— 仓库红线是不引 puppeteer)。同样不抽公共
// 模块:各套断各的面,合并只会让彼此被对方的参数面绑住。
// =============================================================================

import { spawn } from "node:child_process";
import { createServer } from "node:http";
import {
    existsSync,
    mkdtempSync,
    readFileSync,
    rmSync,
    statSync,
    writeSync, // [SL-274] 致命错误直写 fd 2,process.exit() 截不掉
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, extname, join, resolve, sep } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const ROOT =
    process.argv[2] && !process.argv[2].startsWith("--")
        ? process.argv[2]
        : resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");

const argv = new Map(
    process.argv
        .slice(2)
        .filter((a) => a.startsWith("--"))
        .map((a) => {
            const i = a.indexOf("=");
            return i < 0 ? [a.slice(2), "1"] : [a.slice(2, i), a.slice(i + 1)];
        }),
);

const CHROME_CANDIDATES = [
    "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
    "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
    `${process.env.LOCALAPPDATA || ""}\\Google\\Chrome\\Application\\chrome.exe`,
    "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
    "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
];

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let fail = 0;

// 全部输出走 `writeSync(1, …)` 而不是 `console.log`([SL-274] 第 11 轮复审)。
//
// 理由与致命行那条**是同一条**:`process.exit()` 不等挂起的异步写,而本地 gates 是
// `$out = (& node $f.FullName 2>&1)`(`scripts/gates.ps1:387`)—— stdout 被收成管道。
// 上一轮只把**致命那一行**改成同步,于是同一条失败路径上**先前**打的 `[FAIL]` 与进度行
// 仍可能被 `process.exit(1)` 截掉:红是红了,却说不清前面已经断到哪一步。
//
// ⚠ **别把这句写成「同步写在哪儿都无条件成立」**(第 12 轮复审):Node 对
// `process.stdout` 的同步性是**分档**的 —— 管道/socket 在 **Windows 上同步、POSIX 上
// 异步**(libuv 给该 fd 设了 `O_NONBLOCK`),而对非阻塞管道调 `fs.writeSync` 可能抛
// `EAGAIN`。所以这条改动**真正买到保障的是本地 gates 这条 Windows 路径**,也正是
// 出问题的那条;`web-smoke`(`ubuntu-latest`)那侧 `format.yml` 直接 `node "$f"` 继承
// stdio、本来就没有这个截断面。POSIX 上万一真抛 `EAGAIN`,下面的 catch 会兜回
// `console.log` —— 退化成原来的行为,不会因为「想打得更稳」反而把整套打挂。
//
// 一套冒烟的输出量是几十行,同步写的代价可以忽略,而「红的时候话说不全」的代价不行。
function out(s) {
    try {
        writeSync(1, s + "\n");
    } catch {
        // fd 1 不可写(POSIX 非阻塞管道的 EAGAIN、或 fd 已断)时退回异步写。
        // 这里**不再抛**:打印失败不该变成判定失败。
        try {
            console.log(s);
        } catch {}
    }
}
const log = (s) => out(s);
function check(cond, msg) {
    if (cond) return true;
    fail++;
    out(`  [FAIL] ${msg}`);
    return false;
}
function eq(got, want, msg) {
    const a = JSON.stringify(got);
    const b = JSON.stringify(want);
    if (a === b) return true;
    fail++;
    out(`  [FAIL] ${msg}\n         实得 ${a}\n         应为 ${b}`);
    return false;
}

// ---------------------------------------------------------------- 静态服务
const MIME = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".woff2": "font/woff2",
};
const server = createServer((req, res) => {
    let p = decodeURIComponent(new URL(req.url, "http://x").pathname);
    if (p.endsWith("/")) p += "index.html";
    const abs = resolve(join(ROOT, p));
    // 目录**边界**判定,不是字符串前缀判定(#161 复审 pr-agent 安全项)。
    // 裸 `startsWith(root)` 的洞:ROOT=`/a/repo` 时 `/a/repo2/x` 也过 —— 百分号编码的
    // `%2e%2e` 会在 decodeURIComponent 之后还原成 `..`,resolve 出去落到兄弟目录,
    // 而它恰好与 ROOT 共享字符串前缀。服务器虽只绑 127.0.0.1、只活在冒烟进程里,
    // 但这是「读文件的边界判定」,没有理由写成会漏的那种。
    const rootAbs = resolve(ROOT);
    if (abs !== rootAbs && !abs.startsWith(rootAbs + sep)) {
        res.writeHead(403).end("nope");
        return;
    }
    if (!existsSync(abs) || !statSync(abs).isFile()) {
        res.writeHead(404).end("not found");
        return;
    }
    res.writeHead(200, {
        "Content-Type":
            MIME[extname(abs).toLowerCase()] || "application/octet-stream",
        "Cache-Control": "no-store",
    });
    res.end(readFileSync(abs));
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const base = `http://127.0.0.1:${server.address().port}`;

// ---------------------------------------------------------------- CDP 小客户端
function cdpConnect(wsUrl) {
    const ws = new WebSocket(wsUrl);
    const pending = new Map();
    const listeners = [];
    let id = 0;
    ws.addEventListener("message", (ev) => {
        const msg = JSON.parse(ev.data);
        if (msg.id && pending.has(msg.id)) {
            const { resolve: ok, reject: no } = pending.get(msg.id);
            pending.delete(msg.id);
            msg.error ? no(new Error(msg.error.message)) : ok(msg.result);
        } else if (msg.method) {
            for (const fn of listeners) fn(msg);
        }
    });
    const ready = new Promise((ok, no) => {
        ws.addEventListener("open", ok, { once: true });
        ws.addEventListener("error", () => no(new Error("CDP 连接失败")), {
            once: true,
        });
    });
    // ⚠ 每条 CDP 调用**必须有截止时间**([SL-274] 本机实测踩到)。
    //
    // 原版(与其余五套页面级冒烟同源)的 `send()` 把 resolve 塞进 `pending` 就返回,
    // 响应不来就**永远不 resolve** —— 本套实测在 gate 3e 里挂了 **75 分钟**零输出,
    // Chrome 与 node 都还活着。(CI 上则是一路烧到 job 超时才红,而 `web-smoke` 是
    // required check;本机那次还顺带占住了跑 gates 的 agent 自己那把外层目录锁。)
    //
    // **触发形态是「导航把旧渲染器换掉,紧随其后的 `Runtime.evaluate` 等一个永远不来的
    // 响应」**;本套判据 (5) 要换 `?scenario=diff-flood` 重新装载,踩的就是这一下。
    //
    // ⚠ **别写成「六套里只有本套会二次导航」**(第 10 轮复审 grep 证伪,那句话我写错过):
    // 实点 `cdp.send("Page.navigate"` 的**真实调用**(不含注释里提到的 —— 第 11 轮复审
    // 又抓到我把这个数抄错:上一版写「本套 4 次」是 `grep -c 'Page.navigate'` 把注释
    // 一起数进去了):
    //     ui-layout 8 / monitor 4 / output-dist 3 / **本套 2** / seg-restore 1 / output-stale 1
    // 也就是说**曝险面比本卡大得多,而且都还没修**(它们没有超时也只在 happy path 收尾)。
    // 本卡不越界改别人的文件,已在 PR 里点名建议单独立卡统一收口 —— 谁去做那张卡,
    // 照搬本文件这两段(超时 + teardown)即可。
    //
    // 修法是**超时抛错**而不是重试:响应不来说明页面或渲染器已经不对了,重试只会把
    // 一个确定的红拖成一个更慢的红。错误里带上 method 与 id,红出来直接指到是哪一条卡住。
    //
    // 这个数**必须小于 `waitFor` 的 20s 预算**:`waitFor` 内部的 `evaluate` 是 try/catch
    // 兜住再重试的,超时短于它才能「丢一次响应 → 下一轮补上」;若反过来设成 30s,
    // 一次超时就直接吃穿 waitFor 的整个预算,把可恢复的抖动变成硬红。
    const CDP_CALL_TIMEOUT_MS = 10000;
    return {
        ready,
        on: (fn) => listeners.push(fn),
        send(method, params) {
            const mid = ++id;
            return new Promise((ok, no) => {
                const timer = setTimeout(() => {
                    pending.delete(mid);
                    no(
                        new Error(
                            `CDP 调用超时 ${CDP_CALL_TIMEOUT_MS}ms:${method}(id=${mid})—— ` +
                                "响应没回来。多半是这一步之前的导航把渲染器换掉了;" +
                                "**不要**改成重试或调大超时,那只是把红拖慢",
                        ),
                    );
                }, CDP_CALL_TIMEOUT_MS);
                pending.set(mid, {
                    resolve: (v) => {
                        clearTimeout(timer);
                        ok(v);
                    },
                    reject: (e) => {
                        clearTimeout(timer);
                        no(e);
                    },
                });
                ws.send(
                    JSON.stringify({ id: mid, method, params: params || {} }),
                );
            });
        },
        close: () => ws.close(),
    };
}

function noBrowser(msg) {
    // 同样走同步写:这条也紧跟 `process.exit()`,而 gate 3e 判 rc=2 时按 `^❌` 抓解释行
    // (`scripts/gates.ps1:392`)—— 被截断就只剩一行 SKIP、说不出缺的是什么。
    try {
        writeSync(
            2,
            `❌ ${msg}\n` +
                "   页面级冒烟无法运行(退出码 2)。这**不是**通过:装一个 Chrome/Edge," +
                "或用 --chrome=<路径> 指定。\n",
        );
    } catch {
        console.error(`❌ ${msg}`);
    }
    try {
        server.close();
    } catch {}
    process.exit(2);
}

function chromePath() {
    if (argv.has("chrome")) {
        const p = argv.get("chrome");
        if (!existsSync(p)) noBrowser(`--chrome 指定的路径不存在:${p}`);
        return p;
    }
    for (const p of CHROME_CANDIDATES) if (existsSync(p)) return p;
    noBrowser("本机找不到 Chrome/Edge");
    return null;
}

const exe = chromePath();
const CDP_PORT = Number(
    argv.get("cdp") || 9800 + Math.floor(Math.random() * 400),
);
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-diff-fold-"));
const chrome = spawn(
    exe,
    [
        "--headless=new",
        `--remote-debugging-port=${CDP_PORT}`,
        `--user-data-dir=${userDataDir}`,
        "--window-size=1600,1000",
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-extensions",
        "--disable-background-timer-throttling",
        "--disable-backgrounding-occluded-windows",
        "--disable-renderer-backgrounding",
        "--force-device-scale-factor=1",
        ...(process.env.CI ? ["--no-sandbox"] : []),
        "about:blank",
    ],
    { stdio: "ignore" },
);
chrome.on("error", (e) => noBrowser(`浏览器启动失败:${e.message}`));

let cdp = null;
const errors = [];
const exceptions = [];

async function evaluate(expression) {
    const r = await cdp.send("Runtime.evaluate", {
        expression,
        returnByValue: true,
        awaitPromise: true,
    });
    if (r.exceptionDetails) {
        throw new Error(
            "页内求值抛错:" +
                (r.exceptionDetails.exception?.description ||
                    r.exceptionDetails.text),
        );
    }
    return r.result?.value;
}

// 收尾:**任何**退出路径都要走一遍,不只是跑完那一条([SL-274] 本机实测)。
// 原版把 kill/close/rmSync 摊在文件末尾,于是**只有 happy path 会清理** ——
// 一旦中途抛错(比如上面新加的 CDP 超时),进程直接死,headless Chrome 与
// `scvb-diff-fold-*` 临时目录全部留在机器上。这次排查时本机已积了 35 个残留目录,
// 而挂死那次留下的 Chrome 一直活到人工介入才被杀掉。
// (其余五套页面级冒烟同样只在 happy path 收尾 —— 见上面 cdpConnect 头注那条。)
let tornDown = false;
function teardown() {
    if (tornDown) return;
    tornDown = true;
    try {
        cdp?.close();
    } catch {}
    try {
        chrome?.kill();
    } catch {}
    try {
        server.close();
    } catch {}
    try {
        rmSync(userDataDir, { recursive: true, force: true });
    } catch {}
}
// ⚠ 收得干净的是**进程**,不保证临时目录:`chrome.kill()` 之后文件句柄未必立刻释放,
// 紧跟的 `rmSync` 在 Windows 上可能失败,留下一个空的 `scvb-diff-fold-*`。
// 那是可接受的残渣(系统会清),而**跑着的 headless Chrome 不是** —— 它会占住端口与
// 机器资源,这次就是它跟着挂死的 node 一起赖了 75 分钟。`exit` 处理器只能同步收尾
// (Node 规范),所以这里不等、也不重试。

process.on("exit", teardown);
for (const sig of ["SIGINT", "SIGTERM"]) {
    process.on(sig, () => {
        teardown();
        process.exit(130);
    });
}
// 未捕获异常/未处理拒绝:先打印再收尾,否则 Chrome 会跟着一起漏。
//
// 这一行要真的到得了人眼前,两件事都得对(第 10/11 轮复审,**都是核过源码才写的**):
//
//   · **写法**:用 `writeSync(2, …)` 而不是 `console.error` —— `process.exit()` 不等挂起的
//     异步写,stdout/stderr 被重定向成管道/文件时(gates 正是 `$out = (& node …)`)最后
//     那行**可能被截断**。`writeSync` 直写 fd 2,退不退出都不会丢;
//     stderr 到不到得了 gates:`scripts/gates.ps1:387` 是 `(& node $f.FullName 2>&1)`,
//     **stderr 已并进 stdout**,所以写 fd 2 没问题。
//   · **前缀必须含 `[FAIL]`**:gate 3e 红时打解释行的那句是
//     `scripts/gates.ps1:398` 的 `Select-String -Pattern '\[FAIL\]'` ——
//     **不带 `^` 锚点的子串匹配**。于是:
//       (a) 缩进**从来不影响**(本文件 `check()` 打的就是 `  [FAIL] …`,照样被抓到);
//       (b) 而 `[FATAL]` **不含子串 `[FAIL]`**(F-A-T-A-L ≠ F-A-I-L)—— 第 10 轮我
//           写成 `[FATAL]` 并声称「靠去掉缩进解决」,**两句都错**:缩进不是问题,
//           而那个前缀让这条被整条过滤,gate 3e 只会打一行文件名、零解释行。
//     所以这里打 `[FAIL] FATAL …`:含 `[FAIL]` 走同一条渠道(并计入那 20 行上限),
//     后面的 `FATAL` 保住「这不是普通断言失败,是整套死了」的语义。
//     [J96] 之后本地 gates 是子 PR 上唯一的门,这条诊断丢了就真的没有别处能看。
for (const ev of ["uncaughtException", "unhandledRejection"]) {
    process.on(ev, (e) => {
        const msg = e && e.message ? e.message : String(e);
        try {
            writeSync(2, `  [FAIL] FATAL ${ev}:${msg}\n`);
        } catch {
            console.error(`  [FAIL] FATAL ${ev}:`, msg);
        }
        teardown();
        process.exit(1);
    });
}

async function waitFor(expr, ms = 20000) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {
        let v = null;
        try {
            v = await evaluate(expr);
        } catch {
            v = null;
        }
        if (v) return true;
        await sleep(120);
    }
    return false;
}

// 真源文档在 iframe 里(壳页只有工具条);一切选择器走它。
const IN = (js) => `(() => {
    const f = document.querySelector("iframe");
    const w = f && f.contentWindow;
    const d = f && f.contentDocument;
    if (!w || !d) return null;
    const q = (s) => d.querySelector(s);
    const gb = (n) => q('[data-gb="' + n + '"]');
    const vis = (el) => !!el && !el.hidden;
    ${js}
})()`;

const READY = IN(`
    const lane = gb("wave-lane-2-label");
    return !!(lane && lane.textContent && lane.textContent.length > 0);
`);

// ---------------------------------------------------------------- 启动
const CDP_WAIT_TRIES = 300;
const CDP_WAIT_STEP_MS = 200;
let targets = null;
for (let i = 0; i < CDP_WAIT_TRIES && !targets; i++) {
    try {
        const res = await fetch(`http://127.0.0.1:${CDP_PORT}/json/list`);
        const list = await res.json();
        targets = list.find((t) => t.type === "page") ? list : null;
    } catch {
        await sleep(CDP_WAIT_STEP_MS);
    }
}
if (!targets) {
    noBrowser(
        `Chrome 未在 ${Math.round((CDP_WAIT_TRIES * CDP_WAIT_STEP_MS) / 1000)}s 内开出 CDP 端口`,
    );
}
cdp = cdpConnect(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
await cdp.ready;
await cdp.send("Page.enable");
await cdp.send("Runtime.enable");
cdp.on((m) => {
    if (m.method === "Runtime.exceptionThrown") {
        const d = m.params.exceptionDetails;
        exceptions.push(
            (d.exception?.description || d.text || "").split("\n")[0],
        );
    } else if (
        m.method === "Runtime.consoleAPICalled" &&
        m.params.type === "error"
    ) {
        errors.push(
            (m.params.args || [])
                .map((a) => a.value ?? a.description ?? "")
                .join(" "),
        );
    }
});
// `.wave-diff__items` 的 max-height —— **从 CSS 里正则读**,不抄副本(复审第 2 轮)。
// CSS 常量 import 不了,但读法与 smoke-mock.mjs 那条封顶对拍同源:注释绑不住,人会漏。
// 第一版这里写死 168 并在注释里叮嘱「改那边必须同步改这里」—— 那正是本 PR 自己反对的形态。
const ITEMS_MAX_H = (() => {
    const css = readFileSync(join(ROOT, "web/output/index.html"), "utf8");
    const m = css.match(/\.wave-diff__items\s*\{[^}]*?max-height:\s*(\d+)px/s);
    if (!m) {
        throw new Error(
            "在 web/output/index.html 里找不到 .wave-diff__items 的 max-height —— " +
                "规则改名/改写了就同步改这条正则,别让判据 (2) 静默空过",
        );
    }
    return Number(m[1]);
})();
// 两档自动收起时长:**从被测源码直接取**,不抄副本(#179 复审【建议】)。
// 抄一份常量的后果是「改了 tab-wave.js 却忘了改这里」时本套仍然绿 —— 那两个数是**被测
// 行为**本身,副本一旦漂移,(4a)/(4c) 断的就不再是页面实际的收起时长。
const TW = await import(
    pathToFileURL(join(ROOT, "web/output/tab-wave.js")).href
);
const { DIFF_HIDE_MS, DIFF_HIDE_OPEN_MS, DIFF_CHANGED_CAP } = TW;
if (
    !Number.isFinite(DIFF_HIDE_MS) ||
    !Number.isFinite(DIFF_HIDE_OPEN_MS) ||
    !Number.isFinite(DIFF_CHANGED_CAP)
) {
    // 常量被改名/删掉时,`undefined` 会让 sleep 立刻返回、断言全部空过 —— 宁可当场炸。
    throw new Error(
        "tab-wave.js 未导出 DIFF_HIDE_MS / DIFF_HIDE_OPEN_MS / DIFF_CHANGED_CAP —— " +
            "改名了就同步改这里,别让本套静默空过",
    );
}

log(`(站点根 ${ROOT} -> ${base};CDP ${CDP_PORT})`);
log("=== SL-274 diff 摘要折叠 + 泳道保底 —— 页面级 ===");

await cdp.send("Page.navigate", { url: `${base}/web-preview/output.html` });
check(await waitFor(READY), "页面装载并吃到首帧段表");
await evaluate(
    IN(`const b = gb("tabnav-wave"); if (b) b.click(); return true;`),
);
await sleep(600);

// ⚠ `clientHeight` 量的是**盒子**,不是「看得见」。本面板是
// `#content > section[data-tab-panel="wave"] { overflow: hidden }`,工具条一旦长过整栏,
// 泳道窗会保住自己的盒子高度却被整体推到裁切线以下 —— 只看 clientHeight 的断言
// 在那种情形下照绿,而用户屏幕上什么都没有。所以另量一个 `lanesVisibleH`:
// 泳道矩形与面板可视矩形的**交集高度**,那才是眼睛看得到的那几个像素。
const MEASURE = IN(`
    const lanes = gb("wave-lanes");
    const win = d.querySelector(".wave-window");
    const bar = d.querySelector(".wave-toolbar");
    const panel = d.querySelector('section[data-tab-panel="wave"]');
    const diff = gb("wave-diff-list");
    const det = gb("wave-diff-details");
    const sum = gb("wave-diff-summary");
    const items = gb("wave-diff-list-items");
    let visibleH = -1;
    if (lanes && panel) {
        const a = lanes.getBoundingClientRect();
        const b = panel.getBoundingClientRect();
        visibleH = Math.max(0, Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top));
    }
    return {
        lanesH: lanes ? lanes.clientHeight : -1,
        lanesVisibleH: visibleH,
        winH: win ? win.clientHeight : -1,
        barH: bar ? bar.clientHeight : -1,
        // 判据 (3) 的**前置条件**:视口真的被压窄了吗。不量这个,
        // setDeviceMetricsOverride 一旦没生效,(3) 就退化成再量一遍常规视口而照绿。
        vpH: w.innerHeight,
        outerVpH: window.innerHeight,
        panelH: panel ? panel.clientHeight : -1,
        diffShown: !!(diff && !diff.hidden),
        open: !!(det && det.open),
        summary: sum ? String(sum.textContent || "").trim() : "",
        liCount: items ? items.querySelectorAll("li").length : -1,
        itemsClientH: items ? items.clientHeight : -1,
        itemsScrollH: items ? items.scrollHeight : -1,
    };
`);

// 走**真路径**触发一次松手重分段:Tab3 的 SENSITIVITY 杆按方向键,250ms 静默视为松手
// (tab-wave.js 的 SLIDER_KEY_RELEASE_MS),mock 再走 300ms 防抖发 2.8 事件。
// 不合成 PointerEvent:键盘这条路本来就是 role="slider" 的可达性接线,更稳也更短。
const NUDGE = IN(`
    const box = gb("wave-seg-sensitivity");
    if (!box) return "no-slider";
    const track = box.querySelector('[role="slider"]');
    if (!track) return "no-track";
    track.focus();
    for (let i = 0; i < 3; i++) {
        track.dispatchEvent(new w.KeyboardEvent("keydown",
            { key: "ArrowRight", bubbles: true, cancelable: true }));
    }
    return "ok";
`);

const OPEN = (v) =>
    IN(
        `const det = gb("wave-diff-details"); if (det) det.open = ${v}; return true;`,
    );

const base0 = await evaluate(MEASURE);
check(base0.lanesH > 0, `基线:泳道滚动区有高度(实得 ${base0.lanesH}px)`);
check(!base0.diffShown, "基线:diff 摘要条未弹出");

eq(await evaluate(NUDGE), "ok", "SENSITIVITY 杆推得动");
await sleep(1500);
const m1 = await evaluate(MEASURE);
log("  [松手重分段后] " + JSON.stringify(m1));

// ---- (1) 默认折叠(且明细**确实有数据**,不是「没东西所以没撑开」)------------
check(m1.diffShown, "(1) diff 摘要条弹出");
check(!m1.open, "(1) 明细默认**折叠**(details.open 为假)");
check(
    m1.liCount >= 20,
    `(1) 明细条目已在 DOM 里且量是真机量级(实得 ${m1.liCount} 条,要求 ≥20;` +
        "mock 的 changed 封顶与 native 的 kMaxChangedItems 同为 200,实测这份素材出 29 条。" +
        "这一条守的是**素材本身够不够压穿工具条** —— 数量掉回旧的展示档 8 条," +
        "下面 (2)(3) 会因为撑不满而全部变成无牙的空过)",
);
check(
    m1.summary.length > 0 && /\d/.test(m1.summary),
    `(1) 折叠头出了计数(实得 "${m1.summary}")`,
);
check(
    m1.lanesH > 0 && m1.lanesVisibleH > 0,
    `(1) 折叠态下泳道**看得见**(盒高 ${m1.lanesH}px / 可视 ${m1.lanesVisibleH}px;修复前这里是 0)`,
);

// ---- (2) 展开封顶:明细自己滚,不把工具条撑穿 --------------------------------
await evaluate(OPEN("true"));
await sleep(250);
const m2 = await evaluate(MEASURE);
log("  [展开明细] " + JSON.stringify(m2));
check(m2.open, "(2) 明细已展开");
check(
    m2.itemsClientH > 0 && m2.itemsClientH <= ITEMS_MAX_H + 2,
    `(2) 明细区高度被 max-height 封顶(实得 ${m2.itemsClientH}px,上限 ${ITEMS_MAX_H}px)`,
);
check(
    m2.itemsScrollH > m2.itemsClientH,
    `(2) 明细区**真的在内部滚动**(scrollHeight ${m2.itemsScrollH} > clientHeight ${m2.itemsClientH})`,
);
check(
    m2.lanesH > 0 && m2.lanesVisibleH > 0,
    `(2) 展开态下泳道**看得见**(盒高 ${m2.lanesH}px / 可视 ${m2.lanesVisibleH}px)`,
);

// ---- (4) 展开态用长档收起,折叠态用短档 --------------------------------------
// 两个方向都要断,否则「展开不收起」与「整条自动收起被废掉」分不开。
await sleep(DIFF_HIDE_MS + 900);
const m4 = await evaluate(MEASURE);
check(
    m4.diffShown && m4.open,
    `(4a) 展开态跨过折叠档 DIFF_HIDE_MS(${DIFF_HIDE_MS}ms)后摘要条**仍在屏上**` +
        `(实得 diffShown=${m4.diffShown} open=${m4.open};` +
        "把 armDiffHide 的两档合并回一档 => 这里被撤下 => 红)",
);

// 收回折叠 => 按短档重新起表,过 DIFF_HIDE_MS 应当撤下(证明长档没有把自动收起整条废掉)。
await evaluate(OPEN("false"));
await sleep(DIFF_HIDE_MS + 900);
const m5 = await evaluate(MEASURE);
check(
    !m5.diffShown,
    `(4b) 收回折叠后按短档收起并撤下摘要条(实得 diffShown=${m5.diffShown})`,
);

// 展开态**也必须有上限**:再触发一次,展开后一路等过 DIFF_HIDE_OPEN_MS。
// 这一条钉的是「展开态直接 return 不起表」那个第一版形态 —— 那样这块会永远挂在屏上,
// 而它没有关闭钮、后续 reason:"edit" 又进不了 setDiff(见 tab-wave.js armDiffHide 头注)。
eq(await evaluate(NUDGE), "ok", "(4c) 杆推得动(为长档上限再起一轮)");
await sleep(1500);
await evaluate(OPEN("true"));
// 余量 3s(复审第 2 轮:第一版 1.5s)。方向本来就安全 —— 等得越久越容易过 —— 但这套是
// required check,CI runner 上 `setTimeout` 被延后一秒多就会假红,而多等 1.5s 对
// 整套 ~60s 的 wall-clock 几乎没有影响。拿可以忽略的时间换一档确定性。
await sleep(DIFF_HIDE_OPEN_MS + 3000);
const m6 = await evaluate(MEASURE);
check(
    !m6.diffShown,
    `(4c) 展开态跨过长档 DIFF_HIDE_OPEN_MS(${DIFF_HIDE_OPEN_MS}ms)后摘要条被撤下` +
        `(实得 diffShown=${m6.diffShown};展开态改回「不起表」=> 这里永远为 true => 红)`,
);

// ---- (3) 工具条预算:工具条 + 泳道窗必须装得进面板 ---------------------------
// 这是本卡真正的**不变量**,也是唯一一条守得住原缺陷的结构性断言。
//
// ⚠ 两条走不通的路,写在这里省得后人再试一遍:
//   · **压视口没用**。`#card` 是 `height: var(--box-h)` 的固定设计盒(780px,
//     app.js 从 design-box.js 写入),面板高不随视口变。实测把外层视口压到 420:
//     outerVpH 901→420,而 iframe 内 vpH 863→780、panelH 一动不动 555 —— 壳页只是
//     自己长了滚动条。就算直接改 iframe 元素的高把 vpH 压到 520,panelH 仍是 555。
//   · **`.wave-window` 的 min-height 兜底也没用**,所以本卡没有加它:固定设计盒里它
//     永远不承力(没有删之即红的通路),而真溢出时面板是 `overflow: hidden`,
//     泳道窗会保住盒子却被推出裁切线 —— 对用户和压扁是同一件事。逐字见
//     web/output/index.html 该规则的头注。
//
// 于是判据回到能直接量的那件事:**工具条不许把泳道窗挤出面板**。
// 删掉折叠(默认展开)或删掉 `.wave-diff__items` 的 max-height => 29 条明细平铺,
// 工具条从 181/349px 涨到 614px(> 面板 555px)=> 下面三条一起红。
// 前面 (4c) 那一轮已经等到摘要条自动收起了 —— 必须**重新触发一帧 diff**,
// 否则下面量到的是「屏幕上根本没有 diff」的工具条,三条断言全部空过。
eq(await evaluate(NUDGE), "ok", "(3) 杆推得动(为工具条预算再起一帧 diff)");
await sleep(1500);
const m3 = await evaluate(MEASURE);
log("  [工具条预算 · 折叠态] " + JSON.stringify(m3));
// 前置:diff 真的在屏上、明细真的有量 —— 不满足则本判据量的是空工具条。
check(
    m3.diffShown && !m3.open && m3.liCount >= 20,
    `(3) 前置:摘要条在屏、默认折叠、明细 ${m3.liCount} 条(≥20)`,
);
check(
    m3.barH > 0 && m3.panelH > 0 && m3.barH + m3.winH <= m3.panelH + 2,
    `(3) 折叠态工具条 ${m3.barH}px + 泳道窗 ${m3.winH}px 装得进面板 ${m3.panelH}px`,
);
await evaluate(OPEN("true"));
await sleep(300);
const m3b = await evaluate(MEASURE);
log("  [工具条预算 · 展开态] " + JSON.stringify(m3b));
// 折叠**真的省下了高度** —— 折叠态工具条必须明显矮于展开态。
// (原先这里写的是「折叠态工具条 < 面板的一半」,那个 1/2 没有出处、也与上一条真正的
//  不变量 `barH + winH <= panelH` 不同源:工具条以后合法地多一行,它就会以「折叠没生效」
//  的名义假红。#179 复审【建议】。改成两态自比,provenance 就是折叠这件事本身:
//  删掉折叠 => 默认即展开 => 两态同高 => 这条红。)
check(
    m3.barH > 0 && m3b.barH > m3.barH,
    `(3) 折叠确实压住了工具条(折叠态 ${m3.barH}px < 展开态 ${m3b.barH}px;` +
        "删掉折叠 => 默认就是展开态 => 两态同高 => 红)",
);
check(
    m3b.open && m3b.barH + m3b.winH <= m3b.panelH + 2,
    `(3) 展开态工具条 ${m3b.barH}px + 泳道窗 ${m3b.winH}px 仍装得进面板 ${m3b.panelH}px` +
        "(去掉 max-height => 明细整列铺开 => 破)",
);
check(
    m3b.lanesVisibleH > 0,
    `(3) 展开态泳道**仍看得见**(可视高 ${m3b.lanesVisibleH}px,盒高 ${m3b.lanesH}px)`,
);
await evaluate(OPEN("false"));

// ---- (5) 顶到封顶时折叠头印的是「N+」而不是「N」-------------------------------
// `changed[]` 是**会被截断**的那一个(native `kMaxChangedItems` / web `DIFF_CHANGED_CAP` /
// mock 三处同为 200,同值由 smoke-mock.mjs 上门禁),而 added/removed/kept 是如实总数。
// 顶到封顶时直接印 `changed.length` 会把「至少 200 段改了」说成「正好 200 段」——
// 那是这一行唯一可能撒的谎,所以渲染成「200+」。
//
// **为什么要单开一个 mock 场景**:常态素材只出 29 条,这个分支一条用例都到不了 ——
// 新增的、用户可见的分支没有删之即红的通路,等于没守(#179 复审【重要】)。
// `?scenario=diff-flood` 把 `makeSegments` 的 `diffFillToCap` 打开,抽满 200 条。
// 删掉 tab-wave.js 里那个三元(直接印 `String(nChanged)`)⇒ 折叠头出「200」⇒ 本条红。
log("");
log("=== (5) changed 顶到封顶 => 折叠头印下界「N+」===");
// ⚠ **本套是六套里唯一做第二次导航的**,而这一步正是那个「CDP 响应永远不来」的触发点
// (见 cdpConnect 头注:实测挂 75 分钟)。两道防护缺一不可:
//   · `Page.navigate` 本身走带超时的 `send`,卡住就当场抛,而不是静默等下去;
//   · 导航后**先等 load 事件再等 READY**。只等 READY 也能过,但那是拿 `Runtime.evaluate`
//     去戳一个正在被换掉的渲染器 —— 恰好是丢响应的那个窗口。先收 `Page.loadEventFired`
//     等于让浏览器告诉我们「新 document 已经就位」,再开始求值。
//   · 收不到 load 事件不直接判红:后面 READY 那条(20s 预算、每轮 try/catch)才是判据,
//     这里只负责**别在导航途中求值**。
const loadSeen = (() => {
    let hit = false;
    cdp.on((m) => {
        if (m.method === "Page.loadEventFired") hit = true;
    });
    return () => hit;
})();
await cdp.send("Page.navigate", {
    url: `${base}/web-preview/output.html?scenario=diff-flood`,
});
for (let i = 0; i < 100 && !loadSeen(); i++) await sleep(100);
check(await waitFor(READY), "(5) diff-flood 场景装载并吃到首帧段表");
await evaluate(
    IN(`const b = gb("tabnav-wave"); if (b) b.click(); return true;`),
);
await sleep(600);
eq(await evaluate(NUDGE), "ok", "(5) SENSITIVITY 杆推得动(触发满档那一帧)");
await sleep(1500);
const m5cap = await evaluate(MEASURE);
log("  [满档] " + JSON.stringify(m5cap));
check(
    m5cap.diffShown && m5cap.liCount === DIFF_CHANGED_CAP,
    `(5) 前置:明细恰好顶到封顶 ${DIFF_CHANGED_CAP} 条(实得 ${m5cap.liCount};` +
        "不满档则下面那条断言量的是普通计数、空过)",
);
check(
    m5cap.summary.includes(`${DIFF_CHANGED_CAP}+`),
    `(5) 折叠头把计数印成「${DIFF_CHANGED_CAP}+」而不是「${DIFF_CHANGED_CAP}」` +
        `(实得 "${m5cap.summary}";删掉 tab-wave.js 的 nChanged >= DIFF_CHANGED_CAP 三元 => 红)`,
);
check(
    m5cap.lanesH > 0 && m5cap.lanesVisibleH > 0,
    `(5) 满档折叠态泳道仍看得见(盒高 ${m5cap.lanesH}px / 可视 ${m5cap.lanesVisibleH}px)` +
        " —— 200 条明细正是本卡缺陷的真机量级",
);

// ---- 页面零 console.error / 零未捕获异常(全套通用底线)-----------------------
check(
    exceptions.length === 0,
    `页面零未捕获异常(实得 ${exceptions.length} 条:${exceptions.slice(0, 3).join(" | ")})`,
);
check(
    errors.length === 0,
    `页面零 console.error(实得 ${errors.length} 条:${errors.slice(0, 3).join(" | ")})`,
);

log(fail === 0 ? "\n全绿" : `\n${fail} 条 FAIL`);
teardown();
process.exit(fail === 0 ? 0 : 1);
