// SPDX-License-Identifier: GPL-3.0-or-later
#include "OutputAuthority.h"

#include <functional>

#include "SegmentEditService.h" // configureCrvsUndoBudget:CRVS 撤销预算的唯一真源

namespace
{

// 版本复制撤销动作:捕获旧 dst 曲线(shared_ptr 浅拷贝 —— 曲线对象不可变,浅拷贝即完整语义快照)
// 与旧 meta;undo 时原样放回,旧曲线对象由 shared_ptr 保活(曲线不可变契约)。
class CopyVersionAction final : public juce::UndoableAction
{
public:
    CopyVersionAction(scvb::engine::VersionStore& store, int src, int dst, std::function<void()> onChanged)
        : m_store(store), m_src(src), m_dst(dst), m_oldCurves(store.snapshotCurves(dst)), m_oldMeta(store.meta(dst)),
          m_onChanged(std::move(onChanged))
    {
    }

    bool perform() override
    {
        m_store.applyCopy(m_src, m_dst);
        if (m_onChanged)
            m_onChanged();
        return true;
    }

    bool undo() override
    {
        m_store.restoreCurves(m_dst, m_oldCurves);
        m_store.setMeta(m_dst, m_oldMeta);
        if (m_onChanged)
            m_onChanged();
        return true;
    }

    int getSizeInUnits() override { return 1; }

private:
    scvb::engine::VersionStore& m_store;
    int m_src = 0;
    int m_dst = 0;
    std::array<std::shared_ptr<const scvb::CurveEvaluator>, scvb::engine::kNumTracks> m_oldCurves{};
    scvb::engine::VersionMeta m_oldMeta{};
    std::function<void()> m_onChanged;
};

// 版本重命名撤销动作:捕获旧名,undo 时还原。
class RenameVersionAction final : public juce::UndoableAction
{
public:
    RenameVersionAction(scvb::engine::VersionStore& store, int version, std::string newName)
        : m_store(store), m_version(version), m_newName(std::move(newName)), m_oldName(store.versionName(version))
    {
    }

    bool perform() override
    {
        m_store.setVersionName(m_version, m_newName);
        return true;
    }

    bool undo() override
    {
        m_store.setVersionName(m_version, m_oldName);
        return true;
    }

    int getSizeInUnits() override { return 1; }

private:
    scvb::engine::VersionStore& m_store;
    int m_version = 0;
    std::string m_newName;
    std::string m_oldName;
};

} // namespace

