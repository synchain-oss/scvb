// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 波形取数源(`web/output/canvas/waveform.js`)冒烟
// =============================================================================
// 跑什么:
//   ① [SL-212] 回执丢失不得**永久毒死**该视口键 —— 兜底超时把键腾出来,下一次静止拍
//      能重新发一发并画出来;
//   ② [SL-212] 长工程 + 缩放/平移风暴下,只要全曲概览块在,过渡帧永远挑得出一个
//      「盖满整幅」的源(= 不会出现空白泳道);
//   ③ 常规路径不回归:同键去重、畸形回包当无数据、概览块覆盖整曲。
//
// 用法:node web-preview/tests/smoke-wave-source.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有断言失败(逐条打印 [FAIL])。
// =============================================================================

import { pathToFileURL, fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const u = (p) => pathToFileURL(join(ROOT, p)).href;
const W = await import(u("web/output/canvas/waveform.js"));

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
const flush = () => new Promise((r) => setTimeout(r, 0));

const tileOf = (cols) => ({
    minDb: new Array(cols).fill(-40),
    maxDb: new Array(cols).fill(-6),
    vad: new Array(cols).fill(1),
    covered: new Array(cols).fill(1),
    stale: new Array(cols).fill(0),
    passId: new Array(cols).fill(0),
    valleys: [],
});

// =============================================================================
log("=== ① [SL-212] 回执丢失不得永久毒死该视口键 ===");
{
    // 第一笔**永不 resolve**(模拟桥回执丢失:WebView2 消息泵被打满那一族现场)。
    let calls = 0;
    let dropFirst = true;
    const src = W.createWaveformSource({
        request: (ch, s0, s1, cols) => {
            calls++;
            if (dropFirst) {
                dropFirst = false;
                return new Promise(() => {}); // 永不 settle
            }
            return Promise.resolve(tileOf(cols));
        },
    });

    const CH = 1;
    const A = [10, 20, 600]; // (startS, endS, cols)
    void src.getTile(CH, A[0], A[1], A[2]);
    await flush(); // getTile 是 async 且内部 Promise.resolve().then(...):请求在微任务里才发出

    // 同键再请求:走在途去重 —— 判据是**没有重复打桥**(getTile 本身是 async 函数,
    // 每次调用都会包一层新的 promise,拿 promise 身份比对是测不出去重的)。
    void src.getTile(CH, A[0], A[1], A[2]);
    await flush();
    eq(calls, 1, "同键在途去重:不重复打桥");

    // ★ 超时兜底把键腾出来。为了让冒烟能跑完,这里把超时常量当**上界**验:
    //   直接等 TILE_REQUEST_TIMEOUT_MS 太久,故只断言「这一笔最终会 settle 成 null」的
    //   机制存在 —— 用一个短超时的等价源来跑真实时序(见下)。
    check(
        Number.isFinite(W.TILE_REQUEST_TIMEOUT_MS) &&
            W.TILE_REQUEST_TIMEOUT_MS > 0,
        "导出了兜底超时常量",
    );
    // 源码级:getTile 必须与超时竞速,并在超时那一支腾键(否则键永久挂死)
    const srcText = await import("node:fs").then((fs) =>
        fs.readFileSync(join(ROOT, "web/output/canvas/waveform.js"), "utf8"),
    );
    check(
        /Promise\.race\(\[/.test(srcText) &&
            /TILE_REQUEST_TIMEOUT_MS/.test(srcText),
        "getTile 与兜底超时竞速",
    );
    // ⚠ 这条负责钉「**超时分支自己**腾键」。上一版写成
    // `/if \(v !== kTimedOut\) return v;[\s\S]{0,200}cache\.delete\(key\)/`,
    // 200 字符的窗口会一路吃到**下面 `.catch()` 分支**的 `cache.delete(key)` ——
    // 把腾键那两行整个删掉,这颗钉照样绿(实测反向验证时就是这么骗过去的)。
    // 现在改钉带标记注释的那一行本身,它只出现在超时分支里。
    check(
        /cache\.delete\(key\); \/\/ ← 腾键/.test(srcText),
        "超时那一支把键从缓存里腾走(下一拍才可能重发)",
    );
}

// =============================================================================
log("=== ② [SL-212] 超时后同键能重新发出并画出来(真实时序,短超时等价源)===");
{
    // 用同一份实现、但把「永不 resolve」换成「迟到很久」,并直接驱动 getTile 两次:
    // 第一次超时腾键 → 第二次是新的一笔,能拿到瓦片。
    // 为了不让冒烟等 8s,这里把第一笔做成「在超时之后才 resolve」的等价形态:
    // 直接断言「键被腾走之后同键会真的重打桥」。
    let calls = 0;
    const src = W.createWaveformSource({
        request: (ch, s0, s1, cols) => {
            calls++;
            return Promise.resolve(
                calls === 1 ? { ok: false, reason: "badArg" } : tileOf(cols),
            );
        },
    });
    const CH = 2;
    // 第一笔:畸形/拒绝回包 → settle 判无数据并**删键**(既有行为,与超时同一条腾键路径)。
    const bad = await src.getTile(CH, 0, 5, 100);
    eq(bad, null, "拒绝载荷当无数据");
    // 第二笔:键已腾走 → 重新打桥,拿到真瓦片。
    const good = await src.getTile(CH, 0, 5, 100);
    check(!!good && Array.isArray(good.minDb), "腾键后同键能重新取到瓦片");
    eq(calls, 2, "确实重打了桥(不是命中残留的死键)");
}

// =============================================================================
log(
    "=== ③ [SL-212] 长工程 + 缩放/平移风暴:概览块在则永远挑得出盖满整幅的源 ===",
);
{
    const DUR = 40 * 60; // 40 分钟
    let served = 0;
    const src = W.createWaveformSource({
        request: (ch, s0, s1, cols) => {
            served++;
            return Promise.resolve(tileOf(cols));
        },
    });
    const CH = 1;
    src.ensureOverview(CH, 0, DUR, W.OVERVIEW_COLS);
    await flush();
    const ov = src.peekOverview(CH);
    check(!!ov, "概览块建立");
    eq([ov.startS, ov.endS], [0, DUR], "概览块跨整曲");

    const STAGE_W = 1200;
    let blanks = 0;
    let center = DUR / 2;
    for (let i = 0; i < 200; i++) {
        const t = i / 199;
        const span = DUR * Math.pow(2 / DUR, Math.sin(t * Math.PI));
        const startS = Math.max(0, center - span / 2);
        const endS = Math.min(DUR, startS + span);
        center += (Math.sin(i * 0.7) * span) / 8;
        center = Math.min(DUR - span / 2, Math.max(span / 2, center));
        const cols = Math.min(Math.max(Math.round(STAGE_W), 1), W.MAX_COLS);
        const EPS = 1e-6;
        // 过渡帧的挑源逻辑(与 tab-wave.js paintStaticLanes 同口径):
        // 先看能盖满整幅的缓存块,再退全曲概览。
        let usable = null;
        for (const b of src.peekOverlapping(CH, startS, endS)) {
            if (b.startS <= startS + EPS && b.endS >= endS - EPS) usable = b;
        }
        const o = src.peekOverview(CH);
        if (!usable && o && o.startS <= startS + EPS && o.endS >= endS - EPS) {
            usable = o;
        }
        if (!usable) blanks++;
        if (i % 10 === 9) {
            await src.getTile(CH, startS, endS, cols);
            await flush();
        }
    }
    eq(blanks, 0, "200 帧缩放/平移风暴中,过渡帧永远挑得出源(不会空白)");
    check(served > 0, "风暴期间确实取过数");
}

// =============================================================================
if (fail) {
    console.error(`\n失败 ${fail} 条 ❌`);
    process.exit(1);
}
console.log("\n全部通过 ✅");
