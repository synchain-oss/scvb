// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "analysis/AutoAssign.h" // CenterSlotPolicy
#include "analysis/LoudnessMode.h" // LoudnessMode

namespace scvb::analysis
{

// 分析设置失效标记(03 §6.3):analysis_settings_stale := 当前口径 ≠ 上次全量分析所用口径。
// 置位 = 用户改设置;清零 = 一次全量分析完成后 markApplied()。**只提示、不自动重算**(UI 归 T35)。
//
// [SL-279] 本结构**为什么单独一个头**:它要同时装 `LoudnessMode`(LoudnessMode.h)与
//   `CenterSlotPolicy`(AutoAssign.h),而 `AutoAssign.h` 本身 include 了 `LoudnessMode.h`
//   —— 结构留在 LoudnessMode.h 里就成环。放在这里,两个枚举都能直接用,谁都不必前置声明。
//   同一次改动把 `computeSegmentLoudness` 的入参从本结构收成 `LoudnessMode`:它**只读
//   `loudnessMode` 一个成员**,签名收窄之后 LoudnessMode.h 不再需要认识本结构。
//
// [SL-279] **两项各判各的,不共用一个布尔**:两枚「需重新分析」徽标分别挂在响度档与
//   中央槽策略两个控件旁边(T35 设置页)。合成一个 stale 位会让两枚徽标同亮同灭 ——
//   用户改了响度档,中央槽那枚也亮,而那一项其实与上次分析一致。`stale()` 留着是给
//   「要不要整体重分析」那个问题用的,它是两项的或。
// ⚠ [SL-279 复审] **本结构今天在生产侧零消费者**,别把它读成这条派生式的真源。
//   实际落地的是另外两份:native 在 `OutputProcessor::finishAnalysis` 里拿 `runtime_` 的两对
//   `juce::String` 手写了一遍(那里要与落盘 / 撤销 / 桥面载荷同型,用不上本结构);
//   web 在 `tab-settings.js` 里 `analysisConfigStale(cur, applied)` 逐项调两次。
//   本结构是**给尚未落地的「第二响度指标读数」那条路准备的形状**(见 LoudnessMode.h 里
//   [SL-252] 那段),连同它的用例一起留着 —— 那条路落地时它就是现成的载体。
//   同一条派生式眼下有三份写法,是**已知负债**;要收敛请连三处一起收,别只改一处。
struct AnalysisSettingsStale
{
    LoudnessMode loudnessMode = LoudnessMode::KIntegrated; // 当前(用户设置)
    LoudnessMode appliedLoudnessMode = LoudnessMode::KIntegrated; // 上次全量分析所用

    // [SL-278] 中央槽策略与响度档同在这条派生式里 —— 02 §5.6 的奇数分支会改变槽位数与
    // 指派解,改档之后不重分析,段表与设置页显示的策略就对不上了。
    CenterSlotPolicy centerSlotPolicy = CenterSlotPolicy::PriorityQueue; // 当前(用户设置)
    CenterSlotPolicy appliedCenterSlotPolicy = CenterSlotPolicy::PriorityQueue; // 上次全量分析所用

    bool loudnessStale() const { return loudnessMode != appliedLoudnessMode; }
    bool centerSlotStale() const { return centerSlotPolicy != appliedCenterSlotPolicy; }

    // 两项的**或**:回答「这份段表还是不是按当前设置算出来的」。
    bool stale() const { return loudnessStale() || centerSlotStale(); }

    // 一次**全量**分析完成时调用(增量/局部重算不算 —— 那不会把整份段表拉到新口径上)。
    void markApplied()
    {
        appliedLoudnessMode = loudnessMode;
        appliedCenterSlotPolicy = centerSlotPolicy;
    }
};

} // namespace scvb::analysis
