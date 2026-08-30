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
//       `scvb.segments`       → **未冻结**维度的读回值(手动常值段)与解冻提示判定
//                               ([J85] 冻结维度的读回值改取参数面,见 rowFromStore)。
//     上行:`setChannelConfig`(§1.15)/ `setTrackManual`(§1.16,含每轨首次无条件确认)/
//     gesture 三段式(§1.12-§1.14,width 与 freeze)/ `analyze(scope,{clearManual:true})`
//     (§1.6,单轨重新识别)。**不新增任何桥函数。**
// =============================================================================

// 弹道渲染器(契约 §2.5 的 30 Hz 事件 → rAF 补间);模块顶层零副作用,node 可直接 import。
import { createMeterRenderer } from "./canvas/meter.js";
import { paramIdOf, readbackVersion } from "../shared/param-id.js";
// [SL-241] 未冻结维度的读回真源(优先级链 + 它依赖的三个纯判定)已移到
// `web/shared/readback.js` —— Tab1 的分布图要用**同一条链**(它原先只读参数面,
// 于是 SL-211 的验收只在 Tab2 成立)。详见那一件的头注。
import {
    freezeBits,
    readbackSegsOf,
    segmentsOfCh,
} from "../shared/readback.js";
// hostEcho 灰显的批次新鲜度窗口 —— 与 Tab1 **共用同一个常量**,不在这里写第二份数字;
// format = 词条 {x} 占位填充(labelPlaceholder 用;漏导入曾致空轨名行 ReferenceError,
// PR #60 红旗)。
import {
    HOST_ECHO_FRESH_MS,
    format,
    lowSampleChannels,
    secondsToTimecode,
} from "./tab-master.js";

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
 * 都会连出一串值,而 `setTrackManual` 的**手动接管通道**入撤销栈(契约 §0.9;[J85] 冻结通道
 * 不产生 CRVS 事务、不入栈),逐次发会把宿主 UndoManager 灌满 —— 故「回声即时、提交防抖」:
 * 停手 300 ms 才落一次。防抖对两条通道一视同仁(冻结通道逐帧发同样是白费的宿主往返)。
 * 拖拽走的是松手提交,不用它。
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
 * 「音量 / 电平」列宽 = 管本身(设计稿 594 的 268)。
 * 曾有豁免角标(34px)与冻结生效版本小字(16px)两枚附件槽——用户裁定
 * 2026-08-21 移除(豁免改参与语义开关、版本归属信任用户),05 §2.2 对应
 * 验收行建议同步删除(见 deviations §P)。
 */
export const VOL_COL_W = TUBE_W;

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
        // 中性:灰蓝、不脉冲。与 amber/red 刻意拉开 —— 它描述的是「这会儿没在出数据」,
        // 不是「出问题了」。
        case "suspended":
            return { tone: "slate", pulse: false, key: "tracks.suspended" };
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

// [SL-229 复审①] `paramIdOf` / `readbackVersion` 已移到 `web/shared/param-id.js` ——
// Tab1 的分布图也要用同一份判据(它原先自己拼 `v{v}_t{ch}_pan`,切版本那一帧照样
// 查空、照样闪一排居中柱)。**在此原样再导出**,既有 import 点一字不改
// (手法同 tab-master 里的 distGeometry)。
export { paramIdOf, readbackVersion } from "../shared/param-id.js";
// [SL-241] 同上:在此原样再导出,既有 import 点(app.js / tab-wave.js / 冒烟)一字不改。
export {
    curveSegmentAt,
    freezeBits,
    manualConstantOf,
    readbackSegsOf,
    segmentsOfCh,
} from "../shared/readback.js";

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
    // 写方停着(宿主在无信号段挂起 Input / 用户 bypass)。**排在 warn 之后**:真失准比
    // 挂起更要紧,两者同时成立时先说失准。这是**中性态**,不是告警 —— 乐句间隙里宿主挂起
    // Input 属于正常现象,用户自己 bypass 的更不需要红灯(统筹裁定:丙案)。
    if (c.suspended === true) return "suspended";
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

/**
 * §2.8 里 `stale` 为真的轨数(04 §4.5 fingerprint watchdog:该轨上游音频与已采集特征
 * 不一致,建议重新采集)。横幅 ⑧ 与 tab 导航琥珀点共用它。
 *
 * **必须读合并后的段表视图,不能读单个事件的 `channels`** —— §2.8 的 `channels` 只含
 * 受影响轨(一次段编辑只带一轨),拿事件当全量算会把其余轨的 stale 一起抹掉。
 */
export function staleTrackCount(segments) {
    const list = (segments && segments.channels) || [];
    let n = 0;
    for (const c of list) if (c && c.stale) n += 1;
    return n;
}

/**
 * 手动首写确认条要不要弹(纯函数,node 侧可直接断言)。
 *
 * **[J85] 用户裁定 2026-08-27(方案 A):冻结通道不弹。** 确认条正文
 * (`tracks.manualOverwriteConfirm`)说的是「将以固定值**替换该轨的全部分段结果**,可撤销」——
 * 冻结通道两句都不成立:它不替换任何段(`replacedSegments` 恒 0)、也不入撤销栈(无 CRVS 事务)。
 * 拿一条关于「替换全部、可撤销」的警告去拦一次「只改了个旋钮值」的操作,是在吓唬用户。
 * 05 §2.2 R3 的「**无条件**」原指「删掉 `origin=auto` 前置条件」(纯 user_edited 轨同样要弹),
 * 不是「连不会替换段的通道也要弹」—— 本裁定不与之冲突。
 *
 * 未冻结的手动接管通道**照旧弹**:那一路确实会把整条分析曲线整表压成常值段,是真正
 * 需要用户点头的破坏性操作。
 *
 * `freeze` = 该轨 freeze 参数当前值(0-3);`dim` = "pan" | "vol";`confirmed` = 该轨本会话是否已确认过。
 */
