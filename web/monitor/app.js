// SPDX-License-Identifier: GPL-3.0-or-later
//
// web/monitor/app.js —— T45 占位页的最小接线。**不是 T46 的实现**。
//
// 为什么不用 web/shared/bridge.js:
//   bridge.js 的 BRIDGE_FUNCTIONS / BRIDGE_EVENTS 是**冻结名表**,逐字对应
//   docs/SCVB_CONTRACT.md §7 manifest 的 output / input 两侧,由 scripts/check-bridge-parity.mjs
//   把关。Monitor 的桥面要到 **T46** 才正式进契约(manifest.monitor),现在往名表里塞 "monitor"
//   等于偷改冻结契约。所以本占位页自己走一遍最小的 __JUCE__ 接线,**一个字节都不动 bridge.js**。
//   T46 立项时:契约加 manifest.monitor → bridge.js 加名表 → 本页改用 createBridge("monitor")。
//
// 桥面(真源 = src/monitor/MonitorBridgeApi.h):
//   函数 setObservedGroup(1..8) —— Monitor 唯一的写入口(只读换段,不 claim;
//   刻意不叫 setGroupId:契约 §1.4 的那个是 Output 的改组,语义完全不同)
//   事件 scvb.state / scvb.groups / scvb.viz

const GROUP_LABELS = ["A", "B", "C", "D", "E", "F", "G", "H"];

const el = (id) => document.querySelector(`[data-gb="${id}"]`);

/** 惰性取 JUCE 原生函数;无宿主(普通浏览器预览)返回 null,页面照常渲染空态。 */
async function nativeFunction(name) {
    const juce = globalThis.__JUCE__;
    if (!juce || !juce.backend) return null;
    const mod = await import("../js/juce/index.js");
    return mod.getNativeFunction(name);
}

function onEvent(name, cb) {
    const juce = globalThis.__JUCE__;
    if (!juce || !juce.backend) return;
    juce.backend.addEventListener(name, cb);
}

function fmtSamples(n, sr) {
    if (!Number.isFinite(n) || n < 0 || !Number.isFinite(sr) || sr <= 0)
        return "—";
    const s = n / sr;
    const m = Math.floor(s / 60);
    return `${m}:${(s - m * 60).toFixed(1).padStart(4, "0")}`;
}

function renderGroups(container, current, onlineMask, setObservedGroup) {
    container.replaceChildren();
    GROUP_LABELS.forEach((label, i) => {
        const g = i + 1;
        const b = document.createElement("button");
        b.className = "mon-group";
        b.type = "button";
        b.textContent = label;
        b.setAttribute("aria-pressed", String(g === current));
        // 组是否在线只影响提示,不禁用按钮 —— 用户有权切到一个还没上线的组并等它上线。
        b.title = (onlineMask >> i) & 1 ? "online" : "offline";
        b.addEventListener("click", () => setObservedGroup(g));
        container.append(b);
    });
}

function renderTracks(container, tracks) {
    container.replaceChildren();
    for (const t of tracks || []) {
        const row = document.createElement("div");
        row.className = "mon-track";
        row.dataset.covered = String(Boolean(t.hasSegments));

        const dot = document.createElement("span");
        dot.className = "mon-dot";
        dot.style.background = `rgb(var(--track-color-${t.color || t.ch}))`;
        row.append(dot);

        const pan = Number.isFinite(t.pan) ? t.pan.toFixed(1) : "—";
        row.append(
            document.createTextNode(
                `${String(t.ch).padStart(2, "0")} ${t.enabled ? "" : "(off)"} pan ${pan}`,
            ),
        );
        container.append(row);
    }
}

async function main() {
    const groupsBox = el("monitor.groups");
    const tracksBox = el("monitor.tracks");
    const vizLine = el("monitor.viz-state");
    const winLine = el("monitor.window");
    const headLine = el("monitor.playhead");

    let groupId = 1;
    let onlineMask = 0;

    const setObservedGroupFn = await nativeFunction("setObservedGroup");
    const setObservedGroup = async (g) => {
        if (!setObservedGroupFn) return;
        const r = await setObservedGroupFn(g);
        if (r && Number.isFinite(r.groupId)) groupId = r.groupId;
        renderGroups(groupsBox, groupId, onlineMask, setObservedGroup);
    };

    onEvent("scvb.state", (p) => {
        if (Number.isFinite(p.groupId)) groupId = p.groupId;
        vizLine.textContent = `viz: ${p.viz}${p.viz === "online" && !p.fresh ? " (stalled)" : ""}`;
        renderGroups(groupsBox, groupId, onlineMask, setObservedGroup);
    });

    onEvent("scvb.groups", (p) => {
        onlineMask = Number(p.online) || 0;
        renderGroups(groupsBox, groupId, onlineMask, setObservedGroup);
    });

    onEvent("scvb.viz", (p) => {
        const sr = Number(p.sampleRate) || 0;
        winLine.textContent = p.online
            ? `window: 0 – ${fmtSamples(p.windowSpan, sr)} (${p.columns} cols, v${p.versionActive})`
            : "window: —";
        headLine.textContent = p.online
            ? `playhead: ${fmtSamples(p.playhead, sr)}${p.playing ? " ▶" : ""}${p.looping ? " ↻" : ""}`
            : "playhead: —";
        renderTracks(tracksBox, p.tracks);
    });

    renderGroups(groupsBox, groupId, onlineMask, setObservedGroup);

    // §0.6:mBridgeReady 门控由页面掌握 —— 首次装载(与编辑器重建后)各调一次。
    const requestInitialState = await nativeFunction("requestInitialState");
    if (requestInitialState) {
        const snap = await requestInitialState();
        if (snap && Number.isFinite(snap.groupId)) groupId = snap.groupId;
        if (snap && Number.isFinite(snap.groupsOnline))
            onlineMask = snap.groupsOnline;
        if (snap && snap.viz) vizLine.textContent = `viz: ${snap.viz}`;
        renderGroups(groupsBox, groupId, onlineMask, setObservedGroup);
    }
}

main();
