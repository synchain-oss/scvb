// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 三张 UI 卡的**页面级**冒烟(SL-272 / SL-275 / SL-276 / SL-273)
// =============================================================================
// 为什么必须页面级:这一批改的全是**几何与时序**——
//   • SL-272 「已接管…」要单行:折不折行只有排完版才知道,源码正则读不出来;
//   • SL-275 标题区留白:多出来的是 <p> 的 UA 默认 margin,它在 CSS 文件里根本没有
//     对应的一行可以 grep,只能量 getBoundingClientRect;
//   • SL-276 弹窗:「弹出 → 点重新分析 → 真的跑了一次 analyze」是一条跨模块的接线,
//     纯函数断言只能证明「判据会算」,证明不了「算出来的东西真的开了框、真的发了桥调用」。
// 本仓已经栽过三次「三层机检全绿、窗口是白的」,所以这三条一律真跑一次页面。
//
// 跑什么:
//   A. `input.html?scenario=connected`(已接管态)× 三语:
//      A1 pillSub 恰好一行(offsetHeight ≤ 一个 line-height 的 1.4 倍);
//      A2 不被截断(scrollWidth ≤ clientWidth ——「反话护栏」:光靠 A1 的话,
//         把文字裁掉一半也是一行);
//      A3 承载它的 .ipt-header__conn 真的横跨整个 header(≥ header 宽 − 4px)——
//         这一条是 SL-272 的**根因面**:旧布局把它关在品牌块右侧 200px 的栏里。
//         删掉 `grid-column: 1 / -1` 即红;
//      A4 语言胶囊在**右上角**(右边缘齐 header 右缘、顶边齐 header 顶缘,且高于连接行);
//      A5 「?」重看入口在**右下角**(落在 footer 里、且是 footer 最右的一件);
//      A6 header / 连接行 / footer 各自零横向溢出(check-design-box 的运行期对偶);
//   B. `output.html` Tab3 建议表视图:
//      B1 视图内每个 <p> 的上下 margin 都是 0(删掉 `.suggest-view p{margin:0}` 即红);
//      B2 标题区高度 = 标题 + gap + 说明句(±2px),不含任何隐形外边距;
//      B3 标题卡整卡 ≤ 70px(改前 108px,改后实测 61px);
//   C. `output.html` Tab4 「分析口径已更改」弹窗(SL-276):
//      C1 初始不弹;切响度档 ⇒ 弹出,正文/两枚钮的文案取自字典(不是 key 字面量);
//      C2 「稍后」关框,而琥珀 badge **仍在**(它是常驻状态位,不随关框消失);
//      C3 换到另一个脏值 ⇒ 再弹一次(按值记,不是一次性开关);
//      C4 「重新分析」= 关框 + 真的跑完一次 analyze —— 判据是**琥珀 badge 自己灭掉**
//         (基线由 scvb.segments 的 analyze 帧同步),而不是「按钮被点到了」。
//         把 tab-settings.js 里的 `call("analyze", "all")` 删掉,本条即红;
//      C5 Esc 关框;
//      C6 三语各弹一次,正文非空且不等于 key;
//      C7 [SL-273] 换档影响面这句话在**两处**都写着,且逐字同一句:设置页响度卡第二行
//         与弹窗第二段共用词条 set.reanalyze.scopeNote。断言取两处的 textContent 做
//         全等比较 —— 拿掉任一处的 data-t(或把它换成另一条词条)即红,三语各验一次;
//         同批一条排版断言:弹窗正文段的上下 margin 为 0(两段间距只由 .sc-modal__note
//         的 margin-top 决定),删掉 `.sc-modal--reanalyze .sc-modal__body{margin:0}` 即红。
//
// 章节在下面的执行顺序是 A → E → B → C(E 紧跟 A,因为两段用的是同一张 Input 页)。
//
// **本段量的不是 spotlight 本身**:亮区是画在 canvas 上的,取不到 DOM 矩形。
// 量的是它的输入(锚点的 bounding rect)与输出(说明框的落位),这两样才是挪位会改的东西:
//   E. `input.html?scenario=input-first-run` 的 mini tour 末步([J80] 第 ⑤ 步):
//      「?」按 SL-272② 从 header 右上挪到了卡片右下 —— 它同时是 tour 的 spotlight 锚点,
//      挪动锚点会改说明框的落位方向(placeCallout 的 fitsBelow 从真变假)。所以这条不是
//      「顺手多测一点」,而是本卡改动的直接受害面:
//      E1 走到 5/5,且「?」确实落在卡片下半部(挪位本身);
//      E2 说明框整体仍落在卡内(改前它在锚点下方,锚点到了底边就只能翻到上方);
//      E3 说明框与「?」不相交(翻错方向的典型症状是盖住自己要讲的东西)。
//   D. 上面每一段跑完都要零未捕获异常、零 console.error。
//
// 用法:node web-preview/tests/smoke-ui-layout-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;**2 = 环境里没有 Chrome/Edge**(口径同
//   smoke-output-stale-page.mjs 与 CLAUDE.md §6:可选依赖缺席不判红,但也绝不算通过)。
//
// CDP 那 30 行与 smoke-output-stale-page.mjs 同源(node 内置 fetch + WebSocket,
// 零依赖 —— 仓库红线是不引 puppeteer)。同样不抽公共模块:那份断的是 Output 的提示面,
// 本份断的是三处几何,合并只会让两边被对方的参数面绑住。
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

