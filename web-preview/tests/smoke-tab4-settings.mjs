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
//   ⑤ 源码级:改 loudness_mode → 置「改后需重分析」stale(center_slot_policy 不弹);九条 = 读取 guide.rule* 生成物零手抄;
//     「查看全部九条」= 块内展开;J45 措辞零命中;
//   ⑥ native 落点(T37 真机回归):setLang/commitUiScale 落 processor、首启已读位两级落盘、
//     scvb.conn 读 registry 实况 —— 这三件 mock 天然自洽,只能在源码级拦。
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
    // 改后需重分析 = 当前值 !== 基线值(用户 preview:改回原值提示立即消失)
    eq(
        TS.analysisConfigStale("rms", "kw_integrated"),
        true,
        "loudness_mode 改走 → 提示出现",
    );
    eq(
        TS.analysisConfigStale("kw_integrated", "kw_integrated"),
        false,
        "loudness_mode 改回初值 → 提示消失",
    );
    eq(
        TS.analysisConfigStale("peak_dbfs", "rms"),
        true,
        "再次改走 → 提示再出现",
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

    // 只读观察态(second-output fixture):setAnalysisConfig → {observer:true}(§5.6)
    const roSession = driver.createPreviewSession({
        role: "output",
        params: "fixture=second-output",
    });
    const roBridge = createBridge({
        role: "output",
        mockBackend: roSession.mock,
    });
    roSession.start();
    eq(
        await roBridge.setAnalysisConfig({ loudness_mode: "rms" }),
        { observer: true },
        "只读观察态 setAnalysisConfig → {observer:true}",
    );
    roSession.stop();

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
        "set.diag.colCh",
        "set.diag.colHb",
        "set.diag.colMis",
        "set.diag.colGen",
        "set.diag.colSeq",
        "set.reanalyze",
        // [J78] 「手动接管与自动化」说明块(05 §2.4);check-i18n.mjs 未接任何
        // workflow,web-smoke 是这批文案在 CI 上的唯一门禁,漏语言即红。
        "set.automationGuide.eyebrow",
        "set.automationGuide.title",
        "set.automationGuide.line1",
        "set.automationGuide.line2",
        "set.automationGuide.line3",
        "set.automationGuide.line4",
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

    // 「改后需重分析」只在 loudness_mode 变化时出现(用户 preview 口径);
    // center_slot_policy 变化不弹该提示 + setAnalysisConfig 落 state
    check(
        /call\("setAnalysisConfig", \{ \[field\]: value \}\)/.test(ts),
        "两设置块经 setAnalysisConfig(§1.21)落 state",
    );
    check(
        ts.includes("analysisConfigBaseline") &&
            ts.includes("analysisConfigStale("),
        "改后需重分析由「当前值 vs 基线」派生(仅响度口径)",
    );
    check(
        /set.reanalyze/.test(html) && /settings-loudnessmode-stale/.test(html),
        "「改后需重分析」提示落点(三语 key set.reanalyze,只在响度块)",
    );
    check(
        !/settings-centerslot-stale/.test(html),
        "中心槽策略块不弹「改后需重分析」提示",
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

    // [J78] 「手动接管与自动化」说明块:锚点在,且四行正文 + eyebrow/标题全部
    // 经 data-t 接上词条(纯静态块,不进 tab-settings.js;整块被删或某行掉线即红)
    check(
        /data-gb="settings-automationguide"/.test(html) &&
            ["eyebrow", "title", "line1", "line2", "line3", "line4"].every(
                (k) => html.includes(`data-t="set.automationGuide.${k}"`),
            ),
        "「手动接管与自动化」说明块锚点 + 六词条 data-t 落点齐全",
    );

    // 诊断区可复制
    check(
        /navigator\.clipboard/.test(ts) &&
            /settings-diagnostics-copy/.test(html),
        "诊断区有复制落点(navigator.clipboard)",
    );

    // 只读观察态:J69 两设置块整组禁用 + observer 短路(契约 §5.6)
    check(
        ts.includes("isReadOnly") &&
            ts.includes("res.observer") &&
            ts.includes('attr(btn, "data-disabled"'),
        "readOnly 门控 + res.observer 短路 + data-disabled 落点",
    );
    check(
        html.includes('.set-block__row .sc-seg__item[data-disabled="1"]'),
        "只读态三段选置灰 CSS 落点",
    );

    // 诊断区 a11y 词条化(不硬编码 CH/HB/MIS/GEN/SEQ 与 aria-label)
    check(
        html.includes('data-t-aria="set.diag.eyebrow"') &&
            html.includes('data-t="set.diag.colCh"') &&
            html.includes('data-t="set.diag.colSeq"'),
        "诊断区 aria-label 与表头走 data-t/data-t-aria(不硬编码)",
    );
    check(
        !html.includes('aria-label="diagnostics"') &&
            !html.includes(">CH</span"),
        "诊断区无硬编码 aria-label 与表头字面量",
    );

    // 缩放 10 秒防呆与 Bridge 一致:复用 app.js 既有状态机,不重复实现
    check(
        /settings-scale-select/.test(html) && /scale-confirm/.test(html),
        "缩放 select 与 10 秒防呆确认框同源(app.js)",
    );

    // 诊断区初始展开(用户 preview:避免下方空一块)
    check(
        ts.includes("diagOpen: true") && html.includes('data-open="1"'),
        "诊断区初始展开(diagOpen:true + data-open=1)",
    );

    // 版本号用干净 sans(非等宽细体;用户 preview)
    check(
        html.includes(".settings-version__value") &&
            html.includes("font-family: var(--ff-sans)"),
        "版本号排印改干净 sans(--ff-sans,非 sc-mono)",
    );
}

// =============================================================================
log("=== ⑥ native 落点:ui.* 与 conn 的写/读路径(T37 真机回归)===");
{
    // 为什么这几条断言在 web 冒烟里:mock 后端的 setLang 是 patchState({ui:{language}}),
    // 天然自洽,web-preview 永远复现不出 native 的写路径缺失。T37 真机四条 bug 里有三条
    // 断在「native 有下发、没有回写/没有持久化」这一层,这里补上对应的源码级闸。
    const oe = src("src/output/OutputEditor.cpp");
    const op = src("src/output/OutputProcessor.cpp");
    const ie = src("src/input/InputEditor.cpp");

    // A-1:§2.1 的 ui.language / ui.scale 是从 **processor** 取的,而 WebViewHost 的通用
    // handleSetLang / setUiScale 只改 editor 自己的 lang_ / uiScale_。子类不把值落到 processor,
    // 下一拍 state emit 就把旧值原样回推,syncUiFromState 当场把页面切回旧语言 ——
    // 真机表现:选完中文,一切 tab / 一点「开始使用」就变回英文。
    for (const [name, code] of [
        ["Output", oe],
        ["Input", ie],
    ]) {
        check(
            /void\s+\w+Editor::handleSetLang/.test(code) &&
                code.includes("bridgeSetUiLanguage"),
            name +
                "Editor 覆写 handleSetLang 并落 processor(bridgeSetUiLanguage)",
        );
        check(
            /void\s+\w+Editor::persistUiScaleAsDefault/.test(code) &&
                code.includes("bridgeSetUiScalePercent"),
            name + "Editor 覆写 persistUiScaleAsDefault 并落 processor",
        );
    }
    check(
        op.includes("ScvbOutputAudioProcessor::bridgeSetUiLanguage") &&
            op.includes("ScvbOutputAudioProcessor::bridgeSetUiScalePercent"),
        "Output processor 两个 ui setter 有实现",
    );

    // A-3:首启已读位的两级落盘 —— 工程位入 CFGS、系统级全局位入 UiDefaultsStore。
    check(
        op.includes("uiGuideSeen") && op.includes("uiTourSeen"),
        "guide_seen / tour_seen 随工程 state 落盘(CFGS)",
    );
    check(
        oe.includes("uidefaults::guideSeenGlobal()") &&
            oe.includes("uidefaults::tourSeenGlobal()"),
        "快照的 *_global 取自 UiDefaultsStore(不再硬编码 false)",
    );
    check(
        oe.includes("uidefaults::setGuideSeenGlobal") &&
            oe.includes("uidefaults::setTourSeenGlobal"),
        "setGuideSeen / setTourSeen 的 alsoGlobal 真的落盘",
    );

    // B:桥面 conn 必须来自 registry 实况,不得再有 T29 的占位常量。UI 的连接数口径是
    // 「slotState=2 ∧ heartbeatFresh」,heartbeatFresh 恒 false 则连接数恒 0 ——
    // 真机表现:音频通、Input 显示已连接,Output 轨道页却永远「组 X 尚无输入」。
    check(
        oe.includes("processor_.connSnapshot()"),
        "buildConnPayload 读 registry 实况快照",
    );
    check(
        !/put\(ch, "heartbeatFresh", false\)/.test(oe),
        "heartbeatFresh 不再硬编码 false(否则连接数恒 0)",
    );
}

// =============================================================================
log(fail === 0 ? "\n全部通过 ✅" : `\n失败 ${fail} 条 ❌`);
process.exit(fail === 0 ? 0 : 1);
