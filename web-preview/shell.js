// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// web-preview/shell.js —— 壳页注入引擎(T28)
// -----------------------------------------------------------------------------
// 【本文件的唯一职责】把 web-preview 造出来的 mock 后端送进**真源页面的 window**,
//   让 web/{output,input}/app.js 里那句
//       createBridge({ role, mockBackend: window.__SCVB_MOCK__ })
//   在浏览器预览里取到值。除此之外只做一条预览工具条(返回导航 / 重载 / 注入状态)。
//
// 【为什么必须是「注入」而不是别的】06 §6.2 硬约束 1:`web/` 是唯一真源,
//   **严禁在 web-preview/ 里复制任何 UI 代码**。因此:
//     ✗ fetch 真源 HTML 再 document.write / srcdoc —— 复制运行时 DOM,出局;
//     ✗ 改 web/ 让它认识 preview —— 真源不该知道预览器存在,且 T27b 已交付冻结;
//     ✓ 同源 <iframe src="../web/output/index.html"> + 把 mock 挂到 iframe 的 window。
//   壳页与 iframe 同源(同一个静态服务器),对象可以跨 realm 直接共享:driver 在壳页
//   realm 里持有定时器与事件回调,bridge.js 在 iframe realm 里调用同一个对象的方法。
//
// 【时序问题(本文件的全部难点)】
//   真源页面把 app.js 写成 <script type="module" src="./app.js">,module script 恒为
//   deferred:**解析完成后**才求值。所以「注入窗口」= 新文档 commit ~ app.js 求值,
//   中间隔着 index.html 解析 + app.js 及其 4 个 import(bridge/i18n/design-box/mock-data)
//   的模块图抓取(至少两轮网络往返)。同源 iframe 与壳页共用一个 event loop,
//   壳页在这段时间里能拿到大量 task tick —— 注入引擎就住在这些 tick 里。
//
//   ▸ 方案 A(简报定案的第一路):先 `frame.contentWindow.__SCVB_MOCK__ = mock`,再赋 `frame.src`。
//     **假设**:同源导航后 initial about:blank 的全局属性得以保留。
//     **规范推定**:不保留 —— 导航 commit 会为新文档新建 global object,WindowProxy 虽复用
//     但不继承旧 global 上的属性。
//     **实测(T28 写手,Chromium 140 / 本机 Chrome,同源 http://127.0.0.1)**:与推定一致 ——
//     导航前写入成功(读回为 'kept'),iframe load 后同一属性读回 `null`。方案 A 因此**不作依赖**,
//     只当零成本先手保留(万一某宿主 WebView 真保留就直接成)。
//     ※ 逐 fixture 的真浏览器目视验收另由统筹会话完成,与本条时序实测是两件事。
//
//   ▸ 方案 B(实际生效的一路):**重注入泵**。赋 src 之后立刻开一条 MessageChannel 任务链
//     (比 setTimeout(0) 的 4ms 钳位快得多,几乎逐 task 触发),每个 tick 检查
//     `frame.contentWindow.__SCVB_MOCK__`,缺了就补。新文档一 commit,下一个 tick 就把
//     mock 补进新 global;此时页面还在解析 / 还在抓模块图,app.js 尚未求值。
//     一旦观测到「注入进的是目标文档(contentDocument.URL !== about:blank)」即停泵——
//     此后不再有导航,属性不会再被清掉,继续空转只是烧 CPU。
//
//   ▸ **成功判据不是纸面的**:壳页注入的是 `Object.create(session.mock)` 派生的探针对象,
//     只覆盖 addEventListener 做计数。bridge.js 的 wireEvents() 会为本侧每个事件名调用一次
//     addEventListener(output 9 / input 5),所以 iframe load 之后 `wired > 0`
//     ⇔ createBridge 真的接上了我们注入的 mock。wired === 0 ⇔ 注入迟到/失败。
//
//   ▸ 失败时的真实回退(不是纸面回退):
//     1) 自动重试一次(带 cache-bust query,配合 serve.ps1 的 Cache-Control: no-store,
//        强制模块图重新走网络 → 注入窗口重新变宽);
//     2) 方案 C(统筹批准,默认关闭):`pwsh web-preview/serve.ps1 -Inject` —— 服务器在
//        **传输途中**往真源 HTML 的 </head> 前塞一段 classic script,从
//        `window.parent.__SCVB_PREVIEW_MOCK__` 取件。classic script 在解析期同步执行,
//        恒早于 deferred 的 app.js,时序确定不靠泵;磁盘上的 web/ 一个字节没动,
//        也没有任何 UI 代码被复制。代价:预览只能经 serve.ps1 -Inject 打开;
//     3) 仍失败 ⇒ 工具条转红 + console.error 打出诊断(注入次数 / 是否 commit 后注入 /
//        readyState 快照),并提示按纪律走 deviations 申请「web/ 侧一行」——
//        **壳页绝不自行改 web/**。
//
// 【已知硬前提】必须经 HTTP 静态服务器打开(serve.ps1)。file:// 下 Chromium 把每个文件
//   当不透明源,iframe 变跨源,contentWindow 属性写入直接 SecurityError;而且 ES module
//   在 file:// 下本来就被 CORS 拒绝。README 有同款告示。
// =============================================================================

