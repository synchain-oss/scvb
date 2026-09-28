// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— [J147] 宿主速度模型(web/output/host-tempo.js)的逐函数冒烟(node,无 DOM)
// =============================================================================
// 页面级接线归 `smoke-range-bars-page.mjs`;这里钉的是换算与判定本身 —— 每一格只改一个输入,
// 期望值都是手算的(写在格旁),不是跑一遍抄下来的。
//
// 删除式(未提交,人工核过;读数见 PR 描述)—— 每一处落点各一格:
//   D1  删掉 barBeatAt 的毫秒吸附           ⇒ ⑤「103 BPM 下 9.320 s 仍是 5.1」红
//   D2  observeTempo 不看 bpm 变化           ⇒ ③「只有 bpm 变(偏移恰好不变)」红
//   D3  observeTempo 不看偏移变化           ⇒ ④「偏移从 0 变到 −5 拍」红
//   D4  第一帧偏移不与 0 比                 ⇒ ④「第一帧偏移 +2 拍」红
//   D5  observeTempo 不看拍号变化           ⇒ ⑥ 红
//   D6  qnAt 的 exact 不认锚点(校准失效)    ⇒ ③「离 10 s 锚点 0.2 s ⇒ 已校准」红
//   D7  锚点表不设上限                       ⇒ ⑦ 红
//   D8  readTempoFields 不查拍号分子         ⇒ ①「分子 0」「分子非整数」红
//   D8b readTempoFields 不查拍号分母         ⇒ ①「分母 0」「缺分母」红
//   D9  stepByBars 不按拍号折(恒按 4/4)     ⇒ ② 的 3/4、6/8 两格红
//   D10 校准窗不设上界(最近锚点就算)        ⇒ ③「离锚点 0.3 s ⇒ 仍是估算」红
//   D11 不认「改了速度表」(判定恒假)          ⇒ ⑧ ①②③ 各格红
//   D12 外推基点用 latest 而不是 latestPpq     ⇒ ⑨ 红(12 s 退回 7.1)
//
// 用法:node web-preview/tests/smoke-host-tempo.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有断言失败。
// =============================================================================

