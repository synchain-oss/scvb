// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output —— [J146] 拖动 VAD 阈值滑杆 **松手之前** 就看得见预览,**页面级**冒烟(无头 Chrome + CDP)
// -----------------------------------------------------------------------------
// 用户要的是「拖的时候就看到效果」(J146:「1和2现在都做代码功能」;契约 §1.18 拖动档)。这件事
// 只有页面级量得到:node 侧那套(smoke-vad-preview.mjs)断的是纯函数、mock 生命周期与 C++
// 接线的源码钉子 —— 事件到了页面有没有人订阅、虚影有没有画上泳道、VAD 标注带有没有真的重画,
// 三步都在它够不着的地方(「三层机检全绿、窗口是白的」本仓栽过三次)。
//
// 全程走**真鼠标**(CDP `Input.dispatchMouseEvent`,不是页内 new PointerEvent):按下滑杆 →
// 拖到另一头 → **不松手**先断三件事 → 松手再断收尾。
//   (a) 预览事件到了页面:诊断快照 `vadPreview.active` 为真、seq 前进;
//   (b) 虚影画上了泳道:动态层这一帧画的预览边界数 > 0;
//   (c) VAD 标注带**真的重画了**:量泳道 1 静态 canvas 顶部那条 3px 带里的绿色像素数,
//       与按下之前不同(门限拉高 ⇒ 绿带变窄 ⇒ 像素变少)—— 量的是**画出来的像素**,不是数据;
//   (d) 松手 ⇒ 松手档那一趟落地 ⇒ 预览收尾:1s 内 `active` 归假、虚影归零
//       (空闲收尾要 1.5s,1s 内归零只能是「落地即收尾」那条路)。
// 断 (a)(b)(c) 的轮询里每一拍都小幅来回挪一下:那三格量的是「拖动中」,不该依赖下面 (f) 钉的保活。
//
// [SL-561] 按住不动(rc.1 用户实测 B61:「不松手的时候只要不动,绿色的虚线就没了」):
//   根因 —— 契约 §1.18 的松手档防抖防的是**调用流**、不是 pointerup,而拖动档只在值变了才发;
//   「按住不动」= 停止调用 ⇒ 300ms 后松手那一趟自己起跑、落地收尾预览(被抑制时则是 1.5s
//   空闲收尾)。修法是 tab-wave.js 的按住保活(`SLIDER_HOLD_KEEPALIVE_MS`)。本套加四格:
//   (f) 按住不动 2.5s(跨过 300ms 防抖 + 落地、也跨过 1.5s 空闲收尾):预览一直在、虚影一直画着、
//       期间没有 §2.8 段表帧、没有 active:false 帧;保活真的在发(mock 收到的调用数);
//   (e) 松手那一拍保活计时器当场撤掉(不等下一拍自己发现);松手后那一趟照常落地(§2.8 reason:"vad");
//   (h) 按住时窗口失焦 ⇒ 视为松手:保活停、1s 内落地收尾;
//   (i) 按住时丢了指针捕获 ⇒ 同上。
//
// 删除式(每格只动一处,见 PR 描述):
//   W1 app.js 不订阅 `scvb.vadPreview` ⇒ (a) 红;W2 动态层不画虚影 ⇒ (b) 红;
//   W3 onVadPreview 不刷 VAD 列 ⇒ (c) 红;W4 mock 的 vad 列不读预览 ⇒ (c) 红;
//   W5 mock 的 setVadParams 不发预览 ⇒ (a) 红;W6 onVadPreview 丢掉收尾帧 ⇒ (d) 红;
//   W7 mock 落地不收尾 ⇒ (d) 红。
//   [SL-561] K1 保活不发(`sliderHoldTick` 里那次 sendParams 注释掉)⇒ (f) 红;
//   K2 `up` 里不调 stopSliderHold ⇒ (e) 红;K3 不挂窗口 blur ⇒ (h) 红;
//   K4 不挂 lostpointercapture ⇒ (i) 红。
//
// 用法:node web-preview/tests/smoke-vad-preview-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;2 = 环境里没有 Chrome/Edge(可选依赖,不判红也不算通过);
//   3 = 浏览器在,但这一次没起来 / 没连上([SL-297],gates 记 [FLAKY-SKIP])。
//
// CDP 那段、静态服务、收尾与 smoke-seg-diff-fold-page.mjs 同源(那边的头注写着为什么不抽公共模块)。
// =============================================================================

