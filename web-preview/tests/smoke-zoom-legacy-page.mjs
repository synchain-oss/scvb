// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB 旧 WebKit 缩放降级 —— **页面级**冒烟(无头 Chrome + CDP;M09)
// -----------------------------------------------------------------------------
// 被测:web/shared/hit.js 的 `zoomRectMode()`(CSS zoom 下 getBoundingClientRect 的语义
// 探测)与 web/shared/shell-fit.js 的降级三件事(倍率锁 1 / 档位只留 1 档并经 setUiScale
// 请回 1 / 页面一行提示),以及轨迹图 Cmd(metaKey)+滚轮 = 横向缩放。
//
// 为什么必须页面级:「旧语义」只在 WebKit 26.4 之前的系统 WKWebView 上出现,CI 与本机
// 都没有那个引擎。这里用 CDP 的 `Page.addScriptToEvaluateOnNewDocument` 在**每个 frame**
// 的脚本运行之前把 `Element.prototype.getBoundingClientRect` 换成旧语义(按元素的有效
// zoom 除回去,宽高与位置都不含缩放),于是真源页面从第一行起就活在旧语义里 —— 探测、
// 降级、命中测试走的都是产品代码本身,node 侧一个环节都执行不到。
// 模拟只动 rect 这一条(offset* 两种语义本来就相同),与 hit.js 头注写的判据同一口径。
//
// 断言面(两段,**同一个浏览器会话**里先新语义、后旧语义)。三页都以 `?scale=1.5` 开窗 ——
// 「用户存过 1.5 档、宿主按它开窗」:壳页给 1.5 倍的 iframe,mock 快照的 `ui.scale` 也是 1.5
// (与真宿主同源,见 web-preview/mock/state-driver.js 的 openingScaleFor)。
//   【新语义】真 Chromium:
//     (n0) 前提:页内 zoom:2 探针的 rect 宽 = 2 × offsetWidth(真新语义);
//     (n1) 页面读数到 1.5(快照档位已落到界面)之后:zoomLocked() = false、倍率按视口反算
//          = 1.5、窗口仍是设计盒 × 1.5(**没有**请回 1);
//     (n2) 档位一个不少(= design-box.js 的档位表;Output 两处入口都数);
//     (n3) 锁定提示 hidden 且不占版面;
//     (n4)(Output / Input)宿主改到 1.25 档 ⇒ 页面读数跟到 1.25,窗口仍是设计盒 × 1.25、
//          倍率 1.25(state 回推这条路同样**不**请回 1)。Monitor 的 `scvb.state` 处理器本来
//          就不读 `ui.scale`(档位只认快照与本页预览),这一格对它不成立,不跑;
//     (n5)(仅 Output)zoom 1.25 下按曲线点的**真实**屏幕位置按下 ⇒ 曲线编辑器命中该点
//          (对照臂:证明下面 (l5) 那一格的判据在新语义下本来就成立);
//   【旧语义】装上模拟之后:
//     (l0) 前提:模拟生效 —— 页内探针 rect 宽 = offsetWidth;
//     (l4) 快照带着 1.5 ⇒ 页面经现有 setUiScale 请回 1:窗口回到设计盒(1.5 倍的 iframe 只有
//          这一条请求能把它改回去),且 zoomLocked() = true;
//     (l2) 档位只剩 1 档(Output 两处入口都只剩 100%);
//     (l3) 锁定提示可见、文案 = 当前语言的 `scale.lockedLegacy`、整句不被截断、footer 不溢出;
//     (l1) 宿主**自己**把窗口改成 1.5 倍(不经档位,state 不变)⇒ 倍率仍锁在 1,不随视口变
//          (降级而不是换算);
//     (l5)(仅 Output)就在这个 1.5 倍窗口里,按曲线点的真实屏幕位置按下 ⇒ 命中该点
//          (锁 1 之后两种语义逐字相同,命中按构造正确;不锁的话 zoom=1.5 下必然打偏);
//     (m1)(仅 Monitor)轨迹图上 Cmd+滚轮 ⇒ 横向缩放档位变大([M09] metaKey 同 Ctrl);
//   每页零 console.error、零未捕获异常。
//
// 删除式(**注入未提交**,逐格实跑过;读数见 PR 描述)——被注入的是产品代码,不是本文件:
//   D1 shell-fit.js apply() 里的降级分支(`locked ? 1 :`)删掉 ⇒ 红在 (l1) 与 (l5);
//   D2 scalePresets() 不过滤 ⇒ 红在 (l2);
//   D3 enforceZoomLock() 不发请求 ⇒ 红在 (l4);
//   D4 三页不摘提示的 hidden ⇒ 红在 (l3);
//   D5 trajectory-chart.js 的 `e.ctrlKey || e.metaKey` 改回 `e.ctrlKey` ⇒ 红在 (m1);
//   D6 hit.js classifyZoomRect 恒返回 standard(探测失效)⇒ 旧语义段 (l1)–(l5) 全红。
//
// 用法:node web-preview/tests/smoke-zoom-legacy-page.mjs [仓库根绝对路径]
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-zoom-legacy-"));
// 窗口要装得下「Output 以 1.5 档开窗」(1770×1170 + 工具条),理由同 smoke-shell-fit-page:
// iframe 整个留在视区内,躲开无头下对离屏 iframe 的渲染节流;CDP 鼠标事件也要落得进去。
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

