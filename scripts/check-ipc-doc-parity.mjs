// SPDX-License-Identifier: GPL-3.0-or-later
// ======================================================================
// check-ipc-doc-parity.mjs —— IPC 冻结契约「文档 ↔ golden ↔ 头文件」机器对拍(C7)
// ======================================================================
// 为什么有这张脚本:#75(T39a)把 docs/IPC_CONTRACT.md 转正时,文档里的结构体
// 偏移/大小与实现漂移,靠人审来回修了三轮才收口。人眼逐个数 offset 是查不住的
// ——「16..76」「76..136」这种数字错一位,评审 100% 看不出来,而它会让第三方按
// 文档实现的读方读到错位的字段。本脚本把这类漂移变成机器可检。
//
// ----------------------------------------------------------------------
// 一、真源链(方向不可颠倒)
// ----------------------------------------------------------------------
//   ① 代码(编译期真值)  src/core/ipc/SegmentLayout.h、CtrlPlane.h
//        ↓ tests/core/test_ipc_layout.cpp 逐行 offsetof/sizeof/alignof 断言
//   ② golden             tests/golden/ipc-layout.txt      ← **本脚本的权威侧**
//        ↓ 本脚本
//   ③ 文档               docs/IPC_CONTRACT.md             ← 被检侧
//
// ②↔① 这一环已由 C++ 测试用**编译期真值**钉死(golden 不是手抄的数字,任何与
// 编译器算出的 offsetof/sizeof 不符的行都会让测试红),所以本脚本不需要再造一个
// 「从编译产物生成 golden」的生成器 —— 那一环已经存在且更强(编译器算的,不是
// 正则抠注释抠来的)。本脚本补的是链条上唯一还靠人肉维护的一环:golden → 文档。
//
// **以代码/golden 为真**:发现不一致时,脚本一律按「文档错了」报,不反过来改
// golden。文档是冻结契约(CONTRIBUTING.md §8),真要改文档得走 §5 演进流程。
//
// ----------------------------------------------------------------------
// 二、①→② 的完备性补洞(为什么还要读头文件)
// ----------------------------------------------------------------------
// C++ 测试是「golden 每一行都能在代码里找到对应真值」,不是「代码每个字段都在
// golden 里」—— 往结构体里塞一个字段而不更新 golden,只要 sizeof 没变(塞进
// padding 里)测试仍然全绿。所以本脚本额外读头文件,只做**字段集合与顺序**的
// 完备性比对(不从头文件读 offset:头文件里的 offset 也只是注释,不是真值;
// offset 的真值在 C++ 测试那边)。三侧字段表必须逐项相等。
// 同理,**新增一整个结构体**却不冻 golden 也是全绿的(golden 里没有它,只走
// 「golden → 头文件」方向的循环压根碰不到它),故完备性比对**双向**走:头文件里
// 不在 golden 的结构体,必须在脚本的 NON_LAYOUT_STRUCTS 白名单里登记为「不进段」。
//
// ----------------------------------------------------------------------
// 三、检查项
// ----------------------------------------------------------------------
//   A 结构体集合      文档 ```c 围栏里的结构体集合 == golden 结构体集合
//   B 字段名与顺序    每个结构体逐项相等(文档 / golden / 头文件 三侧)
//   C 字段偏移        文档注释里标注的 offset == golden offset
//   D 结构体尺寸对齐  文档注释 `size N align M` / `alignas(K)` == golden
//   E 字段跨度自洽    文档 `a..b` 区间的 b == 下一字段 offset(末字段 == size),
//                     且 b == a + 元素数 × sizeof(元素类型)
//   F 常量            abi / magic / max_channels / max_groups / ring_frames /
//                     capacity_hops / 段预算 —— 文档写的数 == golden 冻结值
//   G 段名模板        文档段名模板代入 {G}/{N} == golden 逐条段名
//   H registry 落点   §1 的 InputSlot@64 / OutputSlot@1024 = 64+15×64 算术自洽
//                     且与 golden 的结构体尺寸 / max_channels / 段预算相符
//   I ctrl 落点       §4 的 CtrlHeader@0 → 广播区@64 → OutputGlobalInfo@9408 →
//                     CtrlRing×15@9664 = 16384 全链算术,且与头文件 kCtrl* 常量相符
//
// 判级(两条原则并列,同一个理由的两种形态):
//
//   原则一(原作者,#85/C7)——「文档没标注」= [WARN]:
//     不一致 = [FAIL](exit 1);文档没标注 = [WARN](不影响退出码)。
//     理由:文档是冻结件,本脚本无权要求它补标注 —— 缺标注的项要走 §5 补,在补上之前把
//     gates 卡红等于逼人改冻结文档。`--strict` 把 WARN 提成 FAIL,供文档补齐标注后收紧
//     (届时把 gates 里的调用加上 --strict 即可)。
//
//   原则二(#89/T44 扩展)——「修宪在途」= [WARN],**但钥匙必须在案**:
//     一个结构体 / 段名可以暂时只在 golden 而不在文档里,**当且仅当** docs/contract-changes/
//     下有一份变更文档点名了它;否则仍然 [FAIL]。
//     理由:原则一只覆盖了「已在文档里、但少标一个 offset」,没覆盖「整个段已实现、修宪尚未
//     执行」——而后者正是 CONTRIBUTING.md §8 规定的**正常**状态(变更文档同 PR +
//     status/frozen-contract 标签 + 用户批准 → **之后**才由统筹转正进冻结文档)。没有这条,
//     新增段的 PR 只有两条路:违规先改冻结文档,或永远无法让 gates 变绿 —— 两条都不该走。
//     为什么钥匙选「变更文档在案」而不是命令行开关:变更文档是**仓内自包含**的证据
//     (本地 gates 拿不到 PR 标签)、是评审看得见、用户批准时要读的东西,而且**可审计**——
//     滥用它得先伪造一份变更文档。转正之后结构体进了文档,检查自动回到正向对拍,通道不再触发。
//     实现见 isPendingRatification();反向验证:移走变更文档 → 相关项当场全红 exit 1。
//
// 用法:
//   node scripts/check-ipc-doc-parity.mjs             # 对拍,漂移即 exit 1
//   node scripts/check-ipc-doc-parity.mjs --verbose   # 附带逐项通过明细
//   node scripts/check-ipc-doc-parity.mjs --strict     # WARN 也算失败
//   node scripts/check-ipc-doc-parity.mjs --help
//
// 零依赖(Node >= 18,ESM),与 check-i18n.mjs / check-bridge-parity.mjs 同规格。
// ======================================================================

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

