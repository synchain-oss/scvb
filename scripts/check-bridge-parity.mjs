// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * check-bridge-parity.mjs —— SCVB JS<->C++ 桥契约的名字集合三方比对(容错版,T25 交付)
 *
 * ============================================================================
 * 一、三方比对的完整口径(T25 定义;T28/T29 起全量生效,后续 agent 不得各自发明)
 * ============================================================================
 * 比对的是「名字集合」——native function 名与事件名,三方必须逐项相等:
 *
 *   A 侧 · 契约 manifest
 *     文件:docs/SCVB_CONTRACT.md
 *     抽取:文档内第一个能解析且含 "contractVersion" 键的 ```json 围栏块(即 §7 manifest)
 *     结构:{ contractVersion, output:{functions:[{name,params,returns}], events:[string]},
 *             input:{...}, enums:{...} }
 *
 *   B 侧 · JS 桥后端(T27/T28 产出)
 *     文件:web/shared/bridge.js(createBridge():JuceBackend / MockBackend 自动切换)
 *     抽取:该文件必须导出两张「冻结名表」,本脚本按正则读取其数组字面量:
 *         export const BRIDGE_FUNCTIONS = { output: [ "..." ], input: [ "..." ] };
 *         export const BRIDGE_EVENTS    = { output: [ "..." ], input: [ "..." ] };
 *       两张表是 JuceBackend 与 MockBackend 的共同真源(两个后端都按表注册/实现),
 *       因此「mock 与 JuceBackend 导出名一致」由结构保证,parity 只需比对表与契约。
 *       此外还读**第三张**表(可缺,缺则按空表):
 *         export const PENDING_FUNCS    = { output: [ "..." ], input: [ "..." ] };
 *       = 待转正桥函数(契约还没改、native 尚未落地;见 bridge.js 头注)。它**不进**
 *       与契约的集合比对 —— 进了必然红,而那张表正是为「桥面先跑、契约后改」留的
 *       不撒谎的落点。但**禁止复活名单照查**:否则往待转正表里写一个 §8.2 的死名字
 *       就能绕过机检,「待转正」就成了复活通道。
 *
 *   C 侧 · C++ 常量表(T26/DeepSeek native 侧产出)
 *     文件:src/input/InputBridgeApi.h、src/output/OutputBridgeApi.h
 *     抽取:头文件中每个桥名必须以字符串字面量形式出现在 constexpr 赋值里,例:
 *         inline constexpr const char* kFnSetGroupId = "setGroupId";
 *         inline constexpr const char* kEvState      = "scvb.state";
 *       分类规则:字面量以 "scvb." 开头 => 事件名;否则 => 函数名。
 *       非桥名的辅助字符串常量必须写在带 "parity-ignore" 注释的行上,否则会被误判。
 *
 *   比对规则:
 *     - 逐侧(output / input)分别比对 functions 与 events 的**集合**(顺序无关)。
 *     - 契约有而某侧缺 => MISSING;某侧有而契约无 => EXTRA;两者都判 ERROR。
 *     - 两侧共有的同名函数/事件(requestInitialState / setGroupId / setLang /
 *       setUiScale / commitUiScale / scvb.state / scvb.conn / scvb.groups / scvb.error)
 *       是**合法**的同名同签名项:**名字**只在各自侧内查重、不跨侧查重,但**签名(params
 *       数组,顺序敏感)必须跨侧逐项相等**(T25 卡冻结项「Input/Output 两侧同名同签名」)。
 *       `returns` 允许跨侧不同(契约 §1.4/§3.3 已定 observer/conflict 差异),显式豁免。
 *
 * ============================================================================
 * 二、容错(T25 时点:B 侧与 C 侧尚不存在)
 * ============================================================================
 *   - 某侧文件不存在 => 打印 [SKIP] 并跳过该侧比对,不影响退出码。
 *   - 文件存在但抽取不到任何名字 => [ERROR](说明格式漂移,必须修)。
 *   - A 侧(契约)始终参与,且是 T25 时点唯一有效的红线,共八项:
 *       ① manifest 可解析;
 *       ② 名字合法(lowerCamelCase / scvb.*)且侧内无重复;
 *       ③ **完备性**:与 05 §1.4 对齐的冻结期望表 EXPECTED 逐项零差异 + 四个计数断言
 *          (Output 35 函数 / 9 事件,Input 7 函数 / 5 事件)——防「契约被误删/误改名而脚本仍绿」;
 *       ④ **跨侧同名函数签名一致**(params 顺序敏感);
 *       ⑤ 无禁止复活名单命中,且每个禁止名都在契约 §8.2 名单行内以反引号整词列明
 *          (只在 §8.2 段落内匹配:`curves` / `misalign` 等子串会被 `curves_per_track`、
 *           `misalignCount` 等合法现行字段遮蔽,故不做全文 includes);
 *       ⑥ 枚举取值与 §5/§7 一致(含 segmentsReason 十值);
 *       ⑦ **manifest 与正文双向可见**:manifest 每项在**对应侧**正文段落内有条目
 *          (正文按 "## 3. Input" 切成 output/input 两段,§5 起截断,避免 side-blind);
 *          反向,正文里定义的每个函数/事件条目也必须被 manifest 收录。
 *       ⑧ 相对 05 §1.4 的授权增量只有 3 项(setAnalysisConfig、Input scvb.error、confirmPrintGuard),
 *          在 EXPECTED 表旁注明来源,使脚本本身成为「对 05 §1.4 零差异 + 3 项显式增量」的断言。
 *
 * 退出码:发现任何差异 / 命中禁止名 / 自检失败 => 1;全部通过 => 0。
 *
 * 用法:node scripts/check-bridge-parity.mjs   (工作目录任意,脚本自解析仓库根)
 */

import { readFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, relative } from "node:path";

const SCRIPT_DIR = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = dirname(SCRIPT_DIR);

const CONTRACT_PATH = join(REPO_ROOT, "docs", "SCVB_CONTRACT.md");
const BRIDGE_JS_PATH = join(REPO_ROOT, "web", "shared", "bridge.js");
const HEADER_PATHS = {
    input: join(REPO_ROOT, "src", "input", "InputBridgeApi.h"),
    output: join(REPO_ROOT, "src", "output", "OutputBridgeApi.h"),
    // [SL-258] monitor 也进来:第五节按侧取常量表,路径只留这一个真源 —— 之前 SIDES 里
    // 另写了一份,改一处漏一处就会走进「读不到 → 静默跳过」。
    monitor: join(REPO_ROOT, "src", "monitor", "MonitorBridgeApi.h"),
};

/** 禁止复活名单 —— 与 docs/SCVB_CONTRACT.md §8.2 同源,改动必须两处同步。 */
const FORBIDDEN = [
    "getAnalyzePreview",
    "setCurveSegmentValue",
    "setSegmentBoundary",
    "splitSegment",
    "mergeSegments",
    "setSegmentLocked",
    "setPanCurvePoint",
    "removePanCurvePoint",
    "setSegmentValue",
    "listSegments",
    "remoteSetChannelConfig",
    "paramBeginGesture",
    "paramSetValue",
    "paramEndGesture",
    "setTrackAutoPanParticipation",
    "analysisProgress",
    "analysisDone",
    "paramChanged",
    "misalign",
    "curves",
    "waveformTile",
    "scvb.segmentList",
];

/**
 * 冻结期望表 —— 「对 05 §1.4 的函数/事件全集零差异」的可执行断言(T25 卡验收原文)。
 * 与 docs/SCVB_CONTRACT.md §7 manifest 同源,**改动必须两处同步**(同 FORBIDDEN 口径)。
 * 相对 05 §1.4 的授权增量共 4 项(契约 §8.4),已在下表就地标注:
 *   - output.functions 的 `setAnalysisConfig`  —— 授权来源 05 §2.4(J69,函数名有 05 字面出处)
 *   - input.events   的 `scvb.error`          —— 授权来源 01 §6.2 + 裁定记录 A-8
 *   - output.functions 的 `confirmPrintGuard` —— 授权来源 04 §5.3 / 05 §2.0 横幅⑦ + 统筹裁定 A-29
 *   - output.functions 的 `setMasterChartMode` —— 授权来源 05 J75 A(T43 双视图);变更文档
 *     docs/contract-changes/20260825-master-chart-mode.md
 *   - output.functions 的 `exportSuggestions` —— 授权来源 07 T41 / 11 §4.2.3 通路 B2 / U12;变更文档
 *     docs/contract-changes/20260825-export-suggestions.md([J81] 转正)
 *   - input.functions  的 `setGuideSeen`      —— 授权来源 05 §3 文末 J80 节 / T48;变更文档
 *     docs/contract-changes/20260825-input-guide-seen.md([J81] 转正)
 * 其余每一项都能在 05 §1.4 的表内逐字找到。
 */
const EXPECTED = {
    output: {
        functions: [
            "requestInitialState",
            "setCaptureEnabled",
            "setOutputEnabled",
            "setGroupId",
            "previewAnalyze",
            "analyze",
            "cancelAnalyze",
            "setRange",
            "setVersionActive",
            "setVersionName",
            "copyVersion",
            "beginParamGesture",
            "setParam",
            "endParamGesture",
            "setChannelConfig",
            "setTrackManual",
            "setPanCurve",
            "setVadParams",
            "setSegmentation",
            "setTransitionRamp",
            "setAnalysisConfig", // ← 授权增量①(05 §2.4 / J69)
            "editSegment",
            "recaptureArm",
            "clearCoverage",
            "undo",
            "redo",
            "requestWaveform",
            "setUiScale",
            "commitUiScale",
            "setLang",
            "setActiveTab",
            "setGuideSeen",
            "setTourSeen",
            "confirmPrintGuard", // ← 授权增量③(04 §5.3 / 05 §2.0 横幅⑦;统筹裁定 A-29)
            "setMasterChartMode", // ← 授权增量④(05 J75 A / T43 双视图;变更文档 20260825-master-chart-mode)
            "exportSuggestions", // ← 授权增量⑤(07 T41 / 11 §4.2.3 通路 B2;U12「进 v1 主线」;变更文档 20260825-export-suggestions,[J81] 转正)
        ],
        events: [
            "scvb.state",
            "scvb.params",
            "scvb.conn",
            "scvb.groups",
            "scvb.meters",
            "scvb.playhead",
            "scvb.captureProgress",
            "scvb.segments",
            "scvb.error",
        ],
    },
    input: {
        functions: [
            "requestInitialState",
            "setChannelId",
            "setGroupId",
            "remoteSetPriority",
            "setUiScale",
            "commitUiScale",
            "setLang",
            "setGuideSeen", // ← 授权增量⑥(05 §3 文末 J80 节 / T48;变更文档 20260825-input-guide-seen,[J81] 转正)
        ],
        events: [
            "scvb.state",
            "scvb.conn",
            "scvb.config",
            "scvb.groups",
            "scvb.error", // ← 授权增量②(01 §6.2 + 裁定 A-8;05 §1.4 Input events 表未列)
        ],
    },
};
/** 计数自检 —— 与契约 §7 文末「计数自检」行同源。 */
const EXPECTED_COUNTS = {
    output: { functions: 36, events: 9 },
    input: { functions: 8, events: 5 },
};

/** 枚举期望值 —— 与 docs/SCVB_CONTRACT.md §5 / §7 同源。 */
const EXPECTED_ENUM_ARRAYS = {
    rangeMode: ["follow", "daw_loop", "manual"],
    editSegmentOp: [
        "move_boundary",
        "split",
        "merge",
        "set_values",
        "set_locked",
    ],
    segmentsReason: [
        "analyze",
        "vad",
        "segmentation",
        "edit",
        "trackManual",
        "undo",
        "redo",
        "versionActive",
        "copyVersion",
        "snapshot",
    ],
    errorCode: [
        "srMismatch",
        "secondOutput",
        "channelConflict",
        "newerState",
        "sidecarMissing",
        "noTimeline",
        "projectCopy",
        "sidecarSwitched",
        "lowSample",
    ],
    claimState: [
        "unassigned",
        "idle",
        "active",
        "conflict",
        "abiMismatch",
        "srMismatch",
    ],
    analysisLoudnessMode: ["kw_integrated", "rms", "peak_dbfs"],
    analysisCenterSlotPolicy: [
        "priority_queue",
        "lead_exclusive",
        "even_spread",
    ],
};
const EXPECTED_CTRL_OP = { kSetPriority: 1, kFpReport: 2 };

const FN_NAME_RE = /^[a-z][A-Za-z0-9]*$/;
const EVT_NAME_RE = /^scvb\.[a-z][A-Za-z0-9]*$/;

const errors = [];
const warns = [];

const log = (msg) => process.stdout.write(msg + "\n");
const ok = (msg) => log("  [OK]    " + msg);
const skip = (msg) => log("  [SKIP]  " + msg);
const fail = (msg) => {
    errors.push(msg);
    log("  [ERROR] " + msg);
};
// ⚠ 想加警告一律走这个 helper,**别直接 `warns.push`**:计数与屏显是一对。
// 裸 push 只进计数、不打 `[WARN]` —— gates.ps1 gate 3i 是按 `[WARN]` **逐行**抓的
// (`$out | Where-Object { $_ -match '\[WARN\]' }`),于是那条「跳过」在门禁屏上**隐身**:
// 某一档真的没跑,汇总表却和全跑过的一次长得一模一样。载荷字段对拍那四处曾经就是裸 push
// (PR#180 复审指出,[SL-286] 改掉)。与「降级路径必须在摘要里可见」同族。
const warn = (msg) => {
    warns.push(msg);
    log("  [WARN]  " + msg);
};
const rel = (p) => relative(REPO_ROOT, p).replace(/\\/g, "/");

// ---------------------------------------------------------------------------
// A 侧:契约 manifest
// ---------------------------------------------------------------------------

log("");
log("== SCVB bridge parity ==");
log("仓库根: " + REPO_ROOT.replace(/\\/g, "/"));
log("");
log("[A] 契约 manifest —— " + rel(CONTRACT_PATH));

if (!existsSync(CONTRACT_PATH)) {
    fail("契约文件不存在:" + rel(CONTRACT_PATH));
    finish();
}

const contractText = readFileSync(CONTRACT_PATH, "utf8");

/** 取出第一个可解析且含 contractVersion 的 ```json 围栏块。 */
function extractManifest(text) {
    const re = /```json\s*\r?\n([\s\S]*?)\r?\n```/g;
    let m;
    while ((m = re.exec(text)) !== null) {
        const raw = m[1];
        let parsed;
        try {
            parsed = JSON.parse(raw);
        } catch (e) {
            continue;
        }
        if (
            parsed &&
            typeof parsed === "object" &&
            "contractVersion" in parsed
        ) {
            return { manifest: parsed, block: m[0] };
        }
    }
    return { manifest: null, block: null };
}

