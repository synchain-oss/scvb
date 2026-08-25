// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · 建议表视图 + CSV 导出(T41 交付物;11 §4.2.3 通路 B 的 B1/B2)
// -----------------------------------------------------------------------------
// 它是什么:把分析产出的**每轨每段** pan/vol/width 建议值列成一张表,并能导出 CSV,
// 让用户在 SCVB 不接管音频时照着在 DAW 里手工应用。三重来历见 07 T41:
// RD-01(Cubase 录不进自发参数变化)的兜底、S1 失败后 F2 兜底的核心组件、
// 以及它对手工混音者本身就有独立价值 —— U12 裁定不等 S1 结果,直接进 v1 主线。
//
// **它是建议,不是执行**:SCVB 不会替用户在 DAW 里应用这些值。这句话在视图顶部
// 常驻(词条 `suggest.disclaimer`),不许省 —— 否则用户会以为「导出即生效」(07 T41 强调栏)。
//
// 【落点:Tab3 内的第二视图,不是第五个 tab】
//   契约 §1.31 `setActiveTab` 的四值枚举(master/tracks/wave/settings)是**冻结面**,
//   加第五个 tab 要动契约;而本视图消费的正是 Tab3 的段数据(§2.8 `scvb.segments`),
//   与泳道同源。故做成 Tab3 的双视图切换 —— 形制照 [J75] T43 给 Tab1 加「分布 ↔ 轨迹」
//   的先例:`section[data-tab-panel="wave"][data-view="suggest"]` 换一套子树。
//   **视图态是纯本地 UI 态,不落 state**(与 T43 的 `ui.master_chart_mode` 不同):
//   它是查阅视图而非长期偏好,不值得为它再动一次冻结 state schema。本会话内切走 tab
//   再回来仍停在原视图,但**重开面板/重开工程一律回泳道档** —— 这是刻意的,不是遗漏
//   (同款处置:Tab3 那枚段检查器开关也只存本地态)。
//
// 【本视图是全项目唯一的表格式视图】[J67] 已废除「结果列表视图」(T33b 删卡),
//   逐段查看/编辑由段检查器 + 泳道点选承载。**不得**以「T41 都有表格了」为由复活分段列表。
//
// 【列定义冻结】(07 T41 / 11 §4.2.3 B2 逐字;顺序不可改、不可增删)
//   track_index, track_label, source_channels, version, version_name,
//   segment_index, t0_sec, t1_sec, pan, vol_db, width, origin, locked
//   `source_channels` 与 `width` 是 [J57] 随立体声进 v1 加的两列,**不可省** ——
//   不带 width 的建议对 stereo 轨不可执行。
//   本文件的 CSV_HEADER 与 C++ 侧 `src/core/export/SuggestionExport.h` 的 `kCsvHeader`
//   由 web-preview/tests/smoke-t41-suggestions.mjs 读源码逐字对拍(漂了当场红)。
//
// 【表格与 CSV 同源】`rowCells(row)` 产出 13 个**显示串**,表格单元格与 CSV 字段
//   都只由它出 —— 07 T41 验收「数值与 UI 显示值逐行相等(不是各算一遍)」是靠这条
//   结构保证的,不是靠两处凑同一个 round()。
//
// 【只看当前激活版本】§2.8 `scvb.segments` 一次只下发一个版本的段表,且 app.js 有
//   版本闸(非激活版本整帧丢弃);§2.2 `scvb.params` 同样只带当前激活版本的 60 个参数。
//   于是 UI 手里**就只有激活版本的真相** —— 表格与导出都按当前版本走,不假装能出全版本。
//   跨版本导出是 native 的事(桥函数 scope 里留了 `versions:"all"`,由 C++ 从 CRVS 真身出)。
//
// 【桥面处置】`exportSuggestions(scope)` **不在冻结契约 §7 manifest 里**。按仓 CLAUDE.md §5
//   写了变更文档 `docs/contract-changes/20260825-export-suggestions.md`,名字停在
//   `web/shared/bridge.js` 的 `PENDING_FUNCS` 里(T43 立的先例):
//     • mock/预览:后端已实现 → 全功能可演示;
//     • 真 JUCE 宿主(native 未落地):桥上根本不挂这个名字 → 导出钮 disabled 并给出说明,
//       **不写、也不假装写了**任何文件。
//
// 两段导出:纯函数(无 DOM,node 直接 import 断言)+ createTabSuggestions()(DOM 接线)。
// 模块顶层零副作用、零 document 触碰。
// =============================================================================

