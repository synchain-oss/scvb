// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Monitor · 前端桥(T46;**已随 [J81]/ipc v1.6 修宪转正进契约 §7 + §10**)
// -----------------------------------------------------------------------------
// 【对表状态】函数名表与事件名表已与 T45 的实交付逐字对齐(**#94 `649c99f`** 的
//   `MonitorBridgeApi.h` / `MonitorEditor.cpp`;#92 已被 #94 取代,名表内容一致),
//   载荷形状三轮对表已收敛。
//
// 【⚠ 本表已转正,以 bridge.js 为准】[J81] 修宪后,契约 §7 manifest 有了 `monitor` 块、
//   `web/shared/bridge.js` 有了 `BRIDGE_FUNCTIONS.monitor` / `BRIDGE_EVENTS.monitor`,
//   `scripts/check-bridge-parity.mjs` 的 [M] 块按那两张表做三向比对(manifest ↔ bridge.js ↔
//   `src/monitor/MonitorBridgeApi.h`)。**本文件的两张表不再是比对面** —— 两份表并存期间
//   一律以 `bridge.js` 为准;本文件按文件头自陈应退化成 `createBridge({role:"monitor"})`
//   的薄封装或直接删除,列 T46 后续小项。
//
// 【⚠ 这些名字与形状现在由谁保证】**在 T44/T45 合入 `feature/v1` 之前,只有 mock 兜着。**
//   本分支上跑的是 `web-preview/mock/monitor-mock.js`,它按对表结论造数,smoke 断言的
//   也是它。真桥是否照办由两个 parity 比对面查(段 golden + `MonitorEditor.cpp` 的
//   `setProperty` 名集,见 `./viz-contract.js` 文件头);而那两面因为两个文件还不在
//   `feature/v1` 的树上,现在打的是 `[SKIP]` —— **rebase 之后才真正生效**。
//   已对着他们的分支头临时嫁接验证过:全绿零 SKIP。本页对桥侧的逐条要求列在
//   `./viz.js` 文件头「本页对桥侧的要求」那一节。
//
// 【为什么不直接用 `web/shared/bridge.js`】
//   那份文件只认 `output` / `input` 两侧,两张名表是 `scripts/check-bridge-parity.mjs`
//   的 B 侧比对面,与**冻结契约** docs/SCVB_CONTRACT.md §7 manifest 逐字全等(含
//   36/9/8/5/5/4 六个计数断言)。**本段描述的是 [J81] 转正之前的状态**,留作溯源:
//   当时 Monitor 是第三个 target,它的函数与事件**还没进契约**
//   —— 往那张表里加一行,parity 门禁必红,而门禁红了是对的:它正在说「你改了桥面
//   却没改契约」。所以 Monitor 侧的名表停在本文件,等 T44(viz 段修宪,ipc v1.6)与
//   T45(Monitor 壳)定稿后,按契约 §9.0 + 仓 CLAUDE.md §5 的冻结变更流程一次性
//   转正 —— 届时本文件退化成 `createBridge({role:"monitor"})` 的一层薄封装或直接删掉。
//   (同一条思路的既有先例:bridge.js 的 `PENDING_FUNCS`。那张表挂在 output 侧、
//    靠能力探测决定挂不挂;Monitor 整个 role 都还不存在,故只能自带一份。)
//
// 【本文件与 bridge.js 逐条对齐的地方】(将来合并时不必重想)
//   • **真 JUCE 宿主判据**照抄 `isJuceHost()`:`__JUCE__.backend` 存在 **且**
//     `initialisationData.__juce__functions` 是非空数组。只判 `window.__JUCE__` 会被
//     `web/js/juce/check_native_interop.js` 造的**占位对象**骗过去,失败形态是白屏零报错;
//   • **显式注入优先**:占位对象 + 已注入 mockBackend ⇒ 走 mock 并 warn 一句;
//   • JUCE helper **惰性 import**(它在模块顶层就碰 window);
//   • 只做传输:不校验参数、不夹取、不代调 requestInitialState、不缓存快照;
//   • 事件名不在表内即抛错(拼错事件名比收不到事件更难查)。
//
// 【只读纪律】Monitor 是纯观察器:**本表里没有任何一个写引擎状态的函数**。
//   `setObservedGroup` 有意**不叫** `setGroupId` —— 契约 §1.4 的 `setGroupId` 是
//   Output 的**改组**(会断开本组全部连接、要弹确认条),而这里只是「换一个组的 viz 段
//   来看」,不 claim、不写 registry、对被观察的那一组毫无副作用(J75 C「只读 attach,
//   不 claim」)。两件事共用一个名字迟早会有人照 §1.4 的语义去实现它。
//   `setUiScale` / `commitUiScale` / `setLang` 写的是**本实例自己的 UI 偏好**,
//   不属于被观察组的状态,故不违反只读。
// =============================================================================

/**
 * Monitor 侧函数名表(**临时**;转正前不进 check-bridge-parity 的比对面)。
 *
 *   requestInitialState()  §0.6 门控:返回首帧快照(见 app.js 的消费点)
 *   setObservedGroup(g)    g = 1..8;换观察对象,只读语义(见文件头)
 *   setUiScale(f)          10 秒防呆的预览档(与 §1.28 同形制)
 *   commitUiScale()        「保持」落盘系统级全局默认(§1.29 同形制)
 *   setLang(code)          zh|en|fr(§1.30 同形制)
 */
export const MONITOR_FUNCTIONS = [
    "requestInitialState",
    "setObservedGroup",
    "setUiScale",
    "commitUiScale",
    "setLang",
];

