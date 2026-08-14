// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"

ScvbInputAudioProcessor::ScvbInputAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
}

ScvbInputAudioProcessor::~ScvbInputAudioProcessor() = default;

const juce::String ScvbInputAudioProcessor::getName() const
{
    return "SCVB Input";
}

void ScvbInputAudioProcessor::prepareToPlay(double /*sampleRate*/, int /*samplesPerBlock*/) {}

void ScvbInputAudioProcessor::releaseResources() {}

bool ScvbInputAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // 直通:输入输出声道数必须一致,支持 mono 与 stereo。
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
        layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;

    return true;
}

void ScvbInputAudioProcessor::processBlock(juce::AudioBuffer<float>& /*buffer*/, juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;

    // T01 空壳:直通,输入原样输出(不改写 buffer)。
    // 后续:Input 捕获人声写入共享内存环,并向宿主输出静音(ADR-002)。
}

void ScvbInputAudioProcessor::setCurrentProgram(int /*index*/) {}

const juce::String ScvbInputAudioProcessor::getProgramName(int /*index*/)
{
    return {};
}

void ScvbInputAudioProcessor::changeProgramName(int /*index*/, const juce::String& /*newName*/) {}

void ScvbInputAudioProcessor::getStateInformation(juce::MemoryBlock& /*destData*/) {}

void ScvbInputAudioProcessor::setStateInformation(const void* /*data*/, int /*sizeInBytes*/) {}

// juce_add_plugin 的 VST3/AU wrapper 从这里实例化插件。
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ScvbInputAudioProcessor();
}