const { manifest, block } = extractManifest(contractText);
if (!manifest) {
    fail(
        "未能在契约中找到可解析的 manifest JSON 块(含 contractVersion 的 ```json 围栏)",
    );
    finish();
}
ok(
    "manifest JSON 可解析;contractVersion = " +
        String(manifest.contractVersion),
);

// 正文(剔除 manifest 块本身),用于「manifest 与正文互相可见」检查
const bodyText = contractText.replace(block, "");

const sides = ["output", "input"];
// monitor 不进 sides(其 4 个通用函数由 WebViewHost 基类注册、正文在 §10),但 manifest 要取出来
// 供下面的 [M] Monitor 专块比对 —— 见该块头注。
const contractFns = { output: [], input: [], monitor: [] };
const contractEvts = { output: [], input: [], monitor: [] };
const contractParams = {
    output: new Map(),
    input: new Map(),
    monitor: new Map(),
};
if (manifest.monitor && typeof manifest.monitor === "object") {
    for (const f of manifest.monitor.functions || [])
        if (f && typeof f.name === "string") contractFns.monitor.push(f.name);
    for (const e of manifest.monitor.events || [])
        if (typeof e === "string") contractEvts.monitor.push(e);
}

for (const side of sides) {
    const node = manifest[side];
    if (!node || typeof node !== "object") {
        fail(`manifest 缺少 "${side}" 节`);
        continue;
    }
    if (!Array.isArray(node.functions)) {
        fail(`manifest.${side}.functions 不是数组`);
    } else {
        for (const [i, fn] of node.functions.entries()) {
            if (!fn || typeof fn.name !== "string") {
                fail(`manifest.${side}.functions[${i}] 缺少 name`);
                continue;
            }
            if (
                !Array.isArray(fn.params) ||
                fn.params.some((p) => typeof p !== "string")
            ) {
                fail(
                    `manifest.${side}.functions[${i}] (${fn.name}) 的 params 必须是字符串数组`,
                );
            } else if (new Set(fn.params).size !== fn.params.length) {
                fail(`manifest.${side}.${fn.name} 的参数名重复`);
            }
            if (typeof fn.returns !== "string" || fn.returns.length === 0) {
                fail(`manifest.${side}.${fn.name} 缺少 returns`);
            }
            if (!FN_NAME_RE.test(fn.name)) {
                fail(`函数名不符合 lowerCamelCase 规范:${side}.${fn.name}`);
            }
            contractFns[side].push(fn.name);
            if (Array.isArray(fn.params))
                contractParams[side].set(fn.name, fn.params);
        }
    }
    if (!Array.isArray(node.events)) {
        fail(`manifest.${side}.events 不是数组`);
    } else {
        for (const [i, ev] of node.events.entries()) {
            if (typeof ev !== "string") {
                fail(`manifest.${side}.events[${i}] 不是字符串`);
                continue;
            }
            if (!EVT_NAME_RE.test(ev)) {
                fail(`事件名不符合 scvb.* 规范:${side}.${ev}`);
            }
            contractEvts[side].push(ev);
        }
    }
}