import { DESIGN } from "../web/shared/design-box.js";
import { MONITOR_SCENARIOS } from "./mock/monitor-mock.js";

/** 真源页面路径 —— web-preview 对 web/ 的**唯一**依赖形式:引用,不复制。 */
const TARGET_PAGE = {
    output: "../web/output/index.html",
    input: "../web/input/index.html",
    monitor: "../web/monitor/index.html",
};

/**
 * 各侧的 mock 模块。Output/Input 共用 state-driver;**Monitor 另起一份** ——
 * 它的数据面(viz 段)与那两侧没有一个字段重合,塞进同一个 driver 要在十几处
 * 分叉(理由详见 `./mock/monitor-mock.js` 头注)。两者导出同一个
 * `createPreviewSession({role, params})`,故下面的装载逻辑一份通用。
 */
const MOCK_MODULE = {
    output: "./mock/state-driver.js",
    input: "./mock/state-driver.js",
    monitor: "./mock/monitor-mock.js",
};

/** 各侧设计盒(iframe 尺寸 = 它 × 当前档位;唯一真源 `web/shared/design-box.js`,壳页不写死数字)。 */
// [T46] Monitor 那一条曾经取自页面侧的 `monitor-box.js`(当时 shared 里还没有 monitor 键,
// 加键要同批动 gen-design-box.py / check-design-box.mjs / BridgeBase.h,而那四处属 T45)。
// T45(PR #94)合入后三侧同源,这里不再多引一个模块。
const DESIGN_BOX = {
    output: DESIGN.output,
    input: DESIGN.input,
    monitor: DESIGN.monitor,
};

/** 契约 §2/§4 的事件条数,仅用于工具条上「已接线 n/N」的自检显示。 */
const EXPECTED_EVENT_COUNT = { output: 9, input: 5, monitor: 4 };

/** 泵的硬上限:超时即判失败,免得某天真源改成同步脚本时壳页无声空转。 */
const PUMP_DEADLINE_MS = 15000;

// -----------------------------------------------------------------------------
// 工具条元信息的**白名单**。
// fixture / scenario / loop 三个值来自 location.search,是外部可控输入;工具条
// 只用 textContent 拼装(不碰 innerHTML),再加一层白名单——表外取值一律显示为
// `unknown`,既挡掉注入面,也顺手把「参数拼错了」变成肉眼可见的信号而不是静默回落。
// 名单真源:六 fixture 见任务卡 / 简报 fixture 表;scenario 名见 05 §2.5(Output)与 §3(Input)。
// -----------------------------------------------------------------------------
const FIXTURE_NAMES = [
    "empty",
    "fifteen-tracks",
    "misaligned",
    "channel-conflict",
    "second-output",
    "stereo-mixed",
];

