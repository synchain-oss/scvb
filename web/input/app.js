// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Input —— 单页接线(T36 正式实现,在 T27b 灰模上替换)。
// -----------------------------------------------------------------------------
// 职责:channel 轮带 + 远程视图 + 连接状态。createBridge → requestInitialState
// (契约 §0.6 门控由页面掌握)→ 订阅契约 §4 五个事件 → 维护 store → 驱动 header
// pill 四态/两红态 + 独立组 badge、组选择器(A-H 八胶囊 + 行内切组确认条)、通道
// 4×4 网格(未分配 + 01-15)、优先级 stepper、远程只读摘要、采集指示、footer 缩放。
// 纪律:函数名/事件名逐字照 web/shared/bridge.js 的 BRIDGE_FUNCTIONS/BRIDGE_EVENTS
// (= 冻结契约 docs/SCVB_CONTRACT.md §3/§4);状态写 data-* 属性;词条一律走 key。
// =============================================================================

import { createBridge } from "../shared/bridge.js";
import { applyI18n, LANGS, dict } from "../shared/i18n.js";
import { DESIGN } from "../shared/design-box.js";
import { createLangStart, shouldShowLangStart } from "../shared/lang-start.js";
import { createInputTour, shouldShowInputGuide } from "./tour-in.js";
import { sourceKind } from "../shared/source-kind.js";

// ------------------------------------------------------------- 单一真源常量
// 契约 §0.2:g = 1..8,UI 显示 A-H;ch = 1..15(J59)。
const GROUP_IDS = Object.freeze(["A", "B", "C", "D", "E", "F", "G", "H"]);
const CHANNEL_COUNT = 15;

// ------------------------------------------------------------- 设计盒尺寸(05 §1.2)
// 真源 = web/shared/design-box.js DESIGN.input;index.html 不写第二份数字。
const shell = document.getElementById("ipt-shell");
shell.style.setProperty("--ipt-w", DESIGN.input.w + "px");
shell.style.setProperty("--ipt-h", DESIGN.input.h + "px");

// ------------------------------------------------------------- createBridge
// 浏览器直开走 web-preview 才有 window.__SCVB_MOCK__;裸开时两者皆无 → 只 warn。
let bridge = null;
try {
    bridge = createBridge({
        role: "input",
        mockBackend: window.__SCVB_MOCK__,
    });
} catch (e) {
    console.warn("SCVB Input:createBridge 未接上后端 —— " + e.message);
    const hint = document.querySelector('[data-gb="input.footer.hint"]');
    if (hint) {
        hint.setAttribute("data-t", "in.footer.noBackend");
        hint.textContent = dict("zh")["in.footer.noBackend"];
    }
}

// ------------------------------------------------------------- 事件仓(单向渲染源)
const store = {
    ready: false, // requestInitialState() 已回(§0.6)
    snapshot: null, // §3.1 返回(channel_id/group_id/conn/config/ui/version)
    state: {}, // §4.1 scvb.state 合并(channel_id/group_id/claim/abi/abi_remote/ui)
    conn: null, // §4.2 scvb.conn(outputOnline/maskBit/capturing/passthrough/…)
    config: null, // §4.3 scvb.config(label/priority/lead_lock/pair_id/freeze/source_channels/channelLabels)
    groups: 0, // §4.4 groups_online 位图(事件缺失 = 0 ⇒ 绿点全灭、零报错)
    local: {
        pendingGroup: 0, // 切组确认条展开期间的目标组
        pendingRelease: false, // 释放确认条展开
        priorityLocal: null, // 优先级本地乐观值(等 scvb.config 回执让位)
        toastTimer: 0,
    },
    // 本会话一次性判定([J80]:不入 state chunk、零桥、零契约)。
    // guideClosed 是首启链的**会话级**闸门:setGuideSeen 若因 native 未落地而落空
    // (见 tour-in.js 头注),这两个标记仍保证本次会话里语言卡与 mini tour 不重弹。
    // 拦的是**预览 / mock 形态** —— 真宿主里下行也还没有 ui.guide_seen /
    // guide_seen_global,首启链根本不会自动弹,这两个标记只是空转。
    session: {
        guideClosed: false, // 本会话已走过/跳过 mini tour
        langChosen: false, // 本会话已在首启语言卡上选过语言
    },
};

// ------------------------------------------------------------- 小工具
function $(gb) {
    return document.querySelector('[data-gb="' + gb + '"]');
}
function $all(gb) {
    return document.querySelectorAll('[data-gb="' + gb + '"]');
}

