// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Monitor —— 页面接线(T46;裁决 [J75] C,规格 05 文末「J75 增补规格」节 C)
// -----------------------------------------------------------------------------
// 职责:createMonitorBridge → requestInitialState(§0.6 同款门控由页面掌握)→
// 订阅三个事件(scvb.viz / scvb.groups / scvb.playhead)→ 维护一份只读 store →
// 驱动 header(组选择 / 语言)、两张图、图例、空态、缩放。
//
// **只读纪律(本页的第一条)**:除了「看哪个组 / 用哪种语言 / 窗口多大」这三件本实例
// 自己的事,本文件不发起任何上行调用,也没有任何写控件。`setObservedGroup` 为什么
// 不叫 `setGroupId`,见 `./monitor-bridge.js` 的头注。
//
// **零重复实现**(07 T46 卡逐字):
//   • 轨迹图 = `web/shared/trajectory-chart.js`(T43 交付,按其文件头的「复用契约」
//     消费:喂 getSeries / getPalette,文案自己写,窗口收起时 destroy);
//   • 分布图与图例 = `web/shared/distribution-chart.js`(T46 从 tab-master.js 提取,
//     Output 侧产物逐字节不变);
//   • 词条 / 占位符填充 / 设计盒缩放机制 = 与 Output、Input 同一套。
//   本文件自己写的只有一件:**viz 段 → 两张图的投影**,在 `./viz.js`(纯函数,
//   node 侧可断言;viz 事件的形状与「待与 T44/T45 对表」的清单也写在那份文件头)。
//
// 纪律(与 Output/Input 页同源):状态一律写 data-* 属性;词条一律走 key;
// 裸开浏览器(无 __JUCE__、无注入的 mock)必须零 console.error。
// =============================================================================

import { applyI18n, format, LANGS } from "../shared/i18n.js";
import {
    distBarsHtml,
    legendChOf,
    legendItemsHtml,
} from "../shared/distribution-chart.js";
import {
    createTrajectoryChart,
    panTickText,
} from "../shared/trajectory-chart.js";
import { MONITOR_DESIGN } from "./monitor-box.js";
import { createMonitorBridge } from "./monitor-bridge.js";
import {
    CHANNEL_COUNT,
    GROUP_LETTERS,
    groupOnline,
    vizAccepts,
    vizDistRows,
    vizDurationS,
    vizLegendRows,
    vizSeries,
} from "./viz.js";

// ------------------------------------------------------------- 设计盒(05 §1.2)
// 真源 = ./monitor-box.js;index.html 里不写第二份数字(页面零硬编码,grep 断言)。
const card = document.getElementById("card");
card.style.setProperty("--box-w", MONITOR_DESIGN.w + "px");
card.style.setProperty("--box-h", MONITOR_DESIGN.h + "px");

// ------------------------------------------------------------- 桥
let bridge = null;
try {
    bridge = createMonitorBridge({ mockBackend: window.__SCVB_MOCK__ });
} catch (e) {
    console.warn("SCVB Monitor:createMonitorBridge 未接上后端 —— " + e.message);
    const hint = document.querySelector('[data-gb="monitor-footer-hint"]');
    if (hint) {
        // dev 兜底(仅无后端直开浏览器可见):不入词条,用英文避免硬编码中文
        hint.textContent =
            "No backend attached — open via web-preview/monitor.html";
    }
}

/**
 * viz 停更多久算「停更」(ms)。
 * 判据取 `seq` 不再前进 —— 段是低频发布,单看「有没有事件」会把正常的发布间隔
 * 误判成停更;而 Output 一旦关掉,发布器整个没了,seq 自然冻住。
 * 3 秒 ≈ 低频档(4Hz)的十几拍,不会被一次调度抖动打红。
 */
const VIZ_STALE_MS = 3000;

// ------------------------------------------------------------- 事件仓(单向渲染源)
const store = {
    ready: false,
    viz: null, // 最近一帧 scvb.viz 载荷
    accepts: { ok: false, reason: "shape", abi: 0 }, // vizAccepts(viz) 的结果
    groups: 0, // scvb.groups 位图(事件缺失 = 0 ⇒ 绿点全灭,零报错)
    playhead: null, // scvb.playhead(§2.6 原样)
    observed: 1, // 当前观察的组(1..8);本地乐观值,由 viz.groupId 回推校正
    scale: 1,
    version: "",
    stalled: false,
    lastSeq: null,
    series: [], // vizSeries 的缓存(轨迹图每次重绘都读它,不能每帧重算)
};

