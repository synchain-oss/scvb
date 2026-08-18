// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <memory>
#include <vector>

#include "OutputParams.h"
#include "engine/DspArbiter.h"

// OutputAuthority:Output 侧调用点(T16)。把 T15 的 ParamHandles(123 参数 raw atomic)绑定到
// T16 的 DspArbiter(核心仲裁 + 统一平滑),并把活动版本的曲线真身(CurveEvaluator)以指针注入。
// 线程契约:setVersionActive/setCurve 可在消息线程任意时刻调用 —— 它们只写本类消息线程独占的
// 配置(m_curves/m_versionActive)并重建「不可变快照」,经 DspArbiter::publish 原子发布
// (release-store);音频线程 processBlock 每 block acquire-load 一次、整 block 用同一份快照。
// 旧快照由本类快照池进程寿命保活,绝不释放。prepare 应在音频启动前调用(它触碰 m_handles 配置,
// 不参与快照发布;若必须与 setCurve/setVersionActive 并发,由调用方串行化)。
namespace scvb::output
{

class OutputAuthority
{
public:
    static constexpr int kNumTracks = scvb::engine::DspArbiter::kNumTracks;
    static constexpr int kNumVersions = scvb::params::kNumVersions;

    void prepare(double sampleRate, const scvb::params::ParamHandles& handles);

    // 消息线程任意时刻可调:重绑 raw atomic + 曲线指针并原子发布新快照。
    void setVersionActive(int version);

    // 消息线程任意时刻可调:注入(version 1-based, track 0-based)曲线真身;可传 nullptr(恒 0)。
    void setCurve(int version, int track, const scvb::CurveEvaluator* curve);

    int versionActive() const { return m_versionActive; }
    bool isPrepared() const { return m_prepared; }

    // 音频线程:仲裁 + arm 平滑。engineAuthority = output_enabled(ARMED/PRINT 均 true)。
    std::array<scvb::engine::DspArbiter::TrackValues, kNumTracks> processBlock(bool engineAuthority, double tSec);

    // 音频线程:前进平滑器。
    std::array<scvb::engine::DspArbiter::TrackValues, kNumTracks> nextSample();

    const scvb::engine::DspArbiter& arbiter() const { return m_arbiter; }

private:
    void rebindSources();

    scvb::params::ParamHandles m_handles;
    scvb::engine::DspArbiter m_arbiter;
    int m_versionActive = 1;
    bool m_prepared = false;
    std::array<std::array<const scvb::CurveEvaluator*, kNumTracks>, kNumVersions> m_curves{};
    // 快照池:进程寿命保活已发布的快照,绝不释放(音频线程可能仍在读旧快照;快照小、数量少)。
    std::vector<std::unique_ptr<scvb::engine::DspArbiter::Snapshot>> m_snapshotPool;
};

} // namespace scvb::output
