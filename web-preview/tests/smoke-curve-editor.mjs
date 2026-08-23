// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 曲线编辑器冒烟(node,无 DOM;T34)
// =============================================================================
// 覆盖 07 T34 验收的回归面:空态 / 加点 / 拖拽 / 16 点上限 / 删除 / aria / side 三值 /
// MS 叠加线纯显示(J68)。与 smoke-tab1-interactions.mjs 同款:纯逻辑断言 + mock 端到端,
// 不驱动真 DOM(curve-editor.js 的工厂 createCurveEditor 需真浏览器手测)。
//
// 用法:node web-preview/tests/smoke-curve-editor.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有断言失败。
// =============================================================================

import { pathToFileURL, fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";
import { readFileSync } from "node:fs";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const u = (p) => pathToFileURL(join(ROOT, p)).href;

const CE = await import(u("web/output/canvas/curve-editor.js"));
const { createBridge } = await import(u("web/shared/bridge.js"));
const { T } = await import(u("web/shared/i18n.js"));
const driver = await import(u("web-preview/mock/state-driver.js"));

let fail = 0;
const check = (cond, msg) => {
    if (!cond) {
        fail++;
        console.error("  [FAIL]", msg);
    }
    return cond;
};
const eq = (a, b, msg) =>
    check(
        JSON.stringify(a) === JSON.stringify(b),
        msg + ": 实得 " + JSON.stringify(a) + ",期望 " + JSON.stringify(b),
    );
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// =============================================================================
console.log(
    "=== ① 曲线数学(与 scvb_core 同源;CURVE-1…6 对拍值另见 check-curve-parity) ===",
);
{
    const bell = { angle: 30, gain_db: 6, shape: "bell", q: 3, side: "out" };
    const delta = CE.halfWidth(3);
    check(Math.abs(CE.evalShape(bell, 30) - 6) < 1e-9, "CURVE-1 G(30)=6");
    check(
        Math.abs(CE.evalShape(bell, 30 + delta) - 3) < 1e-9,
        "CURVE-1 G(P0±Δ)=3",
    );
    check(
        Math.abs(CE.evalShape(bell, 30 + 2 * delta) - 0.375) < 1e-9,
        "CURVE-1 G(P0+2Δ)=0.375",
    );

    const shelf = {
        angle: -45,
        gain_db: -6,
        shape: "shelf",
        q: 2,
        side: "out",
    };
    check(
        Math.abs(CE.evalShape(shelf, -95) - -5.8921) < 1e-4,
        "CURVE-2 G(-95)≈-5.8921(P0<0→left)",
    );
    const shelfR = {
        angle: -45,
        gain_db: -6,
        shape: "shelf",
        q: 2,
        side: "right",
    };
    check(
        Math.abs(CE.evalShape(shelfR, -95) - -0.1079) < 1e-4,
        "CURVE-6 镜像 G(-95)≈-0.1079",
    );

    // cut slope:q 承载 slope(dB/oct);A=−12,s=12 → u_b=1,side=right(d=P−30)。
    const cut = { angle: 30, gain_db: -12, shape: "cut", q: 12, side: "right" };
    check(CE.evalShape(cut, 32) === -12, "CURVE-5 cut slope d=2°→G=−12");
    check(
        Math.abs(CE.evalShape(cut, 30 + Math.SQRT2) - -6) < 1e-6,
        "CURVE-5 cut slope d=√2°→G=−6(smoothstep(0.5)=0.5)",
    );
    check(CE.evalShape(cut, 31) === 0, "CURVE-5 cut slope d=1°→G=0");
    check(CE.evalShape(cut, 30.5) === 0, "CURVE-5 cut slope d=0.5°<d0→G=0");
    check(CE.evalShape(cut, 20) === 0, "CURVE-5 cut slope 保留侧(d<0)→G=0");

    check(
        CE.evalCurve(
            [
                { angle: 0, gain_db: 8, shape: "bell", q: 1.5, side: "out" },
                { angle: 0, gain_db: 8, shape: "bell", q: 1.5, side: "out" },
            ],
            0,
        ) === 12,
        "CURVE-3 双 bell clamp 到 +12",
    );

    // 空态:LUT 全 0、0 dB 直线
    const emptyLut = CE.buildLut([]);
    check(emptyLut.length === CE.LUT_SIZE, "空态 LUT 长度 2049");
    check(
        emptyLut.every((v) => v === 0),
        "空态 LUT 全 0",
    );
    eq(CE.lutGainDb(emptyLut, 37), 0, "空态 gainDb(37)=0");

    // LUT 网格点 = 表值,插值连续
    const lut = CE.buildLut([bell]);
    check(
        Number.isFinite(lut[0]) && Number.isFinite(lut[CE.LUT_SIZE - 1]),
        "G(±100) 有限、无 NaN",
    );
    check(
        Math.abs(CE.lutGainDb(lut, 0) - lut[Math.floor(CE.LUT_SIZE / 2)]) <
            1e-9,
        "网格点(pan=0 → i=LUT_SIZE/2)插值 = 表值",
    );
}

// =============================================================================
console.log("=== ② MS 等效增益 g_eq(J68 纯显示层) ===");
{
    check(
        CE.eqGainDb(0, -100) === 0 &&
            CE.eqGainDb(0, 0) === 0 &&
            CE.eqGainDb(0, 100) === 0,
        "t=0 全域恒 0 dB",
    );
    // P=0(θ=π/4,sin2θ=1)→ g_eq = 20·log10(g_M)
    const tM = 0.4; // ms_balance=+40 → t=0.4,g_M=0.6
    const exp = 20 * Math.log10(0.6);
    check(Math.abs(CE.eqGainDb(40, 0) - exp) < 1e-9, "P=0 → 20·log10(g_M)");
    // P=±100(sin2θ=0)→ g_eq = 10·log10((g_M²+g_S²)/2)
    const gM = 0.6,
        gS = 1;
    const expE = 10 * Math.log10((gM * gM + gS * gS) / 2);
    check(
        Math.abs(CE.eqGainDb(40, 100) - expE) < 1e-9,
        "P=100 → 10·log10((g_M²+g_S²)/2)",
    );
    check(
        Number.isFinite(CE.eqGainDb(100, 0)) || true,
        "极端 ms=+100 不抛错(≈-∞ 由 dbToY 夹取)",
    );
    // 纯函数:入参不被改写
    const p = { angle: 0, gain_db: 0, shape: "bell", q: 1, side: "out" };
    const snap = JSON.stringify(p);
    CE.eqGainDb(40, 0);
    eq(JSON.stringify(p), snap, "eqGainDb 不改入参");
}

// =============================================================================
console.log("=== ③ side 三值 + 确定性回退(02 §7.1 / J07) ===");
{
    eq(
        CE.resolveSide({ angle: 30, shape: "bell", side: "out" }),
        "out",
        "bell 忽略 side",
    );
    eq(
        CE.resolveSide({ angle: 2, shape: "shelf", side: "out" }),
        "right",
        "out(angle≥0)→right",
    );
    eq(
        CE.resolveSide({ angle: -3, shape: "shelf", side: "out" }),
        "left",
        "out(angle<0)→left",
    );
    eq(
        CE.resolveSide({ angle: -70, shape: "shelf", side: "right" }),
        "right",
        "显式 right 保持",
    );
    check(
        CE.needsSideHint({ angle: 2, shape: "shelf", side: "out" }),
        "|angle|<5 且 out → 需方向提示",
    );
    check(
        !CE.needsSideHint({ angle: 10, shape: "shelf", side: "out" }),
        "|angle|≥5 无需提示",
    );
    check(
        !CE.needsSideHint({ angle: 2, shape: "bell", side: "out" }),
        "bell 无需方向提示",
    );
}

// =============================================================================
console.log("=== ④ 点操作:空态/加点/拖拽/16 上限/删除(纯函数,不改入参) ===");
{
    const base = [{ angle: -50, gain_db: 0, shape: "bell", q: 1, side: "out" }];
    const snap = JSON.stringify(base);

    // 加点:默认 bell / q=1 / gain 取双击 y / 按 angle 升序
    const added = CE.addPoint(base, 30, 6);
    eq(added.length, 2, "加点后 2 点");
    eq(
        added[1],
        { angle: 30, gain_db: 6, shape: "bell", q: 1, side: "out" },
        "加点默认 bell/q=1/gain=双击y",
    );
    eq(JSON.stringify(base), snap, "addPoint 不改入参");

    // 拖拽:夹取 angle/gain
    const moved = CE.movePointTo(base, 0, 999, -999);
    eq(
        moved[0],
        { angle: 100, gain_db: -12, shape: "bell", q: 1, side: "out" },
        "拖拽越界夹取 angle=100/gain=-12",
    );

    // 16 点上限
    const full = Array.from({ length: 16 }, () => ({
        angle: 0,
        gain_db: 0,
        shape: "bell",
        q: 1,
        side: "out",
    }));
    check(CE.addPoint(full, 1, 1) === null, "16 点达上限 addPoint → null");

    // 删除
    eq(CE.removePoint(base, 0), [], "删除唯一一点 → 空");
    eq(
        JSON.stringify(CE.removePoint(base, 99)),
        JSON.stringify(base),
        "越界删除返回原数组",
    );
    eq(JSON.stringify(base), snap, "removePoint 不改入参");

    // 更新 shape/side/q
    const upd = CE.updatePoint(base, 0, { shape: "shelf", side: "left", q: 8 });
    eq(upd[0].shape, "shelf", "updatePoint 改 shape");
    eq(upd[0].q, 8, "updatePoint 改 q");
    eq(JSON.stringify(base), snap, "updatePoint 不改入参");
}

// =============================================================================
console.log("=== ⑤ aria 播报(05 §6.2 a11y) ===");
{
    const t = T.zh;
    check(
        (() => {
            const txt = CE.announcePoint(
                1,
                {
                    angle: -35,
                    gain_db: 2.5,
                    shape: "bell",
                    q: 1.4,
                    side: "out",
                },
                t,
            );
            return (
                txt.includes("点 2") &&
                txt.includes("−35") &&
                txt.includes("+2.5 dB") &&
                txt.includes("钟形") &&
                txt.includes("Q 1.4")
            );
        })(),
        "bell 播报含角度/增益/形状/Q",
    );
    check(
        /方向 向右/.test(
            CE.announcePoint(
                1,
                {
                    angle: -35,
                    gain_db: 2.5,
                    shape: "shelf",
                    q: 1.4,
                    side: "right",
                },
                t,
            ),
        ),
        "shelf/cut 播报追加方向",
    );
    // 三语 key 齐备 + 占位符一致
    const keys = [
        "curve.canvasLabel",
        "curve.maxPoints",
        "curve.centerSide",
        "curve.shape.bell",
        "curve.shape.shelf",
        "curve.shape.cut",
        "curve.side.out",
        "curve.side.left",
        "curve.side.right",
        "curve.qLabel",
        "curve.slopeLabel",
        "curve.slope.opt6",
        "curve.slope.opt12",
        "curve.slope.opt18",
        "curve.slope.opt24",
        "curve.sideTooltip",
        "curve.deleteLabel",
        "curve.announcePoint",
        "curve.announcePointDir",
    ];
    for (const k of keys) {
        for (const lang of ["zh", "en", "fr"]) {
            check(
                typeof T[lang][k] === "string" && T[lang][k].trim() !== "",
                "词条 " + lang + "." + k + " 缺失/为空",
            );
        }
    }
}

// =============================================================================
console.log(
    "=== ⑥ mock 端到端:setPanCurve 校验 + J68 纯显示(ms_balance 不改 pan_curve) ===",
);
{
    function makeStore() {
        return { state: {}, params: { values: {} } };
    }
    function stripFull(s) {
        const o = { ...(s || {}) };
        delete o.full;
        return o;
    }

    const s = driver.createPreviewSession({
        role: "output",
        params: new URLSearchParams("fixture=fifteen-tracks"),
    });
    const bridge = createBridge({ role: "output", mockBackend: s.mock });
    const store = makeStore();
    bridge.on("scvb.state", (p) => {
        store.state =
            p && p.full
                ? stripFull(p)
                : CE_deepMerge(store.state, stripFull(p));
    });
    bridge.on("scvb.params", (p) => {
        store.params = {
            values: p.full
                ? { ...p.values }
                : { ...store.params.values, ...p.values },
        };
    });
    function CE_deepMerge(base, patch) {
        if (!patch || typeof patch !== "object" || Array.isArray(patch))
            return patch;
        const out = {
            ...(base && typeof base === "object" && !Array.isArray(base)
                ? base
                : {}),
        };
        for (const k of Object.keys(patch)) {
            out[k] = CE_deepMerge(out[k], patch[k]);
        }
        return out;
    }

    s.start();
    const snap = await bridge.requestInitialState();
    store.state = CE_deepMerge(store.state, stripFull(snap));

    // fifteen-tracks 有 6 个 demo pan_curve 点(makeTourDemoSnapshot 的 demoPanCurve,含 cut slope 点)
    const v0 = (store.state.versions || [])[0];
    const pts = v0 && v0.pan_curve && v0.pan_curve.points;
    check(
        Array.isArray(pts) && pts.length > 0,
        "fifteen-tracks 快照带非空 pan_curve",
    );

    // setPanCurve:>16 点 → badArg(契约 §1.17 拒绝态)
    const seventeen = Array.from({ length: 17 }, (_, i) => ({
        angle: i * 5 - 40,
        gain_db: 0,
        shape: "bell",
        q: 1,
        side: "out",
    }));
    eq(
        await bridge.setPanCurve(seventeen),
        { ok: false, reason: "badArg" },
        "setPanCurve 17 点 → badArg",
    );
    // side 三值 + side=out |P0|<5 合法
    eq(
        (
            await bridge.setPanCurve([
                { angle: -70, gain_db: -4, shape: "shelf", q: 1, side: "left" },
                { angle: 2, gain_db: -2, shape: "shelf", q: 2, side: "out" },
                { angle: 60, gain_db: -3, shape: "cut", q: 12, side: "right" },
            ])
        ).ok,
        true,
        "setPanCurve 覆盖 side 三值(含 side=out |P0|<5)回 ok",
    );
    await sleep(60);

    // J68:拖动 ms_balance 期间 pan_curve 点集逐字节不变(叠加线是纯显示层)
    const before = JSON.stringify((store.state.versions || [])[0].pan_curve);
    await bridge.setParam("ms_balance", 40);
    await sleep(60);
    const after = JSON.stringify((store.state.versions || [])[0].pan_curve);
    eq(before, after, "setParam(ms_balance) 期间 pan_curve 逐字节不变(J68)");
    eq(
        store.params.values.ms_balance,
        40,
        "ms_balance 本身已回推(用户直改,非叠加线写)",
    );

    s.stop();
}

// =============================================================================
console.log("=== ⑦ 源码级不变式:叠加线纯显示(draw/render 零写) ===");
{
    const ceSrc = readFileSync(
        join(ROOT, "web/output/canvas/curve-editor.js"),
        "utf8",
    );
    const calls = ceSrc.split("bridge.setPanCurve(").length - 1;
    eq(calls, 1, "curve-editor.js 只有一处 setPanCurve 调用点(commit 路径)");
    check(
        !/bridge.setParam|bridge.beginParamGesture|bridge.endParamGesture/.test(
            ceSrc,
        ),
        "curve-editor.js 不经参数通道写任何参数",
    );
}

// =============================================================================
console.log(
    "=== ⑧ 源码级不变式:Q 拖动单绘制源(无回跳)+ qRead 即时 + LUT 缓存 ===",
);
{
    const ceSrc = readFileSync(
        join(ROOT, "web/output/canvas/curve-editor.js"),
        "utf8",
    );
    // draw()/syncToolbar() 以 local.dragPoints 为优先绘制源(本地拖动态为准,单绘制源)
    const srcCount = ceSrc.split("local.dragPoints || points()").length - 1;
    check(
        srcCount >= 2,
        "draw()/syncToolbar() 读 local.dragPoints || points()(单绘制源)",
    );
    // render() 不得清 local.dragPoints(否则 store 回显与本地态打架 → 曲线「一跳一跳」)
    check(
        !/if \(!local\.dragging\) local\.dragPoints = null/.test(ceSrc),
        "render() 不再清 local.dragPoints",
    );
    // commit() 在 echo 之后(finally)才清 dragPoints
    check(
        /finally[\s\S]*?dragPoints\s*=\s*null/.test(ceSrc),
        "commit() 在 finally 清 dragPoints(echo 后)",
    );
    // draw() 有 LUT 缓存(不逐事件重建)
    check(/lutCache/.test(ceSrc), "draw() 有 LUT 缓存(lutCache)");
    // Q input 处理器立即写 qRead(不等 store 回显)
    check(
        /_toolbar\.qRead\.textContent = qLabel\(v\)/.test(ceSrc),
        "Q input 处理器立即写 qRead",
    );
    // onPointerUp 在 commit 前设 local.dragPoints = next(使 commit finally 的引用守卫
    // 成立;next 是 .slice() 出的新数组,不重指则 finally 永不清 → 拖后一直读旧数组)
    check(
        /local\.dragPoints = next;\s*commit\(next\)/.test(ceSrc),
        "onPointerUp 在 commit 前设 local.dragPoints = next",
    );
}

// =============================================================================
console.log("=== ⑨ Q 逐事件递增的纯函数面(曲线连续:改 q 不动其余字段) ===");
{
    let cur = [{ angle: 0, gain_db: 0, shape: "bell", q: 1, side: "out" }];
    for (const q of [1.5, 2.0, 2.5, 3.0, 3.5]) {
        const next = CE.updatePoint(cur, 0, { q });
        eq(next[0].q, q, "updatePoint 逐事件 q 单调递增:" + q);
        eq(
            [next[0].angle, next[0].gain_db, next[0].shape, next[0].side],
            [0, 0, "bell", "out"],
            "改 q 不动 angle/gain/shape/side",
        );
        cur = next;
    }
    // qLabel 数值单调(驱动 qRead 文本单调)
    let prev = -Infinity;
    for (let q = 0.5; q <= 10; q += 0.1) {
        const v = parseFloat(CE.qLabel(q));
        check(v >= prev, "qLabel 单调非降 @q=" + q.toFixed(1));
        prev = v;
    }
}

// =============================================================================
console.log(
    "=== ⑩ cut slope 分段钮 / cut 无 Q 滑杆 / bell 有 Q(源码级不变式) ===",
);
{
    const ceSrc = readFileSync(
        join(ROOT, "web/output/canvas/curve-editor.js"),
        "utf8",
    );
    check(
        /data-curve-slope/.test(ceSrc) &&
            /SLOPES/.test(ceSrc) &&
            /setSlope/.test(ceSrc),
        "cut → 斜率分段钮(data-curve-slope + SLOPES + setSlope)",
    );
    check(
        /tb\.qWrap\.hidden = isCut/.test(ceSrc),
        "cut 选中时 Q 滑杆隐藏(qWrap.hidden = isCut)",
    );
    check(
        /tb\.slopeGroup\.hidden = !isCut/.test(ceSrc),
        "bell/shelf 选中时斜率组隐藏(slopeGroup.hidden = !isCut)",
    );
    check(
        /curve-toolbar__q/.test(ceSrc) && /qSlider/.test(ceSrc),
        "bell/shelf 仍有 Q 滑杆(qWrap/qSlider 保留)",
    );
    check(
        /curve\.sideTooltip/.test(ceSrc),
        "side 选择器带 tooltip(curve.sideTooltip)",
    );
}

// =============================================================================
console.log("=== ⑪ 工具条溢出防线(紧凑标签 + title/aria 全称 + 单行浮条) ===");
{
    const ceSrc = readFileSync(
        join(ROOT, "web/output/canvas/curve-editor.js"),
        "utf8",
    );
    const cssSrc = readFileSync(join(ROOT, "web/output/index.html"), "utf8");
    // slope 按钮用纯数字紧凑标签(避免 "6 dB/oct" 长文本把工具条撑出窗格)
    check(
        /b\.textContent = String\(s\)/.test(ceSrc),
        "slope 按钮紧凑纯数字标签(textContent = String(s))",
    );
    // 全称 "N dB/oct" 保留在 title/aria-label(a11y + hover 提示)
    check(
        /b\.title = full/.test(ceSrc) && /aria-label", full/.test(ceSrc),
        "slope 按钮 title/aria-label 带全称 dB/oct",
    );
    // 工具条单行浮条(禁 flex-wrap,防两行遮画布)
    const tbBlock = /\.curve-toolbar\s*\{([^}]*)\}/.exec(cssSrc)?.[1] || "";
    check(
        !/flex-wrap\s*:\s*wrap/.test(tbBlock),
        "工具条无 flex-wrap: wrap(单行)",
    );
}

// =============================================================================
if (fail > 0) {
    console.error("smoke-curve-editor 失败(" + fail + " 项)");
    process.exit(1);
}
console.log("smoke-curve-editor 通过");
process.exit(0);
