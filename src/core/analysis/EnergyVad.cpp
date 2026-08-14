// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/EnergyVad.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace scvb::analysis
{

namespace
{

// 02 §0.2:帧响度 = −0.691 + 10·log10(z) [LUFS];z 为线性均方能量。
constexpr double kLufsOffset = -0.691;
constexpr double kEnergyFloor = 1e-12;

// 02 §2.2 第 4 步动态范围守卫阈值。
constexpr float kLoudRegionThreshold = -30.f; // A > −30
constexpr float kSilentRegionThreshold = -50.f; // A < −50
constexpr float kTFloorMin = -70.f; // T_floor 下限

// 02 §2.2 第 2 步:活跃候选子集判定 margin(F + 12)。
constexpr float kActiveMargin = 12.f;

double clampedEnergy(double e)
{
    return e < kEnergyFloor ? kEnergyFloor : e;
}

float frameLoudness(double energy)
{
    return static_cast<float>(kLufsOffset + 10.0 * std::log10(clampedEnergy(energy)));
}

// 02 §2.2 第 1 步:2-hop 滑动平均平滑 + 转 dB。k=0 用自身(无前 hop)。
void computeFrameLoudness(const float* kwMs, std::size_t n, std::vector<float>& l)
{
    l.resize(n);
    for (std::size_t k = 0; k < n; ++k)
    {
        double e;
        if (k == 0)
        {
            e = static_cast<double>(kwMs[0]);
        }
        else
        {
            e = 0.5 * (static_cast<double>(kwMs[k - 1]) + static_cast<double>(kwMs[k]));
        }
        l[k] = frameLoudness(e);
    }
}

// 分位数(nearest-rank:rank = ceil(p/100 · N),1-based)。测试向量为二值/常量,
// 与线性插值口径同值;对连续值确定性可复现。
float percentile(const std::vector<float>& values, double p)
{
    if (values.empty())
        return 0.0f;
    std::vector<float> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    std::size_t rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    if (rank == 0)
        rank = 1;
    if (rank > sorted.size())
        rank = sorted.size();
    return sorted[rank - 1];
}

struct VadModel
{
    std::vector<float> l; // ℓ[k](dB)
    float F = 0.0f; // 底噪基准(全 hop P10)
    float A = 0.0f; // 活跃基准(活跃候选子集 P95,或退化全 hop P95)
    float ton = 0.0f; // T_on
    float toff = 0.0f; // T_off
    bool activeEmpty = true; // 活跃候选子集 S 是否为空
};

// 02 §2.2 第 2、3 步:自适应基准与阈值。
VadModel buildModel(const float* kwMs, std::size_t n, const VadParams& p)
{
    VadModel m;
    computeFrameLoudness(kwMs, n, m.l);

    m.F = percentile(m.l, 10.0);

    const float activeFloor = std::max(m.F + kActiveMargin, kTFloorMin);
    std::vector<float> activeL;
    activeL.reserve(n);
    for (const float x : m.l)
    {
        if (x > activeFloor)
            activeL.push_back(x);
    }
    m.activeEmpty = activeL.empty();
    m.A = m.activeEmpty ? percentile(m.l, 95.0) : percentile(activeL, 95.0);

    // 阈值:保证 T_on ≥ T_off + 1(滞回方向)。
    float ton = m.A - p.thresholdDb;
    const float tfloor = std::max(m.F + 6.0f, kTFloorMin);
    float toff = std::max(ton - p.hysteresisDb, tfloor);
    ton = std::max(ton, toff + 1.0f);
    m.ton = ton;
    m.toff = toff;
    return m;
}

} // namespace

const char* VadResult::warningMessage() const
{
    switch (guard)
    {
    case VadGuard::LoudRegion:
        return "该轨几乎全程有声";
    case VadGuard::SilentRegion:
        return "该轨疑似无人声";
    case VadGuard::NarrowDynamicRange:
        return "动态范围过小,识别可能不可靠";
    case VadGuard::Normal:
        return nullptr;
    }
    return nullptr;
}

VadResult runEnergyVad(const float* kwMs, std::size_t n, int64_t firstHop, const VadParams& p, float* posteriorOut)
{
    VadResult result;
    if (n == 0)
        return result;

    const VadModel m = buildModel(kwMs, n, p);

    // 后验 p[k](02 §2.4):p_raw 不截断、p 截断到 [0,1] 存 features.vad_posterior[]。
    // 无论守卫分支如何,后验均按当前阈值可算(供 UI 热图)。
    if (posteriorOut != nullptr)
    {
        const float denom = m.ton - m.toff; // ≥ 1(阈值步骤保证)
        for (std::size_t k = 0; k < n; ++k)
        {
            const float raw = (m.l[k] - m.toff) / denom;
            posteriorOut[k] = std::max(0.0f, std::min(1.0f, raw));
        }
    }

    // 02 §2.2 第 4 步:动态范围守卫(仅当 S 为空)。
    if (m.activeEmpty)
    {
        if (m.A > kLoudRegionThreshold)
        {
            result.guard = VadGuard::LoudRegion;
            result.segments.push_back({firstHop, firstHop + static_cast<int64_t>(n)});
            return result;
        }
        if (m.A < kSilentRegionThreshold)
        {
            result.guard = VadGuard::SilentRegion;
            return result; // 空段列表
        }
        result.guard = VadGuard::NarrowDynamicRange; // 照常执行 + 警告
    }

    // 02 §2.3 状态机:IDLE / SPEECH + onsetCount / releaseCount。
    const int hangoverFrames = std::max(1, p.hangoverMs / 10);
    const int attack = std::max(1, p.attackFrames);

    std::vector<VadSegment> cores;
    bool speech = false;
    int onsetCount = 0;
    int releaseCount = 0;
    int64_t coreStart = 0;

    for (std::size_t k = 0; k < n; ++k)
    {
        if (!speech)
        {
            if (m.l[k] > m.ton)
            {
                ++onsetCount;
                if (onsetCount >= attack)
                {
                    speech = true;
                    coreStart = static_cast<int64_t>(k) - onsetCount + 1;
                    releaseCount = 0;
                }
            }
            else
            {
                onsetCount = 0;
            }
        }
        else
        {
            if (m.l[k] < m.toff)
            {
                ++releaseCount;
                if (releaseCount >= hangoverFrames)
                {
                    cores.push_back({coreStart, static_cast<int64_t>(k) - releaseCount + 1});
                    speech = false;
                    onsetCount = 0;
                }
            }
            else
            {
                releaseCount = 0;
            }
        }
    }
    if (speech)
        cores.push_back({coreStart, static_cast<int64_t>(n)}); // 截断到选区尾

    // 后处理顺序固定(02 §2.3,顺序即语义):
    // P1 丢短(先于 padding!)。
    const int64_t minHops = p.minSegmentMs / 10;
    std::vector<VadSegment> segs;
    segs.reserve(cores.size());
    for (const VadSegment& c : cores)
    {
        if (c.endHop - c.startHop >= minHops)
            segs.push_back(c);
    }

    // P2 padding(双字段,截断到选区覆盖范围)。
    const int64_t preHops = p.paddingPreMs / 10;
    const int64_t postHops = p.paddingPostMs / 10;
    const int64_t selStart = firstHop;
    const int64_t selEnd = firstHop + static_cast<int64_t>(n);
    for (VadSegment& s : segs)
    {
        s.startHop = std::max(s.startHop - preHops, selStart);
        s.endHop = std::min(s.endHop + postHops, selEnd);
    }

    // P3 并重叠:padding 导致重叠/相接的相邻段取并集。
    if (segs.size() > 1)
    {
        std::vector<VadSegment> merged;
        merged.push_back(segs[0]);
        for (std::size_t i = 1; i < segs.size(); ++i)
        {
            if (segs[i].startHop <= merged.back().endHop)
                merged.back().endHop = std::max(merged.back().endHop, segs[i].endHop);
            else
                merged.push_back(segs[i]);
        }
        segs = std::move(merged);
    }

    // P4 并间隙:相邻段 gap < mergeGapMs → 合并。
    if (segs.size() > 1)
    {
        const int64_t mergeHops = p.mergeGapMs / 10;
        std::vector<VadSegment> merged;
        merged.push_back(segs[0]);
        for (std::size_t i = 1; i < segs.size(); ++i)
        {
            const int64_t gap = segs[i].startHop - merged.back().endHop;
            if (gap < mergeHops)
                merged.back().endHop = std::max(merged.back().endHop, segs[i].endHop);
            else
                merged.push_back(segs[i]);
        }
        segs = std::move(merged);
    }

    result.segments = std::move(segs);
    return result;
}

void EnergyVad::computeFromFeatures(const float* kwMs, std::size_t numHops, const VadParams& p, float* posteriorRawOut)
{
    if (posteriorRawOut == nullptr || numHops == 0)
        return;

    const VadModel m = buildModel(kwMs, numHops, p);
    const float denom = m.ton - m.toff; // ≥ 1
    for (std::size_t k = 0; k < numHops; ++k)
        posteriorRawOut[k] = (m.l[k] - m.toff) / denom; // p_raw 不截断(§2.7)
}

} // namespace scvb::analysis
