// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output —— 页面接线(外壳层)。
// -----------------------------------------------------------------------------
// 当前阶段(T31 Wave 2「交互接线」):
//   • 本文件负责**外壳与事件仓**:createBridge → requestInitialState(§0.6 门控由页面
//     掌握)→ 订阅契约 §2 的九个事件 → 维护一份深合并后的 store → 驱动 header /
//     横幅区 / footer / 缩放 / 引导页 / tab 路由;Tab1 的全部渲染与上行调用在
//     web/output/tab-master.js(createTabMaster)。
//   • **Wave 1 的 WAVE1_NUMBERS 静态填数路径已删除**:所有 {n}/{m}/{k}/{p}/{t}
//     一律由事件算出(mock fixture 下渲染结果与 Wave 1 同数,因为两者同源于
//     scvb.segments / scvb.captureProgress)。
//   • **T32 Wave 1**:Tab2 的列头 / 15 轨行矩阵 / 图例脚注已迁到
//     web/output/tab-tracks.js(createTabTracks),本文件只留一句装配调用;
//     电平弹道空壳在 web/output/canvas/meter.js(Wave 2 实现)。
//   • Tab3 / Tab4 仍是 T27b 灰模骨架,泳道模板保持原样待 T33-T35 替换 ——
//     它们的模板仍用 web/shared/mock-data.js 撑列宽,零状态渲染逻辑。
//   • 浏览器直开(无 __JUCE__、无注入的 window.__SCVB_MOCK__)必须零 console.error——
//     createBridge 的失败走 try/catch,只 console.warn 一句并在 footer 落一行提示。
//
// 纪律:函数名/事件名逐字照 web/shared/bridge.js 的 BRIDGE_FUNCTIONS/BRIDGE_EVENTS
// (= 冻结契约 docs/SCVB_CONTRACT.md §1/§2);状态一律写 data-* 属性;词条一律走 key。
// =============================================================================

import { createBridge } from "../shared/bridge.js";
import { applyI18n, LANGS } from "../shared/i18n.js";
import { DESIGN } from "../shared/design-box.js";
import { FIFTEEN_TRACKS } from "../shared/mock-data.js";
import {
    createTabMaster,
    deepMerge,
    format,
    connectedCount,
    misalignedTracks,
    outputPhase,
    footerPrintKey,
    secondsToTimecode,
    shouldShowGuide,
    GROUP_IDS,
    CHANNEL_COUNT,
    HOST_ECHO_FRESH_MS,
} from "./tab-master.js";
import { createTabTracks } from "./tab-tracks.js";
import { createTabSettings } from "./tab-settings.js";

// ------------------------------------------------------------- 设计盒尺寸(05 §1.2)
// 真源 = web/shared/design-box.js DESIGN.output;index.html 里不写第二份数字
// (页面零硬编码设计盒宽高,grep 断言)。
const card = document.getElementById("card");
card.style.setProperty("--box-w", DESIGN.output.w + "px");
card.style.setProperty("--box-h", DESIGN.output.h + "px");

// ------------------------------------------------------------- createBridge(T28 附桥)
// 浏览器直开走 web-preview 才有 window.__SCVB_MOCK__;裸开本文件时两者皆无,
// createBridge 会 throw——按纪律只 warn,不 throw 到全局(零 console.error)。
let bridge = null;
try {
    bridge = createBridge({
        role: "output",
        mockBackend: window.__SCVB_MOCK__,
    });
} catch (e) {
    console.warn("SCVB Output:createBridge 未接上后端 —— " + e.message);
    const hint = document.getElementById("footer-hint");
    if (hint) {
        // dev 兜底(仅无后端直开浏览器可见):不入词条,用英文避免硬编码中文
        hint.textContent = "No backend attached — open via web-preview (T28)";
    }
}

// ------------------------------------------------------------- 事件仓(单向渲染源)
// 契约 §2.1「全 UI 的单向渲染源」:事件只往 store 里写,DOM 只从 store 里读。
const store = {
    ready: false, // requestInitialState() 已回(§0.6 mBridgeReady)
    state: {}, // scvb.state 深合并结果(+ §1.1 快照的 state 子树)
    snapshot: null, // §1.1 返回(session_guid / version / *_global / conn)
    params: {
        values: {},
        hostEcho: false,
        hostEchoAt: 0,
        full: true,
        versionActive: 1,
    },
    conn: null, // §2.3
    groups: 0, // §2.4 groups_online 位图(事件缺失 = 0 ⇒ 绿点全灭)
    playhead: null, // §2.6
    segments: null, // §2.8(合并后的全轨段表视图)
    coverage: {}, // ch → coveragePct(§2.7)
    errors: new Map(), // §2.9 code → payload(active:false 即删)
    unknownCodes: [], // §5.1 降级纪律①:未知 code 原样入诊断区
    readOnly: false, // secondOutput / conn.outputReadOnly
    noTimeline: false, // §5.1 noTimeline
    loopMissing: false, // §1.8 的 noLoop 拒绝态(档位 disabled 但不隐藏)
    session: {
        // 本工程会话内的一次性判定(不入 state chunk)
        writeConfirmSeen: false,
        printDoneUntil: 0,
        wasPrinting: false,
        guideClosed: false,
        // C++ 侧 `{rejected:"printing"}` 的**渲染窗口**(§5.6「UI 与 C++ 双保险」的 C++ 半边命中时)。
        // 之所以要一个时间戳而不是就地 setAttribute:renderHeader 每次都按 phase 重算 chip 的
        // data-disabled,就地写会在同一拍被抹回 0(UI 与引擎状态不同步时 phase 恰恰不是 print)。
        rejectedPrintingUntil: 0,
    },
};

/** 拒绝态提示的挂留时长(与 footer 打印结束提示同族的一次性可见反馈)。 */
const REJECT_HINT_MS = 4000;

// ------------------------------------------------------------- 小工具
// (两位轨号 tt() 随 Tab2 行模板一并迁去 tab-tracks.js —— 外壳层已用不到)
function $(gb) {
    return document.querySelector(`[data-gb="${gb}"]`);
}

async function call(name, ...args) {
    if (!bridge || typeof bridge[name] !== "function") return null;
    try {
        return await bridge[name](...args);
    } catch (e) {
        console.warn(`SCVB Output:bridge.${name}() 调用失败 —— ${e.message}`);
        return null;
    }
}

/** 词条 + 占位符 → textContent(applyI18n 写整串,填充必须排在它之后)。 */
function fill(node, key, vals) {
    if (!node) return;
    if (!Object.prototype.hasOwnProperty.call(dictNow, key)) return;
    node.textContent = format(dictNow[key], vals || {});
}

