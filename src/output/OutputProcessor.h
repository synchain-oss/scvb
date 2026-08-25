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
#include "output/VizPublisher.h"
#include "state/OutputStateCodec.h"
#include "state/SegmentEdit.h"
#include "state/StateCodec.h"

namespace scvb::output
{
class OutputEditor; // 桥编辑器(T29),createEditor 实例化。
}

// Output 桥面的运行时 state(T29;消息线程独占 —— [M] 写 / OutputEditor::emitTick 读)。
// 与 CFGS(OutputStateCodec)承载的持久化子集互补:此处字段是「桥面可写、未必持久化」的运行时态,
// 持久化扩展归后续任务;CRVS 段真身单独以 crvsData_ 承载(段/pan_curve/版本名)。
struct OutputRuntimeState
{
    // range(§1.8;0=follow 1=daw_loop 2=manual)
    int rangeMode = 0;
    double rangeStartS = 0.0;
    double rangeEndS = 0.0;

    // analysis(§1.18/§1.19/§1.20/§1.21)
    float vadThresholdDb = -45.0f;
    float vadHysteresisDb = 3.0f;
    int vadHangoverMs = 200;
    int vadPaddingPreMs = 120;
    int vadPaddingPostMs = 200;
    juce::String segmentationMode = "valley"; // 02-dsp-spec §362:valley(默认)/ vad_only
    float segmentationSensitivity = 50.0f;
    int segmentationMinSegmentMs = 120;
    float transitionRampMs = 80.0f;
    juce::String loudnessMode = "kw_integrated";
    juce::String centerSlotPolicy = "priority_queue";

    // channels[15](§1.15;index = ch-1)
    struct Channel
    {
        bool enabled = true;
        juce::String label;
        int sourceChannels = 0; // 只读;0=未检测(Input 实测落点,T30/T32 接线)
        bool participateAutoPanSet = false; // false=未显式设置,emit 时按 J60 推导(mono=true/stereo=false)
        bool participateAutoPan = false;
        int priority = 5;
        bool leadLock = false;
        bool leadVolExempt = false;
        int pairId = 0; // 0=无配对,1..7=配对组
    };
    std::array<Channel, 15> channels;

    // ui(active_tab/guide_seen/tour_seen;scale/language 由 Processor 成员承载)
    juce::String activeTab = "master";
    bool guideSeen = false;
    bool tourSeen = false;

    // 运行时态(不入 state chunk、不随工程持久化)
    bool printGuardPending = false;
    bool recaptureArmed = false;
    std::uint16_t recaptureTracksMask = 0;
    double recaptureStartS = 0.0;
    double recaptureEndS = 0.0;
    bool recaptureAutoStop = false;
    bool analysisRunning = false;
    bool analysisHasProgress = false;
    float analysisProgress = 0.0f;

    // config_seq(§2.1 顶层;ctrl 广播区整体版本号,任一字段变化 +1)
    std::uint32_t configSeq = 0;
};

