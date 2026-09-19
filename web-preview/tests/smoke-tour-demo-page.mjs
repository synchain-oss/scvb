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
//   ② 先轮询到 `#card` 子树的渲染观测安静下来(mock 驱动的一次性差分已落定,
//      理由见调用点注释),再插一个 tour **开始前**就带 `inert` 的节点,然后点
//      「tour-ask-start」进入 tour:蒙版可见;`header-conn-count` 在几秒内变成
//      "15/15"(demo 的 FIFTEEN_TRACKS 15 轨全部健康在线)—— 这一位只能来自
//      **真的跑过一次 render()**,不是"isActive() 已经是 true"就有的副作用。
//   ③ tour 激活期间,再往 `#card` 里插一个 tour **中途**插入、与 tour 无关、
//      自带 `inert` 的新节点,然后点 Skip 结束 tour;断言:
//      · ②插的「开始前」节点与③插的「中途」节点,`inert` **都仍然在**
//        (endTour 两种情形都不该动它们);
//      · `start()` 自己置过 inert 的原生子节点(header)**已经**被正常释放
//        (回归检查:收窄范围没有把真正该清的那一半也弄丢);
//      · `header-conn-count` 变回 "0/15"(viewStore() 切回真实 store)。
//   ④ 每段零 console.error、零未捕获异常。
//
// 删除式(未提交,人工核过):
//   · 去掉 `tour.js` `start()` 里新增的那句 `requestRender()`,②段必须转红(卡在
//     "15/15" 等不到,取到的还是 "0/15");
//   · 把 `start()` 里的 `!child.hasAttribute("inert")` 判断去掉(退回「当时碰到的
//     全收」半修复),③段「开始前」那个节点的 `inert` 仍在断言必须转红;
//   · 把 `endTour()` 的清 inert 循环改回 `for (const child of card.children)
//     child.removeAttribute("inert")`,③段两个节点的 `inert` 仍在断言必须转红。
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
    // [复审【重要】×2] 点「开始」前必须让 mock 驱动的周期帧(scvb.conn/scvb.groups
    // 的 `emitIfChanged`)把它们**唯一一次**"从空基线到真实载荷"的差分发完并稳定
    // 下来 —— 那次差分会触发一次不相干的 `requestRender()`,如果它落在点击**之后**,
    // 没有 SL-33 那个修复也会把画面"顺带"刷成 demo 数据(假绿,本仓最反对的方向)。
    // 旧版在这里写死 `sleep(4000)`,与 CLAUDE.md §10「别写死 sleep,要轮询」冲突,
    // 而且是**经验常数**:headless Chrome 对后台/无焦点标签页的定时器有节流,实测
    // 跨导航能把这次差分的到达时刻从 <300ms 飘到 >3s,机器更慢一档或 mock 的周期
    // 常数一改,这个常数会重新变回随机假阳性;机器快时又白等,而这一段是持锁的
    // (`Local\SCVB-ipc-tests`),白等的时间记在全机预算里。
    // 改成轮询一个**只由那次差分驱动**的可观测量:往 `#card` 子树挂
    // MutationObserver,`renderHeader()` 每次 render() 都无条件重写
    // `header-conn-count.textContent`(即便值不变,`textContent` 赋值本身就是一次
    // childList 变更)⇒ 观察到的每条 mutation 都对应一次真实 render() —— 轮询到
    // "最近一次 mutation 之后已经安静了 SETTLE_MS"再点「开始」,语义与旧注释一模
    // 一样(等一次性差分落定),但换成了自适应条件:快机器几百毫秒就点,慢机器/
    // 节流更狠也照样等得到,不再拿一个固定墙钟时长去赌。
    const SETTLE_MS = 700;
    check(
        await evaluate(
            IN(`const c = gb("card");
                if (!c) return false;
                window.__sl33Mut = { count: 0, lastAt: performance.now() };
                new (w.MutationObserver)(() => {
                    window.__sl33Mut.count++;
                    window.__sl33Mut.lastAt = performance.now();
                }).observe(c, {
                    childList: true,
                    subtree: true,
                    characterData: true,
                    attributes: true,
                });
                return true;`),
        ),
        "在 #card 子树挂好渲染观测(点「开始」前)",
    );
    check(
        await waitFor(
            IN(`const s = window.__sl33Mut;
                return !!s && performance.now() - s.lastAt > ${SETTLE_MS};`),
            12000,
        ),
        `渲染在 ${SETTLE_MS}ms 内安静下来(一次性差分已落定;12s 内没安静判超时,` +
            "不当作通过)",
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
