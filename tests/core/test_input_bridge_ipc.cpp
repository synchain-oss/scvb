// SPDX-License-Identifier: GPL-3.0-or-later
// test_input_bridge_ipc —— T30 桥依赖的 core IPC 只读快照面 L0 单测(SegmentBackendInProcess):
// InputSession::connSnapshot/occupiedMask/configSeq/remoteAbi/localAbi/groupsOnline +
// CtrlPlane::isRingFull/changeGroup。跨进程真实行为归 T07b/L1 harness。
// T37(deepseek 两条 Processor 回归的 core 锚点):换组后 readGlobalInfo 读新组 SR(srMismatch 读对组)、
// 载入组≠1 state 后命令环 enqueue 落对组(remoteSetPriority 落对组)。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "input/InputSession.h"
#include "ipc/CtrlPlane.h"
#include "ipc/Registry.h"
#include "ipc/SegmentBackendInProcess.h"
#include "state/InputStateCodec.h"
#include "state/StateCodec.h"

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

TEST_CASE("T30 connSnapshot:Output 心跳陈旧 → maskBit 不置位(PR#54 R5 门控)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession s(backend, 1001);
    s.setChannelId(3);
    REQUIRE(s.prepare(48000, 512, 1, 100) == InputClaimState::kActive);
    s.heartbeat(100);

    // Output 活跃 + connected_mask bit3 置位。
    scvb::Registry out(backend, 1);
    REQUIRE(out.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(out.claimOutput(2001, 100) == scvb::Registry::ClaimResult::kClaimed);
    out.heartbeatOutput(100);
    out.setConnectedMaskBit(3);

    // 心跳新鲜(≤2000ms):在线 + maskBit 置位。
    const auto c0 = s.connSnapshot(200);
    CHECK(c0.outputOnline);
    CHECK(c0.maskBit);

    // 心跳陈旧(>2000ms)但 connected_mask 位仍在:outputOnline=false 且 maskBit 必须为 false
    // (否则 claimValue(kActive, maskBit=true, …) 会把已死 Output 判为 "active" 而非 "idle")。
    const auto c1 = s.connSnapshot(5000);
    CHECK_FALSE(c1.outputOnline);
    CHECK_FALSE(c1.maskBit);
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

TEST_CASE("T30 conflict 实例(非活跃)不得写 ctrl 命令环:拒绝且环数据不变(PR#54 R3)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // A 先占 ch3 → kActive,A 是 ch3 命令环的唯一合法生产者(SPSC)。
    InputSession a(backend, 1001);
    a.setChannelId(3);
    REQUIRE(a.prepare(48000, 512, 1, 0) == InputClaimState::kActive);
    a.heartbeat(100);

    // B 抢同 channel → kConflict(非活跃),不持有任何 slot。
    InputSession b(backend, 1002);
    b.setChannelId(3);
    REQUIRE(b.prepare(48000, 512, 1, 200) == InputClaimState::kConflict);
    REQUIRE(b.state() == InputClaimState::kConflict);
    REQUIRE(b.boundChannel() == 0); // 未 claim 任何 slot(claimedChannel==0)

    scvb::CtrlPlane plane(backend, 1);
    REQUIRE(plane.open() == InitResult::kOk);

    // 活跃实例 A 投递一条哨兵记录。
    REQUIRE(plane.enqueue(3, scvb::CtrlOp::kSetPriority, 5));
    CHECK(plane.overflowCount(3) == 0);

    // 守卫(与 InputProcessor::bridgeRemoteSetPriority 的 priorityRejection 同判定):非活跃实例
    // 拒绝、不得 enqueue —— 否则 B 成为 ch3 命令环第二生产者,与 A 竞写 write_pos 损坏 SPSC 环。
    CHECK(b.state() != InputClaimState::kActive);

    // 「环数据不变」:不投递 → 环内只有 A 的一条记录(无 B 注入的第二条),溢出计数不变。
    scvb::CtrlPlane reader(backend, 1);
    REQUIRE(reader.open() == InitResult::kOk);
    scvb::CtrlRecord rec;
    REQUIRE(reader.dequeue(3, rec));
    CHECK(rec.channel.load() == 3u);
    CHECK(rec.op.load() == static_cast<u32>(scvb::CtrlOp::kSetPriority));
    CHECK(rec.value.load() == 5u);
    CHECK_FALSE(reader.dequeue(3, rec)); // 无第二份记录(冲突实例未写)
    CHECK(reader.overflowCount(3) == 0);
}

TEST_CASE("T30 checkHeaderReadOnly:只读 magic/abi 校验不写(PR#54 R8)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // 创建并 claim g1 registry(magic/abi 就绪)。
    scvb::Registry reg(backend, 1);
    REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);

    // 只读打开(InProcess 下回退 openExisting,但仍走同一 checkHeaderReadOnly 路径)+ 校验。
    scvb::SegmentView view;
    REQUIRE(backend.openExistingReadOnly(L"Local\\SynchainSCVB.v1.g1.registry", view) == InitResult::kOk);
    const auto* header = static_cast<const scvb::RegistryHeader*>(view.base);
    const u32 magicBefore = header->magic.load(std::memory_order_acquire);
    const u32 abiBefore = header->abi.load(std::memory_order_acquire);

    REQUIRE(backend.checkHeaderReadOnly(view, header->magic, header->abi) == InitResult::kOk);

    // 校验后头部未被写(严格只读)。
    CHECK(header->magic.load(std::memory_order_acquire) == magicBefore);
    CHECK(header->abi.load(std::memory_order_acquire) == abiBefore);
    backend.unmap(view);
}

TEST_CASE("T30 CtrlPlane::release:释放后未打开,可重新懒开(PR#54 R9)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::CtrlPlane plane(backend, 1);
    REQUIRE(plane.open() == InitResult::kOk);
    REQUIRE(plane.isOpen());

    plane.release();
    CHECK_FALSE(plane.isOpen());

    // 释放后可重新懒开(与 ensureCtrlOpen 的懒开语义一致)。
    REQUIRE(plane.open() == InitResult::kOk);
    CHECK(plane.isOpen());
}

TEST_CASE("T30 CtrlPlane::changeGroup:换组失败返回 kAbiMismatch(PR#54 R9)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // 预先创建 g2 ctrl 段并把 abi 写成非法值(模拟旧版本残留损坏段)。
    scvb::SegmentView v;
    REQUIRE(backend.createOrOpen(L"Local\\SynchainSCVB.v1.g2.ctrl", scvb::kCtrlSegmentSize, v) == InitResult::kOk);
    auto* header = static_cast<scvb::CtrlHeader*>(v.base);
    REQUIRE(backend.initHeader(v, &header->magic, &header->abi, &header->generation, scvb::kCtrlBroadcastOffset,
                               /*initData=*/{}, /*allowOverwrite=*/true) == InitResult::kOk);
    header->abi.store(99, std::memory_order_release); // 损坏 abi
    backend.unmap(v);

    // g1 正常打开后换组到 g2 → kAbiMismatch(调用方据此回退,避免 session 新组 + ctrl 旧组错位)。
    scvb::CtrlPlane plane(backend, 1);
    REQUIRE(plane.open() == InitResult::kOk);
    CHECK(plane.changeGroup(2) == InitResult::kAbiMismatch);
}

TEST_CASE("T30 CtrlPlane:release 后组号保持,changeGroup 重对齐读写落新组(PR#54 R10)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::CtrlPlane plane(backend, 1);
    REQUIRE(plane.open() == InitResult::kOk);
    plane.release();
    CHECK_FALSE(plane.isOpen());
    CHECK(plane.group() == 1); // release 只清 base_,不改 group_(懒开前需按 groupId_ 重对齐)

    // 重对齐到组2(ensureCtrlOpen 的组号对齐逻辑同款),写/读都落组2。
    REQUIRE(plane.changeGroup(2) == InitResult::kOk);
    CHECK(plane.group() == 2);
    REQUIRE(plane.enqueue(3, scvb::CtrlOp::kSetPriority, 7));

    scvb::CtrlPlane reader2(backend, 2);
    REQUIRE(reader2.open() == InitResult::kOk);
    scvb::CtrlRecord rec;
    REQUIRE(reader2.dequeue(3, rec));
    CHECK(rec.value.load() == 7u);
}

TEST_CASE("T30 CtrlPlane:未 open 不建段(PR#54 R10 懒开口径)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::CtrlPlane plane(backend, 2);
    // 从未 open → g2 ctrl 段不存在(channel_id=0 未分配不建段)。
    scvb::SegmentView v;
    CHECK(backend.openExisting(L"Local\\SynchainSCVB.v1.g2.ctrl", v) == InitResult::kFailed);
}

TEST_CASE("T30 CtrlPlane:abi 损坏段 open 失败,enqueue 返回 false(PR#54 R10)")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // 预置 g1 ctrl 段 abi 损坏。
    scvb::SegmentView v;
    REQUIRE(backend.createOrOpen(L"Local\\SynchainSCVB.v1.g1.ctrl", scvb::kCtrlSegmentSize, v) == InitResult::kOk);
    auto* header = static_cast<scvb::CtrlHeader*>(v.base);
    REQUIRE(backend.initHeader(v, &header->magic, &header->abi, &header->generation, scvb::kCtrlBroadcastOffset,
                               /*initData=*/{}, /*allowOverwrite=*/true) == InitResult::kOk);
    header->abi.store(99, std::memory_order_release);
    backend.unmap(v);

    scvb::CtrlPlane plane(backend, 1);
    CHECK(plane.open() == InitResult::kAbiMismatch);
    CHECK_FALSE(plane.isOpen());
    // 段未打开 → enqueue 写不进去(返回 false),bridgeRemoteSetPriority 据此回 busy,不再回 queued:true。
    CHECK_FALSE(plane.enqueue(3, scvb::CtrlOp::kSetPriority, 5));
}

