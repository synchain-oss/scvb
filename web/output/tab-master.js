// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · Tab1「整体调整」—— 状态机与桥接线(T31 Wave 2 交付物)。
// -----------------------------------------------------------------------------
// 职责边界:
//   • 本文件只管 **Tab1**(三件套 / 四列网格 / 曲线窗 / 空态卡)。外壳(header 版本区、
//     横幅区、footer、缩放、引导页、tab 路由)在 web/output/app.js —— 那是页面级的,
//     不属于某一个 tab。
//   • 两段导出:**纯函数**(无 DOM,node 可直接 import 断言,见
//     web-preview/tests/smoke-tab1-interactions.mjs)+ `createTabMaster()`(DOM 接线)。
//     模块顶层零副作用、零 document 触碰,否则 check-i18n / 冒烟脚本导入即炸。
//
// 消费(契约 §2,逐字):
//   scvb.state    → data-cap / data-analyze / data-out / data-lock、range 三档、
//                   组胶囊 aria-pressed、Lead Select data-selected、print_guard 小锁、
//                   recapture badge、versions[active].pan_curve → 曲线窗折线
//   scvb.params   → width / ms_balance / lead_select 跟随(--v / --ms-left / --ms-w、
//                   ±{θ}° 大读数 θ=round(width×0.6));hostEcho 灰显且绝不回写(§0.5)
//   scvb.groups   → 八胶囊 data-online(事件缺失时绿点全灭、零报错)
//   scvb.playhead → 采集中 / 已离开采集范围 四态判定、PRINT 判定
//   scvb.segments → 分析完成反馈(data-analyze="done" 闪绿)
//   scvb.captureProgress → 覆盖率 {p}%
//
// 上行(契约 §1,逐字):
//   setCaptureEnabled / previewAnalyze(节流)/ analyze / setOutputEnabled /
//   setGroupId(确认条后才调;PRINT 态整组 disabled)/
//   beginParamGesture + setParam + endParamGesture 三段式(双击回默认)/
//   setRange(三档 + manual 起止校验)/ setTransitionRamp
//
// 纪律(与 index.html 头注同源):状态一律改 data-* 属性,不拼 class 字符串;
// 词条一律走 web/shared/i18n.js 的 key,禁止硬写自由文案。
//
// [J75] T43 增补 —— 分布图升级为双视图「分布 ↔ 轨迹」:
//   • 视图态 = state `ui.master_chart_mode`(新字段,变更文档
//     `docs/contract-changes/20260825-master-chart-mode.md`);
//   • 轨迹图本体在 `web/shared/trajectory-chart.js`(可复用件,T46 Monitor 直接复用),
//     本文件只做装配、喂数据、接图例;
//   • 15 色轨道配色的色值真源 = `web/shared/tokens.css` 第 21 组。
// =============================================================================

import {
    createTrajectoryChart,
    panTickText,
    runsOfSegments,
} from "../shared/trajectory-chart.js";
import { trackColorVar } from "../shared/track-colors.js";

// =============================================================================
// 一、纯函数(无 DOM;node 侧断言面)
// =============================================================================

/**
 * 通道数 —— 契约 §0.2 第 4 条:`ch` = 1..15(J59)。
 * 这里落成本地常量而不是从 web/shared/mock-data.js 借 `CHANNEL_COUNT`:
 * mock-data 是**预览用假数据**模块,正式页面不该为一个契约常量把整份 demo 数据拖进包里。
 */
export const CHANNEL_COUNT = 15;

/** 契约 §0.2:g = 1..8,UI 显示 A-H。 */
export const GROUP_IDS = Object.freeze([
    "A",
    "B",
    "C",
    "D",
    "E",
    "F",
    "G",
    "H",
]);

/** 契约 §5.3 的 `range.mode` 三值 ↔ 页面 data-range 三值(CSS 选择器用短名)。 */
export const RANGE_MODE_TO_UI = Object.freeze({
    follow: "follow",
    daw_loop: "loop",
    manual: "manual",
});
export const RANGE_UI_TO_MODE = Object.freeze({
    follow: "follow",
    loop: "daw_loop",
    manual: "manual",
});

/** 全局三件的出厂默认(契约 §1.12-§1.14 可驱动 ParamID 全集行);双击回默认取这里。 */
export const PARAM_DEFAULTS = Object.freeze({
    width: 100,
    ms_balance: 0,
    lead_select: 0,
});
export const PARAM_RANGES = Object.freeze({
    width: { min: 0, max: 150, step: 1 },
    ms_balance: { min: -100, max: 100, step: 1 },
    lead_select: { min: 0, max: CHANNEL_COUNT, step: 1 },
});

/** 契约 §1.20:transition_ramp_ms ∈ [20, 300],默认 80。 */
export const RAMP_MS = Object.freeze({ min: 20, max: 300, def: 80 });

/**
 * hostEcho 灰显的新鲜度窗口(ms)。`scvb.params` 打印期 25Hz(40ms 周期),
 * 600ms = 15 帧余量:批次不断则灰显续命,停发(打印结束且值不再变,§0.4)后
 * 由 app.js 的定时器补一拍 render 退灰 —— hostEcho 标志本身没有「回落帧」。
 */
export const HOST_ECHO_FRESH_MS = 600;

/**
 * 本地乐观值(`local.paramEcho`)的**失效**规则 —— 收到一帧 `scvb.params` 后算新的 echo 表。
 *
 * 为什么必须有这条:乐观值只是「等 25Hz 回推之前先动起来」的过渡显示,一旦引擎对该 id
 * 发了新值,它就必须让位,否则该 ParamID 的显示会被本地值**永久遮蔽** ——
 * 宿主自动化回吐(`hostEcho:true`)与切版本后的全量重发(`full:true`)都再也进不了 UI,
 * 违反契约 §0.5「只更新显示」与 §2.2「切版本后 C++ 全量重发」。
 *
 * 规则两条:
 *   ① `full:true`(切版本的全量批次)⇒ **整表作废**(新版本的参数与旧版本毫无关系),
 *      **唯独正在拖动中的那个 id 保留到松手** —— 指针还按在把手上时被全量广播抢跳,
 *      与增量分支要防的是同一件事(T33:与 Tab2 `dropParamEcho` 的 full 分支对齐,
 *      两 tab 此前口径不一致,登记见 t32/deviations §O);
 *   ② 增量批次 ⇒ 本帧提到的 id 逐个作废,**同样保留拖动中的那个 id**
 *      (松手前让位会与自己的回声打架,产生把手回跳)。
 *
 * 空载荷(`null`)不是「一批参数」,按整表作废处理(拖动中的 id 也不例外)。
 *
 * @param {object} echo 现有乐观值表(本函数不改入参)
 * @param {object} payload `scvb.params` 载荷 `{values, hostEcho, full, versionActive}`
 * @param {string|null} gestureId 正在拖动中的 ParamID(无拖动传 null)
 */
export function nextParamEcho(echo, payload, gestureId) {
    if (!payload) return {};
    const src = echo || {};
    if (payload.full) {
        const held =
            gestureId != null &&
            Object.prototype.hasOwnProperty.call(src, gestureId);
        return held ? { [gestureId]: src[gestureId] } : {};
    }
    const out = { ...src };
    for (const id of Object.keys(payload.values || {})) {
        if (id !== gestureId) delete out[id];
    }
    return out;
}

/**
 * `scvb.segments` **版本闸**(契约 §2.8 载荷首字段 `version:1..2`)。
 *
 * 事件仓只存**当前激活版本**的段表:`store.segments` 是 Tab1 计数 / Tab2 手动常值 /
 * Tab3 泳道与检查器的共同渲染源,而这三处显示的都是 `global.version_active` 那一版。
 * 唯一会送来别版本段表的面是 `copyVersion(src, dst)` —— 契约 §1.11 只说「完成后经
 * `scvb.segments`(`reason:"copyVersion"`)回推段表」,**没有**规定 dst 不是激活版本时
 * 发不发;web-preview 的 mock 照发(载荷 `version = dst`,只是自己内部不换表)。
 * 不闸的话「复制到非激活版本」会把 `store.segments` 换成一份看不见的版本的表,
 * Tab3 段数/边界与版本 chip 当场不一致(PR#64 评审【重要】6)。
 *
 * ⚠ **C++ 侧对非激活 dst 到底发不发这个事件,待 native 侧确认**。确认为「不发」后
 *   本闸退化成恒真的防御层,不必拆 —— 它同时挡住任何别的版本串台。
 *
 * `reason:"versionActive"` 例外:该事件自身就是「激活版本刚换成 `version`」的通告,
 * 而契约没有规定它与携新 `version_active` 的 `scvb.state` 谁先到 —— 按版本闸判会把
 * 新版本的全量段表整帧误丢。`version` 或 `activeVersion` 任一不是整数时同样放行
 * (首帧 snapshot 早于 state 落地、老载荷不带 `version` 都归这一支)。
 *
 * @param {object|null} seg `scvb.segments` 载荷
 * @param {number|undefined} activeVersion `state.global.version_active`
 */
export function segmentsEventApplies(seg, activeVersion) {
    if (!seg) return false;
    if (seg.reason === "versionActive") return true;
    if (!Number.isInteger(seg.version) || !Number.isInteger(activeVersion)) {
        return true;
    }
    return seg.version === activeVersion;
}

/**
 * §2.8:`channels` 只含受影响轨(snapshot / versionActive / copyVersion 时为全部轨),
 * 段表是该轨的**完整列表**(不做段级增量)—— 所以按 ch 整条替换即可。
 *
 * 命名:本函数此前在 app.js 里用的局部名,与契约 §8.2「禁止复活名单」上那个已废弃的
 * **桥函数**(01 草案的段合并 API,现由 `editSegment(ch,'merge',…)` 取代)逐字撞名。
 * 名单靠整词扫描落实(scripts/check-bridge-parity.mjs 的 FORBIDDEN 表),留着一个同名
 * 局部件迟早让机器扫出假阳性 —— 改叫 `applySegmentsEvent`(PR#64 评审【建议】1)。
 */
export function applySegmentsEvent(prev, next) {
    if (!next) return prev;
    if (
        !prev ||
        next.reason === "snapshot" ||
        next.reason === "versionActive" ||
        next.reason === "copyVersion"
    ) {
        return next;
    }
    const byCh = new Map((prev.channels || []).map((c) => [c.ch, c]));
    for (const c of next.channels || []) byCh.set(c.ch, c);
    return {
        ...next,
        channels: [...byCh.values()].sort((a, b) => a.ch - b.ch),
    };
}

// ---------------------------------------------------------------- 撤销/重做可用性
// [D1] header 常驻「撤销/重做」两钮的置灰判据。放本文件是因为它是**纯 reducer**
// (node 侧可直接断言),接线与 DOM 归 app.js —— header 属外壳,不属 Tab1。
//
// **契约面事实**:§1.25/§1.26 只给 `{ok:bool}` 回执(`false` = 该向栈为空),
// §1.1 首帧快照与 §2.1 `scvb.state` 的字段全集里**都没有** canUndo/canRedo 之类的
// 可用性信号。既然不发明新桥面(零契约变更),可用性只能由两手证据推出:
//   ① **回执**:点下去拿到 `{ok:false}` —— 这是「该向栈空」的第一手、也是唯一权威证据;
//   ② **新事务入栈**:§0.9 左列四类入栈操作里,当前 web 侧真正发得出的三类
//      (`editSegment` / `setTrackManual` / `copyVersion`)都经 §2.8 段表事件带
//      `reason` 回推,见其 reason 即知撤销栈刚长了一条。
//
// **已知缺口(两条,都会让 undo 钮在真实可撤销的操作后误灰)**:
//   • 第四类入栈操作 `setPanCurve` 眼下 web 侧无调用点(曲线窗只读,
//     `grep -n 'call("setPanCurve"'` 零命中)。它**不会**进本 reducer:契约 §1.17 写明
//     「写入后经 `scvb.state` 回推 `versions[active].pan_curve`」,§2.8 的 `reason` 十值里
//     也没有它的份 —— 本 reducer 喂的是段表事件,pan 曲线根本不走那条线。故接线那一卡
//     要补的是**另一路证据**(scvb.state 里 `pan_curve` 变化 ⇒ 撤销栈长了一条),
//     不是把它「同批并进本 reducer」。
//   • `setVersionName` 在契约 §0.9 里列在**不入栈**那一列,但实现是入栈的:
//     `src/output/OutputProcessor.cpp:784-787` 拿 `authority_.undoManager()` 提交了一笔
//     `"Rename V{n}"` 事务(:782 已先做「名字未变则不产生空撤销事务」的短路)。
//     两边对不上,以哪边为准须裁决;在此之前改名后 undo 钮会误灰(§2.8 也不为它发段表事件)。
//     **本卡不改契约、不改 native**,只把出入登记在这里。
//
// 起手为什么两向都**常亮**而不是灰:UndoManager 挂在处理器上(03 §5.3),编辑器
// 关了再开、栈照旧非空 —— 首帧没有任何证据说它是空的,此时置灰会挡住真实可用的
// 动作(真错),而常亮的代价只是白点一下、回执把它置灰(可自愈)。

