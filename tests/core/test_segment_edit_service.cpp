// SPDX-License-Identifier: GPL-3.0-or-later
// test_segment_edit_service —— T29 CRVS 变更事务(结果门控)单测(链接 JUCE UndoManager,离线 Catch2)。
// 覆盖 PR#55 缺陷3:editSegmentTransactional 失败(BadArg/NotAdjacent)不改 CRVS、不新增 undo 步、不触发
// rebuild;成功才压入事务,undo 还原 + 再 rebuild。另覆盖 observer 拒绝的「守卫前置」(见 SERVICE-4,事务层
// 不感知 observer,observer 拒绝由 OutputEditor::isReadOnly 前置,本测试只验证事务层不越权)。

#include <catch2/catch_test_macros.hpp>

#include <juce_data_structures/juce_data_structures.h>

#include "SegmentEditService.h"
#include "state/SegmentEdit.h"
#include "state/StateCodec.h"

namespace
{

using scvb::state::CrvsData;
using scvb::state::makeSegmentFlags;
using scvb::state::Segment;
using scvb::state::SegmentEditArgs;
using scvb::state::SegmentEditOp;
using scvb::state::SegmentEditResult;
using scvb::state::SegmentOrigin;

Segment seg(std::int64_t t0, std::int64_t t1, float pan)
{
    Segment s;
    s.t0 = t0;
    s.t1 = t1;
    s.pan = pan;
    s.flags = makeSegmentFlags(SegmentOrigin::Auto, false);
    return s;
}

} // namespace

TEST_CASE("SERVICE-1 不相邻合并失败:不改 CRVS/不新增 undo/不 rebuild", "[segedit][service]")
{
    juce::UndoManager undo;
    CrvsData crvs;
    crvs.versions[0].tracks[0].segments = {seg(0, 1000, 10.0f), seg(1000, 2000, 20.0f), seg(2000, 3000, 30.0f)};
    const std::size_t beforeSize = crvs.versions[0].tracks[0].segments.size();

    int rebuilds = 0;
    SegmentEditArgs args;
    args.op = SegmentEditOp::Merge;
    args.segIdx = 0;
    args.segIdxB = 2; // 不相邻 → NotAdjacent

    const auto r = scvb::output::editSegmentTransactional(undo, crvs, 1, 0, args, [&] { ++rebuilds; });

    REQUIRE(r == SegmentEditResult::NotAdjacent);
    REQUIRE(rebuilds == 0);
    REQUIRE(crvs.versions[0].tracks[0].segments.size() == beforeSize); // 段表未变
    REQUIRE_FALSE(undo.canUndo()); // 无新增 undo 步
}

TEST_CASE("SERVICE-2 越界失败:不新增 undo/不 rebuild", "[segedit][service]")
{
    juce::UndoManager undo;
    CrvsData crvs;
    crvs.versions[0].tracks[0].segments = {seg(0, 1000, 10.0f)};

    int rebuilds = 0;
    SegmentEditArgs args;
    args.op = SegmentEditOp::SetValues;
    args.segIdx = 9; // 越界
    args.hasPan = true;
    args.pan = 42.0f;

    const auto r = scvb::output::editSegmentTransactional(undo, crvs, 1, 0, args, [&] { ++rebuilds; });

    REQUIRE(r == SegmentEditResult::BadArg);
    REQUIRE(rebuilds == 0);
    REQUIRE(crvs.versions[0].tracks[0].segments[0].pan == 10.0f);
    REQUIRE_FALSE(undo.canUndo());
}

TEST_CASE("SERVICE-3 成功压事务:perform/undo 各 rebuild 一次且值往返", "[segedit][service]")
{
    juce::UndoManager undo;
    CrvsData crvs;
    crvs.versions[0].tracks[0].segments = {seg(0, 1000, 10.0f)};

    int rebuilds = 0;
    SegmentEditArgs args;
    args.op = SegmentEditOp::SetValues;
    args.segIdx = 0;
    args.hasPan = true;
    args.pan = 42.0f;

    const auto r = scvb::output::editSegmentTransactional(undo, crvs, 1, 0, args, [&] { ++rebuilds; });

    REQUIRE(r == SegmentEditResult::Ok);
    REQUIRE(rebuilds == 1); // perform 触发一次 rebuild
    REQUIRE(undo.canUndo());
    REQUIRE(crvs.versions[0].tracks[0].segments[0].pan == 42.0f);

    REQUIRE(undo.undo());
    REQUIRE(rebuilds == 2); // undo 再触发一次 rebuild
    REQUIRE(crvs.versions[0].tracks[0].segments[0].pan == 10.0f); // 还原旧值

    REQUIRE(undo.redo());
    REQUIRE(crvs.versions[0].tracks[0].segments[0].pan == 42.0f); // redo 重放
}

