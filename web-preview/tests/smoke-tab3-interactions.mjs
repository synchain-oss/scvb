// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Tab3 波形页冒烟(node,无 DOM;T33 Wave 1)
// =============================================================================
// 口径同 smoke-tab1/tab2:仓内零 node_modules,断言面 = 纯函数 + 源码级字面断言
// (mock 端到端与交互断言归 Wave 2 的 ⑥-⑧;DOM 侧由统筹在 8823 端口逐屏走)。
//
// 跑什么(Wave 1 的 ①-⑤):
//   ① 纯函数:视口换算/夹取/以光标为中心缩放(canvas/timeline.js)、命中半径与
//      RE-06 透明扩展(shared/hit.js)、播放头插值(canvas/playhead.js)、帧时账
//      三档降级(canvas/layers.js)、LRU 8 块/轨 + 包络映射(canvas/waveform.js)、
//      滑杆行程/读数、mm:ss.mmm、泳道模型投影(tab-wave.js);
//   ② 布局常量:158 / 34 / 262 / 44 / 22 / 20 / 148、7 滑杆值域与 J23 默认值
//      (行程比与设计稿 2070-2074 逐一相符);
//   ③ 词条:T33 新增 key 三语齐、占位符三语一致、词条值禁词零命中;
//   ④ [J67] + 契约禁项:源码级零命中(泳道页无视图切换器、无拉取式段 API);
//   ⑤ token:tokens.css 的 Tab3 深色泳道组 10 条俱在,且 Tab3 CSS 真的在用;
//      附:mock 假波形(5min×15,J59)契约形状 / 确定性 / stale / valleys。
//
// 用法:node web-preview/tests/smoke-tab3-interactions.mjs [仓库根绝对路径]
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

const TW = await import(u("web/output/tab-wave.js"));
const TL = await import(u("web/output/canvas/timeline.js"));
const HD = await import(u("web/output/canvas/hidpi.js"));
const LY = await import(u("web/output/canvas/layers.js"));
const PH = await import(u("web/output/canvas/playhead.js"));
const WF = await import(u("web/output/canvas/waveform.js"));
const HIT = await import(u("web/shared/hit.js"));
const MD = await import(u("web/shared/mock-data.js"));
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
const near = (a, b, tol, msg) =>
    check(Math.abs(a - b) <= tol, `${msg}: 实得 ${a},期望 ${b}±${tol}`);

