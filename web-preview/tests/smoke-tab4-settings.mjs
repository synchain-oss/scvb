// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Tab4 设置页接线冒烟(node,无 DOM;T35)
// =============================================================================
// 口径同 smoke-tab1/tab2:仓内零 node_modules(无 jsdom),故断言面是
// **纯函数 + mock 端到端 + 源码级字面断言**三档;DOM 侧(三段选高亮、九条块内展开、
// 诊断折叠、复制剪贴板)归浏览器手测 / 渲染截图。
//
// 跑什么:
//   ① J69 两设置块常量:三档 value/key 单一真源 + 默认档(契约 §1.21 桥面枚举字符串);
//   ② 纯函数:heartbeatAgeText / formatMegabytes / versionString / storageOf /
//     analysisConfigOf / diagRowsOf / diagText;
//   ③ mock 端到端:setAnalysisConfig 写入 + badArg(含 02/03 拼写不互认)+ save/load 往返不丢;
//   ④ 词条:T35 新增 set.* key 三语齐、占位符三语一致、05 §5 禁词零命中;
//   ⑤ 源码级:改任一项 → 置「改后需重分析」stale;九条 = 读取 guide.rule* 生成物零手抄;
//     「查看全部九条」= 块内展开;J45 措辞零命中。
//
// 用法:node web-preview/tests/smoke-tab4-settings.mjs [仓库根绝对路径]
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
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const TS = await import(u("web/output/tab-settings.js"));
const { createBridge } = await import(u("web/shared/bridge.js"));
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

// =============================================================================
log("=== ① J69 两设置块常量(契约 §1.21 桥面枚举字符串)===");
{
    eq(
        TS.LOUDNESS_MODES.map((m) => m.value),
        ["kw_integrated", "rms", "peak_dbfs"],
        "响度口径三档 value",
    );
    eq(
        TS.LOUDNESS_MODES.map((m) => m.key),
        [
            "set.loudnessMode.opt.kw_integrated",
            "set.loudnessMode.opt.rms",
            "set.loudnessMode.opt.peak_dbfs",
        ],
        "响度口径三档 key",
    );
    eq(
        TS.CENTER_SLOT_POLICIES.map((m) => m.value),
        ["priority_queue", "lead_exclusive", "even_spread"],
        "中心槽策略三档 value",
    );
    eq(
        TS.ANALYSIS_CONFIG_DEFAULTS,
        {
            loudness_mode: "kw_integrated",
            center_slot_policy: "priority_queue",
        },
        "默认档 = kw_integrated / priority_queue(02 §4.3/§5.6)",
    );
    // 选项词条与常量一一对拍(单一真源,不得漂移)
    for (const m of [...TS.LOUDNESS_MODES, ...TS.CENTER_SLOT_POLICIES]) {
        for (const lang of ["zh", "en", "fr"]) {
            check(
                typeof T[lang][m.key] === "string" && T[lang][m.key].length > 0,
                `${lang}.${m.key} 非空`,
            );
        }
    }
    // 三语占位符一致性(选项词条无占位符)
    for (const m of [...TS.LOUDNESS_MODES, ...TS.CENTER_SLOT_POLICIES]) {
        const ph = (s) => (String(s).match(/\{\w+\}/g) || []).sort().join(",");
        eq(ph(T.en[m.key]), ph(T.zh[m.key]), m.key + " en/zh 占位符一致");
        eq(ph(T.fr[m.key]), ph(T.zh[m.key]), m.key + " fr/zh 占位符一致");
    }
}

// =============================================================================
log("=== ② 纯函数 ===");
{
    eq(TS.heartbeatAgeText(0xffffffff), "—", "哨兵 → 空");
    eq(TS.heartbeatAgeText(undefined), "—", "缺值 → 空");
    eq(TS.heartbeatAgeText(80), "0.08s", "80ms → 0.08s");
    eq(TS.heartbeatAgeText(4200), "4.2s", "4200ms → 4.2s");
    eq(TS.formatMegabytes(3145728), "3.0", "3 MiB → 3.0");
    eq(
        TS.versionString({ version: { plugin: "0.1.0", abi: 1 } }),
        "v0.1.0 · abi 1",
        "版本号 mono 串",
    );
    eq(TS.versionString(null), "", "无快照 → 空串");
    eq(
        TS.storageOf({ features: { embedded: true, bytes: 3145728 } }),
        { embedded: true, bytes: 3145728, external: false },
        "内嵌态",
    );
    eq(
        TS.storageOf({ features: { embedded: false, bytes: 9000000 } }),
        { embedded: false, bytes: 9000000, external: true },
        "外置态(>8MB)",
    );
    eq(
        TS.analysisConfigOf({ analysis: {} }),
        {
            loudness_mode: "kw_integrated",
            center_slot_policy: "priority_queue",
        },
        "缺字段回落默认",
    );
    eq(
        TS.diagRowsOf({
            conn: {
                channels: [
                    { heartbeatAgeMs: 40, misalignCount: 0 },
                    { heartbeatAgeMs: 0xffffffff, misalignCount: 2 },
                ],
                generation: 3,
            },
            state: { config_seq: 42 },
        }),
        [
            { ch: "01", hb: "0.04s", mis: 0, gen: 3, seq: 42 },
            { ch: "02", hb: "—", mis: 2, gen: 3, seq: 42 },
        ],
        "诊断行模型",
    );
    check(
        TS.diagText(
            TS.diagRowsOf({
                conn: {
                    channels: [{ heartbeatAgeMs: 40, misalignCount: 0 }],
                    generation: 3,
                },
                state: { config_seq: 42 },
            }),
        ).startsWith("CH HB MIS GEN SEQ\n01 0.04s 0 3 42"),
        "诊断可复制文本以表头 + 行开头",
    );
}

