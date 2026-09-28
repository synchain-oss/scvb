// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// 宿主专属提示([J150] ①;03 §4.2 REAPER / §4.4 Ableton Live)—— 纯判据,零 DOM
// =============================================================================
// 数据源:桥面 §1.1 快照的 `host`(闭集 reaper / live / cubase / other,native 侧取值口径在
// `src/output/HostId.h`)。页面**只对 reaper / live 出提示**;cubase、other、不认识的值、
// 字段缺席 —— 一律不出。
//
// 三条横幅都落在 05 §2.0 横幅区,都是**建议类**(不是 §5.1 错误码),所以都带 ✕,
// 显隐走 app.js 的 `showDismissible()`([SL-373] 口径:条件为假就删「关过」的记录):
//
//   · `reaperKeepOpen` —— REAPER ∧ 输出开关 ON(写入自动化档,ARMED 或 PRINT)。
//     03 §4.2 对策③「UI 常显『打印期间请保持插件窗口打开』」(RD-04:REAPER 在插件窗口
//     关着时可能不写自动化)。挂在「输出 ON」而不是只挂 PRINT:要在用户**按播放之前**
//     就看得到 —— 等进了 PRINT 才出,窗口那时可能已经关了,横幅也就没人看见。
//     ✕ 之后保持关着,直到输出关掉再打开才重新出现。
//   · `reaperPrintNote` —— REAPER ∧ 本会话**进过** PRINT。03 §4.2 对策②「进入 PRINT 时弹
//     一次性提示」,内容是 REAPER 首选项里那条宿主端解法。闩锁本会话内不清 ⇒ ✕ 一次,
//     本会话不再出现(「一次性」)。**不随打印结束收起**:它讲的是「写完没录到自动化怎么办」,
//     用户最需要它的那一刻恰好在打印之后。
//   · `liveReEnable` —— Live ∧ 打印已结束(PRINT → 非 PRINT 的边沿置位,下一次进 PRINT 清位)
//     ∧ **走带停了或输出关了**。03 §4.4 对策①「打印结束时 UI 提示点 Re-Enable Automation」。
//     后一半是 #324 复审采纳的:循环区跨出写入范围时,每一圈出范围都是一次 PRINT → ARMED,
//     而下一圈回到范围里又会接着写 —— 这时说「写入已结束、去点 Re-Enable」既不对、点了也
//     留不住(下一圈又会把按钮点亮),横幅还会随每一圈忽隐忽现。所以只在「这一段写入真的
//     告一段落」(停走 / 关输出)时出。✕ 之后到下一次打印结束才再出。
//
// 「本会话」= 这一个页面实例的寿命(关掉插件窗口再打开 = 新会话),与 `dismissedBanners`
// 同一份口径:不入 state chunk、不落盘、不进契约。
// =============================================================================

/** 三条横幅的锚点名(data-gb)。app.js 的 ✕ 接线循环读这一份,别另抄名单。 */
export const HOST_HINT_BANNERS = Object.freeze([
    "banner-reaperKeepOpen",
    "banner-reaperPrintNote",
    "banner-liveReEnable",
]);

/**
 * §1.1 快照里的宿主标识。字段缺席 / 不是字符串 ⇒ `"other"`(= 不出任何提示)。
 * @param {object|null} snapshot  store.snapshot
 * @returns {string}
 */
export function snapshotHost(snapshot) {
    const h = snapshot && snapshot.host;
    return typeof h === "string" ? h : "other";
}

/**
 * 打印边沿记账。**逐事件调用**(scvb.state / scvb.playhead 各一处,见 app.js
 * `trackHostHintEdges`),不寄生在 render 里:render 是 rAF 合帧的,只看得到每帧最后一份快照,一帧之内
 * 进出 PRINT 各一拍会被合掉(与 app.js `trackRecapOutput` 同一条理由)。
 *
 * 就地写 `session` 的三格:
 *   · `hintEverPrinted`   本会话进过 PRINT(只置不清);
 *   · `hintPrintEnded`    上一次 PRINT 已结束、下一次还没开始(进 PRINT 清位。⚠ 清位这一句在
 *                         **页面上不可分辨**:PRINT 必然「输出 ON ∧ 在播」,而 hostHintFlags 的
 *                         ⑬ 在这时本来就不出 —— 它保的是这一格的记账语义,由 smoke-host-hints
 *                         的逐拍真值表钉住,不是由页面级那一套);
 *   · `hintWasPrinting`   上一拍是不是 PRINT(边沿记忆)。
 *
 * @param {object} session   store.session
 * @param {boolean} printing 此刻 `outputPhase(state, playhead) === "print"`
 */
export function trackPrintEdges(session, printing) {
    if (!session) return;
    if (printing) {
        session.hintEverPrinted = true;
        session.hintPrintEnded = false;
    } else if (session.hintWasPrinting) {
        session.hintPrintEnded = true;
    }
    session.hintWasPrinting = !!printing;
}

/**
 * 三条横幅各自的**条件**(不含「用户关过没有」那一层 —— 那一层归 showDismissible)。
 * @param {string} host      snapshotHost(...) 的结果
 * @param {object} state     store.state(§2.1 深合并结果)
 * @param {object} session   store.session(trackPrintEdges 写过的那一份)
 * @param {object} playhead  store.playhead(§2.6;缺席 = 不在播)
 * @returns {{reaperKeepOpen:boolean, reaperPrintNote:boolean, liveReEnable:boolean}}
 */
export function hostHintFlags(host, state, session, playhead) {
    const outputOn = !!(state && state.global && state.global.output_enabled);
    const playing = !!(playhead && playhead.isPlaying);
    const s = session || {};
    return {
        reaperKeepOpen: host === "reaper" && outputOn,
        reaperPrintNote: host === "reaper" && s.hintEverPrinted === true,
        // 「输出 ON 且还在播」= 这一段写入没告一段落(循环回范围里还会接着写),先不出
        liveReEnable:
            host === "live" &&
            s.hintPrintEnded === true &&
            !(outputOn && playing),
    };
}