const DOC = "docs/IPC_CONTRACT.md";
const GOLDEN = "tests/golden/ipc-layout.txt";
const HEADERS = [
    "src/core/ipc/SegmentLayout.h",
    "src/core/ipc/CtrlPlane.h",
    "src/core/ipc/VizPlane.h", // viz 段(T44/J75):结构体与常量定义在这里
];

const USAGE = [
    "用法: node scripts/check-ipc-doc-parity.mjs [--verbose] [--strict] [--help]",
    "  IPC 冻结契约文档 ↔ golden ↔ 头文件 的机器对拍。",
    "  真源方向:代码 → tests/golden/ipc-layout.txt → docs/IPC_CONTRACT.md。",
    "  --verbose  打印逐项通过明细(默认只打印失败与警告)。",
    "  --strict   把「文档未标注」类警告提升为失败。",
    "  --help     打印本说明并退出 0。",
].join("\n");

let verbose = false;
let strict = false;
for (const a of process.argv.slice(2)) {
    if (a === "--verbose" || a === "-v") verbose = true;
    else if (a === "--strict") strict = true;
    else if (a === "--help" || a === "-h") {
        console.log(USAGE);
        process.exit(0);
    } else {
        console.error("check-ipc-doc-parity: 无法识别的参数 " + a);
        console.error(USAGE);
        process.exit(1);
    }
}

const errors = [];
const warnings = [];
const passes = [];
const fail = (msg) => errors.push(msg);
const warn = (msg) => warnings.push(msg);
const pass = (msg) => passes.push(msg);

function readOrDie(rel) {
    const abs = path.join(REPO, rel);
    if (!fs.existsSync(abs)) {
        console.error("check-ipc-doc-parity: 找不到 " + rel);
        console.error("  本脚本必须能读到全部三侧文件,缺一侧即硬失败;");
        console.error("  「文件不在就跳过」会让对拍一次没跑却判绿。");
        process.exit(1);
    }
    return fs.readFileSync(abs, "utf8");
}

// ------------------------------------------------- 「修宪在途」豁免(doc-backed)
// 一个结构体 / 段名可以**暂时**只在 golden 而不在 docs/IPC_CONTRACT.md 里 ——
// **当且仅当** docs/contract-changes/ 下有一份变更文档点名了它。
//
// 为什么需要这个:本脚本的判级理由(见文件头)是「文档是冻结件,本脚本无权要求它补标注,
// 在补上之前把 gates 卡红等于逼人改冻结文档」。但当初只给「缺标注」留了 WARN 通道,
// 没给「整个段已实现、修宪尚未执行」留 —— 而后者正是 CONTRIBUTING.md §8 规定的正常状态:
// 变更文档同 PR + status/frozen-contract 标签 + 用户批准 → **之后**才由统筹转正进冻结文档。
// 没有这个通道,新增段的 PR 只有两条路:要么违规先改冻结文档,要么永远无法让 gates 变绿。
//
// 为什么这不是「放宽」:豁免的钥匙是**变更文档里出现这个名字**,而变更文档是评审看得见、
// 用户批准时要读的东西。真正的文档漂移(改了实现没走流程)拿不到这把钥匙,仍然 FAIL。
// 转正之后结构体进了 IPC_CONTRACT.md,检查自动回到正向对拍,豁免不再被触发。
function loadPendingRatification() {
    const dir = path.join(REPO, "docs", "contract-changes");
    if (!fs.existsSync(dir)) return "";
    return fs
        .readdirSync(dir)
        .filter((f) => f.endsWith(".md") && f !== "TEMPLATE.md")
        .map((f) => fs.readFileSync(path.join(dir, f), "utf8"))
        .join("\n");
}
const PENDING_TEXT = loadPendingRatification();
// 名字出现在任一变更文档里即视为「修宪在途」。用词边界避免 Viz 撞 VizLanes 这类子串误判。
function isPendingRatification(name) {
    if (!PENDING_TEXT || !name) return false;
    const isWordChar = (c) => c !== "" && /[A-Za-z0-9_]/.test(c);
    // 按词边界找,不用正则拼装 —— 名字里可能含 `.` `{` `}`(段名模板如
    // SynchainSCVB.v1.g{G}.viz),拼进正则要转义,转义写错会静默放行。
    for (let from = 0; ;) {
        const i = PENDING_TEXT.indexOf(name, from);
        if (i < 0) return false;
        const before = i === 0 ? "" : PENDING_TEXT[i - 1];
        const after = PENDING_TEXT[i + name.length] || "";
        if (!isWordChar(before) && !isWordChar(after)) return true;
        from = i + 1;
    }
}