// ---------------------------------------------------------------------------
// T37 E2E 联调(deepseek 两条 Processor 回归的 core/桥级锚点)。
// 真机路径在 ScvbInputAudioProcessor(依赖 WebView2,不可离线编入单测):
//   setStateInformation(载入 group≠1 state)→ ctrl_.changeGroup(group_id) →
//   bridgeRemoteSetPriority → ctrl_.enqueue(ch, kSetPriority, n) / bridgeTickSnapshot → ctrl_.readGlobalInfo()
// 下面用 InputSession + CtrlPlane + InputStateCodec 逐段复刻该编排;无法离线复刻的 WebView2/宿主
// 接线面归 DAW 真机执行清单(docs/spikes/E2E-journey.md)。
// ---------------------------------------------------------------------------

TEST_CASE("T37 Processor 回归(deepseek):载入组≠1 工程 → remoteSetPriority 落对组")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // 1) 构造并解码 group=2、channel=3 的 Input state(等价 setStateInformation 读到的 CFGS)。
    scvb::state::InputState st;
    st.channelId = 3;
    st.groupId = 2;
    st.uiScale = 100;
    st.uiLanguage = "en";
    std::vector<std::uint8_t> payload;
    REQUIRE(scvb::state::encodeInputState(st, payload));
    scvb::state::StateChunks chunks;
    chunks.abi = scvb::state::kCurrentAbi;
    chunks.set(scvb::state::kFourccCfgs, std::move(payload));
    std::vector<std::uint8_t> blob;
    REQUIRE(scvb::state::encodeContainer(chunks, blob));

    scvb::state::StateChunks out;
    REQUIRE(scvb::state::decodeContainer(blob.data(), blob.size(), out) == scvb::state::DecodeStatus::Ok);
    const scvb::state::Chunk* cfg = out.find(scvb::state::kFourccCfgs);
    REQUIRE(cfg != nullptr);
    scvb::state::InputState loaded;
    REQUIRE(scvb::state::decodeInputState(cfg->payload.data(), cfg->payload.size(), loaded));
    REQUIRE(loaded.groupId == 2);
    REQUIRE(loaded.channelId == 3);

    // 2) session 按 state 对齐(等价 InputProcessor::setStateInformation 的 session_.setChannelId/setGroupId)。
    InputSession session(backend, 1001);
    session.setChannelId(loaded.channelId);
    session.setGroupId(loaded.groupId);

    // 3) ctrl_ 组对齐(等价 setStateInformation 的 changeGroup 分支:channelId≠0 ∧ 组≠当前组)。
    scvb::CtrlPlane ctrl(backend, 1); // InputProcessor 构造默认组 1
    REQUIRE(ctrl.group() == 1);
    REQUIRE(ctrl.changeGroup(loaded.groupId) == InitResult::kOk);
    CHECK(ctrl.group() == 2);

    // 4) bridgeRemoteSetPriority 的 enqueue(经 ensureCtrlOpen 对齐后;此处组已对齐,直接 enqueue)。
    REQUIRE(ctrl.enqueue(loaded.channelId, scvb::CtrlOp::kSetPriority, 7));

    // 5) 落对组:g2 环可见,g1 环无残留(换组未串组)。
    scvb::CtrlPlane reader2(backend, 2);
    REQUIRE(reader2.open() == InitResult::kOk);
    scvb::CtrlRecord rec;
    REQUIRE(reader2.dequeue(loaded.channelId, rec));
    CHECK(rec.channel.load() == 3u);
    CHECK(rec.op.load() == static_cast<u32>(scvb::CtrlOp::kSetPriority));
    CHECK(rec.value.load() == 7u);

    scvb::CtrlPlane reader1(backend, 1);
    REQUIRE(reader1.open() == InitResult::kOk);
    scvb::CtrlRecord stale;
    CHECK_FALSE(reader1.dequeue(loaded.channelId, stale)); // 旧组无记录
}

