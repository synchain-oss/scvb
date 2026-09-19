// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/DspArbiter.h"

#include "engine/FreezeBits.h"

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
    m_panCurveLut = nullptr; // 下个 processBlock 会从快照重新锁定;此处与其余音频线程独占态同批清
    m_prevPanCurveLut = nullptr;
    m_panCurveMix = 1.0f;
    m_xfadeRemaining = 0;
    // 换表淡入窗口的总长,按 30ms 切换档算定(与 pan/vol/width 用的是同一个数,不另立)。
    // 至少 1 样本:采样率低到 30ms 不满一个样本时也不能算成 0 —— 0 会让「窗口开得了」恒假。
    m_xfadeSamples = (m_sampleRate > 0.0) ? std::max(1, static_cast<int>(m_cfg.switchRampSec * m_sampleRate + 0.5)) : 0;
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
    // 解码口径与消息线程侧(setTrackManual / ctrl 广播 / 分析入参)共用同一份 —— 见 FreezeBits.h
    // 头注:两边分叉就会出现「一边按未冻结去写曲线、另一边按已冻结去读参数面」的错位。
    return freezeBitsOf(readRaw(src.rawFrz));
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
    // G 的查表与本块 TrackValues 取自**同一份** snap —— 分两次 load 就可能一半旧一半新。
    const scvb::PanCurveLut* const lutNow = (snap != nullptr) ? snap->panCurveLut.get() : nullptr;

    // 换表 → 开 30ms 交叉淡入窗口(曲线编辑与版本切换共用这一条路径,不为版本切换另写一份)。
    // 判的是 **LUT 对象指针**:段编辑会造一堆新快照但不换表,那时这里不触发(见头文件的注)。
    // m_initialized 之前不淡入:首块本来就没有「上一张表」可淡。
    if (m_initialized && lutNow != m_panCurveLut && m_xfadeSamples > 0)
    {
        // 窗口内再次换表:拿当时正在淡向的那张当新的旧表并重启窗口。残留不连续 ≤ 本次残差,
        // 远小于完全不淡入时的整跳;真要消掉它得在音频线程合成一张中间表(禁止分配),不做。
        m_prevPanCurveLut = m_panCurveLut;
        m_xfadeRemaining = m_xfadeSamples;
        m_panCurveMix = 0.0f;
    }
    m_panCurveLut = lutNow;

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
    // 换表淡入:整数递减,到 0 就**把旧表指针置 null**——此后 panCurveGainDb 只查一张表,
    // 与不做淡入时逐位相同。窗口有界由这一句负责,删了它窗口就永远开着(有删除式钉)。
    if (m_xfadeRemaining > 0)
    {
        --m_xfadeRemaining;
        m_panCurveMix = 1.0f - static_cast<float>(m_xfadeRemaining) / static_cast<float>(m_xfadeSamples);
    }
    else if (m_prevPanCurveLut != nullptr)
    {
        m_prevPanCurveLut = nullptr;
        m_panCurveMix = 1.0f;
    }

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
