// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// BridgeArgs —— 桥面 native function 参数提取/白名单助手(消息线程)。纯 JUCE 工具,可离线单测。
// 与 SegmentEditService.h 同放 src/output/(不落 scvb_core,因依赖 JUCE)。

#include <juce_audio_processors/juce_audio_processors.h>

namespace scvb::output
{

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

// 读取参数的**工程值**(契约 §2.2 f32 工程值)。APVTS 的 getRawParameterValue 返回归一化 0..1 原子,
// 须经 convertFrom0to1 还原(PR#55 第3轮重要1;AudioParameterFloat::get() 同款)。
inline float readParamEngineering(juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
{
    if (auto* p = apvts.getParameter(id))
        return p->convertFrom0to1(p->getValue());
    return 0.0f;
}

// gesture 通道白名单(契约 §1.12):全局三件 + 当前激活版本每轨 width/freeze。
// pan/vol 走 setTrackManual(曲线真身),非激活版本参数不进本通道;白名单外回 badArg(不得静默忽略)。
inline bool isGestureParam(const juce::String& id, int activeVersion)
{
    if (id == "width" || id == "ms_balance" || id == "lead_select")
        return true;
    for (int t = 1; t <= 15; ++t)
    {
        if (id == juce::String::formatted("v%d_t%02d_width", activeVersion, t) ||
            id == juce::String::formatted("v%d_t%02d_freeze", activeVersion, t))
            return true;
    }
    return false;
}

// segmentation.mode 白名单(02-dsp-spec §362,params-v0):vad_only(不做 S1)/ valley(默认)。
inline bool isSegmentationMode(const juce::String& mode)
{
    return mode == "vad_only" || mode == "valley";
}

// 样本→秒安全换算:sampleRate<=0 返回 0.0 哨兵,绝不把 NaN/inf 进 JSON(PR#55 第6轮缺陷1)。
inline double samplesToSeconds(std::int64_t samples, double sampleRate)
{
    return sampleRate > 0.0 ? static_cast<double>(samples) / sampleRate : 0.0;
}

} // namespace scvb::output
