// SPDX-License-Identifier: GPL-3.0-or-later
// check-preview-messages.mjs —— 「`shot.mjs` 的失败文案 ⇄ PREVIEW-GUIDE §5 排障表」接线门禁
// (零依赖,Node >= 18,ESM)。
//
// [SL-335] 病灶(同一张卡里连栽三次,三次都由复审发现):`web-preview/shot.mjs` 没有机器
//   调用方,#194 因此立过「**文档是这条约定唯一的出口**」——退出码与失败文案只对人生效。
//   而人的用法是**拿报错原文去 §5 排障表里查**。于是「改了实现却没改出口」这一族就成了
//   反复复发的形态,而且越往后越隐蔽:
//     ① 新增一种失败形态,表里没有它       —— 最好发现;
//     ② 改了**判据前提**(谁触发),表里那句话还在、说的却已不是实现;
//     ③ 改了**报错字符串**本身(语义没变),表按原文逐字查,变一个词就查不到。
//   ③ 正是本卡的来处:`重新注入 __D…失败` 是新加的文案,而表里只有 `注入 __D…失败`,
//   撞上的人复制报错去搜,命中 0。
//
// 判据:`shot.mjs` 里每一条**用户可见失败文案**,其**开头那一段**都要能在 §5 里查到。
//   · 「开头那一段」= 归一化后的前 HEAD_CHARS 个字面字符(插值 `${…}` 当通配,
//     因为表里本来就用 `…` 占位)。只钉开头,不钉全文:表不该抄整条消息,
//     但**人复制的就是开头那一截**;
//   · 两侧都**归一化**:去掉 markdown 的反引号与星号、把连续空白折成一个空格。
//     不归一化的话,表里写 `` `.pv-status` `` 而消息里是裸的 `.pv-status`,会假红;
//   · 引号也算字面:表里写 `『未就绪』` 而消息里是 `「未就绪」` —— 那是**真的**查不到,
//     所以**不**做引号归一,让它判红(本卡第一次跑就照出了这一处)。
//
// ⚠ 边界(照实说,别让人以为它更严):
//   · 本检查是**文本级**的,不解析 JS;失败文案的执行面靠两条正则收
//     (`throw new (Navigation)?Error(` 的首个实参、`evalLanded(` 的 `what` 实参)。
//     换一种抛法(先把消息存进变量再 throw)就收不到 —— 那会**漏判**,不会假红;
//     收空/收得过少时硬失败(见 MIN_MESSAGES),这是本仓「执行面被改空了照样绿」那一族的兜底;
//   · 它只保证「表里查得到这句话」,**不保证那一行的处置是对的**。处置写反过一次
//     (#220 第 4 轮),那种错只有人能看出来;
//   · 它不管退出码分档对不对 —— 那是 `shot.mjs` 自己的事;
//   · 剥注释走**逐字符扫描器** `lib/strip-comments.mjs`(与 check-gates-visibility 同一份)。
//     [SL-330b] 之前这里是行过滤器,复审第 1 轮记过它带来的两个方向,**两个都随这次换实现关掉了**,
//     留在这里是因为它们说明了「为什么剥法本身也算判据面」:
//       — **假红**:行尾注释当年没剥,`const x = 1; // 旧写法是 throw new Error("…")` 会被当成
//         实现里的一条文案收走。`shot.mjs` 注释密度高、正文里就在大段引用旧文案,
//         今天那些恰好没写成 `throw` + `new` + `Error(` 连着的形态,所以没撞上;
//       — **漏判**:行过滤会连**模板串里**以 `*` 或 `//` 起头的续行一起删掉,
//         某条多行消息里写一条 `* 先看服务在不在` 的列表项,那行会被静默删,
//         拼出来的开头就和真消息对不上。逐字符扫描认得出那是串的内容,原样留着。
//   · 换来的**新边界**:模板串里的注释不再剥 —— 往 `shot.mjs` 注入页面的那段模板代码里
//     写一句 `throw new Error("…")` 形态的注释,会被当成一条真文案收走(**假红**,会出声)。
//   · **匹配单位是「症状列的单元格」**,不是整节、也不是整行(复审第 2/4 轮定的,
//     [SL-336] 补进本表 —— 此前只写在 `guideSymptomCells` 与 `run()` 的内联注释里):
//       — 通配 `.{0,40}?` **不跨 `|`、也不跨行**,否则能从隔壁「原因」列的散文里借字符
//         拼出一个命中(那正是第 2 轮的病灶,当时余量只剩 3 个字符);
//       — §5 里**不以 `|` 起头的行(散文)整行不参与判据**:把出口写在表外的散文里会判红。
//         方向是**假红**(会出声),但人打开表会觉得「我明明写了」,所以记在这里;
//       — 竖线两侧的形态**都要看,而且被切的是表那一侧**(复审第 1 轮纠正了我的路径):
//           · 消息侧的 `|` **不会**被切 —— 它走 `headPattern` → `escRe`,`|` 在字符类里被
//             转义成 `\|`;真正的后果是「消息带了一个单元格里不可能出现的字符」,
//             而且**只有当那个 `|` 落进开头 HEAD_CHARS 个字面字符里才查不到**
//             (超出头部的 `|` 根本不进正则,照常能查到)。今天 20 条一条没有;
//           · **镜像方向更容易撞上**:症状列里按 markdown 规矩写转义竖线 `\|` 时,
//             `line.split("|")[1]` 会把这一格从中间切断,于是一行**写对了**的表项匹配不上 ——
//             那是**假红**,而报错只说「查不到」,人打开表看它明明在。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { stripJsComments } from "./lib/strip-comments.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const SHOT = path.join(ROOT, "web-preview", "shot.mjs");
const GUIDE = path.join(ROOT, "web-preview", "PREVIEW-GUIDE.md");
const SECTION = "## 5. 排障表";
const HEAD_CHARS = 22;
// 收得过少 = 执行面被改空/正则被改坏。今天是 20 条上下,取一个不会被日常增删撞到的下界。
const MIN_MESSAGES = 12;

