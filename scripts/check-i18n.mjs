// SPDX-License-Identifier: GPL-3.0-or-later
// check-i18n.mjs —— i18n 词典门禁(零依赖,Node >= 18,ESM)。真源 = 05 §5 + 07 T27 卡。
//   词典契约(T27):web/shared/i18n.js 导出 `const T = {zh,en,fr}` 扁平字典、
//   `dict(lang)`(未知语言回退 zh)、`applyI18n(root, lang)`(刷 [data-t] 并同步 documentElement.lang)。
//   四项检查:
//     1. 三语 key 集合全等,且值一律是字符串(扁平字典,不允许嵌套对象);
//     2. web/**/*.html 里每个 data-t / data-t-<attr> 的 key 必须在字典中;
//     3. 死 key —— 字典 key 未被 web/**/*.{html,js} 引用;
//     4. guide.rule1..9 三语存在、非空、非占位符(05 §5 判据)。
//   用法:
//     node scripts/check-i18n.mjs [--strict-unused] [--skip-guide-rules] [--help]
//   退出码:任一失败 = 1;只有警告仍为 0。
//
//   为什么死 key 默认只警告(--strict-unused 才转失败):T27 交付的是基础层,消费者页面
//   (web/output、web/input)要到 T27b/T31-T36 才落地,字典先行时必然「满屏死 key」。
//   等页面齐了,CI 侧再固定带上 --strict-unused。同理 data-t 引用 0 处不算失败。
//   为什么 guide.rule1..9 可跳过(--skip-guide-rules):这九条红字是 scripts/gen-hard-rules.mjs
//   从唯一真源 docs/USER_GUIDE.zh-CN.md#硬约束 生成写入 i18n.js 的生成物,05 §0.1/§5 明令
//   「任何位置禁止手抄」;而生成器与 USER_GUIDE 要到 T31 才落地,过渡期只能跳过。
//   T31 之后 CI 必须去掉该 flag,并与 `node scripts/gen-hard-rules.mjs --check` 一起跑。
//   05 §5 占位符判据三「纯 ASCII 而 zh 非纯 ASCII」本脚本只对 fr 生效:英文本身就是纯 ASCII,
//   对 en 套用该判据会把每一条合格英文都判成占位符(判据的实际靶子是「fr 里照抄了英文」)。

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const USAGE = [
    "用法: node scripts/check-i18n.mjs [--strict-unused] [--skip-guide-rules] [--help]",
    "  校验 web/shared/i18n.js:三语 key 全等、HTML 的 data-t 全部存在、死 key、九条红字非占位符。",
    "  --strict-unused      把「死 key」从警告升级为失败(消费者页面落地后在 CI 打开)。",
    "  --skip-guide-rules   跳过 guide.rule1..9 断言(gen-hard-rules.mjs 于 T31 落地前的过渡)。",
    "  --help, -h           打印本说明并退出 0。",
].join("\n");

const flags = { strictUnused: false, skipGuideRules: false };
for (const a of process.argv.slice(2)) {
    if (a === "--strict-unused") flags.strictUnused = true;
    else if (a === "--skip-guide-rules") flags.skipGuideRules = true;
    else if (a === "--help" || a === "-h") {
        console.log(USAGE);
        process.exit(0);
    } else {
        console.error("check-i18n: 无法识别的参数 " + a);
        console.error(USAGE);
        process.exit(1);
    }
}

const nodeMajor = Number(process.versions.node.split(".")[0]);
if (nodeMajor < 18) {
    console.error(
        "check-i18n: 需要 Node >= 18(当前 " + process.versions.node + ")",
    );
    process.exit(1);
}

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const WEB = path.join(REPO, "web");
const I18N = path.join(WEB, "shared", "i18n.js");
const LANGS = ["zh", "en", "fr"];
const GUIDE_KEYS = Array.from({ length: 9 }, (_, i) => "guide.rule" + (i + 1));
// 引用扫描不进这两处:JUCE 官方 helper 是第三方代码,fonts 是二进制。
const SKIP_RELDIRS = new Set(["web/js/juce", "web/fonts"]);

const rel = (p) => path.relative(REPO, p).replace(/\\/g, "/");

