// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output —— 页面接线。
// -----------------------------------------------------------------------------
// 当前阶段(T31 Wave 1「视觉复现」):
//   • 外壳(header / 横幅区 / tab 石板 / footer)与 Tab1 已按设计稿正式实现,
//     一切组件状态由 index.html 上的 data-* 属性 + CSS 选择器承载(05 §1.3);
//     本文件只做**结构性模板生成**(重复元素批量出 DOM)与 i18n / tab 路由,
//     不写状态机、不消费桥事件 —— 那是 Wave 2(bridge 事件消费 + 控件上行)。
//   • Tab2 / Tab3 / Tab4 仍是 T27b 灰模骨架,行/泳道模板保持原样待 T32-T35 替换。
//   • 每个交互组件的桥调用点一律以注释桩落位(函数名逐字照 web/shared/bridge.js
//     BRIDGE_FUNCTIONS.output,即冻结契约 docs/SCVB_CONTRACT.md §1 的名字)。
//   • 浏览器直开(无 __JUCE__、无注入的 window.__SCVB_MOCK__)必须零 console.error——
//     createBridge 的失败走 try/catch,只 console.warn 一句并在 footer 落一行提示。
// =============================================================================

import { createBridge } from "../shared/bridge.js";
import { applyI18n, LANGS } from "../shared/i18n.js";
import { DESIGN } from "../shared/design-box.js";
import { FIFTEEN_TRACKS } from "../shared/mock-data.js";

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
    console.warn(
        "SCVB Output greybox: createBridge 未接上后端 —— " + e.message,
    );
    const hint = document.getElementById("footer-hint");
    if (hint) {
        hint.textContent = "未接后端——请经 web-preview 入口预览(T28)";
    }
}

// [Wave 2] bridge.on("scvb.state", (s) => { ... }) —— 契约 §2.1(全量/增量 state 快照)
// [Wave 2] bridge.on("scvb.params", (p) => { ... }) —— 契约 §2.2(25Hz 参数批量快照)
// [Wave 2] bridge.on("scvb.conn", (c) => { ... }) —— 契约 §2.3(每轨连接态,~4Hz)
// [Wave 2] bridge.on("scvb.groups", (g) => { ... }) —— 契约 §2.4(J70,在线组位图,1Hz)
// [Wave 2] bridge.on("scvb.meters", (m) => { ... }) —— 契约 §2.5(电平,30Hz)
// [Wave 2] bridge.on("scvb.playhead", (p) => { ... }) —— 契约 §2.6(播放头,30Hz)
// [Wave 2] bridge.on("scvb.captureProgress", (c) => { ... }) —— 契约 §2.7(采集覆盖增量)
// [Wave 2] bridge.on("scvb.segments", (s) => { ... }) —— 契约 §2.8(分段结果)
// [Wave 2] bridge.on("scvb.error", (e) => { ... }) —— 契约 §2.9(九错误码,驱动 §2.0 警告面)
// [Wave 2] bridge.requestInitialState() —— 契约 §1.1(mBridgeReady 门控由页面掌握,首次装载调用一次)

// ------------------------------------------------------------- 小工具
function tt(ch) {
    return String(ch).padStart(2, "0");
}

function clamp(lo, hi, v) {
    return v < lo ? lo : v > hi ? hi : v;
}

// ------------------------------------------------------------- i18n(applyI18n 首刷 + 语言胶囊)
let lang = "zh";

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

