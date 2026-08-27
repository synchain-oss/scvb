// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output 分布图 rAF 补间 —— **页面级**冒烟(无头 Chrome + CDP;SL-203)
// -----------------------------------------------------------------------------
// 用户实测:Monitor 上了帧间补间之后,观感反超 Output。本套守 Output 侧接同一件
// (`web/shared/dist-motion.js`)之后的两件事 —— 它们都是 node 侧断言不到的:
//
//   ① **渲染循环真的是 rAF 驱动**,不是「收到事件才画」。判据 = 补间器的帧计数
//      (`__SCVB_OUTPUT__.distMotion().frames`):事件驱动的实现里它**恒为 0**。
//      不去赌某次采样恰好落在动画中段 —— 靠撞上的覆盖等于没有覆盖;
//   ② **触达验证**:插出来的中间值必须真的落到柱子的**渲染位置**上。读的是
//      `getBoundingClientRect()`,不是 inline 的 `--x`。这一条的方法学来自 SL-192:
//      那边只断 inline 变量,结果漏掉了「节点常驻后 `transition: all` 活过来、在补间
//      下游再叠 ~300ms 低通」——补间逻辑全绿而屏幕上的柱子滞后 2.89 个百分点。
//      **写入面的断言证明不了渲染面**,这是本套存在的全部理由。
//
// 外加空闲零 rAF(05 §6.1):切到轨迹档 / 切走 Tab1 时一帧都不许跑 —— Output 的
// `scvb.params` 仍以 25Hz 推着 render,少一道闸就是对着没人看的画面烧 60fps 循环。
//
// 用法:node web-preview/tests/smoke-output-dist-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;**2 = 环境里没有 Chrome/Edge**(口径同
//   smoke-monitor-page.mjs 与 CLAUDE.md §6:可选依赖缺席不判红,但也绝不算通过)。
//
// CDP 连接那 30 行与 smoke-output-stale-page.mjs / smoke-monitor-page.mjs 同源
// (node 内置 fetch + WebSocket,零依赖 —— 仓库红线是不引 puppeteer)。同样没有抽
// 公共模块:三套各断各的面,合并只会让彼此被对方的参数面绑住。
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-output-dist-"));
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

async function waitFor(expr, ms = 15000) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {
        let v = null;
        try {
            v = await evaluate(expr);
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
    const all = (s) => Array.from(d.querySelectorAll(s));
    ${js}
})()`;

// 页面已经吃到首帧、且分布图已经出柱的判据(补间器的诊断面也已挂出)。
const READY = IN(`
    const m = w.__SCVB_OUTPUT__;
    return !!(m && m.distMotion() && all(".dist-bar").length > 0);
