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
//   smoke-output-stale-page.mjs 与 CLAUDE.md §6:可选依赖缺席不判红,也绝不算通过)。
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
                // 拆锁后 gate 1–5 不持锁(gates.ps1 只在 gate 6/7/8 外套 `Local\SCVB-ipc-tests`),
                // 而 web smoke 是 gate 3e ⇒ 代价收窄成「**本轮** gates 停死」,不再连累别人。
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

chrome.on("error", (e) => noBrowser(`浏览器启动失败:${e.message}`));

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
log(`(站点根 ${ROOT} → ${base};CDP ${CDP_PORT})`);
log("=== SL-242 段级「恢复自动」作用域 —— 页面级 ===");

await cdp.send("Page.navigate", { url: `${base}/web-preview/output.html` });
check(await waitFor(READY), "页面装载并吃到首帧段表");
// 切到「波形与分段」页:检查器的渲染在 tab-wave 的 render 里。
await evaluate(
    IN(`const b = gb("tabnav-wave"); if (b) b.click(); return true;`),
);
await sleep(500);

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

// ---- ⑥ 零异常 -------------------------------------------------------------
check(
    exceptions.length === 0,
    `⑥ 零未捕获异常(实得 ${exceptions.length}:${exceptions[0] || ""})`,
);
check(
    errors.length === 0,
    `⑥ 零 console.error(实得 ${errors.length}:${errors[0] || ""})`,
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