/** 占位符模板:{x} → vals.x;缺失占位符原样保留(不静默丢字)。 */
function format(tpl, vals) {
    return String(tpl).replace(/\{([A-Za-z_][A-Za-z0-9_]*)\}/g, (m, k) =>
        vals && Object.prototype.hasOwnProperty.call(vals, k)
            ? String(vals[k])
            : m,
    );
}

function groupLetter(g) {
    return GROUP_IDS[(g || 1) - 1];
}

async function call(name, ...args) {
    if (!bridge || typeof bridge[name] !== "function") return null;
    try {
        return await bridge[name](...args);
    } catch (e) {
        console.warn("SCVB Input:bridge." + name + "() 失败 —— " + e.message);
        return null;
    }
}

function show(node, on) {
    if (node) node.hidden = !on;
}

/** disabled 类 tooltip:词条为空就移除 title(空 title 仍会弹空气泡)。 */
function setTitle(node, text) {
    if (!node) return;
    if (text) node.setAttribute("title", text);
    else node.removeAttribute("title");
}

/** 状态卡点亮:data-lit="0|1" 驱动 CSS 亮/暗。 */
function setLit(node, lit) {
    if (node) node.setAttribute("data-lit", lit ? "1" : "0");
}

/** 词条 + 占位符 → textContent(先写 key 供切语言重刷,再填占位符)。 */
function fillKeyed(node, key, vals) {
    if (!node) return;
    node.setAttribute("data-t", key);
    if (Object.prototype.hasOwnProperty.call(dictNow, key)) {
        node.textContent = format(dictNow[key], vals || {});
    }
}
function fill(node, key, vals) {
    fillKeyed(node, key, vals || {});
}

/** 冲突抖动(data-shake="1";animationend 清掉以便重放)。 */
function shake(node) {
    if (!node) return;
    node.setAttribute("data-shake", "1");
    node.addEventListener(
        "animationend",
        () => node.removeAttribute("data-shake"),
        { once: true },
    );
}

/** 一次性 toast(占用冲突反馈 ch.occupied;自动消失)。 */
function showOccupiedToast(channel, group) {
    const toast = $("input.toast.occupied");
    const text = $("input.toast.occupied.text");
    if (!toast) return;
    if (channel >= 1) {
        fill(text, "ch.occupied", { n: channel, g: groupLetter(group) });
    } else {
        // 未选通道(channel=0)时切组冲突不带通道号,避免误导「通道 1」
        fill(text, "ch.occupied.group", { g: groupLetter(group) });
    }
    toast.hidden = false;
    if (store.local.toastTimer) clearTimeout(store.local.toastTimer);
    store.local.toastTimer = setTimeout(() => {
        toast.hidden = true;
    }, 4000);
}

// ------------------------------------------------------------- i18n
let lang = "zh";
let dictNow = dict(lang);

function refreshI18n() {
    dictNow = applyI18n(document, lang);
    render();
}

function setLang(next, { push = true } = {}) {
    if (!LANGS.includes(next)) return;
    lang = next;
    document.querySelectorAll("[data-lang]").forEach((btn) => {
        btn.setAttribute(
            "aria-selected",
            String(btn.getAttribute("data-lang") === lang),
        );
    });
    refreshI18n();
    if (push) call("setLang", next);
}

document.querySelectorAll("[data-lang]").forEach((btn) => {
    btn.addEventListener("click", () => setLang(btn.getAttribute("data-lang")));
});

// ------------------------------------------------------------- 组胶囊(A-H)构建
function buildGroupPills() {
    const seg = $("input.group.pills");
    if (!seg) return;
    seg.replaceChildren(
        ...GROUP_IDS.map((id, i) => {
            const btn = document.createElement("button");
            btn.type = "button";
            btn.className = "ipt-group__pill";
            btn.setAttribute("data-gb", "input.group.pill");
            btn.setAttribute("data-group", String(i + 1));
            btn.setAttribute("role", "tab");
            btn.setAttribute("aria-selected", "false");
            btn.setAttribute("data-online", "0");
            btn.setAttribute("data-pending", "0");
            btn.textContent = id;
            const dot = document.createElement("span");
            dot.className = "sc-dot";
            dot.setAttribute("aria-hidden", "true");
            btn.appendChild(dot);
            return btn;
        }),
    );
}

// ------------------------------------------------------------- 通道 4×4 网格构建
function buildChannelGrid() {
    const grid = $("input.channels.grid");
    if (!grid) return;
    const frag = document.createDocumentFragment();
    frag.appendChild(channelCard(0));
    for (let ch = 1; ch <= CHANNEL_COUNT; ch++)
        frag.appendChild(channelCard(ch));
    grid.replaceChildren(frag);
}

