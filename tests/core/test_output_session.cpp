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

TEST_CASE("OutputStateCodec:ui 首启已读位往返 + 老工程(无尾部)兼容", "[output][state][t37]")
{
    // T37 真机 bug A-3:guide_seen / tour_seen 此前只活在 OutputRuntimeState,从不入 state chunk,
    // 重开工程即回到「首启」——红字九条页与 tour 每次重放。两位必须与语言同样往返。
    scvb::state::OutputState s;
    REQUIRE(s.uiGuideSeen == 0); // 新工程默认「没看过」
    REQUIRE(s.uiTourSeen == 0);

    s.uiLanguage = "zh";
    s.uiGuideSeen = 1;
    s.uiTourSeen = 1;

    std::vector<std::uint8_t> buf;
    REQUIRE(scvb::state::encodeOutputState(s, buf));

    scvb::state::OutputState d;
    REQUIRE(scvb::state::decodeOutputState(buf.data(), buf.size(), d));
    REQUIRE(d.uiLanguage == "zh");
    REQUIRE(d.uiGuideSeen == 1);
    REQUIRE(d.uiTourSeen == 1);

    // 老工程 payload(24 + langBytes,无尾部两位)仍解得动,两位取默认 0。
    std::vector<std::uint8_t> legacy(buf.begin(), buf.end() - 8);
    scvb::state::OutputState l;
    REQUIRE(scvb::state::decodeOutputState(legacy.data(), legacy.size(), l));
    REQUIRE(l.uiLanguage == "zh");
    REQUIRE(l.uiGuideSeen == 0);
    REQUIRE(l.uiTourSeen == 0);

    // 尾部布尔越界 / 半截尾部 → 拒载(CLAUDE.md §7.3 不可信字节)。
    std::vector<std::uint8_t> tampered = buf;
    tampered[tampered.size() - 8] = 2;
    scvb::state::OutputState t;
    REQUIRE_FALSE(scvb::state::decodeOutputState(tampered.data(), tampered.size(), t));

    std::vector<std::uint8_t> halfTrailer(buf.begin(), buf.end() - 4);
    scvb::state::OutputState h;
    REQUIRE_FALSE(scvb::state::decodeOutputState(halfTrailer.data(), halfTrailer.size(), h));
}

TEST_CASE("heartbeatAgeMsOf:哨兵 / 时钟倒退 / 溢出钳位", "[output][conn][t37]")
{
    using scvb::output::heartbeatAgeMsOf;
    using scvb::output::kHeartbeatAgeUnknown;

    // 契约 §2.3:slotState=0 或从未心跳 → 0xFFFFFFFF 哨兵(UI 据此判 heartbeatFresh=false)。
    REQUIRE(heartbeatAgeMsOf(scvb::kSlotFree, 12345, 20000) == kHeartbeatAgeUnknown);
    REQUIRE(heartbeatAgeMsOf(scvb::kSlotActive, 0, 20000) == kHeartbeatAgeUnknown);
    // 正常年龄。
    REQUIRE(heartbeatAgeMsOf(scvb::kSlotActive, 19800, 20000) == 200);
    // 时钟倒退(steady clock 不该发生,但跨实例读值不做信任假设)→ 0,不出负数回绕。
    REQUIRE(heartbeatAgeMsOf(scvb::kSlotActive, 21000, 20000) == 0);
    // 溢出钳到「哨兵-1」:真实的超长年龄绝不能被误读成「无数据」。
    REQUIRE(heartbeatAgeMsOf(scvb::kSlotActive, 1, 0x1'0000'0000ull) == kHeartbeatAgeUnknown - 1u);
}

