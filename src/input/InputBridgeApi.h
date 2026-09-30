// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// InputBridgeApi —— T30 Input 桥名常量表(parity C 侧真源,scripts/check-bridge-parity.mjs 抽取面)。
// 与 docs/SCVB_CONTRACT.md §7 manifest.input 逐项相等:7 函数 / 5 事件。
// 抽取规则:每行 constexpr 上的字符串字面量,"scvb." 前缀 = 事件,否则 = 函数;
// 非桥名的辅助字符串常量不得出现在本文件(或须带 parity-ignore 注释)。

namespace scvb::input::bridge
{
// ---- functions(契约 §3)----
inline constexpr const char* kFnRequestInitialState = "requestInitialState";
inline constexpr const char* kFnSetChannelId = "setChannelId";
inline constexpr const char* kFnSetGroupId = "setGroupId";
inline constexpr const char* kFnRemoteSetPriority = "remoteSetPriority";
inline constexpr const char* kFnSetUiScale = "setUiScale";
inline constexpr const char* kFnCommitUiScale = "commitUiScale";
inline constexpr const char* kFnSetLang = "setLang";
inline constexpr const char* kFnSetGuideSeen = "setGuideSeen"; // [J81] J80/T48 Input 首启引导已读位

// ---- events(契约 §4)----
inline constexpr const char* kEvState = "scvb.state";
inline constexpr const char* kEvConn = "scvb.conn";
inline constexpr const char* kEvConfig = "scvb.config";
inline constexpr const char* kEvGroups = "scvb.groups";
inline constexpr const char* kEvError = "scvb.error";
} // namespace scvb::input::bridge
