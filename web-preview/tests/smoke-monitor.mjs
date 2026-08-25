// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Monitor 冒烟(node,无 DOM;T46 / [J75] C)
// =============================================================================
// 与既有冒烟同款口径:仓内零 node_modules,故断言面是**纯函数 + mock 端到端 +
// 源码不变式**,画布光栅与指针手势归浏览器手测(shot.mjs 截图 + 真机 preview)。
//
// 跑什么:
//   ① **提取件不回归**:`web/shared/distribution-chart.js` 的两处拼串与提取前
//      `tab-master.js` 里那两段模板**逐字节相同**(oracle 是从提取前的源码逐字
//      转写来的,不是照着新实现抄的),且 tab-master 再导出的 distGeometry
//      与共享件是同一个函数对象 —— Output 侧「零行为变化」的机检面;
//   ② **两页不漂**:Monitor 页与 Output 页的分布图/图例类名、轨色接法、
//      以及那几条 tokens.css 里没有的局部 token 逐条对拍;
//   ③ **viz 投影纯函数**(`web/monitor/viz.js`):位图位序、四种拒读理由、
//      断线判据、轨号闸、图例并集、色号回落、时长兜底;
//   ④ **mock 端到端**:真桥(`createMonitorBridge`)+ mock 后端跑通首帧、
//      组切换往返(含在途帧丢弃)、空态、badArg 拒绝态;
//   ⑤ **词条**:`monitor.*` 三语齐备、非空、占位符一致、fr 不是英文照抄;
//   ⑥ **只读不变式**:Monitor 页没有任何写控件,app.js 的上行调用是
//      MONITOR_FUNCTIONS 的子集,页面零硬编码设计盒尺寸;
//   ⑦ **多实例与 destroy**:两张轨迹图同时活着互不干扰,destroy 只退自己那组订阅
//      且幂等 —— 这正是 T43 给 T46 留的复用契约的**首个消费者**。
//
// 用法:node web-preview/tests/smoke-monitor.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有断言失败(逐条打印 [FAIL])。
// =============================================================================

