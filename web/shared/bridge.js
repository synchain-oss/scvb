// SPDX-License-Identifier: GPL-3.0-or-later
// bridge.js —— SCVB 前端桥:JS ↔ C++ 的唯一传输层(T27 交付,契约 docs/SCVB_CONTRACT.md v1.0 已冻结)。
//
// 【职责】把契约 §7 manifest 登记的名字原样接到两种后端上,别的什么都不做:
//   • 上行(UI → C++):对 BRIDGE_FUNCTIONS[role] 的每个名字生成一个 async 方法,参数原样透传;
//   • 下行(C++ → UI):对 BRIDGE_EVENTS[role] 的每个名字挂监听,统一经 on(evt, cb) 分发。
//   页面(T28/T32)只依赖本文件导出的 createBridge(),不直接碰 window.__JUCE__、不自建事件通道。
//
// 【后端切换语义】(照 Bridge 先例 Synchain/vst-plugin/web/bridge.js 的 createBridge 模式)
//   • 检测到**真 JUCE 宿主** ⇒ JuceBackend:惰性 import("../js/juce/index.js") 后取
//     getNativeFunction(name) 调用;事件走 window.__JUCE__.backend.addEventListener。
//     惰性是必需的 —— JUCE helper 在模块顶层就会碰 window(index.js 首行 import
//     "./check_native_interop.js"),静态 import 会让本文件在无 window 的环境里直接炸。
//   • 否则用调用方注入的 opts.mockBackend(浏览器预览入口,T28);
//     **本文件不含任何 mock/假数据实现**(mock 归 web-preview,01 §6.1)。两者都没有即抛错并给指引。
//   • **占位对象陷阱(判定为什么不是 `if (window.__JUCE__)`)**:JUCE 官方 helper
//     web/js/juce/check_native_interop.js(46-56 / 144-145 行)在**没有 JUCE 宿主的普通浏览器**里
//     会自己造一个占位 window.__JUCE__ —— 补上纯本地的 backend(emitEvent 只调空 postMessage)
//     与**空的** initialisationData.__juce__functions。于是预览页只要在 createBridge 之前
//     任何一处 import 过 js/juce/index.js 或 check_native_interop.js(某个 tab 模块顺手取
//     getNativeFunction、页面留一个 script 标签、写手的 JUCE 冒烟脚本,都算),
//     「__JUCE__ 存在即 JUCE」的旧判定就会走 JuceBackend 分支、**把注入的 mockBackend 整个忽略**,
//     所有 await 永久挂起(没有 C++ 侧回 __juce__complete),失败形态是**白屏 + 零报错**。
//     故判据收紧为 isJuceHost():backend 存在 **且** __juce__functions 是非空数组;
//     并让**显式注入优先** —— 占位对象 + 已注入 mockBackend ⇒ 走 mock 并 console.warn 一句。
//
// 【与契约的关系 —— 只做传输,不复制契约语义】
//   • 只登记名字,不复制载荷字段、不校验参数、不做夹取:越界/类型不符一律由 C++ 侧夹取或拒绝(§0.8)。
//   • 不代调 requestInitialState():§0.6 的 mBridgeReady 门控由页面掌握(首次装载、以及编辑器
//     关闭重建后,都必须由 UI 重新调用一次),桥不猜时机、不缓存快照、不重放事件。
//   • 不做节流 / diff-then-emit / 首帧必发(§0.4 全在 C++ 侧),也不按 hostEcho 过滤
//     (§0.5 的防回环是 UI 的纪律:引擎打印期间收到 scvb.params 只更新显示、绝不回写)。
//   • 枚举取值、载荷形状、秒↔样本换算一概不出现在本文件 —— 想知道某个名字收什么、回什么,读契约。
//
// 【parity】下面两张名表是 scripts/check-bridge-parity.mjs 的 B 侧比对面(脚本按正则读数组字面量,
//   不执行本文件),必须与契约 §7 manifest 逐字全等:output 35 函数 / 9 事件,input 7 函数 / 5 事件。
//   改名表 = 改契约,须走 §9「只增不改」流程并同批更新 C++ 常量表与 parity 脚本。
//
// 【mock 后端形状(T28 实现;§0.7 parity 纪律)】opts.mockBackend 必须是 window.__JUCE__.backend
//   的同形替身,以保证「mock 与 JuceBackend 导出名一致」由结构而非人工核对来保证:
//   • BRIDGE_FUNCTIONS[role] 的每个名字一个方法(可 async,返回契约 §0.8 的 JSON 对象);
//   • addEventListener(evtName, cb) 一个方法,用于推送 BRIDGE_EVENTS[role] 的事件。
//   缺名即在 createBridge() 处抛错并列出缺失项 —— 这是 §0.7 的运行期兜底(静态面由 parity 脚本把关)。

