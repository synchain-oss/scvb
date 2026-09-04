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
// ⚠ 这条链有**三段**,少守一段就等于没守(复审【重要】点出第一版只守了第 ② 段);
//   ② 段里另挂了一族**同构**的接线:gate 3i 的标记 —— [SL-315] 起是 ALLOW 放行、
//   [SL-318] 起并上 WARN 降级。同一个失效方向(打了标记只在滚屏闪一行、汇总表照写
//   干净的 PASS),所以判据也逐字同构。那一族**连同守着它的那道散文守卫**单列在下面 §④。
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
// §④ **gate 3i 标记族的散文守卫**(挂在 ② 段里,判据与边界都以本节为准):
//   gate 3i 把这一圈脚本的成功输出按标记回显 ALLOW / BASE,又按行首标记把 WARN 数进
//   汇总标签。于是一句**带方括号**的散文(「见 [ 标记 ] 行」)就能冒充一条真信号 ——
//   轻则回显多一行,重则(散文恰好落在行首那一档)汇总表当场多报一档降级。
//   这条不变量在只有散文守着的时候被违反了**三次**(哪三次写在判据旁边,不在这里重复一遍 ——
//   同一份事实抄两处,改一处就是下一条假注释),所以给它一个跨文件的执行者。
//   · **执行面**从 `gates.ps1` gate 3i 的 `foreach` 里读,不手抄(读空了硬失败);
//   · **判据(第 ④ 版)**:在剥掉整行注释的**整份源码**上逐个找方括号标记,往回跳过空白,
//     **前一个字符是引号才算真信号**,其余判红。既不切字符串,也不看 `console.log` 的实参。
//   · **允许的引号只有 `"` 与 `'`**([SL-318] 去掉了反引号):markdown 反引号正是本仓散文
//     引用一个标记的惯例,留着它等于给整类散文盖章。反向代价为零 —— 今天七个脚本的九处
//     真信号全部是双引号起头。
//   · 前三版怎么错的(**四版是同一个洞逐层下降**,写在这里是为了别有人退回去):
//       ① 只扫 `console.log(...)` 的实参 ⇒ 被违反的那句提成了 `OK_MSG` 常量,注入实测照样绿;
//       ② 只扫双引号 ⇒ `check-native-paths` 的成功行本来就是模板串,旁边补一句就漏;
//       ③ 先用正则把「字符串字面量」切出来再扫 ⇒ 三种引号互相嵌套时**错配**,报到一段横跨
//          代码的假字面量上,**假红**;
//       ④ 改成「往回看一眼引号」⇒ 不再依赖 tokenize,但**允许集**成了新的判据面(见上一条)。
//   · **已知边界**:拼装写法(`"[" + "ALLOW] 行"`)天然不命中 —— 源码里根本没有连着的标记。
//     这是**夹具**的写法:本文件的反例夹具必须逐字带上那个标记,只能这么绕。
//     普通散文不必这么写,**去掉方括号**就行(报错话术给的也是这一条);
//     行内尾注释没剥(与下面那张表同一口径),注释里带方括号标记**会判红** —— 这一档偏严,
//     方向是假红不是漏判,回来读这一节就知道该怎么改。
//
// ⚠ 边界(照实说,别让人以为它更严):本检查是**文本级**的,不解析 PowerShell / YAML / JS。
//   · PowerShell 侧**先剥掉整行注释**再匹配 —— 否则一行 `# TODO: $smokeLabel 拼上 $smokeFlaky`
//     就能顶替真接线(复审指出的**漏判**口子,不是假红口子,已堵);
//   · ③ 段的 YAML/bash 侧**同样先剥整行注释**(与 ② 段对称,复审指出的漏判口子已堵);
//   · ① 段的 JS 侧**同样先剥整行注释**,且连块注释(`/*` ` * ` `*/`)一起收 ——
//     六套里 JSDoc 是常规写法,只剥 `//` 会让同一个哑弹换个注释符号回来;
//     **判定一律用剥过的那份**,连「是不是页面级冒烟」的执行面判据也是;
//   · §④ 的脚本扫描**同样先剥整行注释**(`//` 与块注释三形态)—— 与 ① 段共用
//     `stripJsComments`,[SL-318] 把此前两份逐字相同的拷贝合成了一份;
//   · **四处的行内尾注释都没剥**(`$x = 1  # …` / `foo(); // …`)—— ①②③ 三段里那一档同样
//     可被顶替,§④ 那一处方向相反(尾注释里写标记会**判红**,是偏严不是漏判)。
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