import { pathToFileURL, fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";
import { readFileSync } from "node:fs";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const u = (p) => pathToFileURL(join(ROOT, p)).href;
const src = (p) => readFileSync(join(ROOT, p), "utf8");

const DC = await import(u("web/shared/distribution-chart.js"));
const TM = await import(u("web/output/tab-master.js"));
const TC = await import(u("web/shared/trajectory-chart.js"));
const TCOL = await import(u("web/shared/track-colors.js"));
const VIZ = await import(u("web/monitor/viz.js"));
const MBOX = await import(u("web/monitor/monitor-box.js"));
const MBRIDGE = await import(u("web/monitor/monitor-bridge.js"));
const MMOCK = await import(u("web-preview/mock/monitor-mock.js"));
const { T } = await import(u("web/shared/i18n.js"));

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

// =============================================================================
log("=== ① 提取件不回归(Output 侧零行为变化)===");

{
    // distGeometry 是**同一个函数对象**:tab-master 的再导出不是抄了一份。
    check(
        TM.distGeometry === DC.distGeometry,
        "tab-master.distGeometry ≡ distribution-chart.distGeometry(再导出,不是副本)",
    );
    // 几何值逐条钉住(设计稿 L2037-2056 的换算,提取前后必须同值)
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
    // 张开线**整体排在柱体之前**(DOM 顺序即画序;横线先画才不会盖住柱顶)
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
    // 轨名是用户数据,必须转义(不转义就是一个 innerHTML 注入面)
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

    // 事件委托的轨号提取(两页共用一处判据)
    const rowNode = {
        closest: (sel) =>
            sel === "[data-legend-ch]" ? { getAttribute: () => "12" } : null,
    };
    eq(DC.legendChOf(rowNode), 12, "命中图例行 → 轨号");
    eq(DC.legendChOf({ closest: () => null }), 0, "不在行上 → 0");
    eq(DC.legendChOf(null), 0, "空目标 → 0(不炸)");

    // 接法:Output 侧确实改走共享件了(回退到本地拼串当场红)
    const tm = src("web/output/tab-master.js");
    check(
        /distBarsHtml\(rows, local\.chartHi\)/.test(tm),
        "renderDist 走 distBarsHtml",
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
log("=== ② 两页不漂(Monitor ↔ Output 的图表面对拍)===");

{
    const mon = src("web/monitor/index.html");
    const out = src("web/output/index.html");

    // 类名是共享件的对外契约:拼串里写死了这些名字,页面少一条样式就少一块图。
    const CLASSES = [
        ".dist-plot",
        ".dist-plot__mid",
        ".dist-plot__q",
        ".dist-plot__base",
        ".dist-bar",
        ".dist-span",
        ".dist-axis",
        ".traj-plot",
        ".traj-axis-y",
        ".traj-axis-y__tick",
        ".traj-stage",
        ".traj-stage__canvas",
        ".traj-stage__playhead",
        ".traj-stage__zoom",
        ".traj-stage__actions",
        ".traj-stage__empty",
        ".chart-legend",
        ".chart-legend__item",
        ".chart-legend__dot",
        ".chart-legend__st",
    ];
    for (const cls of CLASSES) {
        const re = new RegExp(cls.replace(/\./g, "\\.") + "\\s*[,{]");
        check(re.test(mon), `Monitor 页有 ${cls} 的样式`);
        check(re.test(out), `Output 页有 ${cls} 的样式(对拍基准)`);
    }

    // 轨色接法 —— 两页都必须是 `rgba(var(--tc), …)`。这条是**行为**面:
    // `--tc` 由共享件逐柱写成 `var(--track-color-N)`,页面这一侧写错色就没了。
    for (const [name, text] of [
        ["Monitor", mon],
        ["Output", out],
    ]) {
        check(
            /\.dist-bar\s*\{[^}]*rgba\(var\(--tc\), 0\.95\)/.test(text),
            `${name} 页柱体走 rgba(var(--tc), .95)`,
        );
        check(
            /\.dist-span\s*\{[^}]*rgba\(var\(--tc\), 0\.8\)/.test(text),
            `${name} 页张开线走 rgba(var(--tc), .8)`,
        );
        check(
            /\.chart-legend__dot\s*\{[^}]*rgba\(var\(--tc\), 0\.95\)/.test(
                text,
            ),
            `${name} 页图例色点走 rgba(var(--tc), .95)`,
        );
        // 非高亮轨的淡出档 = trajectory-chart 的 DIM_ALPHA(两张图看起来是同一次高亮)
        check(
            new RegExp(`opacity:\\s*${TC.DIM_ALPHA}`).test(text),
            `${name} 页的图例淡出档 = DIM_ALPHA(${TC.DIM_ALPHA})`,
        );
        // 默认隐藏节点的总闸(空态盖板、两条横幅都靠它)
        check(
            /\[hidden\]\s*\{\s*display:\s*none\s*!important/.test(text),
            `${name} 页有全局 [hidden] 兜底`,
        );
    }

    // tokens.css 里没有、两页各自 :root 写着的局部 token —— **必须同值**。
    // 与 track-colors.js 的镜像表 ↔ tokens.css 是同一类纪律:漂了当场红。
    const decl = (text, name) => {
        const m = new RegExp(`--${name}:\\s*([^;]+);`).exec(text);
        return m ? m[1].replace(/\s+/g, "") : null;
    };
    for (const name of [
        "w-35",
        "w-45",
        "w-82",
        "w-85",
        "w-90",
        "dist-bar-lead",
    ]) {
        const a = decl(mon, name);
        const b = decl(out, name);
        check(!!a, `Monitor 页声明了 --${name}`);
        check(!!b, `Output 页声明了 --${name}(对拍基准)`);
        eq(a, b, `--${name} 两页同值`);
    }
}

// =============================================================================
log("=== ③ viz 投影纯函数(web/monitor/viz.js)===");

{
    eq(
        VIZ.GROUP_LETTERS.slice(),
        ["A", "B", "C", "D", "E", "F", "G", "H"],
        "组字母 A-H",
    );
    eq(VIZ.CHANNEL_COUNT, 15, "15 轨(J59)");
    eq(VIZ.groupOnline(0b00010011, 1), 1, "位图 bit0 = 组 A 在线");
    eq(VIZ.groupOnline(0b00010011, 3), 0, "组 C 不在线");
    eq(VIZ.groupOnline(0b00010011, 5), 1, "组 E 在线");
    eq(VIZ.groupOnline(undefined, 1), 0, "事件缺失 ⇒ 全灭、零报错");

    // ---- activity 位图:LSB 起、每字 32 格。位序写反了图**照画**,只是断线位置
    // 整体错开 32 的倍数 —— 肉眼几乎看不出来,故必须机检。
    const bits = [0, 1, 31, 32, 33, 63, 64];
    const words = [0, 0, 0];
    for (const i of bits)
        words[i >>> 5] = (words[i >>> 5] | (1 << (i & 31))) >>> 0;
    for (const i of bits)
        check(VIZ.slotActive(words, i), `第 ${i} 格置位后读得出`);
    for (const i of [2, 30, 34, 62, 65]) {
        check(!VIZ.slotActive(words, i), `第 ${i} 格未置位`);
    }
    check(!VIZ.slotActive(words, 999), "越界格 ⇒ false(不画线好过画假线)");
    check(!VIZ.slotActive(null, 0), "activity 缺失 ⇒ false");
    check(!VIZ.slotActive(words, -1), "负下标 ⇒ false");
    // bit 31 的坑:`1 << 31` 是负数,位图里出现负「u32」会让 C++ 侧对表白查半天
    check(words[0] > 0, "第 0 字仍是非负数(>>> 0 折回无符号)");

    // ---- 四种拒读理由
    const base = {
        magic: VIZ.VIZ_MAGIC,
        abi: VIZ.VIZ_ABI,
        online: true,
        channels: [],
    };
    eq(VIZ.vizAccepts(base).reason, "", "合法帧:无拒读理由");
    check(VIZ.vizAccepts(base).ok, "合法帧 ok");
    eq(VIZ.vizAccepts(null).reason, "shape", "null ⇒ shape");
    eq(
        VIZ.vizAccepts({ ...base, channels: undefined }).reason,
        "shape",
        "缺 channels ⇒ shape",
    );
    eq(
        VIZ.vizAccepts({ ...base, magic: "XXXX" }).reason,
        "magic",
        "magic 对不上 ⇒ 整帧丢弃",
    );
    eq(
        VIZ.vizAccepts({ ...base, abi: VIZ.VIZ_ABI + 1 }).reason,
        "abi",
        "段比本机新 ⇒ 停止读取",
    );
    eq(
        VIZ.vizAccepts({ ...base, abi: VIZ.VIZ_ABI + 1 }).abi,
        VIZ.VIZ_ABI + 1,
        "拒读时把对端 abi 带出来(横幅要显示)",
    );
    check(
        VIZ.vizAccepts({ ...base, abi: 0 }).ok,
        "段比本机旧 ⇒ 照读(只拒高不拒低)",
    );
    eq(
        VIZ.vizAccepts({ ...base, online: false }).reason,
        "offline",
        "组不在线 ⇒ 空态,不是错误",
    );

    // ---- 时长与格宽的兜底
    eq(VIZ.vizDurationS({ durationS: 120 }), 120, "时长原样");
    eq(
        VIZ.vizDurationS({}),
        VIZ.VIZ_FALLBACK_DURATION_S,
        "无证据 ⇒ 兜底(不塌到 0)",
    );
    eq(
        VIZ.vizSlotS({ durationS: 100, slotCount: 200 }),
        0.5,
        "缺 slotS 时反算",
    );
    eq(
        VIZ.vizSlotS({ durationS: 100, slotCount: 200, slotS: 2 }),
        2,
        "给了就用给的",
    );
    eq(VIZ.vizSlotS({}), 0, "既无 slotS 又无 slotCount ⇒ 0(调用方据此不画)");
}

{
    // ---- 断线判据:activity 里连续为 1 的格聚成一条折线,有 0 就另起一条。
    const mkViz = (activeSlots, pans) => {
        const slotCount = 8;
        const activity = [0];
        for (const i of activeSlots)
            activity[0] = (activity[0] | (1 << i)) >>> 0;
        return {
            magic: VIZ.VIZ_MAGIC,
            abi: VIZ.VIZ_ABI,
            online: true,
            durationS: 8,
            slotCount,
            slotS: 1,
            channels: [{ ch: 1, pan: pans, activity, stereo: false }],
        };
    };
    const pans = [10, 10, 10, 0, 0, -20, -20, -20];

    const solid = VIZ.vizSeries(mkViz([0, 1, 2], pans));
    eq(solid.length, 1, "单轨一条线");
    eq(solid[0].runs.length, 1, "连续 3 格 = 一条折线");
    eq(
        solid[0].runs[0],
        [
            { tS: 0, pan: 10 },
            { tS: 1, pan: 10 },
            { tS: 1, pan: 10 },
            { tS: 2, pan: 10 },
            { tS: 2, pan: 10 },
            { tS: 3, pan: 10 },
        ],
        "每格一个水平台阶(段内水平、格间竖直;不画斜线)",
    );

    const gapped = VIZ.vizSeries(mkViz([0, 1, 5, 6], pans));
    eq(
        gapped[0].runs.length,
        2,
        "中间有 0 格 ⇒ 断线(J75 A「无分段覆盖的区间不画线」)",
    );
    eq(gapped[0].runs[1][0].tS, 5, "第二条从第 5 格开始");
    // 折线时间恒单调不减
    for (const run of gapped[0].runs) {
        check(
            run.every((p, i) => i === 0 || p.tS >= run[i - 1].tS),
            "折线时间单调不减",
        );
    }

    eq(VIZ.vizSeries(mkViz([], pans)).length, 0, "一格都没覆盖 ⇒ 该轨整条不画");
    eq(
        VIZ.vizSeries({ ...mkViz([0], pans), slotS: 0, slotCount: 0 }).length,
        0,
        "格宽为 0 ⇒ 空(不产生 NaN 坐标)",
    );

    // pan 夹到角度域;lanes 里的脏值不许把线画出框
    const dirty = mkViz([0, 1], [999, -999, 0, 0, 0, 0, 0, 0]);
    const pts = VIZ.vizSeries(dirty)[0].runs[0];
    check(
        pts.every((p) => p.pan >= TC.PAN_MIN && p.pan <= TC.PAN_MAX),
        "lanes 里的越界值夹到 −100..100",
    );

    // 轨号闸:1..15 之外一律不要(画出来的线在图例里找不到对应行)
    const badCh = {
        ...mkViz([0, 1], pans),
        channels: [
            { ch: 0, pan: pans, activity: [3] },
            { ch: 99, pan: pans, activity: [3] },
            { ch: 2, pan: pans, activity: [3] },
        ],
    };
    eq(
        VIZ.vizSeries(badCh).map((s) => s.ch),
        [2],
        "ch=0 / ch=99 滤掉",
    );

    // 升序:色板按轨号固定映射,顺序不许乱
    const shuffled = {
        ...mkViz([0, 1], pans),
        channels: [5, 2, 9].map((ch) => ({ ch, pan: pans, activity: [3] })),
    };
    eq(
        VIZ.vizSeries(shuffled).map((s) => s.ch),
        [2, 5, 9],
        "按轨号升序",
    );

    // 立体声位(J75 A:画 pan 中心线,身份由图例 ST 角标承担)
    const st = {
        ...mkViz([0, 1], pans),
        channels: [{ ch: 3, pan: pans, activity: [3], stereo: true }],
    };
    check(VIZ.vizSeries(st)[0].stereo === true, "立体声位透传");

    // 轨色索引:默认 = 轨号;给了合法色号就用色号;非法回落轨号
    eq(VIZ.colorChOf({ ch: 7 }), 7, "缺 colorIndex ⇒ 用轨号");
    eq(VIZ.colorChOf({ ch: 7, colorIndex: 3 }), 3, "给了色号就用色号");
    eq(VIZ.colorChOf({ ch: 7, colorIndex: 99 }), 7, "色号越界 ⇒ 回落轨号");
    eq(VIZ.colorChOf({ ch: 7, colorIndex: 0 }), 7, "色号 0 ⇒ 回落轨号");
    eq(VIZ.colorChOf(null), 1, "空输入 ⇒ 1(永不返回 undefined)");
}

{
    // ---- 分布图 rows 与图例并集
    const viz = {
        magic: VIZ.VIZ_MAGIC,
        abi: VIZ.VIZ_ABI,
        online: true,
        durationS: 4,
        slotCount: 4,
        slotS: 1,
        channels: [
            // 有值 + 有覆盖 ⇒ 两张图都出现
            {
                ch: 1,
                label: "主唱",
                volDb: -3,
                panNow: -4,
                widthPct: 100,
                pan: [0, 0, 0, 0],
                activity: [0b1111],
            },
            // 有值、无覆盖 ⇒ 只在分布图(还没分析过的轨)
            {
                ch: 2,
                label: "念白",
                volDb: -9,
                panNow: 20,
                widthPct: 100,
                pan: [0, 0, 0, 0],
                activity: [0],
            },
            // 无值、有覆盖 ⇒ 只在轨迹图(空闲轨:画柱会变成幽灵柱)
            {
                ch: 3,
                label: "和声",
                stereo: true,
                panNow: -58,
                widthPct: 82,
                pan: [5, 5, 5, 5],
                activity: [0b1111],
            },
        ],
    };
    eq(
        VIZ.vizDistRows(viz).map((r) => r.ch),
        [1, 2],
        "无 volDb 的轨不画柱(不造幽灵柱)",
    );
    eq(
        VIZ.vizSeries(viz).map((s) => s.ch),
        [1, 3],
        "无覆盖的轨不画线",
    );
    eq(
        VIZ.vizLegendRows(viz).map((r) => r.ch),
        [1, 2, 3],
        "图例 = 两图并集(两图同屏,跟着任一张都会出现『图例里有它、屏幕上找不到』)",
    );
    const rows = VIZ.vizLegendRows(viz);
    check(rows[2].stereo === true && rows[2].ch === 3, "图例带立体声标记");
    check(
        rows.every((r) => typeof r.label === "string"),
        "label 恒为字符串(缺配置时空串,不是 undefined)",
    );
    eq(VIZ.vizDistRows(null), [], "空 viz ⇒ 空 rows");
    eq(VIZ.vizLegendRows(null), [], "空 viz ⇒ 空图例");

    // 分布图 rows 直接喂共享件应当画得出东西(两件的接口真的对得上)
    check(
        DC.distBarsHtml(VIZ.vizDistRows(viz), 0).includes("dist-bar"),
        "vizDistRows → distBarsHtml 端到端画得出柱",
    );
}

// =============================================================================
log("=== ④ mock 端到端(真桥 + mock 后端)===");

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
        "Monitor 侧函数名表(临时形制,待与 T44/T45 对表)",
    );
    eq(
        MBRIDGE.MONITOR_EVENTS.slice(),
        ["scvb.viz", "scvb.groups", "scvb.playhead"],
        "Monitor 侧事件名表",
    );
    // 只读身份的机检面:名表里不许出现任何写引擎状态的名字。
    // `setGroupId` 尤其点名 —— 契约 §1.4 的那个是 Output **改组**(会断连接),
    // 与本页「换观察对象」是两件事,共用名字迟早有人照 §1.4 去实现它。
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
    bridge.on("scvb.viz", (v) => {
        lastViz = v;
    });
    // 事件名不在表内即抛错(拼错事件名比收不到事件更难查)
    let threw = false;
    try {
        bridge.on("scvb.state", () => {});
    } catch {
        threw = true;
    }
    check(threw, "订阅表外事件名当场抛错");

    const snap = await bridge.requestInitialState();
    check(!!snap, "首帧快照拿得到");
    eq(snap.groupId, 1, "monitor-online 场景开箱观察组 A");
    check(
        VIZ.vizAccepts(snap.viz).ok,
        "快照自带首帧 viz 且可读(不必空等一个发布周期)",
    );
    eq(VIZ.vizSeries(snap.viz).length, 15, "组 A = 满配 15 轨全有线");
    eq(VIZ.vizDistRows(snap.viz).length, 15, "15 根柱");
    check(snap.groups_online === 0b00010011, "在线组位图 = A/B/E");

    // 每轨的 lanes 长度与 slotCount 一致,位图字数够用(段的自洽性)
    for (const c of snap.viz.channels) {
        eq(
            c.pan.length,
            snap.viz.slotCount,
            `轨 ${c.ch} 的 lanes 长度 = slotCount`,
        );
        eq(
            c.activity.length,
            Math.ceil(snap.viz.slotCount / 32),
            `轨 ${c.ch} 的位图字数 = ceil(slotCount/32)`,
        );
        check(
            c.activity.every((w) => w >= 0),
            `轨 ${c.ch} 的位图字全为非负(u32 口径)`,
        );
    }
    // 断线确实存在(乐句之间本来就没段);全 1 的位图说明栅格化写错了
    const anyGap = snap.viz.channels.some((c) =>
        c.activity.some((w) => w !== 0xffffffff),
    );
    check(anyGap, "位图里有 0 位 ⇒ 图上真的会断线(不是一整条铺满)");

    // ---- 组切换往返
    const ok = await bridge.setObservedGroup(2);
    eq(ok, { ok: true }, "切到组 B 受理");
    eq(lastViz.groupId, 2, "切组后立刻回推一帧 B 的 viz");
    eq(
        VIZ.vizSeries(lastViz).map((x) => x.ch),
        [1, 2, 3, 4, 5, 6],
        "组 B = 6 轨小编制",
    );
    const backA = await bridge.setObservedGroup(1);
    eq(backA, { ok: true }, "切回组 A");
    eq(
        VIZ.vizSeries(lastViz).length,
        15,
        "组 A 又是 15 轨(切换真的换了数据面)",
    );
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
    // ---- 空态:观察的组没有 Output(J75 C「组不在线显示空态」)
    const s = MMOCK.createPreviewSession({
        params: "?scenario=monitor-offline",
    });
    const bridge = MBRIDGE.createMonitorBridge({ mockBackend: s.mock });
    const snap = await bridge.requestInitialState();
    eq(snap.groupId, 3, "monitor-offline 场景观察组 C");
    const a = VIZ.vizAccepts(snap.viz);
    check(!a.ok, "不在线 ⇒ 不可读");
    eq(a.reason, "offline", "理由是 offline(空态,不是错误)");
    check(
        snap.viz.online === false,
        "**仍然发了一帧**(『没有事件』分不清离线还是桥断了)",
    );
    eq(VIZ.vizSeries(snap.viz).length, 0, "空态无折线");
    s.stop();

    // ---- 组切换场景:开箱停在 B,且三个在线组轨数各不相同(切换肉眼可辨)
    const g = MMOCK.createPreviewSession({
        params: "?scenario=monitor-groups",
    });
    const gb = MBRIDGE.createMonitorBridge({ mockBackend: g.mock });
    const gsnap = await gb.requestInitialState();
    eq(gsnap.groupId, 2, "monitor-groups 场景开箱停在组 B");
    const counts = [];
    for (const gid of [1, 2, 5]) {
        await gb.setObservedGroup(gid);
        counts.push(VIZ.vizSeries(g.ctl.vizFrame(gid, 42)).length);
    }
    eq(counts, [15, 6, 9], "A/B/E 三组轨数各不相同(切换必定看得出来)");
    check(
        VIZ.vizSeries(g.ctl.vizFrame(3, 42)).length === 0,
        "不在线的组给出空帧",
    );
    g.stop();

    // ---- 未知场景不假装支持(与 state-driver 同款口径)
    const w = MMOCK.parseMonitorQuery("?scenario=nope");
    eq(w.scenario, "monitor-online", "未知场景回落默认档");
    check(w.warnings.length === 1, "并留一条 warning(不假装支持)");
    eq(MMOCK.parseMonitorQuery("?group=99").group, null, "组号越界 ⇒ 忽略");
    eq(MMOCK.parseMonitorQuery("?group=5").group, 5, "合法组号原样");
    eq(MMOCK.parseMonitorQuery("?play=0").play, false, "play=0 ⇒ 走带停住");
}

{
    // ---- 栅格化:格中心判据(与格有交集会把不足一格的缝隙抹平,断线跟着消失)
    const g = MMOCK.rasterizeChannel(
        [
            { t0S: 0, t1S: 1, pan: 30 },
            { t0S: 2.2, t1S: 4, pan: -30 },
        ],
        4,
        1,
    );
    eq(g.pan[0], 30, "第 0 格中心 0.5s 落在第一段内");
    check(
        !VIZ.slotActive(g.activity, 1),
        "第 1 格中心 1.5s 落在缝隙里 ⇒ 不置位(断口成立)",
    );
    check(
        VIZ.slotActive(g.activity, 2),
        "第 2 格中心 2.5s 落在第二段内 ⇒ 置位",
    );
    eq(g.pan[2], -30, "第 2 格取第二段的 pan");
    check(VIZ.slotActive(g.activity, 3), "第 3 格置位");
    eq(MMOCK.rasterizeChannel([], 4, 1).activity, [0], "空段表 ⇒ 位图全 0");
    eq(
        MMOCK.rasterizeChannel([{ t0S: 5, t1S: 1, pan: 0 }], 2, 1).activity,
        [0],
        "倒挂段滤掉",
    );
}

// =============================================================================
log("=== ⑤ 词条(monitor.* 三语)===");

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
        // 05 §5 占位符判据:fr 不许照抄英文
        check(T.fr[k] !== T.en[k], `fr.${k} 不是英文照抄`);
    }
    eq(ph(T.zh["monitor.offline"]), "{X}", "monitor.offline 带组字母占位符");
    eq(
        ph(T.zh["monitor.abiMismatch"]),
        "{a},{b}",
        "abi 横幅带两端版本号占位符",
    );

    // **不新立重复词条**:分布图标题与轨迹空态与 Tab1 共用(改文案时不会只改一半)
    check(
        !("monitor.distEyebrow" in T.zh),
        "分布图标题复用 master.distEyebrow,未另立",
    );
    check(
        !("monitor.trajEmpty" in T.zh),
        "轨迹空态复用 chart.trajEmpty,未另立",
    );

    // 页面里 data-t / data-t-aria 引用的 key 必须都在字典里(check-i18n 也查,
    // 这里再钉一遍是为了让本套单跑时也能定位到 Monitor 页)
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
}

