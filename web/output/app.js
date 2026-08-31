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
    errorStoreKey,
    errorKeysToDrop,
    segmentsEventApplies,
    applySegmentsEvent,
    HISTORY_AVAIL_INIT,
    historyAfterCall,
    historyAfterSegments,
    GROUP_IDS,
    CHANNEL_COUNT,
    HOST_ECHO_FRESH_MS,
    HOST_ECHO_RELEASE_MS,
} from "./tab-master.js";
import {
    createTabTracks,
    hasSegmentedMaterial,
    staleTrackCount,
} from "./tab-tracks.js";
import { createTabWave } from "./tab-wave.js";
import { createTabSuggestions } from "./tab-suggestions.js";
import { createCurveEditor } from "./canvas/curve-editor.js";
import { createTabSettings } from "./tab-settings.js";
import {
    createTour,
    shouldShowTourAsk,
    shouldAutoShowTourAsk,
} from "./tour.js";
import { createLangStart, shouldShowLangStart } from "../shared/lang-start.js";
import { disableNativeContextMenu } from "../shared/context-menu.js";
import { suppressBareAltMenu } from "../shared/alt-menu.js";

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
    // §2.9 errorStoreKey(e) → payload(active:false 即删)。键 = 裸 code,**唯
    // `lowSample` 用 `lowSample#{ch}` 复合键**(轨级且会同时命中多轨,裸 code 存一条
    // 会互相覆盖);消费侧走 tab-master.js 的 lowSampleChannels(),与键形解耦。
    errors: new Map(),
    unknownCodes: [], // §5.1 降级纪律①:未知 code 原样入诊断区
    readOnly: false, // secondOutput / conn.outputReadOnly
    noTimeline: false, // §5.1 noTimeline
    loopMissing: false, // §1.8 的 noLoop 拒绝态(档位 disabled 但不隐藏)
    // 此刻宿主给不出可用循环区(§2.6 的 loopStartS/loopEndS 缺字段)。daw_loop 档下据此显示
    // 「循环区已失效,沿用上次范围」。**必须是活判定而不是常真**:这条提示原先只挂在
    // 「档位 == loop」上、无任何谓词,于是循环区完好时也一直挂着(T37 三轮 A 族 L-5)。
    loopStale: false,
    session: {
        // 本工程会话内的一次性判定(不入 state chunk)
        writeConfirmSeen: false,
        printDoneUntil: 0,
        wasPrinting: false,
        guideClosed: false,
        langChosen: false, // 首启语言选择卡本会话已选(不入 state chunk,零桥/零契约)
        // 询问步本会话已答(开始 / 暂不 / tour 已结束)。**必须与 setTourSeen 的回执解耦**:
        // 桥回执 + 下一拍 scvb.state 之前,syncTourAsk 会照旧判「该弹」,询问卡在 tour 结束
        // 的那一帧当场重弹、tour 被重放(T37 真机 bug A-2)。判定见 tour.js shouldAutoShowTourAsk。
        tourAnswered: false,
        // C++ 侧 `{rejected:"printing"}` 的**渲染窗口**(§5.6「UI 与 C++ 双保险」的 C++ 半边命中时)。
        // 之所以要一个时间戳而不是就地 setAttribute:renderHeader 每次都按 phase 重算 chip 的
        // data-disabled,就地写会在同一拍被抹回 0(UI 与引擎状态不同步时 phase 恰恰不是 print)。
        rejectedPrintingUntil: 0,
        // B-04:布防期内输出开关 ON 过的粘滞位(footer 琥珀警告的「或被打开」半边)
        recapOutputOpened: false,
        // [D1] header 撤销/重做两钮的可用性(契约无 canUndo/canRedo 信号 ⇒ 回执驱动;
        // 判据与两条不变式见 tab-master.js 的 historyAfterCall / historyAfterSegments)
        history: HISTORY_AVAIL_INIT,
    },
};

/**
 * 渲染视图切换:tour 激活期返回 demo store(纯 UI 展示层),否则返回真实事件仓 store。
 * 真实事件始终写 store(期间照常入内存、逐字节不变);只有渲染消费面被临时切到 demo。
 */
let tour = null;
let langStart = null;

function viewStore() {
    return tour && tour.isActive() ? tour.demoStore() : store;
}

/** 拒绝态提示的挂留时长(与 footer 打印结束提示同族的一次性可见反馈)。 */
const REJECT_HINT_MS = 4000;

