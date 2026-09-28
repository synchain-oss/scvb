// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Input 优先级滑杆被拒时回滚乐观值的页面级冒烟(SL-20/21)
// =============================================================================
// 病灶:`wirePriority()` 的 change 处理只在回执是 `{queued:false, reason:"ringFull"}` 时回滚
// `priorityLocal`;闸门早退(unassigned / offline)、回执 null(桥调用抛错)、其余 reason
// (outputOffline / unassigned / busy)都不回滚 ⇒ 滑杆与数字停在一个从未送达的值上,
// 直到下一次 scvb.config 回执(而被拒的请求根本不会触发回执)。
// 必须页面级:乐观值、闸门、回执三者只在真 app.js 的事件接线里交汇,node 侧纯函数碰不到。
//
// 跑什么(每格:拖到 X ⇒ 先确认乐观值已上屏,再松手 ⇒ 必须回到基线值):
//   ① 默认场景,回执拒绝的五类:ringFull(对照,旧代码也回滚)/ outputOffline / unassigned /
//      busy / 回执 null(mock 抛错,app.js 的 call() 吞掉返回 null);
//   ② 旧请求被拒时用户已经又拖到了新值 ⇒ 不得把正在拖的值打回去(rollbackPriority 的守卫);
//   ③ 成功对照:用 mock 原实现({queued:true} + 120ms 后 scvb.config 回执)⇒ 停在新值;
//   ④ 闸门早退两类:?scenario=no-output(offline)、?fixture=empty(unassigned)——
//      并断 remoteSetPriority 一次都没被调;
//   每段零 console.error、零未捕获异常。
//
// 删除式(未提交,人工核过,见 PR 描述):
//   · 把回执判断改回只认 ringFull ⇒ ① 除 ringFull 外四格红;
//   · 删掉闸门分支里的 rollbackPriority(next) ⇒ ④ 两格红;
//   · 删掉 rollbackPriority 里的守卫 ⇒ ② 红。
//
// 用法:node web-preview/tests/smoke-input-priority-page.mjs [仓库根绝对路径]
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-input-priority-"));
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
    const card = (ch) => q('[data-gb="input.channels.card"][data-ch="' + ch + '"]');
    ${js}
})()`;

// 页面已经渲染过一轮的判据:16 张通道卡(0..15)已由 buildChannelGrid 建出来。
const READY = IN(`
    const cards = d.querySelectorAll('[data-gb="input.channels.card"]');
    return cards.length === 16;
