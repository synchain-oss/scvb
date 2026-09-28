// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ParamUndo —— [SL-536 / J140] 自动化参数与通道配置进插件撤销栈用到的两个动作基件。
//
// 与 SegmentEditService.h 的 CRVS 事务**同一个** juce::UndoManager(OutputAuthority 持有),
// 于是 Ctrl+Z 按时间倒序弹「最近一步」,不论那一步是段编辑、参数还是配置(单栈,见契约 §0.9)。
//
// 本头只放与 processor 无关、可离线单测的部分(scvb_params_tests 直接断言):
//   · CoalescibleUndoAction:可被「同键、窗口内」的后继编辑并吞(键盘 / 滚轮连按合成一步);
//   · ParamWriteAction:一次参数改动的旧值 / 新值(归一化),撤销 / 重做经注入的 writer 写回。
// writer 由 processor 构造:它把一次写包成 beginChangeGesture / setValueNotifyingHost /
// endChangeGesture(与用户拖旋钮是同一条宿主通路,宿主看到的就是一次正常的用户编辑),
// 并置打印器的自写标记(§3.5 层 2,车道参数的撤销不被记成 hostEcho)。

#include <functional>
#include <utility>

#include <juce_data_structures/juce_data_structures.h>

namespace scvb::output
{

class CoalescibleUndoAction : public juce::UndoableAction
{
public:
    // newer 与本动作同一个合并键(调用方已判过键与时间窗)时,把本动作的「新值」推进到 newer 的
    // 新值,旧值保持不动 —— 一连串步进撤销时回到这一串之前。类型不符返回 false(不并,调用方另起一步)。
    virtual bool absorb(const CoalescibleUndoAction& newer) = 0;
};

class ParamWriteAction final : public CoalescibleUndoAction
{
public:
    using Writer = std::function<void(float normalised)>;

    // alreadyApplied = 新值在压栈之前已经写进参数(UI 的 gesture 三段式就是这样:值由 setParam
    // 落地,到 endParamGesture 才压栈)。此时 UndoManager::perform 触发的第一次 perform() 不再写,
    // 否则宿主会平白多收一次同值 gesture。之后的重做照常写。
    ParamWriteAction(Writer writer, float oldNorm, float newNorm, bool alreadyApplied)
        : writer_(std::move(writer)), oldNorm_(oldNorm), newNorm_(newNorm), skipNextPerform_(alreadyApplied)
    {
    }

    bool perform() override
    {
        if (skipNextPerform_)
        {
            skipNextPerform_ = false;
            return true;
        }
        write(newNorm_);
        return true;
    }

    // 恒回 true:juce::UndoManager 的 ActionSet 只要有一个动作回 false 就清空整个撤销历史
    // (juce_UndoManager.cpp `undo()` 的 else 分支),参数写不写得进去都不值得赔上整条栈。
    bool undo() override
    {
        write(oldNorm_);
        return true;
    }

    int getSizeInUnits() override { return static_cast<int>(sizeof(ParamWriteAction)); }

    bool absorb(const CoalescibleUndoAction& newer) override
    {
        const auto* p = dynamic_cast<const ParamWriteAction*>(&newer);
        if (p == nullptr)
            return false;
        newNorm_ = p->newNorm_;
        return true;
    }

    // [SL-536 ③] 首次接管那一步里的冻结位占位:接管落地时还不知道 UI 随后置哪一位,先压一个
    // 旧值 == 新值的空动作,UI 的冻结 gesture 到了再把新值填进来(见 OutputProcessor 的
    // pendingTakeover_)。
    void setNewValue(float newNorm) { newNorm_ = newNorm; }
    float oldValue() const { return oldNorm_; }
    float newValue() const { return newNorm_; }

private:
    // 旧值 == 新值的一步(占位没被填上 / 连按后回到原值)什么都不写:写一次同值只会让宿主
    // 多录一个 gesture,撤销本身看不出任何变化。
    void write(float v)
    {
        if (oldNorm_ == newNorm_)
            return;
        if (writer_)
            writer_(v);
    }

    Writer writer_;
    float oldNorm_;
    float newNorm_;
    bool skipNextPerform_;
};

} // namespace scvb::output