function channelCard(ch) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "ipt-chcard";
    btn.setAttribute("data-gb", "input.channels.card");
    btn.setAttribute("data-ch", String(ch));
    btn.setAttribute("aria-pressed", "false");

    const num = document.createElement("span");
    num.className = "ipt-chcard__num sc-mono";
    if (ch === 0) {
        num.setAttribute("data-t", "in.unassigned");
        num.textContent = dictNow["in.unassigned"] || "未分配";
    } else {
        num.textContent = String(ch).padStart(2, "0");
    }
    btn.appendChild(num);

    const label = document.createElement("span");
    label.className = "ipt-chcard__label sc-mono";
    btn.appendChild(label);

    // 占用文字标(A8 缺件:右上角灰点 + 「占用」文字标)
    const occ = document.createElement("span");
    occ.className = "ipt-chcard__occupied";
    occ.hidden = true;
    const dot = document.createElement("span");
    dot.className = "sc-dot";
    dot.setAttribute("aria-hidden", "true");
    const occText = document.createElement("span");
    occText.setAttribute("data-t", "occupied");
    occText.textContent = dictNow["occupied"] || "占用";
    occ.append(dot, occText);
    btn.appendChild(occ);

    return btn;
}

// ------------------------------------------------------------- 组选择器接线
function wireGroup() {
    const seg = $("input.group.pills");
    if (seg) {
        seg.addEventListener("click", (e) => {
            const btn =
                e.target instanceof Element
                    ? e.target.closest("[data-group]")
                    : null;
            if (!btn) return;
            const g = Number(btn.getAttribute("data-group"));
            const cur = store.state.group_id || 1;
            if (!g || g === cur) return;
            store.local.pendingGroup = g;
            render();
        });
    }
    const cancel = $("input.group.confirm.cancel");
    if (cancel) {
        cancel.addEventListener("click", () => {
            store.local.pendingGroup = 0;
            render();
        });
    }
    const ok = $("input.group.confirm.primary");
    if (ok) {
        ok.addEventListener("click", async () => {
            const g = store.local.pendingGroup;
            store.local.pendingGroup = 0;
            if (!g) return;
            const res = await call("setGroupId", g);
            if (res && res.conflict === true) {
                // 契约 §3.3:新组同号 channel 被占 → 胶囊抖动 + 红 toast(带组号)
                const pill =
                    seg && seg.querySelector('[data-group="' + g + '"]');
                shake(pill);
                showOccupiedToast(store.state.channel_id || 0, g);
            }
            render();
        });
    }
}

// ------------------------------------------------------------- 通道选择器接线
function wireChannels() {
    const grid = $("input.channels.grid");
    if (grid) {
        grid.addEventListener("click", (e) => {
            const card =
                e.target instanceof Element
                    ? e.target.closest("[data-ch]")
                    : null;
            if (!card) return;
            const ch = Number(card.getAttribute("data-ch"));
            const cur = store.state.channel_id || 0;
            // 点「未分配」卡或再点当前已选卡 = 主动释放(R2 补释放入口)
            if (ch === 0 || ch === cur) {
                store.local.pendingRelease = true;
                render();
                return;
            }
            claimChannel(ch);
        });
    }
    const releaseCancel = $("input.channels.releaseConfirm.cancel");
    if (releaseCancel) {
        releaseCancel.addEventListener("click", () => {
            store.local.pendingRelease = false;
            render();
        });
    }
    const releaseOk = $("input.channels.releaseConfirm.primary");
    if (releaseOk) {
        releaseOk.addEventListener("click", async () => {
            store.local.pendingRelease = false;
            // n=0 = 释放当前 slot,本轨回到直通(契约 §3.2)
            await call("setChannelId", 0);
            render();
        });
    }
    const manualInput = $("input.channels.manual.input");
    const manualOk = $("input.channels.manual.confirm");
    if (manualInput && manualOk) {
        const commitManual = () => {
            const raw = manualInput.value.trim();
            if (raw === "") return;
            const n = Number(raw);
            if (!Number.isInteger(n) || n < 0 || n > CHANNEL_COUNT) {
                manualInput.value = "";
                return;
            }
            manualInput.value = "";
            if (n === 0) {
                store.local.pendingRelease = true;
                render();
                return;
            }
            claimChannel(n);
        };
        manualOk.addEventListener("click", commitManual);
        manualInput.addEventListener("keydown", (e) => {
            if (e.key === "Enter") {
                e.preventDefault();
                commitManual();
            }
        });
    }
}

async function claimChannel(ch) {
    // 主动切卡 = 放弃未决的释放/切组确认(否则残留确认条会误释放刚 claim 的通道)
    store.local.pendingRelease = false;
    store.local.pendingGroup = 0;
    // 契约 §3.2:claim 本组 InputSlot[n-1];已被心跳新鲜实例占 → {conflict:true}
    const res = await call("setChannelId", ch);
    if (res && res.conflict === true) {
        const card = $('input.channels.card[data-ch="' + ch + '"]');
        shake(card);
        showOccupiedToast(ch, store.state.group_id || 1);
    }
    render();
}