const $ = (gb) => document.querySelector(`[data-gb="${gb}"]`);

async function call(name, ...args) {
    if (!bridge || typeof bridge[name] !== "function") return null;
    try {
        return await bridge[name](...args);
    } catch (e) {
        console.warn(`SCVB Monitor:bridge.${name}() 调用失败 —— ${e.message}`);
        return null;
    }
}

// ------------------------------------------------------------- i18n
let lang = "zh";
let dictNow = {};

/** 词条 + 占位符 → textContent(applyI18n 写整串,填充必须排在它之后)。 */
function fill(node, key, vals) {
    if (!node) return;
    if (!Object.prototype.hasOwnProperty.call(dictNow, key)) return;
    node.textContent = format(dictNow[key], vals || {});
}

/**
 * 「拆格」钩子:`data-t-split="key"` 的容器把一条词条按 " · " 拆成 N 个等距格子
 * (分布图底轴的五刻度)。applyI18n 只认 data-t / data-t-aria,写 data-t 会被整串
 * 覆盖,故另立本钩子;词条仍是字典里的单条(零新增自由文案)。与 Output 页同款。
 */
function renderSplitAxes(t) {
    document.querySelectorAll("[data-t-split]").forEach((el) => {
        const key = el.getAttribute("data-t-split");
        const text = Object.prototype.hasOwnProperty.call(t, key) ? t[key] : "";
        el.replaceChildren(
            ...text.split(" · ").map((part) => {
                const span = document.createElement("span");
                span.textContent = part;
                return span;
            }),
        );
    });
}

function refreshI18n() {
    dictNow = applyI18n(document, lang);
    renderSplitAxes(dictNow);
    // 刻度列是拼出来的(不是 data-t),applyI18n 刷不到它 —— 签名带语言,变了才重建
    renderTrajAxis();
    render();
}

function setLang(next, { push = true } = {}) {
    if (!LANGS.includes(next)) return;
    lang = next;
    for (const btn of document.querySelectorAll("[data-lang]")) {
        // 语言胶囊是普通 button(非 role="tab"),可达属性用 aria-pressed
        btn.setAttribute(
            "aria-pressed",
            String(btn.getAttribute("data-lang") === lang),
        );
    }
    refreshI18n();
    if (push) call("setLang", next);
}

for (const btn of document.querySelectorAll("[data-lang]")) {
    btn.addEventListener("click", () => setLang(btn.getAttribute("data-lang")));
}

// ------------------------------------------------------------- 组选择 A-H(只读)
// 八枚胶囊由本文件生成(GROUP_LETTERS 单一来源,避免八段复制粘贴)。
// 点击 = 换观察对象:不 claim、不写 registry、对被观察的组零副作用(J75 C),
// 故**没有确认条** —— Output ⓪ 卡那条「切换到组 {x} 将断开本组全部连接」在这里
// 一个字都不适用。
const groupSeg = $("monitor-group-seg");
if (groupSeg) {
    groupSeg.innerHTML = GROUP_LETTERS.map(
        (letter, i) =>
            `<button type="button" class="group-pill" data-group="${i + 1}"` +
            ` aria-pressed="${i === 0}" data-online="0">` +
            `<span class="group-pill__dot" aria-hidden="true"></span>${letter}</button>`,
    ).join("");
    groupSeg.addEventListener("click", (e) => {
        const btn =
            e.target instanceof Element
                ? e.target.closest("[data-group]")
                : null;
        if (!btn) return;
        observeGroup(Number(btn.getAttribute("data-group")));
    });
}

/**
 * 换观察对象。
 *
 * 本地先行(乐观)+ 清掉上一组的数据面:两组的轨集、轨名、时间线覆盖都不同,
 * 留着旧图等新帧到达会让用户看到「切过去了但画的还是上一组」——**比空白更误导**。
 * `viz.groupId` 回推后由 render 把本地值交还给它(与 Tab1 的 paramEcho 同一纪律)。
 */
function observeGroup(g) {
    if (!Number.isInteger(g) || g < 1 || g > GROUP_LETTERS.length) return;
    if (store.observed === g) return;
    store.observed = g;
    store.viz = null;
    store.accepts = { ok: false, reason: "shape", abi: 0 };
    store.series = [];
    store.lastSeq = null;
    store.stalled = false;
    armStaleTimer();
    call("setObservedGroup", g);
    render();
}