// =============================================================================
log("=== ① 纯函数(视口换算 / 夹取 / 命中 / 弹道基建)===");
{
    // ---- timeline:双向换算与夹取(05 §6.1 命中测试用 CSS 坐标的几何底座)
    const vp = TL.makeViewport(300, 60, 120);
    eq(TL.spanOf(vp), 60, "视口跨度");
    eq(TL.timeToX(vp, 600, 90), 300, "timeToX 线性映射");
    eq(TL.xToTime(vp, 600, 300), 90, "xToTime 逆映射");
    eq(TL.xToTime(vp, 600, -50), 60, "xToTime 越界夹到视口");
    eq(
        TL.clampViewport({ startS: -10, endS: 20 }, 300),
        { startS: 0, endS: 30 },
        "夹取:负起点平移回界内(跨度保持)",
    );
    eq(
        TL.clampViewport({ startS: 290, endS: 350 }, 300),
        { startS: 240, endS: 300 },
        "夹取:越尾平移回界内",
    );

    // 以光标为中心缩放(05 §2.3 行 319):锚点缩放前后落在同一舞台 x 上
    const z = TL.zoomAt(vp, 300, 90, 2);
    near(TL.timeToX(z, 600, 90), 300, 1e-6, "zoomAt 锚点位置不动");
    near(TL.spanOf(z), 30, 1e-9, "zoomAt 跨度减半");
    // 缩放上限:跨度不小于 MIN_SPAN_S
    const zMax = TL.zoomAt(vp, 300, 90, 1e9);
    near(TL.spanOf(zMax), TL.MIN_SPAN_S, 1e-9, "缩放到头夹在最小跨度");
    // 平移贴边即停
    eq(TL.panBy(vp, 300, -1000), { startS: 0, endS: 60 }, "panBy 贴左边即停");
    eq(TL.panBy(vp, 300, 1000), { startS: 240, endS: 300 }, "panBy 贴右边即停");
    // A-16 档位读数与滚动 thumb
    eq(
        TL.zoomLabel({ startS: 0, endS: 75 }, 300),
        "×4.0 / 75s",
        "缩放读数格式",
    );
    eq(
        TL.scrollThumb({ startS: 75, endS: 150 }, 300),
        { left: 0.25, width: 0.25 },
        "底部条 thumb 几何",
    );

    // ---- hit.js:两条命中口径不得混用(05 §6.1 ≥12 / §6.3 ±6)
    eq(HIT.HIT_RADIUS_PX, 12, "一般控件半径 12");
    eq(HIT.BOUNDARY_HIT_PX, 6, "边界专用半径 6");
    check(
        HIT.hitsBoundary(100, 106) && !HIT.hitsBoundary(100, 107),
        "边界 ±6 边缘判定",
    );
    eq(HIT.nearestHit(100, [80, 97, 104]), 1, "多命中取最近");
    eq(HIT.nearestHit(100, [80, 120]), -1, "无命中回 -1");
    // RE-06:15px 的 ✕ 在 0.5 档 → 每侧补 16.5 设计 px;已够大回 0
    near(HIT.hitExpansionPx(15, 0.5), 16.5, 1e-9, "透明扩展 = (48-15)/2");
    eq(HIT.hitExpansionPx(48, 0.5), 0, "已够大不再扩");
    eq(
        HIT.eventToLocal(210, 60, { left: 10, top: 20 }, 2),
        { x: 100, y: 20 },
        "eventToLocal 的 zoom 校正",
    );

    // ---- hidpi:k = uiScale × dpr(05 §6.1 逐字)
    eq(HD.backingScale(1.25, 2), 2.5, "backingScale = uiScale×dpr");
    eq(HD.backingScale(undefined, undefined), 1, "缺参回 1");

    // ---- playhead:插值外推 + 封顶;暂停原地
    near(
        PH.interpolate({ timeS: 10, isPlaying: true }, 1000, 1100),
        10.1,
        1e-9,
        "播放中按墙钟外推",
    );
    eq(
        PH.interpolate({ timeS: 10, isPlaying: false }, 1000, 9000),
        10,
        "暂停不外推",
    );
    near(
        PH.interpolate({ timeS: 10, isPlaying: true }, 1000, 99000),
        10.2,
        1e-9,
        "断流外推封顶 200ms",
    );

    // ---- layers:滚动平均 >20ms 连续触发 → 三档降级序列(05 §6.1 逐字)
    eq(LY.FRAME_BUDGET_MS, 20, "帧时阈 20ms");
    eq(LY.STATIC_REDRAW_BUDGET_MS, 8, "静态层预算 8ms");
    eq(
        LY.DEGRADE_SEQUENCE,
        ["halfRatePlayhead", "blitPan", "noCanvasBlur"],
        "降级序列三档",
    );
    const gov = LY.createFrameGovernor();
    for (let i = 0; i < LY.FRAME_WINDOW; i++) gov.push(30);
    eq(gov.level(), 1, "满窗慢帧 → 升一档");
    for (let i = 0; i < LY.FRAME_WINDOW * 2; i++) gov.push(30);
    eq(gov.level(), 3, "持续慢帧逐档到 3 封顶");
    for (let i = 0; i < LY.FRAME_WINDOW * 3; i++) gov.push(5);
    eq(gov.level(), 0, "恢复快帧逐档回落");
    eq(gov.modes(), [], "0 档无降级模式");

    // ---- waveform:LRU 8 块/轨、包络映射、run 分段
    eq(WF.TILE_LRU_CAP, 8, "LRU 8 块/轨(05 §6.3)");
    eq(WF.IDLE_REFETCH_MS, 120, "静止 120ms 取新块");
    eq(WF.COVERAGE_BAR_PX, 2, "覆盖条 2px(B-07)");
    const cache = WF.createTileCache(3);
    cache.set("a", 1);
    cache.set("b", 2);
    cache.set("c", 3);
    cache.get("a"); // 保鲜 a
    cache.set("d", 4); // 应挤掉 b
    check(
        cache.get("b") === undefined && cache.get("a") === 1,
        "LRU 挤最久未用、get 保鲜",
    );
    near(WF.envelopeHalfPx(0, 34), 13, 1e-9, "0 dB 顶到半高 13(34px 泳道)");
    near(WF.envelopeHalfPx(-80, 34), 1, 1e-9, "地板画最细 1px");
    near(WF.envelopeHalfPx(-160, 34), 1, 1e-9, "哨兵 -160 夹到地板");
    eq(
        WF.runsOf([0, 1, 1, 0, 1], (v) => v > 0),
        [
            [1, 3],
            [4, 5],
        ],
        "位数组 → 连续段表",
    );

    // ---- tab-wave 纯函数
    eq(TW.fmtTimeMs(72.4), "01:12.400", "mm:ss.mmm 主显(B-12)");
    eq(TW.fmtTimeMs(0), "00:00.000", "零点格式");
    near(TW.panYPx(100), 19, 1e-9, "pan +100 → y 19(中线 12 ± 7)");
    near(TW.panYPx(-100), 5, 1e-9, "pan -100 → y 5");
    near(TW.volYPx(12), 15, 1e-9, "vol +12dB → 基线 22 − 7");
    near(TW.volYPx(-24), 22, 1e-9, "vol -24dB → 基线 22");
    eq(TW.durationOf({}), 300, "无线索回落 5 分钟");
    eq(
        TW.durationOf({ state: { global: { range: { end_s: 240 } } } }),
        240,
        "Range 终点参与时长推定",
    );
    // 泳道模型投影(§2.7 覆盖率 / §2.8 段数 / §2.9 lowSample)
    const lanes = TW.laneModelFromStore({
        state: { channels: [{ label: "主唱" }] },
        conn: {
            channels: [{ slotState: 2, heartbeatFresh: true }],
        },
        coverage: { 1: 87.5 },
        segments: {
            channels: [
                {
                    ch: 1,
                    stale: true,
                    segments: [
                        { t0S: 0, t1S: 8, origin: "auto" },
                        {
                            t0S: 8,
                            t1S: 20,
                            origin: "user_edited",
                            locked: true,
                        },
                    ],
                },
            ],
        },
        errors: new Map([["lowSample", { code: "lowSample", ch: 1 }]]),
    });
    eq(lanes.length, 15, "恒 15 条泳道(J59)");
    eq(
        [
            lanes[0].status,
            lanes[0].cov,
            lanes[0].segs,
            lanes[0].stale,
            lanes[0].low,
        ],
        ["active", 88, 2, true, 1],
        "轨头六件投影(状态/覆盖率/段数/stale/黄标)",
    );
    eq(lanes[1].status, "idle", "无 conn 的轨 = idle");
    // 边界与角标(05 行 310/313:相邻任一段非 auto → 实线;锁定标独立)
    const segCh = {
        ch: 1,
        segments: [
            { t0S: 0, t1S: 8, origin: "auto" },
            { t0S: 8, t1S: 20, origin: "user_edited", locked: true },
            { t0S: 20, t1S: 30, origin: "auto" },
        ],
    };
    eq(
        TW.boundariesOf(segCh),
        [
            { tS: 8, manual: 1 },
            { tS: 20, manual: 1 },
        ],
        "边界表与权重",
    );
    eq(
        TW.segMarksOf(segCh),
        [
            { kind: "E", tS: 8 },
            { kind: "lock", tS: 8 },
        ],
        "E 角标 + 锁定标;auto 无角标",
    );
    check(
        TW.hasNonAutoSegments({ channels: [segCh] }),
        "重新识别判据:有非 auto 段",
    );
    check(
        !TW.hasNonAutoSegments({ channels: [] }),
        "重新识别判据:空表 disabled",
    );
    check(TW.isLanesEmpty({ coverage: {} }), "全轨无 coverage = 空态");
    check(!TW.isLanesEmpty({ coverage: { 3: 1 } }), "任一轨有 coverage 即非空");
    // 标尺刻度:步长取「≤10 枚」最小档,首刻度对齐步长整数倍
    const ticks = TW.rulerTicks({ startS: 0, endS: 300 });
    check(ticks.length <= 11 && ticks[0].tS === 0, "标尺刻度数与首刻度");
    eq(ticks[1].label, "00:30", "刻度 mm:ss 文本");
}