TEST_CASE("channelConn:桥面 conn 数据面来自 registry 实况(T37 bug B)", "[output][conn][t37]")
{
    // T37 真机 bug B:Input 侧显示已连接、音频也通(Input 静音、总线出处理后的声音),
    // 但 Output 轨道页永远是「组 X 尚无输入」—— 因为 OutputEditor::buildConnPayload 是 T29 占位:
    // 全轨 slotState 由 claim 态推导、heartbeatFresh 恒 false,而 UI 的连接数口径是
    // 「slotState=2 ∧ heartbeatFresh」(契约 §2.3 / J01),恒为 0。断点在数据面,不在音频环。
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession in(backend, 1001);
    in.setChannelId(3);
    REQUIRE(in.prepare(48000, 512, 1, 1000) == InputClaimState::kActive);
    in.heartbeat(1100);

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1200) == OutputClaimState::kActive);

    // 已认领的 ch3:活跃 + 心跳新鲜(年龄 ≤ 2000 ⇒ UI 的 heartbeatFresh 为真)。
    const auto ch3 = out.channelConn(3, 1300);
    REQUIRE(ch3.slotState == kSlotActive);
    REQUIRE(ch3.heartbeatAgeMs == 200);
    REQUIRE(ch3.heartbeatAgeMs <= static_cast<u32>(scvb::kStaleDisplayMs)); // = UI 侧 heartbeatFresh
    REQUIRE_FALSE(ch3.srMismatch);

    // 未认领的轨:空闲 + 哨兵年龄(UI 显示未连接,而不是「活跃但不新鲜」)。
    const auto ch1 = out.channelConn(1, 1300);
    REQUIRE(ch1.slotState == kSlotFree);
    REQUIRE(ch1.heartbeatAgeMs == scvb::output::kHeartbeatAgeUnknown);

    // 心跳停发 > 2000ms → 年龄越过显示阈值(UI 转「失联」,J10 双阈值的显示半边)。
    const auto stale = out.channelConn(3, 1100 + scvb::kStaleDisplayMs + 500);
    REQUIRE(stale.slotState == kSlotActive);
    REQUIRE(stale.heartbeatAgeMs > static_cast<u32>(scvb::kStaleDisplayMs));

    // 非法 channel 一律回默认(不越界读 registry)。
    REQUIRE(out.channelConn(0, 1300).slotState == kSlotFree);
    REQUIRE(out.channelConn(16, 1300).heartbeatAgeMs == scvb::output::kHeartbeatAgeUnknown);
}

TEST_CASE("channelConn:采样率不一致 → srMismatch(§2.3 该轨禁用)", "[output][conn][t37]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession in(backend, 1001);
    in.setChannelId(5);
    REQUIRE(in.prepare(44100, 512, 1, 1000) == InputClaimState::kActive); // Input 44.1k
    in.heartbeat(1100);

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1200) == OutputClaimState::kActive); // Output 48k

    const auto ch5 = out.channelConn(5, 1300);
    REQUIRE(ch5.slotState == kSlotActive);
    REQUIRE(ch5.srMismatch);
    // 空闲槽不报采样率不一致(sample_rate=0 是「未知」,不是「不同」)。
    REQUIRE_FALSE(out.channelConn(6, 1300).srMismatch);
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

TEST_CASE("[J66] 改组切换后 sources 读新组环 + 旧 slot 释放(缺陷1 语义)", "[output][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // group 1:Input ch1 写环,Output claim + attach。
    InputSession in1(backend, 1001);
    in1.setChannelId(1);
    REQUIRE(in1.prepare(48000, 512, 1, 1000) == InputClaimState::kActive);
    in1.heartbeat(1100);
    float b1[16] = {};
    for (int i = 0; i < 16; ++i)
    {
        b1[i] = 1.0f;
    }
    scvb::AudioRing::write(in1.audioRing().acquire(), 0, b1, 16);

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1200) == OutputClaimState::kActive);
    REQUIRE(out.mixSource(1).bound());
    float d0[16] = {};
    REQUIRE(out.mixSource(1).read(0, d0, 16));
    REQUIRE(d0[0] == 1.0f); // 读 group 1 环

    // group 2:Input ch1 写不同数据。
    InputSession in2(backend, 1002);
    in2.setChannelId(1);
    in2.setGroupId(2);
    REQUIRE(in2.prepare(48000, 512, 1, 1300) == InputClaimState::kActive);
    in2.heartbeat(1400);
    float b2[16] = {};
    for (int i = 0; i < 16; ++i)
    {
        b2[i] = 2.0f;
    }
    scvb::AudioRing::write(in2.audioRing().acquire(), 0, b2, 16);

    // 切组:旧 slot 释放、旧绑定解除、新组 claim + attach 新环。
    REQUIRE(out.changeGroup(2, 48000, 512, 1500) == OutputClaimState::kActive);

    scvb::Registry p1(backend, 1);
    REQUIRE(p1.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(p1.outputSlot()->state.load() == kSlotFree); // 旧 group 1 OutputSlot 释放

    REQUIRE(out.mixSource(1).bound());
    float d1[16] = {};
    REQUIRE(out.mixSource(1).read(0, d1, 16));
    REQUIRE(d1[0] == 2.0f); // 读新 group 2 环,不再是旧 group 1 环
}