// ------------------------------------------------------------- 优先级滑杆接线
// (05 §3 stepper → range,用户 2026-08-23 preview 指令;语义不变:0..10,10=最高)
function wirePriority() {
    const slider = $("input.priority.slider");
    if (!slider) return;
    // 拖动档:本地乐观显示(priorityLocal),回执以 scvb.config 为准(契约 §3.4)
    slider.addEventListener("input", () => {
        store.local.priorityLocal = Number(slider.value);
        render();
    });
    // 松手档:经 ctrl 命令环写 remoteSetPriority(契约 §3.4)
    slider.addEventListener("change", () => {
        const next = Number(slider.value);
        if (priorityBlockReason() !== null) return;
        call("remoteSetPriority", next).then((res) => {
            if (res && res.queued === false && res.reason === "ringFull") {
                // 满环:设置未送达 —— 回滚乐观值(契约 §3.4 的 UI 提示由 footer 承担)
                store.local.priorityLocal = null;
                render();
            }
        });
    });
}

function currentPriority() {
    if (store.local.priorityLocal !== null) return store.local.priorityLocal;
    return store.config ? store.config.priority : 0;
}

/** 契约 §3.4 拒绝态两 reason:unassigned(channel_id=0)/ offline(Output 离线)。 */
function priorityBlockReason() {
    if ((store.state.channel_id || 0) === 0) return "unassigned";
    const conn = store.conn || {};
    if (!conn.outputOnline) return "offline";
    return null;
}

// ------------------------------------------------------------- 缩放(05 §1.2 / 契约 §3.5/§3.6)
// Input 档位 = DESIGN.input.presets(10 档);setUiScale 立即预览 → 10 秒防呆确认。
const scaleUi = {
    select: $("input.footer.scale"),
    confirm: $("input.scale.confirm"),
    countdown: $("input.scale.confirm.countdown"),
    bodyPre: $("input.scale.confirm.bodyPre"),
    bodyPost: $("input.scale.confirm.bodyPost"),
    keep: $("input.scale.confirm.keep"),
    revert: $("input.scale.confirm.revert"),
};
let scaleTimer = 0;
let scalePrev = 1;
let scaleLeft = 0;

function currentScale() {
    const ui = store.state.ui;
    return ui && Number.isFinite(ui.scale) ? ui.scale : 1;
}

function scaleOverflows(f) {
    const s = typeof window !== "undefined" ? window.screen : null;
    if (!s || !s.availWidth || !s.availHeight) return false;
    return (
        DESIGN.input.w * f > s.availWidth || DESIGN.input.h * f > s.availHeight
    );
}

function renderScaleConfirm() {
    const raw = dictNow["scale.confirmBody"];
    if (typeof raw === "string" && scaleUi.bodyPre && scaleUi.bodyPost) {
        const [pre, post] = raw.split("{s}");
        scaleUi.bodyPre.textContent = pre;
        scaleUi.bodyPost.textContent = post === undefined ? "" : post;
    }
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
    call("setUiScale", scalePrev);
}

async function previewScale(f) {
    if (!scaleTimer) scalePrev = currentScale();
    const res = await call("setUiScale", f);
    if (res && res.ok === false) return;
    scaleLeft = 10;
    if (scaleUi.confirm) scaleUi.confirm.hidden = false;
    renderScaleConfirm();
    if (scaleTimer) clearInterval(scaleTimer);
    scaleTimer = setInterval(() => {
        scaleLeft -= 1;
        renderScaleConfirm();
        if (scaleLeft <= 0) revertScale();
    }, 1000);
}

function buildScaleOptions() {
    if (!scaleUi.select) return;
    scaleUi.select.replaceChildren();
    for (const f of DESIGN.input.presets) {
        const opt = document.createElement("option");
        opt.value = String(f);
        opt.textContent = Math.round(f * 100) + "%";
        opt.disabled = scaleOverflows(f);
        if (opt.disabled) opt.title = dictNow["scale.overflow"] || "";
        if (f === 1) opt.selected = true;
        scaleUi.select.appendChild(opt);
    }
    scaleUi.select.addEventListener("change", () =>
        previewScale(Number(scaleUi.select.value)),
    );
}

if (scaleUi.keep) {
    scaleUi.keep.addEventListener("click", () => {
        stopScaleCountdown();
        call("commitUiScale");
    });
}
if (scaleUi.revert) scaleUi.revert.addEventListener("click", revertScale);
addEventListener("pagehide", stopScaleCountdown);