`);

async function open(query) {
    newBucket(query);
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/input.html?${query}`,
    });
    const ok = await waitFor(READY);
    check(ok, `${query}:页面装载并建出通道网格`);
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

    // ---- 页内小工具 -----------------------------------------------------------
    // 数字读 `input.priority.stepper.val`(renderPriority 写的就是 currentPriority())。
    const VAL = IN(`const v = gb("input.priority.stepper.val");
        return v ? v.textContent.trim() : null;`);
    const readVal = async () => Number(await evaluate(VAL));
    const valIs = (n) =>
        IN(`const v = gb("input.priority.stepper.val");
            return !!v && v.textContent.trim() === ${JSON.stringify(String(n))};`);
    // 换掉 mock 的 remoteSetPriority(bridge.js 在调用时才取 mock[name],换掉即生效)。
    // 原实现存进 __prioOrig,计数进 __prioCalls。
    async function mockPriority(body) {
        return evaluate(
            IN(`const m = w.__SCVB_MOCK__;
                if (!m) return false;
                if (!w.__prioOrig) w.__prioOrig = m.remoteSetPriority;
                w.__prioCalls = 0;
                m.remoteSetPriority = function (n) {
                    w.__prioCalls++;
                    ${body}
                };
                return true;`),
        );
    }
    // 拖到 v:与真拖动同形,改 value 再派发 input(不走原生焦点,滑杆 disabled 也照样派发)。
    const drag = (v) =>
        evaluate(
            IN(`const s = gb("input.priority.slider");
                if (!s) return false;
                s.value = ${JSON.stringify(String(v))};
                s.dispatchEvent(new w.Event("input", { bubbles: true }));
                return true;`),
        );
    const release = () =>
        evaluate(
            IN(`const s = gb("input.priority.slider");
                if (!s) return false;
                s.dispatchEvent(new w.Event("change", { bubbles: true }));
                return true;`),
        );

    // 一格:拖到 x ⇒ 乐观值上屏 ⇒ 松手 ⇒ 回到 base。
    async function rejectCell(label, x, base) {
        check(await drag(x), `${label}:拖动派发成功`);
        check(
            await waitFor(valIs(x), 3000),
            `${label}:乐观值 ${x} 已上屏(前提,否则回滚断言恒真)`,
        );
        check(await release(), `${label}:松手派发成功`);
        const back = await waitFor(valIs(base), 3000);
        check(
            back,
            `${label}:被拒后回滚到基线 ${base}(实得 ${await readVal()})`,
        );
    }

    // =========================================================================
    log("=== ① 默认场景:回执拒绝五类都要回滚 ===");
    await open("");
    // 等首帧 scvb.config / conn 到齐:滑杆可用 = 闸门放行(channel 已分配 + Output 在线)。
    check(
        await waitFor(
            IN(`const s = gb("input.priority.slider");
                return !!s && s.disabled === false;`),
            6000,
        ),
        "① 滑杆可用(已分配 + Output 在线)",
    );
    const base1 = await readVal();
    check(Number.isInteger(base1), `① 读到基线值(实得 ${base1})`);
    const x1 = base1 === 7 ? 3 : 7;
    const cells = [
        ["ringFull(对照)", `return { queued: false, reason: "ringFull" };`],
        ["outputOffline", `return { queued: false, reason: "outputOffline" };`],
        ["unassigned", `return { queued: false, reason: "unassigned" };`],
        ["busy", `return { queued: false, reason: "busy" };`],
        [
            "回执 null(桥调用抛错)",
            `throw new Error("smoke: simulated bridge failure");`,
        ],
    ];
    for (const [name, body] of cells) {
        check(await mockPriority(body), `① ${name}:mock 已替换`);
        await rejectCell(`① ${name}`, x1, base1);
        check(
            (await evaluate(IN(`return w.__prioCalls;`))) === 1,
            `① ${name}:remoteSetPriority 恰好被调一次(确认走的是回执路径,不是闸门)`,
        );
    }
    assertClean("① 回执拒绝");

    // =========================================================================
    log("=== ② 旧请求被拒时已拖到新值:不得打回正在拖的值 ===");
    check(
        await mockPriority(
            `return new Promise((ok) => { w.__prioResolve = ok; });`,
        ),
        "② mock 已替换为挂起回执",
    );
    const y1 = x1 === 7 ? 8 : 6;
    check(await drag(x1), "② 拖到 x");
    check(await waitFor(valIs(x1), 3000), "② 乐观值 x 已上屏");
    check(await release(), "② 松手(回执挂起)");
    check(await drag(y1), "② 回执回来前又拖到 y");
    check(await waitFor(valIs(y1), 3000), "② 乐观值 y 已上屏");
    check(
        await evaluate(
            IN(`if (typeof w.__prioResolve !== "function") return false;
                w.__prioResolve({ queued: false, reason: "busy" });
                return true;`),
        ),
        "② 放行第一次请求的拒绝回执",
    );
    await sleep(400);
    eq(await readVal(), y1, "② 旧请求被拒不回滚正在拖的新值");
    // 收尾:y 也被拒 ⇒ 回到基线,给③一个干净起点。
    check(
        await mockPriority(`return { queued: false, reason: "ringFull" };`),
        "② 收尾 mock",
    );
    check(await release(), "② 收尾松手");
    check(await waitFor(valIs(base1), 3000), "② 收尾:回到基线");
    assertClean("② 守卫");

    // =========================================================================
    log("=== ③ 成功对照:{queued:true} 不回滚,停在新值 ===");
    check(
        await evaluate(
            IN(`const m = w.__SCVB_MOCK__;
                if (!m || !w.__prioOrig) return false;
                m.remoteSetPriority = w.__prioOrig;
                return true;`),
        ),
        "③ 还原 mock 原实现",
    );
    check(await drag(x1), "③ 拖到 x");
    check(await waitFor(valIs(x1), 3000), "③ 乐观值 x 已上屏");
    check(await release(), "③ 松手");
    await sleep(600); // mock 120ms 后回 scvb.config;多等一段确认没被回滚打回
    eq(await readVal(), x1, "③ 送达后停在新值(不被回滚)");
    assertClean("③ 成功对照");

    // =========================================================================
    log("=== ④ 闸门早退也要回滚 ===");
    for (const [q, name] of [
        ["scenario=no-output", "offline"],
        ["fixture=empty", "unassigned"],
    ]) {
        await open(q);
        check(
            await waitFor(
                IN(`const s = gb("input.priority.slider");
                    return !!s && s.disabled === true;`),
                6000,
            ),
            `④ ${name}:滑杆已被闸门禁用(前提)`,
        );
        const b = await readVal();
        check(Number.isInteger(b), `④ ${name}:读到基线值(实得 ${b})`);
        check(
            await mockPriority(`return { queued: true };`),
            `④ ${name}:装上计数 mock`,
        );
        await rejectCell(`④ ${name}`, b === 7 ? 3 : 7, b);
        check(
            (await evaluate(IN(`return w.__prioCalls;`))) === 0,
            `④ ${name}:闸门拦下,remoteSetPriority 一次都没调`,
        );
        assertClean(`④ ${name}`);
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
console.log("\n✅ Input 优先级被拒回滚(SL-20/21)页面级冒烟全绿");
process.exit(0);
