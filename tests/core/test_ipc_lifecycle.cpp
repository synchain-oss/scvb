// SPDX-License-Identifier: GPL-3.0-or-later
// test_ipc_lifecycle —— IPC 生命周期状态机单测(T07)。用 SegmentBackendInProcess 模拟多实例
// (快速回归;跨进程用例归 T07b)。覆盖 01 §4.3 a-h 八场景 + 接管四格 + 组隔离 + 命令环 + 看门狗。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

#include "ipc/CtrlPlane.h"
#include "ipc/Registry.h"
#include "ipc/SegmentBackendInProcess.h"

using scvb::kMaxChannels;
using scvb::kSlotActive;
using scvb::kSlotClaimed;
using scvb::kSlotFree;
using scvb::u32;
using scvb::u64;

namespace
{
constexpr u64 kT0 = 1000;
}

// ---------------------------------------------------------------------------
// ① 01 §4.3 八个时序场景
// ---------------------------------------------------------------------------

TEST_CASE("§4.3-a 冷启动 Output 先加载", "[ipc][lifecycle]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry out(backend, 1);
    REQUIRE(out.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(out.generation() == 1); // 创建者路径 generation=1
    REQUIRE(out.claimOutput(/*pid=*/2001, kT0) == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(out.outputSlot()->state.load() == kSlotActive);

    scvb::Registry in(backend, 1);
    REQUIRE(in.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(in.claimInput(/*ch=*/3, /*pid=*/1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(in.inputSlot(3)->state.load() == kSlotActive);

    // Output 判 CH_ONLINE 后置 mask 位,Input 侧可读(同组共享 OutputSlot)。
    out.setConnectedMaskBit(3);
    REQUIRE(out.connectedMask() == (1u << 2));
    REQUIRE(in.connectedMask() == (1u << 2));
}

TEST_CASE("§4.3-b 冷启动 Input 先加载(让位协议)", "[ipc][lifecycle]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry in(backend, 1);
    REQUIRE(in.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(in.claimInput(3, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
    // OutputSlot 空 → 健康判定不成立(Input 直通,状态灯「Output 离线」)。
    REQUIRE(in.outputSlot()->state.load() == kSlotFree);

    // Output 加载 → O2 → 判 CH_ONLINE → 置 mask 位。
    scvb::Registry out(backend, 1);
    REQUIRE(out.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(out.claimOutput(2001, kT0) == scvb::Registry::ClaimResult::kClaimed);
    out.setConnectedMaskBit(3);

    // Input 25Hz 轮询见 mask 位 → 健康判定成立。
    REQUIRE(in.connectedMask() == (1u << 2));
    REQUIRE((in.connectedMask() & (1u << 2)) != 0);
}

TEST_CASE("§4.3-c channel 冲突(组内)", "[ipc][lifecycle]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry in1(backend, 1);
    REQUIRE(in1.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(in1.claimInput(3, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
    in1.heartbeatInput(3, kT0 + 100); // 保持心跳新鲜

    scvb::Registry in2(backend, 1);
    REQUIRE(in2.open() == scvb::Registry::ClaimResult::kClaimed);
    // 后到者 CAS 失败且心跳新鲜 → kConflict。
    REQUIRE(in2.claimInput(3, 1002, 48000, 512, kT0 + 200) == scvb::Registry::ClaimResult::kConflict);
    // 绝不双写同环:仍只有 in1 持有。
    REQUIRE(in1.inputSlot(3)->pid == 1001);
    REQUIRE(in1.inputSlot(3)->state.load() == kSlotActive);
}

TEST_CASE("§4.3-d 心跳丢失(对端失联)", "[ipc][lifecycle]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry out(backend, 1);
    REQUIRE(out.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(out.claimOutput(2001, kT0) == scvb::Registry::ClaimResult::kClaimed);
    const u64 hb = out.outputSlot()->heartbeat_ms.load();

    // 2100ms 后陈旧(>2000ms 显示阈值),但未到 5000ms 接管阈值。
    REQUIRE(scvb::isStaleDisplay(hb, kT0 + 2100));
    REQUIRE_FALSE(scvb::isTakeoverStale(hb, kT0 + 2100));
}

TEST_CASE("§4.3-e DAW 崩溃残段(陈旧接管)", "[ipc][lifecycle]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry in1(backend, 1);
    REQUIRE(in1.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(in1.claimInput(3, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
    // 崩溃:无清理;心跳陈旧化由对端处理。

    const bool holderAlive = false; // pid 已死
    scvb::Registry in2(backend, 1, [&](u32) { return holderAlive; });
    REQUIRE(in2.open() == scvb::Registry::ClaimResult::kClaimed);
    // 5100ms 后:陈旧 + 死 pid → 覆盖式接管。
    REQUIRE(in2.claimInput(3, 1002, 48000, 512, kT0 + 5100) == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(in2.inputSlot(3)->pid == 1002);
}

TEST_CASE("§4.3-f 采样率不符", "[ipc][lifecycle]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry in(backend, 1);
    REQUIRE(in.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(in.claimInput(3, 1001, 44100, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
    const u32 outputSr = 48000;
    // Output 读到 slot.sample_rate != 自身 SR → CH_SR_MISMATCH(禁用不崩溃)。
    REQUIRE(in.inputSlot(3)->sample_rate == 44100);
    REQUIRE(in.inputSlot(3)->sample_rate != outputSr);
    // updateOwnedInputSlot:采样率切换重认领后一致。
    in.updateOwnedInputSlot(3, 1001, 48000, 512);
    REQUIRE(in.inputSlot(3)->sample_rate == 48000);
}

TEST_CASE("§4.3-g DAW 卸载单个插件", "[ipc][lifecycle]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry in(backend, 1);
    REQUIRE(in.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(in.claimInput(3, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
    // 析构路径释放 slot(state→0)。
    in.releaseInput(3, 1001);
    REQUIRE(in.inputSlot(3)->state.load() == kSlotFree);
}

TEST_CASE("§4.3-h 复制粘贴 Input(同 channel 冲突)", "[ipc][lifecycle]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // 复制实例携带同一 channel_id state → 解析到同号。
    const u32 ch = scvb::resolveInputChannelForClaim(/*env=*/0, /*state=*/5, false, 0);
    REQUIRE(ch == 5);

    scvb::Registry in1(backend, 1);
    REQUIRE(in1.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(in1.claimInput(ch, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);

    scvb::Registry in2(backend, 1);
    REQUIRE(in2.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(in2.claimInput(ch, 1002, 48000, 512, kT0 + 100) == scvb::Registry::ClaimResult::kConflict);
}

// ---------------------------------------------------------------------------
// ② 双实例抢同 channel 只有一个活跃
// ---------------------------------------------------------------------------

TEST_CASE("双实例抢同 channel 只有一个活跃", "[ipc][lifecycle][conflict]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry a(backend, 1);
    REQUIRE(a.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(a.claimInput(3, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
    a.heartbeatInput(3, kT0 + 100);

    scvb::Registry b(backend, 1);
    REQUIRE(b.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(b.claimInput(3, 1002, 48000, 512, kT0 + 200) == scvb::Registry::ClaimResult::kConflict);

    // 只有一个活跃:slot 仍属 a。
    REQUIRE(a.inputSlot(3)->state.load() == kSlotActive);
    REQUIRE(a.inputSlot(3)->pid == 1001);
}

// ---------------------------------------------------------------------------
// ③ 接管判定四格(J10 双条件)
// ---------------------------------------------------------------------------

TEST_CASE("接管判定四格(J10 双条件)", "[ipc][lifecycle][takeover]")
{
    SECTION("4900ms × pid 存活 → 不接管")
    {
        scvb::SegmentBackendInProcess::resetAll();
        scvb::SegmentBackendInProcess backend;
        scvb::Registry first(backend, 1);
        REQUIRE(first.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(first.claimInput(3, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
        const bool holderAlive = true;
        scvb::Registry second(backend, 1, [&](u32) { return holderAlive; });
        REQUIRE(second.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(second.claimInput(3, 1002, 48000, 512, kT0 + 4900) == scvb::Registry::ClaimResult::kConflict);
        REQUIRE(first.inputSlot(3)->pid == 1001);
    }

    SECTION("4900ms × pid 已死 → 不接管(未过 5000ms)")
    {
        scvb::SegmentBackendInProcess::resetAll();
        scvb::SegmentBackendInProcess backend;
        scvb::Registry first(backend, 1);
        REQUIRE(first.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(first.claimInput(3, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
        const bool holderAlive = false;
        scvb::Registry second(backend, 1, [&](u32) { return holderAlive; });
        REQUIRE(second.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(second.claimInput(3, 1002, 48000, 512, kT0 + 4900) == scvb::Registry::ClaimResult::kConflict);
        REQUIRE(first.inputSlot(3)->pid == 1001);
    }

    SECTION("5100ms × pid 存活 → 不接管(探活是第二必要条件)")
    {
        scvb::SegmentBackendInProcess::resetAll();
        scvb::SegmentBackendInProcess backend;
        scvb::Registry first(backend, 1);
        REQUIRE(first.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(first.claimInput(3, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
        const bool holderAlive = true;
        scvb::Registry second(backend, 1, [&](u32) { return holderAlive; });
        REQUIRE(second.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(second.claimInput(3, 1002, 48000, 512, kT0 + 5100) == scvb::Registry::ClaimResult::kConflict);
        REQUIRE(first.inputSlot(3)->pid == 1001);
    }

    SECTION("5100ms × pid 已死 → CAS 接管且唯一胜者")
    {
        scvb::SegmentBackendInProcess::resetAll();
        scvb::SegmentBackendInProcess backend;
        scvb::Registry first(backend, 1);
        REQUIRE(first.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(first.claimInput(3, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
        const bool holderAlive = false;
        scvb::Registry second(backend, 1, [&](u32) { return holderAlive; });
        scvb::Registry third(backend, 1, [&](u32) { return holderAlive; });
        REQUIRE(second.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(third.open() == scvb::Registry::ClaimResult::kClaimed);
        const auto r2 = second.claimInput(3, 1002, 48000, 512, kT0 + 5100);
        const auto r3 = third.claimInput(3, 1003, 48000, 512, kT0 + 5100);
        const bool r2won = (r2 == scvb::Registry::ClaimResult::kClaimed);
        const bool r3won = (r3 == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(r2won != r3won); // 恰好一个胜者(CAS 保证唯一)
        const bool atLeastOne = r2won || r3won;
        REQUIRE(atLeastOne);
    }
}

// ---------------------------------------------------------------------------
// ④ 2000ms 边界两格(仅 UI 陈旧,不触发接管)
// ---------------------------------------------------------------------------

TEST_CASE("2000ms 边界只影响 UI 陈旧显示", "[ipc][lifecycle][threshold]")
{
    const u64 hb = kT0;
    REQUIRE_FALSE(scvb::isStaleDisplay(hb, hb + 1900)); // 1900ms 未陈旧
    REQUIRE(scvb::isStaleDisplay(hb, hb + 2100)); // 2100ms 陈旧
    REQUIRE_FALSE(scvb::isTakeoverStale(hb, hb + 1900)); // 都不触发接管
    REQUIRE_FALSE(scvb::isTakeoverStale(hb, hb + 2100));

    // 2100ms × pid 已死 → 仍 kConflict(未过 5000ms 接管阈值)。
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;
    scvb::Registry first(backend, 1);
    REQUIRE(first.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(first.claimInput(3, 1001, 48000, 512, hb) == scvb::Registry::ClaimResult::kClaimed);
    const bool holderAlive = false;
    scvb::Registry second(backend, 1, [&](u32) { return holderAlive; });
    REQUIRE(second.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(second.claimInput(3, 1002, 48000, 512, hb + 2100) == scvb::Registry::ClaimResult::kConflict);
}

// ---------------------------------------------------------------------------
// ⑤ J40 abi 不符 → 不连接 + 横幅回调
// ---------------------------------------------------------------------------

TEST_CASE("J40 abi 不符 → 不连接 + 横幅回调", "[ipc][lifecycle][abi]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // 预置一个 magic 相符但 abi=99 的段(模拟旧/新版本不互认)。
    scvb::SegmentView view;
    REQUIRE(backend.createOrOpen(L"Local\\SynchainSCVB.v1.g1.registry", scvb::kRegistrySegmentSize, view) ==
            scvb::InitResult::kOk);
    auto* hdr = static_cast<scvb::RegistryHeader*>(view.base);
    hdr->magic.store(scvb::kScvbMagic, std::memory_order_release);
    hdr->abi.store(99, std::memory_order_release);

    bool called = false;
    u32 localAbi = 0;
    u32 remoteAbi = 0;
    scvb::Registry reg(backend, 1);
    reg.setAbiMismatchHandler([&](u32 l, u32 r) {
        called = true;
        localAbi = l;
        remoteAbi = r;
    });
    REQUIRE(reg.open() == scvb::Registry::ClaimResult::kAbiMismatch);
    REQUIRE(called);
    REQUIRE(localAbi == scvb::kScvbAbi);
    REQUIRE(remoteAbi == 99);
    REQUIRE_FALSE(reg.isOpen()); // 绝不半兼容
}

// ---------------------------------------------------------------------------
// ⑥ OutputGlobalInfo 单测(Output 写 → Input 读同值,gap_count 可读)
// ---------------------------------------------------------------------------

TEST_CASE("OutputGlobalInfo Output 写 → Input 读", "[ipc][lifecycle][globalinfo]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::CtrlPlane out(backend, 1);
    REQUIRE(out.open() == scvb::InitResult::kOk);
    scvb::CtrlPlane in(backend, 1);
    REQUIRE(in.open() == scvb::InitResult::kOk);

    scvb::OutputGlobalInfoSnapshot s;
    s.capture_enabled = 1;
    s.output_sample_rate = 48000;
    s.flags = scvb::kOutputEnabled;
    for (u32 i = 0; i < kMaxChannels; ++i)
    {
        s.gap_count[i] = i + 100;
        s.overlap_count[i] = i + 200;
        s.epoch_summary[i] = i + 300;
    }
    out.refreshGlobalInfo(s);

    const auto got = in.readGlobalInfo();
    REQUIRE(got.capture_enabled == 1);
    REQUIRE(got.output_sample_rate == 48000);
    REQUIRE(got.flags == scvb::kOutputEnabled);
    REQUIRE(got.gap_count[0] == 100);
    REQUIRE(got.gap_count[14] == 114);
    REQUIRE(got.overlap_count[14] == 214);
    REQUIRE(got.epoch_summary[14] == 314);
}

// ---------------------------------------------------------------------------
// ⑦ CtrlOp 单测(底层类型 u32、三值、未知 op 安全忽略)
// ---------------------------------------------------------------------------

TEST_CASE("CtrlOp 底层类型 u32 且三值与 01 §4.4-c 一致", "[ipc][lifecycle][ctrlop]")
{
    static_assert(std::is_same_v<std::underlying_type_t<scvb::CtrlOp>, u32>);
    REQUIRE(static_cast<u32>(scvb::CtrlOp::kNone) == 0);
    REQUIRE(static_cast<u32>(scvb::CtrlOp::kSetPriority) == 1);
    REQUIRE(static_cast<u32>(scvb::CtrlOp::kFpReport) == 2);

    REQUIRE(scvb::isKnownCtrlOp(scvb::CtrlOp::kNone));
    REQUIRE(scvb::isKnownCtrlOp(scvb::CtrlOp::kSetPriority));
    REQUIRE(scvb::isKnownCtrlOp(scvb::CtrlOp::kFpReport));
    REQUIRE_FALSE(scvb::isKnownCtrlOp(static_cast<scvb::CtrlOp>(99))); // 未知 op 读方安全忽略
}

// ---------------------------------------------------------------------------
// ⑧ J46 fp_report 打包/解包往返
// ---------------------------------------------------------------------------

TEST_CASE("fp_report 打包/解包往返(J46)", "[ipc][lifecycle][fpreport]")
{
    const u32 tiles[] = {0, 1, 0xFFFF};
    const u64 hashes[] = {0, 1, 0xFFFFFFFFFFFFull, 0x123456789ABCull, 0xFFFFFFFFFFFFFFFFull};
    for (const u32 tile : tiles)
    {
        for (const u64 hash : hashes)
        {
            const u64 v = scvb::packFpReport(tile, hash);
            INFO("tile=" << tile << " hash=" << hash);
            REQUIRE((v >> 48) == tile);
            REQUIRE((v & 0xFFFFFFFFFFFFull) == (hash & 0xFFFFFFFFFFFFull));
            REQUIRE(scvb::unpackFpReportTileIdx(v) == tile);
            REQUIRE(scvb::unpackFpReportHash(v) == (hash & 0xFFFFFFFFFFFFull));
        }
    }

    // tile_idx > 0xFFFF → 钳制到 0xFFFF(明确定义,非未定义行为)。
    REQUIRE(scvb::unpackFpReportTileIdx(scvb::packFpReport(0x10000, 0xAB)) == 0xFFFF);
    REQUIRE(scvb::unpackFpReportTileIdx(scvb::packFpReport(0xFFFFFFFF, 0)) == 0xFFFF);
    // hash 高位(>48 位)被截断。
    REQUIRE(scvb::unpackFpReportHash(scvb::packFpReport(0, 0x0001FFFFFFFFFFFFull)) == 0xFFFFFFFFFFFFull);
}

// ---------------------------------------------------------------------------
// ⑨ 命令环满 → 丢最旧 + 计数递增
// ---------------------------------------------------------------------------

TEST_CASE("命令环满 → 丢最旧 + 计数递增", "[ipc][lifecycle][cmdring]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::CtrlPlane in(backend, 1);
    REQUIRE(in.open() == scvb::InitResult::kOk);

    for (u32 i = 0; i < scvb::kCtrlRingCapacity; ++i)
    {
        REQUIRE(in.enqueue(3, scvb::CtrlOp::kSetPriority, i));
    }
    REQUIRE(in.overflowCount(3) == 0); // 恰好填满,未溢出

    REQUIRE(in.enqueue(3, scvb::CtrlOp::kSetPriority, 100)); // 第 17 条 → 丢最旧
    REQUIRE(in.overflowCount(3) == 1);

    scvb::CtrlRecord rec;
    u32 count = 0;
    u64 firstValue = 0;
    while (in.dequeue(3, rec))
    {
        if (count == 0)
        {
            firstValue = rec.value;
        }
        ++count;
    }
    REQUIRE(count == scvb::kCtrlRingCapacity);
    REQUIRE(firstValue == 1); // 最旧(value 0)被丢
}

// ---------------------------------------------------------------------------
// ⑩ R3/J52 停摆看门狗四格
// ---------------------------------------------------------------------------

TEST_CASE("停摆看门狗四格(R3/J52)", "[ipc][lifecycle][watchdog]")
{
    SECTION("停滞 400ms × write_head 推进 → 不清 mask")
    {
        scvb::SegmentBackendInProcess::resetAll();
        scvb::SegmentBackendInProcess backend;
        scvb::Registry out(backend, 1);
        REQUIRE(out.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(out.claimOutput(2001, 0) == scvb::Registry::ClaimResult::kClaimed);
        out.setConnectedMaskBit(1); // channel 1 在线

        scvb::CtrlPlane cp(backend, 1);
        REQUIRE(cp.open() == scvb::InitResult::kOk);
        u64 block = 0; // blockCounter 停滞
        u64 wh = 0; // write_head 推进
        cp.setBlockCounterSource([&] { return block; });
        cp.setWriteHeadSource([&](u32 ch) { return ch == 1 ? wh : 0; });
        cp.setConnectedMaskSource([&] { return out.connectedMask(); });

        REQUIRE(cp.tickWatchdog(0).action == scvb::WatchdogAction::kNone); // init
        wh = 100;
        const auto r = cp.tickWatchdog(400);
        REQUIRE(r.action == scvb::WatchdogAction::kNone); // 未过 0.5s
        REQUIRE(out.connectedMask() == (1u << 0));
    }

    SECTION("停滞 600ms × write_head 推进 → 清 mask")
    {
        scvb::SegmentBackendInProcess::resetAll();
        scvb::SegmentBackendInProcess backend;
        scvb::Registry out(backend, 1);
        REQUIRE(out.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(out.claimOutput(2001, 0) == scvb::Registry::ClaimResult::kClaimed);
        out.setConnectedMaskBit(1);

        scvb::CtrlPlane cp(backend, 1);
        REQUIRE(cp.open() == scvb::InitResult::kOk);
        u64 block = 0;
        u64 wh = 0;
        cp.setBlockCounterSource([&] { return block; });
        cp.setWriteHeadSource([&](u32 ch) { return ch == 1 ? wh : 0; });
        cp.setConnectedMaskSource([&] { return out.connectedMask(); });

        REQUIRE(cp.tickWatchdog(0).action == scvb::WatchdogAction::kNone);
        wh = 100;
        const auto r = cp.tickWatchdog(600);
        REQUIRE(r.action == scvb::WatchdogAction::kClearMask);
    }

    SECTION("停滞 600ms × write_head 也停 → 不清 mask(工程停止播放)")
    {
        scvb::SegmentBackendInProcess::resetAll();
        scvb::SegmentBackendInProcess backend;
        scvb::Registry out(backend, 1);
        REQUIRE(out.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(out.claimOutput(2001, 0) == scvb::Registry::ClaimResult::kClaimed);
        out.setConnectedMaskBit(1);

        scvb::CtrlPlane cp(backend, 1);
        REQUIRE(cp.open() == scvb::InitResult::kOk);
        u64 block = 0;
        const u64 wh = 0; // write_head 也停
        cp.setBlockCounterSource([&] { return block; });
        cp.setWriteHeadSource([&](u32 ch) { return ch == 1 ? wh : 0; });
        cp.setConnectedMaskSource([&] { return out.connectedMask(); });

        REQUIRE(cp.tickWatchdog(0).action == scvb::WatchdogAction::kNone);
        const auto r = cp.tickWatchdog(600);
        REQUIRE(r.action == scvb::WatchdogAction::kNone); // 两个条件缺一不可
        REQUIRE(out.connectedMask() == (1u << 0));
    }

    SECTION("清 mask 后恢复推进 → 让位协议逐轨重置信位(不得硬切)")
    {
        scvb::SegmentBackendInProcess::resetAll();
        scvb::SegmentBackendInProcess backend;
        scvb::Registry out(backend, 1);
        REQUIRE(out.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(out.claimOutput(2001, 0) == scvb::Registry::ClaimResult::kClaimed);
        out.setConnectedMaskBit(1);
        out.setConnectedMaskBit(2);
        out.setConnectedMaskBit(3); // 在线轨 1,2,3

        scvb::CtrlPlane cp(backend, 1);
        REQUIRE(cp.open() == scvb::InitResult::kOk);
        u64 block = 0;
        u64 wh = 0;
        cp.setBlockCounterSource([&] { return block; });
        cp.setWriteHeadSource([&](u32 ch) { return ch == 1 ? wh : 0; });
        cp.setConnectedMaskSource([&] { return out.connectedMask(); });

        REQUIRE(cp.tickWatchdog(0).action == scvb::WatchdogAction::kNone);
        wh = 100;
        REQUIRE(cp.tickWatchdog(600).action == scvb::WatchdogAction::kClearMask);
        out.clearConnectedMask(); // 执行器清 mask

        block = 1; // blockCounter 恢复推进
        const auto r1 = cp.tickWatchdog(700);
        REQUIRE(r1.action == scvb::WatchdogAction::kReacquireBit);
        REQUIRE(r1.channel == 1); // 让位协议:最小号在线轨先置位,不得硬切

        const auto r2 = cp.tickWatchdog(800);
        REQUIRE(r2.action == scvb::WatchdogAction::kNone); // ≥200ms 窗口内

        const auto r3 = cp.tickWatchdog(900);
        REQUIRE(r3.action == scvb::WatchdogAction::kReacquireBit);
        REQUIRE(r3.channel == 2);

        const auto r4 = cp.tickWatchdog(1100);
        REQUIRE(r4.action == scvb::WatchdogAction::kReacquireBit);
        REQUIRE(r4.channel == 3);

        const auto r5 = cp.tickWatchdog(1300);
        REQUIRE(r5.action == scvb::WatchdogAction::kNone); // 重置信位序列结束
    }
}

// ---------------------------------------------------------------------------
// ⑪ J66 组隔离两格
// ---------------------------------------------------------------------------

TEST_CASE("组隔离两格(J66)", "[ipc][lifecycle][group]")
{
    SECTION("两组各自 claim 同号 channel → 零冲突 + 互不可见")
    {
        scvb::SegmentBackendInProcess::resetAll();
        scvb::SegmentBackendInProcess backend;

        scvb::Registry g1in(backend, 1);
        REQUIRE(g1in.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(g1in.claimInput(3, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
        scvb::Registry g1out(backend, 1);
        REQUIRE(g1out.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(g1out.claimOutput(2001, kT0) == scvb::Registry::ClaimResult::kClaimed);
        g1out.setConnectedMaskBit(3);

        // g2 同号 channel 零冲突(独立段)。
        scvb::Registry g2in(backend, 2);
        REQUIRE(g2in.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(g2in.claimInput(3, 1002, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
        scvb::Registry g2out(backend, 2);
        REQUIRE(g2out.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(g2out.claimOutput(2002, kT0) == scvb::Registry::ClaimResult::kClaimed);

        // mask 互不可见。
        REQUIRE(g1out.connectedMask() == (1u << 2));
        REQUIRE(g2out.connectedMask() == 0);

        // 心跳互不可见(同号 slot 各自独立)。
        g1in.heartbeatInput(3, kT0 + 100);
        g2in.heartbeatInput(3, kT0 + 500);
        REQUIRE(g1in.inputSlot(3)->heartbeat_ms.load() == kT0 + 100);
        REQUIRE(g2in.inputSlot(3)->heartbeat_ms.load() == kT0 + 500);
    }

    SECTION("改组 API → 旧组归零 + 新组 claim + 异组 O3 不触发")
    {
        scvb::SegmentBackendInProcess::resetAll();
        scvb::SegmentBackendInProcess backend;

        scvb::Registry reg(backend, 1);
        REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(reg.claimInput(3, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(reg.inputSlot(3)->state.load() == kSlotActive);

        // 改组到 g2。
        REQUIRE(reg.changeGroup(2) == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(reg.group() == 2);

        // 旧组 g1 slot 3 归零。
        scvb::Registry probe(backend, 1);
        REQUIRE(probe.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(probe.inputSlot(3)->state.load() == kSlotFree);

        // 新组 claim 成功。
        REQUIRE(reg.claimInput(3, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(reg.inputSlot(3)->state.load() == kSlotActive);

        // 异组 OutputSlot 占用不触发本组 O3(只读观察为同组内语义)。
        scvb::Registry g1out(backend, 1);
        REQUIRE(g1out.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(g1out.claimOutput(2001, kT0) == scvb::Registry::ClaimResult::kClaimed);
        scvb::Registry g2out(backend, 2);
        REQUIRE(g2out.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(g2out.claimOutput(2002, kT0) == scvb::Registry::ClaimResult::kClaimed); // 非 O3 冲突
    }
}

// ---------------------------------------------------------------------------
// 辅助语义:resolveInputChannelForClaim + 时钟/pid 探活基础
// ---------------------------------------------------------------------------

TEST_CASE("resolveInputChannelForClaim 解析优先级", "[ipc][lifecycle]")
{
    using scvb::kMaxChannels;
    using scvb::resolveInputChannelForClaim;
    // env 优先。
    REQUIRE(resolveInputChannelForClaim(2, 3, false, 0) == 2);
    // 无 env → state。
    REQUIRE(resolveInputChannelForClaim(0, 4, false, 0) == 4);
    // 无 env/state → 已认领保持。
    REQUIRE(resolveInputChannelForClaim(0, 0, true, 7) == 7);
    // 全无 → 0(自动)。
    REQUIRE(resolveInputChannelForClaim(0, 0, false, 0) == 0);
    // env 越界 → 回落 state。
    REQUIRE(resolveInputChannelForClaim(99, 5, false, 0) == 5);
    // 边界:env=15 有效,env=16 越界。
    REQUIRE(resolveInputChannelForClaim(15, 0, false, 0) == 15);
    REQUIRE(resolveInputChannelForClaim(16, 0, false, 0) == 0);
    (void)kMaxChannels;
}

TEST_CASE("时钟与 pid 探活基础", "[ipc][lifecycle]")
{
    const u64 a = scvb::steadyNowMs();
    const u64 b = scvb::steadyNowMs();
    REQUIRE(b >= a); // 单调
    REQUIRE_FALSE(scvb::isProcessAlive(0)); // pid 0 恒判死
}

// ---------------------------------------------------------------------------
// DeepSeek 复审【重要】1:SegmentHandle 引用计数租约 + 消息线程握手释放
// ---------------------------------------------------------------------------

TEST_CASE("SegmentHandle 引用计数租约与握手释放", "[ipc][lifecycle][handle]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::SegmentView v;
    REQUIRE(backend.createOrOpen(L"Local\\SynchainSCVB.v1.g1.registry", scvb::kRegistrySegmentSize, v) ==
            scvb::InitResult::kOk);
    auto* hdr = static_cast<scvb::RegistryHeader*>(v.base);
    REQUIRE(backend.initHeader(v, &hdr->magic, &hdr->abi, &hdr->generation, sizeof(scvb::RegistryHeader)) ==
            scvb::InitResult::kOk);

    scvb::SegmentHandle handle(std::move(v), &backend);
    REQUIRE(handle.valid());
    const void* base = handle.base();
    REQUIRE(base != nullptr);

    // 音频线程按块租约:持有期内不解映射。
    {
        auto lease = handle.lease();
        REQUIRE(static_cast<bool>(lease));
        REQUIRE(lease.base() == base);
        // 消息线程请求释放 → 有租约在途,推迟(宽限期)。
        REQUIRE_FALSE(handle.release(0));
        REQUIRE_FALSE(handle.valid()); // 已摘指针(阻止新租约),但映射未解(租约在途)
    } // lease 析构 → 租约归还

    // 宽限期:leaseCount 归零后记录宽限期起始,宽限期届满才解映射。
    REQUIRE_FALSE(handle.release(1000)); // 首次:记录宽限期起始(未解映射)
    REQUIRE(handle.release(1000 + scvb::SegmentHandle::kReleaseGraceMs)); // 届满 → 解映射
    REQUIRE_FALSE(handle.valid());
    REQUIRE(handle.base() == nullptr);
}

// ---------------------------------------------------------------------------
// DeepSeek 复审【重要】3:initHeader 覆盖式重初始化分支仅限段 owner
// ---------------------------------------------------------------------------

TEST_CASE("initHeader allowOverwrite=false 只读方不覆盖残段", "[ipc][lifecycle][init]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::SegmentView created;
    REQUIRE(backend.createOrOpen(L"Local\\SynchainSCVB.v1.g1.registry", scvb::kRegistrySegmentSize, created) ==
            scvb::InitResult::kOk);
    // 残段:magic==0(创建者死于初始化,未 initHeader)。
    scvb::SegmentView attached;
    REQUIRE(backend.openExisting(L"Local\\SynchainSCVB.v1.g1.registry", attached) == scvb::InitResult::kOk);
    auto* hdr = static_cast<scvb::RegistryHeader*>(attached.base);
    REQUIRE(hdr->magic.load() == 0);

    // 只读方(allowOverwrite=false):非阻塞单次 magic/abi 校验,不覆盖、不重写,直接 kFailed。
    const auto r = backend.initHeader(attached, &hdr->magic, &hdr->abi, &hdr->generation, sizeof(scvb::RegistryHeader),
                                      {}, /*allowOverwrite=*/false);
    REQUIRE(r == scvb::InitResult::kFailed);
    REQUIRE(hdr->magic.load() == 0); // 未覆盖
    REQUIRE(hdr->abi.load() == 0); // 未重写
}

// ---------------------------------------------------------------------------
// PR#38 复审【重要】1:命令环 SPSC 双线程混跑(丢最旧 + 撕裂防护自洽)
// ---------------------------------------------------------------------------

TEST_CASE("命令环双线程混跑(SPSC 丢最旧自洽)", "[ipc][lifecycle][cmdring][concurrent]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;
    scvb::CtrlPlane plane(backend, 1);
    REQUIRE(plane.open() == scvb::InitResult::kOk);

    constexpr u32 kN = 100000;
    std::atomic<bool> producerDone{false};

    // 生产者:持续 enqueue;seq 从 1 递增(0 保留为「在写」标记),value == i == seq-1。
    std::thread producer([&] {
        for (u32 i = 0; i < kN; ++i)
        {
            plane.enqueue(3, scvb::CtrlOp::kSetPriority, i);
        }
        producerDone.store(true, std::memory_order_release);
    });

    // 消费者:持续 dequeue;校验 seq 单调、无 0、value==seq-1(无撕裂记录)。
    u32 lastSeq = 0;
    bool first = true;
    u32 dequeued = 0;
    for (;;)
    {
        scvb::CtrlRecord rec;
        if (plane.dequeue(3, rec))
        {
            const u32 seq = rec.seq.load(std::memory_order_relaxed);
            const u32 ch = rec.channel.load(std::memory_order_relaxed);
            const u64 val = rec.value.load(std::memory_order_relaxed);
            REQUIRE(seq != 0); // 无「在写」记录泄漏
            REQUIRE(ch == 3);
            REQUIRE(val == static_cast<u64>(seq - 1)); // 无撕裂(seq 与 value 同记录)
            if (!first)
            {
                REQUIRE(seq > lastSeq); // 单调递增无回退
            }
            lastSeq = seq;
            first = false;
            ++dequeued;
        }
        else if (producerDone.load(std::memory_order_acquire))
        {
            break;
        }
        else
        {
            std::this_thread::yield();
        }
    }
    producer.join();

    // 溢出计数与丢记录数自洽:生产者只在「见满」时 overflow++,其 read_pos 快照可能略陈旧
    // (消费者刚消费/跳过的槽被误判为满)→ overflow 是丢记录数的上界,绝不低估。
    // 守恒律(每条记录要么被消费、要么被丢)给出 exact 关系:overflow >= kN - dequeued。
    const u32 overflow = plane.overflowCount(3);
    REQUIRE(dequeued + overflow >= kN); // overflow 是丢记录上界(生产者的陈旧 read_pos 可能少量高估)
    REQUIRE(overflow <= kN); // 显然上界:每 enqueue 至多 overflow++ 一次
}

// ---------------------------------------------------------------------------
// PR#38 复审【重要】2:接管分支仅限 kSlotActive(kSlotClaimed 进行中绝不接管)
// ---------------------------------------------------------------------------

TEST_CASE("claimInput 接管分支仅限 kSlotActive", "[ipc][lifecycle][takeover]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry a(backend, 1);
    REQUIRE(a.open() == scvb::Registry::ClaimResult::kClaimed);
    // 手工把槽置为 kSlotClaimed(进行中)+ pid=0/hb=0。
    auto* slot = a.inputSlot(3);
    slot->state.store(kSlotClaimed, std::memory_order_release);
    slot->pid = 0;
    slot->heartbeat_ms.store(0, std::memory_order_relaxed);

    // 第二 claimer(死 pid 探活、时间远超 5000ms)必须 kConflict,不得接管进行中的槽。
    const bool holderAlive = false;
    scvb::Registry b(backend, 1, [&](u32) { return holderAlive; });
    REQUIRE(b.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(b.claimInput(3, 1002, 48000, 512, kT0 + 6000) == scvb::Registry::ClaimResult::kConflict);

    // 槽仍 kSlotClaimed、pid 0(未被接管)。
    REQUIRE(slot->state.load() == kSlotClaimed);
    REQUIRE(slot->pid == 0);
}

TEST_CASE("claimOutput 接管分支仅限 kSlotActive", "[ipc][lifecycle][takeover]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry a(backend, 1);
    REQUIRE(a.open() == scvb::Registry::ClaimResult::kClaimed);
    auto* oslot = a.outputSlot();
    oslot->state.store(kSlotClaimed, std::memory_order_release);
    oslot->pid = 0;
    oslot->heartbeat_ms.store(0, std::memory_order_relaxed);

    const bool holderAlive = false;
    scvb::Registry b(backend, 1, [&](u32) { return holderAlive; });
    REQUIRE(b.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(b.claimOutput(2002, kT0 + 6000) == scvb::Registry::ClaimResult::kConflict);
    REQUIRE(oslot->state.load() == kSlotClaimed);
    REQUIRE(oslot->pid == 0);
}

// ---------------------------------------------------------------------------
// PR#38 复审【重要】3:延迟释放句柄(租约在途入 pending,reap 回收,不泄漏)
// ---------------------------------------------------------------------------

TEST_CASE("延迟释放句柄:租约在途入 pending,reap 回收", "[ipc][lifecycle][handle]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry reg(backend, 1);
    REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(reg.claimInput(3, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);

    {
        // 音频线程持有租约(旧组 g1 段)。
        auto lease = reg.lease();
        REQUIRE(static_cast<bool>(lease));

        // 改组:租约在途 → 旧句柄入 pendingReleases_(未解映射,不泄漏)。
        REQUIRE(reg.changeGroup(2) == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(reg.pendingReleaseCount() == 1);
    } // lease 析构 → 租约归还

    // [M] reap:leaseCount 归零后记录宽限期起始,宽限期届满才解映射。
    reg.reapPendingReleases(1000);
    REQUIRE(reg.pendingReleaseCount() == 1); // 宽限期未满,仍在 pending
    reg.reapPendingReleases(1000 + scvb::SegmentHandle::kReleaseGraceMs); // 届满
    REQUIRE(reg.pendingReleaseCount() == 0);
}
// ---------------------------------------------------------------------------
// PR#38 复审【红旗】1d:SegmentHandle 并发 lease/release(原子裸指针无 UAF)
// ---------------------------------------------------------------------------

TEST_CASE("SegmentHandle 并发 lease/release(原子裸指针无 UAF)", "[ipc][lifecycle][handle][concurrent]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    auto publish = [&]() {
        scvb::SegmentView v;
        REQUIRE(backend.createOrOpen(L"Local\\SynchainSCVB.v1.g1.registry", scvb::kRegistrySegmentSize, v) ==
                scvb::InitResult::kOk);
        auto* hdr = static_cast<scvb::RegistryHeader*>(v.base);
        REQUIRE(backend.initHeader(v, &hdr->magic, &hdr->abi, &hdr->generation, sizeof(scvb::RegistryHeader)) ==
                scvb::InitResult::kOk);
        return scvb::SegmentHandle(std::move(v), &backend);
    };

    scvb::SegmentHandle handle = publish();
    std::vector<scvb::SegmentHandle> pending; // 消息线程延迟释放列表(保活)

    std::atomic<bool> stop{false};
    std::atomic<u64> leases{0};

    // 音频线程:循环 lease()/归还(只读 implPtr_ 原子裸指针)。
    std::thread audio([&] {
        while (!stop.load(std::memory_order_acquire))
        {
            auto lease = handle.lease();
            if (lease)
            {
                REQUIRE(lease.base() != nullptr);
                ++leases;
            }
        }
    });

    // 消息线程:循环 release() + 重新发布新映射(写 implPtr_ 原子 + impl_ shared_ptr 成员)。
    std::thread message([&] {
        u64 nowMs = 0;
        for (int i = 0; i < 3000; ++i)
        {
            nowMs += 100; // 推进虚拟时间,宽限期(500ms)周期性届满
            if (!handle.release(nowMs))
            {
                pending.push_back(std::move(handle)); // 租约在途/宽限期未满 → 入 pending 保活
            }
            handle = publish(); // 重新发布
            for (auto it = pending.begin(); it != pending.end();)
            {
                if (it->release(nowMs))
                {
                    it = pending.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        stop.store(true, std::memory_order_release);
    });

    audio.join();
    message.join();

    REQUIRE(leases.load() > 0); // 音频线程确实租约过
    // 收尾:宽限期是 500ms,release 需两拍(首次记录宽限期起始,次拍届满解映射)。
    const u64 finalNowMs = 1000000000;
    for (auto& h : pending)
    {
        h.release(finalNowMs);
        REQUIRE(h.release(finalNowMs + scvb::SegmentHandle::kReleaseGraceMs));
    }
    REQUIRE_FALSE(handle.release(finalNowMs)); // 首次:宽限期起始
    REQUIRE(handle.release(finalNowMs + scvb::SegmentHandle::kReleaseGraceMs)); // 届满
    REQUIRE_FALSE(handle.valid());
}

// ---------------------------------------------------------------------------
// PR#38 复审【重要】2:命令环 seq 由共享 write_pos 派生(多 channel / 生产者重启)
// ---------------------------------------------------------------------------

TEST_CASE("命令环多 channel 交织各环独立", "[ipc][lifecycle][cmdring]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::CtrlPlane producer(backend, 1);
    REQUIRE(producer.open() == scvb::InitResult::kOk);
    // 每环 10 条(容量 16,不溢出),两环各自独立。
    for (u32 i = 0; i < 10; ++i)
    {
        REQUIRE(producer.enqueue(3, scvb::CtrlOp::kSetPriority, i * 2));
        REQUIRE(producer.enqueue(5, scvb::CtrlOp::kSetPriority, i * 2 + 1));
    }

    scvb::CtrlPlane consumer(backend, 1);
    REQUIRE(consumer.open() == scvb::InitResult::kOk);
    scvb::CtrlRecord rec;
    for (u32 i = 0; i < 10; ++i)
    {
        REQUIRE(consumer.dequeue(3, rec));
        REQUIRE(rec.seq.load() == i + 1);
        REQUIRE(rec.value.load() == i * 2);
        REQUIRE(consumer.dequeue(5, rec));
        REQUIRE(rec.seq.load() == i + 1);
        REQUIRE(rec.value.load() == i * 2 + 1);
    }
    REQUIRE_FALSE(consumer.dequeue(3, rec)); // 两环各自独立耗尽
    REQUIRE_FALSE(consumer.dequeue(5, rec));
}

TEST_CASE("命令环生产者重启 seq 由 write_pos 派生不卡死", "[ipc][lifecycle][cmdring]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    {
        scvb::CtrlPlane p1(backend, 1);
        REQUIRE(p1.open() == scvb::InitResult::kOk);
        for (u32 i = 0; i < 5; ++i)
        {
            REQUIRE(p1.enqueue(3, scvb::CtrlOp::kSetPriority, i));
        }
    } // p1 销毁(模拟崩溃)

    {
        scvb::CtrlPlane p2(backend, 1); // 同 backend 同 group 重建
        REQUIRE(p2.open() == scvb::InitResult::kOk);
        for (u32 i = 5; i < 10; ++i)
        {
            REQUIRE(p2.enqueue(3, scvb::CtrlOp::kSetPriority, i));
        }
    }

    scvb::CtrlPlane consumer(backend, 1);
    REQUIRE(consumer.open() == scvb::InitResult::kOk);
    scvb::CtrlRecord rec;
    u32 count = 0;
    while (consumer.dequeue(3, rec))
    {
        REQUIRE(rec.seq.load() == count + 1); // seq = write_pos+1,重启后续用,不中断
        REQUIRE(rec.value.load() == count);
        ++count;
    }
    REQUIRE(count == 10);
}

// ---------------------------------------------------------------------------
// PR#38 复审【重要】3:幽灵槽(kSlotClaimed+pid==0 超宽限期接管)
// ---------------------------------------------------------------------------

TEST_CASE("幽灵槽接管 claimInput(kSlotClaimed+pid==0 超宽限期)", "[ipc][lifecycle][takeover]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry a(backend, 1);
    REQUIRE(a.open() == scvb::Registry::ClaimResult::kClaimed);
    auto* slot = a.inputSlot(3);
    slot->state.store(kSlotClaimed, std::memory_order_release);
    slot->pid = 0;
    slot->heartbeat_ms.store(0, std::memory_order_relaxed); // 幽灵槽(claim 者 CAS 后、fill 前崩溃)

    const bool holderAlive = false;
    scvb::Registry b(backend, 1, [&](u32) { return holderAlive; });
    REQUIRE(b.open() == scvb::Registry::ClaimResult::kClaimed);

    // 首见(宽限期内)→ kConflict。
    REQUIRE(b.claimInput(3, 1002, 48000, 512, kT0) == scvb::Registry::ClaimResult::kConflict);
    // 虚拟时钟前进 > kClaimGhostGraceMs → 接管成功且槽恢复。
    REQUIRE(b.claimInput(3, 1002, 48000, 512, kT0 + scvb::kClaimGhostGraceMs + 1) ==
            scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(slot->state.load() == kSlotActive);
    REQUIRE(slot->pid == 1002);
}

TEST_CASE("幽灵槽接管 claimOutput(kSlotClaimed+pid==0 超宽限期)", "[ipc][lifecycle][takeover]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry a(backend, 1);
    REQUIRE(a.open() == scvb::Registry::ClaimResult::kClaimed);
    auto* oslot = a.outputSlot();
    oslot->state.store(kSlotClaimed, std::memory_order_release);
    oslot->pid = 0;
    oslot->heartbeat_ms.store(0, std::memory_order_relaxed);

    const bool holderAlive = false;
    scvb::Registry b(backend, 1, [&](u32) { return holderAlive; });
    REQUIRE(b.open() == scvb::Registry::ClaimResult::kClaimed);

    REQUIRE(b.claimOutput(2002, kT0) == scvb::Registry::ClaimResult::kConflict);
    REQUIRE(b.claimOutput(2002, kT0 + scvb::kClaimGhostGraceMs + 1) == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(oslot->state.load() == kSlotActive);
    REQUIRE(oslot->pid == 2002);
}

// ---------------------------------------------------------------------------
// PR#38 复审【顺带补齐】4b/4c/4d
// ---------------------------------------------------------------------------

TEST_CASE("claimOutput 接管判定四格(J10 双条件)", "[ipc][lifecycle][takeover]")
{
    SECTION("4900ms × pid 存活 → 不接管")
    {
        scvb::SegmentBackendInProcess::resetAll();
        scvb::SegmentBackendInProcess backend;
        scvb::Registry first(backend, 1);
        REQUIRE(first.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(first.claimOutput(2001, kT0) == scvb::Registry::ClaimResult::kClaimed);
        const bool alive = true;
        scvb::Registry second(backend, 1, [&](u32) { return alive; });
        REQUIRE(second.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(second.claimOutput(2002, kT0 + 4900) == scvb::Registry::ClaimResult::kConflict);
        REQUIRE(first.outputSlot()->pid == 2001);
    }
    SECTION("4900ms × pid 已死 → 不接管(未过 5000ms)")
    {
        scvb::SegmentBackendInProcess::resetAll();
        scvb::SegmentBackendInProcess backend;
        scvb::Registry first(backend, 1);
        REQUIRE(first.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(first.claimOutput(2001, kT0) == scvb::Registry::ClaimResult::kClaimed);
        const bool alive = false;
        scvb::Registry second(backend, 1, [&](u32) { return alive; });
        REQUIRE(second.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(second.claimOutput(2002, kT0 + 4900) == scvb::Registry::ClaimResult::kConflict);
        REQUIRE(first.outputSlot()->pid == 2001);
    }
    SECTION("5100ms × pid 存活 → 不接管(探活是第二必要条件)")
    {
        scvb::SegmentBackendInProcess::resetAll();
        scvb::SegmentBackendInProcess backend;
        scvb::Registry first(backend, 1);
        REQUIRE(first.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(first.claimOutput(2001, kT0) == scvb::Registry::ClaimResult::kClaimed);
        const bool alive = true;
        scvb::Registry second(backend, 1, [&](u32) { return alive; });
        REQUIRE(second.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(second.claimOutput(2002, kT0 + 5100) == scvb::Registry::ClaimResult::kConflict);
        REQUIRE(first.outputSlot()->pid == 2001);
    }
    SECTION("5100ms × pid 已死 → 接管且唯一胜者")
    {
        scvb::SegmentBackendInProcess::resetAll();
        scvb::SegmentBackendInProcess backend;
        scvb::Registry first(backend, 1);
        REQUIRE(first.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(first.claimOutput(2001, kT0) == scvb::Registry::ClaimResult::kClaimed);
        const bool alive = false;
        scvb::Registry second(backend, 1, [&](u32) { return alive; });
        scvb::Registry third(backend, 1, [&](u32) { return alive; });
        REQUIRE(second.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(third.open() == scvb::Registry::ClaimResult::kClaimed);
        const auto r2 = second.claimOutput(2002, kT0 + 5100);
        const auto r3 = third.claimOutput(2003, kT0 + 5100);
        const bool r2won = (r2 == scvb::Registry::ClaimResult::kClaimed);
        const bool r3won = (r3 == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(r2won != r3won); // 恰好一个胜者
        const bool atLeastOne = r2won || r3won;
        REQUIRE(atLeastOne);
    }
}

TEST_CASE("第二个 Output 同 pid 重认领不清 mask / 异 pid 活跃 O3", "[ipc][lifecycle]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry a(backend, 1);
    REQUIRE(a.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(a.claimOutput(2001, kT0) == scvb::Registry::ClaimResult::kClaimed);
    a.setConnectedMaskBit(3);
    a.bumpConfigSeq();
    REQUIRE(a.connectedMask() == (1u << 2));
    REQUIRE(a.configSeq() == 1);

    // 同 pid 重认领:只刷新 pid/心跳,不清 mask、不清 config_seq。
    scvb::Registry b(backend, 1);
    REQUIRE(b.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(b.claimOutput(2001, kT0 + 100) == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(a.connectedMask() == (1u << 2));
    REQUIRE(a.configSeq() == 1);

    // 异 pid 活跃 → kConflict(只读观察 O3 机制)。
    scvb::Registry c(backend, 1);
    REQUIRE(c.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(c.claimOutput(2002, kT0 + 200) == scvb::Registry::ClaimResult::kConflict);
    REQUIRE(a.outputSlot()->pid == 2001);
}

TEST_CASE("updateOwnedInputSlot 非属主调用为空操作", "[ipc][lifecycle]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    scvb::Registry a(backend, 1);
    REQUIRE(a.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(a.claimInput(3, 1001, 48000, 512, kT0) == scvb::Registry::ClaimResult::kClaimed);

    // 非属主(pid 1002):不刷新。
    a.updateOwnedInputSlot(3, 1002, 44100, 1024);
    REQUIRE(a.inputSlot(3)->sample_rate == 48000);
    REQUIRE(a.inputSlot(3)->max_block == 512);

    // 属主(pid 1001):刷新。
    a.updateOwnedInputSlot(3, 1001, 44100, 1024);
    REQUIRE(a.inputSlot(3)->sample_rate == 44100);
    REQUIRE(a.inputSlot(3)->max_block == 1024);
}