// 渲染调度的两个模块级位(声明必须早于第一次 activateTab() —— 它在 tab 装配前
// 就跑过一次;语义与用法见文件下半部「渲染」小节的 requestRender/render)。
/** 三个 tab 装配完毕的门控:装配前 render() 直接返回。 */
let tabsReady = false;
/** 本帧已排过一次整页 render(rAF 合帧)。 */
let renderQueued = false;

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
    const was = content.getAttribute("data-tab");
    content.setAttribute("data-tab", name);
    // T33 性能批:render() 只投影**当前激活**的那个 tab(见 render 的注释),
    // 故切 tab 必须补一次整页 render —— 新页的投影可能停在上次离开时的那帧。
    if (was !== name) requestRender();
    tabbar.style.setProperty("--tab-i", String(idx));
    if (slate) slate.style.left = slateLeftPx(idx) + "px";
    tabbar.querySelectorAll("[data-tab-btn]").forEach((btn) => {
        const on = btn.getAttribute("data-tab-btn") === name;
        btn.setAttribute("aria-selected", String(on));
        // roving tabindex(WAI-ARIA tabs 模式):tablist 整体只占**一个** Tab 停靠位,
        // 停在激活项上;组内移动归 ←/→ / Home / End(见下方 keydown)。少了这一半,
        // 键盘用户要按四次 Tab 才穿过 tab 条,且 Tab 落点与视觉选中项对不上。
        btn.setAttribute("tabindex", on ? "0" : "-1");
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

// WAI-ARIA tabs 模式的键盘约定(APG「Tabs with Automatic Activation」):
// ←/→ 在组内循环移动,Home/End 跳首/末;**选中即激活**,走的是与点击完全同一条
// activateTab 路径 —— 所以凡是挂在 activateTab 上的行为(切页补 render、写
// state ui.active_tab、石板滑块)键盘与鼠标必然一致,不存在第二套分支要维护。
// Enter/Space 不接管:button 原生就会派发 click,已被上面那条接住。
// ↑/↓ 也不接管:本 tablist 是横排(aria-orientation 默认 horizontal),竖直键留给页面。
const TAB_KEYS = ["ArrowLeft", "ArrowRight", "Home", "End"];

tabbar.addEventListener("keydown", (e) => {
    // 上游已消费掉这次按键就放行 —— 任何先按下 preventDefault 的组件都自动免疫,
    // 不必在这里逐个点名(tour 的 ←/→ 就走这条:它的 keydown 挂在 document
    // **捕获**阶段且不 stopPropagation,事件照样冒泡到这里)。
    if (e.defaultPrevented) return;
    // 但 defaultPrevented 只盖得住 tour **认识**的那几个键;Home/End 它不消费,
    // 于是导览期间焦点若落在 tab 按钮上,Home/End 会把页切走而 tour 的步进机原地不动,
    // spotlight 指向一个已 display:none 的锚点。tour 活着就整体让位,这条缝才是关死的。
    if (tour && tour.isActive()) return;
    if (e.altKey || e.ctrlKey || e.metaKey) return;
    if (TAB_KEYS.indexOf(e.key) < 0) return;
    // 认 closest 而不是 e.target 本身:Tab3 的按钮里就嵌着 <span>,今天它们不可聚焦,
    // 但哪天条内多一个可聚焦子元素,认死 e.target 会让 ←/→ **静默**失效。
    const el =
        e.target instanceof Element ? e.target.closest("[data-tab-btn]") : null;
    const cur = el ? TAB_ORDER.indexOf(el.getAttribute("data-tab-btn")) : -1;
    if (cur < 0) return; // 焦点不在四个 tab 上(如条内滑块)—— 不接管
    const n = TAB_ORDER.length;
    let next = cur;
    if (e.key === "ArrowLeft")
        next = (cur - 1 + n) % n; // 循环:最左再按 → 回最右
    else if (e.key === "ArrowRight") next = (cur + 1) % n;
    else if (e.key === "Home") next = 0;
    else next = n - 1; // End
    e.preventDefault(); // Home/End 否则会把内容区滚到头/尾
    activateTab(TAB_ORDER[next]);
    // 焦点跟着走:roving tabindex 已把新激活项改成 tabindex=0、旧的改成 -1,
    // 不显式移焦的话焦点会留在一个 tabindex=-1 的元素上,再按 Tab 会跳回页首。
    const btn = tabbar.querySelector(
        '[data-tab-btn="' + TAB_ORDER[next] + '"]',
    );
    if (btn && typeof btn.focus === "function") btn.focus();
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
    getStore: () => viewStore(),
    getT: () => dictNow,
    onLocalChange: () => requestRender(),
});
tabMaster.mount();

// ------------------------------------------------------------- Tab2(tab-tracks.js)
// T32:列头 + 15 轨行矩阵 + 图例脚注全部由 createTabTracks() 生成(列宽单一真源 =
// tab-tracks.js 的 TRACK_COLS,页面与 app.js 都不写第二份);Wave 2 起 15 行的值
// 全部由事件算出,行内全部上行调用也在该文件(本文件只做订阅转发)。
const tabTracks = createTabTracks({
    root: document,
    bridge,
    getStore: () => viewStore(),
    getT: () => dictNow,
    onLocalChange: () => requestRender(),
});
tabTracks.mount();

// ------------------------------------------------------------- Tab1 曲线编辑器(T34)
// 曲线窗内部(加点/拖拽/Q/shape/删除/side 三段选/叠加线/a11y)全部在 canvas/curve-editor.js;
// 本文件只做一句装配调用(与 Tab2 的 createTabTracks 同款),state/params 经 getStore 读取。
const curveEditor = createCurveEditor({
    canvas: document.querySelector('[data-gb="master-pancurve-canvas"]'),
    root: document,
    bridge,
    getStore: () => viewStore(),
    getT: () => dictNow,
    // T33 起全页统一走 rAF 合帧的 requestRender(),不再逐事件同步整页 render()
    onLocalChange: () => requestRender(),
});
curveEditor.mount();

// ------------------------------------------------------------- Tab3(tab-wave.js)
// T33:工具条/标尺/15 泳道/选区叠加层/段检查器全部由 createTabWave() 生成与投影
// (泳道模板的单一真源 = tab-wave.js 的 waveLaneHtml,本文件不再写第二份);
// 本文件只做装配调用与订阅转发(segments / captureProgress / playhead,见下)。
const tabWave = createTabWave({
    root: document,
    bridge,
    getStore: () => viewStore(),
    getT: () => dictNow,
    onLocalChange: () => requestRender(),
    // 空态 CTA「去 Tab1 打开采集」等页间跳转(05 §2.3 行 318)
    gotoTab: (name) => activateTab(name),
});
tabWave.mount();

// ---------------------------------------------- Tab3 第二视图(tab-suggestions.js)
// T41:建议表 + CSV 导出。它是 **Tab3 内的视图切换**,不是第五个 tab —— 契约 §1.31
// setActiveTab 的四值枚举是冻结面。视图态纯本地(不写 state、不新增 state 字段);
// 打开时泳道主栏与段检查器由 CSS 按 section 的 data-view 整体让位。
const tabSuggest = createTabSuggestions({
    root: document.querySelector('section[data-tab-panel="wave"]'),
    bridge,
    getStore: () => viewStore(),
    getT: () => dictNow,
    // 13 个列头是首次打开时才现建的,那时 applyI18n 早已刷过一轮 —— 补刷这一块
    applyI18n: (node) => node && applyI18n(node, lang),
    onViewChange: () => requestRender(),
});

// ------------------------------------------------------------- Tab4(tab-settings.js)
// T35:缩放 select 与语言胶囊的接线仍在本文件(scale/语言是外壳级),Tab4 的
// 其余全部内容(两设置块 / 说明块 / 存储 / 诊断 / 版本号)在 createTabSettings。
const tabSettings = createTabSettings({
    root: document,
    bridge,
    getStore: () => viewStore(),
    getT: () => dictNow,
    onLocalChange: () => requestRender(),
    onReopenTour: () => tour && tour.start(),
    onViewWorkflow: () => showWorkflowCard(),
    onOpenDocs: () => openDocsInBrowser(),
});
tabSettings.mount();
// 四个 tab 装配完毕,render() 从这里起可以跑(装配前 render 直接早退 ——
// activateTab 在装配之前就跑过一次)。**必须排在最后一个 mount() 之后**:
// 合并 T34/T35 时这一行曾被解冲突弄丢,整页会永不渲染而 smoke 照样全绿
// (node 侧断言面不含 DOM 投影)。
tabsReady = true;

// 布防 badge 三处的点击跳转(05 §2.3 行 300 ①:点击跳 Tab3 定位选区)——
// Tab1 Range 旁与 Tab2 图例行两枚在别的卡里,跳转归外壳;Tab3 自己的 badge
// 由 tab-wave.js 内部接线(同一 locateRecapture 口径)。
for (const gb of [
    "master-range-recapture-badge",
    "tracks-legend-recapture-badge",
]) {
    const el = $(gb);
    if (el) {
        el.addEventListener("click", () => {
            activateTab("wave");
            tabWave.locateRecapture();
        });
    }
}

// ------------------------------------------------------------- tour(T36b)
// 首启交互式引导;蒙版/spotlight/说明框/步骤机/demo badge 在 tour.js。
// 询问步(tour-ask)与「重看引导」入口(settings-reopentour)在上/下方接线。
tour = createTour({
    root: document,
    card,
    bridge,
    getT: () => dictNow,
    activateTab,
    getActiveTab: () => content.getAttribute("data-tab"),
    requestRender: () => requestRender(),
    expandGuide: () => tabSettings.expandNine(),
    zoomLanes: () => tabWave.zoomLanes(),
    showDemoSelection: () => tabWave.showDemoSelection(),
    showDemoSegment: () => tabWave.showDemoSegment(),
    resetWaveView: () => tabWave.resetWaveView(),
    // 结束(完成或 Skip)即刻落「本会话已答」位:setTourSeen 的回执与下一拍 scvb.state
    // 都还没到,syncTourAsk 若只看 state 会在这一帧把询问卡重新弹出来(bug A-2)。
    // 闭包里的 tourAskUi 在本文件下方才 const 声明 —— 本回调只在用户答复时触发(那时模块
    // 早已求值完毕),不会撞 TDZ;不要把它改成 mount 期同步调用。
    onEnd: () => {
        store.session.tourAnswered = true;
        if (tourAskUi.overlay) tourAskUi.overlay.hidden = true;
    },
});
tour.mount();

// 首启语言选择卡(T36b 第四轮):独立 overlay,先于红字九条页;选中语言即关闭并露出红字页。
langStart = createLangStart({
    root: document,
    card,
    onPick: (code) => {
        store.session.langChosen = true;
        if (langStart) langStart.setShown(false);
        setLang(code); // 复用既有语言切换与持久化(契约 §1.30)
    },
});
langStart.mount();

// ------------------------------------------------------------- 查看工作流程大卡(与 tour 步 2 同一张大卡)
// 设置页「查看工作流程」入口:独立 overlay,渲染 workflow.* 五节点 + 优先级;零桥、零 state。
// ---------------------------------------------------- 说明文档外链(SL-214)
/**
 * 文档地址(中文界面给中文手册,其余给英文)。
 *
 * **分支 pin 而不是版本 pin**:docs/RELEASE.md 里发布说明的写法是
 * `blob/v{X.Y.Z}/docs/…` —— 那是对**已发过 tag** 的版本才成立的写法。眼下唯一的
 * tag 是 v0.1.0(空壳骨架),按版本拼出来的链接对当前每一个构建都是 404。
 * 故先 pin 默认分支;**发版时应按 RELEASE.md 的 tag 口径改回来**,改这一处即可。
 */
const DOCS_URL = {
    zh: "https://github.com/synchain-oss/scvb/blob/dev/docs/USER_GUIDE.zh-CN.md",
    en: "https://github.com/synchain-oss/scvb/blob/dev/docs/USER_GUIDE.md",
};

/**
 * 在**系统浏览器**里打开说明文档。
 *
 * 通道 = `window.open` → WebView2 的 NewWindowRequested → JUCE 的
 * `newWindowAttemptingToLoad` → `URL::launchInDefaultBrowser()`(见
 * src/plugin-common/WebViewHost.cpp 的同名重写)。
 * **不新增桥函数**:契约函数表里没有「打开外部 URL」这一项,而这条路本就是 JUCE
 * 给的现成通道,走它就不用动冻结契约。
 * 不用 `<a href>` 直接导航:那会把插件窗**整页替换**成一个网页,而插件窗没有后退键,
 * 用户回不来。`window.open` 走的是新窗口请求,JUCE 已对它 put_Handled(true),
 * WebView2 不会自己弹窗。
 */
function openDocsInBrowser() {
    const url = DOCS_URL[lang] || DOCS_URL.en;
    // noopener:被打开方拿不到 window.opener,标准外链纪律
    window.open(url, "_blank", "noopener");
}

let workflowCard = null;
function ensureWorkflowCard() {
    if (workflowCard) return workflowCard;
    const style = document.createElement("style");
    style.id = "scvb-workflow-card-style";
    style.textContent = [
        ".workflow-card__flow { display: flex; flex-wrap: wrap; align-items: center; gap: var(--sp-6); margin-top: var(--sp-12); }",
        ".workflow-card__item { display: inline-flex; align-items: center; gap: var(--sp-6); white-space: nowrap; }",
        ".workflow-card__node { padding: var(--sp-6) var(--sp-10); border-radius: var(--r-pill); background: rgba(var(--wh), 0.12); border: 1px solid rgba(var(--wh), 0.2); font-size: var(--fs-110); color: var(--txt-dark-2); white-space: nowrap; }",
        ".workflow-card__arrow { color: var(--txt-dark-4); }",
        ".workflow-card__priority { margin-top: var(--sp-10); padding: var(--sp-8) var(--sp-10); border-radius: var(--r-md); background: rgba(var(--wh), 0.08); border: 1px solid rgba(var(--wh), 0.16); font-size: var(--fs-115); color: var(--txt-dark-3); }",
        // [SL-213 用户裁定 2026-08-27] 收口钮由左下的「取消」改成**右下的「确认」**。
        // 右对齐用 `align-self:flex-end`,前提是面板本身是 flex 列 —— .sc-modal 不是,
        // 故一并给面板加 workflow-card__panel。不用 float / margin-left:auto 那类招:
        // 它们会把按钮从正常流里摘出去,基线与上一块的间距都得再补一遍。
        ".workflow-card__panel { display: flex; flex-direction: column; }",
        ".workflow-card__close { margin-top: var(--sp-14); align-self: flex-end; }",
    ].join("\n");
    document.head.appendChild(style);

    const overlay = document.createElement("div");
    overlay.className = "sc-scrim";
    overlay.setAttribute("data-gb", "workflow-card");
    overlay.style.zIndex = "150";
    overlay.hidden = true;

    const panel = document.createElement("div");
    panel.className = "sc-modal workflow-card__panel";
    panel.style.width = "560px";
    panel.style.maxWidth = "calc(100% - 2 * var(--sp-24))";
    panel.setAttribute("role", "dialog");
    panel.setAttribute("aria-modal", "true");
    panel.setAttribute("aria-labelledby", "workflow-card-title");

    const title = document.createElement("p");
    title.className = "sc-modal__title";
    title.id = "workflow-card-title";
    title.setAttribute("data-t", "tour.step2.title");

    const flow = document.createElement("div");
    flow.className = "workflow-card__flow";
    [
        "workflow.capture",
        "workflow.analyze",
        "workflow.tweak",
        "workflow.write",
        "workflow.manual",
    ].forEach((key, i) => {
        const item = document.createElement("span");
        item.className = "workflow-card__item";
        if (i > 0) {
            const arrow = document.createElement("span");
            arrow.className = "workflow-card__arrow";
            arrow.textContent = "→";
            item.appendChild(arrow);
        }
        const node = document.createElement("span");
        node.className = "workflow-card__node";
        node.setAttribute("data-t", key);
        item.appendChild(node);
        flow.appendChild(item);
    });

    const priority = document.createElement("div");
    priority.className = "workflow-card__priority";
    priority.setAttribute("data-t", "workflow.priority");

    const close = document.createElement("button");
    // [SL-213] 走 CTA 主按钮样式:裸 .sc-btn 在深色模态上是「白 .3 底 + --txt-2 深灰字」,
    // 与 .sc-modal 的深底几乎糊成一片(用户实测「按钮配色与背景几乎同色」)。
    // --acc-cta 实心紫 + --acc-ink 深字是 tokens 里既有的主行动配方,对比度足;
    // base.css 那句「一屏最多一枚主行动」在这里成立 —— 本卡就这一个动作。
    close.className = "sc-btn sc-btn--cta workflow-card__close";
    close.type = "button";
    close.setAttribute("data-t", "common.confirm");
    close.addEventListener("click", () => {
        workflowCard.hidden = true;
    });

    panel.append(title, flow, priority, close);
    overlay.appendChild(panel);
    card.appendChild(overlay);

    // 点蒙版(卡外)关闭;Esc 关闭。
    overlay.addEventListener("click", (e) => {
        if (e.target === overlay) overlay.hidden = true;
    });
    overlay.addEventListener("keydown", (e) => {
        if (e.key === "Escape") overlay.hidden = true;
    });

    workflowCard = overlay;
    return workflowCard;
}

function showWorkflowCard() {
    const c = ensureWorkflowCard();
    applyI18n(c, lang); // 用当前语言刷标题/五节点/优先级/关闭按钮
    c.hidden = false;
    const btn = c.querySelector(".workflow-card__close");
    if (btn) btn.focus({ preventScroll: true });
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
// footer「打印结束」提示的 8 秒窗到点补一拍(见 renderFooter)。
let printDoneTimer = 0;
let scalePrev = 1;
let scaleLeft = 0;

function currentScale() {
    const ui = viewStore().state.ui;
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

// [SL-208] **这里故意没有 Ctrl +/-/0 的键盘入口**,别照直觉补。
// 用户报「必须 Ctrl+- 手动档、重开还记不住」:他按的是 WebView2 自带的浏览器缩放,那层
// 缩放插件收不到也存不了。直觉修法是在这里挂 keydown + preventDefault 转调 previewScale,
// **但 Chromium 把 Ctrl 加减/0 划为浏览器保留加速键,页面 preventDefault 压不住它** ——
// 真接上去会变成「插件档位 + 浏览器缩放」两层叠乘,固定设计盒直接溢出,比不接更糟。
// 正确修法是在 WebView2 控制器上关掉 IsZoomControlEnabled / AreBrowserAcceleratorKeysEnabled,
// 而 JUCE 的 WinWebView2 选项(withUserDataFolder / withDLLLocation / withStatusBarDisabled /
// withBuiltInErrorPageDisabled / withBackgroundColour)没有暴露这两个开关 —— 需要动 JUCE 侧
// 或拿原生控制器,已另开卡跟进。在那之前档位入口只有页脚下拉与设置页 select:两者都走
// previewScale → 10 秒防呆 →「保持」落盘,记忆链路本身是通的(host 用例 SL-208 已钉住)。

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
    const arr = viewStore().state.versions || [];
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
    setTimeout(requestRender, REJECT_HINT_MS + 50);
}

async function switchVersion(v) {
    const res = await call("setVersionActive", v);
    if (res && res.rejected === "printing") noteRejectedPrinting();
    requestRender();
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
        requestRender();
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
        requestRender();
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
        requestRender();
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
        requestRender();
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
        // J62:开始使用 → 询问步衔接(ui.tour_seen=false 且全局默认未置位时弹小卡)。
        if (
            tourAskUi.overlay &&
            shouldShowTourAsk(store.state, store.snapshot)
        ) {
            tourAskUi.overlay.hidden = false;
        }
    });
}

// ============================================================================
// tour 询问步(J62:红字页「开始使用」→ 询问步 → tour;契约 §1.33 setTourSeen)
// ============================================================================
const tourAskUi = {
    overlay: $("tour-ask"),
    start: $("tour-ask-start"),
    later: $("tour-ask-later"),
};

if (tourAskUi.start) {
    tourAskUi.start.addEventListener("click", () => {
        store.session.tourAnswered = true;
        if (tourAskUi.overlay) tourAskUi.overlay.hidden = true;
        if (tour) tour.start();
    });
}
if (tourAskUi.later) {
    tourAskUi.later.addEventListener("click", () => {
        // 暂不 = 婉拒 tour,同样置位(§2.6:完成与暂不都 setTourSeen(true, true))。
        store.session.tourAnswered = true;
        call("setTourSeen", true, true);
        if (tourAskUi.overlay) tourAskUi.overlay.hidden = true;
    });
}

// ============================================================================
// 撤销 / 重做(05 §1.3;契约 §1.25/§1.26)—— 键盘 + [D1] header 两钮
// ----------------------------------------------------------------------------
// 两条入口打**同一个** runHistory():键盘那条的行为逐字不变(同样的拦截判据、
// 同样的 preventDefault、同样的桥函数),只是把回执接住,好让两钮的置灰跟着走 ——
// 否则一路 Ctrl+Z 按到栈空,按钮还亮着,而按钮亮不亮本就该是同一个栈的同一件事。
// ============================================================================
const historyUi = { undo: $("header-undo"), redo: $("header-redo") };

/** 调 §1.25/§1.26 并把 `{ok:bool}` 回执喂给可用性 reducer。 */
async function runHistory(kind) {
    // 只读观察态下不发(契约 §1.x 拒绝态列「只读观察态下全 UI 写控件 disabled」)。
    // 判据放在这里而不是各入口:点击那条已被 `data-disabled` 拦住(renderHeader 里
    // 按 `!roNow` 写),但 Ctrl/Cmd+Z 那条根本不看按钮属性 —— 键盘能干成鼠标干不成的
    // 写操作。两个入口共用的这一层是唯一堵得住的地方。
    if (isReadOnly(store)) return;
    const res = await call(kind);
    // call() 在「桥没接上 / 调用抛错」时回 null —— 那是**没有证据**,不是栈空,
    // 保持原样(置灰会把一次通信故障变成一个永久灰掉的按钮)。
    if (!res || typeof res.ok !== "boolean") return;
    store.session.history = historyAfterCall(
        store.session.history,
        kind,
        res.ok,
    );
    requestRender();
}

for (const kind of ["undo", "redo"]) {
    const btn = historyUi[kind];
    if (!btn) continue;
    btn.addEventListener("click", () => {
        // 置灰只写 data-disabled(sc-btn 族的置灰是 opacity + cursor,不吃事件),
        // 拦截照 header 版本 chip / 复制钮的先例做在这里。
        if (btn.getAttribute("data-disabled") === "1") return;
        runHistory(kind);
    });
}

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
        runHistory(e.shiftKey ? "redo" : "undo");
    },
    true,
);

