// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// S1 spike —— Output 插件(最小路由实现)。ADR-002 / J12+J32 / [J57] / [J59]。
//   读 15 环 → 直接求和(pan/vol/width/ms_balance/lead_select 全旁路,增益 1.0;
//   mono 环复制到 L/R、stereo 环 L→L/R→R)→ 替换总线;gapCount/overlapCount 原子计数;
//   新上线 channel 置 mask 位后延迟 ≥200ms 再注入;busXfade 80–200ms 等功率交叉淡变。
// 无 UI、无参数;group 经环境变量 SCVB_GROUP 配置(构造参数)。

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "AudioRing.h"
#include "BusXfade.h"
#include "Registry.h"
#include "SegmentBackendWin32.h"
#include "SegmentLayout.h"

using namespace scvb;

class OutputProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    OutputProcessor();
    ~OutputProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    const juce::String getName() const override { return "SCVB Output"; }
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

private:
    void timerCallback() override;
    void pollChannels(u64 now);
    void refreshCtrl();
    void tryClaim(u64 now);

    // 配置
    u32 group_ = 1;

    // IPC
    u32 pid_ = 0;
    std::unique_ptr<scvb::Registry> registry_;
    std::array<std::unique_ptr<scvb::AudioRing>, scvb::kMaxChannels> rings_;
    std::atomic<u32> outputActive_{0}; // 0=observer/off,1=active(主实例)
    bool observer_ = false;

    // 音频线程缓冲
    int maxBlock_ = 0;
    double sampleRate_ = 0.0;
    std::vector<float> accumL_;
    std::vector<float> accumR_;
    std::vector<float> trackBuf_; // maxBlock * 2(stereo 上限)

    // 计数导出(→ ctrl OutputGlobalInfo)
    std::array<std::atomic<u32>, scvb::kMaxChannels> gapCount_{};
    std::array<std::atomic<u32>, scvb::kMaxChannels> overlapCount_{};
    std::array<std::atomic<u64>, scvb::kMaxChannels> epochSummary_{};

    // [M] 25Hz 评估的 mask(供 [A] 读)
    std::atomic<u32> usableMask_{0};
    std::array<u64, scvb::kMaxChannels> onlineSince_{}; // [M] only

    // ctrl 段
    scvb::SegmentView ctrlView_;
    scvb::OutputGlobalInfo* ctrl_ = nullptr;
    bool ctrlOpen_ = false;

    scvb::BusXfade busXfade_;
    u32 tick_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OutputProcessor)
};
