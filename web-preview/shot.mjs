// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// web-preview 截图器 —— 无头 Chrome + CDP,零 npm 依赖(T33 交付)。
// -----------------------------------------------------------------------------
// 为什么要它:预览页是 ES module + 同源 iframe 注入(见 serve.ps1 文件头),
// 只能经 http://127.0.0.1:8823 打开;而「改完想看一眼」不该每次都占用人的眼睛。
// 本脚本起一个**独立 user-data-dir 的无头 Chrome**,用 DevTools 协议导航 → 切 tab
// → 等一拍 → 抓 PNG,给人看或喂给会读图的模型。
//
// 零依赖靠两件 node 内置:`fetch`(拿 CDP target 列表)与全局 `WebSocket`
// (node ≥ 22)。**不要**引入 puppeteer:仓库红线是零运行时依赖,dev 侧也照守。
//
// 注意:无头 Chrome 与真机 Chrome 并非同一套光栅路径 —— 颜色/字体/亚像素可能有
// 极小差异,**设计验收仍以真机为准**,本脚本用于快速回归与「有没有画出来」。
//
// 用法(在仓库根跑;先 pwsh web-preview/serve.ps1 起服):
//   node web-preview/shot.mjs --tab=wave --out=tab3.png
//   node web-preview/shot.mjs --role=output --scenario=recapture-armed --tab=wave
//   node web-preview/shot.mjs --tab=tracks --scale=2 --size=1500x900 --out=zoom.png
//   node web-preview/shot.mjs --tab=wave --click='[data-gb="wave-lane-3"]' --wait=800
//   node web-preview/shot.mjs --url=http://127.0.0.1:8823/web-preview/input.html
// =============================================================================

import { spawn } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

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

// ---------------------------------------------------------------- 参数
const args = new Map();
const clicks = [];
for (const raw of process.argv.slice(2)) {
    const m = /^--([^=]+)(?:=([\s\S]*))?$/.exec(raw);
    if (!m) continue;
    if (m[1] === "click") clicks.push(m[2]);
    else args.set(m[1], m[2] === undefined ? "1" : m[2]);
}
const opt = (k, dflt) => (args.has(k) ? args.get(k) : dflt);

if (args.has("help")) {
    console.log(
        [
            "用法:node web-preview/shot.mjs [选项]",
            "  --url=<完整 URL>        直接指定;不给则按 role/fixture/scenario 拼",
            "  --role=output|input|monitor  默认 output",
            "  --fixture=<名>          empty|fifteen-tracks|misaligned|channel-conflict|second-output|stereo-mixed",
            "  --scenario=<名>         见 05 §2.5(printing / recapture-armed / print-guard / first-run …)",
            "  --lang=zh|en|fr         经 UI 切语言(不改 state 默认)",
            "  --tab=master|tracks|wave|settings   载入后点该 tab",
            "  --click=<CSS 选择器>    额外点击,可重复;按给出顺序执行",
            "  --eval=<JS>             截图前在页内求值(调试用)",
            "  --size=1500x900         视口(CSS px),默认 1500x900",
            "  --scale=1               deviceScaleFactor;看颜色/细节用 2",
            "  --wait=1500             载入后等待毫秒(mock 首帧 + 首绘)",
            "  --settle=600            每次点击后等待毫秒",
            "  --full                  整页截图(超出视口部分一并抓)",
            "  --port=9333             CDP 端口",
            "  --out=<路径>            PNG 落点,默认 web-preview/.shots/<tab>-<时间>.png",
            "  --console               把页面 console 打到 stdout(排障用)",
        ].join("\n"),
    );
    process.exit(0);
}

const PORT = Number(opt("port", 9333));
const [VW, VH] = String(opt("size", "1500x900"))
    .split("x")
    .map((n) => Number(n) || 0);
const SCALE = Number(opt("scale", 1)) || 1;
const WAIT_MS = Number(opt("wait", 1500));
const SETTLE_MS = Number(opt("settle", 600));
const TAB = opt("tab", "");
// 三个壳页各一侧;表外取值回落 output(拼错参数不该白屏)。
// ⚠ 两次 `opt()` 的默认值**必须一致**:曾经第二次写的是 `opt("role", "")` ——
// 不传 `--role` 时判定拿到的是 "output"(在表内 ⇒ 走 then 分支),取值却拿到空串,
// URL 因此拼成 `web-preview/.html` 而 404 白屏。回落逻辑看着还在,实际从没生效过。
const ROLES = ["output", "input", "monitor"];
const ROLE = ROLES.includes(opt("role", "output"))
    ? opt("role", "output")
    : "output";