// 侧内查重(跨侧同名合法:两插件同名同签名)
for (const side of sides) {
    const dupFn = findDuplicates(contractFns[side]);
    const dupEv = findDuplicates(contractEvts[side]);
    if (dupFn.length) fail(`${side} 侧函数名重复:${dupFn.join(", ")}`);
    if (dupEv.length) fail(`${side} 侧事件名重复:${dupEv.join(", ")}`);
}
if (errors.length === 0) ok("函数/事件名无重复、命名规范通过");

log(
    `  计数: output 函数 ${contractFns.output.length} / 事件 ${contractEvts.output.length}` +
        ` · input 函数 ${contractFns.input.length} / 事件 ${contractEvts.input.length}`,
);

// 完备性:manifest 必须与冻结期望表(= 05 §1.4 全集 + §8.4 的 2 项授权增量)零差异
let completenessOk = true;
for (const side of sides) {
    const beforeFn = errors.length;
    compareSets(
        `${side} functions`,
        "期望表(05 §1.4+§8.4)",
        EXPECTED[side].functions,
        "manifest",
        contractFns[side],
    );
    compareSets(
        `${side} events`,
        "期望表(05 §1.4+§8.4)",
        EXPECTED[side].events,
        "manifest",
        contractEvts[side],
    );
    if (errors.length !== beforeFn) completenessOk = false;
    const wantFn = EXPECTED_COUNTS[side].functions;
    const wantEv = EXPECTED_COUNTS[side].events;
    if (contractFns[side].length !== wantFn) {
        fail(
            `${side} 函数计数不符:期望 ${wantFn},实得 ${contractFns[side].length}(契约 §7 计数自检行同源)`,
        );
        completenessOk = false;
    }
    if (contractEvts[side].length !== wantEv) {
        fail(
            `${side} 事件计数不符:期望 ${wantEv},实得 ${contractEvts[side].length}(契约 §7 计数自检行同源)`,
        );
        completenessOk = false;
    }
}
if (completenessOk) {
    ok(
        "完备性:manifest 与 05 §1.4 全集零差异(含 §8.4 的 4 项授权增量),四个计数 35/9/7/5 一致",
    );
}

// 跨侧同名函数必须同签名(params 顺序敏感);returns 显式豁免(§1.4/§3.3 已定差异)
const shared = contractFns.output.filter((n) => contractFns.input.includes(n));
let sigOk = true;
for (const name of shared) {
    const a = contractParams.output.get(name) || [];
    const b = contractParams.input.get(name) || [];
    if (a.length !== b.length || a.some((p, i) => p !== b[i])) {
        fail(
            `跨侧同名函数签名不一致:${name}(output params [${a.join(",")}] vs input params [${b.join(",")}])`,
        );
        sigOk = false;
    }
}
const sharedEvts = contractEvts.output.filter((n) =>
    contractEvts.input.includes(n),
);
if (sigOk) {
    ok(
        `跨侧同名函数 ${shared.length} 项签名一致(${shared.join(", ")});跨侧同名事件 ${sharedEvts.length} 项` +
            `(${sharedEvts.join(", ")});returns 差异按契约 §1.4/§3.3 豁免`,
    );
}

