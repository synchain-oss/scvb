// SPDX-License-Identifier: GPL-3.0-or-later
// test_input_session —— InputSession 生命周期单测([J01] 十五 Input 零 CAS / I6 无痕迹 /
// [J66] 改组两条 / 健康判定)。用 SegmentBackendInProcess 模拟多实例(跨进程真实行为归 T07b)。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#include "input/InputSession.h"
#include "ipc/Registry.h"
#include "ipc/SegmentBackendInProcess.h"

using scvb::InitResult;
using scvb::kSlotActive;
using scvb::kSlotFree;
using scvb::u32;
using scvb::input::InputClaimState;
using scvb::input::InputSession;

TEST_CASE("十五个 Input 同时插入(全部默认 channel_id=0)零 CAS 冲突", "[input][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    std::vector<std::unique_ptr<InputSession>> sessions;
    for (int i = 0; i < 15; ++i)
    {
        auto s = std::make_unique<InputSession>(backend, static_cast<u32>(1000 + i));
        REQUIRE(s->channelId() == 0); // 首次插入默认 0
        REQUIRE(s->prepare(48000, 512, 1, 0) == InputClaimState::kUnassigned); // 不 claim
        sessions.push_back(std::move(s));
    }

    // registry 15 个 InputSlot 全部空闲:零 CAS、零冲突(这正是 J01 要消除的连环冲突场景)。
    scvb::Registry probe(backend, 1);
    REQUIRE(probe.open() == scvb::Registry::ClaimResult::kClaimed);
    for (u32 ch = 1; ch <= 15; ++ch)
    {
        REQUIRE(probe.inputSlot(ch)->state.load() == kSlotFree);
    }
}

TEST_CASE("channel_id=0 时段与心跳均未创建(registry 无痕迹)", "[input][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession s(backend, 1001);
    REQUIRE(s.prepare(48000, 512, 1, 0) == InputClaimState::kUnassigned);
    s.heartbeat(100); // 即便调心跳,kUnassigned 无 slot 可写。

    scvb::Registry probe(backend, 1);
    REQUIRE(probe.open() == scvb::Registry::ClaimResult::kClaimed);
    for (u32 ch = 1; ch <= 15; ++ch)
    {
        REQUIRE(probe.inputSlot(ch)->state.load() == kSlotFree);
        REQUIRE(probe.inputSlot(ch)->heartbeat_ms.load() == 0); // 无心跳痕迹
    }

    // audio.ch1 / feat.ch1 段未创建。
    scvb::SegmentView v;
    REQUIRE(backend.openExisting(L"Local\\SynchainSCVB.v1.g1.audio.ch1", v) == InitResult::kFailed);
    REQUIRE(backend.openExisting(L"Local\\SynchainSCVB.v1.g1.feat.ch1", v) == InitResult::kFailed);
}

TEST_CASE("channel_id>=1 claim 成功且创建 audio/feat 段", "[input][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession s(backend, 1001);
    s.setChannelId(3);
    REQUIRE(s.prepare(48000, 512, 2, 0) == InputClaimState::kActive);
    REQUIRE(s.boundChannel() == 3);
    REQUIRE(s.audioRing().bound());
    REQUIRE(s.audioRing().geometry().channels == 2);
    REQUIRE(s.featRing().bound());

    scvb::Registry probe(backend, 1);
    REQUIRE(probe.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(probe.inputSlot(3)->state.load() == kSlotActive);
    REQUIRE(probe.inputSlot(3)->pid == 1001);

    // audio.ch3 / feat.ch3 段已创建。
    scvb::SegmentView v;
    REQUIRE(backend.openExisting(L"Local\\SynchainSCVB.v1.g1.audio.ch3", v) == InitResult::kOk);
    REQUIRE(backend.openExisting(L"Local\\SynchainSCVB.v1.g1.feat.ch3", v) == InitResult::kOk);
}

TEST_CASE("J66② 改组:释放旧组 slot → 新组 claim 成功", "[input][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession s(backend, 1001);
    s.setChannelId(3);
    REQUIRE(s.prepare(48000, 512, 1, 0) == InputClaimState::kActive);

    scvb::Registry probe1(backend, 1);
    REQUIRE(probe1.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(probe1.inputSlot(3)->state.load() == kSlotActive); // 旧组 g1 持有

    // 改组到 g2。
    REQUIRE(s.changeGroup(2, 48000, 512, 1, 100) == InputClaimState::kActive);
    REQUIRE(s.groupId() == 2);
    REQUIRE(s.boundChannel() == 3);

    // 旧组 g1 slot 3 归零。
    REQUIRE(probe1.inputSlot(3)->state.load() == kSlotFree);
    // 新组 g2 slot 3 活跃。
    scvb::Registry probe2(backend, 2);
    REQUIRE(probe2.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(probe2.inputSlot(3)->state.load() == kSlotActive);
}

TEST_CASE("J66② 改组后新组同 channel 被占 → I2 冲突态(不持有 slot)", "[input][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // g2 slot 3 被另一实例占用(心跳新鲜)。
    InputSession other(backend, 2001);
    other.setChannelId(3);
    other.setGroupId(2);
    REQUIRE(other.prepare(48000, 512, 1, 0) == InputClaimState::kActive);
    other.heartbeat(100);

    InputSession s(backend, 1001);
    s.setChannelId(3);
    s.setGroupId(1);
    REQUIRE(s.prepare(48000, 512, 1, 0) == InputClaimState::kActive); // g1 claim 成功

    // 改组到 g2:同 channel 被占 → I2 冲突,本实例不持有任何 slot。
    REQUIRE(s.changeGroup(2, 48000, 512, 1, 200) == InputClaimState::kConflict);
    REQUIRE(s.state() == InputClaimState::kConflict);
    REQUIRE(s.boundChannel() == 0);
    REQUIRE_FALSE(s.audioRing().bound());
}

TEST_CASE("健康判定:无 Output → false;Output 活跃 + mask → true", "[input][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession in(backend, 1001);
    in.setChannelId(3);
    REQUIRE(in.prepare(48000, 512, 1, 0) == InputClaimState::kActive);

    // 无 Output → 不健康(直通)。
    REQUIRE_FALSE(in.isHealthy(0));

    // Output 加载 → 活跃 + 心跳新鲜 + mask 位 → 健康(静音)。
    scvb::Registry out(backend, 1);
    REQUIRE(out.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(out.claimOutput(2001, 1000) == scvb::Registry::ClaimResult::kClaimed);
    out.setConnectedMaskBit(3);
    REQUIRE(in.isHealthy(1100));

    // mask 位清除 → 不健康。
    out.clearConnectedMaskBit(3);
    REQUIRE_FALSE(in.isHealthy(1200));
}

TEST_CASE("channel_id=0 后 prepare 不残留旧 claim", "[input][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession s(backend, 1001);
    s.setChannelId(3);
    REQUIRE(s.prepare(48000, 512, 1, 0) == InputClaimState::kActive);

    // 用户把 channel_id 改回 0 → 释放 slot + 段,I6。
    s.setChannelId(0);
    REQUIRE(s.prepare(48000, 512, 1, 100) == InputClaimState::kUnassigned);
    REQUIRE(s.boundChannel() == 0);

    scvb::Registry probe(backend, 1);
    REQUIRE(probe.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(probe.inputSlot(3)->state.load() == kSlotFree);
}
