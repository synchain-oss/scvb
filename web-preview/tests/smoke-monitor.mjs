// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Monitor 冒烟(node,无 DOM;T46 / [J75] C)
// =============================================================================
// 与既有冒烟同款口径:仓内零 node_modules,故断言面是**纯函数 + mock 端到端 + 源码不变式**,
// 画布光栅与指针手势归浏览器手测(shot.mjs 截图 + 真机 preview)。
//
// 本套的目标是「统筹侧自测到位,不许用户在 DAW 里返工」,故断言一律做到**数值级**:
// 不写「元素存在」「数组非空」这类只要不炸就绿的判据,而是对 fixture 的确定性数据
// 逐条比对轨号、列号、断口位置、时间坐标、定点值。
//
// 跑什么:
//   ① 提取件不回归(Output 侧零行为变化的机检面)
//   ② **viz 契约 parity** —— JS 侧镜像 `web/monitor/viz-contract.js` 逐条比对 T44 的
//      `tests/golden/ipc-layout.txt`(容错口径照 `check-bridge-parity.mjs` 先例:
//      golden 里还没有 viz 段就 [SKIP],有却对不上才红)
//   ③ viz 投影纯函数 —— 位序 / 定点 / 断线 / 掩码 / 车道按需重发合并
//   ④ **两图渲染的数值正确性** —— 对 fixture 逐轨逐列断言,不是「画出来了」
//   ⑤ mock 端到端 —— 首帧 / 组切换 / 空态 / 重连 / 停摆 / 拒连 / 降级
//   ⑥ 词条三语
//   ⑦ 只读不变式与页面纪律
//   ⑧ **生命周期** —— suspend / resume / destroy 的多实例契约与幂等
//
// 用法:node web-preview/tests/smoke-monitor.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有断言失败(逐条打印 [FAIL])。
// =============================================================================

