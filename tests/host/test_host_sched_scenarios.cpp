// SPDX-License-Identifier: GPL-3.0-or-later
// test_host_sched_scenarios —— 宿主调度复现场景 LS-1…15 的落点(A 线 A-4 在这里展开;
// 修复前的红证据用 `[!shouldfail]` 留在这里,修复卡去掉标记后转绿)。
//
// 本文件里的用例一律打 `[.][sched]`(理由见 test_host_sched.cpp 头注):主条目 `scvb_host_tests`
// 默认不跑,ctest 条目 `scvb_host_sched_tests` 显式选中。
// A-1 只放一条最小用例:在 host 目标里确认在场判据对「读到上一圈旧数据」判 Wrong ——
// 场景卡要用的正是这一档判定。

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "support/presence_meter.h"

TEST_CASE("SCHED scenarios scaffold: a lane read one ring lap stale is judged Wrong inside the host target",
          "[.][sched]")
{
    namespace presence = scvb::testsupport::presence;
    constexpr int kFrames = 16;
    const std::int64_t len = kFrames * static_cast<std::int64_t>(presence::kFrame);
    const std::int64_t t0 = 2 * presence::kRingR;

    std::vector<float> left(static_cast<std::size_t>(len), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(len), 0.0f);
    // lane 0 正常;lane 1 的内容来自时间线 t - R(环里上一圈留下的旧数据)。
    presence::addLane(0, t0, left.data(), len, 0.7);
    presence::addLane(0, t0, right.data(), len, 0.7);
    presence::addLane(1, t0 - presence::kRingR, left.data(), len, 0.7);
    presence::addLane(1, t0 - presence::kRingR, right.data(), len, 0.7);

    const presence::CombReport rep = presence::analyze(presence::Span{left.data(), right.data(), t0, len}, {0, 1});
    REQUIRE(rep.lanes.size() == 2u);
    REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));
    CHECK(rep.lanes[0].count(presence::Verdict::Present) == kFrames);
    CHECK(rep.lanes[1].count(presence::Verdict::Wrong) == kFrames);
}
