// SPDX-License-Identifier: GPL-3.0-or-later
// check-native-paths.mjs —— detect-native 的 NATIVE_RE 判据回归(零依赖,Node >= 18,ESM)。
//   真源 = .github/workflows/build-vst3.yml 里 detect-native 步骤的那条 `NATIVE_RE=...`。
//   [SL-283] 这条正则决定「子 PR 碰没碰 native 路径 ⇒ 要不要在 CI 上编一遍」。它一旦被
//   改坏,失效形态是**静默的**:动了 C++ 的子 PR 不再编译,而 CI 照样全绿 —— 正是 [SL-283]
//   要补的那个洞原样复发。所以判据本身也要有判据。
//
//   三件事:
//     ① 正则**从 workflow 文件里读出来**,不在这里手抄一份 —— 手抄的话两边会各自漂移,
//        而漂移的方向恰好是「测试还绿着,CI 已经不编了」。
//     ② 正反例逐条断言(命中面 = 会进构建的路径;不命中面 = 文档 / 脚本 / 浏览器预览)。
//     ③ **删除式验证**:把正则拆成分支,逐个删掉一个分支,要求至少一个正例因此变红。
//        没有这一步,一条写错到永不匹配的分支(比如漏了转义)也能让 ① 全绿。
//   用法:
//     node scripts/check-native-paths.mjs [--help]
//   退出码:任一断言失败 = 1,全通过 = 0。

import fs from "node:fs";
import path from "node:path";
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
const m = yml.match(/NATIVE_RE='([^']+)'/);
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
// 拆了要能原样拼回去,否则说明某个分支里有字面量 '|'(比如写进了字符类),拆法不成立。
if (`^(${alts.join("|")})` !== PATTERN) {
    console.error(
        "check-native-paths: 按 '|' 拆分后拼不回原正则,删除式验证不可靠。",
    );
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
