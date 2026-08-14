// SPDX-License-Identifier: GPL-3.0-or-later
#include "S2OutputProcessor.h"

#include <cmath>

#include "S2OutputEditor.h"

namespace
{
// ---- 显示名格式化函数(03 §1.2-1.8)----

juce::String widthToString(float v, int)
{
    return juce::String(static_cast<int>(std::lround(v))) + " %";
}
float widthFromString(const juce::String& s)
{
    return juce::jlimit(0.0f, 150.0f, s.trimCharactersAtEnd(" %").getFloatValue());
}

juce::String msBalanceToString(float v, int)
{
    if (v < 0.0f)
        return "M" + juce::String(static_cast<int>(std::lround(-v)));
    if (v > 0.0f)
        return "S" + juce::String(static_cast<int>(std::lround(v)));
    return "0";
}
float msBalanceFromString(const juce::String& s)
{
    const juce::String t = s.trim();
    if (t.startsWithIgnoreCase("M"))
        return juce::jlimit(-100.0f, 0.0f, -t.substring(1).getFloatValue());
    if (t.startsWithIgnoreCase("S"))
        return juce::jlimit(0.0f, 100.0f, t.substring(1).getFloatValue());
    return juce::jlimit(-100.0f, 100.0f, t.getFloatValue());
}

juce::String leadSelectToString(int v, int)
{
    if (v == 0)
        return "Follow";
    return juce::String::formatted("Track %02d", v);
}
int leadSelectFromString(const juce::String& s)
{
    juce::String t = s.trim();
    if (t.equalsIgnoreCase("Follow"))
        return 0;
    if (t.startsWithIgnoreCase("Track"))
        t = t.substring(5);
    return juce::jlimit(0, 15, t.getIntValue());
}

juce::String panToString(float v, int)
{
    if (v == 0.0f)
        return "C";
    juce::String num = juce::String(std::abs(v), 1);
    if (num.endsWith(".0"))
        num = num.dropLastCharacters(2);
    return (v < 0.0f ? "L" : "R") + num;
}
float panFromString(const juce::String& s)
{
    const juce::String t = s.trim();
    if (t.equalsIgnoreCase("C"))
        return 0.0f;
    if (t.startsWithIgnoreCase("L"))
        return juce::jlimit(-100.0f, 0.0f, -t.substring(1).getFloatValue());
    if (t.startsWithIgnoreCase("R"))
        return juce::jlimit(0.0f, 100.0f, t.substring(1).getFloatValue());
    return juce::jlimit(-100.0f, 100.0f, t.getFloatValue());
}

juce::String volToString(float v, int)
{
    const juce::String num = juce::String(v, 1);
    if (v > 0.0f)
        return "+" + num + " dB";
    return num + " dB"; // "-12.5 dB" / "0.0 dB"
}
float volFromString(const juce::String& s)
{
    return juce::jlimit(-24.0f, 12.0f, s.trimCharactersAtEnd(" dB").getFloatValue());
}

juce::String trackWidthToString(float v, int)
{
    return juce::String(static_cast<int>(std::lround(v))) + " %";
}
float trackWidthFromString(const juce::String& s)
{
    return juce::jlimit(0.0f, 100.0f, s.trimCharactersAtEnd(" %").getFloatValue());
}

juce::String freezeToString(int v, int)
{
    switch (v)
    {
    case 1:
        return "Pan Frozen";
    case 2:
        return "Vol Frozen";
    case 3:
        return "All Frozen";
    case 0:
    default:
        return "Auto";
    }
}
int freezeFromString(const juce::String& s)
{
    const juce::String t = s.trim();
    if (t.equalsIgnoreCase("Pan Frozen"))
        return 1;
    if (t.equalsIgnoreCase("Vol Frozen"))
        return 2;
    if (t.equalsIgnoreCase("All Frozen"))
        return 3;
    if (t.equalsIgnoreCase("Auto"))
        return 0;
    return juce::jlimit(0, 3, t.getIntValue());
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout scvb::s2::makeS2Layout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    // 全局三件(index 0-2,根组)。
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID{"width", 1}, "Width",
                                                     NormalisableRange<float>(0.0f, 150.0f, 0.0f, 1.0f), 100.0f,
                                                     AudioParameterFloatAttributes()
                                                         .withLabel("%")
                                                         .withStringFromValueFunction(widthToString)
                                                         .withValueFromStringFunction(widthFromString)));
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID{"ms_balance", 1}, "MS Balance",
                                                     NormalisableRange<float>(-100.0f, 100.0f, 0.0f, 1.0f), 0.0f,
                                                     AudioParameterFloatAttributes()
                                                         .withStringFromValueFunction(msBalanceToString)
                                                         .withValueFromStringFunction(msBalanceFromString)));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID{"lead_select", 1}, "Lead Select", 0, 15, 0,
                                                   AudioParameterIntAttributes()
                                                       .withStringFromValueFunction(leadSelectToString)
                                                       .withValueFromStringFunction(leadSelectFromString)));

    for (int v = 1; v <= kNumVersions; ++v)
    {
        auto vg = std::make_unique<AudioProcessorParameterGroup>("v" + String(v), "Version " + String(v), "|");
        for (int t = 1; t <= kNumTracks; ++t)
        {
            auto tg = std::make_unique<AudioProcessorParameterGroup>(String::formatted("v%d_t%02d", v, t),
                                                                     String::formatted("Track %02d", t), "|");

            tg->addChild(std::make_unique<AudioParameterFloat>(
                ParameterID{String::formatted("v%d_t%02d_pan", v, t), 1}, String::formatted("V%d T%02d Pan", v, t),
                NormalisableRange<float>(-100.0f, 100.0f, 0.0f, 1.0f), 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction(panToString)
                    .withValueFromStringFunction(panFromString)));
            tg->addChild(std::make_unique<AudioParameterFloat>(
                ParameterID{String::formatted("v%d_t%02d_vol", v, t), 1}, String::formatted("V%d T%02d Vol", v, t),
                NormalisableRange<float>(-24.0f, 12.0f, 0.0f, 1.0f), 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel("dB")
                    .withStringFromValueFunction(volToString)
                    .withValueFromStringFunction(volFromString)));
            tg->addChild(std::make_unique<AudioParameterFloat>(
                ParameterID{String::formatted("v%d_t%02d_width", v, t), 1}, String::formatted("V%d T%02d Width", v, t),
                NormalisableRange<float>(0.0f, 100.0f, 0.0f, 1.0f), 100.0f,
                AudioParameterFloatAttributes()
                    .withLabel("%")
                    .withStringFromValueFunction(trackWidthToString)
                    .withValueFromStringFunction(trackWidthFromString)));
            tg->addChild(
                std::make_unique<AudioParameterInt>(ParameterID{String::formatted("v%d_t%02d_freeze", v, t), 1},
                                                    String::formatted("V%d T%02d Freeze", v, t), 0, 3, 0,
                                                    AudioParameterIntAttributes()
                                                        .withStringFromValueFunction(freezeToString)
                                                        .withValueFromStringFunction(freezeFromString)));

            vg->addChild(std::move(tg)); // 铁律:先 addChild 再 layout.add
        }
        layout.add(std::move(vg));
    }
    return layout;
}