/**
 * 冻结名表 · native functions —— 逐字转写自契约 §7 manifest 的 output.functions / input.functions,
 * 顺序也照 manifest(顺序对 parity 无影响,照抄是为了人工比对时一眼可对)。
 */
export const BRIDGE_FUNCTIONS = {
    // Output —— 36 个(契约 §1)
    output: [
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
        "setAnalysisConfig",
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
        "confirmPrintGuard",
        "setMasterChartMode",
        "exportSuggestions",
    ],
    // Input —— 8 个(契约 §3)。与 Output 同名的 requestInitialState / setGroupId / setUiScale /
    // commitUiScale / setLang 是契约认可的同名同签名项(§7),名字只在各自侧内查重、不跨侧查重。
    input: [
        "requestInitialState",
        "setChannelId",
        "setGroupId",
        "remoteSetPriority",
        "setUiScale",
        "commitUiScale",
        "setLang",
        "setGuideSeen",
    ],
    // Monitor —— 5 个(契约 §10;[J81] 转正)。除 setObservedGroup 外四个由 WebViewHost 基类注册。
    // setObservedGroup 刻意不叫 setGroupId:§1.4 的 setGroupId 是 Output 的改组(断开本组全部
    // 连接、要弹确认条),这里只是「换一个组的 viz 段来看」,不 claim、对被观察组零副作用。
    monitor: [
        "requestInitialState",
        "setObservedGroup",
        "setUiScale",
        "commitUiScale",
        "setLang",
    ],
};

/**
 * 冻结名表 · events —— 逐字转写自契约 §7 manifest 的 output.events / input.events。
 * 事件是纯下行:UI 只订阅,绝不用这些名字发起上行调用(§0.5)。
 */
