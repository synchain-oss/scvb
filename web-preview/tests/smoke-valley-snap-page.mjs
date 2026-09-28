// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 边界拖拽「吸附能量谷 / 按住 Alt 关吸附」**页面级**冒烟(J145)
// =============================================================================
// 用户裁定 J145(2026-09-29,「1和2现在都做代码功能」):tooltip 许诺的「拖动时吸附到能量谷,
// 按住 Alt 关闭」要真的成立。native 侧此前回的 `requestWaveform.valleys[]` 是一个**从不填**的
// 空数组,所以真机上从没吸附过;本 PR 让 C++ 填上谷点(host 套件的 `HOST J145` 钉那一半)。
//
// 本套钉的是页面这一半:「拿到谷点之后,拖边界真的会吸上去、按 Alt 真的不吸」。
// 为什么必须页面级:smoke-tab3-interactions.mjs 只断了纯函数 `snapBoundary` 算得对 —— 它绕开了
// 真正出事的那一段:拖拽路径有没有拿**当前视口那一块**的 valleys 去调它、Alt 有没有从指针事件
// 传进去、吸附后的时刻有没有变成 `move_boundary` 的 tS。那几步任何一步断了,纯函数照样绿。
//
// 跑什么(全程真 DOM 事件;唯一的宿主侧动作是停带,不碰页面内部函数):
//   ⓪ 横向缩放条键盘放大两格 ⇒ 页面按新视口重新取块(装探针之前取的块探针看不见);
//   ① 边界上按下 → 拖到某个谷点右侧 3px(SNAP_PX=6 之内)→ 抬起:页面真发出去的
//      `editSegment(ch,"move_boundary",{segIdx,edge,tS})` 里 tS **等于谷点**(毫秒量化后逐位),
//      拖动中手柄 `data-snap=1`;
//   ② 换一条边界,同样拖到谷点右侧 3px,但**按住 Alt**:tS 等于指针处的**裸时刻**、不是谷点,
//      手柄 `data-snap=0`;
//   ③ 全程零未捕获异常、零 console.error。
// 谷点从哪来:mock 的 `requestWaveform` 给「乐句间隙中点」(web/shared/mock-data.js 的
// makeWaveformTile)。本套在 mock 上装探针,记下页面**此刻缓存里那一块**的 valleys 与视口,
// 用页面同一套几何(HEAD_W / SCALE_COL_W / stageWidth)换算像素 —— 所以断言不依赖视口默认值。
//
// 删除式(实测读数见 PR 描述):
//   · tab-wave.js 的 updateBoundDrag 里把 `tile && tile.valleys` 换成 `null`(拖拽不再拿谷点)⇒ ① 红;
//   · updateBoundDrag 里把 `e.altKey` 换成 `false`(Alt 没从指针事件传下去)⇒ ② 红
//     —— 纯函数格(smoke-tab3 的 snapBoundary 三条)对这一处**不可分辨**,它直接传 true;
//   · snapBoundary 里去掉 `altKey ||`(Alt 不再关吸附)⇒ ② 红。
//
// 用法:node web-preview/tests/smoke-valley-snap-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;2 = 环境里没有 Chrome/Edge;3 = 浏览器在但这一次没起来 /
//   没连上(口径与 smoke-seg-restore-page.mjs 相同;2/3 都不判红,gates 与 CI 各自显形)。
// CDP / 静态服务 / 收尾那一套与 smoke-seg-restore-page.mjs 同源(零依赖,不引 puppeteer);
// 那边的逐段说明不在这里复抄。
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
    argv.get("cdp") || 9800 + Math.floor(Math.random() * 400),
);
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-valley-snap-"));
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
log("=== J145 边界拖拽吸附能量谷 —— 页面级 ===");

await cdp.send("Page.navigate", { url: `${base}/web-preview/output.html` });
check(await waitFor(READY), "页面装载并吃到首帧段表");
// 宿主停带:走带时采集进度会按 addedRanges 失效瓦片,拖到一半瓦片被换掉就测不到吸附
// (那是另一件事,不是本套要测的)。走带在壳页的预览会话上(SL-270 同一个开关)。
check(
    await evaluate(`(() => {
    const s = window.__SCVB_PREVIEW__;
    if (!s || !s.ctl || typeof s.ctl.setTransport !== "function") return false;
    s.ctl.setTransport({ timeS: 42, isPlaying: false });
    return true;
})()`),
    "宿主停带(壳页预览会话的走带开关在)",
);
await evaluate(
    IN(`const b = gb("tabnav-wave"); if (b) b.click(); return true;`),
);
await sleep(500);

