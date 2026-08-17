// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>

#include "dsp/ParamSmoother.h"
#include "engine/CurveEvaluator.h"

// DspArbiter:DSP 取值仲裁 + 统一平滑层(03 §2.3 / §2.4)。
// 双源:分析曲线(CurveEvaluator,真身)vs host 参数(raw atomic,打印头)。
// 纯 C++17:只读 const std::atomic<float>* 与 const CurveEvaluator*,绝不写入曲线/参数真身(ADR-005)。
// 平滑:pan 在参数值域、vol 在 dB 域、每轨 width 在其值域;常规 10ms,权威/版本/lead_select/freeze
// 切换 30ms,切换只换目标不重置当前值 → 零跳变。
namespace scvb::engine
{

struct DspArbiterConfig
{
    double normalRampSec = 0.010; // §2.4 常规跟随 10ms
    double switchRampSec = 0.030; // §2.4 权威/版本/lead_select/freeze 切换 30ms
};

class DspArbiter
{
public:
    static constexpr int kNumTracks = 15; // [J59]

    // 每轨的「活动版本」取值来源。裸指针由 Output 侧持有;null 表示未接线(按默认值读)。
    struct TrackSources
    {
        const std::atomic<float>* rawPan = nullptr; // host 参数 pan(-100..100)
        const std::atomic<float>* rawVol = nullptr; // host 参数 vol(-24..12)
        const std::atomic<float>* rawTrkW = nullptr; // 每轨 width(0..100,[J57])
        const std::atomic<float>* rawFrz = nullptr; // 每轨 freeze(0..3,[J65],int 存 float)
        const CurveEvaluator* curve = nullptr; // 活动版本曲线真身(null → 恒 0)
    };

    struct TrackValues
    {
        float pan = 0.0f; // 参数值域 [-100,100]
        float volDb = 0.0f; // dB 域 [-24,12]
        float width = 100.0f; // 每轨 width [0,100]
    };

    void prepare(double sampleRate, const DspArbiterConfig& cfg = {});

    // 绑定活动版本来源(setup / version 切换时调用)。绑定变更视为「版本切换」→ 30ms。
    void setSources(const std::array<TrackSources, kNumTracks>& sources);

    // 绑定 lead_select 全局参数的 raw atomic(host 恒权威,[J58])。
    void setLeadSelectSource(const std::atomic<float>* rawLeadSelect);

    // 每 block:算出各轨 raw 目标、检测切换事件并 arm 平滑斜坡。返回 raw(未平滑)目标。
    // engineAuthority = outputEnabled(ARMED/PRINT 均 true);tSec = 本 block 起始时间线位置。
    std::array<TrackValues, kNumTracks> processBlock(bool engineAuthority, double tSec);

    // 每样本:前进一格平滑器,返回平滑后的 pan/vol/width。
    std::array<TrackValues, kNumTracks> nextSample();

    // 最近一次 processBlock 的 raw 目标(未平滑,供测试断言 PRINT 不经过参数等)。
    const std::array<TrackValues, kNumTracks>& lastTargets() const { return m_targets; }

    // 测试内省:最近一次 block 是否触发了任一 30ms 切换。
    bool lastBlockWasSwitch() const { return m_lastAnySwitch; }

    const scvb::dsp::LinearSmoother& panSmoother(int track) const
    {
        return m_panSmoother[static_cast<std::size_t>(track)];
    }
    const scvb::dsp::LinearSmoother& volSmoother(int track) const
    {
        return m_volSmoother[static_cast<std::size_t>(track)];
    }
    const scvb::dsp::LinearSmoother& widthSmoother(int track) const
    {
        return m_widthSmoother[static_cast<std::size_t>(track)];
    }

private:
    float readRaw(const std::atomic<float>* p) const;
    float readRawDefault(const std::atomic<float>* p, float def) const;
    int readFrz(int track) const;
    int readLeadSelect() const;

    void armSmoother(scvb::dsp::LinearSmoother& s, float target, bool isSwitch);

    DspArbiterConfig m_cfg;
    double m_sampleRate = 0.0;
    std::array<TrackSources, kNumTracks> m_sources{};
    const std::atomic<float>* m_rawLeadSelect = nullptr;

    std::array<scvb::dsp::LinearSmoother, kNumTracks> m_panSmoother{};
    std::array<scvb::dsp::LinearSmoother, kNumTracks> m_volSmoother{};
    std::array<scvb::dsp::LinearSmoother, kNumTracks> m_widthSmoother{};

    std::array<TrackValues, kNumTracks> m_targets{};

    bool m_initialized = false;
    bool m_sourcesDirty = false;
    bool m_prevEngineAuthority = false;
    int m_prevLeadSelect = 0;
    std::array<int, kNumTracks> m_prevFrz{};
    bool m_lastAnySwitch = false;
};

} // namespace scvb::engine