export const BRIDGE_EVENTS = {
    // Output —— 9 个(契约 §2)
    output: [
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
    // Input —— 5 个(契约 §4)
    input: [
        "scvb.state",
        "scvb.conn",
        "scvb.config",
        "scvb.groups",
        "scvb.error",
    ],
    // Monitor —— 4 个(契约 §10;[J81] 转正)。scvb.groups 与 scvb.playhead 逐字复用 Output 侧
    // §2.4/§2.6 的既有载荷形状,不另立一套 —— 轨迹图与组胶囊的消费代码因此一行不改。
    monitor: ["scvb.state", "scvb.groups", "scvb.viz", "scvb.playhead"],
};

// 名表冻结,防止调用方就地改写(T28 的 mock 若靠 push 补名,parity 脚本静态扫不出来)。
// 注意:必须写成「先声明字面量、再后置 freeze」——parity 脚本按 `<表名> = {` 的字面量抽取,
// 写成 Object.freeze({...}) 会让 B 侧抽取直接落空。
for (const table of [BRIDGE_FUNCTIONS, BRIDGE_EVENTS]) {
    Object.freeze(table.output);
    Object.freeze(table.input);
    Object.freeze(table.monitor);
    Object.freeze(table);
}

/**
 * **待转正**桥函数 —— 走冻结契约变更流程(仓 CLAUDE.md §5 / 契约 §9.0)、native 侧尚未落地的名字。
 *
 * 为什么单立一张表,而不是直接往 `BRIDGE_FUNCTIONS` 里加一行:那张表是
 * `scripts/check-bridge-parity.mjs` 的 B 侧比对面,与契约 §7 manifest **逐字全等**是硬门禁
 * (含 35/9/7/5 四个计数断言)。契约还没改就往里加名字,门禁必红 —— 而门禁红了是对的,
 * 它正在说「你改了桥面却没改契约」。所以待转正的名字停在本表:
 *
 *   • **能力探测后才挂**:mock 后端实现了就挂得上(预览页当场可用、往返可验),
 *     真 JUCE 宿主要等它把名字登记进 `__juce__functions` 才挂 —— 没登记就**不挂**,
 *     调用方拿到的是 `undefined`,与「桥函数不存在」逐字同一形态,不会有半通不通的中间态;
 *   • 转正时:契约 §7 manifest + 正文 + C++ 常量表 + 本文件 `BRIDGE_FUNCTIONS` 同批加名,
 *     并把该名字从本表**删掉**(留着会让它绕过 parity 门禁,那才是真正的洞)。
 *
 * **现存条目:无。** `exportSuggestions`(Output)与 `setGuideSeen`(Input)已随 [J81] 修宪
 * 转正进 `BRIDGE_FUNCTIONS`(契约 §1.36 / §3.8 + §7 manifest),按本注第二条从本表**删除** ——
 * 留着会让它们绕过 parity 门禁,那才是真正的洞。本表保留空壳供下一个待转正名字使用。
 */
export const PENDING_FUNCS = {
    output: [],
    input: [],
    monitor: [],
};
for (const side of ["output", "input", "monitor"])
    Object.freeze(PENDING_FUNCS[side]);
Object.freeze(PENDING_FUNCS);

/** 合法角色 —— 契约的 Output / Input / Monitor 三侧(Monitor 由 [J81] 随 ipc v1.6 修宪转正,契约 §10)。 */
const ROLES = ["output", "input", "monitor"];

/**
 * 返回对象上的保留键。契约冻结后函数名只增不改,万一将来新增的名字撞上这三个键,
 * 必须先改本文件(而不是让桥函数静默覆盖 on / role / isPreview)。
 */
const RESERVED_KEYS = new Set(["role", "isPreview", "on"]);

/**
 * 极简事件分发器(照 Bridge 先例 bridge.js 22-28 行的 makeEmitter,补了退订)。
 * 同一事件的回调按注册顺序同步调用;派发前取快照,允许回调内退订。
 */
function makeEmitter() {
    const map = new Map();
    return {
        on(evt, cb) {
            if (typeof cb !== "function") {
                throw new TypeError("bridge.on(evt, cb):cb 必须是函数");
            }
            let list = map.get(evt);
            if (!list) {
                list = [];
                map.set(evt, list);
            }
            list.push(cb);
            return function off() {
                const i = list.indexOf(cb);
                if (i >= 0) list.splice(i, 1);
            };
        },
        emit(evt, payload) {
            const list = map.get(evt);
            if (!list) return;
            for (const cb of list.slice()) cb(payload);
        },
    };
}

/**
 * 事件接线 —— JuceBackend 与 mock 后端共用这一段:后端只需实现 addEventListener(name, cb)。
 * 只接本侧名表内的事件;C++ 若推了表外的名字,这里不会有监听者,parity 门禁会在静态面先报出来。
 */
function wireEvents(role, backend, em) {
    for (const name of BRIDGE_EVENTS[role]) {
        backend.addEventListener(name, (payload) => em.emit(name, payload));
    }
}

/** 订阅口 —— 事件名不在本侧名表内即抛错(拼错事件名比收不到事件更难查)。 */
function makeOn(role, em) {
    const allowed = new Set(BRIDGE_EVENTS[role]);
    return function on(evt, cb) {
        if (!allowed.has(evt)) {
            throw new RangeError(
                `bridge.on:事件名 ${JSON.stringify(evt)} 不在 ${role} 侧名表内(契约 §7);` +
                    `可用事件:${BRIDGE_EVENTS[role].join(", ")}`,
            );
        }
        return em.on(evt, cb);
    };
}

/**
 * 组装返回对象:{ role, isPreview, on, ...每函数方法 }。
 * call(name) 交由各后端提供;方法一律 async,参数原样透传(校验归 C++,§0.8)。
 */
function assemble(role, isPreview, em, call, hasPending) {
    const api = { role, isPreview, on: makeOn(role, em) };
    // 能力探测:真宿主传入按 `__juce__functions` 判的谓词,mock 不传(mock 已在
    // makeMockBridge 里断言实现了 BRIDGE_FUNCTIONS 全集,契约 §0.7)。
    const probe = typeof hasPending === "function" ? hasPending : null;
    const absent = [];
    for (const name of BRIDGE_FUNCTIONS[role]) {
        if (RESERVED_KEYS.has(name)) {
            throw new Error(
                `bridge:函数名 ${name} 与返回对象保留键冲突,须先改 bridge.js 的 RESERVED_KEYS 约定`,
            );
        }
        // **契约已收、native 尚未实现**的名字不挂 —— 挂了会让页面的
        // `typeof bridge.X === "function"` 特性探测判成「能用」,于是按钮点亮却什么也不做,
        // 那比「桥上没这个名字」更坏:它在假装写成了。契约与实现的时间差由此如实暴露。
        if (probe && !probe(name)) {
            absent.push(name);
            continue;
        }
        api[name] = call(name);
    }
    if (absent.length > 0) {
        console.warn(
            `[bridge] ${role}:契约 §7 有而宿主未登记的函数 ${absent.length} 个 —— ` +
                `${absent.join(", ")}。这些名字不会挂到桥上(调用方读到 undefined),` +
                `属「契约已转正、native 待落地」的正常中间态;native 接线后本条自动消失。`,
        );
    }
    // 待转正名字:后端**确实实现了**才挂(见 PENDING_FUNCS 头注)。挂不上时调用方
    // 读到 undefined,与「这个桥函数还不存在」是同一形态 —— 页面侧本来就得容错。
    for (const name of PENDING_FUNCS[role]) {
        if (RESERVED_KEYS.has(name) || name in api) continue;
        if (probe ? probe(name) : false) api[name] = call(name);
    }
    return api;
}

/**
 * 是否为**真 JUCE 宿主**(而不是 check_native_interop.js 造的占位对象,详见文件头【后端切换语义】)。
 *
 * 判据取 `initialisationData.__juce__functions` 非空数组:
 *   • 占位对象把它初始化成**空数组**(check_native_interop.js 58-67 行),恒为空;
 *   • 真宿主把每个 registered native function 的名字逐个登记进去
 *     (juce_WebBrowserComponent.cpp 265-266 行 `withInitialisationData("__juce__functions", …)`),
 *     且该表为空时 JUCE **根本不开** native integration(同文件 255-256 行提前返回)——
 *     即「空表」在真宿主里等价于「桥不可能通」,不存在把真宿主误判成占位的空间。
 */
function isJuceHost(juce) {
    if (!juce || !juce.backend) return false;
    const data = juce.initialisationData;
    const fns = data ? data.__juce__functions : undefined;
    return Array.isArray(fns) && fns.length > 0;
}

/**
 * JuceBackend —— 插件内(检测到真 JUCE 宿主)。
 * 照 Bridge 先例 bridge.js 41-47 行:helper 只 import 一次,每次调用取 getNativeFunction。
 */
function makeJuceBackend(role) {
    const em = makeEmitter();
    wireEvents(role, window.__JUCE__.backend, em);

    // 惰性加载 JUCE 官方前端 helper(web/js/juce/,JUCE 8.0.8 原样副本)。
    const nativePromise = import("../js/juce/index.js");
    const call =
        (name) =>
        async (...args) => {
            const { getNativeFunction } = await nativePromise;
            return getNativeFunction(name)(...args);
        };

    // 待转正名字的能力探测:真宿主把每个 registered native function 登记在
    // `initialisationData.__juce__functions`(见 isJuceHost 头注),照它判即可。
    const registered = new Set(
        (window.__JUCE__.initialisationData || {}).__juce__functions || [],
    );
    return assemble(role, false, em, call, (name) => registered.has(name));
}

/**
 * mock 后端 —— 浏览器预览(无 window.__JUCE__)。实现由 web-preview 注入,本文件只负责接线与校名。
 */
function makeMockBridge(role, mock) {
    if (typeof mock.addEventListener !== "function") {
        throw new TypeError(
            "createBridge:opts.mockBackend 缺少 addEventListener(evtName, cb) —— " +
                "mock 后端须与 window.__JUCE__.backend 同形(契约 §0.7)",
        );
    }
    const missing = BRIDGE_FUNCTIONS[role].filter(
        (name) => typeof mock[name] !== "function",
    );
    if (missing.length > 0) {
        throw new TypeError(
            `createBridge:opts.mockBackend 缺少 ${missing.length} 个 ${role} 侧函数` +
                `(契约 §0.7 要求逐一实现全集):${missing.join(", ")}`,
        );
    }

    const em = makeEmitter();
    wireEvents(role, mock, em);
    const call =
        (name) =>
        async (...args) =>
            mock[name](...args);

    return assemble(
        role,
        true,
        em,
        call,
        (name) => typeof mock[name] === "function",
    );
}

/**
 * 工厂 —— UI 只调这一个。
 *
 * @param {{role: "output"|"input"|"monitor", mockBackend?: object}} opts
 *   role       必填;非法即 throw(每个 target 的页面各自传死值,不做自动嗅探)。
 *   mockBackend 仅在**非真 JUCE 宿主**时使用(含 __JUCE__ 是 helper 占位对象的情形),
 *              由 web-preview 入口注入(T28)。
 * @returns {{role: string, isPreview: boolean, on: Function}} 另含本侧全部桥函数(均为 async)。
 */
export function createBridge(opts = {}) {
    const { role, mockBackend } = opts;
    if (!ROLES.includes(role)) {
        throw new TypeError(
            `createBridge:opts.role 必须是 ${ROLES.map((r) => JSON.stringify(r)).join(" / ")} 之一,实得 ${JSON.stringify(role)}`,
        );
    }
    const juce = typeof window !== "undefined" ? window.__JUCE__ : undefined;
    if (isJuceHost(juce)) {
        return makeJuceBackend(role);
    }
    if (mockBackend) {
        // 显式注入优先:__JUCE__ 存在但不是真宿主 ⇒ 是 helper 的占位对象,走 mock 并留一条痕迹,
        // 免得预览页「看起来接上了 JUCE」而实际在跑 mock(反之亦然)时无从判断。
        if (juce) {
            console.warn(
                "createBridge:检测到 JUCE helper 占位对象(window.__JUCE__ 存在但未登记任何 native 函数)," +
                    "已按注入的 opts.mockBackend 运行。",
            );
        }
        return makeMockBridge(role, mockBackend);
    }
    throw new Error(
        "createBridge:未检测到真 JUCE 宿主" +
            (juce
                ? "(window.__JUCE__ 只是 check_native_interop.js 造的占位对象,initialisationData." +
                  "__juce__functions 为空)"
                : "") +
            ",且未注入 opts.mockBackend。" +
            "浏览器预览请经 web-preview 入口加载 mock 后端(T28);bridge.js 本身不含任何假数据实现。",
    );
}
