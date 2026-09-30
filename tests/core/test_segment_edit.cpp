// SPDX-License-Identifier: GPL-3.0-or-later
// test_segment_edit —— T29 段级编辑五个 op 的纯数据单测(契约 §1.22 / §5.4)。
// 覆盖:move_boundary(邻段收缩 + 越界拒绝)、split(继承值 + 后置 origin/locked)、merge(相邻判定 +
// 时长加权)、set_values(可选字段 + 至少一个)、set_locked(不改 origin)、失败不改原表。无 JUCE(ADR-011)。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "state/SegmentEdit.h"
#include "state/StateCodec.h"

using Catch::Approx;

namespace
{

using scvb::state::makeSegmentFlags;
using scvb::state::Segment;
using scvb::state::SegmentEditArgs;
using scvb::state::SegmentEditOp;
using scvb::state::SegmentEditResult;
using scvb::state::segmentLocked;
using scvb::state::SegmentOrigin;
using scvb::state::segmentOrigin;

// 造一段 auto 段(样本域)。
Segment seg(std::int64_t t0, std::int64_t t1, float pan, float volDb)
{
    Segment s;
    s.t0 = t0;
    s.t1 = t1;
    s.pan = pan;
    s.volDb = volDb;
    s.flags = makeSegmentFlags(SegmentOrigin::Auto, false);
    return s;
}

// 三段的轨:[0,1000) pan=10,[1000,2000) pan=20,[2000,3000) pan=30。
std::vector<Segment> three()
{
    return {seg(0, 1000, 10.0f, 0.0f), seg(1000, 2000, 20.0f, -1.0f), seg(2000, 3000, 30.0f, -2.0f)};
}

bool userEditedLocked(const Segment& s)
{
    return segmentOrigin(s.flags) == SegmentOrigin::UserEdited && segmentLocked(s.flags);
}

} // namespace

// ============================================================================
// move_boundary
// ============================================================================

TEST_CASE("SEGEDIT-MOVE-1 移 t1 边:邻段随之收缩", "[segedit][move]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::MoveBoundary;
    a.segIdx = 0;
    a.edgeIsT0 = false; // 移 t1 边
    a.tSamples = 500;

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    REQUIRE(v.size() == 3u);
    REQUIRE(v[0].t1 == 500); // 段 0 收缩
    REQUIRE(v[1].t0 == 500); // 邻段(段 1)t0 随之
    REQUIRE(v[1].t1 == 2000);
    REQUIRE(userEditedLocked(v[0])); // 被移动段 origin=user_edited+locked
    REQUIRE(segmentOrigin(v[1].flags) == SegmentOrigin::Auto); // 邻段不改 origin
}

TEST_CASE("SEGEDIT-MOVE-2 移 t0 边:前邻段收缩", "[segedit][move]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::MoveBoundary;
    a.segIdx = 1;
    a.edgeIsT0 = true; // 移 t0 边
    a.tSamples = 1500;

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    REQUIRE(v[0].t1 == 1500); // 前邻收缩
    REQUIRE(v[1].t0 == 1500);
    REQUIRE(userEditedLocked(v[1]));
}

TEST_CASE("SEGEDIT-MOVE-3 首段移 t0 边(无前邻)不越界", "[segedit][move]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::MoveBoundary;
    a.segIdx = 0;
    a.edgeIsT0 = true;
    a.tSamples = 900; // < t1=1000 合法

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    REQUIRE(v[0].t0 == 900);
    REQUIRE(v[0].t1 == 1000);
}

TEST_CASE("SEGEDIT-MOVE-4 越界拒绝(tS 不小于本段另一端)", "[segedit][move]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::MoveBoundary;
    a.segIdx = 0;
    a.edgeIsT0 = false; // 移 t1 边,需 tS > t0
    a.tSamples = 0; // == t0 → 非法(须严格大于 t0)

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::BadArg);
    REQUIRE(v[0].t0 == 0); // 原表不变
    REQUIRE(v[0].t1 == 1000);
}

TEST_CASE("SEGEDIT-MOVE-5 越界拒绝(移 t1 越过邻段 t1)", "[segedit][move]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::MoveBoundary;
    a.segIdx = 0;
    a.edgeIsT0 = false;
    a.tSamples = 2500; // 越过邻段(段 1)t1=2000

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::BadArg);
    REQUIRE(v[1].t0 == 1000); // 邻段不变
}

// ============================================================================
// split
// ============================================================================

TEST_CASE("SEGEDIT-SPLIT-1 分割继承值 + 两子段 origin=user_edited+locked", "[segedit][split]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::Split;
    a.segIdx = 1;
    a.tSamples = 1500;

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    REQUIRE(v.size() == 4u);
    REQUIRE(v[1].t0 == 1000);
    REQUIRE(v[1].t1 == 1500);
    REQUIRE(v[2].t0 == 1500);
    REQUIRE(v[2].t1 == 2000);
    REQUIRE(v[1].pan == Approx(20.0f)); // 继承原值
    REQUIRE(v[2].pan == Approx(20.0f));
    REQUIRE(userEditedLocked(v[1]));
    REQUIRE(userEditedLocked(v[2]));
}

