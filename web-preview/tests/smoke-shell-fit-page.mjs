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
//   ⑤ 全程零 console.error、零未捕获异常;
//   ⑥(仅 Monitor)**先停掉帧流再改窗口**:轨迹图画布的后备存储 k 必须跟上。
//      ⚠ 这一格钉的是**不变量**(画布后备存储跟着外壳倍率走),**不是** onChange 的
//      删除式 —— 去掉 onChange 它照样绿(画布那一侧被 clientWidth 的取整巧合掩盖了);
//   ⑧(仅 Output,仅 60% 那一格)四个 tab 各查一遍零溢出 —— 缩得最小的那一档最容易
//      暴露「内容比框大」;不做四格视口 × 四 tab 的全乘,那只翻时长不换失效模式;
//   ⑦ **开窗即非 1 档**(`?scale=0.5`)→ 停帧 → 拉回设计尺寸:轨迹图必须失效一次。
//      读的是 Monitor 测试面的**失效计数**,不受 ⑥ 那条取整巧合掩盖 ⇒ 这一格才是
//      「onChange 在不在」与「倍率账初值取早了」两件事的删除式,两者都必红。
//
// 用法:node web-preview/tests/smoke-shell-fit-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;**2 = 环境里没有 Chrome/Edge**(口径同
//   smoke-monitor-page.mjs 与 CLAUDE.md §6:可选依赖缺席不判红,但也绝不算通过);
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
    // [SL-287] 每条 CDP 调用都要有截止时间:把 resolve 塞进 pending 就返回的原版,
    // 响应不来就**永远不 resolve**(SL-274 实测挂过 75 分钟零输出,Chrome 与 node 都还
    // 活着)。gate 3e 的页面级那一趟是持 `Local\SCVB-ipc-tests` 的 ⇒ 一套挂死堵住全场;
    // CI 上则一路烧到 job 超时。超时**抛错不重试** —— 响应不来说明渲染器已经不对了,
    // 重试只是把一个确定的红拖成一个更慢的红。错误里带 method 与 id,直接指到哪一条卡住。
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

// [SL-297] 三档要分开,别把中间那一档并进「缺依赖」:
//   2 = 本机根本没有 Chrome/Edge(可选依赖缺席,gates 打 SKIP);
//   3 = **浏览器是在的**,但这一次没起来 / 没连上(端口被占、机器负载、版本不对);
//   1 = 跑起来了但断言红。
// 把 3 并进 2 的后果是静默的:CI 与 gates 会把「这一套压根没验」当成「环境没有,跳过」。
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
chrome.on("error", (e) => browserFailed(`浏览器启动失败:${e.message}`));

let cdp = null;
let bucket = { label: "启动", errors: [], exceptions: [] };
const newBucket = (label) => {
    bucket = { label, errors: [], exceptions: [] };
};

// [SL-287] 收尾必须走**所有**退出路径,不只 happy path:漏掉的那些路径会把无头 Chrome
// 与临时 user-data 目录留在机器上(本机曾攒下近千个 scvb-* 残留目录),而且**没有任何
// 用例会因此变红** —— 所以钉成机检(scripts/check-smoke-hygiene.mjs)。
// 幂等:`exit` 与信号处理器可能都到,重复收尾不许炸。
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
// `exit` 处理器只能同步收尾(Node 规范):不等句柄、不重试 rmSync —— 留一个空壳目录
// 是可接受的残渣(系统会清),而**跑着的 headless Chrome 不是**。
process.on("exit", teardown);
for (const sig of ["SIGINT", "SIGTERM"]) {
    process.on(sig, () => {
        teardown();
        process.exit(130);
    });
}
// 未捕获异常 / 未处理拒绝:先打印再收尾,否则 Chrome 跟着一起漏。上面新加的 CDP 超时是
// **定时器里 reject**,那条 promise 当时若没人 await 就会以 unhandledRejection 到这里 ——
// 这一支不是摆设。
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
            // 上界 = **本次 waitFor 还剩多少预算**(留 250ms 收尾),不是一个写死的常数:
            // 写死会把「耗时落在常数与预算之间」的**合法**调用从过变成必红,等于凭空
            // 多出一类红。按剩余预算取则不改变任何原本能过的行为,真挂死仍被砍断。
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