import { spawn } from "node:child_process";
import { createServer } from "node:http";
import {
    existsSync,
    mkdtempSync,
    readFileSync,
    rmSync,
    statSync,
    writeSync, // [SL-274] 致命错误直写 fd 2,process.exit() 截不掉
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, extname, join, resolve, sep } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

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

// 全部输出走 `writeSync(1, …)` 而不是 `console.log`([SL-274] 第 11 轮复审)。
//
// 理由与致命行那条**是同一条**:`process.exit()` 不等挂起的异步写,而本地 gates 是
// `$out = (& node $f.FullName 2>&1)`(`scripts/gates.ps1:387`)—— stdout 被收成管道。
// 上一轮只把**致命那一行**改成同步,于是同一条失败路径上**先前**打的 `[FAIL]` 与进度行
// 仍可能被 `process.exit(1)` 截掉:红是红了,却说不清前面已经断到哪一步。
//
// ⚠ **别把这句写成「同步写在哪儿都无条件成立」**(第 12 轮复审):Node 对
// `process.stdout` 的同步性是**分档**的 —— 管道/socket 在 **Windows 上同步、POSIX 上
// 异步**(libuv 给该 fd 设了 `O_NONBLOCK`),而对非阻塞管道调 `fs.writeSync` 可能抛
// `EAGAIN`。所以这条改动**真正买到保障的是本地 gates 这条 Windows 路径**,也正是
// 出问题的那条;`web-smoke`(`ubuntu-latest`)那侧 `format.yml` 直接 `node "$f"` 继承
// stdio、本来就没有这个截断面。POSIX 上万一真抛 `EAGAIN`,下面的 catch 会兜回
// `console.log` —— 退化成原来的行为,不会因为「想打得更稳」反而把整套打挂。
//
// 一套冒烟的输出量是几十行,同步写的代价可以忽略,而「红的时候话说不全」的代价不行。
function out(s) {
    try {
        writeSync(1, s + "\n");
    } catch {
        // fd 1 不可写(POSIX 非阻塞管道的 EAGAIN、或 fd 已断)时退回异步写。
        // 这里**不再抛**:打印失败不该变成判定失败。
        try {
            console.log(s);
        } catch {}
    }
}
const log = (s) => out(s);
function check(cond, msg) {
    if (cond) return true;
    fail++;
    out(`  [FAIL] ${msg}`);
    return false;
}
function eq(got, want, msg) {
    const a = JSON.stringify(got);
    const b = JSON.stringify(want);
    if (a === b) return true;
    fail++;
    out(`  [FAIL] ${msg}\n         实得 ${a}\n         应为 ${b}`);
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
    // 目录**边界**判定,不是字符串前缀判定(#161 复审 pr-agent 安全项)。
    // 裸 `startsWith(root)` 的洞:ROOT=`/a/repo` 时 `/a/repo2/x` 也过 —— 百分号编码的
    // `%2e%2e` 会在 decodeURIComponent 之后还原成 `..`,resolve 出去落到兄弟目录,
    // 而它恰好与 ROOT 共享字符串前缀。服务器虽只绑 127.0.0.1、只活在冒烟进程里,
    // 但这是「读文件的边界判定」,没有理由写成会漏的那种。
    const rootAbs = resolve(ROOT);
    if (abs !== rootAbs && !abs.startsWith(rootAbs + sep)) {
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
    // ⚠ 每条 CDP 调用**必须有截止时间**([SL-274] 本机实测踩到)。
    //
    // 原版(与其余五套页面级冒烟同源)的 `send()` 把 resolve 塞进 `pending` 就返回,
    // 响应不来就**永远不 resolve** —— 本套实测在 gate 3e 里挂了 **75 分钟**零输出,
    // Chrome 与 node 都还活着。(CI 上则是一路烧到 job 超时才红,而 `web-smoke` 是
    // required check;本机那次还顺带占住了跑 gates 的 agent 自己那把外层目录锁。)
    //
    // **触发形态是「导航把旧渲染器换掉,紧随其后的 `Runtime.evaluate` 等一个永远不来的
    // 响应」**;本套判据 (5) 要换 `?scenario=diff-flood` 重新装载,踩的就是这一下。
    //
    // ⚠ **别写成「六套里只有本套会二次导航」**(第 10 轮复审 grep 证伪,那句话我写错过):
    // 实点 `cdp.send("Page.navigate"` 的**真实调用**(不含注释里提到的 —— 第 11 轮复审
    // 又抓到我把这个数抄错:上一版写「本套 4 次」是 `grep -c 'Page.navigate'` 把注释
    // 一起数进去了):
    //     ui-layout 8 / monitor 4 / output-dist 3 / **本套 2** / seg-restore 1 / output-stale 1
    // 也就是说**曝险面比本卡大得多,而且都还没修**(它们没有超时也只在 happy path 收尾)。
    // 本卡不越界改别人的文件,已在 PR 里点名建议单独立卡统一收口 —— 谁去做那张卡,
    // 照搬本文件这两段(超时 + teardown)即可。
    //
    // 修法是**超时抛错**而不是重试:响应不来说明页面或渲染器已经不对了,重试只会把
    // 一个确定的红拖成一个更慢的红。错误里带上 method 与 id,红出来直接指到是哪一条卡住。
    //
    // ⚠ 这里**不再**与 `waitFor` 的预算比大小(固定 10s 时代的残留说法已删):这个常量只给
    // **一次性直接调用**用,而 `waitFor` 内部的 evaluate 走的是自己的剩余预算,两者不相干。
    // 留着旧说法会让下一个人以为它必须 < 20000,从而不敢把它调宽。
    // [SL-287] 与另外五套统一成**按调用点取上界**,不再是一个文件级常数:
    // `waitFor` 内部的 evaluate 传**本次还剩多少预算**(第一版写「预算的一半」,被复审指出
    // 那会把耗时落在 (ms/2, ms) 的合法调用从过变成必红;按剩余预算取不改变原本能过的行为),
    // 一次性直接调用用这个宽默认值,只负责兜住真挂死。
    // 换掉固定 10s 的理由见 smoke-output-dist-page:那一套有合法跑 8s 的采样 evaluate、
    // 还有内部自带 12s 上界的 `measureOff`,而它最紧的 waitFor 预算也是 12s ——
    // 「大于合法最大值」与「小于最小预算」在单一常数下无解。六套用同一形态,免得各自漂。
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
    // 同样走同步写:这条也紧跟 `process.exit()`,而 gate 3e 判 rc=2 时按 `^❌` 抓解释行
    // (`scripts/gates.ps1:392`)—— 被截断就只剩一行 SKIP、说不出缺的是什么。
    try {
        writeSync(
            2,
            `❌ ${msg}\n` +
                "   页面级冒烟无法运行(退出码 2)。这**不是**通过:装一个 Chrome/Edge," +
                "或用 --chrome=<路径> 指定。\n",
        );
    } catch {
        console.error(`❌ ${msg}`);
    }
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
    // 与本文件的 `noBrowser()` 同款走**同步写**:这条也紧跟 `process.exit()`,而 gate 3e
    // 判 rc=3 时同样按 `^❌` 抓解释行 —— 被截断就只剩一行 [FLAKY-SKIP]、说不出没跑成的原因。
    // (本文件特意用 `writeSync` 而不是 `console.error` 的理由见 noBrowser 处;
    //  新出口沿用同一层加固,别因为是「新写的」就退回 console.error。)
    try {
        writeSync(
            2,
            `❌ ${msg}\n` +
                "   页面级冒烟**没跑成**(退出码 3):浏览器是在的,但这一次没起来 / 没连上。" +
                "这**不是**通过,也**不是**「本机没装浏览器」——重跑一次通常就好;" +
                "连续复现请查 CDP 端口占用、机器负载或 Chrome 版本。\n",
        );
    } catch {
        console.error(`❌ ${msg}`);
    }
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
    argv.get("cdp") || 9800 + Math.floor(Math.random() * 400),
);
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-vad-preview-"));
const chrome = spawn(
    exe,
    [
        "--headless=new",
        `--remote-debugging-port=${CDP_PORT}`,
        `--user-data-dir=${userDataDir}`,
        "--window-size=1600,1000",
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
const errors = [];
const exceptions = [];

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

// 收尾:**任何**退出路径都要走一遍,不只是跑完那一条([SL-274] 本机实测)。
// 原版把 kill/close/rmSync 摊在文件末尾,于是**只有 happy path 会清理** ——
// 一旦中途抛错(比如上面新加的 CDP 超时),进程直接死,headless Chrome 与
// `scvb-vad-preview-*` 临时目录全部留在机器上。这次排查时本机已积了 35 个残留目录,
// 而挂死那次留下的 Chrome 一直活到人工介入才被杀掉。
// (其余五套页面级冒烟同样只在 happy path 收尾 —— 见上面 cdpConnect 头注那条。)
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
// ⚠ 收得干净的是**进程**,不保证临时目录:`chrome.kill()` 之后文件句柄未必立刻释放,
// 紧跟的 `rmSync` 在 Windows 上可能失败,留下一个空的 `scvb-vad-preview-*`。
// 那是可接受的残渣(系统会清),而**跑着的 headless Chrome 不是** —— 它会占住端口与
// 机器资源,这次就是它跟着挂死的 node 一起赖了 75 分钟。`exit` 处理器只能同步收尾
// (Node 规范),所以这里不等、也不重试。

process.on("exit", teardown);
for (const sig of ["SIGINT", "SIGTERM"]) {
    process.on(sig, () => {
        teardown();
        process.exit(130);
    });
}
// 未捕获异常/未处理拒绝:先打印再收尾,否则 Chrome 会跟着一起漏。
//
// 这一行要真的到得了人眼前,两件事都得对(第 10/11 轮复审,**都是核过源码才写的**):
//
//   · **写法**:用 `writeSync(2, …)` 而不是 `console.error` —— `process.exit()` 不等挂起的
//     异步写,stdout/stderr 被重定向成管道/文件时最后那行**可能被截断**。
//     `writeSync` 直写 fd 2,退不退出都不会丢。
//     ⚠ 这一条的**理由已被 [SL-287] 改掉**(结论仍成立):原文写「gates 是
//     `(& node … 2>&1)`,stderr 已并进 stdout」—— 现在 gate 3e 用
//     `Start-Process … -RedirectStandardOutput/-RedirectStandardError` 写**两个独立文件**,
//     没有 `2>&1` 了;stderr 仍到得了 gates,是因为它把两个文件**先后拼进** `$out`。
//     顺带一个新事实:stdout 与 stderr 是**先后拼接、不是交错** —— 这一行会整体排在所有
//     stdout 之后,读日志时别按出现顺序去推因果。
//   · **前缀**:[SL-287] 之前 gate 3e 只捞 `Select-String '\[FAIL\]'`,而
//     `[FATAL]` **不含子串 `[FAIL]`**(F-A-T-A-L ≠ F-A-I-L),所以本文件当时改打
//     `[FAIL] FATAL …` 才不至于被整条过滤。**现在 gate 3e 捞 `'\[FAIL\]|\[FATAL\]'`**,
//     那个变通已无必要,故与另外五套统一成 `[FATAL]`。
//     (缩进从来不影响:那是**不带 `^` 锚点的子串匹配**。第 10 轮我把原因归到缩进上,写错了。)
//     [J96] 之后本地 gates 是子 PR 上唯一的门,这条诊断丢了就真的没有别处能看。
//   · 六套里只有本文件用 `writeSync(2, …)`,另五套是 `console.error` —— 那是本文件多出来的
//     一层稳,不是不一致的 bug;要不要推广到另五套,留给动它们的下一张卡。
for (const ev of ["uncaughtException", "unhandledRejection"]) {
    process.on(ev, (e) => {
        const msg = e && e.message ? e.message : String(e);
        try {
            writeSync(2, `  [FATAL] ${ev}:${msg}\n`);
        } catch {
            console.error(`  [FATAL] ${ev}:`, msg);
        }
        teardown();
        process.exit(1);
    });
}

async function waitFor(expr, ms = 20000) {
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
    const gb = (n) => q('[data-gb="' + n + '"]');
    const vis = (el) => !!el && !el.hidden;
    ${js}
})()`;

const READY = IN(`
    const lane = gb("wave-lane-2-label");
    return !!(lane && lane.textContent && lane.textContent.length > 0);
`);

// ---------------------------------------------------------------- 启动
const CDP_WAIT_TRIES = 300;
const CDP_WAIT_STEP_MS = 200;
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
cdp = cdpConnect(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
await cdp.ready;
await cdp.send("Page.enable");
await cdp.send("Runtime.enable");
cdp.on((m) => {
    if (m.method === "Runtime.exceptionThrown") {
        const d = m.params.exceptionDetails;
        exceptions.push(
            (d.exception?.description || d.text || "").split("\n")[0],
        );
    } else if (
        m.method === "Runtime.consoleAPICalled" &&
        m.params.type === "error"
    ) {
        errors.push(
            (m.params.args || [])
                .map((a) => a.value ?? a.description ?? "")
                .join(" "),
        );
    }
});

log(`(站点根 ${ROOT} -> ${base};CDP ${CDP_PORT})`);
log("=== J146 拖动档预览 —— 页面级 ===");

async function mouse(type, x, y) {
    await cdp.send("Input.dispatchMouseEvent", {
        type,
        x,
        y,
        button: "left",
        buttons: type === "mouseReleased" ? 0 : 1,
        clickCount: 1,
        pointerType: "mouse",
    });
}

const DIAG = IN(`return w.__SCVB_OUTPUT__.wave().vadPreview;`);
// 泳道 1 静态 canvas 顶部 VAD 标注带里的绿色像素数(带高 3 CSS px,取第 1 行设备像素)。
const GREEN = IN(`
    const cv = gb("wave-lane-1-static");
    if (!cv || !cv.width) return -1;
    const row = cv.getContext("2d").getImageData(0, 1, cv.width, 1).data;
    let n = 0;
    for (let i = 0; i < row.length; i += 4) {
        if (row[i + 3] > 60 && row[i + 1] > row[i] + 40) n++;
    }
    return n;
`);
const TRACK = IN(`
    const box = gb("wave-vad-threshold");
    const t = box && box.querySelector(".wave-slider__track");
    if (!t) return null;
    const r = t.getBoundingClientRect();
    const fr = f.getBoundingClientRect();
    return { x: fr.left + r.left, y: fr.top + r.top + r.height / 2, w: r.width,
             now: t.getAttribute("aria-valuenow") };
`);

await cdp.send("Page.navigate", { url: `${base}/web-preview/output.html` });
check(await waitFor(READY), "页面装载并吃到首帧段表");
await evaluate(
    IN(`const b = gb("tabnav-wave"); if (b) b.click(); return true;`),
);
check(
    await waitFor(`(${GREEN}) > 0`),
    "前置:泳道 1 的 VAD 标注带画出来了(瓦片已到)",
);
await sleep(400); // 让在途的概览块也落地,基线别被它的晚到改写
const green0 = await evaluate(GREEN);
const diag0 = await evaluate(DIAG);
log(`  [基线] green=${green0} diag=${JSON.stringify(diag0)}`);
check(
    !!diag0 && diag0.active === false && diag0.edgesDrawn === 0,
    "基线:不在预览中、没有虚影",
);

const tr = await evaluate(TRACK);
// fixture 的 `ui.tour_seen=false` ⇒ tour 询问卡连着整块 `.sc-scrim` 罩在页上,真鼠标全落在罩子上
// (smoke-undo-scope-page 同一处理)。收掉之后**必须**断 elementFromPoint 真的落回滑杆 ——
// 少了这一断,「事件没到滑杆」会长得和「预览没发」一模一样,整套变成量错东西的红/绿。
await evaluate(
    IN(
        `const later = gb("tour-ask-later"); if (later) later.click(); return true;`,
    ),
);
check(
    await waitFor(
        IN(`const box = gb("wave-vad-threshold");
            const t = box && box.querySelector(".wave-slider__track");
            if (!t) return false;
            const r = t.getBoundingClientRect();
            const el = d.elementFromPoint(r.left + r.width * 0.44, r.top + r.height / 2);
            return !!el && (el === t || t.contains(el));`),
        5000,
    ),
    "前置:真鼠标落点就是阈值滑杆(没有罩子挡着)",
);
check(!!tr && tr.w > 20, `阈值滑杆量得到(${JSON.stringify(tr)})`);
const xAt = (p) => tr.x + tr.w * p;

// [SL-561] 页内记录器:mock 收到的 setVadParams 调用时刻、§2.8 段表帧、§2.10 收尾帧。
// 按住期间「段表没动 / 没有收尾帧 / 保活真的在发」三件事画面上分不开,只能在桥的 mock 端数。
// 桥按名字现取 `mock[name]`(bridge.js makeMockBridge),所以包一层即可,不改 mock 本身。
check(
    await evaluate(
        IN(`const mk = w.__SCVB_MOCK__;
            if (!mk || typeof mk.setVadParams !== "function") return false;
            const rec = (w.__sl561 = { calls: [], segs: [], ends: [] });
            const orig = mk.setVadParams;
            mk.setVadParams = function () {
                rec.calls.push(Date.now());
                return orig.apply(this, arguments);
            };
            mk.addEventListener("scvb.segments", (e) =>
                rec.segs.push({ at: Date.now(), reason: e && e.reason }));
            mk.addEventListener("scvb.vadPreview", (e) => {
                if (e && e.active === false) rec.ends.push(Date.now());
            });
            return true;`),
    ),
    "前置:[SL-561] 页内记录器装上(mock 的 setVadParams / 段表帧 / 收尾帧)",
);
// 记录器在某一时刻之后的增量。
const REC_SINCE = (t) =>
    IN(`const r = w.__sl561;
        if (!r) return null;
        const t = ${Number(t)};
        const calls = r.calls.filter((x) => x >= t);
        let gap = 0;
        for (let i = 1; i < calls.length; i++) gap = Math.max(gap, calls[i] - calls[i - 1]);
        return {
            calls: calls.length,
            maxGap: gap,
            segs: r.segs.filter((s) => s.at >= t).map((s) => s.reason),
            ends: r.ends.filter((x) => x >= t).length,
        };`);
// 按在当前值(−38 ⇒ p=0.44)上:按下本身不改值,改值从拖动开始 —— 与真人一样。
await mouse("mouseMoved", xAt(0.44), tr.y);
await mouse("mousePressed", xAt(0.44), tr.y);
for (const p of [0.55, 0.7, 0.85, 1.0]) {
    await mouse("mouseMoved", xAt(p), tr.y);
    await sleep(30);
}

// ---- 不松手:一边小幅来回挪(拖动中的样子),一边等 (a)(b)(c) ------------------
let seen = null;
let greenDrag = green0;
const t0 = Date.now();
let flip = false;
while (Date.now() - t0 < 8000) {
    flip = !flip;
    await mouse("mouseMoved", xAt(flip ? 0.96 : 1.0), tr.y); // −12 / −10 dB 来回
    await sleep(60);
    const d = await evaluate(DIAG);
    greenDrag = await evaluate(GREEN);
    if (
        d &&
        d.active &&
        d.edgesDrawn > 0 &&
        greenDrag >= 0 &&
        greenDrag !== green0
    ) {
        seen = d;
        break;
    }
    seen = d;
}
log(`  [拖动中] green=${greenDrag} diag=${JSON.stringify(seen)}`);
const nowVal = await evaluate(TRACK);
check(
    !!nowVal && Number(nowVal.now) > -20,
    `前置:滑杆真的被拖到高门限一侧(aria-valuenow=${nowVal && nowVal.now})`,
);
check(
    !!seen && seen.active === true && seen.seq > (diag0 ? diag0.seq : -1),
    `(a) 松手前预览事件已到页面(实得 ${JSON.stringify(seen)})`,
);
check(
    !!seen && seen.lanes > 0 && seen.spans > 0,
    "(a) 预览载荷里有写回集轨与它们的预览段",
);
check(
    !!seen && seen.edgesDrawn > 0,
    `(b) 松手前虚影已画上泳道(实得 ${seen && seen.edgesDrawn} 条)`,
);
check(
    greenDrag >= 0 && greenDrag < green0,
    `(c) 松手前 VAD 标注带已重画且变窄(绿色像素 ${green0} → ${greenDrag};门限拉高 ⇒ 更少)`,
);
check(
    !!seen && seen.refetches > 0,
    `(c) 刷 VAD 列那条路真的走过(refetches=${seen && seen.refetches})`,
);

// ---- [SL-561] (f) 按住不动 2.5s:预览一直在、段表不动,直到松手 ------------------
// 手停在最后那一拍(1.0),此后一个鼠标事件都不发。2.5s 同时跨过两个旧的收尾时刻:
// 300ms 防抖 → 松手那一趟起跑 → 落地收尾(mock 里是同步落地),以及 1.5s 空闲收尾;各留约 1s 余量。
// 每 ~150ms 采一次诊断,**每一拍**都要在预览中 —— 只看最后一拍会放过「中途收掉、下一发又开」的闪断。
const HOLD_MS = 2500;
await mouse("mouseMoved", xAt(1.0), tr.y); // 落在 1.0,之后不再动
await sleep(60);
const holdVal = (await evaluate(TRACK)) || {};
const holdT0 = Date.now();
const holdSamples = [];
while (Date.now() - holdT0 < HOLD_MS) {
    await sleep(150);
    const d = await evaluate(DIAG);
    holdSamples.push({ t: Date.now() - holdT0, d });
}
const holdRec = await evaluate(REC_SINCE(holdT0));
const holdEndVal = (await evaluate(TRACK)) || {};
const firstOff = holdSamples.find(
    (s) => !s.d || s.d.active !== true || !(s.d.edgesDrawn > 0),
);
log(
    `  [按住不动 ${HOLD_MS}ms] 采样 ${holdSamples.length} 拍;首个失守 ${
        firstOff ? `${firstOff.t}ms ${JSON.stringify(firstOff.d)}` : "无"
    };记录器 ${JSON.stringify(holdRec)}`,
);
check(
    holdSamples.length >= 8 && holdVal.now === holdEndVal.now,
    `(f) 前置:采样够密(${holdSamples.length} 拍)且期间滑杆值没变(${holdVal.now} → ${holdEndVal.now})`,
);
check(
    holdSamples.every((s) => !!s.d && s.d.active === true),
    `(f) **按住不动期间预览一直在**(每一拍 active;首个失守 ${
        firstOff ? `${firstOff.t}ms` : "无"
    };修前:约 300ms 防抖到点、松手那一趟落地即收尾)`,
);
check(
    holdSamples.every((s) => !!s.d && s.d.edgesDrawn > 0),
    "(f) 按住不动期间绿色点划线一直画着(每一拍 edgesDrawn > 0)",
);
check(
    holdSamples.every((s) => !!s.d && s.d.sliderDrag === true),
    "(f) 前置:整段都还是按住态(没有别的路径替它松了手)",
);
check(
    !!holdRec && holdRec.segs.length === 0 && holdRec.ends === 0,
    `(f) 按住期间段表不动、没有收尾帧(实得 段表帧 ${JSON.stringify(holdRec && holdRec.segs)}、` +
        `收尾帧 ${holdRec && holdRec.ends})`,
);
check(
    !!holdRec && holdRec.calls >= Math.floor(HOLD_MS / 300),
    `(f) 保活真的在发:按住期间 mock 收到 ${holdRec && holdRec.calls} 次 setVadParams` +
        `(至少每 300ms 一次 ⇒ ≥ ${Math.floor(HOLD_MS / 300)};最大间隔 ${holdRec && holdRec.maxGap}ms)`,
);

// ---- 松手 ⇒ 松手档落地 ⇒ 收尾 -------------------------------------------------
const relT0 = Date.now();
await mouse("mouseReleased", xAt(1.0), tr.y);
const diagRel = await evaluate(DIAG);
check(
    !!diagRel && diagRel.sliderDrag === false && diagRel.holdTimer === false,
    `(e) [SL-561] 松手那一拍按住态与保活计时器当场撤掉(实得 ${JSON.stringify(diagRel)})`,
);
const closed = await waitFor(
    `(() => { const d = ${DIAG}; return !!d && d.active === false && d.edgesDrawn === 0; })()`,
    1000,
);
const diagEnd = await evaluate(DIAG);
log(`  [松手后] diag=${JSON.stringify(diagEnd)}`);
check(
    closed,
    `(d) 松手 1s 内预览收尾、虚影归零(实得 ${JSON.stringify(diagEnd)};空闲收尾要 1.5s,` +
        "1s 内归零只能是「落地即收尾」)",
);
await sleep(500);
const greenEnd = await evaluate(GREEN);
check(
    greenEnd >= 0 && greenEnd < green0,
    `(d) 落地后 VAD 标注带保持新门限的样子(${greenEnd} < 基线 ${green0};与 native「vadP 按同一组参数写」同形)`,
);
const relRec = await evaluate(REC_SINCE(relT0));
log(`  [松手后记录器] ${JSON.stringify(relRec)}`);
check(
    !!relRec && relRec.segs.includes("vad") && relRec.ends >= 1,
    `(e) 松手后那一趟照常落地:§2.8 reason:"vad" 与收尾帧都在松手之后(实得 ${JSON.stringify(relRec)})`,
);

// ---- [SL-561] (h)(i) 按住时失焦 / 丢捕获 ⇒ 视为松手 ---------------------------
// 按住保活让「拖拽态没收尾」从无害变成有害(保活一直续着防抖,松手那一趟永远不跑、预览永远不收),
// 所以 pointerup / pointercancel 之外的两个出口也要收尾。每格:按下 → 拖一下改值 → 等预览出来 →
// 按住不动 400ms(保活在跑)→ 触发 → 断「当场松手 + 1s 内落地收尾」→ 真鼠标抬起收场。
async function holdThenTrigger(label, pFrom, pTo, what, triggerJs) {
    await mouse("mouseMoved", xAt(pFrom), tr.y);
    await mouse("mousePressed", xAt(pFrom), tr.y);
    for (const p of [(pFrom + pTo) / 2, pTo]) {
        await mouse("mouseMoved", xAt(p), tr.y);
        await sleep(30);
    }
    check(
        await waitFor(
            `(() => { const d = ${DIAG}; return !!d && d.active === true && d.sliderDrag === true && d.holdTimer === true; })()`,
            3000,
        ),
        `(${label}) 前置:按住中、预览已出、保活在跑`,
    );
    await sleep(400);
    const pre = await evaluate(DIAG);
    check(
        !!pre && pre.active === true && pre.sliderDrag === true,
        `(${label}) 前置:按住不动 400ms 后仍在预览中(实得 ${JSON.stringify(pre)})`,
    );
    const t0 = Date.now();
    const trig = await evaluate(IN(triggerJs));
    check(
        trig === true,
        `(${label}) 触发${what}(实得 ${JSON.stringify(trig)})`,
    );
    const released = await waitFor(
        `(() => { const d = ${DIAG}; return !!d && d.sliderDrag === false && d.holdTimer === false; })()`,
        500,
    );
    const closedX = await waitFor(
        `(() => { const d = ${DIAG}; return !!d && d.active === false && d.edgesDrawn === 0; })()`,
        1000,
    );
    const rec = await evaluate(REC_SINCE(t0));
    const dEnd = await evaluate(DIAG);
    log(
        `  [${label} ${what}后] diag=${JSON.stringify(dEnd)} 记录器=${JSON.stringify(rec)}`,
    );
    check(released, `(${label}) **${what} ⇒ 视为松手**:按住态与保活计时器撤掉`);
    check(
        closedX && !!rec && rec.segs.includes("vad"),
        `(${label}) ${what}后 1s 内松手那一趟落地、预览收尾(段表帧 ${JSON.stringify(rec && rec.segs)})`,
    );
    await mouse("mouseReleased", xAt(pTo), tr.y); // 已不在按住态:up 早退,之后的丢捕获同样早退
    await sleep(300);
}
await holdThenTrigger(
    "h",
    1.0,
    0.7,
    "窗口失焦",
    `w.dispatchEvent(new Event("blur")); return true;`,
);
await holdThenTrigger(
    "i",
    0.7,
    0.5,
    "丢指针捕获",
    `const box = gb("wave-vad-threshold");
     const t = box && box.querySelector(".wave-slider__track");
     if (!t) return "no-track";
     for (const id of [1, 0, 2, 3]) {
         if (t.hasPointerCapture(id)) {
             t.releasePointerCapture(id);
             return true;
         }
     }
     return "no-capture";`,
);

// ---- 页面零 console.error / 零未捕获异常(全套通用底线)-----------------------
check(
    exceptions.length === 0,
    `页面零未捕获异常(实得 ${exceptions.length} 条:${exceptions.slice(0, 3).join(" | ")})`,
);
check(
    errors.length === 0,
    `页面零 console.error(实得 ${errors.length} 条:${errors.slice(0, 3).join(" | ")})`,
);

log(fail === 0 ? "\n全绿" : `\n${fail} 条 FAIL`);
teardown();
process.exit(fail === 0 ? 0 : 1);
