// SPDX-License-Identifier: GPL-3.0-or-later
#include "OutputAuthority.h"

namespace scvb::output
{

void OutputAuthority::prepare(double sampleRate, const scvb::params::ParamHandles& handles)
{
    m_handles = handles;
    m_arbiter.prepare(sampleRate);
    m_versionActive = 1;
    m_prepared = true;
    rebindSources();
}

void OutputAuthority::setVersionActive(int version)
{
    if (version < 1)
        version = 1;
    if (version > scvb::params::kNumVersions)
        version = scvb::params::kNumVersions;

    if (version == m_versionActive && m_prepared)
        return;

    m_versionActive = version;
    if (m_prepared)
        rebindSources();
}

void OutputAuthority::setCurve(int version, int track, const scvb::CurveEvaluator* curve)
{
    if (version < 1 || version > kNumVersions || track < 0 || track >= kNumTracks)
        return;

    m_curves[static_cast<std::size_t>(version - 1)][static_cast<std::size_t>(track)] = curve;
    if (version == m_versionActive && m_prepared)
        rebindSources();
}

std::array<scvb::engine::DspArbiter::TrackValues, OutputAuthority::kNumTracks>
OutputAuthority::processBlock(bool engineAuthority, double tSec)
{
    return m_arbiter.processBlock(engineAuthority, tSec);
}

std::array<scvb::engine::DspArbiter::TrackValues, OutputAuthority::kNumTracks> OutputAuthority::nextSample()
{
    return m_arbiter.nextSample();
}

void OutputAuthority::rebindSources()
{
    const int v = m_versionActive - 1; // 0-based

    // 构建不可变快照,完整构造后原子发布;旧快照进池保活(进程寿命)。
    auto snap = std::make_unique<scvb::engine::DspArbiter::Snapshot>();
    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& s = snap->sources[static_cast<std::size_t>(t)];
        s.rawPan = m_handles.rawPan[v][t];
        s.rawVol = m_handles.rawVol[v][t];
        s.rawTrkW = m_handles.rawTrkW[v][t];
        s.rawFrz = m_handles.rawFrz[v][t];
        s.curve = m_curves[static_cast<std::size_t>(v)][static_cast<std::size_t>(t)];
    }
    snap->rawLeadSelect = m_handles.rawLeadSelect;

    m_arbiter.publish(snap.get()); // release-store
    m_snapshotPool.push_back(std::move(snap)); // 进程寿命保活
}

} // namespace scvb::output