// ============================================================================
// 渲染:外壳(header / 横幅 / footer / 引导页)+ 当前激活 tab
// ----------------------------------------------------------------------------
// T33 性能批(t32/deviations §N「渲染性能批」)。改造前:每一次事件到达 / 每一次
// 本地态改变都**同步**跑一遍整页 —— 30Hz 的 §2.6 playhead、25Hz 的 §2.2 params、
// 拖动期按显示帧率触发的 onLocalChange 全都各跑一遍 header+横幅+footer+缩放+引导页
// +Tab1+Tab2 十五行+Tab3 十五泳道。改造后两条:
//   ① **rAF 合帧**:一律走 requestRender(),同一帧内 N 次请求只在下一帧跑一次
//      render()(事件风暴 → 每帧至多一次整页);
//   ② **按需**:四个面板同在 DOM,只有当前激活的那个逐帧重投影;切 tab 由
//      activateTab() 补一次 render。三个 tab 的 render 都是「store+本地态 → DOM」
//      的幂等纯投影,晚一帧投影与提前投影的结果逐字相同。
// 更细一档(拖动期只更新受影响的那一行,不跑整页)在 tab-tracks.js 的
// requestRowRender() —— 那条路径连整页 render 都不排。
// ============================================================================
/**
 * 重采集布防期的「输出开关被打开过」粘滞位(B-04)。
 *
 * **边沿判定必须逐 §2.1 事件做**,不能寄生在 render() 里:render 本波改成 rAF
 * 合帧,只跑每帧最后一次的 store 快照 —— 同一帧内 `output_enabled` ON→OFF 的成对
 * patch(契约 §0.4「值未变不发」不排除一帧两拍)会让 ON 那一拍不被观测到,footer
 * 琥珀警告就此漏挂(对抗校验 minor)。撤防(armed=false)复位。
 */