// =============================================================================
log("=== ② 布局常量(设计稿几何:158 / 34 / 262 / 44 …)===");
{
    eq(TW.LANE_COUNT, 15, "15 泳道(J59)");
    eq(TW.HEAD_W, 158, "轨头 158(标尺/轨头/底部条三处共用)");
    eq(TW.LANE_H, 34, "泳道行高 34([J72a] C-10:不取 TAB_ROWS 的 44)");
    eq(TW.INSPECTOR_W, 262, "检查器 262(灰模 260 → 统一稿内 262)");
    eq(TW.SCALE_COL_W, 44, "右缘双刻度列 44(B-11 共用一列)");
    eq(TW.RULER_H, 22, "标尺行高 22");
    eq(TW.BOTTOM_BAR_H, 20, "底部条 20");
    eq(TW.BOTTOM_HEAD_W, 148, "底部空档 148(+padding 10 = 158)");
    check(TW.BOTTOM_HEAD_W + 10 === TW.HEAD_W, "148+10=158 对齐链");

    // 7 滑杆:顺序 = §1.18 五字段 + §1.19 两字段,不可重排
    eq(
        TW.SLIDERS.map((s) => s.field),
        [
            "threshold_db",
            "hysteresis_db",
            "hangover_ms",
            "padding_pre_ms",
            "padding_post_ms",
            "sensitivity",
            "min_segment_ms",
        ],
        "滑杆顺序与契约字段",
    );
    // 默认值行程比与设计稿 2070-2074 逐一相符(值域反推的自证)
    const P = TW.SLIDERS.map((s) => Math.round(TW.sliderPercent(s, s.def)));
    eq(P, [44, 30, 36, 24, 40, 62, 28], "七杆默认行程比 = 稿内 p 值");
    // J23:padding 默认 120/200;§1.18 五字段整包缓存底账与滑杆默认一致
    eq(TW.DEFAULT_VAD_PARAMS.padding_pre_ms, 120, "J23 前留白 120");
    eq(TW.DEFAULT_VAD_PARAMS.padding_post_ms, 200, "J23 后留白 200");
    eq(Object.keys(TW.DEFAULT_VAD_PARAMS).length, 5, "setVadParams 五字段整包");
    eq(
        Object.keys(TW.DEFAULT_SEGMENTATION).sort(),
        ["min_segment_ms", "mode", "sensitivity"],
        "setSegmentation 三字段整包",
    );
    eq(TW.fmtSliderValue(TW.SLIDERS[0], -38), "-38 dB", "滑杆读数格式(dB)");
    eq(
        TW.fmtSliderValue(TW.SLIDERS[5], 0.62),
        "0.62",
        "滑杆读数格式(无单位两位)",
    );
    eq(TW.sliderPercent(TW.SLIDERS[0], -999), 0, "行程比越界夹取");

    // 泳道模板:锚点面(appendix B 新增件)齐 —— node 侧字符串断言
    const row = TW.waveLaneHtml(3);
    for (const a of [
        "wave-lane-3",
        "wave-lane-3-head",
        "wave-lane-3-checkbox",
        "wave-lane-3-light",
        "wave-lane-3-label",
        "wave-lane-3-lowsample",
        "wave-lane-3-covseg",
        "wave-lane-3-curvevisible",
        "wave-lane-3-stage",
        "wave-lane-3-static",
        "wave-lane-3-badges",
    ]) {
        check(row.includes(`data-gb="${a}"`), `泳道模板含锚点 ${a}`);
    }
}

