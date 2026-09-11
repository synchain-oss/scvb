// SPDX-License-Identifier: GPL-3.0-or-later
// [SL-396 复审②] `analyzeRefusalNote` 的**唯一**一份实现在 `web/shared/analyze-note.js`
// (波形与分段页那份也用它),这里 import 同一份 —— 内联复刻「同一个判断各存一份」正是这一族
// 缺陷反复复发的形状(见 host-echo.js 的同款头注)。
// ⚠ 第一版写成 `from "./tab-wave.js"`:tab-wave 是**工厂函数**,内部函数 export 不出去,
// 那是一个 `SyntaxError`(整页起不来)。别再那样指。
import { analyzeRefusalNote } from "../shared/analyze-note.js";
// =============================================================================
// SCVB Output · Tab4「设置」—— 状态机与桥接线(T35 交付物)。
// -----------------------------------------------------------------------------
// 职责边界:
//   • 本文件只管 **Tab4**(缩放 / 语言 / 版本 / 说明块 / 存储状态 / 诊断 + J69 两设置块)。
//     外壳(header / 横幅 / footer / 缩放状态机 / 引导页 / tab 路由)在 web/output/app.js。
//   • 两段导出:**纯函数**(无 DOM,node 可直接 import 断言,见
//     web-preview/tests/smoke-tab4-settings.mjs)+ createTabSettings()(DOM 接线)。
//     模块顶层零副作用、零 document 触碰,否则 check-i18n / 冒烟脚本导入即炸。
//
// 消费(契约 §2,逐字):
//   scvb.state    → analysis.loudness_mode / center_slot_policy(两设置块白底高亮)、
//                   features{embedded,bytes}(存储状态)、config_seq(诊断 SEQ)、ui.scale
//   snapshot      → version{plugin,abi}(版本号)、session_guid(存储 GUID)
//   scvb.conn     → channels[].heartbeatAgeMs / misalignCount + generation(诊断表)
//
// 上行(契约 §1,逐字):
//   setAnalysisConfig({loudness_mode?, center_slot_policy?})(§1.21,J69 唯一写入口,
//     零 gesture、不入撤销栈、改后不自动重分析);
//   「重看引导」仅保留 data-tour="review" 锚点(tour 本体归 T36b)。
//
// 九条红字纪律(12 §3.4 / 05 §5,不可协商):guide.rule1..9 由 scripts/gen-hard-rules.mjs
// 从 docs/USER_GUIDE.zh-CN.md#硬约束 生成写入 web/shared/i18n.js,任何位置禁止手抄;
// 本文件只**读取**这九个 key 并排版(前 3 条摘要 + 「查看全部九条」块内展开),不写条目文本。
// 生成器与真源归 T39b —— 字典未生成时渲染醒目占位注记(与 app.js 引导页同款降级)。
// =============================================================================

// =============================================================================
// 一、纯函数与常量(无 DOM;node 侧断言面)
// =============================================================================

/**
 * 第二响度指标三档(契约 §1.21;义项/数学 = 02 §4.3,J69)。
 * value = 桥面枚举字符串(以冻结契约 SCVB_CONTRACT.md §1.21 为准 —— 02/03 内部拼作
 * k_integrated / even_offset,桥面真值拼作 kw_integrated / even_spread,UI 只认桥面)。
 */
export const LOUDNESS_MODES = Object.freeze([
    { value: "kw_integrated", key: "set.loudnessMode.opt.kw_integrated" },
    { value: "rms", key: "set.loudnessMode.opt.rms" },
    { value: "peak_dbfs", key: "set.loudnessMode.opt.peak_dbfs" },
]);

/** 中心槽策略三档(契约 §1.21;义项/数学 = 02 §5.6,J69;默认 = priority_queue)。 */
export const CENTER_SLOT_POLICIES = Object.freeze([
    { value: "priority_queue", key: "set.centerSlot.opt.priority_queue" },
    { value: "lead_exclusive", key: "set.centerSlot.opt.lead_exclusive" },
    { value: "even_spread", key: "set.centerSlot.opt.even_spread" },
]);

/** J69 两字段出厂默认(契约 §1.21:「默认 kw_integrated / priority_queue」)。 */
export const ANALYSIS_CONFIG_DEFAULTS = Object.freeze({
    loudness_mode: "kw_integrated",
    center_slot_policy: "priority_queue",
});

/** 九条红字 key(生成物,禁止手抄;见文件头纪律)。 */
export const GUIDE_RULE_KEYS = Object.freeze(
    Array.from({ length: 9 }, (_, i) => "guide.rule" + (i + 1)),
);

/** 特征存储外置阈值(ADR-007:>8MB 自动转外部文件;契约 §1.1 features.bytes)。 */
export const FEATURES_EXTERNAL_BYTES = 8 * 1024 * 1024;

/** 心跳年龄「无数据」哨兵(契约 §2.3:slotState=0/从未心跳 → 0xFFFFFFFF)。 */
export const HEARTBEAT_AGE_NONE = 0xffffffff;

/**
 * 心跳年龄 → 诊断表显示串(设计稿 t4Diag:空闲 "—"、陈旧 "4.2s"、活跃 "0.08s")。
 * @param {number|undefined} ms conn.channels[].heartbeatAgeMs
 */
export function heartbeatAgeText(ms) {
    if (ms == null || ms >= HEARTBEAT_AGE_NONE) return "—";
    const s = Number(ms) / 1000;
    if (!Number.isFinite(s) || s < 0) return "—";
    if (s < 1) return s.toFixed(2) + "s";
    return s.toFixed(1) + "s";
}

/** 字节数 → 一位小数 MB 串(设计稿「内嵌于工程(3.4 MB)」同族;契约 features.bytes)。 */
export function formatMegabytes(bytes) {
    const mb = (Number(bytes) || 0) / (1024 * 1024);
    return (Math.round(mb * 10) / 10).toFixed(1);
}

/** 版本号 mono 串(契约 §1.1 snapshot.version:{plugin,abi};同 footer「v0.1.0 · abi 1」)。 */
export function versionString(snapshot) {
    const v = snapshot && snapshot.version;
    if (!v || typeof v.plugin !== "string") return "";
    return "v" + v.plugin + " · abi " + v.abi;
}

/**
 * 存储状态行模型(契约 §1.1 features:{embedded,bytes})。
 * @returns {{embedded:boolean, bytes:number, external:boolean}}
 */
export function storageOf(state) {
    const f = (state && state.features) || {};
    const bytes = Number(f.bytes) || 0;
    const embedded = f.embedded !== false;
    return {
        embedded,
        bytes,
        external: !embedded || bytes > FEATURES_EXTERNAL_BYTES,
    };
}

/** J69 两设置项当前值(读 state.analysis,缺字段回落默认;契约 §1.1/§2.1)。 */
export function analysisConfigOf(state) {
    const a = (state && state.analysis) || {};
    return {
        loudness_mode:
            a.loudness_mode || ANALYSIS_CONFIG_DEFAULTS.loudness_mode,
        center_slot_policy:
            a.center_slot_policy || ANALYSIS_CONFIG_DEFAULTS.center_slot_policy,
    };
}

/**
 * [SL-279] 「上次全量分析所用」的那一份(契约 §1.1/§2.1 载荷的 `analysis.applied`,
 * 语义与前移条件见 §1.21;落盘见 STATE_SCHEMA §三 CFGS)。
 * 缺字段时回落到**当前值**而不是默认值 —— 那正是 native 侧两级长度回退的语义,
 * 两边取同一个口径,旧插件(不发 applied)下 stale 恒假、行为与本卡之前一致。
 */
export function appliedAnalysisConfigOf(state) {
    const a = (state && state.analysis) || {};
    const applied = a.applied || {};
    const cur = analysisConfigOf(state);
    return {
        loudness_mode: applied.loudness_mode || cur.loudness_mode,
        center_slot_policy:
            applied.center_slot_policy || cur.center_slot_policy,
    };
}

/**
 * [SL-354] 上面那个回落**看不出来**:回落之后 `applied.x === cur.x`,派生的 stale 恒假,
 * 与「基线真的等于当前值」逐字节一样。渲染上这没问题(旧插件下徽标不亮,正是设计),
 * 但拿它去做**破坏性判断**(清开闸位、关掉已经弹出来的框)就是把一次「读不到」当成
 * 一次「读到了相等」—— 与用户报的 ②「弹窗闪一下就没了」同一族(那一条的形态是过期
 * 快照,这一条的形态是缺字段快照,两条都会让 stale 假装归假)。
 *
 * 本函数因此只回答一件事:**这份 state 自己带了 `applied` 那两个字段吗**。
 * 两个都要有:弹窗判据现在是「响度 || 中央槽」,只带一半时另一半仍是回落值。
 * 读的是 store 里合并后的 state,不是单帧 —— `full:false` 增量帧走深合并,上一帧的
 * `applied` 会留下来;能把它整个抹掉的只有 `full:true` 全量帧(app.js 的 scvb.state
 * 订阅对全量帧做的是**整体替换**、对增量帧才做深合并)。
 *
 * 今天的真桥恒发这两个字段(`OutputEditor::buildStateSubtree` 里那一段无条件写
 * `analysis.applied`,契约 §1.1/§2.1 的载荷列里也有),所以这道闸在真机上是**兜底**、
 * 不是当前可达路径;它守的是「回落值被当权威」这个类别本身。
 */
export function hasAppliedAnalysisConfig(state) {
    const a = (state && state.analysis) || {};
    const applied = a.applied;
    if (!applied || typeof applied !== "object") return false;
    // 逐字段断非空字符串:`appliedAnalysisConfigOf` 的回落是 `applied.x || cur.x`,
    // 空串 / undefined 都会触发回落,所以这里的判据要与那个 `||` 同口径。
    return (
        typeof applied.loudness_mode === "string" &&
        applied.loudness_mode !== "" &&
        typeof applied.center_slot_policy === "string" &&
        applied.center_slot_policy !== ""
    );
}

