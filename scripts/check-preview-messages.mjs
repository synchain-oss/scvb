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
//   · **只剥整行注释**带来的两个方向,都在「文本级」这条边界之内,记在这里免得下一个人
//     重新推一遍(复审第 1 轮):
//       — **假红**:行尾注释没剥,`const x = 1; // 旧写法是 throw new Error("…")` 会被当成
//         实现里的一条文案收走。`shot.mjs` 注释密度高、正文里就在大段引用旧文案,
//         今天那些恰好没写成 `throw` + `new` + `Error(` 连着的形态,所以没撞上;
//       — **漏判**:剥注释的行形态会连**模板串里**以 `*` 或 `//` 起头的续行一起删掉,
//         将来某条多行消息里写一条 `* 先看服务在不在` 的列表项,那行会被静默删,
//         拼出来的开头就和真消息对不上。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const SHOT = path.join(ROOT, "web-preview", "shot.mjs");
const GUIDE = path.join(ROOT, "web-preview", "PREVIEW-GUIDE.md");
const SECTION = "## 5. 排障表";
const HEAD_CHARS = 22;
// 收得过少 = 执行面被改空/正则被改坏。今天是 20 条上下,取一个不会被日常增删撞到的下界。
const MIN_MESSAGES = 12;

const errors = [];
const fail = (m) => errors.push(m);

// JS 整行注释(`//` 与块注释的三种行形态)。与 check-gates-visibility 同口径:
// 不剥的话,注释里引用一句旧文案就能冒充实现(本仓 ① 段栽过的那个哑弹)。
const stripJsComments = (text) =>
    text
        .split("\n")
        .filter((l) => !/^\s*(\/\/|\/?\*)/.test(l))
        .join("\n");

const norm = (s) => s.replace(/[`*]/g, "").replace(/\s+/g, " ").trim();

const escRe = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

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

    const code = stripJsComments(fs.readFileSync(SHOT, "utf8"));
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
    const rest = guide.slice(at + SECTION.length);
    const next = rest.search(/\n## /);
    const section = norm(next < 0 ? rest : rest.slice(0, next));

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
        if (!head.re.test(section))
            fail(
                `失败文案在 §5 排障表里查不到(来源:${where}):\n` +
                    `      消息:${norm(raw).slice(0, 60)}\n` +
                    `      找的是它开头那 ${head.literal} 个字(插值当通配)。` +
                    "人撞上这条时会复制报错去查表 —— 查不到就等于这条没有出口。\n" +
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
    const t = (ok, why) => {
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
    console.log("check-preview-messages --self-test 通过:" + 8 + " 格");
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
