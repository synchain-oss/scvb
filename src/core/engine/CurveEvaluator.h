// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

// CurveEvaluator:引擎值层的段间过渡求值(02 §8.2)。
// 过渡是时间线的纯函数:同一绝对时间 t 恒得同一 P(t)/v(t),与块长、求值顺序无关。
namespace scvb
{

// 一条轨的曲线段:值在 [startSec, endSec) 内恒定(pan=P,volDb=v)。段按时间升序、互不重叠。
struct CurveSegment
{
    double startSec = 0.0;
    double endSec = 0.0;
    double pan = 0.0; // P ∈ [−100, +100](名义值;全局 width 缩放是下游独立步骤)
    double volDb = 0.0; // 音量修正(dB)
};

// 段间过渡配置(02 §0.3 / §8.2)。
struct TransitionConfig
{
    double transitionRampSec = 0.080; // transition_ramp_ms 默认 80ms
    double ratePanPerSec = 15.0; // rate_pan(峰值口径)
    double rateGainDbPerSec = 3.0; // rate_gain(峰值口径)
    double maxRampSec = 6.0; // gap=0 的 6s 上限(=1.5×4s)
    double overlapRatio = 0.9; // 相邻 ramp 重叠防护系数
};

class CurveEvaluator
{
public:
    // 单条边界的 ramp 调度结果(只依赖 segments + config 的纯函数)。
    struct Boundary
    {
        bool isJump = false; // 微小变化捷径:在 centerSec 处直接跳变
        double centerSec = 0.0; // ramp 中心(=空隙中点或公共边界)
        double tEffSec = 0.0; // T_eff(已含重叠防护夹取)
        bool warnLimit = false; // 本边界超限速(0.9·len 夹限 / 6s cap 路径共用计数)
    };

    // 由有序段表构建 ramp 调度。segments 可为空、可含 gap(段间空隙)。
    void build(const std::vector<CurveSegment>& segments, const TransitionConfig& config);

    // 求值:过渡是时间线纯函数。
    double panAt(double tSec) const;
    double volAt(double tSec) const;

    // 超限速 warning 计数(§8.2)。
    int warningCount() const noexcept { return m_warningCount; }

    // 边界调度(大小 = n−1),供测试断言 T_eff / 重叠防护。
    const std::vector<Boundary>& boundaries() const noexcept { return m_boundaries; }

private:
    double valueAt(double tSec, bool pan) const;

    std::vector<CurveSegment> m_segments;
    std::vector<Boundary> m_boundaries;
    int m_warningCount = 0;
};

} // namespace scvb
