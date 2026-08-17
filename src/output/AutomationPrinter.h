// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

#include <juce_audio_processors/juce_audio_processors.h>

#include "OutputParams.h"
#include "engine/AuthorityMode.h"
#include "engine/CurveEvaluator.h"
#include "engine/PlayheadShot.h"

// AutomationPrinter:50Hz 消息线程打印器(03 §3.2 / §3.3)。把引擎曲线真身打印到宿主自动化参数。
// 挂在 Processor 上(不是 Editor!GUI 关闭也要打印,§4.2 REAPER 对策①)。
// 关键纪律(CLAUDE.md §8 / R6):setValueNotifyingHost / beginChangeGesture 只在消息线程 Timer
// 回调内调用,音频线程只发布 PlayheadShot(SPSC),绝不触碰宿主参数。
namespace scvb::output
{

class AutomationPrinter : private juce::Timer
{
public:
    static constexpr int kNumTracks = scvb::params::kNumTracks; // 15 [J59]
    static constexpr int kNumLanes = kNumTracks * 2; // 30 = 15 轨 × {pan, vol}(§3.2)

    // §3.5 层 2:setValueNotifyingHost 自触发 listener 抑制(RAII)。
    // 消息线程单线程,普通 bool 即可;listener 首行 isSelfWrite() 短路。
    class ScopedSelfWriteFlag
    {
    public:
        explicit ScopedSelfWriteFlag(AutomationPrinter& printer) noexcept : m_printer(printer)
        {
            m_printer.m_selfWrite = true;
        }
        ~ScopedSelfWriteFlag() noexcept { m_printer.m_selfWrite = false; }
        ScopedSelfWriteFlag(const ScopedSelfWriteFlag&) = delete;
        ScopedSelfWriteFlag& operator=(const ScopedSelfWriteFlag&) = delete;

    private:
        AutomationPrinter& m_printer;
    };

    // §3.3 R12:GestureGuard(RAII)。析构时幂等 endAllGestures,兜底闭合 gesture。
    class GestureGuard
    {
    public:
        explicit GestureGuard(AutomationPrinter& printer) noexcept : m_printer(printer) {}
        ~GestureGuard() noexcept { m_printer.endAllGestures(); }
        GestureGuard(const GestureGuard&) = delete;
        GestureGuard& operator=(const GestureGuard&) = delete;

    private:
        AutomationPrinter& m_printer;
    };

    // 活跃车道(≤30 条)。[t][pan|vol],版本切换时重绑 [J59]。
    struct Lane
    {
        juce::AudioParameterFloat* param = nullptr; // 车道参数(pan/vol;width/freeze/全局三件永不打印)
        const scvb::CurveEvaluator* curve = nullptr; // 活动版本曲线真身(nullptr = 该轨未分析,不打印)
        const std::atomic<float>* freezeRaw = nullptr; // 本轨 freeze 参数 raw(J65:bit0=pan/bit1=vol)
        bool isPan = false; // true=pan 车道,false=vol 车道
        float lastWrittenNorm = -1.0f;
        bool everWritten = false;
        bool gestureOpen = false; // 三段式配对准绳(§3.3)
    };

    AutomationPrinter();
    ~AutomationPrinter() override; // GestureGuard 兜底:析构路径闭合 gesture

    // 状态机驱动(§2.2)。离开 PRINT 的所有路径统一 endAllGestures。
    void setMode(scvb::engine::AuthorityMode mode);

    // 注入 playhead 快照通道(音频线程写,本类消息线程读)。可为 nullptr(未接线)。
    void setShot(const scvb::engine::PlayheadShot* shot) noexcept { m_shot = shot; }

    // 已分析区间(秒)。区间外零 gesture、零写入(R5 / §4.1)。
    void setAnalyzedRange(double startSec, double endSec);

    // 活动版本曲线真身(按轨)。与 bindVersion 一起在版本切换时重绑 [J59]。
    void setCurves(const std::array<const scvb::CurveEvaluator*, kNumTracks>& curves);

    // 重绑活动版本车道(版本切换)。handles = Processor 缓存的 123 参数句柄(T15)。
    void bindVersion(int version, const scvb::params::ParamHandles& handles);

    // 轨 enabled=false 的整轨车道不 begin、不写(§3.2)。
    void setTrackEnabled(int track, bool enabled);

    // state print.* 参数。deadband 单位:pan 值域 / vol dB(§3.2)。
    void setDeadbandPan(float pan) noexcept { m_deadbandPan = pan; }
    void setDeadbandVolDb(float db) noexcept { m_deadbandVolDb = db; }
    void setLookaheadMs(int ms) noexcept { m_lookaheadMs = ms; }
    void setTimeOffsetMs(int ms) noexcept { m_timeOffsetMs = juce::jlimit(-200, 200, ms); } // §3.4 -200..+200

    // 50Hz Timer 启停(仅 Processor 调用;Editor 绝不持有本类)。
    void startPrinting() { startTimerHz(50); }
    void stopPrinting() { stopTimer(); }

    // §3.5 防回环:setValueNotifyingHost 自触发抑制查询。
    bool isSelfWrite() const noexcept { return m_selfWrite; }

    // §3.5 层 1(宿主回吐 → 引擎,按模式屏蔽而非 epsilon 屏蔽):分类打印车道。
    // 仅 v{v}_t{tt}_{pan|vol} 为打印车道;全局三件 + 每轨 width/freeze 为非车道(host 恒权威)。
    static bool isLaneParam(const juce::String& paramId);

    // 四出口兜底(R12):析构 / releaseResources / 模式切换 / epoch 跳变。幂等。
    void endAllGestures();

    // 测试/诊断内省。
    bool anyGestureOpen() const;
    int numGesturesOpen() const;
    scvb::engine::AuthorityMode mode() const noexcept { return m_mode; }
    const std::array<Lane, kNumLanes>& lanes() const noexcept { return m_lanes; }

    // 直接推进一帧打印(等价一次 50Hz timerCallback)。供测试驱动,消息线程调用。
    void tick();

private:
    void timerCallback() override { tick(); }

    bool laneFrozen(const Lane& lane) const;
    bool nearSegmentBoundary(const scvb::CurveEvaluator* curve, double tSec) const;

    scvb::engine::AuthorityMode m_mode = scvb::engine::AuthorityMode::Follow;
    const scvb::engine::PlayheadShot* m_shot = nullptr;
    scvb::engine::PlayheadPod m_lastShot{};

    bool m_hasRange = false; // 未设置分析区间 → 零打印(§4.1)
    double m_rangeStartSec = 0.0;
    double m_rangeEndSec = 0.0;

    std::array<const scvb::CurveEvaluator*, kNumTracks> m_curves{};
    std::array<bool, kNumTracks> m_trackEnabled{};
    std::array<Lane, kNumLanes> m_lanes{};

    float m_deadbandPan = 0.5f; // state print.deadband_pan(pan 值域)
    float m_deadbandVolDb = 0.1f; // state print.deadband_vol_db(dB)
    int m_lookaheadMs = 20; // state print.lookahead_ms,补偿 Timer 抖动
    int m_timeOffsetMs = 0; // state print.time_offset_ms(-200..+200)

    bool m_selfWrite = false; // ScopedSelfWriteFlag 置位,消息线程单线程

    // 最后声明 → 最先析构:在 m_lanes 等成员仍存活时闭合 gesture(析构链兜底)。
    GestureGuard m_gestureGuard{*this};
};

} // namespace scvb::output
