// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Input 首启轻量引导([J80] / T48)冒烟(node,无 DOM)
// =============================================================================
// 口径同 smoke-tour:断言面 = 纯函数 + mock 端到端 + 源码级字面断言。
// DOM 侧(蒙版/亮区/说明框定位、点击推进)归浏览器手测 / 截图。
//
// 跑什么:
//   ① 步骤清单:5 步、步号连续、首步无 spotlight、末步 = header「?」自指(J62 同款);
//   ② shouldShowInputGuide 四种组合(J50a 镜像)+ 会话闸门;
//   ③ mock 端到端:input-first-run 两级 guide_seen 全 false → 首启链可达;
//      setGuideSeen(true, true) 落工程位与全局位,再取快照往返不丢 → seen 后不再弹;
//      「?」重看不依赖 seen(guide_seen 已置位也能再开);
//   ④ 词条:tour-in.* 三语齐、占位符一致;第 ④ 步与九条红字第 3 条口径同源、禁旧表述;
//   ⑤ 源码级:零 Audio API、唯一桥调用 = setGuideSeen、role=dialog、aria-live、
//      Esc=Skip、←/→、左键推进 + 说明框按钮例外;四锚点与「?」落点齐;
//      画法与语言卡确实**复用共享件**(web/shared/tour-paint.js / lang-start.js)。
//
// 用法:node web-preview/tests/smoke-input-tour.mjs [仓库根绝对路径]
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

const TOURIN = await import(u("web/input/tour-in.js"));
const LANGSTART = await import(u("web/shared/lang-start.js"));
const { createBridge, PENDING_FUNCS } = await import(u("web/shared/bridge.js"));
const { T } = await import(u("web/shared/i18n.js"));
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
const eq = (a, b, msg) =>
    check(
        JSON.stringify(a) === JSON.stringify(b),
        msg + " —— 期望 " + JSON.stringify(b) + ",实得 " + JSON.stringify(a),
    );

const LANGS = ["zh", "en", "fr"];

/** 起一个 Input 会话(与 smoke-input 同款),回调里拿 bridge / session。 */
async function withInput(params, fn) {
    const s = driver.createPreviewSession({ role: "input", params });
    const b = createBridge({ role: "input", mockBackend: s.mock });
    s.start();
    const snap = await b.requestInitialState();
    const r = await fn(b, snap, s);
    s.stop();
    return r;
}

// =============================================================================
log("=== ① 步骤清单(mini tour 5 步)===");
{
    eq(TOURIN.TOUR_IN_STEPS.length, 5, "步数 == 5([J80] 5 步基线)");
    eq(
        TOURIN.TOUR_IN_ANCHORS,
        [null, "group", "channel", "pill", "help"],
        "锚点逐条 = 居中 / group / channel / pill / help",
    );
    check(
        TOURIN.TOUR_IN_STEPS[0].anchor === null,
        "首步 = 欢迎居中卡(无 spotlight)",
    );
    eq(
        TOURIN.TOUR_IN_ANCHORS[TOURIN.TOUR_IN_ANCHORS.length - 1],
        "help",
        "末步固定 = header「?」重看入口自指(J62 同款)",
    );
    check(Object.isFrozen(TOURIN.TOUR_IN_STEPS), "步骤表深冻结(渲染侧只读)");
}

// =============================================================================
log("=== ② shouldShowInputGuide(J50a 镜像 + 会话闸门)===");
{
    const snapOff = { guide_seen_global: false };
    const snapOn = { guide_seen_global: true };
    check(
        TOURIN.shouldShowInputGuide(
            { ui: { guide_seen: false } },
            snapOff,
            false,
        ) === true,
        "工程位 false + 全局位 false + 本会话未走 ⇒ 弹",
    );
    check(
        TOURIN.shouldShowInputGuide(
            { ui: { guide_seen: true } },
            snapOff,
            false,
        ) === false,
        "工程位已置 ⇒ 不弹",
    );
    check(
        TOURIN.shouldShowInputGuide(
            { ui: { guide_seen: false } },
            snapOn,
            false,
        ) === false,
        "全局默认位已置(跨工程承诺)⇒ 不弹",
    );
    check(
        TOURIN.shouldShowInputGuide(
            { ui: { guide_seen: false } },
            snapOff,
            true,
        ) === false,
        "本会话已走过/跳过 ⇒ 不弹",
    );
    check(
        TOURIN.shouldShowInputGuide(
            { ui: { guide_seen: false } },
            null,
            false,
        ) === false,
        "首帧快照未到(契约 §0.6 门控)⇒ 不弹",
    );
    // 语言卡与 mini tour 判据同构:同一组输入两者结论一致(差异只在 langChosen 一项)
    check(
        LANGSTART.shouldShowLangStart(
            { ui: { guide_seen: false } },
            snapOff,
            false,
            false,
        ) === true,
        "语言卡与 mini tour 判据同构(共用 ui.guide_seen + guide_seen_global)",
    );
}