// ---------------------------------------------------------------- golden 解析
// golden 行类型见 tests/core/test_ipc_layout.cpp:
//   <scalar> <value> | name g{G} {kind} <logical> |
//   struct <S> size <N> align <M> | field <S>.<f> offset <N> |
//   offset <blockKey> <N>(段内区块偏移,viz 段引入)
function parseGolden(text) {
    const scalars = new Map();
    const names = [];
    const structs = new Map();
    const blockOffsets = new Map();
    let lineNo = 0;
    for (const raw of text.split("\n")) {
        lineNo += 1;
        const line = raw.replace(/\r$/, "").trim();
        if (line === "" || line.startsWith("#")) continue;
        const t = line.split(/\s+/);
        if (t[0] === "name" && t.length === 4) {
            names.push({ group: t[1], kind: t[2], logical: t[3] });
        } else if (t[0] === "struct" && t.length === 6) {
            structs.set(t[1], {
                size: Number(t[3]),
                align: Number(t[5]),
                fields: [],
            });
        } else if (t[0] === "field" && t.length === 4) {
            const dot = t[1].indexOf(".");
            const s = t[1].slice(0, dot);
            const f = t[1].slice(dot + 1);
            const entry = structs.get(s);
            if (!entry) {
                fail(
                    GOLDEN +
                        ":" +
                        lineNo +
                        " field 行的结构体 " +
                        s +
                        " 没有对应的 struct 行",
                );
                continue;
            }
            entry.fields.push({ name: f, offset: Number(t[3]) });
        } else if (t[0] === "offset" && t.length === 3) {
            // 段内区块偏移(viz 段引入):`offset <key> <n>`。真值由 test_ipc_layout.cpp
            // 对拍 C++ 的 kViz*Offset 常量;本脚本只需认得这行、不重复校验。
            blockOffsets.set(t[1], Number(t[2]));
        } else if (t.length === 2) {
            scalars.set(t[0], t[1]);
        } else {
            fail(GOLDEN + ":" + lineNo + " 无法解析的 golden 行: " + line);
        }
    }
    return { scalars, names, structs, blockOffsets };
}

// ------------------------------------------------- C 风格结构体解析(文档/头文件共用)
// 兼容两种大括号风格:文档是 `struct X {   // 注释`(K&R),头文件是
// clang-format 的 Allman(`struct alignas(64) X` 换行再 `{`)。
const RE_STRUCT_OPEN =
    /^\s*struct\s+(?:alignas\((\w+)\)\s+)?([A-Za-z_]\w*)\s*(\{)?\s*(?:\/\/\s*(.*?))?\s*$/;
const RE_STRUCT_CLOSE = /^\s*\};/;
const RE_FIELD =
    /^\s*(?!struct\b|return\b|using\b)([A-Za-z_][\w:]*(?:<[^>]+>)?)\s+([A-Za-z_]\w*)\s*((?:\[[^\]]*\])+)?\s*;\s*(?:\/\/\s*(.*?))?\s*$/;

// 注释里的偏移标注:`0   = kScvbMagic` → {offset:0};`16..76   计数导出` → {0:16,end:76}
function parseOffsetComment(comment) {
    if (!comment) return null;
    const m = /^(\d+)(?:\s*\.\.\s*(\d+))?\b/.exec(comment.trim());
    if (!m) return null;
    return {
        offset: Number(m[1]),
        end: m[2] === undefined ? null : Number(m[2]),
    };
}

function parseStructs(text, source) {
    const out = new Map();
    const lines = text.split("\n");
    let cur = null; // 已进入 { } 的结构体
    let pending = null; // 见到 struct 名但 { 还在下一行
    for (let i = 0; i < lines.length; i += 1) {
        const line = lines[i].replace(/\r$/, "");
        if (cur) {
            if (RE_STRUCT_CLOSE.test(line)) {
                out.set(cur.name, cur);
                cur = null;
                continue;
            }
            const f = RE_FIELD.exec(line);
            if (f) {
                const off = parseOffsetComment(f[4]);
                const dims =
                    f[3] === undefined
                        ? null
                        : [...f[3].matchAll(/\[([^\]]*)\]/g)].map((m) =>
                              m[1].trim(),
                          );
                cur.fields.push({
                    name: f[2],
                    type: f[1],
                    // 多维数组(如 bits[15][32])也要被「看见」—— 否则往结构体里加一个二维
                    // 成员,字段集合完备性比对压根碰不到它,金 golden 漏冻也全绿。
                    // 多维时 arrayLen 是非数字串,下游的跨度自洽(检查 E)会自行跳过。
                    arrayLen:
                        dims === null
                            ? null
                            : dims.length === 1
                              ? dims[0]
                              : dims.join("x"),
                    offset: off ? off.offset : null,
                    end: off ? off.end : null,
                    line: i + 1,
                });
            }
            continue;
        }
        if (pending) {
            if (/^\s*\{\s*$/.test(line)) {
                cur = pending;
                pending = null;
            } else if (line.trim() !== "") {
                pending = null; // 前置声明 / 非结构体定义,丢弃
            }
            if (cur) continue;
        }
        const m = RE_STRUCT_OPEN.exec(line);
        if (!m) continue;
        const comment = m[4] || "";
        const sizeM = /\bsize\s+(\d+)/.exec(comment);
        const alignM = /\balign\s+(\d+)/.exec(comment);
        const entry = {
            name: m[2],
            source,
            line: i + 1,
            // 对齐有两个来源:`alignas(64)` 声明(硬事实)与注释里的 `align N`
            alignas:
                m[1] !== undefined && /^\d+$/.test(m[1]) ? Number(m[1]) : null,
            docSize: sizeM ? Number(sizeM[1]) : null,
            docAlign: alignM ? Number(alignM[1]) : null,
            fields: [],
        };
        if (m[3] === "{") cur = entry;
        else pending = entry;
    }
    return out;
}

