// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Tab1 分布图双视图 + 轨道配色冒烟(node,无 DOM;T43 / [J75])
// =============================================================================
// 与既有冒烟同款口径:仓内零 node_modules,故断言面是**纯函数 + mock 端到端 + 源码不变式**,
// 画布光栅与指针手势归浏览器手测(shot.mjs 截图 + 真机 preview)。
//
// 跑什么:
//   ① 轨迹图纯几何(web/shared/trajectory-chart.js):断线判据、台阶折线、降采样、
//      pan→y、跟随模式的带内不动/越带重定位、时间刻度 nice-number;
//   ② 15 色轨道调色板(web/shared/track-colors.js):镜像表与 tokens.css **逐条对拍**
//      (漂了当场红)、轨号夹取、node 侧回退;
//   ③ Tab1 投影纯函数(web/output/tab-master.js):视图态默认与回退、段表→折线、
//      图例行随视图切换、时长口径与 Tab3 的 durationOf **同值**(重复实现的定桩);
//   ④ 断线 fixture:`makeSegments({trajectoryGap:true})` 在定点窗口齐断,
//      且**基线段表逐字节不变**(不传 opts 与本参数引入前同值);
//   ⑤ mock 端到端:`setMasterChartMode` 往返(state 回推)+ badArg 拒绝态 +
//      `chart-trajectory` 场景开箱即轨迹档;
//   ⑥ 词条:`chart.*` 三语齐备、非空、占位符一致、05 §5 禁词零命中;
//   ⑦ 性能与空闲纪律:15 轨全画布的几何路径耗时预算、降采样确实在削点、
//      两条自持循环在离场时都停(smoke-tab3 ⑨ 的同款不变式)。
//
// 用法:node web-preview/tests/smoke-t43-chart.mjs [仓库根绝对路径]
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

const TC = await import(u("web/shared/trajectory-chart.js"));
const TCOL = await import(u("web/shared/track-colors.js"));
const TM = await import(u("web/output/tab-master.js"));
const TW = await import(u("web/output/tab-wave.js"));
const { T } = await import(u("web/shared/i18n.js"));
const MD = await import(u("web/shared/mock-data.js"));
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

// =============================================================================
log("=== ① 轨迹图纯几何(J75 A)===");

{
    // 断线判据:段与段首尾相接 ⇒ 同一条折线里走台阶;有间隙 ⇒ 另起一条。
    const contiguous = [
        { t0S: 0, t1S: 4, pan: -30 },
        { t0S: 4, t1S: 9, pan: 20 },
    ];
    const runs1 = TC.runsOfSegments(contiguous);
    eq(runs1.length, 1, "首尾相接的两段 = 一条折线");
    eq(
        runs1[0],
        [
            { tS: 0, pan: -30 },
            { tS: 4, pan: -30 },
            { tS: 4, pan: 20 },
            { tS: 9, pan: 20 },
        ],
        "段内水平、交界竖直台阶(不画斜线 —— 80ms ramp 在任何档下都不足 1px)",
    );

    const gapped = [
        { t0S: 0, t1S: 4, pan: -30 },
        { t0S: 7, t1S: 9, pan: 20 },
    ];
    eq(
        TC.runsOfSegments(gapped).length,
        2,
        "有间隙 ⇒ 断线(J75 A「无分段覆盖的区间不画线」)",
    );

    // 容差:10ms 以内算连着(段表 t0S/t1S 都是 2 位小数)
    eq(
        TC.runsOfSegments([
            { t0S: 0, t1S: 4, pan: 0 },
            { t0S: 4.005, t1S: 8, pan: 10 },
        ]).length,
        1,
        "5ms 缝隙在 CONTIGUOUS_EPS_S 容差内,不算断线",
    );
    eq(
        TC.runsOfSegments([
            { t0S: 0, t1S: 4, pan: 0 },
            { t0S: 4.02, t1S: 8, pan: 10 },
        ]).length,
        2,
        "20ms 缝隙超容差 ⇒ 断线",
    );

    // 乱序/脏数据不炸
    eq(TC.runsOfSegments(null).length, 0, "null 段表 → 空折线组");
    eq(
        TC.runsOfSegments([
            { t0S: 5, t1S: 9, pan: 0 },
            { t0S: 0, t1S: 5, pan: 0 },
        ]).length,
        1,
        "乱序输入先按 t0S 排序再判断线",
    );
    eq(
        TC.runsOfSegments([
            { t0S: 3, t1S: 3, pan: 0 }, // 零长
            { t0S: 5, t1S: 1, pan: 0 }, // 倒挂
            { t0S: NaN, t1S: 4, pan: 0 },
        ]).length,
        0,
        "零长/倒挂/NaN 段一律滤掉",
    );

    // pan → y:+100(右)在顶、−100(左)在底;夹取到值域
    eq(TC.panToY(100, 200), 0, "pan +100 在顶");
    eq(TC.panToY(-100, 200), 200, "pan −100 在底");
    eq(TC.panToY(0, 200), 100, "pan 0 居中");
    eq(TC.panToY(999, 200), 0, "越界 pan 夹到 +100");
    eq(
        TC.PAN_TICKS.slice(),
        [100, 50, 0, -50, -100],
        "y 轴五刻度(同曲线编辑器)",
    );
}

