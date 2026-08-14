// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// S3 spike —— Output 最小插件壳(仅承载 WebView2 UI,无真实 DSP)。
//   processBlock 直通;50Hz 打印 Timer 实测频率(验收④)+ 25Hz UI Timer 由编辑器持有。
//   数据全由编辑器内伪随机生成器合成(10 §1.3.1);uiScale 走固定设计盒缩放。

#include <juce_audio_processors/juce_audio_processors.h>

class S3OutputProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    S3OutputProcessor();
    ~S3OutputProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "SCVB S3 Output"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // 缩放(固定设计盒 1180×780;真源 05 §1.2 / 10 §1.3.1)。
    float getUiScale() const { return uiScale_; }
    void setUiScale(float f) { uiScale_ = juce::jlimit(0.5f, 2.0f, f); }
    void persistUiScaleAsDefault() {} // spike:无全局配置落盘

    // 50Hz 打印 Timer 实测频率(EWMA,验收④;消息线程读取)。
    float timerHz50() const { return timerHz50_; }

private:
    void timerCallback() override;

    double sampleRate_ = 48000.0;
    int maxBlock_ = 0;
    float uiScale_ = 1.0f;

    double lastTimerTick_ = 0.0;
    float timerHz50_ = 0.0f;
    unsigned long long tick50_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(S3OutputProcessor)
};
