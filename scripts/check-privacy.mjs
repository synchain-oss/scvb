// SPDX-License-Identifier: GPL-3.0-or-later
// check-privacy.mjs —— 公开仓隐私门禁(零依赖,Node >= 18,ESM)。
//   扫描 **git 跟踪的工作树**(不重写历史;历史里的旧 blob 见 SL-265 卡的说明),
//   拦下四类会把维护者个人信息带进公开仓的内容:
//     P1 项目代号禁词      —— 零容忍,无豁免(硬规:公开材料严禁该标识与其邮箱形态)
//     P2 个人本机路径      —— C:\Users\<具体用户名>;占位符(<workspace> 等)豁免
//     P3 个人邮箱域        —— gmail/qq/163/... ;仅 third_party/ 等 vendored 目录豁免
//                             (对外邮箱 contact@synchain.ca 的域不在表内,本就不会命中)
//     P4 个人主机名        —— DESKTOP-<序列号> / LAPTOP-<序列号>
//
//   **为什么所有针都从片段拼出来**:本脚本自己也是被扫的跟踪文件。若把禁词写成字面量,
//   它会扫到自己 ⇒ 只能给自己开豁免 ⇒ 那个豁免就成了藏东西的地方。拼装后源码里不含任何
//   完整禁词,脚本可以和别的文件一样被自己扫,**不需要任何自我豁免**。
//   同理,**自检夹具与头注示例也必须拼装**。第一版把邮箱、主机名夹具写成了字面量(这里不复述
//   其形态 —— 写出来就又会被自己命中),结果:未跟踪时 `git ls-files` 看不见本文件,本地一路
//   全绿;`git add` 之后它成为被扫对象,自己的夹具当场三处命中。**典型的「入库那一刻才炸」**,
//   本地怎么测都测不出来,只有先入库再扫才暴露。
//   `--self-test` 会校验拼装结果确实等于目标形态(防止有人把片段改坏、让门禁静默失效)。
//
//   用法:
//     node scripts/check-privacy.mjs [--help] [--self-test] [--list-rules]
//   退出码:0 = 干净;1 = 有命中或自检失败。

import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const USAGE = [
    "用法: node scripts/check-privacy.mjs [选项]",
    "  扫描 git 跟踪的工作树,拦截个人隐私信息(代号禁词/本机路径/个人邮箱域/主机名)。",
    "  --self-test    自检:校验拼装出的针确实等于目标形态,并用内置坏样例验证能抓到。",
    "  --list-rules   打印规则与豁免表后退出 0。",
    "  --help, -h     打印本说明并退出 0。",
].join("\n");

let selfTest = false;
let listRules = false;
for (const a of process.argv.slice(2)) {
    if (a === "--help" || a === "-h") {
        console.log(USAGE);
        process.exit(0);
    } else if (a === "--self-test") {
        selfTest = true;
    } else if (a === "--list-rules") {
        listRules = true;
    } else {
        console.error("check-privacy: 无法识别的参数 " + a);
        console.error(USAGE);
        process.exit(1);
    }
}

const nodeMajor = Number(process.versions.node.split(".")[0]);
if (nodeMajor < 18) {
    console.error(
        "check-privacy: 需要 Node >= 18(当前 " + process.versions.node + ")",
    );
    process.exit(1);
}

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

// --- 针的拼装(见文件头注:源码里绝不出现完整禁词)-------------------------
const j = (...parts) => parts.join("");

const CODENAME = j("el", "en", "vo"); // P1 禁词
const WIN_USERS = j("Users", "\\\\"); // P2:与 "C:" 分开拼,避免完整路径字面量
const MAIL_DOMAINS = [
    "gmail",
    "qq",
    "163",
    "126",
    "outlook",
    "hotmail",
    "yahoo",
    "foxmail",
    "icloud",
    "sina",
].join("|");

// P2 的用户名段:排除路径分隔符/引号/空白;占位符(以 < 开头、或 % 包裹的环境变量)另判。
// 分隔符必须吃三种形态,否则最容易**复发**的那几类文件全部漏网(复审 r1 实测:六种写法只中一种):
// 单反斜杠 = 手写 / cmd 输出;正斜杠 = **CMake 规范形式**、compile_commands.json;
// 双反斜杠 = **JSON 转义**(.vscode/settings.json、CMakePresets.json 等落盘路径)。
// 另加 MSYS 与 WSL 的前缀形态(见下面 DRIVE 的两条分支;此处不写出样例 —— 写出来会被自己命中)。
// `i` 标志覆盖小写盘符与小写 users。
// P2 防的正是「构建产物 / 编辑器配置被误提交」,那些文件里的路径几乎都是上面这些形态。
const BS = "\\\\"; // 正则源码里的「一个反斜杠」
const SEP = j("(?:", BS, BS, "|", BS, "|/)"); // 双反斜杠 / 单反斜杠 / 正斜杠
const DRIVE = j("(?:[A-Za-z]:", SEP, "|/(?:mnt/)?[A-Za-z]/)"); // 盘符 / MSYS / WSL