// ScvbOutputAudioProcessor —— Output 插件处理器(01 §5.2 伪代码全实现,T24)。
// 读 15 环 → covered 判定/读中换代弃用/失准计数(ShmRingMixSource)→ 取值仲裁(OutputAuthority/
// DspArbiter)→ gain/pan(mono equal-power / stereo dual-pan+width,[J57])→ 求和 → ms_balance
// ([J58] 总线 M/S)→ busXfade 替换总线([J32] 200ms 注入延迟 + 80ms per-channel 淡入)。
// T29:持有 CRVS 段真身(crvsData_)+ OutputRuntimeState,提供桥编辑器(OutputEditor)入口。
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

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

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
    int versionActive() const { return versionActive_; }
    bool captureEnabled() const { return captureEnabled_; }
    bool outputEnabled() const { return outputEnabled_; }
    double sampleRate() const { return sampleRate_.load(std::memory_order_relaxed); } // 原子读(PR#55 第9轮)
    int uiScalePercent() const { return uiScale_; }
    juce::String uiLanguage() const { return uiLanguage_; }
    // [J75] T43:写 state ui.master_chart_mode(随工程持久化);"trajectory" 以外一律回落 "distribution"。
    void setMasterChartMode(const juce::String& mode);
    juce::String masterChartMode() const { return masterChartMode_; }
    // 只读观察态(O3:同组已有主 Output);写函数据此回 {observer:true}。
    bool isReadOnly() const { return session_.state() == scvb::output::OutputClaimState::kObserver; }
    // 是否已 prepare(sampleRate_>0);触 rebuild 的写入口据此回 badArg(PR#55 第7轮缺陷2)。
    bool isPrepared() const { return sampleRate_.load(std::memory_order_relaxed) > 0.0; }
    // CRVS 修订号:setStateInformation 替换 crvsData_ 后 +1;editor 据此重发 scvb.segments(PR#55 第8轮缺陷1)。
    std::uint32_t crvsRevision() const { return crvsRevision_.load(std::memory_order_acquire); }
    // [M] 该轨累计失准计数(gapCount;scvb.conn.channels[].misalignCount 数据源)。
    scvb::u32 gapCount(int channel) const { return session_.gapCount(static_cast<scvb::u32>(channel)); }

    // PR#53 R1:setStateInformation 读到 abi > kCurrentAbi → 拒载并置位(冻结契约 §7.3:高版本拒载
    // + 提示升级,绝不静默降级;原 blob 由宿主工程保有)。消息线程读写(setStateInformation 持
    // lifecycleMutex_),T30 桥经此把 abiMismatch 横幅推给 UI(Input PR#51 红旗#1 同款)。
    bool hasStateAbiMismatch() const noexcept { return stateAbiMismatch_; }
    scvb::u32 stateAbiSeen() const noexcept { return stateAbiSeen_; }

    // ---- T29 桥面入口(消息线程)----
    // 版本层 + 撤销(T18;供测试/后续)。
    scvb::output::OutputAuthority& authority() { return authority_; }

    // 段真身只读快照(持 lifecycleMutex_;供 emitTick 构建 scvb.state/scvb.segments,避免与宿主
    // prepareToPlay/setStateInformation 的 CRVS 写竞争 —— PR#55 重要1)。
    scvb::state::CrvsData crvsSnapshot();

    // 运行时 state(消息线程独占;仅桥 native function 写 / emitTick 读,宿主不触,无需锁)。
    OutputRuntimeState& runtime() { return runtime_; }
    const OutputRuntimeState& runtime() const { return runtime_; }

    // [J70] 跨组只读探测(u8 位图,bit0=组A…bit7=组H;探测失败=0 位,不弹错、不重试)。
    std::uint8_t probeGroupsOnline();

    // 音频线程 playhead 快照(SPSC,供 scvb.playhead;避免消息线程直读宿主 AudioPlayHead)。
    scvb::engine::PlayheadPod playheadSnapshot() const;

    // ---- CRVS 写事务(全部持 lifecycleMutex_,与 prepareToPlay/setStateInformation 同锁纪律)----
    scvb::engine::SetNameResult setVersionName(int version, const juce::String& name, juce::String& effectiveOut);
    scvb::engine::CopyVersionResult copyVersion(int src, int dst);
    scvb::state::SegmentEditResult editSegment(int track, const scvb::state::SegmentEditArgs& args);
    // 成功返回 true;replacedSegments/replacedLocked = 替换前的段数 / 锁定段数(供确认条计数)。
    bool setTrackManual(int ch, bool isPan, float value, int& replacedSegments, int& replacedLocked);
    void setPanCurve(int version, const std::vector<scvb::PanCurvePoint>& points);
    // 设置过渡 ramp(ms):值变化才重建全部曲线并重新发布(transitionRampSec 烘焙进 CurveEvaluator)。
    // 返回 true = 值已变化并重建(PR#55 第5轮缺陷2)。
    bool setTransitionRamp(float ms);
    bool undo();
    bool redo();

