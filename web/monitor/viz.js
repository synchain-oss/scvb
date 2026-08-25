// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Monitor · viz 段投影(纯函数;T46 / [J75] C)
// -----------------------------------------------------------------------------
// 本文件把 **viz 事件的载荷**翻译成两张图各自要的形状:
//   `scvb.viz` ──► vizSeries()     ──► trajectory-chart.js 的 getSeries()
//              ├─► vizDistRows()   ──► distribution-chart.js 的 distBarsHtml()
//              └─► vizLegendRows() ──► distribution-chart.js 的 legendItemsHtml()
// 无 DOM、无 store、无 i18n —— node 可直接 import 断言
// (`web-preview/tests/smoke-monitor.mjs`)。DOM 接线在 `./app.js`。
//
// =============================================================================
// **viz 事件形状(临时;待与 T44/T45 接口说明对表)**
// =============================================================================
// 段本体是 `SynchainSCVB.v1.g{G}.viz`(J75 C:magic/abi 头 + 降采样 pan lanes +
// 分段 activity 位图 + playhead + 轨色索引),T44 开卡时才定稿(ipc v1.6 修宪)。
// 在那之前,本卡按段的**语义**自拟一个等价的 JSON 投影,并刻意保留段的骨架形状 ——
// 位图就写成 u32 数组、lanes 就写成定长采样数组,而不是拍脑袋换成「一段一条记录」
// 的对象数组。这样对表时讨论的是**字段名与单位**,不是「整个数据模型选错了」。
//
//   {
//     magic: "SCVB",        // 段头 magic(真段里是 u32;此处用可读串,对表时换)
//     abi: 1,               // 段布局 abi。**高于本机即停止读取**(见 vizAccepts)
//     seq: 12345,           // 发布序号,单调递增。停更判据看它(见 app.js stalled)
//     groupId: 1,           // 1..8。回显「你现在看的是哪个组」,与请求值不符即忽略
//     online: true,         // 段 attach 成功 = 该组有 Output 在跑;false ⇒ 空态
//     durationS: 300,       // 工程时间线全长(秒)
//     slotCount: 600,       // 降采样格数 = lanes/activity 的长度
//     slotS: 0.5,           // 每格秒数(= durationS / slotCount,冗余给出免得两边各算)
//     playheadS: 42.31,     // 段里的播放头(低频)。**平滑的那一路走 scvb.playhead**
//     channels: [{
//       ch: 1,              // 1..15
//       label: "主唱",      // 用户数据,渲染时必须转义
//       stereo: false,      // source_channels === 2 ⇒ 画 pan 中心线(J75 A)
//       colorIndex: 1,      // **轨色索引**(J75 C 逐字)。默认 = ch,但独立成字段:
//                           //   将来若允许用户改配色,轨号与色号就会分家
//       panNow: -4,         // 当前最终打印 pan(角度域 −100..100)—— 分布图横位
//       volDb: -1.7,        // 当前音量(−24..+12 dB)          —— 分布图柱高
//       widthPct: 100,      // 当前 width(0..100)             —— 分布图张开横线
//       lead: false,        // lead_lock ⇒ 柱顶绿帽
//       pan: [ … slotCount 个 ],   // 降采样 pan lanes(未覆盖格的值无意义,别读)
//       activity: [ u32, … ]       // 分段 activity 位图:bit i(LSB 起)= 第 i 格有覆盖
//     }]
//   }
//
// **对表时要向 T44 明确要到的三条**(J75 C 的字段清单里没写,但两张图缺了就画不出):
//   ① `volDb` 与 `widthPct` —— 分布图的柱高与张开横线只能来自它们(J75 C 要求
//      Monitor 的分布图「同 Tab1 规格」,而 Tab1 那三个值来自 `scvb.params`,
//      Monitor 没有 params 通路);
//   ② `label` —— 图例要轨名(否则只剩两位轨号,15 条线认不出谁是谁);
//   ③ `stereo` —— 图例 ST 角标 + J75 A「立体声轨画 pan 中心线」的身份位。
// 若 T44 定稿里没有它们,退路是把分布图降级成「只有 pan 的散点」并在 PR 里改判据 ——
// 但那不符合 J75 C 的「同 Tab1 规格」,应优先在段里补齐。
// =============================================================================

import { PAN_MAX, PAN_MIN } from "../shared/trajectory-chart.js";
import { TRACK_COLOR_COUNT } from "../shared/track-colors.js";

/** 段头 magic(与 C++ 侧同值;对不上即整帧丢弃)。 */
export const VIZ_MAGIC = "SCVB";