// ---- 探针:拦下页面真发出去的 requestWaveform(拿「页面此刻缓存里的那一块」)与 editSegment
check(
    await evaluate(
        IN(`
        const m = w.__SCVB_MOCK__;
        if (!m || typeof m.requestWaveform !== "function" || typeof m.editSegment !== "function") return false;
        if (!w.__J145__) {
            const S = (w.__J145__ = { wf: {}, edits: [] });
            const rw = m.requestWaveform.bind(m);
            m.requestWaveform = function (ch, startS, endS, cols) {
                const r = rw(ch, startS, endS, cols);
                const rec = (t) => {
                    if (!t || !Array.isArray(t.valleys)) return;
                    (S.wf[ch] = S.wf[ch] || []).push({ startS, endS, cols, valleys: t.valleys.slice() });
                };
                if (r && typeof r.then === "function") r.then(rec);
                else rec(r);
                return r;
            };
            const ed = m.editSegment.bind(m);
            m.editSegment = function (ch, op, payload) {
                S.edits.push({ ch, op, payload: JSON.parse(JSON.stringify(payload === undefined ? null : payload)) });
                return ed(ch, op, payload);
            };
        }
        return true;
    `),
    ),
    "探针装上(requestWaveform / editSegment 都在 iframe 的 __SCVB_MOCK__ 上)",
);

// 用横向缩放条的键盘档放大两格(role=slider,方向键 = 一格 ZOOM_STEP):视口变了,静止 120ms 后
// 页面按新视口重新取块,探针记下那一块。装探针之前取的块探针看不见,所以要这一步;放大还让
// 每秒的像素更多,6px 的捕获半径落到时间上更窄,用例更好选。
check(
    await evaluate(
        IN(`
        const bar = gb("wave-hzoom-bar");
        if (!bar) return false;
        for (let i = 0; i < 2; i++) {
            bar.dispatchEvent(new w.KeyboardEvent("keydown", { key: "ArrowRight", bubbles: true, cancelable: true }));
        }
        return true;
    `),
    ),
    "横向缩放条在(data-gb=wave-hzoom-bar),键盘放大两格",
);

// 舞台宽 / 瓦片列数:与 tab-wave 的 stageWidth()(HEAD_W 158 + SCALE_COL_W 44)同一套几何。
const STAGE_COLS = `
    const lanes = gb("wave-lanes");
    if (!lanes) return null;
    const stageW = Math.max(lanes.clientWidth - 158 - 44, 0);
    const cols = Math.min(Math.max(Math.round(stageW), 1), 4096);
`;
check(
    await waitFor(
        IN(`${STAGE_COLS}
        const S = w.__J145__;
        return [1, 2, 3, 4, 5].some((ch) => (S.wf[ch] || []).some((r) => r.cols === cols));
    `),
        15000,
    ),
    "放大后页面按新视口重新取了波形块(探针看到了那一块)",
);
await sleep(400); // 回执 → LRU 的那一跳是异步的,给它落地

// 在页内按**页面同一套几何**选拖拽用例:段 j 与段 j+1 之间的空隙里恰有一个谷点 V;
// 目标点 = V 右侧 3px(在 SNAP_PX=6 之内);7px 之内没有别的谷来抢、没有别的边界来抢命中;
// V 与原边界、与目标点的裸时刻都差出 5ms 以上(否则「吸没吸」在毫秒量化后分不开)。
const PLAN = IN(`${STAGE_COLS}
    const S = w.__J145__;
    const P = window.__SCVB_PREVIEW__;
    const out = [];
    for (const ch of [1, 2, 3, 4, 5]) {
        const recs = (S.wf[ch] || []).filter((r) => r.cols === cols);
        if (!recs.length) continue;
        const rec = recs[recs.length - 1];
        const span = rec.endS - rec.startS;
        if (!(span > 0)) continue;
        const X = (t) => ((t - rec.startS) / span) * stageW;
        const entry = P && P.ctl && P.ctl.model && P.ctl.model.segByCh.get(ch);
        const segs = (entry && entry.segments) || [];
        for (let j = 0; j + 1 < segs.length; j++) {
            const a = segs[j];
            const b = segs[j + 1];
            if (!(Number.isFinite(b.t1S) && b.t0S > rec.startS && b.t0S < rec.endS)) continue;
            const inGap = rec.valleys.filter((v) => v > a.t1S && v < b.t0S);
            if (inGap.length !== 1) continue;
            const V = inGap[0];
            const bx = X(b.t0S);
            const tx = X(V) + 3;
            if (!(bx > 1 && bx < stageW - 1 && tx > 1 && tx < stageW - 1)) continue;
            if (rec.valleys.some((v) => v !== V && Math.abs(X(v) - tx) <= 7)) continue;
            if (segs.some((s, k) => k >= 1 && k !== j + 1 && Math.abs(X(s.t0S) - bx) <= 7)) continue;
            const rawT = rec.startS + (tx / stageW) * span;
            const minS = a.t0S + 0.05;
            const maxS = b.t1S - 0.05;
            if (!(V > minS && V < maxS && rawT > minS && rawT < maxS)) continue;
            if (Math.abs(rawT - V) < 0.005 || Math.abs(V - b.t0S) < 0.005) continue;
            out.push({ ch, segIdx: j + 1, tB: b.t0S, V, rawT, bx, tx, nValleys: rec.valleys.length });
        }
    }
    return JSON.stringify(out);
`);
const plans = JSON.parse((await evaluate(PLAN)) || "[]");
log(`  可用的「边界 × 空隙谷点」用例:${plans.length} 个`);
// 两格用两条**不同的**边界(第一格提交后那条边界已经挪到谷点上了)。
const A = plans[0];
const B = plans.find((p) => A && (p.ch !== A.ch || p.segIdx !== A.segIdx));
check(!!A && !!B, "前置:至少找到两条可拖的边界,各自空隙里恰有一个谷点");
if (!A || !B) await finish();
log(
    `  A:ch${A.ch} 边界#${A.segIdx} @${A.tB}s → 谷 ${A.V}s(目标裸时刻 ${A.rawT.toFixed(3)}s)`,
);
log(
    `  B:ch${B.ch} 边界#${B.segIdx} @${B.tB}s → 谷 ${B.V}s(目标裸时刻 ${B.rawT.toFixed(3)}s)`,
);

