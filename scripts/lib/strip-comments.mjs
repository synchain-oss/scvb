// SPDX-License-Identifier: GPL-3.0-or-later
// strip-comments.mjs —— JS / PowerShell 的**逐字符**注释剥离器(零依赖,Node >= 18,ESM)。
//
// 谁在用:`check-gates-visibility.mjs`(①段六套页面级冒烟、§④ gate 3i 脚本圈、②段 gates.ps1)
// 与 `check-preview-messages.mjs`(shot.mjs)。此前**每个文件各带一份行过滤器**,
// 两份 `stripJsComments` 逐字相同 —— 而两份拷贝的坏处不是重复,是**只改一份**。
//
// [SL-330b] 为什么从「按行过滤」换成逐字符扫描:行过滤器只认**行首**的注释符,
//   于是同一个哑弹换个写法就回来了,两处都被实际咬过:
//   · JS:`stripJsComments` 只丢弃 `^\s*(//|/?\*)` 的行,所以块注释**中间那些不带 `*` 的行**
//     原样留在「代码」里 —— 在里面写一句 `browserFailed()`,①段那条断言就退回哑弹;
//   · PowerShell:`stripPsComments` 的逐行状态机只认**整行以 `<#` 起头**,而 PS 允许
//     `<#` 开在**代码行尾**;把一段赋值写进那种块注释里就能顶替真接线。
//   两条的方向都是**漏判**(注释冒充代码),不是假红。
//
// 口径:**只删注释,别的一个字节都不动**。字符串/模板串/正则字面量原样保留 ——
//   判据要在字符串字面量里找东西(消息文案、标签拼装),剥掉它们等于把判据面挖空。
//   注释按「只留换行」剥:行号与行首锚(`^\s*…`,多行正则)全部保真,
//   `[\s\S]{0,N}` 那类窗口也不会因为剥法变了而莫名超窗。
//
// ⚠ 边界(照实说,别让人以为它是编译器):
//   · 它是**词法级**扫描,不建语法树。JS 侧「`/` 是正则还是除号」用的是回看上一个有效字符
//     的启发式(标识符/数字/`)`/`]`/引号之后算除号,关键字之后算正则);判错的兜底是
//     「正则字面量不跨行」—— 扫不到收尾斜杠就退回当除号,不会把后面的代码整段吃掉。
//   · JS 侧不认 HTML 注释(`<!--`),那不是本仓的写法。
//   · PowerShell 侧的双引号串**不解析子表达式** `$( … )`:串里嵌一段自带双引号的代码
//     (`"$($x -replace '"','')"`)会让配对错位。今天 gates.ps1 里没有这种写法,
//     真出现时的方向多半是**少剥**(与换本扫描器之前同向);兜底见下一条。
//   · PowerShell 的 `#` 只在**行首或前面是空白/`;{}()|&,`** 时才当注释开头 ——
//     贴着一个词写的 `#`(`foo#bar`)按裸词的一部分留着。方向是**少剥**,与旧实现同向。
//   · 扫到文件尾还停在串/块注释里(说明上面某处配错了)就**抛错**,不静默返回半份文本:
//     半份文本会让判据面无声缩水,而那正是本仓最贵的那一类失效。
//
// 自测:`node scripts/lib/strip-comments.mjs --self-test`(gate 3i 与 CI docs-truth 各跑一次)。
//   反向验证按「一处分支一次拆除」跑过一轮:每一处拆掉都至少有一格红,且红的就是设计上
//   接住它的那几格。**逐处红了哪几格的表不抄在这里**(那种表一改实现就变成假话,本仓已经
//   为「注释里抄一份别处的事实」栽过好几次)—— 它在本卡 PR 的正文里,连同两条由这轮反向验证
//   逼出来的改动:①`''` / `""` 翻倍转义那两个分支是死的,删了(理由写在 `scanPsString` 旁);
//   ②「没收尾要抛错」那几格原来只钉「抛了」,而拆掉显式抛错之后扫描器照样会崩(空转到
//   `Invalid array length`),所以改钉「抛出来的话认得出是本模块的判断」。

