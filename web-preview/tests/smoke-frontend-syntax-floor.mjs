// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 前端语法下限守卫(node,无 DOM)
// =============================================================================
// 守的是「旧 WebView2 上整页 SyntaxError」这一类故障。
//
// 【为什么需要】前端跑在用户机器的 WebView2 里,版本由用户的 Evergreen Runtime 决定,
// 不由我们决定。一旦某个 JS 文件用了比 kMinRuntimeMajor 更新的语法,旧 Runtime 上会在
// **解析阶段**就整页 SyntaxError —— 脚本一行都不执行,requestInitialState 永远不会被调,
// 桥永不就绪。它的症状与「加载超时」完全一样,而 try/catch 接不住(解析期错误早于任何
// 执行),是最难归因的一类。所以把它挡在静态面。
//
// 【口径】每条规则写明「特性 → 最低 Chromium 版本」。断言 = 用到的特性所需的最高
// Chromium 版本 ≤ src/plugin-common/PlatformWebView.h 的 kMinRuntimeMajor。
// 想用更新的语法?可以 —— 同一批把 kMinRuntimeMajor 抬上去,并想清楚要不要把一批
// 老 Runtime 用户挡在「请升级」面板后面。这套的作用是让那个决定**必须被显式做出**。
//
// 注意扫描面只含**会被编进插件**的文件(与 cmake/ScvbWebAssets.cmake 同口径):
// web-preview/ 只在浏览器里跑,不受 WebView2 版本约束,不在此列;
// web/js/juce/ 是 JUCE 官方原样副本,不归我们管。
//
// 用法:node web-preview/tests/smoke-frontend-syntax-floor.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有超限特性。
// =============================================================================

import { readFileSync, readdirSync, statSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve, relative } from "node:path";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");

let fail = 0;

/**
 * 特性表 —— 每条:正则 + 人话名字 + 最低 Chromium 主版本。
 * 只列**解析期**就会炸的语法(以及少数在旧引擎上直接 TypeError 的内建方法)。
 */
