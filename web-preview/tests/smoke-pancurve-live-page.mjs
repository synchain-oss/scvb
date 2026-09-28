// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output pan 曲线**拖动实时预览** —— 页面级冒烟(无头 Chrome + CDP;[J157] / SL-447)
// -----------------------------------------------------------------------------
// 被测:`web/output/canvas/curve-editor.js` 在拖动 / Q 滑杆 / 中止 / 提交各条路径上**往桥上发了
// 什么、什么时候发**(契约 §1.37 `previewPanCurve(v, points)` 与 §1.17 `setPanCurve`)。
// 为什么必须页面级:节流与「末发必达」是**真定时器 × 真 pointer 事件**的组合,node 侧的纯函数与
// 源码正则一条都证不出来;判据读的是页内给 mock 装的**调用日志**(时刻 + 参数 + 回执),
// 不读画面 —— 画面分不出「发了预览」与「只在本地画了」。
//
// 用户裁定 J157 的四条约束,前两条落在这一侧(后两条在 C++:见 test_authority_params.cpp
// AUTH-PARAMS-15..21 与 test_host_harness.cpp 的 HOST J157 两格):
//   (1) 拖动中预览限速 ≤ 20 Hz、不逐帧 —— 本套 ② 的 (p2)(p3);
//   (2) 预览不进撤销栈、一次手势一次提交 —— 本套 ② 的 (p1)(p6)、③ 的 (q3);
//
// 跑什么(同一条会话上连续走完):
//   ① `?fixture=fifteen-tracks`(demo 快照:pan_curve 6 点)首帧,收掉 tour 罩层,断画布中心命中画布;
//   ② **拖点**:按住第 4 点(angle 0 / 0 dB,bell,离两侧邻点最远)连拖约 1 秒,再做一次「刚发完一份
//      就连动两下」然后停手 —— 断:拖动中零提交 / 拖动中确有预览且间隔 ≥ 50 ms / 每份都带当前版本号、
//      回执 ok / 停手后**最后那一下**被发出去(末发必达:与随后的提交点表逐字相同)/ 松手恰好一次提交 /
//      提交之后零预览 / 拖动期间 store 里的已提交点集一字没动;
//   ②b **松手时节流窗里还压着一份** ⇒ 提交必须把它收掉(否则它会在提交之后才发出去);
//   ③ **Q 滑杆**:拨一下 ⇒ 预览立刻发(不等 140 ms 防抖),防抖到点提交一次,提交之后零预览;
//   ④ **拖动中 Ctrl+Z** ⇒ 补发一次 `previewPanCurve(v, null)`(音频回到已提交曲线),松手零提交;
//   ⑤ **提交没被受理**(把 mock 的 setPanCurve 换成回 `{observer:true}`)⇒ 补发 null;
//   ⑥ **拖动途中点集被远端改短**(`onPointerUp` 的 `idx >= cur.length` 早退,不会走到提交)⇒ 补发 null;
//   ⑦ 每段零 console.error、零未捕获异常。
//
// 删除式(**注入未提交**,逐格实跑过;读数见 PR 描述)——被注入的是产品代码,不是本文件:
//   J1 onPointerMove 不调 schedulePreview ⇒ 红在 ② (p2);
//   J2 schedulePreview 去掉末发定时器(节流窗里来的直接丢)⇒ 红在 ② (p5);
//   J3 PREVIEW_MIN_INTERVAL_MS 改 0 ⇒ 红在 ② (p3);
//   J4 commit() 不调 stopPreview ⇒ 红在 ②b (b3);
//   J5 abortEdit() 不调 stopPreview(true) ⇒ 红在 ④ (c2);
//   J6 armCommit() 不调 schedulePreview ⇒ 红在 ③ (q1);
//   J7 commit() 未受理时不补发 null ⇒ 红在 ⑤ (r2);
//   J8 onPointerUp 早退路径不调 stopPreview(true) ⇒ 红在 ⑥ (e3)。
//
// 用法:node web-preview/tests/smoke-pancurve-live-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;2 = 环境里没有 Chrome/Edge;
//   3 = 浏览器在但这一次没起来/没连上(gates 打 [FLAKY-SKIP])。
//   (浏览器驱动部分照抄 smoke-undo-scope-page.mjs,只改了临时目录前缀。)
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-pancurve-live-"));
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