import { pathToFileURL, fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";
import { existsSync, readFileSync } from "node:fs";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const u = (p) => pathToFileURL(join(ROOT, p)).href;
const src = (p) => readFileSync(join(ROOT, p), "utf8");

const DC = await import(u("web/shared/distribution-chart.js"));
const DM = await import(u("web/shared/dist-motion.js"));
const TM = await import(u("web/output/tab-master.js"));
const TC = await import(u("web/shared/trajectory-chart.js"));
const TCOL = await import(u("web/shared/track-colors.js"));
const VC = await import(u("web/monitor/viz-contract.js"));
const VIZ = await import(u("web/monitor/viz.js"));
const MBOX = await import(u("web/monitor/monitor-box.js"));
const MBRIDGE = await import(u("web/monitor/monitor-bridge.js"));
const MMOCK = await import(u("web-preview/mock/monitor-mock.js"));
const { T } = await import(u("web/shared/i18n.js"));

let fail = 0;
let skips = 0;
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
function skip(msg) {
    skips++;
    console.log("  [SKIP]", msg);
}
const near = (a, b, eps, msg) =>
    check(Math.abs(a - b) <= eps, `${msg}: 实得 ${a},期望 ≈${b}(±${eps})`);

// =============================================================================
log("=== ① 提取件不回归(Output 侧零行为变化)===");

{
    check(
        TM.distGeometry === DC.distGeometry,
        "tab-master.distGeometry ≡ distribution-chart.distGeometry(再导出,不是副本)",
    );
    eq(
        DC.distGeometry(0, -6, 100),
        { x: 50, h: 71.43, half: 16 },
        "居中轨几何",
    );
    eq(
        DC.distGeometry(-100, 12, 100),
        { x: 0, h: 88, half: 0 },
        "贴左缘:半宽被 x 夹到 0",
    );
    eq(
        DC.distGeometry(100, -24, 100),
        { x: 100, h: 8, half: 0 },
        "贴右缘 + 最低音量夹到 8",
    );
    eq(DC.distGeometry(999, 999, 999).x, 100, "越界 pan 夹到 +100");
    eq(DC.distGeometry(0, 0, 0).half, 0, "width 0 ⇒ 无张开线");

    // ---- 拼串 oracle:**逐字转写自提取前的 tab-master.js**(renderDist / renderLegend
    // 两段模板)。照着新实现抄一遍是自证,故这里保留旧写法的每一处空格与顺序。
    const oracleBar = (ch, geo, tc, hi, lead) =>
        `<div class="dist-bar" data-lead="${lead ? 1 : 0}" data-ch="${ch}" data-hi="${hi}" style="${tc}--x:${geo.x.toFixed(2)}%;--h:${geo.h.toFixed(2)}%"></div>`;
    const oracleSpan = (ch, geo, tc, hi) =>
        `<div class="dist-span" data-ch="${ch}" data-hi="${hi}" style="${tc}--x0:${(geo.x - geo.half).toFixed(2)}%;--w:${(geo.half * 2).toFixed(2)}%;--y:calc(18px + ${geo.h.toFixed(2)}%)"></div>`;

    const rows = [
        {
            ch: 1,
            pan: -4,
            volDb: -1.7,
            widthPct: 100,
            stereo: false,
            lead: true,
        },
        {
            ch: 3,
            pan: -58,
            volDb: -8.2,
            widthPct: 82,
            stereo: true,
            lead: false,
        },
        {
            ch: 15,
            pan: 12,
            volDb: -13.9,
            widthPct: 100,
            stereo: false,
            lead: false,
        },
    ];
    for (const hi of [0, 3]) {
        const spans = [];
        const bars = [];
        for (const r of rows) {
            const geo = DC.distGeometry(r.pan, r.volDb, r.widthPct);
            const tc = `--tc:var(${TCOL.trackColorVar(r.ch)});`;
            const dim = hi && hi !== r.ch ? "0" : "1";
            if (r.stereo) spans.push(oracleSpan(r.ch, geo, tc, dim));
            bars.push(oracleBar(r.ch, geo, tc, dim, r.lead));
        }
        eq(
            DC.distBarsHtml(rows, hi),
            spans.concat(bars).join(""),
            `distBarsHtml 与提取前模板逐字节相同(highlight=${hi})`,
        );
    }
    const html = DC.distBarsHtml(rows, 0);
    check(
        html.indexOf("dist-span") < html.indexOf("dist-bar"),
        "立体声张开线排在柱体之前(提取前 spans.concat(bars) 的语义)",
    );

    const legendRows = [
        { ch: 1, label: "主唱", stereo: false },
        { ch: 3, label: 'a<b&"c', stereo: true },
    ];
    const oracleLegend = (r, hi, badge) =>
        `<span class="chart-legend__item" role="listitem" data-legend-ch="${r.ch}" data-hi="${hi === r.ch ? 1 : 0}">` +
        `<span class="chart-legend__dot" style="--tc:var(${TCOL.trackColorVar(r.ch)})" aria-hidden="true"></span>` +
        `${String(r.ch).padStart(2, "0")}${r.label ? " " + DC.esc(r.label) : ""}` +
        (r.stereo
            ? `<span class="chart-legend__st">${DC.esc(badge)}</span>`
            : "") +
        `</span>`;
    eq(
        DC.legendItemsHtml(legendRows, { badge: "ST", highlightCh: 3 }),
        legendRows.map((r) => oracleLegend(r, 3, "ST")).join(""),
        "legendItemsHtml 与提取前模板逐字节相同",
    );
    check(
        DC.legendItemsHtml(legendRows, {}).includes("a&lt;b&amp;&quot;c"),
        '轨名转义(< & " 全部转成实体)',
    );
    check(
        !DC.legendItemsHtml([{ ch: 1, label: "<img onerror=x>" }], {}).includes(
            "<img",
        ),
        "标签形状的轨名不会变成真标签",
    );
    eq(DC.legendItemsHtml(null, {}), "", "空行集 → 空串");
    eq(DC.distBarsHtml(null, 0), "", "空 rows → 空串");
    eq(DC.tt2(7), "07", "两位零填充");

    const rowNode = {
        closest: (sel) =>
            sel === "[data-legend-ch]" ? { getAttribute: () => "12" } : null,
    };
    eq(DC.legendChOf(rowNode), 12, "命中图例行 → 轨号");
    eq(DC.legendChOf({ closest: () => null }), 0, "不在行上 → 0");
    eq(DC.legendChOf(null), 0, "空目标 → 0(不炸)");

    const tm = src("web/output/tab-master.js");
    // [SL-203] Output 侧的写入面也改走 rAF 补间了(与 Monitor 同一件),不再直接拼柱体。
    // 这条断言原本钉的是 `distBarsHtml(rows, local.chartHi, readParam("width"))`;
    // 它真正要守的是**两件事实**,与走不走补间器无关,故按新形态原样守住:
    //   ① renderDist 把算好的 rows 交出去(而不是自己拼第二份模板);
    //   ② 全局「最大角度」进几何 —— 名义 pan 要 ×width/100 才是听到的位置,
    //      不喂它,拧滑杆时分布图纹丝不动(v5 P2-10 用户裁定)。
    check(
        /distMotion\.push\(rows, local\.chartHi\)/.test(tm),
        "renderDist 把 rows 交给补间器(不自己拼第二份柱体模板)",
    );
    check(
        /getGlobalWidthPct:\s*\(\)\s*=>\s*readParam\("width"\)/.test(tm),
        "全局最大角度仍进几何(v5 P2-10:拧滑杆时分布图实时收拢/张开)",
    );
    check(
        !/distBarsHtml/.test(tm),
        "tab-master.js 里不再直接拼柱体(拼串归补间器,免得两条路各写一份)",
    );
    check(
        /legendItemsHtml\(rows, \{/.test(tm),
        "renderLegend 走 legendItemsHtml",
    );
    check(!/class="dist-bar"/.test(tm), "tab-master.js 里不再有第二份柱体模板");
    check(
        !/class="chart-legend__item"/.test(tm),
        "tab-master.js 里不再有第二份图例模板",
    );
}

// =============================================================================
log("=== ② viz 契约 parity(JS 镜像 ↔ T44 golden)===");

// 段布局的唯一真源在 native 侧(T44)。本节把 `web/monitor/viz-contract.js` 的每一条常量与
// 每一个字段名,逐条比对 `tests/golden/ipc-layout.txt`。
// **容错口径照 `scripts/check-bridge-parity.mjs` 的先例**:那边写着「某侧文件不存在 => [SKIP],
// 文件存在但抽取不到 => [ERROR]」。T44(PR #89)还没并进 feature/v1,故 golden 里暂时没有
// viz 段 —— 打印 [SKIP] 不影响退出码;**一旦 T44 合并、本分支 rebase,机检自动生效**,
// 不需要谁记得回来打开它。
{
    const goldenPath = join(ROOT, "tests", "golden", "ipc-layout.txt");
    const golden = existsSync(goldenPath)
        ? readFileSync(goldenPath, "utf8")
        : "";
    const hasViz = /^viz_columns\s+\d+/m.test(golden);

    if (!hasViz) {
        skip(
            "tests/golden/ipc-layout.txt 里还没有 viz 段(T44/PR #89 未合入 feature/v1)—— " +
                "parity 待 rebase 后自动生效;本节的镜像值来自 docs/contract-changes/20260825-viz-segment.md 的逐字转写",
        );
        // golden 缺席时至少把镜像表的**自洽性**查掉:这几条不依赖 native 文件。
        eq(VC.VIZ_COLUMNS % 32, 0, "列数须 32 整除(位图才无残位)");
        eq(VC.VIZ_COVERAGE_WORDS, VC.VIZ_COLUMNS / 32, "位图字数 = 列数 / 32");
        eq(VC.VIZ_PAN_NONE, -32768, "哨兵 = INT16_MIN");
        check(
            VC.VIZ_PAN_NONE < -100 * VIZ.VIZ_PAN_SCALE,
            "哨兵不与任何合法定点值相撞",
        );
        eq(VC.VIZ_TRACKS, 15, "轨数 15(J59)");
    } else {
        // ---- 标量常量逐条对拍
        for (const [key, want] of Object.entries(VC.VIZ_GOLDEN_CONSTS)) {
            const m = new RegExp(`^${key}\\s+(-?\\d+)\\s*$`, "m").exec(golden);
            check(!!m, `golden 里有常量 ${key}`);
            if (m) eq(Number(m[1]), want, `常量 ${key} 与 JS 镜像同值`);
        }
        // ---- 字段名逐条对拍(golden: `field <Struct>.<name> offset <n>`)
        const goldenFields = [];
        const re = /^field\s+(Viz\w+)\.(\w+)\s+offset\s+(\d+)\s*$/gm;
        let m;
        while ((m = re.exec(golden)) !== null) {
            goldenFields.push({
                struct: m[1],
                name: m[2],
                offset: Number(m[3]),
            });
        }
        check(
            goldenFields.length > 0,
            "golden 里抽得到 Viz* 字段(抽不到 = 格式漂移,必须修)",
        );
        const mirrorKey = (f) => `${f.struct}.${f.name}`;
        const gset = goldenFields.map(mirrorKey);

        // **已承诺但尚未落段的字段**(T44 2026-08-25 对表信答应加的 `VizTrackState` /
        // `VizTrackLabels`)。golden 里还没有它们时**不算镜像多写** —— 那是 native 侧
        // 还没推,不是这边写错了;打 [SKIP] 说明白在等谁。它们一落段,下面的分支就
        // 自动切到全量对拍,不需要谁回来改这个脚本。
        const promised = new Set(VC.VIZ_PROMISED_FIELDS);
        const isPromised = (f) => promised.has(f.json);
        const landedMirror = VC.VIZ_FIELDS.filter((f) => !isPromised(f));
        const promisedMirror = VC.VIZ_FIELDS.filter(isPromised);
        const goldenHasPromised = promisedMirror.some((f) =>
            gset.includes(mirrorKey(f)),
        );
        const expectMirror = goldenHasPromised ? VC.VIZ_FIELDS : landedMirror;
        if (!goldenHasPromised) {
            skip(
                `golden 里还没有 T44 已答应的 ${promisedMirror.length} 个字段` +
                    `(${promisedMirror.map((f) => f.json).join(" / ")})—— ` +
                    "等 T44 推段字段扩充;其余字段照常全量对拍",
            );
        }
        const mset = expectMirror.map(mirrorKey);

        for (const k of gset) {
            check(mset.includes(k), `golden 的字段 ${k} 在 JS 镜像表里`);
        }
        for (const k of mset) {
            check(gset.includes(k), `JS 镜像表的 ${k} 在 golden 里(多写了?)`);
        }
        // 顺序 = 偏移升序:镜像表的书写顺序必须与段内偏移一致,否则「照表读」会读串
        eq(mset, gset, "字段顺序与 golden 的偏移升序逐项一致");
        // ---- 段名(每组一份)
        check(
            /name g1 viz SynchainSCVB\.v1\.g1\.viz/.test(golden),
            "golden 里有 g1 的 viz 段名",
        );
    }

    // ---- 第二个比对面:**桥面 JSON**(T45 `MonitorEditor.cpp` 的 `setProperty("X"`)。
    // golden 管段布局,这一条管桥投影出来的字段名 —— 两者会各自漂,必须各查各的。
    // 文件不在树上(T45 未合入)就 [SKIP],同 golden 那一路的容错口径。
    {
        const cppPath = join(ROOT, "src", "monitor", "MonitorEditor.cpp");
        if (!existsSync(cppPath)) {
            skip(
                "src/monitor/MonitorEditor.cpp 不在树上(T45 = PR **#94**,取代已关闭的 #92;尚未合入 feature/v1)—— " +
                    "桥面 JSON parity 待它合入后自动生效;已对着其分支头临时嫁接验证过全绿",
            );
        } else {
            const cpp = readFileSync(cppPath, "utf8");
            const props = new Set();
            const pre = /setProperty\("([^"]+)"/g;
            let mm;
            while ((mm = pre.exec(cpp)) !== null) props.add(mm[1]);
            check(props.size > 0, "抽得到 setProperty 名(抽不到 = 格式漂移)");

            // **按函数切开再比**,不是整文件一个大集合:整文件比对只能证明「这个名字在
            // 某处出现过」—— `groupId` 在 `buildStatePayload` 与 `handleSetObservedGroup`
            // 里都有,于是「viz 帧里到底带没带 groupId」这件事整文件比对根本答不上来,
            // 而那正是「换组后在途帧被当成本组数据画上去」这条闸的前提。
            const bodies = new Map();
            const fre = /MonitorEditor::(\w+)[^\n]*\n\{([\s\S]*?)\n\}/g;
            let fm;
            while ((fm = fre.exec(cpp)) !== null) bodies.set(fm[1], fm[2]);
            const namesIn = (fn) => {
                const body = bodies.get(fn) || "";
                const out = new Set();
                const r = /setProperty\("([^"]+)"/g;
                let m2;
                while ((m2 = r.exec(body)) !== null) out.add(m2[1]);
                return out;
            };
            const vizProps = namesIn("buildVizPayload");
            const stateProps = namesIn("buildStatePayload");
            check(vizProps.size > 0, "切得出 buildVizPayload 的字段集");
            check(stateProps.size > 0, "切得出 buildStatePayload 的字段集");

            // ---- 正向:镜像表 json 列里每个非 null 的名字,**viz 帧**里都得真的有
            for (const f of VC.VIZ_FIELDS) {
                if (!f.json) continue;
                check(
                    vizProps.has(f.json),
                    `viz 帧里有 ${f.json}(段 ${f.struct}.${f.name})`,
                );
            }
            // 桥自己算的三条(段里没有对应字段,故不在 VIZ_FIELDS 里)——
            // 漏送任何一条,页面都会把「在线/停更/掉线」判错或把在途帧当本组数据画上去。
            for (const f of VC.VIZ_DERIVED_FIELDS) {
                check(
                    vizProps.has(f.json),
                    `viz 帧里有派生字段 ${f.json}(${f.from})`,
                );
            }
            // ---- **反向**:桥送出、而镜像表里没有的字段 = 接口加了东西而这边不知道。
            // 这一轮 T45 加 `fresh`/`groupId` 时就是这个形态(且 `online` 的**语义**也变了,
            // 机检查不出语义,只能靠他们明说)—— 反向断言至少让「加了字段」当场可见,
            // 不必等页面上出现莫名其妙的行为再回头找。
            const known = new Set([
                ...VC.VIZ_FIELDS.filter((f) => f.json).map((f) => f.json),
                ...VC.VIZ_DERIVED_FIELDS.map((f) => f.json),
            ]);
            for (const k of vizProps) {
                check(
                    known.has(k),
                    `viz 帧的字段 ${k} 在 JS 镜像表里(桥加了字段?对表补一条)`,
                );
            }
            for (const k of VC.STATE_JSON_FIELDS) {
                check(stateProps.has(k), `scvb.state 里有 ${k}`);
            }
            // `ui` 是子树:它那两个键在同一个函数体里 set 在另一个 DynamicObject 上,
            // 函数级扫描区分不了两层,故把子树键并进期望集(正向也各查一条)。
            for (const k of VC.STATE_UI_FIELDS) {
                check(stateProps.has(k), `scvb.state 的 ui 子树里有 ${k}`);
            }
            const stateKnown = new Set([
                ...VC.STATE_JSON_FIELDS,
                ...VC.STATE_UI_FIELDS,
            ]);
            for (const k of stateProps) {
                check(
                    stateKnown.has(k),
                    `scvb.state 的字段 ${k} 在 STATE_JSON_FIELDS / STATE_UI_FIELDS 里`,
                );
            }
            check(
                props.has(VC.GROUPS_JSON_KEY),
                `桥送出 scvb.groups 的 ${VC.GROUPS_JSON_KEY}`,
            );
            // 仍待落地的字段:**在树上就该断言它还没有** —— 有了就该把它移出
            // VIZ_PENDING_FIELDS 并改成正向断言,这条红了正是在提醒我去做
            for (const k of VC.VIZ_PENDING_FIELDS) {
                check(
                    !props.has(k),
                    `${k} 已经在桥里了 —— 请把它移出 VIZ_PENDING_FIELDS 并改成正向断言`,
                );
            }
        }
    }

    // ---- 与 native 无关、但必须成立的镜像自洽:JS 侧两处常量不许各写一份
    eq(VIZ.VIZ_COLUMNS, VC.VIZ_COLUMNS, "viz.js 的列数 = 契约镜像(再导出)");
    eq(VIZ.VIZ_PAN_SCALE, VC.VIZ_PAN_SCALE, "定点标度同源");
    eq(VIZ.VIZ_ABI, VC.VIZ_ABI, "abi 同源");
    eq(VIZ.CHANNEL_COUNT, VC.VIZ_TRACKS, "轨数同源");
    eq(
        VC.VIZ_ATTACH.slice(),
        ["ok", "failed", "abiMismatch"],
        "attach 三态(failed 与 abiMismatch 必须可区分)",
    );
    eq(
        VC.VIZ_PROMISED_FIELDS.slice(),
        [
            "trackPanNow",
            "trackVolDb",
            "trackWidthPct",
            "trackLabels",
            "leadMask",
        ],
        "T44/T45 依两轮对表新增、尚未合入 feature/v1 的五条",
    );
    eq(
        VC.VIZ_PENDING_FIELDS.slice(),
        [],
        "**已经没有待确认的字段了** —— 两轮提的每一条 native 侧都实现了",
    );
    eq(
        VC.VIZ_STATE.slice(),
        ["online", "offline", "abiMismatch"],
        "scvb.state.viz 三态(T45 vizStateName)",
    );
    eq(
        VC.STATE_JSON_FIELDS.slice(),
        ["group_id", "ui", "viz", "fresh"],
        "scvb.state 的字段:**镜像宪法 state 字段的键一律 snake_case + ui 子树**(契约 §0.2 规则① / 裁定 A-30)",
    );
    eq(
        VC.VIZ_DERIVED_FIELDS.map((f) => f.json),
        ["online", "fresh", "group_id"],
        "帧里桥自己算的三条:online / fresh **各是一件事** + 帧自带组号(`group_id`,snake_case)",
    );
    // 派生字段与段内字段**不许重名**:重了的话「golden 里有它吗」这条断言会两头指着
    // 同一个名字,漂移就被掩盖掉
    for (const f of VC.VIZ_DERIVED_FIELDS) {
        check(
            !VC.VIZ_FIELDS.some((g) => g.json === f.json),
            `派生字段 ${f.json} 不与段内字段的 json 名相撞`,
        );
    }
    eq(
        VC.GROUPS_JSON_KEY,
        "groups_online",
        "scvb.groups 的键 = 契约 §2.4 逐字的 groups_online(与 Output 侧同名同载荷)",
    );
    eq(
        VC.STATE_UI_FIELDS.slice(),
        ["scale", "language"],
        "scvb.state 的 ui 子树键",
    );
    // 拼写纪律本身也钉一条:镜像宪法 state 字段的键不许出现 camelCase
    for (const k of [...VC.STATE_JSON_FIELDS, VC.GROUPS_JSON_KEY]) {
        check(
            !/[a-z][A-Z]/.test(k),
            `state / groups 的键 ${k} 不是 camelCase(契约 §0.2 规则①;camelCase 在 §8.3 是旧文)`,
        );
    }
    eq(VC.DIST_REQUIRES, "trackVolDb", "分布图整块降级的判据字段");
    // 三条标量的量纲逐条钉住 —— 定点解码用的就是它们,写错一个就是整排柱高错
    eq(
        VC.VIZ_TRACK_STATE_RANGE.trackPanNow,
        { lo: -100, hi: 100 },
        "panNow 值域 = 角度域",
    );
    eq(
        VC.VIZ_TRACK_STATE_RANGE.trackVolDb,
        { lo: -24, hi: 12 },
        "volDb 值域 = params-v0 的 vol 行程",
    );
    eq(
        VC.VIZ_TRACK_STATE_RANGE.trackWidthPct,
        { lo: 0, hi: 100 },
        "widthPct 值域",
    );
    eq(
        VC.PAN_NOW_PRIORITY.slice(),
        ["trackPanNow", "lanes[headColumn]", "0"],
        "panNow 取值优先级:**标量优先**(T44 对表信定的口径,不是车道优先)",
    );
    // 三条标量都必须落在 int16 里(定点 ×100 之后)
    for (const [k, r] of Object.entries(VC.VIZ_TRACK_STATE_RANGE)) {
        for (const v of [r.lo, r.hi]) {
            check(
                Math.abs(v * VC.VIZ_PAN_SCALE) <= 32767,
                `${k} 的端点 ${v} 定点后落在 int16 内`,
            );
            check(
                v * VC.VIZ_PAN_SCALE !== VC.VIZ_PAN_NONE,
                `${k} 的端点 ${v} 定点后不与哨兵相撞`,
            );
        }
    }
}

// =============================================================================
log("=== ③ viz 投影纯函数 ===");

{
    eq(
        VIZ.GROUP_LETTERS.slice(),
        ["A", "B", "C", "D", "E", "F", "G", "H"],
        "组字母 A-H",
    );
    eq(VIZ.groupOnline(0b00010011, 1), 1, "位图 bit0 = 组 A 在线");
    eq(VIZ.groupOnline(0b00010011, 3), 0, "组 C 不在线");
    eq(VIZ.groupOnline(0b00010011, 5), 1, "组 E 在线");
    eq(VIZ.groupOnline(undefined, 1), 0, "事件缺失 ⇒ 全灭、零报错");

    // ---- 轨掩码 bit{ch−1}
    const mask = MMOCK.maskOf([1, 5, 15]);
    check(VIZ.maskHas(mask, 1), "掩码含轨 1");
    check(VIZ.maskHas(mask, 5), "掩码含轨 5");
    check(VIZ.maskHas(mask, 15), "掩码含轨 15(bit14,最高位不越界)");
    check(!VIZ.maskHas(mask, 2), "掩码不含轨 2");
    check(!VIZ.maskHas(mask, 0), "轨号 0 ⇒ false");
    check(!VIZ.maskHas(mask, 16), "轨号 16 ⇒ false");
    check(mask > 0, "掩码为非负 u32");

    // ---- 覆盖位图:LSB 优先、每字 32 列。位序写反了图**照画**,只是断线位置整体
    // 错开 32 列的倍数 —— 肉眼几乎看不出来,故必须机检。
    const bits = [0, 1, 31, 32, 33, 63, 64, 1023];
    const words = new Array(VC.VIZ_COVERAGE_WORDS).fill(0);
    for (const i of bits) {
        words[i >>> 5] = (words[i >>> 5] | (1 << (i & 31))) >>> 0;
    }
    for (const i of bits)
        check(VIZ.columnCovered(words, i), `第 ${i} 列置位后读得出`);
    for (const i of [2, 30, 34, 62, 65, 1022]) {
        check(!VIZ.columnCovered(words, i), `第 ${i} 列未置位`);
    }
    check(!VIZ.columnCovered(words, 1024), "越界列(= 列数)⇒ false");
    check(!VIZ.columnCovered(null, 0), "位图缺失 ⇒ false");
    check(!VIZ.columnCovered(words, -1), "负下标 ⇒ false");
    check(
        words.every((w) => w >= 0),
        "位图字全为非负(`1<<31` 是负数,必须 >>> 0 折回 u32)",
    );

    // ---- 定点 ↔ 角度域
    eq(VIZ.panOfFixed(0), 0, "定点 0 = 居中");
    eq(VIZ.panOfFixed(10000), 100, "定点 +10000 = +100");
    eq(VIZ.panOfFixed(-10000), -100, "定点 −10000 = −100");
    eq(VIZ.panOfFixed(-5800), -58, "定点 −5800 = −58");
    eq(VIZ.panOfFixed(VC.VIZ_PAN_NONE), null, "哨兵 ⇒ null(不画)");
    eq(VIZ.panOfFixed(undefined), null, "非数 ⇒ null");
    eq(VIZ.panOfFixed(99999), 100, "越界定点夹到 +100");
    // mock 的编码与消费侧的解码必须互逆
    for (const pan of [-100, -58.4, -4, 0, 12.5, 100]) {
        near(
            VIZ.panOfFixed(MMOCK.panToFixed(pan)),
            pan,
            1 / VC.VIZ_PAN_SCALE,
            `定点往返 pan=${pan}`,
        );
    }
    eq(MMOCK.panToFixed(NaN), VC.VIZ_PAN_NONE, "非数编码成哨兵");

    // ---- 窗口跨度量化(T44:max(段末端, playhead+1, 60s) 上取整到 30s,上限 24h)
    eq(MMOCK.windowSpanS(0, 0), 60, "空工程 ⇒ 最小 60s");
    eq(MMOCK.windowSpanS(61, 0), 90, "61s 上取整到 90s(30s 边界)");
    eq(MMOCK.windowSpanS(90, 0), 90, "正好落在边界上不再抬");
    eq(MMOCK.windowSpanS(0, 200), 210, "playhead 201 上取整到 210");
    eq(MMOCK.windowSpanS(1e9, 0), 24 * 3600, "上限 24h");
    eq(MMOCK.windowSpanS(300, 42), 300, "300s 工程正好是边界值");

    // ---- 拒读理由(六种,`failed` 与 `abi` 必须可区分)
    const ONLINE = { viz: "online", fresh: true };
    const okFrame = {
        magic: VC.VIZ_MAGIC,
        abi: VC.VIZ_ABI,
        columnCount: VC.VIZ_COLUMNS,
        trackCount: VC.VIZ_TRACKS,
        panScale: VC.VIZ_PAN_SCALE,
        online: true,
        windowSpanS: 300,
        lanes: [],
        coverage: [],
    };
    eq(VIZ.vizAccepts(okFrame, ONLINE).reason, "", "合法帧:无拒读理由");
    check(VIZ.vizAccepts(okFrame, ONLINE).ok, "合法帧 ok");
    eq(VIZ.vizAccepts(null, ONLINE).reason, "shape", "null ⇒ shape");
    // **缺车道不算不可读** —— 那只影响轨迹图那一半,分布图与图例照常。
    // 早先这里判 shape,后果是整页(含分布图)一起掉进空态面板。真机截图抓到的。
    eq(
        VIZ.vizAccepts({ ...okFrame, lanes: undefined }, ONLINE).reason,
        "",
        "缺 lanes 仍然可读(只让 vizHasLanes 去管轨迹图那一半)",
    );
    eq(
        VIZ.vizAccepts({ ...okFrame, magic: "XXXX" }, ONLINE).reason,
        "magic",
        "magic 对不上 ⇒ 整帧丢弃",
    );
    eq(
        VIZ.vizAccepts({ ...okFrame, abi: VC.VIZ_ABI + 1 }, ONLINE).reason,
        "abi",
        "段比本机新 ⇒ 停止读取",
    );
    eq(
        VIZ.vizAccepts({ ...okFrame, abi: VC.VIZ_ABI + 1 }, ONLINE).abi,
        VC.VIZ_ABI + 1,
        "拒读时把对端 abi 带出来(横幅要显示)",
    );
    check(
        VIZ.vizAccepts({ ...okFrame, abi: 0 }, ONLINE).ok,
        "段比本机旧 ⇒ 照读(只拒高不拒低)",
    );
    eq(
        VIZ.vizAccepts({ ...okFrame, columnCount: 512 }, ONLINE).reason,
        "geometry",
        "列数不符 ⇒ 几何自检拒读(T44 的 kAbiMismatch 口径)",
    );
    eq(
        VIZ.vizAccepts({ ...okFrame, panScale: 10 }, ONLINE).reason,
        "geometry",
        "定点标度不符 ⇒ 几何自检拒读",
    );
    eq(
        VIZ.vizAccepts(okFrame, { viz: "offline" }).reason,
        "offline",
        "state 报 offline ⇒ 空态,不是错误",
    );
    eq(
        VIZ.vizAccepts(okFrame, { viz: "abiMismatch" }).reason,
        "abi",
        "state 报 abiMismatch ⇒ 拒连",
    );
    eq(
        VIZ.vizAccepts({ ...okFrame, online: false }, ONLINE).reason,
        "offline",
        "帧自己说 online:false ⇒ 同样落空态",
    );
    // ---- 「停更」与「掉线」必须分得开(本页最贵的一条语义:判错 = Output 还在跑、
    // 页面却把整张图清空,而且零报错)。T45 `decae38` 把 `online` / `fresh` 拆成两个
    // 字段之后,**两个信源各测一遍 + 两者的四种组合各测一遍**,不留「靠判据顺序才对」的角。
    //
    // **陈旧不挡出图**:数据还是上一份真数据,清掉会让用户看到一张突然变空的图
    const staleRes = VIZ.vizAccepts(okFrame, { viz: "online", fresh: false });
    eq(staleRes.reason, "stale", "① 只有 state 说停更 ⇒ stale");
    check(staleRes.ok, "stale 仍然 ok(图继续显示,只加一条琥珀横幅)");
    // ② **只看帧也要判得出来** —— 拆开后 `fresh` 就在帧里,不该再依赖 state 先到。
    // 传 `{}` 当 status(= `scvb.state` 还没到达的那一拍)。
    eq(
        VIZ.vizAccepts({ ...okFrame, fresh: false }, {}).reason,
        "stale",
        "② 只有帧说 fresh:false(state 还没到)⇒ 照样 stale",
    );
    eq(
        VIZ.vizAccepts({ ...okFrame, fresh: true }, {}).reason,
        "",
        "② 反面:帧说 online+fresh(state 还没到)⇒ 正常出图,不是 offline",
    );
    // ③ 四种组合(帧与 state 同步)—— 拆开之后每一格都有确定结论,与 T45 的口径表逐格对应
    for (const [online, fresh, want, why] of [
        [true, true, "", "在线且在更新"],
        [true, false, "stale", "**在线但停更** ⇒ 横幅,别清图"],
        [false, false, "offline", "掉线(段没 attach)⇒ 空态"],
        [false, true, "offline", "掉线优先于 fresh(段都没了,新鲜与否无意义)"],
    ]) {
        eq(
            VIZ.vizAccepts(
                { ...okFrame, online, fresh },
                { viz: online ? "online" : "offline", fresh },
            ).reason,
            want,
            `③ online=${online} fresh=${fresh} ⇒ ${want || "可读"}(${why})`,
        );
    }
    // ④ 两个事件谁先到没有保证:state 已报「在线但停更」、帧还停在上一拍的 `online:false`
    // 时,该显示的是横幅 + 图,不是把图清空。(这一路同时兼容**拆分之前**的桥 ——
    // 那时停摆帧里也是 `online:false`,只有 `scvb.state` 分得出来。)
    eq(
        VIZ.vizAccepts(
            { ...okFrame, online: false },
            {
                viz: "online",
                fresh: false,
            },
        ).reason,
        "stale",
        "④ 帧 online:false + state 说在线不新鲜 ⇒ **stale**,不是 offline",
    );
    // ⑤ **留存帧不许压过 native 的段级事实**。`store.frame` 永不清空(它同时是车道缓存),
    // 于是「先停更、再退出」这条 T45 的真实检测路径会在页面手里留下一帧
    // `online:true, fresh:false`:段都没了,它却让 `frameStale` 继续成立 ⇒ 页面**永久**
    // 停在琥珀横幅上,而正确画面是空态。段级存活只有 native 分得出,故 state 的 offline
    // 是更强的事实,一票压过留存帧。场景 `?scenario=monitor-stall-then-gone` 在页面级复现。
    eq(
        VIZ.vizAccepts(
            { ...okFrame, online: true, fresh: false },
            { viz: "offline", fresh: false },
        ).reason,
        "offline",
        "⑤ 留存的停更帧 + state 报 offline ⇒ **offline**(不是停在停更横幅上)",
    );
    eq(
        VIZ.vizAccepts({ ...okFrame, online: true, fresh: false }, {}).reason,
        "stale",
        "⑤ 反面:state 还没说话时同一帧仍是 stale(闸只对 offline 开,别开大)",
    );
    eq(
        VIZ.vizAccepts({ ...okFrame, online: false }, { viz: "abiMismatch" })
            .reason,
        "abi",
        "abiMismatch 压过帧里的 online(拒连是更强的结论)",
    );
    eq(
        VIZ.vizAccepts({ ...okFrame, windowSpanS: 0 }, ONLINE).reason,
        "window",
        "窗口跨度 0(未 prepare)⇒ 空态",
    );
    check(VIZ.vizIsEmptyState("offline"), "offline 归空态");
    check(VIZ.vizIsEmptyState("window"), "window 归空态");
    check(!VIZ.vizIsEmptyState("abi"), "abi **不**归空态(要红横幅)");
    check(!VIZ.vizIsEmptyState("geometry"), "geometry 不归空态");
    check(!VIZ.vizIsEmptyState("stale"), "stale 不归空态(图还在)");

    // ---- 车道有无:三种「没有线」必须分得开(桥没送 / 真没分段 / 组不在线)
    check(VIZ.vizHasLanes(okFrame), "带 lanes+coverage ⇒ hasLanes");
    check(
        !VIZ.vizHasLanes({ ...okFrame, lanes: undefined }),
        "桥没送车道 ⇒ !hasLanes(轨迹图走「未接通」而不是「尚无分段」)",
    );
    check(!VIZ.vizHasLanes(null), "空帧 ⇒ !hasLanes");

    // ---- playhead flags 与 §2.6 形状归一
    eq(
        VIZ.vizFlags({ playheadFlags: 0b101 }),
        { isPlaying: true, looping: false, loopValid: true },
        "flags 三位分别解出来",
    );
    eq(
        VIZ.vizFlags({}),
        { isPlaying: false, looping: false, loopValid: false },
        "缺 flags ⇒ 全 false",
    );
    eq(
        VIZ.vizPlayheadEvent({ playheadS: 12.5, playheadFlags: 1 }),
        { timeS: 12.5, isPlaying: true, inRange: true },
        "viz 帧 → §2.6 扁平标量集",
    );
    eq(
        VIZ.vizPlayheadEvent({ playheadS: null }),
        null,
        "无时间线 ⇒ null(不把竖线钉在 0 秒)",
    );
    eq(VIZ.vizPlayheadEvent({ playheadS: -1 }), null, "−1 的投影同样是 null");

    // ---- 时长与列宽
    eq(
        VIZ.vizDurationS({ windowStartS: 0, windowSpanS: 300 }),
        300,
        "时长 = 窗口末端",
    );
    eq(
        VIZ.vizDurationS({ windowStartS: 60, windowSpanS: 300 }),
        360,
        "窗口起点参与",
    );
    eq(
        VIZ.vizDurationS({}),
        VIZ.VIZ_FALLBACK_DURATION_S,
        "无证据 ⇒ 兜底(不塌到 0)",
    );
    near(
        VIZ.vizColumnS({ windowSpanS: 1024 }),
        1,
        1e-9,
        "1024s 窗口 ⇒ 每列 1s",
    );
    eq(VIZ.vizColumnS({}), 0, "无窗口 ⇒ 列宽 0(调用方据此不画)");

    // ---- 轨色索引
    const colorFrame = { colorIndex: [3, 0, undefined, 99] };
    eq(VIZ.colorChOf(colorFrame, 1), 3, "给了合法色号就用色号");
    eq(VIZ.colorChOf(colorFrame, 2), 2, "色号 0(未指定)⇒ 回落轨号");
    eq(VIZ.colorChOf(colorFrame, 3), 3, "色号缺失 ⇒ 回落轨号");
    eq(VIZ.colorChOf(colorFrame, 4), 4, "色号越界 ⇒ 回落轨号");
    eq(VIZ.colorChOf({}, 7), 7, "整张表缺失 ⇒ 回落轨号");

    // ---- 播放头落在第几列
    const winFrame = { windowStartS: 0, windowSpanS: 1024, playheadS: 10.5 };
    eq(VIZ.playheadColumn(winFrame), 10, "10.5s / 1s 每列 ⇒ 第 10 列");
    eq(
        VIZ.playheadColumn({ ...winFrame, playheadS: null }),
        -1,
        "无时间线 ⇒ −1",
    );
    eq(
        VIZ.playheadColumn({ ...winFrame, playheadS: 9999 }),
        -1,
        "播放头在窗口外 ⇒ −1",
    );
}

{
    // ---- 车道三件的按需重发合并(lane_revision 语义)
    const full = {
        group_id: 1,
        laneRevision: 5,
        lanes: [[1, 2]],
        coverage: [[3]],
        colorIndex: [1],
        seq: 2,
    };
    const headOnly = { group_id: 1, laneRevision: 5, seq: 4 };
    const merged = VIZ.mergeVizFrame(full, headOnly);
    eq(merged.lanes, full.lanes, "revision 一致 ⇒ 沿用缓存车道");
    eq(merged.seq, 4, "帧头取新的");
    eq(merged.colorIndex, full.colorIndex, "轨色也沿用");

    const bumped = VIZ.mergeVizFrame(full, {
        group_id: 1,
        laneRevision: 6,
        seq: 6,
    });
    check(
        bumped.lanes === undefined,
        "revision 变了却没带车道 ⇒ **不拿旧车道配新帧头**(宁可空)",
    );
    const other = VIZ.mergeVizFrame(full, {
        group_id: 2,
        laneRevision: 5,
        seq: 6,
    });
    check(other.lanes === undefined, "换组 ⇒ 绝不沿用上一组的车道");
    const fresh = VIZ.mergeVizFrame(null, headOnly);
    check(fresh.lanes === undefined, "没缓存 ⇒ 原样返回(落空态等重算)");
    const carry = VIZ.mergeVizFrame(full, {
        group_id: 1,
        laneRevision: 6,
        seq: 8,
        lanes: [[9]],
        coverage: [[1]],
    });
    eq(carry.lanes, [[9]], "新帧自带车道 ⇒ 原样用");
}

// =============================================================================
log("=== ④ 两图渲染的数值正确性(对 fixture 逐轨逐列断言)===");

// 造一个**完全确定**的小世界:窗口 1024s、每列 1s,于是「第 i 列」= 「第 i 秒」,
// 断口位置与时间坐标可以逐个数出来。
function makeFrame(over = {}) {
    const lanes = [];
    const coverage = [];
    for (let t = 0; t < VC.VIZ_TRACKS; t++) {
        lanes.push(new Array(VC.VIZ_COLUMNS).fill(VC.VIZ_PAN_NONE));
        coverage.push(new Array(VC.VIZ_COVERAGE_WORDS).fill(0));
    }
    return {
        magic: VC.VIZ_MAGIC,
        abi: VC.VIZ_ABI,
        generation: 1,
        columnCount: VC.VIZ_COLUMNS,
        trackCount: VC.VIZ_TRACKS,
        panScale: VC.VIZ_PAN_SCALE,
        online: true,
        seq: 2,
        laneRevision: 1,
        publishMs: 1000,
        windowStartS: 0,
        windowSpanS: VC.VIZ_COLUMNS, // 每列恰好 1 秒
        playheadS: 0,
        playheadFlags: 0,
        onlineMask: 0,
        coveredMask: 0,
        stereoMask: 0,
        colorIndex: Array.from({ length: VC.VIZ_TRACKS }, (_, i) => i + 1),
        lanes,
        coverage,
        ...over,
    };
}

/** 在 frame 上给某轨的 [from,to) 列填上覆盖与 pan。 */
function paint(frame, ch, from, to, pan) {
    const lane = frame.lanes[ch - 1];
    const words = frame.coverage[ch - 1];
    for (let i = from; i < to; i++) {
        lane[i] = MMOCK.panToFixed(pan);
        words[i >>> 5] = (words[i >>> 5] | (1 << (i & 31))) >>> 0;
    }
    frame.onlineMask = (frame.onlineMask | (1 << (ch - 1))) >>> 0;
    frame.coveredMask = (frame.coveredMask | (1 << (ch - 1))) >>> 0;
}

{
    // 轨 1:[0,10) pan −30,[10,20) pan +40(相接 ⇒ 同一条折线走台阶);
    //       [30,40) pan 0(隔了 10 列 ⇒ 另起一条)。
    const f = makeFrame();
    paint(f, 1, 0, 10, -30);
    paint(f, 1, 10, 20, 40);
    paint(f, 1, 30, 40, 0);

    const series = VIZ.vizSeries(f);
    eq(series.length, 1, "只有轨 1 有数据 ⇒ 一条线");
    eq(series[0].ch, 1, "轨号 1");
    eq(series[0].runs.length, 2, "两段覆盖之间有缺口 ⇒ 断成两条折线");

    const r0 = series[0].runs[0];
    eq(r0[0], { tS: 0, pan: -30 }, "第一条折线起点 = 0s / −30");
    eq(r0[1], { tS: 1, pan: -30 }, "每列一个水平台阶(0→1s 同 pan)");
    // 相接处必须是**竖直台阶**:同一 tS 上两个不同 pan
    eq(r0[19], { tS: 10, pan: -30 }, "第 10 列前的最后一点仍是 −30");
    eq(
        r0[20],
        { tS: 10, pan: 40 },
        "同一时刻 10s 上跳到 +40(竖直台阶,不画斜线)",
    );
    eq(r0[r0.length - 1], { tS: 20, pan: 40 }, "第一条折线止于 20s");
    eq(r0.length, 40, "20 列 × 每列两点 = 40 点");

    const r1 = series[0].runs[1];
    eq(r1[0], { tS: 30, pan: 0 }, "第二条折线从 30s 起(缺口 20–30s 不画线)");
    eq(r1[r1.length - 1], { tS: 40, pan: 0 }, "第二条折线止于 40s");

    // 时间恒单调不减
    for (const run of series[0].runs) {
        check(
            run.every((p, i) => i === 0 || p.tS >= run[i - 1].tS),
            "折线时间单调不减",
        );
    }

    // ---- **断线只能看位图**:车道有值而位图为 0 的列不许画线。
    // 这正是 T44 的「CurveEvaluator 会填补空隙,只有分段表才是覆盖真源」。
    const holdFrame = makeFrame();
    paint(holdFrame, 1, 0, 10, -30);
    paint(holdFrame, 1, 30, 40, 0);
    for (let i = 10; i < 30; i++) {
        holdFrame.lanes[0][i] = MMOCK.panToFixed(-30); // 曲线 hold:车道有值
    }
    const held = VIZ.vizSeries(holdFrame);
    eq(
        held[0].runs.length,
        2,
        "缺口列的车道有值(曲线 hold),但位图为 0 ⇒ 仍然断线",
    );
    eq(held[0].runs[0][held[0].runs[0].length - 1].tS, 10, "第一条仍止于 10s");

    // ---- 哨兵列即使位图置了 1 也当没数据(两者矛盾时以「没数据」为准)
    const sentFrame = makeFrame();
    paint(sentFrame, 2, 0, 10, 20);
    sentFrame.lanes[1][5] = VC.VIZ_PAN_NONE;
    const sent = VIZ.vizSeries(sentFrame);
    eq(
        sent[0].runs.length,
        2,
        "中间一列是哨兵 ⇒ 断成两条(不画一条 −327.68 的线)",
    );
    check(
        sent[0].runs.every((run) =>
            run.every((p) => p.pan >= TC.PAN_MIN && p.pan <= TC.PAN_MAX),
        ),
        "所有点都落在 −100..100 角度域内",
    );

    // ---- onlineMask:未启用的轨不画,也不进图例
    const offFrame = makeFrame();
    paint(offFrame, 3, 0, 5, 10);
    offFrame.onlineMask = 0; // 明确关掉
    eq(VIZ.vizSeries(offFrame).length, 0, "onlineMask 为 0 ⇒ 一条线都不画");
    eq(VIZ.vizLegendRows(offFrame).length, 0, "也不进图例");

    // ---- 轨号升序 + 轨色索引改写轨号
    const multi = makeFrame();
    paint(multi, 9, 0, 5, 0);
    paint(multi, 2, 0, 5, 0);
    paint(multi, 5, 0, 5, 0);
    eq(
        VIZ.vizSeries(multi).map((s) => s.ch),
        [2, 5, 9],
        "按轨号升序(色板按轨号固定映射,顺序不许乱)",
    );
    const recolor = makeFrame({
        colorIndex: [7, 6, 5, 4, 3, 2, 1, 8, 9, 10, 11, 12, 13, 14, 15],
    });
    paint(recolor, 1, 0, 5, 0);
    eq(VIZ.vizSeries(recolor)[0].ch, 7, "colorIndex 改写了取色用的轨号");

    // ---- 立体声位来自 stereoMask
    const st = makeFrame({ stereoMask: MMOCK.maskOf([3]) });
    paint(st, 3, 0, 5, 0);
    paint(st, 4, 0, 5, 0);
    eq(
        VIZ.vizSeries(st).map((s) => s.stereo),
        [true, false],
        "stereoMask 决定 ST 身份(轨 3 是、轨 4 不是)",
    );
}

{
    // ---- 分布图:数据源 = VizTrackState 三条定点标量 + VizTrackLabels + 三张掩码。
    // 段里每一块都是**定长 15、下标即轨号**,故这里也按下标填。
    // 三条标量**桥已经解码成工程量、哨兵已折成 null**(T45 buildVizPayload),
    // 故这里直接填原值 —— 不是定点。
    const fill15 = (pairs) => {
        const a = new Array(VC.VIZ_TRACKS).fill(null);
        for (const [ch, v] of pairs) a[ch - 1] = v;
        return a;
    };
    const f = makeFrame({ playheadS: 7.5 });
    paint(f, 1, 0, 20, -30);
    paint(f, 2, 0, 20, 60);
    f.trackVolDb = fill15([
        [1, -6],
        [2, -12],
    ]);
    f.trackWidthPct = fill15([
        [1, 100],
        [2, 82],
    ]);
    // panNow **刻意与车道不同值**:才验得出「标量优先、不是车道优先」
    f.trackPanNow = fill15([
        [1, -25],
        [2, 55],
    ]);
    f.trackLabels = ["主唱", "和声"].concat(
        new Array(VC.VIZ_TRACKS - 2).fill(""),
    );
    f.stereoMask = MMOCK.maskOf([2]);
    f.leadMask = MMOCK.maskOf([1]);

    const rows = VIZ.vizDistRows(f);
    eq(rows.length, 2, "两轨都画柱");
    // T44 对表信定的口径:panNow 是**播放头精确时刻**的求值,车道是列中心点采样。
    // 分布图要「此刻」⇒ 标量优先。写反了在放大档下柱与播放头对不上,而且看起来正常。
    eq(rows[0].pan, -25, "横位取 trackPanNow(−25),**不是**车道第 7 列的 −30");
    eq(rows[1].pan, 55, "轨 2 同理:标量 55 而非车道 60");
    eq(rows[0].volDb, -6, "柱高来自 trackVolDb");
    eq(rows[1].widthPct, 82, "张开线来自 trackWidthPct");
    eq(rows[0].lead, true, "柱顶绿帽来自 leadMask");
    eq(rows[1].lead, false, "非 lead 轨不戴帽");
    eq(rows[1].stereo, true, "立体声位来自 stereoMask");

    // 标量缺席 ⇒ 回落到播放头所在列的车道点采样
    const sentinelPan = { ...f, trackPanNow: fill15([[2, 55]]) }; // 轨 1 缺席
    eq(
        VIZ.vizDistRows(sentinelPan)[0].pan,
        -30,
        "trackPanNow 缺席 ⇒ 回落车道第 7 列(−30)",
    );
    // 标量哨兵 + 播放头在窗口外 ⇒ 才落到 0
    eq(
        VIZ.vizDistRows({ ...sentinelPan, playheadS: 9999 })[0].pan,
        0,
        "两条都拿不到 ⇒ 0(居中,不撒谎的默认)",
    );

    // ---- 整块降级:trackVolDb 缺失 ⇒ 分布图画空,不猜、不填 0
    const noState = { ...f, trackVolDb: undefined };
    eq(
        VIZ.vizDistRows(noState),
        [],
        "缺 trackVolDb ⇒ 分布图 rows 为空(不造幽灵柱)",
    );
    eq(
        DC.distBarsHtml(VIZ.vizDistRows(noState), 0),
        "",
        "拼串也是空(页面上是一张空图,不是一排居中的假柱)",
    );
    eq(
        VIZ.vizSeries(noState).map((s) => s.ch),
        [1, 2],
        "轨迹图**不受影响**照常画(降级只砍分布图这一半)",
    );
    eq(
        VIZ.vizLegendRows(noState).map((r) => r.ch),
        [1, 2],
        "图例仍列轨迹图画了的轨",
    );
    const noLabels = { ...noState, trackLabels: undefined };
    eq(
        VIZ.vizLegendRows(noLabels).map((r) => r.label),
        ["", ""],
        "缺 trackLabels ⇒ 空串(图例只显示两位轨号,不显示 undefined)",
    );

    // ---- leadMask 缺失(T44 已落地;此处测的是它缺席时的降级)⇒ 柱照画、不戴绿帽
    const noLead = { ...f, leadMask: undefined };
    eq(
        VIZ.vizDistRows(noLead).map((r) => r.lead),
        [false, false],
        "缺 leadMask ⇒ 一律不戴绿帽(少个信息 好过 标错主唱)",
    );
    eq(VIZ.vizDistRows(noLead).length, 2, "缺 leadMask 不影响柱本身");
    check(
        !DC.distBarsHtml(VIZ.vizDistRows(noLead), 0).includes('data-lead="1"'),
        "拼串里没有一个 data-lead=1",
    );

    // ---- 单轨的 volDb 缺席 ⇒ 那一轨不画(与 Tab1「空闲轨不画幽灵柱」同一纪律)
    const partial = { ...f, trackVolDb: fill15([[2, -12]]) }; // 轨 1 缺席
    eq(
        VIZ.vizDistRows(partial).map((r) => r.ch),
        [2],
        "volDb 缺席的那一轨不画柱",
    );
    // 0 dB **不是**哨兵 —— 0 是合法音量,不许被当成「没有数据」
    const zeroDb = { ...f, trackVolDb: fill15([[1, 0]]) };
    eq(
        VIZ.vizDistRows(zeroDb).map((r) => [r.ch, r.volDb]),
        [[1, 0]],
        "volDb = 0 dB 是合法值,照画(缺席才是没有数据)",
    );
    // width = 0 同理:0 是合法宽度,回落 100 只在**哨兵**时发生
    const zeroWidth = {
        ...f,
        trackVolDb: fill15([[1, -6]]),
        trackWidthPct: fill15([[1, 0]]),
    };
    eq(VIZ.vizDistRows(zeroWidth)[0].widthPct, 0, "width = 0 照用,不回落 100");
    const noWidth = {
        ...f,
        trackVolDb: fill15([[1, -6]]),
        trackWidthPct: undefined,
    };
    eq(
        VIZ.vizDistRows(noWidth)[0].widthPct,
        100,
        "width 整块缺 ⇒ 回落 100(不张开)",
    );

    // ---- 定点解码的边界
    eq(
        VIZ.trackScalar(f, "trackVolDb", 1),
        -6,
        "trackScalar 取 dB(桥已解码,这里只夹取)",
    );
    eq(VIZ.trackScalar(f, "trackVolDb", 99), null, "轨号越界 ⇒ null");
    eq(VIZ.trackScalar(f, "nope", 1), null, "未知字段 ⇒ null");
    eq(VIZ.trackScalar({}, "trackVolDb", 1), null, "整块缺 ⇒ null");
    eq(VIZ.trackLabel(f, 1), "主唱", "轨名解码");
    eq(VIZ.trackLabel({}, 1), "", "缺轨名表 ⇒ 空串,不是 undefined");
    eq(VIZ.fixedToUnit(1200, -24, 12), 12, "定点 1200 = +12 dB");
    eq(VIZ.fixedToUnit(-2400, -24, 12), -24, "定点 −2400 = −24 dB");
    eq(VIZ.fixedToUnit(99999, -24, 12), 12, "越界定点夹到上限");
    eq(VIZ.fixedToUnit(VC.VIZ_PAN_NONE, -24, 12), null, "哨兵 ⇒ null,不是 0");
    eq(VIZ.fixedToUnit(0, -24, 12), 0, "0 是合法值,不是 null");

    // ---- 图例 = 两图并集(两图同屏,跟着任一张都会出现「有它却找不到」)
    const union = makeFrame({ playheadS: 1 });
    paint(union, 1, 0, 5, 0); // 只有轨迹
    union.trackVolDb = fill15([[4, -9]]);
    union.trackWidthPct = fill15([[4, 100]]);
    union.trackLabels = new Array(VC.VIZ_TRACKS).fill("");
    union.trackLabels[3] = "只有柱";
    union.onlineMask = (union.onlineMask | (1 << 3)) >>> 0;
    eq(
        VIZ.vizSeries(union).map((s) => s.ch),
        [1],
        "轨迹图只画轨 1",
    );
    eq(
        VIZ.vizDistRows(union).map((r) => r.ch),
        [4],
        "分布图只画轨 4",
    );
    eq(
        VIZ.vizLegendRows(union).map((r) => r.ch),
        [1, 4],
        "图例 = 并集(图例里有它 ⇒ 屏幕上找得到它)",
    );

    // ---- 数值真的能喂进共享件并画出对应数量的柱
    const html = DC.distBarsHtml(rows, 0);
    eq(
        (html.match(/class="dist-bar"/g) || []).length,
        2,
        "两根柱(不是「有 HTML 就算过」)",
    );
    eq(
        (html.match(/class="dist-span"/g) || []).length,
        1,
        "一条张开线(只有立体声那一轨)",
    );
    check(
        html.includes("--tc:var(--track-color-1)"),
        "轨 1 的柱用 --track-color-1",
    );
}

// =============================================================================
log("=== ⑤ mock 端到端(真桥 + mock 后端)===");

{
    eq(
        MBRIDGE.MONITOR_FUNCTIONS.slice(),
        [
            "requestInitialState",
            "setObservedGroup",
            "setUiScale",
            "commitUiScale",
            "setLang",
        ],
        "Monitor 侧函数名表(临时形制,待随 T44/T45 转正进契约 §7)",
    );
    eq(
        MBRIDGE.MONITOR_EVENTS.slice(),
        ["scvb.state", "scvb.groups", "scvb.viz", "scvb.playhead"],
        "Monitor 侧事件名表(逐字对齐 T45 的 MonitorBridgeApi.h)",
    );
    const WRITE_NAMES = [
        "setGroupId",
        "setParam",
        "setCaptureEnabled",
        "setOutputEnabled",
        "analyze",
        "editSegment",
        "setRange",
        "undo",
        "redo",
    ];
    for (const n of WRITE_NAMES) {
        check(
            !MBRIDGE.MONITOR_FUNCTIONS.includes(n),
            `名表里没有写操作 ${n}(Monitor 是纯只读)`,
        );
    }
}

{
    const s = MMOCK.createPreviewSession({
        params: "?scenario=monitor-online",
    });
    const bridge = MBRIDGE.createMonitorBridge({ mockBackend: s.mock });
    check(bridge.isPreview === true, "mock 后端 ⇒ isPreview");
    eq(bridge.role, "monitor", "role = monitor");

    let lastViz = null;
    let lastState = null;
    bridge.on("scvb.viz", (v) => {
        lastViz = v;
    });
    bridge.on("scvb.state", (v) => {
        lastState = v;
    });
    let threw = false;
    try {
        bridge.on("scvb.meters", () => {});
    } catch {
        threw = true;
    }
    check(threw, "订阅表外事件名当场抛错");

    const snap = await bridge.requestInitialState();
    check(!!snap, "首帧快照拿得到");
    eq(snap.group_id, 1, "monitor-online 场景开箱观察组 A");
    eq(snap.groups_online, 0b00010011, "在线组位图 = A/B/E");

    const v = snap.viz;
    const ST = s.ctl.statePayload();
    eq(VIZ.vizAccepts(v, ST).reason, "", "快照自带首帧 viz 且可读");
    eq(v.online, true, "帧里 online = 段已 attach 且可读");
    eq(v.fresh, true, "帧里 fresh = 帧还在更新(与 online 各是一件事)");
    eq(v.group_id, 1, "帧自带组号(换组时用来丢在途帧)");
    eq(ST.viz, "online", "scvb.state 报 online");
    eq(ST.fresh, true, "且新鲜");
    // 帧与 state 是同一组事实的两次投影 —— 同一拍里必须同值,不然消费侧两个信源打架
    eq(v.online, ST.viz === "online", "帧的 online 与 state 的三态同步");
    eq(v.fresh, ST.fresh, "帧的 fresh 与 state 的 fresh 同步");
    eq(v.group_id, ST.group_id, "帧的组号与 state 的组回显同步");
    eq(
        s.ctl.groupsPayload(),
        { groups_online: 0b00010011 },
        "scvb.groups 载荷 = 契约 §2.4 逐字的 `{ groups_online: u8 }`",
    );
    eq(v.columnCount, VC.VIZ_COLUMNS, "几何:列数");
    eq(v.trackCount, VC.VIZ_TRACKS, "几何:轨数");
    eq(v.panScale, VC.VIZ_PAN_SCALE, "几何:定点标度");
    eq(v.seq % 2, 0, "对外的 seq 恒为偶数(奇数 = 写入中,不该被读方看到)");

    // ---- 段内三张表恒为**满 15 轨**定长(这是段的形状,mock 不许只给 N 轨)
    eq(v.lanes.length, VC.VIZ_TRACKS, "车道表 15 行");
    eq(v.coverage.length, VC.VIZ_TRACKS, "位图表 15 行");
    eq(v.colorIndex.length, VC.VIZ_TRACKS, "轨色表 15 项");
    for (let t = 0; t < VC.VIZ_TRACKS; t++) {
        eq(v.lanes[t].length, VC.VIZ_COLUMNS, `轨 ${t + 1} 的车道 1024 列`);
        eq(
            v.coverage[t].length,
            VC.VIZ_COVERAGE_WORDS,
            `轨 ${t + 1} 的位图 32 字`,
        );
        check(
            v.coverage[t].every((w) => w >= 0 && Number.isInteger(w)),
            `轨 ${t + 1} 的位图字是非负整数(u32 口径)`,
        );
        check(
            v.lanes[t].every(
                (x) =>
                    x === VC.VIZ_PAN_NONE ||
                    (Number.isInteger(x) &&
                        Math.abs(x) <= 100 * VC.VIZ_PAN_SCALE),
            ),
            `轨 ${t + 1} 的车道值全是合法定点或哨兵`,
        );
    }
    eq(
        v.colorIndex,
        Array.from({ length: 15 }, (_, i) => i + 1),
        "v1 的轨色索引恒 = 轨号(T44 段内注释)",
    );

    // ---- 掩码与 fixture 对得上
    eq(
        v.onlineMask,
        MMOCK.maskOf(Array.from({ length: 15 }, (_, i) => i + 1)),
        "组 A 满配 ⇒ onlineMask 15 位全 1",
    );
    const MD = await import(u("web/shared/mock-data.js"));
    eq(
        v.stereoMask,
        MMOCK.maskOf(MD.DEMO_STEREO_CHANNELS.slice()),
        "stereoMask 与 fixture 的立体声轨逐位一致",
    );

    // ---- 窗口:demo 是 5 分钟工程,量化后恰好 300s
    eq(v.windowStartS, 0, "窗口起点 = 工程起点(v1 恒 0)");
    eq(v.windowSpanS, 300, "窗口跨度 = 300s(5 分钟,正好落在 30s 边界上)");
    near(v.playheadS, 42, 0.01, "播放头 = 42s(fixture 的走带位置)");
    eq(v.sampleRate, 48000, "采样率(换算式的分母)");

    // ---- 投影出来的两张图与 fixture 对得上
    const series = VIZ.vizSeries(v);
    eq(series.length, 15, "组 A = 15 轨全有线");
    eq(
        series.map((x) => x.ch),
        Array.from({ length: 15 }, (_, i) => i + 1),
        "轨号 1..15 升序",
    );
    eq(
        series.filter((x) => x.stereo).map((x) => x.ch),
        MD.DEMO_STEREO_CHANNELS.slice(),
        "立体声轨与 fixture 一致",
    );
    check(
        series.every((x) => x.runs.length > 1),
        "每轨都断成多条(乐句之间本来就没段 —— 断线真的在发生)",
    );
    // 每条折线的时间坐标都落在窗口内
    const end = v.windowStartS + v.windowSpanS;
    check(
        series.every((x) =>
            x.runs.every((run) =>
                run.every((p) => p.tS >= v.windowStartS && p.tS <= end + 1e-6),
            ),
        ),
        "所有折线点都落在窗口 [0, 300] 内",
    );
    // 位图里确实有 0 位(不是一整条铺满 ⇒ 断线不是装出来的)
    check(
        v.coverage.some((w) => w.some((x) => x !== 0xffffffff)),
        "位图里有 0 位 ⇒ 图上真的会断线",
    );

    eq(VIZ.vizDistRows(v).length, 15, "15 根柱");
    eq(VIZ.vizLegendRows(v).length, 15, "图例 15 行");
    eq(
        VIZ.vizLegendRows(v).map((r) => r.label),
        MD.DEMO_LABELS.slice(),
        "图例轨名与 fixture 的 15 条轨名逐字一致",
    );

    // ---- 组切换往返(数值级:轨号集合逐项)
    const ok = await bridge.setObservedGroup(2);
    eq(ok, { ok: true }, "切到组 B 受理");
    // 组回显走 `scvb.state`;帧里的 `group_id` 是另一件事(丢在途帧的判据)
    check(!!lastState && lastState.group_id === 2, "scvb.state 回显新组号");
    eq(lastViz.group_id, 2, "新组的帧自带新组号");
    check(Array.isArray(lastViz.lanes), "切组那一帧**必带车道**(换组 = 换段)");
    eq(
        VIZ.vizSeries(lastViz).map((x) => x.ch),
        [1, 2, 3, 4, 5, 6],
        "组 B = 6 轨小编制,轨号逐项对上",
    );
    eq(
        lastViz.onlineMask,
        MMOCK.maskOf([1, 2, 3, 4, 5, 6]),
        "组 B 的 onlineMask",
    );

    await bridge.setObservedGroup(5);
    eq(
        VIZ.vizSeries(lastViz).map((x) => x.ch),
        [1, 2, 7, 8, 9, 10, 11, 12, 13],
        "组 E = 9 轨且轨号不连续(切换必定看得出来)",
    );

    const backA = await bridge.setObservedGroup(1);
    eq(backA, { ok: true }, "切回组 A");
    eq(VIZ.vizSeries(lastViz).length, 15, "组 A 又是 15 轨");
    eq(
        await bridge.setObservedGroup(9),
        { ok: false, reason: "badArg" },
        "组号越界 ⇒ badArg",
    );
    eq(
        await bridge.setObservedGroup("A"),
        { ok: false, reason: "badArg" },
        "非整数组号 ⇒ badArg",
    );

    // ---- UI 偏好三件(写的是本实例自己的事,不违反只读)
    eq(await bridge.setLang("fr"), { ok: true }, "setLang 受理");
    eq(
        await bridge.setLang("de"),
        { ok: false, reason: "badArg" },
        "表外语言 ⇒ badArg",
    );
    eq(await bridge.setUiScale(1.5), { ok: true }, "setUiScale 受理");
    eq(
        await bridge.setUiScale(-1),
        { ok: false, reason: "badArg" },
        "非法档位 ⇒ badArg",
    );
    eq(await bridge.commitUiScale(), { ok: true }, "commitUiScale 受理");

    s.stop();
}

{
    // ---- 空态:观察的组没有 Output(attach = failed;J75 C「组不在线显示空态」)
    const s = MMOCK.createPreviewSession({
        params: "?scenario=monitor-offline",
    });
    const bridge = MBRIDGE.createMonitorBridge({ mockBackend: s.mock });
    const snap = await bridge.requestInitialState();
    eq(snap.group_id, 3, "monitor-offline 场景观察组 C");
    eq(s.ctl.statePayload().viz, "offline", "scvb.state 报 offline");
    const a = VIZ.vizAccepts(snap.viz, s.ctl.statePayload());
    check(!a.ok, "不在线 ⇒ 不可读");
    eq(a.reason, "offline", "理由是 offline(空态,不是错误)");
    check(VIZ.vizIsEmptyState(a.reason), "归空态,不挂红横幅");
    eq(snap.viz.online, false, "**仍然发了一帧**且说清了原因");
    eq(snap.viz.windowSpanS, 0, "attach 不上时窗口为 0");
    eq(snap.viz.onlineMask, 0, "掩码全 0");
    eq(VIZ.vizSeries(snap.viz).length, 0, "空态无折线");
    eq(VIZ.vizDistRows(snap.viz).length, 0, "空态无柱");
    s.stop();
}

{
    // ---- 拒连:段 abi 高于本机 ⇒ 红横幅 + 停止读取(与空态**必须可区分**)
    const s = MMOCK.createPreviewSession({ params: "?scenario=monitor-abi" });
    const bridge = MBRIDGE.createMonitorBridge({ mockBackend: s.mock });
    const snap = await bridge.requestInitialState();
    eq(s.ctl.statePayload().viz, "abiMismatch", "scvb.state 报 abiMismatch");
    const a = VIZ.vizAccepts(snap.viz, s.ctl.statePayload());
    eq(a.reason, "abi", "⇒ 拒读");
    check(
        !VIZ.vizIsEmptyState(a.reason),
        "**不**归空态 —— 这是拒连,不是没数据",
    );
    s.stop();
}

{
    // ---- 重连:开箱 attach 失败,Output 上线后自动出图(不需要用户点任何东西)
    const s = MMOCK.createPreviewSession({
        params: "?scenario=monitor-reconnect",
    });
    const bridge = MBRIDGE.createMonitorBridge({ mockBackend: s.mock });
    let last = null;
    bridge.on("scvb.viz", (v) => {
        last = v;
    });
    const snap = await bridge.requestInitialState();
    eq(
        VIZ.vizAccepts(snap.viz, s.ctl.statePayload()).reason,
        "offline",
        "开箱:Output 还没起来 ⇒ 空态",
    );
    const gen0 = snap.viz.generation;

    // driver 的 3 秒定时器在 node 侧不跑(没 start),直接调 ctl 的等价入口
    s.ctl.bringOutputUp();
    check(!!last, "Output 上线时立刻推了一帧");
    eq(s.ctl.statePayload().viz, "online", "state 翻成 online");
    eq(VIZ.vizAccepts(last, s.ctl.statePayload()).reason, "", "重连后可读");
    eq(last.online, true, "帧里也翻成 online");
    check(Array.isArray(last.lanes), "重连帧**必带车道**(段是新建的)");
    check(
        last.generation > gen0,
        `段被重建 ⇒ generation 前进(${gen0} → ${last.generation})`,
    );
    eq(VIZ.vizSeries(last).length, 15, "重连后 15 轨全回来");

    // 重连帧经 mergeVizFrame 也不会拿到旧车道(上一帧压根没有车道)
    const merged = VIZ.mergeVizFrame(snap.viz, last);
    eq(VIZ.vizSeries(merged).length, 15, "合并后照样是 15 轨");
    s.stop();
}

{
    // ---- 停摆:publishMs 冻住(事件照发,停的是时刻)
    const s = MMOCK.createPreviewSession({
        params: "?scenario=monitor-stalled",
    });
    const bridge = MBRIDGE.createMonitorBridge({ mockBackend: s.mock });
    const snap = await bridge.requestInitialState();
    const st = s.ctl.statePayload();
    eq(st.viz, "online", "段仍在线");
    eq(st.fresh, false, "但帧不新鲜(native 侧松开映射再探一次判出来的)");
    const a = VIZ.vizAccepts(snap.viz, st);
    eq(a.reason, "stale", "⇒ stale");
    check(
        a.ok,
        "**仍然 ok** —— 图继续显示上一份真数据,只加一条琥珀横幅(挡掉会让用户看到一张突然变空的图)",
    );
    // 载荷形状:停摆帧与掉线帧**必须长得不一样**。拆开之前两者都是 `online:false`,
    // 完全同形 —— 那时能不能画对,全押在消费侧的判据顺序上。现在形状本身就分得开。
    eq(
        snap.viz.online,
        true,
        "帧里 online **仍是 true**(段还在、只是不发帧了)",
    );
    eq(snap.viz.fresh, false, "分开的那一位:fresh = false");
    eq(
        VIZ.vizAccepts(snap.viz, {}).reason,
        "stale",
        "**不看 state、只看帧**也判得出 stale(拆开之后不再依赖事件先后)",
    );
    s.stop();
}

{
    // ---- 降级 ①:段里没有 VizTrackState / VizTrackLabels(旧版 Output 的形态)
    const s = MMOCK.createPreviewSession({
        params: "?scenario=monitor-no-tracks",
    });
    const bridge = MBRIDGE.createMonitorBridge({ mockBackend: s.mock });
    const snap = await bridge.requestInitialState();
    check(
        snap.viz.trackVolDb === undefined && snap.viz.trackLabels === undefined,
        "段里没有 VizTrackState / VizTrackLabels",
    );
    eq(VIZ.vizAccepts(snap.viz).reason, "", "帧本身仍然可读");
    eq(
        VIZ.vizSeries(snap.viz).length,
        15,
        "**轨迹图照常 15 轨**(降级只砍分布图)",
    );
    eq(VIZ.vizDistRows(snap.viz).length, 0, "分布图画空(不猜、不填 0)");
    eq(
        VIZ.vizLegendRows(snap.viz).length,
        15,
        "图例仍列 15 行(轨迹图画了它们)",
    );
    eq(
        VIZ.vizLegendRows(snap.viz).every((r) => r.label === ""),
        true,
        "缺轨名 ⇒ 图例只剩两位轨号",
    );
    s.stop();
}

{
    // ---- 降级 ②:有三条标量、但没有 track_lead_mask(T44 已落地,此处测缺席时的降级)
    const s = MMOCK.createPreviewSession({
        params: "?scenario=monitor-no-lead",
    });
    const bridge = MBRIDGE.createMonitorBridge({ mockBackend: s.mock });
    const snap = await bridge.requestInitialState();
    check(
        snap.viz.leadMask === undefined,
        "**没有 leadMask 这个键**(不是 0)—— 0 会与「有这个字段、但没有 lead 轨」混起来",
    );
    eq(VIZ.vizDistRows(snap.viz).length, 15, "15 根柱照画");
    check(
        VIZ.vizDistRows(snap.viz).every((r) => r.lead === false),
        "一律不戴绿帽",
    );
    check(
        !DC.distBarsHtml(VIZ.vizDistRows(snap.viz), 0).includes(
            'data-lead="1"',
        ),
        "拼串里没有一个 data-lead=1",
    );
    s.stop();
}

{
    // ---- 轨名的 UTF-8 定长槽(T44:每轨 32 B,按 UTF-8 边界截断,不切半个汉字)
    eq(MMOCK.truncateUtf8("主唱", 32), "主唱", "短名原样");
    // 中文一字 3 字节 ⇒ 32 B 装得下 10 个字
    eq(
        MMOCK.truncateUtf8("一二三四五六七八九十百千", 32).length,
        10,
        "10 个汉字后截断",
    );
    check(
        new TextEncoder().encode(
            MMOCK.truncateUtf8("一二三四五六七八九十百千", 32),
        ).length <= 32,
        "截断后不超 32 字节",
    );
    // 关键:**不切出半个字符**(切了的话 JS 侧会看到替换字符 U+FFFD)
    check(
        !MMOCK.truncateUtf8("一二三四五六七八九十百千", 32).includes("�"),
        "不切出半个汉字",
    );
    // 增补平面(emoji,4 字节)同样不切成半个代理对
    const emo = MMOCK.truncateUtf8("🎤🎤🎤🎤🎤🎤🎤🎤🎤", 32);
    check(!emo.includes("�"), "增补平面字符不切成半个代理对");
    check(
        new TextEncoder().encode(emo).length <= 32,
        "emoji 截断后也不超 32 字节",
    );
    eq([...emo].length, 8, "32 B / 4 B 每个 = 8 个 emoji");
}

{
    // ---- 车道按需重发:稳态帧不带车道(T44「稳态下 4Hz 只写 128 B 帧头」)
    const s = MMOCK.createPreviewSession({
        params: "?scenario=monitor-online",
    });
    await s.mock.requestInitialState(); // 首帧必带车道
    const f1 = s.ctl.vizFrame(1, 42);
    check(
        f1.lanes === undefined,
        "revision 没变的后续帧**不带车道**(省掉 15×1024 次重传)",
    );
    eq(f1.laneRevision, 1, "帧头仍带 laneRevision 供读方判断");
    // 换组那一帧必带(另一份段)
    const f2 = s.ctl.vizFrame(2, 42, true);
    check(Array.isArray(f2.lanes), "显式要求时带车道");
    s.stop();
}

{
    // ---- 栅格化:两条判据刻意不同(位图 = 区间求交,车道 = 列中心点采样 + hold)
    const span = VC.VIZ_COLUMNS; // 每列 1 秒
    const { lane, words } = MMOCK.rasterizeTrack(
        [
            { t0S: 0, t1S: 1, pan: 30 },
            { t0S: 2.2, t1S: 4, pan: -30 },
        ],
        span,
        0,
    );
    check(VIZ.columnCovered(words, 0), "第 0 列 [0,1) 与第一段有交集 ⇒ 置位");
    check(
        !VIZ.columnCovered(words, 1),
        "第 1 列 [1,2) 与两段都无交集 ⇒ 不置位(断口成立)",
    );
    check(
        VIZ.columnCovered(words, 2),
        "第 2 列 [2,3) 与第二段 [2.2,4) 有交集 ⇒ 置位(**保守口径**:交集就算)",
    );
    check(VIZ.columnCovered(words, 3), "第 3 列 [3,4) 置位");
    check(!VIZ.columnCovered(words, 4), "第 4 列 [4,5) 与段端点相接不算交集");
    eq(VIZ.panOfFixed(lane[0]), 30, "第 0 列车道 = 列中心 0.5s 的值");
    eq(
        VIZ.panOfFixed(lane[1]),
        30,
        "第 1 列没有分段,但车道 **hold 上一段**(30)—— 断线只能看位图",
    );
    eq(VIZ.panOfFixed(lane[3]), -30, "第 3 列车道 = 第二段的 pan");
    // 短于一列的分段不会消失(保守口径的意义)
    const tiny = MMOCK.rasterizeTrack(
        [{ t0S: 5.1, t1S: 5.2, pan: 0 }],
        span,
        0,
    );
    check(
        VIZ.columnCovered(tiny.words, 5),
        "短于一列的分段仍然置位(保守口径,分段不消失)",
    );
    // 空段表 ⇒ 全哨兵 + 位图全 0(T44:「整轨无分段 → 车道全哨兵 + 位图全 0」)
    const empty = MMOCK.rasterizeTrack([], span, 0);
    check(
        empty.lane.every((x) => x === VC.VIZ_PAN_NONE),
        "无分段 ⇒ 车道全哨兵",
    );
    check(
        empty.words.every((w) => w === 0),
        "无分段 ⇒ 位图全 0",
    );
    const bad = MMOCK.rasterizeTrack([{ t0S: 5, t1S: 1, pan: 0 }], span, 0);
    check(
        bad.words.every((w) => w === 0),
        "倒挂段滤掉",
    );
}

{
    // ---- 未知场景不假装支持(与 state-driver 同款口径)
    const w = MMOCK.parseMonitorQuery("?scenario=nope");
    eq(w.scenario, "monitor-online", "未知场景回落默认档");
    check(w.warnings.length === 1, "并留一条 warning(不假装支持)");
    eq(MMOCK.parseMonitorQuery("?group=99").group, null, "组号越界 ⇒ 忽略");
    eq(MMOCK.parseMonitorQuery("?group=5").group, 5, "合法组号原样");
    eq(MMOCK.parseMonitorQuery("?play=0").play, false, "play=0 ⇒ 走带停住");
    // 场景表与壳页白名单同源(壳页 import 本表,不抄第二份)
    const shell = src("web-preview/shell.js");
    check(
        /monitor: MONITOR_SCENARIOS/.test(shell),
        "壳页的 monitor 场景白名单引用 MONITOR_SCENARIOS(不抄第二份)",
    );
    for (const name of MMOCK.MONITOR_SCENARIOS) {
        check(
            src("web-preview/index.html").includes(name) ||
                name === "monitor-no-tracks" ||
                name === "monitor-abi",
            `导航页有 ${name} 的入口(或显式豁免)`,
        );
    }
}

// =============================================================================
log("=== ⑥ 词条(monitor.* 三语)===");

{
    const KEYS = [
        "monitor.brandSub",
        "monitor.readOnlyPill",
        "monitor.groupEyebrow",
        "monitor.groupAria",
        "monitor.trajEyebrow",
        "monitor.offline",
        "monitor.stalled",
        "monitor.abiMismatch",
        "monitor.footerHint",
    ];
    const ph = (v) =>
        (String(v).match(/\{[a-zA-Z]+\}/g) || []).sort().join(",");
    for (const k of KEYS) {
        for (const lang of ["zh", "en", "fr"]) {
            check(
                typeof T[lang][k] === "string" && T[lang][k].trim().length > 0,
                `${lang}.${k} 存在且非空`,
            );
        }
        check(ph(T.zh[k]) === ph(T.en[k]), `${k} zh/en 占位符一致`);
        check(ph(T.zh[k]) === ph(T.fr[k]), `${k} zh/fr 占位符一致`);
        check(T.fr[k] !== T.en[k], `fr.${k} 不是英文照抄`);
    }
    eq(ph(T.zh["monitor.offline"]), "{X}", "monitor.offline 带组字母占位符");
    eq(
        ph(T.zh["monitor.abiMismatch"]),
        "{a},{b}",
        "abi 横幅带两端版本号占位符",
    );

    check(
        !("monitor.distEyebrow" in T.zh),
        "分布图标题复用 master.distEyebrow,未另立",
    );
    check(
        !("monitor.trajEmpty" in T.zh),
        "轨迹空态复用 chart.trajEmpty,未另立",
    );

    const mon = src("web/monitor/index.html");
    const re = /\bdata-t(?:-aria|-split)?\s*=\s*"([^"]+)"/g;
    let m;
    let n = 0;
    while ((m = re.exec(mon)) !== null) {
        n += 1;
        check(m[1] in T.zh, `Monitor 页引用的 key「${m[1]}」在字典里`);
        check(m[1] in T.en && m[1] in T.fr, `key「${m[1]}」三语齐备`);
    }
    check(
        n >= 15,
        `Monitor 页的 data-t 数量够(实得 ${n},不是一张全硬编码的页)`,
    );
    // app.js 里按 key 取的两条空态词条也必须在
    for (const k of ["monitor.offline", "chart.trajEmpty"]) {
        check(src("web/monitor/app.js").includes(k), `app.js 引用了 ${k}`);
    }
}

