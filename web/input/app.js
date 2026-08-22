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
    if (hint) hint.textContent = "No backend attached — open via web-preview";
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
    fill(text, "ch.occupied", {
        n: channel,
        g: groupLetter(group),
    });
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
                showOccupiedToast(store.state.channel_id || 1, g);
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
    // 契约 §3.2:claim 本组 InputSlot[n-1];已被心跳新鲜实例占 → {conflict:true}
    const res = await call("setChannelId", ch);
    if (res && res.conflict === true) {
        const card = $('input.channels.card[data-ch="' + ch + '"]');
        shake(card);
        showOccupiedToast(ch, store.state.group_id || 1);
    }
    render();
}

// ------------------------------------------------------------- 优先级 stepper 接线
function wirePriority() {
    const dec = $("input.priority.stepper.dec");
    const inc = $("input.priority.stepper.inc");
    if (!dec || !inc) return;
    dec.addEventListener("click", () => stepPriority(-1));
    inc.addEventListener("click", () => stepPriority(1));
}

function currentPriority() {
    if (store.local.priorityLocal !== null) return store.local.priorityLocal;
    return store.config ? store.config.priority : 0;
}

function priorityBlocked() {
    const conn = store.conn || {};
    return (
        !store.ready ||
        !conn.outputOnline ||
        (store.state.channel_id || 0) === 0
    );
}

async function stepPriority(delta) {
    if (priorityBlocked()) return;
    const next = Math.max(0, Math.min(10, currentPriority() + delta));
    // 本地乐观显示,以 scvb.config 回执为准(契约 §3.4)
    store.local.priorityLocal = next;
    render();
    const res = await call("remoteSetPriority", next);
    if (res && res.queued === false && res.reason === "ringFull") {
        // 满环:设置未送达 —— 回滚乐观值(契约 §3.4 的 UI 提示由 footer 承担)
        store.local.priorityLocal = null;
        render();
    }
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

// ------------------------------------------------------------- 渲染
function render() {
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
    show(row, !!store.config);
    if (!row || !store.config) return;
    const stereo = sc === 2;
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
    if (val) val.textContent = String(currentPriority());
    const blocked = priorityBlocked();
    const stepper = $("input.priority.stepper");
    if (stepper) {
        stepper.setAttribute("data-disabled", blocked ? "1" : "0");
        setTitle(stepper, blocked ? dictNow["in.priority.offline"] : "");
    }
    const dec = $("input.priority.stepper.dec");
    const inc = $("input.priority.stepper.inc");
    if (dec) dec.setAttribute("data-disabled", blocked ? "1" : "0");
    if (inc) inc.setAttribute("data-disabled", blocked ? "1" : "0");
}

function renderRemoteSummary() {
    // 05 §3:Output 离线隐藏;确认条展开期间暂隐(守 560px 高度预算)
    const conn = store.conn || {};
    const confirmOpen =
        store.local.pendingGroup !== 0 || store.local.pendingRelease;
    const cfg = store.config;
    const showRow = !!cfg && conn.outputOnline === true && !confirmOpen;
    show($("input.remoteSummary"), showRow);
    if (!showRow || !cfg) return;
    show($("input.remoteSummary.lead"), !!cfg.lead_lock);
    show($("input.remoteSummary.pair"), (cfg.pair_id || 0) !== 0);
    if ((cfg.pair_id || 0) !== 0) {
        $("input.remoteSummary.pairId").textContent = String(cfg.pair_id);
    }
    const f = cfg.freeze || 0;
    show($("input.remoteSummary.freeze"), f !== 0);
    if (f !== 0) {
        const parts = [];
        if (f & 1) parts.push(dictNow["tracks.colFreezePan"] || "冻结P");
        if (f & 2) parts.push(dictNow["tracks.colFreezeVol"] || "冻结V");
        $("input.remoteSummary.freezeText").textContent = parts.join("/");
    }
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
})();
