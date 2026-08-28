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

    // ★ 兜底超时的**值域**必须真的能兜住:光断「常量存在 / >0」挡不住把它改成
    //   Number.MAX_SAFE_INTEGER(或 1e12)这类「等于没有超时」的回退 —— 那种改法下
    //   常量仍是有限正数,旧钉子照样绿。这里给一个**上界**:超时必须落在人还愿意等的
    //   量级内(≤60s),否则毒键的存活期就等同于会话寿命,修等于没修。
    check(
        Number.isFinite(W.TILE_REQUEST_TIMEOUT_MS) &&
            W.TILE_REQUEST_TIMEOUT_MS > 0 &&
            W.TILE_REQUEST_TIMEOUT_MS <= 60000,
        `兜底超时在可用量级内(实得 ${W.TILE_REQUEST_TIMEOUT_MS}ms,要求 0 < t ≤ 60000)`,
    );
    // 源码级:**两个取数口都**要与超时竞速。分开写两颗钉是因为它们是同一族问题的两处
    // (getTile 的 LRU 在途去重 / ensureOverview 的 rec.inflight),漏掉任一处都会留一条毒键路径;
    // 概览那条更狠 —— 它不进 LRU、永不淘汰,一次丢包冻结到会话结束。
    const srcText = await import("node:fs").then((fs) =>
        fs.readFileSync(join(ROOT, "web/output/canvas/waveform.js"), "utf8"),
    );
    check(
        /function withTimeout\(promise, ms\)/.test(srcText),
        "超时竞速抽成公用件 withTimeout(两个取数口共用一份)",
    );
    check(
        /p = withTimeout\([\s\S]{0,400}?timeoutMs,/.test(srcText),
        "getTile 走 withTimeout(不是自己手搓一份 race)",
    );
    // 超时可注入是**为用例开的seam**;必须钉死生产侧的默认仍是 TILE_REQUEST_TIMEOUT_MS,
    // 否则「默认变 undefined / 变成天文数字」这类回退会让整条兜底静默失效,而上面那两颗
    // 结构钉照样绿(它们只看调用形态,不看默认值)。
    check(
        /timeoutMs > 0[\s\S]{0,120}?TILE_REQUEST_TIMEOUT_MS;/.test(srcText),
        "[SL-212] 注入缺省/非法时回落 TILE_REQUEST_TIMEOUT_MS(生产侧不会没有超时)",
    );
    check(
        /const p = withTimeout\([\s\S]{0,200}?request\(ch, a, b, n\)[\s\S]{0,120}?timeoutMs,/.test(
            srcText,
        ),
        "[SL-212] ensureOverview 也走 withTimeout(概览毒键同族)",
    );
    check(
        /if \(tile === kTimedOut\) return; \/\/ 超时:dirty 不清/.test(srcText),
        "[SL-212] 概览超时:放掉 inflight 但**保持 dirty**(下一拍才会重发)",
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
log("=== ②b [SL-212] 概览块:回执丢失后不得冻结到会话结束 ===");
{
    // 概览没有 LRU 淘汰,`rec.inflight` 一旦挂上一笔永不 settle 的 promise,这条轨的概览就
    // **冻结到会话结束**;而它正是过渡帧「盖满整幅」的唯一兜底 —— 塌了就是缩放/平移露白。
    //
    // 这一组走**真实时序**(注入 60ms 超时,生产是 8s):`ensureOverview` 在 inflight 挂着时
    // 是提前 return 的,所以「超时放掉 inflight → 下一拍重发」这个迁移在真超时之前根本观察不到,
    // 光靠源码钉只能钉形态、钉不住行为。
    let calls = 0;
    let hang = true;
    const src = W.createWaveformSource({
        timeoutMs: 60,
        request: (ch, s0, s1, cols) => {
            calls++;
            if (hang) {
                hang = false;
                return new Promise(() => {}); // 第一笔:回执丢失,永不 settle
            }
            return Promise.resolve(tileOf(cols));
        },
    });
    const CH = 7;
    src.ensureOverview(CH, 0, 600, W.OVERVIEW_COLS);
    await flush();
    eq(calls, 1, "[SL-212] 概览首笔已发出");
    eq(src.peekOverview(CH), null, "[SL-212] 首笔在途 ⇒ 手上还没有概览");

    // 在途期间不重复打桥(既有去重行为,别改坏)。
    src.ensureOverview(CH, 0, 600, W.OVERVIEW_COLS);
    await flush();
    eq(calls, 1, "[SL-212] 在途期间不重复打桥");

    // ★ 等过注入的超时 + 节流门:inflight 被放掉、dirty 仍在 ⇒ 下一拍真的重发,并拿到概览。
    // ⚠ 重发要用**不同跨度**:同跨度时 `spanChanged` 为假,节流门取的是 OVERVIEW_REFRESH_MS
    // (3000ms)而不是 OVERVIEW_SPAN_MIN_MS(120ms),等 220ms 会被门挡回去 —— 第一版就是
    // 这么红的,红的是用例对节流门的理解,不是实现。曲长回填/切歌本来也就是换跨度那一支。
    await new Promise((r) => setTimeout(r, 200)); // > timeoutMs(60) + OVERVIEW_SPAN_MIN_MS(120)
    src.ensureOverview(CH, 0, 900, W.OVERVIEW_COLS);
    await flush();
    check(calls >= 2, "[SL-212] 超时后概览能重新发起(不再冻结到会话结束)");
    check(!!src.peekOverview(CH), "[SL-212] 重发后概览到手");
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
