// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB 外壳自适配 —— node 侧冒烟(纯函数 + 接线;SL-380)
// -----------------------------------------------------------------------------
// 页面级那套(smoke-shell-fit-page.mjs)要一个无头 Chrome,缺了会打 SKIP。本套**不需要
// 浏览器**,恒执行,守两件页面级守不到或不该由它守的事:
//
//   ① `fitFactor` 的算术面 —— 含「量化方向必须向下」:`Math.ceil` 版会让
//      `设计宽 × f` 比视口大出零点几个像素,那点溢出在 `scrollWidth ≤ clientWidth`
//      (整数比较)上量不出来,页面级永远抓不到。所以这一条只能在这里钉。
//   ② **唯一写方**:整个 `web/` 里给外壳写 CSS zoom 的地方只允许一处(shell-fit.js)。
//      本卡的病根就是「档位数字」与「实际倍率」两个真源分家 —— Input/Monitor 各写一次
//      zoom、Output 一次都不写。判据钉的是「只有一个写方」这个**状态**,而不是某一行
//      长什么样:谁再加第二处写方,这里立刻红。
//
// 反向注入记录见 PR 描述的「删除式实得」。
//
// 用法:node web-preview/tests/smoke-shell-fit.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有断言失败。**本套没有 SKIP 档**(不依赖任何外部件)。
// =============================================================================

