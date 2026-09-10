// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB 三侧外壳自适配 —— **页面级**冒烟(无头 Chrome + CDP;SL-380)
// -----------------------------------------------------------------------------
// 用户 v5.6.11 实测 B13:「缩小之后 output 不会缩小比例,而是会让画面超出框大小,
// 是个 bug。放大也是,不会自动放大,还得手动 ctrl+滚轮。」
//
// 病根是机制 9 只落了一半:宿主按档位 `setSize(设计盒 × F)`,而页面侧的 CSS zoom
// Output 一行都没有(Input/Monitor 有,但读的是档位数字、不是实际视口)。修法见
// web/shared/shell-fit.js:页面倍率 = `min(视口宽/设计宽, 视口高/设计高)`。
//
// 为什么必须页面级:整条链路是「iframe(= 宿主窗口)尺寸变 → 页内 resize/
// ResizeObserver → 写 style.zoom → 布局」,node 侧一个环节都执行不到。SL-183 的
// 教训逐字适用 —— 源码正则看得见赋值语句,看不见它有没有被调用、算出来对不对。
//
// 断言面(三侧各跑一遍):
//   ① 视口 = 设计盒 / 缩到 60% / 放到 150% —— 倍率分别是 1 / 0.6 / 1.5,**三个互不相等**
//      (「缩放因子随尺寸变化」;倍率写死不动的实现在这一格必红);
//   ② 每一格都**零溢出**:documentElement 与 body 的 scrollWidth/Height 不超过
//      clientWidth/Height,外壳的包围盒也不越出视口 —— 这就是用户说的「超出框」;
//   ③ **宽高比不匹配**的视口(宽 ×1.5、高 ×0.6):倍率取两轴的**小**者(0.6),高度贴边、
//      宽度留边。`min` 写成 `max` 的实现只在这一格红,前三格全绿 —— 故它必须在;
//   ④ 档位那条路仍然活着:走真 UI 选一档 → 壳页(扮宿主)把 iframe 改成设计盒 × 档位
//      → 页内倍率跟到该档位。壳页少接这一层的话,预览里档位就是哑的;
//   ⑤ 全程零 console.error、零未捕获异常。
//
// 用法:node web-preview/tests/smoke-shell-fit-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;**2 = 环境里没有 Chrome/Edge**(口径同
//   smoke-monitor-page.mjs 与 CLAUDE.md §6:可选依赖缺席不判红,但也绝不算通过)。
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
    return {
        ready,
        on: (fn) => listeners.push(fn),
        send(method, params) {
            const mid = ++id;
            return new Promise((ok, no) => {
                pending.set(mid, { resolve: ok, reject: no });
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-shell-fit-"));
// 窗口要装得下「Output 放到 150%」那一格(1770×1170 + 工具条):iframe 比可视区大时
// 壳页会滚动,但把它整个留在视区内可以躲开无头下对完全离屏 iframe 的渲染节流。
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
chrome.on("error", (e) => noBrowser(`浏览器启动失败:${e.message}`));

let cdp = null;
let bucket = { label: "启动", errors: [], exceptions: [] };
const newBucket = (label) => {
    bucket = { label, errors: [], exceptions: [] };
};

async function evaluate(expression) {
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

/**
 * 等**真源页面**里过去两个渲染帧。
 *
 * 视口尺寸(`clientWidth`)是查询时现算的,而 `resize` / ResizeObserver 的回调排在
 * 「更新渲染」那一步 —— 于是「视口已经是新值」与「倍率已经跟上」之间隔着至少一帧。
 * 第一版这里只等前者,量到的全是**上一格**的倍率(Output 三格全是 1、Input 卡在 0.6),
 * 看起来像「自适配根本没接线」。等两帧再量,量的才是落定后的值。
 *
 * rAF 万一被节流(无头下的离屏帧)也不许把整套挂死:2s 兜底,后面的断言照常报实得值。
 */
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
            v = await evaluate(expr);
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

/** 在**真源页面**里跑(iframe 的 contentWindow / contentDocument)。 */
const IN = (js) => `(() => {
    const f = document.querySelector("iframe");
    const w = f && f.contentWindow;
    const d = f && f.contentDocument;
    if (!w || !d) return null;
    const q = (s) => d.querySelector(s);
    const gb = (n) => q('[data-gb="' + n + '"]');
    ${js}
})()`;

/** 一次往返把「有没有溢出 / 倍率是多少 / 外壳落在哪」全取回来。 */
const GEOM = (sel) =>
    IN(`
    const el = q(${JSON.stringify(sel)});
    if (!el) return null;
    const de = d.documentElement;
    const r = el.getBoundingClientRect();
    return {
        zoom: el.style.zoom,
        vw: de.clientWidth,
        vh: de.clientHeight,
        docScrollW: de.scrollWidth,
        docScrollH: de.scrollHeight,
        bodyScrollW: d.body ? d.body.scrollWidth : 0,
        bodyScrollH: d.body ? d.body.scrollHeight : 0,
        left: r.left,
        top: r.top,
        width: r.width,
        height: r.height,
    };
`);

// ---------------------------------------------------------------- 三侧配置
// 设计盒数字**不在这里写死**:从真源 web/shared/design-box.js 读(05 §1.2 纪律一)。
const { DESIGN } = await import(
    new URL("../../web/shared/design-box.js", import.meta.url)
);

const ROLES = [
    {
        role: "output",
        page: "web-preview/output.html?fixture=fifteen-tracks",
        sel: "#card",
        box: DESIGN.output,
        // 档位入口取**设置页的 select**,不取页脚下拉的选项按钮:后者带
        // 「超出当前屏幕」灰化(05 §1.2,`W*F > screen.availWidth`),而无头 Chrome 的
        // `screen` 恒报 **800×600**(与 --window-size 无关,本机实测),于是 Output 的
        // 1180×780 连 100% 档都算「超屏」,点它按设计就是不生效 —— 那与本卡无关。
        pick: () => "settings-scale-select",
        pickKind: "select",
    },
    {
        role: "input",
        page: "web-preview/input.html",
        sel: "#ipt-shell",
        box: DESIGN.input,
        pick: () => "input.footer.scale",
        pickKind: "select",
    },
    {
        role: "monitor",
        page: "web-preview/monitor.html",
        sel: "#card",
        box: DESIGN.monitor,
        pick: () => "monitor-scale",
        pickKind: "select",
    },
];

/** 档位那一格用的档(都取自各侧 design-box.js 的档位表)。 */
const STEP = { output: 0.5, input: 0.75, monitor: 0.8 };

function assertClean(label) {
    eq(bucket.errors, [], `${label}:零 console.error`);
    eq(bucket.exceptions, [], `${label}:零未捕获异常`);
}

// ---------------------------------------------------------------- 跑
try {
    // --- 连上 CDP ---
    // 要的是**页面 target** 的 ws 端点:浏览器级端点(/json/version)上没有
    // Runtime/Page 域,`Runtime.enable` 会直接回「wasn't found」。
    let wsUrl = null;
    for (let i = 0; i < 60 && !wsUrl; i++) {
        try {
            const r = await fetch(`http://127.0.0.1:${CDP_PORT}/json/list`);
            const targets = await r.json();
            const page = targets.find((t) => t.type === "page");
            if (page) wsUrl = page.webSocketDebuggerUrl;
        } catch {
            /* 还没起来,下一轮再试 */
        }
        if (!wsUrl) await sleep(150);
    }
    if (!wsUrl) noBrowser("浏览器起来了但 CDP 端口没响应");
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

    for (const cfg of ROLES) {
        const { role, sel, box } = cfg;
        log(`\n=== ${role}(设计盒 ${box.w}×${box.h})===`);
        newBucket(role);
        await cdp.send("Page.navigate", { url: "about:blank" });
        await sleep(120);
        await cdp.send("Page.navigate", { url: `${base}/${cfg.page}` });

        // 装载判据取**壳页**的注入状态位:`data-ok="1"` 是 injectAndMount 在
        // `wired > 0` 之后置的,那时真源 app.js 的模块体(含 installShellFit)必然已求值。
        // 只等 `#card` 在不在**不够** —— 那个节点是 HTML 里写死的,app.js 还没跑就查得到,
        // 于是第一格量到的是「尚未落定」的空倍率(Monitor 上实测栽过)。
        const up = await waitFor(
            `(() => {
                const st = document.querySelector(".pv-status");
                const f = document.querySelector("iframe");
                const d = f && f.contentDocument;
                return !!(
                    st &&
                    st.getAttribute("data-ok") === "1" &&
                    d &&
                    d.querySelector(${JSON.stringify(sel)})
                );
            })()`,
            15000,
        );
        if (!check(up, `${role}:真源页面装载完成(mock 已注入)`)) continue;
        await twoFrames();

        // ---- ①②③ 视口栅格 -------------------------------------------------
        // 前三格同比例(倍率必然随尺寸变);第四格宽高比不匹配,专钉 min 那一步。
        const GRID = [
            { name: "设计盒", w: box.w, h: box.h, want: 1, bind: "both" },
            {
                name: "缩到 60%",
                w: Math.round(box.w * 0.6),
                h: Math.round(box.h * 0.6),
                want: 0.6,
                bind: "both",
            },
            {
                name: "放到 150%",
                w: Math.round(box.w * 1.5),
                h: Math.round(box.h * 1.5),
                want: 1.5,
                bind: "both",
            },
            {
                name: "宽 ×1.5 / 高 ×0.6(比例不匹配)",
                w: Math.round(box.w * 1.5),
                h: Math.round(box.h * 0.6),
                want: 0.6,
                bind: "h",
            },
        ];

        const seen = [];
        for (const g of GRID) {
            await evaluate(
                OUT(`
                f.style.width = ${g.w} + "px";
                f.style.height = ${g.h} + "px";
                return true;
            `),
            );
            const settled = await waitFor(
                IN(`return d.documentElement.clientWidth === ${g.w};`),
            );
            check(settled, `${role} / ${g.name}:视口改到 ${g.w}×${g.h}`);
            await twoFrames(); // 见 twoFrames 的注释:少这一步量到的是上一格的倍率

            const m = await evaluate(GEOM(sel));
            if (!check(m, `${role} / ${g.name}:量到几何`)) continue;
            seen.push(m.zoom);

            eq(m.zoom, String(g.want), `${role} / ${g.name}:倍率 = ${g.want}`);

            // ② 零溢出 —— 用户口中的「画面超出框大小」就是这四条里的任意一条不成立
            check(
                m.docScrollW <= m.vw,
                `${role} / ${g.name}:文档无横向溢出(scrollWidth ${m.docScrollW} ≤ clientWidth ${m.vw})`,
            );
            check(
                m.docScrollH <= m.vh,
                `${role} / ${g.name}:文档无纵向溢出(scrollHeight ${m.docScrollH} ≤ clientHeight ${m.vh})`,
            );
            check(
                m.bodyScrollW <= m.vw,
                `${role} / ${g.name}:body 无横向溢出(${m.bodyScrollW} ≤ ${m.vw})`,
            );
            check(
                m.bodyScrollH <= m.vh,
                `${role} / ${g.name}:body 无纵向溢出(${m.bodyScrollH} ≤ ${m.vh})`,
            );
            // 包围盒也不许越出视口(容 1px:zoom 后的亚像素舍入)
            check(
                m.left >= -1 &&
                    m.top >= -1 &&
                    m.left + m.width <= m.vw + 1 &&
                    m.top + m.height <= m.vh + 1,
                `${role} / ${g.name}:外壳包围盒落在视口内` +
                    `(${Math.round(m.left)},${Math.round(m.top)} ${Math.round(m.width)}×${Math.round(m.height)} vs ${m.vw}×${m.vh})`,
            );

            // ③ 贴边:短边**必须**贴满,否则「不会自动放大」原样复发
            if (g.bind === "both") {
                check(
                    m.width >= m.vw - 1 && m.height >= m.vh - 1,
                    `${role} / ${g.name}:两轴都贴边(${Math.round(m.width)}×${Math.round(m.height)} vs ${m.vw}×${m.vh})`,
                );
            } else {
                check(
                    m.height >= m.vh - 1,
                    `${role} / ${g.name}:高度贴边(${Math.round(m.height)} vs ${m.vh})`,
                );
                check(
                    m.width < m.vw - 1,
                    `${role} / ${g.name}:宽度留边而不是拉伸(${Math.round(m.width)} < ${m.vw})`,
                );
            }
        }
        // ① 三个同比例视口必须给出三个**互不相等**的倍率
        eq(
            new Set(seen.slice(0, 3)).size,
            3,
            `${role}:倍率随视口变化(实得 ${seen.slice(0, 3).join(" / ")})`,
        );

        // ---- ④ 档位仍然活着:真 UI → 壳页扮宿主改 iframe → 页内倍率跟上 ------
        await evaluate(
            OUT(`
            f.style.width = ${box.w} + "px";
            f.style.height = ${box.h} + "px";
            return true;
        `),
        );
        await sleep(150);

        const f0 = STEP[role];
        const pct = Math.round(f0 * 100);
        const gbName = cfg.pick(pct);
        const clicked = await evaluate(
            IN(`
            const el = gb(${JSON.stringify(gbName)});
            if (!el) return false;
            ${
                cfg.pickKind === "select"
                    ? `el.value = ${JSON.stringify(String(f0))};
                       el.dispatchEvent(new w.Event("change"));`
                    : `el.click();`
            }
            return true;
        `),
        );
        check(clicked, `${role}:选了 ${pct}% 档`);

        const wantW = Math.round(box.w * f0);
        const resized = await waitFor(
            OUT(
                `return Math.round(f.getBoundingClientRect().width) === ${wantW};`,
            ),
            4000,
        );
        await twoFrames();
        check(
            resized,
            `${role}:壳页(扮宿主)把窗口改成设计盒 × ${f0} = ${wantW}px 宽`,
        );

        const followed = await waitFor(
            IN(
                `return q(${JSON.stringify(sel)}).style.zoom === ${JSON.stringify(String(f0))};`,
            ),
            4000,
        );
        check(followed, `${role}:页内倍率跟到 ${f0}`);

        const m2 = await evaluate(GEOM(sel));
        if (m2) {
            check(
                m2.docScrollW <= m2.vw && m2.docScrollH <= m2.vh,
                `${role}:${pct}% 档下零溢出`,
            );
        }

        // 回到 100%(走同一条 UI 路径,顺便验回退方向也不溢出)
        await evaluate(
            IN(`
            const el = gb(${JSON.stringify(cfg.pick(100))});
            if (!el) return false;
            ${
                cfg.pickKind === "select"
                    ? `el.value = "1"; el.dispatchEvent(new w.Event("change"));`
                    : `el.click();`
            }
            return true;
        `),
        );
        const back = await waitFor(
            IN(`return q(${JSON.stringify(sel)}).style.zoom === "1";`),
            4000,
        );
        check(back, `${role}:回到 100% 档`);

        // ⑤
        assertClean(role);
    }
} catch (e) {
    fail++;
    console.log(`  [FAIL] 跑挂了:${e && e.stack ? e.stack : e}`);
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

log("");
if (fail === 0) {
    log("✅ smoke-shell-fit-page:全绿");
    process.exit(0);
}
log(`❌ smoke-shell-fit-page:${fail} 条断言失败`);
process.exit(1);