const LANGS = ["zh", "en", "fr"];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let fail = 0;
const log = (s) => console.log(s);
function check(cond, msg) {
    if (cond) return true;
    fail++;
    console.log(`  [FAIL] ${msg}`);
    return false;
}
function le(got, want, msg) {
    return check(
        typeof got === "number" && got <= want,
        `${msg}(实得 ${got},上限 ${want})`,
    );
}
function ge(got, want, msg) {
    return check(
        typeof got === "number" && got >= want,
        `${msg}(实得 ${got},下限 ${want})`,
    );
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
    // 无头浏览器会自己发一条 /favicon.ico；仓里没这个文件，404 会以
    // console.error 的形式进错误桶，把「零 console.error」那条断言泳死。
    // 回 204 而不是把它从错误桶里过滤掉：过滤器会顺手放过真的资源 404。
    if (p === "/favicon.ico") {
        res.writeHead(204).end();
        return;
    }
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
    argv.get("cdp") || 9800 + Math.floor(Math.random() * 400),
);
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-ui-layout-"));
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
    const gb = (n) => q('[data-gb="' + n + '"]');
    const vis = (el) => !!el && !el.hidden;
    const R = (el) => { const r = el.getBoundingClientRect(); return {
        x: Math.round(r.left), y: Math.round(r.top),
        r: Math.round(r.right), b: Math.round(r.bottom),
        w: Math.round(r.width), h: Math.round(r.height) }; };
    ${js}
})()`;

async function click(gbOrSel) {
    const sel = gbOrSel.startsWith("[") ? gbOrSel : `[data-gb="${gbOrSel}"]`;
    return await evaluate(
        IN(
            `const n = q(${JSON.stringify(sel)}); if (!n) return false; n.click(); return true;`,
        ),
    );
}

function assertClean(label) {
    check(
        bucket.exceptions.length === 0,
        `${label}:零未捕获异常(实得 ${bucket.exceptions.length} 条:${bucket.exceptions.slice(0, 2).join(" | ")})`,
    );
    check(
        bucket.errors.length === 0,
        `${label}:零 console.error(实得 ${bucket.errors.length} 条:${bucket.errors.slice(0, 2).join(" | ")})`,
    );
}

// =============================================================================
// 探针
// =============================================================================

// ---- A. Input header 几何(SL-272)
const INPUT_READY = IN(`
    const n = gb("input.header.pillSub");
    return !!(n && n.textContent && n.textContent.length > 0);