/** 切 data-t(同一节点在两条词条间切换,如 footer.printing ↔ .follow)后再填。 */
function fillKeyed(node, key, vals) {
    if (!node) return;
    node.setAttribute("data-t", key);
    fill(node, key, vals);
}

// ------------------------------------------------------------- i18n
let lang = "zh";
let dictNow = {};

// 「拆格」钩子:data-t-split="key" 的容器把一条词条按 " · " 拆成 N 个等距格子。
// 用途 = 设计稿里 justify-content:space-between 的刻度行(分布图底轴 L552-554、
// 曲线窗 X 轴 L499-501)。applyI18n 只认 data-t / data-t-aria,写 data-t 会被整串覆盖,
// 故另立本钩子;词条仍是字典里的单条(零新增自由文案)。
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
    render();
}

function setLang(next, { push = true } = {}) {
    if (!LANGS.includes(next)) return;
    lang = next;
    document
        .querySelectorAll(
            '[data-lang="zh"], [data-lang="en"], [data-lang="fr"]',
        )
        .forEach((btn) => {
            // 语言胶囊是普通 button(非 role="tab"),可达属性用 aria-pressed;
            // aria-selected 只在 role="tab" 的四个主 tab 上使用(见 activateTab)。
            btn.setAttribute(
                "aria-pressed",
                String(btn.getAttribute("data-lang") === lang),
            );
        });
    refreshI18n();
    // 契约 §1.30:写 state ui.language;未知 code 由 C++ 回退 zh 并经 scvb.state 回推。
    if (push) call("setLang", next);
}

document.querySelectorAll("[data-lang]").forEach((btn) => {
    btn.addEventListener("click", () => setLang(btn.getAttribute("data-lang")));
});

// ------------------------------------------------------------- tab 路由 + 石板滑块(05 §2.0)
// 滑块几何逐字照设计稿 1874-1881:left = 3px(条壳 padding)+ (tab-1) × 136px,宽恒 136px。
// 两个几何常量的真源是 index.html 的 --sp-3 与 --tab-w,这里读回来算 left 写成行内 px
// (不读 offsetLeft/offsetWidth —— 首帧布局未定时它们为 0,正是灰模「滑块只盖住最左一点」的成因)。
const content = document.getElementById("content");
const tabbar = document.getElementById("tabbar");
const slate = document.getElementById("tabs-slate");
const TAB_ORDER = ["master", "tracks", "wave", "settings"];

function slateLeftPx(idx) {
    // 兜底值 = 设计稿 L287 的 padding:3px 与 L1877/L1681 的 136px。
    const cs =
        typeof window !== "undefined" && window.getComputedStyle
            ? window.getComputedStyle(tabbar)
            : null;
    const pad = (cs && parseFloat(cs.getPropertyValue("--sp-3"))) || 3;
    const w = (cs && parseFloat(cs.getPropertyValue("--tab-w"))) || 136;
    return pad + idx * w;
}

function activateTab(name, { push = true } = {}) {
    const idx = TAB_ORDER.indexOf(name);
    if (idx < 0) return;
    content.setAttribute("data-tab", name);
    tabbar.style.setProperty("--tab-i", String(idx));
    if (slate) slate.style.left = slateLeftPx(idx) + "px";
    tabbar.querySelectorAll("[data-tab-btn]").forEach((btn) => {
        btn.setAttribute(
            "aria-selected",
            String(btn.getAttribute("data-tab-btn") === name),
        );
    });
    // 契约 §1.31:写 state ui.active_tab(重开面板恢复上次 tab);
    // tour 期间的跨 tab 翻页是纯 UI 本地态,不经此口(§2.6)—— 故 push 可关。
    if (push) call("setActiveTab", name);
}

tabbar.querySelectorAll("[data-tab-btn]").forEach((btn) => {
    btn.addEventListener("click", () =>
        activateTab(btn.getAttribute("data-tab-btn")),
    );
});

activateTab("master", { push: false });

// --------------------------------------- Tab1 ① 三件套开关的 data-on / aria 派生(单一状态源)
// 状态真源 = .master-flow 上的 data-cap / data-out(05 §1.3);开关的**视觉**真源 =
// base.css 的 `.sc-toggle[data-on="1"]`。两者之间只允许一条派生边,写在这里:
//     data-cap ∈ {armed, capturing} ⇒ .cap-toggle[data-on="1"][aria-checked="true"]
//     data-out === "1"              ⇒ .out-toggle[data-on="1"][aria-checked="true"]
// MutationObserver 让「事件写属性」与「DevTools 手改属性静态验证四态」走同一条派生。
// 冒烟不变式:任何 data-cap ∈ {armed, capturing} ⇒ cap-toggle 的 aria-checked === "true"。
const masterFlow = $("master-flow");

function syncFlowToggles() {
    if (!masterFlow) return;
    const cap = masterFlow.getAttribute("data-cap");
    const pairs = [
        [".cap-toggle", cap === "armed" || cap === "capturing"],
        [".out-toggle", masterFlow.getAttribute("data-out") === "1"],
    ];
    for (const [sel, on] of pairs) {
        const el = masterFlow.querySelector(sel);
        if (!el) continue;
        el.setAttribute("data-on", on ? "1" : "0");
        el.setAttribute("aria-checked", String(on));
    }
}

if (masterFlow) {
    syncFlowToggles();
    new MutationObserver(syncFlowToggles).observe(masterFlow, {
        attributes: true,
        attributeFilter: ["data-cap", "data-out"],
    });
}

// ------------------------------------------------------------- 光标跟随高光(05 §4「光标发光」)
// document 级**单**监听(15 轨密排下不能每行挂监听),逐字照设计稿 L1474-1489:
// 坐标写在**被悬停的元素自己**身上(--mx/--my 是 [data-glow]::after 的圆心,必须是局部坐标)。
let glowLast = null;
document.addEventListener("pointermove", (e) => {
    const el =
        e.target instanceof Element ? e.target.closest("[data-glow]") : null;
    if (glowLast && glowLast !== el)
        glowLast.style.setProperty("--glow-o", "0");
    if (el) {
        const r = el.getBoundingClientRect();
        el.style.setProperty("--mx", e.clientX - r.left + "px");
        el.style.setProperty("--my", e.clientY - r.top + "px");
        el.style.setProperty("--glow-o", "1");
    }
    glowLast = el;
});

// ------------------------------------------------------------- Tab1(tab-master.js)
const tabMaster = createTabMaster({
    root: document,
    bridge,
    getStore: () => store,
    getT: () => dictNow,
    onLocalChange: () => render(),
});
tabMaster.mount();

