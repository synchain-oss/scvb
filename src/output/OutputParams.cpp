// SPDX-License-Identifier: GPL-3.0-or-later
#include "OutputParams.h"

#include <cmath>

namespace scvb::params
{

// =================================================================================
// stringFromValue / valueFromString(03 §1.2–§1.8,params-v0 v2.1)
// =================================================================================

juce::String widthToString(float value, int)
{
    // 整数显示(四舍五入到 1%)。
    return juce::String(juce::roundToInt(value)) + " %";
}

float widthFromString(const juce::String& text)
{
    const auto t = text.removeCharacters("% 	").trim();
    return juce::jlimit(0.0f, 150.0f, t.getFloatValue());
}

juce::String msBalanceToString(float value, int)
{
    // 0 → "0";<0 → "M" + 整数;>0 → "S" + 整数。
    const int i = juce::roundToInt(value);
    if (i == 0)
        return "0";
    if (i < 0)
        return "M" + juce::String(-i);
    return "S" + juce::String(i);
}

float msBalanceFromString(const juce::String& text)
{
    const auto t = text.trim();
    if (t.isEmpty())
        return 0.0f;

    const juce::juce_wchar c = t[0];
    if (c == 'M' || c == 'm')
        return juce::jlimit(-100.0f, 100.0f, -t.substring(1).getFloatValue());
    if (c == 'S' || c == 's')
        return juce::jlimit(-100.0f, 100.0f, t.substring(1).getFloatValue());
    return juce::jlimit(-100.0f, 100.0f, t.getFloatValue());
}

juce::String leadSelectToString(int value, int)
{
    // 0 → "Follow";n → "Track {n:02d}"。
    if (value == 0)
        return "Follow";
    return juce::String::formatted("Track %02d", value);
}

int leadSelectFromString(const juce::String& text)
{
    const auto t = text.trim();
    if (t.equalsIgnoreCase("Follow"))
        return 0;

    if (t.startsWithIgnoreCase("Track"))
        return juce::jlimit(0, 15, juce::roundToInt(t.substring(5).trimStart().getFloatValue()));

    return juce::jlimit(0, 15, juce::roundToInt(t.getFloatValue()));
}

juce::String panToString(float value, int)
{
    // 0 → "C";<0 → "L" + 一位小数去尾零;>0 → "R" + 一位小数去尾零。
    if (juce::approximatelyEqual(value, 0.0f))
        return "C";

    const juce::juce_wchar prefix = (value < 0.0f) ? 'L' : 'R';
    juce::String s = juce::String(std::fabs(value), 1);
    if (s.endsWith(".0"))
        s = s.dropLastCharacters(2);

    return juce::String::charToString(prefix) + s;
}

float panFromString(const juce::String& text)
{
    const auto t = text.trim();
    if (t.isEmpty() || t.equalsIgnoreCase("C"))
        return 0.0f;

    const juce::juce_wchar c = t[0];
    if (c == 'L' || c == 'l')
        return juce::jlimit(-100.0f, 100.0f, -t.substring(1).getFloatValue());
    if (c == 'R' || c == 'r')
        return juce::jlimit(-100.0f, 100.0f, t.substring(1).getFloatValue());
    return juce::jlimit(-100.0f, 100.0f, t.getFloatValue());
}

juce::String volToString(float value, int)
{
    // 一位小数 + 正号显式:"+3.2 dB" / "0.0 dB" / "-12.5 dB";下限不是静音,无 -inf 特例。
    if (juce::approximatelyEqual(value, 0.0f))
        return "0.0 dB";

    const juce::String s = juce::String(std::fabs(value), 1);
    return (value < 0.0f ? "-" : "+") + s + " dB";
}

float volFromString(const juce::String& text)
{
    auto t = text.trim().toLowerCase();
    t = t.replace("db", "");
    t = t.removeCharacters(" 	");
    return juce::jlimit(-24.0f, 12.0f, t.getFloatValue());
}

juce::String trackWidthToString(float value, int)
{
    return juce::String(juce::roundToInt(value)) + " %";
}

float trackWidthFromString(const juce::String& text)
{
    const auto t = text.removeCharacters("% 	").trim();
    return juce::jlimit(0.0f, 100.0f, t.getFloatValue());
}

juce::String freezeToString(int value, int)
{
    // [J65] 0=全自动 / 1=冻结pan / 2=冻结vol / 3=全冻结。
    switch (value)
    {
    case 0:
        return "Auto";
    case 1:
        return "Pan Frozen";
    case 2:
        return "Vol Frozen";
    case 3:
        return "All Frozen";
    default:
        return juce::String(value);
    }
}

int freezeFromString(const juce::String& text)
{
    const auto t = text.trim();
    if (t.equalsIgnoreCase("Auto"))
        return 0;
    if (t.equalsIgnoreCase("Pan Frozen"))
        return 1;
    if (t.equalsIgnoreCase("Vol Frozen"))
        return 2;
    if (t.equalsIgnoreCase("All Frozen"))
        return 3;
    return juce::jlimit(0, 3, juce::roundToInt(t.getFloatValue()));
}

// =================================================================================
// makeOutputLayout:一次性静态声明 123 参数,深度优先顺序冻结。
// 铁律:ParameterGroup 的 addChild 全部先于 layout.add()。
// =================================================================================

juce::AudioProcessorValueTreeState::ParameterLayout makeOutputLayout()
{
    using namespace juce;

    AudioProcessorValueTreeState::ParameterLayout layout;

    // index 0..2:全局三件(根组)。
    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"width", 1}, "Width", NormalisableRange<float>(0.0f, 150.0f, 0.0f, 1.0f), 100.0f,
        AudioParameterFloatAttributes()
            .withLabel("%")
            .withStringFromValueFunction([](float v, int m) { return widthToString(v, m); })
            .withValueFromStringFunction([](const String& s) { return widthFromString(s); })));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"ms_balance", 1}, "MS Balance", NormalisableRange<float>(-100.0f, 100.0f, 0.0f, 1.0f), 0.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int m) { return msBalanceToString(v, m); })
            .withValueFromStringFunction([](const String& s) { return msBalanceFromString(s); })));

    layout.add(std::make_unique<AudioParameterInt>(
        ParameterID{"lead_select", 1}, "Lead Select", 0, 15, 0,
        AudioParameterIntAttributes()
            .withStringFromValueFunction([](int v, int m) { return leadSelectToString(v, m); })
            .withValueFromStringFunction([](const String& s) { return leadSelectFromString(s); })));

    // index 3..122:版本 v(1..2)→ 轨道 t(1..15)→ (Pan, Vol, Width, Freeze)。
    for (int v = 1; v <= kNumVersions; ++v)
    {
        auto vg = std::make_unique<AudioProcessorParameterGroup>("v" + String(v), "Version " + String(v), "|");

        for (int t = 1; t <= kNumTracks; ++t)
        {
            auto tg = std::make_unique<AudioProcessorParameterGroup>(String::formatted("v%d_t%02d", v, t),
                                                                     String::formatted("Track %02d", t), "|");

            tg->addChild(std::make_unique<AudioParameterFloat>(
                ParameterID{panId(v, t), 1}, String::formatted("V%d T%02d Pan", v, t),
                NormalisableRange<float>(-100.0f, 100.0f, 0.0f, 1.0f), 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction([](float val, int m) { return panToString(val, m); })
                    .withValueFromStringFunction([](const String& s) { return panFromString(s); })));

            tg->addChild(std::make_unique<AudioParameterFloat>(
                ParameterID{volId(v, t), 1}, String::formatted("V%d T%02d Vol", v, t),
                NormalisableRange<float>(-24.0f, 12.0f, 0.0f, 1.0f), 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel("dB")
                    .withStringFromValueFunction([](float val, int m) { return volToString(val, m); })
                    .withValueFromStringFunction([](const String& s) { return volFromString(s); })));

            tg->addChild(std::make_unique<AudioParameterFloat>(
                ParameterID{widthId(v, t), 1}, String::formatted("V%d T%02d Width", v, t),
                NormalisableRange<float>(0.0f, 100.0f, 0.0f, 1.0f), 100.0f,
                AudioParameterFloatAttributes()
                    .withLabel("%")
                    .withStringFromValueFunction([](float val, int m) { return trackWidthToString(val, m); })
                    .withValueFromStringFunction([](const String& s) { return trackWidthFromString(s); })));

            tg->addChild(std::make_unique<AudioParameterInt>(
                ParameterID{freezeId(v, t), 1}, String::formatted("V%d T%02d Freeze", v, t), 0, 3, 0,
                AudioParameterIntAttributes()
                    .withStringFromValueFunction([](int val, int m) { return freezeToString(val, m); })
                    .withValueFromStringFunction([](const String& s) { return freezeFromString(s); })));

            // 铁律:先 addChild,再 layout.add。
            vg->addChild(std::move(tg));
        }

        layout.add(std::move(vg));
    }

    return layout;
}

