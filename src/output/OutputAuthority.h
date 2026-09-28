// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
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
// 可撤销的版本重命名([J05])。version_active 是 state(非自动化),值域 1..2,越界钳制 +
// warning 计数(03 §5.2,不静默取模)。
//
// 本类的改名今天只被 tests/core/test_version_params.cpp 调用;生产路径的改名走
// `ScvbOutputAudioProcessor::setVersionName` 的 CRVS 事务。
//
// 版本复制(03 §5.3)的生产路径是 `ScvbOutputAudioProcessor::copyVersion`(CRVS 事务),
// 本类只借出 `validateCopy` 判据;原先本类那份曲线层 `copyVersion` 零生产调用点,
// 已删([SL-510] / [J109])。
//
// 曲线不可变契约(PR #43 终审,硬前置):
//   setCurve 注入后的曲线对象必须不可变;重分析 = 新建 CurveEvaluator 对象再 setCurve 发布,
//   禁止对已注入对象调用 build()。曲线生命周期须覆盖音频线程可能读它的全部时段 —— VersionStore
//   与本类发布出的快照均以 std::shared_ptr<const CurveEvaluator> 持有曲线,旧曲线对象由引用它的
//   快照保活,音频线程绝不读悬垂/被原地改写的对象。
// 线程契约:setVersionActive/setCurve/setVersionName 均只可在消息线程调用 —— 它们只写
// 本类消息线程独占的配置并重建「不可变快照」,经 DspArbiter::publish 原子发布(release-store);
// 音频线程 processBlock 每 block acquire-load 一次、整 block 用同一份快照。旧快照由本类快照池
// 保活,直到音频线程经 DspArbiter::oldestHeldSeq 确认不再碰它,才在消息线程上释放([SL-445])。
// prepare 应在音频启动前调用(它触碰 m_handles 配置,不参与快照发布;
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

    // ---- [J157 / SL-447] pan 曲线拖动预览(消息线程;契约 §1.37 `previewPanCurve`)----
    // 预览只换**音频线程用的那张 G 表**:不写 m_panCurvePoints / m_panCurveLut(已提交的那张)、
    // 不碰 VersionStore、不进撤销栈、不落盘。打印器与 viz 根本不读 G(它们读的是各轨 pan/vol
    // 曲线),分析读的是 processor 的 crvsData_ —— 三者都只见到已提交的曲线。
    //
    // 发不发由 pump 决定,三道闸按顺序:
    //   ① 限速:距上一次预览发布 ≥ 当前间隔(≥ 50 ms ⇒ ≤ 20 Hz,J157 约束①);烘表贵时按
    //      实测耗时把间隔拉长,让烘表占消息线程 ≤ 10%(kPanCurvePreviewMaxDuty);
    //   ② 回收闸([SL-445] 之上):上一份预览快照音频线程还没越过(oldestHeldSeq < 它的 seq)
    //      ⇒ 不发新的。于是任何时刻**至多一份**预览快照未被确认 —— 音频线程停着(只读观察 /
    //      无时间线 / 无注入轨 / 宿主不调音频)时,拖多久池里也只多这一份,不比「只在松手时
    //      提交」更糟;音频在跑时每份在下一次发布时照 SL-445 的规则放掉;
    //   ③ 发不出去就**留着**(pending),调用方的定时器下一拍再来 —— 手指停在半路不再有
    //      pointermove,不重试的话停手前最后那一下永远听不到。
    static constexpr double kPanCurvePreviewMinIntervalMs = 50.0;
    static constexpr double kPanCurvePreviewMaxDuty = 0.10;

    // 记下「最新一份点表」并置待发,**不烘表**(桥调用几乎免费;一拍里多次请求合并成最后一份)。
    // version 不是当前激活版本 ⇒ staleVersion:UI 传的是**点表捕获时**的版本号,在「切版本已
    // 发出、回声未到」的窗口里它与引擎已不一致 —— 这份点表属于旧版本,落到新版本上就是跨版本串写。
    // 坏点(非有限 / 越界)⇒ badPoints:与 setPanCurve 同一道 arePanCurvePointsUsable。
    enum class PanCurvePreviewRequest
    {
        accepted,
        staleVersion,
        badPoints
    };
    PanCurvePreviewRequest requestPanCurvePreview(int version, const std::vector<scvb::PanCurvePoint>& points);
    // 撤掉预览:丢掉待发的那份;预览表若正在发布,按已提交那张重发一次(音频线程照常 30 ms 淡回)。
    void cancelPanCurvePreview();
    // 按上面三道闸尝试发一次。nowMs = 单调时钟毫秒(只用来限速)。返回 true = 仍有待发
    // (调用方保持重试定时器),false = 没有待发了。
    bool pumpPanCurvePreview(double nowMs);

    bool panCurvePreviewPending() const { return m_preview.pending; }
    // 预览表此刻是否在发布出去的快照里(音频线程听的是它,不是已提交那张)。
    bool panCurvePreviewLive() const { return m_preview.live; }
    std::shared_ptr<const scvb::PanCurveLut> panCurvePreviewLut() const { return m_preview.lut; }
    // 诊断 / 单测:烘过几次、发过几次、最近与最大一次烘表耗时、当前生效的限速间隔。
    struct PanCurvePreviewStats
    {
        std::uint64_t bakes = 0;
        std::uint64_t publishes = 0;
        double lastBakeMs = 0.0;
        double maxBakeMs = 0.0;
        double intervalMs = kPanCurvePreviewMinIntervalMs;
    };
    PanCurvePreviewStats panCurvePreviewStats() const { return m_preview.stats; }
    // 单测注入:烘表耗时的计时源(默认 steady_clock,毫秒)。只影响①里「按耗时拉长间隔」那一半。
    void setPanCurvePreviewBakeClock(std::function<double()> clock) { m_preview.bakeClock = std::move(clock); }

    // ---- 版本复制 §5.3 的判据(执行在 processor 的 CRVS 事务里)----
    // [SL-484] 只校验不执行:生产路径(processor 的 CRVS 复制)借这一份判据,不另写 PRINT 判断。
    scvb::engine::CopyVersionResult validateCopy(int src, int dst, scvb::engine::AuthorityMode mode) const
    {
        return m_versions.validateCopy(src, dst, mode);
    }

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

    // 活动版本**已提交**的 G 查表(消息线程;供单测对拍)。从没设过 → null。
    // [J157] 拖动预览在发布中时,音频线程用的是 panCurvePreviewLut(),不是这一张。
    std::shared_ptr<const scvb::PanCurveLut> activePanCurveLut() const;

    // 快照池当前条数(消息线程;单测钉「池有上界」用)。
    std::size_t snapshotPoolSize() const { return m_snapshotPool.size(); }

    int warningCount() const;
    bool isPrepared() const { return m_prepared; }

    // 音频线程:仲裁 + arm 平滑。engineAuthority = output_enabled(ARMED/PRINT 均 true)。
    std::array<scvb::engine::DspArbiter::TrackValues, kNumTracks> processBlock(bool engineAuthority, double tSec);

    // 音频线程:前进平滑器。
    std::array<scvb::engine::DspArbiter::TrackValues, kNumTracks> nextSample();

    const scvb::engine::DspArbiter& arbiter() const { return m_arbiter; }

