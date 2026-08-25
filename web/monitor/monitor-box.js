// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Monitor · 设计盒(T46;[J75] C「Monitor 设计盒尺寸 T46 依内容定,机制同款」)
// -----------------------------------------------------------------------------
// **为什么不写进 `web/shared/design-box.js`**:那份文件是 `scripts/gen-design-box.py`
// 的输入,生成物 `src/core/DesignBox.h` 由 gates 3d 逐字节对拍 —— 往里加一个 `monitor`
// 键就必须同批重生成那个头文件,而本卡的纪律是**不碰 src/**(native 侧 Monitor 壳
// 归 T45)。故尺寸暂存本文件,由 Monitor 页面与 `web-preview/shell.js` 共用一份。
//
// **集成待办 —— T44/T45 侧已认领**(2026-08-25 对表回信),四处由他们一并做:
//   ① `web/shared/design-box.js` 加 `DESIGN.monitor = {w:960, h:720, presets:<下表>}`
//   ② 重跑 `python scripts/gen-design-box.py` 生成 `src/core/DesignBox.h`
//   ③ `scripts/check-design-box.mjs` 的 `EXPECT` 加 monitor 条目
//      —— 那份 hardcode 就是 gate 3d 本身,不同批改会红
//   ④ `BridgeBase.h` 的 `designBoxWindowSize` 加 monitor 分支
//      (现在非 "input" 一律回落 Output 的 1180×780)
// **他们落地之后,本文件改成转发**:`export const MONITOR_DESIGN = DESIGN.monitor;`
// (或直接删掉,页面与 `web-preview/shell.js` 改引 `web/shared/design-box.js` 真源)。
// 在那之前本文件是**唯一**一处写着 960/720 的地方 —— 与「设计盒数字不得二次硬编码」
// 同一条纪律,只是真源暂时落在这里。
//
// 尺寸依据(J75 C「依内容定」):
//   • 高 720 = header 40 + 分布图卡 ~196 + 轨迹图卡 ~400(**占主体**,J75 C 逐字)
//     + 图例 24 + 三道 gap + 上下内边距 30;轨迹图拿到全高的一半以上。
//   • 宽 960 = y 刻度列 62 + 时间线舞台 ~830 + 左右内边距 36。比 Output 的 1180 窄:
//     Monitor 是**副窗**,用户多半把它摆在 DAW 边上长期开着,不该抢主窗的横向空间;
//     而它只有两张图、没有 Tab2 那种 14 列控件,窄一点仍读得清。
//   • 缩放档位与 Output 同表(七档)—— 同族窗口的缩放手感必须一致(05 §1.2);
//     取同一组档位还有个副作用是好的:native 侧 `parseUiScaleArg` 的既有回落分支
//     不用改也已经对了。
// 缩放档位是设计值不是可用值:运行时仍要按屏幕可用区过滤(见 app.js scaleOverflows)。
// =============================================================================

export const MONITOR_DESIGN = {
    w: 960,
    h: 720,
    presets: [0.5, 0.65, 0.8, 1, 1.25, 1.5, 2],
};