namespace scvb::output
{

void OutputAuthority::prepare(double sampleRate, const scvb::params::ParamHandles& handles)
{
    m_handles = handles;
    m_arbiter.prepare(sampleRate);
    // version_active 是用户 state,prepare(含采样率变化重入)不重置它。
    m_prepared = true;
    rebindSources();
}

void OutputAuthority::setVersionActive(int version)
{
    const int before = m_versions.versionActive();
    m_versions.setVersionActive(version); // 越界钳制 + warning 计数(不静默取模)
    if (m_prepared && m_versions.versionActive() != before)
        rebindSources(); // 版本号未变则不重发快照(避免虚假 versionChanged)
}

OutputAuthority::OutputAuthority()
{
    // 撤销预算的**唯一**装配点。CopyVersion/RenameVersion 这两条动作 getSizeInUnits 恒回 1,
    // 在这套字节口径下等于「几乎不占预算」—— 正确:它们持有的是 shared_ptr 浅拷贝与一个字符串,
    // 与整表 CRVS 快照不在一个量级。
    scvb::output::configureCrvsUndoBudget(m_undoManager);
}

int OutputAuthority::versionActive() const
{
    return m_versions.versionActive();
}

void OutputAuthority::setCurve(int version, int track, const scvb::CurveEvaluator* curve)
{
    // 不可变契约:VersionStore 深拷贝 src 进新建对象再替换,绝不原地改写已发布曲线;
    // 曲线生命周期由 VersionStore 与发布出的快照(shared_ptr)共同保证。
    m_versions.setCurve(version, track, curve);
    if (version == m_versions.versionActive() && m_prepared)
        rebindSources();
}

scvb::engine::CopyVersionResult OutputAuthority::copyVersion(int src, int dst, scvb::engine::AuthorityMode mode)
{
    // 前置校验(§5.3):PRINT 拒绝 / 越界 / src==dst;不满足 → UI 拒绝,不动 state、不进 undo。
    const scvb::engine::CopyVersionResult result = m_versions.validateCopy(src, dst, mode);
    if (result != scvb::engine::CopyVersionResult::Ok)
        return result;

    // 单事务 + 单条撤销(§5.3 line 607):复制是纯 state 深拷贝,零 gesture、零参数写入、零宿主 undo。
    m_undoManager.beginNewTransaction("Copy V" + juce::String(src) + " → V" + juce::String(dst));
    m_undoManager.perform(new CopyVersionAction(m_versions, src, dst, [this, dst] {
        // perform(初始 + redo)与 undo 后,若 dst 是活跃版本则重发快照,保证 DSP 反映复制/撤销结果。
        if (m_prepared && dst == m_versions.versionActive())
            rebindSources();
    }));

    // 注:标记 state dirty + updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true)) 由
    // 将来 PluginProcessor 接线处执行(本类不持有 AudioProcessor;REAPER 有时忽略,仅尽力)。
    return scvb::engine::CopyVersionResult::Ok;
}

scvb::engine::SetNameResult OutputAuthority::setVersionName(int version, const juce::String& name)
{
    std::string effective;
    const scvb::engine::SetNameResult result =
        scvb::engine::normalizeVersionName(version, name.toStdString(), effective);
    if (result == scvb::engine::SetNameResult::InvalidIndex)
        return result;

    // 归一化后与当前名一致 → 无操作(不产生空撤销事务)。
    if (m_versions.versionName(version) == effective)
        return result;

    m_undoManager.beginNewTransaction("Rename V" + juce::String(version));
    m_undoManager.perform(new RenameVersionAction(m_versions, version, effective));

    return result;
}

juce::String OutputAuthority::versionName(int version) const
{
    return juce::String(m_versions.versionName(version));
}

juce::ValueTree OutputAuthority::toState() const
{
    // T18 版本层 state 最小往返(名称 + active)。T19 将把这些字段并入 CRVS chunk(StateCodec)。
    juce::ValueTree state("SCVB-VERSIONS");
    state.setProperty("v1_name", juce::String(m_versions.versionName(1)), nullptr);
    state.setProperty("v2_name", juce::String(m_versions.versionName(2)), nullptr);
    state.setProperty("active", m_versions.versionActive(), nullptr);
    return state;
}

void OutputAuthority::fromState(const juce::ValueTree& state)
{
    if (!state.hasType("SCVB-VERSIONS"))
        return;

    for (int v = 1; v <= kNumVersions; ++v)
    {
        const juce::String key = "v" + juce::String(v) + "_name";
        const juce::String fallback = juce::String(m_versions.versionName(v));
        m_versions.setVersionName(v, state.getProperty(key, fallback).toString().toStdString());
    }

    m_versions.setVersionActive(static_cast<int>(state.getProperty("active", m_versions.versionActive())));

    if (m_prepared)
        rebindSources();
}

int OutputAuthority::warningCount() const
{
    return m_versions.warningCount();
}

std::array<const scvb::CurveEvaluator*, OutputAuthority::kNumTracks> OutputAuthority::activeCurves() const
{
    std::array<const scvb::CurveEvaluator*, kNumTracks> out{};
    const int v = m_versions.versionActive();
    for (int t = 0; t < kNumTracks; ++t)
    {
        out[static_cast<std::size_t>(t)] = m_versions.curve(v, t);
    }
    return out;
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
    const int v = m_versions.versionActive() - 1; // 0-based

    // 构建不可变快照,完整构造后原子发布;旧快照进池保活(进程寿命)。
    auto snap = std::make_unique<scvb::engine::DspArbiter::Snapshot>();
    const auto curves = m_versions.snapshotCurves(m_versions.versionActive());
    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& s = snap->sources[static_cast<std::size_t>(t)];
        s.rawPan = m_handles.rawPan[v][t];
        s.rawVol = m_handles.rawVol[v][t];
        s.rawTrkW = m_handles.rawTrkW[v][t];
        s.rawFrz = m_handles.rawFrz[v][t];
        s.curve = curves[static_cast<std::size_t>(t)]; // shared_ptr 保活曲线(不可变契约)
    }
    snap->rawLeadSelect = m_handles.rawLeadSelect;

    m_arbiter.publish(snap.get()); // release-store
    m_snapshotPool.push_back(std::move(snap)); // 进程寿命保活
}

} // namespace scvb::output
