// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— [J146] 拖动档 VAD/边界预览(契约 §1.18 / §2.10)node 侧冒烟(无 DOM)
// -----------------------------------------------------------------------------
// 跑什么:
//   ① 纯函数 `normalizeVadPreview` / `previewEdgesOf`(tab-wave.js):畸形载荷一律当「不在预览中」;
//      相接两段共用一条边;用户段(origin≠auto 或 locked,含 openEnded)内部的边不画、端点照画;
//   ② mock 后端与 native 同形的**时序与生命周期**(§2.10「何时发 / 何时结束」):
//        · setVadParams / setSegmentation 每次调用当场发一帧 active:true,seq 单调;
//        · 预览期间 requestWaveform 的 vad 列跟着预览走(§1.27 口径补写),参数有牙;
//        · 松手防抖那一趟落地 ⇒ 先到 §2.8 段表、再到 active:false;
//        · 真切版本(丢弃事件)⇒ 立刻 active:false;
//        · 没人接手(分析被取消 ⇒ 防抖被抑制、也没有在跑的分析)⇒ 空闲 1.5s 后 active:false;
//   ③ **C++ 接线的源码钉子**:`OutputEditor` 编不进任何测试目标(依赖 WebView2),host 用例
//      只能打到 processor 那一半 —— 「handler 真的调了 requestVadPreview 并当场发事件」
//      「emitTick 真的补发收尾帧」这两跳只能在这里按源码钉(与 smoke-tab3 ⑮(f) 同一做法)。
//      删除式见 PR 描述(D-S1..D-S3)。
//   ④ [SL-561] 按住保活:`SLIDER_HOLD_KEEPALIVE_MS` 与 native / mock 的松手档防抖常量按源码对拍
//      (2 × 周期 ≤ 防抖);mock 上以保活周期同值重发 2s 不起跑、预览不收,停发后照常落地。
//      按住期间页面真的在发、真的一直有点划线,在页面级那套(smoke-vad-preview-page.mjs (f))。
//
// 用法:node web-preview/tests/smoke-vad-preview.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有断言失败(逐条打印 [FAIL])。
// =============================================================================