const errors = [];
const warnings = [];
const fail = (msg) => errors.push(msg);
const warn = (msg) => warnings.push(msg);

// ---------------------------------------------------------------- 载入词典
if (!fs.existsSync(I18N)) {
    console.error("check-i18n: 未找到词典文件 " + rel(I18N));
    console.error(
        "  该文件是 T27 交付物,契约为 export const T = { zh, en, fr } + dict(lang) + applyI18n(root, lang)。",
    );
    console.error(
        "  若并行开发中它尚未落地,等文件出现后重跑;否则确认当前分支是否 feat/T27-web-shared,或文件是否被误移。",
    );
    process.exit(1);
}

let mod;
try {
    mod = await import(pathToFileURL(I18N).href);
} catch (e) {
    console.error("check-i18n: 导入词典失败 " + rel(I18N));
    console.error("  " + (e && e.message ? e.message : String(e)));
    console.error(
        "  常见原因:语法错误,或模块顶层直接碰了 document/window(词典模块必须无顶层副作用,才能在 Node 里被门禁导入)。",
    );
    process.exit(1);
}

const T = mod.T;
if (!T || typeof T !== "object") {
    console.error(
        "check-i18n: " +
            rel(I18N) +
            " 未导出 T(应为 export const T = { zh, en, fr })",
    );
    process.exit(1);
}
for (const lang of LANGS) {
    if (!T[lang] || typeof T[lang] !== "object") {
        console.error(
            "check-i18n: T." + lang + " 缺失或不是对象(三语必须齐备)",
        );
        process.exit(1);
    }
}
// dict/applyI18n 属同一份契约,消费者页面按它写;缺了不阻断后续检查,但计为失败。
for (const fn of ["dict", "applyI18n"]) {
    if (typeof mod[fn] !== "function") {
        fail(
            "词典未导出 " +
                fn +
                "()(T27 契约:dict(lang) + applyI18n(root, lang))",
        );
    }
}

// ------------------------------------------------- 检查 1:key 集合与值形状
const keySets = {};
for (const lang of LANGS) keySets[lang] = new Set(Object.keys(T[lang]));
const allKeys = new Set(LANGS.flatMap((l) => [...keySets[l]]));

const list = (arr, cap = 12) =>
    arr.length <= cap
        ? arr.join(", ")
        : arr.slice(0, cap).join(", ") + " …(共 " + arr.length + " 个)";

for (const lang of LANGS) {
    const missing = [...allKeys].filter((k) => !keySets[lang].has(k)).sort();
    if (missing.length > 0) {
        fail(lang + " 缺 " + missing.length + " 个 key: " + list(missing));
    }
}

for (const lang of LANGS) {
    for (const [k, v] of Object.entries(T[lang])) {
        if (typeof v !== "string") {
            fail(
                lang +
                    "." +
                    k +
                    " 的值不是字符串(约定为扁平字典,嵌套结构一律拍平成带前缀的 key)",
            );
        } else if (v.trim() === "") {
            warn(lang + "." + k + " 为空字符串");
        }
    }
}

// ------------------------------------------------------------ 扫描 web/ 文件
function walk(dir, exts, out = []) {
    let entries;
    try {
        entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
        return out;
    }
    for (const ent of entries) {
        const full = path.join(dir, ent.name);
        if (ent.isDirectory()) {
            if (ent.name === "node_modules" || ent.name.startsWith("."))
                continue;
            if (SKIP_RELDIRS.has(rel(full))) continue;
            walk(full, exts, out);
        } else if (
            ent.isFile() &&
            exts.includes(path.extname(ent.name).toLowerCase())
        ) {
            out.push(full);
        }
    }
    return out;
}

const webExists = fs.existsSync(WEB);
const htmlFiles = webExists ? walk(WEB, [".html", ".htm"]) : [];
const jsFiles = webExists ? walk(WEB, [".js", ".mjs"]) : [];

