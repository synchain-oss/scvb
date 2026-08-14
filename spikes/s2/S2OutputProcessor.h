// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// S2 spike —— 自动化写入验证插件(J16:验证代码,不进生产路径,由 T15 的 PR 删除)。
//   123 参数完整布局(params-v0 v2.1 §一 / 03 §1)+ AutomationPrinter 打一条固定阶梯包络。
//   无 DSP(processBlock 直通)、无分析、无 IPC;唯一被测对象 = 参数布局 + gesture 写入在 DAW 的落点。

#include <array>
#include <atomic>

#include <juce_audio_processors/juce_audio_processors.h>

namespace scvb::s2
{
constexpr int kNumVersions = 2; // [J59]
constexpr int kNumTracks = 15; // [J59]
constexpr int kParamsPerTrack = 4; // pan / vol / width / freeze [J65]

// index(v,t,k) = 3 + (v-1)*60 + (t-1)*4 + k,k∈{0=pan,1=vol,2=width,3=freeze} → 末位 122,共 123。
constexpr int kNumAutomatable = 3 + kNumVersions * kNumTracks * kParamsPerTrack;
static_assert(kNumAutomatable == 123, "params v2.1 frozen count");

// 打印面(引擎只写 pan/vol,J63):活跃版本 15 轨 × 2 = 30 条。
constexpr int kNumPrintLanes = kNumTracks * 2;

juce::AudioProcessorValueTreeState::ParameterLayout makeS2Layout();
} // namespace scvb::s2

class S2OutputProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    S2OutputProcessor();
    ~S2OutputProcessor() override;

    // 123 参数树(唯一真源;宿主可见 124 = 123 + wrapper 合成 bypass)。
    juce::AudioProcessorValueTreeState apvts;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "SCVB S2 Output"; }
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

    // Output(打印)开关:纯 state 语义(非自动化,ADR-004/005)。spike 用布尔成员,不进 APVTS,
    // 以免破坏 123 声明数 / 124 宿主可见数。
    void setOutputEnabled(bool on);
    bool isOutputEnabled() const { return outputEnabled_; }

    // 编辑器状态栏用(消息线程读)。
    bool isPrinting() const { return gestureOpen_; }
    float lastPlayheadSeconds() const { return playheadSeconds_.load(); }

private:
    void timerCallback() override;

    struct Lane
    {
        juce::AudioParameterFloat* param = nullptr;
        bool dirty = true; // 首点必写
        float lastWritten = 0.0f; // 上次写出的原生值(值域单位)
    };

    void beginPass();
    void endPass();
    float envelopeValue(int laneIndex, float timeSec) const;

    std::array<Lane, scvb::s2::kNumPrintLanes> lanes_;

    bool outputEnabled_ = false;
    bool gestureOpen_ = false;

    // 打印窗口(固定 30s;区间外零 gesture → S2-A0 / R5)。
    static constexpr double kPrintWindowSeconds = 30.0;

    // deadband / lookahead 默认值(03 §3.2;待定项⑦由 S2 实测校准)。
    static constexpr float kDeadbandPan = 0.5f;
    static constexpr float kDeadbandVolDb = 0.1f;
    static constexpr int kLookaheadMs = 20;

    std::atomic<float> playheadSeconds_{0.0f};
    std::atomic<int> isPlaying_{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(S2OutputProcessor)
};
