// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Output 首启 tour 的「demo 数据真的上屏」+「inert 只清自己置的」
// 页面级冒烟(SL-33 / SL-35)
// =============================================================================
// 为什么必须页面级:
//   [SL-33] `tour.js` 的 `start()` 建好 `demoStore` 之后只调了 `showStep(1)`,从没
//   自己叫过 `requestRender()`;而 `showStep()`→`activateTab(cfg.tab,{push:false})`
//   只在**切换了 tab** 时才顺带排一次 render(app.js `activateTab()`:
//   `if (was !== name) requestRender()`)。步 1..15 的 tab 全是 "master" —— 恰好是
//   进入 tour 前默认已激活的那个 tab,`was === name` 恒成立,那条件永远不触发。
//   于是 `viewStore().isActive()` 已经切到 demo,但从没有一次真正的 `render()` 把它
//   投影到屏幕上:用户看到的仍是**自己工程里的真实数据**,同时旁边的说明词在讲解
//   一份演示轨道。node 侧对 `buildDemoStore()`/`TOUR_STEPS` 的纯函数断言(如
//   smoke-tour.mjs)只验证数据形状,从不触碰"到底有没有一次 render() 把它画出来"
//   这件事 ——「源码正则/纯函数断言 ≠ 可执行」,必须起真页面走一次首启入口。
//
//   [SL-35] `endTour()` 对 `card.children` **无条件** `removeAttribute("inert")`,
//   不判断该子节点的 inert 是不是本次 `start()` 自己置上的。缺陷有两种触发时刻,
//   两种都要各有判据:
//     · tour **中途**插入的、与 tour 无关的新节点(`card.children` 是 live 集合,
//       `start()` 从没碰过它,因为它当时还不存在);
//     · tour **开始前**就已经带 `inert` 的节点(第一版修复只堵了前一种——
//       `inertedChildren` 记的是「`start()` 当时碰过的」而不是「`start()` 自己
//       置上的」,于是这一种照样被收进名单、被 `endTour` 误清;复审【重要】①
//       点名的正是这一半)。
//
// 跑什么(同一条会话上连续走完):
//   ① `?fixture=empty` 首帧:`header-conn-count` = "0/15"(真实数据,0 轨连接);
//      guide_seen=true(fixture=empty 显式置)、tour_seen/tour_seen_global 未覆写
//      仍是 false ⇒ 询问步 `tour-ask` 会**自动**弹出(05 §2.6「启动兜底」),
//      不需要经红字九条页,curTab 全程停在默认的 "master" ——这正是①能复现
//      SL-33 的关键:`start()` 时 was===name(master===master),activateTab 自己
//      不会顺手排一次 render。
//   ② 先轮询 `sl33Settled()`(定义见文件顶部,连同下界一起判定 mock 驱动的一次性
//      差分是否已经落定或已过安全网下界),再插一个 tour **开始前**就带 `inert`
//      的节点,然后点「tour-ask-start」进入 tour:蒙版可见;`header-conn-count`
//      在几秒内变成 "15/15"(demo 的 FIFTEEN_TRACKS 15 轨全部健康在线)—— 这一位
//      只能来自**真的跑过一次 render()**,不是"isActive() 已经是 true"就有的副作用。
//   ③ tour 激活期间,再往 `#card` 里插一个 tour **中途**插入、与 tour 无关、
//      自带 `inert` 的新节点,然后点 Skip 结束 tour;断言:
//      · ②插的「开始前」节点与③插的「中途」节点,`inert` **都仍然在**
//        (endTour 两种情形都不该动它们);
//      · `start()` 自己置过 inert 的原生子节点(header)**已经**被正常释放
//        (回归检查:收窄范围没有把真正该清的那一半也弄丢);
//      · `header-conn-count` 变回 "0/15"(viewStore() 切回真实 store)。
//   ④ 每段零 console.error、零未捕获异常。
//
// 删除式:
//   · `sl33Settled()` 的下界(每次跑本文件都会执行,不依赖真实浏览器时序运气):
//     文件顶部 `selfTestSl33Settled()` 直接对这个纯函数注入"差分延迟到阈值之后
//     才落地"的时间序列,钉住"下界=0"与"一见 mutation 就放行"两种更粗糙的写法,
//     退回其中任一种都会让对应的自测用例转红(未提交,人工核过)。
//   · 去掉 `tour.js` `start()` 里新增的那句 `requestRender()`(未提交,人工核过),
//     ②段必须转红(卡在 "15/15" 等不到,取到的还是 "0/15");
//   · 把 `start()` 里的 `!child.hasAttribute("inert")` 判断去掉(退回「当时碰到的
//     全收」半修复,未提交,人工核过),③段「开始前」那个节点的 `inert` 仍在断言
//     必须转红;
//   · 把 `endTour()` 的清 inert 循环改回 `for (const child of card.children)
//     child.removeAttribute("inert")`(未提交,人工核过),③段两个节点的 `inert`
//     仍在断言必须转红。
//
// 用法:node web-preview/tests/smoke-tour-demo-page.mjs [仓库根绝对路径]
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

