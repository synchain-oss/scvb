// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 「重开面板回到上次 tab」的页面级冒烟([J148];契约 §1.31)
// =============================================================================
// 契约 §1.31:`setActiveTab` 写 state `ui.active_tab`,**重开面板恢复上次 tab**。
// [J148] 之前 C++ 只把它放在运行期,存盘重开工程一律回 Tab1;本卡把它落进 PRMS。
// 落盘 / 载入那一半由 scvb_host_tests「HOST J148」在真 Processor 上断;native 桥与快照的
// 两跳接线由 smoke-tab1-interactions 的 [J148] 钉子守。**本套守剩下那一段:页面这一侧** ——
// 宿主手里的 `ui.active_tab` 是 Tab3,编辑器打开时屏幕上到底是不是 Tab3。
//
// 为什么必须页面级:这一段的全部内容是「首帧快照 → syncUiFromState → activateTab →
// #content[data-tab] → CSS 显隐」这条链,外加一条**反方向**的风险 —— 页面装载时若把自己的
// 初始 tab(`master`)经 setActiveTab 往上写,会在宿主回快照**之前**把工程里恢复出来的值冲掉。
// 那一步发生在模块求值期,node 侧的 bridge/mock 断言碰不到。
//
// 怎么造「宿主手里是 Tab3」(全程不调页面内部函数):
//   宿主在预览里就是壳页的 driver 会话。用 `Page.addScriptToEvaluateOnNewDocument` 在壳页
//   文档创建之前装一个 `window.__SCVB_PREVIEW__` 的 setter —— shell.js 建好会话后第一件事
//   就是赋这个值,那一刻 iframe 还没建、页面还没求值。setter 里把会话的
//   `ctl.model.snapshot.ui.active_tab` 置成要播种的值(= 宿主刚载入一份存着该 tab 的工程),
//   并给会话的 `setActiveTab` 套一层记账(页面往上写过什么一目了然)。
//   真源页面一个字节不碰;播种只作用在壳页(`window.top === window`)。
//
// 跑什么:
//   ① 对照:不播种 ⇒ 屏幕在 Tab1(证明 ② 的 Tab3 不是页面默认值蒙出来的);
//   ② 播种 wave ⇒ 首帧后 `#content[data-tab]` = wave、tab 条上**只有** wave 是 aria-selected、
//      **只有** Tab3 的面板在显示;装载全程页面**零次**上行 setActiveTab(回推不上行,
//      初始 tab 也不上行 —— 后者一旦上行就会冲掉宿主恢复的值);
//   ③ 同一页上真点 tab 条的 Tracks ⇒ 宿主手里的值变成 tracks(= 下次存工程写进 PRMS 的那个),
//      且恰好上行一次;
//   ④「关窗再开」:拿 ③ 里宿主记下的值播种下一轮装载 ⇒ 屏幕回到 Tracks。
//   每段零未捕获异常、零 console.error。
//
// 删除式(未提交,人工核过;记录见 PR 描述):
//   · 删 web/output/app.js syncUiFromState 里那句 `activateTab(ui.active_tab, { push: false })`
//     ⇒ ② ④ 的屏幕读数转红(停在 master),上行计数仍绿;
//   · 把 app.js 模块求值期那句 `activateTab("master", { push: false })` 的 push 改成 true
//     ⇒ ② 三条全红(上行了 master、宿主值被冲掉、屏幕停在 master),③ 的上行计数带红;
//   · 把 syncUiFromState 那句的 push 改成 true ⇒ ②③④ 的上行计数转红,屏幕读数全绿
//     (回推上行了同一个值,画面看不出来 —— 这正是要单独计数的理由)。
//
// 用法:node web-preview/tests/smoke-active-tab-restore-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;2 = 环境里没有 Chrome/Edge;
//   3 = 浏览器在但这一次没起来/没连上(gates 打 [FLAKY-SKIP])。
//
// CDP 那几十行与 smoke-input-conflict-page.mjs 同源(node 内置 fetch + WebSocket,零依赖 ——
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-active-tab-"));
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

// 首帧已渲染的判据(与 smoke-output-stale-page 同一口径):泳道标签的文字要等 state 到了、
// render 跑过一轮才会有。app.js 的 bootInner 在 render() **之前**调 syncUiFromState,所以
// READY 成立时 tab 的恢复已经做完 —— 下面仍用 waitFor 读 tab,只是为了红的时候带回实得值。
const READY = IN(`
    const lane = gb("wave-lane-2-label");
    return !!(lane && lane.textContent && lane.textContent.length > 0);
`);

// 用户看得见的三件:#content 上的 tab、tab 条上谁是选中态、哪个面板真的在显示。
const TAB_PROBE = IN(`
    const c = q("#content");
    const selected = Array.from(d.querySelectorAll("[data-tab-btn]"))
        .filter((b) => b.getAttribute("aria-selected") === "true")
        .map((b) => b.getAttribute("data-tab-btn"));
    const shown = Array.from(d.querySelectorAll("#content > section[data-tab-panel]"))
        .filter((s) => w.getComputedStyle(s).display !== "none")
        .map((s) => s.getAttribute("data-tab-panel"));
    return { tab: c ? c.getAttribute("data-tab") : null, selected, shown };
`);

// 宿主侧(壳页 driver 会话)手里的 ui.active_tab —— 真机上它就是下次存工程写进 PRMS 的那个值。
const HOST_TAB = `(() => {
    const s = window.__SCVB_PREVIEW__;
    return s && s.ctl && s.ctl.model && s.ctl.model.snapshot && s.ctl.model.snapshot.ui
        ? s.ctl.model.snapshot.ui.active_tab
        : null;
})()`;
// 页面往上写过的 setActiveTab 实参(播种脚本装的记账;未播种时为 null)。
const UP_CALLS = `(() => (window.__j148 ? window.__j148.calls.slice() : null))()`;

