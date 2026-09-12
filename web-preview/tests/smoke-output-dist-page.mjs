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
// [SL-353] 末节 ⑫ 越出「Output 一页」的范围:它在 **Output 与 Monitor 两页各自的文档里**
// 造同一个沙箱、喂同一批 rows,把柱数/矩形/0 dB 线/计算色逐项对拍。放在本文件是因为
// 被测面就是分布图的渲染面(本文件的主题),而两页的 `.dist-bar` / `.dist-plot__zero`
// CSS 是**手抄两份**、此前没有任何东西比对过。
//
// 用法:node web-preview/tests/smoke-output-dist-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;**2 = 环境里没有 Chrome/Edge**(口径同
//   smoke-monitor-page.mjs 与 CLAUDE.md §6:可选依赖缺席不判红,但也绝不算通过);
//   **3 = 浏览器在,但这一次没起来 / 没连上**([SL-297],见 `browserFailed()`)——
//   同样不判红,但在 gates 汇总里打 `[FLAKY-SKIP]`,免得「没跑成」被读成「跑过了」。
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
import { inflateSync } from "node:zlib";
import { dirname, extname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { FALLBACK_TRACK_COLORS } from "../../web/shared/track-colors.js";
// [SL-394 复审] (h3) 要按真常量停满去抖窗,不在这里写第二份数字。
import { HOST_ECHO_TRANSPORT_HOLD_MS } from "../../web/shared/host-echo.js";

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
// ---------------------------------------------------------------- PNG 解码
// [SL-372] 本文件此前所有断言读的都是**计算样式与矩形**;⑬ 要数的是「柱顶上方那一行
// 到底有没有浅色像素」—— 那是渲染面的问题,计算样式答不了(box-shadow 的几何合法、
// 颜色也合法,病灶恰恰是它落在了柱顶之上)。所以这里落一个**最小 PNG 解码器**:
// `Page.captureScreenshot` 回来的是 base64 PNG,zlib 是 node 内置,不引任何依赖
// (仓库红线:不引 puppeteer / pngjs 之类)。
// 只认 **8bit + 非隔行 + 真彩(color type 2/6)** —— headless Chrome 的截图恒是这一种;
// 遇到别的形态**抛错而不猜**:猜错会把一张解错的图当成「像素干净」,那正好是本卡在治的
// 那类假绿。IHDR/IDAT 之外的块(pHYs/sRGB/…)整块跳过。
function decodePng(buf) {
    const SIG = [137, 80, 78, 71, 13, 10, 26, 10];
    for (let i = 0; i < SIG.length; i++) {
        if (buf[i] !== SIG[i]) throw new Error("不是 PNG(签名对不上)");
    }
    let pos = 8;
    let ihdr = null;
    const idat = [];
    while (pos + 8 <= buf.length) {
        const len = buf.readUInt32BE(pos);
        const type = buf.toString("ascii", pos + 4, pos + 8);
        const data = buf.subarray(pos + 8, pos + 8 + len);
        if (type === "IHDR") {
            ihdr = {
                w: data.readUInt32BE(0),
                h: data.readUInt32BE(4),
                depth: data[8],
                color: data[9],
                interlace: data[12],
            };
        } else if (type === "IDAT") {
            idat.push(data);
        } else if (type === "IEND") {
            break;
        }
        pos += 12 + len;
    }
    if (!ihdr) throw new Error("PNG 里没有 IHDR");
    if (ihdr.depth !== 8 || ihdr.interlace !== 0) {
        throw new Error(
            `本解码器只认 8bit 非隔行 PNG(实得 depth=${ihdr.depth} interlace=${ihdr.interlace})`,
        );
    }
    if (ihdr.color !== 2 && ihdr.color !== 6) {
        throw new Error(`本解码器只认真彩 PNG(实得 color type=${ihdr.color})`);
    }
    const ch = ihdr.color === 6 ? 4 : 3;
    const raw = inflateSync(Buffer.concat(idat));
    const stride = ihdr.w * ch;
    const out = Buffer.alloc(ihdr.h * stride);
    let p = 0;
    for (let y = 0; y < ihdr.h; y++) {
        const f = raw[p];
        p += 1;
        const line = raw.subarray(p, p + stride);
        p += stride;
        const cur = out.subarray(y * stride, (y + 1) * stride);
        const prev = y > 0 ? out.subarray((y - 1) * stride, y * stride) : null;
        for (let x = 0; x < stride; x++) {
            const a = x >= ch ? cur[x - ch] : 0;
            const b = prev ? prev[x] : 0;
            const c = prev && x >= ch ? prev[x - ch] : 0;
            let v = line[x];
            if (f === 1) v += a;
            else if (f === 2) v += b;
            else if (f === 3) v += (a + b) >> 1;
            else if (f === 4) {
                const pa = Math.abs(b - c);
                const pb = Math.abs(a - c);
                const pc = Math.abs(a + b - 2 * c);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            } else if (f !== 0) {
                throw new Error(`未知的 PNG 行滤波器 ${f}(第 ${y} 行)`);
            }
            cur[x] = v & 255;
        }
    }
    return {
        w: ihdr.w,
        h: ihdr.h,
        px(x, y) {
            const i = (y * ihdr.w + x) * ch;
            return [out[i], out[i + 1], out[i + 2]];
        },
    };
}
// 亮度:只用来比「谁更浅」,不做色彩管理,取 Rec.601 权重即可。
const luma = (c) => 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
// 逐分量最大差:判「这一格与图底是不是同一个颜色」——比亮度更严
// (亮度相等而色相不同的像素也要判成「不一样」)。
const dmax = (c1, c2) =>
    Math.max(
        Math.abs(c1[0] - c2[0]),
        Math.abs(c1[1] - c2[1]),
        Math.abs(c1[2] - c2[2]),
    );

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
    // ⚠ CDP 截止时间**按调用点取,不取一个文件级常数**([SL-287],本机实测逼出来的)。
    //
    // 两类调用的合法时长根本不同,一个常数满足不了:
    //   · `waitFor` 内部的 evaluate —— 上界必须**小于该次 waitFor 自己的预算**,
    //     否则一次超时就吃穿整个预算,把「丢一次响应下一轮补上」变成硬红。
    //   · 一次性的直接 evaluate —— 里面可能是**故意跑很久**的在页探针。
    //     实测 smoke-output-dist-page 的 `measureOff` 合法跑满 12s,而同一文件最紧的
    //     `waitFor` 预算也是 12s:两个约束互相矛盾,任何单一常数都满足不了。
    //
    // 所以:`waitFor` 内部的 evaluate 传**本次还剩多少预算**(见下面 waitFor —— 第一版写的是
    // 「预算的一半」,被复审指出那会把耗时落在 (ms/2, ms) 的**合法**调用从过变成必红,
    // 等于新增一类红;按剩余预算取则不改变任何原本能过的行为)。其余调用用这个宽的默认值 ——
    // 它只负责兜住**真挂死**,不负责区分快慢。
    const CDP_DEFAULT_TIMEOUT_MS = 20000;
    return {
        ready,
        on: (fn) => listeners.push(fn),
        send(method, params, timeoutMs) {
            const mid = ++id;
            const budget = timeoutMs || CDP_DEFAULT_TIMEOUT_MS;
            return new Promise((ok, no) => {
                // 每条 CDP 调用都必须有截止时间:原版把 resolve 塞进 `pending` 就返回,
                // 响应不来就**永远不 resolve**。SL-274 在同源的 seg-diff-fold 上实测挂过
                // 75 分钟零输出(Chrome 与 node 都还活着)。
                // ⚠ 因果限定在**当时**:那次还赶上 [SL-277] 拆锁**之前**的形态 ——
                // 整条 gates 被外部目录锁包着,所以一套挂死会把整批 agent 一起堵住。
                // [SL-301] 起 3e 也持 `Local\SCVB-ipc-tests` 了(它的 Chrome 负载会把同机
                // 别人的 gate 6 拖红)⇒ **一套挂死会堵住全场**,不再只停死本轮。
                // 这正是本文件那条 CDP 截止时间与 gates 3e 的 300s/套上界现在更要紧的原因。
                // 那仍然是一整轮,所以超时照加;但别照着旧说法去推断锁的作用域。
                // CI 上则是一路烧到 job 超时才红。
                //
                // 超时**抛错而不重试**:响应不来说明页面或渲染器已经不对了,
                // 重试只会把一个确定的红拖成一个更慢的红。错误里带 method 与 id,
                // 红出来直接指到是哪一条卡住。
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

// [SL-297] **浏览器在,但没起来 / 没连上** —— 与 `noBrowser()` 分开走 **退出码 3**。
// 为什么必须分开:gates 3e 与 CI 都把 **2 读成「本机没有浏览器」** 并按可选依赖记 SKIP、
// 照算 PASS。而「装着 Chrome、这一次没连上」是**一次失败的运行**,不是缺依赖 ——
// 压成同一个码之后,一台装着 Chrome 的机器上一次瞬时超时就会让整套判据**无声消失**,
// 汇总行还写着全 PASS(SL-293 实测撞到三次,每次掉的套件还不一样)。
// **仍然不判红**(理由见调用点):判红会把每个 PR 卡在与改动无关的环境抖动上。
// 3 的语义就是「这一轮没跑成,而且不是因为没装浏览器」——由 gates 打成 [FLAKY-SKIP]。
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

// ⚠ 收尾必须走**所有**退出路径,不只是跑完那一条([SL-287])。
// 本文件原有的 try/finally 只包住**主断言体**,而 Chrome 是在进入那个 try **之前**就 spawn 的 ——
// 那段窗口里抛错(CDP 连接、Page.enable、首次导航,恰好是上面新加的超时最可能开火的地方)
// 就会把 headless Chrome 留在机器上。
// 而且信号完全没人接:SL-287 同时给 gates 3e 加了整套超时,超时会向本进程发信号。
//
// ⚠ **本机(Windows)实测的边界,别把这段的作用说大**:
//   · 浏览器进程:node 一死,Windows 会把 spawn 出来的 Chrome 一起收掉 —— 实测原版在
//     SIGTERM 下也能从 10 个进程回到 0。所以在 Windows 上这段对**进程**是双保险,不是唯一解。
//     它真正吃劲的地方是 **Linux**:`web-smoke` 跑在 ubuntu-latest,而 POSIX 下父进程退出
//     **不会**自动收掉 spawn 的子进程 —— 而本文件的 try/finally 只包住主断言体,
//     spawn 到进 try 之间那段窗口(CDP 连接、Page.enable、首次导航)在 Linux 上没人收。
//   · 临时目录:`chrome.kill()` 之后文件句柄未必立刻释放,紧跟的 `rmSync` 在 Windows 上
//     **会失败**,留下一个空壳目录 —— 实测本 PR 版本与原版在注入失败时**同样各留 1 个**。
//     这一点不吹:本机 temp 下现有 981 个 `scvb-*` 残留目录,这段收不干净它们。
//     它保证的是「每条退出路径都**尝试过**收尾」,以及在 Linux 上真的收得掉。
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
// `exit` 处理器只能同步收尾(Node 规范),所以这里不等句柄、不重试 rmSync ——
// 留一个空壳目录是可接受的残渣(系统会清),而**跑着的 headless Chrome 不是**。
process.on("exit", teardown);
for (const sig of ["SIGINT", "SIGTERM"]) {
    process.on(sig, () => {
        teardown();
        process.exit(130);
    });
}
// 未捕获异常 / 未处理拒绝:先打印再收尾,否则 Chrome 会跟着一起漏。
// 新加的 CDP 超时是**定时器里 reject**,那条 promise 当时若没人 await,
// 就会以 unhandledRejection 形式到这里 —— 这一支不是摆设。
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

// 走带开关在**壳页**上:走带是宿主的东西,`__SCVB_MOCK__` 只有桥面的上行函数。
// [SL-394] 提到模块级:⑪ 的两个块各自是独立作用域,两边都要用它。
const setPlaying = (on) =>
    evaluate(`(() => {
    const s = window.__SCVB_PREVIEW__;
    if (!s || !s.ctl) return "no-session";
    s.ctl.setTransport({ isPlaying: ${on ? "true" : "false"} });
    return "ok";
})()`);

async function waitFor(expr, ms = 15000) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {
        let v = null;
        try {
            // 这次 evaluate 的上界 = **本次 waitFor 还剩多少预算**(留 250ms 收尾),
            // 不是「预算的一半」。第一版写成 ms/2,被复审指出**把语义改窄了**:
            // 一次耗时落在 (ms/2, ms) 区间的**合法**调用,改动前能过、改动后必红 ——
            // 而 monitor 这一套的实测最慢单次是 3020ms、上界只有 5000ms,余量 1.65 倍,
            // 在与别的 job 抢 CPU 的 ubuntu runner 上抖一下就会把慢但合法判成红。
            // (PR 里那次「未复现的 monitor exit=1」很可能就是这个,首要假设。)
            // 按剩余预算取则**不改变任何原本能过的行为**:慢调用可以用掉几乎整个预算,
            // 真挂死仍会在预算到点前被砍断,由下面的 while 条件收尾。
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
        // [SL-394] 这一节里走带**仍在跑**(?play=1)。旧口径下静置越过播放档窗口徽标就该熄,
        // 新口径下**不该** —— 播放期闩锁一旦扣上就亮到停止。所以这里静置的用途从
        // 「等它熄」变成「证明它不熄」:3.4s 远超停走档 900,任何窗口制实现都会在此熄掉。
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
        // [SL-394] **语义换了,这一格跟着换,并且是有意钉住新语义**:
        // 播放中宿主停写(这里是关掉输出让打印头停)之后,徽标**仍然亮**——闩锁扣在
        // 「本次播放里写过」上,不再问「最近一次写有多久了」。
        // 这正是用户裁定选的那一面:插件分不出「自动化平直」与「自动化结束」
        // (桥面上都是「不再有 scvb.params 帧」),宁可多亮,不可在播放中途消失。
        // ⚠ 上一版这里断的是相反的事(静置越过播放档后三张卡与徽标都已熄)。那条随
        // 播放档窗口一起作废;别照它推论「停写就该熄」。
        check(
            quiet.width === "1" &&
                quiet.widthBadge === "1" &&
                quiet.ms === "1" &&
                quiet.lead === "1",
            `(h) ★ 播放中宿主停写 3.4s(远超停走档 900ms)⇒ 徽标与三张卡**仍亮**` +
                `(实得 ${quiet.width}/${quiet.ms}/${quiet.lead},徽标 ${quiet.widthBadge})` +
                ` —— 退回任何窗口制实现,这一格当场红`,
        );
        // (h2) 熄侧改由**停走**触发:这才是新口径下徽标唯一的熄灭路径,也是用户裁定里
        // 「停止后 ≤1s 熄灭」那一条的页面级落点。量的是**停走指令之后**多久熄
        //(停走边沿那一拍 render + 停走档窗口,上界 = 去抖 500 + 50 与 at+900 的较大者)。
        await setPlaying(false);
        const stopT0 = Date.now();
        let offMs = -1;
        for (let i = 0; i < 40; i++) {
            const st = await evaluate(readState);
            if (st.widthBadge === "0") {
                offMs = Date.now() - stopT0;
                break;
            }
            await sleep(50);
        }
        check(
            offMs >= 0 && offMs <= 1500,
            `(h2) ★ 停走后徽标在 ≤1s(判据留 1.5s 余量给页面轮询)内熄灭` +
                `(实得 ${offMs < 0 ? "2s 内从未熄" : offMs + "ms"})`,
        );
        // 收尾:把走带放回播放,后面的段落沿用 `?play=1` 的前提。
        await setPlaying(true);
        await sleep(200);

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
    // ⑪ [SL-280] 柱高真的随音量变 —— 用户点名的那一幕
    // -------------------------------------------------------------------------
    // 用户实测(v5.6.5):「柱状图里面的柱子高度不是应该代表音量吗,现在为什么都一样高?」
    //
    // 定谳:柱高一直绑着 volDb(不是没做),病在旧式归一后**又除 0.70** 再夹 8..88 ——
    // volDb ≥ −1.824 dB 一律画成 88%。每轨 vol 出厂默认就是 0 dB,真实工程各轨也多坐在
    // unity 附近,于是齐刷刷一样高。
    //
    // ⚠ 为什么必须页面级、纯函数用例不够:纯函数断的是 `barHeightPct` 的返回值,
    //   断不到「这个数真的变成了屏幕上的高度」。中间还隔着 `--h` 写入面、CSS
    //   `height: var(--h)` 与容器高 —— 任何一环断掉,纯函数照样全绿而柱子照样一样高。
    //   这里读 `getBoundingClientRect().height`,量的是渲染结果。
    //
    // ⚠ 用 `?scenario=hot-levels`:DEMO_TRACKS 的推子行程最高 0.62(−1.7 dB),全部落在
    //   旧公式饱和点之下 —— 换句话说**默认 fixture 恰好避开了缺陷区间**,这正是它三个月
    //   没被发现的原因。该场景把 ch1..ch4 顶到 +12 / +6 / 0 / −6 dB,跨过 unity 两侧。
    // =========================================================================
    {
        newBucket("sl280-bar-height");
        await cdp.send("Page.navigate", {
            // ⚠ **不带 `play=1`**:走带一跑,mock 的引擎打印头就按段表逐帧改写 vol 参数面,
            // 把本场景的初值冲掉(实测柱高会朝段表值漂,四档差被压成 ~1.2dB 一档)。
            // 柱高映射与走带无关,静态页反而是确定性的量法。
            url: `${base}/web-preview/output.html?scenario=hot-levels`,
        });
        check(await waitFor(READY), "页面装载并吃到首帧");
        // rAF 补间从上一状态插到目标值,补间窗 40ms 级;给足余量再量,避免量在中途。
        await sleep(600);

        // 逐柱读**渲染高度**(按 data-ch 取,不靠下标)
        const barH = IN(`
            const out = {};
            for (const n of all(".dist-bar")) {
                const ch = n.getAttribute("data-ch");
                out[ch] = Math.round(n.getBoundingClientRect().height * 100) / 100;
            }
            return out;
        `);
        const hs = await evaluate(barH);
        log(
            `  ch1..ch4 渲染高度:${JSON.stringify([1, 2, 3, 4].map((c) => hs[c]))}`,
        );

        check(
            hs && Object.keys(hs).length > 0,
            `(a) 前提:页面上确有柱(实得 ${hs && Object.keys(hs).length} 根)`,
        );
        const quad = [1, 2, 3, 4].map((c) => hs[String(c)]);
        check(
            quad.every((v) => typeof v === "number" && v > 0),
            `(a) 前提:ch1..ch4 四根柱都量到了高度(实得 ${JSON.stringify(quad)})`,
        );

        // ★ 核心:+12 / +6 / 0 / −6 dB 四档,渲染高度必须**两两不同且严格递减**。
        //   旧式下这四档的 --h 全是 88% ⇒ 四个数全等 ⇒ 本条红。
        check(
            new Set(quad).size === quad.length,
            `(b) ★ 四档音量的柱高**两两不同**(实得 ${JSON.stringify(quad)})` +
                ` —— 用户报的「都一样高」就是这四个数全等`,
        );
        check(
            quad[0] > quad[1] && quad[1] > quad[2] && quad[2] > quad[3],
            `(c) ★ 音量越大柱越高,严格递减 +12 > +6 > 0 > −6(实得 ${JSON.stringify(quad)})`,
        );

        // ---- 0 dB 基准线:必须落在「0 dB 那根柱(ch3)的顶边」上
        //
        // 这一条同时是**接线判据**。降级分两档,红的理由不同,别读混:
        //   • 只删建器里的 `setProperty`(`has-zero-line` 还在)⇒ 线照显示,但 `bottom`
        //     整条声明失效 ⇒ `bottom:auto` ⇒ 绝对定位又没给 `top` ⇒ 退回静态位置 =
        //     `.dist-plot` 内容盒**顶边**。
        //     实测(2026-09-03,`?scenario=hot-levels`,注入 = 把上面那行 `setProperty`
        //     换成 `void 0` 后整套重跑;下列四个数同一次读出,取自本文件的 `zeroGeom`):
        //       正常态  线 top 592.39 / ch3 柱 top 593.39  ⇒ 压在柱顶(容差 1.5px 内)
        //       降级态  线 top 540.22 / ch3 柱 top 593.39  ⇒ 差 53.17px,线跑到了上面
        //     「上面」具体是哪:同次读到 `.dist-plot` 边框盒 top = 539.22、1px 边框
        //     ⇒ 内容盒顶边 = 540.22,与降级态线 top **逐位相同** —— 这才是「退回静态
        //     位置」的直接证据。(别拿柱 top 593.39 + 柱高 108.11 去推图顶:那两个数
        //     算出来的是柱底 701.5,与图顶无关;柱高 108.11 在这里只用于交叉核对
        //     `barHeightPct(0)=61.33%` 确实铺在块高上。)
        //   • 建器里的 `classList.add` 也删掉 ⇒ `.dist-plot__zero` 默认 `display:none`
        //     ⇒ 矩形全零 ⇒ `lineTop = 0`,同样红,但红在「压根没画」而不是「错位」。
        // (旧版这里写「线跑到容器**底部**」—— 方向是反的;而「CSS 侧不给缺省」那句也已被
        //  `has-zero-line` 取代。两处一并订正,全仓回扫过:无同义残留。)
        const zeroGeom = IN(`
            const line = q(".dist-plot__zero");
            const bar3 = q('.dist-bar[data-ch="3"]');
            if (!line || !bar3) return null;
            const lr = line.getBoundingClientRect();
            const br = bar3.getBoundingClientRect();
            const cs = w.getComputedStyle(line);
            return {
                lineTop: Math.round(lr.top * 100) / 100,
                barTop: Math.round(br.top * 100) / 100,
                bottom: cs.bottom,
                label: (q(".dist-plot__zero-label") || {}).textContent || "",
            };
        `);
        const zg = await evaluate(zeroGeom);
        log(`  0 dB 线:${JSON.stringify(zg)}`);
        check(!!zg, "(d) 前提:页面上确有 .dist-plot__zero 与 ch3 的柱");
        if (zg) {
            check(
                Math.abs(zg.lineTop - zg.barTop) <= 1.5,
                `(d) ★ 0 dB 基准线压在 0 dB 那根柱的顶边上` +
                    `(线 top ${zg.lineTop} vs 柱 top ${zg.barTop},容差 1.5px)` +
                    ` —— 也是 --zero-h 的接线判据:没写进去 bottom 就失效`,
            );
            check(
                /^0\s*dB$/.test(String(zg.label).trim()),
                `(e) 基准线左端标注是 "0 dB"(词条 master.distZero;实得 ${JSON.stringify(zg.label)})`,
            );
        }

        assertClean("SL-280 柱高映射");
    }

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
    // 消失。SL-270 当时判 ② 是 ① 的另一面 —— 按停那一刻闩锁还剩一大截,立刻重按播放,
    // 这一截残余在新的一段播放里走完。
    //
    // ⚠ 同样先说清楚**证明不了**什么:② 的完整现象需要「宿主两次写之间隔着秒级」,而 mock
    //   的打印头一恢复就立刻推帧(本文件 SL-251 节已记过:mock 只有慢通道)。所以这里不去
    //   赌那一幕,而是把**机理**钉在渲染面上:两档窗口各自真的在生效。
    // ⚠ [SL-356] 上一版这里还写着「② 的修复等价于『按停即回短窗口』,那就是 (d)」——
    //   **按实测是错的**,已删。v5.6.7 用户原话:「停播后徽标及时熄灭没问题;但快速起停
    //   几次,播放中徽标还是会中途消失」。② 的真因是走带态那一头零迟滞(一帧 false 就把
    //   窗口收到 900),治它的是下面 ⑪ 的走带态去抖;本节 (a)-(e) 一格未动,守的仍是
    //   「两档确实分开」,别再把本节读成「② 已经被钉住了」。
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
        // [SL-394] `setPlaying` 已提到模块级 —— ⑪ 的两个块是**各自独立的作用域**,
        // 而 (h2) 在前一个块里也要用它;留在这里的话前一个块拿到的是 TDZ 报错,
        // 而 `node --check` 看不出来(它只查语法)。

        // ---- 量法:从**徽标亮起那一刻**量到它熄灭,而不是从「我发了指令」那一刻量。
        //
        // 这一条是本节能不能算数的关键。闩锁量的是「距最后一帧 hostEcho:true 多久」,
        // 而那一帧什么时候来我们并不知道 —— mock 的打印头是「值变了**或**回声位翻转才发」,
        // 实测两帧之间能隔两三秒。从指令时刻起算的话,测出来的间隔里混着一段未知的「上一帧有多旧」,
        // 只能给上界、给不出下界。[SL-394] 「播放档确实更宽」这半条已随播放档一起作废,
        // 但**量法本身照旧要紧**:(b) 量的停走档仍靠它才有下界(本仓记过
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
            // [SL-394] **打灭必须连走带一起停**:播放期闩锁之下,只关打印头是灭不掉的
            // (那正是本卡要的行为,也是 (a)(h) 两格钉的东西)。上一版这里只 `setOutput(false)`,
            // 于是这一句前置在新口径下永远等不到灭 —— 本卡首跑就红在这里,而红出来的
            // 行文是「先把徽标打灭」,真因却是「新语义下它不该灭」。
            await setOutput(false);
            await setPlaying(false);
            if (
                !check(
                    await waitFor(badgeOff, 12000),
                    `(${label}) 前置:先把徽标打灭(需连走带一起停 —— 闩锁只认停走)`,
                )
            )
                return null;
            await setPlaying(true); // 重新起播,下面量的是「这一段播放里写过之后停走」
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

        // ---- (a) [SL-394] **播放期闩锁**:关掉打印头,但走带**继续跑** ⇒ 徽标不该熄。
        // 旧版这里量的是「播放档窗口有多宽(≈2500ms)」;播放档已删,量它没有意义了,
        // 而**它不熄**这件事本身正是本卡要钉的东西。所以改成:先等徽标亮起(起点由
        // 0→1 的跳变钉死,与旧版同一条纪律),再停写 5 秒,全程采样必须一直是 1。
        // 5000ms 远超停走档 900,也超过被删掉的播放档 2500 —— 任何窗口制实现都会在此熄。
        const litThroughFlat = await (async () => {
            // 前置照 measureRelease 的 preamble:先灭 → 开打印头 → 等 0→1 那一次跳变。
            // 上一节 (h) 把输出关掉了,不重新打开的话徽标永远亮不起来 —— 本卡首跑就是
            // 这么红的,而红出来的行文是「徽标先亮起来」,真因却在上一节的收尾状态。
            await setPlaying(true);
            await setOutput(false);
            if (
                !check(await waitFor(badgeOff, 12000), "(a) 前置:先把徽标打灭")
            ) {
                return null;
            }
            await evaluate(armOnEdge);
            await setOutput(true);
            if (
                !check(
                    await waitFor(onEdgeSeen, 15000),
                    "(a) 前置:观察到徽标 0 → 1 的那一次跳变",
                )
            ) {
                return null;
            }
            await setOutput(false); // 打印头停 ⇒ 不再有 hostEcho 帧(= 自动化平直/结束)
            const samples = [];
            for (let i = 0; i < 20; i++) {
                await sleep(250);
                samples.push(await evaluate(badge));
            }
            return samples;
        })();
        if (litThroughFlat) {
            const lit = litThroughFlat.filter((v) => v === "1").length;
            check(
                lit === litThroughFlat.length,
                `(a) ★ 播放中宿主停写 5s ⇒ 徽标全程不灭(实得 ${lit}/${litThroughFlat.length} 次采样为亮)` +
                    ` —— 这是 B29 的页面级落点:退回任何窗口制实现,这一格当场红`,
            );
        }
        // ---- (b) **停走档**:打印头开着,直接停走带(mock 的 PRINT 是三与,停走即停印)。
        const stoppedMs = await measureRelease("b 停走档", () =>
            setPlaying(false),
        );

        check(
            typeof stoppedMs === "number" && stoppedMs > 0,
            `(b) 停走档测到了有效读数(实得 ${stoppedMs})`,
        );
        // 判据写成**区间**而不是等号:读数里含一次 CDP 往返 + 采样步长 + 一拍 render,
        // 上下留足余量。
        // [SL-394] 这一段原先写的是「两条各自都是删除式判据:退回一个窗口打天下 ⇒ (c) 红;
        // 调用方忘了传走带态 ⇒ 播放档掉到停走档上 ⇒ (a) 红」——**两条都随播放档作废**:
        // (c) 已删,而「传没传走带态」这个形状也不存在了(判据改吃整个 store)。
        // 现在这两条这样分工,比原来的差值判据更强(差值判据在「两档同比例放大」时是绿的):
        //   • (a) 播放中停写 5s 不熄 —— 退回**任何**窗口制实现即红;
        //   • (b) 停走后落在停走档量级 —— 把停走档也换成闩锁(「永不熄」)即红。

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
        // 它在 950ms 就烧完了(那一版闩锁还亮着,950 < 当时的播放档),之后再没有东西来 render ——
        // 徽标与 Tab2 灰显**永久滞留**。
        //
        // `ctl.setHostTimeAvailable(false)` 复现的就是这类宿主(预览专用开关,见
        // juce-bridge-mock 那一条)。采集闸不用管:`setOutputEnabled(true)` 的契约副作用
        // 已经把 capture 关了,所以 `scvb.captureProgress` 这条 2Hz 的 render 源本来就
        // 不在场 —— 本节量到的熄灭只可能来自定时器。
        //
        // ⚠ [SL-394] 以上是**旧口径的追述**(播放档 + 它那一拍定时器都已删除)。
        // 新口径下 (e) 断的是反面:播放期一次 render 都不排时徽标**仍亮**,见下面那一段。
        const setHostTime = (on) =>
            evaluate(`(() => {
            const s = window.__SCVB_PREVIEW__;
            if (!s || !s.ctl || !s.ctl.setHostTimeAvailable) return "no-hook";
            s.ctl.setHostTimeAvailable(${on ? "true" : "false"});
            return "ok";
        })()`);
        // [SL-394] 旧版这里量的是「播放期一次 render 都不排时,徽标仍按播放档熄灭
        // (≈2500ms,靠播放档那一拍定时器)」。播放档与那一拍都已删除,而新口径下
        // **播放中本来就不该熄** —— 所以这一格反过来钉:即便没有任何 render 源,
        // 徽标也必须保持亮着,不会被别的定时器误熄。
        // ★ 删除式:让停走档那一拍定时器在播放中也去熄徽标(或把闩锁去掉),本格当场红。
        await setOutput(false);
        check(
            (await setHostTime(false)) === "ok",
            "(e) 前提:预览会话认得 setHostTimeAvailable(没有它这一幕造不出来)",
        );
        const frozen = [];
        for (let i = 0; i < 16; i++) {
            await sleep(250);
            frozen.push(await evaluate(badge));
        }
        const frozenLit = frozen.filter((v) => v === "1").length;
        check(
            frozenLit === frozen.length,
            `(e) ★ 播放期一次 render 都不排 + 宿主停写 4s ⇒ 徽标仍亮` +
                `(实得 ${frozenLit}/${frozen.length} 次采样为亮)`,
        );
        await setHostTime(true);

        // ---- (f) [SL-394] **播放中途一帧裸 `isPlaying:false` 不得结束本次播放窗**。
        //
        // 卡面点名的第三格,也是本卡与 SL-356 的交界处:SL-356 治的是「一帧假停走把
        // 释放窗口收窄、当拍熄灭」,本卡新增的是「一帧假停走把**本次播放的起点**推到
        // 宿主那次写之后,于是闩锁失效」。**两条路不同、落点也不同**,所以单独一格。
        //
        // 造法:走带在播放 → 等徽标亮起(= 本次播放里已经写过)→ 打印头停(不再有新的
        // hostEcho 帧)→ 发**一帧** `isPlaying:false` 紧接着立刻恢复播放(远小于去抖窗)
        // → 再静置到远超停走档。徽标全程必须是亮的。
        //
        // ★ 删除式(两条,各红各的):
        //   • 把 `playbackStartedAt` 的记账改成「每帧都刷新起点」⇒ 起点跑到那次写之后,
        //     闩锁失效 ⇒ 本格红;
        //   • 把 `hostEchoUseWideWindow` 的去抖拆掉 ⇒ 那一帧当拍收窄到停走档 ⇒ 本格红。
        {
            await setPlaying(true);
            await setOutput(true);
            if (check(await waitFor(badgeOn, 15000), "(f) 前置:徽标先亮起来")) {
                await setOutput(false); // 之后不会再有 hostEcho 帧
                await setPlaying(false);
                await sleep(60); // 远小于走带去抖窗(500ms)
                await setPlaying(true);
                const after = [];
                for (let i = 0; i < 12; i++) {
                    await sleep(250);
                    after.push(await evaluate(badge));
                }
                const lit = after.filter((v) => v === "1").length;
                check(
                    lit === after.length,
                    `(f) ★ 播放中途一帧假停走(60ms)之后,徽标全程仍亮` +
                        `(实得 ${lit}/${after.length} 次采样为亮)`,
                );
            }
        }

        // ---- (g) [SL-394 复审【行为错】①] **播放中 playhead 断流之后,徽标仍亮**。
        //
        // `scvb.playhead` 断流的路子有好几条,每一条都与「走带停了」无关:面板切走整帧
        // 丢弃、载荷逐帧逐字相同被两道去重挡掉、消息线程堵住几帧挤着晚到。
        // 若「本次播放的起点」按**时间空洞**判(距上次非停走 ≥ 去抖窗就算新段),断流之后
        // 那一帧 `isPlaying:true` 会开出一段新播放 ⇒ 起点跑到宿主那次写之后 ⇒ 闩锁当场掉,
        // 徽标在播放中途熄灭 —— 本卡要治的那一幕换了个触发条件又回来。
        //
        // 造法:用 `setHostTimeAvailable(false)` 把 playhead 载荷钉成逐帧逐字相同
        //(native 那条真实分支的预览等价物),两道去重于是把整段都挡掉 = 断流;
        // 静置 2s 之后再恢复,期间与之后徽标都必须一直亮。
        //
        // ★ 删除式:把 `playbackStartedAt()` 退回「!prev || now - prev >= HOLD ⇒ now」,
        //   本格与 node 侧 (a24b) 一起红。
        {
            await setPlaying(true);
            await setOutput(true);
            if (check(await waitFor(badgeOn, 15000), "(g) 前置:徽标先亮起来")) {
                await setOutput(false); // 之后不会再有 hostEcho 帧
                check(
                    (await setHostTime(false)) === "ok",
                    "(g) 前置:预览会话认得 setHostTimeAvailable(断流靠它造)",
                );
                await sleep(2000); // 断流 2s(远超走带去抖窗 500ms)
                const midStall = await evaluate(badge);
                await setHostTime(true); // 恢复供帧:下一帧就是 isPlaying:true
                const after = [];
                for (let i = 0; i < 8; i++) {
                    await sleep(250);
                    after.push(await evaluate(badge));
                }
                const lit = after.filter((v) => v === "1").length;
                check(
                    midStall === "1" && lit === after.length,
                    `(g) ★ 播放中 playhead 断流 2s ⇒ 断流期间(实得 ${midStall})与恢复供帧之后` +
                        `(实得 ${lit}/${after.length} 次采样为亮)徽标都仍亮 —— 时间空洞不得开新段`,
                );
            }
        }

        // ---- (h3) [SL-394 复审【行为错】①] **新的一段播放没有宿主写 ⇒ 不亮。**
        //
        // 这一格钉的是**接线**,不是零件:上一版把 `playbackStartedAt()` 的第三实参写成
        // `store.playhead`,而 `store.playhead = p` 就在它上面几行 —— 传进去的其实是本帧,
        // 于是 `wasStopped` 恒 false、起点一个会话只写一次,闩锁退化成「自第一段播放起
        // 一直亮」。那时**两处纯函数用例(含逐帧模拟)全绿**,页面级也没有一格看得见它。
        //
        // 造法:播放 + 让宿主写一次(徽标亮)→ 停走并**停满去抖窗**(徽标灭)→ 再按播放,
        // 但**整段不让宿主写**(输出保持关)⇒ 越过停走档之后必须**仍然不亮**。
        // 起点没跟着新段走的话,`at >= start` 仍成立,徽标会在第二段里亮着 —— 本格当场红。
        //
        // ★ 删除式:把实参换回 `store.playhead`,本格与 node 侧 (b0b)(b0c) 一起红。
        {
            await setPlaying(true);
            await setOutput(true);
            if (
                check(
                    await waitFor(badgeOn, 15000),
                    "(h3) 前置:第一段里徽标亮起",
                )
            ) {
                await setOutput(false); // 之后整程不再有 hostEcho 帧
                await setPlaying(false);
                if (
                    check(
                        await waitFor(badgeOff, 6000),
                        "(h3) 前置:停走后徽标先灭(第一段结束)",
                    )
                ) {
                    // 停满去抖窗再起播 —— 这才算「新的一段」
                    await sleep(HOST_ECHO_TRANSPORT_HOLD_MS + 200);
                    await setPlaying(true);
                    const seen = [];
                    for (let i = 0; i < 8; i++) {
                        await sleep(250);
                        seen.push(await evaluate(badge));
                    }
                    const litCount = seen.filter((v) => v === "1").length;
                    check(
                        litCount === 0,
                        `(h3) ★ 新的一段播放里没有任何宿主写 ⇒ 全程不亮` +
                            `(实得 ${litCount}/${seen.length} 次采样为亮;` +
                            `>0 = 起点没跟着新段走,闩锁把上一段的写算进来了)`,
                    );
                }
            }
        }

        assertClean("SL-270 释放窗口");
    }

    // =========================================================================
    // ⑪ [SL-356] hostEcho 徽标:走带态**去抖**
    // -------------------------------------------------------------------------
    // 用户实测(v5.6.7)原话:「停播后徽标及时熄灭没问题;但快速起停几次,播放中徽标
    // 还是会中途消失」。SL-270 的两档只治了 ①(停走后滞留),② 没治住 —— 上面 ⑩ 的
    // 头注按实测已经订正过一次,别再照 SL-270 当时那句「② 也就不存在了」推论。
    //
    // 定谳:SL-270 只给了**熄侧**迟滞(亮立刻、熄延迟挂在 `hostEchoAt` 上),走带态那一头
    // 零迟滞 —— `hostEchoUseWideWindow` 上一版是「当前这一帧 `isPlaying`」的纯函数,
    // 一帧 false 就把释放窗口从当时的播放档收到 900;而播放中「距最后一次宿主写入超过 900ms」
    // 完全正常,于是那一帧到达的**当拍**徽标就熄。
    // ⚠ [SL-394] 播放档窗口已删,上面这段是**当时的判断记录**;今天播放中该不该亮由
    // 播放期闩锁回答,去抖仍在(它治的是另一条路:一帧假停走)。
    //
    // 为什么必须页面级:纯函数层的判据在 smoke-tab1-interactions ⑧(a15..a20),
    // 它断不到「渲染面真的按这条判据在写属性」,更断不到「停走之后还有人来 render」——
    // 后者是 (b) 的删除式唯一能看见的那一段。
    //
    // 非空绿怎么保证:两条都不靠「我发指令时大概过了多久」去估,而是读页内的
    // `__SCVB_OUTPUT__.hostEcho()`(只读快照:`at` / `playingAt` / `stopped` / `wide` / `on`)
    // 当场量。本仓为「按帧率/节奏判红」栽过好几次,这里把那半条依赖直接去掉。
    // =========================================================================
    {
        newBucket("sl356-transport-debounce");
        await cdp.send("Page.navigate", {
            url: `${base}/web-preview/output.html?scenario=curve-editor&play=1`,
        });
        check(await waitFor(READY), "页面装载并吃到首帧");

        const BADGE356 = `d.querySelector('[data-gb="master-width-hostbadge"]')`;
        const badgeOn356 = IN(`
            const n = ${BADGE356};
            return !!n && n.getAttribute("data-on") === "1";
        `);
        const setOutput356 = (on) =>
            evaluate(
                IN(`
            const mk = w.__SCVB_MOCK__;
            if (!mk) return "no-mock";
            return JSON.stringify(mk.setOutputEnabled(${on ? "true" : "false"}));
        `),
            );
        const setPlaying356 = (on) =>
            evaluate(`(() => {
            const s = window.__SCVB_PREVIEW__;
            if (!s || !s.ctl) return "no-session";
            s.ctl.setTransport({ isPlaying: ${on ? "true" : "false"} });
            return "ok";
        })()`);

        check(
            (await setPlaying356(true)) === "ok",
            "(前提) 壳页暴露了预览会话",
        );
        const armR = await setOutput356(true);
        check(
            typeof armR === "string" && !/rejected|"ok":\s*false/.test(armR),
            `(前提) 打开打印头被接受(回执 ${armR})`,
        );
        check(
            await waitFor(badgeOn356, 15000),
            "(前提) 徽标先亮起来 —— 亮要等打印头写出一个**变化**的值,mock 上实测 2-3 秒",
        );
        check(
            await evaluate(
                IN(`
            const m = w.__SCVB_OUTPUT__;
            return !!(m && typeof m.hostEcho === "function" && m.hostEcho().at > 0);
        `),
            ),
            "(前提) 页面挂出了 hostEcho 只读快照且已收到过 true 帧(__SCVB_OUTPUT__.hostEcho)",
        );

        // ---- (a) ★ 快速起停:徽标全程不灭
        //
        // 先掐掉信号源(关打印头 ⇒ 此后 `hostEchoAt` 不再前进),再按 150ms 一档翻走带,
        // 逐样本记三件事:徽标属性、`Date.now() - at`(距最后一次宿主写入)、以及那一刻
        // 页面手上的走带态是不是「明确停走」。采样**按 age 收尾**而不是按墙钟收尾:
        // 这样窗口右端与 CDP 往返、rAF 节奏全都无关。
        const jitter = await evaluate(
            `(() => {
            const s = window.__SCVB_PREVIEW__;
            const f = document.querySelector("iframe");
            const w = f && f.contentWindow;
            const d = f && f.contentDocument;
            if (!s || !s.ctl || !w || !d) return { err: "no-session" };
            const mk = w.__SCVB_MOCK__;
            const hook = w.__SCVB_OUTPUT__;
            if (!mk || !hook || typeof hook.hostEcho !== "function") return { err: "no-hook" };
            const rc = JSON.stringify(mk.setOutputEnabled(false));
            if (/rejected|"ok":\\s*false/.test(rc)) return { err: "reject:" + rc };
            const badge = () => {
                const n = d.querySelector('[data-gb="master-width-hostbadge"]');
                return n ? n.getAttribute("data-on") : "?";
            };
            return new Promise((res) => {
                let playing = true;
                let offAtAge = -1;      // 首次看到徽标灭时的 age
                let ageWhenStopped = -1; // 「明确停走 ∧ age 最大」的那个 age
                let maxAge = 0;
                let samples = 0;
                const flip = w.setInterval(() => {
                    playing = !playing;
                    s.ctl.setTransport({ isPlaying: playing });
                }, 150);
                const step = () => {
                    const dg = hook.hostEcho();
                    const age = w.Date.now() - dg.at;
                    samples++;
                    if (age > maxAge) maxAge = age;
                    if (badge() === "0" && offAtAge < 0) offAtAge = age;
                    if (dg.stopped && age > ageWhenStopped) ageWhenStopped = age;
                    // 收尾门限取 1600(当时贴着播放档留的余量,现照旧用),两侧余量都要留,免得 CI runner
                    // 抖一下就把「慢但合法」判红(本仓记过好几次这种假红)。两侧**不等宽**,
                    // 分开记(#236 复审第二轮订正:上一版写「各留 ~700ms」,上界那半是错的):
                    //   • 上界:[SL-394] 原先是「maxAge < 2500(播放档)」,播放档已删,
                    //     该前置现改为「maxAge > 900(越过停走档)」—— 方向反过来了,
                    //     它要的不再是「别越界」而是「必须越界,否则窗口制实现也绿」;
                    //   • 下界 ageWhenStopped >= 900:[900, 1600] 这 **700ms** 里约 35 个
                    //     样本、跨 4-5 个 150ms 翻转周期,停走样本必然采得到。
                    if (age >= 1600 || samples > 400) {
                        w.clearInterval(flip);
                        s.ctl.setTransport({ isPlaying: true });
                        return res({ offAtAge, ageWhenStopped, maxAge, samples });
                    }
                    w.setTimeout(step, 20);
                };
                step();
            });
        })()`,
            30000,
        );
        log(`  (a) 快速起停采样:${JSON.stringify(jitter)}`);
        check(
            !jitter || !jitter.err,
            `(a) 前置:探针跑起来了(${jitter && jitter.err})`,
        );
        if (jitter && !jitter.err) {
            // 非空绿的两条前置,都是**测出来的**:
            //   ① 采样窗里真的出现过「页面手上是明确停走 ∧ 距最后一次宿主写入 ≥ 停走档」
            //      —— 那一刻正是修前徽标被打掉的时刻;
            //   ② [SL-394] 原先第二条是「整段没越过**播放档 2500** —— 越过了就该熄」。
            //      播放档已删,而闩锁之下越过多久都**不该**熄,所以这条前置反过来:
            //      采样窗必须**远超停走档**,这样任何窗口制实现都会在这一段里熄掉。
            check(
                jitter.ageWhenStopped >= 900,
                `(a) 前置:采样窗里出现过「明确停走 ∧ age ≥ 900ms」的样本(实得 ${jitter.ageWhenStopped}ms)` +
                    ` —— 没有它本条修前修后都绿`,
            );
            check(
                jitter.maxAge > 900,
                `(a) 前置:采样窗越过了停走档 900ms(实得 maxAge ${jitter.maxAge}ms)` +
                    ` —— 退回窗口制的实现必然在此段熄灭`,
            );
            check(
                jitter.offAtAge < 0,
                `(a) ★ 快速起停(150ms 一档翻走带)期间徽标**全程不灭**` +
                    `(实得首次熄灭于 age=${jitter.offAtAge}ms,-1 = 全程没灭;` +
                    `[SL-394] 删除式实证:**拆掉播放期闩锁**那次注入,本格与 ⑩(f) 一起红 ——` +
                    `别再照旧注释把它读成「拆掉去抖」那一次的读数)`,
            );
        }

        // ---- (b) ★ 真停:停走后仍在 900ms 内熄灭(用户已确认对的那一半,别修坏)
        //
        // 刻意让「宿主先停写 600ms、用户后停走」:这一档下熄灭时刻由**去抖窗到期**决定
        // (max(T+500, at+900) = T+500,因为 at ≈ T−600),而停走之后 `scvb.playhead`
        // 逐帧逐字相同被 `samePlayhead` 挡掉、`scvb.conn`/`scvb.groups` 走 emitIfChanged、
        // `scvb.meters` 那条订阅不排 render、打印头已停 ⇒ **只剩 app.js 那几拍定时器**。
        // ★ 删除式(**SL-394 复测实得,旧数已作废**):去掉 app.js 停走边沿那一拍
        //   `setTimeout(requestRender, 去抖+50)` ⇒ 本条读数由 583ms 变成 **7755ms** ⇒ 红
        //   (同一次注入下 (h2)/(h3) 也红)。**旧注释写的 1947ms 是错的** —— 那是
        //   `hostEchoTimerWide`(排在 at+2550 的**播放档**兜底拍)接住的结果,而那个定时器
        //   随 SL-394 一起删了。现在这一拍是停走后**唯一**的熄灭触发,没了它只能等下一次
        //   偶然的 render,于是从「近两秒」变成「近八秒」。降级形态照实说:**不是**「永远
        //   不熄」—— -2(12s 超时)是本条**另一种**红法,不是这一种的实测值。
        const armR2 = await setOutput356(true);
        check(
            typeof armR2 === "string" && !/rejected|"ok":\s*false/.test(armR2),
            `(b) 前置:重新打开打印头被接受(回执 ${armR2})`,
        );
        check(await waitFor(badgeOn356, 15000), "(b) 前置:徽标重新亮起");
        const hardStop = await evaluate(
            `(() => {
            const s = window.__SCVB_PREVIEW__;
            const f = document.querySelector("iframe");
            const w = f && f.contentWindow;
            const d = f && f.contentDocument;
            if (!s || !s.ctl || !w || !d) return { err: "no-session" };
            const mk = w.__SCVB_MOCK__;
            const hook = w.__SCVB_OUTPUT__;
            if (!mk || !hook || typeof hook.hostEcho !== "function") return { err: "no-hook" };
            const badge = () => {
                const n = d.querySelector('[data-gb="master-width-hostbadge"]');
                return n ? n.getAttribute("data-on") : "?";
            };
            return new Promise((res) => {
                // ① 先等一帧**刚到的**宿主写入。为什么不能直接开始:mock 的打印头是
                //    「值变了**或**回声位翻转才发」,两帧之间实测能隔 3 秒多(本文件 ④(g) 捞到过 3362ms)。
                //    上来就掐信号 + 定长等待的话,at 有多旧完全看运气 —— 第一版就是这么
                //    写的,实测 ageAtStop=2513ms(闩锁其实已经走完播放档),量到的 21ms
                //    是「本来就该熄了」而不是「停走后及时熄」。
                // ⚠ [#236 复审] 页内三段预算之和**必须小于外层 evaluate 的 30s CDP 超时**:
                //    10s(waitFresh)+ 0.6s + 12s(step)= 22.6s。超了的话超时从 CDP 那头
                //    抛出去,被文件末尾的 catch 吞成一句「冒烟过程抛错」,
                //    err:"no-fresh-echo" / offAfterStop:-2 这两个**专为诊断留的**返回值
                //    永远看不到 —— 而失败路径正是最需要读得懂的时候。
                //    10s 对「等一帧新写入」有 3 倍余量(本文件 ④(g) 实测最长间隔 3362ms)。
                const t0 = w.Date.now();
                const waitFresh = () => {
                    const dg = hook.hostEcho();
                    if (dg.at > 0 && w.Date.now() - dg.at <= 150) return armed();
                    if (w.Date.now() - t0 > 10000) return res({ err: "no-fresh-echo" });
                    w.setTimeout(waitFresh, 20);
                };
                const armed = () => {
                    // ② 掐掉信号源:此后 at 冻住。走带**照旧在跑**,闩锁按播放档消耗。
                    const rc = JSON.stringify(mk.setOutputEnabled(false));
                    if (/rejected|"ok":\\s*false/.test(rc)) {
                        return res({ err: "reject:" + rc });
                    }
                    // ③ 停写 600ms 之后再停走 —— 这一档下 at ≈ T-650,熄灭时刻由
                    //    **去抖窗到期**(T+500)决定而不是由 at+900(T+250,已过)决定,
                    //    正好是「停走边沿那一拍 render」唯一能被看见的那一段。
                    w.setTimeout(() => {
                        const dg0 = hook.hostEcho();
                        const stopAt = w.Date.now();
                        const ageAtStop = stopAt - dg0.at;
                        s.ctl.setTransport({ isPlaying: false });
                        const step = () => {
                            if (badge() === "0") {
                                // 熄灭那一刻的三个读数一并带出(**只作诊断,不作断言**)。
                                // 为什么加:本机三轮 offAfterStop 实测 630 / 383 / 582ms,
                                // 只看这一个数没法说清偏差从哪来。带上之后当场读到
                                // (第三轮){ageAtOff:1199, sincePlayingAtOff:606,
                                // wideAtOff:false} —— 熄灭发生在「页面收到最后一帧非停走
                                // 载荷」之后 606ms(去抖窗 500 + 一拍 render),而
                                // stopAt(壳页发指令的时刻)比它晚 24ms。也就是说真正的
                                // 锚点是 playingAt 不是 stopAt,两者之间那段延迟
                                // (30Hz 帧间隔 + 壳页 rAF 抖动)页外量不到。
                                // 结论方向:playingAt <= stopAt 恒成立 ⇒ 实际熄灭比
                                // 「停走后 900ms」这条上界更早,下面那格的余量是真的。
                                const dg1 = hook.hostEcho();
                                return res({
                                    offAfterStop: w.Date.now() - stopAt,
                                    ageAtStop,
                                    ageAtOff: w.Date.now() - dg1.at,
                                    sincePlayingAtOff:
                                        w.Date.now() - dg1.playingAt,
                                    wideAtOff: dg1.wide,
                                });
                            }
                            if (w.Date.now() - stopAt > 12000) {
                                return res({ offAfterStop: -2, ageAtStop });
                            }
                            w.setTimeout(step, 20);
                        };
                        step();
                    }, 600);
                };
                waitFresh();
            });
        })()`,
            30000,
        );
        log(`  (b) 真停读数:${JSON.stringify(hardStop)}`);
        check(
            !hardStop || !hardStop.err,
            `(b) 前置:探针跑起来了(${hardStop && hardStop.err})`,
        );
        if (hardStop && !hardStop.err) {
            check(
                hardStop.ageAtStop > 0,
                `(b) 前置:停走那一刻徽标还亮着(距最后一次宿主写入 ${hardStop.ageAtStop}ms)` +
                    ` —— 否则「停走后多久熄」量的是别的东西。` +
                    `[SL-394] 上界「落在播放档 2500ms 内」已去掉:播放期由闩锁托着,` +
                    `距上次写多久都还亮,再设上界就是拿一个不存在的窗口当前提`,
            );
            check(
                hardStop.offAfterStop >= 0 && hardStop.offAfterStop < 900,
                `(b) ★ 真停之后 900ms 内熄灭(实得 ${hardStop.offAfterStop}ms)。` +
                    `丢掉停走边沿那一拍 render 时实测 7755ms(SL-394 复测;旧注释的 1947ms ` +
                    `是已删除的播放档兜底拍接住的数,别再照抄);` +
                    `-2 = 12s 内根本没熄,是另一种红法`,
            );
        }

        assertClean("SL-356 走带态去抖");
    }

    // =========================================================================
    // ⑫ [SL-353] 分布图渲染面:柱是**单色单元素** + Output↔Monitor 两页逐项对拍
    // -------------------------------------------------------------------------
    // 用户实测(v5.6.7)两条:①「竖条最上方一段颜色与其余部分不同,**有的时候**」;
    // ② Monitor 的分布图与 Output 对不上。
    //
    // ★ 先说**定谳**,免得后来人照着卡面的猜测去改取色:
    //   ① 的真因**不是**取色。柱由 `distBarsHtml` 一行拼一个 `.dist-bar`,颜色只有一处
    //      写入(`--tc: var(--track-color-N)`),CSS 侧两档 alpha 换的是同一色相的浓淡。
    //      屏幕上那一段异色是**两轨声像相同、柱体完全重合**:后画的矮柱盖住高柱的下半截,
    //      只在顶端露出高柱的颜色。用户截图逐像素对上了 —— Output 侧轨 7 靛紫
    //      (72,69,143)压在轨 9 橄榄(98,100,16)之上,Monitor 侧轨 4 芥黄压在轨 5 钢蓝
    //      之上;两张图里其余每根柱都是一条干净的同色渐变。「有的时候」= 只在两轨声像
    //      撞到一起的那些时刻。修法因此是**可读性**(1px 分隔晕),不是「改成单一取色」
    //      ——它本来就是单一取色,本节 (a)-(d) 就是把这句话钉住。
    //   ② 的渲染面**早已共用**:两页都走 `web/shared/distribution-chart.js` 的几何与拼串、
    //      `web/shared/dist-motion.js` 的建器(`--zero-h` / `has-zero-line` 也由建器打)。
    //      真正各写一份的是**两页 index.html 里那段 `.dist-bar` / `.dist-plot__zero` CSS**
    //      ——手抄两份、没有任何东西比对过。本节 (e) 补的就是这道:同一批 rows 喂进两页
    //      各自的样式表,柱数 / 每柱矩形 / 0 dB 线矩形 / 计算色逐项对拍。
    //      **数据源的差异不在本节判定面内**(它们在 native 侧,列在 PR 描述里)。
    //
    // ★ 量法:在**每一页自己的文档里**造一个 600×200 的 `.dist-plot` 沙箱,喂同一份
    //   rows、同一个 `--zero-h`,量 `getBoundingClientRect`。为什么不去量两页真实的图:
    //   真实的图各由各的数据面驱动(Output 读 params/state、Monitor 读 viz 段),两边
    //   的 rows 本来就不同,量出来的差分不清「样式漂了」还是「数据不同」。沙箱把数据
    //   这一维钉死,剩下的差**只可能**来自两页的样式表。
    //
    // ★ 本节**证明不了**什么:
    //   · 沙箱里的 `--zero-h` 与 `has-zero-line` 是本节自己打的,不是建器打的 ——
    //     「建器有没有打」由 smoke-monitor.mjs 的接线格与本文件 ⑪ [SL-280] 钉,别读成这里也钉了;
    //   · 1px 分隔晕在无头 Chrome 上量的是**计算样式**(晕在、且无色差),不是「重合处
    //     真的多了一条分隔线」—— 后者要数像素(得解 PNG),留给真机肉眼;
    //   · 两页真实数据面的一致性(轨集 / pan / vol 从哪来)不在这里,见 PR 描述。
    //   · 分隔晕只在**先画的柱更高**那一个朝向上看得见缝(后画的矮柱把它的顶圈
    //     压在高柱的柱面上);反过来后画的柱更高时,矮柱被完全盖住,
    //     屏幕上看不出两轨重合 —— 那不是本卡引入的(DOM 序 = 轨号升序,现状如此),
    //     晕也治不了。真机复现时别把这个朝向读成「修法没生效」。
    //
    // ★ 删除式(本机实测,逐格的实得 FAIL 清单记录在 PR 描述里)。
    //   **单页注入**验的是跨页对拍有牙,**两页同时注入**验的是单页那几格自己有牙 ——
    //   两页一起改的话 (e) 全绿,红的只可能是 (a)-(d);只改一页则 (e) 也跟着红。
    //   · D1 monitor 删掉 `box-shadow: … var(--dist-bar-halo)`  ⇒ (d) + (e5)
    //   · D2 monitor `.dist-bar { bottom: 15px }` → 17px        ⇒ (e2)
    //   · D3 monitor `.dist-plot__zero { bottom: calc(15px + …) }` → 18px ⇒ (e3)
    //   · D4 output  柱体渐变第二档换成 `rgba(var(--track-color-2), .55)` ⇒ (b) + (e5)
    //   · D5 两页    晕改 `0 0 3px 1px`(模糊光晕)            ⇒ 只有 (d)
    //   · D6 两页    晕改 `inset 0 0 0 1px`(晕跑到柱内侧)     ⇒ 只有 (d)
    //   · D7 两页    lead 帽 `top` 退回 `-3px`(帽悬空)        ⇒ 只有 (c)
    //   · D8 tokens.css 把 `--dist-bar-halo` 改成有色差的红      ⇒ 只有 (d)
    //   · D10 两页    lead 帽高改 `1px`(帽又悬空)          ⇒ 只有 (c)
    //   [SL-372] 晕的形态变了(见下),D1/D5/D6 三条的**注入文本**随之改口径:
    //   · D1 = 删掉整条 `box-shadow`(三段一起)              ⇒ (d) + (e5)
    //   · D5 = 把左右两段改成 `0 0 3px 1px`(模糊外扩)       ⇒ 只有 (d)
    //   · D6 = 把三段整体退回旧的 `0 0 0 1px`                ⇒ (d) + ⑬(a)
    //     ——(d) 的那一格是新加的「外扩段上沿不得越过柱顶」,⑬(a) 是同一件事的像素面;
    //     两格**故意重叠**:一格钉规则、一格钉屏幕,规则那格改得动、屏幕那格改不动。
    //   (统筹裁定 2026-09-06,#235 第 1 轮 PR 评论)复审建议的「晕色亮度护栏 + D9」**不做**。
    //     备忘：单断 `r=g=b` 只钉住「无色差」，`rgb(96,96,96)` 这类中灰同样全绿，
    //     却会给**每一根**独立的柱描一圈可见深色圈；当前值是白 55%，离那一带很远。
    // =========================================================================
    {
        // 同一批 rows 喂两页。刻意造出**两轨同声像**(轨 7 / 轨 9,pan 都是 40)——
        // 那正是用户那一幕的形状;顺带覆盖 lead 绿帽、立体声张开线与两端硬边。
        const SL353_ROWS = [
            {
                ch: 1,
                pan: -60,
                volDb: 3,
                widthPct: 100,
                stereo: false,
                lead: false,
            },
            {
                ch: 3,
                pan: -20,
                volDb: -6,
                widthPct: 100,
                stereo: true,
                lead: false,
            },
            {
                ch: 7,
                pan: 40,
                volDb: 6,
                widthPct: 100,
                stereo: false,
                lead: false,
            },
            {
                ch: 9,
                pan: 40,
                volDb: -2,
                widthPct: 100,
                stereo: false,
                lead: false,
            },
            {
                ch: 12,
                pan: 0,
                volDb: 0,
                widthPct: 60,
                stereo: true,
                lead: true,
            },
            {
                ch: 15,
                pan: 100,
                volDb: -24,
                widthPct: 100,
                stereo: false,
                lead: false,
            },
        ];
        // 页内探针。**不用正则**:反斜杠经工具链会被折掉一层,静默失效(本仓有实伤记录),
        // 而这里只需要从 `rgb(…)` / `rgba(…)` 里取前三个分量,indexOf 就够。
        const PROBE = `(async () => {
    const DC = await import("/web/shared/distribution-chart.js");
    const ROWS = ${JSON.stringify(SL353_ROWS)};
    const host = document.createElement("div");
    host.className = "dist-plot has-zero-line";
    host.style.cssText = "position:absolute;left:0;top:0;width:600px;height:200px;";
    host.style.setProperty("--zero-h", DC.zeroDbLinePct() + "%");
    host.innerHTML = '<div class="dist-plot__zero"></div><div class="dist-bars"></div>';
    document.body.appendChild(host);
    host.querySelector(".dist-bars").innerHTML = DC.distBarsHtml(ROWS, 0, 100);
    const hr = host.getBoundingClientRect();
    const r2 = (v) => Math.round(v * 100) / 100;
    const rel = (n) => {
        const r = n.getBoundingClientRect();
        return { x: r2(r.left - hr.left), y: r2(r.top - hr.top), w: r2(r.width), h: r2(r.height) };
    };
    const rgbOf = (s) =>
        String(s == null ? "" : s)
            .split("rgb")
            .slice(1)
            .map((seg) => {
                const i = seg.indexOf("(");
                const j = seg.indexOf(")");
                if (i < 0 || j < 0) return "?";
                return seg.slice(i + 1, j).split(",").slice(0, 3).map((x) => x.trim()).join(",");
            });
    // [SL-372] 把 box-shadow 的计算值拆成段。**不用正则**(理由同上面的 rgbOf:
    // 反斜杠经工具链会被折掉一层,静默失效)。序列化形态恒是
    //   rgba(r, g, b, a) <dx> <dy> <blur> <spread>[ inset][, 下一段…]
    // 按 "rgb" 切开之后每段的几何量就是纯数字串,逗号换空格再按空格切即可。
    // ⚠ 本段落在页内探针的模板串里,**不能出现反引号**(会当场截断整个模板)。
    const segsOf = (s) =>
        String(s == null ? "" : s)
            .split("rgb")
            .slice(1)
            .map((seg) => {
                const i = seg.indexOf("(");
                const j = seg.indexOf(")");
                if (i < 0 || j < 0) return null;
                const rgb = seg.slice(i + 1, j).split(",").slice(0, 3).map((x) => x.trim()).join(",");
                const parts = seg.slice(j + 1).split(",").join(" ").split(" ").filter((x) => x.length > 0);
                const nums = parts.filter((x) => x !== "inset").map((x) => parseFloat(x));
                return {
                    rgb,
                    inset: parts.indexOf("inset") >= 0,
                    dx: nums.length > 0 ? nums[0] : NaN,
                    dy: nums.length > 1 ? nums[1] : NaN,
                    blur: nums.length > 2 ? nums[2] : 0,
                    spread: nums.length > 3 ? nums[3] : 0,
                };
            })
            .filter((g) => g !== null);
    const zeroEl = host.querySelector(".dist-plot__zero");
    const out = {
        zeroH: DC.zeroDbLinePct(),
        zeroDisplay: getComputedStyle(zeroEl).display,
        zero: rel(zeroEl),
        host: { w: r2(hr.width), h: r2(hr.height) },
        halo: String(getComputedStyle(document.documentElement).getPropertyValue("--dist-bar-halo") || "").trim(),
        bars: [],
        spans: [],
    };
    for (const n of host.querySelectorAll(".dist-span")) out.spans.push(rel(n));
    for (const b of host.querySelectorAll(".dist-bar")) {
        const cs = getComputedStyle(b);
        const bf = getComputedStyle(b, "::before");
        const af = getComputedStyle(b, "::after");
        out.bars.push({
            ch: Number(b.getAttribute("data-ch")),
            lead: b.getAttribute("data-lead"),
            rect: rel(b),
            bgRgb: rgbOf(cs.backgroundImage),
            shadow: cs.boxShadow,
            shadowRgb: rgbOf(cs.boxShadow),
            shadowSegs: segsOf(cs.boxShadow),
            radius: cs.borderRadius,
            before: bf.content,
            after: af.content,
            afterTop: af.top,
            afterH: af.height,
            afterBg: af.backgroundImage,
            afterRgb: rgbOf(af.backgroundImage),
        });
    }
    host.remove();
    return out;
})()`;

        const NORM = (s) => String(s).replace(/\s+/g, "");
        const seen = {};
        for (const [name, url] of [
            ["Output", `${base}/web/output/index.html`],
            ["Monitor", `${base}/web/monitor/index.html`],
        ]) {
            newBucket(`sl353-${name}`);
            await cdp.send("Page.navigate", { url: "about:blank" });
            await sleep(120);
            await cdp.send("Page.navigate", { url });
            check(
                await waitFor(
                    `document.readyState === "complete" && !!document.querySelector("#card")`,
                ),
                `(前提)${name} 裸开装载完成(不白屏)`,
            );
            const p = await evaluate(PROBE);
            seen[name] = p;
            // 探针整体拿不到就别往下走:后面每一格都会因为「没有柱」而空绿。
            if (
                !check(
                    !!p && Array.isArray(p.bars),
                    `(前提)${name} 页内探针取到读数`,
                )
            ) {
                continue;
            }
            log(
                `  ${name}:host=${JSON.stringify(p.host)} 柱 ${p.bars.length} 根 ` +
                    `张开线 ${p.spans.length} 条 zero=${JSON.stringify(p.zero)} halo=${p.halo}`,
            );

            // ---- (a) 一行 = 一个 `.dist-bar`(柱不是几个元素叠出来的)
            eq(
                p.bars.length,
                SL353_ROWS.length,
                `(a) ★ ${name}:${SL353_ROWS.length} 行 rows 出 ${SL353_ROWS.length} 根柱,一行一个元素`,
            );
            eq(
                p.bars.map((b) => b.ch),
                SL353_ROWS.map((r) => r.ch),
                `(a) ${name}:柱的轨号与行序逐项对上`,
            );
            eq(
                p.spans.length,
                SL353_ROWS.filter((r) => r.stereo).length,
                `(a) ${name}:立体声行才出张开线`,
            );

            for (const b of p.bars) {
                // ---- (b) 同一根柱的所有色片同色(渐变两档只换浓淡,不换色相)
                check(
                    b.bgRgb.length >= 2 && new Set(b.bgRgb).size === 1,
                    `(b) ★ ${name} 轨 ${b.ch}:柱体渐变的每一档取的是**同一个** rgb` +
                        `(实得 ${JSON.stringify(b.bgRgb)})`,
                );
                eq(
                    b.bgRgb[0],
                    NORM(FALLBACK_TRACK_COLORS[b.ch - 1]),
                    `(b) ${name} 轨 ${b.ch}:那个 rgb 就是本轨的调色板色号`,
                );
                // ---- (c) 柱上没有第二个绘制片段;帽只跟 lead 走
                eq(
                    b.before,
                    "none",
                    `(c) ★ ${name} 轨 ${b.ch}:柱没有 ::before 片段`,
                );
                if (b.lead === "1") {
                    check(
                        b.after !== "none",
                        `(c) ★ ${name} 轨 ${b.ch}(lead):有柱顶帽 —— 语义色,身份色之外唯一的第二片`,
                    );
                    check(
                        b.afterRgb.length > 0 &&
                            b.afterRgb.every((c) => c === "120,176,142"),
                        `(c) ${name} 轨 ${b.ch}(lead):帽色 = --dist-bar-lead 的绿` +
                            `(实得 ${JSON.stringify(b.afterRgb)})`,
                    );
                    // 断的是**不变量**「帽底边贴住柱顶」= `top + height === 0`,
                    // 不是两个字面量:只钉 `top` 的话,`top:-2px; height:1px` 照绿,
                    // 而那 1px 空档会**露出背后的东西** —— 两柱重合时露后一根的轨色、
                    // 没有重合时露图底,lead 轨柱顶恒常读成三段(SL-353 复审第 2 轮
                    // 【重要】治的那一幕)。
                    // ⚠ [SL-372] 这句的**理由换过一次**:SL-353 时那 1px 空档里填的是
                    // 外扩晕的上侧那一格,而 SL-372 已把上侧那格改成 inset、柱顶之上不再
                    // 有晕 —— 结论(帽必须贴住柱顶)没变,别照旧理由复述。
                    // 先断两个量都**取得到**:取不到时 `parseFloat` 得 NaN、
                    // 比较恒 false,那会把下面那一格变成一个**永远红**的格——
                    // 与「永远绿」是同一族的另一半,报错也指不到真因。
                    check(
                        /px$/.test(b.afterTop) && /px$/.test(b.afterH),
                        `(c) ★ ${name} 轨 ${b.ch}(lead):帽的 top / height 两个量都取得到` +
                            `(实得「${b.afterTop}」/「${b.afterH}」)`,
                    );
                    check(
                        parseFloat(b.afterTop) + parseFloat(b.afterH) === 0,
                        `(c) ★ ${name} 轨 ${b.ch}(lead):帽底边贴住柱顶` +
                            `(top + height === 0,实得 ${b.afterTop} + ${b.afterH})—— ` +
                            `悬空的帽会让那一格空档露出背后的东西(两柱重合时是后一根的` +
                            `轨色,没有重合时是图底),lead 轨柱顶恒常读成三段`,
                    );
                } else {
                    eq(
                        b.after,
                        "none",
                        `(c) ★ ${name} 轨 ${b.ch}(非 lead):没有柱顶帽`,
                    );
                }
                // ---- (d) 重叠分隔晕在、无色差,且[SL-372]**外扩的那几段一律不越过柱顶**
                check(
                    b.shadow !== "none" && b.shadow !== "",
                    `(d) ★ ${name} 轨 ${b.ch}:柱描了分隔晕(实得「${b.shadow}」)`,
                );
                // ⚠ 先断「段数不为零」再断「每一段如何」:`every` 在**空数组**上恒真,
                // 少了这一格,晕整条被删掉时下面三格会一起空绿(本文件立过这条纪律)。
                const segs = Array.isArray(b.shadowSegs) ? b.shadowSegs : [];
                const outs = segs.filter((g) => !g.inset);
                const ins = segs.filter((g) => g.inset);
                check(
                    segs.length > 0,
                    `(d) ★ ${name} 轨 ${b.ch}:分隔晕拆得出至少一段(实得 ${JSON.stringify(segs)})`,
                );
                check(
                    segs.length > 0 &&
                        segs.every((g) => new Set(g.rgb.split(",")).size === 1),
                    `(d) ★ ${name} 轨 ${b.ch}:每一段的晕色都无色差(r=g=b)—— 有色差的描边会把` +
                        `「柱顶有一段别的颜色」从偶发变成恒常(实得 ${JSON.stringify(segs.map((g) => g.rgb))})`,
                );
                check(
                    segs.length > 0 &&
                        segs.every((g) => g.blur === 0 && g.spread === 0),
                    `(d) ★ ${name} 轨 ${b.ch}:每一段都是零模糊零外扩的 1px 实边` +
                        `(实得 ${JSON.stringify(segs)})—— 模糊的光晕分不开两根重合的柱`,
                );
                // ★ [SL-372] 本卡的不变量:**外扩段的上沿不得越过柱顶**。
                //   `dy - blur - spread >= 0` 是「阴影矩形的上边缘不高于柱顶」的逐字写法;
                //   旧的 `0 0 0 1px` 在这里得 `0 - 0 - 1 = -1` ⇒ 红。
                //   左右两段本身就是这条不变量的合法解(dy=0、blur=spread=0)。
                check(
                    outs.length === 2,
                    `(d) ★ ${name} 轨 ${b.ch}:外扩晕恰好两段(左右各一,实得 ${outs.length} 段:` +
                        `${JSON.stringify(outs)})`,
                );
                check(
                    outs.length === 2 &&
                        outs.every((g) => g.dy - g.blur - g.spread >= 0),
                    `(d) ★ ${name} 轨 ${b.ch}:没有任何外扩段的上沿越过柱顶` +
                        `(每段 dy - blur - spread >= 0,实得 ${JSON.stringify(outs)})—— ` +
                        `越过柱顶的那 1px 在「背后没有别的柱」时就是画在图底上的白线`,
                );
                check(
                    outs.length === 2 &&
                        outs
                            .map((g) => g.dx)
                            .sort((x, y) => x - y)
                            .join(",") === "-1,1",
                    `(d) ${name} 轨 ${b.ch}:外扩的两段是左右各外扩 1px` +
                        `(实得 ${JSON.stringify(outs.map((g) => g.dx))})`,
                );
                // ★ [SL-372] 柱内那条顶边线:非 lead 柱有且只有一条,lead 柱一条都没有
                //   (lead 柱顶压着 2px 绿帽,再叠一条就读成三段 —— SL-353 治过的那一幕)。
                if (b.lead === "1") {
                    eq(
                        ins.length,
                        0,
                        `(d) ★ ${name} 轨 ${b.ch}(lead):柱内没有浅色顶边线` +
                            `(实得 ${JSON.stringify(ins)})`,
                    );
                } else {
                    check(
                        ins.length === 1 && ins[0].dx === 0 && ins[0].dy === 1,
                        `(d) ★ ${name} 轨 ${b.ch}(非 lead):柱内顶边有且只有一条 1px 浅色线` +
                            `(inset 0 1px 0,实得 ${JSON.stringify(ins)})—— 两根重合时靠它分隔`,
                    );
                }
            }

            // ---- 0 dB 基准线在沙箱里确实显示(下面 (e3) 量的是它的矩形)
            eq(
                p.zeroDisplay,
                "block",
                `(前提)${name}:拿到 has-zero-line 的 0 dB 线是显示的`,
            );
            check(
                p.zero.h > 0 && p.zero.w > 0,
                `(前提)${name}:0 dB 线有非零矩形(实得 ${JSON.stringify(p.zero)})`,
            );

            // ---- 量 token 本身(上面那几格量的是消费端的计算值)
            const haloRgb = String(p.halo).startsWith("rgb")
                ? String(p.halo)
                      .slice(String(p.halo).indexOf("(") + 1)
                      .split(",")
                      .slice(0, 3)
                      .map((x) => x.trim())
                : [];
            check(
                haloRgb.length === 3,
                `(d) ★ ${name}:tokens.css 的 --dist-bar-halo 解得出来(实得「${p.halo}」)` +
                    ` —— token 没了的话 box-shadow 整条声明失效,晕直接不画`,
            );
            check(
                haloRgb.length === 3 && new Set(haloRgb).size === 1,
                `(d) ★ ${name}:--dist-bar-halo 本身无色差(r=g=b,实得 ${JSON.stringify(haloRgb)})`,
            );

            // 裸开两页的页内异常 / console.error 各断各的:循环第二圈的
            // `newBucket` 会盖掉第一圈的记录,放到循环外只看得见 Monitor 那一页。
            assertClean(`SL-353 ${name} 裸开`);
        }

        // ---- (e) 两页逐项对拍
        //
        // 容差取 **0.5px**,比卡面给的 1px 更紧:两页在**同一个浏览器**里跑**同一份沙箱
        // 尺寸**,合法差是 0,0.5 只用来吃浮点表示;留 1px 会正好放过「一页 15px 一页 16px」
        // 这类真漂移 —— 而那恰是两份手抄 CSS 最容易漂出来的量级。
        const A = seen.Output;
        const B = seen.Monitor;
        if (
            check(
                !!A && !!B && Array.isArray(A.bars) && Array.isArray(B.bars),
                "(e) 前提:两页都取到了读数",
            )
        ) {
            const TOL = 0.5;
            const near = (x, y) => Math.abs(x - y) <= TOL;
            const nearRect = (r1, r2) =>
                near(r1.x, r2.x) &&
                near(r1.y, r2.y) &&
                near(r1.w, r2.w) &&
                near(r1.h, r2.h);
            eq(
                B.bars.length,
                A.bars.length,
                "(e1) ★ 同一批 rows 下两页柱数相等",
            );
            eq(B.host, A.host, "(e) 前提:两页沙箱盒尺寸相同(否则百分比不可比)");
            eq(
                B.zeroH,
                A.zeroH,
                "(e) 前提:两页喂的 --zero-h 同值(同一个 zeroDbLinePct())",
            );
            for (let i = 0; i < Math.min(A.bars.length, B.bars.length); i++) {
                const a = A.bars[i];
                const b = B.bars[i];
                eq(b.ch, a.ch, `(e2) 第 ${i + 1} 根柱的轨号两页一致`);
                check(
                    nearRect(a.rect, b.rect),
                    `(e2) ★ 第 ${i + 1} 根柱(轨 ${a.ch})的横位/宽/顶/高两页相等(±${TOL}px)` +
                        `—— Output ${JSON.stringify(a.rect)} / Monitor ${JSON.stringify(b.rect)}`,
                );
                eq(
                    b.bgRgb,
                    a.bgRgb,
                    `(e5) 第 ${i + 1} 根柱(轨 ${a.ch})的计算色两页一致`,
                );
                eq(
                    b.shadow,
                    a.shadow,
                    `(e5) ★ 第 ${i + 1} 根柱(轨 ${a.ch})的分隔晕两页一致`,
                );
                eq(
                    b.radius,
                    a.radius,
                    `(e5) 第 ${i + 1} 根柱(轨 ${a.ch})的圆角两页一致`,
                );
                eq(
                    b.afterBg,
                    a.afterBg,
                    `(e5) 第 ${i + 1} 根柱(轨 ${a.ch})的柱顶帽两页一致`,
                );
                eq(
                    b.afterTop,
                    a.afterTop,
                    `(e5) 第 ${i + 1} 根柱(轨 ${a.ch})的柱顶帽纵位两页一致`,
                );
                eq(
                    b.afterH,
                    a.afterH,
                    `(e5) 第 ${i + 1} 根柱(轨 ${a.ch})的柱顶帽高两页一致`,
                );
            }
            check(
                nearRect(A.zero, B.zero),
                `(e3) ★ 0 dB 基准线的矩形两页相等(±${TOL}px)—— ` +
                    `Output ${JSON.stringify(A.zero)} / Monitor ${JSON.stringify(B.zero)}`,
            );
            eq(B.spans.length, A.spans.length, "(e4) 张开线条数两页相等");
            for (let i = 0; i < Math.min(A.spans.length, B.spans.length); i++) {
                check(
                    nearRect(A.spans[i], B.spans[i]),
                    `(e4) ★ 第 ${i + 1} 条张开线的矩形两页相等(±${TOL}px)—— ` +
                        `Output ${JSON.stringify(A.spans[i])} / Monitor ${JSON.stringify(B.spans[i])}`,
                );
            }
        }
    }

    // =========================================================================
    // ⑬ [SL-372] **柱顶上方那一行像素**(两页各一遍;像素面,不是计算样式面)
    // -------------------------------------------------------------------------
    // 用户 v5.6.8 真机原话:「好像有的时候白线会出现在柱子的上方,一般情况下没有,
    // 可能在上限很接近的时候会出现」。定谳(截图逐像素算出来的,记在 PR 描述里):
    // 那条线 = [SL-353] 那圈**外扩** 1px 分隔晕的**上侧那一格**。它落在柱顶之上,
    // 背后有另一根柱时它是分隔线(SL-353 要的),背后没柱时它就是画在图底上的一条
    // 1px 浅色短线 —— 每一根柱头上都有一条。修法见两页 index.html 的 `.dist-bar`:
    // 上侧那格改成 inset(落进柱内的第一行),左右两格照旧外扩。
    //
    // ★ 为什么这一节要解 PNG:⑫(d) 断的是**规则**(阴影的段与几何),而「屏幕上那一行
    //   到底有没有浅色像素」是**渲染结果**。两者可以同时为真也可以背离 —— 比如 lead 帽、
    //   0 dB 线、将来某个 ::before 都能在柱顶之上留下浅色,而它们一个都不在 (d) 的判定面里。
    //   本节故意与 (d) **重叠**:(d) 改得动(改一行 CSS 就能让它绿),本节改不动。
    //
    // ★ 沙箱:一块**不透明黑底**的浮层 + 600x200 的 `.dist-plot`。不透明是必需的 ——
    //   `.dist-plot` 的底色是半透明白(--w-10),压在真实页面上时图底逐像素随背后内容变,
    //   「与图底一致」这件事就无从断起。黑底之下图底恒为同一个值。
    //   本节**不打** `has-zero-line`:0 dB 基准线是一条通栏浅色横线,它会横穿被测行,
    //   把「柱顶上方那一行干不干净」和「那一行上有没有基准线」混成一件事(基准线归 ⑪/⑫)。
    //
    // ★ 用 +12 dB(= `BAR_H_MAX_PCT` 88%)当「贴上沿」的那一根,不是卡面字面写的
    //   「柱高 100%」:柱高的**上限就是 88%**(distribution-chart.js 的 BAR_H_MAX_PCT),
    //   100% 在产品里到不了;而真把 `--h` 写成 100% 时柱顶会被 `.dist-plot { overflow: hidden }`
    //   剪掉、图内根本不存在「柱顶上方那一行」,那一格测的是空气。下面 (前提2) 显式断
    //   「这根柱的柱顶离图内上沿还有 >= 2px」,免得哪天版式一改就悄悄退化成测空气。
    //
    // ★ 删除式(本机实测,实得清单在 PR 描述里):
    //   · DA 两页 `.dist-bar` 的 box-shadow 整体退回 `0 0 0 1px var(--dist-bar-halo)`
    //        并删掉 `.dist-bar[data-lead="1"]` 那条覆盖  ⇒ (a1)(a2) 红 + ⑫(d) 红,(b)(c) 绿
    //     ⚠ 这一格是**改过一版**才有牙的:第一版不对齐柱高、(b) 又钉死了分隔线的侧别,
    //       DA 实测只红了 (a1)(b) —— (a2)(满格那根,top=29.766)照绿,因为浏览器把那条
    //       外扩晕吸附到了 floor(top) 那一行、被测的 floor(top)-1 仍是干净图底;
    //       而 (b) 红是假红(分隔线还在,只是在分界上侧)。两处都已改掉,别退回去。
    //   · DB 两页只删 `inset 0 1px 0 var(--dist-bar-halo)` 那一段(左右两段留着)
    //        ⇒ (b) 红 ×2 + ⑫(d)「非 lead 柱柱内有且只有一条 inset」红 ×10,(a)(c) 绿
    //   · DC 两页删掉 `.dist-bar[data-lead="1"]` 那条覆盖(lead 柱也吃到 inset)
    //        ⇒ (c) 红 ×2 + ⑫(d)「lead 柱没有 inset」红 ×2,(a)(b) 绿
    //   DB / DC 都是「规则那格 + 像素那格」一起红:两格本就是同一件事的两面
    //   (见上面「本节故意与 (d) 重叠」),不是漏网。
    // =========================================================================
    {
        const PX_ROWS = [
            {
                ch: 3,
                pan: -60,
                volDb: 3,
                widthPct: 0,
                stereo: false,
                lead: false,
            },
            {
                ch: 5,
                pan: -30,
                volDb: 12,
                widthPct: 0,
                stereo: false,
                lead: false,
            },
            {
                ch: 7,
                pan: 0,
                volDb: 6,
                widthPct: 0,
                stereo: false,
                lead: false,
            },
            {
                ch: 9,
                pan: 0,
                volDb: -2,
                widthPct: 0,
                stereo: false,
                lead: false,
            },
            {
                ch: 12,
                pan: 60,
                volDb: 3,
                widthPct: 0,
                stereo: false,
                lead: true,
            },
        ];
        const WRAP_W = 640;
        const WRAP_H = 240;
        const PAD = 20;
        // 图底取样列:20(浮层内边距)+ 40。四根柱分别落在 x≈140 / 230 / 320 / 500,
        // 离它最近的一根也有 80px,取不到任何柱或它的外扩晕。
        const GROUND_X = PAD + 40;
        // 页内探针用**字符串拼接**而不是模板串:本文件的模板串里已经有一层 ${},
        // 再嵌一层容易把页内的 ${} 当成 node 侧的插值(静默取到 undefined)。
        const PX_PROBE =
            "(async () => {" +
            '  const DC = await import("/web/shared/distribution-chart.js");' +
            "  window.scrollTo(0, 0);" +
            "  const ROWS = " +
            JSON.stringify(PX_ROWS) +
            ";" +
            '  const wrap = document.createElement("div");' +
            '  wrap.id = "sl372-px-sandbox";' +
            '  wrap.style.cssText = "position:fixed;left:0;top:0;width:' +
            WRAP_W +
            "px;height:" +
            WRAP_H +
            "px;background:#000;padding:" +
            PAD +
            'px;box-sizing:border-box;z-index:99999;";' +
            '  const host = document.createElement("div");' +
            '  host.className = "dist-plot";' +
            '  host.style.cssText = "position:relative;width:600px;height:200px;";' +
            '  host.innerHTML = String.fromCharCode(60) + "div class=" + JSON.stringify("dist-bars") + String.fromCharCode(62) + String.fromCharCode(60) + "/div" + String.fromCharCode(62);' +
            "  wrap.appendChild(host);" +
            "  document.body.appendChild(wrap);" +
            '  host.querySelector(".dist-bars").innerHTML = DC.distBarsHtml(ROWS, 0, 100);' +
            "  const r2 = (v) => Math.round(v * 1000) / 1000;" +
            "  const hr = host.getBoundingClientRect();" +
            // 把每根柱的**柱顶就近对齐到整像素**(改的是 height,柱底不动):
            // 柱高是带两位小数的百分比,柱顶因此落在亚像素上,而浏览器会把 box-shadow
            // 按自己的规则吸附到整行 —— 同一条晕,柱顶 .375 时落在上一行、柱顶 .766 时
            // 落在下一行。不对齐的话「柱顶上方那一行」这句话本身是有歧义的:本机实测
            // 满格那根(top=29.766)在**没修**的版本上,floor(top)-1 那行仍是干净图底,
            // 于是那一格拿不到删除式(而普通高度那根 top=69.375 就红了)。
            // 位移上界 0.5px,不改变任何一根柱的档位;对齐后下面显式断柱顶是整数,
            // 断不住就整节判负,免得又悄悄退回「测的是哪一行说不清」。
            '  for (const b of host.querySelectorAll(".dist-bar")) {' +
            "    const r0 = b.getBoundingClientRect();" +
            '    b.style.height = (r0.bottom - Math.round(r0.top)) + "px";' +
            "  }" +
            "  const out = { plotTop: r2(hr.top), plotBottom: r2(hr.bottom), bars: {} };" +
            '  for (const b of host.querySelectorAll(".dist-bar")) {' +
            "    const r = b.getBoundingClientRect();" +
            '    out.bars[b.getAttribute("data-ch")] = { left: r2(r.left), right: r2(r.right), top: r2(r.top) };' +
            "  }" +
            "  return out;" +
            "})()";

        for (const [name, url] of [
            ["Output", `${base}/web/output/index.html`],
            ["Monitor", `${base}/web/monitor/index.html`],
        ]) {
            newBucket(`sl372-px-${name}`);
            await cdp.send("Page.navigate", { url: "about:blank" });
            await sleep(120);
            await cdp.send("Page.navigate", { url });
            check(
                await waitFor(
                    `document.readyState === "complete" && !!document.querySelector("#card")`,
                ),
                `(前提)${name} 裸开装载完成(不白屏)`,
            );
            // 撤沙箱:浮层带 z-index:99999,留在页上会盖住后面任何一格。
            // 抽成函数是因为**下面有两条早退**(探针没建起来 / 截图解不开),
            // 早退那条第一版忘了撤(复审点名)—— 眼下本节是文件最末一节、下一轮
            // `Page.navigate` 会把它冲掉,所以无实害,但下一个往后面加 ⑭ 的人会踩到。
            const dropSandbox = async () => {
                try {
                    await evaluate(
                        'const n = document.getElementById("sl372-px-sandbox"); if (n) n.remove(); 1',
                    );
                } catch {}
            };
            const g = await evaluate(PX_PROBE);
            if (
                !check(
                    !!g &&
                        !!g.bars &&
                        Object.keys(g.bars).length === PX_ROWS.length,
                    `(前提)${name} 像素沙箱建起来了(应有 ${PX_ROWS.length} 根柱,实得 ${
                        g && g.bars ? Object.keys(g.bars).length : "null"
                    })`,
                )
            ) {
                await dropSandbox();
                continue;
            }
            const shot = await cdp.send("Page.captureScreenshot", {
                format: "png",
                clip: { x: 0, y: 0, width: WRAP_W, height: WRAP_H, scale: 1 },
                captureBeyondViewport: true,
            });
            await dropSandbox();
            let img = null;
            try {
                img = decodePng(Buffer.from(shot.data, "base64"));
            } catch (e) {
                check(false, `(前提)${name} 截图解得开:${e.message}`);
            }
            if (!img) continue;
            check(
                img.w === WRAP_W && img.h === WRAP_H,
                `(前提)${name} 截图尺寸 = 沙箱尺寸(实得 ${img.w}x${img.h},应为 ${WRAP_W}x${WRAP_H})`,
            );
            log(
                `  ${name}(像素):plotTop=${g.plotTop} bars=${JSON.stringify(g.bars)}`,
            );

            // ---- (a) 柱顶上方那一行:逐像素与图底一致 ----------------------
            // 取 `Math.floor(top) - 1`:柱顶落在亚像素上时,含柱顶的那一行(floor(top))
            // 本来就会掺进柱体的抗锯齿,断它等于断浏览器的取整策略;而 floor(top)-1 这一行
            // **整行都在柱顶之上**,修好之后必须与图底逐分量相同。
            // 修前:外扩晕占 [top-1, top),正好压在这一行上 ⇒ 这一格红。
            for (const [key, ch] of [
                ["a1", 3],
                ["a2", 5],
            ]) {
                const b = g.bars[String(ch)];
                const row = Math.floor(b.top) - 1;
                const x0 = Math.floor(b.left) - 3;
                const x1 = Math.ceil(b.right) + 3;
                const ground = img.px(GROUND_X, row);
                let worst = 0;
                let worstX = x0;
                for (let x = x0; x <= x1; x++) {
                    const d = dmax(img.px(x, row), ground);
                    if (d > worst) {
                        worst = d;
                        worstX = x;
                    }
                }
                const cx = Math.round((b.left + b.right) / 2);
                // (前提0)柱顶确实被对齐到了整像素 —— 没对齐时「上方那一行」指哪一行
                // 取决于浏览器怎么吸附阴影,这一格的删除式会时灵时不灵(实测过)。
                check(
                    Number.isInteger(b.top),
                    `(${key} 前提0)★ ${name} 轨 ${ch}:柱顶已对齐到整像素(实得 ${b.top})`,
                );
                // (前提1)这根柱真的画出来了 —— 不然「上方那一行干净」是空绿。
                check(
                    dmax(img.px(cx, Math.round(b.top) + 4), ground) > 40,
                    `(${key} 前提1)★ ${name} 轨 ${ch}:柱体确实画出来了` +
                        `(柱内取样 ${JSON.stringify(
                            img.px(cx, Math.round(b.top) + 4),
                        )} 与图底 ${JSON.stringify(ground)} 差 > 40)`,
                );
                // (前提2)柱顶离图内上沿还有余量 —— 顶到上沿会被 overflow 剪掉,
                // 那时「上方那一行」根本不在图内,这一格就成了测空气。
                check(
                    b.top - g.plotTop >= 2,
                    `(${key} 前提2)★ ${name} 轨 ${ch}:柱顶离图内上沿 >= 2px` +
                        `(实得 ${Math.round((b.top - g.plotTop) * 100) / 100}px)`,
                );
                check(
                    worst <= 6,
                    `(${key}) ★ ${name} 轨 ${ch}(${
                        ch === 5 ? "+12 dB 满格" : "普通高度"
                    }):柱顶上方那一行(y=${row})逐像素 = 图底,没有浅色` +
                        `(最大分量差 ${worst} @ x=${worstX},实得 ${JSON.stringify(
                            img.px(worstX, row),
                        )} vs 图底 ${JSON.stringify(ground)})—— ` +
                        `这一行有浅色就是用户看到的「柱子上方的白线」`,
                );
            }

            // ---- (b) 两柱重合处仍有浅色分隔线(SL-353 要的那条,别修没了)----
            {
                const rear = g.bars["7"];
                const front = g.bars["9"];
                const cx = Math.round((front.left + front.right) / 2);
                const seamRow = Math.round(front.top);
                const rearBody = img.px(cx, seamRow - 3);
                const frontBody = img.px(cx, seamRow + 3);
                // 扫**分界前后三行**取最亮的一行。判据是「分界处有一条浅色线」,
                // 不是「它在分界的哪一侧」—— 本卡做的正是把它从上侧挪到下侧,
                // 钉死侧别的话这一格会把本卡自己的修法判成红(而且旧版式退回来时
                // 它也会红,于是这一格分不清「分隔线没了」和「分隔线换了侧」)。
                let seam = 0;
                for (const r of [seamRow - 1, seamRow, seamRow + 1]) {
                    seam = Math.max(seam, luma(img.px(cx, r)));
                }
                const floorL = Math.max(luma(rearBody), luma(frontBody));
                // 前提:两根柱真的重合(同一个 x)且后画的那根更矮 —— 不重合的话
                // 「分界那一行」不存在,下面那一格就没有被测对象。
                check(
                    Math.abs(rear.left - front.left) < 0.5 &&
                        front.top > rear.top + 6,
                    `(b 前提)★ ${name}:轨 7 / 轨 9 同声像完全重合、且后画的轨 9 更矮` +
                        `(实得 left ${rear.left} vs ${front.left},top ${rear.top} vs ${front.top})`,
                );
                check(
                    seam >= floorL + 25,
                    `(b) ★ ${name}:两柱重合处(y=${seamRow}±1)仍有一条浅色分隔线 —— ` +
                        `分界行亮度 ${Math.round(seam)} 应比上下两侧柱色(后 ${Math.round(
                            luma(rearBody),
                        )} / 前 ${Math.round(
                            luma(frontBody),
                        )})高出 >= 25;这是 [SL-353] 治「柱顶像换了个颜色」的那条线,` +
                        `本卡只把它从分界上方 1px 挪到下方 1px,不许挪没了`,
                );
            }

            // ---- (c) lead 柱顶之下没有浅色带(帽 / 浅色带 / 轨色 三段不许回来)----
            {
                const b = g.bars["12"];
                const cx = Math.round((b.left + b.right) / 2);
                // ⚠ 顺序要紧:**先断对齐、再取样**。第一版把 `img.px()` 写在
                // 「柱顶已对齐到整像素」那一格**之前**,于是对齐一旦失效就是这样一条链:
                //   小数 y → `(y * w + x) * ch` 得小数下标 → `out[i]` 是 `undefined`
                //   → `dmax` 得 NaN → 下面 `if (d > worst)` 恒假 → `worst` 停在 0
                //   → `check(worst <= 12, …)` **判绿**。
                // 主格空绿 + 前提红,正是本文件在 ⑫(d) 那里专门先断 `segs.length > 0`
                // 才 `every` 要避的同一族形态(复审第 2 轮点名)。
                const top = Math.round(b.top);
                check(
                    Number.isInteger(b.top),
                    `(c 前提0)★ ${name} 轨 12:柱顶已对齐到整像素(实得 ${b.top})`,
                );
                const capRow = top - 1;
                const cap = img.px(cx, capRow);
                const body = img.px(cx, top + 5);
                // (c 前提1)取样点都落在图内、且取回来的是三个有限数 —— 不断这一条的话,
                // 「什么都没量到」与「量到了、没有浅色带」在下面那一格里长得一模一样。
                const inImg = (x, y) =>
                    Number.isInteger(x) &&
                    Number.isInteger(y) &&
                    x >= 0 &&
                    y >= 0 &&
                    x < img.w &&
                    y < img.h;
                const realPx = (c) =>
                    Array.isArray(c) &&
                    c.length === 3 &&
                    c.every(Number.isFinite);
                check(
                    [capRow, top, top + 1, top + 5].every((r) =>
                        inImg(cx, r),
                    ) &&
                        realPx(cap) &&
                        realPx(body),
                    `(c 前提1)★ ${name} 轨 12:四个取样点都在图内且取回三个有限数` +
                        `(cx=${cx} rows=${JSON.stringify([capRow, top, top + 1, top + 5])} ` +
                        `图 ${img.w}x${img.h};帽 ${JSON.stringify(cap)} 柱色 ${JSON.stringify(body)})`,
                );
                // 前提:柱顶之上确实是那道绿帽(绿分量明显高过红蓝)。
                check(
                    cap[1] - cap[0] >= 15 && cap[1] - cap[2] >= 15,
                    `(c 前提)★ ${name} 轨 12:柱顶之上是 lead 绿帽` +
                        `(y=${capRow} 实得 ${JSON.stringify(cap)})`,
                );
                let worst = 0;
                let worstRow = capRow;
                let sampled = 0;
                for (const r of [top, top + 1]) {
                    const d = dmax(img.px(cx, r), body);
                    // 逐格断 `Number.isFinite(d)`:NaN 在 `d > worst` 下恒假,
                    // 不显式接住就会被读成「这一行没有色差」。
                    if (Number.isFinite(d)) sampled += 1;
                    if (d > worst) {
                        worst = d;
                        worstRow = r;
                    }
                }
                eq(
                    sampled,
                    2,
                    `(c 前提2)★ ${name} 轨 12:绿帽底下那两行都真的量到了色差数` +
                        `(不是 NaN;NaN 在 \`d > worst\` 下恒假,会把主格顶成空绿)`,
                );
                check(
                    worst <= 12,
                    `(c) ★ ${name} 轨 12(lead):绿帽底下紧挨着的两行仍是轨色,没有浅色带` +
                        `(最大分量差 ${worst} @ y=${worstRow},实得 ${JSON.stringify(
                            img.px(cx, worstRow),
                        )} vs 柱色 ${JSON.stringify(body)})—— ` +
                        `浅色带回来 = 「绿帽 / 浅色带 / 轨色」三段,SL-353 治过的那一幕`,
                );
            }

            assertClean(`SL-372 ${name} 像素沙箱`);
        }
    }

    // ---- ⑫ [SL-400] 起播 chase 写同值 + hostEcho ⇒ 徽标在**播放中**亮起 -------------
    // 用户 A23:正常播放时「宿主自动化正在写」的小图标不出现,停止那一下才亮 1 秒。
    // 机理(native):`OutputEditor::emitParams` 的 diff 门只看 `values` —— 宿主 chase 写进去的
    // 值与当前**相同** ⇒ 这一帧连载荷都不构 ⇒ 页面永远收不到 `hostEcho:true`;而停止那一下
    // 段值真的变了,于是走的是「有变化」那条路,徽标才亮。修法见 `BridgeArgs.h` 的
    // `planParamsFrame()`(值变了 **或** 回声位翻转就该发)。
    //
    // 这一格断的是**页面看得见的那半**:起播之后必须真有一帧「值一个都没变 + hostEcho:true」
    // 到达,并且徽标当场亮起 —— 而不是等某个段边界(值变了**或**回声位翻转才发)才亮。
    // mock 侧同形:`printedParamsDiff()` 的判据与 native 同一条(见那处注释)。
    // 删除式 D5:把它的判据改回只看 `values` ⇒ 本格红(那一帧永远不来)。
    newBucket("sl400-hostecho-chase");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?scenario=curve-editor&play=1`,
    });
    check(await waitFor(READY), "⑫ 页面装载并吃到首帧");
    check(
        await evaluate(
            IN(`
            if (!w.__SCVB_PARAMS_PUSH__) {
                w.__SCVB_PARAMS_PUSH__ = [];
                w.__SCVB_MOCK__.addEventListener("scvb.params", (p) => {
                    w.__SCVB_PARAMS_PUSH__.push({
                        hostEcho: !!(p && p.hostEcho),
                        nValues: Object.keys((p && p.values) || {}).length,
                        at: Date.now(),
                    });
                });
            }
            w.__SCVB_PARAMS_PUSH__ = [];
            w.__SCVB_PARAMS_ARMED_AT__ = Date.now();
            return true;
        `),
        ),
        "⑫ params 帧探针装上",
    );
    {
        const setOut = await evaluate(
            IN(`const mk = w.__SCVB_MOCK__;
                if (!mk) return "no-mock";
                return JSON.stringify(mk.setOutputEnabled(true));`),
        );
        check(
            typeof setOut === "string" && !setOut.includes("no-mock"),
            `⑫ 打开打印头(引擎权威)被受理(实得 ${JSON.stringify(setOut)})`,
        );
    }
    // 先等**稳态**:安静 ≥700ms ⇒ 参数面与当前播放位置的段值已经对上。这样下面那一帧
    // 「值一个都没变」是**夹具保证**的,不是碰运气 —— 第一版拿 `sleep(1200)` 赌这一刻,
    // gate 3e 的并发负载下赌输了(打印头还没把值写进参数面就停了,起播那一帧带着值变化,
    // 判据于是红在一个与本条无关的原因上)。
    check(
        await waitFor(
            IN(`
            const p = w.__SCVB_PARAMS_PUSH__ || [];
            const last = p.length ? p[p.length - 1].at : w.__SCVB_PARAMS_ARMED_AT__;
            return !!last && Date.now() - last > 700;
        `),
            8000,
        ),
        "⑫ 前置:打印头进入稳态(≥700ms 没有新帧 ⇒ 参数面与当前段值对齐)",
    );
    // 停一下:mock 在非打印态把回声位复位(与 native 那 600ms 新鲜窗同一条语义)。
    await setPlaying(false);
    await sleep(400);
    // [R0-5] 再加一道前提:**播放头距「下一次值变化」≥0.5s**。
    // 稳态那一格断的是「此刻参数面与**当前**位置的段值对齐」,但起播之后播放头会往前走 ——
    // 若这期间跨过一个「进入新段且值不同」的点,参数面值真的会变,于是「起播后第一帧 values
    // 为空」不成立,判据红在一个与 SL-400 无关的原因上(第一版 ⑫ 就是在 gate 3e 的并发负载下
    // 这么红的)。处置:暂停态下读走带位置与各轨段表,不够干净就挪一小步再测,上界 10 次;
    // 超了红在前提格。段表取 `ctl.model.segByCh`(与 smoke-output-stale-page 同一条读法),
    // 不用 `__SCVB_MOCK__` —— 后者在 iframe 里,而走带是壳页的东西(`setPlaying` 也是壳页)。
    //
    // ⚠ **判据不能钉在「段边界」上**(第一版实测的教训,两个数都是本机量的):
    //   · 15 条轨的边界合起来是**密**的(该 fixture 下 300s 里 1106 个边界,平均间距 0.27s),
    //     「任一轨的下一段边界 ≥0.5s」的位置占比只有 **0.194** —— 第一版按它判,本机实跑
    //     十步之后 gap 仍是 0.206,前提格当场红(那就是第一版的实际失败形态);
    //   · 而真正决定「起播那一帧带不带值」的是 **`printedParamsDiff` 的判据**(进入新段时
    //     `pan`/`volDb` 与参数面现值不同)—— 相邻两段值相同的边界跨过去**不会**产生帧。
    //     同一把尺子量出来是 0.409。所以这里按**值变化点**算,不按边界算。
    // 后视 0.2s 那一半:停走前最后一次 25Hz 同步(≤40ms)之后跨过的变化会让参数面留着旧值,
    // 起播那一帧照样带出来。目标位取「某个值变化点 + 0.21s」(后视窗刚好走完)。
    const SAFE_SPOT = `(() => {
        const s = window.__SCVB_PREVIEW__;
        if (!s || !s.ctl || !s.ctl.model || !s.ctl.model.segByCh) return null;
        const tracks = [];
        let dur = 0;
        for (const entry of s.ctl.model.segByCh.values()) {
            const segs = (entry.segments || []).slice().sort((a, b) => a.t0S - b.t0S);
            if (segs.length) tracks.push(segs);
            for (const x of segs) if (x.t1S > dur) dur = x.t1S;
        }
        if (!tracks.length || !(dur > 1)) return null;
        const segAt = (a, t) => {
            for (const x of a) if (t >= x.t0S && t < x.t1S) return x;
            return null;
        };
        // 值变化点 = 某轨的段起点,且它与该轨在这一刻之前的值不同(没有上一段 ⇒ 也算变:
        // 参数面留着的是更早那一段的值)。
        const risky = [];
        for (const segs of tracks) {
            for (const x of segs) {
                const prev = segAt(segs, x.t0S - 0.001);
                if (!prev || prev.pan !== x.pan || prev.volDb !== x.volDb) risky.push(x.t0S);
            }
        }
        risky.sort((a, b) => a - b);
        const firstIn = (a, b) => {
            for (const v of risky) if (v > a && v < b) return v;
            return Infinity;
        };
        const safeAt = (t) => firstIn(t - 0.2, t + 0.5) === Infinity;
        const tS = s.ctl.model.transport.timeS;
        const next = firstIn(tS, Infinity);
        // ⚠ 返回值必须是**有限数**:CDP 的 returnByValue 序列化遇到 Infinity 会报
        // 「couldn't be returned by value」而把整次 evaluate 打红。-1 = 「之后再也没有值变化」。
        const gap = next === Infinity ? -1 : next - tS;
        if (safeAt(tS)) return { tS, gap, safe: true, advance: 0 };
        // 前视窗本来就干净、只是被后视窗判死(刚跨过一个变化点)⇒ 往前挪一点点就够。
        if (firstIn(tS, tS + 0.5) === Infinity) return { tS, gap, safe: false, advance: 0.25 };
        for (const v of risky) {
            const c = v + 0.21;
            if (c <= tS + 0.01 || c > dur - 0.6) continue;
            if (safeAt(c)) return { tS, gap, safe: false, advance: Math.min(2.5, c - tS) };
        }
        // 时间线尾段找不到:继续往前挪,走带会在 durationS 处回绕(驱动自己的 wrap 语义)。
        return { tS, gap, safe: false, advance: 2.5 };
    })()`;
    let gapNow = NaN;
    let gapOk = false;
    let moves = 0;
    for (let i = 0; i < 10; i++) {
        const st = await evaluate(SAFE_SPOT);
        if (!st) break;
        gapNow = st.gap;
        if (st.safe) {
            gapOk = true;
            break;
        }
        const adv =
            Number.isFinite(st.advance) && st.advance > 0 ? st.advance : 2.5;
        moves++;
        // 走带**没有 seek 口**:驱动的 `tS` 是它自己的局部量,`ctl.setTransport` 只是收
        // 「宿主这一帧的位置」,下一帧就被它盖回去 —— 所以挪位只能靠**真起播再停**。
        await setPlaying(true);
        await sleep(Math.round((adv + 0.05) * 1000));
        await setPlaying(false);
        await sleep(160);
    }
    check(
        gapOk,
        `⑫ 前置:播放头距下一次**值变化** ≥0.5s(起播那一帧的 values 才保证为空;` +
            `实得 gap=${gapNow === -1 ? "∞(之后不再变)" : gapNow},挪位 ${moves} 次,上界 10)`,
    );
    await evaluate(IN(`w.__SCVB_PARAMS_PUSH__ = []; return true;`));
    await setPlaying(true);
    const BADGE400 = `d.querySelector('[data-gb="master-width-hostbadge"]')`;
    const badgeOn400 = IN(`
        const n = ${BADGE400};
        return !!n && n.getAttribute("data-on") === "1";
    `);
    check(
        await waitFor(
            IN(
                `return (w.__SCVB_PARAMS_PUSH__ || []).some((p) => p.hostEcho && p.nValues === 0);`,
            ),
            6000,
        ),
        "⑫ ★ 起播 chase:真有一帧「值一个都没变 + hostEcho:true」到达" +
            "(少了它,徽标只能等某个段边界值变了才亮)",
    );
    // [R0-5] 单独一格:**起播后第一帧** `values` 为空。
    // 与上面那格不是同一件事 —— 上面那格只要**某一帧**是「值没变 + 回声亮」就行(晚到的
    // 那一帧也算),这一格钉的是「起播那一刻值面本来就没有变化」,也就是上面那道
    // 「距下一段边界 ≥0.5s」前提真的兑现了。少了它,边界前提被拆掉时这一格就是唯一的哨兵。
    check(
        await waitFor(
            IN(`return (w.__SCVB_PARAMS_PUSH__ || []).length > 0;`),
            6000,
        ),
        "⑫ 前提:起播后真有 params 帧到达(否则下一格无从判断)",
    );
    {
        const firstFrame = await evaluate(
            IN(
                `const p = w.__SCVB_PARAMS_PUSH__ || []; return p.length ? p[0] : null;`,
            ),
        );
        check(
            firstFrame !== null && firstFrame.nValues === 0,
            `⑫ 起播后**第一帧** \`values\` 为空(距下一段边界 ≥0.5s ⇒ 那一刻值没变;` +
                `实得 ${JSON.stringify(firstFrame)})`,
        );
    }
    check(
        await waitFor(badgeOn400, 4000),
        "⑫ ★ 徽标在**播放中**亮起(不是停止那一下才亮)",
    );
    assertClean("SL-400 起播 chase 徽标");
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