{
    // 跟随模式:带内不动,越带把播放头重新摆到 FOLLOW_LEAD 处
    const vp = { startS: 100, endS: 200 }; // span 100
    eq(
        TC.followViewport(vp, 300, 150),
        vp,
        "播放头在带内(p=0.5)⇒ 视口一动不动(不逐帧漂移)",
    );
    const ahead = TC.followViewport(vp, 300, 195); // p=0.95 > 0.85
    check(ahead.startS > vp.startS, "越过右带 ⇒ 视口向后翻");
    eq(
        Math.round(((195 - ahead.startS) / (ahead.endS - ahead.startS)) * 100),
        Math.round(TC.FOLLOW_LEAD * 100),
        "重定位后播放头落在 FOLLOW_LEAD 处",
    );
    const back = TC.followViewport(vp, 300, 102); // p=0.02 < 0.05
    check(back.startS < vp.startS, "越过左带(倒带)⇒ 视口向前翻");
    // 到头夹取:全长 300、视口已在末尾,再跟随也不越界
    const tail = TC.followViewport({ startS: 200, endS: 300 }, 300, 299);
    eq(tail, { startS: 200, endS: 300 }, "贴到时间线末尾即停,不越界");
    check(
        TC.sameViewport(tail, { startS: 200, endS: 300 }),
        "sameViewport 认得逐字相同的视口(跟随推进的去重判据)",
    );
}

{
    // 降采样:同一像素列且 pan 相同的中间点塌掉;pan 变了的台阶**不许**塌
    const toX = (tS) => tS * 0.01; // 100 秒 → 1px,极端缩小档
    const flat = [];
    for (let i = 0; i < 200; i++) flat.push({ tS: i, pan: 0 });
    const dec = TC.decimateRun(flat, toX);
    check(
        dec.length < flat.length / 10,
        `同列同值的点被塌掉(${flat.length} → ${dec.length})`,
    );
    eq(dec[0], flat[0], "首点保留");
    eq(dec[dec.length - 1], flat[flat.length - 1], "末点保留");

    const stair = [
        { tS: 0, pan: 0 },
        { tS: 1, pan: 0 },
        { tS: 1, pan: 60 }, // 同一列,但 pan 变了 —— 台阶,不许塌
        { tS: 2, pan: 60 },
    ];
    eq(
        TC.decimateRun(stair, toX).length,
        4,
        "同列但 pan 不同的台阶点全部保留(塌掉会把台阶抹成假数据)",
    );
    eq(TC.decimateRun([{ tS: 0, pan: 0 }], toX).length, 1, "单点原样返回");
}