// ------------------------------------------------------------- 轨迹图(共享件)
// 复用契约照 `web/shared/trajectory-chart.js` 的文件头:喂 getSeries / getDurationS,
// 文案与刻度列由本文件写,窗口收起时**必须** destroy(见文末 pagehide)。
let trajAxisTicks = [];
let trajAxisKey = "";
let highlightCh = 0;

const trajCanvas = $("monitor-traj-canvas");
const traj = trajCanvas
    ? createTrajectoryChart({
          canvas: trajCanvas,
          playheadEl: $("monitor-traj-playhead"),
          followBtn: $("monitor-traj-follow"),
          resetPanBtn: $("monitor-traj-reset"),
          zoomEl: $("monitor-traj-zoom"),
          getSeries: () => store.series,
          getDurationS: () => vizDurationS(store.viz),
          getUiScale: () => store.scale,
          // 组不在线时两张卡整体 display:none —— 画布量不到舞台,也不该起 rAF
          // (05 §6.1「空闲零 rAF」)。这是本页唯一的可见性闸:Monitor 没有 tab。
          isVisible: () => store.accepts.ok,
          onFollowChange: () => {},
          onPanAxis: (ticks) => {
              trajAxisTicks = ticks;
              renderTrajAxis();
          },
      })
    : null;

/**
 * y 刻度列。轨迹图推来 `{pan, y, side}`,方位词在这里按字典补上 ——
 * `side` 只会是 R/C/L(即 +100 / 0 / −100 三条锚刻度),纵向放大到看不见它们时
 * 本列就只剩数字;给别的刻度硬贴方位词等于写一个不成立的读数。
 * 方位词**按 key 取**(`chart.panSideR/C/L`),不是拆一串按位置认 —— 语序在三语里
 * 本就不同,按位置取就会把左右标反,而那是一眼看不出来的错(图照画,只是左右颠倒)。
 */
function renderTrajAxis() {
    const el = $("monitor-traj-axis-y");
    if (!el) return;
    const side = (s) => (s ? dictNow["chart.panSide" + s] || "" : "");
    const sig =
        ["R", "C", "L"].map(side).join(" ") +
        "|" +
        trajAxisTicks.map((k) => `${k.pan}@${Math.round(k.y)}`).join(",");
    if (sig === trajAxisKey) return; // 刻度与语言都没动 ⇒ 一步不走
    trajAxisKey = sig;
    el.replaceChildren(
        ...trajAxisTicks.map((k) => {
            const w = side(k.side);
            const span = document.createElement("span");
            span.className = "traj-axis-y__tick" + (w ? " is-anchor" : "");
            span.style.top = k.y.toFixed(2) + "px";
            span.textContent = (w ? w + " " : "") + panTickText(k.pan);
            return span;
        }),
    );
}

// ------------------------------------------------------------- 图例 hover 联动
// 事件委托挂容器,不逐行挂:行是按可见轨集重建的,逐行挂会随重建泄漏。
const legendEl = $("monitor-legend");
if (legendEl) {
    legendEl.addEventListener("pointerover", (e) =>
        setHighlight(legendChOf(e.target)),
    );
    legendEl.addEventListener("pointerleave", () => setHighlight(0));
}

/** 图例 hover 联动(0 = 取消高亮)。纯展示,无写操作(J75 B)。 */
function setHighlight(ch) {
    const v = Number(ch) || 0;
    if (highlightCh === v) return;
    highlightCh = v;
    if (traj) traj.setHighlight(v);
    render();
}

// ============================================================================
// 缩放:10 秒防呆(与 Output/Input 同一机制,05 §1.2)
// setUiScale = 立即预览;10 秒内不点「保持」就回退到进入本次预览前的档位,
// 「保持」才 commitUiScale() 落盘。取消 / 超时 / 关窗一律回退。
// 它写的是**本实例自己的 UI 偏好**,不是被观察组的状态 —— 不违反只读。
// ============================================================================
const scaleUi = {
    select: $("monitor-scale"),
    confirm: $("monitor-scale-confirm"),
    countdown: $("monitor-scale-confirm-countdown"),
    pre: $("monitor-scale-confirm-pre"),
    post: $("monitor-scale-confirm-post"),
    keep: $("monitor-scale-confirm-keep"),
    revert: $("monitor-scale-confirm-revert"),
};
let scaleTimer = 0;
let scalePrev = 1;
let scaleLeft = 0;

