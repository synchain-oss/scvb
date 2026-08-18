// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SegmentEdit —— 段级编辑五个 op 的纯数据操作(契约 §1.22 / §5.4;05 §2.3a)。纯 C++17,无 JUCE(ADR-011)。
// 作用对象 = state CRVS 的段真身(scvb::state::Segment:样本域 t0/t1 + pan/volDb + origin/locked flags)。
// 本层只做「就地改段表 + 结果判定」,不碰 UndoManager、不碰秒↔样本换算、不发事件 —— 事务快照/撤销
// 与事件回推由 OutputEditor 负责。重分析不覆盖 origin≠auto 或 locked 段(ADR-008 v1.1)由上游保证,
// 本层只在值/边界编辑后置 origin=user_edited 且 locked=true([J34]/[J44])。

#include <cstdint>
#include <vector>

#include "StateCodec.h"

namespace scvb::state
{

// 编辑操作(契约 §5.4 五值)。
enum class SegmentEditOp
{
    MoveBoundary, // move_boundary
    Split, // split
    Merge, // merge
    SetValues, // set_values
    SetLocked, // set_locked
};

// 编辑结果。
enum class SegmentEditResult
{
    Ok = 0,
    BadArg = 1, // segIdx 越界 / payload 缺失 / 区间非法
    NotAdjacent = 2, // merge 两段不相邻
};

// 编辑参数(样本域)。字段语义见契约 §5.4。
struct SegmentEditArgs
{
    SegmentEditOp op = SegmentEditOp::SetValues;
    int segIdx = -1; // 0 基段下标;move_boundary/split/set_values/set_locked 用
    int segIdxB = -1; // merge 第二段下标
    bool edgeIsT0 = false; // move_boundary:true=移动 t0 边,false=移动 t1 边
    std::int64_t tSamples = 0; // move_boundary/split 的目标样本位置
    bool hasPan = false; // set_values:是否给 pan
    float pan = 0.0f; // set_values:pan(-100..100,调用方已夹取)
    bool hasVol = false; // set_values:是否给 volDb
    float volDb = 0.0f; // set_values:volDb(-24..+12,调用方已夹取)
    bool locked = false; // set_locked:目标 locked 值
};

// 编辑单轨段列表(样本域,按 t0 升序、互不重叠)。成功就地修改 segments 并返回 Ok;
// 失败返回 BadArg/NotAdjacent,segments 保持原样(强异常安全:先全量校验再写)。
// 后置:move_boundary/split/set_values 命中段 origin=UserEdited 且 locked=true;merge 结果段
// origin=UserEdited 且 locked=true(合并属边界编辑);set_locked 仅改 locked 不动 origin。
SegmentEditResult editTrackSegments(std::vector<Segment>& segments, const SegmentEditArgs& args);

} // namespace scvb::state
