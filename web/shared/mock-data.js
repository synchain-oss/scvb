// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB — 预览模式假数据生成器 + tour demo 快照
// =============================================================================
// 定位(05 §0.1 / 06 §6.2):
//   本文件是 web/ 内**自包含**的假数据源,只产「生成器 + fifteen-tracks 快照」两类东西:
//     ①  make*() 生成器 —— 按契约形状造载荷,供 web-preview 的 mock 后端(T28)与截图/断言使用;
//     ②  FIFTEEN_TRACKS —— J62 交互式引导(tour)的 demo 数据源(05 §2.6),健康满配 15 轨。
//   六个场景 fixture(connected / passthrough / no-output / group-empty / … )归 T28,不在本文件。
//   **禁止 import web-preview/ 的任何东西**(依赖方向 web/ ← web-preview/,06 §6.2);
//   本文件同样不 import bridge.js —— 它只造数据,不碰传输。
//
// 真源:docs/SCVB_CONTRACT.md v1.0(冻结)。
//   §1.1  Output 快照全字段 / §3.1 Input 快照全字段
//   §2.1-§2.9 Output 事件载荷 / §4.1-§4.5 Input 事件载荷
//   §1.27 requestWaveform 返回形状(六数组 + valleys[])
//   §5    共享枚举(九错误码 / claim 六态 / range 三值 / editSegment 五 op / segments reason 十值)
//   §7    manifest(本文件的 ENUMS 是 manifest enums 的镜像,**只用于造数据与自检**,
//         契约的唯一真源永远是 SCVB_CONTRACT.md —— 契约改了要回改这里,不得反向)。
//   轨道画像(label / pan / vol / 优先级 / 配对 / stereo 标)参照设计稿
//   masterPlan/assets/design-v2/「SCVB 设计稿.dc.html」1258 行 CH_LABELS 与 1382-1409 行 TRACKS/SEGMENTS。
//
// 确定性(硬纪律):
//   全文件**不出现** Math.random / Date.now / new Date / Math.sin / Math.cos。
//   随机感一律来自整数 hash + 值噪声(valueNoise):同参数 → 逐位相同的输出,
//   预览截图与断言可复现,且跨 JS 引擎一致(只用 IEEE 精确运算 +、-、*、/ 与 Math.imul/floor)。
//   makeRng() 是给调用方用的种子 LCG,本文件自身的数据一律走 hash(与调用顺序无关)。
//
// 数值合法性:pan -100..+100 / volDb -24..+12 / priority 0..10 / pair_id 0|1..7 /
//   freeze 0..3 / ch 1..15 / g 1..8 / v 1..2 / 枚举值全部取契约 §5 集合。
//   参数越界一律 throw(这是给写手的护栏,不是契约行为 —— 契约 §0.8 的夹取/拒绝归 C++)。
//
// 键名拼写(契约 §0.2):state 镜像键 snake_case(group_id / capture_enabled / source_channels …),
//   纯传输/计算键 lowerCamelCase(tracksMask / startS / volDb / slotState / heartbeatFresh …)。
//   两套拼写在同一对象里并存是**契约要求**,不是笔误。
//
// 数组一律用普通 Array(不用 TypedArray):桥面实际走 JSON,TypedArray 过不了 JSON。
// =============================================================================

// -----------------------------------------------------------------------------
// 0. 契约常量与枚举镜像(§7 manifest enums 逐字)
// -----------------------------------------------------------------------------

/** 通道数(契约 §0.2:ch = 1..15,J59)。 */
export const CHANNEL_COUNT = 15;

/** 版本数(契约 §0.2:v = 1..2,J59)。 */
export const VERSION_COUNT = 2;

/** 组数(契约 §0.2:g = 1..8,UI 显示 A-H,J66)。 */
export const GROUP_COUNT = 8;

/** 电平地板(契约 §2.5:未连接/静音轨发 -60 dB)。 */
export const METER_FLOOR_DB = -60;

/** 波形未覆盖列的哨兵(契约 §1.27:covered=0 且 minDb=maxDb=-160)。 */
export const WAVEFORM_UNCOVERED_DB = -160;

/** `heartbeatAgeMs` 的「无数据」哨兵(契约 §2.3:slotState=0/从未心跳 → 0xFFFFFFFF)。 */
export const HEARTBEAT_AGE_NONE = 0xffffffff;

/**
 * `diff.changed[]` 的条数封顶 —— 与 native 的
 * `src/core/output/SegmentDiff.h::kMaxChangedItems` 和
 * `web/output/tab-wave.js::DIFF_CHANGED_CAP` **三处同值**(门禁见 smoke-mock.mjs)。
 *
 * [SL-274 复审第 3 轮] 原先这里是 `makeSegments` 里的**裸字面量** `200`,而门禁靠正则
 * `changed.length < (\d+)` 去读它 —— 那等于**把「给这个数起个名字」判成红**:谁写成
 * `changed.length < SOME_CONST`,门禁就会以「找不到常量」的名义拦住他。起了名之后
 * 门禁改成直接 import 这个值,比正则更硬(比的是**运行时真值**,不是源码长相)。
 */
export const DIFF_CHANGED_CAP = 200;

/**
 * Tab1 分布图视图两态([J75] T43 的 state `ui.master_chart_mode`)。
 *
 * **刻意不进 `ENUMS`**:那个对象的头注写着「契约 §5 / §7 manifest 的枚举镜像」,
 * 而本枚举还没进契约 —— 它随 `docs/contract-changes/20260825-master-chart-mode.md`
 * 走冻结变更流程,转正后再并入 ENUMS。UI 侧(`web/output/tab-master.js`)另有一份
 * 同值常量,理由同 `CHANNEL_COUNT` 那条:正式页面不为一个常量把整份 demo 数据拖进包里。
 */
export const CHART_MODES = Object.freeze(["distribution", "trajectory"]);

/** 契约 §5 / §7 manifest 的枚举镜像。只读,供生成器取值与调用方自检。 */
export const ENUMS = Object.freeze({
    rangeMode: Object.freeze(["follow", "daw_loop", "manual"]),
    editSegmentOp: Object.freeze([
        "move_boundary",
        "split",
        "merge",
        "set_values",
        "set_locked",
    ]),
    segmentsReason: Object.freeze([
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
    ]),
    errorCode: Object.freeze([
        "srMismatch",
        "secondOutput",
        "channelConflict",
        "newerState",
        "sidecarMissing",
        "noTimeline",
        "projectCopy",
        "sidecarSwitched",
        "lowSample",
    ]),
    claimState: Object.freeze([
        "unassigned",
        "idle",
        "active",
        "conflict",
        "abiMismatch",
        "srMismatch",
    ]),
    analysisLoudnessMode: Object.freeze(["kw_integrated", "rms", "peak_dbfs"]),
    analysisCenterSlotPolicy: Object.freeze([
        "priority_queue",
        "lead_exclusive",
        "even_spread",
    ]),
    // 以下三组不在 manifest enums 里,但正文有闭集,造数据要用:
    segmentOrigin: Object.freeze(["auto", "user_edited", "user_created"]), // §2.8
    activeTab: Object.freeze(["master", "tracks", "wave", "settings"]), // §1.31
    language: Object.freeze(["zh", "en", "fr"]), // §1.30
    segmentationMode: Object.freeze(["vad_only", "valley"]), // 02 §3.1(契约只写 mode:string)
});

