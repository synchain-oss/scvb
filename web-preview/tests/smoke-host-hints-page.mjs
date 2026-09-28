// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 宿主专属提示的**页面级**冒烟([J150] ①;03 §4.2 REAPER / §4.4 Live)
// =============================================================================
// 为什么要页面级:三条横幅(⑫⑬⑭)的显隐由 app.js 的 renderBanners 按「§1.1 快照 `host`
// × 打印相位 × 本会话闩锁」翻,而打印相位的边沿由 scvb.state 与 scvb.playhead **两路**
// 事件带来(host-hints.js 头注)。node 侧 `smoke-host-hints.mjs` 只证明「判据会算」,
// 证明不了「两路事件都真的在记账、算出来的东西真的进了 DOM、✕ 真的关得掉」。
//
// 跑什么(全部 `?scenario=connected`:输出开关初始 OFF、走带在播、播放头落在范围内(该世界的 range 是全曲跟随),
// 所以点一下输出开关就进 PRINT —— 与 smoke-group-lock-page ④ 同一个世界):
//   ① host=reaper:输出 ON ⇒ ⑫「保持窗口打开」+ ⑬「写完没录到怎么办」同时出、⑭ 不出;
//      ✕ ⑬ ⇒ 停走再播(再进 PRINT)也不再出(一次性);⑫ 在 ARMED(停走)时仍在(挂的是输出 ON);
//      ✕ ⑫ ⇒ 输出关掉再打开 ⇒ ⑫ 重新出现(showDismissible「条件为假就删记录」);
//   ② host=live:打印中 ⑭ 不出;停走(PRINT → ARMED)⇒ ⑭ 出、⑫⑬ 不出;
//      ✕ ⑭ ⇒ 再播(进 PRINT)不出 ⇒ 再停(又一次打印结束)重新出现;
//   ③ host=cubase:进出 PRINT 各一次,三条自始至终不出(判据看的是打印结束那一帧的 footer
//      `data-mode="printDone"` —— 同一次 render() 里 renderBanners 先于 renderFooter 跑过);
//   ④ 不给 host(= 快照默认 other):同 ③;
//   ⑤ host=live + **走带位置冻住**(`ctl.setHostTimeAvailable(false)`,§2.6 帧逐字不变 ⇒
//      页面对这种帧不记账,与真桥 diff-then-emit 不发它同形):此时打印边沿**只**从
//      scvb.state(输出开关)来 —— 打开再关掉输出 ⇒ ⑭ 必须出。这是专为 scvb.state 那一处
//      记账造的尺子:它**只**靠那一处。(实测把那一处删掉,② 的第一次打印结束或 ⑥ 的停走
//      **有时**也会红 —— 那两格进 PRINT 的那一拍来自输出开关,本套一看到 footer 进打印行
//      就接着动走带,赶不赶得上下一帧 §2.6 看时机;两次实测红的格不一样。那是顺带的,
//      别拿 ② / ⑥ 当这一处的判据。)
//   ⑥ host=live + **循环跨出写入范围**(播着、输出 ON,把范围改到播放头之前 ⇒ PRINT → ARMED
//      但走带还在走):⑭ **不出**(下一圈回到范围里还会接着写,「写入已结束」是反话;
//      #324 复审采纳)⇒ 停走之后才出;
//   ⑦ host=live:⑭ 出现后 ✕ 掉 ⇒ 在写入范围**外**按播放试听(输出 ON ∧ 在播,但不进 PRINT)
//      再停走 ⇒ ⑭ **仍关着**(没有新的一次写入结束,就不该冒回来)。#324 复审第 2 轮揪出的回归:
//      ⑥ 那道「输出 ON ∧ 在播先压着」一度被并进 showDismissible 的 `on`,试听一下就把「关过」
//      的记录删了。判据取停走**之后**再切一次 tab(强制整页重渲染一次)的那一帧 —— 停走那一拍
//      之后没有别的事件会来 render,不切的话「还没渲染」与「渲染了但仍关着」分不开;
//   各场景都要零未捕获异常、零 console.error。
//
// 用法:node web-preview/tests/smoke-host-hints-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;**2 = 环境里没有 Chrome/Edge**(口径同
//   smoke-output-stale-page.mjs 与 CLAUDE.md §6:可选依赖缺席不判红,但也绝不算通过);
//   **3 = 浏览器在,但这一次没起来 / 没连上**([SL-297],见 `browserFailed()`)。
//
// CDP 连接、静态服务、收尾那几段与 smoke-output-stale-page.mjs 同源(零依赖,仓库红线是
// 不引 puppeteer);同样没有抽公共模块 —— 理由见那份文件头注。
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
import { dirname, extname, join, resolve } from "node:path";
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
    if (!abs.startsWith(resolve(ROOT))) {
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
    argv.get("cdp") || 9400 + Math.floor(Math.random() * 400),
);
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-host-hints-"));
const chrome = spawn(
    exe,
    [
        "--headless=new",
        `--remote-debugging-port=${CDP_PORT}`,
        `--user-data-dir=${userDataDir}`,
        "--window-size=1400,1000",
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
// 本文件原有的 try/finally 只包住**主断言体**,而 Chrome 是在进入那个 try **之前**就 spawn 的 ——
// 那段窗口里抛错(CDP 连接、Page.enable、首次导航,恰好是上面新加的超时最可能开火的地方)
// 就会把 headless Chrome 留在机器上。
// 而且信号完全没人接:SL-287 同时给 gates 3e 加了整套超时,超时会向本进程发信号。
//
// ⚠ **本机(Windows)实测的边界,别把这段的作用说大**:
//   · 浏览器进程:node 一死,Windows 会把 spawn 出来的 Chrome 一起收掉 —— 实测原版在
//     SIGTERM 下也能从 10 个进程回到 0。所以在 Windows 上这段对**进程**是双保险,不是唯一解。
//     它真正吃劲的地方是 **Linux**:`web-smoke` 跑在 ubuntu-latest,而 POSIX 下父进程退出
//     **不会**自动收掉 spawn 的子进程 —— 而本文件的 try/finally 只包住主断言体,
//     spawn 到进 try 之间那段窗口(CDP 连接、Page.enable、首次导航)在 Linux 上没人收。
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
let bucket = { label: "启动", errors: [], exceptions: [] };
const newBucket = (label) => {
    bucket = { label, errors: [], exceptions: [] };
};

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

async function waitFor(expr, ms = 15000) {
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
    ${js}
})()`;

// 词条真源(断言文案逐字等于 zh 词条:判的是「真的走了词条」,不是「模板里那句兜底字面量」)。
const { T } = await import(
    pathToFileURL(join(ROOT, "web/shared/i18n.js")).href
);

// 页面已经吃到首帧 §2.8(段表落地)的判据:泳道轨名被填过(与 smoke-output-stale-page 同一条)。
const READY = IN(`
    const lane = gb("wave-lane-2-label");
    return !!(lane && lane.textContent && lane.textContent.length > 0);
`);

// 一次读出本套要看的全部投影。可见 = 不带 hidden 且 computed display 不是 none
// (本页那条全局 `[hidden]{display:none !important}` 兜的就是后者)。
const PROBE = IN(`
    const vis = (el) => !!el && !el.hidden && w.getComputedStyle(el).display !== "none";
    const txt = (n) => { const e = gb(n); return e ? e.textContent.trim() : null; };
    const sw = gb("master-output-toggle-switch");
    const ft = gb("footer");
    return {
        present: !!gb("banner-reaperKeepOpen") && !!gb("banner-reaperPrintNote") && !!gb("banner-liveReEnable"),
        keepOpen: vis(gb("banner-reaperKeepOpen")),
        printNote: vis(gb("banner-reaperPrintNote")),
        live: vis(gb("banner-liveReEnable")),
        keepOpenText: txt("banner-reaperKeepOpen-text"),
        printNoteText: txt("banner-reaperPrintNote-text"),
        liveText: txt("banner-liveReEnable-text"),
        outputOn: !!sw && sw.getAttribute("aria-checked") === "true",
        footerMode: ft ? ft.getAttribute("data-mode") : null,
    };
`);

// 轮询 PROBE 直到谓词成立;返回最后一次读到的快照(超时也返回,调用方据此报「实得」)。
// 与 waitFor 同一条截止口径:每次 evaluate 的上界 = 本次还剩多少预算([SL-287])。
async function until(pred, ms = 8000) {
    const t0 = Date.now();
    let p = null;
    while (Date.now() - t0 < ms) {
        try {
            p = await evaluate(
                PROBE,
                Math.max(1000, ms - (Date.now() - t0) - 250),
            );
        } catch {
            p = null;
        }
        if (p && pred(p)) return p;
        await sleep(120);
    }
    return p;
}

const shown = (p) =>
    p
        ? `⑫=${p.keepOpen ? "显" : "隐"} ⑬=${p.printNote ? "显" : "隐"} ⑭=${p.live ? "显" : "隐"}` +
          ` 输出=${p.outputOn ? "ON" : "OFF"} footer=${p.footerMode}`
        : "(没读到页面)";

// footer 的打印行:`renderFooter` 在 PRINT 时写 `data-mode` = "print"(手动 / 循环范围)
// 或 "follow"(全曲跟随范围);打印结束后 8 秒是 "printDone",其余 "default"。
// 这是页面自己的 outputPhase(...) === "print" 投影出来的**可观测**信号。
const printing = (p) =>
    !!p && (p.footerMode === "print" || p.footerMode === "follow");

// 走带与预览会话挂在**壳页**上(shell.js:session),不走 IN()。
const SHELL = (js) => `(() => {
    const s = window.__SCVB_PREVIEW__;
    if (!s || !s.ctl) return null;
    ${js}
})()`;
const setPlaying = (on) =>
    evaluate(
        SHELL(
            `s.ctl.setTransport({ isPlaying: ${on ? "true" : "false"} }); return true;`,
        ),
    );
const clickGb = (name) =>
    evaluate(
        IN(
            `const b = gb("${name}"); if (!b) return false; b.click(); return true;`,
        ),
    );

async function open(host) {
    const label = host ? `host=${host}` : "host 缺省";
    newBucket(label);
    await cdp.send("Page.navigate", {
        url:
            `${base}/web-preview/output.html?scenario=connected` +
            (host ? `&host=${host}` : ""),
    });
    check(await waitFor(READY), `${label}:页面装载并吃到首帧`);
    const p = await evaluate(PROBE);
    check(p !== null && p.present, `${label}:三条横幅节点都在模板里`);
    check(
        p && !p.keepOpen && !p.printNote && !p.live,
        `${label}:输出 OFF 时三条都不出(实得 ${shown(p)})`,
    );
    check(p && !p.outputOn, `${label}:前提 —— 输出开关初始 OFF`);
    return p;
}

// 点输出开关并等到 PRINT(输出 ON ∧ footer 进入打印行)。
async function outputOnIntoPrint(label) {
    check(
        await clickGb("master-output-toggle-switch"),
        `${label}:点到了输出开关`,
    );
    const p = await until((x) => x.outputOn && printing(x));
    check(
        p && p.outputOn && printing(p),
        `${label}:输出 ON 且进入 PRINT(实得 ${shown(p)})`,
    );
    return p;
}

// 停走并等到「打印结束」那一次 render(footer 切到 printDone)。
async function stopAfterPrint(label) {
    check(await setPlaying(false), `${label}:停走`);
    const p = await until((x) => x.footerMode === "printDone");
    check(
        p && p.footerMode === "printDone",
        `${label}:PRINT 结束那一帧渲染过了(footer=printDone;实得 ${shown(p)})`,
    );
    return p;
}

function assertClean(label) {
    check(
        bucket.exceptions.length === 0,
        `${label}:零未捕获异常(实得 ${bucket.exceptions.length} 条:${bucket.exceptions.join(" | ").slice(0, 400)})`,
    );
    check(
        bucket.errors.length === 0,
        `${label}:零 console.error(实得 ${bucket.errors.length} 条:${bucket.errors.join(" | ").slice(0, 400)})`,
    );
}

const CDP_WAIT_TRIES = 300;
const CDP_WAIT_STEP_MS = 200;

try {
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
    cdp = cdpConnect(
        targets.find((t) => t.type === "page").webSocketDebuggerUrl,
    );
    await cdp.ready;
    await cdp.send("Page.enable");
    await cdp.send("Runtime.enable");
    cdp.on((m) => {
        if (m.method === "Runtime.exceptionThrown") {
            const d = m.params.exceptionDetails;
            bucket.exceptions.push(
                (d.exception?.description || d.text || "").split("\n")[0],
            );
        } else if (
            m.method === "Runtime.consoleAPICalled" &&
            m.params.type === "error"
        ) {
            bucket.errors.push(
                (m.params.args || [])
                    .map((a) => a.value ?? a.description ?? "")
                    .join(" "),
            );
        }
    });
    log(`(站点根 ${ROOT} → ${base};CDP ${CDP_PORT})`);

    // =========================================================================
    log("=== ① host=reaper:⑫⑬ 进 PRINT 即出;⑬ 一次性;⑫ 关掉后输出重开再出 ===");
    {
        await open("reaper");
        const p1 = await outputOnIntoPrint("①");
        check(p1 && p1.keepOpen, `① ⑫「保持窗口打开」出现(实得 ${shown(p1)})`);
        check(
            p1 && p1.printNote,
            `① ⑬「写完没录到怎么办」出现(实得 ${shown(p1)})`,
        );
        check(p1 && !p1.live, "① ⑭(Live 专属)不出");
        eq(
            p1 && p1.keepOpenText,
            T.zh["banner.reaperKeepOpen"],
            "① ⑫ 文案逐字 = 词条 zh",
        );
        eq(
            p1 && p1.printNoteText,
            T.zh["banner.reaperPrintNote"],
            "① ⑬ 文案逐字 = 词条 zh",
        );

        // ✕ ⑬ ⇒ 收起
        check(await clickGb("banner-reaperPrintNote-dismiss"), "① 点了 ⑬ 的 ✕");
        const p2 = await until((x) => !x.printNote);
        check(p2 && !p2.printNote, `① ✕ 之后 ⑬ 收起(实得 ${shown(p2)})`);

        // 停走(PRINT → ARMED):⑫ 挂的是「输出 ON」,不随打印结束收起;⑭ 仍不出
        const p3 = await stopAfterPrint("①");
        check(
            p3 && p3.keepOpen,
            `① ARMED(停走、输出仍 ON)⑫ 仍在(实得 ${shown(p3)})`,
        );
        check(p3 && !p3.printNote, "① ARMED:⑬ 仍是关着的");
        check(p3 && !p3.live, "① ARMED:⑭ 不出(宿主不是 Live)");

        // 再播(再进 PRINT):⑬ 是一次性的,不再出
        check(await setPlaying(true), "① 再播");
        const p4 = await until((x) => printing(x));
        check(p4 && printing(p4), `① 再次进入 PRINT(实得 ${shown(p4)})`);
        check(
            p4 && !p4.printNote,
            `① ⑬ 一次性:再进 PRINT 也不再出(实得 ${shown(p4)})`,
        );
        check(p4 && p4.keepOpen, "① ⑫ 仍在");

        // ✕ ⑫ ⇒ 收起;输出关掉(条件为假,删记录)再打开 ⇒ ⑫ 重新出现
        check(await clickGb("banner-reaperKeepOpen-dismiss"), "① 点了 ⑫ 的 ✕");
        const p5 = await until((x) => !x.keepOpen);
        check(p5 && !p5.keepOpen, `① ✕ 之后 ⑫ 收起(实得 ${shown(p5)})`);
        check(await clickGb("master-output-toggle-switch"), "① 关掉输出");
        const p6 = await until((x) => !x.outputOn);
        check(
            p6 && !p6.outputOn && !p6.keepOpen,
            `① 输出 OFF:⑫ 不出(实得 ${shown(p6)})`,
        );
        const p7 = await outputOnIntoPrint("① 重开");
        check(
            p7 && p7.keepOpen,
            `① 输出重新打开 ⇒ ⑫ 重新出现(实得 ${shown(p7)})`,
        );
        check(p7 && !p7.printNote, "① ⑬ 本会话仍不再出");
        assertClean("① host=reaper");
    }

    // =========================================================================
    log("=== ② host=live:打印结束才出 ⑭;关掉后下一次打印结束再出 ===");
    {
        await open("live");
        const p1 = await outputOnIntoPrint("②");
        check(
            p1 && !p1.live && !p1.keepOpen && !p1.printNote,
            `② 打印中三条都不出(⑭ 要等打印结束;实得 ${shown(p1)})`,
        );
        const p2 = await stopAfterPrint("②");
        check(
            p2 && p2.live,
            `② 打印结束 ⇒ ⑭「点 Re-Enable Automation」出现(实得 ${shown(p2)})`,
        );
        check(p2 && !p2.keepOpen && !p2.printNote, "② ⑫⑬(REAPER 专属)不出");
        eq(
            p2 && p2.liveText,
            T.zh["banner.liveReEnable"],
            "② ⑭ 文案逐字 = 词条 zh",
        );

        check(await clickGb("banner-liveReEnable-dismiss"), "② 点了 ⑭ 的 ✕");
        const p3 = await until((x) => !x.live);
        check(p3 && !p3.live, `② ✕ 之后 ⑭ 收起(实得 ${shown(p3)})`);

        check(await setPlaying(true), "② 再播");
        const p4 = await until((x) => printing(x));
        check(p4 && printing(p4), `② 再次进入 PRINT(实得 ${shown(p4)})`);
        check(p4 && !p4.live, "② 打印中 ⑭ 不出");
        const p5 = await stopAfterPrint("② 第二次");
        check(
            p5 && p5.live,
            `② 又一次打印结束 ⇒ ⑭ 重新出现(关掉是那一次,不是永久;实得 ${shown(p5)})`,
        );
        assertClean("② host=live");
    }

    // =========================================================================
    for (const [tag, host] of [
        ["③", "cubase"],
        ["④", null],
    ]) {
        log(
            `=== ${tag} ${host ? "host=" + host : "host 缺省(other)"}:进出 PRINT,三条自始至终不出 ===`,
        );
        await open(host);
        const p1 = await outputOnIntoPrint(tag);
        check(
            p1 && !p1.keepOpen && !p1.printNote && !p1.live,
            `${tag} 打印中三条都不出(实得 ${shown(p1)})`,
        );
        const p2 = await stopAfterPrint(tag);
        check(
            p2 && !p2.keepOpen && !p2.printNote && !p2.live,
            `${tag} 打印结束那一帧三条仍都不出(实得 ${shown(p2)})`,
        );
        assertClean(`${tag} ${host || "host 缺省"}`);
    }

    // =========================================================================
    log("=== ⑤ host=live + 走带位置冻住:打印边沿只从 scvb.state 来 ===");
    {
        await open("live");
        // 冻住 §2.6 帧:timeS 恒 0;范围改成手动 0–60s ⇒ inRange 在 t=0 为真 ⇒ 帧逐字不变
        // 却处在「可打印」的位置。页面对逐字不变的帧不记账(与真桥不发它同形)。
        check(
            await evaluate(
                SHELL(`s.ctl.setHostTimeAvailable(false);
                       const r = s.mock.setRange("manual", 0, 60);
                       return !!r && r.ok === true;`),
            ),
            "⑤ 冻住走带位置 + 范围改成手动 0–60s",
        );
        await sleep(600); // 让冻住之后的第一帧(与之前不同,会记一拍)先落地
        const p1 = await outputOnIntoPrint("⑤");
        check(p1 && !p1.live, "⑤ 打印中 ⑭ 不出");
        // 关掉输出 = PRINT → FOLLOW,这个边沿**只**由 scvb.state 带来(§2.6 帧逐字不变)
        check(await clickGb("master-output-toggle-switch"), "⑤ 关掉输出");
        const p2 = await until((x) => !x.outputOn && x.live);
        check(
            p2 && !p2.outputOn && p2.live,
            `⑤ 关掉输出(打印结束)⇒ ⑭ 出现 —— 边沿是 scvb.state 那一处记上的(实得 ${shown(p2)})`,
        );
        assertClean("⑤ host=live 冻住走带");
    }

    // =========================================================================
    log("=== ⑥ host=live + 播着出了写入范围:⑭ 等停走才出 ===");
    {
        await open("live");
        await outputOnIntoPrint("⑥");
        // 范围改到播放头之前(fixture 的播放头在 42s 之后)⇒ 页面相位 PRINT → ARMED,走带照走
        check(
            await evaluate(
                SHELL(`const r = s.mock.setRange("manual", 0, 30);
                       return !!r && r.ok === true;`),
            ),
            "⑥ 范围改成手动 0–30s(播放头已在范围之后)",
        );
        const p1 = await until((x) => x.footerMode === "printDone");
        check(
            p1 && p1.footerMode === "printDone" && p1.outputOn,
            `⑥ 播着离开范围:PRINT 结束那一帧渲染过了、输出仍 ON(实得 ${shown(p1)})`,
        );
        check(
            p1 && !p1.live,
            `⑥ 输出 ON 且还在播 ⇒ ⑭ 先不出(实得 ${shown(p1)})`,
        );
        check(await setPlaying(false), "⑥ 停走");
        const p2 = await until((x) => x.live);
        check(p2 && p2.live, `⑥ 停走之后 ⑭ 出现(实得 ${shown(p2)})`);
        assertClean("⑥ host=live 播着出范围");
    }

    // =========================================================================
    log("=== ⑦ host=live:✕ 掉 ⑭ 后在范围外试听再停走 ⇒ ⑭ 仍关着 ===");
    {
        await open("live");
        await outputOnIntoPrint("⑦");
        await stopAfterPrint("⑦");
        const p1 = await until((x) => x.live);
        check(p1 && p1.live, `⑦ 前提:打印结束后 ⑭ 出现(实得 ${shown(p1)})`);
        check(await clickGb("banner-liveReEnable-dismiss"), "⑦ 点了 ⑭ 的 ✕");
        const p2 = await until((x) => !x.live);
        check(p2 && !p2.live, `⑦ ✕ 之后 ⑭ 收起(实得 ${shown(p2)})`);
        // 范围改到播放头之前 ⇒ 再播也进不了 PRINT(ARMED ∧ 在播 = 试听)
        check(
            await evaluate(
                SHELL(`const r = s.mock.setRange("manual", 0, 30);
                       return !!r && r.ok === true;`),
            ),
            "⑦ 范围改成手动 0–30s(播放头在范围之后)",
        );
        const pageStopped = (want) =>
            IN(`const o = w.__SCVB_OUTPUT__;
                return !!o && o.hostEcho().stopped === ${want ? "true" : "false"};`);
        check(await setPlaying(true), "⑦ 在范围外按播放");
        check(
            await waitFor(pageStopped(false), 6000),
            "⑦ 页面已收到「在播」那一帧",
        );
        const p3 = await evaluate(PROBE);
        check(
            p3 && !printing(p3) && p3.outputOn,
            `⑦ 在播但没进 PRINT(试听;实得 ${shown(p3)})`,
        );
        check(p3 && !p3.live, "⑦ 试听中 ⑭ 不出");
        check(await setPlaying(false), "⑦ 停走");
        check(
            await waitFor(pageStopped(true), 6000),
            "⑦ 页面已收到「停走」那一帧",
        );
        // 强制整页重渲染一次(切走再切回),判据取那之后的一帧
        const tabIs = (name) =>
            IN(`const c = d.getElementById("content");
                return !!c && c.getAttribute("data-tab") === "${name}";`);
        await evaluate(
            IN(
                `const b = q('[data-tab-btn="tracks"]'); if (b) b.click(); return true;`,
            ),
        );
        check(await waitFor(tabIs("tracks"), 6000), "⑦ 切到「轨道」页");
        await evaluate(
            IN(
                `const b = q('[data-tab-btn="master"]'); if (b) b.click(); return true;`,
            ),
        );
        check(await waitFor(tabIs("master"), 6000), "⑦ 切回总览页");
        const p4 = await evaluate(PROBE);
        check(
            p4 && !p4.live,
            `⑦ 停走之后 ⑭ **仍关着** —— 试听不是新的一次写入结束,不该把 ✕ 冲掉(实得 ${shown(p4)})`,
        );
        assertClean("⑦ host=live ✕ 后试听");
    }
} catch (e) {
    fail++;
    console.log(`  [FAIL] 冒烟过程抛错:${e && e.message ? e.message : e}`);
} finally {
    try {
        if (cdp) cdp.close();
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
}

if (fail > 0) {
    console.log(`\n❌ ${fail} 条断言失败`);
    process.exit(1);
}
console.log("\n✅ 宿主专属提示页面级冒烟全绿");
process.exit(0);
