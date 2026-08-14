// SPDX-License-Identifier: GPL-3.0-or-later
#include "S3OutputProcessor.h"

#include <cstring>

#include "S3OutputEditor.h"

S3OutputProcessor::S3OutputProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    // 50Hz 打印 Timer(验收④:UI 全负载下实测 ≥ 30Hz)。
    startTimerHz(50);
}

S3OutputProcessor::~S3OutputProcessor()
{
    stopTimer();
}

void S3OutputProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    maxBlock_ = samplesPerBlock;
}

void S3OutputProcessor::releaseResources() {}

bool S3OutputProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo() &&
        layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono())
    {
        return false;
    }
    return layouts.getMainOutputChannelSet() == layouts.getMainInputChannelSet();
}

void S3OutputProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    // spike:直通,不做 DSP。UI 承载力是唯一被测对象。
}

juce::AudioProcessorEditor* S3OutputProcessor::createEditor()
{
    return new S3OutputEditor(*this);
}

void S3OutputProcessor::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (lastTimerTick_ > 0.0)
    {
        const double dt = now - lastTimerTick_;
        if (dt > 0.0)
        {
            const float hz = static_cast<float>(1000.0 / dt);
            timerHz50_ = (timerHz50_ <= 0.0f) ? hz : timerHz50_ * 0.95f + hz * 0.05f;
        }
    }
    lastTimerTick_ = now;
    ++tick50_;
}

void S3OutputProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const float scale = uiScale_;
    destData.setSize(sizeof(scale));
    destData.copyFrom(&scale, 0, sizeof(scale));
}

void S3OutputProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes < static_cast<int>(sizeof(float)))
    {
        return;
    }
    float scale = 1.0f;
    std::memcpy(&scale, data, sizeof(scale));
    if (scale >= 0.5f && scale <= 2.0f)
    {
        uiScale_ = scale;
    }
}

void S3OutputProcessor::setCurrentProgram(int) {}
const juce::String S3OutputProcessor::getProgramName(int)
{
    return {};
}
void S3OutputProcessor::changeProgramName(int, const juce::String&) {}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new S3OutputProcessor();
}
