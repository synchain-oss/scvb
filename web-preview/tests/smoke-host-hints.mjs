// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 宿主专属提示冒烟(node,无 DOM;[J150] ①,03 §4.2 REAPER / §4.4 Live)
// =============================================================================
// 与既有 node 冒烟同款口径:仓内零 node_modules,断言面是**纯函数 + mock 端到端 + 源码钉子**。
// 行为面(横幅真的出 / 关得掉 / 边沿对不对)在页面级 `smoke-host-hints-page.mjs`;那一套缺浏览器时
// 整套 SKIP,这里留下**不会 SKIP** 的那一半。
//
// 跑什么:
//   ① `web/output/host-hints.js` 的三个纯函数:宿主 × 输出开关 × 会话闩锁的真值表
//      (只有 reaper / live 出提示;cubase / other / 不认识的值 / 字段缺席一律不出),
//      以及打印边沿记账的逐拍序列;
//   ② mock 端到端:`?host=` 解析(合法值落进 §1.1 快照、非法值出警告回落 other)、
//      默认 other、§2.1 全量帧里**没有** `host`(它是快照专属键);
//   ③ 词条:三条 × 三语齐备非空;zh 用词随 [J88]「写入自动化」、不出现「打印」「车道」;
//      REAPER 首选项路径与 docs/DAW_COMPATIBILITY.md 逐字一致(用户照着找的是那串英文);
//   ④ 源码钉子:`OutputEditor::buildSnapshot` 把 `processor_.hostId()` 放进 `host` 键
//      (那个 TU 编不进任何 C++ 测试目标,见 smoke-tab2-interactions 同款钉子的头注);
//      三条横幅在模板里带 ✕、在 app.js 里走 showDismissible、在 ✕ 接线名单里。
//
// 用法:node web-preview/tests/smoke-host-hints.mjs [仓库根绝对路径]
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

const HH = await import(u("web/output/host-hints.js"));
const { T } = await import(u("web/shared/i18n.js"));
const MD = await import(u("web/shared/mock-data.js"));
const driver = await import(u("web-preview/mock/state-driver.js"));
const { stripJsComments } = await import(u("scripts/lib/strip-comments.mjs"));

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

const ON = { global: { output_enabled: true } };
const OFF = { global: { output_enabled: false } };
const BANNERS = [
    ["banner-reaperKeepOpen", "banner.reaperKeepOpen", "reaperKeepOpen"],
    ["banner-reaperPrintNote", "banner.reaperPrintNote", "reaperPrintNote"],
    ["banner-liveReEnable", "banner.liveReEnable", "liveReEnable"],
];

// =============================================================================
log("=== ① host-hints.js 纯函数 ===");
{
    // snapshotHost:缺席 / 非字符串 ⇒ other
    eq(HH.snapshotHost(null), "other", "snapshotHost(null)");
    eq(HH.snapshotHost({}), "other", "snapshotHost({}) —— 字段缺席");
    eq(HH.snapshotHost({ host: 3 }), "other", "snapshotHost 非字符串");
    eq(HH.snapshotHost({ host: "reaper" }), "reaper", "snapshotHost reaper");
    eq(HH.snapshotHost({ host: "live" }), "live", "snapshotHost live");

    // 真值表:会话闩锁两格都置上,只看宿主 × 输出开关。
    const both = { hintEverPrinted: true, hintPrintEnded: true };
    const table = [
        // host,      state, 期望 [keepOpen, printNote, liveReEnable]
        ["reaper", ON, [true, true, false]],
        ["reaper", OFF, [false, true, false]],
        ["live", ON, [false, false, true]],
        ["live", OFF, [false, false, true]],
        ["cubase", ON, [false, false, false]],
        ["cubase", OFF, [false, false, false]],
        ["other", ON, [false, false, false]],
        // 不认识的值 / 大小写不同 —— 一律不出(闭集,页面不猜)
        ["studioone", ON, [false, false, false]],
        ["REAPER", ON, [false, false, false]],
        ["Live", ON, [false, false, false]],
        ["", ON, [false, false, false]],
    ];
    for (const [host, state, want] of table) {
        const f = HH.hostHintFlags(host, state, both);
        eq(
            [f.reaperKeepOpen, f.reaperPrintNote, f.liveReEnable],
            want,
            `hostHintFlags(${JSON.stringify(host)}, output ${state.global.output_enabled ? "ON" : "OFF"})`,
        );
    }
    // 会话闩锁没置 ⇒ ⑫⑬ 不出(⑪ 不看闩锁)
    const none = HH.hostHintFlags("reaper", ON, {});
    eq(
        [none.reaperKeepOpen, none.reaperPrintNote],
        [true, false],
        "REAPER 未进过 PRINT:⑪ 出、⑫ 不出",
    );
    eq(
        HH.hostHintFlags("live", ON, {}).liveReEnable,
        false,
        "Live 未结束过打印:⑬ 不出",
    );
    // state / session 缺席不抛
    const safe = HH.hostHintFlags("reaper", null, null);
    eq(
        [safe.reaperKeepOpen, safe.reaperPrintNote, safe.liveReEnable],
        [false, false, false],
        "state / session 缺席 ⇒ 全不出,不抛",
    );

    // 打印边沿逐拍:follow → print → print → armed → armed → print → follow
    const s = {};
    const seq = [false, true, true, false, false, true, false];
    const got = seq.map((p) => {
        HH.trackPrintEdges(s, p);
        return [s.hintEverPrinted === true, s.hintPrintEnded === true];
    });
    eq(
        got,
        [
            [false, false], // 还没进过
            [true, false], // 进 PRINT:闩上「进过」
            [true, false],
            [true, true], // PRINT → 非 PRINT:置「刚结束」
            [true, true], // 保持
            [true, false], // 再进 PRINT:清「刚结束」
            [true, true], // 再结束
        ],
        "trackPrintEdges 逐拍 [hintEverPrinted, hintPrintEnded]",
    );
    // 从未打印过就一直非 PRINT ⇒ 不会凭空置「刚结束」
    const s2 = {};
    for (let i = 0; i < 3; i++) HH.trackPrintEdges(s2, false);
    eq(s2.hintPrintEnded === true, false, "从未进过 PRINT ⇒ 不置「刚结束」");
    HH.trackPrintEdges(null, true); // 不抛

    eq(
        [...HH.HOST_HINT_BANNERS],
        BANNERS.map((b) => b[0]),
        "HOST_HINT_BANNERS = 三条横幅锚点名",
    );
}