{
    // 时间刻度:1/2/5×10ⁿ 中屏距 ≥64px 的最小档
    check(TC.tickStepS(300, 600) >= 30, "5 分钟 / 600px ⇒ 步长 ≥30s");
    const steps = [300, 60, 10, 2].map((span) => TC.tickStepS(span, 600));
    check(
        steps.every((s, i) => i === 0 || s <= steps[i - 1]),
        "视口越小步长越细(单调)",
    );
    for (const s of steps) {
        const m = s / Math.pow(10, Math.floor(Math.log10(s)));
        check(
            Math.abs(m - 1) < 1e-9 ||
                Math.abs(m - 2) < 1e-9 ||
                Math.abs(m - 5) < 1e-9,
            `步长 ${s} 落在 1/2/5×10ⁿ 档上`,
        );
    }
    const ticks = TC.ticksIn({ startS: 0, endS: 120 }, 600);
    check(ticks.length > 0, "视口内有刻度");
    check(ticks[0] >= 0 && ticks[ticks.length - 1] <= 120, "刻度不越出视口");
    eq(TC.mmss(0), "0:00", "mm:ss 起点");
    eq(TC.mmss(125), "2:05", "mm:ss 补零");
}

// =============================================================================
log("=== ② 15 色轨道调色板(J75 B)===");

