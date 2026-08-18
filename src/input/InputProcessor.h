// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// windows.h 的 min/max 宏会污染 std::numeric_limits<T>::min() 与 std::max;先禁用再包含
// SegmentBackendWin32.h(其内部 include windows.h 但未定义 NOMINMAX)。
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

#include "input/InputSession.h"
#include "input/OutputStage.h"
#include "ipc/SegmentBackendWin32.h"
#include "state/InputStateCodec.h"
#include "state/StateCodec.h"

// ScvbInputAudioProcessor —— Input 插件处理器(01 §5.1 伪代码全实现,T23)。
// 捕获(mono/stereo,[J57])→ 时间线定位 → epoch → 写音频环 → 特征(BS.1770 多通道求和,T08 口径)
// → 输出级仲裁(J12/J32:健康静音 / 其余直通,80ms 等功率 ramp + 5s 滞回)。
class ScvbInputAudioProcessor final : public juce::AudioProcessor, private juce::Timer
{
public:
    ScvbInputAudioProcessor();
    ~ScvbInputAudioProcessor() override;

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

    // [M] UI/桥入口(T25 冻结契约):设置 channel/group,触发 claim 迁移(01 §4.1)。
    void setChannelId(int channelId);
    void setGroupId(int groupId);

private:
    // 25Hz [M] 定时器:健康判定 → C18 模式字;每 ~250ms 心跳(4Hz)。
    void timerCallback() override;

    // 捕获:interleaved capBuf 打包([J57] 不下混、不互换)。
    static void captureFrames(const float* const* src, int srcCh, float* dst, int n);
    // 跨零点块:写 t0>=0 尾段(R3,01 §5.1 步骤 2)。
    void writeTailFromZero(const float* interleaved, int n, int64_t t0);

    juce::CriticalSection lifecycleMutex_; // 串行化 prepareToPlay/release/setState/claim/心跳([M] 与宿主回调互斥)

    // IPC(段操作持 lifecycleMutex_ 于非实时线程;音频线程只经 session_ 拿裸指针做原子读写)。
    scvb::SegmentBackendWin32 backend_;
    scvb::input::InputSession session_;

    // 输出级仲裁(J12/J32)。
    scvb::input::StageSwitchStateMachine stageMachine_;
    scvb::input::RampSwitcher rampSwitcher_;
    std::atomic<scvb::u32> c18Stage_{static_cast<scvb::u32>(scvb::input::OutputStageMode::kPassthrough)};
    std::atomic<scvb::u32> captureArmed_{0}; // 采集开关(Output ctrl 广播 capture_enabled;ADR-007)
    std::atomic<float> meter_{0.0f};

    // state(持久化经 T19 StateCodec + InputStateCodec;params-v0 §三)。
    int channelId_ = 0;
    int groupId_ = 1;
    int uiScale_ = 100;
    juce::String uiLanguage_ = "en";

    bool prepared_ = false;

    // 音频线程零分配缓冲(prepareToPlay 分配)。
    double sampleRate_ = 48000.0;
    int srcChannels_ = 1; // [J57] 1|2,prepareToPlay 依布局判定,运行期不变
    int preparedMaxBlock_ = 512;
    std::vector<float> capInterleaved_; // stereo 容量(2 × preparedMaxBlock),interleaved LR
    std::array<const float*, 2> planarPtrs_{};

    // 时间线定位(§5.1 步骤 3)。
    int64_t lastT0_ = std::numeric_limits<int64_t>::lowest();
    int64_t expectedNext_ = std::numeric_limits<int64_t>::lowest();
    uint64_t lastHeartbeatMs_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScvbInputAudioProcessor)
};