// 禁止复活名单
const allContractNames = new Set([
    ...contractFns.output,
    ...contractFns.input,
    ...contractEvts.output,
    ...contractEvts.input,
]);
const hits = FORBIDDEN.filter((n) => allContractNames.has(n));
if (hits.length) {
    fail("manifest 命中禁止复活名单:" + hits.join(", "));
} else {
    ok(`禁止复活名单 ${FORBIDDEN.length} 项:manifest 零命中`);
}
// 反向:禁止名必须在契约 §8.2 的名单行内以反引号整词列明(防止契约与本脚本清单漂移)。
// 注意:不能用整文 contractText.includes(n) —— `curves`/`misalign` 会被合法现行字段
// `curves_per_track` / `misalignCount` 的子串遮蔽,那条断言将恒真、约束力为零。
const s82Start = contractText.indexOf("### 8.2");
const s83Start = contractText.indexOf("### 8.3");
if (s82Start < 0 || s83Start < 0 || s83Start <= s82Start) {
    fail(
        '契约中未找到 §8.2 禁止复活名单段落(应有 "### 8.2" 与 "### 8.3" 两个小节标题)',
    );
} else {
    const section82 = contractText.slice(s82Start, s83Start);
    const unlisted = FORBIDDEN.filter(
        (n) => !section82.includes("`" + n + "`"),
    );
    if (unlisted.length) {
        fail(
            "契约 §8.2 未以反引号整词列出以下禁止名(契约与脚本清单漂移):" +
                unlisted.join(", "),
        );
    } else {
        ok(
            `禁止复活名单与契约 §8.2 名单行逐项一致(${FORBIDDEN.length} 项整词匹配)`,
        );
    }
}

// 枚举
const enums = manifest.enums;
if (!enums || typeof enums !== "object") {
    fail("manifest 缺少 enums 节");
} else {
    for (const [key, expected] of Object.entries(EXPECTED_ENUM_ARRAYS)) {
        const actual = enums[key];
        if (!Array.isArray(actual)) {
            fail(`enums.${key} 缺失或不是数组`);
            continue;
        }
        const diff = diffSets(new Set(expected), new Set(actual));
        if (
            actual.length !== expected.length ||
            diff.missing.length ||
            diff.extra.length
        ) {
            fail(
                `enums.${key} 取值不符(期望 ${expected.length} 值 [${expected.join("|")}],` +
                    `实得 ${actual.length} 值 [${actual.join("|")}])`,
            );
        }
    }
    const ctrl = enums.ctrlOp;
    if (!ctrl || typeof ctrl !== "object" || Array.isArray(ctrl)) {
        fail("enums.ctrlOp 缺失或不是对象");
    } else {
        for (const [k, v] of Object.entries(EXPECTED_CTRL_OP)) {
            if (ctrl[k] !== v)
                fail(
                    `enums.ctrlOp.${k} 期望 ${v},实得 ${JSON.stringify(ctrl[k])}`,
                );
        }
        for (const k of Object.keys(ctrl)) {
            if (!(k in EXPECTED_CTRL_OP))
                fail(`enums.ctrlOp 出现契约未定义的 op:${k}`);
        }
    }
    if (errors.length === 0) {
        ok(
            "枚举取值全部一致(rangeMode 三值 / editSegmentOp 五值 / segmentsReason 十值 / " +
                "errorCode 九值 / claimState 六值 / ctrlOp 两值 / analysis 两组三值)",
        );
    }
}

// manifest 与正文**双向**可见,且**分侧**检查(跨侧同名的 5 函数 + 4 事件曾是 side-blind 盲区)
const sideBody = splitBodyBySide(bodyText);
let visibilityOk = true;

if (!sideBody) {
    fail(
        '契约正文未找到 "## 1. Output" / "## 3. Input" / "## 5." 三个章节锚点,无法分侧检查可见性',
    );
    visibilityOk = false;
} else {
    // 正向:manifest 每一项必须在**对应侧**正文里有条目
    const invisibleFns = [];
    const invisibleEvts = [];
    for (const side of sides) {
        for (const name of contractFns[side]) {
            if (!sideBody[side].includes(name + "("))
                invisibleFns.push(`${side}.${name}`);
        }
        for (const name of contractEvts[side]) {
            if (!sideBody[side].includes(name))
                invisibleEvts.push(`${side}.${name}`);
        }
    }
    if (invisibleFns.length) {
        fail(
            "以下 manifest 函数在**对应侧**契约正文中无定义条目:" +
                invisibleFns.join(", "),
        );
        visibilityOk = false;
    }
    if (invisibleEvts.length) {
        fail(
            "以下 manifest 事件在**对应侧**契约正文中无定义条目:" +
                invisibleEvts.join(", "),
        );
        visibilityOk = false;
    }

    // 反向:正文里定义了条目(### 小节标题)而 manifest 漏收 => 将来 manifest 与正文漂移会在
    // T28/T29 变成 EXTRA 误报或干脆无人实现,故此处硬断言。
    const uncollected = [];
    for (const side of sides) {
        const { functions, events } = extractBodyEntries(sideBody[side]);
        for (const name of functions) {
            if (!contractFns[side].includes(name))
                uncollected.push(`${side} 函数 ${name}`);
        }
        for (const name of events) {
            if (!contractEvts[side].includes(name))
                uncollected.push(`${side} 事件 ${name}`);
        }
    }
    if (uncollected.length) {
        fail(
            "契约正文定义了以下条目但 §7 manifest 未收录:" +
                uncollected.join(", "),
        );
        visibilityOk = false;
    }
}
if (visibilityOk)
    ok(
        "manifest 与正文**双向**逐项可见(分侧检查:output 段 §1-§2 / input 段 §3-§4)",
    );

// ---------------------------------------------------------------------------
// B 侧:web/shared/bridge.js
// ---------------------------------------------------------------------------

log("");
log("[B] JS 桥后端 —— " + rel(BRIDGE_JS_PATH));

let bridgeNames = null;
let sidesPresent = 0;
if (!existsSync(BRIDGE_JS_PATH)) {
    skip("文件不存在(T27/T28 后产出);跳过 B 侧比对");
} else {
    sidesPresent += 1;
    const src = readFileSync(BRIDGE_JS_PATH, "utf8");
    const fns = extractJsNameTable(src, "BRIDGE_FUNCTIONS");
    const evts = extractJsNameTable(src, "BRIDGE_EVENTS");
    if (!fns || !evts) {
        fail(
            "bridge.js 中未找到 BRIDGE_FUNCTIONS / BRIDGE_EVENTS 名表(见本文件头注释 B 侧口径)",
        );
    } else if (fns.output.length + fns.input.length === 0) {
        fail("bridge.js 的 BRIDGE_FUNCTIONS 名表为空");
    } else {
        // `PENDING_FUNCS` = 待转正桥函数(见 bridge.js 的头注)。它**有意**不进
        // compareSets —— 契约还没改,拿它跟 §7 manifest 比必然红,而那张表正是为了
        // 让「桥面先跑、契约后改」有个不撒谎的落点。
        // 但**禁止复活名单必须照查**:否则往 PENDING_FUNCS 里写一个 §8.2 的死名字
        // 就能绕过机检,「待转正」成了复活通道。缺表按空表处理(该常量是 T43 才引入的,
        // 老版本 bridge.js 里没有,不该因此红)。
        const pending = extractJsNameTable(src, "PENDING_FUNCS") || {
            output: [],
            input: [],
        };
        bridgeNames = { functions: fns, events: evts, pending };
        ok(
            `已抽取:output 函数 ${fns.output.length} / 事件 ${evts.output.length}` +
                ` · input 函数 ${fns.input.length} / 事件 ${evts.input.length}` +
                ` · 待转正 ${pending.output.length + pending.input.length}`,
        );
    }
}

