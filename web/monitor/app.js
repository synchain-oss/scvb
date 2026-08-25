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
import { GROUPS_JSON_KEY, VIZ_ABI } from "./viz-contract.js";
import {
    CHANNEL_COUNT,
    GROUP_LETTERS,
    groupOnline,
    mergeVizFrame,
    vizAccepts,
    vizDistRows,
    vizDurationS,
    vizHasLanes,
    vizIsEmptyState,
    vizLegendRows,
    vizPlayheadEvent,
    vizSeries,
} from "./viz.js";

// ------------------------------------------------------------- 设计盒(05 §1.2)
// 真源 = `web/shared/design-box.js` 的 DESIGN.monitor(./monitor-box.js 只是一层转发);
// index.html 里不写第二份数字(页面零硬编码,grep 断言)。
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
        // dev 兜底(仅无后端直开浏览器可见):不入词条,用英文避免硬编码中文。
        // **必须先摘掉 data-t**:本文件末尾的 `refreshI18n()` 晚于这里执行,而
        // `applyI18n` 认 data-t 写整串 textContent —— 不摘的话这句提示写完就被词条盖掉,
        // 裸开浏览器时永远看不见(浏览器级 smoke ⑨ 抓到的:实得的是那条只读词条)。
        hint.removeAttribute("data-t");
        hint.textContent =
            "No backend attached — open via web-preview/monitor.html";
    }
}