/**
 * 「改后需重分析」判定:当前值 !== 基线值。**逐项判**,响度档与中心槽策略各调一次 ——
 * 两枚徽标挂在两个控件旁,合成一个布尔会让它们同亮同灭。
 * 改走 → true(提示出现);改回基线值 → false(提示立即消失)。纯函数供 node 断言。
 *
 * [SL-279] 基线的**真源已经换了**:从「设置页 mount 那一刻的本地快照」换成工程 state 里的
 * `analysis.applied.*`(上次全量分析所用)。旧真源派生出一对方向相反的偏差,同根:
 * mount 早于首次 state 到达 —— 存成非默认档的工程一进 Tab4 就误报;同一会话里把口径切回
 * mount 默认值时又漏报(段表其实还是按旧档分析的)。补一次本地同步只是把误报换成漏报,
 * 所以改的是真源,不是补丁。
 */
export function analysisConfigStale(currentMode, baselineMode) {
    return currentMode !== baselineMode;
}

/**
 * 诊断区行模型(05 §2.4:每轨 heartbeat 年龄 / 失准计数 / generation / config_seq)。
 * @param {object} store app.js 事件仓({state, conn})
 */
export function diagRowsOf(store) {
    const conn = (store && store.conn) || {};
    const chans = conn.channels || [];
    const generation = conn.generation;
    const configSeq = ((store && store.state) || {}).config_seq || 0;
    return chans.map((c, i) => ({
        ch: String(i + 1).padStart(2, "0"),
        hb: heartbeatAgeText(c ? c.heartbeatAgeMs : undefined),
        mis: c ? c.misalignCount || 0 : 0,
        gen: generation,
        seq: configSeq,
    }));
}

