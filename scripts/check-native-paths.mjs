// SPDX-License-Identifier: GPL-3.0-or-later
// check-native-paths.mjs —— detect-native 的 NATIVE_RE 判据回归(零依赖,Node >= 18,ESM)。
//   真源 = .github/workflows/build-vst3.yml 里 detect-native 步骤的那条 `NATIVE_RE=...`。
//   [SL-283] 这条正则决定「子 PR 碰没碰 native 路径 ⇒ 要不要在 CI 上编一遍」。它一旦被
//   改坏,失效形态是**静默的**:动了 C++ 的子 PR 不再编译,而 CI 照样全绿 —— 正是 [SL-283]
//   要补的那个洞原样复发。所以判据本身也要有判据。
//
//   四件事:
//     ① 正则**从 workflow 文件里读出来**,不在这里手抄一份 —— 手抄的话两边会各自漂移,
//        而漂移的方向恰好是「测试还绿着,CI 已经不编了」。
//     ② 正反例逐条断言(命中面 = 会进构建的路径;不命中面 = 文档 / 脚本 / 浏览器预览)。
//     ②b **两个引擎的命中集合逐条一致**:CI 侧执行者是 `grep -E`(ERE),这里是 JS
//        RegExp。只在 JS 下断言等于「在另一个引擎下验收」,`\d` 这类两边都合法但含义
//        不同的写法会让这里绿、CI 悄悄少编。
//     ③ **删除式验证**:把正则拆成分支,逐个删掉一个分支,要求至少一个正例因此变红。
//        没有这一步,一条写错到永不匹配的分支(比如漏了转义)也能让 ① 全绿。
//   用法:
//     node scripts/check-native-paths.mjs [--help]
//   退出码:任一断言失败 = 1,全通过 = 0。

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(
    path.dirname(fileURLToPath(import.meta.url)),
    "..",
);
const WORKFLOW = path.join(repoRoot, ".github", "workflows", "build-vst3.yml");

if (process.argv.includes("--help")) {
    console.log("用法: node scripts/check-native-paths.mjs");
    console.log(
        "断言 build-vst3.yml 的 NATIVE_RE 命中面正确,且每个分支都有牙。",
    );
    process.exit(0);
}

// ---- ① 从真源读正则 ----------------------------------------------------------
const yml = fs.readFileSync(WORKFLOW, "utf8");
// 行锚(`^\s*`)不是装饰:无锚的话,谁哪天在**注释里**逐字引用一句 NATIVE_RE='…'
// 作例子,`match` 取到的就是那个例子而不是真判据 —— 于是这个门禁会去验一条根本没在
// 用的正则,真判据被改坏它照样绿。当前文件里这是唯一一处单引号形态,加锚是免疫,不是修 bug。
const m = yml.match(/^\s*NATIVE_RE='([^']+)'/m);
if (!m) {
    console.error(
        "check-native-paths: 在 .github/workflows/build-vst3.yml 里找不到 NATIVE_RE='...'。",
    );
    console.error(
        "  detect-native 的路径判据被改名或删掉了?这个测试与它是一对,改一个就要改另一个。",
    );
    process.exit(1);
}
const PATTERN = m[1];
const rx = new RegExp(PATTERN);

