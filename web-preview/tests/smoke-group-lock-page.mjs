// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 组冲突态**不锁 GROUP 选择器**的页面级冒烟(SL-381)
// =============================================================================
// 用户 v5.6.11 实测 B22 原话:「第二个组打开之后,GROUP · 分组 / 本实例只属于一个组 /
// A B C D E F G H 会警告,但是所有界面都被锁死导致无法切换成别的组,这个要修。」
//
// 为什么必须页面级:被测的东西是**两层闸的组合**,node 侧的纯函数断言证不出来 ——
//   · JS 闸:`tab-master.js` 的 click handler 读组卡的 `data-disabled` 属性;
//   · CSS 闸:`[data-gb="master-group-selector"][data-disabled="1"] .group-pills`
//     写 `pointer-events: none`。
// 只断属性会漏掉后者(属性对了、CSS 选择器写错照样点不动),而**程序化 `.click()` 又穿透
// `pointer-events: none`** —— 所以两层各断各的:属性/handler 一条,computed 样式一条。
// 这一课来自本仓「显形那半边也会连环」:文案 × 落点 × 下游过滤三个自由度耦合,
// 每修一次就挪个地方。
//
// 跑什么(四段,前三段是同一个页面里连续走完的一条用户路径):
//   ① `?fixture=second-output`(只读观察态):横幅② 在、**组卡 `data-disabled="0"`**、
//      `.group-pills` 的 computed `pointer-events` 不是 none;而**其余锁面照旧** ——
//      采集开关 / 输出开关 / header 撤销重做仍是 `data-disabled="1"`(互不串台);
//   ② 真走一遍出口:点组 E(空组)⇒ 确认条展开且**下一帧渲染之后仍开着**
//      (修复前 `renderGroup` 里 `if (off) local.pendingGroup = 0` 会当场把它抹掉,
//      于是「点得动」也没用)⇒ 点确认 ⇒ 横幅② 撤下、只读解除、采集开关回到可操作、
//      组 E 胶囊 aria-pressed=true;
//   ③ 反向:改回组 A(已有主 Output)⇒ 横幅② 回来、写控件重新锁上,而**组卡仍不锁**
//      (不是单向门:用户随时能再走一次);
//   ④ 反向:PRINT 态**仍然**锁组卡(这一刀没有把唯一那条锁面也拆掉)——
//      输出 ON + 走带在 range 内 ⇒ 组卡 `data-disabled="1"`、`pointer-events: none`、
//      tooltip 是「打印中不可切组」、点胶囊**不**展开确认条;
//   ⑤ 每段零 console.error、零未捕获异常。
//
// 用法:node web-preview/tests/smoke-group-lock-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;**2 = 环境里没有 Chrome/Edge**(口径同
//   smoke-output-stale-page.mjs 与 CLAUDE.md §6);**3 = 浏览器在,但这一次没起来 /
//   没连上**([SL-297],gates 打 `[FLAKY-SKIP]`)。
//
// CDP 那一段与 smoke-output-stale-page.mjs 同源(node 内置 fetch + WebSocket,零依赖 ——
// 仓库红线是不引 puppeteer)。同样不抽公共模块:那份断的是提示面,本份断的是锁面,
// 合并只会让两边被对方的参数面绑住。
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
// [SL-381] PRINT 态 tooltip 的判据直接对**词条真源**,不在这里手抄一句中文 ——
// 手抄的那句会随 U17 审校漂走,而漂走时这一格是绿的(第一版就抄错了:按 05 §2.1 ⓪ 的
// 「打印中不可切组」写,真词条是「自动化写入中不可切换分组」)。
import { T } from "../../web/shared/i18n.js";

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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-group-lock-"));
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

// 页面已经渲染过一轮 Tab1 的判据:组胶囊 A 被 renderGroup 写过 aria-pressed。
// (模板里八枚一律 aria-pressed="false";fixture 的 group_id=1 ⇒ 渲染后 A 变 true。)
const READY = IN(`
    const a = gb("master-group-A");
    return !!(a && a.getAttribute("aria-pressed") === "true");
`);

