// SPDX-License-Identifier: GPL-3.0-or-later
// test_output_session —— OutputSession 生命周期单测(claim/observer/[J32] 200ms 注入延迟/
// [J66] 改组)+ OutputStateCodec 往返。用 SegmentBackendInProcess 模拟多实例(进程内,不碰全局段)。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "analysis/LoudnessMode.h"
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

TEST_CASE("UiConfig(UICF) roundtrip:默认/两值/未知回退/非法长度拒载", "[output][state]")
{
    // [J75] T43:两值往返(0=distribution | 1=trajectory),payload 定长 4 字节。
    const std::uint32_t tags[2] = {scvb::state::kMasterChartModeDistribution, scvb::state::kMasterChartModeTrajectory};
    for (std::uint32_t tag : tags)
    {
        std::vector<std::uint8_t> buf;
        REQUIRE(scvb::state::encodeUiConfig(tag, buf));
        REQUIRE(buf.size() == scvb::state::kUiConfigBytes);
        std::uint32_t d = 99;
        REQUIRE(scvb::state::decodeUiConfig(buf.data(), buf.size(), d));
        REQUIRE(d == tag);
    }

    // 未知取值(≥2)回落默认 distribution,不拒载。
    {
        std::vector<std::uint8_t> buf;
        REQUIRE(scvb::state::encodeUiConfig(7, buf));
        std::uint32_t d = 99;
        REQUIRE(scvb::state::decodeUiConfig(buf.data(), buf.size(), d));
        REQUIRE(d == scvb::state::kMasterChartModeDistribution);
    }

    // 非法长度(1 / 5 / 8 / 12 字节,均非 4)→ 拒载并回落默认(§7.3 钉死)。
    for (std::size_t badLen : {std::size_t(1), std::size_t(5), std::size_t(8), std::size_t(12)})
    {
        std::vector<std::uint8_t> bad(badLen, 0xFF);
        std::uint32_t d = 99;
        REQUIRE_FALSE(scvb::state::decodeUiConfig(bad.data(), bad.size(), d));
        REQUIRE(d == scvb::state::kMasterChartModeDistribution);
    }
}

TEST_CASE("OutputStateCodec:[J69/U24] loudness_mode/center_slot_policy 默认与两值逐字节往返", "[output][state]")
{
    // 默认档:kw_integrated / priority_queue。
    {
        scvb::state::OutputState s;
        REQUIRE(s.loudnessMode == "kw_integrated");
        REQUIRE(s.centerSlotPolicy == "priority_queue");
        std::vector<std::uint8_t> b1;
        REQUIRE(scvb::state::encodeOutputState(s, b1));
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        REQUIRE(scvb::state::decodeOutputState(b1.data(), b1.size(), d, &r));
        REQUIRE(d.loudnessMode == "kw_integrated");
        REQUIRE(d.centerSlotPolicy == "priority_queue");
        REQUIRE(r.loudnessModeFallbacks == 0);
        REQUIRE(r.centerSlotPolicyFallbacks == 0);
        std::vector<std::uint8_t> b2;
        REQUIRE(scvb::state::encodeOutputState(d, b2));
        REQUIRE(b1 == b2); // save→load→save 逐字节一致
    }
    // 两值:loudness=rms/peak_dbfs,center=lead_exclusive/even_spread。
    const std::string lm[2] = {"rms", "peak_dbfs"};
    const std::string cp[2] = {"lead_exclusive", "even_spread"};
    for (int i = 0; i < 2; ++i)
    {
        scvb::state::OutputState s;
        s.loudnessMode = lm[i];
        s.centerSlotPolicy = cp[i];
        std::vector<std::uint8_t> b1;
        REQUIRE(scvb::state::encodeOutputState(s, b1));
        scvb::state::OutputState d;
        REQUIRE(scvb::state::decodeOutputState(b1.data(), b1.size(), d));
        REQUIRE(d.loudnessMode == lm[i]);
        REQUIRE(d.centerSlotPolicy == cp[i]);
        std::vector<std::uint8_t> b2;
        REQUIRE(scvb::state::encodeOutputState(d, b2));
        REQUIRE(b1 == b2);
    }
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

TEST_CASE("OutputStateCodec:[J69/U24] 未知序号回落默认并计数", "[output][state]")
{
    scvb::state::OutputState s;
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    REQUIRE(b.size() == 34u); // 24 头 + "en" 2 + 2×u32
    auto put = [&](std::size_t off, std::uint32_t v) {
        b[off] = static_cast<std::uint8_t>(v & 0xFF);
        b[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        b[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        b[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    };
    put(26, 99); // loudness 越界
    put(30, 7); // center 越界
    scvb::state::OutputState d;
    scvb::state::OutputDecodeReport r;
    REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
    REQUIRE(d.loudnessMode == "kw_integrated"); // 回落默认
    REQUIRE(d.centerSlotPolicy == "priority_queue");
    REQUIRE(r.loudnessModeFallbacks == 1);
    REQUIRE(r.centerSlotPolicyFallbacks == 1);
}

TEST_CASE("OutputStateCodec:旧版 payload(无枚举字段)回落默认且不计数", "[output][state]")
{
    scvb::state::OutputState s;
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    b.resize(b.size() - 8); // 去掉末尾 2 个枚举 u32 → 旧版 24+langBytes
    scvb::state::OutputState d;
    scvb::state::OutputDecodeReport r;
    REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
    REQUIRE(d.loudnessMode == "kw_integrated");
    REQUIRE(d.centerSlotPolicy == "priority_queue");
    REQUIRE(r.loudnessModeFallbacks == 0);
    REQUIRE(r.centerSlotPolicyFallbacks == 0);
}

TEST_CASE("OutputStateCodec:kw_integrated roundtrip → parseLoudnessMode 解析成功", "[output][state]")
{
    // 复评重要①:落盘/契约桥面真值是 kw_integrated(SCVB_CONTRACT §1.21/§9.2),analysis 层
    // parseLoudnessMode 必须认它,否则解析不了 state 层写回的默认档(k_integrated 保留兼容)。
    scvb::state::OutputState s; // 默认 loudnessMode = "kw_integrated"
    REQUIRE(s.loudnessMode == "kw_integrated");
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b)); // 落盘
    scvb::state::OutputState d;
    REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d)); // 读回
    REQUIRE(d.loudnessMode == "kw_integrated");
    const auto parsed = scvb::analysis::parseLoudnessMode(d.loudnessMode.c_str()); // 解析
    REQUIRE(!parsed.fellBack);
    REQUIRE(parsed.mode == scvb::analysis::LoudnessMode::KIntegrated);
}

