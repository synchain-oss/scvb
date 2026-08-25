// SPDX-License-Identifier: GPL-3.0-or-later
// ======================================================================
// gen-hard-rules.mjs —— 九条硬约束红字的唯一生成器(12 §3.4 / 05 §5 / 07 T39b)
// ======================================================================
// 唯一真源 = docs/USER_GUIDE.zh-CN.md 的 `## 硬约束` 小节里那一个 `> ⚠️` 块。
// 这段安全文本要落到 7 处:真源 1 处 + 本脚本生成的 6 处 ——
//   ① docs/USER_GUIDE.md 的 EN 块
//   ② README.md 的 Quick start(EN)
//   ③ README.zh-CN.md 的 快速上手(zh)
//   ④⑤⑥ web/shared/i18n.js 的 guide.title + guide.rule1..9(zh / en / fr)
// 12 §3.4 的判定是「手抄必漂」:既有机检(条目数 == 9、grep「六条」、check-i18n 的
// key 全等)只查得出数量与标题,查不出条目**文本**漂移。因此任何位置禁止手抄,
// 改红字只能改真源再跑本脚本;CI 与 gates 跑 --check 逐字节比对,不一致即非零退出。
//
// 译文的家:docs/hard-rules.i18n.json。en/fr 无法由 zh 机器推导,但也不能散落在
// 六个落地面上各写一份 —— 它们集中住在这一个文件里,并且每条带 zhSha256。zh 改了
// 而译文没跟上时,--check / --write 都会指名道姓地报「译文已过期」,而不是默默把
// 一条旧英文发到用户手册。fr 红字必须经人工审校后方可发布(05 §5:机翻安全警告
// 发到公开产品是明确禁止项),审校状态记在 JSON 的 frReview 字段。
//
// 用法:
//   node scripts/gen-hard-rules.mjs            # = --write,重写 6 处落地面
//   node scripts/gen-hard-rules.mjs --write
//   node scripts/gen-hard-rules.mjs --check    # 只比对,不落盘;漂移即 exit 1
//   node scripts/gen-hard-rules.mjs --help
//
// 零依赖(Node >= 18,ESM),与 check-i18n.mjs / check-bridge-parity.mjs 同规格。
// ======================================================================

import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

const SOURCE = "docs/USER_GUIDE.zh-CN.md";
const TRANSLATIONS = "docs/hard-rules.i18n.json";
const RULE_COUNT = 9;
const LANGS = ["zh", "en", "fr"];

const USAGE = [
    "用法: node scripts/gen-hard-rules.mjs [--write|--check] [--help]",
    "  九条硬约束红字的唯一生成器。真源 = docs/USER_GUIDE.zh-CN.md 的 `## 硬约束` 小节。",
    "  --write   (默认)把真源与 docs/hard-rules.i18n.json 生成到 6 处落地面。",
    "  --check   只比对不落盘;任一落地面与生成结果有一个字节不同即 exit 1。",
    "  --help    打印本说明并退出 0。",
].join("\n");

let mode = "write";
for (const a of process.argv.slice(2)) {
    if (a === "--write") mode = "write";
    else if (a === "--check") mode = "check";
    else if (a === "--help" || a === "-h") {
        console.log(USAGE);
        process.exit(0);
    } else {
        console.error("gen-hard-rules: 无法识别的参数 " + a);
        console.error(USAGE);
        process.exit(1);
    }
}

const errors = [];
const fail = (msg) => errors.push(msg);
const abs = (rel) => path.join(REPO, rel);
const sha256 = (s) =>
    crypto.createHash("sha256").update(s, "utf8").digest("hex");

function readOrDie(rel) {
    const p = abs(rel);
    if (!fs.existsSync(p)) {
        console.error("gen-hard-rules: 未找到 " + rel);
        process.exit(1);
    }
    return fs.readFileSync(p, "utf8");
}

