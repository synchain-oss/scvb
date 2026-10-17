// SPDX-License-Identifier: GPL-3.0-or-later
// test_registry_probe —— T29 [J70] 跨组只读探测单测。文件名沿用 T29(B 线 M03 起不再改名):
// 被测对象已从只限 Win32 的 RegistryProbe 换成经后端抽象的 GroupProbe::probeGroupsOnline ——
// Output 的跨组绿点自此与 Input / Monitor 同走这一处。
// 覆盖:异组 OutputSlot 活跃 + 心跳新鲜 → online;state!=active → offline;不存在段 → offline(降级不报错);
// 本组位默认不探测;段尺寸不足以容纳 OutputSlot → offline(RegistryProbe 迁来的尺寸校验)。
// PROBE-1..3 走 PlatformSegmentBackend(本目标今天只在 WIN32 下编 = 真命名共享内存);
// PROBE-4 走 InProcess 后端 —— 只有它能造出「比 OutputSlot 末端短」的段(Win32 视图按页取整)。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "ipc/GroupProbe.h"
#include "ipc/PlatformSegmentBackend.h"
#include "ipc/Registry.h"
#include "ipc/SegmentBackendInProcess.h"

namespace
{
constexpr scvb::u32 kProbeGroup = 6; // 避开默认组 1;测试进程独占
constexpr scvb::u32 kAbsentGroup = 5; // 本测试进程从不创建
constexpr scvb::u32 kOwnGroup = 1; // 探测方自称的本组(与上面两组都不同)
constexpr scvb::u32 kPid = 0x1234;

bool bitOf(std::uint8_t bits, scvb::u32 group)
{
    return (bits & (1u << (group - 1))) != 0;
}

// 与 Registry / GroupProbe 内部同构的段全名(InProcess 后端以它为键)。
std::wstring registryName(scvb::u32 group)
{
    return L"Local\\" + scvb::segmentLogicalName(group, scvb::SegmentKind::kRegistry);
}

// 直接在一块 InProcess 段上摆出「magic/abi 合法 + OutputSlot 活跃 + 心跳新鲜」的形态。
// size 只需覆盖到 OutputSlot::heartbeat_ms 末端;写入的字节都落在 size 以内。
void forgeOnlineRegistry(scvb::SegmentBackendInProcess& backend, scvb::u32 group, std::size_t size, scvb::u64 now)
{
    scvb::SegmentView view;
    REQUIRE(backend.createOrOpen(registryName(group), size, view) == scvb::InitResult::kOk);
    REQUIRE(view.size == size);
    auto* base = static_cast<char*>(view.base);
    auto* hdr = reinterpret_cast<scvb::RegistryHeader*>(base);
    hdr->abi.store(scvb::kScvbAbi, std::memory_order_relaxed);
    hdr->magic.store(scvb::kScvbMagic, std::memory_order_release);
    auto* os = reinterpret_cast<scvb::OutputSlot*>(base + scvb::kOutputSlotOffset);
    os->heartbeat_ms.store(now, std::memory_order_relaxed);
    os->state.store(scvb::kSlotActive, std::memory_order_release);
    backend.unmap(view);
}
} // namespace

TEST_CASE("REGISTRY-PROBE-1 异组 OutputSlot 活跃 + 心跳新鲜 → online", "[registry][probe]")
{
    scvb::PlatformSegmentBackend backend;
    scvb::Registry registry(backend, kProbeGroup);
    REQUIRE(registry.open() == scvb::Registry::ClaimResult::kClaimed);

    const scvb::u64 now = scvb::steadyNowMs();
    REQUIRE(registry.claimOutput(kPid, now) == scvb::Registry::ClaimResult::kClaimed);
    registry.heartbeatOutput(now);

    CHECK(bitOf(scvb::probeGroupsOnline(backend, kOwnGroup, now), kProbeGroup)); // 心跳新鲜(≤2000ms)
    // 本组位默认不探测(由调用方从本组状态填,OutputProcessor::probeGroupsOnline 即如此);
    // includeOwnGroup=true 的回退路径才探本组。
    CHECK_FALSE(bitOf(scvb::probeGroupsOnline(backend, kProbeGroup, now), kProbeGroup));
    CHECK(bitOf(scvb::probeGroupsOnline(backend, kProbeGroup, now, /*includeOwnGroup=*/true), kProbeGroup));

    registry.releaseOutput(kPid);
}

TEST_CASE("REGISTRY-PROBE-2 段存在但 state!=active → offline", "[registry][probe]")
{
    scvb::PlatformSegmentBackend backend;
    scvb::Registry registry(backend, kProbeGroup);
    REQUIRE(registry.open() == scvb::Registry::ClaimResult::kClaimed);
    // 不 claim output(state 保持 0)→ 探测应为 offline。
    CHECK_FALSE(bitOf(scvb::probeGroupsOnline(backend, kOwnGroup, scvb::steadyNowMs()), kProbeGroup));
}

TEST_CASE("REGISTRY-PROBE-3 段不存在 → offline(降级不报错)", "[registry][probe]")
{
    // kAbsentGroup 在本测试进程从未创建 → openExistingReadOnly 失败 → 该位为 0。
    scvb::PlatformSegmentBackend backend;
    CHECK_FALSE(bitOf(scvb::probeGroupsOnline(backend, kOwnGroup, scvb::steadyNowMs()), kAbsentGroup));
}

TEST_CASE("REGISTRY-PROBE-4 段尺寸不足以容纳 OutputSlot → offline(尺寸校验自 RegistryProbe 迁入)", "[registry][probe]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;
    const scvb::u64 now = scvb::steadyNowMs();

    // 截断段:刚好盖住 OutputSlot 的 state / pid / heartbeat_ms(前 16 字节),短于整个 OutputSlot。
    // 内容与在线段一模一样 —— 删掉尺寸校验时探测会读到「活跃 + 心跳新鲜」而判在线,
    // 红在下面这条 CHECK 上(读的字节都在段内,不靠越界读的偶然结果)。
    constexpr std::size_t kTruncated = scvb::kOutputSlotOffset + offsetof(scvb::OutputSlot, connected_mask);
    static_assert(kTruncated < scvb::kOutputSlotOffset + sizeof(scvb::OutputSlot), "truncated segment must be short");
    forgeOnlineRegistry(backend, kProbeGroup, kTruncated, now);

    // 对照:同样内容、足尺寸的段判在线 —— 证明上一格的「离线」只因尺寸,不因摆放方式。
    constexpr scvb::u32 kFullGroup = 7;
    forgeOnlineRegistry(backend, kFullGroup, scvb::kRegistrySegmentSize, now);

    const std::uint8_t bits = scvb::probeGroupsOnline(backend, kOwnGroup, now);
    CHECK_FALSE(bitOf(bits, kProbeGroup));
    CHECK(bitOf(bits, kFullGroup));

    scvb::SegmentBackendInProcess::resetAll();
}