export function needsManualConfirm(freeze, dim, confirmed) {
    if (confirmed) return false; // 每轨每会话一次
    const bits = freezeBits(freeze);
    return !(dim === "vol" ? bits.vol : bits.pan); // 冻结维度不弹
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
 * 一帧内的**跨行公共量**(T33 性能批):版本号 / 配对计数 / 多主唱 / 主唱居中 /
 * 布防位图 / lowSample 轨集 —— 与 ch 无关,15 行共用一份。
 *
 * 为什么单拎出来:`rowOf(ch)` 在拖动期每步进都要读一行的当前值,原先走
 * `rowsFromStore()[ch-1]` 会把 15 行连同这些公共量整套重算(pairCounts /
 * leadLockCount 各扫一遍 channels)。拆开后单行路径只算自己那一行,
 * 整页路径仍只算一次公共量(见 rowsFromStore)。
 */
export function rowContext(store) {
    const st = store || {};
    const state = st.state || {};
    const chans = state.channels || [];
    const vals = (st.params && st.params.values) || {};
    const rec = state.recapture || null;
    return {
        chans,
        conn: (st.conn && st.conn.channels) || [],
        vals,
        segments: st.segments,
        // [SL-211] 未冻结维度按**播放头所在的曲线段**读回;没有播放头就是 0 = 曲线起点,
        // 正是「刚切进一个版本、还没播放」那一档。
        timeS: num((st.playhead || {}).timeS, 0),
        // [SL-211 复审终轮③a 裁定] 输出档决定未冻结维度的**权威在哪一边**:
        //   OFF(跟随宿主)= 引擎不驱动,声音跟的就是宿主参数面 ⇒ 显示参数面;
        //   ON(写入自动化)= 引擎按曲线驱动 ⇒ 显示曲线。
        // 这才是 J78「显示权威」的完整形态:显示与 DSP 在**两个档上都**一致,
        // 而不是只在 ON 档对上。
        outputOn: (state.global || {}).output_enabled === true,
        // [SL-229] 读回命名空间跟着**params 真的带着的那一版**走,不跟 state 抢跑
        // (详见 readbackVersion 头注:抢跑会让 15 轨闪一帧居中)。
        active: readbackVersion(
            vals,
            (state.global && state.global.version_active) || 0,
            (st.params && st.params.versionActive) || 0,
        ),
        counts: pairCounts(chans),
        multiLead: leadLockCount(chans) >= 2 ? 1 : 0,
        leadSel: Math.trunc(num(vals.lead_select, 0)),
        recMask: rec && rec.armed ? Math.trunc(num(rec.tracksMask, 0)) : 0,
        // §2.9 `lowSample` 是轨级 error(载荷带 ch),且会同时命中多轨 ——
        // app.js 按 `lowSample#{ch}` 复合键存,这里按值扫成轨号集合(T33)。
        low: lowSampleChannels(st.errors),
    };
}

/**
 * 事件仓 → **单行**模型(ch ∈ 1..15)。`ctx` 省略时就地算一份(node 侧单行断言方便);
 * 整页渲染与拖动期增量都从外面传进来复用。字段说明见 rowsFromStore。
 */
export function rowFromStore(store, ch, ctx) {
    const c = ctx || rowContext(store || {});
    const { vals, active } = c;
    const cfg = c.chans[ch - 1] || {};
    const cc = c.conn[ch - 1] || null;
    // 原值直接交给 `freezeBits` —— 它内部已做 `num()` 兜底、四舍五入、钳到 [0,3]、非有限回 0。
    // 这里**不许再先 `Math.trunc`**:那会把 1.9 截成 1(只冻 pan),而 native `freezeBitsOf`
    // 与 mock 都进位成 2(只冻 vol),同一个值在三侧给出两种答案。本文件其余调用点
    // (`requestManual` / `beginVolDrag`)传的都是原值,`rowFromStore` 曾是唯一的例外(SL-188)。
    const freeze = num(vals[paramIdOf(active, ch, "freeze")], 0);
    const bits = freezeBits(freeze);
    const segCh = segmentsOfCh(c.segments, ch);
    // 读回值,**逐维按 freeze 位分叉**([J85]):
    //   • 冻结维度 → **参数面**。冻结的静态值只存参数面 + 冻结位,曲线真身不再被烘焙成
    //     常值段(`setTrackManual` 的冻结通道不写曲线)。此时段表里若还留着一条**旧的**
    //     常值段(先在未冻结态接管过手动、UI 随即置位冻结),读段表就会把把手弹回那个旧值,
    //     而耳朵听到的是参数面上的新值 —— 「看着没改、听着改了」。
    //   • 未冻结维度 → 有手动常值段就取该段(05 §2.2「读回值同样取自该段」),否则取参数面。
    //     这一支**必须**先读段表:未冻结轨拖卡箍(05 明确允许,走一次性确认)写的是曲线真身,
    //     25 Hz 的 `scvb.params` 在非 PRINT 态没有新值,改读参数面会让把手在下一帧弹回去。
    //   [SL-211] 未冻结那一支的回落**按输出档分叉**(复审终轮③a 裁定):
    //     · 输出 ON(写入自动化)= 引擎按曲线驱动 ⇒ 读**播放头所处的曲线段**。
    //       否则会出现用户实测的那一幕:刚复制完版本切进去还没播放,参数面装的是宿主
    //       的出厂默认,15 轨声像齐刷刷居中;一播放又全对(引擎开始驱动参数面)。
    //     · 输出 OFF(跟随宿主)= 引擎根本不驱动,声音跟的就是宿主参数面 ⇒ 读参数面。
    //       这一档若也显示曲线,就成了「看着曲线、听着宿主」——显示与 DSP 反而分了家。
    //   只有段表整个为空(还没分析过)才无条件回落参数面。
    // [SL-241] 这条链本身已抽到 `readbackSegsOf`(web/shared/readback.js)—— Tab1 的
    // 分布图读的是同一个量,却一直只读参数面。抽出来之后两处共用,再想分叉得先改那里。
    // 一次算两维:frozen 只决定「用不用」,不影响算出来是哪一段。
    // `manual` = 行上那枚「手动常值」标,由同一次调用回出 —— 标与读回链因此必然同判定。
    const {
        pan: panSeg,
        vol: volSeg,
        manual: seg,
    } = readbackSegsOf(segCh, bits, c.outputOn, c.timeS);
    const pan = panSeg
        ? num(panSeg.pan, PAN_RANGE.def)
        : num(vals[paramIdOf(active, ch, "pan")], PAN_RANGE.def);
    const volDb = volSeg
        ? num(volSeg.volDb, VOL_RANGE.def)
        : num(vals[paramIdOf(active, ch, "vol")], VOL_RANGE.def);
    const pair = Math.trunc(num(cfg.pair_id, 0));
    return {
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
        volPart: cfg.lead_vol_exempt ? 0 : 1, // 显示层参与语义(=!exempt)
        part: cfg.participate_in_auto_pan ? 1 : 0,
        pair,
        fp: bits.pan ? 1 : 0,
        fv: bits.vol ? 1 : 0,
        on: cfg.enabled === false ? 0 : 1,
        low: c.low.has(ch) ? 1 : 0,
        misalign: cc ? Math.trunc(num(cc.misalignCount, 0)) : 0,
        leadCenter: c.leadSel === ch ? 1 : 0,
        pairFull: isPairOverflow(c.counts, pair) ? 1 : 0,
        recapture: c.recMask & (1 << (ch - 1)) ? 1 : 0,
        multiLead: c.multiLead,
        manualConst: seg ? 1 : 0,
        // [SL-230] 手动常值有没有被锁:锁着的话 clearManual 碰不了它(契约 §1.6
        // 「locked 免疫,须先逐段解锁」),「恢复自动」得改说「先解锁」而不是给钮。
        manualConstLocked: seg && seg.locked ? 1 : 0,
    };
}

/**
 * 事件仓 → 15 行模型(**Tab2 的唯一渲染源**)。
 * 字段名沿用 Wave 1 模板的短名(`trackRowHtml` 不动):
 * st=stereo 标 / vol=卡箍行程比 0..1 / lv·pk=液柱与峰线行程比(由 meter.js 逐帧覆写) /
 * volPart=音量参与(=!lead_vol_exempt) / part=participate_in_auto_pan /
 * fp·fv=冻结两位 / low=样本不足。
 * 公共量只算一次(rowContext),逐行走 rowFromStore —— 行为与 T32 逐字一致。
 */
export function rowsFromStore(store) {
    const st = store || {};
    const ctx = rowContext(st);
    const rows = [];
    for (let ch = 1; ch <= CHANNEL_COUNT; ch++) {
        rows.push(rowFromStore(st, ch, ctx));
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

// [SL-230 复审②]「恢复自动」的**触发钮**图标。逆时针环箭头,画成**内联 SVG**而不是
// 字符 —— `↺`(U+21BA)在四款子集字体里一个字形都没有(gates 3h 实测),真机上会掉进
// 系统字体或直接上屏方块;而重跑 fetch_fonts.py 要联网、且每加一个符号就得再跑一次。
// 画出来的图形不吃字体,这条路一次走通就永久有效。`currentColor` 让它跟着按钮配色走。
//
// ⚠ 图标本身**不进文案字符集**,但**注释不要写进模板字面量里** —— gates 3h 的扫描面是
// JS 字符串字面量,行模板是一整条模板字面量,写在里面的 HTML 注释会被当成文案统计,
// 于是一段中文注释就能让门禁要求字体补上十几个汉字。故这段说明留在模块层。
//
// 它为什么在**行内**而不是浮条:`.tracks-row__hint` 是 absolute + top:100%,画在**下一行**
// 头上。做临时提示(一次只出一条、点掉就走)没问题,做**常驻**入口就是灾难 —— 两条以上
// 手动常值轨时每条都盖住下一行,下一行点不动,正是本卡要消灭的那类「点了没反应」。
// 行高 44px 是设计常量(15 行 × 44 撑起整块),浮条改成占位会顶掉布局;故触发钮进单元格
// (零宽度代价:只在该轨手动时才出),浮条只留给**确认**那一瞬(与 manual-overwrite
// 确认条同一档,一次一条)。页面级实测见 web-preview/tests/row-occlusion-probe.mjs。
const RESTORE_ICON =
    '<svg viewBox="0 0 16 16" aria-hidden="true" focusable="false">' +
    '<path d="M4.2 6.4A4.6 4.6 0 1 1 3.5 8.6" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/>' +
    '<path d="M1.9 6.1 4.2 6.4 4.9 4.2" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>' +
    "</svg>";

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
    // 全参数导览(tour)锚点:只挂第 1 行(与 freeze 同款),挂在有 principal box 的真实盒上
    const dt = (name) => (ch === 1 ? ` data-tour="${name}"` : "");
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
    <div class="tracks-row" role="row" data-glow="1" data-gb="tracks-row-${ch}"${dt("trackrow")} data-ch="${ch}"
         data-status="${t.status}" data-on="${t.on}" data-lead="${t.lead}" data-dead="${dead}"
         data-confirm="0">
      <!-- 重采集布防目标轨的行首 badge(05 §2.3 ②,B-05 裁定「行首」:贴行左缘的
           琥珀点 + tooltip —— 152px label 列里塞不下长句,压成点;T33 Wave 2 迁位) -->
      <span class="tracks-row__dotmark" data-gb="${gb("recapture-badge")}"${t.recapture ? "" : " hidden"}></span>
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
        <!-- SL-230 restore-auto trigger (see rowHtml header note) -->
        <button type="button" class="tracks-row__restore" data-gb="${gb("restore-auto")}"
                data-disabled="0" hidden>${RESTORE_ICON}</button>
      </span>
      <span class="tracks-row__cell" role="cell" style="width:${W.pan}px">
        <!-- 冻结态:垂直拖拽 / 滚轮 ±1 / 双击回 0 → bridge.setTrackManual(${ch}, "pan", v)(契约 §1.16;
             每轨首次操作前无条件弹行内确认,05 §2.2 R3);未冻结态 = 只读实时表盘 + tooltip。
             role="slider" + tabindex 是键盘可达面(未冻结时 aria-disabled,不摘 tabindex —— 摘了
             屏幕阅读器就读不到「自动模式」这条 tooltip)。 -->
        <span class="sc-knob" data-live="${panLive}" data-disabled="${dead}"
              role="slider" tabindex="0" aria-valuemin="${PAN_RANGE.min}" aria-valuemax="${PAN_RANGE.max}"
              aria-valuenow="${Math.round(num(t.pan, 0))}" data-t-aria="tracks.colPan"
              style="--ang:${panAngleDeg(t.pan)}deg" data-gb="${gb("pan")}"${dt("pan")}><span class="sc-knob__needle"></span></span>
      </span>
      <span class="tracks-row__cell" role="cell" style="width:${W.width}px">
        <!-- bridge.beginParamGesture / setParam / endParamGesture(v{active}_t${tt(ch)}_width)
             —— 契约 §1.12-§1.14(双击回默认 100);mono 轨 v1 no-op(灰显 + tooltip)。 -->
        <span class="sc-knob" data-live="0" data-disabled="${wDis}"
              role="slider" tabindex="0" aria-valuemin="${WIDTH_RANGE.min}" aria-valuemax="${WIDTH_RANGE.max}"
              aria-valuenow="${Math.round(num(t.w, 100))}" data-t-aria="tracks.colW"
              style="--ang:${widthAngleDeg(t.w)}deg" data-gb="${gb("width")}"${dt("widthknob")}><span class="sc-knob__needle"></span></span>
      </span>
      <span class="tracks-row__cell tracks-row__voltube" role="cell" style="width:${W.vol}px" data-gb="${gb("voltube")}">
        <span class="sc-tube" data-gb="${gb("vol-tube")}"${dt("vollevel")}>
          <span class="sc-tube__slot">
            <span class="sc-tube__liquid" style="--lv:${(t.lv * 100).toFixed(1)}%"></span>
            <span class="sc-tube__peak" style="--pk:${(t.pk * 100).toFixed(1)}%" data-alert="${t.pk > PEAK_ALERT_RATIO ? 1 : 0}"${t.pk ? "" : " hidden"}></span>
          </span>
          <span class="sc-tube__gloss"></span>
          <!-- 卡箍 = 音量推子把手,层序在液柱之上(满幅电平仍可辨,T32 验收硬要求②);
               拖拽命中区 = 卡箍本体(管体不接受拖拽,05 §2.2),透明命中扩展见 index.html(RE-06)。
               水平拖拽 / 键盘 ±0.1 dB(Shift 加速)/ 双击回 0 dB
               → bridge.setTrackManual(${ch}, "vol", v) —— 契约 §1.16。 -->
          <span class="sc-tube__collar" style="--vol:${t.vol * 100}%" data-frozen="${t.fv}" data-disabled="${dead}"
                role="slider" tabindex="0" aria-valuemin="${VOL_RANGE.min}" aria-valuemax="${VOL_RANGE.max}"
                aria-valuenow="${quantize(VOL_RANGE, num(t.volDb, 0))}" data-t-aria="tracks.colVolLevel"
                data-gb="${gb("vol-collar")}"></span>
        </span>
      </span>
      <span class="tracks-row__cell sc-stepper tracks-row__prio" role="cell" style="width:${W.prio}px" data-gb="${gb("priority")}"${dt("prio")}>
        <button type="button" data-t-aria="common.decrease" data-gb="${gb("priority-dec")}"${dis}>−</button>
        <span class="sc-stepper__val" data-gb="${gb("priority-val")}">${clampPriority(t.prio)}</span>
        <button type="button" data-t-aria="common.increase" data-gb="${gb("priority-inc")}"${dis}>+</button>
        <!-- ± 步进 → bridge.setChannelConfig(${ch}, {priority})(契约 §1.15;0..10 钳制,到头不发) -->
      </span>
      <span class="tracks-row__cell" role="cell" style="width:${W.lead}px">
        <span class="sc-toggle" data-on="${t.lead}" data-disabled="${dead}"${sw(t.lead)}
              data-t-aria="leadLock" data-gb="${gb("leadlock")}"${dt("leadlock")}></span>
      </span>
      <span class="tracks-row__cell" role="cell" style="width:${W.pair}px">
        <!-- 配对:05 要「下拉:无 / A–G」,设计稿画的是只读 chip —— 保留原生 select 语义,
             视觉压成设计稿那枚 52px chip(select 透明叠在 chip 之上,统筹裁定 B16)。 -->
        <span class="tracks-row__pair" data-set="${t.pair ? 1 : 0}" data-pair="${t.pair}" data-gb="${gb("pair-chip")}"${dt("pair")}>
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
        <!-- 参与语义(用户裁定 2026-08-21:开=参与音量调节,与声像一致;
             契约字段仍是反义的 lead_vol_exempt,仅显示层取反,桥面不动) -->
        <span class="sc-toggle" data-on="${t.volPart}" data-disabled="${dead}"${sw(t.volPart)}
              data-t-aria="tracks.colVolPart" data-gb="${gb("volexempt")}"${dt("volexempt")}></span>
      </span>
      <span class="tracks-row__cell" role="cell" style="width:${W.autopan}px">
        <span class="sc-toggle" data-on="${t.part}" data-disabled="${dead}"${sw(t.part)}
              data-t-aria="participateAutoPan" data-gb="${gb("autopan")}"${dt("autopan")}></span>
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
              data-t-aria="tracks.colOn" data-gb="${gb("enable")}"${dt("enable")}></span>
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
           链接 → 直接 bridge.analyze({tracksMask:1<<${ch - 1}}, {clearManual:true})(契约 §1.6)。
           **单链一次确认**:本条自身就是那次确认(正文含「已锁定段保持不变」),链接不再另开
           第二道确认条 —— 老写法「提示 → 确认条 → 取消 → 提示又回来」两件互为对方的出口,
           成了一个关不掉的环(v5 实测 P0-2)。「知道了」是这条链唯一的终止出口。 -->
      <!-- [SL-230] **持久**的「恢复自动」入口。用户实测「找不到 clearManual」——
           定谳:单轨 clearManual 此前**唯一**的入口是下面那条解冻提示条,而它只在
           freeze 位 1→0 的那一下挂起、点「知道了」就永久消失;没走过冻结→解冻的人
           (或点掉过的人)此后再也没有单轨入口,只剩 Tab3 工具条那个要先勾轨拖选区的
           范围版。本行的显示条件与提示条**互补**:只要该轨仍被手动常值驱动就在,
           提示条正显示时让位给它(那条自带同一个入口,两个一起出是重复)。
           两态就地切换:按钮 → 确认(复用 tracks.reidentifyConfirm,与提示条同一句)。 -->
      <div class="tracks-row__hint" data-gb="${gb("restore-auto-row")}" hidden>
        <span data-gb="${gb("restore-auto-text")}"></span>
        <button type="button" class="tracks-row__relink" data-gb="${gb("restore-auto-cancel")}" data-t="common.cancel"></button>
        <button type="button" class="tracks-row__relink" data-gb="${gb("restore-auto-ok")}" data-t="common.continue"></button>
      </div>
      <div class="tracks-row__hint" data-gb="${gb("manualdriven-hint")}" hidden>
        <span data-t="tracks.manualDrivenHint"></span>
        <button type="button" class="tracks-row__relink" data-gb="${gb("manualdriven-reidentify")}"></button>
        <button type="button" class="tracks-row__relink" data-gb="${gb("manualdriven-dismiss")}" data-t="common.gotIt"></button>
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
    const recaptureBadge = $("tracks-legend-recapture-badge");

    /**
     * 页面内一次性状态(不属 state chunk,重开面板即重置)。
     *   • `manualConfirmed` —— 已弹过 `setTrackManual` 确认的轨(契约 §1.16 线程/频率行:
     *     **每轨每会话一次,无条件** —— 纯 user_edited 轨同样弹);
     *   • `manualEcho` —— 拖动/键盘期间的本地乐观值(松手才发一次 `setTrackManual`:
     *     手动接管通道入撤销栈,逐帧发会把撤销栈灌满,口径同 §1.22「边界拖拽释放才发」;
     *     [J85] 冻结通道虽不入栈,松手才发这一条同样适用 —— 逐帧写参数面是白费的宿主往返);
     *   • `paramEcho` —— width/freeze 的 gesture 乐观值(由 `scvb.params` 逐帧失效);
     *   • `unfreezeHint` —— 解冻(freeze 某位 1→0)且仍由手动常值驱动的轨(05 §2.2 R2);
     *     存的是**触发位**(bit0=pan / bit1=vol),该位重新冻回 0→1 时逐位撤销。
     */
    const local = {
        manualConfirmed: new Set(),
        confirm: null, // {ch, kind:"manual", dim, value, locked}(重新识别不再走确认条,见 P0-2)
        manualEcho: new Map(), // "ch:pan" / "ch:vol" → 本地乐观值
        paramEcho: new Map(), // ParamID → 本地乐观值
        gesture: null, // 拖动中的 ParamID(灰显/让位判定要排除自己)
        drag: null, // {ch, kind, id, startX, startY, start, moved, node}
        unfreezeHint: new Map(), // ch → 触发提示的 freeze 位(1..3)
        // [SL-230 复审②] 展开确认的**那一条**轨(0 = 没有)。**不能是 Set** ——
        // 确认浮条同样是 absolute/top:100%,两条同时展开照样互相盖。
        restoreConfirm: 0,
        reidentifying: new Set(), // 单轨重新识别在途:期间不接受新的解冻提示挂起(P0-2)
        pairOpen: 0, // 配对面板展开中的轨(0 = 无;单例,同一时刻只开一个)
        editingCh: 0, // label 行内编辑中的轨(渲染不得覆写该行的输入框)
        freezePrev: new Map(), // ParamID → 上一次见到的 freeze 值(检测 1→0)
        // 延迟提交计时器 **per (ch,dim)**:共享单个句柄时,在轨 1 滚完 300ms 内去滚轨 2
        // 会把轨 1 那次待提交一起 clearTimeout 掉 —— 乐观值留在 UI 上,引擎里却没写进去。
        manualTimers: new Map(), // "ch:pan" / "ch:vol" → setTimeout 句柄
        rows: new Map(), // ch → 该行的节点缓存(15 行 × 30 Hz 下不许逐帧 querySelector)
        // T33 性能批:拖动期的**按行增量**队列(见 requestRowRender)。
        dirtyRows: new Set(), // 待重投影的 ch
        rowRaf: 0, // 合帧句柄(同一帧内多次请求只跑一次)
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

    /**
     * `rowCh` 给出时走**按行增量**(width 旋钮拖动期:乐观值只改那一行的一个属性,
     * 不必请求整页);省略时照旧请求整页 render(开关/键盘等一次性写入,T33)。
     * 回滚分支是异步且罕见的一拍,一律走整页,免得漏掉别处的连带投影。
     */
    function sendParam(id, value, rowCh) {
        local.paramEcho.set(id, value); // 乐观本地态:等 25 Hz 回推之前先动起来
        call("setParam", id, value).then((res) => {
            // 写被拒时 25Hz 回推不会点名该 id,乐观值会挂死;与 sendManual
            // 同款:非成功且 echo 仍是本次值才回滚(后到的写不受累)
            if (
                !(res && res.ok === true) &&
                local.paramEcho.get(id) === value
            ) {
                local.paramEcho.delete(id);
                requestRender();
            }
        });
        if (rowCh) requestRowRender(rowCh);
        else requestRender();
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
            // full(切版本/全量重发)整表作废,**唯拖动中的 id 保留到松手**——
            // 否则指针还按着旋钮就被广播抢跳(pr-agent;与增量分支的
            // gesture 保护同口径)。Tab1 的 `nextParamEcho` full 分支已在本波
            // 对齐到同一口径(tab-master.js:保留 gestureId),两 tab 不再有差异
            // ——旧注里「Tab1 尚为全清、口径对齐记 deviations 归 T33 批」已作废。
            for (const id of [...local.paramEcho.keys()]) {
                if (id !== local.gesture) local.paramEcho.delete(id);
            }
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
     * 手动常值写入的**唯一入口**。**未冻结**维度首次(每轨每会话)先弹行内确认,确认后才落;
     * **冻结**维度直接落([J85] 用户裁定 2026-08-27 方案 A,判定见 `needsManualConfirm` 头注:
     * 冻结通道不替换任何段、也不入撤销栈,确认条正文的两句话都不成立)。
     * `defer=true` 时走延迟提交(滚轮档 / 键盘连按:回声即时、提交防抖),见 `queueManual`。
     * 返回 true = 已下发或已排程;false = 已改为弹确认条(值挂在 local.confirm 上待提交)。
     */
    function requestManual(ch, dim, value, defer) {
        if (isWriteBlocked() || isRowDead(ch)) return false;
        const freeze = readParam(paramIdOf(activeVersion(), ch, "freeze"), 0);
        if (needsManualConfirm(freeze, dim, local.manualConfirmed.has(ch))) {
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
        // 中央闸口:确认条挂着/拖动期间面板可能转只读或该轨转 srErr——
        // 所有入口(accept/endDrag/防抖计时器)统一在此复检,拦下则连
        // 乐观值一并撤(持久评审 Write race)
        if (isWriteBlocked() || isRowDead(ch)) {
            const k0 = manualKey(ch, dim);
            local.manualEcho.delete(k0);
            clearTimeout(local.manualTimers.get(k0));
            local.manualTimers.delete(k0);
            requestRender();
            return;
        }
        const rng = dim === "vol" ? VOL_RANGE : PAN_RANGE;
        const v = quantize(rng, value);
        // 同一 (ch,dim) 上若还挂着延迟提交,直接落这一次就够了(否则稍后会再发一遍旧值)
        if (local.manualTimers.has(manualKey(ch, dim))) {
            clearTimeout(local.manualTimers.get(manualKey(ch, dim)));
            local.manualTimers.delete(manualKey(ch, dim));
        }
        local.manualEcho.set(manualKey(ch, dim), v);
        // 契约 §1.16 撤销:**只有未冻结的手动接管通道入撤销栈**(Ctrl+Z 由 app.js 的全局键盘
        // 钩子映射到 undo())。[J85] 之后冻结通道不产生 CRVS 事务,压根不入栈 —— 那一路的写入
        // 落在宿主自动化面上,回滚归宿主的撤销栈管,插件 UndoManager 不碰自动化参数(§0.9)。
        const echoKey = manualKey(ch, dim);
        // 请求前捕获:冻结参数 id **连同版本上下文**一起定格(在途切版本时,
        // 回调若重算 activeVersion 会把置位落到新版本的参数上——整笔判定
        // 绑到发起时的版本;Reviewer Guide 边缘)
        const freezeId = paramIdOf(activeVersion(), ch, "freeze");
        const bitsNow = freezeBits(readParam(freezeId, 0));
        const wasUnfrozen = !(dim === "vol" ? bitsNow.vol : bitsNow.pan);
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
            // 只在「请求时就是未冻结」且「落地时仍未冻结」双条件下置位:
            // 冻结态拖动本就不该动开关;在途期间用户手动改过的也不覆盖
            // (Reviewer Guide 在途竞态)
            if (res && res.ok === true && wasUnfrozen) {
                // 复用请求时捕获的 freezeId(不经 toggleFreeze 以免其内部重算版本)
                const bits = freezeBits(readParam(freezeId, 0));
                if (!(dim === "vol" ? bits.vol : bits.pan)) {
                    const next =
                        dim === "vol"
                            ? freezeValue(bits.pan, true)
                            : freezeValue(true, bits.vol);
                    noteFreezeTransition(
                        ch,
                        freezeValue(bits.pan, bits.vol),
                        next,
                    );
                    oneShotGesture(freezeId, next);
                }
            }
            requestRender();
        });
        requestRender();
    }

    // ------------------------------------------------------------- 行内确认层
    function openConfirm(ch, kind, dim, value) {
        // 与配对面板互斥:确认条是「需要一个决定」,同格浮层不共存(持久评审 UI Overlap)
        openPairPanel(0);
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

    /** 确认条只剩「手动首写」一种(重新识别已改为提示条自带执行,见 P0-2)。 */
    function acceptConfirm() {
        const c = local.confirm;
        if (!c) return;
        local.confirm = null;
        {
            // 每轨每会话一次:确认过之后同轨的后续拖动直接落
            local.manualConfirmed.add(c.ch);
            // 值未变(拖动发起的确认只带**抓握值**)⇒ 只标记不写入——
            // 原值写整轨是纯破坏(白建手动段+自动冻结+撤销项,零听感变化),
            // 用户确认后的下一次拖动才是真调整;滚轮/键盘发起的确认带
            // **已调整值**,照常写入(持久评审 Drag Confirm)
            const cur = c.dim === "vol" ? currentVolDb(c.ch) : currentPan(c.ch);
            const rng = c.dim === "vol" ? VOL_RANGE : PAN_RANGE;
            if (quantize(rng, c.value) !== quantize(rng, cur)) {
                sendManual(c.ch, c.dim, c.value);
            } else {
                requestRender();
            }
        }
    }

    /** 单轨重新识别(契约 §1.6:`analyze(scope, {clearManual:true})`;locked 段仍免疫)。 */
    async function doReidentify(ch) {
        // 与 patchConfig/toggleFreeze 同口径双守卫:解冻提示条按钮不受整行
        // disabled 约束,srErr 轨挂着提示时点「重新识别」也不得发写面调用
        if (isWriteBlocked() || isRowDead(ch)) return;
        // 提示**先撤**,再发请求。老写法「只有 res.ok===true 才撤」看着稳妥,实际是这个环的
        // 另一半:analyze 被拒(无覆盖 / busy / 桥异常)时提示原样回来,用户除了再点一次没有
        // 别的动作可做,而再点一次照样被拒 —— 于是永远出不去(v5 实测 P0-2)。
        // 用户已经在这条提示上做过一次决定,不该因为引擎拒绝而被反复追问;失败的原因由
        // §2.2 的分析态与工具条反馈承载,不靠这条提示重播。
        local.unfreezeHint.delete(ch);
        // 在途期间挡住重新挂起:clearManual 会把 freeze 位清零,那个 1→0 转换经 scvb.params
        // 回来时正好命中 noteFreezeTransition —— 不挡的话修复本身会把提示重新点亮。
        local.reidentifying.add(ch);
        requestRender();
        try {
            await call(
                "analyze",
                { tracksMask: 1 << (ch - 1) },
                { clearManual: true },
            );
        } finally {
            local.reidentifying.delete(ch);
            requestRender();
        }
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
        // 重新识别在途:这一轮 1→0 是修复动作自己造成的,不是用户解冻,不挂提示。
        if (local.reidentifying.has(ch)) return;
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
        // 确认闸口与 `requestManual` 共用**同一个判定**([J85] 方案 A)。拖拽的落地走
        // `endDrag → sendManual`,**不经 `requestManual`**,所以这里必须自己接上 ——
        // 各留一份裸 `manualConfirmed.has(ch)` 就等于裁定在拖拽这条主路径上没落地
        // (#106 终轮复审重要①)。
        if (
            needsManualConfirm(
                readParam(paramIdOf(activeVersion(), ch, "freeze"), 0),
                "vol",
                local.manualConfirmed.has(ch),
            )
        ) {
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
        // 同 beginVolDrag:共用 `needsManualConfirm`。**本函数尤其要紧** —— 上一行刚保证
        // 「未冻结不可拖」,于是能走到这里的 pan 拖拽**必定已冻结**,按方案 A 一条确认条都
        // 不该弹。留裸判定的后果不只是多弹一条:accept 走 `manualConfirmed.add(ch)` 是**按轨**
        // 记额度,误弹一次被点掉之后,该轨真正需要确认的「未冻结 vol 首拖」(那一路才会把整条
        // 分析曲线整表压成常值段)反而不弹了 —— 等于把确认条从该弹的地方挪到了不该弹的地方。
        // freeze 值复用上面已读出的 bits,不再读一次参数(同一笔判定绑同一个快照)。
        if (
            needsManualConfirm(
                freezeValue(bits.pan, bits.vol),
                "pan",
                local.manualConfirmed.has(ch),
            )
        ) {
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
            requestRowRender(d.ch); // 拖动期只更新受影响行,不跑整页(T33)
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
            requestRowRender(d.ch); // 同上
            return;
        }
        const next = quantize(
            WIDTH_RANGE,
            d.start + dy * (WIDTH_RANGE.max - WIDTH_RANGE.min) * fine,
        );
        // 契约 §1.12-§1.14 线程/频率行:拖动期间 setParam 由 UI 侧节流(建议 ≤50 Hz)。
        // 末值必达:落在 20ms 窗内的最后一动记入 lastVal,endDrag 收尾补发,
        // 否则松手位置与引擎里落的值差最后一格(Reviewer Guide)
        d.lastVal = next;
        const t = Date.now();
        if (t - d.lastSent < 20) return;
        d.lastSent = t;
        d.lastSentVal = next;
        sendParam(d.id, next, d.ch); // 拖动期按行增量(T33)
    }

    function endDrag() {
        const d = local.drag;
        if (!d) return;
        local.drag = null;
        if (d.kind === "width") {
            local.gesture = null;
            // 节流窗吞掉的末值在 end 前补发(begin…set*…end 序不变)
            if (d.lastVal !== undefined && d.lastVal !== d.lastSentVal)
                sendParam(d.id, d.lastVal);
            call("endParamGesture", d.id);
            return;
        }
        // pan / vol:**松手才发一次** setTrackManual(手动接管通道入撤销栈,逐帧发会灌满撤销栈;
        // [J85] 冻结通道不入栈,但逐帧写参数面同样是白费的宿主往返,一并适用;
        // 口径同契约 §1.22 段边界「拖拽释放才发」)。
        // **零位移不发**:manualEcho 有意留到 §2.8 回推才清,原地单击会把留存的乐观值
        // 原样再写一遍 —— 同值重写段表 + 撤销栈白多一步。
        if (!d.moved) return;
        const v = local.manualEcho.get(manualKey(d.ch, d.kind));
        if (v !== undefined) sendManual(d.ch, d.kind, v);
    }

    /** pointercancel:中止不提交。width 仍要 endParamGesture 收束(契约 §1.14
     *  begin 必配 end);pan/vol 丢弃本次乐观值(回到引擎值),不 sendManual。 */
    function cancelDrag() {
        const d = local.drag;
        if (!d) return;
        local.drag = null;
        if (d.kind === "width") {
            local.gesture = null;
            // 「中止不提交」对 width 同样成立:拖动期已发过中间值,
            // 收束前回滚到抓握值(与 pan/vol 的丢弃语义对齐;pr-agent)
            if (d.lastSentVal !== undefined && d.lastSentVal !== d.start)
                sendParam(d.id, d.start);
            call("endParamGesture", d.id);
            return;
        }
        const k = manualKey(d.ch, d.kind);
        local.manualEcho.delete(k);
        // 同键的滚轮/键盘防抖计时器一并取消——否则中止后计时器仍会把
        // 未确认的过期值 sendManual 进引擎(持久评审)
        clearTimeout(local.manualTimers.get(k));
        local.manualTimers.delete(k);
        requestRender();
    }

    function currentVolDb(ch) {
        const row = rowOf(ch);
        return readManual(ch, "vol", row ? row.volDb : VOL_RANGE.def);
    }

    function currentPan(ch) {
        const row = rowOf(ch);
        return readManual(ch, "pan", row ? row.pan : PAN_RANGE.def);
    }

    /**
     * 单行模型(T33 性能批)。改造前是 `rowsFromStore(getStore())[ch-1]` ——
     * 拖动/滚轮的**每一步进**都把 15 行连同跨行公共量整套重建一遍;现在
     * 只算被问到的那一行(公共量走 rowContext,与 ch 无关的部分照样只算一次)。
     */
    function rowOf(ch) {
        return rowFromStore(getStore(), ch);
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
        // [SL-230] 持久入口的三枚钮:同样**不受整行 disabled 约束**(它们是撤下确认的出口),
        // 真正的写面守卫在 doReidentify 里(isWriteBlocked / isRowDead 双守卫)。
        if (part === "restore-auto") {
            // 锁定态的钮是 data-disabled=1 + tooltip 说明原因(口径同 SL-193 的灰钮):
            // clearManual 对 locked 段免疫,给它一次「展开确认再什么都不发生」更糟。
            const btn = (local.rows.get(ch) || {}).restoreAuto;
            if (btn && btn.getAttribute("data-disabled") === "1") return;
            local.restoreConfirm = ch; // 一次一条
            return requestRender();
        }
        if (part === "restore-auto-cancel") {
            local.restoreConfirm = 0;
            return requestRender();
        }
        if (part === "restore-auto-ok") {
            local.restoreConfirm = 0;
            doReidentify(ch); // 与解冻提示条逐字同一条路(analyze + clearManual)
            return;
        }
        if (part === "manualdriven-dismiss") {
            // 这条链的终止出口:撤下提示,不发任何写面调用。
            local.unfreezeHint.delete(ch);
            return requestRender();
        }
        if (part === "manualdriven-reidentify") {
            // 只读观察/无时间线不得发起重析(analyze 是写面;持久评审)
            if (isWriteBlocked()) return;
            // 锁定档:与行内触发钮同一条守卫(渲染面已置灰,这里再复检一次入口)
            const rb = (local.rows.get(ch) || {}).manualdrivenReidentify;
            if (rb && rb.getAttribute("data-disabled") === "1") return;
            // 提示正文本身即二次确认(见模板注释),这里直接发起 —— 不再开第二道确认条。
            return doReidentify(ch);
        }
        if (isRowDead(ch) || isWriteBlocked()) return;
        if (Object.prototype.hasOwnProperty.call(CONFIG_TOGGLES, part)) {
            return toggleConfig(ch, CONFIG_TOGGLES[part]);
        }
        // 配对自定义下拉:触发钮开合;选项 = 选中 pair_id 并收起(契约 §1.15,0|1..7)
        if (part === "pair") {
            // 该行确认条未决时不开面板(两浮层同挂 top:100%,开了会盖住确认文案)
            if (local.confirm && local.confirm.ch === ch) return;
            return openPairPanel(local.pairOpen === ch ? 0 : ch);
        }
        if (part.startsWith("pair-opt-")) {
            const raw = Number(part.slice(9)) || 0;
            const v = Math.min(PAIR_LETTERS.length, Math.max(0, raw));
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
        // cancel ≠ up:手势被系统中止(Esc/触控板滚动接管)不得把拖了一半的值
        // 提交进引擎+撤销栈——丢弃本次拖动并回退乐观值(pr-agent Reviewer Guide)
        body.addEventListener("pointercancel", cancelDrag);

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
            const v = Math.min(PAIR_LETTERS.length, Math.max(0, cur + dir));
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
            "restore-auto-row",
            "restore-auto-text",
            "restore-auto",
            "restore-auto-cancel",
            "restore-auto-ok",
            "manualdriven-reidentify",
            "manualdriven-dismiss",
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
        // 空闲零 rAF(T33):弹道循环在无可见变化 / 本页不在前台时自停;
        // 回到前台由这一句重新起帧(start() 在循环已跑时是空操作)。
        if (isPanelActive()) meters.start();
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
        const st = getStore();
        const chans = (st.state || {}).channels || [];
        show(multiLeadBadge, leadLockCount(chans) >= 2);
        // 布防 badge②(三处之一,05 行 300 ①;点击跳转在 app.js 接的)。
        // PR#64 评审【建议】3:此前只有 app.js 的点击接线与 CSS,从没有人 show 它,
        // 而 HTML 里 data-t="wave.recaptureArmed" 又是带 {x}{y}{n} 的长式 ——
        // 一旦显示就是裸占位符。改为在这里按 §2.1 `recapture` 显隐 + fmt 灌值。
        if (recaptureBadge) {
            const rec = (st.state || {}).recapture || null;
            const armed = !!(rec && rec.armed);
            show(recaptureBadge, armed);
            if (armed) {
                const mask = Math.trunc(num(rec.tracksMask, 0));
                let cnt = 0;
                for (let b = 0; b < CHANNEL_COUNT; b++) {
                    if (mask & (1 << b)) cnt++;
                }
                text(
                    recaptureBadge,
                    fmt(t["wave.recaptureArmed"], {
                        x: secondsToTimecode(num(rec.startS, 0)),
                        y: secondsToTimecode(num(rec.endS, 0)),
                        n: cnt,
                    }),
                );
            }
        }
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

    /**
     * 一帧的渲染上下文:跨行公共量(rowContext)+ 写闸 + hostEcho 灰显判定。
     * 整页 renderRows 与按行增量 flushDirtyRows **共用同一份**,15 行只算一次。
     */
    function frameCtx(st) {
        const model = rowContext(st);
        return {
            model,
            counts: model.counts,
            active: model.active,
            blocked: isWriteBlocked(),
            // hostEcho 灰显按**批次新鲜度**判定(口径与 Tab1 一致:hostEcho 标志停发后不会
            // 翻回 false,直接用它会让 pan/width/卡箍/冻结永久滞留 55% 透明)。
            echo:
                (st.params || {}).hostEcho &&
                Date.now() - ((st.params || {}).hostEchoAt || 0) <
                    HOST_ECHO_FRESH_MS
                    ? "1"
                    : "0",
        };
    }

    /**
     * 拖动期的**按行增量**(T33 性能批)。改造前:拖一格 pan/vol 就请求一次整页
     * render(header + 横幅 + footer + Tab1 + 15 轨行),而这一格实际只改了**一行**的
     * 一个属性。现在拖动路径只把受影响的 ch 排进队列,下一帧只重投影那一行。
     * 同一帧内多次请求合成一次;rAF 不可用(node / 后台标签)时同步落地。
     */
    function requestRowRender(ch) {
        local.dirtyRows.add(ch);
        if (local.rowRaf) return;
        const run = () => {
            local.rowRaf = 0;
            flushDirtyRows();
        };
        if (typeof requestAnimationFrame === "function") {
            local.rowRaf = requestAnimationFrame(run);
        } else {
            run();
        }
    }

    function flushDirtyRows() {
        if (!local.dirtyRows.size) return;
        const chs = [...local.dirtyRows];
        local.dirtyRows.clear();
        const t = getT() || {};
        const st = getStore();
        const ctx = frameCtx(st);
        for (const ch of chs) syncRow(rowFromStore(st, ch, ctx.model), t, ctx);
    }

    function renderRows(t, st) {
        // 整页刚把 15 行全投影一遍,排队中的单行增量随之作废(合帧句柄留着,
        // 它下一帧只会看到空队列直接返回)。
        local.dirtyRows.clear();
        const ctx = frameCtx(st);
        for (let ch = 1; ch <= CHANNEL_COUNT; ch++)
            syncRow(rowFromStore(st, ch, ctx.model), t, ctx);
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
            // label 直接读 store(channels[].label 已在 demo 构建层本地化;
            // 真实插件路径 = DAW 轨名,不经 mock → 零误伤)。
            const shown = row.label || labelPlaceholder(ch, t);
            text(n.label, shown);
            attr(n.label, "data-placeholder", row.label ? 0 : 1);
            // 轨名被挤窄时靠 `text-overflow: ellipsis` 收场,但截断后名字就不可读了 ——
            // label 单元格固定 150px,而条件角标最多同时挂三件(主唱居中标 26px +
            // 样本不足角标 44px + SL-230 的触发钮 18px),实测最坏档轨名只剩 44px。
            // 补一条 tooltip,截断至少是可恢复的(#148 二轮【建议】4)。
            setTitle(n.label, shown);
        }
        setTitle(n.labelInput, t["tracks.labelEdit"]);
        show(n.leadcenter, !!row.leadCenter);
        setTitle(n.leadcenter, t["master.leadSelectHint"]);
        show(n.lowsample, !!row.low);
        setTitle(n.lowsample, t["lowSample.full"]);
        show(n.recaptureBadge, !!row.recapture);
        // tooltip 走**无占位符**短式:行模型只有布尔 `recapture` 位(rowFromStore
        // 由 recMask 派生),拿不到 {x}{y}{n} —— 灌 `wave.recaptureArmed` 会把字典
        // 原文连同占位符一起写进 title(PR#64 评审【重要】1)。范围/轨数的完整串
        // 在图例行 badge 与 Tab1 Range badge 上,两处都有 fmt 灌值。
        setTitle(n.recaptureBadge, t["wave.recaptureArmedShort"]);

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
        // ---- PRIO / LEAD / 配对 ----
        text(n.priorityVal, String(row.prio));
        attr(n.priorityDec, "data-disabled", dis);
        attr(n.priorityInc, "data-disabled", dis);
        syncToggle(n.leadlock, row.lead, dis);
        syncPair(n, row, t, ctx.counts, dis, ch);

        // ---- 参与性两枚 + 冻结两枚 + ON ----
        syncToggle(n.volexempt, row.volPart, dis);
        syncToggle(n.autopan, row.part, dis);
        syncToggle(n.freezepan, freeze.pan ? 1 : 0, dis);
        syncToggle(n.freezevol, freeze.vol ? 1 : 0, dis);
        attr(n.freezepan, "data-host-echo", ctx.echo);
        attr(n.freezevol, "data-host-echo", ctx.echo);
        syncToggle(n.enable, row.on, dis);

        // ---- 行内确认层 / 解冻提示 ----
        syncConfirm(n, row, t, ch, dis);
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
     * 行内确认条(R3 防误伤:手动首写)与解冻提示(R2)。两者共用行下方那一格,
     * 同一时刻只出一件 —— 确认条是「需要一个决定」,提示是「一条建议 + 它自己的出口」。
     * 单轨重新识别**不再**经确认条(P0-2:两件互为出口会成环),提示条自带执行与终止两个钮。
     */
    // `dis` = 整行的 disabled 位("1"/"0",= srErr 死轨 ∪ 只读观察态),由调用方算好传进来。
    function syncConfirm(n, row, t, ch, dis) {
        const c =
            local.confirm && local.confirm.ch === ch ? local.confirm : null;
        show(n.manualOverwriteConfirm, !!c);
        if (c) {
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
            text(n.manualOverwriteCancel, t["common.cancel"] || "");
        }
        // 解冻提示:该位 1→0 且该轨当前版本曲线仍是单段全时限 user_edited 常值
        const hint = !c && local.unfreezeHint.has(ch) && !!row.manualConst;
        show(n.manualdrivenHint, hint);
        // [SL-230] 常驻「恢复自动」触发钮:只要该轨仍被手动常值驱动就在(行内,不浮)。
        // 与解冻提示条**不再互斥** —— 触发钮不占下一行的地方,提示条出没出都无所谓;
        // 那条自带的「重新识别轨 {n}」仍是同一个动作,两处并存不冲突。
        // 只读观察态(第二个 Output 实例)/ srErr 死轨:`doReidentify` 的写面守卫会直接
        // 静默返回,钮再露出来就又是一枚「点了没反应」—— 正是本卡要去掉的东西。口径与
        // 检查器那份的 `editable = !isWriteBlocked() && !isLaneDead()` 对齐(#148 复审【建议】2)。
        const canRestore = !!row.manualConst && dis !== "1";
        // 锁定的手动常值:clearManual 碰不了它(§1.6 locked 免疫)⇒ 置灰 + tooltip 说清
        // 原因,不给一次「点了什么都不发生」(口径同 SL-193 的灰钮)。
        const lockedConst = !!row.manualConstLocked;
        const lockedTip = t["tracks.restoreAutoLocked"] || "";
        if (n.restoreAuto) {
            show(n.restoreAuto, canRestore);
            if (canRestore) {
                attr(n.restoreAuto, "data-disabled", lockedConst ? 1 : 0);
                const tip = lockedConst
                    ? lockedTip
                    : t["tracks.restoreAuto"] || "";
                attr(n.restoreAuto, "title", tip);
                attr(n.restoreAuto, "aria-label", tip);
                // 置灰只做视觉不够:原生 <button> 的 `data-disabled` 不摘 tab 序、不挡
                // Enter/Space,键盘用户拿到的正是「按了没反应」。不上原生 `disabled` ——
                // 部分平台会连 title tooltip 一起吞掉,那样连原因都说不出来了。
                attr(
                    n.restoreAuto,
                    "aria-disabled",
                    lockedConst ? "true" : "false",
                );
            }
        }
        // 确认浮条:**一次只出一条**(它和解冻提示条同族,都是 absolute/top:100%,
        // 浮在下一行上;两条同时展开会互相盖)。
        const asking = !c && !hint && canRestore && local.restoreConfirm === ch;
        show(n.restoreAutoRow, asking);
        attr(n.row, "data-confirm", c || hint || asking ? 1 : 0);
        if (asking) {
            text(
                n.restoreAutoText,
                fmt(t["tracks.reidentifyConfirm"], { n: tt(ch) }),
            );
            text(n.restoreAutoCancel, t["common.cancel"] || "");
            text(n.restoreAutoOk, t["common.continue"] || "");
        } else if (local.restoreConfirm === ch && (!canRestore || c || hint)) {
            // 手动常值没了(刚恢复成功)⇒ 收起展开态。
            // 被 `c` / `hint` **临时**顶掉时也要收:不收的话那两件一撤,确认浮条自己弹回来、
            // 直接停在「取消 / 继续」上,用户没点过却已经在问他(#148 二轮【建议】1)。
            local.restoreConfirm = 0;
        }
        if (!hint) return;
        // 正文 = 提问 + 「已锁定段保持不变」——「链接即执行」之后,这条就是用户看到的
        // **唯一**一次告知,原先第二道确认条上的免疫说明不能跟着一起消失。
        text(
            n.hintText,
            (t["tracks.manualDrivenHint"] || "") +
                " " +
                (t["tracks.manualDrivenHint.locked"] || ""),
        );
        // 锁定档:这枚钮与上面那枚行内触发钮是**同一个动作**,得给同一个结论。
        // 复审② 撤掉了「触发钮与提示条互斥」,复审③ 又让 native 真的对 locked 段免疫 ——
        // 两下叠加之后,这枚钮在锁定档上点下去 `analyze(mask,{clearManual:true})` 什么都
        // 不会变,而 `doReidentify` 已经先把提示条永久撤掉了,用户拿到的是零反馈。
        // 同屏两个入口对同一件事给出相反结论,被判「可点」的那个才是无效的那个
        // (#148 二轮【重要】)。置灰口径与行内钮逐字相同,只留「知道了」这个出口。
        text(
            n.manualdrivenReidentify,
            fmt(t["tracks.reidentifyOne"], { n: tt(ch) }),
        );
        if (n.manualdrivenReidentify) {
            attr(
                n.manualdrivenReidentify,
                "data-disabled",
                lockedConst ? 1 : 0,
            );
            attr(
                n.manualdrivenReidentify,
                "aria-disabled",
                lockedConst ? "true" : "false",
            );
            attr(
                n.manualdrivenReidentify,
                "title",
                lockedConst ? lockedTip : "",
            );
        }
        text(n.manualdrivenDismiss, t["common.gotIt"] || "");
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
            // 拖动中的那个 (ch,dim) 不清:§2.8 回推可能来自别的轨/别的编辑,
            // 清掉在拖的乐观值会让 endDrag 读到 undefined 静默丢弃最终值
            // (pr-agent 在途竞态②)
            const dragging = (dim) =>
                local.drag && local.drag.ch === c.ch && local.drag.kind === dim;
            // 按事件源分叉(持久评审两轮竞态的合并解):
            //   • reason="trackManual" = 用户自己的写入回推——同轨**另一维度**
            //     排队中的新意图(300ms 防抖)必须存活,只清无待提交的陈旧乐观值;
            //   • 其余 reason(analyze/copy/versionActive/…)= 失效性回推——
            //     排队值落地会重新弄脏刚清好的轨,echo 与计时器一并作废。
            const ownWrite = payload && payload.reason === "trackManual";
            for (const dim of ["pan", "vol"]) {
                if (dragging(dim)) continue;
                const k = manualKey(c.ch, dim);
                if (ownWrite && local.manualTimers.has(k)) continue;
                local.manualEcho.delete(k);
                clearTimeout(local.manualTimers.get(k));
                local.manualTimers.delete(k);
            }
        }
    }

    return { mount, render, onMeters, onParams, onSegments };
}