// 「Wave 1 静态数」钩子:词条里的 {n}/{m}/{k}/{p}/{t} 占位符在 Wave 1 也必须落成数字。
// 理由:Wave 1 交的是**可与设计稿并排比对的静态屏**(用户反馈④),设计稿对应位置是具体数
// (稿 L323「将影响 86 区段 / 12 轨;4 处手动编辑将保留」、L330「范围内 78% 已覆盖」、
// L395「共 6 段 · 合计 2:48」);留着裸大括号既对不上稿,截图也像坏页。
// 取值口径与分布图柱体同源 —— web/shared/mock-data.js 的 FIFTEEN_TRACKS(零自造数字)。
// [Wave 2] 三处的数据源换成 scvb.segments(n/m/k)、scvb.captureProgress(p)、
//          scvb.state.range + scvb.segments(follow 档的 n/t),format() 本身照用。
function format(text, vals) {
    return String(text).replace(/\{(\w+)\}/g, (m, k) =>
        Object.prototype.hasOwnProperty.call(vals, k) ? String(vals[k]) : m,
    );
}

function mmss(totalS) {
    const s = Math.round(totalS);
    return Math.floor(s / 60) + ":" + tt(s % 60);
}

// 区间并集(升序合并),用于把 15 轨各自的采集覆盖并成「已分析区域共 N 段 · 合计 T」。
function mergeRanges(ranges) {
    const sorted = ranges.slice().sort((a, b) => a.startS - b.startS);
    const out = [];
    for (const r of sorted) {
        const last = out[out.length - 1];
        if (last && r.startS <= last.endS) {
            last.endS = Math.max(last.endS, r.endS);
        } else {
            out.push({ startS: r.startS, endS: r.endS });
        }
    }
    return out;
}

const WAVE1_NUMBERS = (() => {
    const chans = FIFTEEN_TRACKS.segments.channels;
    const cov = FIFTEEN_TRACKS.captureProgress.channels;
    const regions = mergeRanges(cov.flatMap((c) => c.addedRanges));
    return {
        // 02 分析 · 影响预览行(master.step2.desc)
        n: chans.reduce((sum, c) => sum + c.segments.length, 0),
        m: chans.filter((c) => c.segments.length > 0).length,
        k: FIFTEEN_TRACKS.segments.diff.kept,
        // 02 分析 · 覆盖率行(master.step2.coverage)
        p: Math.round(
            cov.reduce((sum, c) => sum + c.coveragePct, 0) / cov.length,
        ),
        // Range 全曲跟随档提示(master.rangeFollowHint):{n} 复用区段数不对——
        // 这里的 n 是「已分析区域」段数,与上面的区段数是两个量,故单独一份。
        followN: regions.length,
        followT: mmss(regions.reduce((s, r) => s + (r.endS - r.startS), 0)),
    };
})();

// applyI18n 写的是 textContent(整串覆盖),所以填充必须排在它之后、且每次切语言都重跑。
function fillWave1Numbers(t) {
    const put = (gb, key, vals) => {
        const el = document.querySelector(`[data-gb="${gb}"]`);
        if (el && Object.prototype.hasOwnProperty.call(t, key)) {
            el.textContent = format(t[key], vals);
        }
    };
    put("master-analyze-preview", "master.step2.desc", WAVE1_NUMBERS);
    put("master-analyze-coverage", "master.step2.coverage", WAVE1_NUMBERS);
    put("master-range-follow-hint", "master.rangeFollowHint", {
        n: WAVE1_NUMBERS.followN,
        t: WAVE1_NUMBERS.followT,
    });
}

function refreshI18n() {
    const t = applyI18n(document, lang);
    renderSplitAxes(t);
    fillWave1Numbers(t);
}

refreshI18n();

function setLang(next) {
    if (!LANGS.includes(next)) return;
    lang = next;
    refreshI18n();
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
    // [Wave 2] bridge.setLang(code) —— 契约 §1.30(纯 UI 本地切换 + 落盘调用点)
}

document.querySelectorAll("[data-lang]").forEach((btn) => {
    btn.addEventListener("click", () => setLang(btn.getAttribute("data-lang")));
});