const SCENARIO_NAMES = {
    output: [
        "misaligned",
        "conflict",
        "empty",
        "stale",
        "printing",
        "newer-state",
        "sidecar-missing",
        "no-timeline",
        // [SL-247 / J92a] 布防在、采集关 ⇒ 横幅 ⑩(见 state-driver 的 SCENARIO_MAP)。
        "recapture-voided",
        "project-copy",
        "sidecar-switched",
        "low-sample",
        "print-guard",
        "first-run",
        "recapture-armed",
        "first-run-tour",
        "group-switch",
        // 05 正文之外、由实施卡引入的演示场景(SCENARIO_MAP 里确有,不是「参数拼错了」):
        "curve-editor", // T34 曲线编辑器(非零 ms_balance,叠加线可见)
        "hot-levels", // [SL-280] 若干轨 vol 顶到 0 dB 及以上(柱高映射回归场景)
        "chart-trajectory", // T43([J75] A)分布图轨迹档 + 断线缺口
        "loudness-nondefault", // [SL-276 复审] 工程存的响度口径非默认档(rms)
        // [SL-354] 下面五个是**补登**:它们早就在 `SCENARIO_MAP` 里、页面级冒烟也一直在
        // 用,只是历次实施卡各自漏了往这张白名单里登记一笔。两张表不齐的后果 =
        // 壳页工具条把场景名印成 `scenario=unknown`(`allowedOr` 对表外值的字面输出),
        // 「参数拼错了」那条肉眼信号就此失灵 —— `smoke-output-dist-page` 那一格记的正是
        // 这两种漏法(它自己只钉了 `curve-editor` 一个名字,钉不住其余的)。
        "loudness-stale-on-load", // [SL-276 复审] 装载即 stale(基线读的是 applied 不是本地快照)
        "range-manual", // [SL-279 复审第 6 轮] 范围档装载:重新分析受理但基线不前移
        "diff-flood", // [SL-274] diff 摘要顶到 changed[] 封顶(200)的那一帧
        "slow-state-echo", // [SL-354] 真桥时序:写回执先到、scvb.state 后到一拍
        "applied-echo-drop", // [SL-354] 写落地后补一帧缺 analysis.applied 的全量快照
        "sync-state-echo", // [SL-357] 同步回声逃生口(默认异步之后的旧语义)
    ],
    input: [
        "occupied",
        "no-output",
        "connected",
        "passthrough",
        "abi-mismatch",
        "sr-mismatch",
        "group-mismatch",
        // 05 §3 文末 J80 节引入:T48 Input 首启轻量引导(语言卡 → 5 步 mini tour)
        "input-first-run",
    ],
    // [T46] Monitor 侧的演示场景;名单真源 = ./mock/monitor-mock.js 的
    // MONITOR_SCENARIOS(此处引用而不是抄一份,免得两处漂开)。
    monitor: MONITOR_SCENARIOS,
};

// URL 上只接受 `loop=none`;driver 会把「宿主提供循环区」的常态归一成 `host` 回在
// info.loop 里,故白名单两值都收,别的一律 unknown。
const LOOP_VALUES = ["none", "host"];

/** 白名单过滤:空值回 null(不显示这一格),表外值回 "unknown"。 */
function allowedOr(value, allowed) {
    if (value === null || value === undefined || value === "") return null;
    return allowed.includes(value) ? value : "unknown";
}

/** 往工具条里追加一格 `key=<code>value</code>`,全程 textContent,不拼 HTML。 */
function appendMetaField(parent, key, value) {
    if (value === null) return;
    if (parent.childNodes.length > 0) {
        parent.appendChild(document.createTextNode(" · "));
    }
    parent.appendChild(document.createTextNode(key + "="));
    const code = document.createElement("code");
    code.textContent = value;
    parent.appendChild(code);
}

/** 壳页工具条样式 —— 预览家具,不是产品 UI。
 *  故意**不**引 web/shared/tokens.css:工具条必须一眼看出「不属于插件画面」,
 *  否则目视验收时会把壳页家具误当灰模的一部分。零 hex 字面量(用 rgb()),
 *  免得撞上 web/ 侧「无裸 hex」的 grep 口径。 */
