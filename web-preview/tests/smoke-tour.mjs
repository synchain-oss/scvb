// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— tour(T36b)冒烟(node,无 DOM)
// =============================================================================
// 口径同 smoke-tab1/tab2/tab4:断言面 = 纯函数 + mock 端到端 + 源码级字面断言。
// DOM 侧(蒙版/亮区/说明框定位、点击推进、跨 tab 翻页)归浏览器手测 / Playwright 截图。
//
// 跑什么:
//   ① 步骤清单:全参数导览 44 步、步号连续、首步无 spotlight、末步=review、锚点与 tab 目标逐条对拍;
//   ② shouldShowTourAsk 四种组合(J50a 镜像);
//   ③ buildDemoStore 形状 + 不就地改写深冻结的 FIFTEEN_TRACKS;
//   ④ mock 端到端:first-run-tour 场景(guide 已过、tour_seen=false)+ setTourSeen(true,true)
//     落工程位与全局位,再取快照往返不丢;
//   ⑤ 词条:tour.* 三语齐、占位符三语一致、step4 措辞纪律(无「写入完成」);
//   ⑥ 源码级:零 Audio API、唯一桥调用=setTourSeen、role=dialog、aria-live、
//     Esc=Skip、←/→、左键推进;data-tour 全锚点齐、demo badge / 询问步 / 重看入口落点齐。
//
// 用法:node web-preview/tests/smoke-tour.mjs [仓库根绝对路径]
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

const TOUR = await import(u("web/output/tour.js"));
const LANGSTART = await import(u("web/output/lang-start.js"));
const { createBridge } = await import(u("web/shared/bridge.js"));
const { T } = await import(u("web/shared/i18n.js"));
const { FIFTEEN_TRACKS } = await import(u("web/shared/mock-data.js"));

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
        msg + ": 实得 " + JSON.stringify(a) + ",期望 " + JSON.stringify(b),
    );

// =============================================================================
log("=== ① 步骤清单(全参数导览 44 步)===");
{
    eq(TOUR.TOUR_STEPS.length, 44, "步数 == 44");
    eq(
        TOUR.TOUR_ANCHORS,
        [
            null,
            null,
            "tab1",
            "group",
            "cap",
            "an",
            "out",
            "dist",
            "width",
            "msbalance",
            "leadselect",
            "range",
            "transition",
            "curve",
            "tab2",
            "trackrow",
            "pan",
            "widthknob",
            "vollevel",
            "prio",
            "leadlock",
            "volexempt",
            "autopan",
            "pair",
            "freeze",
            "enable",
            "tab3",
            "vad",
            "segments",
            "actions",
            "lanes",
            "selection",
            "wave-panel",
            "inspector",
            "tab4",
            "guideblock",
            "loudnessmode",
            "centerslot",
            "scale",
            "lang",
            "storage",
            "diagnostic",
            "version",
            "review",
        ],
        "44 锚点(含步 1/2 两个居中卡 null),末步=review",
    );
    eq(
        TOUR.TOUR_STEPS[0].anchor,
        null,
        "首步 = 欢迎公告(无 spotlight,居中大卡)",
    );
    eq(TOUR.TOUR_STEPS[0].tab, "master", "首步欢迎公告在整体调整页");
    eq(
        TOUR.TOUR_STEPS[1].anchor,
        null,
        "第 2 步 = 工作流程与优先级(居中大卡,无 spotlight)",
    );
    eq(
        TOUR.TOUR_STEPS.map((s) => s.tab),
        [
            "master",
            "master",
            "master",
            "master",
            "master",
            "master",
            "master",
            "master",
            "master",
            "master",
            "master",
            "master",
            "master",
            "master",
            "tracks",
            "tracks",
            "tracks",
            "tracks",
            "tracks",
            "tracks",
            "tracks",
            "tracks",
            "tracks",
            "tracks",
            "tracks",
            "tracks",
            "wave",
            "wave",
            "wave",
            "wave",
            "wave",
            "wave",
            "wave",
            "wave",
            "settings",
            "settings",
            "settings",
            "settings",
            "settings",
            "settings",
            "settings",
            "settings",
            "settings",
            "settings",
        ],
        "跨 tab 翻页目标逐条对拍(14 master / 12 tracks / 8 wave / 10 settings)",
    );
    eq(
        TOUR.TOUR_STEPS[43].anchor,
        "review",
        "末步固定 = 设置页「重看引导」入口",
    );
    eq(TOUR.TOUR_STEPS[5].anchor, "an", "第 6 步 = 三件套「02 分析」段");
    eq(TOUR.TOUR_STEPS[30].action, "zoomLanes", "步 31 泳道区自动放大泳道");
    eq(
        TOUR.TOUR_STEPS[31].action,
        "showDemoSelection",
        "步 32 选区手柄自动创建示例选区",
    );
    eq(
        TOUR.TOUR_STEPS[33].action,
        "showDemoSegment",
        "步 34 段检查器自动选中示例段",
    );
    eq(TOUR.TOUR_STEPS[35].action, "expandGuide", "步 36 自动展开九条");
    check(TOUR.SPOT_PAD >= 8, "spotlight 内边距 ≥8 设计 px");
}

