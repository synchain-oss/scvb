// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// web-preview 截图器 —— 无头 Chrome + CDP,零 npm 依赖(T33 交付)。
// -----------------------------------------------------------------------------
// 为什么要它:预览页是 ES module + 同源 iframe 注入(见 serve.ps1 文件头),
// 只能经 http://127.0.0.1:8823 打开;而「改完想看一眼」不该每次都占用人的眼睛。
// 本脚本起一个**独立 user-data-dir 的无头 Chrome**,用 DevTools 协议导航 → 切 tab
// → 等一拍 → 抓 PNG,给人看或喂给会读图的模型。
//
// 零依赖靠两件 node 内置:`fetch`(拿 CDP target 列表)与全局 `WebSocket`
// (node ≥ 22)。**不要**引入 puppeteer:仓库红线是零运行时依赖,dev 侧也照守。
//
// 注意:无头 Chrome 与真机 Chrome 并非同一套光栅路径 —— 颜色/字体/亚像素可能有
// 极小差异,**设计验收仍以真机为准**,本脚本用于快速回归与「有没有画出来」。
//
// 用法(在仓库根跑;先 pwsh web-preview/serve.ps1 起服):
//   node web-preview/shot.mjs --tab=wave --out=tab3.png
//   node web-preview/shot.mjs --role=output --scenario=recapture-armed --tab=wave
//   node web-preview/shot.mjs --tab=tracks --scale=2 --size=1500x900 --out=zoom.png
//   node web-preview/shot.mjs --tab=wave --click='[data-gb="wave-lane-3"]' --wait=800
//   node web-preview/shot.mjs --url=http://127.0.0.1:8823/web-preview/input.html
// =============================================================================

import { spawn } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

// PNG 的八字节魔数。放顶层与 CHROME_CANDIDATES 同档,不是性能考虑 —— 它和用它那处的
// 注释是一条纪律(落盘前认一次,别写出伪证据),埋在热路径中段容易被下次重构顺手挪没。
const PNG_MAGIC = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

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

// ---------------------------------------------------------------- 参数
const args = new Map();
const clicks = [];
for (const raw of process.argv.slice(2)) {
    const m = /^--([^=]+)(?:=([\s\S]*))?$/.exec(raw);
    if (!m) continue;
    if (m[1] === "click") clicks.push(m[2]);
    else args.set(m[1], m[2] === undefined ? "1" : m[2]);
}
const opt = (k, dflt) => (args.has(k) ? args.get(k) : dflt);

if (args.has("help")) {
    console.log(
        [
            "用法:node web-preview/shot.mjs [选项]",
            "  --url=<完整 URL>        直接指定;不给则按 role/fixture/scenario 拼",
            "  --role=output|input|monitor  默认 output",
            "  --fixture=<名>          empty|fifteen-tracks|misaligned|channel-conflict|second-output|stereo-mixed",
            "  --scenario=<名>         见 05 §2.5(printing / recapture-armed / print-guard / first-run …)",
            "  --lang=zh|en|fr         经 UI 切语言(不改 state 默认)",
            "  --tab=master|tracks|wave|settings   载入后点该 tab",
            "  --click=<CSS 选择器>    额外点击,可重复;按给出顺序执行",
            "  --eval=<JS>             截图前在页内求值(调试用)",
            "  --size=1500x900         视口(CSS px),默认 1500x900",
            "  --scale=1               deviceScaleFactor;看颜色/细节用 2",
            "  --wait=1500             载入后等待毫秒(mock 首帧 + 首绘)",
            "  --settle=600            每次点击后等待毫秒",
            "  --full                  整页截图(超出视口部分一并抓)",
            "  --port=9333             CDP 端口",
            "  --out=<路径>            PNG 落点,默认 web-preview/.shots/<tab>-<时间>.png",
            "  --console               把页面 console 打到 stdout(排障用)",
            "",
            "退出码:0 = 拍到了 / 1 = 其它失败 / 2 = 目标页面没能真的呈现(连不上、错误页、非 2xx、壳页报注入失败、",
            "        壳页一直没离开「未就绪」态,以及落地后页内求值抛错)。分界是「谁的锅」:抛错源自你给的输入算 1 不算 2 ——",
            "        --eval 的表达式抛错、--click/--lang/--tab 的选择器语法错,都归 1。",
        ].join("\n"),
    );
    process.exit(0);
}

const PORT = Number(opt("port", 9333));
const [VW, VH] = String(opt("size", "1500x900"))
    .split("x")
    .map((n) => Number(n) || 0);
