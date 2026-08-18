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
//   • **Wave 1(本次)** = 视觉。行五态 / on:0 / 空 label 占位 / 冻结两态 / 配对七色 /
//     mono 轨 width 灰显 …… 一律写成 `data-*`,可在 DevTools 改属性静态验证。
//     占位数据 = 下面的 `WAVE1_SAMPLE`(设计稿 TRACKS 1382-1397 逐行转写),
//     它的 15 行恰好覆盖全部状态样本 —— **Wave 2 接线后整段删除**(同 T31 的 WAVE1_NUMBERS)。
//   • **Wave 2** = 交互。消费 scvb.meters / scvb.params / scvb.state / scvb.conn,
//     上行 setChannelConfig / setTrackManual / gesture 三段式。接线位在本文件以
//     `[T32 Wave 2]` 注释桩标出,电平弹道在 web/output/canvas/meter.js。
// =============================================================================

// =============================================================================
// 一、纯函数与常量(无 DOM;node 侧断言面)
// =============================================================================

/** 通道数 —— 契约 §0.2 第 4 条:`ch` = 1..15(J59)。 */
export const CHANNEL_COUNT = 15;

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

/** 字母 → pair_id(设计稿的 TRACKS 用字母,契约用整数;Wave 1 样本转写用)。 */
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

