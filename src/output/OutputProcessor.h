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

#include "OutputAuthority.h"
#include "OutputParams.h"
#include "dsp/ParamSmoother.h"
#include "engine/PlayheadShot.h"
#include "AutomationPrinter.h"
#include "ipc/SegmentBackendWin32.h"
#include "output/BusXfade.h"
#include "output/OutputSession.h"
#include "state/OutputStateCodec.h"
#include "state/StateCodec.h"

// ScvbOutputAudioProcessor —— Output 插件处理器(01 §5.2 伪代码全实现,T24)。
// 读 15 环 → covered 判定/读中换代弃用/失准计数(ShmRingMixSource)→ 取值仲裁(OutputAuthority/
// DspArbiter)→ gain/pan(mono equal-power / stereo dual-pan+width,[J57])→ 求和 → ms_balance
// ([J58] 总线 M/S)→ busXfade 替换总线([J32] 200ms 注入延迟 + 80ms per-channel 淡入)。
class ScvbOutputAudioProcessor final : public juce::AudioProcessor, private juce::Timer
{
public:
    ScvbOutputAudioProcessor();
    ~ScvbOutputAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

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

    // T15:123 参数冻结布局(APVTS)。
    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // T17:50Hz 打印器挂 Processor 而非 Editor。
    scvb::output::AutomationPrinter& getPrinter() { return printer_; }

    // [M] UI/桥入口(T25 冻结契约):group/采集/输出开关/活动版本。
    void setGroupId(int groupId);
    void setCaptureEnabled(bool on);
    void setOutputEnabled(bool on);
    void setVersionActive(int version);
    int groupId() const { return groupId_; }

    // PR#53 R1:setStateInformation 读到 abi > kCurrentAbi → 拒载并置位(冻结契约 §7.3:高版本拒载
    // + 提示升级,绝不静默降级;原 blob 由宿主工程保有)。消息线程读写(setStateInformation 持
    // lifecycleMutex_),T30 桥经此把 abiMismatch 横幅推给 UI(Input PR#51 红旗#1 同款)。
    bool hasStateAbiMismatch() const noexcept { return stateAbiMismatch_; }
    scvb::u32 stateAbiSeen() const noexcept { return stateAbiSeen_; }

private:
    // 25Hz [M] 定时器:心跳(4Hz 折半)+ session tick(per-channel 判定/看门狗/全局小节)。
    void timerCallback() override;

    // [A] 从 playhead 构造 C8 快照并发布(§5.2 / C8)。
    void publishPlayhead(const juce::AudioPlayHead::PositionInfo& pos, bool haveTime, bool playing);

    // [A] 读全局三件 raw(host 恒权威,不参与仲裁)。
    float readGlobalWidth() const noexcept;
    float readMsBalance() const noexcept;

    // [A] 按时间线读 15 环做 unity 求和(§5.4 bypass 语义)。
    void renderBypassedUnity(juce::AudioBuffer<float>& buffer, int n, int64_t t0);

    // [M] 版本切换/接线:authority 重绑活动版本 + 打印器重绑车道(曲线真身,T24 无分析曲线)。
    void rebindVersion();

    juce::CriticalSection lifecycleMutex_; // 串行化 prepare/release/setState/claim/心跳

    // IPC(段操作持 lifecycleMutex_ 于非实时线程;音频线程只经 session_ 拿裸指针做原子读写)。
    scvb::SegmentBackendWin32 backend_;
    scvb::output::OutputSession session_;

    // 取值仲裁 + 平滑(T16/DspArbiter + T18 版本层)。
    scvb::output::OutputAuthority authority_;
    // 打印器(T17;本卡接线 setShot/setCurves/bindVersion/startPrinting/hostEchoShield)。
    scvb::output::AutomationPrinter printer_;
    // 总线交叉淡变 + per-channel 淡入(§5.2 过渡语义 R2)。
    scvb::output::BusXfade busXfade_;

    // C8 playhead 快照(音频线程写 / 打印器消息线程读)。
    scvb::engine::PlayheadShot playheadShot_;

    juce::AudioProcessorValueTreeState apvts;
    scvb::params::ParamHandles handles_;

    // state(params-v0 §二 最小 T24 子集)。
    int groupId_ = 1;
    bool captureEnabled_ = false;
    bool outputEnabled_ = true;
    int versionActive_ = 1;
    int uiScale_ = 100;
    juce::String uiLanguage_ = "en";

    // PR#53 R1:state abi 拒载标志 + 保留的宿主原始字节 + 上次成功加载的容器(未知 chunk 原样回写)。
    bool stateAbiMismatch_ = false;
    scvb::u32 stateAbiSeen_ = 0;
    std::vector<std::uint8_t> preservedStateBlob_; // 拒载更高 abi 后保留的宿主原始字节(getStateInformation 原样回写)
    scvb::state::StateChunks loadedChunks_; // 上次成功加载的容器(FEAT/CRVS/未知 fourcc 原样回写,T19 纪律)

    bool prepared_ = false;
    double sampleRate_ = 48000.0;
    int preparedMaxBlock_ = 512;

    // 音频线程时间线状态(§5.2 步骤 2)。
    int64_t lastT0Out_ = std::numeric_limits<int64_t>::lowest();
    int64_t expectedNextOut_ = std::numeric_limits<int64_t>::lowest();
    uint32_t podEpoch_ = 0;
    std::atomic<uint64_t> timelineInvalidBlocks_{0}; // [A] 无时间线计数 / [M] 健康前置
    std::atomic<uint32_t> timelineValid_{1}; // [A] 本块时间线有效标志(负 t0 视为有效,[J51])

    // 音频线程零分配缓冲(prepareToPlay 分配)。
    std::vector<float> accumL_;
    std::vector<float> accumR_;
    std::array<std::vector<float>, 15> trackBuf_; // stereo 容量(2 × preparedMaxBlock)

    // 平滑器([A] 独占):ms_balance g_M/g_S(10ms)、全局 width(10ms)、per-channel 注入 fade(80ms)。
    scvb::dsp::LinearSmoother gMSmoother_{1.0f};
    scvb::dsp::LinearSmoother gSSmoother_{1.0f};
    scvb::dsp::LinearSmoother globalWidthSmoother_{100.0f};
    std::array<scvb::dsp::LinearSmoother, 15> channelFade_;

    // [M] 状态。
    uint64_t lastHeartbeatMs_ = 0;
    int timelineInvalidTicks_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScvbOutputAudioProcessor)
};
