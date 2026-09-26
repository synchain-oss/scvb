// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output web 侧状态同步 —— **页面级**冒烟(无头 Chrome + CDP;批 2-C)
// -----------------------------------------------------------------------------
// 七张卡的行为面,全部只在真 DOM / 真键 / 真指针上分辨得出来:
//   ② [SL-469] Tab2 音量卡箍方向键的 300ms 防抖:Ctrl+Z / 切版本之前**冲刷**(那一发先于
//      undo / setVersionActive 到桥面,且之后不再补发);按住拖动中 Ctrl+Z ⇒ 中止、松手零提交。
//      (曲线编辑器那一半 [SL-460] 在 smoke-undo-scope-page.mjs 的 ④b / ⑥ 里,与 SL-450 同套。)
//   ③ [SL-492] 段检查器乐观回声:同轨事件落在「移动 ↔ 松手」之间不清在拖那一维
//      (松手照常提交);提交在途时别的轨来事件不动回声;拖动中 Ctrl+Z ⇒ 中止。
//   ④ [SL-496] 七滑杆键盘档「视为松手」按杆计时(A 杆 250ms 内去按 B 杆,两根脏位都清);
//      任意一根杆计时挂着时 state 回推不覆盖本地;[SL-497] 指针松手后,在飞写的回推
//      追平之前读数不弹回旧值。
//   ⑤ [SL-526] ARMED 确认框开着时起分析 ⇒ 框收起、「继续」不再切版本。
//   ⑥ [SL-499] 只读观察态下曲线编辑器拖拽 / 双击 / 滚轮 / 键盘零提交、工具条收起;
//      每个入口都配「非只读时同一动作确实提交」的对照臂 —— 否则零提交可能只是没打中。
// 观测一律读**计数 / 调用日志下标**(包在 `__SCVB_MOCK__` 上的页内日志)与
// `__SCVB_OUTPUT__.curve()` / `.wave()` 两个只读诊断面,不读画面。
// 时序类(300ms / 140ms / 250ms / 回声窗)都是**定点造窗**:在窗内立刻发第二个动作,
// 或把 mock 的对应函数包成跨宏任务,不靠蹲复现率。
//
// 删除式网格:见 PR 描述(注入未提交,逐格实跑;每格只动一处产品代码)。
//
// 用法:node web-preview/tests/smoke-web-sync-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;2 = 环境里没有 Chrome/Edge;
//   3 = 浏览器在但这一次没起来/没连上(gates 打 [FLAKY-SKIP])。
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
import {
    dirname,
    extname,
    isAbsolute,
    join,
    relative,
    resolve,
} from "node:path";
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