// ---------------------------------------------------------------------------
// C 侧:C++ 常量表
// ---------------------------------------------------------------------------

log("");
log(
    "[C] C++ 常量表 —— " +
        rel(HEADER_PATHS.output) +
        " / " +
        rel(HEADER_PATHS.input),
);

const headerNames = {
    functions: { output: [], input: [] },
    events: { output: [], input: [] },
};
for (const side of sides) {
    const p = HEADER_PATHS[side];
    if (!existsSync(p)) {
        skip(`${rel(p)} 不存在(DeepSeek native 侧产出);跳过 ${side} 侧比对`);
        continue;
    }
    sidesPresent += 1;
    const { functions, events } = extractCppNames(readFileSync(p, "utf8"));
    if (functions.length + events.length === 0) {
        fail(`${rel(p)} 中未抽取到任何桥名(见本文件头注释 C 侧口径)`);
        continue;
    }
    headerNames.functions[side] = functions;
    headerNames.events[side] = events;
    ok(`${rel(p)}:函数 ${functions.length} / 事件 ${events.length}`);
}

// ---------------------------------------------------------------------------
// 三方比对
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// [M] Monitor 侧([J81] manifest.monitor 转正;契约 §10)
//
// 为什么单立一块而不折进上面的 sides 主循环:
//   ① Monitor 的 5 个函数里只有 setObservedGroup 由 MonitorBridgeApi.h 声明,
//      requestInitialState / setUiScale / commitUiScale / setLang 四个由 WebViewHost
//      **基类**注册 —— 折进主循环会让 C++ 侧比对报 4 项「缺少」,而那是设计如此;
//   ② Monitor 正文在契约 §10,不在主循环双向解析的 §1-§4 区段内。
// 故这里做三件事:manifest ↔ bridge.js 零差异、计数 5/4、C++ 头是 manifest 的子集
// 且补集恰为那四个基类函数(写死在 MONITOR_BASE_FNS,少一个多一个都会红)。
const MONITOR_BASE_FNS = [
    "requestInitialState",
    "setUiScale",
    "commitUiScale",
    "setLang",
];
const MONITOR_COUNTS = { functions: 5, events: 4 };
// [SL-262] 路径取自 `HEADER_PATHS.monitor` —— 此前这里是同一路径的**第二份字面量**
// (字符串常量 + 另一种 join 拼法),而 SL-258 的注释却写着「路径只留这一个真源」,
// 那句话当时只对第五节成立。同一语义两个落点(§0.1 第 4 条)正是本卡在收的债。
const MONITOR_HEADER = rel(HEADER_PATHS.monitor);

log("");
log("[M] Monitor 侧 —— 契约 §10 / " + MONITOR_HEADER);
{
    const mFns = contractFns.monitor || [];
    const mEvts = contractEvts.monitor || [];
    if (mFns.length === 0 && mEvts.length === 0) {
        fail("契约 §7 manifest 缺 monitor 块([J81] 已转正,不应缺失)");
    } else {
        if (mFns.length !== MONITOR_COUNTS.functions)
            fail(
                `monitor 函数计数不符:期望 ${MONITOR_COUNTS.functions},实得 ${mFns.length}(契约 §7 计数自检行同源)`,
            );
        if (mEvts.length !== MONITOR_COUNTS.events)
            fail(
                `monitor 事件计数不符:期望 ${MONITOR_COUNTS.events},实得 ${mEvts.length}(契约 §7 计数自检行同源)`,
            );

        if (bridgeNames) {
            compareSets(
                "monitor functions",
                "contract",
                mFns,
                "bridge.js",
                bridgeNames.functions.monitor || [],
            );
            compareSets(
                "monitor events",
                "contract",
                mEvts,
                "bridge.js",
                bridgeNames.events.monitor || [],
            );
            checkForbiddenIn("bridge.js(monitor)", [
                ...(bridgeNames.functions.monitor || []),
                ...(bridgeNames.events.monitor || []),
            ]);
        }

        const mhPath = HEADER_PATHS.monitor;
        if (existsSync(mhPath)) {
            const mh = extractCppNames(readFileSync(mhPath, "utf8"));
            const extra = mh.functions.filter((f) => !mFns.includes(f));
            if (extra.length)
                fail(
                    `monitor functions: C++ header 多出 ${extra.length} 项(manifest 无而 header 有): ${extra.join(", ")}`,
                );
            const missing = mFns.filter((f) => !mh.functions.includes(f));
            const unexpected = missing.filter(
                (f) => !MONITOR_BASE_FNS.includes(f),
            );
            const absentBase = MONITOR_BASE_FNS.filter(
                (f) => !missing.includes(f),
            );
            if (unexpected.length)
                fail(
                    `monitor functions: C++ header 缺少非基类项 ${unexpected.join(", ")}(只有 ${MONITOR_BASE_FNS.join("/")} 允许由 WebViewHost 基类注册)`,
                );
            if (absentBase.length)
                fail(
                    `monitor functions: ${absentBase.join(", ")} 记为基类注册却出现在 C++ header —— 两处注册即双落点(§0.1 第 4 条)`,
                );
            compareSets(
                "monitor events",
                "contract",
                mEvts,
                "C++ header",
                mh.events,
            );
            checkForbiddenIn("C++ header(monitor)", [
                ...mh.functions,
                ...mh.events,
            ]);
            if (!unexpected.length && !absentBase.length && !extra.length)
                ok(
                    `monitor functions: C++ header 声明 ${mh.functions.length} 项 + 基类 ${MONITOR_BASE_FNS.length} 项 = manifest ${mFns.length} 项`,
                );
        } else {
            fail(
                `找不到 ${MONITOR_HEADER}(monitor 已转正进契约 §7,C++ 真源必须在)`,
            );
        }
    }
}

log("");
log("[=] 名字集合比对");

let compared = 0;
for (const side of sides) {
    if (bridgeNames) {
        compared += 1;
        compareSets(
            `${side} functions`,
            "contract",
            contractFns[side],
            "bridge.js",
            bridgeNames.functions[side],
        );
        compareSets(
            `${side} events`,
            "contract",
            contractEvts[side],
            "bridge.js",
            bridgeNames.events[side],
        );
        checkForbiddenIn(`bridge.js(${side})`, [
            ...bridgeNames.functions[side],
            ...bridgeNames.events[side],
            // 待转正表同样过闸(它不进 compareSets,但绝不能成为死名字的复活通道)
            ...bridgeNames.pending[side],
        ]);
    }
    if (headerNames.functions[side].length || headerNames.events[side].length) {
        compared += 1;
        compareSets(
            `${side} functions`,
            "contract",
            contractFns[side],
            "C++ header",
            headerNames.functions[side],
        );
        compareSets(
            `${side} events`,
            "contract",
            contractEvts[side],
            "C++ header",
            headerNames.events[side],
        );
        checkForbiddenIn(`C++ header(${side})`, [
            ...headerNames.functions[side],
            ...headerNames.events[side],
        ]);
    }
}
if (compared === 0) {
    skip(
        sidesPresent === 0
            ? "B 侧与 C 侧均不存在:本次只对契约侧做自检(三方比对留待 T28/T29)"
            : "B/C 侧文件存在但抽取失败(已在上方报错);本次未执行名字集合比对",
    );
}