TEST_CASE("OutputStateCodec:枚举字段截断(0<remaining<8)→ 拒载", "[output][state]")
{
    scvb::state::OutputState s;
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b)); // 34 字节 = 24 头 + "en" 2 + 2×u32
    b.pop_back(); // 砍掉 1 字节 → remaining = 7,落在 (0,8) → 拒载
    scvb::state::OutputState d;
    REQUIRE_FALSE(scvb::state::decodeOutputState(b.data(), b.size(), d));
}

TEST_CASE("OutputStateCodec:unknownTail 解码保留 + 编码原样回写", "[output][state]")
{
    scvb::state::OutputState s;
    s.loudnessMode = "rms";
    s.centerSlotPolicy = "lead_exclusive";
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    // 模拟未来小版本追加:已知字段之后追加 4 字节未知尾部。
    b.push_back(0xDE);
    b.push_back(0xAD);
    b.push_back(0xBE);
    b.push_back(0xEF);
    scvb::state::OutputState d;
    REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d));
    REQUIRE(d.loudnessMode == "rms");
    REQUIRE(d.centerSlotPolicy == "lead_exclusive");
    REQUIRE(d.unknownTail.size() == 4u); // 未知尾部保留
    std::vector<std::uint8_t> b2;
    REQUIRE(scvb::state::encodeOutputState(d, b2));
    REQUIRE(b == b2); // 逐字节回写
}

TEST_CASE("OutputStateCodec:非 en 的 uiLanguage 偏移(base=24+langBytes)推导", "[output][state]")
{
    scvb::state::OutputState s;
    s.uiLanguage = "zh-CN"; // 5 字节(非默认 "en" 2 字节),验证 base=24+langBytes 推导
    s.loudnessMode = "rms";
    s.centerSlotPolicy = "lead_exclusive";
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    REQUIRE(b.size() == 24u + 5u + 8u); // 24 头 + 5 语言 + 2×u32
    scvb::state::OutputState d;
    REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d));
    REQUIRE(d.uiLanguage == "zh-CN");
    REQUIRE(d.loudnessMode == "rms");
    REQUIRE(d.centerSlotPolicy == "lead_exclusive");
    std::vector<std::uint8_t> b2;
    REQUIRE(scvb::state::encodeOutputState(d, b2));
    REQUIRE(b == b2); // 逐字节一致
}