TEST_CASE("SERVICE-4 通用 commitCrvsTransaction:undo 还原 meta.name", "[segedit][service]")
{
    juce::UndoManager undo;
    CrvsData crvs;
    crvs.versions[0].meta.name = "V1";

    scvb::output::commitCrvsTransaction(undo, crvs, "Rename V1", [&] { crvs.versions[0].meta.name = "Lead"; }, [] {});

    REQUIRE(crvs.versions[0].meta.name == "Lead");
    REQUIRE(undo.canUndo());
    REQUIRE(undo.undo());
    REQUIRE(crvs.versions[0].meta.name == "V1"); // 还原
}

// ---------------------------------------------------------------------------
// T37 三轮 D 族回归:setTrackManual 的两个维度不得互相冲掉。
// 真机症状:「改了音量再改 pan,音量退回默认;先 pan 后音量,pan 回居中」。
// 根因是构造常值段时把**另一维**硬写成默认值,而 UI 的读回值对 pan/vol 读的是同一段。
// ---------------------------------------------------------------------------

TEST_CASE("SERVICE-5 makeManualConstantSegment:写 pan 保留既有 vol", "[segedit][service][t37]")
{
    std::vector<Segment> existing;
    Segment prev;
    prev.t0 = 0;
    prev.t1 = static_cast<std::int64_t>(1) << 40;
    prev.pan = 0.0f;
    prev.volDb = -6.0f; // 用户先调过音量
    prev.flags = makeSegmentFlags(SegmentOrigin::UserEdited, false);
    existing.push_back(prev);

    const Segment seg = scvb::output::makeManualConstantSegment(existing, /*isPan=*/true, 40.0f);

    REQUIRE(seg.pan == 40.0f);
    REQUIRE(seg.volDb == -6.0f); // ← 修复前这里是 0.0f(音量被打回默认)
    REQUIRE(scvb::state::segmentOrigin(seg.flags) == SegmentOrigin::UserEdited);
    REQUIRE(seg.t0 == 0);
}

TEST_CASE("SERVICE-6 makeManualConstantSegment:写 vol 保留既有 pan", "[segedit][service][t37]")
{
    std::vector<Segment> existing;
    Segment prev;
    prev.t0 = 0;
    prev.t1 = static_cast<std::int64_t>(1) << 40;
    prev.pan = -75.0f; // 用户先调过 pan
    prev.volDb = 0.0f;
    prev.flags = makeSegmentFlags(SegmentOrigin::UserEdited, false);
    existing.push_back(prev);

    const Segment seg = scvb::output::makeManualConstantSegment(existing, /*isPan=*/false, 3.5f);

    REQUIRE(seg.volDb == 3.5f);
    REQUIRE(seg.pan == -75.0f); // ← 修复前这里是 0.0f(pan 回居中)
}

TEST_CASE("SERVICE-7 makeManualConstantSegment:空表落默认 + 越界钳制", "[segedit][service][t37]")
{
    const std::vector<Segment> empty;

    const Segment a = scvb::output::makeManualConstantSegment(empty, /*isPan=*/true, 999.0f);
    REQUIRE(a.pan == 100.0f); // 钳到 +100
    REQUIRE(a.volDb == 0.0f); // 空表 → vol 默认 0dB

    const Segment b = scvb::output::makeManualConstantSegment(empty, /*isPan=*/false, -999.0f);
    REQUIRE(b.volDb == -24.0f); // 钳到 -24dB
    REQUIRE(b.pan == 0.0f); // 空表 → pan 默认居中
}

TEST_CASE("SERVICE-8 makeManualConstantSegment:交替写两维互不干扰(真机复现序列)", "[segedit][service][t37]")
{
    std::vector<Segment> segs; // 从空表起步

    // 先调音量 → 再调 pan → 再调音量:三步之后两维都应是最后一次写入的值。
    segs.assign(1, scvb::output::makeManualConstantSegment(segs, /*isPan=*/false, -8.0f));
    REQUIRE(segs[0].volDb == -8.0f);

    segs.assign(1, scvb::output::makeManualConstantSegment(segs, /*isPan=*/true, 55.0f));
    REQUIRE(segs[0].pan == 55.0f);
    REQUIRE(segs[0].volDb == -8.0f); // 音量没被 pan 冲掉

    segs.assign(1, scvb::output::makeManualConstantSegment(segs, /*isPan=*/false, 2.0f));
    REQUIRE(segs[0].volDb == 2.0f);
    REQUIRE(segs[0].pan == 55.0f); // pan 没被音量冲掉
}
