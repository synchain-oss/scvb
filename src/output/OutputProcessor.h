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
#include <filesystem>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "OutputAuthority.h"
#include "OutputParams.h"
#include "dsp/ParamSmoother.h"
#include "engine/PlayheadShot.h"
#include "AutomationPrinter.h"
#include "ipc/SegmentBackendWin32.h"
#include "analysis/AnalysisPipeline.h"
#include "output/BusXfade.h"
#include "output/MeterShot.h"
#include "output/OutputSession.h"
#include "output/VizPublisher.h"
#include "state/OutputStateCodec.h"
#include "state/SegmentEdit.h"
#include "state/StateCodec.h"

namespace scvb::output
{
class OutputEditor; // 桥编辑器(T29),createEditor 实例化。
}

// Output 桥面的运行时 state(T29;除下面标注的两个首启已读位外,**消息线程独占** ——
// [M] 写 / OutputEditor::emitTick 读)。
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
        int sourceChannels = 0; // 只读;0=未检测(每拍由 refreshSourceChannels 从音频环段头回填)
        bool participateAutoPanSet = false; // false=未显式设置 → participatesInAutoPan() 一律 true([J83])
        bool participateAutoPan = false;
        int priority = 5;
        bool leadLock = false;
        bool leadVolExempt = false;
        int pairId = 0; // 0=无配对,1..7=配对组

        // 参与自动 pan 的取值口径(三处消费方 —— 广播区 / §2.1 快照 / 分析流水线 —— 同源)。
        //
        // **未显式设置时一律参与**,不再按源声道推导。[J60] 原本写的是「mono 默认参与 /
        // stereo 默认不参与」,理由是「立体声源自带声像,自动 pan 会把它压塌」。但检测值来自
        // `getMainBusNumInputChannels()`,那是**轨道总线布局**,不是素材本身是不是立体声 ——
        // Cubase 里一条单声道人声放在立体声轨上就报 2。于是接上检测之后,真机上 15 条人声轨
        // 里绝大多数被判成「不参与」,而 AutoAssign 对不参与的轨按「保持现值」处理,现值 = 从未
        // 被写过的 pan 参数 = 0 —— 分析把 0 烘焙进段表,打印器再把 0 写进自动化:
        // 「大部分轨回到中间、只剩两条(恰好是 mono 轨)在左边」(v5.1 实测 P0-B)。
        // 这比 v5 的「全居中」更隐蔽:它一半有效,看起来像分配算法本身不平衡。
        //
        // 真正该由用户决定的是「这条轨要不要参与」,轨道页每轨都有那个开关;检测值继续服务
        // 它该服务的地方(分布图的 ST 角标与张开线、viz 的 stereoMask、dual-pan 解码)。
        // 本行的默认档由 **[J83]** 裁决(取代 [J60] 的按源声道推导);变更文档见
        // docs/contract-changes/20260826-j83-participate-default.md。
        bool participatesInAutoPan() const { return participateAutoPanSet ? participateAutoPan : true; }
    };
    std::array<Channel, 15> channels;

    // ui(active_tab/guide_seen/tour_seen;scale/language 由 Processor 成员承载)
    juce::String activeTab = "master";
    // 首启已读位是本结构里**唯一跨线程**的两个字段:自 T37 起它们随 PRMS 持久化,
    // 于是宿主线程的 setStateInformation 会写、消息线程 25Hz 的 buildStateSubtree 会读。
    // 用 atomic 而不是让读方去抢 lifecycleMutex_ —— 25Hz 的 emit 路径不该为两个 bool
    // 跟宿主的 prepare/setState 抢锁。写方仍走 bridgeSetGuideSeen/bridgeSetTourSeen。
    std::atomic<bool> guideSeen{false};
    std::atomic<bool> tourSeen{false};
    // 用户显式选过语言(§1.30 setLang 被调用过)。与上面两位同机制随 PRMS 持久化。
    std::atomic<bool> langChosen{false};

    // 运行时态(不入 state chunk、不随工程持久化)
    bool printGuardPending = false;
    bool recaptureArmed = false;
    std::uint16_t recaptureTracksMask = 0;
    double recaptureStartS = 0.0;
    double recaptureEndS = 0.0;
    bool recaptureAutoStop = false;
    // [J87] 布防时是不是**由我们**替用户打开的 01 采集(裁定①)。撤防时只有这一位为真才把
    // 采集关回去(裁定③「恢复布防前的原值」)—— 布防前本来就开着的,撤防后必须保持开。
    bool recaptureAutoEnabledCapture = false;
    // [J87] 上一拍的播放头位置(秒),用于「越过选区右边界」的**边沿**判定(裁定③)。
    // 用边沿而不是电平:布防时播放头若已在选区右侧,电平判定会当场自撤防。<0 = 尚无上一拍。
    double recapturePrevPlayheadS = -1.0;
    bool analysisRunning = false;
    bool analysisHasProgress = false;
    // [W] 分析线程写 / [M] 25Hz emit 读 —— runtime_ 其余字段都由 lifecycleMutex_ 串行,
    // 只有这一条是跨线程的,必须 atomic(裸 float 在严格内存模型下是 UB)。
    std::atomic<float> analysisProgress{0.0f};

    // config_seq(§2.1 顶层;ctrl 广播区整体版本号,任一字段变化 +1)
    std::uint32_t configSeq = 0;
};