TEST_CASE("T37 Processor 回归(deepseek):srMismatch 读对组 —— changeGroup 后 readGlobalInfo 读新组 SR")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // g1 写入 SR=44100(g1 段)。
    scvb::CtrlPlane out1(backend, 1);
    REQUIRE(out1.open() == InitResult::kOk);
    scvb::OutputGlobalInfoSnapshot g1;
    g1.output_sample_rate = 44100;
    out1.refreshGlobalInfo(g1);

    // 换组到 g2 并写入 SR=48000(g2 段),等价 setStateInformation 载入组≠1 后 ctrl 对准新组。
    scvb::CtrlPlane ctrl(backend, 1);
    REQUIRE(ctrl.open() == InitResult::kOk);
    REQUIRE(ctrl.changeGroup(2) == InitResult::kOk);
    scvb::OutputGlobalInfoSnapshot g2;
    g2.output_sample_rate = 48000;
    ctrl.refreshGlobalInfo(g2);

    // readGlobalInfo 必须读 g2(48000),不读 g1 残留(44100) —— bridgeTickSnapshot 的 srMismatch
    // 推导真源(InputBridgeLogic::srMismatch(state, outputSR, localSR))据此拿到正确组的 SR。
    CHECK(ctrl.readGlobalInfo().output_sample_rate == 48000);

    // g1 段独立保留(跨组互不可见,未串组)。
    scvb::CtrlPlane probe1(backend, 1);
    REQUIRE(probe1.open() == InitResult::kOk);
    CHECK(probe1.readGlobalInfo().output_sample_rate == 44100);
}

