// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Tab1 手动范围「按小节显示」的页面级冒烟([J147])
// =============================================================================
// 用户裁定 J147(「我们不能读取bpm来判断小节吗…别的插件不都可以吗」):手动范围读宿主的
// BPM / 拍号 / 拍位置换算小节,同时显示小节与 mm:ss,「−4 / +4」真挪 4 小节;读不到宿主速度时
// 按秒显示并明说。此前那两个钮恒挪 4 **秒**,而注释「小节为估算值,播放该区域后校准」常亮、
// 背后没有任何逻辑。
//
// 为什么必须页面级:换算本身在 node 侧有逐函数的判据(`smoke-host-tempo.mjs`),但这一卡的
// 缺陷形态是**接线**——速度字段有没有从 `scvb.playhead` 喂进模型、钮有没有按小节折秒、
// 注释行有没有跟着状态换词条和显隐。这些只有起真页面、点真按钮才看得见。
//
// 跑什么(预览会话,`?fixture=stereo-mixed` = 手动档 12–96 s;`play=0` 让播放头停在 42 s):
//   ① 默认宿主(120 BPM、4/4、恒速):换算行「小节 7.1 → 49.1」、非估算态、注释行隐藏;
//      点 +4 ⇒ 终点 96 → 104 s(= 4 小节 × 2 s),换算行随之到 53.1,桥面真收到 104;
//      点 −4 ⇒ 回到 96 s。
//   ② `tempo=none`(宿主不报速度):换算行隐藏,注释行说「按秒显示」;+4 ⇒ 终点 +4 s。
//   ③ 运行中宿主换速度(120 → 100):注释行换成「小节为估算值,播放该区域后校准」,换算行
//      进估算态,且按最近的 100 BPM 换算(6.1 → 41.1);+4 ⇒ 按 100 BPM 挪 9.6 s。
//   ④ 校准:把两个端点都放到播放头(42 s)0.25 s 之内 —— 播放头停着时宿主一直在报那一点的
//      拍位置,两个端点都算「播放过」⇒ 注释行隐藏、换算行回到非估算态。
//   ⑤ 运行中宿主换拍号(4/4 → 3/4):注释行换成「拍号有变化,小节号为估算值」。
//   每段零 console.error、零未捕获异常。
//
// 删除式(未提交,人工核过;读数见 PR 描述):
//   · app.js 删掉喂速度模型那一行 ⇒ ① 的换算行等不到(页面恒按秒)红;
//   · tab-master.js `nudgeRange` 的小节分支退回「恒挪 4 秒」⇒ ① 的 104 s 与 ③ 的 105.6 s 红;
//   · `renderRangeBars` 注释行恒显示 ⇒ ① / ④ 的「注释行隐藏」红;
//   · state-driver 不往下传 `tempo: parsed.tempo` ⇒ ② 整段红(页面仍按 120 BPM 显示小节)。
//
// 用法:node web-preview/tests/smoke-range-bars-page.mjs [仓库根绝对路径]
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
// 判据对词条真源,不在本文件手抄中文(手抄的那句会随 U17 审校漂走)。
import { T, format } from "../../web/shared/i18n.js";

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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-range-bars-"));
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

// 页面已经吃到首帧 state 的判据:RANGE 卡切到了「手动」档,起点输入框已被填成 fixture 的 12 s。
const READY = IN(`
    const card = gb("master-range");
    const start = gb("master-range-start-bars");
    return !!card && card.getAttribute("data-range") === "manual" &&
        !!start && start.value === "00:12.000";
`);

