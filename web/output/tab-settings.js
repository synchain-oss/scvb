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
        guideBox: $("settings-guideblock-rules"),
        guideList: $("settings-guideblock-rules-list"),
        guideMissing: $("settings-guideblock-rules-missing"),
        guideExpand: $("settings-guideblock-expand"),
        reopenTour: $("settings-reopentour"),
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
    const local = {
        analysisConfigBaseline: null,
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
        if (el.diagChevron)
            el.diagChevron.addEventListener("click", toggleDiag);
        if (el.diagCopy) el.diagCopy.addEventListener("click", copyDiag);
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
     * 全量分析完成(scvb.segments reason="analyze")→ 基线同步为当前值(03 §6.3:
     * 全量分析完成时同步 applied.*,派生 stale 归零)。局部分析/快照不同步。
     */
    function onSegments(seg) {
        if (seg && seg.reason === "analyze") {
            local.analysisConfigBaseline = config().loudness_mode;
            requestRender();
        }
    }

    return { mount, render, onSegments, expandNine };
}