const errors = [];
const fail = (m) => errors.push(m);

const norm = (s) => s.replace(/[`*]/g, "").replace(/\s+/g, " ").trim();

const escRe = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

/**
 * 把 §5 正文切成**单元格**并归一化 —— 匹配单位是单元格,不是整节、也不是整行。
 * [复审第 3 轮] 这一步原来内联在 `run()` 里,于是它**自己没有常驻执行者**:
 * 自测全打在另外两个导出的纯函数上,没有一格碰得到它;把它整理回「按行切」之后,
 * 实跑与自测**两边都绿**(实测过)。而它恰恰是第 2 轮的核心判据 ——
 * 本卡的立论是「没有机器守着就会漂」,这句话当时原样复发在了判据自己身上。
 * 抽成导出的纯函数,下面的自测才够得着它。
 *
 * [复审第 4 轮] 再收一列:只取**症状列**。失败文案从第一版起就写着「写进那一行的**症状列**」,
 * 而实现搜的是任意列 —— **文案一直比判据严**,而文案严、判据松的时候没有任何东西会红。
 * 实测 19 条(20 减掉 EXEMPT 那条)**全部落在症状列、0 条只在别的列命中**,收窄零代价。
 */
export const guideSymptomCells = (section) =>
    section
        .split("\n")
        .filter((line) => line.trim().startsWith("|"))
        .map((line) => norm(line.split("|")[1] ?? ""))
        .filter(Boolean);

/** 从源码里收「用户可见失败文案」。两条执行面各自带上出处,报错时能指到人。 */
export function collectMessages(code) {
    const out = [];
    const seen = new Set();
    const add = (raw, where) => {
        if (raw === undefined || seen.has(raw)) return;
        seen.add(raw);
        out.push({ raw, where });
    };
    for (const m of code.matchAll(
        /throw new (?:Navigation)?Error\(\s*(?:`([\s\S]*?)`|"([^"]*)")/g,
    ))
        add(m[1] ?? m[2], "throw");
    // `evalLanded(cdp, <表达式>, "<这一步在做什么>")` —— 第三个实参会被拼进消息开头。
    // `(?<!function )` 排掉函数声明本身(它也长得像一次调用)。
    // [复审第 1 轮] 中段用 `(?:(?!evalLanded\()[\s\S])*?` 而不是裸 `[\s\S]*?`,
    // 让一次匹配**永远不能跨过下一个调用点**。
    // ⚠ **如实标注:这是收窄射程的防御,不是在修一个已证实的缺陷。** 复审担心的是
    // 「某个调用点不在本地闭合(`what` 传变量、选项对象里嵌套花括号)时,裸版跨过去把中间
    // 那条吞掉」。我试了四种形态(真源上把一处 `what` 改成变量;合成夹具里让下一个调用的
    // 表达式里带 `, "串"`;两条、三条连排)——**每一种下新旧两版收到的消息集合都一样**:
    // 吞并发生时,那一次匹配捕获的恰好就是下一个调用点的 `what`,所以并不丢。
    // 也就是说这条护栏**没有删除式支撑**;留着是因为它只会让匹配射程更短(方向安全),
    // 别把它读成一条有用例撑腰的判据。
    for (const m of code.matchAll(
        /(?<!function )evalLanded\((?:(?!evalLanded\()[\s\S])*?,\s*(?:"([^"]*)"|`([^`]*)`)\s*,?\s*(?:\{[^{}]*\}\s*,?\s*)?\)/g,
    ))
        add(m[1] ?? m[2], "evalLanded");
    return out;
}

