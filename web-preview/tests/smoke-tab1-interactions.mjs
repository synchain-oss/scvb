// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Tab1 交互接线冒烟(node,无 DOM;T31 Wave 2)
// =============================================================================
// 为什么是「纯逻辑断言」而不是驱动真 DOM:仓内无 jsdom/linkedom 依赖(零 node_modules),
// 而 T31 Wave 2 的可回归面恰好都能提成纯函数 —— web/output/tab-master.js 顶部那一段
// (§一「纯函数」)就是为此切出来的:模块顶层零 document 触碰,node 可直接 import。
// DOM 侧(属性写入、指针拖动、确认条展开)归浏览器手测,由统筹在 8823 端口逐屏走。
//
// 跑什么:
//   ① 契约映射的纯函数:§2.1 深合并、§1.2/§2.6 采集四态、03 §2.2 输出三态(含 §1.34 守卫)、
//      §1.8 三档与 manual 校验、§1.12-§1.14 参数域、§1.20 斜坡、§2.3 N/15、§2.4 位图、
//      §2.1 pan_curve 折线、§1.5/§1.6 scope 形状;
//   ② **Wave 1 静态填数已被事件驱动取代且数字不变**:同一份 FIFTEEN_TRACKS 下,
//      新的 segmentTotals/coveragePercent/analyzedRegions 与 Wave 1 的 WAVE1_NUMBERS 口径同值;
//   ③ mock 端到端:requestInitialState → 深合并 store → 三件套/组/版本/Range 的上行调用
//      与拒绝态(observer / noLoop / printing / badArg)按契约返回;
//   ④ 词条:T31 Wave 2 新增 key 三语齐、占位符三语一致、05 §5 禁词零命中、
//      guide.rule1..9 缺失判定(引导页占位注记的触发条件)。
//
// 用法:node web-preview/tests/smoke-tab1-interactions.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有断言失败(逐条打印 [FAIL])。
// =============================================================================