// ------------------------------------------------------------- tab 路由 + 石板滑块(05 §2.0)
// 滑块几何逐字照设计稿 1874-1881:left = 3px(条壳 padding)+ (tab-1) × 136px,宽恒 136px,
// 位移 transition:left .42s cubic-bezier(.2,.7,.2,1)(= base.css 的 --dur-slate/--ease)。
// 两个几何常量的真源是 index.html 的 --sp-3(条壳 padding)与 --tab-w(tab 项宽,同一变量
// 也定 .sc-tabs__item 的宽度),这里读回来算 left,写成**行内 px**:
//   • 不读 offsetLeft/offsetWidth —— 首帧布局未定时它们为 0,正是灰模「滑块只盖住最左一点」
//     的成因(用户反馈①);
//   • 行内 left 也是设计稿本身的写法(slateStyle 是一个行内 style 对象),照移植不另发挥。
//   #tabs-slate 的 CSS 规则保留同一算式(读 --tab-i,缺省 0)作首帧/无 JS 默认值,
//   行内值优先级更高,两者始终同值,不会打架。
const content = document.getElementById("content");
const tabbar = document.getElementById("tabbar");
const slate = document.getElementById("tabs-slate");
const TAB_ORDER = ["master", "tracks", "wave", "settings"];

function slateLeftPx(idx) {
    // 兜底值 = 设计稿 L287 的 padding:3px 与 L1877/L1681 的 136px:样式表尚未就绪
    // (或在 linkedom 一类无排版引擎的冒烟环境里)getComputedStyle 取不到值,
    // 兜底保证首帧位置正确 —— 兜底与 CSS 里的 --sp-3/--tab-w 必须同值。
    const cs =
        typeof window !== "undefined" && window.getComputedStyle
            ? window.getComputedStyle(tabbar)
            : null;
    const pad = (cs && parseFloat(cs.getPropertyValue("--sp-3"))) || 3;
    const w = (cs && parseFloat(cs.getPropertyValue("--tab-w"))) || 136;
    return pad + idx * w;
}

function activateTab(name) {
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
    // [Wave 2] bridge.setActiveTab(tab) —— 契约 §1.31(tour.js 期间不经此口,§2.6)
}

tabbar.querySelectorAll("[data-tab-btn]").forEach((btn) => {
    btn.addEventListener("click", () =>
        activateTab(btn.getAttribute("data-tab-btn")),
    );
});

activateTab("master");

// --------------------------------------- Tab1 ① 三件套开关的 data-on / aria 派生(单一状态源)
// 状态真源 = .master-flow 上的 data-cap / data-out(05 §1.3);开关的**视觉**真源 =
// base.css 的 `.sc-toggle[data-on="1"]`。两者之间只允许一条派生边,写在这里:
//     data-cap ∈ {armed, capturing} ⇒ .cap-toggle[data-on="1"][aria-checked="true"]
//     data-out === "1"              ⇒ .out-toggle[data-on="1"][aria-checked="true"]
// 页面 CSS 里**不得**再写第二份 `[data-cap=…] .cap-toggle{绿}` —— 那会让轨道变绿而
// data-on/aria-checked 停在 0/false,视觉「开」而屏幕阅读器读「关」。
// MutationObserver 让「手改 data-cap 静态验证四态」(Wave 1 的验收方式)也走同一条派生,
// 不会漏掉 aria。Wave 2 只需把 data-cap/data-out 从 scvb.state 写进来,本函数照旧生效。
// 冒烟不变式:任何 data-cap ∈ {armed, capturing} ⇒ cap-toggle 的 aria-checked === "true"。
const masterFlow = document.querySelector('[data-gb="master-flow"]');

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
// document 级**单**监听(15 轨密排下不能每行挂监听,base.css L984 头注),逐字照设计稿
// L1474-1489:坐标写在**被悬停的元素自己**身上(--mx/--my 是 [data-glow]::after 的
// radial-gradient 圆心,必须是元素内局部坐标;写到 :root 会让每个元素都拿到视口坐标而错位),
// 离开时把上一个元素的 --glow-o 归零。base.css 的 `[data-glow]::after` 只读这三个变量。
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

