// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output 撤销作用面 —— **页面级**冒烟(无头 Chrome + CDP;SL-450)
// -----------------------------------------------------------------------------
// 两个**今天就存在**的撤销缺陷,一套里一起守。两件都只在真 DOM 上分辨得出来,
// node 侧的纯函数 / 源码正则一条都证不出:
//
//   ① **Ctrl+Z 豁免对滑杆 / 勾选框过宽**(用户可感知)。
//      `app.js` 那道「焦点在文本输入框时不拦截」的闸原先只看 `tagName === "INPUT"`
//      ⇒ `<input type="range">`(曲线工具条 Q 滑杆)与 `<input type="checkbox">`
//      (自动停、导览「不再显示」)一并落进豁免,焦点停在它们身上按 Ctrl+Z 直接
//      漏给**宿主撤销栈**,到不了插件。判据面是 `el.closest(选择器)` 的 CSS 语义 ——
//      仓内零 node_modules、没有 jsdom,自己写个 `closest` 桩就是在测那个桩;
//      真语义只有真浏览器给得出。
//   ② **拖动中按 Ctrl+Z,撤销被松手那一下抹掉**。曲线编辑器在 pointerdown 时把点集
//      抄进 `local.dragPoints`,pointerup 提交的是**那份抄本**。要分辨它必须有真
//      pointer capture + 真的一记松手,而且要断的是「那一次提交**没有发生**」——
//      一件没发生的事:看画面分不出「没提交」与「提交了但 mock 回显还没到」,
//      故读 `__SCVB_OUTPUT__.curve().commits` 这个**计数**,并配一条「本该 +1」的
//      对照臂(同样的拖动不按 Ctrl+Z ⇒ commits 必须 +1)。没有对照臂的话,
//      这一套在「拖动根本没跑起来」时同样全绿。
//
// 跑什么(同一条会话上连续走完):
//   ① `?fixture=fifteen-tracks`(demo 快照:pan_curve 6 点、非只读)首帧;
//   ② **豁免矩阵 × 真 closest**:在页内 import 真的 `web/shared/context-menu.js`,
//      对**真造出来的**元素逐格判定 —— text / number / 缺省无 type 放行(回归格,
//      防止收窄把立意一起收掉),range / checkbox / select / contenteditable="false"
//      拦截;并对**生产页上那 10 个真控件**逐个核一遍(4 收回 + 6 保留);
//      外加 select 在右键那一侧仍放行(两处用途正当不同,别被「统一」掉);
//   ③ **真键路径**:CDP `Input.dispatchKeyEvent` 发真 Ctrl+Z,焦点先落到各类控件上
//      (发之前断言 `activeElement` 真的是它 —— 焦点没进去的话这一格测的是 `<body>`,
//      会静默变成假绿),读 `defaultPrevented` 判「拦没拦」;
//   ④ **拖动中 Ctrl+Z**:对照臂(不按 ⇒ commits +1)+ 实验臂(按 ⇒ commits 不变、
//      aborts +1、dragging=false);
//   ⑤ **拖动中换版本**:同一个 `abortEdit()` 的第二条触发路径 —— §1.17 的
//      `setPanCurve` 写「当前激活版本」、载荷不带版本号,不中止就是把 V1 的抄本
//      整表写进 V2;
//   ⑥ 每段零 console.error、零未捕获异常。
//
// 删除式网格(**注入未提交**,逐格实跑过;15 格全部按设计转红,且红在设计接住它的
// 那条断言上 —— 不是「红了就算」)。被注入的是**产品代码**,不是本文件:
//   白名单本体(8 格,红在 ②③):range / checkbox / select 各自放回文本族白名单;
//     删掉 `input:not([type])` / `input[type=number]` / `input[type=text]` 三条回归项;
//     contenteditable 三条换成裸 `[contenteditable]`(`="false"` 被放行);
//     `EDITABLE_SELECTOR` 丢掉 `, select`(拆分把右键那不该动的一半也动了)。
//   Ctrl+Z 闸接线(1 格,红在 ③):判据换回 `a.tagName === "INPUT"` 旧形态。
//   abortEdit 的四件事 + 两条触发路径(6 格,红在 ④④b⑤):摘掉 runHistory 里的
//     `abortEdit()`;`abortEdit()` 里逐条去掉 `dragging=false` / `clearTimeout` /
//     `releasePointerCapture` / `dragPoints=null`;摘掉 `render()` 里的版本闸。
// ⚠ 一条实测教训写在这里:`lostpointercapture` 是**排任务**派发的,不在
//   `releasePointerCapture()` 那一行同步发出 —— (d8b) 起初写成即刻读,结果被一个
//   与捕获毫无关系的注入(去掉 `dragPoints=null`)带红。现在是有界 waitFor。
//
// 用法:node web-preview/tests/smoke-undo-scope-page.mjs [仓库根绝对路径]
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-undo-scope-"));
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
        { expression, returnByValue: true, awaitPromise: true },
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