// =============================================================================
log("=== ③ 词条(T33 新增 key 三语 + 占位符 + 禁词)===");
{
    const KEYS = [
        "wave.sldThreshold",
        "wave.sldHysteresis",
        "wave.sldHangover",
        "wave.sldPadPre",
        "wave.sldPadPost",
        "wave.sldSensitivity",
        "wave.sldMinSeg",
        "wave.emptyMain",
        "wave.emptyCta",
        "wave.followHostNote",
        "wave.btnRecapture",
        "wave.btnReanalyze",
        "wave.btnClearCoverage",
        "wave.applyCountdown",
        "wave.applying",
        "wave.recaptureInlineNote",
        "wave.inspectorTitle",
        "wave.segStart",
        "wave.segEnd",
        "wave.segLen",
        "wave.segLoudness",
        "wave.originField",
        "wave.volField",
        "wave.lockSegment",
        "wave.lockBadge",
        "wave.covSeg",
        "wave.curveVisible",
        "wave.pickTrack",
        "wave.boundaryHandleTip",
        "wave.reidentifyConfirm",
        "wave.clearCoverageConfirm",
        // 既有 Tab3 词条(05 §5 定稿)一并钉住三语齐
        "wave.trackPickHint",
        "wave.selChip",
        "wave.setRangeTip",
        "wave.originLegend",
        "wave.diffKept",
        "wave.recaptureArmed",
        "footer.recaptureOutputWarn",
    ];
    const ph = (s) => (String(s).match(/\{\w+\}/g) || []).sort().join(",");
    for (const k of KEYS) {
        for (const lang of ["zh", "en", "fr"]) {
            check(
                typeof T[lang][k] === "string" && T[lang][k].length > 0,
                `${lang}.${k} 非空`,
            );
        }
        eq(ph(T.en[k]), ph(T.zh[k]), `${k} en 占位符与 zh 一致`);
        eq(ph(T.fr[k]), ph(T.zh[k]), `${k} fr 占位符与 zh 一致`);
    }
    // 7 滑杆短标 = mono 大写微标族,三语同值(A-19 裁定)
    for (const k of KEYS.slice(0, 7)) {
        check(
            T.zh[k] === T.en[k] && T.zh[k] === T.fr[k],
            `${k} 三语同值(mono 微标)`,
        );
        check(T.zh[k] === T.zh[k].toUpperCase(), `${k} 全大写`);
    }
    // C-12:段响度标签不得回流被契约 §1.21 废弃的旧显示名
    const oldLoudnessName = "LUFS-" + "S";
    for (const lang of ["zh", "en", "fr"]) {
        check(
            !String(T[lang]["wave.segLoudness"]).includes(oldLoudnessName),
            `${lang}.wave.segLoudness 不含被废显示名`,
        );
    }
    // 05 §5 禁词:查词条值(注释为解释纪律引用禁词是允许的,口径同 tab2 冒烟)
    const BAD = [
        "写入" + "完成",
        "推子" + "后",
        "post-" + "fader",
        "六" + "条",
    ];
    for (const lang of ["zh", "en", "fr"]) {
        for (const [k, v] of Object.entries(T[lang])) {
            for (const bad of BAD) {
                check(
                    !String(v).toLowerCase().includes(bad.toLowerCase()),
                    `${lang}.${k} 含禁词「${bad}」`,
                );
            }
        }
    }
}