// 真 DOM 指针事件(与 smoke-seg-restore-page 的 CLICK 同一套几何):边界上按下 → 拖到目标点 → 抬起。
// 抬起前读一次手柄的 data-snap(吸附命中态,A-14)。
const DRAG = (p, alt) =>
    IN(`
    const lanes = gb("wave-lanes");
    if (!lanes) return null;
    const r = lanes.getBoundingClientRect();
    const laneH = (lanes.querySelector('[data-gb="wave-lane-1"]') || {}).offsetHeight || 34;
    const cy = r.top + laneH * (${p.ch} - 0.5) - lanes.scrollTop;
    const mk = (type, sx, buttons) => new w.PointerEvent(type, {
        bubbles: true, cancelable: true, composed: true,
        clientX: r.left + 158 + sx, clientY: cy, button: 0, buttons,
        pointerId: 1, isPrimary: true, pointerType: "mouse", altKey: ${alt ? "true" : "false"},
    });
    lanes.setPointerCapture = () => {};
    lanes.releasePointerCapture = () => {};
    const n0 = w.__J145__.edits.length;
    lanes.dispatchEvent(mk("pointerdown", ${p.bx}, 1));
    lanes.dispatchEvent(mk("pointermove", (${p.bx} + ${p.tx}) / 2, 1));
    lanes.dispatchEvent(mk("pointermove", ${p.tx}, 1));
    const h = gb("wave-boundary-handle");
    const snapAttr = h ? h.getAttribute("data-snap") : null;
    lanes.dispatchEvent(mk("pointerup", ${p.tx}, 0));
    const sent = w.__J145__.edits.slice(n0);
    return JSON.stringify({ snapAttr, sent });
`);

const ms = (t) => Math.round(t * 1000) / 1000;

// ① 不按 Alt:拖到谷点旁 3px ⇒ 提交的 tS 就是谷点(毫秒量化后逐位相等)。
{
    const r = JSON.parse((await evaluate(DRAG(A, false))) || "{}");
    const sent = r.sent || [];
    eq(sent.length, 1, "① 松手恰好发出一次 editSegment");
    const e = sent[0] || {};
    eq(e.op, "move_boundary", "① op = move_boundary");
    eq(e.ch, A.ch, "① 发到被拖的那一轨");
    eq(e.payload && e.payload.segIdx, A.segIdx, "① segIdx = 被拖的那条边界");
    eq(
        e.payload && e.payload.tS,
        ms(A.V),
        `① ★ 吸附:提交的 tS = 谷点 ${ms(A.V)}s(不吸附会是裸时刻 ${ms(A.rawT)}s)`,
    );
    eq(r.snapAttr, "1", "① 拖动中手柄亮「已吸附」(data-snap=1)");
}
await sleep(400); // 段表回推(scvb.segments)落地后再拖下一条

// ② 按住 Alt:同样拖到谷点旁 3px ⇒ 提交裸时刻,不吸。
{
    const r = JSON.parse((await evaluate(DRAG(B, true))) || "{}");
    const sent = r.sent || [];
    eq(sent.length, 1, "② 松手恰好发出一次 editSegment");
    const e = sent[0] || {};
    eq(e.op, "move_boundary", "② op = move_boundary");
    eq(e.ch, B.ch, "② 发到被拖的那一轨");
    eq(e.payload && e.payload.segIdx, B.segIdx, "② segIdx = 被拖的那条边界");
    eq(
        e.payload && e.payload.tS,
        ms(B.rawT),
        `② ★ Alt 关吸附:提交的 tS = 指针处裸时刻 ${ms(B.rawT)}s(吸附会是谷点 ${ms(B.V)}s)`,
    );
    check(!!e.payload && e.payload.tS !== ms(B.V), "② 反向:提交的 tS 不是谷点");
    eq(r.snapAttr, "0", "② 拖动中手柄不亮「已吸附」(data-snap=0)");
}

check(
    exceptions.length === 0,
    `全程零未捕获异常(实得 ${exceptions.length}:${exceptions.slice(0, 3).join(" | ")})`,
);
check(
    errors.length === 0,
    `全程零 console.error(实得 ${errors.length}:${errors.slice(0, 3).join(" | ")})`,
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
