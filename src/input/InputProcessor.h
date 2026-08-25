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
#include "ipc/CtrlPlane.h"
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

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

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
    // 返回迁移后的 claim 态(T30 桥据此回 {ok}/{conflict:true} 并经 scvb.state 回推 claim)。
    scvb::input::InputClaimState setChannelId(int channelId);
    scvb::input::InputClaimState setGroupId(int groupId);

    // PR#51 红旗#1:setStateInformation 读到 abi > kCurrentAbi → 拒载并置位(冻结契约 §7.3:
    // 高版本拒载 + 提示升级,绝不静默丢数据;原 blob 由宿主工程保有)。消息线程读写(setStateInformation
    // 持 lifecycleMutex_),T30 桥经此把 abiMismatch 横幅推给 UI。
    bool hasStateAbiMismatch() const noexcept { return stateAbiMismatch_; }
    scvb::u32 stateAbiSeen() const noexcept { return stateAbiSeen_; }

    // --- T30 Input 桥接入面([M] 编辑器消息线程;除注明外均持 lifecycleMutex_,绝不触碰音频线程成员)---
    struct PriorityResult
    {
        bool queued = false;
        juce::String reason; // ""=成功;"ringFull" | "outputOffline" | "unassigned" | "busy"(§3.4/§5.6;busy=ctrl
                             // 段未打开,临时可重试)
    };
    struct BridgeTickSnapshot
    {
        int channelId = 0;
        int groupId = 1;
        scvb::input::InputClaimState claimState = scvb::input::InputClaimState::kUnassigned;
        scvb::input::InputConnSnapshot conn;
        bool healthy = false; // session_.isHealthy(now)
        bool passthrough = true; // StageSwitchStateMachine 当前目标档
        double sampleRate = 48000.0;
        int sourceChannels = 1;
        scvb::OutputGlobalInfoSnapshot globalInfo; // 本组 ctrl 段 OutputGlobalInfo(srMismatch 推导源)
        // 本组 ctrl 段广播区(Output→Input 配置只读镜像,§4.3)。broadcastValid=false = 段未打开
        // 或本次 seqlock 撕裂 —— 调用方沿用上帧,不自旋。
        scvb::CtrlBroadcastSnapshot broadcast;
        bool broadcastValid = false;
        // 变化检测真源 = **广播区的** config_seq。**不是** registry 的 OutputSlot.config_seq:
        // 后者从未被插件代码写过(只有单测调 bumpConfigSeq),恒 0,于是 scvb.config 每开一次
        // 编辑器只发一帧就再不更新(T37 三轮 C 族)。
        scvb::u32 configSeq = 0;
        scvb::u32 localAbi = scvb::kScvbAbi;
        scvb::u32 remoteAbi = 0; // abi 不符时探测到的对端 abi(0 = 未探测到)
    };
    BridgeTickSnapshot bridgeTickSnapshot(); // 25Hz emitTick 单次持锁采集(含 ctrl 段懒打开)
    std::uint8_t bridgeGroupsOnline(); // 1Hz:本组位 + 跨组只读探测(01 §4.5/J70)
    PriorityResult bridgeRemoteSetPriority(int n); // remoteSetPriority(§3.4;内部 clamp 0..10)
    void bridgeSetUiLanguage(const juce::String& lang); // setLang 落 state(normalize 由桥层做)
    void bridgeSetUiScalePercent(int percent); // commitUiScale 落 state(clamp 33..300)
    int bridgeUiScalePercent() const;
    juce::String bridgeUiLanguage() const;

private:
    // 25Hz [M] 定时器:健康判定 → C18 模式字;每 ~250ms 心跳(4Hz)。
    void timerCallback() override;

    // 命令环 ctrl 段懒打开(调用方已持 lifecycleMutex_):Input 是本组 ctrl 段的合法创建/覆盖者
    // (写命令环 + 读 OutputGlobalInfo;CtrlPlane::open 注释同口径)。
    void ensureCtrlOpen();

    // 捕获:interleaved capBuf 打包([J57] 不下混、不互换)。
    static void captureFrames(const float* const* src, int srcCh, float* dst, int n);
    // 跨零点块:写 t0>=0 尾段(R3,01 §5.1 步骤 2)。b = 本块 acquireBlock() 的音频环绑定快照。
    void writeTailFromZero(const scvb::AudioRingBinding* b, const float* interleaved, int n, int64_t t0);

    juce::CriticalSection lifecycleMutex_; // 串行化 prepareToPlay/release/setState/claim/心跳([M] 与宿主回调互斥)

    // IPC(段操作持 lifecycleMutex_ 于非实时线程;音频线程只经 session_ 拿裸指针做原子读写)。
    scvb::SegmentBackendWin32 backend_;
    scvb::input::InputSession session_;
    // 本组 ctrl 段(T30 桥:remoteSetPriority 命令环上行 + OutputGlobalInfo 只读;per-组语义 J66,
    // setGroupId 经 CtrlPlane::changeGroup 随组走)。
    scvb::CtrlPlane ctrl_;
    // 广播区上一帧成功读到的配置([M] 独占,持 lifecycleMutex_)。seqlock 撕裂时沿用它,
    // 免得 UI 闪一帧默认值(见 bridgeTickSnapshot)。
    // ctrl 段懒开的退避时刻(见 ensureCtrlOpen:open() 最坏含 500ms sleep,不能每 25Hz 重试)。
    scvb::u64 ctrlOpenRetryAtMs_ = 0;
    static constexpr scvb::u64 kCtrlOpenRetryMs = 1000;

    scvb::CtrlBroadcastSnapshot lastBroadcast_{};
    bool lastBroadcastValid_ = false;
    // 缓存所属的组号。改组/释放后旧组的 label/priority/lead 必须立刻作废,否则 Input 会长期
    // 显示**上一组**的配置。存组号而不是在五处 changeGroup/release 调用点手工复位。
    int lastBroadcastGroup_ = 0;

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

    // PR#51 红旗#1:state abi 拒载标志(setStateInformation 持锁写;T30 桥消息线程读)。
    bool stateAbiMismatch_ = false;
    scvb::u32 stateAbiSeen_ = 0;
    // PR#51 重要#2:拒载更高 abi 后保留的宿主原始字节(getStateInformation 原样回写,持 lifecycleMutex_)。
    std::vector<std::uint8_t> preservedStateBlob_;

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
