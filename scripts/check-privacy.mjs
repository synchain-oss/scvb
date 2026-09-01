// SPDX-License-Identifier: GPL-3.0-or-later
// check-privacy.mjs —— 公开仓隐私门禁(零依赖,Node >= 18,ESM)。
//   扫描 **git 跟踪的工作树**(不重写历史;历史里的旧 blob 见 SL-265 卡的说明),
//   拦下四类会把维护者个人信息带进公开仓的内容:
//     P1 项目代号禁词      —— 零容忍,无豁免(硬规:公开材料严禁该标识与其邮箱形态)
//     P2 个人本机路径      —— C:\Users\<具体用户名>;占位符(<workspace> 等)豁免
//     P3 个人邮箱域        —— gmail/qq/163/... ;third_party/ 等 vendored 目录与
//                             contact@synchain.ca / *.noreply.github.com 豁免
//     P4 个人主机名        —— DESKTOP-<序列号> / LAPTOP-<序列号>
//
//   **为什么所有针都从片段拼出来**:本脚本自己也是被扫的跟踪文件。若把禁词写成字面量,
//   它会扫到自己 ⇒ 只能给自己开豁免 ⇒ 那个豁免就成了藏东西的地方。拼装后源码里不含任何
//   完整禁词,脚本可以和别的文件一样被自己扫,**不需要任何自我豁免**。
//   同理,**自检夹具也必须拼装**:第一版把 `someone@gmail.com` / `DESKTOP-AB12CD3` 写成字面量,
//   脚本一被纳入跟踪就扫到自己的夹具、当场三处命中(未跟踪时 `git ls-files` 看不见它,所以
//   加进版本库前一直是绿的 —— 典型的「入库那一刻才炸」)。头注里的示例也不能写成能匹配的形态。
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
const RE_CODENAME = new RegExp(CODENAME, "gi");
const RE_WIN_HOME = new RegExp(
    j("[A-Za-z]:", "\\\\", WIN_USERS) + "([^\\\\/\\s\"'`|;*?]+)",
    "g",
);
const RE_MAIL = new RegExp(
    j("[A-Za-z0-9._%+-]+", "@", "(", MAIL_DOMAINS, ")", "\\.", "(com|cn|net)"),
    "gi",
);
const RE_HOST = new RegExp(j("\\b(DESKTOP", "|LAPTOP)-[A-Z0-9]{5,}"), "g");

// 占位符用户名:<workspace> / <user> / <你的用户名> / %USERPROFILE% / $env:...
const isPlaceholderUser = (s) =>
    s.startsWith("<") || s.startsWith("%") || s.startsWith("$") || s === "...";

// vendored / 上游代码目录:P3 豁免(别人的版权头里带作者邮箱,不是我们的泄漏)。
const VENDORED = ["third_party/", "external/", "vendor/", "LICENSES/"];
const isVendored = (rel) => VENDORED.some((d) => rel.startsWith(d));

// P3 允许的对外邮箱(唯一对外联络地址)与 GitHub noreply 提交身份。
const MAIL_ALLOW = [/contact@synchain\.ca/i, /@users\.noreply\.github\.com/i];

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
        exempt: (rel, m) =>
            isVendored(rel) || MAIL_ALLOW.some((a) => a.test(m[0])),
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
    console.log("  P3 允许邮箱:contact@synchain.ca、*.noreply.github.com");
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
        [
            "P3",
            j("someone", "@", "gmail", ".com"),
            j("contact", "@", "synchain", ".ca"),
        ],
        ["P4", j("DESKTOP", "-", "AB12CD3"), j("DESKTOP", "-", "短")],
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
                    text: lines[i].trim().slice(0, 160),
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
console.error(
    "误报请调豁免表(脚本内 VENDORED / MAIL_ALLOW / isPlaceholderUser),不要给整个文件开天窗。",
);
process.exit(1);