// 一次取全锁面的实况。**属性与 computed 样式分开取**,理由见文件头注:
// 程序化 click 穿透 pointer-events,只断属性会漏掉 CSS 那一侧写坏的情形。
const PROBE = IN(`
    const vis = (el) => !!el && !el.hidden;
    const card = gb("master-group-selector");
    const pills = card ? card.querySelector(".group-pills") : null;
    const blocked = (name) => {
        const el = gb(name);
        return el ? el.getAttribute("data-disabled") === "1" : null;
    };
    const pressed = [];
    for (const id of ["A","B","C","D","E","F","G","H"]) {
        const p = gb("master-group-" + id);
        if (p && p.getAttribute("aria-pressed") === "true") pressed.push(id);
    }
    const pending = [];
    for (const id of ["A","B","C","D","E","F","G","H"]) {
        const p = gb("master-group-" + id);
        if (p && p.getAttribute("data-pending") === "1") pending.push(id);
    }
    return {
        cardPresent: !!card,
        // ← SL-381 的核心一位:冲突态下这里必须是 "0"。
        cardDisabled: card ? card.getAttribute("data-disabled") : null,
        cardTitle: card ? (card.getAttribute("title") || "") : "",
        confirmOpen: card ? card.getAttribute("data-confirm") : null,
        // CSS 闸:整卡 disabled 时胶囊排 pointer-events:none(index.html 那条规则)。
        pillsPointerEvents: pills ? w.getComputedStyle(pills).pointerEvents : null,
        pillsOpacity: pills ? w.getComputedStyle(pills).opacity : null,
        confirmBarPresent: !!gb("master-group-switch-confirm"),
        pressed: pressed,
        pending: pending,
        // 横幅②(§5.1 secondOutput / §2.3 outputReadOnly 的 UI 落点)。
        bannerSecond: vis(gb("banner-secondOutput")),
        bannerSecondText: (() => {
            const n = gb("banner-secondOutput");
            const t = n ? n.querySelector("[data-t]") : null;
            return t ? t.textContent.trim() : null;
        })(),
        // 其余锁面(应当照旧被只读观察态锁住)。
        captureBlocked: blocked("master-capture-toggle-switch"),
        outputBlocked: blocked("master-output-toggle-switch"),
        undoBlocked: blocked("header-undo"),
        redoBlocked: blocked("header-redo"),
    };
`);

/** 点一枚组胶囊(程序化 click —— 它会穿透 pointer-events,所以 CSS 那一侧另有断言)。 */
const clickPill = (id) =>
    IN(
        `const p = gb("master-group-${id}"); if (!p) return false; p.click(); return true;`,
    );

/** 点改组确认条上的「切换到 {x}」。 */
const clickConfirm = IN(`
    const b = gb("master-group-switch-confirm-btn");
    if (!b) return false;
    b.click();
    return true;
`);