/**
 * 本机认得的 viz 段布局 abi。
 * **只拒高不拒低**:高于本机 = 对端比我新,字段可能整体挪位,读了就是读错(与契约
 * §5.1 `newerState` 的拒载语义同族);低于或等于本机 = 我认得,照读。
 */
export const VIZ_ABI = 1;

/** 组号 → 显示字母(契约 §0.2:g = 1..8,UI 显示 A-H)。 */
export const GROUP_LETTERS = Object.freeze([
    "A",
    "B",
    "C",
    "D",
    "E",
    "F",
    "G",
    "H",
]);

/** 通道数(契约 §0.2 第 4 条:ch = 1..15,J59)。 */
export const CHANNEL_COUNT = 15;

/**
 * 兜底时间线长度(秒)。与 `tab-master.js` 的 `CHART_FALLBACK_DURATION_S` 同值 ——
 * 拿不到任何长度证据时视口不该塌到 0(那样时间轴一格刻度都画不出来)。
 */
export const VIZ_FALLBACK_DURATION_S = 300;

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

function clamp(lo, hi, v) {
    return v < lo ? lo : v > hi ? hi : v;
}

/** `scvb.groups` 位图 → 某组是否在线(事件缺失时传 0,绿点全灭、零报错)。 */
export function groupOnline(bitmap, g) {
    return ((Number(bitmap) || 0) >> (g - 1)) & 1 ? 1 : 0;
}

/**
 * activity 位图的第 i 格是否有分段覆盖。
 *
 * 位序 = **LSB 起、每字 32 格**(`word = i >>> 5`,`bit = i & 31`),与 C++ 侧
 * 逐字同序 —— 位序是这类位图最容易两边写反的地方,且写反了图**照画**,只是
 * 断线位置整体错开 32 格的倍数,肉眼几乎看不出来。故只此一处实现,两张图共用。
 *
 * 越界 / 脏数据一律回 false(读不到覆盖 = 不画线,比画一条假线安全)。
 */
export function slotActive(activity, i) {
    if (!Array.isArray(activity) || !Number.isInteger(i) || i < 0) return false;
    const word = activity[i >>> 5];
    if (!Number.isFinite(word)) return false;
    return ((word >>> (i & 31)) & 1) === 1;
}

/**
 * 这一帧 viz 能不能读。
 *
 * @returns {{ok:boolean, reason:"" | "shape" | "magic" | "abi" | "offline",
 *            abi:number}}
 *   `reason`:
 *     shape   —— 不是对象 / 缺 channels(桥抖动、首帧未到)⇒ 静默忽略;
 *     magic   —— 段头对不上 ⇒ 整帧丢弃(读错段比读不到段危险得多);
 *     abi     —— 段比本机新 ⇒ **停止读取**并挂红横幅(monitor.abiMismatch);
 *     offline —— 段 attach 不上 = 该组没有 Output ⇒ 空态(J75 C),不是错误。
 */
export function vizAccepts(viz) {
    if (!viz || typeof viz !== "object") {
        return { ok: false, reason: "shape", abi: 0 };
    }
    if (viz.magic !== VIZ_MAGIC) {
        return { ok: false, reason: "magic", abi: num(viz.abi, 0) };
    }
    const abi = num(viz.abi, 0);
    if (abi > VIZ_ABI) return { ok: false, reason: "abi", abi };
    if (viz.online === false) return { ok: false, reason: "offline", abi };
    if (!Array.isArray(viz.channels)) {
        return { ok: false, reason: "shape", abi };
    }
    return { ok: true, reason: "", abi };
}

/** 时间线全长(秒);拿不到证据就压在兜底值上,不塌到 0。 */
export function vizDurationS(viz) {
    const d = num(viz && viz.durationS, 0);
    return d > 0 ? d : VIZ_FALLBACK_DURATION_S;
}

/** 每格秒数;缺省或非法时由 durationS / slotCount 反算。 */
export function vizSlotS(viz) {
    const s = num(viz && viz.slotS, 0);
    if (s > 0) return s;
    const n = num(viz && viz.slotCount, 0);
    return n > 0 ? vizDurationS(viz) / n : 0;
}

/** 轨号闸:1..15 之外一律不要(脏数据画出来的线,图例里找不到对应行)。 */
function validCh(ch) {
    return Number.isFinite(ch) && ch >= 1 && ch <= CHANNEL_COUNT;
}

/**
 * 轨色索引 → 传给两张图的「轨号」。
 *
 * 两件消费方(`trackIndex()` / `trackColorVar()`)都按轨号取色,故这里把
 * `colorIndex` 归一成 1..15 后当轨号交出去。默认 `colorIndex === ch`,
 * 缺字段就退回 ch —— 缺色号不该让这条线消失,顶多是颜色不是「指定的那个」。
 */