const SCALE = Number(opt("scale", 1)) || 1;
const WAIT_MS = Number(opt("wait", 1500));
const SETTLE_MS = Number(opt("settle", 600));
const TAB = opt("tab", "");
// 三个壳页各一侧;表外取值回落 output(拼错参数不该白屏)。
// ⚠ 两次 `opt()` 的默认值**必须一致**:曾经第二次写的是 `opt("role", "")` ——
// 不传 `--role` 时判定拿到的是 "output"(在表内 ⇒ 走 then 分支),取值却拿到空串,
// URL 因此拼成 `web-preview/.html` 而 404 白屏。回落逻辑看着还在,实际从没生效过。
const ROLES = ["output", "input", "monitor"];
const ROLE = ROLES.includes(opt("role", "output"))
    ? opt("role", "output")
    : "output";

function buildUrl() {
    if (args.has("url")) return args.get("url");
    const qs = new URLSearchParams();
    // `group` / `play` 是 Monitor 壳页的参数(mock/monitor-mock.js 解析),
    // 与 Output 侧的 fixture/loop 同样只是原样透传。
    for (const k of ["fixture", "scenario", "loop", "role", "group", "play"]) {
        if (args.has(k)) qs.set(k, args.get(k));
    }
    const q = qs.toString();
    return `http://127.0.0.1:8823/web-preview/${ROLE}.html${q ? "?" + q : ""}`;
}

const URL_ = buildUrl();
const OUT =
    opt("out", "") ||
    join(
        "web-preview",
        ".shots",
        `${TAB || ROLE}-${new Date().toISOString().replace(/[:.]/g, "-")}.png`,
    );