import { readFileSync } from "node:fs";
import { pathToFileURL, fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const u = (p) => pathToFileURL(join(ROOT, p)).href;

const TW = await import(u("web/output/tab-wave.js"));
const { createBridge } = await import(u("web/shared/bridge.js"));
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
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// -----------------------------------------------------------------------------
log("=== ① 纯函数 ===");
{
    const { normalizeVadPreview: norm, previewEdgesOf: edges } = TW;
    check(
        typeof norm === "function" && typeof edges === "function",
        "tab-wave.js 导出 normalizeVadPreview / previewEdgesOf",
    );
    for (const bad of [
        null,
        {},
        { seq: 3, active: "yes" },
        { seq: 3, active: true }, // 缺窗
        { seq: 3, active: true, startS: NaN, endS: 2 },
    ]) {
        const n = norm(bad);
        check(
            n.active === false && n.byCh.size === 0,
            `畸形载荷当「不在预览中」:${JSON.stringify(bad)}`,
        );
    }
    const ok = norm({
        seq: 7,
        active: true,
        startS: 0,
        endS: 10,
        channels: [
            {
                ch: 2,
                spans: [
                    { t0S: 1, t1S: 2 },
                    { t0S: 3, t1S: 2 }, // 倒挂:丢
                    { t0S: 4, t1S: NaN }, // 非有限:丢
                ],
            },
            { ch: 16, spans: [{ t0S: 1, t1S: 2 }] }, // 越界轨号:丢
            { ch: 3, spans: [] }, // 空数组 = 该轨判为无声(保留,虚影清空)
        ],
    });
    check(
        ok.active && ok.seq === 7 && ok.byCh.size === 2,
        `合法载荷收下两条轨(实得 ${ok.byCh.size})`,
    );
    check(
        (ok.byCh.get(2) || []).length === 1 &&
            (ok.byCh.get(3) || []).length === 0,
        "逐段清洗:倒挂 / 非有限的段丢掉,空数组原样保留",
    );

    const spans = [
        { t0S: 1, t1S: 3 },
        { t0S: 3, t1S: 5 }, // 与上一段相接(谷切分)⇒ 3 只画一条
        { t0S: 8, t1S: 9 },
    ];
    check(
        JSON.stringify(edges(spans, [])) === JSON.stringify([1, 3, 5, 8, 9]),
        `相接两段共用一条边(实得 ${JSON.stringify(edges(spans, []))})`,
    );
    const user = [
        { t0S: 2.5, t1S: 6, origin: "user_edited", locked: true },
        { t0S: 7, t1S: 8, origin: "auto", locked: false }, // auto 段:不挡
    ];
    check(
        JSON.stringify(edges(spans, user)) === JSON.stringify([1, 8, 9]),
        `用户段内部的边不画(3、5 在 [2.5,6) 内),auto 段不挡(实得 ${JSON.stringify(edges(spans, user))})`,
    );
    const onEdge = [{ t0S: 3, t1S: 4, origin: "auto", locked: true }];
    check(
        JSON.stringify(edges(spans, onEdge)) ===
            JSON.stringify([1, 3, 5, 8, 9]),
        "恰好落在用户段端点上的照画(locked auto 段也算用户段,但 3 是它的端点)",
    );
    const open = [
        { t0S: 4, t1S: 4.5, origin: "user_created", openEnded: true },
    ];
    check(
        JSON.stringify(edges(spans, open)) === JSON.stringify([1, 3]),
        `openEnded 用户段按 +∞ 处理(实得 ${JSON.stringify(edges(spans, open))})`,
    );
}

// -----------------------------------------------------------------------------
log("=== ② mock 生命周期(与 native 同形)===");
async function session() {
    const s = driver.createPreviewSession({ role: "output", params: "" });
    const bridge = createBridge({ role: "output", mockBackend: s.mock });
    const pv = [];
    const seg = [];
    bridge.on("scvb.vadPreview", (p) => pv.push({ p, at: seg.length }));
    bridge.on("scvb.segments", (e) => seg.push(e.reason));
    const snap = await bridge.requestInitialState();
    return { s, bridge, pv, seg, snap };
}
const voiced = (tile) => (tile.vad || []).reduce((n, v) => n + (v ? 1 : 0), 0);

{
    const { s, bridge, pv, seg, snap } = await session();
    const vad0 = { ...snap.analysis.vad };
    const CH = 1;
    const tile0 = await bridge.requestWaveform(CH, 0, 300, 600);
    check(voiced(tile0) > 0, `前置:基线 vad 列有有声列(实得 ${voiced(tile0)})`);

    await bridge.setVadParams({ ...vad0, threshold_db: -15 });
    const f1 = pv[pv.length - 1];
    check(
        !!f1 && f1.p.active === true && f1.p.seq === 1,
        `(a) setVadParams 当场发 active:true(实得 ${JSON.stringify(f1 && { a: f1.p.active, s: f1.p.seq })})`,
    );
    const ch1 = f1 && (f1.p.channels || []).find((c) => c.ch === CH);
    check(!!ch1 && ch1.spans.length > 0, "(a) 载荷里有写回集轨与它的 S1 段");
    check(
        !!f1 &&
            Number.isFinite(f1.p.startS) &&
            Number.isFinite(f1.p.endS) &&
            f1.p.endS > f1.p.startS,
        "(a) 载荷带写回窗",
    );
    const tile1 = await bridge.requestWaveform(CH, 0, 300, 600);
    check(
        voiced(tile1) < voiced(tile0),
        `(b) 预览期间 vad 列跟着预览走:门限拉高 ⇒ 有声列变少(${voiced(tile0)} → ${voiced(tile1)})`,
    );

    await bridge.setSegmentation({
        ...snap.analysis.segmentation,
        sensitivity: 100,
    });
    const f2 = pv[pv.length - 1];
    check(
        !!f2 && f2.p.active === true && f2.p.seq === 2,
        "(a) setSegmentation 同样当场发,seq 单调",
    );
    const ch1b = f2 && (f2.p.channels || []).find((c) => c.ch === CH);
    check(
        !!ch1b && ch1b.spans.length > ch1.spans.length,
        `(b) 灵敏度拉满 ⇒ 预览段更碎(${ch1 ? ch1.spans.length : "?"} → ${ch1b ? ch1b.spans.length : "?"})`,
    );

    // 松手:300ms 防抖 → §2.8 段表 → 预览收尾
    await sleep(900);
    const endF = pv[pv.length - 1];
    check(
        seg.includes("segmentation"),
        `(c) 松手那一趟落地发了 §2.8 reason:"segmentation"(实得 ${JSON.stringify(seg)})`,
    );
    check(
        !!endF && endF.p.active === false && endF.p.seq === 3,
        `(c) 落地即收尾:active:false、seq 3(实得 ${JSON.stringify(endF && endF.p)})`,
    );
    check(
        !!endF && endF.at > seg.indexOf("segmentation"),
        "(c) 顺序:先段表、后收尾(UI 不会有一拍两样都没有)",
    );
    const tile2 = await bridge.requestWaveform(CH, 0, 300, 600);
    check(
        JSON.stringify(tile2.vad) === JSON.stringify(tile1.vad),
        `(c) 落地后 vad 列 = 拖动时预览的那一份(与 native「vadP 按同一组参数写」同形;` +
            `有声列 ${voiced(tile2)} vs 预览 ${voiced(tile1)},分析前 ${voiced(tile0)})`,
    );

    // 丢弃事件:真切版本
    await bridge.setVadParams({ ...vad0, threshold_db: -20 });
    check(pv[pv.length - 1].p.active === true, "(d) 前置:又在预览中");
    const cur = snap.global.version_active;
    await bridge.setVersionActive(cur === 1 ? 2 : 1);
    check(
        pv[pv.length - 1].p.active === false,
        "(d) 真切版本 ⇒ 立刻收尾(native:discardPendingResegment)",
    );
    s.ctl.dispose();
}

{
    // 没人接手:点「分析」让防抖被抑制,再取消分析 ⇒ 既没有已排的防抖、也没有在跑的分析。
    const { s, bridge, pv, snap } = await session();
    const r = await bridge.analyze("all");
    check(r && r.ok, "前置:analyze 受理");
    await bridge.setVadParams({ ...snap.analysis.vad, threshold_db: -20 });
    check(pv[pv.length - 1].p.active === true, "(e) 抑制期照样当场出预览");
    await bridge.cancelAnalyze();
    await sleep(900);
    check(
        pv[pv.length - 1].p.active === true,
        "(e) 对照:空闲不到 1.5s 不许早收",
    );
    await sleep(1000);
    check(
        pv[pv.length - 1].p.active === false,
        "(e) 没人接手 ⇒ 空闲 1.5s 后收尾",
    );
    s.ctl.dispose();
}

// -----------------------------------------------------------------------------
log("=== ③ C++ 接线的源码钉子(OutputEditor 编不进测试目标)===");
{
    const src = readFileSync(join(ROOT, "src/output/OutputEditor.cpp"), "utf8");
    // 函数体:从定义行到下一个「行首 }」。
    const body = (sig) => {
        const i = src.indexOf(sig);
        if (i < 0) return null;
        const j = src.indexOf("\n}\n", i);
        return src.slice(i, j < 0 ? src.length : j);
    };
    // 注释不算数:按行剥掉 `//` 之后的部分再判(本文件的桥名里没有 `//`)。
    const code = (s) =>
        (s || "")
            .split("\n")
            .map((l) => {
                const k = l.indexOf("//");
                return k < 0 ? l : l.slice(0, k);
            })
            .join("\n");
    for (const fn of ["handleSetVadParams", "handleSetSegmentation"]) {
        const b = code(body(`void OutputEditor::${fn}(`));
        const iArm = b.indexOf("processor_.armResegment(");
        const iPrev = b.indexOf("processor_.requestVadPreview();");
        const iEmit = b.indexOf("emitVadPreview();");
        const iOk = b.lastIndexOf("c(okResp());");
        check(
            iArm >= 0 && iPrev > iArm && iEmit > iPrev && iOk > iEmit,
            `(D-S1/D-S2) ${fn}:armResegment → requestVadPreview → emitVadPreview → 回执,四步齐且有序` +
                `(实得 arm=${iArm} prev=${iPrev} emit=${iEmit} ok=${iOk};排在 armResegment 前面会让空闲收尾误收第一拍)`,
        );
    }
    const tick = code(body("void OutputEditor::emitTick("));
    check(
        tick.includes("emitVadPreview();"),
        "(D-S3) emitTick 补发「调用之外结束」的收尾帧",
    );
    const emitFn = code(body("void OutputEditor::emitVadPreview("));
    check(
        emitFn.includes("Event::VadPreview") &&
            emitFn.includes("webView().isVisible()"),
        "emitVadPreview 发的是 §2.10 那个名字,且不可见时不推进基线",
    );
    // (D-S4)editor 只读加锁的头 / 快照:宿主可能在别的线程 setStateInformation → 结束预览、
    // swap 掉 spans 的内存,不加锁逐段读会与之竞争。
    check(
        emitFn.includes("processor_.vadPreviewSnapshot()") &&
            emitFn.includes("processor_.vadPreviewHead()") &&
            !code(src).includes("processor_.vadPreview()"),
        "(D-S4) editor 全文件只走加锁的 vadPreviewHead / vadPreviewSnapshot(无锁引用只给 host 用例)",
    );
}

// -----------------------------------------------------------------------------
log(
    "=== ④ [SL-561] 按住保活:周期对拍防抖常量 + mock 上「一直在调用」就不起跑 ===",
);
{
    // 保活周期必须短于松手档防抖,否则按住不动时防抖照样到点(SL-561 的原样)。
    // 界线按机制给:**漏掉一拍(或晚一整拍)仍不到点** ⇒ 2 × 周期 ≤ 防抖。native 的到点检查挂在
    // 25Hz 定时器上(300ms 实际落在 300~340ms),web 定时器与桥也会排队 —— 余量要留给它们。
    const K = TW.SLIDER_HOLD_KEEPALIVE_MS;
    const hdr = readFileSync(
        join(ROOT, "src/output/OutputProcessor.h"),
        "utf8",
    );
    const m = /kResegmentDebounceMs\s*=\s*(\d+)\s*;/.exec(hdr);
    const deb = m ? Number(m[1]) : NaN;
    const mockSrc = readFileSync(
        join(ROOT, "web-preview/mock/juce-bridge-mock.js"),
        "utf8",
    );
    const mm = /function debounceAnalysisPipeline[\s\S]*?later\((\d+),/.exec(
        mockSrc,
    );
    const mockDeb = mm ? Number(mm[1]) : NaN;
    check(
        Number.isFinite(deb) && Number.isFinite(mockDeb),
        `取到 native / mock 的防抖常量(实得 ${deb} / ${mockDeb})`,
    );
    check(
        Number.isFinite(K) && K >= TW.PARAM_THROTTLE_MS,
        `保活周期是有限数且不快于拖动节流(${K}ms ≥ ${TW.PARAM_THROTTLE_MS}ms,≤50Hz 纪律)`,
    );
    check(
        2 * K <= deb && 2 * K <= mockDeb,
        `保活周期 × 2 ≤ 防抖(${K} × 2 ≤ native ${deb} / mock ${mockDeb}):漏一拍仍不到点`,
    );

    // 保活靠的是契约 §1.18「防抖的是调用流」—— **同值重发也重排**。mock 上以保活周期原样重发
    // 2s(> 防抖、> 1.5s 空闲收尾):不许出段表、预览一直 active;停发后照常落地收尾。
    const { s, bridge, pv, seg, snap } = await session();
    const p = { ...snap.analysis.vad, threshold_db: -15 };
    await bridge.setVadParams(p);
    const t0 = Date.now();
    while (Date.now() - t0 < 2000) {
        await sleep(K);
        await bridge.setVadParams({ ...p });
    }
    check(
        seg.length === 0,
        `(f) 同值重发期间不起跑(段表帧 ${JSON.stringify(seg)})`,
    );
    check(
        pv.length > 0 && pv.every((f) => f.p.active === true),
        `(f) 同值重发期间预览一直 active(${pv.length} 帧,收尾帧 ${pv.filter((f) => !f.p.active).length})`,
    );
    await sleep(900);
    const endF = pv[pv.length - 1];
    check(
        seg.includes("vad") && !!endF && endF.p.active === false,
        `(f) 停发后照常落地收尾(段表帧 ${JSON.stringify(seg)};末帧 active=${endF && endF.p.active})`,
    );
    s.ctl.dispose();
}

log(fail === 0 ? "\n全绿" : `\n${fail} 条 FAIL`);
process.exit(fail === 0 ? 0 : 1);