// =============================================================================
log("=== ③ mock 端到端:input-first-run + setGuideSeen 往返 ===");
{
    await withInput("scenario=input-first-run", async (b, snap) => {
        check(
            snap.ui && snap.ui.guide_seen === false,
            "input-first-run:工程位 ui.guide_seen=false,实得 " +
                JSON.stringify(snap.ui),
        );
        check(
            snap.guide_seen_global === false,
            "input-first-run:全局默认位 guide_seen_global=false",
        );
        check(
            TOURIN.shouldShowInputGuide(snap, snap, false) === true,
            "input-first-run 首启链开箱可达",
        );

        // 完成 / Skip 的唯一写入口(J50a:alsoGlobal=true 同步写全局默认位)
        check(
            typeof b.setGuideSeen === "function",
            "setGuideSeen 已挂到 Input 桥(PENDING_FUNCS 能力探测)",
        );
        const res = await b.setGuideSeen(true, true);
        eq(res, { ok: true }, "setGuideSeen 返回 {ok:true}");

        const again = await b.requestInitialState();
        check(again.ui.guide_seen === true, "工程位 ui.guide_seen 往返不丢");
        check(
            again.guide_seen_global === true,
            "全局默认位 guide_seen_global 往返不丢(跨工程承诺)",
        );
        check(
            TOURIN.shouldShowInputGuide(again, again, false) === false,
            "seen 已置位 ⇒ 首启链不再自动弹",
        );
        // 「?」重看入口不看 seen:createInputTour().start() 无任何 seen 判据(源码级见 ⑤)
    });

    // 其余场景不被首启链挡住(七态验收面照旧)
    for (const sc of ["connected", "no-output", "passthrough"]) {
        await withInput("scenario=" + sc, async (b, snap) => {
            check(
                snap.ui.guide_seen === true && snap.guide_seen_global === true,
                "场景 " + sc + " 按「已看过」渲染,不被语言卡挡住",
            );
        });
    }

    eq(
        PENDING_FUNCS.input,
        ["setGuideSeen"],
        "Input 侧待转正名表 = [setGuideSeen](契约 §3 仍是 7 个函数)",
    );
}

// =============================================================================
log("=== ④ 词条:tour-in.* 三语 + 第 ④ 步口径 ===");
{
    const keys = ["tour-in.help"];
    for (let i = 1; i <= 5; i++) {
        keys.push("tour-in.step" + i + ".title");
        keys.push("tour-in.step" + i + ".body");
    }
    for (const k of keys) {
        for (const l of LANGS) {
            const v = T[l][k];
            check(
                typeof v === "string" && v.trim() !== "",
                l + "." + k + " 三语齐备且非空",
            );
        }
        check(
            T.en[k] !== T.zh[k] && T.fr[k] !== T.en[k],
            k + " 三语互不逐字相同(05 §5 占位符判据)",
        );
    }

    // 第 ④ 步 = 九条红字第 3 条的场景化改写:关键用词与 guide.rule3 同源。
    const zh4 = T.zh["tour-in.step4.body"];
    for (const phrase of [
        "静音",
        "接管",
        "设计行为",
        "不是 bug",
        "检测不到",
        "直通",
        "没有声音",
    ]) {
        check(zh4.includes(phrase), "zh 第 ④ 步含「" + phrase + "」");
    }
    // 05 §2.4 明令禁止的旧表述(ADR-002 v1 已被 J12 实质修订)
    for (const banned of ["哑", "永久静音", "一直静音"]) {
        check(!zh4.includes(banned), "zh 第 ④ 步禁用「" + banned + "」");
    }
    for (const l of LANGS) {
        check(
            !/(TODO|TBD|FIXME|xxx)/i.test(T[l]["tour-in.step4.body"]),
            l + " 第 ④ 步无占位符标记",
        );
    }
    check(
        /by design, not a bug/i.test(T.en["tour-in.step4.body"]) &&
            /no sound/i.test(T.en["tour-in.step4.body"]),
        "en 第 ④ 步与 en guide.rule3 同源用词(by design, not a bug / no sound)",
    );
    check(
        /pas un bug/i.test(T.fr["tour-in.step4.body"]) &&
            /sans aucun son/i.test(T.fr["tour-in.step4.body"]),
        "fr 第 ④ 步与 fr guide.rule3 同源用词(pas un bug / sans aucun son)",
    );

    // 占位符三语一致(本组全部无占位符)
    const PH = /\{[A-Za-z_][A-Za-z0-9_]*\}/g;
    for (const k of keys) {
        const sets = LANGS.map((l) =>
            [...new Set(T[l][k].match(PH) || [])].sort().join(","),
        );
        check(
            sets[0] === sets[1] && sets[1] === sets[2],
            k + " 三语占位符一致",
        );
    }
}

