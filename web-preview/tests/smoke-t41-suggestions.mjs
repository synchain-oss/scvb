// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 建议表 + CSV 导出冒烟(node,无 DOM;T41 / U12)
// =============================================================================
// 与既有冒烟同款口径:断言面是**纯函数 + mock 端到端 + 源码不变式**,DOM 投影与
// 滚动手感归浏览器手测(shot.mjs 截图 + 真机 preview)。
//
// 跑什么(逐条对着 07 T41 的验收行):
//   ① 列定义冻结:13 列、顺序逐字;**web 的 CSV_HEADER 与 C++ 的 kCsvHeader 逐字相等**、
//      四个小数位常量同档、**跨语言格式化黄金表**(真源在 C++ 测试里,这里读源码跑 JS)——
//      两侧的舍入底座不同(printf 就近偶数 / toFixed 平局取大),光对拍表头盖不住数值这一层;
//   ② 文件形制:UTF-8 BOM + CRLF(含末行)+ 表头行;
//   ③ RFC 4180:轨名含逗号 / 引号 / 换行时该行仍是 13 个字段;
//   ④ 规模:15 轨 × 2 版本 × 每轨 40 段的 fixture 下,行数 == 段总数(单版本 600,
//      两版本合计 1200),且 `origin`/`locked` 取值与 T19 state 编码逐字一致;
//   ⑤ width:stereo 有值、mono **留空**、参数未到也**留空**(都不写 0/100 ——
//      0 是「收成 mono」的有效值 [J57],猜一个数用户会照着它去 DAW 里设);
//   ⑥ 表格与 CSV **同源**:CSV 的每个字段就是 rowCells() 那 13 个显示串(转义后),
//      这条就是「数值与 UI 显示值逐行相等,不是各算一遍」的机器化;
//   ⑦ mock 端到端:`exportSuggestions` 往返 —— mock **自己**从 state 数行数,
//      与 web 从事件仓算出的行数对上;badArg / noData / versions:"all" 三条拒绝与分支;
//   ⑧ 虚拟滚动窗口的边界(空表 / 顶部 / 底部 / 超长滚动)+ 行高常量与 CSS 同值;
//   ⑧b 脏检查:同一份输入不重建行集;state/segments 换对象与 width 真改才算脏;
//   ⑧c stale:过期采集的轨逐行可辨,且**不混进** CSV(13 列冻结);
//   ⑨ 词条:`suggest.*` 三语齐备、非空、占位符一致、禁词零命中;
//   ⑩ 源码不变式:桥面名字停在 PENDING_FUNCS(没有偷偷进 BRIDGE_FUNCTIONS)、变更文档在库、
//      ARIA table 树连着、斑马纹按行号取、行节点走池子复用 + scroll 合帧、
//      表头与数据行的 9px 滚动条错位两条配对修复都在、桥不可用时状态行常驻说明、
//      返回泳道还焦、建议表开着时泳道 rAF 停、[J67] 的「不得复活列表视图」纪律仍在文件头。
//
// 用法:node web-preview/tests/smoke-t41-suggestions.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有断言失败(逐条打印 [FAIL])。
// =============================================================================