function buildUrl() {
    if (args.has("url")) return args.get("url");
    const qs = new URLSearchParams();
    // `group` / `play` 是 Monitor 壳页的参数(mock/monitor-mock.js 解析),
    // 与 Output 侧的 fixture/loop 同样只是原样透传。
    for (const k of ["fixture", "scenario", "loop", "role", "group", "play"]) {
        if (args.has(k)) qs.set(k, args.get(k));
    }
    const q = qs.toString();
    return `http://127.0.0.1:8823/web-preview/${ROLE}.html${q ? "?" + q : ""}`;
}

const URL_ = buildUrl();
const OUT =
    opt("out", "") ||
    join(
        "web-preview",
        ".shots",
        `${TAB || ROLE}-${new Date().toISOString().replace(/[:.]/g, "-")}.png`,
    );

function chromePath() {
    if (args.has("chrome")) return args.get("chrome");
    for (const p of CHROME_CANDIDATES) if (existsSync(p)) return p;
    throw new Error(
        "找不到 Chrome/Edge 可执行文件 —— 用 --chrome=<路径> 显式指定",
    );
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------------------------------------------------------------- CDP 小客户端
function cdpConnect(wsUrl) {
    const ws = new WebSocket(wsUrl);
    const pending = new Map();
    const listeners = [];
    let id = 0;
    ws.addEventListener("message", (ev) => {
        const msg = JSON.parse(ev.data);
        if (msg.id && pending.has(msg.id)) {
            const { resolve, reject } = pending.get(msg.id);
            pending.delete(msg.id);
            msg.error
                ? reject(new Error(msg.error.message))
                : resolve(msg.result);
        } else if (msg.method) {
            for (const fn of listeners) fn(msg);
        }
    });
    const ready = new Promise((resolve, reject) => {
        ws.addEventListener("open", resolve, { once: true });
        ws.addEventListener("error", () => reject(new Error("CDP 连接失败")), {
            once: true,
        });
    });
    return {
        ready,
        on: (fn) => listeners.push(fn),
        send(method, params) {
            const mid = ++id;
            return new Promise((resolve, reject) => {
                pending.set(mid, { resolve, reject });
                ws.send(
                    JSON.stringify({ id: mid, method, params: params || {} }),
                );
            });
        },
        close: () => ws.close(),
    };
}

/**
 * 页内注入 `window.__D` = **真源文档**。
 * 预览页是「壳页 + 同源 iframe」(serve.ps1 文件头:iframe 才能注入 mock),
 * UI 的一切节点都在 iframe 里 —— 顶层文档只有工具条。选择器一律走 __D,
 * 否则 `[data-tab-btn]` 永远找不到(本脚本第一版就栽在这)。
 */
const DOC_PRELUDE = `window.__D = (() => {
    const f = document.querySelector("iframe");
    try { return (f && f.contentDocument) || document; } catch { return document; }
})(), 1`;

/** 页内求值(返回 JSON 可序列化值);抛错时把页面异常原样冒上来。 */
async function evaluate(cdp, expression) {
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

// ---------------------------------------------------------------- 主流程
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-shot-"));
const chrome = spawn(
    chromePath(),
    [
        "--headless=new",
        `--remote-debugging-port=${PORT}`,
        `--user-data-dir=${userDataDir}`,
        `--window-size=${VW},${VH}`,
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-extensions",
        "--disable-background-timer-throttling",
        "--force-device-scale-factor=1",
        "about:blank",
    ],
    { stdio: "ignore" },
);

let cdp = null;
let failed = null;
try {
    // 等 CDP 起来(HttpListener 式轮询;Chrome 冷启动约 0.3–2s)
    let targets = null;
    for (let i = 0; i < 60 && !targets; i++) {
        try {
            const res = await fetch(`http://127.0.0.1:${PORT}/json/list`);
            const list = await res.json();
            targets = list.find((t) => t.type === "page") ? list : null;
        } catch {
            await sleep(200);
        }
    }
    if (!targets) throw new Error("Chrome 未在 12s 内开出 CDP 端口");

    const page = targets.find((t) => t.type === "page");
    cdp = cdpConnect(page.webSocketDebuggerUrl);
    await cdp.ready;

    await cdp.send("Page.enable");
    await cdp.send("Runtime.enable");
    if (args.has("console")) {
        cdp.on((msg) => {
            if (msg.method === "Runtime.consoleAPICalled") {
                const text = (msg.params.args || [])
                    .map((a) => a.value ?? a.description ?? "")
                    .join(" ");
                console.log(`[页面 ${msg.params.type}] ${text}`);
            }
            if (msg.method === "Runtime.exceptionThrown") {
                console.log(
                    "[页面 异常] " +
                        (msg.params.exceptionDetails.exception?.description ||
                            msg.params.exceptionDetails.text),
                );
            }
        });
    }
    // 视口与 dpr 走 CDP 覆盖(比命令行 --window-size 准,且能给 scale>1)
    await cdp.send("Emulation.setDeviceMetricsOverride", {
        width: VW,
        height: VH,
        deviceScaleFactor: SCALE,
        mobile: false,
    });

    const loaded = new Promise((resolve) => {
        cdp.on((m) => {
            if (m.method === "Page.loadEventFired") resolve();
        });
    });
    await cdp.send("Page.navigate", { url: URL_ });
    await Promise.race([loaded, sleep(15000)]);
    await sleep(WAIT_MS); // mock 注入 + 首帧事件 + 首绘

    await evaluate(cdp, DOC_PRELUDE); // 之后一切选择器走 __D(iframe 内的真源文档)

    const clickIn = (sel) =>
        evaluate(
            cdp,
            `(() => { const el = window.__D.querySelector(${JSON.stringify(sel)}); if (!el) return false; el.click(); return true; })()`,
        );

    if (args.has("lang")) {
        await clickIn(`[data-lang="${args.get("lang")}"]`);
        await sleep(SETTLE_MS);
    }
    if (TAB) {
        const hit = await clickIn(`[data-tab-btn="${TAB}"]`);
        if (!hit) throw new Error(`页面上找不到 tab 按钮:${TAB}`);
        await sleep(SETTLE_MS);
    }
    for (const sel of clicks) {
        const ok = await clickIn(sel);
        if (!ok) console.warn(`⚠ 选择器无命中,已跳过:${sel}`);
        await sleep(SETTLE_MS);
    }
    if (args.has("eval")) {
        const v = await evaluate(cdp, args.get("eval"));
        console.log("eval →", JSON.stringify(v));
        await sleep(SETTLE_MS);
    }

    // --clip=x,y,w,h:只抓一块并按 scale 放大 —— 看颜色/角标/线宽这类细节时,
    // 整屏缩略图会把 1px 差异糊掉,必须裁切放大看(真机 QA 同款纪律)。
    let clip;
    if (args.has("clip")) {
        const [x, y, w, h] = String(args.get("clip"))
            .split(",")
            .map((n) => Number(n) || 0);
        if (!(w > 0 && h > 0)) throw new Error("--clip 需要 x,y,w,h 四个数");
        clip = { x, y, width: w, height: h, scale: SCALE };
    }
    const shot = await cdp.send("Page.captureScreenshot", {
        format: "png",
        captureBeyondViewport: args.has("full"),
        ...(clip ? { clip } : {}),
    });
    const { mkdirSync } = await import("node:fs");
    const { dirname } = await import("node:path");
    mkdirSync(dirname(OUT), { recursive: true });
    writeFileSync(OUT, Buffer.from(shot.data, "base64"));
    console.log(`✅ ${URL_}${TAB ? "  (tab=" + TAB + ")" : ""}\n   → ${OUT}`);
} catch (e) {
    failed = e;
    console.error("❌ 截图失败:" + e.message);
} finally {
    try {
        cdp?.close();
    } catch {}
    chrome.kill();
    await sleep(300);
    try {
        rmSync(userDataDir, { recursive: true, force: true });
    } catch {}
    process.exit(failed ? 1 : 0);
}