// ---- ①b 引擎分叉:判据与执行者不是同一个正则引擎 -------------------------------
// CI 侧是 `grep -E`(POSIX ERE),这里是 JS `RegExp`。真源读对了,但语义是在**另一个
// 引擎**下被断言的。两种分叉的严重性不同:
//   · 写了 ERE 不认的语法(`(?:…)` / `(?=…)`)⇒ grep 退出 >=2 ⇒ 被 detect-native 里
//     那条 `rc >= 2` 分档接住,倒向照编 —— **安全方向**。
//   · 写了两边都合法**但含义不同**的东西 ⇒ 这里绿、CI 的命中面悄悄变窄 —— **危险方向**,
//     正是本脚本要防的那类。典型例子:`\d` 在 JS 里是数字类,在 ERE 里就是字面量 `d`
//     (实测 `^\.sccache-sha\d56$`:JS 匹配 `.sccache-sha256`,`grep -E` 不匹配)。
// 所以下面两道都要:
const CHARSET_WHITELIST = /[^\w^$()|/.\\-]/;
if (CHARSET_WHITELIST.test(PATTERN)) {
    console.error(
        "check-native-paths: NATIVE_RE 含白名单之外的正则字符 —— CI 侧用 grep -E(ERE)、" +
            "这里用 JS RegExp,复杂语法在两个引擎下可能含义不同。",
    );
    console.error(`  实际拿到: ${PATTERN}`);
    process.exit(1);
}
// **白名单挡不住危险方向**:`\d` 的两个字符(`\` 与 `d`)都在白名单里。所以真正有牙的是
// 下面这道 —— 把同一批用例喂给**真的 `grep -E`**,两个引擎的命中集合必须逐条一致。
function ereHitSet(pattern, paths) {
    const r = spawnSync("grep", ["-E", pattern], {
        input: paths.join("\n") + "\n",
        encoding: "utf8",
    });
    if (r.error) return null; // 本机没有 grep(非 CI 环境),交给调用处降级
    // `r.signal` 有值时 `r.status` 是 null,会从 `>= 2` 底下穿过去,然后拿着**被截断的**
    // stdout 去比对 —— 结果仍是红(命中集合对不上),但错因会被误报成「两个引擎不一致」。
    // 概率极低,但错因说准的成本是一个 `||`(PR#180 复审采纳)。
    if (r.signal || r.status >= 2) {
        console.error(
            r.signal
                ? `check-native-paths: grep 被信号 ${r.signal} 终止,拿不到可信的命中集合(不是正则的问题)。`
                : `check-native-paths: grep -E 拒绝了这条正则(exit=${r.status})—— ERE 不认它的语法。`,
        );
        console.error((r.stderr || "").trim());
        process.exit(1);
    }
    return new Set(r.stdout.split("\n").filter(Boolean));
}

// ---- ② 正反例 ---------------------------------------------------------------
// 命中 = 改了它就必须在 CI 上编一遍。
const HIT = [
    "src/core/analysis/Loudness.cpp",
    "src/output/OutputEditor.h",
    "src/input/CMakeLists.txt",
    "tests/core/test_state.cpp",
    "tests/webview/test_web_assets_embedded.cpp",
    "cmake/ScvbWebAssets.cmake",
    "CMakeLists.txt",
    // web/ 不是纯前端:经 juce_add_binary_data 编进插件二进制(cmake/ScvbWebAssets.cmake)。
    // 重名 / EXTRA_DIRS 扫空是 configure 期 FATAL_ERROR,「页面要取的文件真进了包」由
    // ctest 的 test_web_assets_embedded 兜 —— 两道都只在编译时跑,跳过构建 = 两道都不跑。
    "web/monitor/app.js",
    "web/shared/trajectory-chart.js",
    "web/fonts/IBMPlexSans-Regular.woff2",
    "third_party/miniz/miniz.c",
    ".juce-version",
    ".pluginval-version",
    ".sccache-version",
    ".sccache-sha256",
    ".github/workflows/build-vst3.yml",
];
// 不命中 = 不进构建,跳过是对的。
const MISS = [
    "README.md",
    "CLAUDE.md",
    "CHANGELOG.md",
    "docs/PARAMETERS.md",
    "docs/adr/ADR-011.md",
    "scripts/gates.ps1",
    "scripts/check-privacy.mjs",
    "scripts/check-native-paths.mjs", // 本文件:改测试不改产物
    // web-preview/ 是浏览器预览,不参与构建。它必须**不**被 `^web/` 误命中(前缀要带 /)。
    "web-preview/tests/smoke.mjs",
    "web-preview/index.html",
    "screenshots/foo.png",
    "LICENSES/GPL-3.0-or-later.txt",
    ".github/workflows/format.yml", // 别的 workflow 不影响 native 产物
    ".clang-format",
];

let failed = 0;
for (const p of HIT) {
    if (!rx.test(p)) {
        console.error(`  [FAIL] 应命中却没命中: ${p}`);
        failed++;
    }
}
for (const p of MISS) {
    if (rx.test(p)) {
        console.error(`  [FAIL] 不该命中却命中了: ${p}`);
        failed++;
    }
}
if (failed > 0) {
    console.error(
        `check-native-paths: ${failed} 条断言失败 —— NATIVE_RE 的命中面变了,先确认这是有意的。`,
    );
    process.exit(1);
}
console.log(`① 正反例: ${HIT.length} 命中 / ${MISS.length} 不命中,全过。`);

