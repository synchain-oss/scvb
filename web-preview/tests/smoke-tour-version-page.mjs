// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 导览期间设置页显示的是真插件的版本号(rc1 切版 PR)页面级冒烟
// =============================================================================
// 缺陷:导览期间页面渲染的是 demo 仓(`web/output/tour.js` 的 `buildDemoStore`),它的快照
// 整份来自 mock(`web/shared/mock-data.js`),`version` 也是 mock 固定值 `0.1.0` / abi 1 ——
// 于是真插件(0.9.0)一进导览,设置页版本行与页脚就显示「v0.1.0 · abi 1」。修法:demo 仓只从
// 真快照(app.js 的 `store.snapshot`)取 `version`,真快照给不出合法 version 时干脆不带。
//
// 为什么必须页面级:修法有三个落点 —— buildDemoStore 用第二个参数、createTour 在 start() 里
// 把真快照传进去、app.js 把 `getRealSnapshot` 接给 createTour。smoke-tour.mjs 的 ③ 只钉第一处
// (纯函数),后两处是「有没有传下去」,纯函数断言碰不到。
//
// 怎么造「真插件是 0.9.0」(全程不调页面内部函数):与 smoke-active-tab-restore-page.mjs 同一手法 ——
// `Page.addScriptToEvaluateOnNewDocument` 在壳页文档创建前装一个 `window.__SCVB_PREVIEW__` 的 setter,
// shell.js 建好 driver 会话后第一件事就是赋这个值,那时 iframe 还没建;setter 里把会话的
// `ctl.model.snapshot.version` 换成要播种的值(= native 下发的 §1.1 `version`)。abi 故意播 2:
// demo 快照的 abi 是 1,播 1 就分不出 abi 取自哪一份。
//
// 跑什么(都用 `?fixture=empty`:真数据 0 轨连接、询问步自动弹出,见 smoke-tour-demo-page.mjs):
//   ① 播种 {plugin:"0.9.0", abi:2}:装载后页脚是 v0.9.0(播种到了页面)→ 点「开始」进导览,
//      header-conn-count 变 15/15(确实在渲染 demo 仓)→ 方向键翻到第一个设置页步骤 ⇒
//      设置页版本行 = 「v0.9.0 · abi 2」、页脚仍是 v0.9.0 → Esc 结束 ⇒ 页脚仍是 v0.9.0、
//      header-conn-count 回到 0/15;
//   ② 播种 version = null(真快照给不出版本):同一路径进导览翻到设置页 ⇒ 版本行为空,
//      **不是**「v0.1.0 · abi 1」(不回落到 mock 值);
//   每段零未捕获异常、零 console.error。
//
// 删除式(未提交,人工核过;记录见 PR 描述):
//   · tour.js start() 里 `buildDemoStore(getT, getRealSnapshot())` 改回 `buildDemoStore(getT)`
//     ⇒ ① 的版本行转红(实得空串);
//   · 删 app.js createTour 调用里的 `getRealSnapshot: () => store.snapshot,` ⇒ ① 的版本行转红;
//   · buildDemoStore 里删掉给 `snap.version` 赋值那一句 ⇒ ① 的版本行转红(smoke-tour ③ 同红);
//   · buildDemoStore 退回改前形态(快照整份用 demo 的)⇒ ① ② 的版本行都转红(实得 v0.1.0 · abi 1)。
//
// 用法:node web-preview/tests/smoke-tour-version-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;2 = 环境里没有 Chrome/Edge;
//   3 = 浏览器在但这一次没起来/没连上(gates 打 [FLAKY-SKIP])。
//
// CDP 那几十行与 smoke-active-tab-restore-page.mjs 同源(node 内置 fetch + WebSocket,零依赖 ——
// 仓库红线是不引 puppeteer)。同样不抽公共模块,理由见 smoke-seg-restore-page.mjs 头注。
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-tour-version-"));
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

