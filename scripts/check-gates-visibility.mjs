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
//   · ③ 段的 YAML/bash 侧**同样先剥整行注释**(与 ② 段对称,复审指出的漏判口子已堵);
//   · ① 段的 JS 侧**同样先剥整行注释**,且连块注释(`/*` ` * ` `*/`)一起收 ——
//     六套里 JSDoc 是常规写法,只剥 `//` 会让同一个哑弹换个注释符号回来;
//     **判定一律用剥过的那份**,连「是不是页面级冒烟」的执行面判据也是;
//   · **三侧的行内尾注释都没剥**(`$x = 1  # …` / `foo(); // …`),那一档三侧同样可被顶替 ——
//     要堵得上 AST,对一道防「照模板复制」的门禁不划算;写在这里是为了别让人高估它;
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
          .map((f) => {
              const text = read(path.join(TESTS, f));
              // [SL-297 复审] **判定要用剥掉整行注释的那一份**。第一版直接在原文上
              // `includes("browserFailed")`,而我自己在 `smoke-monitor-page.mjs` 的
              // `if (!targets)` 上方写了一大段解释、里面就有 `browserFailed()` 字样 ——
              // 于是那条断言在**最该守的那个文件**上变成了哑弹:把真调用改回
              // `noBrowser(` 它照样绿。这是本卡第三次撞上同一形态(② PowerShell、
              // ③ YAML、这里 JS),三处口径统一:**整行注释一律先剥**。
              // 只剥**整行**注释,不碰行内 `//` —— 后者会把 `http://127.0.0.1` 这类串
              // 拦腰截断(`check-smoke-hygiene` 对同一个坑有明说)。
              const code = text
                  .split("\n")
                  // 收 `//` **与** 块注释的三种行形态(`/*` 开头、` * ` 中间、`*/` 收尾)——
                  // 复审指出:六套里 JSDoc 是常规写法(`smoke-monitor-page.mjs` 一个文件 5 处),
                  // 只剥 `//` 的话,下一个人在 `if (!targets)` 附近补一句
                  // `/** …走 browserFailed()… */`,① 段就**原样退回哑弹**,只是注释符号换了。
                  .filter((l) => !/^\s*(\/\/|\/?\*)/.test(l))
                  .join("\n");
              return { name: f, code };
          })
          // 用剥过注释的 `code` 判定「是不是页面级冒烟」:注释里提一句 `chrome.on("error")`
          // 不应该把一个文件收进执行面。
          .filter((f) => f.code.includes('chrome.on("error"'))
    : [];

if (pageSuites.length === 0)
    fail(
        "web-preview/tests 下没找到任何起浏览器的 smoke-*.mjs —— 执行面收空了," +
            "本检查会退化成永远通过(参照 sccache「静默零命中照样绿」那一族)。",
    );

