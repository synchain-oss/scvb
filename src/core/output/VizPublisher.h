// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// VizPublisher —— viz 段的 Output 侧发布器([T44/J75])。
//
// 线程口径(硬铁律):**只在消息线程 [M] 调用**。`processBlock`(音频线程 [A])对 viz 段零写入 ——
// 本类不提供任何可从 [A] 调用的入口,VizPlane 也只被本类持有。
//
// 发布节拍(分频):
//   - 帧头标量(playhead / 循环区 / 掩码 / 时刻)实得 **30 Hz**(kPublishIntervalMs 是闸门下限 25ms,
//     真正定频的是驱动量化;SL-192 升频,原为 250ms/4Hz)。驱动它的是 `OutputProcessor` 的**独立 60Hz 定时器**
//     `vizTimer_`,不再搭主 25Hz [M] tick —— 25Hz 驱动不出 30Hz 的发布率。
//   - 车道 + 位图 + 轨色(15×1024 次曲线求值)**按需重算**:CRVS 修订变化 / 活动版本切换 /
//     窗口跨度变化 / 距上次重算 ≥ kLaneRefreshMaxMs(30s)四者之一触发。**升频不影响这一条** ——
//     稳态下一帧只写约 380 字节(实测 p50 1.20us),消息线程负担仍可忽略。
//
// 窗口口径:起点恒 0(工程起点),跨度 = max(最大分段末端, playhead+1, 最小跨度 60s) 向上取整到
// kWindowQuantumSec(30s)边界 —— 量化是为了让跨度在播放中保持稳定,避免每帧重算车道。
// 「无末端」分段(setTrackManual 的 t1 = 1<<40 哨兵,注释「真末端由宿主时间线提供」)只以其 t0
// 参与跨度计算,并在覆盖位图中一路覆盖到窗口末端。

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "../engine/CurveEvaluator.h"
#include "../engine/PlayheadShot.h"
#include "../ipc/VizPlane.h"
#include "../state/StateCodec.h"

namespace scvb::output
{

// 「无末端」分段哨兵下界:CRVS 里 t1 = 1<<40 表示「覆盖到时间线末端」(真末端由宿主提供)。
inline constexpr std::int64_t kVizOpenEndedT1 = static_cast<std::int64_t>(1) << 40;

// 发布器的一次输入快照(全部由调用方在 [M] 持锁期间填好;本类不持有任何指针跨调用)。
struct VizPublishInput
{
    const scvb::state::CrvsData* crvs = nullptr; // 分段真源(覆盖位图口径)
    std::array<const scvb::CurveEvaluator*, scvb::state::kNumTracks> curves{}; // 活动版本曲线(pan 求值)
    scvb::u32 versionActive = 1; // 1|2
    scvb::u32 enabledMask = 0; // bit{N-1} = 该轨启用
    scvb::u32 stereoMask = 0; // bit{N-1} = 该轨立体声源
    scvb::u32 leadMask = 0; // bit{N-1} = 该轨 lead_lock(分布图柱顶绿帽,同 Tab1 规格)
    scvb::engine::PlayheadPod playhead{};
    scvb::u32 crvsRevision = 0; // CRVS 修订号(变化即重算车道)
    double sampleRate = 0.0;
    // 每轨 width(engineering 0..100,来自参数 raw atomic)。分布图的张开横线要它。
    std::array<float, scvb::state::kNumTracks> widthPct{};
    // 每轨轨名(UTF-8;发布器按 UTF-8 边界截断到 kVizLabelBytes-1)。图例要它。
    std::array<std::string, scvb::state::kNumTracks> label{};
    // 轨名/宽度不进 crvsRevision,单独给一个修订号驱动「车道块」重写(轨名随车道一起落段)。
    scvb::u32 metaRevision = 0;
};

class VizPublisher
{
public:
    // 帧头发布的**闸门下限**(ms)。实得发布率 = **30 Hz**(SL-192;此前 250ms = 4Hz)。
    //
    // ⚠ 25 不是「1000/25 = 40Hz」的意思 —— 它是一条**下限**,真正定出频率的是驱动定时器的
    // 量化:`kPublishTimerHz`(60Hz)每拍 16.67ms,发布只能落在它的整数倍上
    // (16.7 / 33.3 / 50 …)。闸门取 25 ⇒ 一拍(16.7)够不着、两拍(33.3)够得着
    // ⇒ 稳定 **33.3ms = 30.0Hz**。
    //
    // 为什么**不能**把闸门写成 33(= 目标周期本身):那样两拍(33.33ms)只比闸门多 0.33ms,
    // 余量几乎为零 —— 定时器稍有抖动,那一拍就够不着,于是顺延到三拍 = 50ms,频率塌到 20Hz。
    // 这正是本卡反复栽的「同频异相」的第三种长相,而且它**只在有抖动的真机上**才现形:
    // 理想时钟下数出来是漂亮的 30 帧。闸门必须**夹在两个可达节拍之间并留出余量**:
    //   一拍 + 抖动上限 < 闸门 < 两拍 − 抖动上限   ⇒   20.7 < 25 < 29.3(按 ±4ms 估)
    // 用例把这条规则本身钉住了(test_viz_plane.cpp 的 `[rate]`:带抖动跑,同频驱动必红)。
    //
    // 升频依据不是估算,是实测(`tests/core/test_viz_publish_cost.cpp`,Release/本机):
    //   • 稳态一帧 `tick()` p50 **1.2-1.6 µs** ⇒ 30Hz = **0.005% 一颗核**;
    //   • 调用方的输入采集(15 轨名 + FNV)p50 **0.2-0.3 µs** ⇒ 30Hz = 0.001%;
    //   • 车道重算帧 p50 **~0.5 ms**,但它的触发条件(CRVS 修订 / 活动版本 / 窗口跨度 /
    //     metaRevision + 30s 兜底)**与本频率无关** —— 升频一次都不会多算。
    // 关键事实:段是 64KB,**但稳态一帧只写约 380 B**(15 个帧头标量 + 15×3 个每轨当前值);
    // 整块 64KB 只在 `writeLanes` 那一帧写。按「64KB × 30Hz ≈ 2MB/s」估预算会高估三个数量级,
    // 实际稳态写入量 ≈ 11 KB/s。
    static constexpr scvb::u64 kPublishIntervalMs = 25; // 闸门下限;实得 30 Hz(60Hz 驱动的两拍)

