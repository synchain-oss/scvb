// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Output tab 条的 WAI-ARIA tabs 模式冒烟(node,无 DOM)
// =============================================================================
// 【为什么单开一套】role=tab / role=tablist 在场,axe 的自动规则就基本闭嘴了 ——
// 它查不出「tab 没有 aria-controls」「面板没有 role=tabpanel」「没有 roving tabindex」
// 这几件缺失。而缺了它们,屏幕阅读器关联不上 tab 与面板(读到 tab 不知道它控制谁、
// 进了面板不知道自己在哪一页),键盘用户也没有标准的 ←/→ 组内切换。这类「有角色、
// 没接线」的半成品正是自动化 a11y 工具的盲区,所以在这里手写成断言。
//
// 跑什么:
//   ① 静态配对(web/output/index.html):四个 tab ↔ 四个面板的
//      aria-controls / id / aria-labelledby 构成**双射**,且引用的 id 真实存在;
//      每个 [data-tab-panel] 都是 role=tabpanel(不许有面板漏挂角色);
//   ② 静态 roving tabindex:恰好一个 tab 是 tabindex=0,且它就是 aria-selected=true
//      的那个(首帧落点必须与 app.js 的 activateTab("master") 一致);
//   ③ **真执行**键盘处理器:把 app.js 里那段 keydown 监听按括号配平原样切出来,
//      喂给 new Function + 桩 tabbar 跑真代码,断言 ←/→ 循环、Home/End、
//      焦点跟随、以及三条不接管(tour 已消费 / 修饰键 / 焦点不在 tab 上);
//   ④ 源码级:roving tabindex 由 activateTab 统一维护,键盘走的是与点击同一条
//      activateTab 路径(不许出现第二套切页分支)。
//
// 用法:node web-preview/tests/smoke-a11y-tabs.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有断言失败(逐条打印 [FAIL])。
// =============================================================================

import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";
import { readFileSync } from "node:fs";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const src = (p) => readFileSync(join(ROOT, p), "utf8");

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

const html = src("web/output/index.html");
const app = src("web/output/app.js");

// -----------------------------------------------------------------------------
// 极简开标签扫描:够用就好,不做 HTML 解析器。
// 先掏空注释与 <style>/<script>(index.html 的注释里成段讨论 role=tab 本身,
// 不掏空必然误判),再逐个开标签取属性。属性值里的 > 由引号态挡住。
function stripNonMarkup(s) {
    return s
        .replace(/<!--[\s\S]*?-->/g, "")
        .replace(/<style\b[^>]*>[\s\S]*?<\/style>/g, "")
        .replace(/<script\b[^>]*>[\s\S]*?<\/script>/g, "");
}

