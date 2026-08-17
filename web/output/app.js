// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output —— T27b 结构化灰模最小接线。
// -----------------------------------------------------------------------------
// 纪律(T27b 卡,与 index.html 头注同源):
//   • 只做 createBridge 接线 + applyI18n 刷字典 + tab 路由(纯 CSS 显隐,不写 state,
//     不调 setActiveTab)+ 15 轨行/15 泳道的模板生成(仅撑开骨架,零状态渲染逻辑)。
//   • 每个交互组件的桥调用点一律以注释桩落位(函数名逐字照 web/shared/bridge.js
//     BRIDGE_FUNCTIONS.output,即冻结契约 docs/SCVB_CONTRACT.md §1 的名字),
//     不在本文件里真正调用 —— T31-T36 替换灰模时按锚点接上真实调用与状态机。
//   • 除 createBridge / applyI18n 外零业务逻辑:无状态机、无渲染循环、无事件处理实现。
//   • 浏览器直开(无 __JUCE__、无注入的 window.__SCVB_MOCK__)必须零 console.error——
//     createBridge 的失败走 try/catch,只 console.warn 一句并在 footer 落一行提示。
// =============================================================================

import { createBridge } from "../shared/bridge.js";
import { applyI18n, LANGS } from "../shared/i18n.js";
import { DESIGN } from "../shared/design-box.js";
import { FIFTEEN_TRACKS } from "../shared/mock-data.js";

// ------------------------------------------------------------- 设计盒尺寸(05 §1.2)
// 真源 = web/shared/design-box.js DESIGN.output;index.html 的 --box-w/--box-h
// 默认值只是同值锚定,这里用 JS 写一次,防止两处漂移。
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
        hint.removeAttribute("data-t");
        hint.textContent = "未接后端——请经 web-preview 入口预览(T28)";
    }
}

// [T28] bridge.on("scvb.state", (s) => { ... }) —— 契约 §2.1(全量/增量 state 快照)
// [T28] bridge.on("scvb.params", (p) => { ... }) —— 契约 §2.2(25Hz 参数批量快照)
// [T28] bridge.on("scvb.conn", (c) => { ... }) —— 契约 §2.3(每轨连接态,~4Hz)
// [T28] bridge.on("scvb.groups", (g) => { ... }) —— 契约 §2.4(J70,在线组位图,1Hz)
// [T28] bridge.on("scvb.meters", (m) => { ... }) —— 契约 §2.5(电平,30Hz)
// [T28] bridge.on("scvb.playhead", (p) => { ... }) —— 契约 §2.6(播放头,30Hz)
// [T28] bridge.on("scvb.captureProgress", (c) => { ... }) —— 契约 §2.7(采集覆盖增量)
// [T28] bridge.on("scvb.segments", (s) => { ... }) —— 契约 §2.8(分段结果)
// [T28] bridge.on("scvb.error", (e) => { ... }) —— 契约 §2.9(九错误码,驱动 §2.0 警告面)
// [T31] bridge.requestInitialState() —— 契约 §1.1(mBridgeReady 门控由页面掌握,首次装载调用一次)

// ------------------------------------------------------------- i18n(applyI18n 首刷 + 语言胶囊)
let lang = "zh";
applyI18n(document, lang);

function setLang(next) {
    if (!LANGS.includes(next)) return;
    lang = next;
    applyI18n(document, lang);
    document
        .querySelectorAll(
            '[data-lang="zh"], [data-lang="en"], [data-lang="fr"]',
        )
        .forEach((btn) => {
            btn.setAttribute(
                "aria-selected",
                String(btn.getAttribute("data-lang") === lang),
            );
        });
    // [T31] bridge.setLang(code) —— 契约 §1.30(纯 UI 本地切换 + 落盘调用点)
}

document.querySelectorAll("[data-lang]").forEach((btn) => {
    btn.addEventListener("click", () => setLang(btn.getAttribute("data-lang")));
});

// ------------------------------------------------------------- tab 路由(05 §2.0)
// 点击导航切 data-tab,CSS 按 #content[data-tab="x"] 选择器显隐;不写 state、
// 不调 setActiveTab(注释桩见下)。「滑动选中石板」是共享组件 .sc-tabs 自带的视觉,
// 这里只做必要的位置计算,不是额外视觉发挥。
const content = document.getElementById("content");
const tabbar = document.getElementById("tabbar");
const slate = document.getElementById("tabs-slate");

function positionSlate(btn) {
    if (!slate || !btn) return;
    slate.style.left = btn.offsetLeft + "px";
    slate.style.width = btn.offsetWidth + "px";
}

function activateTab(name) {
    content.setAttribute("data-tab", name);
    tabbar.querySelectorAll("[data-tab-btn]").forEach((btn) => {
        const active = btn.getAttribute("data-tab-btn") === name;
        btn.setAttribute("aria-selected", String(active));
        if (active) positionSlate(btn);
    });
    // [T31] bridge.setActiveTab(tab) —— 契约 §1.31(灰模不写 state;tour.js 期间同样不经此口,§2.6)
}