import { pathToFileURL, fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";
import { readFileSync } from "node:fs";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const u = (p) => pathToFileURL(join(ROOT, p)).href;

const TM = await import(u("web/output/tab-master.js"));
const { createBridge } = await import(u("web/shared/bridge.js"));
const { T } = await import(u("web/shared/i18n.js"));
const { FIFTEEN_TRACKS } = await import(u("web/shared/mock-data.js"));
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
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// =============================================================================
log("=== ① 契约映射的纯函数 ===");

// §2.1 字段纪律:full:false = 增量,只含变化子树,UI 做深合并;数组是叶子(整体替换)
{
    const base = {
        group_id: 1,
        global: {
            capture_enabled: false,
            range: { mode: "follow", start_s: 0 },
        },
        channels: [{ enabled: true }, { enabled: true }],
    };
    const merged = TM.deepMerge(base, {
        global: { capture_enabled: true },
        channels: [{ enabled: false }],
    });
    check(merged.group_id === 1, "深合并:未提及的顶层字段保留");
    check(merged.global.capture_enabled === true, "深合并:子树字段被覆盖");
    eq(
        merged.global.range,
        { mode: "follow", start_s: 0 },
        "深合并:同级兄弟保留",
    );
    check(merged.channels.length === 1, "深合并:数组整体替换(不逐元素合并)");
    check(base.global.capture_enabled === false, "深合并:不改写入参");
}

// §1.2 语义行 + §2.6:采集四态
{
    const on = { global: { capture_enabled: true } };
    const off = { global: { capture_enabled: false } };
    eq(TM.captureVisual(off, null), "off", "采集四态 OFF");
    eq(
        TM.captureVisual(on, null),
        "armed",
        "采集四态 已布防·等待播放(无 playhead)",
    );
    eq(
        TM.captureVisual(on, { isPlaying: false, inRange: true }),
        "armed",
        "采集四态 已布防(停止)",
    );
    eq(
        TM.captureVisual(on, { isPlaying: true, inRange: true }),
        "capturing",
        "采集四态 采集中",
    );
    eq(
        TM.captureVisual(on, { isPlaying: true, inRange: false }),
        "outside",
        "采集四态 已离开采集范围",
    );
}

// 03 §2.2 三态 + §1.3/§1.34:守卫未确认时行为止于 ARMED
{
    const playing = { isPlaying: true, inRange: true };
    eq(
        TM.outputPhase({ global: { output_enabled: false } }, playing),
        "follow",
        "输出三态 FOLLOW",
    );
    eq(
        TM.outputPhase(
            { global: { output_enabled: true } },
            { isPlaying: false },
        ),
        "armed",
        "输出三态 ARMED(停止)",
    );
    eq(
        TM.outputPhase({ global: { output_enabled: true } }, playing),
        "print",
        "输出三态 PRINT",
    );
    eq(
        TM.outputPhase(
            {
                global: { output_enabled: true },
                print_guard: { pending: true },
            },
            playing,
        ),
        "armed",
        "加载守卫未确认 ⇒ 止于 ARMED(契约 §1.3/§1.34)",
    );
}

// §1.12-§1.14 参数域 + 设计稿读数
{
    eq(TM.widthAngleDeg(100), 60, "Width 默认 100% ⇒ ±60°(设计稿静态值)");
    eq(
        TM.widthAngleDeg(150),
        90,
        "Width 满档 150% ⇒ ±90°(与 widthAngleHint 自洽)",
    );
    eq(TM.widthAngleDeg(0), 0, "Width 0% ⇒ ±0°");
    check(
        Math.abs(TM.sliderPct(100, 0, 150) - 66.666) < 0.01,
        "Width 100 的 --v ≈ 66.7%(Wave 1 静态行内值)",
    );
    eq(TM.msFill(0), { left: 50, width: 0, knob: 50 }, "MS 0 ⇒ 中点零宽填充");
    eq(TM.msFill(100), { left: 50, width: 50, knob: 100 }, "MS +100 ⇒ 右满");
    eq(TM.msFill(-100), { left: 0, width: 50, knob: 0 }, "MS −100 ⇒ 左满");
    eq(TM.msReading(0), "0", "MS 读数 0 不带号");
    eq(TM.msReading(-35), "−35", "MS 读数负值带 U+2212");
    eq(TM.PARAM_DEFAULTS.width, 100, "双击回默认:width=100(契约 §1.12 全集行)");
    eq(TM.PARAM_DEFAULTS.ms_balance, 0, "双击回默认:ms_balance=0");
    eq(TM.PARAM_DEFAULTS.lead_select, 0, "双击回默认:lead_select=0");
    eq(TM.PARAM_RANGES.lead_select.max, 15, "lead_select 上限 = 15(契约 §0.2)");
}

// §1.20 斜坡:ms=80 必须复现设计稿的静态几何(112 / 146)
{
    const g80 = TM.rampGeometry(80);
    eq([g80.x0, g80.x1], [112, 146], "斜坡 ms=80 复现设计稿静态拐点");
    eq(g80.d, "M0 16 L112 16 L146 5 L300 5", "斜坡 ms=80 复现设计稿 path");
    const g20 = TM.rampGeometry(20);
    const g300 = TM.rampGeometry(300);
    check(g20.x1 - g20.x0 < g80.x1 - g80.x0, "斜坡:20ms 比 80ms 更陡");
    check(g300.x1 - g300.x0 > g80.x1 - g80.x0, "斜坡:300ms 比 80ms 更缓");
    check(g300.x0 >= 0 && g300.x1 <= 300, "斜坡:上限档仍在 viewBox 内");
    eq(TM.rampGeometry(1e9).x1, g300.x1, "斜坡:越界夹取到 300ms(契约 §1.20)");
}

// §2.3 / §2.4:N/15 与组位图
{
    const conn = {
        channels: [
            { slotState: 2, heartbeatFresh: true },
            { slotState: 2, heartbeatFresh: false }, // 心跳陈旧不计
            { slotState: 1, heartbeatFresh: true }, // 仅「已声明」不计
            { slotState: 0, heartbeatFresh: false },
        ],
    };
    eq(
        TM.connectedCount(conn),
        1,
        "N/15 只数 slotState=2 ∧ heartbeatFresh(J01)",
    );
    eq(TM.connectedCount(null), 0, "conn 缺失 ⇒ 0/15,零报错");
    eq(
        TM.misalignedTracks({ channels: [{ misalignCount: 3 }, {}] }),
        1,
        "失准轨计数",
    );
    eq(TM.groupOnline(0b00010011, 1), 1, "groups 位图 bit0 = 组 A");
    eq(TM.groupOnline(0b00010011, 2), 1, "groups 位图 bit1 = 组 B");
    eq(TM.groupOnline(0b00010011, 3), 0, "groups 位图 bit2 = 组 C(灭)");
    eq(TM.groupOnline(0b00010011, 5), 1, "groups 位图 bit4 = 组 E");
    eq(TM.groupOnline(0, 1), 0, "事件缺失(位图 0)⇒ 绿点全灭,零报错");
}

// §1.5/§1.6:scope 形状 + §0.2 tracksMask
{
    const channels = [
        { enabled: true },
        { enabled: false },
        { enabled: true },
    ].concat(Array.from({ length: 12 }, () => ({ enabled: false })));
    eq(TM.tracksMaskOf(channels), 0b101, "tracksMask bit0=ch1 … bit14=ch15");
    check(
        (TM.tracksMaskOf(
            Array.from({ length: 15 }, () => ({ enabled: true })),
        ) &
            0x8000) ===
            0,
        "tracksMask bit15 保留为 0",
    );
    eq(
        TM.analyzeScope({ global: { range: { mode: "follow" } }, channels }),
        "all",
        'follow 档无界 ⇒ scope = "all"(§5.3 无哨兵约定)',
    );
    eq(
        TM.analyzeScope({
            global: { range: { mode: "manual", start_s: 12, end_s: 96 } },
            channels,
        }),
        { tracksMask: 0b101, startS: 12, endS: 96 },
        "manual 档 ⇒ scope 对象带起止",
    );
}

// §1.8 manual 起止校验(mm:ss.mmm ↔ 秒;桥面单位只有秒)
{
    eq(TM.secondsToTimecode(72.4), "01:12.400", "秒 → mm:ss.mmm");
    eq(
        TM.secondsToTimecode(72.9996),
        "01:13.000",
        "毫秒四舍五入进位不产生四位 ms(踩坑口径锁死)",
    );
    eq(TM.secondsToTimecode(59.9996), "01:00.000", "毫秒进位链到分钟");
    eq(TM.timecodeToSeconds("01:12.400"), 72.4, "mm:ss.mmm → 秒");
    eq(TM.timecodeToSeconds("42"), 42, "裸秒数也收");
    eq(TM.timecodeToSeconds(""), null, "空串非法");
    eq(TM.timecodeToSeconds("abc"), null, "乱码非法");
    eq(TM.timecodeToSeconds("01:99.000"), null, "秒位 >59 非法");
    eq(
        TM.RANGE_UI_TO_MODE.loop,
        "daw_loop",
        "档位短名 ↔ 契约枚举:loop → daw_loop",
    );
    eq(
        TM.RANGE_MODE_TO_UI.daw_loop,
        "loop",
        "档位短名 ↔ 契约枚举:daw_loop → loop",
    );
}

// write 确认条 / footer:follow 档必须走 .follow 变体(否则出现 {x}–{y} 空洞)
{
    eq(
        TM.writeConfirmKey("follow"),
        "out.master.writeConfirm.follow",
        "follow 档 write 确认条走 .follow 变体",
    );
    eq(
        TM.writeConfirmKey("manual"),
        "out.master.writeConfirm",
        "manual 档走主条",
    );
    eq(
        TM.footerPrintKey("follow", false),
        "footer.printing.follow",
        "footer 打印中(follow)",
    );
    eq(
        TM.footerPrintKey("daw_loop", true),
        "footer.printDone",
        "footer 打印结束(loop)",
    );
    for (const key of [
        TM.writeConfirmKey("follow"),
        TM.footerPrintKey("follow", false),
        TM.footerPrintKey("follow", true),
    ]) {
        // 先断言键在三语字典里存在且非空 —— 否则 String(undefined) 里没有
        // {x}/{y},下面的空洞断言会对缺失键静默放行(PR #52 bot 加固建议)。
        for (const lang of ["zh", "en", "fr"]) {
            check(
                typeof T[lang][key] === "string" && T[lang][key].trim() !== "",
                `follow 变体 ${key} 在 ${lang} 字典缺失或为空`,
            );
        }
        check(
            !/\{x\}|\{y\}/.test(T.zh[key] || ""),
            `follow 变体 ${key} 不得含 {x}/{y} 空洞`,
        );
    }
}

// §2.1:pan_curve 折线(统筹增补③:有点集就画线,不再停在空态)
{
    eq(TM.panCurvePath([]), "M0 109 L660 109", "空点集 ⇒ 0 dB 退化直线");
    const path = TM.panCurvePath([
        { angle: 0, gain_db: 0 },
        { angle: -100, gain_db: 12 },
        { angle: 100, gain_db: -12 },
    ]);
    check(path.startsWith("M0 35"), "折线:按 angle 升序,最左点为 −100/+12dB");
    check(path.endsWith("L660 183"), "折线:右端延到画布边缘并保持末点值");
    check(path.includes("L330 109"), "折线:angle=0 / 0 dB 落在中线交点");
}

// §2.2 + §0.5:本地乐观值(paramEcho)必须逐帧让位 —— 评审 P1 的回归面。
// 缺了这条,任一次本地拖动都会把该 ParamID 的显示永久遮蔽:宿主自动化回吐(hostEcho)
// 与切版本的 full:true 全量重发都再进不了 UI。
{
    const echo = { width: 130, ms_balance: -20 };
    eq(
        TM.nextParamEcho(echo, { values: { width: 100 }, full: true }, null),
        {},
        "full:true(切版本全量重发)⇒ 乐观值整表作废",
    );
    eq(
        TM.nextParamEcho(
            echo,
            { values: { width: 100 }, hostEcho: true },
            null,
        ),
        { ms_balance: -20 },
        "hostEcho 增量批次:本帧提到的 id 让位,其余保留",
    );
    eq(
        TM.nextParamEcho(echo, { values: { width: 100 } }, "width"),
        { width: 130, ms_balance: -20 },
        "拖动中的 id 不让位(松手前让位会与自己的回声打架)",
    );
    eq(TM.nextParamEcho(echo, null, null), {}, "空载荷不抛错(按整表作废处理)");
    eq(echo, { width: 130, ms_balance: -20 }, "nextParamEcho 不改入参");

    // T33:full 分支与 Tab2 dropParamEcho 口径对齐(t32/deviations §O 登记项)——
    // 整表作废,**唯拖动中的 id 保留到松手**;指针还按在把手上时不许被全量广播抢跳。
    eq(
        TM.nextParamEcho(echo, { values: { width: 100 }, full: true }, "width"),
        { width: 130 },
        "full:true 拖动中的 id 保留(与 Tab2 dropParamEcho 同口径)",
    );
    eq(
        TM.nextParamEcho(
            echo,
            { values: { width: 100 }, full: true },
            "lead_select",
        ),
        {},
        "full:true 时拖动中的 id 不在表里 ⇒ 仍是空表(不凭空造 undefined 项)",
    );
    eq(echo, { width: 130, ms_balance: -20 }, "full 分支同样不改入参");
    // 两 tab 的 full 分支必须都留 gesture:Tab2 侧是源码级不变式(它没有纯函数出口)
    check(
        /if \(!payload \|\| payload\.full\) \{[\s\S]{0,600}?id !== local\.gesture/.test(
            readFileSync(join(ROOT, "web/output/tab-tracks.js"), "utf8"),
        ),
        "Tab2 dropParamEcho 的 full 分支仍保留拖动中的 id(两 tab 口径一致)",
    );
}

// §1.6:分析按钮「无数据」= 覆盖 ∪ 段表并集判空 —— 此口径两度踩坑
// (PR #52 首审【重要】鸡生蛋 + pr-agent 补丁的覆盖帧未到回归面),三形态锁死。
{
    check(
        !TM.analyzeNoData(84, 0),
        "首采未析(覆盖>0,段表空)⇒ 可分析(鸡生蛋回归面)",
    );
    check(
        !TM.analyzeNoData(null, 327),
        "重开工程未播放(§2.7 无覆盖帧,段表有货)⇒ 可分析",
    );
    check(TM.analyzeNoData(null, 0), "双空 ⇒ 禁用(真无数据)");
    check(
        TM.analyzeNoData(0, 0),
        "覆盖 0%(range ∩ coverage = ∅)且段表空 ⇒ 禁用",
    );
}

// 分布图几何(设计稿 L2037-2056)
{
    const g = TM.distGeometry(0, -6, 100);
    eq(g.x, 50, "分布图:pan=0 ⇒ 横位 50%");
    check(g.h > 8 && g.h < 88, "分布图:柱高在夹取区间内");
    eq(
        TM.distGeometry(-100, 0, 100).half,
        0,
        "分布图:贴左边框时张开半宽被夹到 0",
    );
}

// =============================================================================
log("=== ② Wave 1 静态填数 → 事件驱动,同一 fixture 下数字不变 ===");
{
    // Wave 1 的 WAVE1_NUMBERS 口径(app.js 旧实现,已删除)在此原样复算一次作对照。
    const chans = FIFTEEN_TRACKS.segments.channels;
    const cov = FIFTEEN_TRACKS.captureProgress.channels;
    const wave1 = {
        n: chans.reduce((s, c) => s + c.segments.length, 0),
        m: chans.filter((c) => c.segments.length > 0).length,
        k: FIFTEEN_TRACKS.segments.diff.kept,
        p: Math.round(cov.reduce((s, c) => s + c.coveragePct, 0) / cov.length),
    };
    const totals = TM.segmentTotals(FIFTEEN_TRACKS.segments);
    const coverage = {};
    for (const c of cov) coverage[c.ch] = c.coveragePct;

    eq(
        { n: totals.n, m: totals.m, k: totals.k },
        { n: wave1.n, m: wave1.m, k: wave1.k },
        "事件驱动的 {n}/{m}/{k} 与 Wave 1 静态值同值",
    );
    eq(
        TM.coveragePercent(coverage),
        wave1.p,
        "事件驱动的 {p}% 与 Wave 1 静态值同值",
    );
    eq(
        TM.coveragePercent({}),
        null,
        "一轨都没报到 ⇒ null(调用方隐掉整行,不显示假 0%)",
    );
    eq(TM.coveragePercent({ 3: 80, 7: 90 }), 85, "分母取已报到轨数,首帧即稳定");
    check(
        totals.n > 0 && totals.m > 0,
        "fifteen-tracks 下影响面非空(否则分析按钮会误判 disabled)",
    );

    // 「已分析区域共 {n} 段 · 合计 {t}」取 scvb.segments 的段表(§2.8),不是采集增量
    const regions = TM.analyzedRegions(FIFTEEN_TRACKS.segments);
    check(regions.n > 0, "follow 档提示行 {n} 段 > 0");
    check(/^\d+:\d{2}$/.test(regions.t), "follow 档提示行 {t} 是 m:ss");
    check(
        regions.n <= totals.n,
        "已分析区域段数 ≤ 区段总数(15 轨的段在时间轴上有重叠,合并后只会更少)",
    );
    eq(
        TM.analyzedRegions(null),
        { n: 0, t: "0:00" },
        "无段表 ⇒ 0 段 / 0:00,零报错",
    );
}

// =============================================================================
log("=== ③ mock 端到端:首帧 → 深合并 store → 上行调用与拒绝态 ===");

/** 把 app.js 的 store 合并规则原样复用一遍(事件 → 单向渲染源)。 */
function makeStore() {
    return {
        state: {},
        params: { values: {} },
        conn: null,
        groups: 0,
        segments: null,
        errors: new Map(),
    };
}
function stripFull(s) {
    const out = { ...(s || {}) };
    delete out.full;
    return out;
}

async function openSession(params) {
    const s = driver.createPreviewSession({ role: "output", params });
    const bridge = createBridge({ role: "output", mockBackend: s.mock });
    const store = makeStore();
    bridge.on("scvb.state", (p) => {
        store.state =
            p && p.full
                ? stripFull(p)
                : TM.deepMerge(store.state, stripFull(p));
    });
    bridge.on("scvb.params", (p) => {
        store.params = {
            values: p.full
                ? { ...p.values }
                : { ...store.params.values, ...p.values },
            hostEcho: !!p.hostEcho,
        };
    });
    bridge.on("scvb.conn", (c) => (store.conn = c));
    bridge.on("scvb.groups", (g) => (store.groups = g.groups_online));
    bridge.on("scvb.segments", (seg) => (store.segments = seg));
    bridge.on("scvb.error", (e) => {
        if (e.active === false) store.errors.delete(e.code);
        else store.errors.set(e.code, e);
    });
    s.start();
    const snap = await bridge.requestInitialState();
    store.state = TM.deepMerge(store.state, stripFull(snap));
    store.conn = snap.conn || store.conn;
    await sleep(320); // 等一拍 conn(~4Hz)/ groups(1Hz)首帧
    return { session: s, bridge, store, snap };
}

{
    const { session, bridge, store, snap } = await openSession(
        "fixture=fifteen-tracks",
    );
    eq(TM.connectedCount(store.conn), 15, "fifteen-tracks:pill 显示 15/15");
    check(
        typeof snap.guide_seen_global === "boolean",
        "快照带 guide_seen_global(J50a 全局默认判定位)",
    );
    check(
        typeof store.state.ui.guide_seen === "boolean",
        "快照带 ui.guide_seen(持久化字段)",
    );
    check(
        store.state.print_guard.pending === false,
        "快照带运行时态 print_guard",
    );

    // 引导页首启判定(契约 §1.32 语义行):四种组合逐条走一遍
    eq(
        TM.shouldShowGuide(
            { ui: { guide_seen: false } },
            { guide_seen_global: false },
            false,
        ),
        true,
        "引导页:工程未看过 ∧ 全局未看过 ⇒ 弹",
    );
    eq(
        TM.shouldShowGuide(
            { ui: { guide_seen: false } },
            { guide_seen_global: true },
            false,
        ),
        false,
        "引导页:全局「不再显示」已置位 ⇒ 新工程也不弹(跨工程承诺)",
    );
    eq(
        TM.shouldShowGuide(
            { ui: { guide_seen: true } },
            { guide_seen_global: false },
            false,
        ),
        false,
        "引导页:本工程已看过 ⇒ 不弹",
    );
    eq(
        TM.shouldShowGuide(
            { ui: { guide_seen: false } },
            { guide_seen_global: false },
            true,
        ),
        false,
        "引导页:本会话已关过 ⇒ 不再弹",
    );
    eq(
        TM.shouldShowGuide({ ui: { guide_seen: false } }, null, false),
        false,
        "引导页:首帧未到(§0.6 门控)⇒ 不渲染真实数据态",
    );
    eq(
        TM.shouldShowGuide(store.state, snap, false),
        false,
        "fifteen-tracks 是「已看过」的世界 ⇒ 不弹",
    );

    // 三件套上行:setCaptureEnabled → scvb.state 回推 → 四态判定翻到 armed
    await bridge.setCaptureEnabled(true);
    await sleep(60);
    eq(
        store.state.global.capture_enabled,
        true,
        "setCaptureEnabled(true) 经 state 回推",
    );
    eq(
        TM.captureVisual(store.state, { isPlaying: false }),
        "armed",
        "回推后 data-cap 应为 armed",
    );

    // 影响预览:UI 用的 scope 形状必须被 mock 认下来(§1.5)
    const preview = await bridge.previewAnalyze(TM.analyzeScope(store.state));
    check(
        Number.isFinite(preview.intervals) && preview.intervals > 0,
        "previewAnalyze(UI scope) 回 {intervals,tracks,manualKept}",
    );

    // gesture 三段式:三个 ParamID 都在可驱动全集内(§1.12-§1.14)
    for (const id of ["width", "ms_balance", "lead_select"]) {
        const b = await bridge.beginParamGesture(id);
        const st = await bridge.setParam(id, TM.PARAM_DEFAULTS[id]);
        const e = await bridge.endParamGesture(id);
        check(b.ok && st.ok && e.ok, `gesture 三段式接受 ${id}`);
    }
    const bad = await bridge.setParam("not_a_param", 1);
    eq(
        bad,
        { ok: false, reason: "badArg" },
        "表外 ParamID 不得静默忽略(§1.12 返回行)",
    );

    // Range 三档 + manual 校验
    eq(
        (await bridge.setRange("follow", 0, 0)).ok,
        true,
        "setRange(follow) 成立",
    );
    eq(
        (await bridge.setRange("daw_loop", 0, 0)).ok,
        true,
        "setRange(daw_loop) 成立(宿主有 loop)",
    );
    eq(
        await bridge.setRange("manual", 96, 12),
        { ok: false, reason: "badArg" },
        "manual 且 start ≥ end ⇒ badArg(UI 侧同条件不发调用)",
    );
    await bridge.setRange("manual", 12, 96);
    await sleep(60);
    eq(store.state.global.range.mode, "manual", "manual 档回推");
    eq(
        TM.analyzeScope(store.state),
        {
            tracksMask: TM.tracksMaskOf(store.state.channels),
            startS: 12,
            endS: 96,
        },
        "manual 档下 scope 用 state 的起止",
    );

    // 版本:setVersionActive / setVersionName(≤16、空值回落)
    eq((await bridge.setVersionActive(2)).ok, true, "setVersionActive(2)");
    await sleep(60);
    eq(store.state.global.version_active, 2, "version_active 回推");
    const named = await bridge.setVersionName(2, "  ");
    eq(named, { ok: true, name: "V2" }, "空白名回落默认 V{v}(§1.10)");
    const long = await bridge.setVersionName(2, "0123456789abcdefGHIJ");
    check(long.ok && long.name.length === 16, "超长名由 C++ 截断到 16 并回显");

    // 组:setGroupId 成功后 group_id 回推
    eq((await bridge.setGroupId(3)).ok, true, "setGroupId(3)");
    await sleep(60);
    eq(store.state.group_id, 3, "group_id 回推");
    eq(TM.GROUP_IDS[store.state.group_id - 1], "C", "组号 3 ⇒ 显示 C");

    // 过渡:越界夹取回推(§1.20)
    await bridge.setTransitionRamp(9999);
    await sleep(60);
    eq(
        store.state.analysis.transition_ramp_ms,
        300,
        "setTransitionRamp 越界夹取到 300",
    );

    // 加载守卫:confirmPrintGuard 幂等(§1.34)
    eq(
        (await bridge.confirmPrintGuard()).ok,
        true,
        "confirmPrintGuard() 幂等回 ok",
    );

    // 引导页:setGuideSeen(true, alsoGlobal=true) 是唯一写入口(§1.32)
    eq(
        (await bridge.setGuideSeen(true, true)).ok,
        true,
        "setGuideSeen(true, true)",
    );
    await sleep(60);
    eq(
        store.state.ui.guide_seen,
        true,
        "guide_seen 回推 ⇒ 新工程不再弹(勾选→不再显示)",
    );

    // tab:setActiveTab 写 ui.active_tab(§1.31)
    eq((await bridge.setActiveTab("wave")).ok, true, "setActiveTab(wave)");
    eq(
        await bridge.setActiveTab("nope"),
        { ok: false, reason: "badArg" },
        "未知 tab ⇒ badArg",
    );

    // 缩放:档位表外 ⇒ badArg(§1.28)
    eq((await bridge.setUiScale(1.25)).ok, true, "setUiScale(1.25) 在档位表内");
    eq(
        await bridge.setUiScale(1.11),
        { ok: false, reason: "badArg" },
        "setUiScale 档位表外 ⇒ badArg",
    );
    eq((await bridge.commitUiScale()).ok, true, "commitUiScale() 落盘全局默认");

    session.stop();
}

// 无宿主循环区:档位 disabled 但绝不隐藏(§1.8 noLoop)
{
    const { session, bridge } = await openSession(
        "fixture=stereo-mixed&loop=none",
    );
    eq(
        await bridge.setRange("daw_loop", 0, 0),
        { ok: false, reason: "noLoop" },
        "宿主不提供 loop ⇒ noLoop(UI 置 data-loop-missing=1)",
    );
    session.stop();
}

// 只读观察态:写控件全 disabled,C++ 侧回 {observer:true} 且不改 state(§5.1 secondOutput)
{
    const { session, bridge, store } = await openSession(
        "fixture=second-output",
    );
    check(
        store.errors.has("secondOutput"),
        "second-output fixture 发 secondOutput 错误码",
    );
    eq(
        await bridge.setGroupId(5),
        { observer: true },
        "只读观察态 setGroupId ⇒ {observer:true}",
    );
    session.stop();
}

// 路由失准:横幅① 的 {m} 由 scvb.conn.misalignCount 数出来
{
    const { session, store } = await openSession("fixture=misaligned");
    check(
        TM.misalignedTracks(store.conn) > 0,
        "misaligned fixture 下失准轨数 > 0(横幅① 的 {m})",
    );
    session.stop();
}

// 空工程:A1 空态卡的两个条件(零连接 ∧ 无段表)同时成立
{
    const { session, store } = await openSession("fixture=empty");
    eq(TM.connectedCount(store.conn), 0, "empty fixture:0/15");
    eq(TM.segmentTotals(store.segments).n, 0, "empty fixture:段表为空");
    session.stop();
}

// =============================================================================
log("=== ④ 词条(T31 Wave 2 新增 + 05 §5 禁词)===");
{
    const NEW_KEYS = [
        "banner.misaligned",
        "banner.srMismatch",
        "banner.sidecarMissing",
        "banner.noTimeline",
        "master.versionArmedConfirm",
        // 空态卡五步制(用户裁定 2026-08-18:原 prerequisite 红字并入步 2/3)
        "master.empty.step1",
        "master.empty.step2",
        "master.empty.step3",
        "master.empty.step4",
        "master.empty.step5",
        "common.continue",
        "master.msEyebrow",
        "master.leadEyebrow",
        "master.transitionEyebrow",
        // 评审修订(P2-4 / P2-6):toast①② + 缩放确认框 + PRINT 态三处 tooltip
        "toast.projectCopy",
        "toast.sidecarSwitched",
        "scale.confirmBody",
        "scale.keep",
        "master.printLock.group",
        "master.printLock.version",
        "master.printLock.copy",
    ];
    for (const k of NEW_KEYS) {
        for (const lang of ["zh", "en", "fr"]) {
            check(
                typeof T[lang][k] === "string" && T[lang][k].trim() !== "",
                `新增词条 ${lang}.${k} 缺失或为空`,
            );
        }
        const ph = (s) =>
            [...new Set(s.match(/\{[A-Za-z_]\w*\}/g) || [])].sort().join(",");
        check(
            ph(T.zh[k]) === ph(T.en[k]) && ph(T.zh[k]) === ph(T.fr[k]),
            `新增词条 ${k} 三语占位符不一致`,
        );
    }
    eq(
        TM.format(T.zh["banner.srMismatch"], { n: 7 }),
        "轨 7 采样率不一致,已禁用",
        "banner③ 占位符 {n} 可填",
    );
    check(
        /\{m\}/.test(T.zh["banner.misaligned"]),
        "banner① 带 {m} 占位符(失准轨数)",
    );

    // eyebrow 三语同值(视觉层 mono 大写元素,照设计稿不译)
    for (const k of ["master.msEyebrow", "master.leadEyebrow"]) {
        check(
            T.zh[k] === T.en[k] && T.zh[k] === T.fr[k],
            `eyebrow ${k} 三语应同值(统筹 Wave 2 增补①)`,
        );
    }

    // 05 §5 禁词。判定面是**会被用户看到的字**:
    //   • 字典 = 三语的全部**值**(注释里为解释纪律而引用禁词本身不算命中 —— i18n.js 的
    //     tour.step4 措辞纪律注释就写着「不得出现『写入完成』类表述」,把它判红等于禁止写纪律);
    //   • 页面/脚本 = 全文(这三个文件的注释里本来就不该出现禁词)。
    const BANNED = ["写入完成", "推子后", "post-fader", "六条"];
    for (const lang of ["zh", "en", "fr"]) {
        for (const [k, v] of Object.entries(T[lang])) {
            for (const banned of BANNED) {
                check(
                    !String(v).includes(banned),
                    `词条 ${lang}.${k} 命中禁词「${banned}」`,
                );
            }
        }
    }
    for (const f of [
        "web/output/app.js",
        "web/output/tab-master.js",
        "web/output/index.html",
    ]) {
        const text = readFileSync(join(ROOT, f), "utf8");
        for (const banned of BANNED) {
            check(!text.includes(banned), `${f} 命中禁词「${banned}」`);
        }
    }

    // guide.rule1..9:字典没有 ⇒ 引导页必须走占位注记分支(T39b 生成器未落地)
    const ruleKeys = Array.from(
        { length: 9 },
        (_, i) => "guide.rule" + (i + 1),
    );
    const have = ruleKeys.filter((k) =>
        Object.prototype.hasOwnProperty.call(T.zh, k),
    );
    check(
        have.length === 0 || have.length === 9,
        "guide.rule1..9 只能「全有」或「全无」,不得半套",
    );
    if (have.length === 0) {
        log(
            "  (guide.rule1..9 未生成 ⇒ 引导页渲染占位注记,符合 T39b 未落地的现状)",
        );
    }
}

// =============================================================================
log("=== ⑤ 评审修订(对抗校验 findings)的源码级不变式 ===");
// 这一段断言的是「改回去就红」的几处**源码事实**:它们的运行期表现落在 DOM 上
// (CSS 命中、title 属性、事件订阅顺序),纯 node 侧无从驱动,但退化都是一行改动,
// 因此用最小的文本不变式钉住,免得后续波次无意中回退。
{
    const html = readFileSync(join(ROOT, "web/output/index.html"), "utf8");
    const appJs = readFileSync(join(ROOT, "web/output/app.js"), "utf8");
    const tabJs = readFileSync(join(ROOT, "web/output/tab-master.js"), "utf8");

    // P2-2:hostEcho 只灰显、不禁操作(契约 §1.12-§1.14 拒绝态「无」)
    const echoRule = /\[data-host-echo="1"\]\s*\{([^}]*)\}/.exec(html);
    check(!!echoRule, 'index.html 仍有 [data-host-echo="1"] 灰显规则');
    if (echoRule) {
        check(
            /opacity/.test(echoRule[1]),
            "hostEcho 规则保留 opacity 灰显(05 §2.1 ④「跟随」的视觉提示)",
        );
        check(
            !/pointer-events/.test(echoRule[1]),
            "hostEcho 规则不得含 pointer-events:none(会把四张参数卡永久锁死)",
        );
    }
    check(
        !/params\.hostEcho/.test(
            /function isParamBlocked\(\)\s*\{[^}]*\}/.exec(tabJs)?.[0] || "",
        ),
        "isParamBlocked() 不得再按 hostEcho 阻断 gesture",
    );

    // P1:scvb.params 订阅里必须让乐观值失效,且排在**请求重渲染**之前。
    // T33 起整页渲染走 rAF 合帧的 requestRender()(app.js「渲染」小节),
    // 排序不变式照旧 —— 尾包的 requestRender() 必须在两个 onParams 之后。
    const paramsSub = /bridge\.on\("scvb\.params"[\s\S]*?\n    \}\);/.exec(
        appJs,
    );
    check(!!paramsSub, "app.js 仍订阅 scvb.params");
    if (paramsSub) {
        const body = paramsSub[0];
        check(
            body.includes("tabMaster.onParams("),
            "scvb.params 订阅调用 tabMaster.onParams()(乐观值逐帧失效)",
        );
        // 末尾那一句请求重渲染的位置(取最后一次出现,hostEcho 的定时器兜底不算)
        const renderAt = body.lastIndexOf("requestRender()");
        check(
            renderAt > 0 && body.indexOf("tabMaster.onParams(") < renderAt,
            "onParams 必须排在请求重渲染之前(否则本帧仍读到已作废的乐观值)",
        );
        check(
            body.indexOf("tabTracks.onParams(") < renderAt,
            "Tab2 的 onParams 同款排在请求重渲染之前",
        );
    }

    // P2-3:footer 打印行填时间码而不是裸秒数(词条写「小节」,桥面无 tempo map,A26)
    check(
        !/fmtS\(/.test(appJs),
        "footer 打印行不再用裸秒数 fmtS()(会读成「12–96 小节」)",
    );
    check(
        /footerPrintKey\(range\.mode, false\)[\s\S]{0,120}secondsToTimecode\(range\.start_s\)/.test(
            appJs,
        ),
        "footer 打印行 {x}/{y} 填 secondsToTimecode(桥面单位,契约 §1.8)",
    );

    // P2-4:复制按钮 PRINT 态 disabled + tooltip,且 rejected 不静默
    check(
        /\.hdr-copy-btn\[data-disabled="1"\]/.test(html),
        "index.html 有「复制到…」按钮的 disabled 视觉(05 §2.1 ③)",
    );
    check(
        /copyBtn\.setAttribute\("data-disabled"/.test(appJs),
        "renderHeader 同步「复制到…」的 data-disabled",
    );
    check(
        /master\.printLock\.copy/.test(appJs) &&
            /master\.printLock\.version/.test(appJs) &&
            /master\.printLock\.group/.test(tabJs),
        "PRINT 态三处 tooltip 均已接词条(A22 兑现)",
    );
    check(
        /copyVersion[\s\S]{0,200}rejected === "printing"/.test(appJs),
        'copyVersion 的 {rejected:"printing"} 有渲染落点(§5.6 双保险)',
    );
    check(
        /rejectedPrintingUntil/.test(appJs),
        "rejected 记进 store 由 renderHeader 统一渲染(就地 setAttribute 会被下一拍抹掉)",
    );

    // P2-5:守卫确认与 write 确认条互斥(05 §2.0 横幅⑦)
    check(
        /confirmPrintGuard[\s\S]{0,400}session\.writeConfirmSeen = true/.test(
            appJs,
        ),
        "confirmPrintGuard 确认后置 writeConfirmSeen(守卫已确认即不再补弹)",
    );

    // P2-6:本波接线的交互组件零硬编码中文
    check(
        /data-gb="toast-projectCopy"[\s\S]{0,400}data-t="toast\.projectCopy"/.test(
            html,
        ),
        "toast① 走词条 toast.projectCopy",
    );
    check(
        /data-gb="toast-sidecarSwitched"[\s\S]{0,400}data-t="toast\.sidecarSwitched"/.test(
            html,
        ),
        "toast② 走词条 toast.sidecarSwitched",
    );
    // 从 DOM 那一处起算(CSS 里也有 [data-gb="scale-confirm"] 选择器,不能作锚点)
    const scaleBlock =
        /<div class="sc-scrim" data-gb="scale-confirm"[\s\S]*?data-gb="scale-confirm-keep"[\s\S]{0,200}?<\/button>/.exec(
            html,
        );
    check(!!scaleBlock, "index.html 仍有缩放确认框");
    if (scaleBlock) {
        check(
            !/data-gb-todo/.test(scaleBlock[0]),
            "缩放确认框内零「词条待立」(正文/取消/保持三处均已立 key)",
        );
        check(
            /data-t="common\.cancel"/.test(scaleBlock[0]) &&
                /data-t="scale\.keep"/.test(scaleBlock[0]),
            "缩放确认框两钮走 common.cancel / scale.keep",
        );
    }
    check(
        /scale\.confirmBody/.test(appJs) &&
            /\{s\}/.test(T.zh["scale.confirmBody"]),
        "缩放确认正文由 scale.confirmBody 按 {s} 切三格填(applyI18n 会抹掉倒计时 span)",
    );

    // P3-8:嵌套预览沿用既有 scalePrev(回退到**进入预览前**的档位)
    check(
        /if \(!scaleTimer\) scalePrev = currentScale\(\);/.test(appJs),
        "倒计时进行中再次预览不得重新采样 scalePrev",
    );
}

// =============================================================================
log("=== ⑥ 分布图 rAF 补间(SL-203)===");

// 用户实测:Monitor 上了补间之后观感反超 Output。本卡把同一件(web/shared/dist-motion.js)
// 接到 Output 侧。补间器自身的行为断言已在 smoke-monitor ⑨ 节覆盖(同一件,不重复),
// 这里只守 Output 侧的**接线**与两条容易回头踩的纪律。
{
    const tm = readFileSync(join(ROOT, "web/output/tab-master.js"), "utf8");
    const html = readFileSync(join(ROOT, "web/output/index.html"), "utf8");

    check(
        /createDistMotion\(/.test(tm) &&
            /distMotion\.push\(rows, local\.chartHi\)/.test(tm),
        "renderDist 走补间器(不再每次重拼 innerHTML)",
    );
    check(!/distBarsHtml/.test(tm), "tab-master.js 里不再直接拼柱体");
    // 空闲零 rAF(05 §6.1):可见性两道闸必须与轨迹图**对称** —— 视图切到轨迹档、
    // 或 Tab1 不是当前页时不起 rAF。Output 的 scvb.params 仍以 25Hz 推着 render,
    // 少一道闸就是对着没人看的画面烧一条 60fps 循环。
    check(
        /currentChartMode\(\) === "distribution" && isPanelActive\(\)/.test(tm),
        "补间器的可见性闸 = 分布档 ∧ Tab1 在前台(与轨迹图对称)",
    );
    check(
        /getGlobalWidthPct:\s*\(\)\s*=>\s*readParam\("width"\)/.test(tm),
        "全局最大角度仍进几何(v5 P2-10:拧滑杆时分布图实时收拢/张开)",
    );

    // 几何**不许**进 CSS 过渡集 —— SL-192 在 Monitor 侧实测过:节点常驻之后
    // `transition: all` 会活过来,在补间下游再叠一层 ~300ms 低通,屏幕上的柱子追不上
    // 写入值(实测滞后 2.89 个百分点)。断言前先剥 CSS 注释:上面那段讲这个坑的
    // 注释里逐字写着 `transition: all`,不剥会把自己的注释当成回归。
    {
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
                `${sel} 不用 transition: all(几何进过渡集会把 rAF 补间叠成 300ms 低通)`,
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

// =============================================================================
log("=== ⑦ SL-241:复制版本切进去,分布图不许回落出厂默认 ===");
//
// 用户实测(Cubase 15 Pro,v5.6.2):复制版本后切到新版本,**声像显示全轨居中**,
// 一开始播放就正常。SL-211 把这一幕在 Tab2 修掉了(未冻结维度改读曲线段),
// SL-229 又给分布图补了**版本闸** —— 可病灶在**源**上:`copyVersion` 契约是
// 「零参数写入」(03 §5.3;`tests/core/test_version_params.cpp` VERSION-COPY-ZERO-1),
// 引擎打印头又只驱动**当前激活版本**,于是刚复制出来还没播过的版本,那 63 个 id
// 装的就是出厂默认(pan 居中)。分布图一直只读参数面,所以它照旧全轨居中。
//
// 本组两层:(a) 读回链本身;(b) **mock/native 对拍** —— mock 原先每次切版本都现造
// 一帧带画像值的参数面,于是 preview 里这一幕根本重现不出来(mock 盖住真机的第五次)。
{
    const RB = await import(u("web/shared/readback.js"));

    // ---- (a) 读回链(纯函数;口径逐条即 J78「显示的是该维度的权威」)
    const segCh = {
        ch: 1,
        segments: [
            { t0S: 0, t1S: 10, pan: -70, volDb: -3, origin: "auto" },
            { t0S: 10, t1S: 20, pan: 40, volDb: 2, origin: "auto" },
        ],
    };
    const NONE = { pan: false, vol: false };
    const BOTH = { pan: true, vol: true };
    eq(
        RB.readbackSegsOf(segCh, BOTH, true, 0),
        { pan: null, vol: null },
        "(a1) 冻结维度 ⇒ 回落参数面([J85])",
    );
    eq(
        RB.readbackSegsOf(segCh, NONE, true, 0).pan.pan,
        -70,
        "(a2) 未冻结 + 输出 ON + 播放头在曲线之前 ⇒ 首段(= 切进版本还没播放那一档)",
    );
    eq(
        RB.readbackSegsOf(segCh, NONE, true, 15).pan.pan,
        40,
        "(a3) 播放头落在第二段内 ⇒ 第二段",
    );
    eq(
        RB.readbackSegsOf(segCh, NONE, false, 15).pan,
        null,
        "(a4) 输出 OFF(跟随宿主)⇒ 回落参数面(SL-211 复审终轮③a 裁定)",
    );
    eq(
        RB.readbackSegsOf(null, NONE, true, 0),
        { pan: null, vol: null },
        "(a5) 段表为空 ⇒ 参数面",
    );
    const manualSeg = {
        ch: 1,
        segments: [
            { t0S: 0, t1S: 999, pan: 12, volDb: 1, origin: "user_edited" },
        ],
    };
    eq(
        RB.readbackSegsOf(manualSeg, NONE, false, 500).pan.pan,
        12,
        "(a6) 手动常值段优先于输出档(05 §2.2「读回值同样取自该段」)",
    );
    // 逐维分叉:只冻 pan 时 vol 仍读段(一次算两维不能把两维的位混掉)。
    eq(
        [
            RB.readbackSegsOf(segCh, { pan: true, vol: false }, true, 0).pan,
            RB.readbackSegsOf(segCh, { pan: true, vol: false }, true, 0).vol
                .volDb,
        ],
        [null, -3],
        "(a7) 只冻 pan ⇒ pan 走参数面、vol 仍走段",
    );

    // ---- (b) mock/native 对拍:复制版本 → 切过去,参数面必须是**出厂默认**
    const sl241 = await openSession("fixture=fifteen-tracks");
    const panId = (v, ch) => `v${v}_t${String(ch).padStart(2, "0")}_pan`;
    const heads = [1, 2, 3];

    // 前置:V1 的参数面本来是有画像值的(否则下面那条「V2 全 0」空绿)。
    const v1Pans = heads.map((ch) => sl241.store.params.values[panId(1, ch)]);
    check(
        v1Pans.some((x) => Number.isFinite(x) && x !== 0),
        `前置:V1 参数面有非默认 pan(实得 ${JSON.stringify(v1Pans)})`,
    );

    await sl241.bridge.copyVersion(1, 2);
    await sl241.bridge.setVersionActive(2);
    await sleep(120);

    const v2Pans = heads.map((ch) => sl241.store.params.values[panId(2, ch)]);
    check(
        v2Pans.every((x) => x === 0),
        `(b1) 切到刚复制出来的 V2:参数面是出厂默认 pan=0(实得 ${JSON.stringify(v2Pans)})` +
            " —— 契约「copyVersion 零参数写入」+「打印头只驱动激活版」的直接后果;" +
            "mock 原先现造一帧画像值,这一条即红",
    );

    // 而**段表**带着复制过来的真曲线 —— 这就是分布图该读的那一份。
    const segCh1 = ((sl241.store.segments || {}).channels || []).find(
        (c) => c && c.ch === 1,
    );
    check(
        segCh1 &&
            (segCh1.segments || []).length > 0 &&
            segCh1.segments.some(
                (sg) => Number.isFinite(sg.pan) && sg.pan !== 0,
            ),
        // 措辞留意:这里钉的是「V2 段表存在且带非零 pan」,**不是**「copyVersion 深拷了
        // src 的曲线」—— mock 的 copyVersion 走 regenerateSegments 现生成 dst 那一版的
        // 种子曲线,与契约 §1.11 的深拷贝并不一致(基线如此,非本卡引入)。本卡只需要
        // 「段表里有真值可读」这一条(#159 复审【建议】4)。
        "(b2) 切版本同拍到达的段表里有 V2 的曲线(非零 pan)—— 读回真源就在手边",
    );

    // 用读回链把两者串起来:输出 ON 时,分布图该显示的是**段**的 pan,不是参数面的 0。
    // 对**所有**已连接轨取,不押宝某一轨:段值是按 version 播种的伪随机,
    // 单看 ch1 的首段恰好取整到 0 就会误红(#159 复审【建议】5)。
    const shownPans = ((sl241.store.segments || {}).channels || []).map((c) => {
        const seg = RB.readbackSegsOf(c, NONE, /*outputOn=*/ true, 0).pan;
        return seg ? seg.pan : null;
    });
    check(
        shownPans.length > 0 &&
            shownPans.some((p) => Number.isFinite(p) && p !== 0),
        `(b3) 读回链在「刚切进 V2、还没播放」这一刻给出曲线值而非居中(实得 ${JSON.stringify(shownPans.slice(0, 5))}…)`,
    );

    // 切回 V1:它自己那一份参数面必须原样还在(按版本存,不是每次现造)。
    await sl241.bridge.setVersionActive(1);
    await sleep(120);
    eq(
        heads.map((ch) => sl241.store.params.values[panId(1, ch)]),
        v1Pans,
        "(b4) 切回 V1:参数面还是 V1 自己那一份(按版本存的往返)",
    );

    // (b5) 全局三件**不跟版本走**(#159 复审【重要】1):native 侧 width / ms_balance /
    // lead_select 没有 v{n}_ 前缀、版本无关,打印头也不碰。整帧按版本存会造出
    // 「V1 拧到 130 → 切 V2 打回 100 → 切回 V1 又变 130」这种真机没有的往返。
    await sl241.bridge.beginParamGesture("width");
    await sl241.bridge.setParam("width", 130);
    await sl241.bridge.endParamGesture("width");
    await sleep(120);
    eq(
        sl241.store.params.values.width,
        130,
        "(b5) 前置:V1 上全局 width 拧到 130",
    );
    await sl241.bridge.setVersionActive(2);
    await sleep(120);
    eq(
        sl241.store.params.values.width,
        130,
        "(b5) 切到 V2:全局 width 不跟版本走(打回 100 即红)",
    );
    await sl241.bridge.setVersionActive(1);
    await sleep(120);
    eq(
        sl241.store.params.values.width,
        130,
        "(b5) 切回 V1:全局 width 仍是 130",
    );
    sl241.session.stop();
}

// =============================================================================
if (fail > 0) {
    console.error(`\nsmoke-tab1-interactions 失败(${fail} 项)`);
    process.exit(1);
}
log("\nsmoke-tab1-interactions 通过");
process.exit(0);