import { pathToFileURL } from "node:url";

const RE_WORD = /[A-Za-z0-9_$]/;

// `/` 跟在这些关键字后面是**正则**不是除号。回看只拿得到「上一个标识符」,
// 所以这里列的是关键字本身,不是它们的前缀。
const REGEX_AFTER_KEYWORD = new Set([
    "await",
    "case",
    "delete",
    "do",
    "else",
    "in",
    "instanceof",
    "new",
    "of",
    "return",
    "throw",
    "typeof",
    "void",
    "yield",
]);

const lineOf = (text, at) => text.slice(0, at).split("\n").length;

// 注释的替身:**只留换行**,不产生任何空白。行号、行首锚、行数三样都不变。
const newlinesOf = (chunk) => chunk.replace(/[^\n]/g, "");

// **返回** Error 而不是抛:调用点写成 `throw unterminatedError(…)`,「扫不下去就必须终止」
// 由控制流本身保证,拆不掉。写成「函数内部抛」的那一版是**本卡自己踩过的形态** ——
// 把那句 throw 拆掉之后,扫描器不是安静地返回半份文本,而是原地空转到
// `Invalid array length` 才崩,自测那几格照绿(见 --self-test 里 throwsClear 的注释)。
const unterminatedError = (where, what, text, at) =>
    new Error(
        `${where}:第 ${lineOf(text, at)} 行开始的${what}扫到文件尾都没有收尾。` +
            "这多半不是源文件的问题,而是 scripts/lib/strip-comments.mjs 在前面某处配错了" +
            "(把除号读成正则、或引号配错)。别绕过这条错:剥不干净的文本会让调用它的判据" +
            "无声缩水。",
    );

/**
 * 剥掉 JS 源码里的注释(`//` 与 `/* … *\/`),字符串/模板串/正则字面量原样保留。
 * @param {string} text 源码
 * @param {string} where 出错时报给人的出处(文件名)
 */
export function stripJsComments(text, where = "js") {
    const n = text.length;
    const out = [];
    let i = 0;
    // 上一个**有效代码字符**与它所在的标识符:只给「`/` 是不是正则」那一处用。
    // 空白不重置标识符(`return /re/` 中间隔着空格),注释也不参与(注释对词法透明)。
    let last = "";
    let word = "";
    let mode = "code"; // code | tpl
    let tplAt = 0; // 当前模板串的起点,只为报错时说得出行号
    const braces = []; // 模板插值栈:每层记该层已开未闭的 `{` 数

    const sig = (c) => {
        last = c;
        word = RE_WORD.test(c) ? word + c : "";
    };
    const afterLiteral = (c) => {
        last = c;
        word = "";
    };

    // 行首 `#!`:node 脚本的 shebang,当整行代码原样留着(它不是注释,剥了会改行内容)。
    if (text.startsWith("#!")) {
        const eol = text.indexOf("\n");
        const stop = eol < 0 ? n : eol;
        out.push(text.slice(0, stop));
        i = stop;
    }

    while (i < n) {
        const c = text[i];

        if (mode === "tpl") {
            if (c === "\\") {
                out.push(text.slice(i, i + 2));
                i += 2;
                continue;
            }
            if (c === "`") {
                out.push(c);
                i += 1;
                mode = "code";
                afterLiteral("`");
                continue;
            }
            if (c === "$" && text[i + 1] === "{") {
                out.push(text.slice(i, i + 2));
                i += 2;
                braces.push(0);
                mode = "code";
                afterLiteral("{");
                continue;
            }
            out.push(c);
            i += 1;
            continue;
        }

        // ── 注释:两种形态都**只留换行** ────────────────────────────────────────
        if (c === "/" && text[i + 1] === "/") {
            const eol = text.indexOf("\n", i);
            i = eol < 0 ? n : eol; // 换行本身留给下一轮原样输出
            continue;
        }
        if (c === "/" && text[i + 1] === "*") {
            const end = text.indexOf("*/", i + 2);
            if (end < 0) throw unterminatedError(where, "块注释", text, i);
            out.push(newlinesOf(text.slice(i, end + 2)));
            i = end + 2;
            continue;
        }

        // ── 字面量:原样保留 ───────────────────────────────────────────────────
        if (c === '"' || c === "'") {
            const end = scanJsString(text, i, c);
            if (end < 0) throw unterminatedError(where, "字符串", text, i);
            out.push(text.slice(i, end + 1));
            i = end + 1;
            afterLiteral(c);
            continue;
        }
        if (c === "`") {
            out.push(c);
            i += 1;
            mode = "tpl";
            tplAt = i - 1;
            continue;
        }
        if (c === "/" && regexAllowed(last, word)) {
            const end = scanJsRegex(text, i);
            // 扫不到收尾斜杠(或撞上换行)= 上面那个启发式判错了,退回当除号。
            if (end >= 0) {
                out.push(text.slice(i, end + 1));
                i = end + 1;
                afterLiteral("/");
                continue;
            }
        }

        // ── 模板插值的花括号配对:`}` 回到模板态要认准是哪一层 ───────────────────
        if (c === "{" && braces.length) braces[braces.length - 1] += 1;
        if (c === "}" && braces.length) {
            if (braces[braces.length - 1] === 0) {
                braces.pop();
                out.push(c);
                i += 1;
                mode = "tpl";
                afterLiteral("}");
                continue;
            }
            braces[braces.length - 1] -= 1;
        }

        out.push(c);
        i += 1;
        if (!/\s/.test(c)) sig(c);
    }

    if (mode === "tpl" || braces.length)
        throw unterminatedError(where, "模板串", text, tplAt);
    return out.join("");
}

