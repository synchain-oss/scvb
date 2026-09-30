// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MonitorBridgeApi —— Monitor 桥名常量表(T45 起的壳层面,T46 补齐)。
//
// 本表已随 [J81] 转正进 `docs/SCVB_CONTRACT.md §7` 的 `manifest.monitor`(正文 §10),
// `scripts/check-bridge-parity.mjs` 的 [M] 块把它与契约 manifest、`web/shared/bridge.js` 的
// `BRIDGE_FUNCTIONS.monitor` / `BRIDGE_EVENTS.monitor` 三向比对;本文件的路径就登记在该脚本的
// `HEADER_PATHS.monitor`。改名字先改契约(§9 变更流程),再同步这三处。
//
// 通用四函数(requestInitialState / setUiScale / commitUiScale / setLang)由 WebViewHost 基类注册,
// 名字真源在 scvb::bridge::Fn,本表只列 Monitor 专属项。

namespace scvb::monitor::bridge
{
// ---- functions ----
// **刻意不叫 `setGroupId`**:契约 §1.4 的 `setGroupId` 是 Output 的改组 —— 断开本组全部连接、
// 要弹确认条。Monitor 只是换一个组的 viz 段来看,不 claim、对被观察的组零副作用。
// 两件事共用一个名字,迟早有人照 §1.4 的语义去实现它。(T46 提出,采纳。)
inline constexpr const char* kFnSetObservedGroup = "setObservedGroup"; // 组选择 A-H(只读换段)

// ---- events ----
inline constexpr const char* kEvState = "scvb.state"; // 组/缩放/语言/viz 在线态
inline constexpr const char* kEvGroups = "scvb.groups"; // 1Hz 跨组在线位图(J70 只读探测)
inline constexpr const char* kEvViz = "scvb.viz"; // 4Hz viz 帧(降采样车道 + 每轨当前值)
inline constexpr const char* kEvPlayhead =
    "scvb.playhead"; // 25Hz 播放头(WebViewHost 定时器上限;载荷形状复用 Output 侧)
} // namespace scvb::monitor::bridge