import { readFileSync, readdirSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { pathToFileURL } from "node:url";

const ROOT =
    process.argv[2] && !process.argv[2].startsWith("--")
        ? process.argv[2]
        : resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");

let fail = 0;
const log = (s) => console.log(s);
function check(cond, msg) {
    if (cond) return true;
    fail++;
    console.log(`  [FAIL] ${msg}`);
    return false;
}
function eq(got, want, msg) {
    const a = JSON.stringify(got);
    const b = JSON.stringify(want);
    if (a === b) return true;
    fail++;
    console.log(`  [FAIL] ${msg}\n         实得 ${a}\n         应为 ${b}`);
    return false;
}

const src = (rel) => readFileSync(join(ROOT, rel), "utf8");
const url = (rel) => pathToFileURL(join(ROOT, rel)).href;

const { fitFactor } = await import(url("web/shared/shell-fit.js"));
const { DESIGN } = await import(url("web/shared/design-box.js"));

// ---------------------------------------------------------------- ① 算术面
log("\n=== ① fitFactor ===");
for (const [role, box] of Object.entries(DESIGN)) {
    eq(fitFactor(box.w, box.h, box.w, box.h), 1, `${role}:视口 = 设计盒 → 1`);
    eq(
        fitFactor(box.w * 0.6, box.h * 0.6, box.w, box.h),
        0.6,
        `${role}:两轴 60% → 0.6`,
    );
    eq(
        fitFactor(box.w * 1.5, box.h * 1.5, box.w, box.h),
        1.5,
        `${role}:两轴 150% → 1.5`,
    );
    // 取两轴的**小**者:宽松一轴不许把画面拉出另一轴
    eq(
        fitFactor(box.w * 1.5, box.h * 0.6, box.w, box.h),
        0.6,
        `${role}:宽 ×1.5 / 高 ×0.6 → 取小者 0.6`,
    );
    eq(
        fitFactor(box.w * 0.6, box.h * 1.5, box.w, box.h),
        0.6,
        `${role}:宽 ×0.6 / 高 ×1.5 → 取小者 0.6`,
    );
}

// 量化**方向**:算出来的倍率不许超过真值 —— 超过就是「设计盒比视口大」,即溢出本身。
// 这一格是 ceil / round 版唯一会红的地方(见文件头 ①)。
{
    const box = DESIGN.output;
    let worst = null;
    // 取一串故意除不尽的视口宽(高给足,让宽成为约束轴)
    for (let w = 997; w <= 1237; w += 7) {
        const f = fitFactor(w, 1e6, box.w, box.h);
        const truth = w / box.w;
        if (f > truth) worst = { w, f, truth };
        if (box.w * f > w) worst = { w, f, truth, over: box.w * f - w };
    }
    eq(
        worst,
        null,
        "量化向下:倍率不超过真值,且 设计宽 × 倍率 ≤ 视口宽(ceil/round 版在此必红)",
    );
    // 同时它不许保守过头:量化误差上界 1e-4
    const f = fitFactor(997, 1e6, box.w, box.h);
    check(
        997 / box.w - f < 1e-4,
        `量化误差在 1e-4 内(实得 ${997 / box.w - f})`,
    );
}

// 退化输入一律回落 1:首帧视口未定 / 页面隐藏时不许把画面缩成 0 或 NaN
for (const [vw, vh, name] of [
    [0, 0, "视口 0×0"],
    [-10, 100, "视口负宽"],
    [NaN, 100, "视口 NaN"],
    [undefined, undefined, "视口缺参"],
]) {
    eq(fitFactor(vw, vh, 1180, 780), 1, `${name} → 回落 1`);
}
eq(fitFactor(1180, 780, 0, 0), 1, "设计盒 0×0 → 回落 1");

// ---------------------------------------------------------------- ② 唯一写方
log("\n=== ② CSS zoom 的唯一写方 ===");

/**
 * 剥注释(块注释 + 整行 `//` 注释)。
 *
 * 必须剥:本卡的几处改动**在注释里逐字提到**了被撤掉的那两行(「这里不再写
 * shell.style.zoom」之类)。不剥的话判据会命中自己的说明文字,而那恰好是它最该
 * 放过的东西 —— 也是文本级判据最常见的自欺形态。
 *
 * 只剥**整行** `//`:行尾注释里出现 `//` 的合法形态(URL 里的 `https://`)会被
 * 一刀切错,而整行注释足以覆盖本仓库的注释风格。
 */
function stripComments(text) {
    const noBlock = text.replace(/\/\*[\s\S]*?\*\//g, "");
    return noBlock
        .split("\n")
        .filter((line) => !/^\s*\/\//.test(line))
        .join("\n");
}

/** 递归收 web/ 下的 .js(node_modules 之类本仓库里没有,仍防一手)。 */
function jsFiles(dir, out = []) {
    for (const name of readdirSync(dir)) {
        if (name === "node_modules" || name.startsWith(".")) continue;
        const abs = join(dir, name);
        if (statSync(abs).isDirectory()) jsFiles(abs, out);
        else if (name.endsWith(".js")) out.push(abs);
    }
    return out;
}

// 模式拼装,不写字面量:本文件自己也在被扫的目录之外,但「扫描器把自己扫成命中」
// 这个坑不值得再踩一次(SL-181)。
const ZOOM_WRITE = new RegExp(
    ["\\.", "style", "\\.", "zoom", "\\s*=[^=]"].join(""),
);

const writers = [];
for (const abs of jsFiles(join(ROOT, "web"))) {
    const body = stripComments(readFileSync(abs, "utf8"));
    for (const line of body.split("\n")) {
        if (ZOOM_WRITE.test(line)) {
            writers.push(abs.slice(ROOT.length + 1).replace(/\\/g, "/"));
            break;
        }
    }
}
eq(
    writers.sort(),
    ["web/shared/shell-fit.js"],
    "web/ 里给外壳写 CSS zoom 的文件只有 shell-fit.js",
);

// ---------------------------------------------------------------- ③ 三侧接线
log("\n=== ③ 三侧都装了自适配 ===");
const PAGES = [
    { role: "output", file: "web/output/app.js", box: "DESIGN.output" },
    { role: "input", file: "web/input/app.js", box: "DESIGN.input" },
    { role: "monitor", file: "web/monitor/app.js", box: "MONITOR_DESIGN" },
];
for (const p of PAGES) {
    const body = stripComments(src(p.file));
    check(
        /installShellFit/.test(body) &&
            /from "\.\.\/shared\/shell-fit\.js"/.test(body),
        `${p.role}:导入并调用 installShellFit`,
    );
    check(
        new RegExp("box:\\s*" + p.box.replace(".", "\\.")).test(body),
        `${p.role}:设计盒取 ${p.box}(不写第二份数字)`,
    );
}

// ---- 后备存储 k 的读方:清单**现算**,不手写 --------------------------------
// 第一版这里是一句手写清单(`["web/output/tab-wave.js", "web/monitor/app.js"]`),于是
// 同族的第三个文件 `web/output/tab-master.js` 整个漏掉了 —— 那里有**两处**逐字同款的读点
// (Tab1 轨迹图的 getUiScale、它的倍率账),漏了之后没有任何东西会红。
//
// ⚠ 这份扫描**自己读文件**,不能改写成 shell 的 `grep`:`web/output/tab-master.js` 里有
// 一个真的 NUL 字节(那句 `join` 拿它当分隔符),`grep` 会把整个文件判成二进制、只印一行
// "Binary file matches" 而不印命中行 —— 当初漏掉那两处,这是原因之一。下面第一条断言
// 实际验证「这个文件确实被读进来了」,免得哪天扫描静默跳过它。
{
    const files = jsFiles(join(ROOT, "web"));
    const rel = (abs) =>
        abs
            .slice(ROOT.length + 1)
            .split("\\")
            .join("/");
    check(
        files.some((f) => rel(f) === "web/output/tab-master.js"),
        "扫描面覆盖 web/output/tab-master.js(含 NUL 字节,shell grep 会跳过它)",
    );

    // 「谁在给后备存储供倍率」的两种形态,逐个要求供的是 backingFitFactor()。
    // 钉的是**状态**(所有供给点都读粗量化倍率),不是某一行长什么样。
    const SUPPLY = [
        { re: /getUiScale\s*:\s*([^,\n]*)/g, what: "getUiScale 回调" },
        { re: /backingScale\(\s*([^,]*)/g, what: "backingScale 的倍率实参" },
    ];
    const offenders = [];
    for (const abs of files) {
        const body = stripComments(readFileSync(abs, "utf8"));
        for (const { re, what } of SUPPLY) {
            re.lastIndex = 0;
            let m;
            while ((m = re.exec(body))) {
                const arg = m[1];
                // 函数**声明**不是供给点:`export function backingScale(uiScale, dpr)`
                // 里的 `uiScale` 是形参名。按前文是不是 `function ` 判,不走文件白名单。
                if (/function\s*$/.test(body.slice(0, m.index))) continue;
                // 形参默认值同理(trajectory-chart 自己那两行)
                if (/^\s*$/.test(arg)) continue;
                if (/typeof|function|=>\s*1\b/.test(arg)) continue;
                if (!/backingFitFactor\(\)|getUiScale\(\)/.test(arg)) {
                    offenders.push(`${rel(abs)}:${what} → ${arg.trim()}`);
                }
            }
        }
    }
    eq(offenders, [], "后备存储的倍率供给点全部读 backingFitFactor()");

    // 反向:记「倍率账」的地方(`local.lastUiScale` 与它比的那个值)也必须来自
    // backingFitFactor()。判据钉在**每一处赋值**上,不是「这个文件里出现过就算」——
    // 后者是第一版的形态,而 tab-master.js 里有两处同款读点:改回去一处、留下另一处,
    // 文件级判据照样绿(删除式实测到的第二个洞)。
    // `uiScale` 与 `lastUiScale` 首字母大小写不同,\b + 大小写敏感足以分开两者。
    const accounts = [];
    for (const abs of files) {
        const body = stripComments(readFileSync(abs, "utf8"));
        if (!/lastUiScale/.test(body)) continue;
        const re = /\buiScale\s*=\s*([^;]*);/g;
        let m;
        let seen = 0;
        while ((m = re.exec(body))) {
            seen++;
            if (!/backingFitFactor\(\)/.test(m[1])) {
                accounts.push(`${rel(abs)}: uiScale = ${m[1].trim()}`);
            }
        }
        if (seen === 0) accounts.push(`${rel(abs)}: 有倍率账却找不到赋值点`);
    }
    eq(accounts, [], "倍率账的每一处赋值都取自 backingFitFactor()");
}

// ---------------------------------------------------------------- ④ 壳页扮宿主
log("\n=== ④ 预览壳扮宿主 ===");
{
    const body = stripComments(src("web-preview/shell.js"));
    // 钉的是**调用点**(`hostResize: <某个函数>`),不是形参名:只删调用点、留着
    // `hostResize = null` 那个形参声明的话,"名字在不在" 这种判据是绿的(删除式实测)。
    check(
        /hostResize:\s*\w/.test(body),
        "shell.js:mountPreview 把窗口尺寸函数传进 injectAndMount",
    );
    check(/hostResize\(/.test(body), "shell.js:setUiScale 被接受之后真的调它");
    check(
        /box\.w \* s/.test(body) && /box\.h \* s/.test(body),
        "shell.js:iframe 尺寸 = 设计盒 × 档位",
    );
    // 从前是 `width = "100%"` + minWidth 兜底;那样预览视口恒大于设计盒,
    // 与真机(WebView 恰好铺满 setSize 出来的编辑器)对不上。
    check(
        !/frame\.style\.minWidth/.test(body) &&
            !/frame\.style\.minHeight/.test(body),
        "shell.js:iframe 不再靠 min-width/min-height 兜底",
    );
}

log("");
if (fail === 0) {
    log("✅ smoke-shell-fit:全绿");
    process.exit(0);
}
log(`❌ smoke-shell-fit:${fail} 条断言失败`);
process.exit(1);
