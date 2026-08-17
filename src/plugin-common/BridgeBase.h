// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_core/juce_core.h>

#include "DesignBox.h"

namespace scvb::bridge
{
// BridgeBase —— 两插件共用的桥契约常量 + 缩放/回执助手(01 §6.1 机制 6 的「唯一真源常量」半边)。
// 完整函数/事件集 = T25 冻结契约 docs/SCVB_CONTRACT.md + T29/T30 的 InputBridgeApi.h / OutputBridgeApi.h;
// 这里只承载两插件**共用**的缩放 / 语言 / 首帧命名与 clamp 边界,插件专属命名归 T29/T30。

// --- C++ -> JS 事件名(WebViewHost 用 emitEventIfBrowserIsVisible 推送)----------------
namespace Event
{
inline constexpr const char* State = "scvb.state";
} // namespace Event

// --- JS -> C++ 原生函数名(withNativeFunction 注册;两插件共用)-------------------------
namespace Fn
{
inline constexpr const char* RequestInitialState = "requestInitialState";
inline constexpr const char* SetLang = "setLang";
inline constexpr const char* SetUiScale = "setUiScale";
inline constexpr const char* CommitUiScale = "commitUiScale";
} // namespace Fn

// --- withInitialisationData 预置键(首帧同步可读,免一次异步往返)----------------------
namespace Init
{
inline constexpr const char* Version = "version";
inline constexpr const char* Lang = "lang";
inline constexpr const char* UiScale = "uiScale";
inline constexpr const char* Role = "role"; // "input" | "output"
inline constexpr const char* ChannelLimit = "channelLimit"; // [J59] 15
} // namespace Init

// --- 缩放 clamp 边界(setUiScale 的硬边界;可选档位真源 = DesignBox.h 的 presets)-------
namespace plugin
{
inline constexpr float DefaultUiScale = 1.0f;
inline constexpr float MinUiScale = 0.33f;
inline constexpr float MaxUiScale = 3.0f;
} // namespace plugin

// clamp 到 [MinUiScale, MaxUiScale](与 Bridge 逐条一致)。
float clampUiScale(float scale);

// 设计盒窗口尺寸(机制 9):setSize(round(W×F), round(H×F)),固定设计盒 + CSS zoom。
struct DesignBoxSize
{
    int width = 0;
    int height = 0;
};

// 设计盒常量唯一真源 = DesignBox.h(由 design-box.js 生成);role 取 "input" 或 "output",
// 其余值回落 Output。纯函数,便于离线单测。
inline DesignBoxSize designBoxWindowSize(const juce::String& role, float scale)
{
    const int w = role == "input" ? scvb::design::kInputDesignW : scvb::design::kOutputDesignW;
    const int h = role == "input" ? scvb::design::kInputDesignH : scvb::design::kOutputDesignH;
    return {juce::roundToInt(static_cast<float>(w) * scale), juce::roundToInt(static_cast<float>(h) * scale)};
}

// --- 回执助手(两插件共用的原生函数回执构造)-------------------------------------------
// {ok:true}
juce::var okResponse();
// {ok:true, key:value}
juce::var okResponseWith(const juce::String& key, const juce::var& value);

} // namespace scvb::bridge