// 真源文档在 iframe 里(壳页只有工具条);页面侧的选择器一律走它。
const IN = (js) => `(() => {
    const f = document.querySelector("iframe");
    const w = f && f.contentWindow;
    const d = f && f.contentDocument;
    if (!w || !d) return null;
    const q = (s) => d.querySelector(s);
    const gb = (n) => q('[data-gb="' + n + '"]');
    ${js}
})()`;

// 第一个设置页步骤的步号(从步骤表现算,不写死 36:增删步时这里跟着走)。
const { TOUR_STEPS } = await import(
    pathToFileURL(join(ROOT_ABS, "web", "output", "tour.js")).href
);
const FIRST_SETTINGS_STEP =
    TOUR_STEPS.findIndex((s) => s && s.tab === "settings") + 1;

// 首帧判据(fixture=empty):真实数据已经渲染过一轮 —— header-conn-count 落定 "0/15"。
const READY = IN(`
    const n = gb("header-conn-count");
    return !!(n && n.textContent === "0/15");
`);
const CONN_COUNT = IN(
    `const n = gb("header-conn-count"); return n ? n.textContent : null;`,
);
const FOOTER_VERSION = IN(
    `const n = gb("footer-version"); return n ? n.textContent.trim() : null;`,
);
const UNRENDERED = "__settings_not_rendered__";
const SETTINGS_VERSION = IN(
    `const n = gb("settings-version-value"); return n ? n.textContent : null;`,
);
const OVERLAY_HIDDEN = IN(
    `const o = d.querySelector("[data-tour-overlay]"); return o ? o.hidden : null;`,
);
const CONTENT_TAB = IN(
    `const c = q("#content"); return c ? c.getAttribute("data-tab") : null;`,
);
const KEY = (key) =>
    IN(`d.dispatchEvent(new w.KeyboardEvent("keydown", {
            key: ${JSON.stringify(key)},
            bubbles: true,
            cancelable: true,
        }));
        return true;`);

// 播种脚本:只在壳页生效。`version === null` ⇒ 删掉会话快照里的 version(真快照给不出版本)。
const SEED_SCRIPT = (version) => `(() => {
    if (window.top !== window) return;
    const VERSION = ${JSON.stringify(version)};
    window.__tourVer = { seeded: "no-setter-call" };
    let session;
    Object.defineProperty(window, "__SCVB_PREVIEW__", {
        configurable: true,
        enumerable: true,
        get() { return session; },
        set(v) {
            session = v;
            try {
                if (VERSION === null) delete v.ctl.model.snapshot.version;
                else v.ctl.model.snapshot.version = VERSION;
                const got = v.ctl.model.snapshot.version;
                window.__tourVer.seeded = got === undefined ? null : got;
            } catch (e) {
                window.__tourVer.seeded = "ERR " + (e && e.message);
            }
        },
    });
})();`;

async function open(label, version) {
    newBucket(label);
    const r = await cdp.send("Page.addScriptToEvaluateOnNewDocument", {
        source: SEED_SCRIPT(version),
    });
    const scriptId = r && r.identifier;
    try {
        await cdp.send("Page.navigate", {
            url: `${base}/web-preview/output.html?fixture=empty`,
        });
        check(
            await waitFor(READY, 20000),
            `${label}:页面装载并渲染出真实数据(header-conn-count=0/15)`,
        );
    } finally {
        // 播种只对这一次装载生效,别漏到下一段。
        if (scriptId)
            await cdp.send("Page.removeScriptToEvaluateOnNewDocument", {
                identifier: scriptId,
            });
    }
    eq(
        await evaluate(
            `(() => (window.__tourVer ? window.__tourVer.seeded : "no-seed-script"))()`,
        ),
        version,
        `${label}:播种生效(会话快照的 version 在页面装载前已换好)`,
    );
}