/** 两向可用性的起手值(见上:无证据 ⇒ 保守常亮)。 */
export const HISTORY_AVAIL_INIT = Object.freeze({ undo: true, redo: true });

/** §2.8 `reason` 十值里**入撤销栈**的那几个(= §0.9 左列 ∩ 段表回推面)。 */
const UNDOABLE_REASONS = new Set(["edit", "trackManual", "copyVersion"]);

/**
 * `undo()` / `redo()` 回执 → 新可用性(证据①)。
 *
 * `ok:true` 表示一条事务从本向挪到了**反向**栈:反向必有货 ⇒ 置亮;本向还剩几条
 * 回执不说,保守留亮(下一次点空了自然拿到 `ok:false`)。
 * `ok:false` = 本向栈空 ⇒ 只灰本向;反向的可用性与本次调用无关,不动。
 *
 * @param {{undo:boolean,redo:boolean}|null|undefined} prev
 * @param {"undo"|"redo"} kind
 * @param {boolean} ok 回执的 `ok` 字段
 */
export function historyAfterCall(prev, kind, ok) {
    const cur = prev || HISTORY_AVAIL_INIT;
    if (kind !== "undo" && kind !== "redo") return cur;
    const other = kind === "undo" ? "redo" : "undo";
    return ok ? { ...cur, [other]: true } : { ...cur, [kind]: false };
}

/**
 * `scvb.segments` → 新可用性(证据②)。
 *
 * 新事务入栈会**清空 redo 栈**(juce::UndoManager 语义,03 §5.3),故 undo 置亮、
 * redo 置灰。`reason:"undo"`/`"redo"` 是本 reducer 自己动作的回推,必须排除在外
 * (不然一次 undo 会把刚长出来的 redo 当场灭掉);其余 reason(analyze/vad/
 * segmentation/versionActive/snapshot)按 §0.9 右列不入栈,不动两向。
 *
 * @param {{undo:boolean,redo:boolean}|null|undefined} prev
 * @param {object|null} seg `scvb.segments` 载荷
 */
export function historyAfterSegments(prev, seg) {
    const cur = prev || HISTORY_AVAIL_INIT;
    if (!seg || !UNDOABLE_REASONS.has(seg.reason)) return cur;
    return { undo: true, redo: false };
}

/**
 * `scvb.state` 深合并(契约 §2.1 字段纪律:`full:false` = 增量,只含变化子树,UI 做深合并)。
 * 数组整体替换 —— `channels[15]` / `versions[2]` 是定长表,逐元素合并会把「C++ 只发前两轨」
 * 误解成「后 13 轨保持旧值」以外的东西;契约说的是子树替换语义,数组即叶子。
 * @param {object} base 现有 state(会被就地改写的**副本**,本函数不改入参)
 * @param {object} patch 新到的一帧
 */
export function deepMerge(base, patch) {
    if (!isPlainObject(patch)) return patch;
    const out = isPlainObject(base) ? { ...base } : {};
    for (const key of Object.keys(patch)) {
        const v = patch[key];
        out[key] = isPlainObject(v) ? deepMerge(out[key], v) : v;
    }
    return out;
}

function isPlainObject(v) {
    return typeof v === "object" && v !== null && !Array.isArray(v);
}

/** 占位符求值 —— 词条里的 {n}/{x}/{name} 由调用方填,i18n.js 只发字典(不做模板求值)。 */
export function format(text, vals) {
    return String(text).replace(/\{(\w+)\}/g, (m, k) =>
        Object.prototype.hasOwnProperty.call(vals, k) ? String(vals[k]) : m,
    );
}

export function clamp(lo, hi, v) {
    return v < lo ? lo : v > hi ? hi : v;
}

// --------------------------------------------------------- §2.9 轨级 error 的键
/**
 * `store.errors` 的存储键(T33)。契约 §2.9 字段纪律:`ch` 只在轨级错误
 * (`srMismatch` / `channelConflict` / `lowSample`)出现;其中 **`lowSample` 是
 * 会同时命中多轨**的一条(每轨采集后各自判「有效唱段 <1.5s」),按裸 code 存一条时
 * 后到的轨会把先到的覆盖掉 —— Tab2 状态灯旁与 Tab3 轨头只剩最后一轨挂得上黄标。
 * 故 `lowSample` 一律存 `lowSample#{ch}` 复合键。
 *
 * 其余 code **保持既有行为**(裸 code 一条):横幅 ③⑤⑥ 等是页级落点,同一 code 的
 * 第二帧本就该覆盖第一帧;`srMismatch` 的横幅文案也只显示一个轨号(05 §2.0)。
 *
 * @param {object} e `scvb.error` 载荷 `{code, ch?, detail?, active?}`
 */
export function errorStoreKey(e) {
    const code = e && e.code;
    if (code !== "lowSample") return code;
    const ch = Math.trunc(Number(e.ch));
    return Number.isFinite(ch) && ch > 0 ? code + "#" + ch : code;
}

/**
 * `active:false`(条件已解除,契约 §2.9)时**该删哪些键**。
 *
 * 存删对称性:`errorStoreKey` 把 `lowSample` 拆成 `lowSample#{ch}` 复合键,而契约
 * §2.9 只保证「`ch` 仅在轨级错误出现」,**没有**「解除事件必带 `ch`」的正面保证。
 * 若宿主发 `{code:"lowSample", active:false}`(不带 ch),按键直删只会删掉一个不存在
 * 的裸键,`lowSample#{ch}` 永远留在 store 里 —— Tab2 行内黄标与 Tab3 轨头黄标
 * 就此再也熄不掉(对抗校验 minor)。故:**带 ch = 只撤该轨;不带 ch = 撤该 code 的
 * 全部条目**(裸键 + 全部复合键)。其余 code 是页级落点,键即裸 code,行为不变。
 *
 * @param {Map<string, object>|null} errors 当前 `store.errors`
 * @param {object} e `scvb.error` 载荷 `{code, ch?, active:false}`
 * @returns {string[]} 待删键(可能为空)
 */
export function errorKeysToDrop(errors, e) {
    const key = errorStoreKey(e);
    const code = e && e.code;
    if (code !== "lowSample" || key !== code) return [key];
    // 裸键 = 解除事件没带 ch ⇒ 连带撤下该 code 的全部轨级条目
    const out = [key];
    if (errors && typeof errors.forEach === "function") {
        errors.forEach((v, k) => {
            if (String(k).split("#")[0] === code && k !== key) out.push(k);
        });
    }
    return out;
}

/**
 * `store.errors` → 命中 `lowSample` 的轨号集合(Tab2 轨行与 Tab3 轨头**消费同一份**)。
 * 按**值**扫描而非按键取:与 `errorStoreKey` 的键形解耦(裸 `lowSample` 键同样命中),
 * 消费侧不必知道键怎么拼。
 * @param {Map<string, object>|null} errors
 * @returns {Set<number>} 轨号(1..15)集合;无命中时空集
 */
export function lowSampleChannels(errors) {
    const out = new Set();
    if (!errors || typeof errors.forEach !== "function") return out;
    errors.forEach((v, k) => {
        const code = (v && v.code) || String(k).split("#")[0];
        if (code !== "lowSample") return;
        const ch = Math.trunc(Number(v && v.ch));
        if (Number.isFinite(ch) && ch > 0) out.add(ch);
    });
    return out;
}

/** 两位零填充(契约 §1.12 的 `t{t:02d}` 与轨号显示同款)。 */
export function tt(n) {
    return String(n).padStart(2, "0");
}

/** m:ss(已分析区域「合计 {t}」用;05 §2.1 ② follow 档提示行)。 */
export function mmss(totalS) {
    const s = Math.max(0, Math.round(totalS));
    return Math.floor(s / 60) + ":" + tt(s % 60);
}

/** mm:ss.mmm(Range 手动档输入框的桥面单位显示;契约 §1.8 只收秒)。 */
export function secondsToTimecode(sec) {
    const s = Math.max(0, Number(sec) || 0);
    let mm = Math.floor(s / 60);
    let ss = Math.floor(s % 60);
    // 毫秒四舍五入可能进到 1000(如 72.9996)——进位到秒/分,否则输出 "01:12.1000"
    let ms = Math.round((s - Math.floor(s)) * 1000);
    if (ms >= 1000) {
        ms -= 1000;
        ss += 1;
        if (ss >= 60) {
            ss -= 60;
            mm += 1;
        }
    }
    return tt(mm) + ":" + tt(ss) + "." + String(ms).padStart(3, "0");
}

/**
 * 解析 mm:ss.mmm / m:ss / 裸秒数;非法返回 null(调用方据此不发 setRange)。
 */
export function timecodeToSeconds(text) {
    const raw = String(text ?? "").trim();
    if (raw === "") return null;
    const m = /^(?:(\d+):)?(\d{1,2})(?:\.(\d{1,3}))?$/.exec(raw);
    if (m) {
        const mm = m[1] === undefined ? 0 : Number(m[1]);
        const ss = Number(m[2]);
        const ms = m[3] === undefined ? 0 : Number(m[3].padEnd(3, "0"));
        if (ss > 59 && m[1] !== undefined) return null;
        return mm * 60 + ss + ms / 1000;
    }
    const plain = Number(raw);
    return Number.isFinite(plain) && plain >= 0 ? plain : null;
}

/**
 * Width 大读数:θ = round(width% × 0.6),显示 ±{θ}°(设计稿 L2015;05 §2.1 ④)。
 * width 0..150 % ⇒ θ 0..90°,与 master.widthAngleHint 的「0–90°」自洽。
 */
export function widthAngleDeg(widthPct) {
    return Math.round(clamp(0, 150, Number(widthPct) || 0) * 0.6);
}

/** 滑轨填充比(0..100 %),供 --v 使用。 */
export function sliderPct(value, min, max) {
    if (max === min) return 0;
    return (clamp(min, max, Number(value) || 0) - min) * (100 / (max - min));
}

/**
 * MS Balance 的双向填充(设计稿 L2022-2023:填充从中点向两侧扩)。
 * @returns {{left:number, width:number, knob:number}} 三个百分数
 */
export function msFill(value) {
    const pos = 50 + clamp(-100, 100, Number(value) || 0) / 2;
    return {
        left: Math.min(50, pos),
        width: Math.abs(pos - 50),
        knob: pos,
    };
}

/** MS 读数:0 不带号,非 0 带 +/−(U+2212,与页面其他负号同形)。 */
export function msReading(value) {
    const v = Math.round(Number(value) || 0);
    if (v === 0) return "0";
    return (v > 0 ? "+" : "−") + Math.abs(v);
}

/**
 * 过渡斜坡 SVG 的几何(viewBox 0 0 300 20)。
 * 静态稿(设计稿 L433-438)在 ms=80 时给出 x0=112 / x1=146 —— 即 34px 对应 80ms;
 * 本函数照这条比例把斜坡宽度做成 ms 的线性函数,中心固定在两点中位 129。
 * ms=300(上限)时 x0≈65.3 / x1≈192.8,仍在画布内。
 */
export function rampGeometry(ms) {
    const v = clamp(RAMP_MS.min, RAMP_MS.max, Number(ms) || RAMP_MS.def);
    const span = v * (34 / 80);
    const x0 = 129 - span / 2;
    const x1 = 129 + span / 2;
    return {
        x0: round2(x0),
        x1: round2(x1),
        d: `M0 16 L${round2(x0)} 16 L${round2(x1)} 5 L300 5`,
    };
}

function round2(v) {
    return Math.round(v * 100) / 100;
}

/**
 * 采集四态(设计稿 L1908 的标签四态 = 页面 data-cap)。
 * 判据逐字照 04 §5.1 / 契约 §1.2 语义行:「实际写特征段只在播放中且在 range 内发生」。
 *   OFF        capture_enabled = false
 *   armed      ON,未播放(「已布防·等待播放」)
 *   capturing  ON,播放中且 inRange
 *   outside    ON,播放中但已离开 range(follow 档 inRange 恒 true,故永不出现)
 */
