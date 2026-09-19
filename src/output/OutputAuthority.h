// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include <juce_data_structures/juce_data_structures.h>

#include "OutputParams.h"
#include "analysis/PanCurve.h"
#include "engine/DspArbiter.h"
#include "engine/VersionStore.h"
#include "state/StateCodec.h" // [#256 复审⑥] scvb::state::kNumTracks(下面那条 static_assert)

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
    // [#256 复审⑥] **state 那一份**也要绑住。`scvb::state::kNumTracks`(StateCodec.h,CRVS 容器
    // 头用)与本文件/params 是各自独立写的字面量,三处任意一处被改错都不会有人报错 —— 而它们的
    // 失效方向是「段表按 15 条轨解码、引擎按 16 条轨发布」这类静默错位。
    // 显式转 `int`:state 那份是 `std::size_t`,直接比在 /W4 下是 C4389(有符号/无符号比较)。
    static_assert(static_cast<int>(scvb::state::kNumTracks) == scvb::params::kNumTracks, "state/params 轨数漂移");

    // [#152 复审【重要】①②] 构造时给 m_undoManager 装上 CRVS 撤销预算(见 SegmentEditService.h 的
    // `configureCrvsUndoBudget` / `kCrvsUndoBudgetBytes`)。不能吃 juce::UndoManager 的默认
    // (30000 units, 30 步):`CrvsTransactionAction::getSizeInUnits()` 按字节记账后,真实工程的
    // **单条**事务就吃光默认预算 —— 深度塌到 30 步,而内存反倒没被按字节封住。
    OutputAuthority();

    void prepare(double sampleRate, const scvb::params::ParamHandles& handles);

    // ---- 版本切换(消息线程)----
    void setVersionActive(int version); // 越界钳制 + warning 计数
    int versionActive() const;

    // ---- 曲线注入(消息线程;不可变契约见类注释)----
    void setCurve(int version, int track, const scvb::CurveEvaluator* curve);

    // ---- pan 角度域曲线 G 注入(消息线程;02 §7 / §8.1 步骤 5)----
    // 把该版本的点列表烘成 PanCurveLut 并(若是活动版本)重发快照,音频线程即刻按新表施加 G。
    // 点列表与上次烘的**逐字段相同则整个调用是 no-op**:不重建、不重发。这不只是省 CPU ——
    // LUT 对象指针的稳定性是下游判「这次换表了没有」的依据,无谓重建会把它抖掉。
    // 点列表为空 → LUT 全 0 dB → 增益恒 1(从没画过曲线的工程声音一个字节不变)。
    void setPanCurve(int version, const std::vector<scvb::PanCurvePoint>& points);

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

    // 活动版本各轨曲线裸指针(消息线程;供 T29 打印器重绑,曲线对象由 VersionStore 保活)。
    std::array<const scvb::CurveEvaluator*, kNumTracks> activeCurves() const;

    // 活动版本的 G 查表(消息线程;供单测与 UI 对拍「画的 == 听的」)。从没设过 → null。
    std::shared_ptr<const scvb::PanCurveLut> activePanCurveLut() const;

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
    // 快照池:进程寿命保活已发布的快照,绝不释放(音频线程可能仍在读旧快照)。
    //
    // ⚠ [SL-442 → SL-445] 自 SL-442 起每条快照多钉住一份 `PanCurveLut`。实测 `sizeof`:
    //   Snapshot = 744 B、PanCurveLut = 131,076 B(128.0 KiB),比值 176×。
    //   LUT 是**每张不同曲线一份**(快照持 shared_ptr,复制只加引用计数);池不回收
    //   ⇒ 每次「真编辑」(点列表确有变化)留下的那 128 KiB 此后一直在。
    //   产出速率实测:拖点松手 1 份;Q 滑杆 140 ms 防抖 ⇒ 约 7 份/秒;撤销、重做各 1 份。
    //   池本身在 SL-442 之前就不回收(`rebuildAllCurves` 一次经 setCurve 发 15 条 = 11,160 B)。
    //   回收机制转 SL-445。
    std::vector<std::unique_ptr<scvb::engine::DspArbiter::Snapshot>> m_snapshotPool;
    // 每版本一张 G 查表(pan_curve 是 per-version,不是 per-track)+ 烘它时用的点列表。
    // 留着点列表是为了 setPanCurve 的 no-op 判定 —— 见该函数注释。
    std::array<std::shared_ptr<const scvb::PanCurveLut>, kNumVersions> m_panCurveLut{};
    std::array<std::vector<scvb::PanCurvePoint>, kNumVersions> m_panCurvePoints{};
};

} // namespace scvb::output
