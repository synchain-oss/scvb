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

// [J146] 自适应基准 —— 02 §2.2 第 1、2 步里**与 VadParams 无关**的那一半:帧响度 ℓ[k]
// (2-hop 平滑后转 dB)、底噪基准 F(全 hop P10)、活跃基准 A(活跃候选子集 P95,子集空则
// 退化为全 hop P95)、子集是否为空。它只取决于输入的 kw_ms,拖阈值滑杆时一个字节都不变。
//
// 为什么拆出来:拖动档预览(契约 §1.18「每次调用即时重判决」)每次调用都要按新参数重跑,
// 而整条流程里最贵的正是这一半(两次分位数 = 两次整段排序,外加每 hop 一次 log10)。
// 算一次缓存住,后面每次只跑与参数有关的那一半(阈值 → 状态机 → 后处理),单次代价从
// O(n log n) 降到 O(n)。`runEnergyVad` 本身就是「算基准 + 跑那一半」两步,所以预览与
// 分析流水线**走的是同一份实现**,不是两份「差不多的」。
struct VadBaseline
{
    std::vector<float> l; // ℓ[k](dB),长度 = 输入 hop 数
    float F = 0.0f; // 底噪基准
    float A = 0.0f; // 活跃基准
    bool activeEmpty = true; // 活跃候选子集 S 是否为空(决定是否走 §2.6 守卫)
};

// 算基准(与参数无关的那一半)。n == 0 时 ℓ 为空。
VadBaseline computeVadBaseline(const float* kwMs, std::size_t n);

// 在已算好的基准上跑与参数有关的那一半(阈值 → §2.3 状态机 → P1..P4)。语义与 `runEnergyVad`
// 逐字相同:`runEnergyVad(kw, n, h, p, post)` ≡
// `runEnergyVadOnBaseline(computeVadBaseline(kw, n), h, p, post)`(后者就是前者的实现)。
VadResult runEnergyVadOnBaseline(const VadBaseline& base, int64_t firstHop, const VadParams& p,
                                 float* posteriorOut = nullptr);

// 能量域 VAD v1 全流程(02-dsp-spec §2):前处理与自适应基准 → 双阈值状态机 →
// 后处理 P1(丢短)→ P2(padding)→ P3(并重叠)→ P4(并间隙)。
// 输入:某轨某选区的 kw_ms[0..n)(K-weighted mean-square,线性能量;hop=10ms)。
// firstHop = 选区首 hop 在绝对时间线上的序号。posteriorOut 可 null(写截断后验 p[k],§2.4)。
// 结果段以绝对 hop 序号表达,截断到选区边界,不产生越界 hop。
// [J146] 实现 = `computeVadBaseline` + `runEnergyVadOnBaseline` 两步(见上)。
VadResult runEnergyVad(const float* kwMs, std::size_t n, int64_t firstHop, const VadParams& p,
                       float* posteriorOut = nullptr);

// 帧响度 ℓ(02 §0.2):ℓ = −0.691 + 10·log10(max(z, 1e−12)) [LUFS];z = 线性 K 加权均方能量。
//
// [SL-382] **这是 ℓ 的唯一口径** —— VAD 状态机(§2.2 第 1 步)与谷切分(§3.2 的 ℓ 输入)
// 共用这一份,不是「各写一份差不多的」。要害在**能量下限 1e−12**:它决定「一个 kw==0 的
// hop 到底有多深」。`AnalysisPipeline.cpp` 的 `lufsFromMeanKw` 是**上报口径**(§2.8),
// 对 m<=0 回 −120、对极小正数**不设下限**(10·log10(1e−30) = −300)。任一种落进 §3.2
// 第 1 步的 `movingAverage(ℓ, 5 hop)`,单个数字静音 hop 都会被摊成一个几十 dB 的**假谷** ——
// 足以越过任何 minDepth(值域 [3,12])⇒ 凭空切一刀。所以谷切分**不得**复用上报口径。
// (`lufsFromMeanKw` 保持不动:它的 −120 地板是 §2.8 的上报语义,[SL-257] 已按它对拍。)
float frameLoudnessDb(double meanSquareEnergy);

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
