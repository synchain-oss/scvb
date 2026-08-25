// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Monitor —— **页面级**冒烟(无头 Chrome + CDP;T46)
// -----------------------------------------------------------------------------
// 为什么非有它不可 —— 两个真实的例子,都是同一类错:
//
//   ① `onViz()` 里写了 `vizPlayheadEvent(viz)`,而 `viz` 是 `render()` 的块级局部量,
//      在那个作用域里根本不存在 ⇒ **ReferenceError**。它恰好落在「首帧可读且还没收到
//      playhead」这唯一一拍上,之后 25Hz 的 playhead 一到就再也不进那个分支 ——
//      于是页面看起来完全正常,只是每次装载白扔一帧。
//   ② `pagehide` 处理器第一句是 `clearTimeout(staleTimer)`,而 `staleTimer` 早在判据
//      改归 native 时就删掉了 ⇒ 关窗必抛 ⇒ **同一个处理器里的 `traj.destroy()` 一次都
//      没跑过**。而 node 侧的 smoke 还有一条 `/clearTimeout\(staleTimer\)/` 的源码正则
//      当「不留孤儿 timer」的证据钉着,全绿。
//
// 两条都逃过了 `smoke-monitor.mjs`,因为**那边从不执行 app.js**:仓内零 node_modules
// (无 jsdom/linkedom),页面接线只能靠源码正则去「看」。源码正则能证明的只有「字符在」,
// 证明不了「代码跑得通」。而这两条错的共同点是:**画面上看不出来**,DAW 里更看不出来。
//
// 本脚本因此把真页面在无头 Chrome 里跑起来,断言两件 node 侧永远断言不到的事:
//   • **零未捕获异常、零 console.error**(逐场景分桶,含 pagehide/换组/换语言这些
//     只在真事件里才走到的路径);
//   • **投影结果与 DOM 的数值**(`__SCVB_MONITOR__.snapshot()` + 柱数/图例行数/
//     横幅可见性/CSS 空态闸的 computed display)。
//
// CDP 那套连接方式与 `web-preview/shot.mjs` 同源(node 内置 fetch + WebSocket,零依赖 ——
// 仓库红线是不引 puppeteer)。**没有抽公共模块**:那份是给人用的截图 CLI,本份是给机器
// 用的断言器,合并只会让两边都被对方的参数面绑住;这里只搬了那 30 行连接逻辑。
// 静态服务也自带(node http,临时端口)—— 断言器不该要求谁先手动把 serve.ps1 起起来。
//
// 用法:node web-preview/tests/smoke-monitor-page.mjs [仓库根绝对路径]
//   --keep-open   不杀 Chrome(排障用)
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;**2 = 环境里没有 Chrome/Edge**(与失败区分开:
//   本机没浏览器不是代码错,但也绝不能当成「通过」静默混过去)。
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
    process.argv.find((a) => !a.startsWith("--") && a.includes(":\\")) &&
    process.argv[2] &&
    !process.argv[2].startsWith("--")
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

// ---------------------------------------------------------------- 断言器
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
// 站点根 = 仓库根:壳页用 `../web/monitor/index.html` 引真源,两棵目录必须同源
// (mock 注入靠同源 iframe,见 web-preview/shell.js 文件头)。
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
    // 目录逃逸闸:URL 里的 `..` 不该能读到仓库外的文件
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
const HTTP_PORT = server.address().port;
const base = `http://127.0.0.1:${HTTP_PORT}`;

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

/** 缺浏览器 ⇒ 退出码 2(与「断言失败」的 1 分开;口径见文件头与 CLAUDE.md §6)。 */
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
    // `--chrome` 给了就**只认它**,但要先确认存在 —— 直接丢给 spawn 会炸成
    // 「Unhandled 'error' event」那种看不懂的栈,而它本质上就是「没有浏览器」。
    if (argv.has("chrome")) {
        const p = argv.get("chrome");
        if (!existsSync(p)) noBrowser(`--chrome 指定的路径不存在:${p}`);
        return p;
    }
    for (const p of CHROME_CANDIDATES) if (existsSync(p)) return p;
    noBrowser("本机找不到 Chrome/Edge");
    return null; // 到不了(noBrowser 已退出),写着让读的人不必回头找
}

const exe = chromePath();

