// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB · demo 轨名本地化(主界面轨列表;与 tour.js buildDemoStore 同源 getT)
// -----------------------------------------------------------------------------
// 职责边界:
//   • demo 模式的 15 条轨名是我们自己造的 UI 内容(mock-data.js DEMO_TRACKS 供数,
//     zh 原值),切语言时应跟随 i18n demo.ch* 本地化 —— 与 buildDemoStore 同一份词条。
//   • 真实插件路径(DAW 轨名 kChannelUIDKey)是**用户数据**,绝不改写:只有 label 逐字
//     等于未改动的 zh demo 轨名(DEMO_LABELS 原值)才映射,否则原样返回。
//   • 纯函数、无 DOM、node 可直接 import 断言;不 import i18n.js(t 由调用方传入,
//     依赖方向与 mock-data.js 的「数据源不依赖 UI 层」纪律一致)。
// =============================================================================

import { DEMO_LABELS } from "./mock-data.js";

/** demo 轨名词条 key(ch=1..15 → "demo.chN";与 buildDemoStore 同一派生)。 */
export function demoChannelKey(ch) {
    return "demo.ch" + ch;
}

/**
 * 主界面轨名显示值:仅当 label 是未改动的 zh demo 轨名时本地化。
 *
 * @param {number} ch    通道号 1..15
 * @param {string} label 事件仓里的原始 label(scvb.state.channels[].label)
 * @param {object} t     当前语言字典(getT() 的结果;空对象/undefined = 不本地化)
 * @returns {string} 本地化后的显示名;非 demo 原值则原样返回 label。
 */
export function demoChannelLabel(ch, label, t) {
    if (t && label === DEMO_LABELS[ch - 1]) {
        const v = t[demoChannelKey(ch)];
        if (v) return v;
    }
    return label;
}
