// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Output「采集数据已过期」提示的**页面级**冒烟(SL-177)
// =============================================================================
// 为什么要页面级:04 §4.5 的提示面是三处 DOM(横幅 ⑧ / tab 导航琥珀点 / 泳道 ⚠),
// 它们的显隐由 `renderBanners()` 与 tab-wave 的 `render()` 在真事件里翻 —— node 侧
// 的纯函数断言只能证明「reducer 会算」,证明不了「算出来的东西真的进了 DOM」。
// 本仓已经栽过三次「三层机检全绿、窗口是白的」,所以这条提示的启用分支必须真渲染一次。
//
// 跑什么:
//   ① `?scenario=stale`(fingerprint watchdog 摆开三条轨)⇒ 横幅 ⑧ 可见且写着 3 轨、
//      tab 导航琥珀点亮、泳道 2/5/11 的 ⚠ 可见且带整句 tooltip、其余 12 轨的 ⚠ 收起;
//   ② **反向** `?scenario=connected`(素材没变)⇒ 三处提示全部收起;
//   ②b [SL-239] 横幅 ⑨「采集开着 ⇒ 上游改动比对已暂停」的三档(判据四项,后两项是
//      「不说反话」的护栏):`connected`(采集 ON + 段表 + 无 stale + 未布防)⇒ 亮起且文案
//      带得动作;`stale`(采集**同样** ON,只是 ⚠ 已挂)⇒ 让位只留 ⑧;
//      `recapture-armed`(布防期,采集是被 §1.23 裁定① 替用户打开的)⇒ 收起 ——
//      此刻用户正应当保持采集开着播完那一段,⑨ 的「先关掉采集」是反向指令;
//   ②c [SL-247] `no-timeline`:⑨ 让位给 ⑥ —— noTimeline 下比对停摆的真因不是采集开关,
//      而采集开关此时还是 disabled,⑨ 的「先关掉采集」既做不到、也把因果说反了;
//   ③ 各场景都要零未捕获异常、零 console.error;
//   ④ 提示不阻断任何操作(04 §4.5「只提示,不自动失效」):stale 场景下采集/输出开关、
//      分析按钮一个都不许被 disable。
//
// 用法:node web-preview/tests/smoke-output-stale-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;**2 = 环境里没有 Chrome/Edge**(口径同
//   smoke-monitor-page.mjs 与 CLAUDE.md §6:可选依赖缺席不判红,但也绝不算通过);
//   **3 = 浏览器在,但这一次没起来 / 没连上**([SL-297],见 `browserFailed()`)——
//   同样不判红,但在 gates 汇总里打 `[FLAKY-SKIP]`,免得「没跑成」被读成「跑过了」。
//
// CDP 连接那 30 行与 smoke-monitor-page.mjs 同源(node 内置 fetch + WebSocket,
// 零依赖 —— 仓库红线是不引 puppeteer)。同样没有抽公共模块:那份断的是 Monitor 的
// 投影面,本份断的是 Output 的提示面,合并只会让两边被对方的参数面绑住。
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

const STALE_CHANNELS = [2, 5, 11];
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-output-stale-"));
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

// 页面已经吃到首帧 §2.8(段表落地)的判据:泳道轨名被填过。
const READY = IN(`
    const lane = gb("wave-lane-2-label");
    return !!(lane && lane.textContent && lane.textContent.length > 0);
`);