// CDP 端口随机取:`shot.mjs` 固定用 9333,而本机同时跑两个 agent 的门禁并不罕见 ——
// 固定端口一撞,失败形态是「Chrome 起不来」这种看不懂的超时。HTTP 那一路已经用了
// 临时端口(listen 0),这里也别留死数字。
const CDP_PORT = Number(
    argv.get("cdp") || 9400 + Math.floor(Math.random() * 400),
);
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-monitor-page-"));
const chrome = spawn(
    exe,
    [
        "--headless=new",
        `--remote-debugging-port=${CDP_PORT}`,
        `--user-data-dir=${userDataDir}`,
        "--window-size=1200,900",
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-extensions",
        // 后台节流会让 rAF/定时器在无头下被压到 1Hz,4Hz 的 mock 帧就等不到了
        "--disable-background-timer-throttling",
        "--disable-backgrounding-occluded-windows",
        "--disable-renderer-backgrounding",
        "--force-device-scale-factor=1",
        // CI 的 runner 里 Chrome 的沙箱常常起不来(容器/无 user namespace)。只在 CI 上关,
        // 本机保持默认 —— 页面只加载 127.0.0.1 上的本仓库文件,但没必要平时也松一档。
        ...(process.env.CI ? ["--no-sandbox"] : []),
        "about:blank",
    ],
    { stdio: "ignore" },
);
// 起不来(权限 / 镜像里其实没这个可执行 / 沙箱拒绝)也归「缺依赖」那一档 ——
// 不挂这个监听器的话,node 会把它变成 Unhandled 'error' event 直接崩,
// 退出码与「断言失败」混成一个,门禁那边就分不出该不该判红。
chrome.on("error", (e) => noBrowser(`浏览器启动失败:${e.message}`));

let cdp = null;
let crashed = null;

// 逐场景分桶的页面诊断(未捕获异常 / console.error / console.warn)
let bucket = { label: "启动", errors: [], warns: [], exceptions: [] };
const newBucket = (label) => {
    bucket = { label, errors: [], warns: [], exceptions: [] };
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

/** 轮询直到页内表达式为真(默认 10s);超时不抛,由调用方的断言去报。 */
async function waitFor(expr, ms = 10000) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {
        let v = null;
        try {
            v = await evaluate(expr);
        } catch {
            v = null;
        }
        if (v) return true;
        await sleep(100);
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
    const gb = (n) => q('[data-gb="' + n + '"]');
    const M = w.__SCVB_MONITOR__;
    ${js}
})()`;

const SNAP = IN(`return M ? M.snapshot() : null;`);

/** 一次性把要断言的 DOM 事实取回来(减少往返,也让失败时能一眼看全)。 */
const PROBE = IN(`
    const card = q("#card");
    const disp = (el) => (el ? w.getComputedStyle(el).display : "(缺节点)");
    const vis = (el) => !!el && !el.hidden;
    const empty = gb("monitor-empty");
    return {
        cardOnline: card ? card.getAttribute("data-online") : null,
        distBars: all(".dist-bar").length,
        distLeadCaps: all('.dist-bar[data-lead="1"]').length,
        distSpans: all(".dist-span").length,
        legendItems: all(".chart-legend__item").length,
        abiBanner: vis(gb("monitor-banner-abi")),
        stalledBanner: vis(gb("monitor-banner-stalled")),
        emptyTitle: vis(gb("monitor-empty-title")),
        emptyText: vis(gb("monitor-empty-text")),
        emptyTextKey: (gb("monitor-empty-text") || {}).getAttribute
            ? gb("monitor-empty-text").getAttribute("data-t")
            : null,
        emptyTextContent: (gb("monitor-empty-text") || {}).textContent || "",
        trajEmptyHidden: !!(gb("monitor-traj-empty") || {}).hidden,
        trajEmptyKey: gb("monitor-traj-empty")
            ? gb("monitor-traj-empty").getAttribute("data-t")
            : null,
        trajEmptyText: (gb("monitor-traj-empty") || {}).textContent || "",
        emptyPanelDisplay: disp(empty),
        trajCardDisplay: disp(q(".mon-chart--traj")),
        distCardDisplay: disp(q(".mon-chart--dist")),
        groupPressed: all("[data-group]")
            .filter((b) => b.getAttribute("aria-pressed") === "true")
            .map((b) => Number(b.getAttribute("data-group"))),
        groupDots: all("[data-group]")
            .filter((b) => b.getAttribute("data-online") === "1")
            .map((b) => Number(b.getAttribute("data-group"))),
        version: (gb("monitor-version") || {}).textContent || "",
        scaleConfirm: vis(gb("monitor-scale-confirm")),
        writeControls: all("input, textarea, [contenteditable='true']").length,
        langPressed: all("[data-lang]")
            .filter((b) => b.getAttribute("aria-pressed") === "true")
            .map((b) => b.getAttribute("data-lang")),
    };
