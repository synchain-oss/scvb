// SPDX-License-Identifier: GPL-3.0-or-later
// check-native-paths.mjs —— detect-native 的 NATIVE_RE 判据回归(零依赖,Node >= 18,ESM)。
//   真源 = .github/workflows/build-vst3.yml 里 detect-native 步骤的那条 `NATIVE_RE=...`。
//   [SL-283] 这条正则决定「子 PR 碰没碰 native 路径 ⇒ 要不要在 CI 上编一遍」。它一旦被
//   改坏,失效形态是**静默的**:动了 C++ 的子 PR 不再编译,而 CI 照样全绿 —— 正是 [SL-283]
//   要补的那个洞原样复发。所以判据本身也要有判据。
//
//   五件事:
//     ① 正则**从 workflow 文件里读出来**,不在这里手抄一份 —— 手抄的话两边会各自漂移,
//        而漂移的方向恰好是「测试还绿着,CI 已经不编了」。
//     ② 正反例逐条断言(命中面 = 会进构建的路径;不命中面 = 文档 / 脚本 / 浏览器预览)。
//     ②b **两个引擎的命中集合逐条一致**:CI 侧执行者是 `grep -E`(ERE),这里是 JS
//        RegExp。只在 JS 下断言等于「在另一个引擎下验收」,`\d` 这类两边都合法但含义
//        不同的写法会让这里绿、CI 悄悄少编。
//     ③ **删除式验证**:把正则拆成分支,逐个删掉一个分支,要求至少一个正例因此变红。
//        没有这一步,一条写错到永不匹配的分支(比如漏了转义)也能让 ② 全绿。
//     ④ **顶层条目全覆盖**:上面那几档保的都是「这条正则别被改坏」,保不住「构建面新长出
//        一个它没覆盖的目录」。穷举 `git ls-files` 的顶层条目,要求每一个都落进命中面或
//        **显式**的 non-native 清单,两边都不沾即判红 —— 新目录红在加它的那个 PR 上。
//        **边界:这一档只兜顶层。**已在 NON_NATIVE_TOP 里的目录**内部**长出构建边
//        (哪天 CMake `add_custom_command` 消费 `scripts/foo.py`,此后只改 foo.py 的子 PR
//        仍静默 native=false)不在本档能力内 —— 别把它读成「不再静默漏编」的全称保证。[SL-286]
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

// `process.env.CI` 是**字符串**:`CI=false` / `CI=0` 都是非空串,裸真值判断一律为真。
// 后果是本地误判成 CI ⇒ 本该降级的档硬红(假红,不是静默放过),但「显式说了 false 却
// 不算数」本身就会让人不再信这个开关(PR#180 复审转 [SL-286])。
const isCI = (() => {
    const v = process.env.CI;
    if (v === undefined || v === null) return false;
    const t = String(v).trim().toLowerCase();
    return t !== "" && t !== "false" && t !== "0";
})();

// ---- ②b 引擎分叉:判据与执行者不是同一个正则引擎 -------------------------------
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
    if (r.error) {
        // **只有「找不到 grep」才允许降级**。原先一句 `if (r.error) return null` 把
        // 权限不足、进程数耗尽这些失败也一律说成「本机没有 grep」—— 错因说窄了,人会
        // 照着那句话去装一个已经装好的东西。与下面 `r.signal` 那条同类(PR#180 复审转 [SL-286])。
        if (r.error.code === "ENOENT") return null; // 真的没有 grep,交给调用处降级
        console.error(
            `check-native-paths: 调用 grep 失败(${r.error.code || r.error.message})——不是「没装 grep」,也不是正则的问题。`,
        );
        process.exit(1);
    }
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
console.log(`② 正反例: ${HIT.length} 命中 / ${MISS.length} 不命中,全过。`);