// ---------------------------------------------------------------- 旧语义模拟
// WebKit 26.4 之前:getBoundingClientRect 按元素的**有效** zoom(自身 × 祖先)除回去,
// 宽高与位置都不含缩放。有效 zoom 优先读 `currentCSSZoom`(Chromium 128+),没有时沿祖先链
// 把 computed zoom 连乘(旧 Chromium 的 computed zoom 是元素自身那一层)。
// 原函数挂在 `__scvbTrueRect` 上(不可枚举),断言侧用它取**真实**屏幕位置。
const LEGACY_RECT_SCRIPT = `(() => {
    const proto = Element.prototype;
    if (proto.__scvbTrueRect) return;
    const orig = proto.getBoundingClientRect;
    Object.defineProperty(proto, "__scvbTrueRect", { value: orig });
    const effZoom = (el) => {
        if (typeof el.currentCSSZoom === "number" && el.currentCSSZoom > 0) {
            return el.currentCSSZoom;
        }
        let z = 1;
        for (let n = el; n && n.nodeType === 1; n = n.parentElement) {
            const v = parseFloat(getComputedStyle(n).zoom);
            if (v > 0) z *= v;
        }
        return z;
    };
    proto.getBoundingClientRect = function () {
        const r = orig.call(this);
        const z = effZoom(this);
        if (z === 1) return r;
        return new DOMRect(r.x / z, r.y / z, r.width / z, r.height / z);
    };
})();`;

// ---------------------------------------------------------------- 三侧配置
// 设计盒与档位表**从真源读**(web/shared/design-box.js),词条从 web/shared/i18n.js 读。
const { DESIGN } = await import(
    new URL("../../web/shared/design-box.js", import.meta.url)
);
const { fitFactor } = await import(
    new URL("../../web/shared/shell-fit.js", import.meta.url)
);
const { T } = await import(
    new URL("../../web/shared/i18n.js", import.meta.url)
);
const expectedZoom = (box, f) =>
    String(
        fitFactor(Math.round(box.w * f), Math.round(box.h * f), box.w, box.h),
    );

/** 档位入口:每一处都按 `[{name, values}]` 取回(Output 有两处:footer 下拉与设置页)。 */
const ROLES = [
    {
        role: "output",
        page: "web-preview/output.html?fixture=fifteen-tracks",
        sel: "#card",
        box: DESIGN.output,
        ready: `!!(w.__SCVB_OUTPUT__ && gb("footer-scale-panel"))`,
        lock: "footer-scale-lock",
        footer: "footer",
        entries: `[
            { name: "footer 下拉", values: Array.from(gb("footer-scale-panel").querySelectorAll("[data-scale]")).map((o) => o.getAttribute("data-scale")) },
            { name: "设置页 select", values: Array.from(gb("settings-scale-select").options).map((o) => o.value) },
        ]`,
        // 页面把 state 回推的档位落到界面上的那个读数(新语义 (n4) 的「已经跟到」信号)
        shown: `gb("footer-scale-label").textContent`,
        shownFor: (f) => Math.round(f * 100) + "%",
    },
    {
        role: "input",
        page: "web-preview/input.html",
        sel: "#ipt-shell",
        box: DESIGN.input,
        ready: `!!gb("input.footer.scale")`,
        lock: "input.footer.scaleLock",
        footer: "input.footer",
        entries: `[{ name: "footer select", values: Array.from(gb("input.footer.scale").options).map((o) => o.value) }]`,
        shown: `gb("input.footer.scale").value`,
        shownFor: (f) => String(f),
    },
    {
        role: "monitor",
        page: "web-preview/monitor.html",
        sel: "#card",
        box: DESIGN.monitor,
        ready: `!!(w.__SCVB_MONITOR__ && gb("monitor-scale"))`,
        lock: "monitor-scale-lock",
        footer: "monitor-footer",
        entries: `[{ name: "footer select", values: Array.from(gb("monitor-scale").options).map((o) => o.value) }]`,
        shown: `gb("monitor-scale").value`,
        shownFor: (f) => String(f),
        // Monitor 的 `scvb.state` 处理器不读 `ui.scale`(见头注 (n4)),宿主推档位它不跟
        stateEchoScale: false,
    },
];