`);

const INPUT_PROBE = IN(`
    const sub = gb("input.header.pillSub");
    const conn = gb("input.header.conn");
    const lang = gb("input.header.lang");
    const help = gb("input.header.help");
    const badge = gb("input.header.groupBadge");
    const header = gb("input.header");
    const footer = gb("input.footer");
    const connrow = q(".ipt-header__connrow");
    if (!sub || !conn || !lang || !help || !badge || !header || !footer || !connrow) return null;
    const lh = parseFloat(w.getComputedStyle(sub).lineHeight) || 0;
    const fr = R(footer);
    return {
        text: sub.textContent.trim(),
        subH: sub.offsetHeight,
        lineH: lh,
        subScrollW: sub.scrollWidth,
        subClientW: sub.clientWidth,
        connW: conn.clientWidth,
        headerW: header.clientWidth,
        lang: R(lang),
        help: R(help),
        badge: R(badge),
        header: R(header),
        conn: R(conn),
        footer: fr,
        helpInFooter: footer.contains(help),
        headerScrollW: header.scrollWidth,
        headerClientW: header.clientWidth,
        connrowScrollW: connrow.scrollWidth,
        connrowClientW: connrow.clientWidth,
        footerScrollW: footer.scrollWidth,
        footerClientW: footer.clientWidth,
        docScrollW: d.documentElement.scrollWidth,
        docClientW: d.documentElement.clientWidth,
    };
`);

// ---- E. mini tour 末步的「?」自指(SL-272② 挪了 spotlight 锚点)
const TOUR_PROBE = IN(`
    const help = q('[data-tour="help"]');
    const callout = q(".tour-callout");
    const shell = d.getElementById("ipt-shell");
    const overlay = q("[data-tour-overlay]");
    if (!help || !callout || !shell || !overlay || overlay.hidden) return null;
    // spotlight 是画在 canvas 上的,取不到 DOM 矩形;改用 tour-in 自己算亮区那条路
    // (spotRectOf 的输入 = 锚点的 bounding rect),故这里直接量锚点与说明框。
    return {
        step: (q(".tour-callout__step") || {}).textContent || "",
        help: R(help),
        callout: R(callout),
        shell: R(shell),
    };
`);

// ---- B. 建议表留白(SL-275)
const SUGGEST_PROBE = IN(`
    const view = q(".suggest-view");
    const head = q(".suggest-head");
    const titles = q(".suggest-head__titles");
    const title = q(".suggest-title");
    const disc = q(".suggest-disclaimer");
    if (!view || !head || !titles || !title || !disc) return null;
    const margins = [];
    for (const p of view.querySelectorAll("p")) {
        const cs = w.getComputedStyle(p);
        margins.push({
            gb: p.getAttribute("data-gb") || p.className,
            mt: cs.marginTop,
            mb: cs.marginBottom,
        });
    }
    const gap = parseFloat(w.getComputedStyle(titles).rowGap) || 0;
    return {
        open: !view.hidden,
        headH: head.offsetHeight,
        titlesH: titles.offsetHeight,
        titleH: title.offsetHeight,
        discH: disc.offsetHeight,
        gap: gap,
        margins: margins,
    };
`);

// ---- C. 重分析弹窗(SL-276)
const ASK_PROBE = IN(`
    const ask = gb("reanalyze-ask");
    const body = gb("reanalyze-ask-body");
    const later = gb("reanalyze-ask-later");
    const primary = gb("reanalyze-ask-primary");
    const badge = gb("settings-loudnessmode-stale");
    // [SL-273] 同一条词条的两个渲染点:弹窗第二段 / 设置页响度卡第二行。
    const askNote = gb("reanalyze-ask-scopenote");
    const setNote = gb("settings-loudnessmode-scopenote");
    const panel = gb("reanalyze-ask-panel");
    if (!ask || !body || !later || !primary) return null;
    const bs = w.getComputedStyle(body);
    return {
        open: vis(ask),
        body: body.textContent.trim(),
        later: later.textContent.trim(),
        primary: primary.textContent.trim(),
        badgeShown: vis(badge),
        askNote: askNote ? askNote.textContent.trim() : null,
        setNote: setNote ? setNote.textContent.trim() : null,
        noteInPanel: !!(panel && askNote && panel.contains(askNote)),
        bodyMt: bs.marginTop,
        bodyMb: bs.marginBottom,
    };