async function open(query) {
    newBucket(query);
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?${query}`,
    });
    const ok = await waitFor(READY);
    check(ok, `${query}:页面装载并渲染过一轮 Tab1`);
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
    log("=== ① 只读观察态:组卡可操作,其余锁面照旧(SL-381)===");
    const p1 = await open("fixture=second-output");
    check(p1 !== null, "取到页内 DOM 快照");
    check(p1.cardPresent, "组卡节点在(模板真的有 master-group-selector)");
    // 先钉「警告确实在」—— 用户看到的那句警告不该消失,要改的只是锁面。
    check(p1.bannerSecond, "横幅②「组 A 已有主 Output,本实例只读观察」可见");
    check(
        !/\{X\}/.test(p1.bannerSecondText || ""),
        `横幅② 的占位符 {X} 已被替换(实得「${p1.bannerSecondText}」)`,
    );
    // ---- SL-381 的核心两位 ----
    eq(
        p1.cardDisabled,
        "0",
        "只读观察态下组卡**不**整组 disabled(用户必须有路可退)",
    );
    check(
        p1.pillsPointerEvents !== "none",
        `组胶囊的 computed pointer-events 不是 none(实得 ${p1.pillsPointerEvents})`,
    );
    // ---- 互不串台:其余锁面一个都没被放开 ----
    eq(p1.captureBlocked, true, "只读观察态:采集开关仍 disabled");
    eq(p1.outputBlocked, true, "只读观察态:输出开关仍 disabled");
    eq(p1.undoBlocked, true, "只读观察态:header 撤销仍 disabled");
    eq(p1.redoBlocked, true, "只读观察态:header 重做仍 disabled");
    eq(p1.pressed, ["A"], "当前组是 A(fixture group_id=1)");
    assertClean("① fixture=second-output 首帧");

    // =========================================================================
    log("=== ② 真走一遍出口:点组 E → 确认 → 只读解除 ===");
    check(await evaluate(clickPill("E")), "点到了组 E 胶囊");
    // **等一帧再断**:确认条能不能撑过下一次 render 才是判据。修复前
    // `renderGroup` 里 `if (off) local.pendingGroup = 0` 会在只读态下当场抹掉它 ——
    // 那种实现下「点得动」也没用,而只断点击后那一刹那的 DOM 是看不出来的。
    // 不写死 sleep 去等,轮询到条件成立(CLAUDE.md §10)。
    check(
        await waitFor(
            IN(`const c = gb("master-group-selector");
                const p = gb("master-group-E");
                return !!c && c.getAttribute("data-confirm") === "1"
                    && !!p && p.getAttribute("data-pending") === "1";`),
            6000,
        ),
        "改组确认条展开且组 E 琥珀预亮,并**撑过了后续渲染**",
    );
    check(await evaluate(clickConfirm), "点到了「切换到 E」");
    check(
        await waitFor(
            IN(`const b = gb("banner-secondOutput");
                const e = gb("master-group-E");
                return !!b && b.hidden
                    && !!e && e.getAttribute("aria-pressed") === "true";`),
            6000,
        ),
        "改组落地:横幅② 撤下(§2.9 active:false)且组 E 变成当前组",
    );
    const p2 = await evaluate(PROBE);
    eq(p2.pressed, ["E"], "当前组换成 E");
    eq(p2.bannerSecond, false, "横幅② 已撤下(冲突态解除)");
    eq(p2.cardDisabled, "0", "改组后组卡仍可操作");
    // 只读解除之后,原先被锁的写控件必须**跟着回来** —— 否则「解除」只解了一半。
    eq(p2.captureBlocked, false, "只读解除后采集开关回到可操作");
    eq(p2.outputBlocked, false, "只读解除后输出开关回到可操作");
    assertClean("② 改到空组");

    // =========================================================================
    log("=== ③ 反向:改回被占的组 A ⇒ 冲突态回来,组卡仍不锁 ===");
    check(await evaluate(clickPill("A")), "点到了组 A 胶囊");
    check(
        await waitFor(
            IN(`const c = gb("master-group-selector");
                return !!c && c.getAttribute("data-confirm") === "1";`),
            6000,
        ),
        "确认条再次展开",
    );
    check(await evaluate(clickConfirm), "点到了「切换到 A」");
    // 两件都要等:横幅② 由 `scvb.error` **同步**推(mock 与真桥都是),而 `group_id`
    // 走 `scvb.state` 回声、**晚一拍**(CLAUDE.md §10:mock 默认异步,与真桥同形)。
    // 只等横幅会在回声到达前就断胶囊,读到的还是上一组 —— 第一版正是这么红的。
    check(
        await waitFor(
            IN(`const b = gb("banner-secondOutput");
                const a = gb("master-group-A");
                return !!b && !b.hidden
                    && !!a && a.getAttribute("aria-pressed") === "true";`),
            6000,
        ),
        "改回被占组:横幅② 重新出现且组 A 变回当前组",
    );
    const p3 = await evaluate(PROBE);
    eq(p3.pressed, ["A"], "当前组回到 A");
    eq(p3.captureBlocked, true, "重新进只读观察:采集开关又锁上");
    eq(p3.cardDisabled, "0", "重新进只读观察后组卡**仍然**可操作(不是单向门)");
    check(
        p3.pillsPointerEvents !== "none",
        `回到冲突态后胶囊 pointer-events 仍不是 none(实得 ${p3.pillsPointerEvents})`,
    );
    assertClean("③ 改回被占组");

    // =========================================================================
    log("=== ④ 反向:PRINT 态仍然锁组卡(唯一那条锁面没被一起拆掉)===");
    // 先从冲突态退出去(组 E 是空组 ⇒ 本实例接管为主实例),否则输出开关是锁着的。
    check(await evaluate(clickPill("E")), "点到了组 E 胶囊(退出只读观察)");
    check(
        await waitFor(
            IN(`const c = gb("master-group-selector");
                return !!c && c.getAttribute("data-confirm") === "1";`),
            6000,
        ),
        "确认条展开",
    );
    check(await evaluate(clickConfirm), "点到了「切换到 E」");
    check(
        await waitFor(
            IN(`const sw = gb("master-output-toggle-switch");
                const e = gb("master-group-E");
                return !!sw && sw.getAttribute("data-disabled") !== "1"
                    && !!e && e.getAttribute("aria-pressed") === "true";`),
            6000,
        ),
        "只读解除,输出开关可操作且当前组是 E",
    );
    // 输出 ON + 走带在 range 内(fixture 的 daw_loop 24-96s,transport 42s 且在播)
    // ⇒ outputPhase = "print"(03 §2.2)。
    check(
        await evaluate(
            IN(`const sw = gb("master-output-toggle-switch");
                if (!sw) return false; sw.click(); return true;`),
        ),
        "点到了输出开关",
    );
    check(
        await waitFor(
            IN(`const c = gb("master-group-selector");
                return !!c && c.getAttribute("data-disabled") === "1";`),
            8000,
        ),
        "PRINT 态:组卡整组 disabled(05 §2.1 ⓪ 逐字)",
    );
    const p4 = await evaluate(PROBE);
    eq(p4.pillsPointerEvents, "none", "PRINT 态:胶囊 pointer-events 为 none");
    check(
        (p4.cardTitle || "").length > 0,
        `PRINT 态:组卡带 tooltip(实得「${p4.cardTitle}」)`,
    );
    eq(
        p4.cardTitle,
        T.zh["master.printLock.group"],
        "PRINT 态 tooltip 逐字等于词条 master.printLock.group(zh)",
    );
    // 点胶囊**不**展开确认条 —— JS 那一层的闸也还在(CSS 挡不住程序化 click)。
    eq(p4.confirmOpen, "0", "PRINT 态:确认条是收起的");
    check(await evaluate(clickPill("B")), "PRINT 态下点了组 B 胶囊");
    await sleep(400);
    const p4b = await evaluate(PROBE);
    eq(p4b.confirmOpen, "0", "PRINT 态:点胶囊**不**展开确认条(JS 闸也在)");
    eq(p4b.pending, [], "PRINT 态:没有胶囊被预亮");
    eq(p4b.pressed, ["E"], "PRINT 态:当前组没变");
    assertClean("④ PRINT 态");
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
console.log("\n✅ 组冲突态不锁 GROUP 选择器 页面级冒烟全绿");
process.exit(0);