/** 「超出当前屏幕」判据(05 §1.2):W*F > availWidth ∨ H*F > availHeight。 */
function scaleOverflows(f) {
    const s = typeof window !== "undefined" ? window.screen : null;
    if (!s || !s.availWidth || !s.availHeight) return false;
    return (
        MONITOR_DESIGN.w * f > s.availWidth ||
        MONITOR_DESIGN.h * f > s.availHeight
    );
}

function buildScaleOptions() {
    if (!scaleUi.select) return;
    scaleUi.select.replaceChildren();
    for (const f of MONITOR_DESIGN.presets) {
        const opt = document.createElement("option");
        opt.value = String(f);
        opt.textContent = Math.round(f * 100) + "%";
        // 超屏档位**灰掉但不隐藏**(05 §1.2 逐字)
        opt.disabled = scaleOverflows(f);
        if (f === 1) opt.selected = true;
        scaleUi.select.appendChild(opt);
    }
    scaleUi.select.addEventListener("change", () =>
        previewScale(Number(scaleUi.select.value)),
    );
}

/**
 * 确认框正文 = 词条 `scale.confirmBody`(带 {s} 占位符)按 {s} 切成前后两段。
 * 不挂 data-t:applyI18n 写整串 textContent,会把中间那个倒计时 span 一并抹掉。
 */
function renderScaleConfirm() {
    const raw = dictNow["scale.confirmBody"];
    if (typeof raw === "string" && scaleUi.pre && scaleUi.post) {
        const [pre, post] = raw.split("{s}");
        scaleUi.pre.textContent = pre;
        scaleUi.post.textContent = post === undefined ? "" : post;
    }
    // 没有倒计时在跑时不动那个数字(确认框此刻是 hidden 的)
    if (scaleUi.countdown && scaleLeft > 0) {
        scaleUi.countdown.textContent = String(scaleLeft);
    }
}

function stopScaleCountdown() {
    if (scaleTimer) clearInterval(scaleTimer);
    scaleTimer = 0;
    if (scaleUi.confirm) scaleUi.confirm.hidden = true;
}

function revertScale() {
    stopScaleCountdown();
    const back = scalePrev;
    call("setUiScale", back);
    applyScale(back);
}

async function previewScale(f) {
    // **嵌套预览**:倒计时还在跑时再选一档,回退目标必须仍是「进入本次预览**前**」
    // 的档位 —— 重新采样会把回退终点钉在中途那一档。
    if (!scaleTimer) scalePrev = store.scale;
    const res = await call("setUiScale", f);
    if (res && res.ok === false) return; // 拒绝态:不在档位表 → badArg
    // Monitor 没有 `scvb.state` 回推通路(它只订 viz/groups/playhead),故档位由
    // 本地就地应用 —— 与 Output/Input「等 state 回推再 zoom」是同一效果的两条路,
    // 差别只在这边少一跳。落盘仍归 commitUiScale()。
    applyScale(f);
    scaleLeft = 10;
    if (scaleUi.confirm) scaleUi.confirm.hidden = false;
    renderScaleConfirm();
    if (scaleTimer) clearInterval(scaleTimer);
    scaleTimer = setInterval(() => {
        scaleLeft -= 1;
        renderScaleConfirm();
        if (scaleLeft <= 0) revertScale(); // 超时回退,与「取消」同一路径
    }, 1000);
}

if (scaleUi.keep) {
    scaleUi.keep.addEventListener("click", () => {
        stopScaleCountdown();
        call("commitUiScale");
    });
}
if (scaleUi.revert) scaleUi.revert.addEventListener("click", revertScale);
addEventListener("pagehide", stopScaleCountdown); // 关窗回退,不污染新实例

/** CSS zoom 应用点(设计盒 + zoom + 宿主 setSize 同步,05 §1.2 机制同款)。 */
function applyScale(f) {
    if (!Number.isFinite(f) || f <= 0) return;
    store.scale = f;
    card.style.zoom = String(f);
    if (scaleUi.select) scaleUi.select.value = String(f);
    // 后备存储的 k = uiScale × dpr 变了(CSS zoom 不动 dpr、也不动父盒的 CSS px
    // 尺寸,ResizeObserver 与 observeResolution 都不会响)—— 必须显式请求一次重绘,
    // 否则画布一直用着旧 k,画面持续糊。与 Tab1 的 lastUiScale 同款理由。
    if (traj) traj.invalidate();
}

