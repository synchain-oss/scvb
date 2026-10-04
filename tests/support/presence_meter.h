// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// presence_meter —— 逐轨「在场判据」仪器(A 线 A-1)。header-only,不依赖 JUCE,也不依赖 Catch2。
//
// 用途:调度模拟器(A-2 起)把 1 个 Output + N 条 mono Input 放在各种宿主调度下跑,录下 Output 的
// L/R;本仪器按**时间线对齐**的 1024 样本帧,逐轨回答「这一帧里这条轨在不在、对不对」。
// 它是后面所有判据的尺子 —— 尺子自己不判红,后面全部假绿。所以每个设计选择都对着一种
// 具体的错读方式,删掉它对应的自测必红(见 tests/core/test_presence_meter.cpp 头注的删除式表)。
//
// ## 信号
//
// lane i 播放 s_i(t) = A · env_i(t) · sin(2π · ν_i · t),t = 时间线样本号(int64,可为负)。
//
//   · 载波 ν_i = (2·n_i + 1) / (2R) 周/样本,即 f_i = (n_i + 1/2) · sr / R,R = 2^19 = 环长。
//     整整平移 R(读到上一圈留在环里的旧数据)时相位推进 2π·n_i + π ⇒ **反相**,
//     与期望波形的相关 ≈ −1,判 Wrong。若取 R 点 DFT 的 bin 中心(ν = n/R),平移 R 后
//     逐样本相同 —— 别名读出完全不可见。这是要避开的坑(删除式落点 ①:kCarrierHalfStep)。
//   · 平移 2R 时载波又回到同相,只能靠包络:env(t) = 0.6 + 0.4·sin(2π·t/P + θ_i),
//     P = 28807(素数,与 R 互素)。R、2R、4R、6R、8R 对 P 的余数分数分别约为
//     0.2 / 0.4 / 0.8 / 0.2 / 0.6,所以 2R 别名读出的包络与期望错开约 144°,
//     幅度比在 0.3…4.4 之间来回摆,幅度核对抓得住(删除式落点 ②:kEnvDepth)。
//   · n_i 落在「分析帧 bin 的整数间隔」上:lane 间隔 8 个分析帧 bin,在 4 项 Blackman-Harris 窗下
//     彼此严格正交(稳态串扰为数值噪声级)。整体再加 0.584 个分析帧 bin 的小数偏移(kFracUnits),
//     这样整块错位的读出相关也会掉下去:错 64 / 128 / 256 / 480 / 512 / 1024 / 2048 / 3072 / 4096
//     样本时,16 条 lane 的相关全部 < 0.81(按 cos(2π·ν·d) 逐格算过),不会被当成在场。
//
// ## 逐帧测量(每轨每帧)
//
// 对齐时间线的帧 [k·1024, (k+1)·1024):对观测 L、R 和期望波形各做一次加窗 Goertzel
// (在 ν_i 处的单点 DFT),得到复数 X_L、X_R、X_E。窗用 4 项 Blackman-Harris 而不是 Hann:
// probe 离 lane 只有 4 个 bin,包络调制出来的边带用 Hann 会漏进 probe,稳态时 probe 读数
// 只比 Absent 下限低 10 dB 左右,稍有增益平滑就会被误判成「有硬切」;换成 Blackman-Harris 后
// 稳态 probe 读数落到 float32 量化噪声那一级(自测里核 ≥ 30 dB 余量)。
//   · 能量 energy = msL + msR:该轨频点上两声道的均方之和(正弦幅度 a ⇒ 均方 a²/2)。
//   · 相关 rho = cos(arg((X_L + X_R) / X_E)):观测在该频点上的分量与期望波形的归一化互相关
//     (窄带形式;多轨混在一起时宽带互相关会被别的轨稀释,窄带不会)。
//   · 幅度比 ampRatio = |X_L + X_R| / |X_E|。
//
// 零值对照频点组(probe):每两条 lane 之间的中点各一个,外加最低 lane 之下、最高 lane 之上
// 各一个,共 kMaxLanes + 1 个。**任何 Input 都不播放这些频点**。稳态时它们只有数值噪声;
// 帧内一旦有硬切(读失败整块置零、错读首尾的跳变),宽带泄漏会立刻抬高它们 ——
// 这就是本帧的噪声底标定。probe 0(最低那个)就是「零值对照频率」,自测要求它在无硬切的
// 渲染上**全帧 Absent**。
//
// ## 四类判定
//
// 期望在场(expectActive = true)时,按下列顺序取第一个成立的:
//   1. energy < floorAbsent                                    ⇒ Absent
//   2. energy ≥ thrPresent 且 rho ≥ 0.9 且幅度核对通过         ⇒ Present
//   3. 本帧有宽带泄漏(任一 probe ≥ floorAbsent)              ⇒ Partial(Dirty)
//   4. rho < 0.5                                               ⇒ Wrong(反相 / 错位)
//   5. 其余                                                    ⇒ Partial(幅度不符 / 能量偏弱 / 相关偏弱)
// 期望静音(expectActive = false)时:
//   1. energy < floorAbsent ⇒ Absent;2. 本帧有宽带泄漏 ⇒ Partial(Dirty);3. 其余 ⇒ Wrong。
//
// 第 3 条(Dirty)的理由:硬切帧里宽带泄漏会落到每一个频点上,窄带相关在那一帧是随机的,
// 照常判会把**无辜的**轨判成 Wrong。所以有泄漏的帧只许判 Absent / Present / Partial,
// Wrong 只在干净帧里下 —— 真的错读(旧数据、错位)是连续一段,它内部的帧都是干净的,照样判 Wrong。
// 有没有硬切本身由 probe 直接报出来(controlNonAbsent / dirtyFrames),调用方可以单独断言。
//
// 幅度核对:默认自标定 —— 取本段内「干净、期望在场、能量够、rho ≥ 0.9」帧的 ampRatio 中位数,
// **按来源分组**(未平衡原声一组、SCVB 混音一组,见下)各标一个。所以常数增益、声像不影响判定
// (增益和声像不敏感);只有「同一来源内幅度忽大忽小」才判幅度不符。ampTol < 0 时不核幅度
// (被测增益本身随时间变的场景用)。也可以传固定参考值 fixedGain。
//
// ## 来源:未平衡原声 vs SCVB 混音
//
// 调度模拟器里 lane 进总线时用宿主侧可辨声像:**只进 L、增益 0.5**(kRawBusGainL / kRawBusGainR)。
// SCVB 混音是居中的(L、R 都有)。所以按该轨频点上的 R/L 能量比区分:
// msR < rawMaxRtoL · msL(默认 1%,即 −20 dB)⇒ Raw,否则 ⇒ Mix。
// SCVB 侧若把某轨声像打到极左,会被认成 Raw —— 场景里给 lane 设居中即可。
//
// ## 不覆盖的(写清楚,免得被当成全称)
//
//   · 相关只看一个频点:错位量恰好接近载波周期整数倍、包络又没走开的读出(例如 lane 1 错
//     1 个载波周期 ≈ 50 样本),相关仍 ≈ 1,只能靠幅度核对抓一部分帧。自测钉住的是上面那九种
//     块级错位(64…4096 样本),不是「任意错位都判得出」。
//   · 同一轨内容被**常数倍**放大(例如两条路径叠加)在自标定下看不出来;双路叠加要靠
//     来源判别(Raw 与 Mix 同时出现时 R/L 比变化)或调用方给 fixedGain。
//   · 运行期文案(toString)一律 ASCII(中文字面量在本机 CP936 上会触发 C4819)。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

