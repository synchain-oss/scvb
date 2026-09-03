// SPDX-License-Identifier: GPL-3.0-or-later
// check-gates-visibility.mjs —— 「浏览器在但没连上 ⇒ 必须显形」的**整条链**接线门禁
// (零依赖,Node >= 18,ESM)。
//
// [SL-297] 病灶:页面级冒烟在 CDP 握手失败时退 **2**,而 gates 3e 与 CI 都把 2 读成
//   「缺可选外部依赖」⇒ `[SKIP]` ⇒ **照算 PASS**。于是在一台**装着 Chrome** 的机器上,
//   一次瞬时超时就让整套判据**无声消失**,汇总行还写着全 PASS(SL-293 多轮 gates 撞到三次,
//   每次掉的套件还不一样,两套单独重跑都全绿 —— 丢的是运行机会,不是代码)。
//   修法是把它拆成 rc=3 + `[FLAKY-SKIP]`;而**那个修复自己也需要有人守着**。
//
// ⚠ 这条链有**三段**,少守一段就等于没守(复审【重要】点出第一版只守了第 ② 段):
//   ① **生产端**:六套 `smoke-*-page.mjs` 的两个「有依赖但失败」出口必须走 `browserFailed()`。
//      它是**六份手抄副本** —— 「只改五份」「照旧模板新起一套」都是本仓有前科的形态;
//      把任一份改回 `noBrowser(`,那一套就静默退回 `[SKIP]` ⇒ 照算 PASS,病灶原样复现。
//   ② **gates 端**:`gates.ps1` 3e 的 rc=3 分支要有**独立计数**且**计数拼进汇总标签**。
//      只断「文件里有 FLAKY-SKIP 字样」是没牙的:那行 `Write-Host` 留着、计数没进
//      `$smokeLabel`,**恰恰就是本卡要防的形态**(标记只在滚屏里闪一下,汇总照写全 PASS)。
//   ③ **CI 端**:`format.yml` 的 rc=3 分支要在、且**不判红**。这一段的失效方向与 ② 相反:
//      删了它 flake 会落进 `[ $rc -ne 0 ]` ⇒ `::error::` + fail=1 ⇒ 在 required check 上
//      把瞬时抖动变成**硬红**,与本卡「不判红」的决定相反。那个方向是**响的**(CI 立刻红),
//      不像 ② 是**哑的**;但既然一并守得住,就不留这个不对称给后人猜。
//
// ⚠ 边界(照实说,别让人以为它更严):本检查是**文本级**的,不解析 PowerShell / YAML / JS。
//   · PowerShell 侧**先剥掉整行注释**再匹配 —— 否则一行 `# TODO: $smokeLabel 拼上 $smokeFlaky`
//     就能顶替真接线(复审指出的**漏判**口子,不是假红口子,已堵);
//   · 但**行内**尾注释(`$x = 1  # …$smokeFlaky…`)仍不剥,那一档依旧可被顶替;
//   · 变量改名 / 标签改用别的拼接写法(`-join`)会**假红**。假红逼人回来读这段注释,
//     漏判才会让洞悄悄回来 —— 所以这两类的方向是**不对称的**,别笼统说「方向安全」。
//   · 它**不验证运行时真打出了那行**;那由 SL-297 的删除式实跑覆盖(注入 CDP 握手失败 ⇒
//     汇总必须出现计数)。本检查只保证**接线还在**。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const errors = [];
const fail = (m) => errors.push(m);
const read = (p) => fs.readFileSync(p, "utf8");