function assertClean(label) {
    eq(bucket.errors, [], `${label}:零 console.error`);
    eq(bucket.exceptions, [], `${label}:零未捕获异常`);
}

/** 以 `?scale=` 开窗并等到真源页面装好(壳页注入完成 + 本页自己的就绪件都在)。 */
async function open(cfg, scale) {
    await cdp.send("Page.navigate", { url: "about:blank" });
    await sleep(120);
    const sep = cfg.page.includes("?") ? "&" : "?";
    await cdp.send("Page.navigate", {
        url: `${base}/${cfg.page}${sep}scale=${scale}`,
    });
    // 装载判据取壳页的注入状态位(`data-ok="1"` = 真源 app.js 的模块体已求值,
    // installShellFit 与档位选项都已落定),再加本页自己的就绪件。
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

/** 一次往返取回本页与降级相关的全部读数。 */
const STATE = (cfg) =>
    IN_ASYNC(`
    const sf = await w.eval('import("/web/shared/shell-fit.js")');
    const el = q(${JSON.stringify(cfg.sel)});
    const de = d.documentElement;
    const hint = gb(${JSON.stringify(cfg.lock)});
    const footer = gb(${JSON.stringify(cfg.footer)});
    const cs = hint ? w.getComputedStyle(hint) : null;
    // 探针:与 hit.js 同形(zoom:2、宽 100px),量的是**此刻生效**的 rect 语义
    const p = d.createElement("div");
    p.style.cssText = "position:absolute;left:0;top:0;width:100px;height:1px;visibility:hidden;zoom:2";
    d.body.appendChild(p);
    const probe = { rect: p.getBoundingClientRect().width, offset: p.offsetWidth };
    p.remove();
    return {
        locked: sf.zoomLocked(),
        zoom: el.style.zoom,
        vw: de.clientWidth,
        vh: de.clientHeight,
        docOver: de.scrollWidth > de.clientWidth || de.scrollHeight > de.clientHeight,
        entries: ${cfg.entries},
        hint: hint
            ? {
                  hidden: hint.hidden,
                  display: cs.display,
                  text: hint.textContent.trim(),
                  w: hint.getBoundingClientRect().width,
                  clipped: hint.scrollWidth > hint.clientWidth + 1,
              }
            : null,
        footerOver: footer ? footer.scrollWidth > footer.clientWidth + 1 : null,
        lang: de.lang || "zh",
        probe,
    };
`);

/** 壳页扮宿主改档位(= 原生 setUiScale:mock 记 state.ui.scale,壳页按设计盒 × 档位改 iframe)。 */
const hostPush = (f) =>
    evaluate(`(() => {
        const m = window.__SCVB_PREVIEW_MOCK__;
        if (!m || typeof m.setUiScale !== "function") return false;
        m.setUiScale(${f});
        return true;
    })()`);

const frameWidth = () =>
    evaluate(OUT(`return Math.round(f.getBoundingClientRect().width);`));

// ---- Output 曲线编辑器:按曲线点的**真实**屏幕位置按下,读诊断面判命中 ------------
// 换算走真模块(不在测试里抄第二份几何);包围盒取真实值(`tr`),不吃旧语义模拟。
const curvePointXY = (angle, db) =>
    evaluate(
        IN_ASYNC(`
        const m = await import("${base}/web/output/canvas/curve-editor.js");
        const c = gb("master-pancurve-canvas");
        const r = tr(c);
        const fr = (Element.prototype.__scvbTrueRect || Element.prototype.getBoundingClientRect).call(f);
        return {
            x: fr.left + r.left + (m.angleToX(${angle}) / m.PLOT_W) * r.width,
            y: fr.top + r.top + (m.dbToY(${db}) / m.PLOT_H) * r.height,
        };
    `),
    );

async function mouse(type, x, y) {
    await cdp.send("Input.dispatchMouseEvent", {
        type,
        x,
        y,
        button: "left",
        buttons: type === "mouseReleased" ? 0 : 1,
        clickCount: 1,
        pointerType: "mouse",
    });
}

/**
 * 曲线点 (angle 0 / 0 dB,demo 夹具的第 4 点,离两侧邻点最远)此刻能不能被按中。
 * 前提格(tour 罩层收起 / 6 个点进了 store / 画布位置稳住 / 按下点命中画布本身)
 * 各自出声,别让「没进拖动态」只剩一句话。返回是否进入了拖动态。
 */
async function curveHit(tag) {
    await evaluate(
        IN(`const later = gb("tour-ask-later");
            const ov = gb("tour-ask");
            if (later && ov && !ov.hidden) later.click();
            return true;`),
    );
    check(
        await waitFor(
            IN(`const c = gb("master-pancurve-canvas");
                const r = c ? tr(c) : null;
                const cd = w.__SCVB_OUTPUT__.curve();
                return !!r && r.width > 50 && r.height > 20 &&
                       cd.curveSig.split("|").length === 6 &&
                       cd.dragging === false && cd.hasPreview === false;`),
            8000,
        ),
        `${tag}:demo 的 6 个曲线点已进 store、画布已布局、没有在飞的编辑`,
    );
    // 画布位置连续 5 次(约 400ms)不变才按:首帧之后横幅出 / 收会把画布上下挪几十 px。
    let last = "";
    let same = 0;
    for (let i = 0; i < 60 && same < 5; i++) {
        const r = await evaluate(
            IN(`const c = gb("master-pancurve-canvas");
                const r = tr(c);
                return [r.left, r.top, r.width, r.height].map((v) => v.toFixed(1)).join(",");`),
        );
        same = r === last ? same + 1 : 0;
        last = r;
        if (same < 5) await sleep(80);
    }
    check(same >= 5, `${tag}:画布位置已稳定`);
    const pt = await curvePointXY(0, 0);
    check(
        await waitFor(
            IN(`const ov = gb("tour-ask");
                if (ov && !ov.hidden) return false;
                const c = gb("master-pancurve-canvas");
                const fr = (Element.prototype.__scvbTrueRect || Element.prototype.getBoundingClientRect).call(f);
                return d.elementFromPoint(${pt.x} - fr.left, ${pt.y} - fr.top) === c;`),
            5000,
        ),
        `${tag}:按下点此刻命中画布本身(无罩层)`,
    );
    await mouse("mouseMoved", pt.x, pt.y);
    await mouse("mousePressed", pt.x, pt.y);
    const hit = await waitFor(
        IN(`return w.__SCVB_OUTPUT__.curve().dragging === true;`),
        2000,
    );
    await mouse("mouseReleased", pt.x, pt.y);
    // 松手会提交一次(点表未变);等它落定,别把在飞的提交带进下一格。
    await waitFor(
        IN(`const cd = w.__SCVB_OUTPUT__.curve();
            return cd.dragging === false && cd.hasPreview === false;`),
        5000,
    );
    return hit;
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

    // =========================================================================
    log("=== 【新语义】真 Chromium:行为与改前逐字相同 ===");
    for (const cfg of ROLES) {
        const { role, box } = cfg;
        const tag = `新/${role}`;
        newBucket(tag);
        if (!check(await open(cfg, 1.5), `${tag}:以 1.5 档开窗装载完成`)) {
            continue;
        }
        // (n1) 正向时刻:快照的 1.5 已经落到界面读数上。请回 1 若会发,就发在落读数的
        // 这同一次渲染里(enforceZoomLock 是同步的),其后壳页改 iframe 只隔一个微任务 ——
        // 所以「读数到了 + 两帧」之后量窗口,「没发」才有牙。
        check(
            await waitFor(
                IN(
                    `return ${cfg.shown} === ${JSON.stringify(cfg.shownFor(1.5))};`,
                ),
                8000,
            ),
            `${tag}(n1)页面读数到 1.5(mock 快照带着开窗档位)`,
        );
        await twoFrames();
        await twoFrames();
        const s = await evaluate(STATE(cfg));
        if (!check(s, `${tag}:取到读数`)) continue;
        // (n0) 前提:这一段跑在真新语义上,否则下面「没锁」的判据什么也没证明
        eq(
            s.probe,
            { rect: 200, offset: 100 },
            `${tag}(n0)前提:zoom:2 探针 rect 宽 = 2 × offsetWidth(新语义)`,
        );
        eq(s.locked, false, `${tag}(n1)zoomLocked() = false`);
        eq(
            [s.zoom, await frameWidth()],
            [expectedZoom(box, 1.5), Math.round(box.w * 1.5)],
            `${tag}(n1)倍率按视口反算 = 1.5,窗口仍是设计盒 × 1.5(没有请回 1)`,
        );
        const all = box.presets.map(String);
        for (const e of s.entries) {
            eq(e.values, all, `${tag}(n2)${e.name}:档位一个不少`);
        }
        check(
            s.hint && s.hint.hidden === true && s.hint.display === "none",
            `${tag}(n3)锁定提示 hidden 且不占版面(实得 ${JSON.stringify(s.hint)})`,
        );

        // (n4) 宿主改到 1.25:state 回推这条路同样不许请回 1。
        if (cfg.stateEchoScale !== false) {
            check(await hostPush(1.25), `${tag}(n4)壳页扮宿主改到 1.25 档`);
            check(
                await waitFor(
                    IN(
                        `return ${cfg.shown} === ${JSON.stringify(cfg.shownFor(1.25))};`,
                    ),
                    6000,
                ),
                `${tag}(n4)页面读数跟到 1.25(state 回推已渲染)`,
            );
            await twoFrames();
            await twoFrames();
            eq(
                [
                    await frameWidth(),
                    await evaluate(
                        IN(`return q(${JSON.stringify(cfg.sel)}).style.zoom;`),
                    ),
                ],
                [Math.round(box.w * 1.25), expectedZoom(box, 1.25)],
                `${tag}(n4)窗口仍是设计盒 × 1.25、倍率 ${expectedZoom(box, 1.25)}(没有请回 1)`,
            );
        }

        // (n5) 对照臂:新语义 + zoom≠1 下命中本来就对
        if (role === "output") {
            check(
                await curveHit(tag),
                `${tag}(n5)zoom ${expectedZoom(box, 1.25)} 下按曲线点的真实位置 ⇒ 命中该点(进入拖动态)`,
            );
        }
        assertClean(tag);
    }

    // =========================================================================
    log(
        "=== 【旧语义】模拟 WebKit 26.4 之前的 rect:缩放锁 1、提示可见、命中正确 ===",
    );
    await cdp.send("Page.addScriptToEvaluateOnNewDocument", {
        source: LEGACY_RECT_SCRIPT,
    });
    for (const cfg of ROLES) {
        const { role, box } = cfg;
        const tag = `旧/${role}`;
        newBucket(tag);
        if (!check(await open(cfg, 1.5), `${tag}:以 1.5 档开窗装载完成`)) {
            continue;
        }
        // (l4) 1.5 倍的 iframe 只有页面那一记 setUiScale(1) 能改回设计盒(壳页扮宿主)。
        check(
            await waitFor(
                OUT(
                    `return Math.round(f.getBoundingClientRect().width) === ${box.w};`,
                ),
                8000,
            ),
            `${tag}(l4)快照带 1.5 ⇒ 页面经 setUiScale 请回 1:窗口回到设计盒宽 ${box.w}(实得 ${await frameWidth()})`,
        );
        await twoFrames();
        const s = await evaluate(STATE(cfg));
        if (!check(s, `${tag}:取到读数`)) continue;
        // (l0) 前提:模拟真的装上了(否则下面每一格都在测新语义)
        eq(
            s.probe,
            { rect: 100, offset: 100 },
            `${tag}(l0)前提:旧语义模拟生效(zoom:2 探针 rect 宽 = offsetWidth)`,
        );
        eq(
            [s.locked, s.zoom],
            [true, "1"],
            `${tag}(l4)zoomLocked() = true,倍率 1`,
        );
        for (const e of s.entries) {
            eq(e.values, ["1"], `${tag}(l2)${e.name}:只剩 1 档`);
        }
        const want = T[s.lang]
            ? T[s.lang]["scale.lockedLegacy"]
            : T.zh["scale.lockedLegacy"];
        check(
            s.hint &&
                s.hint.hidden === false &&
                s.hint.display !== "none" &&
                s.hint.w > 0,
            `${tag}(l3)锁定提示可见(实得 ${JSON.stringify(s.hint)})`,
        );
        eq(
            s.hint && s.hint.text,
            want,
            `${tag}(l3)提示文案 = ${s.lang}.scale.lockedLegacy`,
        );
        eq(
            [s.hint && s.hint.clipped, s.footerOver],
            [false, false],
            `${tag}(l3)提示整句可见(不被省略号截断)、footer 不溢出`,
        );

        // (l1) 宿主**自己**改窗口(部分 DAW 会绕过 setResizable 直接改编辑器尺寸):
        // state 不变、只有视口变 —— 新语义下倍率会跟到 1.5,旧语义下必须仍是 1。
        const W = Math.round(box.w * 1.5);
        const H = Math.round(box.h * 1.5);
        await evaluate(
            OUT(
                `f.style.width = ${W} + "px"; f.style.height = ${H} + "px"; return true;`,
            ),
        );
        check(
            await waitFor(IN(`return d.documentElement.clientWidth === ${W};`)),
            `${tag}(l1)宿主把窗口改成 ${W}×${H}`,
        );
        await twoFrames();
        await twoFrames();
        eq(
            await evaluate(
                IN(`return q(${JSON.stringify(cfg.sel)}).style.zoom;`),
            ),
            "1",
            `${tag}(l1)视口 1.5 倍,倍率仍锁在 1(降级而不是换算)`,
        );

        // (l5) 命中:就在这个 1.5 倍窗口里(倍率锁 1)按曲线点的真实位置。
        // 不锁的话这里 zoom = 1.5,而旧语义 rect 不含缩放 ⇒ logicalFromEvent 必然打偏。
        if (role === "output") {
            check(
                await curveHit(tag),
                `${tag}(l5)1.5 倍窗口里按曲线点的真实位置 ⇒ 命中该点(进入拖动态)`,
            );
        }

        // (m1) Cmd+滚轮 = 横向缩放(真 CDP 输入管线,modifiers 位 4 = Meta)
        if (role === "monitor") {
            const readZoom = () =>
                evaluate(IN(`return gb("monitor-traj-zoom").textContent;`));
            const factorOf = (txt) => {
                const m = /×([\d.]+)/.exec(String(txt || ""));
                return m ? Number(m[1]) : NaN;
            };
            const before = await readZoom();
            const c = await evaluate(
                IN(`const r = tr(gb("monitor-traj-canvas"));
                    const fr = (Element.prototype.__scvbTrueRect || Element.prototype.getBoundingClientRect).call(f);
                    return { x: fr.left + r.left + r.width / 2, y: fr.top + r.top + r.height / 2 };`),
            );
            await cdp.send("Input.dispatchMouseEvent", {
                type: "mouseWheel",
                x: c.x,
                y: c.y,
                deltaX: 0,
                deltaY: -120,
                modifiers: 4,
                pointerType: "mouse",
            });
            let after = before;
            const t0 = Date.now();
            while (Date.now() - t0 < 3000) {
                after = await readZoom();
                if (factorOf(after) > factorOf(before)) break;
                await sleep(80);
            }
            check(
                factorOf(after) > factorOf(before),
                `${tag}(m1)Cmd+滚轮 ⇒ 轨迹图横向放大(档位 ${before} → ${after})`,
            );
        }
        assertClean(tag);
    }
} catch (e) {
    fail++;
    console.log(`  [FAIL] 跑挂了:${e && e.stack ? e.stack : e}`);
} finally {
    teardown();
}

log("");
if (fail === 0) {
    log("✅ smoke-zoom-legacy-page:全绿");
    process.exit(0);
}
log(`❌ smoke-zoom-legacy-page:${fail} 条断言失败`);
process.exit(1);
