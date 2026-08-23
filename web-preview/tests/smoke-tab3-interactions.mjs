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
    // 区间级失效(pr-agent):§2.7 是 2Hz 增量事件,整轨清会把 8 块全丢 ⇒
    // 采集中反复整轨重取。只该丢与 addedRanges 相交的块。
    {
        const src = WF.createWaveformSource({
            request: async () => ({
                minDb: [0],
                maxDb: [0],
                covered: [1],
                vad: [0],
                stale: [0],
                passId: [1],
                valleys: [],
            }),
        });
        await src.getTile(1, 0, 10, 1); // 键 0:10000:1
        await src.getTile(1, 60, 70, 1); // 键 60000:70000:1
        src.invalidate(1, [{ startS: 2, endS: 3 }]); // 只碰前一块
        check(
            src.peek(1, 0, 10, 1) === null && src.peek(1, 60, 70, 1) !== null,
            "区间级失效只丢相交块,不相交的块留着",
        );
        src.invalidate(1); // 缺 ranges = 整轨清(clearCoverage 那类语义)
        check(src.peek(1, 60, 70, 1) === null, "不给区间仍是整轨清");
    }
    // **软失效**:失效的块进影子缓存供过渡帧垫底,`peek` 永不命中它。
    // 病根:captureProgress 每 500ms 一个半秒宽的 addedRanges,而跨度大的块
    // 与几乎每个新增区间都相交 ⇒ 硬删的话「整曲概览」每 500ms 消失一次,
    // 缩小视口时两侧永远露白(用户实测三轮的第二个现象)。
    {
        const mk = (n) => ({
            minDb: Array(n).fill(-20),
            maxDb: Array(n).fill(-10),
            covered: Array(n).fill(1),
            vad: Array(n).fill(0),
            stale: Array(n).fill(0),
            passId: Array(n).fill(1),
            valleys: [],
        });
        const src = WF.createWaveformSource({
            request: async (ch, a, b, c) => mk(c),
        });
        await src.getTile(3, 0, 300, 90); // 整曲概览块
        await src.getTile(3, 140, 170, 90); // 放大后的窄块
        // 模拟一次采集帧:半秒新增区间 —— 两块都与它相交
        src.invalidate(3, [{ startS: 150, endS: 150.5 }]);
        check(
            src.peek(3, 0, 300, 90) === null,
            "软失效后 peek 不再命中(权威绘制只认新鲜块)",
        );
        const near = src.peekOverlapping(3, 100, 200);
        check(
            near.length === 2 && near[0].endS - near[0].startS === 30,
            "过渡帧仍拿得到陈旧块补位,且**窄块排在前**(细节先占位)",
        );
        // clearCoverage 那类:数据真删了,影子也要清
        src.invalidate(3, null, { keepStale: false });
        check(
            src.peekOverlapping(3, 100, 200).length === 0,
            "硬删(keepStale:false)连影子一并清 —— 不画已不存在的波形",
        );
    }
    // 过渡帧**块数封顶**:每帧 × 每条可见泳道各跑一次,不封顶时 LRU 8 + 影子 8
    // 一帧要画十几块 × 每块上千列 ⇒ 帧率崩、整页卡片闪白(用户实测)。
    {
        eq(WF.TRANSIENT_BLOCK_CAP, 3, "过渡帧最多拼 3 块");
        const mk = (n) => ({
            minDb: Array(n).fill(-20),
            maxDb: Array(n).fill(-10),
            covered: Array(n).fill(1),
            vad: Array(n).fill(0),
            stale: Array(n).fill(0),
            passId: Array(n).fill(1),
            valleys: [],
        });
        const src = WF.createWaveformSource({
            request: async (ch, a, b, c) => mk(c),
        });
        // 造 6 块都与视口相交(跨度递减)
        for (const [a, b] of [
            [0, 300],
            [0, 200],
            [50, 180],
            [80, 160],
            [90, 150],
            [100, 140],
        ]) {
            await src.getTile(4, a, b, 60);
        }
        const near = src.peekOverlapping(4, 100, 140);
        eq(near.length, 3, "相交块超上限时只取 3 块");
        // **窄→宽**:细节块先占位,宽块只补空隙。反过来「宽块垫底窄块压顶」
        // 会让重叠区被半透明波形画两三遍,α 叠到近 1 ⇒ 波形闪白(用户实测)。
        const spans = near.map((b) => b.endS - b.startS);
        check(
            spans[0] <= spans[1] && spans[1] <= spans[2],
            `过渡帧块序是窄→宽(细节优先),实得 ${spans.join("/")}`,
        );
    }
    // **亮芯不随列宽膨胀**(用户实测「一放大缩小波形区域就闪白」的根因):
    // 亮芯色近白(α=.6)。原来宽度 = colW−0.6,稳态 colW≈1px 无碍,但过渡帧
    // 源列少、列宽涨到十几二十 px 时它就成了近白实心板并连成一片 ——
    // 逐像素实测:放大过程中近白像素占画布 17.2%(稳态 0%)。封顶后降到 2%。
    {
        const fills = [];
        let style = "";
        const ctx = {
            clearRect: () => {},
            fillRect: (x, y, w2) => fills.push({ style, w: w2 }),
            save: () => {},
            restore: () => {},
            beginPath: () => {},
            rect: () => {},
            clip: () => {},
            moveTo: () => {},
            lineTo: () => {},
            stroke: () => {},
            set fillStyle(v) {
                style = v;
            },
            get fillStyle() {
                return style;
            },
            set strokeStyle(_v) {},
            set lineWidth(_v) {},
        };
        const n = 10;
        const t3 = {
            minDb: Array(n).fill(-20),
            maxDb: Array(n).fill(-10),
            covered: Array(n).fill(1),
            vad: Array(n).fill(0),
            stale: Array(n).fill(0),
            passId: Array(n).fill(1),
            valleys: [],
        };
        // 10 列铺满 300px ⇒ colW = 30px(过渡帧放大档的典型值)
        WF.paintWaveTile(ctx, t3, 300, 34);
        const coreOf = (fs) =>
            fs.filter((f) => f.style === WF.DEFAULT_PALETTE.envCore);
        const envOf = (fs) =>
            fs.filter((f) => f.style === WF.DEFAULT_PALETTE.env);
        check(coreOf(fills).length === 0, "粗块(colW=30)整层不画亮芯");
        check(
            envOf(fills).some((f) => f.w > WF.ENV_CORE_MAX_COL_PX),
            "外柱仍随列宽铺满成连续轮廓(跳过的只是亮芯,不是整根柱子)",
        );

        // 同一块铺到 1px/列的稳态宽度:亮芯必须回来,否则「跳过」就退化成
        // 「永远不画」—— 那是另一个缺陷,而且同样能让上面两条断言变绿。
        fills.length = 0;
        WF.paintWaveTile(ctx, t3, n, 34);
        check(coreOf(fills).length > 0, "稳态(colW=1)照常画亮芯");
        check(
            coreOf(fills).every((f) => f.w <= WF.ENV_CORE_MAX_COL_PX),
            "稳态亮芯宽度不超过列宽门限",
        );

        // 镜像病:缩小时一列被压到 0.5px,而亮芯有 0.4px 最小宽度 ⇒ 列列重叠,
        // 一个像素里叠进两三道近白,合成实心白带(实测缩小过程白占 5.5%)。
        fills.length = 0;
        WF.paintWaveTile(ctx, t3, n * 0.5, 34);
        check(coreOf(fills).length === 0, "细块(colW=0.5)整层不画亮芯");
        check(envOf(fills).length > 0, "细块的外柱照画(跳过的只是亮芯)");
    }
    // **全曲概览块**:过渡帧的兜底垫底件(见 waveform.js `perChOverview` 头注)。
    // LRU 只有 8 块/轨、块宽都 ≈ 取它时的视口宽,过渡帧还要再砍到 3 块 ——
    // 缩小时它们合起来盖不满新视口,清屏后就是大片留白。实测:拖动缩放条时
    // 「完全没画的列」占比一路涨到 98.6%(接上概览后全程 0%)。
    {
        const calls = [];
        const mkTile = (n, cov) => ({
            minDb: Array(n).fill(-30),
            maxDb: Array(n).fill(-10),
            covered: Array(n).fill(cov ? 1 : 0),
            valleys: [],
        });
        const src = WF.createWaveformSource({
            request: (ch, s, e, n) => {
                calls.push([ch, s, e, n]);
                return Promise.resolve(mkTile(n, true));
            },
        });
        check(src.peekOverview(1) === null, "没取过时概览为 null");
        src.ensureOverview(1, 0, 300, WF.OVERVIEW_COLS);
        await new Promise((r) => setTimeout(r, 0));
        const ov = src.peekOverview(1);
        check(!!ov, "ensureOverview 取到后 peekOverview 命中");
        check(
            ov && ov.startS === 0 && ov.endS === 300,
            "概览块自带几何(0..300),拼接方按它映射",
        );
        check(
            calls.length === 1 && calls[0][3] === WF.OVERVIEW_COLS,
            `按 OVERVIEW_COLS(${WF.OVERVIEW_COLS})取,且只取一次`,
        );
        // 节流:紧接着再调不该再发一次(采集中每帧都会调到)
        src.ensureOverview(1, 0, 300, WF.OVERVIEW_COLS);
        await new Promise((r) => setTimeout(r, 0));
        check(calls.length === 1, "节流生效:同参紧接着再调不重复取数");
        // 软失效(captureProgress 那类)只标脏,手上那份继续垫底
        src.invalidate(1, [{ startS: 10, endS: 10.5 }]);
        check(
            !!src.peekOverview(1),
            "软失效后概览仍可垫底(2Hz 采集事件不该让它每 500ms 消失)",
        );
        // 硬失效(clearCoverage)必须真丢 —— 数据没了还垫底就是画不存在的波形
        src.invalidate(1, null, { keepStale: false });
        check(
            src.peekOverview(1) === null,
            "clearCoverage 走硬删:概览一并丢弃",
        );
    }
    // 可见列裁剪:块只有一小截落在画布内时,画布外的列不该逐列 fillRect
    {
        let fills = 0;
        const ctx = {
            clearRect: () => {},
            fillRect: () => fills++,
            save: () => {},
            restore: () => {},
            beginPath: () => {},
            rect: () => {},
            clip: () => {},
            moveTo: () => {},
            lineTo: () => {},
            stroke: () => {},
            set fillStyle(_v) {},
            set strokeStyle(_v) {},
            set lineWidth(_v) {},
        };
        const n = 1000;
        const big = {
            minDb: Array(n).fill(-20),
            maxDb: Array(n).fill(-10),
            covered: Array(n).fill(1),
            vad: Array(n).fill(0),
            stale: Array(n).fill(0),
            passId: Array(n).fill(1),
            valleys: [],
        };
        // 1000 列的块覆盖 [0,300),视口只看 [150,153) ⇒ 仅约 10 列可见
        WF.paintWaveTile(ctx, big, 300, 34, undefined, {
            tileStartS: 0,
            tileEndS: 300,
            viewStartS: 150,
            viewEndS: 153,
        });
        check(
            fills > 0 && fills < 80,
            `可见列裁剪生效(1000 列块只画可见的十几列,实得 ${fills} 次 fillRect)`,
        );
    }
    // 在途请求被失效后**不得回写**(pr-agent):采集中 2Hz 的区间级失效与用户
    // 平移取数会重叠,迟到的 resolve 若照写就是把失效前算出的旧块塞回缓存,
    // 后续 peek 命中它 ⇒ 新采/新清的区域一直显示旧图,直到下次失效才纠正。
    {
        let release;
        const gate = new Promise((r) => (release = r));
        const src = WF.createWaveformSource({ request: () => gate });
        const inflight = src.getTile(2, 0, 10, 1);
        // **必须用区间级失效**:整轨清走 perCh.delete(ch) 会把整个 Map 换掉,
        // 旧笔回写进的是已成孤儿的 Map,天然无害 —— 拿它做判据测不出这条竞态
        // (本断言第一版就栽在这,反向验证时才发现)。区间级走的是同一个 Map 上
        // 的 cache.delete(key),正是 pr-agent 指明的隐患路径。
        src.invalidate(2, [{ startS: 0, endS: 10 }]);
        release({
            minDb: [0],
            maxDb: [0],
            covered: [1],
            vad: [0],
            stale: [0],
            passId: [1],
            valleys: [],
        });
        await inflight;
        check(
            src.peek(2, 0, 10, 1) === null,
            "在途期间被失效 ⇒ 迟到的 resolve 不回写缓存",
        );
    }
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
    // **Wave 6 第三轮**:5px 判「有点太粗,颜色也不是很好看」→ 带收到 3px;
    // 亮薄荷 rgb(88,208,148) 换柔和青绿 rgb(122,205,178) —— 原色偏黄绿且饱和度
    // 高,与粉紫主题相冲,新色偏青、与薰衣草成邻近色;带窄了 alpha 再提到 .62。
    eq(WF.VAD_ALPHA, 0.62, "VAD 标注带 alpha 取 Wave 6 用户裁定的 .62");
    eq(WF.VAD_BAND_PX, 3, "VAD 标注带高 3px(Wave 6:5px 判「太粗」后收窄)");
    check(
        WF.VAD_BAND_PX <= 34 / 2 - WF.envelopeHalfPx(0, 34) + 1,
        "带高不越过包络顶(半高上限 h/2−4 ⇒ 柱尖恒在 y=4,最多咬 1px 且被柱盖回)",
    );
    check(
        WF.DEFAULT_PALETTE.vad === `rgba(122, 205, 178, ${WF.VAD_ALPHA})`,
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
        "rgba(154, 226, 202, 0.72)",
        "VAD 顶缘线 = --wave-vad-edge(Wave 6 随带体同步收敛)",
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
    eq(
        TW.BOTTOM_BAR_H,
        44,
        "底部条 44(Wave 6 裁定:20 → 44,腾出竖直纵向缩放条)",
    );
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
        "wave.recaptureArmedShort", // PR#64【重要】1 新增的无占位符短式
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

    // ---- ③ 过渡帧 = **按列映射重画 + 多块拼合**;与取数共用「静止 120ms」闸
    //
    // 用户 preview 三轮才收敛,前三版各错一处:
    //   ① 「重跑画笔 + ctx.scale」—— 非等比 scale 把 45° 斜纹剪成缓坡、
    //      lineWidth 横向拉粗;
    //   ② 「只 translate,跨度一变就不 blit」—— 缩放期波形整幅停在旧视口比例上,
    //      而标尺/曲线/边界/播放头每帧跟着新视口走,层间时间轴对不上;
    //   ③ 「drawImage 位图拉伸」—— 放大档糊(光栅升采样)、**缩小档两侧露白**
    //      (老块比新视口窄,margin 没东西可贴)。三条都是用户实测报回来的。
    // 终修:`paintWaveTile` 支持时间映射(tileStartS/EndS → viewStartS/EndS),
    // **列宽随映射变而画笔不变** ⇒ 斜纹/线宽/覆盖条几何恒正确;缩小档再把
    // 缓存里所有与视口相交的块按跨度从宽到窄拼上去,两侧不再留白。
    check(
        /paintWaveTile\(ctx, blk\.tile, w, laneH, pal, \{[\s\S]{0,300}tileStartS: blk\.startS/.test(
            tw,
        ),
        "过渡帧按**时间映射**重画缓存块(不是位图拉伸)",
    );
    check(
        !/ctx\.drawImage\(/.test(tw) && !/snapshotLane/.test(tw),
        "位图老底与 drawImage 拉伸已删除(放大糊 / 缩小露白的根因)",
    );
    check(
        !/ctx\.scale\(\(last\.endS - last\.startS\) \/ span, 1\)/.test(tw),
        "非等比 ctx.scale 重映射未复活(斜纹被剪切 / 线宽被拉粗的根因)",
    );
    check(
        /peekOverlapping\(\s*ch,\s*vp\.startS,\s*vp\.endS,?\s*\)[\s\S]{0,4000}for \(const blk of near\) drawBlock\(blk\)/.test(
            tw,
        ),
        "缩小档用**多块拼合**补满视口(单块只覆盖一段 ⇒ 两侧露白)",
    );
    check(
        /ctx\.clearRect\(0, 0, w, laneH\);[\s\S]{0,400}const drawBlock = /.test(
            tw,
        ),
        "拼合前整幅 clearRect 一次(每块自己不再清屏,clear:false)",
    );
    check(
        /const sameVp =[\s\S]{0,1600}!sameVp && \(near\.length \|\| ov\)/.test(
            tw,
        ),
        "画布已是当前视口时不清屏(宽度抖动导致的键失配不该闪空)",
    );
    // 概览兜底是**惰性**的:只有前面几块没盖满才动用。它跨整首曲子,每帧每轨
    // 都拉进来画的话超 32ms 长帧 3 → 13(brief §4.5 红线);多数帧本就盖得满。
    check(
        /for \(const \[f0, f1\] of filled\) covered \+= f1 - f0;[\s\S]{0,200}if \(covered < w - 0\.5\) drawBlock\(ov\)/.test(
            tw,
        ),
        "概览块只在前面没盖满时才动用(每帧无条件画 ⇒ 长帧超预算)",
    );
    // **空隙裁剪**:波形是半透明的(外柱 α=.52 / 内柱 α=.6),重叠区被画两三遍
    // 就把 α 叠到 0.89 / 0.94 —— 几乎纯白,一动视口波形区域就闪白(用户实测)。
    check(
        /const filled = \[\];[\s\S]{0,1600}ctx\.clip\(\);[\s\S]{0,400}paintWaveTile\(/.test(
            tw,
        ),
        "过渡帧按已画区间裁剪后再画(同一像素恒只画一次,α 不叠)",
    );
    // 裁剪边界**不得**对齐到整像素:实测那样能把发丝级接缝补到 0,但相邻空隙
    // 因此重叠,半透明又叠出白 —— 近白像素占比 0 → 14.6%,正是打了三轮的那个病。
    check(
        !/ctx\.rect\(\s*Math\.(floor|round)\(g0\)/.test(tw),
        "空隙裁剪不取整(取整会让相邻段重叠,α 叠加回潮成闪白)",
    );
    check(
        /xFrom: g0,\s*\n\s*xTo: g1,/.test(tw),
        "逐空隙画且把列循环收到该段(clip 只裁像素不裁循环,空跑上千列)",
    );
    check(
        /filled\.push\(\.\.\.gaps\)/.test(tw),
        "画完把空隙并入已画区间(后续宽块不再覆盖同一段)",
    );
    // 时间映射的纯函数面:块只覆盖视口一段时,只占那一段
    {
        const calls = [];
        const fakeCtx = {
            clearRect: (...a) => calls.push(["clear", ...a]),
            fillRect: (...a) => calls.push(["fill", ...a]),
            save: () => {},
            restore: () => {},
            beginPath: () => {},
            rect: () => {},
            clip: () => {},
            moveTo: () => {},
            lineTo: () => {},
            stroke: () => {},
            set fillStyle(_v) {},
            set strokeStyle(_v) {},
            set lineWidth(_v) {},
        };
        const t2 = {
            minDb: [-20, -20],
            maxDb: [-10, -10],
            covered: [1, 1],
            vad: [0, 0],
            stale: [0, 0],
            passId: [1, 1],
            valleys: [],
        };
        // 块覆盖 [0,10),视口 [0,20) ⇒ 只该画在左半边 [0,100) 的 100px 宽里
        WF.paintWaveTile(fakeCtx, t2, 200, 34, undefined, {
            clear: true,
            tileStartS: 0,
            tileEndS: 10,
            viewStartS: 0,
            viewEndS: 20,
        });
        const fills = calls.filter((c) => c[0] === "fill");
        check(
            fills.length > 0 && fills.every((c) => c[1] >= 0 && c[1] < 100),
            "时间映射:块只覆盖视口前半 ⇒ 绘制全部落在左半边",
        );
        check(
            calls.some((c) => c[0] === "clear"),
            "clear 缺省为 true(单块路径与老口径一致)",
        );
        const calls2 = [];
        const ctx2 = {
            ...fakeCtx,
            clearRect: (...a) => calls2.push(["clear", ...a]),
            fillRect: () => {},
        };
        WF.paintWaveTile(ctx2, t2, 200, 34, undefined, { clear: false });
        check(
            !calls2.some((c) => c[0] === "clear"),
            "clear:false 不清屏(多块拼合时只在最外层清一次)",
        );
    }
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
    // 位图老底已废除(过渡帧改按 tile 数据重画)⇒ 行高变化不再需要清老底,
    // 但两层必须标脏:canvas 后备存储要按新行高重建。
    check(
        /function applyLaneH\([\s\S]{0,900}local\.staticDirty = true;[\s\S]{0,200}local\.overlayDirty = true;/.test(
            tw,
        ),
        "行高变化 ⇒ 静态层与动态层两层标脏(后备存储按新行高重建)",
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
    // ---- Wave 5 用户裁定②:纵向缩放条移到窗口右下角(与纵向滚动条同列、
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
        "syncScrollGutter 不给纵向杆算右让(它在底部条的常规流里)",
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
            html.indexOf('class="wave-window"') < bar,
            "泳道区内不再有该件(件整体在底部条里)",
        );
    }
    // ---- Wave 6 用户裁定:纵向缩放条**竖回来**(preview 第三轮原话:「纵向缩放条
    //      在纵向进度条的下方,纵向放置,两个缩放条呈 90 度,都在右下角」)
    eq(TW.VZOOM_W, undefined, "横轨轨长常量 VZOOM_W 已撤销(Wave 6 竖回来)");
    eq(
        TW.VZOOM_H,
        22,
        "纵向缩放条轨长 22(竖轨;底部条内高 43 − 上下各 10.5 圆角余量)",
    );
    check(
        /\.wave-vzoom \{[^}]*width: 6px;[^}]*height: 22px;/.test(html),
        "杆本体 6×22 竖轨(与横向杆 64×6 呈 90°)",
    );
    check(
        /data-gb="wave-vzoom"[\s\S]{0,200}aria-orientation="vertical"/.test(
            html,
        ),
        "竖轨补回 aria-orientation=vertical(Wave 5 改横轨时删掉的那条)",
    );
    check(
        /\.wave-vzoom \{[^}]*cursor: ns-resize;/.test(html),
        "光标 ns-resize(竖轨只吃纵向位移)",
    );
    check(
        /\.wave-vzoom::after \{[^}]*inset: -10px -7px;/.test(html),
        "命中扩展 20×42 CSS px(纵轴吃满 44px 底条行高,不越界压泳道 canvas)",
    );
    check(
        /return laneHFromPercent\(1 - \(e\.clientY - rect\.top\) \/ rect\.height\)/.test(
            tw,
        ),
        "拖拽换算回纵轴(clientY ÷ 轨高,且**向上 = 变高**)",
    );
    check(
        /els\.vzoomThumb\.style\.top = \(1 - laneHPercent\(v\)\) \* 100 \+ "%"/.test(
            tw,
        ),
        "thumb 走纵向百分比行程(top = 1 − p,上 = 高,与拖拽同向)",
    );
    // 与纵向滚动条同列:槽宽跟 syncScrollGutter 实测的滚动条宽走(兜底 16px 是
    // 圆角账 —— 9px 窄滚动条下严格居中会让 11px 圆点探出窗口右内缘被啃)
    check(
        /els\.hzoomRow\.style\.setProperty\("--wave-gutter", sb \+ "px"\)/.test(
            tw,
        ),
        "syncScrollGutter 把滚动条宽写成 --wave-gutter(竖轨与滚动条对列)",
    );
    check(
        /\.wave-hzoom__vzoomslot \{[^}]*width: max\(var\(--wave-gutter, 0px\), 16px\);/.test(
            html,
        ),
        "槽宽 = max(滚动条宽, 16px),竖轨在槽内居中 = 与滚动条同列",
    );
    check(
        /\.wave-hzoom__vzoomctl \{[^}]*margin-right: calc\(var\(--sp-10\) \* -1\);/.test(
            html,
        ),
        "角落组抵掉底部条 10px 右内边距(不抵就落在刻度列正下方,不是滚动条下方)",
    );
    check(
        /\.wave-hzoom__vzoomctl \{[^}]*background: rgba\(var\(--wh\), 0\.035\);[^}]*border-left: 1px solid var\(--dark-rule\);/.test(
            html,
        ),
        "裁定⑦:两条共用的角落区有同族浅色底 + 分隔线(竖轨不再是孤零零一根针)",
    );
    // 底部条抬高到 44 后,三处窗级覆盖层的 bottom 必须同步(否则盖住底部条)
    for (const cls of ["wave-scalecol", "wave-selection", "wave-dim"]) {
        const i = html.indexOf(`.${cls} {`);
        check(
            i > 0 && /bottom: 44px;/.test(html.slice(i, i + 400)),
            `${cls} 的 bottom 与底部条 44 同步`,
        );
    }
    check(
        /\.wave-hzoom \{[^}]*height: 44px;/.test(html),
        "底部条行高 44px(与 BOTTOM_BAR_H 同步)",
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
    // 钉「注明了用户裁定来历」这件事本身,不钉某一次的具体色值 —— 色值经三轮
    // preview 改过两次(亮薄荷 → 柔和青绿),把 rgb 写进正则等于每调一次色就红一次。
    check(
        /Wave 6 用户 preview[\s\S]{0,400}--wave-vad: 122, 205, 178;/.test(css),
        "--wave-vad 为 Wave 6 柔和青绿且注明用户裁定来历",
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
        /local\.staticDirty = true;[\s\S]{0,300}schedulePaint\(\);/.test(
            tw.slice(tw.indexOf("const setInspectorOpen")),
        ),
        "开关改左栏宽 ⇒ 标脏重绘(舞台宽变了,canvas 后备存储要重建)",
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
log("\n=== ⑭ PR #64 评审处置(重要 6 条 + 建议若干)===");
{
    const tw = src("web/output/tab-wave.js");
    const tt = src("web/output/tab-tracks.js");
    const ap = src("web/output/app.js");
    const html = src("web/output/index.html");
    const TM = await import(u("web/output/tab-master.js"));

    // ---- 【重要】1:Tab2 布防圆点 tooltip 不泄漏占位符 --------------------
    {
        for (const lang of ["zh", "en", "fr"]) {
            eq(
                (
                    String(T[lang]["wave.recaptureArmedShort"]).match(
                        /\{\w+\}/g,
                    ) || []
                ).length,
                0,
                `重要1:${lang}.wave.recaptureArmedShort 零占位符`,
            );
        }
        check(
            /setTitle\(n\.recaptureBadge, t\["wave\.recaptureArmedShort"\]\)/.test(
                tt,
            ),
            "重要1:行首圆点 tooltip 走无占位符短式",
        );
        check(
            !/setTitle\(n\.recaptureBadge, t\["wave\.recaptureArmed"\]\)/.test(
                tt,
            ),
            "重要1:行模型只有布尔位,长式(带 {x}{y}{n})不得再灌进 title",
        );
    }

    // ---- 【重要】2:段表事件到达即作废进行中的边界拖拽 --------------------
    {
        check(
            /function cancelBoundDrag\(\) \{/.test(tw),
            "重要2:有独立的边界拖拽作废口",
        );
        const cIdx = tw.indexOf("function cancelBoundDrag() {");
        const cWin = tw.slice(cIdx, cIdx + 260);
        check(
            /local\.boundDrag = null;/.test(cWin) && !/sendEdit\(/.test(cWin),
            "重要2:作废只清态、**不发** move_boundary(与「释放才发」不冲突)",
        );
        const oIdx = tw.indexOf("    function onSegments(seg) {");
        const oWin = tw.slice(oIdx, tw.indexOf("local.echo = {};", oIdx));
        check(
            oWin.includes("cancelBoundDrag();"),
            "重要2:onSegments 与 rebindSegKeys 同拍作废边界拖拽(segIdx 已重编号)",
        );
        check(
            oWin.includes("rebindSegKeys("),
            "重要2:选中态重绑仍在(两条重绑纪律同处)",
        );
    }

    // ---- 【重要】3:停播态视口变化后播放头补写一帧 ------------------------
    {
        // 纯函数面:refresh() 的三条纪律(首帧前不写 / 不起 rAF / 不动节流账)
        const seen = [];
        const p = PH.createPlayhead({
            apply: (tS, playing) => seen.push([tS, playing]),
        });
        p.refresh();
        eq(
            seen.length,
            0,
            "重要3:首帧 §2.6 事件前 refresh() 不写入(维持初始 hidden)",
        );
        p.push({ timeS: 12.5, isPlaying: false });
        const afterPush = seen.length;
        check(!p.running(), "重要3:停播事件 push 后 rAF 不常驻(空闲零 rAF)");
        p.refresh();
        eq(seen.length, afterPush + 1, "重要3:停播态 refresh() 恰好补写一帧");
        eq(seen[seen.length - 1][0], 12.5, "重要3:补写的是当前 tS(停住则原地)");
        eq(seen[seen.length - 1][1], false, "重要3:playing 位如实传下去");
        check(!p.running(), "重要3:refresh() 不起 rAF");
        // 播放中 refresh() 同样只写一帧、不多起 rAF(补写是幂等的)
        const p2 = PH.createPlayhead({ apply: () => {} });
        p2.push({ timeS: 3, isPlaying: true });
        const rafBefore = p2.running();
        p2.refresh();
        eq(
            p2.running(),
            rafBefore,
            "重要3:播放中 refresh() 不改变 rAF 常驻状态",
        );
        // 接线面:视口变化与容器尺寸变化两处都补;**不得**加「循环没跑才补」前置
        // ——「循环在不在跑」不等于「停播没停播」(位置微抖的 30Hz 停住事件会让
        // rAF 常驻,加了前置恰好在最像 bug 的那一档上把补写跳掉)
        const gIdx = tw.indexOf("function refreshPlayheadGeometry() {");
        const gWin = tw.slice(gIdx, tw.indexOf("\n    }", gIdx));
        check(
            gWin.includes("playhead.refresh();") &&
                !gWin.includes("playhead.running()"),
            "重要3:补写无条件执行(不按 running() 前置跳过)",
        );
        const tlIdx = tw.indexOf("const timeline = createTimeline({");
        const tlWin = tw.slice(tlIdx, tw.indexOf("});", tlIdx));
        check(
            tlWin.includes("refreshPlayheadGeometry();"),
            "重要3:timeline.onChange 里补一次(平移/缩放)",
        );
        check(
            /new ResizeObserver\(\(\) => \{[\s\S]{0,400}?refreshPlayheadGeometry\(\);/.test(
                tw,
            ),
            "重要3:容器尺寸变化(舞台宽变)同样补一次",
        );
    }

    // ---- 【重要】4:滑杆零改动点击不整包下发 ------------------------------
    {
        const rIdx = tw.indexOf("    function releaseSlider(s) {");
        const rWin = tw.slice(rIdx, tw.indexOf("\n    }", rIdx));
        // 脏位**按杆记账**:共享位下键盘档那 250ms 内在另一根杆上按一下,会把
        // 前一根杆的脏位吃掉 ⇒ 整包下发与倒计时条被整条吞掉(校验实测)。
        check(
            /const dirty = !!s\.dirty;/.test(rWin) &&
                /s\.dirty = false;/.test(rWin),
            "重要4:releaseSlider 取并清**本杆**的「改过值没有」闸(不用共享位)",
        );
        check(
            !/local\.sliderDirty/.test(tw),
            "重要4:全页共享脏位已废除(共享位会跨杆互吃提交)",
        );
        // 节流尾包是跨杆共享的单例:零改动的那一次释放若先去清它,会把别的杆
        // 已排队的最后一档值丢掉且不补发 —— 修复反而制造新的丢包路径。
        check(
            rWin.indexOf("if (!dirty) return;") <
                rWin.indexOf("clearTimeout(local.paramTimer)"),
            "重要4:清共享节流尾包排在 !dirty 早退**之后**(别吃掉别的杆的在途值)",
        );
        check(
            rWin.indexOf("if (!dirty) return;") > 0 &&
                rWin.indexOf("if (!dirty) return;") <
                    rWin.indexOf("sendParams(s.def.api);"),
            "重要4:闸排在整包下发**之前**(零改动 ⇒ setVadParams/setSegmentation 都不发)",
        );
        check(
            rWin.indexOf("if (!dirty) return;") > 0 &&
                rWin.indexOf("if (!dirty) return;") <
                    rWin.indexOf("armCountdown("),
            "重要4:零改动也不挂倒计时条(C++ 侧根本不会跑流水线)",
        );
        eq(
            (tw.match(/s\.dirty = true;/g) || []).length,
            4,
            "重要4:四条写入路径(按下 / 移动 / 松手补值 / 方向键)各自置位",
        );
    }

    // ---- 【重要】5:总宽 <100ms 的相邻两段不建边界拖拽 --------------------
    {
        check(
            /if \(j >= 0 && minS < maxS\) \{/.test(tw),
            "重要5:可拖窗反转(minS >= maxS)时不建 boundDrag",
        );
        // 窗定义没被改动:两侧各留 50ms
        check(
            /const minS = j < 0 \? 0 : num\(segs\[j\] && segs\[j\]\.t0S, 0\) \+ 0\.05;/.test(
                tw,
            ),
            "重要5:左界仍是左段起点 +50ms",
        );
        check(/\) - 0\.05;/.test(tw), "重要5:右界仍是右段终点 −50ms");
    }

    // ---- 【重要】6:非激活版本的 scvb.segments 不进 store ------------------
    {
        const A = { version: 1, reason: "edit", channels: [{ ch: 1 }] };
        const B = { version: 2, reason: "copyVersion", channels: [{ ch: 1 }] };
        check(
            TM.segmentsEventApplies(A, 1),
            "重要6:版本与 version_active 一致 ⇒ 消费",
        );
        check(
            !TM.segmentsEventApplies(B, 1),
            "重要6:copyVersion 到非激活 dst(version=2,active=1)⇒ 丢弃",
        );
        check(
            TM.segmentsEventApplies({ ...B, version: 1 }, 1),
            "重要6:copyVersion 到**激活** dst ⇒ 照常消费",
        );
        check(
            TM.segmentsEventApplies({ version: 2, reason: "versionActive" }, 1),
            "重要6:versionActive 例外(与 scvb.state 的先后契约没规定,不得误丢新表)",
        );
        check(
            TM.segmentsEventApplies({ reason: "snapshot" }, 1),
            "重要6:载荷不带 version ⇒ 放行(不把老/简载荷判死)",
        );
        check(
            TM.segmentsEventApplies(
                { version: 1, reason: "snapshot" },
                undefined,
            ),
            "重要6:version_active 尚未落地 ⇒ 放行(首帧顺序不确定)",
        );
        check(!TM.segmentsEventApplies(null, 1), "重要6:空载荷不消费");
        // 合并语义本身没被改动
        const prev = {
            version: 1,
            reason: "snapshot",
            channels: [
                { ch: 1, segments: [] },
                { ch: 2, segments: [] },
            ],
        };
        const inc = {
            version: 1,
            reason: "edit",
            channels: [{ ch: 2, segments: [{ segIdx: 0 }] }],
        };
        const out = TM.applySegmentsEvent(prev, inc);
        eq(
            out.channels.length,
            2,
            "重要6:增量 reason 按 ch 整条替换,其余轨保留",
        );
        eq(out.channels[1].segments.length, 1, "重要6:受影响轨换成新表");
        eq(
            TM.applySegmentsEvent(prev, { version: 1, reason: "snapshot" })
                .reason,
            "snapshot",
            "重要6:全量 reason 整表替换",
        );
        // 接线面:丢弃是整帧丢弃(不转发给三个 tab)
        const sIdx = ap.indexOf('bridge.on("scvb.segments"');
        const sWin = ap.slice(
            sIdx,
            ap.indexOf("tabMaster.onSegments(seg);", sIdx),
        );
        check(
            /segmentsEventApplies\(\s*seg,\s*\(store\.state\.global \|\| \{\}\)\.version_active,?\s*\)/.test(
                sWin,
            ) && sWin.includes("return;"),
            "重要6:app.js 在转发给任何 tab 之前先过版本闸",
        );
        check(
            /待 native 侧确认/.test(src("web/output/tab-master.js")),
            "重要6:「C++ 对非激活 dst 是否发事件」的待确认项写在代码注释里",
        );
        // mock 的既有行为不动(不拿 mock 掩盖问题)
        check(
            /const store = dst === model\.snapshot\.global\.version_active;/.test(
                src("web-preview/mock/juce-bridge-mock.js"),
            ),
            "重要6:mock 仍按原样对非激活 dst 发事件(UI 侧防御,不改 mock)",
        );
    }

    // ---- 【建议】1:局部件不再与契约 §8.2 禁项撞名 ------------------------
    {
        const banned = "merge" + "Segments";
        for (const f of [
            "web/output/app.js",
            "web/output/tab-master.js",
            "web/output/tab-wave.js",
            "web/output/tab-tracks.js",
        ]) {
            check(!src(f).includes(banned), `建议1:${f} 无与禁项撞名的标识符`);
        }
        check(
            /export function applySegmentsEvent\(prev, next\)/.test(
                src("web/output/tab-master.js"),
            ),
            "建议1:改名后的件仍是可 import 的纯函数(node 侧可断言)",
        );
    }

    // ---- 【建议】2:Tab3 布防 badge 的静态兜底走 key ----------------------
    {
        check(
            /data-gb="wave-recapture-badge"\s+data-t="wave\.recaptureArmedShort"/.test(
                html,
            ),
            "建议2:Tab3 badge 补 data-t(词条一律走 key)",
        );
        check(
            !/data-gb="wave-recapture-badge"[\s\S]{0,200}data-t="wave\.recaptureArmed"[^S]/.test(
                html,
            ),
            "建议2:兜底用的是短式 —— 长式挂 data-t 会在切语言那一拍闪出裸占位符",
        );
        check(
            /fmtKey\("wave\.recaptureArmed", \{/.test(tw),
            "建议2:布防中仍由 render() 用长式灌完整的 {x}–{y}·{n} 串覆盖",
        );
    }

    // ---- 【建议】3:Tab2 图例行 badge 不再是死接线 ------------------------
    {
        check(
            /const recaptureBadge = \$\("tracks-legend-recapture-badge"\);/.test(
                tt,
            ),
            "建议3:图例 badge 有节点句柄",
        );
        check(
            /show\(recaptureBadge, armed\);/.test(tt),
            "建议3:按 §2.1 recapture 显隐(此前从没有人 show 它)",
        );
        check(
            /fmt\(t\["wave\.recaptureArmed"\], \{[\s\S]{0,160}?x: secondsToTimecode/.test(
                tt,
            ),
            "建议3:显示前 fmt 灌值 —— 一显示就裸渲占位符的情形已消除",
        );
        check(
            html.includes('data-gb="tracks-legend-recapture-badge"'),
            "建议3:app.js 的点击跳转接线与 CSS 都还指着这个锚点",
        );
    }

    // ---- 【建议】4:tab-tracks 的过期注释已更正 ---------------------------
    {
        check(
            !/Tab1 nextParamEcho 的 full 分支尚为/.test(tt),
            "建议4:「Tab1 full 分支尚为全清」的过期断言已从注释里去掉",
        );
        // 事实面:Tab1 的 full 分支现在确实保留拖动中的 id(两 tab 口径一致)
        const npIdx = src("web/output/tab-master.js").indexOf(
            "export function nextParamEcho(",
        );
        const npWin = src("web/output/tab-master.js").slice(npIdx, npIdx + 420);
        check(
            /if \(payload\.full\) \{[\s\S]{0,220}?held \? \{ \[gestureId\]: src\[gestureId\] \} : \{\}/.test(
                npWin,
            ),
            "建议4:Tab1 full 分支保留 gestureId(注释更正后的事实依据)",
        );
    }

    // ---- 【额外】指针捕获统一走会吞异常的 helper --------------------------
    // setPointerCapture 对非活动指针抛 NotFoundError,而多数调用点排在
    // `local.xxxDrag = …` **之前** —— 一抛就把建态语句甩掉,症状是「按住拖动
    // 完全没反应且零报错」。本波在 web-preview 的合成 PointerEvent 上实测到:
    // 边界拖拽的 pointerdown 整段被这一抛吞掉。
    {
        check(
            /function capturePointer\(el, e\) \{[\s\S]{0,220}?try \{[\s\S]{0,80}?el\.setPointerCapture\(e\.pointerId\);[\s\S]{0,60}?\} catch \{/.test(
                tw,
            ),
            "额外:capturePointer 把 setPointerCapture 包进 try/catch",
        );
        // 除 helper 自身外,全页不得再有裸调
        const bare = [...tw.matchAll(/(\w[\w.]*)\.setPointerCapture\(/g)].map(
            (m) => m[1],
        );
        eq(
            bare.join(","),
            "el",
            "额外:全页只剩 helper 内那一处 setPointerCapture(其余一律走 capturePointer)",
        );
        eq(
            (tw.match(/capturePointer\(/g) || []).length,
            11, // 1 处定义 + 10 个手势入口
            "额外:十个手势入口(滑杆 / 泳道两支 / 标尺 / 选区手柄 / 底部条 / 两条缩放条 / 检查器旋钮与滑杆)全部改道",
        );
    }

    // ---- 【建议】5:waveform 块形状守卫 ----------------------------------
    {
        const ok = { minDb: [-20], maxDb: [-10], covered: [1] };
        check(WF.isTileShape(ok), "建议5:齐全等长的块通过");
        check(!WF.isTileShape(null), "建议5:空载荷不通过");
        check(
            !WF.isTileShape({ minDb: [-20] }),
            "建议5:只有 minDb(旧守卫的全部)不通过 —— 绘制循环还要下标 covered/maxDb",
        );
        check(
            !WF.isTileShape({ minDb: [-20], maxDb: [-10], covered: [1, 1] }),
            "建议5:三条不等长不通过",
        );
        check(
            !WF.isTileShape({ minDb: [-20], maxDb: [-10] }),
            "建议5:缺 covered 不通过(§1.27 拒绝载荷 {reason}/{observer} 同归此支)",
        );
        // 画笔自守:畸形块进来只是不画,不抛
        let threw = false;
        const stub = {
            clearRect() {},
            fillRect() {},
            save() {},
            restore() {},
            beginPath() {},
            rect() {},
            clip() {},
            moveTo() {},
            lineTo() {},
            stroke() {},
        };
        try {
            WF.paintWaveTile(stub, { minDb: [-20] }, 100, 34);
            WF.paintWaveTile(stub, { minDb: [-20], passId: [1] }, 100, 34);
        } catch {
            threw = true;
        }
        check(!threw, "建议5:畸形块进画笔只是不画,不炸 rAF 静态层重绘");
        check(
            /if \(!isTileShape\(tile\)\) \{/.test(
                src("web/output/canvas/waveform.js"),
            ),
            "建议5:拉取侧同一道守卫(畸形响应不进 LRU)",
        );
    }

    // ---- 【建议】6:两条缩放条进帧时账 ------------------------------------
    {
        const iIdx = tw.indexOf("function interactionActive() {");
        const iWin = tw.slice(iIdx, tw.indexOf("\n    }", iIdx));
        check(
            iWin.includes("local.hzoomDrag") &&
                iWin.includes("local.vzoomDrag"),
            "建议6:横/纵缩放条拖拽计入交互期(否则降级序列在最重的路径上失效)",
        );
        check(
            /local\.vzoomDrag = true;[\s\S]{0,200}?ensureTicker\(\);/.test(tw),
            "建议6:纵向条按下也起帧时账循环",
        );
    }

    // ---- 【建议】7:滑杆窗级松手兜底 --------------------------------------
    {
        check(
            /window\.addEventListener\("pointerup", \(\) => up\(null\)\);/.test(
                tw,
            ) &&
                /window\.addEventListener\("pointercancel", \(\) => up\(null\)\);/.test(
                    tw,
                ),
            "建议7:指针在窗外释放 / setPointerCapture 抛错时滑杆不卡在拖拽态",
        );
        check(
            /const v = e \? sliderValueFromEvent\(s, e\) : null;/.test(tw),
            "建议7:窗级那一道拿不到杆内坐标 ⇒ 只收尾不取值",
        );
    }

    // ---- 【建议】8:Backspace 在按钮/控件聚焦时不吞键 ---------------------
    {
        const kIdx = tw.indexOf("function mountKeyboard() {");
        const kWin = tw.slice(kIdx, tw.indexOf("\n    }\n", kIdx));
        check(
            /const onControl =/.test(kWin) &&
                /a\.tagName === "BUTTON"/.test(kWin) &&
                /role === "slider"/.test(kWin),
            "建议8:按钮 / role=slider 等自定义控件算「焦点在控件上」",
        );
        check(
            /e\.key === "Delete" \|\| e\.key === "Backspace"\) &&\s*\n\s*!inField &&\s*\n\s*!onControl/.test(
                kWin,
            ),
            "建议8:合并快捷键在控件聚焦时让路(不再吞 Backspace)",
        );
        check(
            kWin.indexOf('e.key === "Escape"') < kWin.indexOf("!onControl"),
            "建议8:Escape 不受此闸影响(按钮上按 Esc 仍能关确认框)",
        );
    }

    // ---- 【建议】9:死字段清掉 --------------------------------------------
    {
        check(
            !tw.includes("lastVpChange"),
            "建议9:只写不读的 lastVpChange 已删",
        );
        check(
            !/local\.pendingDown = \{[\s\S]{0,120}?moved: 0,/.test(tw),
            "建议9:从未被读的 pendingDown.moved 已删",
        );
        check(/vpIdleTimer: 0,/.test(tw), "建议9:真正在用的静止计时账保留");
    }
}

// =============================================================================
if (fail) {
    console.error(`\n${fail} 处断言失败`);
    process.exit(1);
}
log("\n全部通过");