const SHELL_CSS = `
html, body { margin: 0; height: 100%; }
body {
    display: flex;
    flex-direction: column;
    background: rgb(18 18 20);
    color: rgb(238 238 242);
    font: 13px/1.5 system-ui, "Segoe UI", sans-serif;
}
.pv-bar {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 12px;
    padding: 6px 12px;
    background: rgb(32 32 38);
    border-bottom: 1px solid rgb(72 72 82);
}
.pv-bar a { color: rgb(150 202 255); }
.pv-bar button {
    font: inherit;
    color: rgb(238 238 242);
    background: rgb(58 58 68);
    border: 1px solid rgb(104 104 118);
    border-radius: 4px;
    padding: 2px 10px;
    cursor: pointer;
}
.pv-bar code { font-family: ui-monospace, Consolas, monospace; }
.pv-status[data-ok="1"] { color: rgb(126 226 150); }
.pv-status[data-ok="0"] { color: rgb(255 146 146); }
.pv-status[data-ok="wait"] { color: rgb(240 214 128); }
.pv-warn { color: rgb(240 214 128); }
.pv-warn:empty { display: none; }
.pv-stage { flex: 1 1 auto; min-height: 0; overflow: auto; }
.pv-stage iframe { display: block; border: 0; background: rgb(18 18 20); }
`;

/**
 * 注入引擎(可独立测试的那一半:session 由调用方给)。
 *
 * @param {object}   o
 * @param {"output"|"input"} o.role
 * @param {HTMLIFrameElement} o.frame  **不带 src** 的 iframe(处在 initial about:blank)
 * @param {string}   o.targetUrl       真源页面 URL
 * @param {object}   o.session         { mock, start(), stop(), info } —— 见 mock/state-driver.js
 * @param {(s:{ok:boolean|null,text:string})=>void} [o.onStatus] 状态回调(工具条/探针共用)
 * @param {(f:number)=>void} [o.hostResize] **壳页扮演宿主**的那一半:档位被接受之后按
 *   设计盒 × 档位改 iframe 尺寸,与原生 `WebViewHost::setUiScale → resizeToDesignBox`
 *   逐条对应([SL-380])。缺省不接 —— 桩 session 的单测不需要一个真 iframe。
 * @param {number}   [o.maxRetries=1]  wired===0 时的自动重试次数
 * @returns {Promise<object>} 诊断对象(wired / injectCount / committedInject / …)
 */
