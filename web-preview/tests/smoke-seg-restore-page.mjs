// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 段级「恢复自动」的作用域 **页面级**冒烟(SL-242)
// =============================================================================
// 用户实测(v5.6.2 终验 A7,Cubase 15 Pro):在段检查器里点**一个小片段**的
// 「恢复自动」,结果整个大片段被合并、全部回了自动。定谳:那枚钮发的是**轨级**
// scope(`{tracksMask}`,不带范围),而它就长在这一段的锁定开关底下 —— 入口的
// 位置承诺的是「这一段」,行为却是整轨。
//
// 为什么必须页面级:node 侧能断的只有「纯函数算得对」(segmentRestoreScope)与
// 「mock 收到范围会照做」(smoke-tab3 的 ⑥)。**这两条都绕开了真正出事的那一段**
// —— 「用户在屏幕上看到的这一段」到底有没有变成 analyze 的入参。中间隔着点选、
// 段身份重绑、检查器渲染、两态确认三步,任何一步串了段,前两条照样全绿。
// 本仓「三层机检全绿、窗口是白的」已经栽过三次,这条链不许只有正则看着。
//
// 判据取**检查器屏幕上写着的起止时间**:那是用户读到的「这一段是哪一段」。
// 断言 = 页面真发出去的 analyze scope 必须与它逐毫秒对上。任何一环选错段,
// 这两个数就对不上 —— 这是本套存在的全部理由。
//
// 跑什么(全程只用真 DOM 事件,不调页面内部函数):
//   ① 在泳道上真点一下选中一个段 → 检查器出「起 / 止」;
//   ② 用检查器自己的锁定开关把段解锁(手动段默认 locked=true,而 clearManual
//      对 locked 免疫,§1.6)⇒「恢复自动」两态入口出现;
//   ③ 点「恢复自动」→「继续」,拦下页面真发出去的 `analyze(scope, opts)`;
//   ④ 断言 scope = `{tracksMask: 1<<(ch-1), startS, endS}` 且起止与检查器逐毫秒
//      相符、`opts.clearManual === true`;**并且 scope 不是轨级**(不带范围那种);
//   ⑤ 反向:同一页上点 Tab3 工具条的「重新识别(含手动段)」走的是选区/全量那条,
//      不受本卡影响 —— 证明 ④ 的绿不是「页面把每个 analyze 都改成段级」蒙的;
//   ⑥ 全程零未捕获异常、零 console.error。
//
// 用法:node web-preview/tests/smoke-seg-restore-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;**2 = 环境里没有 Chrome/Edge**(口径同
//   smoke-output-stale-page.mjs 与 CLAUDE.md §6:可选依赖缺席不判红,也绝不算通过);
//   **3 = 浏览器在,但这一次没起来 / 没连上**([SL-297],见 `browserFailed()`)——
//   同样不判红,但在 gates 汇总里打 `[FLAKY-SKIP]`,免得「没跑成」被读成「跑过了」。
//
// CDP 那 30 行与 smoke-output-stale-page.mjs 同源(node 内置 fetch + WebSocket,
// 零依赖 —— 仓库红线是不引 puppeteer)。同样不抽公共模块:那份断的是提示面的显隐,
// 本份断的是一次写面调用的入参,合并只会让两边被对方的参数面绑住。
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-seg-restore-"));
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