TEST_CASE("observer 态 tick 会 reap pending 句柄(缺陷2)", "[output][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // 统一用真实稳态时钟作时间基准,保证 claim 心跳新鲜度与 500ms 宽限期口径一致。
    const scvb::u64 base = scvb::steadyNowMs();

    // a 占 group 2。
    OutputSession a(backend, 2001);
    REQUIRE(a.prepare(48000, 512, base) == OutputClaimState::kActive);
    REQUIRE(a.changeGroup(2, 48000, 512, base + 100) == OutputClaimState::kActive);

    // group 1 的 Input 写环,使 b 在 group 1 attach audio 环。
    InputSession in(backend, 1001);
    in.setChannelId(1);
    REQUIRE(in.prepare(48000, 512, 1, base) == InputClaimState::kActive);
    in.heartbeat(base + 100);
    float buf[16] = {};
    scvb::AudioRing::write(in.audioRing().acquire(), 0, buf, 16);

    // b 在 group 1 活跃(attach group 1 audio 环)。
    OutputSession b(backend, 2002);
    REQUIRE(b.prepare(48000, 512, base + 200) == OutputClaimState::kActive);

    // 切到被占 group 2 → observer(a 心跳距 base+300 仅 200ms,仍新鲜,非 stale 接管)。
    REQUIRE(b.changeGroup(2, 48000, 512, base + 300) == OutputClaimState::kObserver);
    REQUIRE(b.pendingReleaseCount() > 0);

    // observer 分支 tick → reap:宽限期 500ms 届满后 pending 清空(缺陷2 修复,否则映射积累)。
    b.tick(base + 1000);
    REQUIRE(b.pendingReleaseCount() == 0);
}

TEST_CASE("[J66] changeGroup 后新组同 idx 轨重新过 200ms 注入延迟(第5轮)", "[output][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    // group 1:Input ch3 上线,Output 注入满 200ms。
    InputSession in1(backend, 1001);
    in1.setChannelId(3);
    REQUIRE(in1.prepare(48000, 512, 1, 1000) == InputClaimState::kActive);
    in1.heartbeat(1100);
    float buf1[16] = {};
    scvb::AudioRing::write(in1.audioRing().acquire(), 0, buf1, 16);

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1200) == OutputClaimState::kActive);
    out.tick(1300); // 首次上线(onlineSinceMs=1300)
    out.tick(1600); // ≥200ms → injectMask 置 ch3
    REQUIRE((out.injectMask() & (1u << 2)) != 0);

    // group 2:Input ch3 上线(同 idx)。
    InputSession in2(backend, 1002);
    in2.setChannelId(3);
    in2.setGroupId(2);
    REQUIRE(in2.prepare(48000, 512, 1, 1600) == InputClaimState::kActive);
    in2.heartbeat(1700);
    float buf2[16] = {};
    scvb::AudioRing::write(in2.audioRing().acquire(), 0, buf2, 16);

    // 切到 group 2:per-channel 跟踪被重置 → 同 idx 轨首次上线必须重新过 200ms 注入延迟。
    REQUIRE(out.changeGroup(2, 48000, 512, 1800) == OutputClaimState::kActive);

    out.tick(1900); // 新组 ch3 首次上线:injectMask 不应立即置位(旧 onlinePrev_ 残留会绕过延迟)
    scvb::Registry probe2(backend, 2);
    REQUIRE(probe2.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE((probe2.connectedMask() & (1u << 2)) != 0); // ch3 在线(未被陈旧 write_head 误判挂起)
    REQUIRE((out.injectMask() & (1u << 2)) == 0); // 200ms 注入延迟重新计时

    out.tick(2200); // 1900+300ms ≥200ms → 注入
    REQUIRE((out.injectMask() & (1u << 2)) != 0);
}
