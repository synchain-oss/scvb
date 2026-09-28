// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Input 认领「非冲突失败」反馈的页面级冒烟([SL-463] / J156)
// =============================================================================
// 缺陷:契约 §3.2 `setChannelId` / §3.3 `setGroupId` 的返回并集此前只有 `{ok:true}` |
// `{conflict:true}`,真桥把 abi 不符 / 共享内存段打不开这两种失败都回成 `{ok:true}`。界面上:
// abi 不符至少还有红 pill + 横幅;段打不开只剩一个灰 pill「未选择通道」,换通道时补偿式回滚
// 成功则连它都没有 —— 用户点了卡,什么都没发生。J156 把并集扩成加上 `{ok:false, reason}`,
// Input 页据此抖卡 / 抖胶囊并弹一条说明原因的 toast。
//
// 为什么必须页面级:要钉的是 web/input/app.js 里「拿到回执 → 抖哪个节点、弹哪条词条」这段 DOM
// 接线;node 侧的 smoke-input.mjs ⑥ 只走到 mock 的返回值,碰不到它。
//
// 跑什么(同一条会话上依次开三个场景):
//   ① ?scenario=claim-unavailable 首帧:开箱已接管 ch2;
//   ② 点卡 5 ⇒ 回执 unavailable:toast 上屏、词条与文案逐字对、卡 5 抖过;选中仍是卡 2
//      (补偿式回滚:会话留在原通道;界面没有乐观态可回滚,也不许把卡 5 显示成选中);
//   ③ 切到组 B 并确认 ⇒ 回执 unavailable:toast 上屏、胶囊 B 抖过;状态帧到了之后
//      「未分配」卡选中(组号已换,新组里一个 slot 也没持住);
//   ④ ?scenario=claim-abi-mismatch:点卡 4 ⇒ 回执 abiMismatch:toast 上屏、卡 4 抖过,
//      状态帧到了之后 pill 为「版本不匹配」、abi 横幅两端数字都填上;
//   ⑤ 反向对照 fixture=empty:成功的点卡**不弹** toast(否则「无条件弹」也能让 ②-④ 全绿)。
//   每段零 console.error、零未捕获异常。
//
// 删除式(未提交,人工核过,读数见 PR 描述):
//   · 删 app.js claimChannel() 里 [SL-463] 那段 ⇒ ② ④ 红(③ 与 ⑤ 仍绿);
//   · 删 app.js 切组确认里 [SL-463] 那段 ⇒ 只有 ③ 红;
//   · 把 claimFailedKey() 的 `res.ok !== false` 判据去掉(成功回执也映射成词条)⇒ 只有 ⑤ 红
//     ——(做法:让它对 `{ok:true}` 也返回 unavailable 的词条)。
//
// 用法:node web-preview/tests/smoke-input-claim-failure-page.mjs [仓库根绝对路径]
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
// 判据对词条真源,不在本文件手抄中文(手抄的那句会随 U17 审校漂走)。
import { T } from "../../web/shared/i18n.js";

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
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-input-claimfail-"));
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
    const card = (ch) => q('[data-gb="input.channels.card"][data-ch="' + ch + '"]');
    ${js}
})()`;

// 页面已经渲染过一轮的判据:16 张通道卡(0..15)已由 buildChannelGrid 建出来。
const READY = IN(`
    const cards = d.querySelectorAll('[data-gb="input.channels.card"]');
    return cards.length === 16;