import { pathToFileURL, fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";
import { readFileSync, existsSync } from "node:fs";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const u = (p) => pathToFileURL(join(ROOT, p)).href;
const src = (p) => readFileSync(join(ROOT, p), "utf8");

const TS = await import(u("web/output/tab-suggestions.js"));
const BR = await import(u("web/shared/bridge.js"));
const { T } = await import(u("web/shared/i18n.js"));
const driver = await import(u("web-preview/mock/state-driver.js"));

let fail = 0;
const log = (...a) => console.log(...a);
function check(cond, msg) {
    if (!cond) {
        fail++;
        console.error("  [FAIL]", msg);
    }
    return cond;
}
const eq = (a, b, msg) =>
    check(
        JSON.stringify(a) === JSON.stringify(b),
        `${msg}: 实得 ${JSON.stringify(a)},期望 ${JSON.stringify(b)}`,
    );

/** CSV 切行(CRLF);末行也有行尾,故最后一项是空串。 */
const splitCrLf = (csv) => csv.split("\r\n");

/** RFC 4180 单行解析(带引号态),用来验「这一行到底有几个字段」。 */
function parseCsvLine(line) {
    const out = [];
    let cur = "";
    let quoted = false;
    for (let i = 0; i < line.length; i++) {
        const c = line[i];
        if (quoted) {
            if (c === '"') {
                if (line[i + 1] === '"') {
                    cur += '"';
                    i++;
                } else quoted = false;
            } else cur += c;
        } else if (c === '"') quoted = true;
        else if (c === ",") {
            out.push(cur);
            cur = "";
        } else cur += c;
    }
    out.push(cur);
    return out;
}

/**
 * 07 T41 验收规模的事件仓 fixture:15 轨 × 每轨 40 段(单版本);偶数轨 stereo。
 * 形状照契约 §2.1 / §2.2 / §2.8 —— 与 app.js 的 store 逐字同形。
 */
function makeStore(version = 1, segsPerTrack = 40, labelOfTrack = null) {
    const channels = [];
    const values = {};
    for (let ch = 1; ch <= 15; ch++) {
        const stereo = ch % 2 === 0;
        channels.push({
            label: labelOfTrack ? labelOfTrack(ch) : "T" + ch,
            source_channels: stereo ? 2 : 1,
        });
        values["v" + version + "_t" + String(ch).padStart(2, "0") + "_width"] =
            stereo ? 80 + ch : 100;
    }
    const chList = [];
    for (let ch = 1; ch <= 15; ch++) {
        const segments = [];
        for (let i = 0; i < segsPerTrack; i++) {
            const mark = (ch * 7 + i * 13) % 23;
            const origin =
                mark === 0
                    ? "user_edited"
                    : mark === 11 && i > 0
                      ? "user_created"
                      : "auto";
            segments.push({
                segIdx: i,
                t0S: i * 2.5,
                t1S: i * 2.5 + 2.5,
                pan: ((ch * 7 + i) % 41) - 20,
                volDb: (i % 5) - 2,
                origin,
                locked: origin !== "auto",
            });
        }
        chList.push({ ch, segments, stale: false });
    }
    return {
        state: {
            global: { version_active: version },
            versions: [{ name: "V1" }, { name: "版本 B" }],
            channels,
        },
        params: { values },
        segments: { version, reason: "snapshot", channels: chList },
    };
}

// =============================================================================
log("=== ① 列定义冻结 + 两侧表头逐字对拍 ===");

{
    const cols = TS.CSV_HEADER.split(",");
    eq(cols.length, 13, "13 列");
    eq(
        cols,
        [
            "track_index",
            "track_label",
            "source_channels",
            "version",
            "version_name",
            "segment_index",
            "t0_sec",
            "t1_sec",
            "pan",
            "vol_db",
            "width",
            "origin",
            "locked",
        ],
        "列顺序 = 07 T41 / 11 §4.2.3 B2 的冻结定义",
    );
    eq(
        TS.SUGGEST_COLUMNS.map((c) => c.key),
        cols,
        "SUGGEST_COLUMNS 的 key 序 = 表头序(表格与 CSV 不可能排出两种列序)",
    );

    // C++ 侧的 kCsvHeader 是拆成两段字符串字面量写的(80 列行宽),这里拼回来再比。
    const h = src("src/core/export/SuggestionExport.h");
    const m = h.match(/kCsvHeader\s*\[\]\s*=\s*((?:\s*"[^"]*")+)\s*;/);
    if (check(!!m, "C++ 头里找得到 kCsvHeader 字面量")) {
        const cpp = [...m[1].matchAll(/"([^"]*)"/g)].map((x) => x[1]).join("");
        eq(cpp, TS.CSV_HEADER, "C++ kCsvHeader 与 web CSV_HEADER 逐字相等");
    }
    // 小数位数两侧也必须同档,否则同一段在表格里 0.1 度、在 CSV 里 0.001 度
    for (const [k, v] of [
        ["kDecimalsSec", TS.DECIMALS.sec],
        ["kDecimalsPan", TS.DECIMALS.pan],
        ["kDecimalsVol", TS.DECIMALS.vol],
        ["kDecimalsWidth", TS.DECIMALS.width],
    ]) {
        const mm = h.match(new RegExp(k + "\\s*=\\s*(\\d+)"));
        check(!!mm && Number(mm[1]) === v, `${k} 两侧同为 ${v}`);
    }
    // 轨数/版本数两侧同值(JS 侧是自己的常量,不能悄悄跟宪法分家)
    check(
        TS.TRACK_COUNT === 15 && TS.VERSION_COUNT === 2,
        "15 轨 / 2 版本([J59])",
    );
    check(TS.ALL_TRACKS_MASK === 0x7fff, "全轨掩码 = 0x7FFF(bit15 保留 0)");

    // **跨语言格式化黄金表**:两侧的底座不同(C++ printf 就近偶数 / JS toFixed 平局取大),
    // 光对拍表头与小数位常量盖不住数值这一层。表的真源在 C++ 测试里,这里读源码解析出来跑 JS。
    const cpp = src("tests/core/test_suggestion_export.cpp");
    const block = cpp.slice(
        cpp.indexOf("kFmtGolden[] = {"),
        cpp.indexOf("// clang-format on"),
    );
    const golden = [
        ...block.matchAll(/\{\s*(-?[\d.]+),\s*(\d+),\s*"([^"]*)"\s*\}/g),
    ].map((m) => [Number(m[1]), Number(m[2]), m[3]]);
    check(
        golden.length >= 10,
        `黄金表解析出 ${golden.length} 条(C++ 侧是真源)`,
    );
    for (const [v, d, want] of golden) {
        const got = TS.fmtFixed(v, d);
        check(
            got === want,
            `fmtFixed(${v}, ${d}) = "${got}",C++ 侧写的是 "${want}"`,
        );
    }
}

// =============================================================================
log("=== ② 文件形制:BOM + CRLF + 表头 ===");

{
    const rows = TS.buildSuggestionRows(makeStore(1, 2));
    const csv = TS.toCsv(rows);

    check(csv.charCodeAt(0) === 0xfeff, "首字符是 UTF-8 BOM(Excel 中文不乱码)");
    check(
        csv.slice(1).startsWith(TS.CSV_HEADER + "\r\n"),
        "BOM 之后紧跟表头行 + CRLF",
    );
    check(csv.endsWith("\r\n"), "末行也有行尾");
    check(!/[^\r]\n/.test(csv), "没有裸 LF —— 每个 \\n 前面都是 \\r");

    const lines = splitCrLf(csv);
    eq(lines.length, 1 + rows.length + 1, "行数 = 表头 + 数据行 + 末尾空串");
    eq(lines[lines.length - 1], "", "末尾空串来自最后一行的行尾");

    eq(TS.toCsv([]), TS.CSV_BOM + TS.CSV_HEADER + "\r\n", "空行集也出表头");
    eq(TS.CSV_BOM.charCodeAt(0), 0xfeff, "CSV_BOM 就是 U+FEFF 一个码点");
}

// =============================================================================
log("=== ③ RFC 4180 转义(轨名是用户数据)===");

{
    eq(TS.csvField("Lead"), "Lead", "无特殊字符不加引号");
    eq(TS.csvField("Lead, Dbl"), '"Lead, Dbl"', "含逗号整体加引号");
    eq(TS.csvField('say "hi"'), '"say ""hi"""', "内部双引号翻倍");
    eq(TS.csvField("a\r\nb"), '"a\r\nb"', "含换行整体加引号");
    eq(TS.csvField(""), "", "空串原样");
    eq(TS.csvField(null), "", "null → 空串(不写 'null' 进表)");

    const nasty = (ch) =>
        ch === 1 ? '主唱, 双轨 "A"' : ch === 2 ? "换\r\n行" : "T" + ch;
    const rows = TS.buildSuggestionRows(makeStore(1, 1, nasty));
    const csv = TS.toCsv(rows);
    // 含换行的字段会把「按 CRLF 切」骗过去,所以这里按引号态整份解析
    const fields = [];
    {
        let cur = "";
        let quoted = false;
        let line = [];
        for (let i = 1; i < csv.length; i++) {
            const c = csv[i];
            if (quoted) {
                if (c === '"') {
                    if (csv[i + 1] === '"') {
                        cur += '"';
                        i++;
                    } else quoted = false;
                } else cur += c;
            } else if (c === '"') quoted = true;
            else if (c === ",") {
                line.push(cur);
                cur = "";
            } else if (c === "\r" && csv[i + 1] === "\n") {
                line.push(cur);
                cur = "";
                fields.push(line);
                line = [];
                i++;
            } else cur += c;
        }
    }
    eq(fields.length, 1 + rows.length, "带换行轨名时总行数不变");
    check(
        fields.every((f) => f.length === 13),
        "每一行都恰好 13 个字段(轨名里的逗号/换行没把行撑开)",
    );
    eq(fields[1][1], '主唱, 双轨 "A"', "解析回来的轨名逐字还原");
    eq(fields[2][1], "换\r\n行", "含 CRLF 的轨名逐字还原");
}

// =============================================================================
log("=== ④ 规模:15 × 40 行数 == 段总数;origin/locked 取值 ===");

{
    const store = makeStore(1, 40);
    const rows = TS.buildSuggestionRows(store);
    const total = store.segments.channels.reduce(
        (n, c) => n + c.segments.length,
        0,
    );
    eq(rows.length, total, "行数 == 段总数(单版本 15 轨 × 40 段)");
    eq(rows.length, 600, "= 600");

    // 两个版本各导一次 = 07 T41 那句「15 轨 × 2 版本 × 每轨 40 段」的 1200 行
    const rowsV2 = TS.buildSuggestionRows(makeStore(2, 40));
    eq(rows.length + rowsV2.length, 15 * 2 * 40, "两版本合计 1200 行");
    eq(rowsV2[0].version, 2, "版本列跟着 version_active 走");
    eq(rowsV2[0].versionName, "版本 B", "版本名取 state.versions[v-1].name");

    // 行序 = 轨升序 → 段升序
    let ok = true;
    for (let i = 1; i < rows.length; i++) {
        const a = rows[i - 1];
        const b = rows[i];
        if (
            b.trackIndex < a.trackIndex ||
            (b.trackIndex === a.trackIndex && b.segmentIndex <= a.segmentIndex)
        ) {
            ok = false;
            break;
        }
    }
    check(ok, "行序 = 轨升序 → 段升序");

    // origin / locked 与 T19 state 编码逐字一致
    const origins = new Set(rows.map((r) => r.origin));
    eq(
        [...origins].sort(),
        ["auto", "user_created", "user_edited"],
        "origin 三值逐字 = T19 编码",
    );
    const cells = rows.map(TS.rowCells);
    check(
        cells.every((c) => c[12] === "true" || c[12] === "false"),
        "locked 列只写 true/false",
    );
    check(
        rows.every((r, i) => cells[i][12] === String(r.locked)),
        "locked 列 = 行上的布尔值",
    );
    // 未知 origin 一律回落 auto(手改工程文件 / 跨版本)
    eq(TS.originOf("wat"), "auto", "未知 origin 回落 auto");
    eq(TS.originOf(undefined), "auto", "缺失 origin 回落 auto");

    // 空仓 / 缺子树:不炸、出空行集
    eq(TS.buildSuggestionRows(undefined).length, 0, "空仓 → 空行集");
    eq(TS.buildSuggestionRows({}).length, 0, "缺 state → 空行集");
    eq(
        TS.buildSuggestionRows({ state: {}, segments: { channels: [] } })
            .length,
        0,
        "无段 → 空行集",
    );
}

// =============================================================================
log("=== ⑤ width:stereo 有值,mono 留空(不是 0)===");

{
    const rows = TS.buildSuggestionRows(makeStore(1, 1));
    const mono = rows.find((r) => r.trackIndex === 1);
    const stereo = rows.find((r) => r.trackIndex === 2);
    eq(mono.sourceChannels, 1, "轨 1 = mono");
    eq(stereo.sourceChannels, 2, "轨 2 = stereo");
    check(!mono.hasWidth, "mono 行 hasWidth=false");
    check(stereo.hasWidth, "stereo 行 hasWidth=true");
    eq(TS.rowCells(mono)[10], "", "mono 的 width 单元格是**空**,不是 0.0");
    eq(TS.rowCells(stereo)[10], "82.0", "stereo 的 width 取参数值(80+ch)");

    // stereo 轨的 width=0 是「收成 mono」的有效建议,必须写出来
    const store = makeStore(1, 1);
    store.params.values.v1_t02_width = 0;
    const z = TS.buildSuggestionRows(store).find((r) => r.trackIndex === 2);
    eq(TS.rowCells(z)[10], "0.0", "stereo 的 0 照写(与 mono 的空是两回事)");

    // 参数还没到(首帧 scvb.params 之前):**留空**,不拿默认值 100 顶上 ——
    // 猜一个数写进建议表,用户会照着它在 DAW 里设值。与 C++ 侧 kWidthUnknown 同口径。
    const noParams = makeStore(1, 1);
    noParams.params = { values: {} };
    const u = TS.buildSuggestionRows(noParams).find((r) => r.trackIndex === 2);
    check(!u.hasWidth, "width 参数未到 ⇒ stereo 行也不声称有 width");
    eq(TS.rowCells(u)[10], "", "未知 width 留空,不写 100");
    // C++ 侧的同款哨兵必须在 0..100 之外(落进值域就成了一个「有效建议」)
    const hh = src("src/core/export/SuggestionExport.h");
    const sent = hh.match(/kWidthUnknown\s*=\s*(-?[\d.]+)f/);
    check(
        !!sent && Number(sent[1]) < 0,
        "C++ kWidthUnknown 是 0..100 之外的负值",
    );

    // 数值格式:定点 + 负零归一
    eq(TS.fmtFixed(1 / 3, 3), "0.333", "3 位小数");
    eq(TS.fmtFixed(-0, 1), "0.0", "负零归一");
    eq(TS.fmtFixed(-0.04, 1), "0.0", "四舍五入到 -0.0 也归一");
    eq(TS.fmtFixed(-12.34, 1), "-12.3", "真负数保留负号");
    eq(TS.fmtFixed(2, 3), "2.000", "补足小数位");
    eq(TS.fmtFixed(NaN, 1), "0.0", "NaN 不写进 CSV 毒化下游");
}

// =============================================================================
log("=== ⑥ 表格与 CSV 同源(逐行相等,不是各算一遍)===");

{
    const rows = TS.buildSuggestionRows(makeStore(1, 6));
    const csv = TS.toCsv(rows);
    const lines = splitCrLf(csv).slice(1, -1);
    eq(lines.length, rows.length, "数据行数 = 行集长度");

    let same = true;
    for (let i = 0; i < rows.length; i++) {
        const cells = TS.rowCells(rows[i]);
        const parsed = parseCsvLine(lines[i]);
        if (JSON.stringify(cells) !== JSON.stringify(parsed)) {
            same = false;
            console.error(
                "  第 " + i + " 行不一致:",
                JSON.stringify(cells),
                "vs",
                JSON.stringify(parsed),
            );
            break;
        }
    }
    check(same, "CSV 的每个字段解析回来 == rowCells() 的那 13 个显示串");

    // 结构面:toCsv 里除了 rowCells + csvField,不许再出现第二套格式化
    const js = src("web/output/tab-suggestions.js");
    const body = js.slice(js.indexOf("export function toCsv"));
    const fn = body.slice(0, body.indexOf("\n}"));
    check(
        /rowCells\(row\)\.map\(csvField\)/.test(fn),
        "toCsv 直接序列化 rowCells() 的产物(表格与 CSV 同一份显示串)",
    );
    check(
        !/toFixed|Math\.round/.test(fn),
        "toCsv 里没有第二套数值格式化(否则两处迟早漂)",
    );
}

// =============================================================================
log("=== ⑦ mock 端到端:exportSuggestions 往返 ===");

{
    const s = driver.createPreviewSession({
        role: "output",
        params: "?fixture=fifteen-tracks",
    });
    const bridge = BR.createBridge({ role: "output", mockBackend: s.mock });

    check(
        typeof bridge.exportSuggestions === "function",
        "mock 实现了待转正名字 ⇒ 桥上挂得到(预览页当场可走全流程)",
    );

    // 事件仓:快照走桥(与 app.js 同路),段表取 fixture 那一份 —— §0.4 的「首帧必发」
    // 由 driver 的周期编排负责,而本冒烟不起 driver(起了就得管停),故直接取 world 的
    // 段表画像:它正是 mock 装进 segByCh 的**同一份**,与桥推下来的首帧逐字节相同。
    const store = {
        state: await bridge.requestInitialState(),
        params: { values: {} },
        segments: s.world.output.segments,
    };
    check(!!store.segments, "fixture 段表可用");

    const ok = await bridge.exportSuggestions(TS.currentScope());
    check(ok && ok.ok === true, "当前版本 + 全轨导出成功");
    check(Number.isInteger(ok.rows) && ok.rows > 0, "回执带 rows(整数、>0)");
    check(
        typeof ok.path === "string" && ok.path.endsWith(".csv"),
        "回执带 .csv 落点",
    );

    // mock 是**独立算路**(它从自己的 state 数行,不接受 UI 报数)——
    // 两条路对上,才说明「表格里那 N 行」与「导出的那 N 行」是同一批段。
    const uiRows = TS.buildSuggestionRows(store);
    eq(uiRows.length, ok.rows, "UI 行数 == mock 报回的行数(两条独立算路)");
    check(uiRows.length > 0, "fifteen-tracks 下建议表非空");

    // 同一批行走一遍序列化,行数与形制照旧成立(端到端而非只测纯函数)
    const csvLines = splitCrLf(TS.toCsv(uiRows));
    eq(
        csvLines.length,
        1 + ok.rows + 1,
        "端到端 CSV 行数 = 表头 + rows + 空串",
    );

    const all = await bridge.exportSuggestions({ versions: "all" });
    check(all.ok && all.rows > ok.rows, "versions:'all' 的行数严格多于单版本");

    eq(
        await bridge.exportSuggestions({ versions: "both" }),
        { ok: false, reason: "badArg" },
        "未知 versions → badArg",
    );
    eq(
        await bridge.exportSuggestions({ startS: 10, endS: 5 }),
        { ok: false, reason: "badArg" },
        "endS ≤ startS → badArg",
    );
    eq(
        await bridge.exportSuggestions({ tracksMask: 0 }),
        { ok: false, reason: "noData" },
        "零轨 → noData",
    );

    // bit15 保留 0(契约 §9.2):置位不该多导出一轨
    const b15 = await bridge.exportSuggestions({ tracksMask: 0xffff });
    eq(b15.rows, ok.rows, "tracksMask 的 bit15 被忽略");

    // 默认 scope 就是「当前版本 + 全 15 轨」
    eq(
        TS.currentScope(),
        { versions: "active", tracksMask: 0x7fff },
        "UI 送出的 scope",
    );
}

// =============================================================================
log("=== ⑧ 虚拟滚动窗口 ===");

{
    const H = TS.ROW_H;
    const w0 = TS.visibleWindow(0, 0, 0);
    eq([w0.start, w0.end, w0.padTop], [0, 0, 0], "空表窗口为空");

    const top = TS.visibleWindow(0, 440, 1200);
    eq(top.start, 0, "顶部 start=0(不为负)");
    eq(top.padTop, 0, "顶部无偏移");
    check(top.end >= 20 && top.end < 1200, "顶部只建可见 + overscan 行");

    const mid = TS.visibleWindow(100 * H, 440, 1200);
    eq(mid.start, 100 - TS.OVERSCAN, "中段 start 回退 overscan 行");
    eq(mid.padTop, (100 - TS.OVERSCAN) * H, "padTop = start × 行高");
    check(mid.end - mid.start < 60, "窗口不随总行数增长");

    const bot = TS.visibleWindow(1e9, 440, 1200);
    eq(bot.end, 1200, "滚过头也不越界");
    check(bot.start <= 1200, "start 被夹在总行数内");

    // 行高常量必须与 CSS 同值,否则行的 top 与实际排布错位
    const css = src("web/output/index.html");
    const m = css.match(/\.suggest-row\s*\{[^}]*height:\s*(\d+)px/);
    check(!!m && Number(m[1]) === H, `CSS .suggest-row 行高 = ROW_H(${H}px)`);
}

// =============================================================================
log("=== ⑧b 脏检查:同一份输入不该反复重建 600 个行对象 ===");

{
    // render() 由外壳的每帧合帧驱动,播放中 30Hz playhead / 25Hz params 都会排 render。
    // 三个输入没变时必须整帧跳过(与 T33 的按需投影同一纪律)。
    const store = makeStore(1, 40);
    const sig = TS.suggestionsSignature(store);

    check(TS.suggestionsDirty(store, null), "首次(无上一版签名)算脏");
    check(!TS.suggestionsDirty(store, sig), "同一份输入 ⇒ 不脏");

    // state / segments 是换对象语义(app.js 深合并后整体替换)
    check(
        TS.suggestionsDirty({ ...store, state: { ...store.state } }, sig),
        "state 换对象 ⇒ 脏",
    );
    check(
        TS.suggestionsDirty({ ...store, segments: { ...store.segments } }, sig),
        "segments 换对象 ⇒ 脏",
    );

    // params 每帧换对象但内容常没动 —— 只认本视图真读的那 15 个 width 参数
    const sameValues = { ...store, params: { values: store.params.values } };
    check(
        !TS.suggestionsDirty(sameValues, sig),
        "params 换了容器但 width 没动 ⇒ 不脏(这条是省帧的关键)",
    );
    const movedWidth = {
        ...store,
        params: { values: { ...store.params.values, v1_t02_width: 42 } },
    };
    check(TS.suggestionsDirty(movedWidth, sig), "width 真改了 ⇒ 脏");

    // 无关参数(pan/vol 走曲线真身,不进本表)不该把整份行集顶脏
    const otherParam = {
        ...store,
        params: { values: { ...store.params.values, v1_t02_pan: 33 } },
    };
    check(!TS.suggestionsDirty(otherParam, sig), "非 width 参数变化不触发重建");
}

// =============================================================================
log("=== ⑧c stale:过期采集的轨要能被认出来 ===");

{
    // §2.8 每轨 stale = 该轨有过期采集区间。13 列已冻结不便加列,故落在行属性上 ——
    // 照着一张过期的表在 DAW 里手工设值,是本视图唯一能骗到用户的地方。
    const store = makeStore(1, 3);
    store.segments.channels[2].stale = true; // 轨 3
    const rows = TS.buildSuggestionRows(store);
    const stale = rows.filter((r) => r.stale);
    eq(new Set(stale.map((r) => r.trackIndex)).size, 1, "只有轨 3 被标过期");
    eq(stale.length, 3, "该轨的每一行都带上标记");
    check(
        rows.filter((r) => r.trackIndex !== 3).every((r) => !r.stale),
        "其余轨不受影响",
    );
    // stale 只是行属性,**不进 CSV**(13 列冻结)
    eq(TS.rowCells(stale[0]).length, 13, "CSV 仍是 13 列");
    check(
        !TS.toCsv(rows).includes("stale"),
        "stale 没有偷偷混进 CSV(列定义是冻结的)",
    );
}

// =============================================================================
log("=== ⑨ 词条 suggest.* ===");

{
    const keys = Object.keys(T.zh).filter((k) => k.startsWith("suggest."));
    check(keys.length >= 27, `zh 侧有 ${keys.length} 个 suggest.* 词条`);
    for (const lang of ["en", "fr"]) {
        const miss = keys.filter((k) => !(k in T[lang]));
        eq(miss, [], `${lang} 侧无缺失`);
    }
    for (const lang of ["zh", "en", "fr"]) {
        const empty = keys.filter(
            (k) => typeof T[lang][k] !== "string" || T[lang][k].trim() === "",
        );
        eq(empty, [], `${lang} 侧无空词条`);
    }
    // 占位符三语一致(改一处漏两处的经典事故面)
    const ph = (s) => (s.match(/\{[a-zA-Z]+\}/g) || []).sort().join(",");
    for (const k of keys) {
        check(
            ph(T.zh[k]) === ph(T.en[k]) && ph(T.zh[k]) === ph(T.fr[k]),
            `「${k}」三语占位符一致`,
        );
    }
    // 13 个列头词条都在
    for (const col of TS.SUGGEST_COLUMNS) {
        check(col.t in T.zh, `列头词条 ${col.t} 存在`);
    }
    check("suggest.staleNote" in T.zh, "过期采集提示词条在");
    // 词条走 textContent,不过 markdown —— 写了 ** 就会原样显示成星号
    for (const lang of ["zh", "en", "fr"]) {
        const md = keys.filter((k) => /\*\*|__/.test(T[lang][k]));
        eq(md, [], `${lang} 词条里有 markdown 强调标记(会原样显示)`);
    }
    // 「它是建议,不是执行」——07 T41 强调栏:三语都得把这层意思说出来
    check(
        /不会替你/.test(T.zh["suggest.disclaimer"]),
        "zh 免责句写明 SCVB 不会替你应用",
    );
    check(
        /does not apply/i.test(T.en["suggest.disclaimer"]),
        "en 免责句写明 SCVB 不会替你应用",
    );
    check(
        /ne les applique pas/i.test(T.fr["suggest.disclaimer"]),
        "fr 免责句写明 SCVB 不会替你应用",
    );

    // 禁词:任何一条都会把「建议」说成「已经生效」,与本卡的强调栏直接冲突。
    // 只扫**用户可见文案**(词典)—— 源码注释里正需要引用这些说法来解释为什么禁它们。
    const BANNED = ["写入完成", "已应用到 DAW", "自动应用", "导出即生效"];
    for (const banned of BANNED) {
        for (const lang of ["zh", "en", "fr"]) {
            const hit = keys.filter((k) => T[lang][k].includes(banned));
            eq(hit, [], `${lang} 词条命中禁词「${banned}」`);
        }
    }
}

// =============================================================================
log("=== ⑩ 源码不变式 ===");

{
    // 桥面名字必须停在 PENDING_FUNCS,**没有**偷偷进 parity 比对面
    check(
        BR.PENDING_FUNCS.output.includes("exportSuggestions"),
        "exportSuggestions 在 PENDING_FUNCS.output",
    );
    check(
        !BR.BRIDGE_FUNCTIONS.output.includes("exportSuggestions"),
        "exportSuggestions **不在** BRIDGE_FUNCTIONS(契约 §7 还没收它)",
    );
    eq(
        BR.BRIDGE_FUNCTIONS.output.length,
        35,
        "冻结名表 35 个(#80 setMasterChartMode 转正 +1;本卡 exportSuggestions 一个没多)",
    );

    // 变更文档在库(仓 CLAUDE.md §5:桥面新增必须有它)
    check(
        existsSync(
            join(ROOT, "docs/contract-changes/20260825-export-suggestions.md"),
        ),
        "契约变更文档在库",
    );

    const js = src("web/output/tab-suggestions.js");
    // [J67]:本视图是全项目唯一的表格式视图,不得据此复活分段列表视图
    check(
        /J67/.test(js) && /不得/.test(js),
        "文件头留着 [J67] 的「不得复活列表视图」纪律",
    );
    // 桥函数不存在时不许假装写了文件
    check(
        /typeof bridge\.exportSuggestions === "function"/.test(js),
        "导出前做能力探测(桥上没挂就不调)",
    );
    check(
        /isPreview/.test(js),
        "浏览器下载只在预览态触发(真宿主由 native 落盘)",
    );
    // 没有第五个 tab:setActiveTab 的四值枚举没被动过
    check(
        !/setActiveTab\(\s*["']suggest/.test(js),
        "没有把 suggest 当成第五个 tab 送进 setActiveTab",
    );

    // ARIA table 树必须是连着的:table → (row 表头 | rowgroup) → row → cell。
    // 中间夹一层 div 就断了,读屏软件念不出行列关系。取 table 而非 grid:
    // grid 隐含格级键盘导航,而这里只有整体滚动。
    const html = src("web/output/index.html");
    check(
        /data-gb="suggest-table"[\s\S]{0,60}role="table"/.test(html),
        "表容器是 table(不是 grid —— 没有格级键盘导航)",
    );
    check(
        /data-gb="suggest-scroll"[\s\S]{0,140}role="rowgroup"/.test(html),
        "滚动容器是 rowgroup",
    );
    check(
        /data-gb="suggest-thead"[\s\S]{0,140}aria-rowindex="1"/.test(html),
        "表头占 aria-rowindex 1",
    );
    check(
        /el\.scroll\.replaceChildren\(el\.strut, \.\.\.local\.pool/.test(js),
        "数据行是 rowgroup 的**直接**子节点(不套 spacer/rows 两层)",
    );
    for (const attr of ["aria-rowindex", "aria-colindex", "columnheader"]) {
        check(js.includes(attr), `虚拟滚动补齐了 ${attr}`);
    }
    check(
        /aria-rowcount/.test(js) && /aria-colcount/.test(js),
        "table 声明了总行列数(虚拟滚动下 DOM 里只有可见行)",
    );
    // 表头 13 个格也要有列号(数据格有、表头没有 = 读屏软件对不上列)
    const headBlock = js.slice(
        js.indexOf("function mountHead()"),
        js.indexOf("function rowNodeAt("),
    );
    check(/aria-colindex/.test(headBlock), "表头格也带 aria-colindex");
    // 斑马纹按行号取而不是 :nth-child —— 节点是复用的,按子节点序号上色会随滚动翻转
    check(
        !/suggest-row:nth-child/.test(html),
        "斑马纹不按子节点序号(否则滚动时黑白翻转)",
    );

    // 滚动路径:节点复用 + rAF 合帧(每滚 22px 重建几百个节点是与「每帧重建行集」
    // 同一类的浪费,只是触发源换成了滚动)
    check(
        /function rowNodeAt\(/.test(js) && /local\.pool\[slot\]/.test(js),
        "行节点走池子复用",
    );
    check(
        /on\(el\.scroll, "scroll", scheduleRows/.test(js),
        "scroll 只标脏,重排合到下一帧",
    );
    check(
        /cancelAnimationFrame\(local\.scrollRaf\)/.test(js),
        "destroy() 撤掉在途的那一帧(否则拆完还会再画一次)",
    );

    // 表头在滚动容器外、数据行在其 padding box 内 —— 9px 竖滚动条(base.css)会把两边
    // 错开。两条配对修复缺一不可:表头补右内边距 + 滚动槽恒定预留。
    check(
        /\.suggest-thead\s*\{[^}]*padding-right:\s*calc\(var\(--sp-13\) \+ var\(--scrollbar-w\)\)/.test(
            html,
        ),
        "表头补了一个滚动条宽的右内边距",
    );
    check(
        /\.suggest-scroll\s*\{[^}]*scrollbar-gutter:\s*stable/.test(html),
        "滚动槽恒定预留(否则行少不溢出时反过来错一个滚动条宽)",
    );
    // 滚动条宽度是**布局量**,三处消费同一个 token,改一处不必人记着改另两处
    const tokens = src("web/shared/tokens.css");
    const base = src("web/shared/base.css");
    check(
        /--scrollbar-w:\s*9px/.test(tokens),
        "tokens.css 立了 --scrollbar-w 真源",
    );
    check(
        /-webkit-scrollbar\s*\{[^}]*width:\s*var\(--scrollbar-w\)/.test(base),
        "base.css 消费 --scrollbar-w(不再写死 9px)",
    );

    // renderChrome 每帧都跑(local.busy / status / exportAvailable 不在脏检查签名里),
    // 而 .suggest-status / .suggest-stale 是 aria-live 区 —— textContent 的 setter
    // 一律新建文本节点,写同一句话也会让读屏重播。
    check(
        /function setText\(node, next\)/.test(js) &&
            /node\.textContent !== next/.test(js),
        "文案走 setText(值没变就不写 DOM)",
    );
    const chrome = js.slice(
        js.indexOf("function renderChrome()"),
        js.indexOf("function render()"),
    );
    check(
        !/\.textContent =/.test(chrome),
        "renderChrome 里没有裸 textContent 赋值(全走 setText)",
    );
    check(
        !/new Set\(/.test(chrome),
        "renderChrome 里没有 O(行数) 全表扫描(计数挪进了脏块)",
    );
    check(
        !/\.setAttribute\(/.test(chrome),
        "renderChrome 里没有裸 setAttribute(属性写入同样每帧,全走 setAttr)",
    );
    check(
        /data-gb="suggest-scroll"[\s\S]{0,200}data-t-aria="suggest\.rowsAria"/.test(
            html,
        ),
        "可聚焦的 rowgroup 有可访问名(否则读屏只念「行组」)",
    );
    check(
        /el\.scroll\.replaceChildren\(el\.strut \|\| ""\)/.test(js),
        "destroy() 把行节点从 DOM 摘掉(不只是清数组)",
    );

    // 桥不可用时要有**常驻**说明,不能只挂在 title 上(点一枚灰钮零反馈)
    check(
        /local\.status \|\|[\s\S]{0,200}suggest\.exportUnavailable/.test(js),
        "桥上没挂 exportSuggestions 时状态行常驻说明",
    );
    check(
        /el\.entry\.focus\(\)/.test(js),
        "返回泳道时焦点还给入口钮(不掉回 body)",
    );

    // 建议表开着时泳道的 rAF 必须停:`data-tab` 仍是 wave、泳道只是被 CSS 藏了,
    // 只看 data-tab 的话插值层会往 0 宽 canvas 上空转,帧时还会喂进 governor 误触降级。
    const tw = src("web/output/tab-wave.js");
    const active = tw.slice(
        tw.indexOf("function isPanelActive()"),
        tw.indexOf("function isPanelActive()") + 700,
    );
    check(
        /data-view/.test(active) && /"suggest"/.test(active),
        "isPanelActive() 同时看 data-tab 与 data-view",
    );

    // web/ 不许反向依赖 web-preview/(06 §6.2 单向依赖)
    check(
        !/web-preview/.test(js.replace(/\/\/.*|\/\*[\s\S]*?\*\//g, "")),
        "代码里不 import web-preview(注释里提一嘴不算)",
    );
}

// =============================================================================
log(fail === 0 ? "\n全部通过 ✅" : `\n${fail} 条断言失败 ❌`);
process.exit(fail === 0 ? 0 : 1);
