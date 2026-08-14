// SPDX-License-Identifier: GPL-3.0-or-later
#include "dsp/KWeighting.h"

#include <cmath>

namespace scvb::dsp
{

namespace
{
constexpr double kPi = 3.14159265358979323846264338327950288;

// Direct Form II Transposed 单个 biquad 节(状态 double,输入输出 double)。
double processBiquad(const KWCoeffs& c, double x, double& z1, double& z2)
{
    const double y = c.b0 * x + z1;
    z1 = c.b1 * x - c.a1 * y + z2;
    z2 = c.b2 * x - c.a2 * y;
    return y;
}
} // namespace

KWCoeffs makeStage1(double fs)
{
    const double f0 = 1681.9744509555319;
    const double gainDb = 3.999843853973347;
    const double q = 0.7071752369554196;
    const double k = std::tan(kPi * f0 / fs);
    const double vh = std::pow(10.0, gainDb / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double d = 1.0 + k / q + k * k;

    KWCoeffs out;
    out.b0 = (vh + vb * k / q + k * k) / d;
    out.b1 = 2.0 * (k * k - vh) / d;
    out.b2 = (vh - vb * k / q + k * k) / d;
    out.a1 = 2.0 * (k * k - 1.0) / d;
    out.a2 = (1.0 - k / q + k * k) / d;
    return out;
}

KWCoeffs makeStage2(double fs)
{
    const double f0 = 38.13547087602444;
    const double q = 0.5003270373238773;
    const double k = std::tan(kPi * f0 / fs);
    const double d = 1.0 + k / q + k * k;

    KWCoeffs out;
    out.b0 = 1.0;
    out.b1 = -2.0;
    out.b2 = 1.0;
    out.a1 = 2.0 * (k * k - 1.0) / d;
    out.a2 = (1.0 - k / q + k * k) / d;
    return out;
}

void KWeighting::prepare(double sampleRate)
{
    stage1_ = makeStage1(sampleRate);
    stage2_ = makeStage2(sampleRate);
    reset();
}

void KWeighting::reset()
{
    z11_ = 0.0;
    z12_ = 0.0;
    z21_ = 0.0;
    z22_ = 0.0;
}

float KWeighting::process(float input)
{
    const double x = static_cast<double>(input);
    const double y1 = processBiquad(stage1_, x, z11_, z12_);
    const double y2 = processBiquad(stage2_, y1, z21_, z22_);
    return static_cast<float>(y2);
}

} // namespace scvb::dsp