#include "ipc/SegmentLayout.h"

namespace scvb::testsupport::presence
{

// ---------------------------------------------------------------------------
// 几何
// ---------------------------------------------------------------------------

inline constexpr double kTwoPi = 6.283185307179586476925286766559;

// 环长 R 直接取生产常量:环长哪天改了,别名的位置跟着变,仪器跟着变。
inline constexpr std::int64_t kRingR = static_cast<std::int64_t>(scvb::kDefaultRingFrames);
static_assert(kRingR > 0 && (kRingR & (kRingR - 1)) == 0, "ring length must be a power of two");

// 分析帧:1024 样本,对齐时间线(帧起点是 1024 的整数倍)。
inline constexpr int kFrame = 1024;
static_assert(kRingR % kFrame == 0, "ring length must be a multiple of the analysis frame");

// n 的单位是 sr/R;一个分析帧 bin(sr/1024)= R/1024 = 512 个单位。
inline constexpr std::int64_t kUnitsPerBin = kRingR / kFrame;

// 删除式落点 ①:载波分子里的「+1」。f = (n + kCarrierHalfStep/2)·sr/R。
// 置 0 ⇒ 频率落在 R 点 DFT 的 bin 中心,平移 R 不可见 ⇒ R 别名自测必红。
inline constexpr std::int64_t kCarrierHalfStep = 1;

// 梳状频点:lane i 在第 (kLaneBaseBin + kLaneStepBins·i) 个分析帧 bin(再加小数偏移)。
// 48 kHz 下 lane 0..3 约为 590 / 965 / 1340 / 1715 Hz。间隔 8 = Blackman-Harris 主瓣全宽,
// probe 落在两条 lane 正中间(各隔 4 个 bin,正好在两侧主瓣的零点上)。
inline constexpr int kLaneBaseBin = 12;
inline constexpr int kLaneStepBins = 8;
inline constexpr int kMaxLanes = 16;
inline constexpr int kProbeCount = kMaxLanes + 1;
// 0.584 个分析帧 bin 的小数偏移:整块错位读出时相关掉下去(见文件头注)。
inline constexpr std::int64_t kFracUnits = 299;

// 包络。删除式落点 ②:kEnvDepth 置 0 ⇒ 包络消失 ⇒ 2R 别名自测必红。
inline constexpr std::int64_t kEnvPeriod = 28807;
inline constexpr double kEnvBase = 0.6;
inline constexpr double kEnvDepth = 0.4;
inline constexpr double kGoldenAngle = 2.399963229728653; // 各轨包络相位错开

// 每轨默认幅度:4 条满幅包络叠加峰值 0.8,不削波。
inline constexpr double kDefaultLaneAmp = 0.2;

// 宿主侧把 lane 送进总线时的可辨声像:未平衡原声只进 L、增益 0.5。
inline constexpr double kRawBusGainL = 0.5;
inline constexpr double kRawBusGainR = 0.0;

inline constexpr double ampToMs(double amp)
{
    return 0.5 * amp * amp;
}

// 均方 → 等效正弦峰值的 dBFS(仅供 INFO 打印)。
inline double msToDbfs(double ms)
{
    return ms > 0.0 ? 10.0 * std::log10(2.0 * ms) : -400.0;
}

inline constexpr std::int64_t laneUnits(int lane)
{
    return static_cast<std::int64_t>(kLaneBaseBin + kLaneStepBins * lane) * kUnitsPerBin + kFracUnits;
}

// probe k(k = 0..kMaxLanes):lane k−1 与 lane k 之间的中点;probe 0 在 lane 0 之下。
inline constexpr std::int64_t probeUnits(int k)
{
    return static_cast<std::int64_t>(kLaneBaseBin - kLaneStepBins / 2 + kLaneStepBins * k) * kUnitsPerBin + kFracUnits;
}

// 每样本周数。2R 是 2 的幂,分子是整数 ⇒ 这个 double 是精确的。
inline double cyclesPerSample(std::int64_t units)
{
    return static_cast<double>(2 * units + kCarrierHalfStep) / static_cast<double>(2 * kRingR);
}

// ---------------------------------------------------------------------------
// 信号合成
// ---------------------------------------------------------------------------

struct Tone
{
    std::int64_t units = 0; // n_i
    double envPhase = 0.0; // θ_i
    double amp = kDefaultLaneAmp; // A
};

inline Tone laneTone(int lane, double amp = kDefaultLaneAmp)
{
    return Tone{laneUnits(lane), static_cast<double>(lane) * kGoldenAngle, amp};
}

inline std::int64_t positiveMod(std::int64_t a, std::int64_t m)
{
    const std::int64_t r = a % m;
    return r < 0 ? r + m : r;
}

// 载波相位用整数取模精确算:t 可以是几亿、也可以为负(起播前的预备拍),都不丢精度。
// (2n+1) < 2R = 2^20,t mod 2R < 2^20,乘积 < 2^40,int64 放得下。
inline double carrierPhase(std::int64_t units, std::int64_t t)
{
    const std::int64_t twoR = 2 * kRingR;
    const std::int64_t num = positiveMod(2 * units + kCarrierHalfStep, twoR);
    const std::int64_t k = (num * positiveMod(t, twoR)) % twoR;
    return kTwoPi * static_cast<double>(k) / static_cast<double>(twoR);
}

inline double envelopeAt(const Tone& tone, std::int64_t t)
{
    const std::int64_t tm = positiveMod(t, kEnvPeriod);
    return kEnvBase +
           kEnvDepth * std::sin(kTwoPi * static_cast<double>(tm) / static_cast<double>(kEnvPeriod) + tone.envPhase);
}

// 期望波形在时间线位置 t 的值。
inline double sampleAt(const Tone& tone, std::int64_t t)
{
    return tone.amp * envelopeAt(tone, t) * std::sin(carrierPhase(tone.units, t));
}

// dst[i] += gain · sampleAt(laneTone(lane), contentT0 + i)。
// contentT0 = dst[0] 承载的内容在时间线上的位置;故意错读时由调用方把它减去偏移量。
inline void addLane(int lane, std::int64_t contentT0, float* dst, std::int64_t n, double gain = 1.0)
{
    const Tone tone = laneTone(lane);
    for (std::int64_t i = 0; i < n; ++i)
    {
        dst[i] += static_cast<float>(gain * sampleAt(tone, contentT0 + i));
    }
}

// ---------------------------------------------------------------------------
// 加窗 Goertzel
// ---------------------------------------------------------------------------

struct Cplx
{
    double re = 0.0;
    double im = 0.0;
};

inline double norm2(const Cplx& z)
{
    return z.re * z.re + z.im * z.im;
}

// 周期 4 项 Blackman-Harris 窗(旁瓣 −92 dB,主瓣半宽 4 个 bin),和 = kFrame · kBh0。
inline constexpr double kBh0 = 0.35875;
inline constexpr double kBh1 = 0.48829;
inline constexpr double kBh2 = 0.14128;
inline constexpr double kBh3 = 0.01168;

inline const std::array<double, kFrame>& analysisWindow()
{
    static const std::array<double, kFrame> window = [] {
        std::array<double, kFrame> w{};
        for (int i = 0; i < kFrame; ++i)
        {
            const double x = kTwoPi * static_cast<double>(i) / kFrame;
            w[static_cast<std::size_t>(i)] =
                kBh0 - kBh1 * std::cos(x) + kBh2 * std::cos(2.0 * x) - kBh3 * std::cos(3.0 * x);
        }
        return w;
    }();
    return window;
}

// 在 nu(周/样本)处的单点 DFT,相位以本帧第 0 个样本为参考。
template<typename T>
inline Cplx goertzelWindowed(const T* x, double nu)
{
    const std::array<double, kFrame>& w = analysisWindow();
    const double omega = kTwoPi * nu;
    const double coeff = 2.0 * std::cos(omega);
    double s1 = 0.0;
    double s2 = 0.0;
    for (int i = 0; i < kFrame; ++i)
    {
        const double s0 = static_cast<double>(x[i]) * w[static_cast<std::size_t>(i)] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    // y = s1 − e^{−jω}·s2 = e^{jω(N−1)} · X(ω);转回以第 0 个样本为相位参考。
    const double yre = s1 - std::cos(omega) * s2;
    const double yim = std::sin(omega) * s2;
    const double rot = -omega * static_cast<double>(kFrame - 1);
    const double c = std::cos(rot);
    const double s = std::sin(rot);
    return Cplx{yre * c - yim * s, yre * s + yim * c};
}

// 幅度 a 的正弦:|X| = a · Σw / 2 = a · N · kBh0 / 2 ⇒ 均方 a²/2 = 2·|X|² / (N · kBh0)²。
inline double meanSquare(const Cplx& x)
{
    const double sumW = static_cast<double>(kFrame) * kBh0;
    return 2.0 * norm2(x) / (sumW * sumW);
}

// ---------------------------------------------------------------------------
// 判定口径
// ---------------------------------------------------------------------------

enum class Verdict
{
    Present,
    Absent,
    Wrong,
    Partial
};

enum class Why
{
    Ok, // Present
    LowEnergy, // Absent
    Dirty, // Partial:本帧有宽带泄漏,窄带相关不可信
    Decorrelated, // Wrong:rho < rhoWrong(反相 / 错位)
    SilentHasEnergy, // Wrong:期望静音却有能量
    AmpMismatch, // Partial:相关够、能量够,幅度与同来源参考不符
    BelowPresent, // Partial:能量介于下限与在场阈值之间
    WeakCorrelation // Partial:rho 介于 rhoWrong 与 rhoPresent 之间
};

enum class Source
{
    None, // 能量不到下限
    Raw, // 未平衡原声(宿主侧只进 L)
    Mix // SCVB 混音(L、R 都有)
};

inline const char* toString(Verdict v)
{
    switch (v)
    {
    case Verdict::Present:
        return "Present";
    case Verdict::Absent:
        return "Absent";
    case Verdict::Wrong:
        return "Wrong";
    case Verdict::Partial:
        return "Partial";
    }
    return "?";
}

inline const char* toString(Why w)
{
    switch (w)
    {
    case Why::Ok:
        return "Ok";
    case Why::LowEnergy:
        return "LowEnergy";
    case Why::Dirty:
        return "Dirty";
    case Why::Decorrelated:
        return "Decorrelated";
    case Why::SilentHasEnergy:
        return "SilentHasEnergy";
    case Why::AmpMismatch:
        return "AmpMismatch";
    case Why::BelowPresent:
        return "BelowPresent";
    case Why::WeakCorrelation:
        return "WeakCorrelation";
    }
    return "?";
}

inline const char* toString(Source s)
{
    switch (s)
    {
    case Source::None:
        return "None";
    case Source::Raw:
        return "Raw";
    case Source::Mix:
        return "Mix";
    }
    return "?";
}

struct Thresholds
{
    // 删除式落点 ③:floorAbsent 置 0 ⇒ 没有任何帧能判 Absent ⇒ 缺席自测必红。
    double floorAbsent = ampToMs(5.0e-4); // 等效正弦峰值 −66 dBFS
    double thrPresent = ampToMs(2.0e-3); // −54 dBFS
    double rhoPresent = 0.9;
    double rhoWrong = 0.5;
    double ampTol = 0.2; // |ampRatio / 参考 − 1| ≤ ampTol;< 0 表示不核幅度
    double rawMaxRtoL = 0.01; // R/L 能量比低于它 ⇒ Raw(−20 dB)
};

// ---------------------------------------------------------------------------
// 测量
// ---------------------------------------------------------------------------

struct FrameNoise
{
    double control = 0.0; // probe 0(零值对照频率)的能量
    double probeMax = 0.0; // 全部 probe 的最大能量
};

struct LaneMeasure
{
    std::int64_t frameStart = 0;
    double msL = 0.0;
    double msR = 0.0;
    double energy = 0.0; // msL + msR
    double rho = 0.0;
    double ampRatio = 0.0;
    bool dirty = false; // 本帧 probeMax ≥ floorAbsent
    bool expectActive = true;
};

// left / right 指向本帧第 0 个样本。
inline FrameNoise measureNoise(const float* left, const float* right)
{
    FrameNoise n;
    for (int k = 0; k < kProbeCount; ++k)
    {
        const double nu = cyclesPerSample(probeUnits(k));
        const double e = meanSquare(goertzelWindowed(left, nu)) + meanSquare(goertzelWindowed(right, nu));
        if (k == 0)
        {
            n.control = e;
        }
        n.probeMax = std::max(n.probeMax, e);
    }
    return n;
}

inline LaneMeasure measureLane(int lane, const float* left, const float* right, std::int64_t frameStart,
                               const FrameNoise& noise, bool expectActive, const Thresholds& th)
{
    const Tone tone = laneTone(lane);
    const double nu = cyclesPerSample(tone.units);
    std::array<double, kFrame> expected{};
    for (int i = 0; i < kFrame; ++i)
    {
        expected[static_cast<std::size_t>(i)] = sampleAt(tone, frameStart + i);
    }
    const Cplx xl = goertzelWindowed(left, nu);
    const Cplx xr = goertzelWindowed(right, nu);
    const Cplx xe = goertzelWindowed(expected.data(), nu);

    LaneMeasure m;
    m.frameStart = frameStart;
    m.msL = meanSquare(xl);
    m.msR = meanSquare(xr);
    m.energy = m.msL + m.msR;
    m.dirty = noise.probeMax >= th.floorAbsent;
    m.expectActive = expectActive;

    // G = (X_L + X_R) / X_E。期望波形的包络恒 ≥ 0.2·A,X_E 不会为零。
    const Cplx xm{xl.re + xr.re, xl.im + xr.im};
    const double den = norm2(xe);
    if (den > 0.0)
    {
        const double gre = (xm.re * xe.re + xm.im * xe.im) / den;
        const double gim = (xm.im * xe.re - xm.re * xe.im) / den;
        const double mag = std::sqrt(gre * gre + gim * gim);
        m.ampRatio = mag;
        m.rho = mag > 0.0 ? gre / mag : 0.0;
    }
    return m;
}

inline Source classifySource(const LaneMeasure& m, const Thresholds& th)
{
    if (m.energy < th.floorAbsent)
    {
        return Source::None;
    }
    return m.msR < th.rawMaxRtoL * m.msL ? Source::Raw : Source::Mix;
}

// 按来源分组的幅度参考;0 = 该组没有可用的标定帧(该组不核幅度)。
struct GainRef
{
    double raw = 0.0;
    double mix = 0.0;
};

inline double gainFor(const GainRef& g, Source s)
{
    return s == Source::Raw ? g.raw : (s == Source::Mix ? g.mix : 0.0);
}

struct Judged
{
    Verdict verdict = Verdict::Absent;
    Why why = Why::LowEnergy;
};

inline Judged classify(const LaneMeasure& m, double gainRef, const Thresholds& th)
{
    if (m.energy < th.floorAbsent)
    {
        return {Verdict::Absent, Why::LowEnergy};
    }
    if (!m.expectActive)
    {
        if (m.dirty)
        {
            return {Verdict::Partial, Why::Dirty};
        }
        return {Verdict::Wrong, Why::SilentHasEnergy};
    }
    const bool strong = m.energy >= th.thrPresent;
    const bool coherent = m.rho >= th.rhoPresent;
    const bool ampOk = th.ampTol < 0.0 || gainRef <= 0.0 || std::fabs(m.ampRatio / gainRef - 1.0) <= th.ampTol;
    if (strong && coherent && ampOk)
    {
        return {Verdict::Present, Why::Ok};
    }
    if (m.dirty)
    {
        return {Verdict::Partial, Why::Dirty};
    }
    if (m.rho < th.rhoWrong)
    {
        return {Verdict::Wrong, Why::Decorrelated};
    }
    if (strong && coherent)
    {
        return {Verdict::Partial, Why::AmpMismatch};
    }
    if (!strong)
    {
        return {Verdict::Partial, Why::BelowPresent};
    }
    return {Verdict::Partial, Why::WeakCorrelation};
}

// ---------------------------------------------------------------------------
// 整段分析
// ---------------------------------------------------------------------------

// 一段时间线连续的 Output 录音:left[i] / right[i] 对应时间线位置 start + i。
struct Span
{
    const float* left = nullptr;
    const float* right = nullptr;
    std::int64_t start = 0;
    std::int64_t length = 0;
};

inline std::int64_t floorDiv(std::int64_t a, std::int64_t b)
{
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
    {
        --q;
    }
    return q;
}

// span 里完整覆盖的对齐帧的起点(不完整的头尾帧不分析)。
inline std::vector<std::int64_t> alignedFrames(std::int64_t start, std::int64_t length)
{
    std::vector<std::int64_t> out;
    if (length < kFrame)
    {
        return out;
    }
    for (std::int64_t k = floorDiv(start + kFrame - 1, kFrame); (k + 1) * kFrame <= start + length; ++k)
    {
        out.push_back(k * kFrame);
    }
    return out;
}

struct LaneReport
{
    int lane = 0;
    std::vector<LaneMeasure> frames;
    std::vector<Source> sources;
    std::vector<Judged> judged;
    GainRef gain;

    int count(Verdict v) const
    {
        return static_cast<int>(
            std::count_if(judged.begin(), judged.end(), [v](const Judged& j) { return j.verdict == v; }));
    }
    int countWhy(Why w) const
    {
        return static_cast<int>(
            std::count_if(judged.begin(), judged.end(), [w](const Judged& j) { return j.why == w; }));
    }
    double fraction(Verdict v) const
    {
        return judged.empty() ? 0.0 : static_cast<double>(count(v)) / static_cast<double>(judged.size());
    }
};

struct CombReport
{
    std::vector<std::int64_t> frameStarts;
    std::vector<FrameNoise> noise;
    std::vector<LaneReport> lanes;

    // 零值对照频率(probe 0)不是 Absent 的帧数。
    int controlNonAbsent(const Thresholds& th) const
    {
        return static_cast<int>(std::count_if(noise.begin(), noise.end(),
                                              [&th](const FrameNoise& n) { return n.control >= th.floorAbsent; }));
    }
    // 任一 probe 不是 Absent 的帧数(有宽带泄漏 = 帧内有硬切)。
    int dirtyFrames(const Thresholds& th) const
    {
        return static_cast<int>(std::count_if(noise.begin(), noise.end(),
                                              [&th](const FrameNoise& n) { return n.probeMax >= th.floorAbsent; }));
    }
    double maxProbe() const
    {
        double m = 0.0;
        for (const FrameNoise& n : noise)
        {
            m = std::max(m, n.probeMax);
        }
        return m;
    }
};

// expect(lane, frameStart) == false 表示这一帧这条轨应当静音;不传 = 全程期望在场。
using ExpectFn = std::function<bool(int lane, std::int64_t frameStart)>;

inline double medianOf(std::vector<double> v)
{
    if (v.empty())
    {
        return 0.0;
    }
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    return v[mid];
}

// 自标定:干净、期望在场、能量够、rho ≥ rhoPresent 的帧,按来源分组取 ampRatio 中位数。
inline GainRef calibrateGain(const LaneReport& r, const Thresholds& th)
{
    std::vector<double> raw;
    std::vector<double> mix;
    for (std::size_t i = 0; i < r.frames.size(); ++i)
    {
        const LaneMeasure& m = r.frames[i];
        if (m.dirty || !m.expectActive || m.energy < th.thrPresent || m.rho < th.rhoPresent)
        {
            continue;
        }
        if (r.sources[i] == Source::Raw)
        {
            raw.push_back(m.ampRatio);
        }
        else if (r.sources[i] == Source::Mix)
        {
            mix.push_back(m.ampRatio);
        }
    }
    return GainRef{medianOf(std::move(raw)), medianOf(std::move(mix))};
}

// fixedGain 非空时用它做幅度参考,否则逐轨自标定。
inline CombReport analyze(const Span& span, const std::vector<int>& lanes, const ExpectFn& expect = {},
                          const Thresholds& th = {}, const GainRef* fixedGain = nullptr)
{
    CombReport rep;
    if (span.left == nullptr || span.right == nullptr)
    {
        return rep;
    }
    rep.frameStarts = alignedFrames(span.start, span.length);
    rep.noise.reserve(rep.frameStarts.size());
    for (std::int64_t fs : rep.frameStarts)
    {
        const std::int64_t off = fs - span.start;
        rep.noise.push_back(measureNoise(span.left + off, span.right + off));
    }
    for (int lane : lanes)
    {
        LaneReport lr;
        lr.lane = lane;
        lr.frames.reserve(rep.frameStarts.size());
        for (std::size_t i = 0; i < rep.frameStarts.size(); ++i)
        {
            const std::int64_t fs = rep.frameStarts[i];
            const std::int64_t off = fs - span.start;
            const bool active = expect ? expect(lane, fs) : true;
            lr.frames.push_back(measureLane(lane, span.left + off, span.right + off, fs, rep.noise[i], active, th));
            lr.sources.push_back(classifySource(lr.frames.back(), th));
        }
        lr.gain = fixedGain != nullptr ? *fixedGain : calibrateGain(lr, th);
        lr.judged.reserve(lr.frames.size());
        for (std::size_t i = 0; i < lr.frames.size(); ++i)
        {
            lr.judged.push_back(classify(lr.frames[i], gainFor(lr.gain, lr.sources[i]), th));
        }
        rep.lanes.push_back(std::move(lr));
    }
    return rep;
}

} // namespace scvb::testsupport::presence
