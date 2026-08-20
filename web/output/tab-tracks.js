// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · Tab2「轨道」—— 15 轨行矩阵(T32 Wave 1 交付物:视觉)。
// -----------------------------------------------------------------------------
// 职责边界(与 tab-master.js 同构):
//   • 本文件只管 **Tab2**(列头 / 15 行 / 行内全部控件 / 图例脚注 / 行内确认层)。
//     外壳(header / 横幅 / footer / 缩放 / 引导页 / tab 路由)在 web/output/app.js;
//     Tab1 在 web/output/tab-master.js。app.js 只留一句装配调用。
//   • 两段导出:**纯函数**(无 DOM,node 可直接 import 断言)+ `createTabTracks()`
//     (DOM 接线)。模块顶层零副作用、零 document 触碰。
//
// 视觉真源 = 设计稿 `docs/design/SCVB 设计稿.dc.html`:
//   Tab2 DOM 561-648、行/列渲染值 1726-1770、列表 2059-2068、knob() 1711-1724、tog() 1512-1521。
// 交互语义与词条真源 = masterPlan/plan/05-ui-spec.md §2.2。
// 桥面 = 冻结契约 docs/SCVB_CONTRACT.md(§1.15-§1.19 每轨函数、§2.5 meters、§1.12-§1.14 gesture)。
//
// **列宽单一真源**:下面的 `TRACK_COLS` 同时喂列头与轨行(设计稿把两处宽度表分开手写,
// 靠同一组 px 值对齐 —— 移植时若各写一份,错位将无从排查)。全部布局是
// `display:flex` + `flex:none` + 固定 width,没有 grid / table 参与。
//
// 分波:
//   • **Wave 1** = 视觉。行五态 / on:0 / 空 label 占位 / 冻结两态 / 配对七色 /
//     mono 轨 width 灰显 …… 一律写成 `data-*`,可在 DevTools 改属性静态验证。
//   • **Wave 2(本次)** = 交互。**Wave 1 的 `WAVE1_SAMPLE` 静态填数路径已整段删除**
//     (同 T31 的 WAVE1_NUMBERS):15 行的每一个值都由事件算出 ——
//       `scvb.state.channels` → label/priority/lead_lock/lead_vol_exempt/
//                               participate_in_auto_pan/pair_id/enabled/ST;
//       `scvb.conn`           → 状态灯五态 / 失准计数 / 采样率错整行 disabled / 0 轨空态;
//       `scvb.params`         → 每轨 pan/vol/width/freeze 跟随 + hostEcho 灰显;
//       `scvb.meters`(30 Hz) → canvas/meter.js 的 rAF 弹道写 15 管液柱与峰线;
//       `scvb.segments`       → 冻结维度的读回值(常值段)与解冻提示判定。
//     上行:`setChannelConfig`(§1.15)/ `setTrackManual`(§1.16,含每轨首次无条件确认)/
//     gesture 三段式(§1.12-§1.14,width 与 freeze)/ `analyze(scope,{clearManual:true})`
//     (§1.6,单轨重新识别)。**不新增任何桥函数。**
// =============================================================================

// 弹道渲染器(契约 §2.5 的 30 Hz 事件 → rAF 补间);模块顶层零副作用,node 可直接 import。
import { createMeterRenderer } from "./canvas/meter.js";
// hostEcho 灰显的批次新鲜度窗口 —— 与 Tab1 **共用同一个常量**,不在这里写第二份数字;
// format = 词条 {x} 占位填充(labelPlaceholder 用;漏导入曾致空轨名行 ReferenceError,
// PR #60 红旗)。
import { HOST_ECHO_FRESH_MS, format } from "./tab-master.js";

// =============================================================================
// 一、纯函数与常量(无 DOM;node 侧断言面)
// =============================================================================

/** 通道数 —— 契约 §0.2 第 4 条:`ch` = 1..15(J59)。 */
export const CHANNEL_COUNT = 15;

/**
 * 旋钮拖拽的满程像素(垂直位移 → 全值域)。30px 见方的旋钮不可能按自身尺寸换算,
 * 取一个手感常量;与 Tab1 滑轨「按轨道宽换算」是两种控件的两种口径,不共用。
 */
export const KNOB_DRAG_PX = 150;

/**
 * 手动常值的**延迟提交**窗口(ms)。滚轮一格一格、方向键按住不放(OS 自动重复 ~30 Hz)
 * 都会连出一串值,而 `setTrackManual` 入撤销栈(契约 §0.9),逐次发会把宿主 UndoManager
 * 灌满 —— 故「回声即时、提交防抖」:停手 300 ms 才落一次。拖拽走的是松手提交,不用它。
 */
export const MANUAL_COMMIT_MS = 300;

/** 行高 44 设计 px(设计稿 1737 行 + `TAB_ROWS[2].rowH`;[J59] 密度实测按此口径复算)。 */
export const ROW_H = 44;
/** 列头行高 30px(设计稿 571)。 */
export const HEAD_H = 30;
/** 列间距 14px(设计稿 571/1737 同值,行与列头共用)。 */
export const COL_GAP = 14;
/** 行左右内边距 10px(设计稿 571/1737)。 */
export const ROW_PAD_X = 10;

/** 玻璃管宽 268px(设计稿 594;tokens.css `--t2-tube-w` 同值)。 */
export const TUBE_W = 268;
/**
 * 管右侧两枚附件的固定预留槽(A 组自由发挥件,设计稿未画):
 *   • 豁免角标(05 §2.2「ON 时推子旁『豁免』角标」)—— 34px;
 *   • 冻结生效版本 mono 小字(J65「行尾 mono 小字标注生效版本」)—— 16px。
 * **必须是固定槽而不是内容撑开**:两件都按轨显隐,若让它们参与撑宽,同一列会在
 * 「有角标的行」与「没角标的行」之间左右错开,列头也就对不上了。
 */
export const EXEMPT_SLOT_W = 34;
export const VERSION_SLOT_W = 16;

/** 「音量 / 电平」整列宽 = 管 + 两枚附件槽(设计稿的 268 只是管本身)。 */
export const VOL_COL_W =
    TUBE_W + 6 + EXEMPT_SLOT_W + 6 + VERSION_SLOT_W; /* = 330 */

/**
 * 列表(顺序不可重排,05 §2.2「列序与分组」代码块 = 真源;宽度取设计稿 2059-2064)。
 *   • `w`      1× 设计 px 列宽
 *   • `t`      列头词条 key(空串 = 无字列头,如状态灯列)
 *   • `tight`  26px 列塞 3 个汉字的字号例外(设计稿 2067:7.5px + 零字距 + 允许折行)
 *   • `divider` 分组竖分隔线(05 代码块里的三个 `‖`);设计稿只画冻结组左侧一条,
 *               统筹裁定 B15 取 05 的三条,并按灰模做成**实体 span**(列头/行成对出现,
 *               天然对齐;设计稿的绝对定位伪元素零布局宽度,列头得另算偏移)。
 * 冻结两列在轨行里合成一个 66px 的盒(26 + gap14 + 26),与列头两列 + 列间距逐像素等宽 ——
 * 那个盒必须是真实 inline-flex(不是 display:contents),否则 T36b spotlight 的
 * getBoundingClientRect() 恒为 0×0,tour 第 4 步没有亮区。
 */
export const TRACK_COLS = Object.freeze(
    [
        // 状态灯列无可见文字,列头给 aria(axe empty-table-header;P3-3)
        { key: "light", w: 8, t: "", aria: "tracks.colState" },
        { key: "ch", w: 44, t: "tracks.colCh" },
        { key: "label", w: 150, t: "track" },
        { key: "pan", w: 30, t: "tracks.colPan" },
        { key: "width", w: 30, t: "tracks.colW" },
        { key: "vol", w: VOL_COL_W, t: "tracks.colVolLevel" },
        { key: "prio", w: 66, t: "tracks.colPrio" },
        { key: "lead", w: 26, t: "tracks.colLead" },
        { key: "pair", w: 52, t: "pair" },
        { key: "div1", w: 1, divider: true },
        { key: "volexempt", w: 26, t: "tracks.colVolExempt" },
        { key: "autopan", w: 26, t: "tracks.colAutoPan" },
        { key: "div2", w: 1, divider: true },
        { key: "freezepan", w: 26, t: "tracks.colFreezePan", tight: true },
        { key: "freezevol", w: 26, t: "tracks.colFreezeVol", tight: true },
        { key: "div3", w: 1, divider: true },
        { key: "on", w: 26, t: "tracks.colOn" },
    ].map(Object.freeze),
);

/** 冻结两列合成盒的宽度(26 + 14 + 26,见 TRACK_COLS 注释)。 */
export const FREEZE_GROUP_W = 26 + COL_GAP + 26;

/**
 * 一行的总占宽(列宽和 + 列间距 + 左右内边距)。
 * 与 `TRACKS_VIEWPORT_W` 一起构成「1180 设计盒零横向溢出」的可断言口径([J59] 复核项)。
 */
export function rowTotalWidth() {
    const sum = TRACK_COLS.reduce((a, c) => a + c.w, 0);
    return sum + COL_GAP * (TRACK_COLS.length - 1) + ROW_PAD_X * 2;
}

/**
 * 轨道表可用宽度(1× 设计 px)。推导链,改任一环都要回改这里:
 *   1180(设计盒 DESIGN.output.w)− 36(#card 左右 padding 各 18)
 *   − 2(.tracks-wrap 左右 1px 描边)− 9(竖滚动条,base.css `*::-webkit-scrollbar`)= 1133。
 * [J59] 统筹 Chrome 实测同为 1133 —— 两者对上才说明推导没漏项。
 */
export const TRACKS_VIEWPORT_W = 1133;

/** pan 旋钮角度(设计稿 1712:val/100×140;pan ∈ -100..100 → -140°..+140°,0 = 正上)。 */
export function panAngleDeg(pan) {
    return (num(pan, 0) / 100) * 140;
}

/**
 * width 旋钮角度(设计稿 1732:`knob((w-50)*2, …)`)。
 * w ∈ 0..100(源张开度)→ -140°..+140°;w=50 指针朝上、w=100 最右。
 */
export function widthAngleDeg(widthPct) {
    return (((num(widthPct, 100) - 50) * 2) / 100) * 140;
}

/** vol 推子行程百分比:契约 vol ∈ -24..+12 dB(36 dB 跨度)→ 0..100%(J03:0 dB 落 2/3)。 */
export function volPercent(db) {
    return clamp01((num(db, -24) + 24) / 36) * 100;
}

/** 电平百分比:契约 §2.5 地板 -60 dB → 0..100%。 */
export function meterPercent(db) {
    return clamp01((num(db, -60) + 60) / 60) * 100;
}

/** 峰值线警戒阈:05 §2.2「peak > .86 转警戒红」(设计稿 1752 同值)。 */
export const PEAK_ALERT_RATIO = 0.86;

/**
 * 行不透明度(设计稿 1730 的判定链,顺序不可换):idle → off → dead → 正常。
 * idle 优先于 off:离线轨即使 ON 关掉也只降到 .5(配置仍可改,ADR-004)。
 * CSS 侧用等权重属性选择器实现,故三条规则必须按 srErr → off → idle 的顺序书写
 * (后写者赢);本函数是那三条 CSS 的可断言镜像。
 */
export function rowDim(status, on) {
    if (status === "idle") return 0.5;
    if (!on) return 0.3;
    if (status === "srErr") return 0.45;
    return 1;
}

/**
 * 状态灯映射(设计稿 1703-1709 五态)。`key` 是 tooltip 词条;`warn` 的计数由调用方填 {n}。
 * 05 §2.2 状态灯行:绿脉冲=活跃 / 灰=未连接 / 琥珀=失联 / 琥珀=时间线失准 N 次 / 红=采样率不一致。
 */