// ------------------------------------------- 检查 2:HTML 的 data-t 必须有词条
// 只认 data-t="key" 与 data-t-aria="key" —— 这两种正是 i18n.js 的 applyI18n 真正实现的选择器。
// 刻意不放宽成 data-t-<任意属性>:门禁若认了 data-t-title/data-t-placeholder,页面写了会全绿,
// 而运行期该属性永远不被翻译、切语言静默保持原文。要新增属性,先改 applyI18n,再同步放宽本正则。
const DATA_T_RE = /\bdata-t(?:-aria)?\s*=\s*(?:"([^"]*)"|'([^']*)')/g;
let dataTCount = 0;
for (const file of htmlFiles) {
    const text = fs.readFileSync(file, "utf8");
    let m;
    DATA_T_RE.lastIndex = 0;
    while ((m = DATA_T_RE.exec(text)) !== null) {
        const key = (m[1] ?? m[2] ?? "").trim();
        if (key === "") continue;
        dataTCount += 1;
        if (!allKeys.has(key)) {
            const line = text.slice(0, m.index).split("\n").length;
            fail(
                rel(file) +
                    ":" +
                    line +
                    " 引用了词典里不存在的 key「" +
                    key +
                    "」",
            );
        }
    }
}

// ------------------------- 检查 2b:HTML 内联回退文案必须与 zh 词条逐字相同
//
// [SL-293] 为什么要有这条:`applyI18n` 在运行期会用词条覆盖 `<span data-t=...>` 的静态文本,
// 所以内联那份只是**首帧回退**——正因为「稳态看不出来」,它漂了没有任何东西会红。
// 实测代价:一次扫出 33 处不一致,其中 12 处是**词条偏离了 05 规格**、内联反而是对的
// (`master.leadSelectHint` 把「音量豁免为独立选项,不随此联动」整句丢了;
//  `tracks.colLegend` 的「音量」列语义被写反;`out.master.writeConfirm` 把「30 条车道」
//  写成「30 条轨道」——只有 15 轨,车道才是 30)。这类错**用户看得见**,而首帧那一瞬
//  还会闪一下另一份文案。
//
// 判据方向:**以 zh 词条为准**(它是上屏那一份),内联必须逐字等于它。
// 只比中文内联:纯 ASCII 的占位文本(如 `0 dB`、`{v}`)与空元素不参与。
// ★ 删之即红:把任一页某个 data-t 元素的内联文字**改一个字**,本条当场红。
//   边界(复审③要求写清,免得后人误信这句话的覆盖面):把内联文字**整个删空**
//   (`>正文</span>` → `></span>`)**不会**红 —— 它走下面 `inline === ""` 那条跳过分支。
//   这不是缺陷:空元素本来就是「运行期由 applyI18n 填」的合法写法,断它反而误杀。
//   本判据管的是「内联里写了中文、但写得跟词条不一样」,不是「内联必须有中文」。
const INLINE_RE = /data-t="([\w.]+)"[^>]*>([^<]{1,600})</g;
const NL = String.fromCharCode(10);
const norm = (s) => String(s).replace(/\s+/g, " ").trim();
let inlineChecked = 0;
for (const file of htmlFiles) {
    const text = fs.readFileSync(file, "utf8");
    let m;
    INLINE_RE.lastIndex = 0;
    while ((m = INLINE_RE.exec(text)) !== null) {
        const key = m[1];
        const inline = norm(m[2]);
        if (inline === "" || !/[一-鿿]/.test(inline)) continue;
        const want = T.zh ? T.zh[key] : undefined;
        if (want === undefined) continue; // 缺 key 由检查 2 报
        inlineChecked += 1;
        if (inline !== norm(want)) {
            const line = text.slice(0, m.index).split(NL).length;
            fail(
                rel(file) +
                    ":" +
                    line +
                    " 内联回退文案与 zh 词条不一致「" +
                    key +
                    "」 词条=" +
                    norm(want) +
                    " / 内联=" +
                    inline,
            );
        }
    }
}