const FEATURES = [
    { re: /\?\?=|\|\|=|&&=/g, name: "逻辑赋值 ??= / ||= / &&=", chromium: 85 },
    { re: /\?\?/g, name: "空值合并 ??", chromium: 80 },
    { re: /\?\.[a-zA-Z_$([]/g, name: "可选链 ?.", chromium: 80 },
    { re: /^\s*static\s*\{/gm, name: "class static 初始化块", chromium: 94 },
    {
        re: /^\s*#[a-zA-Z_$][\w$]*\s*[=;(]|this\.#/gm,
        name: "class 私有字段",
        chromium: 74,
    },
    { re: /\(\?<[=!]/g, name: "正则 lookbehind", chromium: 62 },
    { re: /\.flatMap\(/g, name: "Array.prototype.flatMap", chromium: 69 },
    { re: /\.at\(/g, name: "Array/String.prototype.at", chromium: 92 },
    {
        re: /\breplaceAll\(/g,
        name: "String.prototype.replaceAll",
        chromium: 85,
    },
    { re: /\bObject\.hasOwn\(/g, name: "Object.hasOwn", chromium: 93 },
    {
        re: /\.toSorted\(|\.toReversed\(|\.findLast\(/g,
        name: "ES2023 数组方法",
        chromium: 110,
    },
    { re: /\bstructuredClone\(/g, name: "structuredClone", chromium: 98 },
    { re: /\bPromise\.any\(/g, name: "Promise.any", chromium: 85 },
    { re: /\bWeakRef\b/g, name: "WeakRef", chromium: 84 },
    { re: /\bimport\.meta\b/g, name: "import.meta", chromium: 64 },
];

/** 顶层 await 单独查:必须是模块顶层(行首无缩进)才算。 */
const TOP_LEVEL_AWAIT = { name: "顶层 await", chromium: 89 };

/**
 * 把注释与字符串字面量掏空(保留换行,行号不变)。
 *
 * 必须做:本仓的注释里大量出现被讨论的语法本身(bridge.js 的文件头就在讲「所有 await
 * 永久挂起」),不掏空就是成片误报,而误报会让这套门禁很快被当噪音关掉。
 * 逐字符状态机,不用正则 —— 正则处理嵌套引号/转义必然出错。
 * 不识别正则字面量(把 / 一律当除号):本仓的正则里没有引号,够用。
 */
function stripCommentsAndStrings(src) {
    let out = "";
    let i = 0;
    const n = src.length;
    while (i < n) {
        const c = src[i];
        const next = src[i + 1];

        if (c === "/" && next === "/") {
            while (i < n && src[i] !== "\n") i++;
            continue;
        }
        if (c === "/" && next === "*") {
            i += 2;
            while (i < n && !(src[i] === "*" && src[i + 1] === "/")) {
                out += src[i] === "\n" ? "\n" : "";
                i++;
            }
            i += 2;
            continue;
        }
        if (c === '"' || c === "'" || c === "`") {
            const quote = c;
            i++;
            while (i < n && src[i] !== quote) {
                if (src[i] === "\\") i++;
                else if (src[i] === "\n") out += "\n";
                i++;
            }
            i++;
            out += '""'; // 占位:保住「这里有个表达式」,但内容不参与匹配
            continue;
        }
        out += c;
        i++;
    }
    return out;
}

/** HTML 只取 <script> 块内容(页面正文/CSS 不受 JS 语法约束)。 */
function scriptsFromHtml(html) {
    let out = "";
    for (const m of html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g))
        out += m[1] + "\n";
    return out;
}

function sourceOf(file) {
    const raw = readFileSync(join(ROOT, file), "utf8");
    return stripCommentsAndStrings(
        file.endsWith(".html") ? scriptsFromHtml(raw) : raw,
    );
}

function listFiles(dir, exts) {
    const out = [];
    const walk = (d) => {
        for (const name of readdirSync(d)) {
            const p = join(d, name);
            if (statSync(p).isDirectory()) walk(p);
            else if (exts.some((e) => name.endsWith(e)))
                out.push(relative(ROOT, p).split("\\").join("/"));
        }
    };
    walk(join(ROOT, dir));
    return out;
}

/** 与 cmake/ScvbWebAssets.cmake 同口径,但排除 web/js/juce(JUCE 官方原样副本)。 */
const files = [
    ...listFiles("web/output", [".js", ".html"]),
    ...listFiles("web/input", [".js", ".html"]),
    ...listFiles("web/shared", [".js"]),
];

/** 读 C++ 真源里的下限,避免这套和插件各写一个数。 */
const header = readFileSync(
    join(ROOT, "src/plugin-common/PlatformWebView.h"),
    "utf8",
);
const m = header.match(/kMinRuntimeMajor\s*=\s*(\d+)/);
if (!m) {
    console.error("  [FAIL] PlatformWebView.h 里读不到 kMinRuntimeMajor");
    process.exit(1);
}
const floor = Number(m[1]);
console.log(
    `kMinRuntimeMajor = ${floor}(真源:src/plugin-common/PlatformWebView.h)`,
);

const used = new Map(); // 特性名 -> { chromium, 命中位置[] }

for (const file of files) {
    const text = sourceOf(file);

    for (const f of FEATURES) {
        f.re.lastIndex = 0;
        if (!f.re.test(text)) continue;
        if (!used.has(f.name))
            used.set(f.name, { chromium: f.chromium, at: [] });
        used.get(f.name).at.push(file);
    }

    // 顶层 await:行首(零缩进)出现 await。缩进的都在函数体里,不算。
    for (const line of text.split("\n")) {
        if (
            /^(?:const|let|var|)\s*[^\s].*\bawait\s/.test(line) &&
            !/^\s/.test(line)
        ) {
            if (!used.has(TOP_LEVEL_AWAIT.name))
                used.set(TOP_LEVEL_AWAIT.name, {
                    chromium: TOP_LEVEL_AWAIT.chromium,
                    at: [],
                });
            used.get(TOP_LEVEL_AWAIT.name).at.push(file);
            break;
        }
    }
}

console.log(`\n扫描 ${files.length} 个会被编进插件的前端文件,用到的受限特性:`);
if (used.size === 0) {
    console.log("  (无)");
}

let highest = 0;
for (const [name, info] of [...used.entries()].sort(
    (a, b) => b[1].chromium - a[1].chromium,
)) {
    const over = info.chromium > floor;
    if (over) fail++;
    highest = Math.max(highest, info.chromium);
    const where = [...new Set(info.at)].slice(0, 3).join(", ");
    console.log(
        `  ${over ? "[FAIL]" : "  ok  "} ${name} —— 需 Chromium ${info.chromium}(${where})`,
    );
    if (over) {
        console.error(
            `  [FAIL] ${name} 需要 Chromium ${info.chromium} > kMinRuntimeMajor ${floor};` +
                " 同批抬高 PlatformWebView.h 的 kMinRuntimeMajor,或改写成更保守的语法。",
        );
    }
}

console.log(
    `\n前端事实下限 = Chromium ${highest || "(无约束)"};插件声明下限 = ${floor}`,
);
console.log(`=== 结果:${fail === 0 ? "全部通过" : fail + " 项超限"} ===`);
process.exit(fail === 0 ? 0 : 1);