export function colorChOf(c) {
    const idx = Number(c && c.colorIndex);
    if (Number.isInteger(idx) && idx >= 1 && idx <= TRACK_COLOR_COUNT) {
        return idx;
    }
    return c && validCh(c.ch) ? c.ch : 1;
}

/**
 * viz 的降采样 lanes + activity 位图 → 轨迹图的折线段组。
 *
 * **断线判据在这里**,且与 Tab1 的 `runsOfSegments()` 是同一条语义的两种表达:
 * 那边看的是「段表里有没有段」,这边看的是「activity 位图里这一格是不是 1」——
 * 两者都是 J75 A 的「无分段覆盖的区间不画线」。连续为 1 的格聚成一条折线,
 * 中间只要有一格是 0 就另起一条。
 *
 * 每格画成一个**水平台阶**(`[t, t+slotS]` 两点同 pan),格与格之间用竖直连接线 ——
 * 与 Tab1 侧逐字同款:引擎按 ramp 过渡,80ms 在任何缩放档下都不足 1px,画斜线
 * 会在放到最大时变成一条**假的**渐变轨迹。
 *
 * @param {object} viz `scvb.viz` 载荷(调用方已过 `vizAccepts`)
 * @returns {{ch:number, stereo:boolean, runs:{tS:number,pan:number}[][]}[]}
 */
export function vizSeries(viz) {
    const slotS = vizSlotS(viz);
    const out = [];
    if (!(slotS > 0)) return out;
    for (const c of (viz && viz.channels) || []) {
        if (!c || !validCh(c.ch)) continue;
        const lanes = Array.isArray(c.pan) ? c.pan : [];
        const n = Math.min(num(viz.slotCount, lanes.length), lanes.length);
        const runs = [];
        let cur = null;
        for (let i = 0; i < n; i++) {
            if (!slotActive(c.activity, i)) {
                cur = null; // 断口:下一格有覆盖时另起一条
                continue;
            }
            const pan = clamp(PAN_MIN, PAN_MAX, num(lanes[i], 0));
            if (!cur) {
                cur = [];
                runs.push(cur);
            }
            cur.push({ tS: i * slotS, pan }, { tS: (i + 1) * slotS, pan });
        }
        if (runs.length === 0) continue;
        out.push({ ch: colorChOf(c), stereo: c.stereo === true, runs });
    }
    // 按轨号升序:色板按轨号固定映射,顺序不许乱(与 Tab1 的 trajectorySeries 同款)
    return out.sort((a, b) => a.ch - b.ch);
}

/**
 * viz → 分布图的 rows(喂 `distBarsHtml`)。
 *
 * 只画**当前真的在出声/有值**的轨:`volDb` 缺失的轨不画 —— 与 Tab1「空闲轨无参数值,
 * vol=0 会被画成居中高的幽灵柱」是同一条纪律,只是这边的判据落在 viz 的字段有无上。
 */
export function vizDistRows(viz) {
    const out = [];
    for (const c of (viz && viz.channels) || []) {
        if (!c || !validCh(c.ch)) continue;
        if (!Number.isFinite(c.volDb)) continue;
        out.push({
            ch: colorChOf(c),
            pan: num(c.panNow, 0),
            volDb: c.volDb,
            widthPct: num(c.widthPct, 100),
            stereo: c.stereo === true,
            lead: c.lead === true,
        });
    }
    return out.sort((a, b) => a.ch - b.ch);
}

/**
 * viz → 图例行(喂 `legendItemsHtml`)。
 *
 * 行集 = **两张图里任意一张真的画了的那些轨**的并集。Tab1 的图例跟着当前视图走
 * (两视图二选一);Monitor 两张图**同屏**,跟着其中一张就必然出现「图例里有它、
 * 另一张图上找不到它」或反之。取并集才让「图例里有它 ⇒ 屏幕上找得到它」成立。
 */
export function vizLegendRows(viz) {
    const seen = new Map();
    const add = (c) => {
        const ch = colorChOf(c);
        if (seen.has(ch)) return;
        seen.set(ch, {
            ch,
            label: String((c && c.label) || ""),
            stereo: c && c.stereo === true,
        });
    };
    const drawn = new Set(vizSeries(viz).map((s) => s.ch));
    const dist = new Set(vizDistRows(viz).map((r) => r.ch));
    for (const c of (viz && viz.channels) || []) {
        if (!c || !validCh(c.ch)) continue;
        const ch = colorChOf(c);
        if (drawn.has(ch) || dist.has(ch)) add(c);
    }
    return [...seen.values()].sort((a, b) => a.ch - b.ch);
}
