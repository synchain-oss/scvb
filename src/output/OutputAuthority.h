// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include <juce_data_structures/juce_data_structures.h>

#include "OutputParams.h"
#include "engine/DspArbiter.h"
#include "engine/VersionStore.h"

// OutputAuthority:Output 侧调用点(T16)。把 T15 的 ParamHandles(123 参数 raw atomic)绑定到
// T16 的 DspArbiter(核心仲裁 + 统一平滑),并把活动版本的曲线真身(CurveEvaluator)注入快照。
//
// T18:本类持有 engine::VersionStore(versions[2] 曲线真身 + 版本名),并经 juce::UndoManager 提供
// 可撤销的版本复制与重命名(03 §5.3 / [J05])。version_active 是 state(非自动化),值域 1..2,
// 越界钳制 + warning 计数(03 §5.2,不静默取模)。
//
// 曲线不可变契约(PR #43 终审,硬前置):
//   setCurve 注入后的曲线对象必须不可变;重分析 = 新建 CurveEvaluator 对象再 setCurve 发布,
//   禁止对已注入对象调用 build()。曲线生命周期须 ≥ OutputAuthority/音频线程寿命 —— VersionStore
//   与本类发布出的快照均以 std::shared_ptr<const CurveEvaluator> 持有曲线,旧曲线对象由引用它的
//   快照保活,音频线程绝不读悬垂/被原地改写的对象。
// 线程契约:setVersionActive/setCurve/copyVersion/setVersionName 均只可在消息线程调用 —— 它们只写
// 本类消息线程独占的配置并重建「不可变快照」,经 DspArbiter::publish 原子发布(release-store);
// 音频线程 processBlock 每 block acquire-load 一次、整 block 用同一份快照。旧快照由本类快照池
// 进程寿命保活,绝不释放。prepare 应在音频启动前调用(它触碰 m_handles 配置,不参与快照发布;
// 若必须与 setCurve/setVersionActive 并发,由调用方串行化)。
namespace scvb::output
{

class OutputAuthority
{
public:
    static constexpr int kNumTracks = scvb::engine::kNumTracks;
    static constexpr int kNumVersions = scvb::engine::kNumVersions;

    static_assert(kNumVersions == scvb::params::kNumVersions, "engine/params 版本数漂移");
    static_assert(kNumTracks == scvb::params::kNumTracks, "engine/params 轨数漂移");

    void prepare(double sampleRate, const scvb::params::ParamHandles& handles);

    // ---- 版本切换(消息线程)----
    void setVersionActive(int version); // 越界钳制 + warning 计数
    int versionActive() const;

    // ---- 曲线注入(消息线程;不可变契约见类注释)----
    void setCurve(int version, int track, const scvb::CurveEvaluator* curve);

    // ---- 版本复制 §5.3(消息线程;纯 state 深拷贝,零 gesture、零参数写入;单条撤销)----
    scvb::engine::CopyVersionResult copyVersion(int src, int dst, scvb::engine::AuthorityMode mode);

    // ---- 版本重命名 [J05](消息线程;可撤销;返回归一化结果供 UI 反馈)----
    scvb::engine::SetNameResult setVersionName(int version, const juce::String& name);
    juce::String versionName(int version) const;

    // ---- 版本层 state 的最小往返(名称 + active + meta),仅 T18 桥接/单测;T19 并入 CRVS chunk ----
    juce::ValueTree toState() const;
    void fromState(const juce::ValueTree& state);

    // ---- 插件自有撤销(§5.3:一次复制 = 单条撤销;供 UI/T25 桥接 undo/redo)----
    juce::UndoManager& undoManager() { return m_undoManager; }

    int warningCount() const;
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
    scvb::engine::VersionStore m_versions;
    juce::UndoManager m_undoManager;
    bool m_prepared = false;
    // 快照池:进程寿命保活已发布的快照,绝不释放(音频线程可能仍在读旧快照;快照小、数量少)。
    std::vector<std::unique_ptr<scvb::engine::DspArbiter::Snapshot>> m_snapshotPool;
};

} // namespace scvb::output