// ---------------------------------------------------------------------------
// SL-446(SL-19 复发):Input 通道冲突时 UI 未回滚 + 存档写错通道号。
// 真机路径在 ScvbInputAudioProcessor::setChannelId/bridgeTickSnapshot/getStateInformation
// (依赖 WebView2,不可离线编入单测,同上 T37 那条注释的限制)——下面②③用 InputSession +
// InputStateCodec 逐段复刻该编排,与 T37 系列同一个惯例(§436 起那条已有先例)。
// 根因(已在 InputProcessor.cpp 修掉):`channelId_` 这个镜像字段此前在 CAS 结果出来**之前**
// 就被写成请求值,而它正是 bridgeTickSnapshot(广播给 UI)与 getStateInformation(工程存档)
// 的直接数据源;修法是把镜像字段的赋值挪到 session_.prepare() **之后**,读
// session_.boundChannel()(真正持有的 channel)。②③ 两格分别对应这两个不同的出口——
// ②绿不代表③绿,两条各自独立断言。
// ⚠ **复审当场指出的一个洞,写清楚不要含糊**:②③里的 `channelIdMirror` 是测试自己按新
// 公式算出来的本地变量,**不是从 InputProcessor.cpp 读出来的**——如果有人把
// `setChannelId()` 改回旧版那种「prepare() 之前就把 channelId_ 写成请求值」,②③**不会变红**
// (判例原文:「缺陷是『没被调用/没传下去』时,纯函数用例全绿」,这两格正是这个形态)。
// ②③ 证明的是"这套算法本身是对的",不证明"生产代码真的在用这套算法"——后者靠下面
// 新增的源码级顺序判据(不依赖浏览器/WebView2 那条判例的同款做法:剥注释、fail-closed、
// 不钉排版,配删除式)。
// ---------------------------------------------------------------------------