// =============================================================================
// 一、纯函数与常量(无 DOM;node 侧断言面)
// =============================================================================

/** 冻结表头(逐字 = C++ 侧 kCsvHeader)。 */
export const CSV_HEADER =
    "track_index,track_label,source_channels,version,version_name,segment_index,t0_sec,t1_sec,pan,vol_db,width,origin,locked";

/** CSV 行尾(CRLF)与 UTF-8 BOM —— 后者是 Excel 打开中文轨名不乱码的前提。 */
export const CSV_NEWLINE = "\r\n";
export const CSV_BOM = "\uFEFF"; // 写转义,不写字面 BOM —— 源码里它是个看不见的字符

/**
 * 13 列的显示定义。`key` = 冻结列名(同时是 CSV 字段名),`t` = 列头词条,
 * `num` = 右对齐的数值列,`w` = 网格列宽(px;轨名与版本名给弹性列)。
 * **顺序即冻结顺序**,改这张表 = 改列定义 = 改冻结口径。
 */
export const SUGGEST_COLUMNS = Object.freeze(
    [
        { key: "track_index", t: "suggest.col.trackIndex", num: 1, w: "52px" },
        {
            key: "track_label",
            t: "suggest.col.trackLabel",
            num: 0,
            w: "minmax(96px, 1.4fr)",
        },
        {
            key: "source_channels",
            t: "suggest.col.sourceChannels",
            num: 1,
            w: "62px",
        },
        { key: "version", t: "suggest.col.version", num: 1, w: "52px" },
        {
            key: "version_name",
            t: "suggest.col.versionName",
            num: 0,
            w: "minmax(72px, 1fr)",
        },
        {
            key: "segment_index",
            t: "suggest.col.segmentIndex",
            num: 1,
            w: "56px",
        },
        { key: "t0_sec", t: "suggest.col.t0Sec", num: 1, w: "74px" },
        { key: "t1_sec", t: "suggest.col.t1Sec", num: 1, w: "74px" },
        { key: "pan", t: "suggest.col.pan", num: 1, w: "62px" },
        { key: "vol_db", t: "suggest.col.volDb", num: 1, w: "66px" },
        { key: "width", t: "suggest.col.width", num: 1, w: "62px" },
        { key: "origin", t: "suggest.col.origin", num: 0, w: "94px" },
        { key: "locked", t: "suggest.col.locked", num: 0, w: "60px" },
    ].map(Object.freeze),
);

/**
 * 小数位数 —— 与 UI 其它处的显示档一致,也与 C++ 侧 kDecimals* 一致:
 * 秒 3 位(= 05 §2.3a 的 mm:ss.mmm 同精度)、pan/vol/width 各 1 位。
 */
export const DECIMALS = Object.freeze({ sec: 3, pan: 1, vol: 1, width: 1 });

/** 虚拟滚动的行高(px)与上下预渲染余量(行)。 */
export const ROW_H = 22;
export const OVERSCAN = 6;

function num(v, dflt = 0) {
    return typeof v === "number" && Number.isFinite(v) ? v : dflt;
}

/**
 * 定点格式化:`decimals` 位小数,**负零归一成正零**。
 * 不归一的话同一个零会在不同轨上写出 "0.0" 与 "-0.0" 两种字面值,导表软件与 diff 都当两个值。
 */
export function fmtFixed(v, decimals) {
    const d = Math.max(0, Math.min(9, num(decimals, 0) | 0));
    const x = num(v, 0);
    const s = x.toFixed(d);
    return /^-0(\.0*)?$/.test(s) ? s.slice(1) : s;
}

/**
 * RFC 4180 字段转义:含 `,` `"` CR LF 的字段整体加双引号,内部双引号翻倍;否则原样。
 * 轨名是**用户数据**,「主唱, 双轨」这种名字不转义就会把一行撑成 14 个字段。
 */
