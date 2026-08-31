// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "DesignBox.h"

namespace scvb::bridge
{
// BridgeBase —— 两插件共用的桥契约常量 + 缩放/回执/快照助手(01 §6.1 机制 6 的「唯一真源常量」半边)。
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
inline constexpr const char* Role = "role"; // "input" | "output" | "monitor"([J75])
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

// [SL-234] 百分比档位 clamp,边界 = 上面同一对常量 ×100(**禁止再写第三份硬编码边界**)。
// 调用方三处:桥面 setUiScale(bridge*SetUiScalePercent)、**加载期** CFGS.uiScale
// (STATE_SCHEMA §三 明写 `ui.scale` 在 CFGS 解码器里「不作范围校验(原样透出,**由上层处理**)」——
//  上层就是加载路径;工程文件是不可信字节,铁律出处 = CLAUDE.md §7 铁律 3)、
// UiDefaultsStore 的全局默认区间判定。
// 入参取 std::int64_t 而不是 int:CFGS 里 uiScale 是 u32,先 static_cast<int> 再夹会把
// 4294967295 折成 -1 —— 结果虽然也落在下界,但那是**溢出撞上的**,不是范围校验。
// 定义放在头里(与下面的 designBoxWindowSize 同款):三个调用方分属三个不同的链接单元,
// UiDefaultsStore.cpp 就被 scvb_params_tests 单独链去而不带 BridgeBase.cpp —— 纯计算函数
// 走 inline,免得为一条 jlimit 给每个目标补依赖(PR #154 CI 的 LNK2019)。
inline int clampUiScalePercent(std::int64_t percent)
{
    // 边界只在这一处由 Min/MaxUiScale 换算成百分比,调用方一律复用。
    const auto lo = static_cast<std::int64_t>(juce::roundToInt(plugin::MinUiScale * 100.0f));
    const auto hi = static_cast<std::int64_t>(juce::roundToInt(plugin::MaxUiScale * 100.0f));
    return static_cast<int>(juce::jlimit(lo, hi, percent));
}

// 设计盒窗口尺寸(机制 9):setSize(round(W×F), round(H×F)),固定设计盒 + CSS zoom。
struct DesignBoxSize
{
    int width = 0;
    int height = 0;
};

// 设计盒常量唯一真源 = DesignBox.h(由 design-box.js 生成);role 取 "input" / "output" /
// "monitor"([J75]),其余值回落 Output。纯函数,便于离线单测。
inline DesignBoxSize designBoxWindowSize(const juce::String& role, float scale)
{
    int w = scvb::design::kOutputDesignW;
    int h = scvb::design::kOutputDesignH;
    if (role == "input")
    {
        w = scvb::design::kInputDesignW;
        h = scvb::design::kInputDesignH;
    }
    else if (role == "monitor")
    {
        w = scvb::design::kMonitorDesignW;
        h = scvb::design::kMonitorDesignH;
    }
    return {juce::roundToInt(static_cast<float>(w) * scale), juce::roundToInt(static_cast<float>(h) * scale)};
}

// --- 首帧 seed(机制 5)与 scvb.state 快照共用的 UI 基础信息 ----------------------------
struct UiSeed
{
    juce::String role; // "input" | "output" | "monitor"([J75])
    juce::String version;
    juce::String lang; // 归一化后的 {zh,en,fr}
    float uiScale = 1.0f;
    int channelLimit = 15; // [J59]
};

// 返回 (key, value) 列表,key 取 Init::*,顺序与 WebViewHost::makeOptions 的 withInitialisationData 一致。
// 与 buildUiSnapshot 同源,保证「首帧 seed」与「scvb.state 快照」键值永不漂移。
std::vector<std::pair<juce::String, juce::var>> buildUiSeedPairs(const UiSeed& seed);

// 由 seed 构造 scvb.state 快照基础(机制 8 的 requestInitialState 回执;setLang/setUiScale 写入后
// 经本快照回推实际生效值,§1.30/§1.28)。
juce::var buildUiSnapshot(const UiSeed& seed);

// setLang 归一化(§1.30):仅 {zh,en,fr},未知 code 回退 "zh"。
juce::String normalizeLang(const juce::String& code);

// setUiScale 参数校验(§1.28):数量≥1、数值(isDouble/isInt)、且在 role 的档位表内
// (Input 用 kInputPresets / Output 用 kOutputPresets,唯一真源 = DesignBox.h)。档位是 double,
// 用 double 比较避免 0.33 类精度误判。命中则 outScale=档位值并返回 true;
// 否则(缺参/非数值/不在档位表)返回 false(调用方回 {ok:false, reason:"badArg"})。
bool parseUiScaleArg(const juce::Array<juce::var>& args, const juce::String& role, float& outScale);

// [SL-258] 从 src/output/BridgeArgs.h 上提到共用层:Input 的 §3.8 setGuideSeen 与 Output 的
// §1.32 是**逐字同签名**的两侧同名函数,布尔口径必须同一份 —— 抄第二份正是两侧漂移的起点
// (§0.1 第 4 条:同一语义不得两个落点)。Output 侧调用点不变,BridgeArgs.h 留 using 转发。
// 严格布尔提取(PR#55 缺陷3):仅接受 bool / int / int64 数值;isBool → 真值,isInt/isInt64 → !=0。
// 其它(字符串/对象/void)返回 false,调用方回 badArg。JUCE 8.0.8 的 var::operator bool() 实为
// type->toBool(value)(juce_Variant.cpp:567;bool→boolToBool L246、int→intToBool L149、void→defaultToBool
// L100),对 bool 返回真值、对 void 返回 false;此处仍显式分型,避免字符串 "false" 被 stringToBool
// 误判为非空即 true。
inline bool strictBool(const juce::var& v, bool& out)
{
    if (v.isBool())
    {
        out = static_cast<bool>(v);
        return true;
    }
    if (v.isInt() || v.isInt64())
    {
        out = static_cast<juce::int64>(v) != 0;
        return true;
    }
    return false;
}

// --- 回执助手(两插件共用的原生函数回执构造)-------------------------------------------
// {ok:true}
juce::var okResponse();
// {ok:true, key:value}
juce::var okResponseWith(const juce::String& key, const juce::var& value);
// {ok:false, reason:"badArg"}(§1.28 拒绝态)
juce::var badArgResponse();

} // namespace scvb::bridge