// ------------------------------------------------------------- Tab1 ⓪ 组胶囊 A–H(设计稿 L364-373)
// 八枚胶囊结构一致,批量出 DOM 而不是在 HTML 里复制八遍。
// 状态位:aria-pressed(当前组,白底)/ data-online(在线绿点,scvb.groups 位图)/
// data-pending(改组确认条弹出时的琥珀预选态)。Wave 1 的初值取 mock 位图,Wave 2 换成事件。
const GROUP_IDS = ["A", "B", "C", "D", "E", "F", "G", "H"]; // 契约 §0.2:g = 1..8
const groupPills = document.querySelector('[data-gb="master-group-seg"]');
if (groupPills) {
    const onlineBits = FIFTEEN_TRACKS.groups.groups_online;
    groupPills.innerHTML = GROUP_IDS.map((id, i) => {
        const online = (onlineBits >> i) & 1;
        return `
      <button class="group-pill" data-glow="1" data-group="${i + 1}"
              aria-pressed="${i === 0}" data-online="${online}" data-pending="0"
              data-gb="master-group-${id}">
        ${id}<span class="group-pill__dot" aria-hidden="true"></span>
      </button>`;
    }).join("");
}

// ------------------------------------------------------------- Tab1 ④ Lead Select 选项(05 §2.1④)
const leadSelect = document.querySelector(
    '[data-gb="master-leadselect-select"]',
);
if (leadSelect) {
    leadSelect.insertAdjacentHTML(
        "beforeend",
        FIFTEEN_TRACKS.labels
            .map(
                (label, i) =>
                    `<option value="${i + 1}">${tt(i + 1)} · ${label}</option>`,
            )
            .join(""),
    );
}

// ------------------------------------------------------------- Tab1 ④ 声像/音量分布图柱体(设计稿 L546-551)
// 设计稿是「纯 DOM + CSS 绝对定位」,不是 canvas:先铺一层立体声张开横线,再铺柱体层。
// 几何(设计稿 L2037-2056):横位 x =(pan+100)/200;柱高 ∝ 音量行程;
// 张开半宽 halfRaw =(轨 width%/100)× 16,并被 x 与 100−x 夹住不出框。
// Wave 1 用 mock 的 params 快照撑出真实密度(与 Tab2 十五行同源);
// Wave 2 换成 scvb.params + scvb.state,并把 Width/MS 的实时联动接上。
const distBars = document.querySelector('[data-gb="master-distchart-bars"]');
if (distBars) {
    const vals = FIFTEEN_TRACKS.params.values;
    const chans = FIFTEEN_TRACKS.snapshot.channels;
    const stereo = new Set(FIFTEEN_TRACKS.stereoChannels);
    const html = [];
    const bars = [];
    for (let ch = 1; ch <= FIFTEEN_TRACKS.labels.length; ch++) {
        const p = "v1_t" + tt(ch) + "_";
        const pan = vals[p + "pan"];
        const volDb = vals[p + "vol"];
        const width = vals[p + "width"];
        const x = ((pan + 100) / 200) * 100;
        // volDb 的 −24..+12 行程归一后除以 0.70 拉满卡片高度(设计稿 L2043 的 plotH/0.62 同法)
        const h = clamp(8, 88, ((volDb + 24) / 36 / 0.7) * 100);
        const lead = chans[ch - 1] && chans[ch - 1].lead_lock ? 1 : 0;
        if (stereo.has(ch)) {
            const half = Math.min((width / 100) * 16, x, 100 - x);
            html.push(
                `<div class="dist-span" style="--x0:${(x - half).toFixed(2)}%;--w:${(half * 2).toFixed(2)}%;--y:calc(18px + ${h.toFixed(2)}%)"></div>`,
            );
        }
        bars.push(
            `<div class="dist-bar" data-lead="${lead}" data-ch="${ch}" style="--x:${x.toFixed(2)}%;--h:${h.toFixed(2)}%"></div>`,
        );
    }
    distBars.innerHTML = html.concat(bars).join("");
}

