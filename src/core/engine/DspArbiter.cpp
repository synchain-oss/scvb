// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/DspArbiter.h"

namespace scvb::engine
{

void DspArbiter::prepare(double sampleRate, const DspArbiterConfig& cfg)
{
    m_cfg = cfg;
    m_sampleRate = (sampleRate > 0.0) ? sampleRate : 0.0;

    for (auto& s : m_panSmoother)
        s.reset(m_sampleRate, m_cfg.normalRampSec);
    for (auto& s : m_volSmoother)
        s.reset(m_sampleRate, m_cfg.normalRampSec);
    for (auto& s : m_widthSmoother)
        s.reset(m_sampleRate, m_cfg.normalRampSec);

    m_initialized = false;
    m_sourcesDirty = false;
    m_prevEngineAuthority = false;
    m_prevLeadSelect = 0;
    m_prevFrz.fill(0);
    m_lastAnySwitch = false;
}

void DspArbiter::setSources(const std::array<TrackSources, kNumTracks>& sources)
{
    m_sources = sources;
    m_sourcesDirty = true; // 视为版本切换(首个 processBlock 会因 !m_initialized 走硬置位)
}

void DspArbiter::setLeadSelectSource(const std::atomic<float>* rawLeadSelect)
{
    m_rawLeadSelect = rawLeadSelect;
}

float DspArbiter::readRaw(const std::atomic<float>* p) const
{
    return (p != nullptr) ? p->load(std::memory_order_relaxed) : 0.0f;
}

float DspArbiter::readRawDefault(const std::atomic<float>* p, float def) const
{
    return (p != nullptr) ? p->load(std::memory_order_relaxed) : def;
}

int DspArbiter::readFrz(int track) const
{
    return static_cast<int>(readRaw(m_sources[static_cast<std::size_t>(track)].rawFrz));
}

int DspArbiter::readLeadSelect() const
{
    return static_cast<int>(readRaw(m_rawLeadSelect));
}

void DspArbiter::armSmoother(scvb::dsp::LinearSmoother& s, float target, bool isSwitch)
{
    if (!m_initialized)
    {
        s.setCurrentAndTargetValue(target);
        return;
    }

    const double ramp = isSwitch ? m_cfg.switchRampSec : m_cfg.normalRampSec;
    s.reset(m_sampleRate, ramp);
    s.setTargetValue(target);
    if (isSwitch)
        s.reset(m_sampleRate, m_cfg.normalRampSec); // 切换 block 用 30ms,之后回到常规 10ms
}

std::array<DspArbiter::TrackValues, DspArbiter::kNumTracks> DspArbiter::processBlock(bool engineAuthority, double tSec)
{
    const bool authorityChanged = engineAuthority != m_prevEngineAuthority;
    const int lead = readLeadSelect();
    const bool leadChanged = lead != m_prevLeadSelect;
    const bool versionChanged = m_sourcesDirty && m_initialized;

    bool anyFrzChanged = false;

    for (int t = 0; t < kNumTracks; ++t)
    {
        const TrackSources& src = m_sources[static_cast<std::size_t>(t)];
        const int frz = readFrz(t);
        const int prevFrz = m_prevFrz[static_cast<std::size_t>(t)];
        if (frz != prevFrz)
            anyFrzChanged = true;

        const bool frzPanChanged = (frz & 1) != (prevFrz & 1);
        const bool frzVolChanged = (frz & 2) != (prevFrz & 2);

        // —— 双源取值仲裁(§2.3)——
        float panTarget;
        float volTarget;
        if (engineAuthority)
        {
            // 引擎权威:曲线真身采样;被冻结维度改读 host 参数(J65,优先级最高)。
            panTarget = (src.curve != nullptr) ? static_cast<float>(src.curve->panAt(tSec)) : 0.0f;
            volTarget = (src.curve != nullptr) ? static_cast<float>(src.curve->volAt(tSec)) : 0.0f;
            if ((frz & 1) != 0)
                panTarget = readRaw(src.rawPan);
            if ((frz & 2) != 0)
                volTarget = readRaw(src.rawVol);
        }
        else
        {
            // follow:host 参数是权威。
            panTarget = readRaw(src.rawPan);
            volTarget = readRaw(src.rawVol);
        }

        // [J58] lead_select 覆盖层:仅把第 n 轨 pan 强制居中;vol 不受影响,其余轨不动。
        if (lead == t + 1)
            panTarget = 0.0f;

        // 每轨 width 恒 host 权威,无曲线(§2.3)。
        const float widthTarget = readRawDefault(src.rawTrkW, 100.0f);

        m_targets[static_cast<std::size_t>(t)] = TrackValues{panTarget, volTarget, widthTarget};

        // —— 切换检测 → 30ms 平滑换档(§2.4)——
        const bool panSwitch = authorityChanged || leadChanged || frzPanChanged || versionChanged;
        const bool volSwitch = authorityChanged || frzVolChanged || versionChanged;
        const bool widthSwitch = versionChanged;

        armSmoother(m_panSmoother[static_cast<std::size_t>(t)], panTarget, panSwitch);
        armSmoother(m_volSmoother[static_cast<std::size_t>(t)], volTarget, volSwitch);
        armSmoother(m_widthSmoother[static_cast<std::size_t>(t)], widthTarget, widthSwitch);

        m_prevFrz[static_cast<std::size_t>(t)] = frz;
    }

    m_prevEngineAuthority = engineAuthority;
    m_prevLeadSelect = lead;
    m_sourcesDirty = false;
    m_initialized = true;
    m_lastAnySwitch = authorityChanged || leadChanged || versionChanged || anyFrzChanged;

    return m_targets;
}

std::array<DspArbiter::TrackValues, DspArbiter::kNumTracks> DspArbiter::nextSample()
{
    std::array<TrackValues, kNumTracks> out{};
    for (int t = 0; t < kNumTracks; ++t)
    {
        out[static_cast<std::size_t>(t)] = TrackValues{m_panSmoother[static_cast<std::size_t>(t)].getNextValue(),
                                                       m_volSmoother[static_cast<std::size_t>(t)].getNextValue(),
                                                       m_widthSmoother[static_cast<std::size_t>(t)].getNextValue()};
    }
    return out;
}

} // namespace scvb::engine
