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
//     docsUrl(另在 R3 增补段真跑,[J149] 固定指向 prod) /
//     analysisConfigOf / diagRowsOf / diagText;
//   ③ mock 端到端:setAnalysisConfig 写入 + badArg(含 02/03 拼写不互认)+ save/load 往返不丢;
//   ④ 词条:T35 新增 set.* key 三语齐、占位符三语一致、05 §5 禁词零命中;
//   ⑤ 源码级:改任一设置项 → 置「改后需重分析」stale,且**两项都会弹**重新分析询问框
//     ([SL-354] 用户 2026-09-06 改口径;SL-276 的「中央槽只上徽标」已作废);九条 = 读取 guide.rule* 生成物零手抄;
//     「查看全部九条」= 块内展开;J45 措辞零命中;
//   ⑥ native 落点(T37 真机回归):setLang/commitUiScale 落 processor、首启已读位两级落盘、
//     scvb.conn 读 registry 实况 —— 这三件 mock 天然自洽,只能在源码级拦。
//
// 用法:node web-preview/tests/smoke-tab4-settings.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有断言失败(逐条打印 [FAIL])。
// =============================================================================

import { pathToFileURL, fileURLToPath } from "node:url";
import { dirname, join, relative, resolve } from "node:path";
import { readdirSync, readFileSync } from "node:fs";
// [SL-354] 源码级判据**必须先剥注释**:否则作者在实现里逐字引一遍旧写法,判据就被
// 喂饱了(本卡实测过一次:代码改了、这一格照绿)。用 SL-330b 那个共享的字符级扫描器,
// 不在这里再手写一份 —— 手写的那种只丢弃整行注释,块注释中间那几行漏得干干净净。
import { stripJsComments } from "../../scripts/lib/strip-comments.mjs";
const stripComments = (text) => stripJsComments(text, "smoke-tab4-settings");

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
    // [SL-395] 判据只有 embedded 这一位:**内嵌但 >8MB** 仍是内嵌(按字节数判外置会误报;
    // 8MB 是「压缩后」的阈值,内嵌的特征体照样能超过它)。
    eq(
        TS.storageOf({ features: { embedded: true, bytes: 9000000 } }),
        { embedded: true, bytes: 9000000, external: false },
        "内嵌且 >8MB ⇒ 仍内嵌(不再按字节数误判)",
    );
    eq(
        TS.storageOf({ features: { embedded: false, bytes: 9000000 } }),
        { embedded: false, bytes: 9000000, external: true },
        "外置态(读路径:打开带 sidecar 的老工程会走到这里,保存一次后消失)",
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
    // [SL-354] `appliedAnalysisConfigOf` 缺字段时**回落到当前值**,回落之后派生的 stale
    // 恒假 —— 与「基线真的等于当前值」在读数上无法区分。`hasAppliedAnalysisConfig` 就是
    // 那个区分器:回答「这份 state 自己带了 applied 那两个字段吗」。
    // 下面四格钉的是**它与那个回落同口径**(`applied.x || cur.x`:空串 / 缺字段 / 整支
    // 缺席都会触发回落),以及「只带一半不算带」—— 弹窗判据是两项取或,半份仍有一项在吃
    // 回落值。删掉实现里任一半的字段判断,对应那一格红。
    eq(
        TS.hasAppliedAnalysisConfig({
            analysis: {
                loudness_mode: "rms",
                applied: {
                    loudness_mode: "kw_integrated",
                    center_slot_policy: "priority_queue",
                },
            },
        }),
        true,
        "两个字段都带 → 基线是真读数",
    );
    eq(
        TS.hasAppliedAnalysisConfig({ analysis: { loudness_mode: "rms" } }),
        false,
        "整支 applied 缺席 → 基线是回落值",
    );
    eq(
        TS.hasAppliedAnalysisConfig({
            analysis: { applied: { loudness_mode: "rms" } },
        }),
        false,
        "只带一半 → 另一半仍在吃回落值,不算带",
    );
    eq(
        TS.hasAppliedAnalysisConfig({
            analysis: {
                applied: {
                    loudness_mode: "",
                    center_slot_policy: "priority_queue",
                },
            },
        }),
        false,
        "空串与缺字段同口径(回落用的就是 `||`)",
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
    // [SL-298 / J130] 音量那枚开关的用户可见叫法统一为「参与音量调节」(三语参与语义)。
    // 此前这里钉的是反方向(「不影响音量豁免」、且**不得**出现「参与音量调节」)——
    // 那是按 05 旧口径钉的,J130 用户裁定后与之相反,故同一 commit 改钉。参数本体
    // `lead_vol_exempt` 不改名,只约束显示文案。
    //   格 1:centerSlot.note 三语都是参与口径;
    //   格 2:tour 第 24 步标题 = 同格 aria `tracks.colVolPart`(同一个词);
    //   格 3:tour 第 24 步正文是「是否参与」极性,不是「不参与/被排除」;
    //   格 4:全部词条值零「豁免 / exempt」字样(纯文案层;注释与参数名不在扫描面)。
    check(
        /不影响各轨是否参与音量调节/.test(T.zh["set.centerSlot.note"]),
        "[J130] zh centerSlot.note 用参与口径",
    );
    check(
        /whether each track joins volume adjustment/.test(
            T.en["set.centerSlot.note"],
        ),
        "[J130] en centerSlot.note 用参与口径",
    );
    check(
        /participation de chaque piste à l'ajustement du volume/.test(
            T.fr["set.centerSlot.note"],
        ),
        "[J130] fr centerSlot.note 用参与口径",
    );
    for (const lang of ["zh", "en", "fr"]) {
        eq(
            T[lang]["tour.step24.title"],
            T[lang]["tracks.colVolPart"],
            `[J130] ${lang} tour 第 24 步标题 = tracks.colVolPart`,
        );
    }
    // 上面那格只比了字典;「同格」要靠音量开关那一格真的挂着这个 aria(复审第 2 轮补)。
    check(
        /data-t-aria="tracks\.colVolPart"\s+data-gb="\$\{gb\("volexempt"\)\}"/.test(
            stripComments(src("web/output/tab-tracks.js")),
        ),
        "[J130] 音量开关那格(volexempt)的 aria 仍指向 tracks.colVolPart",
    );
    check(
        /该轨是否参与音量平衡计算/.test(T.zh["tour.step24.body"]) &&
            /whether this track joins level balancing/.test(
                T.en["tour.step24.body"],
            ) &&
            /si la piste entre dans l'équilibrage/.test(
                T.fr["tour.step24.body"],
            ),
        "[J130] tour 第 24 步正文三语都是「是否参与」极性",
    );
    {
        const hits = [];
        for (const lang of ["zh", "en", "fr"])
            for (const [k, v] of Object.entries(T[lang]))
                if (/豁免|exempt/i.test(String(v))) hits.push(`${lang}:${k}`);
        eq(
            hits,
            [],
            "[J130] 词条值零「豁免 / exempt」字样(音量开关显示层是参与语义,默认全开;" +
                "写「豁免」会让用户把默认读成「全被豁免」。若命中的词条与音量开关无关," +
                "把它的 key 显式排除,别删这一格)",
        );
    }
    //   格 5:用户手册轨道页那一行用同一个叫法(中英两份)。
    check(
        /主唱锁、参与音量调节、参与自动声像/.test(
            src("docs/USER_GUIDE.zh-CN.md"),
        ) &&
            /lead lock, volume participation, auto-pan participation/.test(
                src("docs/USER_GUIDE.md"),
            ),
        "[J130] USER_GUIDE 中英两份轨道页一行用参与口径",
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

    // 两设置块的写入口:改档经 setAnalysisConfig(§1.21)落 state。
    // ([SL-354] 原来这里还写着「弹窗只在 loudness_mode 变化时出现、中央槽不弹」——
    //  那是 SL-276 的旧口径,用户 2026-09-06 已改成两项同形,见下面翻面的那两格。)
    check(
        /call\("setAnalysisConfig", \{ \[field\]: value \}\)/.test(ts),
        "两设置块经 setAnalysisConfig(§1.21)落 state",
    );
    // [SL-279] 基线的真源换了:从本地 `analysisConfigBaseline` 快照换成工程 state 的
    // `analysis.applied.*`。这一格跟着换 —— 钉「本地快照那条路真的没了」+「新真源真的在用」,
    // 两半都要:只钉后者的话,谁把旧快照加回来当兜底,这一格照绿。
    check(
        !ts.includes("local.analysisConfigBaseline") &&
            ts.includes("appliedAnalysisConfigOf(") &&
            ts.includes("analysisConfigStale("),
        "改后需重分析由「当前值 vs state.analysis.applied.*」派生,本地基线快照已删",
    );
    check(
        /set.reanalyze/.test(html) && /settings-loudnessmode-stale/.test(html),
        "「改后需重分析」提示落点(三语 key set.reanalyze,响度块)",
    );
    // [SL-278] **口径反转**:中心槽策略与响度档同在分析派生式里(02 §5.6),改后段表就与
    // 设置对不上了 —— 此前这一块没有任何失效标记,本卡给它同形的琥珀徽标 + 影响面说明。
    // 徽标 key 与响度块**共用** set.reanalyze(两处一致由结构保证);影响面各一条。
    check(
        /settings-centerslot-stale/.test(html) &&
            /set\.centerSlot\.scopeNote/.test(html),
        "中心槽策略块有「改后需重分析」徽标 + 自己的影响面说明",
    );
    // [SL-354] **口径翻面**:弹窗从「只由响度档承载」改成两项都弹(用户 v5.6.7 实测
    // 「B3 完全没有弹出弹窗,应该和前面一样」)。SL-276 当时只读响度是**按当时的用户口径
    // 有意做的**,不是缺陷 —— 所以这里连同判据一起翻,而不是留着两处反向断言。
    //
    // ⚠ 这一格原来钉的是**源码字面**(`/const stale = loudnessStale;/`),本卡实测它有个
    // 大洞:作者在 tab-settings 的注释里**逐字引一遍旧写法**,这格就当场被喂饱 —— 代码
    // 已经改了、判据照绿。本格不剥注释(与 SL-297 记的「文本级判据先剥注释」同族)。
    // 所以这里改成**两条都钉、且都不是可被注释冒充的字面**:开闸点不再按字段名分叉、
    // 弹窗判据两项都读。真正的行为由页面级那几格(smoke-ui-layout-page 的 C10)承担。
    check(
        /const stale = loudnessStale \|\| centerStale;/.test(stripComments(ts)),
        "[SL-354] 弹窗判据两项都读(改回只读响度即红;注释冒充不了 —— 本格剥过注释)",
    );
    // [SL-354] 「开闸点不再按字段名分叉」这条断言**必须只看 wireSeg 的函数体**。
    // 全文件扫的话会误报:复审第 1 轮补的 `pendingStale` 选择器在 syncStale 里按字段名
    // 分叉是**对的**(要判「刚改走的那一项自己脏不脏」),而它与本格要拦的东西无关 ——
    // 全文件版当场把它判成回归(实测栽过一次)。取函数体 = 从 `function wireSeg(` 到
    // 下一个同层 `function `,并先断真的取到了(取不到就是下面那格在空跑)。
    {
        const bare = stripComments(ts);
        const from = bare.indexOf("function wireSeg(");
        const to = bare.indexOf("function toggleNine(");
        check(
            from >= 0 && to > from,
            "[SL-354] 取到 wireSeg 的函数体(取不到就说明下面那格在空跑)",
        );
        const body = from >= 0 && to > from ? bare.slice(from, to) : "";
        check(
            !/field === "loudness_mode"/.test(body) &&
                /local\.askPending = \{ field, value \}/.test(body),
            "[SL-354] 开闸点不再按字段名分叉:两个设置块写成功都置闸,且记的是「哪个字段的哪个值」",
        );
    }
    // [SL-354] **接线格**:纯函数 ②(上面四格)只回答「拿到 state 之后怎么判」,答不了
    // 「这条判断有人跑吗」。行为面由页面级的 C10f 承担,但那一套依赖无头浏览器、缺依赖时
    // 整套 SKIP —— 所以这条**最起码的接线**必须留一份在这个永不 SKIP 的 node 套里:
    // syncStale 的函数体内真的调了它。剥注释,免得被头注里的函数名喂饱。
    // ← 把 syncStale 里那次调用删掉(哪怕保留 export 与四格纯函数用例),本格红。
    {
        const bare = stripComments(ts);
        const from = bare.indexOf("function syncStale()");
        const to = bare.indexOf("function renderGuideRules()");
        check(
            from >= 0 && to > from,
            "[SL-354] 取到 syncStale 的函数体(取不到就说明下面那格在空跑)",
        );
        const body = from >= 0 && to > from ? bare.slice(from, to) : "";
        check(
            body.includes("hasAppliedAnalysisConfig("),
            "[SL-354] syncStale 真的调了 hasAppliedAnalysisConfig(回落值不许做破坏性判断的那道兜底闸)",
        );
    }

    // [SL-371] 「稍后」→「撤销更改」的**接线格**。行为面由页面级的 C2 / C2b 承担
    // (smoke-ui-layout-page),但那一套依赖无头浏览器、缺依赖时整套 SKIP —— 所以
    // 「这枚钮到底挂了谁 / 它到底发不发写」必须留一份在这个永不 SKIP 的 node 套里。
    // 剥注释再匹配:本卡在这两处写了大量解释性注释,不剥就会被自己的注释喂饱
    // (SL-358 记的那个坑,同一个文件上刚栽过)。
    {
        const bare = stripComments(ts);
        // ← 把 handler 改回 closeReanalyzeAsk,本格红。
        check(
            /el\.reanalyzeAskLater\.addEventListener\(\s*"click",\s*revertFromAsk\s*\)/.test(
                bare,
            ),
            "[SL-371] 框里那枚次要钮挂的是 revertFromAsk(挂回 closeReanalyzeAsk 即红)",
        );
        // ← Esc 那条分支若也被改成 revertFromAsk,本格红(它断的是「只有一处接了撤销」)。
        //   页面级 C2b 断的是同一件事的行为面,这里断的是接线面 —— 那一套会 SKIP。
        check(
            (bare.match(/revertFromAsk/g) || []).length === 2,
            "[SL-371] 全文件只有两处 revertFromAsk(一处定义 + 一处接线)—— Esc / 遮罩没被接上写操作",
        );
        const from = bare.indexOf("async function revertFromAsk()");
        const to = bare.indexOf("async function doReanalyzeFromAsk()");
        check(
            from >= 0 && to > from,
            "[SL-371] 取到 revertFromAsk 的函数体(取不到就说明下面三格在空跑)",
        );
        const body = from >= 0 && to > from ? bare.slice(from, to) : "";
        // ← 把这一次写删掉(退回「只关框」),本格红。
        check(
            /call\(\s*"setAnalysisConfig"/.test(body),
            "[SL-371] 撤销那一路真的发了一次 setAnalysisConfig(不是只关框)",
        );
        // ← 把 baseline 改成别的来源(如硬写默认档),本格红:写回去的必须是
        //   「上次全量分析真正用过的那一档」,那才是用户说的「原来的方案」。
        check(
            /appliedAnalysisConfigOf\(st\)\[pending\.field\]/.test(body),
            "[SL-371] 写回去的是 analysis.applied.* 那一份(基线),不是默认档、也不是「上一个值」",
        );
        // ← 删掉这道闸,本格红:state 没带 applied 时上面那个取值会**回落到当前值**,
        //   拿回落值去写就是一次骗人的空写(与 syncStale 里同名的兜底闸同一条理由)。
        check(
            /hasAppliedAnalysisConfig\(st\)/.test(body),
            "[SL-371] 读不到基线时不发写(回落值只许渲染,不许写回去)",
        );
        // [SL-371 复审第 1 轮,统筹裁定] 两枚钮**共用一位** `askInFlight`,任一在途另一枚
        // 早退。行为面由页面级 C4e(analyze 在途 → 点撤销)与 C4f(撤销在途 → 点重新分析)
        // 两格钉住,这里守接线 —— 那一套会 SKIP。
        // ← 把 `local.askInFlight` 换回各管各的 `local.revertInFlight`,本格红。
        check(
            /local\.askInFlight/.test(body),
            "[SL-371] 撤销这一路读的是共用的 askInFlight(不是各管各的布尔)",
        );
        // 另一半:主钮那一路也读同一位(只改一侧的话「撤销在途 → 点重新分析」照样可达)。
        {
            const f2 = bare.indexOf("async function doReanalyzeFromAsk()");
            const t2 = bare.indexOf("function syncStale()");
            check(
                f2 >= 0 && t2 > f2,
                "[SL-371] 取到 doReanalyzeFromAsk 的函数体",
            );
            const b2 = f2 >= 0 && t2 > f2 ? bare.slice(f2, t2) : "";
            check(
                /local\.askInFlight/.test(b2) &&
                    !/local\.reanalyzeInFlight/.test(b2),
                "[SL-371] 主钮那一路也读同一位(旧的 reanalyzeInFlight 已全部换掉)",
            );
        }
    }

    // [SL-375] 范围档下重分析完成后,两枚徽标改念「只更新了部分范围」。
    // 行为面由页面级 C8r-p 钉住(点主钮前后同一枚徽标念的不是同一句话);这里守
    // node 断得到的两件:①词条三语齐;②syncStale 真的每帧重填徽标文本(接线)。
    // ← 把 syncStale 里那句 renderStaleBadges() 删掉,②当场红。
    {
        const bare = stripComments(ts);
        const from = bare.indexOf("function syncStale()");
        const to = bare.indexOf("function renderGuideRules()");
        check(
            from >= 0 && to > from,
            "[SL-375] 取到 syncStale 的函数体(取不到就说明下面那格在空跑)",
        );
        const body = from >= 0 && to > from ? bare.slice(from, to) : "";
        check(
            body.includes("renderStaleBadges()"),
            "[SL-375] syncStale 每帧重填两枚徽标的文案(不重填就停在 index.html 的静态词条上)",
        );
        for (const lang of ["zh", "en", "fr"]) {
            const v = T[lang]["set.reanalyze.partialRange"];
            check(
                typeof v === "string" && v.trim() !== "",
                `[SL-375] 词条 ${lang}.set.reanalyze.partialRange 非空`,
            );
            check(
                v !== T[lang]["set.reanalyze"],
                `[SL-375] ${lang} 的两条徽标词条不相等(相等的话 C8r-p 那格永远分不出来)`,
            );
        }
    }

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

    // A-3:首启已读位的两级落盘 —— 工程位挂 PRMS 的 ValueTree(不是 CFGS:那是定长枚举式
    // 解码,追加字段会让旧构建整块拒载、把 group/开关/版本静默打回默认)、系统级全局位入
    // UiDefaultsStore。
    check(
        op.includes("writeUiFlags") && op.includes("readUiFlags"),
        "guide_seen / tour_seen 随 PRMS 落盘(getState 写 / setState 读)",
    );
    check(
        !src("src/core/state/OutputStateCodec.h").includes("uiGuideSeen"),
        "CFGS 定长布局未被追加字段(旧构建仍解得动新工程)",
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
    // [SL-167b / J132] 契约里「全局位按侧分」那句要写实现真正用的键名:同一个落盘文件、
    // 两个键(Output `guide_seen_global` / Input `guide_seen_global_input`)。此前契约写
    // 「`input.*` / `output.*` 分键、命名空间本就按侧分」,实现里两样都没有。
    {
        // 这里拿 JS 词法扫描器剥 C++ 注释:已核过本文件没有字符字面量 / raw string 等歧义
        // (扫不下去会抛错,不会静默返回半份)。不是通则,别照搬到别的 .cpp 上。
        const uds = stripComments(src("src/plugin-common/UiDefaultsStore.cpp"));
        const contract = src("docs/SCVB_CONTRACT.md");
        const schema = src("docs/STATE_SCHEMA.md");
        check(
            uds.includes('"guide_seen_global"') &&
                uds.includes('"guide_seen_global_input"'),
            "[J132] 实现侧确有两个键 guide_seen_global / guide_seen_global_input",
        );
        for (const [name, text] of [
            ["SCVB_CONTRACT", contract],
            ["STATE_SCHEMA", schema],
        ]) {
            check(
                text.includes("`guide_seen_global_input`"),
                `[J132] ${name} 写出 Input 侧真实键名 guide_seen_global_input`,
            );
            check(
                !/命名空间本就按侧分|`input\.\*` \/ `output\.\*` 分键,/.test(
                    text,
                ),
                `[J132] ${name} 不再写「命名空间按侧分 / input.* output.* 分键」`,
            );
        }
        // 契约里另外两个实现事实也要有尺子(复审补):落盘文件名从 openFile() 现读拼出;
        // tour 全局位只有一个键。
        const app = /options\.applicationName\s*=\s*"([^"]+)"/.exec(uds);
        const suffix = /options\.filenameSuffix\s*=\s*"([^"]+)"/.exec(uds);
        const fileName = app && suffix ? `${app[1]}.${suffix[1]}` : null;
        check(
            fileName !== null,
            "[J132] 从 UiDefaultsStore.cpp 读到 applicationName / filenameSuffix",
        );
        for (const [name, text] of [
            ["SCVB_CONTRACT", contract],
            ["STATE_SCHEMA", schema],
        ])
            check(
                fileName !== null && text.includes(`\`${fileName}\``),
                `[J132] ${name} 写的落盘文件名 = 实现的 ${fileName}`,
            );
        eq(
            uds.match(/"tour_seen_global[^"]*"/g),
            ['"tour_seen_global"'],
            "[J132] 实现侧 tour 全局位只有一个键(契约:只有 Output 一个键)",
        );
        check(
            contract.includes("`tour_seen_global` 只有 Output 一个键"),
            "[J132] SCVB_CONTRACT 写明 tour_seen_global 只有 Output 一个键",
        );
    }

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
log(
    "=== R3 增补:J88 改名 / SL-211 复制确认框 / SL-213 工作流程卡 / SL-214 文档外链 ===",
);
{
    const app = src("web/output/app.js");
    const html = src("web/output/index.html");
    const ts = src("web/output/tab-settings.js");

    // ---- J88:Output 模式开关 ON 档改名「写入自动化」(OFF 档不动)----
    for (const [lang, want] of [
        ["zh", "写入自动化"],
        ["en", "WRITE AUTOMATION"],
        ["fr", "ÉCRITURE AUTOMATION"],
    ]) {
        eq(T[lang].engineDrive, want, `J88 ${lang}.engineDrive 已改名`);
    }
    eq(T.zh.followHost, "跟随宿主", "J88 OFF 档不动");
    // [复审终轮④] 静态兜底文案也得改:applyI18n 会覆盖它,但**字典未注入 / 首帧未刷**
    // 时露出来的就是 HTML 里这一份。
    check(
        !/引擎驱动/.test(html),
        "J88 index.html 的静态兜底文案(含注释)无旧档位名残留",
    );
    // 旧名不得残留在**任何**词条值里(footer / 加载守卫横幅 / tour / 工作流程卡都引用了它)。
    // [SL-293] 原先这里给 tracks.colLegend 开了一条豁免,理由是它那句「不再被引擎驱动」
    // 是动词用法。本卡按 05 规格重写 colLegend 后,三语分别是「不再驱动」/「no longer
    // driven」/「plus piloté」,**豁免的对象已经不存在** —— 而一条对不上任何词条的
    // `continue` 不是无害的死代码:它让 colLegend 永久退出这道检查,谁把档位名写回这一条
    // 都不会红。故连豁免带注释一并删掉,让 J88 的判据覆盖全部词条。
    for (const lang of ["zh", "en", "fr"]) {
        for (const [k, v] of Object.entries(T[lang])) {
            check(
                !/引擎驱动|ENGINE DRIVE|Engine drive|PILOTAGE MOTEUR|Pilotage moteur/.test(
                    String(v),
                ),
                `J88 ${lang}.${k} 无旧档位名残留`,
            );
        }
    }

    // ---- SL-211①:复制确认框搬到卡片层 ----
    // .sc-scrim 是 absolute + inset:0,盖住的是**最近的定位祖先**;原先它嵌在
    // 210px 宽的 header-version 里,模态被挤扁、长正文当场截断(用户实测)。
    check(
        html.indexOf('data-gb="header-version-copy-confirm"') >
            html.indexOf('data-gb="header-version"'),
        "SL-211 复制确认框已挪出 header-version",
    );
    check(
        /\.sc-modal--copyconfirm\s*\{[^}]*width:\s*460px/.test(html),
        "SL-211 模态给了定宽(不再由文案长度决定框宽)",
    );
    check(
        /\.sc-modal__foot\s*\{[^}]*justify-content:\s*flex-end/.test(html),
        "SL-211 两枚钮排右下",
    );

    // ---- SL-213:工作流程卡收口钮 = 右下「确认」+ CTA 配色 ----
    check(
        /close\.setAttribute\("data-t", "common\.confirm"\)/.test(app),
        "SL-213 收口钮文案改「确认」(原 common.cancel)",
    );
    check(
        /close\.className = "sc-btn sc-btn--cta workflow-card__close"/.test(
            app,
        ),
        "SL-213 收口钮走 CTA 配色(裸 .sc-btn 在深色模态上与底几乎同色)",
    );
    check(
        /workflow-card__close \{[^}]*align-self: flex-end/.test(app),
        "SL-213 收口钮排右下",
    );
    for (const [lang, want] of [
        ["zh", "确认"],
        ["en", "Confirm"],
        ["fr", "Confirmer"],
    ]) {
        eq(T[lang]["common.confirm"], want, `SL-213 ${lang}.common.confirm`);
    }

    // ---- SL-214:文档钮接线 + 走系统浏览器 ----
    // 定谳:这颗钮此前**根本没有 handler**(index.html 原注写着「点击行为仍未定」)。
    check(
        /el\.docs\.addEventListener\("click", openDocs\)/.test(ts),
        "SL-214 文档钮已接线",
    );
    check(
        /window\.open\(url, "_blank", "noopener"\)/.test(app),
        "SL-214 走 window.open(→ WebView2 NewWindowRequested → JUCE 外链通道)",
    );
    // ---- J149:插件内文档链接固定指向 prod 分支(纯函数真跑,不看字面)----
    // [J149] 推翻了 [SL-220] 的「按插件版本号 pin 到同号 tag」:地址与快照、版本号无关。
    {
        const PROD = "https://github.com/synchain-oss/scvb/blob/prod/";
        const snap = (plugin) => ({ version: { plugin, abi: 1 } });
        eq(
            TS.docsUrl("zh"),
            PROD + "docs/USER_GUIDE.zh-CN.md",
            "J149 中文界面 → prod 上的中文手册",
        );
        eq(
            TS.docsUrl("en"),
            PROD + "docs/USER_GUIDE.md",
            "J149 英文界面 → prod 上的英文手册",
        );
        eq(
            TS.docsUrl("fr"),
            PROD + "docs/USER_GUIDE.md",
            "J149 法文界面 → prod 上的英文手册(无法文手册)",
        );
        // 反向:就算调用方照旧把快照递进来,也不许拼出 tag / 回退到 dev(SL-220 的旧形态)。
        for (const [label, s] of [
            ["快照带正式版本号", snap("9.8.7")],
            ["快照带预发布版本号", snap("9.8.7-rc.2")],
            ["无快照", null],
        ]) {
            eq(
                TS.docsUrl("zh", s),
                PROD + "docs/USER_GUIDE.zh-CN.md",
                `J149 地址不随快照变:${label}`,
            );
        }
        // 接线:openDocsInBrowser 真的用 docsUrl(lang) 去算地址(剥注释后看代码)。
        // 纯函数全绿而这一跳没接上 = 按钮仍开旧地址,所以接线单独钉。
        const appCode = stripComments(app);
        const body = (appCode.match(
            /function openDocsInBrowser\(\)\s*\{([\s\S]*?)\n\}/,
        ) || [, ""])[1];
        check(
            /const url = docsUrl\(lang\);/.test(body) &&
                /window\.open\(url, "_blank", "noopener"\)/.test(body),
            "J149 openDocsInBrowser 用 docsUrl(lang) 算地址再 window.open",
        );
        check(
            /import \{[^}]*\bdocsUrl\b[^}]*\} from "\.\/tab-settings\.js"/.test(
                appCode,
            ),
            "J149 app.js 从 tab-settings.js 引入 docsUrl",
        );
        // 全插件面:web/ 下任何一处代码(剥注释)里的仓库 blob 链接都只许走 prod。
        // 扫整个 web/ 而不是只看 tab-settings.js —— 以后谁在别的页面再加一个文档外链,
        // 照 SL-220 的旧写法 pin 到 tag 或写死 dev,这一格就红。
        {
            const webFiles = [];
            (function walk(dir) {
                for (const ent of readdirSync(dir, { withFileTypes: true })) {
                    const p = join(dir, ent.name);
                    if (ent.isDirectory()) walk(p);
                    else if (/\.(m?js|html)$/.test(ent.name)) webFiles.push(p);
                }
            })(join(ROOT, "web"));
            const blobRe = /synchain-oss\/scvb\/blob\/([^/"'`\s]+)\//g;
            let prodHits = 0;
            const offProd = [];
            for (const f of webFiles) {
                const text = readFileSync(f, "utf8");
                const code = f.endsWith("js") ? stripComments(text) : text;
                for (const m of code.matchAll(blobRe)) {
                    if (m[1] === "prod") prodHits++;
                    else
                        offProd.push(
                            relative(ROOT, f).split("\\").join("/") +
                                " → blob/" +
                                m[1],
                        );
                }
            }
            check(
                webFiles.length > 0 && prodHits > 0,
                `J149 扫到 web/ 下的 prod 链接(文件 ${webFiles.length} 个,prod 链接 ${prodHits} 处;为 0 说明这一格在空跑)`,
            );
            check(
                offProd.length === 0,
                "J149 web/ 下的仓库 blob 链接一律指向 prod,不 pin tag、不写 dev:" +
                    offProd.join(" / "),
            );
        }
        // 九条红字(生成物,真源 docs/USER_GUIDE.zh-CN.md#硬约束):凡提到 DAW 兼容表的条目,
        // 三语都必须给 prod 上的完整地址 —— 插件里的用户拿到一句仓内相对路径 `docs/…` 是找不到的。
        {
            const DAW = PROD + "docs/DAW_COMPATIBILITY.md";
            let mentions = 0;
            for (let n = 1; n <= 9; n++) {
                const langs = [];
                for (const lang of ["zh", "en", "fr"]) {
                    const v = T[lang]["guide.rule" + n] || "";
                    if (!v.includes("DAW_COMPATIBILITY")) continue;
                    langs.push(lang);
                    mentions++;
                    check(
                        v.includes(DAW),
                        `J149 ${lang}.guide.rule${n} 提到 DAW 兼容表时给的是 prod 上的完整地址`,
                    );
                }
                check(
                    langs.length === 0 || langs.length === 3,
                    `J149 guide.rule${n} 三语要么都提 DAW 兼容表、要么都不提,实得 [${langs.join(", ")}]`,
                );
            }
            // 非空跑:为 0 说明 key 名、字典路径或红字措辞变了,上面几格全在空转。
            check(
                mentions > 0,
                `J149 三语红字里至少有一条提到 DAW 兼容表(实得 ${mentions} 处)`,
            );
        }
        // 下面三格与文档地址已无关(J149 起地址不读版本号),留着是因为页脚版本号仍取自
        // 快照的 version.plugin,而那个值由 native 下发。Output 侧此前是字面量 "0.1.0"
        // (#298 复审【重要】)——改了 CMakeLists 的版本号它照旧自报旧版本。
        // mock 里的版本号是另一份,页面侧的格看不见这一跳,只能在源码级钉。
        const outEd = stripComments(src("src/output/OutputEditor.cpp"));
        check(
            /put\(version, "plugin", JucePlugin_VersionString\);/.test(outEd),
            "SL-220 Output 快照的 version.plugin 取 JucePlugin_VersionString(CMake project VERSION)",
        );
        check(
            /config\.version = JucePlugin_VersionString;/.test(outEd),
            "SL-220 Output 首帧 version seed 取 JucePlugin_VersionString",
        );
        check(
            !/"plugin",\s*"/.test(outEd) && !/config\.version = "/.test(outEd),
            "SL-220 OutputEditor.cpp 不再写死版本号字面量",
        );
    }
    {
        // C++ 侧:JUCE 没暴露 WebView2 的 AreDefaultContextMenusEnabled,但**暴露了**
        // newWindowAttemptingToLoad —— 不重写它外链就是死的(JUCE 默认实现什么都不做)。
        const wv = src("src/plugin-common/WebViewHost.cpp");
        check(
            /void newWindowAttemptingToLoad\(const juce::String& newURL\) override/.test(
                wv,
            ),
            "SL-214 C++ 侧重写了 JUCE 的外链虚函数",
        );
        check(
            /launchInDefaultBrowser\(\)/.test(wv),
            "SL-214 外链交给系统浏览器",
        );
        check(
            /startsWithIgnoreCase\("https:\/\/"\)/.test(wv),
            "SL-214 只放行 http/https(外链 = 把字符串交给系统执行,须限协议)",
        );
    }

    // ---- SL-205 增补:悬停即可滚(焦点收拢的三道闸)----
    {
        const tw = src("web/output/tab-wave.js");
        const i = tw.indexOf('els.window.addEventListener("pointerenter"');
        const body = tw.slice(i, i + 900);
        check(i > 0, "SL-205 指针进入泳道窗时收拢焦点");
        check(
            /document\.hasFocus\(\)\) return;/.test(body),
            "SL-205 本文档已有焦点就不动(不抢焦点)",
        );
        // [复审终轮②] 第三道闸改成复用 context-menu 的 isEditableTarget ——
        // 这里原先是全仓**第三份**可编辑判定,且是收窄之前的裸 input 版
        // (会把 type=range 滑杆也当成「正在编辑」)。
        check(
            /isEditableTarget\(document\.activeElement\)/.test(body),
            "SL-205 第三道闸复用 isEditableTarget(不再自带第三份判定)",
        );
        check(
            /from "\.\.\/shared\/context-menu\.js"/.test(tw),
            "SL-205 tab-wave 从 shared 引 isEditableTarget",
        );
    }
}