// 引号串:`\` 吃掉后一个字符(含续行的 `\` + 换行);裸换行 = 没收尾。
function scanJsString(text, start, quote) {
    const n = text.length;
    let j = start + 1;
    while (j < n) {
        const c = text[j];
        if (c === "\\") {
            j += 2;
            continue;
        }
        if (c === quote) return j;
        if (c === "\n") return -1;
        j += 1;
    }
    return -1;
}

// 正则字面量:`\` 转义、`[…]` 字符类里的 `/` 不算收尾;跨行即判「不是正则」。
function scanJsRegex(text, start) {
    const n = text.length;
    let j = start + 1;
    let inClass = false;
    while (j < n) {
        const c = text[j];
        if (c === "\\") {
            j += 2;
            continue;
        }
        if (c === "\n") return -1;
        if (inClass) {
            if (c === "]") inClass = false;
        } else if (c === "[") inClass = true;
        else if (c === "/") return j;
        j += 1;
    }
    return -1;
}

// 回看一个字符定「正则还是除号」。判错的代价由 `scanJsRegex` 的跨行兜底兜住:
// 把除号读成正则时扫不到收尾斜杠 ⇒ 退回除号;把正则读成除号时正则体按代码走,
// 里面的引号有可能开一个串 —— 所以引号/闭括号那几档写死成除号,别靠猜。
function regexAllowed(last, word) {
    if (last === "") return true;
    if (RE_WORD.test(last)) return REGEX_AFTER_KEYWORD.has(word);
    if (last === ")" || last === "]") return false;
    if (last === '"' || last === "'" || last === "`") return false;
    return true;
}

/**
 * 剥掉 PowerShell 源码里的注释(`#` 与 `<# … #>`),字符串/here-string 原样保留。
 * @param {string} text 源码
 * @param {string} where 出错时报给人的出处(文件名)
 */
