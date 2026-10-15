// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB 缩放档位下拉 —— **页面级**冒烟(无头 Chrome + CDP;UI-1)
// -----------------------------------------------------------------------------
// 用户 2026-10-15 报:缩放下拉的展开列表,Output 右下角已是自定义深色浮层,而 Monitor
// 右下角、Output 设置页「界面缩放」(以及 Input 右下角)仍是原生 <select> —— 展开后是浏览器
// 自绘的白底系统列表,页面 CSS 改不到。UI-1 把 Output 右下角那一套抽成共享组件
// web/shared/scale-select.js(样式 base.css .sc-scale*),四处共用。
//
// 为什么必须页面级:开合、焦点、键盘、浮层定位、防呆回退全是「真 DOM + 真事件 + 真布局」,
// node 侧一个环节都执行不到。键盘走 CDP 的 Input.dispatchKeyEvent(真输入管线),不是
// dispatchEvent 造的合成事件。
//
// 屏幕可用区:每个文档起跑前钉成 SCREEN(1600×1000),于是「超出当前屏幕」(05 §1.2:
// W*F > availWidth ∨ H*F > availHeight)的档位与机器无关、每侧都有几档灰着。
//
// 断言面(四处下拉:Output 右下角 / Output 设置页 / Monitor 右下角 / Input 右下角,逐处跑):
//   (s0) 全页一个原生 <select> 都没有(这四处原是仅有的几个);
//   (s1) 收起态:data-open=0、浮层不渲染;触发钮 aria-haspopup=listbox / aria-expanded=false;
//        浮层 role=listbox、aria-label = 当前语言的 `scale`;
//   (s2) 档位项 = design-box.js 的档位表(一个不少、顺序不变),role=option;超屏档
//        data-over=1 + aria-disabled=true、注记 = `scale.overflow`(**灰着但在**);
//        当前档(100%)data-current=1 + aria-selected=true、注记 = `scale.current`;
//   (s3) 点开:浮层渲染出来、是深色玻璃(底色 = tokens.css --dark-solid)、档位字色 =
//        --txt-dark-2 / 超屏档 --txt-dark-dim;整块浮层落在外壳(= 窗口)里,不被边缘裁掉;
//        焦点落在当前档;
//   (s4) 键盘(真输入管线):↓ / ↑ 逐项、Home / End 到两端、Esc 收起且焦点回触发钮;
//        触发钮上 ↓ = 展开;Ctrl / Cmd(Meta)+↓ 放行不吞(浮层不开);
//        ↑ 移到上一档 + Enter ⇒ 浮层收起、10 秒防呆框弹出、窗口变成设计盒 × 该档、
//        触发钮读数跟到该档;点「取消」⇒ 回到 100%(窗口、读数一起回);
//   (s5) 鼠标:点超屏档 ⇒ 浮层收起、不发预览(防呆框不弹);点 125% ⇒ 预览 + 防呆框;
//        (仅 Output)设置页与右下角显示同一个档位:一处选了,另一处的读数与当前档跟着变;
//        点「取消」⇒ 两处都回 100%;
//   (s6) Tab 离开 ⇒ 浮层收起;点外面 ⇒ 浮层收起;
//   (s7) 三语:切到 en / fr,注记与浮层 aria-label 跟着换(词条从 i18n.js 读);
//   (s8) 定位:把下拉挪到外壳顶上(合成场景)⇒ 向上放不下,翻成向下,浮层仍在外壳里;
//        (仅 Input,10 档)挪到外壳正中 ⇒ 上下都放不下,压 max-height 改内部滚动,
//        仍在外壳里;End 移到末项时末项滚进可见区;
//   (s9)(仅 Output 右下角)Tab3 有段选区时在浮层里按 Esc ⇒ 只收起浮层,选区还在;
//        对照臂:浮层收起后再按一次 Esc,Tab3 照常清选区;
//   每页零 console.error、零未捕获异常。
// 锁定态(旧 WebKit 只剩 100% 一档、点它不弹防呆框)在 smoke-zoom-legacy-page.mjs 的 (l2)(l6)。
//
// 删除式(**注入未提交**,逐格实跑过;读数见 PR 描述)——被注入的是 web/shared/scale-select.js:
//   K1 浮层 keydown 的 ↑/↓/Home/End 分支删掉 ⇒ 红在 (s4);
//   K2 触发钮 ↓ 展开删掉 ⇒ 红在 (s4);
//   K3 Esc 分支删掉 ⇒ 红在 (s4);
//   K6 Esc 分支里的 stopPropagation 删掉 ⇒ 红在 (s9)(对照臂照绿);
//   K4 focusout 收起删掉 ⇒ 红在 (s6) Tab;
//   K5 组合键放行(isModified)删掉 ⇒ 红在 (s4) Ctrl/Cmd;
//   O1 refreshOver 恒判「不超屏」⇒ 红在 (s2);
//   O2 pick() 里超屏档的拦截删掉 ⇒ 红在 (s5);
//   P1 choosePlacement 不翻方向 ⇒ 红在 (s8);
//   P2 choosePlacement 不压 max-height ⇒ 红在 (s8) Input 那一格;
//   S1 Output app.js renderScale 只刷右下角那一处 ⇒ 红在 (s5) 两处同步。
//
// 用法:node web-preview/tests/smoke-scale-select-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;**2 = 环境里没有 Chrome/Edge**(口径同
//   smoke-shell-fit-page.mjs 与 CLAUDE.md §6:可选依赖缺席不判红,但也绝不算通过);
//   **3 = 浏览器在、但这一次没起来 / 没连上**([SL-297] 那一档不许并进 2)。
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
    // [SL-287] 每条 CDP 调用都要有截止时间(理由见 smoke-shell-fit-page.mjs 同名段):
    // 响应不来就**永远不 resolve**,而 gate 3e 的页面级段持着全机互斥 ⇒ 一套挂死堵住全场。
    // 超时**抛错不重试**,错误里带 method 与 id,直接指到哪一条卡住。
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