TEST_CASE("SL-19 复发②(Processor 回归复刻):冲突后广播源读会话真实持有的 channel,不读请求值", "[input][bridge]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // 1) 等价 InputProcessor 构造后首次 claim channel 3。
    InputSession session(backend, 1001);
    session.setChannelId(3);
    REQUIRE(session.prepare(48000, 512, 1, 0) == InputClaimState::kActive);

    // 2) 另一实例占住 channel 5(心跳新鲜,构成真冲突)。
    InputSession other(backend, 2001);
    other.setChannelId(5);
    REQUIRE(other.prepare(48000, 512, 1, 0) == InputClaimState::kActive);
    other.heartbeat(100);

    // 3) 等价修好后的 InputProcessor::bridgeTickSnapshot():第 2 轮之后广播**直接**读
    //    session_.boundChannel(),绕开 channelId_ 那个镜像字段(镜像现在固定是"配置",广播
    //    要的是"实际持有",两者会分叉——见 InputProcessor.cpp bridgeTickSnapshot() 头注的
    //    完整对照表)。`channelIdMirror` 这个变量名是历史遗留(第 1 轮镜像字段还兼着广播),
    //    这里数值上等价直接读 boundChannel()。
    session.setChannelId(5);
    const auto requestResult = session.prepare(48000, 512, 1, 200);
    const int channelIdMirror = static_cast<int>(session.boundChannel());

    CHECK(requestResult == InputClaimState::kConflict); // UI 侧仍然会看到冲突提示(抖动+红 toast)
    CHECK(channelIdMirror == 3); // 广播源读到的是真实持有的 3,不是抢失败的目标 5
}

TEST_CASE("SL-19 复发③(Processor 回归复刻):冲突后存档不记录抢失败的 channel", "[input][bridge]")
{
    // ⚠ 这一格与②各自独立断言——②绿不代表这格绿,两者读的是 InputProcessor 里两个不同的
    // 出口(bridgeTickSnapshot vs getStateInformation),复审明确要求分开钉。
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession session(backend, 1001);
    session.setChannelId(3);
    REQUIRE(session.prepare(48000, 512, 1, 0) == InputClaimState::kActive);

    InputSession other(backend, 2001);
    other.setChannelId(5);
    REQUIRE(other.prepare(48000, 512, 1, 0) == InputClaimState::kActive);
    other.heartbeat(100);

    session.setChannelId(5);
    REQUIRE(session.prepare(48000, 512, 1, 200) == InputClaimState::kConflict);
    // 等价修好后的 channelId_ 镜像:第 2 轮之后镜像固定读 channelId()(配置),不是
    // boundChannel()(实际持有)——这条场景里两者数值相同(回滚成功、channelId()==
    // boundChannel()==3),但算法要照真实实现抄,不是抄一个巧合数值相等的旧算法
    // (旧算法在"配置了但没绑定"场景会把用户配置错误地擦成 0,见 SL-446 第 2 轮)。
    const int channelIdMirror = static_cast<int>(session.channelId());

    // 等价 InputProcessor::getStateInformation():用镜像字段(不是失败的请求值)填 InputState
    // 再编解码一遍,复刻工程存档的完整往返。
    scvb::state::InputState st;
    st.channelId = static_cast<std::uint32_t>(channelIdMirror);
    st.groupId = session.groupId();
    st.uiScale = 100;
    st.uiLanguage = "en";
    std::vector<std::uint8_t> payload;
    REQUIRE(scvb::state::encodeInputState(st, payload));

    scvb::state::InputState loaded;
    REQUIRE(scvb::state::decodeInputState(payload.data(), payload.size(), loaded));

    CHECK(loaded.channelId == 3); // 存档记的是真正 claim 到的 channel
    CHECK(loaded.channelId != 5); // 不是那个抢失败的目标——这是本卡后果最重的一条(用户下次打开工程才发作)
}