S2OutputProcessor::S2OutputProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "SCVB S2 Params", scvb::s2::makeS2Layout())
{
    // 缓存 30 条打印车道(活跃版本 v1 的 15 轨 × pan/vol)。
    for (int t = 1; t <= scvb::s2::kNumTracks; ++t)
    {
        lanes_[static_cast<size_t>((t - 1) * 2 + 0)].param =
            dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(juce::String::formatted("v1_t%02d_pan", t)));
        lanes_[static_cast<size_t>((t - 1) * 2 + 1)].param =
            dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(juce::String::formatted("v1_t%02d_vol", t)));
    }

    // 布局自检(编译期 static_assert 已保证计数;运行期核对头尾 ID 与总数)。
    jassert(getParameters().size() == scvb::s2::kNumAutomatable);
    jassert(getParameters().getFirst()->getParameterID().getParamID() == "width");
    jassert(getParameters().getLast()->getParameterID().getParamID() == "v2_t15_freeze");

    startTimerHz(50);
}

S2OutputProcessor::~S2OutputProcessor()
{
    stopTimer();
    if (gestureOpen_)
        endPass();
}

void S2OutputProcessor::prepareToPlay(double, int) {}
void S2OutputProcessor::releaseResources() {}

bool S2OutputProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo() &&
        layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono())
    {
        return false;
    }
    return layouts.getMainOutputChannelSet() == layouts.getMainInputChannelSet();
}

