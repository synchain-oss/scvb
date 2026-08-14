// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "analysis/IVadBackend.h"

namespace scvb::analysis
{

// VAD 参数(02-dsp-spec §2.1 / §0.3)。
// paddingPreMs/paddingPostMs 为双字段默认 120/200(params-v0 v1 [J23]);无 padding_ms 单值、
// 无 pre=0.6·padding_ms 派生规则。
struct VadParams
{
    float thresholdDb = 30.f; // onDepth: T_on = A − thresholdDb
    float hysteresisDb = 6.f; // T_off = T_on − hysteresisDb
    int hangoverMs = 250; // 最短出段(挂起)
    int paddingPreMs = 120; // 前留白([J23] 独立字段)
    int paddingPostMs = 200; // 后留白([J23] 独立字段)
    int attackFrames = 2; // 内部:onset 攻击帧数
    int minSegmentMs = 120; // 来自 segmentation.min_segment_ms
    int mergeGapMs = 150; // 内部:换气容忍的一半
};

// VAD 段 [startHop, endHop),hop 单位(绝对时间线)。
struct VadSegment
{
    int64_t startHop = 0;
    int64_t endHop = 0;
};

// 动态范围守卫分支(02-dsp-spec §2.6;仅当活跃候选子集 S 为空时触发)。
enum class VadGuard
{
    Normal, // S 非空,正常执行
    LoudRegion, // A > −30 LUFS → 整选区一个有声段
    SilentRegion, // A < −50 LUFS → 空段列表
    NarrowDynamicRange, // −50 ≤ A ≤ −30 → 照常执行 + 警告
};

struct VadResult
{
    std::vector<VadSegment> segments;
    VadGuard guard = VadGuard::Normal;

    // 是否触发动态范围守卫(需 UI 警告)。
    bool warned() const { return guard != VadGuard::Normal; }

    // UI 警告文案(02 §2.6);未触发时返回 nullptr。
    const char* warningMessage() const;
};

// 能量域 VAD v1 全流程(02-dsp-spec §2):前处理与自适应基准 → 双阈值状态机 →
// 后处理 P1(丢短)→ P2(padding)→ P3(并重叠)→ P4(并间隙)。
// 输入:某轨某选区的 kw_ms[0..n)(K-weighted mean-square,线性能量;hop=10ms)。
// firstHop = 选区首 hop 在绝对时间线上的序号。posteriorOut 可 null(写截断后验 p[k],§2.4)。
// 结果段以绝对 hop 序号表达,截断到选区边界,不产生越界 hop。
VadResult runEnergyVad(const float* kwMs, std::size_t n, int64_t firstHop, const VadParams& p,
                       float* posteriorOut = nullptr);

// EnergyVad:IVadBackend 的 v1 实现(能量域,离线)。
class EnergyVad : public IVadBackend
{
public:
    const char* name() const override { return "energy_v1"; }
    bool worksFromFeatures() const override { return true; }

    void computeFromFeatures(const float* kwMs, std::size_t numHops, const VadParams& p,
                             float* posteriorRawOut) override;

    // v2 流式路径:energy_v1 不实现。
    bool streamBegin(double sampleRate) override
    {
        (void)sampleRate;
        return false;
    }
    void streamProcess(const float* mono, std::size_t numSamples) override
    {
        (void)mono;
        (void)numSamples;
    }
    std::size_t streamDrainPosterior(float* out, std::size_t maxHops) override
    {
        (void)out;
        (void)maxHops;
        return 0;
    }
    void streamEnd() override {}
};

} // namespace scvb::analysis