/** 空 label 的淡色占位(设计稿 1734:`"Track " + 两位轨号`)。 */
export function labelPlaceholder(ch) {
    return "Track " + tt(ch);
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
// Wave 1 占位数据(设计稿 TRACKS 1382-1397 逐行转写)
// -----------------------------------------------------------------------------
// **Wave 2 接线后整段删除**(同 T31:Wave 1 的 WAVE1_NUMBERS 已在 Wave 2 删净)。
// 为什么不用 web/shared/mock-data.js 的 FIFTEEN_TRACKS:那份是 tour demo 的「健康满配」
// 画像(15 轨全活跃、全有名字、全 ON),**覆盖不到本页要静态验收的五态**;设计稿这 15 行
// 才是状态样本表(移植地图 §25):lost/srErr/warn/idle 各一条、on:0 一条、空 label 两条、
// fp/fv 各一条、low 一条、ST 四条、配对 A/B/C 三色各就位。
// 字段沿用设计稿原名:st=stereo 标 / vol·lv·pk = 0..1 行程比例(不是 dB) /
// ex=lead_vol_exempt / part=participate_in_auto_pan / fp·fv=冻结两位 / low=样本不足。
// prettier-ignore
const WAVE1_ROWS = [
    // n   label       st status    pan   w  vol   lv   pk   prio lead ex part pair fp fv on low
    [ 1, "主唱",       0, "active",  -4, 100, .62, .74, .88,  5,  1,  0,  1,  "",  0, 0, 1, 0],
    [ 2, "主唱双",     0, "active",  12, 100, .55, .61, .72,  4,  0,  0,  1,  "A", 0, 0, 1, 0],
    [ 3, "和声 L",     1, "active", -58,  82, .44, .48, .58,  3,  0,  0,  0,  "B", 0, 0, 1, 0],
    [ 4, "和声 R",     1, "active",  58,  82, .44, .46, .57,  3,  0,  0,  0,  "B", 0, 0, 1, 0],
    [ 5, "和声 C",     0, "lost",     0, 100, .40,   0,   0,  3,  0,  0,  1,  "",  0, 0, 1, 0],
    [ 6, "Ad-lib 1",   0, "active", -34, 100, .38, .31, .44,  2,  0,  0,  1,  "",  1, 0, 1, 0],
    [ 7, "Ad-lib 2",   0, "srErr",   34, 100, .38,   0,   0,  2,  0,  0,  1,  "",  0, 0, 1, 0],
    [ 8, "低八度",     0, "active", -18, 100, .34, .28, .39,  2,  0,  1,  1,  "",  0, 0, 1, 0],
    [ 9, "高八度",     0, "warn",    18, 100, .34, .26, .36,  2,  0,  0,  1,  "",  0, 0, 1, 0],
    [10, "群唱 L",     1, "active", -76,  64, .30, .22, .33,  1,  0,  0,  0,  "C", 0, 0, 1, 0],
    [11, "群唱 R",     1, "active",  76,  64, .30, .21, .32,  1,  0,  0,  0,  "C", 0, 0, 1, 0],
    [12, "呼吸轨",     0, "idle",     0, 100, .25,   0,   0,  1,  0,  0,  1,  "",  0, 0, 1, 0],
    [13, "念白",       0, "active",  26, 100, .42, .18, .27,  1,  0,  0,  1,  "",  0, 1, 1, 1],
    [14, "",           0, "idle",     0, 100, .50,   0,   0,  1,  0,  0,  1,  "",  0, 0, 1, 0],
    // 轨 15 的 status 由设计稿的 "idle" 改成 "active":设计稿这一行是 idle + on:0,
    // 而判定链里 idle(.5)优先于 off(.3)—— 照抄的话「ON 关闭 = 整行 .3」这一态
    // 在 15 行样本里根本看不见,静态验收就少一态(差异已记 T32 差异清单)。
    [15, "",           0, "active",   0, 100, .50,   0,   0,  1,  0,  0,  1,  "",  0, 0, 0, 0],
];

export const WAVE1_SAMPLE = Object.freeze(
    WAVE1_ROWS.map((r) =>
        Object.freeze({
            n: r[0],
            label: r[1],
            st: r[2],
            status: r[3],
            pan: r[4],
            w: r[5],
            vol: r[6],
            lv: r[7],
            pk: r[8],
            prio: r[9],
            lead: r[10],
            ex: r[11],
            part: r[12],
            pair: pairIdOf(r[13]),
            fp: r[14],
            fv: r[15],
            on: r[16],
            low: r[17],
            // 时间线失准计数(05 §2.2「琥珀⚠+计数」;设计稿 statusMap.warn 的「失准 ×2」)
            misalign: r[3] === "warn" ? 2 : 0,
            // Wave 2 由 scvb.state.versions[active].name 填;Wave 1 给设计稿的默认名
            version: "V1",
            // Lead Select≠0 的行首居中标记(05 §2.1 ④ 联动);Wave 1 全灭
            leadCenter: 0,
            // 配对满员琥珀标(05 §2.2「某组已满 2 轨」);Wave 1 全灭
            pairFull: 0,
            // 重采集布防目标轨的行首 badge(05 §2.3 ②);Wave 1 全灭
            recapture: 0,
        }),
    ),
);

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
        const aria = c.aria ? ` aria-label="状态" data-t-aria="${c.aria}"` : "";
        const tight = c.tight ? ` data-tight="1"` : "";
        return `<span class="tracks-head__col" role="columnheader" style="width:${c.w}px"${tight}${t}${aria} data-gb="tracks-head-${c.key}"></span>`;
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
             [T32 Wave 2] bridge.setChannelConfig(${ch}, {label}) —— 契约 §1.15 -->
        <input class="tracks-row__label-input" type="text" maxlength="${LABEL_MAX}"
               data-t-aria="tracks.labelEdit" data-gb="${gb("label-input")}" />
        <!-- Lead Select≠0 选中轨的行首居中标记(05 §2.2 主唱锁行 → §2.1 ④) -->
        <span class="tracks-row__leadmark" data-t="tracks.leadCenter" data-gb="${gb("leadcenter")}"${t.leadCenter ? "" : " hidden"}></span>
        <!-- 采集后有效唱段 <1.5s(05 §2.2 R1):角标保短版,全句「样本不足,结果可能不稳」进 tooltip(统筹裁定 B12) -->
        <span class="sc-badge--amber tracks-row__mark" data-t="lowSample" data-gb="${gb("lowsample")}"${t.low ? "" : " hidden"}></span>
        <!-- 重采集布防目标轨的行首 badge(05 §2.3 ②):52px 内塞不下长句,压成琥珀点 + tooltip -->
        <span class="tracks-row__dotmark" data-gb="${gb("recapture-badge")}"${t.recapture ? "" : " hidden"}></span>
      </span>
      <span class="tracks-row__cell" role="cell" style="width:${W.pan}px">
        <span class="sc-knob" data-live="${panLive}" data-disabled="${dead}"
              style="--ang:${panAngleDeg(t.pan)}deg" data-gb="${gb("pan")}"><span class="sc-knob__needle"></span></span>
        <!-- [T32 Wave 2] 冻结态:bridge.setTrackManual(${ch}, "pan", v) —— 契约 §1.16
             (每轨首次拖动前无条件弹行内确认,05 §2.2 R3);未冻结态只读 + tooltip -->
      </span>
      <span class="tracks-row__cell" role="cell" style="width:${W.width}px">
        <span class="sc-knob" data-live="0" data-disabled="${wDis}"
              style="--ang:${widthAngleDeg(t.w)}deg" data-gb="${gb("width")}"><span class="sc-knob__needle"></span></span>
        <!-- [T32 Wave 2] bridge.beginParamGesture / setParam / endParamGesture(v{active}_t${tt(ch)}_width)
             —— 契约 §1.12-§1.14(双击回默认 100) -->
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
               [T32 Wave 2] bridge.setTrackManual(${ch}, "vol", v) —— 契约 §1.16 -->
          <span class="sc-tube__collar" style="--vol:${t.vol * 100}%" data-frozen="${t.fv}" data-disabled="${dead}"
                data-gb="${gb("vol-collar")}"></span>
        </span>
        <span class="sc-badge--amber tracks-row__exempt" data-t="tracks.exemptBadge" data-gb="${gb("volexempt-badge")}"${t.ex ? "" : " hidden"}></span>
        <span class="tracks-row__version" data-gb="${gb("freeze-version")}"${t.fv ? "" : " hidden"}>${esc(t.version)}</span>
      </span>
      <span class="tracks-row__cell sc-stepper tracks-row__prio" role="cell" style="width:${W.prio}px" data-gb="${gb("priority")}">
        <button type="button" data-t-aria="common.decrease" data-gb="${gb("priority-dec")}"${dis}>−</button>
        <span class="sc-stepper__val" data-gb="${gb("priority-val")}">${clampPriority(t.prio)}</span>
        <button type="button" data-t-aria="common.increase" data-gb="${gb("priority-inc")}"${dis}>+</button>
        <!-- [T32 Wave 2] bridge.setChannelConfig(${ch}, {priority}) —— 契约 §1.15(0..10 钳制) -->
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
          <select class="tracks-row__pair-select" data-t-aria="pair" data-gb="${gb("pair")}"${dead ? " disabled" : ""}>
            <option class="tracks-row__pair-none" value="0"></option>
            ${PAIR_LETTERS.map((L, i) => `<option value="${i + 1}">${L}</option>`).join("")}
          </select>
          <!-- 某组已满 2 轨仍可选,行上出琥珀标「配对超员」(05 §2.2);52px 内改用角点 + tooltip -->
          <span class="tracks-row__pair-full" data-gb="${gb("pair-overflow")}"${t.pairFull ? "" : " hidden"}></span>
        </span>
        <!-- [T32 Wave 2] bridge.setChannelConfig(${ch}, {pair_id}) —— 契约 §1.15(0|1..7) -->
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
             [T32 Wave 2] beginParamGesture / setParam / endParamGesture(四态双向映射断言) -->
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
        <!-- [T32 Wave 2] bridge.setChannelConfig(${ch}, {enabled}) —— 契约 §1.15 -->
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
           [T32 Wave 2] bridge.analyze({tracksMask:1<<${ch - 1}}, {clearManual:true}) —— 契约 §1.6 -->
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
 * @param {object|null} opts.bridge     createBridge() 结果(Wave 2 上行用;Wave 1 只存不调)
 * @param {() => object} opts.getStore  取事件仓(Wave 2 的唯一渲染源)
 * @param {() => object} opts.getT      取当前语言字典
 * @param {() => void} [opts.onLocalChange] 本地态改变后请求外壳重渲染
 */
export function createTabTracks(opts) {
    const root = opts.root;
    const getStore = opts.getStore || (() => ({}));
    const getT = opts.getT || (() => ({}));
    // [T32 Wave 2] 上行全部经这里:setChannelConfig / setTrackManual /
    // beginParamGesture / setParam / endParamGesture / analyze(单轨重新识别)。
    const bridge = opts.bridge || null;

    const $ = (gb) => root.querySelector(`[data-gb="${gb}"]`);
    // [T32 Wave 2] panel 上的 data-empty 由「连接轨数 === 0」判定(scvb.conn),
    // 空态与表格互斥的显隐全部由那一个属性驱动(CSS 在 index.html)。
    const panel = $("tab-tracks");
    const head = $("tracks-head");
    const body = $("tracks-body");
    const legend = $("tracks-legend-text");
    const emptyMain = $("tracks-emptystate-main");

    /** Wave 1 的渲染源;Wave 2 换成 store 派生的行模型(见 rowsFromStore 桩)。 */
    function rows() {
        return WAVE1_SAMPLE;
    }

    // [T32 Wave 2] 行模型 = scvb.state.channels(label/priority/lead_lock/lead_vol_exempt/
    //   participate_in_auto_pan/pair_id/enabled/source_channels)+ scvb.conn(状态灯五态)
    //   + scvb.params(pan/vol/width/freeze)+ scvb.meters(lv/pk,经 canvas/meter.js 弹道)。
    //   接上之后删掉 WAVE1_SAMPLE 与上面的 rows()。

    function mount() {
        if (head) head.innerHTML = trackHeadHtml();
        if (body) body.innerHTML = rows().map(trackRowHtml).join("");
        // [T32 Wave 2] 事件委托挂在 body 上(15 行密排不逐行挂监听):
        //   pointerdown → 卡箍/旋钮拖拽三段式;click → toggle/stepper/select/label 编辑。
    }

    /**
     * 渲染 = 只做「字典驱动」的那一半(tooltip / 图例分段 / 占位符填充)。
     * 状态类 data-* 由 mount 时写进模板,Wave 2 再改为按 store 逐帧回写。
     */
    function render() {
        const t = getT() || {};
        renderLegend(t);
        renderEmpty(t);
        renderTitles(t);
    }

    function renderLegend(t) {
        if (!legend) return;
        const segs = legendSegments(t["tracks.colLegend"]);
        if (!segs.length) return;
        // 设计稿 626:三处 <strong>(font-weight 600 + --txt-min),分隔符是「·」+ 全角空格。
        const frag = document.createDocumentFragment();
        segs.forEach((seg, i) => {
            if (i > 0) frag.appendChild(document.createTextNode(" ·　"));
            if (seg.term) {
                const b = document.createElement("strong");
                b.textContent = seg.term;
                frag.appendChild(b);
            }
            frag.appendChild(document.createTextNode(seg.rest));
        });
        legend.replaceChildren(frag);
    }

    function renderEmpty(t) {
        // 空态主句是**组号变体**(05 §2.2:句中出现两次 {g})。
        // [T32 Wave 2] data-empty 由「连接轨数 === 0」判定(scvb.conn),现由 index.html 静态置 0。
        if (!emptyMain) return;
        const raw = t["tracks.emptyGroup"];
        if (typeof raw !== "string") return;
        const s = getStore();
        const gid = ((s.state || {}).group_id || 1) - 1;
        const letter = "ABCDEFGH"[gid < 0 || gid > 7 ? 0 : gid];
        emptyMain.textContent = raw.split("{g}").join(letter);
    }

    /** tooltip 一律 title:词条为空就移除(空 title 在部分宿主 WebView 里仍弹空气泡)。 */
    function setTitle(node, text) {
        if (!node) return;
        if (text) node.setAttribute("title", text);
        else node.removeAttribute("title");
    }

    function renderTitles(t) {
        for (const row of rows()) {
            const ch = row.n;
            const q = (suffix) => $(`tracks-row-${ch}-${suffix}`);

            // 状态灯:五态 tone/pulse + hover tooltip(05 §2.2 状态灯行)
            const vis = statusVisual(row.status);
            const light = q("statuslight");
            if (light) {
                light.setAttribute("data-tone", vis.tone);
                light.setAttribute("data-pulse", vis.pulse ? "1" : "0");
                // 失准态把计数并进 tooltip(设计稿只有琥珀灯、无可见计数)
                const raw = t[vis.key];
                setTitle(
                    light,
                    typeof raw === "string"
                        ? raw.split("{n}").join(String(row.misalign || 2))
                        : "",
                );
            }

            // pan 旋钮:未冻结 = 自动态,tooltip「自动模式——由分析曲线驱动」(05 §2.2)
            setTitle(q("pan"), row.fp ? "" : t["tracks.panAutoHint"]);
            // width 旋钮:mono 轨 v1 no-op(05 §2.2)
            setTitle(q("width"), row.st ? "" : t["tracks.monoWidthNoop"]);
            // 「样本不足」角标:短版在角标,全句进 tooltip(统筹裁定 B12)
            setTitle(q("lowsample"), t["lowSample.full"]);
            // 豁免角标 / 居中标记 / 配对超员 / 重采集布防:短标在视觉,全称进 tooltip
            setTitle(q("volexempt-badge"), t.leadVolExempt);
            setTitle(q("leadcenter"), t["master.leadSelectHint"]);
            setTitle(q("pair-overflow"), t["tracks.pairOverflow"]);
            setTitle(q("recapture-badge"), t["wave.recaptureArmed"]);
            // 冻结生效版本(J65:vol 冻结后手动值属当前版本)
            const ver = q("freeze-version");
            if (ver) {
                const raw = t["tracks.freezeVersion"];
                setTitle(
                    ver,
                    typeof raw === "string"
                        ? raw.split("{v}").join(row.version)
                        : "",
                );
            }
            // 配对下拉的「无」选项 + 单轨重新识别入口(带轨号的动态文案)
            const sel = q("pair");
            if (sel) {
                const none = sel.querySelector(".tracks-row__pair-none");
                if (none && typeof t["tracks.pairNone"] === "string") {
                    none.textContent = t["tracks.pairNone"];
                }
                sel.value = String(row.pair);
                // chip 上的字:无配对时显示「无」,有配对时显示 A–G(设计稿 1768 pairText)
                const text = q("pair-text");
                if (
                    text &&
                    !row.pair &&
                    typeof t["tracks.pairNone"] === "string"
                ) {
                    text.textContent = t["tracks.pairNone"];
                }
            }
            const relink = q("manualdriven-reidentify");
            if (relink && typeof t["tracks.reidentifyOne"] === "string") {
                relink.textContent = t["tracks.reidentifyOne"]
                    .split("{n}")
                    .join(tt(ch));
            }
            // 空 label 的行内编辑框首帧值(占位不进 value —— 提交空串才是「清空 label」)
            const input = q("label-input");
            if (input && input.value === "") input.value = row.label;
        }
    }

    // [T32 Wave 2] 事件入口(app.js 订阅后转发):
    //   onMeters(m) → canvas/meter.js 的 rAF 弹道 → 每行写 --lv/--pk/data-alert
    //   onParams(p) → 15 轨 pan/vol/width/freeze 跟随(hostEcho 灰显复用 T31 批次新鲜度)
    //   onSegments(s) → 解冻提示的「单段全时限 user_edited 常值」判定
    function onMeters() {}

    function onParams() {}

    function onSegments() {}

    return { mount, render, onMeters, onParams, onSegments };
}