// ---- ②b 两个引擎的命中集合必须逐条一致 ----------------------------------------
const ALL = [...HIT, ...MISS];
const ere = ereHitSet(PATTERN, ALL);
if (ere === null) {
    // **在 CI 上必须硬红**(PR#180 复审采纳)。这一档在本地可以降级,是因为我在
    // gates.ps1 的注释里写了 **本地绿不等于这几档验过,它们以 CI(ubuntu)为准** ——
    // (不加引号:那句话的措辞本 PR 改过一次,带引号的转述会立刻变成引不到的原文;
    //  要找它就 grep 「本地绿不等于」。)
    // 那句话要成立,CI 上就不能也悄悄跳过它。否则「以 CI 为准」这条兜底本身是空的:
    // 门禁绿着,而它自称验过的那一档一次没跑,正是本卡通篇在治的形态。
    // ubuntu runner 一定有 grep,所以这条分支今天不会触发 —— 它守的是「哪天不再有」。
    if (isCI) {
        console.error(
            "check-native-paths: 当前在 CI 环境却找不到 grep,「JS ↔ grep -E 命中集合一致」这一档无法执行。",
        );
        console.error(
            "  这一档只有 CI 跑得到(本地允许降级),所以 CI 上跳过它 = 门禁绿着但没验过,必须判负。",
        );
        process.exit(1);
    }
    // 本地(非 CI)降级成警告 —— 但要说清降级掉的是**哪一档**,别让人以为
    // 「绿了就等于两个引擎一致」。
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
    `③ 删除式验证: ${alts.length} 个分支,删掉任一都有正例变红,判据有牙。`,
);

// ---- ④ 顶层条目全覆盖:构建面新长出一个目录,必须当场分类 ----------------------
// ①②③ 保的都是「**这条正则**别被改坏」。它们保不住另一个方向:**构建面长出一个正则
// 没覆盖的新目录**。将来新增 `res/` 并被 CMake 消费,加它的那个 PR 自己一定会编
// (它必然动 `CMakeLists.txt` ⇒ 命中 NATIVE_RE),于是**当场看不出问题**;而此后
// 只碰 `res/` 的子 PR 静默判 native=false —— [SL-283] 那个洞原样复发,触发事件从
// 「改正则」换成「加目录」。[SL-286]
//
// 所以这一档不看正则,看**仓库现状**:穷举顶层条目,要求每一个都被显式分类。于是新目录
// 红在**加它的那个 PR** 上,而不是等下一张子 PR 静默漏编 —— 拦在引入时刻,不是后果时刻。
//
// ⚠ 白名单**逐条列举,不留通配兜底**。写成「其余都算 non-native」这道门当场退化成恒真,
//    与 [SL-291] 刚修掉的 `braceBody`「匹配不到就放过」是镜像的同一种病。代价是新增一个
//    `screenshots-*` 也得来加一行 —— 这正是要的:**分类是一个动作,不是默认值**。
const NON_NATIVE_TOP = [
    ".clang-format",
    ".editorconfig",
    ".gitattributes",
    ".gitignore",
    ".gitleaks-version",
    ".gitleaks.toml",
    ".markdown-link-check.json",
    ".prettierignore",
    "CHANGELOG.md",
    "CLAUDE.md",
    "CODE_OF_CONDUCT.md",
    "CODE_OF_CONDUCT.zh-CN.md",
    "CONTRIBUTING.md",
    "LICENSE",
    "LICENSES/",
    "README.md",
    "README.zh-CN.md",
    "REUSE.toml",
    "SECURITY.md",
    "THIRD-PARTY-NOTICES.md",
    "docs/",
    "screenshots-d1/",
    "screenshots-font/",
    "screenshots-sl193/",
    "screenshots-sl211/",
    "screenshots-sl213/",
    "screenshots-sl230/",
    "screenshots-sl272/",
    "screenshots-t41/",
    "screenshots-t43/",
    "screenshots-t46/",
    "screenshots-t48/",
    "screenshots/",
    "scripts/",
    "spikes/",
    "web-preview/",
];
// **混合**顶层条目:内部既有命中面又有不命中面,所以两张清单哪张都放不下它。
// 今天只有 `.github/`:`workflows/build-vst3.yml` 命中(改构建配方要自己编一遍),
// 同目录的 `format.yml` 等 14 个文件不命中。单列一张,并在下面**断言它真的是混合的** ——
// 否则「混合」会变成一个万能借口:哪天 `.github/` 整个进了构建,把它挂在这里就能瞒过去。
const MIXED_TOP = [".github/"];
// **混合条目的命中集合也要逐条钉死**(复审第 1 轮)。只断言「0 < 命中 < 总数」的话,
// `.github/` 就成了本卡唯一豁免于「新长出的东西必须当场分类」的入口 —— 而这道门整篇
// 就是来消灭这种豁免的。具体形态:哪天加 `.github/actions/build-cache/action.yml` 并被
// `build-vst3.yml` 消费,`.github/` 仍然「有的命中有的不命中」⇒ 照绿,而新长出来的那棵
// 子树谁也没分类过。钉死集合之后,`.github/` 内部再长出命中面也必须来登记一笔。
const MIXED_EXPECT = {
    ".github/": [".github/workflows/build-vst3.yml"],
};

