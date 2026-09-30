// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace scvb::dsp
{

// BS.1770 K-weighting 两级 biquad 系数(a0=1 已归一化)。
// 字段顺序 {b0,b1,b2,a1,a2},与 02-dsp-spec §1.1/§1.2 一致。
struct KWCoeffs
{
    double b0 = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

// Stage 1:高频搁架(≈1.68 kHz,+4 dB)。任意采样率下由模拟原型参数反推(De Man 常数),
// 48 kHz 走同一函数 —— 这是全项目 K-weighting 系数的唯一来源(02-dsp-spec §1.2)。
KWCoeffs makeStage1(double fs);

// Stage 2:RLB 高通(≈38 Hz)。
KWCoeffs makeStage2(double fs);

// BS.1770 K-weighting:stage1 → stage2 串联。
// Direct Form II Transposed,状态 double、输入输出 float,零分配(02-dsp-spec §1.1)。
class KWeighting
{
public:
    KWeighting() = default;

    // 依采样率重算两级系数并清零状态。
    void prepare(double sampleRate);

    // 仅清零滤波器状态(系数不变),用于 run/epoch 边界(02-dsp-spec §1.4)。
    void reset();

    // 处理单个样本,返回 K-weighted 输出。
    float process(float input);

private:
    KWCoeffs stage1_;
    KWCoeffs stage2_;
    double z11_ = 0.0; // stage1 状态 s1
    double z12_ = 0.0; // stage1 状态 s2
    double z21_ = 0.0; // stage2 状态 s1
    double z22_ = 0.0; // stage2 状态 s2
};

} // namespace scvb::dsp