// =============================================================================
log(
    "=== [SL-415] sidecar 三处 UI 收起:node 侧负向判据 + 「无生产者」事实守卫 ===",
);
// 用户 2026-09-14 裁定「sidecar 不上了」,统筹按最小改动落:**只收 UI,不删代码**
// (变更文档 `docs/contract-changes/20260915-sl415-sidecar-ui-hidden.md`)。
//
// 这里补的是**页面级判据(often SKIP)之外的、不随浏览器缺席而消失的那一半**。
// 页面级 G 节量的是真实布局盒,但 CLAUDE.md §6 的纪律写着浏览器缺席(exit 2)/ 这一次没起来
// (exit 3)都不判红 —— 所以「三处不产生布局盒」这件事在 CI 上有概率一次都断不到;
// 下面两格是**不可跳过**的那一层。
{
    // ---- (1)「不要给它加 show()」那句注释,今天有牙齿了 ----------------------
    // `tab-settings.js` 的 `renderStorage()` 头注写着「本函数照旧填文本,但一个字都不许让它
    // 重新可见」。在本次之前,那句话**没有任何机器拦得住** —— 唯一的守卫是会 SKIP 的页面级
    // 那一格(第 2 轮复审【建议】4)。
    //
    // ⚠ **必须先剥注释再找**:本卡的注释里逐字写着 `show(el.storageCard, …)` 这些形态,
    // 拿裸源码找的话判据会被作者自己的话喂饱(本仓栽过同款;写法与
    // `smoke-tab1-interactions.mjs` 那两格逐字同源)。
    {
        const code = stripComments(src("web/output/tab-settings.js"));
        check(
            code.length > 0,
            "(1) 剥注释后取到 tab-settings.js 正文(取不到就说明下面两格在空跑)",
        );
        // ← 在 tab-settings.js 里加一句 `show($("settings-storage"), true)` / `el.storageCard.hidden = false`,
        //   本格红。**这是本卡唯一的 node 侧牙齿**,别删。
        check(
            !/["']settings-storage["']/.test(code),
            "(1) ★ 剥注释后 tab-settings.js 里**没有任何**对「存储状态」卡锚点的引用" +
                "(settings-storage 是那张卡自己的 data-gb;它只该出现在 index.html 与页面级判据里)",
        );
        check(
            !/storage[A-Za-z]*\.hidden\s*=\s*false/i.test(code),
            "(1) ★ 剥注释后 tab-settings.js 里没有 `…storage….hidden = false`" +
                "(收起只由 index.html 的 `hidden` 承担,JS 侧不再开合)",
        );
    }

    // ---- (2) 裁定 1 立论的那条事实:sidecarMissing 在 src/ 里没有生产者 -------
    // 冻结契约六处(SCVB_CONTRACT §1.1/§5.1×2/§9、STATE_SCHEMA §4.2/§4.3)本次改成「v1 无出口,
    // **因为该 code 在 `src/` 里没有生产者**」。这句话是本变更文档的立论基础,
    // **写进文档就得有人守着** —— 否则哪天有人接了线、而 UI 仍静默,没有任何东西会红。
    //
    // ⚠ **接线那天这一格会红,这是设计好的**:届时必须**同时恢复横幅⑤**,并把这一格改成
    // 「有生产者」的形态(而不是把断言放宽或删掉)。
    {
        const files = [];
        (function walk(dir) {
            for (const ent of readdirSync(dir, { withFileTypes: true })) {
                const p = join(dir, ent.name);
                if (ent.isDirectory()) walk(p);
                else if (ent.isFile()) files.push(p);
            }
        })(join(ROOT, "src"));
        check(
            files.length > 0,
            "(2) 扫到 src/ 下的文件(扫不到就说明下面两格在空跑 —— 路径或 cwd 变了)",
        );
        const hits = [];
        for (const f of files) {
            const lines = readFileSync(f, "utf8").split(/\r?\n/);
            lines.forEach((line, i) => {
                if (line.includes("sidecarMissing")) {
                    hits.push({
                        where: relative(ROOT, f).split("\\").join("/"),
                        line: i + 1,
                        text: line.trim(),
                    });
                }
            });
        }
        // 注释行 = 「提到」,非注释行 = 「可能发出」。判据只看后者。
        //
        // [SL-415 第 2 轮裁定 2] **改成「非注释命中数 == 0」,不再是「命中数恰为 1」**。
        // 理由:旧写法对**同族注释**过敏 —— 以后谁在 `src/` 里顺手再写一句提到这个 code 的
        // 注释(完全无害),那一格就会红,而失败话术写的是「接线那天会红」,把人往
        // 「我去把断言放宽到 <= 2 吧」那一侧带 —— 那正是本格要防的路。放宽到 `<= 2` 等于
        // 把牙齿拔掉;收紧成「非注释 == 0」则**对生产者方向的牙齿一分不减**(真的发出它,
        // 必然是某一行**非注释**代码里出现这个字面量)。
        const isComment = (t) => /^(\/\/|\*|\/\*)/.test(t);
        const codeHits = hits.filter((h) => !isComment(h.text));
        log(
            `  [SL-415] src/ 下 sidecarMissing 命中 ${hits.length} 处` +
                `(其中非注释 ${codeHits.length} 处)` +
                (hits.length
                    ? `:${hits.map((h) => ` ${h.where}:${h.line}${isComment(h.text) ? "(注释)" : "(★代码)"}`).join("")}`
                    : ""),
        );
        // ← 在 src/ 里任何一处**非注释**代码里写上 sidecarMissing(哪怕只多一行字符串字面量),
        //   本格红。那正是「接线了」的信号,必须连同横幅⑤ 一起恢复。
        eq(
            codeHits.length,
            0,
            "(2) ★ src/ 下 sidecarMissing 的**非注释**命中数为 0(接线那天本格会红,见注释;" +
                `实得 ${codeHits.length}` +
                (codeHits.length
                    ? `:${codeHits.map((h) => ` ${h.where}:${h.line}`).join("")}`
                    : "") +
                ")",
        );
        // 反向那一半:今天**必须**至少还有那条待接线注释(sidecarSwitched 一条都没有,
        // 所以这里只钉 sidecarMissing)。删掉它还留着「无生产者」这句话就会变成没人守的断言。
        check(
            hits.length > 0,
            "(2) 对照:src/ 里仍留着那条「待接线」注释(`SidecarStore.h`)—— 它消失时本格红," +
                "免得「非注释命中 == 0」在「连注释都没了」时静默为真",
        );
    }
}

// =============================================================================
log(fail === 0 ? "\n全部通过 ✅" : `\n失败 ${fail} 条 ❌`);
process.exit(fail === 0 ? 0 : 1);