// 文档里的结构体只认 ```c 围栏内的内容(散文里的反引号片段不参与)。
// 围栏外的行置空而不是删掉 —— 行号必须与原文一一对应,报错才指得准位置。
function extractCFences(md) {
    const out = [];
    let inFence = false;
    for (const raw of md.split("\n")) {
        const line = raw.replace(/\r$/, "");
        if (/^```/.test(line)) {
            inFence = /^```c\s*$/.test(line);
            out.push("");
            continue;
        }
        out.push(inFence ? line : "");
    }
    return out.join("\n");
}

// ---------------------------------------------------------------- 元素尺寸
const PRIM_BYTES = new Map([
    ["u8", 1],
    ["i8", 1],
    ["u16", 2],
    ["i16", 2],
    ["u32", 4],
    ["i32", 4],
    ["f32", 4],
    ["float32", 4],
    ["u64", 8],
    ["i64", 8],
    ["f64", 8],
]);

function elemBytes(type, golden) {
    const bare = type.replace(/^std::/, "").replace(/^atomic<(.+)>$/, "$1");
    if (PRIM_BYTES.has(bare)) return PRIM_BYTES.get(bare);
    const s = golden.structs.get(bare);
    return s ? s.size : null;
}

// ---------------------------------------------------------------- 主流程
const docText = readOrDie(DOC);
const goldenText = readOrDie(GOLDEN);
const golden = parseGolden(goldenText);

const docStructs = parseStructs(extractCFences(docText), DOC);

const headerStructs = new Map();
for (const h of HEADERS) {
    for (const [name, s] of parseStructs(readOrDie(h), h)) {
        if (!headerStructs.has(name)) headerStructs.set(name, s);
    }
}

// 头文件常量表(只取 `inline constexpr <type> kName = <数字>;`)
const headerConsts = new Map();
for (const h of HEADERS) {
    const text = readOrDie(h);
    const re = /^\s*inline\s+constexpr\s+[\w:]+\s+(k\w+)\s*=\s*([^;]+?);/gm;
    let m;
    while ((m = re.exec(text)) !== null) {
        const expr = m[2].trim().replace(/u?ll?$/i, "");
        let value = null;
        const shift = /^(\d+)u?\s*<<\s*(\d+)$/.exec(expr);
        if (shift) value = Number(shift[1]) * 2 ** Number(shift[2]);
        else if (/^\d+$/.test(expr)) value = Number(expr);
        if (value !== null) headerConsts.set(m[1], value);
    }
}

const goldenNum = (key) => {
    if (!golden.scalars.has(key)) {
        fail("golden 缺少标量行 `" + key + "`(本脚本的对拍锚点)");
        return null;
    }
    const v = golden.scalars.get(key);
    return /^0x/i.test(v) ? parseInt(v, 16) : Number(v);
};

const MAX_CHANNELS = goldenNum("max_channels");
const MAX_GROUPS = goldenNum("max_groups");
const BUDGET_REGISTRY = goldenNum("budget_registry_bytes");
const BUDGET_CTRL = goldenNum("budget_ctrl_bytes");

// 从文档全文抠一个数;抠不到就是 [FAIL] —— 「抠不到就跳过」是假绿的经典来源
function probe(label, re, groupCount) {
    const m = re.exec(docText);
    if (!m) {
        fail(
            "文档 " +
                DOC +
                " 里找不到「" +
                label +
                "」的书写(正则 " +
                re +
                ");文档格式漂移会让本项对拍静默失效,故按失败处理",
        );
        return null;
    }
    const nums = [];
    for (let i = 1; i <= groupCount; i += 1) nums.push(Number(m[i]));
    return nums;
}

function eq(label, docValue, goldenValue, goldenLabel) {
    if (docValue === null || goldenValue === null) return false;
    if (docValue !== goldenValue) {
        fail(
            label +
                ": 文档 = " +
                docValue +
                ",但 " +
                (goldenLabel || GOLDEN) +
                " = " +
                goldenValue,
        );
        return false;
    }
    pass(label + " = " + docValue);
    return true;
}

/**
 * 取头文件常量,抠不到即**硬失败**。
 * 「has() 抠不到就静默跳过」是本脚本自己在头注里点名的假绿来源:常量改了名、
 * 换了写法(宏 / enum / 带表达式的初值)或整块挪走,正则一失手,对拍就悄悄少跑一项
 * 而 gates 照样绿 —— 恰好是这张脚本存在的理由的反面。
 * @returns {boolean} true = 常量在表里,调用方可继续 eq
 */
function requireConst(name, label) {
    if (headerConsts.has(name)) return true;
    fail(
        "头文件(" +
            HEADERS.join(" / ") +
            ")里抠不到常量 " +
            name +
            "(" +
            label +
            " 无从对拍)。若它确实改了名或改了写法,请同步本脚本的常量正则/名字," +
            "不要让这一项静默跳过",
    );
    return false;
}