// =============================================================================
log("=== ② mock 端到端(?host=)===");
{
    eq(
        [...driver.HOST_VALUES],
        ["reaper", "live", "cubase", "other"],
        "HOST_VALUES = 契约 §1.1 `host` 闭集",
    );
    eq(MD.makeOutputSnapshot().host, "other", "快照生成器默认 host = other");
    for (const h of ["reaper", "live", "cubase", "other"]) {
        const q = driver.parsePreviewQuery(`host=${h}`);
        eq(q.host, h, `parsePreviewQuery host=${h}`);
        eq(q.warnings.length, 0, `host=${h} 不出警告`);
    }
    const bad = driver.parsePreviewQuery("host=bogus");
    eq(bad.host, null, "host=bogus ⇒ null");
    check(
        bad.warnings.some((w) => w.includes("host=bogus")),
        "host=bogus 出警告(不静默吞)",
    );
    eq(driver.parsePreviewQuery("").host, null, "不给 host ⇒ null");
    // 原型链上的名字不算合法值
    eq(
        driver.parsePreviewQuery("host=constructor").host,
        null,
        "host=constructor ⇒ null",
    );

    // 接线:URL → buildWorld → 快照 → requestInitialState()
    for (const h of ["reaper", "live", "cubase"]) {
        const sess = driver.createPreviewSession({
            role: "output",
            params: `scenario=connected&host=${h}`,
        });
        eq(
            sess.mock.requestInitialState().host,
            h,
            `?host=${h} ⇒ requestInitialState().host`,
        );
        // §2.1 全量帧不带 host(快照专属键)
        const full = sess.ctl.fullStatePayload();
        check(
            !Object.prototype.hasOwnProperty.call(full, "host"),
            `?host=${h}:scvb.state 全量帧里没有 host`,
        );
        sess.stop();
    }
    const dflt = driver.createPreviewSession({
        role: "output",
        params: "scenario=connected",
    });
    eq(dflt.mock.requestInitialState().host, "other", "不给 host ⇒ other");
    dflt.stop();
    // 导览 demo 快照:宿主 other(导览期三条恒不出的前提)
    eq(MD.FIFTEEN_TRACKS.snapshot.host, "other", "导览 demo 快照 host = other");
}