// ---------------------------------------------------------------- 解析真源
// 真源块的形状(逐字):
//   ## 硬约束
//   （可有若干行说明）
//   > ⚠️ **<标题>**
//   >
//   > 1. <条目 1>
//   ...
//   > 9. <条目 9>
// 条目一律**单行**:它们要原样变成 i18n 的一个字符串值,允许换行等于允许两种排版
// 在 markdown 与 UI 之间产生不可比对的差异。多行条目在此直接报错,不做拼接猜测。
function parseSource(text) {
    const lines = text.split("\n");
    const head = lines.findIndex((l) => /^##\s+硬约束\s*$/.test(l));
    if (head < 0) {
        console.error(
            "gen-hard-rules: " + SOURCE + " 里找不到 `## 硬约束` 小节标题。",
        );
        console.error(
            "  该标题同时是 12 §4.4 / §7.3 对外链接的锚点 (#硬约束),不能改名或改层级。",
        );
        process.exit(1);
    }
    let start = -1;
    for (let i = head + 1; i < lines.length; i++) {
        if (/^##\s/.test(lines[i])) break;
        if (lines[i].startsWith("> ⚠️")) {
            start = i;
            break;
        }
    }
    if (start < 0) {
        console.error(
            "gen-hard-rules: `## 硬约束` 小节里找不到 `> ⚠️` 起头的红字块。",
        );
        process.exit(1);
    }
    let end = start;
    while (end + 1 < lines.length && lines[end + 1].startsWith(">")) end++;
    const block = lines.slice(start, end + 1);

    const titleLine = block[0];
    const m = /^>\s⚠️\s+\*\*(.+)\*\*\s*$/.exec(titleLine);
    if (!m) {
        console.error(
            "gen-hard-rules: 红字块首行必须是 `> ⚠️ **<标题>**`,实际为:\n  " +
                titleLine,
        );
        process.exit(1);
    }
    const title = m[1];

    const rules = [];
    for (const line of block.slice(1)) {
        const r = /^>\s(\d+)\.\s+(.+?)\s*$/.exec(line);
        if (r) rules.push({ n: Number(r[1]), md: r[2] });
    }
    if (rules.length !== RULE_COUNT) {
        console.error(
            "gen-hard-rules: 红字块条目数 = " +
                rules.length +
                ",要求恰好 " +
                RULE_COUNT +
                " 条(12 §3.4)。",
        );
        console.error(
            "  条目必须写成单行 `> N. 正文`;换行续写的条目不会被识别为一条。",
        );
        process.exit(1);
    }
    for (let i = 0; i < RULE_COUNT; i++) {
        if (rules[i].n !== i + 1) {
            console.error(
                "gen-hard-rules: 条目编号不连续,第 " +
                    (i + 1) +
                    " 条印作 " +
                    rules[i].n +
                    "。",
            );
            process.exit(1);
        }
    }
    // 标题必须写「九条」而不是「六条」(12 §3.4 已四处同改;这是唯一一处会同时
    // 出现在插件 UI、README 与官网的文案,写错一次就是四个面一起错)。
    if (!title.includes("九条") || title.includes("六条")) {
        console.error(
            "gen-hard-rules: 标题行必须逐字写「九条」且不得出现「六条」,实际为:\n  " +
                title,
        );
        process.exit(1);
    }
    return { title, rules };
}

// markdown → UI 纯文本:UI 侧的加重靠样式做(05 §7b 条 38),不靠 markdown 记号;
// 把 ** 与反引号原样送进 i18n 值,用户会在插件里看到一串星号。
function toPlain(md) {
    return (
        md
            .replace(/\*\*(.+?)\*\*/g, "$1")
            .replace(/`([^`]+)`/g, "$1")
            // 剥掉 ** 之后,markdown 里用来分隔「加重块」与后文的那个半角空格会留在中文
            // 句号后面(`……总线。** 不要把……` → `……总线。 不要把……`)。中文排版里
            // 句号后不跟空格,而九条红字在首启引导页是逐条全文显示的 —— 九条里有八条都
            // 带这个尾巴。
            //
            // 标点必须逐个写成**全角码点**,不能图省事写 `[。,、;:!?]` —— 那串里的
            // `,;:!?` 是 ASCII,会连法文的 `À lire : les neuf…` 一起吃掉那个空格,
            // 变成 `À lire :les neuf…`(法文排版里冒号前后本来就该有空格)。en/fr 用的
            // 全是 ASCII 标点,收窄到全角之后它们一律不受影响。
            //   U+3002 。 / U+FF0C , / U+3001 、 / U+FF1B ; / U+FF1A : / U+FF01 ! / U+FF1F ?
            .replace(/([。，、；：！？])[ \t]+/g, "$1")
    );
}

// ---------------------------------------------------------------- 载入译文
function loadTranslations(source) {
    let json;
    try {
        json = JSON.parse(readOrDie(TRANSLATIONS));
    } catch (e) {
        console.error("gen-hard-rules: " + TRANSLATIONS + " 不是合法 JSON");
        console.error("  " + (e && e.message ? e.message : String(e)));
        process.exit(1);
    }
    const stale = [];
    const check = (label, zhText, entry) => {
        if (!entry) {
            fail(TRANSLATIONS + " 缺少 " + label + " 条目");
            return;
        }
        for (const lang of ["en", "fr"]) {
            if (typeof entry[lang] !== "string" || entry[lang].trim() === "") {
                fail(
                    TRANSLATIONS + " 的 " + label + "." + lang + " 缺失或为空",
                );
            }
        }
        const want = sha256(zhText);
        if (entry.zhSha256 !== want) {
            stale.push(
                "  " +
                    label +
                    ": zhSha256=" +
                    String(entry.zhSha256).slice(0, 12) +
                    "… 实际=" +
                    want.slice(0, 12) +
                    "…",
            );
        }
    };
    check("title", source.title, json.title);

    // 译文这一侧的自校验,与 parseSource 对 zh 真源的严格程度对称。只做正向 byN 查找
    // 是查不出这三种病的:重复的 n 会被 Map 静默取最后一条(前一条译文永远用不到);
    // 多余的条目(n=10,或 zh 删到 8 条后残留的第 9 条)根本不被访问,--check 照样绿;
    // n 缺失或非数字只会表现成「缺少 ruleN 条目」,指不到真正的病灶。
    // 这个文件是九条红字唯一的译文之家,它自己漂了就是六个落地面一起漂。
    const ns = (json.rules || []).map((r) => r && r.n);
    if (
        ns.length !== RULE_COUNT ||
        new Set(ns).size !== ns.length ||
        ns.some((n, i) => n !== i + 1)
    ) {
        fail(
            TRANSLATIONS +
                " 的 rules 必须恰好 " +
                RULE_COUNT +
                " 条、n 从 1 连续到 " +
                RULE_COUNT +
                " 且不重复,实际 n = [" +
                ns.join(", ") +
                "]",
        );
    }

    const byN = new Map((json.rules || []).map((r) => [r.n, r]));
    for (const rule of source.rules)
        check("rule" + rule.n, rule.md, byN.get(rule.n));

    if (stale.length) {
        fail(
            "译文已过期 —— zh 真源改了但 " +
                TRANSLATIONS +
                " 没跟上:\n" +
                stale.join("\n") +
                "\n  处置:更新对应的 en/fr 译文,把 zhSha256 改成上面的「实际」值," +
                "并把 fr 重新交人工审校(frReview 字段)。",
        );
    }
    if (json.frReview && json.frReview.status !== "reviewed") {
        console.warn(
            "gen-hard-rules: [警告] fr 红字尚未人工审校(frReview.status=" +
                json.frReview.status +
                ")—— 发布前必须完成(05 §5 / 12 §1.2,审校人归属 U17)。",
        );
    }
    return { title: json.title, byN };
}

// ---------------------------------------------------------------- 渲染
function mdBlock(lang, source, tr) {
    const title = lang === "zh" ? source.title : tr.title[lang];
    const out = ["> ⚠️ **" + title + "**", ">"];
    for (const rule of source.rules) {
        const text = lang === "zh" ? rule.md : tr.byN.get(rule.n)[lang];
        out.push("> " + rule.n + ". " + text);
    }
    return out.join("\n");
}

// i18n.js 的生成区必须与 prettier 打成平手 —— 两道门禁同时管着这个文件:
// `npx prettier --check .`(gate 3)与本脚本的 --check。生成物只要有一处不是 prettier
// 的规范形状,两道门禁就会互相拆台:跑完 prettier 本脚本红,跑完本脚本 prettier 红,
// 而且没有任何一步能同时让两者绿。所以这里照抄 prettier 的两条判据:
//
//   ① 引号选择:默认双引号;但值里的**双引号比单引号多**时改用单引号(prettier 的
//      "fewer escapes wins" 规则)。九条红字里 rule3/5/8/9 都带成对引号,是真会踩到的。
//   ② 折行:`        "key": "value",` 一行放得下(printWidth 80)就不折,放不下才折成
//      两行、值缩进 12 空格。宽度按 prettier 的 getStringWidth 算 —— **CJK 全角字符宽 2**,
//      否则中文条目会被算成半宽而误判「放得下」。
//
// .editorconfig 的 indent_size=4 决定了 8 / 12 这两个缩进量。
const PRINT_WIDTH = 80;

// 全角判定:CJK 统一表意文字/兼容、假名、谚文、全角标点与全角形式 → 宽 2。
// 覆盖九条红字实际用到的字符集(中文正文 + 全角标点 + A–H 的 en dash 走半宽)。
function charWidth(cp) {
    if (
        (cp >= 0x1100 && cp <= 0x115f) || // 谚文字母
        (cp >= 0x2e80 && cp <= 0x303e) || // CJK 部首 / 汉字结构 / CJK 标点
        (cp >= 0x3041 && cp <= 0x33ff) || // 假名 / 谚文兼容 / CJK 兼容
        (cp >= 0x3400 && cp <= 0x4dbf) || // 扩展 A
        (cp >= 0x4e00 && cp <= 0x9fff) || // 统一表意文字
        (cp >= 0xa000 && cp <= 0xa4cf) || // 彝文
        (cp >= 0xac00 && cp <= 0xd7a3) || // 谚文音节
        (cp >= 0xf900 && cp <= 0xfaff) || // 兼容表意文字
        (cp >= 0xfe30 && cp <= 0xfe6f) || // CJK 兼容形式 / 小写变体
        (cp >= 0xff00 && cp <= 0xff60) || // 全角 ASCII 变体
        (cp >= 0xffe0 && cp <= 0xffe6) ||
        (cp >= 0x20000 && cp <= 0x3fffd) // 扩展 B+
    ) {
        return 2;
    }
    return 1;
}

function strWidth(s) {
    let w = 0;
    for (const ch of s) w += charWidth(ch.codePointAt(0));
    return w;
}

// prettier 的引号选择 + 转义:只转义定界引号与反斜杠。
function jsString(value) {
    const doubles = (value.match(/"/g) || []).length;
    const singles = (value.match(/'/g) || []).length;
    const q = doubles > singles ? "'" : '"';
    const escaped = value
        .replace(/\\/g, "\\\\")
        .replace(new RegExp(q, "g"), "\\" + q);
    return q + escaped + q;
}

function jsBlock(lang, source, tr) {
    const title = toPlain(lang === "zh" ? source.title : tr.title[lang]);
    const entries = [["guide.title", title]];
    for (const rule of source.rules) {
        const text = lang === "zh" ? rule.md : tr.byN.get(rule.n)[lang];
        entries.push(["guide.rule" + rule.n, toPlain(text)]);
    }
    const out = [];
    for (const [key, value] of entries) {
        const lit = jsString(value);
        const oneLine = '        "' + key + '": ' + lit + ",";
        if (strWidth(oneLine) <= PRINT_WIDTH) {
            out.push(oneLine);
        } else {
            out.push('        "' + key + '":');
            out.push("            " + lit + ",");
        }
    }
    return out.join("\n");
}

// ---------------------------------------------------------------- 落地面
const BEGIN = "BEGIN GENERATED hard-rules:";
const END = "END GENERATED hard-rules:";

function spliceRegion(fileRel, text, tag, body) {
    const beginRe = new RegExp("^.*" + BEGIN + tag + "\\b.*$", "m");
    const endRe = new RegExp("^.*" + END + tag + "\\b.*$", "m");
    const b = beginRe.exec(text);
    const e = endRe.exec(text);
    if (!b || !e || e.index < b.index) {
        fail(
            fileRel +
                " 找不到成对的生成区标记(" +
                BEGIN +
                tag +
                " … " +
                END +
                tag +
                ")",
        );
        return null;
    }
    const head = text.slice(0, b.index + b[0].length);
    const tail = text.slice(e.index);
    return head + "\n" + body + "\n" + tail;
}

function main() {
    const source = parseSource(readOrDie(SOURCE));
    const tr = loadTranslations(source);
    if (errors.length) {
        for (const e of errors) console.error("gen-hard-rules: " + e);
        process.exit(1);
    }

    const targets = [
        {
            file: "docs/USER_GUIDE.md",
            tag: "en",
            body: mdBlock("en", source, tr),
        },
        { file: "README.md", tag: "en", body: mdBlock("en", source, tr) },
        { file: "README.zh-CN.md", tag: "zh", body: mdBlock("zh", source, tr) },
        ...LANGS.map((lang) => ({
            file: "web/shared/i18n.js",
            tag: lang,
            body: jsBlock(lang, source, tr),
        })),
    ];

    // 同一文件有多个生成区(i18n.js 三个),按文件累积后一次落盘。
    const buffers = new Map();
    for (const t of targets) {
        if (!buffers.has(t.file)) buffers.set(t.file, readOrDie(t.file));
        const next = spliceRegion(t.file, buffers.get(t.file), t.tag, t.body);
        if (next !== null) buffers.set(t.file, next);
    }
    if (errors.length) {
        for (const e of errors) console.error("gen-hard-rules: " + e);
        process.exit(1);
    }

    let drift = 0;
    for (const [fileRel, next] of buffers) {
        const current = fs.readFileSync(abs(fileRel), "utf8");
        if (current === next) {
            if (mode === "check") console.log("  [OK]   " + fileRel);
            continue;
        }
        drift++;
        if (mode === "check") {
            console.error("  [DRIFT] " + fileRel + " 与真源生成结果不一致");
        } else {
            fs.writeFileSync(abs(fileRel), next);
            console.log("  [写入] " + fileRel);
        }
    }

    if (mode === "check") {
        if (drift > 0) {
            console.error("");
            console.error(
                "gen-hard-rules --check: " +
                    drift +
                    " 个落地面漂移。九条红字禁止在任何落地面手抄 —— " +
                    "改文案请只改 " +
                    SOURCE +
                    " 的 `## 硬约束` 小节(译文改 " +
                    TRANSLATIONS +
                    "),再跑 `node scripts/gen-hard-rules.mjs`。",
            );
            process.exit(1);
        }
        console.log(
            "gen-hard-rules --check: 6 个落地面与真源逐字节一致(9 条 × zh/en/fr)。",
        );
        process.exit(0);
    }

    console.log(
        drift === 0
            ? "gen-hard-rules: 6 个落地面已是最新,无需改动。"
            : "gen-hard-rules: 已重写 " + drift + " 个文件。",
    );
    process.exit(0);
}

main();