// 同上,但**异步**(要 await 页内 import)。
const IN_ASYNC = (js) => `(async () => {
    const f = document.querySelector("iframe");
    const w = f && f.contentWindow;
    const d = f && f.contentDocument;
    if (!w || !d) return null;
    const q = (s) => d.querySelector(s);
    const gb = (n) => q('[data-gb="' + n + '"]');
    ${js}
})()`;

// 首帧判据:曲线画布挂上了(mount 跑完会把 role/tabindex 写上去),
// 且诊断面已经挂出(app.js 末尾那句 `window.__SCVB_OUTPUT__ = {...}`)。
const READY = IN(`
    const c = gb("master-pancurve-canvas");
    return !!(c && c.getAttribute("role") === "application" &&
              w.__SCVB_OUTPUT__ && w.__SCVB_OUTPUT__.curve);
`);

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

// ---- 真 Ctrl+Z:走 CDP 的输入管线,不是页内 new KeyboardEvent -----------------
// `modifiers` 位 2 = Ctrl(CDP `Input.dispatchKeyEvent` 的约定)。
async function pressCtrlZ() {
    for (const type of ["keyDown", "keyUp"]) {
        await cdp.send("Input.dispatchKeyEvent", {
            type,
            modifiers: 2,
            key: "z",
            code: "KeyZ",
            windowsVirtualKeyCode: 90,
            nativeVirtualKeyCode: 90,
        });
    }
}

async function mouse(type, x, y, button) {
    await cdp.send("Input.dispatchMouseEvent", {
        type,
        x,
        y,
        button: button || "left",
        buttons: type === "mouseReleased" ? 0 : 1,
        clickCount: 1,
        pointerType: "mouse",
    });
}