// ------------------------------------------------------------- Tab2(tab-tracks.js)
// T32:列头 + 15 轨行矩阵 + 图例脚注全部由 createTabTracks() 生成(列宽单一真源 =
// tab-tracks.js 的 TRACK_COLS,页面与 app.js 都不写第二份);Wave 2 起 15 行的值
// 全部由事件算出,行内全部上行调用也在该文件(本文件只做订阅转发)。
const tabTracks = createTabTracks({
    root: document,
    bridge,
    getStore: () => store,
    getT: () => dictNow,
    onLocalChange: () => render(),
});
tabTracks.mount();

// ------------------------------------------------------------- Tab4(tab-settings.js)
// T35:缩放 select 与语言胶囊的接线仍在本文件(scale/语言是外壳级),Tab4 的
// 其余全部内容(两设置块 / 说明块 / 存储 / 诊断 / 版本号)在 createTabSettings。
const tabSettings = createTabSettings({
    root: document,
    bridge,
    getStore: () => store,
    getT: () => dictNow,
    onLocalChange: () => render(),
});
tabSettings.mount();

// ------------------------------------------------------------- Tab3:15 泳道模板生成(05 §2.3)
// 轨头/曲线叠加层/特征波形/VAD 着色/分段边界/采集覆盖进度 六件套,画布仅给尺寸注释,
// 零绘制逻辑。播放头与选区手柄是共享覆盖层,已在 index.html 静态占位,不随行生成。
function waveLaneHtml(ch, label) {
    // 泳道不是表格行(无 columnheader 对位,轨头里还有 checkbox),原先的 role="row"
    // 既无 table/grid 祖先、子元素也不是 cell,axe 会同时报 aria-required-parent 与
    // aria-required-children 两条 critical;这里去掉 role,保留纯视觉容器语义。
    return `
    <div class="wave-lane" data-gb="wave-lane-${ch}" data-ch="${ch}">
      <div class="wave-lane__head" data-gb="wave-lane-${ch}-head">
        <input type="checkbox" data-gb="wave-lane-${ch}-checkbox" aria-label="select track ${ch}" />
        <span class="wave-lane__label" data-gb="wave-lane-${ch}-label">${label}</span>
        <span class="sc-dot" data-tone="gray"></span>
        <!-- 05 §2.3 泳道轨头行状态列:有效唱段 <1.5s 黄标「样本不足」(04 §7 步7;与 Tab2
             tracks-row-{ch}-lowsample 同款,词条 lowSample 字典已备,不需 todo)。 -->
        <span class="sc-badge--amber" data-t="lowSample" data-gb="wave-lane-${ch}-lowsample" hidden></span>
        <span class="sc-mono" data-gb="wave-lane-${ch}-coverage">0%</span>
        <span class="sc-mono" data-gb="wave-lane-${ch}-segcount">0</span>
        <!-- 词条待立:「曲线可见」toggle 无独立 tooltip 词条(05 §2.3 泳道,字典缺) -->
        <span class="sc-toggle" data-on="1" style="width:20px" data-gb="wave-lane-${ch}-curvevisible" data-gb-todo="词条待立"></span>
      </div>
      <div class="sc-dark wave-lane__stage" data-gb="wave-lane-${ch}-stage">
        <!-- 画布尺寸注释:随泳道区宽度撑满、高 60 设计 px,零绘制逻辑,T33 实现 -->
        <!-- [T3x] bridge.requestWaveform(ch, startS, endS, cols) —— 契约 §1.27
             (05 §2.3 泳道「特征波形」行的数据来源 = 拉取式分块;异步 Promise,
             每次调用恰好一次 completion;VAD 着色取同一返回的 .vad[]) -->
        <canvas class="wave-lane__waveform" data-gb="wave-lane-${ch}-waveform" width="900" height="60"></canvas>
        <canvas class="wave-lane__vad" data-gb="wave-lane-${ch}-vad" width="900" height="60"></canvas>
        <canvas class="wave-lane__curve" data-gb="wave-lane-${ch}-curve" width="900" height="60"></canvas>
        <div class="wave-lane__boundaries" data-gb="wave-lane-${ch}-boundaries"></div>
        <div class="wave-lane__coverage" data-gb="wave-lane-${ch}-coverage-bar"></div>
      </div>
    </div>`;
}

const waveLanes = document.getElementById("wave-lanes");
if (waveLanes) {
    // insertAdjacentHTML("afterbegin") 插在既有子节点(选区手柄/播放头,index.html 静态占位)
    // 之前——两者是 position:absolute 覆盖层,留在后面的 DOM 顺序保证层叠在泳道之上。
    const lanesHtml = FIFTEEN_TRACKS.labels
        .map((label, i) => waveLaneHtml(i + 1, label))
        .join("");
    waveLanes.insertAdjacentHTML("afterbegin", lanesHtml);
}

// ------------------------------------------------------------- 缩放档位(footer 下拉 + 设置页,05 §1.2)
// 档位表单一真源 = web/shared/design-box.js 的 DESIGN.output.presets(05 §1.2「常量真源」栏)。
// 「超出当前屏幕」判据(05 §1.2):W*F > screen.availWidth ∨ H*F > screen.availHeight
// → 档位灰掉并提示,绝不隐藏。
function scaleOverflows(f) {
    const s = typeof window !== "undefined" ? window.screen : null;
    if (!s || !s.availWidth || !s.availHeight) return false;
    return (
        DESIGN.output.w * f > s.availWidth ||
        DESIGN.output.h * f > s.availHeight
    );
}

const scalePanel = $("footer-scale-panel");
if (scalePanel) {
    scalePanel.innerHTML = DESIGN.output.presets
        .map((f) => {
            const over = scaleOverflows(f) ? 1 : 0;
            return `
      <button class="footer-scale__opt" role="option" aria-selected="false"
              data-scale="${f}" data-current="0" data-over="${over}"
              data-gb="footer-scale-opt-${Math.round(f * 100)}">
        <span>${Math.round(f * 100)}%</span>
        <span class="footer-scale__note"></span>
      </button>`;
        })
        .join("");
}

for (const sel of document.querySelectorAll(
    '[data-gb="settings-scale-select"]',
)) {
    for (const f of DESIGN.output.presets) {
        const opt = document.createElement("option");
        opt.value = String(f);
        opt.textContent = Math.round(f * 100) + "%";
        if (f === 1) opt.selected = true;
        sel.appendChild(opt);
    }
}