async function open(query) {
    newBucket(query);
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?${query}`,
    });
    const ok = await waitFor(READY);
    check(ok, `${query}:页面装载并吃到首帧 state(手动档 12–96 s)`);
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

// ---- 本套专用的探针与动作 -------------------------------------------------
const LANGS = ["zh", "en", "fr"];
const anyLang = (key, vals) => LANGS.map((l) => format(T[l][key], vals || {}));

const RANGE_PROBE = IN(`
    const bars = gb("master-range-bars");
    const note = gb("master-range-note");
    const s = gb("master-range-start-bars");
    const e = gb("master-range-end-bars");
    const sess = window.__SCVB_PREVIEW__;
    const r = sess && sess.ctl && sess.ctl.model && sess.ctl.model.snapshot
        ? sess.ctl.model.snapshot.global.range : null;
    return {
        barsShown: !!bars && !bars.hidden,
        barsText: bars ? bars.textContent : null,
        barsEst: bars ? bars.getAttribute("data-est") : null,
        noteShown: !!note && !note.hidden,
        noteKey: note ? note.getAttribute("data-t") : null,
        noteText: note ? note.textContent : null,
        start: s ? s.value : null,
        end: e ? e.value : null,
        bridgeEnd: r ? r.end_s : null,
    };
`);

const probe = () => evaluate(RANGE_PROBE);

async function clickGb(name) {
    return evaluate(
        IN(
            `const b = gb(${JSON.stringify(name)}); if (!b) return false; b.click(); return true;`,
        ),
    );
}

// 预览专用开关(不在桥面契约里,见 juce-bridge-mock 的 setHostTempo)。
async function setHostTempo(t) {
    return evaluate(`(() => {
        const s = window.__SCVB_PREVIEW__;
        if (!s || !s.ctl || !s.ctl.setHostTempo) return "no-hook";
        s.ctl.setHostTempo(${JSON.stringify(t)});
        return "ok";
    })()`);
}

// 两个输入框同一拍写好再触发 change(render 会按 state 回写未聚焦的输入框,分两拍写会被冲掉)。
async function setManualInputs(startTc, endTc) {
    return evaluate(
        IN(`const s = gb("master-range-start-bars");
            const e = gb("master-range-end-bars");
            if (!s || !e) return false;
            s.value = ${JSON.stringify(startTc)};
            e.value = ${JSON.stringify(endTc)};
            e.dispatchEvent(new w.Event("change"));
            return true;`),
    );
}

// 轮询到条件成立(不写死 sleep:mock 的状态回声是异步的,CLAUDE.md §10)。
async function waitProbe(pred, ms = 8000) {
    const t0 = Date.now();
    let last = null;
    while (Date.now() - t0 < ms) {
        last = await probe();
        if (last && pred(last)) return { ok: true, last };
        await sleep(120);
    }
    return { ok: false, last };
}

const barsIs = (x, y) => (p) =>
    anyLang("master.rangeBars", { x, y }).includes(p.barsText);

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
    log("=== ① 默认宿主 120 BPM · 4/4:小节换算 + ±4 真挪 4 小节 ===");
    await open("fixture=stereo-mixed&play=0");
    {
        const r = await waitProbe(barsIs("7.1", "49.1"));
        check(
            r.ok,
            `① 换算行显示「小节 7.1 → 49.1」(12 s / 96 s @120 BPM 4/4;实得 ${JSON.stringify(r.last)})`,
        );
        eq(
            r.last && r.last.barsEst,
            "0",
            "① 恒速:换算行是非估算态(data-est=0)",
        );
        eq(r.last && r.last.noteShown, false, "① 恒速:注释行隐藏");
    }
    check(await clickGb("master-range-step-plus4"), "① 点到 +4");
    {
        const r = await waitProbe(
            (p) =>
                p.end === "01:44.000" &&
                p.bridgeEnd === 104 &&
                barsIs("7.1", "53.1")(p),
        );
        check(
            r.ok,
            `① +4 ⇒ 终点 96 → 104 s(4 小节 × 2 s)、桥面收到 104、换算行到 53.1(实得 ${JSON.stringify(r.last)})`,
        );
    }
    check(await clickGb("master-range-step-minus4"), "① 点到 −4");
    {
        const r = await waitProbe(
            (p) => p.end === "01:36.000" && p.bridgeEnd === 96,
        );
        check(r.ok, `① −4 ⇒ 终点回到 96 s(实得 ${JSON.stringify(r.last)})`);
    }
    assertClean("①");

    // =========================================================================
    log("=== ② 宿主不报速度:按秒显示并明说,±4 挪 4 秒 ===");
    await open("fixture=stereo-mixed&play=0&tempo=none");
    {
        const r = await waitProbe(
            (p) => p.noteShown && p.noteKey === "master.rangeSecondsNote",
        );
        check(
            r.ok,
            `② 注释行 = master.rangeSecondsNote(实得 ${JSON.stringify(r.last)})`,
        );
        check(
            !!r.last &&
                anyLang("master.rangeSecondsNote").includes(r.last.noteText),
            "② 注释行文字逐字等于词条",
        );
        eq(r.last && r.last.barsShown, false, "② 换算行隐藏(没有小节可言)");
    }
    check(await clickGb("master-range-step-plus4"), "② 点到 +4");
    {
        const r = await waitProbe(
            (p) => p.end === "01:40.000" && p.bridgeEnd === 100,
        );
        check(r.ok, `② +4 ⇒ 终点 96 → 100 s(实得 ${JSON.stringify(r.last)})`);
    }
    assertClean("②");

    // =========================================================================
    log("=== ③ 运行中宿主换速度 120 → 100:估算态 + 按最近 BPM 换算 ===");
    await open("fixture=stereo-mixed&play=0");
    {
        const r0 = await waitProbe(barsIs("7.1", "49.1"));
        check(r0.ok, "③ 前提:先以 120 BPM 显示小节");
    }
    eq(
        await setHostTempo({ bpm: 100, num: 4, den: 4 }),
        "ok",
        "③ 预览会话认得 setHostTempo",
    );
    {
        // 播放头停在 42 s:拍位置 = 42 × 100 / 60 = 70;12 s ⇒ 20 拍 = 6.1;96 s ⇒ 160 拍 = 41.1。
        const r = await waitProbe(
            (p) =>
                p.noteShown &&
                p.noteKey === "master.barsEstimateNote" &&
                barsIs("6.1", "41.1")(p),
        );
        check(
            r.ok,
            `③ 注释行 = master.barsEstimateNote,换算行按 100 BPM 为 6.1 → 41.1(实得 ${JSON.stringify(r.last)})`,
        );
        eq(r.last && r.last.barsEst, "1", "③ 换算行进估算态(data-est=1)");
        check(
            !!r.last &&
                anyLang("master.barsEstimateNote").includes(r.last.noteText),
            "③ 注释行文字逐字等于词条",
        );
    }
    check(await clickGb("master-range-step-plus4"), "③ 点到 +4");
    {
        const r = await waitProbe(
            (p) => p.end === "01:45.600" && p.bridgeEnd === 105.6,
        );
        check(
            r.ok,
            `③ +4 ⇒ 按最近的 100 BPM 挪 9.6 s(96 → 105.6;实得 ${JSON.stringify(r.last)})`,
        );
    }

    // =========================================================================
    log(
        "=== ④ 两个端点都落在播放过的地方(播放头 42 s 附近)⇒ 校准,注释行隐藏 ===",
    );
    check(
        await setManualInputs("00:41.900", "00:42.100"),
        "④ 两个端点改到 41.9 / 42.1 s",
    );
    {
        const r = await waitProbe(
            (p) => p.bridgeEnd === 42.1 && !p.noteShown && p.barsEst === "0",
        );
        check(
            r.ok,
            `④ 两端都在锚点 0.25 s 内 ⇒ 注释行隐藏、换算行非估算态(实得 ${JSON.stringify(r.last)})`,
        );
    }
    assertClean("③④");

    // =========================================================================
    log("=== ⑤ 运行中宿主换拍号 4/4 → 3/4:拍号变化提示(不说「播放后校准」)===");
    eq(
        await setHostTempo({ bpm: 100, num: 3, den: 4 }),
        "ok",
        "⑤ 预览会话认得 setHostTempo",
    );
    {
        const r = await waitProbe(
            (p) => p.noteShown && p.noteKey === "master.barsMeterNote",
        );
        check(
            r.ok,
            `⑤ 注释行 = master.barsMeterNote(即使两端都在播放过的地方;实得 ${JSON.stringify(r.last)})`,
        );
        eq(r.last && r.last.barsEst, "1", "⑤ 换算行进估算态");
        check(
            !!r.last &&
                anyLang("master.barsMeterNote").includes(r.last.noteText),
            "⑤ 注释行文字逐字等于词条",
        );
    }
    assertClean("⑤");
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
console.log("\n✅ Tab1 手动范围按小节显示(J147)页面级冒烟全绿");
process.exit(0);