    // 驱动本发布器的定时器频率(`OutputProcessor` 的独立 `vizTimer_` 取它)。
    // **必须 ≥2× 发布频率**:驱动与闸门同频不同相时,抖动会周期性把整帧丢掉 ——
    // 那正是 SL-192 在读方两级各栽了一次的坑。60Hz 驱动 33ms 闸门 = 稳定每两拍一帧 = 30.0Hz。
    static constexpr int kPublishTimerHz = 60;
    // 车道重算的**兜底**间隔。车道只依赖 CRVS 修订 / 活动版本 / 窗口跨度 / 轨名(metaRevision),
    // 四者全部显式跟踪 —— 兜底只为 metaRevision 的 FNV-1a 64 位哈希碰撞留一条后路,
    // 而那个概率可以忽略。所以放到 30s:重算一次要 15360 次曲线求值,1s 兜底等于把它变成常态开销
    // (I4);真出碰撞最多迟 30s 生效一次轨名,不影响任何数据正确性。
    static constexpr scvb::u64 kLaneRefreshMaxMs = 30000;
    static constexpr double kMinWindowSec = 60.0; // 空工程也给一条 60s 的轴
    static constexpr double kWindowQuantumSec = 30.0; // 跨度量化步长
    static constexpr double kMaxWindowSec = 24.0 * 3600.0; // 跨度上限(防哨兵/脏数据炸轴)

    VizPublisher(scvb::ISegmentBackend& backend, scvb::u32 group);

    scvb::InitResult open() { return plane_.open(); }
    // 只换段指向,不建段(建/释放由调用方按 claim 态裁决 —— [J66] 同组只有 kActive 那个 Output 写)。
    void setGroup(scvb::u32 group);
    scvb::InitResult changeGroup(scvb::u32 group);
    void release();
    bool isOpen() const { return plane_.isOpen(); }
    const scvb::VizPlane& plane() const { return plane_; }

    // 本拍是否到发布闸门。调用方据此**跳过输入采集**(采 15 个轨名 = 15 次堆分配),
    // 而不是采完再被 tick() 丢掉 —— 驱动 60Hz、发布 30Hz,一半的拍子在这里早退。
    bool due(scvb::u64 nowMs) const { return !everPublished_ || nowMs - lastPublishMs_ >= kPublishIntervalMs; }

    // [M] 每 tick 调用(生产路径 = 60Hz 的 vizTimer_,内部按 kPublishIntervalMs 闸门分频)。
    // 返回 true = 本次真的发布了一帧。
    bool tick(scvb::u64 nowMs, const VizPublishInput& in);

    // 测试内省:上次发布的快照(只读)。
    const scvb::VizSnapshot& lastSnapshot() const { return *snap_; }
    scvb::u64 publishCount() const { return publishCount_; }
    scvb::u64 laneRebuildCount() const { return laneRebuildCount_; }

private:
    // 计算本帧窗口跨度(样本);返回 0 = 无有效窗口(未 prepare)。
    scvb::u64 computeWindowSpan(const VizPublishInput& in) const;
    // 重算车道 + 覆盖位图 + 轨色 + covered 掩码。
    void rebuildLanes(const VizPublishInput& in, scvb::u64 spanSamples);

    scvb::VizPlane plane_;
    std::unique_ptr<scvb::VizSnapshot> snap_; // ≈32KB,堆持有(勿上栈)

    scvb::u64 lastPublishMs_ = 0;
    scvb::u64 lastLaneMs_ = 0;
    bool everPublished_ = false;
    bool everBuiltLanes_ = false;
    scvb::u32 lastCrvsRevision_ = 0;
    scvb::u32 lastMetaRevision_ = 0;
    scvb::u32 lastVersionActive_ = 0;
    scvb::u64 lastSpanSamples_ = 0;
    scvb::u64 publishCount_ = 0;
    scvb::u64 laneRebuildCount_ = 0;
};

} // namespace scvb::output