import { pathToFileURL, fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const HT = await import(
    pathToFileURL(join(ROOT, "web/output/host-tempo.js")).href
);

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
const near = (a, b, msg, tol = 1e-9) =>
    check(
        Number.isFinite(a) && Math.abs(a - b) <= tol,
        `${msg}: 实得 ${a},期望 ${b}`,
    );

/** 一帧 `scvb.playhead`(只带本模块关心的字段)。 */
const frame = (timeS, bpm, num, den, ppq) => {
    const p = { timeS, isPlaying: false, inRange: true };
    if (bpm !== undefined) p.bpm = bpm;
    if (num !== undefined) p.timeSigNum = num;
    if (den !== undefined) p.timeSigDen = den;
    if (ppq !== undefined) p.ppq = ppq;
    return p;
};
/** 恒速、原点在 0 秒的一帧:ppq = 秒 × bpm / 60。 */
const steady = (timeS, bpm = 120, num = 4, den = 4) =>
    frame(timeS, bpm, num, den, (timeS * bpm) / 60);
const fed = (...frames) => {
    let m = HT.emptyTempo();
    for (const f of frames) m = HT.observeTempo(m, f);
    return m;
};
const bb = (m, t) => HT.formatBarBeat(HT.barBeatAt(m, t));

// =============================================================================
log("=== ① 没有速度:一切换算回 null,页面按秒 ===");
{
    const m = HT.emptyTempo();
    eq(HT.hasTempo(m), false, "空模型没有速度");
    eq(HT.qnAt(m, 10), null, "空模型 qnAt = null");
    eq(HT.barBeatAt(m, 10), null, "空模型 barBeatAt = null");
    eq(HT.stepByBars(m, 10, 4), null, "空模型 stepByBars = null(调用方按秒挪)");

    // 字段不全 / 不合法的帧一律当「这一帧没给」
    for (const [label, f] of [
        ["缺 bpm", frame(1, undefined, 4, 4, 2)],
        ["缺拍号", frame(1, 120, undefined, undefined, 2)],
        ["缺分母", frame(1, 120, 4, undefined, 2)],
        ["bpm=0", frame(1, 0, 4, 4, 2)],
        ["bpm=NaN", frame(1, NaN, 4, 4, 2)],
        ["分子 0", frame(1, 120, 0, 4, 2)],
        ["分母 0", frame(1, 120, 4, 0, 2)],
        ["分子非整数", frame(1, 120, 3.5, 4, 2)],
        ["null 帧", null],
    ]) {
        eq(HT.readTempoFields(f), null, `readTempoFields 拒「${label}」`);
        eq(HT.hasTempo(fed(f)), false, `「${label}」的帧喂进去后仍没有速度`);
    }

    // J147「停带时用最后一次读到的值」:收到过速度之后,缺字段的帧不清掉它
    const m2 = fed(steady(42), frame(42.5));
    eq(HT.hasTempo(m2), true, "收到过速度后,缺字段的帧不清掉它");
    near(m2.latest.bpm, 120, "沿用的是最后一次读到的 bpm");
}

// =============================================================================
log("=== ② 恒速:小节换算与 ±4 小节 ===");
{
    const m = fed(steady(42));
    // 120 BPM 4/4:一小节 2 s。12 s = 24 拍 = 第 7 小节第 1 拍;96 s = 192 拍 = 49.1
    eq(bb(m, 12), "7.1", "12 s @120 4/4");
    eq(bb(m, 96), "49.1", "96 s @120 4/4");
    eq(bb(m, 0), "1.1", "0 s = 1.1");
    eq(bb(m, 1.6), "1.4", "1.6 s = 第 3.2 拍 ⇒ 1.4");
    eq(HT.barBeatAt(m, 96).exact, true, "恒速远处也是精确值");
    near(HT.stepByBars(m, 96, 4), 104, "+4 小节 = +8 s");
    near(HT.stepByBars(m, 96, -4), 88, "−4 小节 = −8 s");

    // 3/4 @90:一拍 2/3 s,一小节 2 s;4 s = 6 拍 = 3.1
    const m34 = fed(steady(0, 90, 3, 4));
    eq(bb(m34, 4), "3.1", "4 s @90 3/4");
    near(HT.stepByBars(m34, 0, 4), 8, "3/4 @90:+4 小节 = 12 拍 = 8 s");

    // 6/8 @120:拍按八分音符,一拍 0.25 s,一小节 1.5 s;3 s = 12 个八分 = 3.1
    const m68 = fed(steady(0, 120, 6, 8));
    eq(bb(m68, 3), "3.1", "3 s @120 6/8");
    eq(bb(m68, 3.25), "3.2", "3.25 s @120 6/8");
    near(HT.stepByBars(m68, 0, 4), 6, "6/8 @120:+4 小节 = 6 s");

    // 宿主给了速度没给拍位置:按原点 0 秒推
    const noPpq = fed(frame(42, 120, 4, 4));
    eq(bb(noPpq, 12), "7.1", "没有 ppq 时按原点 0 秒推");

    // 拍位置原点不在 0(用宿主的拍位置,不自己按秒推):42 s 处报 86 拍(偏移 +2 拍)
    const off = fed(frame(42, 120, 4, 4, 86));
    eq(
        bb(off, 42),
        "22.3",
        "42 s 处 86 拍 ⇒ 22.3(按宿主拍位置,不是按秒推的 22.1)",
    );
}

// =============================================================================
log("=== ③ 速度变过:远处是估算,锚点 0.25 s 内算已校准 ===");
{
    // 10 s 处 120 BPM(20 拍),之后宿主报 100 BPM(30 s 处 20+20×100/60≈53.33 拍)
    const m = fed(
        frame(10, 120, 4, 4, 20),
        frame(30, 100, 4, 4, 20 + (20 * 100) / 60),
    );
    eq(m.tempoVaried, true, "bpm 变过 ⇒ tempoVaried");
    eq(HT.barBeatAt(m, 60).exact, false, "远离两个锚点 ⇒ 估算");
    eq(HT.barBeatAt(m, 10.2).exact, true, "离 10 s 锚点 0.2 s ⇒ 已校准");
    eq(HT.barBeatAt(m, 9.8).exact, true, "锚点另一侧 0.2 s ⇒ 已校准");
    eq(HT.barBeatAt(m, 10.3).exact, false, "离锚点 0.3 s ⇒ 仍是估算");
    // 已校准的点按**那个锚点**的速度换算:10.2 s = 20 + 0.2×2 = 20.4 拍 ⇒ 6.1
    near(HT.qnAt(m, 10.2).qn, 20.4, "10.2 s 按 10 s 锚点(120 BPM)换算");
    // 估算的点按**最近一次**的速度:60 s = 53.33 + 30×100/60 = 103.33 拍
    near(
        HT.qnAt(m, 60).qn,
        20 + (50 * 100) / 60,
        "60 s 按最近的 100 BPM 外推",
        1e-9,
    );
    // ±4 小节按该点换算所用的速度折秒:估算点用 100 BPM ⇒ 16 拍 = 9.6 s
    near(HT.stepByBars(m, 60, 4), 69.6, "估算点 +4 小节按最近 BPM 折 9.6 s");
    near(HT.stepByBars(m, 10.2, 4), 18.2, "校准点 +4 小节按锚点 BPM 折 8 s");

    // 只有 bpm 变、偏移恰好不变(两帧的 ppq 都等于 秒 × 各自 bpm / 60)也算变过 ——
    // 这一格单独钉「看 bpm」那一条,不让「看偏移」那一条替它兜着。
    const bpmOnly = fed(steady(10, 120), steady(30, 100));
    eq(bpmOnly.tempoVaried, true, "只有 bpm 变(偏移恰好不变)⇒ tempoVaried");
}

// =============================================================================
log("=== ④ 同一个 bpm、偏移变了 ⇒ 中间夹着别的速度 ===");
{
    // 两处都是 120 BPM,但 30 s 处报 55 拍(恒速应为 60):中间有过更慢的一段
    const m = fed(frame(10, 120, 4, 4, 20), frame(30, 120, 4, 4, 55));
    eq(m.tempoVaried, true, "偏移从 0 变到 −5 拍 ⇒ tempoVaried");
    // 对照:同样两处、偏移不变 ⇒ 不算变过
    const c = fed(steady(10), steady(30), steady(30.033), steady(12.5));
    eq(c.tempoVaried, false, "对照:恒速跳来跳去(偏移恒 0)不算变过");
    // 第一帧偏移就不为 0(前面有过别的速度 / 原点不在 0 秒)⇒ 按估算处理
    const first = fed(frame(42, 120, 4, 4, 86));
    eq(first.tempoVaried, true, "第一帧偏移 +2 拍 ⇒ tempoVaried");
    eq(HT.barBeatAt(first, 96).exact, false, "…于是远处是估算");
    eq(HT.barBeatAt(first, 42.1).exact, true, "…停着的那一点附近是已校准");
    // 容差:偏移抖 0.005 拍(< 0.01)不算
    const jitter = fed(steady(10), frame(20, 120, 4, 4, 40.005));
    eq(jitter.tempoVaried, false, "偏移抖 0.005 拍不算变过");
}

// =============================================================================
log("=== ⑤ 毫秒吸附:落在拍线上的端点经毫秒取整后不掉到前一拍 ===");
{
    // 103 BPM:16 拍 = 9.3204 s,输入框只到毫秒 ⇒ 9.320 s ⇒ 15.99933 拍。不吸附会显示 4.4。
    const m = fed(steady(0, 103));
    eq(bb(m, 9.32), "5.1", "103 BPM 下 9.320 s 仍是 5.1");
    // 吸附只在 1 ms 之内:差 5 ms 的点照实显示前一拍
    eq(bb(m, 9.315), "4.4", "差 5 ms 不吸附");
}

// =============================================================================
log("=== ⑥ 拍号变过:一律估算,锚点也校准不了 ===");
{
    const m = fed(steady(10, 120, 4, 4), steady(30, 120, 3, 4));
    eq(m.meterVaried, true, "拍号变过 ⇒ meterVaried");
    eq(m.tempoVaried, false, "对照:速度没变");
    eq(
        HT.barBeatAt(m, 30.1).exact,
        false,
        "锚点旁边也是估算(小节号依赖拍号历史)",
    );
}

// =============================================================================
log("=== ⑦ 锚点表有上限,超了丢最久没更新的 ===");
{
    let m = HT.emptyTempo();
    const n = HT.MAX_ANCHORS + 100;
    for (let i = 0; i < n; i++)
        m = HT.observeTempo(m, steady(i * HT.ANCHOR_BUCKET_S));
    eq(m.anchors.size, HT.MAX_ANCHORS, "锚点数封顶在 MAX_ANCHORS");
    eq(m.anchors.has(0), false, "最早的桶已被丢掉");
    eq(m.anchors.has(n - 1), true, "最新的桶还在");
    // 同一个桶反复更新只占一个位置
    let m2 = HT.emptyTempo();
    for (let i = 0; i < 50; i++)
        m2 = HT.observeTempo(m2, steady(5 + i * 0.001));
    eq(m2.anchors.size, 1, "同一桶里 50 帧只留一个锚点");
}

// =============================================================================
log("=== ⑧ 停着时宿主改了速度表 ⇒ 旧锚点与旧判断整份作废(PR #325 复审①)===");
{
    // 先按 120 BPM 从 0 播到 20 s(每 33 ms 一帧,锚点铺满),再停在 20 s 上。
    const played = () => {
        let m = HT.emptyTempo();
        for (let t = 0; t <= 20; t += 0.033) m = HT.observeTempo(m, steady(t));
        for (let i = 0; i < 3; i++) m = HT.observeTempo(m, steady(20));
        return m;
    };
    const base = played();
    eq(HT.barBeatAt(base, 10).exact, true, "前提:改之前 10 s 是精确的 6.1");
    eq(bb(base, 10), "6.1", "前提:10 s @120 = 20 拍 = 6.1");

    // ① 全曲改成 100 BPM:停着的 20 s 处拍位置 40 → 33.33、速度 120 → 100
    const m = HT.observeTempo(played(), steady(20, 100));
    eq(m.anchors.size, 1, "① 旧锚点清空,只剩这一帧");
    eq(m.tempoVaried, false, "① 改完是恒速 100(偏移为 0)⇒ 不算变过");
    // 10 s @100 = 16.67 拍 ⇒ 5.1(旧锚点会给出 6.1 且标成精确)
    eq(bb(m, 10), "5.1", "① 10 s 按新速度换算 = 5.1");
    eq(HT.barBeatAt(m, 10).exact, true, "① 新速度表下恒速 ⇒ 精确");

    // ② 前面插了一段变速:20 s 处速度不变、拍位置从 40 变成 38
    const m2 = HT.observeTempo(played(), frame(20, 120, 4, 4, 38));
    eq(m2.anchors.size, 1, "② 拍位置变了 ⇒ 旧锚点清空");
    eq(m2.tempoVaried, true, "② 新的第一帧偏移 −2 拍 ⇒ 变过");
    eq(HT.barBeatAt(m2, 10).exact, false, "② 10 s 不再被旧锚点判成已校准");

    // ③ 只在停着的这一点改了速度(拍位置不变):20 s 处 120 → 100
    const m3 = HT.observeTempo(played(), frame(20, 100, 4, 4, 40));
    eq(m3.anchors.size, 1, "③ 只有速度变了也算改过 ⇒ 旧锚点清空");
    eq(HT.barBeatAt(m3, 10).exact, false, "③ 10 s 不再被旧锚点判成已校准");

    // 对照:播放中穿过变速点(每帧时刻都在走)不算改速度表 —— 锚点保留
    let c = HT.emptyTempo();
    for (let t = 0; t <= 10; t += 0.033) c = HT.observeTempo(c, steady(t));
    for (let t = 10.033; t <= 12; t += 0.033)
        c = HT.observeTempo(c, frame(t, 100, 4, 4, 20 + ((t - 10) * 100) / 60));
    check(
        c.anchors.size > 40,
        `对照:播放中变速不清锚点(实得 ${c.anchors.size} 个)`,
    );
    eq(HT.barBeatAt(c, 5).exact, true, "对照:5 s 仍是已校准");
    // 对照:停着、同一帧重复到来(宿主停带时逐帧重发)不算改
    // (0–20 s、0.25 s 一桶 ⇒ 81 个锚点)
    const same = HT.observeTempo(played(), steady(20));
    eq(same.anchors.size, 81, "对照:停着重复收到同一帧不清锚点");
}

// =============================================================================
log("=== ⑨ 没有时间线的帧不替换外推基点(PR #325 复审②)===");
{
    // 42 s 处宿主报 86 拍(原点偏 +2 拍);之后来一帧没时间线的(timeS 填 0、不带 ppq)。
    // 取 12 s(离 42 s 的锚点远,走的是外推那一支):86 − 30 × 2 = 26 拍 ⇒ 7.3;
    // 退回「原点在 0 秒」的话是 24 拍 ⇒ 7.1。
    const m = fed(frame(42, 120, 4, 4, 86), frame(0, 120, 4, 4));
    eq(bb(m, 12), "7.3", "外推仍从 42 s / 86 拍出发(不退回按秒推的 7.1)");
    eq(HT.hasTempo(m), true, "速度仍在");
}

if (fail > 0) {
    console.log(`\n❌ ${fail} 条断言失败`);
    process.exit(1);
}
console.log("\n✅ host-tempo(J147)逐函数冒烟全绿");
process.exit(0);