TEST_CASE("Output state 容器:旧版读新 CFGS(高 abi)→ RejectedNewer + 原样回写", "[output][state][abi]")
{
    // 复评重要②:旧版(abi=1)读到含 loudness_mode/center_slot_policy 的新(abi=2)blob → RejectedNewer
    // + preservedOriginal 原样回写,绝不把用户 CFGS 覆盖成默认(CLAUDE.md §7.3 / STATE_SCHEMA)。
    // 模拟「旧版读新」:当前 kCurrentAbi=2,把容器 abi 抬到 kCurrentAbi+1 代表未来/更高版本。
    scvb::state::OutputState s;
    s.groupId = 5;
    s.loudnessMode = "peak_dbfs";
    s.centerSlotPolicy = "even_spread";
    std::vector<std::uint8_t> cfg;
    REQUIRE(scvb::state::encodeOutputState(s, cfg));

    scvb::state::StateChunks chunks;
    chunks.abi = scvb::state::kCurrentAbi + 1; // 未来 abi(旧版读新)
    chunks.set(scvb::state::kFourccCfgs, cfg);
    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeContainer(chunks, enc));

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

// ---------------------------------------------------------------------------
// T37 三轮 A 族回归:失准计数必须能自行撤下。
// 真机症状:「误 bypass 一个 Input → Output 正确报失准;重开 Input、音频链路恢复正常,
// 但 Output 的失准警告一直不消失」。根因是上桥的 misalignCount 直接用了进程寿命累计的
// gapCount —— 只增不减,恢复健康也撤不下横幅。
// ---------------------------------------------------------------------------

TEST_CASE("misalignCountRecent:失准发作后恢复健康 → 归零(T37 三轮 A 族)", "[output][session][t37]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession in(backend, 1001);
    in.setChannelId(3);
    REQUIRE(in.prepare(48000, 512, 1, 1000) == InputClaimState::kActive);
    in.heartbeat(1100);

    float buf[32] = {};
    scvb::AudioRing::write(in.audioRing().acquire(), 0, buf, 32); // write_head = 32

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1200) == OutputClaimState::kActive);
    out.tick(1300);

    auto& src = out.mixSource(3);
    REQUIRE(src.bound());

    // ① 先成功读一次(primed);冷启动追赶期不计失准,故此前计数必须是 0。
    std::vector<float> dst(32, 0.0f);
    REQUIRE(src.read(0, dst.data(), 16));
    REQUIRE(out.gapCount(3) == 0);

    // 走带在跑:停流判定的前提(走带停住时写头本来就该冻着,那不是故障)。
    out.setTransportPlaying(true);

    // ② 读位置越过写头(等价于 Input 被 bypass、不再推进 write_head)。
    //    这是**写方停滞**,不是失准 —— gapCount 不动,只留饿读痕迹。
    REQUIRE_FALSE(src.read(32, dst.data(), 16));
    REQUIRE(out.gapCount(3) == 0);

    // ③ 短停(<kSuspendStallMs = 500ms)零信号:宿主在 -inf 段挂起 Input 又很快恢复,
    //    不该闪一次「失准」再自愈(v5 实测 P1-7)。
    out.tick(1400);
    CHECK(out.misalignCountRecent(3) == 0);

    // ④ 停滞坐实(写头自 1300 起没动过,已 >500ms)→ 记一笔停流,横幅亮起。
    in.heartbeat(1900);
    out.tick(1900);
    CHECK(out.misalignCountRecent(3) == 1);

    // ⑤ **只是心跳还在、不再产生新的缺口/饿读,不算恢复**(v4 实测 P0-2 的假恢复):
    //    Input 被 bypass 后本轨转 suspended 退出注入集,read() 不再被调用,痕迹自然停止增长 ——
    //    此时若判恢复,横幅会在实际仍无声时撤下。写头没动,警告必须保持。
    in.heartbeat(3200);
    out.tick(3200);
    CHECK(out.misalignCountRecent(3) == 1);

    // ⑥ 数据真的恢复推进(写头前移)→ 发作结束。撤警还要再满足两条:距上次发作 >1s
    //    (kMisalignRecoverMs)且**此刻**数据仍在推进(dataAdvancing,<500ms 内有新帧)——
    //    所以这里再写一次,而不是干等。
    scvb::AudioRing::write(in.audioRing().acquire(), 32, buf, 32); // write_head = 64
    in.heartbeat(3300);
    out.tick(3300); // 本拍记下写头前移 → 发作结束
    CHECK(out.misalignCountRecent(3) == 1); // 恢复窗未过,警告仍在(不抢跑)

    scvb::AudioRing::write(in.audioRing().acquire(), 64, buf, 32); // write_head = 96
    in.heartbeat(4300);
    out.tick(4300); // 距上次发作 1.1s > 1s,且数据正在推进 → 撤警
    CHECK(out.misalignCountRecent(3) == 0);

    // ⑦ 再次停流 → 重新报数(不是一次性静音)。
    REQUIRE_FALSE(src.read(128, dst.data(), 16));
    in.heartbeat(4900);
    out.tick(4900); // 写头自 4300 起没动过,已 >500ms → 第二次发作
    CHECK(out.misalignCountRecent(3) == 1);
}