const curveDiag = () => evaluate(IN(`return w.__SCVB_OUTPUT__.curve();`));

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
    log("=== ① 首帧(fixture=fifteen-tracks:pan_curve 6 点、非只读)===");
    newBucket("首帧");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?fixture=fifteen-tracks`,
    });
    check(await waitFor(READY), "页面装载、曲线编辑器已 mount、诊断面已挂出");

    // demo 快照的 `ui.tour_seen=false` ⇒ tour 询问卡连着一整块 `.sc-scrim` 罩在页上,
    // 真鼠标事件会全部落到那块罩子上、一记都到不了画布。先按「暂不」收掉它。
    // ⚠ 收完**必须**断 `elementFromPoint` 真的落回画布:少了这一断,④⑤ 里
    // 「pointerdown 没进拖动态」会长得和「中止逻辑起了作用」一模一样,
    // 而后者恰恰是本套要证的东西 —— 遮挡会把这一套整段变成假绿。
    check(
        await evaluate(
            IN(`const later = gb("tour-ask-later");
                if (later) later.click();
                const ov = gb("tour-ask");
                return !ov || ov.hidden === true;`),
        ),
        "tour 询问卡已收起(它的 scrim 会吃掉所有真鼠标事件)",
    );
    check(
        await waitFor(
            IN(`const c = gb("master-pancurve-canvas");
                if (!c) return false;
                const r = c.getBoundingClientRect();
                return d.elementFromPoint(r.left + r.width / 2,
                                          r.top + r.height / 2) === c;`),
            6000,
        ),
        "画布中心点此刻**命中画布本身**(无遮挡;④⑤ 的真鼠标事件由此才有意义)",
    );
    assertClean("① 首帧");

    // =========================================================================
    log("=== ② 豁免矩阵 × 真 closest(页内 import 真模块,判真造出来的元素)===");
    newBucket("豁免矩阵");
    {
        // 七格矩阵。前三格是**回归格** —— 收窄很容易把「焦点在文本框时别拦」这个
        // 立意一起收掉,那才是豁免存在的全部理由。
        const MATRIX = [
            ["input", { type: "text" }, true, "text 放行(立意本身)"],
            ["input", { type: "number" }, true, "number 放行(立意本身)"],
            ["input", {}, true, "缺省无 type 放行(缺省即 text)"],
            ["input", { type: "range" }, false, "range **拦截**(本卡新增行为)"],
            [
                "input",
                { type: "checkbox" },
                false,
                "checkbox **拦截**(本卡新增行为)",
            ],
            ["select", {}, false, "select **拦截**(下拉无文本撤销语义)"],
            [
                "div",
                { contenteditable: "false", tabindex: "0" },
                false,
                'contenteditable="false" 拦截(显式声明不可编辑)',
            ],
        ];
        const got = await evaluate(
            IN_ASYNC(`
            const m = await import("${base}/web/shared/context-menu.js");
            const cases = ${JSON.stringify(MATRIX.map(([tag, attrs]) => [tag, attrs]))};
            const out = [];
            for (const [tag, attrs] of cases) {
                const el = d.createElement(tag);
                for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, v);
                d.body.appendChild(el);
                out.push({
                    text: m.isEditableTextTarget(el),
                    any: m.isEditableTarget(el),
                });
                el.remove();
            }
            return out;
        `),
        );
        check(
            Array.isArray(got) && got.length === MATRIX.length,
            `矩阵取到 ${MATRIX.length} 格读数(实得 ${Array.isArray(got) ? got.length : got})`,
        );
        if (Array.isArray(got) && got.length === MATRIX.length) {
            MATRIX.forEach(([, , want, label], i) => {
                eq(got[i].text, want, `(a${i + 1})Ctrl+Z 豁免:${label}`);
            });
            // 右键那一侧:select **仍放行**(两处用途正当不同,别被「统一」掉)。
            // 这一格是本次拆分的唯一回归风险面,单独钉住。
            eq(
                got[5].any,
                true,
                "(a8)右键菜单仍放行 select —— 下拉的右键菜单归宿主系统,与文本撤销无关",
            );
            // 其余六格两侧同值 ⇒ 拆分没有改到右键侧任何别的判定。
            for (const i of [0, 1, 2, 3, 4, 6]) {
                eq(
                    got[i].any,
                    got[i].text,
                    `(a9.${i})除 select 外两侧判定逐格相同(第 ${i} 格)`,
                );
            }
        }

        // ---- 生产页上的真控件:4 个该收回 + 6 个该保留,逐个核 ----
        // 这一组与上面的矩阵**不重复**:矩阵钉的是「哪些 type 该拦」,这一组钉的是
        // 「本页面上那些控件确实是那些 type」——枚举漂了(有人把 Q 滑杆换成 number
        // 输入框、把勾选框换成 div)上面全绿、这里才红。
        const PROD = await evaluate(
            IN_ASYNC(`
            const m = await import("${base}/web/shared/context-menu.js");
            const pick = (sel) => {
                const el = q(sel);
                return el ? m.isEditableTextTarget(el) : "缺失";
            };
            return {
                // ---- 该收回(4)----
                qSlider: pick('[data-curve-q]'),
                autostop: pick('.wave-autostop__input'),
                dontShow: pick('[data-gb="guide-overlay-dontshow"]'),
                scaleSelect: pick('[data-gb="settings-scale-select"]'),
                // ---- 该保留(6:豁免的立意本身)----
                verRename: pick('.ver-rename__input'),
                tcStart: pick('[data-gb="master-range-start-bars"]'),
                tcEnd: pick('[data-gb="master-range-end-bars"]'),
                inspPan: pick('[data-gb="inspector-pan-input"]'),
                inspVol: pick('[data-gb="inspector-vol-input"]'),
                trackLabel: pick('.tracks-row__label-input'),
            };
        `),
        );
        // ⚠ 先断「十列都取到了布尔值」:任何一列写错选择器都会变成 "缺失",
        // 而 "缺失" 不等于 false —— 不先拦这一道,选择器打错字会让判据静默变空。
        for (const [k, v] of Object.entries(PROD || {})) {
            check(
                typeof v === "boolean",
                `(b0)生产控件 ${k} 在页面上找得到(实得 ${JSON.stringify(v)})`,
            );
        }
        for (const k of ["qSlider", "autostop", "dontShow", "scaleSelect"]) {
            eq(PROD[k], false, `(b1)生产控件 ${k}:Ctrl+Z **拦截**(本卡收回)`);
        }
        for (const k of [
            "verRename",
            "tcStart",
            "tcEnd",
            "inspPan",
            "inspVol",
            "trackLabel",
        ]) {
            eq(PROD[k], true, `(b2)生产控件 ${k}:Ctrl+Z 放行(文本族,保留)`);
        }
    }
    assertClean("② 豁免矩阵");

    // =========================================================================
    log("=== ③ 真键路径(CDP 发真 Ctrl+Z;先断焦点真的进去了)===");
    newBucket("真键路径");
    {
        // 探针挂在**冒泡**阶段的 document 上:产品那道闸是 capture 阶段的,
        // 冒泡这一轮跑在它之后,读到的 defaultPrevented 才是它的处置结果。
        await evaluate(
            IN(`
            if (!w.__undoScopeProbe) {
                w.__undoScopeProbe = { seen: 0, prevented: null, active: "" };
                d.addEventListener("keydown", (ev) => {
                    if (!(ev.ctrlKey || ev.metaKey)) return;
                    if (String(ev.key).toLowerCase() !== "z") return;
                    w.__undoScopeProbe.seen++;
                    w.__undoScopeProbe.prevented = ev.defaultPrevented;
                }, false);
            }
            return true;
        `),
        );

        const KEYS = [
            ["input", { type: "text" }, true, "text:不拦(留给文本框自己撤销)"],
            ["input", { type: "range" }, false, "range:**拦截**"],
            ["input", { type: "checkbox" }, false, "checkbox:**拦截**"],
            ["select", {}, false, "select:**拦截**"],
        ];
        for (const [tag, attrs, expectPass, label] of KEYS) {
            // 造元素 + 真 focus;**发键之前**断言 activeElement 就是它 ——
            // 焦点没进去的话这一格测的是 <body>(恒被拦),会静默假绿。
            const focused = await evaluate(
                IN(`
                const el = d.createElement(${JSON.stringify(tag)});
                for (const [k, v] of Object.entries(${JSON.stringify(attrs)}))
                    el.setAttribute(k, v);
                el.id = "__undoScopeProbeEl";
                d.body.appendChild(el);
                w.focus();
                el.focus();
                w.__undoScopeProbe.seen = 0;
                w.__undoScopeProbe.prevented = null;
                w.__undoScopeProbe.active = d.activeElement === el
                    ? "ok"
                    : (d.activeElement ? d.activeElement.tagName : "null");
                return w.__undoScopeProbe.active;
            `),
            );
            check(
                focused === "ok",
                `(c0)${label}:焦点真的落到了该控件上(实得 ${focused})`,
            );
            await pressCtrlZ();
            const probe = await evaluate(
                IN(`
                const el = d.getElementById("__undoScopeProbeEl");
                if (el) el.remove();
                return { ...w.__undoScopeProbe };
            `),
            );
            // 键真的到页面了吗 —— 到不了的话 prevented 恒 null,四格会一起「假通过」。
            check(
                probe.seen === 1,
                `(c1)${label}:真 Ctrl+Z 到达页面一次(实得 seen=${probe.seen})`,
            );
            eq(
                probe.prevented,
                !expectPass,
                `(c2)${label}:${expectPass ? "不 preventDefault(漏给文本框)" : "preventDefault(不漏给宿主撤销栈)"}`,
            );
        }
    }
    assertClean("③ 真键路径");

    // =========================================================================
    log("=== ④ 拖动中 Ctrl+Z ⇒ 中止在飞拖动,松手不再提交陈旧抄本 ===");
    newBucket("拖动中 Ctrl+Z");
    {
        // 画布上一个**一定命中**的点:直接问页面「第 3 个点此刻画在哪」。
        // 逻辑→CSS 的换算走真模块(PLOT_W / angleToX / dbToY),不在测试里抄第二份。
        const where = await evaluate(
            IN_ASYNC(`
            const m = await import("${base}/web/output/canvas/curve-editor.js");
            const c = gb("master-pancurve-canvas");
            const r = c.getBoundingClientRect();
            const fr = f.getBoundingClientRect();
            // 点集从**下行状态**取:预览 mock 的 demo 快照是 6 点,取中间那个
            // (angle=0, gain_db=0)——离两侧邻点最远,命中最稳。
            const p = { angle: 0, gain_db: 0 };
            return {
                x: fr.left + r.left + (m.angleToX(p.angle) / m.PLOT_W) * r.width,
                y: fr.top + r.top + (m.dbToY(p.gain_db) / m.PLOT_H) * r.height,
                w: r.width,
                h: r.height,
            };
        `),
        );
        check(
            where && where.w > 50 && where.h > 20,
            `画布已布局出真实尺寸(实得 ${JSON.stringify(where)})`,
        );

        // ---- 对照臂:同样的拖动,**不按** Ctrl+Z ⇒ commits 必须 +1 ----------
        // 没有这一臂,整个 ④ 在「拖动压根没跑起来」时同样全绿 —— 那时实验臂的
        // 「commits 没变」是因为什么都没发生,不是因为中止逻辑起了作用。
        const before0 = await curveDiag();
        await mouse("mousePressed", where.x, where.y);
        const dragging0 = await curveDiag();
        check(
            dragging0.dragging === true,
            `(d1)对照臂:pointerdown 后确实进了拖动态(实得 ${JSON.stringify(dragging0)})`,
        );
        await mouse("mouseMoved", where.x + 30, where.y - 18);
        await mouse("mouseReleased", where.x + 30, where.y - 18);
        const after0 = await curveDiag();
        eq(
            after0.commits - before0.commits,
            1,
            "(d2)对照臂:松手提交了一次(这一臂证明本套看得见一次提交)",
        );
        eq(after0.aborts - before0.aborts, 0, "(d3)对照臂:零中止");

        // mock 的 §2.1 回声是**异步**的(SL-357)——等它落地再算下一臂的坐标。
        check(
            await waitFor(
                IN(`return w.__SCVB_OUTPUT__.curve().hasPreview === false;`),
                5000,
            ),
            "(d4)对照臂:回显到位,本地预览态已清",
        );

        // ---- 实验臂:拖到一半按 Ctrl+Z ⇒ 中止;随后那记松手不许再提交 --------
        const where2 = await evaluate(
            IN_ASYNC(`
            const m = await import("${base}/web/output/canvas/curve-editor.js");
            const c = gb("master-pancurve-canvas");
            const r = c.getBoundingClientRect();
            const fr = f.getBoundingClientRect();
            // 换一个**上一臂没碰过**的点(demo 快照第 5 点:angle=28, gain_db=1.8)。
            // 不去追上一臂那个点的新位置:它经 movePointTo 归一化+重排之后,落点要
            // 反推才知道,而反推错了表现出来就是「没进拖动态」—— 与本格要测的
            // 「中止生效」长得一样。换个干净的点,歧义就不存在。
            const p = { angle: 28, gain_db: 1.8 };
            return {
                x: fr.left + r.left + (m.angleToX(p.angle) / m.PLOT_W) * r.width,
                y: fr.top + r.top + (m.dbToY(p.gain_db) / m.PLOT_H) * r.height,
            };
        `),
        );
        const before1 = await curveDiag();
        // 指针捕获的释放是 abortEdit 的第二件事,而它在「提交没提交」上看不出来:
        // 隐式释放在 pointerup 时也会发生,所以只看结果分辨不出有没有显式放。
        // 闩住 `lostpointercapture` 并在**松手之前**读它 —— 这个时刻只有显式释放
        // 才可能让它合上。
        await evaluate(
            IN(`const c = gb("master-pancurve-canvas");
                w.__undoScopeLostCapture = false;
                if (!c.__undoScopeCapHook) {
                    c.__undoScopeCapHook = true;
                    c.addEventListener("lostpointercapture", () => {
                        w.__undoScopeLostCapture = true;
                    });
                }
                return true;`),
        );
        await mouse("mousePressed", where2.x, where2.y);
        const dragging1 = await curveDiag();
        check(
            dragging1.dragging === true,
            `(d5)实验臂:pointerdown 后确实进了拖动态(实得 ${JSON.stringify(dragging1)})`,
        );
        await mouse("mouseMoved", where2.x + 25, where2.y - 12);
        const midCommits = (await curveDiag()).commits;

        await pressCtrlZ(); // ← 拖动中按下

        const aborted = await curveDiag();
        eq(aborted.dragging, false, "(d6)Ctrl+Z ⇒ 在飞拖动被中止(dragging)");
        eq(aborted.hasPreview, false, "(d7)Ctrl+Z ⇒ 本地预览抄本被丢弃");
        eq(
            aborted.aborts - before1.aborts,
            1,
            "(d8)Ctrl+Z ⇒ abortEdit() 确实认领了这一次(计数 +1)",
        );

        // ⚠ 用**有界 waitFor**、不用即刻读:`lostpointercapture` 按规范是**排一个任务**
        // 派发的,不在 releasePointerCapture() 那一行同步发出。即刻读会把「事件还在队列里」
        // 读成「没放捕获」—— 删除式网格里一个与捕获无关的注入(去掉丢弃抄本那一行)
        // 就把它带红过一次,那是假红。仍然是有牙的断言:真不放捕获的话,松手之前这个
        // 闩锁永远合不上,3s 到点转红(C4 那一格实测如此)。
        check(
            await waitFor(
                IN(`return w.__undoScopeLostCapture === true;`),
                3000,
            ),
            "(d8b)Ctrl+Z ⇒ 指针捕获已被显式放掉(**松手之前**就判定,隐式释放还没轮到)",
        );

        // 关键的一记:松手。旧行为在这里把陈旧抄本整表提交上去,把 undo 抹掉。
        await mouse("mouseReleased", where2.x + 25, where2.y - 12);
        await sleep(200); // 给「若真发生了提交」一个上屏窗口,避免读得太早
        const after1 = await curveDiag();
        eq(
            after1.commits - midCommits,
            0,
            "(d9)**中止之后那一记松手零提交** —— 陈旧抄本没有被写回去(缺陷一的落点)",
        );
    }
    assertClean("④ 拖动中 Ctrl+Z");

    // =========================================================================
    log("=== ④b Q 滑杆的 140ms 防抖提交 ⇒ Ctrl+Z 同样要把它掐掉 ===");
    newBucket("Q 滑杆防抖");
    {
        // `local.dragPoints` 不只被拖动写:Q 滑杆(与键盘微调)也写它,并挂一个
        // 140ms 的防抖 `commit(next)`。那个定时器拿的是**闭包里捕获的** next,
        // 不读 `local.dragPoints` —— 所以 abortEdit 里只把抄本置空是不够的,
        // 不 clearTimeout 的话「拨完滑杆 140ms 内按 Ctrl+Z」原样复现缺陷一。
        // 这一格就是钉那一行 clearTimeout 的;没有它,那一行删掉也没人会红。
        const ready = await evaluate(
            IN(`const qs = q('[data-curve-q]');
                const wrap = q('.curve-toolbar__q');
                if (!qs || !wrap || wrap.hidden) return "工具条 Q 滑杆不可用";
                return "ok";`),
        );
        check(ready === "ok", `(f0)Q 滑杆此刻在工具条上(实得 ${ready})`);

        if (ready === "ok") {
            const bump = (v) =>
                evaluate(
                    IN(`const qs = q('[data-curve-q]');
                        qs.value = ${JSON.stringify(String(v))};
                        qs.dispatchEvent(new w.Event("input", { bubbles: true }));
                        return qs.value;`),
                );

            // ---- 对照臂:拨一下、**不按** Ctrl+Z ⇒ 140ms 后必须提交一次 ----
            const b0 = await curveDiag();
            await bump(2.0);
            await sleep(400); // > 140ms 防抖窗
            eq(
                (await curveDiag()).commits - b0.commits,
                1,
                "(f1)对照臂:拨完滑杆,防抖窗到点提交一次(证明本格看得见这条路径)",
            );

            // ---- 实验臂:拨一下、140ms 内按 Ctrl+Z ⇒ 那次提交必须不发生 ----
            const b1 = await curveDiag();
            await bump(3.0);
            await pressCtrlZ();
            await sleep(400); // 把防抖窗整段走完,给「若没掐掉」一个开火机会
            eq(
                (await curveDiag()).commits - b1.commits,
                0,
                "(f2)**Ctrl+Z 掐掉了在飞的防抖提交** —— 少一行 clearTimeout 这里就红",
            );
        }
    }
    assertClean("④b Q 滑杆防抖");

    // =========================================================================
    log("=== ⑤ 拖动中换版本 ⇒ 同样中止(否则把 V1 的抄本写进 V2)===");
    newBucket("拖动中换版本");
    {
        const where3 = await evaluate(
            IN_ASYNC(`
            const m = await import("${base}/web/output/canvas/curve-editor.js");
            const c = gb("master-pancurve-canvas");
            const r = c.getBoundingClientRect();
            const fr = f.getBoundingClientRect();
            // 上面两臂都没动 angle=-72 那个点,拿它当锚。
            return {
                x: fr.left + r.left + (m.angleToX(-72) / m.PLOT_W) * r.width,
                y: fr.top + r.top + (m.dbToY(-2.5) / m.PLOT_H) * r.height,
            };
        `),
        );
        const before2 = await curveDiag();
        eq(before2.activeVersion, 1, "(e0)起手停在 V1");
        await mouse("mousePressed", where3.x, where3.y);
        const dragging2 = await curveDiag();
        check(
            dragging2.dragging === true,
            `(e1)pointerdown 后确实进了拖动态(实得 ${JSON.stringify(dragging2)})`,
        );
        eq(dragging2.dragVersion, 1, "(e2)抄本记下了它属于 V1");
        await mouse("mouseMoved", where3.x + 20, where3.y - 10);

        // 换版本。用 chip 的 click()(直达 switchVersion):此刻画布持着 pointer
        // capture,真鼠标点不到 header —— 而本格要测的是**中止逻辑**,不是点击路由。
        check(
            await evaluate(
                IN(`const c2 = gb("header-version-chip-2");
                    if (!c2 || c2.getAttribute("data-disabled") === "1") return false;
                    c2.click();
                    return true;`),
            ),
            "(e3)点到了 V2 chip(未被置灰)",
        );
        check(
            await waitFor(
                IN(`return w.__SCVB_OUTPUT__.curve().activeVersion === 2;`),
                6000,
            ),
            "(e4)激活版本已切到 V2(§2.1 回声到位)",
        );
        // ⚠ 不能读完 activeVersion 就立刻断 dragging:版本闸落在 `render()` 里,
        // 而 render 是 **rAF 合帧**的 —— store 已是新版本、render 还没轮到跑,
        // 这中间有一整帧的窗口。这里等的是**效果**本身,并给一个有界超时:
        // 闸被删掉的话 dragging 会一直是 true,这条 waitFor 就超时转红(非空断言)。
        check(
            await waitFor(
                IN(`return w.__SCVB_OUTPUT__.curve().dragging === false;`),
                6000,
            ),
            "(e5)换版本 ⇒ 在飞拖动被中止(等 render() 那一帧,上界 6s)",
        );
        const afterSwitch = await curveDiag();
        eq(afterSwitch.hasPreview, false, "(e6)换版本 ⇒ 本地预览抄本被丢弃");
        eq(
            afterSwitch.aborts - before2.aborts,
            1,
            "(e7)换版本 ⇒ abortEdit() 认领了这一次(计数 +1)",
        );

        const mid2 = afterSwitch.commits;
        await mouse("mouseReleased", where3.x + 20, where3.y - 10);
        await sleep(200);
        const after2 = await curveDiag();
        eq(
            after2.commits - mid2,
            0,
            "(e8)**换版本后那一记松手零提交** —— V1 的抄本没有被写进 V2",
        );
    }
    assertClean("⑤ 拖动中换版本");
} catch (e) {
    fail++;
    console.log(`  [FAIL] 冒烟过程抛错:${e && e.message ? e.message : e}`);
} finally {
    teardown();
}

if (fail > 0) {
    console.log(`\n❌ ${fail} 条断言失败`);
    process.exit(1);
}
console.log("\n✅ Output 撤销作用面(SL-450)页面级冒烟全绿");
process.exit(0);
