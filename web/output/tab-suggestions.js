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

/** 轨数与版本数([J59]:15 轨 / 2 版本;与 C++ 侧 `state::kNumTracks` / `kNumVersions` 同值)。 */
export const TRACK_COUNT = 15;
export const VERSION_COUNT = 2;

/** 全轨掩码:bit0=轨1 … bit14=轨15,**bit15 保留 0**(契约 §9.2)。 */
export const ALL_TRACKS_MASK = (1 << TRACK_COUNT) - 1;

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
        VERSION_COUNT,
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
    for (let ch = 1; ch <= TRACK_COUNT; ch++) {
        const entry = byCh.get(ch);
        const segs = (entry && entry.segments) || [];
        if (segs.length === 0) continue;

        const cfg = channels[ch - 1] || {};
        const stereo = num(cfg.source_channels, 1) === 2;
        const label = typeof cfg.label === "string" ? cfg.label : "";
        // mono 轨的 width 参数是 v1 no-op 占位(params v2.0):列留空,**不写 0** ——
        // 0 在 stereo 轨上是「收成 mono」的有效建议([J57]),两者不可混。
        // 参数还没到(首帧 scvb.params 之前)时**同样留空**,不拿默认值 100 顶上:
        // 猜一个数写进建议表,用户会照着它在 DAW 里设值。与 C++ 侧的 kWidthUnknown 同口径。
        const rawWidth =
            params[
                "v" + version + "_t" + String(ch).padStart(2, "0") + "_width"
            ];
        const hasWidth =
            stereo && typeof rawWidth === "number" && Number.isFinite(rawWidth);

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
                hasWidth,
                width: hasWidth ? rawWidth : 0,
                origin: originOf(seg.origin),
                locked: !!seg.locked,
                // 13 列已冻结,过期标记只能落在行属性上(见 §2.8 每轨 stale)
                stale: !!(entry && entry.stale),
            });
        }
    }
    return rows;
}

/**
 * 行集的输入签名 —— 只取本视图真读的那几样:
 *   state / segments 是**换对象**语义(app.js 深合并后整体替换),引用即签名;
 *   params 每帧换对象但内容常没动,故只把当前版本那 15 个 width 参数拼成一串。
 * 15 次查表远比重建 600 个行对象便宜,这就是脏检查划算的地方。
 */
export function suggestionsSignature(store) {
    const st = store || {};
    const { version } = activeVersionOf(st);
    const values = (st.params || {}).values || {};
    let widths = "";
    for (let ch = 1; ch <= TRACK_COUNT; ch++) {
        const v =
            values[
                "v" + version + "_t" + String(ch).padStart(2, "0") + "_width"
            ];
        widths += (typeof v === "number" ? v : "") + ",";
    }
    return { state: st.state, segments: st.segments, widths };
}