// ScvbOutputAudioProcessor —— Output 插件处理器(01 §5.2 伪代码全实现,T24)。
// 读 15 环 → covered 判定/读中换代弃用/失准计数(ShmRingMixSource)→ 取值仲裁(OutputAuthority/
// DspArbiter)→ gain/pan(mono equal-power / stereo dual-pan+width,[J57])→ 求和 → ms_balance
// ([J58] 总线 M/S)→ busXfade 替换总线([J32] 200ms 注入延迟 + 80ms per-channel 淡入)。
// T29:持有 CRVS 段真身(crvsData_)+ OutputRuntimeState,提供桥编辑器(OutputEditor)入口。
class ScvbOutputAudioProcessor final : public juce::AudioProcessor, private juce::Timer, private juce::AsyncUpdater
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

    // [J87] 局部重采集布防(04 §4.2;桥面 §1.23 recaptureArm 的落地方)。两个口都在 [M]。
    // 放在 processor 而不是 editor 里,是因为**撤防有两条触发路径**:桥面显式撤防,与 25Hz
    // tick 里「播放头越过选区右边界」的自动撤防(裁定③)。两条必须走同一段「关采集/恢复原值」
    // 的代码,分头写迟早会漂。armRecapture 只在 false→true 那一跳记「是不是我们开的采集」——
    // 中途改选区/改轨勾选会再次调用它(04 §4.2 ②「立即以新值为布防范围」),那时不能重记,
    // 否则布防前的原值就被现在这个(已被我们改成 true 的)值冲掉,撤防后再也关不回去。
    void armRecapture(std::uint16_t tracksMask, double startS, double endS, bool autoStop);
    void disarmRecapture();
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
    // [SL-215] 会话 GUID(36 字符 dashed UUID,恒非全零)。桥面 §1.1 快照的 session_guid 取这里。
    juce::String sessionGuid() const { return sessionGuid_; }
    // [SL-215/SL-226] 设置页存储状态行的两个真源:特征落在哪(内嵌/外部)+ 实际多少字节。
    // 两个必须成对读 —— 只看字节数分不出「内嵌 0.5MB」与「转出后引用节 0.5MB」。
    std::int64_t featureBytes() const;
    bool featuresInSidecar() const;
    // 桥面 ui 落 state(§1.30 setLang / §1.29 commitUiScale)。基类 WebViewHost 只维护 editor
    // 局部值,而 §2.1 的 ui.language / ui.scale 取自这里 —— 不落 processor,下一次 state emit
    // 会把旧值回推给 UI(T37 真机 bug A-1:选中文后切 tab 变回英文)。
    void bridgeSetUiLanguage(const juce::String& lang); // 已由桥层 normalize({zh,en,fr})
    void bridgeSetUiScalePercent(int percent); // clamp [MinUiScale, MaxUiScale] × 100
    // 首启已读位(§1.32/§1.33)。**必须走这两个口而不是直接写 runtime()**:两位自 T37 起
    // 随 PRMS 持久化,读方 getStateInformation 持 lifecycleMutex_ 且可能不在消息线程 ——
    // 写方不持同一把锁就等于没有锁。runtime_ 的其余字段仍是消息线程独占,不受此约束。
    void bridgeSetGuideSeen(bool seen);
    void bridgeSetTourSeen(bool seen);
    // 只读观察态(O3:同组已有主 Output);写函数据此回 {observer:true}。
    bool isReadOnly() const { return session_.state() == scvb::output::OutputClaimState::kObserver; }
    // 是否已 prepare(sampleRate_>0);触 rebuild 的写入口据此回 badArg(PR#55 第7轮缺陷2)。
    bool isPrepared() const { return sampleRate_.load(std::memory_order_relaxed) > 0.0; }
    // CRVS 修订号:setStateInformation 替换 crvsData_ 后 +1;editor 据此重发 scvb.segments(PR#55 第8轮缺陷1)。
    std::uint32_t crvsRevision() const { return crvsRevision_.load(std::memory_order_acquire); }
    // [M] 该轨累计失准计数(gapCount;ctrl 全局小节 + Tab4 诊断,进程寿命只增)。
    scvb::u32 gapCount(int channel) const { return session_.gapCount(static_cast<scvb::u32>(channel)); }
    // [M] 该轨**本次失准发作**的缺口数(scvb.conn.channels[].misalignCount 数据源)。恢复健康
    // 满 1s 即归零 —— 累计值上桥会把「路由失准」横幅永久钉死(T37 三轮 A 族)。
    scvb::u32 misalignCount(int channel) const { return session_.misalignCountRecent(static_cast<scvb::u32>(channel)); }
    // [M] 该轨采集数据是否已过期(§2.8 segments.channels[].stale 数据源;04 §4.5 fingerprint
    // watchdog)。判定由 session_ 在 tick() 的命令环消费里推进 —— 与本读点同在消息线程,串行。
    bool captureStale(int channel) const { return session_.channelStale(static_cast<scvb::u32>(channel)); }

    // scvb.conn(契约 §2.3)的整帧数据面快照。T29 桥曾以「claim 态推导」的占位值充数
    // (全轨 slotState=2、heartbeatFresh 恒 false),UI 的 `slotState=2 ∧ heartbeatFresh`
    // 口径下连接数恒为 0 —— 音频照常出声但 Tab2 永远显示「组 X 尚无输入」(T37 真机 bug B)。
    // 持 lifecycleMutex_:registry 段的映射/解映射走 prepareToPlay/changeGroup,不能与之竞争。
    struct ConnSnapshot
    {
        std::array<scvb::output::ChannelConnInfo, 15> channels{};
        std::uint32_t generation = 0;
        bool readOnly = false;
    };
    ConnSnapshot connSnapshot();

    // PR#53 R1:setStateInformation 读到 abi > kCurrentAbi → 拒载并置位(冻结契约 §7.3:高版本拒载
    // + 提示升级,绝不静默降级;原 blob 由宿主工程保有)。消息线程读写(setStateInformation 持
    // lifecycleMutex_),T30 桥经此把 abiMismatch 横幅推给 UI(Input PR#51 红旗#1 同款)。
    bool hasStateAbiMismatch() const noexcept { return stateAbiMismatch_; }
    // [SL-217] 最近一次 setStateInformation 是否**没能恢复段真身**(缺 CRVS chunk 或解码失败)。
    // 此时段表被**保留**而不是清空(§7.3 不得静默丢数据),这一位供诊断与后续上桥告警使用。
    bool hasCrvsNotRestored() const noexcept { return crvsNotRestored_; }
    scvb::u32 stateAbiSeen() const noexcept { return stateAbiSeen_; }

    // ---- T29 桥面入口(消息线程)----
    // 版本层 + 撤销(T18;供测试/后续)。
    scvb::output::OutputAuthority& authority() { return authority_; }

    // 段真身只读快照(持 lifecycleMutex_;供 emitTick 构建 scvb.state/scvb.segments,避免与宿主
    // prepareToPlay/setStateInformation 的 CRVS 写竞争 —— PR#55 重要1)。
    scvb::state::CrvsData crvsSnapshot();

    // [J69/U24] analysis.loudness_mode/center_slot_policy 快照(持 lifecycleMutex_;与 setAnalysisConfig /
    // getStateInformation 同锁读,消除 emitState 无锁读 runtime_ 的剩余竞态 —— 复评重要②)。
    std::pair<juce::String, juce::String> analysisConfigSnapshot();

    // 运行时 state(消息线程独占;仅桥 native function 写 / emitTick 读,宿主不触,无需锁)。
    // 例外:loudnessMode/centerSlotPolicy 由 setAnalysisConfig 持锁写、getStateInformation 持锁读,
    // emitState 必须经 analysisConfigSnapshot() 持锁读 —— 其余字段仍消息线程独占。
    OutputRuntimeState& runtime() { return runtime_; }
    const OutputRuntimeState& runtime() const { return runtime_; }

    // [J70] 跨组只读探测(u8 位图,bit0=组A…bit7=组H;探测失败=0 位,不弹错、不重试)。
    std::uint8_t probeGroupsOnline();

    // 音频线程 playhead 快照(SPSC,供 scvb.playhead;避免消息线程直读宿主 AudioPlayHead)。
    scvb::engine::PlayheadPod playheadSnapshot() const;

    // 音频线程电平快照(SPSC,供 scvb.meters;线性幅度,dB 换算在 emitMeters)。
    scvb::output::MeterPod meterSnapshot() const;

    // [M] 某轨在 [startS,endS) 内的采集覆盖(§2.7 scvb.captureProgress 数据面)。
    // 数据源 = OutputSession 的 FrameStore(Input 写 feat 段 → Output 25Hz 增量拉取)。
    // 持 lifecycleMutex_:FrameStore 由 timerCallback 的 session_.tick 写,与本读点串行。
    struct CoverageInfo
    {
        float pct = 0.0f; // 0..100(契约 §2.7 coveragePct 就是百分数,不是 0..1)
        double coveredS = 0.0;
        std::vector<scvb::analysis::HopRange> ranges; // 覆盖区间(hop 域;秒换算用 featHopSeconds)
    };
    CoverageInfo coverageOf(int channel, double startS, double endS);
    // hop → 秒的换算系数(feat 段几何常量,不是采样率派生量)。
    static double featHopSeconds();

    // [M] 某轨在 [startS,endS) 内的**波形瓦片**(§1.27 requestWaveform 的数据面)。
    // 每列聚合该列覆盖到的所有 hop:maxDb 取 peak 的最大值、minDb 取 K 加权的最小值、
    // vad 取该列是否有 vadP>127 的 hop。未覆盖列 covered=0 且 min/max 留 -160 哨兵
    // (泳道据此画斜纹)。持 lifecycleMutex_:与写 FrameStore 的 timerCallback 串行。
    // 单列最多采样多少个 hop。概览块(512 列跨全曲)是缩略图,抽样对观感无损;
    // 有了它,waveformOf 的代价与 cols 同阶,而不是与「请求跨度」同阶(P0-A 活锁的止血点)。
    static constexpr std::uint64_t kMaxHopsPerCol = 4096;
    // 单次 requestWaveform 允许的最大时间跨度(秒)。24h 远超任何真实工程,
    // 只用来挡住被污染的时长(真机上出现过 2^40 采样 ÷ 48k ≈ 265 天)。
    static constexpr double kMaxRequestSpanS = 24.0 * 60.0 * 60.0;

    struct WaveformTile
    {
        std::vector<double> minDb;
        std::vector<double> maxDb;
        std::vector<int> vad;
        std::vector<int> covered;
    };
    WaveformTile waveformOf(int channel, double startS, double endS, int cols);

    // [M] 已采集内容的时间线右端(秒)= 全轨 coverage 的最大终点;无采集数据回 0。
    // follow 档下「分析全部」的终点取它,而不是当前播放头 —— 见 parseAnalyzeScope 的头注。
    double capturedExtentSeconds();
    // [M] 清除选中轨 × 区间的采集覆盖(§1.24 clearCoverage:打洞,页数据留待后续覆盖)。
    // 返回实际清除的总时长秒数(各轨相加,供 UI 反馈)。
    double clearCoverage(std::uint16_t tracksMask, double startS, double endS);

    // ---- 分析作业(§1.6/§1.7)------------------------------------------------------
    // 长耗时分析在**后台线程**跑(契约 §1.6「绝不阻塞消息线程」):startAnalysis 只做取样 +
    // 起线程并立即返回;进度经 runtime_.analysisProgress 上桥;完成后回到消息线程写 CRVS。
    struct AnalyzeAccepted
    {
        bool ok = false;
        // 桥面只允许 §5.6 八值闭集里的 reason,而 §7 manifest 给 analyze 只登记了 "busy"。
        // 「范围∩覆盖=∅」按 §1.6 拒绝态行回 {ok:false, affected:{0,0,0}}(**不带 reason**),
        // 所以这里不再自造 "noData"/"notPrepared" 字符串,只留一个 busy 布尔。
        bool busy = false;
        int intervals = 0; // 影响面预估(受理回执用)
        int tracks = 0;
        int manualKept = 0;
    };
    // tracksMask=0 表示不限轨;[startS,endS) 为分析范围(follow 档由调用方折算)。
    // clearManual(§1.6 opts):true = 「重新识别(含手动段)」,连用户段一并重算;
    // false(默认)= ADR-008 语义,用户段一律保留。
    AnalyzeAccepted startAnalysis(std::uint16_t tracksMask, double startS, double endS, bool clearManual = false);
    void cancelAnalysis();
    bool analysisRunning() const { return analysisRunning_.load(std::memory_order_acquire); }
    // [M] 取走「分析刚完成」标记(取走即清)。editor 据此把段表以 reason:"analyze" 发出。
    bool takeAnalysisDone()
    {
        const bool v = analysisDone_;
        analysisDone_ = false;
        return v;
    }
    // 干跑影响面(§1.5 previewAnalyze):不改任何数据,只数「范围 × 有覆盖的轨」。
    AnalyzeAccepted previewAnalysis(std::uint16_t tracksMask, double startS, double endS);

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
    // [J69/U24] 设置 analysis.loudness_mode / center_slot_policy(持 lifecycleMutex_,与 getStateInformation
    // 同锁读,消除 juce::String COW 跨线程竞态 —— 复评重要①)。hasLoudness/hasCenter = patch 是否含该字段;
    // 传入值已由桥层白名单校验。变化才 bump configSeq,返回是否变化。
    bool setAnalysisConfig(const juce::String& loudnessMode, const juce::String& centerSlotPolicy, bool hasLoudness,
                           bool hasCenter);
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

    // [A] 本块电平测量并发布(§2.5 数据面)。hasData/nch 为本块读环结果,trackGain 为本块
    // 起点仲裁目标导出的线性增益;busL/busR = 求和后的总线缓冲(nullptr = 本块未混音)。
    void publishMeters(const std::array<bool, 15>& hasData, const std::array<scvb::u32, 15>& nch,
                       const std::array<float, 15>& trackGain, const float* busL, const float* busR, int n) noexcept;
    // [A] 全部归零发布(直通/观察/无注入轨路径:电平表落回地板,不冻在上一块的值)。
    void publishSilentMeters() noexcept;

    // [M] 版本切换/接线:authority 重绑活动版本 + 打印器重绑车道(曲线真身)。
    void rebindVersion();

    // [M] 命令环收到的远程优先级落 runtime state(§3.4);有变化返回 true(调用方 bump config_seq)。
    bool applyRemotePriorities();

    // [M] 非阻塞回收退休的分析作业(每拍一次;线程还在跑就留到下一拍)。
    void reapRetiredJobs();
    // [M] **阻塞**回收:只在析构调用 —— 见析构里的行注(R5 的不变式全靠它)。
    void joinRetiredJobs();

    // [M] 音频环段头 channels(Input 的 prepareToPlay 写定,[J57])→ runtime.channels[].sourceChannels。
    // 有变化返回 true(调用方 bump config_seq,让广播区/UI 一起跟上)。
    bool refreshSourceChannels();

    // 后台分析线程体 + 完成回落(见 startAnalysis)。
    class AnalysisJob;
    friend class AnalysisJob;
    void finishAnalysis(scvb::analysis::PipelineResult result, std::int64_t rangeStartSample,
                        std::int64_t rangeEndSample, bool clearManual);
    // 线程 → 消息线程的交接:AsyncUpdater 而不是裸 callAsync(见 handleAsyncUpdate 头注)。
    void handleAsyncUpdate() override;
    // [M] 把 runtime 配置镜像进 ctrl 广播区(§4.3);config_seq 未变则不写。
    void publishConfigBroadcast();

    // 由 CRVS 段真身重建全部 30 轨 CurveEvaluator 并注入 authority + 打印器重取活动版本曲线。
    // 不可变契约(ADR-005);非锁定 —— 调用方须已持 lifecycleMutex_(rebindVersion 与 CRVS 写事务)。
    void rebuildAllCurves();

    // [SL-226] 特征持久化两端。调用方须已持 lifecycleMutex_(与 get/setStateInformation 同锁)。
    // writeFeaturesChunk:FrameStore → FEAT chunk;超 ADR-007 阈值转 sidecar(写引用节)。
    // readFeaturesChunk:FEAT chunk → FrameStore(embedded 直解;引用节经 sessionGuid 读 sidecar)。
    void writeFeaturesChunk(scvb::state::StateChunks& chunks);
    void readFeaturesChunk(const scvb::state::StateChunks& chunks);
    // sidecar 落盘根目录 = <appdata>/Synchain/SCVB(与 UiDefaultsStore 同根,STATE_SCHEMA §4.3)。
    static std::filesystem::path sidecarBaseDir();
    // 特征的 hop 时基是否与本构建一致(当前冻结 10ms,恒真;为将来放开 hop 预留的闸)。
    static bool featureHopMatchesBuild(std::uint32_t hopMs);

    // [J87] 采集开关的**不加锁**内核:调用方须已持 lifecycleMutex_。setCaptureEnabled 是它的
    // 加锁外壳;25Hz tick 全程持锁,自动撤防那一路直接用内核,不去依赖 CriticalSection 的可重入。
    void applyCaptureEnabled(bool on);
    // [J87] 撤防的**不加锁**内核:桥面撤防与「越界自动撤防」共用它,两条路不许分头写。
    void disarmRecaptureLocked();
    // [J87] 把记账门控(时间维 gate + 轨维 mask)按当前布防态套到 session 上。
    // arm/disarm 与 25Hz tick 三处都调:桥面调用与 tick 不同拍,只在 tick 里设会留一个 40ms
    // 的窗口,那段时间仍按 global.range 记账 —— 选区外的既有特征会在那一小段里被盖掉。
    void applyFeatureGates();
    // [J87] 25Hz tick 内的布防维护:套门控 + 「越过选区右边界」自动撤防。
    void tickRecapture();

    juce::CriticalSection lifecycleMutex_; // 串行化 prepare/release/setState/claim/心跳

    // IPC(段操作持 lifecycleMutex_ 于非实时线程;音频线程只经 session_ 拿裸指针做原子读写)。
    scvb::SegmentBackendWin32 backend_;
    scvb::output::OutputSession session_;
    // [T44/J75] viz 段发布器(Monitor 只读数据面)。只在 [M] 触碰;processBlock 对 viz 段零写入。
    scvb::output::VizPublisher vizPublisher_;
    // [SL-192] viz 发布的**独立定时器**,与主 25Hz [M] tick 分开。
    //
    // 为什么不把主 tick 提到 30Hz:那条 tick 上挂着心跳、session tick、看门狗、配置广播、
    // 打印区间等一整串东西,提频等于给它们全体加 20% 的调用次数 —— 本卡只需要 viz 一件更快。
    // 为什么不把 viz 留在主 tick 上:25Hz < 30Hz,发布率会被 tick 直接卡死在 25Hz。
    //
    // 频率取 **60Hz 而不是 30Hz**:发布闸门是 `kPublishIntervalMs`(33ms),驱动若也是 30Hz,
    // 两者同频不同相 —— 定时器抖动让某拍差 1ms 没够着闸门,那一帧就整个丢掉,实得频率掉到
    // 30Hz 以下还发抖。**这正是本卡在读方那两级栽过的同一个坑**(见 MonitorProcessor.cpp
    // 的 kVizPollHz 注释),不能在写方这边再犯一次。60Hz 驱动 33ms 闸门 = 稳定每两拍一帧 = 30.0Hz。
    // 未到闸门的那一拍在 `publishVizFrame` 的 `due()` 处早退:不采输入、不构造任何东西。
    std::unique_ptr<juce::TimedCallback> vizTimer_;

    // 取值仲裁 + 平滑(T16/DspArbiter + T18 版本层)。
    scvb::output::OutputAuthority authority_;
    // 打印器(T17;本卡接线 setShot/setCurves/bindVersion/startPrinting/hostEchoShield)。
    scvb::output::AutomationPrinter printer_;
    // 打印区间缓存([M] 独占):段表(crvsRevision)变了才重算,不在 25Hz 里逐拍扫 15 轨段表。
    std::uint32_t lastPrintRangeRevision_ = 0xFFFFFFFFu; // 首拍必算
    bool printRangeValid_ = false;
    double printRangeStartS_ = 0.0;
    double printRangeEndS_ = 0.0;
    // 总线交叉淡变 + per-channel 淡入(§5.2 过渡语义 R2)。
    scvb::output::BusXfade busXfade_;

    // C8 playhead 快照(音频线程写 / 打印器消息线程读)。
    scvb::engine::PlayheadShot playheadShot_;
    // 电平快照(音频线程写 / 桥 emitMeters 消息线程读)。
    scvb::output::MeterShot meterShot_;

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
    // [SL-215] 会话 GUID:构造时生成一次,setStateInformation 读到工程里存过的合法值就改用那个。
    // 同一工程反复开 → 恒是同一串(sidecar 文件名才稳定);全新实例 → 各自唯一。
    // 存取口径与相邻的 uiLanguage_ / masterChartMode_ 逐字相同(同样由 setStateInformation 写、
    // 桥面按值读),不另立一套同步纪律。
    juce::String sessionGuid_;

    // T29:桥面运行时 state + CRVS 段真身(消息线程独占)。
    OutputRuntimeState runtime_;
    scvb::state::CrvsData crvsData_;

    // PR#53 R1:state abi 拒载标志 + 保留的宿主原始字节 + 上次成功加载的容器(未知 chunk 原样回写)。
    bool stateAbiMismatch_ = false;
    bool crvsNotRestored_ = false; // [SL-217] 最近一次加载没恢复段真身(段表已保留,不清空)
    scvb::u32 stateAbiSeen_ = 0;
    std::vector<std::uint8_t> preservedStateBlob_; // 拒载更高 abi 后保留的宿主原始字节(getStateInformation 原样回写)
    scvb::state::StateChunks loadedChunks_; // 上次成功加载的容器(FEAT/CRVS/未知 fourcc 原样回写,T19 纪律)
    std::vector<std::uint8_t>
        preservedCfgsTail_; // CFGS 已知字段之后的未知尾部(未来小版本追加;save 原样回写,防静默丢字段)

    // [SL-226] 特征持久化的两位运行时态。
    // featuresSidecar_:上一次落盘是否走了 sidecar —— `shouldUseSidecar` 的回滞(>8MB 转出 /
    // <6MB 收回)需要「当前在哪一侧」才判得了,只看本次字节数会在阈值附近来回抖。
    bool featuresSidecar_ = false;
    // featCodecNewer_:读到 codecVer 高于本构建的 FEAT。此时特征按空处理,但**绝不重编码** ——
    // 保存时原样回写 loadedChunks_ 里那份原始 chunk(与容器级 abi 拒载同一条纪律:
    // 不认识的数据只能原样带走,不能用「我这边是空的」去覆盖用户的真数据)。
    bool featCodecNewer_ = false;
    // featRefUnresolved_:工程里有 FEAT 引用节,但外部 sidecar 读不出来(文件不在 / sha256 不符)。
    // 此时内存里没有特征,而保存路径的「一轨都没采过 → 删掉 FEAT chunk」会把**指针本身**也删掉 ——
    // 文件还躺在磁盘上,工程里却再没有找回它的线索。置位后保存改为原样保留那一节。
    bool featRefUnresolved_ = false;
    // 特征实际字节数(压缩后)。内嵌 = FEAT chunk 大小;转出 = sidecar 文件大小(**不是**引用节的
    // 一百来字节)。设置页存储状态行的分子;仅在 load/save 时更新,故「本会话新采集但尚未存盘」
    // 恒为 0 —— 与工程文件里的实际字节数一致,不是「内存里有多少」。
    std::int64_t featureBytes_ = 0;
    // 上面两位(featCodecNewer_ / featRefUnresolved_)要「原样带走」的那份**原始 FEAT 字节**。
    // 必须自己留一份,**不能指望 loadedChunks_**:载入一份只带 PRMS 的部分 blob(轨道/参数预设)
    // 时 loadedChunks_ 会被整个换成 {PRMS},而那条路不走 readFeaturesChunk、两位也就不复位 ——
    // 「什么都不做就是原样回写」的前提当场失效,那份不认识的字节永久消失(#147 三轮复审)。
    std::vector<std::uint8_t> preservedFeatChunk_;

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
    std::atomic<uint32_t> crvsRevision_{0}; // CRVS **整体替换**修订号([M] 写 / emitTick 读;PR#55 第8轮缺陷1)
    // 求值曲线修订号:**每次 rebuildAllCurves 都 +1**,涵盖所有段编辑路径(editSegment /
    // setTrackManual / copyVersion / undo·redo / 换版本 / 改 ramp / 分析回落 / 加载工程)。
    //
    // 为什么不复用 crvsRevision_:那一个是「段表被整体换掉了,桥要重发一次 §2.8」的信号,
    // OutputEditor 拿它触发 reason:"snapshot" 全量下发。段编辑路径**已经各自**发过带具体
    // reason 的 §2.8(edit / trackManual / undo / …),再让 crvsRevision_ 跟着动会紧随其后
    // 多发一次 "snapshot" —— 而 web 侧按 reason 分叉:非 trackManual 的回推被当成「失效性回推」,
    // 会连同**排队中的**手动值一起作废(tab-tracks.js onSegments)。那等于把刚修好的
    // 「手动写回不丢」又拆一遍。故分成两个号:替换给桥,求值给引擎侧的两个消费方。
    std::atomic<uint32_t> curvesRevision_{0};
    // 轨启用位图(bit{N-1} = ch N 的 channels[].enabled)。[M] 25Hz 写 / [A] 每块 acquire 读。
    // §1.15 的 enabled 此前**全 Output 侧零消费**:混音不看它、打印器的车道闸(setTrackEnabled,
    // 已实现且有单测)没有任何生产调用点 —— 开关一拧,音频与自动化都毫无反应(v4 实测 P1-5)。
    std::atomic<std::uint32_t> enabledMask_{0x7FFFu};

    // 音频线程零分配缓冲(prepareToPlay 分配)。
    std::vector<float> accumL_;
    std::vector<float> accumR_;
    std::array<std::vector<float>, 15> trackBuf_; // stereo 容量(2 × preparedMaxBlock)
    // [A] bypass 路径的本块读环结果(renderBypassedUnity 写 / processBlockBypassed 读):
    // bypass 期间同样要报电平,否则液柱冻在 bypass 前那一刻(I5)。音频线程独占,不跨线程。
    std::array<bool, 15> bypassHasData_{};
    std::array<scvb::u32, 15> bypassChannels_{};

    // 平滑器([A] 独占):ms_balance g_M/g_S(10ms)、全局 width(10ms)、per-channel 注入 fade(80ms)。
    scvb::dsp::LinearSmoother gMSmoother_{1.0f};
    scvb::dsp::LinearSmoother gSSmoother_{1.0f};
    scvb::dsp::LinearSmoother globalWidthSmoother_{100.0f};
    std::array<scvb::dsp::LinearSmoother, 15> channelFade_;

    // [M] 状态。
    uint64_t lastHeartbeatMs_ = 0;
    int timelineInvalidTicks_ = 0;
    // 分析作业([M] 起/停;线程体只读快照,完成后经 AsyncUpdater 回消息线程写 CRVS)。
    std::unique_ptr<AnalysisJob> analysisJob_;
    // 已退休、但线程可能还没跑完的作业([M] 独占)。
    //
    // 取消/重启分析原先在**消息线程**上 stopThread(2000) 等 join —— 最多把消息泵堵 2 秒,
    // 而它可由 web 的 cancelAnalyze 直接触发。这与 P0-A(消息线程上做可长时间阻塞的事)
    // 同属一类,一并改掉:这里只 signal 不 join,把作业挪进退休区,由 25Hz 的
    // reapRetiredJobs() **非阻塞**地回收(isThreadRunning() 为假才析构)。
    // 代号(analysisGeneration_)已保证退休作业即便跑完也不会碰 CRVS。
    // 析构时仍然 join —— 那是拆机时刻,阻塞是对的,也必须等线程真的停了才放对象。
    std::vector<std::unique_ptr<AnalysisJob>> retiredJobs_;
    std::atomic<bool> analysisRunning_{false};
    // 作业代号:每次 start/cancel 都 +1。线程把自己的代号连同结果放进 pendingResult_,
    // [M] 取件时比对 —— 不匹配即整份丢弃。这道门同时挡住两件事:
    //   ① 「已投递完成消息 → 用户随后取消」的竞态(旧口径会照写 CRVS,与「取消 = 结果整份丢弃」相左);
    //   ② 取消后紧接着重启的新作业,不会被上一份迟到的结果污染。
    std::atomic<std::uint32_t> analysisGeneration_{0};
    // 结果交接槽([W] 写 / [M] 取,pendingMutex_ 串行)。
    juce::CriticalSection pendingMutex_;
    struct PendingAnalysis
    {
        scvb::analysis::PipelineResult result;
        std::int64_t rangeStartSample = 0;
        std::int64_t rangeEndSample = 0;
        std::uint32_t generation = 0;
        bool clearManual = false;
        bool valid = false;
    };
    PendingAnalysis pendingAnalysis_;
    // 分析刚完成([M] 置位 / editor 取走)。§2.8 的 reason 要落 "analyze" —— web 有两处认它:
    // Tab4 的「参数已改、结果陈旧」基线同步,与 Tab3 的分析 diff 摘要条。只 bump crvsRevision_
    // 会让段表以 "snapshot" 发出,那两处静默失效。
    bool analysisDone_ = false;
    // 本次作业是否带 clearManual(§1.6 opts);[M] 写、交接时随结果一起传给 finishAnalysis。
    bool analysisClearManual_ = false;

    // 广播区上次写出的 config_seq(哨兵 = 从未写过,首次 tick 必写一次让 Input 立刻拿到实况)。
    std::uint32_t lastBroadcastConfigSeq_ = 0xFFFFFFFFu;
    // 上次写出的组号。改组后 ctrl 段整个换了一张,旧组的 config_seq 对新组毫无意义 ——
    // 只比 config_seq 会让「换组后新组广播区永不写」(C 族症状在换组路径原样复现)。
    // 存组号而不是在每个 changeGroup 调用点手工复位:调用点会增加,这道判据不会漏。
    int lastBroadcastGroup_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScvbOutputAudioProcessor)
};