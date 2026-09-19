// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 「首帧已绘」上行信号的**页面级**冒烟(SL-370)
// =============================================================================
// 守的是开窗遮挡闸的第一条放行路。C++ 侧在导航开始后把 WebView 子窗口挪出宿主可视区、
// 由 WebViewHost::paint 铺占位底色,收到 `__scvb__firstFrame` 才放回来;机理只写在
// src/plugin-common/WebViewRevealGate.h 一处,这里不复述。
//
// 为什么必须页面级:node 侧(smoke-embedded-resources.mjs 的 ⑦)只能断**源码形态** ——
// 「有两层嵌套的 requestAnimationFrame」。而这条信号的全部价值在于**时序**:它到底是不是
// 在页面已经合成过一帧之后才发出去的。源码形态对、时序错(比如 rAF 被谁改成了 setTimeout、
// 或者外层回调里提前 return),⑦ 照样全绿,而真机上放回来的仍是一块没画上东西的 WebView ——
// 本仓「三层机检全绿、窗口是白的」栽过三次,这条链不许只有正则看着。
//
// 怎么量(全程不碰页面内部函数):
//   ① 用 `Page.addScriptToEvaluateOnNewDocument` 在**文档创建之前**装几个桩 ——
//      这正是 JUCE 注入 `window.__JUCE__` 的同一个时机;
//      · 一个自增的 rAF 计数器(注册最早 ⇒ 每一帧里它都排在页面自己的回调之前);
//      · 一个假的 `window.__JUCE__.postMessage`,收信号时把当时的帧计数与时刻记下来;
//      · 一个 DOMContentLoaded 监听(注册最早 ⇒ 早于页面那个),记下 DCL 当时的帧计数;
//      · [SL-429] 一个**注册在页面之前**的 PerformanceObserver,记下 first-paint 的
//        帧计数与时刻 —— **基线是它,不是 DCL**(理由见下面 for 循环上方那段)。
//   ② 导航到**三个真页面**(input / output / monitor,不是预览壳页:壳页的 iframe 会让
//      addScriptToEvaluateOnNewDocument 的注入面与真机不一致)。[SL-429] 之前只跑 output,
//      而 output 恰是三页里唯一测不出本缺陷的那个;这一条同时关掉 SL-423。
//   ③ 断言:收到且**只收到一次** `__scvb__firstFrame`;first-paint 记录真的取到了;
//      `信号时刻 − first-paint 时刻 > 0`(A1);`帧计数(信号) − 帧计数(first-paint) >= 2`(A2);
//      [SL-430 前半] 载荷里的 `paintDeltaMs` 取到了、且与本套独立量到的 Δms 一致(A3)。
//      A2 那个 2 就是「嵌套两层 rAF」的可观测形态:外层回调在**下一帧**跑(+1),内层再等
//      一帧(+2)。写成单层 rAF ⇒ 差值 1 ⇒ 本套变红(删除式实测见 PR 描述)。
//      判据钉的是**差值**不是绝对帧号:绝对帧号随渲染阻塞而变,会假红。
//
// 【B. [SL-437] 撤网必须落在 postMessage **之后**,不能落在守卫检查之前】
//   三份 index.html 的 signal() 曾经把 `sent = true` / `clearTimeout(guard)` 写在
//   `__JUCE__` 存在性检查**之前**:守卫不满足(本格用「document 创建时 __JUCE__ 缺席」
//   模拟这一档)时,两行仍然执行 ⇒ 网已经撤了,但 `postMessage` 从没被调用过 ——
//   2.5s 保险(`guard` 那个 setTimeout)本该在网还没撤时再补一次机会,可它已经被清空,
//   永远不会再触发。本格纯黑盒:不读页面内部的 `sent`/`guard` 变量(与本文件其余判据
//   同一条红线),只看**外部可观测的结果**——`__JUCE__` 迟到时,信号最终有没有送到。
//   ⚠ **三页都跑,不是挑一份代表**:这段逻辑是三份 index.html 各自独立写错的同一处
//   顺序(复制粘贴出来的三份,不是共享代码),只测一页测不出另外两页各自被单独改回错误
//   顺序 —— 判例「测接线不只测零件」+「删除式粒度要配得上判据粒度」。
//   删除式:把某一份 index.html 的 `sent = true` / `clearTimeout(guard)` 挪回
//   `postMessage` 调用之前,**只有那一页对应的这一格**必须由绿转红,另外两页仍应全绿
//   (证明三页判据互不串台;见 PR 描述的删除式记录,三份逐一注入过)。
//
// 用法:node web-preview/tests/smoke-first-frame-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;**2 = 环境里没有 Chrome/Edge**(可选依赖缺席不判红,
//   也绝不算通过);**3 = 浏览器在,但这一次没起来 / 没连上**([SL-297],打 [FLAKY-SKIP])。
//
// CDP 那几十行与 smoke-seg-restore-page.mjs 同源(node 内置 fetch + WebSocket,零依赖 ——
// 仓库红线是不引 puppeteer)。同样不抽公共模块,理由见那一套的头注。
// =============================================================================

