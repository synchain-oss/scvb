// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Reanalysis —— 局部重分析(04 §4.3 / 02 §3.5 / ADR-007/008)。
// 失效粒度 = 轨 × 时间区间(ADR-007 冻结):只对 T×R⁺ 重算并 splice 写回,其余数据
// 一个字节不动。三件能力:
//   1. guard 扩展:R → R⁺(两侧各扩 max(1.0s, 延伸到最近静音帧));
//   2. splice:候选在 R⁺ 边界截断,替换与 R⁺ 相交的旧段,外侧半段继承原值;
//   3. manual 挖洞:origin≠auto 或 locked 的段不覆盖;候选与占位段重叠 >50% 整条
//      丢弃、≤50% 挖洞让位(细则唯一规格 = Segmentation::mergeTrackSegments)。
// 重算流水线(VAD→S1→S2→assign→balance)由调用方/引擎组装并以 RecomputeFn 注入;本模块
// 只负责失效粒度 + splice + 不变式,不绑定具体 DSP。

#include <cstdint>
#include <functional>
#include <vector>

#include "analysis/Segmentation.h"

namespace scvb::analysis
{

// ============================================================================
// guard:静音感知的 R⁺ 扩展(04 §4.3)
// ============================================================================

// 静音判据:静音帧 = VAD 后验低于退出阈值且持续 > minPauseMs。loudnessDb 为 ℓ[dB]
// (VAD 后验/平滑包络);silenceExitDb = T_off(由 VAD 配置导出)。
struct SilenceWindow
{
    const float* loudnessDb = nullptr; // ℓ[k] dB,hop=10ms,绝对时间线
    int64_t firstHop = 0; // loudnessDb[0] 对应的绝对 hop 序号
    int64_t numHops = 0; // 可用 hop 数(0 = 无静音信息 → 退化为固定 guardMs)
};

struct GuardParams
{
    int sampleRate = 48000;
    double guardMs = 1000.0; // 最小 guard(两侧各 ≥1.0s,04 §4.3)
    float silenceExitDb = -60.0f; // ℓ < exitDb → 静音 hop
    double minPauseMs = 250.0; // 静音持续 > minPauseMs 才算「静音帧」
};

// R → R⁺:两侧各扩 max(guardMs, 延伸到最近静音帧)。「最近静音帧」= 严格在 R 外侧、
// 长度 ≥ minPauseMs 的连续静音 run,取最靠近 R 边界的一端:
//   左 guard = max(guardMs, r0 − 最近左静音 run 的右端);
//   右 guard = max(guardMs, 最近右静音 run 的左端 − r1)。
// 无静音信息(SilenceWindow.numHops==0)或找不到静音 run → 该侧退化为固定 guardMs。
// 结果截断到窗口覆盖的样本范围 [firstHop·hopSamples, (firstHop+numHops)·hopSamples]。
void guardExtend(const SilenceWindow& win, int64_t r0, int64_t r1, const GuardParams& p, int64_t& outR0,
                 int64_t& outR1);

// ============================================================================
// 模型与请求
// ============================================================================

// 局部重分析请求:轨集合 T × 时间区间 R(样本,半开)。
struct ReanalysisRequest
{
    std::vector<uint32_t> tracks; // T(轨下标;越界轨被忽略,绝不触碰其他数据)
    int64_t startSample = 0; // R 起点
    int64_t endSample = 0; // R 终点(半开)
};

// 单版本分析结果(ADR-005:版本独立;[J59] 15 轨)。segments[t] 按 t0 排序、互不重叠;
// excluded[t] 为该轨该版本的删除记录(防复活,03 §6.1 CRVS)。局部重分析不改 excluded。
struct VersionAnalysis
{
    std::vector<std::vector<AnalysisSegment>> segments; // [track]
    std::vector<std::vector<ExcludedRange>> excluded; // [track]

    uint32_t numTracks() const noexcept { return static_cast<uint32_t>(segments.size()); }
    void resize(uint32_t n)
    {
        segments.resize(n);
        excluded.resize(n);
    }
};

// 特征提供器:guard 静音检测用。返回 (track) 的 ℓ[dB] 窗口;空窗口 → 该轨退化为固定 guardMs。
using FeatureProvider = std::function<SilenceWindow(uint32_t track)>;

// 重算回调:对 (track, R⁺) 重跑流水线,产出候选段(样本域,pan/vol 已解)。必须是纯函数
// (同输入同输出)→ 幂等。候选允许越出 R⁺,splice 会在 R⁺ 边界截断(04 §4.3)。
using RecomputeFn = std::function<std::vector<AnalysisSegment>(uint32_t track, int64_t rPlus0, int64_t rPlus1)>;

// ============================================================================
// splice + manual 挖洞
// ============================================================================

// 单轨 splice:候选裁剪到 R⁺ 后交给 mergeTrackSegments(占位保留 / ExcludedRange 防复活 /
// 挖洞 / locked 吸附 / 邻接同值合并)。R⁺ 外侧段逐字节不动(不变式 2)。
std::vector<AnalysisSegment> spliceTrack(const std::vector<AnalysisSegment>& oldSegments,
                                         const std::vector<AnalysisSegment>& candidates,
                                         const std::vector<ExcludedRange>& excludedRanges, int64_t rPlus0,
                                         int64_t rPlus1, const MergeParams& p);

// ============================================================================
// 编排
// ============================================================================

// 局部重分析:对每个 track∈T 依次 guard → recompute → splice;只写 T×R⁺,其余逐字节不变
// (不变式 2/3)。T 外轨与 T 内 R⁺ 外段保持原值;origin≠auto 或 locked 段保留(不变式 1);
// ExcludedRange 内候选不复活。
VersionAnalysis reanalyze(const VersionAnalysis& in, const ReanalysisRequest& req, const FeatureProvider& features,
                          const RecomputeFn& recompute, const GuardParams& guard, const MergeParams& merge);

// 确定性字节序列化(逐字节对比用;同输入同平台逐字节一致)。字段顺序固定,不含结构体
// padding(逐字段写出,避免未初始化 padding 字节破坏逐字节比较)。
std::vector<uint8_t> serializeVersion(const VersionAnalysis& v);

} // namespace scvb::analysis
