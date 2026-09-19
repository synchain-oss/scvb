// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Input 通道冲突反馈的页面级冒烟(SL-19)
// =============================================================================
// 为什么必须页面级:病灶是 `claimChannel()` 里拿一个复合选择器字符串
// `'input.channels.card[data-ch="' + ch + '"]'` 当 `$()` 的实参传进去,套进 `$()`
// 自己的模板后变成 `[data-gb="input.channels.card[data-ch="5"]"]` —— 引号嵌套的
// 非法 CSS,`querySelector` 直接抛 `SyntaxError`。这条异常**没有任何 try/catch 接住**,
// 而且抛在 `shake(card)` / `showOccupiedToast(...)` / `render()` 三句**之前**,
// 于是通道冲突时:卡片不抖、红 toast 不出、后续渲染也没跑。node 侧对 bridge/mock 的
// 纯函数断言(如 smoke-input.mjs)只走到 `setChannelId()` 的返回值,从不触碰这段
// DOM 接线,证不出这个缺陷 ——「源码正则/返回值断言 ≠ 可执行」,必须起真页面点一次卡片。
//
// 跑什么(同一条会话上连续走完):
//   ① `?scenario=occupied`(= fixture=channel-conflict,channel_id=0、
//      caps.occupiedMask 全 15 位默认置位)首帧渲染完毕;
//   ② 点「未分配」卡(ch=0)⇒ 打开释放确认条(这一步走的是 wireChannels 的直达分支,
//      不经过 claimChannel,先确认确认条真的能开、给后面的③ 一个「本该被清掉」的初始态);
//   ③ 点通道卡 ch=3(占用位默认全置)⇒ 触发 `claimChannel(3)` → 冲突分支:
//      · 红 toast 出现,文案逐字等于词条 `ch.occupied` 填上 {n:3,g:"A"};
//      · 卡片 3 在动画结束前拿到 `data-shake="1"`;
//      · 释放确认条被**关掉**——`claimChannel` 顶部已把 `pendingRelease` 置回 false,
//        但只有跑到最后那句 `render()`(→ `renderChannels()` 的 `show(release, …)`)
//        confirm 条才会真的从屏幕上消失;异常挡在 render() 之前的旧代码,这一位
//        会停在"本地状态已经翻转、屏幕上却还留着上一帧" —— 拿它当"render 确实跑了"
//        的判据,不与 toast/shake 这两条重复(那两条是 claimChannel 里在 render()
//        **之前**就直接调用的,证明不了 render() 本身有没有执行到)。
//   ④ 每段零 console.error、零未捕获异常(旧代码的 SyntaxError 会在这里现形)。
//
// 删除式(未提交,人工核过):把 claimChannel 里的选择器改回
// `$('input.channels.card[data-ch="' + ch + '"]')` 那个坏形态,本文件②③段必须转红
// (SyntaxError 未捕获 ⇒ ④ 段的零异常断言先炸,②③ 的后续断言全部读到 null/超时)。
//
// 用法:node web-preview/tests/smoke-input-conflict-page.mjs [仓库根绝对路径]
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-input-conflict-"));
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

    // =========================================================================
    log(
        "=== ① ?scenario=occupied 首帧(channel-conflict:channel_id=0,全占用位)===",
    );
    await open("scenario=occupied");
    assertClean("① 首帧");

    // =========================================================================
    log("=== ② 点「未分配」卡(ch=0)⇒ 释放确认条展开 ===");
    check(
        await evaluate(
            IN(
                `const c = card(0); if (!c) return false; c.click(); return true;`,
            ),
        ),
        "点到了「未分配」卡",
    );
    check(
        await waitFor(
            IN(`const r = gb("input.channels.releaseConfirm");
                return !!r && r.hidden === false;`),
            6000,
        ),
        "释放确认条已展开(初始态:等 ③ 去清掉它)",
    );
    assertClean("② 释放确认条展开");

    // =========================================================================
    log("=== ③ 点通道卡 ch=3(占用位已置)⇒ SL-19 冲突反馈 ===");
    check(
        await evaluate(
            IN(
                `const c = card(3); if (!c) return false; c.click(); return true;`,
            ),
        ),
        "点到了通道卡 3",
    );

    // ---- 红 toast:文案逐字等于词条 ch.occupied 填上 {n:3,g:"A"} ----
    check(
        await waitFor(
            IN(`const t = gb("input.toast.occupied");
                return !!t && t.hidden === false;`),
            6000,
        ),
        "红 toast 在 6s 内上屏(旧代码这里永远等不到:异常挡在 showOccupiedToast 之前)",
    );
    const toastText = await evaluate(
        IN(`const t = gb("input.toast.occupied.text");
            return t ? t.textContent : null;`),
    );
    const wantToast = String(T.zh["ch.occupied"])
        .replace("{n}", "3")
        .replace("{g}", "A");
    eq(toastText, wantToast, "toast 文案逐字等于词条 ch.occupied(n=3, g=A)");

    // ---- 卡片抖动:在 --dur-shake(.45s)结束前必须已经带上 data-shake="1" ----
    const shaking = await evaluate(
        IN(
            `const c = card(3); return c ? c.getAttribute("data-shake") : null;`,
        ),
    );
    eq(
        shaking,
        "1",
        "通道卡 3 拿到 data-shake=1(旧代码这里永远是 null:异常挡在 shake() 之前)",
    );

    // ---- render 确实跑了:释放确认条(①②那份"本该被清掉"的初始态)必须已经隐藏。
    // claimChannel 顶部已经把 pendingRelease 同步置回 false,但只有跑到最后那句
    // render()(→ renderChannels() 的 show(release, pendingRelease))屏幕才会跟上;
    // 旧代码在 render() 之前就抛了异常,这一位会停在「本地态已翻转、画面没跟上」。
    check(
        await waitFor(
            IN(`const r = gb("input.channels.releaseConfirm");
                return !!r && r.hidden === true;`),
            6000,
        ),
        "释放确认条已被收起(证明 claimChannel 跑完了最后那句 render(),不只是跑到 shake/toast)",
    );

    assertClean("③ 通道冲突反馈");
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
console.log("\n✅ Input 通道冲突反馈(SL-19)页面级冒烟全绿");
process.exit(0);
