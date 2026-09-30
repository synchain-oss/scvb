// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// T15:Output 插件 123 个自动化参数的布局真源(params-v0 v2.1 §一 / J59 / J65)。
// makeOutputLayout() 按冻结的深度优先顺序静态声明全部参数;ParamHandles 供音频线程零查找访问。
// 铁律:ParameterGroup 的 addChild 必须全部先于 layout.add()(03 §0 三条铁律之二)。

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

namespace scvb::params
{

// [J59] 2 版本;[J59] 15 轨;每轨 4 参数(pan/vol/width/freeze,[J65])。
constexpr int kNumVersions = 2;
constexpr int kNumTracks = 15;
constexpr int kNumParamsPerTrack = 4;

// 123 = 全局三件(width/ms_balance/lead_select)+ 2 版本 × 15 轨 × 4 参数。[J59/J65]
constexpr int kNumAutomatable = 3 + kNumVersions * kNumTracks * kNumParamsPerTrack;
static_assert(kNumAutomatable == 123, "params v2.1 frozen count");

// 124 = 宿主可见数(wrapper 合成 bypass,J02 双口径)。Ableton Live 128 上限余 4 —— 预算封顶(J65)。
constexpr int kNumHostVisible = kNumAutomatable + 1;

// 全局三件在根组,index 0..2(冻结)。
constexpr int kGlobalWidthIndex = 0;
constexpr int kGlobalMsBalanceIndex = 1;
constexpr int kGlobalLeadSelectIndex = 2;

// 索引公式(冻结,写入单测):index(v,t,k) = 3 + (v-1)*60 + (t-1)*4 + k
// k:pan=0 / vol=1 / width=2 / freeze=3。末位 = index(2,15,freeze) = 122。
constexpr int kTrackParamIndex(int v, int t, int k) noexcept
{
    return 3 + (v - 1) * 60 + (t - 1) * 4 + k;
}

// ParamID 字符串(冻结):v{v}_t{t:02d}_{pan|vol|width|freeze}。
inline juce::String panId(int v, int t)
{
    return juce::String::formatted("v%d_t%02d_pan", v, t);
}
inline juce::String volId(int v, int t)
{
    return juce::String::formatted("v%d_t%02d_vol", v, t);
}
inline juce::String widthId(int v, int t)
{
    return juce::String::formatted("v%d_t%02d_width", v, t);
}
inline juce::String freezeId(int v, int t)
{
    return juce::String::formatted("v%d_t%02d_freeze", v, t);
}

// ---- 各组 stringFromValue / valueFromString(03 §1.2–§1.8)----

juce::String widthToString(float value, int /*maxLen*/);
float widthFromString(const juce::String& text);

juce::String msBalanceToString(float value, int /*maxLen*/);
float msBalanceFromString(const juce::String& text);

juce::String leadSelectToString(int value, int /*maxLen*/);
int leadSelectFromString(const juce::String& text);

juce::String panToString(float value, int /*maxLen*/);
float panFromString(const juce::String& text);

juce::String volToString(float value, int /*maxLen*/);
float volFromString(const juce::String& text);

juce::String trackWidthToString(float value, int /*maxLen*/);
float trackWidthFromString(const juce::String& text);

juce::String freezeToString(int value, int /*maxLen*/);
int freezeFromString(const juce::String& text);

// ---- 布局与句柄 ----

// 一次性静态声明全部 123 个参数(冻结深度优先顺序,versionHint=1)。
juce::AudioProcessorValueTreeState::ParameterLayout makeOutputLayout();

// 音频线程零查找访问的裸指针二维表(构造后由 collectParamHandles 缓存)。
struct ParamHandles
{
    juce::AudioParameterFloat* width = nullptr;
    juce::AudioParameterFloat* msBalance = nullptr;
    juce::AudioParameterInt* leadSelect = nullptr;

    juce::AudioParameterFloat* pan[kNumVersions][kNumTracks]{};
    juce::AudioParameterFloat* vol[kNumVersions][kNumTracks]{};
    juce::AudioParameterFloat* trkW[kNumVersions][kNumTracks]{}; // 每轨 width,[J57]
    juce::AudioParameterInt* frz[kNumVersions][kNumTracks]{}; // 每轨 freeze,[J65]

    std::atomic<float>* rawPan[kNumVersions][kNumTracks]{};
    std::atomic<float>* rawVol[kNumVersions][kNumTracks]{};
    std::atomic<float>* rawTrkW[kNumVersions][kNumTracks]{};
    std::atomic<float>* rawFrz[kNumVersions][kNumTracks]{};
    std::atomic<float>* rawWidth = nullptr;
    std::atomic<float>* rawMsBalance = nullptr;
    std::atomic<float>* rawLeadSelect = nullptr;
};

// 从已构造的 APVTS 缓存全部句柄(与 makeOutputLayout 的 ParamID 一一对应)。
ParamHandles collectParamHandles(juce::AudioProcessorValueTreeState& apvts);

} // namespace scvb::params
