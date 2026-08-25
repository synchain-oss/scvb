// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Header 常驻「撤销 / 重做」冒烟(node,无 DOM;小卡 D1)
// =============================================================================
// 与既有冒烟同款口径:仓内零 node_modules,断言面 = **纯 reducer + mock 端到端 +
// 源码不变式 + 词条**;像素与指针手势归浏览器手测(screenshots-d1/)。
//
// 跑什么:
//   ① 可用性 reducer(web/output/tab-master.js 的 historyAfterCall /
//      historyAfterSegments):契约 §1.25/§1.26 只给 `{ok:bool}`、没有 canUndo/canRedo,
//      故置灰判据全在这两个纯函数里 —— 它们是本卡唯一的「禁用态逻辑」真身;
//   ② mock 端到端:桥上确实挂得到 §1.25/§1.26 两个名字,回执形制 = `{ok:boolean}`,
//      喂回 reducer 后当场置灰(preview 里点一下就是这条路径);
//   ③ 源码不变式:两钮在 header 内、落在 spacer 之前(不挤 Version 区)、点击接线
//      走同一个 runHistory()、键盘钩子仍在且仍 preventDefault、置灰 data-disabled +
//      aria-disabled 双写并叠只读观察态、Input 页零改动;
//   ④ 词条:`header.*` 五条三语齐备、非空、05 §5 禁词零命中。
//
// 用法:node web-preview/tests/smoke-undo-redo.mjs [仓库根绝对路径]
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

const TM = await import(u("web/output/tab-master.js"));
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
        `${msg}: 实得 ${JSON.stringify(a)},期望 ${JSON.stringify(b)}`,
    );

// =============================================================================
log("=== ① 可用性 reducer(契约 §1.25/§1.26 回执驱动)===");

{
    const {
        HISTORY_AVAIL_INIT: INIT,
        historyAfterCall,
        historyAfterSegments,
    } = TM;

    // 起手两向常亮:UndoManager 挂在处理器上(03 §5.3),重开编辑器时栈可能非空,
    // 首帧没有任何证据说它空 —— 置灰会挡住真实可用的动作。
    eq({ ...INIT }, { undo: true, redo: true }, "起手值 = 两向常亮");
    check(Object.isFrozen(INIT), "起手值冻结(共享常量不得被就地改写)");

    // ---- 回执:{ok:false} = 该向栈空 ⇒ 只灰该向 ----
    eq(
        historyAfterCall(INIT, "undo", false),
        { undo: false, redo: true },
        "undo 回 ok:false ⇒ 灰 undo,redo 不动",
    );
    eq(
        historyAfterCall(INIT, "redo", false),
        { undo: true, redo: false },
        "redo 回 ok:false ⇒ 灰 redo,undo 不动",
    );

    // ---- 回执:{ok:true} = 一条事务挪到反向栈 ⇒ 反向必有货 ----
    eq(
        historyAfterCall({ undo: true, redo: false }, "undo", true),
        { undo: true, redo: true },
        "undo 成功 ⇒ redo 置亮",
    );
    eq(
        historyAfterCall({ undo: false, redo: true }, "redo", true),
        { undo: true, redo: true },
        "redo 成功 ⇒ undo 置亮",
    );

    // 灰掉之后还能自愈:栈空 → 反向 redo 成功 → undo 回亮(不会永久灰死)
    const afterEmpty = historyAfterCall(INIT, "undo", false);
    eq(
        historyAfterCall(afterEmpty, "redo", true),
        { undo: true, redo: true },
        "灰掉的 undo 由一次成功的 redo 回亮(禁用态可自愈)",
    );

    // ---- 入参守卫 ----
    eq(
        historyAfterCall(null, "undo", false),
        { undo: false, redo: true },
        "prev 为空时回落起手值再算",
    );
    eq(
        { ...historyAfterCall(INIT, "analyze", false) },
        { undo: true, redo: true },
        "非 undo/redo 的调用名原样返回(不误伤)",
    );

    // ---- 段表事件:新事务入栈 ⇒ undo 亮、redo 灭(juce::UndoManager 语义)----
    for (const reason of ["edit", "trackManual", "copyVersion"]) {
        eq(
            historyAfterSegments({ undo: false, redo: true }, { reason }),
            { undo: true, redo: false },
            `§0.9 左列 reason:"${reason}" ⇒ undo 置亮、redo 清空`,
        );
    }
    // undo/redo 自己的回推必须排除:否则一次 undo 会把刚长出来的 redo 当场灭掉
    for (const reason of ["undo", "redo"]) {
        eq(
            historyAfterSegments({ undo: true, redo: true }, { reason }),
            { undo: true, redo: true },
            `reason:"${reason}"(本操作自己的回推)不动两向`,
        );
    }
    // §0.9 右列 + 首帧:不入栈,不动
    for (const reason of [
        "analyze",
        "vad",
        "segmentation",
        "versionActive",
        "snapshot",
    ]) {
        eq(
            historyAfterSegments({ undo: false, redo: false }, { reason }),
            { undo: false, redo: false },
            `§0.9 右列 reason:"${reason}" 不入栈 ⇒ 两向不动`,
        );
    }
    eq(
        historyAfterSegments({ undo: false, redo: false }, null),
        { undo: false, redo: false },
        "空载荷不动两向",
    );

    // §2.8 reason 十值全覆盖:每个 reason 都得被上面两支之一分类掉(枚举闭合,
    // 契约 §2.8 明令新增函数须同批扩枚举 —— 漏一个这里就红)。
    const TEN = [
        "analyze",
        "vad",
        "segmentation",
        "edit",
        "trackManual",
        "undo",
        "redo",
        "versionActive",
        "copyVersion",
        "snapshot",
    ];
    const pushes = TEN.filter((reason) => {
        const next = historyAfterSegments(
            { undo: false, redo: true },
            {
                reason,
            },
        );
        return next.undo === true && next.redo === false;
    });
    eq(
        pushes,
        ["edit", "trackManual", "copyVersion"],
        "十值里判为「新事务入栈」的恰是 §0.9 左列在段表面的那三个",
    );
}