// 点「开始」进导览 → 确认在渲染 demo 仓 → 方向键翻到第一个设置页步骤。
async function tourToSettings(label) {
    check(
        await waitFor(
            IN(`const o = gb("tour-ask"); return !!o && o.hidden === false;`),
            8000,
        ),
        `${label}:询问步自动弹出`,
    );
    check(
        await evaluate(
            IN(
                `const b = gb("tour-ask-start"); if (!b) return false; b.click(); return true;`,
            ),
        ),
        `${label}:点到了「开始」`,
    );
    check(
        await waitFor(`${OVERLAY_HIDDEN} === false`, 6000),
        `${label}:导览蒙版上屏`,
    );
    // 这一位证明下面读到的是 demo 仓的渲染,而不是真 store 的(真数据是 0/15)。
    check(
        await waitFor(`${CONN_COUNT} === "15/15"`, 5000),
        `${label}:导览期间渲染的是 demo 仓(header-conn-count=15/15)`,
    );
    check(
        FIRST_SETTINGS_STEP > 1,
        `${label}:步骤表里有设置页步骤(实得第 ${FIRST_SETTINGS_STEP} 步)`,
    );
    // 版本行在设置页渲染之前就是空串 ——「期望为空」的那一格(②)不先写个哨兵的话,
    // 设置页还没渲染就会读到空串判过(空转)。只有当前 tab 才渲染,此刻在 master 上,
    // 写进去的哨兵只会被设置页那次 render 覆盖。
    check(
        await evaluate(
            IN(`const n = gb("settings-version-value");
                if (!n) return false;
                n.textContent = ${JSON.stringify(UNRENDERED)};
                return true;`),
        ),
        `${label}:设置页版本行先写入哨兵(区分「渲染成空」与「还没渲染」)`,
    );
    for (let i = 1; i < FIRST_SETTINGS_STEP; i++) {
        await evaluate(KEY("ArrowRight"));
    }
    check(
        await waitFor(`${CONTENT_TAB} === "settings"`, 6000),
        `${label}:方向键翻到第 ${FIRST_SETTINGS_STEP} 步,页面切到设置页`,
    );
    check(
        (await evaluate(OVERLAY_HIDDEN)) === false,
        `${label}:翻页后仍在导览中`,
    );
}

async function expectSettingsVersion(label, want) {
    check(
        await waitFor(
            `${SETTINGS_VERSION} !== ${JSON.stringify(UNRENDERED)}`,
            5000,
        ),
        `${label}:设置页渲染过一次(哨兵被覆盖)`,
    );
    await waitFor(`${SETTINGS_VERSION} === ${JSON.stringify(want)}`, 3000);
    eq(await evaluate(SETTINGS_VERSION), want, `${label}:设置页版本行`);
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
    log(
        "=== ① 真插件是 0.9.0 / abi 2 ⇒ 导览里的设置页与页脚显示 0.9.0 / abi 2 ===",
    );
    const REAL = { plugin: "0.9.0", abi: 2 };
    await open("①", REAL);
    check(
        await waitFor(`${FOOTER_VERSION} === "v0.9.0"`, 5000),
        "① 导览前页脚是 v0.9.0(播种真的到了页面)",
    );
    await tourToSettings("①");
    await expectSettingsVersion("① 导览中", "v0.9.0 · abi 2");
    eq(await evaluate(FOOTER_VERSION), "v0.9.0", "① 导览中页脚仍是 v0.9.0");
    await evaluate(KEY("Escape"));
    check(await waitFor(`${OVERLAY_HIDDEN} === true`, 6000), "① Esc 结束导览");
    check(
        await waitFor(`${CONN_COUNT} === "0/15"`, 5000),
        "① 导览结束后切回真实 store(header-conn-count=0/15)",
    );
    eq(await evaluate(FOOTER_VERSION), "v0.9.0", "① 导览结束后页脚仍是 v0.9.0");
    assertClean("①");

    // =========================================================================
    log(
        "=== ② 真快照给不出版本 ⇒ 导览里的设置页版本行留空,不回落到 mock 的 v0.1.0 ===",
    );
    await open("②", null);
    await tourToSettings("②");
    await expectSettingsVersion("② 导览中", "");
    await evaluate(KEY("Escape"));
    check(await waitFor(`${OVERLAY_HIDDEN} === true`, 6000), "② Esc 结束导览");
    assertClean("②");
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
console.log("\n✅ 导览期间设置页显示真插件版本号 页面级冒烟全绿");
process.exit(0);
