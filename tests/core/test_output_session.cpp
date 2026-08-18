// SPDX-License-Identifier: GPL-3.0-or-later
// test_output_session —— OutputSession 生命周期单测(claim/observer/[J32] 200ms 注入延迟/
// [J66] 改组)+ OutputStateCodec 往返。用 SegmentBackendInProcess 模拟多实例(进程内,不碰全局段)。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "input/InputSession.h"
#include "ipc/SegmentBackendInProcess.h"
#include "ipc/Registry.h"
#include "output/OutputSession.h"
#include "state/OutputStateCodec.h"

using scvb::kSlotActive;
using scvb::kSlotFree;
using scvb::u32;
using scvb::input::InputClaimState;
using scvb::input::InputSession;
using scvb::output::OutputClaimState;
using scvb::output::OutputSession;

TEST_CASE("Output claim → kActive + OutputSlot 活跃", "[output][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1000) == OutputClaimState::kActive);
    REQUIRE(out.state() == OutputClaimState::kActive);

    scvb::Registry probe(backend, 1);
    REQUIRE(probe.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(probe.outputSlot()->state.load() == kSlotActive);
    REQUIRE(probe.outputSlot()->pid == 2001);
}

TEST_CASE("第二个 Output → O3 observer(只读观察)", "[output][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    OutputSession a(backend, 2001);
    REQUIRE(a.prepare(48000, 512, 1000) == OutputClaimState::kActive);

    OutputSession b(backend, 2002);
    REQUIRE(b.prepare(48000, 512, 1100) == OutputClaimState::kObserver);
    b.tick(1200); // 主实例仍活跃 → 保持 observer
    REQUIRE(b.state() == OutputClaimState::kObserver);
}

TEST_CASE("[J32] 200ms 注入延迟:muted 前不注入、≥200ms 后注入", "[output][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession in(backend, 1001);
    in.setChannelId(3);
    REQUIRE(in.prepare(48000, 512, 1, 1000) == InputClaimState::kActive);
    in.heartbeat(1100);
    float buf[16] = {};
    in.audioRing().write(0, buf, 16); // 推进 write_head

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1200) == OutputClaimState::kActive);

    out.tick(1300); // 首次上线:mask 置位,injectMask 尚未置(0 < 200ms 且无 muted)
    scvb::Registry probe(backend, 1);
    REQUIRE(probe.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE((probe.connectedMask() & (1u << 2)) != 0); // channel 3 mask 位
    REQUIRE(out.injectMask() == 0);

    out.tick(1600); // 1600-1300 = 300ms ≥ 200ms → 注入
    REQUIRE((out.injectMask() & (1u << 2)) != 0);
}

TEST_CASE("[J32] muted 确认位先到 → 立即注入", "[output][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession in(backend, 1001);
    in.setChannelId(3);
    REQUIRE(in.prepare(48000, 512, 1, 1000) == InputClaimState::kActive);
    in.heartbeat(1100);
    in.setMuted(true); // muted 确认位(C19)
    float buf[16] = {};
    in.audioRing().write(0, buf, 16);

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1200) == OutputClaimState::kActive);

    out.tick(1300);
    REQUIRE((out.injectMask() & (1u << 2)) != 0); // muted 位先到 → 立即注入(不等 200ms)
}

TEST_CASE("[J66] 改组:释放旧组 OutputSlot → 新组 claim 成功", "[output][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1000) == OutputClaimState::kActive);

    scvb::Registry probe1(backend, 1);
    REQUIRE(probe1.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(probe1.outputSlot()->state.load() == kSlotActive); // 旧组 g1 持有

    REQUIRE(out.changeGroup(2, 48000, 512, 1100) == OutputClaimState::kActive);
    REQUIRE(out.groupId() == 2);
    REQUIRE(probe1.outputSlot()->state.load() == kSlotFree); // 旧组归零

    scvb::Registry probe2(backend, 2);
    REQUIRE(probe2.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(probe2.outputSlot()->state.load() == kSlotActive); // 新组活跃
}

TEST_CASE("OutputStateCodec:默认 group_id=1 且 save/load 往返 + 越界拒载", "[output][state]")
{
    scvb::state::OutputState s;
    REQUIRE(s.groupId == 1); // [J66] 默认 1
    REQUIRE(s.outputEnabled == 1); // 输出开关默认 on

    s.groupId = 5;
    s.captureEnabled = 1;
    s.outputEnabled = 0;
    s.versionActive = 2;
    s.uiScale = 150;
    s.uiLanguage = "zh-CN";

    std::vector<std::uint8_t> buf;
    REQUIRE(scvb::state::encodeOutputState(s, buf));

    scvb::state::OutputState d;
    REQUIRE(scvb::state::decodeOutputState(buf.data(), buf.size(), d));
    REQUIRE(d.groupId == 5);
    REQUIRE(d.captureEnabled == 1);
    REQUIRE(d.outputEnabled == 0);
    REQUIRE(d.versionActive == 2);
    REQUIRE(d.uiScale == 150);
    REQUIRE(d.uiLanguage == "zh-CN");

    // groupId=9 越界 → 拒载(不可信字节)。
    buf[0] = 9;
    scvb::state::OutputState bad;
    REQUIRE_FALSE(scvb::state::decodeOutputState(buf.data(), buf.size(), bad));
}