const PROBE = IN(`
    const vis = (el) => !!el && !el.hidden;
    const lanes = [];
    for (let ch = 1; ch <= 15; ch++) {
        const n = gb("wave-lane-" + ch + "-stale");
        lanes.push({ ch: ch, present: !!n, shown: vis(n), title: n ? (n.getAttribute("title") || "") : null });
    }
    const banner = gb("banner-staleCapture");
    const text = gb("banner-staleCapture-text");
    const paused = gb("banner-fpPausedByCapture");
    const pausedText = gb("banner-fpPausedByCapture-text");
    const disabled = (name) => { const el = gb(name); return !!el && !!el.disabled; };
    return {
        banner: vis(banner),
        bannerDisplay: banner ? w.getComputedStyle(banner).display : "(缺节点)",
        bannerText: text ? text.textContent.trim() : null,
        captureEnabled: (() => {
            const sw = gb("master-capture-toggle-switch");
            return !!sw && sw.getAttribute("aria-checked") === "true";
        })(),
        noTimelineBanner: vis(gb("banner-noTimeline")),
        voided: vis(gb("banner-recaptureVoided")),
        voidedPresent: !!gb("banner-recaptureVoided"),
        voidedText: (() => {
            const n = gb("banner-recaptureVoided-text");
            return n ? n.textContent.trim() : null;
        })(),
        // 采集开关的写闸在 DOM 上是 data-disabled(它是 div 不是表单控件,没有 .disabled)。
        // tab-master.js 的 isWriteBlocked() = readOnly || noTimeline,两处都写这一位。
        captureSwitchBlocked: (() => {
            const sw = gb("master-capture-toggle-switch");
            return !!sw && sw.getAttribute("data-disabled") === "1";
        })(),
        pausedPresent: !!paused,
        paused: vis(paused),
        pausedDisplay: paused ? w.getComputedStyle(paused).display : "(缺节点)",
        pausedText: pausedText ? pausedText.textContent.trim() : null,
        tabDot: vis(gb("tabnav-wave-stale-dot")),
        lanesShown: lanes.filter((l) => l.shown).map((l) => l.ch),
        lanesPresent: lanes.every((l) => l.present),
        laneTitle: (lanes.find((l) => l.shown) || {}).title || "",
        captureToggleDisabled: disabled("master-capture-toggle"),
        outputToggleDisabled: disabled("master-output-toggle"),
    };
`);