export function captureVisual(state, playhead) {
    const on = !!(state && state.global && state.global.capture_enabled);
    if (!on) return "off";
    if (!playhead || !playhead.isPlaying) return "armed";
    return playhead.inRange ? "capturing" : "outside";
}

/**
 * 输出三态(03 §2.2:FOLLOW / ARMED / PRINT)。
 * PRINT = output_enabled ∧ isPlaying ∧ inRange(契约 §2.6 UI 消费行逐字)。
 * **加载守卫未确认时行为止于 ARMED**(契约 §1.3 / §1.34)—— UI 判定同样卡在 armed,
 * 否则版本 chip 会在守卫未确认时就整组 disabled,而引擎其实没在打印。
 */
export function outputPhase(state, playhead) {
    if (!state || !state.global || !state.global.output_enabled)
        return "follow";
    const guarded = !!(state.print_guard && state.print_guard.pending);
    if (guarded) return "armed";
    if (playhead && playhead.isPlaying && playhead.inRange) return "print";
    return "armed";
}

/**
 * 分析影响面(master.step2.desc 的 {n}/{m}/{k})。
 * 首选 `previewAnalyze()` 的返回(契约 §1.5 `{intervals, tracks, manualKept}`);
 * 拿不到时从 `scvb.segments`(契约 §2.8)兜底出同一组数,避免影响预览行留裸大括号。
 */
export function segmentTotals(segments) {
    const chans = (segments && segments.channels) || [];
    return {
        n: chans.reduce((sum, c) => sum + ((c.segments || []).length || 0), 0),
        m: chans.filter((c) => (c.segments || []).length > 0).length,
        k: (segments && segments.diff && segments.diff.kept) || 0,
    };
}

/**
 * 覆盖率 {p}%(05 §2.1 ①「范围内 {p}% 已覆盖」)= 已报到的各轨 `coveragePct` 均值。
 *
 * 分母取**已报到的轨数**而不是恒 15:契约 §2.7 的 `scvb.captureProgress` 是增量事件,
 * 「仅包含本帧有变化的轨」且「非播放不发」—— 拿 15 作分母的话,首帧到齐前这一行会从
 * 0% 一路爬到真值,截图与手测都像坏页。分母取已知轨 = 「在已知的轨上,范围内覆盖了多少」,
 * 首帧即稳定。**一轨都没报到时返回 null**,调用方据此把整行隐掉(不显示假的 0%)。
 */
/**
 * 分析按钮「无数据」判据 —— 覆盖与段表的**并集**判空(两者都无才算真没数据)。
 * 只看段表会鸡生蛋(首采未析永远禁用);只看覆盖会误伤重开工程
 * (§2.7 captureProgress 非播放不发,覆盖帧未到但段表有货)。
 * 此口径两度踩坑(PR #52 首审【重要】+ pr-agent 建议),抽纯函数配 smoke 断言锁死。
 * @param {number|null} coveragePct coveragePercent() 的返回(null=无帧)
 * @param {number} segTotalN 已分析段数(segmentTotals().n)
 */
export function analyzeNoData(coveragePct, segTotalN) {
    return !coveragePct && segTotalN === 0;
}

export function coveragePercent(coverage) {
    const list = Object.values(coverage || {}).filter((v) =>
        Number.isFinite(v),
    );
    if (list.length === 0) return null;
    return Math.round(list.reduce((s, v) => s + v, 0) / list.length);
}

/** 区间并集(升序合并)—— 把 15 轨各自的覆盖并成「已分析区域共 {n} 段 · 合计 {t}」。 */
export function mergeRanges(ranges) {
    const sorted = (ranges || [])
        .filter((r) => r && r.endS > r.startS)
        .slice()
        .sort((a, b) => a.startS - b.startS);
    const out = [];
    for (const r of sorted) {
        const last = out[out.length - 1];
        if (last && r.startS <= last.endS) {
            last.endS = Math.max(last.endS, r.endS);
        } else {
            out.push({ startS: r.startS, endS: r.endS });
        }
    }
    return out;
}

/**
 * follow 档提示行「已分析区域共 {n} 段 · 合计 {t}」(05 §2.1 ②),以及 write 确认条
 * `out.master.writeConfirm.follow` 里同名的两个占位符。
 *
 * 数据源 = `scvb.segments` 的全轨段表(契约 §2.8),**不是** `scvb.captureProgress`:
 * 「已分析区域」按字面就是分析产物覆盖的时间域;而 §2.7 的 `addedRanges` 是**采集**覆盖的
 * **本帧增量**(全量区间表 `features.per_channel[].coverage_ranges[]` 不在桥面下行面上),
 * 拿它累积只能得到「本次会话新采到的那几秒」,首开工程恒为空。
 */
export function analyzedRegions(segments) {
    const spans = ((segments && segments.channels) || []).flatMap((c) =>
        (c.segments || []).map((s) => ({ startS: s.t0S, endS: s.t1S })),
    );
    const regions = mergeRanges(spans);
    return {
        n: regions.length,
        t: mmss(regions.reduce((s, r) => s + (r.endS - r.startS), 0)),
    };
}

/**
 * `tracksMask` = u16 位图,bit0=ch1 … bit14=ch15,bit15 保留为 0(契约 §0.2 第 5 条)。
 * 取 `channels[].enabled` —— `setCaptureEnabled` 的布防面就是 `{enabled 轨} × {range}`(§1.2)。
 */
export function tracksMaskOf(channels) {
    let mask = 0;
    const list = channels || [];
    for (let i = 0; i < Math.min(list.length, CHANNEL_COUNT); i++) {
        if (list[i] && list[i].enabled) mask |= 1 << i;
    }
    return mask & 0x7fff;
}

/**
 * `analyze` / `previewAnalyze` 的 scope(契约 §1.5/§1.6:对象或字符串 "all")。
 * follow 档无界(§5.3「忽略 startS/endS,无哨兵约定」)⇒ 只能用 "all",
 * 绝不能拿 range.start_s/end_s 去凑一个 [0,0] 的空区间。
 */
export function analyzeScope(state) {
    const g = (state && state.global) || {};
    const mask = tracksMaskOf(state && state.channels);
    if (!g.range || g.range.mode === "follow") return "all";
    return { tracksMask: mask, startS: g.range.start_s, endS: g.range.end_s };
}

/** write 确认条的词条 key(follow 档走 .follow 变体,无 {x}–{y} 空洞)。 */
export function writeConfirmKey(rangeMode) {
    return rangeMode === "follow"
        ? "out.master.writeConfirm.follow"
        : "out.master.writeConfirm";
}

/**
 * footer 打印中/打印结束的词条 key(同上,follow 档各有专用变体)。
 * 四个 key 写成**字面量**而不是 `base + ".follow"` —— check-i18n 的死 key 扫描按字面量
 * 认引用,拼出来的 key 会被误报成「字典里有、页面没人用」。
 */
export function footerPrintKey(rangeMode, done) {
    if (rangeMode === "follow") {
        return done ? "footer.printDone.follow" : "footer.printing.follow";
    }
    return done ? "footer.printDone" : "footer.printing";
}

/** 版本名(契约 §1.10:空串/纯空白由 C++ 回落默认 "V{v}";UI 侧同口径兜底)。 */
export function versionNameOf(state, v) {
    const entry = ((state && state.versions) || [])[v - 1];
    return entry && entry.name ? entry.name : "V" + v;
}

/**
 * 首启引导页是否该弹(契约 §1.32 语义行 + J50a)。
 * 判据:①首帧到了(§0.6 门控,没拿到快照前不渲染真实数据态);②本会话没关过;
 * ③工程 `ui.guide_seen === false`;④**且**系统级全局默认 `guide_seen_global` 为 false
 * ——「不再显示」的承诺跨工程成立,新工程 guide_seen=false 时先读全局默认。
 */
export function shouldShowGuide(state, snapshot, closedThisSession) {
    if (!snapshot || closedThisSession) return false;
    const ui = (state && state.ui) || {};
    return ui.guide_seen === false && snapshot.guide_seen_global === false;
}

/** `scvb.groups` 位图 → 某组是否在线(事件缺失时传 0,绿点全灭、零报错)。 */
export function groupOnline(bitmap, g) {
    return ((Number(bitmap) || 0) >> (g - 1)) & 1 ? 1 : 0;
}

/** Header 连接 pill 的 N/15:只数 slotState=2 ∧ heartbeatFresh(契约 §2.3 UI 消费行,J01)。 */
export function connectedCount(conn) {
    return connectedChannels(conn).length;
}

/**
 * 已连接轨号(升序)—— 与 `connectedCount` 同判据的**轨号面**。
 * 分布图画哪些柱、图例列哪些行都取它:空闲轨无参数值,vol=0 会被 `distGeometry`
 * 画成居中高的「幽灵柱」(设计稿绘制前就滤掉 idle/srErr 轨)。
 */
export function connectedChannels(conn) {
    const chans = (conn && conn.channels) || [];
    const out = [];
    for (let i = 0; i < chans.length; i++) {
        const c = chans[i];
        if (c && c.slotState === 2 && c.heartbeatFresh) out.push(i + 1);
    }
    return out;
}

/** 失准轨数(横幅① 的 {m};契约 §2.3 `misalignCount`)。 */
export function misalignedTracks(conn) {
    const chans = (conn && conn.channels) || [];
    return chans.filter((c) => c && (c.misalignCount || 0) > 0).length;
}

/**
 * 角度域曲线的 SVG path(viewBox 0 0 660 214;网格 y=109 为 0 dB、y=35 为 +12 dB)。
 * 契约 §2.1 字段纪律:`versions[].pan_curve` 是 pan 曲线编辑器渲染既有点集的**唯一**下行落点。
 * Wave 2 只做**折线**(统筹增补③:消除「有数据却显示空态」的违和);真插值归 T34。
 */
export function panCurvePath(points) {
    const pts = (points || [])
        .filter((p) => p && Number.isFinite(p.angle))
        .slice()
        .sort((a, b) => a.angle - b.angle);
    if (pts.length === 0) return "M0 109 L660 109";
    const x = (angle) => round2(((clamp(-100, 100, angle) + 100) / 200) * 660);
    const y = (db) => round2(109 - clamp(-12, 12, Number(db) || 0) * (74 / 12));
    const segs = [`M0 ${y(pts[0].gain_db)}`];
    for (const p of pts) segs.push(`L${x(p.angle)} ${y(p.gain_db)}`);
    segs.push(`L660 ${y(pts[pts.length - 1].gain_db)}`);
    return segs.join(" ");
}

/**
 * 分布图一根柱/一条张开线的几何(设计稿 L2037-2056)。
 * 横位 x =(pan+100)/200;柱高 ∝ 音量行程(−24..+12 dB 归一后 /0.70 拉满卡片高);
 * 张开半宽 =(轨 width%/100)×16,并被 x 与 100−x 夹住不出框。
 */
export function distGeometry(pan, volDb, widthPct) {
    const x = ((clamp(-100, 100, pan) + 100) / 200) * 100;
    const h = clamp(8, 88, ((clamp(-24, 12, volDb) + 24) / 36 / 0.7) * 100);
    const half = Math.min((clamp(0, 100, widthPct) / 100) * 16, x, 100 - x);
    return { x: round2(x), h: round2(h), half: round2(half) };
}

// =============================================================================
// 一b、[J75] T43 分布图双视图 —— 纯函数面
// =============================================================================

/**
 * 视图两态(state `ui.master_chart_mode`)。
 * **新 state 字段**,非自动化;定义/默认值/迁移语义/建议桥面见
 * `docs/contract-changes/20260825-master-chart-mode.md`(仓 CLAUDE.md §5 流程)。
 */
export const CHART_MODES = Object.freeze(["distribution", "trajectory"]);

/** 默认档(J75 A 逐字:默认 `distribution`)。 */
export const CHART_MODE_DEFAULT = "distribution";

/** state → 视图态;缺字段 / 未知值一律回默认档(旧工程读进来就是这一路)。 */
export function chartModeOf(state) {
    const v = state && state.ui ? state.ui.master_chart_mode : undefined;
    return CHART_MODES.includes(v) ? v : CHART_MODE_DEFAULT;
}

