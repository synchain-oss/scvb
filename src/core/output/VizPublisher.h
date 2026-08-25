// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// VizPublisher —— viz 段的 Output 侧发布器([T44/J75])。
//
// 线程口径(硬铁律):**只在消息线程 [M] 调用**。`processBlock`(音频线程 [A])对 viz 段零写入 ——
// 本类不提供任何可从 [A] 调用的入口,VizPlane 也只被本类持有。
//
// 发布节拍(分频):
//   - 帧头标量(playhead / 循环区 / 掩码 / 时刻)每 kPublishIntervalMs = 250ms 刷一次(**4 Hz**),
//     与既有 4Hz 心跳闸门同款(OutputProcessor::timerCallback 的 lastHeartbeatMs_ 模式)。
//   - 车道 + 位图 + 轨色(15×1024 次曲线求值)**按需重算**:CRVS 修订变化 / 活动版本切换 /
//     窗口跨度变化 / 距上次重算 ≥ kLaneRefreshMaxMs(1s)四者之一触发。稳态下 4Hz 只写 128 字节帧头,
//     消息线程负担可忽略。
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
    static constexpr scvb::u64 kPublishIntervalMs = 250; // 4 Hz 帧头
    static constexpr scvb::u64 kLaneRefreshMaxMs = 1000; // 车道最长 1s 强制重算
    static constexpr double kMinWindowSec = 60.0; // 空工程也给一条 60s 的轴
    static constexpr double kWindowQuantumSec = 30.0; // 跨度量化步长
    static constexpr double kMaxWindowSec = 24.0 * 3600.0; // 跨度上限(防哨兵/脏数据炸轴)

    VizPublisher(scvb::ISegmentBackend& backend, scvb::u32 group);

    scvb::InitResult open() { return plane_.open(); }
    scvb::InitResult changeGroup(scvb::u32 group);
    void release();
    bool isOpen() const { return plane_.isOpen(); }
    const scvb::VizPlane& plane() const { return plane_; }

    // [M] 每 tick 调用(25Hz 亦可,内部按 250ms 闸门分频)。返回 true = 本次真的发布了一帧。
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
