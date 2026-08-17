// SPDX-License-Identifier: GPL-3.0-or-later
// web/input/app.js —— SCVB Input 单页灰模最小接线(T27b)。
//
// 灰模纪律(scratchpad/t27b/t27b-brief.md 逐条照做,不得扩权):
//   • 除 createBridge() 调用本身 + applyI18n() 调用本身外,不写任何业务逻辑——
//     无状态机、无渲染循环、无真实事件处理实现。
//   • 每个交互组件的桥调用点一律以【注释桩】落位(函数名逐字照冻结契约
//     docs/SCVB_CONTRACT.md,供 check-bridge-parity.mjs 一类未来 grep 扫到),
//     不实际发起调用——真正接线是 T31-T36 的工作,本文件只占位、不实现。
//   • 语言胶囊按 05 §2.0 明文允许「点击瞬时切换,纯 UI 本地」——这是本文件
//     唯一落了真实交互的组件;其余组件(组胶囊/通道卡/优先级 stepper/…)一律
//     只留注释桩,不挂 click 逻辑,避免把灰模写成半成品实现。
//   • 浏览器直开必须零 console.error(console.warn 允许)。
//
// 真源:masterPlan/plan/05-ui-spec.md §3 / §1.2;契约 docs/SCVB_CONTRACT.md §3-§4。

import { createBridge } from "../shared/bridge.js";
import { applyI18n } from "../shared/i18n.js";
import { DESIGN } from "../shared/design-box.js";

const shell = document.getElementById("ipt-shell");

// ---------------------------------------------------------------------------
// 设计盒尺寸(05 §1.2 单一真源 = web/shared/design-box.js,JS 设置项)
// ---------------------------------------------------------------------------
shell.style.setProperty("--ipt-w", DESIGN.input.w + "px");
shell.style.setProperty("--ipt-h", DESIGN.input.h + "px");
// 缩放机制本体(style.zoom = F / setSize 同步 / 10 秒防呆确认,05 §1.2)是 T31 的实现范围,
// 灰模只把 1x 基准盒尺寸落到位;zoom 档位下拉的选项生成见下方「Footer · 缩放下拉」小节。

// ---------------------------------------------------------------------------
// createBridge(role:"input") —— 唯一允许的真实业务调用(brief 明文要求)
// ---------------------------------------------------------------------------
let bridge = null;
try {
    bridge = createBridge({ role: "input", mockBackend: window.__SCVB_MOCK__ });
} catch (e) {
    console.warn("createBridge(input) 失败,按未接后端渲染 footer 提示:", e);
    const hint = document.querySelector('[data-gb="input.footer.hint"]');
    if (hint) {
        hint.removeAttribute("data-gb-todo");
        hint.textContent = "未接后端——请经 web-preview 入口预览(T28)";
    }
}

// ---------------------------------------------------------------------------
// applyI18n() —— 唯一允许的真实业务调用(brief 明文要求);首帧用中文兜底,
// 真实语言值来自 requestInitialState() 的 ui.language(T31 接线,本文件不调用)。
// ---------------------------------------------------------------------------
let currentLang = "zh";
applyI18n(document, currentLang);

// ---------------------------------------------------------------------------
// 语言胶囊(05 §2.0:点击瞬时切换,选中态属性同步,纯 UI 本地)
// 本页胶囊挂的是 role="tab"(外层 role="tablist"),选中态属性因此是 aria-selected;
// Output 侧同款胶囊是普通 <button>,那边用 aria-pressed(普通 button 上 aria-selected
// 违反 ARIA)。两页语义分叉、都通过 axe,统一由 T31 定夺。
// ---------------------------------------------------------------------------
const langSeg = document.querySelector('[data-gb="input.header.lang"]');
if (langSeg) {
    langSeg.querySelectorAll("[data-lang]").forEach((btn) => {
        btn.addEventListener("click", () => {
            currentLang = btn.getAttribute("data-lang");
            langSeg
                .querySelectorAll("[data-lang]")
                .forEach((b) =>
                    b.setAttribute("aria-selected", String(b === btn)),
                );
            applyI18n(document, currentLang);
            // [T3x] bridge.setLang(currentLang) —— 契约 §3.7(同 §1.30 签名);
            // 灰模不落盘,仅本地视觉切换,真实回环由 T31 接线。
        });
    });
}