// ---------------------------------------------------------------- SL-33 等待谓词
// [复审【重要】] 「安静 SETTLE_MS」这种"没有变化"的观测量,下界结构上是 0——挂上
// 观察器那一刻,`count===0` 时只要等满 SETTLE_MS 就会判定"安静",而这并不能推出
// "mock 那次一次性差分已经落地":它可能压根还没发生。实测这次差分的到达时刻能
// 从 <300ms 飘到 >3s(headless 对后台标签页的定时器节流),纯粹的"安静"判据会在
// 差分真正发生**之前**放行,点击之后差分才落地 ⇒ 没有 SL-33 修复也会把画面刷成
// demo(假绿)。
// 本该优先找一个正向的、确定的目标态直接等(它天然有下界:目标没出现就不会放行)。
// 但这次差分在设计上就不产生任何可观测的值变化 —— mock 只是把内部 undefined 基线
// 与真实载荷做一次 diff,fixture=empty 下 conn/groups 的值在差分前后逐字相同,找不到
// 一个只由它产出的正向信号。因此退回有界版本,把残留面写实:FLOOR_MS 是安全网,
// 与被替换的 `sleep(4000)` 同量级下界(覆盖实测最坏 >3s 的节流场景),不是"已经不
// 赌墙钟"那么干净 —— 只是把赌注从"唯一出口"降成"兜底出口"。SETTLE_MS 本身也
// 主动抬到 > mock 最慢的周期发射器 `groups1Hz`(1000ms),让"安静"在正常(不节流)
// 路径下真的能推出"两个周期发射器都已经不再产出新变化",而不是碰巧只跨过了
// `conn4Hz`(250ms)一档。
//
// 用 `toString()` 把这个函数原样注入页面(见下方调用点),保证节点侧轮询与这里的
// 自测跑的是**同一段代码**,不会两处各写一份、漂出分歧。
//
// [复审【重要】第 4 轮] 上一版是 `return settled || floorPassed`——`floorPassed`
// 在 t>4000 无条件成立,会把"已经看到过 mutation、但还没安静"这个状态在 4s 处
// 自己翻成放行。假绿路径是实的:mock 那次"一次性差分"其实是两个独立定时器各自
// 的首发(`conn4Hz` 250ms / `groups1Hz` 1000ms,天然差 ~1s)——conn 的被节流到
// 3.5s 落地,FLOOR 在 4.0s 无条件放行,点击之后 groups 的才落地,它排的那次
// render 把 demoStore 投成 15/15,没有 SL-33 的修复也绿。改成 `count > 0` 时
// **只走安静分支**,FLOOR 收窄回它真正该管的那一格(一次都没观察到);看到过就
// 老实等安静,等不到就让调用点的 12s `waitFor` 判红(宁红不假绿)。
function sl33Settled({ count, lastAt, installedAt, now }) {
    const SETTLE_MS = 1200;
    const FLOOR_MS = 4000;
    // FLOOR 只是"一次 mutation 都没观察到"时的兜底出口:一旦观察到过,就必须
    // 等满 SETTLE_MS 的安静期,不能再被 FLOOR 短路——否则 conn4Hz 的差分被节流
    // 到 FLOOR 附近落地时,groups1Hz 那条(晚 ~1s)会落到点击之后,回到假绿。
    // 观察到过、却一直不安静 ⇒ 让调用点的 12s waitFor 判红,不放行。
    if (count > 0) return now - lastAt > SETTLE_MS;
    return now - installedAt > FLOOR_MS;
}