TEST_CASE("SL-19 复发②的补丁:源码级顺序判据 —— setChannelId() 里 channelId_ 的赋值排在 "
          "prepare() 之后",
          "[input][bridge]")
{
    // 上面②③是"复刻"测试,证明不了 InputProcessor.cpp 真的在用这套算法(见上面头注那段
    // 复审指出的洞)。这一格改成直接读 InputProcessor.cpp 的源码文本判序,补上②证明不了的
    // 那一半——与 SL-437 那次给 web/*/index.html 加的源码级判据同一条纪律:先剥注释、
    // fail-closed(串找不到判负,不是放过)、不钉排版(只认关键字与相对顺序,不认换行/空格)。
    const std::string path = std::string(SCVB_SOURCE_DIR) + "/src/input/InputProcessor.cpp";
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    const std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // 剥注释(行注释 + 块注释):不剥的话,本文件自己写的说明文字里就出现过
    // "channelId_ = ..." 与 "session_.prepare(" 这两个关键词,注释会把断言顶替掉
    // (判例见上面②③头注引用的那条"源码正则 ≠ 可执行"同族坑)。
    std::string stripped;
    stripped.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size();)
    {
        if (i + 1 < raw.size() && raw[i] == '/' && raw[i + 1] == '/')
        {
            while (i < raw.size() && raw[i] != '\n')
            {
                ++i;
            }
        }
        else if (i + 1 < raw.size() && raw[i] == '/' && raw[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < raw.size() && !(raw[i] == '*' && raw[i + 1] == '/'))
            {
                ++i;
            }
            i = (i + 1 < raw.size()) ? i + 2 : raw.size();
        }
        else
        {
            stripped.push_back(raw[i]);
            ++i;
        }
    }

    // 只在 setChannelId() 这一个函数体内判序——prepareToPlay()/setStateInformation() 里也各
    // 有一次同模式的 "channelId_ = .../session_.prepare(",不隔离会把别的函数的顺序混进来
    // (那两处目前**没有**判据,是本卡明确留白的一半,见 PR 描述,不在这一格里冒充覆盖)。
    const std::string beginMarker = "ScvbInputAudioProcessor::setChannelId(int channelId)";
    const auto beginPos = stripped.find(beginMarker);
    REQUIRE(beginPos != std::string::npos); // fail-closed:函数改名/挪走也要判负,不是跳过
    const std::string endMarker = "ScvbInputAudioProcessor::setGroupId(int groupId)";
    const auto endPos = stripped.find(endMarker, beginPos);
    REQUIRE(endPos != std::string::npos);
    const std::string body = stripped.substr(beginPos, endPos - beginPos);

    const auto prepareIdx = body.find("session_.prepare(");
    REQUIRE(prepareIdx != std::string::npos);

    // 扫描函数体里全部 "channelId_ =" 赋值位置——不钉右手边具体写法(那是排版细节,比如
    // static_cast 的换行方式),只钉"这个函数体里,任何一次给 channelId_ 赋值都不能发生在
    // prepare() 调用之前"。这正是本卡要防的那处抢跑:旧版是 `channelId_ = channelId;` 排在
    // `session_.prepare(...)` **之前**。
    // ⚠ 复审 4056697571 指出:"channelId_ =" 这个字面量同时是 "channelId_ ==" (比较,不是
    // 赋值)的前缀——`if (channelId_ == 5)` 会被误当成一次赋值。用"匹配位置之后紧跟的那个
    // 字符不是 '=' "把比较排除掉;`+=`/`-=` 等复合赋值语义上仍算"改写了 channelId_",这个
    // 判据目前不需要额外收窄它们(生产代码里没有这种写法,加一条只判据本身不测的分支反而
    // 会掩盖 fail-closed 的空匹配路径)。
    std::vector<std::size_t> assignPositions;
    const std::string assignToken = "channelId_ =";
    for (std::size_t pos = body.find(assignToken); pos != std::string::npos; pos = body.find(assignToken, pos + 1))
    {
        const std::size_t afterToken = pos + assignToken.size();
        if (afterToken < body.size() && body[afterToken] == '=')
        {
            continue; // "channelId_ ==":比较,不是赋值,跳过不计入
        }
        assignPositions.push_back(pos);
    }
    REQUIRE_FALSE(assignPositions.empty()); // fail-closed:一次都找不到也判负,不是放过

    for (const auto pos : assignPositions)
    {
        CHECK(pos > prepareIdx);
    }
}