// ---------------------------------------------------------------------------
// Footer · 缩放下拉 —— 档位表来自 design-box.js(DESIGN.input.presets,05 §1.2/§2.7 A8),
// 仅生成 <option> 列表(结构性模板生成,非状态渲染),不接 change 业务逻辑。
// ---------------------------------------------------------------------------
const scaleSelect = document.querySelector('[data-gb="input.footer.scale"]');
if (scaleSelect) {
    for (const f of DESIGN.input.presets) {
        const opt = document.createElement("option");
        opt.value = String(f);
        opt.textContent = f + "x";
        if (f === 1) opt.selected = true;
        scaleSelect.appendChild(opt);
    }
    // [T3x] scaleSelect.addEventListener("change", ...) →
    //   bridge.setUiScale(f) —— 契约 §3.5(同 §1.28 签名,{ok}|{ok:false,reason:"badArg"})
    //   → 预览 style.zoom → 10 秒防呆确认框(复用 .sc-modal/.sc-scrim)→ 确认后
    //   bridge.commitUiScale() —— 契约 §3.6(同 §1.29);取消/超时/关窗回退。
    //   超屏档位过滤(W*F>screen.availWidth ∨ H*F>screen.availHeight)同样留 T31。
}

// ---------------------------------------------------------------------------
// 桥调用点注释桩(05 §3 逐组件;函数名/签名逐字照 docs/SCVB_CONTRACT.md,未来
// grep/check-bridge-parity.mjs 一类门禁可扫到死名)。均不实际绑定事件监听。
// ---------------------------------------------------------------------------
//
// [T3x] requestInitialState() —— 契约 §3.1;首帧加载后调用一次,取
//        {channel_id, group_id, conn, config, ui, version} 全量快照驱动下方各态。
//
// [input.group.pill] 点击非当前组胶囊 →
//   1) 就地展开 input.group.confirm(文案 group.switchConfirm,占位符 {x}{y}{n})
//   2) 确认按钮(input.group.confirm.primary)→ bridge.setGroupId(g) —— 契约 §3.3
//      (同 §1.4 签名,返回 {ok:true}|{conflict:true})
//   3) 确认条展开期间暂隐 input.remoteSummary(05 §3 560px 高度预算硬要求)
//
// [input.channels.card] 点击编号卡 → bridge.setChannelId(n) —— 契约 §3.2
//   (返回 {ok:true}|{conflict:true};冲突 → 卡片 data-shake="1" + 展示
//   input.toast.occupied,文案 ch.occupied)
// [input.channels.card data-ch="0"] 或再点当前已选卡 → 展开
//   input.channels.releaseConfirm → 确认(.primary)→ bridge.setChannelId(0)
// [input.channels.manual.confirm] → 校验后同样调用 bridge.setChannelId(n)
//   (契约 §3.2;n=0 同释放确认路径)
//
// [input.priority.stepper.dec / .inc] → bridge.remoteSetPriority(n) —— 契约 §3.4
//   (ctrl 命令环,返回 {queued:bool, reason?})；Output 离线时按钮组 disabled。
//
// ---------------------------------------------------------------------------
// 事件订阅点注释桩(05 §3 数据来源列;契约 §4,共 5 个,均只订阅、不改动本文件行为)
// ---------------------------------------------------------------------------
//
// [T3x] bridge?.on("scvb.state", (s) => { ... })   —— 契约 §4.1
//   驱动:input.header.pill 四态/两红态、input.channels.card 选中态、
//   input.header.pillSub 音频路径副文案。
// [T3x] bridge?.on("scvb.conn", (c) => { ... })    —— 契约 §4.2
//   驱动:input.header.pill 在线判定、input.header.pillSub 滞回过渡提示、
//   input.captureIndicator 显隐、input.channels.card 的 occupied 灰点。
// [T3x] bridge?.on("scvb.config", (c) => { ... })  —— 契约 §4.3
//   驱动:input.remoteSummary(lead/pair/freeze)、input.priority.stepper.val
//   回执、input.channels.card 的 Output 侧 label(channelLabels)。
// [T3x] bridge?.on("scvb.groups", (g) => { ... })  —— 契约 §4.4
//   驱动:input.group.pill 内在线绿点(scvb.dot 无 tone → tone="green")。
// [T3x] bridge?.on("scvb.error", (e) => { ... })   —— 契约 §4.5
//   驱动:input.banner.abiMismatch 显隐、input.toast.occupied(channelConflict)、
//   input.header.pill 的 srMismatch 红态(契约 §5.1 九码取 Input 侧两码)。