// ------------------------------------------------------------- Tab2:15 轨行模板生成(05 §2.2)
// T27b 灰模骨架,原样保留待 T32 替换:仅用 FIFTEEN_TRACKS 撑开列宽,零状态渲染逻辑。
function trackRowHtml(ch, label) {
    // data-tour="freeze":05 §2.6 步骤5「一条轨道行的冻结开关组(pad 14)」,取首行为代表。
    // 锚点必须挂在**有 principal box** 的元素上 —— T36b 的 spotlight 用 getBoundingClientRect()
    // 算亮区,display:contents 的元素返回 0×0@(0,0)(见下方冻结组注释)。
    const isFreezeAnchor = ch === 1;
    return `
    <div class="tracks-row" role="row" data-gb="tracks-row-${ch}" data-ch="${ch}">
      <span class="sc-dot" data-tone="gray" role="cell" style="width:8px" data-gb="tracks-row-${ch}-statuslight" title=""></span>
      <!-- 轨状态增补(05 §2.2 line 282,R1):采集后有效唱段 <1.5s 的轨在状态灯旁显示黄标
           「样本不足,结果可能不稳」(04 §7 步7;Tab3 轨头同款 = wave-lane-{ch}-lowsample)。
           词条 lowSample 字典已备,不需 todo;显隐数据源 scvb.captureProgress/scvb.segments,留 T32。 -->
      <span class="sc-badge--amber" data-t="lowSample" data-gb="tracks-row-${ch}-lowsample" hidden></span>
      <!-- 重采集布防 badge②(05 §2.3「重采集选区」行 ②:Tab2 目标轨行首同款小 badge) -->
      <span class="sc-badge--amber" data-t="wave.recaptureArmed" data-gb="tracks-row-${ch}-recapture-badge" hidden></span>
      <span class="tracks-row__ch sc-mono" role="cell" style="width:44px" data-gb="tracks-row-${ch}-ch">
        ${tt(ch)}
        <span class="sc-badge" data-t="stereoBadge" data-gb="tracks-row-${ch}-stereo" hidden>ST</span>
      </span>
      <span class="tracks-row__label" role="cell" style="width:150px" data-gb="tracks-row-${ch}-label">${label}</span>
      <span class="sc-knob" data-live="1" role="cell" style="width:30px;--ang:0deg" data-gb="tracks-row-${ch}-pan"><span class="sc-knob__needle"></span></span>
      <span class="sc-knob" data-live="1" role="cell" style="width:30px;--ang:0deg" data-gb="tracks-row-${ch}-width"><span class="sc-knob__needle"></span></span>
      <span class="tracks-row__voltube" role="cell" style="width:268px" data-gb="tracks-row-${ch}-voltube">
        <span class="sc-tube">
          <span class="sc-tube__slot">
            <span class="sc-tube__liquid" style="--lv:0%"></span>
            <span class="sc-tube__peak" style="--pk:0%"></span>
          </span>
          <span class="sc-tube__gloss"></span>
          <span class="sc-tube__collar" style="--vol:50%" data-frozen="0" data-gb="tracks-row-${ch}-vol-collar"></span>
        </span>
        <span class="sc-badge--amber" data-t="leadVolExempt" data-gb="tracks-row-${ch}-volexempt-badge" hidden></span>
        <!-- 词条待立:冻结后行尾生效版本小字(A8,05 §2.2 回流⑦,字典缺) -->
        <span class="sc-mono" data-gb="tracks-row-${ch}-freeze-version" data-gb-todo="词条待立" hidden>V1</span>
      </span>
      <span class="sc-stepper" role="cell" style="width:66px" data-gb="tracks-row-${ch}-priority">
        <button data-gb="tracks-row-${ch}-priority-dec" aria-label="dec">−</button>
        <span class="sc-stepper__val sc-mono">5</span>
        <button data-gb="tracks-row-${ch}-priority-inc" aria-label="inc">+</button>
      </span>
      <span class="sc-toggle" role="cell" style="width:26px" data-on="0" data-gb="tracks-row-${ch}-leadlock" title="lead lock"></span>
      <span style="width:52px;display:inline-flex;align-items:center;gap:2px" role="cell">
        <!-- aria-label 是首帧兜底:本模板在 applyI18n(document) 之后才注入 DOM,
             data-t-aria 要等下一次 setLang 才生效(a11y 占位名,T32 换词条)。 -->
        <select class="sc-select" style="width:100%" aria-label="配对" data-t-aria="pair" data-gb="tracks-row-${ch}-pair">
          <!-- 词条待立:下拉「无」选项(05 §2.2 配对列,字典缺 none 词) -->
          <option value="0" data-gb-todo="词条待立">–</option>
          <option value="1">A</option><option value="2">B</option><option value="3">C</option>
          <option value="4">D</option><option value="5">E</option><option value="6">F</option>
          <option value="7">G</option>
        </select>
        <!-- 词条待立:「配对超员」标(A8,05 §2.2 配对行,字典缺) -->
        <span class="sc-badge--amber" data-gb="tracks-row-${ch}-pair-overflow" hidden data-gb-todo="词条待立">满</span>
      </span>
      <span class="sc-divider" aria-hidden="true"></span>
      <span class="sc-toggle" role="cell" style="width:26px" data-on="0" data-gb="tracks-row-${ch}-volexempt"></span>
      <span class="sc-toggle" role="cell" style="width:26px" data-on="0" data-gb="tracks-row-${ch}-autopan"></span>
      <span class="sc-divider" aria-hidden="true"></span>
      <!-- 冻结开关组:必须是**真实 inline-flex 盒**(不用 display:contents)——
           display:contents 的元素不生成 principal box,getBoundingClientRect() 恒为 0×0@(0,0),
           T36b 的 spotlight 会退化成左上角零尺寸洞;role=cell 在 display:contents 下也不进 a11y 树。
           宽度 26+gap(--sp-6 = 6px)+26 = 58px,与 §2.2 表头「冻结P」「冻结V」两列(各 26px,
           列间同为 --sp-6)逐像素对齐;内部 gap 与行 gap 同值,故换成一个盒不改变列位。 -->
      <span class="tracks-row__freezegroup" role="cell" style="width:58px"${isFreezeAnchor ? ' data-tour="freeze"' : ""}>
        <span class="sc-toggle" style="width:26px" data-on="0" data-gb="tracks-row-${ch}-freezepan"></span>
        <span class="sc-toggle" style="width:26px" data-on="0" data-gb="tracks-row-${ch}-freezevol"></span>
      </span>
      <span class="sc-divider" aria-hidden="true"></span>
      <span class="sc-toggle" role="cell" style="width:26px" data-on="1" data-gb="tracks-row-${ch}-enable"></span>
      <!-- R3 防误伤(05 §2.2 pan 旋钮行 / §1.4 setTrackManual;验收出处 05 §2.5 倒数第 5 条):
           每轨**首次**手动拖 pan 旋钮 / vol 推子前就地展开一次性行内确认(每轨每会话一次,无条件);
           存在 locked 段时正文追加 tracks.manualOverwriteConfirm.locked「(含 {l} 个锁定段)」。
           灰模只落 DOM 与词条,一次性判定/展开/撤销栈全留 T32。 -->
      <div class="sc-confirm tracks-row__confirm" data-gb="tracks-row-${ch}-manual-overwrite-confirm" hidden>
        <span data-t="tracks.manualOverwriteConfirm"></span>
        <span data-t="tracks.manualOverwriteConfirm.locked" data-gb="tracks-row-${ch}-manual-overwrite-confirm-locked" hidden></span>
        <button class="sc-btn" data-gb="tracks-row-${ch}-manual-overwrite-cancel" data-t="common.cancel"></button>
        <!-- 词条待立:R3 防误伤确认条主按钮「继续」(05 §2.2,字典缺) -->
        <button class="sc-btn sc-btn--cta" data-gb="tracks-row-${ch}-manual-overwrite-ok" data-gb-todo="词条待立">继续</button>
        <!-- [T3x] bridge.setTrackManual(ch, "pan"|"vol", value) —— 契约 §1.16(确认后才落) -->
      </div>
      <!-- R2 语义保留(05 §2.2 冻结行):解冻(该位 1→0)且该轨当前版本曲线为「单段全时限
           user_edited 常值」(setTrackManual 产物)时,行内提示 + 单轨重新识别入口。 -->
      <div class="sc-confirm tracks-row__confirm" data-gb="tracks-row-${ch}-manualdriven-hint" hidden>
        <span data-t="tracks.manualDrivenHint"></span>
        <button class="sc-btn" data-gb="tracks-row-${ch}-manualdriven-reidentify" data-t="reidentify"></button>
        <!-- [T3x] bridge.analyze({tracksMask:1<<(${ch}-1)}, {clearManual:true}) —— 契约 §1.6(单轨重新识别;二次确认同 §2.3;locked 段免疫) -->
      </div>
    </div>`;
}