// ------------------------------------------------------------- 首启轻量引导([J80] T48)
// 链条:独立语言卡(web/shared/lang-start.js,与 Output 同一件)→ 5 步 mini tour
// (web/input/tour-in.js)。Output 那条链中间还有红字九条页与询问步 —— Input 侧 J80 定的
// 是「轻量」:语言卡之后直接进第 1 步(欢迎居中卡),那张卡自带 Skip,不再单设询问步。
const tour = createInputTour({
    root: document,
    card: shell,
    bridge,
    getT: () => dictNow,
    onEnd: () => {
        // 完成与 Skip 同样落会话标记(桥侧的置位在 tour-in.js 的 endTour 里,J50a 镜像)
        store.session.guideClosed = true;
        render();
    },
});
tour.mount();

const langStart = createLangStart({
    root: document,
    card: shell,
    onPick: (code) => {
        store.session.langChosen = true;
        langStart.setShown(false);
        setLang(code); // 复用既有语言切换与持久化(契约 §3.7 setLang)
        tour.start(); // 选完语言直接进 mini tour 首步
    },
});
langStart.mount();

// header「?」= 重看引导(Input 无设置页,这是唯一重看入口;guide_seen 已置位也可再开)。
// replay:true —— 重看结束时不写已读位(setGuideSeen 只是首启的写入口,见契约变更说明 §四)。
const helpBtn = $("input.header.help");
if (helpBtn)
    helpBtn.addEventListener("click", () => tour.start({ replay: true }));

/**
 * 首启链的渲染侧闸门(每帧跑,判据 = 纯函数):
 *   • 语言卡:guide 未见 ∧ 全局位未置 ∧ 本会话未选过语言 ∧ 本会话未走完引导;
 *   • mini tour:语言卡已让位(选过语言)后由 onPick 直接拉起;此处只兜底
 *     「快照到达时语言卡已被跳过但引导仍未见」的边角(例:用户切了 header 语言胶囊)。
 */
function renderGuide() {
    const snap = store.ready ? store.snapshot : null;
    langStart.setShown(
        shouldShowLangStart(
            store.state,
            snap,
            store.session.guideClosed,
            store.session.langChosen,
        ),
    );
    if (
        !langStart.isShown() &&
        !tour.isActive() &&
        store.session.langChosen &&
        shouldShowInputGuide(store.state, snap, store.session.guideClosed)
    ) {
        tour.start();
    }
}

// ------------------------------------------------------------- 渲染
function render() {
    renderGuide();
    renderHeader();
    renderBanners();
    renderGroup();
    renderChannels();
    renderSource();
    renderPriority();
    renderRemoteSummary();
    renderCapture();
    renderFooter();
}

/** 本组之外是否有别的组在线(groups_online 位图里有非本组的置位)。 */
function otherGroupOnline() {
    const g = store.state.group_id || 1;
    const others = store.groups & ~(1 << (g - 1));
    return others !== 0;
}

/**
 * pill 状态推导(05 §3 Header 行 + §5.1 例外① + J71③):
 * 红两态(abi/sr) > 未选择通道 > 组不匹配(等待 Output · 组 {X})> Output 未运行
 * > 等待 Output > 已连接。组号只进 group-mismatch 这条,其余走独立 badge。
 */
function pillState() {
    const claim = store.state.claim;
    const channelId = store.state.channel_id;
    const conn = store.conn || {};
    if (claim === "abiMismatch") {
        return {
            key: "in.pill.abiMismatch",
            tone: "red",
            pulse: false,
            vals: {},
        };
    }
    if (claim === "srMismatch") {
        return {
            key: "in.pill.srMismatch",
            tone: "red",
            pulse: false,
            vals: {},
        };
    }
    // 首帧门:快照/首帧 scvb.state 到达前 channel_id 为 undefined ——
    // 不拦在这里会一路落到 state.connected 绿「已连接」(违反契约 §0.6 首帧前不得渲染真实数据态)。
    if (channelId === 0 || channelId == null || claim === "unassigned") {
        return { key: "state.noChannel", tone: "gray", pulse: false, vals: {} };
    }
    if (conn.outputOnline === false) {
        if (otherGroupOnline()) {
            return {
                key: "state.waitingForOutput.group",
                tone: "amber",
                pulse: false,
                vals: { X: groupLetter(store.state.group_id) },
            };
        }
        return {
            key: "state.outputOffline",
            tone: "gray",
            pulse: false,
            vals: {},
        };
    }
    if (conn.maskBit === false) {
        return {
            key: "state.waitingForOutput",
            tone: "amber",
            pulse: false,
            vals: {},
        };
    }
    return { key: "state.connected", tone: "green", pulse: true, vals: {} };
}