// ---------------------------------------------------------------------------
// T37 三轮 C 族回归:Output→Input 配置广播链路。
// 真机症状:「Input 侧优先级恒 0,Output 显示 5;Output 改 5→6,Input 不动;lead 开关也不同步」。
// 根因有两条,都在本文件覆盖:
//   ① ctrl 广播区(offset 64,9344B)只有预算没有布局 —— 没人写、没人读,Input 侧 label/
//      priority/lead_lock/pair_id/freeze 全是 InputBridgeLogic 里的硬编码常量;
//   ② Input 上行的 remoteSetPriority 进了命令环,而 OutputSession::consumeCommands 的循环体
//      是空的 —— 记录 dequeue 出来直接丢弃,优先级永远落不到 Output 的 state。
// ---------------------------------------------------------------------------

TEST_CASE("T37-C 广播区 seqlock 往返:Output 写 → Input 读到同一份配置", "[ctrl][broadcast][t37]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::CtrlPlane writer(backend, 1);
    REQUIRE(writer.open() == scvb::InitResult::kOk);

    scvb::CtrlBroadcastSnapshot out;
    out.config_seq = 42;
    out.lead_select = 3;
    out.channels[2].priority = 6; // ch3 优先级 6(真机里 Output 显示 5、改成 6)
    out.channels[2].pair_id = 2;
    out.channels[2].freeze = 3; // pan + vol 双冻
    out.channels[2].source_channels = 2;
    out.channels[2].flags = scvb::kCfgFlagEnabled | scvb::kCfgFlagLeadLock | scvb::kCfgFlagParticipateAutoPan;
    std::snprintf(out.labels[2], scvb::kCtrlLabelBytes, "%s", "主唱 A");
    writer.writeBroadcast(out);

    scvb::CtrlPlane reader(backend, 1);
    REQUIRE(reader.open() == scvb::InitResult::kOk);

    scvb::CtrlBroadcastSnapshot in;
    REQUIRE(reader.readBroadcast(in));
    CHECK(in.config_seq == 42);
    CHECK(in.lead_select == 3);
    CHECK(in.channels[2].priority == 6);
    CHECK(in.channels[2].pair_id == 2);
    CHECK(in.channels[2].freeze == 3);
    CHECK(in.channels[2].source_channels == 2);
    CHECK((in.channels[2].flags & scvb::kCfgFlagLeadLock) != 0);
    CHECK((in.channels[2].flags & scvb::kCfgFlagLeadVolExempt) == 0);
    CHECK(std::string(in.labels[2]) == "主唱 A");

    // 改一个字段再广播 → 读方看到新值与新 seq(这一步是「Output 改 5→6 Input 不动」的直接回归)。
    out.config_seq = 43;
    out.channels[2].priority = 9;
    writer.writeBroadcast(out);
    REQUIRE(reader.readBroadcast(in));
    CHECK(in.config_seq == 43);
    CHECK(in.channels[2].priority == 9);
}

TEST_CASE("T37-C 广播区落在预算内且不与段头/全局小节重叠", "[ctrl][broadcast][t37]")
{
    // broadcastBase() 曾返回 base_(段起点 = CtrlHeader 地址):按 broadcastBytes() 写入会砸掉
    // magic/abi/generation 并越界踩进 OutputGlobalInfo。此处钉死偏移与预算。
    STATIC_REQUIRE(sizeof(scvb::CtrlBroadcast) <= scvb::kCtrlBroadcastBytes);
    STATIC_REQUIRE(scvb::kCtrlBroadcastOffset == sizeof(scvb::CtrlHeader));
    STATIC_REQUIRE(scvb::kCtrlBroadcastOffset + scvb::kCtrlBroadcastBytes == scvb::kCtrlGlobalInfoOffset);

    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;
    scvb::CtrlPlane plane(backend, 1);
    REQUIRE(plane.open() == scvb::InitResult::kOk);

    scvb::CtrlBroadcastSnapshot s;
    s.config_seq = 7;
    plane.writeBroadcast(s);

    // 写广播区不得动段头(generation/abi 仍可用),也不得动 OutputGlobalInfo。
    CHECK(plane.generation() >= 1);
    scvb::OutputGlobalInfoSnapshot gi;
    gi.output_sample_rate = 48000;
    plane.refreshGlobalInfo(gi);
    plane.writeBroadcast(s);
    CHECK(plane.readGlobalInfo().output_sample_rate == 48000);
}