TEST_CASE("T37-C 命令环 kSetPriority 派发到 Output(不再静默丢弃)", "[output][session][ctrl][t37]")
{
    // 真机症状:Input 拖优先级滑杆 → remoteSetPriority → 命令环 → Output 排空丢弃 → 值永不生效。
    // consumeCommands 此前的循环体是空的;本例断言记录确实被派发出来。
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1000) == OutputClaimState::kActive);

    // Input 侧生产一条 kSetPriority(与 InputProcessor::bridgeRemoteSetPriority 同款调用)。
    scvb::CtrlPlane input(backend, 1);
    REQUIRE(input.open() == scvb::InitResult::kOk);
    REQUIRE(input.enqueue(4, scvb::CtrlOp::kSetPriority, 7));

    scvb::u32 value = 0;
    CHECK_FALSE(out.takeRemotePriority(4, value)); // tick 之前没有待应用值

    out.tick(1100); // consumeCommands 在 tick 内
    REQUIRE(out.takeRemotePriority(4, value));
    CHECK(value == 7);

    // 取走即消费:不重复应用。
    CHECK_FALSE(out.takeRemotePriority(4, value));

    // 越界值钳到 0..10(Input 侧已 clamp,这里是读方自保)。
    REQUIRE(input.enqueue(4, scvb::CtrlOp::kSetPriority, 999));
    out.tick(1200);
    REQUIRE(out.takeRemotePriority(4, value));
    CHECK(value == 10);

    // 同一拍多条只留最后一条(值语义,不是增量)。
    REQUIRE(input.enqueue(9, scvb::CtrlOp::kSetPriority, 2));
    REQUIRE(input.enqueue(9, scvb::CtrlOp::kSetPriority, 5));
    out.tick(1300);
    REQUIRE(out.takeRemotePriority(9, value));
    CHECK(value == 5);

    // 未知 op 不得中断排空,也不得产生待应用值。
    REQUIRE(input.enqueue(6, static_cast<scvb::CtrlOp>(0xFE), 3));
    out.tick(1400);
    CHECK_FALSE(out.takeRemotePriority(6, value));
}

TEST_CASE("T37-A 特征拉取端到端:Input 写 feat 段 → Output FrameStore 记账覆盖", "[output][session][feat][t37]")
{
    // 真机症状 L-6:采集开着、播放一段后,分析区显示「当前范围内无采集数据」「已分析区域共 0 段」,
    // 分析按钮点不了。根因是 Output 侧完全没接 feat 读侧 —— FeatPuller/FrameStore 只在 tests/ 里
    // 出现过,Input 兢兢业业写了一路特征,对面没人开那个段。
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession in(backend, 1001);
    in.setChannelId(2);
    REQUIRE(in.prepare(48000, 512, 1, 1000) == InputClaimState::kActive);
    in.heartbeat(1100);
    REQUIRE(in.featRing().bound());

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1200) == OutputClaimState::kActive);
    out.setCaptureEnabled(true); // 采集闸:ChannelFrames 默认 readOnly,不开闸拉了也不记账

    // 让该轨过 [J32] 注入延迟并进 connected_mask —— pullTick 的 activeMask 只拉在线轨。
    float audio[64] = {};
    scvb::AudioRing::write(in.audioRing().acquire(), 0, audio, 64);
    out.tick(1300);
    out.tick(1600);

    // Input 侧产特征:startRun 定位时间线原点,再喂满若干 hop(10ms @48k = 480 样本/hop)。
    in.featRing().setCapturing(true);
    in.featRing().startRun(0);
    std::vector<float> mono(480 * 20, 0.25f); // 20 hop 的非静音信号
    const float* planar[1] = {mono.data()};
    const int wrote = in.featRing().processBlock(planar, static_cast<int>(mono.size()));
    REQUIRE(wrote > 0);

    // Output 拉取(在 tick 内)。
    out.tick(1700);

    const auto& frames = out.frameStore().channel(2);
    const std::uint64_t covered = frames.coveredHops(scvb::analysis::HopRange{0, static_cast<std::uint64_t>(wrote)});
    CHECK(covered == static_cast<std::uint64_t>(wrote)); // ← 修复前恒为 0
    CHECK(frames.coversFully(scvb::analysis::HopRange{0, 1}));

    // 采集关 → 回只读,已记的覆盖不丢(ADR-007)。
    out.setCaptureEnabled(false);
    out.tick(1800);
    CHECK(frames.coveredHops(scvb::analysis::HopRange{0, static_cast<std::uint64_t>(wrote)}) == covered);
}