/** 音频路径副文案(J12/J32;直通副文案对红两态照常)。 */
function pillSubKey() {
    const conn = store.conn || {};
    if (conn.passthroughPending) return "in.pillSub.hysteresis";
    if (conn.passthrough === false) return "in.pillSub.takenOver";
    return "in.pillSub.passthrough";
}

function renderHeader() {
    const badge = $("input.header.groupBadgeLetter");
    if (badge) badge.textContent = groupLetter(store.state.group_id);

    const ps = pillState();
    const pill = $("input.header.pill");
    if (pill) {
        pill.setAttribute("data-tone", ps.tone);
        pill.setAttribute("data-pulse", ps.pulse ? "1" : "0");
        fillKeyed($("input.header.pillText"), ps.key, ps.vals);
    }
    fillKeyed($("input.header.pillSub"), pillSubKey(), {});
}

function renderBanners() {
    // group.noOutput 异组引导:本组无 Output 但异组在线,且组号非默认(或用户改过)
    const conn = store.conn || {};
    const groupMismatch = conn.outputOnline === false && otherGroupOnline();
    show($("input.banner.noOutput"), groupMismatch);
    if (groupMismatch) {
        fill($("input.banner.noOutput.text"), "group.noOutput", {
            g: groupLetter(store.state.group_id),
        });
    }
    // in.chHint.groupEmpty 同场景通道区提示
    show($("input.channels.groupEmptyHint"), groupMismatch);

    // banner.abiMismatch(claim=abiMismatch;两端 abi 取 scvb.state,契约 §4.1)
    const abiMismatch = store.state.claim === "abiMismatch";
    show($("input.banner.abiMismatch"), abiMismatch);
    if (abiMismatch) {
        fill($("input.banner.abiMismatch.text"), "banner.abiMismatch", {
            a: store.state.abi_remote,
            b: store.state.abi,
        });
    }
}

function renderGroup() {
    const cur = store.state.group_id || 1;
    const seg = $("input.group.pills");
    if (seg) {
        for (const btn of seg.querySelectorAll("[data-group]")) {
            const gi = Number(btn.getAttribute("data-group"));
            btn.setAttribute("aria-selected", String(gi === cur));
            btn.setAttribute(
                "data-online",
                String((store.groups >>> (gi - 1)) & 1 ? "1" : "0"),
            );
            btn.setAttribute(
                "data-pending",
                store.local.pendingGroup === gi ? "1" : "0",
            );
        }
    }
    const confirm = $("input.group.confirm");
    const pending = store.local.pendingGroup;
    show(confirm, pending !== 0);
    if (pending) {
        const letter = groupLetter(pending);
        fill($("input.group.confirm.text"), "group.switchConfirm", {
            x: letter,
            y: groupLetter(cur),
            n: store.state.channel_id || 0,
        });
        fill($("input.group.confirm.primary"), "group.switchConfirm.primary", {
            x: letter,
        });
    }
}

function renderChannels() {
    const cur = store.state.channel_id || 0;
    const conn = store.conn || {};
    const occupiedMask = conn.occupiedMask || 0;
    const labels = (store.config && store.config.channelLabels) || [];
    for (const card of $all("input.channels.card")) {
        const ch = Number(card.getAttribute("data-ch"));
        card.setAttribute("aria-pressed", String(ch === cur));
        const label = card.querySelector(".ipt-chcard__label");
        if (label) {
            label.textContent =
                ch >= 1 && ch <= labels.length ? labels[ch - 1] : "";
        }
        // 占用 = 本组该 slot 被心跳新鲜实例占(含自己),UI 以 channel_id 区分自身
        const occupiedByOther =
            ch >= 1 && ((occupiedMask >>> (ch - 1)) & 1) === 1 && ch !== cur;
        const occ = card.querySelector(".ipt-chcard__occupied");
        if (occ) occ.hidden = !occupiedByOther;
        if (occupiedByOther) {
            card.setAttribute(
                "title",
                format(dictNow["ch.occupied"] || "", {
                    n: ch,
                    g: groupLetter(store.state.group_id),
                }),
            );
        } else {
            card.removeAttribute("title");
        }
    }
    // 首开空态引导(未选 channel)
    show($("input.channels.emptyHint"), cur === 0);
    // 释放确认条(点未分配/再点当前卡触发;展开期间暂隐远程摘要行)
    const release = $("input.channels.releaseConfirm");
    show(release, store.local.pendingRelease);
    if (store.local.pendingRelease) {
        fill($("input.channels.releaseConfirm.text"), "in.releaseConfirm", {
            n: cur,
        });
    }
}

