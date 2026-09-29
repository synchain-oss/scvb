// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 确认条次按钮对比度 + 长轨名不撑宽的页面级冒烟(SL-560 / SL-562)
// =============================================================================
// 两张卡都是**样式**缺陷,源码里没有一行可以 grep 出「看不清」或「被撑宽」:
//   • SL-560 用户 rc.1 实测:Output 轨道页的行内确认条(拖一条没冻结的轨的音量卡箍时
//     出现,两枚钮「取消」「继续」)里「取消」颜色跟背景一样。根因是这条确认条早已改成
//     深色玻璃底,而「取消」还是浅色面那枚裸 `.sc-btn`(白 .3 底 + --txt-2 深灰字)。
//     对比度只能按**合成后的真颜色**算:钮底是半透明的,叠在什么底上才是它的样子。
//   • SL-562 用户 rc.1 实测(截图 inputbug.png):Input 窗口通道格里轨名一长,整张卡片
//     被横向撑宽、左边被卷走。根因是网格列写的裸 `1fr`(下限 = 内容的 min-content,
//     nowrap 的轨名行就是整串宽)。「撑没撑宽」只有排完版才知道。
//
// 跑什么:
//   A. Input 通道网格(SL-562),在 1 / 0.75 / 1.5 三个缩放档各跑一遍:
//      A0 基线:演示轨名下 16 张卡等宽,记下格宽;
//      A1 15 条轨名全换成 53 字的中英混排长名(契约上限是 24 字,这里故意超出留余量)⇒
//         轨名真的上屏了(防空转)、且真的被截断了(轨名行 scrollWidth > clientWidth,
//         否则说明这串不够长、本格没测到东西);网格 / 通道区 / 组卡片 / 内容区四层
//         scrollWidth ≤ clientWidth;每张卡宽与基线相同(±0.5px)、右缘不越出网格;
//      A2 悬停提示(被截掉的半句只剩这条路):一张卡只有一个 title,挂在卡上 ——
//         未被占用的卡 = 完整轨名;被别的实例占用的卡 = 完整轨名 + 换行 + 占用说明;
//         空轨名那张卡不带轨名行;轨名那一行自己不挂 title(内层 title 会把卡上的占用
//         说明遮住,#336 复审点出)。两种卡(占用 / 未占用)都必须真的出现,防空转。
//   B. Output 整体调整页 Lead Select(SL-562 顺查出的同族):
//      B0 基线:短轨名下选中轨 1,记下触发钮高度、选项高度;
//      B1 轨名换成 24 个 W 连写(无断点,最宽的不可断串)与 24 个汉字两种,各选中轨 1 ⇒
//         触发钮与所在卡片 scrollWidth ≤ clientWidth、触发钮高度与基线相同(单行)、
//         轨名右缘不压到下拉箭头上、触发钮轨名 title = 「01 轨名」;
//         展开下拉 ⇒ 面板无横向溢出、选项高度与基线相同、选项 title = 「01 轨名」。
//   C. 确认条 / 确认框里每一枚钮的文字对比度(SL-560),Output / Input / Monitor 全部
//      「需要一个决定」的面(清单写在 SURFACES 里,每面至少要量到一枚钮,防空转):
//      C1 每枚可见、有字的钮:文字色 vs 钮底 ≥ 4.5:1(WCAG AA 正文档;钮上是 10–11px 小字)。
//         取真值纪律同 smoke-ui-layout-page 的 A8:全部 getComputedStyle;半透明钮底按它
//         **实际叠在什么底上**合成;底沿祖先链逐层合成到第一层不透明为止(渐变在钮上的
//         左 / 中 / 右三点取值,取最差的一点);找不到不透明底即判红(结论不能建立在兜底色上)。
//      C2 行内确认条的主次层级:「继续」钮底 vs 条底 ≥ 3:1,且高于「取消」钮底 vs 条底。
//      面默认是隐藏的,同一次页内求值里临时显出 → 量 → 还原,不经过任何产品代码路径:
//      本组判的是**长相**,接线(什么时候弹)由各自的冒烟管。
//
// 删除式(未提交,人工核过,见 PR 描述):
//   · tab-tracks.js 里「取消」去掉 sc-btn--dark ⇒ C1 的 tracks-row-1-manual-overwrite-cancel 红;
//   · input/index.html 网格列改回 repeat(4, 1fr) ⇒ A1 三个缩放档全红;
//   · input/app.js 卡片 title 里去掉轨名那一行 ⇒ A2 两种卡红;只去掉占用说明那一行 ⇒
//     A2「被占用的卡」红;
//   · 另在轨名那一行上挂回 title ⇒ A2「轨名那一行不另挂」红;
//   · output/index.html 单行截断规则里去掉触发钮那个选择器 ⇒ B1 触发钮那几格红;
//     只去掉 .lead-select__name ⇒ B1 面板那几格红;去掉 margin-right ⇒ 「不压箭头」那格红;
//   · tab-master.js 删掉选项 / 触发钮的 title ⇒ 各自那格红。
//
// 用法:node web-preview/tests/smoke-contrast-and-name-fit-page.mjs [仓库根绝对路径]
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-contrast-namefit-"));
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