// 播种脚本:只在壳页生效。shell.js 建好 driver 会话后立刻 `window.__SCVB_PREVIEW__ = session`,
// 那时 iframe 还没建 —— 在 setter 里改会话的快照,等价于「宿主先载入了一份存着该 tab 的工程,
// 编辑器随后才打开」。同时给会话的 setActiveTab 套记账:页面经桥调它时走的是
// `mock[name](...)`,而注入进 iframe 的 mock 以会话的 mock 为原型,套在这里就接得住。
const SEED_SCRIPT = (tab) => `(() => {
    if (window.top !== window) return;
    const TAB = ${JSON.stringify(tab)};
    window.__j148 = { seeded: null, calls: [] };
    let session;
    Object.defineProperty(window, "__SCVB_PREVIEW__", {
        configurable: true,
        enumerable: true,
        get() { return session; },
        set(v) {
            session = v;
            try {
                v.ctl.model.snapshot.ui.active_tab = TAB;
                const orig = v.mock.setActiveTab;
                v.mock.setActiveTab = function (...args) {
                    window.__j148.calls.push(args[0]);
                    return orig.apply(this, args);
                };
                window.__j148.seeded = v.ctl.model.snapshot.ui.active_tab;
            } catch (e) {
                window.__j148.seeded = "ERR " + (e && e.message);
            }
        },
    });
})();`;

async function open(label, seedTab) {
    newBucket(label);
    let scriptId = null;
    if (seedTab) {
        const r = await cdp.send("Page.addScriptToEvaluateOnNewDocument", {
            source: SEED_SCRIPT(seedTab),
        });
        scriptId = r && r.identifier;
    }
    try {
        await cdp.send("Page.navigate", {
            url: `${base}/web-preview/output.html`,
        });
        check(await waitFor(READY), `${label}:页面装载并完成首帧渲染`);
    } finally {
        // 播种只对这一次装载生效,别漏到下一段(对照段必须是干净的默认装载)。
        if (scriptId)
            await cdp.send("Page.removeScriptToEvaluateOnNewDocument", {
                identifier: scriptId,
            });
    }
    if (seedTab) {
        eq(
            await evaluate(
                `(() => (window.__j148 ? window.__j148.seeded : "no-seed-script"))()`,
            ),
            seedTab,
            `${label}:播种生效(宿主会话的 ui.active_tab 在页面装载前已是 ${seedTab})`,
        );
    }
}

async function expectTab(label, want) {
    await waitFor(
        IN(
            `const c = q("#content"); return !!c && c.getAttribute("data-tab") === ${JSON.stringify(want)};`,
        ),
        6000,
    );
    eq(
        await evaluate(TAB_PROBE),
        { tab: want, selected: [want], shown: [want] },
        `${label}:屏幕停在 ${want}(#content[data-tab] / tab 条选中态 / 面板显示三者一致)`,
    );
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
    log("=== ① 对照:不播种 ⇒ 默认装载停在 Tab1 ===");
    await open("① 对照", null);
    await expectTab("① 对照", "master");
    assertClean("① 对照");

    // =========================================================================
    log(
        "=== ② 宿主手里是 Tab3(wave)⇒ 编辑器打开就在 Tab3,且装载全程不上行 ===",
    );
    await open("② 播种 wave", "wave");
    await expectTab("② 播种 wave", "wave");
    // 页面装载 + 恢复的全程零次上行:恢复走 push:false,初始 tab 也走 push:false。
    // 后者要是上行了,宿主手里的 wave 会在页面拿到首帧快照**之前**被冲成 master。
    eq(
        await evaluate(UP_CALLS),
        [],
        "② 装载全程页面零次上行 setActiveTab(初始 tab / 恢复都不许回写宿主)",
    );
    eq(
        await evaluate(HOST_TAB),
        "wave",
        "② 装载之后宿主手里仍是 wave(没被页面冲掉)",
    );
    assertClean("② 播种 wave");

    // =========================================================================
    log(
        "=== ③ 真点 tab 条的 Tracks ⇒ 宿主手里的值跟着变(下次存工程写进去的就是它)===",
    );
    check(
        await evaluate(
            IN(
                `const b = q('[data-tab-btn="tracks"]'); if (!b) return false; b.click(); return true;`,
            ),
        ),
        "③ 点到了 tab 条上的 Tracks",
    );
    check(
        await waitFor(`${HOST_TAB} === "tracks"`, 6000),
        "③ 宿主手里的 ui.active_tab 变成 tracks",
    );
    eq(
        await evaluate(UP_CALLS),
        ["tracks"],
        "③ 恰好上行一次 setActiveTab(tracks)",
    );
    await expectTab("③ 点 Tracks", "tracks");
    const hostTabAfterClick = await evaluate(HOST_TAB);
    assertClean("③ 点 Tracks");

    // =========================================================================
    log("=== ④ 关窗再开:用 ③ 里宿主记下的值装载 ⇒ 回到 Tracks ===");
    await open("④ 重开", hostTabAfterClick || "(③ 没取到宿主值)");
    await expectTab("④ 重开", "tracks");
    eq(await evaluate(UP_CALLS), [], "④ 重开装载全程零次上行");
    assertClean("④ 重开");
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
console.log("\n✅ 重开面板回到上次 tab([J148] / §1.31)页面级冒烟全绿");
process.exit(0);
