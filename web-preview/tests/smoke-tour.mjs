// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— tour(T36b)冒烟(node,无 DOM)
// =============================================================================
// 口径同 smoke-tab1/tab2/tab4:断言面 = 纯函数 + mock 端到端 + 源码级字面断言。
// DOM 侧(蒙版/亮区/说明框定位、点击推进、跨 tab 翻页)归浏览器手测 / Playwright 截图。
//
// 跑什么:
//   ① 步骤清单:全参数导览 39 步、步号连续、首步无 spotlight、末步=review、锚点与 tab 目标逐条对拍;
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
log("=== ① 步骤清单(全参数导览 39 步)===");
{
    eq(TOUR.TOUR_STEPS.length, 39, "步数 == 39");
    eq(
        TOUR.TOUR_ANCHORS,
        [
            "tab1",
            "group",
            "cap",
            "an",
            "out",
            "width",
            "msbalance",
            "leadselect",
            "range",
            "transition",
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
            "lanes",
            "selection",
            "actions",
            "inspector",
            "segments",
            "vad",
            "wave-panel",
            "tab4",
            "guideblock",
            "loudnessmode",
            "centerslot",
            "scale",
            "lang",
            "storage",
            "diagnostic",
            "review",
        ],
        "39 锚点连续无跳号,末步=review",
    );
    eq(
        TOUR.TOUR_STEPS[0].anchor,
        "tab1",
        "首步 spotlight = tab1 页签(整体调整)",
    );
    eq(TOUR.TOUR_STEPS[0].tab, "master", "首步在整体调整页");
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
        ],
        "跨 tab 翻页目标逐条对拍(10 master / 12 tracks / 8 wave / 9 settings)",
    );
    eq(
        TOUR.TOUR_STEPS[38].anchor,
        "review",
        "末步固定 = 设置页「重看引导」入口",
    );
    eq(TOUR.TOUR_STEPS[3].anchor, "an", "第 4 步 = 三件套「02 分析」段");
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
    for (let i = 1; i <= 39; i++) {
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
    // 首步正文含三件套主线 + clickAnywhere 独立词条
    check(
        /采集 → 分析 → 输出/.test(T.zh["tour.step2.body"]),
        "zh step2 含三件套主线",
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
        "width",
        "msbalance",
        "leadselect",
        "range",
        "transition",
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
log(fail === 0 ? "\n全部通过 ✅" : "\n失败 " + fail + " 条 ❌");
process.exit(fail === 0 ? 0 : 1);