function renderSource() {
    // 本轨 mono/stereo 实测结果(J57,只读,来自 scvb.config.source_channels)
    const sc = store.config ? store.config.source_channels : 0;
    const row = $("input.source");
    // source_channels 1=mono 2=stereo;0/undefined = 尚未测量 —— 隐藏本行,等有效值(J63 首帧核实)。
    const kind = sourceKind(sc);
    const measured = kind !== "unmeasured";
    show(row, !!store.config && measured);
    if (!row || !store.config || !measured) return;
    const stereo = kind === "stereo";
    const badge = $("input.source.badge");
    if (badge) badge.textContent = stereo ? "ST" : "MONO";
    fillKeyed(
        $("input.source.hint"),
        stereo ? "in.pillSub.stereo" : "in.source.mono",
        {},
    );
}

function renderPriority() {
    const val = $("input.priority.stepper.val");
    const slider = $("input.priority.slider");
    const reason = priorityBlockReason();
    const blocked = reason !== null;
    const stepper = $("input.priority.stepper");
    if (stepper) {
        stepper.setAttribute("data-disabled", blocked ? "1" : "0");
        setTitle(
            stepper,
            blocked
                ? dictNow[
                      reason === "unassigned"
                          ? "in.priority.unassigned"
                          : "in.priority.offline"
                  ]
                : "",
        );
    }
    const prio = String(currentPriority());
    if (val) {
        val.textContent = prio;
        val.setAttribute("data-disabled", blocked ? "1" : "0");
    }
    if (slider) {
        slider.disabled = blocked;
        slider.setAttribute("aria-disabled", String(blocked));
        slider.value = prio;
        slider.setAttribute("aria-valuenow", prio);
        slider.setAttribute("aria-valuetext", prio);
    }
}

function renderRemoteSummary() {
    // 05 §3:Output 离线隐藏;确认条展开期间暂隐;其余(已选通道)显示,未激活 = 暗淡。
    const conn = store.conn || {};
    const confirmOpen =
        store.local.pendingGroup !== 0 || store.local.pendingRelease;
    const cfg = store.config;
    const showRow =
        !!cfg &&
        conn.outputOnline === true &&
        !confirmOpen &&
        (store.state.channel_id || 0) >= 1;
    show($("input.remoteSummary"), showRow);
    if (!showRow || !cfg) return;

    // MONO / STEREO:源类型状态(只读 J57),当前哪种亮哪种;未测量(0/undefined)两者都不亮。
    const kind = sourceKind(cfg.source_channels || 0);
    const stereo = kind === "stereo";
    const mono = kind === "mono";
    setLit($("input.remoteSummary.mono"), mono);
    setLit($("input.remoteSummary.stereo"), stereo);
    const monoCard = $("input.remoteSummary.mono");
    const stereoCard = $("input.remoteSummary.stereo");
    setTitle(monoCard, "MONO");
    monoCard.setAttribute("aria-label", "MONO");
    setTitle(stereoCard, "STEREO");
    stereoCard.setAttribute("aria-label", "STEREO");

    // lead / pair / freeze:激活点亮,未激活暗淡(不隐藏)。
    const hasLead = !!cfg.lead_lock;
    const hasPair = (cfg.pair_id || 0) !== 0;
    const f = cfg.freeze || 0;
    const hasFreeze = f !== 0;
    setLit($("input.remoteSummary.lead"), hasLead);
    setLit($("input.remoteSummary.pair"), hasPair);
    setLit($("input.remoteSummary.freeze"), hasFreeze);

    const lead = $("input.remoteSummary.lead");
    setTitle(lead, dictNow["leadLock"]);
    lead.setAttribute("aria-label", dictNow["leadLock"]);

    const pair = $("input.remoteSummary.pair");
    const pairLabel = hasPair
        ? (dictNow["pair"] || "配对") + " " + cfg.pair_id
        : dictNow["pair"] || "配对";
    setTitle(pair, pairLabel);
    pair.setAttribute("aria-label", pairLabel);
    const num = $("input.remoteSummary.pairId");
    if (num) num.textContent = hasPair ? String(cfg.pair_id) : "";

    const freeze = $("input.remoteSummary.freeze");
    // 短标:"冻结P"→"冻结"、"FRZ P"→"FRZ"、"GEL P"→"GEL"
    const freezeBase = (dictNow["tracks.colFreezePan"] || "冻结P").replace(
        /\s*[PV]$/,
        "",
    );
    const freezeLabelEl = $("input.remoteSummary.freezeLabel");
    if (freezeLabelEl) freezeLabelEl.textContent = freezeBase;
    const parts = [];
    if (f & 1) parts.push(dictNow["tracks.colFreezePan"] || "冻结P");
    if (f & 2) parts.push(dictNow["tracks.colFreezeVol"] || "冻结V");
    const freezeTitle = parts.length
        ? parts.join("/")
        : (dictNow["tracks.colFreezePan"] || "冻结P") +
          "/" +
          (dictNow["tracks.colFreezeVol"] || "冻结V");
    setTitle(freeze, freezeTitle);
    freeze.setAttribute("aria-label", freezeTitle);
}

