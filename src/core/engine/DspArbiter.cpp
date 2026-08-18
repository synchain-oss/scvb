// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/DspArbiter.h"

#include <algorithm>

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
    m_prevSnapshot = nullptr;
    m_prevEngineAuthority = false;
    m_prevLeadSelect = 0;
    m_prevFrz.fill(0);
    m_lastAnySwitch = false;
    // 注意:m_snapshot 原子不在此重置 —— 由消息线程 publish 独占管理。prepare(音频线程)与
    // publish(消息线程)经原子安全并置;首个 processBlock 因 m_initialized=false 走硬置位。
}

void DspArbiter::publish(const Snapshot* snapshot)
{
    // 消息线程:release-store。发布前快照须已完整构造;旧快照由发布方保活(进程寿命)。
    m_snapshot.store(snapshot, std::memory_order_release);
}

const std::array<DspArbiter::TrackSources, DspArbiter::kNumTracks>& DspArbiter::emptySources()
{
    static const std::array<TrackSources, kNumTracks> s{}; // 全 null = 未接线默认值
    return s;
}

float DspArbiter::readRaw(const std::atomic<float>* p) const
{
    return (p != nullptr) ? p->load(std::memory_order_relaxed) : 0.0f;
}

float DspArbiter::readRawDefault(const std::atomic<float>* p, float def) const
{
    return (p != nullptr) ? p->load(std::memory_order_relaxed) : def;
}

int DspArbiter::readFrz(const TrackSources& src) const
{
    return static_cast<int>(readRaw(src.rawFrz));
}

int DspArbiter::readLeadSelect(const std::atomic<float>* rawLead) const
{
    // 防越界值误触发全轨 30ms:钳到声明值域 [0, kNumTracks]。
    return std::clamp(static_cast<int>(readRaw(rawLead)), 0, kNumTracks);
}

void DspArbiter::armSmoother(scvb::dsp::LinearSmoother& s, float target, bool isSwitch)
{
    if (!m_initialized)
    {
        s.setCurrentAndTargetValue(target);
        return;
    }

    // 稳态非切换且目标未变:不重 arm,保护进行中的 30ms 切换斜坡不被截断(§2.4)。
    if (!isSwitch && s.getTargetValue() == target)
        return;

    const double ramp = isSwitch ? m_cfg.switchRampSec : m_cfg.normalRampSec;
    s.reset(m_sampleRate, ramp); // 只改后续 setTargetValue 的斜坡时长,不重置当前值
    s.setTargetValue(target); // 从当前实际值滑向目标(零跳变)
    if (isSwitch)
        s.reset(m_sampleRate, m_cfg.normalRampSec); // 第二次 reset 只影响下一次换档,不打断本次 30ms 斜坡
}

std::array<DspArbiter::TrackValues, DspArbiter::kNumTracks> DspArbiter::processBlock(bool engineAuthority, double tSec)
{
    // 每 block 只 acquire-load 一次快照,整 block 用同一份(无撕裂)。
    const Snapshot* snap = m_snapshot.load(std::memory_order_acquire);
    const auto& sources = (snap != nullptr) ? snap->sources : emptySources();
    const std::atomic<float>* rawLead = (snap != nullptr) ? snap->rawLeadSelect : nullptr;

    const bool authorityChanged = engineAuthority != m_prevEngineAuthority;
    const int lead = readLeadSelect(rawLead);
    const bool leadChanged = lead != m_prevLeadSelect;
    const bool versionChanged = (snap != m_prevSnapshot) && m_initialized;

    bool anyFrzChanged = false;

    for (int t = 0; t < kNumTracks; ++t)
    {
        const TrackSources& src = sources[static_cast<std::size_t>(t)];
        const int frz = readFrz(src);
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
        // 覆盖优先级高于 freeze:被冻结 pan 的轨若同时被 lead_select 选中,仍强制居中。
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

    m_prevSnapshot = snap;
    m_prevEngineAuthority = engineAuthority;
    m_prevLeadSelect = lead;
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