// JS 整行注释:`//` **与**块注释的三种行形态(`/*` 开头、` * ` 中间、`*/` 收尾)。
// ① 段与 §④ 用的是**同一份**过滤器 —— 此前是两份逐字相同的拷贝([SL-318] 合一),
// 两份拷贝的坏处不是重复,是**只改一份**:那正是本文件 ① 段自己写的「六份手抄副本」那一族。
const stripJsComments = (text) =>
    text
        .split("\n")
        .filter((l) => !/^\s*(\/\/|\/?\*)/.test(l))
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
              // 块注释的三种行形态也一并收:六套里 JSDoc 是常规写法
              // (`smoke-monitor-page.mjs` 一个文件 5 处),只剥 `//` 的话,下一个人在
              // `if (!targets)` 附近补一句 `/** …走 browserFailed()… */`,
              // ① 段就**原样退回哑弹**,只是注释符号换了(复审指出)。
              const code = stripJsComments(text);
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
    // spawn 失败。**不按「行」找**:逐行 `find` 等于把判据钉在 prettier 的换行决定上 ——
    // 这两行都贴着 printWidth 80 的上限、余量只有几列(具体几列取决于怎么算 CJK 宽度,
    // 别把某个具体数字写死在这里 —— 本卡已经因为「拿 awk 的字符数当 prettier 的显示宽」
    // 把错数写进过 commit;结论不依赖那个数:余量小到一次改名就会被折行),
    // 一旦折成两行,`find` 到的那一行里就没有 `browserFailed`,判据**假红**且报
    // 「没走 browserFailed」,把人指到不存在的问题上。
    // 与下面 `--chrome` 那条同款,改用带上界的跨行匹配。
    if (!/chrome\.on\("error"[\s\S]{0,200}?browserFailed\(/.test(s.code))
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
    // `\s*` 而不是一个 literal 空格:这一行贴着 printWidth 80 的上限、**余量只有几列**
    //(见上面 spawn 那段:别在注释里写死一个具体列数)—— 任何一次改名都会让 prettier
    // 把它折行,而写死空格的正则会当场**假红**、
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

    // ---- [SL-315] gate 3i 的 `[ALLOW]` 放行:与上面 3e 那条**逐字同构** ----------
    // 同一族、同一个失效方向:打了标记、只在滚屏里闪一行,而汇总表照写干净的 PASS ——
    // 于是「有一条烂在注释块里的豁免」与「没有任何豁免」在本地长得一模一样。
    // SL-295 立的规矩是「豁免不显形就等于没有豁免纪律」,那条规矩自己也需要有人守着。
    if (!/\$draftsAllow\s*=\s*0/.test(ps))
        fail("`$draftsAllow` 没有初始化 —— gate 3i 的 ALLOW 放行没有独立计数");
    if (!/\$draftsAllow\s*\+=/.test(ps))
        fail("`$draftsAllow` 没有自增 —— ALLOW 行没有被数进去");
    // ★ 与 3e 那条同一个核心:计数要**接进汇总标签**。上面全绿而这条红 = 数了却没进摘要。
    if (!/-f\s+\$parityLabel\s*,\s*\$draftsAllow/.test(ps))
        fail(
            "`$draftsAllow` 没有被拼进 `$parityLabel` —— ALLOW 行只会出现在滚屏里," +
                "而跑完 gates 的人看的是汇总表:与 SL-297 在 3e 上堵的是同一个洞。",
        );
    // ★ 本卡特有的第二条:计数模式要**对真放行行命中、对成功消息不命中**。
    //   这里断的是**行为**,不是拼写:从 gates.ps1 里把那条正则**抽出来实跑正反例**。
    //   拼写式断言(「源码里有没有写着这几个字符」)在这一条上是没牙的 ——
    //   两个坏写法都会被它放过一个方向:
    //     · 裸 `\[ALLOW\]`     ⇒ 把成功消息里提到的那句也数进去,**0 条放行恒报 1**;
    //     · `^\[ALLOW\]`(不吃前导空格)⇒ 真放行行是 `"  [ALLOW] #…"`(两个前导空格),
    //       **有放行也恒报 0**。两个方向一样坏,而「恒 0」那种连删除式都照不出来(0 == 0)。
    //   PS 与 JS 在 `^ \s * \[ \]` 这几个元字符上同义,可以直接用 JS 引擎代跑。
    //   好处还有一条:不再逐字耦合到某一种写法 —— 谁换成等价实现,只要行为对就不会假红。
    const cnt = /\$draftsAllow\s*\+=[^\n]*-match\s*'([^']+)'/.exec(ps);
    if (!cnt)
        fail(
            "`$draftsAllow` 的计数行里读不到 `-match '<正则>'` —— 无法验证它到底数不数得对",
        );
    else {
        const re = new RegExp(cnt[1]);
        // 正例:真放行行带**两个前导空格**(check-changelog-drafts.mjs 的 `"  [ALLOW] #"`)。
        if (!re.test("  [ALLOW] #189 放行 —— 理由"))
            fail(
                "`$draftsAllow` 的计数正则 /" +
                    cnt[1] +
                    "/ 数不到真放行行(带两个前导空格)—— 有放行也恒报 0",
            );
        // 反例:**带方括号的散文提法**(下面那条守卫禁的正是它)。用它当反例才有区分度 ——
        // 拿今天这句不带方括号的成功消息去试,裸 `\[ALLOW\]` 也不会命中,这一格就没牙了。
        // 夹具**拼装**而不写成整串:整串会让下面那条「散文里不许出现方括号标记」的守卫
        // 扫到本文件自己,入库即自炸(本仓「扫描器入库才炸」那一族的老判例)。
        if (re.test("…放行的号见 [" + "ALLOW] 行;…"))
            fail(
                "`$draftsAllow` 的计数正则 /" +
                    cnt[1] +
                    "/ 把成功消息也数进去了 —— 0 条放行恒报 1",
            );
    }

    // ---- [SL-318] gate 3i 的 WARN 降级计数:与上面 ALLOW 那条**逐字同构**,风险高一档 ----
    // 同构在:都是「脚本成功路径打出来的标记行 → 数出来 → 拼进 $parityLabel」。
    // 高一档在:ALLOW 的裸匹配只多回显一行,而 WARN 的那个数**直接就是汇总表里的降级档数** ——
    // 任一脚本在成功散文里提一句带方括号的 WARN,汇总表当场多报一档降级,而 gates.ps1 自己
    // 写着「喊错一次,下次真降级也会被当噪声」。所以这一族三个标记要一份判据、一个口径。
    if (!/\$parityWarn\s*=\s*0/.test(ps))
        fail("`$parityWarn` 没有初始化 —— gate 3i 的 WARN 降级没有独立计数");
    if (!/\$parityWarn\s*\+=/.test(ps))
        fail("`$parityWarn` 没有自增 —— 降级行没有被数进去");
    // ★ 与 3e / ALLOW 两条同一个核心:计数要**接进汇总标签**。
    if (!/-f\s+\$parityWarn\b/.test(ps))
        fail(
            "`$parityWarn` 没有被拼进 `$parityLabel` —— 降级只会出现在滚屏里," +
                "而跑完 gates 的人看的是汇总表:与 SL-297 在 3e 上堵的是同一个洞。",
        );
    // ★ 同 ALLOW 那条:断的是**行为**,把 gates.ps1 里那条正则抽出来实跑正反例。
    const wcnt = /\$warnLines\s*=[^\n]*-match\s*'([^']+)'/.exec(ps);
    if (!wcnt)
        fail(
            "`$warnLines` 的匹配行里读不到 `-match '<正则>'` —— 无法验证它到底数不数得对",
        );
    else {
        const wre = new RegExp(wcnt[1]);
        // 正例:真降级行带**两个前导空格**(check-native-paths / check-bridge-parity 的写法)。
        if (!wre.test("  [WARN] 本机没有 grep,跳过这一档。"))
            fail(
                "`$warnLines` 的匹配正则 /" +
                    wcnt[1] +
                    "/ 数不到真降级行(带两个前导空格)—— 有降级也恒报 0,而恒 0 连删除式都照不出来",
            );
        // 反例:**带方括号的散文提法**(下面那条守卫禁的正是它)。夹具同样**拼装**,
        // 免得守卫扫到本文件自己(「扫描器入库才炸」那一族)。
        if (wre.test("① 手写用例全过;另见 [" + "WARN] 那一档…"))
            fail(
                "`$warnLines` 的匹配正则 /" +
                    wcnt[1] +
                    "/ 把成功消息里的散文也数进去了 —— 没有降级也会在汇总表里报出一档",
            );
    }

    // ★ 第三条:**成功路径打出来的话里不许出现方括号标记**。gate 3i 对这一圈脚本的
    //   成功输出按裸标记回显 ALLOW 与 BASE,又按行首标记把 WARN 数进汇总标签 —— 所以任何
    //   一句「见 ALLOW 行」这样**带方括号**的散文都会冒充一条真信号:轻则回显多一行,
    //   重则(那句散文恰好落在行首那一档)汇总表当场多报一档降级。
    //   这条不变量到 [SL-315] 为止已被违反**三次**(#197 第 5 轮那次是 BASE、`379de23`
    //   那次是 ALLOW、以及本卡第一版**在本文件自己身上**),三次都只有散文守着 ——
    //   所以给它一个跨文件的执行者,而不是每个脚本各写一份手抄自测。
    //   **判据的第 ④ 版**:扫的是「剥掉整行注释后的整份源码」,既不切字符串、也不看
    //   `console.log(...)` 的实参;逐个标记往回跳过空白,**前一个字符是引号**才算真信号。
    //   前三版怎么错的、这版的已知边界在哪,写在**头注的边界表**里(§④ 那一节),
    //   别只读这里 —— 那张表才是这道守卫的口径真源。
    //   [SL-318] 允许的引号是 `"` 与 `'` **两种**,反引号**不在其中**:markdown 反引号
    //   正是本仓散文引用一个标记的惯例(这个文件的注释里就有一堆),把它留在允许集里
    //   等于给整类散文盖章 —— 前三版是**扫不到**那类写法,第 ④ 版扫到了却**放行**,
    //   比扫不到更坏。反向代价为零:今天七个脚本九处真信号**全部**是双引号起头的
    //   `"  [标记] …"`,没有一处用模板串起头。
    //   清单**从 gates.ps1 的 foreach 里读出来**,不手抄 —— 手抄的话新加一个脚本就漏守,
    //   而这正是本仓「照模板复制一份就漏掉」那一族的形态。
    const listM = /foreach\s*\(\s*\$sc\s+in\s+@\(([^)]*)\)\s*\)/.exec(ps);
    const gate3iScripts = listM
        ? [...listM[1].matchAll(/'([^']+\.mjs)'/g)].map((m) => m[1])
        : [];
    if (gate3iScripts.length === 0)
        fail(
            "从 gates.ps1 的 gate 3i `foreach` 里读不出脚本清单 —— " +
                "下面那条「成功消息不许带方括号标记」会退化成永远通过",
        );
    for (const sc of gate3iScripts) {
        const f = path.join(ROOT, "scripts", sc);
        if (!fs.existsSync(f)) {
            fail(`gate 3i 的清单里有 ${sc},但 scripts/ 下找不到它`);
            continue;
        }
        const src = stripJsComments(read(f));
        // 判据本身与它的边界都在头注 §④,这里只留「读代码时会想问的那一句」。
        // 三个标记**同一份判据**([SL-318]):gate 3i 对 ALLOW / BASE / WARN 的处理各不相同
        // (前两个只回显,WARN 还要计数进标签),但「散文冒充信号」这个失效方向完全一样。
        for (const m0 of src.matchAll(/\[(?:ALLOW|BASE|WARN)\]/g)) {
            const before = src.slice(0, m0.index).replace(/[ \t]+$/, "");
            // 允许集**只有两种引号**,反引号不在内(理由见上面 [SL-318] 那段与头注 §④)。
            if (/["']$/.test(before)) continue; // 开头即标记 = 真信号
            fail(
                `${sc}:源码里有一句散文提到了方括号标记` +
                    `(「${src.slice(Math.max(0, m0.index - 12), m0.index + 9).replace(/\n/g, " ")}」)—— ` +
                    `gate 3i 按标记回显/计数成功输出,这句会冒充一条真信号;` +
                    `提到 ALLOW / BASE / WARN 时去掉方括号(要写成真信号,就让它紧跟在 " 或 ' 后面开一条字符串)`,
            );
        }
    }
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
        " 套两个失败出口都走 browserFailed;gates 3e 的 rc=2/rc=3 两档俱在且 FLAKY 计数已接进汇总标签;" +
        "gates 3i 的 ALLOW 放行与 WARN 降级各有独立计数、两个计数都锚在行首、且都已接进汇总标签;" +
        "三个标记同一道散文守卫(允许集只有两种引号,反引号不在内);CI 侧 rc=3 分支在且不判红。",
);
process.exit(0);