const tracksBody = document.getElementById("tracks-body");
if (tracksBody) {
    tracksBody.innerHTML = FIFTEEN_TRACKS.labels
        .map((label, i) => trackRowHtml(i + 1, label))
        .join("");
}

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
// footer 走设计稿 L1026-1033 的深色浮层 listbox,设置页仍是原生 <select>(Tab4 留 T35);
// 两处的档位都在此生成,避免把同一份档位表复制成第二/第三真源。
// 「超出当前屏幕」判据(05 §1.2):W*F > screen.availWidth ∨ H*F > screen.availHeight
// → 档位灰掉并提示,绝不隐藏。tooltip / 10 秒防呆 / 回退计时全在 Wave 2。
function scaleOverflows(f) {
    const s = typeof window !== "undefined" ? window.screen : null;
    if (!s || !s.availWidth || !s.availHeight) return false;
    return (
        DESIGN.output.w * f > s.availWidth ||
        DESIGN.output.h * f > s.availHeight
    );
}

const scalePanel = document.querySelector('[data-gb="footer-scale-panel"]');
if (scalePanel) {
    scalePanel.innerHTML = DESIGN.output.presets
        .map((f) => {
            const over = scaleOverflows(f) ? 1 : 0;
            const current = f === 1 ? 1 : 0;
            const noteKey = over
                ? "scale.overflow"
                : current
                  ? "scale.current"
                  : "";
            return `
      <button class="footer-scale__opt" role="option" aria-selected="${current === 1}"
              data-scale="${f}" data-current="${current}" data-over="${over}"
              data-gb="footer-scale-opt-${Math.round(f * 100)}">
        <span>${Math.round(f * 100)}%</span>
        <span class="footer-scale__note"${noteKey ? ` data-t="${noteKey}"` : ""}></span>
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

// 模板注入完成后重刷一次字典:首刷发生在组胶囊/轨行/泳道/档位项注入之前,
// 注入子树内的 data-t/data-t-aria 否则要等下一次切语言才生效(PR #37 评审建议 2)。
refreshI18n();

// ------------------------------------------------------------- 键盘(05 §1.3 撤销/重做)
// 05 §1.3「键盘与撤销(R1 增补)」:WebView 内捕获 Ctrl+Z / Ctrl+Shift+Z 映射插件自有
// UndoManager 并 preventDefault 防止冒泡到宿主撤销;焦点在文本输入框时不拦截。
// Wave 1 不装监听(零业务逻辑),只把两个冻结契约函数名落成注释桩供 Wave 2 接线与未来 grep:
// [Wave 2] bridge.undo() —— 契约 §1.25(Ctrl+Z 捕获 + preventDefault)
// [Wave 2] bridge.redo() —— 契约 §1.26(Ctrl+Shift+Z 捕获 + preventDefault)