// =============================================================================
log("=== ② shouldShowTourAsk(J50a 镜像)===");
{
    eq(
        TOUR.shouldShowTourAsk(
            { ui: { tour_seen: false } },
            { tour_seen_global: false },
        ),
        true,
        "工程未看过 ∧ 全局未看过 ⇒ 弹询问步",
    );
    eq(
        TOUR.shouldShowTourAsk(
            { ui: { tour_seen: false } },
            { tour_seen_global: true },
        ),
        false,
        "全局已置 ⇒ 新工程不再自动询问",
    );
    eq(
        TOUR.shouldShowTourAsk(
            { ui: { tour_seen: true } },
            { tour_seen_global: false },
        ),
        false,
        "本工程已看过 ⇒ 不弹",
    );
    eq(
        TOUR.shouldShowTourAsk({ ui: { tour_seen: false } }, null),
        false,
        "首帧未到(§0.6 门控)⇒ 不弹",
    );

    // 启动兜底:guide 已见 + tour 未置位 ⇒ 直接显示询问步(不需「开始使用」点击)
    eq(
        TOUR.shouldAutoShowTourAsk(
            { ui: { guide_seen: true, tour_seen: false } },
            { tour_seen_global: false },
        ),
        true,
        "guide 已见 ∧ tour 未置位 ⇒ 兜底直弹询问步",
    );
    eq(
        TOUR.shouldAutoShowTourAsk(
            { ui: { guide_seen: false, tour_seen: false } },
            { tour_seen_global: false },
        ),
        false,
        "guide 未看 ⇒ 走红字页路径,不兜底直弹",
    );
    eq(
        TOUR.shouldAutoShowTourAsk(
            { ui: { guide_seen: true, tour_seen: true } },
            { tour_seen_global: false },
        ),
        false,
        "tour 已置位 ⇒ 不弹",
    );
    eq(
        TOUR.shouldAutoShowTourAsk(
            { ui: { guide_seen: true, tour_seen: false } },
            { tour_seen_global: true },
        ),
        false,
        "全局默认已置 ⇒ 不弹",
    );
    eq(
        TOUR.shouldAutoShowTourAsk(
            { ui: { guide_seen: true, tour_seen: false } },
            null,
        ),
        false,
        "首帧未到 ⇒ 不兜底直弹",
    );
}

// =============================================================================
log("=== ③ buildDemoStore(纯 UI 展示层)===");
{
    const snapBefore = FIFTEEN_TRACKS.snapshot;
    const ds = TOUR.buildDemoStore();
    check(ds.ready === true, "demo store ready=true");
    check(ds.snapshot === snapBefore, "snapshot 引用复用(只读)");
    check(ds.state !== snapBefore, "state 子树为独立对象");
    check(ds.session && typeof ds.session === "object", "session 独立可变对象");
    eq(ds.conn, snapBefore.conn, "conn 复用 demo 快照");
    check(ds.state.ui.guide_seen === true, "demo 里 guide 已过(引导页不弹)");
    check(ds.state.ui.tour_seen === false, "demo 里 tour 未看(讲解前提)");
    check(Object.isFrozen(snapBefore), "FIFTEEN_TRACKS.snapshot 深冻结");
}

