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

// 后备存储 k 的读方改读实际倍率,不读档位数字([SL-380])
for (const f of ["web/output/tab-wave.js", "web/monitor/app.js"]) {
    const body = stripComments(src(f));
    check(
        /shellFitFactor\(\)/.test(body),
        `${f}:后备存储倍率读 shellFitFactor()`,
    );
}
// tab-wave 里**一处**档位读法都不许剩:它有两个读点(backingK 与 lastUiScale 账),
// 只断「shellFitFactor 在场」的话,改回去一个、留下另一个,判据照样绿(删除式实测)。
// Monitor 不在此列 —— 它的 syncUiFromState 读 `ui.scale` 是为了对齐下拉选中项,合法。
check(
    !/\.ui\b.*\.scale\b/.test(stripComments(src("web/output/tab-wave.js"))),
    "web/output/tab-wave.js:不再从 state 读档位数字当倍率",
);

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