export function stripPsComments(text, where = "ps") {
    const n = text.length;
    const out = [];
    let i = 0;

    while (i < n) {
        const c = text[i];

        // `<#` 可以开在**任何位置**,包括代码行尾 —— 这正是换掉逐行状态机的理由之一。
        // 引号串里的 `<#` 不会走到这里:下面的串分支先把整串吃掉了。
        if (c === "<" && text[i + 1] === "#") {
            const end = text.indexOf("#>", i + 2);
            if (end < 0) throw unterminatedError(where, "块注释 <#", text, i);
            out.push(newlinesOf(text.slice(i, end + 2)));
            i = end + 2;
            continue;
        }
        if (c === "#" && psCommentStarts(text, i)) {
            const eol = text.indexOf("\n", i);
            i = eol < 0 ? n : eol;
            continue;
        }

        // here-string:`@"` / `@'` 后面必须是行尾,收尾的 `"@` / `'@` 必须独占行首。
        if (
            c === "@" &&
            (text[i + 1] === '"' || text[i + 1] === "'") &&
            restOfLineBlank(text, i + 2)
        ) {
            const term = "\n" + text[i + 1] + "@";
            const end = text.indexOf(term, i + 2);
            if (end < 0) throw unterminatedError(where, "here-string", text, i);
            out.push(text.slice(i, end + term.length));
            i = end + term.length;
            continue;
        }
        if (c === "'" || c === '"') {
            const end = scanPsString(text, i, c);
            if (end < 0) throw unterminatedError(where, "引号串", text, i);
            out.push(text.slice(i, end + 1));
            i = end + 1;
            continue;
        }
        // 反引号是 PowerShell 的转义符:吃掉后一个字符(续行的反引号 + 换行也在内)。
        if (c === "`") {
            out.push(text.slice(i, i + 2));
            i += 2;
            continue;
        }

        out.push(c);
        i += 1;
    }
    return out.join("");
}

// PowerShell 的 `#` 只在词首才开注释。少剥的方向与旧实现同向,见头注边界段。
function psCommentStarts(text, at) {
    if (at === 0) return true;
    const p = text[at - 1];
    return /\s/.test(p) || ";{}()|&,".includes(p);
}

function restOfLineBlank(text, from) {
    const n = text.length;
    for (let j = from; j < n; j += 1) {
        if (text[j] === "\n") return true;
        if (!/\s/.test(text[j])) return false;
    }
    return true;
}

// PowerShell 引号串:双引号里反引号转义;两种引号都**允许跨行**(与 JS 不同),
// 所以没有「撞上换行就算没收尾」那一档。
// ⚠ **不认 `''` / `""` 翻倍转义,而且不必认**:那两种写法是「收尾紧接着重开」——
//   按翻倍读得到一整段串,按普通读得到两段首尾相接的串,**覆盖到的字符区间逐字相同**,
//   中间挤不进一个能被误当注释的 `#`,拼回去的文本也逐字相同。写那个分支就是一条
//   永远造不出反例的死判据(反向验证跑出来正是「删掉它没有任何一格变红」)。
function scanPsString(text, start, quote) {
    const n = text.length;
    let j = start + 1;
    while (j < n) {
        const c = text[j];
        if (quote === '"' && c === "`") {
            j += 2;
            continue;
        }
        if (c === quote) return j;
        j += 1;
    }
    return -1;
}

