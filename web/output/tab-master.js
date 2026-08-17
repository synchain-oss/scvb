// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · Tab1「整体调整」—— 状态机与桥接线(T31 Wave 2 交付物)。
// -----------------------------------------------------------------------------
// 现状:**空壳**。Wave 1(视觉复现)完全靠 index.html 的 data-* 属性 + CSS 选择器
// 承载全部状态视觉,不需要样式之外的逻辑;唯一的结构性模板生成(八组胶囊、分布图柱体、
// Lead Select 选项、缩放档位)与页面其他 tab 同源,留在 web/output/app.js 里。
// 本文件先立好位置与边界,Wave 2 再填 —— 提前搬迁只会把 app.js 的 i18n/tab 路由
// 也拖进来,反而制造第二个入口。
//
// Wave 2 落地时本文件负责(且只负责)Tab1:
//   消费(契约 §2):
//     scvb.state    → 三件套 data-cap / data-analyze / data-out / data-lock、
//                     range 档位 data-range / data-loop-missing、组胶囊 aria-pressed、
//                     Lead Select data-selected、print_guard 小锁、recapture badge 位
//     scvb.params   → width / ms_balance / lead_select 跟随(--v / --ms-left / --ms-w、
//                     ±{θ}° 大读数 θ=round(width×0.6));hostEcho 时灰显且绝不回写(§0.5)
//     scvb.groups   → 八胶囊 data-online(事件缺失时绿点全灭、零报错)
//     scvb.playhead → 采集中 / 已离开采集范围 提示与 PRINT 判定
//     scvb.segments → 分析完成反馈(data-analyze="done" 闪绿)+ J69 stale 提示位
//     scvb.error    → 横幅/toast 按 05 §5.1 落点(active:false 撤下)
//   上行(契约 §1):
//     setCaptureEnabled / previewAnalyze(节流)/ analyze / cancelAnalyze /
//     setOutputEnabled / setGroupId(确认条后才调;PRINT 态 disabled)/
//     beginParamGesture + setParam + endParamGesture 三段式(双击回默认)/
//     setRange(三档 + manual 起止校验)/ setTransitionRamp / setPanCurve(T34 接管窗内)
//   页面内状态机:
//     write 确认条(每工程会话首次 OFF→ON 就地展开;follow 档走
//       out.master.writeConfirm.follow,无 {x}–{y} 空洞;确认后本会话不再弹;
//       与加载守卫互斥)、加载守卫(?scenario=print-guard)、过渡卡斜坡随值变形。
//
// 纪律(与 index.html 头注同源):状态一律改 data-* 属性,不拼 class 字符串;
// 词条一律走 web/shared/i18n.js 的 key,禁止硬写自由文案。
// =============================================================================

export {};