{
    eq(TCOL.TRACK_COLOR_COUNT, 15, "15 色(J59 十五轨)");
    eq(TCOL.FALLBACK_TRACK_COLORS.length, 15, "镜像表 15 条");

    // 镜像表 ↔ tokens.css 逐条对拍:两处必须同改,漂了当场红
    const tokens = src("web/shared/tokens.css");
    for (let ch = 1; ch <= 15; ch++) {
        const m = new RegExp(`--track-color-${ch}:\\s*([^;]+);`).exec(tokens);
        check(!!m, `tokens.css 有 --track-color-${ch}`);
        if (!m) continue;
        const declared = m[1].trim();
        eq(
            declared,
            TCOL.FALLBACK_TRACK_COLORS[ch - 1],
            `轨 ${ch} 的镜像值与 tokens.css 一致`,
        );
        check(
            /^\d{1,3},\s*\d{1,3},\s*\d{1,3}$/.test(declared),
            `轨 ${ch} 走 rgb 分量形式(消费侧才好调 alpha)`,
        );
    }
    // 15 色两两不同 —— 撞色等于两条轨永远分不开
    check(
        new Set(TCOL.FALLBACK_TRACK_COLORS).size === 15,
        "15 色两两不同(无重复色)",
    );

    eq(TCOL.trackColorVar(7), "--track-color-7", "轨号 → 变量名");
    eq(TCOL.trackIndex(0), 1, "轨号 0 夹到 1");
    eq(TCOL.trackIndex(16), 1, "轨号 16 按 15 取模回 1");
    eq(TCOL.trackIndex(NaN), 1, "非法轨号回 1(永不返回空串)");
    eq(
        TCOL.trackColorCss(3, 0.5),
        "rgba(var(--track-color-3), 0.5)",
        "DOM 侧色串带 alpha",
    );
    eq(TCOL.rgbaOf("1, 2, 3", 0.4), "rgba(1, 2, 3, 0.4)", "canvas 侧字面色串");
    eq(
        TCOL.resolveTrackPalette(null),
        TCOL.FALLBACK_TRACK_COLORS.slice(),
        "node 侧(无 CSSOM)回退镜像表",
    );

    // Tab2 单色玻璃管电平表**不受影响**(回流⑦ token 特例):第 17 组一字未动
    check(
        /--t2-meter-liquid:\s*linear-gradient\(\s*180deg,\s*rgba\(196, 188, 226, 0\.92\)/.test(
            tokens,
        ),
        "Tab2 单色液柱渐变原样(回流⑦ 特例不得被轨色外溢)",
    );
    check(
        !/--t2-[a-z-]*:\s*[^;]*--track-color/.test(tokens),
        "第 17 组没有任何一条引用轨色",
    );
    check(
        !/track-color/.test(src("web/output/tab-tracks.js")),
        "tab-tracks.js(Tab2)不消费轨色",
    );
}

// =============================================================================
log("=== ③ Tab1 投影纯函数 ===");

{
    eq(TM.CHART_MODES.slice(), ["distribution", "trajectory"], "视图两态");
    eq(TM.CHART_MODE_DEFAULT, "distribution", "默认档 = 分布(J75 A 逐字)");
    eq(TM.chartModeOf({}), "distribution", "缺 ui 子树 → 默认档");
    eq(TM.chartModeOf({ ui: {} }), "distribution", "缺字段(旧工程)→ 默认档");
    eq(
        TM.chartModeOf({ ui: { master_chart_mode: "nope" } }),
        "distribution",
        "未知值 → 默认档(不炸、不留空)",
    );
    eq(
        TM.chartModeOf({ ui: { master_chart_mode: "trajectory" } }),
        "trajectory",
        "合法值原样",
    );
    // UI 侧常量与 mock-data 侧那份**同值**(两处各留一份的理由见各自头注)
    eq(TM.CHART_MODES.slice(), MD.CHART_MODES.slice(), "两处枚举同值");
}

{
    const segs = MD.FIFTEEN_TRACKS.segments;
    const chans = MD.FIFTEEN_TRACKS.snapshot.channels;
    const series = TM.trajectorySeries(segs, chans);
    eq(series.length, 15, "15 轨全部有线(fifteen-tracks 满配)");
    check(
        series.every((s, i) => i === 0 || s.ch > series[i - 1].ch),
        "按轨号升序(色板按轨号固定映射,顺序不许乱)",
    );
    const stereo = series.filter((s) => s.stereo).map((s) => s.ch);
    eq(
        stereo,
        MD.DEMO_STEREO_CHANNELS.slice(),
        "立体声轨标记与 fixture 一致(画 pan 中心线,J75 A)",
    );
    check(
        series.every((s) => s.runs.length > 1),
        "每轨都断成多条(乐句之间本来就没段)",
    );
    // pan 全部落在角度域内
    check(
        series.every((s) =>
            s.runs.every((r) =>
                r.every((p) => p.pan >= TC.PAN_MIN && p.pan <= TC.PAN_MAX),
            ),
        ),
        "全部 pan 落在 −100..100 角度域",
    );
    eq(TM.trajectorySeries(null, chans).length, 0, "空段表 → 空数据面");

    // 时长口径与 Tab3 的 durationOf **同值**(两处各写一份,靠本断言定桩)
    const store = {
        state: MD.FIFTEEN_TRACKS.snapshot,
        segments: segs,
        playhead: { timeS: 42, isPlaying: true },
    };
    eq(
        TM.chartDurationS(store),
        TW.durationOf(store),
        "chartDurationS ≡ tab-wave.durationOf(重复实现的定桩断言)",
    );
    eq(
        TM.chartDurationS({}),
        TM.CHART_FALLBACK_DURATION_S,
        "无任何长度证据 → 兜底值(不塌到 0)",
    );
    check(
        TM.chartDurationS({ playhead: { timeS: 999 } }) === 999,
        "播放头越过兜底值仍能把估计抬上去",
    );
}

{
    const chans = MD.FIFTEEN_TRACKS.snapshot.channels;
    const series = TM.trajectorySeries(MD.FIFTEEN_TRACKS.segments, chans);
    const connected = [1, 2, 3];
    const distRows = TM.legendRows("distribution", connected, series, chans);
    eq(
        distRows.map((r) => r.ch),
        connected,
        "分布档图例 = 已连接轨(空闲轨不画柱,也就不该进图例)",
    );
    const trajRows = TM.legendRows("trajectory", connected, series, chans);
    eq(trajRows.length, 15, "轨迹档图例 = 有分段的轨(与画出来的线一一对应)");
    check(
        trajRows[2].stereo === true && trajRows[2].ch === 3,
        "图例带立体声标记(ST 角标的数据面)",
    );
    check(
        trajRows.every((r) => typeof r.label === "string"),
        "label 恒为字符串(缺配置时空串,不是 undefined)",
    );
    eq(
        TM.legendRows("distribution", [0, 99, NaN], series, chans).length,
        0,
        "越界轨号滤掉",
    );
    eq(TM.connectedChannels(null), [], "conn 缺失 → 无已连接轨");
    eq(
        TM.connectedChannels(MD.FIFTEEN_TRACKS.snapshot.conn).length,
        TM.connectedCount(MD.FIFTEEN_TRACKS.snapshot.conn),
        "connectedChannels 与 connectedCount 同判据",
    );
}

// =============================================================================
log("=== ④ 断线 fixture(TRAJECTORY_GAP)===");

{
    const base = MD.makeSegments(1, "snapshot");
    const plain = MD.makeSegments(1, "snapshot", undefined, {});
    eq(
        base,
        plain,
        "不传 opts 与传空 opts 同值 —— 基线段表逐字节不变(既有冒烟数字全部照旧)",
    );

    const gapped = MD.makeSegments(1, "snapshot", undefined, {
        trajectoryGap: true,
    });
    const G = MD.TRAJECTORY_GAP;
    for (const entry of gapped.channels) {
        const hits = (entry.segments || []).filter(
            (s) => s.t1S > G.startS && s.t0S < G.endS,
        );
        if (MD.TRAJECTORY_GAP_CHANNELS.includes(entry.ch)) {
            eq(hits.length, 0, `轨 ${entry.ch} 在缺口窗口内确实没有段`);
        } else {
            check(
                hits.length > 0,
                `轨 ${entry.ch} **不在**名单上,窗口内的段原样保留`,
            );
        }
    }
    // 缺口在轨迹图上确实表现为一条断线(而不是被别的间隙吞掉)
    for (const ch of MD.TRAJECTORY_GAP_CHANNELS) {
        const entry = gapped.channels.find((c) => c.ch === ch);
        const runs = TC.runsOfSegments(entry.segments);
        const spans = runs.map((r) => ({
            t0: r[0].tS,
            t1: r[r.length - 1].tS,
        }));
        const across = spans.some((s) => s.t0 < G.startS && s.t1 > G.endS);
        check(!across, `轨 ${ch} 没有任何一条折线横跨缺口`);
        const before = spans.filter((s) => s.t1 <= G.startS).length;
        const after = spans.filter((s) => s.t0 >= G.endS).length;
        check(before > 0 && after > 0, `轨 ${ch} 缺口两侧都还有线(断口成立)`);
    }
    check(
        G.endS - G.startS >= 24,
        "缺口够宽(≥24s),截图里一眼可见而不是一道发丝缝",
    );
}

// =============================================================================
log("=== ⑤ mock 端到端:视图态往返 ===");

{
    const s = driver.createPreviewSession({
        role: "output",
        params: "?fixture=fifteen-tracks",
    });
    const { createBridge, PENDING_FUNCS } = await import(
        u("web/shared/bridge.js")
    );
    eq(
        PENDING_FUNCS.output.slice(),
        ["setMasterChartMode"],
        "待转正名表只有本卡这一项(转正后从表里删掉)",
    );
    const bridge = createBridge({ role: "output", mockBackend: s.mock });
    check(
        typeof bridge.setMasterChartMode === "function",
        "mock 实现了待转正名字 ⇒ 桥上挂得到(预览页当场可往返)",
    );

    let lastState = null;
    bridge.on("scvb.state", (st) => {
        lastState = st;
    });
    const snap = await bridge.requestInitialState();
    eq(
        snap.ui.master_chart_mode,
        "distribution",
        "快照默认档 = distribution(J75 A)",
    );

    const ok = await bridge.setMasterChartMode("trajectory");
    eq(ok, { ok: true }, "合法值受理(形制照 §1.31 setActiveTab)");
    check(
        lastState &&
            lastState.ui &&
            lastState.ui.master_chart_mode === "trajectory",
        "写入后经 scvb.state 回推 —— 「切换态持久化往返」成立",
    );
    // 回推的值喂回投影函数,应当读出同一档(往返闭环)
    eq(
        TM.chartModeOf(lastState),
        "trajectory",
        "回推值经 chartModeOf 读回同一档",
    );

    const bad = await bridge.setMasterChartMode("nope");
    eq(bad, { ok: false, reason: "badArg" }, "枚举外的值回 badArg");
    const back = await bridge.setMasterChartMode("distribution");
    eq(back, { ok: true }, "切回默认档");
    eq(
        lastState.ui.master_chart_mode,
        "distribution",
        "切回后 state 也跟着回推",
    );
}

{
    // 场景开箱即轨迹档 + 段表带缺口(供 shot.mjs 截图与用户 preview)
    check(
        driver.SCENARIO_MAP["chart-trajectory"] === "fifteen-tracks",
        "chart-trajectory 场景已登记",
    );
    const s = driver.createPreviewSession({
        role: "output",
        params: "?scenario=chart-trajectory",
    });
    const { createBridge } = await import(u("web/shared/bridge.js"));
    const bridge = createBridge({ role: "output", mockBackend: s.mock });
    const snap = await bridge.requestInitialState();
    eq(
        snap.ui.master_chart_mode,
        "trajectory",
        "场景快照就在轨迹档(重开面板的回读半边)",
    );
    // 首帧段表在 world 里备着(周期事件由 start() 推;node 侧不起定时器,直接读世界)
    const segs = s.world.output.segments;
    check(!!segs, "场景备了首帧段表");
    if (segs) {
        const ch = MD.TRAJECTORY_GAP_CHANNELS[0];
        const entry = segs.channels.find((c) => c.ch === ch);
        const inGap = (entry.segments || []).filter(
            (x) =>
                x.t1S > MD.TRAJECTORY_GAP.startS &&
                x.t0S < MD.TRAJECTORY_GAP.endS,
        );
        eq(inGap.length, 0, `场景段表里轨 ${ch} 的缺口确实挖了`);
    }
}

// =============================================================================
log("=== ⑥ 词条(chart.* 三语)===");

{
    const KEYS = [
        "chart.modeGroup",
        "chart.modeDistribution",
        "chart.modeTrajectory",
        "chart.trajHint",
        "chart.trajAxisY",
        "chart.trajCanvasAria",
        "chart.zoomAria",
        "chart.backToPlayhead",
        "chart.trajEmpty",
        "chart.legendAria",
        "chart.legendHint",
    ];
    const ph = (v) => (String(v).match(/\{[a-z]+\}/gi) || []).sort().join(",");
    for (const k of KEYS) {
        for (const lang of ["zh", "en", "fr"]) {
            check(
                typeof T[lang][k] === "string" && T[lang][k].length > 0,
                `${lang}.${k} 存在且非空`,
            );
        }
        check(
            ph(T.zh[k]) === ph(T.en[k]) && ph(T.zh[k]) === ph(T.fr[k]),
            `${k} 三语占位符一致`,
        );
        // 05 §5 占位符判据:fr 不许照抄英文
        check(T.fr[k] !== T.en[k], `fr.${k} 不是英文照抄`);
    }
    // y 轴刻度词条要能被 data-t-split 拆成 5 格(与曲线编辑器 X 轴同款)
    for (const lang of ["zh", "en", "fr"]) {
        eq(
            T[lang]["chart.trajAxisY"].split(" · ").length,
            5,
            `${lang} 的 y 轴刻度拆成 5 格`,
        );
    }
    const BANNED = ["写入完成", "推子后", "post-fader", "六条"];
    for (const f of [
        "web/shared/trajectory-chart.js",
        "web/shared/track-colors.js",
    ]) {
        const text = src(f);
        for (const banned of BANNED) {
            check(!text.includes(banned), `${f} 命中禁词「${banned}」`);
        }
    }
}

// =============================================================================
log("=== ⑦ 性能预算与空闲纪律(05 §6.1)===");

{
    // 几何路径(段表 → 折线 → 降采样)是每次静态层重绘都要跑的那段。
    // 15 轨全画布、最小缩放档(全长塞进 900px)下测一轮:光栅之外的部分必须
    // 远低于 8ms 的静态层预算(05 §6.3),否则单帧一定超 32ms。
    const chans = MD.FIFTEEN_TRACKS.snapshot.channels;
    const t0 = performance.now();
    let points = 0;
    let kept = 0;
    for (let rep = 0; rep < 10; rep++) {
        const series = TM.trajectorySeries(MD.FIFTEEN_TRACKS.segments, chans);
        const toX = (tS) => (tS / MD.DEMO_DURATION_S) * 900;
        for (const s of series) {
            for (const run of s.runs) {
                points += run.length;
                kept += TC.decimateRun(run, toX).length;
            }
        }
    }
    const per = (performance.now() - t0) / 10;
    check(
        per < 8,
        `15 轨几何路径单轮 ${per.toFixed(2)}ms < 8ms 静态层预算(05 §6.3)`,
    );
    check(points > 1000, `样本量够大(${points / 10} 点/轮),不是空跑`);
    check(kept <= points, "降采样只会削点,不会凭空造点");

    // 空闲零 rAF:离场时**两条**自持循环都要停(smoke-tab3 ⑨ 同款不变式)
    const tj = src("web/shared/trajectory-chart.js");
    check(
        /onPlayhead\(ev\) \{[\s\S]{0,400}if \(!isVisible\(\)\) \{[\s\S]{0,120}playhead\.stop\(\);\s*return;/.test(
            tj,
        ),
        "不在前台不驱动播放头插值(onPlayhead 早退并停帧)",
    );
    check(
        /suspend\(\) \{\s*layers\.stop\(\);\s*playhead\.stop\(\);/.test(tj),
        "suspend 把分层循环与插值循环一起停",
    );
    check(
        /function paintStatic\(\) \{\s*if \(!canvas \|\| !isVisible\(\)\) return;/.test(
            tj,
        ),
        "不在前台不烧 canvas(paintStatic 早退)",
    );
    check(!/setInterval\(/.test(tj), "轨迹图无常驻 setInterval");
    check(
        /drawDynamic: \(\) => false/.test(tj),
        "动态层无逐帧诉求(播放头走 DOM 竖线,分层循环可自停)",
    );
    // 按需起帧的配对:停帧口有了,起帧口也必须有,且由 Tab1 的 render 调
    check(
        /resume,/.test(tj) &&
            /local\.traj\.resume\(\);/.test(src("web/output/tab-master.js")),
        "回到前台由 render 补一次起帧(停帧/起帧配对不变式)",
    );
    // 整页 render 每帧都跑,静态层必须走脏位而不是无脑重画
    check(
        /if \(local\.trajDirty\) \{\s*local\.trajDirty = false;\s*local\.traj\.invalidate\(\);/.test(
            src("web/output/tab-master.js"),
        ),
        "静态层重绘走脏位(播放中不逐帧重画 15 条折线)",
    );
}

// =============================================================================
log(fail === 0 ? "\n=== 结果:全部通过 ===" : `\n=== 结果:${fail} 项失败 ===`);
process.exit(fail === 0 ? 0 : 1);