export function injectAndMount({
    role,
    frame,
    targetUrl,
    session,
    onStatus = () => {},
    hostResize = null,
    maxRetries = 1,
}) {
    // ---- 探针:非侵入式包一层 addEventListener,用来判定「bridge 是否真的接上了它」----
    // 用 Object.create 而不是改写 session.mock 本体:bridge.js 的
    // `typeof mock[name] === "function"` 走原型链一样成立,driver 手里的对象则毫发无伤。
    let wired = 0;
    const probeMock = Object.create(session.mock);
    probeMock.addEventListener = function (name, cb) {
        wired += 1;
        return session.mock.addEventListener(name, cb);
    };

    // ---- 壳页 = 宿主([SL-380])-------------------------------------------------
    // 真机上「档位」两件事各有其主:mock 后端记 `state.ui.scale`(那是 Processor 的活),
    // **窗口尺寸由宿主改**(`WebViewHost::setUiScale → setSize(设计盒 × F)`)。壳页里的
    // 「窗口」就是这个 iframe,所以这一层必须由壳页补上 —— 否则预览里改档位只动数字不动
    // 视口,而页面的倍率是从视口反算的,档位在预览里就成了哑的(而真机上是活的)。
    // 拒绝态(不在档位表 → `{ok:false}`)不改尺寸,与原生的 clamp/校验顺序一致。
    if (typeof session.mock.setUiScale === "function" && hostResize) {
        probeMock.setUiScale = function (f) {
            const r = session.mock.setUiScale(f);
            Promise.resolve(r)
                .then((res) => {
                    if (!res || res.ok !== false) hostResize(f);
                })
                .catch(() => {});
            return r; // 同步/异步形态原样透传,不改桥口语义
        };
    }

    // ---- 方案 C(serve.ps1 -Inject)的取件口 ----
    // 服务器在传输途中往真源 HTML 的 </head> 前塞一段 classic script(磁盘上的 web/ 零改动、
    // 零 UI 代码复制),那段脚本从 `window.parent.__SCVB_PREVIEW_MOCK__` 取件。classic script
    // 在解析期同步执行,恒早于 deferred 的 app.js —— 时序确定,不靠泵。默认关闭,
    // 只在 A/B 都被实测判死时才开(README「注入机制」节)。
    window.__SCVB_PREVIEW_MOCK__ = probeMock;

    const diag = {
        role,
        targetUrl,
        wired: 0,
        expectedEvents: EXPECTED_EVENT_COUNT[role],
        injectCount: 0,
        preNavInject: false, // 方案 A 是否写成功(不代表能活过导航)
        committedInject: false, // 方案 B 是否在目标文档上注入成功
        committedReadyState: null, // 注入进目标文档那一刻的 readyState
        pumpTicks: 0,
        retries: 0,
        ok: false,
        error: null,
    };

    let stopPump = () => {};
    let settle;
    const done = new Promise((r) => {
        settle = r;
    });

    /** 单次注入尝试:contentWindow 上缺 mock 就补,并记录补进的是哪个文档。 */
    function tryInject() {
        let w;
        try {
            w = frame.contentWindow;
        } catch (e) {
            // 跨源(file:// 打开,或被反代改了源)—— 注入这条路整个不成立。
            diag.error = String(e);
            return true;
        }
        if (!w) return false;
        if (w.__SCVB_MOCK__ === probeMock) {
            // 已经在当前文档上了。若当前文档就是目标文档,说明注入已落定,可以停泵。
            return diag.committedInject;
        }
        try {
            w.__SCVB_MOCK__ = probeMock;
            diag.injectCount += 1;
            const doc = frame.contentDocument;
            const url = doc ? doc.URL : "";
            if (url && url !== "about:blank") {
                diag.committedInject = true;
                diag.committedReadyState = doc.readyState;
                return true; // 目标文档已拿到 mock,导航已结束,不会再被清掉 → 停泵
            }
        } catch (e) {
            diag.error = String(e);
            return true;
        }
        return false;
    }

    /**
     * 重注入泵(方案 B 本体)。用 MessageChannel 排 task:setTimeout(0) 在嵌套第 5 层起
     * 被钳到 4ms,而 commit→app.js 求值之间可能只有几毫秒;MessageChannel 无钳位,
     * 几乎能吃到每一个 task 间隙。停泵条件:注入落定 / 出错 / 超时。
     */
    function startPump() {
        const chan = new MessageChannel();
        const deadline = Date.now() + PUMP_DEADLINE_MS;
        let stopped = false;
        chan.port1.onmessage = () => {
            if (stopped) return;
            diag.pumpTicks += 1;
            if (tryInject() || Date.now() > deadline) {
                stopped = true;
                chan.port1.close();
                chan.port2.close();
                return;
            }
            chan.port2.postMessage(0);
        };
        chan.port2.postMessage(0);
        return () => {
            stopped = true;
            try {
                chan.port1.close();
                chan.port2.close();
            } catch {
                /* 已关就算了 */
            }
        };
    }

    /** 发起一次装载:方案 A 先手 → 赋 src → 开泵。 */
    function navigate(attempt) {
        diag.committedInject = false;
        diag.committedReadyState = null;
        wired = 0;

        // ---- 方案 A(零成本先手):在 initial about:blank 上先挂一次。
        // Chromium 实测不保留(见文件头),但写一次不花钱,且能覆盖「某 WebView 真保留」的情形。
        try {
            const w0 = frame.contentWindow;
            if (w0) {
                w0.__SCVB_MOCK__ = probeMock;
                diag.preNavInject = true;
            }
        } catch (e) {
            diag.error = String(e);
        }

        // cache-bust 只在重试时加:配合 serve.ps1 的 Cache-Control: no-store,
        // 强制模块图重新走网络,把注入窗口重新拉宽(真源页面不读 search,加参数无副作用)。
        const url =
            attempt === 0
                ? targetUrl
                : `${targetUrl}?_scvbPreviewRetry=${attempt}`;

        stopPump = startPump(); // 先开泵再赋 src:导航是异步的,泵必须已经在跑
        frame.src = url;
    }

    frame.addEventListener("load", () => {
        // about:blank 的初始 load 不算(部分浏览器会先来一发)。
        let href = "";
        try {
            href = frame.contentDocument ? frame.contentDocument.URL : "";
        } catch (e) {
            diag.error = String(e);
        }
        if (!href || href === "about:blank") return;

        stopPump();
        diag.wired = wired;

        if (wired > 0) {
            diag.ok = true;
            onStatus({
                ok: true,
                text: `mock 已注入 · ${wired}/${diag.expectedEvents} 事件已接线`,
            });
            try {
                session.start();
            } catch (e) {
                diag.error = String(e);
                onStatus({ ok: false, text: `driver.start() 抛错:${e}` });
            }
            settle(diag);
            return;
        }

        // ---- 失败路径(真实判定,不是纸面):bridge 没碰过我们的 mock ----
        if (diag.retries < maxRetries) {
            diag.retries += 1;
            onStatus({
                ok: null,
                text: `注入迟到,正在重试(第 ${diag.retries} 次,带 cache-bust)…`,
            });
            try {
                session.stop();
            } catch {
                /* 还没 start 过,忽略 */
            }
            navigate(diag.retries);
            return;
        }

        onStatus({
            ok: false,
            text: "注入失败:createBridge 未拿到 mockBackend(详见 console)",
        });
        console.error(
            "[web-preview] 壳页注入失败 —— 方案 A(导航前挂 contentWindow)与方案 B(重注入泵)均未赶在 " +
                "app.js 求值之前落地。诊断:",
            diag,
            "\n排查顺序:①确认是经 serve.ps1 的 http://127.0.0.1 打开而非 file://;" +
                "②确认 serve.ps1 发了 Cache-Control: no-store;" +
                "③若真源 app.js 已从 module script 改成同步 <script>,注入窗口会消失 —— " +
                "此时按纪律 append scratchpad/t28/deviations.md 提「需改 web/ 一行」申请给统筹,壳页不得自行改 web/。",
        );
        settle(diag);
    });

    onStatus({ ok: null, text: "正在注入 mock…" });
    navigate(0);

    // 页面关掉/刷新时停掉 driver 的定时器,免得 HMR 式反复刷新后留下一堆孤儿 timer。
    addEventListener("pagehide", () => {
        stopPump();
        try {
            session.stop();
        } catch {
            /* 没起过就算了 */
        }
    });

    return done;
}