const RE_CODENAME = new RegExp(CODENAME, "gi");
const RE_WIN_HOME = new RegExp(
    j(DRIVE, "Users", SEP) + "([^\\\\/\\s\"'`|;*?]+)",
    "gi",
);
const RE_MAIL = new RegExp(
    j("[A-Za-z0-9._%+-]+", "@", "(", MAIL_DOMAINS, ")", "\\.", "(com|cn|net)"),
    "gi",
);
const RE_HOST = new RegExp(j("\\b(DESKTOP", "|LAPTOP)-[A-Z0-9]{5,}"), "gi"); // i:小写主机名同样拦

// 占位符用户名:<workspace> / <user> / <你的用户名> / %USERPROFILE% / $env:...
const isPlaceholderUser = (s) =>
    s.startsWith("<") || s.startsWith("%") || s.startsWith("$") || s === "...";

// vendored / 上游代码目录:P3 豁免(别人的版权头里带作者邮箱,不是我们的泄漏)。
const VENDORED = ["third_party/", "external/", "vendor/", "LICENSES/"];
const isVendored = (rel) => VENDORED.some((d) => rel.startsWith(d));

// 【复审 r1 删除 MAIL_ALLOW】原先这里有一张「允许邮箱」表(contact@synchain.ca、
// *.noreply.github.com)。它是**不可达代码**:RE_MAIL 要求域名 ∈ MAIL_DOMAINS 且 TLD ∈
// (com|cn|net),而 `synchain.ca`(TLD 不在表内)与 `users.noreply.github.com`(域名不在表内)
// **都不可能匹配 RE_MAIL**,所以那张表一次也不会被查到 —— 更糟的是它让自检的负样例变成**空断言**
// (删掉整张表,自检照样绿)。看着像安全网、实则从不生效的代码比没有更危险,故删除。
// 这两个地址本来就不会被 P3 命中,不需要豁免;将来若往 MAIL_DOMAINS 里加了通用域名,再连同
// 配套断言一起补回来。

const RULES = [
    {
        id: "P1",
        name: "项目代号禁词(零容忍,无豁免)",
        re: RE_CODENAME,
        exempt: () => false,
    },
    {
        id: "P2",
        name: j("个人本机路径 <盘符>:", "\\", "Users", "\\", "<用户名>"),
        re: RE_WIN_HOME,
        // m[1] = 用户名段;占位符豁免。
        exempt: (rel, m) => isPlaceholderUser(m[1]),
    },
    {
        id: "P3",
        name: "个人邮箱域(" + MAIL_DOMAINS.replace(/\|/g, "/") + ")",
        re: RE_MAIL,
        exempt: (rel) => isVendored(rel),
    },
    {
        id: "P4",
        name: "个人主机名(DESKTOP-/LAPTOP-)",
        re: RE_HOST,
        exempt: () => false,
    },
];

if (listRules) {
    console.log("check-privacy 规则:");
    for (const r of RULES) {
        console.log("  " + r.id + "  " + r.name);
    }
    console.log("豁免:");
    console.log("  P2 占位符用户名:<...> / %...% / $...");
    console.log("  P3 vendored 目录:" + VENDORED.join(" "));
    process.exit(0);
}