// ------------------------------------------------------------- 事件仓(单向渲染源)
const store = {
    ready: false,
    // 最近一帧**合并后**的 viz(帧头 + 沿用/新收的车道块,见 mergeVizFrame)。
    // **accept 失败也不清空** —— 它同时是 mergeVizFrame 的缓存,而且 `scvb.state`
    // 迟到时要靠它重算一遍投影(见 applyProjection 的头注)。
    frame: null,
    accepts: { ok: false, reason: "shape", abi: 0 }, // vizAccepts(viz) 的结果
    groups: 0, // scvb.groups 位图(事件缺失 = 0 ⇒ 绿点全灭,零报错)
    observed: 1, // 当前观察的组(1..8);本地乐观值,由 scvb.state 的 group_id 回推校正
    scale: 1,
    version: "",
    // `scvb.state` 的 viz 面:三态 + 独立的 fresh。**停摆判据归 native** ——
    // 只有它能松开映射再探一次,分得出「Output 进程真没了」与「还在但不发帧了」;
    // UI 侧靠「多久没收到事件」猜,永远会把 4Hz 的正常间隔与停摆混起来。
    vizStatus: { viz: "offline", fresh: true },
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
 * `scvb.state` 的 `group_id` 回推后由 render 把本地值交还给它(与 Tab1 的 paramEcho 同一纪律)。
 */
function observeGroup(g) {
    if (!Number.isInteger(g) || g < 1 || g > GROUP_LETTERS.length) return;
    if (store.observed === g) return;
    store.observed = g;
    store.frame = null;
    store.accepts = { ok: false, reason: "shape" };
    store.series = [];
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
          getDurationS: () => vizDurationS(visibleFrame()),
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
    // `scvb.state` 里虽然有 `uiScale`,但它 1Hz 变化才发 —— 缩放要**当场**跟手,
    // 等一秒才动是坏体验。故档位就地应用,state 回推只当校正。落盘仍归 commitUiScale()。
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

/**
 * 是否收到过 `scvb.playhead`(收到之后就不再用 viz 帧里那份低频的播放头种子)。
 *
 * 那一路是 **25Hz**(T45:WebViewHost 定时器的上限,Output 侧也一样),数据源是
 * Monitor **自己的 AudioPlayHead** —— 与 viz 段无关。故 Output 停摆时竖线照常走,
 * 本页不为「viz 陈旧」特判播放头。
 */
let playheadSeen = false;

/**
 * 首帧的播放头种子那一支**跑到过没有**(只读诊断,供浏览器级 smoke 断言覆盖到)。
 *
 * 为什么值得单开一个量:那一支的条件是「本帧可读 **且** 25Hz 那一路还没来过」——
 * 一次装载里最多命中一拍,而且命中与否取决于两个事件谁先到。它曾经藏着一个
 * ReferenceError(`viz` 是 render() 的局部量),正因为「命中一次就再也不进」而在
 * 画面上完全看不出来。没有这个量的话,机检只能靠场景的时序恰好撞上才抓得到 ——
 * 而「靠撞上」的覆盖等于没有覆盖。
 */
let seededFromFrame = false;

/**
 * 当前该画的那一帧(不可读时是 null)。
 *
 * `store.frame` **永远保留最近一帧**,哪怕这一帧判下来不可读 —— 它同时是
 * `mergeVizFrame` 的车道缓存,清掉会让下一帧的稳态合并失去依据。
 * 「能不能画」是 `store.accepts.ok` 说了算,不是靠把帧清成 null 来表达。
 */
function visibleFrame() {
    return store.accepts.ok ? store.frame : null;
}

/**
 * 重跑投影:判这一帧能不能读 + 重算 15 轨折线。
 *
 * **两个入口都要调**:viz 帧到达、以及 `scvb.state` 到达(段级三态与 fresh 变了,
 * 同一帧的结论会跟着变)。只在其中一处调过一次的后果,真机截图抓到过:
 * `scvb.state` 比首帧 viz 晚到时,首帧被按「未知状态」判成不可读,而之后的稳态帧
 * 不带车道 —— 于是分布图有 15 根柱、轨迹图却永远是空的。
 */
function applyProjection() {
    const a = vizAccepts(store.frame, store.vizStatus);
    store.accepts = a;
    // **陈旧不清帧**:`reason === "stale"` 时 `ok` 仍为 true,图继续显示上一份真数据。
    store.series = a.ok ? vizSeries(store.frame) : [];
}

// ============================================================================
// 渲染(store → DOM 的幂等纯投影)
// ============================================================================
function render() {
    const viz = visibleFrame();
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
    // 拒读 = abi 或几何自检不符 —— T44 的 `attachReadOnly()` 把几何漂移也按
    // `kAbiMismatch` 处理(同 abi 下理论不该发生,按拒连而非半兼容),UI 侧照同一口径。
    const refused = a.reason === "abi" || a.reason === "geometry";

    // 空态面板三态(判据只此一处,理由见 index.html 那块的注释):
    //   • `failed` ⇒ 标题「Output 未运行」+ 正文「组 {X} …」;
    //   • `window`/`shape`/首帧未到 ⇒ 收起标题,正文换成「尚无分段结果…」;
    //   • 拒连 ⇒ 标题与正文都收起 —— Output 明明在跑,挂「Output 未运行」是错的,
    //     而「尚无分段结果」也不对(不是没有分段,是我们拒绝去读)。话全由红横幅说。
    const offline = a.reason === "offline";
    const titleEl = $("monitor-empty-title");
    if (titleEl) titleEl.hidden = !offline;
    const emptyEl = $("monitor-empty-text");
    if (emptyEl) {
        emptyEl.hidden = refused;
        const key = offline ? "monitor.offline" : "chart.trajEmpty";
        if (emptyEl.getAttribute("data-t") !== key) {
            emptyEl.setAttribute("data-t", key);
        }
        fill(emptyEl, key, { X: GROUP_LETTERS[store.observed - 1] || "A" });
    }

    const abiBanner = $("monitor-banner-abi");
    if (abiBanner) abiBanner.hidden = !refused;
    if (refused) {
        fill($("monitor-banner-abi-text"), "monitor.abiMismatch", {
            a: VIZ_ABI,
            b: a.reason === "geometry" ? "?" : VIZ_ABI + 1,
        });
    }
    // 「在线但陈旧」:Output 还在跑、只是不再发帧(T45 修僵尸数据时引入的那一档,
    // 由 native 侧松开映射再探一次判定 —— UI 侧靠「多久没收到事件」猜不出来)。
    // **图仍然显示**:数据还是上一份真数据,挡掉反而更糟(用户会看到一张突然
    // 变空的图,而 Output 其实还在)。只挂一条琥珀横幅说明它不再更新。
    const stalledBanner = $("monitor-banner-stalled");
    if (stalledBanner) stalledBanner.hidden = a.reason !== "stale";

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
    //
    // 空态**两句话**,分得清才不骗人:
    //   • 桥根本没送车道块(旧版 Output / 桥回归)⇒「监视数据未接通」;
    //   • 送了、但一条线都没有(工程真没分析过)⇒「尚无分段结果」。
    // 画面一样、原因完全不同 —— 说错了会让用户去 DAW 里白找一遍。
    const empty = $("monitor-traj-empty");
    if (empty) {
        const hasLanes = vizHasLanes(viz);
        empty.hidden = hasLanes && store.series.length > 0;
        const key = hasLanes ? "chart.trajEmpty" : "monitor.noLanes";
        if (empty.getAttribute("data-t") !== key) {
            empty.setAttribute("data-t", key);
        }
        fill(empty, key, {});
    }
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
// 事件订阅(四个;名字逐字照 ./monitor-bridge.js 的 MONITOR_EVENTS)
// ============================================================================
if (bridge) {
    // `scvb.state`:组回显 + viz 三态 + fresh。**段级状态的唯一真源**。
    // 命名段是引用计数存活的:只要 Monitor 自己不松手,「Output 进程退出」就永远看不
    // 出来(段还在、读也成功)。T45 的做法是帧陈旧时**松开映射再探一次** —— 这件事
    // UI 侧做不了,故状态从事件里来,不按「多久没收到事件」自己猜。
    // 组回显也走这里(viz 帧里那个 `group_id` 只用来丢在途帧,不当回显 —— 回显要的是
    // 「native 现在观察哪个组」这一个事实的**单一**来源,两处各写一份迟早对不上)。
    bridge.on("scvb.state", (st) => {
        if (!st || typeof st !== "object") return;
        store.vizStatus = {
            viz: typeof st.viz === "string" ? st.viz : "offline",
            fresh: st.fresh !== false,
        };
        if (Number.isInteger(st.group_id)) store.observed = st.group_id;
        // 状态变了要**重跑一遍投影** —— 不只是重判。`scvb.state` 可能比首帧 viz 晚到
        // (壳页里 driver 的 onReady 排在页面 requestInitialState 之后),那时首帧已经
        // 被按「未知状态」判过一次;不重算的话,那一帧的折线就永远补不回来了。
        // 真机截图抓到的:停摆场景下分布图有 15 根柱、轨迹图却是空的。
        applyProjection();
        if (traj) traj.invalidate();
        render();
    });

    bridge.on("scvb.viz", onViz);

    // 组胶囊绿点:事件缺失时全灭、零报错(§2.4 同款容错)。
    // ⚠ 键名照**契约 §2.4 逐字**:`groups_online`(与 Output / Input 两侧同名同载荷;
    // 常量在 viz-contract.js)。读错的后果是**绿点永远不亮、而页面一切正常零报错** ——
    // 故走常量,不写字面量。它一度被写成 `online`:桥与本页一起偏离了契约,两边
    // 「一致地错」,shape parity 照样全绿 —— 见 GROUPS_JSON_KEY 的注释。
    bridge.on("scvb.groups", (g) => {
        store.groups = (g && g[GROUPS_JSON_KEY]) || 0;
        render();
    });

    // §2.6 原样复用:25Hz 扁平标量集,直接喂轨迹图的插值层。
    // **不排整页 render** —— 竖线与跟随滚动都在插值层里,整页投影与它无关
    // (与 Tab1/Tab3 的同款分工;逐帧触发整页渲染是纯烧 CPU)。
    bridge.on("scvb.playhead", (p) => {
        playheadSeen = true;
        if (traj) traj.onPlayhead(p);
    });
}

/**
 * viz 帧到达。
 *
 * ⓪ **在途帧按组号丢弃**。T45 依对表信在 viz 帧里补了 `group_id`,
 * 于是「刚点了 B、A 的最后一帧还在路上」这一拍认得出来了 —— 认不出来时它会被当成 B 组
 * 的数据画上去(轨集、轨名、时间线全是 A 的),而画面看着完全正常。
 * 这条闸**必须建立在字段真的存在之上**:之前载荷里没有这个字段,写了也永远不命中,
 * 那种「留着不生效的判断」比没有更糟(读代码的人以为已经防住了),故当时删掉了。
 * 现在字段有了才加回来。缺字段的旧桥 ⇒ `Number.isInteger` 判 false ⇒ 照旧全收。
 */
function onViz(raw) {
    if (
        raw &&
        Number.isInteger(raw.group_id) &&
        raw.group_id !== store.observed
    ) {
        return;
    }
    // ---- ① 车道按需重发:稳态帧不带车道,与缓存拼成完整的一帧(判据见 mergeVizFrame)
    store.frame = mergeVizFrame(store.frame, raw);
    // ---- ② 投影(含折线重算)。只在这里与 `scvb.state` 到达时算,**不在 render 里** ——
    // render 每帧都跑,不能在里面算 15 轨折线。
    applyProjection();
    const a = store.accepts;

    // ---- ④ 首帧的播放头种子。`scvb.playhead` 要等下一拍才到,而快照自带的 viz 帧里
    // 就有播放头位置 —— 用它先把竖线摆上,首帧出图时位置就是对的。只在还没收到过
    // playhead 事件时做:之后那一路更准(25Hz + 插值),不该被 4Hz 的帧覆盖。
    if (traj && a.ok && !playheadSeen) {
        // ⚠ 种子取自 `store.frame`(**合并后**的那一帧),不是 render() 里那个同名局部量 ——
        // 那是另一个作用域的 `const viz`,在这里引用它是 ReferenceError:首帧恰好又是
        // 「`a.ok` 且还没收到 playhead」的唯一一拍,于是**每次装载必炸一次**,
        // 而后续 25Hz 的 playhead 事件一到就再也不进这个分支,页面看着完全正常。
        // node 侧的 smoke 不跑 app.js,是浏览器级 smoke(smoke-monitor-page.mjs)抓到的。
        const ev = vizPlayheadEvent(store.frame);
        seededFromFrame = true;
        if (ev) traj.onPlayhead(ev);
    }

    if (traj) traj.invalidate(); // 数据面变了 ⇒ 请求一次静态层重绘(幂等)
    render();
}

// ============================================================================
// 首帧:§0.6 同款门控由页面掌握 —— 装载时调一次 requestInitialState()。
//
// **它可重入、无副作用**(T45 `f61094a` 起写进了 `MonitorEditor::buildSnapshot()` 的注释,
// 作为对消费侧的保证):`bridgeReady_` 是幂等赋值,`buildSnapshot()` 做的全是**复位 diff
// 基线**再返回一份**必带车道**的完整快照 —— 不建段、不碰共享内存、不改 state、不 claim,
// 净效果只是下一个 tick 把四个事件各重发一次首帧。WebView 每次重载走的就是这条路。
//
// 记这一笔是为了**将来那条自愈路**:万一出现「已 online 却持续拿不到车道」
// (`hasLanes` 长期为假 —— 桥侧车道基线出过这类 bug),页面可以重新拉一次首帧快照把车道
// 补回来,而不必给帧加一个只在故障期有用的 `lanesOmitted` 字段(与 T45 商定:**不加**)。
// **现在不实现** —— 那个 bug 已在桥侧修掉,为一个不该出现的状态提前写恢复逻辑,
// 等于给页面永久背一段没人验得到的代码。真出现时按这条注释做,N 取 3–5 秒
// (重发首帧会带来一帧重绘,太短会抖)。
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
        if (Number.isInteger(snap.group_id)) store.observed = snap.group_id;
        if (snap.version) store.version = String(snap.version);
        if (Number.isFinite(snap.groups_online))
            store.groups = snap.groups_online;
        const ui = snap.ui || {};
        if (ui.language && LANGS.includes(ui.language)) {
            setLang(ui.language, { push: false });
        }
        if (Number.isFinite(ui.scale)) applyScale(ui.scale);
        // 快照自带首帧 viz(T45 按对表信照做)。它是完整帧、必带车道 —— 页面因此
        // 不必空等一个 4Hz 周期才出图。
        //
        // **不在这里瞎猜状态**:`scvb.state` 通常在 `requestInitialState()` 里就已经
        // 推过一遍(T45 的 onReady 与 mock 同款),那才是真值。早先这里无条件写
        // `{viz:"online", fresh:true}`,把已经收到的真状态覆盖掉了 —— 停摆场景因此被
        // 显示成掉线。真机截图抓到的,node 侧看不出来(那边不跑 boot)。
        // 现在帧里 `online` 与 `fresh` 是分开的两件事实(T45 `decae38`),`vizAccepts()`
        // 能只凭帧就分出「掉线」与「在线但停更」;`scvb.state` 晚到时上面的处理器还会
        // `applyProjection()` 补算一遍。两条路都通,这里仍然只管把帧收下,不猜。
        if (snap.viz) onViz(snap.viz);
    }
    render();
})();