// =============================================================================
log("=== ③ mock 端到端:setAnalysisConfig(§1.21)+ save/load 往返 ===");
{
    const driver = await import(u("web-preview/mock/state-driver.js"));
    const session = driver.createPreviewSession({
        role: "output",
        params: "fixture=fifteen-tracks",
    });
    const bridge = createBridge({ role: "output", mockBackend: session.mock });
    session.start();
    const snap = await bridge.requestInitialState();
    check(
        snap &&
            snap.analysis &&
            snap.analysis.loudness_mode === "kw_integrated",
        "首帧快照 analysis.loudness_mode 默认 kw_integrated",
    );
    check(
        snap &&
            snap.analysis &&
            snap.analysis.center_slot_policy === "priority_queue",
        "首帧快照 analysis.center_slot_policy 默认 priority_queue",
    );

    // 写入第二响度指标 → ok + state 回推
    eq(
        await bridge.setAnalysisConfig({ loudness_mode: "rms" }),
        { ok: true },
        "setAnalysisConfig({loudness_mode:rms})",
    );
    // save/load 往返:重新取快照(等价于重开工程读 CFGS)值不丢
    const snap2 = await bridge.requestInitialState();
    eq(
        snap2.analysis.loudness_mode,
        "rms",
        "重取快照 loudness_mode=rms(往返不丢)",
    );

    // 写入中心槽策略 → ok
    eq(
        await bridge.setAnalysisConfig({ center_slot_policy: "even_spread" }),
        { ok: true },
        "setAnalysisConfig({center_slot_policy:even_spread})",
    );
    const snap3 = await bridge.requestInitialState();
    eq(
        snap3.analysis.center_slot_policy,
        "even_spread",
        "重取快照 center_slot_policy=even_spread(往返不丢)",
    );

    // badArg:未知档(设计稿旧词 LUFS-S)与 02/03 内部拼写(even_offset)均不互认
    eq(
        await bridge.setAnalysisConfig({ loudness_mode: "LUFS-S" }),
        { ok: false, reason: "badArg" },
        "LUFS-S 误称 → badArg",
    );
    eq(
        await bridge.setAnalysisConfig({ center_slot_policy: "even_offset" }),
        { ok: false, reason: "badArg" },
        "02/03 内部拼写 even_offset → badArg(桥面只认 even_spread)",
    );
    eq(
        await bridge.setAnalysisConfig({}),
        { ok: false, reason: "badArg" },
        "空 patch → badArg",
    );

    // 零 gesture、不入撤销栈(§1.21):setAnalysisConfig 不触发参数事件
    const params = { seen: null };
    bridge.on("scvb.params", (p) => (params.seen = p));
    await bridge.setAnalysisConfig({ loudness_mode: "peak_dbfs" });
    await sleep(60);
    check(
        params.seen === null,
        "setAnalysisConfig 不触发 scvb.params(零 gesture)",
    );

    session.stop();
}

