// SPDX-License-Identifier: GPL-3.0-or-later
// test_registry_probe —— T29 [J70] 跨组只读探测(RegistryProbe)单测(仅 WIN32,命名共享内存)。
// 覆盖:异组 OutputSlot 活跃 + 心跳新鲜 → online;state!=active → offline;不存在段 → offline(降级不报错)。

#include <catch2/catch_test_macros.hpp>

#include "ipc/Registry.h"
#include "ipc/RegistryProbe.h"
#include "ipc/SegmentBackendWin32.h"

namespace
{
constexpr scvb::u32 kProbeGroup = 6; // 避开默认组 1;测试进程独占
constexpr scvb::u32 kPid = 0x1234;
} // namespace

TEST_CASE("REGISTRY-PROBE-1 异组 OutputSlot 活跃 + 心跳新鲜 → online", "[registry][probe]")
{
    scvb::SegmentBackendWin32 backend;
    scvb::Registry registry(backend, kProbeGroup);
    REQUIRE(registry.open() == scvb::Registry::ClaimResult::kClaimed);

    const scvb::u64 now = scvb::steadyNowMs();
    REQUIRE(registry.claimOutput(kPid, now) == scvb::Registry::ClaimResult::kClaimed);
    registry.heartbeatOutput(now);

    REQUIRE(scvb::probeRegistryGroupOnline(kProbeGroup, now)); // 心跳新鲜(≤2000ms)

    registry.releaseOutput(kPid);
}

TEST_CASE("REGISTRY-PROBE-2 段存在但 state!=active → offline", "[registry][probe]")
{
    scvb::SegmentBackendWin32 backend;
    scvb::Registry registry(backend, kProbeGroup);
    REQUIRE(registry.open() == scvb::Registry::ClaimResult::kClaimed);
    // 不 claim output(state 保持 0)→ 探测应为 offline。
    REQUIRE_FALSE(scvb::probeRegistryGroupOnline(kProbeGroup, scvb::steadyNowMs()));
}

TEST_CASE("REGISTRY-PROBE-3 段不存在 → offline(降级不报错)", "[registry][probe]")
{
    // 组 5 在本测试进程从未创建 → OpenFileMappingW 失败 → false。
    REQUIRE_FALSE(scvb::probeRegistryGroupOnline(5, scvb::steadyNowMs()));
}