// 删除式(钉住"下界"这一格,不依赖真实浏览器时序运气——直接对纯函数注入
// "差分延迟到阈值之后才落地"的时间序列):
(function selfTestSl33Settled() {
    eq(
        sl33Settled({ count: 0, lastAt: 0, installedAt: 0, now: 0 }),
        false,
        "自测 sl33Settled:刚挂上观察器、零 mutation ⇒ 不该立刻放行" +
            "(钉住「下界=0」那个旧缺陷——bot 4054093079 指出的正是这一格)",
    );
    eq(
        sl33Settled({ count: 0, lastAt: 0, installedAt: 0, now: 3999 }),
        false,
        "自测 sl33Settled:零 mutation、飘到实测最坏区间(3999ms)仍未到 FLOOR_MS ⇒ 仍不该放行",
    );
    eq(
        sl33Settled({ count: 0, lastAt: 0, installedAt: 0, now: 4001 }),
        true,
        "自测 sl33Settled:零 mutation 但已过 FLOOR_MS ⇒ 兜底放行(与旧 sleep(4000) 同量级下界)",
    );
    eq(
        sl33Settled({ count: 1, lastAt: 3500, installedAt: 0, now: 3501 }),
        false,
        "自测 sl33Settled:差分刚落地(t=3500,落在实测最坏区间内)、还没安静满 " +
            "SETTLE_MS ⇒ 不该立刻放行(这一格钉的是「一见 mutation 就放行」那个更粗糙的写法)",
    );
    eq(
        sl33Settled({
            count: 1,
            lastAt: 3500,
            installedAt: 0,
            now: 3500 + 1200 + 1,
        }),
        true,
        "自测 sl33Settled:差分落地后安静满 SETTLE_MS ⇒ 放行",
    );
    // [复审【重要】第 4 轮] 钉死"看到过 mutation 就不许被 FLOOR 短路"这条新不
    // 变量。⚠ 复审给的例子 `{lastAt:3500, now:5000}`(距上次 mutation 1500ms)
    // 代入两版都是 true——1500 已经过了 SETTLE_MS(1200),旧版的 `settled` 分支
    // 自己就成立,新旧结果相同,钉不住这处改动(同「找不到新旧结果不同的输入 =
    // 这格没钉住改动」那条判例,已用 node -e 实际算过两版结果,不是照抄）。改用
    // mutation 发生得晚、且已经过了 FLOOR_MS 的一点:`lastAt=4500`(late,尚未
    // 安静满 1200)、`now=4600`(已过 FLOOR_MS=4000)——旧版 `floorPassed` 无条件
    // 成立 ⇒ true(错,应继续等);新版只看 `count>0` 分支 ⇒ 100ms<1200 ⇒ false
    // (对,继续等)。退回 `settled || floorPassed` 会让这一格转红(已反注验证)。
    eq(
        sl33Settled({ count: 1, lastAt: 4500, installedAt: 0, now: 4600 }),
        false,
        "自测 sl33Settled:已观察到 mutation(即便已过 FLOOR_MS)、尚未安静满 " +
            "SETTLE_MS 时,FLOOR 不得短路放行(退回 settled || floorPassed 会让这一格转红)",
    );
})();
// 纯函数自测不依赖浏览器:在下方任何可能走 rc=2/3(没装 Chrome / 连不上)的退出
// 路径之前就结账,别让「没装 Chrome」的 SKIP 把这道判据一起吞掉([复审【建议】]
// 这一族本来就不该受浏览器有无影响)。
if (fail > 0) {
    console.log(`\n❌ sl33Settled 自测 ${fail} 条断言失败`);
    process.exit(1);
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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-tour-demo-"));
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

const CONN_COUNT_TEXT = IN(`
    const n = gb("header-conn-count");
    return n ? n.textContent : null;
`);

// 首帧判据:真实数据(fixture=empty)已经渲染过一轮 —— header-conn-count 落定 "0/15"。
const READY = IN(`
    const n = gb("header-conn-count");
    return !!(n && n.textContent === "0/15");
`);

async function open(query) {
    newBucket(query);
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?${query}`,
    });
    const ok = await waitFor(READY, 20000);
    check(ok, `${query}:页面装载并渲染出真实数据(header-conn-count=0/15)`);
}

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
    log("=== ① ?fixture=empty 首帧(真实数据 0 轨连接)+ 询问步自动弹出 ===");
    await open("fixture=empty");
    eq(
        await evaluate(CONN_COUNT_TEXT),
        "0/15",
        "首帧 header-conn-count 读真实数据(0 轨连接)",
    );
    check(
        await waitFor(
            IN(`const o = gb("tour-ask"); return !!o && o.hidden === false;`),
            8000,
        ),
        "询问步 tour-ask 自动弹出(guide_seen=true ∧ tour_seen/global 未置位)",
    );
    assertClean("① 首帧 + 询问步");

    // =========================================================================
    log("=== ② 点「开始」进 tour ⇒ demo 数据必须真的上屏(SL-33) ===");
    // [复审【重要】] 点「开始」前必须让 mock 驱动的周期帧(scvb.conn/scvb.groups 的
    // `emitIfChanged`)把它们**唯一一次**"从空基线到真实载荷"的差分发完并稳定下来
    // —— 那次差分会触发一次不相干的 `requestRender()`,如果它落在点击**之后**,
    // 没有 SL-33 那个修复也会把画面"顺带"刷成 demo 数据(假绿,本仓最反对的方向)。
    // 旧版在这里写死 `sleep(4000)`,与 CLAUDE.md §10「别写死 sleep,要轮询」冲突;
    // 第一版改成"轮询到 #card 安静 700ms",但那个谓词的下界是 0——挂上观察器那一刻
    // 若一次 mutation 都没见到,700ms 后照样判"安静",推不出"差分已经落地",而实测
    // 这次差分能飘到 >3s。判据形状与残留面见上方 `sl33Settled()` 的头注(那份函数
    // 原样注入到这里,保证节点侧轮询与自测跑的是同一段代码)。
    // 观察面也收窄到 `header-conn-count` 一个节点、只看 `childList`/`characterData`
    // —— 论证只用到「`renderHeader()` 无条件重写这一格 `textContent`」这一条信号,
    // `attributes` 与 `#card` 整棵子树都是论证之外的面:今天 idle 期没有别的东西在
    // 动(meters 有 T33 空闲零 rAF 自停、playhead 有 `samePlayhead` 过滤、conn/groups
    // 走 `emitIfChanged`),但那是**当前实现的性质,不是不变量**——哪天 idle 期多一处
    // 逐帧直写属性的东西,原来那份宽观察面会把这一格从"偶发"变成 12s 超时假红。
    check(
        await evaluate(
            IN(`const n = gb("header-conn-count");
                if (!n) return false;
                window.__sl33Mut = {
                    count: 0,
                    lastAt: performance.now(),
                    installedAt: performance.now(),
                };
                new (w.MutationObserver)(() => {
                    window.__sl33Mut.count++;
                    window.__sl33Mut.lastAt = performance.now();
                }).observe(n, { childList: true, characterData: true });
                return true;`),
        ),
        "在 header-conn-count 上挂好渲染观测(点「开始」前)",
    );
    check(
        await waitFor(
            IN(`const s = window.__sl33Mut;
                if (!s) return false;
                const settledFn = ${sl33Settled.toString()};
                return settledFn({
                    count: s.count,
                    lastAt: s.lastAt,
                    installedAt: s.installedAt,
                    now: performance.now(),
                });`),
            12000,
        ),
        "一次性差分已落定或已过安全网下界(见 sl33Settled 头注;12s 内都没成立才判超时)",
    );
    // [复审【重要】① 的另一半] SL-35 的缺陷有两种触发时刻:「tour 中途插入」(下面
    // ③ 段已经在验)与「tour 开始前就已经带 inert」——两种都要被 start() 跳过、
    // 不计入 inertedChildren,endTour 才不会误清。这里在点「开始」**之前**先插一个
    // 自带 inert 的节点,验的是后一种(旧代码把 `card.children` 当时碰到的全收进
    // inertedChildren,不管它本来带没带 inert,于是这一种同样会被误清)。
    check(
        await evaluate(
            IN(`const marker = d.createElement("div");
                marker.setAttribute(
                    "data-gb",
                    "sl35-pre-existing-inert-marker",
                );
                marker.setAttribute("inert", "");
                const card = gb("card");
                if (!card) return false;
                card.appendChild(marker);
                return true;`),
        ),
        "在点「开始」前插了一个自带 inert 的节点(模拟 tour 开始前就已置位的那一种)",
    );
    check(
        await evaluate(
            IN(
                `const b = gb("tour-ask-start"); if (!b) return false; b.click(); return true;`,
            ),
        ),
        "点到了「开始」",
    );
    check(
        await waitFor(
            IN(`const o = d.querySelector("[data-tour-overlay]");
                return !!o && o.hidden === false;`),
            6000,
        ),
        "tour 蒙版已上屏(isActive() 变 true)",
    );
    // ---- SL-33 的核心一位:上面那句只证明"tour 激活了",不证明"画面跟上了" ----
    check(
        await waitFor(
            IN(`const n = gb("header-conn-count");
                return !!n && n.textContent === "15/15";`),
            5000,
        ),
        "header-conn-count 在 5s 内变成 15/15(demo 的 FIFTEEN_TRACKS 上屏了;" +
            "旧代码这里会一直卡在 0/15 —— isActive() 已经是 true,但从没有一次 render() 真正读过 demoStore())",
    );
    assertClean("② demo 数据上屏");

    // =========================================================================
    log(
        "=== ③ tour 激活期间插入一个与 tour 无关的 inert 节点,Skip 结束(SL-35) ===",
    );
    check(
        await evaluate(
            IN(`const marker = d.createElement("div");
                marker.setAttribute("data-gb", "sl35-unrelated-inert-marker");
                marker.setAttribute("inert", "");
                const card = gb("card");
                if (!card) return false;
                card.appendChild(marker);
                return true;`),
        ),
        "插入了一个自带 inert、与 tour 无关的新节点(模拟别处逻辑独立置的 inert)",
    );
    // header 是 start() 自己置过 inert 的原生子节点(回归检查:收窄范围后它仍要被正常释放)。
    eq(
        await evaluate(
            IN(
                `const h = gb("header"); return h ? h.getAttribute("inert") : null;`,
            ),
        ),
        "",
        "tour 激活期间 header 确实被 start() 置了 inert(先确认置位这一半还在,不是本来就没做)",
    );
    check(
        await evaluate(
            IN(`const btn = d.querySelector('[data-tour-btn="skip"]');
                if (!btn) return false;
                btn.click();
                return true;`),
        ),
        "点到了 Skip",
    );
    check(
        await waitFor(
            IN(`const o = d.querySelector("[data-tour-overlay]");
                return !!o && o.hidden === true;`),
            6000,
        ),
        "tour 蒙版已收起(endTour 跑完)",
    );
    // ---- SL-35 的核心三位(两种触发时刻各一位 + header 回归一位)----
    eq(
        await evaluate(
            IN(`const m = gb("sl35-unrelated-inert-marker");
                return m ? m.getAttribute("inert") : null;`),
        ),
        "",
        "无关节点(tour 中途插入那一种)的 inert **仍然在**(endTour 不该盲扫 " +
            "card.children 把它也摘掉;旧代码这里会变成 null —— getAttribute 在属性" +
            "不存在时才回 null,直接判等即可)",
    );
    eq(
        await evaluate(
            IN(`const m = gb("sl35-pre-existing-inert-marker");
                return m ? m.getAttribute("inert") : null;`),
        ),
        "",
        "无关节点(tour 开始前就已置位那一种)的 inert **仍然在**(复审【重要】① 指出" +
            "的那一半:start() 不该把「当时碰到的」全收进 inertedChildren,只该收" +
            "「自己从无到有置上的」;旧的半修复这里会变成 null)",
    );
    eq(
        await evaluate(
            IN(
                `const h = gb("header"); return h ? h.getAttribute("inert") : null;`,
            ),
        ),
        null,
        "header(start() 自己置过 inert 的原生子节点)已被正常释放(回归:收窄范围没有连它一起漏)",
    );
    check(
        await waitFor(
            IN(`const n = gb("header-conn-count");
                return !!n && n.textContent === "0/15";`),
            5000,
        ),
        "tour 结束后 viewStore() 切回真实 store,header-conn-count 在 5s 内变回 0/15" +
            "(endTour 末尾那句 requestRender() 走 rAF,故不直接同步 evaluate)",
    );
    assertClean("③ inert 收尾");
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
console.log(
    "\n✅ Output tour demo 上屏(SL-33)+ inert 收窄(SL-35)页面级冒烟全绿",
);
process.exit(0);