// =============================================================================
log("=== ④ mock 端到端:first-run-tour + setTourSeen ===");
{
    const driver = await import(u("web-preview/mock/state-driver.js"));
    const session = driver.createPreviewSession({
        role: "output",
        params: "scenario=first-run-tour",
    });
    check(
        session.info.fixture === "fifteen-tracks",
        "first-run-tour 落在 fifteen-tracks 世界",
    );
    const bridge = createBridge({ role: "output", mockBackend: session.mock });
    session.start();
    const snap = await bridge.requestInitialState();
    // first-run-tour 复现完整首启链:两级 guide_seen 与 tour_seen 全 false
    check(
        snap && snap.ui && snap.ui.guide_seen === false,
        "guide_seen=false(红字页会弹,完整链起点)",
    );
    check(
        snap && snap.guide_seen_global === false,
        "guide_seen_global=false(全局默认未置位)",
    );
    check(
        snap && snap.ui && snap.ui.tour_seen === false,
        "tour_seen=false(可询问/可重看)",
    );
    check(
        snap && snap.tour_seen_global === false,
        "tour_seen_global=false(默认)",
    );

    eq(
        await bridge.setTourSeen(true, true),
        { ok: true },
        "setTourSeen(true, true)",
    );
    await sleep(60);
    const snap2 = await bridge.requestInitialState();
    eq(snap2.ui.tour_seen, true, "工程位 tour_seen 已置");
    eq(
        snap2.tour_seen_global,
        true,
        "全局位 tour_seen_global 已置(换新工程不再自动询问)",
    );
    eq(
        TOUR.shouldShowTourAsk({ ui: { tour_seen: false } }, snap2),
        false,
        "全局位已置 ⇒ 新工程不弹询问步(J50a 镜像)",
    );
    session.stop();
}

// =============================================================================
log("=== ⑤ 词条:tour.* 三语 ===");
{
    const KEYS = [
        "ask",
        "ask.start",
        "ask.later",
        "clickAnywhere",
        "skip",
        "prev",
        "next",
        "done",
        "demoBadge",
    ];
    for (let i = 1; i <= 44; i++) {
        KEYS.push("step" + i + ".title", "step" + i + ".body");
    }
    for (const k of KEYS) {
        const key = "tour." + k;
        for (const lang of ["zh", "en", "fr"]) {
            check(
                typeof T[lang][key] === "string" && T[lang][key].length > 0,
                lang + "." + key + " 非空",
            );
        }
        const ph = (s) => (String(s).match(/\{\w+\}/g) || []).sort().join(",");
        eq(ph(T.en[key]), ph(T.zh[key]), key + " en/zh 占位符一致");
        eq(ph(T.fr[key]), ph(T.zh[key]), key + " fr/zh 占位符一致");
    }
    // 措辞纪律:输出开关一步不得出现「写入完成」(05 §2.6 / J45)
    check(
        !/写入完成/.test(T.zh["tour.step4.body"]),
        "zh step4 不含「写入完成」",
    );
    check(
        !/write complete|written to/i.test(T.en["tour.step4.body"]),
        "en step4 不含 write complete / written",
    );
    // 组这一步只讲组的作用(用户实机发现:组选择器在三件套**下方**,原「下方/从左到右」方向表述有误,已删)
    check(
        !/从左到右|下方三个模块/.test(T.zh["tour.step4.body"]),
        "zh step4 无「下方/从左到右」方向表述",
    );
    check(
        /组把通道分成 A–H 八个独立工作区/.test(T.zh["tour.step4.body"]),
        "zh step4 只讲组的作用",
    );
    check(
        !/left to right/i.test(T.en["tour.step4.body"]),
        "en step4 无 left to right 方向表述",
    );
    check(
        !/gauche à droite/i.test(T.fr["tour.step4.body"]),
        "fr step4 无 gauche à droite 方向表述",
    );
    check(
        /左键点击任意处/.test(T.zh["tour.clickAnywhere"]),
        "zh clickAnywhere 交互说明",
    );
}