/** 轨迹图画布的后备存储比值(见 ⑥ 的注释)。 */
const CANVAS_K = IN(`
    const c = gb("monitor-traj-canvas");
    if (!c) return null;
    const r = c.getBoundingClientRect();
    if (!(r.width > 0)) return null;
    return {
        ratio: c.width / r.width,
        dpr: w.devicePixelRatio || 1,
        zoom: (q("#card") || {}).style ? q("#card").style.zoom : "",
    };
`);

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
// 期望倍率**现算**,不写死成档位数字:宿主给的窗口是 `round(设计盒 × 档位)`
// (原生 designBoxWindowSize 与壳页 sizeFrame 同口径),不整除的档位下 fit 本来就
// 不等于档位 —— 例如 Input 的 0.33 档真机上是 152×185,fit = 0.3303。那不是回归,
// 「刚好装进实际窗口」才是正确行为。今天三个 STEP 恰好整除,写死也绿,但换一档就
// 会变成假红,而失败文案会指着一个不存在的问题。
const { fitFactor } = await import(
    new URL("../../web/shared/shell-fit.js", import.meta.url)
);
const expectedZoom = (box, f) =>
    fitFactor(Math.round(box.w * f), Math.round(box.h * f), box.w, box.h);

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

            // ⑧ Output 的四个 tab 各查一遍零溢出 —— **只在 60% 这一格**做。
            // 理由:溢出是「内容比框大」,最容易在缩得最小的那一档暴露;而四格视口
            // × 四个 tab 全乘等于把这一套的时长翻两番,换不来新的失效模式。
            // 四面板同在 DOM、靠 `#content[data-tab]` 切换,所以别的 tab 的内容在别的
            // 格里其实也参与了 body 的 scrollWidth —— 这里补的是「切过去之后各自的
            // 版面在 0.6 档下也不撑破」。
            if (role === "output" && g.want === 0.6 && g.bind === "both") {
                for (const tab of ["master", "tracks", "wave", "settings"]) {
                    const clicked = await evaluate(
                        IN(`
                        const t = gb("tabnav-${tab}");
                        if (!t) return false;
                        t.click();
                        return true;
                    `),
                    );
                    if (!check(clicked, `output / 60% / ${tab}:切到该 tab`)) {
                        continue;
                    }
                    await twoFrames();
                    const tm = await evaluate(GEOM(sel));
                    if (!check(tm, `output / 60% / ${tab}:量到几何`)) continue;
                    check(
                        tm.docScrollW <= tm.vw && tm.bodyScrollW <= tm.vw,
                        `output / 60% / ${tab}:无横向溢出` +
                            `(doc ${tm.docScrollW} / body ${tm.bodyScrollW} ≤ ${tm.vw})`,
                    );
                    check(
                        tm.docScrollH <= tm.vh && tm.bodyScrollH <= tm.vh,
                        `output / 60% / ${tab}:无纵向溢出` +
                            `(doc ${tm.docScrollH} / body ${tm.bodyScrollH} ≤ ${tm.vh})`,
                    );
                    eq(
                        tm.zoom,
                        String(g.want),
                        `output / 60% / ${tab}:切 tab 不改倍率`,
                    );
                }
                // 收尾切回第一个 tab,后面几格量的仍是同一个版面
                await evaluate(
                    IN(
                        `const t = gb("tabnav-master"); if (t) t.click(); return true;`,
                    ),
                );
                await twoFrames();
            }

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

        const wantZoom = String(expectedZoom(box, f0));
        const followed = await waitFor(
            IN(
                `return q(${JSON.stringify(sel)}).style.zoom === ${JSON.stringify(wantZoom)};`,
            ),
            4000,
        );
        check(
            followed,
            `${role}:页内倍率跟到窗口(${pct}% 档 ⇒ ${wantZoom},实得 ${await evaluate(IN(`return q(${JSON.stringify(sel)}).style.zoom;`))})`,
        );

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

        // ---- ⑥ 停帧之后改窗口:后备存储 k 必须跟上(只在 Monitor 跑)----------
        // 05 §6.1:`canvas.width = 本地 px × k`,`k = 外壳缩放 × dpr`。包围盒量的是
        // **视觉 px**(已含 zoom),两边的 zoom 正好约掉 ⇒ `canvas.width ÷ 包围盒宽`
        // 恒等于 dpr。倍率变了而 k 没跟上,这个比值立刻变成 `dpr ÷ 倍率`。
        //
        // **必须先把帧流停掉再量**:Monitor 每收到一帧 viz 都会 `traj.invalidate()`
        // 一次,25Hz 的帧流下「有没有那次显式重绘」在任何时刻都测不出差别 —— 我第一版
        // 正是在帧流开着的时候量的,量到「都一样」就把那句 invalidate 删了。判据不可
        // 分辨 ≠ 代码不需要。停帧靠壳页挂出来的 driver session(见 shell.js 的测试面),
        // 停的是 mock 的事件循环,页面侧一行都没动 —— 组仍在线、图仍在版面上,
        // 正是真机上「宿主侧门控把帧挡掉、而组还连着」的那一段。
        if (role === "monitor") {
            const stopped = await evaluate(
                `(() => {
                    const s = window.__SCVB_PREVIEW_SESSION__;
                    if (!s || typeof s.stop !== "function") return false;
                    s.stop();
                    return true;
                })()`,
            );
            check(stopped, "monitor:停掉预览 driver(帧流不再推 invalidate)");

            // 停帧后再等一会,确保在途的那几帧都落完
            await sleep(400);
            await twoFrames();

            const before = await evaluate(CANVAS_K);
            check(
                before && Math.abs(before.ratio - before.dpr) < 0.05,
                `monitor:停帧后基线 k 正确(实得 ${before ? before.ratio.toFixed(3) : "(缺)"},应为 ${before ? before.dpr : "?"})`,
            );

            const vw = Math.round(box.w * 0.6);
            const vh = Math.round(box.h * 0.6);
            await evaluate(
                OUT(`
                f.style.width = ${vw} + "px";
                f.style.height = ${vh} + "px";
                return true;
            `),
            );
            await waitFor(
                IN(`return d.documentElement.clientWidth === ${vw};`),
            );
            // 重建排在 invalidate 之后的下一帧,给它几帧;等不到就往下走,
            // 断言照样报实得值(不靠 waitFor 判红)。
            await twoFrames();
            await twoFrames();
            await sleep(300);

            // ⚠ 去掉 installShellFit 的 onChange,这一格**仍然绿**(实测:停帧 + 拆
            // onChange,resize 后 +300/+800/+2000ms 三次量到的比值都还是 dpr)。原因不是
            // 「不需要那个回调」,而是轨迹图父盒的 clientWidth 在 zoom 下**顺带**变了一点
            // (Chrome 152 实测 822 → 818:不是按倍率缩,是亚像素取整的偏移),于是它自己
            // 那个 `if (measure())` 的 ResizeObserver 恰好被叫醒。那是**取整的巧合**,不是
            // 不变量 —— 换一个恰好取整到同一个整数的版面就没有了,而那时画布会一直用旧 k。
            // 所以 onChange 留着(构造上正确),但**这一格**不冒充它的删除式 ——
            // 真正钉住 onChange 的是下面 ⑦:它读的是失效**计数**,不是画布像素,
            // 于是不受这条取整巧合的掩盖(拆掉 onChange,⑦ 必红)。
            const after = await evaluate(CANVAS_K);
            if (check(after, "monitor:停帧改窗口后量到轨迹图画布")) {
                check(
                    Math.abs(after.ratio - after.dpr) < 0.05,
                    `monitor:停帧改窗口后 k 跟上了` +
                        `(canvas.width ÷ 包围盒宽 = ${after.ratio.toFixed(3)},应为 dpr ${after.dpr};` +
                        `倍率 ${after.zoom})`,
                );
            }
        }

        // ⑤
        assertClean(role);
    }

    // ---- ⑦ 开窗即非 1 档:第一次拉回设计尺寸也必须让轨迹图失效 -----------------
    // 真机路径:`commitUiScale` 把档位落成系统级默认 ⇒ 用户存过 0.5 之后,下一次开窗
    // 宿主**一上来**给的就是设计盒×0.5。此时 installShellFit 的首帧 `apply` 是静默的
    // (只落样式、不回调),模块级倍率直接就是 0.5 —— 于是「倍率账的初值在安装**之前**
    // 取」这个写法会把账钉死在 1,而**第一次**把窗口拉回设计尺寸(量化后正好 1.00)就被
    // `f === lastBackingFit` 吞掉:轨迹图不失效,画布带着 0.5 的旧 k 被上采样。
    //
    // 判据读的是 `__SCVB_MONITOR__.snapshot().shellFitInvalidations`(onChange 真的失效了
    // 几次),**不是**画布像素:后者在这里测不出来 —— 轨迹图父盒的 clientWidth 在 zoom 下
    // 会顺带偏几个像素(Chrome 152 实测 822 → 818,亚像素取整),把它自己那个
    // `if (measure())` 的 ResizeObserver 恰好叫醒,于是「有没有这次失效」不可分辨。
    // 把初值那一行挪回安装之前,这一格必红。
    {
        newBucket("monitor 开窗即 0.5 档");
        const box = DESIGN.monitor;
        const F0 = 0.5; // 必须在 design-box.js 的 monitor 档位表内,否则壳页回落 1
        await cdp.send("Page.navigate", { url: "about:blank" });
        await sleep(120);
        await cdp.send("Page.navigate", {
            url: `${base}/web-preview/monitor.html?scale=${F0}`,
        });
        const ready = await waitFor(
            `(() => {
                const st = document.querySelector(".pv-status");
                const f = document.querySelector("iframe");
                const d = f && f.contentDocument;
                return !!(
                    st &&
                    st.getAttribute("data-ok") === "1" &&
                    d &&
                    d.querySelector("#card") &&
                    d.defaultView.__SCVB_MONITOR__
                );
            })()`,
            15000,
        );
        check(ready, "monitor?scale=0.5:装载完成");
        await twoFrames();

        const wantW = Math.round(box.w * F0);
        const framed = await evaluate(
            OUT(`return Math.round(f.getBoundingClientRect().width);`),
        );
        eq(framed, wantW, `monitor?scale=0.5:壳页开窗就给 ${wantW}px 宽`);
        eq(
            await evaluate(IN(`return q("#card").style.zoom;`)),
            String(F0),
            "monitor?scale=0.5:开窗倍率就是 0.5(首帧静默落的那一次)",
        );

        // 停帧:让「有没有 onChange」成为唯一变量
        check(
            await evaluate(
                `(() => {
                    const s = window.__SCVB_PREVIEW_SESSION__;
                    if (!s || typeof s.stop !== "function") return false;
                    s.stop();
                    return true;
                })()`,
            ),
            "monitor?scale=0.5:停掉预览 driver",
        );
        await sleep(300);

        const before = await evaluate(
            IN(`return w.__SCVB_MONITOR__.snapshot().shellFitInvalidations;`),
        );

        // 拉回设计尺寸 —— 量化后正好 1.00,正是被吞掉的那一次
        await evaluate(
            OUT(`
            f.style.width = ${box.w} + "px";
            f.style.height = ${box.h} + "px";
            return true;
        `),
        );
        await waitFor(IN(`return d.documentElement.clientWidth === ${box.w};`));
        await twoFrames();
        await twoFrames();

        eq(
            await evaluate(IN(`return q("#card").style.zoom;`)),
            "1",
            "monitor?scale=0.5:拉回设计尺寸后倍率 = 1",
        );
        const after = await evaluate(
            IN(`return w.__SCVB_MONITOR__.snapshot().shellFitInvalidations;`),
        );
        check(
            typeof before === "number" && typeof after === "number",
            `monitor?scale=0.5:读到失效计数(前 ${before} 后 ${after})`,
        );
        check(
            after > before,
            `monitor?scale=0.5:第一次拉回设计尺寸让轨迹图失效了` +
                `(计数 ${before} → ${after};倍率账初值若在安装前取,这一格必红)`,
        );
        assertClean("monitor 开窗即 0.5 档");
    }
} catch (e) {
    fail++;
    console.log(`  [FAIL] 跑挂了:${e && e.stack ? e.stack : e}`);
} finally {
    teardown();
}

log("");
if (fail === 0) {
    log("✅ smoke-shell-fit-page:全绿");
    process.exit(0);
}
log(`❌ smoke-shell-fit-page:${fail} 条断言失败`);
process.exit(1);
