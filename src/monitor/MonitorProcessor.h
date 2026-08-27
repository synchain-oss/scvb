// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// windows.h 的 min/max 宏会污染 std::numeric_limits<T>::min() 与 std::max;先禁用再包含
// SegmentBackendWin32.h(其内部 include windows.h 但未定义 NOMINMAX)。与 Input/Output 同款。
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <memory>

#include "engine/PlayheadShot.h"
#include "ipc/SegmentBackendWin32.h"
#include "ipc/VizPlane.h"

// ScvbMonitorAudioProcessor —— SCVB Monitor(T45 / J75)。纯只读监视器,三条铁律:
//
//   1. **0 自动化参数**。没有 APVTS,`getParameters()` 恒为空 —— Output 的 123 参数面
//      (docs/PARAMETERS.md,冻结)一个不动;宿主的 bypass 由 JUCE wrapper 提供,不计在内。
//   2. **音频直通**。`processBlock` 对 buffer 一个样本都不改,输出与输入**逐样本按位相等**
//      (T42 同款旁路口径)。不写延迟、不做增益、不清 buffer。
//   3. **对任何共享段零写入**。registry 只经 GroupProbe 的 `openExistingReadOnly` 探测组在线;
//      viz 段经 `VizPlane::attachReadOnly()`。不 claim InputSlot/OutputSlot、不碰 ctrl 段、
//      不建任何段 —— 对既有注册表/心跳/接管/看门狗机制零改动(01 J75 注记)。
//
// 跨组只读沿用 J70 口径(J66 隔离的是音频/控制域,只读观察不破坏隔离本意)。
// 组不在线 = viz 段不存在 → `attachReadOnly()` 得 kFailed → 空态,由 [M] 4Hz 重试,不崩、不刷屏。
class ScvbMonitorAudioProcessor final : public juce::AudioProcessor, private juce::Timer
{
public:
    ScvbMonitorAudioProcessor();
    ~ScvbMonitorAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    // 直通:buffer 原样返回(逐样本按位相等)。
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "SCVB Monitor"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    // state:只有「看哪一组 + UI 缩放/语言」三项,全部非自动化(与 Input 同款口径,
    // docs/PARAMETERS.md 三「Input 插件:state(无自动化参数)」的镜像)。
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ---- [M] 桥入口 ----

    int groupId() const { return groupId_; }
    // 组选择 A-H(1..8):只读换段,不 claim、不写。返回是否真的换了组。
    // **不叫 setGroupId** —— 契约 §1.4 的那个是 Output 的改组(断连本组全部、要弹确认条),
    // 与「换一个组来看」是两件事,共用名字迟早有人照 §1.4 的语义去实现它(T46 提出,采纳)。
    bool setObservedGroup(int groupId);

    int uiScalePercent() const { return uiScalePercent_; }
    void setUiScalePercent(int percent);
    juce::String uiLanguage() const { return uiLanguage_; }
    void setUiLanguage(const juce::String& lang);

    // 组在线位图(bit0 = 组 A/g1 … bit7 = 组 H/g8),1Hz 刷新;探测失败 = 全 0(空态,不报错)。
    std::uint8_t groupsOnline() const { return groupsOnline_; }

    // viz 段当前状态(空态判定用)。
    enum class VizState
    {
        kOffline, // 段不存在 / 未 attach —— Output 未在该组上线:空态
        kOnline, // 段已 attach 且可读(**不含**「帧还在更新」—— 那是 vizFresh() 的事)
        kAbiMismatch // magic 相符 abi/几何不符 —— 拒连横幅(J40),不半兼容
    };
    VizState vizState() const { return vizState_; }

    // 播放头快照([M] 读):Monitor 与 Output 在同一个宿主里,看到的是**同一条 transport**,
    // 所以播放头直接读自己的 AudioPlayHead 就行 —— 不用等 viz 段那 4Hz 的冗余副本,
    // 竖线才能跟手,而且 Output 停摆时播放头照样走(不假装冻结)。
    scvb::engine::PlayheadPod playheadSnapshot() const;