`);

// 注:脚手架里那对 `PROBE` / `open()` 是从 smoke-output-stale-page 照抄来的,
// 探的是那套的提示节点,本套一次没调 —— 已删,免得下一个人以为它们还有用。

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
            await sleep(CDP_WAIT_STEP_MS);
        }
    }
    if (!targets) {
        noBrowser(
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
    log("=== ① Output 分布图:rAF 驱动,不是收到事件才画(SL-203)===");
    {
        newBucket("connected");
        await cdp.send("Page.navigate", {
            // 用**已接线**的场景名(SCENARIO_MAP 里确有 `connected` → fifteen-tracks)。
            // 曾经写的是 `?scenario=printing` —— 那个名字在 shell 白名单里有、
            // SCENARIO_MAP 里没有,会静默回落并 console.warn;功能上恰好够用,
            // 但场景名与实得世界对不上。本套的数据面由探针自己用 setTrackManual 驱动,
            // 不依赖任何走带状态。
            url: `${base}/web-preview/output.html?scenario=connected`,
        });
        const up = await waitFor(READY);
        check(up, "页面装载并吃到首帧");

        // **把数据面驱动起来 —— 走真实用户路径**:`setTrackManual(ch,"pan",v)` 就是用户把某轨
        // 声像设成手动值时 UI 发的那一个上行调用(契约 §1.16)。mock 收到后同拍补一帧
        // `scvb.params`,于是 render → renderDist → push,补间器拿到新的 pan。
        //
        // 为什么不是 `setParam`:pan/vol **刻意不可经 setParam 写**(`isWritableParamId` 里
        // 那条 `!PAN_OR_VOL_ID.test(id)`)——它们是引擎所有的维度,手动接管走 §1.16。
        // 为什么不是「打开输出让引擎打印头推」:那条要 PRINT 三与条件同时成立
        // (输出 ON ∧ 播放中 ∧ 在 range 内),把本套的前置动作绑在另外几张卡的行为面上。
        const drive = async (pan) =>
            evaluate(
                IN(`
            const mk = w.__SCVB_MOCK__;
            if (mk && typeof mk.setTrackManual === "function") {
                mk.setTrackManual(1, "pan", ${pan});
            }
            return true;
        `),
            );
        await drive(-80);
        await sleep(150);
        await drive(80);
        const moving = await waitFor(
            IN(`
            const m = w.__SCVB_OUTPUT__;
            const dg = m && m.distMotion();
            return !!(dg && dg.pushes >= 2);
        `),
            6000,
        );
        check(
            moving,
            "setTrackManual 真的把新 pan 推到了补间器(收到 ≥2 帧新值)",
        );

        // 分布图是 Tab1 的默认视图(CHART_MODE_DEFAULT = "distribution"),开箱即在前台。
        const bars0 = await evaluate(IN(`return all(".dist-bar").length;`));
        check(bars0 > 0, `分布图有柱子(实得 ${bars0} 根)`);

        // ---- 判据一:**渲染循环是 rAF 驱动的**。
        // `dist.frames` 是补间循环的帧计数 —— 事件驱动的实现里它恒为 0,这是两者最干脆的
        // 分界(与 SL-192 在 Monitor 侧同一方法学)。这里不赌某次采样恰好落在动画中段:
        // 探针**自己边驱动边采样** —— 每 200ms 把轨 1 的 pan 在 ±80 之间来回搬,
        // 25ms 采一次。于是「有没有新值」这个前提由探针自己保证,不看 mock 的走带心情。
        const probe = await evaluate(
            IN(`
            return new Promise((res) => {
                const mk = w.__SCVB_MOCK__;
                const out = [];
                const t0 = performance.now();
                let flip = 0;
                let lastDrive = 0;
                const step = () => {
                    const now = performance.now();
                    if (now - lastDrive >= 200) {
                        lastDrive = now;
                        flip = 1 - flip;
                        if (mk && typeof mk.setTrackManual === "function") {
                            mk.setTrackManual(1, "pan", flip ? 80 : -80);
                        }
                    }
                    const m = w.__SCVB_OUTPUT__;
                    const dg = m && m.distMotion ? m.distMotion() : null;
                    const cont = q(".dist-bars");
                    const bars = all(".dist-bar");
                    let drift = -1;
                    if (cont && bars.length) {
                        const cr = cont.getBoundingClientRect();
                        let mx = 0;
                        for (const b of bars) {
                            const written = parseFloat(
                                b.style.getPropertyValue("--x"),
                            );
                            const r = b.getBoundingClientRect();
                            const rendered =
                                ((r.left + r.width / 2 - cr.left) / cr.width) *
                                100;
                            if (Number.isFinite(written) && cr.width > 0) {
                                mx = Math.max(mx, Math.abs(rendered - written));
                            }
                        }
                        drift = mx;
                    }
                    out.push({
                        frames: dg ? dg.frames : -1,
                        pushes: dg ? dg.pushes : -1,
                        shown: dg ? JSON.stringify(dg.shown) : "",
                        target: dg ? JSON.stringify(dg.target) : "",
                        drift,
                    });
                    if (now - t0 >= 3000) res(out);
                    else setTimeout(step, 25);
                };
                step();
            });
        `),
        );
        const first = probe[0];
        const last = probe[probe.length - 1];
        check(
            first.frames >= 0,
            "页面挂出了补间诊断面(__SCVB_OUTPUT__.distMotion)",
        );
        const pushes = last.pushes - first.pushes;
        const frames = last.frames - first.frames;
        // 前提:这 3 秒里数据面真的动过(mock 的引擎在推 params)。没动过的话下面两条
        // 等于没测,故先把前提断死。
        check(
            pushes >= 2,
            `3 秒内至少两帧带来新值(实得 ${pushes} —— 为 0 说明数据面没动,下面的断言不作数)`,
        );
        // ⚠ 判据必须**与机器速度无关**。这里原本写的是 `frames >= pushes * 3`
        // (「每帧数据铺成至少 3 帧渲染」)—— 那是按帧率判红,CI 的无头 Chrome 上
        // rAF 节奏比本机慢,实得 23 渲染帧 / 15 数据帧就红了,而代码完全正确。
        // **这正是我在 `[rate]` 用例里刚讲过的坑,自己又在这儿踩了一次。**
        //
        // 换成按**构造**成立的两侧夹(下面这条 + ② 节那条),两条都与帧率无关:
        //   • 可见且数据在动 ⇒ `frames` **严格 > 0**:每一次「值真的变了」的 push 都会
        //     `start()` 一个 rAF 循环,循环至少跑一帧。事件驱动的实现里这个计数**恒为 0**
        //     (它只在循环里自增),所以 >0 本身就是「rAF 驱动」的完整判据;
        //   • 不可见且数据在动 ⇒ `frames` **恒等于 0**(② 节)。
        // 两条合起来既证明循环真的在跑,又证明它真的被可见性闸住 —— 而快慢机上都一样。
        //
        // 补间的**数值正确性**(半程、到位、封顶不外推、时长定值……)归 node 侧
        // `smoke-monitor.mjs` ⑨ 节:那边用**注入的逻辑时钟**驱动 `tick()`,确定且与
        // 机器无关。页面级只回答「循环是不是真的在跑、写出去的东西有没有到屏幕上」。
        check(
            frames > 0,
            `补间循环真的在跑(实得 ${frames} 渲染帧 / ${pushes} 数据帧;事件驱动时恒为 0)`,
        );

        // ---- 判据二:补间中段的采样数 —— **只打印,不判红**。
        // 「有没有采到 shown ≠ target」取决于 rAF 周期与补间时长(40ms)的相对快慢:
        // 机器慢到 rAF 周期 ≥ 补间时长时,第一帧就 p=1,永远采不到中段 —— 那时代码没错,
        // 是那台机器本来就补不出中间帧。拿它判红就是又一条按帧率判红的断言。
        // 留作排障读数:红了的时候它能一眼分清「循环没跑」和「循环跑了但一步到位」。
        const midFlight = probe.filter(
            (p) => p.shown && p.target && p.shown !== p.target,
        ).length;
        log(
            `  (补间中段采样 ${midFlight} / ${probe.length};渲染帧 ${frames} / 数据帧 ${pushes})`,
        );

        // ---- 判据三(**触达验证**,方法学复用 SL-192):插出来的中间值必须真的落到柱子的
        // 位置上,而不是被 CSS 过渡糊住。读的是 getBoundingClientRect,不是 inline 变量 ——
        // 后者只能证明「我写进去了」,证明不了「屏幕上在哪」。Monitor 侧就是这条把
        // `transition: all` 那个坑抓出来的(实测滞后 2.89 个百分点)。
        const maxDrift = Math.max(...probe.map((p) => p.drift));
        check(
            maxDrift >= 0 && maxDrift < 1.5,
            `柱子渲染位置紧跟写入值,没有被 CSS 过渡拖住(最大偏差 ${maxDrift.toFixed(2)} 个百分点,阈值 1.5)`,
        );
        assertClean("connected 分布图补间");
    }

    // =========================================================================
    log("=== ② 空闲零 rAF:切到轨迹档 / 切走 Tab1 都不许空转(05 §6.1)===");
    {
        // 切到轨迹视图 ⇒ 分布图不可见 ⇒ 补间器一帧都不许跑。
        // 这条在 Output 侧是实打实的:scvb.params 仍以 25Hz 推着 render,少一道闸就是
        // 对着没人看的画面烧一条 60fps 循环。
        const switched = await evaluate(
            IN(`
            const b = q('[data-chart-mode="trajectory"]');
            if (!b) return false;
            b.click();
            return true;
        `),
        );
        check(switched, "点到「轨迹」视图");
        await sleep(600); // 让在途的那一次补间收手

        // ⚠ 判据必须是「**隐藏期数据仍在变**,而 rAF 一帧不跑」。
        // 只静置不驱动的话,这条测的是「值没变就不起帧」——那是另一条纪律,
        // 摘掉可见性闸它照样绿(反向验证抓到过:`isVisible: () => true` 时本条不红)。
        // Output 侧的真实风险恰恰是「切走了但 scvb.params 还在 25Hz 推」,
        // 故这里边隐藏边驱动。
        const spin = await evaluate(
            IN(`
            return new Promise((res) => {
                const mk = w.__SCVB_MOCK__;
                const m = w.__SCVB_OUTPUT__;
                const f0 = m.distMotion().frames;
                const p0 = m.distMotion().pushes;
                const t0 = performance.now();
                let flip = 0;
                let lastDrive = 0;
                const step = () => {
                    const now = performance.now();
                    if (now - lastDrive >= 150) {
                        lastDrive = now;
                        flip = 1 - flip;
                        if (mk && typeof mk.setTrackManual === "function") {
                            mk.setTrackManual(1, "pan", flip ? 70 : -70);
                        }
                    }
                    if (now - t0 >= 1200) {
                        const d = m.distMotion();
                        res({ frames: d.frames - f0, pushes: d.pushes - p0 });
                    } else setTimeout(step, 25);
                };
                step();
            });
        `),
        );
        eq(
            spin.frames,
            0,
            `轨迹档下即便数据仍在变,也一帧 rAF 都不许跑(实得 ${spin.frames} 帧)`,
        );

        // 切回分布档:必须重新出图(隐藏期把结构指纹清了,回来走重建分支)。
        await evaluate(
            IN(`
            const btn = q('[data-chart-mode="distribution"]');
            if (btn) btn.click();
            return true;
        `),
        );
        await sleep(600);
        // ⚠ 这里**不能**数 DOM 里的柱子:隐藏期只清了补间器的内部状态,DOM 从没被清空,
        // 所以 `all(".dist-bar").length > 0` 恒真 —— 重建分支一步不跑它也绿。
        // (旧版就是这么写的,等于一条永远不会红的断言。)
        //
        // 真正该守的性质是:**转一圈回来之后,屏幕上画的是「现在」的数据,不是隐藏前那份**。
        // 上面隐藏期把 pan 一直搬到 ±70,所以只要重新接管这一步没做对,柱子就会停在
        // 隐藏前的位置上。判据仍用空间量尺(渲染位置 vs 写入值),与帧率无关。
        //
        // 老实说一句它的**强度边界**:这是**性质守卫**,不是变异诱饵。补间器回到可见有
        // 两条冗余的追平路径(重建分支;以及「值变了 ⇒ start → tick → paint」这条普通路径),
        // 把其中任一条单独摘掉,另一条仍会把 DOM 拉到当前值 —— 实测摘掉隐藏分支的清理
        // 它照样绿。留着它是为了守住「转一圈回来画的是现在的数据」这条**结果**,
        // 真要哪天两条路一起坏了(比如 paint 被条件挡死),它才是唯一会红的那条。
        const back = await evaluate(
            IN(`
            const dg = w.__SCVB_OUTPUT__.distMotion();
            const cont = q(".dist-bars");
            const bars = all(".dist-bar");
            let drift = -1;
            if (cont && bars.length) {
                const cr = cont.getBoundingClientRect();
                let mx = 0;
                for (const b of bars) {
                    const written = parseFloat(b.style.getPropertyValue("--x"));
                    const r = b.getBoundingClientRect();
                    const rendered =
                        ((r.left + r.width / 2 - cr.left) / cr.width) * 100;
                    if (Number.isFinite(written) && cr.width > 0) {
                        mx = Math.max(mx, Math.abs(rendered - written));
                    }
                }
                drift = mx;
            }
            return { shown: dg.shown.length, bars: bars.length, drift };
        `),
        );
        check(
            back.shown > 0,
            `切回分布档后补间器重新接管(shown ${back.shown} 行,DOM ${back.bars} 根柱)`,
        );
        check(
            back.drift >= 0 && back.drift < 1.5,
            `切回后屏幕上画的是当前数据(渲染 vs 写入偏差 ${back.drift.toFixed(2)} 个百分点,阈值 1.5)`,
        );
        assertClean("视图切换");
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
console.log("\n✅ Output 分布图 rAF 补间页面级冒烟全绿");
process.exit(0);