TEST_CASE("SEGEDIT-SPLIT-2 切点在段外拒绝", "[segedit][split]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::Split;
    a.segIdx = 1;
    a.tSamples = 1000; // 恰在 t0 上,不严格在段内

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::BadArg);
    REQUIRE(v.size() == 3u);
}

// ============================================================================
// merge
// ============================================================================

TEST_CASE("SEGEDIT-MERGE-1 相邻合并按时长加权", "[segedit][merge]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::Merge;
    a.segIdx = 0;
    a.segIdxB = 1;

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    REQUIRE(v.size() == 2u);
    REQUIRE(v[0].t0 == 0);
    REQUIRE(v[0].t1 == 2000);
    // 等长:加权平均 = (10+20)/2 = 15。
    REQUIRE(v[0].pan == Approx(15.0f));
    REQUIRE(v[0].volDb == Approx(-0.5f));
    REQUIRE(userEditedLocked(v[0]));
}

TEST_CASE("SEGEDIT-MERGE-2 不等长按加权(前短后长)", "[segedit][merge]")
{
    std::vector<Segment> v;
    // 段 0 长 100,段 1 长 300。
    v.push_back(seg(0, 100, 0.0f, 0.0f));
    v.push_back(seg(100, 400, 40.0f, 0.0f));

    SegmentEditArgs a;
    a.op = SegmentEditOp::Merge;
    a.segIdx = 0;
    a.segIdxB = 1;

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    REQUIRE(v.size() == 1u);
    REQUIRE(v[0].t1 == 400);
    // (0*100 + 40*300)/400 = 30。
    REQUIRE(v[0].pan == Approx(30.0f));
}

TEST_CASE("SEGEDIT-MERGE-3 不相邻拒绝(notAdjacent)", "[segedit][merge]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::Merge;
    a.segIdx = 0;
    a.segIdxB = 2; // 隔一个段

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::NotAdjacent);
    REQUIRE(v.size() == 3u); // 原表不变
}

TEST_CASE("SEGEDIT-MERGE-4 越界拒绝", "[segedit][merge]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::Merge;
    a.segIdx = 0;
    a.segIdxB = 5;

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::BadArg);
}

// ============================================================================
// set_values
// ============================================================================

TEST_CASE("SEGEDIT-VALUES-1 只改 pan", "[segedit][values]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::SetValues;
    a.segIdx = 1;
    a.hasPan = true;
    a.pan = 42.0f;

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    REQUIRE(v[1].pan == Approx(42.0f));
    REQUIRE(v[1].volDb == Approx(-1.0f)); // 未给字段不变
    REQUIRE(userEditedLocked(v[1]));
}

TEST_CASE("SEGEDIT-VALUES-2 两字段都给", "[segedit][values]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::SetValues;
    a.segIdx = 1;
    a.hasPan = true;
    a.pan = 42.0f;
    a.hasVol = true;
    a.volDb = -6.0f;

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    REQUIRE(v[1].pan == Approx(42.0f));
    REQUIRE(v[1].volDb == Approx(-6.0f));
}

TEST_CASE("SEGEDIT-VALUES-3 两个字段都不给拒绝", "[segedit][values]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::SetValues;
    a.segIdx = 1;

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::BadArg);
}

TEST_CASE("SEGEDIT-VALUES-4 越界拒绝", "[segedit][values]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::SetValues;
    a.segIdx = 9;
    a.hasPan = true;
    a.pan = 42.0f;

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::BadArg);
}

// ============================================================================
// set_locked
// ============================================================================

TEST_CASE("SEGEDIT-LOCK-1 加锁不改 origin", "[segedit][lock]")
{
    auto v = three();
    SegmentEditArgs a;
    a.op = SegmentEditOp::SetLocked;
    a.segIdx = 0;
    a.locked = true;

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    REQUIRE(segmentLocked(v[0].flags));
    REQUIRE(segmentOrigin(v[0].flags) == SegmentOrigin::Auto); // 不改 origin
}

TEST_CASE("SEGEDIT-LOCK-2 解锁", "[segedit][lock]")
{
    auto v = three();
    v[0].flags = makeSegmentFlags(SegmentOrigin::UserEdited, true);

    SegmentEditArgs a;
    a.op = SegmentEditOp::SetLocked;
    a.segIdx = 0;
    a.locked = false;

    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    REQUIRE_FALSE(segmentLocked(v[0].flags));
    REQUIRE(segmentOrigin(v[0].flags) == SegmentOrigin::UserEdited); // origin 保留
}