// --- 自检:针拼对了没 + 坏样例真能抓到 --------------------------------------
if (selfTest) {
    // ★ 针的**独立**真值:用码点另拼一遍,与上面的片段拼装是两条互不引用的路径。
    //   为什么必须独立:第一版把 P1 正样例写成 `"联系 " + CODENAME + "..."`,样例跟着针一起变 ——
    //   把片段改成 j("el","en","XX") 后自检**照样全绿**(实测),等于自检测不出它声称要防的
    //   「针被改坏 ⇒ 门禁静默放行」。长度校验同样没用:改坏后仍是 6 字符。
    //   **断言必须落在与被测物独立的真值上**,否则就是自我循环。
    const CODENAME_TRUTH = String.fromCharCode(101, 108, 101, 110, 118, 111);

    const cases = [
        // [规则 id, 应命中的样例, 应豁免/不命中的样例]
        // P1 正样例用**独立真值**构造,不复用 CODENAME。
        [
            "P1",
            "联系 " + CODENAME_TRUTH + "@example.com",
            "无关文本 elephant venue",
        ],
        [
            "P2",
            j("D:", "\\", "Users", "\\", "someone", "\\", "proj"),
            j("C:", "\\", "Users", "\\", "<workspace>", "\\", "proj"),
        ],
        // P2 的其余五种形态各来一条(复审 r1:原先只覆盖手写形态,而复发路径恰是另外几种)。
        [
            "P2",
            j("c:", "\\", "users", "\\", "someone"),
            j("%", "USERPROFILE", "%"),
        ],
        [
            "P2",
            j("C:", "/", "Users", "/", "someone"),
            j("C:", "/", "Users", "/", "<user>"),
        ],
        [
            "P2",
            j("C:", "\\\\", "Users", "\\\\", "someone"),
            j("$", "env:USERPROFILE"),
        ],
        ["P2", j("/", "c", "/", "Users", "/", "someone"), "无关 /c/ 文本"],
        [
            "P2",
            j("/", "mnt", "/", "c", "/", "Users", "/", "someone"),
            "无关文本",
        ],
        [
            "P3",
            j("someone", "@", "gmail", ".com"),
            j("someone", "@", "example", ".org"), // 域不在表内 ⇒ 本就不该命中
        ],
        ["P4", j("DESKTOP", "-", "AB12CD3"), j("DESKTOP", "-", "短")],
        // 小写主机名:`i` 标志被去掉就红(复审 r1)。
        ["P4", j("laptop", "-", "z9y8x7w"), j("LAPTOP", "-", "abc")],
    ];
    let bad = 0;
    for (const [id, shouldHit, shouldMiss] of cases) {
        const rule = RULES.find((r) => r.id === id);
        const hit = [...shouldHit.matchAll(rule.re)].some(
            (m) => !rule.exempt("some/file.txt", m),
        );
        const miss = [...shouldMiss.matchAll(rule.re)].some(
            (m) => !rule.exempt("some/file.txt", m),
        );
        if (!hit) {
            console.error("self-test: " + id + " 漏报 —— 应命中却没命中");
            bad++;
        }
        if (miss) {
            console.error("self-test: " + id + " 误报 —— 应豁免却命中");
            bad++;
        }
    }
    // ★ 豁免表**被放宽**同样会让门禁静默失效,而上面所有用例都传 rel="some/file.txt",
    //   `isVendored` 永远走 false 分支 ⇒ 把 VENDORED 改成 [""] 或 isVendored 改成恒 true,
    //   自检与全量扫描**双双全绿**(仓里唯一真命中 miniz 本就被豁免),P3 整条静默失效。
    //   所以豁免必须按**范围**断言:同一条命中,vendored 下豁免、非 vendored 下必须照红。
    {
        const p3 = RULES.find((r) => r.id === "P3");
        const mail = j("someone", "@", "gmail", ".com");
        const hitIn = (rel) =>
            [...mail.matchAll(p3.re)].some((m) => !p3.exempt(rel, m));
        if (hitIn("third_party/x.h")) {
            console.error("self-test: P3 vendored 豁免失效 —— 上游文件不该红");
            bad++;
        }
        if (!hitIn("src/core/X.cpp")) {
            console.error(
                "self-test: P3 豁免被放宽 —— 非 vendored 路径也被放行(门禁静默失效)",
            );
            bad++;
        }
    }
    // ★ MAIL_DOMAINS 逐字比对独立真值:十个域里原先只有 gmail 被样例覆盖,
    //   删掉 qq/163/foxmail 等任意一个,自检照样全绿(与 CODENAME 同一个形态的漏洞)。
    {
        const DOMAINS_TRUTH = [
            "gmail",
            "qq",
            "163",
            "126",
            "outlook",
            "hotmail",
            "yahoo",
            "foxmail",
            "icloud",
            "sina",
        ].join("|");
        if (MAIL_DOMAINS !== DOMAINS_TRUTH) {
            console.error(
                "self-test: MAIL_DOMAINS 与独立真值不符 —— 有域被增删,P3 覆盖面变了",
            );
            bad++;
        }
    }
    // 针的形态校验:与独立真值逐字比对(长度校验不够 —— 改坏后仍是 6 字符)。
    if (CODENAME !== CODENAME_TRUTH) {
        console.error(
            "self-test: 代号针与独立真值不符 —— 片段被改坏,门禁会静默放行",
        );
        bad++;
    }
    if (bad > 0) {
        console.error("check-privacy --self-test: 失败 " + bad + " 项");
        process.exit(1);
    }
    console.log(
        "check-privacy --self-test: 全部通过(" + RULES.length + " 条规则)",
    );
    process.exit(0);
}