/**
 * 段表 → 轨迹图数据面(每轨一组折线段)。
 *
 * 数据源是 §2.8 段表本身,不是覆盖率事件:段表里有段的地方就是「有分段覆盖」,
 * 没段的地方就是 J75 A 说的「无分段覆盖的区间」—— 断线判据因此只有一处
 * (`runsOfSegments`),不需要把 `scvb.captureProgress` 的覆盖区间再叠一层。
 * 段的 `pan` 已经是**最终打印值**(auto 段与 user_edited 段同在一张表里)。
 *
 * @param {object|null} segments 事件仓的 `store.segments`(§2.8 合并后)
 * @param {Array} channels `state.channels`(取 `source_channels` 判立体声)
 */
export function trajectorySeries(segments, channels) {
    const out = [];
    for (const c of (segments && segments.channels) || []) {
        // 轨号闸与 legendRows **同一道**(1..CHANNEL_COUNT,契约 §0.2)。
        // 只判 Number.isFinite 的话,脏数据里的 ch=0 / ch=99 会画出线来而图例
        // 把它滤掉 —— 正好打破本卡自己立的「图例里有它、图上就找得到它」。
        if (!c || !Number.isFinite(c.ch) || c.ch < 1 || c.ch > CHANNEL_COUNT) {
            continue;
        }
        const runs = runsOfSegments(c.segments);
        if (runs.length === 0) continue;
        const cfg = (channels || [])[c.ch - 1] || {};
        out.push({
            ch: c.ch,
            stereo: cfg.source_channels === 2,
            runs,
        });
    }
    return out.sort((a, b) => a.ch - b.ch);
}

/**
 * 轨迹图的时间线全长(秒)。
 *
 * 判据与 Tab3 的 `durationOf`(tab-wave.js)**逐条同款**:range 终点、段表最大 t1S、
 * 播放头三个证据取大,都拿不到就压在兜底值上;播放头只是「工程至少这么长」的下界,
 * 不能单独当长度(否则刚起播时视口会塌到 1 秒再慢慢爬回来)。
 *
 * 为什么不 `import { durationOf } from "./tab-wave.js"`:tab-wave 反过来 import 本文件
 * (`format` / `outputPhase` / `lowSampleChannels`),直接引会成模块环。两处口径不许漂 ——
 * `web-preview/tests/smoke-t43-chart.mjs` 拿同一份 fixture 断言两者同值。
 */
export const CHART_FALLBACK_DURATION_S = 300;

export function chartDurationS(store) {
    const st = store || {};
    let d = 0;
    const range = ((st.state || {}).global || {}).range || {};
    d = Math.max(d, num(range.end_s, 0));
    for (const c of (st.segments && st.segments.channels) || []) {
        for (const seg of (c && c.segments) || []) {
            d = Math.max(d, num(seg && seg.t1S, 0));
        }
    }
    const ph = num(st.playhead && st.playhead.timeS, 0);
    return Math.max(d > 0 ? d : CHART_FALLBACK_DURATION_S, ph);
}

/**
 * 轨迹图的脏位这一帧要不要**兑现**(= 清标志 + 真去重绘)。
 *
 * 只有可见时才算数。`invalidate()` 在不可见时早退不画(05 §6.1 空闲零 rAF),
 * 这一帧清了脏位就等于**把这次重绘诉求丢掉**,切回 Tab1 会看到一张过期的图 ——
 * 段表变了而线还是旧的,且没有任何后续事件会来补这一笔(脏位只在数据变化时置)。
 *
 * 这条路真的走得到:`onSegments` 里有一个 `setTimeout(render, 1650)`,它调的是
 * **本模块自己的** render,绕开了 app.js 那个「只投影当前激活 tab」的 switch。
 * 分析完成后 1.65s 内切走(且停在轨迹档),定时器就会在不可见时跑到这里。
 *
 * @param {boolean} dirty 脏位
 * @param {boolean} visible 轨迹图当前是否真的可见
 */
export function chartRepaintNow(dirty, visible) {
    return !!dirty && !!visible;
}

/**
 * 图例行(两视图共用;行集 = **当前视图真的画了的那些轨**)。
 *
 * 两视图的可见轨集本来就不同 —— 分布图画的是「已连接轨」(空闲轨没有参数值,
 * 画出来是居中的幽灵柱),轨迹图画的是「有分段的轨」(没分析过的轨没有线)。
 * 图例跟着当前视图走,才不会出现「图例里有它、图上找不到它」。
 *
 * @param {string} mode `distribution` | `trajectory`
 * @param {number[]} connected 已连接轨号(分布图口径)
 * @param {{ch:number, stereo:boolean}[]} series 轨迹图数据面
 * @param {Array} channels `state.channels`(取 label 与 source_channels)
 * @returns {{ch:number, label:string, stereo:boolean}[]}
 */
export function legendRows(mode, connected, series, channels) {
    const chans = channels || [];
    const list =
        mode === "trajectory"
            ? (series || []).map((s) => s.ch)
            : (connected || []).slice();
    return list
        .filter((ch) => Number.isFinite(ch) && ch >= 1 && ch <= CHANNEL_COUNT)
        .sort((a, b) => a - b)
        .map((ch) => {
            const cfg = chans[ch - 1] || {};
            return {
                ch,
                label: String(cfg.label || ""),
                stereo: cfg.source_channels === 2,
            };
        });
}

// =============================================================================
// 二、DOM 接线
// =============================================================================

/**
 * @param {{
 *   root: Document|Element,
 *   bridge: object|null,
 *   getStore: () => object,
 *   getT: () => object,
 *   onLocalChange: () => void
 * }} opts
 *   getStore() 返回 app.js 维护的事件仓(state / params / groups / playhead /
 *   segments / coverage / preview);onLocalChange() 请求一次重渲染(乐观本地态用)。
 */