`);

async function open(query) {
    newBucket(query);
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/input.html?${query}`,
    });
    const ok = await waitFor(READY);
    check(ok, `${query}:页面装载并建出通道网格`);
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

    // 点击前在某个节点上挂 data-shake 闩锁 —— 读闩锁不读此刻的属性值(shake() 在 animationend
    // 就把属性摘掉,.45s;与 smoke-input-conflict-page.mjs ③ 同一手法,理由见那里)。
    const LATCH = (sel, flag) =>
        IN(`const n = ${sel};
            if (!n) return false;
            window[${JSON.stringify(flag)}] = false;
            new (w.MutationObserver)(() => {
                if (n.getAttribute("data-shake") === "1") window[${JSON.stringify(flag)}] = true;
            }).observe(n, { attributes: true, attributeFilter: ["data-shake"] });
            return true;`);
    // toast 复位:同一个 toast 槽 4s 才自动收起,前一步留下的那条会让下一步的「上屏」判据空转。
    const RESET_TOAST = IN(`const t = gb("input.toast.occupied");
        const x = gb("input.toast.occupied.text");
        if (!t || !x) return false;
        t.hidden = true;
        x.textContent = "";
        x.removeAttribute("data-t");
        return true;`);
    const TOAST_SHOWN = IN(`const t = gb("input.toast.occupied");
        return !!t && t.hidden === false;`);
    const toastState = () =>
        evaluate(
            IN(`const t = gb("input.toast.occupied");
                const x = gb("input.toast.occupied.text");
                return {
                    hidden: t ? t.hidden : null,
                    key: x ? x.getAttribute("data-t") : null,
                    text: x ? x.textContent : null,
                };`),
        );
    const textsOf = (key) => [T.zh, T.en, T.fr].map((d) => String(d[key]));
    const PRESSED = (ch) =>
        IN(`const c = card(${ch});
            return !!c && c.getAttribute("aria-pressed") === "true";`);

    // =========================================================================
    log("=== ① ?scenario=claim-unavailable:开箱 ch2 已接管 ===");
    await open("scenario=claim-unavailable");
    check(await waitFor(PRESSED(2), 6000), "① 卡 2 显示为选中(开箱态)");
    assertClean("① 首帧");

    // =========================================================================
    log(
        '=== ② 点卡 5 ⇒ 回执 {ok:false, reason:"unavailable"}:抖卡 + 说明 toast,选中仍是卡 2 ===',
    );
    check(
        await evaluate(LATCH("card(5)", "__sl463Card5")),
        "② 在卡 5 上挂好 data-shake 闩锁(点击前)",
    );
    check(
        await evaluate(
            IN(
                `const c = card(5); if (!c) return false; c.click(); return true;`,
            ),
        ),
        "② 点到了卡 5",
    );
    check(
        await waitFor(TOAST_SHOWN, 6000),
        "② toast 在 6s 内上屏(此前真桥回 {ok:true},界面什么都不弹)",
    );
    {
        const st = await toastState();
        eq(
            st.key,
            "ch.claimFailed.unavailable",
            "② toast 词条 = ch.claimFailed.unavailable",
        );
        check(
            textsOf("ch.claimFailed.unavailable").includes(st.text),
            `② toast 文案逐字等于该词条(实得 ${JSON.stringify(st.text)})`,
        );
    }
    check(
        await waitFor(IN(`return window.__sl463Card5 === true;`), 6000),
        "② 卡 5 曾经拿到过 data-shake=1",
    );
    // 补偿式回滚:会话留在原通道 ⇒ 选中仍是卡 2,卡 5 不被显示成选中(界面没有乐观态可回滚)。
    await sleep(600); // 等过 mock 的一拍状态回声(250ms);回滚这一支本来就不该有任何状态帧
    check(
        (await evaluate(PRESSED(2))) === true &&
            (await evaluate(PRESSED(5))) === false,
        "② 选中仍是卡 2,卡 5 未被显示成选中",
    );
    assertClean("② 点卡失败");

    // =========================================================================
    log(
        '=== ③ 切到组 B 并确认 ⇒ 回执 {ok:false, reason:"unavailable"}:抖胶囊 + 说明 toast ===',
    );
    check(await evaluate(RESET_TOAST), "③ toast 复位(不让 ② 那条顶替本步)");
    check(
        await evaluate(
            IN(`const p = gb("input.group.pills");
            const b = p && p.querySelector('[data-group="2"]');
            if (!b) return false;
            b.click();
            return true;`),
        ),
        "③ 点到了组胶囊 B",
    );
    check(
        await waitFor(
            IN(`const c = gb("input.group.confirm");
            return !!c && c.hidden === false;`),
            6000,
        ),
        "③ 切组确认条已展开",
    );
    check(
        await evaluate(
            LATCH(
                `gb("input.group.pills") && gb("input.group.pills").querySelector('[data-group="2"]')`,
                "__sl463PillB",
            ),
        ),
        "③ 在组胶囊 B 上挂好 data-shake 闩锁(确认前)",
    );
    check(
        await evaluate(
            IN(`const b = gb("input.group.confirm.primary");
            if (!b) return false;
            b.click();
            return true;`),
        ),
        "③ 点到了「切换到 B」",
    );
    check(await waitFor(TOAST_SHOWN, 6000), "③ toast 在 6s 内上屏");
    {
        const st = await toastState();
        eq(
            st.key,
            "ch.claimFailed.unavailable",
            "③ toast 词条 = ch.claimFailed.unavailable",
        );
        check(
            textsOf("ch.claimFailed.unavailable").includes(st.text),
            `③ toast 文案逐字等于该词条(实得 ${JSON.stringify(st.text)})`,
        );
    }
    check(
        await waitFor(IN(`return window.__sl463PillB === true;`), 6000),
        "③ 组胶囊 B 曾经拿到过 data-shake=1",
    );
    // 组号已换、新组里一个 slot 也没持住 ⇒ 状态帧到了之后「未分配」卡显示为选中(channel_id 0)。
    check(
        await waitFor(PRESSED(0), 6000),
        "③ 状态帧到达后「未分配」卡显示为选中(channel_id 0)",
    );
    assertClean("③ 切组失败");

    // =========================================================================
    log(
        "=== ④ ?scenario=claim-abi-mismatch:点卡 4 ⇒ 回执 abiMismatch + 红 pill + 横幅 ===",
    );
    await open("scenario=claim-abi-mismatch");
    check(await waitFor(PRESSED(0), 6000), "④ 开箱「未分配」卡选中");
    check(
        await evaluate(LATCH("card(4)", "__sl463Card4")),
        "④ 在卡 4 上挂好 data-shake 闩锁(点击前)",
    );
    check(
        await evaluate(
            IN(
                `const c = card(4); if (!c) return false; c.click(); return true;`,
            ),
        ),
        "④ 点到了卡 4",
    );
    check(await waitFor(TOAST_SHOWN, 6000), "④ toast 在 6s 内上屏");
    {
        const st = await toastState();
        eq(
            st.key,
            "ch.claimFailed.abiMismatch",
            "④ toast 词条 = ch.claimFailed.abiMismatch",
        );
        check(
            textsOf("ch.claimFailed.abiMismatch").includes(st.text),
            `④ toast 文案逐字等于该词条(实得 ${JSON.stringify(st.text)})`,
        );
    }
    check(
        await waitFor(IN(`return window.__sl463Card4 === true;`), 6000),
        "④ 卡 4 曾经拿到过 data-shake=1",
    );
    check(
        await waitFor(
            IN(`const p = gb("input.header.pillText");
            return !!p && p.getAttribute("data-t") === "in.pill.abiMismatch";`),
            6000,
        ),
        "④ 状态帧到达后 pill 为「版本不匹配」",
    );
    check(
        await waitFor(
            IN(`const b = gb("input.banner.abiMismatch");
            const x = gb("input.banner.abiMismatch.text");
            return !!b && b.hidden === false && !!x && !/undefined|\\{[ab]\\}/.test(x.textContent);`),
            6000,
        ),
        "④ abi 横幅上屏,两端 abi 数字都已填上(没有 undefined / 未替换的占位符)",
    );
    check((await evaluate(PRESSED(4))) === false, "④ 卡 4 未被显示成选中");
    assertClean("④ abi 不符");

    // =========================================================================
    // 反向对照:成功的点卡**不许**弹这条 toast —— 否则「无条件弹」也能让 ②-④ 全绿。
    log("=== ⑤ 对照:fixture=empty(未分配、无占用)点卡 4 成功 ⇒ 不弹 toast ===");
    await open("fixture=empty");
    check(await waitFor(PRESSED(0), 6000), "⑤ 开箱「未分配」卡选中");
    check(
        await evaluate(
            IN(
                `const c = card(4); if (!c) return false; c.click(); return true;`,
            ),
        ),
        "⑤ 点到了卡 4",
    );
    check(
        await waitFor(PRESSED(4), 6000),
        "⑤ 状态帧到达后卡 4 显示为选中(点卡成功)",
    );
    {
        const st = await toastState();
        check(
            st.hidden === true,
            `⑤ 成功时 toast 保持隐藏(实得 ${JSON.stringify(st)})`,
        );
    }
    assertClean("⑤ 对照");
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
console.log("\n✅ Input 认领非冲突失败反馈(SL-463 / J156)页面级冒烟全绿");
process.exit(0);