export function statusVisual(status) {
    switch (status) {
        case "active":
            return { tone: "green", pulse: true, key: "state.connected" };
        case "lost":
            return { tone: "amber", pulse: false, key: "state.staleLink" };
        case "warn":
            return { tone: "amber", pulse: false, key: "tracks.misaligned" };
        case "srErr":
            return { tone: "red", pulse: false, key: "tracks.srErr" };
        default:
            return { tone: "gray", pulse: false, key: "state.notConnected" };
    }
}

/** 契约 §1.15:pair_id = 0(无)| 1..7 → UI 字母 A–G。 */
export const PAIR_LETTERS = Object.freeze(["A", "B", "C", "D", "E", "F", "G"]);

export function pairLetter(pairId) {
    const i = Math.trunc(num(pairId, 0));
    return i >= 1 && i <= PAIR_LETTERS.length ? PAIR_LETTERS[i - 1] : "";
}

/** 字母 → pair_id(设计稿/词条用字母 A–G,契约 §1.15 用整数 0|1..7;`pairLetter` 的逆)。 */
export function pairIdOf(letter) {
    const i = PAIR_LETTERS.indexOf(String(letter || ""));
    return i < 0 ? 0 : i + 1;
}

/**
 * 每轨 freeze 参数 `v{active}_t{ch:02d}_freeze`(契约 §1.12-§1.14:int 0-3,
 * bit0=pan / bit1=vol,两枚开关各改一位)。
 */
export function freezeBits(freeze) {
    const f = Math.trunc(num(freeze, 0)) & 3;
    return { pan: (f & 1) === 1, vol: (f & 2) === 2 };
}

export function freezeValue(panFrozen, volFrozen) {
    return (panFrozen ? 1 : 0) | (volFrozen ? 2 : 0);
}

/** 优先级钳制(05 §2.2:`− n +` stepper,0..10;统筹裁定 B17)。 */
export const PRIORITY_MIN = 0;
export const PRIORITY_MAX = 10;

export function clampPriority(v) {
    const n = Math.round(num(v, 5));
    return n < PRIORITY_MIN
        ? PRIORITY_MIN
        : n > PRIORITY_MAX
          ? PRIORITY_MAX
          : n;
}

/** Label 长度上限(05 §2.2:点击变行内 input,≤24 字符)。 */
export const LABEL_MAX = 24;

/**
 * 空 label 的淡色占位(设计稿 1734 为硬编码 "Track NN";用户可见文案须有 key
 * ——查字典 `tracks.labelPlaceholder`({n} 占位),字典缺失时回落设计稿原文。
 */
export function labelPlaceholder(ch, t) {
    const tpl = t && t["tracks.labelPlaceholder"];
    return tpl ? format(tpl, { n: tt(ch) }) : "Track " + tt(ch);
}

/**
 * 图例长句 → 三段 `{term, rest}`(设计稿 626 行的三处 `<strong>` 高亮)。
 * 切法:先按分隔符 `·` 切段,每段再按首个等号(半角 `=` 或全角 `＝`)切出被强调的词。
 * 三语的 `tracks.colLegend` 都是「术语 = 释义」×3 结构,故此切法三语通用;
 * 任一段切不出等号就整段当普通正文(不强行加粗),渲染永不丢字。
 */
export function legendSegments(text) {
    const raw = String(text || "");
    if (!raw) return [];
    return raw
        .split(/\s*·\s*/)
        .filter((s) => s !== "")
        .map((seg) => {
            const m = /^([^=＝]{1,24})([=＝])([\s\S]*)$/.exec(seg);
            if (!m) return { term: "", rest: seg };
            return { term: m[1].trim(), rest: m[2] + m[3] };
        });
}

/** 两位零填充轨号(契约 §1.12-§1.14:t = "01".."15")。 */
export function tt(ch) {
    return String(ch).padStart(2, "0");
}

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