// [SL-297] 三档要分开:2 = 本机没有浏览器;3 = 浏览器在、这一次没起来 / 没连上;1 = 断言红。
function browserFailed(msg) {
    console.error(
        `❌ ${msg}
` +
            "   页面级冒烟**没跑成**(退出码 3):浏览器是在的,但这一次没起来 / 没连上。" +
            "这**不是**通过,也**不是**「本机没装浏览器」—— 重跑一次通常就好;" +
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-scale-select-"));
// 窗口要装得下「Output 选 125% 档」(1475×975 + 工具条),理由同 smoke-shell-fit-page:
// iframe 整个留在视区内,躲开无头下对离屏 iframe 的渲染节流;CDP 键鼠事件也要落得进去。
const chrome = spawn(
    exe,
    [
        "--headless=new",
        `--remote-debugging-port=${CDP_PORT}`,
        `--user-data-dir=${userDataDir}`,
        "--window-size=1900,1300",
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
chrome.on("error", (e) => browserFailed(`浏览器启动失败:${e.message}`));

let cdp = null;
let bucket = { label: "启动", errors: [], exceptions: [] };
const newBucket = (label) => {
    bucket = { label, errors: [], exceptions: [] };
};

// [SL-287] 收尾必须走**所有**退出路径(机检:scripts/check-smoke-hygiene.mjs)。幂等。
const CDP_WAIT_TRIES = 300;
const CDP_WAIT_STEP_MS = 200;

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

/** 等**真源页面**里过去两个渲染帧(理由见 smoke-shell-fit-page.mjs 的同名函数)。 */
async function twoFrames() {
    await evaluate(`new Promise((r) => {
        const f = document.querySelector("iframe");
        const w = f && f.contentWindow;
        if (!w) return r(true);
        let done = false;
        const fin = () => { if (!done) { done = true; r(true); } };
        w.setTimeout(fin, 2000);
        w.requestAnimationFrame(() => w.requestAnimationFrame(fin));
    })`);
}

async function waitFor(expr, ms = 8000) {
    const t0 = Date.now();
    for (;;) {
        let v = null;
        try {
            // 上界 = 本次 waitFor 还剩多少预算(留 250ms 收尾),不是写死的常数
            // (C 档纪律,理由见 scripts/check-smoke-hygiene.mjs 头注)。
            v = await evaluate(
                expr,
                Math.max(1000, ms - (Date.now() - t0) - 250),
            );
        } catch {
            v = null;
        }
        if (v) return true;
        if (Date.now() - t0 >= ms) return false;
        await sleep(80);
    }
}

/** 在**壳页**里跑(拿得到 iframe 元素本身 —— 它就是「宿主窗口」)。 */
const OUT = (js) => `(() => {
    const f = document.querySelector("iframe");
    if (!f) return null;
    ${js}
})()`;

/** 在**真源页面**里跑。`tr(el)` = 元素的**真实**包围盒(旧语义模拟装上之后也不受它影响)。 */
const IN_BODY = (js) => `
    const f = document.querySelector("iframe");
    const w = f && f.contentWindow;
    const d = f && f.contentDocument;
    if (!w || !d) return null;
    const q = (s) => d.querySelector(s);
    const gb = (n) => q('[data-gb="' + n + '"]');
    const tr = (el) =>
        (w.Element.prototype.__scvbTrueRect || w.Element.prototype.getBoundingClientRect).call(el);
    ${js}`;
const IN = (js) => `(() => {${IN_BODY(js)}
})()`;
const IN_ASYNC = (js) => `(async () => {${IN_BODY(js)}
})()`;

// ---------------------------------------------------------------- 本套配置
// 设计盒、档位表从真源读(web/shared/design-box.js),词条从 web/shared/i18n.js 读。
const { DESIGN } = await import(
    new URL("../../web/shared/design-box.js", import.meta.url)
);
const { T } = await import(
    new URL("../../web/shared/i18n.js", import.meta.url)
);

/** 屏幕可用区(CSS px),每个文档(含 iframe)的脚本运行之前钉死。 */
const SCREEN = { w: 1600, h: 1000 };
const SCREEN_SCRIPT = `(() => {
    try {
        Object.defineProperty(screen, "availWidth", { get: () => ${SCREEN.w} });
        Object.defineProperty(screen, "availHeight", { get: () => ${SCREEN.h} });
    } catch (e) {}
})();`;
/** 05 §1.2 的超屏判据(本套给的屏幕下哪几档该灰)。 */
const overOf = (box, f) => box.w * f > SCREEN.w || box.h * f > SCREEN.h;

/** tokens.css 的两个颜色(深色玻璃浮层底色 / 深色底正文 / 深色底灰字),断言用的是计算值。 */
const DARK_SOLID = "rgb(36, 31, 46)"; // --dark-solid #241f2e
const TXT_DARK_2 = "rgb(230, 225, 240)"; // --txt-dark-2 #e6e1f0
const TXT_DARK_DIM = "rgba(233, 229, 244, 0.38)"; // --txt-dark-dim

const pct = (f) => Math.round(f * 100);

/**
 * 每一侧:页面、外壳选择器、设计盒、防呆框锚点、下拉若干处。
 * 每处下拉:根 / 触发钮 / 读数 / 浮层 / 档位项锚点、所在 tab(Output 设置页)、默认方向、
 * 选「上一档」与「125%」两格要选的档。
 */
const ROLES = [
    {
        role: "output",
        // hot-levels:四个首启位全真 ⇒ 引导页与 tour 询问步都不弹(否则罩层会吃掉键盘焦点)
        page: "web-preview/output.html?fixture=fifteen-tracks&scenario=hot-levels",
        sel: "#card",
        box: DESIGN.output,
        ready: `!!(w.__SCVB_OUTPUT__ && gb("footer-scale-panel") && gb("settings-scale-panel"))`,
        confirm: "scale-confirm",
        revert: "scale-confirm-revert",
        sites: [
            {
                name: "右下角",
                wrap: "footer-scale-select",
                trigger: "footer-scale-trigger",
                label: "footer-scale-label",
                panel: "footer-scale-panel",
                opt: (p) => `footer-scale-opt-${p}`,
                placement: "up",
                escOverTab3: true, // (s9):Tab3 有段选区时在浮层里按 Esc
            },
            {
                name: "设置页",
                tab: "settings",
                wrap: "settings-scale-select",
                trigger: "settings-scale-trigger",
                label: "settings-scale-label",
                panel: "settings-scale-panel",
                opt: (p) => `settings-scale-opt-${p}`,
                placement: "down",
            },
        ],
    },
    {
        role: "monitor",
        page: "web-preview/monitor.html",
        sel: "#card",
        box: DESIGN.monitor,
        ready: `!!(w.__SCVB_MONITOR__ && gb("monitor-scale-panel"))`,
        confirm: "monitor-scale-confirm",
        revert: "monitor-scale-confirm-revert",
        sites: [
            {
                name: "右下角",
                wrap: "monitor-scale",
                trigger: "monitor-scale-trigger",
                label: "monitor-scale-label",
                panel: "monitor-scale-panel",
                opt: (p) => `monitor-scale-opt-${p}`,
                placement: "up",
            },
        ],
    },
    {
        role: "input",
        page: "web-preview/input.html",
        sel: "#ipt-shell",
        box: DESIGN.input,
        ready: `!!gb("input.footer.scale.panel")`,
        confirm: "input.scale.confirm",
        revert: "input.scale.confirm.revert",
        sites: [
            {
                name: "右下角",
                wrap: "input.footer.scale",
                trigger: "input.footer.scale.trigger",
                label: "input.footer.scale.label",
                panel: "input.footer.scale.panel",
                opt: (p) => `input.footer.scale.opt.${p}`,
                placement: "up",
                clampCase: true, // 10 档:挪到外壳正中时上下都放不下
            },
        ],
    },
];

function assertClean(label) {
    eq(bucket.errors, [], `${label}:零 console.error`);
    eq(bucket.exceptions, [], `${label}:零未捕获异常`);
}

async function open(cfg) {
    await cdp.send("Page.navigate", { url: "about:blank" });
    await sleep(120);
    await cdp.send("Page.navigate", { url: `${base}/${cfg.page}` });
    const up = await waitFor(
        `(() => {
            const st = document.querySelector(".pv-status");
            if (!st || st.getAttribute("data-ok") !== "1") return false;
            return ${IN(`return !!q(${JSON.stringify(cfg.sel)}) && ${cfg.ready};`)};
        })()`,
        15000,
    );
    if (up) await twoFrames();
    return up;
}

// ---- 真输入管线的按键(CDP Input.dispatchKeyEvent;modifiers 位:Alt 1 / Ctrl 2 / Meta 4 / Shift 8)
const VK = {
    ArrowDown: 40,
    ArrowUp: 38,
    Home: 36,
    End: 35,
    Escape: 27,
    Enter: 13,
    Tab: 9,
};
async function key(name, modifiers = 0) {
    const ev = {
        key: name,
        code: name,
        windowsVirtualKeyCode: VK[name],
        nativeVirtualKeyCode: VK[name],
        modifiers,
    };
    await cdp.send("Input.dispatchKeyEvent", { type: "rawKeyDown", ...ev });
    await cdp.send("Input.dispatchKeyEvent", { type: "keyUp", ...ev });
}

/** 一次往返取回某处下拉的全部读数。 */
const READ = (cfg, site) =>
    IN(`
    const wr = gb(${JSON.stringify(site.wrap)});
    const t = gb(${JSON.stringify(site.trigger)});
    const p = gb(${JSON.stringify(site.panel)});
    const lb = gb(${JSON.stringify(site.label)});
    const card = q(${JSON.stringify(cfg.sel)});
    if (!wr || !t || !p || !card) return null;
    const pr = p.getBoundingClientRect();
    const cr = card.getBoundingClientRect();
    const de = d.documentElement;
    const ae = d.activeElement;
    const opts = Array.from(p.querySelectorAll("[data-scale]")).map((o) => {
        const n = o.querySelector(".sc-scale__note");
        return {
            v: o.getAttribute("data-scale"),
            gb: o.getAttribute("data-gb"),
            role: o.getAttribute("role"),
            cur: o.getAttribute("data-current"),
            sel: o.getAttribute("aria-selected"),
            over: o.getAttribute("data-over"),
            dis: o.getAttribute("aria-disabled"),
            note: n ? n.textContent : null,
            color: w.getComputedStyle(o).color,
            focused: o === ae,
            top: o.getBoundingClientRect().top,
            bottom: o.getBoundingClientRect().bottom,
        };
    });
    return {
        wrapTag: wr.tagName,
        open: wr.getAttribute("data-open"),
        placement: wr.getAttribute("data-placement"),
        expanded: t.getAttribute("aria-expanded"),
        haspopup: t.getAttribute("aria-haspopup"),
        role: p.getAttribute("role"),
        aria: p.getAttribute("aria-label"),
        rendered: p.getClientRects().length > 0,
        bg: w.getComputedStyle(p).backgroundColor,
        maxH: p.style.maxHeight,
        scrollable: p.scrollHeight > p.clientHeight + 1,
        panel: { top: pr.top, bottom: pr.bottom, left: pr.left, right: pr.right },
        card: { top: cr.top, bottom: cr.bottom, left: cr.left, right: cr.right },
        vw: de.clientWidth,
        vh: de.clientHeight,
        label: lb ? lb.textContent : null,
        triggerFocused: ae === t,
        focusInWrap: !!ae && wr.contains(ae),
        selects: d.querySelectorAll("select").length,
        opts,
    };
`);

const inside = (r, box) =>
    r.panel.top >= box.top - 0.5 &&
    r.panel.bottom <= box.bottom + 0.5 &&
    r.panel.left >= box.left - 0.5 &&
    r.panel.right <= box.right + 0.5;

const frameWidth = () =>
    evaluate(OUT(`return Math.round(f.getBoundingClientRect().width);`));

const confirmShown = (cfg) =>
    IN(
        `const c = gb(${JSON.stringify(cfg.confirm)}); return !!c && !c.hidden;`,
    );

/** 点「取消」⇒ 防呆框收起、窗口回设计盒、读数回 100%(读数靠 state 回推,等那一帧)。 */
async function revertTo100(cfg, site, tag) {
    check(
        await evaluate(
            IN(
                `const b = gb(${JSON.stringify(cfg.revert)}); if (!b) return false; b.click(); return true;`,
            ),
        ),
        `${tag}:点「取消」`,
    );
    check(
        await waitFor(
            IN(
                `const c = gb(${JSON.stringify(cfg.confirm)});
                 return !!c && c.hidden && gb(${JSON.stringify(site.label)}).textContent === "100%";`,
            ),
            6000,
        ),
        `${tag}:防呆框收起、读数回 100%`,
    );
    check(
        await waitFor(
            OUT(
                `return Math.round(f.getBoundingClientRect().width) === ${cfg.box.w};`,
            ),
            6000,
        ),
        `${tag}:窗口回到设计盒宽 ${cfg.box.w}`,
    );
    await twoFrames();
}

/**
 * 等条件**持续**成立 holdMs(默认 600ms,长过 mock 的一拍回推延迟 250ms + 余量)。
 * 用在「写了一次、桥的回推还在路上」的地方:条件第一次成立时,迟到的回推可能还会把它拨回去。
 */
async function waitStable(expr, holdMs = 600, ms = 8000) {
    const t0 = Date.now();
    let since = 0;
    for (;;) {
        let v = null;
        try {
            v = await evaluate(
                expr,
                Math.max(1000, ms - (Date.now() - t0) - 250),
            );
        } catch {
            v = null;
        }
        const now = Date.now();
        if (v) {
            if (!since) since = now;
            if (now - since >= holdMs) return true;
        } else since = 0;
        if (now - t0 >= ms) return false;
        await sleep(80);
    }
}

/** 负向判据要有一个等的窗口:预览若发了,确认框在桥回执(微任务级)之后立刻弹出。 */
const NO_PREVIEW_WINDOW_MS = 800;

async function runSite(cfg, site) {
    const { box } = cfg;
    const tag = `${cfg.role}/${site.name}`;
    const all = box.presets.map(String);
    const prev = box.presets[box.presets.indexOf(1) - 1]; // 100% 的上一档
    const step = 1.25;

    if (site.tab) {
        check(
            await evaluate(
                IN(
                    `const b = q('[data-tab-btn="${site.tab}"]'); if (!b) return false; b.click(); return true;`,
                ),
            ),
            `${tag}:切到 ${site.tab} tab`,
        );
        // 切 tab 会经桥写 ui.active_tab,回推是异步的:等下拉**持续**可见(见 waitStable)
        check(
            await waitStable(
                IN(
                    `return gb(${JSON.stringify(site.trigger)}).getClientRects().length > 0;`,
                ),
            ),
            `${tag}:${site.tab} tab 已显示(下拉触发钮持续可见)`,
        );
    }

    // ---- (s0)(s1)(s2) 收起态 + 档位项 ----
    let r = await evaluate(READ(cfg, site));
    if (!check(r, `${tag}:取到读数(锚点都在)`)) return;
    eq(r.selects, 0, `${tag}(s0)全页没有原生 <select>`);
    eq(
        [
            r.wrapTag !== "SELECT",
            r.open,
            r.rendered,
            r.expanded,
            r.haspopup,
            r.role,
        ],
        [true, "0", false, "false", "listbox", "listbox"],
        `${tag}(s1)收起态:浮层不渲染,触发钮 aria-haspopup=listbox / aria-expanded=false,浮层 role=listbox`,
    );
    eq(r.aria, T.zh.scale, `${tag}(s1)浮层 aria-label = zh.scale`);
    eq(
        r.opts.map((o) => o.v),
        all,
        `${tag}(s2)档位项 = design-box.js 的档位表(一个不少、顺序不变)`,
    );
    eq(
        r.opts.map((o) => o.gb),
        box.presets.map((f) => site.opt(pct(f))),
        `${tag}(s2)档位项锚点`,
    );
    eq(
        r.opts.map((o) => [o.role, o.over, o.dis]),
        box.presets.map((f) =>
            overOf(box, f) ? ["option", "1", "true"] : ["option", "0", null],
        ),
        `${tag}(s2)role=option;超屏档(${box.presets
            .filter((f) => overOf(box, f))
            .map(pct)
            .join("/")}%)灰着但在:data-over=1 + aria-disabled`,
    );
    eq(
        r.opts.map((o) => [o.cur, o.sel, o.note]),
        box.presets.map((f) =>
            overOf(box, f)
                ? ["0", "false", T.zh["scale.overflow"]]
                : f === 1
                  ? ["1", "true", T.zh["scale.current"]]
                  : ["0", "false", ""],
        ),
        `${tag}(s2)当前档 100% 标 data-current/aria-selected + 注记「${T.zh["scale.current"]}」,超屏档注记「${T.zh["scale.overflow"]}」`,
    );
    eq(r.label, "100%", `${tag}(s2)触发钮读数 100%`);

    // ---- (s3) 点开 ----
    await evaluate(
        IN(`gb(${JSON.stringify(site.trigger)}).click(); return true;`),
    );
    await twoFrames();
    r = await evaluate(READ(cfg, site));
    eq(
        [r.open, r.expanded, r.rendered, r.placement],
        ["1", "true", true, site.placement],
        `${tag}(s3)点开:浮层渲染、aria-expanded=true、向${site.placement === "up" ? "上" : "下"}展开`,
    );
    eq(r.bg, DARK_SOLID, `${tag}(s3)浮层是深色玻璃(底色 = --dark-solid)`);
    eq(
        r.opts.map((o) => o.color),
        box.presets.map((f) => (overOf(box, f) ? TXT_DARK_DIM : TXT_DARK_2)),
        `${tag}(s3)档位字色 = --txt-dark-2,超屏档 --txt-dark-dim`,
    );
    check(
        inside(r, r.card) &&
            inside(r, { top: 0, left: 0, bottom: r.vh, right: r.vw }),
        `${tag}(s3)浮层整块落在外壳与窗口里(浮层 ${JSON.stringify(r.panel)},外壳 ${JSON.stringify(r.card)})`,
    );
    eq(
        r.opts.filter((o) => o.focused).map((o) => o.v),
        ["1"],
        `${tag}(s3)展开后焦点落在当前档`,
    );

    // ---- (s4) 键盘 ----
    const focusedV = async () =>
        (await evaluate(READ(cfg, site))).opts
            .filter((o) => o.focused)
            .map((o) => o.v);
    await key("ArrowDown");
    eq(
        await focusedV(),
        [String(box.presets[box.presets.indexOf(1) + 1])],
        `${tag}(s4)↓ ⇒ 下一档`,
    );
    await key("ArrowUp");
    eq(await focusedV(), ["1"], `${tag}(s4)↑ ⇒ 回到 100%`);
    await key("End");
    eq(
        await focusedV(),
        [all[all.length - 1]],
        `${tag}(s4)End ⇒ 末项(超屏档也能停,读屏读得到它的注记)`,
    );
    await key("Home");
    eq(await focusedV(), [all[0]], `${tag}(s4)Home ⇒ 首项`);
    await key("Escape");
    r = await evaluate(READ(cfg, site));
    eq(
        [r.open, r.triggerFocused],
        ["0", true],
        `${tag}(s4)Esc ⇒ 收起,焦点回触发钮`,
    );
    await key("ArrowDown", 2);
    await key("ArrowDown", 4);
    r = await evaluate(READ(cfg, site));
    eq(r.open, "0", `${tag}(s4)Ctrl+↓ / Cmd(Meta)+↓ 放行不吞:浮层不开`);
    await key("ArrowDown");
    r = await evaluate(READ(cfg, site));
    eq(
        [r.open, r.opts.filter((o) => o.focused).map((o) => o.v)],
        ["1", ["1"]],
        `${tag}(s4)触发钮上 ↓ ⇒ 展开,焦点在当前档`,
    );
    await key("ArrowUp");
    await key("Enter");
    check(
        await waitFor(confirmShown(cfg), 4000),
        `${tag}(s4)↑ 到 ${pct(prev)}% + Enter ⇒ 10 秒防呆框弹出`,
    );
    check(
        await waitFor(
            OUT(
                `return Math.round(f.getBoundingClientRect().width) === ${Math.round(box.w * prev)};`,
            ),
            6000,
        ),
        `${tag}(s4)窗口变成设计盒 × ${prev}(实得 ${await frameWidth()})`,
    );
    check(
        await waitFor(
            IN(
                `return gb(${JSON.stringify(site.label)}).textContent === "${pct(prev)}%";`,
            ),
            6000,
        ),
        `${tag}(s4)触发钮读数跟到 ${pct(prev)}%`,
    );
    eq((await evaluate(READ(cfg, site))).open, "0", `${tag}(s4)选中后浮层收起`);
    await revertTo100(cfg, site, `${tag}(s4)`);

    // ---- (s5) 鼠标:超屏档不生效;125% 生效(Output 两处同步)----
    const overF = box.presets.find((f) => overOf(box, f));
    await evaluate(
        IN(`gb(${JSON.stringify(site.trigger)}).click();
            gb(${JSON.stringify(site.opt(pct(overF)))}).click();
            return true;`),
    );
    eq(
        (await evaluate(READ(cfg, site))).open,
        "0",
        `${tag}(s5)点超屏档 ${pct(overF)}% ⇒ 浮层收起`,
    );
    check(
        !(await waitFor(confirmShown(cfg), NO_PREVIEW_WINDOW_MS)),
        `${tag}(s5)点超屏档不发预览(防呆框不弹)`,
    );
    await evaluate(
        IN(`gb(${JSON.stringify(site.trigger)}).click();
            gb(${JSON.stringify(site.opt(pct(step)))}).click();
            return true;`),
    );
    check(
        await waitFor(confirmShown(cfg), 4000),
        `${tag}(s5)点 ${pct(step)}% ⇒ 防呆框弹出`,
    );
    check(
        await waitFor(
            OUT(
                `return Math.round(f.getBoundingClientRect().width) === ${Math.round(box.w * step)};`,
            ),
            6000,
        ),
        `${tag}(s5)窗口变成设计盒 × ${step}`,
    );
    for (const other of cfg.sites) {
        check(
            await waitFor(
                IN(`const lb = gb(${JSON.stringify(other.label)});
                    const o = gb(${JSON.stringify(other.opt(pct(step)))});
                    return !!lb && !!o && lb.textContent === "${pct(step)}%" && o.getAttribute("data-current") === "1";`),
                6000,
            ),
            `${tag}(s5)${other.name}的读数与当前档跟到 ${pct(step)}%`,
        );
    }
    await revertTo100(cfg, site, `${tag}(s5)`);
    for (const other of cfg.sites) {
        eq(
            await evaluate(
                IN(`return gb(${JSON.stringify(other.label)}).textContent;`),
            ),
            "100%",
            `${tag}(s5)取消后${other.name}也回 100%`,
        );
    }

    // ---- (s6) Tab 离开 / 点外面 ----
    await evaluate(
        IN(`gb(${JSON.stringify(site.trigger)}).click(); return true;`),
    );
    await key("Tab");
    // 收起排在 focusout 之后的下一个任务里(要看焦点落定在哪),等那一拍
    check(
        await waitFor(
            IN(`const wr = gb(${JSON.stringify(site.wrap)});
                return wr.getAttribute("data-open") === "0" && !wr.contains(d.activeElement);`),
            3000,
        ),
        `${tag}(s6)Tab 离开 ⇒ 收起,焦点已不在下拉里`,
    );
    await evaluate(
        IN(`gb(${JSON.stringify(site.trigger)}).click(); return true;`),
    );
    eq((await evaluate(READ(cfg, site))).open, "1", `${tag}(s6)再点开`);
    await evaluate(
        IN(`q(${JSON.stringify(cfg.sel)}).dispatchEvent(new w.PointerEvent("pointerdown", { bubbles: true }));
            return true;`),
    );
    eq((await evaluate(READ(cfg, site))).open, "0", `${tag}(s6)点外面 ⇒ 收起`);

    // ---- (s7) 三语 ----
    // 切语言经桥写 ui.language,回推异步到达(mock 延后一拍):连点三语时,前一次的回推可能
    // 落在后一次之后把语言拨回去。所以每一格都等条件**持续**成立(waitStable),回推都落完
    // 再切下一种 —— 不然最后一格 zh 刚绿,迟到的 fr 回推就把下一处下拉带进法语。
    for (const lang of ["en", "fr", "zh"]) {
        await evaluate(
            IN(
                `const b = q('[data-lang="${lang}"]'); if (b) b.click(); return true;`,
            ),
        );
        const t = T[lang];
        check(
            await waitStable(
                IN(`const p = gb(${JSON.stringify(site.panel)});
                    const cur = gb(${JSON.stringify(site.opt(100))}).querySelector(".sc-scale__note");
                    const ov = gb(${JSON.stringify(site.opt(pct(overF)))}).querySelector(".sc-scale__note");
                    return p.getAttribute("aria-label") === ${JSON.stringify(t.scale)} &&
                        cur.textContent === ${JSON.stringify(t["scale.current"])} &&
                        ov.textContent === ${JSON.stringify(t["scale.overflow"])} &&
                        d.documentElement.lang === ${JSON.stringify(lang)};`),
            ),
            `${tag}(s7)${lang}:aria-label / 「当前」/「超出当前屏幕」三处跟着换(且回推落完后仍是 ${lang})`,
        );
    }

    // ---- (s8) 定位(合成场景:把下拉挪到外壳顶上 / 正中)----
    const moveTo = (topExpr) =>
        evaluate(
            IN(`const wr = gb(${JSON.stringify(site.wrap)});
                const card = q(${JSON.stringify(cfg.sel)});
                wr.style.position = "absolute";
                wr.style.right = "24px";
                wr.style.zIndex = "50";
                wr.style.top = (${topExpr}) + "px";
                return true;`),
        );
    const restore = () =>
        evaluate(
            IN(`const wr = gb(${JSON.stringify(site.wrap)});
                wr.style.position = wr.style.right = wr.style.zIndex = wr.style.top = "";
                return true;`),
        );
    await moveTo("8");
    await twoFrames();
    await evaluate(
        IN(`gb(${JSON.stringify(site.trigger)}).click(); return true;`),
    );
    await twoFrames();
    r = await evaluate(READ(cfg, site));
    eq(r.placement, "down", `${tag}(s8)挪到外壳顶上 ⇒ 向上放不下,翻成向下`);
    check(
        inside(r, r.card),
        `${tag}(s8)翻向之后浮层仍在外壳里(浮层 ${JSON.stringify(r.panel)},外壳 ${JSON.stringify(r.card)})`,
    );
    await key("Escape");
    if (site.clampCase) {
        await moveTo("(card.offsetHeight - wr.offsetHeight) / 2");
        await twoFrames();
        await evaluate(
            IN(`gb(${JSON.stringify(site.trigger)}).click(); return true;`),
        );
        await twoFrames();
        r = await evaluate(READ(cfg, site));
        check(
            r.maxH !== "" && r.scrollable && inside(r, r.card),
            `${tag}(s8)挪到外壳正中 ⇒ 上下都放不下:压 max-height(${r.maxH})改内部滚动,浮层仍在外壳里`,
        );
        await key("End");
        r = await evaluate(READ(cfg, site));
        const last = r.opts[r.opts.length - 1];
        check(
            last.focused &&
                last.top >= r.panel.top - 0.5 &&
                last.bottom <= r.panel.bottom + 0.5,
            `${tag}(s8)End ⇒ 末项获焦并滚进浮层可见区`,
        );
        await key("Escape");
    }
    await restore();
    await twoFrames();

    // ---- (s9) 浮层里的 Esc 只归浮层(仅 Output 右下角:Tab3 在场时才有页面级 Esc 处理器)----
    // tab-wave.js 的 document 级 keydown 收到 Esc 就清段选区,且不看 defaultPrevented。
    // Esc 若冒泡上去,「收起浮层」会顺带把选区清掉(PR #402 第 1 轮 claude-review【建议】1)。
    if (site.escOverTab3) {
        const waveDiag = () => evaluate(IN(`return w.__SCVB_OUTPUT__.wave();`));
        check(
            await evaluate(
                IN(
                    `const b = q('[data-tab-btn="wave"]'); if (!b) return false; b.click(); return true;`,
                ),
            ),
            `${tag}(s9)切到 Tab3`,
        );
        check(
            await waitStable(
                IN(`return gb("wave-lanes").getClientRects().length > 0;`),
            ),
            `${tag}(s9)Tab3 泳道已显示`,
        );
        // 在泳道 2 上横扫落点,取第一个真的选中段的那一下(段之间有空隙;同 smoke-web-sync-page ③)
        let selCh = 0;
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
            const ok = await waitFor(
                IN(`return w.__SCVB_OUTPUT__.wave().selectedCh > 0;`),
                1500,
            );
            if (ok) {
                selCh = (await waveDiag()).selectedCh;
                break;
            }
        }
        if (
            check(
                selCh > 0,
                `${tag}(s9)前提:Tab3 上选中了一个段(selectedCh = ${selCh})`,
            )
        ) {
            await evaluate(
                IN(`gb(${JSON.stringify(site.trigger)}).click(); return true;`),
            );
            eq(
                (await evaluate(READ(cfg, site))).open,
                "1",
                `${tag}(s9)Tab3 上点开右下角缩放下拉`,
            );
            await key("Escape");
            r = await evaluate(READ(cfg, site));
            eq(
                [r.open, r.triggerFocused, (await waveDiag()).selectedCh],
                ["0", true, selCh],
                `${tag}(s9)浮层里按 Esc ⇒ 只收起浮层(焦点回触发钮),Tab3 的段选区还在`,
            );
            // 对照臂:浮层收起之后再按一次 Esc,页面级处理器照常清选区(证明上一格的「还在」
            // 不是因为 Tab3 的 Esc 处理器根本没接上)
            await key("Escape");
            check(
                await waitFor(
                    IN(`return w.__SCVB_OUTPUT__.wave().selectedCh === 0;`),
                    3000,
                ),
                `${tag}(s9)对照:浮层收起后再按 Esc ⇒ Tab3 清掉段选区`,
            );
        }
        await evaluate(
            IN(
                `const b = q('[data-tab-btn="master"]'); if (b) b.click(); return true;`,
            ),
        );
        await twoFrames();
    }
}

// ---------------------------------------------------------------- 跑
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
    const wsUrl = targets.find((t) => t.type === "page").webSocketDebuggerUrl;
    cdp = cdpConnect(wsUrl);
    await cdp.ready;
    cdp.on((msg) => {
        if (msg.method === "Runtime.consoleAPICalled") {
            if (msg.params.type === "error") {
                bucket.errors.push(
                    (msg.params.args || [])
                        .map((a) => a.value ?? a.description ?? "")
                        .join(" "),
                );
            }
        } else if (msg.method === "Runtime.exceptionThrown") {
            const d = msg.params.exceptionDetails || {};
            bucket.exceptions.push(d.exception?.description || d.text || "?");
        }
    });
    await cdp.send("Runtime.enable");
    await cdp.send("Page.enable");
    await cdp.send("Page.addScriptToEvaluateOnNewDocument", {
        source: SCREEN_SCRIPT,
    });

    for (const cfg of ROLES) {
        log(
            `\n=== ${cfg.role}(设计盒 ${cfg.box.w}×${cfg.box.h},屏幕可用区 ${SCREEN.w}×${SCREEN.h})===`,
        );
        newBucket(cfg.role);
        if (!check(await open(cfg), `${cfg.role}:页面装载完成`)) continue;
        const screenOk = await evaluate(
            IN(
                `return w.screen.availWidth === ${SCREEN.w} && w.screen.availHeight === ${SCREEN.h};`,
            ),
        );
        if (
            !check(
                screenOk,
                `${cfg.role}:前提:屏幕可用区已钉成 ${SCREEN.w}×${SCREEN.h}`,
            )
        )
            continue;
        for (const site of cfg.sites) await runSite(cfg, site);
        assertClean(cfg.role);
    }
} catch (e) {
    fail++;
    console.log(`  [FAIL] 跑挂了:${e && e.stack ? e.stack : e}`);
} finally {
    teardown();
}

log("");
if (fail === 0) {
    log("✅ smoke-scale-select-page:全绿");
    process.exit(0);
}
log(`❌ smoke-scale-select-page:${fail} 条断言失败`);
process.exit(1);
