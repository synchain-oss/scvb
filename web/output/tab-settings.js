// SPDX-License-Identifier: GPL-3.0-or-later
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
 * 响度口径「改后需重分析」判定:当前值 !== 基线值。
 * 改走 → true(提示出现);改回基线值 → false(提示立即消失)。纯函数供 node 断言。
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
        // [SL-276] 重分析提示弹窗(卡片层单例,不在 Tab4 子树里 —— 见 index.html 那段注释)
        reanalyzeAsk: $("reanalyze-ask"),
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

    // 页面内一次性状态(不属 state chunk,重开面板即重置):
    //   analysisConfigBaseline —— 响度口径的「基线值」= 分析已应用的口径。03 §6.3 把
    //   analysis_settings_stale 定义为 (当前 ≠ applied.*) 的派生字段,但 T25 冻结契约
    //   §1.1/§2.1 未暴露 applied.* / analysis_settings_stale,故 UI 以本地基线承载同语义:
    //   mount 时快照当前值(mock 环境即初值);真插件下由分析完成事件(onSegments)同步;
    //   stale = 当前值 !== 基线值 —— 改走提示出现,改回基线值立即消失。
    //
    //   **已知边界,归 [SL-279],本卡不动**:mount() 早于 bootInner() 的
    //   requestInitialState,所以这里快照到的是空 store 的默认档,不是工程存的那个值。
    //   由此派生出一对方向相反的偏差 —— 存成非默认档的工程一进 Tab4 琥珀 badge 就亮
    //   (误报);同一会话里把口径切回 mount 默认值时 stale 立刻归假、badge 灭,而段表
    //   其实还是按旧档分析的(漏报)。两条同根,都是「基线在 state 到达前快照」。
    //   看着像顺手能修(初始快照落地后补一次 syncBaseline),实际是**产品取舍不是清理**:
    //   契约没暴露 applied.*,UI 分不出「工程存的就是这个档且已按它分析过」与「上次改了档
    //   没重分析就存盘」,补同步等于把误报换成漏报。故整条留给 SL-279 评估,不在本卡顺手做。
    //   注意弹窗**不吃这条**:它另有 askOnNextStale 一次性闸(见下),三条误报路径都不弹框。
    const local = {
        analysisConfigBaseline: null,
        // [SL-276] 已就哪个口径值弹过框。**按值记而不是按布尔记**:改走 → 弹一次;
        // 点「稍后」后继续在别的档之间来回切,每换到一个新的脏值都该再弹一次;
        // 改回基线(stale 归 false)时清空,下次再改走照弹。
        reanalyzeAskedFor: null,
        // [SL-276 复审] 弹窗的触发面是**用户点击**,不是派生的 stale 位。
        // 琥珀 badge 可以纯派生(多一枚小标记的代价很小),模态框不行 —— stale 有三条
        // 「用户什么都没做也为真」的路径,升级成框之后每条都变成一次要点掉的打断:
        //   ① 开工程即真:mount() 在 app.js 里同步跑,那时 store.state 还是 {},基线取到的是
        //      ANALYSIS_CONFIG_DEFAULTS.loudness_mode;工程真值要等 bootInner() 的
        //      requestInitialState 落地。存成 rms/peak_dbfs 的工程一进 Tab4 就 stale 恒真,
        //      而首帧 scvb.segments 的 reason 是 "snapshot",onSegments 不认、纠不回来。
        //   ② 只读观察态(J69):主实例改档,观察实例经 scvb.state 收到新值也会 stale ——
        //      框里那枚「重新分析」是写控件,契约 §5.6 要求只读态下写控件一律不可操作。
        //   ③ 切版本 / 快照恢复:基线刻意不随这两条同步(见 analysisConfigBaseline 那段),
        //      而那个版本的段表与它自己的口径本来就是对齐的。
        // 本位只由 wireSeg 里 loudness_mode **写成功**的回调置起,syncStale 之外无人写它。
        // ①②③ 三条因此一次性关掉,而琥珀 badge 的既有语义一个字节没动。
        // **一次性**:syncStale 真开框那一下就地清掉(见那处注释)。留着的话「稍后」
        // 关框之后本位仍为真,后续任何非用户驱动的口径变化都能再弹一次 —— ①②③ 换个
        // 入口又漏回来。下一次要弹,得由 wireSeg 里新的一次写成功重新置位。
        askOnNextStale: false,
        // 开框前的焦点落点,关框时还回去(「稍后」/ 遮罩 / Esc 三个出口都走 closeReanalyzeAsk)。
        reanalyzeReturnFocus: null,
        // analyze("all") 在途:主钮置灰 + 早退,防连点打出第二发(见 doReanalyzeFromAsk)。
        reanalyzeInFlight: false,
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
                // 不做乐观 dirty —— syncStale 按「当前值 vs 基线」派生提示;
                // 且该提示只由 loudness_mode 承载(center_slot_policy 不弹,用户 preview 口径)。
                //
                // [SL-276 复审] **这里是弹窗唯一的开闸点**:写真的被受理了,才允许下一次
                // syncStale 把框推到眼前。放在 res 判定之后 —— 桥缺失 / observer 拒 / badArg
                // 上面已经早退,走到这儿就是「用户刚改了档且改成了」。
                if (field === "loudness_mode") local.askOnNextStale = true;
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
        // 基线 = mount 时当前响度口径(mock 环境即初值;真插件下由 onSegments 同步)
        local.analysisConfigBaseline = config().loudness_mode;
        if (el.guideExpand)
            el.guideExpand.addEventListener("click", toggleNine);
        if (el.reopenTour) el.reopenTour.addEventListener("click", reopenTour);
        if (el.viewWorkflow)
            el.viewWorkflow.addEventListener("click", viewWorkflow);
        if (el.docs) el.docs.addEventListener("click", openDocs);
        if (el.diagChevron)
            el.diagChevron.addEventListener("click", toggleDiag);
        if (el.diagCopy) el.diagCopy.addEventListener("click", copyDiag);
        // [SL-276] 弹窗三个出口:「稍后」/ 点遮罩本身 / Esc —— 都只关框,不写任何 state。
        if (el.reanalyzeAskLater)
            el.reanalyzeAskLater.addEventListener("click", closeReanalyzeAsk);
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
            // Tab 在那一小段时间里等于失灵。置灰的只可能是主钮,故退到「稍后」。
            const focusable = (pref, alt) =>
                pref && pref.disabled !== true ? pref : alt;
            if (e.shiftKey && here === first) {
                e.preventDefault();
                focusable(last, first).focus({ preventScroll: true });
            } else if (!e.shiftKey && here === last) {
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
        if (
            el.reanalyzeAskPrimary &&
            typeof el.reanalyzeAskPrimary.focus === "function"
        )
            el.reanalyzeAskPrimary.focus({ preventScroll: true });
    }

    // 「重新分析」= 契约 §1.6 analyze("all")(全轨全时长;设置页没有选区概念)。
    // 受理回执之外什么都不做:结果经 §2.8 回推,基线由 onSegments 同步、琥珀 badge 自己灭。
    //
    // [SL-276 复审] **拒绝态不关框**。§1.6 会回 {ok:false, reason:"busy"}(已有分析在跑),
    // §5.6 会回 {observer:true};先关框再发请求的话,这两种情况下框没了、琥珀 badge 还挂着、
    // 也没有任何别的反馈 —— 看起来就是「这枚钮坏了」。框留着 = 这一下没生效、可以再点,
    // 与 wireSeg 里「被拒就只 requestRender、不落乐观值」是同一口径(本仓不用 toast)。
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
    //   ③ 防连点由 `reanalyzeInFlight` 早退与 `disabled` 两道**各自独立**挡住。冒烟 C4c
    //      钉的是「连点两下只打出一发 analyze」,**两道都拆掉才会红**(留一道仍守得住);
    //      C4b 钉的是另一件事 —— 跑完一定解锁(finally 丢了就永久停在 disabled)。
    async function doReanalyzeFromAsk() {
        if (local.reanalyzeInFlight) return;
        local.reanalyzeInFlight = true;
        const btn = el.reanalyzeAskPrimary;
        if (btn) {
            btn.disabled = true;
            btn.setAttribute("data-disabled", "1");
        }
        try {
            const res = await call("analyze", "all");
            if (!res || res.observer || res.ok === false) {
                requestRender();
                return;
            }
            closeReanalyzeAsk();
            requestRender();
        } finally {
            local.reanalyzeInFlight = false;
            if (btn) {
                btn.disabled = false;
                btn.removeAttribute("data-disabled");
                if (el.reanalyzeAsk && !el.reanalyzeAsk.hidden)
                    btn.focus({ preventScroll: true });
            }
        }
    }

    function syncStale() {
        const stale =
            local.analysisConfigBaseline !== null &&
            analysisConfigStale(
                config().loudness_mode,
                local.analysisConfigBaseline,
            );
        show(el.loudnessStale, stale);
        if (el.loudnessStale)
            attr(el.loudnessStale, "data-stale", stale ? "1" : "0");

        // 琥珀 badge 是**常驻状态位**(点过「稍后」之后还看得见口径是脏的),纯派生;
        // 弹窗在同一判据之上**再加一道 askOnNextStale 闸**(只由用户点档写成功置起) ——
        // 理由见 local.askOnNextStale 那段的 ①②③。reanalyzeAskedFor 则挡住每帧重弹。
        if (!stale) {
            local.reanalyzeAskedFor = null;
            local.askOnNextStale = false;
            closeReanalyzeAsk();
            return;
        }
        if (!local.askOnNextStale) return; // 不是用户刚改的档 ⇒ 只留 badge,不弹框
        const mode = config().loudness_mode;
        if (local.reanalyzeAskedFor !== mode) {
            local.reanalyzeAskedFor = mode;
            // [SL-276 二轮复审] **弹之前就地消费掉这一位**,一次置位只换一次弹框。
            // 不清的话它要等 stale 归假才灭,于是「改档 → 弹 → 稍后」之后本位仍为真;
            // 此后 ②(只读观察态收 scvb.state)或 ③(切版本 / 快照恢复)把口径换到
            // **另一个**脏值,reanalyzeAskedFor !== mode 就成立 —— 框照弹,而用户这一
            // 轮什么都没点。上一轮【重要】关掉的正是这类打断,换个入口又漏了回来。
            // 清在 openReanalyzeAsk() 之前:只读早退那条路也算消费掉(那次 asked 已按
            // 本值记下,转成可写态后同值不会补弹),免得本位在只读实例里一直挂着。
            local.askOnNextStale = false;
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
        if (
            seg &&
            (seg.reason === "analyze" ||
                seg.reason === "vad" ||
                seg.reason === "segmentation")
        ) {
            local.analysisConfigBaseline = config().loudness_mode;
            requestRender();
        }
    }

    return { mount, render, onSegments, expandNine };
}