    // 最近一次一致性读到的 viz 帧(消息线程独占;撕裂时沿用上帧)。
    const scvb::VizSnapshot& vizSnapshot() const { return *viz_; }
    // 帧是否新鲜(写方停摆 ≥kVizStaleMs 判陈旧;UI 显示为「Output 停摆」而非假装在线)。
    bool vizFresh() const { return vizFresh_; }

    // 生命周期锁的外借:编辑器读 `vizSnapshot()` / `groupsOnline()` / `groupId()` 时必须持它 ——
    // 那些访问器返回的是**成员的引用/取值,不是副本**,而 `refreshViz()` 与宿主的生命周期回调
    // 会在别的线程上改写同一份数据(`*viz_` 整体赋值、`vizPlane_` 解映射)。
    // 与 Output 侧 `crvsSnapshot()` 持锁同口径,只是那边返回深拷贝、这边外借锁让调用方整体持有
    // (viz 快照 ≈34KB,逐帧深拷贝不值得)。
    const juce::CriticalSection& lifecycleLock() const noexcept { return lifecycleMutex_; }

    // [M] 一次轮询:viz attach/读 + 1Hz 跨组探测 + 延迟释放回收。
    // 生产路径由 [M] 定时器驱动(SL-192 起 20Hz;发布器仍 4Hz);做成公开入口是为了让单测能在没有 MessageManager 的
    // 离线环境里直接驱动一拍(否则「零写入」只能靠真机跑 —— 那就不叫断言了)。
    void tickMessageThread(std::uint64_t nowMs);

private:
    void timerCallback() override;
    void refreshViz(std::uint64_t nowMs);
    // [A] 每块把宿主 transport 发布给 [M](零分配零锁;不碰 buffer)。
    void publishPlayhead();

    // viz 帧陈旧阈值:发布器 4Hz,连续 2s 没新帧即判写方停摆(与轮询频率无关,按毫秒计)。
    static constexpr std::uint64_t kVizStaleMs = 2000;

    // 串行化「段生命周期」与「[M] 读段」——与 Input/Output 两个先例同款
    // (`docs/SCVB_CONTRACT.md` §0.3 第 5 条逐字要求)。宿主可以从任意线程调
    // prepareToPlay / releaseResources / setStateInformation,而 [M] 定时器同时在
    // refreshViz() 里读同一份映射:无锁的话失败模式是**宿主访问违例**(解映射与读并发),
    // 不是数据轻微不一致。锁只在 [M] 与宿主生命周期回调上,**进不了 processBlock**
    // (CLAUDE.md §8:RT 路径无锁),直通那条路一个字节都没动。
    juce::CriticalSection lifecycleMutex_;

    // [A] → [M] 播放头快照(SPSC seqlock,进程内非 IPC;与 Output 的 C8 同款)。
    scvb::engine::PlayheadShot playheadShot_;
    double sampleRate_ = 48000.0; // prepareToPlay 写,[A] 只读

    scvb::SegmentBackendWin32 backend_;
    scvb::VizPlane vizPlane_; // 只读 attach;绝不 open()
    std::unique_ptr<scvb::VizSnapshot> viz_; // ≈32KB,堆持有

    int groupId_ = 1; // 1..8(A-H)
    int uiScalePercent_ = 100;
    juce::String uiLanguage_ = "zh";

    VizState vizState_ = VizState::kOffline;
    bool vizFresh_ = false;
    bool sawVizFrame_ = false; // 是否已成功读到过一帧 —— publish_ms 可能合法地为 0,
                               // 光靠 lastVizPublishMs_ != 0 判不出「读到过」(踩过)
    std::uint64_t lastVizPublishMs_ = 0; // 段内 publish_ms 的上一次取值(判新帧)
    std::uint64_t lastVizChangeMs_ = 0; // 本地观察到新帧的时刻(判陈旧)
    std::uint64_t lastAttachTryMs_ = 0; // 上次尝试 attach 的时刻(离线时 4Hz 重试,不刷屏)
    std::uint64_t lastStaleProbeMs_ = 0; // 上次「松手重探」的时刻(陈旧时每 kVizStaleMs 探一次)
    std::uint8_t groupsOnline_ = 0;
    std::uint64_t lastGroupsProbeMs_ = 0; // 1Hz 跨组探测折半

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScvbMonitorAudioProcessor)
};
