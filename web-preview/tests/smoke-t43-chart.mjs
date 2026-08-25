// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Tab1 分布图双视图 + 轨道配色冒烟(node,无 DOM;T43 / [J75])
// =============================================================================
// 与既有冒烟同款口径:仓内零 node_modules,故断言面是**纯函数 + mock 端到端 + 源码不变式**,
// 画布光栅与指针手势归浏览器手测(shot.mjs 截图 + 真机 preview)。
//
// 跑什么:
//   ① 轨迹图纯几何(web/shared/trajectory-chart.js):断线判据、台阶折线、降采样、
//      pan→y、跟随模式的带内不动/越带重定位、时间刻度 nice-number,以及
//      **纵向缩放**(视野夹取 1×–8× / 锚点守恒 / 复位 / 刻度自适应细分 / 读数后缀);
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
//      两条自持循环在离场时都停(smoke-tab3 ⑨ 的同款不变式);外加纵向缩放的
//      **接法**不变式(Shift+滚轮分支不脱离横向跟随、折线走 clip、复位按钮接线);
//   ⑧ 空态盖板 hidden 时真的不可见(靠本页那条全局 `[hidden]{display:none !important}`,
//      不逐类补兜底)+ 脏位只在**画得成**的那一帧兑现(不可见期不清,切回来才画);
//   ⑨ **把图真的跑起来**(手搓 canvas 桩 + `layers.tick` 单帧驱动):ui.scale 换档
//      重建后备存储、离场期欠下的重绘由 `resume()` 还清、已在前台时不无脑重画;
//   ⑩ 脏轨号(取色回落 / 系列与图例同口径)+ `destroy()` 退订与幂等;
//   ⑪ **滚轮四路映射**(滚轮=横向平移 / Ctrl=横向缩放 / Shift=纵向平移 /
//      Alt=纵向缩放):把事件真发进桩去验落点、锚点、平移比例、跟随脱离与
//      四路 preventDefault ——「哪个修饰键走哪条」是源码正则钉不住的。
//
// 关于 ⑨ 的「无 DOM」:桩不是 DOM 实现,只是把 `resizeCanvas` / `paintStatic` 认的
// 那几个字段凑齐(width/height/style/getContext + 一个记账 ctx)。layers.js 的头注
// 本就写着 `tick` 是「导出给 node 侧驱动」的单帧推进口,这里按其声明消费。
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

    // 重叠段(§2.8 的段互不重叠,但脏数据里时间会倒流)。夹取语义 =
    // 「先到的段占住那段时间,交接发生在它的末尾」;折线因此恒为时间单调递增。
    {
        const overlap = TC.runsOfSegments([
            { t0S: 0, t1S: 5, pan: -30 },
            { t0S: 3, t1S: 9, pan: 20 }, // 与前段重叠 2s
        ]);
        eq(overlap.length, 1, "重叠 ⇒ 仍是一条折线(重叠不是间隙)");
        eq(
            overlap[0],
            [
                { tS: 0, pan: -30 },
                { tS: 5, pan: -30 },
                { tS: 5, pan: 20 },
                { tS: 9, pan: 20 },
            ],
            "重叠段夹到前段末尾(交接在 5s,不是画一段从 3s 往回走的负宽台阶)",
        );
        // 完全被吞:没有自己的时间 ⇒ 整段跳过,且**不该**因此断线
        const swallowed = TC.runsOfSegments([
            { t0S: 0, t1S: 10, pan: 0 },
            { t0S: 2, t1S: 6, pan: 50 }, // 完全落在前段内
            { t0S: 10, t1S: 14, pan: -40 },
        ]);
        eq(swallowed.length, 1, "被吞的段不产生断口(它压根没占住时间)");
        eq(
            swallowed[0],
            [
                { tS: 0, pan: 0 },
                { tS: 10, pan: 0 },
                { tS: 10, pan: -40 },
                { tS: 14, pan: -40 },
            ],
            "被吞的段整段跳过,后面的段照常接上",
        );
        // 所有折线的时间**恒单调不减** —— 这条是重叠夹取要守住的总不变式
        const messy = TC.runsOfSegments([
            { t0S: 0, t1S: 8, pan: 10 },
            { t0S: 1, t1S: 3, pan: 20 },
            { t0S: 5, t1S: 12, pan: 30 },
            { t0S: 11, t1S: 11.5, pan: 40 },
            { t0S: 30, t1S: 35, pan: 50 }, // 真间隙 ⇒ 断线
        ]);
        for (const r of messy) {
            check(
                r.every((p, i) => i === 0 || p.tS >= r[i - 1].tS),
                `折线时间单调不减(实得 ${JSON.stringify(r.map((p) => p.tS))})`,
            );
        }
        eq(
            messy.length,
            2,
            "脏数据里的**真**间隙仍然断线(夹取没把断线判据吃掉)",
        );
    }

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
    // 纵向缩放(2026-08-25 用户 preview 反馈追加)。纵向视野 {lo,hi} 与 x 轴共用
    // 同一套视口几何(pan 折成轴坐标 a = 100 − pan),故这里连夹取带锚点一起验。
    const FULL = TC.PAN_VIEW_FULL;
    eq(TC.PAN_ZOOM_MAX, 8, "纵向放大上限 8×");
    eq(TC.PAN_SPAN_MIN, 25, "上限档的视野跨度 = 全域 / 8");
    check(TC.isPanViewFull(FULL), "默认视野 = 全域");
    check(Math.abs(TC.panZoomFactor(FULL) - 1) < 1e-9, "全域 = 1×");

    // 全域那组刻度**逐字不动**:细分只发生在放大之后
    eq(
        TC.panTicksIn(FULL),
        TC.PAN_TICKS.slice(),
        "全域刻度 ≡ PAN_TICKS(纵向缩放没有偷改默认档的刻度口径)",
    );

    // 缩放:放大一档跨度按 PAN_ZOOM_STEP 收窄,且锚点 pan 落在同一 y 上
    const z1 = TC.zoomPanView(FULL, 0, TC.PAN_ZOOM_STEP);
    check(
        Math.abs(z1.hi - z1.lo - TC.PAN_SPAN_FULL / TC.PAN_ZOOM_STEP) < 1e-9,
        `放大一档跨度 = 全域/${TC.PAN_ZOOM_STEP}`,
    );
    check(!TC.isPanViewFull(z1), "放大后不再是全域(reset 按钮的显隐判据)");
    const anchor = 40;
    const before = TC.panToY(anchor, 200, FULL);
    const after = TC.panToY(anchor, 200, TC.zoomPanView(FULL, anchor, 2));
    check(
        Math.abs(before - after) < 1e-6,
        `锚点 pan=${anchor} 在缩放前后落在同一 y(${before} vs ${after})`,
    );

    // clamp:放大到底停在 8×,缩小到底停在全域(两头都不越界)
    let deep = FULL;
    for (let i = 0; i < 30; i++) deep = TC.zoomPanView(deep, 0, 2);
    check(
        Math.abs(deep.hi - deep.lo - TC.PAN_SPAN_MIN) < 1e-9,
        `连续放大夹到上限 8×(跨度 ${deep.hi - deep.lo})`,
    );
    let wide = deep;
    for (let i = 0; i < 30; i++) wide = TC.zoomPanView(wide, 0, 0.5);
    eq(wide, { lo: -100, hi: 100 }, "连续缩小夹回全域,不越出角度域");
    // 视野贴边后也不越界(在 +100 一侧放大再往上推)
    const top = TC.panPanView(TC.zoomPanView(FULL, 100, 4), 999);
    check(top.hi <= 100 + 1e-9 && top.lo >= -100 - 1e-9, "平移到头贴边即停");
    eq(Math.round(top.hi), 100, "顶到 +100 就停在 +100(不会把视野推出角度域)");

    // 全域档下纵向平移是个空操作 —— 拖拽的纵向分量在没放大时不该动画面
    eq(TC.panPanView(FULL, 50), { lo: -100, hi: 100 }, "全域纵向平移 = 空操作");
    // 放大后才拖得动,且方向 = 「视野向 +100 移」
    const moved = TC.panPanView(TC.zoomPanView(FULL, 0, 4), 10);
    check(
        moved.lo > -25 - 1e-9 && moved.hi > 25 - 1e-9,
        "dPan>0 视野向 +100 移",
    );

    // 重置:一键回全域(按钮与 "0" 键走的是同一条 setPanView(PAN_VIEW_FULL))
    check(TC.isPanViewFull(TC.clampPanView(FULL)), "重置目标就是全域");
    eq(TC.clampPanView(deep).hi - TC.clampPanView(deep).lo, 25, "夹取幂等");

    // 脏输入不炸(倒挂 / NaN / 缺字段)
    eq(TC.clampPanView({ lo: 50, hi: -50 }), { lo: -50, hi: 50 }, "倒挂即交换");
    eq(TC.clampPanView({}), { lo: -100, hi: 100 }, "缺字段 → 全域");
    eq(TC.clampPanView(null), { lo: -100, hi: 100 }, "null → 全域");
    check(
        TC.clampPanView({ lo: NaN, hi: NaN }).hi === 100,
        "NaN 视野回全域(不塌成一条线)",
    );

    // 刻度自适应:档位越深步长越细,且始终落在候选步长表里
    const spans = [200, 100, 50, 25];
    const steps = spans.map((s) => {
        const v = TC.clampPanView({ lo: -s / 2, hi: s / 2 });
        const ticks = TC.panTicksIn(v);
        return ticks.length > 1 ? Math.abs(ticks[0] - ticks[1]) : 0;
    });
    check(
        steps.every((s, i) => i === 0 || s <= steps[i - 1]),
        `视野越小刻度越细(单调):${steps.join(" → ")}`,
    );
    for (const s of steps) {
        check(TC.PAN_TICK_STEPS.includes(s), `步长 ${s} 落在候选表里`);
    }
    for (const s of spans) {
        const v = TC.clampPanView({ lo: -s / 2, hi: s / 2 });
        const ticks = TC.panTicksIn(v);
        check(
            ticks.length >= TC.PAN_TICK_MIN_COUNT,
            `跨度 ${s} 的视野至少 ${TC.PAN_TICK_MIN_COUNT} 格(实得 ${ticks.length})`,
        );
        check(
            ticks.every((p) => p <= v.hi + 1e-9 && p >= v.lo - 1e-9),
            `跨度 ${s} 的刻度不越出视野`,
        );
        check(
            ticks.every((p, i) => i === 0 || p < ticks[i - 1]),
            `跨度 ${s} 的刻度从上往下降序(与 y 向下一致)`,
        );
    }

    // pan → y:两参调用(T43 首版)逐字不变;带视野时按视野线性映射
    eq(TC.panToY(50, 200), 50, "两参 panToY 与首版同值(全域)");
    eq(
        TC.panToY(50, 200, { lo: 0, hi: 100 }),
        100,
        "放大到 0..100 时 +50 居中",
    );
    eq(TC.panToY(0, 200, { lo: 0, hi: 100 }), 200, "视野下缘落在折线区底");
    check(
        TC.panToY(-50, 200, { lo: 0, hi: 100 }) > 200,
        "视野外的点算得出界外的 y(由画布 clip 裁,不夹到边缘变成假水平线)",
    );
    eq(TC.yToPan(0, 200, { lo: 0, hi: 100 }), 100, "y=0 → 视野上缘");
    eq(TC.yToPan(200, 200, { lo: 0, hi: 100 }), 0, "y=底 → 视野下缘");
    eq(TC.yToPan(9999, 200, { lo: 0, hi: 100 }), 0, "越界 y 夹到视野内");

    // 刻度数字:负号用 U+2212(与词条里的 −50 同一个字符)
    eq(TC.panTickText(0), "0", "0 不带符号");
    eq(TC.panTickText(50), "+50", "正值带 +");
    eq(TC.panTickText(-50), "−50", "负值用 U+2212 减号,不是 ASCII 连字符");

    // 读数后缀:全域不显示,放大后才多出 Y 档位(aria-live 顺带播报)
    eq(TC.panZoomLabel(FULL), "", "全域时读数不多出一截");
    eq(TC.panZoomLabel({ lo: -50, hi: 50 }), " · Y ×2.0", "2× 的读数后缀");
    eq(
        TC.panZoomLabel({ lo: -12.5, hi: 12.5 }),
        " · Y ×8.0",
        "上限档的读数后缀",
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
    const { createBridge, BRIDGE_FUNCTIONS, PENDING_FUNCS } = await import(
        u("web/shared/bridge.js")
    );
    check(
        BRIDGE_FUNCTIONS.output.includes("setMasterChartMode"),
        "setMasterChartMode 已转正进 BRIDGE_FUNCTIONS.output(契约 §7 + C++ 常量表同批)",
    );
    check(
        !PENDING_FUNCS.output.includes("setMasterChartMode"),
        "setMasterChartMode 已从待转正名表移除(本卡只删自己那一项)",
    );
    eq(
        PENDING_FUNCS.output.slice(),
        [],
        "待转正名表已清空([J81] 修宪:exportSuggestions 也转正了,契约 §1.36 + §7)",
    );
    const bridge = createBridge({ role: "output", mockBackend: s.mock });
    check(
        typeof bridge.setMasterChartMode === "function",
        "转正后桥上按 BRIDGE_FUNCTIONS 挂得到(预览页当场可往返)",
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
        "chart.panSideR",
        "chart.panSideC",
        "chart.panSideL",
        "chart.trajCanvasAria",
        "chart.zoomAria",
        "chart.backToPlayhead",
        "chart.resetPanZoom",
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
        // 05 §5 占位符判据:fr 不许照抄英文。
        // 例外:`chart.panSideC` —— 方位词是单字母缩写,英 Centre / 法 Centre 同为
        // 「C」,本来就该一样。这不是漏译,故显式豁免而不是放宽整条判据。
        if (k !== "chart.panSideC") {
            check(T.fr[k] !== T.en[k], `fr.${k} 不是英文照抄`);
        }
    }
    eq(
        [T.en["chart.panSideR"], T.en["chart.panSideL"]],
        ["R", "L"],
        "en 左右方位词(照抄豁免只给 C 那一条,左右仍逐字对拍)",
    );
    eq(
        [T.fr["chart.panSideR"], T.fr["chart.panSideL"]],
        ["D", "G"],
        "fr 左右方位词 = Droite / Gauche(确实译过,不是照抄英文)",
    );
    // 方位词**三条各自成 key**,不再合成一串按语序拆 —— 合成串的话,U17 审校
    // 调一下词序就会把左右标反,而那是一眼看不出来的错(图照画,只是左右颠倒)。
    check(
        !("chart.trajAxisSides" in T.zh),
        "合成串词条已删(按语序取词的隐患随它一起没了)",
    );
    check(
        !("chart.trajAxisY" in T.zh),
        "写死五格的旧词条已删(留着就会有人以为刻度还是固定五格)",
    );
    // 消费侧必须**按 key 取**:出现 split(" · ") 就是又退回按位置认了
    {
        const tm = src("web/output/tab-master.js");
        check(
            /t\["chart\.panSide" \+ side\]/.test(tm),
            "方位词按 key 取(`chart.panSide` + R/C/L)",
        );
        check(!/chart\.trajAxisSides/.test(tm), "消费侧不再引用合成串词条");
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
    // (脏位**何时清**是另一条不变式,见 ⑧:不可见那一帧不许清)
    check(
        /chartRepaintNow\(local\.trajDirty, isPanelActive\(\)\)\) \{\s*local\.trajDirty = false;\s*local\.traj\.invalidate\(\);/.test(
            src("web/output/tab-master.js"),
        ),
        "静态层重绘走脏位(播放中不逐帧重画 15 条折线)",
    );
}

{
    // 纵向缩放的源码不变式(光栅与手势归浏览器手测,这里钉住接法)
    const tj = src("web/shared/trajectory-chart.js");
    const tm = src("web/output/tab-master.js");
    const html = src("web/output/index.html");

    check(
        /Math\.abs\(e\.deltaX\) > Math\.abs\(e\.deltaY\) \? e\.deltaX : e\.deltaY/.test(
            tj,
        ),
        "滚轮增量取主轴(触控板横向 deltaX / Shift+滚轮被改写成横向滚动,同一路认)",
    );
    check(
        /e\.deltaMode === 1/.test(tj) && /e\.deltaMode === 2/.test(tj),
        "deltaMode 行/页折成 px(不折的话跨浏览器跨度差两个数量级)",
    );
    // 纵向两路**不**脱离横向跟随:altKey / shiftKey 分支里不许出现 breakFollow
    for (const [name, key] of [
        ["Alt(纵向缩放)", "altKey"],
        ["Shift(纵向平移)", "shiftKey"],
    ]) {
        const br = new RegExp(
            `if \\(e\\.${key}\\) \\{([\\s\\S]*?)\\n {16}\\}`,
        ).exec(tj);
        check(!!br, `找得到 ${name} 分支(下一条断言的前提)`);
        if (br) {
            check(
                !/breakFollow/.test(br[1]),
                `${name} 不脱离横向跟随(两条轴的状态互不牵连)`,
            );
        }
    }
    check(
        /ctx\.save\(\);\s*ctx\.beginPath\(\);\s*ctx\.rect\(0, PAD_TOP, local\.stageW, H\);\s*ctx\.clip\(\);/.test(
            tj,
        ),
        "折线画在 clip 里(纵向放大后出界的线不许糊到时间标签行上)",
    );
    check(
        /ctx\.restore\(\);/.test(tj),
        "clip 用完即还原(不还原会把后续绘制一起裁掉)",
    );
    check(
        /o\.resetPanBtn\.addEventListener\("click", \(\) =>\s*setPanView\(PAN_VIEW_FULL\),?\s*\);/.test(
            tj,
        ),
        "「纵向复位」按钮回全域",
    );
    check(
        /if \(o\.resetPanBtn\) o\.resetPanBtn\.hidden = isPanViewFull\(v\);/.test(
            tj,
        ),
        "复位按钮只在纵向非全域时露出(不挤占默认态的画面)",
    );
    // setPanView 是纵向态的唯一入口,且它不碰 following
    const setPan = /function setPanView\(next\) \{([\s\S]*?)\n {4}\}/.exec(tj);
    check(!!setPan, "找得到 setPanView(纵向态的唯一写入口)");
    if (setPan) {
        check(
            !/following|breakFollow/.test(setPan[1]),
            "setPanView 不改跟随态(横向跟随不受纵向缩放影响)",
        );
    }
    check(
        /ArrowUp" \|\| e\.key === "ArrowDown"/.test(tj) &&
            /e\.shiftKey/.test(tj) &&
            /e\.key === "0"/.test(tj),
        "键盘等价物齐备(↑↓ 纵向平移 / Shift+↑↓ 纵向缩放 / 0 复位)",
    );
    check(
        /resetPanBtn: el\.trajReset/.test(tm) && /onPanAxis:/.test(tm),
        "Tab1 把复位按钮与刻度回调接上了",
    );
    check(
        /data-gb="master-trajchart-reset"/.test(html) &&
            /data-t="chart\.resetPanZoom"/.test(html),
        "HTML 有复位按钮且文案走词条",
    );
    check(
        !/data-t-split="chart\.trajAxisY"/.test(html),
        "y 刻度列不再是写死的五格拆格(改由 onPanAxis 逐格建)",
    );
}

// =============================================================================
log("=== ⑧ 空态盖板与脏位兑现(pr-agent 两条 Possible issue)===");

{
    const html = src("web/output/index.html");
    const tm = src("web/output/tab-master.js");

    // ---- ① 空态盖板:hidden 时必须真的不可见。
    // 本页第 223 行有一条**全局** `[hidden]{display:none !important}`(作者层 +
    // !important),压得过任何类选择器上的 display —— 盖板从来没真的盖过图。
    // 上一轮我在这里加过一条逐类兜底,理由写的是「靠 UA 表恰好带 !important」,
    // 那个理由是错的(claude-review 指出);规则已删,改钉这条真正在管事的全局兜底。
    check(
        /\[hidden\]\s*\{\s*display:\s*none\s*!important/.test(html),
        "本页有全局 [hidden] 兜底(作者层 + !important:一切默认隐藏节点的总闸)",
    );
    check(
        !/\.traj-stage__empty\[hidden\]/.test(html),
        "空态盖板**没有**逐类兜底(全局那条已覆盖,逐类补是冗余)",
    );
    // 反向:盖板确实设了非 none 的 display —— 全局兜底不在场时它就会盖住图,
    // 这条钉住「为什么需要那条全局规则」的理由本身。
    check(
        /\.traj-stage__empty\s*\{[^}]*display:\s*flex/.test(html),
        "盖板本体设了 display:flex(全局 [hidden] 兜底存在的理由)",
    );
    // 盖板归 tab-master 按「有没有段」开关,判据不许漂
    check(
        /el\.trajEmpty\.hidden = local\.trajSeries\.length > 0/.test(tm),
        "有分段就藏盖板、没分段才露(开关判据只有一处)",
    );

    // ---- ② 脏位只在**画得成**的那一帧兑现(真值表;不是源码正则)。
    eq(TM.chartRepaintNow(true, true), true, "脏 + 可见 ⇒ 这一帧重绘");
    eq(
        TM.chartRepaintNow(true, false),
        false,
        "脏 + **不可见** ⇒ 不兑现(脏位留着,切回来那一帧才画)",
    );
    eq(TM.chartRepaintNow(false, true), false, "不脏 + 可见 ⇒ 不必重绘");
    eq(TM.chartRepaintNow(false, false), false, "不脏 + 不可见 ⇒ 什么都不做");
    // 脏位的语义就是「留到画得成为止」:不可见期间连来几次脏,兑现的仍是同一次
    let dirty = false;
    const step = (setDirty, visible) => {
        dirty = dirty || setDirty;
        if (TM.chartRepaintNow(dirty, visible)) dirty = false;
        return dirty;
    };
    check(step(true, false), "不可见期置脏 ⇒ 脏位**留着**(不被这一帧吃掉)");
    check(step(false, false), "不可见期再跑一帧 ⇒ 仍然留着");
    check(step(true, false), "不可见期又来一次段表事件 ⇒ 还是留着");
    check(!step(false, true), "切回可见的第一帧 ⇒ 兑现并清掉(图不再是过期的)");
    check(!step(false, true), "兑现之后不再重复重绘(幂等)");

    // 接法:renderChart 里的清标志确实走 chartRepaintNow,且判据是「面板可见」。
    // 直接 `if (local.trajDirty)` 就是 pr-agent 指的那个形态,回退了当场红。
    check(
        /if \(chartRepaintNow\(local\.trajDirty, isPanelActive\(\)\)\) \{\s*local\.trajDirty = false;\s*local\.traj\.invalidate\(\);/.test(
            tm,
        ),
        "renderChart 的脏位兑现走 chartRepaintNow(不是无条件清)",
    );
    // 这条路走得到的证据:onSegments 里那个 setTimeout 调的是**本模块的** render,
    // 绕开了 app.js「只投影当前激活 tab」的 switch(否则本 bug 根本够不着)。
    check(
        /setTimeout\(render, 1650\)/.test(tm) &&
            /local\.trajDirty = true;/.test(tm),
        "onSegments 置脏 + 自排定时器 render(不可见时跑到清标志那一行的那条路)",
    );
}

// =============================================================================
log("=== ⑨ 后备存储与离场期欠账(claude-review;stub canvas 行为断言)===");

// 本节把轨迹图**真的跑起来** —— 前面各节是纯函数与源码不变式,而「换缩放档要不要
// 重建后备存储」「离场期的重绘诉求丢没丢」是**时序**问题,正则钉不住。
// 仓内零 node_modules,故手搓两件桩:
//   • **假 rAF**:让分层骨架跑它自己那条真循环(`start` 排帧 / `stop` 撤帧)。
//     这一件是必需的 —— `suspend()` 丢帧丢的正是「已排队但还没跑的那一帧」,
//     不把 requestAnimationFrame / cancelAnimationFrame 的语义摆出来就复现不了;
//   • **canvas 桩**:hidpi.js 的 `resizeCanvas` 只认 width/height/style/getContext,
//     `observeResolution` 在无 matchMedia 的环境自动 no-op;2D ctx 只把 paintStatic
//     用到的方法记个账,画得对不对归浏览器截图管(shot.mjs)。
const rafQ = new Map();
let rafSeq = 0;
globalThis.requestAnimationFrame = (cb) => {
    const id = ++rafSeq;
    rafQ.set(id, cb);
    return id;
};
globalThis.cancelAnimationFrame = (id) => rafQ.delete(id);
/** 跑完当前排队的帧;回调里再排的帧留到下一轮(与浏览器同语义)。 */
function pump(nowMs) {
    const due = [...rafQ.values()];
    rafQ.clear();
    for (const cb of due) cb(nowMs);
}

function makeCanvasStub(cssW = 600, cssH = 200) {
    const calls = {
        setTransform: [],
        clip: 0,
        clearRect: 0,
        stroke: 0,
        strokeStyle: [], // 每次赋值都记一笔(取色断言看它)
    };
    const ctx = new Proxy(
        {
            setTransform: (k) => calls.setTransform.push(k),
            clip: () => calls.clip++,
            clearRect: () => calls.clearRect++,
            stroke: () => calls.stroke++,
        },
        {
            get: (t, p) =>
                p in t ? t[p] : typeof p === "string" ? () => {} : undefined,
            set: (t, p, v) => {
                if (p === "strokeStyle") calls.strokeStyle.push(v);
                return true;
            },
        },
    );
    // 监听器登记表 —— 滚轮四路映射要**真的把事件发进去**才验得到:
    // 「哪个修饰键走哪条」是源码正则钉不住的。
    const handlers = new Map();
    const canvas = {
        width: 0,
        height: 0,
        style: {},
        parentElement: { clientWidth: cssW, clientHeight: cssH },
        getContext: () => ctx,
        addEventListener: (type, fn) => {
            if (!handlers.has(type)) handlers.set(type, []);
            handlers.get(type).push(fn);
        },
        getBoundingClientRect: () => ({
            left: 0,
            top: 0,
            width: cssW,
            height: cssH,
        }),
    };
    /** 造一个滚轮事件发进去;返回 preventDefault 有没有被调。 */
    const wheel = (init) => {
        let prevented = false;
        const ev = {
            deltaX: 0,
            deltaY: 0,
            deltaMode: 0,
            ctrlKey: false,
            shiftKey: false,
            altKey: false,
            clientX: cssW / 2,
            clientY: cssH / 2,
            ...init,
            preventDefault: () => {
                prevented = true;
            },
        };
        for (const fn of handlers.get("wheel") || []) fn(ev);
        return prevented;
    };
    return { canvas, calls, handlers, wheel };
}

{
    const { canvas, calls } = makeCanvasStub();
    let visible = true;
    let uiScale = 1;
    const chart = TC.createTrajectoryChart({
        canvas,
        getSeries: () => [],
        getDurationS: () => 300,
        getUiScale: () => uiScale,
        isVisible: () => visible,
    });
    let t = 0;
    /** 跑掉已排队的帧(没排队就什么都不跑);返回本轮之后的后备存储宽。 */
    const frame = () => {
        pump((t += 16));
        return canvas.width;
    };

    // 首帧:invalidate 标脏 → 推一帧 → 后备存储按 k = uiScale × dpr(node 无 dpr ⇒ 1)建起来
    chart.invalidate();
    eq(frame(), 600, "首帧后备存储 = 600 CSS px × k(1.0 档)");
    check(
        calls.clip > 0,
        "折线画在 clip 里(纵向缩放那一节的源码不变式,这里实跑到)",
    );

    // ---- ① ui.scale 换档 ⇒ 后备存储必须重建(claude-review 第 1 条)
    // 真机上 CSS zoom 换档**不动** dpr、也不动父盒 CSS px 尺寸 —— observeResolution
    // 与 ResizeObserver 都不会响,没有任何既有信号能把这一帧敲出来。
    uiScale = 1.5;
    eq(frame(), 600, "只换 uiScale 而不标脏 ⇒ 这一帧什么也没重建(bug 的原貌)");
    chart.invalidate(); // ← Tab1 的 lastUiScale 账变化时会调它
    eq(frame(), 900, "标脏后按新档重建后备存储(600 × 1.5)");
    eq(
        calls.setTransform[calls.setTransform.length - 1],
        1.5,
        "ctx 变换按新 k 重设(05 §6.1 k = uiScale × dpr)",
    );
    uiScale = 0.5;
    chart.invalidate();
    eq(frame(), 300, "换到更小的档同样重建(不是只涨不落)");

    // ---- ② 离场期欠账:重绘诉求不许丢(claude-review 第 2 条)。三个丢法逐个验。
    // 丢法 ①:invalidate() 不可见时早退 —— 连脏都没标上。
    uiScale = 2;
    visible = false;
    chart.invalidate();
    eq(frame(), 300, "不可见期推帧:画不成,后备存储停在旧档");
    visible = true;
    chart.resume(); // ← renderChart 每帧都调它;欠账在这里还
    eq(
        frame(),
        1200,
        "回到可见后补画,后备存储按 2.0 档重建(invalidate 那笔诉求没丢)",
    );

    // 丢法 ②:脏位**被那一帧吃掉**。setDuration 不看可见性,直接 invalidateStatic ——
    // 不可见时 tick 先清 staticDirty 再调 drawStatic,画不成,那一笔就没了。
    // 这条路 invalidate() 根本没参与,故只有 drawStatic 里的记账救得回来。
    // (真机上够得着:Tab1 不在前台时 onSegments 的 setTimeout(render) 仍会走到
    //  renderChart 里的 local.traj.setDuration。)
    uiScale = 3;
    visible = false;
    chart.setDuration(600); // ← 标脏,但没经 invalidate()
    eq(frame(), 1200, "不可见期这一帧把脏位吃掉了(画不成,尺寸不动)");
    visible = true;
    chart.resume();
    eq(frame(), 1800, "被吃掉的那一笔由欠账补回(600 × 3.0)");

    // 上升沿只认**边沿**:已经可见的后续帧不许再无脑重绘(脏位纪律,05 §6.1)
    const before = calls.clearRect;
    chart.resume();
    frame();
    chart.resume();
    frame();
    eq(calls.clearRect, before, "已在前台时 resume() 一步不走(不是每帧重画)");

    // 丢法 ③:suspend() 撤掉**已排队但还没跑**的那一帧,staticDirty 就此没人来跑。
    // 这条与前两条都不同:脏位还立着,只是循环停了、再没人 start() —— 故意不调
    // invalidate()(调了就成了丢法 ① 那条路,验不到本条)。
    uiScale = 4;
    chart.setDuration(900); // 可见时标脏 ⇒ 排了一帧
    chart.suspend(); // ← 帧被撤掉;切走(mode ≠ trajectory 时 renderChart 就这么做)
    visible = false;
    eq(frame(), 1800, "帧已被 suspend 撤掉,这一轮没有任何帧可跑");
    visible = true;
    chart.resume();
    eq(frame(), 2400, "suspend 撤掉的那一帧由欠账补回(600 × 4.0)");
}

{
    // Tab1 侧的 ui.scale 档位账 —— **必须与 Tab3 同一机制**(claude-review 点名:
    // 「别发明新路」)。两处各留一份账,靠下面这对断言钉住它们不漂。
    const tm = src("web/output/tab-master.js");
    const tw = src("web/output/tab-wave.js");
    const ACCOUNT =
        /if \(local\.lastUiScale !== uiScale\) \{\s*local\.lastUiScale = uiScale;/;
    check(ACCOUNT.test(tw), "Tab3 有 lastUiScale 账(本卡照抄的那份真源)");
    check(
        ACCOUNT.test(tm),
        "Tab1 用同一形制的 lastUiScale 账(不是另发明一条路)",
    );
    check(
        /local\.lastUiScale = uiScale;\s*local\.trajDirty = true;/.test(tm),
        "档位变化 ⇒ 轨迹图标脏(脏位一置,重绘就走既有那条路)",
    );
    check(
        /lastUiScale: NaN/.test(tm) && /lastUiScale: NaN/.test(tw),
        "两处初值都是 NaN(首帧必然不等 ⇒ 开机第一次一定重建一遍)",
    );
    // 为什么非记这一笔不可:k 的两个因子各有各的信号,**只有 uiScale 这条没人管**。
    const tj = src("web/shared/trajectory-chart.js");
    check(
        /observeResolution\(\(\) => layers\.invalidateStatic\(\)\)/.test(tj),
        "dpr 那条因子有 observeResolution 兜着(换屏/改系统缩放会响)",
    );
    check(
        /backingScale\(num\(getUiScale\(\), 1\), dpr\(\)\)/.test(tj),
        "k = uiScale × dpr 只在 paintStatic 里算 ⇒ 不重绘就一直用旧 k(bug 的根)",
    );
    // uiScale 那条因子:CSS zoom 换档既不动 dpr 也不动父盒 CSS px 尺寸,
    // 两个 observer 都不响 —— 账不记就真没人来敲门。
    check(
        /getUiScale/.test(tj) &&
            !/getUiScale[\s\S]{0,200}addEventListener/.test(tj),
        "模块自身不监听 uiScale(它只在绘制时读,信号归调用方给)",
    );
}

// =============================================================================
log("=== ⑩ 脏轨号与拆除口(claude-review 第四轮四条)===");

{
    // ---- ① 取色:轨号越界也必须拿到**合法**色串。
    // canvas 对非法 strokeStyle **静默忽略赋值** —— 赋了个 `rgba(undefined, .95)`
    // 不会抛、不会警告,只是这一轨沿用上一轨的颜色。故这里断言「每次赋的都是
    // 合法色串」,而不是断言「没抛」。
    const { canvas, calls } = makeCanvasStub();
    const RUN = [
        [
            { tS: 0, pan: 0 },
            { tS: 10, pan: 0 },
        ],
    ];
    // ch=0 / 负数 / 超界:三种脏轨号都直接喂给绘制路径
    const series = [
        { ch: 0, stereo: false, runs: RUN },
        { ch: -3, stereo: false, runs: RUN },
        { ch: 16, stereo: false, runs: RUN },
        { ch: 7, stereo: false, runs: RUN },
    ];
    const chart = TC.createTrajectoryChart({
        canvas,
        getSeries: () => series,
        getDurationS: () => 300,
        isVisible: () => true,
    });
    chart.invalidate();
    pump(16);
    // 网格线也赋 strokeStyle,且画在折线**之前** —— 只看末尾这四笔(15 条折线的顺序
    // 就是 series 的顺序,无高亮时不重排)。
    const lines = calls.strokeStyle.slice(-series.length);
    check(
        calls.strokeStyle.length > series.length,
        "网格线也赋过 strokeStyle(确认取的是末尾那几笔折线色)",
    );
    for (const v of lines) {
        check(
            /^rgba\(\s*\d{1,3},\s*\d{1,3},\s*\d{1,3},\s*[\d.]+\)$/.test(v),
            `色串合法(实得 ${JSON.stringify(v)})—— 非法串会被 canvas 静默吃掉`,
        );
    }
    // 越界轨号的落点必须与 trackIndex 的口径逐字一致(不是「随便给个色就行」)
    const expect = (ch) =>
        TCOL.rgbaOf(TCOL.FALLBACK_TRACK_COLORS[TCOL.trackIndex(ch) - 1], 0.95);
    eq(lines[0], expect(0), "ch=0 回落到轨 1 的色(同 trackIndex)");
    eq(lines[1], expect(-3), "ch=−3 同样回落到轨 1");
    eq(lines[2], expect(16), "ch=16 按 15 取模回轨 1");
    eq(lines[3], expect(7), "合法轨号原样取第 7 色");
    // 反向:四条线里没有两条撞成同一串**因为取不到色而沿用上一笔**的情况 ——
    // 合法轨 7 与回落轨 1 必须不同色(bug 复现时它们会一模一样)。
    check(lines[3] !== lines[2], "合法轨与回落轨颜色不同(没有沿用上一笔)");
    chart.destroy();
}

{
    // ---- ② 系列过滤与图例**同口径**:图上有线 ⇔ 图例有行。
    // 这是本卡自己立的不变式(「不会出现图例里有它、图上找不到它」),脏轨号正是
    // 它最容易破的地方 —— 两处各写一份闸,靠本断言钉住它们同判据。
    const run = [{ t0S: 0, t1S: 5, pan: 10 }];
    const dirty = {
        channels: [
            { ch: 0, segments: run }, // 越下界
            { ch: 1, segments: run }, // 合法
            { ch: 16, segments: run }, // 越上界
            { ch: 99, segments: run },
            { ch: -2, segments: run },
            { ch: NaN, segments: run },
            { ch: 1.5, segments: run }, // 非整数但在界内
            { ch: 15, segments: run }, // 边界合法
        ],
    };
    const chans = MD.FIFTEEN_TRACKS.snapshot.channels;
    const series = TM.trajectorySeries(dirty, chans);
    eq(
        series.map((s) => s.ch),
        [1, 1.5, 15],
        "越界轨号(0 / 16 / 99 / −2 / NaN)一律滤掉,只留 1..15",
    );
    // 与图例逐字同集合 —— 两处口径若漂,这一条当场红
    const rows = TM.legendRows("trajectory", [], series, chans);
    eq(
        rows.map((r) => r.ch),
        series.map((s) => s.ch),
        "轨迹档图例行集 ≡ 折线轨集(图上有线 ⇔ 图例有行)",
    );
    // 边界逐个点名(1 与 15 进、0 与 16 出)
    const chOf = (ch) =>
        TM.trajectorySeries({ channels: [{ ch, segments: run }] }, chans)
            .length;
    eq(chOf(1), 1, "ch=1 在界内");
    eq(chOf(15), 1, "ch=15 在界内");
    eq(chOf(0), 0, "ch=0 出界");
    eq(chOf(16), 0, "ch=16 出界");
}

{
    // ---- ③ parity 机检覆盖 PENDING_FUNCS(禁止复活名单不许被待转正表绕过)。
    const parity = src("scripts/check-bridge-parity.mjs");
    check(
        /extractJsNameTable\(src, "PENDING_FUNCS"\)/.test(parity),
        "parity 脚本会抽 PENDING_FUNCS 表",
    );
    check(
        /checkForbiddenIn\([\s\S]{0,200}bridgeNames\.pending\[side\]/.test(
            parity,
        ),
        "抽出来的待转正名字确实喂进了禁止复活名单机检",
    );
    // 反向:它**不该**进与契约的集合比对 —— 进了必然红,那张表就失去了存在的意义
    check(
        !/compareSets\([\s\S]{0,200}bridgeNames\.pending/.test(parity),
        "待转正表**不**进 compareSets(契约还没改,比了必红)",
    );
    // 缺表要按空表处理:老版本 bridge.js 没有这个常量,不该因此红
    check(
        /extractJsNameTable\(src, "PENDING_FUNCS"\) \|\| \{/.test(parity),
        "缺 PENDING_FUNCS 时按空表处理(向后兼容,不误红)",
    );
    // bridge.js 侧:名字仍在,且头注仍指着转正流程
    check(
        /export const PENDING_FUNCS = \{/.test(src("web/shared/bridge.js")),
        "bridge.js 仍导出 PENDING_FUNCS(转正时才删名字)",
    );
}

{
    // ---- ④ destroy():退掉活得比 canvas 长的三处订阅,且**幂等**。
    // 本卡单实例用不到,导出是为 T46 Monitor(窗口开合里反复建销毁)。
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
        const { canvas } = makeCanvasStub();
        const chart = TC.createTrajectoryChart({
            canvas,
            getSeries: () => [],
            getDurationS: () => 300,
            isVisible: () => true,
        });
        eq(
            winCalls.add,
            ["pointerup"],
            "构造时在 window 上挂了 pointerup 兜底",
        );
        eq(roCalls.observe, 1, "构造时观察了父盒尺寸");
        eq(winCalls.remove, [], "还没拆,退订记录为空");

        check(typeof chart.destroy === "function", "导出了 destroy()");
        chart.destroy();
        eq(winCalls.remove, ["pointerup"], "destroy 退掉 window 上那条兜底");
        eq(roCalls.disconnect, 1, "destroy 断开 ResizeObserver");

        // 幂等:再调两次,不该抛、也不该重复退订
        chart.destroy();
        chart.destroy();
        eq(winCalls.remove, ["pointerup"], "重复 destroy 不重复退订(幂等)");
        eq(roCalls.disconnect, 1, "ResizeObserver 也只断开一次");

        // 退订函数抛错不该拦住后面几条(拆到一半更难查)
        const boom = TC.createTrajectoryChart({
            canvas: makeCanvasStub().canvas,
            getSeries: () => [],
            getDurationS: () => 300,
            isVisible: () => true,
        });
        globalThis.window.removeEventListener = () => {
            throw new Error("boom");
        };
        let threw = false;
        try {
            boom.destroy();
        } catch {
            threw = true;
        }
        check(!threw, "某条退订抛错时 destroy 不外泄异常");
        eq(roCalls.disconnect, 2, "抛错那条之后的退订照样执行完");
    } finally {
        if (savedWin === undefined) delete globalThis.window;
        else globalThis.window = savedWin;
        if (savedRO === undefined) delete globalThis.ResizeObserver;
        else globalThis.ResizeObserver = savedRO;
    }
}

// =============================================================================
log("=== ⑪ 滚轮四路映射(2026-08-25 用户 preview 后定稿)===");

// 滚轮  = 横向平移      Ctrl+滚轮  = 横向缩放(锚 x)
// Shift = 纵向平移      Alt+滚轮   = 纵向缩放(锚 y)
// 四路都**真的把事件发进去**验落点 —— 「哪个修饰键走哪条」正则钉不住。
{
    const { canvas, wheel } = makeCanvasStub(600, 200);
    const chart = TC.createTrajectoryChart({
        canvas,
        getSeries: () => [],
        getDurationS: () => 300,
        isVisible: () => true,
    });
    const vp = () => chart.viewport();
    const pv = () => chart.panView();
    /** 每条断言前把两轴都摆回已知起点(横向留出左右余量,纵向回全域)。 */
    const reset = () => {
        chart.resetPanView();
        chart.timeline.set({ startS: 100, endS: 200 });
    };

    // ---- ① 裸滚轮 = 横向平移
    reset();
    const t0 = vp().startS;
    wheel({ deltaY: 100 });
    check(vp().startS > t0, "裸滚轮向下 ⇒ 视口向后(时间变大)");
    eq(Math.round(spanOfVp(vp())), 100, "裸滚轮**不改跨度**(是平移不是缩放)");
    const t1 = vp().startS;
    wheel({ deltaY: -100 });
    check(vp().startS < t1, "裸滚轮向上 ⇒ 视口向前");
    // 触控板横向 deltaX 走同一路(与无修饰滚轮一致)
    reset();
    const t2 = vp().startS;
    wheel({ deltaX: 100 });
    check(vp().startS > t2, "触控板横向 deltaX ⇒ 同样横向平移");
    eq(Math.round(spanOfVp(vp())), 100, "deltaX 也不改跨度");
    // 平移的**量**按 px→秒 实比例(100px / 6px每秒 ≈ 16.7s)
    reset();
    wheel({ deltaY: 60 });
    eq(
        Math.round(vp().startS - 100),
        10,
        "60px / (600px÷100s) = 10s(按实比例平移,不是固定档位)",
    );
    // 裸滚轮**脱离跟随**(动的是横向视口)
    check(!chart.following(), "裸滚轮平移 ⇒ 脱离跟随");

    // ---- ② Ctrl+滚轮 = 横向缩放
    reset();
    const span0 = spanOfVp(vp());
    wheel({ deltaY: -100, ctrlKey: true });
    check(spanOfVp(vp()) < span0, "Ctrl+滚轮向上 ⇒ 横向放大(跨度变小)");
    wheel({ deltaY: 100, ctrlKey: true });
    check(
        Math.abs(spanOfVp(vp()) - span0) < 1e-6,
        "Ctrl+滚轮向下一格回到原跨度(同一档位倍率)",
    );
    // 锚点跟光标 x:光标压在视口左缘时,左缘时刻缩放前后不动
    reset();
    const leftBefore = vp().startS;
    wheel({ deltaY: -100, ctrlKey: true, clientX: 0 });
    check(
        Math.abs(vp().startS - leftBefore) < 1e-6,
        "锚点跟光标 x(压左缘缩放,左缘时刻不动)",
    );
    check(!chart.following(), "Ctrl+滚轮缩放 ⇒ 脱离跟随");

    // ---- ③ Shift+滚轮 = 纵向平移
    reset();
    chart.timeline.set({ startS: 100, endS: 200 });
    // 先纵向放大,否则全域档下平移被夹取成空操作(那本身也是对的,见 ① 节)
    wheel({ deltaY: -100, altKey: true });
    wheel({ deltaY: -100, altKey: true });
    const pvBefore = { ...pv() };
    wheel({ deltaY: 100, shiftKey: true });
    check(pv().hi < pvBefore.hi, "Shift+滚轮向下 ⇒ 视野向 −100 走");
    eq(
        Math.round((pv().hi - pv().lo) * 1e6) / 1e6,
        Math.round((pvBefore.hi - pvBefore.lo) * 1e6) / 1e6,
        "Shift+滚轮**不改跨度**(是平移不是缩放)",
    );
    wheel({ deltaY: -200, shiftKey: true });
    check(pv().hi > pvBefore.hi, "Shift+滚轮向上 ⇒ 视野向 +100 走");
    // Chromium 系把 Shift+滚轮改写成横向滚动(值落 deltaX)—— 同样要认
    const pvX = { ...pv() };
    wheel({ deltaX: 100, deltaY: 0, shiftKey: true });
    check(
        pv().hi < pvX.hi,
        "Shift+滚轮值落在 deltaX 上时同样纵向平移(Chromium 改写)",
    );
    // 全域档下是空操作(夹取自然兜住)
    chart.resetPanView();
    const full = { ...pv() };
    wheel({ deltaY: 100, shiftKey: true });
    eq(pv(), full, "全域档下 Shift+滚轮 = 空操作(没放大就没得平移)");

    // ---- ④ Alt+滚轮 = 纵向缩放
    chart.resetPanView();
    const panSpan0 = pv().hi - pv().lo;
    wheel({ deltaY: -100, altKey: true });
    check(pv().hi - pv().lo < panSpan0, "Alt+滚轮向上 ⇒ 纵向放大");
    wheel({ deltaY: 100, altKey: true });
    check(
        Math.abs(pv().hi - pv().lo - panSpan0) < 1e-6,
        "Alt+滚轮向下一格回到全域",
    );
    // 锚点跟光标 y:压在折线区顶时,视野上缘缩放前后不动
    chart.resetPanView();
    const hiBefore = pv().hi;
    wheel({ deltaY: -100, altKey: true, clientY: 6 }); // 6 = PAD_TOP,折线区顶
    check(
        Math.abs(pv().hi - hiBefore) < 1e-6,
        "锚点跟光标 y(压折线区顶缩放,视野上缘不动)",
    );

    // ---- preventDefault:四路一律拦
    for (const [name, init] of [
        ["裸滚轮(否则连带滚动祖先容器)", { deltaY: 100 }],
        ["Ctrl+滚轮(否则触发浏览器页面缩放)", { deltaY: 100, ctrlKey: true }],
        ["Shift+滚轮", { deltaY: 100, shiftKey: true }],
        [
            "Alt+滚轮(部分平台有历史前进/后退默认)",
            { deltaY: 100, altKey: true },
        ],
    ]) {
        check(wheel(init) === true, `${name} 调了 preventDefault`);
    }
    // 监听器必须是 { passive: false },否则 preventDefault 无效
    check(
        /"wheel",[\s\S]*?\{ passive: false \}/.test(
            src("web/shared/trajectory-chart.js"),
        ),
        "wheel 监听器登记为 passive:false(否则上面四条 preventDefault 全是白调)",
    );

    chart.destroy();
}

{
    // ---- 跟随:横向两路脱离、纵向两路**不**脱离。
    // 各用一个**全新实例**(跟随默认开),否则拿一个已经脱离的实例验「不脱离」
    // 就是 false === false,什么也没证明。
    const fresh = () => {
        const { canvas, wheel } = makeCanvasStub(600, 200);
        const chart = TC.createTrajectoryChart({
            canvas,
            getSeries: () => [],
            getDurationS: () => 300,
            isVisible: () => true,
        });
        chart.timeline.set({ startS: 100, endS: 200 });
        return { chart, wheel };
    };
    for (const [name, init, stillFollowing] of [
        ["裸滚轮(横向平移)", { deltaY: 100 }, false],
        ["Ctrl+滚轮(横向缩放)", { deltaY: -100, ctrlKey: true }, false],
        ["Shift+滚轮(纵向平移)", { deltaY: 100, shiftKey: true }, true],
        ["Alt+滚轮(纵向缩放)", { deltaY: -100, altKey: true }, true],
    ]) {
        const { chart, wheel } = fresh();
        check(chart.following(), `${name}:起点跟随开着(前提)`);
        wheel(init);
        eq(
            chart.following(),
            stillFollowing,
            `${name} ⇒ 跟随${stillFollowing ? "**不**脱离" : "脱离"}`,
        );
        chart.destroy();
    }
}

function spanOfVp(v) {
    return v.endS - v.startS;
}

// =============================================================================
log(fail === 0 ? "\n=== 结果:全部通过 ===" : `\n=== 结果:${fail} 项失败 ===`);
process.exit(fail === 0 ? 0 : 1);