function chromePath() {
    if (args.has("chrome")) return args.get("chrome");
    for (const p of CHROME_CANDIDATES) if (existsSync(p)) return p;
    throw new Error(
        "找不到 Chrome/Edge 可执行文件 —— 用 --chrome=<路径> 显式指定",
    );
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// [SL-290] 「没拍到」与「拍到的是错误页」要能被调用方分开:前者多半是环境/参数问题,
// 后者说明**目标服务没起**——同样是失败,但排障方向完全不同,退出码就该不同。
// 退出码:0 = 拍到了;1 = 其它失败;2 = 目标页面没能真的呈现(含错误页、非 2xx、壳页报
// 注入失败、壳页一直没离开「未就绪」态([SL-333]),以及落地后的页内求值抛错)。
// 落地后那半的分界是**谁的锅**:源自命令行给进来的
// 输入(`--eval` 表达式、`--click`/`--lang`/`--tab` 的选择器语法)算 1,其余算 2;
// 收口在 `evalLanded` 与 `clickIn`。
// 注:本脚本不被 gates / CI 调用(只在 PREVIEW-GUIDE 与 E2E-journey 里给人用),
// 所以 2 不会撞上 gate 3e 那条「退 2 当缺可选依赖打 SKIP」的读法。
class NavigationError extends Error {}

// ---------------------------------------------------------------- CDP 小客户端
function cdpConnect(wsUrl) {
    const ws = new WebSocket(wsUrl);
    const pending = new Map();
    const listeners = [];
    let id = 0;
    ws.addEventListener("message", (ev) => {
        const msg = JSON.parse(ev.data);
        if (msg.id && pending.has(msg.id)) {
            const { resolve, reject } = pending.get(msg.id);
            pending.delete(msg.id);
            msg.error
                ? reject(new Error(msg.error.message))
                : resolve(msg.result);
        } else if (msg.method) {
            for (const fn of listeners) fn(msg);
        }
    });
    const ready = new Promise((resolve, reject) => {
        ws.addEventListener("open", resolve, { once: true });
        ws.addEventListener("error", () => reject(new Error("CDP 连接失败")), {
            once: true,
        });
    });
    return {
        ready,
        on: (fn) => listeners.push(fn),
        send(method, params) {
            const mid = ++id;
            return new Promise((resolve, reject) => {
                pending.set(mid, { resolve, reject });
                ws.send(
                    JSON.stringify({ id: mid, method, params: params || {} }),
                );
            });
        },
        close: () => ws.close(),
    };
}

/**
 * 页内注入 `window.__D` = **真源文档**。
 * 预览页是「壳页 + 同源 iframe」(serve.ps1 文件头:iframe 才能注入 mock),
 * UI 的一切节点都在 iframe 里 —— 顶层文档只有工具条。选择器一律走 __D,
 * 否则 `[data-tab-btn]` 永远找不到(本脚本第一版就栽在这)。
 */
const DOC_PRELUDE = `window.__D = (() => {
    const f = document.querySelector("iframe");
    try { return (f && f.contentDocument) || document; } catch { return document; }
})(), 1`;

/**
 * [SL-312] **页内求值的唯一入口**(落地之后的求值一律走这里)。
 *
 * 为什么要收成一个函数:裸的 `Runtime.evaluate` 包装抛的是普通 `Error`,冒到 finally 时
 * `instanceof NavigationError` 不成立 ⇒ 退 1 而不是 2,屏幕上还打印「页内求值抛错」
 * 把人指向错误方向;而落地页上连求值都跑不了,几乎只可能是页面没正常起来 —— 正是
 * 2 该覆盖的语义。同一条纪律在 #194 里补过两轮(第 1 轮补 `location.href`、第 4 轮
 * 补壳页状态那两处),夹在中间的 `DOC_PRELUDE` 那次三轮都没被看见:每轮只补复审
 * 点名的位置,漏的那处就一直漏着。所以修法不是「再补第三处」,而是**把包法收进
 * 唯一入口**,调用点只给一句「这一步在做什么」,少一个能忘的地方。
 *
 * ⚠ 裸包装 `evaluateRaw` **关在这个闭包里**,外面拿不到它。放在顶层时它仍是个名字更短、
 *   签名更少一个参数的函数,下一个人加第六处求值时它依然是最顺手的写法 ——「少一个能忘
 *   的地方」就只做了一半。
 *
 *   **射程仅此:堵死的是最顺手的那条歧路,不是全称保证。** `cdp` 是模块级 `let`,
 *   文件里任何位置都写得出
 *   `cdp.send("Runtime.evaluate", { expression, returnByValue: true, awaitPromise: true })`,
 *   它一样绕过分档 —— 但那要把 `Runtime.evaluate` 的三个选项连着手抄一遍,已经是
 *   要绕的人自己动手写,不叫顺手了。别把这一段读成「只剩一条路」(与 `CLAUDE.md` 里
 *   `check-native-paths.mjs` 那段同一口径:边界要写成边界)。
 *   [SL-332] **这件事全文件只在这里说一次**,调用点不复述 —— 原来两处各写一遍,
 *   改一处就会在另一处留个旧副本,而那正是这条改动自己要治的毛病。
 *
 * 唯一例外是 `--eval`:表达式是人现给的,抛错是这句表达式自己的锅、不是页面没起来,
 * 退 1 才对。这个例外由 `userExpr: true` **显式声明**,而不是靠调用点绕开本函数 ——
 * 绕开就又回到「有人会忘」的老形态,声明则会在调用点上留下痕迹。
 */
const evalLanded = (() => {
    /** 页内求值(返回 JSON 可序列化值);抛错时把页面异常原样冒上来。 */
    async function evaluateRaw(cdp, expression) {
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

    return async function evalLanded(
        cdp,
        expression,
        what,
        { userExpr = false } = {},
    ) {
        try {
            return await evaluateRaw(cdp, expression);
        } catch (e) {
            // 两支只差**错误类**(它决定退 1 还是退 2);`what` 两支都带上 —— 否则
            // `--eval` 抛错时打印的文案与探针抛错一字不差,读的人分不出这是谁的锅。
            // `cause` 两支都挂:打印路径只用 `e.message`,一字不变;但页内异常的完整
            // description 从此还在手里,不用再开 `--console` 重跑一次(复审第 1 轮)。
            const why = `${what}(${e.message})`;
            if (userExpr) throw new Error(why, { cause: e });
            throw new NavigationError(`${why},目标 ${URL_} 大概率没正常起来`, {
                cause: e,
            });
        }
    };
})();

// ---------------------------------------------------------------- 主流程
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-shot-"));
const chrome = spawn(
    chromePath(),
    [
        "--headless=new",
        `--remote-debugging-port=${PORT}`,
        `--user-data-dir=${userDataDir}`,
        `--window-size=${VW},${VH}`,
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-extensions",
        "--disable-background-timer-throttling",
        "--force-device-scale-factor=1",
        "about:blank",
    ],
    { stdio: "ignore" },
);

let cdp = null;
let failed = null;
try {
    // 等 CDP 起来(HttpListener 式轮询;Chrome 冷启动约 0.3–2s)
    let targets = null;
    for (let i = 0; i < 60 && !targets; i++) {
        try {
            const res = await fetch(`http://127.0.0.1:${PORT}/json/list`);
            const list = await res.json();
            targets = list.find((t) => t.type === "page") ? list : null;
        } catch {
            await sleep(200);
        }
    }
    if (!targets) throw new Error("Chrome 未在 12s 内开出 CDP 端口");

    const page = targets.find((t) => t.type === "page");
    cdp = cdpConnect(page.webSocketDebuggerUrl);
    await cdp.ready;

    await cdp.send("Page.enable");
    await cdp.send("Runtime.enable");
    if (args.has("console")) {
        cdp.on((msg) => {
            if (msg.method === "Runtime.consoleAPICalled") {
                const text = (msg.params.args || [])
                    .map((a) => a.value ?? a.description ?? "")
                    .join(" ");
                console.log(`[页面 ${msg.params.type}] ${text}`);
            }
            if (msg.method === "Runtime.exceptionThrown") {
                console.log(
                    "[页面 异常] " +
                        (msg.params.exceptionDetails.exception?.description ||
                            msg.params.exceptionDetails.text),
                );
            }
        });
    }
    // 视口与 dpr 走 CDP 覆盖(比命令行 --window-size 准,且能给 scale>1)
    await cdp.send("Emulation.setDeviceMetricsOverride", {
        width: VW,
        height: VH,
        deviceScaleFactor: SCALE,
        mobile: false,
    });

    const loaded = new Promise((resolve) => {
        cdp.on((m) => {
            if (m.method === "Page.loadEventFired") resolve();
        });
    });

    // [SL-290 复审] **第三道:HTTP 状态码**。前两道只覆盖「连不上」——
    // serve 起着但 URL 拼错时(404/500),`Page.navigate` **不带** errorText
    // (HTTP 错误不是导航失败),`location.href` 也**不是** chrome-error://,
    // 于是 404 白屏照样被拍成 PNG 并报 ✅ —— 与本卡要治的伪证据**完全同一类**。
    // 不是假想:PREVIEW-GUIDE §5 排障表第一行就是「页面 404 | URL 写成了 /output/」;
    // 本文件上面 `opt("role","")` 那段注释记的事故,形态正是「拍出白屏图并报成功」。
    // 实测:`--url=…/nope.html` 修复前落盘 7550 字节且 exit=0。
    //
    // ⚠ **Document 响应不止一条**:壳页给 iframe 赋的是**真实 src**(shell.js `frame.src = url`),
    //    子 frame 的文档导航在 CDP 里同样是 `type === "Document"`。所以这里**按主 frame 的
    //    frameId 挑**,而不是「取首条」——「主文档必先于 iframe 到」是个**到达顺序假设**,
    //    把它当成「只有一条」写进注释,下一个人重构时就会静默拿到 iframe 的状态码
    //    (复审第 2 轮指出:上一版正是这么写的,结论碰巧对、理由是假的)。
    const docResponses = [];
    cdp.on((m) => {
        if (
            m.method === "Network.responseReceived" &&
            m.params.type === "Document"
        ) {
            docResponses.push({
                frameId: m.params.frameId,
                status: m.params.response.status,
                url: m.params.response.url,
            });
        }
    });
    await cdp.send("Network.enable");
    // [SL-290] **导航结果必须校验** —— 这是本脚本最贵的一课:
    // `Page.navigate` 对连不上的目标**照样 resolve**,`Page.loadEventFired` 也照样来
    // (Chrome 把错误页当一个正常文档加载完),于是下面一路走到 captureScreenshot,
    // 把「无法访问此网站」那张图存成 PNG 并打印 ✅。实测(serve 没起时)两次出图
    // **sha256 逐字相同、均 23767 字节** —— 而截图是「肉眼验过」这类结论的唯一证据载体,
    // 工具谎报成功等于**产出伪证据**(v5.6.6 出包自测差一点把两张错误页当验证结果上报)。
    //
    // **思路**与页面级冒烟同一档(`web-preview/tests/smoke-*-page.mjs`:导航之后必须验
    // 「页面到底成没成」),但**机制并不同源** —— 那边用**正向断言**(`waitFor(READY)`、
    // `querySelector("#card")`),这边用 CDP 的**导航/HTTP 信号**。
    // 上一版这里写的是「取与页面级冒烟同一套判据」,那是假话:全仓核过,
    // `smoke-*-page.mjs` 里 `errorText` / `chrome-error` / `responseReceived` **零命中**
    // (复审第 2 轮指出)。按字面读会让下一个人以为两边共用一套,改一边时去对拍另一边。
    // 下面两个信号都与界面语言无关:
    //   ① `Page.navigate` 的返回值带 `errorText`(如 `net::ERR_CONNECTION_REFUSED`);
    //   ② 错误页的 `location.href` 是 `chrome-error://chromewebdata/`。
    // 两条都测过(见本卡 PR 描述里的探针输出)。**不按 body 文案判**:错误页文案随
    // 浏览器界面语言变(本机实测是中文「无法访问此网站」),按文案判就是给自己埋 locale 坑。
    const nav = await cdp.send("Page.navigate", { url: URL_ });
    if (nav && nav.errorText) {
        throw new NavigationError(
            `导航失败:${nav.errorText}
   目标:${URL_}
` + "   预览页要先起服务:pwsh web-preview/serve.ps1",
        );
    }
    await Promise.race([loaded, sleep(15000)]);
    await sleep(WAIT_MS); // mock 注入 + 首帧事件 + 首绘

    // ② 二道:errorText 只覆盖「这一次 navigate 自己失败」。重定向到错误页、
    //    或首次导航成功但随后被换成错误页,都还得靠落地后的 URL 认。
    // ⚠ 走 `evalLanded`(见其函数头):「在落地页上连 location.href 都求不到」几乎
    //    只可能是页面没正常起来,该退 2。为什么这里不该自己去调裸包装、以及那道收口的
    //    射程到哪为止,写在 `evalLanded` 的函数头,本处不复述。
    const landed = await evalLanded(cdp, "location.href", "落地页无法求值");
    if (typeof landed === "string" && landed.startsWith("chrome-error://")) {
        throw new NavigationError(
            `落到浏览器错误页(${landed}),目标 ${URL_} 没打开
` + "   预览页要先起服务:pwsh web-preview/serve.ps1",
        );
    }
    // ③ 主文档非 2xx ⇒ 拍下来的是错误页/白屏,不是要看的页面。
    //    按 `Page.navigate` 回的 frameId 认主 frame,不依赖到达顺序。
    let mainDoc = docResponses.find((d) => d.frameId === nav.frameId);
    // ⚠ `frameId` 在 CDP 里是 **optional**:拿不到时上面的 find 返回 undefined ⇒ mainStatus
    //    为 null ⇒ 下面那道 `!== null` 短路 ⇒ **第三道整条静默消失**,404 又会被拍成 ✅。
    //    上一版「取首条」虽然理由是假的,但判据一定带电;换成 frameId 精确匹配反而多了一个
    //    fail-open 前提。所以退回「首条 Document」兜底,并**出声**——判据可以降精度,
    //    但不能悄悄不在(复审第 4 轮;这正是本卡开头那句「工具没报错不等于验过」)。
    if (!mainDoc && docResponses.length === 0) {
        console.warn("⚠ 本次没收到任何 Document 响应,第三道未生效");
    }
    if (!mainDoc && docResponses.length > 0) {
        mainDoc = docResponses[0];
        console.warn(
            "⚠ responseReceived 未带 frameId,第三道退回「取首条 Document」(精度降一档,判据仍在)",
        );
    }
    const mainStatus = mainDoc ? mainDoc.status : null;
    if (mainStatus !== null && (mainStatus < 200 || mainStatus > 299)) {
        throw new NavigationError(
            `主文档 HTTP ${mainStatus},目标 ${URL_} 没正常返回(多半是 URL 拼错,见 PREVIEW-GUIDE §5 排障表)`,
        );
    }

    // 之后一切选择器走 __D(iframe 内的真源文档)。这一处正是 #194 三轮都没被看见的
    // 那个漏包点(SL-312):它夹在两个已包 try 的调用中间,抛错时退 1 不退 2。
    await evalLanded(cdp, DOC_PRELUDE, "注入 __D(真源文档)失败");

    // ④ [SL-290 复审第 2/3/4 轮] **第三道只看主文档,iframe 那半还漏着**:壳页自己没有 UI
    //    (`output.html` 除工具条外画面 100% 来自 iframe 里的 `web/<role>/index.html`),
    //    所以真源页被挪走/改名时,壳页照样 **200** ⇒ 上面那道放行 ⇒ 拍下一张**空舞台**报 ✅
    //    —— 与开头那张 404 白屏是同一类伪证据,只是位置从主文档挪到了 iframe。
    //
    //    **不自造判据:壳页自己已经算出过这个结论,读它就行。** `shell.js` 的 `setStatus()`
    //    把 `.pv-status` 的 `data-ok` 写成 1/0/wait,`"0"` 只在**真失败**时出现 —— 现有三条
    //    路径(`grep -n "ok: false" web-preview/shell.js`:注入重试耗尽 / mock 后端不可用 /
    //    `driver.start()` 抛错)。**别在消息里猜是哪一条**:第三条是在 `wired > 0` 之后触发的,
    //    也就是注入其实成功了、真源页也取到了 —— 猜「index.html 取不到」会把人指反方向。
    //    壳页的 `textContent` 已经把它自己的原话带出来了,让它说话就够(复审第 4 轮)。
    //    也不会误杀 `--url` 指向的非预览页:那些页面没有 `.pv-status`,取到 null ⇒ 放行。
    //
    //    ⚠ 两个属性**一次读回**:分两次读时,第二次(取原话)是在**已经判负之后**跑的
    //    —— 它一抛错,`throw new NavigationError` 就再也执行不到,**判负的证据在手里
    //    却打印不出来**。求值本身走 `evalLanded`(退 2,见其函数头)。
    //    ⚠ [SL-333] **只拦 `"0"` 是不够的:壳页还有个「未就绪」态,而它是默认值。**
    //    `setStatus()` 把 `ok === null` 写成 `data-ok="wait"`,三个壳页的 HTML 里
    //    `.pv-status` **初始就带 `data-ok="wait"`**(文案「初始化…」)。所以「还没就绪」
    //    与「就绪且成功」在旧判据下**长得一样**:两者都不是 `"0"`,一律放行 ⇒ 拍下一张
    //    中间态(甚至是壳页从头到尾没起来的那张)报 ✅ —— 又一张伪证据。
    //    `shell.js` 的注入重试(`wired === 0` ⇒ 重导航 iframe)期间同样停在这一档。
    //
    //    修法是**等它落定再判**,不是一见 `"wait"` 就判负:重试窗口本来就是设计的一部分,
    //    立刻判负会把一次正常的慢注入变成红。落定 = `data-ok` 变成 `"1"` 或 `"0"`。
    //    预算是**经验值,量级对齐** `shell.js` 的 `PUMP_DEADLINE_MS`(15s)—— **不是推导出来的
    //    上界**(复审第 1 轮纠正,我核过):`PUMP_DEADLINE_MS` 管的只是注入泵,泵到点**只停泵、
    //    不写状态**;终态是在 iframe 的 `load` 处理器里设的,而 `load` 一直不来时 `shell.js`
    //    **没有超时兜底**,`.pv-status` 会永久停在 `wait`。所以别把这个数读成「照
    //    `PUMP_DEADLINE_MS` 调就一定够」。
    //    **边界照实说**:把 `maxRetries` 调大、或真需要第二次重试时会超出这个预算,那时按超时
    //    判负,消息里写明是「一直没离开未就绪态」而不是「注入失败」,免得把人指向
    //    `shell.js` 的失败路径去查。
    const SHELL_SETTLE_MS = 16000;
    const readShell = () =>
        evalLanded(
            cdp,
            `(() => { const e = document.querySelector(".pv-status");
                  return e ? { ok: e.getAttribute("data-ok"), why: e.textContent } : null; })()`,
            "落地页读不到壳页状态",
        );
    // 落定判据写成「是不是那两个终态之一」而不是「是不是 wait」:属性缺失(取到 null)、
    // 或将来多出一档新状态,都该继续等而不是当成成功放行。
    const settled = (s) => !s || s.ok === "1" || s.ok === "0";
    let shell = await readShell();
    // ⚠ [SL-333 复审第 1 轮] `null` 那一支要**分档**,不能一律当「这不是预览页」放行。
    //    没给 `--url` 时 URL 一定是 `buildUrl()` 拼的壳页,而 `.pv-status` 是壳页 HTML 里
    //    **写死的静态节点** —— 这时取不到它,只可能是壳页自己变了(class 改名、工具条被重构
    //    掉、或这个路径被喂了别的东西),那是**第四道整条被拆掉**,不是「不是预览页」。
    //    照本文件第三道立的规矩:判据可以降精度,但不能悄悄不在。
    //    给了 `--url` 才是合法的「打非预览页」,继续放行(本卡的 ⑤ 号格守的就是它)。
    if (!shell && !args.has("url")) {
        throw new NavigationError(
            `壳页 ${URL_} 上找不到 .pv-status —— 第四道判据失去着力点(壳页 HTML 变了?)`,
        );
    }
    const settleBy = Date.now() + SHELL_SETTLE_MS;
    while (!settled(shell) && Date.now() < settleBy) {
        await sleep(250);
        shell = await readShell();
    }
    if (shell && !settled(shell)) {
        throw new NavigationError(
            `壳页 ${SHELL_SETTLE_MS / 1000}s 内没离开「未就绪」态(data-ok=${shell.ok})——` +
                `拍下去只会是中间态。壳页原话:${shell.why}`,
        );
    }
    // 到这里只剩 `"1"` / `"0"` / 非预览页(null)三种。**判负写成「不是 "1"」而不是「是 "0"」**:
    // 上面已经把非终态挡在外面,剩下的任何非 `"1"` 值都不该放行。
    if (shell && shell.ok !== "1") {
        throw new NavigationError(
            `壳页报注入失败 —— 主文档 ${mainStatus ?? "状态未取到"} 但舞台是空的。壳页原话:${shell.why}`,
        );
    }

    // 点击也在落地之后:`window.__D` 没注入成功时这句会抛,那属于「页面没起来」⇒ 退 2。
    // ⚠ 但**选择器语法错是人给的输入**,与 `--eval` 同类,不该算页面没起来。三个入口都能
    //    喂进非法选择器:`--click='div['` 直接透传,`--lang` / `--tab` 下面两处是原样插值
    //    (`--tab=a"b` ⇒ `[data-tab-btn="a"b"]`)。所以在**页内**就把这一种分出来:
    //    `querySelector` 对非法选择器抛的是 name 为 `SyntaxError` 的 DOMException,只截这
    //    一种回传标记(退 1);`__D` 没注入那种是 `TypeError`,原样抛出去让 `evalLanded` 接
    //    (退 2)。**别整段 catch** —— 那会把「页面没起来」也误报成「选择器写错了」。
    //
    //    [SL-332] **按 `e.name` 判,不是写得不够严谨,而是这里成本最低的那条。**
    //    `DOC_PRELUDE` 有兜底:有 iframe 且
    //    `contentDocument` 取得到时 `__D` 是**iframe 的文档**,否则(`--url` 打的是非预览
    //    页,或取不到)回落成**顶层 document**。异常对象产在 `__D` 所属的那个 realm,而
    //    这段代码跑在顶层 —— 于是 `instanceof` 的真假**随分支翻转**。本机 `--eval` 探针
    //    实测两条分支:
    //      预览页 output.html(`__D === document` 为 false):
    //        name = "SyntaxError";instanceof DOMException = **false**;
    //        instanceof Error = **false**;`__D.defaultView.DOMException` 那个才 true。
    //      `--url=…/web/output/index.html`(页面无 iframe,`__D === document` 为 true):
    //        name = "SyntaxError";instanceof DOMException = **true**;instanceof Error = true。
    //    所以把这句「改严谨」成 `instanceof DOMException`(或 `instanceof Error`)拿到的不是
    //    恒假、而是**时灵时不灵**的判据:预览页上失效(语法错掉回退 2,正好退回 #215 刚修掉
    //    的形态),而拿 `--url` 打个普通页面去复现时它又是 true —— 复现不出来的人会以为
    //    这条注释写错了。看情况假的判据比恒假的更难查,这才是不能用它的理由。
    //    **别把上面读成「只有 name 这一条可用」**:`e instanceof __D.defaultView.DOMException`
    //    两条分支下也都成立(兜底时 `__D.defaultView === window`,就是顶层那个)。不选它是因为
    //    它得先摸到对端 realm,而 `defaultView` 在文档被 detach 后是 `null` —— 实测把 iframe
    //    `remove()` 掉之后,`__D.defaultView` 从 **iframe 的那个 window** 变成 null,
    //    那一句当场 `TypeError: Cannot read properties of null (reading 'DOMException')`。
    //    ⚠ **这一段说的是那条备选写法多担的一份前提,不是在报告本脚本的已知缺陷** ——
    //    别顺着它去 diff 里找 bug。这里**不去穷举「有没有路径能走到 detach」**:入口不止
    //    一个(上面那次是手工 `--eval` 造的;壳页自己的注入重试也算 —— `shell.js` 的
    //    `navigate()` 走 `frame.src = url`,导航同样 discard 掉旧 Document),
    //    而穷举出来的「没有任何路径」正是本卡前几轮反复写错的那种全称句。
    //    真撞上时退码仍是 2(`TypeError` 照样被 `evalLanded`
    //    收成 `NavigationError`),难查的是**报错文案指向 `DOMException` 这个和病因无关的
    //    名字**。所以是「成本最低」,不是「唯一」。
    //    另:`--tab` 选择器合法但没命中时下面是 `throw new Error`(退 1),语法错也落 1,
    //    同一个参数的两种打字错这才在同一个码上(复审第 1 轮)。
    const clickIn = async (sel) => {
        const r = await evalLanded(
            cdp,
            `(() => { let el;
                      try { el = window.__D.querySelector(${JSON.stringify(sel)}); }
                      catch (e) { if (e && e.name === "SyntaxError") return { badSel: String(e.message) }; throw e; }
                      if (!el) return false; el.click(); return true; })()`,
            `页内点击 ${sel} 失败`,
        );
        // 只有语法错这一支会回传对象,当场抛掉 —— 所以调用点拿到的仍只有 true/false。
        // ⚠ 按**有没有这个键**判,不按值真不真:`e.message` 万一是空串,真值判断会放行,
        //    而放行交出去的是 `{ badSel: "" }` 这个**恒真的对象** —— `if (!hit)` / `if (!ok)`
        //    读它都成立,于是 tab 根本没点成却照样截图退 0。现实里 Chrome 的选择器
        //    DOMException 是固定模板、不会空,但这正是本文件 §③ 那句「判据可以降精度,
        //    不能悄悄不在」说的形状(复审第 2 轮)。
        if (r && typeof r === "object" && "badSel" in r)
            throw new Error(`选择器语法错:${sel}(${r.badSel})`);
        return r;
    };

    if (args.has("lang")) {
        await clickIn(`[data-lang="${args.get("lang")}"]`);
        await sleep(SETTLE_MS);
    }
    if (TAB) {
        const hit = await clickIn(`[data-tab-btn="${TAB}"]`);
        if (!hit) throw new Error(`页面上找不到 tab 按钮:${TAB}`);
        await sleep(SETTLE_MS);
    }
    for (const sel of clicks) {
        const ok = await clickIn(sel);
        if (!ok) console.warn(`⚠ 选择器无命中,已跳过:${sel}`);
        await sleep(SETTLE_MS);
    }
    if (args.has("eval")) {
        // `--eval` 是人现给的表达式:抛错是这句表达式的锅,不是页面没起来 ⇒ 保持退 1。
        // 走 `evalLanded` 而不是绕开它,是为了把这个例外**写在调用点上**(见其函数头)。
        const v = await evalLanded(cdp, args.get("eval"), "--eval 表达式抛错", {
            userExpr: true,
        });
        console.log("eval →", JSON.stringify(v));
        await sleep(SETTLE_MS);
    }

    // --clip=x,y,w,h:只抓一块并按 scale 放大 —— 看颜色/角标/线宽这类细节时,
    // 整屏缩略图会把 1px 差异糊掉,必须裁切放大看(真机 QA 同款纪律)。
    let clip;
    if (args.has("clip")) {
        const [x, y, w, h] = String(args.get("clip"))
            .split(",")
            .map((n) => Number(n) || 0);
        if (!(w > 0 && h > 0)) throw new Error("--clip 需要 x,y,w,h 四个数");
        clip = { x, y, width: w, height: h, scale: SCALE };
    }
    const shot = await cdp.send("Page.captureScreenshot", {
        format: "png",
        captureBeyondViewport: args.has("full"),
        ...(clip ? { clip } : {}),
    });
    const { mkdirSync } = await import("node:fs");
    const { dirname } = await import("node:path");
    const png = Buffer.from(shot.data, "base64");
    // 落盘前认一下这确实是 PNG:空/截断的 capture 也会 resolve,而写出一个 0 字节的
    // .png 同样是伪证据。八字节魔数比「尺寸下界」稳 —— 尺寸下界会把合法的小截图误杀。
    if (png.length < 8 || !png.subarray(0, 8).equals(PNG_MAGIC)) {
        throw new Error(`captureScreenshot 返回的不是 PNG(${png.length} 字节)`);
    }
    mkdirSync(dirname(OUT), { recursive: true });
    writeFileSync(OUT, png);
    console.log(`✅ ${URL_}${TAB ? "  (tab=" + TAB + ")" : ""}\n   → ${OUT}`);
} catch (e) {
    failed = e;
    console.error("❌ 截图失败:" + e.message);
} finally {
    try {
        cdp?.close();
    } catch {}
    chrome.kill();
    await sleep(300);
    try {
        rmSync(userDataDir, { recursive: true, force: true });
    } catch {}
    process.exit(failed ? (failed instanceof NavigationError ? 2 : 1) : 0);
}