private:
    void rebindSources();
    // [J157] 丢掉预览(待发 + 在发布中的那张),**不**重发快照;调用方随后自己 rebind。
    // 返回:预览表此前是否在发布中(调用方据此决定要不要重发)。
    bool dropPanCurvePreview();

    scvb::params::ParamHandles m_handles;
    scvb::engine::DspArbiter m_arbiter;
    scvb::engine::VersionStore m_versions;
    juce::UndoManager m_undoManager;
    bool m_prepared = false;
    // 快照池:保活已发布、音频线程可能仍在读的快照。按发布顺序排列(seq 严格递增)。
    //
    // [SL-445] 此前是「进程寿命保活,绝不释放」,而自 SL-442 起每条快照多钉住一份 128 KiB 的
    // `PanCurveLut`(每张不同曲线一份)—— Q 滑杆断续拖 10 分钟约 500 MB 只增不减。现在每次发布
    // 之后在消息线程上释放 seq < DspArbiter::oldestHeldSeq() 的快照(判据与安全性论证见该函数)。
    // 旧 LUT / 旧曲线只由快照的 shared_ptr 持有时随之释放,析构全在消息线程,音频线程零分配零释放。
    // ⚠ 已知局限:只有 **authority 的 processBlock 在跑**时确认才前进。宿主不调音频、或走直通分支
    //   (只读观察 / 无时间线 / 负 t0 / 无注入轨,见 OutputProcessor::renderSpan)期间,池照旧只增;
    //   一旦再跑过一块,下次发布就把积压一次放掉。回收只挂在发布上,不另起定时器:停手时
    //   最后一次发布那一刻尚未确认的几条,要等下一次发布才放。
    std::deque<std::unique_ptr<scvb::engine::DspArbiter::Snapshot>> m_snapshotPool;
    std::uint64_t m_nextSnapshotSeq = 0; // 最近一次发布的 seq;首份为 1(0 留给「不记账」)
    // 每版本一张 G 查表(pan_curve 是 per-version,不是 per-track)+ 烘它时用的点列表。
    // 留着点列表是为了 setPanCurve 的 no-op 判定 —— 见该函数注释。
    std::array<std::shared_ptr<const scvb::PanCurveLut>, kNumVersions> m_panCurveLut{};
    std::array<std::vector<scvb::PanCurvePoint>, kNumVersions> m_panCurvePoints{};

    // [J157 / SL-447] 拖动预览(消息线程独占;语义见 requestPanCurvePreview 一组的注释)。
    struct PanCurvePreview
    {
        // 待发的那份(pending=true 时有效)。
        bool pending = false;
        int pendingVersion = 0;
        std::vector<scvb::PanCurvePoint> pendingPoints;
        // 在发布中的那张(live=true 时 rebindSources 用它替掉已提交那张)。lut 可为 null:
        // 点表与已提交的相同 ⇒ 直接借用已提交那张(从没画过曲线时那张就是 null ⇒ G≡0)。
        bool live = false;
        int liveVersion = 0;
        std::vector<scvb::PanCurvePoint> livePoints;
        std::shared_ptr<const scvb::PanCurveLut> lut;
        // 限速与回收闸的记账。**跨手势保留**(cancel / 提交都不清):
        // lastSeq 清零会让下一次手势的首份预览绕过回收闸 —— 音频线程停着时每个手势就多留
        // 一份,「至多一份」退化成「每手势一份」。
        std::uint64_t lastSeq = 0; // 最近一次预览发布的快照 seq;0 = 从没发过
        double lastPublishMs = 0.0;
        PanCurvePreviewStats stats;
        std::function<double()> bakeClock; // 空 ⇒ steady_clock
    };
    PanCurvePreview m_preview;
};

} // namespace scvb::output