// ★ [SL-293 复审③] 静默零命中要红 —— 检查 2b 有两条**不报错的退出口**:
//   ① 内联正文超 600 字(`[^<]{1,600}` 整体失配);
//   ② data-t 元素里嵌了子元素(`>` 之后第一个字符就是 `<`)。
// 两种情形都是**整条跳过、不计数、不报**。今天三页一处都没有(legend 的 `<strong>` 是
// applyI18n 之后由 JS 重建的,静态 HTML 里没有),但门禁的作用期是「以后」:重排一次 HTML
// 就可能把比对面悄悄清零,而汇总行照样绿 —— 本仓在 sccache 上栽过同一形态。
// 故 ①把实际比对条数打进汇总行(人一眼看得见它有没有塌),②钉一条下界当机检。
// 下界取 100 是**留足增删余量的下限**、不是当前值(当前 150+);它拦的是「塌成个位数或零」
// 这种量级事故,不是正常增删几条词条。
//
// 另记两条**本判据的维护面**(方向都安全 —— 会假红或漏检,不会静默变绿):
//   • 内联若用 HTML 实体写 CJK 标点(`&times;` 之类)而词条是字面字符 ⇒ 假红;
//   • 空白归一只压缩空白、不删 token 中间的换行 ⇒ 折行必须落在词间自然空白处。
// 还有一条**本判据管不到**的:JS 里 `t.someKey || "字面量"` 这一族兜底是词条的**第三份拷贝**,
// 2b 只认 HTML 的 `data-t=` ⇒ 扫不到(本卡就在 tab-master.js:1877/1933 逮到两处漏网的旧词)。
const INLINE_MIN = 100;
if (webExists && htmlFiles.length > 0 && inlineChecked < INLINE_MIN) {
    fail(
        "检查 2b 实际只比对了 " +
            inlineChecked +
            " 处内联(下界 " +
            INLINE_MIN +
            ")—— 比对面塌了。多半不是内联真的少了,而是 HTML 结构变化让 INLINE_RE 整体失配:" +
            "先查 data-t 元素里是否新嵌了子元素、或正文是否超过 600 字。",
    );
}