`);

const clickIn = (sel) =>
    evaluate(
        IN(
            `const el = q(${JSON.stringify(sel)}); if (!el) return false; el.click(); return true;`,
        ),
    );

/** 打开一个场景,等到投影稳定;返回 `{snap, probe}`。 */
async function open(scenario, readyExpr, extraQuery = "") {
    await cdp.send("Page.navigate", { url: "about:blank" });
    await sleep(120);
    newBucket(scenario + (extraQuery ? ` ${extraQuery}` : ""));
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/monitor.html?scenario=${scenario}${extraQuery}`,
    });
    // 先等页面把测试面挂出来,再等本场景自己的稳定判据
    const up = await waitFor(IN(`return !!M;`));
    check(up, `${scenario}:页面装载并挂出 __SCVB_MONITOR__ 测试面`);
    if (readyExpr) {
        const ok = await waitFor(readyExpr);
        check(ok, `${scenario}:等到了本场景的稳定态`);
    }
    return { snap: await evaluate(SNAP), probe: await evaluate(PROBE) };
}

/** 本场景内页面必须**零未捕获异常、零 console.error**。 */
function assertClean(label) {
    check(
        bucket.exceptions.length === 0,
        `${label}:零未捕获异常(实得 ${bucket.exceptions.length} 条:${bucket.exceptions.join(" | ").slice(0, 400)})`,
    );
    check(
        bucket.errors.length === 0,
        `${label}:零 console.error(实得 ${bucket.errors.length} 条:${bucket.errors.join(" | ").slice(0, 400)})`,
    );
    if (bucket.warns.length) {
        log(
            `  (warn ×${bucket.warns.length}:${bucket.warns[0].slice(0, 160)})`,
        );
    }
}

const RANGE15 = Array.from({ length: 15 }, (_, i) => i + 1);