// ============================================================================
// 缩放:10 秒防呆(契约 §1.28/§1.29;05 §1.2)
// setUiScale = 立即预览;10 秒内不点「保持」就回退到进入本次预览前的档位,
// 「保持」才 commitUiScale() 落盘系统级全局默认。取消 / 超时 / 关窗一律回退。
// ============================================================================
const scaleUi = {
    trigger: $("footer-scale-trigger"),
    wrap: $("footer-scale-select"),
    label: $("footer-scale-label"),
    confirm: $("scale-confirm"),
    countdown: $("scale-confirm-countdown"),
    bodyPre: $("scale-confirm-body-pre"),
    bodyPost: $("scale-confirm-body-post"),
    keep: $("scale-confirm-keep"),
    revert: $("scale-confirm-revert"),
};
let scaleTimer = 0;
// hostEcho 灰显的退出定时器(批次停发后补一拍 render;见 scvb.params 订阅)。
let hostEchoTimer = 0;
let scalePrev = 1;
let scaleLeft = 0;

function currentScale() {
    const ui = store.state.ui;
    return ui && Number.isFinite(ui.scale) ? ui.scale : 1;
}

function openScalePanel(open) {
    if (!scaleUi.wrap) return;
    scaleUi.wrap.setAttribute("data-open", open ? "1" : "0");
    if (scaleUi.trigger) {
        scaleUi.trigger.setAttribute("aria-expanded", String(open));
    }
}

function stopScaleCountdown() {
    if (scaleTimer) clearInterval(scaleTimer);
    scaleTimer = 0;
    if (scaleUi.confirm) scaleUi.confirm.hidden = true;
}

/**
 * 确认框正文 = 词条 `scale.confirmBody`(带 {s} 占位符)按 {s} 切成前缀/数字/后缀三段。
 * 为什么不挂 data-t:applyI18n 写整串 textContent,会把中间那个倒计时 span 一并抹掉。
 * 切语言(refreshI18n → render)与每一拍倒计时都重写,故三语与秒数同时正确。
 */
function renderScaleConfirm() {
    const raw = dictNow["scale.confirmBody"];
    if (typeof raw === "string") {
        const [pre, post] = raw.split("{s}");
        if (scaleUi.bodyPre) scaleUi.bodyPre.textContent = pre;
        if (scaleUi.bodyPost) {
            scaleUi.bodyPost.textContent = post === undefined ? "" : post;
        }
    }
    // 没有倒计时在跑时不动那个数字(确认框此刻是 hidden 的,写个 0 进去只会让
    // 下次打开前的静态兜底值变成 0)。
    if (scaleUi.countdown && scaleLeft > 0) {
        scaleUi.countdown.textContent = String(scaleLeft);
    }
}

