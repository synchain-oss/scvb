// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

// ParamSmoother:scvb_core 的 JUCE-free 一阶线性平滑器(等价于 juce::LinearSmoothedValue<float> 的
// setTargetValue/getNextValue/reset 语义,03 §2.4 统一平滑层)。ADR-011:core 不得依赖 JUCE,故在
// core 内自实现;语义与 §2.4 完全一致:
//   - reset(sampleRate, rampSeconds) 只换档,不重置当前值;
//   - setTargetValue(v) 从当前值起经 rampSeconds 线性滑向 v(零跳变);
//   - getNextValue() 逐样本前进一格;countdown 归零后钉在 target 上。
namespace scvb::dsp
{

class LinearSmoother
{
public:
    LinearSmoother() = default;

    explicit LinearSmoother(float initial) noexcept : m_current(initial), m_target(initial) {}

    // 换档:设置采样率与斜坡时长(秒),不改变当前值。sampleRate<=0 或 ramp<=0 时后续
    // setTargetValue 直接跳变(测试/未准备期语义)。
    void reset(double sampleRate, double rampSeconds) noexcept
    {
        m_sampleRate = (sampleRate > 0.0) ? sampleRate : 0.0;
        m_rampSeconds = (rampSeconds > 0.0) ? rampSeconds : 0.0;
    }

    // 硬置位:当前值=目标值=给定值,无斜坡(初始化/权威交接读数落地)。
    void setCurrentAndTargetValue(float value) noexcept
    {
        m_current = m_target = value;
        m_countdown = 0;
        m_step = 0.0f;
    }

    // 设目标:从当前值经 reset() 设定的斜坡时长线性滑向 target。target==current 时立即完成。
    void setTargetValue(float target) noexcept
    {
        m_target = target;
        if (m_sampleRate > 0.0 && m_rampSeconds > 0.0 && m_target != m_current)
        {
            const int steps = std::max(1, static_cast<int>(m_rampSeconds * m_sampleRate + 0.5));
            m_stepsToTarget = steps;
            m_step = (m_target - m_current) / static_cast<float>(steps);
            m_countdown = steps;
        }
        else
        {
            m_current = m_target;
            m_countdown = 0;
            m_step = 0.0f;
        }
    }

    // 前进一格,返回当前平滑值。斜坡走完后钉在 target 上。
    float getNextValue() noexcept
    {
        if (m_countdown <= 0)
        {
            m_current = m_target;
            return m_target;
        }

        --m_countdown;
        m_current += m_step;
        if (m_countdown == 0)
            m_current = m_target; // 末格精确落靶,避免浮点累积漂移
        return m_current;
    }

    float getCurrentValue() const noexcept { return m_current; }
    float getTargetValue() const noexcept { return m_target; }
    bool isSmoothing() const noexcept { return m_countdown > 0; }

private:
    float m_current = 0.0f;
    float m_target = 0.0f;
    float m_step = 0.0f;
    int m_stepsToTarget = 0;
    int m_countdown = 0;
    double m_sampleRate = 0.0;
    double m_rampSeconds = 0.0;
};

} // namespace scvb::dsp
