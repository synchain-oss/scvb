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
#include "state/StateCodec.h"
#include "state/StateMigration.h"

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
    scvb::AudioRing::write(in.audioRing().acquire(), 0, buf, 16); // 推进 write_head

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
    scvb::AudioRing::write(in.audioRing().acquire(), 0, buf, 16);

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

TEST_CASE("Output state 容器:abi 高于当前 → RejectedNewer + preservedOriginal 原样回写", "[output][state][abi]")
{
    // PR#53 R1:setStateInformation 经 loadState 做 abi 判读 —— 高 abi 拒载并保留原 blob(getStateInformation
    // 原样回写,绝不静默降级;CLAUDE.md §7.3 / STATE_SCHEMA)。
    scvb::state::OutputState s;
    s.groupId = 5;
    std::vector<std::uint8_t> cfg;
    REQUIRE(scvb::state::encodeOutputState(s, cfg));

    scvb::state::StateChunks chunks;
    chunks.abi = scvb::state::kCurrentAbi;
    chunks.set(scvb::state::kFourccCfgs, cfg);
    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeContainer(chunks, enc));

    enc[4] = 2; // abi 1 → 2(高版本)
    scvb::state::StateChunks out;
    scvb::state::StateLoadResult res = scvb::state::loadState(enc.data(), enc.size(), out);
    REQUIRE(res.status == scvb::state::StateLoadStatus::RejectedNewer);
    REQUIRE(res.preservedOriginal == enc); // 原样回写,不毁高版本数据
}

TEST_CASE("Output state 容器:重建 PRMS/CFGS 时 FEAT/CRVS 原样回写", "[output][state]")
{
    // 模拟 getStateInformation:load 一次含 FEAT/CRVS 的容器,原位替换 PRMS/CFGS 后 encode,
    // FEAT/CRVS 必须逐字节保留(T19 未知 fourcc 回写纪律)。
    const std::vector<std::uint8_t> prms0 = {0x50, 0x30}; // "P0"
    const std::vector<std::uint8_t> cfgs0 = {0x43, 0x30}; // "C0"
    scvb::state::StateChunks chunks;
    chunks.abi = scvb::state::kCurrentAbi;
    chunks.set(scvb::state::kFourccPrms, prms0);
    chunks.set(scvb::state::kFourccCfgs, cfgs0);
    const std::vector<std::uint8_t> feat = {0xDE, 0xAD, 0xBE, 0xEF};
    const std::vector<std::uint8_t> crvs = {0x01, 0x00, 0x02, 0x0F, 0xAA, 0xBB};
    chunks.chunks.push_back(scvb::state::Chunk{scvb::state::kFourccFeat, feat});
    chunks.chunks.push_back(scvb::state::Chunk{scvb::state::kFourccCrvs, crvs});
    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeContainer(chunks, enc));

    scvb::state::StateChunks loaded;
    REQUIRE(scvb::state::loadState(enc.data(), enc.size(), loaded).status == scvb::state::StateLoadStatus::Ok);

    // 重建:原位替换 PRMS/CFGS(abi 保持当前)。
    loaded.abi = scvb::state::kCurrentAbi;
    loaded.set(scvb::state::kFourccPrms, std::vector<std::uint8_t>{0x50, 0x31}); // "P1"
    loaded.set(scvb::state::kFourccCfgs, std::vector<std::uint8_t>{0x43, 0x31}); // "C1"
    std::vector<std::uint8_t> enc2;
    REQUIRE(scvb::state::encodeContainer(loaded, enc2));

    scvb::state::StateChunks round;
    REQUIRE(scvb::state::loadState(enc2.data(), enc2.size(), round).status == scvb::state::StateLoadStatus::Ok);
    REQUIRE(round.find(scvb::state::kFourccPrms)->payload == std::vector<std::uint8_t>{0x50, 0x31});
    REQUIRE(round.find(scvb::state::kFourccCfgs)->payload == std::vector<std::uint8_t>{0x43, 0x31});
    REQUIRE(round.find(scvb::state::kFourccFeat)->payload == feat); // 原样保留
    REQUIRE(round.find(scvb::state::kFourccCrvs)->payload == crvs); // 原样保留
}