function topLevelOf(file) {
    const i = file.indexOf("/");
    return i < 0 ? file : file.slice(0, i + 1);
}

// `core.quotepath=false` 不是抄来的防御,是实测:本仓**已经有**一个非 ASCII 路径
// (`docs/design/SCVB 设计稿.dc.html`)。默认 `quotepath=true` 会把它整条输出成
// `"docs/design/SCVB \350\256\276…"` —— **带前导双引号**,顶层段于是被切成 `"docs/`,
// 一个仓里根本不存在的条目。实测不加这个开关顶层条目从 47 变 48,多出来的正是 `"docs/`,
// 它既不命中 NATIVE_RE 也不在清单里 ⇒ 这道门会对着一个幻影**常红**,而且报的还是错的因。
// 与 detect-native 用同一个开关同因(build-vst3.yml 里那段 `core.quotepath=false` 注释)。
function trackedFiles() {
    const r = spawnSync("git", ["-c", "core.quotepath=false", "ls-files"], {
        encoding: "utf8",
        cwd: repoRoot,
    });
    if (r.error) {
        // 只有「找不到 git」才允许降级;别的失败(权限、进程数耗尽…)说成「没装 git」
        // 就是把错因说窄,人会照着那句话去装一个已经装好的东西。
        if (r.error.code === "ENOENT") return null;
        console.error(
            `check-native-paths: 调用 git 失败(${r.error.code || r.error.message})——不是「没装 git」。`,
        );
        process.exit(1);
    }
    if (r.signal || r.status !== 0) {
        console.error(
            r.signal
                ? `check-native-paths: git ls-files 被信号 ${r.signal} 终止,拿不到可信的文件清单。`
                : `check-native-paths: git ls-files 失败(exit=${r.status})——这里不是 git 工作树?`,
        );
        console.error((r.stderr || "").trim());
        process.exit(1);
    }
    return r.stdout.split("\n").filter(Boolean);
}