export function createTabMaster(opts) {
    const { root, bridge, getStore, getT, onLocalChange } = opts;
    const $ = (gb) => root.querySelector(`[data-gb="${gb}"]`);
    const call = makeCaller(bridge);

    // ---- 页面内一次性状态(不属 state chunk,重开面板即重置)----------------
    const local = {
        pendingGroup: 0, // 改组确认条的预选组(0 = 未弹)
        analyzeFlashUntil: 0, // data-analyze="done" 闪绿的截止时刻
        analyzePending: false, // analyze 受理回执→state 确认之间的在途标志
        preview: null, // previewAnalyze 的最近一次返回
        previewTimer: 0,
        previewKey: "", // scope 指纹,变了才重新 previewAnalyze
        gesture: null, // 拖动中的 ParamID(灰显判定要排除自己)
        paramEcho: {}, // 本地乐观值(gesture 期间先行显示;由 onParams 逐帧失效,见 nextParamEcho)
        rampLocal: null, // 过渡拖动中的本地 ms
        // [J75] T43:视图态的本地乐观值。null = 以 state 为准;非 null = 点过切换但
        // state 还没回推(桥函数转正前**永远**是这一路,见 setChartMode 的 TODO)。
        chartMode: null,
        chartHi: 0, // 图例 hover 高亮的轨号(0 = 无);纯展示
        traj: null, // 轨迹图实例(web/shared/trajectory-chart.js)
        trajSeries: [], // 轨迹图数据面(段表投影;段表事件到达时重算)
        // y 轴刻度。数据面(pan + y 位)由轨迹图按纵向缩放档推来(onPanAxis),
        // 文案在这一侧按当前字典拼 —— 那模块不认 i18n(它的「复用契约」)。
        trajAxisTicks: [],
        trajAxisKey: "", // 已渲染的刻度列签名(含语言;不变就不重建 DOM)
        // 画布重绘的脏位。render() 走 rAF 合帧、播放中每秒跑十几次,而轨迹图的
        // **静态层**只随段表/视图/尺寸变 —— 不设脏位就会每帧重画 15 条折线。
        trajDirty: true,
        // ui.scale 档位账(Tab3 的 local.lastUiScale 同款,见 tab-wave.js 的 render)。
        lastUiScale: NaN,
    };

    const el = {
        flow: $("master-flow"),
        capSwitch: $("master-capture-toggle-switch"),
        outSwitch: $("master-output-toggle-switch"),
        analyzeBtn: $("master-analyze-button"),
        preview: $("master-analyze-preview"),
        coverage: $("master-analyze-coverage"),
        writeConfirm: $("master-write-confirm"),
        writeConfirmText: $("master-write-confirm-text"),
        writeConfirmOk: $("master-write-confirm-ok"),
        writeConfirmUndo: $("master-write-confirm-undo"),
        emptyState: $("master-emptystate"),
        groupCard: $("master-group-selector"),
        groupSeg: $("master-group-seg"),
        groupConfirm: $("master-group-switch-confirm"),
        groupConfirmText: null, // 见下(inline-confirm__text 无独立 data-gb)
        groupCancel: $("master-group-switch-cancel"),
        groupOk: $("master-group-switch-confirm-btn"),
        widthCard: $("master-width"),
        widthSlider: $("master-width-slider"),
        widthRead: $("master-width-reading"),
        msCard: $("master-msbalance"),
        msSlider: $("master-msbalance-slider"),
        msRead: $("master-msbalance-reading"),
        leadCard: $("master-leadselect"),
        leadWrap: $("master-leadselect-select"),
        leadTrigger: $("master-leadselect-trigger"),
        leadLabel: $("master-leadselect-label"),
        leadPanel: $("master-leadselect-panel"),
        distCard: $("master-distchart"),
        distBars: $("master-distchart-bars"),
        // [J75] T43 双视图
        chartSeg: $("master-chart-mode-seg"),
        chartLegend: $("master-chart-legend"),
        trajCanvas: $("master-trajchart-canvas"),
        trajPlayhead: $("master-trajchart-playhead"),
        trajFollow: $("master-trajchart-follow"),
        trajReset: $("master-trajchart-reset"),
        trajZoom: $("master-trajchart-zoom"),
        trajEmpty: $("master-trajchart-empty"),
        trajAxisY: $("master-trajchart-axis-y"),
        transSlider: $("master-transition-slider"),
        transRead: $("master-transition-reading"),
        rampPath: $("master-transition-ramp-path"),
        rampA: $("master-transition-ramp-mark-a"),
        rampB: $("master-transition-ramp-mark-b"),
        rangeCard: $("master-range"),
        rangeSeg: $("master-range-seg"),
        rangeFollowHint: $("master-range-follow-hint"),
        rangeStart: $("master-range-start-bars"),
        rangeEnd: $("master-range-end-bars"),
        rangeTimecode: $("master-range-timecode"),
        rangeSetPlayhead: $("master-range-setplayhead"),
        rangeMinus4: $("master-range-step-minus4"),
        rangePlus4: $("master-range-step-plus4"),
        rangeRecapture: $("master-range-recapture-badge"),
        curveCard: $("master-pancurve"),
        curvePath: $("master-pancurve-path"),
    };
    if (el.groupConfirm) {
        el.groupConfirmText = el.groupConfirm.querySelector(
            ".inline-confirm__text",
        );
    }

    // ---------------------------------------------------------------- mount
    function mount() {
        buildGroupPills();
        wireFlow();
        wireGroup();
        wireParamSliders();
        wireTransition();
        wireRange();
        wireChart();
    }

    // ⓪ 八枚 A-H 组胶囊:结构一致,批量出 DOM(状态位在 render 里刷)。
    function buildGroupPills() {
        if (!el.groupSeg) return;
        el.groupSeg.innerHTML = GROUP_IDS.map(
            (id, i) => `
      <button class="group-pill" data-glow="1" data-group="${i + 1}"
              aria-pressed="false" data-online="0" data-pending="0"
              data-gb="master-group-${id}">
        ${id}<span class="group-pill__dot" aria-hidden="true"></span>
      </button>`,
        ).join("");
    }

    // ---- ① 三件套 --------------------------------------------------------
    function wireFlow() {
        onActivate(el.capSwitch, () => {
            if (isWriteBlocked()) return;
            const s = getStore().state;
            const on = !!(s.global && s.global.capture_enabled);
            call("setCaptureEnabled", !on);
        });

        onActivate(el.outSwitch, () => {
            if (isWriteBlocked()) return;
            const s = getStore().state;
            const on = !!(s.global && s.global.output_enabled);
            if (on) {
                call("setOutputEnabled", false);
                hideWriteConfirm();
                return;
            }
            call("setOutputEnabled", true);
            // 每工程会话首次 OFF→ON 就地展开双后果文案(非模态);
            // **与加载守卫互斥**:守卫横幅⑦在场时不补弹(05 §2.0 横幅⑦逐字)。
            const guarded = !!(s.print_guard && s.print_guard.pending);
            if (!getStore().session.writeConfirmSeen && !guarded) {
                getStore().session.writeConfirmSeen = true;
                showWriteConfirm();
            }
        });

        if (el.writeConfirmOk) {
            el.writeConfirmOk.addEventListener("click", hideWriteConfirm);
        }
        if (el.writeConfirmUndo) {
            el.writeConfirmUndo.addEventListener("click", () => {
                call("setOutputEnabled", false);
                hideWriteConfirm();
            });
        }

        if (el.analyzeBtn) {
            el.analyzeBtn.addEventListener("click", async () => {
                if (!el.flow) return;
                if (el.flow.getAttribute("data-analyze") === "disabled") return;
                if (el.flow.getAttribute("data-analyze") === "running") {
                    // 契约 §1.7:进行中再点 = 取消
                    local.analyzePending = false;
                    await call("cancelAnalyze");
                    return;
                }
                // 在途标志:受理回执到 state 确认之间还有别的事件帧(params/playhead)
                // 会触发 render,只写 DOM 属性会被 renderFlow 冲回——flag 让它撑住
                // (PR #52 bot 建议);state.analysis_run 确认后由 renderFlow 清。
                local.analyzePending = true;
                el.flow.setAttribute("data-analyze", "running");
                const res = await call(
                    "analyze",
                    analyzeScope(getStore().state),
                );
                // 受理回执 ≠ 最终结果(契约 §1.6):失败(busy / 空影响面)立刻回落,
                // 成功则等 scvb.state.analysis_run 与 scvb.segments 接管。
                if (!res || res.ok === false) {
                    local.analyzePending = false;
                    render();
                }
            });
        }
    }

    function showWriteConfirm() {
        if (el.writeConfirm) el.writeConfirm.hidden = false;
        render();
    }
    function hideWriteConfirm() {
        if (el.writeConfirm) el.writeConfirm.hidden = true;
    }

    /** 只读观察态(契约 §5.1 `secondOutput`)与无时间线(§5.1 `noTimeline`)下全写控件失效。 */
    function isWriteBlocked() {
        const st = getStore();
        return !!(st.readOnly || st.noTimeline);
    }

    // ---- ⓪ 组选择 --------------------------------------------------------
    function wireGroup() {
        if (el.groupSeg) {
            el.groupSeg.addEventListener("click", (e) => {
                const btn =
                    e.target instanceof Element
                        ? e.target.closest("[data-group]")
                        : null;
                if (!btn) return;
                // PRINT 态整组 disabled(05 §2.1 ⓪);只读观察态同样不可写。
                if (
                    el.groupCard &&
                    el.groupCard.getAttribute("data-disabled") === "1"
                )
                    return;
                const g = Number(btn.getAttribute("data-group"));
                const cur = getStore().state.group_id || 1;
                if (!g || g === cur) return;
                local.pendingGroup = g;
                render();
            });
        }
        if (el.groupCancel) {
            el.groupCancel.addEventListener("click", () => {
                local.pendingGroup = 0;
                render();
            });
        }
        if (el.groupOk) {
            el.groupOk.addEventListener("click", async () => {
                const g = local.pendingGroup;
                local.pendingGroup = 0;
                if (!g) return;
                const res = await call("setGroupId", g);
                // 契约 §1.4:新组 OutputSlot 已被占 → 本实例进只读观察(不是错误码)。
                if (res && res.observer) getStore().readOnly = true;
                render();
            });
        }
    }

    // ---- ④ Width / MS / Lead:gesture 三段式(契约 §1.12-§1.14)------------
    function wireParamSliders() {
        wireSliderGesture(el.widthSlider, "width");
        wireSliderGesture(el.msSlider, "ms_balance");

        wireLeadDropdown();
    }

    /**
     * 主唱选择自定义下拉(用户反馈 2026-08-18:原生 <select> 弹层不可样式化)。
     * 选中 = 一个完整 gesture(begin → set → end,同原生 change 语义);
     * 键盘等价原生 select:焦点在触发钮上时 ↑/↓ 逐项换值(不必开面板)。
     */
    function wireLeadDropdown() {
        if (!el.leadTrigger || !el.leadWrap) return;
        const openLead = (open) => {
            el.leadWrap.setAttribute("data-open", open ? "1" : "0");
            el.leadTrigger.setAttribute(
                "aria-expanded",
                open ? "true" : "false",
            );
        };
        el.leadTrigger.addEventListener("click", () => {
            openLead(el.leadWrap.getAttribute("data-open") !== "1");
        });
        el.leadTrigger.addEventListener("keydown", (e) => {
            if (e.key !== "ArrowUp" && e.key !== "ArrowDown") return;
            e.preventDefault();
            const cur = Math.round(readParam("lead_select"));
            const v = clamp(
                0,
                CHANNEL_COUNT,
                cur + (e.key === "ArrowDown" ? 1 : -1),
            );
            if (v !== cur) oneShotGesture("lead_select", v);
        });
        if (el.leadPanel) {
            el.leadPanel.addEventListener("click", (e) => {
                const opt =
                    e.target instanceof Element
                        ? e.target.closest("[data-lead]")
                        : null;
                if (!opt) return;
                openLead(false);
                el.leadTrigger.focus();
                oneShotGesture(
                    "lead_select",
                    clamp(0, CHANNEL_COUNT, Number(opt.dataset.lead)),
                );
            });
        }
        // 点外关、Esc 关(与缩放下拉同手感)
        root.addEventListener("pointerdown", (e) => {
            if (!(e.target instanceof Node) || !el.leadWrap.contains(e.target))
                openLead(false);
        });
        root.addEventListener("keydown", (e) => {
            if (e.key === "Escape") openLead(false);
        });
    }

    /**
     * 一条滑轨的三段式:pointerdown = begin,拖动中节流 setParam(≤50Hz),
     * pointerup/cancel = end;双击回默认值(05 §2.1 ④ msHint 逐字「双击回 0」);
     * 键盘(role="slider" + tabindex)方向键每次一个完整三段式。
     */
    function wireSliderGesture(node, id) {
        if (!node) return;
        const rng = PARAM_RANGES[id];
        let dragging = false;
        let lastSent = 0;

        const valueAt = (clientX) => {
            const r = node.getBoundingClientRect();
            const ratio = r.width > 0 ? (clientX - r.left) / r.width : 0;
            const raw = rng.min + clamp(0, 1, ratio) * (rng.max - rng.min);
            return Math.round(raw / rng.step) * rng.step;
        };

        node.addEventListener("pointerdown", (e) => {
            if (isParamBlocked()) return;
            dragging = true;
            local.gesture = id;
            try {
                node.setPointerCapture(e.pointerId);
            } catch {
                /* 无指针捕获也能用,只是拖出元素外会断 */
            }
            call("beginParamGesture", id);
            sendParam(id, valueAt(e.clientX));
            lastSent = now();
            e.preventDefault();
        });

        node.addEventListener("pointermove", (e) => {
            if (!dragging) return;
            // 契约 §1.12-§1.14 线程/频率行:拖动期间 setParam 由 UI 侧节流(建议 ≤50Hz)
            if (now() - lastSent < 20) return;
            lastSent = now();
            sendParam(id, valueAt(e.clientX));
        });

        const finish = () => {
            if (!dragging) return;
            dragging = false;
            local.gesture = null;
            call("endParamGesture", id);
        };
        node.addEventListener("pointerup", finish);
        node.addEventListener("pointercancel", finish);

        node.addEventListener("dblclick", () => {
            if (isParamBlocked()) return;
            oneShotGesture(id, PARAM_DEFAULTS[id]);
        });

        node.addEventListener("keydown", (e) => {
            if (isParamBlocked()) return;
            const dir =
                e.key === "ArrowRight" || e.key === "ArrowUp"
                    ? 1
                    : e.key === "ArrowLeft" || e.key === "ArrowDown"
                      ? -1
                      : 0;
            if (!dir) return;
            e.preventDefault();
            const cur = readParam(id);
            const step = rng.step * (e.shiftKey ? 10 : 1);
            oneShotGesture(id, clamp(rng.min, rng.max, cur + dir * step));
        });
    }

    function oneShotGesture(id, value) {
        call("beginParamGesture", id);
        sendParam(id, value);
        call("endParamGesture", id);
    }

    function sendParam(id, value) {
        local.paramEcho[id] = value; // 乐观本地态:等 25Hz 的 scvb.params 回来之前先动起来
        call("setParam", id, value);
        if (onLocalChange) onLocalChange();
        else render();
    }

    /**
     * §0.5 防回环的正确读法:「**发起端唯一**」—— UI 只在**用户直接操作**时上行 `setParam`,
     * 收到的 `scvb.params`(含 `hostEcho:true`)一律只更新显示、绝不回写。本实现天然满足:
     * 所有上行都挂在 pointer/键盘/dblclick 事件上,渲染路径里没有一处 call("setParam")。
     *
     * **`hostEcho` 因此只做灰显视觉,绝不阻断用户 gesture** —— 契约 §1.12-§1.14 拒绝态行
     * 逐字「无(PRINT/ARMED 下照常允许——这是宿主可录的用户操作面)」;05 §2.1 ④ 的
     * 「被宿主自动化覆盖时滑杆与角度读数一并跟随」也是**跟随**而非禁用。
     * 且 `hostEcho` 是「最近一批」的标志:PRINT 停止后若值不再变(§0.4 值未变不发),
     * 按 hostEcho 禁操作会让四张参数卡**永久灰死**。
     *
     * 真正该挡的只有写权限缺失:只读观察(§5.1 `secondOutput`)与无时间线(§5.1 `noTimeline`)。
     */
    function isParamBlocked() {
        return isWriteBlocked();
    }

    function readParam(id) {
        const vals = getStore().params.values || {};
        if (Object.prototype.hasOwnProperty.call(local.paramEcho, id))
            return local.paramEcho[id];
        return Object.prototype.hasOwnProperty.call(vals, id)
            ? vals[id]
            : PARAM_DEFAULTS[id];
    }

    // ---- ④ 过渡时间(契约 §1.20)------------------------------------------
    function wireTransition() {
        const node = el.transSlider;
        if (!node) return;
        let dragging = false;
        let lastSent = 0;
        const valueAt = (clientX) => {
            const r = node.getBoundingClientRect();
            const ratio = r.width > 0 ? (clientX - r.left) / r.width : 0;
            return Math.round(
                RAMP_MS.min + clamp(0, 1, ratio) * (RAMP_MS.max - RAMP_MS.min),
            );
        };
        const push = (ms) => {
            local.rampLocal = ms;
            call("setTransitionRamp", ms);
            if (onLocalChange) onLocalChange();
            else render();
        };
        node.addEventListener("pointerdown", (e) => {
            if (isWriteBlocked()) return;
            dragging = true;
            try {
                node.setPointerCapture(e.pointerId);
            } catch {
                /* 同上 */
            }
            push(valueAt(e.clientX));
            lastSent = now();
            e.preventDefault();
        });
        node.addEventListener("pointermove", (e) => {
            if (!dragging || now() - lastSent < 20) return;
            lastSent = now();
            push(valueAt(e.clientX));
        });
        const finish = () => {
            dragging = false;
            local.rampLocal = null; // 松手后交回 scvb.state 的回推值(C++ 会夹取到 [20,300])
        };
        node.addEventListener("pointerup", finish);
        node.addEventListener("pointercancel", finish);
        node.addEventListener("dblclick", () => {
            if (isWriteBlocked()) return;
            push(RAMP_MS.def);
        });
        node.addEventListener("keydown", (e) => {
            const dir =
                e.key === "ArrowRight" || e.key === "ArrowUp"
                    ? 1
                    : e.key === "ArrowLeft" || e.key === "ArrowDown"
                      ? -1
                      : 0;
            if (!dir || isWriteBlocked()) return;
            e.preventDefault();
            const cur = currentRampMs();
            push(
                clamp(
                    RAMP_MS.min,
                    RAMP_MS.max,
                    cur + dir * (e.shiftKey ? 10 : 1),
                ),
            );
        });
    }

    function currentRampMs() {
        if (local.rampLocal !== null) return local.rampLocal;
        const a = getStore().state.analysis;
        return a && Number.isFinite(a.transition_ramp_ms)
            ? a.transition_ramp_ms
            : RAMP_MS.def;
    }

    // ---- ② Range 三档(契约 §1.8)------------------------------------------
    function wireRange() {
        if (el.rangeSeg) {
            el.rangeSeg.addEventListener("click", async (e) => {
                const btn =
                    e.target instanceof Element
                        ? e.target.closest("[data-range-mode]")
                        : null;
                if (!btn || isWriteBlocked()) return;
                if (btn.getAttribute("data-disabled") === "1") return;
                const ui = btn.getAttribute("data-range-mode");
                await applyRange(RANGE_UI_TO_MODE[ui]);
            });
        }
        for (const input of [el.rangeStart, el.rangeEnd]) {
            if (!input) continue;
            input.addEventListener("change", () => applyRange("manual"));
            input.addEventListener("keydown", (e) => {
                if (e.key === "Enter") applyRange("manual");
            });
        }
        if (el.rangeSetPlayhead) {
            el.rangeSetPlayhead.addEventListener("click", () => {
                const t = getStore().playhead
                    ? getStore().playhead.timeS
                    : null;
                if (t === null || t === undefined) return;
                // 就近端点吸到播放头(设计稿只给一个「设为播放头」钮,不区分起止)
                const cur = readManualRange();
                let startS = cur.startS;
                let endS = cur.endS;
                if (startS === endS) {
                    // 空区间(如双 0)时就近吸附会写出 start >= end 的非法区间
                    // 被 applyRange 静默拒 —— 撑成 [0, 播放头] 合法区间
                    // (PR #52 bot 建议)。
                    startS = 0;
                    endS = Math.max(t, 0.001);
                } else if (Math.abs(t - startS) <= Math.abs(t - endS)) {
                    startS = t;
                } else {
                    endS = t;
                }
                writeManualInputs(startS, endS);
                applyRange("manual");
            });
        }
        if (el.rangeMinus4) {
            el.rangeMinus4.addEventListener("click", () => nudgeRange(-4));
        }
        if (el.rangePlus4) {
            el.rangePlus4.addEventListener("click", () => nudgeRange(+4));
        }
    }

    function nudgeRange(deltaS) {
        const cur = readManualRange();
        const endS = Math.max(cur.startS + 0.001, cur.endS + deltaS);
        writeManualInputs(cur.startS, endS);
        applyRange("manual");
    }

    function readManualRange() {
        const g = getStore().state.global || {};
        const fallback = g.range || { start_s: 0, end_s: 0 };
        const s = timecodeToSeconds(el.rangeStart && el.rangeStart.value);
        const e = timecodeToSeconds(el.rangeEnd && el.rangeEnd.value);
        return {
            startS: s === null ? fallback.start_s : s,
            endS: e === null ? fallback.end_s : e,
            valid: s !== null && e !== null && s < e,
        };
    }

    /**
     * 手动档起止的种子值。优先级:
     *   ① state 里已有有效区间(manual / daw_loop 档都给得出);
     *   ② 已分析区域的外包络(§2.8 段表,真数据);
     *   ③ [0, 播放头](§2.6);
     * 三者皆无 ⇒ null(输入框保持原样,用户自己填 —— 不编一个假区间出来)。
     */
    function manualSeed(st, range) {
        if (range && range.end_s > range.start_s) {
            return { startS: range.start_s, endS: range.end_s };
        }
        const spans = ((st.segments && st.segments.channels) || []).flatMap(
            (c) => c.segments || [],
        );
        if (spans.length > 0) {
            return {
                startS: Math.min(...spans.map((s) => s.t0S)),
                endS: Math.max(...spans.map((s) => s.t1S)),
            };
        }
        const t = st.playhead ? st.playhead.timeS : 0;
        return t > 0 ? { startS: 0, endS: t } : null;
    }

    function writeManualInputs(startS, endS) {
        if (el.rangeStart) el.rangeStart.value = secondsToTimecode(startS);
        if (el.rangeEnd) el.rangeEnd.value = secondsToTimecode(endS);
    }

    /**
     * 三档统一入口。follow / daw_loop 忽略起止(契约 §1.8「follow 时忽略 startS/endS」),
     * manual 要求 startS < endS —— 校验不过就**不发调用**并把两个输入框标红,
     * 而不是让 C++ 回 `{ok:false, reason:"badArg"}` 再补救(§0.8 第 2 条只兜底,不是交互设计)。
     */
    async function applyRange(mode) {
        if (mode === "manual") {
            const r = readManualRange();
            setInvalid(!r.valid);
            if (!r.valid) return;
            await call("setRange", "manual", r.startS, r.endS);
            return;
        }
        setInvalid(false);
        const res = await call("setRange", mode, 0, 0);
        // 契约 §1.8 拒绝态:宿主从不提供 loop 字段 → `noLoop`,该档 disabled 但**绝不隐藏**。
        if (res && res.ok === false && res.reason === "noLoop") {
            getStore().loopMissing = true;
            render();
        }
    }

    function setInvalid(bad) {
        for (const input of [el.rangeStart, el.rangeEnd]) {
            if (input) input.setAttribute("data-invalid", bad ? "1" : "0");
        }
    }

    // ---- [J75] T43 分布图双视图 + 轨道配色 --------------------------------

    /** 当前视图态:本地乐观值优先,否则以 state 为准(默认 distribution)。 */
    function currentChartMode() {
        return local.chartMode || chartModeOf(getStore().state);
    }

    /** Tab1 是否为当前激活页(四面板同在 DOM,#content[data-tab] 切换;同 Tab3 口径)。 */
    function isPanelActive() {
        const node = el.distCard;
        if (!node || typeof node.closest !== "function") return true;
        const host = node.closest("[data-tab]");
        return !host || host.getAttribute("data-tab") === "master";
    }

    function wireChart() {
        if (el.chartSeg) {
            for (const btn of el.chartSeg.querySelectorAll(
                "[data-chart-mode]",
            )) {
                onActivate(btn, () =>
                    setChartMode(btn.getAttribute("data-chart-mode")),
                );
            }
        }

        if (el.trajCanvas) {
            local.traj = createTrajectoryChart({
                canvas: el.trajCanvas,
                playheadEl: el.trajPlayhead,
                followBtn: el.trajFollow,
                resetPanBtn: el.trajReset,
                zoomEl: el.trajZoom,
                getSeries: () => local.trajSeries,
                getDurationS: () => chartDurationS(getStore()),
                getUiScale: () =>
                    num(((getStore().state || {}).ui || {}).scale, 1),
                // 不画不起 rAF 的两道闸(05 §6.1 空闲零 rAF):视图切到分布档,
                // 或 Tab1 根本不是当前页(四面板同在 DOM,#content[data-tab] 切换)。
                isVisible: () =>
                    currentChartMode() === "trajectory" && isPanelActive(),
                onFollowChange: () => onLocalChange(),
                // 纵向缩放/平移/改尺寸后刻度就变了。轨迹图只推「pan + y 位」,
                // 方位词与排版在这一侧;推来即渲染,不等下一帧 render(纵向缩放
                // 常发生在空闲态,那时 render 根本不跑,等它刻度会滞后一整拍)。
                onPanAxis: (ticks) => {
                    local.trajAxisTicks = ticks;
                    renderTrajAxis(getT());
                },
            });
        }

        // 图例 hover 联动(J75 B「纯展示,无写操作」)。事件委托挂容器,不逐行挂 ——
        // 行是每帧按可见轨集重建的,逐行挂会随重建泄漏(与 Tab2 密排行同一个理由)。
        if (el.chartLegend) {
            el.chartLegend.addEventListener("pointerover", (e) =>
                setChartHighlight(legendChOf(e.target)),
            );
            el.chartLegend.addEventListener("pointerleave", () =>
                setChartHighlight(0),
            );
        }
    }

    /** 事件目标 → 图例行的轨号(不在行上即 0)。 */
    function legendChOf(target) {
        if (!target || typeof target.closest !== "function") return 0;
        const row = target.closest("[data-legend-ch]");
        return row ? Number(row.getAttribute("data-legend-ch")) || 0 : 0;
    }

    function setChartHighlight(ch) {
        const v = Number(ch) || 0;
        if (local.chartHi === v) return;
        local.chartHi = v;
        if (local.traj) local.traj.setHighlight(v);
        onLocalChange();
    }

    /**
     * 视图切换。
     *
     * `setMasterChartMode` 已进契约 §7 manifest(§1.35,[J75] T43),桥上按
     * `BRIDGE_FUNCTIONS` 直接挂载:native 侧 state codec 落 `ui.master_chart_mode`
     * + 桥 setter(形制照 §1.31 `setActiveTab`:`{ok:true}` / `{ok:false, reason:"badArg"}`),
     * 调用落地 → `scvb.state` 回推 `ui.master_chart_mode` → 下面的 renderChart 把本地
     * 乐观值交还给 state,「切换态持久化往返」在 mock 与真宿主下都成立。字段定义 /
     * 默认值 / 迁移语义:`docs/contract-changes/20260825-master-chart-mode.md`。
     */
    function setChartMode(mode) {
        if (!CHART_MODES.includes(mode) || currentChartMode() === mode) return;
        local.chartMode = mode;
        local.trajDirty = true;
        call("setMasterChartMode", mode);
        onLocalChange();
    }

    // ---------------------------------------------------------------- render
    function render() {
        const st = getStore();
        const t = getT();
        const s = st.state || {};
        const g = s.global || {};
        const phase = outputPhase(s, st.playhead);

        renderFlow(st, t, s, g);
        renderGroup(st, t, s, phase);
        renderParams(st);
        renderTransition();
        renderRange(st, t, g);
        renderCurve(s);
        renderDist(st, s);
        renderChart(st, s, t);
        renderEmptyState(st);
    }

    // ① 三件套 ------------------------------------------------------------
    function renderFlow(st, t, s, g) {
        if (!el.flow) return;
        el.flow.setAttribute("data-cap", captureVisual(s, st.playhead));
        el.flow.setAttribute("data-out", g.output_enabled ? "1" : "0");
        el.flow.setAttribute(
            "data-lock",
            s.print_guard && s.print_guard.pending ? "1" : "0",
        );

        // 影响预览 {n}/{m}/{k}:previewAnalyze 优先,拿不到时用 scvb.segments 兜底
        const totals = local.preview
            ? {
                  n: local.preview.intervals,
                  m: local.preview.tracks,
                  k: local.preview.manualKept,
              }
            : segmentTotals(st.segments);
        fill(el.preview, t, "master.step2.desc", totals);
        // 覆盖率行:一轨都没报到(未播放过 / 首帧未到)就整行隐掉,
        // 而不是显示一个假的「0% 已覆盖」—— §2.7 非播放不发,这行本来就无数据可依。
        const p = coveragePercent(st.coverage);
        if (el.coverage) el.coverage.hidden = p === null;
        if (p !== null) fill(el.coverage, t, "master.step2.coverage", { p });

        // 分析按钮四态(单一状态源 = data-analyze)。
        // disabled 判「无数据」取覆盖与段表的**并集**:只看 totals.n(已分析段数)会鸡生蛋——
        // 首次采集完还没分析过,段表恒空,分析键永远点不开(PR #52 bot 建议);
        // 只看覆盖率又会误伤重开工程——§2.7 captureProgress 非播放不发,覆盖帧
        // 未到但段表有货的工程本可再分析。两者都空才是真没数据。
        let an = "ready";
        if (s.analysis_run && s.analysis_run.running) {
            local.analyzePending = false; // 状态面已确认,交回 state 驱动
            an = "running";
        } else if (now() < local.analyzeFlashUntil) an = "done";
        else if (local.analyzePending)
            an = "running"; // 受理回执前的在途窗口
        else if (isWriteBlocked() || analyzeNoData(p, totals.n))
            an = "disabled";
        el.flow.setAttribute("data-analyze", an);
        // disabled 的原因面分离(PR #52 bot 建议 4):写权限缺失(只读/无时间线)时
        // 不能亮「当前范围内无采集数据」——真实原因由横幅②/⑥承载,原因句只留给 nodata。
        el.flow.setAttribute(
            "data-analyze-reason",
            an === "disabled" && isWriteBlocked() ? "blocked" : "nodata",
        );

        for (const sw of [el.capSwitch, el.outSwitch]) {
            if (sw)
                sw.setAttribute("data-disabled", isWriteBlocked() ? "1" : "0");
        }

        // write 确认条正文:follow 档走 .follow 变体(无 {x}–{y} 空洞)
        if (el.writeConfirmText) {
            const key = writeConfirmKey(g.range ? g.range.mode : "follow");
            el.writeConfirmText.setAttribute("data-t", key);
            const regions = analyzedRegions(st.segments);
            fill(el.writeConfirmText, t, key, {
                v: versionNameOf(s, g.version_active || 1),
                x: secondsToTimecode(g.range ? g.range.start_s : 0),
                y: secondsToTimecode(g.range ? g.range.end_s : 0),
                n: regions.n,
                t: regions.t,
            });
        }
    }

    // ⓪ 组选择 ------------------------------------------------------------
    function renderGroup(st, t, s, phase) {
        const cur = s.group_id || 1;
        if (el.groupSeg) {
            for (const btn of el.groupSeg.querySelectorAll("[data-group]")) {
                const gi = Number(btn.getAttribute("data-group"));
                btn.setAttribute("aria-pressed", String(gi === cur));
                btn.setAttribute(
                    "data-online",
                    String(groupOnline(st.groups, gi)),
                );
                btn.setAttribute(
                    "data-pending",
                    local.pendingGroup === gi ? "1" : "0",
                );
            }
        }
        if (el.groupCard) {
            // PRINT 态整组 disabled(05 §2.1 ⓪);只读观察态同理。
            const off = phase === "print" || isWriteBlocked();
            el.groupCard.setAttribute("data-disabled", off ? "1" : "0");
            // 已展开的改组确认条随禁用一并收起(PR #52 bot 建议):否则确认钮
            // 仍可在 PRINT/只读态下提交 setGroupId,绕过整卡 disabled。
            if (off) local.pendingGroup = 0;
            // 05 §2.1 ⓪ 逐字要求 disabled **+ tooltip**;词条 master.printLock.group(T31 新增)。
            // title 不走 applyI18n(它只刷 data-t / data-t-aria),故每次渲染按当前字典重写。
            setTitle(
                el.groupCard,
                phase === "print" ? t["master.printLock.group"] : "",
            );
            el.groupCard.setAttribute(
                "data-confirm",
                local.pendingGroup ? "1" : "0",
            );
        }
        if (local.pendingGroup) {
            const letter = GROUP_IDS[local.pendingGroup - 1];
            fill(el.groupConfirmText, t, "group.switchConfirm.out", {
                x: letter,
                n: connectedCount(st.conn),
            });
            fill(el.groupOk, t, "group.switchConfirm.primary", { x: letter });
        }
    }

    /**
     * Lead Select 的 15 个「{ch} · {label}」选项来自 `scvb.state.channels[].label`
     * (契约 §2.1);自定义下拉面板(用户反馈 2026-08-18:原生 <select> 弹层
     * 不可样式化),0 项「遵循分析」随选项一起重建、带 data-t。
     * 标签签名没变就不重建 —— 重建会打断用户正在展开的下拉。
     */
    let leadSig = "";
    function renderLeadOptions(s) {
        if (!el.leadPanel) return;
        const chans = s.channels || [];
        const labels = [];
        for (let ch = 1; ch <= CHANNEL_COUNT; ch++) {
            labels.push((chans[ch - 1] && chans[ch - 1].label) || "");
        }
        const sig = labels.join("\u0001");
        if (sig === leadSig) return;
        leadSig = sig;
        // DOM API 建项(轨名是用户数据,不走 innerHTML);0 项带 data-t,
        // 语言切换由 applyI18n 统一刷,重建时用当前字典先填一遍。
        const t = getT();
        const frag = document.createDocumentFragment();
        const mkOpt = (v) => {
            const b = document.createElement("button");
            b.type = "button";
            b.className = "lead-select__opt";
            b.setAttribute("role", "option");
            b.setAttribute("aria-selected", "false");
            b.setAttribute("data-lead", String(v));
            b.setAttribute("data-current", "0");
            return b;
        };
        const follow = mkOpt(0);
        const followText = document.createElement("span");
        followText.setAttribute("data-t", "leadFollowAnalysis");
        followText.textContent = t.leadFollowAnalysis || "遵循分析";
        follow.appendChild(followText);
        frag.appendChild(follow);
        for (let ch = 1; ch <= CHANNEL_COUNT; ch++) {
            const opt = mkOpt(ch);
            const num = document.createElement("span");
            num.className = "lead-select__ch";
            num.textContent = tt(ch);
            opt.appendChild(num);
            // label 为空(空工程、轨未命名)时只显轨号,不留一个悬空的轨名位
            if (labels[ch - 1]) {
                const name = document.createElement("span");
                name.textContent = labels[ch - 1];
                opt.appendChild(name);
            }
            frag.appendChild(opt);
        }
        el.leadPanel.replaceChildren(frag);
    }

    // ④ Width / MS / Lead(scvb.params)-------------------------------------
    function renderParams(st) {
        renderLeadOptions(st.state || {});
        const width = readParam("width");
        const ms = readParam("ms_balance");
        const lead = readParam("lead_select");

        if (el.widthSlider) {
            el.widthSlider.style.setProperty(
                "--v",
                sliderPct(width, 0, 150).toFixed(1) + "%",
            );
            el.widthSlider.setAttribute(
                "aria-valuenow",
                String(Math.round(width)),
            );
        }
        if (el.widthRead) {
            el.widthRead.textContent = "±" + widthAngleDeg(width) + "°";
        }

        if (el.msSlider) {
            const f = msFill(ms);
            el.msSlider.style.setProperty("--ms-left", f.left.toFixed(1) + "%");
            el.msSlider.style.setProperty("--ms-w", f.width.toFixed(1) + "%");
            el.msSlider.style.setProperty("--v", f.knob.toFixed(1) + "%");
            el.msSlider.setAttribute("aria-valuenow", String(Math.round(ms)));
        }
        if (el.msRead) el.msRead.textContent = msReading(ms);

        // 触发钮标签 + 面板当前项(自定义下拉;0 走词条,轨走「NN 轨名」)
        const leadV = Math.round(lead);
        if (el.leadLabel) {
            if (leadV === 0) {
                el.leadLabel.setAttribute("data-t", "leadFollowAnalysis");
                el.leadLabel.textContent =
                    getT().leadFollowAnalysis || "遵循分析";
            } else {
                el.leadLabel.removeAttribute("data-t");
                const chans = (getStore().state || {}).channels || [];
                const label =
                    (chans[leadV - 1] && chans[leadV - 1].label) || "";
                el.leadLabel.textContent = label
                    ? tt(leadV) + " " + label
                    : tt(leadV);
            }
        }
        if (el.leadPanel) {
            for (const opt of el.leadPanel.querySelectorAll("[data-lead]")) {
                const cur = Number(opt.dataset.lead) === leadV;
                opt.setAttribute("data-current", cur ? "1" : "0");
                opt.setAttribute("aria-selected", cur ? "true" : "false");
            }
        }
        if (el.leadCard) {
            el.leadCard.setAttribute("data-selected", lead > 0 ? "1" : "0");
        }

        // hostEcho:ARMED/PRINT 下 scvb.params 来自宿主回吐 —— 只更新显示、绝不回写(§0.5)。
        // **只做灰显,不禁操作**(契约 §1.12-§1.14 拒绝态「无」;见 isParamBlocked 的头注)。
        // 只灰这三张**参数**卡:Range / 过渡 / 曲线窗写的是 state 与曲线真身,不走参数通道,
        // 引擎打印期间照样可改,整片 grid 一起灰会把它们一并锁死。
        // 灰显按批次新鲜度判定(hostEchoAt 由 app.js 记):hostEcho 标志本身停发后不会
        // 翻回 false,直接用它会让四张卡在打印结束后**永久滞留** 55% 透明。
        const echo =
            st.params.hostEcho &&
            Date.now() - (st.params.hostEchoAt || 0) < HOST_ECHO_FRESH_MS
                ? "1"
                : "0";
        for (const node of [
            el.widthCard,
            el.msCard,
            el.leadCard,
            el.distCard,
        ]) {
            if (node) node.setAttribute("data-host-echo", echo);
        }
    }

    // ④ 过渡时间 + 斜坡 SVG -------------------------------------------------
    function renderTransition() {
        const ms = currentRampMs();
        if (el.transSlider) {
            el.transSlider.style.setProperty(
                "--v",
                sliderPct(ms, RAMP_MS.min, RAMP_MS.max).toFixed(1) + "%",
            );
            el.transSlider.setAttribute(
                "aria-valuenow",
                String(Math.round(ms)),
            );
        }
        if (el.transRead) el.transRead.textContent = Math.round(ms) + " ms";
        const geo = rampGeometry(ms);
        if (el.rampPath) el.rampPath.setAttribute("d", geo.d);
        if (el.rampA) {
            el.rampA.setAttribute("x1", String(geo.x0));
            el.rampA.setAttribute("x2", String(geo.x0));
        }
        if (el.rampB) {
            el.rampB.setAttribute("x1", String(geo.x1));
            el.rampB.setAttribute("x2", String(geo.x1));
        }
    }

    // ② Range --------------------------------------------------------------
    function renderRange(st, t, g) {
        const range = g.range || { mode: "follow", start_s: 0, end_s: 0 };
        if (el.rangeCard) {
            el.rangeCard.setAttribute(
                "data-range",
                RANGE_MODE_TO_UI[range.mode] || "follow",
            );
            el.rangeCard.setAttribute(
                "data-loop-missing",
                st.loopMissing ? "1" : "0",
            );
        }
        if (el.rangeSeg) {
            for (const btn of el.rangeSeg.querySelectorAll(
                "[data-range-mode]",
            )) {
                const ui = btn.getAttribute("data-range-mode");
                btn.setAttribute(
                    "aria-pressed",
                    String(RANGE_UI_TO_MODE[ui] === range.mode),
                );
                if (ui === "loop") {
                    btn.setAttribute(
                        "data-disabled",
                        st.loopMissing ? "1" : "0",
                    );
                }
            }
        }
        const regions = analyzedRegions(st.segments);
        fill(el.rangeFollowHint, t, "master.rangeFollowHint", regions);

        // 输入框显示桥面单位 mm:ss.mmm(契约 §1.8 只收秒);编辑中不覆写。
        // **三档都同步**,不只是 manual 档:用户从 follow 切到「手动」时,输入框里必须已经
        // 是一对可提交的秒值,否则第一次点「手动」会被自身的 start<end 校验挡下来。
        const editing =
            document.activeElement === el.rangeStart ||
            document.activeElement === el.rangeEnd;
        if (!editing) {
            const seed = manualSeed(st, range);
            if (seed) writeManualInputs(seed.startS, seed.endS);
        }
        if (el.rangeTimecode) {
            el.rangeTimecode.textContent =
                secondsToTimecode(range.start_s) +
                " → " +
                secondsToTimecode(range.end_s);
        }

        // 重采集布防 badge①(契约 §2.1 `recapture`;05 §2.3「重采集选区」行)
        const rec = st.state.recapture;
        if (el.rangeRecapture) {
            el.rangeRecapture.hidden = !(rec && rec.armed);
            if (rec && rec.armed) {
                fill(el.rangeRecapture, t, "wave.recaptureArmed", {
                    x: secondsToTimecode(rec.startS),
                    y: secondsToTimecode(rec.endS),
                    n: popcount(rec.tracksMask),
                });
            }
        }
    }

    // ④ 曲线窗(统筹增补③:有点集就画折线,不再停在空态)----------------------
    function renderCurve(s) {
        const v = (s.global && s.global.version_active) || 1;
        const version = (s.versions || [])[v - 1];
        const pts =
            version && version.pan_curve ? version.pan_curve.points || [] : [];
        if (el.curveCard) {
            el.curveCard.setAttribute("data-empty", pts.length ? "0" : "1");
        }
        if (el.curvePath) el.curvePath.setAttribute("d", panCurvePath(pts));
    }

    // ④ 声像 / 音量分布图(scvb.params + scvb.state.channels)-----------------
    function renderDist(st, s) {
        if (!el.distBars) return;
        const vals = st.params.values || {};
        const v = (s.global && s.global.version_active) || 1;
        const chans = s.channels || [];
        const spans = [];
        const bars = [];
        const hi = local.chartHi;
        // 图例 hover 的联动位:0 = 淡出(与轨迹图 DIM_ALPHA 同档,CSS 侧落地)。
        const dim = (ch) => (hi && hi !== ch ? "0" : "1");
        for (const ch of connectedChannels(st.conn)) {
            // 只画已连接轨(slotState=2 ∧ heartbeatFresh,与 pill 同判据)——
            // 空闲轨无参数值,vol=0 会被 distGeometry 画成居中高「幽灵柱」
            // (设计稿绘制前滤掉 idle/srErr 轨;PR #52 bot 抓取)。
            const p = `v${v}_t${tt(ch)}_`;
            const cfg = chans[ch - 1] || {};
            const geo = distGeometry(
                num(vals[p + "pan"], 0),
                num(vals[p + "vol"], 0),
                num(vals[p + "width"], 100),
            );
            // [J75] B:柱体与 width 横线按轨着色。`--tc` 走**变量指向变量**
            // (`--tc: var(--track-color-7)`),色值本身仍只在 tokens.css 里定义一处。
            const tc = `--tc:var(${trackColorVar(ch)});`;
            if (cfg.source_channels === 2) {
                spans.push(
                    `<div class="dist-span" data-ch="${ch}" data-hi="${dim(ch)}" style="${tc}--x0:${(geo.x - geo.half).toFixed(2)}%;--w:${(geo.half * 2).toFixed(2)}%;--y:calc(18px + ${geo.h.toFixed(2)}%)"></div>`,
                );
            }
            bars.push(
                `<div class="dist-bar" data-lead="${cfg.lead_lock ? 1 : 0}" data-ch="${ch}" data-hi="${dim(ch)}" style="${tc}--x:${geo.x.toFixed(2)}%;--h:${geo.h.toFixed(2)}%"></div>`,
            );
        }
        const html = spans.concat(bars).join("");
        if (el.distBars.innerHTML !== html) el.distBars.innerHTML = html;
    }

    // ④b 双视图:视图态 + 轨迹图驱动 + 图例([J75] A/B)---------------------
    function renderChart(st, s, t) {
        // state 回推追上本地乐观值 ⇒ 交回 state 驱动(与 nextParamEcho 同一纪律)。
        if (local.chartMode && chartModeOf(s) === local.chartMode) {
            local.chartMode = null;
        }
        const mode = currentChartMode();
        if (el.distCard) {
            const was = el.distCard.getAttribute("data-chart-mode");
            if (was !== mode) {
                el.distCard.setAttribute("data-chart-mode", mode);
                local.trajDirty = true; // 刚露出来的画布要重量一次舞台
            }
        }
        if (el.chartSeg) {
            for (const btn of el.chartSeg.querySelectorAll(
                "[data-chart-mode]",
            )) {
                btn.setAttribute(
                    "aria-pressed",
                    String(btn.getAttribute("data-chart-mode") === mode),
                );
            }
        }

        renderLegend(
            legendRows(
                mode,
                connectedChannels(st.conn),
                local.trajSeries,
                s.channels,
            ),
            t,
        );

        if (!local.traj) return;
        if (mode !== "trajectory") {
            local.traj.suspend();
            return;
        }
        if (el.trajEmpty) el.trajEmpty.hidden = local.trajSeries.length > 0;
        // ui.scale 档位变化 = 后备存储 k 变(05 §6.1 `k = uiScale × dpr`)→ 标脏重建。
        // 与 Tab3 的 render 逐字同款(tab-wave.js 的 local.lastUiScale)。
        // 这一笔非记不可:CSS zoom 换档**不动** dpr(observeResolution 不响),也不动
        // 父盒的 CSS px 尺寸(ResizeObserver 不响)—— 没有任何既有信号会来敲门,
        // 而 backingScale() 只在 paintStatic 里算,不重绘就一直用着旧 k,画面持续糊。
        const uiScale = num((s.ui || {}).scale, 1);
        if (local.lastUiScale !== uiScale) {
            local.lastUiScale = uiScale;
            local.trajDirty = true;
        }
        // 刻度本身由 onPanAxis 推来时就渲染过了;这里再走一遍是为了**切语言** ——
        // applyI18n 只刷 data-t,而这一列是拼出来的(签名带语言,没变就是空跑)。
        renderTrajAxis(t);
        local.traj.setDuration(chartDurationS(st));
        // 切回本页/本视图时按需起帧(离场时 onPlayhead 停过插值循环);幂等且便宜。
        local.traj.resume();
        // 脏位只在**真的画得成**的那一帧清掉(判据见 chartRepaintNow 的头注):
        // 不可见时 invalidate() 早退不画,这时清了就等于把重绘诉求丢了。
        if (chartRepaintNow(local.trajDirty, isPanelActive())) {
            local.trajDirty = false;
            local.traj.invalidate();
        }
    }

    /**
     * y 刻度列。轨迹图推来 `{pan, y, side}`,方位词在这里按字典补上 ——
     * `side` 只会是 `R`/`C`/`L`(即 +100 / 0 / −100 三条锚刻度),纵向放大到看不见
     * 它们时本列就只剩数字;给别的刻度硬贴方位词等于写一个不成立的读数。
     *
     * 方位词**按 key 取**(`chart.panSideR/C/L`),不是拆一串按位置认。语序在
     * 三语里本就不同,再经 U17 审校一改,按位置取就会把左右标反 —— 而那是一眼
     * 看不出来的错:图照画,只是左右颠倒。key 取词条则怎么改都错不到别人头上。
     */
    function sideWord(t, side) {
        return (t && t["chart.panSide" + side]) || "";
    }

    function renderTrajAxis(t) {
        if (!el.trajAxisY) return;
        // 签名先算先比:本函数挂在每帧 render 上,刻度与语言都没动时应当一步不走。
        const sides = ["R", "C", "L"].map((s) => sideWord(t, s)).join(" ");
        const key =
            sides +
            "|" +
            local.trajAxisTicks
                .map((k) => `${k.pan}@${Math.round(k.y)}`)
                .join(",");
        if (key === local.trajAxisKey) return;
        local.trajAxisKey = key;
        el.trajAxisY.innerHTML = local.trajAxisTicks
            .map((k) => {
                const w = sideWord(t, k.side);
                return (
                    `<span class="traj-axis-y__tick${w ? " is-anchor" : ""}"` +
                    ` style="top:${k.y.toFixed(2)}px">` +
                    `${w ? esc(w) + " " : ""}${esc(panTickText(k.pan))}</span>`
                );
            })
            .join("");
    }

    /** 图例 = 色点 + 轨号 + 轨名(+ 立体声 ST 角标);hover 高亮由 data-hi 承载。 */
    function renderLegend(rows, t) {
        if (!el.chartLegend) return;
        const badge = t && t["stereoBadge"] ? t["stereoBadge"] : "ST";
        const hint = (t && t["chart.legendHint"]) || "";
        if (el.chartLegend.getAttribute("title") !== hint) {
            el.chartLegend.setAttribute("title", hint);
        }
        // 轨名是**用户数据**,一律转义再拼(与 Tab2/Tab3 同款纪律)。轨号始终在场 ——
        // 二色觉下非相邻两轨可能同色(tokens.css 第 21 组的实测口径),色点不是唯一线索。
        const html = rows
            .map(
                (r) =>
                    `<span class="chart-legend__item" role="listitem" data-legend-ch="${r.ch}" data-hi="${local.chartHi === r.ch ? 1 : 0}">` +
                    `<span class="chart-legend__dot" style="--tc:var(${trackColorVar(r.ch)})" aria-hidden="true"></span>` +
                    `${tt(r.ch)}${r.label ? " " + esc(r.label) : ""}` +
                    (r.stereo
                        ? `<span class="chart-legend__st">${esc(badge)}</span>`
                        : "") +
                    `</span>`,
            )
            .join("");
        if (el.chartLegend.innerHTML !== html) el.chartLegend.innerHTML = html;
    }

    // A1 空态卡(J72):未连接任何 Input 且无数据 → 显示;任一轨连接后消失 -------
    function renderEmptyState(st) {
        if (!el.emptyState) return;
        const connected = connectedCount(st.conn) > 0;
        const hasData = segmentTotals(st.segments).n > 0;
        el.emptyState.hidden = connected || hasData || !st.ready;
    }

    // ------------------------------------------------- 事件回调(app.js 调)
    /**
     * `scvb.params` 到达:让本地乐观值逐帧让位(规则见纯函数 nextParamEcho)。
     * 必须在 render() **之前**跑,否则本帧仍读到已作废的乐观值。
     */
    function onParams(payload) {
        local.paramEcho = nextParamEcho(
            local.paramEcho,
            payload,
            local.gesture,
        );
    }

    /** scvb.segments 到达:分析完成闪绿(data-analyze="done")。 */
    function onSegments(payload) {
        const token = payload && payload.reason;
        if (
            token === "analyze" ||
            token === "vad" ||
            token === "segmentation"
        ) {
            local.analyzePending = false; // 完成路径兜底清在途标志
            local.analyzeFlashUntil = now() + 1600;
            setTimeout(render, 1650);
        }
        // 段表变了 ⇒ 影响面预览的旧数字作废,下一拍重取
        local.preview = null;
        local.previewKey = "";
        // [J75] A:轨迹图的数据面就是段表 —— 在这里重投影一次,而不是每帧从 store 现算。
        // **必须读 store 而不是 payload**:§2.8 的增量帧只带受影响轨,直接拿 payload
        // 建线会把没变的轨整片抹掉(合并在 app.js 的 applySegmentsEvent 里已经做过了)。
        local.trajSeries = trajectorySeries(
            getStore().segments,
            (getStore().state || {}).channels,
        );
        local.trajDirty = true;
    }

    /**
     * `scvb.playhead`(30Hz)到达:喂给轨迹图的 rAF 插值层 + 跟随模式推进。
     *
     * 与 Tab3 同款分工 —— 竖线**不经** render() 逐帧投影(那是 rAF 合帧的整页渲染,
     * 30Hz 事件流会把它打满),而是直接进 `canvas/playhead.js` 的插值循环。
     */
    function onPlayhead(payload) {
        if (local.traj) local.traj.onPlayhead(payload);
    }

    /**
     * 影响预览节流刷新(契约 §1.5:纯只读 dry-run,毫秒级返回,UI 侧节流调用)。
     * scope 指纹没变就不重复问 —— previewAnalyze 虽便宜,但它是 [M] 同步调用。
     */
    function refreshPreview() {
        const scope = analyzeScope(getStore().state);
        const key = JSON.stringify(scope);
        if (key === local.previewKey) return;
        local.previewKey = key;
        if (local.previewTimer) clearTimeout(local.previewTimer);
        local.previewTimer = setTimeout(async () => {
            const res = await call("previewAnalyze", scope);
            if (res && Number.isFinite(res.intervals)) {
                local.preview = res;
                render();
            }
        }, 200);
    }

    return {
        mount,
        render,
        onParams,
        onSegments,
        onPlayhead,
        refreshPreview,
    };
}