checkEventPayloadFields();

// ---------------------------------------------------------------------------
// 五、**已注册 handler** 对拍([SL-256] 新增 Output;[SL-258] 扩到 input / monitor 三侧)
// ---------------------------------------------------------------------------
// 为什么补这一节:上面的三方比对查的是**名字集合**(契约 manifest ↔ mock ↔ C++ 常量表)。
// C++ 那一侧读的是各侧 `*BridgeApi.h` 的**常量表** —— 常量写了就算数。可 web 能不能真的
// 调到一个桥函数,取决于该侧 editor 的 `registerNativeFunctions` 有没有把那个名字
// **挂上 handler**;常量表里有、注册表里没有,三方名字集合照样全等,门禁全绿。
//
// 这不是假想,而且**同一族栽了两次**:
//   · `exportSuggestions`(Output)—— 契约 §1.36 齐全、§7 manifest 收录、§5.6 三个 reason 登记、
//     web 与 mock 都实现了、常量表也有,唯独没注册 handler([SL-256] 定谳);
//   · `setGuideSeen`(Input)—— 契约 §3.8 齐全、§7 manifest.input 收录、常量 `kFnSetGuideSeen` 在、
//     `web/input/tour-in.js` 首启链完成与 Skip 都真调用,唯独没注册([SL-258] 定谳)。
//     跨工程的「不再显示」承诺因此从未兑现,而这正是 J80 立 T48 的全部理由。
//
// 第二次栽是**本节自己的覆盖面不够**:[SL-256] 初版只查 Output,头注还写着「Input/Monitor 的
// 注册面不在本仓这一侧」—— 那是假的,`src/input/InputEditor.cpp` 与 `src/monitor/MonitorEditor.cpp`
// 就在本仓。所以本次不是「再补一个洞」,是把门禁扩到**三侧全覆盖**,让这一族不能再有第三次。
//
// 口径:从各侧 `<Role>Editor::registerNativeFunctions` 函数体里抽注册名,经常量表映射成桥名,
// 与 manifest 对应侧的函数名集合**双向**比对。三侧注册形状不同,三种都要认:
//   · Output ——  `add(Fn::Xxx, …)`(先在编辑器里建了 add 助手);
//   · Input / Monitor —— `.withNativeFunction(juce::Identifier(bridge::kFnXxx), …)`;
//   · 通用外壳四件 —— `WebViewHost` 的 `.withNativeFunction(juce::Identifier(bridge::Fn::Xxx), …)`
//     (requestInitialState / setLang / setUiScale / commitUiScale,常量在 `plugin-common/BridgeBase.h`),
//     三侧**共用**同一份注册,故每侧都要并进去。漏读它会把这四个判成「没注册」——
//     本节初版就是这么误报的,它们其实一直是通的。
{
    const hostPath = join(REPO_ROOT, "src", "plugin-common", "WebViewHost.cpp");
    const basePath = join(REPO_ROOT, "src", "plugin-common", "BridgeBase.h");
    const NL = String.fromCharCode(10);

    // **先剥行注释再抽名字**(#163 复审【建议】):正则直接扫源码文本的话,
    // `// add(Fn::ExportSuggestions, …); // 先关掉查个 bug` 会被算成「已注册」,门禁绿 ——
    // 而「有人临时注释掉一行注册忘了恢复」恰恰是本节最想拦的那一族(两次定谳的那两个洞,
    // 只是把「注释掉」换成了「从没写」)。块注释那一族见下(SL-262 已补剥离)。
    // [SL-262] **先剥块注释再剥行注释**。只剥 `//` 的话,整块 `/* … */` 注释掉的注册会被算成
    // 「已注册」——失败方向是**假绿**,恰是本节最想拦的那一族(历史两次栽的是「从没写」,
    // 但「临时整块注释掉忘了恢复」是同一形状)。
    // **顺序是有意的取舍**(#169 复审订正了我原本写反的理由):块优先的代价是
    // `// 见 /* 这里` 这种行注释里的 `/*` 会启动一次跨段吞噬、一路吃到下一个 `*/`,
    // 把中间真实的注册行一起剥掉 —— 方向是**假红**(误报「没注册」),可接受;
    // 行优先则会让 `/* … // … */` 漏剥,方向是**假绿**,不可接受。故取块优先。
    // (现四个被扫文件里没有任何 `//` 行含 `/*`,不是活 bug。)
    const stripBlockComments = (src) => src.replace(/\/\*[\s\S]*?\*\//g, "");
    const stripLineComments = (src) =>
        stripBlockComments(src)
            .split(NL)
            .map((line) => {
                const i = line.indexOf("//");
                return i < 0 ? line : line.slice(0, i);
            })
            .join(NL);

    // 三种注册形状的抽取:返回常量标识符(尚未经常量表映射成桥名)。
    const identsOf = (body) => {
        const out = [];
        for (const m of body.matchAll(
            /add\(\s*Fn::([A-Za-z_][A-Za-z0-9_]*)/g,
        )) {
            out.push(m[1]);
        }
        for (const m of body.matchAll(
            /withNativeFunction\(\s*juce::Identifier\(\s*bridge::(?:Fn::)?([A-Za-z_][A-Za-z0-9_]*)/g,
        )) {
            out.push(m[1]);
        }
        return out;
    };

    // 通用外壳四件:三侧共用,抽一次。
    const hostIdents = existsSync(hostPath)
        ? identsOf(stripLineComments(readFileSync(hostPath, "utf8")))
        : [];

    const SIDES = [
        {
            side: "output",
            editor: join(REPO_ROOT, "src", "output", "OutputEditor.cpp"),
            marker: "void OutputEditor::registerNativeFunctions",
            header: HEADER_PATHS.output,
        },
        {
            side: "input",
            editor: join(REPO_ROOT, "src", "input", "InputEditor.cpp"),
            marker: "void InputEditor::registerNativeFunctions",
            header: HEADER_PATHS.input,
        },
        {
            side: "monitor",
            editor: join(REPO_ROOT, "src", "monitor", "MonitorEditor.cpp"),
            marker: "void MonitorEditor::registerNativeFunctions",
            header: HEADER_PATHS.monitor,
        },
    ];

    for (const cfg of SIDES) {
        log("");
        log(
            `[E] 已注册 handler 对拍 —— ${cfg.marker.replace("void ", "").replace("::registerNativeFunctions", "")}::registerNativeFunctions ↔ manifest.${cfg.side}`,
        );
        const contractSide = contractFns[cfg.side];
        if (!Array.isArray(contractSide) || contractSide.length === 0) {
            skip(
                `契约 manifest.${cfg.side} 为空,跳过 ${cfg.side} 侧已注册 handler 对拍`,
            );
            continue;
        }
        if (!existsSync(cfg.editor) || !existsSync(cfg.header)) {
            // [SL-258 复审【建议】] **不能 skip**。本节存在的全部理由就是「不能静默变绿」——
            // 三侧的 editor 与 header 都是已知在仓的文件,不是可选依赖;路径写错 / 文件改名 /
            // 目录重构都会让这一节静默跳过,而「找不到 marker」那条走的是 fail,两者必须同档。
            fail(
                `读不到 ${rel(cfg.editor)} 或 ${rel(cfg.header)} —— ${cfg.side} 侧注册面/常量表移动了?本节口径需同步`,
            );
            continue;
        }
        const editorSrc = readFileSync(cfg.editor, "utf8");
        const start = editorSrc.indexOf(cfg.marker);
        if (start < 0) {
            fail(
                `找不到 ${cfg.marker} —— ${cfg.side} 侧注册面改名了?本节口径需同步`,
            );
            continue;
        }
        const end = editorSrc.indexOf(NL + "}" + NL, start);
        // 函数体截断前提:体内**没有列首裸 `}` 行**(三侧当前都成立 —— 体是一串平铺注册调用,
        // 多行 lambda 收尾是缩进的 `});` / `    };`,不是 `\n}\n`)。将来若在体内加独立 `{…}` 块
        // 会提前截断、漏抽后续注册 —— 那是**少抽 ⇒ 误红**,失败方向安全,不至于放过真洞。
        const body = stripLineComments(
            editorSrc.slice(start, end > 0 ? end : editorSrc.length),
        );
        // 常量名 → 桥名(`inline constexpr const char* Xxx = "yyy";`,允许跨行折行)。
        // 每侧只取**本侧头 + 共用 BridgeBase.h**:三侧常量表各自独立,合并会让 A 侧的名字
        // 在 B 侧被解析成功、把「B 没注册」洗成绿。
        const constMap = new Map();
        for (const hp of [cfg.header, basePath]) {
            if (!existsSync(hp)) continue;
            for (const m of readFileSync(hp, "utf8").matchAll(
                /constexpr\s+const\s+char\*\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"([^"]+)"/g,
            )) {
                constMap.set(m[1], m[2]);
            }
        }
        const registered = new Set();
        const unresolved = [];
        for (const ident of [...identsOf(body), ...hostIdents]) {
            const bridgeName = constMap.get(ident);
            if (bridgeName) registered.add(bridgeName);
            else unresolved.push(ident);
        }
        if (unresolved.length > 0) {
            fail(
                `${cfg.side} 侧注册面用到的常量在常量表里查不到:${unresolved.join(", ")}` +
                    " —— 常量表与注册面对不上,本节无法判定",
            );
            continue;
        }
        if (registered.size === 0) {
            fail(`${cfg.marker} 里抽不到任何注册调用,本节口径需同步`);
            continue;
        }
        const contracted = new Set(contractSide);
        const notRegistered = [...contracted].filter((n) => !registered.has(n));
        const notContracted = [...registered].filter((n) => !contracted.has(n));
        if (notRegistered.length > 0) {
            fail(
                `契约 §7 manifest.${cfg.side} 登记了但**没注册 handler**:${notRegistered.join(", ")}` +
                    " —— web 在真宿主上调不到它(常量表有名字不等于挂了 handler)",
            );
        }
        if (notContracted.length > 0) {
            fail(
                `${cfg.side} 侧注册了但契约 §7 manifest 没登记:${notContracted.join(", ")}` +
                    " —— 桥面多出契约外的函数,须走 §9 补契约",
            );
        }
        if (notRegistered.length === 0 && notContracted.length === 0) {
            ok(
                `${cfg.side}:已注册 handler ${registered.size} 个,与 manifest.${cfg.side} 函数名集合双向相等`,
            );
        }
    }
}

finish();

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

function finish() {
    log("");
    if (errors.length > 0) {
        log(
            `check-bridge-parity 失败:${errors.length} 处差异/违规` +
                (warns.length ? `,${warns.length} 处警告` : ""),
        );
        for (const e of errors) log("  - " + e);
        process.exit(1);
    }
    log(
        "check-bridge-parity 通过" +
            (warns.length ? `(${warns.length} 处警告)` : ""),
    );
    process.exit(0);
}

function findDuplicates(arr) {
    const seen = new Set();
    const dup = new Set();
    for (const x of arr) {
        if (seen.has(x)) dup.add(x);
        seen.add(x);
    }
    return [...dup];
}

function diffSets(expected, actual) {
    return {
        missing: [...expected].filter((x) => !actual.has(x)),
        extra: [...actual].filter((x) => !expected.has(x)),
    };
}

function compareSets(label, nameA, listA, nameB, listB) {
    const { missing, extra } = diffSets(new Set(listA), new Set(listB));
    if (missing.length)
        fail(
            `${label}: ${nameB} 缺少 ${missing.length} 项(${nameA} 有而 ${nameB} 无): ${missing.join(", ")}`,
        );
    if (extra.length)
        fail(
            `${label}: ${nameB} 多出 ${extra.length} 项(${nameA} 无而 ${nameB} 有): ${extra.join(", ")}`,
        );
    if (!missing.length && !extra.length)
        ok(`${label}: ${nameA} <-> ${nameB} 零差异(${listA.length} 项)`);
}

function checkForbiddenIn(where, names) {
    const set = new Set(names);
    const hit = FORBIDDEN.filter((n) => set.has(n));
    if (hit.length) fail(`${where} 命中禁止复活名单:${hit.join(", ")}`);
}

/**
 * 把正文切成 output 段(§1-§2)与 input 段(§3-§4);§5 起(共享枚举 / ctrl 表 / 附录)截断,
 * 因为那里对函数名与事件名的引用不构成「定义条目」。
 */
function splitBodyBySide(text) {
    const iOut = text.indexOf("\n## 1. Output");
    const iIn = text.indexOf("\n## 3. Input");
    const iEnd = text.indexOf("\n## 5.");
    if (iOut < 0 || iIn < 0 || iEnd < 0 || !(iOut < iIn && iIn < iEnd))
        return null;
    return { output: text.slice(iOut, iIn), input: text.slice(iIn, iEnd) };
}

/**
 * 从一侧正文抽取「定义条目」:只看 `### ` 小节标题行,取反引号包裹的 `name(` 作函数名、
 * `scvb.xxx` 作事件名。兼容合并标题(如 "### 1.25 `undo()` / 1.26 `redo()`")。
 */
function extractBodyEntries(text) {
    const functions = new Set();
    const events = new Set();
    for (const rawLine of text.split(/\r?\n/)) {
        if (!rawLine.startsWith("### ")) continue;
        for (const m of rawLine.matchAll(/`([a-z][A-Za-z0-9]*)\s*\(/g))
            functions.add(m[1]);
        for (const m of rawLine.matchAll(/`(scvb\.[a-z][A-Za-z0-9]*)`/g))
            events.add(m[1]);
    }
    return { functions: [...functions], events: [...events] };
}

/**
 * 从 bridge.js 抽取 `export const <TABLE> = { output: [...], input: [...] };`
 * 只读字符串字面量,不执行任何代码(bridge.js 依赖浏览器全局,不可 import)。
 */
function extractJsNameTable(src, tableName) {
    const re = new RegExp(`${tableName}\\s*=\\s*\\{([\\s\\S]*?)\\}\\s*;`);
    const m = re.exec(src);
    if (!m) return null;
    const body = m[1];
    const out = { output: [], input: [], monitor: [] };
    for (const side of ["output", "input", "monitor"]) {
        const sideRe = new RegExp(`${side}\\s*:\\s*\\[([\\s\\S]*?)\\]`);
        const sm = sideRe.exec(body);
        if (!sm) continue;
        out[side] = [...sm[1].matchAll(/["'`]([^"'`]+)["'`]/g)].map(
            (x) => x[1],
        );
    }
    return out;
}

/**
 * 从 C++ 头文件抽取 constexpr 字符串字面量;"scvb." 前缀归事件,其余归函数。
 * 带 parity-ignore 注释的行、以及注释行(// 开头)一律跳过。
 */
function extractCppNames(src) {
    const functions = [];
    const events = [];
    // 按**逻辑语句**聚合而不是逐行:clang-format 会把过长的声明折成
    //   inline constexpr const char* kEvPlayhead =
    //       "scvb.playhead"; // ...
    // 逐行匹配会因为「constexpr 与字面量不在同一行」而**静默漏抽**这个名字 ——
    // 漏抽的后果是 C++ 里明明声明了、parity 却看不见,比报错更难查
    // (本条由 [J81] 转正时新增的 Monitor 比对块当场逮到:kEvPlayhead 就是这么丢的)。
    const statements = [];
    let buf = "";
    for (const rawLine of src.split(/\r?\n/)) {
        const line = rawLine.trim();
        if (
            line.startsWith("//") ||
            line.startsWith("*") ||
            line.includes("parity-ignore")
        ) {
            buf = "";
            continue;
        }
        buf = buf ? buf + " " + line : line;
        if (line.includes(";")) {
            statements.push(buf);
            buf = "";
        }
    }
    if (buf) statements.push(buf);
    for (const line of statements) {
        if (!/constexpr/.test(line)) continue;
        for (const m of line.matchAll(/"([^"]+)"/g)) {
            const v = m[1];
            if (v.startsWith("scvb.")) events.push(v);
            else if (FN_NAME_RE.test(v)) functions.push(v);
        }
    }
    return { functions: [...new Set(functions)], events: [...new Set(events)] };
}

// ---------------------------------------------------------------------------
// 四、事件**载荷字段**对拍(v5.3 R5 新增)
// ---------------------------------------------------------------------------
// 为什么补这一节:上面三方比对的是**名字集合**(函数名 / 事件名),它**看不见载荷字段**。
// 于是「给 scvb.conn 加一个 suspended、给 scvb.segments 加一个 openEnded」这类改动,
// 三方全绿而契约文档一个字没动 —— 契约 §7.4 要求载荷新增走 §9,却没有任何机器在查。
// 这条挂了六轮评审才被人眼抓到;补上之后第七轮不可能存在。
//
// 口径:从契约的「载荷」行抽出字段名集合,与 OutputEditor.cpp 里**实际 put 出去**的
// 字段名集合对拍。两侧都用最保守的抽取(只认字面量键名);抽不到就报**警告**而不是假绿 ——
// 一个「悄悄跳过」的门禁比没有门禁更坏。
function checkEventPayloadFields() {
    log("");
    log("=== 四、事件载荷字段对拍(契约「载荷」行 vs 实发 put) ===");

    const editorPath = join(REPO_ROOT, "src", "output", "OutputEditor.cpp");
    if (!existsSync(editorPath)) {
        warn("载荷字段对拍:读不到 OutputEditor.cpp,跳过");
        return;
    }
    const editor = readFileSync(editorPath, "utf8");
    const contract = readFileSync(CONTRACT_PATH, "utf8");

    // 每条:事件名 / 契约「载荷」行的定位锚 / C++ 侧装配该条目的 juce::var 变量名 /
    // 该事件里不属于这一层对象的键(顶层或别层,列出来免得误判成缺失)。
    const CASES = [
        {
            event: "scvb.conn",
            anchor: "slotState",
            fn: "OutputEditor::buildConnPayload",
            varName: "ch",
            otherLevels: ["channels", "outputReadOnly", "generation"],
        },
        {
            event: "scvb.segments",
            anchor: "segIdx",
            fn: "OutputEditor::buildSegmentsPayload",
            varName: "seg",
            otherLevels: [
                "version",
                "reason",
                "channels",
                "diff",
                "ch",
                "segments",
                "stale",
            ],
        },
    ];

    for (const c of CASES) {
        const line = contract
            .split(/\r?\n/)
            .find((l) => l.includes("| 载荷 |") && l.includes(c.anchor));
        if (!line) {
            warn(
                `载荷字段对拍:契约里找不到 ${c.event} 的载荷行(锚点 ${c.anchor}),跳过`,
            );
            continue;
        }
        const docFields = new Set(
            [...line.matchAll(/([A-Za-z_][A-Za-z0-9_]*)\s*:/g)].map(
                (m) => m[1],
            ),
        );

        // **必须限定在装配该事件的那个函数体内**:`ch` / `seg` 这类变量名在别处的 payload
        // 装配里也用(比如 scvb.state 的逐通道配置也叫 `ch`),全文件扫会把别处的字段算进来
        // —— 那是**假红**,而假红会让人把门禁关掉,比没门禁更坏。
        const fnStart = editor.indexOf(c.fn);
        if (fnStart < 0) {
            warn(`载荷字段对拍:找不到 ${c.fn},跳过`);
            continue;
        }
        const NL = String.fromCharCode(10);
        const fnEnd = editor.indexOf(NL + "}" + NL, fnStart);
        const body = editor.slice(fnStart, fnEnd > 0 ? fnEnd : editor.length);
        const re = new RegExp(
            `put\\(\\s*${c.varName}\\s*,\\s*"([A-Za-z_][A-Za-z0-9_]*)"`,
            "g",
        );
        const codeFields = new Set([...body.matchAll(re)].map((m) => m[1]));
        if (codeFields.size === 0) {
            warn(
                `载荷字段对拍:${c.event} 在实现里抽不到 put(${c.varName}, …),跳过`,
            );
            continue;
        }

        const missingInDoc = [...codeFields].filter(
            (f) => !docFields.has(f) && !c.otherLevels.includes(f),
        );
        if (missingInDoc.length > 0) {
            errors.push(
                `${c.event} 实发了契约未登记的载荷字段:${missingInDoc.join(", ")}` +
                    " —— 契约 §7.4:载荷新增须走 §9(补契约载荷行 + 变更文档)",
            );
        } else {
            log(
                `  [OK]   ${c.event}:${codeFields.size} 个实发字段均已在契约载荷行登记`,
            );
        }
    }
}
