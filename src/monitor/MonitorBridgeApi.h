// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MonitorBridgeApi —— Monitor 桥名常量表(T45 壳层最小面)。
//
// 与 Input/Output 的 *BridgeApi.h 不同,本文件**尚未**进 `docs/SCVB_CONTRACT.md §7 manifest`,
// 也不在 `scripts/check-bridge-parity.mjs` 的抽取路径里(该脚本只扫 src/input 与 src/output 两个
// 显式路径)。原因:05 J75 节 C 把 Monitor 的**真 UI 与完整桥面归 T46**;T45 只交插件壳,
// 桥面按「够跑通一次端到端」定最小集。**T46 立项时把本表正式收进契约 §7 manifest.monitor
// 并接入 parity 抽取**(见 docs/contract-changes/20260825-monitor-target.md)。
//
// 通用四函数(requestInitialState / setUiScale / commitUiScale / setLang)由 WebViewHost 基类注册,
// 名字真源在 scvb::bridge::Fn,本表只列 Monitor 专属项。

namespace scvb::monitor::bridge
{
// ---- functions ----
inline constexpr const char* kFnSetGroupId = "setGroupId"; // 组选择 A-H(只读换段,不 claim)

// ---- events ----
inline constexpr const char* kEvState = "scvb.state"; // 组/缩放/语言/viz 在线态
inline constexpr const char* kEvGroups = "scvb.groups"; // 1Hz 跨组在线位图(J70 只读探测)
inline constexpr const char* kEvViz = "scvb.viz"; // 4Hz viz 帧头 + 每轨摘要(车道全量归 T46)
} // namespace scvb::monitor::bridge