// ---------------------------------------------------------- A/B/C/D/E 结构体对拍
{
    const goldenNames = [...golden.structs.keys()];
    const docNames = [...docStructs.keys()];
    for (const s of goldenNames) {
        if (!docStructs.has(s)) {
            if (isPendingRatification(s)) {
                warn(
                    "结构体 " +
                        s +
                        " 已在 golden 冻结、尚未转正进 " +
                        DOC +
                        " —— 有 docs/contract-changes/ 的变更文档担保(修宪在途)。" +
                        "用户批准并由统筹转正后,本条会自动变回正向对拍",
                );
                continue;
            }
            fail(
                "结构体 " +
                    s +
                    " 在 golden 里冻结,但 " +
                    DOC +
                    " 的 ```c 围栏里没有它的定义",
            );
        }
    }
    for (const s of docNames) {
        if (!golden.structs.has(s)) {
            fail(
                "文档 " +
                    DOC +
                    ":" +
                    docStructs.get(s).line +
                    " 定义了结构体 " +
                    s +
                    ",但 golden 里没有冻结它(布局未被机器钉死)",
            );
        }
    }

    for (const [name, g] of golden.structs) {
        const d = docStructs.get(name);
        if (!d) continue;
        const where = DOC + ":" + d.line + " " + name;

        // B 字段名与顺序(顺序敏感:顺序变了偏移必然变)
        const gf = g.fields.map((f) => f.name);
        const df = d.fields.map((f) => f.name);
        if (gf.join(",") !== df.join(",")) {
            fail(
                where +
                    " 字段表与 golden 不一致:\n" +
                    "      文档   = [" +
                    df.join(", ") +
                    "]\n" +
                    "      golden = [" +
                    gf.join(", ") +
                    "]",
            );
        } else {
            pass(name + " 字段表 " + gf.length + " 项一致");
            // C 逐字段偏移
            for (let i = 0; i < gf.length; i += 1) {
                const dfield = d.fields[i];
                const gfield = g.fields[i];
                if (dfield.offset === null) {
                    warn(
                        DOC +
                            ":" +
                            dfield.line +
                            " " +
                            name +
                            "." +
                            dfield.name +
                            " 未标注偏移(golden = " +
                            gfield.offset +
                            "),本项无法机检",
                    );
                    continue;
                }
                if (dfield.offset !== gfield.offset) {
                    fail(
                        DOC +
                            ":" +
                            dfield.line +
                            " " +
                            name +
                            "." +
                            dfield.name +
                            " offset 文档 = " +
                            dfield.offset +
                            ",golden = " +
                            gfield.offset,
                    );
                } else {
                    pass(name + "." + dfield.name + " offset " + gfield.offset);
                }
            }
        }

        // D 尺寸与对齐
        if (d.docSize === null) {
            warn(
                where +
                    " 未标注 `size N`(golden size = " +
                    g.size +
                    "),本项无法机检",
            );
        } else if (d.docSize !== g.size) {
            fail(where + " size 文档 = " + d.docSize + ",golden = " + g.size);
        } else {
            pass(name + " size " + g.size);
        }

        const dAlign = d.docAlign !== null ? d.docAlign : d.alignas;
        if (dAlign === null) {
            warn(
                where +
                    " 未标注对齐(既无 `align N` 注释也无 alignas(...);golden align = " +
                    g.align +
                    "),本项无法机检",
            );
        } else if (dAlign !== g.align) {
            fail(where + " align 文档 = " + dAlign + ",golden = " + g.align);
        } else {
            pass(name + " align " + g.align);
        }

        // E 区间注释自洽:`a..b` 的 b 必须落在下一字段起点(末字段落在 size 上),
        //   且 b - a 必须等于元素数 × sizeof(元素)。#75 那轮修的就是这类数字。
        for (let i = 0; i < d.fields.length; i += 1) {
            const f = d.fields[i];
            if (f.end === null || f.offset === null) continue;
            const nextOffset =
                i + 1 < g.fields.length ? g.fields[i + 1].offset : g.size;
            if (f.end !== nextOffset) {
                fail(
                    DOC +
                        ":" +
                        f.line +
                        " " +
                        name +
                        "." +
                        f.name +
                        " 的区间注释写作 " +
                        f.offset +
                        ".." +
                        f.end +
                        ",但按 golden 该字段止于 " +
                        nextOffset +
                        (i + 1 < g.fields.length
                            ? "(下一字段 " + g.fields[i + 1].name + " 的起点)"
                            : "(结构体尺寸)"),
                );
                continue;
            }
            const unit = elemBytes(f.type, golden);
            const count =
                f.arrayLen === null
                    ? 1
                    : /^\d+$/.test(f.arrayLen)
                      ? Number(f.arrayLen)
                      : null;
            if (unit === null || count === null) continue;
            if (f.end - f.offset !== unit * count) {
                fail(
                    DOC +
                        ":" +
                        f.line +
                        " " +
                        name +
                        "." +
                        f.name +
                        " 区间跨度 " +
                        (f.end - f.offset) +
                        " 字节,与声明 " +
                        f.type +
                        "[" +
                        count +
                        "] 应占的 " +
                        unit * count +
                        " 字节不符",
                );
            } else {
                pass(name + "." + f.name + " 区间 " + f.offset + ".." + f.end);
            }
        }
    }
}