const tracked = trackedFiles();
if (tracked === null) {
    // 与 ②b 同一条口径:CI 上跳过 = 门禁绿着但没验过,必须判负。
    if (isCI) {
        console.error(
            "check-native-paths: 当前在 CI 环境却找不到 git,「顶层条目全覆盖」与「全仓路径引擎对拍」两档都无法执行。",
        );
        process.exit(1);
    }
    console.warn(
        "  [WARN] 本机没有 git,跳过「顶层条目全覆盖」**与「全仓路径引擎对拍」两档** —— " +
            "后者嵌在前者里(拿不到全仓路径就无从对拍),所以这一行代表两档没跑。" +
            "新增顶层目录漏进 NATIVE_RE 这一类只有跑得到 git 的地方查得出。",
    );
} else {
    const byTop = new Map();
    for (const f of tracked) {
        const t = topLevelOf(f);
        if (!byTop.has(t)) byTop.set(t, []);
        byTop.get(t).push(f);
    }

    const unclassified = [];
    const listRot = [];
    // 引擎分叉与三张清单**一点关系都没有**(它是「JS 与 grep -E 对同一条正则的理解不一样」)。
    // 混进 listRot 会让它顶着「清单与仓库现状对不上」的抬头打印,人会先去翻 NON_NATIVE_TOP
    // 找一条根本不存在的错行 —— 错因说不准,正是本卡通篇在治的东西(复审第 2 轮)。
    const engineRot = [];
    for (const [top, group] of [...byTop].sort()) {
        const hits = group.filter((f) => rx.test(f)).length;
        const inNon = NON_NATIVE_TOP.includes(top);
        const inMixed = MIXED_TOP.includes(top);
        // 顶层条目自身命中 NATIVE_RE(`src/` 命中 `^src/`,`CMakeLists.txt` 命中它那条)
        // = 整棵子树都在命中面里,不需要上任何清单。
        if (rx.test(top)) {
            if (inNon || inMixed)
                listRot.push(
                    `${top}:命中 NATIVE_RE(整体进构建),却还挂在清单里 —— 清单该删掉它`,
                );
            continue;
        }
        if (inMixed) {
            // 断言它**真的**混合:全命中或全不命中都说明它不该在这张单子上。
            if (hits === 0)
                listRot.push(
                    `${top}:挂在 MIXED_TOP,但里面一个文件都不命中 NATIVE_RE —— 它是纯 non-native,挪去 NON_NATIVE_TOP`,
                );
            else if (hits === group.length)
                listRot.push(
                    `${top}:挂在 MIXED_TOP,但里面每个文件都命中 —— 它已是整体 native,该由 NATIVE_RE 覆盖`,
                );
            // 上面两条只管「还算不算混合」,拦不住**混合内部长出新的命中面**。命中集合逐条对拍
            // 才是那道门:多一条 = 有新东西进了构建没人登记;少一条 = 登记的那条没了(僵尸)。
            const want = MIXED_EXPECT[top];
            if (!want) {
                listRot.push(
                    `${top}:挂在 MIXED_TOP 却没在 MIXED_EXPECT 里钉命中集合 —— 混合条目必须逐条钉死,否则它是个豁免口`,
                );
            } else {
                const got = group.filter((f) => rx.test(f)).sort();
                const extra = got.filter((f) => !want.includes(f));
                const gone = want.filter((f) => !got.includes(f));
                for (const f of extra)
                    listRot.push(
                        `${top}:新长出命中 NATIVE_RE 的文件 ${f},但 MIXED_EXPECT 里没登记 —— 它进构建了,来登记一笔`,
                    );
                for (const f of gone)
                    listRot.push(
                        `${top}:MIXED_EXPECT 里登记了 ${f},但它现在不命中 NATIVE_RE(改名了?正则改窄了?)`,
                    );
            }
            continue;
        }
        if (inNon) {
            // 白名单是**判据**不是装饰:被列为 non-native 的目录一旦长出命中面,
            // 说明它进构建了,清单在替它挡着 —— 必须红。
            if (hits > 0)
                listRot.push(
                    `${top}:列在 NON_NATIVE_TOP,但里面有 ${hits} 个文件命中 NATIVE_RE —— 它已经进构建了`,
                );
            continue;
        }
        unclassified.push([top, group.length, hits]);
    }

    // 僵尸条目:清单里列着、仓库里已经没有的顶层条目。留着它 = 这行不再有任何判据兜着,
    // 而它长得和有效条目一模一样;更坏的是同名目录哪天以 native 身份回来,它当场替其挡住。
    // 清单里**重复一行**(新增 screenshots-* 时最容易贴两遍)或**同一条同时挂在两张单子上**,
    // 僵尸检查用 `present.has(t)` 两份副本都为真、过得去,`.length` 却多算一个 ⇒ 汇总行静默偏一;
    // 交叠更坏:`inMixed` 先 `continue`,NON_NATIVE 那道「长出命中面就红」一次都不跑(复审第 1 轮)。
    const dupTop = [...NON_NATIVE_TOP, ...MIXED_TOP].filter(
        (t, i, a) => a.indexOf(t) !== i,
    );
    if (dupTop.length)
        listRot.push(
            `清单里有重复或交叠条目(计数与断言都会失真): ${[...new Set(dupTop)].join(", ")}`,
        );

    // MIXED_EXPECT 是本卡新加的**第三张清单**,别让它成为唯一没有判据的那张(复审第 2 轮):
    // key 不在 MIXED_TOP 里 ⇒ `want` 永远读不到 ⇒ 打错字或 `.github/` 哪天挪走之后,
    // 那条**没有任何人再看**,却长得和有效条目一模一样。与下面对另外两张单子的规矩同源。
    for (const t of Object.keys(MIXED_EXPECT))
        if (!MIXED_TOP.includes(t))
            listRot.push(
                `${t}:在 MIXED_EXPECT 里钉了命中集合,却不在 MIXED_TOP 里 —— 没有任何人会读它(僵尸条目)`,
            );

    const present = new Set(byTop.keys());
    for (const t of [...NON_NATIVE_TOP, ...MIXED_TOP])
        if (!present.has(t))
            listRot.push(
                `${t}:清单里列着,但仓库里已经没有这个顶层条目(僵尸条目)`,
            );

    // ②b 的引擎对拍只喂了手写用例,而「JS 认、ERE 当字面量」这类分叉**恰恰在真实路径上
    // 才现形** —— 手写用例是照着当前正则编的,天然覆盖不到「正则改坏之后才分叉的那些路径」。
    // ④ 手上已经有全仓路径了,顺手再对拍一次,这一档就从「样本对拍」升级成「仓库现状对拍」。
    // (复审第 1 轮加;第 2 轮起 grep 缺席时这一档**也出声** —— 见下面的 else 分支。
    //  原先这里写的是「②b 已经报过降级,这里静默跳过不再重复刷屏」,那句已被下面那段推翻,
    //  留着两句相反的话等于让下一个人自己跑一遍才知道哪句算数。)
    const ereAll = ereHitSet(PATTERN, tracked);
    if (ereAll) {
        const jsAll = new Set(tracked.filter((f) => rx.test(f)));
        const onlyJs2 = [...jsAll].filter((f) => !ereAll.has(f));
        const onlyEre2 = [...ereAll].filter((f) => !jsAll.has(f));
        for (const f of onlyJs2)
            engineRot.push(`只有 JS 命中,CI 的 grep -E 会漏编: ${f}`);
        for (const f of onlyEre2)
            engineRot.push(`只有 grep -E 命中,本门禁判据失真: ${f}`);
        if (engineRot.length === 0)
            // 前缀自带归属,不用 `└`:这行打在 ④ 的汇总行**之前**,树形子项会挂到上一条
            // 打印(`③ 删除式验证`)上,而它其实是引擎对拍那一档的延伸(复审第 3 轮)。
            console.log(
                `②b(全仓路径): ${tracked.length} 条真实路径,JS 与 grep -E 命中集合一致。`,
            );
    } else {
        // **跳过要出声**:②b 的 WARN 原话是「跳过『JS ↔ grep -E 命中集合一致』这一档」,
        // 读者会理解成那 30 条手写用例,不会想到它还捎带把全仓真实路径**那一档**一起带走了。
        // 按 gates.ps1 自己的口径:降级过的一次不能和全跑过的一次长得一模一样(复审第 2 轮)。
        console.warn(
            "  [WARN] 本机没有 grep,「全仓路径引擎对拍」这一档也一并跳过了 —— " +
                "它与上面那条 WARN 是两档:那条是 30 条手写用例,这条是全仓真实路径。",
        );
    }

    // ⚠ 这一段必须落在下面那道汇总判负**之前**:它是往 `listRot` 里塞发现的,
    //    而 `listRot` 在那道判负里被消费掉。放到判负之后 = 塞进去的东西没有任何人再看,
    //    门禁照绿 —— 本卡初稿就是这么写的,复审第 1 轮的注入实测把它逮出来了。
    if (unclassified.length || listRot.length || engineRot.length) {
        // 抬头是最响、也是最先被读到的一行,它必须说对是哪一类问题(复审第 3 轮)。
        // 之前不管哪种红都打「顶层条目分类不全」:僵尸条目不是分类不全,引擎分叉更不是 ——
        // 人会照着抬头去翻 NON_NATIVE_TOP 找一条根本不存在的错行,正是这道门要消除的动作。
        // 三个独立 `if`,不是 `else if`(复审第 4 轮):这三类**可以共现** —— 改窄 NATIVE_RE
        // 的某条分支就会一起发生(MIXED_EXPECT 报 gone,同时两个引擎命中面分叉)。写成三选一时,
        // 抬头会打出「顶层条目一条不缺,是三张清单自己过期了」这种**排他断言**,而下面紧跟着
        // 还有引擎红 —— 人照抬头去改清单,改完还是红。
        if (unclassified.length) {
            console.error(
                "check-native-paths: **顶层条目分类不全** —— detect-native 靠 NATIVE_RE 决定子 PR 编不编,",
            );
            console.error(
                "  没被分类的顶层条目会**静默**落到 native=false 那一侧(改它不编译,CI 照样全绿)。",
            );
        }
        if (listRot.length) {
            console.error(
                "check-native-paths: **清单与仓库现状对不上** —— 这一类里顶层条目一条不缺,是三张清单自己过期了。",
            );
        }
        if (engineRot.length) {
            console.error(
                "check-native-paths: **两个引擎对 NATIVE_RE 的理解不一致** —— 这里(JS RegExp)与 CI(grep -E)命中面不同。",
            );
            console.error("  这一类与三张清单无关,别去翻 NON_NATIVE_TOP。");
        }
        for (const [t, n, h] of unclassified) {
            console.error(
                `  [FAIL] 未分类顶层条目: ${t}(${n} 个文件,${h} 个命中 NATIVE_RE)`,
            );
            // 出路有**三条**,别只说两条(复审第 1 轮):把天生混合的新目录一句「整体进
            // NATIVE_RE」打发掉,照做就是整棵子树进命中面 ⇒ 此后每个子 PR 白编七分钟。
            console.error(
                h === 0
                    ? "         **不进构建 ⇒ 加进本文件的 NON_NATIVE_TOP;进构建 ⇒ 加进 build-vst3.yml 的 NATIVE_RE。**"
                    : h === n
                      ? "         里面每个文件都命中 NATIVE_RE ⇒ **该整体进 NATIVE_RE**(顶层加一条分支)。"
                      : "         **它是混合的**:要么把命中的那几条挪进 NATIVE_RE,要么把这个顶层条目登记进 MIXED_TOP + MIXED_EXPECT。",
            );
        }
        for (const msg of listRot)
            console.error(`  [FAIL] 清单与仓库现状对不上: ${msg}`);
        for (const msg of engineRot)
            console.error(`  [FAIL] 两个引擎对同一条正则理解不一致: ${msg}`);
        if (engineRot.length)
            console.error(
                // ⚠ 这里必须是**双**反斜杠:普通 JS 字符串里 "\d" 是 `d`、"\b" 是 U+0008 BACKSPACE。
                //    第 2 轮抄 ②b 那句话时漏了一层,于是这条**讲转义陷阱的报错自己踩了转义陷阱** ——
                //    实测打出来是 「`d` / `s` / `^H`」。失败路径专属,没有任何机器会替你发现(复审第 3 轮)。
                "         典型成因:`\\d` / `\\s` / `\\b` 这类 JS 认、ERE 当字面量的转义。与三张清单无关。",
            );
        process.exit(1);
    }
    // **直接数,不用减法**(复审第 1 轮):减法把「三个数加起来一定等于总数」当不变量,
    // 而重复/交叠恰好会打破它 —— 于是汇总行会静默少报一条命中,正是本文件通篇在治的形态。
    const nativeTops = [...byTop.keys()].filter((t) => rx.test(t)).length;
    console.log(
        `④ 顶层条目全覆盖: ${byTop.size} 个顶层条目全部已分类` +
            `(${nativeTops} 命中 NATIVE_RE / ${NON_NATIVE_TOP.length} 显式 non-native / ` +
            `${MIXED_TOP.length} 混合)。`,
    );
}

console.log("check-native-paths 通过。");