// =============================================================================
log("=== ④ [J67] + 契约禁项(源码级零命中)===");
{
    // 断言针用拼接构造,免得本文件把自己判红
    const LIST_SWITCH = "tab-wave-" + "list";
    const PULL_API = [
        "list" + "Segments",
        "scvb.segment" + "List",
        "setSegment" + "Value",
    ];
    const FILES = [
        "web/output/tab-wave.js",
        "web/output/app.js",
        "web/output/index.html",
        "web/output/canvas/timeline.js",
        "web/output/canvas/hidpi.js",
        "web/output/canvas/layers.js",
        "web/output/canvas/playhead.js",
        "web/output/canvas/waveform.js",
        "web/shared/hit.js",
        "web/shared/i18n.js",
        "web/shared/tokens.css",
        "web-preview/mock/state-driver.js",
        "web/shared/mock-data.js",
    ];
    for (const f of FILES) {
        const s = src(f);
        check(!s.includes(LIST_SWITCH), `[J67] ${f} 无波形⇄列表切换器痕迹`);
        for (const p of PULL_API) {
            check(!s.includes(p), `${f} 无契约禁项 ${p}`);
        }
    }
    // T33 交付面的禁词零命中(brief §0.10:全文,含注释 —— 与 ③ 的词条值口径不同,
    // 这里只扫本卡新建文件,既有文件的历史注释不在本卡整改面)
    const BAD = [
        "写入" + "完成",
        "推子" + "后",
        "post-" + "fader",
        "六" + "条",
    ];
    for (const f of [
        "web/output/tab-wave.js",
        "web/output/canvas/timeline.js",
        "web/output/canvas/hidpi.js",
        "web/output/canvas/layers.js",
        "web/output/canvas/playhead.js",
        "web/output/canvas/waveform.js",
        "web/shared/hit.js",
        "web-preview/tests/smoke-tab3-interactions.mjs",
    ]) {
        const s = src(f).toLowerCase();
        for (const bad of BAD) {
            check(!s.includes(bad.toLowerCase()), `${f} 全文无禁词「${bad}」`);
        }
    }
    // 桥面零新增:tab-wave.js 里出现的 bridge 调用名必须都在契约 §7 manifest 内
    const tw = src("web/output/tab-wave.js");
    const called = [...tw.matchAll(/call\("(\w+)"/g)].map((m) => m[1]);
    eq(called, ["requestWaveform"], "Wave 1 桥面只拉波形块(§1.27)");
    // requestWaveform 纪律:request/response,不当事件消费(不 on() 它)
    check(
        !/on\(["']scvb\.waveform/.test(tw),
        "无波形事件订阅(§1.27 绝不进事件流)",
    );
}

// =============================================================================
log("=== ⑤ token 存在性 + mock 假波形(5min×15,J59)===");
{
    const css = src("web/shared/tokens.css");
    check(css.includes("Tab3 深色泳道组(T33)"), "token 注释组名逐字");
    const TOKENS = [
        "--dark-rule:",
        "--dark-rule-weak:",
        "--dark-chip:",
        "--txt-dark-0:",
        "--wave-env:",
        "--sem-amber-hi:",
        "--sem-amber-ink-dark:",
        "--ink-on-amber:",
        "--acc-ink-on-glass:",
        "--acc-link-on-glass-rgb:",
    ];
    for (const t of TOKENS) check(css.includes(t), `tokens.css 含 ${t}`);
    const html = src("web/output/index.html");
    for (const t of [
        "--dark-rule",
        "--dark-chip",
        "--txt-dark-0",
        "--sem-amber-hi",
        "--ink-on-amber",
        "--acc-ink-on-glass",
        "--acc-link-on-glass-rgb",
    ]) {
        check(html.includes(`var(${t}`), `Tab3 CSS 在用 ${t}`);
    }
    // 检查器 = flex 兄弟(C-11):不得再有浮层定位
    const tab3 = html.slice(
        html.indexOf(".segment-inspector"),
        html.indexOf(".inspector-note"),
    );
    check(!/position:\s*absolute/.test(tab3), "检查器无 absolute 浮层定位");

    // ---- mock:5 分钟 ×15 轨,契约 §1.27 形状 / 确定性 / stale / valleys
    eq(MD.DEMO_DURATION_S, 300, "mock 时间线 5 分钟(J59 / 05 §6.3 验收)");
    const tile = MD.makeWaveformTile(1, 0, 300, 512);
    for (const k of ["minDb", "maxDb", "vad", "covered", "stale", "passId"]) {
        eq(tile[k].length, 512, `tile.${k} 长度 = cols`);
    }
    check(Array.isArray(tile.valleys), "valleys 数组在");
    check(
        tile.valleys.every((v, i, a) => i === 0 || a[i - 1] < v),
        "valleys 升序(契约 §1.27:边界吸附用)",
    );
    // 未覆盖列哨兵:covered=0 ⇒ minDb=maxDb=-160,vad/stale/passId 全 0
    const un = tile.covered.map((c, i) => i).filter((i) => !tile.covered[i]);
    check(un.length > 0, "存在未覆盖列(coverage 缺口场景)");
    check(
        un.every(
            (i) =>
                tile.minDb[i] === MD.WAVEFORM_UNCOVERED_DB &&
                tile.maxDb[i] === MD.WAVEFORM_UNCOVERED_DB &&
                !tile.vad[i] &&
                !tile.stale[i] &&
                !tile.passId[i],
        ),
        "未覆盖列哨兵纪律(§1.27)",
    );
    // 确定性:同参重取逐字节一致(种子固定)
    eq(
        MD.makeWaveformTile(4, 10, 90, 256),
        MD.makeWaveformTile(4, 10, 90, 256),
        "同参重取逐字节一致(确定性伪随机)",
    );
    // stale 演示轨:三轨各一段;其余轨全 0
    check(
        MD.makeWaveformTile(2, 0, 300, 512).stale.some((v) => v === 1),
        "stale 演示轨含 stale 列",
    );
    check(
        MD.makeWaveformTile(1, 0, 300, 512).stale.every((v) => v === 0),
        "健康轨 stale 全 0",
    );
    eq(MD.STALE_DEMO_CHANNELS, [2, 7, 12], "stale 演示轨清单");
    check(
        MD.staleRangesOf(2).length === 1 && MD.staleRangesOf(1).length === 0,
        "staleRangesOf 只在演示轨给区间",
    );
    // passId 两轮采集 → 底色微差有据可依
    check(
        tile.passId.includes(1) && tile.passId.includes(2),
        "两轮采集 passId 1/2 俱在",
    );

    // recapture-armed 场景走通(state-driver 的 SCENARIO_MAP + 快照覆写)
    const SD = await import(u("web-preview/mock/state-driver.js"));
    check(
        SD.SCENARIO_MAP["recapture-armed"] === "fifteen-tracks",
        "scenario=recapture-armed 已映射",
    );
    const world = SD.buildWorld({
        role: "output",
        fixture: "fifteen-tracks",
        scenario: "recapture-armed",
    });
    const rec = world.output.snapshot.recapture;
    check(rec && rec.armed === true, "布防快照 armed=true");
    check(rec.tracksMask > 0 && rec.startS < rec.endS, "布防面(mask+选区)成立");
    eq(rec.autoStop, false, "autoStop 契约默认 false");
}

// =============================================================================
if (fail) {
    console.error(`\n${fail} 处断言失败`);
    process.exit(1);
}
log("\n全部通过");