// =============================================================================
// 三、小工具(DOM 相关,不导出)
// =============================================================================

/** HTML 文本转义 —— label 是**用户数据**,绝不许拼进 innerHTML 不转义(同 Tab2/Tab3 的 esc)。 */
function esc(s) {
    return String(s == null ? "" : s).replace(
        /[&<>"']/g,
        (c) =>
            ({
                "&": "&amp;",
                "<": "&lt;",
                ">": "&gt;",
                '"': "&quot;",
                "'": "&#39;",
            })[c],
    );
}

function makeCaller(bridge) {
    return async function call(name, ...args) {
        if (!bridge || typeof bridge[name] !== "function") return null;
        try {
            return await bridge[name](...args);
        } catch (e) {
            // 桥调用失败绝不打断渲染(浏览器直开 / mock 未注入时是常态)
            console.warn(`SCVB Tab1:bridge.${name}() 调用失败 —— ${e.message}`);
            return null;
        }
    };
}

function now() {
    return Date.now();
}

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

function popcount(mask) {
    let n = 0;
    let m = Number(mask) || 0;
    while (m) {
        n += m & 1;
        m >>>= 1;
    }
    return n;
}

/** 词条 + 占位符 → textContent(applyI18n 写的是整串,所以填充必须排在它之后)。 */
function fill(node, t, key, vals) {
    if (!node) return;
    if (!Object.prototype.hasOwnProperty.call(t, key)) return;
    node.textContent = format(t[key], vals);
}

/**
 * disabled 类 tooltip:词条为空(未立 key / 非 disabled 态)时**移除** title,
 * 不留一个空 title —— 空 title 在部分宿主 WebView 里仍会弹一个空气泡。
 */
function setTitle(node, text) {
    if (!node) return;
    if (text) node.setAttribute("title", text);
    else node.removeAttribute("title");
}

/** 开关类节点:点击 + 空格/回车(role="switch" + tabindex 的键盘可达)。 */
function onActivate(node, fn) {
    if (!node) return;
    node.addEventListener("click", fn);
    node.addEventListener("keydown", (e) => {
        if (e.key === " " || e.key === "Enter") {
            e.preventDefault();
            fn();
        }
    });
}