// =============================================================================
log("=== ⑤ 源码级:零 Audio / 唯一桥调用 / a11y / 锚点 / 复用共享件 ===");
{
    const ts = src("web/input/tour-in.js");
    const html = src("web/input/index.html");
    const app = src("web/input/app.js");
    const paint = src("web/shared/tour-paint.js");

    // 无声音,仅视觉 + 文字(05 §2.6)
    for (const a of ["AudioContext", "new Audio", "playbackRate", ".play("]) {
        check(!ts.includes(a), "tour-in.js 零 Audio API:" + a);
    }

    // 唯一桥调用 = setGuideSeen(不写 state、不触引擎)
    const calls = [...ts.matchAll(/call\("([a-zA-Z]+)"/g)].map((m) => m[1]);
    eq(calls, ["setGuideSeen"], "唯一桥调用 = setGuideSeen");
    check(
        ts.includes('call("setGuideSeen", true, true)'),
        "完成与 Skip 都经 setGuideSeen(true, true) 置位(J50a 镜像)",
    );
    // 重看**不**写已读位:置位受 persistOnEnd 闸,该闸由 start({replay}) 翻
    // (契约变更说明 20260825-input-guide-seen §四「header「?」重看引导不调用本函数」)
    check(
        /if \(persistOnEnd\) call\("setGuideSeen", true, true\);/.test(ts),
        "setGuideSeen 置位受 persistOnEnd 闸门管辖",
    );
    check(
        /persistOnEnd = !\(o && o\.replay\);/.test(ts),
        "persistOnEnd 由 start({replay}) 决定 —— 重看路径不置位",
    );
    // 完成与 Skip 共用同一个出口 endTour —— 置位不会只覆盖其中一条路径
    check(
        /function skip\(\)\s*\{\s*endTour\(false\);/.test(ts),
        "Skip 与完成同一出口(endTour)",
    );

    // a11y:说明框 role=dialog + 步骤文案 aria-live 播报
    check(ts.includes('"dialog"'), "role=dialog");
    check(ts.includes('"polite"'), "aria-live=polite 播报");

    // 键盘与指针(05 §2.6 交互规则)
    check(ts.includes('"Escape"'), "Esc=Skip");
    check(ts.includes('"ArrowRight"'), "→ = 下一步");
    check(ts.includes('"ArrowLeft"'), "← = 上一步");
    check(ts.includes("e.button !== 0"), "仅左键推进(button===0)");
    check(
        ts.includes('closest("[data-tour-btn]")'),
        "说明框按钮例外(各自动作,不推进)",
    );

    // 锚点落点:四个 data-tour + header「?」重看入口
    for (const a of ["group", "channel", "pill", "help"]) {
        check(
            html.includes('data-tour="' + a + '"'),
            "index.html data-tour=" + a,
        );
    }
    check(
        html.includes('data-gb="input.header.help"'),
        "header「?」重看入口落点",
    );
    check(
        html.includes('data-t-aria="tour-in.help"'),
        "「?」按钮有可读名(data-t-aria=tour-in.help)",
    );

    // 重看:「?」接线,且 start() 无 seen 判据(guide_seen 已置位也可再开)
    check(
        /input\.header\.help[\s\S]{0,300}tour\.start\(\{\s*replay:\s*true\s*\}\)/.test(
            app,
        ),
        "app.js 把「?」接到 tour.start({replay:true})",
    );
    check(
        !/function start\([\s\S]*?guide_seen/.test(ts),
        "start() 不看 guide_seen —— 重看随时可再入",
    );

    // 复用共享件:画法 = tour-paint.js;语言卡 = shared/lang-start.js(与 Output 同一件)
    check(
        ts.includes('from "../shared/tour-paint.js"'),
        "tour-in.js 复用共享画法 web/shared/tour-paint.js",
    );
    check(
        app.includes('from "../shared/lang-start.js"'),
        "app.js 复用共享语言卡 web/shared/lang-start.js",
    );
    check(
        src("web/output/tour.js").includes('from "../shared/tour-paint.js"'),
        "Output 侧同样走共享画法(两侧一份实现,不会各自漂移)",
    );
    check(
        !/document\.|window\./.test(paint.split("export function maskFill")[0]),
        "tour-paint.js 顶层零 document/window 触碰(node 可 import)",
    );
}

// =============================================================================
if (fail > 0) {
    console.error("\nsmoke-input-tour 失败 " + fail + " 项");
    process.exit(1);
}
log("\n全部通过 ✅");