// ── --self-test ────────────────────────────────────────────────────────────────
// **每一种 token 各一格**:漏掉哪种,这份剥离器就在那一种上退回旧实现的哑弹。
// 格数由 `t()` 累加,不写死 —— 写死的数只会在加格的那天变成一句假话。
//
// ⚠ 每一格都写成**无参函数**,由 `t()` 在 try 里调用:夹具抛错 = **那一格**红,而不是
//   把整份自测炸成一段栈。这不是洁癖 —— 反向验证时,拆掉扫描器的一个分支往往让某个夹具
//   直接抛错,写成表达式的话整份自测在第一格就中断,后面几十格**一格都没跑**,
//   于是「哪几格接得住这次拆除」根本读不出来(本卡第一轮反向验证就是这么读不出来的)。
function selfTest() {
    const bad = [];
    let cases = 0;
    const t = (fn, why) => {
        cases += 1;
        let ok = false;
        try {
            ok = fn();
        } catch {
            ok = false;
        }
        if (!ok) bad.push(why);
    };
    const js = (s) => stripJsComments(s, "自测");
    const ps = (s) => stripPsComments(s, "自测");
    const nl = (s) => s.split("\n").length;
    // 钉的**不是「抛了」,是「抛出来的话认得出是本模块下的判断」**。反向验证实测:
    // 早先那一版把 throw 写在 `unterminated()` **函数内部**,拆掉它之后扫描器会在同一个
    // 位置原地空转、一路把 `out` 撑到 `Invalid array length` 才崩 —— **照样是抛**,
    // 只写 `throws()` 的话这几格全绿,而人拿到的是一句什么都指不出来的引擎错。
    // 所以对一句话:失败要**报得出人话**。
    // (复审第 1 轮起结构也换了:工厂函数**返回** Error、由调用点 `throw`,「必须终止」
    // 不再靠约定;这几格照留 —— 它们钉的是**话**,与用哪种结构无关。)
    const throwsClear = (fn) => {
        try {
            fn();
            return false;
        } catch (e) {
            return String(e && e.message).includes("扫到文件尾都没有收尾");
        }
    };
    // 夹具里的注释符与引号一律**拼装**:这份文件将来若也被某道「散文不许长得像代码」
    // 的守卫扫到,连续字面量就是它第一个咬住的东西(本仓 check-preview-messages 的
    // 自测已经因为同一条理由改成拼装)。
    const SL = "/" + "/";
    const BO = "/" + "*";
    const BC = "*" + "/";
    const BQ = String.fromCharCode(96); // 反引号
    const HASH = "#";

    // ── JS:注释的两种形态 ────────────────────────────────────────────────────
    t(
        () => js("a\n" + SL + " 注释\nb") === "a\n\nb",
        "JS 整行 " + SL + " 注释没剥干净(或把那一行的换行也吃了)",
    );
    t(
        () => js("a = 1; " + SL + " 尾注释") === "a = 1; ",
        "JS **行尾** 注释没剥 —— 旧的行过滤器只认行首,这一格就是它漏的那一档",
    );
    t(
        () =>
            !js(
                "x\n" +
                    BO +
                    " 头\n  中间这行不带星号 browserFailed()\n" +
                    BC +
                    "\ny",
            )
                .split("\n")
                .some((l) => l.includes("browserFailed")),
        "JS 块注释里**不带星号的中间行**没剥 —— [SL-330b] 要关的就是这个洞",
    );
    t(
        () => js("a " + BO + " 夹在中间 " + BC + " b") === "a  b",
        "JS 行内块注释剥完没把两边的代码留下",
    );

    // ── JS:三种引号 + 转义,串里的注释符必须原样留着 ───────────────────────────
    t(
        () => js('const u = "http:' + SL + '127.0.0.1";').includes("127.0.0.1"),
        "双引号串里的 " + SL + " 被当成注释剥了(URL 会被拦腰截断)",
    );
    t(
        () =>
            js('const s = "' + BO + " 不是注释 " + BC + '";').includes(
                "不是注释",
            ),
        "双引号串里的块注释符被当成注释开头",
    );
    t(
        () => js("const s = 'http:" + SL + "x';").includes("http:" + SL + "x"),
        "单引号串里的 " + SL + " 被剥了",
    );
    t(
        () =>
            js("const s = " + BQ + "line\n" + SL + " 这是串的内容\n" + BQ + ";")
                .split("\n")
                .some((l) => l.includes("这是串的内容")),
        "模板串里的 " + SL + " 行被剥了 —— 那是串的内容,不是这个文件的注释",
    );
    t(
        () =>
            js(
                'const s = "带转义的引号 \\" 之后 ' + SL + ' 仍在串里";',
            ).includes("仍在串里"),
        "转义引号让串提前收尾了",
    );

    // ── JS:模板插值里回到代码态 ───────────────────────────────────────────────
    t(() => {
        const out = js(
            "const s = " + BQ + "a${x " + SL + " 注释\n}b" + BQ + ";",
        );
        return (
            !out.includes("注释") && out.includes("a${x") && out.includes("}b")
        );
    }, "模板插值 ${…} 里的注释没剥(或把插值外的模板文本一起动了)");
    t(
        () =>
            js(
                "const s = " +
                    BQ +
                    "A${" +
                    BQ +
                    "B" +
                    SL +
                    "C" +
                    BQ +
                    "}D" +
                    BQ +
                    ";",
            ).includes("B" + SL + "C"),
        "嵌套模板串里的 " + SL + " 被剥了 —— 插值栈没数对层",
    );

    // ── JS:正则字面量 vs 除号 ────────────────────────────────────────────────
    t(() => {
        const out = js("if (/[\"']$/.test(x)) y; " + SL + " 尾注释");
        return out.includes("/[\"']$/") && !out.includes("尾注释");
    }, "正则字面量里的引号开了一个串 —— 后面的真注释就剥不掉了");
    t(() => {
        // 转义斜杠后面**紧跟一个引号**:少认这一档时正则会在 `\/` 处提前收尾,
        // 露出来的那个引号开一个串,一路吞到文件尾 ⇒ 抛错 ⇒ 本格红。
        const out = js('const re = /a\\/"/; ' + SL + " 尾注释");
        return out.includes('/a\\/"/') && !out.includes("尾注释");
    }, "正则字面量里**转义过的斜杠**被当成了收尾斜杠");
    t(() => {
        const out = js('const re = /[/"]/; ' + SL + " 尾注释");
        return out.includes('/[/"]/') && !out.includes("尾注释");
    }, "正则**字符类里**的裸斜杠被当成了收尾斜杠 —— 剩下那个引号会开一个串");
    t(() => {
        const out = js("const q = a / b; " + SL + " 尾注释");
        return out.includes("a / b") && !out.includes("尾注释");
    }, "除号被当成正则开头,把后面的代码吃进了字面量");
    t(
        () => js('return /a"b/;').includes('/a"b/'),
        "关键字 return 后面的正则被当成除号 —— 里面那个引号会开一个串",
    );

    // ── JS:行号保真 + 没收尾就抛 ─────────────────────────────────────────────
    t(() => {
        const src = "a\n" + BO + "\n注释\n" + BC + "\nb\n";
        return nl(js(src)) === nl(src);
    }, "JS 剥完行数变了 —— 行首锚与跨行窗口都会跟着漂");
    t(
        () => throwsClear(() => js("a " + BO + " 开了没关\nb")),
        "JS 块注释没收尾时没抛错(会静默返回半份文本)",
    );
    t(
        () => throwsClear(() => js('const s = "开了没关\n')),
        "JS 字符串没收尾时没抛错",
    );
    t(
        () => throwsClear(() => js("const s = " + BQ + "开了没关\n")),
        "JS 模板串没收尾时没抛错",
    );

    // ── PowerShell:两种注释 ──────────────────────────────────────────────────
    t(
        () =>
            ps("$a = 1\n" + HASH + " 整行注释\n$b = 2")
                .split("\n")
                .every((l) => !l.includes("整行注释")),
        "PS 整行 " + HASH + " 注释没剥",
    );
    t(
        () => ps("$a = 1  " + HASH + " 尾注释") === "$a = 1  ",
        "PS **行尾** " +
            HASH +
            " 注释没剥 —— 旧实现只认行首,这一档能顶替真接线",
    );
    t(
        () =>
            !ps(
                "$a = 1 <" +
                    HASH +
                    " 开在行尾\n$smokeLabel = $x " +
                    HASH +
                    ">\n$b = 2",
            )
                .split("\n")
                .some((l) => l.includes("smokeLabel")),
        "PS 块注释**开在代码行尾**时没剥 —— [SL-330b] 要关的就是这个洞",
    );
    t(
        () =>
            !ps("<" + HASH + "\n.SYNOPSIS 帮助块\n" + HASH + ">\n$a = 1")
                .split("\n")
                .some((l) => l.includes("SYNOPSIS")),
        "PS 跨行块注释没剥",
    );

    // ── PowerShell:引号串 / here-string / 转义 ───────────────────────────────
    // ⚠ 这里**没有**「`''` / `""` 翻倍转义」那一格,也没有对应的分支:那两种写法是
    //   「收尾紧接着重开」,两种读法覆盖到的字符区间**逐字相同**,对「哪一段是串、哪一段是
    //   代码」没有任何可观测差别。写了分支也永远造不出能让它红的夹具 —— 本仓不留看不出来
    //   是死的判据(check-preview-messages 的 `|…` 死分支是同一条纪律),所以两边都不写。
    t(
        () => ps("$a = '井号 " + HASH + " 在串里'").includes("在串里"),
        "PS 单引号串里的 " + HASH + " 被当成注释剥了",
    );
    t(
        () => ps('$a = "井号 ' + HASH + ' 在串里"').includes("在串里"),
        "PS 双引号串里的 " + HASH + " 被当成注释剥了",
    );
    t(
        () =>
            ps('$a = "转义 ' + BQ + '" 之后"  ' + HASH + " 尾注释").includes(
                "之后",
            ),
        "PS 双引号里的反引号转义让串提前收尾",
    );
    t(
        () => {
            // here-string 里放一个**撇号**:少认这一档时,`@` 后面那个引号会开一个普通单引号串,
            // 在撇号处收尾,剩下的 `'@` 再开一个一路吞到文件尾的串 ⇒ 抛错 ⇒ 本格红。
            // (不放撇号的话,普通串的读法恰好覆盖同一段,这一格就照绿 —— 实测过。)
            const out = ps(
                "$a = @'\n井号 " +
                    HASH +
                    " 在 here-string 里,还有个撇号 don't\n'@\n$b = 2  " +
                    HASH +
                    " 尾注释",
            );
            return (
                out.includes("在 here-string 里") &&
                out.includes("$b = 2") &&
                !out.includes("尾注释")
            );
        },
        "PS here-string 没按「收尾独占行首」整段读 —— 里面的 " +
            HASH +
            " 会被当注释",
    );
    t(
        () =>
            ps("$p = '<" + HASH + " 引用一段词法'\n$b = 2").includes(
                "引用一段词法",
            ),
        "PS 串里的 <" + HASH + " 开了块注释 —— 那是 SL-322 第 6 轮栽过的形态",
    );
    t(
        () =>
            ps("$a = 'x'\nfoo" + HASH + "bar\n").includes("foo" + HASH + "bar"),
        "PS 贴着词写的 " + HASH + " 被当成注释开头(方向应为少剥)",
    );
    t(() => {
        const out = ps("$a = " + BQ + "'\n$b = 2  " + HASH + " 尾注释");
        return out.includes("$b = 2") && !out.includes("尾注释");
    }, "PS **代码里**的反引号没吃掉后一个字符 —— 被它转义的那个引号会开一个串," + "把后面整段连同真注释一起吞进去");

    // ── PowerShell:行号保真 + 没收尾就抛 ─────────────────────────────────────
    t(() => {
        const src = "$a = 1\n<" + HASH + "\n注释\n" + HASH + ">\n$b = 2\n";
        return nl(ps(src)) === nl(src);
    }, "PS 剥完行数变了 —— gates.ps1 的行首锚断言全靠它");
    t(
        () => throwsClear(() => ps("$a = 1 <" + HASH + " 开了没关\n$b = 2")),
        "PS 块注释没收尾时没抛错",
    );
    t(
        () => throwsClear(() => ps("$a = '开了没关\n$b = 2")),
        "PS 引号串没收尾时没抛错",
    );

    if (bad.length) {
        console.error("strip-comments --self-test 失败:");
        for (const b of bad) console.error("  " + b);
        process.exit(1);
    }
    console.log("strip-comments --self-test 通过:" + cases + " 格");
}

// 只有**直接跑这个文件**时才自测 —— 它是被 import 的库,而导入它的脚本自己也带
// `--self-test`(check-preview-messages),不加这道判别会在那条命令上跑错一份自测。
const entry = process.argv[1] ? pathToFileURL(process.argv[1]).href : "";
if (entry === import.meta.url) {
    if (process.argv.includes("--self-test")) selfTest();
    else {
        console.error(
            "strip-comments.mjs 是库,不是门禁:它只有 --self-test 一个可跑的入口。",
        );
        process.exit(2);
    }
}