function renderCapture() {
    // 采集指示(小绿点「采集中」):本实例 flags bit0 → scvb.conn.capturing(契约 §4.2)
    const conn = store.conn || {};
    show($("input.captureIndicator"), !!conn.capturing);
}

function renderFooter() {
    const ver = $("input.footer.version");
    if (ver && store.snapshot && store.snapshot.version) {
        ver.textContent = "v" + store.snapshot.version.plugin;
    }
    // 满环回执(remoteSetPriority → {queued:false,reason:"ringFull"})的 footer 提示
    // 由 priorityLocal 回滚 + footer 静默承担;Input footer 无打印状态行,保持 in.footer.hint。
}

// ------------------------------------------------------------- 事件订阅(契约 §4 五事件)

// Input scvb.state 合并:顶层标量整字段替换,ui 子树按字段深合并
// (setUiScale 只推 {ui:{scale}} 一类稀疏 patch,浅合并会把 language 一并抹掉)。
function mergeInputState(prev, next) {
    const out = { ...prev, ...(next || {}) };
    if (prev.ui && next && next.ui) {
        out.ui = { ...prev.ui, ...next.ui };
    }
    return out;
}

if (bridge) {
    bridge.on("scvb.state", (s) => {
        store.state = mergeInputState(store.state, s);
        syncUiFromState();
        render();
    });

    bridge.on("scvb.conn", (c) => {
        store.conn = c;
        render();
    });

    bridge.on("scvb.config", (c) => {
        store.config = c;
        // 本地乐观值让位给回执(契约 §3.4:以 scvb.config 回执为准)
        if (c && Number.isFinite(c.priority)) {
            store.local.priorityLocal = null;
        }
        render();
    });

    bridge.on("scvb.groups", (g) => {
        // §4.4:事件缺失时绿点全灭、零报错 → 默认值 0,此处不做字段校验
        store.groups = (g && g.groups_online) || 0;
        render();
    });

    bridge.on("scvb.error", (e) => {
        if (!e || !e.code) return;
        // §4.5:Input 实际 code = channelConflict / srMismatch;冲突反馈已由
        // setChannelId/setGroupId 的返回 + ch.occupied toast 承担,这里不重复弹。
        if (e.code === "srMismatch" && store.state.claim !== "srMismatch") {
            store.state = { ...store.state, claim: "srMismatch" };
            render();
        }
    });
}

/** state 里的 ui(语言 / 缩放)回推到页面本地态 —— 回推不再上行,避免自激。 */
function syncUiFromState() {
    const ui = store.state.ui || {};
    if (ui.language && ui.language !== lang && LANGS.includes(ui.language)) {
        setLang(ui.language, { push: false });
    }
    if (ui.scale && Number.isFinite(ui.scale)) {
        shell.style.zoom = String(ui.scale);
        if (scaleUi.select) scaleUi.select.value = String(ui.scale);
    }
}

// ------------------------------------------------------------- 首帧
buildGroupPills();
buildChannelGrid();
buildScaleOptions();
wireGroup();
wireChannels();
wirePriority();
refreshI18n();

(async function boot() {
    try {
        await bootInner();
    } catch (e) {
        // 首帧链路炸掉 = 界面起不来。显式上报,免得 native 侧只能等看门狗超时,
        // 且兜底面板还写着「加载太慢」这种误导文案(守卫装在 index.html 的 <head>)。
        if (typeof window.__scvbReportBootError === "function") {
            window.__scvbReportBootError("input-boot", (e && e.stack) || e);
        }
        throw e;
    }
})();

async function bootInner() {
    if (!bridge) {
        render();
        return;
    }
    const snap = await call("requestInitialState");
    if (snap) {
        store.snapshot = snap;
        // §3.1:快照 = channel_id/group_id/conn/config/ui/version;claim 只经 §4.1
        // scvb.state 下发,不在快照字段集里 —— 这里只并 state 面字段,元数据留旁路。
        const { conn: snapConn, config: snapConfig, ...stateFields } = snap;
        store.state = { ...store.state, ...stateFields };
        store.conn = snapConn || store.conn;
        store.config = snapConfig || store.config;
        store.ready = true;
        syncUiFromState();
    }
    render();
}