private:
    // 25Hz [M] 定时器:心跳(4Hz 折半)+ session tick(per-channel 判定/看门狗/全局小节)。
    void timerCallback() override;

    // [A] 从 playhead 构造 C8 快照并发布(§5.2 / C8)。
    void publishPlayhead(const juce::AudioPlayHead::PositionInfo& pos, bool haveTime, bool playing);

    // [M] 按 claim 态裁决 viz 段的建/释放:唯一写方 = 本组 claim 到 OutputSlot 的那一个
    // ([J66] 同组内只读观察)。**每拍都做** —— claim 态会在接管/让位/改组时翻转。
    void syncVizSegment();
    // [M] 组装 viz 发布输入并交给 vizPublisher_(内部 4Hz 分频)。调用方须已持 lifecycleMutex_。
    void publishVizFrame(std::uint64_t nowMs);

    // [A] 读全局三件 raw(host 恒权威,不参与仲裁)。
    float readGlobalWidth() const noexcept;
    float readMsBalance() const noexcept;

    // [A] 按时间线读 15 环做 unity 求和(§5.4 bypass 语义)。
    void renderBypassedUnity(juce::AudioBuffer<float>& buffer, int n, int64_t t0);

    // [M] 版本切换/接线:authority 重绑活动版本 + 打印器重绑车道(曲线真身)。
    void rebindVersion();

    // 由 CRVS 段真身重建全部 30 轨 CurveEvaluator 并注入 authority + 打印器重取活动版本曲线。
    // 不可变契约(ADR-005);非锁定 —— 调用方须已持 lifecycleMutex_(rebindVersion 与 CRVS 写事务)。
    void rebuildAllCurves();

    juce::CriticalSection lifecycleMutex_; // 串行化 prepare/release/setState/claim/心跳

    // IPC(段操作持 lifecycleMutex_ 于非实时线程;音频线程只经 session_ 拿裸指针做原子读写)。
    scvb::SegmentBackendWin32 backend_;
    scvb::output::OutputSession session_;
    // [T44/J75] viz 段发布器(Monitor 只读数据面)。只在 [M] 触碰;processBlock 对 viz 段零写入。
    scvb::output::VizPublisher vizPublisher_;

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
    juce::String masterChartMode_ = "distribution"; // [J75] T43(ui.master_chart_mode)

    // T29:桥面运行时 state + CRVS 段真身(消息线程独占)。
    OutputRuntimeState runtime_;
    scvb::state::CrvsData crvsData_;

    // PR#53 R1:state abi 拒载标志 + 保留的宿主原始字节 + 上次成功加载的容器(未知 chunk 原样回写)。
    bool stateAbiMismatch_ = false;
    scvb::u32 stateAbiSeen_ = 0;
    std::vector<std::uint8_t> preservedStateBlob_; // 拒载更高 abi 后保留的宿主原始字节(getStateInformation 原样回写)
    scvb::state::StateChunks loadedChunks_; // 上次成功加载的容器(FEAT/CRVS/未知 fourcc 原样回写,T19 纪律)

    bool prepared_ = false;
    // 跨线程读写(宿主 prepareToPlay/音频线程写 vs editor emitTick/消息线程读)→ 必须原子(PR#55 第9轮)。
    std::atomic<double> sampleRate_{0.0}; // 0 = 未 prepare(宿主 prepareToPlay 前),防御零除(PR#55 第7轮)
    static_assert(std::atomic<double>::is_always_lock_free, "sampleRate_ 必须 lock-free(§8 实时线程纪律)");
    int preparedMaxBlock_ = 512;

    // 音频线程时间线状态(§5.2 步骤 2)。
    int64_t lastT0Out_ = std::numeric_limits<int64_t>::lowest();
    int64_t expectedNextOut_ = std::numeric_limits<int64_t>::lowest();
    uint32_t podEpoch_ = 0;
    std::atomic<uint64_t> timelineInvalidBlocks_{0}; // [A] 无时间线计数 / [M] 健康前置
    std::atomic<uint32_t> timelineValid_{1}; // [A] 本块时间线有效标志(负 t0 视为有效,[J51])
    std::atomic<uint32_t> crvsRevision_{0}; // CRVS 替换修订号([M] 写 / emitTick 读;PR#55 第8轮缺陷1)

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