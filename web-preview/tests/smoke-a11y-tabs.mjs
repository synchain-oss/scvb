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
//      喂给 new Function + 桩 tabbar / 桩 Element / 桩 tour 跑真代码,断言 ←/→ 循环、
//      Home/End、焦点跟随、焦点落在 tab 子元素上也照常切,以及四类不接管
//      (上游已 preventDefault / tour 活着 / 带修饰键 / 焦点不在 tab 上);
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

/**
 * 从 `head` 起把一整个调用表达式原样切出来(括号配平)。
 * 用来把 app.js 里那段 keydown 监听整块取出:③ 拿去真跑,④ 拿去做源码级断言。
 * 为什么不 import app.js:它模块顶层就摸 document(getElementById),node 侧起不来 ——
 * 全仓的 smoke 都是这个前提。配平时要跳过字符串与行注释:注释里的中文全角括号不参与
 * 配平,但 ASCII 括号会。
 */
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
    if (tablists.length === 1) {
        // tablist 也要有可及名 —— 否则 SR 只报「标签页列表」,不知道是哪一组
        const t = tablists[0].attrs;
        check(
            !!(t["aria-label"] || t["aria-labelledby"]),
            "tablist 有可及名(aria-label / aria-labelledby)",
        );
    }
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
    const keysDecl = (app.match(/const TAB_KEYS\s*=\s*\[[^\]]*\];/) || [])[0];
    const listener = sliceCall(app, 'tabbar.addEventListener("keydown",');
    check(!!keysDecl, "app.js 有 TAB_KEYS 白名单");
    check(!!listener, "app.js 有 tabbar 上的 keydown 监听(切得出来)");
    // 白名单同样读回来用,不在这里再抄一份键名
    const TAB_KEYS = keysDecl
        ? JSON.parse((keysDecl.match(/\[[\s\S]*\]/) || ["[]"])[0])
        : [];
    eq(TAB_KEYS, ["ArrowLeft", "ArrowRight", "Home", "End"], "接管的四个键");

    if (keysDecl && listener) {
        const activated = [];
        const focused = [];
        // 桩 Element:处理器认 `e.target instanceof Element` + `closest()`,所以桩必须
        // 是同一个类的实例。`Element` 与 `tabbar` 一样当参数注进去 —— node 里没有这个
        // 全局,不注就是 ReferenceError。
        class Element {
            constructor(owner) {
                this.owner = owner; // 所属 tab 名;null = 条内的非 tab 元素
            }
            getAttribute(k) {
                return k === "data-tab-btn" ? this.owner : null;
            }
            closest(sel) {
                return /\[data-tab-btn\]/.test(sel) && this.owner ? this : null;
            }
            focus() {
                if (this.owner) focused.push(this.owner);
            }
        }
        const btns = {};
        for (const name of TAB_ORDER) btns[name] = new Element(name);
        // tab 按钮里的子元素(Tab3 的 <span>):closest 应当找回它所属的 tab
        const childOf = (name) => {
            const child = new Element(null);
            child.closest = (sel) =>
                /\[data-tab-btn\]/.test(sel) ? btns[name] : null;
            return child;
        };
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
        // tour:默认「不在导览中」;个别用例把它换成活跃态。
        let tourActive = false;
        const tour = { isActive: () => tourActive };
        const fn = new Function(
            "tabbar",
            "TAB_ORDER",
            "activateTab",
            "Element",
            "tour",
            keysDecl + "\n" + listener,
        );
        fn(
            tabbar,
            TAB_ORDER.slice(),
            (name) => activated.push(name),
            Element,
            tour,
        );
        check(!!handler, "监听确实挂在 keydown 上");

        function press(key, from, extra) {
            activated.length = 0;
            focused.length = 0;
            const e = {
                key,
                // from:tab 名 / null(条内非 tab 元素)/ 直接给一个桩元素
                target:
                    from === null
                        ? new Element(null)
                        : typeof from === "string"
                          ? btns[from]
                          : from,
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

            // 焦点落在 tab 的**子元素**上(Tab3 按钮里嵌着 <span>):closest 应当
            // 找回所属 tab 照常切页。认死 e.target 的写法在这一条上会静默失效。
            {
                const r = press("ArrowRight", childOf("master"));
                eq(r.to, "tracks", "焦点在 tab 的子元素上时,→ 照常切页");
            }

            // 「不接管」的四类 —— 每条都对应一个真实的回归风险
            const tourEaten = press("ArrowRight", "master", {
                defaultPrevented: true,
            });
            eq(
                tourEaten.to,
                null,
                "上游已消费该键(defaultPrevented)⇒ 不再切 tab",
            );
            // tour 活着就整体让位:defaultPrevented 只盖得住 tour 认识的 ←/→,
            // Home/End 它不消费 —— 少了 isActive 这条守卫,导览期间 Home/End 会把页
            // 切走而步进机原地不动,spotlight 指向已 display:none 的锚点。
            tourActive = true;
            for (const key of TAB_KEYS) {
                eq(
                    press(key, "master").to,
                    null,
                    "tour 激活期间不接管 " +
                        key +
                        "(含 tour 自己不消费的 Home/End)",
                );
            }
            tourActive = false;
            eq(
                press("ArrowRight", "master").to,
                "tracks",
                "tour 结束后键盘切换恢复",
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
    const listener = sliceCall(app, 'tabbar.addEventListener("keydown",') || "";
    check(
        /activateTab\(TAB_ORDER\[/.test(listener),
        "键盘切换走 activateTab(与点击同一条路径,行为必然一致)",
    );
    check(
        !/call\(\s*"setActiveTab"/.test(listener),
        "键盘处理器不自行调 setActiveTab(避免与 activateTab 各写一次上行)",
    );
}

// =============================================================================
log(fail === 0 ? "\n全部通过 ✅" : "\n失败 " + fail + " 条 ❌");
process.exit(fail === 0 ? 0 : 1);