// =============================================================================
log("=== ④ 词条:T35 新增 set.* key ===");
{
    const KEYS = [
        "set.usage.eyebrow",
        "set.usage.workflow",
        "set.usage.docs",
        "set.loudnessMode.eyebrow",
        "set.centerSlot.eyebrow",
        "set.guide.showAll",
        "set.guide.collapse",
        "set.guide.rulesMissing",
        "set.storage.eyebrow",
        "set.storage.embedded",
        "set.storage.external",
        "set.storage.sessionGuid",
        "set.diag.eyebrow",
        "set.diag.copy",
        "set.diag.copied",
        "set.reanalyze",
    ];
    for (const k of KEYS) {
        for (const lang of ["zh", "en", "fr"]) {
            check(
                typeof T[lang][k] === "string" && T[lang][k].length > 0,
                `${lang}.${k} 非空`,
            );
        }
        const ph = (s) => (String(s).match(/\{\w+\}/g) || []).sort().join(",");
        eq(ph(T.en[k]), ph(T.zh[k]), k + " en/zh 占位符一致");
        eq(ph(T.fr[k]), ph(T.zh[k]), k + " fr/zh 占位符一致");
    }
    // 说明块 eyebrow 逐字「使用说明」,tab 名「设置」两 key 不得串用(J71⑥/回流④)
    eq(
        T.zh["set.usage.eyebrow"],
        "使用说明",
        "说明块 eyebrow 逐字「使用说明」",
    );
    eq(T.zh["tab.settings"], "设置", "tab 名保持「设置」");
    check(
        T.zh["set.usage.eyebrow"] !== T.zh["tab.settings"],
        "两处文案独立 key,不串用",
    );
    // centerSlot.note 与 05 §5.2 / 02 §5.6 对拍:「不影响音量豁免」不是「参与音量调节」
    check(
        /不影响音量豁免/.test(T.zh["set.centerSlot.note"]),
        "zh centerSlot.note = 不影响音量豁免",
    );
    check(
        !/参与音量调节/.test(T.zh["set.centerSlot.note"]),
        "zh centerSlot.note 不含旧误词「参与音量调节」",
    );
    check(
        /Vol Exempt/.test(T.en["set.centerSlot.note"]),
        "en centerSlot.note = Vol Exempt",
    );
    check(
        /exemption de volume/.test(T.fr["set.centerSlot.note"]),
        "fr centerSlot.note = exemption de volume",
    );
    // 选项正名:zh 无 LUFS-S 误称
    check(
        !/LUFS-S/.test(T.zh["set.loudnessMode.opt.kw_integrated"]),
        "zh 响度口径首档无 LUFS-S 误称",
    );
    check(
        /K 加权段积分/.test(T.zh["set.loudnessMode.opt.kw_integrated"]),
        "zh 响度口径首档 = K 加权段积分",
    );
    // 05 §5 / J45 禁词(词条值,非注释)
    const BAD = ["写入完成", "推子后", "post-fader", "六条"];
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
log("=== ⑤ 源码级:stale / 九条零手抄 / 块内展开 / J45 ===");
{
    const ts = src("web/output/tab-settings.js");
    const html = src("web/output/index.html");

    // 改任一项 → 置「改后需重分析」stale + setAnalysisConfig 落 state
    check(
        /call\("setAnalysisConfig", \{ \[field\]: value \}\)/.test(ts),
        "两设置块经 setAnalysisConfig(§1.21)落 state",
    );
    check(
        /analysisConfigDirty = true/.test(ts),
        "改动置 stale(analysisConfigDirty)",
    );
    check(
        /set.reanalyze/.test(html) && /settings-loudnessmode-stale/.test(html),
        "「改后需重分析」提示落点(三语 key set.reanalyze)",
    );

    // 九条 = 读取 guide.rule* 生成物,零手抄
    check(
        ts.includes("GUIDE_RULE_KEYS") && ts.includes('"guide.rule" + (i + 1)'),
        "九条 key 由 GUIDE_RULE_KEYS 生成,不逐条手写 key",
    );
    check(
        !/人声轨必须保持/.test(ts),
        "tab-settings.js 不含九条正文(零手抄;文本只来自生成物)",
    );
    check(
        /data-rest/.test(ts) && /i >= 3/.test(ts),
        "第 4-9 条标记 data-rest(降一档色重)",
    );

    // 「查看全部九条」= 块内展开(不复用 overlay)
    check(
        html.includes('data-open="0"') && html.includes('li[data-rest="1"]'),
        "九条展开态 = 块内 data-open(非 overlay)",
    );
    check(
        /data-gb="settings-guideblock-expand"/.test(html) &&
            /set\.guide\.showAll/.test(html) &&
            /set\.guide\.collapse/.test(ts),
        "「查看全部九条」↔「收起」块内 toggle",
    );

    // 诊断区可复制
    check(
        /navigator\.clipboard/.test(ts) &&
            /settings-diagnostics-copy/.test(html),
        "诊断区有复制落点(navigator.clipboard)",
    );

    // 缩放 10 秒防呆与 Bridge 一致:复用 app.js 既有状态机,不重复实现
    check(
        /settings-scale-select/.test(html) && /scale-confirm/.test(html),
        "缩放 select 与 10 秒防呆确认框同源(app.js)",
    );
}

// =============================================================================
log(fail === 0 ? "\n全部通过 ✅" : `\n失败 ${fail} 条 ❌`);
process.exit(fail === 0 ? 0 : 1);