// =============================================================================
log("=== ⑦ 只读不变式与页面纪律 ===");

{
    const mon = src("web/monitor/index.html");
    const app = src("web/monitor/app.js");

    for (const [pat, what] of [
        [/class="[^"]*sc-toggle/, "开关 .sc-toggle"],
        [/class="[^"]*sc-knob/, "旋钮 .sc-knob"],
        [/class="[^"]*sc-slider/, "滑杆 .sc-slider"],
        [/class="[^"]*sc-tube/, "玻璃管电平表(带可拖卡箍)"],
        [/type="range"/, "原生滑杆"],
        [/class="[^"]*sc-confirm/, "行内确认条(只读页无需要确认的操作)"],
        [/<input(?![^>]*type="hidden")/, "任何输入框"],
    ]) {
        check(!pat.test(mon), `Monitor 页没有${what}`);
    }

    const calls = new Set();
    const callRe = /call\("([^"]+)"/g;
    let m;
    while ((m = callRe.exec(app)) !== null) calls.add(m[1]);
    check(calls.size > 0, "app.js 确实有上行调用(否则下一条断言是空跑)");
    for (const name of calls) {
        check(
            MBRIDGE.MONITOR_FUNCTIONS.includes(name),
            `app.js 调用的 ${name} 在 MONITOR_FUNCTIONS 里`,
        );
    }
    eq(
        [...calls].sort(),
        [
            "commitUiScale",
            "requestInitialState",
            "setLang",
            "setObservedGroup",
            "setUiScale",
        ],
        "上行调用集 = 名表全集(不多不少)",
    );

    check(
        !new RegExp(`\\b${MBOX.MONITOR_DESIGN.w}\\b`).test(mon) &&
            !new RegExp(`\\b${MBOX.MONITOR_DESIGN.h}\\b`).test(mon),
        "index.html 里不出现设计盒宽高数字(真源 monitor-box.js)",
    );
    check(
        /--box-w/.test(app) && /--box-h/.test(app),
        "app.js 把设计盒写成 --box-w/--box-h",
    );
    // ---- 设计盒的交接:**已完成**(T45 = PR #94 合入 `feature/v1` 的 `649c99f`)。
    // 960×720 现在的唯一真源是 `web/shared/design-box.js` 的 `DESIGN.monitor`,它同时是
    // `gen-design-box.py` 的输入(→ `src/core/DesignBox.h`,gate 3d 逐字节对拍)。
    // 这条断言从「在场才查同值」升级成**转发身份**:两处必须是**同一个对象**,不是同值副本。
    // 为什么要判到 `===`:同值断言只能挡住「一处改了另一处没改」,挡不住「有人把转发
    // 又抄回一份字面量」—— 抄回去的那天两边仍然同值,断言全绿,而真源纪律已经破了。
    {
        const { DESIGN } = await import(u("web/shared/design-box.js"));
        check(!!DESIGN.monitor, "web/shared/design-box.js 里有 DESIGN.monitor");
        check(
            MBOX.MONITOR_DESIGN === DESIGN.monitor,
            "monitor-box.js 是**转发**(同一个对象),不是第二份字面量",
        );
        eq(
            MBOX.MONITOR_DESIGN,
            { w: 960, h: 720, presets: [0.5, 0.65, 0.8, 1, 1.25, 1.5, 2] },
            "转发过来的值 = J75 C 定稿的 960×720 与七档",
        );
        // 页面侧不许再出现第二份字面量(转发之后本文件应当只剩一行 export)
        const box = src("web/monitor/monitor-box.js");
        check(
            !/\bw:\s*\d|\bh:\s*\d|presets:\s*\[/.test(box),
            "monitor-box.js 里没有第二份设计盒字面量",
        );
    }
    check(
        MBOX.MONITOR_DESIGN.presets.includes(1),
        "缩放档位含 1x(设计盒原尺寸)",
    );
    check(
        MBOX.MONITOR_DESIGN.presets.every((v, i, a) => i === 0 || v > a[i - 1]),
        "缩放档位严格递增",
    );

    const distH = /\.mon-chart--dist\s*\{[^}]*height:\s*(\d+)px/.exec(mon);
    check(!!distH, "分布图卡有固定高");
    if (distH) {
        check(
            Number(distH[1]) < MBOX.MONITOR_DESIGN.h / 2,
            `分布图卡 ${distH[1]}px < 盒高一半 ⇒ 轨迹图占主体(J75 C 逐字)`,
        );
    }
    check(
        /\.mon-chart--traj\s*\{[^}]*flex:\s*1/.test(mon),
        "轨迹图卡 flex:1(拿走剩余全部高度)",
    );

    const hex = mon
        .replace(/data:image\/[^"')]+/g, "")
        .match(/#[0-9a-fA-F]{3,8}\b/g);
    check(!hex, `Monitor 页零裸 hex(实得 ${JSON.stringify(hex)})`);

    // ---- 数据面接线的四条不变式
    check(
        /bridge\.on\("scvb\.state"/.test(app),
        "订了 scvb.state(段级三态 + fresh 的唯一真源)",
    );
    check(
        /store\.vizStatus/.test(app),
        "状态存进 store.vizStatus 并参与 vizAccepts",
    );
    // 在途帧闸:**建立在字段真的存在之上**。载荷里没有 `groupId` 时它永远不命中,
    // 留一段不生效的判断比没有更糟(读代码的人以为已经防住了)—— 故这条断言与
    // 「桥真的送 groupId」那条(② 的派生字段)是一对,少一半都不成立。
    check(
        /raw\.group_id !== store\.observed/.test(app),
        "viz 帧按组号丢在途帧(换组后 A 的尾帧不会被当成 B 的数据画上去)",
    );
    check(
        /g\[GROUPS_JSON_KEY\]/.test(app),
        "组位图走 GROUPS_JSON_KEY 常量(T45 用 online,不是 groups_online)",
    );
    // `groups_online` 只在 **requestInitialState 的快照**里出现(T45 那一处确实用它);
    // **周期事件** `scvb.groups` 用的是 `online`。两个键名不同,这条钉住事件那一路
    // 不许写死 —— 读错的后果是绿点永远不亮而页面一切正常、零报错。
    const groupsHandler = new RegExp(
        'bridge\\.on\\("scvb\\.groups", \\(g\\) => \\{([\\s\\S]*?)\\n {4}\\}\\);',
    ).exec(app);
    check(!!groupsHandler, "找得到 scvb.groups 订阅");
    if (groupsHandler) {
        check(
            !/groups_online/.test(groupsHandler[1]),
            "scvb.groups 的处理里没有写死的 groups_online",
        );
    }
    check(
        /vizHasLanes\(viz\)/.test(app),
        "轨迹图按有没有车道选空态文案(「未接通」vs「尚无分段」)",
    );
    check(
        /store\.frame = null;[\s\S]{0,240}store\.series = \[\];/.test(app),
        "observeGroup 清掉上一组的数据面(含车道缓存)",
    );
    check(
        /function applyProjection\(\)/.test(app) &&
            (app.match(/applyProjection\(\);/g) || []).length >= 2,
        "投影在 viz 帧与 scvb.state **两个入口**都重跑(只跑一处会让迟到的 state 补不回折线)",
    );
    const phHandler =
        /bridge\.on\("scvb\.playhead", \(p\) => \{([\s\S]*?)\n {4}\}\);/.exec(
            app,
        );
    check(!!phHandler, "找得到 playhead 订阅(下一条断言的前提)");
    if (phHandler) {
        check(!/render\(\)/.test(phHandler[1]), "30Hz 播放头不触发整页 render");
    }
    check(
        /traj\.suspend\(\)/.test(app),
        "不在线时 suspend(两条自持循环一起停)",
    );
    check(
        /isVisible: \(\) => store\.accepts\.ok/.test(app),
        "可见性闸 = viz 可读",
    );
    check(
        !/store\.lastPublishMs/.test(app) && !/VIZ_STALE_MS/.test(app),
        "停摆判据交给 native 的 fresh,UI 不再自己按事件间隔猜",
    );
}

// =============================================================================
log("=== ⑧ 生命周期:suspend / resume / destroy(T43 复用契约的首个消费者)===");

// 轨迹图在 window / 媒体查询 / ResizeObserver 上有三处**活得比 canvas 长**的订阅。
// Tab1 单实例用不到 destroy;Monitor 在插件窗口开合里反复建销毁,不拆就是每开一次漏一组。
{
    const winCalls = { add: [], remove: [] };
    const roCalls = { observe: 0, disconnect: 0 };
    const rafQ = new Map();
    let rafSeq = 0;
    const savedWin = globalThis.window;
    const savedRO = globalThis.ResizeObserver;
    const savedRAF = globalThis.requestAnimationFrame;
    const savedCAF = globalThis.cancelAnimationFrame;
    globalThis.window = {
        addEventListener: (t) => winCalls.add.push(t),
        removeEventListener: (t) => winCalls.remove.push(t),
    };
    globalThis.ResizeObserver = class {
        observe() {
            roCalls.observe++;
        }
        disconnect() {
            roCalls.disconnect++;
        }
    };
    globalThis.requestAnimationFrame = (cb) => {
        const id = ++rafSeq;
        rafQ.set(id, cb);
        return id;
    };
    globalThis.cancelAnimationFrame = (id) => rafQ.delete(id);
    const pump = (now) => {
        const due = [...rafQ.values()];
        rafQ.clear();
        for (const cb of due) cb(now);
    };

    try {
        const mkStub = () => {
            const calls = { clearRect: 0, stroke: 0 };
            const ctx = new Proxy(
                {
                    clearRect: () => calls.clearRect++,
                    stroke: () => calls.stroke++,
                },
                {
                    get: (t, p) => (p in t ? t[p] : () => {}),
                    set: () => true,
                },
            );
            return {
                calls,
                canvas: {
                    width: 0,
                    height: 0,
                    style: {},
                    parentElement: { clientWidth: 600, clientHeight: 200 },
                    getContext: () => ctx,
                    addEventListener: () => {},
                    getBoundingClientRect: () => ({
                        left: 0,
                        top: 0,
                        width: 600,
                        height: 200,
                    }),
                },
            };
        };

        // 两张图同时活着 = Tab1 与 Monitor 各一张的真实形态
        const a = mkStub();
        const b = mkStub();
        let visibleB = true;
        const series = [
            {
                ch: 1,
                stereo: false,
                runs: [
                    [
                        { tS: 0, pan: 0 },
                        { tS: 10, pan: 0 },
                    ],
                ],
            },
        ];
        const tab1 = TC.createTrajectoryChart({
            canvas: a.canvas,
            getSeries: () => series,
            getDurationS: () => 300,
            isVisible: () => true,
        });
        const monitor = TC.createTrajectoryChart({
            canvas: b.canvas,
            getSeries: () => series,
            getDurationS: () => 300,
            isVisible: () => visibleB,
        });
        eq(
            winCalls.add,
            ["pointerup", "pointerup"],
            "两实例各自挂了一条 window 兜底",
        );
        eq(roCalls.observe, 2, "两实例各自观察了自己的父盒");

        // ---- resume/invalidate:可见时真的画
        monitor.invalidate();
        pump(16);
        pump(32);
        check(
            b.calls.stroke > 0,
            "可见时 invalidate ⇒ 真的画了(stroke 有调用)",
        );
        const drawnOnce = b.calls.stroke;

        // ---- suspend:停帧,且不再画
        monitor.suspend();
        pump(48);
        pump(64);
        eq(b.calls.stroke, drawnOnce, "suspend 之后不再画(两条自持循环都停了)");

        // ---- 离场期的重绘诉求要**留着**:不可见时 invalidate 不画,但记账
        visibleB = false;
        monitor.invalidate();
        pump(80);
        eq(b.calls.stroke, drawnOnce, "不可见时 invalidate 不画");

        // ---- resume:回到前台把欠的那次重绘还上
        visibleB = true;
        monitor.resume();
        pump(96);
        pump(112);
        check(
            b.calls.stroke > drawnOnce,
            "resume 把离场期欠下的重绘还清(切回来不是一张过期的图)",
        );

        // ---- 另一张图**全程不受影响**
        const aDrawn = a.calls.stroke;
        tab1.invalidate();
        pump(128);
        pump(144);
        check(
            a.calls.stroke > aDrawn,
            "另一张图照常工作(suspend 的是另一个实例)",
        );

        // ---- destroy:只退自己那组订阅,幂等
        monitor.destroy();
        eq(winCalls.remove, ["pointerup"], "拆 Monitor 那张只退掉它自己那条");
        eq(roCalls.disconnect, 1, "只断开它自己的 ResizeObserver");
        tab1.timeline.set({ startS: 10, endS: 60 });
        eq(tab1.viewport(), { startS: 10, endS: 60 }, "另一张图仍然可用");

        monitor.destroy();
        monitor.destroy();
        eq(winCalls.remove, ["pointerup"], "重复 destroy 不重复退订(幂等)");
        eq(roCalls.disconnect, 1, "ResizeObserver 也只断开一次");

        // ---- destroy 之后不再画(rAF 循环真的停了)
        const afterDestroy = b.calls.stroke;
        monitor.invalidate();
        pump(160);
        pump(176);
        eq(b.calls.stroke, afterDestroy, "destroy 之后 invalidate 也不再画");

        tab1.destroy();
        eq(
            winCalls.remove,
            ["pointerup", "pointerup"],
            "两张都拆完 ⇒ 两条兜底都退掉了",
        );
        eq(roCalls.disconnect, 2, "两个 ResizeObserver 都断开");
        eq(
            winCalls.add.length,
            winCalls.remove.length,
            "挂了几条就退了几条(零泄漏)",
        );
        eq(rafQ.size, 0, "没有留下悬着的 rAF(空闲零 rAF)");
    } finally {
        for (const [k, v] of [
            ["window", savedWin],
            ["ResizeObserver", savedRO],
            ["requestAnimationFrame", savedRAF],
            ["cancelAnimationFrame", savedCAF],
        ]) {
            if (v === undefined) delete globalThis[k];
            else globalThis[k] = v;
        }
    }
}

{
    // 接法:页面确实在窗口收起时拆图,并清掉自己的定时器
    const app = src("web/monitor/app.js");
    check(
        /addEventListener\("pagehide", \(\) => \{[\s\S]{0,200}traj\.destroy\(\)/.test(
            app,
        ),
        "pagehide 时 destroy() 轨迹图",
    );
    // ⚠ 这里**曾经**有一条 `/clearTimeout\(staleTimer\)/` 的源码正则,当「不留孤儿 timer」
    // 的证据。而 `staleTimer` 那个变量在判据改归 native 之后早就没了 —— 于是关窗时
    // 处理器**必抛 ReferenceError,同一个处理器里的 traj.destroy() 一次都没跑过**。
    // 断言全绿,而 destroy 契约的第一个消费者一直在漏订阅。
    // 教训:**源文本断言只能证明「字符在」,证明不了「代码跑得通」**。要证明后者,
    // 得真的把页面跑起来 —— 那是 `smoke-monitor-page.mjs`(无头 Chrome)在做的事,
    // 那边显式派发 pagehide 并断言零未捕获异常。
    // 只查**代码**:文件里那段讲这个坑的注释提到这个名字是应该的,不该把它算成回归。
    const appCode = app.replace(/^\s*\/\/.*$/gm, "");
    check(
        !/staleTimer/.test(appCode),
        "pagehide 里没有对不存在的 staleTimer 的引用(那会让 traj.destroy() 永远跑不到)",
    );
    check(
        /addEventListener\("pagehide", stopScaleCountdown\)/.test(app),
        "pagehide 时缩放倒计时也停(关窗回退,不污染新实例)",
    );
}

// =============================================================================
log("=== ⑨ 分布图帧间补间(SL-192;web/shared/dist-motion.js)===");

// 用户实测(v5.4):「monitor 里的声像/音量分布……跟 output 的图相比帧数很低,一秒钟
// 刷新一两次的感觉,而且速度比那边慢」。两页画的是同一件 `distribution-chart.js`,差的是
// 数据面频率:Output 吃 25Hz 的 `scvb.params`,Monitor 吃 **4Hz** 的 viz 段(冻结口径)。
// 补间层只做一件事:把离散到达的目标铺成 rAF 逐帧的连续运动,且**绝不外推**。
// (SL-192 后半程把段侧提到 30Hz、页面实得 25Hz;补间仍在,因为 25Hz 依旧是离散步,
//  且停摆 / 宿主节流 / 换组 / 旧版 4Hz Output 这些降级路径一个都没消失。)
{
    const row = (ch, pan, volDb, widthPct, extra) => ({
        ch,
        pan,
        volDb,
        widthPct,
        stereo: false,
        lead: false,
        ...(extra || {}),
    });

    // ---- 纯函数:进度封顶(反鬼影的那一条)
    eq(DM.frameProgress(0, 250), 0, "p:起点 = 0");
    eq(DM.frameProgress(125, 250), 0.5, "p:半程 = 0.5");
    eq(DM.frameProgress(250, 250), 1, "p:整程 = 1");
    eq(DM.frameProgress(9999, 250), 1, "p:**封顶 1**,写方停摆也不外推");
    eq(DM.frameProgress(-5, 250), 0, "p:负 elapsed 夹到 0(时钟回拨不倒放)");
    eq(DM.frameProgress(100, 0), 1, "p:时长为 0 ⇒ 直接到位,不出 Infinity");
    eq(DM.frameProgress(NaN, 250), 1, "p:非有限 elapsed ⇒ 到位,不出 NaN");

    // ---- 补间时长是**定值** = 页面实得的到达周期,不是「实测帧间隔」、也不是段侧发布周期。
    // 链路:发布器 30Hz → [M] 60Hz → 桥面 25Hz(WebViewHost 基准 tick,既有 50→25 裁定)。
    // 最慢的一级才是页面实得的速率 ⇒ 40ms。
    // 为什么不取实测间隔:那量的是「数据多久变一次」(ramp 之间可以好几秒没动静),
    // 拿它当时长会把一次孤立的段边界跳变抹成几秒的爬行 —— 编出来的运动。
    eq(
        DM.DIST_SPAN_MS,
        40,
        "补间时长 = 页面实得到达周期 40ms(= 桥面 25Hz;不是段侧的 33ms,更不是升频前的 250ms)",
    );

    // ---- 纯函数:逐行插值
    eq(
        DM.lerpDistRow(row(1, -100, -24, 0), row(1, 100, 12, 100), 0.5),
        { ch: 1, stereo: false, lead: false, pan: 0, volDb: -6, widthPct: 50 },
        "三条连续量各自线性插值",
    );
    // ⚠ 端点**刻意挑的是 −0.1 → 0.3**:`a + (b − a) · 1` 在这一对上算出
    // 0.30000000000000004,而在 −100 → 100 这类整数对上恰好等于 b —— 拿整数对写这条断言
    // 等于没写(改成 `a + (b − a) · p` 照样全绿)。到位要**逐位**等于目标,否则
    // 「到没到位」就没法用相等去判,自停判据会永远差那么一丁点。
    eq(
        DM.lerpDistRow(row(1, -0.1, -0.1, -0.1), row(1, 0.3, 0.3, 0.3), 1),
        {
            ch: 1,
            stereo: false,
            lead: false,
            pan: 0.3,
            volDb: 0.3,
            widthPct: 0.3,
        },
        "p=1 **逐位等于**目标(不是 a+(b−a)·1,那在浮点下不保证)",
    );
    {
        const mixed = DM.lerpDistRow(
            row(3, 0, -6, 100),
            row(3, 40, -2, 80, { stereo: true, lead: true }),
            0.25,
        );
        eq(
            [mixed.stereo, mixed.lead],
            [true, true],
            "离散面(立体声 / lead)一律取目标行,不插",
        );
    }

    // ---- 结构指纹:决定「能不能按下标插值」
    eq(
        DM.distShapeKey([row(1, 0, 0, 100), row(2, 0, 0, 100)]),
        "1/00,2/00",
        "指纹 = 轨号 + 立体声位 + lead 位",
    );
    check(
        DM.distShapeKey([row(1, 0, 0, 100)]) !==
            DM.distShapeKey([row(1, 0, 0, 100, { stereo: true })]),
        "立体声位变了 ⇒ 指纹变(张开线的节点是新增的)",
    );
    check(
        DM.distShapeKey([row(1, 0, 0, 100), row(2, 0, 0, 100)]) !==
            DM.distShapeKey([row(2, 0, 0, 100)]),
        "轨集变了 ⇒ 指纹变(下标不再对齐)",
    );

    // ---- 工厂:首帧 / 补间中段 / 到位 / 陈旧
    {
        const m = DM.createDistMotion({});
        m.push([row(1, -50, -6, 100)], 0, 1000);
        eq(
            m.rows().map((r) => [r.pan, r.volDb]),
            [[-50, -6]],
            "首帧**直接落到目标**(从 0 扫过来会是一段虚构的开场动画)",
        );
        eq(m.diag().animating, false, "首帧不起 rAF");

        m.push([row(1, 50, 6, 100)], 0, 1040);
        m.tick(1040);
        eq(m.rows()[0].pan, -50, "补间起点 = **当前显示值**,不是上一帧的目标");
        m.tick(1060);
        eq(
            [m.rows()[0].pan, m.rows()[0].volDb],
            [0, 0],
            "半程:横位与柱高都走到一半(4Hz 的一步被铺成连续运动)",
        );
        check(m.tick(1079), "到位前 tick 报「还要续帧」");
        eq(m.tick(1080), false, "到位那一帧报「可以自停」(空闲零 rAF)");
        eq(m.rows()[0].pan, 50, "整程:精确落到目标");

        // 写方停摆:帧流断在这里,此后无论过多久都不许再动一丝一毫。
        m.tick(9500);
        eq(
            m.rows().map((r) => [r.pan, r.volDb, r.widthPct]),
            [[50, 6, 100]],
            "陈旧:冻在最后一份真数据上(**零外推** —— 外推出来的柱位是捏造的读数)",
        );
    }

    // ---- 工厂:值没变的帧不起帧(宿主/mock 会照常按 4Hz 重发同一份值)
    {
        const m = DM.createDistMotion({});
        m.push([row(1, 10, -3, 100)], 0, 1000);
        m.push([row(1, 10, -3, 100)], 0, 1040);
        eq(m.diag().pushes, 0, "重复值 ⇒ 一次补间都不起(空闲零 rAF)");
        // 与数据面无关的 render(`scvb.groups` 1Hz 那一路)夹在两帧之间时,
        // 补间仍须走满一个发布周期 —— 早先那版拿「上次 push 到现在」当时长,
        // 这一下会把时长压成 50ms,画面上就是运动时快时慢。
        m.push([row(1, 20, -3, 100)], 0, 1200); // ← 无关事件,值又变了
        m.tick(1220);
        eq(
            m.rows()[0].pan,
            15,
            "补间时长不受「上一次 push 什么时候」影响(定值 40ms 的半程)",
        );
    }

    // ---- 工厂:轨集变了不许跨轨插值
    {
        const m = DM.createDistMotion({});
        m.push([row(1, -80, -6, 100), row(2, 80, -6, 100)], 0, 1000);
        m.push([row(2, 80, -6, 100)], 0, 1040); // 轨 1 下线,轨 2 挪到下标 0
        m.tick(1060);
        eq(
            m.rows().map((r) => [r.ch, r.pan]),
            [[2, 80]],
            "轨集变化 ⇒ 直接落位:轨 2 不会从轨 1 的位置滑过来",
        );
    }

    // ---- 工厂:reset 之后不许从上一组的值插过来(换组清零)
    {
        const m = DM.createDistMotion({});
        m.push([row(1, -90, -20, 100)], 0, 1000);
        m.reset();
        eq(m.rows(), [], "reset 清空显示值");
        m.push([row(1, 90, 10, 100)], 0, 1040);
        eq(
            m.rows().map((r) => [r.pan, r.volDb]),
            [[90, 10]],
            "换组后第一帧直接落位(指纹清成 null,轨集相同也不会误判成「结构没变」)",
        );
    }

    // ---- 工厂:一次 push 只取一个 width(#135 复审【重要】)
    // 一拍里最多会「重拼一次 + 写变量两次」。三处各自去取 getter 的话,滑杆正在拖时
    // 这一拍**内部**就能取到不同的值:拼串那一版用旧 width、写变量那一版用新 width,
    // 画出半帧新半帧旧的几何。上一版只给 `rebuild(width)` 加了形参,而 `paint()` 仍在
    // 自己取 —— 取值次数与时机与改动前逐字等价,注释却宣称「两处钉成一个数」。
    // 这条断言钉的就是那个数:**整拍一次调用**。
    {
        let widthCalls = 0;
        const m = DM.createDistMotion({
            // 只给 innerHTML,不给 querySelectorAll:rebuild 拼完串就早退,bars 保持空,
            // paint 照常跑完取 width 那一步 —— 正是本条要数的东西。
            container: { innerHTML: "" },
            getGlobalWidthPct: () => {
                widthCalls++;
                return 100;
            },
        });
        m.push([row(1, -50, -6, 100)], 0, 1000); // 结构变 ⇒ 重拼 + 立刻画,同一拍
        eq(widthCalls, 1, "首帧(重拼 + 画)整拍只取一次 width");
        widthCalls = 0;
        // 只改高亮 ⇒ 走重拼分支,随后那句 width 比对**判等、不补帧**(getter 恒回 100)。
        // 这一拍照样只许取一次:旧写法在重拼与比对表达式里各取一次 ⇒ 计数 2。
        m.push([row(1, -50, -6, 100)], 2, 1040);
        eq(widthCalls, 1, "改高亮那一拍(重拼 + width 比对)也只取一次");
    }

    // ---- 工厂:坏行在入口就滤掉,谓词与拼串侧同义(PR 115 审查第 3 条)
    // `distBarsHtml` 对 ch 非有限的行是 continue(不产出节点),而 paint() 按**下标**
    // 把 shown[i] 写进 bars[i]。两边谓词不一致,坏行会让节点与行整体错位一格 ——
    // 每根柱写上邻轨的几何,而 setVars 的 null 保护让它一声不吭。
    {
        const m = DM.createDistMotion({});
        m.push(
            [row(1, -50, -6, 100), { ch: undefined }, row(3, 50, -6, 100)],
            0,
            1000,
        );
        eq(
            m.rows().map((r) => r.ch),
            [1, 3],
            "ch 非有限的行在入口被滤掉(与 distBarsHtml 同一谓词)",
        );
        eq(
            DC.distBarsHtml(m.rows(), 0).match(/class="dist-bar"/g).length,
            m.rows().length,
            "滤后的行数与拼串产出的柱数一一对应(下标不会错位)",
        );
        m.push([null, row(1, -50, -6, 100), row(3, 50, -6, 100)], 0, 1040);
        eq(
            m.rows().map((r) => r.ch),
            [1, 3],
            "null 行也不炸、也不进数据面",
        );
    }

    // ---- 工厂:隐藏期跳过的 paint 记欠账,恢复可见时补画(PR 115 审查第 4 条)
    // tick() 在不可见时照常推进 shown 却不写 DOM;若之后来的 push 数值与 target 相同,
    // sameDistValues 会早退 —— DOM 就停在隐藏前那一帧上。
    {
        let visible = true;
        const painted = [];
        const node = () => ({
            style: { setProperty: (k, v) => painted.push(k + v) },
        });
        const bars = [node()];
        const container = {
            innerHTML: "",
            querySelectorAll: (sel) => (sel === ".dist-bar" ? bars : []),
        };
        const m = DM.createDistMotion({
            container,
            isVisible: () => visible,
        });
        m.push([row(1, -50, -6, 100)], 0, 1000);
        m.push([row(1, 50, 6, 100)], 0, 1040);
        visible = false;
        m.tick(1080); // 补间到位,但不可见 ⇒ 跳过 paint,记欠账
        const beforeRestore = painted.length;
        visible = true;
        m.push([row(1, 50, 6, 100)], 0, 1080); // 值与 target 相同 ⇒ 老实现会早退不重画
        check(
            painted.length > beforeRestore,
            "恢复可见后补画了隐藏期欠下的那一帧(不留陈旧 DOM)",
        );
    }

    // ---- 工厂:destroy 之后一切写入口早退(与 trajectory-chart 同一条纪律),且**放掉引用**
    {
        const m = DM.createDistMotion({});
        m.push([row(1, 0, 0, 100)], 0, 1000);
        eq(m.rows().length, 1, "destroy 前有数据面");
        m.destroy();
        // 放掉引用:app.js 的 pagehide 注释说「不拆就连着数据面与节点数组一起钉在内存里」,
        // 只停 rAF 不清引用那句话就不成立(PR 115 审查第 5 条)。
        eq(m.rows(), [], "destroy 清掉数据面引用");
        m.push([row(1, 100, 12, 100)], 0, 1040);
        eq(m.rows(), [], "destroy 之后 push 一步不走(不会把数据面又填回来)");
        eq(m.diag().animating, false, "destroy 之后循环也不再起");
    }

    // ---- 几何格式化只有一处:补间写入面与拼串产物必须逐字对齐
    {
        const geo = DC.distGeometry(-33.3, -7.7, 66.6);
        const html = DC.distBarsHtml(
            [row(1, -33.3, -7.7, 66.6, { stereo: true })],
            0,
        );
        const bar = DC.distBarVars(geo);
        const span = DC.distSpanVars(geo);
        check(
            html.includes(`--x:${bar["--x"]};--h:${bar["--h"]}`),
            "distBarVars 的串与拼串模板逐字相同",
        );
        check(
            html.includes(
                `--x0:${span["--x0"]};--w:${span["--w"]};--y:${span["--y"]}`,
            ),
            "distSpanVars 的串与拼串模板逐字相同",
        );
    }

    // ---- 页面接线:分布图不再走「收到帧就重拼 innerHTML」那条路
    {
        const app = src("web/monitor/app.js");
        check(
            /distMotion\.push\(vizDistRows\(viz\), highlightCh\)/.test(app),
            "render 把行集交给补间器(不是每帧重拼 innerHTML)",
        );
        check(
            !/distBarsHtml/.test(app),
            "app.js 里不再直接拼柱体(拼串归补间器,免得两条路各写一份)",
        );
        check(
            /distMotion\.reset\(\)/.test(app),
            "不在线 / 换组时 reset(),不留上一组的补间状态",
        );
        check(
            /distMotion\.destroy\(\)/.test(app),
            "pagehide 时 destroy(),不漏自持 rAF",
        );
        const motion = src("web/shared/dist-motion.js");
        check(
            /requestAnimationFrame/.test(motion) &&
                /cancelAnimationFrame/.test(motion),
            "补间由 rAF 驱动,且有配对的 cancel",
        );

        // ---- 几何**不许**进 CSS 过渡集(PR 115 审查抓到的那条)
        // 老写法每帧重拼 innerHTML,节点是新建的 ⇒ `transition: all` 从不生效;
        // 改成「给既有节点逐帧写变量」之后它**开始生效**,于是 left/height 被叠上一层
        // 300ms 低通 —— 补间逻辑全绿而屏幕上照样慢半拍(实测滞后 2.89 个百分点)。
        // 行为面的守卫在 smoke-monitor-page ⑧c(量渲染位置);这里再钉一道源码面,
        // 让「顺手把 transition 改回 all」在 node 侧就红,不必等页面级跑起来。
        {
            const html = src("web/monitor/index.html");
            // 只查**生效的声明**,先把 CSS 注释剥掉 —— 上面那段讲这个坑的注释里逐字写着
            // `transition: all`,不剥就会把自己的注释当成回归(与 ⑧ 节 `staleTimer`
            // 那条同一手法:源文本断言必须先划清「代码」与「讲代码的话」)。
            // **先剥注释,再找规则体的 `}`**。反过来的话,注释里哪天出现一个 `}`
            // 就会把规则截断在半路,断言从此断在一段残缺文本上 —— 而它多半还是绿的。
            //
            // 另注:下面只断「过渡集里除了 opacity 没有别的」,**不**断「hover 淡出会动画」——
            // 高亮改的是 `data-hi`,那条路径走 rebuild()(整块 innerHTML 换新),新节点没有
            // 可过渡的旧计算值,这条 opacity 过渡其实一次都不会跑。断一个不成立的事实,
            // 等于把它写进合同。
            const stripped = html.replace(/\/\*[\s\S]*?\*\//g, "");
            const ruleOf = (sel) => {
                const i = stripped.indexOf(sel + " {");
                if (i < 0) return "";
                return stripped.slice(i, stripped.indexOf("}", i));
            };
            // 按括号深度拆逗号:深度 > 0 的逗号在函数记法内部(cubic-bezier / rgb / calc),
            // 不是过渡项之间的分隔符。
            const topLevelSplit = (s) => {
                const out = [];
                let depth = 0;
                let cur = "";
                for (const c of s) {
                    if (c === "(") depth++;
                    else if (c === ")") depth--;
                    if (c === "," && depth === 0) {
                        out.push(cur);
                        cur = "";
                    } else cur += c;
                }
                out.push(cur);
                return out;
            };
            for (const sel of [".dist-bar", ".dist-span"]) {
                const rule = ruleOf(sel);
                check(rule.length > 0, `找得到 ${sel} 的规则`);
                check(
                    !/transition:\s*all/.test(rule),
                    `${sel} 不用 transition: all —— 几何进过渡集会把 rAF 补间叠成 300ms 低通`,
                );
                // 要断的性质是「过渡集里**只有** opacity」,所以用**白名单**(拆逗号,
                // 每段都必须是 opacity),不用黑名单 —— 黑名单永远漏一批:
                // translate / scale / rotate / padding 都不在里面,而且补不全。
                // 用 matchAll 收**每一条** transition 声明:后一条整体覆盖前一条,
                // 只看第一条的话 `transition: opacity .3s; transition: left .3s;` 会放行,
                // 而实际生效的正是 left。取到 `;` 为止,**不依赖尾分号** ——
                // 规则块最后一条声明省略分号是合法 CSS,依赖它会在合法写法上误红。
                // (`}` 是冗余兜底:`ruleOf` 已经切在 `}` 之前,这一路走不到它。)
                const decls = [...rule.matchAll(/transition:\s*([^;}]*)/g)].map(
                    (m) => m[1].trim(),
                );
                // 拆的是**顶层**逗号:`cubic-bezier(0.2, 0.7, 0.2, 1)` 里的逗号不是分隔符。
                // 现在靠 `var(--ease)` 把它包着才没炸 —— 哪天有人把 `--ease` 内联成字面值
                // (tokens.css 里它就是这个 cubic-bezier),朴素的 `split(",")` 会切出四段、
                // 后三段过不了 opacity 判据 ⇒ **在合法 CSS 上误红**,失败文案还会打成
                // 「过渡集里有别的属性」。与刚修掉的「依赖尾分号」是同一类脆弱,换了个字符。
                const onlyOpacity = (d) =>
                    topLevelSplit(d).every((s) => /^opacity\b/.test(s.trim()));
                check(
                    decls.length > 0 && decls.every(onlyOpacity),
                    `${sel} 的过渡集里只有 opacity、没有别的属性(实得 "${decls.join(" | ")}")`,
                );
            }
        }
    }

    // ---- 整条频率链(SL-192 两段合并后的口径)
    // 发布器 30Hz → [M] 轮询 60Hz → 桥面 25Hz。**每一级都必须 ≥ 上一级,或是它的整数倍**:
    // 同频异相的两级会周期性整帧跳过,这一卡在读方栽了两次、在写方差点栽第三次。
    // 数值断言归 C++(test_viz_plane.cpp 的 [rate] 按逻辑钟数帧);这里守住常量本身不被改回去。
    {
        const proc = src("src/monitor/MonitorProcessor.cpp");
        const edit = src("src/monitor/MonitorEditor.cpp");
        const vp = src("src/core/output/VizPublisher.h");

        // ① 发布端:33ms ≈ 30Hz(冻结契约变更,docs/contract-changes/20260827-viz-30hz.md)
        check(
            /kPublishIntervalMs\s*=\s*25;/.test(vp),
            "发布闸门 25ms —— 夹在 60Hz 驱动的一拍(16.7)与两拍(33.3)之间,实得 30Hz",
        );
        // ② 驱动它的定时器必须 ≥2× 闸门 —— 同频异相会周期性丢帧
        check(
            /kPublishTimerHz\s*=\s*60;/.test(vp),
            "发布驱动 60Hz:两拍 = 33.3ms = 30Hz,且闸门两头都有余量(抗抖动)",
        );
        check(
            /vizTimer_->startTimerHz\(scvb::output::VizPublisher::kPublishTimerHz\)/.test(
                src("src/output/OutputProcessor.cpp"),
            ),
            "发布走**独立**定时器,不搭主 25Hz tick(25Hz 驱动不出 30Hz)",
        );
        // ③ 读方轮询 60Hz = 2× 发布率
        check(
            /startTimerHz\(kVizPollHz\)/.test(proc) &&
                /constexpr int kVizPollHz = 60;/.test(proc),
            "[M] 轮询 60Hz = 2× 发布率(不会与 30Hz 发布器同频异相地跳过整帧)",
        );
        // ④ 桥面:没有闸,变化即发。只查**代码** —— 文件里讲这个坑的注释提到旧常量名是
        // 应该的(与 ⑧ 节 `staleTimer` 那条同一手法),不该算成回归。
        const editCode = edit.replace(/^\s*\/\/.*$/gm, "");
        check(
            !/kVizEmitIntervalMs/.test(editCode),
            "编辑器侧的 250ms 推送闸已取消(它在 25Hz 栅格上实得 280ms = 3.57Hz)",
        );
        check(
            /emitIfChanged\(bridge::kEvViz/.test(edit),
            "scvb.viz 仍走 emitIfChanged:值未变不发,桥面实得 ≤25Hz(基准 tick 上限)",
        );
    }
}

// =============================================================================
// v5 实测回归(web 侧)—— P0-2 单链一次确认 / P1-8 峰线对位 / P2-9..P2-13 产品项
// 仓内零 node_modules(无 jsdom),故这一组走「源码级字面断言 + 词条断言」:
// 断的都是**唯一能造成该现象的那一处写法**,改回旧写法当场红。
// =============================================================================
{
    const tt = src("web/output/tab-tracks.js");
    const html = src("web/output/index.html");
    const css = src("web/shared/base.css");
    const tokens = src("web/shared/tokens.css");
    const master = src("web/output/tab-master.js");

    // ---- P0-2:「该轨仍由手动固定值驱动」提示条与「将清除…继续?」确认条互为出口 → 死循环。
    //      修法是单链:提示条自身即确认,链接直接执行,另给一个终止出口。
    check(
        /part === "manualdriven-reidentify"[\s\S]{0,400}?return doReidentify\(ch\)/.test(
            tt,
        ),
        "P0-2 解冻提示的「重新识别」直接执行,不再开第二道确认条",
    );
    check(
        !/openConfirm\(\s*ch\s*,\s*"reidentify"/.test(tt),
        "P0-2 不存在 kind=reidentify 的确认条(那一环已拆掉)",
    );
    check(
        /part === "manualdriven-dismiss"/.test(tt) &&
            /local\.unfreezeHint\.delete\(ch\)/.test(tt),
        "P0-2 提示条带终止出口(「知道了」撤下提示,不发写面调用)",
    );
    check(
        /local\.reidentifying\.add\(ch\)/.test(tt) &&
            /if \(local\.reidentifying\.has\(ch\)\) return;/.test(tt),
        "P0-2 重新识别在途时不接受新的解冻提示挂起(clearManual 清冻结位造成的 1→0 不该重新点亮提示)",
    );
    check(
        /manualdriven-dismiss/.test(tt) && /common\.gotIt/.test(tt),
        "P0-2 终止钮在模板里且用 common.gotIt 词条",
    );

    // ---- §9 分层:任一插件目录都不得直接编译 / include 另一插件的源码。
    //      共用件的正确归属是 plugin-common(core ← plugin-common ← 插件,方向不能倒)。
    //      #100 复审【重要】1:UiDefaultsStore 曾以 `../output/…` 被编进 Input target。
    //      判据只看**生效的构建语句**,注释先剥掉:这条断言原先是对整份文件做子串扫描,
    //      于是一句**描述**模块图的注释(`… -> ../output/canvas/timeline.js`)就能把它打红,
    //      而真正的跨角色引用反而可以写成不带 `../` 的形式溜过去 —— 两头都不对。
    {
        const effectiveLines = (text) =>
            text
                .split("\n")
                .map((l) => l.replace(/#.*$/, ""))
                .join("\n");

        for (const role of ["input", "output", "monitor"]) {
            const cm = effectiveLines(src(`src/${role}/CMakeLists.txt`));
            const others = ["input", "output", "monitor"].filter(
                (o) => o !== role,
            );
            for (const other of others) {
                check(
                    !cm.includes(`../${other}/`),
                    `§9 src/${role}/CMakeLists.txt 不引用 ../${other}/ 的源码或头文件目录`,
                );
                // 跨角色引用还有第二种写法:scvb_add_web_assets 的 EXTRA_DIRS(相对 web/,不带 ../)。
                // 那一层跨的是 **web 资源**不是 C++ 源码,§9 允许 —— 但只允许**已登记**的那一条,
                // 免得这个口子被当成绕开分层的后门。新增一条就要来这里改,顺带解释为什么。
                const webXref = new RegExp(
                    `EXTRA_DIRS[^)]*\\b${other}/([\\w./-]+)`,
                    "g",
                );
                for (const m of cm.matchAll(webXref)) {
                    check(
                        role === "monitor" &&
                            other === "output" &&
                            m[1] === "canvas",
                        `§9 src/${role}/CMakeLists.txt 的 EXTRA_DIRS 跨到 ${other}/${m[1]} —— ` +
                            `web 资源跨角色只登记了 monitor -> output/canvas 这一条(欠债:` +
                            `web/shared/trajectory-chart.js 反向 import 了 role 目录,终局是把 canvas/ 提到 shared/)`,
                    );
                }
            }
        }
        // C++ 面的正向断言:Monitor 的编译单元里不许出现另外两个插件的源码。
        // 上面那条禁的是 `../output/` 这种写法,这条禁的是「换个写法照样编进来」。
        {
            const mon = effectiveLines(src("src/monitor/CMakeLists.txt"));
            const sources = mon.slice(
                mon.indexOf("target_sources"),
                mon.indexOf("target_include_directories"),
            );
            check(
                !/\b(input|output)\//.test(sources),
                "§9 src/monitor/CMakeLists.txt 的 target_sources 不含另外两个插件的目录",
            );
        }
        check(
            src("src/plugin-common/CMakeLists.txt").includes(
                "UiDefaultsStore.cpp",
            ),
            "§9 UiDefaultsStore 归 plugin-common(两插件共用,依赖只指向本层)",
        );

        // P0-C 的门禁缺口(评审【建议】):Monitor 漏 scvb_add_web_assets 既不影响构建
        // 也不影响 pluginval(那条路不开窗),`resourceSource = {}` 还是个合法值 ——
        // 三个 target 都必须**既生成各自的 WebAssets、又把它链进去、还要真的把 source 填上**,
        // 少任何一环都会在真机上变成「WebView 去公网找 juce.backend」。
        for (const [role, editor, ns] of [
            ["input", "src/input/InputEditor.cpp", "ScvbInputWebData"],
            ["output", "src/output/OutputEditor.cpp", "ScvbOutputWebData"],
            ["monitor", "src/monitor/MonitorEditor.cpp", "ScvbMonitorWebData"],
        ]) {
            const cm = src(`src/${role}/CMakeLists.txt`);
            check(
                cm.includes("scvb_add_web_assets") &&
                    cm.includes(`NAMESPACE ${ns}`),
                `P0-C src/${role} 生成 ${ns} 嵌入资源`,
            );
            check(
                /target_link_libraries[\s\S]*WebAssets/.test(cm),
                `P0-C src/${role} 把 WebAssets 链进插件 target`,
            );
            const ed = src(editor).replace(/\s+/g, " ");
            check(
                ed.includes(`resourceSource = {${ns}::`),
                `P0-C ${role} 的 resourceSource 真的填了 ${ns}(不是空 {})`,
            );
        }
    }

    // ---- P0-4:泳道波形。数据面(OutputProcessor::waveformOf)由 host harness 断言;
    //      这里守的是**桥接线**那一跳 —— handleRequestWaveform 曾是写死「全未覆盖」的桩,
    //      回包形状合法、能过 isTileShape,于是泳道照常画斜纹与栅格却一根包络都没有。
    //      桩与真回包形状相同,只有「有没有真去问处理器」能把两者分开。
    {
        const oe = src("src/output/OutputEditor.cpp");
        const body = oe.slice(
            oe.indexOf("void OutputEditor::handleRequestWaveform"),
            oe.indexOf("void OutputEditor::handleSetActiveTab"),
        );
        check(
            /processor_\.waveformOf\(ch, startS, endS, cols\)/.test(body),
            "P0-4 requestWaveform 向处理器取真实瓦片",
        );
        check(
            !/push\(covered, 0\);/.test(body),
            "P0-4 回包里不存在写死的 covered=0(那是 T29 的桩)",
        );
    }

    // ---- P1-8:峰线 left 定位的是内侧边,线体再向外长一个线宽 → 恒高出柱顶。
    //      往回让一个线宽,外沿与柱顶重合。
    for (const cls of ["sc-tube__peak", "sc-meter__peak"]) {
        // 截到**本规则的第一个 `}`** 为止。原先用固定 slice(0,900) 窗口:注释一长就溢到
        // 后续规则里,后面任一规则出现 `border-radius: 0;` 都会假绿(评审【建议】)。
        const from = css.indexOf("." + cls + " {");
        const block = css.slice(from, css.indexOf("}", from) + 1);
        check(
            /left: calc\(var\(--pk, 0%\) - var\(--meter-peak-w\)\)/.test(block),
            `P1-8 .${cls} 的 left 回让一个线宽(外沿贴柱顶)`,
        );
        // [SL-191 → SL-204 用户裁定 2026-08-27] 这里原先断言 `transition: left
        // var(--dur-meter) linear` **存在**,理由写的是「与液柱同步插值,瞬态不再裂开
        // 一道缝」。裁定推翻了它:同步的是**时长**、不是**到达时刻** —— linear transition
        // 在固定 180ms 内走完各自的距离,液柱要跑几十上百 px 而白线常常一步不动,两者恒
        // 不同时到位。这正是用户报的「柱头从来碰不到白线」。
        // SL-191 把玻璃管两件的过渡整条去掉;SL-204 按用户「软化起跳」的裁定改成
        // `--dur-meter-tube`(33ms = 一个 §2.5 事件间隔)。**不得回到 --dur-meter**:
        // 那是 5.4 个事件间隔,补的是过期目标,病会原样回来。
        // .sc-meter*(总线表)在 web 侧尚无驱动、不盲改,故仍用 --dur-meter —— 断言按件分叉。
        if (cls === "sc-meter__peak") {
            check(
                /transition: left var\(--dur-meter\) linear/.test(block),
                `P1-8 .${cls} 暂留 --dur-meter(总线表无驱动;接线时按 SL-191 重测再定)`,
            );
        } else {
            check(
                /transition: left var\(--dur-meter-tube\) linear/.test(block),
                `SL-204 .${cls} 走一个事件间隔的补间(--dur-meter-tube)`,
            );
        }
        // v5.1 P1-G:2px 宽的盒子套胶囊圆角会被画成一颗收圆的点,视觉重心离开外沿 ——
        // 几何对齐了仍看着高半个线宽。峰线是刻线,要齐头齐尾。
        check(
            /border-radius: 0;/.test(block),
            `P1-G .${cls} 不用胶囊圆角(2px 刻线要齐头齐尾)`,
        );
    }
    // [SL-191 → SL-204] **成对**断言:液柱与峰线的补间时长必须逐字相同。
    // 只改一边实测会得到反向病 —— 只去液柱 ⇒ 柱头反冲到白线外侧(最多 −16.2px),
    // 因为白线换向时轮到它单独滞后。这条就是拦「只改一边」的。
    {
        const blockOf = (cls) => {
            const from = css.indexOf("." + cls + " {");
            return css.slice(from, css.indexOf("}", from) + 1);
        };
        const durOf = (block) => {
            const m = /transition:\s*\w+\s+var\((--[\w-]+)\)/.exec(block);
            return m ? m[1] : null;
        };
        const liq = blockOf("sc-tube__liquid");
        const pk = blockOf("sc-tube__peak");
        check(
            liq.length > 0 && pk.length > 0,
            "SL-191 玻璃管液柱/峰线两条规则都在",
        );
        eq(
            durOf(liq),
            durOf(pk),
            "SL-204 液柱与峰线取同一个补间时长 token(只改一边会得到反向错位)",
        );
        eq(
            durOf(liq),
            "--dur-meter-tube",
            "SL-204 玻璃管走一个事件间隔的补间;回到 --dur-meter(5.4 个间隔)柱头就又够不着白线",
        );
        // token 的值本身也钉住:33ms = 30Hz 一发。改这个数等于改「补间跨几个事件」。
        check(
            /--dur-meter-tube:\s*33ms;/.test(src("web/shared/tokens.css")),
            "SL-204 --dur-meter-tube = 33ms(= 一个 §2.5 事件间隔)",
        );
    }
    // 首帧与弹道帧用同一套取整,免得首帧偏半个像素(meter.js 两处都是 toFixed(1))。
    check(
        /--lv:\$\{\(t\.lv \* 100\)\.toFixed\(1\)\}%/.test(tt) &&
            /--pk:\$\{\(t\.pk \* 100\)\.toFixed\(1\)\}%/.test(tt),
        "P1-G 首帧 --lv/--pk 与弹道帧同精度",
    );

    // ---- P2-9:分析键不再拿「有没有覆盖数据」当前置。
    check(
        /else if \(isWriteBlocked\(\)\) an = "disabled";/.test(master),
        "P2-9 disabled 只表示写权限缺失",
    );
    check(
        /"data-analyze-nodata",\s*\n?\s*analyzeNoData\(p, totals\.n\) \? "1" : "0",/.test(
            master,
        ),
        "P2-9 无数据改由独立属性承载(空态原因句照旧出)",
    );
    check(
        /\.master-flow\[data-analyze-nodata="1"\] \.an-preview\[data-when="ok"\]/.test(
            html,
        ) &&
            /\.master-flow:not\(\[data-analyze-nodata="1"\]\)\s*\n?\s*\.an-preview\[data-when="nodata"\]/.test(
                html,
            ),
        "P2-9 空态原因句由 data-analyze-nodata 驱动",
    );

    // ---- P2-10:分布图跟随全局「最大角度」。这一条有**真几何断言**,不只看写法。
    {
        const at100 = DC.distGeometry(60, 0, 100, 100);
        const at50 = DC.distGeometry(60, 0, 100, 50);
        const at0 = DC.distGeometry(60, 0, 100, 0);
        check(
            at100.x > at50.x && at50.x > at0.x,
            `P2-10 最大角度越小,柱位越靠中(100%→${at100.x} / 50%→${at50.x} / 0%→${at0.x})`,
        );
        eq(at0.x, 50, "P2-10 最大角度 0 → 全部收到正中");
        // 名义 pan 60 × 0.5 = 30 → x =(30+100)/200 = 65%
        eq(at50.x, 65, "P2-10 缩放口径与 PanMath::scaleByGlobalWidth 同式");
        // 缺省参数保持老几何(Monitor 的 viz 段不带全局 width,得原样沿用)
        eq(
            DC.distGeometry(60, 0, 100).x,
            at100.x,
            "P2-10 不传全局 width 时几何与从前逐字节一致",
        );
        // 立体声张开线同样被缩放(两个子声像一起被缩)
        check(
            DC.distGeometry(0, 0, 100, 50).half <
                DC.distGeometry(0, 0, 100, 100).half,
            "P2-10 立体声张开半宽随最大角度收窄",
        );
    }

    // ---- P2-11:空态提示压在曲线中线上。
    check(
        /transform: translate\(-50%, calc\(-50% - 18px\)\);/.test(
            html.slice(html.indexOf(".curve-empty {")),
        ),
        "P2-11「双击添加控制点」上移,不压中线",
    );

    // ---- P2-12:文档钮并到「重看引导」旁(同一 foot 行)。
    {
        const foot = html.slice(
            html.indexOf('data-gb="settings-reopentour"'),
            html.indexOf('data-gb="settings-reopentour"') + 1400,
        );
        check(
            foot.includes('data-gb="settings-docs"'),
            "P2-12「文档」与「重看引导」同排",
        );
        check(
            !/settings-guideblock__docs/.test(html),
            "P2-12 旧的卡外左对齐样式已移除",
        );
    }

    // ---- P2-13:红卡改紫渐变,且**字色与底色分离**。
    check(
        /class="sc-card sc-banner--violet settings-guideblock-rules"/.test(
            html,
        ),
        "P2-13 使用须知卡改紫面",
    );
    check(
        /--sem-violet-surface: linear-gradient\(/.test(tokens) &&
            /--sem-violet-ink:/.test(tokens) &&
            /--sem-violet-title:/.test(tokens),
        "P2-13 紫面 token 齐备(底=渐变,正文/标题各自独立字色)",
    );
    {
        const rules = html.slice(
            html.indexOf(".settings-guideblock-rules__title {"),
        );
        check(
            /color: var\(--sem-violet-title\)/.test(rules.slice(0, 300)),
            "P2-13 标题走独立字色 token",
        );
    }
    {
        // 只看真正生效的声明,不看注释里提到的旧 token 名。
        const block = html.slice(
            html.indexOf(".settings-guideblock-rules {"),
            html.indexOf(".settings-guideblock-rules__spacer"),
        );
        const reds = block
            .split(/\r?\n/)
            .filter((l) =>
                /^\s*(color|background|stroke)\s*:.*--sem-red/.test(l),
            );
        eq(
            reds.length,
            0,
            `P2-13 该卡内不再有生效的红色声明(紫底红字);实得 ${reds.join(" | ")}`,
        );
    }
}

// =============================================================================
if (fail > 0) {
    console.error(`\n=== 失败 ${fail} 条 ===`);
    process.exit(1);
}
log(`\n=== 结果:全部通过${skips > 0 ? `(${skips} 条 SKIP)` : ""} ===`);
process.exit(0);