// mm:ss.mmm → 秒。检查器屏幕上写着的那个数,就是用户读到的「这一段是哪一段」。
function parseTimeMs(txt) {
    const m = /^(\d{2}):(\d{2})\.(\d{3})$/.exec(String(txt || "").trim());
    if (!m) return NaN;
    return Number(m[1]) * 60 + Number(m[2]) + Number(m[3]) / 1000;
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
log(`(站点根 ${ROOT} → ${base};CDP ${CDP_PORT})`);
log("=== SL-242 段级「恢复自动」作用域 —— 页面级 ===");

await cdp.send("Page.navigate", { url: `${base}/web-preview/output.html` });
check(await waitFor(READY), "页面装载并吃到首帧段表");
// 切到「波形与分段」页:检查器的渲染在 tab-wave 的 render 里。
await evaluate(
    IN(`const b = gb("tabnav-wave"); if (b) b.click(); return true;`),
);
await sleep(500);

// ---- [SL-382] 分段灵敏度滑杆不产生布局盒(节点还在,但不渲染)-----------------
//
// 节名与下面 `check()` 的断言文本**逐字相同**,别再起第二个叫法:别处(HTML / tab-wave.js)
// 的判据指针就是拿这句话去 grep 的,改了名那些指针当场失效(#251 复审就抓到过一次
// 指错文件的假句)。用户裁定 2026-09-10:分段灵敏度这个功能暂时不做,先把杆藏起来。
//
// 搭本套的车而不另开一套:这条判据要的前置条件与本套完全一样(输出页装载 + 切到
// 「波形与分段」页),而那段 CDP/无头 Chrome 的脚手架是本套里最贵的部分。
//
// **为什么必须页面级、不能在 smoke-tab3-interactions.mjs 里断**:那套断的是纯函数与
// 源码字面,`getClientRects()` 在无 DOM 环境里根本不存在。而这条缺陷的落点恰恰在
// 布局引擎上 —— `.wave-slider` 自己设了 `display:flex`,**UA 表的 `[hidden]{display:none}`
// 压不过它**;本页能藏住,靠的是 output/index.html 第 351 行那条作者层
// `[hidden]{display:none !important}` 兜底。只断 `el.hidden === true`(本文件的 `vis()`
// 就是这么写的)会在「属性挂上了但样式没生效」时**全绿**,那正是要防的那一种。
// 所以这里量的是**真实布局盒**:`getClientRects().length`。
//
// 两条一起断,`minlen` 那条是**对照**:证明我藏的是灵敏度那一根,而不是把整个
// `wave-toolbar__group--seg` 组连坐了(只断前者的话,把整组删掉也照样绿)。
//
// 删除式:拿掉 output/index.html 里 `data-gb="wave-seg-sensitivity"` 那个 div 上的
// `hidden` ⇒ 灵敏度杆量到非零盒 ⇒ 第一条红。
const SEG_SLIDER_BOXES = IN(`
    const box = (n) => {
        const el = gb(n);
        if (!el) return -1;                       // -1 = 节点根本不在(与「藏起来」区分开)
        return el.getClientRects().length;        // 0 = 不产生布局盒(display:none)
    };
    return JSON.stringify({ sens: box("wave-seg-sensitivity"), minlen: box("wave-seg-minlen") });
`);
{
    const raw = await evaluate(SEG_SLIDER_BOXES);
    const boxes = JSON.parse(raw || "{}");
    log(
        `  [SL-382] 滑杆布局盒:sensitivity=${boxes.sens} / min_segment=${boxes.minlen}`,
    );
    // 节点仍在 DOM 里(隐藏 ≠ 删除:契约 §1.19 仍整包下发 sensitivity 字段)。
    check(
        boxes.sens === 0,
        "[SL-382] 分段灵敏度滑杆不产生布局盒(节点还在,但不渲染)",
    );
    check(
        boxes.minlen > 0,
        "[SL-382] 对照:最短段长滑杆照常渲染(没有把整个分段组连坐)",
    );
}

// ---- [SL-392] 波形与分段页那排滑杆:六根可见的必须逐像素等宽 -------------------
//
// 用户 v5.6.12 回验:灵敏度那根被 [SL-382] 藏掉之后,MIN SEG **撑成一整行**,与左边
// 五根 VAD 明显不齐。根因是纯 CSS 记账:`.wave-toolbar__group--seg` 的
// `flex: 2 1 var(--sp-16)` 是按**两根**滑杆写的(grow = 根数、basis = 内部间隙总宽),
// 藏掉一根之后组里只剩 1 根、0 个内部间隙,那份 `2` 就成了独吞两份。
// 实测(修前):五根 VAD 各 **106.28px**,MIN SEG **228.58px**。
//
// **为什么必须页面级**:这条判据的全部内容就是布局引擎算出来的宽度。node 侧那套
// (smoke-tab3-interactions.mjs)断的是纯函数与源码字面,`getBoundingClientRect()` 在
// 无 DOM 环境里根本不存在 —— 与 [SL-382] 那一节同理由,判例见本文件上方那段。
//
// **容差 1px 的来历**:实测六根是 126.66 / 126.67 交替,差 0.01px,那是分数像素舍入,
// 不是不齐。取 1px 既盖得住舍入,又离缺陷态(差 **122.3px**)有两个数量级的余量 ——
// 别因为哪天差了 0.02 就把它调大,那说明真的有东西变了。
//
// **先断根数再断等宽**:只断「max−min ≤ 1」的话,哪天有人把 MIN SEG 也藏了、只剩五根
// VAD,那五根天然等宽 ⇒ 照样绿,而工具条已经少了一半。所以 `visible.length === 6` 是
// 前提断言,不是装饰。
//
// 删除式:把 `web/output/index.html` 的 `.wave-toolbar__group--seg` 改回
// `flex: 2 1 var(--sp-16)` ⇒ MIN SEG 回到 228.58px ⇒ 本节红。
const SLIDER_WIDTHS = IN(`
    const names = ["wave-vad-threshold", "wave-vad-hysteresis", "wave-vad-hangover",
                   "wave-vad-paddingpre", "wave-vad-paddingpost", "wave-seg-minlen"];
    const out = [];
    for (const n of names) {
        const el = gb(n);
        if (!el) continue;
        // 只收真的在渲染的那些(藏起来的不占布局盒,宽度恒 0,不该拉低 min)。
        if (el.getClientRects().length === 0) continue;
        out.push({ n: n, w: el.getBoundingClientRect().width });
    }
    return JSON.stringify(out);
`);
{
    const visible = JSON.parse((await evaluate(SLIDER_WIDTHS)) || "[]");
    const widths = visible.map((v) => Math.round(v.w * 100) / 100);
    log(`  [SL-392] 六根可见滑杆宽度 = ${JSON.stringify(widths)}`);

    // 前提:可见的正好六根(五根 VAD + MIN SEG)。少了任何一根都要红 —— 否则「剩下的
    // 恰好等宽」会把「工具条少了一半」读成通过。
    check(
        visible.length === 6,
        `[SL-392] 前提:波形与分段页可见滑杆恰为 6 根(实得 ${visible.length} 根)`,
    );

    if (visible.length === 6) {
        const max = Math.max(...widths);
        const min = Math.min(...widths);
        check(
            max - min <= 1.0,
            `[SL-392] 六根可见滑杆逐像素等宽(极差 ${Math.round((max - min) * 100) / 100}px,` +
                `上界 1px;修前 MIN SEG 228.58 / VAD 106.28,极差 122.3px)`,
        );
    }
}

// ---- 装 analyze 探针 ------------------------------------------------------
// 包在 `window.__SCVB_MOCK__` 上(壳页注进 iframe 的 mock 后端就是它)。用自有属性
// 遮蔽原型上的实现,原实现照常执行 —— 页面的行为一个字节不改,只是把入参留个底。
check(
    await evaluate(
        IN(`
        if (!w.__SCVB_MOCK__ || typeof w.__SCVB_MOCK__.analyze !== "function") return false;
        if (!w.__SCVB_ANALYZE_SPY__) {
            w.__SCVB_ANALYZE_SPY__ = [];
            const inner = w.__SCVB_MOCK__.analyze.bind(w.__SCVB_MOCK__);
            w.__SCVB_MOCK__.analyze = function (scope, opts) {
                w.__SCVB_ANALYZE_SPY__.push({
                    scope: JSON.parse(JSON.stringify(scope === undefined ? null : scope)),
                    opts: JSON.parse(JSON.stringify(opts === undefined ? null : opts)),
                });
                    return inner(scope, opts);
            };
        }
        // 记下重算之后推回来的 scvb.segments 的**段值快照**,按 ch 分桶 —— 本文件 ⑦ 拿它
        // 逐字段比对(重算这一段的 pan/vol 必须回到引擎算出来的自动值)。
        // (这段跑在页面里,注释里不能出现反引号 —— 它会把外层模板串就地截断。)
        // [SL-393] 原先这里还记了一个 chs(push 里点名了哪几条轨)—— 那是**旧轴**:SL-393
        // 定谳把「计算集」与「写回集」分开了,一条 push 里出现的轨不再等于「被重算的轨」,
        // 那个字段既没有消费者、留着还会把下一个人引回旧口径,故删。
        if (!w.__SCVB_SEG_PUSH__) {
            w.__SCVB_SEG_PUSH__ = [];
            w.__SCVB_MOCK__.addEventListener("scvb.segments", (p) => {
                const byCh = {};
                for (const c of (p && p.channels) || []) {
                    byCh[c.ch] = (c.segments || []).map((s) => [
                        s.t0S,
                        s.t1S,
                        s.pan,
                        s.volDb,
                        s.origin,
                        s.locked ? 1 : 0,
                    ]);
                }
                w.__SCVB_SEG_PUSH__.push({
                    reason: p && p.reason,
                    byCh,
                });
            });
        }
        return true;
    `),
    ),
    "analyze 探针装上(mock 后端在 iframe 的 __SCVB_MOCK__ 上)",
);

// ---- ① 在泳道上真点一下,选中一个段 --------------------------------------
// 不算时间→像素:沿泳道横扫若干落点,取**第一个真的把检查器点出来**的那一下。
// 段与段之间有空隙(VAD 静音区没有段),硬算一个 x 反而更脆。
const CLICK = (chIdx, frac) =>
    IN(`
    const lanes = gb("wave-lanes");
    if (!lanes) return null;
    const r = lanes.getBoundingClientRect();
    const HEAD_W = 158, SCALE_COL_W = 44;
    const laneH = (lanes.querySelector('[data-gb="wave-lane-1"]') || {}).offsetHeight || 34;
    const stageW = Math.max(lanes.clientWidth - HEAD_W - SCALE_COL_W, 0);
    const cx = r.left + HEAD_W + stageW * ${frac};
    const cy = r.top + laneH * (${chIdx} - 0.5) - lanes.scrollTop;
    const mk = (type) => new w.PointerEvent(type, {
        bubbles: true, cancelable: true, composed: true,
        clientX: cx, clientY: cy, button: 0, buttons: 1, pointerId: 1,
        isPrimary: true, pointerType: "mouse",
    });
    // 合成 PointerEvent 的 pointerId 不对应任何**活跃指针**,真的 setPointerCapture
    // 会抛 NotFoundError —— 而页面的 pointerdown 处理器第一件事就是 capturePointer,
    // 抛出去整个手势就断在这里,点选永远出不来。桩掉这两个方法是让**页面自己的**
    // 处理器跑完,不是绕过它:被断言的那条链(点选 → 重绑 → 检查器 → 两态确认 →
    // analyze 入参)一步都没少。
    lanes.setPointerCapture = () => {};
    lanes.releasePointerCapture = () => {};
    lanes.dispatchEvent(mk("pointerdown"));
    lanes.dispatchEvent(mk("pointerup"));
    return true;
`);

const INSPECT = IN(`
    const times = gb("inspector-times");
    const start = gb("inspector-time-start");
    const end = gb("inspector-time-end");
    const origin = gb("inspector-origin-value");
    const lock = gb("inspector-locked-toggle");
    const restore = gb("inspector-restore");
    const btn = gb("inspector-restore-btn");
    const ok = gb("inspector-restore-ok");
    const text = gb("inspector-restore-text");
    const panel = gb("segment-inspector");
    return {
        selected: !!panel && panel.getAttribute("data-empty") === "0",
        startTxt: start ? start.textContent.trim() : null,
        endTxt: end ? end.textContent.trim() : null,
        origin: vis(origin) ? origin.textContent.trim() : "",
        locked: lock ? lock.getAttribute("data-on") === "1" : null,
        restoreShown: vis(restore),
        restoreBtnShown: vis(btn),
        restoreOkShown: vis(ok),
        restoreText: text ? text.textContent.trim() : "",
        timesShown: vis(times),
    };
`);

let picked = null;
let pickedCh = 0;
outer: for (const ch of [1, 2, 3, 4, 5]) {
    for (const frac of [0.12, 0.22, 0.32, 0.42, 0.52, 0.62, 0.72, 0.82]) {
        await evaluate(CLICK(ch, frac));
        await sleep(160);
        const st = await evaluate(INSPECT);
        if (st && st.selected && Number.isFinite(parseTimeMs(st.startTxt))) {
            picked = st;
            pickedCh = ch;
            break outer;
        }
    }
}
check(!!picked, "① 泳道上真点一下能选中一个段(检查器出起止时间)");
if (!picked) {
    await finish();
}

const segStartS = parseTimeMs(picked.startTxt);
const segEndS = parseTimeMs(picked.endTxt);
check(
    Number.isFinite(segStartS) &&
        Number.isFinite(segEndS) &&
        segEndS > segStartS,
    `① 检查器写着的起止可解析(${picked.startTxt} → ${picked.endTxt})`,
);

// ---- ② 把这一段变成「未锁定的手动段」——「恢复自动」出得来的唯一形状 -------
// 手动段(split / move_boundary / set_values 的后置,§5.4)默认 locked=true,
// 而 clearManual 对 locked 免疫(§1.6),所以入口在锁定档上只说不做。
// 全程走检查器自己的控件:先改一次 pan(→ user_edited + locked),再点锁定开关解锁。
const PAN_COMMIT = IN(`
    const inp = gb("inspector-pan-input");
    if (!inp) return false;
    const cur = parseFloat(inp.value);
    inp.value = String(Number.isFinite(cur) ? Math.round(cur) + 7 : 7);
    inp.dispatchEvent(new w.KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
    return true;
`);
check(
    await evaluate(PAN_COMMIT),
    "② 检查器 PAN 框提交一次(§5.4 后置:user_edited + locked)",
);
await sleep(400);
let st = await evaluate(INSPECT);
check(st.origin === "E", `② 段变成 user_edited(角标 E,实得「${st.origin}」)`);
check(st.locked === true, "② 段被自动上锁(§5.4 后置)");
check(
    st.restoreShown && !st.restoreBtnShown,
    "② 锁定档:入口只说不做(不给点了没反应的钮)",
);

check(
    await evaluate(
        IN(
            `const t = gb("inspector-locked-toggle"); if (!t) return false; t.click(); return true;`,
        ),
    ),
    "② 点检查器的锁定开关解锁",
);
await sleep(400);
st = await evaluate(INSPECT);
check(st.locked === false, "② 段已解锁");
check(st.restoreBtnShown, "②「恢复自动」按钮出现");
// 段的起止不该被这两步动过(set_locked 不改边界,set_values 也不改)
near(parseTimeMs(st.startTxt), segStartS, 1e-9, "② 起点没被这两步动过");
near(parseTimeMs(st.endTxt), segEndS, 1e-9, "② 终点没被这两步动过");

// ---- ⑦ 前置:在**另一条轨**上按一个引擎绝不会算出来的手动值 ----------------
// 光靠「重算前后比段值」测不出写回集变宽:别的轨此刻的段本来就是同一套 mock 生成器
// 按同一段素材算出来的 —— 写回集就算变宽、把它们原样重算一遍,结果**逐字节相同**,
// 断言照样绿(第一版实测:放宽成两轨,⑦ 不红)。与 host 侧那格同一个手法:
// `setTrackManual` 写的是覆盖全时间线的 `origin=user_edited` 常值段,而 `clearManual`
// 恰好会放开这种段重算 —— 于是「有没有多写一条轨」才有可分辨的痕迹。
const otherCh = pickedCh === 1 ? 2 : 1;
check(
    await evaluate(
        IN(`
        const r = w.__SCVB_MOCK__.setTrackManual(${otherCh}, "pan", 77);
        return !!(r && r.ok !== false);
    `),
    ),
    `⑦ 前置:在第 ${otherCh} 轨按下手动 pan=77`,
);
await sleep(300);

// ---- ③ 点「恢复自动」→「继续」,拦下页面真发出去的 analyze ----------------
await evaluate(
    IN(`const b = gb("inspector-restore-btn"); if (b) b.click(); return true;`),
);
await sleep(250);
st = await evaluate(INSPECT);
check(st.restoreOkShown, "③ 两态就地切换成「取消 / 继续」");
check(
    st.restoreText.length > 0,
    `③ 确认句非空(实得「${st.restoreText.slice(0, 24)}…」)`,
);

await evaluate(
    IN(`const b = gb("inspector-restore-ok"); if (b) b.click(); return true;`),
);
await sleep(600);

const spy = await evaluate(IN(`return w.__SCVB_ANALYZE_SPY__ || [];`));
check(
    Array.isArray(spy) && spy.length === 1,
    `③ 恰好发了一次 analyze(实得 ${spy?.length})`,
);
const call = (spy || [])[0] || {};
const scope = call.scope || {};

// ---- ④ 断言:scope = 屏幕上那一段 ----------------------------------------
eq(
    call.opts,
    { clearManual: true },
    "④ opts = {clearManual:true}(§1.6 的重新识别分支)",
);
eq(
    scope.tracksMask,
    1 << (pickedCh - 1),
    "④ tracksMask = 选中段所在的那一条轨",
);
check(
    Number.isFinite(scope.startS) && Number.isFinite(scope.endS),
    "④ scope 带范围 —— **不是**轨级 `{tracksMask}`(← SL-242 的原样)",
);
// 逐毫秒对上:检查器上写的就是发出去的。差一个段、差一次重绑,这两个数就分家。
near(scope.startS, segStartS, 0.0006, "④ startS = 检查器写着的「起」");
near(scope.endS, segEndS, 0.0006, "④ endS = 检查器写着的「止」");
check(scope.endS - scope.startS < 3600, "④ 范围是一个段的量级,不是整条时间线");

// ---- ⑦ [SL-393] 写回集不许变宽:推回来的段只点名选中的那一条轨 --------------
// 确认文案许诺「只重算选中的这一段」。④ 钉的是**发出去的请求**长什么样,这一格钉的是
// **回来的那一帧**作用到了谁 —— 两者不是同一件事:请求的 `tracksMask` 是窄的,后端
// 仍可能把同范围内别的轨一并重写(native 侧 SL-393 的修法正是在 applyAnalysisSegments
// 上补了这道写回掩码)。
//
// ⚠ 轴选的是「**段值有没有变**」,不是「这一帧点名了哪几条轨」。后者是 **mock 独有**的
// 语义:真桥的分析帧恒以 `kAllTracksMask` 全量推(`OutputEditor.cpp:266`),按「只点名
// 选中轨」去断,真桥这一侧永远为假 —— 那样的判据只证明 mock 长什么样,证明不了产品。
// 「除选中轨外其余轨的段值与重算前逐字段相同」在两侧是同一个形状。
//
// 为什么值得单钉:SL-393 的 native 修法把**计算集**放宽成了「范围内所有有覆盖的轨」
// (只喂一条轨会被引擎判成「独唱」而按到正中,那正是本卡的病根)。计算集一宽,
// 「写回集跟着宽了」就成了一个真实的、静默的失效方向 —— 屏幕上别的轨悄悄换了值,
// 而确认文案说的是只动这一段。
// mock 的重算排在 `later(800, ...)` 上,③ 之后只睡了 600ms —— 等它到,别按固定睡眠赌。
const pushed = await waitFor(
    IN(
        `return (w.__SCVB_SEG_PUSH__ || []).some((p) => p.reason === "analyze");`,
    ),
    6000,
);
const pushes = await evaluate(IN(`return w.__SCVB_SEG_PUSH__ || [];`));
const analyzePush = (pushes || []).filter((p) => p.reason === "analyze");
check(
    pushed && analyzePush.length >= 1,
    `⑦ 重算之后确实推回了 scvb.segments(reason=analyze,实得 ${analyzePush.length} 帧)`,
);
if (analyzePush.length >= 1) {
    // 基线 = 重算**之前**每条轨最后一次被推上来的段值(首帧全量 dump 起就有)。
    const base = {};
    for (const p of pushes) {
        if (p.reason === "analyze") break; // 只取重算之前的
        for (const [ch, segs] of Object.entries(p.byCh || {})) base[ch] = segs;
    }
    const after = analyzePush[analyzePush.length - 1].byCh || {};

    // ⚠ 先断**取到的字段真有值**,再谈「逐字段相同」。
    // 键名写错时(本轮就写错过一个:桥面是 `volDb`,`vol_db` 只是 CSV 导出的列名)
    // 两侧都会取到 `undefined` → 序列化成 `null` → 这一列在比对里恒等,
    // 于是这一格**静默变空**:轴还叫「逐字段」,实际少比了一列,而且少的正好是
    // 用户症状的另一半(音量变成 0.0)。这条守卫让「键名写错」当场红,而不是变绿。
    {
        const sample = (after[String(pickedCh)] || [])[0];
        check(
            Array.isArray(sample) &&
                sample.length === 6 &&
                sample.every((v) => v !== undefined && v !== null),
            `⑦ 前提:段快照六列都取到了值(键名写错会让某列恒为 null 而比对变空;实得 ${JSON.stringify(sample)})`,
        );
    }
    const drifted = [];
    for (const [ch, segs] of Object.entries(after)) {
        if (Number(ch) === pickedCh) continue;
        const b = base[ch];
        if (!b) continue; // 这一轨重算前没被推过 ⇒ 无从比对,不算证据
        if (JSON.stringify(b) !== JSON.stringify(segs))
            drifted.push(Number(ch));
    }
    eq(
        drifted,
        [],
        `⑦ 除选中轨外,其余轨的段值与重算前逐字段相同(漂了的:[${drifted.join(",")}])`,
    );
    check(
        Object.prototype.hasOwnProperty.call(after, String(pickedCh)),
        `⑦ 正对照:选中的那一轨确实在这一帧里(否则「都没推」也能让上一条绿)`,
    );
}

// ---- ⑤ 反向:工具条那条(选区/全量)不受本卡影响 --------------------------
// 不这么钉的话,「把每个 analyze 都改成段级」也能让 ④ 全绿 —— 那会把 Tab3
// 「重新识别(含手动段)」的作用面一起缩掉,是另一个方向的同款缺陷。
await evaluate(IN(`return (w.__SCVB_ANALYZE_SPY__ || []).length = 0;`));
const TOOLBAR = IN(`
    const b = gb("wave-btn-reanalyze");
    if (!b || b.disabled) return "disabled";
    b.click();
    return "clicked";
`);
const toolbarState = await evaluate(TOOLBAR);
if (toolbarState === "clicked") {
    await sleep(250);
    // 二次确认框的「继续」
    await evaluate(
        IN(`
        const box = gb("wave-confirm-reidentify");
        if (!box) return false;
        const btns = box.querySelectorAll("button");
        const ok = btns[btns.length - 1];
        if (ok) ok.click();
        return true;
    `),
    );
    await sleep(500);
    const spy2 = await evaluate(IN(`return w.__SCVB_ANALYZE_SPY__ || [];`));
    const s2 = (spy2 || [])[0];
    if (check(!!s2, "⑤ 工具条「重新识别(含手动段)」也真发了 analyze")) {
        check(
            s2.scope === "all" ||
                !Number.isFinite(s2.scope?.startS) ||
                s2.scope.endS - s2.scope.startS > segEndS - segStartS,
            "⑤ 工具条那条走的是选区/全量,作用面没被本卡缩成一个段",
        );
    }
} else {
    log(`  (工具条「重新识别」当前 ${toolbarState})`);
}
// [#161 复审【建议】⑧] ⑤ 是「④ 的绿不是把每个 analyze 都改成段级蒙的」这条**反向
// 护栏**,而上面那个 else 分支只 log 一行 —— fixture 一变(钮变 disabled)它就永久
// 失效,而且不判红:一条护栏悄悄退化成恒跳过,比没有这条护栏更糟,因为它还在报绿。
// 本 fixture(默认 output.html)下工具条钮**已知可用**,所以「⑤ 真的跑到了」本身
// 就是一条断言。将来 fixture 真要改成钮不可用,这里会红,逼人显式重新裁定。
check(
    toolbarState === "clicked",
    `⑤ 反向护栏真的跑到了(工具条钮可点;实得 ${toolbarState})`,
);

// ---- ⑧ [SL-396] analyze **拒回执**必须有提示(页面级)------------------------
// 断什么:§1.6 的两条拒绝回执 —— 范围 ∩ 覆盖 = ∅ 的 `{ok:false, affected:{…0}}`
// (**不带 reason**)与「已有分析在跑」的 `{ok:false, reason:"busy"}` —— 在工具条上都必须变成
// 一条**看得见**的行内提示(`wave-arm-note`);而受理成功(`ok:true`)那一次不许出现这两句。
// 修前形态:这五处调用点把回执整个丢掉,两种拒绝在屏上都与「受理了」一模一样 —— 用户报的
// 就是「点了没反应」。
// 为什么必须页面级:「回执有没有被读、提示有没有真的上屏」node 侧断不到(那边只能断
// 「mock 会回拒绝」,见 smoke-tab2)。本节全程走**真 DOM 事件**:点「重新识别(含手动段)」
// → 确认框主钮 → `doReidentify` → `analyze`。
// ⚠ 三格都用**合成的回执**驱动(见 FORCE_ANALYZE),不依赖本 fixture 的覆盖情况 ——
// 判据是「提示由回执决定」,不是「这份素材恰好会被拒」。
// 删除式:把 tab-wave.js 里那句 `setToolbarNote(analyzeRefusalNote(res))` 注掉(或让
// `analyzeRefusalNote` 恒返回 null)⇒ (a)(b) 两格必红、(c) 仍绿。
const FORCE_ANALYZE = (mode) =>
    IN(`
    if (!w.__SCVB_ANALYZE_FORCE_WRAPPED__) {
        w.__SCVB_ANALYZE_FORCE_WRAPPED__ = true;
        const prev = w.__SCVB_MOCK__.analyze.bind(w.__SCVB_MOCK__);
        w.__SCVB_MOCK__.analyze = function (scope, opts) {
            // [R3] 计数:合成回执**不会**走到真正的 mock(所以 spy 记不到),本套要一个
            // 「这一次 analyze 真被调过」的可观测点,用它等,不写 sleep 常数。
            w.__SCVB_ANALYZE_FORCED_N__ = (w.__SCVB_ANALYZE_FORCED_N__ || 0) + 1;
            const mode = w.__SCVB_ANALYZE_FORCE__;
            if (mode === "refused") {
                return Promise.resolve({
                    ok: false,
                    affected: { tracks: 0, intervals: 0, manualKept: 0 },
                });
            }
            if (mode === "busy") return Promise.resolve({ ok: false, reason: "busy" });
            if (mode === "ok") return Promise.resolve({ ok: true });
            return prev(scope, opts);
        };
    }
    w.__SCVB_ANALYZE_FORCE__ = ${JSON.stringify(mode)};
    return true;
`);
const ARM_NOTE = IN(`
    const n = gb("wave-arm-note");
    return n ? { hidden: !!n.hidden, text: (n.textContent || "").trim() } : null;
`);
const REIDENTIFY_FLOW = IN(`
    const b = gb("wave-btn-reidentify");
    if (!b || b.disabled) return "disabled";
    b.click();
    const box = gb("wave-confirm-reidentify");
    if (!box || box.hidden) return "no-modal";
    const ok = gb("wave-confirm-reidentify-ok");
    if (!ok) return "no-ok";
    ok.click();
    return "clicked";
`);
const pageLang = await evaluate(
    IN(`return document.documentElement.lang || "";`),
);
check(
    pageLang.startsWith("zh"),
    `⑧ 前置:页面语言是 zh(实得 "${pageLang}")—— 下面两句按 zh 词条逐字对`,
);
const ZH = (
    await import(
        `file:///${join(ROOT, "web/shared/i18n.js").replace(/\\/g, "/")}`
    )
).dict("zh");

async function reidentifyWithReceipt(mode, label) {
    await evaluate(FORCE_ANALYZE(mode));
    // [#256 R3(复审 2-3)] **点之前**先等上一格的提示真灭(`n.hidden || t === ""`)。
    // 不这么做的话:上一格的 busy 提示还挂在屏上,而本格受理成功时 `setToolbarNote` 只
    // 「有 note 才写」⇒ 什么都不写 ⇒ (c) 会靠「5s 自撤还没到、恰好读到的不是那两句」蒙过去。
    // [#256 R8(复审 3-4)] **这一句的返回值必须接住**:`waitFor` 只回 true/false,不接的话
    // 超时是静悄悄地不成立,红会掉在**下一格**上(用下一格的措辞),读日志的人会去查一个
    // 不存在的原因。下面三处 `waitFor` 一律这样接住(等上一格提示灭 / 等计数 +1 / 等期望文本)。
    check(
        await waitFor(
            IN(`
        const n = gb("wave-arm-note");
        return !n || n.hidden || (n.textContent || "").trim() === "";
    `),
            8000,
        ),
        `⑧ 前置:${label} —— 上一格的提示位在 8s 内真灭(等超时)`,
    );
    const before = await evaluate(
        IN(`return w.__SCVB_ANALYZE_FORCED_N__ || 0;`),
    );
    const st = await evaluate(REIDENTIFY_FLOW);
    check(
        st === "clicked",
        `⑧ 前置:${label} —— 「重新识别(含手动段)」这条链真走到了主钮(实得 ${st})`,
    );
    // 等到**这一次 analyze 真被调过**:合成回执不走进真 mock,所以用上面那个计数当可观测点
    // (而不是 sleep 常数,也不是「等提示自己消失」)。
    check(
        await waitFor(
            IN(`return (w.__SCVB_ANALYZE_FORCED_N__ || 0) > ${before};`),
            8000,
        ),
        `⑧ 前置:${label} —— 这一次 analyze 在 8s 内真被调过(等超时)`,
    );
    if (mode === "ok") {
        // [#256 R9(复审 3-1 前半)] **受理成功那一档改成反向等待**,不再「计数 +1 之后立刻读一次」。
        // 那样读在 rAF 渲染**之前**是竞态:计数 +1 只证明 `analyze` 被调到,提示位的写入与重绘
        // 都还在后面 —— D-d 注入那次撞巧红了,不等于这一格有牙(判据没接住 = 换个时序就假绿)。
        // 现在盯住提示位 1.5s:只要它**出现**这两句就判负;测的是「这 1.5s 里始终没出现」,
        // 而不是「某一瞬间恰好不是」。
        const appeared = await waitFor(
            IN(`
        const n = gb("wave-arm-note");
        if (!n || n.hidden) return false;
        const t = (n.textContent || "").trim();
        return t === ${JSON.stringify(ZH["analyze.refused"])}
            || t === ${JSON.stringify(ZH["analyze.busy"])};
    `),
            1500,
        );
        check(
            !appeared,
            "⑧ (c) 受理成功 ⇒ 1.5s 内提示位始终没出现那两句" +
                `(实得 appeared=${appeared})`,
        );
        // 下面那条 (c) 断言(在调用点)**保留**:反向等待管「中途有没有闪过」,
        // 它管「这一格结束时屏上留的是什么」,两件事。
        return evaluate(ARM_NOTE);
    }
    const wantText = JSON.stringify(
        mode === "busy" ? ZH["analyze.busy"] : ZH["analyze.refused"],
    );
    check(
        await waitFor(
            IN(`
        const n = gb("wave-arm-note");
        if (!n) return false;
        return !n.hidden && (n.textContent || "").trim() === ${wantText};
    `),
            8000,
        ),
        `⑧ 前置:${label} —— 期望的提示在 8s 内上屏(等超时)`,
    );
    return evaluate(ARM_NOTE);
}

const noteRefused = await reidentifyWithReceipt("refused", "拒回执");
check(
    !!noteRefused &&
        !noteRefused.hidden &&
        noteRefused.text === ZH["analyze.refused"],
    `⑧ (a) 范围 ∩ 覆盖 = ∅(不带 reason 的 {ok:false})⇒ 行内提示逐字出词条 analyze.refused` +
        `(实得 ${JSON.stringify(noteRefused)};期望文本 ${JSON.stringify(ZH["analyze.refused"])})`,
);

const noteBusy = await reidentifyWithReceipt("busy", "busy 回执");
check(
    !!noteBusy && !noteBusy.hidden && noteBusy.text === ZH["analyze.busy"],
    `⑧ (b) reason:"busy" ⇒ 行内提示逐字出词条 analyze.busy` +
        `(实得 ${JSON.stringify(noteBusy)};期望文本 ${JSON.stringify(ZH["analyze.busy"])})`,
);

const noteOk = await reidentifyWithReceipt("ok", "受理回执");
check(
    !!noteOk &&
        (noteOk.hidden ||
            (noteOk.text !== ZH["analyze.refused"] &&
                noteOk.text !== ZH["analyze.busy"])),
    `⑧ (c) 受理成功 ⇒ **不出**这两句(实得 ${JSON.stringify(noteOk)})` +
        ` —— 少了这一格,(a)(b) 的绿可以由「无脑弹提示」蒙出来`,
);

await evaluate(FORCE_ANALYZE(null)); // 复原:后面还有别的断言在跑

// ---- ⑨ 零异常 -------------------------------------------------------------
check(
    exceptions.length === 0,
    `⑨ 零未捕获异常(实得 ${exceptions.length}:${exceptions[0] || ""})`,
);
check(
    errors.length === 0,
    `⑨ 零 console.error(实得 ${errors.length}:${errors[0] || ""})`,
);

await finish();

async function finish() {
    try {
        cdp.close();
    } catch {}
    try {
        chrome.kill();
    } catch {}
    try {
        server.close();
    } catch {}
    try {
        rmSync(userDataDir, { recursive: true, force: true });
    } catch {}
    if (fail) {
        console.error(`\n${fail} 处断言失败`);
        process.exit(1);
    }
    log("\n全部通过");
    process.exit(0);
}