// PowerShell 整行注释:行首(允许缩进)是 `#`。剥掉它们再匹配。
const stripPsComments = (text) =>
    text
        .split("\n")
        .filter((l) => !/^\s*#/.test(l))
        .join("\n");

// ---------------------------------------------------------------- ① 生产端
// 执行面按 **glob** 收,与 gate 3e / CI 同口径(`smoke-*.mjs`)——
// 加一套新的页面级冒烟会被自动守住,不必回来改这里。
// 「是页面级冒烟」的判据 = 它自己起浏览器(有 `chrome.on("error"`),
// 而不是靠文件名里有没有 `-page`:后者是命名约定,前者是事实。
const TESTS = path.join(ROOT, "web-preview", "tests");
const pageSuites = fs.existsSync(TESTS)
    ? fs
          .readdirSync(TESTS)
          .filter((f) => f.startsWith("smoke-") && f.endsWith(".mjs"))
          .map((f) => ({ name: f, text: read(path.join(TESTS, f)) }))
          .filter((f) => f.text.includes('chrome.on("error"'))
    : [];

if (pageSuites.length === 0)
    fail(
        "web-preview/tests 下没找到任何起浏览器的 smoke-*.mjs —— 执行面收空了," +
            "本检查会退化成永远通过(参照 sccache「静默零命中照样绿」那一族)。",
    );

for (const s of pageSuites) {
    if (!/function\s+browserFailed\s*\(/.test(s.text))
        fail(
            `${s.name}:没有 browserFailed() —— 「浏览器在但没起来」会退回缺依赖那一档`,
        );
    // spawn 失败
    const spawnLine = s.text
        .split("\n")
        .find((l) => l.includes('chrome.on("error"'));
    if (spawnLine && !spawnLine.includes("browserFailed"))
        fail(
            `${s.name}:spawn 失败(chrome.on("error"))没走 browserFailed —— ` +
                `能走到 spawn 就说明二进制在,那是失败不是缺依赖`,
        );
    // CDP 握手超时:`if (!targets)` 之后的那一小段里必须调 browserFailed
    const i = s.text.indexOf("if (!targets)");
    if (i < 0) {
        fail(
            `${s.name}:找不到 CDP 握手的 \`if (!targets)\` 分支(判据锚点变了,回来同步本检查)`,
        );
    } else {
        const seg = s.text.slice(i, i + 1400);
        if (!seg.includes("browserFailed"))
            fail(
                `${s.name}:CDP 握手超时没走 browserFailed —— 会静默退回 [SKIP] 且照算 PASS`,
            );
    }
    // 反向:真缺依赖那两处必须**仍然**是 noBrowser(两类不能又被合并回去)
    if (!/noBrowser\("本机找不到 Chrome\/Edge"\)/.test(s.text))
        fail(
            `${s.name}:「本机找不到 Chrome/Edge」不再走 noBrowser —— 真缺依赖被误升成失败档`,
        );
}

// ---------------------------------------------------------------- ② gates 端
const GATES = path.join(ROOT, "scripts", "gates.ps1");
if (!fs.existsSync(GATES)) fail("找不到 scripts/gates.ps1");
else {
    const ps = stripPsComments(read(GATES));
    if (!/\$rc\s+-eq\s+3\b/.test(ps))
        fail(
            "gates.ps1 Gate 3e 没有 `$rc -eq 3` 分支 —— 浏览器在但没连上会被当成缺依赖",
        );
    if (!/\[FLAKY-SKIP\]/.test(ps))
        fail(
            "gates.ps1 里没有 `[FLAKY-SKIP]` 标记 —— 「没跑成」会和「跳过」长得一样",
        );
    if (!/\$smokeFlaky\s*\+\+/.test(ps))
        fail("`$smokeFlaky++` 不在 —— rc=3 没有独立计数");
    if (!/\$smokeFlaky\s*=\s*0/.test(ps)) fail("`$smokeFlaky` 没有初始化");
    // ★ 本检查的核心:计数要**接进汇总标签**。上面全绿而这条红 = 打了标记却没进摘要。
    if (!/-f\s+\$smokeLabel\s*,\s*\$smokeFlaky/.test(ps))
        fail(
            "`$smokeFlaky` 没有被拼进 `$smokeLabel` —— [FLAKY-SKIP] 只会出现在滚屏里," +
                "而跑完 gates 的人看的是汇总表:这正是 SL-297 要堵的那个洞原样复现。",
        );
    // 反向:rc=2 那一档要还在,且自增的仍是 $smokeSkipped(两类不能共用一个计数器)
    if (!/\$rc\s+-eq\s+2\b/.test(ps))
        fail("gates.ps1 丢了 `$rc -eq 2` 分支(真缺依赖那一档)");
    if (!/\$smokeSkipped\s*\+\+/.test(ps))
        fail(
            "`$smokeSkipped++` 不在 —— rc=2 与 rc=3 共用计数器就等于两类又合并了",
        );
}

// ---------------------------------------------------------------- ③ CI 端
const WF = path.join(ROOT, ".github", "workflows", "format.yml");
if (!fs.existsSync(WF)) fail("找不到 .github/workflows/format.yml");
else {
    const yml = read(WF);
    const m =
        /elif\s+\[\s*\$rc\s+-eq\s+3\s*\]\s*;\s*then([\s\S]{0,600}?)(?:elif|fi)\b/.exec(
            yml,
        );
    if (!m)
        fail(
            "format.yml 的 web-smoke 没有 `elif [ $rc -eq 3 ]` 分支 —— rc=3 会落进 " +
                "`[ $rc -ne 0 ]` ⇒ ::error:: + fail=1,在 required check 上把瞬时抖动变成硬红",
        );
    else {
        if (!/::warning/.test(m[1]))
            fail(
                "format.yml 的 rc=3 分支没有 ::warning:: —— 降级在 checks 摘要里看不见",
            );
        if (/::error/.test(m[1]) || /fail=1/.test(m[1]))
            fail(
                "format.yml 的 rc=3 分支判红了(::error 或 fail=1)—— 与本卡「不判红」的决定相反;" +
                    "要改成判红须先有重试/退避(SL-297 第二步),并同步 gates 侧与本检查",
            );
    }
}

if (errors.length) {
    console.error("check-gates-visibility 失败(" + errors.length + " 项):");
    for (const e of errors) console.error("  [FAIL] " + e);
    console.error(
        "  真源 = smoke-*.mjs 的 browserFailed 出口 / gates.ps1 Gate 3e / format.yml web-smoke;口径见本文件头注与 SL-297。",
    );
    process.exit(1);
}
console.log(
    "check-gates-visibility 通过:生产端 " +
        pageSuites.length +
        " 套两个失败出口都走 browserFailed;gates 3e 的 rc=2/rc=3 两档俱在且 FLAKY 计数已接进汇总标签;CI 侧 rc=3 分支在且不判红。",
);
process.exit(0);