function clamp01(v) {
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

// -----------------------------------------------------------------------------
// Wave 2 纯函数:事件仓 → 15 行模型(无 DOM;node 侧可直接断言)
// -----------------------------------------------------------------------------
// **Wave 1 的 WAVE1_SAMPLE 静态填数路径已整段删除**(同 T31 的 WAVE1_NUMBERS)。
// 行模型的每一个字段都必须能指回一个契约字段,否则它就是又一份「设计稿抄来的数」:
//   label/priority/lead_lock/lead_vol_exempt/participate_in_auto_pan/pair_id/
//   enabled/source_channels ← §2.1 `scvb.state.channels[]`
//   pan/vol/width/freeze                                ← §2.2 `scvb.params.values`
//   状态灯五态 / 失准计数 / 整行 disabled                 ← §2.3 `scvb.conn.channels[]`
//   液柱 lv / 峰线 pk                                    ← §2.5 `scvb.meters`(经 meter.js 弹道)
//   冻结维度的读回值 / 解冻提示判定                        ← §2.8 `scvb.segments`(常值段)

/** 每轨 ParamID(契约 §1.12-§1.14:`v{v}_t{t:02d}_{knob}`,t 两位零填充)。 */
export function paramIdOf(versionActive, ch, knob) {
    return `v${Math.trunc(num(versionActive, 1))}_t${tt(ch)}_${knob}`;
}

/**
 * 三个可拖控件的值域与默认值。
 *   • vol —— 契约 §1.16 的 `value` 域(-24..+12 dB);双击回 0 dB、键盘步进 0.1 dB(05 §2.2);
 *   • pan —— 契约 §1.16 的 `value` 域(-100..+100);双击回 0;
 *   • width —— 契约 §1.12-§1.14 每轨 `width`(0..100 %,默认 100);双击回 100。
 * `fine` = Shift 微调倍率(05 §2.2 拖拽「shift 微调」);`coarse` = 键盘 Shift 加速倍率。
 */
export const VOL_RANGE = Object.freeze({
    min: -24,
    max: 12,
    def: 0,
    step: 0.1,
    fine: 0.2,
    coarse: 10,
});
export const PAN_RANGE = Object.freeze({
    min: -100,
    max: 100,
    def: 0,
    step: 1,
    fine: 0.2,
    coarse: 10,
});
export const WIDTH_RANGE = Object.freeze({
    min: 0,
    max: 100,
    def: 100,
    step: 1,
    fine: 0.2,
    coarse: 10,
});

export function clampRange(rng, v) {
    const n = num(Number(v), rng.def);
    return n < rng.min ? rng.min : n > rng.max ? rng.max : n;
}

/** 步进量化到 `step`(0.1 dB 的浮点尾巴会让 aria-valuenow 出现 -3.4000000000000004)。 */
export function quantize(rng, v) {
    const q = Math.round(clampRange(rng, v) / rng.step) * rng.step;
    return Math.round(q * 1000) / 1000;
}

/**
 * 状态灯五态(05 §2.2 状态灯行;数据源 = 契约 §2.3 `scvb.conn.channels[]`)。
 * 判定顺序不可换:采样率错(整行 disabled)→ slot 未活跃(未连接)→ 心跳陈旧(失联)
 * → 失准计数(琥珀⚠)→ 活跃。
 *
 * 「未连接」的判据是 **`slotState !== 2`**,不是 `=== 0`:ipc §1 的 slotState=1 是
 * 「已声明、尚未活跃」,它**不计入 header 的 N/15**(契约 §2.3 UI 消费行,J01)。
 * 只排除 0 的话,已声明但未活跃的 slot 会在行上亮绿脉冲却不进 N/15,两处口径打架。
 */
export function trackStatusOf(c) {
    if (!c) return "idle";
    if (c.srMismatch) return "srErr";
    if (c.slotState !== 2) return "idle";
    if (!c.heartbeatFresh) return "lost";
    if (num(c.misalignCount, 0) > 0) return "warn";
    return "active";
}

/** 配对占用计数:下标 = pair_id(1..7),值 = 已选该组的轨数(契约 §1.15)。 */
export function pairCounts(channels) {
    const counts = new Array(PAIR_LETTERS.length + 1).fill(0);
    for (const cfg of channels || []) {
        const p = Math.trunc(num(cfg && cfg.pair_id, 0));
        if (p >= 1 && p <= PAIR_LETTERS.length) counts[p] += 1;
    }
    return counts;
}

/**
 * 下拉选项的「(满)」后缀判据(05 §2.2:某组已满 2 轨时后缀「(满)」**仍可选**)。
 * 本轨已在该组时把自己减掉 —— 否则自己那一格永远显示「(满)」。
 */
export function isPairFullOption(counts, pairId, selfPairId) {
    if (!pairId) return false;
    const used = (counts && counts[pairId]) || 0;
    return used - (selfPairId === pairId ? 1 : 0) >= 2;
}

/** 行上的琥珀「配对超员」标(05 §2.2):该组实际超过 2 轨才亮。 */
export function isPairOverflow(counts, pairId) {
    if (!pairId) return false;
    return ((counts && counts[pairId]) || 0) > 2;
}

/** `lead_lock` 计数 ≥2 ⇒ 图例行琥珀「多主唱居中」badge(05 §2.2 主唱锁行)。 */
export function leadLockCount(channels) {
    let n = 0;
    for (const cfg of channels || []) if (cfg && cfg.lead_lock) n += 1;
    return n;
}

/** 从合并后的段表视图里取某轨(契约 §2.8:`channels` 只含受影响轨)。 */
export function segmentsOfCh(segments, ch) {
    const list = (segments && segments.channels) || [];
    for (const c of list) if (c && c.ch === ch) return c;
    return null;
}

/**
 * 「单段全时限 `user_edited` 常值」判定 —— `setTrackManual` 的产物特征
 * (契约 §1.16 编码 = 04 §1.5 方案 A)。两处用它:
 *   ① 冻结维度的**读回值**(05 §2.2「读回值同样取自该段」);
 *   ② 解冻提示(该位 1→0 且该轨仍由手动常值驱动)。
 * 命中返回该段本身(调用方要读 pan/volDb),否则 null。
 */
export function manualConstantOf(segChannel) {
    const segs = (segChannel && segChannel.segments) || [];
    if (segs.length !== 1) return null;
    const s = segs[0];
    return s && s.origin === "user_edited" ? s : null;
}

/**
 * 解冻提示的**位账**(纯函数,node 侧可直接断言;05 §2.2 R2)。
 * `cur` = 该轨已记下的触发位(bit0=pan / bit1=vol),`prev`→`next` = 本次 freeze 变化。
 * 某位 **1→0** 记上(触发提示),某位 **0→1** 抹掉(触发条件已不成立);返回 0 = 撤下提示。
 *
 * 05 字面只定义了触发、没定义消除。若只在「双位全冻」时才撤,只用 vol 一个维度的轨
 * 解冻(2→0)后再冻回(0→2)会留一条陈旧提示 —— 恢复原状态却还挂着提示,是错的。
 */
export function unfreezeHintBits(cur, prev, next) {
    const p = Math.trunc(num(prev, 0)) & 3;
    const n = Math.trunc(num(next, 0)) & 3;
    return ((Math.trunc(num(cur, 0)) & 3) | (p & ~n)) & ~(n & ~p) & 3;
}

/** 该轨的 locked 段计数(确认条 `.locked` 变体的 {l};契约 §1.16 会连 locked 一并替换)。 */
export function lockedCountOf(segments, ch) {
    const c = segmentsOfCh(segments, ch);
    const segs = (c && c.segments) || [];
    let n = 0;
    for (const s of segs) if (s && s.locked) n += 1;
    return n;
}

/** 版本显示名(契约 §2.1 `versions[].name`;空名回落 "V{v}",与 header 同口径)。 */
export function versionLabel(state, v) {
    const entry = ((state && state.versions) || [])[v - 1];
    return entry && entry.name ? entry.name : "V" + v;
}

/**
 * 事件仓 → 15 行模型(**Tab2 的唯一渲染源**)。
 * 字段名沿用 Wave 1 模板的短名(`trackRowHtml` 不动):
 * st=stereo 标 / vol=卡箍行程比 0..1 / lv·pk=液柱与峰线行程比(由 meter.js 逐帧覆写) /
 * ex=lead_vol_exempt / part=participate_in_auto_pan / fp·fv=冻结两位 / low=样本不足。
 */
export function rowsFromStore(store) {
    const st = store || {};
    const state = st.state || {};
    const chans = state.channels || [];
    const conn = (st.conn && st.conn.channels) || [];
    const vals = (st.params && st.params.values) || {};
    const active =
        (state.global && state.global.version_active) ||
        (st.params && st.params.versionActive) ||
        1;
    const counts = pairCounts(chans);
    const multiLead = leadLockCount(chans) >= 2 ? 1 : 0;
    const leadSel = Math.trunc(num(vals.lead_select, 0));
    const rec = state.recapture || null;
    const recMask = rec && rec.armed ? Math.trunc(num(rec.tracksMask, 0)) : 0;
    // §2.9 `lowSample` 是轨级 error(载荷带 ch);app.js 的 store.errors 按 code 存一条
    const lowErr =
        st.errors && typeof st.errors.get === "function"
            ? st.errors.get("lowSample")
            : null;
    const ver = versionLabel(state, active);

    const rows = [];
    for (let ch = 1; ch <= CHANNEL_COUNT; ch++) {
        const cfg = chans[ch - 1] || {};
        const cc = conn[ch - 1] || null;
        const freeze = Math.trunc(
            num(vals[paramIdOf(active, ch, "freeze")], 0),
        );
        const bits = freezeBits(freeze);
        const seg = manualConstantOf(segmentsOfCh(st.segments, ch));
        // 读回值:该轨曲线是**手动常值段**时一律取该段(05 §2.2「读回值同样取自该段」),
        // 否则取参数面(引擎打印头 / 宿主回吐)。
        // **不按 freeze 位分叉**:常值段既是「冻结时手动/host 权威的那个值」,也是
        // 「未冻结时引擎从曲线上取到的那个值」—— 两条路径的正确答案是同一个数。
        // 按 freeze 位分叉的写法会让「未冻结轨拖卡箍」(05 明确允许,走一次性确认)
        // 在下一帧把把手弹回去:`setTrackManual` 写的是曲线真身,不写参数面(§1.16),
        // 25 Hz 的 `scvb.params` 只在 PRINT 态才有新值。
        const pan = seg
            ? num(seg.pan, PAN_RANGE.def)
            : num(vals[paramIdOf(active, ch, "pan")], PAN_RANGE.def);
        const volDb = seg
            ? num(seg.volDb, VOL_RANGE.def)
            : num(vals[paramIdOf(active, ch, "vol")], VOL_RANGE.def);
        const pair = Math.trunc(num(cfg.pair_id, 0));
        rows.push({
            n: ch,
            label: typeof cfg.label === "string" ? cfg.label : "",
            st: cfg.source_channels === 2 ? 1 : 0,
            status: trackStatusOf(cc),
            pan,
            volDb,
            w: num(vals[paramIdOf(active, ch, "width")], WIDTH_RANGE.def),
            vol: volPercent(volDb) / 100,
            lv: 0,
            pk: 0,
            prio: clampPriority(num(cfg.priority, 5)),
            lead: cfg.lead_lock ? 1 : 0,
            ex: cfg.lead_vol_exempt ? 1 : 0,
            part: cfg.participate_in_auto_pan ? 1 : 0,
            pair,
            fp: bits.pan ? 1 : 0,
            fv: bits.vol ? 1 : 0,
            on: cfg.enabled === false ? 0 : 1,
            low: lowErr && lowErr.ch === ch ? 1 : 0,
            misalign: cc ? Math.trunc(num(cc.misalignCount, 0)) : 0,
            // 16px 槽只放短号「V{n}」,版本全名(可长至 16 字)进 tooltip ——
            // 塞全名会在槽内竖排(统筹亲验 2026-08-20)
            version: "V" + active,
            versionName: ver,
            leadCenter: leadSel === ch ? 1 : 0,
            pairFull: isPairOverflow(counts, pair) ? 1 : 0,
            recapture: recMask & (1 << (ch - 1)) ? 1 : 0,
            multiLead,
            manualConst: seg ? 1 : 0,
        });
    }
    return rows;
}

/** 首帧(store 尚空)用的 15 行骨架 —— mount 只需要结构,值由 render 逐帧回写。 */
export function emptyRows() {
    return rowsFromStore({});
}

// =============================================================================
// 二、模板(纯字符串拼装;值全部来自上面的纯函数,不在这里算几何)
// =============================================================================

/** HTML 文本转义 —— label 是**用户数据**,绝不许拼进 innerHTML 不转义。 */
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

/** 列头行(设计稿 571-575 + 2066-2068);宽度与轨行共用 TRACK_COLS。 */
export function trackHeadHtml() {
    return TRACK_COLS.map((c) => {
        if (c.divider) {
            return `<span class="sc-divider" aria-hidden="true"></span>`;
        }
        const t = c.t ? ` data-t="${c.t}"` : "";
        const tight = c.tight ? ` data-tight="1"` : "";
        // 无字列头给 sr-only 实文本(axe empty-table-header 认可视文本/隐藏文本,
        // 纯 aria-label 仍报 minor);词条随 data-t 走三语。
        const srText = c.aria
            ? `<span class="sr-only" data-t="${c.aria}">状态</span>`
            : "";
        return `<span class="tracks-head__col" role="columnheader" style="width:${c.w}px"${tight}${t} data-gb="tracks-head-${c.key}">${srText}</span>`;
    }).join("");
}

const W = Object.fromEntries(TRACK_COLS.map((c) => [c.key, c.w]));

/**
 * 一条轨行(设计稿 578-621)。
 * 全部状态写成 data-*:行 data-status / data-on / data-lead / data-dead,
 * 控件 data-on / data-live / data-disabled / data-frozen / data-alert / data-set / data-pair。
 * 实时几何走内联 CSS 变量:--ang(旋钮角) / --lv(液柱) / --pk(峰线) / --vol(卡箍位)。
 */
export function trackRowHtml(t) {
    const ch = t.n;
    const dead = t.status === "srErr" ? 1 : 0; // srErr:全行控件 disabled(设计稿 1729)
    const gb = (suffix) => `tracks-row-${ch}-${suffix}`;
    // pan 旋钮:**未冻结 = live**(引擎驱动、只读表盘),冻结才解锁为手动(设计稿 1731)
    const panLive = t.fp ? 0 : 1;
    // width 旋钮:`live` 恒 false —— 它直写自动化参数,不受冻结位管辖(设计稿 1732);
    // mono 轨(st=0)灰显 + tooltip(05 §2.2「mono 源:宽度在 v1 无效」)
    const wDis = dead || !t.st ? 1 : 0;
    const dis = dead ? ` data-disabled="1"` : "";
    // 状态灯五态直接写进模板(不等 render()):无 JS 的静态页面也能逐行验收
    const vis = statusVisual(t.status);
    const sw = (on) =>
        ` role="switch" aria-checked="${on ? "true" : "false"}" tabindex="0"`;

    return `
    <div class="tracks-row" role="row" data-glow="1" data-gb="tracks-row-${ch}" data-ch="${ch}"
         data-status="${t.status}" data-on="${t.on}" data-lead="${t.lead}" data-dead="${dead}"
         data-confirm="0">
      <span class="tracks-row__cell" role="cell" style="width:${W.light}px">
        <span class="sc-dot tracks-row__light" data-tone="${vis.tone}" data-pulse="${vis.pulse ? 1 : 0}" data-gb="${gb("statuslight")}"></span>
      </span>
      <span class="tracks-row__cell tracks-row__ch" role="cell" style="width:${W.ch}px" data-gb="${gb("ch")}">
        <span class="tracks-row__chnum">${tt(ch)}</span>
        <span class="sc-badge" data-t="stereoBadge" data-gb="${gb("stereo")}"${t.st ? "" : " hidden"}>ST</span>
      </span>
      <span class="tracks-row__cell tracks-row__labelcell" role="cell" style="width:${W.label}px"
            data-editing="0" data-gb="${gb("labelcell")}">
        <span class="tracks-row__label" data-placeholder="${t.label ? 0 : 1}" data-gb="${gb("label")}">${esc(t.label || labelPlaceholder(ch))}</span>
        <!-- 05 §2.2 Label 行:点击变行内 input(≤${LABEL_MAX} 字符),Enter/失焦提交。
             提交 → bridge.setChannelConfig(${ch}, {label})(契约 §1.15;按码点截断)。 -->
        <input class="tracks-row__label-input" type="text" maxlength="${LABEL_MAX * 2}"
               data-t-aria="tracks.labelEdit" data-gb="${gb("label-input")}" />
        <!-- Lead Select≠0 选中轨的行首居中标记(05 §2.2 主唱锁行 → §2.1 ④) -->
        <span class="tracks-row__leadmark" data-t="tracks.leadCenter" data-gb="${gb("leadcenter")}"${t.leadCenter ? "" : " hidden"}></span>
        <!-- 采集后有效唱段 <1.5s(05 §2.2 R1):角标保短版,全句「样本不足,结果可能不稳」进 tooltip(统筹裁定 B12) -->
        <span class="sc-badge--amber tracks-row__mark" data-t="lowSample" data-gb="${gb("lowsample")}"${t.low ? "" : " hidden"}></span>
        <!-- 重采集布防目标轨的行首 badge(05 §2.3 ②):52px 内塞不下长句,压成琥珀点 + tooltip -->
        <span class="tracks-row__dotmark" data-gb="${gb("recapture-badge")}"${t.recapture ? "" : " hidden"}></span>
      </span>
      <span class="tracks-row__cell" role="cell" style="width:${W.pan}px">
        <!-- 冻结态:垂直拖拽 / 滚轮 ±1 / 双击回 0 → bridge.setTrackManual(${ch}, "pan", v)(契约 §1.16;
             每轨首次操作前无条件弹行内确认,05 §2.2 R3);未冻结态 = 只读实时表盘 + tooltip。
             role="slider" + tabindex 是键盘可达面(未冻结时 aria-disabled,不摘 tabindex —— 摘了
             屏幕阅读器就读不到「自动模式」这条 tooltip)。 -->
        <span class="sc-knob" data-live="${panLive}" data-disabled="${dead}"
              role="slider" tabindex="0" aria-valuemin="${PAN_RANGE.min}" aria-valuemax="${PAN_RANGE.max}"
              aria-valuenow="${Math.round(num(t.pan, 0))}" data-t-aria="tracks.colPan"
              style="--ang:${panAngleDeg(t.pan)}deg" data-gb="${gb("pan")}"><span class="sc-knob__needle"></span></span>
      </span>
      <span class="tracks-row__cell" role="cell" style="width:${W.width}px">
        <!-- bridge.beginParamGesture / setParam / endParamGesture(v{active}_t${tt(ch)}_width)
             —— 契约 §1.12-§1.14(双击回默认 100);mono 轨 v1 no-op(灰显 + tooltip)。 -->
        <span class="sc-knob" data-live="0" data-disabled="${wDis}"
              role="slider" tabindex="0" aria-valuemin="${WIDTH_RANGE.min}" aria-valuemax="${WIDTH_RANGE.max}"
              aria-valuenow="${Math.round(num(t.w, 100))}" data-t-aria="tracks.colW"
              style="--ang:${widthAngleDeg(t.w)}deg" data-gb="${gb("width")}"><span class="sc-knob__needle"></span></span>
      </span>
      <span class="tracks-row__cell tracks-row__voltube" role="cell" style="width:${W.vol}px" data-gb="${gb("voltube")}">
        <span class="sc-tube" data-gb="${gb("vol-tube")}">
          <span class="sc-tube__slot">
            <span class="sc-tube__liquid" style="--lv:${t.lv * 100}%"></span>
            <span class="sc-tube__peak" style="--pk:${t.pk * 100}%" data-alert="${t.pk > PEAK_ALERT_RATIO ? 1 : 0}"${t.pk ? "" : " hidden"}></span>
          </span>
          <span class="sc-tube__gloss"></span>
          <!-- 卡箍 = 音量推子把手,层序在液柱之上(满幅电平仍可辨,T32 验收硬要求②);
               拖拽命中区 = 卡箍本体(管体不接受拖拽,05 §2.2),透明命中扩展见 index.html(RE-06)。
               水平拖拽 / 键盘 ±0.1 dB(Shift 加速)/ 双击回 0 dB
               → bridge.setTrackManual(${ch}, "vol", v) —— 契约 §1.16。 -->
          <span class="sc-tube__collar" style="--vol:${t.vol * 100}%" data-frozen="${t.fv}" data-disabled="${dead}"
                role="slider" tabindex="0" aria-valuemin="${VOL_RANGE.min}" aria-valuemax="${VOL_RANGE.max}"
                aria-valuenow="${num(t.volDb, 0)}" data-t-aria="tracks.colVolLevel"
                data-gb="${gb("vol-collar")}"></span>
        </span>
        <span class="sc-badge--amber tracks-row__exempt" data-t="tracks.exemptBadge" data-gb="${gb("volexempt-badge")}"${t.ex ? "" : " hidden"}></span>
        <span class="tracks-row__version" data-gb="${gb("freeze-version")}"${t.fv ? "" : " hidden"}>${esc(t.version)}</span>
      </span>
      <span class="tracks-row__cell sc-stepper tracks-row__prio" role="cell" style="width:${W.prio}px" data-gb="${gb("priority")}">
        <button type="button" data-t-aria="common.decrease" data-gb="${gb("priority-dec")}"${dis}>−</button>
        <span class="sc-stepper__val" data-gb="${gb("priority-val")}">${clampPriority(t.prio)}</span>
        <button type="button" data-t-aria="common.increase" data-gb="${gb("priority-inc")}"${dis}>+</button>
        <!-- ± 步进 → bridge.setChannelConfig(${ch}, {priority})(契约 §1.15;0..10 钳制,到头不发) -->
      </span>
      <span class="tracks-row__cell" role="cell" style="width:${W.lead}px">
        <span class="sc-toggle" data-on="${t.lead}" data-disabled="${dead}"${sw(t.lead)}
              data-t-aria="leadLock" data-gb="${gb("leadlock")}"></span>
      </span>
      <span class="tracks-row__cell" role="cell" style="width:${W.pair}px">
        <!-- 配对:05 要「下拉:无 / A–G」,设计稿画的是只读 chip —— 保留原生 select 语义,
             视觉压成设计稿那枚 52px chip(select 透明叠在 chip 之上,统筹裁定 B16)。 -->
        <span class="tracks-row__pair" data-set="${t.pair ? 1 : 0}" data-pair="${t.pair}" data-gb="${gb("pair-chip")}">
          <span class="tracks-row__pair-dot"></span>
          <span class="tracks-row__pair-text" data-gb="${gb("pair-text")}">${esc(pairLetter(t.pair))}</span>
          <!-- 自定义下拉(用户反馈 2026-08-20:原生 select 弹层不可样式化)——
               触发钮透明覆盖 chip(命中区 RE-06 不变),面板复用 Tab1 主唱选择
               的深色玻璃配方;第 11 轨起向上展开(免被滚动容器裁掉)。 -->
          <button type="button" class="tracks-row__pair-trigger" data-t-aria="pair"
                  aria-haspopup="listbox" aria-expanded="false"
                  data-gb="${gb("pair")}"${dead ? " disabled" : ""}></button>
          <span class="tracks-row__pair-panel" role="listbox" data-t-aria="pair"
                data-open="0"${ch > 10 ? ' data-up="1"' : ""} data-gb="${gb("pair-panel")}">
            ${[0, 1, 2, 3, 4, 5, 6, 7]
                .map(
                    (
                        v,
                    ) => `<button type="button" class="tracks-row__pair-opt" role="option"
                     aria-selected="false" data-pair="${v}" data-current="0"
                     data-gb="${gb(`pair-opt-${v}`)}">${v ? `<span class="tracks-row__pair-dot"></span><span class="tracks-row__pair-optlabel">${PAIR_LETTERS[v - 1]}</span>` : `<span class="tracks-row__pair-optlabel" data-gb="${gb("pair-opt-none-label")}"></span>`}</button>`,
                )
                .join("")}
          </span>
          <!-- 某组已满 2 轨仍可选,行上出琥珀标「配对超员」(05 §2.2);52px 内改用角点 + tooltip -->
          <span class="tracks-row__pair-full" data-gb="${gb("pair-overflow")}"${t.pairFull ? "" : " hidden"}></span>
        </span>
        <!-- 选中 → bridge.setChannelConfig(${ch}, {pair_id})(契约 §1.15;0 | 1..7) -->
      </span>
      <span class="sc-divider" aria-hidden="true"></span>
      <span class="tracks-row__cell" role="cell" style="width:${W.volexempt}px">
        <span class="sc-toggle" data-on="${t.ex}" data-disabled="${dead}"${sw(t.ex)}
              data-t-aria="leadVolExempt" data-gb="${gb("volexempt")}"></span>
      </span>
      <span class="tracks-row__cell" role="cell" style="width:${W.autopan}px">
        <span class="sc-toggle" data-on="${t.part}" data-disabled="${dead}"${sw(t.part)}
              data-t-aria="participateAutoPan" data-gb="${gb("autopan")}"></span>
      </span>
      <span class="sc-divider" aria-hidden="true"></span>
      <span class="tracks-row__cell" role="cell" style="width:${FREEZE_GROUP_W}px">
        <!-- 冻结组:两枚开关共写一个每轨自动化参数 v{active}_t${tt(ch)}_freeze
             (int 0-3,bit0=pan / bit1=vol,05 §2.2 / 契约 §1.12-§1.14)。
             data-tour="freeze" 只挂第 1 行(05 §2.6 步骤5,pad 14),且必须挂在有 principal box
             的真实盒上 —— display:contents 的元素 getBoundingClientRect() 恒为 0×0。
             两枚开关各改一位 → beginParamGesture / setParam / endParamGesture 一次性三段式。 -->
        <span class="tracks-row__freezegroup"${ch === 1 ? ' data-tour="freeze"' : ""}>
          <span class="sc-toggle" data-on="${t.fp}" data-disabled="${dead}"${sw(t.fp)}
                data-t-aria="tracks.colFreezePan" data-gb="${gb("freezepan")}"></span>
          <span class="sc-toggle" data-on="${t.fv}" data-disabled="${dead}"${sw(t.fv)}
                data-t-aria="tracks.colFreezeVol" data-gb="${gb("freezevol")}"></span>
        </span>
      </span>
      <span class="sc-divider" aria-hidden="true"></span>
      <span class="tracks-row__cell" role="cell" style="width:${W.on}px">
        <span class="sc-toggle" data-on="${t.on}" data-disabled="${dead}"${sw(t.on)}
              data-t-aria="tracks.colOn" data-gb="${gb("enable")}"></span>
        <!-- 切换 → bridge.setChannelConfig(${ch}, {enabled})(契约 §1.15) -->
      </span>
      <!-- R3 防误伤(05 §2.2 / §1.4 setTrackManual):每轨**首次**手动拖 pan 旋钮 / vol 卡箍前
           就地展开一次性行内确认(每轨每会话一次,无条件 —— 纯 user_edited 轨同样弹);
           存在 locked 段时正文追加 tracks.manualOverwriteConfirm.locked「(含 {l} 个锁定段)」。
           位置以 05 为准取**行内**(设计稿 633-639 画成页尾整条),视觉配方照抄设计稿。 -->
      <div class="sc-confirm tracks-row__confirm" data-gb="${gb("manual-overwrite-confirm")}" hidden>
        <span class="tracks-row__confirm-text" data-t="tracks.manualOverwriteConfirm"></span>
        <span data-t="tracks.manualOverwriteConfirm.locked" data-gb="${gb("manual-overwrite-confirm-locked")}" hidden></span>
        <button class="sc-btn" data-gb="${gb("manual-overwrite-cancel")}" data-t="common.cancel"></button>
        <button class="sc-btn sc-btn--cta" data-gb="${gb("manual-overwrite-ok")}" data-t="common.continue"></button>
      </div>
      <!-- R2 语义保留(05 §2.2 冻结行):解冻(该位 1→0)且该轨当前版本曲线为「单段全时限
           user_edited 常值」时,行内提示 + 单轨重新识别入口。中性玻璃底 + 下划线链接
           (设计稿 641-643;与上面的琥珀确认条刻意区分:提示 ≠ 需要一个决定)。
           链接 → 二次确认 → bridge.analyze({tracksMask:1<<${ch - 1}}, {clearManual:true})(契约 §1.6)。 -->
      <div class="tracks-row__hint" data-gb="${gb("manualdriven-hint")}" hidden>
        <span data-t="tracks.manualDrivenHint"></span>
        <button type="button" class="tracks-row__relink" data-gb="${gb("manualdriven-reidentify")}"></button>
      </div>
    </div>`;
}

// =============================================================================
// 三、DOM 接线
// =============================================================================

/**
 * @param {object} opts
 * @param {Document|Element} opts.root  查询根(app.js 传 document)
 * @param {object|null} opts.bridge     createBridge() 结果 —— 全部上行经它
 * @param {() => object} opts.getStore  取事件仓(**Tab2 的唯一渲染源**)
 * @param {() => object} opts.getT      取当前语言字典
 * @param {() => void} [opts.onLocalChange] 本地态改变后请求外壳重渲染
 */
export function createTabTracks(opts) {
    const root = opts.root;
    const getStore = opts.getStore || (() => ({}));
    const getT = opts.getT || (() => ({}));
    const bridge = opts.bridge || null;
    const requestRender = opts.onLocalChange || (() => render());

    async function call(name, ...args) {
        if (!bridge || typeof bridge[name] !== "function") return null;
        try {
            return await bridge[name](...args);
        } catch (e) {
            // 桥调用失败绝不打断渲染(浏览器直开 / mock 未注入时是常态)
            console.warn(`SCVB Tab2:bridge.${name}() 调用失败 —— ${e.message}`);
            return null;
        }
    }

    const $ = (gb) => root.querySelector(`[data-gb="${gb}"]`);
    // panel 上的 data-empty 由「连接轨数 === 0」判定(scvb.conn);空态与表格互斥的
    // 显隐全部由那一个属性驱动(CSS 在 index.html)。
    const panel = $("tab-tracks");
    const head = $("tracks-head");
    const body = $("tracks-body");
    const legend = $("tracks-legend-text");
    const emptyMain = $("tracks-emptystate-main");
    const multiLeadBadge = $("tracks-legend-multilead-badge");

    /**
     * 页面内一次性状态(不属 state chunk,重开面板即重置)。
     *   • `manualConfirmed` —— 已弹过 `setTrackManual` 确认的轨(契约 §1.16 线程/频率行:
     *     **每轨每会话一次,无条件** —— 纯 user_edited 轨同样弹);
     *   • `manualEcho` —— 拖动/键盘期间的本地乐观值(松手才发一次 `setTrackManual`:
     *     它入撤销栈,逐帧发会把撤销栈灌满,口径同 §1.22「边界拖拽释放才发」);
     *   • `paramEcho` —— width/freeze 的 gesture 乐观值(由 `scvb.params` 逐帧失效);
     *   • `unfreezeHint` —— 解冻(freeze 某位 1→0)且仍由手动常值驱动的轨(05 §2.2 R2);
     *     存的是**触发位**(bit0=pan / bit1=vol),该位重新冻回 0→1 时逐位撤销。
     */
    const local = {
        manualConfirmed: new Set(),
        confirm: null, // {ch, kind:"manual"|"reidentify", dim, value, locked}
        manualEcho: new Map(), // "ch:pan" / "ch:vol" → 本地乐观值
        paramEcho: new Map(), // ParamID → 本地乐观值
        gesture: null, // 拖动中的 ParamID(灰显/让位判定要排除自己)
        drag: null, // {ch, kind, id, startX, startY, start, moved, node}
        unfreezeHint: new Map(), // ch → 触发提示的 freeze 位(1..3)
        pairOpen: 0, // 配对面板展开中的轨(0 = 无;单例,同一时刻只开一个)
        editingCh: 0, // label 行内编辑中的轨(渲染不得覆写该行的输入框)
        freezePrev: new Map(), // ParamID → 上一次见到的 freeze 值(检测 1→0)
        // 延迟提交计时器 **per (ch,dim)**:共享单个句柄时,在轨 1 滚完 300ms 内去滚轨 2
        // 会把轨 1 那次待提交一起 clearTimeout 掉 —— 乐观值留在 UI 上,引擎里却没写进去。
        manualTimers: new Map(), // "ch:pan" / "ch:vol" → setTimeout 句柄
        rows: new Map(), // ch → 该行的节点缓存(15 行 × 30 Hz 下不许逐帧 querySelector)
    };

    /**
     * Tab2 是否为当前激活页 —— 四个 tab 面板同时在 DOM 里,靠 `#content[data-tab]` 切换
     * (app.js 的 `activateTab`)。查不到宿主(node 侧 / 单独嵌入)一律按活跃处理。
     */
    function isPanelActive() {
        if (!panel || typeof panel.closest !== "function") return true;
        const host = panel.closest("[data-tab]");
        return !host || host.getAttribute("data-tab") === "tracks";
    }

    const meters = createMeterRenderer({
        body,
        getStore,
        isActive: isPanelActive,
    });

    // ---------------------------------------------------------------- 小工具
    /** 属性写入去抖:值没变就不写(15 行 × 30 Hz 下,重复 setAttribute 会白白触发样式失效)。 */
    function attr(node, name, value) {
        if (!node) return;
        const v = String(value);
        if (node.getAttribute(name) !== v) node.setAttribute(name, v);
    }

    function text(node, value) {
        if (node && node.textContent !== value) node.textContent = value;
    }

    function show(node, on) {
        if (node && node.hidden === !!on) node.hidden = !on;
    }

    /** tooltip 一律 title:词条为空就移除(空 title 在部分宿主 WebView 里仍弹空气泡)。 */
    function setTitle(node, value) {
        if (!node) return;
        if (value) attr(node, "title", value);
        else node.removeAttribute("title");
    }

    /** 词条占位符求值(i18n.js 只发字典,不做模板求值)。 */
    function fmt(raw, vals) {
        if (typeof raw !== "string") return "";
        return raw.replace(/\{(\w+)\}/g, (m, k) =>
            Object.prototype.hasOwnProperty.call(vals, k) ? String(vals[k]) : m,
        );
    }

    function activeVersion(st) {
        const s = st || getStore();
        return (
            ((s.state || {}).global || {}).version_active ||
            (s.params && s.params.versionActive) ||
            1
        );
    }

    /**
     * 写权限缺失才挡上行:只读观察(契约 §1.15/§1.16 → `{observer:true}`)。
     * **`hostEcho` 不挡**——契约 §1.12-§1.14 拒绝态行逐字「无」,它只做灰显(口径同 Tab1)。
     * `noTimeline` 也不挡:它只 disable 采集/输出两个开关(§1.2/§1.3),配置类照常可写。
     */
    function isWriteBlocked() {
        return !!getStore().readOnly;
    }

    /** 采样率不一致 ⇒ 整行 disabled(05 §2.2 状态灯行 / ipc §5)。 */
    function isRowDead(ch) {
        const cc = ((getStore().conn || {}).channels || [])[ch - 1] || null;
        return trackStatusOf(cc) === "srErr";
    }

    /** N/15 口径同 header(契约 §2.3,J01):slotState=2 ∧ 心跳新鲜。 */
    function connectedTracks(conn) {
        let n = 0;
        for (const c of (conn && conn.channels) || []) {
            if (c && c.slotState === 2 && c.heartbeatFresh) n += 1;
        }
        return n;
    }

    // ------------------------------------------------------------ 参数面读写
    function readParam(id, dflt) {
        if (local.paramEcho.has(id)) return local.paramEcho.get(id);
        const vals = (getStore().params || {}).values || {};
        return Object.prototype.hasOwnProperty.call(vals, id) ? vals[id] : dflt;
    }

    function sendParam(id, value) {
        local.paramEcho.set(id, value); // 乐观本地态:等 25 Hz 回推之前先动起来
        call("setParam", id, value);
        requestRender();
    }

    /** 三段式的一次性形态(键盘 / 双击 / 开关:begin → set → end 各一次)。 */
    function oneShotGesture(id, value) {
        call("beginParamGesture", id);
        sendParam(id, value);
        call("endParamGesture", id);
    }

    /**
     * `scvb.params` 到达后让本地乐观值让位(规则同 Tab1 的 `nextParamEcho`):
     * `full:true` 整表作废;增量批次只作废本帧提到的 id,**正在拖动中的那个除外**
     * (松手前让位会与自己的回声打架,把手回跳)。
     */
    function dropParamEcho(payload) {
        if (!payload || payload.full) {
            local.paramEcho.clear();
            return;
        }
        for (const id of Object.keys(payload.values || {})) {
            if (id !== local.gesture) local.paramEcho.delete(id);
        }
    }

    // ------------------------------------------------- setTrackManual(§1.16)
    function manualKey(ch, dim) {
        return ch + ":" + dim;
    }

    function readManual(ch, dim, fallback) {
        const k = manualKey(ch, dim);
        return local.manualEcho.has(k) ? local.manualEcho.get(k) : fallback;
    }

    /**
     * 手动常值写入的**唯一入口**。首次(每轨每会话)先弹行内确认,确认后才落。
     * `defer=true` 时走延迟提交(滚轮档 / 键盘连按:回声即时、提交防抖),见 `queueManual`。
     * 返回 true = 已下发或已排程;false = 已改为弹确认条(值挂在 local.confirm 上待提交)。
     */
    function requestManual(ch, dim, value, defer) {
        if (isWriteBlocked() || isRowDead(ch)) return false;
        if (!local.manualConfirmed.has(ch)) {
            openConfirm(ch, "manual", dim, value);
            return false;
        }
        if (defer) queueManual(ch, dim, value);
        else sendManual(ch, dim, value);
        return true;
    }

    /**
     * 延迟提交:乐观值立刻上屏、`setTrackManual` 停手 `MANUAL_COMMIT_MS` 才发一次。
     * 计时器按 **(ch,dim)** 分别持有 —— 换轨/换维度各自计时,互不取消。
     */
    function queueManual(ch, dim, value) {
        const k = manualKey(ch, dim);
        const rng = dim === "vol" ? VOL_RANGE : PAN_RANGE;
        const v = quantize(rng, value);
        local.manualEcho.set(k, v);
        clearTimeout(local.manualTimers.get(k));
        local.manualTimers.set(
            k,
            setTimeout(() => {
                local.manualTimers.delete(k);
                // 排程与落地隔着 300 ms:这中间该轨可能刚变成采样率错 / 面板转只读。
                // 落不下去就把乐观值一并撤掉 —— 留着它 UI 就在显示一个从没写进引擎的数。
                if (isWriteBlocked() || isRowDead(ch)) {
                    local.manualEcho.delete(k);
                    requestRender();
                    return;
                }
                sendManual(ch, dim, v);
            }, MANUAL_COMMIT_MS),
        );
        requestRender();
    }

    function sendManual(ch, dim, value) {
        const rng = dim === "vol" ? VOL_RANGE : PAN_RANGE;
        const v = quantize(rng, value);
        // 同一 (ch,dim) 上若还挂着延迟提交,直接落这一次就够了(否则稍后会再发一遍旧值)
        if (local.manualTimers.has(manualKey(ch, dim))) {
            clearTimeout(local.manualTimers.get(manualKey(ch, dim)));
            local.manualTimers.delete(manualKey(ch, dim));
        }
        local.manualEcho.set(manualKey(ch, dim), v);
        // 契约 §1.16:入撤销栈(Ctrl+Z 由 app.js 的全局键盘钩子映射到 undo())。
        const echoKey = manualKey(ch, dim);
        call("setTrackManual", ch, dim, v).then((res) => {
            // 非成功响应(observer / badArg / 桥异常 res=null)撤乐观值,
            // 别让 UI 显示一个从没写进引擎的数(PR #60 复审【重要】2)。
            // 但只撤**本次发送**的值:两笔在途时旧笔失败不得抹掉新笔的乐观值
            // (pr-agent 在途竞态)
            if (
                !(res && res.ok === true) &&
                local.manualEcho.get(echoKey) === v
            )
                local.manualEcho.delete(echoKey);
            // 用户裁定 2026-08-20:拖动 = 接管手动 —— 写入成功后把该维度的
            // 冻结位同步置 1,开关如实反映「当前由手动驾驶」;不置位的话轨子
            // 卡在「手动常值但开关灭」的哑态:解冻提示只认 1→0 转换,永远
            // 不会触发,回实时的正规出口(解冻 → 重新识别)也就断了。
            if (res && res.ok === true) {
                const fid = paramIdOf(activeVersion(), ch, "freeze");
                const bits = freezeBits(readParam(fid, 0));
                if (!(dim === "vol" ? bits.vol : bits.pan))
                    toggleFreeze(ch, dim);
            }
            requestRender();
        });
        requestRender();
    }

    // ------------------------------------------------------------- 行内确认层
    function openConfirm(ch, kind, dim, value) {
        local.confirm = {
            ch,
            kind,
            dim: dim || "",
            value,
            locked: lockedCountOf(getStore().segments, ch),
        };
        requestRender();
    }

    function closeConfirm() {
        local.confirm = null;
        requestRender();
    }

    function acceptConfirm() {
        const c = local.confirm;
        if (!c) return;
        local.confirm = null;
        if (c.kind === "manual") {
            // 每轨每会话一次:确认过之后同轨的后续拖动直接落
            local.manualConfirmed.add(c.ch);
            sendManual(c.ch, c.dim, c.value);
            return;
        }
        doReidentify(c.ch);
    }

    /** 单轨重新识别(契约 §1.6:`analyze(scope, {clearManual:true})`;locked 段仍免疫)。 */
    async function doReidentify(ch) {
        const res = await call(
            "analyze",
            { tracksMask: 1 << (ch - 1) },
            { clearManual: true },
        );
        // 只有真发起了重析才清提示;桥异常(res=null)什么都没发生,提示要留着
        if (res && res.ok === true) local.unfreezeHint.delete(ch);
        requestRender();
    }

    // ------------------------------------------------------ setChannelConfig
    function channelCfg(ch) {
        return ((getStore().state || {}).channels || [])[ch - 1] || {};
    }

    function patchConfig(ch, patch) {
        if (isWriteBlocked() || isRowDead(ch)) return;
        // 契约 §1.15:patch 全部字段可选、只写给出的字段;写入后 config_seq+1 经 scvb.state 回推
        call("setChannelConfig", ch, patch).then(requestRender);
    }

    function toggleConfig(ch, field) {
        const cfg = channelCfg(ch);
        const cur = field === "enabled" ? cfg.enabled !== false : !!cfg[field];
        patchConfig(ch, { [field]: !cur });
    }

    function stepPriority(ch, dir) {
        const cur = clampPriority(num(channelCfg(ch).priority, 5));
        const next = clampPriority(cur + dir);
        if (next === cur) return; // 0..10 钳制(统筹裁定 B17):到头就不发
        patchConfig(ch, { priority: next });
    }

    // ------------------------------------------------------------ 冻结双开关
    /**
     * 两枚 UI 开关 ↔ 一个每轨参数 `v{active}_t{ch:02d}_freeze`(契约 §1.12-§1.14:
     * int 0-3,bit0=pan / bit1=vol)。**不新增桥函数**,走 gesture 三段式(可被 DAW 录)。
     */
    function toggleFreeze(ch, which) {
        if (isWriteBlocked() || isRowDead(ch)) return;
        const id = paramIdOf(activeVersion(), ch, "freeze");
        const bits = freezeBits(readParam(id, 0));
        const next =
            which === "pan"
                ? freezeValue(!bits.pan, bits.vol)
                : freezeValue(bits.pan, !bits.vol);
        noteFreezeTransition(ch, freezeValue(bits.pan, bits.vol), next);
        oneShotGesture(id, next);
    }

    /**
     * 解冻提示的触发判定(05 §2.2 R2,J65 改触发词):某位 **1→0** 且该轨当前版本曲线
     * 仍是「单段全时限 user_edited 常值」时挂行内提示 + 单轨重新识别入口。
     * 本地翻转与宿主回推(`scvb.params`)两条路径都要走这里,否则被 DAW 自动化解冻时不提示。
     *
     * 记的是**触发位**而非一个布尔(位账见 `unfreezeHintBits`):1→0 记上、0→1 抹掉,
     * 清零即撤提示 —— 只在「双位全冻」时才撤的写法会让单维度轨留下陈旧提示。
     */
    function noteFreezeTransition(ch, prev, next) {
        const bits = unfreezeHintBits(local.unfreezeHint.get(ch), prev, next);
        if (bits) local.unfreezeHint.set(ch, bits);
        else local.unfreezeHint.delete(ch);
    }

    // -------------------------------------------------------- label 行内编辑
    function beginLabelEdit(ch) {
        if (isWriteBlocked() || isRowDead(ch)) return;
        const n = local.rows.get(ch);
        if (!n || !n.labelInput) return;
        local.editingCh = ch;
        attr(n.labelcell, "data-editing", "1");
        n.labelInput.value = channelCfg(ch).label || "";
        n.labelInput.focus();
        n.labelInput.select();
    }

    function endLabelEdit(commit) {
        const ch = local.editingCh;
        if (!ch) return;
        local.editingCh = 0;
        const n = local.rows.get(ch);
        if (n) attr(n.labelcell, "data-editing", "0");
        if (!commit || !n || !n.labelInput) {
            requestRender();
            return;
        }
        const raw = n.labelInput.value;
        // 契约 §1.15:label ≤24 字符。按**码点**截断(.length 是 UTF-16 码元,
        // 增补平面字符会虚高;口径与 header 重命名计数一致)。
        const next = Array.from(raw).slice(0, LABEL_MAX).join("");
        if (next !== (channelCfg(ch).label || "")) {
            patchConfig(ch, { label: next });
        } else {
            requestRender();
        }
    }

    // ---------------------------------------------------------------- 拖拽面
    /** 卡箍拖拽:水平位移 → dB(管内槽宽 = 全行程 36 dB);Shift 微调(05 §2.2)。 */
    function beginVolDrag(e, ch, node) {
        const row = local.rows.get(ch);
        const slotW =
            row && row.tube ? row.tube.getBoundingClientRect().width : TUBE_W;
        const startDb = currentVolDb(ch);
        if (!local.manualConfirmed.has(ch)) {
            openConfirm(ch, "manual", "vol", startDb);
            return;
        }
        local.drag = {
            ch,
            kind: "vol",
            startX: e.clientX,
            start: startDb,
            span: slotW > 0 ? slotW : TUBE_W,
            node,
        };
        capture(node, e);
    }

    /** pan 旋钮:**仅冻结态可拖**(未冻结是只读实时表盘,05 §2.2);垂直拖拽 150px = 满程。 */
    function beginPanDrag(e, ch, node) {
        const bits = freezeBits(
            readParam(paramIdOf(activeVersion(), ch, "freeze"), 0),
        );
        if (!bits.pan) return; // 自动态:交互禁用(tooltip 已说明由分析曲线驱动)
        const startPan = currentPan(ch);
        if (!local.manualConfirmed.has(ch)) {
            openConfirm(ch, "manual", "pan", startPan);
            return;
        }
        local.drag = {
            ch,
            kind: "pan",
            startY: e.clientY,
            start: startPan,
            span: KNOB_DRAG_PX,
            node,
        };
        capture(node, e);
    }

    /** width 旋钮:gesture 三段式直写 `v{active}_t{ch:02d}_width`(契约 §1.12-§1.14)。 */
    function beginWidthDrag(e, ch, node) {
        const st = getStore();
        // mono 轨 v1 no-op(05 §2.2:灰显 + tooltip),不开 gesture
        if (
            (((st.state || {}).channels || [])[ch - 1] || {})
                .source_channels !== 2
        ) {
            return;
        }
        const id = paramIdOf(activeVersion(st), ch, "width");
        local.drag = {
            ch,
            kind: "width",
            id,
            startY: e.clientY,
            start: num(readParam(id, WIDTH_RANGE.def), WIDTH_RANGE.def),
            span: KNOB_DRAG_PX,
            node,
            lastSent: 0,
        };
        local.gesture = id;
        call("beginParamGesture", id);
        capture(node, e);
    }

    function capture(node, e) {
        try {
            node.setPointerCapture(e.pointerId);
        } catch {
            /* 无指针捕获也能用,只是拖出元素外会断 */
        }
        e.preventDefault();
    }

    function onDragMove(e) {
        const d = local.drag;
        if (!d) return;
        // 有**真实位移**才算「拖过」——endDrag 据此区分「拖完松手」与「原地单击」(见下)。
        // 判的是该控件实际读的那根轴(卡箍读 x、旋钮读 y),而不是「收到过 pointermove」:
        // 部分宿主 WebView 会在 pointerdown 之后补一发同坐标的 move。
        if (
            d.kind === "vol" ? e.clientX !== d.startX : e.clientY !== d.startY
        ) {
            d.moved = 1;
        }
        // Shift 微调倍率**按控件取自己的值域常量**(三者今天同为 .2,分叉了也不会错位)
        const rng =
            d.kind === "vol"
                ? VOL_RANGE
                : d.kind === "pan"
                  ? PAN_RANGE
                  : WIDTH_RANGE;
        const fine = e.shiftKey ? rng.fine : 1;
        if (d.kind === "vol") {
            const dx = (e.clientX - d.startX) / d.span;
            const next = d.start + dx * (VOL_RANGE.max - VOL_RANGE.min) * fine;
            local.manualEcho.set(
                manualKey(d.ch, "vol"),
                quantize(VOL_RANGE, next),
            );
            requestRender();
            return;
        }
        // 旋钮一律**向上为增**(屏幕 y 向下,故取负号)
        const dy = (d.startY - e.clientY) / d.span;
        if (d.kind === "pan") {
            const next = d.start + dy * (PAN_RANGE.max - PAN_RANGE.min) * fine;
            local.manualEcho.set(
                manualKey(d.ch, "pan"),
                quantize(PAN_RANGE, next),
            );
            requestRender();
            return;
        }
        const next = quantize(
            WIDTH_RANGE,
            d.start + dy * (WIDTH_RANGE.max - WIDTH_RANGE.min) * fine,
        );
        // 契约 §1.12-§1.14 线程/频率行:拖动期间 setParam 由 UI 侧节流(建议 ≤50 Hz)
        const t = Date.now();
        if (t - d.lastSent < 20) return;
        d.lastSent = t;
        sendParam(d.id, next);
    }

    function endDrag() {
        const d = local.drag;
        if (!d) return;
        local.drag = null;
        if (d.kind === "width") {
            local.gesture = null;
            call("endParamGesture", d.id);
            return;
        }
        // pan / vol:**松手才发一次** setTrackManual(它入撤销栈,逐帧发会灌满撤销栈;
        // 口径同契约 §1.22 段边界「拖拽释放才发」)。
        // **零位移不发**:manualEcho 有意留到 §2.8 回推才清,原地单击会把留存的乐观值
        // 原样再写一遍 —— 同值重写段表 + 撤销栈白多一步。
        if (!d.moved) return;
        const v = local.manualEcho.get(manualKey(d.ch, d.kind));
        if (v !== undefined) sendManual(d.ch, d.kind, v);
    }

    function currentVolDb(ch) {
        const row = rowOf(ch);
        return readManual(ch, "vol", row ? row.volDb : VOL_RANGE.def);
    }

    function currentPan(ch) {
        const row = rowOf(ch);
        return readManual(ch, "pan", row ? row.pan : PAN_RANGE.def);
    }

    function rowOf(ch) {
        return rowsFromStore(getStore())[ch - 1] || null;
    }

    // ------------------------------------------------------------- 事件委托
    /** 命中解析:15 行密排下不逐行挂监听,全部走 body 上的一组委托。 */
    function hit(e) {
        if (!(e.target instanceof Element)) return null;
        const row = e.target.closest(".tracks-row");
        if (!row) return null;
        const ch = Number(row.getAttribute("data-ch")) || 0;
        const el = e.target.closest("[data-gb]");
        const gb = el ? el.getAttribute("data-gb") || "" : "";
        const prefix = `tracks-row-${ch}-`;
        return {
            row,
            ch,
            el,
            part: gb.startsWith(prefix) ? gb.slice(prefix.length) : "",
        };
    }

    const CONFIG_TOGGLES = Object.freeze({
        leadlock: "lead_lock",
        volexempt: "lead_vol_exempt",
        autopan: "participate_in_auto_pan",
        enable: "enabled",
    });

    function activate(h) {
        const { ch, part } = h;
        // 确认条/提示行的按钮**不受整行 disabled 约束**(它们是撤下确认的出口)
        if (part === "manual-overwrite-cancel") return closeConfirm();
        if (part === "manual-overwrite-ok") return acceptConfirm();
        if (part === "manualdriven-reidentify") {
            // 05 §2.2:单轨重新识别走二次确认(同 §2.3「重新识别(含手动段)」)
            return openConfirm(ch, "reidentify", "", 0);
        }
        if (isRowDead(ch) || isWriteBlocked()) return;
        if (Object.prototype.hasOwnProperty.call(CONFIG_TOGGLES, part)) {
            return toggleConfig(ch, CONFIG_TOGGLES[part]);
        }
        // 配对自定义下拉:触发钮开合;选项 = 选中 pair_id 并收起(契约 §1.15,0|1..7)
        if (part === "pair") {
            return openPairPanel(local.pairOpen === ch ? 0 : ch);
        }
        if (part.startsWith("pair-opt-")) {
            const v = clamp(0, PAIR_LETTERS.length, Number(part.slice(9)) || 0);
            openPairPanel(0);
            return patchConfig(ch, { pair_id: v });
        }
        switch (part) {
            case "freezepan":
                return toggleFreeze(ch, "pan");
            case "freezevol":
                return toggleFreeze(ch, "vol");
            case "priority-dec":
                return stepPriority(ch, -1);
            case "priority-inc":
                return stepPriority(ch, 1);
            case "label":
            case "labelcell":
                return beginLabelEdit(ch);
            default:
                return undefined;
        }
    }

    function wire() {
        if (!body) return;
        body.addEventListener("click", (e) => {
            const h = hit(e);
            if (h) activate(h);
        });

        // 双击回默认值(05 §2.2:vol 回 0 dB / pan 回 0 / width 回 100)
        body.addEventListener("dblclick", (e) => {
            const h = hit(e);
            if (!h || isRowDead(h.ch) || isWriteBlocked()) return;
            if (h.part === "vol-collar") {
                e.preventDefault();
                requestManual(h.ch, "vol", VOL_RANGE.def);
            } else if (h.part === "pan") {
                const id = paramIdOf(activeVersion(), h.ch, "freeze");
                if (!freezeBits(readParam(id, 0)).pan) return;
                e.preventDefault();
                requestManual(h.ch, "pan", PAN_RANGE.def);
            } else if (h.part === "width") {
                // mono 轨 v1 no-op:双击回默认也要跟 beginWidthDrag 同款拦截,
                // 灰显控件不得发起写(PR #60 pr-agent)
                const st = getStore();
                if (
                    (((st.state || {}).channels || [])[h.ch - 1] || {})
                        .source_channels !== 2
                ) {
                    return;
                }
                e.preventDefault();
                oneShotGesture(
                    paramIdOf(activeVersion(st), h.ch, "width"),
                    WIDTH_RANGE.def,
                );
            }
        });

        body.addEventListener("pointerdown", (e) => {
            const h = hit(e);
            if (!h || isRowDead(h.ch) || isWriteBlocked()) return;
            if (h.part === "vol-collar") beginVolDrag(e, h.ch, h.el);
            else if (h.part === "pan") beginPanDrag(e, h.ch, h.el);
            else if (h.part === "width") beginWidthDrag(e, h.ch, h.el);
        });
        body.addEventListener("pointermove", onDragMove);
        body.addEventListener("pointerup", endDrag);
        body.addEventListener("pointercancel", endDrag);

        // pan 滚轮 ±1(05 §2.2 冻结态);滚完 300ms 才提交一次,不逐格灌撤销栈
        body.addEventListener(
            "wheel",
            (e) => {
                const h = hit(e);
                if (!h || h.part !== "pan") return;
                if (isRowDead(h.ch) || isWriteBlocked()) return;
                const id = paramIdOf(activeVersion(), h.ch, "freeze");
                if (!freezeBits(readParam(id, 0)).pan) return;
                e.preventDefault();
                const dir = e.deltaY < 0 ? 1 : -1;
                // 回声即时、提交防抖(计时器 per (ch,dim),换轨不互相取消)
                requestManual(h.ch, "pan", currentPan(h.ch) + dir, true);
            },
            { passive: false },
        );

        body.addEventListener("keydown", onKeyDown);
        // 配对面板点外收起(触发钮/面板内部除外)
        root.addEventListener("pointerdown", (e) => {
            if (!local.pairOpen) return;
            if (
                e.target instanceof Node &&
                e.target instanceof Element &&
                e.target.closest(".tracks-row__pair")
            )
                return;
            openPairPanel(0);
        });
        body.addEventListener("focusout", (e) => {
            const h = hit(e);
            if (h && h.part === "label-input") endLabelEdit(true); // 失焦提交
        });
    }

    /** 键盘面:开关(空格/回车)+ 三个 slider 的方向键步进 + label 输入框提交/取消。 */
    function onKeyDown(e) {
        const h = hit(e);
        if (!h) return;
        if (h.part === "label-input") {
            if (e.key === "Enter") {
                e.preventDefault();
                endLabelEdit(true);
            } else if (e.key === "Escape") {
                e.preventDefault();
                endLabelEdit(false);
            }
            return;
        }
        if (e.key === "Escape" && local.pairOpen) {
            e.preventDefault();
            return openPairPanel(0);
        }
        if (e.key === " " || e.key === "Enter") {
            // role="switch"/button 的键盘可达(toggle 六枚 + PRIO ± + label +
            // 确认条 + 配对触发钮/选项——自定义下拉后 pair 走 activate 开合,
            // 不再有原生 select 行为可放行)
            if (
                h.part === "pan" ||
                h.part === "width" ||
                h.part === "vol-collar"
            )
                return;
            e.preventDefault();
            activate(h);
            return;
        }
        const dir =
            e.key === "ArrowRight" || e.key === "ArrowUp"
                ? 1
                : e.key === "ArrowLeft" || e.key === "ArrowDown"
                  ? -1
                  : 0;
        if (!dir) return;
        if (isRowDead(h.ch) || isWriteBlocked()) return;
        const ch = h.ch;
        // 配对触发钮:↑/↓ 不开面板直接换值(复刻原生 collapsed select 手感)
        if (h.part === "pair") {
            e.preventDefault();
            const cur = Math.trunc(num(channelCfg(ch).pair_id, 0));
            const v = clamp(0, PAIR_LETTERS.length, cur + dir);
            if (v !== cur) patchConfig(ch, { pair_id: v });
            return;
        }
        if (h.part === "vol-collar") {
            e.preventDefault();
            // 键盘步进 ±0.1 dB,Shift 加速 ×10(05 §2.2 的 -24..+12 dB 域)。
            // 提交与滚轮同款防抖:按住不放时 OS 自动重复 ~30 Hz,逐次发会灌满撤销栈。
            const step = VOL_RANGE.step * (e.shiftKey ? VOL_RANGE.coarse : 1);
            requestManual(ch, "vol", currentVolDb(ch) + dir * step, true);
            return;
        }
        if (h.part === "pan") {
            const id = paramIdOf(activeVersion(), ch, "freeze");
            if (!freezeBits(readParam(id, 0)).pan) return;
            e.preventDefault();
            const step = PAN_RANGE.step * (e.shiftKey ? PAN_RANGE.coarse : 1);
            requestManual(ch, "pan", currentPan(ch) + dir * step, true);
            return;
        }
        if (h.part === "width") {
            const st = getStore();
            if (
                (((st.state || {}).channels || [])[ch - 1] || {})
                    .source_channels !== 2
            ) {
                return;
            }
            e.preventDefault();
            const id = paramIdOf(activeVersion(st), ch, "width");
            const step =
                WIDTH_RANGE.step * (e.shiftKey ? WIDTH_RANGE.coarse : 1);
            oneShotGesture(
                id,
                quantize(
                    WIDTH_RANGE,
                    num(readParam(id, WIDTH_RANGE.def), WIDTH_RANGE.def) +
                        dir * step,
                ),
            );
        }
    }

    // ---------------------------------------------------------------- mount
    function mount() {
        if (head) head.innerHTML = trackHeadHtml();
        // 首帧结构用空行模型撑出来(值全部由 render 逐帧回写;WAVE1_SAMPLE 已删)
        if (body) body.innerHTML = emptyRows().map(trackRowHtml).join("");
        cacheRows();
        wire();
        meters.attach();
        meters.start(); // rAF 弹道(契约 §2.5 30 Hz 事件 → 60 fps 补间)
    }

    /** 15 行的节点缓存 —— 每行一次性取齐,render 与弹道循环都不再 querySelector。 */
    function cacheRows() {
        local.rows.clear();
        if (!body) return;
        const SUFFIX = [
            "statuslight",
            "stereo",
            "labelcell",
            "label",
            "label-input",
            "leadcenter",
            "lowsample",
            "recapture-badge",
            "pan",
            "width",
            "vol-tube",
            "vol-collar",
            "volexempt-badge",
            "freeze-version",
            "priority-val",
            "priority-dec",
            "priority-inc",
            "leadlock",
            "pair-chip",
            "pair-text",
            "pair",
            "pair-panel",
            "pair-opt-none-label",
            "pair-overflow",
            "volexempt",
            "autopan",
            "freezepan",
            "freezevol",
            "enable",
            "manual-overwrite-confirm",
            "manual-overwrite-confirm-locked",
            "manual-overwrite-cancel",
            "manual-overwrite-ok",
            "manualdriven-hint",
            "manualdriven-reidentify",
        ];
        for (let ch = 1; ch <= CHANNEL_COUNT; ch++) {
            const row = body.querySelector(`[data-gb="tracks-row-${ch}"]`);
            if (!row) continue;
            const n = { row };
            for (const s of SUFFIX) {
                n[camel(s)] = row.querySelector(
                    `[data-gb="tracks-row-${ch}-${s}"]`,
                );
            }
            n.tube = n.volTube;
            n.confirmText = n.manualOverwriteConfirm
                ? n.manualOverwriteConfirm.querySelector(
                      ".tracks-row__confirm-text",
                  )
                : null;
            n.hintText = n.manualdrivenHint
                ? n.manualdrivenHint.querySelector("span")
                : null;
            local.rows.set(ch, n);
        }
    }

    function camel(s) {
        return s.replace(/-([a-z])/g, (m, c) => c.toUpperCase());
    }

    // --------------------------------------------------------------- render
    function render() {
        const t = getT() || {};
        const st = getStore();
        renderLegend(t);
        renderEmpty(t, st);
        renderRows(t, st);
    }

    function renderLegend(t) {
        if (legend) {
            const segs = legendSegments(t["tracks.colLegend"]);
            if (segs.length) {
                // 设计稿 626:三处 <strong>(font-weight 600 + --txt-min),分隔符「·」+ 全角空格
                const frag = document.createDocumentFragment();
                segs.forEach((seg, i) => {
                    if (i > 0)
                        frag.appendChild(document.createTextNode(" ·　"));
                    if (seg.term) {
                        const b = document.createElement("strong");
                        b.textContent = seg.term;
                        frag.appendChild(b);
                    }
                    frag.appendChild(document.createTextNode(seg.rest));
                });
                legend.replaceChildren(frag);
            }
        }
        // 多主唱琥珀 badge(05 §2.2 主唱锁行:多轨同时锁中允许,但要显示提示)
        const chans = (getStore().state || {}).channels || [];
        show(multiLeadBadge, leadLockCount(chans) >= 2);
    }

    function renderEmpty(t, st) {
        // 0 轨连接 ⇒ 空态与表格互斥(首帧快照未到之前不判定,免得闪一下空态)
        if (panel) {
            const empty = st.ready && connectedTracks(st.conn) === 0;
            attr(panel, "data-empty", empty ? "1" : "0");
        }
        // 空态主句是**组号变体**(05 §2.2:句中出现两次 {g})
        if (!emptyMain) return;
        const raw = t["tracks.emptyGroup"];
        if (typeof raw !== "string") return;
        const gid = ((st.state || {}).group_id || 1) - 1;
        text(
            emptyMain,
            raw.split("{g}").join("ABCDEFGH"[gid < 0 || gid > 7 ? 0 : gid]),
        );
    }

    function renderRows(t, st) {
        const rows = rowsFromStore(st);
        const chans = (st.state || {}).channels || [];
        const counts = pairCounts(chans);
        const active = activeVersion(st);
        const blocked = isWriteBlocked();
        // hostEcho 灰显按**批次新鲜度**判定(口径与 Tab1 一致:hostEcho 标志停发后不会
        // 翻回 false,直接用它会让 pan/width/卡箍/冻结永久滞留 55% 透明)。
        const echo =
            (st.params || {}).hostEcho &&
            Date.now() - ((st.params || {}).hostEchoAt || 0) <
                HOST_ECHO_FRESH_MS
                ? "1"
                : "0";
        for (const row of rows)
            syncRow(row, t, { counts, active, blocked, echo });
    }

    /* eslint-disable-next-line complexity -- 一行 20 余个控件的属性回写,拆开反而更难核对 */
    function syncRow(row, t, ctx) {
        const ch = row.n;
        const n = local.rows.get(ch);
        if (!n) return;
        const dead = row.status === "srErr" ? 1 : 0;
        const dis = dead || ctx.blocked ? "1" : "0";
        // 拖动/键盘中的乐观值优先(等 setTrackManual 的段表回推之前先动起来)
        const volDb = readManual(ch, "vol", row.volDb);
        const pan = readManual(ch, "pan", row.pan);
        const widthId = paramIdOf(ctx.active, ch, "width");
        const width = num(readParam(widthId, row.w), row.w);
        const freeze = freezeBits(
            readParam(
                paramIdOf(ctx.active, ch, "freeze"),
                freezeValue(row.fp, row.fv),
            ),
        );

        // ---- 行本体(五态 / on:0 / lead 底纹 / 确认层抬层)----
        attr(n.row, "data-status", row.status);
        attr(n.row, "data-on", row.on);
        attr(n.row, "data-lead", row.lead);
        attr(n.row, "data-dead", dead);

        // ---- 状态灯(§2.3 五态 + 失准计数进 tooltip)----
        const vis = statusVisual(row.status);
        attr(n.statuslight, "data-tone", vis.tone);
        attr(n.statuslight, "data-pulse", vis.pulse ? "1" : "0");
        setTitle(n.statuslight, fmt(t[vis.key], { n: row.misalign }));

        // ---- CH / ST / label ----
        show(n.stereo, !!row.st);
        if (local.editingCh !== ch) {
            attr(n.labelcell, "data-editing", "0");
            text(n.label, row.label || labelPlaceholder(ch, t));
            attr(n.label, "data-placeholder", row.label ? 0 : 1);
        }
        setTitle(n.labelInput, t["tracks.labelEdit"]);
        show(n.leadcenter, !!row.leadCenter);
        setTitle(n.leadcenter, t["master.leadSelectHint"]);
        show(n.lowsample, !!row.low);
        setTitle(n.lowsample, t["lowSample.full"]);
        show(n.recaptureBadge, !!row.recapture);
        setTitle(n.recaptureBadge, t["wave.recaptureArmed"]);

        // ---- pan 旋钮(冻结 = 手动可拖;未冻结 = 只读实时表盘)----
        if (n.pan) n.pan.style.setProperty("--ang", panAngleDeg(pan) + "deg");
        attr(n.pan, "data-live", freeze.pan ? 0 : 1);
        attr(n.pan, "data-disabled", dis);
        attr(
            n.pan,
            "aria-disabled",
            freeze.pan && dis === "0" ? "false" : "true",
        );
        attr(n.pan, "aria-valuenow", Math.round(pan));
        attr(n.pan, "data-host-echo", ctx.echo);
        setTitle(n.pan, freeze.pan ? "" : t["tracks.panAutoHint"]);

        // ---- width 旋钮(mono 轨 v1 no-op:灰显 + tooltip)----
        const wDis = dead || ctx.blocked || !row.st ? "1" : "0";
        if (n.width)
            n.width.style.setProperty("--ang", widthAngleDeg(width) + "deg");
        attr(n.width, "data-disabled", wDis);
        // ARIA 只认 true/false(data-* 的 "1"/"0" 惯性会触发 axe aria-valid-attr-value)
        attr(n.width, "aria-disabled", wDis === "1" ? "true" : "false");
        attr(n.width, "aria-valuenow", Math.round(width));
        attr(n.width, "data-host-echo", ctx.echo);
        setTitle(n.width, row.st ? "" : t["tracks.monoWidthNoop"]);

        // ---- 玻璃管:卡箍(vol)。液柱与峰线由 canvas/meter.js 的 rAF 弹道逐帧写 ----
        if (n.volCollar)
            n.volCollar.style.setProperty("--vol", volPercent(volDb) + "%");
        attr(n.volCollar, "data-frozen", freeze.vol ? 1 : 0);
        attr(n.volCollar, "data-disabled", dis);
        // 量化后再写:volDb 可能直接来自段表/参数面的 f32,原样写会出 -3.4000000000000004
        attr(n.volCollar, "aria-valuenow", quantize(VOL_RANGE, volDb));
        attr(n.volCollar, "data-host-echo", ctx.echo);
        show(n.volexemptBadge, !!row.ex);
        // 用户反馈 2026-08-20:角标要说清是**音量**豁免(v1 仅此一种;
        // 声像侧退出是「声像」参与开关,不叫豁免)——详文进 tooltip,
        // 角标本体维持「豁免」(34px 槽放不下四字,槽加宽会破 1133 宽度预算)
        setTitle(n.volexemptBadge, t["tracks.volExemptTip"]);
        show(n.freezeVersion, !!freeze.vol);
        text(n.freezeVersion, row.version);
        // tooltip 用版本全名(词条 {v} 占位)
        setTitle(
            n.freezeVersion,
            fmt(t["tracks.freezeVersion"], { v: row.versionName }),
        );

        // ---- PRIO / LEAD / 配对 ----
        text(n.priorityVal, String(row.prio));
        attr(n.priorityDec, "data-disabled", dis);
        attr(n.priorityInc, "data-disabled", dis);
        syncToggle(n.leadlock, row.lead, dis);
        syncPair(n, row, t, ctx.counts, dis, ch);

        // ---- 参与性两枚 + 冻结两枚 + ON ----
        syncToggle(n.volexempt, row.ex, dis);
        syncToggle(n.autopan, row.part, dis);
        syncToggle(n.freezepan, freeze.pan ? 1 : 0, dis);
        syncToggle(n.freezevol, freeze.vol ? 1 : 0, dis);
        attr(n.freezepan, "data-host-echo", ctx.echo);
        attr(n.freezevol, "data-host-echo", ctx.echo);
        syncToggle(n.enable, row.on, dis);

        // ---- 行内确认层 / 解冻提示 ----
        syncConfirm(n, row, t, ch);
    }

    function syncToggle(node, on, dis) {
        if (!node) return;
        attr(node, "data-on", on ? 1 : 0);
        attr(node, "aria-checked", on ? "true" : "false");
        attr(node, "data-disabled", dis);
        attr(node, "aria-disabled", dis === "1" ? "true" : "false");
    }

    /** 配对面板开合(单例:同一时刻只开一个;ch=0 = 全收)。 */
    function openPairPanel(ch) {
        if (local.pairOpen === ch) return;
        const prev = local.rows.get(local.pairOpen);
        local.pairOpen = ch;
        if (prev) {
            attr(prev.pairPanel, "data-open", 0);
            attr(prev.pair, "aria-expanded", "false");
        }
        const cur = local.rows.get(ch);
        if (cur) {
            attr(cur.pairPanel, "data-open", 1);
            attr(cur.pair, "aria-expanded", "true");
        }
        requestRender();
    }

    function syncPair(n, row, t, counts, dis, ch) {
        attr(n.pairChip, "data-set", row.pair ? 1 : 0);
        attr(n.pairChip, "data-pair", row.pair);
        // chip 上的字:无配对显示词条「无」,有配对显示 A–G(设计稿 1768 pairText)
        text(
            n.pairText,
            row.pair ? pairLetter(row.pair) : t["tracks.pairNone"] || "",
        );
        show(n.pairOverflow, !!row.pairFull);
        setTitle(n.pairOverflow, t["tracks.pairOverflow"]);
        const trig = n.pair;
        if (!trig) return;
        if (dis === "1") {
            trig.setAttribute("disabled", "");
            if (local.pairOpen === ch) openPairPanel(0);
        } else trig.removeAttribute("disabled");
        attr(n.pairPanel, "data-open", local.pairOpen === ch ? 1 : 0);
        attr(trig, "aria-expanded", local.pairOpen === ch ? "true" : "false");
        if (!n.pairPanel) return;
        // 选项:「无」+ A–G;某组已满 2 轨的选项**仍可选**,只加「(满)」后缀(05 §2.2)
        if (n.pairOptNoneLabel)
            text(n.pairOptNoneLabel, t["tracks.pairNone"] || "");
        for (const opt of n.pairPanel.querySelectorAll("[data-pair]")) {
            const v = Number(opt.getAttribute("data-pair")) || 0;
            const curSel = v === row.pair;
            attr(opt, "data-current", curSel ? 1 : 0);
            attr(opt, "aria-selected", curSel ? "true" : "false");
            if (v) {
                const label = opt.querySelector(".tracks-row__pair-optlabel");
                const suffix = isPairFullOption(counts, v, row.pair)
                    ? t["tracks.pairFullSuffix"] || ""
                    : "";
                if (label) text(label, PAIR_LETTERS[v - 1] + suffix);
            }
        }
    }

    /**
     * 行内确认条(R3 防误伤)与解冻提示(R2)。两者共用行下方那一格,
     * 同一时刻只出一件 —— 确认条是「需要一个决定」,提示是「一条建议」。
     */
    function syncConfirm(n, row, t, ch) {
        const c =
            local.confirm && local.confirm.ch === ch ? local.confirm : null;
        show(n.manualOverwriteConfirm, !!c);
        if (c) {
            if (c.kind === "manual") {
                text(n.confirmText, t["tracks.manualOverwriteConfirm"] || "");
                // 存在 locked 段时追加「(含 {l} 个锁定段)」——§1.16 会连 locked 一并替换
                show(n.manualOverwriteConfirmLocked, c.locked > 0);
                text(
                    n.manualOverwriteConfirmLocked,
                    fmt(t["tracks.manualOverwriteConfirm.locked"], {
                        l: c.locked,
                    }),
                );
                text(n.manualOverwriteOk, t["common.continue"] || "");
            } else {
                text(
                    n.confirmText,
                    fmt(t["tracks.reidentifyConfirm"], { n: tt(ch) }),
                );
                show(n.manualOverwriteConfirmLocked, false);
                text(n.manualOverwriteOk, t.reidentify || "");
            }
            text(n.manualOverwriteCancel, t["common.cancel"] || "");
        }
        // 解冻提示:该位 1→0 且该轨当前版本曲线仍是单段全时限 user_edited 常值
        const hint = !c && local.unfreezeHint.has(ch) && !!row.manualConst;
        show(n.manualdrivenHint, hint);
        attr(n.row, "data-confirm", c || hint ? 1 : 0);
        if (!hint) return;
        text(n.hintText, t["tracks.manualDrivenHint"] || "");
        text(
            n.manualdrivenReidentify,
            fmt(t["tracks.reidentifyOne"], { n: tt(ch) }),
        );
    }

    // ------------------------------------------------- 事件入口(app.js 转发)
    /** §2.5 30 Hz:只喂弹道,**不触发 render**(15 行 × 30 Hz 的整页重渲染扛不住)。 */
    function onMeters(m) {
        meters.push(m);
    }

    /** §2.2 25 Hz:乐观值让位 + 冻结位 1→0 的解冻提示判定。 */
    function onParams(payload) {
        dropParamEcho(payload);
        const values = (payload && payload.values) || {};
        const active = activeVersion();
        for (let ch = 1; ch <= CHANNEL_COUNT; ch++) {
            const id = paramIdOf(active, ch, "freeze");
            if (!Object.prototype.hasOwnProperty.call(values, id)) continue;
            const next = Math.trunc(num(values[id], 0)) & 3;
            const seen = local.freezePrev.has(id);
            const prev = seen ? local.freezePrev.get(id) : next;
            local.freezePrev.set(id, next);
            if (seen) noteFreezeTransition(ch, prev, next);
        }
    }

    /** §2.8:段表变了 ⇒ 手动值的乐观显示让位给回推的常值段。 */
    function onSegments(payload) {
        for (const c of (payload && payload.channels) || []) {
            if (!c) continue;
            local.manualEcho.delete(manualKey(c.ch, "pan"));
            local.manualEcho.delete(manualKey(c.ch, "vol"));
        }
    }

    return { mount, render, onMeters, onParams, onSegments };
}