for (const s of pageSuites) {
    if (!/function\s+browserFailed\s*\(/.test(s.code))
        fail(
            `${s.name}:没有 browserFailed() —— 「浏览器在但没起来」会退回缺依赖那一档`,
        );
    // ★ 断的是**退出码**,不是名字。名字留着而 `process.exit(3)` 被改回 2,
    //   上面那条与 ②③ 两段**全绿**,而那一套的 CDP 超时又回到 `[SKIP]` ⇒ 照算 PASS ——
    //   两类当场合并,病灶原样复现,读代码的人却因为 `browserFailed` 这个名字还在而更确信。
    //   这正是本文件 ② 段自己写过的那句「只断『文件里有这个字样』是**没牙的**」。
    if (
        !/function\s+browserFailed\s*\([\s\S]{0,900}?process\.exit\(3\)/.test(
            s.code,
        )
    )
        fail(
            `${s.name}:browserFailed() 退的不是 3(或函数体长过匹配窗口)—— ` +
                `退 2 就等于把「浏览器在但没连上」又并回「缺依赖」,而名字还在、其余断言全绿`,
        );
    // spawn 失败
    const spawnLine = s.code
        .split("\n")
        .find((l) => l.includes('chrome.on("error"'));
    if (spawnLine && !spawnLine.includes("browserFailed"))
        fail(
            `${s.name}:spawn 失败(chrome.on("error"))没走 browserFailed —— ` +
                `能走到 spawn 就说明二进制在,那是失败不是缺依赖`,
        );
    // CDP 握手超时:`if (!targets)` 之后的那一小段里必须调 browserFailed
    const i = s.code.indexOf("if (!targets)");
    if (i < 0) {
        fail(
            `${s.name}:找不到 CDP 握手的 \`if (!targets)\` 分支(判据锚点变了,回来同步本检查)`,
        );
    } else {
        // 窗口同 ③ 段放宽到 2000(剥注释后这一段本就短得多,余量是给将来加代码用的)。
        const seg = s.code.slice(i, i + 2000);
        if (!seg.includes("browserFailed"))
            fail(
                `${s.name}:CDP 握手超时没走 browserFailed(**或该分支长过了 2000 字符的匹配窗口** —— 先确认是哪一种)—— 会静默退回 [SKIP] 且照算 PASS`,
            );
    }
    // 反向:真缺依赖那两处必须**仍然**是 noBrowser(两类不能又被合并回去)。
    // 两个调用点都要断 —— 上一版只断了一个,而 PR 描述里「反向:rc=2 原样」的实跑
    // 在机器上的唯一对应物就是这几条。
    if (!/noBrowser\("本机找不到 Chrome\/Edge"\)/.test(s.code))
        fail(
            `${s.name}:「本机找不到 Chrome/Edge」不再走 noBrowser —— 真缺依赖被误升成失败档`,
        );
    // `\s*` 而不是一个literal空格:这一行在六套里**恰好是 80 列**(prettier 的 printWidth),
    // 余量为零 —— 任何一次改名都会让 prettier 把它折行,而写死空格的正则会当场**假红**、
    // 还报成「不再走 noBrowser」把人指到不存在的问题上。判据不该钉排版决定。
    if (!/if \(!existsSync\(p\)\)\s*noBrowser\(/.test(s.code))
        fail(
            `${s.name}:\`--chrome\` 路径不存在那处不再走 noBrowser —— 那是真缺依赖,不是「浏览器在但没连上」`,
        );
    // ★ 与上面 browserFailed 那条**同款**:断的是**退出码**,不是名字。
    //   把 `noBrowser()` 退成 3,上面两条与 ②③ 两段全绿,而一台**真没装浏览器**的机器
    //   会在汇总里被写成「浏览器在但没连上」—— 逐字是假话,本卡为两类分家立的那条界线
    //   当场被抹掉。坏的不是判定(两档都不判红),是**摘要说了假话** ——
    //   而「摘要不能说假话」正是本卡的全部主题。
    if (
        !/function\s+noBrowser\s*\([\s\S]{0,900}?process\.exit\(2\)/.test(
            s.code,
        )
    )
        fail(
            `${s.name}:noBrowser() 退的不是 2(或函数体长过匹配窗口)—— ` +
                `退 3 会让「本机没装浏览器」在汇总里显示成「浏览器在但没连上」`,
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
    // 与 ② 段**对称**地先剥掉整行注释(复审指出:不剥的话,rc=3 分支里一行
    // `# … ::warning …` 注释就能顶替真 `echo`,那是**漏判**)。YAML/bash 的整行注释同为 `#`。
    const yml = read(WF)
        .split("\n")
        .filter((l) => !/^\s*#/.test(l))
        .join("\n");
    // 窗口取 2000 而不是 600:rc=3 分支当前约 330 字符,而**这类分支最容易长大的正是中文注释**;
    // 超窗时 `m` 为 null,报出来的却是「没有 rc=3 分支」—— 那个分支明明在,会把人指到一个
    // 不存在的问题上。所以既放宽窗口,也在失败文案里点明「或超出匹配窗口」这一可能。
    const m =
        /elif\s+\[\s*\$rc\s+-eq\s+3\s*\]\s*;\s*then([\s\S]{0,2000}?)(?:elif|fi)\b/.exec(
            yml,
        );
    if (!m)
        fail(
            "format.yml 的 web-smoke 没有 `elif [ $rc -eq 3 ]` 分支(**或该分支长过了 2000 字符的匹配窗口** —— 先确认是哪一种)—— rc=3 会落进 " +
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