/** 签名比对;`prev` 缺失(首次)一律算脏。 */
export function suggestionsDirty(store, prev) {
    if (!prev) return true;
    const next = suggestionsSignature(store);
    return (
        next.state !== prev.state ||
        next.segments !== prev.segments ||
        next.widths !== prev.widths
    );
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
    return { versions: "active", tracksMask: ALL_TRACKS_MASK };
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
        strut: $("suggest-strut"),
        stale: $("suggest-stale"),
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
        src: null, // 行集输入签名(脏检查,见 render)
        pool: [], // 可复用的行节点(虚拟滚动,见 rowNodeAt)
        scrollRaf: 0,
        trackCount: 0, // 行集里出现过的轨数(随行集算一次,renderChrome 只读)
        staleTracks: 0, // 其中采集数据过期的轨数(§2.8 stale)
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
        SUGGEST_COLUMNS.forEach((col, c) => {
            const cell = document.createElement("span");
            cell.className =
                "suggest-cell suggest-cell--head" +
                (col.num ? " suggest-cell--num" : "");
            cell.setAttribute("role", "columnheader");
            cell.setAttribute("aria-colindex", String(c + 1));
            cell.setAttribute("data-t", col.t);
            // title 挂冻结列名:表头是本地化的,而 CSV 里是这一串 —— 悬停即可对上
            cell.setAttribute("title", col.key);
            el.thead.appendChild(cell);
        });
    }

    /**
     * 取一行的 DOM(不够就新建)。虚拟滚动**复用节点**:每滚一行就整窗
     * `replaceChildren` 掉几百个节点,是与「每帧重建行集」同一类的浪费,只是触发源
     * 换成了滚动。池子大小 = 一个可见窗口,滚动期只改 textContent 与几个属性。
     */
    function rowNodeAt(slot) {
        let line = local.pool[slot];
        if (line) return line;
        line = document.createElement("div");
        line.className = "suggest-row";
        line.setAttribute("role", "row");
        for (let c = 0; c < SUGGEST_COLUMNS.length; c++) {
            const span = document.createElement("span");
            span.className =
                "suggest-cell" +
                (SUGGEST_COLUMNS[c].num ? " suggest-cell--num" : "");
            span.setAttribute("role", "cell");
            span.setAttribute("aria-colindex", String(c + 1));
            line.appendChild(span);
        }
        local.pool[slot] = line;
        return line;
    }

    /** 三个行级标记只有「有/无」两态,统一走这一个口子避免各处 set/remove 写岔。 */
    function flag(node, name, on) {
        if (on) node.setAttribute(name, "1");
        else node.removeAttribute(name);
    }

    // ------------------------------------------------------------- 渲染
    // 虚拟滚动:strut 撑出「总行数 × 行高」的滚动高度,可见窗口那几十行**绝对定位**到各自
    // 的 top。行是 rowgroup(滚动容器)的直接子节点 —— 中间不夹容器,ARIA 的 grid→rowgroup→row
    // 这条树才是连着的(夹一层 div 就断了,读屏软件会念不出行列关系)。
    function renderRows(force) {
        if (!el.scroll || !el.strut) return;
        const total = local.rows.length;
        el.strut.style.height = total * ROW_H + "px";

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
        const wasCount = local.rendered.end - local.rendered.start;
        local.rendered = { start: w.start, end: w.end };
        const count = w.end - w.start;

        // 就地更新复用节点:只改 textContent 与几个属性,不动 DOM 结构
        for (let slot = 0; slot < count; slot++) {
            const i = w.start + slot;
            const row = local.rows[i];
            const line = rowNodeAt(slot);
            // 表头占第 1 行,数据行从 2 起(aria-rowcount 在 renderChrome 里同口径)
            line.setAttribute("aria-rowindex", String(i + 2));
            line.style.top = w.padTop + slot * ROW_H + "px";
            // 斑马纹按**行号**取,不用 :nth-child —— 复用节点时子节点序号是不动的,
            // 按序号上色会让整片底色随滚动黑白翻转。
            flag(line, "data-odd", i % 2 === 1);
            flag(line, "data-manual", row.origin !== "auto");
            // §2.8 每轨 stale = 该轨有过期采集区间。13 列已冻结不便加列,故落在行属性上:
            // 照着一张过期的表在 DAW 里手工设值是本视图**唯一**能骗到用户的地方。
            flag(line, "data-stale", !!row.stale);
            const cells = rowCells(row);
            for (let c = 0; c < cells.length; c++) {
                const span = line.children[c];
                if (span.textContent !== cells[c]) span.textContent = cells[c];
            }
        }

        // 结构只在「可见行数变了」时动一次(换视口、行集从空变满、滚到末尾那一屏)
        if (count !== wasCount || el.scroll.childElementCount !== count + 1) {
            el.scroll.replaceChildren(el.strut, ...local.pool.slice(0, count));
        }
    }

    /**
     * 滚动只标脏,真重排合到下一帧 —— 浏览器一次连续滚动会同步派发一串 scroll,
     * 每条都进重排的话高频输入设备上会把主线程占满(这是「脏检查治好了播放期,
     * 滚动期换了个触发源」的那一半)。
     */
    function scheduleRows() {
        if (local.scrollRaf || typeof requestAnimationFrame !== "function") {
            if (!local.scrollRaf) renderRows(false);
            return;
        }
        local.scrollRaf = requestAnimationFrame(() => {
            local.scrollRaf = 0;
            if (local.open) renderRows(false);
        });
    }

    /**
     * 只在真变了时写 `textContent`。
     * **不是微优化**:`.suggest-status` 与 `.suggest-stale` 都是 `role="status"
     * aria-live="polite"`,而 textContent 的 setter 一律新建文本节点 —— 即使字串逐字
     * 相同也是一次真实 DOM 变更,atomic live region 会照播。renderChrome() 每帧都跑
     * (它读 local.busy / local.status / exportAvailable(),这三样不在脏检查签名里),
     * 于是播放期读屏软件会被同一句话每秒轰 30 次。`renderRows` 里的单元格已经这么写了。
     */
    function setText(node, next) {
        if (node && node.textContent !== next) node.textContent = next;
    }

    /** 同上,属性侧。renderChrome 每帧都跑,属性写入同样是每帧的真实 DOM 变更。 */
    function setAttr(node, name, next) {
        const v = String(next);
        if (node && node.getAttribute(name) !== v) node.setAttribute(name, v);
    }

    function renderChrome() {
        const total = local.rows.length;
        // 版本名从 state 读而不是从行里读:空表时也要说清「你看的是哪个版本的建议」
        const { versionName } = activeVersionOf(getStore());

        if (el.scope) {
            setText(
                el.scope,
                fill(t("suggest.scope", "{v} · {t} 轨 · {n} 行"), {
                    v: versionName,
                    t: local.trackCount,
                    n: total,
                }),
            );
        }
        if (el.empty) el.empty.hidden = total > 0;
        if (el.table) el.table.hidden = total === 0;
        // 图例讲的是那几列怎么读 —— 表都不在了就别挂着它占位
        if (el.legend) el.legend.hidden = total === 0;
        if (el.table) {
            setAttr(el.table, "aria-rowcount", total + 1); // +1 = 表头行
            setAttr(el.table, "aria-colcount", SUGGEST_COLUMNS.length);
        }

        // 过期采集提示:照着一张过期的表在 DAW 里设值,是本视图唯一会误导用户的场景。
        // 计数在 render() 的脏块里算(两趟 O(行数) 扫描 + 两个 Set,600 行时不该每帧跑)。
        if (el.stale) {
            el.stale.hidden = local.staleTracks === 0;
            if (local.staleTracks > 0) {
                setText(
                    el.stale,
                    fill(
                        t(
                            "suggest.staleNote",
                            "其中 {n} 条轨的采集数据已过期——建议先重新采集再照表设值。",
                        ),
                        { n: local.staleTracks },
                    ),
                );
            }
        }

        if (el.exportBtn) {
            const off = total === 0 || local.busy || !exportAvailable();
            setAttr(el.exportBtn, "data-disabled", off ? "1" : "0");
            setAttr(el.exportBtn, "aria-disabled", off ? "true" : "false");
            setAttr(
                el.exportBtn,
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
            // 桥上没挂 exportSuggestions(真 JUCE 宿主,native 未落地)时**常驻**说明:
            // 只挂在 title 上的话,用户点了一枚灰钮却什么也没发生,零反馈。
            const s =
                local.status ||
                (total > 0 && !exportAvailable()
                    ? { key: "suggest.exportUnavailable", tone: "warn" }
                    : null);
            setText(el.status, s ? fill(t(s.key, s.key), s.args || {}) : "");
            el.status.hidden = !s;
            setAttr(el.status, "data-tone", (s && s.tone) || "info");
        }
    }

    function render() {
        if (!local.open) return;
        // 脏检查:render() 由外壳的每帧合帧驱动,而播放中 30Hz playhead / 25Hz params 都会
        // 排 render —— 无条件重建 600 个行对象 + 几百个节点是白烧帧(与 T33 的按需投影同一纪律)。
        // 三个输入里 state/segments 是**换对象**语义(app.js 深合并后整体替换),引用比对即可;
        // params 每帧换对象但内容常常没动,故只对本视图真正读的那 15 个 width 参数取签名。
        const st = getStore() || {};
        const dirty = suggestionsDirty(st, local.src);
        if (dirty) {
            local.src = suggestionsSignature(st);
            local.rows = buildSuggestionRows(st);
            local.rendered = { start: -1, end: -1 }; // 行集换了,可见行必须重建
            // 两个计数只随行集变 —— 放这里算,renderChrome 只读。
            // (renderChrome 本身不能整块被 dirty 挡掉:local.busy / local.status /
            //  exportAvailable() 都不在签名里,它们变了也得重画。)
            const seen = new Set();
            const staleSeen = new Set();
            for (const r of local.rows) {
                seen.add(r.trackIndex);
                if (r.stale) staleSeen.add(r.trackIndex);
            }
            local.trackCount = seen.size;
            local.staleTracks = staleSeen.size;
        }
        renderChrome();
        renderRows(dirty);
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
        } else if (el.entry && typeof el.entry.focus === "function") {
            // 还焦到入口钮:不还的话焦点掉回 <body>,键盘用户从头 Tab 一遍才回得来
            el.entry.focus();
        }
        onViewChange(next);
    }

    // ------------------------------------------------------------- 导出
    // 三条前置都由 renderChrome 反映在钮的 disabled 与状态行上(桥不可用那一档是**常驻**
    // 说明,不靠点一下才出现),这里只是把它们再挡一道 —— 不在这里另写一套解释文案。
    async function doExport() {
        if (local.busy || local.rows.length === 0 || !exportAvailable()) return;
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
    on(el.scroll, "scroll", scheduleRows, { passive: true });
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
            if (local.scrollRaf && typeof cancelAnimationFrame === "function") {
                cancelAnimationFrame(local.scrollRaf);
            }
            local.scrollRaf = 0;
            // 清池子的同时把节点从 DOM 摘掉 —— 只清数组的话上一屏还挂在滚动容器里
            if (el.scroll) el.scroll.replaceChildren(el.strut || "");
            local.pool.length = 0;
        },
    };
}
