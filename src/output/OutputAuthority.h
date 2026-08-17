// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

#include "OutputParams.h"
#include "engine/DspArbiter.h"

// OutputAuthority:Output 侧调用点(T16)。把 T15 的 ParamHandles(123 参数 raw atomic)绑定到
// T16 的 DspArbiter(核心仲裁 + 统一平滑),并把活动版本的曲线真身(CurveEvaluator,由 T18/T19
// 的 state 层持有)以指针注入。本类不做混音/写环 —— 那是 T24;它只负责「仲裁取值 → 平滑值」。
namespace scvb::output
{

class OutputAuthority
{
public:
    static constexpr int kNumTracks = scvb::engine::DspArbiter::kNumTracks;
    static constexpr int kNumVersions = scvb::params::kNumVersions;

    // 缓存句柄并准备平滑器。version_active 默认 1。
    void prepare(double sampleRate, const scvb::params::ParamHandles& handles);

    // 切活动版本(1-based);重绑 raw atomic 与曲线指针。与首次 prepare 共用 rebindSources。
    void setVersionActive(int version);

    // 注入(version 1-based, track 0-based)曲线真身;可传 nullptr(该轨曲线恒 0)。
    void setCurve(int version, int track, const scvb::CurveEvaluator* curve);

    int versionActive() const { return m_versionActive; }
    bool isPrepared() const { return m_prepared; }

    // 每 block:仲裁 + arm 平滑。engineAuthority = output_enabled(ARMED/PRINT 均 true)。
    std::array<scvb::engine::DspArbiter::TrackValues, kNumTracks> processBlock(bool engineAuthority, double tSec);

    // 每样本:前进平滑器。
    std::array<scvb::engine::DspArbiter::TrackValues, kNumTracks> nextSample();

    const scvb::engine::DspArbiter& arbiter() const { return m_arbiter; }

private:
    void rebindSources();

    scvb::params::ParamHandles m_handles;
    scvb::engine::DspArbiter m_arbiter;
    int m_versionActive = 1;
    bool m_prepared = false;
    std::array<std::array<const scvb::CurveEvaluator*, kNumTracks>, kNumVersions> m_curves{};
};

} // namespace scvb::output