// -----------------------------------------------------------------------------
// 1. 确定性伪随机:整数 hash + 值噪声 + 种子 LCG
// -----------------------------------------------------------------------------

/**
 * 32 位整数混合 hash(MurmurHash3 finalizer 家族)。
 * 只用 Math.imul 与移位 —— 结果与平台/引擎无关。
 */
function hash32(a, b) {
    let h = Math.imul(a | 0, 0x9e3779b1) ^ Math.imul(b | 0, 0x85ebca77);
    h ^= h >>> 15;
    h = Math.imul(h, 0x2c1b3c6d);
    h ^= h >>> 12;
    h = Math.imul(h, 0x297a2d39);
    h ^= h >>> 15;
    return h >>> 0;
}

/** hash → [0,1) 的确定性单位值。 */
function unit(a, b) {
    return hash32(a, b) / 4294967296;
}

/**
 * 一维值噪声:对整数格点取 hash,再用 smoothstep 插值。
 * 只有 +、-、*、/ 与 Math.floor —— 全是 IEEE 精确运算,跨引擎逐位一致
 * (这也是本文件不用 Math.sin 造波形的原因:sin 的最后一位在不同引擎会漂)。
 */
function valueNoise(seed, x) {
    const i = Math.floor(x);
    const f = x - i;
    const a = unit(seed, i);
    const b = unit(seed, i + 1);
    const u = f * f * (3 - 2 * f);
    return a + (b - a) * u;
}

/**
 * 种子 LCG(数值配方同 Numerical Recipes)。
 * 供调用方需要「一串随机数」时用;**本文件内部的数据一律走 hash**,
 * 这样任何一个生成器都不受调用顺序影响。
 * @param {number} seed 任意 32 位整数;0 视作 1。
 * @returns {() => number} 每次返回 [0,1) 的确定性伪随机数。
 */
export function makeRng(seed = 1) {
    let s = seed >>> 0 || 1;
    return function rng() {
        s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
        return s / 4294967296;
    };
}

// -----------------------------------------------------------------------------
// 2. 小工具
// -----------------------------------------------------------------------------

/** 四舍五入到 n 位小数(避免 JSON 里出现 0.30000000000000004 这种噪声)。 */
function round(x, n = 2) {
    const p = 10 ** n;
    return Math.round(x * p) / p;
}

function clamp(x, lo, hi) {
    return x < lo ? lo : x > hi ? hi : x;
}

function isPlainObject(v) {
    return v !== null && typeof v === "object" && !Array.isArray(v);
}

/**
 * 深合并 overrides:普通对象递归合并,数组与标量**整体替换**。
 * base 一律是刚 new 出来的对象,所以就地改是安全的。
 */
function mergeDeep(base, patch) {
    if (!isPlainObject(patch)) return base;
    for (const key of Object.keys(patch)) {
        const v = patch[key];
        if (isPlainObject(v) && isPlainObject(base[key]))
            mergeDeep(base[key], v);
        else base[key] = v;
    }
    return base;
}

/** 递归冻结(FIFTEEN_TRACKS 是全局共享的 demo 数据,谁都不许就地改)。 */
function deepFreeze(o) {
    if (o === null || typeof o !== "object" || Object.isFrozen(o)) return o;
    Object.freeze(o);
    for (const key of Object.keys(o)) deepFreeze(o[key]);
    return o;
}

function assertInt(name, v, lo, hi) {
    if (!Number.isInteger(v) || v < lo || v > hi) {
        throw new RangeError(
            `mock-data: ${name} 须为 ${lo}..${hi} 的整数,收到 ${v}`,
        );
    }
    return v;
}

function assertEnum(name, v, list) {
    if (!list.includes(v)) {
        throw new RangeError(
            `mock-data: ${name} 须取 [${list.join(" | ")}],收到 ${v}`,
        );
    }
    return v;
}

/** 1..15 的通道号数组。 */
function allChannels() {
    return Array.from({ length: CHANNEL_COUNT }, (_, i) => i + 1);
}

/** ParamID 的两位零填充轨号(契约 §1.12-§1.14:t = "01".."15")。 */
function tt(ch) {
    return String(ch).padStart(2, "0");
}

// -----------------------------------------------------------------------------
// 3. 轨道画像(设计稿 1258 / 1382-1397 行转写)
// -----------------------------------------------------------------------------
//
// 设计稿的 `vol` 是推子行程 0..1,不是 dB。换算 volDb = -24 + 36*v:
//   契约/params-v0 的 vol 值域是 -24..+12(36 dB 跨度),J03 又要求 0 dB 落在行程 2/3 处 ——
//   线性映射下 0 dB 恰好在 v=2/3,两条约束自洽,故此换算不是拍脑袋。
// 设计稿的 `lv`/`pk` 是电平表填充比例 0..1,换算 db = -60 + 60*lv(地板 -60,契约 §2.5)。
// 设计稿 14/15 轨无 label(mock 里是空位),tour demo 要「15 轨全连接」,
// 故 14/15 由本卡按 CH_LABELS 风格补名(见 T27 差异清单第二波)。

const DEMO_TRACKS = [
    // ch 由下标推得;列序:label, stereo, pan, width, vol(行程), lv, prio, lead, volEx, pair, freeze
    ["主唱1", 0, -4, 100, 0.62, 0.74, 5, 1, 0, 1, 0],
    ["主唱2", 0, 12, 100, 0.55, 0.61, 4, 0, 0, 1, 0],
    ["和声 L", 1, -58, 82, 0.44, 0.48, 3, 0, 0, 2, 0],
    ["和声 R", 1, 58, 82, 0.44, 0.46, 3, 0, 0, 2, 0],
    ["和声 C", 0, 0, 100, 0.4, 0.4, 3, 0, 0, 0, 0],
    ["Ad-lib 1", 0, -34, 100, 0.38, 0.31, 2, 0, 0, 0, 1],
    ["Ad-lib 2", 0, 34, 100, 0.38, 0.29, 2, 0, 0, 0, 0],
    ["低八度", 0, -18, 100, 0.34, 0.28, 2, 0, 1, 0, 0],
    ["高八度", 0, 18, 100, 0.34, 0.26, 2, 0, 0, 0, 0],
    ["和唱 L", 1, -76, 64, 0.3, 0.22, 1, 0, 0, 3, 0],
    ["和唱 R", 1, 76, 64, 0.3, 0.21, 1, 0, 0, 3, 0],
    ["音效轨", 0, 0, 100, 0.25, 0.16, 1, 0, 0, 0, 0],
    ["念白", 0, 26, 100, 0.42, 0.18, 1, 0, 0, 0, 2],
    ["和唱 C", 0, 6, 100, 0.32, 0.24, 1, 0, 0, 0, 0],
    ["outro", 0, -12, 100, 0.28, 0.19, 1, 0, 0, 0, 0],
].map((row, i) => ({
    ch: i + 1,
    label: row[0],
    sourceChannels: row[1] === 1 ? 2 : 1,
    pan: row[2],
    width: row[3],
    volDb: round(-24 + 36 * row[4], 1),
    meterBaseDb: round(-60 + 60 * row[5], 1),
    priority: row[6],
    leadLock: row[7] === 1,
    leadVolExempt: row[8] === 1,
    pairId: row[9],
    freeze: row[10],
}));