// =============================================================================
log("=== ⑥ 只读不变式与页面纪律 ===");

{
    const mon = src("web/monitor/index.html");
    const app = src("web/monitor/app.js");

    // 没有任何写控件 —— J75 C「纯只读监视器」的机检面
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

    // 上行调用只允许名表里的五个(多一个就是越界)
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

    // 设计盒:页面零硬编码尺寸,数字只在 monitor-box.js
    check(
        !new RegExp(`\\b${MBOX.MONITOR_DESIGN.w}\\b`).test(mon) &&
            !new RegExp(`\\b${MBOX.MONITOR_DESIGN.h}\\b`).test(mon),
        "index.html 里不出现设计盒宽高数字(真源 monitor-box.js)",
    );
    check(
        /--box-w/.test(app) && /--box-h/.test(app),
        "app.js 把设计盒写成 --box-w/--box-h",
    );
    check(
        MBOX.MONITOR_DESIGN.presets.includes(1),
        "缩放档位含 1x(设计盒原尺寸)",
    );
    check(
        MBOX.MONITOR_DESIGN.presets.every((v, i, a) => i === 0 || v > a[i - 1]),
        "缩放档位严格递增",
    );
    check(
        MBOX.MONITOR_DESIGN.h >= 640 && MBOX.MONITOR_DESIGN.w >= 800,
        "设计盒够装下两张图 + header + 图例",
    );

    // J75 C「轨迹图占主体」:轨迹卡 flex:1,分布卡固定高,且分布高 < 盒高的一半
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

    // 零裸 hex(与 Output/Input 同一条 grep 口径;data-URI 内部的编码色除外)
    const hex = mon
        .replace(/data:image\/[^"')]+/g, "")
        .match(/#[0-9a-fA-F]{3,8}\b/g);
    check(!hex, `Monitor 页零裸 hex(实得 ${JSON.stringify(hex)})`);

    // 组切换的在途帧必须丢弃 —— 否则「切过去了但画的是上一组」会伪装成正确显示
    check(
        /viz\.groupId !== store\.observed/.test(app),
        "onViz 丢弃组号不符的在途帧",
    );
    // 切组时清掉上一组的数据面(留着旧图比空白更误导)
    check(
        /store\.viz = null;[\s\S]{0,200}store\.series = \[\];/.test(app),
        "observeGroup 清掉上一组的数据面",
    );
    // 30Hz 播放头不排整页渲染(整页投影与竖线无关;排了就是纯烧 CPU)
    const phHandler =
        /bridge\.on\("scvb\.playhead", \(p\) => \{([\s\S]*?)\n {4}\}\);/.exec(
            app,
        );
    check(!!phHandler, "找得到 playhead 订阅(下一条断言的前提)");
    if (phHandler) {
        check(!/render\(\)/.test(phHandler[1]), "30Hz 播放头不触发整页 render");
    }
    // 空闲零 rAF:不在线时轨迹图收手
    check(
        /traj\.suspend\(\)/.test(app),
        "不在线时 suspend(两条自持循环一起停)",
    );
    check(
        /isVisible: \(\) => store\.accepts\.ok/.test(app),
        "可见性闸 = viz 可读",
    );
}

// =============================================================================
log("=== ⑦ 多实例与 destroy(T43 复用契约的首个消费者)===");

// 轨迹图在 window / 媒体查询 / ResizeObserver 上有三处**活得比 canvas 长**的订阅。
// Tab1 单实例用不到 destroy;Monitor 在插件窗口开合里反复建销毁,不拆就是每开一次
// 漏一组订阅。这里让两张图**同时活着**(= Tab1 与 Monitor 各一张的真实形态),
// 验「拆一张不影响另一张」与「重复拆是幂等的」。
{
    const winCalls = { add: [], remove: [] };
    const roCalls = { observe: 0, disconnect: 0 };
    const savedWin = globalThis.window;
    const savedRO = globalThis.ResizeObserver;
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
    try {
        const stub = () => ({
            width: 0,
            height: 0,
            style: {},
            parentElement: { clientWidth: 600, clientHeight: 200 },
            getContext: () =>
                new Proxy({}, { get: () => () => {}, set: () => true }),
            addEventListener: () => {},
            getBoundingClientRect: () => ({
                left: 0,
                top: 0,
                width: 600,
                height: 200,
            }),
        });
        const mk = () =>
            TC.createTrajectoryChart({
                canvas: stub(),
                getSeries: () => [],
                getDurationS: () => 300,
                isVisible: () => true,
            });

        const tab1 = mk();
        const monitor = mk();
        eq(
            winCalls.add,
            ["pointerup", "pointerup"],
            "两实例各自挂了一条 window 兜底",
        );
        eq(roCalls.observe, 2, "两实例各自观察了自己的父盒");

        monitor.destroy();
        eq(winCalls.remove, ["pointerup"], "拆 Monitor 那张只退掉它自己那条");
        eq(roCalls.disconnect, 1, "只断开它自己的 ResizeObserver");
        // 另一张仍然活着:视口还能动(拆错了这里会抛或不动)
        tab1.timeline.set({ startS: 10, endS: 60 });
        eq(
            tab1.viewport(),
            { startS: 10, endS: 60 },
            "另一张图不受影响,照常工作",
        );
        check(
            tab1.following() === false || tab1.following() === true,
            "另一张图的跟随态可读",
        );

        monitor.destroy();
        monitor.destroy();
        eq(winCalls.remove, ["pointerup"], "重复 destroy 不重复退订(幂等)");
        eq(roCalls.disconnect, 1, "ResizeObserver 也只断开一次");

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
    } finally {
        if (savedWin === undefined) delete globalThis.window;
        else globalThis.window = savedWin;
        if (savedRO === undefined) delete globalThis.ResizeObserver;
        else globalThis.ResizeObserver = savedRO;
    }
}

{
    // 接法:页面确实在窗口收起时拆图(不拆就是每开一次漏一组订阅)
    const app = src("web/monitor/app.js");
    check(
        /addEventListener\("pagehide", \(\) => \{[\s\S]{0,200}traj\.destroy\(\)/.test(
            app,
        ),
        "pagehide 时 destroy() 轨迹图",
    );
    check(
        /clearTimeout\(staleTimer\)/.test(app),
        "pagehide 时把停更定时器也清掉(不留孤儿 timer)",
    );
}

// =============================================================================
if (fail > 0) {
    console.error(`\n=== 失败 ${fail} 条 ===`);
    process.exit(1);
}
log("\n=== 结果:全部通过 ===");
process.exit(0);
