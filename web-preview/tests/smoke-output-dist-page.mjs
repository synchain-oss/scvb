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
        newBucket("curve-editor");
        await cdp.send("Page.navigate", {
            // 场景名必须**两张表都在**才不留不匹配 —— 这地方栽了两次:
            //   • `printing` 在 `SCENARIO_NAMES.output` 白名单里,但 `SCENARIO_MAP` 里没有
            //     ⇒ 静默回落 fifteen-tracks + console.warn;
            //   • `connected` 反过来:`SCENARIO_MAP` 里有,却只在 `SCENARIO_NAMES.input`
            //     ⇒ `allowedOr()` 判成表外值,壳页工具条显示 `scenario=unknown`。
            // 两次都「功能上恰好够用」,所以都没被断言抓到 —— 而白名单那段注释的原话
            // 正是「顺手把『参数拼错了』变成肉眼可见的信号」。
            // `curve-editor` 在两张表里都有,且同样落满配 fifteen-tracks 世界。
            // 本套的数据面由探针自己用 setTrackManual 驱动,不依赖场景带任何特殊状态。
            url: `${base}/web-preview/output.html?scenario=curve-editor`,
        });
        const up = await waitFor(READY);
        check(up, "页面装载并吃到首帧");

        // 把上面那段注释**钉成机器断言** —— 光写注释挡不住第三次。壳页工具条就在顶层
        // document(`appendMetaField` 全程 textContent,格式是 `scenario=<code>值</code>`,
        // 故 textContent 里就是 `scenario=curve-editor`),而 `allowedOr()` 对表外值
        // 输出的正是字面量 `unknown`。这一条把两种错法都封上:
        //   • 名字不在 `SCENARIO_NAMES.output` ⇒ 这里读到 `scenario=unknown`,红;
        //   • 名字不在 `SCENARIO_MAP` ⇒ 静默回落只有一条 console.warn,而 `assertClean()`
        //     只收 console.error 抓不到 —— 但那种名字进不了白名单,同样在这里红。
        const named = await waitFor(
            `/scenario=curve-editor/.test(document.body.textContent || "")`,
            5000,
        );
        check(named, "壳页工具条认出了场景名(不是 scenario=unknown)");

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
                    // n = 真正参与比较的柱数。没有它就有个**空绿口**:mx 初值 0,
                    // 若哪天所有柱的 --x 都读不出来(改了写入面、或节点没建起来),
                    // drift 会是 0 而不是 -1,阈值断言照样绿。
                    // (注:本段在模板字面量里,注释**不能带反引号** —— 会把模板提前截断。)
                    let drift = -1;
                    let n = 0;
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
                                n += 1;
                            }
                        }
                        drift = n > 0 ? mx : -1;
                    }
                    out.push({
                        frames: dg ? dg.frames : -1,
                        pushes: dg ? dg.pushes : -1,
                        shown: dg ? JSON.stringify(dg.shown) : "",
                        target: dg ? JSON.stringify(dg.target) : "",
                        drift,
                        n,
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
        // 先断「真的有柱参与了比较」——否则下面那条会在「一根都没读到」时空绿。
        const compared = Math.max(...probe.map((p) => p.n || 0));
        check(compared > 0, `有柱参与渲染面比较(最多 ${compared} 根)`);
        const maxDrift = Math.max(...probe.map((p) => p.drift));
        check(
            maxDrift >= 0 && maxDrift < 1.5,
            `柱子渲染位置紧跟写入值,没有被 CSS 过渡拖住(最大偏差 ${maxDrift.toFixed(2)} 个百分点,阈值 1.5)`,
        );
        assertClean("curve-editor 分布图补间");
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
        // 该守的性质是:**转一圈回来之后,屏幕上画的是「现在」的数据,不是隐藏前那份**。
        // 判据分两截,缺一截都不成立:
        //   ① `shown` 追上了**切回之后新写进去的** pan —— 隐藏期 `shown` 被清空,
        //      不重新接管它就还是空的 / 还是旧值;
        //   ② 渲染位置 == 写入的 `--x`(空间量尺,与帧率无关)—— 保证 ① 那份新值
        //      真的到了屏幕上,而不是被 CSS 过渡拖在半路。
        //
        // ⚠ **单靠 ② 是不够的**,这一点上一版写错了:若重新接管压根没做对,`--x` 与渲染
        // 位置会**一起**停在旧值上,drift ≈ 0 照样绿。所以必须有 ① 去钉「值是新的」,
        // ② 只回答「新值有没有到屏幕上」。为此切回之后**显式写一个已知 pan** 再断 ——
        // 隐藏期那段 ±70 的铺垫对这两条其实都不加区分力(它只保证隐藏期数据在动)。
        const KNOWN_PAN = -95;
        await evaluate(
            IN(`
            const mk = w.__SCVB_MOCK__;
            if (mk && typeof mk.setTrackManual === "function") {
                mk.setTrackManual(1, "pan", ${KNOWN_PAN});
            }
            return true;
        `),
        );
        await sleep(400);
        const back = await evaluate(
            IN(`
            const dg = w.__SCVB_OUTPUT__.distMotion();
            const cont = q(".dist-bars");
            const bars = all(".dist-bar");
            let drift = -1;
            let n = 0;
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
                        n += 1;
                    }
                }
                drift = n > 0 ? mx : -1;
            }
            // 按**轨号**取,不按下标:写的是 setTrackManual(1, ...),
            // 行序由 connectedChannels 决定,fixture 里哪天有一轨没连,
            // shown[0] 就是别的轨 —— 那时这条会对着错的对象断言,而且多半还是绿的。
            const rows = (dg.shown || []).filter((r) => r.ch === 1);
            const ch1 = rows.length === 1 ? rows[0].pan : null;
            return { shown: dg.shown.length, bars: bars.length, drift, n, ch1 };
        `),
        );
        check(
            back.shown > 0,
            `切回分布档后补间器重新接管(shown ${back.shown} 行,DOM ${back.bars} 根柱)`,
        );
        // ① 值是**新的**:切回之后写进去的那个已知 pan 真的到了补间器手里。
        //    重新接管没做对的话,这里会是 null(shown 还是空)或旧值。
        check(
            back.ch1 === KNOWN_PAN,
            `切回后补间器吃到的是切回之后写的新值(实得 ${back.ch1},期望 ${KNOWN_PAN})`,
        );
        // ② 新值到了**屏幕上**:渲染位置 == 写入的 --x。
        check(back.n > 0, `有柱参与渲染面比较(${back.n} 根)`);
        check(
            back.drift >= 0 && back.drift < 1.5,
            `切回后屏幕上画的就是那个新值(渲染 vs 写入偏差 ${back.drift.toFixed(2)} 个百分点,阈值 1.5)`,
        );
        assertClean("视图切换");
    }

    // =========================================================================
    log("=== ③ SL-241:复制版本 → 切进去,分布图不许全轨居中 ===");
    //
    // 用户实测(Cubase 15 Pro,v5.6.2):复制版本后切到新版本,**声像显示全轨居中**,
    // 一开始播放就正常。成因见 `web/shared/readback.js` 头注:`copyVersion` 契约是
    // 「零参数写入」,引擎打印头又只驱动当前激活版本 —— 刚切进去还没播过的那一版,
    // 参数面装的就是出厂默认(pan 居中)。分布图此前**只读参数面**,于是照单全收。
    //
    // 这一条必须是**页面级**的:node 侧断得到读回链(smoke-tab1-interactions ⑦),
    // 断不到「renderDist 真的改用了那条链」——而后者正是本卡改的那几行。
    // 判据取补间器的 `target`(= renderDist 最近推进来的那一帧行模型)。
    {
        newBucket("sl241-version-switch");
        await cdp.send("Page.navigate", {
            // 重新装载,且 **`play=0` 停走带**:上一节 setTrackManual 给轨 1 留了条
            // 手动常值段,会盖住「读曲线段」这条支路 —— 本节要断的恰是那一支。
            //
            // ⚠ `play=0` 不是可选项。mock 的 PRINT 是三与(输出 ON ∧ 播放中 ∧ 在 range 内),
            // 一旦成立,`printedParamsDiff` 就把段值写进参数面 —— 那正是用户说的
            // 「一开始播放就正常」。带着走带跑本节,**修复前也会绿**(实测 14/15 不居中),
            // 这一条就再也钉不住 renderDist 那几行。走带停着才是「刚切进去还没播」那一刻。
            url: `${base}/web-preview/output.html?scenario=curve-editor&play=0`,
        });
        check(await waitFor(READY), "页面重新装载并吃到首帧");

        // 前置:**切版本之前**这张图本来就画得开。这一条同时把两个隐式前提钉住
        // (#159 复审【建议】3):① 渲染面这条路是通的;② 全局「最大角度」不为 0 ——
        // `distGeometry` 的横位是 `pan x globalWidthPct/100`,width=0 时不论 pan 多少
        // 全都落在 50%,底下那条渲染面断言会变成假红,而排查会从渲染层一路往回找。
        const readBars = IN(`
            const dg = w.__SCVB_OUTPUT__.distMotion();
            const rows = dg.target || [];
            const bars = all(".dist-bar");
            const xs = bars
                .map((b) => parseFloat(b.style.getPropertyValue("--x")))
                .filter((x) => Number.isFinite(x));
            return {
                n: rows.length,
                offCenter: rows.filter((r) => Math.abs(r.pan) > 0.05).length,
                bars: bars.length,
                barsOffCenter: xs.filter((x) => Math.abs(x - 50) > 0.05).length,
            };
        `);
        // 等首轮补间收手再读:柱子的 `--x` 从居中起补,页面刚装载那一刻本来就都在 50%
        // (实测 13/15 轨已有值、0/15 根柱到位)。等不到就说明渲染这条路根本不通 ——
        // 那正是这条前置要抓的。
        const settled = await waitFor(
            IN(`
            const bars = all(".dist-bar");
            const xs = bars
                .map((b) => parseFloat(b.style.getPropertyValue("--x")))
                .filter((x) => Number.isFinite(x));
            return bars.length > 0 && xs.some((x) => Math.abs(x - 50) > 0.05);
        `),
            8000,
        );
        check(settled, "首轮补间收手,柱子落到各自的位置上");
        const before = await evaluate(readBars);
        check(
            before.n > 0 && before.offCenter > 0 && before.barsOffCenter > 0,
            `前置:切版本**之前**这张图本来就画得开(${before.offCenter}/${before.n} 轨、${before.barsOffCenter}/${before.bars} 根柱不在中间)`,
        );

        // 用户那一幕的三步(走带已由 `play=0` 停着):输出 ON → 复制 → 切过去。
        // 三步的**回执**都带回来断:mock 的 copyVersion / setVersionActive 在 PRINT 态会回
        // `{rejected:"printing"}`,只 `return "ok"` 的话这一步被拒也照样绿,真正被钉住的
        // 就只剩「切进一个没被驱动过的版本」了(#159 复审【建议】3)。
        const acted = await evaluate(
            IN(`
            const mk = w.__SCVB_MOCK__;
            if (!mk) return "no-mock";
            for (const fn of ["setOutputEnabled", "copyVersion", "setVersionActive"]) {
                if (typeof mk[fn] !== "function") return "no-" + fn;
            }
            const r1 = mk.setOutputEnabled(true);
            const r2 = mk.copyVersion(1, 2);
            const r3 = mk.setVersionActive(2);
            return JSON.stringify([r1, r2, r3]);
        `),
        );
        const okAll =
            typeof acted === "string" &&
            acted.startsWith("[") &&
            // 两种拒绝形态都要堵:PRINT 闸回 {rejected:"printing"},而参数/前置校验回的是
            // {ok:false, reason:"..."}(mock 的 BAD_ARG / noTimeline)——后者里既没有
            // "rejected" 也没有 "error",只查 rejected 的话 copyVersion 被 BAD_ARG 拒掉
            // 这条 check 照样绿,正是它本来要消灭的那种「被拒也绿」(#159 复审第二轮)。
            !/rejected|"ok":\s*false/.test(acted);
        check(
            okAll,
            `输出 ON + copyVersion(1,2) + 切到 V2 三步都被接受(回执 ${acted})`,
        );
        await sleep(600); // 全量 params/segments 到齐 + 一轮补间收手

        // 渲染面读的是 `--x`(柱心横向百分比),pan=0(居中)恰好是 50%。
        const shot = await evaluate(readBars);
        check(
            shot.n > 0,
            `切版本后分布图仍有行(实得 ${shot.n} 行 / ${shot.bars} 根柱)`,
        );
        log(
            `  (切到 V2 后:${shot.offCenter}/${shot.n} 轨不在中间;柱 ${shot.barsOffCenter}/${shot.bars} 根不在 50%)`,
        );
        // ★ 核心:**多数轨不在中间**。修复前这两个计数都恰好是 0 —— 参数面上 V2 的
        // 63 个 id 全是出厂默认,分布图照着画,15 根柱齐刷刷落在 50%。
        // 取「过半」而不是「全部」:mock 的段生成器是随机的,某一轨的首段 pan 恰好
        // 落在 0 上是合法的,拿它判红就是按随机数判红。修复前后是 0 与 ~15 的对比,
        // 过半这道线两边都离得很远。
        check(
            shot.offCenter * 2 > shot.n,
            `(写入面)切进刚复制的 V2:多数轨读的是曲线值而非出厂默认居中(实得 ${shot.offCenter}/${shot.n})`,
        );
        check(
            shot.barsOffCenter * 2 > shot.bars,
            `(渲染面)屏幕上的柱子也不在中间(实得 ${shot.barsOffCenter}/${shot.bars} 根偏离 50%)`,
        );
        assertClean("SL-241 切版本");
    }

    // =========================================================================
    log("=== ④ SL-251/J93:播放期不再闪烁 + 图表卡不再压暗 ===");
    //
    // 用户实测(v5.6.3):播放时整体调整页很多设置变暗、**包括下面的图表**,然后开始闪烁。
    // 修前本探针实测:四张卡的 data-host-echo 各 1.3 次/秒翻转(8s / 10 次)。
    //
    // 这一节必须是页面级:node 侧断得到闩锁纯函数(smoke-tab1-interactions ⑧),
    // 断不到「renderParams 真的改用了它、且图表卡真的退出了名单」。
    {
        newBucket("sl251-flicker");
        await cdp.send("Page.navigate", {
            // play=1:走带在跑,配合输出 ON 进 PRINT —— 用户那一幕的前提。
            url: `${base}/web-preview/output.html?scenario=curve-editor&play=1`,
        });
        check(await waitFor(READY), "页面装载并吃到首帧");
        await evaluate(
            IN(`
            const mk = w.__SCVB_MOCK__;
            if (mk) mk.setOutputEnabled(true);
            return true;
        `),
        );
        // 裁定③ 的 console 读数钩子:**必须在 8 秒采样之前**装上 —— 那行读数只在
        // 「两帧 hostEcho:true 间隔越过释放窗口」时打印,而那正是采样窗里徽标灭一下
        // 再亮的同一时刻。装晚了(等采样跑完再装)就会错过整段,实测捞到 0 条。
        await evaluate(
            IN(`
            w.__SL251_DBG__ = [];
            const orig = w.console.debug;
            w.__SL251_CANARY__ = 0;
            w.console.debug = function (...a) {
                const s = a.join(" ");
                if (s.indexOf("[SCVB][SL-251]") >= 0) w.__SL251_DBG__.push(s);
                if (s.indexOf("__canary__") >= 0) w.__SL251_CANARY__++;
                return orig.apply(this, a);
            };
            w.console.debug("__canary__");
            return true;
        `),
        );
        await sleep(1200); // 等打印头开始推 hostEcho:true 的帧

        const readState = IN(`
            const pick = (gb) => {
                const n = d.querySelector('[data-gb="' + gb + '"]');
                return n ? (n.getAttribute("data-host-driven") || "-") : "?";
            };
            const badge = (gb) => {
                const n = d.querySelector('[data-gb="' + gb + '-hostbadge"]');
                return n ? (n.getAttribute("data-on") || "-") : "?";
            };
            const dist = d.querySelector('[data-gb="master-distchart"]');
            return {
                width: pick("master-width"),
                ms: pick("master-msbalance"),
                lead: pick("master-leadselect"),
                widthBadge: badge("master-width"),
                msBadge: badge("master-msbalance"),
                leadBadge: badge("master-leadselect"),
                distDriven: dist ? (dist.getAttribute("data-host-driven") || "-") : "?",
                distEcho: dist ? (dist.getAttribute("data-host-echo") || "-") : "?",
                distOpacity: dist ? getComputedStyle(dist).opacity : "?",
            };
        `);

        const on = await evaluate(readState);
        log(`  打印中:${JSON.stringify(on)}`);
        check(
            on.width === "1" && on.ms === "1" && on.lead === "1",
            `(a) 打印中三张参数卡挂上 data-host-driven=1(实得 ${on.width}/${on.ms}/${on.lead})`,
        );
        check(
            on.widthBadge === "1" && on.msBadge === "1" && on.leadBadge === "1",
            "(b) 三枚徽标同步亮起(裁定③:提示改成徽标)",
        );
        // ★ 裁定②:图表卡整个退出提示名单 —— 既不该挂属性,更不该被压暗。
        check(
            on.distDriven === "-" && on.distEcho === "-",
            `(c) ★ 图表卡不再挂任何 hostEcho 属性(实得 driven=${on.distDriven} echo=${on.distEcho})`,
        );
        check(
            on.distOpacity === "1" || parseFloat(on.distOpacity) > 0.99,
            `(d) ★ 图表卡不透明度回到 1(实得 ${on.distOpacity};修前是 0.55)`,
        );

        // ---- (e) **native 快通道对拍**:中间插一帧 hostEcho:false,徽标不许被打断。
        //
        // ⚠ mock 在纯播放期只发 hostEcho:true 的帧,所以**它自己重现不出真机那条快通道**
        // (native 每帧都带 C++ 那个 600ms 窗口的当前值,宿主一停写就是 false)。
        // 这里借 `setParam` 走一条**真的 mock 代码路径**造出那一帧:它发的正是
        // `{values:{...}, hostEcho:false}`(juce-bridge-mock.js 的 §1.13 回声),
        // 与 native 插进来的 false 帧同形。不加这一条,这一节在 mock 上是**空绿**的。
        const afterFalse = await evaluate(
            IN(`
            const mk = w.__SCVB_MOCK__;
            if (!mk || typeof mk.setParam !== "function") return null;
            mk.setParam("width", 101);   // ← 发一帧 hostEcho:false
            return true;
        `),
        );
        check(afterFalse === true, "(e) 成功注入一帧 hostEcho:false");
        await sleep(120); // 让那一帧走完 store → render
        const still = await evaluate(readState);
        check(
            still.width === "1" && still.widthBadge === "1",
            `(e) ★ 一帧 hostEcho:false **没有**打断徽标(实得 driven=${still.width} badge=${still.widthBadge})—— 退回旧判据这里即红`,
        );

        // ---- (f) 8 秒逐帧采样:翻转次数必须回到 0
        const flick = await evaluate(
            IN(`
            return new Promise((res) => {
                const ids = ["master-width","master-msbalance","master-leadselect"];
                const seen = {}; const flips = {};
                let frames = 0; const t0 = performance.now();
                // 采样窗内**越窗间隔**的条数:每一条会让徽标灭一次再亮一次 = 每张卡 2 次翻转。
                const g0 = (w.__SL251_DBG__ || []).length;
                const step = () => {
                    frames++;
                    for (const id of ids) {
                        const n = d.querySelector('[data-gb="' + id + '"]');
                        const v = n ? n.getAttribute("data-host-driven") : "-";
                        if (seen[id] === undefined) { seen[id] = v; flips[id] = 0; }
                        else if (seen[id] !== v) { flips[id] += 1; seen[id] = v; }
                    }
                    if (performance.now() - t0 >= 8000) {
                        res({ frames, flips, gaps: (w.__SL251_DBG__ || []).length - g0 });
                    } else requestAnimationFrame(step);
                };
                requestAnimationFrame(step);
            });
        `),
        );
        const total = Object.values(flick.flips).reduce((a, b) => a + b, 0);
        log(
            `  8s / ${flick.frames} 帧,三张卡翻转合计 ${total} 次(修前实测每张 10 次)`,
        );
        // 判据是**不变式**,不是魔数:每一次翻转都必须被一条「越窗间隔」解释掉。
        //
        // 一条越窗间隔 ⇒ 徽标灭一次、再亮一次 = 每张卡 2 次翻转,三张卡 6 次。所以
        //     总翻转 <= 2 × 卡数 × 采样窗内的越窗间隔条数
        // 越窗间隔 = 闩锁**该**释放的时刻(信号真的停了 >2s),不是判据在抖;判据抖的话
        // 翻转会**多于**这个上界。修前那一版(看最近一帧的原始布尔)实测 30 次而间隔只有
        // 一两条,这条不变式一样拦得住。
        //
        // ⚠ 上一版这里写的是 `total <= 6`。本次 CI 实得**正好 6**,余量归零 —— 而
        // web-smoke 是 required check,下一窗多撞上半条间隔就是一次假红,与 (g) 上一轮
        // 栽的是同一个坑(按帧率/走带节奏判红)。换成不变式之后就与这些无关了。
        // 顺带:这也把 PR 正文里「那 6 次不是抖动、是信号真停了」的论证从**注释升级成断言**。
        const gaps = flick.gaps || 0;
        // 上界 = 3 张卡 ×(每条间隔 2 次 + 1 次跨窗余量)。那个 +1 是给「间隔在采样窗
        // **开始之前**就起头、窗内只看到重新亮起那一半」的情形:它的日志条目落在 g0 之前,
        // 不计进 gaps,却贡献 1 次翻转 —— 不留这一格会在另一个方向上假红。
        const bound = 3 * (2 * gaps + 1);
        log(`  采样窗内越窗间隔 ${gaps} 条 ⇒ 翻转上界 ${bound}`);
        check(
            total <= bound,
            `(f) ★ 每一次翻转都被越窗间隔解释掉(实得 ${total} 次 <= 上界 ${bound};` +
                `间隔 ${gaps} 条;${JSON.stringify(flick.flips)})—— 判据若在抖,翻转会多于上界`,
        );
        // ---- (g) 裁定③ 的 console 读数**真的会打印**(复审第一轮【重要】1 的回归)
        //
        // 那行 `console.debug` 是「释放窗口本机测不出真机间隔分布」的唯一补偿手段
        // (SL-251 当时是 2000ms 一个窗口打天下;SL-270 之后是停走 900 / 播放 2500 两档),
        // 而它第一版是**死代码**:`store.params` 在读 prevAt 之前就被整体重写了,gap 恒 ≈0。
        // 静态看不出来,只有真跑才知道 —— 所以这一条必须是页面级。
        // ⚠ 间隔**自己造**,不靠等 mock 的段边界撞上来:后者取决于走带在这一节里跑到哪、
        // rAF 节奏多快,CI 上实测捞不到(本机能捞到 3048ms 那一条)—— 那就是本仓注释里
        // 反复记过的「按帧率判红」。这里改成关掉输出 → 打印停 → 静置超过释放窗口 →
        // 再打开,下一帧 hostEcho:true 的间隔必然越窗。两步都走真实桥调用。
        const offR = await evaluate(
            IN(`
            const mk = w.__SCVB_MOCK__;
            if (!mk) return "no-mock";
            return JSON.stringify(mk.setOutputEnabled(false));
        `),
        );
        check(
            typeof offR === "string" && !/rejected|"ok":\s*false/.test(offR),
            `(h) 关输出被接受(回执 ${offR})—— 丢返回值的话下面整段会在「没真关掉」上空绿`,
        );
        // [SL-270] 这一节里走带**仍在跑**(?play=1),所以此刻生效的是**播放档**
        // (HOST_ECHO_RELEASE_PLAYING_MS = 2500),不是停走档的 900。静置必须越过播放档,
        // 否则下面 (h) 会在「还没到该熄的时候」判红。留足余量:2500 + 900,不卡在边界上
        // (本仓记过一次「按帧率/节奏判红」的假红)。
        await sleep(3400);

        // ---- (h) 顺带把**熄侧**钉一下:静置超过释放窗口之后,徽标必须已经灭了。
        // ⚠ 说清它证明什么、不证明什么:它断的是**用户可见结果**(停了就该退)。
        //
        // 本节 render 停不下来,**原因不是 conn 心跳** —— 这一句上一版写错了,已按实测订正:
        //   • `scvb.conn` 走的是 `emitIfChanged`(`state-driver.js:809`),`JSON.stringify`
        //     逐字相同即**不发**(`juce-bridge-mock.js:422`),静置期一帧都不推;
        //   • `heartbeatAgeMs` 也不是活计数器,是 `40 + floor(unit(0x5001, ch) * 260)`
        //     (`mock-data.js:1109`),按**轨号**确定性取值,与时间无关。
        // 真正让 render 停不下来的是本节的 `?play=1`:`scvb.playhead` 每帧 `timeS` 在走
        // ⇒ `samePlayhead` 判不同 ⇒ 逐帧 `requestRender`。
        //
        // 所以本条的免责范围要跟着收窄:走带在跑时它兜不到「定时器挂错档」这类回归
        // (逐帧 render 会替定时器把徽标熄掉)。**真正钉住那一拍定时器的是下面的 (e)**
        // —— 它用 `setHostTimeAvailable(false)` 把 playhead 载荷钉成逐帧逐字相同,
        // 唯一那条活着的 render 源就没了,删掉长定时器当场红(实测 8166ms)。
        // 别再照上一版那句话推论「conn 心跳还在,所以 (e) 也会空绿」—— 那条推论错在
        // 前提上,而它已经真的误导过一个审查端点。
        const quiet = await evaluate(readState);
        check(
            quiet.width === "0" &&
                quiet.widthBadge === "0" &&
                quiet.ms === "0" &&
                quiet.lead === "0",
            `(h) ★ 静置 3.4s(> 播放档释放窗口)后三张卡与徽标都已熄(实得 ${quiet.width}/${quiet.ms}/${quiet.lead},徽标 ${quiet.widthBadge})`,
        );

        const onR = await evaluate(
            IN(`
            const mk = w.__SCVB_MOCK__;
            if (!mk) return "no-mock";
            return JSON.stringify(mk.setOutputEnabled(true));
        `),
        );
        check(
            typeof onR === "string" && !/rejected|"ok":\s*false/.test(onR),
            `(h) 重新打开输出被接受(回执 ${onR})`,
        );
        await sleep(1200); // 等打印头重新推第一帧 hostEcho:true
        const dbg = await evaluate(
            IN(`
            return {
                n: (w.__SL251_DBG__ || []).length,
                first: (w.__SL251_DBG__ || [])[0] || "",
                canary: w.__SL251_CANARY__ || 0,
            };
        `),
        );
        log(
            `  console 读数命中 ${dbg.n} 次(钩子自检 canary=${dbg.canary});首条:${dbg.first.slice(0, 120)}`,
        );
        check(
            dbg.canary === 1,
            `(g) 前置:console.debug 钩子本身有效(canary=${dbg.canary})`,
        );
        check(
            dbg.n > 0,
            `(g) ★ 裁定③ 的 console 读数真的打印了(实得 ${dbg.n} 次)—— ` +
                `第一版是死代码(prevAt 在 store.params 被重写后才取,gap 恒 ≈0),那一版这里是 0`,
        );

        assertClean("SL-251 闪烁");
    }

    // =========================================================================
    // ⑨ [SL-269] 分布图的**光栅面**:合成层隔离 + 零宽张开线
    // -------------------------------------------------------------------------
    // 用户实测(v5.6.5,WebView2):播放中每根柱子的顶端往上拖出一条与轨同色的细竖线,
    // 一路到 plot 顶边,多轨同时。
    //
    // ⚠ 先把这一节**证明不了**什么说清楚,免得下一个人把它读成「线没了」:
    //   拖影是 WebView2 的失效矩形行为,**无头 Chrome 上修前修后都不出线** —— 本套跑的
    //   正是无头 Chrome,所以它守不到现象。它守的是**修法还在**:两条声明(合成层隔离)
    //   与一条几何(零宽 ⇒ 零高)在**渲染面**上确实生效。现象一侧的判据只有真机。
    //   这也是为什么这里读的是 getComputedStyle / getBoundingClientRect 而不是源码正则:
    //   源码里写了 ≠ 这条规则真的落到了元素上(选择器写错、被后面的规则盖掉都可能)。
    // =========================================================================
    {
        newBucket("sl269-raster");
        await cdp.send("Page.navigate", {
            url: `${base}/web-preview/output.html?scenario=curve-editor&play=1`,
        });
        check(await waitFor(READY), "页面装载并吃到首帧");

        // ---- (a) 柱与张开线各自独占合成层
        const layers = await evaluate(
            IN(`
            const one = (sel) => {
                const n = q(sel);
                if (!n) return null;
                const cs = w.getComputedStyle(n);
                return { willChange: cs.willChange, transform: cs.transform };
            };
            return { bar: one(".dist-bar"), span: one(".dist-span"),
                     bars: all(".dist-bar").length, spans: all(".dist-span").length };
        `),
        );
        check(
            layers && layers.bars > 0 && layers.spans > 0,
            `(a) 前提:页面上确有柱与张开线(实得 ${layers && layers.bars} / ${layers && layers.spans})`,
        );
        for (const [name, got] of [
            ["dist-bar", layers && layers.bar],
            ["dist-span", layers && layers.span],
        ]) {
            check(
                !!got && /transform/.test(got.willChange),
                `(a) ★ ${name} 声明了 will-change: transform(实得 ${got && got.willChange})`,
            );
            // translateZ(0) 计算出来是 matrix3d(…),不会是 "none"。删掉那一行即红。
            check(
                !!got && got.transform !== "none" && got.transform !== "",
                `(a) ★ ${name} 有非 none 的 transform(= 强制独立层;实得 ${got && got.transform})`,
            );
        }

        // ---- (b) 零宽的张开线必须**一个像素都不画**
        //
        // 用「最大角度」这把真滑杆造零宽:distGeometry 的 half = min(width%/100×16×g, x, 100−x),
        // g = 全局 width/100。g=0 ⇒ 每一行的 half 都归零,一次把所有张开线推进退化态,
        // 不用去猜某一轨的 pan 参数 id。走的是 setParam 这条真桥路径。
        //
        // 断的是**逐帧写变量**那条路,不是重拼那条(PR 178 复审有人读成了后者):
        // 只拧全局 width 时轨集/立体声/lead/高亮一个没变 ⇒ `distShapeKey` 不变 ⇒
        // dist-motion 的 `push` 不进 `key !== shapeKey` 的重拼支,落在
        // `if (width !== lastPaintedWidth) paint(width)` 上 —— `paint` 就是 rAF 补间
        // 每帧调的那一个,`--span-h` 由它经 `setVars`/`distSpanVars` 写下去。
        // 所以「补间落点上零宽也零高」这条,下面这组已经断到了。
        const rects = IN(`
            const out = [];
            for (const n of all(".dist-span")) {
                const r = n.getBoundingClientRect();
                out.push({ w: Math.round(r.width * 100) / 100, h: Math.round(r.height * 100) / 100 });
            }
            return out;
        `);
        const wide = await evaluate(rects);
        check(
            wide.length > 0 && wide.some((v) => v.w > 0 && v.h > 1),
            `(b) 前提:常态下张开线有宽也有粗(实得 ${JSON.stringify(wide.slice(0, 3))})`,
        );

        const setW0 = await evaluate(
            IN(`
            const mk = w.__SCVB_MOCK__;
            if (!mk || typeof mk.setParam !== "function") return "no-mock";
            return JSON.stringify(mk.setParam("width", 0));
        `),
        );
        check(
            typeof setW0 === "string" && !/rejected|"ok":\s*false/.test(setW0),
            `(b) 「最大角度」拧到 0 被接受(回执 ${setW0})—— 丢返回值的话下面整段会空绿`,
        );
        await sleep(500); // 让 rAF 补间走完(补间窗 40ms 级)
        const zero = await evaluate(rects);
        log(`  最大角度=0 时的张开线矩形:${JSON.stringify(zero.slice(0, 3))}`);
        check(
            zero.length > 0 && zero.every((v) => v.w === 0),
            `(b) 前提:最大角度=0 之后每条张开线都是零宽(实得 ${JSON.stringify(zero.slice(0, 3))})`,
        );
        check(
            zero.length > 0 && zero.every((v) => v.h === 0),
            `(b) ★ 零宽的张开线渲染高度也是 0 —— 退回写死的 height:1.5px 即红` +
                `(实得 ${JSON.stringify(zero.slice(0, 3))})`,
        );

        assertClean("SL-269 光栅面");
    }

    // =========================================================================
    // ⑩ [SL-270] hostEcho 徽标:释放窗口按走带态分两档
    // -------------------------------------------------------------------------
    // 用户实测(v5.6.5):① 停走之后徽标还挂着近两秒;② 快速起停会让徽标在**播放中途**
    // 消失。② 是 ① 的另一面 —— 按停那一刻闩锁还剩一大截,立刻重按播放,这一截残余在新的
    // 一段播放里走完。
    //
    // ⚠ 同样先说清楚**证明不了**什么:② 的完整现象需要「宿主两次写之间隔着秒级」,而 mock
    //   的打印头一恢复就立刻推帧(本文件 SL-251 节已记过:mock 只有慢通道)。所以这里不去
    //   赌那一幕,而是把**机理**钉在渲染面上:两档窗口各自真的在生效。② 的修复等价于
    //   「按停即回短窗口」,那就是 (d)。
    // =========================================================================
    {
        newBucket("sl270-release-windows");
        await cdp.send("Page.navigate", {
            url: `${base}/web-preview/output.html?scenario=curve-editor&play=1`,
        });
        check(await waitFor(READY), "页面装载并吃到首帧");

        const BADGE = `d.querySelector('[data-gb="master-width-hostbadge"]')`;
        const badge = IN(`
            const n = ${BADGE};
            return n ? (n.getAttribute("data-on") || "-") : "?";
        `);
        const badgeOn = IN(`
            const n = ${BADGE};
            return !!n && n.getAttribute("data-on") === "1";
        `);
        const badgeOff = IN(`
            const n = ${BADGE};
            return !!n && n.getAttribute("data-on") === "0";
        `);
        const setOutput = (on) =>
            evaluate(
                IN(`
            const mk = w.__SCVB_MOCK__;
            if (!mk) return "no-mock";
            return JSON.stringify(mk.setOutputEnabled(${on ? "true" : "false"}));
        `),
            );
        // 走带开关在**壳页**上:走带是宿主的东西,`__SCVB_MOCK__` 只有桥面的上行函数。
        const setPlaying = (on) =>
            evaluate(`(() => {
            const s = window.__SCVB_PREVIEW__;
            if (!s || !s.ctl) return "no-session";
            s.ctl.setTransport({ isPlaying: ${on ? "true" : "false"} });
            return "ok";
        })()`);

        // ---- 量法:从**徽标亮起那一刻**量到它熄灭,而不是从「我发了指令」那一刻量。
        //
        // 这一条是本节能不能算数的关键。闩锁量的是「距最后一帧 hostEcho:true 多久」,
        // 而那一帧什么时候来我们并不知道 —— mock 的打印头是「值变了才发」,实测两帧之间
        // 能隔两三秒。从指令时刻起算的话,测出来的间隔里混着一段未知的「上一帧有多旧」,
        // 只能给上界、给不出下界,于是「播放档确实更宽」这半条根本证明不了(本仓记过
        // 「按帧率/节奏判红」的假红,这里是同一个坑的另一面)。
        //
        // 改成:先把徽标打灭,再打开打印头,**在页内**盯住 0 → 1 那一次跳变并落一个
        // 时间戳(`__SL270_ON_AT__`)—— 跳变意味着刚刚到了一帧 true,起点就此钉死;
        // 随后停掉信号源,页内一直采到徽标转 0,回报的差值就是**真正的释放窗口**
        // (外加一次 CDP 往返,几十毫秒量级,只会让读数偏大一点点)。
        const armOnEdge = IN(`
            w.__SL270_ON_AT__ = 0;
            const tick = () => {
                const n = ${BADGE};
                if (n && n.getAttribute("data-on") === "1") {
                    w.__SL270_ON_AT__ = w.performance.now();
                    return;
                }
                w.setTimeout(tick, 30);
            };
            tick();
            return true;
        `);
        const onEdgeSeen = IN(`return (w.__SL270_ON_AT__ || 0) > 0;`);
        const measureOff = IN(`
            return new Promise((res) => {
                const t0 = w.__SL270_ON_AT__ || 0;
                if (!t0) return res(-1);
                const step = () => {
                    const n = ${BADGE};
                    if (n && n.getAttribute("data-on") === "0") {
                        return res(Math.round(w.performance.now() - t0));
                    }
                    if (w.performance.now() - t0 > 12000) return res(-2);
                    w.setTimeout(step, 30);
                };
                step();
            });
        `);

        // 一次完整测量:打灭 → 装边沿探针 → 开打印头 → 等 0→1 跳变 → 掐掉信号源 → 采到熄。
        async function measureRelease(label, killSignal) {
            await setOutput(false);
            if (
                !check(
                    await waitFor(badgeOff, 12000),
                    `(${label}) 前置:先把徽标打灭`,
                )
            )
                return null;
            await evaluate(armOnEdge);
            const r = await setOutput(true);
            if (
                !check(
                    typeof r === "string" && !/rejected|"ok":\s*false/.test(r),
                    `(${label}) 前置:打开打印头被接受(回执 ${r})`,
                )
            )
                return null;
            // 亮起要等打印头写出一个**变化**的值;mock 上实测能到 2-3 秒,故给足额度。
            if (
                !check(
                    await waitFor(onEdgeSeen, 15000),
                    `(${label}) 前置:观察到徽标 0 → 1 的那一次跳变(起点由它钉死)`,
                )
            )
                return null;
            await killSignal();
            const ms = await evaluate(measureOff);
            log(`  ${label}:从亮起到熄灭 ${ms} ms`);
            return ms;
        }

        check(
            (await setPlaying(true)) === "ok",
            "(前提) 壳页暴露了预览会话(window.__SCVB_PREVIEW__.ctl)—— 没有它整节都测不了",
        );

        // ---- (a) **播放档**:关掉打印头,但走带**继续跑**。
        const playingMs = await measureRelease("a 播放档", () =>
            setOutput(false),
        );
        // ---- (b) **停走档**:打印头开着,直接停走带(mock 的 PRINT 是三与,停走即停印)。
        const stoppedMs = await measureRelease("b 停走档", () =>
            setPlaying(false),
        );

        check(
            typeof playingMs === "number" && playingMs > 0,
            `(a) 播放档测到了有效读数(实得 ${playingMs})`,
        );
        check(
            typeof stoppedMs === "number" && stoppedMs > 0,
            `(b) 停走档测到了有效读数(实得 ${stoppedMs})`,
        );
        // 判据写成**区间**而不是等号:读数里含一次 CDP 往返 + 采样步长 + 一拍 render,
        // 上下各留 600ms。这两条各自都是删除式判据 ——
        //   • 退回「一个窗口打天下」:两个读数会挤到同一个数上,(c) 必红;
        //   • 调用方忘了把走带态传进 hostEchoOn:播放档掉到停走档上,(a) 必红。
        if (typeof playingMs === "number" && playingMs > 0) {
            check(
                playingMs > 1500 && playingMs < 4000,
                `(a) ★ 播放中的释放窗口落在播放档量级(实得 ${playingMs}ms,期望 ≈2500)`,
            );
        }
        if (typeof stoppedMs === "number" && stoppedMs > 0) {
            check(
                stoppedMs < 1600,
                `(b) ★ 停走后的释放窗口落在停走档量级(实得 ${stoppedMs}ms,期望 ≈900)` +
                    ` —— 用户报的「停走之后图标停留过久」就是这个数原先是 2000`,
            );
        }
        if (
            typeof playingMs === "number" &&
            typeof stoppedMs === "number" &&
            playingMs > 0 &&
            stoppedMs > 0
        ) {
            check(
                playingMs - stoppedMs > 800,
                `(c) ★ 两档确实分开(播放 ${playingMs}ms − 停走 ${stoppedMs}ms > 800ms)` +
                    ` —— 这一条是「按停即回短窗口」的直接证据,也就是用户报的第二幕` +
                    `(快速起停时残余在播放中途走完)被修掉的机理`,
            );
        }

        // ---- (d) 重按播放之后徽标必须**能回来**:短窗口是给停走用的,不能把重新开始的
        // 那一段播放也一起摁死。这一条守的是「修第一幕别修出一个新的第二幕」。
        // ⚠ 用 waitFor 而不是定长 sleep:亮起要等打印头写出一个变化的值,mock 上实测
        // 2-3 秒(本机跑过 1.5s 的定长,假红一次)。真出回归的话它永远不亮,一样红。
        await setPlaying(true);
        check(
            await waitFor(badgeOn, 15000),
            "(d) ★ 重按播放后徽标重新亮起(短窗口没有把它锁死)",
        );
        log(`  (d) 重按播放后徽标 = ${await evaluate(badge)}`);

        // ---- (e) [PR 178 复审【重要】2] **播放中没有任何人来 render** 那一幕。
        //
        // (a) 之所以量得到播放档,是因为 `scvb.playhead` 的 `timeS` 每帧在走 ⇒ 页面侧
        // `samePlayhead` 每帧判不同 ⇒ 每帧 requestRender,长窗口到期那一刻正好有 render
        // 顺手把徽标熄了。但那是**宿主的行为**,不是我们能担保的事:native 的
        // `OutputEditor::emitPlayhead` 算 timeS 用
        // `pod.timeSamples >= 0 ? samplesToSeconds(...) : 0.0` —— 宿主给了 `isPlaying`
        // 却不给 `timeInSamples` 时 timeS 恒 0.0,载荷逐帧逐字相同,native 的
        // `emitIfChanged` 与页面的 `samePlayhead` 两道去重都判「没变」,整个播放期
        // **一次 render 都不排**。那时能把徽标熄掉的只剩定时器,而只排停走档那一拍的话
        // 它在 950ms 就烧完了(闩锁还亮着,950 < 2500),之后再没有东西来 render ——
        // 徽标与 Tab2 灰显**永久滞留**。
        //
        // `ctl.setHostTimeAvailable(false)` 复现的就是这类宿主(预览专用开关,见
        // juce-bridge-mock 那一条)。采集闸不用管:`setOutputEnabled(true)` 的契约副作用
        // 已经把 capture 关了,所以 `scvb.captureProgress` 这条 2Hz 的 render 源本来就
        // 不在场 —— 本节量到的熄灭只可能来自定时器。
        //
        // ★ 删除式:去掉播放档那一拍 setTimeout,(e) 会一路采到 12s 超时(-2)当场红。
        const setHostTime = (on) =>
            evaluate(`(() => {
            const s = window.__SCVB_PREVIEW__;
            if (!s || !s.ctl || !s.ctl.setHostTimeAvailable) return "no-hook";
            s.ctl.setHostTimeAvailable(${on ? "true" : "false"});
            return "ok";
        })()`);
        const frozenMs = await measureRelease(
            "e 播放中·宿主不给走带位置",
            async () => {
                await setOutput(false);
                check(
                    (await setHostTime(false)) === "ok",
                    "(e) 前提:预览会话认得 setHostTimeAvailable(没有它这一幕造不出来)",
                );
            },
        );
        check(
            typeof frozenMs === "number" && frozenMs > 0,
            `(e) 测到了有效读数(实得 ${frozenMs};-2 = 12s 内**根本没熄**,正是回归的形状)`,
        );
        if (typeof frozenMs === "number" && frozenMs > 0) {
            check(
                frozenMs > 1500 && frozenMs < 4000,
                `(e) ★ 播放期一次 render 都不排时,徽标仍按播放档熄灭` +
                    `(实得 ${frozenMs}ms,期望 ≈2500)—— 靠的是播放档那一拍定时器`,
            );
        }
        await setHostTime(true);

        assertClean("SL-270 释放窗口");
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
