// SPDX-License-Identifier: GPL-3.0-or-later
// test_input_bridge_ipc —— T30 桥依赖的 core IPC 只读快照面 L0 单测(SegmentBackendInProcess):
// InputSession::connSnapshot/occupiedMask/configSeq/remoteAbi/localAbi/groupsOnline +
// CtrlPlane::isRingFull/changeGroup。跨进程真实行为归 T07b/L1 harness。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "input/InputSession.h"
#include "ipc/CtrlPlane.h"
#include "ipc/Registry.h"
#include "ipc/SegmentBackendInProcess.h"

using scvb::InitResult;
using scvb::kSlotActive;
using scvb::kSlotClaimed;
using scvb::u32;
using scvb::input::InputClaimState;
using scvb::input::InputSession;

TEST_CASE("T30 connSnapshot:无 Output → 默认;Output 活跃 + mask → 在线/位/占用(§4.2)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession s(backend, 1001);
    s.setChannelId(3);
    REQUIRE(s.prepare(48000, 512, 1, 100) == InputClaimState::kActive);
    s.heartbeat(500);

    // 无 Output:outputOnline/maskBit false;occupiedMask 含本实例(ch3 心跳新鲜)。
    const auto c0 = s.connSnapshot(600);
    CHECK_FALSE(c0.outputOnline);
    CHECK_FALSE(c0.maskBit);
    CHECK_FALSE(c0.capturing);
    CHECK((c0.occupiedMask & (1u << 2)) != 0);

    // Output 活跃 + connected_mask bit3 → 在线 + maskBit。
    scvb::Registry out(backend, 1);
    REQUIRE(out.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(out.claimOutput(2001, 600) == scvb::Registry::ClaimResult::kClaimed);
    out.heartbeatOutput(600);
    out.setConnectedMaskBit(3);

    const auto c1 = s.connSnapshot(700);
    CHECK(c1.outputOnline);
    CHECK(c1.maskBit);
}

TEST_CASE("T30 occupiedMask:心跳陈旧/幽灵槽不置位(§4.2 陈旧可覆盖语义)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession s(backend, 1001);
    s.setChannelId(3);
    REQUIRE(s.prepare(48000, 512, 1, 100) == InputClaimState::kActive);
    s.heartbeat(500);

    // 心跳过期(>2000ms)→ 本实例自己的位也不置位(陈旧可覆盖)。
    CHECK(s.connSnapshot(3000).occupiedMask == 0);

    // 幽灵槽(state=kSlotClaimed,pid=0,hb=0)→ 不置位。
    scvb::Registry probe(backend, 1);
    REQUIRE(probe.open() == scvb::Registry::ClaimResult::kClaimed);
    probe.inputSlot(1)->state.store(kSlotClaimed, std::memory_order_release);

    s.heartbeat(3100);
    const auto mask = s.connSnapshot(3200).occupiedMask;
    CHECK((mask & 1u) == 0); // ch1 幽灵不置位
    CHECK((mask & (1u << 2)) != 0); // ch3 本实例心跳新鲜
}

TEST_CASE("T30 configSeq:透传本组 OutputSlot.config_seq(§4.3 变化检测真源)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession s(backend, 1001);
    s.setChannelId(3);
    REQUIRE(s.prepare(48000, 512, 1, 100) == InputClaimState::kActive);
    CHECK(s.configSeq() == 0);

    scvb::Registry out(backend, 1);
    REQUIRE(out.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(out.claimOutput(2001, 100) == scvb::Registry::ClaimResult::kClaimed);
    CHECK(out.bumpConfigSeq() == 1);
    CHECK(s.configSeq() == 1);
}

TEST_CASE("T30 remoteAbi:registry abi 不符 → kAbiMismatch 并记录对端 abi(J40 拒连)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // 先正常建 g1 registry(abi=1),再伪装对端 abi=2。
    scvb::Registry probe(backend, 1);
    REQUIRE(probe.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(probe.abi() == scvb::kScvbAbi);
    probe.header()->abi.store(2, std::memory_order_release);

    InputSession s(backend, 1001);
    s.setChannelId(3);
    REQUIRE(s.prepare(48000, 512, 1, 100) == InputClaimState::kAbiMismatch);
    CHECK(s.localAbi() == scvb::kScvbAbi);
    CHECK(s.remoteAbi() == 2); // scvb.state.abi_remote 来源(§4.1)
}

TEST_CASE("T30 groupsOnline:本组位 + 跨组只读探测(01 §4.5/J70)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession s(backend, 1001);
    s.setChannelId(3);
    REQUIRE(s.prepare(48000, 512, 1, 100) == InputClaimState::kActive);

    // g2 有活跃 Output(g1 无)→ 位图 = bit1。
    scvb::Registry out2(backend, 2);
    REQUIRE(out2.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(out2.claimOutput(2001, 100) == scvb::Registry::ClaimResult::kClaimed);
    out2.heartbeatOutput(100);
    CHECK(s.groupsOnline(150) == 0b00000010);

    // g1 本组 Output 上线 → 位图 = bit0|bit1。
    scvb::Registry out1(backend, 1);
    REQUIRE(out1.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(out1.claimOutput(2002, 150) == scvb::Registry::ClaimResult::kClaimed);
    out1.heartbeatOutput(150);
    CHECK(s.groupsOnline(200) == 0b00000011);

    // 心跳全部陈旧(>2000ms)→ 位图全灭(探测失败/离线 = 可接受降级,不报错)。
    CHECK(s.groupsOnline(5000) == 0);
}

TEST_CASE("T30 groupsOnline:未分配(channel_id=0)实例本组位经只读探测点亮(PR#54 R2)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // 未分配实例:不 prepare、不 claim,本组 registry 未打开(openAndClaim 未走)。
    InputSession s(backend, 1001);
    CHECK(s.groupsOnline(100) == 0); // 无任何 Output → 全灭

    // g1 有活跃 Output → 本组位经只读探测点亮(修复前 outputSlot()==nullptr 恒 0)。
    scvb::Registry out(backend, 1);
    REQUIRE(out.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(out.claimOutput(2001, 100) == scvb::Registry::ClaimResult::kClaimed);
    out.heartbeatOutput(100);
    CHECK(s.groupsOnline(150) == 0b00000001);

    // 心跳陈旧(>2000ms)→ 熄灭。
    CHECK(s.groupsOnline(5000) == 0);
}

TEST_CASE("T30 CtrlPlane::isRingFull:满环判定与覆盖语义(IPC-13)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::CtrlPlane plane(backend, 1);
    REQUIRE(plane.open() == InitResult::kOk);

    for (u32 i = 0; i < 15; ++i)
    {
        REQUIRE(plane.enqueue(3, scvb::CtrlOp::kSetPriority, i));
        CHECK_FALSE(plane.isRingFull(3)); // 未满
    }
    REQUIRE(plane.enqueue(3, scvb::CtrlOp::kSetPriority, 15));
    CHECK(plane.isRingFull(3)); // 恰好填满(容量 16)

    // 满环继续写:丢最旧 + 溢出计数(enqueue 既有语义),满态保持。
    REQUIRE(plane.enqueue(3, scvb::CtrlOp::kSetPriority, 99));
    CHECK(plane.overflowCount(3) == 1);
    CHECK(plane.isRingFull(3));

    // 消费清空后不再满。
    scvb::CtrlRecord rec;
    while (plane.dequeue(3, rec))
    {
    }
    CHECK_FALSE(plane.isRingFull(3));

    // 非法 channel。
    CHECK_FALSE(plane.isRingFull(0));
    CHECK_FALSE(plane.isRingFull(16));
}

TEST_CASE("T30 CtrlPlane::changeGroup:换组即重开新组段(J66 per-组 ctrl)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::CtrlPlane plane(backend, 1);

    // 换组 → 立即映射 + §4.0 初始化新组 ctrl 段(与 Registry::changeGroup 同构;valid() 守卫防
    // moved-from 句柄,T24 #53 已合入语义)。
    REQUIRE(plane.changeGroup(2) == InitResult::kOk);
    CHECK(plane.group() == 2);
    CHECK(plane.isOpen());
    scvb::SegmentView v;
    CHECK(backend.openExisting(L"Local\\SynchainSCVB.v1.g2.ctrl", v) == InitResult::kOk);

    // 再次换组 → g3 段立即可用。
    REQUIRE(plane.changeGroup(3) == InitResult::kOk);
    CHECK(plane.group() == 3);
    CHECK(backend.openExisting(L"Local\\SynchainSCVB.v1.g3.ctrl", v) == InitResult::kOk);

    // 同组 changeGroup → kOk 且保持打开(重开同组段)。
    REQUIRE(plane.changeGroup(3) == InitResult::kOk);
    CHECK(plane.isOpen());

    // PR#54 复审【重要】1 支撑断言:换组后 enqueue 必须落到新组段(g3 环可见)、旧组段(g2)为空 ——
    // InputProcessor::setStateInformation 载入非默认组 state 后依赖此语义把命令环对准 state 的
    // group_id(remoteSetPriority 上行 / srMismatch 推导真源)。
    REQUIRE(plane.enqueue(5, scvb::CtrlOp::kSetPriority, 7));
    scvb::CtrlPlane reader3(backend, 3);
    REQUIRE(reader3.open() == InitResult::kOk);
    scvb::CtrlRecord rec;
    REQUIRE(reader3.dequeue(5, rec));
    CHECK(rec.channel.load() == 5u);
    CHECK(rec.op.load() == static_cast<u32>(scvb::CtrlOp::kSetPriority));
    CHECK(rec.value.load() == 7u);

    scvb::CtrlPlane reader2(backend, 2);
    REQUIRE(reader2.open() == InitResult::kOk);
    scvb::CtrlRecord stale;
    CHECK_FALSE(reader2.dequeue(5, stale)); // 旧组段无记录(换组未串组)

    // 非法组号。
    CHECK(plane.changeGroup(0) == InitResult::kFailed);
    CHECK(plane.changeGroup(9) == InitResult::kFailed);
}
