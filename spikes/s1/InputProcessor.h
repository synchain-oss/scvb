// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// S1 spike —— Input 插件(最小路由实现)。ADR-002 v1 / J12+J32 / [J57]。
//   捕获 → 写共享内存环(mono 原样、stereo interleaved LR)→ 经 RampSwitcher 输出
//   SilenceStage(健康连接)/ PassthroughStage(无健康 Output)。
// 无 UI、无参数、无特征段;channel/group 经环境变量(SCVB_CHANNEL/SCVB_GROUP)或 state 配置。

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "AudioRing.h"
#include "OutputStage.h"
#include "Registry.h"
#include "SegmentLayout.h"

using namespace scvb;

class InputProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    InputProcessor();
    ~InputProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    const juce::String getName() const override { return "SCVB Input"; }
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

    // 配置解析:env > state > 默认。
    void resolveConfig();
    // claim + 创建环(消息线程)。
    void doClaim();
    // v9:认领失败落一条诊断日志(每次失败事件仅一条,成功时复位)。
    void logClaimFail(const char* reason);
    // 25Hz 健康仲裁(5s 滞回仅作用于 静音→直通)。
    void pollHealth(u64 now);
    bool isHealthy(u64 now) const;
    void setMutedFlag(bool muted);

    void captureInterleaved(const juce::AudioBuffer<float>& buffer, int offset, int numSamples);

    // 音频线程成员
    int srcChannels_ = 1;
    int maxBlock_ = 0;
    double sampleRate_ = 0.0;
    std::vector<float> capBuf_; // interleaved, maxBlock*srcChannels
    i64 lastT0_ = -1;
    i64 expectedNext_ = -1;

    // v6 诊断计数(仅音频线程写,消息线程读;§8 零 I/O)。
    std::atomic<u64> audioBlocks_{0};
    std::atomic<u64> audioSamples_{0};
    std::atomic<i64> lastWriteT0_{-1};
    u64 lastLoggedBlocks_ = 0;

    // 配置
    u32 group_ = 1;
    u32 channel_ = 0; // 0 = 自动 claim 最低空闲 slot
    u32 envGroup_ = 0;
    u32 envChannel_ = 0;
    u32 stateGroup_ = 0;
    u32 stateChannel_ = 0;

    // IPC(组号 G 为构造参数,env/state 解析后惰性创建)
    u32 pid_ = 0;
    std::unique_ptr<Registry> registry_;
    std::unique_ptr<AudioRing> ring_;
    RampSwitcher stage_;

    bool claimed_ = false;
    bool active_ = false;
    bool claimFailLogged_ = false; // v9:每次失败事件只落一条 claimFail 日志

    // 健康仲裁状态
    u64 unhealthySince_ = 0;
    u64 silenceCommandedAt_ = 0;
    bool mutePending_ = false;
    u32 tick_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InputProcessor)
};
