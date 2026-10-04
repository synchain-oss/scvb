// SPDX-License-Identifier: GPL-3.0-or-later
// test_host_sched —— 宿主调度模拟器 SchedRig 的框架自测落点(A 线 A-2 在这里展开)。
//
// 本文件里的用例一律打 `[.][sched]`:
//   · `[.]` 隐藏 ⇒ 主条目 `scvb_host_tests`(命令行不带过滤)默认不跑它们;
//   · ctest 条目 `scvb_host_sched_tests` 以 `scvb_host_tests "[sched]"` 显式选中它们。
// A-1 只做脚手架:放一条最小用例,保证 `[sched]` 过滤至少命中一条(过滤不到任何用例时
// Catch2 以非零退出,ctest 条目会红)。这条用例本身也是真判据:在 host 目标里(JUCE 编译选项、
// /W4)把在场判据跑一遍,理想两轨总线必须逐帧 Present。

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "support/presence_meter.h"

TEST_CASE("SCHED scaffold: the presence meter judges an ideal two-lane bus Present inside the host target",
          "[.][sched]")
{
    namespace presence = scvb::testsupport::presence;
    constexpr int kFrames = 16;
    const std::int64_t len = kFrames * static_cast<std::int64_t>(presence::kFrame);
    const std::int64_t t0 = 7 * static_cast<std::int64_t>(presence::kFrame);

    std::vector<float> left(static_cast<std::size_t>(len), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(len), 0.0f);
    for (int lane = 0; lane < 2; ++lane)
    {
        presence::addLane(lane, t0, left.data(), len, 0.7);
        presence::addLane(lane, t0, right.data(), len, 0.7);
    }
    const presence::CombReport rep = presence::analyze(presence::Span{left.data(), right.data(), t0, len}, {0, 1});
    REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));
    for (const presence::LaneReport& lr : rep.lanes)
    {
        INFO("lane " << lr.lane);
        CHECK(lr.count(presence::Verdict::Present) == kFrames);
    }
}