// =============================================================================
log("=== ③ 词条(三语)===");
{
    for (const [, key] of BANNERS) {
        for (const lang of ["zh", "en", "fr"]) {
            const v = T[lang] && T[lang][key];
            check(
                typeof v === "string" && v.trim().length > 0,
                `${lang}.${key} 存在且非空`,
            );
        }
    }
    // 宿主名打头,一眼看得出这条只跟哪个宿主有关
    for (const lang of ["zh", "en", "fr"]) {
        check(
            T[lang]["banner.reaperKeepOpen"].startsWith("REAPER"),
            `${lang} ⑪ 以 REAPER 打头`,
        );
        check(
            T[lang]["banner.reaperPrintNote"].startsWith("REAPER"),
            `${lang} ⑫ 以 REAPER 打头`,
        );
        check(
            T[lang]["banner.liveReEnable"].startsWith("Live"),
            `${lang} ⑬ 以 Live 打头`,
        );
        check(
            T[lang]["banner.liveReEnable"].includes("Re-Enable Automation"),
            `${lang} ⑬ 带 Live 按钮原名 Re-Enable Automation`,
        );
        // ⑫ 的首选项路径与 DAW_COMPATIBILITY §2.2 逐字一致(英文原文,各语言都不译)
        for (const frag of [
            "Preferences → Plug-ins → VST → VST compatibility",
            "Parameter automation notifications",
            "process all notifications",
        ]) {
            check(
                T[lang]["banner.reaperPrintNote"].includes(frag),
                `${lang} ⑫ 含「${frag}」`,
            );
        }
    }
    const daw = src("docs/DAW_COMPATIBILITY.md");
    check(
        daw.includes(
            "Preferences → Plug-ins → VST → VST compatibility → Parameter automation notifications = process all notifications",
        ),
        "DAW_COMPATIBILITY.md 仍写着同一条 REAPER 首选项路径(两处不许各说各的)",
    );
    // zh 用词:[J88] 输出 ON 档叫「写入自动化」;[J97] 不用「车道」
    for (const [, key] of BANNERS) {
        const v = T.zh[key];
        check(!v.includes("打印"), `zh.${key} 不出现「打印」([J88] 用词)`);
        check(!v.includes("车道"), `zh.${key} 不出现「车道」([J97] 用词)`);
    }
}

// =============================================================================
log("=== ④ 源码钉子 ===");
{
    // ---- native:buildSnapshot 把 hostId() 放进 host 键 -----------------------
    // 取**这一个函数自己的**函数体(定义处到下一个顶格 `}`),免得别的函数里一行同名
    // put 把本格喂饱。行形态约束 `^\s+…$`:注释里写同一串不会被当成代码(见
    // smoke-tab2-interactions [SL-199] 那组钉子的头注)。
    const oe = src("src/output/OutputEditor.cpp");
    const start = oe.indexOf("juce::var OutputEditor::buildSnapshot()");
    const end = start < 0 ? -1 : oe.indexOf("\n}", start);
    const body = start >= 0 && end > start ? oe.slice(start, end) : "";
    check(body.length > 0, "取到 OutputEditor::buildSnapshot 函数体");
    check(
        /^\s+put\(o, "host", juce::var\(processor_\.hostId\(\)\)\);\s*$/m.test(
            body,
        ),
        '[J150] ★ buildSnapshot 里有 `put(o, "host", juce::var(processor_.hostId()));` —— ' +
            "删掉这一行,页面永远拿不到宿主、三条提示在任何宿主上都不出",
    );
    const op = src("src/output/OutputProcessor.cpp");
    check(
        /^\s+hostId_ = scvb::output::hostIdOf\(/m.test(op),
        "[J150] OutputProcessor 构造期把 hostIdOf(...) 写进 hostId_(逐支行为由 HOST J150 用例钉)",
    );

    // ---- web:模板 / 渲染 / ✕ 接线 ------------------------------------------------
    const html = src("web/output/index.html");
    const appCode = stripJsComments(
        src("web/output/app.js"),
        "web/output/app.js",
    );
    const flat = appCode.replace(/\s+/g, "");
    const listStart = flat.indexOf("DISMISSIBLE_BANNERS=[");
    const listEnd = flat.indexOf("]", listStart);
    const listSrc =
        listStart >= 0 && listEnd > listStart
            ? flat.slice(listStart, listEnd)
            : "";
    check(listSrc.length > 0, "取到 DISMISSIBLE_BANNERS 那个数组");
    check(
        listSrc.includes("...HOST_HINT_BANNERS"),
        "DISMISSIBLE_BANNERS 并进了 HOST_HINT_BANNERS(✕ 的 handler 与焦点交接都读这一份)",
    );
    for (const [gb, key, flag] of BANNERS) {
        check(html.includes(`data-gb="${gb}"`), `${gb} 在模板里`);
        check(html.includes(`data-gb="${gb}-dismiss"`), `${gb} 带 ✕ 钮`);
        check(html.includes(`data-t="${key}"`), `${gb} 的文案走词条 ${key}`);
        check(
            flat.includes(`showDismissible("${gb}",hints.${flag},`),
            `${gb} 走 showDismissible,条件取 hostHintFlags(...).${flag}`,
        );
    }
    // host 是快照专属键:不进 state 子树(§1.1 语义行)
    check(
        /host:_host,/.test(flat),
        "bootInner 把 host 从快照里摘出来、不并进 store.state",
    );
}

// =============================================================================
log(fail === 0 ? "\n=== 结果:全部通过 ===" : `\n=== 结果:${fail} 项失败 ===`);
process.exit(fail === 0 ? 0 : 1);