// ------------------------------------------- ①→② 完备性:头文件字段表 == golden
//
// 两个方向都要走:
//   golden → 头文件  golden 冻结的每个结构体在头文件里都找得到,且字段表逐项相等;
//   头文件 → golden  头文件里**新增**的结构体,要么进了 golden,要么在下表里登记。
// 只走前一个方向的话,往头文件里新加一个段内结构体、忘了冻 golden —— 三侧对拍一句话
// 都不会说(golden 里没有它,循环压根不会碰到它),而那正是 C7 要防的漂移。
{
    // 头文件里**不进共享内存**、故不该被 golden 冻结的结构体。
    // 名字写死在这里而不是靠 alignas 之类的启发式判据:新加一个结构体时,要么它进 golden,
    // 要么作者得来这一行写明它为什么不进 —— 不留第三条静默通过的路。
    const NON_LAYOUT_STRUCTS = new Map([
        [
            "VizSnapshot",
            "viz 段的**主机侧**快照(写方填、读方取),不进共享内存 —— 段内布局是 VizHeader/" +
                "VizFrame/VizTrackColors/VizCoverage/VizLanes/VizTrackState/VizTrackLabels 七个,已逐个冻进 golden",
        ],
        [
            "OutputGlobalInfoSnapshot",
            "OutputGlobalInfo 的宿主侧只读快照(带默认初值的普通结构体,不落段)",
        ],
        ["WatchdogResult", "停摆看门狗的动作返回值(纯宿主侧,01 §4.2 [R3/J52])"],
        [
            "CtrlBroadcastSnapshot",
            "ctrl 广播区的**宿主侧**普通 POD 快照(无原子、可自由拷贝;读写两侧各持一份)——" +
                "段内布局是 CtrlBroadcast + CtrlChannelConfig 两个,已冻进 golden",
        ],
    ]);

    for (const [name, h] of headerStructs) {
        if (golden.structs.has(name)) continue;
        if (NON_LAYOUT_STRUCTS.has(name)) {
            pass(
                name +
                    " 头文件独有,已登记为非段内结构体(" +
                    NON_LAYOUT_STRUCTS.get(name) +
                    ")",
            );
            continue;
        }
        fail(
            h.source +
                ":" +
                h.line +
                " 结构体 " +
                name +
                " 既没有被 " +
                GOLDEN +
                " 冻结布局,也不在本脚本的「非段内结构体」白名单里。二选一:" +
                "若它进共享内存,补进 golden(布局须由 tests/core/test_ipc_layout.cpp 的" +
                "编译期真值钉死)并写进 " +
                DOC +
                ";若它只活在宿主侧,在 NON_LAYOUT_STRUCTS 里登记并写明理由",
        );
    }

    for (const [name, g] of golden.structs) {
        const h = headerStructs.get(name);
        if (!h) {
            fail(
                "golden 冻结的结构体 " +
                    name +
                    " 在头文件(" +
                    HEADERS.join(" / ") +
                    ")里找不到定义",
            );
            continue;
        }
        const gf = g.fields.map((f) => f.name).join(",");
        const hf = h.fields.map((f) => f.name).join(",");
        if (gf !== hf) {
            fail(
                h.source +
                    ":" +
                    h.line +
                    " " +
                    name +
                    " 的字段表与 golden 不一致(golden 漏冻或代码新增未同步):\n" +
                    "      头文件 = [" +
                    h.fields.map((f) => f.name).join(", ") +
                    "]\n" +
                    "      golden = [" +
                    g.fields.map((f) => f.name).join(", ") +
                    "]",
            );
        } else {
            pass(
                name +
                    " 头文件字段表与 golden 一致(" +
                    h.fields.length +
                    " 项)",
            );
        }
    }
}