/**
 * Monitor 侧事件名表 —— 逐字对齐 T45 的 `src/monitor/MonitorBridgeApi.h`。
 *
 *   scvb.state     组回显 + 缩放/语言 + **viz 三态与 fresh**(段级状态的唯一真源)
 *   scvb.groups    §2.4 原样复用:`{groups_online}` 位图,组胶囊绿点
 *   scvb.viz       4Hz viz 帧(每轨摘要 + 按需重发的车道块;形状见 viz.js 头注)
 *   scvb.playhead  §2.6 原样复用:**25Hz**(WebViewHost 定时器上限)扁平标量集
 *
 * 后两个**逐字复用 Output 侧的既有载荷形状**,不另立一套 —— 轨迹图的
 * `onPlayhead(ev)` 与组胶囊的消费代码因此一行不改。
 *
 * `scvb.state` 的 viz 面为什么必须由 native 给:「Output 进程真没了」与「还在但不再
 * 发帧」在 UI 侧长得一模一样(命名段是引用计数存活的,只要 Monitor 自己不松手,段
 * 就一直在、读也一直成功)。T45 的做法是帧陈旧时松开映射再探一次 —— 这件事 UI 做不了。
 */
export const MONITOR_EVENTS = [
    "scvb.state",
    "scvb.groups",
    "scvb.viz",
    "scvb.playhead",
];

Object.freeze(MONITOR_FUNCTIONS);
Object.freeze(MONITOR_EVENTS);

/** 返回对象上的保留键(与 bridge.js 的 RESERVED_KEYS 同一条约定)。 */
const RESERVED_KEYS = new Set(["role", "isPreview", "on"]);

/** 极简事件分发器(照 bridge.js 的 makeEmitter,派发前取快照以允许回调内退订)。 */
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

function wireEvents(backend, em) {
    for (const name of MONITOR_EVENTS) {
        backend.addEventListener(name, (payload) => em.emit(name, payload));
    }
}

function makeOn(em) {
    const allowed = new Set(MONITOR_EVENTS);
    return function on(evt, cb) {
        if (!allowed.has(evt)) {
            throw new RangeError(
                `bridge.on:事件名 ${JSON.stringify(evt)} 不在 monitor 侧名表内;` +
                    `可用事件:${MONITOR_EVENTS.join(", ")}`,
            );
        }
        return em.on(evt, cb);
    };
}

function assemble(isPreview, em, call) {
    const api = { role: "monitor", isPreview, on: makeOn(em) };
    for (const name of MONITOR_FUNCTIONS) {
        if (RESERVED_KEYS.has(name)) {
            throw new Error(
                `monitor-bridge:函数名 ${name} 与返回对象保留键冲突,须先改 RESERVED_KEYS 约定`,
            );
        }
        api[name] = call(name);
    }
    return api;
}

/**
 * 是否为**真 JUCE 宿主**(判据与理由逐字同 `web/shared/bridge.js` 的 `isJuceHost`)。
 * 占位对象把 `__juce__functions` 初始化成空数组;真宿主该表为空时根本不开 native
 * integration —— 「空表」等价于「桥不可能通」,不存在把真宿主误判成占位的空间。
 */
function isJuceHost(juce) {
    if (!juce || !juce.backend) return false;
    const data = juce.initialisationData;
    const fns = data ? data.__juce__functions : undefined;
    return Array.isArray(fns) && fns.length > 0;
}

/**
 * Monitor 桥工厂 —— 页面只调这一个。
 *
 * @param {{mockBackend?: object}} opts `mockBackend` 由 web-preview 注入(壳页时序见
 *   `web-preview/shell.js` 文件头);真 JUCE 宿主下忽略。
 * @returns {{role:"monitor", isPreview:boolean, on:Function}} 另含本侧全部函数(均 async)。
 */
export function createMonitorBridge(opts = {}) {
    const mock = opts.mockBackend;
    const juce = typeof window !== "undefined" ? window.__JUCE__ : undefined;

    if (isJuceHost(juce)) {
        const em = makeEmitter();
        wireEvents(window.__JUCE__.backend, em);
        const nativePromise = import("../js/juce/index.js");
        const call =
            (name) =>
            async (...args) => {
                const { getNativeFunction } = await nativePromise;
                return getNativeFunction(name)(...args);
            };
        return assemble(false, em, call);
    }

    if (mock) {
        if (juce) {
            console.warn(
                "createMonitorBridge:检测到 JUCE helper 占位对象(window.__JUCE__ 存在但未登记" +
                    "任何 native 函数),已按注入的 opts.mockBackend 运行。",
            );
        }
        if (typeof mock.addEventListener !== "function") {
            throw new TypeError(
                "createMonitorBridge:opts.mockBackend 缺少 addEventListener(evtName, cb) —— " +
                    "mock 后端须与 window.__JUCE__.backend 同形",
            );
        }
        const missing = MONITOR_FUNCTIONS.filter(
            (name) => typeof mock[name] !== "function",
        );
        if (missing.length > 0) {
            throw new TypeError(
                `createMonitorBridge:opts.mockBackend 缺少 ${missing.length} 个函数` +
                    `(须逐一实现全集):${missing.join(", ")}`,
            );
        }
        const em = makeEmitter();
        wireEvents(mock, em);
        const call =
            (name) =>
            async (...args) =>
                mock[name](...args);
        return assemble(true, em, call);
    }

    throw new Error(
        "createMonitorBridge:未检测到真 JUCE 宿主" +
            (juce
                ? "(window.__JUCE__ 只是 check_native_interop.js 造的占位对象)"
                : "") +
            ",且未注入 opts.mockBackend。" +
            "浏览器预览请经 web-preview/monitor.html 加载 mock 后端;本文件不含任何假数据实现。",
    );
}