/** 诊断区可复制文本(「复制诊断信息」按钮的目标内容;extraLines = 未知 code 降级行)。 */
export function diagText(rows, extraLines) {
    const head = ["CH", "HB", "MIS", "GEN", "SEQ"];
    const body = (rows || []).map((r) =>
        [r.ch, r.hb, r.mis, r.gen, r.seq].join(" "),
    );
    return [head.join(" "), ...body, ...(extraLines || [])].join("\n");
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
 *   getStore() 返回 app.js 维护的事件仓(state / conn / snapshot);
 *   getT() 返回当前语言字典;onLocalChange() 请求一次重渲染(乐观本地态用)。
 */
export function createTabSettings(opts) {
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
            console.warn(
                "SCVB Tab4:bridge." + name + "() 调用失败 —— " + e.message,
            );
            return null;
        }
    }

    const $ = (gb) => root.querySelector('[data-gb="' + gb + '"]');

    const el = {
        loudnessSeg: $("settings-loudnessmode-seg"),
        centerSeg: $("settings-centerslot-seg"),
        loudnessStale: $("settings-loudnessmode-stale"),
        centerStale: $("settings-centerslot-stale"), // [SL-278]
        // [SL-276] 重分析提示弹窗(卡片层单例,不在 Tab4 子树里 —— 见 index.html 那段注释)
        reanalyzeAsk: $("reanalyze-ask"),
        reanalyzeAskPanel: $("reanalyze-ask-panel"),
        // [SL-354] 框里的影响面说明段:内容**按触发它的那一项**换词条(见 syncReanalyzeScopeNote)。
        reanalyzeAskScopenote: $("reanalyze-ask-scopenote"),
        reanalyzeAskRangenote: $("reanalyze-ask-rangenote"),
        // [#256 R1(复审 2-1)] 那枚静态范围解释 span —— 它只随「范围档」显隐,与拒回执无关。
        reanalyzeAskRangehint: $("reanalyze-ask-rangehint"),
        reanalyzeAskRangedone: $("reanalyze-ask-rangedone"),
        // [SL-371] **名字沿旧、钮面已换**:锚点仍是 `reanalyze-ask-later`、本位仍叫
        // `reanalyzeAskLater`,而这枚钮现在写着「撤销更改」、点下去会**发一次写**
        // (revertFromAsk)。锚点不改是为了不动 C4d 那几格已经钉住的焦点圈闭断言;
        // 照名字读逻辑会读反,所以在取元素这一处就说清楚。
        reanalyzeAskLater: $("reanalyze-ask-later"),
        reanalyzeAskPrimary: $("reanalyze-ask-primary"),
        guideBox: $("settings-guideblock-rules"),
        guideList: $("settings-guideblock-rules-list"),
        guideMissing: $("settings-guideblock-rules-missing"),
        guideExpand: $("settings-guideblock-expand"),
        reopenTour: $("settings-reopentour"),
        viewWorkflow: $("settings-viewworkflow"),
        docs: $("settings-docs"), // [SL-214] 此前**从未接线**(见 index.html 那段行注)
        versionValue: $("settings-version-value"),
        storageValue: $("settings-storage-value"),
        storageGuid: $("settings-storage-guid"),
        diagBody: $("settings-diagnostics-body"),
        diagRows: $("settings-diagnostics-rows"),
        diagCopy: $("settings-diagnostics-copy"),
        diagChevron: $("settings-diagnostics-chevron"),
        diagBox: $("settings-diagnostics"),
        scaleSelect: $("settings-scale-select"),
    };

    // 页面内一次性状态(不属 state chunk,重开面板即重置)。
    //
    // [SL-279] **`analysisConfigBaseline` 没有了**。它曾是「响度口径的基线值」的本地承载:
    // mount 时快照当前值、由分析完成事件(onSegments)同步。契约当时没暴露 `applied.*`,
    // 只能这么凑。那条路派生出一对方向相反的偏差,同根 ——「基线在 state 到达前快照」:
    // 存成非默认档的工程一进 Tab4 就误报;同一会话里把口径切回 mount 默认值时又漏报。
    // 现在基线的真源是**工程 state 里的 `analysis.applied.*`**(上次全量分析所用),
    // 由 native 落盘、随撤销一起回退。判据面、两级回退语义与 abi 升格写在
    // `docs/contract-changes/20260905-sl279-applied-analysis-settings.md`,这里不复述。
    const local = {
        // [SL-276] 已就哪个口径值弹过框:开框那一下记下来,此后同一次 ask 的每一帧都被它挡住。
        // [SL-354] 记的从「值」变成 **`字段=值`** 的串:两个设置项现在都能弹,只按值记的话
        // 「响度改成 X」与「中央槽改成 X」会互相冒充(两边枚举值域不同,今天撞不上,但那是
        // 巧合不是判据)。同一轮里它还接下了「一次性」那半职责 —— 见下面 askPending 那段。
        //
        // [SL-354 复审第 2 轮] **清空点只有一个:wireSeg 里的一次新用户写。**
        // 原话「改回基线(stale 归 false)时清空,下次再改走照弹」在新尺子下**不成立**:
        // 「当前值 != 刚写的值」那句早退排在 `!stale` 那支之前,非 UI 路径把口径改回基线时
        // 那一支一次都跑不到,本位于是留着旧 token、把用户下一次重选同值挡成不弹。
        // 所以清空改由 wireSeg 承担(见那处);`!stale` 那支里同名的那一行留着,管的是
        // 「先经那一支清过」的另一条入口。两道**各自独立兜得住**,单删任一道本套都不红,
        // 两道一起拆才红(C10e3 / C10h,实测记在那两段注释里)—— 别把任一行单独当牙齿。
        reanalyzeAskedFor: null,
        // [SL-348] 播报句存 **key**(不是文本),每次 render 按当前字典重填 —— 见 renderRangeDone()。
        reanalyzeRangeDoneKey: null,
        // [SL-396 复审①] analyze **拒回执**(refused / busy)的提示 key。与上面那条**分开存**:
        // rangeDone 那句是「这次部分重算按哪份口径跑的」,只在范围档有意义,切出范围档就该清;
        // 而拒回执是「你这一下没生效、下一步该怎么办」,**任何档位都得留着**。
        // 合成一个字段的后果实测过:follow 档下 `syncReanalyzeRangeNote()` 的
        // `if (!limited) key = null` 会把它当场抹掉,而承载它的 `<p>` 在 follow 档还是 hidden
        // ⇒ 用户点了主钮依旧「什么也没发生」(#256 第 1 轮 bot ①)。
        reanalyzeRefusalKey: null,
        // [SL-276 复审] 弹窗的触发面是**用户点击**,不是派生的 stale 位。
        // 琥珀 badge 可以纯派生(多一枚小标记的代价很小),模态框不行 —— stale 有几条
        // 「用户什么都没做也为真」的路径,升级成框之后每条都变成一次要点掉的打断。
        //
        // [SL-279] **原来列在这里的第 ① 条已经不存在了**,本卡把它修掉了:那一条是
        // 「开工程即真」——mount() 早于首次 state 到达,基线取到的是默认档,于是存成
        // rms/peak_dbfs 的工程一进 Tab4 就 stale 恒真。基线换成工程 state 里的
        // `analysis.applied.*` 之后,这条路没有了(那正是 SL-279 的卡面)。
        // 还在的是:
        //   · **只读观察态(J69)**:主实例改档,观察实例经 scvb.state 收到新值也会 stale ——
        //     框里那枚「重新分析」是写控件,契约 §5.6 要求只读态下写控件一律不可操作。
        //   · **撤销 / 重做**:分析可撤销(SL-209),而 applied.* 随分析结果一起回退
        //     ([SL-279] 同一条撤销步),所以 Ctrl+Z 之后 stale 可能翻转 —— 那不是用户在改档。
        // `applied.*` 落在 CFGS(**工程级**,不分版本),所以切版本不会让它变。
        // 本位只由 wireSeg 里**写成功**的回调置起,syncStale 之外无人写它。
        // 上面这几条因此一次性关掉,而琥珀 badge 的既有语义一个字节没动。
        // **一次性**:弹过一次就不许再自己弹第二次。留着不管的话关框之后本位仍
        // 有效,后续任何非用户驱动的口径变化都能再弹一次 —— 上面这几条换个入口又漏回来。
        // [SL-371] 「关框」现在有三条各不相同的路:撤销更改(写受理后清 pending + token)/
        // Esc / 点遮罩(只关框,两位一个都不动)。本段说的是后两条 —— 撤销那条已经把
        // 两位都清掉了,不会留下「本位仍有效」这个形态。
        // [SL-354] **承担者换了,别照旧文找那一行**:原来靠「syncStale 真开框那一下就地
        // 清掉本位」实现,那一行**已经没有了** —— 本位开框之后还要继续当「这一帧新不新」
        // 的尺子(见下一段)。接替它的是 `reanalyzeAskedFor` 记下的 `字段=值`:同一次写
        // 算出来的 token 逐字相同,于是不会重开。下一次要弹,仍得由 wireSeg 里新的一次
        // 写成功把本位换掉(或先改回基线让 `!stale` 那支把两者一起清)。
        //
        // [SL-354] **从布尔改成「用户刚写成功的那一次是什么」**(`{field, value}`)。
        // 用户 v5.6.7 实测:「第一下会出老版本的那种横幅,第二次切换才会出弹窗」。
        // 根因就在这一位是布尔:点档 → 回执到 → 置位 → `requestRender()`,而**真桥上
        // 这一刻 state 还没回来**,`config()` 仍是旧值 ⇒ 派生的 stale 为假 ⇒ syncStale
        // 走 `!stale` 分支,**把刚置起的这一位当场清掉**。之后 state 到了,徽标亮(纯派生)
        // 而闸没了 ⇒ 不弹。第二次点时 stale 因第一次的改动本来就是真,不进那个分支,
        // 闸活下来 ⇒ 才弹。mock 侧看不到这条:它的 `patchState` 是**同步** emit 的,
        // `.then()` 跑到时 state 已是新值(登记在案的时序口径分叉,见本卡 PR)。
        //
        // 记住值之后,syncStale 就能区分**两个长得一模一样的形态**:
        //   · 「写还没回来」(当前值 ≠ 刚写的值)⇒ 这一帧的读数不作数,什么都不做;
        //   · 「用户自己改回去了」(当前值 == 基线)⇒ 该清、该关。
        // 布尔位没有这个信息,所以只能把两者当同一件事 —— 那正是 ① 的成因。
        askPending: null,
        // 开框前的焦点落点,关框时还回去(三个出口都会经过 closeReanalyzeAsk:遮罩 / Esc
        // 直接走它,[SL-371] 的「撤销更改」在写受理之后走它)。
        reanalyzeReturnFocus: null,
        // 框里**任一枚钮**发出去的调用在途:`null` / `"analyze"` / `"revert"`。
        // [SL-371 复审第 1 轮,统筹裁定] 原来是两个各管各的布尔(`reanalyzeInFlight` /
        // `revertInFlight`),**互不相认** —— 撤销这枚钮故意不挂 `disabled`(理由见下),
        // 于是两个方向的竞态都可达,而且都产生说反话的状态:
        //   · 「先点重新分析、在途时点撤销」:分析拿改动后那一档跑,撤销的写落在它后面
        //     ⇒ 跑完 `applied.*` 前移到那一档、当前值已撤回基线 ⇒ **徽标反向亮起**;
        //   · 「先点撤销、在途时点重新分析」:分析拿**即将被撤掉**的那一档跑起来。
        // 合成一位之后「任一在途 ⇒ 另一枚早退」是一句话的事,不必两处各记一遍。
        // 存的是**哪一路**而不是布尔:真红时看得出在途的是谁(诊断成本为零)。
        askInFlight: null,
        // [SL-375] 上一次**范围档**部分重算所用的那份口径快照(`{loudness_mode,
        // center_slot_policy}`)。为空 = 还没做过、或做过但口径之后又变了。
        // 只影响两枚徽标念哪条词条,不碰 stale 判定本身(见 syncStale 里 STALE_KEY 那段)。
        partialRangeFor: null,
        // [SL-371] 撤销这一路**只早退、不置灰** —— 上面 Tab 圈闭那段的护栏写着
        // 「置灰的只可能是主钮」,给这一枚也挂 disabled 会把那条推理变成假话
        // (而它正是回卷目标怎么选的依据)。所以它的防连点全压在 `askInFlight` 上,
        // 不像主钮那样有 `disabled` 当第二道。
        nineOpen: false,
        diagOpen: true, // 诊断区初始展开(用户 preview:避免下方空一块)
        copyDoneUntil: 0,
    };

    // ---------------------------------------------------------------- 小工具
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

    function fmt(raw, vals) {
        if (typeof raw !== "string") return "";
        return raw.replace(/\{(\w+)\}/g, (m, k) =>
            Object.prototype.hasOwnProperty.call(vals, k) ? String(vals[k]) : m,
        );
    }

    function hasOwn(obj, k) {
        return Object.prototype.hasOwnProperty.call(obj || {}, k);
    }

    function config(st) {
        return analysisConfigOf((st || getStore()).state);
    }

    /**
     * [SL-279] 当前是**范围档**吗(§1.8 `daw_loop` / `manual`)。
     *
     * 为什么这一页要关心它:范围档下「分析(全部)」推出来的范围是 `global.range`,
     * 不是整条时间线 —— 契约 §1.21 规定这种重算**不前移** `applied.*`,于是徽标不灭。
     * 这枚钮照样拿到 `ok:true`(后端确实受理并重算了范围内),所以**不能靠回执判**:
     * 受理成功不等于达成了用户点它的目的。
     *
     * 判据只能看**档位**:native 那边真正决定的是「`analyzeAllRange` 走没走整条那条分支」,
     * 而 web 侧拿不到那个分支结果。桥面上「非 follow」⇒ 一定有**有效**范围 ⇒ 一定走范围支,
     * 这个蕴含由 `handleSetRange`(挡掉 `manual` 的倒挂范围)与 `hostLoopSeconds`(挡掉空
     * `daw_loop`)两道校验给,**不是判据自带的** —— 同一句话在 `OutputProcessor.h` 的
     * `startAnalysis` 头注里,两处别再各写各的(复审第 7 轮:这已经是第三次)。
     *
     * 写成「非 follow」而不枚举 `daw_loop`/`manual`:§1.8 将来多一档时,漏枚举会静默倒向
     * 「当成 follow」—— 那是把提示藏起来的方向,错要错在多显一次。
     */
    function rangeLimited(st) {
        const g = ((st || getStore()).state || {}).global || {};
        const mode = (g.range || {}).mode;
        return !!mode && mode !== "follow";
    }

    /** 只读观察态(second-output / conn.outputReadOnly):J69 两设置块整组不可操作。 */
    function isReadOnly() {
        return !!getStore().readOnly;
    }

    // ---------------------------------------------------------------- mount
    function renderOptions(container, modes) {
        if (!container) return;
        const t = getT() || {};
        container.replaceChildren(
            ...modes.map((m) => {
                const btn = document.createElement("button");
                btn.type = "button";
                btn.className = "sc-seg__item";
                btn.setAttribute("data-value", m.value);
                btn.setAttribute("data-t", m.key);
                btn.setAttribute("aria-pressed", "false");
                text(btn, hasOwn(t, m.key) ? t[m.key] : m.value);
                return btn;
            }),
        );
    }

    function wireSeg(container, field) {
        if (!container) return;
        container.addEventListener("click", (e) => {
            if (isReadOnly()) return; // 只读观察态:整组不可操作(契约 §5.6 {observer:true})
            const btn =
                e.target instanceof Element
                    ? e.target.closest("[data-value]")
                    : null;
            if (!btn) return;
            const value = btn.getAttribute("data-value");
            if (value === config()[field]) return; // 点击已选中档不重复写
            call("setAnalysisConfig", { [field]: value }).then((res) => {
                // 桥缺失/异常(res===null)、只读被拒(observer)、badArg 一律不做乐观 dirty
                if (!res || res.observer || res.ok === false) {
                    requestRender();
                    return;
                }
                // 不做乐观 dirty —— syncStale 按「当前值 vs 基线」派生提示。
                //
                // [SL-276 复审] **这里是弹窗唯一的开闸点**:写真的被受理了,才允许下一次
                // syncStale 把框推到眼前。放在 res 判定之后 —— 桥缺失 / observer 拒 / badArg
                // 上面已经早退,走到这儿就是「用户刚改了档且改成了」。
                //
                // [SL-354] 两处改动:
                //   · **中央槽策略同样开闸**。原来只有 `loudness_mode` 置位,所以改中央槽
                //     完全不弹(用户 v5.6.7 实测「B3 完全没有弹出弹窗,应该和前面一样」)。
                //     那不是缺陷而是 SL-276 当时按用户口径有意做的 —— 用户这次改了口径,
                //     所以连同 syncStale 里的判据、两处注释、SL-276 那格用例一起翻面。
                //   · **记住写的是哪个字段的哪个值**,而不是置一个布尔 —— 理由见
                //     `local.askPending` 那段:布尔位分不开「写还没回来」与「用户改回去了」。
                local.askPending = { field, value };
                // [SL-354 复审第 2 轮] **一次新的用户写就重新开闸。**
                // 复审给的可复现序列(默认同步页即可):改档 → 弹 → Esc 关框 ⇒
                // 非 UI 路径把这一项改回基线 ⇒ 上面那把尺子(当前值 != 刚写的值)
                // 早退,`!stale` 那支一次都没跑到,于是 reanalyzeAskedFor 里那个 token
                // 留着 ⇒ 用户**再点同一个值**时 token 逐字相同 ⇒ 不弹,只亮徽标。
                // 那正是用户报的 ①(该弹不弹)换了个入口,而旧的布尔实现在这条序列上
                // 是会弹的 —— 是本卡引入的静默行为变化,所以修。
                //
                // 修在这里而不是去改那把尺子:尺子看不出「非 UI 改回基线」与「过期帧」
                // 的差别(两者在那一帧上逐字节相同),但**一次新的用户写**是尺子之外的
                // 另一个信号 —— 而 reanalyzeAskedFor 要挡的本来就只是「同一次 ask 被
                // 每帧重弹」,不是挡用户的下一次点击。上面那句 `value === config()[field]`
                // 的去重保证了「点已选中档」根本走不到这里,所以不会因为空点重开框。
                local.reanalyzeAskedFor = null;
                requestRender();
            });
        });
    }

    function toggleNine() {
        local.nineOpen = !local.nineOpen;
        if (el.guideBox)
            attr(el.guideBox, "data-open", local.nineOpen ? "1" : "0");
        if (el.guideExpand) {
            const key = local.nineOpen
                ? "set.guide.collapse"
                : "set.guide.showAll";
            attr(el.guideExpand, "data-t", key);
            text(el.guideExpand, hasOwn(getT(), key) ? getT()[key] : key);
        }
    }

    /** 幂等展开「查看全部九条」(tour 步 36 的 per-step 动作钩子;已展开则不动)。 */
    function expandNine() {
        if (local.nineOpen) return;
        local.nineOpen = true;
        if (el.guideBox) attr(el.guideBox, "data-open", "1");
        if (el.guideExpand) {
            const key = "set.guide.collapse";
            attr(el.guideExpand, "data-t", key);
            text(el.guideExpand, hasOwn(getT(), key) ? getT()[key] : key);
        }
    }

    function reopenTour() {
        // [T36b] tour.js 消费 data-tour="review" 锚点并重启交互式引导(tour_seen 已置位也可再开);
        // T35 只落入口,真正重启经 opts.onReopenTour 回 app.js 调 tour.start()。
        // 无 tour 时零副作用(按钮本身不写 state、不发桥函数)。
        if (typeof opts.onReopenTour === "function") opts.onReopenTour();
    }

    function viewWorkflow() {
        // 查看工作流程大卡(与 tour 步 2 同一张大卡);回 app.js 打开独立 overlay。
        if (typeof opts.onViewWorkflow === "function") opts.onViewWorkflow();
    }

    function openDocs() {
        // [SL-214 用户实测 2026-08-27] 这颗钮此前**根本没有 handler** —— index.html 里
        // 那段行注写着「点击行为仍未定,故不挂 handler」,于是点它什么都不发生。
        // 现在定了:走系统浏览器(见 app.js 的 openDocsInBrowser)。用回调而不是在这里
        // 直接开,是因为语言在 app.js 手里,且外链是整页级行为、不该由某个 tab 私自发起。
        if (typeof opts.onOpenDocs === "function") opts.onOpenDocs();
    }

    function toggleDiag() {
        local.diagOpen = !local.diagOpen;
        if (el.diagBox)
            attr(el.diagBox, "data-open", local.diagOpen ? "1" : "0");
        if (el.diagChevron)
            attr(el.diagChevron, "aria-expanded", String(local.diagOpen));
    }

    async function copyDiag() {
        const rows = diagRowsOf(getStore());
        const unknown = ((getStore() && getStore().unknownCodes) || []).map(
            (c) => "unknown: " + c,
        );
        const payload = diagText(rows, unknown);
        let ok = false;
        try {
            if (
                typeof navigator !== "undefined" &&
                navigator.clipboard &&
                typeof navigator.clipboard.writeText === "function"
            ) {
                await navigator.clipboard.writeText(payload);
                ok = true;
            }
        } catch {
            ok = false;
        }
        if (!ok && typeof document !== "undefined") {
            // WebView2 旧壳兜底:临时 textarea + execCommand(剪贴板 API 不可用时)
            try {
                const ta = document.createElement("textarea");
                ta.value = payload;
                ta.style.position = "fixed";
                ta.style.opacity = "0";
                document.body.appendChild(ta);
                ta.select();
                ok = document.execCommand("copy");
                document.body.removeChild(ta);
            } catch {
                ok = false;
            }
        }
        if (ok) {
            local.copyDoneUntil = Date.now() + 2000;
            requestRender();
            // 「已复制」回落兜底:2s 后补一拍渲染翻回「复制诊断信息」
            setTimeout(() => requestRender(), 2000);
        }
    }

    function mount() {
        renderOptions(el.loudnessSeg, LOUDNESS_MODES);
        renderOptions(el.centerSeg, CENTER_SLOT_POLICIES);
        wireSeg(el.loudnessSeg, "loudness_mode");
        wireSeg(el.centerSeg, "center_slot_policy");
        if (el.guideExpand)
            el.guideExpand.addEventListener("click", toggleNine);
        if (el.reopenTour) el.reopenTour.addEventListener("click", reopenTour);
        if (el.viewWorkflow)
            el.viewWorkflow.addEventListener("click", viewWorkflow);
        if (el.docs) el.docs.addEventListener("click", openDocs);
        if (el.diagChevron)
            el.diagChevron.addEventListener("click", toggleDiag);
        if (el.diagCopy) el.diagCopy.addEventListener("click", copyDiag);
        // [SL-276] 弹窗三个出口:框里那枚次要钮 / 点遮罩本身 / Esc。
        // [SL-371] **三个出口不再同义**:次要钮从「稍后 = 只关框」改成「撤销更改 =
        // 把刚改的那一项写回基线」(用户 v5.6.8 原话「直接强制回退到原来的方案吧,
        // 这样用户完全知道自己在干什么」);遮罩与 Esc **仍旧一个字节都不写**。
        // 三条理由,缺一条我都会把它们并回去:
        //   · Esc / 点遮罩是「关掉这个框」的通用手势,不是对内容的裁定 —— 绑上写操作,
        //     一次误按就改了工程状态,而且没有任何提示;
        //   · 撤销那一路会被桥拒(桥缺失 / 只读 observer / badArg),按主钮同一口径
        //     **拒了不关框**;此时遮罩与 Esc 是唯一还能把框收掉的出口。两条路都写的话,
        //     用户会被关在框里;
        //   · 钮面现在写着「撤销更改」,它不再是「什么都不做」那一枚 —— 框里两枚钮都是
        //     动作,「什么都不做」这个第三种结果只能由 Esc / 遮罩承担(说明段有一句
        //     写明这件事,见 set.reanalyzeAsk.revertHint)。
        if (el.reanalyzeAskLater)
            el.reanalyzeAskLater.addEventListener("click", revertFromAsk);
        if (el.reanalyzeAskPrimary)
            el.reanalyzeAskPrimary.addEventListener(
                "click",
                doReanalyzeFromAsk,
            );
        if (el.reanalyzeAsk)
            el.reanalyzeAsk.addEventListener("click", (e) => {
                if (e.target === el.reanalyzeAsk) closeReanalyzeAsk();
            });
        // Esc 挂在 document 上(而不是框上):框里只有两枚按钮,焦点一旦被挪走
        // (点了遮罩、或 AT 把焦点收回 body)就再也收不到键。只在本框可见时动作,
        // 且 mount() 全程只跑一次(app.js:471),不会叠加同一个监听。
        // 与别处 Esc 不打架:另一条 document 级 Esc 在 tab-wave.js,自带
        // `isPanelActive()` 闸;而本框只可能在 Tab4 弹出 —— app.js 的 render 按
        // 当前 tab 分派,`tabSettings.render()`(=> syncStale)只在设置页跑。
        (root.ownerDocument || root).addEventListener("keydown", (e) => {
            if (!el.reanalyzeAsk || el.reanalyzeAsk.hidden) return;
            if (e.key === "Escape") {
                e.preventDefault();
                closeReanalyzeAsk();
                return;
            }
            // [SL-276 复审] Tab 圈在框里。框声明了 aria-modal="true",而 aria-modal
            // **只影响辅助技术的朗读范围,不拦 Tab** —— 不圈的话焦点会走到遮罩背后那些
            // 此刻不该被操作的控件上(响度胶囊、诊断区、页脚)。框里只有两枚钮,
            // 所以不必引入通用 focus-trap:两端各自回卷即可。
            if (e.key !== "Tab") return;
            const first = el.reanalyzeAskLater;
            const last = el.reanalyzeAskPrimary;
            if (!first || !last) return;
            const doc = root.ownerDocument || root;
            const here = doc && doc.activeElement;
            // [SL-276 二轮复审] 回卷目标要避开 disabled 的那枚:主钮在 analyze 在途期间
            // 会被置灰(见 doReanalyzeFromAsk),而 focus() 对 disabled 元素是空操作 ——
            // 直接回卷过去的话,preventDefault() 已经吃掉了这次 Tab、焦点却原地不动,
            // Tab 在那一小段时间里等于失灵。置灰的只可能是主钮,故退到次要钮
            // (reanalyze-ask-later;[SL-371] 钮面已改成「撤销更改」,**没有**跟着挂
            // disabled —— 见 local.askInFlight 那段,那正是为了让这句话继续成立)。
            //
            // [SL-276 四轮复审] `focusable` 不能只用在「回卷**进来**」那两条,**正向 Tab
            // 出去**那条的比较对象也得换成它 —— 否则圈闭在次要钮这一格上是**开口**的:
            // 主钮置灰时焦点停在 first,正向 Tab 三条分支一条都不命中 ⇒ 不 preventDefault
            // ⇒ 浏览器按 DOM 顺序接着走。主钮可点时下一个正好是它(index.html 里 later 在前、
            // primary 在后),所以平时看不出来;而在途期间它 disabled、会被跳过,焦点直接
            // 落到遮罩背后的响度胶囊 / 诊断区 —— 正是上面那段说要拦住的东西。
            // 这条路正是「在途置灰」那条修补引出来的,而且走得到:置灰把焦点掉回 <body>
            // → 一次 Tab 命中第三分支被送到次要钮 → 再一次就出框了。
            // 换成「实际生效的末位」之后:主钮可点时 focusable(last, first) === last,
            // 行为与从前逐字相同;置灰时它等于 first,于是从次要钮正向 Tab 就地回卷到
            // 自己,焦点出不去。
            const focusable = (pref, alt) =>
                pref && pref.disabled !== true ? pref : alt;
            if (e.shiftKey && here === first) {
                e.preventDefault();
                focusable(last, first).focus({ preventScroll: true });
            } else if (!e.shiftKey && here === focusable(last, first)) {
                e.preventDefault();
                first.focus({ preventScroll: true });
            } else if (here !== first && here !== last) {
                // 焦点已经在框外(点过遮罩、或被 AT 收回 body):收回框里再继续。
                e.preventDefault();
                focusable(last, first).focus({ preventScroll: true });
            }
        });
    }

    // --------------------------------------------------------------- render
    function syncOptions() {
        const c = config();
        const ro = isReadOnly();
        const pairs = [
            [el.loudnessSeg, c.loudness_mode],
            [el.centerSeg, c.center_slot_policy],
        ];
        for (const [seg, cur] of pairs) {
            if (!seg) continue;
            for (const btn of seg.querySelectorAll("[data-value]")) {
                const on = btn.getAttribute("data-value") === cur;
                attr(btn, "aria-pressed", String(on));
                attr(btn, "data-disabled", ro ? "1" : "0");
                attr(btn, "aria-disabled", ro ? "true" : "false");
            }
        }
    }

    // ---------------------------------------------------------- [SL-276] 重分析弹窗
    // [J85] 的口径是「不弹阻塞确认框」;本框是用户 2026-09-01 明确点名的**唯一**例外
    // (原来只有一条小琥珀 badge,用户看不清)。别据此在别处再开第二个弹窗。
    function closeReanalyzeAsk() {
        const wasOpen = !!el.reanalyzeAsk && !el.reanalyzeAsk.hidden;
        show(el.reanalyzeAsk, false);
        if (!wasOpen) return;
        // 焦点还回开框前那一件(通常是响度胶囊里刚被按下的那枚钮)。不还的话
        // 键盘用户按 Esc 之后焦点落在 <body>,Tab 得从卡片开头重走一遍。
        const back = local.reanalyzeReturnFocus;
        local.reanalyzeReturnFocus = null;
        if (back && typeof back.focus === "function" && back.isConnected)
            back.focus({ preventScroll: true });
    }

    /**
     * 范围档提示随档位开合。**开框那一下要同步、状态更新也要同步** —— 框开着时用户仍可能
     * 在别处(Tab3 工具条)改范围档,只在开框时算一次的话提示会停在旧档位上。
     */
    /**
     * 写 `role="status"` 那半动态文本(结构见 index.html 那两段注释)。
     *
     * **存的是 key,不是文本** —— 这是本函数与 `renderRangeDone()` 分家的全部理由:
     * 文本必须只从字典取,不能在逻辑层拼串(i18n 门禁扫不到、也是「新增用户可见文案必须
     * 有 key」那条),而且**存文本会在切语言时当场变成中英混排** ——
     *
     * [SL-348 复审第 1 轮] 原实现直接写当前语言的字面文本,没有留刷新钩子,两个可达形态:
     *   ① 框开着切语言:`refreshI18n()` = `applyI18n(document, lang)` + `render()`;
     *      `applyI18n` 只认 `data-t`,所以静态那半换成新语言、这半停在旧语言 ——
     *      同一个 `<p>` 里前半英文后半中文。与 `tab-wave.js` 的 `renderReidentifyBody()`
     *      同一族(那边是「框可见就在 render 里补填」),这里走同一条路;
     *   ② `limited` 真→假→真:`syncReanalyzeRangeNote()` 原来只开合 `<p>`,里面那句
     *      不动 —— 切回范围档时上一轮那句会随 `<p>` 重新显出来,而它当下已经为假。
     *      所以 `limited` 为假时**连 key 一起清掉**。
     */
    function setRangeDoneKey(key) {
        local.reanalyzeRangeDoneKey = key || null;
        renderRangeDone();
    }

    /** [SL-396 复审①] 拒回执提示的写入位(与 rangeDone 分开,见 local 那边的头注)。 */
    function setAnalyzeRefusalKey(key) {
        local.reanalyzeRefusalKey = key || null;
        // [#256 R1(b)(复审 2-1)] **先把承载它的 `<p>` 显出来,再写文本** —— live region 只在
        // **可见态**下发生文本变化才会被播报;在 hidden 时写进去,读屏用户什么也听不到,
        // 而那正是这一格要修的东西。`syncReanalyzeRangeNote()` 下一次 render 会按同一条判据再同步。
        if (local.reanalyzeRefusalKey && el.reanalyzeAskRangenote) {
            show(el.reanalyzeAskRangenote, true);
        }
        renderRangeDone();
    }

    /** 按当前字典重填那半文本。每次 render 都会被 `syncReanalyzeRangeNote()` 叫到。 */
    function renderRangeDone() {
        const node = el.reanalyzeAskRangedone;
        if (!node) return;
        // [SL-396 复审①] **拒回执优先**:两条同时在场时,用户更该看到的是「这一下没生效」。
        const key = local.reanalyzeRefusalKey || local.reanalyzeRangeDoneKey;
        const t = getT() || {};
        // 兜底**有意与本文件其余各处相反**:别处是 `hasOwn(t, k) ? t[k] : k`(漏词条时把 key
        // 本身显出来,一眼看得见);这半是 live region,把 `set.reanalyzeAsk.rangeDone` 这串
        // 机器 key 念给读屏用户,比什么都不念更糟 —— 所以取不到就写空串。
        // **做「统一兜底」时别顺手把这里改成 fallback-to-key**(复审第 3 轮点名)。
        const next = key && hasOwn(t, key) ? t[key] : "";
        // 同值不写:`role="status"` 是 live region,每次 render 都重写一遍同一句会让
        // 部分 AT 反复播报。只有真变化才落笔 —— 这也正是「清空 → 写入」能被念到的原因。
        if (node.textContent !== next) node.textContent = next;
    }

    function syncReanalyzeRangeNote() {
        if (!el.reanalyzeAskRangenote) return;
        const limited = rangeLimited();
        // [SL-396 复审①] 有拒回执时**无论哪个档位都要显出来** —— 否则 follow 档下这句被
        // `hidden` 吃掉,用户在屏上什么都看不到(等于没提示)。范围那句仍然只随 `limited` 走。
        show(el.reanalyzeAskRangenote, limited || !!local.reanalyzeRefusalKey);
        // [#256 R1(复审 2-1,假句)] **里面那枚静态 span 仍然只随 `limited` 走**。
        // 它写的是「当前是范围档,请先把范围切回跟随播放头」—— 那是**范围**这件事的解释,
        // 与拒回执无关;第一版把它跟着 `<p>` 一起显出来,于是 follow 档下用户看到一句
        // 「请先把范围切回跟随播放头」,而他本来就是跟随播放头:一句假话。
        if (el.reanalyzeAskRangehint) show(el.reanalyzeAskRangehint, limited);
        // [SL-348 复审第 1 轮] 切出范围档就把**播报句**连 key 一起清掉:不清的话,再切回来时
        // 上一轮那句会随 `<p>` 重新显出来,而它描述的那次分析早已不是「当前范围」。
        // ⚠ 只清 rangeDone 那一句:**拒回执不清**(它说的不是范围口径,清了就是 #256 bot ①)。
        if (!limited) local.reanalyzeRangeDoneKey = null;
        // 每次 render 都按当前字典重填 —— 框开着切语言时靠的就是这一句(见 renderRangeDone)。
        renderRangeDone();
        // [SL-279 复审第 7 轮] **`aria-describedby` 跟着一起动** —— 否则读屏用户拿到的仍是
        // 「框不关、什么也没说」:本框是 role="alertdialog",描述只念 describedby 指到的节点。
        //
        // ⚠ **不能把两个 id 静态并进去**:AccName/Description 计算对 describedby **直接引用**
        // 的节点是「即使 hidden 也纳入」的,静态并进去会让 follow 档下也念出那句范围提示 ——
        // 那是把假话念给读屏用户,比不念更糟。
        //
        // 与显隐**共用同一个 `limited`**,不另起第二个条件:两个条件迟早分叉,而「同一件事
        // 两处各判一次」正是这张卡在收的那一族。
        //
        // [SL-371] 两档的**共同前缀**里多了 `reanalyze-ask-reverthint`(「撤销更改」
        // 那枚钮的后果说明)。它与上面那条禁令不冲突,判据是**它恒可见** —— 没有任何
        // 分支会把它 hidden,所以不存在「念了却看不见」的形态;`index.html` 里的静态
        // 默认值与这里的前缀**必须逐字同一串**(那份是首帧、这份是每次 render 覆盖,
        // 两处漂开的话首帧念一套、之后念另一套)。
        const panel = el.reanalyzeAskPanel;
        if (panel && typeof panel.setAttribute === "function") {
            const base = "reanalyze-ask-scopenote reanalyze-ask-reverthint";
            // [#256 R1(a)(复审 2-1)] 「要不要把 `<p>` 挂进描述」必须与**它的显隐**同一条判据:
            // 拒回执把 `<p>` 显出来之后,它就不再是「hidden 也被念」的那一类了(它有内容要念),
            // 所以这里跟着一起放开;而 `<p>` 里面的范围解释 span 仍只随 `limited` 走,
            // 描述里念到的是拒回执那句本身。
            panel.setAttribute(
                "aria-describedby",
                limited || local.reanalyzeRefusalKey
                    ? base + " reanalyze-ask-rangenote"
                    : base,
            );
        }
    }

    /**
     * [SL-354 复审第 1 轮] 框里的影响面说明段**跟着触发它的那一项走**。
     *
     * 本卡把弹窗铺到中央槽策略之后,这一段仍恒是响度口径专用的那条词条 ——
     * 只改了中央槽的用户,看到的框在解释另一件事;而这个 `<p>` 同时是本框
     * `aria-describedby` 的目标(见 index.html 那段与 syncReanalyzeRangeNote()),
     * 读屏用户听到的描述同样是错的那一条。
     *
     * 两条词条**都已存在、三语齐、也都已经渲染在各自的设置卡上**(响度卡第二行 /
     * 中央槽卡第二行),这里只是按字段取其一,**零新增词条**。
     * 做法与 syncReanalyzeRangeNote() 改写 `aria-describedby` 同源:每次 render 都按
     * 当前字典重填,框开着切语言才不会停在旧语言上。
     *
     * 没有待观察的写时回落到响度那条 —— 与 index.html 里写死的静态默认值同一条,
     * 免得「框还没被任何一次改档触发过」时这里与 HTML 各说各的。
     */
    const ASK_SCOPE_NOTE_KEY = {
        loudness_mode: "set.reanalyze.scopeNote",
        center_slot_policy: "set.centerSlot.scopeNote",
    };
    function syncReanalyzeScopeNote() {
        const node = el.reanalyzeAskScopenote;
        if (!node) return;
        const pending = local.askPending;
        const key =
            (pending && ASK_SCOPE_NOTE_KEY[pending.field]) ||
            ASK_SCOPE_NOTE_KEY.loudness_mode;
        attr(node, "data-t", key);
        const t = getT() || {};
        text(node, hasOwn(t, key) ? t[key] : key);
    }

    function openReanalyzeAsk() {
        // [SL-276 复审] 只读观察态一律不弹:框里那枚「重新分析」是写控件,
        // 契约 §5.6 要求只读态下写控件不可操作(后端另有 {observer:true} 兜底,
        // 但那是「点了才知道」,UI 这一闸才是用户看得见的那道)。
        if (isReadOnly()) return;
        if (el.reanalyzeAsk && el.reanalyzeAsk.hidden) {
            const doc = root.ownerDocument || root;
            local.reanalyzeReturnFocus = doc && doc.activeElement;
        }
        show(el.reanalyzeAsk, true);
        // [SL-279 复审第 8 轮] 每次开框先清掉上一轮的播报文本:live region 只在**文本变化**时
        // 播报,不清的话第二次点主钮写入同一句话 = 零变化 = 读屏什么也不念。
        setRangeDoneKey(null);
        // [#256 R2(复审 2-2)] **拒回执同样要清**:它与 rangeDone 是同一条 live region 的两个来源,
        // 只清一个 ⇒ 上一次的拒回执留在屏上,用户这次看到的是上一轮的结论(与上面那条同一个病)。
        setAnalyzeRefusalKey(null);
        syncReanalyzeRangeNote();
        if (
            el.reanalyzeAskPrimary &&
            typeof el.reanalyzeAskPrimary.focus === "function"
        )
            el.reanalyzeAskPrimary.focus({ preventScroll: true });
    }

    /**
     * [SL-371] 「撤销更改」= 把**刚改的那一项**写回 `analysis.applied.*` 的值。
     *
     * 用户 v5.6.8 原话:「如果点击『稍后』的话,直接强制回退到原来的方案吧,这样用户
     * 完全知道自己在干什么」。原来这枚钮只关框,于是点完之后当前值仍是新档、徽标还
     * 亮着 —— 用户手上是一个「我没决定,但设置已经改了」的中间态,而框已经没了。
     *
     * 写的是**基线值**,不是「上一个值」:基线 = 上次全量分析真正用过的那一档
     * (`appliedAnalysisConfigOf`,SL-279 的真源),这才是「原来的方案」。
     *
     * 四条早退,各自的理由:
     *   ① 框里任一路在途 —— 连点两下只准打出一发,且与主钮互斥(见 local.askInFlight);
     *   ② **没有待观察的写** —— 那说明这个框不是被一次用户改档推出来的(理论上
     *      openReanalyzeAsk 走不到,但 `pending` 是可空的),此时「原来的方案」是哪一项
     *      根本无从谈起,退化成纯关框,别把用户困在框里;
     *   ③ **这份 state 没带 `applied`** —— `appliedAnalysisConfigOf` 会回落到当前值
     *      (见它的头注),拿回落值去写就是把「读不到基线」当成「基线 = 当前值」,
     *      写出去是一次骗人的空写。同 syncStale 那道兜底闸的口径:回落值只许渲染。
     *      这一支同样退化成纯关框;
     *   ④ **当前值已经等于基线** —— 没什么可撤的(`wireSeg` 里那句「点已选中档不重复写」
     *      的同族),直接清闸关框,不发一次必然无效的写。
     *
     * 拒绝态**不关框**,与主钮逐字同一条理由(见下面 doReanalyzeFromAsk 的头注):
     * 桥缺失 / observer 拒 / badArg 时框留着 = 这一下没生效、可以再点;框没了、档位没变、
     * 又没有任何别的反馈,看起来就是「这枚钮坏了」。要放弃就按 Esc / 点遮罩。
     *
     * 受理之后**闸与 token 一起清**:这一次 ask 已经被用户处置完了。清了之后
     * syncStale 走 `if (!pending) return;` 那支 —— 于是「写还没回来」的那几帧只会让
     * 徽标继续亮着,不会再弹一次框;等状态帧到了,当前 == 基线 ⇒ 徽标自己灭。
     * 徽标不在这里就地熄:它是纯派生的,就地写会被下一帧按旧 state 抹回去。
     */
    async function revertFromAsk() {
        // [SL-371 复审第 1 轮,统筹裁定] **框里任一路在途,这枚钮就早退**(claude 与
        // pr-agent 各自独立指出同一条)。两位合成 `local.askInFlight` 的理由与两个方向的
        // 后果写在那一位的声明处,这里不复述。
        if (local.askInFlight) return;
        const pending = local.askPending;
        if (!pending) {
            closeReanalyzeAsk();
            return;
        }
        const st = getStore().state;
        if (!hasAppliedAnalysisConfig(st)) {
            local.askPending = null;
            local.reanalyzeAskedFor = null;
            closeReanalyzeAsk();
            return;
        }
        const baseline = appliedAnalysisConfigOf(st)[pending.field];
        if (config()[pending.field] === baseline) {
            local.askPending = null;
            local.reanalyzeAskedFor = null;
            closeReanalyzeAsk();
            return;
        }
        local.askInFlight = "revert";
        try {
            const res = await call("setAnalysisConfig", {
                [pending.field]: baseline,
            });
            if (!res || res.observer || res.ok === false) {
                requestRender();
                return;
            }
            local.askPending = null;
            local.reanalyzeAskedFor = null;
            closeReanalyzeAsk();
            requestRender();
        } finally {
            local.askInFlight = null;
        }
    }

    // 「重新分析」= 契约 §1.6 analyze("all")(全轨;设置页没有选区概念)。
    // **时间维不一定是全时长**:§1.8 范围档下 `"all"` 推出来的是 `global.range` —— 契约 §1.21
    // 规定那种重算不前移 `applied.*`,所以徽标不灭。follow 档下才是整条已采集时间线,
    // 结果经 §2.8 回推、基线由 `applied.*` 同步、琥珀 badge 自己灭。
    //
    // [SL-276 复审] **拒绝态不关框**。§1.6 会回 {ok:false, reason:"busy"}(已有分析在跑),
    // §5.6 会回 {observer:true};先关框再发请求的话,这两种情况下框没了、琥珀 badge 还挂着、
    // 也没有任何别的反馈 —— 看起来就是「这枚钮坏了」。框留着 = 这一下没生效、可以再点,
    // 与 wireSeg 里「被拒就只 requestRender、不落乐观值」是同一口径(本仓不用 toast)。
    //
    // [SL-279 复审第 6 轮] **范围档也不关框**,理由与上面**逐字同一条**:范围档下回执是
    // `ok:true`(后端确实重算了范围内),但徽标不灭 —— 关框就正好落进上一段说的那个形态。
    // 所以判据不能是回执,得是「这一下有没有可能达成用户点它的目的」。框留着 + 范围提示
    // 亮着,用户下一步是去把范围切回 follow,而不是对着一枚「坏钮」再点几次。
    //
    // [SL-276 二轮复审] **在途期间锁主钮**。「拒绝态不关框」之后框在 await 期间是开着的、
    // 主钮也还可点,连点两下就打出第二发 analyze(第二发被 §1.6 的 busy 拒掉 —— 但那是
    // 让后端替 UI 兜一个 UI 自己拦得住的连点)。
    // call() 内有 try/catch、异常路径回 null 而不抛,所以 finally 一定跑得到,不会锁死钮。
    //
    // [SL-276 三轮复审] 三条都是「看着做了、其实没生效」那一类,逐条核过:
    //   ① **光设 `.disabled` 在本仓看不见**。`web/` 里唯一的 `:disabled` 规则是
    //      `.tracks-row__pair-trigger:disabled`(output/index.html),与本钮无关;而
    //      `.sc-btn--cta` 自带 background/color,作者样式在场时 UA 的禁用灰不生效,
    //      `.sc-btn:hover{scale:1.02}` 也照样命中 —— 用户看到的是「按钮没变、hover 还会动、
    //      点了没反应」。禁用视觉的仓内口径是属性钩子 `.sc-btn[data-disabled="1"]`
    //      (base.css:opacity .4 + not-allowed + scale 归 1),故两者一起挂、一起摘,
    //      不新写 CSS。
    //   ② **disable 一个正持焦的元素会把焦点掉回 `<body>`**(Chromium)。成功路径无所谓
    //      (框马上关,closeReanalyzeAsk 把焦点还回响度胶囊);但 busy / observer 这条路
    //      **框是留着的**,焦点却已经在框外 —— 键盘用户再按 Enter 什么都不会发生,想重试
    //      反而更难。故 finally 里框还开着就把焦点还给主钮。
    //   ③ 防连点由 `askInFlight` 早退与 `disabled` 两道**各自独立**挡住。冒烟 C4c
    //      钉的是「连点两下只打出一发 analyze」,**两道都拆掉才会红**(留一道仍守得住);
    //      C4b 钉的是另一件事 —— 跑完一定解锁(finally 丢了就永久停在 disabled)。
    async function doReanalyzeFromAsk() {
        // [SL-371 复审第 1 轮] 判据从 `reanalyzeInFlight` 换成共用的 `askInFlight`:
        // 撤销那一路在途时点这枚钮同样要早退(见 askInFlight 声明处列的两个方向)。
        if (local.askInFlight) return;
        local.askInFlight = "analyze";
        const btn = el.reanalyzeAskPrimary;
        if (btn) {
            btn.disabled = true;
            btn.setAttribute("data-disabled", "1");
        }
        try {
            // 清在**发请求之前**:这样「受理后写入」必定是一次真变化,live region 才会念。
            // 放在受理之后清再写,同一帧内 textContent 一去一回,AT 可能一次都不播报。
            setRangeDoneKey(null);
            // [SL-396 复审①] 拒回执走**自己那一位**,与 rangeDone 分开:它不受范围档门控、
            // 也不会被 `syncReanalyzeRangeNote()` 的「切出范围档就清」抹掉。新一次尝试先撤旧的。
            setAnalyzeRefusalKey(null);
            const res = await call("analyze", "all");
            if (!res || res.observer || res.ok === false) {
                // [SL-396] **拒回执要说出来**。此前这里静默 `requestRender()` 就返回,屏上
                // 与受理成功一模一样(框不关、也没多一个字)⇒ 用户读到的就是「点了没反应」。
                // [SL-396 复审②] 判据用 tab-wave 导出的那一份(`analyzeRefusalNote`),
                // 不在这里内联复刻 ——「同一个判断各存一份」正是这一族缺陷复发的形状。
                // `!res`(桥没回话)与 `observer`(只读观察态)它一律回 null,与右侧两页同口径。
                setAnalyzeRefusalKey(analyzeRefusalNote(res));
                requestRender();
                return;
            }
            if (rangeLimited()) {
                // 受理了,但只重算范围内 ⇒ `applied.*` 不前移、徽标不灭(§1.21)。
                // 与拒绝态同口径:不关框,让范围提示继续摆在眼前。
                // [SL-375] 记下**这次部分重算是按哪一份口径跑的**。徽标此后改说
                // 「只更新了部分范围」而不是「改后需重分析」—— 用户刚刚就重新分析过了,
                // 再叫他做一遍是句废话(用户 2026-09-06 裁定)。
                // 记的是**配置快照**而不是一个布尔:口径再改一次,这次部分重算就与新口径
                // 无关了,快照对不上 ⇒ 自动退回「改后需重分析」,不必再找地方清它。
                local.partialRangeFor = { ...config() };
                syncReanalyzeRangeNote();
                // [复审第 8 轮] **视觉与读屏两侧要对称**:视觉用户按下去看到「框没关」本身
                // 就是反馈,读屏用户不会被自动告知「什么都没发生」—— 所以这里必须往
                // live region 里写一句真话,否则 AT 那一侧仍是零反馈(上一轮只做了
                // aria-describedby 那半,把这半漏了)。
                setRangeDoneKey("set.reanalyzeAsk.rangeDone");
                requestRender();
                return;
            }
            closeReanalyzeAsk();
            requestRender();
        } finally {
            local.askInFlight = null;
            if (btn) {
                btn.disabled = false;
                btn.removeAttribute("data-disabled");
                if (el.reanalyzeAsk && !el.reanalyzeAsk.hidden)
                    btn.focus({ preventScroll: true });
            }
        }
    }

    /**
     * [SL-375] 两枚徽标**念哪条词条**。用户 2026-09-06 裁定:范围档下点完「重新分析」
     * 之后徽标不该再写「改后需重分析」—— 契约 §1.21 规定那种重算不前移 `applied.*`,
     * 所以徽标必然还亮着,而用户**刚刚就重新分析过了**,那句话是叫他再做一遍已经做过的事。
     * 改成陈述状态:「只更新了部分范围」(范围外仍按旧口径)。
     *
     * **stale 判定本身一个字节没动** —— 亮不亮还是 `当前值 !== applied`,本函数只换文本。
     *
     * 三个合取项,各自的理由:
     *   · `rangeLimited()` —— 与弹窗里范围提示**共用同一个谓词**(不另起第二个条件,
     *     那正是本文件立过的纪律)。切回 follow 档之后「重新分析」这条建议重新可执行,
     *     文案就该退回去叫他做;
     *   · 快照非空 —— 这一档下**做过**一次部分重算(没做过就还是「改后需重分析」);
     *   · 快照 == 当前口径 —— 那次部分重算算的就是眼前这一份。口径之后又改了的话
     *     快照对不上,自动退回,不必再找地方清它。
     *
     * 每次 render 都按当前字典重填(与 syncReanalyzeScopeNote 同一条理由:切语言时
     * `applyI18n` 只认写在 DOM 上的 `data-t`,所以 `data-t` 与 textContent 一起换)。
     */
    const STALE_KEY_PLAIN = "set.reanalyze";
    const STALE_KEY_PARTIAL = "set.reanalyze.partialRange";
    function staleBadgeKey() {
        const snap = local.partialRangeFor;
        if (!rangeLimited() || !snap) return STALE_KEY_PLAIN;
        const cur = config();
        return snap.loudness_mode === cur.loudness_mode &&
            snap.center_slot_policy === cur.center_slot_policy
            ? STALE_KEY_PARTIAL
            : STALE_KEY_PLAIN;
    }
    function renderStaleBadges() {
        const key = staleBadgeKey();
        const t = getT() || {};
        for (const node of [el.loudnessStale, el.centerStale]) {
            if (!node) continue;
            attr(node, "data-t", key);
            text(node, hasOwn(t, key) ? t[key] : key);
        }
    }

    function syncStale() {
        // [SL-279 复审第 6 轮] 框开着时用户仍可能在 Tab3 改范围档,提示要跟着当前 state 走 ——
        // 只在开框那一下算一次的话,提示会停在开框时的档位上。
        syncReanalyzeRangeNote();
        // [SL-354 复审第 1 轮] 影响面说明段按触发字段换词条。**放在所有早退之前**:
        // 下面几支会在「这一帧不作数」「不是用户刚改的档」等情形早退,而框开着切语言
        // 时靠的就是每次 render 都重填这一段(与 renderRangeDone 同一个理由)。
        syncReanalyzeScopeNote();
        // [SL-279] 基线来自 state 的 `analysis.applied.*`(上次全量分析所用),不再是本地快照。
        // [SL-278] **逐项判**:两枚徽标各挂各的控件,合成一个布尔会让它们同亮同灭。
        const cur = config();
        const applied = appliedAnalysisConfigOf(getStore().state);
        // [SL-354] `applied` 缺字段时上面那个取值会**回落到当前值**(见它的头注),回落之后
        // 派生的 stale 恒假,与「基线真的等于当前值」长得一模一样。徽标照回落值渲染是设计
        // (旧插件下不亮),但下面那支破坏性分支不许吃回落值 —— 见它自己那段注释。
        const hasApplied = hasAppliedAnalysisConfig(getStore().state);
        const loudnessStale = analysisConfigStale(
            cur.loudness_mode,
            applied.loudness_mode,
        );
        const centerStale = analysisConfigStale(
            cur.center_slot_policy,
            applied.center_slot_policy,
        );
        show(el.loudnessStale, loudnessStale);
        if (el.loudnessStale)
            attr(el.loudnessStale, "data-stale", loudnessStale ? "1" : "0");
        show(el.centerStale, centerStale);
        if (el.centerStale)
            attr(el.centerStale, "data-stale", centerStale ? "1" : "0");
        renderStaleBadges();

        // [SL-354] 弹窗判据由「只读响度」改成**两项都算**。用户 v5.6.7 实测:「B3(中央槽
        // 策略)完全没有弹出弹窗,应该和前面一样」。原来只读响度是 SL-276 按当时的用户
        // preview 口径**有意**做的(不是缺陷),这次用户改了口径 ⇒ 连同 wireSeg 的开闸点、
        // 这两处注释、以及 SL-276 那格钉住旧判据字面的用例一起翻面。
        //
        // ⚠ **不要在注释里逐字引用被判据钉住的那行代码。** 本卡实测:我第一版在这里把旧
        // 写法原样引了一遍,`smoke-tab4-settings` 那格源码级正则**当场被注释喂饱** ——
        // 判据该红没红。那格不剥注释(与 SL-297 记的「文本级判据先剥注释」同一族),
        // 要引就描述它,别抄它。
        const stale = loudnessStale || centerStale;

        // [SL-354] **待观察的那次写还没在 state 里露面 ⇒ 这一帧的读数不作数,什么都不做。**
        // 这一句同时修掉用户报的第 ① 条(「第一下只出横幅、第二下才弹」):点档 → 回执 →
        // 置闸 → requestRender(),而真桥上这时 state 还没回来,派生的 stale 为假,下面
        // `!stale` 那支会把刚置起的闸清掉。区分「写没回来」与「用户改回去了」靠的就是
        // `askPending` 里记的那个值 —— 布尔位没有这个信息。
        const pending = local.askPending;
        if (pending && cur[pending.field] !== pending.value) return;
        // ⚠ 这把尺子分不出第三种形态:**非用户路径**(撤销 / 切版本 / 快照恢复)把这一项
        // 改到了别的值。它与「写还没回来」在这一帧上逐字节相同,没有第二个信号能区分。
        // 代价是:框正开着时来这么一下,框不会自动关(旧实现会关)。取这一侧是因为
        // 反过来的代价是用户报的 ①②(该弹不弹 / 弹了就没),而这一侧用户随手可解 ——
        // 撤销更改 / Esc / 遮罩三个出口照常关框,再改一次档也会重新置闸。
        // 写落地之后**本位也不清** —— 它此后充当「这一帧新不新」的尺子,一直留到
        // 「改回基线」那一支(下面 `!stale` 里)或者用户下一次写成功把它换掉为止。
        // 用户报的第 ② 条「弹窗出来一下就闪现消失了」不需要 applied 缺席就能发生 ——
        // 只要有一帧**内容早于这次写、送达晚于这次写**的全量快照(4Hz 下这一帧正常存在:
        // 快照在写落地前组装、写落地后送达),它带的 current 与 applied 都是旧值 ⇒ 两者相等
        // ⇒ 派生 stale 当场为假 ⇒ 旧实现关框 + 清闸,而且此后不再弹。有了这把尺子,
        // 那一帧因为「没显示出用户写的值」被直接判为不作数,上面那句 return 就挡住了。

        // 琥珀 badge 是**常驻状态位**(Esc / 点遮罩关掉框之后还看得见口径是脏的),纯派生;
        // [SL-371] 走「撤销更改」那条出口时它会灭 —— 那不是本位变了,是当前值真的回到了
        // 基线、派生结果跟着为假。别把「关了框徽标还亮」写成三条出口共通的性质。
        // 弹窗在同一判据之上**再加一道 askPending 闸**(只由用户点档写成功置起) ——
        // 理由见 local.askPending 那段列的那几条「用户什么都没做也为真」的路径。
        // reanalyzeAskedFor 则挡住每帧重弹。
        if (!stale) {
            // [SL-354] **回落值只许渲染,不许做破坏性判断。** 这份 state 根本没带
            // `applied` 的话,上面的 stale 是拿「基线 := 当前值」算出来的,它为假只
            // 说明**读不到基线**,不说明两者相等 —— 拿它来清闸 + 关框,就是 ② 的
            // 缺字段变体。真桥今天恒发这两个字段(见 hasAppliedAnalysisConfig 头注),
            // 所以这里是兜底闸:什么都不做,把框和闸原样留着,等一帧带得全的。
            if (!hasApplied) return;
            // 走到这里 = 这一帧**确实是新的**(上面那把尺子已经放行)、基线读得到,
            // 且当前 == 基线,也就是「用户自己改回去了」或「分析跑完把基线前移了」
            // —— 这才该清、该关。
            // ⚠ [SL-354 复审第 2 轮] 下面**两行各自都有一条搭档**,单删任一行本套都不红
            // (实测),所以别照单行写删除式:
            //   · `reanalyzeAskedFor = null` 的搭档 = wireSeg 里「一次新的用户写就清它」;
            //     两道一起拆 ⇒ C10e3 与 C10h 红。这一行管「先经本支清过」那条入口。
            //   · `askPending = null` 的搭档 = 下面那句 `if (!pendingStale) return;`;
            //     两道一起拆 ⇒ C10e2 与 C10g 红。这一行管「这条待观察记录该作废了」,
            //     `pendingStale` 管「要弹的是刚改走的那一项」,两条独立成立,都留。
            local.reanalyzeAskedFor = null;
            local.askPending = null;
            // [SL-375] 徽标已经灭了,那份部分重算快照也没有消费者了。清它是卫生,
            // **不是判据**:`staleBadgeKey()` 的「快照 == 当前口径」那一项本来就兜得住
            // (走到这里当前 == 基线,而快照记的是改动后的那一份)。别把这一行
            // 当成 SL-375 的牙齿 —— 删掉它本套一格都不红(实测)。
            local.partialRangeFor = null;
            closeReanalyzeAsk();
            return;
        }
        if (!pending) return; // 不是用户刚改的档 ⇒ 只留 badge,不弹框
        // [SL-354 复审第 1 轮] **要弹的是「用户刚改走的那一项」自己脏**,不是「两项里随便
        // 哪一项脏」。上面的 stale 现在是两项取或,于是「两项都脏 → 把其中一项改回基线」
        // 会走不进 `!stale` 那支(另一项还脏),而 token 换了 ⇒ 框又弹一次,并把焦点抢到
        // 主钮上。用户刚**撤销**了自己的一个改动却收到一个 alertdialog —— 与 SL-276 立的
        // 「弹窗 = 用户刚把某一档改走了」对不上,也不在本卡四条用户反馈里。
        // 未知字段一律**不弹**(失效方向倒向安静):本位只由 wireSeg 的两个设置块写,
        // 出现第三种字段说明是别处写进来的,那更不该弹框。
        const pendingStale =
            pending.field === "loudness_mode"
                ? loudnessStale
                : pending.field === "center_slot_policy"
                  ? centerStale
                  : false;
        if (!pendingStale) return;
        const token = pending.field + "=" + pending.value;
        if (local.reanalyzeAskedFor !== token) {
            local.reanalyzeAskedFor = token;
            // **一次置位只换一次弹框**。要挡的形态([SL-276 二轮复审]立的):
            // 「改档 → 弹 → Esc 关框」之后,只读观察态收 scvb.state / 切版本 / 快照恢复把
            // 口径换到**另一个**脏值 —— 框照弹,而用户这一轮什么都没点。上一轮【重要】
            // 关掉的正是这类打断,换个入口又漏了回来。
            //
            // [SL-354] **承担者从 askPending 换成了本行这个 token**。SL-276 当时是在这里
            // 就地把开闸位清掉,现在**不能清** —— 它开框之后还要继续当「这一帧新不新」的
            // 尺子(见上面那段),清了 ② 就又漏回来。改成:token 里带上**字段名 + 值**,
            // 同一次写第二次走到这儿算出来的串逐字相同,于是不重开。
            // 带字段名是因为两个字段现在都能弹,只按值记的话「响度改成 X」与「中央槽改成
            // X」会互相冒充(两边枚举值域不同,今天撞不上,但那是**巧合**不是判据)。
            //
            // 记在 openReanalyzeAsk() **之前**:只读早退那条路也算消费掉(那次 asked 已按
            // 本值记下,转成可写态后同值不会补弹)。
            openReanalyzeAsk();
        }
    }

    function renderGuideRules() {
        const t = getT() || {};
        const have = GUIDE_RULE_KEYS.filter((k) => hasOwn(t, k));
        if (have.length === GUIDE_RULE_KEYS.length) {
            if (el.guideList) {
                el.guideList.replaceChildren(
                    ...GUIDE_RULE_KEYS.map((k, i) => {
                        const li = document.createElement("li");
                        if (i >= 3) li.setAttribute("data-rest", "1");
                        const num = document.createElement("span");
                        num.className = "settings-guideblock-rules__num";
                        text(num, i + 1 + ".");
                        const body = document.createElement("span");
                        body.setAttribute("data-t", k);
                        text(body, t[k]);
                        li.append(num, body);
                        return li;
                    }),
                );
            }
            show(el.guideMissing, false);
        } else {
            if (el.guideList) el.guideList.replaceChildren();
            show(el.guideMissing, true);
        }
    }

    function renderVersion(st) {
        if (el.versionValue)
            text(el.versionValue, versionString(st && st.snapshot));
    }

    function renderStorage(t, st) {
        const s = storageOf((st && st.state) || {});
        if (el.storageValue) {
            const key = s.external
                ? "set.storage.external"
                : "set.storage.embedded";
            if (hasOwn(t, key)) {
                text(
                    el.storageValue,
                    fmt(t[key], { mb: formatMegabytes(s.bytes) }),
                );
            }
        }
        if (el.storageGuid) {
            const guid = (st && st.snapshot && st.snapshot.session_guid) || "";
            const key = "set.storage.sessionGuid";
            text(el.storageGuid, hasOwn(t, key) ? fmt(t[key], { guid }) : guid);
        }
    }

    function renderDiagnostics() {
        if (el.diagRows) {
            el.diagRows.replaceChildren(
                ...diagRowsOf(getStore()).map((r) => {
                    const row = document.createElement("div");
                    row.className = "settings-diagnostics__row";
                    const mk = (v, cls) => {
                        const s = document.createElement("span");
                        s.className = cls;
                        text(s, String(v));
                        return s;
                    };
                    row.append(
                        mk(r.ch, "settings-diagnostics__ch"),
                        mk(r.hb, "settings-diagnostics__hb"),
                        mk(r.mis, "settings-diagnostics__mis"),
                        mk(r.gen, "settings-diagnostics__gen"),
                        mk(r.seq, "settings-diagnostics__seq"),
                    );
                    return row;
                }),
            );
        }
        if (el.diagCopy) {
            const done = Date.now() < local.copyDoneUntil;
            const key = done ? "set.diag.copied" : "set.diag.copy";
            attr(el.diagCopy, "data-t", key);
            text(el.diagCopy, hasOwn(getT(), key) ? getT()[key] : key);
        }
    }

    function renderScale(st) {
        if (!el.scaleSelect) return;
        const ui = ((st && st.state) || {}).ui || {};
        const f = ui && Number.isFinite(ui.scale) ? ui.scale : 1;
        if (el.scaleSelect.value !== String(f))
            el.scaleSelect.value = String(f);
    }

    function render() {
        const st = getStore();
        syncOptions();
        syncStale();
        renderGuideRules();
        renderVersion(st);
        renderStorage(getT(), st);
        renderDiagnostics();
        renderScale(st);
    }

    /**
     * 分析完成(scvb.segments reason ∈ {analyze, vad, segmentation})→ 基线同步为当前值
     * (03 §6.3:全量分析完成时同步 applied.*,派生 stale 归零)。快照/切版本不同步。
     *
     * ⚠ **三个 reason 都要认**([SL-255])。松手档(`vad`/`segmentation`)自 [J95③a] 起
     * 跑的是**同一条完整流水线**、用的是**当前的 `loudness_mode`**,产出与「点分析」同质;
     * 只认 `analyze` 的话,拖 VAD 滑杆松手重分段完成后基线不同步,派生 stale 不归零 ——
     * 「参数已改、结果陈旧」提示会一直挂着,而结果其实已经是新的。
     * (这条不一致是本卡新引入的:此前这条路根本不存在。)
     */
    function onSegments(seg) {
        // [SL-279] 这里原来还顺手同步一次本地基线 —— 基线现在由 native 落在
        // `analysis.applied.*` 里,随 `scvb.state` 到达,不再需要(也不该)由段表事件推。
        // 段表回推仍要重渲染:徽标读的是 state,而这一帧的 state 可能刚随分析完成更新。
        if (
            seg &&
            (seg.reason === "analyze" ||
                seg.reason === "vad" ||
                seg.reason === "segmentation")
        ) {
            requestRender();
        }
    }

    return { mount, render, onSegments, expandNine };
}
