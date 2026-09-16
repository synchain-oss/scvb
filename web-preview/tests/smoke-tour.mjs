// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— tour(T36b)冒烟(node,无 DOM)
// =============================================================================
// 口径同 smoke-tab1/tab2/tab4:断言面 = 纯函数 + mock 端到端 + 源码级字面断言。
// DOM 侧(蒙版/亮区/说明框定位、点击推进、跨 tab 翻页)归浏览器手测 / Playwright 截图。
//
// 跑什么:
//   ① 步骤清单:全参数导览 43 步、步号连续、首步无 spotlight、末步=review、锚点与 tab 目标逐条对拍;
//      [SL-415] 并向**反方向**断一条:步骤表里不再含 `storage` 锚点(用户 2026-09-14 裁定
//      「sidecar 不上了」⇒ 那一步整步移除)。单靠「锚点数组逐条对拍」挡不住它回来 ——
//      把 storage 插回表里、再照抄进那份期望数组,两处一起改就是全绿;这条负向断言才是牙齿。
//   ② shouldShowTourAsk 四种组合(J50a 镜像)+ 兜底直弹的「本会话已答」闸(T37 bug A-2);
//   ③ buildDemoStore 形状 + 不就地改写深冻结的 FIFTEEN_TRACKS;
//   ④ mock 端到端:first-run-tour 场景(guide 已过、tour_seen=false)+ setTourSeen(true,true)
//     落工程位与全局位,再取快照往返不丢;
//   ⑤ 词条:tour.* 三语齐、占位符三语一致、step8 措辞纪律(无「写入完成」);
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
const LANGSTART = await import(u("web/shared/lang-start.js"));
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
log("=== ① 步骤清单(全参数导览 43 步)===");
{
    eq(
        TOUR.TOUR_STEPS.length,
        43,
        "步数 == 43(原 44;SL-415 移除了 storage 那一步)",
    );
    eq(
        TOUR.TOUR_ANCHORS,
        [
            null,
            null,
            "version",
            "tab1",
            "cap",
            "an",
            "out",
            "group",
            "width",
            "msbalance",
            "leadselect",
            "dist",
            "transition",
            "range",
            "curve",
            "tab2",
            "trackrow",
            "pan",
            "widthknob",
            "vollevel",
            "prio",
            "leadlock",
            "pair",
            "volexempt",
            "autopan",
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
            "diagnostic",
            "review",
        ],
        "43 锚点(含步 1/2 两个居中卡 null),末步=review",
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
        ],
        "跨 tab 翻页目标逐条对拍(15 master / 12 tracks / 8 wave / 8 settings;" +
            "settings 由 9 降为 8 —— SL-415 移除了 storage 那一步)",
    );
    eq(
        TOUR.TOUR_STEPS[42].anchor,
        "review",
        "末步固定 = 设置页「重看引导」入口",
    );
    // [SL-415] 负向那一条:步骤表里不再含 `storage` 锚点(用户 2026-09-14 裁定
    // 「sidecar 不上了」⇒ 设置页「存储状态」那一步整步移除,44 → 43)。
    // ⚠ 为什么不能只靠上面那份数组对拍:把 `{anchor:"storage"}` 插回表里、再顺手把
    // "storage" 抄回期望数组,**两处一起改照样全绿** —— 而 DOM 里那张卡恒 `hidden`,
    // 那一步的 spotlight 会挖出一个空处。这条断言是唯一挡得住「悄悄加回来」的东西。
    // 删除式:把 `{ anchor: "storage", tab: "settings" }` 插回 tour.js 步骤表
    // (原第 42 位)⇒ 本格红(而上面那份对拍只要不改期望数组,反而会先红在步数上 ——
    // 所以这条补的是「两处一起改」那条路)。
    check(
        !TOUR.TOUR_ANCHORS.includes("storage"),
        "★ 步骤表里不再含 storage 锚点(原第 42 步已被 SL-415 整步移除)",
    );
    // 反面对照「DOM 里那个锚点还在」在 §⑥ 那一节断(那里才读得到 index.html 的源码,
    // 见 `staticAnchors` 那一段的 `data-tour=storage` 那一格)。
    eq(TOUR.TOUR_STEPS[5].anchor, "an", "第 6 步 = 三件套「02 分析」段");
    eq(TOUR.TOUR_STEPS[31].action, "zoomLanes", "步 32 泳道区自动放大泳道");
    eq(
        TOUR.TOUR_STEPS[32].action,
        "showDemoSelection",
        "步 33 选区手柄自动创建示例选区",
    );
    eq(
        TOUR.TOUR_STEPS[34].action,
        "showDemoSegment",
        "步 35 段检查器自动选中示例段",
    );
    eq(TOUR.TOUR_STEPS[36].action, "expandGuide", "步 37 自动展开九条");
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

    // T37 真机 bug A-2 回归:tour 走完 → endTour 发出 setTourSeen 后立刻 requestRender,
    // 这一帧 ui.tour_seen 还是 false(桥回执 + 下一拍 25Hz scvb.state 都没到)。
    // 「本会话已答」位必须当场拦住兜底直弹,否则询问卡重弹、用户再点「开始」就重放整个 tour。
    eq(
        TOUR.shouldAutoShowTourAsk(
            { ui: { guide_seen: true, tour_seen: false } }, // 回执未到,state 仍是旧值
            { tour_seen_global: false },
            true, // 本会话已答(开始 / 暂不 / tour 已结束)
        ),
        false,
        "本会话已答 ⇒ 回执未到也不重弹(tour 不重放)",
    );
    eq(
        TOUR.shouldAutoShowTourAsk(
            { ui: { guide_seen: true, tour_seen: false } },
            { tour_seen_global: false },
            false,
        ),
        true,
        "本会话未答 ⇒ 兜底直弹照旧",
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

    // 本地化 demo 轨名(合入后补漏:en/fr 用户可见轨名不再中文)
    const dsEn = TOUR.buildDemoStore(() => T.en);
    eq(
        dsEn.state.channels[0].label,
        "Lead Vocal 1",
        "demo 轨名 en 本地化(ch1)",
    );
    const dsFr = TOUR.buildDemoStore(() => T.fr);
    eq(
        dsFr.state.channels[0].label,
        "Voix principale 1",
        "demo 轨名 fr 本地化(ch1)",
    );
    eq(ds.state.channels[0].label, "主唱1", "不传 getT 保持 zh 原 label(ch1)");
    // 三语 + mock 同步:zh demo.chN == mock DEMO_LABELS;en/fr 均已本地化(无 CJK 字符)
    const labels = FIFTEEN_TRACKS.labels;
    check(Array.isArray(labels) && labels.length === 15, "demo labels 15 条");
    let synced = true;
    const cjk = /[\u4e00-\u9fff]/;
    for (let n = 1; n <= 15; n++) {
        const k = "demo.ch" + n;
        if (T.zh[k] !== labels[n - 1]) synced = false;
        if (cjk.test(T.en[k]) || cjk.test(T.fr[k])) synced = false;
    }
    check(synced, "zh demo.ch* == mock DEMO_LABELS 且 en/fr 无中文");

    // 三语长度齐备 + en/fr 两两互异(合入后补漏:撞名会让用户分不清谁是谁)
    const enNames = [];
    const frNames = [];
    let complete = true;
    for (let n = 1; n <= 15; n++) {
        const k = "demo.ch" + n;
        if (typeof T.zh[k] !== "string" || T.zh[k].length === 0)
            complete = false;
        if (typeof T.en[k] !== "string" || T.en[k].length === 0)
            complete = false;
        if (typeof T.fr[k] !== "string" || T.fr[k].length === 0)
            complete = false;
        enNames.push(T.en[k]);
        frNames.push(T.fr[k]);
    }
    check(complete, "三语 demo.ch1..15 词条长度齐备(均非空字符串)");
    check(new Set(enNames).size === 15, "en demo.ch1..15 两两互异(无撞名)");
    check(new Set(frNames).size === 15, "fr demo.ch1..15 两两互异(无撞名)");

    // 本地化 demo 版本名(合入后补漏):versions[0](V1「基础平衡」)三语;V2 保持原样
    eq(ds.state.versions[0].name, "基础平衡", "不传 getT 保持 zh 原版本名(V1)");
    eq(
        dsEn.state.versions[0].name,
        "Base balance",
        "demo 版本名 en 本地化(V1)",
    );
    eq(
        dsFr.state.versions[0].name,
        "Équilibre de base",
        "demo 版本名 fr 本地化(V1)",
    );
    check(
        !cjk.test(dsEn.state.versions[0].name) &&
            !cjk.test(dsFr.state.versions[0].name),
        "en/fr 版本名无中文",
    );
    eq(dsEn.state.versions[1].name, "V2", "V2 版本名保持原样(不本地化)");
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
    // [SL-415] 43 步,不是 44 —— 步数必须与 `TOUR.TOUR_STEPS.length` 对得上,否则
    // 「末尾那一步的词条缺了」这件事会被这个写死的上界盖住(44 时它只到 43,不报错)。
    // 上界直接取整表长度,别再写一个会漂的字面量。
    eq(TOUR.TOUR_STEPS.length, 43, "步数 == 43(下面那份词条清单按它铺开)");
    for (let i = 1; i <= TOUR.TOUR_STEPS.length; i++) {
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
        !/写入完成/.test(T.zh["tour.step8.body"]),
        "zh step8 不含「写入完成」",
    );
    check(
        !/write complete|written to/i.test(T.en["tour.step8.body"]),
        "en step8 不含 write complete / written",
    );
    // 组这一步只讲组的作用(用户实机发现:组选择器在三件套**下方**,原「下方/从左到右」方向表述有误,已删)
    check(
        !/从左到右|下方三个模块/.test(T.zh["tour.step8.body"]),
        "zh step8 无「下方/从左到右」方向表述",
    );
    check(
        /组把通道分成 A–H 八个独立工作区/.test(T.zh["tour.step8.body"]),
        "zh step8 只讲组的作用",
    );
    check(
        !/left to right/i.test(T.en["tour.step8.body"]),
        "en step8 无 left to right 方向表述",
    );
    check(
        !/gauche à droite/i.test(T.fr["tour.step8.body"]),
        "fr step8 无 gauche à droite 方向表述",
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

    // T37 bug A-2 接线:一次性门控不得只依赖 setTourSeen 的异步回执 ——
    // endTour 必须同步通知外壳落「本会话已答」位,外壳三处答复点都要置位并把它喂给判定。
    const app = src("web/output/app.js");
    check(ts.includes("onEnd()"), "endTour 同步通知外壳(onEnd)");
    check(
        (app.match(/session\.tourAnswered = true/g) || []).length >= 3,
        "外壳三处答复点(开始 / 暂不 / tour 结束)都落本会话已答位",
    );
    check(
        /shouldAutoShowTourAsk\(\s*store\.state,\s*store\.snapshot,\s*store\.session\.tourAnswered/.test(
            app,
        ),
        "syncTourAsk 把本会话已答位喂进判定",
    );

    // data-tour 锚点:index.html 静态锚 + tab-tracks.js 首行动态锚(dt() 助手)
    //
    // ⚠ 这份名单 = 「**步骤表要用的**锚点」,不是「DOM 里存在的锚点」。`storage` 已从
    // 两处一起下榜:步骤表里整步移除(SL-415),index.html 里那个属性按「隐藏 ≠ 删除」
    // **仍然留着** —— 所以它不在这份「步骤要用的锚点」名单里,而是单独一格断
    // 「属性还在」(见下面 storage 那一格)。混在一起的话,删掉属性会红在一条
    // 读起来像「步骤表缺锚点」的断言上,而真正的原因恰好相反。
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
        "diagnostic",
        "version",
    ];
    for (const a of staticAnchors) {
        check(
            html.includes('data-tour="' + a + '"'),
            "index.html data-tour=" + a,
        );
    }
    // [SL-415] `storage` 的**反向**那一格:步骤表里没有这一步了,但 DOM 里那个锚点
    // 必须还在(用户裁定是「收起 UI」,不是「删掉 DOM」;那张卡片自己恒挂 `hidden`)。
    //
    // ⚠ **必须切到那个标签里面再找**(与下面 (a21) 的 `listSrc` 同款理由,第一版就栽在这):
    // 拿 `html.includes('data-tour="storage"')` 全文件找的话,**本卡自己写在那张卡片上方
    // 的注释里就逐字引了一遍这个属性名** —— 判据被作者自己的话喂饱,把属性真删掉也照绿
    // (实测:删掉属性,那一格仍然 exit 0)。改成先在标签上取到那段文本再断。
    // 删除式:把 `data-tour="storage"` 属性从 index.html 删掉 ⇒ 本格红
    // (而上面那份「步骤要用的锚点」名单里已经没有它,所以那条路不会替它红)。
    const storageCardTag = /<div\b[^>]*data-gb="settings-storage"[^>]*>/.exec(
        html,
    );
    check(
        !!storageCardTag &&
            /(^|\s)data-tour="storage"(\s|$)/.test(storageCardTag[0]),
        "index.html 仍留 data-tour=storage 锚点(对照:DOM 留,只是不再有步骤指它)",
    );
    // 同一条卡片的第二半:它自己恒挂 `hidden`。这里只断**属性在**(布局面归页面级
    // `smoke-ui-layout-page.mjs` G 节)—— 少了这一格,「隐藏 ≠ 删除」这句就只剩删除那一半
    // 有判据。
    check(
        !!storageCardTag && /(^|\s)hidden(\s|$)/.test(storageCardTag[0]),
        "对照:「存储状态」卡模板里恒挂 hidden",
    );
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
    const ls = src("web/shared/lang-start.js");
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