function trackRecapOutput() {
    const s = store.state || {};
    if (!(s.recapture && s.recapture.armed)) {
        store.session.recapOutputOpened = false;
    } else if ((s.global || {}).output_enabled) {
        store.session.recapOutputOpened = true;
    }
}

/** 整页重渲染**请求**(rAF 合帧;高频路径一律走它,不要直呼 render())。 */
function requestRender() {
    if (renderQueued) return;
    renderQueued = true;
    const run = () => {
        renderQueued = false;
        render();
    };
    if (typeof requestAnimationFrame === "function") requestAnimationFrame(run);
    else run();
}

function render() {
    if (!tabsReady) return;
    renderHeader();
    renderBanners();
    renderFooter();
    renderScale();
    renderGuide();
    renderLangStart();
    syncTourAsk();
    // 只投影当前激活 tab(T33 渲染性能批):四面板同在 DOM,但每个 tab 的
    // render 都是「store + 本地态 → DOM」的幂等纯投影,切回来时 activateTab
    // 会补一次 requestRender(),晚一帧与提前一帧结果逐字相同。
    // 合并 T34/T35 后:曲线编辑器属 Tab1、设置页属 Tab4,各自归位。
    switch (content.getAttribute("data-tab")) {
        case "master":
            tabMaster.render();
            curveEditor.render();
            return undefined;
        case "tracks":
            return tabTracks.render();
        case "wave":
            // T41:Tab3 有两个视图。建议表开着时泳道整块被 CSS 藏起来了,
            // 再去投影它只是白跑一遍(它自己的脏位也会被清掉)。
            return tabSuggest.isOpen() ? tabSuggest.render() : tabWave.render();
        case "settings":
            return tabSettings.render();
        default:
            return undefined;
    }
}