// ------------------------------------------------------------- 停更判定
let staleTimer = 0;
function armStaleTimer() {
    clearTimeout(staleTimer);
    staleTimer = setTimeout(() => {
        // 只有「本该有数据」时才算停更:组不在线走的是空态,不是停更。
        if (!store.accepts.ok) return;
        store.stalled = true;
        render();
    }, VIZ_STALE_MS);
}

// ============================================================================
// 渲染(store → DOM 的幂等纯投影)
// ============================================================================
function render() {
    const viz = store.viz;
    const a = store.accepts;
    const online = a.ok;

    // 组选择:aria-pressed = 当前观察的组;data-online = 该组有 Output 在跑
    for (const btn of document.querySelectorAll("[data-group]")) {
        const g = Number(btn.getAttribute("data-group"));
        btn.setAttribute("aria-pressed", String(g === store.observed));
        btn.setAttribute("data-online", String(groupOnline(store.groups, g)));
    }

    // 两张图与空态的总闸(J75 C「组不在线显示空态」)
    card.setAttribute("data-online", online ? "1" : "0");
    fill($("monitor-empty-text"), "monitor.offline", {
        X: GROUP_LETTERS[store.observed - 1] || "A",
    });

    // 横幅:abi 拒读(红)/ 停更(琥珀)。两者都只在「本该有数据」时才有意义。
    const abiBad = a.reason === "abi";
    const abiBanner = $("monitor-banner-abi");
    if (abiBanner) abiBanner.hidden = !abiBad;
    if (abiBad) {
        fill($("monitor-banner-abi-text"), "monitor.abiMismatch", {
            a: 1,
            b: a.abi,
        });
    }
    const stalledBanner = $("monitor-banner-stalled");
    if (stalledBanner) stalledBanner.hidden = !(online && store.stalled);

    // footer 版本号(§1.1 同款:快照到了才写)。**必须排在空态早退之前** ——
    // 版本号是本实例自己的身份,与被观察的组在不在线无关;放在早退之后,空态下
    // 页脚会一直挂着 HTML 里那个 v0.0.0 占位。
    const ver = $("monitor-version");
    if (ver && store.version) ver.textContent = "v" + store.version;

    if (!online) {
        // 不在线就不画图,也不留下上一组的残影;轨迹图收手(两条自持循环一起停)
        if (traj) traj.suspend();
        writeHtml($("monitor-dist-bars"), "");
        writeHtml(legendEl, "");
        return;
    }

    // ---- 上:分布图(几何与拼串归 web/shared/distribution-chart.js)
    writeHtml(
        $("monitor-dist-bars"),
        distBarsHtml(vizDistRows(viz), highlightCh),
    );

    // ---- 图例(两图共用;行集 = 两图并集,理由见 viz.js 的 vizLegendRows)
    const legendHint = dictNow["chart.legendHint"] || "";
    if (legendEl && legendEl.getAttribute("title") !== legendHint) {
        legendEl.setAttribute("title", legendHint);
    }
    writeHtml(
        legendEl,
        legendItemsHtml(vizLegendRows(viz), {
            badge: dictNow["stereoBadge"] || "ST",
            highlightCh,
        }),
    );

    // ---- 下:轨迹图。数据面在 onViz 里算好存进 store.series(每帧重算 15 轨
    // 的折线是纯浪费:载荷没变时结果逐字相同)。
    const empty = $("monitor-traj-empty");
    if (empty) empty.hidden = store.series.length > 0;
    if (traj) {
        traj.setDuration(vizDurationS(viz));
        traj.resume(); // 与 suspend 配对的按需起帧;幂等且便宜
    }
}

/** 只在真变了才写 innerHTML(重写会打断 CSS transition,也白费一次解析)。 */
function writeHtml(node, html) {
    if (node && node.innerHTML !== html) node.innerHTML = html;
}

