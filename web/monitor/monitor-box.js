// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Monitor · 设计盒(T46;[J75] C「Monitor 设计盒尺寸 T46 依内容定,机制同款」)
// -----------------------------------------------------------------------------
// **本文件现在只是一层转发** —— 数字的唯一真源是 `web/shared/design-box.js` 的
// `DESIGN.monitor`,那份文件同时是 `scripts/gen-design-box.py` 的输入,生成
// `src/core/DesignBox.h` 并由 gate 3d 逐字节对拍。960×720 的定稿依据(轨迹图占主体、
// 副窗不抢主窗横向空间、缩放档位与 Output 同七档)写在那份文件的头注里,不在这里重复。
//
// 【为什么曾经在这里存过一份字面量】T46 的纪律是**不碰 `src/`**(native 侧 Monitor 壳归
// T45),而往 `design-box.js` 里加键必须同批重跑生成器、改 `check-design-box.mjs` 的
// `EXPECT`、动 `BridgeBase.h` —— 那四处都在 T45 的范围里。故当时页面侧暂存一份,
// 等 T45(PR #94)落地。**它已经落地并合入 `feature/v1`(`649c99f`),故此处转发。**
//
// 交接不是靠人记着的:`smoke-monitor.mjs` ⑦ 节那条「在场才查」的断言在 `DESIGN.monitor`
// 缺席时打 [SKIP] 并写明还欠这一步,一出现就断言两处同值、并在断言语里直说「现在该改成
// 转发了」。现在它已转成对本文件的转发身份断言(值不可能漂 —— 只有一份)。
//
// 【为什么不直接删掉本文件】页面与 `web-preview/shell.js` 的既有 import 点不必改;
// 更重要的是「Monitor 取的是 `DESIGN` 里的哪个键」这件事需要一个说得清的落点 ——
// 删掉之后,那句「Monitor 用 monitor 键」就只剩散在各消费点的字面量了。
//
// 缩放档位是设计值不是可用值:运行时仍要按屏幕可用区过滤(见 app.js 的 scaleOverflows)。
// =============================================================================

import { DESIGN } from "../shared/design-box.js";

export const MONITOR_DESIGN = DESIGN.monitor;