async function previewScale(f) {
    // **嵌套预览**:倒计时还在跑时再选一档,回退目标必须仍是「进入本次预览**前**」的档位。
    // currentScale() 此刻读到的是上一次预览值(state 已回推),重新采样会把回退终点钉在
    // 中途那一档 —— 05 §1.2「取消/超时/关窗都回退」说的是回到用户改档前的样子。
    if (!scaleTimer) scalePrev = currentScale();
    const res = await call("setUiScale", f);
    if (res && res.ok === false) return; // §1.28 拒绝态:不在档位表 → badArg
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

function revertScale() {
    stopScaleCountdown();
    call("setUiScale", scalePrev);
}

if (scaleUi.trigger) {
    scaleUi.trigger.addEventListener("click", () => {
        const open = scaleUi.wrap.getAttribute("data-open") === "1";
        openScalePanel(!open);
    });
}
if (scalePanel) {
    scalePanel.addEventListener("click", (e) => {
        const opt =
            e.target instanceof Element
                ? e.target.closest("[data-scale]")
                : null;
        if (!opt) return;
        openScalePanel(false);
        // 超屏档位灰掉但不隐藏(05 §1.2)—— 点它不生效
        if (opt.getAttribute("data-over") === "1") return;
        previewScale(Number(opt.getAttribute("data-scale")));
    });
}
for (const sel of document.querySelectorAll(
    '[data-gb="settings-scale-select"]',
)) {
    sel.addEventListener("change", () => previewScale(Number(sel.value)));
}
if (scaleUi.keep) {
    scaleUi.keep.addEventListener("click", () => {
        stopScaleCountdown();
        call("commitUiScale");
    });
}
if (scaleUi.revert) scaleUi.revert.addEventListener("click", revertScale);
addEventListener("pagehide", stopScaleCountdown); // 关窗回退,不污染新实例

// ============================================================================
// Header 版本区(契约 §1.9/§1.10/§1.11;05 §2.1 ③)
// ============================================================================
const verUi = {
    box: $("header-version"),
    chips: [$("header-version-chip-1"), $("header-version-chip-2")],
    rename: $("header-version-rename"),
    renameChip0: $("header-version-rename-chip0"),
    renameInput: $("header-version-rename-input"),
    renameCount: $("header-version-rename-count"),
    copyBtn: $("header-version-copy"),
    copyConfirm: $("header-version-copy-confirm"),
    copyOk: $("header-version-copy-ok"),
    copyCancel: $("header-version-copy-cancel"),
    armedConfirm: $("header-version-armed-confirm"),
    armedOk: $("header-version-armed-ok"),
    armedCancel: $("header-version-armed-cancel"),
};
let renamingVersion = 0;
let armedPendingVersion = 0;

function versionName(v) {
    const arr = store.state.versions || [];
    const entry = arr[v - 1];
    return entry && entry.name ? entry.name : "V" + v;
}

/**
 * 契约 §1.9 拒绝态:PRINT 态 C++ 硬拒绝 → `{rejected:"printing"}`。
 * 能走到这里的只有「UI 以为不是 PRINT、引擎说是」的不同步场景(UI 半边的 disabled 在
 * §5.6 里是**双保险的另一半**,不是唯一防线)——此时把拒绝记进 store 并由 renderHeader
 * 统一渲染(chip 整组 disabled + tooltip),而不是就地 setAttribute 被下一拍抹掉。
 */
function noteRejectedPrinting() {
    store.session.rejectedPrintingUntil = Date.now() + REJECT_HINT_MS;
    setTimeout(render, REJECT_HINT_MS + 50);
}

async function switchVersion(v) {
    const res = await call("setVersionActive", v);
    if (res && res.rejected === "printing") noteRejectedPrinting();
    render();
}

verUi.chips.forEach((chip, i) => {
    if (!chip) return;
    const v = i + 1;
    chip.addEventListener("click", () => {
        if (chip.getAttribute("data-disabled") === "1") return;
        if (v === (store.state.global || {}).version_active) return;
        const phase = outputPhase(store.state, store.playhead);
        if (phase === "print") return; // 05 §2.1 ③:PRINT 不再弹确认,直接不可切
        if (phase === "armed") {
            // ARMED 轻确认(词条 master.versionArmedConfirm)
            armedPendingVersion = v;
            if (verUi.armedConfirm) verUi.armedConfirm.hidden = false;
            return;
        }
        switchVersion(v); // FOLLOW:直接切
    });
    // 双击 chip 就地重命名(行内 input,≤16 字符,Enter 提交,空值回落默认 "V{n}")
    chip.addEventListener("dblclick", () => {
        if (chip.getAttribute("data-disabled") === "1") return;
        renamingVersion = v;
        if (verUi.box) verUi.box.setAttribute("data-mode", "rename");
        if (verUi.renameChip0) verUi.renameChip0.textContent = "V" + v;
        if (verUi.renameInput) {
            verUi.renameInput.value = versionName(v);
            verUi.renameInput.focus();
            verUi.renameInput.select();
        }
        updateRenameCount();
    });
});

function updateRenameCount() {
    if (!verUi.renameCount || !verUi.renameInput) return;
    // 码点计数(与 §1.10 C++ 截断口径一致;.length 是 UTF-16 码元,增补平面字符会虚高)
    verUi.renameCount.textContent =
        Array.from(verUi.renameInput.value).length + "/16";
}

function endRename(commit) {
    const v = renamingVersion;
    renamingVersion = 0;
    if (verUi.box) verUi.box.setAttribute("data-mode", "normal");
    if (!commit || !v) return;
    const raw = verUi.renameInput ? verUi.renameInput.value : "";
    // 契约 §1.10:≤16 由 C++ 截断、空串/纯空白回落默认 "V{v}";返回回显实际落盘名。
    call("setVersionName", v, raw).then((res) => {
        if (res && typeof res.name === "string") {
            const versions = (store.state.versions || []).slice();
            if (versions[v - 1]) {
                versions[v - 1] = { ...versions[v - 1], name: res.name };
                store.state = { ...store.state, versions };
            }
        }
        render();
    });
}

if (verUi.renameInput) {
    verUi.renameInput.addEventListener("input", updateRenameCount);
    verUi.renameInput.addEventListener("blur", () => endRename(true));
    verUi.renameInput.addEventListener("keydown", (e) => {
        if (e.key === "Enter") {
            e.preventDefault();
            verUi.renameInput.blur();
        } else if (e.key === "Escape") {
            e.preventDefault();
            endRename(false);
        }
    });
}

if (verUi.armedOk) {
    verUi.armedOk.addEventListener("click", () => {
        const v = armedPendingVersion;
        armedPendingVersion = 0;
        if (verUi.armedConfirm) verUi.armedConfirm.hidden = true;
        if (v) switchVersion(v);
    });
}
if (verUi.armedCancel) {
    verUi.armedCancel.addEventListener("click", () => {
        armedPendingVersion = 0;
        if (verUi.armedConfirm) verUi.armedConfirm.hidden = true;
    });
}

if (verUi.copyBtn) {
    verUi.copyBtn.addEventListener("click", () => {
        // 05 §2.1 ③:PRINT 态按钮 disabled + tooltip「打印中不可复制版本」(渲染在 renderHeader)。
        if (verUi.copyBtn.getAttribute("data-disabled") === "1") return;
        if (verUi.copyConfirm) verUi.copyConfirm.hidden = false;
        render();
    });
}
if (verUi.copyCancel) {
    verUi.copyCancel.addEventListener("click", () => {
        if (verUi.copyConfirm) verUi.copyConfirm.hidden = true;
    });
}
if (verUi.copyOk) {
    verUi.copyOk.addEventListener("click", async () => {
        if (verUi.copyConfirm) verUi.copyConfirm.hidden = true;
        const src = (store.state.global || {}).version_active || 1;
        const dst = src === 1 ? 2 : 1;
        // 契约 §1.11(入撤销栈,确认框末句已写「可撤销(Ctrl+Z)」);
        // PRINT 态 C++ 硬拒绝 {rejected:"printing"} —— 与 setVersionActive 同款渲染,不静默。
        const res = await call("copyVersion", src, dst);
        if (res && res.rejected === "printing") noteRejectedPrinting();
        render();
    });
}

// ============================================================================
// 加载守卫(横幅⑦ + 输出开关小锁;契约 §1.34 / §2.1 print_guard)
// ============================================================================
const guardBtn = $("banner-printGuard-confirm");
if (guardBtn) {
    guardBtn.addEventListener("click", async () => {
        const res = await call("confirmPrintGuard"); // 幂等;确认后经 scvb.state.print_guard 回推
        // 05 §2.0 横幅⑦逐字:与 §2.1 输出开关的 OFF→ON 一次性确认**互斥**——守卫已确认
        // 即不再补弹。守卫本身就是「输出 ON 且后果已被告知」的确认,此后再 OFF→ON 不该
        // 又弹一遍 write 确认条,故在这里把本会话的一次性判定一并置真。
        if (!res || res.ok !== false) store.session.writeConfirmSeen = true;
        render();
    });
}

// ============================================================================
// 首启引导页(05 §2.0;契约 §1.32 setGuideSeen)
// ============================================================================
const guideUi = {
    overlay: $("guide-overlay"),
    rules: $("guide-overlay-rules"),
    missing: $("guide-overlay-rules-missing"),
    dontShow: $("guide-overlay-dontshow"),
    start: $("guide-overlay-start"),
};
const GUIDE_RULE_KEYS = Array.from(
    { length: 9 },
    (_, i) => "guide.rule" + (i + 1),
);

/**
 * 九条红字 = scripts/gen-hard-rules.mjs 从 docs/USER_GUIDE.zh-CN.md#硬约束 生成的产物
 * (05 §0.1/§5:任何位置禁止手抄)。生成器归 T39b,当前字典没有这九个 key ——
 * 此时**不留空**,改渲染醒目占位注记,免得引导页看起来像坏页(差异已记 deviations)。
 */
function renderGuideRules() {
    if (!guideUi.rules) return;
    const have = GUIDE_RULE_KEYS.filter((k) =>
        Object.prototype.hasOwnProperty.call(dictNow, k),
    );
    if (have.length === GUIDE_RULE_KEYS.length) {
        guideUi.rules.replaceChildren(
            ...GUIDE_RULE_KEYS.map((k) => {
                const li = document.createElement("li");
                li.setAttribute("data-t", k);
                li.textContent = dictNow[k];
                return li;
            }),
        );
        if (guideUi.missing) guideUi.missing.hidden = true;
    } else {
        guideUi.rules.replaceChildren();
        if (guideUi.missing) guideUi.missing.hidden = false;
    }
}

if (guideUi.start) {
    guideUi.start.addEventListener("click", () => {
        store.session.guideClosed = true;
        // params-v0 [J50]:guide_seen = 首启引导页「已读标记」——正常关闭即已读,
        // 必须落**工程位**,否则重开工程引导页又弹(PR #52 bot 开放项 1)。
        // 「不再显示」勾选才加写全局位(契约 §1.32 alsoGlobal;跨工程承诺)。
        const alsoGlobal = !!(guideUi.dontShow && guideUi.dontShow.checked);
        call("setGuideSeen", true, alsoGlobal);
        if (guideUi.overlay) guideUi.overlay.hidden = true;
    });
}

// ============================================================================
// 键盘(05 §1.3 撤销/重做;契约 §1.25/§1.26)
// ============================================================================
document.addEventListener(
    "keydown",
    (e) => {
        if (!(e.ctrlKey || e.metaKey) || e.key.toLowerCase() !== "z") return;
        // 焦点在文本输入框时不拦截(05 §1.3)
        const a = document.activeElement;
        if (
            a &&
            (a.tagName === "INPUT" ||
                a.tagName === "TEXTAREA" ||
                a.isContentEditable)
        ) {
            return;
        }
        e.preventDefault(); // 防止冒泡到宿主撤销栈
        call(e.shiftKey ? "redo" : "undo");
    },
    true,
);

// ============================================================================
// 渲染:外壳(header / 横幅 / footer / 引导页)+ Tab1
// ============================================================================
function render() {
    renderHeader();
    renderBanners();
    renderFooter();
    renderScale();
    renderGuide();
    tabMaster.render();
    tabTracks.render();
    tabSettings.render();
}

function renderHeader() {
    const s = store.state;
    const g = s.global || {};
    const phase = outputPhase(s, store.playhead);

    // 连接摘要 pill:N/15 只统计 slotState=2 ∧ heartbeatFresh(契约 §2.3,J01)
    const n = connectedCount(store.conn);
    const pill = $("header-conn-pill");
    const count = $("header-conn-count");
    if (count) count.textContent = n + "/" + CHANNEL_COUNT;
    if (pill) {
        pill.setAttribute("data-tone", n > 0 ? "green" : "gray");
        pill.setAttribute("data-pulse", n > 0 ? "1" : "0");
        const label = pill.querySelector("[data-t]");
        if (label) {
            fillKeyed(
                label,
                n > 0 ? "state.connected" : "state.notConnected",
                {},
            );
        }
    }

    // 独立「组 {X}」badge(J71③)
    const letter = $("header-group-badge-letter");
    if (letter) letter.textContent = GROUP_IDS[(s.group_id || 1) - 1];

    // 版本 chip 五态:data-active / data-empty / data-disabled(+ 外层 data-mode="rename")
    // disabled 的两个来源:①UI 自己判定的 PRINT 态(05 §2.1 ③);②C++ 回的
    // `{rejected:"printing"}`(§5.6 双保险的另一半,UI 与引擎不同步时才命中)。
    const active = g.version_active || 1;
    const rejected = Date.now() < store.session.rejectedPrintingUntil;
    const printLocked = phase === "print" || rejected;
    verUi.chips.forEach((chip, i) => {
        if (!chip) return;
        const v = i + 1;
        const entry = (s.versions || [])[i] || {};
        chip.setAttribute("data-active", v === active ? "1" : "0");
        chip.setAttribute("aria-pressed", String(v === active));
        chip.setAttribute("data-empty", entry.empty ? "1" : "0");
        chip.setAttribute("data-disabled", printLocked ? "1" : "0");
        chip.setAttribute("aria-disabled", String(printLocked));
        // 05 §2.1 ③ 逐字要求 disabled **+ tooltip**(title 不走 applyI18n,每次渲染重写)
        setTitle(chip, printLocked ? dictNow["master.printLock.version"] : "");
        // 名字是 state 里的用户串,只能进 textContent(不拼 HTML)
        chip.childNodes[0].nodeValue = versionName(v);
    });
    // 「复制到…」同款:05 §2.1 ③「PRINT 态按钮 disabled + tooltip『打印中不可复制版本』」
    if (verUi.copyBtn) {
        verUi.copyBtn.setAttribute("data-disabled", printLocked ? "1" : "0");
        verUi.copyBtn.setAttribute("aria-disabled", String(printLocked));
        setTitle(
            verUi.copyBtn,
            printLocked ? dictNow["master.printLock.copy"] : "",
        );
    }
    if (verUi.box && renamingVersion === 0) {
        verUi.box.setAttribute("data-mode", "normal");
    }
    // 复制确认框正文的 {name} = 目标版本名(契约 §1.11:不复制 name,故显示目标现名)
    if (verUi.copyConfirm && !verUi.copyConfirm.hidden) {
        const dst = active === 1 ? 2 : 1;
        fill(
            verUi.copyConfirm.querySelector("[data-t]"),
            "master.copyConfirmWarn",
            {
                name: versionName(dst),
            },
        );
    }
}

/**
 * 横幅落点逐字照契约 §5.1「UI 落点」列。
 * 降级纪律(§5.1):①未知 code 不静默 —— 原样入 Tab4 诊断区;
 * ②持续性条件(①-⑥)不可手动关闭,`active:false` 才撤下(所以显隐一律从 store.errors 算)。
 */
function renderBanners() {
    const s = store.state;
    const err = store.errors;
    const groupLetter = GROUP_IDS[(s.group_id || 1) - 1];

    // ① 路由失准(数据源 scvb.conn 的 misalignCount,非 error code)
    const mis = misalignedTracks(store.conn);
    show($("banner-misaligned"), mis > 0);
    fill($("banner-misaligned-text"), "banner.misaligned", { m: mis });

    // ② 组内只读观察 → 全 UI 写控件 disabled
    const second = err.get("secondOutput");
    const readOnly =
        !!second || !!(store.conn && store.conn.outputReadOnly === true);
    store.readOnly = readOnly;
    show($("banner-secondOutput"), readOnly);
    if (readOnly) {
        const gid =
            second && second.detail ? second.detail.groupId : s.group_id;
        fill(
            $("banner-secondOutput").querySelector("[data-t]"),
            "banner.secondOutput",
            { X: GROUP_IDS[(gid || 1) - 1] || groupLetter },
        );
    }

    // ③ 采样率不一致(轨级,detail = {inputSr, outputSr})
    const sr = err.get("srMismatch");
    show($("banner-srMismatch"), !!sr);
    if (sr)
        fill($("banner-srMismatch-text"), "banner.srMismatch", { n: sr.ch });

    // ④ 工程 state chunk 的 abi 高于本机 → 拒载
    const newer = err.get("newerState");
    show($("banner-versionMismatch"), !!newer);
    if (newer) {
        fill(
            $("banner-versionMismatch").querySelector("[data-t]"),
            "banner.versionMismatch",
            {
                a: (newer.detail || {}).localAbi,
                b: (newer.detail || {}).projectAbi,
            },
        );
    }

    // ⑤ 采集数据缺失/过期  ⑥ 宿主未提供时间线(后者同时 disable 采集/输出开关)
    show($("banner-sidecarMissing"), err.has("sidecarMissing"));
    store.noTimeline = err.has("noTimeline");
    show($("banner-noTimeline"), store.noTimeline);

    // ⑦ 加载守卫(数据源 scvb.state.print_guard,不是 error code)
    show($("banner-printGuard"), !!(s.print_guard && s.print_guard.pending));

    // toast:一次性提示(§5.1 降级纪律②:可关闭)
    show($("toast-projectCopy"), err.has("projectCopy"));
    show($("toast-sidecarSwitched"), err.has("sidecarSwitched"));

    // 未知 code:原样显示并入 Tab4 诊断区(ADR-002 / ipc §5,UI 不静默)
    const diag = $("settings-diagnostics-list");
    if (diag && store.unknownCodes.length) {
        diag.replaceChildren(
            ...store.unknownCodes.map((code) => {
                const li = document.createElement("li");
                li.textContent = code;
                return li;
            }),
        );
    }
}

function show(node, on) {
    if (node) node.hidden = !on;
}

/** disabled 类 tooltip:词条为空就移除 title(空 title 在部分 WebView 里仍弹空气泡)。 */
function setTitle(node, text) {
    if (!node) return;
    if (text) node.setAttribute("title", text);
    else node.removeAttribute("title");
}

function renderFooter() {
    const s = store.state;
    const g = s.global || {};
    const range = g.range || { mode: "follow", start_s: 0, end_s: 0 };
    const footer = $("footer");
    const printing = outputPhase(s, store.playhead) === "print";

    // print → printDone 的一次性切换(打印结束提示只挂 8 秒,之后回默认提示行)
    if (store.session.wasPrinting && !printing) {
        store.session.printDoneUntil = Date.now() + 8000;
    }
    store.session.wasPrinting = printing;

    let mode = "default";
    if (printing) mode = range.mode === "follow" ? "follow" : "print";
    else if (Date.now() < store.session.printDoneUntil) mode = "printDone";
    if (footer) footer.setAttribute("data-mode", mode);

    // {x}/{y} 一律填 **mm:ss.mmm**(桥面单位,契约 §1.8/§0.2 第 3 条:UI 永不见样本、只收秒),
    // 与 Range 卡可编辑侧、write 确认条同一单位。词条 footer.printing 的 zh 写作
    // 「{x}–{y} 小节」/ en「BARS」/ fr「MESURES」—— **桥面没有宿主 tempo map 入口**(A17),
    // 小节值算不出来;填裸秒数会读成「12–96 小节」(明确错标),填时间码至少不撒谎。
    // 该单位词待统筹按 A17/A19 同款裁定(deviations A26),拿到 tempo map(T33)后回填小节。
    const vName = versionName(g.version_active || 1);
    fillKeyed($("footer-print-status"), footerPrintKey(range.mode, false), {
        v: vName,
        x: secondsToTimecode(range.start_s),
        y: secondsToTimecode(range.end_s),
    });
    fillKeyed($("footer-print-done"), footerPrintKey(range.mode, true), {
        x: secondsToTimecode(range.start_s),
        y: secondsToTimecode(range.end_s),
    });

    // 重采集布防 badge③:输出开关 ON 且布防中 → footer 追加琥珀警告(05 §2.3)
    const rec = s.recapture;
    show($("footer-recapture-warn"), !!(rec && rec.armed && g.output_enabled));

    // 版本号(§1.1 快照的 version.plugin;快照没到之前保持 HTML 里的占位)
    const ver = $("footer-version");
    if (ver && store.snapshot && store.snapshot.version) {
        ver.textContent = "v" + store.snapshot.version.plugin;
    }
}

function renderScale() {
    const f = currentScale();
    renderScaleConfirm(); // 切语言后确认框正文跟着换(倒计时进行中也照刷)
    if (scaleUi.label) scaleUi.label.textContent = Math.round(f * 100) + "%";
    if (!scalePanel) return;
    for (const opt of scalePanel.querySelectorAll("[data-scale]")) {
        const v = Number(opt.getAttribute("data-scale"));
        const cur = v === f ? 1 : 0;
        const over = opt.getAttribute("data-over") === "1";
        opt.setAttribute("data-current", String(cur));
        opt.setAttribute("aria-selected", String(cur === 1));
        const note = opt.querySelector(".footer-scale__note");
        if (note) {
            const key = over ? "scale.overflow" : cur ? "scale.current" : "";
            if (key) fillKeyed(note, key, {});
            else {
                note.removeAttribute("data-t");
                note.textContent = "";
            }
        }
    }
}

function renderGuide() {
    if (!guideUi.overlay) return;
    renderGuideRules();
    // 首启判定归 tab-master.js 的 shouldShowGuide()(纯函数,node 侧可断言)。
    guideUi.overlay.hidden = !shouldShowGuide(
        store.state,
        store.ready ? store.snapshot : null,
        store.session.guideClosed,
    );
}

// ============================================================================
// 事件订阅(契约 §2 九个事件;名字逐字照 BRIDGE_EVENTS.output)
// ============================================================================
if (bridge) {
    bridge.on("scvb.state", (s) => {
        // §2.1 字段纪律:full:true = 全量快照;full:false = 增量(只含变化子树,UI 做深合并)
        store.state =
            s && s.full ? stripFull(s) : deepMerge(store.state, stripFull(s));
        syncUiFromState();
        tabMaster.refreshPreview();
        render();
    });

    bridge.on("scvb.params", (p) => {
        // §2.2:values 是稀疏 diff;full:true 时整批替换(切版本后 C++ 全量重发)
        const values =
            p && p.full
                ? { ...p.values }
                : { ...store.params.values, ...(p && p.values) };
        store.params = {
            values,
            hostEcho: !!(p && p.hostEcho),
            // hostEcho 是「最近一批」的标志,批次停发后不会再有帧把它翻回 false(§0.4
            // 值未变不发)—— 灰显必须按**批次新鲜度**退出:记时间戳,25Hz 批次不断续命,
            // 停发 HOST_ECHO_FRESH_MS 后由下面的定时器补一拍 render 清灰。
            hostEchoAt:
                p && p.hostEcho ? Date.now() : store.params.hostEchoAt || 0,
            full: !!(p && p.full),
            versionActive: (p && p.versionActive) || store.params.versionActive,
        };
        if (p && p.hostEcho) {
            clearTimeout(hostEchoTimer);
            hostEchoTimer = setTimeout(render, HOST_ECHO_FRESH_MS + 50);
        }
        // 本地乐观值让位给引擎回推(必须排在 render 之前;规则见 tab-master.js nextParamEcho):
        // 少了这一步,任一次本地拖动都会把该 ParamID 的显示**永久遮蔽**——
        // 宿主自动化回吐(hostEcho)与切版本的全量重发(full:true)都再进不了 UI。
        tabMaster.onParams(p);
        // Tab2 同款让位 + 冻结位 1→0 的解冻提示判定(05 §2.2 R2)
        tabTracks.onParams(p);
        render();
    });

    // §2.5 30 Hz:**只喂 Tab2 的 rAF 弹道,不触发 render** —— 15 行 × 30 Hz 的
    // 整页重渲染扛不住,而液柱/峰线本来就由 canvas/meter.js 逐帧直写三个属性。
    bridge.on("scvb.meters", (m) => {
        tabTracks.onMeters(m);
    });

    bridge.on("scvb.conn", (c) => {
        store.conn = c;
        render();
    });

    // §2.4:事件缺失时绿点全灭、零报错 —— 故默认值 0 且此处不做任何字段校验
    bridge.on("scvb.groups", (g) => {
        store.groups = (g && g.groups_online) || 0;
        render();
    });

    bridge.on("scvb.playhead", (p) => {
        store.playhead = p;
        render();
    });

    bridge.on("scvb.segments", (seg) => {
        store.segments = mergeSegments(store.segments, seg);
        tabMaster.onSegments(seg);
        // Tab2:手动常值的乐观显示让位给回推的段表(§1.16 的写入结果经本事件回来)
        tabTracks.onSegments(seg);
        // Tab4:全量分析完成 → 清「改后需重分析」标志(03 §6.3 applied.* 同步)
        tabSettings.onSegments(seg);
        // J69 stale:任一轨 stale → tab 导航「波形与分段」挂琥珀点
        const stale = (seg && seg.channels ? seg.channels : []).some(
            (c) => c && c.stale,
        );
        show($("tabnav-wave-stale-dot"), stale);
        render();
    });

    bridge.on("scvb.captureProgress", (cp) => {
        // §2.7 是增量事件:`coveragePct` 是该轨在 range 内的当前覆盖率(直接覆盖),
        // `addedRanges` 是本帧新增区间 —— Tab1 只消费前者(泳道底部的 2px 覆盖条归 T33)。
        for (const c of (cp && cp.channels) || []) {
            store.coverage[c.ch] = c.coveragePct;
        }
        render();
    });

    bridge.on("scvb.error", (e) => {
        if (!e || !e.code) return;
        // §2.9:active 缺省视为 true;false = 条件已解除(持续性横幅据此撤下)
        if (e.active === false) store.errors.delete(e.code);
        else store.errors.set(e.code, e);
        if (!KNOWN_CODES.has(e.code) && !store.unknownCodes.includes(e.code)) {
            store.unknownCodes.push(e.code);
        }
        render();
    });
}

/** §5.1 九码;表外一律进诊断区(UI 不静默)。 */
const KNOWN_CODES = new Set([
    "srMismatch",
    "secondOutput",
    "channelConflict",
    "newerState",
    "sidecarMissing",
    "noTimeline",
    "projectCopy",
    "sidecarSwitched",
    "lowSample",
]);

/** `full` 是事件专用标志,不属 state 树,合并前摘掉免得污染快照字段集(§1.1 语义行)。 */
function stripFull(s) {
    if (!s) return {};
    const out = { ...s };
    delete out.full;
    return out;
}

/**
 * §2.8:`channels` 只含受影响轨(snapshot / versionActive 时为全部轨),
 * 段表是该轨的**完整列表**(不做段级增量)—— 所以按 ch 整条替换即可。
 */
function mergeSegments(prev, next) {
    if (!next) return prev;
    if (
        !prev ||
        next.reason === "snapshot" ||
        next.reason === "versionActive" ||
        next.reason === "copyVersion"
    ) {
        return next;
    }
    const byCh = new Map((prev.channels || []).map((c) => [c.ch, c]));
    for (const c of next.channels || []) byCh.set(c.ch, c);
    return {
        ...next,
        channels: [...byCh.values()].sort((a, b) => a.ch - b.ch),
    };
}

/** state 里的 UI 面(语言 / tab)回推到页面本地态 —— 回推不再往上行,避免自激。 */
function syncUiFromState() {
    const ui = store.state.ui || {};
    if (ui.language && ui.language !== lang && LANGS.includes(ui.language)) {
        setLang(ui.language, { push: false });
    }
    if (ui.active_tab && content.getAttribute("data-tab") !== ui.active_tab) {
        activateTab(ui.active_tab, { push: false });
    }
}

// ============================================================================
// 首帧:§0.6 mBridgeReady 门控由页面掌握 —— 装载时调一次 requestInitialState()。
// ============================================================================
refreshI18n();

(async function boot() {
    if (!bridge) {
        render();
        return;
    }
    const snap = await call("requestInitialState");
    if (snap) {
        store.snapshot = snap;
        // §1.1:快照 = state 子树 + 五个快照专属键(session_guid / version /
        // guide_seen_global / tour_seen_global / conn)。只把 state 子树并入
        // store.state,元数据留在 store.snapshot 旁路 —— 混入会让后续 §2.1
        // 增量深合并把它们当 state 字段拖着走(PR #52 bot 建议)。
        const {
            session_guid: _sg,
            version: _ver,
            guide_seen_global: _gg,
            tour_seen_global: _tg,
            conn: snapConn,
            ...stateFields
        } = snap;
        store.state = deepMerge(store.state, stripFull(stateFields));
        store.conn = snapConn || store.conn;
        store.ready = true;
        syncUiFromState();
        tabMaster.refreshPreview();
    }
    render();
})();