// ============================================================================
// 事件订阅(三个;名字逐字照 ./monitor-bridge.js 的 MONITOR_EVENTS)
// ============================================================================
if (bridge) {
    bridge.on("scvb.viz", onViz);

    // 组胶囊绿点:事件缺失时全灭、零报错(§2.4 同款容错)
    bridge.on("scvb.groups", (g) => {
        store.groups = (g && g.groups_online) || 0;
        render();
    });

    // §2.6 原样复用:30Hz 扁平标量集,直接喂轨迹图的插值层。
    // **不排整页 render** —— 竖线与跟随滚动都在插值层里,整页投影与它无关
    // (与 Tab1/Tab3 的同款分工;30Hz 触发整页渲染是纯烧 CPU)。
    bridge.on("scvb.playhead", (p) => {
        store.playhead = p;
        if (traj) traj.onPlayhead(p);
    });
}

/**
 * viz 帧到达。
 *
 * 三道闸,顺序不能换:
 *   ① `vizAccepts` —— magic/abi/online 的总闸(拒读的理由要能显示成横幅);
 *   ② **组号回显校正** —— 切组请求与 viz 帧之间有一拍在途,期间到达的是**上一组**
 *      的帧。原样吃下去会让「切过去了但画的是上一组」这一拍变成正确显示的假象,
 *      而这正是切组最容易被漏测的地方。groupId 与请求值不符即整帧忽略;
 *   ③ 折线重算 —— 只在这里算一次(render 是每帧跑的,不能在里面算 15 轨折线)。
 */
function onViz(viz) {
    const a = vizAccepts(viz);
    if (
        a.ok &&
        Number.isInteger(viz.groupId) &&
        viz.groupId !== store.observed
    ) {
        return; // 上一组的在途帧,丢掉
    }
    store.viz = a.ok ? viz : null;
    store.accepts = a;
    store.series = a.ok ? vizSeries(viz) : [];

    // 停更判定看 seq(低频发布下「有没有事件」不是判据,见 VIZ_STALE_MS 注)
    const seq = viz && Number.isFinite(viz.seq) ? viz.seq : null;
    if (seq !== store.lastSeq) {
        store.lastSeq = seq;
        store.stalled = false;
        armStaleTimer();
    }

    if (traj) traj.invalidate(); // 数据面变了 ⇒ 请求一次静态层重绘(幂等)
    render();
}

// ============================================================================
// 首帧:§0.6 同款门控由页面掌握 —— 装载时调一次 requestInitialState()。
// ============================================================================
buildScaleOptions();
refreshI18n();

(async function boot() {
    if (!bridge) {
        render();
        return;
    }
    const snap = await call("requestInitialState");
    if (snap) {
        store.ready = true;
        if (Number.isInteger(snap.groupId)) store.observed = snap.groupId;
        if (snap.version) store.version = String(snap.version);
        if (Number.isFinite(snap.groups_online))
            store.groups = snap.groups_online;
        const ui = snap.ui || {};
        if (ui.language && LANGS.includes(ui.language)) {
            setLang(ui.language, { push: false });
        }
        if (Number.isFinite(ui.scale)) applyScale(ui.scale);
        if (snap.viz) onViz(snap.viz);
    }
    render();
})();

// 窗口收起 ⇒ 拆掉轨迹图。**这一步非做不可**:trajectory-chart 在 window 上挂了
// pointerup 兜底、订了媒体查询与父盒的 ResizeObserver,三者都活得比 canvas 长
// (见该文件 destroy() 的头注)。Monitor 的编辑器会在插件窗口开合里反复建销毁,
// 不拆就是每开一次漏一组订阅 —— window 上那条尤其致命,它连着整个闭包
// (含 15 条折线的数据面)一起钉在内存里。destroy() 是幂等的,重复调用一步不走。
addEventListener("pagehide", () => {
    clearTimeout(staleTimer);
    if (traj) traj.destroy();
});

// 测试面:web-preview 的 smoke 与截图器要能在页内读到投影结果(不经私有闭包)。
// 只读快照,不暴露任何写入口 —— 与本页的只读身份一致。
window.__SCVB_MONITOR__ = {
    channelCount: CHANNEL_COUNT,
    design: MONITOR_DESIGN,
    snapshot: () => ({
        online: store.accepts.ok,
        reason: store.accepts.reason,
        observed: store.observed,
        groups: store.groups,
        stalled: store.stalled,
        seriesTracks: store.series.map((s) => s.ch),
        distTracks: vizDistRows(store.viz).map((r) => r.ch),
        legendTracks: vizLegendRows(store.viz).map((r) => r.ch),
    }),
};