/**
 * 只读观察态判据(契约 §1.x 拒绝态列「只读观察态下全 UI 写控件 disabled」)。
 * 单独成函数是因为它有**两个**消费点且渲染顺序不同:renderBanners 里算完写
 * `vs.readOnly` 供其后的各 tab 用,而 renderHeader 排在 renderBanners **之前**,
 * 读那个字段会晚一帧 —— header 两钮宁可当帧算一遍。
 */
function isReadOnly(vs) {
    return (
        !!vs.errors.get("secondOutput") ||
        !!(vs.conn && vs.conn.outputReadOnly === true)
    );
}

function renderHeader() {
    const s = viewStore().state;
    const g = s.global || {};
    const phase = outputPhase(s, viewStore().playhead);

    // [D1] 撤销/重做两钮:可用 = 回执驱动的本地位 ∧ 非只读观察态。
    // tour demo 期间 viewStore() 换成 demo 仓(它的 session 里没有 history)—— 回落
    // 起手值即可,demo 是纯展示层,不会有真回执。
    const hist = viewStore().session.history || HISTORY_AVAIL_INIT;
    const roNow = isReadOnly(viewStore());
    for (const kind of ["undo", "redo"]) {
        const btn = historyUi[kind];
        if (!btn) continue;
        const on = hist[kind] && !roNow;
        btn.setAttribute("data-disabled", on ? "0" : "1");
        btn.setAttribute("aria-disabled", String(!on));
        // tooltip 只解释「栈空」这一种灰:只读观察态已有横幅②整屏说明,
        // 每个控件再挂一遍同一句话是噪音。
        setTitle(
            btn,
            !on && !roNow
                ? dictNow[
                      kind === "undo" ? "header.undoEmpty" : "header.redoEmpty"
                  ]
                : "",
        );
    }

    // 连接摘要 pill:N/15 只统计 slotState=2 ∧ heartbeatFresh(契约 §2.3,J01)
    const n = connectedCount(viewStore().conn);
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
    const rejected = Date.now() < viewStore().session.rejectedPrintingUntil;
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
    const vs = viewStore();
    const s = vs.state;
    const err = vs.errors;
    const groupLetter = GROUP_IDS[(s.group_id || 1) - 1];

    // ① 路由失准(数据源 scvb.conn 的 misalignCount,非 error code)
    const mis = misalignedTracks(vs.conn);
    show($("banner-misaligned"), mis > 0);
    fill($("banner-misaligned-text"), "banner.misaligned", { m: mis });

    // ② 组内只读观察 → 全 UI 写控件 disabled
    const second = err.get("secondOutput");
    const readOnly = isReadOnly(vs);
    vs.readOnly = readOnly;
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
    vs.noTimeline = err.has("noTimeline");
    show($("banner-noTimeline"), vs.noTimeline);

    // ⑦ 加载守卫(数据源 scvb.state.print_guard,不是 error code)
    show($("banner-printGuard"), !!(s.print_guard && s.print_guard.pending));

    // ⑧ 上游改动 → 采集数据过期(04 §4.5 fingerprint watchdog;数据源 = §2.8
    //    segments.channels[].stale,不是 error code)。只提示,不 disable 任何控件。
    const staleTracks = staleTrackCount(vs.segments);
    show($("banner-staleCapture"), staleTracks > 0);
    if (staleTracks > 0)
        fill($("banner-staleCapture-text"), "banner.staleCapture", {
            m: staleTracks,
        });

    // ⑨ [SL-239] 采集开着 ⇒ 上面那条提示整条是哑的,而用户完全看不出来。
    //
    // 04 §4.5 的比对**只在采集 OFF 期间发生**(FeatRing::accumulateFp 的 `if (capturing) return;`
    //  —— 这一秒的特征正被写成新基线,拿它跟自己比毫无意义)。这本身不是缺陷;缺陷是
    // 它不可见:v5.6.2 实测里用户按终验清单做「改狠上游 EQ → 应出 ⚠」,采集还开着,
    // 于是既没有 ⚠、也没有任何线索说明为什么(SL-239 定谳)。更糟的是机会**一次性消耗** ——
    // 那一遍带采集的重播已经把基线刷成改后素材,事后再关采集也换不回那条 ⚠
    // (`HOST SL-239:采集 ON 期间提示是哑的,且机会一次性消耗` 把这两件事都钉住了)。
    //
    // 四个判据缺一不可,后三个都是**为了不说反话**(#158 三个 bot 各自独立揪出后两条):
    //
    //   · `capture_enabled` —— 比对被暂停的充要条件;
    //   · **已经有段表** —— 光看 capture_enabled 会在用户**第一遍采集**期间就把横幅摆出来,
    //     而那时他正按流程采集,「提示暂停」对他没有意义;
    //   · **没有任何 stale 轨** —— ⑨ 的职责只是解释「为什么没有 ⚠」。⚠ 已经在了就不需要它,
    //     而且此时两条会当场打架:⑧ 说「上游已不一致,建议重新采集」,⑨ 说「先关掉采集」。
    //     这个组合**真机可达**,不是理论情形:`stale` 是**闩住**的(撤销它的唯一路径是
    //     `FingerprintWatch::resetChannel` —— 拉到新特征 / 打洞 / 换组),`setCaptureEnabled`
    //     只写布尔位、不碰它。于是「看到 ⚠ → 打开采集准备重采 → 还没按播放」这一拍,
    //     stale 仍为真而采集已 ON。初版把这个组合从 mock 里排除掉了,等于把缝糊在了预览世界里;
    //   · **不在重采集布防期** —— `recaptureArm` 会在采集原为 false 时**替用户打开**采集
    //     (SCVB_CONTRACT §1.23 [J87] 裁定①)。此刻他正应当保持采集开着把那段播完,
    //     而 ⑨ 会叫他「先关掉采集」——**指令方向正好相反**;照做还会被记成「用户接管」
    //     (§1.23 裁定③),连撤防恢复原值都一并作废。
    //
    // **只读态(第二个 Output)不特殊处理**,有意的:那边关不了采集,但「为什么没有 ⚠」
    // 这个解释对他同样成立,而且坐在 DAW 前的往往就是他。把一条正确的解释藏起来,
    // 换来的只是「又一次说不清为什么」——这正是本卡要修的那件事。
    //
    // **而 `noTimeline`(⑥)在场时必须让位** —— 与只读态**不同类**,别把两者按同一口径处理
    // (#158 复审 deepseek 的观察,本卡采纳):
    //   · 只读态下「比对暂停是因为采集开着」**仍然为真**,只是他关不了 ⇒ ⑨ 照出无妨;
    //   · `noTimeline` 下比对停摆的真因是**没有时间线**,而 ⑨ 会把它归因到采集开关上。
    //     更糟的是那条动作**做不到**:`tab-master.js` 的写控件闸是 `readOnly || noTimeline`
    //     一并挡,采集开关此时是 disabled —— 用户既关不掉采集,也没有时间线可播,照做 ⚠
    //     也不会回来。⑥ 此刻已经在把真因说清楚了,⑨ 再说一遍反而是**把因果说反**,
    //     与本卡要修的「说不清为什么」是同一种毛病、只是换了一格。
    //   · 这一态可达:工程已有段表 + 换一个不给时间线的宿主打开。
    // `vs.noTimeline` 就在本函数上方(⑥ 那一段)刚赋过值,零额外读取。
    show(
        $("banner-fpPausedByCapture"),
        !!(s.global && s.global.capture_enabled) &&
            !vs.noTimeline &&
            staleTracks === 0 &&
            !(s.recapture && s.recapture.armed) &&
            hasSegmentedMaterial(vs.segments),
    );

    // ⑩ [SL-247 / J92a] 布防还在、采集却已经关了 ⇒ **这次重采集不会记录任何东西**。
    //
    // 判据只用 §2.1 的两个**现成**字段(`recapture.armed` ∧ `!capture_enabled`),
    // 不新增契约字段 —— 这也是 J92a 选择「保留布防位、不自动撤防」的直接理由:
    // 撤了用户就丢了刚拖出来的工作选区且毫无痕迹,留着这个组合本身就是可观测的证据。
    //
    // **两条路都落到这一态,文案因此只说状态与出路、不说是谁关的**:
    //   ① 布防期用户手动开跟随引擎 —— J92a 互斥把采集关了(§1.3 副作用);
    //   ② 布防期用户手动关采集 —— §1.23 裁定③ 的「接管」。**这条早于本卡就存在**,
    //      而此前界面上一个字都没有,用户只会觉得「布防着却什么都没采到」。
    //
    // 醒目但**非阻塞**:不弹确认框、不 disable 任何控件(沿 [J85] 口径)。
    show(
        $("banner-recaptureVoided"),
        !!(s.recapture && s.recapture.armed) &&
            !(s.global && s.global.capture_enabled),
    );

    // toast:一次性提示(§5.1 降级纪律②:可关闭)
    show($("toast-projectCopy"), err.has("projectCopy"));
    show($("toast-sidecarSwitched"), err.has("sidecarSwitched"));

    // 未知 code:原样显示并入 Tab4 诊断区(ADR-002 / ipc §5,UI 不静默)
    const diag = $("settings-diagnostics-list");
    if (diag && vs.unknownCodes.length) {
        diag.replaceChildren(
            ...vs.unknownCodes.map((code) => {
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
    const vs = viewStore();
    const s = vs.state;
    const g = s.global || {};
    const range = g.range || { mode: "follow", start_s: 0, end_s: 0 };
    const footer = $("footer");
    const printing = outputPhase(s, vs.playhead) === "print";

    // print → printDone 的一次性切换(打印结束提示只挂 8 秒,之后回默认提示行)
    if (vs.session.wasPrinting && !printing) {
        vs.session.printDoneUntil = Date.now() + 8000;
    }
    vs.session.wasPrinting = printing;

    let mode = "default";
    if (printing) mode = range.mode === "follow" ? "follow" : "print";
    else if (Date.now() < vs.session.printDoneUntil) mode = "printDone";
    if (footer) footer.setAttribute("data-mode", mode);
    // printDone 是**时间窗**,窗口到点必须补一拍才会回默认行 —— 契约 §0.4 是
    // 「值未变不发」,打印结束后走带停住、事件流随之安静,不能指望「下一帧事件」
    // 把它带下来(与 rejectedPrintingUntil / hostEcho 同款自带定时器)。
    if (mode === "printDone") {
        clearTimeout(printDoneTimer);
        printDoneTimer = setTimeout(
            requestRender,
            store.session.printDoneUntil - Date.now() + 50,
        );
    }

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

    // 重采集布防警告③(05 §2.3 行 300 / B-04 裁定):触发 =「输出开关 ON
    // **或在布防期被打开**」—— 布防期内输出一旦 ON 过就置粘滞位,关回 OFF
    // 警告仍挂(引擎已按全局范围工作过);撤防才复位。
    // 粘滞位的**边沿判定不在这里**(见 trackRecapOutput):render 是 rAF 合帧的,
    // 只看得到每帧最后一份快照;本函数只读位、只投影。
    const armed = !!(s.recapture && s.recapture.armed);
    show(
        $("footer-recapture-warn"),
        armed && (!!g.output_enabled || !!store.session.recapOutputOpened),
    );

    // 版本号(§1.1 快照的 version.plugin;快照没到之前保持 HTML 里的占位)
    const ver = $("footer-version");
    if (ver && vs.snapshot && vs.snapshot.version) {
        ver.textContent = "v" + vs.snapshot.version.plugin;
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
        viewStore().state,
        viewStore().ready ? viewStore().snapshot : null,
        viewStore().session.guideClosed,
    );
}

// 首启语言选择卡(独立 overlay,先于红字九条页;T36b 第四轮)。
// 判据与 shouldShowGuide 同构 + 本会话已选标记;guide 已见(重看引导/兜底)不显示。
function renderLangStart() {
    if (!langStart) return;
    langStart.setShown(
        shouldShowLangStart(
            viewStore().state,
            viewStore().ready ? viewStore().snapshot : null,
            viewStore().session.guideClosed,
            viewStore().session.langChosen,
        ),
    );
}

/**
 * 询问步启动兜底(05 §2.6 首启顺序 + 真实世界边角)。
 * 正常路径 guide 未看 → 红字页 → 「开始使用」→ 询问步;但若某会话已过红字页却
 * 没答询问步(guide_seen=true ∧ tour_seen=false ∧ 全局默认未置位),红字页不再弹、
 * 没有「开始使用」可点,询问步将永远无入口 —— 故每次 render 时直接把它显示出来。
 * 与「开始使用」处理器共用 shouldShowTourAsk;tour 进行中不重弹(问询只在 tour 前一次)。
 */
function syncTourAsk() {
    if (!tourAskUi.overlay) return;
    if (tour && tour.isActive()) return;
    if (
        shouldAutoShowTourAsk(
            store.state,
            store.snapshot,
            store.session.tourAnswered,
        )
    ) {
        tourAskUi.overlay.hidden = false;
    }
}

// ============================================================================
// 事件订阅(契约 §2 九个事件;名字逐字照 BRIDGE_EVENTS.output)
// ============================================================================
if (bridge) {
    bridge.on("scvb.state", (s) => {
        // §2.1 字段纪律:full:true = 全量快照;full:false = 增量(只含变化子树,UI 做深合并)
        store.state =
            s && s.full ? stripFull(s) : deepMerge(store.state, stripFull(s));
        trackRecapOutput(); // B-04 粘滞位:逐事件做边沿判定(render 是合帧的)
        syncUiFromState();
        tabMaster.refreshPreview();
        requestRender();
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
            // [SL-251/J93 裁定③] 调试读数,**不进设置页 UI**:hostEcho:true 两帧之间的间隔
            // 一旦超过闩锁的释放窗口,徽标就会灭一下再亮 —— 也就是用户看得见的那一次眨眼。
            // 释放窗口取 2000ms 是按「宿主自动化按曲线事件写、间隔秒级」估的,**真机的间隔
            // 分布本机测不出来**(mock 只有慢通道)。所以把超窗的那几次原样打到 console:
            // 用户真机若仍见眨眼,拿这几行就能直接把窗口调对,不用再猜。
            const prevAt = store.params.hostEchoAt || 0;
            const gap = prevAt ? Date.now() - prevAt : 0;
            if (prevAt && gap >= HOST_ECHO_RELEASE_MS) {
                console.debug(
                    `[SCVB][SL-251] hostEcho 间隔 ${gap}ms ≥ 释放窗口 ${HOST_ECHO_RELEASE_MS}ms —— 提示徽标会在这一段灭掉再亮(看到眨眼就是这里)`,
                );
            }
            clearTimeout(hostEchoTimer);
            hostEchoTimer = setTimeout(requestRender, HOST_ECHO_FRESH_MS + 50);
        }
        // 本地乐观值让位给引擎回推(必须排在 render 之前;规则见 tab-master.js nextParamEcho):
        // 少了这一步,任一次本地拖动都会把该 ParamID 的显示**永久遮蔽**——
        // 宿主自动化回吐(hostEcho)与切版本的全量重发(full:true)都再进不了 UI。
        tabMaster.onParams(p);
        // Tab2 同款让位 + 冻结位 1→0 的解冻提示判定(05 §2.2 R2)
        tabTracks.onParams(p);
        requestRender();
    });

    // §2.5 30 Hz:**只喂 Tab2 的 rAF 弹道,不触发 render** —— 15 行 × 30 Hz 的
    // 整页重渲染扛不住,而液柱/峰线本来就由 canvas/meter.js 逐帧直写三个属性。
    bridge.on("scvb.meters", (m) => {
        tabTracks.onMeters(m);
    });

    bridge.on("scvb.conn", (c) => {
        store.conn = c;
        requestRender();
    });

    // §2.4:事件缺失时绿点全灭、零报错 —— 故默认值 0 且此处不做任何字段校验
    bridge.on("scvb.groups", (g) => {
        store.groups = (g && g.groups_online) || 0;
        requestRender();
    });

    bridge.on("scvb.playhead", (p) => {
        // 契约 §0.4「值未变不发」的 UI 侧防御(T33 空闲零 rAF):与上一帧**逐字相同**
        // 的 30Hz 事件不排整页重渲染 —— 投影只由载荷字段派生,同样的输入必然渲染出
        // 同样的输出。走带停住而宿主照旧按 30Hz 重发同一位置时,这一条把 30 次/秒的
        // 空转整页渲染削成 0。
        const same = samePlayhead(store.playhead, p);
        store.playhead = p;
        // §2.6:循环区经 loopStartS/loopEndS 出现,缺字段 = 此刻没有可用循环区。
        // 循环区一旦回来,同时解除 setRange 的 noLoop 置灰 —— 否则档位会一直卡在不可选。
        const hasLoop =
            !!p && Number.isFinite(p.loopStartS) && Number.isFinite(p.loopEndS);
        store.loopStale = !hasLoop;
        if (hasLoop) store.loopMissing = false;
        // Tab3:播放头 rAF 插值层直接吃 30Hz 事件(canvas/playhead.js;
        // render() 只管其余投影,不逐帧驱动播放头)
        tabWave.onPlayhead(p);
        // Tab1 轨迹图([J75] T43):同款分工 —— 竖线与跟随模式的滚动都在插值层里,
        // 不经整页 render。视图切走时组件自己不画(isVisible)。
        tabMaster.onPlayhead(p);
        if (!same) requestRender();
    });

    bridge.on("scvb.segments", (seg) => {
        // [D1] 新事务入栈的第二手证据(reason ∈ tab-master.js 的 UNDOABLE_REASONS ——
        // edit/trackManual/copyVersion,[J89] 起加 analyze;那张白名单是唯一真源,别在这里另抄一份)。
        // **必须排在下面的版本闸之前**:撤销栈挂在处理器上、是整个工程一条(03 §5.3),
        // 不分版本;而 `copyVersion` 的段表事件带的正是**目标**版本号,拿去和
        // `version_active` 比必然不等 —— 放在闸后就等于「复制到非激活版本」这一类
        // 入栈操作永远看不见,undo 钮误灰在一次真实可撤销的操作上。
        store.session.history = historyAfterSegments(
            store.session.history,
            seg,
        );
        // 版本闸(§2.8 载荷 `version`):非当前激活版本的段表整帧丢弃 —— 既不进
        // store,也不转发给三个 tab(转发同样有害:tabWave.onSegments 会按它撤
        // 倒计时条、弹 diff、作废检查器乐观值与进行中的边界拖拽)。判据与理由
        // 见 tab-master.js `segmentsEventApplies` 头注(含 native 侧待确认项)。
        if (
            !segmentsEventApplies(
                seg,
                (store.state.global || {}).version_active,
            )
        ) {
            requestRender(); // 上面刚可能翻了 undo 位,得让 header 跟上
            return;
        }
        store.segments = applySegmentsEvent(store.segments, seg);
        tabMaster.onSegments(seg);
        // Tab2:手动常值的乐观显示让位给回推的段表(§1.16 的写入结果经本事件回来)
        tabTracks.onSegments(seg);
        // Tab3:段角标/曲线/边界层标脏(渲染在 store 合并之后的 render() 里)
        tabWave.onSegments(seg);
        // Tab4:全量分析完成 → 清「改后需重分析」标志(03 §6.3 applied.* 同步)
        tabSettings.onSegments(seg);
        // J69 stale:任一轨 stale → tab 导航「波形与分段」挂琥珀点。
        // 读 **store.segments**(合并后的全量视图)而不是本次事件的 channels —— §2.8 的
        // channels 只含受影响轨,一次单轨段编辑会把其余轨的 stale 一起当成「不存在」(04 §4.5)。
        show($("tabnav-wave-stale-dot"), staleTrackCount(store.segments) > 0);
        requestRender();
    });

    bridge.on("scvb.captureProgress", (cp) => {
        // §2.7 是增量事件:`coveragePct` 是该轨在 range 内的当前覆盖率(直接覆盖),
        // `addedRanges` 是本帧新增区间 —— Tab1 只消费前者(泳道底部的 2px 覆盖条归 T33)。
        for (const c of (cp && cp.channels) || []) {
            store.coverage[c.ch] = c.coveragePct;
        }
        // Tab3:该轨波形块缓存失效 + 轨头覆盖率重投影(2px 覆盖条归 T33)
        tabWave.onCaptureProgress(cp);
        requestRender();
    });

    bridge.on("scvb.error", (e) => {
        if (!e || !e.code) return;
        // §2.9:active 缺省视为 true;false = 条件已解除(持续性横幅据此撤下)。
        // 键由 errorStoreKey 统一(lowSample = code+ch 复合键);撤下走
        // errorKeysToDrop —— 契约没保证解除事件必带 ch,不带 ch 的解除必须把该
        // code 的全部轨级条目一并撤掉,否则黄标永不熄灭。
        if (e.active === false) {
            for (const k of errorKeysToDrop(store.errors, e)) {
                store.errors.delete(k);
            }
        } else store.errors.set(errorStoreKey(e), e);
        if (!KNOWN_CODES.has(e.code) && !store.unknownCodes.includes(e.code)) {
            store.unknownCodes.push(e.code);
        }
        requestRender();
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

/**
 * §2.6 载荷是**扁平的标量集**(`timeS` / `isPlaying` / `loopStartS?` / `loopEndS?` /
 * `inRange`)—— 逐键严格相等即可断定「由它派生的一切投影都相同」。
 * 键集不同、出现非标量(引用不等)时一律判不同,宁可多渲染一帧也不漏。
 */
function samePlayhead(a, b) {
    // 两侧都缺(null/undefined)= 同样的「没有播放头」,派生投影一模一样;
    // 原来一律判不同,连发 null 会每次都排一次整页渲染(pr-agent)。
    if (!a && !b) return true;
    if (!a || !b) return false;
    const ka = Object.keys(a);
    if (ka.length !== Object.keys(b).length) return false;
    for (const k of ka) {
        if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
        if (a[k] !== b[k]) return false;
    }
    return true;
}

/** `full` 是事件专用标志,不属 state 树,合并前摘掉免得污染快照字段集(§1.1 语义行)。 */
function stripFull(s) {
    if (!s) return {};
    const out = { ...s };
    delete out.full;
    return out;
}

/** state 里的 UI 面(语言 / tab)回推到页面本地态 —— 回推不再往上行,避免自激。 */
function syncUiFromState() {
    const ui = store.state.ui || {};
    if (ui.language && ui.language !== lang && LANGS.includes(ui.language)) {
        setLang(ui.language, { push: false });
    }
    // tour 期间 tab 为 UI 本地态,真实 state 的 active_tab 不回推(§2.6)。
    if (tour && tour.isActive()) return;
    if (ui.active_tab && content.getAttribute("data-tab") !== ui.active_tab) {
        activateTab(ui.active_tab, { push: false });
    }
}

// ============================================================================
// 首帧:§0.6 mBridgeReady 门控由页面掌握 —— 装载时调一次 requestInitialState()。
// ============================================================================
refreshI18n();

// [SL-207] 原生右键菜单抑制**挂在 try 之外**:首帧链路炸掉时露出来的是兜底面板,
// 那上面右键冒出「查看网页源代码」比正常界面更穿帮。它不依赖桥、不会抛,先挂上。
disableNativeContextMenu(document);
// [SL-227] 裸 Alt 会激活 Windows 的菜单模式、把焦点从 WebView 内容上挪走 ——
// 而 Alt+滚轮的手势天然以裸 Alt 的 keyup 收尾,于是「Alt 用过之后 Ctrl 横缩放
// 概率失效」。同样挂在 try 之外:它不依赖桥、不会抛。
suppressBareAltMenu(document);

(async function boot() {
    try {
        await bootInner();
    } catch (e) {
        // 首帧链路炸掉 = 界面起不来。显式上报,免得 native 侧只能等看门狗超时,
        // 且兜底面板还写着「加载太慢」这种误导文案(守卫装在 index.html 的 <head>)。
        if (typeof window.__scvbReportBootError === "function") {
            window.__scvbReportBootError("output-boot", (e && e.stack) || e);
        }
        throw e;
    }
})();

async function bootInner() {
    if (!bridge) {
        requestRender();
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
        trackRecapOutput(); // 快照落地也算一拍(布防中打开着输出重开面板)
        store.conn = snapConn || store.conn;
        store.ready = true;
        syncUiFromState();
        tabMaster.refreshPreview();
    }
    render();
}

// 测试面:web-preview 的页面级冒烟要能在页内读到分布图补间的诊断值(不经私有闭包)。
// **只读快照,不暴露任何写入口** —— 与 Monitor 侧 `__SCVB_MONITOR__` 同一口径。
// 为什么需要它:补间是不是 rAF 驱动的,只能从「循环跑了几帧」看出来 ——
// 事件驱动的实现里那个数恒为 0。靠采样撞动画中段的覆盖等于没有覆盖(SL-192 教训)。
window.__SCVB_OUTPUT__ = {
    distMotion: () => tabMaster.distDiag(),
};
