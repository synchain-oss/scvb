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

// 单条时间线的合理上界(秒)。24h 远超任何真实工程,只用来挡住**明显非法**的采样数 ——
// 真机上出现过 t1 = 2^40 采样(÷48k ≈ 265 天)这种「无末端哨兵」被当成真值换算成秒,
// 一路喂到前端的 durationOf,再被当作 requestWaveform 的 endS 发回来,把消息线程跑死
// (P0-A)。哨兵语义应当在**产生它的地方**处理掉,而不是让它以「一个很大的秒数」的伪装
// 穿过整条链路 —— #89 在 viz 侧就是这么做的(无末端只取 t0),本处补齐同一口径。
inline constexpr double kMaxTimelineSeconds = 24.0 * 60.0 * 60.0;

// 样本→秒安全换算:sampleRate<=0 返回 0.0 哨兵,绝不把 NaN/inf 进 JSON(PR#55 第6轮缺陷1)。
// 采样数超出合理上界(或为负)同样回 0.0 哨兵 —— 宁可让 UI 看到「没有这个时间点」,
// 也不要给它一个 265 天的假时长。
inline double samplesToSeconds(std::int64_t samples, double sampleRate)
{
    if (!(sampleRate > 0.0) || samples < 0)
    {
        return 0.0;
    }
    const double seconds = static_cast<double>(samples) / sampleRate;
    return seconds <= kMaxTimelineSeconds ? seconds : 0.0;
}

} // namespace scvb::output
