// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"

ScvbOutputAudioProcessor::ScvbOutputAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMETERS", createParameterLayout())
{
}

ScvbOutputAudioProcessor::~ScvbOutputAudioProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout ScvbOutputAudioProcessor::createParameterLayout()
{
    // T15:123 参数冻结布局(params-v0 v2.1 §一 / J59 / J65)。
    return scvb::params::makeOutputLayout();
}

const juce::String ScvbOutputAudioProcessor::getName() const
{
    return "SCVB Output";
}

void ScvbOutputAudioProcessor::prepareToPlay(double /*sampleRate*/, int /*samplesPerBlock*/) {}

void ScvbOutputAudioProcessor::releaseResources()
{
    // T17:releaseResources 出口兜底闭合 gesture(GestureGuard 四出口之一,§3.3 R12)。
    printer.endAllGestures();
}

bool ScvbOutputAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // 直通:输入输出声道数必须一致,支持 mono 与 stereo。
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
        layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;

    return true;
}

void ScvbOutputAudioProcessor::processBlock(juce::AudioBuffer<float>& /*buffer*/, juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;

    // T01 空壳:直通,输入原样输出(不改写 buffer)。
    // 后续:Output 从共享内存环按时间线寻址读取各轨,求和替换总线输入(ADR-002)。
}

void ScvbOutputAudioProcessor::setCurrentProgram(int /*index*/) {}

const juce::String ScvbOutputAudioProcessor::getProgramName(int /*index*/)
{
    return {};
}

void ScvbOutputAudioProcessor::changeProgramName(int /*index*/, const juce::String& /*newName*/) {}

void ScvbOutputAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // T15:最小状态接线 —— 123 参数的保存/恢复(pluginval gate 7 状态往返依赖)。
    const auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void ScvbOutputAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr)
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

// juce_add_plugin 的 VST3/AU wrapper 从这里实例化插件。
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ScvbOutputAudioProcessor();
}