/**
 * 把一条消息的**开头**编译成正则:字面量逐字匹配(空格放宽),`${…}` 当通配。
 * 表里用 `…` 占位的写法由通配**顺手接住**(`.` 匹配任意非换行字符,`…` 就是其中之一)——
 * [复审第 1 轮] 原来这里写成 `(?:.{0,40}?|…)`,那个 `|…` 分支**永远轮不到**:凡是它能
 * 匹配的,前一支必然先匹配上。本仓不留看不出来是死的判据,所以删掉,只留通配。
 * 返回 `null` 表示这条消息开头的字面量太短,不足以当查找锚点(调用方判负,不静默跳过)。
 */
export function headPattern(raw, headChars = HEAD_CHARS) {
    let literal = 0;
    let src = "";
    for (const part of raw.split(/(\$\{[^}]*\})/)) {
        if (/^\$\{/.test(part)) {
            src += ".{0,40}?";
            continue;
        }
        const n = norm(part);
        if (!n) continue;
        const take = n.slice(0, Math.max(0, headChars - literal));
        literal += take.length;
        src += escRe(take).replace(/ /g, "\\s*");
        if (literal >= headChars) break;
    }
    return literal >= 6 ? { re: new RegExp(src), literal } : null;
}

/**
 * 明确豁免:这些文案**故意**不进排障表,每条都要写清为什么。
 * 键是消息里的一段字面量(**不是**正则)。**留一条对不上任何消息的豁免就判红** ——
 * 否则改完文案之后,豁免会悄悄变成一张永远用不到的空头支票。
 * 这里**不留通配兜底**:写成「其余都豁免」这道门当场退化成恒真。
 */
const EXEMPT = new Map([
    [
        "--clip 需要 x,y,w,h 四个数",
        "命令行参数自检(退 1),消息自己就是完整说明;排障表是给「跑起来之后出问题」用的",
    ],
]);