/** tour demo 的 15 条轨名(设计稿 CH_LABELS + 本卡补的 14/15),中文原值。 */
export const DEMO_LABELS = Object.freeze(DEMO_TRACKS.map((t) => t.label));

/**
 * 与 `DEMO_LABELS` 一一对应的 i18n key(`web/shared/i18n.js` 的 `demo.ch1..demo.ch15`)。
 *
 * 为什么要有这一份:用户轨的 label 是**用户数据**、三语共用原值天经地义;而 tour demo 的 15 条轨名
 * 是**我们自己造的 UI 内容**,05 §5「切语言同步全部可见文案」对它成立 —— 否则 EN/FR 用户走 tour 时
 * 讲解框是英/法文、被讲解的 15 条轨全是中文名。
 *
 * 本文件**有意不 import i18n.js**(06 §6.2 依赖方向:mock-data 是数据源,不依赖 UI 层),
 * 因此只给 key 表、不做翻译:T31 渲染 tour demo 时决定用 `DEMO_LABELS[i]` 还是
 * `t(DEMO_LABEL_KEYS[i])`。两份表**必须同改**(i18n.js 的 zh 侧逐字 = 本文件的 label)。
 */
export const DEMO_LABEL_KEYS = Object.freeze(
    DEMO_TRACKS.map((t) => `demo.ch${t.ch}`),
);

/** tour demo 的 V1 版本名词条 key(zh 值 = 下方快照里的「基础平衡」,同改纪律同上;
 *  PR #32 deepseek-review 建议 5:版本名同为我方 demo 内容,切语言应可跟随)。 */
export const DEMO_VERSION_NAME_KEY = "demo.versionName";

/**
 * demo 轨名本地化(仅 web-preview mock 在**构建 demo store/fixture** 时调用;
 * 真实插件数据永不经过 mock 构建 → 零误伤 —— 与 tour.js buildDemoStore 同一落点)。
 *
 * 把 demo 快照的 channels[].label 换成目标语言的 demo.ch* 词条;**仅当某轨 label 仍是
 * demo 词条(zh/en/fr 任一语言的 demo.ch* 原值)才换**,用户改过的 label(非 demo 词条)
 * 原样保留 —— 这样四消费面(轨列表 / wave 泳道轨头 / Lead Select / 改名框)读到的
 * 是同一份已本地化的 label,不再渲染层各打补丁。
 *
 * @param {object[]} channels demo 快照的 channels(15 条,label 为 demo 词条)
 * @param {string} lang       目标语言 "zh"|"en"|"fr"
 * @param {object} T          三语字典 { zh, en, fr }(调用方 import 传入;
 *                            本文件不 import i18n.js,依赖方向不变)
 * @returns {object[]} 新的 channels(不改写入参;非 demo label 的轨原样保留)
 */
export function localizeDemoChannels(channels, lang, T) {
    if (!Array.isArray(channels) || !T) return channels;
    const t = T[lang] || {};
    return channels.map((c, i) => {
        const key = "demo.ch" + (i + 1);
        const demo = new Set([DEMO_LABELS[i]]);
        for (const l of ["zh", "en", "fr"]) {
            const v = T[l] && T[l][key];
            if (v) demo.add(v);
        }
        if (!demo.has(c.label)) return c;
        const next = t[key];
        return next ? { ...c, label: next } : c;
    });
}

/** tour demo 的 stereo 轨号(source_channels=2)。只喂 ST 角标 / 张开线 / viz stereoMask ——
 *  [J83] 起它**不再**决定 participate_in_auto_pan(默认一律 true)。 */
export const DEMO_STEREO_CHANNELS = Object.freeze(
    DEMO_TRACKS.filter((t) => t.sourceChannels === 2).map((t) => t.ch),
);

/** tour demo 的时间线长度(秒)。段表 / 覆盖 / 波形三处共用同一条时间线。
 *  = 5 分钟 ×15 轨(J59;05 §6.3 验收行「预览 mock 注入 5 分钟 ×15 轨假数据」逐字)。 */
export const DEMO_DURATION_S = 300;

/** tour demo 的在线组位图(设计稿 1261 行 GROUPS_ONLINE = A/B/E → bit0|bit1|bit4)。 */
export const DEMO_GROUPS_ONLINE = 0b00010011;

/** 快照默认的 session GUID(固定字面量:确定性优先于「像真的」)。 */
const DEFAULT_SESSION_GUID = "5c0b7d2e-3a41-4f88-9b6a-1d2e3f405162";

/** 插件版本号(与 CMakeLists.txt 的 project(SCVB VERSION 0.1.0) 对齐)。 */
const PLUGIN_VERSION = "0.1.0";

/** 段布局 abi(契约 §1.1:version.abi = ipc 段布局 abi,`RegistryHeader.abi` 同源)。 */
const LOCAL_ABI = 1;

// -----------------------------------------------------------------------------
// 4. 时间线模型:乐句(phrase)与采集覆盖(coverage)
// -----------------------------------------------------------------------------
//
// 段表、波形 VAD 位、能量谷、覆盖条四处必须讲同一个故事,
// 所以它们全部从下面这两个函数派生,而不是各造各的。

/**
 * 某轨的乐句布局(唱段),确定性、与调用顺序无关。
 * @returns {{t0S:number, t1S:number}[]} 升序、互不重叠。
 */
function phrasesOf(ch, durationS = DEMO_DURATION_S) {
    const out = [];
    let t = 2 + unit(0x9101, ch) * 3;
    for (let k = 0; k < 128 && t < durationS - 1.5; k++) {
        const len = 3.2 + unit(0x9102, ch * 131 + k) * 4.4; // 3.2..7.6s
        const t1 = Math.min(t + len, durationS);
        if (t1 - t >= 1.2) out.push({ t0S: round(t, 2), t1S: round(t1, 2) });
        t = t1 + 0.8 + unit(0x9103, ch * 137 + k) * 2.2; // 0.8..3.0s 间隙
    }
    return out;
}

/**
 * 某轨的采集覆盖区间(两轮采集:passId 1 / 2,中间留一段没采到的空当)。
 * 契约 §2.7 的 `coverage_ranges` 真身在 C++ 侧;这里造的是同形状的预览值。
 */
export function coverageRangesOf(ch, durationS = DEMO_DURATION_S) {
    const u = unit(0x9201, ch);
    const v = unit(0x9202, ch);
    return [
        {
            startS: round(1 + u * 2, 2),
            endS: round(durationS * 0.42 + u * 9, 2),
        },
        {
            startS: round(durationS * 0.5 + v * 6, 2),
            endS: round(durationS - 3 - v * 5, 2),
        },
    ];
}

/** 波形 stale 演示轨(T33:琥珀斜条纹 + ⚠ 的稳定验收面;三轨错开取样)。 */
export const STALE_DEMO_CHANNELS = Object.freeze([2, 7, 12]);

/**
 * [J75] T43 轨迹图「断线」的**确定性**验收缺口(秒)。
 *
 * `coverageRangesOf` 本来就在两轮采集之间留了一段空当(≈126–150s),乐句之间也
 * 处处是短间隙 —— 断线在基线 fixture 上本就画得出来。但那些缺口的位置随轨号抖动,
 * 做不了「断在这儿」的定点断言,也不好截图。故另立一个**跨轨对齐、宽到一眼可见**
 * 的窗口:落在第二轮采集的正中,几条轨在同一处齐刷刷断开。
 *
 * 只有 `makeSegments(..., {trajectoryGap:true})` 才会挖它 —— 基线 fixture 的段表
 * **逐字节不变**(既有 T31/T33 冒烟的数字全部照旧)。
 */