// =============================================================================
log("=== ② mock 端到端:桥回执 → 置灰 ===");

{
    const s = driver.createPreviewSession({
        role: "output",
        params: "?fixture=fifteen-tracks",
    });
    const { createBridge, BRIDGE_FUNCTIONS } = await import(
        u("web/shared/bridge.js")
    );
    check(
        BRIDGE_FUNCTIONS.output.includes("undo") &&
            BRIDGE_FUNCTIONS.output.includes("redo"),
        "§1.25/§1.26 是**已冻结**的桥名(本卡零契约变更、零新增桥面)",
    );

    const bridge = createBridge({ role: "output", mockBackend: s.mock });
    await bridge.requestInitialState();

    for (const kind of ["undo", "redo"]) {
        const res = await bridge[kind]();
        check(
            res && typeof res.ok === "boolean",
            `${kind}() 回执形制 = {ok:bool}(契约 §1.25/§1.26 返回行)`,
        );
        // mock 的撤销栈恒空(juce-bridge-mock.js「brief:栈空即可」)⇒ 预览页点一下
        // 就该看见该向灰掉 —— 截图里的禁用态正是这条路径。
        eq(res, { ok: false }, `mock ${kind}() = {ok:false}(栈空)`);
        const next = TM.historyAfterCall(TM.HISTORY_AVAIL_INIT, kind, res.ok);
        check(next[kind] === false, `${kind} 拿到 ok:false 后当场置灰`);
    }
}

// =============================================================================
log("=== ③ 源码不变式(DOM 侧退化都是一行改动,用文本不变式钉住)===");