// --- 扫描 -------------------------------------------------------------------
let files;
try {
    files = execFileSync("git", ["ls-files", "-z"], {
        cwd: REPO,
        maxBuffer: 64 * 1024 * 1024,
    })
        .toString("utf8")
        .split("\0")
        .filter(Boolean);
} catch (e) {
    console.error("check-privacy: git ls-files 失败 —— " + e.message);
    process.exit(1);
}

// 命中片段打码(与同 job 的 gitleaks --redact 同口径:公开 Actions 日志不得回显 PII 本体)。
//
// 分级:**P1 全打**——它是唯一「一次都不能公开出现」的串,而首尾各留 2 字符对一个 6 字符的词
// 等于泄漏 4/6(实测输出曾是 `el**vo`,基本等于明文)。其余规则保留**前 2 字符**给出形态线索
// (如 `C:***********`),但**不留尾部**——尾部往往正是用户名/域名的可辨识部分。
// 定位本来就靠 file:line:col,开发者在自己的工作副本里一看便知,打码片段只是辅助。
const redact = (str, ruleId) => {
    // 含 P1 禁词的命中一律**全打**,不论是被哪条规则抓到的:同一个串常同时命中 P1 与 P3
    // (禁词出现在邮箱本地部),只给 P1 全打的话,P3 那行仍会漏出禁词前缀。
    RE_CODENAME.lastIndex = 0;
    if (ruleId === "P1" || RE_CODENAME.test(str) || str.length <= 4) {
        return "*".repeat(str.length);
    }
    return str.slice(0, 2) + "*".repeat(str.length - 2);
};

const findings = [];
let scanned = 0;

for (const rel of files) {
    const abs = path.join(REPO, rel);
    let buf;
    try {
        const st = fs.statSync(abs);
        if (!st.isFile()) continue;
        buf = fs.readFileSync(abs);
    } catch {
        continue; // 已删除 / 不可读:交给别的门禁管
    }
    // 二进制跳过(前 8KB 内有 NUL 即判二进制,与 git 的启发式同口径)。
    if (buf.subarray(0, 8192).includes(0)) continue;

    const text = buf.toString("utf8");
    scanned++;
    const lines = text.split(/\r?\n/);

    for (const rule of RULES) {
        for (let i = 0; i < lines.length; i++) {
            rule.re.lastIndex = 0;
            for (const m of lines[i].matchAll(rule.re)) {
                if (rule.exempt(rel, m)) continue;
                findings.push({
                    rule,
                    rel,
                    line: i + 1,
                    col: (m.index ?? 0) + 1,
                    // ⚠ **只存打码后的命中片段,绝不存整行原文**。本门禁跑在公开仓的
                    // compliance job 上,Actions 日志是**公开、长期留存、可被抓取**的:
                    // 回显整行 = 门禁把它要拦的 PII 亲手发布了一次(P1 尤其致命 —— 红一次
                    // 就等于公开一次)。同 job 的 gitleaks 用 `--redact` 正是这个理由。
                    // 定位靠 file:line:col 已经足够;打码片段还能直接告诉开发者「命中的是哪一段」。
                    text: redact(m[0], rule.id),
                });
            }
        }
    }
}

if (findings.length === 0) {
    console.log(
        "check-privacy 通过: " +
            scanned +
            " 个文本文件,四条规则零命中(P1 代号 / P2 本机路径 / P3 个人邮箱域 / P4 主机名)",
    );
    process.exit(0);
}

console.error("check-privacy 失败: " + findings.length + " 处命中\n");
for (const f of findings) {
    console.error(
        "  [" +
            f.rule.id +
            "] " +
            f.rel +
            ":" +
            f.line +
            ":" +
            f.col +
            "  " +
            f.text,
    );
}
console.error("\n规则说明见 `node scripts/check-privacy.mjs --list-rules`。");
// 提示必须按规则分叉:P1 是**唯一**没有豁免点的规则(exempt 恒 false),对它说「去调豁免表」
// 是把人指向一条走不通的路 —— 零容忍是本卡刻意的设计,命中就只能清理内容本身。
if (findings.some((f) => f.rule.id === "P1")) {
    console.error(
        "P1 **无豁免**(零容忍):命中即须清理内容本身,不存在调表放行的选项。",
    );
}
if (findings.some((f) => f.rule.id !== "P1")) {
    console.error(
        "P2/P3/P4 若确属误报,调对应豁免点(isPlaceholderUser / VENDORED),不要给整个文件开天窗。",
    );
}
process.exit(1);