export const TRAJECTORY_GAP = Object.freeze({ startS: 168, endS: 204 });

/** 上述缺口作用的轨(挑 5 条错开:两条 mono 主唱族 + 一条 stereo + 两条陪衬)。 */
export const TRAJECTORY_GAP_CHANNELS = Object.freeze([1, 4, 8, 10, 14]);

/**
 * 某轨的 stale 区间(fingerprint watchdog 语义预留,05 §2.3「特征波形」行)。
 * 只有 STALE_DEMO_CHANNELS 三轨各有一段,落在**第二轮采集**(passId 2)内,
 * 确定性、与调用顺序无关;其余轨回空(健康数据)。
 * @returns {{startS:number, endS:number}[]}
 */
export function staleRangesOf(ch, durationS = DEMO_DURATION_S) {
    if (!STALE_DEMO_CHANNELS.includes(ch)) return [];
    const u = unit(0x9301, ch);
    const startS = round(durationS * (0.56 + u * 0.05), 2);
    return [{ startS, endS: round(startS + durationS * 0.08, 2) }];
}

/**
 * 覆盖率 0..100(契约 §2.7 `coveragePct`:该轨在时间线内已覆盖的百分比)。
 *
 * `upToS` = 播放头位置:只累计 `startS < upToS` 的部分,最后一段按 `upToS` 截断 ——
 * 于是「播放头走到哪、覆盖率长到哪」,与泳道底部 2px 覆盖条讲同一个故事(本文件第 4 节自立纪律)。
 * 缺省 `Infinity` = 全量覆盖率(首帧/快照口径,行为与本参数引入前逐字一致)。
 */
function coveragePctOf(ch, durationS = DEMO_DURATION_S, upToS = Infinity) {
    const covered = coverageRangesOf(ch, durationS).reduce(
        (sum, r) => sum + Math.max(0, Math.min(r.endS, upToS) - r.startS),
        0,
    );
    return round(clamp((covered / durationS) * 100, 0, 100), 1);
}

/**
 * 区间列表求交:`[aStartS, aEndS)` ∩ `ranges`,返回同形的 `{startS, endS}[]`(升序、可为空)。
 * 用于把「本帧新增覆盖」限制在该轨真的采到的区间内(两轮采集之间的空当不该长出覆盖)。
 */
function intersectRange(ranges, aStartS, aEndS) {
    const out = [];
    for (const r of ranges) {
        const s = Math.max(r.startS, aStartS);
        const e = Math.min(r.endS, aEndS);
        if (e > s) out.push({ startS: round(s, 3), endS: round(e, 3) });
    }
    return out;
}

/** t 是否落在某个覆盖区间(startS/endS)内;返回命中下标(0 基),未命中 -1。 */
function indexOfRange(ranges, t) {
    for (let i = 0; i < ranges.length; i++) {
        if (t >= ranges[i].startS && t < ranges[i].endS) return i;
    }
    return -1;
}

