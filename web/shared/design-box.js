// SPDX-License-Identifier: GPL-3.0-or-later
// design-box.js —— 全项目「设计盒」常量的唯一真源(05 §1.2;07 T27 设计定稿回流①)。
//   Output 1180×780 的定案依据:1080×720 的内容区仅 1044×536,装不下 Tab2 全部 14 列控件;
//   加宽到 1180 才够横向呼吸,780 高把内容区抬到约 596px(15×44=660 仍需垂直滚动 + 表头 sticky)。
//   Input 460×560 与 Bridge 完全一致(家族感,05 §1.2 表)。
//   Monitor 960×720([J75] 节 C「设计盒尺寸依内容定」,T46 依内容定稿、T45 落 native):
//   高 720 里轨迹图卡拿走 400+(J75 C「轨迹图占主体」);宽 960 比 Output 的 1180 窄,
//   因为 Monitor 是副窗、不该抢主窗的横向空间。缩放档位与 Output 同七档(同族窗口手感一致)。
//   纪律一:C++ 侧 src/core/DesignBox.h 由 T26 的 scripts/gen-design-box.py 从本文件生成,
//   任何位置(C++ / HTML / CSS / web-preview)不得二次硬编码这些数字;改值只改这里再重跑生成器。
//   纪律二:本文件必须保持「单个对象字面量 + 纯数字」的形状,不引入运算、依赖或条件分支,
//   否则 gen-design-box.py 无法静态解析。
//   缩放档位是设计值,不是可用值:运行时还要按屏幕可用区过滤
//   (W*F > screen.availWidth ∨ H*F > screen.availHeight 的档位灰掉并提示「超出当前屏幕」,05 §1.2)。

export const DESIGN = {
    output: { w: 1180, h: 780, presets: [0.5, 0.65, 0.8, 1, 1.25, 1.5, 2] },
    input: {
        w: 460,
        h: 560,
        presets: [0.33, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2, 2.5, 3],
    },
    monitor: { w: 960, h: 720, presets: [0.5, 0.65, 0.8, 1, 1.25, 1.5, 2] },
};
