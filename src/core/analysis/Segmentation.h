// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

#include "analysis/EnergyVad.h"

namespace scvb::analysis
{

// ============================================================================
// 段语义字段(02-dsp-spec §3.5;params-v0 v1.1 [J34])
// ============================================================================

// [J34] origin 三值:auto / user_edited / user_created。manual_edited 已被
// origin+locked 替换,不再派生(ADR-008 v1.1)。
enum class Origin : uint8_t
{
    Auto = 0, // auto
    UserEdited = 1, // user_edited
    UserCreated = 2, // user_created
};

// 分析层段数据模型(02 §3.5;与 params-v0 v1.1 [J34] segments 字段对齐)。
// pan / volDb 为 [J34] 持久化字段;captureEpoch / analysisEpoch / loudnessLufs /
// energyLinear 为分析层内部字段(state 编码由 04 文档定)。
struct AnalysisSegment
{
    int64_t t0Samples = 0;
    int64_t t1Samples = 0;
    double pan = 0.0; // [J34] pan(-100..100)
    double volDb = 0.0; // [J34] vol_db
    Origin origin = Origin::Auto;
    bool locked = false;
    uint64_t captureEpoch = 0;
    uint64_t analysisEpoch = 0;
    double loudnessLufs = 0.0;
    double energyLinear = 0.0;

    int64_t length() const { return t1Samples - t0Samples; }

    // 用户段:自动重分析永不覆盖(ADR-008「不覆盖 manual / 显式解锁」语义)。
    bool isUserSegment() const { return origin != Origin::Auto || locked; }
};

// 用户删除记录,防复活(02 §3.5)。未入宪:params v1.1 [J34] 只定义 origin+locked;
// 本结构是计划层数据结构,持久化唯一落点 = 03 §6.1 CRVS 每轨记录
// excluded_ranges[](u32 count + count×{i64 t0, i64 t1}),由 T19 编码。
struct ExcludedRange
{
    int64_t t0 = 0;
    int64_t t1 = 0;
};

// ============================================================================
// S1 谷切分(02 §3.2)
// ============================================================================

struct SegmentationParams
{
    double maxSegmentS = 8.0; // 02 §0.3 maxSegment_s(3..20)
    double minSegmentMs = 120.0; // 02 §0.3 segmentation.min_segment_ms
    double sensitivity = 50.0; // 02 §0.3 segmentation.sensitivity(0..100)
    double w1 = 1.0; // depth 权重
    double w2 = 0.3; // valleyWidthMs/100 权重
    double w3 = 0.5; // breathScore 权重
    int smoothHops = 5; // movingAverage(ℓ, 5 hop)= 50ms

    // sensitivity s → minDepth(dB):minDepth = 6 · 2^((50−s)/50)(02 §3.2)。
    // s=0→12、50→6、100→3(越灵敏越容易切)。
    double minDepthDb() const;
};

// 候选谷(两遍 prominence 结果,02 §3.2 步骤 2)。
struct Valley
{
    int64_t hop = 0; // 谷底代表 hop(绝对时间线)
    double depthDb = 0.0; // prominence depth(02 §3.2 步骤 2b)
    double widthMs = 0.0; // 谷宽(半深全宽,ms)
    double breath = 0.0; // breathScore ∈ [0,1](02 §3.3)
    double score = 0.0; // w1·depth + w2·widthMs/100 + w3·breath
};

// 两遍法:返回段 [startHop, endHop) 内全部局部极小及其 depth(不筛 minDepth)。
// 侧峰边界 = 相邻局部极小(或段端),固定、不依赖筛选结果 —— 无自指(02 §3.2 步骤 2a)。
// l 指向 ℓ[0..N)(dB,绝对 hop 时间线,10ms/hop)。
std::vector<Valley> detectValleys(const float* l, int64_t startHop, int64_t endHop, const SegmentationParams& p);

struct ValleySplitResult
{
    std::vector<VadSegment> segments; // hop 域段序列(绝对时间线,按 start 排序、互不重叠)
    bool noNaturalCut = false; // 任一 >maxSegment 的段找不到自然切点(02 §3.2 步骤 4)
};

// S1:递归二分谷切分(02 §3.2)。l 指向 ℓ[0..N)(dB);[startHop, endHop) 为待切段。
ValleySplitResult splitValleys(const float* l, int64_t startHop, int64_t endHop, const SegmentationParams& p);

// ============================================================================
// S2 全局区间划分(02 §3.4)
// ============================================================================

struct GlobalInterval
{
    int64_t t0 = 0; // 样本
    int64_t t1 = 0; // 样本
    std::vector<int> tracks; // 活跃集合 A_t(轨下标,升序)
};

// S2:收集所有轨段边界 → 排序去重 → 碎片合并(对称差)→ 同活跃集合合并。
// trackSegments[t] 为该轨段列表(样本域,按 t0 排序、互不重叠)。
std::vector<GlobalInterval> buildGlobalIntervals(const std::vector<std::vector<AnalysisSegment>>& trackSegments,
                                                 int sampleRate, double minIntervalMs = 150.0);

// ============================================================================
// S3 manual 合并(02 §3.5;ADR-008)
// ============================================================================

struct MergeParams
{
    int sampleRate = 48000;
    double minSegmentMs = 120.0; // 挖洞后短于 minSegment → 丢弃(02 §3.5 步骤 c)
    double snapMs = 50.0; // 幸存候选边界吸附到 locked 边界 ±50ms(步骤 d)
};

// guard 扩展(02 §3.5 / 04 §4.3):R⁺ = R 两侧各扩 guardMs。
// 「延伸到最近静音帧」依赖上游静音检测;本卡离线只做固定 guardMs 扩展(静音判定由 T04 管线承载)。
void expandRegion(int64_t r0, int64_t r1, double guardMs, int sampleRate, int64_t& outR0, int64_t& outR1);

// S3:单轨重分析合并(02 §3.5 步骤 2a-2e)。返回该轨完整最终段列表(按 t0 排序、互不重叠)。
// oldSegments:该轨旧段(全时间线,按 t0 排序);newCandidates:S_new(已在 R⁺ 内,按 t0 排序);
// excludedRanges:该轨 excluded_ranges;rPlus0/rPlus1 = R⁺(样本)。
// 保证:origin!=Auto || locked 的段不丢;候选与任一 ExcludedRange 重叠 >50%(以候选自身长度计)→ 丢弃。
std::vector<AnalysisSegment> mergeTrackSegments(const std::vector<AnalysisSegment>& oldSegments,
                                                const std::vector<AnalysisSegment>& newCandidates,
                                                const std::vector<ExcludedRange>& excludedRanges, int64_t rPlus0,
                                                int64_t rPlus1, const MergeParams& p);

} // namespace scvb::analysis