// --------------------------------------------------------- 检查 3:死 key
// 引用判据(宽松,宁可漏判也不误杀):引号字符串字面量、模板串、`.ident` 属性访问;
// 拼接式取词(`"guide.rule" + n`、带插值的 `tour.step${i}`)按字面前缀登记,避免整组 key 被误报为死 key。
const refs = new Set();
const prefixes = [];
function collectRefs(text) {
    const addLiteral = (s) => {
        if (typeof s !== "string" || s === "") return;
        refs.add(s);
        if (s.endsWith(".") && s.length >= 3) prefixes.push(s);
    };
    let m;
    const quoted = /"([^"\n]{0,200})"|'([^'\n]{0,200})'/g;
    while ((m = quoted.exec(text)) !== null) addLiteral(m[1] ?? m[2]);
    const tpl = /`([^`]*)`/g;
    while ((m = tpl.exec(text)) !== null) {
        const body = m[1];
        if (!body.includes("${")) {
            addLiteral(body);
            continue;
        }
        const head = body.slice(0, body.indexOf("${"));
        if (head.length >= 3) prefixes.push(head);
    }
    const concat = /(["'])([^"'\n]{1,160})\1\s*\+/g;
    while ((m = concat.exec(text)) !== null) {
        if (m[2].length >= 3) prefixes.push(m[2]);
    }
    const prop = /\.([A-Za-z_$][\w$]*)/g;
    while ((m = prop.exec(text)) !== null) refs.add(m[1]);
}

for (const file of [...htmlFiles, ...jsFiles]) {
    // 词典自身当然「引用」了全部 key,必须排除,否则死 key 检查永远通过。
    if (path.resolve(file) === path.resolve(I18N)) continue;
    collectRefs(fs.readFileSync(file, "utf8"));
}

const isReferenced = (k) =>
    refs.has(k) || prefixes.some((p) => k.startsWith(p) && k.length > p.length);
const deadKeys = [...allKeys].filter((k) => !isReferenced(k)).sort();
if (deadKeys.length > 0) {
    const msg =
        "死 key " +
        deadKeys.length +
        " 个(未被 web/**/*.{html,js} 引用): " +
        list(deadKeys);
    if (flags.strictUnused) fail(msg);
    else
        warn(
            msg +
                " —— 消费者页面(T27b/T31-T36)未落地时属预期;--strict-unused 可升级为失败",
        );
}

// ------------------------------------------ 检查 4:guide.rule1..9 非占位符
const PLACEHOLDER_TOKEN = /todo|tbd|xxx|fixme/i;
const isAscii = (s) => !/[^\x00-\x7f]/.test(s);

if (flags.skipGuideRules) {
    console.log(
        "check-i18n: 已跳过 guide.rule1..9 断言(--skip-guide-rules)—— 九条红字是 scripts/gen-hard-rules.mjs",
    );
    console.log(
        "  从 docs/USER_GUIDE.zh-CN.md#硬约束 生成的产物(05 §0.1/§5:任何位置禁止手抄),生成器与真源均待 T31 落地;",
    );
    console.log("  T31 之后 CI 必须去掉该 flag。");
} else {
    for (const key of GUIDE_KEYS) {
        const vals = {};
        let ok = true;
        for (const lang of LANGS) {
            const v = T[lang][key];
            if (typeof v !== "string" || v.trim() === "") {
                fail(
                    lang +
                        "." +
                        key +
                        " 缺失或为空(九条红字三语必须齐备,来源 gen-hard-rules.mjs)",
                );
                ok = false;
                continue;
            }
            vals[lang] = v;
            if (PLACEHOLDER_TOKEN.test(v)) {
                fail(lang + "." + key + " 含占位符标记(TODO/TBD/xxx/FIXME)");
            }
        }
        if (!ok) continue;
        if (isAscii(vals.zh)) {
            fail("zh." + key + " 为纯 ASCII —— 中文红字不含中文,判定为占位符");
        }
        for (const lang of ["en", "fr"]) {
            if (vals[lang] === vals.zh) {
                fail(lang + "." + key + " 与 zh 逐字相同(05 §5 占位符判据一)");
            }
        }
        if (vals.fr === vals.en) {
            fail(
                "fr." +
                    key +
                    " 与 en 逐字相同 —— fr 红字必须经人工审校,机翻/照抄英文不得发布(05 §5)",
            );
        }
        if (isAscii(vals.fr) && !isAscii(vals.zh)) {
            fail(
                "fr." +
                    key +
                    " 为纯 ASCII 而 zh 非纯 ASCII(05 §5 占位符判据三)",
            );
        }
    }
}

// -------------------------------- 附加警告:三语占位符集合应一致({n}/{name}…)
const PH_RE = /\{[A-Za-z_][A-Za-z0-9_]*\}/g;
for (const key of allKeys) {
    const sets = {};
    let complete = true;
    for (const lang of LANGS) {
        const v = T[lang][key];
        if (typeof v !== "string") {
            complete = false;
            break;
        }
        sets[lang] = [...new Set(v.match(PH_RE) || [])].sort().join(",");
    }
    if (!complete) continue;
    if (sets.zh !== sets.en || sets.zh !== sets.fr) {
        warn(
            "key「" +
                key +
                "」三语占位符不一致: zh[" +
                sets.zh +
                "] en[" +
                sets.en +
                "] fr[" +
                sets.fr +
                "]",
        );
    }
}

// ---------------------------------------------------------------- 汇总输出
if (!webExists) {
    console.log(
        "check-i18n: 未发现 web/ 目录,data-t 与死 key 扫描均按 0 处计。",
    );
} else if (htmlFiles.length === 0) {
    console.log(
        "check-i18n: web/ 下无 HTML,data-t 引用 0 处(消费者页面未落地,不算失败)。",
    );
}

for (const w of warnings) console.log("  [WARN] " + w);

if (errors.length > 0) {
    console.error("");
    console.error("check-i18n 失败(" + errors.length + " 项):");
    for (const e of errors) console.error("  [FAIL] " + e);
    console.error(
        "  真源 = 05 §5(词条逐字)、07 T27 卡;词典 = " + rel(I18N) + "。",
    );
    process.exit(1);
}

console.log(
    "check-i18n 通过: " +
        allKeys.size +
        " 个 key × 3 语;data-t 引用 " +
        dataTCount +
        " 处(HTML " +
        htmlFiles.length +
        " 个 / JS " +
        jsFiles.length +
        " 个);内联回退比对 " +
        inlineChecked +
        " 处;警告 " +
        warnings.length +
        " 条",
);
process.exit(0);