void S2OutputProcessor::processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // 音频线程只发布 playhead 快照(ADR-006):写入全在消息线程。
    if (auto* ph = getPlayHead())
    {
        juce::AudioPlayHead::CurrentPositionInfo info;
        if (ph->getCurrentPosition(info))
        {
            isPlaying_.store(info.isPlaying ? 1 : 0);
            playheadSeconds_.store(static_cast<float>(info.timeInSeconds));
        }
    }
    // spike:直通,无 DSP。
}

juce::AudioProcessorEditor* S2OutputProcessor::createEditor()
{
    return new S2OutputEditor(*this);
}

void S2OutputProcessor::setCurrentProgram(int) {}
const juce::String S2OutputProcessor::getProgramName(int)
{
    return {};
}
void S2OutputProcessor::changeProgramName(int, const juce::String&) {}

void S2OutputProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const char b = outputEnabled_ ? 1 : 0;
    destData.setSize(1);
    destData.copyFrom(&b, 0, 1);
}

void S2OutputProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data != nullptr && sizeInBytes >= 1)
        outputEnabled_ = (static_cast<const char*>(data)[0] != 0);
}

void S2OutputProcessor::setOutputEnabled(bool on)
{
    outputEnabled_ = on;
    if (!on && gestureOpen_)
        endPass(); // 关闭 = 打印出口之一(03 §2.2)。
}

void S2OutputProcessor::timerCallback()
{
    const bool playing = isPlaying_.load() != 0;
    const float t = playheadSeconds_.load() + static_cast<float>(kLookaheadMs) * 0.001f;

    const bool inWindow = t >= 0.0f && t <= static_cast<float>(kPrintWindowSeconds);
    const bool print = outputEnabled_ && playing && inWindow;

    if (print && !gestureOpen_)
        beginPass();
    if (!print && gestureOpen_)
        endPass();
    if (!print)
        return;

    const float tc = juce::jlimit(0.0f, static_cast<float>(kPrintWindowSeconds), t);
    for (int i = 0; i < scvb::s2::kNumPrintLanes; ++i)
    {
        Lane& lane = lanes_[static_cast<size_t>(i)];
        if (lane.param == nullptr)
            continue;
        const float v = envelopeValue(i, tc);
        const float deadband = (i % 2 == 0) ? kDeadbandPan : kDeadbandVolDb;
        if (lane.dirty || std::abs(v - lane.lastWritten) > deadband)
        {
            lane.param->setValueNotifyingHost(lane.param->convertTo0to1(v));
            lane.lastWritten = v;
            lane.dirty = false;
        }
    }
}

void S2OutputProcessor::beginPass()
{
    for (auto& lane : lanes_)
        if (lane.param != nullptr)
            lane.param->beginChangeGesture();
    gestureOpen_ = true;
}

void S2OutputProcessor::endPass()
{
    for (auto& lane : lanes_)
        if (lane.param != nullptr)
            lane.param->endChangeGesture();
    gestureOpen_ = false;
    for (auto& lane : lanes_)
        lane.dirty = true; // 下一 pass 首点必写
}

float S2OutputProcessor::envelopeValue(int laneIndex, float timeSec) const
{
    const int track = laneIndex / 2; // 0..14
    const int kind = laneIndex % 2; // 0=pan,1=vol

    if (kind == 0)
    {
        // pan 阶梯:-60/-20/+20/+60,每 4s 一档,轨道间错相。
        const float phase = static_cast<float>(track) * 0.25f;
        const int step = static_cast<int>(std::floor(timeSec / 4.0f + phase)) % 4;
        constexpr float levels[4] = {-60.0f, -20.0f, 20.0f, 60.0f};
        return levels[step];
    }

    // vol 阶梯:-6/-3/0/+3/+6 dB,每 3s 一档,轨道间错相。
    const float phase = static_cast<float>(track) * 0.5f;
    const int step = static_cast<int>(std::floor(timeSec / 3.0f + phase)) % 5;
    constexpr float levels[5] = {-6.0f, -3.0f, 0.0f, 3.0f, 6.0f};
    return levels[step];
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new S2OutputProcessor();
}
