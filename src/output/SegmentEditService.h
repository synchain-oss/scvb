// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SegmentEditService —— CRVS 变更事务(全量快照 + UndoManager + 重建回调)。JUCE 依赖(UndoManager),
// 故不落 scvb_core(ADR-011 JUCE-free);落 src/output/ 供 OutputEditor 与 scvb_params_tests 共用。
// 关键修复(PR#55 缺陷3):editSegmentTransactional 先跑 editTrackSegments 判结果,仅 Ok 才压入
// undo 事务并重建;失败(BadArg/NotAdjacent)不改原表、不碰 undo、不重建 —— 避免无效 undo 步。

#include <algorithm>
#include <functional>
#include <vector>

#include <juce_data_structures/juce_data_structures.h>

#include "state/SegmentEdit.h"
#include "state/StateCodec.h"

namespace scvb::output
{

// CRVS 全量快照 undo 事务:perform 置 newData + 重建,undo 置 oldData + 重建。
class CrvsTransactionAction final : public juce::UndoableAction
{
public:
    CrvsTransactionAction(scvb::state::CrvsData& crvs, scvb::state::CrvsData oldData, scvb::state::CrvsData newData,
                          std::function<void()> rebuild)
        : crvs_(crvs), oldData_(std::move(oldData)), newData_(std::move(newData)), rebuild_(std::move(rebuild))
    {
    }

    bool perform() override
    {
        crvs_ = newData_;
        if (rebuild_)
            rebuild_();
        return true;
    }

    bool undo() override
    {
        crvs_ = oldData_;
        if (rebuild_)
            rebuild_();
        return true;
    }

    int getSizeInUnits() override { return 1; }

private:
    scvb::state::CrvsData& crvs_;
    scvb::state::CrvsData oldData_;
    scvb::state::CrvsData newData_;
    std::function<void()> rebuild_;
};

// 通用 CRVS 变更事务(无条件成功,如 setVersionName/copyVersion/setTrackManual/setPanCurve)。
inline void commitCrvsTransaction(juce::UndoManager& undo, scvb::state::CrvsData& crvs, const juce::String& name,
                                  const std::function<void()>& mutator, const std::function<void()>& rebuild)
{
    const scvb::state::CrvsData oldData = crvs;
    mutator(); // 就地改 crvs
    const scvb::state::CrvsData newData = crvs;
    undo.beginNewTransaction(name);
    undo.perform(new CrvsTransactionAction(crvs, oldData, newData, rebuild));
}

// setTrackManual(契约 §1.16 / 04 §1.5 方案 A)的产物:覆盖全时间线的单段 user_edited 常值。
//
// **pan 与 vol 是同一条常值段上的两个独立维度**,而 UI 的读回值对两维读的是同一段
// (tab-tracks.js manualConstantOf)。所以写一维时必须原样保留另一维 —— 早先的实现把另一维
// 硬写成默认值(pan 写 0、vol 写 0dB),于是「先调 vol 再调 pan」会把 vol 打回 0dB、反过来
// 把 pan 打回居中,两个维度互相冲掉(T37 三轮 D 族)。
//
// existing = 该轨替换前的段表:非空则从首段继承另一维;空表(从未编辑/分析过)才落各自默认。
// 纯函数,可离线断言。
inline scvb::state::Segment makeManualConstantSegment(const std::vector<scvb::state::Segment>& existing, bool isPan,
                                                      float value)
{
    float keepPan = 0.0f;
    float keepVolDb = 0.0f;
    if (!existing.empty())
    {
        keepPan = existing.front().pan;
        keepVolDb = existing.front().volDb;
    }

    scvb::state::Segment seg;
    seg.t0 = 0;
    seg.t1 = static_cast<std::int64_t>(1) << 40; // 覆盖全时间线近似(真末端由宿主时间线提供)
    seg.pan = isPan ? std::clamp(value, -100.0f, 100.0f) : keepPan;
    seg.volDb = isPan ? keepVolDb : std::clamp(value, -24.0f, 12.0f);
    seg.flags = scvb::state::makeSegmentFlags(scvb::state::SegmentOrigin::UserEdited, false);
    return seg;
}

// 结果门控的段编辑事务:先 editTrackSegments 判结果,仅 Ok 才压入 undo 事务并重建;
// 失败(BadArg/NotAdjacent)直接返回,不改原表、不碰 undo、不重建(PR#55 缺陷3)。
inline scvb::state::SegmentEditResult editSegmentTransactional(juce::UndoManager& undo, scvb::state::CrvsData& crvs,
                                                               int version, int track,
                                                               const scvb::state::SegmentEditArgs& args,
                                                               const std::function<void()>& rebuild)
{
    const scvb::state::CrvsData oldData = crvs;
    auto& segments =
        crvs.versions[static_cast<std::size_t>(version - 1)].tracks[static_cast<std::size_t>(track)].segments;
    const scvb::state::SegmentEditResult result = scvb::state::editTrackSegments(segments, args);
    if (result != scvb::state::SegmentEditResult::Ok)
        return result; // 强异常安全:失败不改 segments

    const scvb::state::CrvsData newData = crvs;
    undo.beginNewTransaction("Edit segment");
    undo.perform(new CrvsTransactionAction(crvs, oldData, newData, rebuild));
    return scvb::state::SegmentEditResult::Ok;
}

} // namespace scvb::output