/** t 是否落在某个乐句(t0S/t1S)内 —— 乐句与覆盖区间是两套键名,别混用。 */
function isVoicedAt(phrases, t) {
    for (let i = 0; i < phrases.length; i++) {
        if (t >= phrases[i].t0S && t < phrases[i].t1S) return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// 5. Output 侧 —— state / 快照(契约 §2.1 / §1.1)
// -----------------------------------------------------------------------------

/** 单条通道配置(契约 §1.1/§2.1 的 channels[15] 元素)。 */
function makeChannelConfig(patch = {}) {
    const sourceChannels = patch.source_channels === 2 ? 2 : 1;
    return mergeDeep(
        {
            enabled: true,
            label: "",
            source_channels: sourceChannels,
            // [J83] 未显式设置一律 true(契约 §1.15 语义行)。旧口径 `sourceChannels === 1`
            // 按源声道推导,而 source_channels 是**轨道总线布局**不是素材声道数 —— 已废。
            participate_in_auto_pan: true,
            priority: 5,
            lead_lock: false,
            lead_vol_exempt: false,
            pair_id: 0,
        },
        patch,
    );
}

/** 空 pan 曲线(契约 §1.1:versions[].pan_curve.points[],整表提交、≤16 点)。 */
function emptyPanCurve() {
    return { points: [] };
}

/**
 * §2.1 `scvb.state` 载荷(`full:true` 全量)。
 * §1.1 快照的 state 子树 **复用本函数** —— 契约 §1.1 语义行要求「两者不得各自漂移」,
 * 这里用「同一个构造函数」把这条纪律做成结构保证,而不是靠人肉对齐。
 */
export function makeOutputState(overrides = {}) {
    const state = {
        full: true,
        config_seq: 1,
        group_id: 1,
        global: {
            capture_enabled: false,
            output_enabled: false,
            version_active: 1,
            // §5.3:follow 为默认档,start_s/end_s 被忽略且**无哨兵约定**(v0 的 start=end=0 哨兵已废除)
            range: { mode: "follow", start_s: 0, end_s: 0 },
        },
        analysis: {
            // 默认值口径(T33 Wave 2 统一):05 §2.3 滑杆值域 + 设计稿默认
            // (threshold −60..−10 dB、sensitivity 0..1;J23 的 pre/post = 120/200)。
            // 02 §2.1 旧默认(threshold 30 正值 / sensitivity 50)与 05 的 UI 值域
            // 相悖,mock 是 UI 消费面 —— 取 05,登记 deviations 供 02 复核。
            vad: {
                threshold_db: -38,
                hysteresis_db: 6,
                hangover_ms: 180,
                padding_pre_ms: 120,
                padding_post_ms: 200,
            },
            segmentation: {
                mode: "valley",
                // [SL-251 同批] 单位照 **02-dsp-spec §0.3 常量表**(sensitivity 0..100、
                // min_segment_ms 50..500),与 native 上线的那一份逐字同刻度。
                // ⚠ 这里留旧的 0.62/420 会让 preview 一开窗就把灵敏度显示成接近 0、
                // 把手贴左端 —— 与本卡要修的「刚开窗那一小段咬人」是同一个形状的新缺陷,
                // 只是搬到了 preview(CLAUDE.md §10:mock 桥与真桥必须同契约,两侧同改)。
                sensitivity: 62,
                min_segment_ms: 420,
            },
            transition_ramp_ms: 80,
            loudness_mode: "kw_integrated", // §1.21 默认档
            center_slot_policy: "priority_queue", // §1.21 默认档
        },
        channels: allChannels().map(() => makeChannelConfig()),
        versions: Array.from({ length: VERSION_COUNT }, (_, i) => ({
            name: `V${i + 1}`,
            empty: true,
            pan_curve: emptyPanCurve(),
        })),
        features: { embedded: true, bytes: 0 },
        ui: {
            scale: 1,
            language: "zh",
            active_tab: "master",
            guide_seen: false,
            tour_seen: false,
            // 用户显式选过语言(§1.30 setLang 被调用过);首启语言卡的抑制位。
            lang_chosen: false,
            // [J75] T43:Tab1 分布图视图态。新字段、默认 distribution ——
            // 旧工程读进来没有这一键,UI 侧 chartModeOf() 一律回默认档。
            master_chart_mode: CHART_MODES[0],
        },
        // 运行时态三件(不入 state chunk、不随工程持久化,§2.1 字段纪律)
        print_guard: { pending: false },
        recapture: {
            armed: false,
            tracksMask: 0,
            startS: 0,
            endS: 0,
            autoStop: false,
        },
        // progress 为可选字段:不在跑就不发(契约 `progress?`,不发哨兵)
        analysis_run: { running: false },
    };
    return mergeDeep(state, overrides);
}

/**
 * §1.1 `requestInitialState()` 的 Output 全量快照。
 * = §2.1 state 全字段 + 快照专属的 session_guid / guide_seen_global / tour_seen_global /
 *   version / conn(契约 §1.1 语义行);**没有** `full` 键(那是事件字段),**没有**顶层 abi。
 */
export function makeOutputSnapshot(overrides = {}) {
    const state = makeOutputState();
    delete state.full; // 快照不带事件专用的 full 标志

    const snapshot = {
        session_guid: DEFAULT_SESSION_GUID,
        group_id: state.group_id,
        config_seq: state.config_seq,
        global: state.global,
        analysis: state.analysis,
        channels: state.channels,
        versions: state.versions,
        features: state.features,
        ui: state.ui,
        // J50a:系统级全局默认判定位,只读、不属工程 state
        guide_seen_global: false,
        tour_seen_global: false,
        lang_chosen_global: false,
        print_guard: state.print_guard,
        recapture: state.recapture,
        analysis_run: state.analysis_run,
        // 本机 abi 的唯一落点(§1.1:无顶层 abi 键)
        version: { plugin: PLUGIN_VERSION, abi: LOCAL_ABI },
        conn: makeConn(),
    };
    return mergeDeep(snapshot, overrides);
}

// -----------------------------------------------------------------------------
// 6. Output 侧 —— 事件生成器(契约 §2.2-§2.9)
// -----------------------------------------------------------------------------

/**
 * §2.2 `scvb.params`。
 * 覆盖面 = 全局三件 + 当前激活版本 60 个(15 轨 × pan/vol/width/freeze)= 63 个 id。
 * 轨 pan/vol 由引擎打印头驱动(UI 不直写,§1.12-§1.14「不在本通道」行),但**读**得到,
 * 所以 full 帧里它们必须在。
 * 每轨取值来自 DEMO_TRACKS 画像(与 makeMeters 同一份画像,免得电平和推子对不上号);
 * 要「出厂默认全 100/0」的一帧,用 overrides.values 整体替换。
 */
export function makeParams(overrides = {}) {
    const versionActive = overrides.versionActive ?? 1;
    assertInt("versionActive", versionActive, 1, VERSION_COUNT);

    const values = {
        width: 100, // 0..150 %,默认 100
        ms_balance: 0, // -100..+100,默认 0
        lead_select: 0, // int 0..15,默认 0
    };
    for (const t of DEMO_TRACKS) {
        const p = `v${versionActive}_t${tt(t.ch)}_`;
        values[`${p}pan`] = t.pan;
        values[`${p}vol`] = t.volDb;
        values[`${p}width`] = t.width;
        values[`${p}freeze`] = t.freeze;
    }

    return mergeDeep(
        {
            values,
            hostEcho: false, // true = 宿主回吐/引擎打印,UI 灰显且绝不回写(§0.5)
            full: true,
            versionActive,
        },
        overrides,
    );
}

/**
 * §2.3 Output `scvb.conn`。
 * 默认 = 15 轨全空(slotState 0 + heartbeatAgeMs 哨兵),即「刚插入、还没有 Input 连上来」。
 * 健康满配的那一份见 FIFTEEN_TRACKS.snapshot.conn。
 */
export function makeConn(overrides = {}) {
    return mergeDeep(
        {
            channels: allChannels().map(() => ({
                slotState: 0, // 0=空闲 1=已声明 2=活跃(ipc §1 逐字)
                heartbeatAgeMs: HEARTBEAT_AGE_NONE,
                heartbeatFresh: false,
                capturing: false,
                misalignCount: 0,
                srMismatch: false,
            })),
            outputReadOnly: false,
            generation: 1,
        },
        overrides,
    );
}

/**
 * §2.4 / §4.4 `scvb.groups`(Input/Output 同名同载荷)。
 * @param {number} bitmap u8 位图,bit0=组 A … bit7=组 H。
 */
export function makeGroups(bitmap = 0b00000001) {
    assertInt("groups_online", bitmap, 0, 0xff);
    return { groups_online: bitmap };
}

/**
 * §2.5 `scvb.meters`。每轨后置 gain 电平 + 峰值;地板 -60 dB。
 * @param {number} tS 时间(秒)—— 同一个 t 永远给同一组读数。
 */
export function makeMeters(tS = 0, overrides = {}) {
    const tracks = DEMO_TRACKS.map((t) => {
        // 三层噪声叠出「有起伏但不抖」的电平;全确定性
        const swell = valueNoise(0x7001 + t.ch, tS * 1.7) * 8 - 4;
        const jitter = valueNoise(0x7002 + t.ch, tS * 6.5) * 3 - 1.5;
        const db = clamp(t.meterBaseDb + swell + jitter, METER_FLOOR_DB, 0);
        const peakDb = clamp(
            db + 2.2 + valueNoise(0x7003 + t.ch, tS * 0.9) * 2.6,
            METER_FLOOR_DB,
            0,
        );
        return { db: round(db, 1), peakDb: round(peakDb, 1) };
    });

    // 总线 = 两声道各自求和口径的近似(mock 不做真加法,只给形状与量级)
    const busL = clamp(
        -9 + valueNoise(0x7101, tS * 1.3) * 5,
        METER_FLOOR_DB,
        0,
    );
    const busR = clamp(
        -9 + valueNoise(0x7102, tS * 1.3) * 5,
        METER_FLOOR_DB,
        0,
    );

    return mergeDeep(
        {
            tracks,
            bus: {
                l: {
                    db: round(busL, 1),
                    peakDb: round(clamp(busL + 2.4, METER_FLOOR_DB, 0), 1),
                },
                r: {
                    db: round(busR, 1),
                    peakDb: round(clamp(busR + 2.4, METER_FLOOR_DB, 0), 1),
                },
            },
        },
        overrides,
    );
}

/**
 * §2.6 `scvb.playhead`。
 * `loopStartS`/`loopEndS` 默认**不出现**(契约:缺失即字段不存在,不发哨兵);
 * 需要 daw_loop 场景时由 overrides 补进来。
 */
export function makePlayhead(tS = 0, overrides = {}) {
    return mergeDeep(
        {
            timeS: round(tS, 3),
            isPlaying: true,
            // range.mode="follow" 时 inRange 恒 true(§5.3;follow 无界)
            inRange: true,
        },
        overrides,
    );
}

/**
 * §2.7 `scvb.captureProgress`(播放中 2Hz,只含本帧有变化的轨)。
 *
 * 两个字段**都从同一份 coverage 模型派生**(本文件第 4 节自立纪律:段表、波形 VAD 位、
 * 能量谷、覆盖条四处必须讲同一个故事):
 *   • `addedRanges` = 本帧窗口 `[tS-0.5, tS)` ∩ `coverageRangesOf(ch)` —— 播放头走在
 *     两轮采集之间的空当里时交集为空,该轨**本帧不进 `channels`**,顺带把契约 §2.7
 *     「仅包含本帧有变化的轨」从口头约定变成结构保证;
 *   • `coveragePct` = 截断到 `tS` 的覆盖率,随播放头单调增长。
 * 旧实现两者都与 coverage 模型脱钩(窗口直接给 `[tS-0.5, tS]`、覆盖率恒等于全量值),
 * 结果是覆盖条会在波形显示「未采集」的空当里继续延伸、而轨头覆盖率数字一动不动。
 *
 * @param {number} tS 当前播放位置(秒)。
 * @param {number[]} channels 候选轨(默认全 15 轨);本帧无新增覆盖的轨会被过滤掉,
 *   故返回的 `channels` 可能短于入参、甚至为空数组(此时 T28 不必推这一帧)。
 */
export function makeCaptureProgress(tS = 0, channels = allChannels()) {
    const startS = Math.max(0, tS - 0.5);
    const endS = Math.max(startS, tS);
    const list = [];
    for (const ch of channels) {
        assertInt("ch", ch, 1, CHANNEL_COUNT);
        const addedRanges = intersectRange(coverageRangesOf(ch), startS, endS);
        if (addedRanges.length === 0) continue;
        list.push({
            ch,
            addedRanges,
            coveragePct: coveragePctOf(ch, DEMO_DURATION_S, tS),
        });
    }
    return { channels: list };
}

/**
 * §2.8 `scvb.segments`。
 * @param {number} version 1..2
 * @param {string} reason §2.8 十值枚举之一
 * @param {number[]} channels 受影响轨(`snapshot`/`versionActive` 语义上必须是全部轨)
 * @param {{trajectoryGap?: boolean, diffFillToCap?: boolean}} [opts]
 *   `trajectoryGap` = 在 `TRAJECTORY_GAP` 窗口内把 `TRAJECTORY_GAP_CHANNELS` 几条轨的段
 *   整段挖掉([J75] T43 轨迹图断线的定点验收面)。
 *   `diffFillToCap` = 拿掉 `diff.changed` 的 `% 17` 抽稀,让它顶到封顶 200 条
 *   ([SL-274] `?scenario=diff-flood`:页面那条「顶到封顶就印 N+」的分支只有满档才可达)。
 *   两者**缺省都是 false** —— 不传就与各自引入前逐字节相同。
 */
export function makeSegments(
    version = 1,
    reason = "snapshot",
    channels = allChannels(),
    opts = {},
) {
    assertInt("version", version, 1, VERSION_COUNT);
    assertEnum("reason", reason, ENUMS.segmentsReason);
    if (!Array.isArray(channels) || channels.length === 0) {
        throw new RangeError("mock-data: channels 须为非空的通道号数组");
    }

    let total = 0;
    let manualKept = 0;
    const changed = [];

    const list = channels.map((ch) => {
        assertInt("ch", ch, 1, CHANNEL_COUNT);
        const track = DEMO_TRACKS[ch - 1];
        const ranges = coverageRangesOf(ch);
        // 只有采到的地方才会有分析产物:乐句 ∩ 覆盖
        let phrases = phrasesOf(ch).filter(
            (p) => indexOfRange(ranges, (p.t0S + p.t1S) / 2) >= 0,
        );
        // [J75] T43:定点缺口 —— 与窗口有**任何**重叠的段整段拿掉,窗口两侧才是干净的断口
        // (只切掉重叠部分会留下半截段,断线位置就跟着乐句边界抖,断言不住)。
        if (opts.trajectoryGap && TRAJECTORY_GAP_CHANNELS.includes(ch)) {
            phrases = phrases.filter(
                (p) =>
                    p.t1S <= TRAJECTORY_GAP.startS ||
                    p.t0S >= TRAJECTORY_GAP.endS,
            );
        }

        const segments = phrases.map((p, i) => {
            const panJitter = unit(0x8101 + version, ch * 149 + i) * 12 - 6;
            const volJitter = unit(0x8102 + version, ch * 151 + i) * 3 - 1.5;
            // 每 23 段左右出一段用户编辑段(编辑后按 J34/J44 必定 locked)。
            // T33 Wave 1 亲验:11 取模下 fifteen-tracks 满屏琥珀「锁定」chip,
            // 视觉噪音过大 —— 密度减半(角标尺寸维持图例帧规格),登记 deviations。
            const mark = (ch * 7 + i * 13) % 23;
            const origin =
                mark === 0
                    ? "user_edited"
                    : mark === 11 && i > 0
                      ? "user_created"
                      : "auto";
            const seg = {
                segIdx: i, // 0 基,每次事件后重新编号(§2.8 字段纪律)
                t0S: p.t0S,
                t1S: p.t1S,
                pan: round(clamp(track.pan + panJitter, -100, 100), 1),
                volDb: round(clamp(track.volDb + volJitter, -24, 12), 1),
                origin,
                locked: origin !== "auto",
                loudnessLufs: round(-12 - unit(0x8103, ch * 157 + i) * 14, 1),
            };
            if (seg.origin !== "auto") manualKept++;
            // diff.changed 只登记「本次真的改了值」的 auto 段。
            //
            // [SL-274] 封顶从 8 抬到 **200** —— 与 native 的
            // `src/core/output/SegmentDiff.h::kMaxChangedItems`、web 侧的
            // `tab-wave.js::DIFF_CHANGED_CAP` **三处同值**,改一处要三处一起改。
            // 改前的 8 是个「展示档」—— 而正因为它,**冒烟永远看不到用户看到的东西**:
            // 一次全量重分段在真机上给几十上百条,把 `.wave-toolbar` 撑到把泳道窗挤没
            // (用户 v5.6.5 实测「泳道完全消失」),mock 只给 8 条时页面看着一切正常。
            // 这是本仓「mock 盖住真机」判例的又一例(见 AnalysisPipeline 的 vadPosterior
            // 头注、mergeReanalyzed 的 SL-242 头注)。要让页面级冒烟能真的守住这条,
            // mock 必须给得出 native 给得出的量。
            //
            // [SL-274] **与 native 的 changed 判据同口径**:native 自
            // `changedAtDisplayPrecision` 起,只登记「量化到 1 位小数后不同**且**幅度过
            // 半个显示步长」的段;mock 不许发出 native 发不出的那种「看不见的改动」,
            // 否则页面在 mock 下会显示「pan 4.2→4.2 · vol −8.0→−8.0」这种空条目。
            // **这里没有加过滤代码**:实测两条路径(默认档 232 条 / 满档 1600 条)里,
            // 逐条 `max(|Δpan|, |ΔvolDb|)` 的最小值分别是 0.2 / 0.1(都 ≥ 一整档),
            // 一条都滤不掉 —— 加了就是永不触发的死判据
            // (删掉它没有任何用例会红)。真正决定这件事的是 `panJitter`/`volJitter` 的量级,
            // 所以约束落在 **smoke-mock.mjs 的断言**上:抖动哪天被调小到产生亚显示精度的
            // 改动,那条会立刻红、逼人当场处理,而不是被一段静默过滤盖过去。
            //
            // [SL-274] `opts.diffFillToCap`(**默认关**,只有 `?scenario=diff-flood` 的预览
            // 会开)拿掉 `% 17` 那道抽稀,让 changed 真的**顶到封顶**。为什么需要它:
            // 常态素材出 29 条,`tab-wave.js` 里「顶到封顶就把计数渲染成 `N+`」那个分支
            // 就**一条用例都到不了** —— 那是本卡新增的、用户可见的分支,没有删之即红的通路
            // 等于没守(#179 复审【重要】)。505 条 auto 段里抽满 200 条绰绰有余,
            // 所以开关一开就必然撞封顶,页面级冒烟据此断「印的是 200+ 而不是 200」。
            if (
                origin === "auto" &&
                (opts.diffFillToCap || (ch * 3 + i) % 17 === 0) &&
                changed.length < DIFF_CHANGED_CAP
            ) {
                changed.push({
                    ch,
                    segIdx: i,
                    panFrom: round(
                        clamp(seg.pan - panJitter * 0.8, -100, 100),
                        1,
                    ),
                    panTo: seg.pan,
                    volDbFrom: round(
                        clamp(seg.volDb - volJitter * 0.7, -24, 12),
                        1,
                    ),
                    volDbTo: seg.volDb,
                });
            }
            return seg;
        });

        total += segments.length;
        return { ch, segments, stale: false };
    });

    // 首帧快照是「全量给到」,不是「改了多少」;其余 reason 才有增删改摘要
    const isFullDump = reason === "snapshot" || reason === "versionActive";
    const diff = isFullDump
        ? { kept: manualKept, changed: [], added: total, removed: 0 }
        : {
              kept: manualKept,
              changed,
              added: hash32(0x8201, total) % 4,
              removed: hash32(0x8202, total) % 3,
          };

    return { version, reason, channels: list, diff };
}

/**
 * §2.9 / §4.5 `scvb.error`(两侧同形状)。
 * @param {string} code §5.1 九码之一
 * @param {object} extra 可覆盖 `ch` / `detail` / `active`
 */
export function makeError(code, extra = {}) {
    assertEnum("code", code, ENUMS.errorCode);

    // detail 逐码照 §5.1 表;`ch` 只在轨级错误出现(srMismatch/channelConflict/lowSample)
    const byCode = {
        srMismatch: { ch: 1, detail: { inputSr: 44100, outputSr: 48000 } },
        secondOutput: { detail: { groupId: 1 } },
        channelConflict: { ch: 1, detail: { groupId: 1 } },
        newerState: {
            detail: { localAbi: LOCAL_ABI, projectAbi: LOCAL_ABI + 1 },
        },
        sidecarMissing: { detail: { path: "SCVB/demo-session.scvbfeat" } },
        noTimeline: { detail: {} },
        projectCopy: { detail: { sessionGuid: DEFAULT_SESSION_GUID } },
        sidecarSwitched: { detail: { bytes: 8912896 } },
        lowSample: { ch: 13, detail: { voicedS: 0.9 } },
    };

    // `active` 缺省视为 true,故默认不发这个键(§2.9 字段纪律)
    return mergeDeep({ code, ...byCode[code] }, extra);
}

// -----------------------------------------------------------------------------
// 7. Input 侧 —— state / conn / config / 快照(契约 §4.1-§4.3 / §3.1)
// -----------------------------------------------------------------------------

/** §4.1 Input `scvb.state`。`abi_remote` 默认不出现(探测不到时字段不存在)。 */
export function makeInputState(overrides = {}) {
    return mergeDeep(
        {
            channel_id: 0, // 0 = 未分配(J01,首次插入默认值;引导态非错误)
            group_id: 1,
            claim: "unassigned", // §5.2 六态
            abi: LOCAL_ABI,
            // guide_seen = [J80] T48 的 Input 首启轻量引导已读位(契约变更文档
            // docs/contract-changes/20260825-input-guide-seen.md;native 未落地)。
            // 默认 false = 真实首装值 —— 预览里除 input-first-run 外一律由 state-driver
            // 覆写为 true,免得每个场景都被语言卡挡住(与 Output 侧 first-run 同款处置)。
            ui: { scale: 1, language: "zh", guide_seen: false },
        },
        overrides,
    );
}

/** §4.2 Input `scvb.conn`(全部为本组语义,J66)。 */
export function makeInputConn(overrides = {}) {
    return mergeDeep(
        {
            outputOnline: false,
            maskBit: false,
            capturing: false,
            // true = 直通(按原路径出声,未经平衡);false = 静音转发(已接管),J12/J32
            passthrough: true,
            passthroughPending: false,
            // u16 位图,bit0=ch1 … bit14=ch15,bit15 保留 0;含本实例自己占的位
            occupiedMask: 0,
        },
        overrides,
    );
}

/** §4.3 Input `scvb.config`(本组 ctrl 广播区中本 channel 的只读快照,全部只读)。 */
export function makeInputConfig(overrides = {}) {
    return mergeDeep(
        {
            label: "",
            priority: 5,
            lead_lock: false,
            pair_id: 0,
            freeze: 0, // 0..3,bit0=pan / bit1=vol(J65 只读镜像)
            source_channels: 1,
            participate_in_auto_pan: true,
            config_seq: 0,
            // A-32:本组 15 通道 label 的只读镜像(索引 = ch-1,空串 = 未设)
            channelLabels: Array.from({ length: CHANNEL_COUNT }, () => ""),
        },
        overrides,
    );
}

/**
 * §3.1 Input `requestInitialState()` 全量快照。
 * conn/config 两块**复用** §4.2/§4.3 的构造函数(A-30:快照与事件同拼写、同字段集)。
 */
export function makeInputSnapshot(overrides = {}) {
    const state = makeInputState();
    return mergeDeep(
        {
            channel_id: state.channel_id,
            group_id: state.group_id,
            role: "input",
            conn: makeInputConn(),
            config: makeInputConfig(),
            ui: state.ui,
            // J50a 系统级全局默认判定位,只读、不属工程 state([J80] Input 侧同款镜像)
            guide_seen_global: false,
            version: { plugin: PLUGIN_VERSION, abi: LOCAL_ABI },
        },
        overrides,
    );
}

// -----------------------------------------------------------------------------
// 8. 波形瓦片(契约 §1.27 requestWaveform 返回形状)
// -----------------------------------------------------------------------------

/**
 * §1.27 的返回形状:六个 cols 长数组 + valleys[]。
 * 纪律:
 *   - 未覆盖列 `covered=0` 且 `minDb=maxDb=-160`(哨兵),`vad`/`stale`/`passId` 一律 0;
 *   - `passId` 取该列所属采集轮次(本 mock 里 = 覆盖区间下标 +1),不同轮次 UI 底色微差;
 *   - `stale` 由 `staleRangesOf(ch)` 派生(T33:三轨各一段琥珀斜条纹,其余全 0);
 *   - `valleys` = 升序的能量谷时间点(秒),取乐句之间的间隙中点,供边界拖拽吸附;
 *   - 包络值是**时间的函数**而非列下标的函数 —— 换个 cols/视口再拉同一段,波形长得一样。
 * @param {number} ch 1..15
 * @param {number} startS
 * @param {number} endS 须 > startS
 * @param {number} cols 1..4096
 */
export function makeWaveformTile(ch, startS, endS, cols) {
    assertInt("ch", ch, 1, CHANNEL_COUNT);
    assertInt("cols", cols, 1, 4096);
    if (!(endS > startS)) {
        throw new RangeError(
            `mock-data: requestWaveform 要求 startS < endS,收到 ${startS} / ${endS}`,
        );
    }

    const ranges = coverageRangesOf(ch);
    const phrases = phrasesOf(ch);
    const staleRanges = staleRangesOf(ch);
    const minDb = [];
    const maxDb = [];
    const vad = [];
    const covered = [];
    const stale = [];
    const passId = [];

    const span = endS - startS;
    for (let i = 0; i < cols; i++) {
        const tMid = startS + (span * (i + 0.5)) / cols;
        const rIdx = indexOfRange(ranges, tMid);
        if (rIdx < 0) {
            // 未覆盖列:哨兵 -160,其余位全 0
            minDb.push(WAVEFORM_UNCOVERED_DB);
            maxDb.push(WAVEFORM_UNCOVERED_DB);
            vad.push(0);
            covered.push(0);
            stale.push(0);
            passId.push(0);
            continue;
        }
        const voiced = isVoicedAt(phrases, tMid);
        const n1 = valueNoise(0x6001 + ch, tMid * 9);
        const n2 = valueNoise(0x6002 + ch, tMid * 31);
        const hi = voiced ? -7 - n1 * 9 : -44 - n1 * 10;
        const depth = voiced ? 5 + n2 * 9 : 3 + n2 * 6;
        maxDb.push(round(clamp(hi, -80, 0), 1));
        minDb.push(round(clamp(hi - depth, -80, 0), 1));
        vad.push(voiced ? 1 : 0);
        covered.push(1);
        stale.push(indexOfRange(staleRanges, tMid) >= 0 ? 1 : 0);
        passId.push(rIdx + 1);
    }

    const valleys = [];
    for (let k = 0; k + 1 < phrases.length; k++) {
        const t = round((phrases[k].t1S + phrases[k + 1].t0S) / 2, 3);
        if (t >= startS && t <= endS) valleys.push(t);
    }

    return { minDb, maxDb, vad, covered, stale, passId, valleys };
}

// -----------------------------------------------------------------------------
// 9. tour demo —— 健康满配 15 轨(J62 / 05 §2.6)
// -----------------------------------------------------------------------------
//
// 口径:15 轨全连接(slotState=2 ∧ 心跳新鲜)、四条 stereo([J83] 起同样 participate=true)、
//   V1 有非空 pan 曲线与全轨段表、两段采集覆盖、采集开关 ON、输出开关 OFF(FOLLOW)。
//   选 FOLLOW 而不是 PRINT:tour 要逐个高亮控件讲解,PRINT 态会把版本 chip 等大片控件
//   置灰/硬拒绝(§1.9/§5.6),demo 里不该出现「点不动」的东西。

/** demo 的 V1 pan 曲线(≤16 点,§1.17 整表提交语义)。cut 点的 q 承载 slope(dB/oct)。 */
function demoPanCurve() {
    return {
        points: [
            { angle: -72, gain_db: -2.5, shape: "shelf", q: 0.7, side: "left" },
            { angle: -50, gain_db: -12, shape: "cut", q: 12, side: "out" },
            { angle: -24, gain_db: 1.5, shape: "bell", q: 1.2, side: "out" },
            { angle: 0, gain_db: 0, shape: "bell", q: 1, side: "out" },
            { angle: 28, gain_db: 1.8, shape: "bell", q: 1.1, side: "out" },
            { angle: 72, gain_db: -2.2, shape: "shelf", q: 0.7, side: "right" },
        ],
    };
}

/** demo 的 §2.3 conn:15 轨全活跃、心跳新鲜、无失准、无采样率错。 */
function demoConn(capturing = true) {
    return makeConn({
        channels: DEMO_TRACKS.map((t) => ({
            slotState: 2,
            heartbeatAgeMs: 40 + Math.floor(unit(0x5001, t.ch) * 260), // 40..300ms,远小于 2000
            heartbeatFresh: true,
            capturing,
            misalignCount: 0,
            srMismatch: false,
        })),
        outputReadOnly: false,
        generation: 3,
    });
}

/**
 * J62 tour 的 demo 快照(契约 §1.1 全字段)。
 * @param {object} overrides 局部覆盖(深合并;数组整体替换)
 */
export function makeTourDemoSnapshot(overrides = {}) {
    const snapshot = makeOutputSnapshot({
        config_seq: 42,
        global: {
            capture_enabled: true,
            output_enabled: false,
            version_active: 1,
            range: { mode: "manual", start_s: 0, end_s: DEMO_DURATION_S },
        },
        channels: DEMO_TRACKS.map((t) =>
            makeChannelConfig({
                enabled: true,
                label: t.label,
                source_channels: t.sourceChannels,
                // participate_in_auto_pan **有意不给** —— demo 就是要走 makeChannelConfig 的
                // 默认档([J83]:未显式设置一律 true,含 stereo 轨)。
                priority: t.priority,
                lead_lock: t.leadLock,
                lead_vol_exempt: t.leadVolExempt,
                pair_id: t.pairId,
            }),
        ),
        versions: [
            { name: "基础平衡", empty: false, pan_curve: demoPanCurve() },
            // 空版本:05 §2.1 ③ 的空版本 chip 角标要有东西可显示
            { name: "V2", empty: true, pan_curve: emptyPanCurve() },
        ],
        features: { embedded: true, bytes: 3145728 },
        ui: {
            scale: 1,
            language: "zh",
            active_tab: "master",
            guide_seen: true,
            tour_seen: false, // tour 还没走完 —— 这是 demo 的前提
            // tour 的分布图步讲的是柱体/横位,固定停在分布档(J75 A 默认档)
            master_chart_mode: CHART_MODES[0],
        },
        guide_seen_global: true,
        tour_seen_global: false,
        conn: demoConn(true),
    });
    return mergeDeep(snapshot, overrides);
}

/** J62 tour 的 demo 段表(§2.8;首帧口径 = 全部轨全量段表)。 */
export function makeTourDemoSegments(version = 1, reason = "snapshot", opts) {
    return makeSegments(version, reason, allChannels(), opts);
}

/** demo 的 §2.7 首帧覆盖(把两段覆盖当作首帧「新增」一次性给出)。 */
function demoCaptureProgress() {
    return {
        channels: allChannels().map((ch) => ({
            ch,
            addedRanges: coverageRangesOf(ch),
            coveragePct: coveragePctOf(ch),
        })),
    };
}

/**
 * `FIFTEEN_TRACKS` —— tour(J62)的 demo 数据源,健康满配 15 轨。
 * 深冻结:它是全局共享的一份数据,谁都不许就地改;要变形请用 make*() 重造。
 * 结构:{ durationS, labels, stereoChannels, snapshot, segments, params,
 *        meters, playhead, captureProgress, groups }
 */
export const FIFTEEN_TRACKS = deepFreeze({
    durationS: DEMO_DURATION_S,
    labels: DEMO_LABELS.slice(),
    stereoChannels: DEMO_STEREO_CHANNELS.slice(),
    snapshot: makeTourDemoSnapshot(),
    segments: makeTourDemoSegments(1, "snapshot"),
    params: makeParams({ versionActive: 1, full: true, hostEcho: false }),
    meters: makeMeters(42),
    playhead: makePlayhead(42, { isPlaying: true, inRange: true }),
    captureProgress: demoCaptureProgress(),
    groups: makeGroups(DEMO_GROUPS_ONLINE),
});