// ---------------------------------------------------------------- F 常量
{
    // abi:文档写作 `= kScvbAbi(1)`
    const abi = probe("kScvbAbi(N)", /kScvbAbi\((\d+)\)/, 1);
    if (abi) eq("abi", abi[0], goldenNum("abi"));

    // magic:文档写作 `kScvbMagic('SCVB')`;段内前 4 字节小端字面拼出 "SCVB"
    const mm = /kScvbMagic\('([\x20-\x7e]{4})'\)/.exec(docText);
    if (!mm) {
        fail("文档里找不到 `kScvbMagic('XXXX')` 的四字符声明");
    } else {
        const fourcc =
            (mm[1].charCodeAt(3) << 24) |
            (mm[1].charCodeAt(2) << 16) |
            (mm[1].charCodeAt(1) << 8) |
            mm[1].charCodeAt(0);
        eq("magic('" + mm[1] + "' 小端)", fourcc >>> 0, goldenNum("magic"));
    }

    // max_channels:两处独立书写都要对上(§2 段名 N=1..15;§5 registry 编制)
    const chA = probe(
        "§2 段名 audio.ch{N}(N=1..N)",
        /audio\.ch\{N\}`?\(N=1\.\.(\d+)/,
        1,
    );
    if (chA) eq("max_channels(§2 段名 N 上限)", chA[0], MAX_CHANNELS);
    const chB = probe("§5 registry 编制 InputSlot×N", /InputSlot×(\d+)/, 1);
    if (chB) eq("max_channels(§5 InputSlot×N)", chB[0], MAX_CHANNELS);

    // max_groups:§1 标题 G=1..8
    const gp = probe("§1 标题 G=1..N", /G=1\.\.(\d+)/, 1);
    if (gp) eq("max_groups", gp[0], MAX_GROUPS);

    // 环容量:文档同时写了移位式与展开值,两者都要自洽且等于 golden
    const rf = probe(
        "kDefaultRingFrames = 1<<k = N",
        /kDefaultRingFrames\s*=\s*1<<(\d+)\s*=\s*(\d+)/,
        2,
    );
    if (rf) {
        eq(
            "default_ring_frames(1<<" + rf[0] + " 展开)",
            2 ** rf[0],
            rf[1],
            "同行展开值",
        );
        eq("default_ring_frames", rf[1], goldenNum("default_ring_frames"));
    }
    const fc = probe(
        "kFeatCapacityHops = 1<<k = N",
        /kFeatCapacityHops\s*=\s*1<<(\d+)\s*=\s*(\d+)/,
        2,
    );
    if (fc) {
        eq(
            "feat_capacity_hops(1<<" + fc[0] + " 展开)",
            2 ** fc[0],
            fc[1],
            "同行展开值",
        );
        eq("feat_capacity_hops", fc[1], goldenNum("feat_capacity_hops"));
    }

    // 段预算:registry 标题的 `(4 KB;`、ctrl 的 `16 KB = kCtrlSegmentSize 16384`
    const rb = probe("§1 registry 段预算(N KB)", /registry`\((\d+) KB/, 1);
    if (rb) eq("budget_registry_bytes", rb[0] * 1024, BUDGET_REGISTRY);
    const cb = probe(
        "§4 ctrl 段预算(N KB = kCtrlSegmentSize M)",
        /(\d+) KB = kCtrlSegmentSize (\d+)/,
        2,
    );
    if (cb) {
        eq(
            "budget_ctrl_bytes(KB 与字节数自洽)",
            cb[0] * 1024,
            cb[1],
            "同行字节数",
        );
        eq("budget_ctrl_bytes", cb[1], BUDGET_CTRL);
        if (requireConst("kCtrlSegmentSize", "§4 ctrl 段预算")) {
            eq(
                "kCtrlSegmentSize",
                cb[1],
                headerConsts.get("kCtrlSegmentSize"),
                "src/core/ipc/CtrlPlane.h",
            );
        }
    }

    // 头文件里有、golden 没冻的两个常量:直接与头文件对。
    // 这两项的头文件侧是**唯一**机检面(golden 没冻它们),所以常量抠不到不能跳过 ——
    // 一跳就等于这两个数从此没有任何一侧在看着(下面 requireConst 硬失败的理由)。
    const hop = probe("kFeatHopMs(N)", /kFeatHopMs\((\d+)\)/, 1);
    if (hop && requireConst("kFeatHopMs", "§3 特征跳距")) {
        eq("kFeatHopMs", hop[0], headerConsts.get("kFeatHopMs"), HEADERS[0]);
    }
    const cap = probe("kCtrlRingCapacity = N", /kCtrlRingCapacity = (\d+)/, 1);
    if (cap && requireConst("kCtrlRingCapacity", "§4 CtrlRing 容量")) {
        eq(
            "kCtrlRingCapacity",
            cap[0],
            headerConsts.get("kCtrlRingCapacity"),
            HEADERS[1],
        );
    }
}

// ---------------------------------------------------------------- G 段名模板
{
    const templates = new Set();
    const re = /`Local\\(SynchainSCVB\.v1\.g\{G\}\.[^`]+)`/g;
    let m;
    while ((m = re.exec(docText)) !== null) templates.add(m[1]);
    if (templates.size === 0) {
        fail("文档里找不到任何 `Local\\SynchainSCVB.v1.g{G}.*` 段名模板");
    }
    const used = new Set();
    for (const n of golden.names) {
        if (!n.logical.startsWith("SynchainSCVB.v1.")) {
            fail(
                "golden 段名 " +
                    n.logical +
                    " 不带冻结前缀 SynchainSCVB.v1.(§5 永久冻结项)",
            );
            continue;
        }
        const tpl = n.logical
            .replace(/\.g\d+\./, ".g{G}.")
            .replace(/ch\d+$/, "ch{N}");
        if (!templates.has(tpl)) {
            if (isPendingRatification(tpl) || isPendingRatification(n.kind)) {
                warn(
                    "golden 段名 " +
                        n.logical +
                        " 的模板 " +
                        tpl +
                        " 尚未转正进 " +
                        DOC +
                        " —— 有变更文档担保(修宪在途)",
                );
                used.add(tpl);
                continue;
            }
            fail(
                "golden 段名 " +
                    n.logical +
                    " 归一化成模板 " +
                    tpl +
                    " 后,在文档段名模板集合 {" +
                    [...templates].join(", ") +
                    "} 里找不到",
            );
        } else {
            used.add(tpl);
            pass("段名 " + n.logical + " ← 模板 " + tpl);
        }
    }
    for (const t of templates) {
        if (!used.has(t)) {
            fail(
                "文档段名模板 " +
                    t +
                    " 在 golden 里没有任何一条对应的 name 行(未被冻结)",
            );
        }
    }
}

// ---------------------------------------------------------------- H registry 落点
{
    const p = probe(
        "§1 registry 落点(OutputSlot 起始 @X = A + N×B)",
        /OutputSlot 起始 @(\d+) = (\d+) \+ (\d+)×(\d+)/,
        4,
    );
    const inp = probe("§1 InputSlot 起始 @X", /InputSlot 起始 @(\d+)/, 1);
    const regHdr = golden.structs.get("RegistryHeader");
    const inSlot = golden.structs.get("InputSlot");
    const outSlot = golden.structs.get("OutputSlot");
    if (p && inp && regHdr && inSlot && outSlot) {
        const [outAt, base, count, stride] = p;
        eq(
            "§1 InputSlot 起始偏移 = sizeof(RegistryHeader)",
            inp[0],
            regHdr.size,
        );
        eq(
            "§1 OutputSlot 算式的基址 = sizeof(RegistryHeader)",
            base,
            regHdr.size,
        );
        eq("§1 OutputSlot 算式的槽数 = max_channels", count, MAX_CHANNELS);
        eq("§1 OutputSlot 算式的步长 = sizeof(InputSlot)", stride, inSlot.size);
        eq(
            "§1 OutputSlot 起始偏移(算式自洽)",
            outAt,
            base + count * stride,
            "同行算式",
        );
        const used = outAt + outSlot.size;
        if (used > BUDGET_REGISTRY) {
            fail(
                "§1 registry 用量 " +
                    used +
                    " 字节超出段预算 " +
                    BUDGET_REGISTRY +
                    " 字节",
            );
        } else {
            pass("§1 registry 用量 " + used + " ≤ 预算 " + BUDGET_REGISTRY);
        }
        // §1 结构体注释里的 `InputSlot0 @64` / `OutputSlot0 @1024` 要与落点一致
        const i0 = probe("§1 InputSlot0 @X", /InputSlot0 @(\d+)/, 1);
        if (i0) eq("§1 InputSlot0 落点", i0[0], inp[0], "§1 段布局注释");
        const o0 = probe("§1 OutputSlot0 @X", /OutputSlot0 @(\d+)/, 1);
        if (o0) eq("§1 OutputSlot0 落点", o0[0], outAt, "§1 段布局注释");
    }
}

// ---------------------------------------------------------------- I ctrl 落点
{
    const hdr = probe(
        "§4 `CtrlHeader`@X(YB)",
        /`CtrlHeader`@(\d+)\((\d+)B\)/,
        2,
    );
    const bc = probe("§4 广播区@X(YB)", /广播区@(\d+)\((\d+)B\)/, 2);
    const gi = probe(
        "§4 `OutputGlobalInfo`@X(YB)",
        /`OutputGlobalInfo`@(\d+)\((\d+)B\)/,
        2,
    );
    const rings = probe(
        "§4 `CtrlRing`×N @X(各 YB)",
        /`CtrlRing`×(\d+) @(\d+)\(每 channel 一条,各 (\d+)B/,
        3,
    );
    const total = probe(
        "§4 总量算式 X + N×Y = Z",
        /(\d+) \+ (\d+)×(\d+) = (\d+) 恰占满预算/,
        4,
    );
    const gHdr = golden.structs.get("CtrlHeader");
    const gInfo = golden.structs.get("OutputGlobalInfo");
    const gRing = golden.structs.get("CtrlRing");
    if (hdr && bc && gi && rings && total && gHdr && gInfo && gRing) {
        eq("§4 CtrlHeader 落点", hdr[0], 0, "段起点");
        eq("§4 CtrlHeader 尺寸", hdr[1], gHdr.size);
        eq(
            "§4 广播区起点(= CtrlHeader 之后)",
            bc[0],
            hdr[0] + hdr[1],
            "上一段落点",
        );
        eq(
            "§4 OutputGlobalInfo 起点(= 广播区之后)",
            gi[0],
            bc[0] + bc[1],
            "上一段落点",
        );
        eq("§4 OutputGlobalInfo 尺寸", gi[1], gInfo.size);
        eq(
            "§4 CtrlRing 阵列起点(= OutputGlobalInfo 之后)",
            rings[1],
            gi[0] + gi[1],
            "上一段落点",
        );
        eq("§4 CtrlRing 条数 = max_channels", rings[0], MAX_CHANNELS);
        eq("§4 CtrlRing 尺寸", rings[2], gRing.size);
        eq("§4 总量算式基址", total[0], rings[1], "§4 CtrlRing 阵列起点");
        eq("§4 总量算式条数", total[1], rings[0], "§4 CtrlRing 条数");
        eq("§4 总量算式步长", total[2], rings[2], "§4 CtrlRing 尺寸");
        eq(
            "§4 总量(算式自洽)",
            total[3],
            total[0] + total[1] * total[2],
            "同行算式",
        );
        eq("§4 总量 = ctrl 段预算", total[3], BUDGET_CTRL);
        // 与头文件常量对齐(落点常量的编译期真源在 CtrlPlane.h)
        const hc = [
            ["kCtrlBroadcastOffset", bc[0], "§4 广播区起点"],
            ["kCtrlBroadcastBytes", bc[1], "§4 广播区尺寸"],
            ["kCtrlGlobalInfoOffset", gi[0], "§4 OutputGlobalInfo 起点"],
            ["kCtrlRingsOffset", rings[1], "§4 CtrlRing 阵列起点"],
        ];
        for (const [k, v, label] of hc) {
            if (!requireConst(k, label)) continue;
            eq(label + " = " + k, v, headerConsts.get(k), HEADERS[1]);
        }
    }
}

// ---------------------------------------------------------------- 汇总输出
if (verbose) {
    for (const p of passes) console.log("  [OK] " + p);
}
for (const w of warnings) console.log("  [WARN] " + w);

const warnFatal = strict && warnings.length > 0;
if (errors.length > 0 || warnFatal) {
    console.error("");
    console.error(
        "check-ipc-doc-parity 失败(" +
            errors.length +
            " 项不一致" +
            (warnFatal ? " + " + warnings.length + " 项未标注[--strict]" : "") +
            "):",
    );
    for (const e of errors) console.error("  [FAIL] " + e);
    console.error("");
    console.error(
        "  真源方向:代码 → " + GOLDEN + " → " + DOC + "(以 golden 为准)。",
    );
    console.error(
        "  " +
            DOC +
            " 是冻结契约:不要为了让本脚本变绿而随手改它,先走 CONTRIBUTING.md §8 / 契约 §5。",
    );
    process.exit(1);
}

console.log(
    "check-ipc-doc-parity 通过: 结构体 " +
        golden.structs.size +
        " 个 / 字段 " +
        [...golden.structs.values()].reduce((n, s) => n + s.fields.length, 0) +
        " 项 / 段名 " +
        golden.names.length +
        " 条;逐项断言 " +
        passes.length +
        " 条通过,警告 " +
        warnings.length +
        " 条",
);
process.exit(0);
