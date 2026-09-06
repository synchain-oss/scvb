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
//   ① 用 `Page.addScriptToEvaluateOnNewDocument` 在**文档创建之前**装两个桩 ——
//      这正是 JUCE 注入 `window.__JUCE__` 的同一个时机;
//      · 一个自增的 rAF 计数器(注册最早 ⇒ 每一帧里它都排在页面自己的回调之前);
//      · 一个假的 `window.__JUCE__.postMessage`,收信号时把当时的帧计数记下来;
//      · 一个 DOMContentLoaded 监听(注册最早 ⇒ 早于页面那个),记下 DCL 当时的帧计数。
//   ② 导航到**真页面** web/output/index.html(不是预览壳页:壳页的 iframe 会让
//      addScriptToEvaluateOnNewDocument 的注入面与真机不一致);
//   ③ 断言:收到且**只收到一次** `__scvb__firstFrame`,且
//      `帧计数(信号时) - 帧计数(DCL 时) >= 2`。
//      这个 2 就是「嵌套两层 rAF」的可观测形态:外层回调在 DCL 之后的**下一帧**跑
//      (计数 +1),内层再等一帧(计数 +2)。写成单层 rAF ⇒ 差值恰好是 1 ⇒ 本套变红
//      (删除式实测见 PR 描述)。判据钉的是**差值**不是绝对帧号:首帧在 DCL 之前还是之后
//      随渲染阻塞而变,绝对帧号会假红。
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
    window.__scvbSignals = [];
    window.__JUCE__ = {
        postMessage: function (s) {
            window.__scvbSignals.push({ raw: String(s), frames: window.__scvbFrames });
        },
    };
`;
await cdp.send("Page.addScriptToEvaluateOnNewDocument", { source: PROBE });

log("A. SL-370 「首帧已绘」信号发在首帧之后");
await cdp.send("Page.navigate", { url: `${base}/web/output/index.html` });

// 等信号到位。上界给足:渲染阻塞的两条外链 css 要先到,页面才会出第一帧。
const arrived = await waitFor(
    `(() => (window.__scvbSignals || []).some(
        (m) => m.raw.indexOf("__scvb__firstFrame") >= 0) &&
        window.__scvbDclFrames >= 0)()`,
    20000,
);
check(arrived, "页面发出了 __scvb__firstFrame(20s 内)");

const probe = await evaluate(`(() => ({
    signals: (window.__scvbSignals || []).map((m) => ({
        id: (JSON.parse(m.raw) || {}).eventId,
        frames: m.frames,
    })),
    dcl: window.__scvbDclFrames,
    frames: window.__scvbFrames,
}))()`);

if (check(probe && Array.isArray(probe.signals), "取到桩里的信号记录")) {
    const ff = probe.signals.filter((m) => m.id === "__scvb__firstFrame");
    check(
        ff.length === 1,
        `__scvb__firstFrame 恰好发一次(实得 ${ff.length} 条,全部信号 ` +
            `${JSON.stringify(probe.signals)})`,
    );
    check(probe.dcl >= 0, `DOMContentLoaded 的帧计数取到了(实得 ${probe.dcl})`);
    if (ff.length === 1 && probe.dcl >= 0) {
        const delta = ff[0].frames - probe.dcl;
        // 把实测值打出来:红/绿之外还要能读到「差值到底是几」——
        // 删除式跑出来的那个 1 与正常的 2,只有这一行能直接对上。
        log(
            `  帧计数:DCL=${probe.dcl} / 信号=${ff[0].frames} / 现在=${probe.frames} ⇒ Δ=${delta}`,
        );
        check(
            delta >= 2,
            `信号发在 DOMContentLoaded 之后的**第二帧或更晚**(实得 Δ=${delta};` +
                `Δ=1 就是单层 rAF —— 回调跑在本帧提交之前,信号早于首帧,` +
                `C++ 放回来的仍是一块没画上东西的 WebView)`,
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