// =============================================================================

import { spawn } from "node:child_process";
import { createServer } from "node:http";
import {
    existsSync,
    mkdtempSync,
    readFileSync,
    rmSync,
    statSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, extname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

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
const log = (s) => console.log(s);
function check(cond, msg) {
    if (cond) return true;
    fail++;
    console.log(`  [FAIL] ${msg}`);
    return false;
}
function eq(got, want, msg) {
    const a = JSON.stringify(got);
    const b = JSON.stringify(want);
    if (a === b) return true;
    fail++;
    console.log(`  [FAIL] ${msg}\n         实得 ${a}\n         应为 ${b}`);
    return false;
}
function near(got, want, tol, msg) {
    if (Number.isFinite(got) && Math.abs(got - want) <= tol) return true;
    fail++;
    console.log(`  [FAIL] ${msg}: 实得 ${got},应为 ${want}±${tol}`);
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
    // ⚠ CDP 截止时间**按调用点取,不取一个文件级常数**([SL-287],本机实测逼出来的)。
    //
    // 两类调用的合法时长根本不同,一个常数满足不了:
    //   · `waitFor` 内部的 evaluate —— 上界必须**小于该次 waitFor 自己的预算**,
    //     否则一次超时就吃穿整个预算,把「丢一次响应下一轮补上」变成硬红。
    //   · 一次性的直接 evaluate —— 里面可能是**故意跑很久**的在页探针。
    //     实测 smoke-output-dist-page 的 `measureOff` 合法跑满 12s,而同一文件最紧的
    //     `waitFor` 预算也是 12s:两个约束互相矛盾,任何单一常数都满足不了。
    //
    // 所以:`waitFor` 内部的 evaluate 传**本次还剩多少预算**(见下面 waitFor —— 第一版写的是
    // 「预算的一半」,被复审指出那会把耗时落在 (ms/2, ms) 的**合法**调用从过变成必红,
    // 等于新增一类红;按剩余预算取则不改变任何原本能过的行为)。其余调用用这个宽的默认值 ——
    // 它只负责兜住**真挂死**,不负责区分快慢。
    const CDP_DEFAULT_TIMEOUT_MS = 20000;
    return {
        ready,
        on: (fn) => listeners.push(fn),
        send(method, params, timeoutMs) {
            const mid = ++id;
            const budget = timeoutMs || CDP_DEFAULT_TIMEOUT_MS;
            return new Promise((ok, no) => {
                // 每条 CDP 调用都必须有截止时间:原版把 resolve 塞进 `pending` 就返回,
                // 响应不来就**永远不 resolve**。SL-274 在同源的 seg-diff-fold 上实测挂过
                // 75 分钟零输出(Chrome 与 node 都还活着)。
                // ⚠ 因果限定在**当时**:那次还赶上 [SL-277] 拆锁**之前**的形态 ——
                // 整条 gates 被外部目录锁包着,所以一套挂死会把整批 agent 一起堵住。
                // [SL-301] 起 3e 也持 `Local\SCVB-ipc-tests` 了(它的 Chrome 负载会把同机
                // 别人的 gate 6 拖红)⇒ **一套挂死会堵住全场**,不再只停死本轮。
                // 这正是本文件那条 CDP 截止时间与 gates 3e 的 300s/套上界现在更要紧的原因。
                // 那仍然是一整轮,所以超时照加;但别照着旧说法去推断锁的作用域。
                // CI 上则是一路烧到 job 超时才红。
                //
                // 超时**抛错而不重试**:响应不来说明页面或渲染器已经不对了,
                // 重试只会把一个确定的红拖成一个更慢的红。错误里带 method 与 id,
                // 红出来直接指到是哪一条卡住。
                const timer = setTimeout(() => {
                    pending.delete(mid);
                    no(
                        new Error(
                            `CDP 调用超时 ${budget}ms:${method}(id=${mid})—— ` +
                                "响应没回来。多半是这一步之前的导航把渲染器换掉了;" +
                                "**不要**改成重试或调大超时,那只是把红拖慢",
                        ),
                    );
                }, budget);
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
    console.error(
        `❌ ${msg}\n` +
            "   页面级冒烟无法运行(退出码 2)。这**不是**通过:装一个 Chrome/Edge," +
            "或用 --chrome=<路径> 指定。",
    );
    try {
        server.close();
    } catch {}
    process.exit(2);
}

// [SL-297] **浏览器在,但没起来 / 没连上** —— 与 `noBrowser()` 分开走 **退出码 3**。
// 为什么必须分开:gates 3e 与 CI 都把 **2 读成「本机没有浏览器」** 并按可选依赖记 SKIP、
// 照算 PASS。而「装着 Chrome、这一次没连上」是**一次失败的运行**,不是缺依赖 ——
// 压成同一个码之后,一台装着 Chrome 的机器上一次瞬时超时就会让整套判据**无声消失**,
// 汇总行还写着全 PASS(SL-293 实测撞到三次,每次掉的套件还不一样)。
// **仍然不判红**(理由见调用点):判红会把每个 PR 卡在与改动无关的环境抖动上。
// 3 的语义就是「这一轮没跑成,而且不是因为没装浏览器」——由 gates 打成 [FLAKY-SKIP]。
function browserFailed(msg) {
    console.error(
        `❌ ${msg}\n` +
            "   页面级冒烟**没跑成**(退出码 3):浏览器是在的,但这一次没起来 / 没连上。" +
            "这**不是**通过,也**不是**「本机没装浏览器」——重跑一次通常就好;" +
            "连续复现请查 CDP 端口占用、机器负载或 Chrome 版本。",
    );
    try {
        server.close();
    } catch {}
    process.exit(3);
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-first-frame-"));
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

// ⚠ 收尾必须走**所有**退出路径,不只是跑完那一条([SL-287])。
// **本文件与另外四套不同:它连 try/finally 都没有** —— 从头到尾是顶层 await 直线代码,
// 收尾全压在末尾那个 `finish()` 上。也就是说在这一套里「只有 happy path 会清理」是字面属实:
// 中途任何一次抛错都会把 headless Chrome 留在机器上。
// 而且信号完全没人接:SL-287 同时给 gates 3e 加了整套超时,超时会向本进程发信号。
//
// ⚠ **本机(Windows)实测的边界,别把这段的作用说大**:
//   · 浏览器进程:node 一死,Windows 会把 spawn 出来的 Chrome 一起收掉 —— 实测原版在
//     SIGTERM 下也能从 10 个进程回到 0。所以在 Windows 上这段对**进程**是双保险,不是唯一解。
//     它真正吃劲的地方是 **Linux**:`web-smoke` 跑在 ubuntu-latest,而 POSIX 下父进程退出
//     **不会**自动收掉 spawn 的子进程 —— 尤其 `smoke-seg-restore-page.mjs` **连 try/finally 都没有**,
//     一次抛错就再没有任何地方会 `chrome.kill()`(本文件是那一套)。
//   · 临时目录:`chrome.kill()` 之后文件句柄未必立刻释放,紧跟的 `rmSync` 在 Windows 上
//     **会失败**,留下一个空壳目录 —— 实测本 PR 版本与原版在注入失败时**同样各留 1 个**。
//     这一点不吹:本机 temp 下现有 981 个 `scvb-*` 残留目录,这段收不干净它们。
//     它保证的是「每条退出路径都**尝试过**收尾」,以及在 Linux 上真的收得掉。
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
// `exit` 处理器只能同步收尾(Node 规范),所以这里不等句柄、不重试 rmSync ——
// 留一个空壳目录是可接受的残渣(系统会清),而**跑着的 headless Chrome 不是**。
process.on("exit", teardown);
for (const sig of ["SIGINT", "SIGTERM"]) {
    process.on(sig, () => {
        teardown();
        process.exit(130);
    });
}
// 未捕获异常 / 未处理拒绝:先打印再收尾,否则 Chrome 会跟着一起漏。
// 新加的 CDP 超时是**定时器里 reject**,那条 promise 当时若没人 await,
// 就会以 unhandledRejection 形式到这里 —— 这一支不是摆设。
for (const ev of ["uncaughtException", "unhandledRejection"]) {
    process.on(ev, (e) => {
        console.error(`  [FATAL] ${ev}:`, e && e.message ? e.message : e);
        teardown();
        process.exit(1);
    });
}

chrome.on("error", (e) => browserFailed(`浏览器启动失败:${e.message}`));

let cdp = null;
const errors = [];
const exceptions = [];

async function evaluate(expression, timeoutMs) {
    const r = await cdp.send(
        "Runtime.evaluate",
        {
            expression,
            returnByValue: true,
            awaitPromise: true,
        },
        timeoutMs,
    );
    if (r.exceptionDetails) {
        throw new Error(
            "页内求值抛错:" +
                (r.exceptionDetails.exception?.description ||
                    r.exceptionDetails.text),
        );
    }
    return r.result?.value;
}

async function waitFor(expr, ms = 20000) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {
        let v = null;
        try {
            // 这次 evaluate 的上界 = **本次 waitFor 还剩多少预算**(留 250ms 收尾),
            // 不是「预算的一半」。第一版写成 ms/2,被复审指出**把语义改窄了**:
            // 一次耗时落在 (ms/2, ms) 区间的**合法**调用,改动前能过、改动后必红 ——
            // 而 monitor 这一套的实测最慢单次是 3020ms、上界只有 5000ms,余量 1.65 倍,
            // 在与别的 job 抢 CPU 的 ubuntu runner 上抖一下就会把慢但合法判成红。
            // (PR 里那次「未复现的 monitor exit=1」很可能就是这个,首要假设。)
            // 按剩余预算取则**不改变任何原本能过的行为**:慢调用可以用掉几乎整个预算,
            // 真挂死仍会在预算到点前被砍断,由下面的 while 条件收尾。
            v = await evaluate(
                expr,
                Math.max(1000, ms - (Date.now() - t0) - 250),
            );
        } catch {
            v = null;
        }
        if (v) return true;
        await sleep(120);
    }
    return false;
}

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
    browserFailed(
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

// ---------------------------------------------------------------- 桩(文档创建之前)
// JUCE 是在文档创建前注入 `window.__JUCE__` 的,`addScriptToEvaluateOnNewDocument`
// 是 CDP 侧同一个时机 —— 页面 <head> 里那段内联脚本一定跑在它之后。
// ⚠ [SL-429] 这个桩自己那条 `tick` 的 rAF 循环**会把帧驱起来**,所以它量到的
//   「帧计数」只说明 rAF 回调跑过几次,**不说明页面画过没画过**。SL-370 那版判据的基线
//   取的是 DOMContentLoaded 的帧计数 —— 于是「信号早于真正的 first-paint」这件事
//   在它眼里完全看不见。改成拿 **paint 记录**当基线:`__scvbPaintFrames` / `__scvbPaintMs`
//   由一个**注册在页面之前**的 PerformanceObserver 落下(本桩先跑 ⇒ 回调也先于页面那个),
//   `first-paint` 才是「这一帧真的画上去了」的可观测证据。
const PROBE = `
    window.__scvbFrames = 0;
    (function tick() {
        window.__scvbFrames++;
        requestAnimationFrame(tick);
    })();
    window.__scvbDclFrames = -1;
    document.addEventListener("DOMContentLoaded", function () {
        window.__scvbDclFrames = window.__scvbFrames;
    });
    window.__scvbPaintFrames = -1;
    window.__scvbPaintMs = -1;
    try {
        var __po = new PerformanceObserver(function (list) {
            if (window.__scvbPaintFrames >= 0) return;
            var e = list.getEntries()[0];
            if (!e) return;
            window.__scvbPaintFrames = window.__scvbFrames;
            window.__scvbPaintMs = e.startTime;
        });
        __po.observe({ type: "paint", buffered: true });
    } catch (e) {}
    window.__scvbSignals = [];
    window.__JUCE__ = {
        postMessage: function (s) {
            window.__scvbSignals.push({
                raw: String(s),
                frames: window.__scvbFrames,
                ms: performance.now(),
            });
        },
    };
`;
await cdp.send("Page.addScriptToEvaluateOnNewDocument", { source: PROBE });

// [SL-429] **三页都跑**。SL-370 那版只跑 output —— 而 output 恰好是三页里唯一
// first-paint 天然早于信号的那个(页面重、画得早),所以它是**唯一测不出本缺陷的页面**。
// ⚠ **别把这三页读成「符号 = 用户真机那三比一的判别式」**:本机 12 轮复测里 monitor 修前的
// `信号 − first-paint` 是 −8 .. +73 ms(十有八九为正、且整段落在 C++ 那 kRevealSettleMs
// 放行余量的量级里,[SL-436] 改到 64 ms 后余量更宽,结论不因此削弱),按该模型它根本不该闪,
// 可用户真机上它稳定闪 —— **monitor 的白没有被这条链解释**。
// 三档结论(input 机制已定 / output 天然免疫的对照组 / monitor 未被解释)写在
// src/plugin-common/WebViewRevealGate.h 一处。本套要守的是那条**结构性质**本身:
// 信号必须发在页面真的画过一帧之后。
// ⇒ 把触发改回 DOMContentLoaded 时本套会红,但**哪一页红在哪一格不是常数**(随 first-paint
// 与 DCL 的相对快慢而变,本机就量到过同一页两种形态)。实跑到的形态见 PR 描述的删除式数表,
// 别照抄一个固定答案。
log("A. [SL-370 / SL-429] 「首帧已绘」信号发在页面**真的画过一帧**之后");
for (const role of ["input", "output", "monitor"]) {
    await cdp.send("Page.navigate", { url: `${base}/web/${role}/index.html` });

    // 等信号到位。上界给足:渲染阻塞的两条外链 css 要先到,页面才会出第一帧。
    // ⚠ 三页连跑:`Page.navigate` 只保证导航**开始**。不把 pathname 一起判,这一轮会
    // 撞上**上一页还没换掉**的文档 —— 它的 __scvbSignals 早就满足条件,于是量到的是上一页
    // 的数,而判据名字一点没变(「比对轴会静默变空」那一族的邻居)。
    const arrived = await waitFor(
        `(() => location.pathname.indexOf("/web/${role}/") >= 0 &&
            (window.__scvbSignals || []).some(
                (m) => m.raw.indexOf("__scvb__firstFrame") >= 0) &&
            window.__scvbDclFrames >= 0)()`,
        20000,
    );
    check(arrived, `${role}:页面发出了 __scvb__firstFrame(20s 内)`);

    const probe = await evaluate(`(() => ({
        path: location.pathname,
        signals: (window.__scvbSignals || []).map((m) => ({
            id: (JSON.parse(m.raw) || {}).eventId,
            frames: m.frames,
            ms: m.ms,
            // [SL-430 前半] 页面自己在载荷里报的「信号 − first-paint」差值。
            // 用 ?? null 显式落成 null:字段缺席与「差值恰好是 0」必须分得开
            // (0 是合法值,拿真值判会把它读成缺席 —— 判例 comparison-axis-can-be-silently-hollow)。
            paintDeltaMs:
                ((JSON.parse(m.raw) || {}).payload || {}).paintDeltaMs ?? null,
        })),
        dcl: window.__scvbDclFrames,
        paintFrames: window.__scvbPaintFrames,
        paintMs: window.__scvbPaintMs,
        frames: window.__scvbFrames,
    }))()`);

    if (
        !check(
            probe &&
                Array.isArray(probe.signals) &&
                String(probe.path).indexOf(`/web/${role}/`) >= 0,
            `${role}:取到的是**本页**桩里的信号记录(实得 path=${probe && probe.path})`,
        )
    )
        continue;

    const ff = probe.signals.filter((m) => m.id === "__scvb__firstFrame");
    check(
        ff.length === 1,
        `${role}:__scvb__firstFrame 恰好发一次(实得 ${ff.length} 条,全部信号 ` +
            `${JSON.stringify(probe.signals.map((m) => m.id))})`,
    );
    // paint 记录必须真的取到:取不到时下面两格的差值会恒为正/恒可比,判据静默变空
    // (「比对轴会静默变空」那一族)。用 >= 0 判,不用真值判 —— 0 是合法帧计数。
    check(
        probe.paintFrames >= 0 && probe.paintMs >= 0,
        `${role}:first-paint 记录取到了(实得帧计数 ${probe.paintFrames} / ` +
            `${Math.round(probe.paintMs)}ms;**两个 −1 = 信号已经发了、页面却还没画过任何一帧**,` +
            `这不是探针缺陷 —— 它与下面 Δms 为负是同一件事的两种形态,` +
            `本卡把触发改回 DOMContentLoaded 时 input / output 就落在这一格)`,
    );
    if (ff.length !== 1 || probe.paintFrames < 0) continue;

    // 把实测值打出来:红/绿之外还要能读到「差了多少」——
    // 删除式跑出来的负数与正常的正数,只有这一行能直接对上。
    const dFrames = ff[0].frames - probe.paintFrames;
    const dMs = ff[0].ms - probe.paintMs;
    log(
        `  ${role}:first-paint=${Math.round(probe.paintMs)}ms(第 ${probe.paintFrames} 帧)/ ` +
            `信号=${Math.round(ff[0].ms)}ms(第 ${ff[0].frames} 帧)⇒ ` +
            `Δ帧=${dFrames}、Δms=${Math.round(dMs)};DCL 帧计数=${probe.dcl};` +
            `载荷 paintDeltaMs=${JSON.stringify(ff[0].paintDeltaMs)}`,
    );
    // (A1) 时刻:信号必须**晚于** first-paint。这一格钉的是「按 paint 触发」本身。
    check(
        dMs > 0,
        `${role}:信号发在 first-paint **之后**(实得 Δms=${Math.round(dMs)};` +
            `负值 = 页面一帧都还没画就报了「首帧已绘」,C++ 放回来的是一块没画上东西的 ` +
            `WebView —— [SL-429] 的第二段白)`,
    );
    // (A2) 帧数:再等两层 rAF。这一格钉的是「嵌套两层」,与 (A1) 各守一件事:
    // 只改触发点、不改 rAF 层数时 (A2) 仍会红(paint 与信号同帧 ⇒ Δ帧=0/1)。
    check(
        dFrames >= 2,
        `${role}:信号发在 first-paint 之后的**第二帧或更晚**(实得 Δ帧=${dFrames};` +
            `Δ帧<2 说明少了一层 rAF —— 已绘的那一帧还没提交给合成器)`,
    );
    // (A3) [SL-430 前半] 载荷里那个 `paintDeltaMs` 诊断字段 —— **三格,缺一不可**。
    //
    // 它的用途是让用户机的一份日志能直接读出「信号 − first-paint」,而不是像 SL-429 那样
    // 从 A/B 差值反推。所以它必须**真的是那个量**,不能只是「有个数在那儿」:
    //   · 先断**取到了**(用 `typeof number` + isFinite,**不用真值判** —— 差值恰好是 0
    //     是完全合法的读数,拿真值判会把它误读成缺席;判例同上面那条注释);
    //   · 再断**符号为正**。这一格是**结构性的、不吃机器快慢**:信号在 paint 之后隔了两层
    //     rAF 才发,差值只可能为正;减号写反(或拿 DCL 当基线)在**任何**机器上当场红。
    //   · 最后断**它与本套独立量到的 Δms 对得上**。两边是两条独立的路:页面用它自己那个
    //     PerformanceObserver 的 startTime,本套用**注册得更早的**桩里那个。
    //
    // ⚠ [SL-429 第 4 轮] 容差从 ±20 ms 收到 ±5 ms,**而且符号那一格是新加的** —— 复审指出
    // 原来那条立论(「漏个减号照样全绿 ⇒ 所以要断一致」)是**数据凑出来的**:±20 比它要量的
    // 那个量还大,`|−10 − 10| = 20 ≤ 20` ⇒ 换台快机器(Δms 掉到 10 ms 以内)这一格就变绿。
    // 两个读数取自**同一个同步任务**(页面在 postMessage 前一行取 performance.now(),桩在
    // postMessage 里取),差值本该亚毫秒级,`Math.round` 再加 ±0.5 ⇒ 5 ms 是宽松上界。
    //
    // C++ 那一侧(读载荷 + 拼日志)**没有任何判据**:WebViewHost.cpp 不进任何测试目标,
    // 这是本仓既有的空白,不是本卡新开的口子 —— 照实说,别假装它被守着。
    // 但**字段名**那一侧有:⑦ 的 (e) 从 WebViewHost.h 抓 `kFirstFramePaintDeltaKey` 与三页
    // 逐字对拍,而且那一套不需要浏览器、永远会跑(本套在无浏览器时会整套 [SKIP])。
    const reported = ff[0].paintDeltaMs;
    const reportedOk =
        typeof reported === "number" && Number.isFinite(reported);
    check(
        reportedOk,
        `${role}:信号载荷里带了 paintDeltaMs(实得 ${JSON.stringify(reported)};` +
            `缺席 = 用户机上那份日志会打 "(no paint record)",拿不到余量读数。` +
            `⚠ 别只往「赋值被删了」上想:**走 2.5s 保险路时页面也可能不带它** ——` +
            `判别式是信号时刻 ≈ 2500ms,本轮实得 ${Math.round(ff[0].ms)}ms)`,
    );
    if (reportedOk) {
        check(
            reported > 0,
            `${role}:载荷里的 paintDeltaMs 必须为正(实得 ${reported};` +
                `负值 / 零 = 页面那行算式的减号写反了,或基线取的根本不是 first-paint)`,
        );
        near(
            reported,
            dMs,
            5,
            `${role}:载荷里报的 paintDeltaMs 与本套独立量到的 Δms 一致`,
        );
    }
}

// ---------------------------------------------------------------- B. [SL-437]
log(
    "\nB. [SL-437] __JUCE__ 迟到时,信号必须靠 2.5s 保险补发,不能被提前撤掉的网吞掉",
);
// ⚠ **三份 index.html 都跑**,不是挑一份代表:SL-437 的缺陷是「三份文件各自独立写错了
// 同一段逻辑」(复制粘贴出来的三份撤网顺序),不是「一处共享代码错了」。只测一页测不出
// 另外两页各自被单独改回错误顺序 —— 判例「测接线不只测零件」+「删除式粒度要配得上判据
// 粒度」,PR 描述里对三份文件各自做过删除式(改一份、只有那一份对应的这一格转红)。
// 追加一段在文档创建时删掉 `window.__JUCE__` 的脚本 —— 上面 PROBE 已经把它注册在
// 文档创建前,这一段注册得更晚,同一时机里跑在 PROBE **之后**,净效果是「先装 → 再拆」,
// 把「守卫检查会失败」这个前提做成黑盒可控的。只需注册一次,对三次导航都生效。
await cdp.send("Page.addScriptToEvaluateOnNewDocument", {
    source: `delete window.__JUCE__;`,
});
for (const role of ["input", "output", "monitor"]) {
    await cdp.send("Page.navigate", {
        url: `${base}/web/${role}/index.html`,
    });
    const paintedNoJuce = await waitFor(
        `(() => location.pathname.indexOf("/web/${role}/") >= 0 &&
            window.__scvbPaintFrames >= 0)()`,
        20000,
    );
    check(
        paintedNoJuce,
        `${role}:B:__JUCE__ 缺席时页面仍然画出了首帧(first-paint 记录取到了)`,
    );
    // 留出双层 rAF 的时间,让 armOnce() 的正常路径真的尝试过 signal()(此刻必然因为
    // `__JUCE__` 缺席而提前 return)。这一步只是把前提做实,还不是判据本身。
    await sleep(300);
    const beforeInject = await evaluate(
        `(() => (window.__scvbSignals || []).length)()`,
    );
    check(
        beforeInject === 0,
        `${role}:B:__JUCE__ 缺席期间没有任何信号被记录(实得 ${beforeInject} 条;` +
            "这一步只是确认前提成立,不是本格的判据本身)",
    );
    // 补上一个能用的 __JUCE__ —— 模拟「controller 建好、__JUCE__ 真正就位」比两层 rAF
    // 晚到达的那种时序。之后**不再**主动调用页面里的任何函数,只等 guard 的 2.5s
    // setTimeout 自己触发。
    await evaluate(`(() => {
        window.__JUCE__ = {
            postMessage: function (s) {
                window.__scvbSignals.push({
                    raw: String(s),
                    frames: window.__scvbFrames,
                    ms: performance.now(),
                });
            },
        };
    })()`);
    // guard 是 2500ms;给够余量等它触发,同时别把上界拉到会拖慢 CI 的地步。
    const delivered = await waitFor(
        `(() => (window.__scvbSignals || []).length > 0)()`,
        4000,
    );
    check(
        delivered,
        `${role}:B:__JUCE__ 迟到之后,2.5s 保险最终还是把信号送出去了(实得:` +
            "delivered=" +
            delivered +
            ";红 = 撤网提前发生,保险已经空转,信号永远发不出去了 —— 正是 SL-437 那个缺陷)",
    );
    if (delivered) {
        const afterInject = await evaluate(
            `(() => (window.__scvbSignals || []).map((m) => (JSON.parse(m.raw) || {}).eventId))()`,
        );
        eq(
            afterInject,
            ["__scvb__firstFrame"],
            `${role}:B:补发的信号**恰好一条**、且是 __scvb__firstFrame(不是重复发送,` +
                "也不是别的事件)",
        );
    }
}

// 页面自己的运行期噪声只报不判:这一套没有 mock 后端,app.js 拿不到桥是预期内的,
// 拿它判负会把一条与本判据无关的失败混进来。
if (exceptions.length > 0 || errors.length > 0)
    log(
        `  (提示)页内噪声 ${exceptions.length} 抛错 / ${errors.length} console.error —— ` +
            "本套无 mock 后端,不判负",
    );

console.log(`\n=== 结果:${fail === 0 ? "全部通过" : fail + " 项失败"} ===`);
teardown();
process.exit(fail === 0 ? 0 : 1);