function run() {
    if (!fs.existsSync(SHOT)) fail("找不到 web-preview/shot.mjs");
    if (!fs.existsSync(GUIDE)) fail("找不到 web-preview/PREVIEW-GUIDE.md");
    if (errors.length) return;

    const code = stripJsComments(
        fs.readFileSync(SHOT, "utf8"),
        "web-preview/shot.mjs",
    );
    const guide = fs.readFileSync(GUIDE, "utf8");
    const at = guide.indexOf(SECTION);
    if (at < 0) {
        fail(
            `PREVIEW-GUIDE.md 里找不到小节标题 "${SECTION}" —— 判据面读空了,` +
                "整条检查会退化成永远通过",
        );
        return;
    }
    // [复审第 1 轮] **只切 §5 这一节**,不能一路切到文件末尾:`## 6. 红线`(以及将来的 §7)
    // 会一并进判据面,于是「在 §5 里查得到」静默退化成「在 §5 到文末里查得到」——
    // 而这种放行**零输出**。这正是本卡要根除的那一族的镜像:判据面比宣称的宽。
    // [复审第 2 轮] **匹配单位是「单元格」,不是整节、也不是整行**。原来把整节折成一行再匹配,
    // 于是 `.{0,40}?` 里的 `.` 不受任何边界约束,可以跨着去拼一个命中。
    // 复审说的是「不许跨行」,**实测下来真正的边界是单元格**:markdown 里一个表行就是一行,
    // 按行切根本挡不住 —— `选择器语法错:…(…)` 那条打在整行上为真、打在症状列上为假,
    // 它命中的那对括号是从隔壁「原因」列的散文里借的(`你给的输入(` … `的选择器)`),
    // 40 的预算只剩 3 个字符。两个方向都不好:判据没在钉它宣称的东西(症状列里根本没那对
    // 括号),而且往那句原因里再加一个参数名就会顶过 40 变成红,报的还是「查不到」——
    // 人打开表一看它明明在。所以按 `|` 切到单元格再匹配。
    // [复审第 4 轮] 再收一列:只在**症状列**里找(见 `guideSymptomCells` 的注释)。
    const rest = guide.slice(at + SECTION.length);
    const next = rest.search(/\n## /);
    const cells = guideSymptomCells(next < 0 ? rest : rest.slice(0, next));

    const messages = collectMessages(code);
    if (messages.length < MIN_MESSAGES)
        fail(
            `只从 shot.mjs 收到 ${messages.length} 条失败文案(下界 ${MIN_MESSAGES})——` +
                "多半是抛法变了或本文件的两条执行面正则被改坏;收空了照样通过是本仓有前科的形态",
        );

    const usedExempt = new Set();
    for (const { raw, where } of messages) {
        const hitExempt = [...EXEMPT.keys()].find((k) => raw.includes(k));
        if (hitExempt) {
            usedExempt.add(hitExempt);
            continue;
        }
        const head = headPattern(raw);
        // [复审第 1 轮] **不许静默跳过**。原来这里是 `if (!head) continue;` —— 零输出,
        // 而 `MIN_MESSAGES` 兜的是**收集数**、够不到这一节:把某条消息的开头改短
        // (literal 掉到 6 以下)就能让它悄悄退出判据,而收集数一条没少、门照绿。
        // 今天 20 条全部有锚点,所以「一条都不许掉」是零代价的严档;真有消息短到没锚点,
        // 那它本来也无法用「拿原文查表」的方式排障,该改文案或写进 EXEMPT。
        if (!head) {
            fail(
                `这条失败文案的开头短到当不了查找锚点(来源:${where}):\n` +
                    `      消息:${norm(raw).slice(0, 60)}\n` +
                    "      判据要求开头至少 6 个字面字符(插值不算)。把开头写长一点," +
                    "或在 EXEMPT 里写明为什么它不需要排障表出口。",
            );
            continue;
        }
        if (!cells.some((cell) => head.re.test(cell)))
            fail(
                `失败文案在 §5 排障表里查不到(来源:${where}):\n` +
                    `      消息:${norm(raw).slice(0, 60)}\n` +
                    `      找的是它开头那 ${head.literal} 个字(插值当通配),` +
                    "只在 §5 表格的「症状」列里找(不看原因列与处置列)。\n" +
                    "      人撞上这条时会复制报错去查表 —— 查不到就等于这条没有出口。\n" +
                    "      修法:把这句开头照原文写进 §5 那一行的症状列(引号也要一致)," +
                    "或在本文件的 EXEMPT 里写明为什么它不该进表。",
            );
    }
    for (const k of EXEMPT.keys())
        if (!usedExempt.has(k))
            fail(
                `EXEMPT 里这条对不上任何消息,已经过期:${k}\n` +
                    "      文案改了就把豁免一起改;留着它等于给一条不存在的消息发通行证。",
            );
}

function selfTest() {
    const bad = [];
    // [复审第 2 轮] 格数**由 `t()` 累加**,不写死:加一格忘了改数字就会印错数,
    // 而「印错数」正是本仓「注释/输出比实现强」那一族最轻也最常见的一种。
    let cases = 0;
    const t = (ok, why) => {
        cases += 1;
        if (!ok) bad.push(why);
    };
    // 夹具里的标记与文案一律**拼装**,别写成连续字面量 —— 本文件自己也在被
    // check-gates-visibility 的散文守卫扫,而且下面这些串正是判据要找的东西。
    const D = "$" + "{x}";
    t(
        collectMessages('throw new Error("abc' + 'def");').length === 1,
        "collectMessages 收不到普通 throw",
    );
    t(
        collectMessages("throw new NavigationError(`abc" + "def`);").length ===
            1,
        "collectMessages 收不到 NavigationError",
    );
    t(
        collectMessages('evalLanded(cdp, "expr", "第三个实参");')[0]?.raw ===
            "第三个实参",
        "collectMessages 取不到 evalLanded 的 what",
    );
    t(
        collectMessages(
            "async function evalLanded(cdp, expression, what) {\n  return 1;\n}",
        ).length === 0,
        "collectMessages 把函数声明当成了调用",
    );
    // [复审第 3 轮] 这两格钉住**第 2 轮的核心判据**:匹配单位是单元格,通配不许跨 `|`。
    // 夹具用的正是当时的病灶形态 —— 症状列只写到 `选择器语法错:…`,而那对括号在隔壁
    // 「原因」列里;整行拼得出来,单元格拼不出来。
    // 反向验证(**下表是逐格实测的结果,不是推演**;三种注入各跑一次三格版本):
    //   A ← 把切法退回「按行切」  ⇒ **A 与 C 同时红**(整行里两格的夹具都拼得中);
    //   B ← 切得过头(连全角左括号一起切) ⇒ 只有 B 红;
    //   C ← 把取列退回「任意列」  ⇒ 只有 C 红。
    // 也就是说 **B / C 各自有一个「只让自己红」的注入,而 A 没有** —— 按行切时 A 会红,
    // 但同一注入下 C 也红;A 在这个方向上不是唯一守卫,删掉它这条线仍由 C 兜住。
    // (别把它读成「按行切时 A 不响」——A 是响的,只是不独占。)
    // [SL-336] 这三行原来写的是「三格各有各的注入,互不串台」,**是假的**:
    // A/B 那两格是在 C 还不存在时验的,加 C 之后我没有复跑 A,把两格的结论外推到了三格。
    // 教训写在这里而不是删掉:**验证记录也是一种宣称,而且是唯一一种不会被执行的宣称** ——
    // 判据面一改(收窄、放宽、加一格),全部反向注入都要从头跑,不许拿上一版的结论外推。
    const crossCol = headPattern("选择器语法错:" + D + "(" + D + ")");
    const borrowRow =
        "| 报「选择器语法错:…」 | 抛错源自你给的输入(--eval 的表达式)与页面无关 |";
    t(
        !!crossCol &&
            !guideSymptomCells(borrowRow).some((c) => crossCol.re.test(c)),
        "通配跨过了 `|` —— 单元格口径失守(第 2 轮病灶:那对括号是从原因列借的)",
    );
    t(
        !!crossCol &&
            guideSymptomCells("| 报「选择器语法错:…(…)」 | 原因列 |").some(
                (c) => crossCol.re.test(c),
            ),
        "症状列把括号写全了反而不命中 —— 单元格切法把该留的也切没了",
    );
    // C:[复审第 4 轮] 只在**症状列**里找。这一格钉的是「症状列里没有、只在别的列出现」——
    // 失败文案从第一版就写着「写进症状列」,判据却搜任意列,那是文案比判据严。
    t(
        !!crossCol &&
            !guideSymptomCells(
                "| 别的症状 | 原因里顺手写了 选择器语法错:…(…) | 处置 |",
            ).some((c) => crossCol.re.test(c)),
        "只在原因列出现也算命中 —— 判据面比失败文案宣称的宽了一列",
    );
    const h = headPattern("壳页 " + D + " 上找不到 .pv-status —— 第四道判据");
    t(!!h, "headPattern 把插值开头的消息判成了没有锚点");
    t(
        !!h &&
            h.re.test(
                norm("| 报「壳页 … 上找不到 `.pv-status` —— 第四道判据」 |"),
            ),
        "headPattern 匹配不上表里用 … 占位、带反引号的写法",
    );
    t(
        !!h &&
            !h.re.test(norm("| 报「壳页 … 上找不到 .pv-statusX —— 第四道」 |")),
        "headPattern 太松:改了字也照样命中",
    );
    t(headPattern("短") === null, "headPattern 没把太短的开头判成无锚点");
    if (bad.length) {
        console.error("check-preview-messages --self-test 失败:");
        for (const b of bad) console.error("  " + b);
        process.exit(1);
    }
    console.log("check-preview-messages --self-test 通过:" + cases + " 格");
}

if (process.argv.includes("--self-test")) {
    selfTest();
} else {
    run();
    if (errors.length) {
        console.error("check-preview-messages 判负:");
        for (const e of errors) console.error("  [FAIL] " + e);
        console.error(
            "  口径:shot.mjs 没有机器调用方,文档是失败文案唯一的出口(#194);" +
                "人的用法是拿报错原文查 PREVIEW-GUIDE §5。",
        );
        process.exit(1);
    }
    console.log(
        "check-preview-messages 通过:shot.mjs 的每条用户可见失败文案,开头那一段都能在 " +
            "PREVIEW-GUIDE §5 排障表里按原文查到。",
    );
}