export function csvField(raw) {
    const s = raw === null || raw === undefined ? "" : String(raw);
    if (!/[",\r\n]/.test(s)) return s;
    return '"' + s.replace(/"/g, '""') + '"';
}

/** origin 三值(与 T19 state 编码逐字一致);未知值一律回落 auto。 */
export function originOf(raw) {
    return raw === "user_edited" || raw === "user_created" ? raw : "auto";
}

/**
 * 当前激活版本号与显示名(§2.1 `global.version_active` + `versions[].name`,[J05] 空名回落 V{n})。
 * 单拎出来是因为**空表时也要显示版本名** —— 从行集里读的话,零行就没有名字可读了。
 */
export function activeVersionOf(store) {
    const st = (store && store.state) || {};
    const version = Math.min(
        2,
        Math.max(1, num((st.global || {}).version_active, 1) | 0),
    );
    const vmeta =
        (Array.isArray(st.versions) ? st.versions : [])[version - 1] || {};
    const name =
        typeof vmeta.name === "string" && vmeta.name !== ""
            ? vmeta.name
            : "V" + version;
    return { version, versionName: name };
}

/**
 * 事件仓 → 建议表行集(只读投影,当前激活版本)。
 *   version / versionName ← §2.1 state.global.version_active + state.versions[]
 *   track_label / source_channels ← §2.1 state.channels[](source_channels 是只读检测值)
 *   段 ← §2.8 store.segments(app.js 的版本闸保证它就是激活版本的段表)
 *   width ← §2.2 store.params.values[`v{v}_t{tt}_width`](同样只有激活版本)
 * 行序 = 轨升序 → 段升序(与 C++ 侧「版本 → 轨 → 段」的字典序在单版本下同序)。
 */
export function buildSuggestionRows(store) {
    const st = (store && store.state) || {};
    const { version, versionName } = activeVersionOf(store);

    const channels = Array.isArray(st.channels) ? st.channels : [];
    const params = ((store && store.params) || {}).values || {};
    const byCh = new Map();
    for (const c of ((store && store.segments) || {}).channels || []) {
        if (c && Number.isInteger(c.ch)) byCh.set(c.ch, c);
    }

    const rows = [];
    for (let ch = 1; ch <= 15; ch++) {
        const entry = byCh.get(ch);
        const segs = (entry && entry.segments) || [];
        if (segs.length === 0) continue;

        const cfg = channels[ch - 1] || {};
        const stereo = num(cfg.source_channels, 1) === 2;
        const label = typeof cfg.label === "string" ? cfg.label : "";
        // mono 轨的 width 参数是 v1 no-op 占位(params v2.0):列留空,**不写 0** ——
        // 0 在 stereo 轨上是「收成 mono」的有效建议([J57]),两者不可混。
        const widthId =
            "v" + version + "_t" + String(ch).padStart(2, "0") + "_width";

        for (let i = 0; i < segs.length; i++) {
            const seg = segs[i] || {};
            rows.push({
                trackIndex: ch,
                trackLabel: label,
                sourceChannels: stereo ? 2 : 1,
                version,
                versionName,
                segmentIndex: Number.isInteger(seg.segIdx) ? seg.segIdx : i,
                t0Sec: num(seg.t0S, 0),
                t1Sec: num(seg.t1S, 0),
                pan: num(seg.pan, 0),
                volDb: num(seg.volDb, 0),
                hasWidth: stereo,
                width: stereo ? num(params[widthId], 100) : 0,
                origin: originOf(seg.origin),
                locked: !!seg.locked,
            });
        }
    }
    return rows;
}

/**
 * 一行 → 13 个显示串(顺序 = SUGGEST_COLUMNS)。
 * **表格单元格与 CSV 字段的唯一产地**,见文件头【表格与 CSV 同源】。
 */
export function rowCells(row) {
    const r = row || {};
    return [
        String(num(r.trackIndex, 0)),
        typeof r.trackLabel === "string" ? r.trackLabel : "",
        String(num(r.sourceChannels, 1)),
        String(num(r.version, 1)),
        typeof r.versionName === "string" ? r.versionName : "",
        String(num(r.segmentIndex, 0)),
        fmtFixed(r.t0Sec, DECIMALS.sec),
        fmtFixed(r.t1Sec, DECIMALS.sec),
        fmtFixed(r.pan, DECIMALS.pan),
        fmtFixed(r.volDb, DECIMALS.vol),
        r.hasWidth ? fmtFixed(r.width, DECIMALS.width) : "", // mono:空,不是 0
        originOf(r.origin),
        r.locked ? "true" : "false",
    ];
}

/** 行集 → CSV 文本:BOM + 表头 + 每行,行尾一律 CRLF(含最后一行)。 */
export function toCsv(rows) {
    let out = CSV_BOM + CSV_HEADER + CSV_NEWLINE;
    for (const row of rows || []) {
        out += rowCells(row).map(csvField).join(",") + CSV_NEWLINE;
    }
    return out;
}

/**
 * 虚拟滚动窗口:1200 行 × 13 列一次性建 DOM 会把切视图卡成一拍,故只建可见行 ± OVERSCAN。
 * 返回 `{start, end, padTop}`:渲染 rows[start, end),整块下移 padTop px。
 */
export function visibleWindow(
    scrollTop,
    viewportH,
    total,
    rowH = ROW_H,
    overscan = OVERSCAN,
) {
    const h = Math.max(1, num(rowH, ROW_H));
    const n = Math.max(0, num(total, 0) | 0);
    const first = Math.max(0, Math.floor(num(scrollTop, 0) / h) - overscan);
    const count =
        Math.ceil(Math.max(0, num(viewportH, 0)) / h) + overscan * 2 + 1;
    const start = Math.min(first, n);
    const end = Math.min(n, start + count);
    return { start, end, padTop: start * h };
}

/** 导出文件名:`SCVB-suggestions-<版本名>-<YYYYMMDD-HHmm>.csv`(版本名里的路径字符换 `_`)。 */
export function suggestFileName(versionName, when) {
    const d = when instanceof Date ? when : new Date();
    const p2 = (n) => String(n).padStart(2, "0");
    const stamp =
        d.getFullYear() +
        p2(d.getMonth() + 1) +
        p2(d.getDate()) +
        "-" +
        p2(d.getHours()) +
        p2(d.getMinutes());
    const safe = String(versionName || "V1")
        .replace(/[\\/:*?"<>|\s]+/g, "_")
        .slice(0, 24);
    return "SCVB-suggestions-" + safe + "-" + stamp + ".csv";
}

/** 桥面 scope(契约变更文档 §「参数」):v1 UI 只导当前版本 + 全部 15 轨。 */
export function currentScope() {
    return { versions: "active", tracksMask: 0x7fff };
}

// =============================================================================
// 二、DOM 接线
// =============================================================================

/**
 * @param {object} opts
 *   root            —— Tab3 的 `section[data-tab-panel="wave"]`
 *   getStore()      —— 事件仓
 *   getT()          —— 当前语言词典
 *   bridge          —— createBridge() 的产物(可无 exportSuggestions)
 *   onViewChange(v) —— 视图切换回调("lanes" | "suggest"),供外壳补一次 render
 */
export function createTabSuggestions(opts) {
    const o = opts || {};
    const root = o.root;
    const getStore = o.getStore || (() => ({}));
    const getT = o.getT || (() => ({}));
    const bridge = o.bridge || null;
    const onViewChange = o.onViewChange || (() => {});

    const $ = (gb) =>
        root ? root.querySelector('[data-gb="' + gb + '"]') : null;

    const el = {
        view: $("suggest-view"),
        entry: $("wave-btn-suggest"),
        close: $("suggest-close"),
        exportBtn: $("suggest-export"),
        scope: $("suggest-scope"),
        status: $("suggest-status"),
        thead: $("suggest-thead"),
        scroll: $("suggest-scroll"),
        spacer: $("suggest-spacer"),
        rowsBox: $("suggest-rows"),
        empty: $("suggest-empty"),
        table: $("suggest-table"),
        legend: $("suggest-legend"),
    };

    const local = {
        open: false,
        rows: [],
        rendered: { start: -1, end: -1 },
        busy: false,
        status: null, // {key, args} —— 一次性反馈,切视图即清
    };

    function t(key, fallback) {
        const dict = getT() || {};
        const v = dict[key];
        return typeof v === "string" && v !== ""
            ? v
            : fallback !== undefined
              ? fallback
              : key;
    }

    /** `{n}` 式占位替换(与页面其余处同款,不引模板引擎)。 */
    function fill(str, args) {
        let s = String(str);
        for (const k of Object.keys(args || {})) {
            s = s.split("{" + k + "}").join(String(args[k]));
        }
        return s;
    }

    async function call(name, ...args) {
        if (!bridge || typeof bridge[name] !== "function") return null;
        try {
            return await bridge[name](...args);
        } catch (e) {
            console.warn(
                "SCVB 建议表:bridge." + name + "() 调用失败 —— " + e.message,
            );
            return null;
        }
    }

    function exportAvailable() {
        return !!(bridge && typeof bridge.exportSuggestions === "function");
    }

    // ------------------------------------------------------------- 列头(一次性)
    function mountHead() {
        if (!el.thead || el.thead.childElementCount > 0) return;
        // 网格列模板同时给表头与数据行,两者才不会各排各的
        const template = SUGGEST_COLUMNS.map((c) => c.w).join(" ");
        if (el.table) el.table.style.setProperty("--suggest-cols", template);
        for (const col of SUGGEST_COLUMNS) {
            const cell = document.createElement("span");
            cell.className =
                "suggest-cell suggest-cell--head" +
                (col.num ? " suggest-cell--num" : "");
            cell.setAttribute("data-t", col.t);
            // title 挂冻结列名:表头是本地化的,而 CSV 里是这一串 —— 悬停即可对上
            cell.setAttribute("title", col.key);
            el.thead.appendChild(cell);
        }
    }

    // ------------------------------------------------------------- 渲染
    function renderRows(force) {
        if (!el.scroll || !el.rowsBox || !el.spacer) return;
        const total = local.rows.length;
        el.spacer.style.height = total * ROW_H + "px";

        const w = visibleWindow(
            el.scroll.scrollTop,
            el.scroll.clientHeight,
            total,
        );
        if (
            !force &&
            w.start === local.rendered.start &&
            w.end === local.rendered.end
        )
            return;
        local.rendered = { start: w.start, end: w.end };

        const frag = document.createDocumentFragment();
        for (let i = w.start; i < w.end; i++) {
            const row = local.rows[i];
            const line = document.createElement("div");
            line.className = "suggest-row";
            line.setAttribute("role", "row");
            if (row.origin !== "auto") line.setAttribute("data-manual", "1");
            const cells = rowCells(row);
            for (let c = 0; c < cells.length; c++) {
                const span = document.createElement("span");
                span.className =
                    "suggest-cell" +
                    (SUGGEST_COLUMNS[c].num ? " suggest-cell--num" : "");
                span.setAttribute("role", "gridcell");
                span.textContent = cells[c];
                line.appendChild(span);
            }
            frag.appendChild(line);
        }
        el.rowsBox.replaceChildren(frag);
        el.rowsBox.style.transform = "translateY(" + w.padTop + "px)";
    }

    function renderChrome() {
        const total = local.rows.length;
        const tracks = new Set(local.rows.map((r) => r.trackIndex)).size;
        // 版本名从 state 读而不是从行里读:空表时也要说清「你看的是哪个版本的建议」
        const { versionName } = activeVersionOf(getStore());

        if (el.scope) {
            el.scope.textContent = fill(
                t("suggest.scope", "{v} · {t} 轨 · {n} 行"),
                {
                    v: versionName,
                    t: tracks,
                    n: total,
                },
            );
        }
        if (el.empty) el.empty.hidden = total > 0;
        if (el.table) el.table.hidden = total === 0;
        // 图例讲的是那几列怎么读 —— 表都不在了就别挂着它占位
        if (el.legend) el.legend.hidden = total === 0;

        if (el.exportBtn) {
            const off = total === 0 || local.busy || !exportAvailable();
            el.exportBtn.setAttribute("data-disabled", off ? "1" : "0");
            el.exportBtn.setAttribute("aria-disabled", off ? "true" : "false");
            el.exportBtn.setAttribute(
                "title",
                exportAvailable()
                    ? ""
                    : t(
                          "suggest.exportUnavailable",
                          "导出待 native 侧接线(见 docs/contract-changes/20260825-export-suggestions.md)",
                      ),
            );
        }
        if (el.status) {
            const s = local.status;
            el.status.textContent = s
                ? fill(t(s.key, s.key), s.args || {})
                : "";
            el.status.hidden = !s;
            el.status.setAttribute("data-tone", (s && s.tone) || "info");
        }
    }

    function render() {
        if (!local.open) return;
        local.rows = buildSuggestionRows(getStore());
        renderChrome();
        renderRows(true);
    }

    // ------------------------------------------------------------- 视图切换
    function setView(view) {
        const next = view === "suggest" ? "suggest" : "lanes";
        if (local.open === (next === "suggest")) return;
        local.open = next === "suggest";
        local.status = null;
        if (root) root.setAttribute("data-view", next);
        if (el.view) el.view.hidden = !local.open;
        if (el.entry) el.entry.setAttribute("aria-pressed", String(local.open));
        if (local.open) {
            mountHead();
            local.rendered = { start: -1, end: -1 };
            if (el.scroll) el.scroll.scrollTop = 0;
            render();
            // 列头词条随视图首次挂载才存在,补一次刷字(applyI18n 由外壳在切语言时统管)
            if (typeof o.applyI18n === "function") o.applyI18n(el.view);
            if (el.close && typeof el.close.focus === "function")
                el.close.focus();
        }
        onViewChange(next);
    }

    // ------------------------------------------------------------- 导出
    async function doExport() {
        if (local.busy || local.rows.length === 0) return;
        if (!exportAvailable()) {
            local.status = { key: "suggest.exportUnavailable", tone: "warn" };
            renderChrome();
            return;
        }
        local.busy = true;
        local.status = { key: "suggest.exporting", tone: "info" };
        renderChrome();

        const res = await call("exportSuggestions", currentScope());
        local.busy = false;

        if (res && res.ok) {
            local.status = {
                key: "suggest.exportOk",
                tone: "ok",
                args: {
                    n: num(res.rows, local.rows.length),
                    path: res.path || "",
                },
            };
            // 预览专用:把同一份行集另存一份到浏览器下载,好让人真打开文件看 BOM/CRLF/转义。
            // 真 JUCE 宿主里 native 已经落盘了,这一步不跑(也跑不动 —— WebView 拦下载)。
            if (bridge && bridge.isPreview) downloadForPreview();
        } else if (res && res.reason === "cancelled") {
            local.status = { key: "suggest.exportCancelled", tone: "info" };
        } else {
            local.status = {
                key: "suggest.exportFail",
                tone: "warn",
                args: { reason: (res && res.reason) || "unknown" },
            };
        }
        renderChrome();
    }

    function downloadForPreview() {
        try {
            const blob = new Blob([toCsv(local.rows)], {
                type: "text/csv;charset=utf-8",
            });
            const url = URL.createObjectURL(blob);
            const a = document.createElement("a");
            a.href = url;
            a.download = suggestFileName(
                local.rows[0] && local.rows[0].versionName,
            );
            a.click();
            // revoke 要等点击那一拍走完,立刻撤会让下载拿到一个已释放的 URL
            setTimeout(() => URL.revokeObjectURL(url), 4000);
        } catch (e) {
            console.warn("SCVB 建议表:预览下载失败 —— " + e.message);
        }
    }

    // ------------------------------------------------------------- 接线
    const offs = [];
    function on(node, type, fn, opt) {
        if (!node) return;
        node.addEventListener(type, fn, opt);
        offs.push(() => node.removeEventListener(type, fn, opt));
    }

    on(el.entry, "click", () => setView("suggest"));
    on(el.close, "click", () => setView("lanes"));
    on(el.exportBtn, "click", () => {
        if (el.exportBtn.getAttribute("data-disabled") === "1") return;
        void doExport();
    });
    on(el.scroll, "scroll", () => renderRows(false), { passive: true });
    on(el.view, "keydown", (e) => {
        if (e.key === "Escape") {
            e.stopPropagation();
            setView("lanes");
        }
    });

    if (el.view) el.view.hidden = true;
    if (root) root.setAttribute("data-view", "lanes");

    return {
        isOpen: () => local.open,
        open: () => setView("suggest"),
        close: () => setView("lanes"),
        render,
        /** 段/参数/state 变化后由外壳调一次(只在打开时才真的重建行集)。 */
        onStoreChange: render,
        rowCount: () => local.rows.length,
        destroy() {
            for (const off of offs) off();
            offs.length = 0;
        },
    };
}