const ROOT_ABS = resolve(ROOT);

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
    const rel = relative(ROOT_ABS, abs);
    if (rel !== "" && (rel.startsWith("..") || isAbsolute(rel))) {
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
    const CDP_DEFAULT_TIMEOUT_MS = 20000;
    return {
        ready,
        on: (fn) => listeners.push(fn),
        send(method, params, timeoutMs) {
            const mid = ++id;
            const budget = timeoutMs || CDP_DEFAULT_TIMEOUT_MS;
            return new Promise((ok, no) => {
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-web-sync-"));
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
process.on("exit", teardown);
for (const sig of ["SIGINT", "SIGTERM"]) {
    process.on(sig, () => {
        teardown();
        process.exit(130);
    });
}
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
        { expression, returnByValue: true, awaitPromise: true },
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

// 同上,但**异步**(要 await 页内 import)。
const IN_ASYNC = (js) => `(async () => {
    const f = document.querySelector("iframe");
    const w = f && f.contentWindow;
    const d = f && f.contentDocument;
    if (!w || !d) return null;
    const q = (s) => d.querySelector(s);
    const gb = (n) => q('[data-gb="' + n + '"]');
    ${js}
})()`;

// 首帧判据:曲线画布已 mount、两个诊断面都挂出、泳道吃到了首帧段表。
const READY = IN(`
    const c = gb("master-pancurve-canvas");
    const lane = gb("wave-lane-2-label");
    return !!(c && c.getAttribute("role") === "application" &&
              w.__SCVB_OUTPUT__ && w.__SCVB_OUTPUT__.curve && w.__SCVB_OUTPUT__.wave &&
              lane && lane.textContent && lane.textContent.length > 0);
`);

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

// ---- 真 Ctrl+Z:走 CDP 的输入管线,不是页内 new KeyboardEvent -----------------
// `modifiers` 位 2 = Ctrl(CDP `Input.dispatchKeyEvent` 的约定)。
async function pressCtrlZ() {
    for (const type of ["keyDown", "keyUp"]) {
        await cdp.send("Input.dispatchKeyEvent", {
            type,
            modifiers: 2,
            key: "z",
            code: "KeyZ",
            windowsVirtualKeyCode: 90,
            nativeVirtualKeyCode: 90,
        });
    }
}

async function mouse(type, x, y, button) {
    await cdp.send("Input.dispatchMouseEvent", {
        type,
        x,
        y,
        button: button || "left",
        buttons: type === "mouseReleased" ? 0 : 1,
        clickCount: 1,
        pointerType: "mouse",
    });
}

const curveDiag = () => evaluate(IN(`return w.__SCVB_OUTPUT__.curve();`));

try {
    let targets = null;
    for (let i = 0; i < CDP_WAIT_TRIES && !targets; i++) {
        try {
            const res = await fetch(`http://127.0.0.1:${CDP_PORT}/json/list`);
            const list = await res.json();
            targets = list.find((t) => t.type === "page") ? list : null;
        } catch {
            targets = null;
        }
        if (!targets) await sleep(CDP_WAIT_STEP_MS);
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
    // 公共:页内调用日志。包在 `window.__SCVB_MOCK__` 上(自有属性遮蔽,原实现照常执行,
    // 页面行为一个字节不改),只留「谁、什么时候、带什么」。判「先后」读下标,不读时间。
    const INSTALL_LOG = IN(`
        const mk = w.__SCVB_MOCK__;
        if (!mk) return false;
        if (!w.__syncLog) {
            w.__syncLog = [];
            for (const n of ["setPanCurve", "undo", "redo", "setVersionActive",
                             "setTrackManual", "editSegment", "setVadParams",
                             "setSegmentation"]) {
                if (typeof mk[n] !== "function") return false;
                const orig = mk[n];
                mk[n] = function (...a) {
                    w.__syncLog.push({ n: n, a: JSON.parse(JSON.stringify(a)) });
                    return orig.apply(mk, a);
                };
            }
        }
        return true;
    `);
    const logLen = () => evaluate(IN(`return w.__syncLog.length;`));
    const logSince = (i) =>
        evaluate(IN(`return w.__syncLog.slice(${i}).map((e) => e.n);`));
    const logEntriesSince = (i) =>
        evaluate(IN(`return w.__syncLog.slice(${i});`));
    const waveDiag = () => evaluate(IN(`return w.__SCVB_OUTPUT__.wave();`));
    const count = (arr, n) => arr.filter((x) => x === n).length;

    async function key(k, code, vk, modifiers) {
        for (const type of ["keyDown", "keyUp"]) {
            await cdp.send("Input.dispatchKeyEvent", {
                type,
                modifiers: modifiers || 0,
                key: k,
                code,
                windowsVirtualKeyCode: vk,
                nativeVirtualKeyCode: vk,
            });
        }
    }
    const arrowUp = () => key("ArrowUp", "ArrowUp", 38);
    const arrowRight = () => key("ArrowRight", "ArrowRight", 39);

    // 元素中心的**页面坐标**(iframe 偏移已加上)—— 真鼠标事件用。
    const centerOf = (sel) =>
        evaluate(
            IN(`const el = q(${JSON.stringify(sel)});
                if (!el) return null;
                const r = el.getBoundingClientRect();
                const fr = f.getBoundingClientRect();
                return { x: fr.left + r.left + r.width / 2,
                         y: fr.top + r.top + r.height / 2,
                         w: r.width, h: r.height };`),
        );
    const focusEl = (sel) =>
        evaluate(
            IN(`const el = q(${JSON.stringify(sel)});
                if (!el) return "缺失";
                w.focus();
                el.focus();
                return d.activeElement === el ? "ok" : String(d.activeElement && d.activeElement.tagName);`),
        );
    const clickGb = (name) =>
        evaluate(
            IN(`const el = gb(${JSON.stringify(name)});
                if (!el) return false;
                el.click();
                return true;`),
        );
    const closeTour = async (label) => {
        check(
            await evaluate(
                IN(`const later = gb("tour-ask-later");
                    if (later) later.click();
                    const ov = gb("tour-ask");
                    return !ov || ov.hidden === true;`),
            ),
            `${label}:tour 询问卡已收起(它的 scrim 会吃掉所有真鼠标事件)`,
        );
    };
    const activeVersion = () =>
        evaluate(IN(`return w.__SCVB_OUTPUT__.curve().activeVersion;`));
    async function switchTo(v, label) {
        check(
            await clickGb(`header-version-chip-${v}`),
            `${label}:点到 V${v} chip`,
        );
        check(
            await waitFor(
                IN(`return w.__SCVB_OUTPUT__.curve().activeVersion === ${v};`),
                6000,
            ),
            `${label}:激活版本 = V${v}`,
        );
    }

    // =========================================================================
    log("=== ① 首帧(默认 fixture:15 轨有段、非只读)===");
    newBucket("首帧");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html`,
    });
    check(await waitFor(READY), "页面装载、诊断面(curve + wave)已挂出");
    await closeTour("①");
    check(await evaluate(INSTALL_LOG), "调用日志装上(八个桥函数都找得到)");
    assertClean("① 首帧");

    // =========================================================================
    log("=== ② [SL-469] Tab2 音量卡箍方向键 300ms 防抖 × Ctrl+Z / 切版本 ===");
    newBucket("Tab2 防抖冲刷");
    {
        check(await clickGb("tabnav-tracks"), "切到 Tab2");
        await sleep(300);
        const COLLAR = '[data-gb="tracks-row-1-vol-collar"]';
        const f0 = await focusEl(COLLAR);
        check(f0 === "ok", `(t0)焦点落到轨 1 音量卡箍上(实得 ${f0})`);
        // 本会话首写会弹「手动首写」确认条(未冻结通道,§1.16 / 05 §2.2 R3)——点掉它。
        await arrowUp();
        await sleep(150);
        const confirmShown = await evaluate(
            IN(`const c = gb("tracks-row-1-manual-overwrite-confirm");
                return !!c && !c.hidden;`),
        );
        if (confirmShown) {
            check(
                await clickGb("tracks-row-1-manual-overwrite-ok"),
                "(t0b)点掉手动首写确认条",
            );
        }
        await sleep(600);

        // ---- 对照臂:按一下方向键、不按 Ctrl+Z ⇒ 300ms 后 setTrackManual 恰一次 ----
        // 没有这一臂,下面「在 undo 之前」那条在「方向键压根没走到防抖」时同样成立。
        check((await focusEl(COLLAR)) === "ok", "(t1)对照臂:焦点仍在卡箍上");
        let i0 = await logLen();
        await arrowUp();
        const early = await logSince(i0);
        eq(
            count(early, "setTrackManual"),
            0,
            "(t2)对照臂:按键当拍**还没发**(确实走的是 300ms 防抖,不是即发)",
        );
        await sleep(600);
        eq(
            count(await logSince(i0), "setTrackManual"),
            1,
            "(t3)对照臂:防抖窗到点,setTrackManual 恰一次",
        );

        // ---- 实验臂:方向键后立刻 Ctrl+Z ⇒ 那一发必须**先于** undo 到桥面 ----
        check((await focusEl(COLLAR)) === "ok", "(t4)实验臂:焦点仍在卡箍上");
        i0 = await logLen();
        await arrowUp();
        await pressCtrlZ();
        check(
            await waitFor(
                IN(
                    `return w.__syncLog.slice(${i0}).some((e) => e.n === "undo");`,
                ),
                3000,
            ),
            "(t5)Ctrl+Z 到达桥面(undo 已发)",
        );
        {
            const seq = await logSince(i0);
            const iM = seq.indexOf("setTrackManual");
            const iU = seq.indexOf("undo");
            check(
                iM >= 0 && iU >= 0 && iM < iU,
                `(t6)**在飞的 setTrackManual 先于 undo 落到桥面**(冲刷,不是丢弃也不是晚到;实得 ${JSON.stringify(seq)})`,
            );
        }
        await sleep(600); // 走完整个 300ms 窗:定时器若没被清掉,这里会再发一次
        eq(
            count(await logSince(i0), "setTrackManual"),
            1,
            "(t7)防抖窗走完后 setTrackManual 仍恰一次(冲刷那一发之后定时器没再开火)",
        );

        // ---- 「等回执」:undo 要等冲刷那一发**落地**才发,不只是排在它后面发 ----
        // 把 mock 的 setTrackManual 包成晚 300ms 才回,记下「回来了」的时刻 ——
        // 只看调用顺序的话,不 await 回执也照样先调后调,这一格就钉不住 await。
        check(
            await evaluate(
                IN(`const mk = w.__SCVB_MOCK__;
                    if (!mk.__origTM) mk.__origTM = mk.setTrackManual;
                    mk.setTrackManual = function (...a) {
                        return new Promise((r) => setTimeout(() => {
                            w.__syncLog.push({ n: "setTrackManual:done", a: [] });
                            r(mk.__origTM.apply(mk, a));
                        }, 300));
                    };
                    return true;`),
            ),
            "(t7a)setTrackManual 已包成晚 300ms 回执",
        );
        check((await focusEl(COLLAR)) === "ok", "(t7b)焦点仍在卡箍上");
        i0 = await logLen();
        await arrowUp();
        await pressCtrlZ();
        check(
            await waitFor(
                IN(
                    `return w.__syncLog.slice(${i0}).some((e) => e.n === "undo");`,
                ),
                3000,
            ),
            "(t7c)undo 到达桥面",
        );
        {
            const seq = await logSince(i0);
            const iD = seq.indexOf("setTrackManual:done");
            const iU = seq.indexOf("undo");
            check(
                iD >= 0 && iU >= 0 && iD < iU,
                `(t7d)**undo 等到冲刷那一发的回执之后才发**(实得 ${JSON.stringify(seq)})`,
            );
        }
        check(
            await evaluate(
                IN(`const mk = w.__SCVB_MOCK__;
                    if (mk.__origTM) { mk.setTrackManual = mk.__origTM; delete mk.__origTM; }
                    return true;`),
            ),
            "(t7e)已还原 mock 的 setTrackManual",
        );
        await sleep(400);

        // ---- 实验臂:方向键后立刻切版本 ⇒ 那一发先于 setVersionActive ----
        check((await focusEl(COLLAR)) === "ok", "(t8)切版本臂:焦点仍在卡箍上");
        i0 = await logLen();
        await arrowUp();
        check(
            await clickGb("header-version-chip-2"),
            "(t9)切版本臂:点 V2 chip",
        );
        check(
            await waitFor(
                IN(
                    `return w.__syncLog.slice(${i0}).some((e) => e.n === "setVersionActive");`,
                ),
                3000,
            ),
            "(t10)setVersionActive 已发",
        );
        {
            const seq = await logSince(i0);
            const iM = seq.indexOf("setTrackManual");
            const iV = seq.indexOf("setVersionActive");
            check(
                iM >= 0 && iV >= 0 && iM < iV,
                `(t11)**在飞的 setTrackManual 先于 setVersionActive**(落在它本来的 V1 上;实得 ${JSON.stringify(seq)})`,
            );
        }
        await sleep(600);
        eq(
            count(await logSince(i0), "setTrackManual"),
            1,
            "(t12)切版本后没有再补发一次(不会落进 V2)",
        );
        await switchTo(1, "(t13)");

        // ---- 按住拖动中 Ctrl+Z ⇒ 中止,松手零提交 ----
        const c = await centerOf(COLLAR);
        check(
            c && c.w > 2 && c.h > 2,
            `(t14)卡箍有真实尺寸(实得 ${JSON.stringify(c)})`,
        );
        // 对照臂:拖一下、松手 ⇒ setTrackManual +1
        i0 = await logLen();
        await mouse("mousePressed", c.x, c.y);
        await mouse("mouseMoved", c.x + 24, c.y);
        await mouse("mouseReleased", c.x + 24, c.y);
        await sleep(300);
        eq(
            count(await logSince(i0), "setTrackManual"),
            1,
            "(t15)拖动对照臂:松手提交一次(证明本格看得见这条路径)",
        );
        await sleep(400);
        // ⚠ 对照臂把音量拖大了 ⇒ 卡箍**挪了位置**。沿用旧坐标会按在管体上、拖动根本没开始,
        // 下面「零提交」就成了假绿(删除式第一轮实测如此)。重取坐标并断落点就是卡箍本身。
        const c2 = await centerOf(COLLAR);
        eq(
            await evaluate(
                IN(`const fr = f.getBoundingClientRect();
                    const el = d.elementFromPoint(${c2.x} - fr.left, ${c2.y} - fr.top);
                    return !!el && el === q(${JSON.stringify(COLLAR)});`),
            ),
            true,
            "(t15b)实验臂的按下点**落在卡箍本身**上(重取过坐标)",
        );
        i0 = await logLen();
        await mouse("mousePressed", c2.x, c2.y);
        await mouse("mouseMoved", c2.x + 30, c2.y);
        await pressCtrlZ();
        check(
            await waitFor(
                IN(
                    `return w.__syncLog.slice(${i0}).some((e) => e.n === "undo");`,
                ),
                3000,
            ),
            "(t16)拖动中 Ctrl+Z 到达桥面",
        );
        await mouse("mouseReleased", c2.x + 30, c2.y);
        await sleep(500);
        eq(
            count(await logSince(i0), "setTrackManual"),
            0,
            "(t17)**按住拖动中 Ctrl+Z ⇒ 中止,松手零提交**(不把刚撤掉的写回去)",
        );
    }
    assertClean("② Tab2 防抖冲刷");

    // =========================================================================
    log(
        "=== ③ Tab3 段检查器:[SL-492] 回声按轨过滤 / 在拖那一维不清;拖动中 Ctrl+Z ===",
    );
    newBucket("Tab3 检查器");
    {
        check(await clickGb("tabnav-wave"), "切到 Tab3");
        await sleep(500);
        // 段事件探针:按 ch 记 reason,判「注入真的到了」用。
        check(
            await evaluate(
                IN(`if (!w.__segPush) {
                        w.__segPush = [];
                        w.__SCVB_MOCK__.addEventListener("scvb.segments", (p) => {
                            w.__segPush.push({
                                reason: p && p.reason,
                                chs: ((p && p.channels) || []).map((c) => c && c.ch),
                            });
                        });
                    }
                    return true;`),
            ),
            "(s0)scvb.segments 探针装上",
        );
        // 在泳道 2 上横扫落点,取第一个真的把检查器点出来的那一下(段之间有空隙)。
        let selected = false;
        for (const frac of [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7]) {
            await evaluate(
                IN(`const lanes = gb("wave-lanes");
                    if (!lanes) return null;
                    const r = lanes.getBoundingClientRect();
                    const HEAD_W = 158, SCALE_COL_W = 44;
                    const laneH = (lanes.querySelector('[data-gb="wave-lane-1"]') || {}).offsetHeight || 34;
                    const stageW = Math.max(lanes.clientWidth - HEAD_W - SCALE_COL_W, 0);
                    const cx = r.left + HEAD_W + stageW * ${frac};
                    const cy = r.top + laneH * 1.5 - lanes.scrollTop;
                    const mk = (type) => new w.PointerEvent(type, {
                        bubbles: true, cancelable: true, composed: true,
                        clientX: cx, clientY: cy, button: 0, buttons: 1, pointerId: 1,
                        isPrimary: true, pointerType: "mouse",
                    });
                    lanes.setPointerCapture = () => {};
                    lanes.releasePointerCapture = () => {};
                    lanes.dispatchEvent(mk("pointerdown"));
                    lanes.dispatchEvent(mk("pointerup"));
                    return true;`),
            );
            await sleep(150);
            if ((await waveDiag()).selectedCh > 0) {
                selected = true;
                break;
            }
        }
        check(selected, "(s1)泳道 2 上选中了一个段(检查器有主)");
        const ch = (await waveDiag()).selectedCh;
        const otherCh = ch === 5 ? 6 : 5;
        const knob = await centerOf('[data-gb="inspector-pan-knob"]');
        check(
            knob && knob.w > 2,
            `(s2)检查器 PAN 旋钮有真实尺寸(实得 ${JSON.stringify(knob)})`,
        );

        // ---- (a) 同轨事件落在「最后一次移动 ↔ 松手」之间 ⇒ 在拖的那一维不许清 ----
        let i0 = await logLen();
        await mouse("mousePressed", knob.x, knob.y);
        await mouse("mouseMoved", knob.x, knob.y - 20);
        const mid = await waveDiag();
        check(
            mid.knobDrag === true && Number.isFinite(mid.echo.pan),
            `(s3)拖动中有乐观回声(实得 ${JSON.stringify(mid)})`,
        );
        const p0 = (await evaluate(IN(`return w.__segPush.length;`))) || 0;
        // **同一轨**来一次无关的段事件(锁定位来回翻一次:两发 §2.8,都点名这一轨)
        await evaluate(
            IN(`const mk = w.__SCVB_MOCK__;
                mk.editSegment(${ch}, "set_locked", { segIdx: 0, locked: true });
                mk.editSegment(${ch}, "set_locked", { segIdx: 0, locked: false });
                return true;`),
        );
        check(
            await waitFor(
                IN(
                    `return w.__segPush.slice(${p0}).some((e) => e.chs.includes(${ch}));`,
                ),
                3000,
            ),
            "(s4)同轨的 scvb.segments 确实到了(注入生效)",
        );
        await sleep(100);
        const after = await waveDiag();
        eq(
            after.echo.pan,
            mid.echo.pan,
            "(s5)**同轨事件后,在拖的 PAN 回声原样保留**(旧行为:整包清空)",
        );
        await mouse("mouseReleased", knob.x, knob.y - 20);
        check(
            await waitFor(
                IN(`return w.__syncLog.slice(${i0}).some((e) =>
                        e.n === "editSegment" && e.a[1] === "set_values");`),
                3000,
            ),
            "(s6)**松手照常提交**(旧行为:回声被清 ⇒ 提交判据为假、静默丢失)",
        );
        {
            const ents = (await logEntriesSince(i0)).filter(
                (e) => e.n === "editSegment" && e.a[1] === "set_values",
            );
            eq(
                ents.length && ents[0].a[2].pan,
                mid.echo.pan,
                "(s7)提交的正是拖好的那个值",
            );
        }
        await sleep(500);

        // ---- (b) 提交在途时**别的轨**来事件 ⇒ 回声不动(那一轨的段一个都没变)----
        // 造在途窗:把 mock 的 set_values 包成跨宏任务(默认 mock 是微任务解析,窗几乎不存在)。
        check(
            await evaluate(
                IN(`const mk = w.__SCVB_MOCK__;
                    if (!mk.__origEdit) mk.__origEdit = mk.editSegment;
                    mk.editSegment = function (c, op, pl) {
                        if (op !== "set_values") return mk.__origEdit(c, op, pl);
                        return new Promise((r) =>
                            setTimeout(() => r(mk.__origEdit(c, op, pl)), 800));
                    };
                    return true;`),
            ),
            "(s8)set_values 已包成跨宏任务(造出在途窗)",
        );
        await mouse("mousePressed", knob.x, knob.y);
        await mouse("mouseMoved", knob.x, knob.y - 14);
        await mouse("mouseReleased", knob.x, knob.y - 14);
        const inflight = await waveDiag();
        check(
            inflight.knobDrag === false && Number.isFinite(inflight.echo.pan),
            `(s9)在途窗确实存在(已松手、回声仍挂着;实得 ${JSON.stringify(inflight)})`,
        );
        const p1 = (await evaluate(IN(`return w.__segPush.length;`))) || 0;
        await evaluate(
            IN(`const mk = w.__SCVB_MOCK__;
                mk.editSegment(${otherCh}, "set_locked", { segIdx: 0, locked: true });
                mk.editSegment(${otherCh}, "set_locked", { segIdx: 0, locked: false });
                return true;`),
        );
        check(
            await waitFor(
                IN(`return w.__segPush.slice(${p1}).some((e) =>
                        e.chs.includes(${otherCh}) && !e.chs.includes(${ch}));`),
                3000,
            ),
            "(s10)**另一轨**的 scvb.segments 确实到了(且没点名选中轨)",
        );
        await sleep(100);
        eq(
            (await waveDiag()).echo.pan,
            inflight.echo.pan,
            "(s11)**别的轨来事件,选中段的回声不动**(旧行为:清空 ⇒ 旋钮弹回旧值一拍)",
        );
        await sleep(1000); // 走完在途窗
        check(
            await evaluate(
                IN(`const mk = w.__SCVB_MOCK__;
                    if (mk.__origEdit) { mk.editSegment = mk.__origEdit; delete mk.__origEdit; }
                    return true;`),
            ),
            "(s12)已还原 mock 的 editSegment",
        );

        // ---- (c) 拖动中 Ctrl+Z ⇒ 中止,松手零提交 ----
        i0 = await logLen();
        await mouse("mousePressed", knob.x, knob.y);
        await mouse("mouseMoved", knob.x, knob.y - 26);
        await pressCtrlZ();
        check(
            await waitFor(
                IN(
                    `return w.__syncLog.slice(${i0}).some((e) => e.n === "undo");`,
                ),
                3000,
            ),
            "(s13)拖动中 Ctrl+Z 到达桥面",
        );
        const ab = await waveDiag();
        eq(ab.knobDrag, false, "(s14)Ctrl+Z ⇒ 检查器拖动被中止");
        eq(
            ab.echo.pan,
            undefined,
            "(s14b)Ctrl+Z ⇒ 那一维的乐观回声一并丢弃(旋钮回到引擎值,不显示一个没提交的数)",
        );
        await mouse("mouseReleased", knob.x, knob.y - 26);
        await sleep(400);
        eq(
            (await logEntriesSince(i0)).filter(
                (e) => e.n === "editSegment" && e.a[1] === "set_values",
            ).length,
            0,
            "(s15)**中止之后那一记松手零提交**",
        );

        // ---- (d) 拖动中**远端**换版本(全量类 reason)⇒ 连在拖那一维也清,松手不提交 ----
        // 与 (a) 相反的那一支:同轨事件要保留在拖的值,但换版本之后这一段已经是另一版的段,
        // 松手若照常提交,就是按时间锚把 V1 的值写进 V2。远端切换掐不到 settlePendingEdits。
        await sleep(300);
        i0 = await logLen();
        await mouse("mousePressed", knob.x, knob.y);
        await mouse("mouseMoved", knob.x, knob.y - 18);
        check(
            Number.isFinite((await waveDiag()).echo.pan),
            "(s16)拖动中有乐观回声",
        );
        const p2 = (await evaluate(IN(`return w.__segPush.length;`))) || 0;
        await evaluate(IN(`w.__SCVB_MOCK__.setVersionActive(2); return true;`));
        check(
            await waitFor(
                IN(
                    `return w.__segPush.slice(${p2}).some((e) => e.reason === "versionActive");`,
                ),
                3000,
            ),
            "(s17)远端换版本的 scvb.segments(versionActive)到了",
        );
        await sleep(100);
        eq(
            (await waveDiag()).echo.pan,
            undefined,
            "(s18)**换版本 ⇒ 在拖那一维的回声也清掉**(与 (s5) 相反的那一支)",
        );
        await mouse("mouseReleased", knob.x, knob.y - 18);
        await sleep(400);
        eq(
            (await logEntriesSince(i0)).filter(
                (e) => e.n === "editSegment" && e.a[1] === "set_values",
            ).length,
            0,
            "(s19)换版本后那一记松手零提交",
        );
        await switchTo(1, "(s20)收尾");
    }
    assertClean("③ Tab3 检查器");

    // =========================================================================
    log(
        "=== ④ Tab3 七滑杆:[SL-496] 键盘档按杆计时 / [SL-497] 松手后回声追平前不覆盖 ===",
    );
    newBucket("Tab3 滑杆");
    {
        const TRACK = (n) => `[data-gb="${n}"] .wave-slider__track`;
        const readVal = (n) =>
            evaluate(
                IN(`const el = gb(${JSON.stringify(n + "-val")});
                    return el ? el.textContent.trim() : null;`),
            );
        await sleep(700); // 让前面段落留下的在飞写都过了兜底窗

        // ---- (a) [SL-496] A 杆方向键 → 250ms 内 B 杆方向键 ⇒ 两根都要走到「松手」----
        check(
            (await focusEl(TRACK("wave-vad-threshold"))) === "ok",
            "(k0)焦点落到 THRESHOLD 杆",
        );
        await arrowRight();
        check(
            (await focusEl(TRACK("wave-vad-hysteresis"))) === "ok",
            "(k1)焦点落到 HYSTERESIS 杆",
        );
        await arrowRight();
        const k1 = await waveDiag();
        eq(k1.sliderKeyTimers, 2, "(k2)两根杆**各挂一个**「视为松手」计时");
        await sleep(700);
        const k2 = await waveDiag();
        eq(k2.sliderKeyTimers, 0, "(k3)静默窗过后两根杆的计时都已开火");
        eq(
            k2.dirty.filter(Boolean).length,
            0,
            `(k4)**两根杆的脏位都清了**(旧行为:第一根那一发被清掉、脏位永久卡住;实得 ${JSON.stringify(k2.dirty)})`,
        );
        await sleep(700);

        // ---- (b) [SL-496] 键盘操作途中(任意一根杆计时挂着)state 回推不覆盖 ----
        // 回推造法:直接打 mock 的 setVadParams —— 引擎侧阈值真的变了、§2.1 回推照常来
        // (mock 的回推固定晚 250ms,SL-357)。键盘操作挂在**分段组**的杆上、回推改的是
        // **VAD 组** ⇒ VAD 组没有在飞写(SL-497 的回声闸不在场),挡住覆盖的只可能是
        // 「有杆的键盘计时挂着」这一道。⚠ 回推要在**计时还挂着的时候**到:所以注入之后
        // 每 100ms 按一次方向键、连按 600ms,在这段中途(注入后约 450ms,回推已到)读数。
        const thr0 = await readVal("wave-vad-threshold");
        check(
            (await focusEl(TRACK("wave-seg-minlen"))) === "ok",
            "(k5)焦点落到 MIN SEG 杆",
        );
        await arrowRight();
        const injected = await evaluate(
            IN_ASYNC(`const mk = w.__SCVB_MOCK__;
                const snap = await mk.requestInitialState();
                const vad = { ...((snap.analysis || {}).vad || {}) };
                vad.threshold_db = vad.threshold_db <= -50 ? -30 : -50;
                mk.setVadParams(vad);
                w.__injectAt = performance.now();
                return vad.threshold_db;`),
        );
        check(Number.isFinite(injected), `(k6)回推已注入(阈值 → ${injected})`);
        let midRead = null;
        let midPending = null;
        for (let i = 0; i < 6; i++) {
            await sleep(100);
            await arrowRight();
            if (i === 3) {
                midRead = await readVal("wave-vad-threshold");
                midPending = (await waveDiag()).sliderKeyTimers;
                const since = await evaluate(
                    IN(`return performance.now() - w.__injectAt;`),
                );
                check(
                    since > 300,
                    `(k6b)读数时回推已经到了(注入后 ${Math.round(since)}ms > 250ms 回推延迟)`,
                );
            }
        }
        eq(midPending, 1, "(k6c)读数那一刻 MIN SEG 的键盘计时确实挂着");
        eq(midRead, thr0, "(k7)**有杆的键盘计时挂着时,state 回推不覆盖本地**");
        await sleep(900);
        check(
            (await readVal("wave-vad-threshold")) !== thr0,
            "(k8)对照:计时结束后回推照常生效(证明 (k7) 的回推真的到了)",
        );
        await sleep(700);

        // ---- (c) [SL-497] 指针拖完松手 ⇒ 回推追平之前读数不许弹回 ----
        // 窗是现成的:mock 的 §2.1 回推固定晚 250ms(与真桥同形,SL-357)⇒ 松手那一拍
        // state 里还是拖动之前的值。这段时间里若有任何一拍拿 state 覆盖,读数就弹回 before。
        const tr = await centerOf(TRACK("wave-vad-threshold"));
        check(
            tr && tr.w > 20,
            `(h1)THRESHOLD 杆有真实宽度(实得 ${JSON.stringify(tr)})`,
        );
        const before = await readVal("wave-vad-threshold");
        await mouse("mousePressed", tr.x - tr.w * 0.3, tr.y);
        await mouse("mouseMoved", tr.x + tr.w * 0.3, tr.y);
        await mouse("mouseReleased", tr.x + tr.w * 0.3, tr.y);
        const tRel = Date.now();
        const released = await readVal("wave-vad-threshold");
        check(released !== before, `(h2)拖动改了值(${before} → ${released})`);
        const samples = [];
        for (let i = 0; i < 5; i++) {
            await sleep(40);
            samples.push(await readVal("wave-vad-threshold"));
        }
        eq(
            samples.filter((v) => v !== released),
            [],
            "(h3)**松手后回推追平之前,读数一拍都没弹回旧值**",
        );
        // 追平即放开:回推约 250ms 到,**早于** 500ms 兜底 ⇒ 放开必须来自「值相等」那一支。
        check(
            await waitFor(
                IN(
                    `return w.__SCVB_OUTPUT__.wave().paramInflight.vad === false;`,
                ),
                3000,
            ),
            "(h4)在飞写已结清",
        );
        const settleMs = Date.now() - tRel;
        check(
            settleMs < 470,
            `(h5)**回推追平即放开**,不是等 500ms 兜底(松手后 ${settleMs}ms 结清)`,
        );
        eq(
            await readVal("wave-vad-threshold"),
            released,
            "(h6)结清后读数仍是松手的值",
        );
        await sleep(400);

        // ---- (h7) 同上,分段组那一根(MIN SEG):两组各有自己的在飞写记账,各验一次 ----
        {
            const tm = await centerOf(TRACK("wave-seg-minlen"));
            check(tm && tm.w > 20, "(h7a)MIN SEG 杆有真实宽度");
            const b0 = await readVal("wave-seg-minlen");
            await mouse("mousePressed", tm.x - tm.w * 0.25, tm.y);
            await mouse("mouseMoved", tm.x + tm.w * 0.25, tm.y);
            await mouse("mouseReleased", tm.x + tm.w * 0.25, tm.y);
            const r0 = await readVal("wave-seg-minlen");
            check(r0 !== b0, `(h7b)拖动改了值(${b0} → ${r0})`);
            const ss = [];
            for (let i = 0; i < 5; i++) {
                await sleep(40);
                ss.push(await readVal("wave-seg-minlen"));
            }
            eq(
                ss.filter((v) => v !== r0),
                [],
                "(h7c)**MIN SEG 松手后回推追平之前,读数一拍都没弹回旧值**",
            );
        }
        await sleep(700);

        // ---- (h8) 追不平的那一档:兜底到点必须放开 ----
        // 松手之后引擎侧的值被**别处**改掉(另一实例 / 自动化;这里直接打 mock),回推永远
        // 等不到发出去的那个值 ⇒ 只能靠 PARAM_ECHO_HOLD_MS 兜底放开,否则读数永远停在
        // 用户放下的值上、与引擎不一致。
        {
            const tr2 = await centerOf(TRACK("wave-vad-threshold"));
            await mouse("mousePressed", tr2.x + tr2.w * 0.2, tr2.y);
            await mouse("mouseMoved", tr2.x - tr2.w * 0.2, tr2.y);
            await mouse("mouseReleased", tr2.x - tr2.w * 0.2, tr2.y);
            const rel2 = await readVal("wave-vad-threshold");
            const other = await evaluate(
                IN_ASYNC(`const mk = w.__SCVB_MOCK__;
                    const snap = await mk.requestInitialState();
                    const vad = { ...((snap.analysis || {}).vad || {}) };
                    vad.threshold_db = vad.threshold_db <= -55 ? -25 : -55;
                    mk.setVadParams(vad);
                    return vad.threshold_db;`),
            );
            check(Number.isFinite(other), `(h8a)松手后引擎侧被改成 ${other}`);
            check(
                await waitFor(
                    IN(`const el = gb("wave-vad-threshold-val");
                        return !!el && el.textContent.trim() !== ${JSON.stringify(rel2)};`),
                    3000,
                ),
                "(h8b)**追不平时兜底到点放开**,读数跟上引擎(不永远停在松手的值上)",
            );
        }
    }
    assertClean("④ Tab3 滑杆");

    // =========================================================================
    log("=== ⑤ [SL-526] ARMED 确认框「继续」也受「分析中」闸 ===");
    newBucket("ARMED 确认框");
    {
        check(await clickGb("tabnav-master"), "切回 Tab1");
        await sleep(300);
        // 预览默认在播(42s,落在范围内)⇒ 开输出就是 PRINT、chip 直接不可切。先停走带。
        // 走带开关在**壳页**上(走带是宿主的东西,`__SCVB_MOCK__` 只有桥面上行函数)。
        eq(
            await evaluate(`(() => {
                const s = window.__SCVB_PREVIEW__;
                if (!s || !s.ctl) return "no-session";
                s.ctl.setTransport({ isPlaying: false });
                return "ok";
            })()`),
            "ok",
            "(a-pre)停走带(非播放 ⇒ 开输出后是 ARMED 而不是 PRINT)",
        );
        await sleep(300);
        check(
            await evaluate(
                IN(`const r = w.__SCVB_MOCK__.setOutputEnabled(true);
                    return !!r && r.ok === true;`),
            ),
            "(a0)打开输出(非 PRINT 播放态 ⇒ ARMED)",
        );
        await sleep(300);
        const armedVisible = () =>
            evaluate(
                IN(`const c = gb("header-version-armed-confirm");
                    return !!c && !c.hidden;`),
            );
        // ---- 对照臂:弹框 → 继续 ⇒ setVersionActive ----
        check(await clickGb("header-version-chip-2"), "(a1)对照臂:点 V2 chip");
        check(await armedVisible(), "(a2)对照臂:ARMED 轻确认框弹出");
        let i0 = await logLen();
        check(
            await clickGb("header-version-armed-ok"),
            "(a3)对照臂:点「继续」",
        );
        check(
            await waitFor(
                IN(
                    `return w.__syncLog.slice(${i0}).some((e) => e.n === "setVersionActive");`,
                ),
                3000,
            ),
            "(a4)对照臂:「继续」照常切版本(证明本格看得见这条路径)",
        );
        check(
            await waitFor(
                IN(`return w.__SCVB_OUTPUT__.curve().activeVersion === 2;`),
                6000,
            ),
            "(a5)对照臂:已在 V2",
        );
        // ---- 实验臂:弹框 → 起分析 → 框收起、「继续」不再切 ----
        check(await clickGb("header-version-chip-1"), "(a6)点 V1 chip");
        check(await armedVisible(), "(a7)ARMED 轻确认框弹出");
        check(
            await evaluate(
                IN(`w.__SCVB_MOCK__.analyze();
                    return true;`),
            ),
            "(a8)框开着的时候起一趟分析(mock 直调,等同于用户去 master 页点分析)",
        );
        check(
            await waitFor(
                IN(`const c = gb("header-version-chip-1");
                    return !!c && c.getAttribute("data-disabled") === "1";`),
                3000,
            ),
            "(a9)分析在途:chip 已置灰",
        );
        check(
            await waitFor(
                IN(`const c = gb("header-version-armed-confirm");
                    return !!c && c.hidden;`),
                3000,
            ),
            "(a10)**分析在途:ARMED 确认框已收起**",
        );
        i0 = await logLen();
        // 框藏着也直接 .click() 它 —— 钉「待切版本已清」:只收框不清账,这一下仍会切。
        await clickGb("header-version-armed-ok");
        await sleep(400);
        eq(
            count(await logSince(i0), "setVersionActive"),
            0,
            "(a11)**「继续」不再发 setVersionActive**(待切版本已清,分析不会被切版本取消)",
        );
        check(
            await waitFor(
                IN(`const c = gb("header-version-chip-1");
                    return !!c && c.getAttribute("data-disabled") === "0";`),
                8000,
            ),
            "(a12)分析结束,chip 恢复可点",
        );
        await evaluate(
            IN(`w.__SCVB_MOCK__.setOutputEnabled(false); return true;`),
        );
        await sleep(300);
        await switchTo(1, "(a13)收尾");
    }
    assertClean("⑤ ARMED 确认框");

    // =========================================================================
    log("=== ⑥ [SL-499] 只读观察态:曲线编辑器四入口 + 工具条 ===");
    newBucket("曲线只读闸");
    {
        await cdp.send("Page.navigate", {
            url: `${base}/web-preview/output.html?fixture=second-output`,
        });
        check(await waitFor(READY), "(r0)second-output 装载");
        await closeTour("(r0b)");
        const ro = () =>
            evaluate(IN(`return w.__SCVB_OUTPUT__.curve().readOnly;`));
        const setGroup = (g) =>
            evaluate(IN(`w.__SCVB_MOCK__.setGroupId(${g}); return true;`));
        check(
            (await ro()) === true,
            "(r1)起手是只读观察态(组 1 已有主 Output)",
        );
        await setGroup(2);
        check(
            await waitFor(
                IN(`return w.__SCVB_OUTPUT__.curve().readOnly === false;`),
                5000,
            ),
            "(r2)改到空组 2 ⇒ 接管为主实例、只读解除",
        );
        const ptXY = async (angle, db) =>
            evaluate(
                IN_ASYNC(`
                const m = await import("${base}/web/output/canvas/curve-editor.js");
                const c = gb("master-pancurve-canvas");
                const r = c.getBoundingClientRect();
                const fr = f.getBoundingClientRect();
                return {
                    x: fr.left + r.left + (m.angleToX(${angle}) / m.PLOT_W) * r.width,
                    y: fr.top + r.top + (m.dbToY(${db}) / m.PLOT_H) * r.height,
                };
            `),
            );
        const bell = await ptXY(-24, 1.5); // demo 快照里的 bell 点
        const empty = await ptXY(-90, 8); // 远离所有点的空白处
        const toolbarShown = () =>
            evaluate(
                IN(`const t = gb("master-pancurve-toolbar");
                    return !!t && !t.hidden;`),
            );
        const commits = async () => (await curveDiag()).commits;
        const wheel = () =>
            cdp.send("Input.dispatchMouseEvent", {
                type: "mouseWheel",
                x: bell.x,
                y: bell.y,
                deltaX: 0,
                deltaY: -120,
            });
        const dbl = async (p) => {
            for (const cc of [1, 2]) {
                for (const type of ["mousePressed", "mouseReleased"]) {
                    await cdp.send("Input.dispatchMouseEvent", {
                        type,
                        x: p.x,
                        y: p.y,
                        button: "left",
                        buttons: type === "mousePressed" ? 1 : 0,
                        clickCount: cc,
                        pointerType: "mouse",
                    });
                }
            }
        };

        // [SL-499 按族] 版本改名(双击 chip)与「复制到…」—— 同一道只读闸。
        const renameProbe = () =>
            evaluate(
                IN(`const chip = gb("header-version-chip-1");
                    chip.dispatchEvent(new w.MouseEvent("dblclick", { bubbles: true }));
                    const box = gb("header-version");
                    const mode = box ? box.getAttribute("data-mode") : null;
                    if (mode === "rename") {
                        const inp = gb("header-version-rename-input");
                        inp.dispatchEvent(new w.KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
                    }
                    return mode;`),
            );
        const copyProbe = () =>
            evaluate(
                IN(`const b = gb("header-version-copy");
                    const dis = b.getAttribute("data-disabled");
                    b.click();
                    const c = gb("header-version-copy-confirm");
                    const shown = !!c && !c.hidden;
                    if (shown) gb("header-version-copy-cancel").click();
                    return { dis: dis, shown: shown };`),
            );
        // ---- 对照臂(非只读):四个入口各自真能提交 —— 否则下面的「零提交」可能只是没打中 ----
        await mouse("mousePressed", bell.x, bell.y);
        await mouse("mouseReleased", bell.x, bell.y);
        await sleep(300);
        check(await toolbarShown(), "(r3)对照:选中 bell 点,工具条出来了");
        let c0 = await commits();
        await wheel();
        await sleep(400);
        eq((await commits()) - c0, 1, "(r4)对照:滚轮提交一次");
        await sleep(300);
        check(
            (await focusEl('[data-gb="master-pancurve-canvas"]')) === "ok",
            "(r5)焦点落到曲线画布",
        );
        c0 = await commits();
        await arrowUp();
        await sleep(200);
        eq((await commits()) - c0, 1, "(r6)对照:方向键微调提交一次");
        await sleep(300);
        c0 = await commits();
        await mouse("mousePressed", bell.x, bell.y);
        await mouse("mouseMoved", bell.x + 20, bell.y - 10);
        await mouse("mouseReleased", bell.x + 20, bell.y - 10);
        await sleep(300);
        eq((await commits()) - c0, 1, "(r7)对照:拖拽提交一次");
        await sleep(300);
        c0 = await commits();
        await dbl(empty);
        await sleep(300);
        check((await commits()) - c0 >= 1, "(r8)对照:双击空白加点提交");
        await sleep(300);
        // 重新选中那个 bell 点(拖过之后它挪了位置,取它现在的坐标),再转只读
        const moved = await evaluate(
            IN_ASYNC(`
            const m = await import("${base}/web/output/canvas/curve-editor.js");
            const pts = (w.__SCVB_OUTPUT__.curve().curveSig || "").split("|")
                .map((s) => s.split(":"));
            const bell = pts.find((p) => p[2] === "bell");
            if (!bell) return null;
            const c = gb("master-pancurve-canvas");
            const r = c.getBoundingClientRect();
            const fr = f.getBoundingClientRect();
            return {
                x: fr.left + r.left + (m.angleToX(Number(bell[0])) / m.PLOT_W) * r.width,
                y: fr.top + r.top + (m.dbToY(Number(bell[1])) / m.PLOT_H) * r.height,
            };
        `),
        );
        check(!!moved, "(r9)找回 bell 点的当前位置");
        await mouse("mousePressed", moved.x, moved.y);
        await mouse("mouseReleased", moved.x, moved.y);
        await sleep(300);
        check(await toolbarShown(), "(r10)转只读之前:点已选中、工具条在");
        eq(
            await renameProbe(),
            "rename",
            "(r10b)对照:非只读时双击 chip 进改名(证明本格看得见这条路径)",
        );
        eq(
            await copyProbe(),
            { dis: "0", shown: true },
            "(r10c)对照:非只读时「复制到…」可点、确认框弹出",
        );

        // ---- 转只读 ----
        await setGroup(1);
        check(
            await waitFor(
                IN(`return w.__SCVB_OUTPUT__.curve().readOnly === true;`),
                5000,
            ),
            "(r11)改回组 1 ⇒ 只读观察",
        );
        check(
            await waitFor(
                IN(`const t = gb("master-pancurve-toolbar");
                    return !!t && t.hidden;`),
                3000,
            ),
            "(r12)**只读 ⇒ 工具条收起**(五组按钮 + Q 滑杆一并不可达)",
        );
        eq(
            await evaluate(
                IN(
                    `return gb("master-pancurve").getAttribute("data-readonly");`,
                ),
            ),
            "1",
            "(r13)卡片挂上只读态",
        );
        eq(
            await evaluate(
                IN(
                    `return gb("master-pancurve-canvas").getAttribute("aria-disabled");`,
                ),
            ),
            "true",
            "(r13b)画布 aria-disabled=true(读屏同步知道它不可改)",
        );
        c0 = await commits();
        await wheel();
        await sleep(400);
        eq((await commits()) - c0, 0, "(r14)**只读:滚轮零提交**");
        check(
            (await focusEl('[data-gb="master-pancurve-canvas"]')) === "ok",
            "(r15)焦点落到曲线画布",
        );
        c0 = await commits();
        await arrowUp();
        await key("Enter", "Enter", 13);
        await sleep(300);
        eq(
            (await commits()) - c0,
            0,
            "(r16)**只读:键盘(方向键 / Enter)零提交**",
        );
        // ⚠ 转只读会在页顶挂出横幅②、整页下移 ⇒ 转只读之前取的坐标全部作废
        // (删除式第一轮实测:拖拽入口的闸拿掉仍全绿,因为按下点根本没落在点上)。
        // 重取 bell 点(按当前点表换算)与空白处的坐标,并断两个落点都在画布上。
        const ro1 = await evaluate(
            IN_ASYNC(`
            const m = await import("${base}/web/output/canvas/curve-editor.js");
            const pts = (w.__SCVB_OUTPUT__.curve().curveSig || "").split("|")
                .map((s) => s.split(":"));
            const bell = pts.find((p) => p[2] === "bell");
            const c = gb("master-pancurve-canvas");
            const r = c.getBoundingClientRect();
            const fr = f.getBoundingClientRect();
            const at = (a, g) => ({
                x: fr.left + r.left + (m.angleToX(a) / m.PLOT_W) * r.width,
                y: fr.top + r.top + (m.dbToY(g) / m.PLOT_H) * r.height,
            });
            const pb = at(Number(bell[0]), Number(bell[1]));
            const pe = at(-90, 8);
            const onCanvas = (p) => d.elementFromPoint(p.x - fr.left, p.y - fr.top) === c;
            return { bell: pb, empty: pe, ok: onCanvas(pb) && onCanvas(pe) };
        `),
        );
        check(
            ro1 && ro1.ok,
            `(r16b)只读态下重取的两个落点都在画布上(实得 ${JSON.stringify(ro1)})`,
        );
        const moved2 = ro1.bell;
        const empty2 = ro1.empty;
        c0 = await commits();
        await mouse("mousePressed", moved2.x, moved2.y);
        const dr = await curveDiag();
        await mouse("mouseMoved", moved2.x + 20, moved2.y - 10);
        await mouse("mouseReleased", moved2.x + 20, moved2.y - 10);
        await sleep(300);
        eq(dr.dragging, false, "(r17)只读:按下不进拖动态");
        eq((await commits()) - c0, 0, "(r18)**只读:拖拽零提交**");
        c0 = await commits();
        await dbl(empty2);
        await sleep(300);
        eq((await commits()) - c0, 0, "(r19)**只读:双击零提交**");
        eq(
            await renameProbe(),
            "normal",
            "(r20)**只读:双击版本 chip 不进改名**",
        );
        eq(
            await copyProbe(),
            { dis: "1", shown: false },
            "(r21)**只读:「复制到…」置灰、点了不弹确认框**",
        );
    }
    assertClean("⑥ 曲线只读闸");
} catch (e) {
    fail++;
    console.log(`  [FAIL] 冒烟过程抛错:${e && e.message ? e.message : e}`);
} finally {
    teardown();
}

if (fail > 0) {
    console.log(`\n❌ ${fail} 条断言失败`);
    process.exit(1);
}
console.log("\n✅ Output web 侧状态同步(批 2-C)页面级冒烟全绿");
process.exit(0);
