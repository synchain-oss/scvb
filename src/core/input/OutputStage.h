// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OutputStage —— Input 输出级策略(01 §8-2)+ C18 模式字 + RampSwitcher + 滞回状态机(J12/J32)。
// 命名空间 scvb::input(01 §8.2)。JUCE-free,可离线单测(不引用 AudioProcessor/AudioBuffer)。
//
// 两档仲裁(ADR-002 v1/J12):连接健康 → SilenceStage(静音);其余 → PassthroughStage(直通)。
// 两者均为 v1 常规运行时实现(01 §8-2),降级模式只是把 PassthroughStage 设为常开、不再评估健康。
// 判定结果经 C18 模式字(atomic<u32> stage ∈ {silence,passthrough},[M] 25Hz 写 / [A] 每块读)
// 由 RampSwitcher 执行 80ms 等功率交叉 ramp;5s 滞回只作用于 静音→直通 方向(直通→静音在确认
// 健康后立即 ramp,不等滞回,J32)。

#include <atomic>
#include <cstdint>

namespace scvb::input
{

// C18 模式字取值(atomic<std::uint32_t> 承载;[M] 25Hz 写 / [A] 每块读)。
enum class OutputStageMode : std::uint32_t
{
    kSilence = 0, // 健康:静音
    kPassthrough = 1 // 其余:直通(源通道原样,[J57] 不下混)
};

// 可配常量(验收:默认 kStageRampMs=80、kPassthroughHysteresisMs=5000)。
inline constexpr double kStageRampMs = 80.0;
inline constexpr double kPassthroughHysteresisMs = 5000.0;

// 块长规划(01 §5.1 步骤 1):采集/写环按夹取后的 captureSamples(越界夹取),输出级渲染按
// renderSamples = 全块 numSamples(消除静音档大块尾段泄漏,PR#51 重要#2)。JUCE-free 纯函数。
struct InputBlockPlan
{
    int captureSamples = 0; // 采集/写环长度 = min(numSamples, preparedMaxBlock)
    int renderSamples = 0; // 输出级渲染长度 = numSamples(全块)
};

inline InputBlockPlan planBlock(int numSamples, int preparedMaxBlock) noexcept
{
    InputBlockPlan p;
    p.renderSamples = numSamples > 0 ? numSamples : 0;
    const int cap = preparedMaxBlock > 0 ? preparedMaxBlock : 0;
    p.captureSamples = numSamples < cap ? numSamples : cap;
    if (p.captureSamples < 0)
    {
        p.captureSamples = 0;
    }
    return p;
}

// 输出级策略接口(01 §8-2)。ch 为平面声道指针(numChannels 路,每路 numSamples)。
class IOutputStage
{
public:
    virtual ~IOutputStage() = default;
    virtual void render(float* const* ch, int numChannels, int numSamples) = 0;
};

// 静音档:清零(ADR-002 v1 健康路径)。
class SilenceStage final : public IOutputStage
{
public:
    void render(float* const* ch, int numChannels, int numSamples) override;
};

// 直通档:保持源通道原样([J57] 不下混、不互换)。稳态零开销。
class PassthroughStage final : public IOutputStage
{
public:
    void render(float* const*, int, int) override {}
};

// RampSwitcher:80ms 等功率交叉 ramp(01 §5.1 步骤 6)。在直通(源)与静音(零)之间等功率交叉
// 淡变(cos²+sin²=1;静音源为零 → 输出 = 源 × cos(θ),θ ∈ [0, π/2])。
// target_/theta_ 仅音频线程(render)访问 —— [A] 每块从 C18 模式字读出目标档并作为参数传入,
// 进程内 C18 模式字是 [M] 25Hz 写 / [A] 每块读的 atomic<u32>,与本类的进度状态无共享可变成员。
// 稳态直通 = 零开销逐样本保持(输出==输入,验收判据);稳态静音 = 清零。render 零分配。
class RampSwitcher
{
public:
    RampSwitcher() = default;

    // 采样率与 ramp 时长(可配,默认 kStageRampMs)。
    void prepare(double sampleRate, double rampMs = kStageRampMs);

    // 音频线程:target 为当前目标档(调用方从 C18 模式字读出)。原地渲染(直通=缩放 cos(θ),静音=清零)。
    void render(float* const* ch, int numChannels, int numSamples, OutputStageMode target);

    // 稳态判定(供 [M] muted 确认位 C19;仅音频线程写,消息线程经 atomic 间接读,见 InputProcessor)。
    bool isSettledSilence() const noexcept { return target_ == OutputStageMode::kSilence && theta_ >= kHalfPi; }
    bool isSettledPassthrough() const noexcept { return target_ == OutputStageMode::kPassthrough && theta_ <= 0.0; }

private:
    static constexpr double kHalfPi = 1.57079632679489661923;

    double sampleRate_ = 48000.0;
    double rampSamples_ = 3840.0; // 80ms @48k
    OutputStageMode target_ = OutputStageMode::kPassthrough;
    double theta_ = 0.0; // 当前相位:0=直通,π/2=静音
};

// StageSwitchStateMachine:健康判定 → 目标档(J12/J32 滞回)。
// evaluate(healthy, nowMs) 由 [M] 25Hz 调用:
//   healthy → 目标 = silence(直通→静音立即,无滞回);
//   !healthy → 目标维持 silence 直到持续不健康满 kPassthroughHysteresisMs → passthrough(5s 滞回)。
// 时间可注入(virtual clock),便于单测。
class StageSwitchStateMachine
{
public:
    explicit StageSwitchStateMachine(double hysteresisMs = kPassthroughHysteresisMs);

    OutputStageMode target() const noexcept { return target_; }

    // [M] 25Hz 评估;返回当前目标档。
    OutputStageMode evaluate(bool healthy, std::uint64_t nowMs) noexcept;

    // 改组/释放/claim 未就绪:强制直通(期间输出走直通档,人声不消失,01 §4.1 改组转移)。
    void forcePassthrough() noexcept;

private:
    double hysteresisMs_;
    OutputStageMode target_ = OutputStageMode::kPassthrough;
    std::uint64_t unhealthySinceMs_ = 0;
    bool haveUnhealthySince_ = false;
};

} // namespace scvb::input