// ============================================================================
// [SL-548 / J162] 手动位(flags bit3 = pan / bit4 = vol)的维护
// ============================================================================
// set_locked 保留;set_values / split / move_boundary / merge 清掉**被编辑段**的位(整字改写
// flags,见 SegmentEdit.cpp `userEditedLockedFlags` 注)。别的段一律不动 —— 尤其 move_boundary 随之
// 收缩的邻段:它的边界变了,但 flags 没被这个 op 碰过。
// 夹具:三段都像刚被手动接管过 vol(user_edited、未锁、带 vol 位),首段另带 pan 位 —— 用来区分
// 「保留整组位」与「只保留某一位」。每格各钉一个 op,删除式逐 op 注入(变更文档 ND 表)。

namespace
{
using scvb::state::kSegmentManualMask;
using scvb::state::kSegmentManualPanBit;
using scvb::state::kSegmentManualVolBit;

std::vector<Segment> threeManual()
{
    auto v = three();
    for (auto& s : v)
        s.flags = makeSegmentFlags(SegmentOrigin::UserEdited, false) | kSegmentManualVolBit;
    v[0].flags |= kSegmentManualPanBit;
    return v;
}

std::uint32_t manualBits(const Segment& s)
{
    return s.flags & kSegmentManualMask;
}
} // namespace

TEST_CASE("SEGEDIT-MANUAL-1 set_locked 保留手动位(加锁、解锁都不动)", "[segedit][lock][SL548]")
{
    auto v = threeManual();
    SegmentEditArgs a;
    a.op = SegmentEditOp::SetLocked;
    a.segIdx = 0;
    a.locked = true;
    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    CHECK(segmentLocked(v[0].flags));
    CHECK(segmentOrigin(v[0].flags) == SegmentOrigin::UserEdited);
    CHECK(manualBits(v[0]) == kSegmentManualMask); // ← 整字重建(只拼 origin + locked)时这里是 0

    a.locked = false;
    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    CHECK_FALSE(segmentLocked(v[0].flags));
    CHECK(manualBits(v[0]) == kSegmentManualMask);

    a.segIdx = 1;
    a.locked = true;
    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    CHECK(manualBits(v[1]) == kSegmentManualVolBit); // 只保留自己有的那一位,不多不少
}

TEST_CASE("SEGEDIT-MANUAL-2 set_values 清被编辑段的手动位,别的段不动", "[segedit][values][SL548]")
{
    auto v = threeManual();
    SegmentEditArgs a;
    a.op = SegmentEditOp::SetValues;
    a.segIdx = 0;
    a.hasVol = true;
    a.volDb = -5.0f;
    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    CHECK(manualBits(v[0]) == 0u);
    CHECK(userEditedLocked(v[0]));
    CHECK(manualBits(v[1]) == kSegmentManualVolBit);
    CHECK(manualBits(v[2]) == kSegmentManualVolBit);
}

TEST_CASE("SEGEDIT-MANUAL-3 split 清两个子段的手动位", "[segedit][split][SL548]")
{
    auto v = threeManual();
    SegmentEditArgs a;
    a.op = SegmentEditOp::Split;
    a.segIdx = 0;
    a.tSamples = 500;
    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    REQUIRE(v.size() == 4);
    CHECK(manualBits(v[0]) == 0u);
    CHECK(manualBits(v[1]) == 0u);
    CHECK(manualBits(v[2]) == kSegmentManualVolBit); // 原第 2 段不动
}

TEST_CASE("SEGEDIT-MANUAL-4 move_boundary 清本段的手动位,随之收缩的邻段不动", "[segedit][move][SL548]")
{
    auto v = threeManual();
    SegmentEditArgs a;
    a.op = SegmentEditOp::MoveBoundary;
    a.segIdx = 0;
    a.edgeIsT0 = false;
    a.tSamples = 1500;
    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    REQUIRE(v[1].t0 == 1500); // 前置:邻段真的随之收缩了
    CHECK(manualBits(v[0]) == 0u);
    CHECK(manualBits(v[1]) == kSegmentManualVolBit);
    CHECK(manualBits(v[2]) == kSegmentManualVolBit);
}

TEST_CASE("SEGEDIT-MANUAL-5 merge 的结果段不带手动位", "[segedit][merge][SL548]")
{
    auto v = threeManual();
    SegmentEditArgs a;
    a.op = SegmentEditOp::Merge;
    a.segIdx = 0;
    a.segIdxB = 1;
    REQUIRE(editTrackSegments(v, a) == SegmentEditResult::Ok);
    REQUIRE(v.size() == 2);
    CHECK(manualBits(v[0]) == 0u); // 两段都带 vol 位也不例外:合并属段编辑,按 [J162] ③ 清位
    CHECK(manualBits(v[1]) == kSegmentManualVolBit);
}