async function open(scenario) {
    newBucket(scenario);
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?scenario=${scenario}`,
    });
    const ok = await waitFor(READY);
    check(ok, `${scenario}:页面装载并吃到首帧段表`);
    // 切到「波形与分段」页 —— 泳道 ⚠ 的显隐由 tab-wave 的 render 翻,得让它真渲染一次。
    await evaluate(
        IN(`const b = gb("tabnav-wave"); if (b) b.click(); return true;`),
    );
    await sleep(400);
    return await evaluate(PROBE);
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
    log("=== ① scenario=stale:三处提示真的进了 DOM(04 §4.5)===");
    {
        const p = await open("stale");
        check(p !== null, "取到页内 DOM 快照");
        check(p.lanesPresent, "15 条泳道都有 ⚠ 角标节点(模板真的加了这一件)");
        check(p.banner, "横幅 ⑧「采集数据已过期」可见");
        check(
            p.bannerDisplay !== "none",
            `横幅 ⑧ 的 computed display 不是 none(实得 ${p.bannerDisplay})`,
        );
        check(
            /\b3\b/.test(p.bannerText || ""),
            `横幅文案填进了轨数 3(实得「${p.bannerText}」)`,
        );
        check(
            !/\{m\}/.test(p.bannerText || ""),
            "横幅占位符 {m} 已被替换(不是把模板原样显示出来)",
        );
        check(p.tabDot, "tab 导航「波形与分段」的琥珀点亮起");
        eq(
            p.lanesShown,
            STALE_CHANNELS,
            "只有 2/5/11 三条泳道挂 ⚠(其余 12 条收起)",
        );
        check(
            (p.laneTitle || "").length > 0,
            `泳道 ⚠ 带整句 tooltip(实得「${p.laneTitle}」)`,
        );
        check(
            /重新采集|re-capture|recapture/i.test(p.laneTitle || ""),
            `tooltip 说的是「建议重新采集」(实得「${p.laneTitle}」)`,
        );
        // 04 §4.5:只提示,不自动失效、不阻断任何操作。
        check(
            !p.captureToggleDisabled,
            "stale 不 disable 采集开关(只提示,不阻断)",
        );
        check(
            !p.outputToggleDisabled,
            "stale 不 disable 输出开关(只提示,不阻断)",
        );
        // [SL-239] ⑨ 的**反向**,而且是**有牙齿的那一种**:本场景采集是 **ON**、段表也在,
        // 唯一差别就是 stale 已置位 —— ⑨ 必须让位,只留 ⑧。
        //
        // 这个组合真机可达(stale 是闩住的,`setCaptureEnabled` 不清它):
        // 「看到 ⚠ → 打开采集准备重采 → 还没按播放」。若 ⑨ 不让位,用户会同时收到
        // 「建议重新采集」和「先关掉采集」两条互相打架的指令 —— 那正是本卡要修的那类毛病。
        check(p.pausedPresent, "横幅 ⑨ 的节点在模板里(不是选择器写错了)");
        check(
            p.captureEnabled,
            `前置:本场景采集确实是 ON(否则下一条退化成白测,实得 ${p.captureEnabled}）`,
        );
        check(!p.paused, "stale 已挂 ⇒ 横幅 ⑨ 让位,只留 ⑧(采集 ON 也不出)");
        check(
            !p.voided,
            "未布防 ⇒ 横幅 ⑩ 收起(反向:它只在布防在 ∧ 采集关时出)",
        );
        assertClean("scenario=stale");
    }

    // =========================================================================
    log("=== ② 反向 scenario=connected:素材没变 ⇒ 三处提示全部收起 ===");
    {
        const p = await open("connected");
        check(p !== null, "取到页内 DOM 快照");
        check(!p.banner, "横幅 ⑧ 收起");
        check(!p.tabDot, "tab 导航琥珀点熄灭");
        eq(p.lanesShown, [], "没有任何泳道挂 ⚠");
        // [SL-239] 而本场景采集是 **ON** 且段表已在 ⇒ 横幅 ⑨ 必须亮,并把可执行动作说出来。
        // 这正是用户 v5.6.2 实测所处的那一态:他改狠了上游 EQ 却什么都没等到,
        // 而真因是采集还开着(采集 ON 期间 Input 一条 fp_report 都不发)。
        check(p.paused, "采集 ON + 已有段表 ⇒ 横幅 ⑨「比对已暂停」亮起");
        check(
            p.pausedDisplay !== "none",
            `横幅 ⑨ 的 computed display 不是 none(实得 ${p.pausedDisplay})`,
        );
        check(
            /关掉采集|turn capture off|désactivez la capture/i.test(
                p.pausedText || "",
            ),
            `横幅 ⑨ 说出了可执行动作「先关掉采集」(实得「${p.pausedText}」)`,
        );
        assertClean("scenario=connected");
    }

    // =========================================================================
    // [SL-239] ③ 布防期:采集是 §1.23 裁定① **替用户打开**的,⑨ 必须收起。
    //
    // 这一档是 #158 复审【重要】1:此刻用户正应当保持采集开着把选区播完,而 ⑨ 会叫他
    // 「先关掉采集」——**指令方向正好相反**;照做还会被记成「用户接管」(裁定③),
    // 连撤防恢复原值都一并作废。判据里 `!recapture.armed` 那一项守的就是这里。
    log("=== ③ scenario=recapture-armed:布防期 ⑨ 必须收起(否则是反向指令)===");
    {
        const p = await open("recapture-armed");
        check(p !== null, "取到页内 DOM 快照");
        check(
            p.captureEnabled,
            `前置:布防已替用户打开采集(否则本档退化成白测,实得 ${p.captureEnabled}）`,
        );
        eq(
            p.lanesShown,
            [],
            "布防期没有 stale 轨(前置:⑨ 的收起不是被 ⑧ 顶掉的)",
        );
        check(
            !p.paused,
            "布防期 ⇒ 横幅 ⑨ 收起(采集 ON + 段表在 + 无 stale,只差布防位)",
        );
        assertClean("scenario=recapture-armed");
    }

    // =========================================================================
    // [SL-247] ④ `noTimeline` 在场:⑨ 必须让位给 ⑥,否则是**做不到且归因说反**的动作。
    //
    // 来自 #158 复审(deepseek)。与只读态**不同类**:只读态下「比对暂停是因为采集开着」
    // 仍然为真(只是他关不了),而 noTimeline 下真因是**没有时间线** —— ⑨ 若还在,
    // 就把停摆归因到采集开关上,而那把开关此时是 disabled(写控件闸 = readOnly || noTimeline),
    // 用户既关不掉也没得播,照做 ⚠ 也不会回来。
    log("=== ④ scenario=no-timeline:⑨ 让位给 ⑥(否则给的是做不到的动作)===");
    {
        const p = await open("no-timeline");
        check(p !== null, "取到页内 DOM 快照");
        check(
            p.captureEnabled,
            `前置:本场景采集确实是 ON(否则 ⑨ 本来就不该出,这一档退化成白测,实得 ${p.captureEnabled}）`,
        );
        check(
            p.noTimelineBanner,
            "横幅 ⑥「宿主未提供时间线」在场(它才是说真因的那条)",
        );
        check(
            p.captureSwitchBlocked,
            "前置:采集开关此时确实被写闸挡住(data-disabled=1)—— ⑨ 的「先关掉采集」做不到",
        );
        check(!p.paused, "noTimeline ⇒ 横幅 ⑨ 让位,只留 ⑥");
        assertClean("scenario=no-timeline");
    }

    // =========================================================================
    // [SL-247 / J92a] ⑤ 布防还在、采集已关 ⇒ 横幅 ⑩「这次重采集不会记录任何东西」。
    //
    // 这一态**早于本卡就可达**(布防期手动关采集 = §1.23 裁定③ 的接管),而此前界面上
    // 一个字都没有 —— 用户只会觉得「布防着却什么都没采到」。J92a 又给它加了第二条路
    // (布防期手动开跟随引擎,互斥把采集关了),两条路对页面是同一态。
    log("=== ⑤ scenario=recapture-voided:布防在、采集关 ⇒ ⑩ 亮 ===");
    {
        const p = await open("recapture-voided");
        check(p !== null, "取到页内 DOM 快照");
        check(p.voidedPresent, "横幅 ⑩ 的节点在模板里");
        check(!p.captureEnabled, "前置:本场景采集确实是 OFF(⑩ 的判据之一)");
        check(p.voided, "布防在 ∧ 采集关 ⇒ 横幅 ⑩ 亮");
        check(
            /重新打开采集|Turn capture back on|Réactivez la capture/i.test(
                p.voidedText || "",
            ),
            `横幅 ⑩ 给出了出路(实得「${p.voidedText}」)`,
        );
        // ⑨ 在这一档必须收起 —— 判据里 capture_enabled 为假,它本来就不该出;
        // 断上一句是为了钉住两条横幅不会同框说两套话。
        check(!p.paused, "⑩ 在场时 ⑨ 收起(采集是关的,⑨ 的前提本就不成立)");
        assertClean("scenario=recapture-voided");
    }

    // =========================================================================
    // [SL-247 / J92a] ⑥ 「写入双后果」确认板:开引擎 ⇒ 真的出现;再开采集 ⇒ 收起。
    //
    // 本档守的是**行为**:板子该出现时出现、互斥拨掉引擎时收起。
    //
    // ⚠ **它分辨不了「与门写对了」与「与门把板子吃了」** —— 这一句是订正:本卡曾在
    // commit message 与 PR 处置帖里宣称「退回单判与门 ⇒ 本档当场红」,那是**半命中注入**
    // 造成的假红,不成立(#162 复审第三轮 deepseek 提出质疑,本卡精确还原第一版后核实)。
    // 机理:`call()` 虽是 async,但 `bridge[name](...)` 在第一个 await 之前**同步求值**,
    // 而 mock 的 `patchState → emit` 也是同步派发 —— 预览世界里 `output_enabled` 在
    // `showWriteConfirm()` 渲染之前就已翻真,状态单判的与门根本不会 hide。
    // **那个回归是真机时序独有的,自动化套件结构上测不出。**
    // 守它的是 `smoke-tab3-interactions` 的源码级断言(「showWriteConfirm 不直写 hidden」)。
    log("=== ⑥ 写入确认板:开引擎 ⇒ 出现;开采集(互斥关引擎)⇒ 收起 ===");
    {
        const p0 = await open("connected");
        check(p0 !== null, "取到页内 DOM 快照");
        // 回到 Tab1(前面几档把页面切到了「波形与分段」)。
        await evaluate(
            IN(`const b = gb("tabnav-master"); if (b) b.click(); return true;`),
        );
        await sleep(200);

        const confirmHidden = () =>
            evaluate(
                IN(
                    `const n = gb("master-write-confirm"); return n ? !!n.hidden : null;`,
                ),
            );
        const capOn = () =>
            evaluate(
                IN(`const n = gb("master-capture-toggle-switch");
                    return !!n && n.getAttribute("aria-checked") === "true";`),
            );

        check((await confirmHidden()) === true, "前置:确认板初始收起");

        // 开跟随引擎 —— 本工程会话首次 OFF→ON,05 §2.0 要求就地展开双后果。
        await evaluate(
            IN(
                `const n = gb("master-output-toggle-switch"); if (n) n.click(); return true;`,
            ),
        );
        await sleep(400);
        check(
            (await confirmHidden()) === false,
            "★ 开引擎 ⇒ 确认板真的出现在 DOM 上(守行为;分辨不了单判与门那一版,见档头)",
        );

        // 再开采集 —— J92a 互斥把引擎拨掉 ⇒ 板子必须跟着收起。
        await evaluate(
            IN(
                `const n = gb("master-capture-toggle-switch"); if (n) n.click(); return true;`,
            ),
        );
        await sleep(400);
        check(await capOn(), "前置:采集确实开起来了(否则下一条是空转)");
        check(
            (await confirmHidden()) === true,
            "★ 互斥把引擎关掉 ⇒ 确认板跟着收起(不再摊着讲已不成立的双后果)",
        );
        assertClean("scenario=connected(写入确认板序列)");
    }

    // =========================================================================
    // [SL-373] ⑦ 建议类横幅的 ✕:关得掉、本会话内同一条不再出现、条件变了要能再出现。
    //
    // 用户 v5.6.8 原话:「上方的黄色警告横幅加一个 x 可以关掉,不然一直在很烦」。
    //
    // 为什么必须页面级:这条链是「点击 → 会话记忆 → 下一帧 renderBanners 重算显隐」,
    // 而 renderBanners 每帧都跑 —— 只把 hidden 设一次的实现在源码级看得见、在页面上
    // 下一帧就被打回来。三段里只有最后一段是肉眼可见的那一段。
    //
    // 夹具:直接改 mock 的段表并补发一帧 `scvb.segments`,reason 取 `snapshot`
    // ——**不取 `analyze`**:那个 reason 在 `UNDOABLE_REASONS` 里,会把 header 的撤销钮
    // 置亮,给本档掺进一个与判据无关的副作用。
    // `__SCVB_PREVIEW__` 挂在**壳页**上(shell.js:session),所以这几条不走 IN()。
    log("=== ⑦ scenario=stale:横幅 ✕ 的三条判据(SL-373)===");
    {
        const p0 = await open("stale");
        check(p0 !== null, "取到页内 DOM 快照");
        // 首启遮挡(引导页 / tour 询问框)必须先点掉:本档有一格用 `elementFromPoint`
        // 问「这一点命中的是谁」,而 `.sc-scrim` 盖在整页之上 —— 不点掉的话命中的永远是
        // 遮罩,那一格恒红且红得与它要守的东西无关(实测第一版就是 `tour-ask`)。
        // 前面几档只读 hidden 属性,遮罩不碍事,所以 open() 里没有这一步。
        await evaluate(
            IN(`for (const n of ["guide-overlay-start", "tour-ask-later"]) {
                    const b = gb(n);
                    if (b && !b.disabled) b.click();
                }
                return true;`),
        );
        await sleep(300);
        check(
            await evaluate(
                IN(`for (const n of ["guide-overlay", "tour-ask"]) {
                        const o = gb(n);
                        if (o && !o.hidden) return false;
                    }
                    return true;`),
            ),
            "⑦ 前置:首启遮挡都收掉了(elementFromPoint 那一格才问得到真正的命中者)",
        );
        check(p0.banner, "⑦ 前置:横幅 ⑧ 可见(有东西可关)");
        check(p0.tabDot, "⑦ 前置:tab 导航琥珀点亮着");
        check(
            p0.lanesShown.join(",") === STALE_CHANNELS.join(","),
            `⑦ 前置:泳道 ⚠ 在 ${STALE_CHANNELS.join("/")} 三条上(实得 ${p0.lanesShown.join("/")})`,
        );

        // [复审第 1 轮] 几何那半原来读 `getBoundingClientRect()` —— 取的是 button 的
        // **border box**(≈18×16),绝对定位的 `::after` **不计入**,而注释却声称
        // 「量的是钮面 + 两侧扩展」「钉住横轴那一半」:把 `::after` 的 left/right 改成 0、
        // RE-06 的横轴 48px 当场破,那一格照绿。改成拿 `elementFromPoint` 在**钮面左侧
        // 15px**(落在 `::after` 的 −17px 扩展里、还没到钮面)问一句「这一点命中的是谁」——
        // 命中扩展没了就返回横幅正文的 span,当场红。
        // 坐标取 iframe 内文档坐标(`elementFromPoint` 就是这个坐标系),y 取钮面竖中线。
        const DISMISS = IN(`
            const b = gb("banner-staleCapture-dismiss");
            if (!b) return null;
            const r = b.getBoundingClientRect();
            const probeX = r.left - 15;
            const probeY = r.top + r.height / 2;
            const hit = d.elementFromPoint(probeX, probeY);
            return {
                name: b.getAttribute("aria-label") || "",
                w: Math.round(r.width),
                h: Math.round(r.height),
                probeX: Math.round(probeX),
                probeY: Math.round(probeY),
                hitIsDismiss: hit === b,
                hitGb: hit ? hit.getAttribute("data-gb") || hit.tagName : null,
            };
        `);
        const bannerShown = IN(
            `const n = gb("banner-staleCapture"); return !!n && !n.hidden;`,
        );
        const bannerHidden = IN(
            `const n = gb("banner-staleCapture"); return !!n && n.hidden;`,
        );
        const tabDotOff = IN(
            `const n = gb("tabnav-wave-stale-dot"); return !!n && n.hidden;`,
        );
        const bannerText = IN(`
            const n = gb("banner-staleCapture-text");
            return n ? n.textContent.trim() : null;
        `);
        // ★ **每换一次夹具,先等一个与本卡无关的「这一帧真到了 UI」信号,再看横幅。**
        // 理由是实测出来的(D6 那次注入照出来的):横幅此刻**本来就是收起的**,于是
        // `waitFor(bannerHidden)` 当场返回 true、一次都没等,下一条 setStale 就可能抢在
        // 上一帧渲染之前发出去 —— 「条件为假」那一帧根本没被渲染过,`seen.delete` 也就
        // 没跑,本档随机变红。取的信号是**泳道 ⚠ 集合**与 **tab 导航琥珀点**:两者同样
        // 由 §2.8 的 stale 位派生,但渲染在别的函数里(tab-wave / app.js 的 tab 点那行),
        // 与 showDismissible 这条路无关 —— 它们变了 = 这一帧确实到了 UI。
        const lanesNow = IN(`
            const out = [];
            for (let ch = 1; ch <= 15; ch++) {
                const n = gb("wave-lane-" + ch + "-stale");
                if (n && !n.hidden) out.push(ch);
            }
            return out.join(",");
        `);
        const lanesAre = async (chs, label) => {
            const want = chs.join(",");
            const ok = await waitFor(
                IN(`
                    const out = [];
                    for (let ch = 1; ch <= 15; ch++) {
                        const n = gb("wave-lane-" + ch + "-stale");
                        if (n && !n.hidden) out.push(ch);
                    }
                    return out.join(",") === ${JSON.stringify(want)};
                `),
                3000,
            );
            check(ok, `${label}(实得 ${await evaluate(lanesNow)})`);
            return ok;
        };
        // 段表就地改 stale 位 + 补发一帧 §2.8。返回**这一帧真的带了几条 stale**,
        // 用它当正证据:没有它的话「横幅没出现」分不清「判据挡住了」与「夹具没生效」。
        const setStale = (chs) =>
            evaluate(`(() => {
                const s = window.__SCVB_PREVIEW__;
                if (!s || !s.ctl || !s.ctl.model || !s.ctl.model.segByCh) return null;
                const want = ${JSON.stringify(chs)};
                const all = [];
                for (const [ch, entry] of s.ctl.model.segByCh) {
                    entry.stale = want.indexOf(ch) >= 0;
                    all.push(ch);
                }
                all.sort((a, b) => a - b);
                const frame = s.ctl.segmentsPayload("snapshot", all);
                s.ctl.emit("scvb.segments", frame);
                return frame.channels.filter((c) => c.stale).length;
            })()`);

        // ---- ⑦a ✕ 在、有可访问名,且三语各不相同(data-t-aria 真的被 applyI18n 刷到)
        const dz = await evaluate(DISMISS);
        if (check(dz, "⑦a 横幅 ⑧ 上有 ✕ 钮")) {
            check(
                dz.name.length > 0 && !dz.name.startsWith("banner."),
                `⑦a ✕ 的无障碍名取自字典(实得 ${JSON.stringify(dz.name)})`,
            );
            check(
                dz.w > 0 && dz.h > 0,
                `⑦a ✕ 真的上屏了(钮面 ${dz.w}×${dz.h} CSS px —— 这是 border box,不含命中扩展)`,
            );
            // 横轴命中扩展**真的生效**:钮面左 15px 那一点仍命中这枚钮。
            // 横轴合计 = 17(左扩)+ 18(钮面)+ 13(右扩)= 48 CSS px = 0.5 档 24 物理 px;
            // 纵轴只有 36(18 物理 px),是 base.css 里写明的**明知欠达标**(纵向再扩会盖住
            // 相邻横幅的同名钮),本格只钉横轴那一半。
            // ← 把 base.css 里 `.sc-banner__dismiss::after` 的 left/right 改成 0,本格红。
            check(
                dz.hitIsDismiss,
                `⑦a 钮面左 15px 仍命中这枚 ✕(命中扩展生效;实得命中 ${JSON.stringify(dz.hitGb)} @ ${dz.probeX},${dz.probeY})`,
            );
        }
        const names = { zh: dz ? dz.name : "" };
        for (const lang of ["en", "fr"]) {
            await evaluate(
                IN(
                    `const b = gb("header-lang-${lang}"); if (b) b.click(); return true;`,
                ),
            );
            await sleep(300);
            const d2 = await evaluate(DISMISS);
            names[lang] = d2 ? d2.name : "";
            check(
                names[lang].length > 0 && !names[lang].startsWith("banner."),
                `⑦a ${lang} 的无障碍名非空且取自字典(实得 ${JSON.stringify(names[lang])})`,
            );
        }
        // 三语两两不等 —— 只断「非空」的话,fr 整块缺失回退到 en / zh 也会全绿。
        check(
            names.zh !== names.en &&
                names.en !== names.fr &&
                names.zh !== names.fr,
            `⑦a 三语无障碍名两两不同(zh/en/fr = ${JSON.stringify([names.zh, names.en, names.fr])})`,
        );
        await evaluate(
            IN(
                `const b = gb("header-lang-zh"); if (b) b.click(); return true;`,
            ),
        );
        await sleep(300);

        // ---- ⑦b 点 ✕ ⇒ 横幅收起,而**同条件的另外两处提示照旧**
        //   ← 把 showDismissible() 里那句 `show(node, seen.get(gb) !== sig)` 改回
        //     `show(node, true)`(= 拆掉「关过没有」这道查表),本格与 ⑦c 一起红。
        //   ⚠ 这里**先 focus 再 click**:焦点在钮上是「下一帧把它藏起来」这一幕的前提,
        //   ⑦b-f 那一格断的正是焦点被交出去了。纯 `b.click()` 不走焦。
        await evaluate(
            IN(
                `const b = gb("banner-staleCapture-dismiss");
                 if (b) { b.focus(); b.click(); }
                 return true;`,
            ),
        );
        check(await waitFor(bannerHidden, 3000), "⑦b 点 ✕ ⇒ 横幅 ⑧ 收起");
        const after = await evaluate(PROBE);
        if (check(after, "⑦b 关掉之后取到页内快照")) {
            // 正证据:条件本身**一点没变**(⚠ 还在三条泳道上、tab 点还亮着),
            // 所以「横幅没了」只可能是这枚 ✕ 干的,不是条件消失了。
            check(after.tabDot, "⑦b tab 导航琥珀点**不受影响**(仍亮着)");
            check(
                after.lanesShown.join(",") === STALE_CHANNELS.join(","),
                `⑦b 泳道 ⚠ **不受影响**(仍是 ${STALE_CHANNELS.join("/")};实得 ${after.lanesShown.join("/")})`,
            );
        }

        // ---- ⑦b-f [复审第 1 轮,统筹裁定] **焦点不许掉回 `<body>`。**
        //   下一帧 showDismissible 给横幅挂 hidden,而焦点正落在它里面这枚钮上 ⇒
        //   Chromium 把焦点丢给 `<body>`,键盘用户得从卡片开头重走一遍 Tab
        //   (与 tab-settings.js `doReanalyzeFromAsk` 头注 ② 记的是同一类)。
        //   本档只有 ⑧ 一条建议类横幅在场(⑨ 让位、⑩ 条件不成立),所以交接目标落在
        //   「当前那枚 tab」——它在 DOM 里紧跟横幅区、恒可见、恒可聚焦。
        //   ← 把 app.js 里那句 `moveFocusOffDismiss(gb);` 删掉,本格红
        //     (`activeElement` 变成 `BODY`)。
        const c7f = await evaluate(
            IN(`const a = d.activeElement;
                if (!a) return null;
                return {
                    tag: a.tagName,
                    gb: a.getAttribute("data-gb") || null,
                    role: a.getAttribute("role") || null,
                };`),
        );
        if (check(c7f, "⑦b-f 取到 activeElement")) {
            check(
                c7f.tag !== "BODY",
                `⑦b-f 关掉横幅之后焦点没有掉回 <body>(实得 ${JSON.stringify(c7f)})`,
            );
            check(
                c7f.role === "tab",
                `⑦b-f 焦点落在横幅区之后那枚当前 tab 上(实得 role=${JSON.stringify(c7f.role)})`,
            );
        }

        // ---- ⑦c 之后每一帧都不再出现(renderBanners 每帧重算显隐,不是设一次 hidden)
        await sleep(900);
        check(
            await evaluate(bannerHidden),
            "⑦c 近 1s 的多帧渲染之后仍不出现(本会话内同一条不再弹回来)",
        );

        // ---- ⑦d **内容签名变了 ⇒ 再出现**:3 轨 → 2 轨,这是另一句话。
        //   ← 把 renderBanners ⑧ 传给 showDismissible 的 `String(staleTracks)` 改成 `""`
        //     (= 签名不带轨数),本格红,而 ⑦b/⑦c/⑦e 照绿。
        const n2 = await setStale([2, 5]);
        check(n2 === 2, `⑦d 夹具生效:这一帧带 2 条 stale(实得 ${n2})`);
        await lanesAre(
            [2, 5],
            "⑦d 正证据:这一帧真的到了 UI —— 泳道 ⚠ 只剩 2/5",
        );
        check(
            await waitFor(bannerShown, 3000),
            "⑦d 轨数从 3 变 2 ⇒ 横幅**再次出现**(签名变了 = 另一句话)",
        );
        const t2 = await evaluate(bannerText);
        check(
            /(^|[^0-9])2([^0-9]|$)/.test(t2 || ""),
            `⑦d 横幅文案写的是 2 轨(实得「${t2}」)`,
        );

        // ---- ⑦e **条件消失 ⇒ 记录清掉 ⇒ 同样的签名也要能再出现**
        //   序列:再关一次(记下签名「2」)→ 清光 stale(条件为假)→ 摆回**同样那 2 条**。
        //   最后一步必须再出现;记录不清的话签名逐字相同 ⇒ 永远不再出现。
        //   ← 把 showDismissible() 里 `!on` 那支的 `seen.delete(gb)` 删掉,只红本格最后一条
        //     (⑦b/⑦c/⑦d 全绿 —— 那正是「永久关闭」这个错误实现的样子)。
        await evaluate(
            IN(
                `const b = gb("banner-staleCapture-dismiss"); if (b) b.click(); return true;`,
            ),
        );
        check(await waitFor(bannerHidden, 3000), "⑦e 再关一次(记下签名「2」)");
        const n0 = await setStale([]);
        check(n0 === 0, `⑦e 夹具生效:这一帧零 stale(实得 ${n0})`);
        // ★ 这两条**必须在下一次 setStale 之前**把「零 stale 那一帧渲染过了」钉死:
        // 横幅此刻已经收着,拿 bannerHidden 等于没等(见上面 lanesAre 那段注释)。
        check(
            await waitFor(tabDotOff, 3000),
            "⑦e 正证据:零 stale 那一帧真的渲染过了(tab 导航琥珀点灭)—— 于是关掉记录也删了",
        );
        await lanesAre([], "⑦e 正证据:泳道 ⚠ 全部收起");
        check(
            await evaluate(bannerHidden),
            "⑦e 条件消失 ⇒ 横幅收起(这一帧同时把关掉记录删了)",
        );
        const n2b = await setStale([2, 5]);
        check(n2b === 2, `⑦e 夹具生效:又摆回同样的 2 条(实得 ${n2b})`);
        await lanesAre([2, 5], "⑦e 正证据:这一帧也到了 UI —— 泳道 ⚠ 回到 2/5");
        check(
            await waitFor(bannerShown, 3000),
            "⑦e 条件重新成立 ⇒ **同一个签名照样再出现**(关掉是这一次,不是永久)",
        );
        assertClean("scenario=stale(SL-373 ✕)");
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
console.log("\n✅ Output stale 提示页面级冒烟全绿");
process.exit(0);