// ---- ②b 两个引擎的命中集合必须逐条一致 ----------------------------------------
const ALL = [...HIT, ...MISS];
const ere = ereHitSet(PATTERN, ALL);
if (ere === null) {
    // CI(ubuntu)上一定有 grep;本机没有时降级成警告 —— 但要说清降级掉的是**哪一档**,
    // 别让人以为「绿了就等于两个引擎一致」。
    console.warn(
        "  [WARN] 本机没有 grep,跳过「JS ↔ grep -E 命中集合一致」这一档。" +
            "白名单挡不住 `\\d` 这类两边都合法但含义不同的写法,这一档只有 CI 上跑得到。",
    );
} else {
    const js = new Set(ALL.filter((p) => rx.test(p)));
    const onlyJs = [...js].filter((p) => !ere.has(p));
    const onlyEre = [...ere].filter((p) => !js.has(p));
    if (onlyJs.length || onlyEre.length) {
        console.error(
            "check-native-paths: **两个引擎的命中面不一致** —— 这里(JS RegExp)绿了,但 CI 用的是 grep -E。",
        );
        for (const p of onlyJs)
            console.error(`  [FAIL] 只有 JS 命中(CI 会漏编): ${p}`);
        for (const p of onlyEre)
            console.error(`  [FAIL] 只有 ERE 命中(本测试判据失真): ${p}`);
        console.error(
            "  典型成因:`\\d` / `\\s` / `\\b` 这类 JS 认、ERE 当字面量的转义。改用 ERE 也认的写法。",
        );
        process.exit(1);
    }
    console.log(
        `②b 引擎一致性: JS 与 grep -E 对 ${ALL.length} 条用例命中集合逐条一致。`,
    );
}

// ---- ③ 删除式验证 -----------------------------------------------------------
// 形态必须是 ^(a|b|c...) —— 变了就让这个测试硬红,而不是悄悄跳过删除式验证。
if (!(PATTERN.startsWith("^(") && PATTERN.endsWith(")"))) {
    console.error(
        "check-native-paths: NATIVE_RE 的形态不再是 ^(a|b|c...),删除式验证的拆法要跟着改。",
    );
    console.error(`  实际拿到: ${PATTERN}`);
    process.exit(1);
}
const alts = PATTERN.slice(2, -1).split("|");
// 这里原本写的是「拆完能拼回去就说明拆法成立」—— 那是个**恒真式**
// (`split("|").join("|")` 恒等于原串),它什么都拦不住,却在注释里声称拦得住
// (PR#180 复审指出,实测嵌套组也照样「拼得回去」)。换成真的检查:
// 白名单放行 `(` 与 `)`,所以将来写成 `^(src/|(tests|cmake)/)` 是合法的,而按顶层 `|`
// 硬拆会拼出 `^(src/|cmake)/)` 这类非法正则 —— `new RegExp` 抛 SyntaxError 且无人接,
// 报错形态从这句中文退化成栈回溯。当前正则不需要嵌套组,直接判负并说清原因。
if (alts.some((a) => a.includes("(") || a.includes(")"))) {
    console.error(
        "check-native-paths: NATIVE_RE 里出现了嵌套组 —— 删除式验证按顶层 '|' 拆分," +
            "拆到嵌套组会拼出非法正则。要用嵌套组,得先把 ③ 的拆法换成真正的解析。",
    );
    console.error(`  实际拿到: ${PATTERN}`);
    process.exit(1);
}

const toothless = [];
for (let i = 0; i < alts.length; i++) {
    const rest = new RegExp(`^(${alts.filter((_, j) => j !== i).join("|")})`);
    const broke = HIT.filter((p) => rx.test(p) && !rest.test(p));
    if (broke.length === 0) {
        toothless.push(alts[i]);
        console.error(
            `  [FAIL] 删掉分支 ${JSON.stringify(alts[i])} 没有任何正例变红(判据无牙)`,
        );
    }
}
if (toothless.length > 0) {
    console.error(
        "check-native-paths: 上列分支没有用例兜着 —— 要么它写错了永不匹配,要么补一条正例。",
    );
    process.exit(1);
}
console.log(
    `② 删除式验证: ${alts.length} 个分支,删掉任一都有正例变红,判据有牙。`,
);
console.log("check-native-paths 通过。");