// 壳页上下文:mock driver 的控制面(`window.__SCVB_PREVIEW__.ctl`)挂在壳页 window 上,
// 不在 iframe 里 —— 事件的下行口只在这里(见 smoke-group-lock-page 同名处的说明)。
const SHELL = (js) => `(() => {
    const s = window.__SCVB_PREVIEW__ || window.__SCVB_PREVIEW_SESSION__;
    if (!s || !s.ctl || !s.ctl.model) return null;
    ${js}
})()`;

async function nav(path, readyExpr, label) {
    await cdp.send("Page.navigate", { url: "about:blank" });
    await sleep(120);
    newBucket(label);
    await cdp.send("Page.navigate", { url: `${base}${path}` });
    const ok = await waitFor(readyExpr, 20000);
    check(ok, `${label}:页面装载完成`);
    return ok;
}

// ---- C 组的取真值库(页内执行)--------------------------------------------------
// 颜色一律取 getComputedStyle 的序列化值(rgb()/rgba());渐变按 CSS 规范的 linear-gradient
// 几何(方向 (sinθ,−cosθ)、线过盒心、线长 = |W·sinθ|+|H·cosθ|)在取样点求值,色标缺省位置
// 按规范均分。半透明层自上而下收集、到第一层不透明为止,再自下而上合成。
// 相对亮度按 WCAG 的 sRGB 线性化公式现算。
const CONTRAST_LIB = `
    const parseC = (s) => {
        const m = /rgba?\\(([^)]+)\\)/.exec(s || "");
        if (!m) return null;
        const p = m[1].split(/[\\s,\\/]+/).filter(Boolean).map((x) => parseFloat(x));
        if (p.length < 3 || p.slice(0, 3).some((v) => !Number.isFinite(v))) return null;
        return { r: p[0], g: p[1], b: p[2], a: p.length > 3 && Number.isFinite(p[3]) ? p[3] : 1 };
    };
    const splitTop = (s) => {
        const out = [];
        let depth = 0, cur = "";
        for (const ch of s) {
            if (ch === "(") depth++;
            if (ch === ")") depth--;
            if (ch === "," && depth === 0) { out.push(cur.trim()); cur = ""; } else cur += ch;
        }
        if (cur.trim()) out.push(cur.trim());
        return out;
    };
    const over = (top, bottom) => ({
        r: top.r * top.a + bottom.r * (1 - top.a),
        g: top.g * top.a + bottom.g * (1 - top.a),
        b: top.b * top.a + bottom.b * (1 - top.a),
        a: 1,
    });
    const lin = (v) => { v /= 255; return v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); };
    const lum = (c) => 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
    const ratio = (a, b) => { const x = lum(a), y = lum(b); return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05); };
    const KW = { "to top": 0, "to right": 90, "to bottom": 180, "to left": 270 };
    const gradAt = (el, img, x, y) => {
        const m = /^linear-gradient\\(([\\s\\S]*)\\)$/.exec(img.trim());
        if (!m) return null;
        const parts = splitTop(m[1]);
        let deg = 180;
        const dm = /^(-?[\\d.]+)deg$/.exec(parts[0]);
        if (dm) { deg = parseFloat(dm[1]); parts.shift(); }
        else if (Object.prototype.hasOwnProperty.call(KW, parts[0])) { deg = KW[parts[0]]; parts.shift(); }
        else if (/^to /.test(parts[0])) return null;
        const stops = parts.map((p) => {
            const c = parseC(p);
            const pm = /\\)\\s+(-?[\\d.]+)%\\s*$/.exec(p);
            return c ? { c: c, p: pm ? parseFloat(pm[1]) / 100 : null } : null;
        });
        if (stops.length < 2 || stops.some((s) => !s)) return null;
        if (stops[0].p === null) stops[0].p = 0;
        if (stops[stops.length - 1].p === null) stops[stops.length - 1].p = 1;
        for (let i = 1; i < stops.length - 1; i++) {
            if (stops[i].p !== null) continue;
            let j = i;
            while (stops[j].p === null) j++;
            const a = stops[i - 1].p, b = stops[j].p;
            for (let k = i; k < j; k++) stops[k].p = a + ((b - a) * (k - i + 1)) / (j - i + 1);
        }
        const box = el.getBoundingClientRect();
        const rad = (deg * Math.PI) / 180;
        const dx = Math.sin(rad), dy = -Math.cos(rad);
        const L = Math.abs(box.width * dx) + Math.abs(box.height * dy);
        if (!(L > 0)) return null;
        const t = 0.5 + ((x - (box.left + box.width / 2)) * dx + (y - (box.top + box.height / 2)) * dy) / L;
        const mix = (a, b, k) => ({ r: a.r + k * (b.r - a.r), g: a.g + k * (b.g - a.g), b: a.b + k * (b.b - a.b), a: a.a + k * (b.a - a.a) });
        if (t <= stops[0].p) return stops[0].c;
        for (let i = 0; i + 1 < stops.length; i++) {
            const a = stops[i], b = stops[i + 1];
            if (t <= b.p) return b.p > a.p ? mix(a.c, b.c, (t - a.p) / (b.p - a.p)) : b.c;
        }
        return stops[stops.length - 1].c;
    };
    // 一个元素自己的背景层,自上而下(背景图第一层在最上,背景色在最下)。
    const ownLayers = (el, x, y) => {
        const cs = w.getComputedStyle(el);
        const out = [];
        const img = cs.backgroundImage;
        if (img && img !== "none") {
            for (const one of splitTop(img)) {
                const c = gradAt(el, one, x, y);
                if (c) out.push(c);
            }
        }
        const bc = parseC(cs.backgroundColor);
        if (bc && bc.a > 0) out.push(bc);
        return out;
    };
    // 从 start 起沿祖先链收集,到第一层不透明为止;返回合成后的底色与是否真的碰到了不透明层。
    const backdropAt = (start, x, y) => {
        const layers = [];
        let opaque = false;
        for (let el = start; el && !opaque; el = el.parentElement) {
            for (const c of ownLayers(el, x, y)) {
                layers.push(c);
                if (c.a >= 0.999) { opaque = true; break; }
            }
        }
        let acc = { r: 255, g: 255, b: 255, a: 1 };
        for (let i = layers.length - 1; i >= 0; i--) acc = over(layers[i], acc);
        return { c: acc, opaque: opaque };
    };
    const measureBtn = (btn) => {
        const r = btn.getBoundingClientRect();
        const text = (btn.textContent || "").trim();
        const disabled = btn.disabled || btn.getAttribute("data-disabled") === "1";
        if (r.width < 1 || r.height < 1 || !text || disabled) {
            return { visible: r.width >= 1 && r.height >= 1, text: text, disabled: !!disabled, measured: false };
        }
        const cs = w.getComputedStyle(btn);
        const fg = parseC(cs.color);
        const y = r.top + r.height / 2;
        const pts = [r.left + r.width / 2, r.left + 3, r.right - 3];
        let cr = Infinity, crBar = Infinity, opaque = true;
        for (const x of pts) {
            const under = backdropAt(btn.parentElement, x, y);
            if (!under.opaque) opaque = false;
            let bg = under.c;
            const own = ownLayers(btn, x, y);
            for (let i = own.length - 1; i >= 0; i--) bg = over(own[i], bg);
            const txt = fg.a >= 1 ? fg : over(fg, bg);
            cr = Math.min(cr, ratio(txt, bg));
            crBar = Math.min(crBar, ratio(bg, under.c));
        }
        return { visible: true, text: text, disabled: false, measured: true, cr: cr, crBar: crBar, opaque: opaque, cls: btn.className };
    };
    // 临时显出一个面:去掉它与祖先上的 hidden,prep 做额外的显出动作;量完按原样还原。
    const sweep = (name, prep) => {
        const s = gb(name);
        if (!s) return { found: false };
        const unhid = [];
        for (let el = s; el && el !== d.body; el = el.parentElement) {
            if (el.hidden) { el.hidden = false; unhid.push(el); }
        }
        const undo = prep ? prep(s) : null;
        const btns = Array.from(s.querySelectorAll("button")).map((b) => Object.assign({ gb: b.getAttribute("data-gb") || b.className }, measureBtn(b)));
        if (typeof undo === "function") undo();
        for (const el of unhid) el.hidden = true;
        return { found: true, btns: btns };
    };
`;
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
    // A. Input 通道网格(SL-562)
    // =========================================================================
    // 53 字中英混排。**契约上限是 24 字**(setChannelConfig 截断),这里故意超出:格宽
    // 恒等分之后,轨名多长都只是省略号截得多一点,判据不该只在「刚好 24 字」上成立。
    const LONG =
        "bad apple_东流月干音_Lead Vocal 主唱 double take 第三遍 comp v2";
    const IN_READY = IN(`
        const cards = d.querySelectorAll('[data-gb="input.channels.card"]');
        const l = d.querySelector('[data-gb="input.channels.card"][data-ch="5"] .ipt-chcard__label');
        return cards.length === 16 && !!l && l.textContent.trim() !== "";
    `);
    const GRID = IN(`
        const g = gb("input.channels.grid");
        const chans = gb("input.channels");
        const content = gb("input.content");
        const panel = gb("input.channelPanel");
        if (!g || !chans || !content || !panel) return null;
        const cards = Array.from(d.querySelectorAll('[data-gb="input.channels.card"]'));
        const gr = g.getBoundingClientRect();
        const lab = (ch) => {
            const x = d.querySelector('[data-gb="input.channels.card"][data-ch="' + ch + '"] .ipt-chcard__label');
            return x ? { text: x.textContent, sw: x.scrollWidth, cw: x.clientWidth } : null;
        };
        const sc = (e) => [e.scrollWidth, e.clientWidth];
        // 每张卡的悬停提示面:卡自己的 title、轨名、占用角标是否可见、是否本实例选中的那张。
        const tips = cards.map((x) => {
            const l = x.querySelector(".ipt-chcard__label");
            const o = x.querySelector(".ipt-chcard__occupied");
            return {
                ch: Number(x.getAttribute("data-ch")),
                name: l ? l.textContent : "",
                labelTitle: l ? l.getAttribute("title") : null,
                title: x.getAttribute("title"),
                occ: !!o && !o.hidden,
                pressed: x.getAttribute("aria-pressed") === "true",
            };
        });
        return {
            grid: sc(g), chans: sc(chans), content: sc(content), panel: sc(panel),
            widths: cards.map((x) => x.getBoundingClientRect().width),
            overRight: cards.filter((x) => x.getBoundingClientRect().right > gr.right + 0.5).length,
            l5: lab(5), tips: tips,
        };
    `);
    const fits = (pair) => Array.isArray(pair) && pair[0] <= pair[1];

    for (const scale of [1, 0.75, 1.5]) {
        const tag = `A[${scale}]`;
        log(`=== ${tag} Input 通道网格,缩放 ${scale} ===`);
        const q = scale === 1 ? "" : `&scale=${scale}`;
        if (
            !(await nav(
                `/web-preview/input.html?fixture=fifteen-tracks${q}`,
                IN_READY,
                tag,
            ))
        )
            continue;
        const base0 = await evaluate(GRID);
        if (!check(!!base0, `${tag} A0:取到网格与三层容器`)) continue;
        const w0 = base0.widths[0];
        check(
            base0.widths.length === 16 &&
                base0.widths.every((x) => Math.abs(x - w0) <= 0.5),
            `${tag} A0:基线 16 张卡等宽(实得 ${base0.widths.map((x) => x.toFixed(1)).join("/")})`,
        );
        check(fits(base0.grid), `${tag} A0:基线网格无横向溢出(${base0.grid})`);

        // 15 条轨名换成长名,第 3 条留空(验「空轨名不挂 title」)
        const labels = Array.from({ length: 15 }, (_, i) =>
            i === 2 ? "" : LONG,
        );
        check(
            await evaluate(
                SHELL(`s.ctl.model.config.channelLabels = ${JSON.stringify(labels)};
                    s.ctl.emit("scvb.config", s.ctl.configPayload());
                    return true;`),
            ),
            `${tag} A1:经 mock 控制面推送长轨名`,
        );
        check(
            await waitFor(
                IN(`const l = d.querySelector('[data-gb="input.channels.card"][data-ch="5"] .ipt-chcard__label');
                    return !!l && l.textContent === ${JSON.stringify(LONG)};`),
                6000,
            ),
            `${tag} A1:长轨名已上屏(防空转)`,
        );
        const m = await evaluate(GRID);
        if (!check(!!m, `${tag} A1:取到网格`)) continue;
        check(
            m.l5 && m.l5.sw > m.l5.cw,
            `${tag} A1:轨名行确实被截断(scrollWidth ${m.l5 && m.l5.sw} > clientWidth ${m.l5 && m.l5.cw};不成立说明这串不够长、本格没测到东西)`,
        );
        check(
            fits(m.grid),
            `${tag} A1:网格无横向溢出(scrollWidth/clientWidth = ${m.grid})`,
        );
        check(fits(m.chans), `${tag} A1:通道区无横向溢出(${m.chans})`);
        check(fits(m.panel), `${tag} A1:组卡片无横向溢出(${m.panel})`);
        check(fits(m.content), `${tag} A1:内容区不出横向滚动(${m.content})`);
        check(
            m.widths.every((x) => Math.abs(x - w0) <= 0.5),
            `${tag} A1:每张卡宽与短名时相同(基线 ${w0.toFixed(1)},实得 ${m.widths.map((x) => x.toFixed(1)).join("/")})`,
        );
        check(
            m.overRight === 0,
            `${tag} A1:没有卡片右缘越出网格(越出 ${m.overRight} 张)`,
        );
        // A2 悬停提示:一张卡只有一个 title(挂在卡上),第一行完整轨名,被别的实例占用时
        // 第二行接「已被占用」说明。夹具 fifteen-tracks 的 occupiedMask 是全 15 位 ⇒ 除本实例
        // 选中的那张外都带占用角标 —— 两种卡都要真的出现,否则本组没测到东西。
        const tips = m.tips || [];
        const occLong = tips.find((x) => x.occ && x.name === LONG);
        const freeLong = tips.find((x) => !x.occ && x.name === LONG);
        const empty = tips.find((x) => x.ch === 3);
        const TIP = (x) => JSON.stringify(x && x.title);
        if (
            check(
                !!occLong && !!freeLong,
                `${tag} A2:夹具里同时有「长名 + 被占用」与「长名 + 未被占用」两种卡(实得 ${!!occLong} / ${!!freeLong})`,
            )
        ) {
            const lines = (occLong.title || "").split("\n");
            check(
                lines.length === 2 &&
                    lines[0] === LONG &&
                    lines[1].trim() !== "" &&
                    lines[1] !== LONG,
                `${tag} A2:被占用的卡 title = 完整轨名 + 换行 + 占用说明(卡 ${occLong.ch} 实得 ${TIP(occLong)})`,
            );
            eq(
                freeLong.title,
                LONG,
                `${tag} A2:未被占用的卡 title = 完整轨名(卡 ${freeLong.ch})`,
            );
        }
        check(
            tips.every((x) => x.labelTitle === null),
            `${tag} A2:轨名那一行不另挂 title(内层 title 会遮住卡上的占用说明)`,
        );
        check(
            !!empty &&
                empty.name === "" &&
                (empty.occ
                    ? !!empty.title && !empty.title.includes("\n")
                    : empty.title === null),
            `${tag} A2:空轨名那张卡不带轨名行(占用时只剩占用说明、未占用时不挂 title;实得 occ=${empty && empty.occ} title=${TIP(empty)})`,
        );
        assertClean(tag);
    }

    // =========================================================================
    // B. Output Lead Select(SL-562 顺查)
    // =========================================================================
    log("=== B Output Lead Select:触发钮与选项里的长轨名 ===");
    const OUT_READY = IN(`
        const a = gb("master-group-A");
        return !!(a && a.getAttribute("aria-pressed") === "true");
    `);
    const LEAD = IN(`
        const trig = gb("master-leadselect-trigger");
        const label = gb("master-leadselect-label");
        const card = gb("master-leadselect");
        const wrap = gb("master-leadselect-select");
        const panel = gb("master-leadselect-panel");
        if (!trig || !label || !card || !wrap || !panel) return null;
        const tr = trig.getBoundingClientRect();
        const ts = w.getComputedStyle(trig);
        const opt = panel.querySelector('[data-lead="1"]');
        return {
            open: wrap.getAttribute("data-open") === "1",
            text: label.textContent,
            title: label.getAttribute("title"),
            trigH: tr.height,
            trig: [trig.scrollWidth, trig.clientWidth],
            card: [card.scrollWidth, card.clientWidth],
            labelRight: label.getBoundingClientRect().right,
            // 下拉箭头是触发钮的背景图:贴内边距盒右缘 padding-right 处、宽 8px
            // (index.html .lead-select 的 background-position 与 padding 取同一个 --sp-10)。
            arrowLeft: tr.right - parseFloat(ts.borderRightWidth) - parseFloat(ts.paddingRight) - 8,
            panel: [panel.scrollWidth, panel.clientWidth],
            optH: opt ? opt.getBoundingClientRect().height : null,
            optTitle: opt ? opt.getAttribute("title") : null,
        };
    `);
    const clickSel = (sel) =>
        evaluate(
            IN(
                `const e = q(${JSON.stringify(sel)}); if (!e) return false; e.click(); return true;`,
            ),
        );
    const panelOpen = IN(
        `return gb("master-leadselect-select").getAttribute("data-open") === "1";`,
    );
    async function pickCh1(tag) {
        check(
            await clickSel('[data-gb="master-leadselect-trigger"]'),
            `${tag}:点开下拉`,
        );
        check(await waitFor(panelOpen, 4000), `${tag}:下拉已展开`);
        check(
            await clickSel(
                '[data-gb="master-leadselect-panel"] [data-lead="1"]',
            ),
            `${tag}:选中轨 1`,
        );
        check(
            await waitFor(
                IN(
                    `return /^01( |$)/.test(gb("master-leadselect-label").textContent);`,
                ),
                6000,
            ),
            `${tag}:触发钮已显示轨 1`,
        );
    }
    async function openPanel(tag) {
        check(
            await clickSel('[data-gb="master-leadselect-trigger"]'),
            `${tag}:点开下拉`,
        );
        check(await waitFor(panelOpen, 4000), `${tag}:下拉已展开`);
        const r = await evaluate(LEAD);
        await clickSel('[data-gb="master-leadselect-trigger"]'); // 收起
        return r;
    }
    if (
        await nav("/web-preview/output.html?scenario=connected", OUT_READY, "B")
    ) {
        await pickCh1("B0");
        const b0 = await evaluate(LEAD);
        const b0open = await openPanel("B0");
        const baseH = b0 && b0.trigH;
        const baseOptH = b0open && b0open.optH;
        check(
            !!baseH && !!baseOptH,
            `B0:取到基线触发钮高 ${baseH} / 选项高 ${baseOptH}`,
        );
        // 「仍是单行」的容差取 3px 而不是 0.5px:基线轨名是中文、24 个 W 是纯拉丁,两种字体的
        // 行盒本来就差约 1px(实测选项 27 vs 26);折一行至少多出一整个行高(≈ 13px 以上,
        // 改前 24 个汉字的选项实测折成三行、高 57),3px 分得开这两件事。
        const ONE_LINE_TOL = 3;
        for (const [name, L] of [
            ["24 个 W 连写", "W".repeat(24)],
            ["24 个汉字", "东流月干音".repeat(5).slice(0, 24)],
        ]) {
            const tag = `B1[${name}]`;
            check(
                await evaluate(
                    IN(`const m = w.__SCVB_MOCK__;
                        for (let ch = 1; ch <= 15; ch++) m.setChannelConfig(ch, { label: ${JSON.stringify(L)} });
                        return true;`),
                ),
                `${tag}:经桥写入长轨名`,
            );
            const want = "01 " + L;
            check(
                await waitFor(
                    IN(
                        `return gb("master-leadselect-label").textContent === ${JSON.stringify(want)};`,
                    ),
                    6000,
                ),
                `${tag}:触发钮已显示长轨名(防空转)`,
            );
            const b = await evaluate(LEAD);
            if (!check(!!b, `${tag}:取到 Lead Select`)) continue;
            check(fits(b.trig), `${tag}:触发钮无横向溢出(${b.trig})`);
            check(fits(b.card), `${tag}:所在卡片无横向溢出(${b.card})`);
            check(
                Math.abs(b.trigH - baseH) <= ONE_LINE_TOL,
                `${tag}:触发钮仍是单行(高 ${b.trigH},基线 ${baseH})`,
            );
            check(
                b.labelRight <= b.arrowLeft + 0.5,
                `${tag}:轨名右缘不压到下拉箭头(轨名右缘 ${b.labelRight.toFixed(1)},箭头左缘 ${b.arrowLeft.toFixed(1)})`,
            );
            eq(b.title, want, `${tag}:触发钮轨名 title = 「01 轨名」`);
            const o = await openPanel(tag);
            check(
                !!o && fits(o.panel),
                `${tag}:展开的选项面板无横向溢出(${o && o.panel})`,
            );
            check(
                !!o && Math.abs(o.optH - baseOptH) <= ONE_LINE_TOL,
                `${tag}:选项仍是单行(高 ${o && o.optH},基线 ${baseOptH})`,
            );
            eq(o && o.optTitle, want, `${tag}:选项 title = 「01 轨名」`);
        }
        assertClean("B");
    }

    // =========================================================================
    // C. 确认条 / 确认框的钮文字对比度(SL-560)
    // =========================================================================
    // 每面列出**必须量到**的钮(次要钮必在其中:SL-560 病在次要钮上)。量不到 = 没测到东西 = 红。
    // prep:额外的显出动作(返回还原函数)。
    const GROUP_SWITCH_PREP = `(s) => {
        const sel = s.closest('[data-gb="master-group-selector"]');
        if (!sel) return null;
        const old = sel.getAttribute("data-confirm");
        sel.setAttribute("data-confirm", "1");
        return () => { if (old === null) sel.removeAttribute("data-confirm"); else sel.setAttribute("data-confirm", old); };
    }`;
    const SURFACES = {
        output: [
            {
                tab: "tracks",
                gb: "tracks-row-1-manual-overwrite-confirm",
                need: [
                    "tracks-row-1-manual-overwrite-cancel",
                    "tracks-row-1-manual-overwrite-ok",
                ],
                hier: [
                    "tracks-row-1-manual-overwrite-ok",
                    "tracks-row-1-manual-overwrite-cancel",
                ],
            },
            {
                tab: "tracks",
                gb: "tracks-row-1-restore-auto-row",
                need: [
                    "tracks-row-1-restore-auto-cancel",
                    "tracks-row-1-restore-auto-ok",
                ],
            },
            {
                tab: "tracks",
                gb: "tracks-row-1-manualdriven-hint",
                need: ["tracks-row-1-manualdriven-dismiss"],
            },
            {
                tab: "master",
                gb: "master-write-confirm",
                need: ["master-write-confirm-undo", "master-write-confirm-ok"],
            },
            {
                tab: "master",
                gb: "master-group-switch-confirm",
                need: [
                    "master-group-switch-cancel",
                    "master-group-switch-confirm-btn",
                ],
                prep: GROUP_SWITCH_PREP,
            },
            {
                tab: "master",
                gb: "header-version-armed-confirm",
                need: [
                    "header-version-armed-cancel",
                    "header-version-armed-ok",
                ],
            },
            {
                tab: "master",
                gb: "header-version-copy-confirm",
                need: ["header-version-copy-cancel", "header-version-copy-ok"],
            },
            {
                tab: "master",
                gb: "scale-confirm",
                need: ["scale-confirm-revert", "scale-confirm-keep"],
            },
            {
                tab: "master",
                gb: "reanalyze-ask",
                need: ["reanalyze-ask-later", "reanalyze-ask-primary"],
            },
            {
                tab: "master",
                gb: "tour-ask",
                need: ["tour-ask-later", "tour-ask-start"],
            },
            {
                tab: "wave",
                gb: "wave-confirm-reidentify",
                need: [
                    "wave-confirm-reidentify-cancel",
                    "wave-confirm-reidentify-ok",
                ],
            },
            {
                tab: "wave",
                gb: "wave-confirm-clearcoverage",
                need: [
                    "wave-confirm-clearcoverage-cancel",
                    "wave-confirm-clearcoverage-ok",
                ],
            },
        ],
        input: [
            { gb: "input.group.confirm", need: ["input.group.confirm.cancel"] },
            {
                gb: "input.channels.releaseConfirm",
                need: ["input.channels.releaseConfirm.cancel"],
            },
            {
                gb: "input.scale.confirm",
                need: [
                    "input.scale.confirm.revert",
                    "input.scale.confirm.keep",
                ],
            },
        ],
        monitor: [
            {
                gb: "monitor-scale-confirm",
                need: [
                    "monitor-scale-confirm-revert",
                    "monitor-scale-confirm-keep",
                ],
            },
        ],
    };
    const MIN_TEXT = 4.5;
    const table = [];
    async function runSurface(page, sf) {
        const r = await evaluate(
            IN(
                CONTRAST_LIB +
                    `return sweep(${JSON.stringify(sf.gb)}, ${sf.prep || "null"});`,
            ),
        );
        const tag = `C[${page}:${sf.gb}]`;
        if (!check(!!r && r.found, `${tag}:找到这个面`)) return;
        const by = new Map(r.btns.map((b) => [b.gb, b]));
        for (const need of sf.need) {
            const b = by.get(need);
            check(
                !!b && b.measured,
                `${tag}:量到了 ${need}(实得 ${JSON.stringify(b || null)})`,
            );
        }
        for (const b of r.btns) {
            if (!b.measured) {
                table.push(
                    `${page} ${sf.gb} ${b.gb}: 未量(visible=${b.visible} text=${JSON.stringify(b.text)} disabled=${b.disabled})`,
                );
                continue;
            }
            table.push(
                `${page} ${sf.gb} ${b.gb} [${b.cls}] 文字/钮底 ${b.cr.toFixed(2)}:1  钮底/面底 ${b.crBar.toFixed(2)}:1`,
            );
            check(
                b.opaque,
                `${tag}:${b.gb} 的底沿祖先链找到了不透明层(结论不建立在兜底色上)`,
            );
            check(
                b.cr >= MIN_TEXT,
                `${tag}:${b.gb}「${b.text}」文字 vs 钮底对比度 ≥ ${MIN_TEXT}:1(实得 ${b.cr.toFixed(2)}:1)`,
            );
        }
        if (sf.hier) {
            const p = by.get(sf.hier[0]);
            const s = by.get(sf.hier[1]);
            if (
                check(
                    !!p && p.measured && !!s && s.measured,
                    `${tag}:主次两枚都量到`,
                )
            ) {
                check(
                    p.crBar >= 3,
                    `${tag}:主钮底 vs 条底 ≥ 3:1(实得 ${p.crBar.toFixed(2)}:1)`,
                );
                check(
                    p.crBar > s.crBar,
                    `${tag}:主钮比次钮更突出(主 ${p.crBar.toFixed(2)}:1 > 次 ${s.crBar.toFixed(2)}:1)`,
                );
            }
        }
    }
    const TAB = (t) =>
        IN(
            `const c = q("#content"); return !!c && c.getAttribute("data-tab") === ${JSON.stringify(t)};`,
        );

    log("=== C Output 确认条 / 确认框 ===");
    if (
        await nav(
            "/web-preview/output.html?scenario=connected",
            OUT_READY,
            "C:output",
        )
    ) {
        for (const tab of ["tracks", "master", "wave"]) {
            await clickSel(`[data-tab-btn="${tab}"]`);
            check(await waitFor(TAB(tab), 6000), `C:output 切到 ${tab} 页`);
            for (const sf of SURFACES.output.filter((x) => x.tab === tab))
                await runSurface("output", sf);
        }
        assertClean("C:output");
    }
    log("=== C Input 确认条 / 确认框 ===");
    if (
        await nav(
            "/web-preview/input.html?fixture=fifteen-tracks",
            IN_READY,
            "C:input",
        )
    ) {
        for (const sf of SURFACES.input) await runSurface("input", sf);
        assertClean("C:input");
    }
    log("=== C Monitor 确认框 ===");
    if (
        await nav(
            "/web-preview/monitor.html?scenario=monitor-online",
            IN(
                `const b = gb("monitor-scale-confirm-revert"); return !!b && b.textContent.trim() !== "";`,
            ),
            "C:monitor",
        )
    ) {
        for (const sf of SURFACES.monitor) await runSurface("monitor", sf);
        assertClean("C:monitor");
    }
    log("--- C 实测对比度(给 PR 描述用)---");
    for (const line of table) log("  " + line);
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
console.log("\n✅ 确认条对比度 + 长轨名不撑宽(SL-560 / SL-562)页面级冒烟全绿");
process.exit(0);