// 窗口收起 ⇒ 拆掉轨迹图。**这一步非做不可**:trajectory-chart 在 window 上挂了
// pointerup 兜底、订了媒体查询与父盒的 ResizeObserver,三者都活得比 canvas 长
// (见该文件 destroy() 的头注)。Monitor 的编辑器会在插件窗口开合里反复建销毁,
// 不拆就是每开一次漏一组订阅 —— window 上那条尤其致命,它连着整个闭包
// (含 15 条折线的数据面)一起钉在内存里。destroy() 是幂等的,重复调用一步不走。
// (曾经这里还有一句 `clearTimeout(staleTimer)` —— 那是「按 publishMs 自己猜停摆」那版
// 留下的残句,判据改归 `scvb.state` 之后定时器早就没了,变量也不存在:关窗时
// **必抛 ReferenceError,于是同一个处理器里的 traj.destroy() 一次都没跑过** ——
// 也就是说 destroy 契约的第一个消费者其实一直在漏订阅。node 侧那条
// `/clearTimeout\(staleTimer\)/` 源码正则还把它当成「不留孤儿 timer」的证据钉着,
// 正是「按源文本断言」这种做法的反例:它证明的是字符在,不是代码跑得通。)
addEventListener("pagehide", () => {
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
        emptyState: vizIsEmptyState(store.accepts.reason),
        observed: store.observed,
        groups: store.groups,
        stalled: store.accepts.reason === "stale",
        // 首帧种子那一支跑到过没有(浏览器级 smoke 用它确认那条路径真被覆盖了);
        // `playheadSeen` 一并给出 —— 两者是同一个条件的两半,断言红了能一眼分清
        // 是「种子那一支没跑」还是「25Hz 那一路已经先到了」。
        seededFromFrame,
        playheadSeen,
        laneRevision: store.frame ? store.frame.laneRevision : null,
        generation: store.frame ? store.frame.generation : null,
        durationS: vizDurationS(visibleFrame()),
        hasLanes: vizHasLanes(visibleFrame()),
        seriesTracks: store.series.map((s) => s.ch),
        // 每轨的折线段数 —— 断线是本页最核心的语义,截图之外还要有个数字面
        seriesRuns: store.series.map((s) => s.runs.length),
        distTracks: vizDistRows(visibleFrame()).map((r) => r.ch),
        legendTracks: vizLegendRows(visibleFrame()).map((r) => r.ch),
    }),
};
