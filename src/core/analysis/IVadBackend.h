// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace scvb::analysis
{

struct VadParams;

// VAD 后端抽象(02-dsp-spec §2.7):v1 冻结接口,v2(Silero 流式)预留。
// EnergyVad 实现本接口;下游状态机只消费后验 p_raw(进段 p_raw≥1、出段 p_raw≤0)。
// 统一语义见 02 §2.7:v1 能量域后验与 v2 Silero 后验归一化到同一尺度后融合(OR/MIN)。
class IVadBackend
{
public:
    virtual ~IVadBackend() = default;

    // 后端标识:"energy_v1" / "silero_v2"。
    virtual const char* name() const = 0;

    // energy_v1 仅由 kw_ms 特征离线可算 → true;silero_v2 需 16k 音频 → false。
    virtual bool worksFromFeatures() const = 0;

    // 离线路径(v1):由 kw_ms 计算后验 p_raw(不截断,02 §2.4 语义)。
    // posteriorRawOut 可为 nullptr(不写);容量须 ≥ numHops。
    virtual void computeFromFeatures(const float* kwMs, std::size_t numHops, const VadParams& p,
                                     float* posteriorRawOut) = 0;

    // 流式路径(v2:Input 采集线程调用)。energy_v1 不实现:streamBegin 返回 false,
    // streamProcess/streamEnd 空转,streamDrainPosterior 返回 0。
    virtual bool streamBegin(double sampleRate) = 0;
    virtual void streamProcess(const float* mono, std::size_t numSamples) = 0;
    virtual std::size_t streamDrainPosterior(float* out, std::size_t maxHops) = 0;
    virtual void streamEnd() = 0;
};

} // namespace scvb::analysis
