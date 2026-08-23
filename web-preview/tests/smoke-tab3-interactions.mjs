// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Tab3 波形页冒烟(node,无 DOM;T33 Wave 1+2)
// =============================================================================
// 口径同 smoke-tab1/tab2:仓内零 node_modules,断言面 = 纯函数 + mock 端到端 +
// 源码级字面断言(DOM 侧由统筹在 8823 端口逐屏走)。
//
// 跑什么(①-⑤ = Wave 1;⑥-⑧ = Wave 2):
//   ⑥ mock 端到端:两段式(松手 300ms 防抖在 mock 侧、reason vad/segmentation
//      + diff、segIdx 重编号)、editSegment 五 op(后置状态/notAdjacent)、
//      recaptureArm(§5.5 reason / state 回读 / 撤防)、clearCoverage 真扣除
//      (covered 哨兵 + §1.5 coverage 口径)、setRange manual(badArg);
//   ⑦ 交互纯函数(点选/位图/重绑/吸附/scope 计数)+ 源码级纪律(≤50Hz 节流、
//      整包下发、UI 不自建防抖、释放才发、onSegments 重绑、中央写闸覆盖);
//   ⑧ Wave 2 新词条三语 + 占位符一致;
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
    eq(HD.backingScale(0, 0), 0.1, "下夹 0.1(0 会让位图尺寸为 0)");
    eq(HD.MAX_BACKING_SCALE, 4, "后备存储倍率上限 4");
    eq(
        HD.backingScale(2, 6),
        4,
        "上夹 4:超高 dpr 不把位图面积按 k² 涨到分配失败",
    );

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
    // **中段**必须钉住:两个端点在任意 γ 下恒等(0^γ=0、1^γ=1),只测端点时把
    // ENV_GAMMA 改回 1(= 退回本批要修的「等高栅栏」)五套 smoke 照样全绿。
    eq(WF.ENV_GAMMA, 2.2, "包络对比度整形指数 γ=2.2");
    near(
        WF.envelopeHalfPx(-40, 34),
        1 + Math.pow(0.5, 2.2) * 12,
        1e-9,
        "中段按 γ 幂整形(-40 dB ≈ 3.61px,线性档会是 7px)",
    );
    check(
        WF.envelopeHalfPx(-40, 34) < 4.5,
        "有声/静默的半高比拉得开(γ=1 的等高栅栏会 ≥7px)",
    );
    // VAD 罩 alpha 只有一处真源(DEFAULT_PALETTE 与 tab-wave 的 computedPalette
    // 各写一份字面量正是本波踩过的坑)。Wave 4 用户 preview:.13 的灰绿「太灰太淡」
    // → .20 + 更鲜亮的绿(tokens §20a);**Wave 5 第二轮**:满高罩「像绿底放了个
    // 波」→ 收成顶部标注带 + alpha 提到 .55(带窄了就可以画实)。
    eq(WF.VAD_ALPHA, 0.55, "VAD 标注带 alpha 取 Wave 5 用户裁定的 .55");
    eq(
        WF.VAD_BAND_PX,
        5,
        "VAD 标注带高 5px(裁定「顶部一条 5–6px,含 1.5px 亮线」)",
    );
    check(
        WF.VAD_BAND_PX <= 34 / 2 - WF.envelopeHalfPx(0, 34) + 1,
        "带高不越过包络顶(半高上限 h/2−4 ⇒ 柱尖恒在 y=4,最多咬 1px 且被柱盖回)",
    );
    check(
        WF.DEFAULT_PALETTE.vad === `rgba(88, 208, 148, ${WF.VAD_ALPHA})`,
        "DEFAULT_PALETTE.vad 由 VAD_ALPHA 拼出(不写死字面 alpha)",
    );
    // 波形本体「粉 + 白」(Wave 4 反馈⑥):外柱淡粉紫 / 内柱近白,与 tokens 同值
    eq(
        WF.DEFAULT_PALETTE.env,
        "rgba(216, 186, 216, 0.52)",
        "外柱 = --wave-env-pink 淡粉紫",
    );
    eq(
        WF.DEFAULT_PALETTE.envCore,
        "rgba(255, 250, 255, 0.6)",
        "内柱亮芯 = --wave-env-core 近白",
    );
    eq(
        WF.DEFAULT_PALETTE.vadEdge,
        "rgba(120, 236, 172, 0.78)",
        "VAD 顶缘线 = --wave-vad-edge",
    );
    // 只改波形本体与 VAD:覆盖条(accent)与 stale(amber)的语义色一字不动
    eq(
        WF.DEFAULT_PALETTE.coverage,
        "rgba(181, 172, 201, 0.85)",
        "覆盖条仍是 accent 薰衣草(B-07 不受配色批影响)",
    );
    eq(
        WF.DEFAULT_PALETTE.stale,
        "rgba(212, 176, 118, 0.22)",
        "stale 仍是 amber 斜条纹",
    );
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
    eq(TW.fmtTimeMs(1.9996), "00:02.000", "毫秒四舍五入进位联动到秒");
    eq(TW.fmtTimeMs(59.9999), "01:00.000", "进位联动到分");
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
    // T33:§2.9 lowSample 是 code+ch 复合键(t32/deviations §N)——多轨同时低采样时
    // Tab3 轨头与 Tab2 轨行**消费同一份**,不再互相覆盖成一枚黄标。
    {
        const multi = TW.laneModelFromStore({
            errors: new Map([
                ["lowSample#4", { code: "lowSample", ch: 4 }],
                ["lowSample#11", { code: "lowSample", ch: 11 }],
                ["noTimeline", { code: "noTimeline" }],
            ]),
        });
        eq(
            multi.filter((l) => l.low).map((l) => l.n),
            [4, 11],
            "两轨同时低采样 ⇒ 两条泳道各自挂黄标(复合键)",
        );
        eq(
            TW.laneModelFromStore({
                errors: new Map([["lowSample", { code: "lowSample", ch: 6 }]]),
            })
                .filter((l) => l.low)
                .map((l) => l.n),
            [6],
            "裸 lowSample 键同样命中(消费侧与键形解耦)",
        );
    }
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
    // 契约 §0.4:captureProgress 只在播放中发,空态由 scvb.state 承载 ——
    // 停播打开面板(coverage 事件仓恒空)也不得误判空态
    check(
        !TW.isLanesEmpty({
            coverage: {},
            state: { features: { embedded: true, bytes: 3145728 } },
        }),
        "state.features.bytes>0 即非空(停播场景)",
    );
    check(
        !TW.isLanesEmpty({ coverage: {}, segments: { channels: [segCh] } }),
        "有段表即非空(快照 reason:snapshot 场景)",
    );
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
    // 桥面零新增(brief §0.11):tab-wave.js 里出现的 bridge 调用名必须都落在
    // 本卡契约面(§1.5/§1.6/§1.8/§1.18/§1.19/§1.22/§1.23/§1.24/§1.27)内
    const tw = src("web/output/tab-wave.js");
    const ALLOWED = new Set([
        "requestWaveform",
        "setVadParams",
        "setSegmentation",
        "previewAnalyze",
        "analyze",
        "editSegment",
        "setRange",
        "recaptureArm",
        "clearCoverage",
    ]);
    const called = [...tw.matchAll(/call\(\s*"(\w+)"/g)].map((m) => m[1]);
    check(called.length >= 9, `桥面调用点数量(实得 ${called.length})`);
    for (const n of new Set(called)) {
        check(ALLOWED.has(n), `桥面调用 ${n} 在本卡契约面内(零新增)`);
    }
    for (const n of ALLOWED) {
        check(called.includes(n), `Wave 2 桥面 ${n} 已接线`);
    }
    // requestWaveform 纪律:request/response,不当事件消费(不 on() 它)
    check(
        !/on\(["']scvb\.waveform/.test(tw),
        "无波形事件订阅(§1.27 绝不进事件流)",
    );
    // 复审回修钉桩:hidpi 的 dpr 变化监听必须有调用方(05 §6.1 重建闭环)
    check(
        tw.includes("observeResolution("),
        "dpr 变化重建后备存储已接线(05 §6.1)",
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
    // B-06:泳道状态灯 8×8 —— base.css 的 .sc-dot 默认 6px,两轴都须覆盖
    check(
        /\.wave-lane__light\s*\{[^}]*width:\s*var\(--sp-8\)[^}]*height:\s*var\(--sp-8\)/.test(
            html,
        ),
        "泳道状态灯 8×8 尺寸规则在(B-06)",
    );
    // 层序:Wave 4 用户裁定⑤⑥后 **VAD 罩在包络之下**(canvas 后画者在上)——
    // .20 的绿压在粉柱上会把「粉 + 白」整体染绿,与裁定⑤的验收句「不能盖住
    // 波形」冲突;图谱 §12 的 DOM 序对调,deviations §S 登记。
    const wf = src("web/output/canvas/waveform.js");
    check(
        wf.indexOf("fillStyle = pal.vad") < wf.indexOf("fillStyle = pal.env"),
        "静态层先 VAD 罩后包络柱(Wave 4 层序:绿罩不盖波形)",
    );
    check(
        wf.indexOf("fillStyle = pal.env") <
            wf.indexOf("fillStyle = pal.coverage"),
        "覆盖条仍画在最上(B-07)",
    );
    // Wave 5 用户裁定①:绿从「铺满包络之外的上下两片」收成**顶部一条标注带**,
    // 带下一寸不填(用户原话「像绿底放了个波」= 绿面积比波形还大)。
    check(
        /ctx\.fillStyle = pal\.vad;[\s\S]{0,200}ctx\.fillRect\(x0\(a\), 0, \(b - a\) \* colW, band\)/.test(
            wf,
        ),
        "VAD 只画顶部标注带(每 run 一次 fillRect,带高 = VAD_BAND_PX)",
    );
    check(
        !/if \(top > 0\) ctx\.rect\(/.test(wf) &&
            !/if \(bot < h\) ctx\.rect\(/.test(wf),
        "「铺满包络之外」的旧画法已删(上下两片空白不再填绿)",
    );
    check(
        !/ctx\.fillStyle = pal\.vad;\s*\n\s*for \(const \[a, b\] of runs\) \{\s*\n\s*ctx\.fillRect\(x0\(a\), 0, \(b - a\) \* colW, h\);/.test(
            wf,
        ),
        "满高绿罩铺底的更旧画法仍然不在",
    );
    check(
        /ctx\.fillStyle = pal\.vadEdge;[\s\S]{0,200}ctx\.fillRect\(x0\(a\), 0, \(b - a\) \* colW, 1\.5\)/.test(
            wf,
        ) && !/edgeY/.test(wf),
        "亮线只剩最上沿一条(带下已无罩体,下缘线失去依附对象)",
    );

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
log("=== ⑥ mock 端到端(两段式 / 五 op / 布防 / 清除 / setRange)===");
{
    const { createBridge } = await import(u("web/shared/bridge.js"));
    const driver = await import(u("web-preview/mock/state-driver.js"));
    const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

    async function withOutput(params, fn) {
        const s = driver.createPreviewSession({ role: "output", params });
        const b = createBridge({ role: "output", mockBackend: s.mock });
        const segEvents = [];
        b.on("scvb.segments", (p) => segEvents.push(p));
        s.start();
        await b.requestInitialState();
        segEvents.length = 0; // 快照帧不计入断言账
        const r = await fn(b, segEvents, s);
        s.stop();
        return r;
    }

    // ---- 两段式:拖动档 setVadParams 整包不触发段表;松手 300ms 防抖在
    //      mock 侧跑流水线 → reason:"vad" + diff(A-01/A-02 的数据面)
    await withOutput("fixture=fifteen-tracks", async (b, seen) => {
        const vad = {
            threshold_db: -40,
            hysteresis_db: 6,
            hangover_ms: 180,
            padding_pre_ms: 120,
            padding_post_ms: 200,
        };
        eq((await b.setVadParams(vad)).ok, true, "setVadParams 整包受理");
        await sleep(120);
        eq(seen.length, 0, "300ms 内不出段表(防抖在 mock 侧)");
        // 防抖重置:再来一发,前一发的计时器作废
        eq(
            (await b.setVadParams({ ...vad, threshold_db: -42 })).ok,
            true,
            "第二发重置防抖",
        );
        await sleep(420);
        eq(seen.length, 1, "松手后恰好一帧段表");
        const ev = seen[0];
        eq(ev.reason, "vad", "reason = vad(§2.8)");
        check(
            ev.diff && Number.isInteger(ev.diff.kept),
            "diff.kept 在(wave.diffKept 的 {k})",
        );
        // §2.8 字段纪律:segIdx 每次事件后重新编号(0 基连续)
        for (const c of ev.channels.slice(0, 2)) {
            check(
                c.segments.every((s, i) => s.segIdx === i),
                `ch${c.ch} segIdx 重新编号 0 基连续`,
            );
        }
        // 松手档只改写 auto 未锁定段:用户/锁定段逐字节保留
        const before = await b.setSegmentation({
            mode: "valley",
            sensitivity: 0.5,
            min_segment_ms: 420,
        });
        eq(before.ok, true, "setSegmentation 整包受理");
        await sleep(420);
        eq(seen.length, 2, "分段参数松手后一帧段表");
        eq(seen[1].reason, "segmentation", "reason = segmentation");
    });

    // ---- editSegment 五 op(§1.22/§5.4)+ notAdjacent + 布防 + 清除 + setRange
    await withOutput("fixture=fifteen-tracks", async (b, seen, s) => {
        const st = await b.requestInitialState();
        const ch = 1;
        const seg0 = (await b.requestWaveform(ch, 0, 10, 16)) || {};
        void seg0;
        // set_values:origin → user_edited 且 locked(J44 前半)
        let r = await b.editSegment(ch, "set_values", { segIdx: 0, pan: 12 });
        eq(r.ok, true, "set_values 受理");
        let last = seen[seen.length - 1];
        eq(last.reason, "edit", "编辑帧 reason = edit");
        const c1 = last.channels.find((c) => c.ch === ch);
        eq(
            [c1.segments[0].origin, c1.segments[0].locked, c1.segments[0].pan],
            ["user_edited", true, 12],
            "set_values 后置状态(origin/locked/值)",
        );
        // set_locked:**不改 origin**(§5.4)
        r = await b.editSegment(ch, "set_locked", { segIdx: 1, locked: true });
        eq(r.ok, true, "set_locked 受理");
        last = seen[seen.length - 1];
        const s1 = last.channels.find((c) => c.ch === ch).segments[1];
        check(s1.locked === true, "set_locked 锁上");
        check(s1.origin === "auto", "set_locked 不改 origin");
        // move_boundary(释放才发的那一发)
        const t0 = last.channels.find((c) => c.ch === ch).segments[1].t0S;
        r = await b.editSegment(ch, "move_boundary", {
            segIdx: 1,
            edge: "t0",
            tS: t0 + 0.4,
        });
        eq(r.ok, true, "move_boundary 受理");
        // split:两子段继承原值
        last = seen[seen.length - 1];
        const segsNow = last.channels.find((c) => c.ch === ch).segments;
        const tgt = segsNow[0];
        r = await b.editSegment(ch, "split", {
            segIdx: 0,
            tS: (tgt.t0S + tgt.t1S) / 2,
        });
        eq(r.ok, true, "split 受理");
        last = seen[seen.length - 1];
        const after = last.channels.find((c) => c.ch === ch).segments;
        eq(after.length, segsNow.length + 1, "split 段数 +1");
        near(after[0].pan, after[1].pan, 1e-9, "两子段继承原值(pan)");
        // merge:相邻可并;不相邻回 notAdjacent(UI 行内反馈词条已备)
        r = await b.editSegment(ch, "merge", { segIdxA: 0, segIdxB: 1 });
        eq(r.ok, true, "merge 相邻受理");
        r = await b.editSegment(ch, "merge", { segIdxA: 0, segIdxB: 2 });
        eq(r.reason, "notAdjacent", "merge 不相邻拒绝");

        // 布防(§1.23):armed:false 走 §5.5 reason;成态落 state.recapture;
        // mask=0∧start=end=0 = 撤销布防(无 reason)
        eq(
            (await b.recaptureArm(0, 1, 5)).reason,
            "noTracks",
            "布防拒绝 noTracks",
        );
        eq(
            (await b.recaptureArm(0b111, 5, 5)).reason,
            "noSelection",
            "布防拒绝 noSelection",
        );
        r = await b.recaptureArm(0b111, 30, 60, true);
        eq(r.armed, true, "布防受理");
        const rec = (await b.requestInitialState()).recapture;
        eq(
            [rec.armed, rec.tracksMask, rec.autoStop],
            [true, 7, true],
            "state.recapture 回读(armed/mask/autoStop)",
        );
        r = await b.recaptureArm(0, 0, 0);
        check(
            r.armed === false && !("reason" in r),
            "撤销布防 armed:false 无 reason",
        );

        // 清除(§1.24):真扣除 → clearedS>0、波形 covered 哨兵、coveragePct 降
        const covBefore = (await b.requestWaveform(ch, 20, 40, 64)).covered;
        check(
            covBefore.some((v) => v === 1),
            "清除前 20-40s 有覆盖列",
        );
        r = await b.clearCoverage(1, 20, 40);
        eq(r.ok, true, "clearCoverage 受理");
        check(r.clearedS > 0, "clearedS 为真扣除量");
        const tile = await b.requestWaveform(ch, 20, 40, 64);
        check(
            tile.covered.every((v) => v === 0),
            "清除区间 covered 全 0",
        );
        check(
            tile.minDb.every((v) => v === -160),
            "清除区间回未覆盖哨兵 -160(§1.27)",
        );
        // §1.5 coverage 口径:清干净的区间 dry-run 数不到段
        const pv = await b.previewAnalyze({
            tracksMask: 1,
            startS: 20,
            endS: 40,
        });
        eq(pv.intervals, 0, "previewAnalyze:range∩coverage=∅ → 0");

        // setRange(§1.8):Tab3「设为范围」manual 档;startS>=endS → badArg
        r = await b.setRange("manual", 30, 60);
        eq(r.ok, true, "setRange manual 受理");
        eq(
            (await b.requestInitialState()).global.range.mode,
            "manual",
            "Range 档位已切 manual",
        );
        eq(
            (await b.setRange("manual", 60, 60)).reason,
            "badArg",
            "退化选区拒绝 badArg 且不改 state",
        );
        void st;
        void s;
    });
}

// =============================================================================
log("=== ⑦ 交互纯函数 + 源码级纪律断言(Wave 2)===");
{
    // ---- 泳道点选(05 行 288 语义:toggle / shift 锚点连选并集追加)
    eq(TW.applyPick([], 3, false), [3], "点选追加");
    eq(TW.applyPick([3], 3, false), [], "再点 toggle 掉");
    eq(
        TW.applyPick([5, 2], 6, true),
        [5, 2, 3, 4, 6],
        "shift 以尾元素为锚点连选",
    );
    eq(TW.applyPick([2], 2, true), [2], "shift 含自身不重复");
    eq(TW.maskOfPicked([1, 3, 15]), 0b100000000000101, "轨表 → u16 位图");
    eq(TW.maskOfPicked([0, 16]), 0, "越界轨不入位图");

    // 布防条纹只盖布防轨(05 行 300 的 {布防轨}×{选区} 交集;无头 QA 实拍
    // 抓到过「一条满高带盖 15 泳道」的回归 —— 位图→条带这层必须钉住)
    eq(
        TW.maskRuns(TW.maskOfPicked([3, 4, 7, 12])),
        [
            { ch0: 3, count: 2 },
            { ch0: 7, count: 1 },
            { ch0: 12, count: 1 },
        ],
        "相邻布防轨并成一条,不相邻各自成条",
    );
    eq(TW.maskRuns(0), [], "未布防 ⇒ 零条带");
    eq(
        TW.maskRuns(
            TW.maskOfPicked(Array.from({ length: 15 }, (_, i) => i + 1)),
        ),
        [{ ch0: 1, count: 15 }],
        "全轨布防 ⇒ 并成满高一条",
    );
    check(
        /wave-recapband__seg[\s\S]{0,240}\(r\.ch0 - 1\) \* local\.laneH/.test(
            src("web/output/tab-wave.js"),
        ),
        "recapband 按 maskRuns 建子块(不再整条满高),子块几何读运行时行高",
    );

    // ---- segIdx 重绑(brief §0.7):时间锚中点包含 → 最大重叠 → 失效
    const segsNew = [
        { t0S: 0, t1S: 4 },
        { t0S: 4, t1S: 9 },
        { t0S: 9, t1S: 12 },
    ];
    eq(
        TW.rebindSegKeys([{ idx: 5, t0S: 4.2, t1S: 8.8 }], segsNew),
        [{ idx: 1, t0S: 4, t1S: 9 }],
        "中点包含重绑(旧 idx 作废)",
    );
    eq(
        TW.rebindSegKeys([{ idx: 0, t0S: 40, t1S: 44 }], segsNew),
        [],
        "找不到即失效(不保留旧下标)",
    );
    eq(
        TW.rebindSegKeys(
            [
                { idx: 0, t0S: 4.1, t1S: 8.9 },
                { idx: 1, t0S: 4.2, t1S: 8.8 },
            ],
            segsNew,
        ).length,
        1,
        "重复命中去重",
    );

    // ---- 边界吸附(A-14:半径按 px/秒换算;Alt 关吸附)
    eq(
        TW.snapBoundary(10.02, [9.0, 10.05], 100, false),
        { tS: 10.05, snapped: true },
        "半径内吸最近谷点",
    );
    eq(
        TW.snapBoundary(10.02, [9.0, 10.05], 100, true),
        { tS: 10.02, snapped: false },
        "Alt 关吸附",
    );
    eq(
        TW.snapBoundary(10.02, [9.0], 100, false).snapped,
        false,
        "半径外不吸附",
    );

    // ---- scope 内计数(确认框 {k}/{l} 与「将覆盖 K 段」)
    const segTable = {
        channels: [
            {
                ch: 1,
                segments: [
                    { t0S: 0, t1S: 5, origin: "user_edited", locked: false },
                    { t0S: 5, t1S: 9, origin: "auto", locked: true },
                    { t0S: 9, t1S: 20, origin: "auto", locked: false },
                ],
            },
            { ch: 2, segments: [{ t0S: 0, t1S: 9, origin: "auto" }] },
        ],
    };
    eq(
        TW.countsInScope(segTable, 0b1, 0, 10),
        { marks: 1, locked: 1, overlap: 3 },
        "countsInScope(mask×区间相交)",
    );
    eq(TW.countsInScope(segTable, 0b11, 0, 10).overlap, 4, "多轨并入 overlap");

    // ---- 检查器读数
    eq(TW.fmtSigned(2, 1), "+2.0", "带符号正值");
    eq(TW.fmtSigned(-14.25, 1), "-14.2", "负值四舍五入");
    eq(TW.fmtSigned(-0.01, 1), "0.0", "-0 归零");

    // ---- 源码级纪律(tab-wave.js)
    const tw = src("web/output/tab-wave.js");
    check(
        TW.PARAM_THROTTLE_MS >= 20,
        `拖动下发 ≤50Hz(节流 ${TW.PARAM_THROTTLE_MS}ms ≥ 20ms)`,
    );
    check(
        tw.includes('call("setVadParams", { ...local.vadParams })'),
        "setVadParams 五字段整包下发(不发单字段)",
    );
    check(
        tw.includes('call("setSegmentation", { ...local.segmentation })'),
        "setSegmentation 三字段整包下发",
    );
    // UI 不自建 300ms 防抖去调 analyze(brief §0.5):releaseSlider 体内无 analyze
    const relBody = tw.slice(
        tw.indexOf("function releaseSlider"),
        tw.indexOf("function armCountdown"),
    );
    check(
        !relBody.includes("analyze"),
        "松手档不调 analyze(300ms 防抖在 C++/mock 侧)",
    );
    // 释放才发(§1.22):拖动帧函数体内无 editSegment,提交在 commit/up 路径
    const dragBody = tw.slice(
        tw.indexOf("function updateBoundDrag"),
        tw.indexOf("function commitBoundDrag"),
    );
    check(
        !dragBody.includes("sendEdit") && !dragBody.includes("editSegment"),
        "边界拖动帧不发 editSegment(释放才发)",
    );
    const commitBody = tw.slice(
        tw.indexOf("function commitBoundDrag"),
        tw.indexOf("function mountSelection"),
    );
    check(
        commitBody.includes('"move_boundary"'),
        "commitBoundDrag 提交 move_boundary",
    );
    // segIdx 重绑(brief §0.7):onSegments 里必须过 rebindSegKeys
    const onSegBody = tw.slice(
        tw.indexOf("function onSegments"),
        tw.indexOf("function onCaptureProgress"),
    );
    check(
        onSegBody.includes("rebindSegKeys"),
        "onSegments 重绑选中段(segIdx 每事件重编号)",
    );
    // 中央写闸:写面统一过 isWriteBlocked()(采样点计数下限)
    const gateHits = (tw.match(/isWriteBlocked\(\)/g) || []).length;
    check(gateHits >= 10, `isWriteBlocked() 闸口覆盖(实得 ${gateHits} 处)`);
    // 频率纪律:previewAnalyze 节流、视口静止 120ms 取新块
    check(TW.PREVIEW_THROTTLE_MS >= 100, "previewAnalyze 节流窗");
    check(
        tw.includes("IDLE_REFETCH_MS"),
        "视口静止 120ms 才取新块已接线(§6.3)",
    );
}

// =============================================================================
log("=== ⑧ Wave 2 新词条三语(反馈件族;U17 注记)===");
{
    const KEYS2 = [
        "wave.armReason.noTracks",
        "wave.armReason.noSelection",
        "wave.armReason.readOnly",
        "wave.armReason.noTimeline",
        "wave.notAdjacent",
        "wave.btnMerge",
        "wave.recaptureOverlap",
        "wave.diffItem",
        "wave.diffAddedRemoved",
        "wave.clearedCoverage",
    ];
    const ph = (s) => (String(s).match(/\{\w+\}/g) || []).sort().join(",");
    for (const k of KEYS2) {
        for (const lang of ["zh", "en", "fr"]) {
            check(
                typeof T[lang][k] === "string" && T[lang][k].length > 0,
                `${lang}.${k} 非空`,
            );
        }
        eq(ph(T.en[k]), ph(T.zh[k]), `${k} en 占位符与 zh 一致`);
        eq(ph(T.fr[k]), ph(T.zh[k]), `${k} fr 占位符与 zh 一致`);
    }
}

// =============================================================================
log("=== ⑨ 空闲零 rAF(brief §0.13 性能预算;T33 收尾)===");
{
    const tw = src("web/output/tab-wave.js");
    // Tab3 只有三处 rAF 循环,三处都必须**自停**:
    //   ① layers.js 的分层循环 —— tick() 返回 false(无脏层、无动态层)即不续帧;
    //   ② playhead.js 的插值循环 —— 事件 isPlaying=false 即不续帧;
    //   ③ tab-wave 的帧时账 ticker —— 无交互且未播放即不续帧。
    check(
        /if \(tick\(ts\)\) \{\s*raf = requestAnimationFrame\(loop\);\s*\} else \{\s*raf = 0;/.test(
            src("web/output/canvas/layers.js"),
        ),
        "layers 循环在无脏层时自停(空闲零 rAF)",
    );
    check(
        /if \(ev && ev\.isPlaying\) raf = requestAnimationFrame\(loop\);/.test(
            src("web/output/canvas/playhead.js"),
        ),
        "playhead 循环只在播放中续帧",
    );
    check(
        /interactionActive\(\) \|\| \(p && p\.isPlaying\)[\s\S]{0,120}requestAnimationFrame\(loop\)/.test(
            tw,
        ),
        "帧时账 ticker 只在交互中或播放中续帧",
    );
    // 反向哨兵:模块顶层不许起常驻循环(mount 里挂一个 setInterval/rAF 长跑)
    check(
        !/setInterval\(/.test(tw),
        "tab-wave 无常驻 setInterval(空闲不烧 CPU)",
    );
    // 拖动期不跑整页:交互路径请求的是外壳合帧的 requestRender,而画布走 schedulePaint
    check(
        /function schedulePaint\(\)[\s\S]{0,400}if \(!isPanelActive\(\)\) return;/.test(
            tw,
        ),
        "非前台不烧 canvas(schedulePaint 早退)",
    );
    // 「只投影当前激活 tab」在 **rAF 侧**也要成立:停在 Tab1/Tab2 且宿主在播放时,
    // Tab3 的播放头插值与帧时账两条自持循环必须一起停(否则每显示帧仍有 2 个回调
    // 在给 display:none 的 Tab3 写 DOM)。
    check(
        /function onPlayhead\(p\) \{[\s\S]{0,600}if \(!isPanelActive\(\)\) \{[\s\S]{0,200}playhead\.stop\(\);\s*return;/.test(
            tw,
        ),
        "非前台不驱动播放头插值(onPlayhead 早退并停帧)",
    );
    check(
        /function ensureTicker\(\)[\s\S]{0,400}if \(!isPanelActive\(\)\) \{[\s\S]{0,120}return;/.test(
            tw,
        ),
        "非前台不起帧时账(ensureTicker 早退)",
    );
    check(
        /function resumePlayhead\(\)[\s\S]{0,300}onPlayhead\(local\.playheadEv\);/.test(
            tw,
        ) && /function render\(\)[\s\S]{0,400}resumePlayhead\(\);/.test(tw),
        "切回本页由 render 补一次起帧(按需起帧的配对不变式)",
    );
    check(
        /if \(\s*ev\.isPlaying &&\s*degradeLevel\(\) >= 1/.test(
            src("web/output/canvas/playhead.js"),
        ),
        "降级档节流只在播放中生效(停播那一帧不许被吞,否则位置永久错位)",
    );
}

// =============================================================================
log("\n=== ⑩ Wave 3 视觉修的口径钉子(对抗校验修订批)===");
{
    const tw = src("web/output/tab-wave.js");
    const html = src("web/output/index.html");

    // (a) 边界拖拽:点一下不拖 ⇒ 零位移不发 move_boundary(契约 §5.4 的后置会把
    //     该段变成 user_edited + locked,且 analyze(clearManual) 对 locked 免疫)
    check(
        /function commitBoundDrag\(\)[\s\S]{0,900}if \(tS === Math\.round\(d\.origT \* 1000\) \/ 1000\) return;[\s\S]{0,200}sendEdit\(d\.ch, "move_boundary"/.test(
            tw,
        ),
        "commitBoundDrag 有零位移短路(误点不再把 auto 段钉死成 locked)",
    );

    // (b) 曲线两色锁死 05 行 310 / 图谱 §12 ③:pan = 薰衣草 181,172,201、vol = 白;
    //     两级权重只走线宽(B-09)与 alpha,不改色相
    check(
        !/198,\s*190,\s*232/.test(tw),
        "手动档 pan 不再用 tokens 里查无此项的 rgb(198,190,232)",
    );
    eq(
        (tw.match(/rgba\(181,172,201,\.(?:98|82)\)/g) || []).length,
        2,
        "pan 两档同色相(181,172,201),只差 alpha",
    );
    check(
        /const CURVE_HALO_W = 0\.8;/.test(tw) &&
            /ctx\.lineWidth = lw \+ CURVE_HALO_W;/.test(tw),
        "曲线深底 halo 收到 +0.8px(不再以 2.4:1 的墨量压过本色)",
    );

    // (c) 角标落在泳道底带,避开 pan(y 5..19)与 vol(y 15..22)两条曲线
    check(
        /\.wave-seg-marks \{[^}]*bottom: 2px;/.test(html),
        "段角标组落在泳道底带(bottom:2px,让开 pan/vol 曲线)",
    );
    check(
        /\.wave-lane__badges \{[^}]*overflow: hidden;/.test(html),
        "角标层 overflow:hidden(末段角标不溢到右缘 44px 刻度列)",
    );
    check(
        /\.wave-seg-lock \{[^}]*pointer-events: auto;/.test(html),
        "锁定角标收回指针事件(「锁定」词条的 title 才弹得出来)",
    );

    // (d) 轨头 158px:轨名优先于覆盖率读数
    check(
        /\.wave-lane__label \{[^}]*min-width: 38px;/.test(html),
        "轨头 label 保底 38px(「Ad-lib 1/2」不再截成同一串)",
    );
    check(
        /\.wave-lane__covseg \{[^}]*flex: 0 1 auto;[^}]*text-overflow: ellipsis;/.test(
            html,
        ),
        "覆盖率读数可收缩并省略(次要读数先让位)",
    );

    // (e) VAD alpha 两处同源:tab-wave 的 computedPalette 不许再写第二份字面量
    check(
        /pal\.vad = `rgba\(\$\{v\("--wave-vad"\)\}, \$\{VAD_ALPHA\}\)`/.test(
            tw,
        ),
        "computedPalette 的 VAD alpha 从 waveform.js import(不写第二份字面量)",
    );
}

// =============================================================================
log("=== ⑪ Wave 4 用户 preview 八条反馈(配色 / 勾选框 / 缩放条 / blit)===");
{
    const tw = src("web/output/tab-wave.js");
    const html = src("web/output/index.html");
    const css = src("web/shared/tokens.css");

    // ---- ① 文案:「播完自动停」整句化,三语齐(key 不变)
    eq(T.zh.autoStop, "播放结束自动停止", "zh autoStop 改完整句");
    check(
        /playback ends/i.test(T.en.autoStop),
        "en autoStop 是同义完整句(不再是 Auto-stop 短语)",
    );
    check(
        /lecture/i.test(T.fr.autoStop),
        "fr autoStop 是同义完整句(不再是 Arrêt auto 短语)",
    );
    check(
        ![T.zh, T.en, T.fr].some((d) =>
            Object.values(d).some((v) =>
                /播完自动停|Auto-stop|Arrêt auto/.test(String(v)),
            ),
        ),
        "旧短语在三语词条值里零命中(html 的旧文案由 applyI18n 覆盖)",
    );

    // ---- ② 自绘勾选框:仍是真 input(只视觉隐藏),同族形制 + 焦点环 + ≥24 命中
    check(
        /<input\s+type="checkbox"\s+class="wave-autostop__input"/.test(html),
        "autoStop 仍是真 checkbox(不是 div/role=checkbox)",
    );
    check(
        /\.wave-autostop__input \{[^}]*opacity: 0;/.test(html),
        "input 只做视觉隐藏(留在 DOM 与 tab 序)",
    );
    check(
        /\.wave-autostop__input:checked \+ \.wave-autostop__box \{/.test(html),
        "勾选态由原生 :checked 驱动自绘框",
    );
    check(
        /\.wave-autostop__input:focus-visible \+ \.wave-autostop__box \{[^}]*outline:/.test(
            html,
        ),
        "focus-visible 焦点环落在自绘框上",
    );
    // RE-06 的实测口径:0.5 档要 ≥24 物理 px ⇒ ≥48 CSS px(hit.js
    // hitExpansionPx(14) = 17/侧)。扩展挂 **label**(横轴 94..127px 早就够,
    // 缺的只有 14px 的纵轴);挂 12px 框上会横向盖住左邻按钮(行内 gap 9px)。
    check(
        /\.wave-autostop::after \{[^}]*top: -8px;[\s\S]{0,80}bottom: -26px;/.test(
            html,
        ),
        "RE-06:命中扩展挂 label,14 + 8 + 26 = 48 CSS px(= 0.5 档 24 物理 px)",
    );
    check(
        !/\.wave-autostop__box::after \{/.test(html),
        "扩展不挂 12px 框上(±18px 会抢左邻按钮的事件)",
    );

    // ---- ③ blit = **位图搬运**,平移与缩放走同一条路径;两者共用「静止 120ms」闸
    //
    // 用户 preview 两轮都报「放大缩小错乱」。第一轮的实现是「重跑矢量画笔 +
    // ctx.scale」——非等比 scale 把 45° 斜纹剪成缓坡、lineWidth 横向拉粗;
    // 第二轮改成「只 translate,跨度一变就不 blit」——缩放期波形整幅停在旧
    // 视口的比例上,而标尺/曲线/边界/播放头每帧都跟着新视口走,层与层时间轴
    // 对不上(无头实测:旧图保留 ~150ms 才被新块换掉)。
    // 终修:老底存**位图**,一次 drawImage 按时间映射贴过去 —— 光栅重采样不
    // 重跑画笔,斜纹与线宽都不会变形,平移(dw==w)与缩放(dw!=w)同一条路径。
    check(
        /function snapshotLane\([\s\S]{0,700}bctx\.drawImage\(srcCanvas, 0, 0\)/.test(
            tw,
        ),
        "命中后把整幅存成位图老底(blit 的搬运源,不再存 tile 重跑画笔)",
    );
    check(
        !/paintWaveTile\(ctx, last\.tile/.test(tw) &&
            !/ctx\.scale\(\(last\.endS - last\.startS\) \/ span, 1\)/.test(tw),
        "blit 不再重跑矢量画笔(斜纹被剪切/线宽被拉粗的根因已删除)",
    );
    check(
        /const dx = \(\(last\.startS - vp\.startS\) \/ span\) \* w;[\s\S]{0,200}const dw = \(\(last\.endS - last\.startS\) \/ span\) \* w;/.test(
            tw,
        ),
        "老底按**时间映射**定位(dx/dw 同时吃下平移与缩放)",
    );
    check(
        /ctx\.clearRect\(0, 0, w, laneH\);[\s\S]{0,400}ctx\.drawImage\(\s*last\.bmp,/.test(
            tw,
        ),
        "blit 前整幅 clearRect(否则边缘留上一帧拖影)",
    );
    check(
        /const sameVp =[\s\S]{0,400}!sameVp &&\s*moving &&\s*last &&\s*last\.bmp/.test(
            tw,
        ),
        "画布已是当前视口时不清屏(宽度抖动导致的键失配不该闪空)",
    );
    check(
        /if \(!got\) \{[\s\S]{0,1200}clearRect/.test(tw),
        "取数回 null 才清(降级底有界,不把错档的图永久留在屏上)",
    );
    // 缩放那半边的钉子:缩放期每帧跨度都不同 = 每帧新键,LRU 与在途去重全失效,
    // 不设闸就是每帧 × 每条可见泳道一次桥调用(实测 20 次 ctrl+滚轮 = 308 次)。
    check(
        /视口在动就\*\*不取新块\*\*[\s\S]{0,400}continue;/.test(tw),
        "blit 分支内不取新块(05 §6.3:静止 120ms 后才取)",
    );
    check(
        /if \(moving\) continue;[\s\S]{0,600}waveSource\.getTile\(/.test(tw),
        "没有老底可搬(首绘)也过 moving 闸,取数分支前无条件 continue",
    );
    check(
        /const moving = !!local\.vpIdleTimer;/.test(tw) &&
            /IDLE_REFETCH_MS\)/.test(tw),
        "moving 唯一真源 = vpIdleTimer(到点即 IDLE_REFETCH_MS = 120 标脏补取)",
    );
    check(
        /if \(!got\) \{[\s\S]{0,700}drawn\.startS !== reqStartS \|\| drawn\.endS !== reqEndS/.test(
            tw,
        ),
        "迟到的 null 不擦掉更新的一帧(比对请求视口与画布现视口)",
    );
    check(
        /if \(!got\) \{[\s\S]{0,900}const w2 = stageWidth\(\);[\s\S]{0,300}backingK\(\)/.test(
            tw,
        ),
        "null 分支的宽/k 当场重读(不用发起帧的闭包值)",
    );

    // ---- ④ 两条缩放拖拽条:运行时行高 + 键盘可达 + aria
    eq(TW.LANE_H_DEFAULT, 34, "行高默认档仍是 34(设计稿 1812)");
    eq(TW.LANE_H, TW.LANE_H_DEFAULT, "LANE_H 兼容别名 = 默认档");
    eq([TW.LANE_H_MIN, TW.LANE_H_MAX], [22, 88], "行高区间 22..88");
    eq(TW.clampLaneH(10), 22, "行高下夹");
    eq(TW.clampLaneH(999), 88, "行高上夹");
    eq(TW.clampLaneH(undefined), 34, "非法值回默认档");
    near(TW.laneHPercent(34), (34 - 22) / 66, 1e-9, "行高 → 行程比");
    eq(TW.laneHFromPercent(0), 22, "行程比 0 = 最矮");
    eq(TW.laneHFromPercent(1), 88, "行程比 1 = 最高");
    // 曲线纵向几何按行高等比缩放(34 档的既有断言不受影响)
    near(TW.panYPx(100), 19, 1e-9, "pan +100 @34 仍是 19");
    near(TW.panYPx(100, 68), 38, 1e-9, "行高翻倍 ⇒ pan 落点等比翻倍");
    near(TW.volYPx(-24, 68), 44, 1e-9, "行高翻倍 ⇒ vol 基线等比翻倍");
    // 编译期常量不再被交互期几何引用(几何唯一真源 = local.laneH)
    check(
        !/[^_]\bLANE_H\b(?!_)/.test(
            tw.slice(tw.indexOf("function createTabWave")),
        ),
        "createTabWave 内零 LANE_H 直引(几何一律读 local.laneH)",
    );
    // 横向缩放条:对数刻度换算 + 视口中心为锚
    near(TW.zoomMaxFactor(300), 300, 1e-9, "最大倍率 = 全长 / MIN_SPAN_S");
    near(TW.zoomFactorFromPercent(0, 300), 1, 1e-9, "行程 0 = 全览 ×1");
    near(TW.zoomFactorFromPercent(1, 300), 300, 1e-9, "行程 1 = 最大倍率");
    near(
        TW.zoomPercentOfFactor(TW.zoomFactorFromPercent(0.37, 300), 300),
        0.37,
        1e-9,
        "行程 ↔ 倍率互逆",
    );
    check(
        /const anchorT = \(vp\.startS \+ vp\.endS\) \/ 2; \/\/ 视口中心为锚/.test(
            tw,
        ),
        "横向缩放条以视口中心为锚(与 Ctrl+滚轮同一 timeline API)",
    );
    for (const gb of [
        "wave-hzoom-bar",
        "wave-hzoom-thumb",
        "wave-hzoom-value",
        "wave-vzoom",
        "wave-vzoom-thumb",
    ]) {
        check(html.includes(`data-gb="${gb}"`), `新件锚点 ${gb} 就位`);
    }
    check(
        /data-gb="wave-hscroll-thumb"/.test(html) &&
            /\.wave-hzoom__thumb \{/.test(html),
        "既有底部滚动条(平移)保留不变",
    );
    for (const [sel, key] of [
        ["wave-hzoom-bar", "wave.hZoomBar"],
        ["wave-vzoom", "wave.vZoomBar"],
    ]) {
        const i = html.indexOf(`data-gb="${sel}"`);
        const win = html.slice(Math.max(i - 400, 0), i + 600);
        check(/role="slider"/.test(win), `${sel} 是 role=slider`);
        check(/tabindex="0"/.test(win), `${sel} 键盘可聚焦`);
        check(/aria-valuenow=/.test(win), `${sel} 有 aria-valuenow`);
        check(/aria-valuemin=/.test(win), `${sel} 有 aria-valuemin`);
        check(/aria-valuemax=/.test(win), `${sel} 有 aria-valuemax`);
        check(
            win.includes(`data-t-aria="${key}"`),
            `${sel} 的 aria-label 走词条 ${key}`,
        );
    }
    check(
        /\.wave-lane__stage \{[^}]*height: var\(--lane-h, 34px\);/.test(html),
        "泳道舞台高读运行时变量 --lane-h",
    );
    check(
        /function applyLaneH\([\s\S]{0,900}local\.lastPaint\.clear\(\)/.test(
            tw,
        ),
        "行高变化清 blit 老底(旧行高的位图会纵向错位)",
    );
    // ---- ④a 修订轮:两条杆的 aria 同源 + 键盘全套 + 拖拽态兜底 + 纵向杆自留槽
    check(
        /attr\(els\.hzoomBar, "aria-valuetext", zoomLabel\(vp, durS\)\)/.test(
            tw,
        ),
        "横向杆 aria-valuetext 复用可见读数(线性 valuenow 与对数行程不同源)",
    );
    check(
        /attr\(els\.vzoom, "aria-valuetext", v \+ "px"\)/.test(tw),
        "纵向杆 aria-valuetext 带单位(裸 22..88 播报读不出是什么)",
    );
    for (const [bar, name] of [
        ["hzoomBar", "横向"],
        ["vzoom", "纵向"],
    ]) {
        const i = tw.indexOf(`els.${bar}.addEventListener("keydown"`);
        const win = tw.slice(i, i + 1400);
        check(
            i > 0 && /"Home"/.test(win) && /"End"/.test(win),
            `${name}杆 role=slider 的 Home/End 已实现`,
        );
        check(
            /"PageUp"/.test(win) && /"PageDown"/.test(win),
            `${name}杆 role=slider 的 PageUp/PageDown 已实现`,
        );
    }
    for (const up of ["hup", "vup"]) {
        check(
            new RegExp(
                `window\\.addEventListener\\("pointerup", ${up}\\)`,
            ).test(tw),
            `拖拽态在窗级也收 pointerup(${up}:setPointerCapture 抛错时的兜底)`,
        );
    }
    check(
        /if \(e\.buttons === 0\) \{[\s\S]{0,200}local\.hzoomDrag = null;/.test(
            tw,
        ) &&
            /if \(e\.buttons === 0\) \{[\s\S]{0,200}local\.vzoomDrag = null;/.test(
                tw,
            ),
        "pointermove 里按 buttons===0 兜底收尾(拖拽态卡住 = 悬停即跳值)",
    );
    // ---- Wave 5 用户裁定②:纵向缩放条移到窗口右下角(与纵向滚动条同一右缘、
    //      在其轨道下方),泳道区内的 24px 自留槽随之撤销
    eq(TW.VZOOM_GUTTER_W, undefined, "纵向缩放条自留槽常量已撤销(裁定②)");
    check(
        /Math\.max\(w - HEAD_W - SCALE_COL_W, 0\)/.test(tw) &&
            !/export const VZOOM_GUTTER_W/.test(tw),
        "舞台宽拿回自留槽(= 容器宽 − 158 − 44,C-04 舞台坐标系)",
    );
    check(
        /\.wave-ruler__scale \{[^}]*margin-right: 44px;/.test(html) &&
            /\.wave-overlay \{[^}]*right: 44px;/.test(html) &&
            /\.wave-lane__badges \{[^}]*right: 44px;/.test(html) &&
            /\.wave-lane__static \{[^}]*right: 44px;/.test(html),
        "标尺/overlay/角标层/静态层四处右让口径归并到单一 44(code-review finding 1)",
    );
    check(
        /els\.rulerScale\.style\.marginRight = SCALE_COL_W \+ sb \+ "px"/.test(
            tw,
        ) && !/els\.vzoom\.style\.right/.test(tw),
        "syncScrollGutter 不再给纵向杆算右让(它已在底部条的常规流里)",
    );
    {
        // 位置:落在 `.wave-hzoom` 行内、且排在横向缩放控件**之后** = 最右端
        const bar = html.indexOf('class="wave-hzoom"');
        const hz = html.indexOf('class="wave-hzoom__zoomctl"', bar);
        const vz = html.indexOf('class="wave-hzoom__vzoomctl"', bar);
        const barEnd = html.indexOf('class="segment-inspector"');
        check(
            bar > 0 && vz > hz && vz < barEnd,
            "纵向缩放条在底部条行内、横向缩放控件之后(窗口右下角、滚动条轨道下方)",
        );
        check(
            html.indexOf('class="wave-window"') < bar &&
                !/data-gb="wave-vzoom"[\s\S]{0,200}aria-orientation="vertical"/.test(
                    html,
                ),
            "泳道区内不再有该件(竖杆形制随位置改成短横轨)",
        );
    }
    eq(TW.VZOOM_W, 44, "纵向缩放条轨长 44(与 .wave-hzoom__zoombar 同族短轨)");
    check(
        /\.wave-vzoom \{[^}]*width: 44px;[^}]*height: 6px;/.test(html),
        "杆本体 44×6 横轨",
    );
    check(
        /\.wave-vzoom::after \{[^}]*inset: -7px -8px;/.test(html),
        "命中扩展 20×60 CSS px(纵轴吃满底条 20px 行高;与横向杆同账)",
    );
    check(
        /return laneHFromPercent\(\(e\.clientX - rect\.left\) \/ rect\.width\)/.test(
            tw,
        ),
        "拖拽换算改横轴(clientX ÷ 轨宽)",
    );
    check(
        /els\.vzoomThumb\.style\.left = laneHPercent\(v\) \* 100 \+ "%"/.test(
            tw,
        ),
        "thumb 走百分比行程(与 .wave-hzoom__zoomthumb 同一套定位)",
    );
    check(
        /if \(els\.vzoomVal\) text\(els\.vzoomVal, "⇕ " \+ v\)/.test(tw) &&
            html.includes('data-gb="wave-vzoom-value"'),
        "补可见读数「⇕ 34」(此前只有 tooltip / aria-valuetext)",
    );

    // ---- ⑤⑥ 新增 token 与「只改波形本体」纪律
    for (const t of [
        "--wave-vad:",
        "--wave-vad-edge:",
        "--wave-env-pink:",
        "--wave-env-core:",
    ]) {
        check(css.includes(t), `tokens.css 含 ${t}`);
    }
    check(
        /用户 preview 裁定[\s\S]{0,900}--wave-vad: 88, 208, 148;[^\n]*更鲜亮/.test(
            css,
        ),
        "新 token 注明「用户 preview 裁定:比 --sem-green 更鲜亮」",
    );
    check(
        !/--sem-green: 88/.test(css) &&
            css.includes("--sem-green: 120, 176, 142;"),
        "既有 token --sem-green 值未被改动(只新增不改既有值)",
    );
    check(
        css.includes("--wave-env: rgba(196, 190, 220, 0.5);"),
        "稿内原值 --wave-env 保留存档(不改既有值)",
    );

    // ---- ⑦⑧ 边界手柄可发现性 + 分割/合并说明可循
    check(
        /\.wave-bhandle \{[^}]*cursor: ew-resize;/.test(html),
        "边界手柄 cursor: ew-resize",
    );
    check(
        /\.wave-bhandle \{[^}]*pointer-events: auto;/.test(html),
        "手柄收回 pointer-events(否则 title tooltip 永远弹不出)",
    );
    // 修订轮:① `.wave-bhandle::after` 对有效靶区一寸也加不了(命中判定在 JS
    // 里按 BOUNDARY_HIT_PX=6 对 .wave-lanes 做,手柄只在已命中 ±6 时才出现),
    // 纯装饰 → 删;② aria-label 写在 aria-hidden 的子树上是死代码 → 删,
    // 语义由 title tooltip 单独承担。
    check(
        !/\.wave-bhandle::after \{/.test(html),
        "手柄不挂假的 RE-06 扩展(有效靶区就是 ±6 CSS px,05 §6.3 边界专用口径)",
    );
    check(
        /function setBoundHandleTip\([\s\S]{0,900}setTitle\(els\.bhandle, s\);/.test(
            tw,
        ) &&
            !/function setBoundHandleTip\([\s\S]{0,900}setAttribute\("aria-label"/.test(
                tw,
            ),
        "手柄只写 title(节点 aria-hidden,aria-label 无消费者)",
    );
    check(
        /data-gb="wave-boundary-handle"[\s\S]{0,200}aria-hidden="true"/.test(
            html,
        ),
        "手柄仍是 aria-hidden 的纯视觉浮标(不进 tab 序 / 无障碍树)",
    );
    for (const [lang, dict] of [
        ["zh", T.zh],
        ["en", T.en],
        ["fr", T.fr],
    ]) {
        for (const k of [
            "wave.boundarySnapTip",
            "wave.hZoomBar",
            "wave.vZoomBar",
        ]) {
            check(
                typeof dict[k] === "string" && dict[k].length > 0,
                `${lang} 有词条 ${k}`,
            );
        }
        const tip = dict["wave.boundaryHandleTip"] || "";
        check(/Alt/i.test(tip), `${lang} 边界 tooltip 说清 Alt 关吸附`);
        check(
            /分割|split|scinder/i.test(tip),
            `${lang} 边界 tooltip 说清双击分割(反馈⑧)`,
        );
        check(
            /Delete|合并|merge|fusionner|Suppr/i.test(tip),
            `${lang} 边界 tooltip 说清 Delete 合并(反馈⑧)`,
        );
    }
}

// =============================================================================
log("=== ⑫ Wave 5 用户 preview 第二轮四条(绿罩/缩放条位/选中态/检查器)===");
{
    const tw = src("web/output/tab-wave.js");
    const html = src("web/output/index.html");

    // ---- ③ 选中段高亮:泳道内画薰衣草半透明底 + 四边实框
    check(
        /const SEG_SEL_FILL = "rgba\(181,172,201,\.22\)"/.test(tw) &&
            /const SEG_SEL_EDGE = "rgba\(214,208,235,\.95\)"/.test(tw) &&
            /const SEG_SEL_EDGE_W = 2;/.test(tw),
        "选中段高亮配色 = 薰衣草底 .22 + 亮薰衣草 2px 边(亲验:点段同时勾选该轨,行底已是 .16)",
    );
    check(
        /local\.selectedCh === ch && local\.selectedSegs\.length/.test(tw),
        "高亮按 local.selectedSegs **整表**画(shift 连选的两段都高亮)",
    );
    {
        const i = tw.indexOf("local.selectedCh === ch && local.selectedSegs");
        const win = tw.slice(i, i + 1200);
        check(
            /ctx\.fillStyle = SEG_SEL_FILL;[\s\S]{0,120}ctx\.fillRect\(cx0, y0, cw, laneH\)/.test(
                win,
            ),
            "半透明底铺满段 × 泳道行高",
        );
        check(
            /ctx\.fillRect\(cx0, y0, cw, ew\); \/\/ 上/.test(win) &&
                /ctx\.fillRect\(cx0, y0 \+ laneH - ew, cw, ew\); \/\/ 下/.test(
                    win,
                ),
            "上下两条水平边线",
        );
        check(
            /if \(sx0 >= 0\) ctx\.fillRect\(cx0, y0, ew, laneH\)/.test(win) &&
                /if \(sx1 <= w\) \{[\s\S]{0,120}ctx\.fillRect\(cx0 \+ cw - ew, y0, ew, laneH\)/.test(
                    win,
                ),
            "左右两条竖边只在段端真的在视口内时画(否则成假边界)",
        );
        // 层序:高亮在曲线/边界**之前**,且在「曲线可见 toggle 灭 → continue」之前
        check(
            i < tw.indexOf("drawStepCurve(ctx, segs, vp, w, y0, rampPx,") &&
                i <
                    tw.indexOf(
                        'eye.eye.getAttribute("aria-pressed") === "false"',
                    ),
            "高亮画在曲线/边界之前,且不受曲线可见 toggle 影响(选中态属身份指示)",
        );
    }
    check(
        /local\.overlayDirty = true;[\s\S]{0,120}requestRender\(\);\s*\}\s*\n\s*function clearSegSelection/.test(
            tw,
        ),
        "selectSegment 标脏共享动态层(否则 schedulePaint 空转,高亮画不出来)",
    );

    // ---- ④ 检查器「常驻 + 开关」(覆盖 C-11 的条件渲染)
    check(
        /inspectorOpen: true,/.test(tw),
        "面板开关本地态默认开(关掉则点段无任何面板反应,J67 入口不可发现)",
    );
    check(
        /show\(els\.inspector, local\.inspectorOpen\);/.test(tw) &&
            !/show\(els\.inspector, !!cur\)/.test(tw),
        "面板显隐只由开关决定,与「有没有选中段」解耦(=零布局抖动)",
    );
    check(
        /attr\(els\.inspector, "data-empty", cur \? 0 : 1\)/.test(tw),
        "无选中段 → data-empty=1 出空态句(面板宽不变)",
    );
    check(
        /\.segment-inspector\[data-empty="1"\] \.inspector-times,/.test(html) &&
            /\.segment-inspector:not\(\[data-empty="1"\]\) \.inspector-empty \{/.test(
                html,
            ),
        "空态/实态互斥的 CSS 就位",
    );
    for (const gb of [
        "wave-btn-inspector",
        "inspector-close",
        "inspector-empty",
    ]) {
        check(html.includes(`data-gb="${gb}"`), `新件锚点 ${gb} 就位`);
    }
    {
        const i = html.indexOf('data-gb="wave-btn-inspector"');
        const win = html.slice(Math.max(i - 300, 0), i + 300);
        check(/aria-pressed=/.test(win), "面板开关走 aria-pressed(不自造属性)");
        check(
            win.includes('data-t="wave.inspectorToggle"'),
            "开关文案走词条 wave.inspectorToggle",
        );
    }
    check(
        /attr\(\s*els\.inspToggle,\s*"aria-pressed"/.test(tw),
        "开关按下态每帧与本地态同源",
    );
    check(
        /local\.lastPaint\.clear\(\);[\s\S]{0,200}local\.staticDirty = true;[\s\S]{0,200}schedulePaint\(\);/.test(
            tw.slice(tw.indexOf("const setInspectorOpen")),
        ),
        "开关改左栏宽 ⇒ 清 blit 老底 + 两层标脏(旧舞台宽的位图会被横向拉伸)",
    );
    // 桥面零新增:开关不写 state、不新增桥函数(契约无 UI 偏好落点)
    check(
        !/setInspectorOpen[\s\S]{0,400}call\(/.test(tw),
        "面板开关不碰桥面(契约 §7 manifest 无 UI 偏好写面,只存内存)",
    );

    for (const [lang, dict] of [
        ["zh", T.zh],
        ["en", T.en],
        ["fr", T.fr],
    ]) {
        for (const k of ["wave.inspectorToggle", "wave.inspectorEmpty"]) {
            check(
                typeof dict[k] === "string" && dict[k].length > 0,
                `${lang} 有词条 ${k}`,
            );
        }
    }
}

// =============================================================================
log("\n=== ⑬ /code-review high 七条 finding(Wave 5 下半场)===");
{
    const tw = src("web/output/tab-wave.js");
    const html = src("web/output/index.html");

    // ---- 1. 段角标层宽必须与舞台坐标系同源(角标右漂)
    check(
        !/VZOOM_GUTTER_W\s*[=:]/.test(tw),
        "finding 1:VZOOM_GUTTER_W 已撤销(纵向缩放条移到底部条右端,舞台不再自留槽)",
    );
    check(
        /return Math\.max\(w - HEAD_W - SCALE_COL_W, 0\);/.test(tw),
        "finding 1:stageWidth() = clientW − 158 − 44(单一右让口径)",
    );
    for (const cls of [
        "wave-lane__static",
        "wave-lane__badges",
        "wave-overlay",
    ]) {
        const i = html.indexOf(`.${cls} {`);
        check(i > 0, `finding 1:${cls} 规则在`);
        const win = html.slice(i, i + 400);
        check(
            /right:\s*44px;/.test(win),
            `finding 1:.${cls} 右让 = 44(与 SCALE_COL_W / stageWidth 同一个数)`,
        );
    }
    check(
        TW.SCALE_COL_W === 44,
        "finding 1:SCALE_COL_W 与 CSS 里的 44px 逐字同值",
    );

    // ---- 2. 标尺 data-sig 必须含刻度落点(小幅平移时不得冻结)
    {
        const i = tw.indexOf("function renderRuler(");
        const win = tw.slice(i, i + 1400);
        check(
            /const sig = parts\.map\(\(p\) => `\$\{p\.label\}@\$\{p\.left\}\$\{p\.last\}`\)/.test(
                win,
            ),
            "finding 2:sig 由「标签 + 算好的 left + 末位标记」组成(与渲染输出逐字同源)",
        );
        check(
            !/const sig = ticks\.map\(\(k\) => k\.label\)\.join\("\|"\) \+ "@" \+ span/.test(
                win,
            ),
            "finding 2:老的「标签串 + span」sig 已不在(平移不足一格时会早退冻结)",
        );
        check(
            /style="left:\$\{p\.left\}%"/.test(win),
            "finding 2:写进 style 的 left 与进 sig 的 left 是同一个值",
        );
    }

    // ---- 3 + 4. 检查器数字框:零位移不发 / Enter 不发两次
    {
        const i = tw.indexOf("function wireNumInput(");
        check(i > 0, "finding 3:wireNumInput 在");
        const win = tw.slice(i, i + 2000);
        check(
            /if \(st && st\.last !== null && st\.last === q1\) \{/.test(win),
            "finding 3:解析值等于「最后渲染进框的值」即早退,一个字节都不上行",
        );
        check(
            /if \(st\) st\.last = q1;\s*\n\s*spec\.commit\(q1\);/.test(win),
            "finding 4:commit 真送出时把基准推到刚送的值 ⇒ 紧随的 blur 落进零位移早退",
        );
        // 基准的唯一写入点:render 写值(setNumBox)+ commit 送出
        const setter = tw.slice(
            tw.indexOf("function setNumBox("),
            tw.indexOf("function setNumBox(") + 600,
        );
        check(
            /if \(root\.activeElement === input\) return;/.test(setter),
            "finding 3:聚焦中的框不覆写、基准也按兵不动(基准 = 用户开编前框里的值)",
        );
        check(
            /st\.last = Math\.round\(num\(v, 0\) \* q\) \/ q;/.test(setter),
            "finding 3:基准与提交值同精度量化(dp 位),避免 +12.34 → 「12.3」被误判为改动",
        );
        check(
            (tw.match(/numBoxes\.get\(/g) || []).length >= 2 &&
                /numBoxes\.set\(input, \{ last: null \}\)/.test(tw),
            "finding 3:每个数字框各有一份基准账",
        );
        check(
            /setNumBox\(els\.inspPanInput, pan, 1\);/.test(tw) &&
                /setNumBox\(els\.inspVolInput, vol, 1\);/.test(tw),
            "finding 3:PAN/VOL 两框的渲染写值都过 setNumBox(否则基准永远是 null)",
        );
        check(
            !/if \(root\.activeElement !== els\.insp(Pan|Vol)Input\) \{/.test(
                tw,
            ),
            "finding 3:renderInspector 里裸写 input.value 的老路径已收口",
        );
    }

    // ---- 5. 播放头只抬高、不压低时间轴估计
    {
        const D = TW.FALLBACK_DURATION_S;
        eq(
            TW.durationOf({ playhead: { timeS: 0.03 } }),
            D,
            "finding 5:空态刚起播(playhead 0.03s,无段无 range)⇒ 仍是 FALLBACK,不塌成 0.03",
        );
        eq(TW.durationOf({}), D, "finding 5:全空 ⇒ FALLBACK");
        eq(
            TW.durationOf({ playhead: { timeS: 420 } }),
            420,
            "finding 5:播放头越过 FALLBACK ⇒ 照旧把估计抬上去(只抬不压)",
        );
        eq(
            TW.durationOf({
                state: { global: { range: { end_s: 120 } } },
                playhead: { timeS: 0.03 },
            }),
            120,
            "finding 5:有 range 证据时播放头不把它压低",
        );
        eq(
            TW.durationOf({
                state: { global: { range: { end_s: 120 } } },
                playhead: { timeS: 200 },
            }),
            200,
            "finding 5:播放头超出 range ⇒ 取大",
        );
        eq(
            TW.durationOf({
                segments: { channels: [{ segments: [{ t1S: 88 }] }] },
                playhead: { timeS: 1 },
            }),
            88,
            "finding 5:段表证据同理(88 不被 1s 的播放头压掉)",
        );
        // 塌轴的判据:若 durationOf 回 0.03,视口会被夹到 MIN_SPAN_S
        check(
            TW.durationOf({ playhead: { timeS: 0.03 } }) > TL.MIN_SPAN_S,
            "finding 5:空态起播的估计必须大于 MIN_SPAN_S(否则标尺/读数/滚动条塌成 1 秒)",
        );
    }

    // ---- 6. 重新识别确认框:切语言不退回 {k}/{l}
    {
        check(
            /function renderReidentifyBody\(\)/.test(tw),
            "finding 6:确认框正文有独立的补填函数",
        );
        const win = tw.slice(
            tw.indexOf("function renderReidentifyBody()"),
            tw.indexOf("function renderReidentifyBody()") + 700,
        );
        check(
            /if \(!box \|\| box\.hidden \|\| !local\.reidentifyCounts\) return;/.test(
                win,
            ),
            "finding 6:只在框可见且计数已定格时补填",
        );
        check(
            /fmtKey\("wave\.reidentifyConfirm", local\.reidentifyCounts\)/.test(
                win,
            ),
            "finding 6:补填用开框那刻定格的计数(不在 render 里重算 scope)",
        );
        // 补填必须挂在 render 里:refreshI18n = applyI18n(整串覆盖) + render(补填)
        const rIdx = tw.indexOf("    function render() {");
        const rEnd = tw.indexOf("schedulePaint();", rIdx);
        check(
            rIdx > 0 &&
                tw.slice(rIdx, rEnd).includes("renderReidentifyBody();"),
            "finding 6:render() 里补填(切语言走 applyI18n + render,补填必排在整串覆盖之后)",
        );
        check(
            /local\.reidentifyCounts = \{ k: counts\.marks, l: counts\.locked \};/.test(
                tw,
            ),
            "finding 6:开框只负责定格计数",
        );
        // data-t 保留:check-i18n 的覆盖盘点 + 未注入词典时的兜底原文
        check(
            html.includes('data-t="wave.reidentifyConfirm"'),
            "finding 6:正文节点保留 data-t(词条覆盖盘点与兜底原文都靠它)",
        );
    }

    // ---- 7. 锁角标 tooltip 随语言
    {
        check(
            /if \(local\.lastDict !== t\) \{\s*\n\s*local\.lastDict = t;\s*\n\s*local\.marksDirty = true;/.test(
                tw,
            ),
            "finding 7:词典对象一变就置 marksDirty(切语言既不动视口也不来 segments 事件)",
        );
        check(/lastDict: null,/.test(tw), "finding 7:词典账在 local 里有落点");
        // 恒等比较成立的前提:dict(lang) 每门语言返回同一个常量对象
        const { dict } = await import(u("web/shared/i18n.js"));
        check(
            dict("zh") === dict("zh") && dict("zh") !== dict("en"),
            "finding 7:i18n.dict() 按语言返回稳定的同一对象 ⇒ 恒等比较是切语言的准信号",
        );
        check(
            /const tip = esc\(t\["wave\.lockBadge"\] \|\| ""\);/.test(tw) &&
                /aria-label="\$\{tip\}" title="\$\{tip\}"/.test(tw),
            "finding 7:锁角标的 aria-label/title 仍烤自当帧词典(所以必须靠 marksDirty 重建)",
        );
    }
}

// =============================================================================
if (fail) {
    console.error(`\n${fail} 处断言失败`);
    process.exit(1);
}
log("\n全部通过");
