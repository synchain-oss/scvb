// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "AutomationPrinter.h"
#include "OutputParams.h"

// T01 空壳:processBlock 直通的最小 Output 插件。
// ADR-002:Output 最终将从共享内存环读取各 channel 时间线区间,做每轨 gain/pan 后求和替换总线输入。
// 本骨架仅直通;共享内存/IPC/时间线寻址由 S1 spike(T02)与后续任务填充。
// T15:APVTS 接入 123 参数冻结布局(scvb::params::makeOutputLayout)。
class ScvbOutputAudioProcessor : public juce::AudioProcessor
{
public:
    ScvbOutputAudioProcessor();
    ~ScvbOutputAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    const juce::String getName() const override;
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

    // T15:123 参数冻结布局(APVTS)。
    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // T17:50Hz 打印器挂 Processor 而非 Editor(§4.2 REAPER:GUI 关闭也要打印)。
    // 【端到端接线归 T24】T17 只挂实例并接 releaseResources 兜底;运行态接线
    // (processBlock 发布 PlayheadShot、setMode/setCurves/bindVersion/startPrinting、
    // installHostEchoShield)由 T24 的 OutputProcessor 完整 processBlock 完成。
    scvb::output::AutomationPrinter& getPrinter() { return printer; }

private:
    juce::AudioProcessorValueTreeState apvts;
    scvb::output::AutomationPrinter printer; // 声明在 apvts 之后 → 析构先于 apvts(参数存活)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScvbOutputAudioProcessor)
};