/**
 * 壳页入口 —— output.html / input.html 各自只写 `mountPreview({ role: "…" })`。
 * 负责:注样式 → 建工具条与 iframe → 起 driver session → 调注入引擎。
 */
export async function mountPreview({ role: pageRole }) {
    const style = document.createElement("style");
    style.textContent = SHELL_CSS;
    document.head.appendChild(style);

    const params = new URLSearchParams(location.search);

    // `?target=` —— 允许一个壳页装载另一侧的真源(简报里点名的 `?target=monitor`)。
    // 默认仍是页面自己声明的那一侧,故既有链接一个不变;表外取值一律忽略
    // (拼错参数就该落回默认档,而不是白屏)。
    const target = params.get("target");
    const role = Object.prototype.hasOwnProperty.call(TARGET_PAGE, target)
        ? target
        : pageRole;

    const statusEl = document.querySelector(".pv-status");
    const warnEl = document.querySelector(".pv-warn");
    const metaEl = document.querySelector(".pv-meta");
    const stage = document.querySelector(".pv-stage");
    const reloadBtn = document.querySelector(".pv-reload");

    function setStatus({ ok, text }) {
        statusEl.textContent = text;
        statusEl.setAttribute("data-ok", ok === null ? "wait" : ok ? "1" : "0");
    }

    // ---- driver session ----------------------------------------------------
    // 接口由统筹裁定(见 README「与 mock/state-driver.js 的接口」):
    // createPreviewSession({role, params}) → { mock, start(), stop(), info }。
    // 只认具名导出,取不到再退 default;不猜第三个名字(猜名字会把「driver 还没写好」
    // 伪装成「注入失败」,排查成本翻倍)。
    let session;
    try {
        const mod = await import(MOCK_MODULE[role]);
        const factory = mod.createPreviewSession ?? mod.default;
        if (typeof factory !== "function") {
            throw new TypeError(
                `${MOCK_MODULE[role]} 未导出 createPreviewSession({role, params})(也无 default 导出)`,
            );
        }
        session = factory({ role, params });
        // [SL-270] 把会话挂到**壳页**窗口上,给页面级冒烟一个走带开关。
        //
        // 为什么挂在这里而不是往真源页面加测试钩子:走带是**宿主**的东西,页面侧只是
        // `scvb.playhead` 的读者;要造「快速起停」就得动宿主,而预览里的宿主就是这个
        // driver。`__SCVB_MOCK__`(= session.mock)只有桥面那些上行函数,没有走带 ——
        // 走带在 `session.ctl.setTransport` 上。
        // 影响面为零:web-preview/ 不进插件包(ResourceProvider 嵌的是 web/),壳页本身
        // 就是开发工具;真源页面一个字节没改。
        window.__SCVB_PREVIEW__ = session;
    } catch (e) {
        setStatus({ ok: false, text: `mock 后端不可用:${e.message}` });
        console.error("[web-preview] 载入 mock/state-driver.js 失败:", e);
        return null;
    }

    // ---- 测试面:把 driver session 挂到壳页 window 上([SL-380])------------
    // 页面级冒烟需要制造「帧流停了、但组还在线」那一段 —— 真机上它由宿主侧的
    // `emitEventIfBrowserIsVisible` 门控造成,而 mock 的场景表里没有这一档
    // (`monitor-stalled` 只冻时刻、事件照发;`monitor-stall-then-gone` 会连带翻成
    // offline,图就不在版面上了)。没有它,「倍率变了要不要显式重绘」这条判据在
    // 25Hz 帧流下**不可分辨** —— 那正是本卡一度据此删错代码的原因。
    // 只挂在壳页(web-preview 本身就是测试装置),真源页面拿不到、也不知道它存在。
    window.__SCVB_PREVIEW_SESSION__ = session;

    // ---- 工具条元信息(白名单 + textContent,理由见 FIXTURE_NAMES 上方注释)----
    const info = session.info || {};
    metaEl.textContent = "";
    appendMetaField(metaEl, "role", role);
    appendMetaField(
        metaEl,
        "fixture",
        allowedOr(info.fixture ?? params.get("fixture"), FIXTURE_NAMES) ??
            "(默认)",
    );
    appendMetaField(
        metaEl,
        "scenario",
        allowedOr(
            info.scenario ?? params.get("scenario"),
            SCENARIO_NAMES[role],
        ),
    );
    appendMetaField(
        metaEl,
        "loop",
        allowedOr(info.loop ?? params.get("loop"), LOOP_VALUES),
    );
    warnEl.textContent = (info.warnings || []).join(" / ");

    // ---- iframe = 宿主窗口:尺寸**就是**设计盒 × 档位([SL-380])--------------
    // 从前这里给的是「100% + 设计盒当最小值」,于是浏览器窗口一大,iframe 就比设计盒大
    // 一圈。真机上不存在那种视口:`WebViewHost` 把 WebView 铺满编辑器,而编辑器恰是
    // `setSize(设计盒 × 档位)`。页面的倍率现在是从视口反算的(web/shared/shell-fit.js),
    // 视口对不上就等于预览与插件的画面倍率对不上 —— 故这里改成实打实的设计盒尺寸,
    // 档位变化时由 hostResize 同步(数字仍全部来自 web/shared/design-box.js)。
    // 视口装不下时照旧由 .pv-stage 滚动,真源的布局一行不动。
    const box = DESIGN_BOX[role];
    const frame = document.createElement("iframe");
    frame.title = `SCVB ${role} 灰模预览(真源 web/${role}/index.html)`;
    const sizeFrame = (f) => {
        const s = typeof f === "number" && Number.isFinite(f) && f > 0 ? f : 1;
        frame.style.width = Math.round(box.w * s) + "px";
        frame.style.height = Math.round(box.h * s) + "px";
    };
    // `?scale=` —— **开窗时就不是 1 档**。这不是为测试造的场景:`commitUiScale` 会把档位
    // 落成系统级默认,于是用户存过 0.5 之后,下一次开窗宿主**一上来**给的就是设计盒×0.5。
    // 预览从前只会以 1 档开窗,这条路径整个进不来。取值必须在该侧的档位表内,表外一律回落 1
    // (拼错参数该落回默认档,不是白屏)。
    const askedScale = Number(params.get("scale"));
    const initialScale = (DESIGN_BOX[role].presets || []).includes(askedScale)
        ? askedScale
        : 1;
    sizeFrame(initialScale);
    stage.appendChild(frame);

    reloadBtn.addEventListener("click", () => location.reload());

    return injectAndMount({
        role,
        frame,
        targetUrl: TARGET_PAGE[role],
        session,
        onStatus: setStatus,
        hostResize: sizeFrame,
    });
}