try {
    // ---- 等 CDP 起来(冷启动 0.3–2s)
    let targets = null;
    for (let i = 0; i < 60 && !targets; i++) {
        try {
            const res = await fetch(`http://127.0.0.1:${CDP_PORT}/json/list`);
            const list = await res.json();
            targets = list.find((t) => t.type === "page") ? list : null;
        } catch {
            await sleep(200);
        }
    }
    if (!targets) throw new Error("Chrome 未在 12s 内开出 CDP 端口");
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
        } else if (m.method === "Runtime.consoleAPICalled") {
            const text = (m.params.args || [])
                .map((a) => a.value ?? a.description ?? "")
                .join(" ");
            if (m.params.type === "error") bucket.errors.push(text);
            else if (m.params.type === "warning") bucket.warns.push(text);
        }
    });
    log(`(站点根 ${ROOT} → ${base};CDP ${CDP_PORT})`);

    // =========================================================================
    log("=== ① 满配在线(monitor-online):两图都画,数值与 fixture 对得上 ===");
    {
        const { snap, probe } = await open(
            "monitor-online",
            IN(`return M && M.snapshot().online === true;`),
        );
        eq(snap.reason, "", "可读:无拒读理由");
        eq(snap.online, true, "online");
        eq(snap.stalled, false, "不是停更态");
        eq(snap.observed, 1, "开箱观察组 A");
        eq(snap.groups, 0b00010011, "组位图 = A/B/E");
        eq(snap.hasLanes, true, "帧带车道");
        eq(snap.durationS, 300, "时间线全长 300s");
        eq(snap.seriesTracks, RANGE15, "轨迹图 15 轨,轨号逐项");
        eq(snap.distTracks, RANGE15, "分布图 15 轨");
        eq(snap.legendTracks, RANGE15, "图例 15 行");
        check(
            snap.seriesRuns.every((n) => n >= 1),
            "每轨至少一条折线段(断线不是把整轨画没)",
        );
        check(
            snap.seriesRuns.some((n) => n > 1),
            "至少一轨真的断成了多段(位图 0 位确实生效)",
        );
        // ---- DOM:投影出来的数字必须真的进了 DOM(node 侧断言不到这一步)
        eq(probe.cardOnline, "1", "卡片总闸 data-online=1");
        eq(probe.distBars, 15, "DOM 里真的有 15 根柱");
        eq(probe.legendItems, 15, "DOM 里真的有 15 行图例");
        eq(probe.groupPressed, [1], "只有 A 处于 aria-pressed");
        eq(probe.groupDots, [1, 2, 5], "绿点亮在 A/B/E(键名读错就全灭)");
        eq(probe.abiBanner, false, "无红横幅");
        eq(probe.stalledBanner, false, "无琥珀横幅");
        eq(probe.emptyPanelDisplay, "none", "空态面板被 CSS 收起");
        check(probe.trajCardDisplay !== "none", "轨迹图卡可见");
        check(probe.distCardDisplay !== "none", "分布图卡可见");
        eq(probe.trajEmptyHidden, true, "轨迹图的空态文案收起(真的有线)");
        check(
            /^v\d/.test(probe.version),
            `页脚写上了版本号(实得 ${probe.version}）`,
        );
        // 只读身份:整页零输入控件(J75 B「没有任何写控件」的 DOM 级证据)
        eq(probe.writeControls, 0, "整页零 input/textarea/contenteditable");
        assertClean("monitor-online");
    }

    // =========================================================================
    log("=== ② 停更(monitor-stalled):琥珀横幅 + **图照常显示** ===");
    {
        // 这一条正是真机截图抓到、node 侧看不出来的那个 bug 的回归位:
        // 判成掉线的话 seriesTracks 会变成 []、空态面板会显示出来。
        const { snap, probe } = await open(
            "monitor-stalled",
            IN(`return M && M.snapshot().stalled === true;`),
        );
        eq(snap.reason, "stale", "reason = stale");
        eq(snap.online, true, "仍然 ok(不清图)");
        eq(snap.seriesTracks, RANGE15, "**15 轨折线还在**(停更不清图)");
        eq(snap.distTracks, RANGE15, "分布图也还在");
        eq(probe.stalledBanner, true, "琥珀横幅出现");
        eq(probe.abiBanner, false, "不是红横幅(Output 还在,不是拒连)");
        eq(probe.emptyPanelDisplay, "none", "**不进空态面板**");
        eq(probe.distBars, 15, "DOM 里柱子没被清掉");
        assertClean("monitor-stalled");
    }

    // =========================================================================
    log("=== ②b 先停更、再退出:必须从琥珀横幅切到空态,不能停在横幅上 ===");
    {
        // T45 检测掉线的真实路径:帧陈旧 ⇒ 松开映射再探一次 ⇒ 探不到 ⇒ offline。
        // 关键形态:第二段只推 `scvb.state`,viz 事件流**停发**,页面手里只剩一帧
        // `online:true, fresh:false` 的**留存帧**(`store.frame` 永不清空,它同时是
        // 车道缓存)。留存帧若压过 native 的段级事实,画面就**永久**停在停更横幅上,
        // 而 Output 其实已经没了 —— 这条曾经真的错着。
        const { snap, probe } = await open(
            "monitor-stall-then-gone",
            IN(`return M && M.snapshot().stalled === true;`),
        );
        eq(snap.reason, "stale", "第一段:停更 ⇒ 琥珀横幅");
        eq(probe.stalledBanner, true, "横幅在");
        eq(snap.seriesTracks.length, 15, "图还在(停更不清图)");

        // 2 秒后段没了;只有 scvb.state 会说话
        const gone = await waitFor(
            IN(`return M.snapshot().reason === "offline";`),
            8000,
        );
        check(gone, "第二段:段没了 ⇒ **切到 offline**(不是停在停更横幅上)");
        const p2 = await evaluate(PROBE);
        eq(p2.stalledBanner, false, "琥珀横幅撤掉");
        eq(p2.cardOnline, "0", "卡片总闸 data-online=0");
        eq(p2.emptyTitle, true, "空态标题出现");
        eq(p2.distBars, 0, "柱子清干净(不是留着上一帧的 15 根)");
        assertClean("monitor-stall-then-gone");
    }

    // =========================================================================
    log("=== ③ 掉线(monitor-offline):空态面板 + 组字母 ===");
    {
        const { snap, probe } = await open(
            "monitor-offline",
            IN(`return M && M.snapshot().reason === "offline";`),
        );
        eq(snap.online, false, "不可读");
        eq(snap.emptyState, true, "归空态(不是错误)");
        eq(snap.observed, 3, "观察组 C");
        eq(snap.seriesTracks, [], "没有折线");
        eq(probe.cardOnline, "0", "卡片总闸 data-online=0");
        eq(probe.emptyTitle, true, "空态标题「Output 未运行」出现");
        eq(probe.emptyTextKey, "monitor.offline", "正文用 offline 词条");
        check(
            probe.emptyTextContent.includes("C"),
            `正文点名了组 C(实得「${probe.emptyTextContent}」)`,
        );
        check(probe.trajCardDisplay === "none", "轨迹图卡被 CSS 收起");
        check(probe.distCardDisplay === "none", "分布图卡被 CSS 收起");
        eq(probe.abiBanner, false, "掉线不挂红横幅");
        assertClean("monitor-offline");
    }

    // =========================================================================
    log("=== ④ 拒连(monitor-abi):红横幅,且**不说** Output 未运行 ===");
    {
        const { snap, probe } = await open(
            "monitor-abi",
            IN(`return M && M.snapshot().reason === "abi";`),
        );
        eq(snap.online, false, "停止读取");
        eq(snap.emptyState, false, "**不归空态** —— 话由红横幅说");
        eq(probe.abiBanner, true, "红横幅出现");
        eq(probe.emptyTitle, false, "不挂「Output 未运行」(它明明在跑)");
        eq(probe.emptyText, false, "也不说「尚无分段结果」(不是没有分段)");
        assertClean("monitor-abi");
    }

    // =========================================================================
    log("=== ⑤ 三种「没有线」互不混淆 ===");
    {
        const { snap, probe } = await open(
            "monitor-no-lanes",
            IN(`return M && M.snapshot().online === true;`),
        );
        eq(snap.hasLanes, false, "桥没送车道");
        eq(snap.seriesTracks, [], "轨迹图空");
        eq(snap.distTracks, RANGE15, "**分布图照常 15 轨**(缺车道只砍一半)");
        eq(probe.distBars, 15, "DOM 里柱子照画");
        eq(probe.trajEmptyHidden, false, "轨迹图显示空态文案");
        eq(
            probe.trajEmptyKey,
            "monitor.noLanes",
            "用的是「监视数据未接通」那条",
        );
        check(probe.trajEmptyText.length > 0, "文案真的填进去了(不是空 span)");
        eq(probe.emptyPanelDisplay, "none", "不把整页拖进空态");
        assertClean("monitor-no-lanes");
    }
    {
        const { snap, probe } = await open(
            "monitor-no-tracks",
            IN(`return M && M.snapshot().online === true;`),
        );
        eq(snap.distTracks, [], "分布图画空(缺 trackVolDb ⇒ 不猜、不填 0)");
        eq(probe.distBars, 0, "DOM 里一根幽灵柱都没有");
        eq(snap.seriesTracks, RANGE15, "轨迹图照常 15 轨");
        eq(snap.legendTracks, RANGE15, "图例跟着轨迹图列 15 行");
        assertClean("monitor-no-tracks");
    }
    {
        const { snap, probe } = await open(
            "monitor-no-lead",
            IN(`return M && M.snapshot().online === true;`),
        );
        eq(snap.distTracks, RANGE15, "15 根柱照画");
        eq(probe.distBars, 15, "DOM 里 15 根柱");
        eq(probe.distLeadCaps, 0, "**一顶绿帽都没有**(缺 leadMask 不猜主唱)");
        assertClean("monitor-no-lead");
    }

    // =========================================================================
    log("=== ⑥ 换组(点胶囊):数据面真的换了,且没有旧组残影 ===");
    {
        const { snap } = await open(
            "monitor-groups",
            IN(`return M && M.snapshot().online === true;`),
        );
        eq(snap.observed, 2, "开箱停在 B");
        eq(snap.seriesTracks, [1, 2, 3, 4, 5, 6], "B = 6 轨小编制");

        check(await clickIn('[data-group="5"]'), "点到了 E 胶囊");
        const gotE = await waitFor(IN(`return M.snapshot().observed === 5;`));
        check(gotE, "换组回显到 E");
        const e = await evaluate(SNAP);
        eq(
            e.seriesTracks,
            [1, 2, 7, 8, 9, 10, 11, 12, 13],
            "E = 9 轨且轨号不连续(换组真的换了数据面)",
        );
        const probeE = await evaluate(PROBE);
        eq(probeE.groupPressed, [5], "只有 E 处于 aria-pressed");
        eq(probeE.distBars, 9, "DOM 里柱数跟着变成 9");
        eq(probeE.legendItems, 9, "图例也变成 9 行(没有 B 组残影)");

        check(await clickIn('[data-group="1"]'), "点回 A");
        check(
            await waitFor(
                IN(`return M.snapshot().seriesTracks.length === 15;`),
            ),
            "切回 A ⇒ 又是 15 轨",
        );
        // 换到一个没有 Output 的组:空态面板要说得出是哪个组
        check(await clickIn('[data-group="3"]'), "点到没有 Output 的 C");
        check(
            await waitFor(IN(`return M.snapshot().reason === "offline";`)),
            "C 组 ⇒ 空态",
        );
        const probeC = await evaluate(PROBE);
        check(
            probeC.emptyTextContent.includes("C"),
            `空态点名组 C(实得「${probeC.emptyTextContent}」)`,
        );
        eq(probeC.distBars, 0, "换到空组把上一组的柱子清干净");
        assertClean("monitor-groups(含三次换组)");
    }

    // =========================================================================
    log("=== ⑦ 重连(monitor-reconnect):离线 → 自动恢复,零手动干预 ===");
    {
        const { snap } = await open("monitor-reconnect", null);
        eq(snap.reason, "offline", "开箱:Output 还没起来 ⇒ 空态");
        const back = await waitFor(
            IN(`return M.snapshot().online === true;`),
            12000,
        );
        check(back, "3s 后 Output 起来 ⇒ 页面自己恢复(不用刷新)");
        const s2 = await evaluate(SNAP);
        eq(s2.seriesTracks, RANGE15, "恢复后 15 轨折线补齐");
        const p2 = await evaluate(PROBE);
        eq(p2.emptyPanelDisplay, "none", "空态面板收起");
        eq(p2.distBars, 15, "柱子补齐");
        assertClean("monitor-reconnect");
    }

    // =========================================================================
    log("=== ⑧ 交互:换语言 / 缩放确认条 / pagehide 拆图 ===");
    {
        await open(
            "monitor-online",
            IN(`return M && M.snapshot().online === true;`),
        );

        // ---- 语言:三语各切一遍,词条真的换掉且不报错
        for (const lang of ["en", "fr", "zh"]) {
            check(await clickIn(`[data-lang="${lang}"]`), `点了 ${lang}`);
            await sleep(200);
            const p = await evaluate(PROBE);
            eq(p.langPressed, [lang], `${lang} 胶囊按下`);
        }

        // ---- 缩放:预览 → 确认条出现 → 取消回退(10 秒防呆的两端)
        const changed = await evaluate(
            IN(`
            const sel = gb("monitor-scale");
            if (!sel) return false;
            sel.value = "0.8";
            sel.dispatchEvent(new w.Event("change"));
            return true;
        `),
        );
        check(changed, "改了缩放档位");
        check(
            await waitFor(
                IN(
                    `const c = gb("monitor-scale-confirm"); return c && !c.hidden;`,
                ),
            ),
            "确认条弹出(10 秒防呆)",
        );
        check(
            await clickIn('[data-gb="monitor-scale-confirm-revert"]'),
            "点取消",
        );
        await sleep(200);
        const pr = await evaluate(PROBE);
        eq(pr.scaleConfirm, false, "确认条收起");
        eq(
            await evaluate(IN(`return q("#card").style.zoom;`)),
            "1",
            "档位回退到 1x",
        );

        // ---- pagehide:**真的派发一次**。这里曾经必抛 ReferenceError,
        // 于是同一处理器里的 traj.destroy() 一次都没跑过(node 侧的源码正则看不出来)。
        const before = bucket.exceptions.length;
        await evaluate(
            IN(`w.dispatchEvent(new w.Event("pagehide")); return true;`),
        );
        await sleep(400); // 让后续几帧 mock 事件打到已 destroy 的图上
        eq(
            bucket.exceptions.length,
            before,
            "pagehide 处理器零抛错(拆图真的执行了)",
        );
        // 拆图之后 mock 仍在 4Hz 发帧、25Hz 发播放头 —— 死图上再来事件也不许炸
        await sleep(400);
        eq(
            bucket.exceptions.length,
            before,
            "destroy 之后继续来事件仍然零抛错",
        );
        assertClean("交互(语言/缩放/pagehide)");
    }

    // =========================================================================
    log("=== ⑧b 宿主不推 playhead:回落到 viz 帧自带的种子 ===");
    {
        // 「首帧播放头种子」那一支的条件是「本帧可读 **且** 25Hz 那一路还没来过」——
        // 一次装载最多命中一拍,而命中与否取决于两个事件谁先到。那一支里曾经藏着一个
        // ReferenceError,当初是靠某个场景的时序**恰好撞上**才暴露的。
        // **靠撞上的覆盖等于没有覆盖**,故这里用 `?playhead=off`(宿主拿不到
        // AudioPlayHead 的真实降级)把 25Hz 整路关掉,让那一支**确定**被走到。
        const { snap, probe } = await open(
            "monitor-online",
            IN(`return M && M.snapshot().online === true;`),
            "&playhead=off",
        );
        eq(
            snap.playheadSeen,
            false,
            "`?playhead=off` 真的把 25Hz 那一路关掉了(下一条断言的前提)",
        );
        // 种子那一支挂在 **viz 帧到达**这条路径上,而 `online` 翻绿可能是
        // `scvb.state` 先到那一拍造成的 —— 两者之间隔着最多一个 4Hz 周期。
        // 故这里**等条件**,不是采一次样(采样会把「还没轮到」读成「没跑到」)。
        const seeded = await waitFor(
            IN(`return M.snapshot().seededFromFrame === true;`),
            4000,
        );
        check(
            seeded,
            "没有 25Hz 那一路时,**确定**走到首帧播放头种子(不是碰运气撞上)",
        );
        eq(
            (await evaluate(SNAP)).playheadSeen,
            false,
            "并且全程没有一个 playhead 事件顶替它(竖线只能靠这条种子)",
        );
        eq(snap.seriesTracks.length, 15, "图照常出(播放头缺席不影响两图)");
        eq(probe.emptyPanelDisplay, "none", "不进空态");
        assertClean("monitor-online&playhead=off");
    }

    // =========================================================================
    log("=== ⑨ 裸开(无 mock 后端):零 console.error,页面不白屏 ===");
    {
        // 直开真源页(不经壳页 ⇒ 没有 __SCVB_MOCK__,也没有 __JUCE__)。
        // 纪律逐字来自 app.js 文件头:「裸开浏览器必须零 console.error」——
        // 桥接不上只能 console.warn 并给一句英文提示,不能抛。
        await cdp.send("Page.navigate", { url: "about:blank" });
        await sleep(120);
        newBucket("裸开");
        await cdp.send("Page.navigate", {
            url: `${base}/web/monitor/index.html`,
        });
        check(
            await waitFor(`!!document.querySelector("#card")`),
            "页面装载出卡片(不白屏)",
        );
        await sleep(500);
        const hint = await evaluate(
            `(() => { const el = document.querySelector('[data-gb="monitor-footer-hint"]'); return el ? el.textContent : ""; })()`,
        );
        check(
            /No backend attached/.test(hint),
            `页脚给出了无后端提示(实得「${hint}」)`,
        );
        check(
            bucket.errors.length === 0,
            `裸开零 console.error(实得:${bucket.errors.join(" | ").slice(0, 300)})`,
        );
        check(
            bucket.exceptions.length === 0,
            `裸开零未捕获异常(实得:${bucket.exceptions.join(" | ").slice(0, 300)})`,
        );
        check(
            bucket.warns.length > 0,
            "但**有** warn(桥接不上要说话,不是静默)",
        );
    }
} catch (e) {
    crashed = e;
    console.error("❌ 页面级冒烟自身出错:" + e.message);
} finally {
    try {
        cdp?.close();
    } catch {}
    if (!argv.has("keep-open")) chrome.kill();
    server.close();
    await sleep(200);
    try {
        rmSync(userDataDir, { recursive: true, force: true });
    } catch {}
}

if (crashed || fail > 0) {
    console.error(
        `\n=== 失败 ${fail} 条${crashed ? " + 脚本自身出错" : ""} ===`,
    );
    process.exit(1);
}
log("\n=== 结果:全部通过 ===");
process.exit(0);