// =================================================================================
// ParamHandles 缓存
// =================================================================================

ParamHandles collectParamHandles(juce::AudioProcessorValueTreeState& apvts)
{
    ParamHandles h;

    h.width = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("width"));
    h.msBalance = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("ms_balance"));
    h.leadSelect = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("lead_select"));

    h.rawWidth = apvts.getRawParameterValue("width");
    h.rawMsBalance = apvts.getRawParameterValue("ms_balance");
    h.rawLeadSelect = apvts.getRawParameterValue("lead_select");

    for (int v = 0; v < kNumVersions; ++v)
    {
        for (int t = 0; t < kNumTracks; ++t)
        {
            const int ver = v + 1;
            const int trk = t + 1;

            h.pan[v][t] = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(panId(ver, trk)));
            h.vol[v][t] = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(volId(ver, trk)));
            h.trkW[v][t] = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(widthId(ver, trk)));
            h.frz[v][t] = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter(freezeId(ver, trk)));

            h.rawPan[v][t] = apvts.getRawParameterValue(panId(ver, trk));
            h.rawVol[v][t] = apvts.getRawParameterValue(volId(ver, trk));
            h.rawTrkW[v][t] = apvts.getRawParameterValue(widthId(ver, trk));
            h.rawFrz[v][t] = apvts.getRawParameterValue(freezeId(ver, trk));
        }
    }

    return h;
}

} // namespace scvb::params