tabbar.querySelectorAll("[data-tab-btn]").forEach((btn) => {
    btn.addEventListener("click", () =>
        activateTab(btn.getAttribute("data-tab-btn")),
    );
});

activateTab("master");
// 首帧 slate 定位需等布局完成(offsetLeft/Width 在部分场景首帧为 0)。
requestAnimationFrame(() => {
    const active = tabbar.querySelector('[data-tab-btn="master"]');
    positionSlate(active);
});

// ------------------------------------------------------------- Tab2:15 轨行模板生成(05 §2.2)
// 仅用 FIFTEEN_TRACKS 撑开骨架(label 撑宽度),零状态渲染逻辑 —— 数值/开关态一律用
// 灰模默认值,不读 scvb.params/scvb.state(那是 T31 的事)。列序/宽度逐字照 05 §2.2
// 代码块,不得重排。
function tt(ch) {
    return String(ch).padStart(2, "0");
}

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
           词条 lowSample 字典已备,不需 todo;显隐数据源 scvb.captureProgress/scvb.segments,留 T31。 -->
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
        <select class="sc-select" style="width:100%" data-gb="tracks-row-${ch}-pair">
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
           灰模只落 DOM 与词条,一次性判定/展开/撤销栈全留 T31。 -->
      <div class="sc-confirm tracks-row__confirm" data-gb="tracks-row-${ch}-manual-overwrite-confirm" hidden>
        <span data-t="tracks.manualOverwriteConfirm"></span>
        <span data-t="tracks.manualOverwriteConfirm.locked" data-gb="tracks-row-${ch}-manual-overwrite-confirm-locked" hidden></span>
        <!-- 词条待立:通用「取消」按钮文案(字典缺通用 cancel key,同 deviations §B 首条) -->
        <button class="sc-btn" data-gb="tracks-row-${ch}-manual-overwrite-cancel" data-gb-todo="词条待立">取消</button>
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

// ------------------------------------------------------------- Tab1:Lead Select 15 个选项(05 §2.1④)
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

// ------------------------------------------------------------- Tab3:15 泳道模板生成(05 §2.3)
// 轨头/曲线叠加层/特征波形/VAD 着色/分段边界/采集覆盖进度 六件套,画布仅给尺寸注释,
// 零绘制逻辑。播放头与选区手柄是共享覆盖层,已在 index.html 静态占位,不随行生成。
function waveLaneHtml(ch, label) {
    return `
    <div class="wave-lane" role="row" data-gb="wave-lane-${ch}" data-ch="${ch}">
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
        <!-- 画布尺寸注释:随泳道区宽度撑满、高 60 设计 px,零绘制逻辑,T31/T33 实现 -->
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

// ------------------------------------------------------------- 缩放下拉(footer / 设置页,05 §1.2)
// 档位表单一真源 = web/shared/design-box.js 的 DESIGN.output.presets(05 §1.2「常量真源」栏)。
// footer 与设置页两处 <select> 的 <option> 都在此生成,index.html 内只留空 <select> ——
// 避免把同一份档位表复制成第二/第三真源(与 web/input/app.js 同一写法)。
// 属结构性模板生成(纯数值列表,无 i18n 内容、无状态渲染),不越 T27b 的「零业务逻辑」界。
for (const sel of document.querySelectorAll(
    '[data-gb="footer-scale-select"], [data-gb="settings-scale-select"]',
)) {
    for (const f of DESIGN.output.presets) {
        const opt = document.createElement("option");
        opt.value = String(f);
        opt.textContent = Math.round(f * 100) + "%";
        if (f === 1) opt.selected = true;
        sel.appendChild(opt);
    }
}
// 真正的 setUiScale/commitUiScale + 10 秒防呆 + 超屏档位过滤见各控件旁注释桩(index.html)。
// [T31] bridge.setUiScale(f) —— 契约 §1.28;[T31] bridge.commitUiScale() —— 契约 §1.29

// ------------------------------------------------------------- 键盘(05 §1.3 撤销/重做)
// 05 §1.3「键盘与撤销(R1 增补)」:WebView 内捕获 Ctrl+Z / Ctrl+Shift+Z 映射插件自有
// UndoManager 并 preventDefault 防止冒泡到宿主撤销;焦点在文本输入框时不拦截。
// 灰模不装监听(零业务逻辑),只把两个冻结契约函数名落成注释桩供 T31 接线与未来 grep:
// [T31] bridge.undo() —— 契约 §1.25(Ctrl+Z 捕获 + preventDefault)
// [T31] bridge.redo() —— 契约 §1.26(Ctrl+Shift+Z 捕获 + preventDefault)