{
    const html = src("web/output/index.html");
    const appJs = src("web/output/app.js");
    const inputHtml = src("web/input/index.html");

    // ---- 两钮存在 ----
    check(
        /data-gb="header-undo"/.test(html) &&
            /data-gb="header-redo"/.test(html),
        "index.html 有 header-undo / header-redo 两钮",
    );
    check(
        /data-gb="header-undo"[\s\S]{0,200}data-t="header\.undo"/.test(html),
        "撤销钮走词条 header.undo(不硬写自由文案)",
    );
    check(
        /data-gb="header-redo"[\s\S]{0,200}data-t="header\.redo"/.test(html),
        "重做钮走词条 header.redo",
    );
    check(
        /class="sc-btn hdr-history__btn"/.test(html),
        "样式走既有 sc-btn 族(不另造按钮外观)",
    );

    // ---- 落位:header 内,且在 spacer 之前(不挤右簇的 Version 区)----
    const headOpen = html.indexOf('<header id="header"');
    const headClose = html.indexOf("</header>");
    const undoAt = html.indexOf('data-gb="header-undo"');
    const spacerAt = html.indexOf('class="header__spacer"');
    const versionAt = html.indexOf('class="header__version"');
    check(
        headOpen > 0 && undoAt > headOpen && undoAt < headClose,
        "两钮在 <header> 之内(常驻,不随 tab 走)",
    );
    check(
        undoAt < spacerAt && spacerAt < versionAt,
        "两钮排在 header__spacer 之前 ⇒ 归左簇,不与 Version/复制/连接 pill/语言胶囊抢右簇宽度",
    );

    // ---- 禁用态:data-disabled + aria-disabled 双写 ----
    check(
        /data-gb="header-undo"[\s\S]{0,300}data-disabled="0"[\s\S]{0,80}aria-disabled="false"/.test(
            html,
        ),
        "两钮初始 data-disabled/aria-disabled 双写(渲染前的静态默认)",
    );
    check(
        /btn\.setAttribute\("data-disabled"[\s\S]{0,160}btn\.setAttribute\("aria-disabled"/.test(
            appJs,
        ),
        "renderHeader 逐帧双写 data-disabled + aria-disabled",
    );
    check(
        /const on = hist\[kind\] && !roNow;/.test(appJs),
        "可用 = 回执驱动的本地位 ∧ 非只读观察态(只读态全 UI 写控件 disabled)",
    );
    check(
        /getAttribute\("data-disabled"\) === "1"\) return;/.test(appJs),
        "置灰期的点击在 JS 侧拦截(sc-btn 的置灰只有 opacity/cursor,不吃事件)",
    );

    // ---- 接线:两条入口打同一个 runHistory,且它调的就是桥上那两个名字 ----
    check(
        /async function runHistory\(kind\) \{[\s\S]{0,600}await call\(kind\)/.test(
            appJs,
        ),
        "runHistory(kind) 直调桥函数 call(kind)(kind ∈ undo/redo)",
    );
    // 只读观察态:点击那条已被 data-disabled 拦住,键盘那条根本不看按钮属性 ——
    // 判据必须落在两个入口共用的 runHistory 里,否则 Ctrl+Z 能干成鼠标干不成的写操作。
    check(
        /async function runHistory\(kind\) \{[\s\S]{0,600}?if \(isReadOnly\(store\)\) return;[\s\S]{0,40}?const res = await call\(kind\);/.test(
            appJs,
        ),
        "只读观察态在 runHistory 开头直接返回 —— 键盘路径(Ctrl+Z)同样不发写调用",
    );
    check(
        /addEventListener\("click", \(\) => \{[\s\S]{0,300}runHistory\(kind\)/.test(
            appJs,
        ),
        "两钮 click → runHistory(显式入口接线)",
    );
    check(
        /historyAfterCall\(\s*store\.session\.history,\s*kind,\s*res\.ok,?\s*\)/.test(
            appJs,
        ),
        "回执喂进 reducer(禁用态的唯一数据源)",
    );
    check(
        /historyAfterSegments\(\s*store\.session\.history,\s*seg,?\s*\)/.test(
            appJs,
        ),
        "scvb.segments 喂进 reducer(新事务入栈的第二手证据)",
    );
    // ---- 顺序:入栈证据必须排在版本闸**之前** ----
    // 撤销栈挂在处理器上、整个工程一条(03 §5.3),不分版本;而 copyVersion 的段表
    // 事件带的是**目标**版本号,与 version_active 必然不等 —— 证据若排在闸后,
    // 「复制到非激活版本」这类真实可撤销的操作就永远看不见,undo 钮误灰。
    {
        const h = appJs.slice(appJs.indexOf('bridge.on("scvb.segments"'));
        const iHist = h.indexOf("historyAfterSegments(");
        const iGate = h.indexOf("segmentsEventApplies(");
        check(
            iHist >= 0 && iGate >= 0 && iHist < iGate,
            "scvb.segments:入栈证据(historyAfterSegments)排在版本闸(segmentsEventApplies)之前" +
                `,实得 hist@${iHist} / gate@${iGate}`,
        );
    }

    // ---- 键盘行为不动(本卡只加显式入口)----
    check(
        /e\.preventDefault\(\); \/\/ 防止冒泡到宿主撤销栈/.test(appJs),
        "Ctrl+Z 仍 preventDefault(契约 §1.25 语义行:不触碰宿主撤销栈)",
    );
    check(
        /runHistory\(e\.shiftKey \? "redo" : "undo"\)/.test(appJs),
        "Ctrl+Z / Ctrl+Shift+Z 仍映射 undo/redo,只是改走同一个 runHistory",
    );
    check(
        !/call\("undo"\)|call\("redo"\)/.test(appJs),
        "撤销/重做只剩 runHistory 一个出口(不留第二条不更新按钮态的旁路)",
    );

    // ---- Input 页不加 ----
    check(
        !/header-undo|header-redo/.test(inputHtml),
        "Input 页零改动(撤销栈是 Output 侧的,§1.25/§1.26 只在 Output 桥面)",
    );
}

// =============================================================================
log("=== ④ 词条(header.* 五条 × 三语)===");

{
    const KEYS = [
        "header.history",
        "header.undo",
        "header.redo",
        "header.undoEmpty",
        "header.redoEmpty",
    ];
    for (const k of KEYS) {
        for (const lang of ["zh", "en", "fr"]) {
            const v = T[lang][k];
            check(
                typeof v === "string" && v.trim().length > 0,
                `词条 ${lang}.${k} 存在且非空`,
            );
        }
        check(
            T.zh[k] !== T.en[k] || T.zh[k] !== T.fr[k],
            `词条 ${k} 三语不是同一串(说明确实译过)`,
        );
        check(!/\{/.test(String(T.zh[k])), `词条 ${k} 无占位符(纯静态标签)`);
    }
    const BANNED = ["写入完成", "推子后", "post-fader", "六条"];
    for (const k of KEYS) {
        for (const lang of ["zh", "en", "fr"]) {
            for (const banned of BANNED) {
                check(
                    !String(T[lang][k]).includes(banned),
                    `词条 ${lang}.${k} 命中 05 §5 禁词「${banned}」`,
                );
            }
        }
    }
}

// =============================================================================
log(fail === 0 ? "\n=== 结果:全部通过 ===" : `\n=== 结果:${fail} 项失败 ===`);
process.exit(fail === 0 ? 0 : 1);