function parseOpenTags(s) {
    const out = [];
    const tagRe = /<([a-zA-Z][\w-]*)((?:"[^"]*"|'[^']*'|[^>"'])*)>/g;
    let m;
    while ((m = tagRe.exec(s))) {
        const attrs = {};
        const attrRe =
            /([a-zA-Z_:][-\w:.]*)(?:\s*=\s*("[^"]*"|'[^']*'|[^\s>]+))?/g;
        let a;
        while ((a = attrRe.exec(m[2]))) {
            let v = a[2] === undefined ? "" : a[2];
            if (v && (v[0] === '"' || v[0] === "'")) v = v.slice(1, -1);
            attrs[a[1]] = v;
        }
        out.push({ tag: m[1].toLowerCase(), attrs });
    }
    return out;
}

const tags = parseOpenTags(stripNonMarkup(html));

// TAB_ORDER 的真源在 app.js —— 这里读回来,免得两处各写一份四值枚举
// (契约 §1.31 setActiveTab 是四值冻结枚举,顺序即 tab 条从左到右的视觉顺序)。
const orderM = app.match(/const TAB_ORDER\s*=\s*\[([^\]]*)\]/);
if (!orderM) {
    console.error("  [FAIL] app.js 里读不到 const TAB_ORDER");
    process.exit(1);
}
const TAB_ORDER = orderM[1]
    .split(",")
    .map((x) => x.trim().replace(/^["']|["']$/g, ""))
    .filter(Boolean);
eq(TAB_ORDER, ["master", "tracks", "wave", "settings"], "TAB_ORDER 四值枚举");

// =============================================================================
log("=== ① tab ↔ 面板的 aria 配对(双射 + 引用可解析)===");
{
    const ids = new Map(); // id -> tag 对象
    let dupIds = 0;
    for (const t of tags) {
        const id = t.attrs.id;
        if (!id) continue;
        if (ids.has(id)) dupIds++;
        else ids.set(id, t);
    }
    check(dupIds === 0, "index.html 内 id 唯一(重复 " + dupIds + " 个)");

    const tablists = tags.filter((t) => t.attrs.role === "tablist");
    const tabs = tags.filter((t) => t.attrs.role === "tab");
    const panels = tags.filter((t) => t.attrs.role === "tabpanel");
    eq(tablists.length, 1, "恰好一个 role=tablist");
    eq(tabs.length, 4, "恰好四个 role=tab");
    eq(panels.length, 4, "恰好四个 role=tabpanel");
    eq(
        tabs.map((t) => t.attrs["data-tab-btn"]),
        TAB_ORDER,
        "tab 条的 DOM 顺序 = TAB_ORDER(阅读顺序与 ←/→ 顺序一致)",
    );

    // 每个 [data-tab-panel] 都必须挂上 role=tabpanel —— 防「新加一页忘了挂角色」
    const panelSections = tags.filter((t) => t.attrs["data-tab-panel"]);
    eq(
        panelSections.map((t) => t.attrs["data-tab-panel"]),
        TAB_ORDER,
        "四个 [data-tab-panel] 与 TAB_ORDER 一一对应",
    );
    for (const p of panelSections) {
        check(
            p.attrs.role === "tabpanel",
            "面板 " + p.attrs["data-tab-panel"] + " 挂 role=tabpanel",
        );
    }

    // 双射:tab.aria-controls → 面板 id,面板.aria-labelledby → tab id,两边互指
    for (const name of TAB_ORDER) {
        const tab = tabs.find((t) => t.attrs["data-tab-btn"] === name);
        if (!check(!!tab, "找得到 tab " + name)) continue;

        const tabId = tab.attrs.id;
        const controls = tab.attrs["aria-controls"];
        check(!!tabId, "tab " + name + " 有 id(供面板 aria-labelledby 回指)");
        check(!!controls, "tab " + name + " 有 aria-controls");
        if (!controls) continue;

        const panel = ids.get(controls);
        check(
            !!panel,
            "tab " +
                name +
                " 的 aria-controls=" +
                controls +
                " 指向真实存在的 id",
        );
        if (!panel) continue;
        check(
            panel.attrs.role === "tabpanel",
            "tab " + name + " 的 aria-controls 指向的是一个 role=tabpanel",
        );
        eq(
            panel.attrs["data-tab-panel"],
            name,
            "tab " + name + " 的 aria-controls 指向同名面板",
        );
        eq(
            panel.attrs["aria-labelledby"],
            tabId,
            "面板 " + name + " 的 aria-labelledby 回指 tab " + name,
        );
        check(
            ids.has(panel.attrs["aria-labelledby"] || ""),
            "面板 " + name + " 的 aria-labelledby 指向真实存在的 id",
        );
    }
}

// =============================================================================
log("=== ② 静态 roving tabindex(首帧落点)===");
{
    const tabs = tags.filter((t) => t.attrs.role === "tab");
    const zero = tabs.filter((t) => t.attrs.tabindex === "0");
    const minus = tabs.filter((t) => t.attrs.tabindex === "-1");
    eq(
        zero.length,
        1,
        "恰好一个 tab 是 tabindex=0(tablist 整体只占一个 Tab 停靠位)",
    );
    eq(minus.length, 3, "其余三个 tab 是 tabindex=-1");
    if (zero.length === 1) {
        eq(
            zero[0].attrs["aria-selected"],
            "true",
            "tabindex=0 的那个就是 aria-selected=true 的那个",
        );
        eq(
            zero[0].attrs["data-tab-btn"],
            "master",
            ' 首帧落点 = master(与 app.js 的 activateTab("master") 一致)',
        );
    }
    eq(
        tabs.filter((t) => t.attrs["aria-selected"] === "true").length,
        1,
        "恰好一个 aria-selected=true",
    );
}

// =============================================================================
log("=== ③ 真执行:键盘切换处理器(←/→ 循环 + Home/End + 焦点跟随)===");
{
    // 把 app.js 里那段监听原样切出来跑。为什么不 import app.js:它模块顶层就摸
    // document(getElementById),node 侧起不来 —— 全仓的 smoke 都是这个前提。
    // 切法 = 从 `tabbar.addEventListener(` 起做括号配平,跳过字符串与行注释
    // (注释里的中文全角括号不参与配平,但 ASCII 括号会,所以必须跳)。
    function sliceCall(s, head) {
        const start = s.indexOf(head);
        if (start < 0) return null;
        let i = s.indexOf("(", start); // 实参表的开括号
        if (i < 0) return null;
        let depth = 0;
        while (i < s.length) {
            const c = s[i];
            if (c === "/" && s[i + 1] === "/") {
                while (i < s.length && s[i] !== "\n") i++;
                continue;
            }
            if (c === '"' || c === "'" || c === "`") {
                const q = c;
                i++;
                while (i < s.length && s[i] !== q) {
                    if (s[i] === "\\") i++;
                    i++;
                }
                i++;
                continue;
            }
            if (c === "(") depth++;
            else if (c === ")") {
                depth--;
                if (depth === 0) return s.slice(start, i + 1) + ";";
            }
            i++;
        }
        return null;
    }

    const keysDecl = (app.match(/const TAB_KEYS\s*=\s*\[[^\]]*\];/) || [])[0];
    const listener = sliceCall(app, 'tabbar.addEventListener("keydown",');
    check(!!keysDecl, "app.js 有 TAB_KEYS 白名单");
    check(!!listener, "app.js 有 tabbar 上的 keydown 监听(切得出来)");

    if (keysDecl && listener) {
        const activated = [];
        const focused = [];
        const btns = {};
        for (const name of TAB_ORDER) {
            btns[name] = {
                getAttribute: (k) => (k === "data-tab-btn" ? name : null),
                focus: () => focused.push(name),
            };
        }
        let handler = null;
        const tabbar = {
            addEventListener: (type, cb) => {
                if (type === "keydown") handler = cb;
            },
            querySelector: (sel) => {
                const m = /\[data-tab-btn="([^"]+)"\]/.exec(sel);
                return m ? btns[m[1]] || null : null;
            },
        };
        const fn = new Function(
            "tabbar",
            "TAB_ORDER",
            "activateTab",
            keysDecl + "\n" + listener,
        );
        fn(tabbar, TAB_ORDER.slice(), (name) => activated.push(name));
        check(!!handler, "监听确实挂在 keydown 上");

        function press(key, from, extra) {
            activated.length = 0;
            focused.length = 0;
            const e = {
                key,
                target:
                    from === null ? { getAttribute: () => null } : btns[from],
                altKey: false,
                ctrlKey: false,
                metaKey: false,
                defaultPrevented: false,
                prevented: false,
                preventDefault() {
                    this.prevented = true;
                },
            };
            if (extra) for (const k of Object.keys(extra)) e[k] = extra[k];
            handler(e);
            return {
                to: activated.length ? activated[activated.length - 1] : null,
                focus: focused.length ? focused[focused.length - 1] : null,
                prevented: e.prevented,
            };
        }

        if (handler) {
            const cases = [
                ["ArrowRight", "master", "tracks", "→ 前进一格"],
                ["ArrowRight", "wave", "settings", "→ 前进到末格"],
                ["ArrowRight", "settings", "master", "→ 在末格循环回首格"],
                ["ArrowLeft", "tracks", "master", "← 后退一格"],
                ["ArrowLeft", "master", "settings", "← 在首格循环回末格"],
                ["Home", "wave", "master", "Home 跳首格"],
                ["Home", "master", "master", "Home 停在首格(幂等)"],
                ["End", "master", "settings", "End 跳末格"],
                ["End", "settings", "settings", "End 停在末格(幂等)"],
            ];
            for (const [key, from, want, why] of cases) {
                const r = press(key, from);
                eq(r.to, want, "键盘切换 " + why + "(自 " + from + ")");
                eq(
                    r.focus,
                    want,
                    "焦点跟随到新激活项 " + why + "(自 " + from + ")",
                );
                check(
                    r.prevented,
                    "接管后 preventDefault(" + key + " 自 " + from + ")",
                );
            }

            // 三条「不接管」—— 每条都对应一个真实的回归风险
            const tourEaten = press("ArrowRight", "master", {
                defaultPrevented: true,
            });
            eq(
                tourEaten.to,
                null,
                "tour 已消费该键(defaultPrevented)⇒ 不再切 tab",
            );
            for (const mod of ["altKey", "ctrlKey", "metaKey"]) {
                const r = press("ArrowRight", "master", { [mod]: true });
                eq(r.to, null, "带 " + mod + " 的 → 不接管(留给宿主/浏览器)");
            }
            eq(
                press("ArrowRight", null).to,
                null,
                "焦点不在四个 tab 上(如条内滑块)⇒ 不接管",
            );
            for (const key of [
                "ArrowUp",
                "ArrowDown",
                "a",
                "Tab",
                "Enter",
                " ",
            ]) {
                eq(
                    press(key, "master").to,
                    null,
                    "非 ←/→/Home/End 的键不接管:" + key,
                );
            }
        }
    }
}

// =============================================================================
log("=== ④ 源码级:roving tabindex 由 activateTab 统一维护 ===");
{
    const body = (app.match(/function activateTab\([\s\S]*?\n}\n/) || [])[0];
    check(!!body, "找得到 activateTab 函数体");
    if (body) {
        check(
            /setAttribute\(\s*"tabindex"/.test(body),
            "activateTab 写 tabindex(roving 与 aria-selected 同一处派生)",
        );
        check(
            /"0"\s*:\s*"-1"/.test(body),
            "activateTab 的 tabindex 取值 = 激活 0 / 其余 -1",
        );
        check(
            /setAttribute\(\s*"aria-selected"/.test(body),
            "activateTab 仍写 aria-selected",
        );
    }
    // 键盘路径不许自造第二套切页分支:它必须落在 activateTab 上
    const listener = app.slice(
        app.indexOf('tabbar.addEventListener("keydown"'),
    );
    check(
        /activateTab\(TAB_ORDER\[/.test(listener),
        "键盘切换走 activateTab(与点击同一条路径,行为必然一致)",
    );
    check(
        !/call\(\s*"setActiveTab"/.test(
            listener.slice(0, listener.indexOf("\n});") + 4),
        ),
        "键盘处理器不自行调 setActiveTab(避免与 activateTab 各写一次上行)",
    );
}

// =============================================================================
log(fail === 0 ? "\n全部通过 ✅" : "\n失败 " + fail + " 条 ❌");
process.exit(fail === 0 ? 0 : 1);