// =============================================================================
log("=== ⑥ 源码级:零 Audio / 唯一桥调用 / a11y / 六锚点 ===");
{
    const ts = src("web/output/tour.js");
    const html = src("web/output/index.html");
    const tracks = src("web/output/tab-tracks.js");
    const wave = src("web/output/tab-wave.js");

    // 无声音:零 Audio API 调用(§2.6 验收⑦)
    const audio = [
        "AudioContext",
        "webkitAudioContext",
        "new Audio",
        "createOscillator",
        "OscillatorNode",
        "AudioBuffer",
        "AudioParam",
        "createGain",
    ];
    for (const a of audio) {
        check(!ts.includes(a), "tour.js 零 Audio API:" + a);
    }

    // 除 setTourSeen 外不发任何桥函数(§2.6 demo 注入纪律)
    const calls = [...ts.matchAll(/call\("([a-zA-Z]+)"/g)].map((m) => m[1]);
    check(
        calls.length === 1 && calls[0] === "setTourSeen",
        "唯一桥调用 = setTourSeen,实得 " + JSON.stringify(calls),
    );

    // a11y:说明框 role=dialog + 步骤文案 aria-live 播报
    check(ts.includes('"dialog"'), "role=dialog");
    check(ts.includes('"polite"'), "aria-live=polite 播报");

    // 键盘:Esc=Skip,←/→=上一步/下一步
    check(ts.includes('"Escape"'), "Esc=Skip");
    check(ts.includes('"ArrowRight"'), "→ = 下一步");
    check(ts.includes('"ArrowLeft"'), "← = 上一步");

    // 左键点击任意处 = 下一步(右键/滚轮不推进)
    check(ts.includes("e.button !== 0"), "仅左键推进(button===0)");
    check(ts.includes('closest("[data-tour-btn]")'), "说明框按钮例外");

    // data-tour 锚点:index.html 静态锚 + tab-tracks.js 首行动态锚(dt() 助手)
    const staticAnchors = [
        "tab1",
        "tab2",
        "tab3",
        "tab4",
        "group",
        "cap",
        "an",
        "out",
        "dist",
        "width",
        "msbalance",
        "leadselect",
        "range",
        "transition",
        "curve",
        "lanes",
        "selection",
        "actions",
        "inspector",
        "segments",
        "vad",
        "wave-panel",
        "guideblock",
        "review",
        "loudnessmode",
        "centerslot",
        "scale",
        "lang",
        "storage",
        "diagnostic",
        "version",
    ];
    for (const a of staticAnchors) {
        check(
            html.includes('data-tour="' + a + '"'),
            "index.html data-tour=" + a,
        );
    }
    const trackAnchors = [
        "trackrow",
        "pan",
        "widthknob",
        "vollevel",
        "prio",
        "leadlock",
        "volexempt",
        "autopan",
        "pair",
        "enable",
    ];
    for (const a of trackAnchors) {
        check(
            tracks.includes('dt("' + a + '")'),
            "tab-tracks.js dt(" + a + ")",
        );
    }
    check(
        tracks.includes('data-tour="freeze"'),
        "tab-tracks.js data-tour=freeze(第 1 行冻结组)",
    );

    // 步 28/29 视图层增强:tab-wave.js 暴露三个纯视图函数;tour.js 结束/离开还原
    check(wave.includes("zoomLanes"), "tab-wave.js 暴露 zoomLanes");
    check(
        wave.includes("showDemoSelection"),
        "tab-wave.js 暴露 showDemoSelection",
    );
    check(wave.includes("showDemoSegment"), "tab-wave.js 暴露 showDemoSegment");
    check(wave.includes("resetWaveView"), "tab-wave.js 暴露 resetWaveView");
    check(
        ts.includes("resetWaveView()"),
        "tour.js 结束/离开调用 resetWaveView 还原",
    );

    // demo badge / 询问步 / 重看入口 落点
    check(
        html.includes('data-gb="header-demo-chip"'),
        "header demo badge 落点",
    );
    check(html.includes('data-gb="tour-ask"'), "询问步 overlay 落点");
    check(
        html.includes('data-gb="settings-reopentour"'),
        "「重看引导」入口落点",
    );
}

// =============================================================================
log("=== ⑦ 首启语言选择卡(lang-start,独立 overlay)===");
{
    // 纯函数:判据与 shouldShowGuide 同构 + 本会话已选标记
    eq(
        LANGSTART.shouldShowLangStart(
            { ui: { guide_seen: false } },
            { guide_seen_global: false },
            false,
            false,
        ),
        true,
        "guide 未看 ∧ 全局默认未置 ∧ 未选语言 ⇒ 显示语言卡",
    );
    eq(
        LANGSTART.shouldShowLangStart(
            { ui: { guide_seen: true } },
            { guide_seen_global: false },
            false,
            false,
        ),
        false,
        "guide 已看(重看引导/兜底)⇒ 不显示语言卡",
    );
    eq(
        LANGSTART.shouldShowLangStart(
            { ui: { guide_seen: false } },
            { guide_seen_global: true },
            false,
            false,
        ),
        false,
        "全局默认已置(不再显示)⇒ 不显示语言卡",
    );
    eq(
        LANGSTART.shouldShowLangStart(
            { ui: { guide_seen: false } },
            { guide_seen_global: false },
            true,
            false,
        ),
        false,
        "本会话已关过引导页 ⇒ 不显示",
    );
    eq(
        LANGSTART.shouldShowLangStart(
            { ui: { guide_seen: false } },
            { guide_seen_global: false },
            false,
            true,
        ),
        false,
        "本会话已选过语言 ⇒ 不重复显示",
    );
    eq(
        LANGSTART.shouldShowLangStart(
            { ui: { guide_seen: false } },
            null,
            false,
            false,
        ),
        false,
        "首帧未到(§0.6 门控)⇒ 不显示",
    );
    eq(
        LANGSTART.LANG_PICK_CODES,
        ["zh", "en", "fr"],
        "语言卡三按钮 = zh / en / fr",
    );

    const html = src("web/output/index.html");
    const ls = src("web/output/lang-start.js");
    const audio = [
        "AudioContext",
        "webkitAudioContext",
        "new Audio",
        "createOscillator",
        "OscillatorNode",
        "AudioBuffer",
        "AudioParam",
        "createGain",
    ];
    for (const a of audio) {
        check(!ls.includes(a), "lang-start.js 零 Audio API:" + a);
    }
    // 零桥调用(语言切换走 app.js 的 setLang,本文件不触引擎)
    const calls = [...ls.matchAll(/call\("([a-zA-Z]+)"/g)].map((m) => m[1]);
    check(
        calls.length === 0,
        "lang-start.js 零桥调用,实得 " + JSON.stringify(calls),
    );
    // a11y:role=dialog + aria-modal + aria-labelledby
    check(ls.includes('"dialog"'), "lang-start role=dialog");
    check(
        ls.includes('"true"') && ls.includes("aria-modal"),
        "lang-start aria-modal",
    );
    check(ls.includes("aria-labelledby"), "lang-start aria-labelledby");

    // i18n:lang-start.* 三语齐(标题三语常显 + 三按钮各用各自语言)
    for (const k of ["title", "zh", "en", "fr"]) {
        const key = "lang-start." + k;
        for (const lang of ["zh", "en", "fr"]) {
            check(
                typeof T[lang][key] === "string" && T[lang][key].length > 0,
                lang + "." + key + " 非空",
            );
        }
    }

    // index.html:header 语言胶囊不再是 tour 锚点(恢复普通控件)
    check(
        !html.includes('data-tour="langpick"'),
        "header 语言胶囊已移除 data-tour=langpick",
    );
}

// =============================================================================
log(fail === 0 ? "\n全部通过 ✅" : "\n失败 " + fail + " 条 ❌");
process.exit(fail === 0 ? 0 : 1);