// 首帧判据:曲线画布挂上了(mount 跑完会把 role/tabindex 写上去),
// 且诊断面已经挂出(app.js 末尾那句 `window.__SCVB_OUTPUT__ = {...}`)。
const READY = IN(`
    const c = gb("master-pancurve-canvas");
    return !!(c && c.getAttribute("role") === "application" &&
              w.__SCVB_OUTPUT__ && w.__SCVB_OUTPUT__.curve);
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

// ---- [J157] 页内调用日志:给 mock 的 previewPanCurve / setPanCurve 各包一层 -----------------
// 记 {n, t(页内 performance.now), a(参数深拷贝), r(回执,异步填)}。`w.__liveRejectCommit` 为真时
// setPanCurve 不落 mock、直接回 `{observer:true}`(⑤ 用:模拟「提交没被受理」)。
// 原函数留在 `w.__liveOrig`,⑥ 的「远端改点集」直接调它,不进日志。
const installLog = () =>
    evaluate(
        IN(`const mk = w.__SCVB_MOCK__;
            if (!mk || typeof mk.previewPanCurve !== "function") return false;
            if (!w.__liveLog) {
                w.__liveLog = [];
                w.__liveOrig = {};
                w.__liveRejectCommit = false;
                for (const n of ["previewPanCurve", "setPanCurve"]) {
                    const orig = mk[n];
                    w.__liveOrig[n] = orig;
                    mk[n] = function (...a) {
                        const e = { n, t: w.performance.now(), a: JSON.parse(JSON.stringify(a)) };
                        w.__liveLog.push(e);
                        if (n === "setPanCurve" && w.__liveRejectCommit) {
                            e.r = { observer: true };
                            return { observer: true };
                        }
                        const r = orig.apply(mk, a);
                        Promise.resolve(r).then((x) => { e.r = x; });
                        return r;
                    };
                }
            }
            return true;`),
    );
const logMark = () => evaluate(IN(`return w.__liveLog.length;`));
const logSince = (k) => evaluate(IN(`return w.__liveLog.slice(${k});`));
const previewsOf = (entries) =>
    entries.filter((e) => e.n === "previewPanCurve");
const commitsOf = (entries) => entries.filter((e) => e.n === "setPanCurve");

// 画布上某个逻辑点(angle, gain_db)此刻的页面坐标;换算走真模块,不在测试里抄第二份。
const pointXY = (angle, db) =>
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

// mock 的 §2.1 回声是异步的(SL-357):上一段提交之后等本地抄本清掉再开下一段。
const settled = () =>
    waitFor(
        IN(`const cd = w.__SCVB_OUTPUT__.curve();
            return cd.hasPreview === false && cd.dragging === false && cd.livePreviewPending === false;`),
        5000,
    );

// 按下之前:收掉可能晚到的 tour 询问卡,并断按下点此刻命中的是画布本身(不是罩层 / 工具条)。
const ensureHit = (pt) =>
    waitFor(
        IN(`const later = gb("tour-ask-later");
            const ov = gb("tour-ask");
            if (later && ov && !ov.hidden) later.click();
            const c = gb("master-pancurve-canvas");
            const fr = f.getBoundingClientRect();
            return d.elementFromPoint(${pt.x} - fr.left, ${pt.y} - fr.top) === c;`),
        5000,
    );
// 诊断:按下点此刻是什么元素(前提格红了时打出来,别让「没进拖动态」只剩一句话)。
const whatIsAt = (pt) =>
    evaluate(
        IN(`const fr = f.getBoundingClientRect();
            const el = d.elementFromPoint(${pt.x} - fr.left, ${pt.y} - fr.top);
            return el ? el.tagName + "." + el.className + "[" + (el.getAttribute("data-gb") || "") + "]" : null;`),
    );

const minGap = (ps) => {
    let g = Infinity;
    for (let i = 1; i < ps.length; i++) g = Math.min(g, ps[i].t - ps[i - 1].t);
    return g;
};

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
    log("=== ① 首帧(fixture=fifteen-tracks:pan_curve 6 点、非只读)===");
    newBucket("首帧");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?fixture=fifteen-tracks`,
    });
    check(await waitFor(READY), "页面装载、曲线编辑器已 mount、诊断面已挂出");
    // demo 快照的 tour 询问卡连着一整块 scrim,真鼠标事件会全落在罩子上 —— 先收掉,再断画布中心
    // 真的命中画布(少了这一断,「拖动没跑起来」会长得和「没发预览」一模一样)。
    check(
        await evaluate(
            IN(`const later = gb("tour-ask-later");
                if (later) later.click();
                const ov = gb("tour-ask");
                return !ov || ov.hidden === true;`),
        ),
        "tour 询问卡已收起",
    );
    check(
        await waitFor(
            IN(`const c = gb("master-pancurve-canvas");
                if (!c) return false;
                const r = c.getBoundingClientRect();
                return d.elementFromPoint(r.left + r.width / 2,
                                          r.top + r.height / 2) === c;`),
            6000,
        ),
        "画布中心点此刻命中画布本身(无遮挡)",
    );
    // 点集与画布尺寸都到位再动手:首帧 READY 只说明编辑器挂上了,demo 的 6 个点经异步回声进 store、
    // 画布经 ResizeObserver 定尺寸都可能晚一拍 —— 早按下去 hitTest 落空,整段「没进拖动态」,
    // 与「没发预览」长得一样(实测 10 次里有 2 次)。
    check(
        await waitFor(
            IN(`const c = gb("master-pancurve-canvas");
                const r = c.getBoundingClientRect();
                return w.__SCVB_OUTPUT__.curve().curveSig.split("|").length === 6 &&
                       r.width > 50 && r.height > 20;`),
            8000,
        ),
        "(p0a)demo 的 6 个点已进 store、画布已布局出真实尺寸",
    );
    check(await installLog(), "(p0)页内调用日志装上(mock 有 previewPanCurve)");
    assertClean("① 首帧");

    // =========================================================================
    log(
        "=== ② 拖点:拖动中实时预览(≤20 Hz、末发必达),拖动中零提交,松手恰好一次 ===",
    );
    newBucket("拖点");
    {
        // 第 4 点(angle 0 / 0 dB,bell):离两侧邻点(-24° / 28°)最远,纵向拖不会越过谁。
        const start = await pointXY(0, 0);
        check(await ensureHit(start), "(p0c)按下点此刻命中画布本身(无罩层)");
        const d0 = await curveDiag();
        const k0 = await logMark();
        await mouse("mousePressed", start.x, start.y);
        const pressed = await curveDiag();
        if (
            !check(
                pressed.dragging === true,
                "(p0b)pointerdown 后确实进了拖动态",
            )
        ) {
            log(
                `  诊断:按下点 ${JSON.stringify(start)} 处是 ${await whatIsAt(start)};diag=${JSON.stringify(pressed)}`,
            );
        }
        // 约 1 秒连续拖:每 ~16 ms 一步,往上抬增益,横向 ±3 px 抖动。
        const tLoop = Date.now();
        let step = 0;
        let x = start.x;
        let y = start.y;
        while (Date.now() - tLoop < 1000) {
            step++;
            x = start.x + ((step % 7) - 3);
            y = start.y - Math.min(36, step * 0.6);
            await mouse("mouseMoved", x, y);
            await sleep(16);
        }
        const loopMs = Date.now() - tLoop;
        const inLoop = previewsOf(await logSince(k0));

        // 末发必达的确定性构造:先停 150 ms(节流窗走空),再连动两下 —— 第一下走「窗口空着立刻发」,
        // 第二下落在它刚打开的 50 ms 窗口里,只能靠末发定时器送出去。然后停手不动。
        await sleep(150);
        await mouse("mouseMoved", x + 2, y - 3);
        await mouse("mouseMoved", x + 4, y - 6);
        const mid = await curveDiag();
        check(
            mid.livePreviewPending === true,
            `(p5a)前提:第二下确实落在节流窗里、在等末发(实得 ${JSON.stringify(mid)})`,
        );
        await sleep(250);
        eq(
            (await curveDiag()).curveSig,
            d0.curveSig,
            "(p8)拖动期间 store 里的已提交点集一字没动(预览不写 state、不回推)",
        );
        const beforeRelease = await logSince(k0);
        eq(
            commitsOf(beforeRelease).length,
            0,
            "(p1)拖动中零提交(不逐帧提交,撤销栈不因拖动增长)",
        );

        await mouse("mouseReleased", x + 4, y - 6);
        await sleep(300);
        const all = await logSince(k0);
        const ps = previewsOf(all.filter((e) => e.t <= all[all.length - 1].t));
        const commits = commitsOf(all);

        check(
            inLoop.length >= 8,
            `(p2)约 1 秒的拖动里确有实时预览(≥ 8 份;实得 ${inLoop.length} 份 / ${loopMs} ms)`,
        );
        const gap = minGap(previewsOf(all));
        log(
            `  (拖动 ${loopMs} ms:预览 ${inLoop.length} 份,最小间隔 ${gap.toFixed(1)} ms,全程预览 ${previewsOf(all).length} 份 / 提交 ${commits.length} 次)`,
        );
        check(
            gap >= 49.9,
            `(p3)相邻两份预览间隔 ≥ 50 ms(≤ 20 Hz;实得最小间隔 ${gap.toFixed(1)} ms)`,
        );
        check(
            inLoop.length <= Math.ceil(loopMs / 50) + 1,
            `(p3b)份数不超过 20 Hz 的上限(实得 ${inLoop.length} 份 / ${loopMs} ms)`,
        );
        check(
            ps.length > 0 &&
                ps.every(
                    (e) =>
                        e.a[0] === 1 &&
                        Array.isArray(e.a[1]) &&
                        e.r &&
                        e.r.ok === true,
                ),
            `(p4)每份预览都带当前版本号 1、整表、回执 ok(实得 ${JSON.stringify(ps.slice(0, 2).map((e) => [e.a[0], e.r]))})`,
        );
        eq(commits.length, 1, "(p6)松手恰好一次提交(一次手势一步)");
        const iCommit = all.findIndex((e) => e.n === "setPanCurve");
        const lastPreviewBeforeCommit = previewsOf(all.slice(0, iCommit)).pop();
        eq(
            lastPreviewBeforeCommit && lastPreviewBeforeCommit.a[1],
            commits[0] && commits[0].a[0],
            "(p5)停手前最后那一下被发了出去:最后一份预览与松手提交的点表逐字相同(末发必达)",
        );
        eq(
            previewsOf(all.slice(iCommit + 1)).length,
            0,
            "(p6b)提交之后零预览(提交收掉了预览)",
        );
        check(
            commits[0] && commits[0].a[0][3].gain_db > 0.5,
            `(p7)拖动真的抬了增益(提交的第 4 点 gain_db > 0.5;实得 ${commits[0] && commits[0].a[0][3].gain_db})`,
        );
        check(await settled(), "(p9)回显到位,本地抄本与预览节流态已清");
    }
    assertClean("② 拖点");

    // =========================================================================
    log("=== ②b 松手时节流窗里还压着一份 ⇒ 提交必须把它收掉 ===");
    newBucket("松手收掉末发");
    {
        // 第 5 点(angle 28 / 1.8 dB,bell),② 没碰过它。
        const p = await pointXY(28, 1.8);
        const kb = await logMark();
        await mouse("mousePressed", p.x, p.y);
        await mouse("mouseMoved", p.x, p.y - 6); // 窗口空着:立刻发
        await mouse("mouseMoved", p.x, p.y - 12); // 落在窗口里:等末发
        const d = await curveDiag();
        check(
            d.dragging === true && d.livePreviewPending === true,
            `(b1)前提:松手前节流窗里压着一份(实得 ${JSON.stringify(d)})`,
        );
        await mouse("mouseReleased", p.x, p.y - 12);
        await sleep(300); // > 50 ms:末发定时器若没被收掉,此时已经开过火
        const all = await logSince(kb);
        eq(commitsOf(all).length, 1, "(b2)松手恰好一次提交");
        const iCommit = all.findIndex((e) => e.n === "setPanCurve");
        eq(
            previewsOf(all.slice(iCommit + 1)).length,
            0,
            "(b3)**提交之后零预览** —— 压着的那一份被提交收掉了(否则它会在松手之后才发出去)",
        );
        check(await settled(), "(b4)回显到位");
    }
    assertClean("②b 松手收掉末发");

    // =========================================================================
    log("=== ③ Q 滑杆:拨一下 ⇒ 预览立刻发,140 ms 防抖到点提交一次 ===");
    newBucket("Q 滑杆");
    {
        // ②b 松手后选中的是第 5 点(bell)⇒ 工具条的 Q 滑杆可见。
        check(
            await waitFor(
                IN(`const qs = q('[data-curve-q]');
                    return !!(qs && qs.offsetParent !== null);`),
                3000,
            ),
            "(q0)前提:Q 滑杆可见(选中的是 bell 点)",
        );
        const kq = await logMark();
        await evaluate(
            IN(`const qs = q('[data-curve-q]');
                qs.value = "2.5";
                qs.dispatchEvent(new w.Event("input", { bubbles: true }));
                return true;`),
        );
        const right = await logSince(kq);
        check(
            previewsOf(right).length === 1 && commitsOf(right).length === 0,
            `(q1)拨完立刻有一份预览、还没有提交(实得 ${JSON.stringify(right.map((e) => e.n))})`,
        );
        await sleep(400); // > 140 ms 防抖窗
        const all = await logSince(kq);
        eq(commitsOf(all).length, 1, "(q2)防抖到点提交一次");
        const iCommit = all.findIndex((e) => e.n === "setPanCurve");
        eq(previewsOf(all.slice(iCommit + 1)).length, 0, "(q3)提交之后零预览");
        const pv = previewsOf(all)[0];
        check(
            pv && pv.a[1].some((pt) => Math.abs(pt.q - 2.5) < 1e-9),
            "(q4)预览里带的就是新 Q 值 2.5",
        );
        check(await settled(), "(q5)回显到位");
    }
    assertClean("③ Q 滑杆");

    // =========================================================================
    log("=== ④ 拖动中 Ctrl+Z ⇒ 补发 previewPanCurve(v, null),松手零提交 ===");
    newBucket("拖动中 Ctrl+Z");
    {
        // 第 3 点(angle -24 / 1.5 dB,bell)。
        const p = await pointXY(-24, 1.5);
        // 真 Ctrl+Z 走 CDP 的输入管线,只送到**有焦点的那个 frame**:先把焦点放进 iframe 里的画布
        // (pointerdown 被 preventDefault,不会顺带给焦点),并断它真的在那儿 —— 焦点不在的话
        // 这一段测的是壳页的 <body>,「没中止」会长得和「中止了但没补发 null」一样。
        check(
            await evaluate(
                IN(`const c = gb("master-pancurve-canvas");
                    w.focus();
                    c.focus();
                    return d.activeElement === c;`),
            ),
            "(c0)前提:焦点在 iframe 里的曲线画布上",
        );
        const dc = await curveDiag();
        const kc = await logMark();
        await mouse("mousePressed", p.x, p.y);
        await mouse("mouseMoved", p.x + 2, p.y - 10);
        check(
            previewsOf(await logSince(kc)).length >= 1,
            "(c1)前提:拖动已发出预览(音频此刻按预览在响)",
        );
        await pressCtrlZ();
        const after = await curveDiag();
        eq(after.dragging, false, "(c1b)Ctrl+Z 中止了拖动");
        const mid = await logSince(kc);
        const nulls = previewsOf(mid).filter((e) => e.a[1] === null);
        check(
            nulls.length === 1 && nulls[0].a[0] === 1,
            `(c2)**补发了一次 previewPanCurve(1, null)** —— 音频回到已提交曲线(实得 ${JSON.stringify(previewsOf(mid).map((e) => (e.a[1] === null ? "null" : "pts")))})`,
        );
        eq(
            after.livePreviewCancels - dc.livePreviewCancels,
            1,
            "(c2b)诊断计数同步 +1",
        );
        await mouse("mouseReleased", p.x + 2, p.y - 10);
        await sleep(300);
        const all = await logSince(kc);
        eq(commitsOf(all).length, 0, "(c3)中止之后那一记松手零提交");
        const iNull = all.findIndex(
            (e) => e.n === "previewPanCurve" && e.a[1] === null,
        );
        eq(
            previewsOf(all.slice(iNull + 1)).length,
            0,
            "(c4)撤回之后不再有任何预览",
        );
        check(await settled(), "(c5)本地态已清");
    }
    assertClean("④ 拖动中 Ctrl+Z");

    // =========================================================================
    log("=== ⑤ 提交没被受理(observer)⇒ 补发 null ===");
    newBucket("提交未受理");
    {
        await evaluate(IN(`w.__liveRejectCommit = true; return true;`));
        const p = await pointXY(-24, 1.5); // ④ 中止了,点还在原处
        const dr = await curveDiag();
        const kr = await logMark();
        await mouse("mousePressed", p.x, p.y);
        await mouse("mouseMoved", p.x + 2, p.y - 10);
        await sleep(80);
        await mouse("mouseReleased", p.x + 2, p.y - 10);
        await sleep(300);
        const all = await logSince(kr);
        const cm = commitsOf(all);
        check(
            cm.length === 1 && cm[0].r && cm[0].r.observer === true,
            `(r1)前提:这次提交被回了 {observer:true}(实得 ${JSON.stringify(cm.map((e) => e.r))})`,
        );
        const iCommit = all.findIndex((e) => e.n === "setPanCurve");
        const tail = previewsOf(all.slice(iCommit + 1));
        check(
            tail.length === 1 && tail[0].a[1] === null,
            `(r2)**提交没被受理 ⇒ 补发 null**(引擎不会替我们撤预览;实得 ${JSON.stringify(tail.map((e) => (e.a[1] === null ? "null" : "pts")))})`,
        );
        eq(
            (await curveDiag()).livePreviewCancels - dr.livePreviewCancels,
            1,
            "(r3)诊断计数同步 +1",
        );
        await evaluate(IN(`w.__liveRejectCommit = false; return true;`));
        check(await settled(), "(r4)本地态已清");
    }
    assertClean("⑤ 提交未受理");

    // =========================================================================
    log(
        "=== ⑥ 拖动途中点集被远端改短(松手走 idx 越界早退、不提交)⇒ 补发 null ===",
    );
    newBucket("远端改短");
    {
        // 第 6 点(angle 72 / -2.2 dB,shelf right)—— 下标 5,远端改成 2 点后必越界。
        const p = await pointXY(72, -2.2);
        const de = await curveDiag();
        const ke = await logMark();
        await mouse("mousePressed", p.x, p.y);
        check((await curveDiag()).dragging === true, "(e0)前提:按住了第 6 点");
        await mouse("mouseMoved", p.x - 2, p.y - 8);
        check(
            previewsOf(await logSince(ke)).length >= 1,
            "(e1)前提:拖动已发出预览",
        );
        // 远端改点集(绕过 UI、不进日志):只剩 2 点。等回声进 store(指纹里只剩 2 段)。
        await evaluate(
            IN(`w.__liveOrig.setPanCurve.call(w.__SCVB_MOCK__, [
                    { angle: -40, gain_db: -3, shape: "bell", q: 1, side: "out" },
                    { angle: 40, gain_db: -3, shape: "bell", q: 1, side: "out" },
                ]);
                return true;`),
        );
        check(
            await waitFor(
                IN(
                    `return w.__SCVB_OUTPUT__.curve().curveSig.split("|").length === 2;`,
                ),
                5000,
            ),
            "(e1b)前提:远端改短的点集已进 store(此刻拖动下标 5 越界)",
        );
        await mouse("mouseReleased", p.x - 2, p.y - 8);
        await sleep(300);
        const all = await logSince(ke);
        eq(commitsOf(all).length, 0, "(e2)越界早退:不提交");
        const nulls = previewsOf(all).filter((e) => e.a[1] === null);
        check(
            nulls.length === 1,
            `(e3)**没有提交来收预览 ⇒ 就地补发 null**(实得 ${nulls.length} 次)`,
        );
        eq(
            (await curveDiag()).livePreviewCancels - de.livePreviewCancels,
            1,
            "(e4)诊断计数同步 +1",
        );
        eq((await curveDiag()).dragging, false, "(e5)拖动态已退出");
    }
    assertClean("⑥ 远端改短");
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
console.log("\n✅ pan 曲线拖动实时预览(J157)页面级冒烟全绿");
process.exit(0);