`);

// [SL-273] 定谳原文要求「设置页响度口径项说明**与**弹窗正文」都写明换档影响面。
// 实现上两处共用同一条词条(set.reanalyze.scopeNote),所以这里断言的是**全等**而不是
// 「各自非空」—— 把任一处的 data-t 拿掉、或改指另一条词条,这条立刻红。
function assertScopeNote(probe, lang) {
    if (
        !check(
            typeof probe.askNote === "string" && probe.askNote.length > 0,
            `${lang}:弹窗写明了换档影响面(实得 ${JSON.stringify(probe.askNote)})`,
        )
    )
        return;
    if (
        !check(
            typeof probe.setNote === "string" && probe.setNote.length > 0,
            `${lang}:设置页响度卡写明了换档影响面(实得 ${JSON.stringify(probe.setNote)})`,
        )
    )
        return;
    check(
        !probe.askNote.startsWith("set."),
        `${lang}:影响面这段取自字典而不是 key 字面量(实得 ${JSON.stringify(probe.askNote)})`,
    );
    check(
        probe.askNote === probe.setNote,
        `${lang}:两处逐字同一句(弹窗 ${JSON.stringify(probe.askNote)} / 设置页 ${JSON.stringify(probe.setNote)})`,
    );
}

const askOpen = IN(`const n = gb("reanalyze-ask"); return !!n && !n.hidden;`);
const askClosed = IN(`const n = gb("reanalyze-ask"); return !!n && n.hidden;`);
const badgeGone = IN(
    `const n = gb("settings-loudnessmode-stale"); return !!n && n.hidden;`,
);

async function setLoudness(value) {
    return await evaluate(
        IN(`const btn = q('[data-gb="settings-loudnessmode-seg"] [data-value="${value}"]');
            if (!btn) return false; btn.click(); return true;`),
    );
}

async function pressEscape() {
    await evaluate(
        IN(`d.dispatchEvent(new w.KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
            return true;`),
    );
}

// 语言胶囊切语言(Input 与 Output 的锚点名不同族,各给一个)。
async function switchLangInput(code) {
    return await click(`input.header.lang.${code}`);
}
async function switchLangOutput(code) {
    return await click(`header-lang-${code}`);
}

// 首启遮挡(引导页 / tour 询问框)——都点掉,别让它们盖住待测面。
async function dismissOverlays() {
    await evaluate(
        IN(`for (const n of ["guide-overlay-start", "tour-ask-later"]) {
                const b = gb(n);
                if (b && b.offsetParent !== null) b.click();
            }
            return true;`),
    );
    await sleep(250);
    await evaluate(
        IN(`const b = gb("tour-ask-later");
            if (b && b.offsetParent !== null) b.click();
            return true;`),
    );
    await sleep(250);
}

// =============================================================================
// 主流程
// =============================================================================
try {
    // ---- 连上无头浏览器
    let targets = null;
    for (let i = 0; i < 60 && !targets; i++) {
        try {
            const r = await fetch(`http://127.0.0.1:${CDP_PORT}/json/list`);
            const list = await r.json();
            targets = list.find((t) => t.type === "page") ? list : null;
        } catch {
            await sleep(200);
        }
    }
    if (!targets) noBrowser("CDP 端口没起来");
    const page = targets.find((t) => t.type === "page");
    cdp = cdpConnect(page.webSocketDebuggerUrl);
    await cdp.ready;
    await cdp.send("Runtime.enable");
    await cdp.send("Log.enable");
    await cdp.send("Page.enable");
    cdp.on((msg) => {
        if (msg.method === "Runtime.exceptionThrown") {
            const d = msg.params.exceptionDetails;
            bucket.exceptions.push(
                d.exception?.description || d.text || "(无描述)",
            );
        } else if (msg.method === "Runtime.consoleAPICalled") {
            if (msg.params.type !== "error") return;
            bucket.errors.push(
                (msg.params.args || [])
                    .map((a) => a.value ?? a.description ?? "")
                    .join(" "),
            );
        } else if (msg.method === "Log.entryAdded") {
            if (msg.params.entry.level === "error")
                bucket.errors.push(msg.params.entry.text);
        }
    });

    // =========================================================== A. SL-272
    log("A. SL-272 Input header 布局(三语)");
    for (const lang of LANGS) {
        newBucket(`input/${lang}`);
        await cdp.send("Page.navigate", {
            url: `${base}/web-preview/input.html?scenario=connected`,
        });
        check(await waitFor(INPUT_READY), `${lang}:Input 页装载并吃到首帧`);
        check(await switchLangInput(lang), `${lang}:语言胶囊可点`);
        await sleep(400);
        const p = await evaluate(INPUT_PROBE);
        if (!check(p, `${lang}:探针取到全部锚点`)) continue;

        // A1 单行 —— line-height 的 1.4 倍是「一行 + 一点点行盒余量」的上限,
        // 两行必然超过它(两行 = 2.0 倍)。
        le(p.subH, Math.ceil(p.lineH * 1.4), `${lang}:pillSub 恰好一行`);
        // A2 不截断(反话护栏:裁掉一半字也是一行)
        le(
            p.subScrollW,
            p.subClientW,
            `${lang}:pillSub 未被截断(scrollWidth ≤ clientWidth)`,
        );
        check(p.text.length > 0, `${lang}:pillSub 有文案`);
        // A3 根因面:连接块横跨整个 header
        ge(p.connW, p.headerW - 4, `${lang}:连接块横跨整个 header`);
        // A4 语言胶囊在右上角
        le(
            Math.abs(p.lang.r - p.header.r),
            2,
            `${lang}:语言胶囊右缘齐 header 右缘`,
        );
        le(
            Math.abs(p.lang.y - p.header.y),
            2,
            `${lang}:语言胶囊顶边齐 header 顶缘`,
        );
        check(
            p.lang.b <= p.conn.y,
            `${lang}:语言胶囊在连接行之上(实得 lang.bottom=${p.lang.b} / conn.top=${p.conn.y})`,
        );
        // A4b 组 badge 落在第二行行首(左贴齐,不再挤在右上角)
        check(
            p.badge.x < p.lang.x && p.badge.y > p.lang.y,
            `${lang}:组 badge 在第二行行首(实得 badge=${p.badge.x},${p.badge.y} / lang=${p.lang.x},${p.lang.y})`,
        );
        // A5 「?」在右下角
        check(p.helpInFooter, `${lang}:「?」落在 footer 里`);
        le(
            Math.abs(p.help.r - p.footer.r),
            2,
            `${lang}:「?」是 footer 最右的一件`,
        );
        check(
            p.help.y > p.conn.b,
            `${lang}:「?」在 header 连接行之下(右下角,不再是右上角)`,
        );
        // A6 header 三个横向容器零溢出(check-design-box 的运行期对偶)。
        //    量的是 header / 连接行 / footer 自身,**不是** #ipt-shell —— shell 的
        //    scrollWidth 恒为 578(三枚装饰性 .sc-halo 是 absolute 且刻意画在盒外,
        //    改前改后都一样),拿它当判据只会得到一条永远红的断言。
        le(p.headerScrollW, p.headerClientW, `${lang}:header 零横向溢出`);
        le(p.connrowScrollW, p.connrowClientW, `${lang}:连接行零横向溢出`);
        le(p.footerScrollW, p.footerClientW, `${lang}:footer 零横向溢出`);
        le(p.docScrollW, p.docClientW, `${lang}:文档零横向滚动`);
        assertClean(`input/${lang}`);
    }

    // =========================================================== E. SL-272②
    // 「?」既是重看入口、也是 mini tour 末步的 spotlight 锚点。它从 header 右上挪到
    // 卡片右下之后,placeCallout 的落位分支从 fitsBelow 翻到 fitsAbove —— 这一段就是
    // 在真页面上确认「翻对了方向、没盖住自己要讲的按钮、也没掉出卡外」。
    log("E. SL-272② 「?」挪位后 mini tour 末步仍指得准");
    newBucket("input-tour");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/input.html?scenario=input-first-run`,
    });
    check(
        await waitFor(
            IN(`const n = gb("lang-start"); return !!n && !n.hidden;`),
        ),
        "首启语言卡弹出",
    );
    check(
        await click('[data-lang-pick="zh"]'),
        "选中文 ⇒ 语言卡关闭、mini tour 起",
    );
    check(
        await waitFor(
            IN(`const n = q("[data-tour-overlay]"); return !!n && !n.hidden;`),
        ),
        "mini tour 起来了",
    );
    // 5 步基线:点 4 次「下一步」到末步(末步的按钮变成「完成」,不再点)。
    for (let i = 0; i < 4; i++) {
        check(
            await click('[data-tour-btn="next"]'),
            `mini tour 第 ${i + 1} 次「下一步」可点`,
        );
        await sleep(200);
    }
    const tp = await evaluate(TOUR_PROBE);
    if (check(tp, "tour 探针取到锚点")) {
        check(
            tp.step.trim() === "5/5",
            `走到末步(实得 ${JSON.stringify(tp.step)})`,
        );
        // E2 说明框整体落在卡内
        check(
            tp.callout.x >= tp.shell.x &&
                tp.callout.r <= tp.shell.r &&
                tp.callout.y >= tp.shell.y &&
                tp.callout.b <= tp.shell.b,
            `E2 说明框仍在卡内(callout=${tp.callout.x},${tp.callout.y},${tp.callout.r},${tp.callout.b} / shell=${tp.shell.x},${tp.shell.y},${tp.shell.r},${tp.shell.b})`,
        );
        // E3 说明框不压住它要讲的那枚「?」
        const overlaps =
            tp.callout.x < tp.help.r &&
            tp.callout.r > tp.help.x &&
            tp.callout.y < tp.help.b &&
            tp.callout.b > tp.help.y;
        check(
            !overlaps,
            `E3 说明框没盖住「?」(help=${tp.help.x},${tp.help.y},${tp.help.r},${tp.help.b})`,
        );
        // E1 「?」确实在卡片右下(挪位本身),且末步讲的就是它
        check(
            tp.help.b > tp.shell.y + (tp.shell.b - tp.shell.y) / 2,
            `E1 「?」落在卡片下半部(help.bottom=${tp.help.b} / shell 中线=${Math.round(tp.shell.y + (tp.shell.b - tp.shell.y) / 2)})`,
        );
    }
    assertClean("input-tour");

    // =========================================================== B. SL-275
    log("B. SL-275 建议表标题区留白");
    newBucket("suggest");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?fixture=fifteen-tracks`,
    });
    check(
        await waitFor(IN(`const n = gb("tabnav-wave"); return !!n;`)),
        "Output 页装载",
    );
    await dismissOverlays();
    await click("tabnav-wave");
    await sleep(400);
    check(await click("wave-btn-suggest"), "打开建议表视图");
    await sleep(600);
    const b = await evaluate(SUGGEST_PROBE);
    if (check(b && b.open, "建议表视图已打开")) {
        // B1 每个 <p> 的上下 margin 归零(删掉 `.suggest-view p{margin:0}` 即红)
        const bad = b.margins.filter((m) => m.mt !== "0px" || m.mb !== "0px");
        check(
            bad.length === 0,
            `建议表内 <p> 上下 margin 全为 0(实得 ${bad.length} 个不为 0:${bad
                .slice(0, 3)
                .map((m) => `${m.gb} ${m.mt}/${m.mb}`)
                .join(" | ")})`,
        );
        check(
            b.margins.length >= 4,
            `探针真的量到了 <p>(实得 ${b.margins.length} 个)`,
        );
        // B2 标题区高度 = 标题 + gap + 说明句(±2px):没有隐形外边距
        le(
            Math.abs(b.titlesH - (b.titleH + b.gap + b.discH)),
            2,
            "标题区高度 = 标题 + gap + 说明句",
        );
        // B3 整卡高度回到与仓内其他卡片同档(改前实测 108px)
        le(b.headH, 70, "标题卡高度收紧(改前 108px,改后 61px)");
    }
    assertClean("suggest");

    // =========================================================== C. SL-276
    log("C. SL-276 「分析口径已更改」弹窗");
    newBucket("reanalyze-ask");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?fixture=fifteen-tracks`,
    });
    check(
        await waitFor(
            IN(`const n = gb("settings-loudnessmode-seg"); return !!n;`),
        ),
        "Output 页装载(Tab4 锚点在)",
    );
    await dismissOverlays();
    await click("tabnav-settings");
    await sleep(400);

    // C1 初始不弹
    check(await evaluate(askClosed), "初始不弹");
    check(await setLoudness("rms"), "切响度档 rms 可点");
    check(await waitFor(askOpen, 4000), "C1 切响度档 ⇒ 弹窗弹出");
    let c = await evaluate(ASK_PROBE);
    if (check(c, "弹窗探针取到锚点")) {
        check(
            c.body.length > 0 && !c.body.startsWith("set."),
            `C1 正文取自字典(实得 ${JSON.stringify(c.body)})`,
        );
        check(
            c.primary.length > 0 && !c.primary.startsWith("set."),
            `C1 「重新分析」钮取自字典(实得 ${JSON.stringify(c.primary)})`,
        );
        check(
            c.later.length > 0 && !c.later.startsWith("set."),
            `C1 「稍后」钮取自字典(实得 ${JSON.stringify(c.later)})`,
        );
        // C7 [SL-273] 换档影响面这句在**两处**都写着,且逐字同一句。
        assertScopeNote(c, "zh");
        // C7b 弹窗正文段不吃 <p> 的 UA 默认 margin(两段间距只由 .sc-modal__note 定)。
        check(
            c.bodyMt === "0px" && c.bodyMb === "0px",
            `C7 弹窗正文段上下 margin 归零(实得 ${c.bodyMt}/${c.bodyMb})`,
        );
        check(c.noteInPanel, "C7 影响面这段落在弹窗面板里(不是被遮罩截在框外)");
    }

    // C2 「稍后」关框,但琥珀 badge 仍在
    check(await click("reanalyze-ask-later"), "「稍后」可点");
    check(await waitFor(askClosed, 3000), "C2 「稍后」关框");
    c = await evaluate(ASK_PROBE);
    check(c && c.badgeShown, "C2 关框后琥珀 badge 仍在(常驻状态位)");

    // C3 换到另一个脏值 ⇒ 再弹
    check(await setLoudness("peak_dbfs"), "切到 peak_dbfs 可点");
    check(await waitFor(askOpen, 4000), "C3 换脏值 ⇒ 再弹一次");

    // C4 「重新分析」= 关框 + 真的跑完一次 analyze(判据 = badge 自己灭)
    check(await click("reanalyze-ask-primary"), "「重新分析」可点");
    check(await waitFor(askClosed, 3000), "C4 点「重新分析」后关框");
    check(
        await waitFor(badgeGone, 8000),
        "C4 analyze 真的跑完(琥珀 badge 由 segments 帧自己灭)",
    );

    // C5 Esc 关框
    check(await setLoudness("rms"), "再切一次 rms 可点");
    check(await waitFor(askOpen, 4000), "C5 基线已同步,再改走仍会弹");
    await pressEscape();
    check(await waitFor(askClosed, 3000), "C5 Esc 关框");
    assertClean("reanalyze-ask");

    // C6 三语各弹一次:正文非空且不是 key 字面量
    for (const lang of LANGS) {
        newBucket(`ask/${lang}`);
        await cdp.send("Page.navigate", {
            url: `${base}/web-preview/output.html?fixture=fifteen-tracks`,
        });
        check(
            await waitFor(
                IN(`const n = gb("settings-loudnessmode-seg"); return !!n;`),
            ),
            `${lang}:Output 页装载`,
        );
        await dismissOverlays();
        check(await switchLangOutput(lang), `${lang}:语言胶囊可点`);
        await sleep(300);
        await click("tabnav-settings");
        await sleep(300);
        check(await setLoudness("rms"), `${lang}:切响度档可点`);
        check(await waitFor(askOpen, 4000), `${lang}:弹窗弹出`);
        const t = await evaluate(ASK_PROBE);
        if (check(t, `${lang}:探针取到锚点`)) {
            check(
                t.body.length > 0 && !t.body.startsWith("set."),
                `${lang}:正文已翻(实得 ${JSON.stringify(t.body)})`,
            );
            check(
                t.primary.length > 0 && !t.primary.startsWith("set."),
                `${lang}:主行动钮已翻(实得 ${JSON.stringify(t.primary)})`,
            );
            check(
                t.later.length > 0 && !t.later.startsWith("set."),
                `${lang}:「稍后」已翻(实得 ${JSON.stringify(t.later)})`,
            );
            assertScopeNote(t, lang);
        }
        assertClean(`ask/${lang}`);
    }
} catch (e) {
    fail++;
    console.log(`  [FAIL] 冒烟自身抛错:${e && e.stack ? e.stack : e}`);
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

if (fail === 0) {
    console.log(
        "✅ smoke-ui-layout-page:SL-272 / SL-275 / SL-276 / SL-273 全绿",
    );
    process.exit(0);
}
console.log(`❌ smoke-ui-layout-page:${fail} 条断言失败`);
process.exit(1);
