// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// UiDefaultsStore —— 系统级 UI 全局默认(**跨工程**,不随工程 state 走)。
//
// 承载三项(契约 §1.29/§1.32/§1.33):
//   • guide_seen_global —— 首启红字九条页勾了「不再显示」;
//   • tour_seen_global  —— 交互式导览已完成或已婉拒;
//   • uiScalePercent    —— 缩放防呆确认「保持」后落的默认档位(0 = 未设置)。
//
// 为什么存在:此前 §1.1 快照里的 guide_seen_global / tour_seen_global 是**硬编码 false**、
// WebViewHost::persistUiScaleAsDefault 是**空实现** —— 「不再显示」的跨工程承诺从未兑现
// (T37 真机 bug A-3)。工程内的 guide_seen / tour_seen 归 CFGS chunk,与本存储互补。
//
// 实现纪律:不驻留任何进程内状态 —— 每次读写现开一份 juce::PropertiesFile,读完/写完即析构。
// 同一宿主里两个 Output 实例(或 Output 与将来的其它角色)因此永远看到磁盘上的同一份真值,
// 无需跨实例广播;调用点稀疏(编辑器开窗一次 / 用户点「不再显示」「保持」各一次),开销可忽略。
// 只在消息线程调用(桥 native function 与 buildSnapshot 均在消息线程)。

namespace scvb::output::uidefaults
{

bool guideSeenGlobal();
void setGuideSeenGlobal(bool seen);

bool tourSeenGlobal();
void setTourSeenGlobal(bool seen);

// 0 = 未设置过(调用方沿用自己的默认 100)。
int uiScalePercent();
void setUiScalePercent(int percent);

} // namespace scvb::output::uidefaults
