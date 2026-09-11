// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// analyze 受理回执 → 该弹哪一条提示([SL-396];跨页共用)
// =============================================================================
// **为什么单独一份**:这一条判据有两个消费者 —— 波形与分段页(tab-wave.js 的四处调用点)与
// 设置页的重分析确认框(tab-settings.js 的 doReanalyzeFromAsk)。内联复刻第二份,正是这一族
// 缺陷反复复发的形状(同款头注见 `web/shared/host-echo.js` 的 `hostEchoUseWideWindow`)。
//
// ⚠ **[#256 复审②] 为什么不是一个页面 import 另一个页面**:两个 tab 模块都是**工厂函数**
// (`createXxxTab(...)`),它们内部的函数**不能** `export`(export 只能在模块顶层)——
// 第一版就是这么写的,结果是 `SyntaxError`,整页起不来,`smoke-tab3` / `smoke-seg-restore-page`
// 双双红在「① 泳道上真点一下能选中一个段」这种最靠前的格上。所以挪到 shared/。
// =============================================================================

/**
 * analyze **受理回执** → 该弹哪一条行内提示(`null` = 受理了,不必弹)。
 *
 * 为什么要有这一处:此前**五处**调用点都不看回执 —— §1.6 的两条拒绝态(范围 ∩ 覆盖 = ∅
 * 回 `{ok:false, affected:{0,0,0}}`,**不带 reason**;已有分析在跑回
 * `{ok:false, reason:"busy"}`)在屏上都与「受理了」一模一样,用户看到的是「点了没反应」。
 *
 * ⚠ 只认 `ok === false`。回执缺席(`!res`)与 `observer`(**只读观察态**)不在这里出提示:
 * 前者是「桥没回话」,后者 §5.1 已有它自己的面,且两页的这些钮本来就被 `isWriteBlocked()`
 * 挡着 —— 本卡不顺手扩面,要扩另立卡。
 *
 * @param {{ok?: boolean, reason?: string}|null|undefined} res `call("analyze", …)` 的受理回执
 * @returns {string|null} 词条 key(`analyze.busy` / `analyze.refused`),或 null = 不弹
 */
export function analyzeRefusalNote(res) {
    if (!res || res.ok !== false) return null;
    return res.reason === "busy" ? "analyze.busy" : "analyze.refused";
}
