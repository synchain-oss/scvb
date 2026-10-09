// SPDX-License-Identifier: GPL-3.0-or-later
// test_output_session —— OutputSession 生命周期单测(claim/observer/[J32] 200ms 注入延迟/
// [J66] 改组)+ OutputStateCodec 往返。用 SegmentBackendInProcess 模拟多实例(进程内,不碰全局段)。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "analysis/LoudnessMode.h"
#include "input/InputSession.h"
#include "input/OutputStage.h"
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

// [SL-210] 上面那条用的是**两个不同 pid**,而真实 DAW 里同一宿主进程的两个 Output 实例
// pid 是同一个 —— 正是这个盲点让「同 pid 重认领」分支把第二个实例也放成了 kActive。
TEST_CASE("[SL-210] 同进程(同 pid)第二个 Output → observer,不是第二个主实例", "[output][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    constexpr u32 kHostPid = 4242; // 同一个 DAW 进程

    OutputSession a(backend, kHostPid);
    REQUIRE(a.prepare(48000, 512, 1000) == OutputClaimState::kActive);

    OutputSession b(backend, kHostPid); // 同 bus 插入的第二个 Output:同组、同 pid
    REQUIRE(b.prepare(48000, 512, 1100) == OutputClaimState::kObserver);
    b.tick(1200);
    REQUIRE(b.state() == OutputClaimState::kObserver);

    // 主实例不受影响:仍是 slot 属主(observer 既没抢走 slot,也没把 pid 改写成自己)。
    REQUIRE(a.state() == OutputClaimState::kActive);
    scvb::Registry probe(backend, 1);
    REQUIRE(probe.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(probe.outputSlot()->state.load() == kSlotActive);
    REQUIRE(probe.outputSlot()->pid == kHostPid);

    // observer 不得注入:injectMask 恒 0 → processBlock 走直通,不替换总线。
    REQUIRE(b.injectMask() == 0);
}

// 反向验证①:同一个实例重新 prepare(采样率切换 / 宿主重开)必须仍拿到 kActive ——
// ownsOutput_ 这一条不能把正当续期一起挡掉。
TEST_CASE("[SL-210] 反向:同实例重 prepare(采样率切换)仍 kActive", "[output][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    constexpr u32 kHostPid = 4242;
    OutputSession a(backend, kHostPid);
    REQUIRE(a.prepare(48000, 512, 1000) == OutputClaimState::kActive);
    // 采样率切换 → 宿主再调一次 prepareToPlay。
    REQUIRE(a.prepare(44100, 256, 1100) == OutputClaimState::kActive);
    REQUIRE(a.prepare(96000, 1024, 1200) == OutputClaimState::kActive);
    REQUIRE(a.state() == OutputClaimState::kActive);
}

// 反向验证②:删掉 observer 不能动主实例的 slot(旧 releaseOutput 只比 pid,同 pid 的
// observer 一析构就会把主实例的 slot 释放掉)。
TEST_CASE("[SL-210] 反向:observer 析构不释放主实例 slot", "[output][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    constexpr u32 kHostPid = 4242;
    OutputSession a(backend, kHostPid);
    REQUIRE(a.prepare(48000, 512, 1000) == OutputClaimState::kActive);

    {
        OutputSession b(backend, kHostPid);
        REQUIRE(b.prepare(48000, 512, 1100) == OutputClaimState::kObserver);
    } // b 析构:releaseSlot + Registry::releaseOwnedSlot 都不该碰 slot

    scvb::Registry probe(backend, 1);
    REQUIRE(probe.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(probe.outputSlot()->state.load() == kSlotActive); // 仍活跃,没被释放成 kSlotFree
    REQUIRE(probe.outputSlot()->pid == kHostPid);
    REQUIRE(a.state() == OutputClaimState::kActive);
}

// 反向验证③:主实例走掉后,同 pid 的 observer 必须能在 25Hz tick 上接管(否则这条修复
// 会把「删掉第一个 Output」变成整组失去主实例)。
TEST_CASE("[SL-210] 反向:主实例释放后 observer 接管", "[output][session]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    constexpr u32 kHostPid = 4242;
    OutputSession b(backend, kHostPid);
    {
        OutputSession a(backend, kHostPid);
        REQUIRE(a.prepare(48000, 512, 1000) == OutputClaimState::kActive);
        REQUIRE(b.prepare(48000, 512, 1100) == OutputClaimState::kObserver);
    } // a 析构 → slot 归还

    b.tick(1200); // observer 的 25Hz 重试 claim
    REQUIRE(b.state() == OutputClaimState::kActive);
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
    REQUIRE(s.outputEnabled == 1); // 值对象的缺省;新插实例的初值不在这里([J169] 为关,见 OutputProcessor.h)

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
// [SL-367] 「已连接」判据的真值表。这条判据此前在仓里有**三份手抄件**(桥面
// `heartbeatFresh`、web 的 `connectedChannels`、SL-361 给 VizPublisher 加连接闸时抄的第三份),
// **三份之间没有任何门禁对拍** —— 谁改了一份另外两份不会红。抽成函数之后 C++ 两处共用,
// 这一格是那三份手抄件里的**第一道机检**。
//
// ⚠ 本格钉的是**判据本身**,钉不住「调用方真的调了它」。后者在 C++ 里没有便宜的机检手段;
// 已在 PR 描述里写明,不假装钉住了。
TEST_CASE("isConnectedForDisplay:两条都满足才算已连接", "[output][conn][sl367]")
{
    using scvb::output::ChannelConnInfo;
    using scvb::output::isConnectedForDisplay;
    using scvb::output::isHeartbeatFreshForDisplay;

    const auto make = [](scvb::u32 state, scvb::u32 age) {
        ChannelConnInfo i;
        i.slotState = state;
        i.heartbeatAgeMs = age;
        return i;
    };

    // 活跃 + 心跳新鲜 ⇒ 已连接。
    REQUIRE(isConnectedForDisplay(make(scvb::kSlotActive, 0)));
    REQUIRE(isConnectedForDisplay(make(scvb::kSlotActive, static_cast<scvb::u32>(scvb::kStaleDisplayMs))));
    // 边界:恰好超一毫秒即失联(阈值是 ≤,不是 <)。
    REQUIRE_FALSE(isConnectedForDisplay(make(scvb::kSlotActive, static_cast<scvb::u32>(scvb::kStaleDisplayMs) + 1)));
    // 槽位不活跃 ⇒ 不算,哪怕心跳是新的 —— **只钉后半的话这一格会漏**,而 SL-361 的
    // 回落闸正是靠前半挡住「enabled 但未连接」的轨(默认 enabled 全 true)。
    REQUIRE_FALSE(isConnectedForDisplay(make(scvb::kSlotFree, 0)));
    REQUIRE_FALSE(isConnectedForDisplay(make(scvb::kSlotClaimed, 0)));
    // 哨兵年龄(从未心跳)自然为假 —— 不需要额外分支,靠 0xFFFFFFFF > 2000 就成立。
    REQUIRE_FALSE(isConnectedForDisplay(make(scvb::kSlotActive, scvb::output::kHeartbeatAgeUnknown)));

    // 前半单独可用(桥面 §2.3 只发这一半):它**不看槽位**。
    REQUIRE(isHeartbeatFreshForDisplay(make(scvb::kSlotFree, 0)));
    REQUIRE_FALSE(isHeartbeatFreshForDisplay(make(scvb::kSlotActive, scvb::output::kHeartbeatAgeUnknown)));
    // 两者的关系:已连接 ⇒ 心跳新鲜(反之不成立)。
    REQUIRE(isConnectedForDisplay(make(scvb::kSlotActive, 10)) == true);
    REQUIRE(isHeartbeatFreshForDisplay(make(scvb::kSlotActive, 10)) == true);
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
    // [rc-misc a] scvb.error{srMismatch} 的 detail.inputSr 取这一位(§5.1)。
    CHECK(ch5.inputSampleRate == 44100u);
    // 空闲槽不报采样率不一致(sample_rate=0 是「未知」,不是「不同」)。
    REQUIRE_FALSE(out.channelConn(6, 1300).srMismatch);
    CHECK(out.channelConn(6, 1300).inputSampleRate == 0u);
}

TEST_CASE("OutputStateCodec:[J69/U24] 未知序号回落默认并计数", "[output][state]")
{
    scvb::state::OutputState s;
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    REQUIRE(b.size() ==
            78u + 1860u); // 24 头 + "en" 2 + 13×u32(当前 2 + applied 2 + [SL-411] seg 3* + [SL-416] vad/ramp 6*)
    // + [SL-472] channels[15] 一整档 15×124 = 1860
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
    // [SL-279] 砍 16 而不是 8:尾部现在是**多级**(当前 2×u32 + applied 2×u32 + [SL-411] segmentation),
    // 「abi=1 的旧版」= 一档都没有。只砍 8 得到的是 abi=2、只砍 20 得到的是 abi=3,那是下面另两格。
    // [SL-411] 起总尾长 28 字节,[SL-416] 起 52 字节(28 + vad/ramp 那一整档 24),故这里砍 52。
    // [SL-472] 起再 + channels 一整档 1860,故再多砍 1860。
    b.resize(b.size() - 52 - 1860); // 去掉末尾整条尾巴 → 旧版 24+langBytes
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
    REQUIRE(scvb::state::encodeOutputState(s, b)); // 54 字节 = 24 头 + "en" 2 + 7×u32(三级尾部)
    // [SL-411] 按 **base+7** 显式截,而不是「砍掉一个固定字节数」——尾部级数还会再长,
    // 而本格要的形态逐字是「第一档只剩 7 字节」,不写清就迟早砍到别的档上去。
    b.resize(24u + 2u + 7u); // remaining = 7,落在 (0,8) → 拒载
    scvb::state::OutputState d;
    REQUIRE_FALSE(scvb::state::decodeOutputState(b.data(), b.size(), d));
}

TEST_CASE("OutputStateCodec:[SL-279] applied.* 往返 + 与当前值互不串", "[output][state][sl279]")
{
    scvb::state::OutputState s;
    s.loudnessMode = "rms";
    s.centerSlotPolicy = "lead_exclusive";
    s.appliedLoudnessMode = "peak_dbfs";
    s.appliedCenterSlotPolicy = "even_spread";
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    scvb::state::OutputState d;
    scvb::state::OutputDecodeReport r;
    REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
    // 四个字段各走各的 —— 写反顺序或共用一个槽,这四条里至少一条会红。
    REQUIRE(d.loudnessMode == "rms");
    REQUIRE(d.centerSlotPolicy == "lead_exclusive");
    REQUIRE(d.appliedLoudnessMode == "peak_dbfs");
    REQUIRE(d.appliedCenterSlotPolicy == "even_spread");
    REQUIRE(r.appliedLoudnessModeFallbacks == 0);
    REQUIRE(r.appliedCenterSlotPolicyFallbacks == 0);
    std::vector<std::uint8_t> b2;
    REQUIRE(scvb::state::encodeOutputState(d, b2));
    REQUIRE(b == b2); // 逐字节往返
}

TEST_CASE("OutputStateCodec:[SL-279] abi=2 旧 payload ⇒ applied := 当前值(不是默认值)", "[output][state][sl279]")
{
    // 这一格钉的是本卡的产品取舍:旧工程视为「已经按它存着的那档分析过」。
    // 回落默认会让一个存了非默认档的工程一打开就报「需重新分析」—— 那正是 SL-279 要修的误报。
    scvb::state::OutputState s;
    s.loudnessMode = "rms";
    s.centerSlotPolicy = "even_spread";
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    // [SL-411] 砍 20 = applied 那两个 u32(8)+ segmentation 那一整档(12);「abi=2 的形态」=
    // 尾部到「当前」那两个 u32 为止。[SL-416] 起还要再砍 vad/ramp 那一整档(24)⇒ 共 44。
    b.resize(b.size() - 44 - 1860); // → abi=2 形态([SL-472] 起再多砍 channels 一整档 1860)
    scvb::state::OutputState d;
    scvb::state::OutputDecodeReport r;
    REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
    REQUIRE(d.loudnessMode == "rms");
    REQUIRE(d.centerSlotPolicy == "even_spread");
    REQUIRE(d.appliedLoudnessMode == "rms"); // ← 取当前值;回落默认时这条红
    REQUIRE(d.appliedCenterSlotPolicy == "even_spread"); // ← 同上
    REQUIRE(r.appliedLoudnessModeFallbacks == 0); // 缺席不算回落
    REQUIRE(r.appliedCenterSlotPolicyFallbacks == 0);
}

TEST_CASE("OutputStateCodec:[SL-279] applied 字段截断(8<remaining<16)→ 拒载", "[output][state][sl279]")
{
    scvb::state::OutputState s;
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    b.resize(24u + 2u + 15u); // remaining = 15,落在 (8,16) → 半截 applied,拒载
    scvb::state::OutputState d;
    REQUIRE_FALSE(scvb::state::decodeOutputState(b.data(), b.size(), d));
}

TEST_CASE("OutputStateCodec:[SL-279] applied 越界序号回落默认并**单独**计数", "[output][state][sl279]")
{
    scvb::state::OutputState s;
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    // applied 两个 u32 在 base+8 / base+12([SL-411] 起它们后面还跟着 segmentation 一整档,
    // 所以**不能再从尾巴倒着数** —— 那是这份测试在这次升格前最脆的一处)。
    const std::size_t appliedAt = 24u + 2u + 8u;
    b[appliedAt] = 9;
    b[appliedAt + 4] = 9;
    scvb::state::OutputState d;
    scvb::state::OutputDecodeReport r;
    REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
    REQUIRE(d.appliedLoudnessMode == "kw_integrated");
    REQUIRE(d.appliedCenterSlotPolicy == "priority_queue");
    REQUIRE(r.appliedLoudnessModeFallbacks == 1);
    REQUIRE(r.appliedCenterSlotPolicyFallbacks == 1);
    // **不与「当前」那两个计数器合并** —— 合并之后诊断行会把人指到错的字段上。
    REQUIRE(r.loudnessModeFallbacks == 0);
    REQUIRE(r.centerSlotPolicyFallbacks == 0);
}

// ============================================================================
// [SL-411] analysis.segmentation 三项随工程落盘(CFGS 尾扩 12 字节,abi 3→4)
//
// 契约面:docs/STATE_SCHEMA.md §一/§三、docs/contract-changes/20260914-sl411-segmentation-persist.md。
// 本组四格把**搬运层**钉死(值往返 / 旧档缺席 / 越界回落 / 半截拒载);「保存路径真的把 runtime_
// 写进去了」与「加载路径真的恢复了」这两跳在 tests/host 的 `HOST SL411`(编辑器那一跳离线不可达,
// 缺口登记在那条用例的头注里)。
// ============================================================================

TEST_CASE("OutputStateCodec:[SL-411] segmentation 三项往返 + 与前面几档互不串", "[output][state][sl411]")
{
    scvb::state::OutputState s;
    s.segmentationMode = "vad_only";
    s.segmentationSensitivity = 37.5f;
    s.segmentationMinSegmentMs = 1000u;
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    REQUIRE(b.size() == 24u + 2u + 52u + 1860u); // [SL-472] + channels 1860
    // 24 头 + "en" 2 + 13×u32(当前 2 + applied 2 + seg 3* + vad/ramp 6*)

    scvb::state::OutputState d;
    scvb::state::OutputDecodeReport r;
    REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
    REQUIRE(d.segmentationMode == "vad_only");
    REQUIRE(d.segmentationSensitivity == 37.5f);
    REQUIRE(d.segmentationMinSegmentMs == 1000u);
    // 三项之外的字段一个都不许被这一档搅动(写反顺序 / 共用一个槽,这几条里至少一条会红)。
    REQUIRE(d.loudnessMode == "kw_integrated");
    REQUIRE(d.centerSlotPolicy == "priority_queue");
    REQUIRE(d.appliedLoudnessMode == "kw_integrated");
    REQUIRE(d.appliedCenterSlotPolicy == "priority_queue");
    REQUIRE(r.segmentationModeFallbacks == 0);
    REQUIRE(r.segmentationSensitivityFallbacks == 0);
    REQUIRE(r.segmentationMinSegmentMsFallbacks == 0);

    std::vector<std::uint8_t> b2;
    REQUIRE(scvb::state::encodeOutputState(d, b2));
    REQUIRE(b == b2); // 逐字节往返(f32 走位模式落盘,所以 37.5 必须一位不差地回来)
}

TEST_CASE("OutputStateCodec:[SL-411] abi=3 旧 payload(无 seg 档)⇒ 三默认且不计回落", "[output][state][sl411]")
{
    // 这一格钉的是与 [SL-279] `applied := 当前值` **相反**的那个取舍:segmentation 的语义是
    // 「当前设置」本身,旧工程确实没存过 → 取规格默认(valley/50/120,也正是旧构建 runtime_ 的初值),
    // 且**不计回落** —— 缺席不是「值不可信」,把它记成回落会让诊断行凭空多出三行噪声。
    scvb::state::OutputState s;
    s.segmentationMode = "vad_only"; // 先写成非默认,好证明下面读到的默认不是「本来就没写」
    s.segmentationSensitivity = 12.5f;
    s.segmentationMinSegmentMs = 900u;
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    b.resize(b.size() - 36 -
             1860); // 砍掉 segmentation(12)+ vad/ramp(24)+ [SL-472] channels(1860)三整档 → abi=3 形态(尾长 16)
    scvb::state::OutputState d;
    scvb::state::OutputDecodeReport r;
    REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
    REQUIRE(d.segmentationMode == "valley");
    REQUIRE(d.segmentationSensitivity == 50.0f);
    REQUIRE(d.segmentationMinSegmentMs == 120u);
    REQUIRE(r.segmentationModeFallbacks == 0); // 缺席不算回落
    REQUIRE(r.segmentationSensitivityFallbacks == 0);
    REQUIRE(r.segmentationMinSegmentMsFallbacks == 0);
}

TEST_CASE("OutputStateCodec:[SL-411] segmentation 越界 ⇒ 各字段**单独**回落默认并计数", "[output][state][sl411]")
{
    // 三个字段各自越界一次,计数器必须**分别**加一(合并计数会让诊断行说「segmentation 回落了 1 次」
    // 而实际三个字段全回落了 —— 与 [SL-279] applied 那两个不合并是同一条理由)。
    const auto putU32At = [](std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) {
        v[off] = static_cast<std::uint8_t>(x & 0xFF);
        v[off + 1] = static_cast<std::uint8_t>((x >> 8) & 0xFF);
        v[off + 2] = static_cast<std::uint8_t>((x >> 16) & 0xFF);
        v[off + 3] = static_cast<std::uint8_t>((x >> 24) & 0xFF);
    };
    // 先钉「在场且合法」的那一版,避免下面几条实际上打在缺席档上(那样它们会因别的原因变绿)。
    {
        scvb::state::OutputState s;
        s.segmentationMode = "vad_only";
        s.segmentationSensitivity = 100.0f; // 上界本身合法
        s.segmentationMinSegmentMs = 2000u; // 上界本身合法
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
        REQUIRE(d.segmentationMode == "vad_only");
        REQUIRE(d.segmentationSensitivity == 100.0f);
        REQUIRE(d.segmentationMinSegmentMs == 2000u);
        REQUIRE(r.segmentationModeFallbacks == 0);
        REQUIRE(r.segmentationSensitivityFallbacks == 0);
        REQUIRE(r.segmentationMinSegmentMsFallbacks == 0);
    }
    // ① mode 越界(序号 7 不是白名单里的两档)→ valley + 计一次
    {
        scvb::state::OutputState s;
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        putU32At(b, 42u, 7u); // base(26) + 16 = segmentationMode
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
        REQUIRE(d.segmentationMode == "valley");
        REQUIRE(r.segmentationModeFallbacks == 1);
        REQUIRE(r.segmentationSensitivityFallbacks == 0);
        REQUIRE(r.segmentationMinSegmentMsFallbacks == 0);
    }
    // ② 灵敏度 **NaN** → 50 + 计一次。NaN 是这一档唯一必须单独守的形态:`x < lo || x > hi`
    //    对 NaN 恒假,只写范围比较的实现在这里会**静默放行**,而 NaN 一旦进了 PipelineConfig
    //    的灵敏度,下游所有比较都是假 —— 那种坏法不报错、只是结果不对。
    {
        scvb::state::OutputState s;
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        putU32At(b, 46u, 0x7FC00000u); // base(26) + 20 = segmentationSensitivity(quiet NaN)
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
        REQUIRE(d.segmentationSensitivity == 50.0f);
        REQUIRE(r.segmentationSensitivityFallbacks == 1);
        REQUIRE(r.segmentationModeFallbacks == 0);
    }
    // ③ 灵敏度 1e9(有限但越界)→ 同样回落默认,**不夹到 100**
    {
        scvb::state::OutputState s;
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        putU32At(b, 46u, 0x4E6E6B28u); // 1.0e9f 的 IEEE-754 位模式
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
        REQUIRE(d.segmentationSensitivity == 50.0f); // ← 夹到 100 的实现会让这条红(口径见头注)
        REQUIRE(r.segmentationSensitivityFallbacks == 1);
    }
    // ④ min_segment_ms 越界(下限 49 / 上限 2001 / 0 哨兵)三个值都要回落 120
    for (const std::uint32_t bad : {0u, 49u, 2001u, 0xFFFFFFFFu})
    {
        scvb::state::OutputState s;
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        putU32At(b, 50u, bad); // base(26) + 24 = segmentationMinSegmentMs
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
        INFO("bad min_segment_ms = " << bad);
        REQUIRE(d.segmentationMinSegmentMs == 120u);
        REQUIRE(r.segmentationMinSegmentMsFallbacks == 1);
        REQUIRE(r.segmentationSensitivityFallbacks == 0);
    }
}

TEST_CASE("OutputStateCodec:[SL-411] segmentation 半截(16<remaining<28)→ 拒载", "[output][state][sl411]")
{
    // 一整档 12 字节是同一个 commit 写下去的,「只有前 8 个字节」不可能是任何真实构建的产物。
    scvb::state::OutputState s;
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    b.resize(24u + 2u + 16u + 8u); // remaining = 24,落在 (16,28) → 拒载
    scvb::state::OutputState d;
    REQUIRE_FALSE(scvb::state::decodeOutputState(b.data(), b.size(), d));
}

// ============================================================================
// [SL-416] analysis.vad 五字段 + analysis.transition_ramp_ms 随工程落盘(CFGS 尾扩 24 字节,abi 4→5)
//
// 契约面:docs/STATE_SCHEMA.md §一/§三、docs/contract-changes/20260914-sl416-vad-persist.md;
// 值域与默认值的真源 = masterPlan 02 §0.3 常量表(经 U24 收敛),C++ 侧单一真源 =
// `OutputStateCodec.h` 的 `kOutputVad*` / `kOutputTransitionRampMs*`。
// 本组四格把**搬运层**钉死(往返 / abi≤4 旧档六默认不计回落 / 越界回落**逐字段**计数 / 半截拒载);
// 「保存路径真的把 runtime_ 写进去了」与「加载路径真的恢复了、且重开后的 VAD 真按持久值跑」这两跳
// 在 tests/host 的 `HOST SL416`(编辑器那一跳离线不可达,缺口登记在那条用例的头注里)。
// ⚠ 本组**故意用字面量**钉规格值(−38/6/250/120/200/80、3..12、100..600…):断言取自被测常量会变成
// 恒真,字面量才把规格独立钉住一遍(与 SL-411 那一组同一条纪律)。
// ============================================================================

TEST_CASE("OutputStateCodec:[SL-416] vad 五字段 + ramp 往返 + 与前面几档互不串", "[output][state][sl416]")
{
    scvb::state::OutputState s;
    s.vadThresholdDb = -52.5f;
    s.vadHysteresisDb = 9.0f;
    s.vadHangoverMs = 330u;
    s.vadPaddingPreMs = 55u;
    s.vadPaddingPostMs = 260u;
    s.transitionRampMs = 140u;
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    REQUIRE(b.size() == 24u + 2u + 52u + 1860u); // [SL-472] + channels 1860
    // 24 头 + "en" 2 + 13×u32(13×4 = 52)

    scvb::state::OutputState d;
    scvb::state::OutputDecodeReport r;
    REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
    REQUIRE(d.vadThresholdDb == -52.5f); // f32 走位模式落盘,一位不差地回来
    REQUIRE(d.vadHysteresisDb == 9.0f);
    REQUIRE(d.vadHangoverMs == 330u);
    REQUIRE(d.vadPaddingPreMs == 55u);
    REQUIRE(d.vadPaddingPostMs == 260u);
    REQUIRE(d.transitionRampMs == 140u);
    // 六项之外的字段一个都不许被这一档搅动(写反顺序 / 共用一个槽,这几条里至少一条会红)。
    REQUIRE(d.loudnessMode == "kw_integrated");
    REQUIRE(d.centerSlotPolicy == "priority_queue");
    REQUIRE(d.appliedLoudnessMode == "kw_integrated");
    REQUIRE(d.appliedCenterSlotPolicy == "priority_queue");
    REQUIRE(d.segmentationMode == "valley");
    REQUIRE(d.segmentationSensitivity == 50.0f);
    REQUIRE(d.segmentationMinSegmentMs == 120u);
    REQUIRE(r.vadThresholdDbFallbacks == 0);
    REQUIRE(r.vadHysteresisDbFallbacks == 0);
    REQUIRE(r.vadHangoverMsFallbacks == 0);
    REQUIRE(r.vadPaddingPreMsFallbacks == 0);
    REQUIRE(r.vadPaddingPostMsFallbacks == 0);
    REQUIRE(r.transitionRampMsFallbacks == 0);

    std::vector<std::uint8_t> b2;
    REQUIRE(scvb::state::encodeOutputState(d, b2));
    REQUIRE(b == b2); // 逐字节往返
}

TEST_CASE("OutputStateCodec:[SL-416] abi=4 旧 payload(无 vad/ramp 档)⇒ 六默认且不计回落", "[output][state][sl416]")
{
    // 这一格钉的是与 [SL-411] 同一条取舍(与 [SL-279] `applied := 当前值` **相反**):vad/ramp 六项的
    // 语义就是「当前设置」本身,旧工程确实没存过 → 取规格默认(−38/6/250/120/200/80,真源 02 §0.3),
    // 且**不计回落** —— 缺席不是「值不可信」,记成回落会让诊断行凭空多出六行噪声。
    // 这一格也是 A24 的机检形态:修前用户存盘重开读到的就是这一组默认。
    scvb::state::OutputState s;
    s.vadThresholdDb = -52.0f; // 先写成非默认,好证明下面读到的默认不是「本来就没写」
    s.vadHysteresisDb = 11.0f;
    s.vadHangoverMs = 500u;
    s.vadPaddingPreMs = 300u;
    s.vadPaddingPostMs = 380u;
    s.transitionRampMs = 200u;
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    b.resize(b.size() - 24 - 1860); // 砍掉 vad/ramp 与 [SL-472] channels 两整档 → abi=4 形态(尾长 28)
    scvb::state::OutputState d;
    scvb::state::OutputDecodeReport r;
    REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
    REQUIRE(d.vadThresholdDb == -38.0f);
    REQUIRE(d.vadHysteresisDb == 6.0f);
    REQUIRE(d.vadHangoverMs == 250u);
    REQUIRE(d.vadPaddingPreMs == 120u);
    REQUIRE(d.vadPaddingPostMs == 200u);
    REQUIRE(d.transitionRampMs == 80u);
    REQUIRE(r.vadThresholdDbFallbacks == 0); // 缺席不算回落
    REQUIRE(r.vadHysteresisDbFallbacks == 0);
    REQUIRE(r.vadHangoverMsFallbacks == 0);
    REQUIRE(r.vadPaddingPreMsFallbacks == 0);
    REQUIRE(r.vadPaddingPostMsFallbacks == 0);
    REQUIRE(r.transitionRampMsFallbacks == 0);
}

TEST_CASE("OutputStateCodec:[SL-416] vad/ramp 越界 ⇒ 各字段**单独**回落默认并计数", "[output][state][sl416]")
{
    // 六个字段各自越界一次,计数器必须**分别**加一(合并计数会让诊断行说「vad 回落了 1 次」而实际六个
    // 字段全回落了 —— 与 [SL-279]/[SL-411] 那两组不合并是同一条理由)。
    const auto putU32At = [](std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) {
        v[off] = static_cast<std::uint8_t>(x & 0xFF);
        v[off + 1] = static_cast<std::uint8_t>((x >> 8) & 0xFF);
        v[off + 2] = static_cast<std::uint8_t>((x >> 16) & 0xFF);
        v[off + 3] = static_cast<std::uint8_t>((x >> 24) & 0xFF);
    };
    // 先钉「在场且合法」的那一版(含两个边界值本身合法),避免下面几条实际上打在缺席档上。
    {
        scvb::state::OutputState s;
        s.vadThresholdDb = -10.0f; // 上界本身合法(且是「最不保守」的那一端)
        s.vadHysteresisDb = 12.0f; // 上界
        s.vadHangoverMs = 600u; // 上界
        s.vadPaddingPreMs = 20u; // 下界
        s.vadPaddingPostMs = 400u; // 上界
        s.transitionRampMs = 300u; // 上界
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
        REQUIRE(d.vadThresholdDb == -10.0f);
        REQUIRE(d.vadHysteresisDb == 12.0f);
        REQUIRE(d.vadHangoverMs == 600u);
        REQUIRE(d.vadPaddingPreMs == 20u);
        REQUIRE(d.vadPaddingPostMs == 400u);
        REQUIRE(d.transitionRampMs == 300u);
        REQUIRE(r.vadThresholdDbFallbacks == 0);
        REQUIRE(r.transitionRampMsFallbacks == 0);
    }
    // ① threshold 两个越界方向 + **NaN**:NaN 是这一档必须单独守的形态(`x < lo || x > hi` 对 NaN
    //    恒假,只写范围比较的实现会**静默放行**,而 NaN 一旦进了 runtime_ → `cfg.vad.thresholdDb`,
    //    下游所有比较都是假 —— 那种坏法是静默的)。NaN 与 ±Inf 都走 `!(x >= lo && x <= hi)` 同一支。
    for (const std::uint32_t bad :
         {0xC2F00000u /* -120.0f */, 0x00000000u /* 0.0f */, 0x7FC00000u /* NaN */, 0x7F800000u /* +Inf */})
    {
        scvb::state::OutputState s;
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        putU32At(b, 54u, bad); // base(26) + 28 = vadThresholdDb
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
        INFO("bad threshold_db bits = " << bad);
        REQUIRE(d.vadThresholdDb == -38.0f); // ← 夹取到 −60 的实现会让这条红(口径:回落默认,不夹取)
        REQUIRE(r.vadThresholdDbFallbacks == 1);
        REQUIRE(r.vadHysteresisDbFallbacks == 0); // 不合并计数
    }
    // ② hysteresis 越界(下限 2.9 侧:用位模式钉 2.5f)+ 有限但超界的大值
    for (const std::uint32_t bad : {0x40200000u /* 2.5f */, 0x4E6E6B28u /* 1.0e9f */})
    {
        scvb::state::OutputState s;
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        putU32At(b, 58u, bad); // base(26) + 32 = vadHysteresisDb
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
        INFO("bad hysteresis_db bits = " << bad);
        REQUIRE(d.vadHysteresisDb == 6.0f);
        REQUIRE(r.vadHysteresisDbFallbacks == 1);
        REQUIRE(r.vadThresholdDbFallbacks == 0);
    }
    // ③ 四个 u32 字段:各自的上/下越界值(含 0 哨兵与 0xFFFFFFFF)
    struct BadU32
    {
        std::size_t off;
        std::uint32_t value;
        std::uint32_t expected;
    };
    const BadU32 badU32[] = {
        {62u, 99u, 250u}, // hangover < 100
        {62u, 601u, 250u}, // hangover > 600
        {66u, 0u, 120u}, // pad_pre < 20
        {70u, 401u, 200u}, // pad_post > 400
        {70u, 0xFFFFFFFFu, 200u}, // pad_post 哨兵
        {74u, 19u, 80u}, // ramp < 20
        {74u, 301u, 80u}, // ramp > 300
    };
    for (const auto& t : badU32)
    {
        scvb::state::OutputState s;
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        putU32At(b, t.off, t.value);
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
        INFO("bad u32 at off " << t.off << " = " << t.value);
        REQUIRE(d.vadHangoverMs == (t.off == 62u ? t.expected : 250u));
        REQUIRE(d.vadPaddingPreMs == (t.off == 66u ? t.expected : 120u));
        REQUIRE(d.vadPaddingPostMs == (t.off == 70u ? t.expected : 200u));
        REQUIRE(d.transitionRampMs == (t.off == 74u ? t.expected : 80u));
        // 计数器**逐字段独立**:被点中的那个 == 1、其余五个 == 0(与 ① ② 两组同款)。
        // 第 1 轮复审②:这段断言此前缺失 —— 标题写了「各字段单独回落默认并计数」,而 `r` 只被
        // `decodeOutputState` 填、一次都没被读,于是「把某个 `++…Fallbacks` 误写成别的字段」这种
        // 改法整套一条都不红(六个计数器的唯一消费方是 `OutputProcessor.cpp` 那条诊断 `DBG`,
        // 没有第二处兜得住)。补上之后,这类误写由本格直接照出来。
        REQUIRE(r.vadHangoverMsFallbacks == (t.off == 62u ? 1u : 0u));
        REQUIRE(r.vadPaddingPreMsFallbacks == (t.off == 66u ? 1u : 0u));
        REQUIRE(r.vadPaddingPostMsFallbacks == (t.off == 70u ? 1u : 0u));
        REQUIRE(r.transitionRampMsFallbacks == (t.off == 74u ? 1u : 0u));
        REQUIRE(r.vadThresholdDbFallbacks == 0u); // 两个 f32 字段不受 u32 越界影响
        REQUIRE(r.vadHysteresisDbFallbacks == 0u);
    }
}

TEST_CASE("OutputStateCodec:[SL-416] vad/ramp 半截(28<remaining<52)→ 拒载", "[output][state][sl416]")
{
    // 一整档 24 字节是同一个 commit 写下去的,「只有前 8 / 20 个字节」不可能是任何真实构建的产物。
    for (const std::size_t extra : {8u, 20u})
    {
        scvb::state::OutputState s;
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        b.resize(24u + 2u + 28u + extra); // remaining = 36 / 48,都落在 (28,52)
        scvb::state::OutputState d;
        INFO("truncated extra = " << extra);
        REQUIRE_FALSE(scvb::state::decodeOutputState(b.data(), b.size(), d));
    }
    // 边界本身合法:remaining = 28(abi=4 形态)与 52(完整档)都必须能解。
    {
        scvb::state::OutputState s;
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        b.resize(24u + 2u + 28u); // 恰好 abi=4 的形态
        scvb::state::OutputState d;
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d));
    }
}

// ============================================================================
// [SL-472] channels[15] 七项随工程落盘(CFGS 尾扩 15 × 124 = 1860 字节,abi 5→6)
//
// 契约面:docs/STATE_SCHEMA.md §一/§三、docs/contract-changes/20260927-sl472-channel-config-persist.md。
// 用户实测 J113:「配对、优先级、命名全部没有保存下来,主唱锁定也没有保存下来」。
// 本组把**搬运层**钉死(往返 / 编码侧截断 / abi≤5 旧档七项默认不计回落 / 非法值逐字段回落并计数 /
// 半截拒载);「保存路径真的读 runtime_」「加载路径真的写回 runtime_ 并推到广播区」两跳在 tests/host 的
// `HOST SL472`。
// ⚠ 本组**故意用字面量**钉规格值(5 / 0..10 / 0..7 / 24 码点 / 96 字节 / 记录 124 字节):断言取自被测
// 常量会变成恒真(与 SL-411/SL-416 两组同一条纪律)。
// 偏移口径("en" 语言,base = 26):channels 档起点 = 26 + 52 = 78;第 t 轨记录 = 78 + 124·t;
// 记录内 +0 enabled / +4 participate / +8 priority / +12 lead_lock / +16 lead_vol_exempt / +20 pair_id /
// +24 labelBytes / +28 label[96]。
// ============================================================================
namespace
{
constexpr std::size_t kSl472ChBase = 78u;
constexpr std::size_t kSl472Rec = 124u;

void sl472PutU32(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x)
{
    v[off] = static_cast<std::uint8_t>(x & 0xFF);
    v[off + 1] = static_cast<std::uint8_t>((x >> 8) & 0xFF);
    v[off + 2] = static_cast<std::uint8_t>((x >> 16) & 0xFF);
    v[off + 3] = static_cast<std::uint8_t>((x >> 24) & 0xFF);
}

std::string sl472Repeat(const char* unit, int n)
{
    std::string s;
    for (int i = 0; i < n; ++i)
    {
        s += unit;
    }
    return s;
}
// 24 个「中」(U+4E2D,3 字节)= 72 字节;24 个 U+1F3A4(4 字节)= 96 字节,恰好顶满槽。
const char* const kZhong = "\xE4\xB8\xAD"; // U+4E2D
const char* const kMic = "\xF0\x9F\x8E\xA4"; // U+1F3A4(4 字节)
const char* const kHarmonyL = "\xE5\x92\x8C\xE5\xA3\xB0 L"; // 「和声 L」
const char* const kHarmonyR = "\xE5\x92\x8C\xE5\xA3\xB0 R"; // 「和声 R」

scvb::state::OutputState sl472NonDefault()
{
    scvb::state::OutputState s;
    // 每一项都至少在一条轨上取非默认值;不同轨取不同值,写串轨 / 共用槽会在逐项比对里现形。
    s.channels[0].label = "Lead Vox";
    s.channels[0].leadLock = true;
    s.channels[0].priority = 10u;
    s.channels[1].label = kHarmonyL;
    s.channels[1].pairId = 3u;
    s.channels[1].participateAutoPan = 0u; // 显式不参与
    s.channels[2].label = kHarmonyR;
    s.channels[2].pairId = 3u;
    s.channels[2].participateAutoPan = 1u; // 显式参与(与「未显式设置」2 分开存)
    s.channels[3].enabled = false;
    s.channels[3].priority = 0u;
    s.channels[4].leadVolExempt = true;
    s.channels[4].pairId = 7u;
    s.channels[13].label = sl472Repeat(kZhong, 24); // 24 码点 / 72 字节
    s.channels[14].label = sl472Repeat(kMic, 24); // 24 码点 / 96 字节(槽上限)
    s.channels[14].priority = 1u;
    return s;
}
} // namespace

TEST_CASE("OutputStateCodec:[SL-472] channels 七项往返 + 与前面几档互不串", "[output][state][sl472]")
{
    const scvb::state::OutputState s = sl472NonDefault();
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    REQUIRE(b.size() == 24u + 2u + 52u + 1860u); // 24 头 + "en" 2 + 前四档 52 + channels 15×124

    scvb::state::OutputState d;
    scvb::state::OutputDecodeReport r;
    REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
    for (std::size_t t = 0; t < 15u; ++t)
    {
        INFO("channel index " << t);
        CHECK(d.channels[t].enabled == s.channels[t].enabled);
        CHECK(d.channels[t].label == s.channels[t].label);
        CHECK(d.channels[t].participateAutoPan == s.channels[t].participateAutoPan);
        CHECK(d.channels[t].priority == s.channels[t].priority);
        CHECK(d.channels[t].leadLock == s.channels[t].leadLock);
        CHECK(d.channels[t].leadVolExempt == s.channels[t].leadVolExempt);
        CHECK(d.channels[t].pairId == s.channels[t].pairId);
    }
    // 字面量再钉一遍几格(上面是「与输入相等」,这里是「与规格值相等」—— 夹具被改成默认值时上面恒真)。
    CHECK(d.channels[0].label == "Lead Vox");
    CHECK(d.channels[0].leadLock);
    CHECK(d.channels[0].priority == 10u);
    CHECK(d.channels[1].label == kHarmonyL);
    CHECK(d.channels[1].pairId == 3u);
    CHECK(d.channels[1].participateAutoPan == 0u);
    CHECK(d.channels[2].participateAutoPan == 1u);
    CHECK(d.channels[5].participateAutoPan == 2u); // 没动过的轨保持「未显式设置」
    CHECK_FALSE(d.channels[3].enabled);
    CHECK(d.channels[3].priority == 0u);
    CHECK(d.channels[4].leadVolExempt);
    CHECK(d.channels[4].pairId == 7u);
    CHECK(d.channels[13].label.size() == 72u);
    CHECK(d.channels[14].label.size() == 96u);
    // 七项之外一个都不许被这一档搅动。
    CHECK(d.loudnessMode == "kw_integrated");
    CHECK(d.appliedCenterSlotPolicy == "priority_queue");
    CHECK(d.segmentationMinSegmentMs == 120u);
    CHECK(d.vadThresholdDb == -38.0f);
    CHECK(d.transitionRampMs == 80u);
    CHECK(d.unknownTail.empty());
    CHECK(r.channelEnabledFallbacks == 0u);
    CHECK(r.channelLabelFallbacks == 0u);
    CHECK(r.channelParticipateFallbacks == 0u);
    CHECK(r.channelPriorityFallbacks == 0u);
    CHECK(r.channelLeadLockFallbacks == 0u);
    CHECK(r.channelLeadVolExemptFallbacks == 0u);
    CHECK(r.channelPairIdFallbacks == 0u);

    std::vector<std::uint8_t> b2;
    REQUIRE(scvb::state::encodeOutputState(d, b2));
    CHECK(b == b2); // 逐字节往返
}

TEST_CASE("OutputStateCodec:[SL-472] label 编码侧截断:≤24 码点、≤96 字节、不切半个码点", "[output][state][sl472]")
{
    const auto roundTrip = [](const std::string& label) {
        scvb::state::OutputState s;
        s.channels[6].label = label;
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        REQUIRE(b.size() == 24u + 2u + 52u + 1860u); // 定长槽:截不截都不改变档长
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
        CHECK(r.channelLabelFallbacks == 0u); // 编码侧截出来的东西一定能被原样收回
        return d.channels[6].label;
    };
    // 25 个「中」→ 24 个(码点上限先到)。
    CHECK(roundTrip(sl472Repeat(kZhong, 25)) == sl472Repeat(kZhong, 24));
    // 30 个 4 字节码点 → 24 个 = 96 字节(两个上限同时到)。
    CHECK(roundTrip(sl472Repeat(kMic, 30)) == sl472Repeat(kMic, 24));
    // 100 个 ASCII → 24 个。
    CHECK(roundTrip(std::string(100, 'a')) == std::string(24, 'a'));
    // 23 个 ASCII + 1 个 4 字节码点 = 24 码点 / 27 字节:恰好在上限内,原样保留。
    CHECK(roundTrip(std::string(23, 'b') + kMic) == std::string(23, 'b') + kMic);
    // 内存里出现坏字节(0xFF)时在那里截止,而不是整条写成一个解码时会被判非法的串。
    std::string withBad = "ab";
    withBad.push_back(static_cast<char>(0xFF));
    withBad += "cd";
    CHECK(roundTrip(withBad) == "ab");
    // 空名原样空。
    CHECK(roundTrip(std::string()).empty());
}

TEST_CASE("OutputStateCodec:[SL-472] abi≤5 旧 payload(无 channels 档)⇒ 七项构造默认且不计回落",
          "[output][state][sl472]")
{
    // 与 [SL-411]/[SL-416] 同一条取舍(与 [SL-279] `applied := 当前值` **相反**):七项就是「当前设置」本身,
    // 旧工程确实没存过 → 取构造默认(= 旧构建重开后 runtime_.channels 的值),且**不计回落**。
    // 这一格也是 J113 的机检形态:修前用户存盘重开读到的就是这一组默认。
    // 每一种更旧的形态(abi=5/4/3/2/1)都走一遍:channels 档的缺席判定不能依赖前面哪一档在不在。
    for (const std::size_t tail : {52u, 28u, 16u, 8u, 0u})
    {
        const scvb::state::OutputState s = sl472NonDefault(); // 先写成非默认,好证明读到的默认不是「本来就没写」
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        b.resize(24u + 2u + tail);
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        INFO("tail bytes = " << tail);
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
        for (std::size_t t = 0; t < 15u; ++t)
        {
            INFO("channel index " << t);
            CHECK(d.channels[t].enabled);
            CHECK(d.channels[t].label.empty());
            CHECK(d.channels[t].participateAutoPan == 2u); // 未显式设置 ⇒ [J83] 一律参与
            CHECK(d.channels[t].priority == 5u);
            CHECK_FALSE(d.channels[t].leadLock);
            CHECK_FALSE(d.channels[t].leadVolExempt);
            CHECK(d.channels[t].pairId == 0u);
        }
        CHECK(r.channelEnabledFallbacks == 0u); // 缺席不算回落
        CHECK(r.channelLabelFallbacks == 0u);
        CHECK(r.channelParticipateFallbacks == 0u);
        CHECK(r.channelPriorityFallbacks == 0u);
        CHECK(r.channelLeadLockFallbacks == 0u);
        CHECK(r.channelLeadVolExemptFallbacks == 0u);
        CHECK(r.channelPairIdFallbacks == 0u);
        CHECK(d.unknownTail.empty());
    }
}

TEST_CASE("OutputStateCodec:[SL-472] 非法值 ⇒ 该轨该项单独回落默认并按字段计数", "[output][state][sl472]")
{
    struct BadU32
    {
        std::size_t field; // 记录内偏移
        std::uint32_t value;
    };
    // 每个判据原子各打一格;边界本身(1 / 2 / 10 / 7)的「合法」由往返那一格覆盖。
    const BadU32 bads[] = {
        {0u, 2u}, // enabled 只认 0/1
        {4u, 3u}, // participate 只认 0/1/2
        {8u, 11u}, // priority > 10
        {8u, 0xFFFFFFFFu}, // priority 哨兵(负数 static_cast 过来就是这个形态)
        {12u, 2u}, // lead_lock
        {16u, 7u}, // lead_vol_exempt
        {20u, 8u}, // pair_id > 7
    };
    constexpr std::size_t kT = 4u; // 第 5 轨:它的 lead_vol_exempt / pair_id 在夹具里是非默认值
    for (const auto& bad : bads)
    {
        const scvb::state::OutputState s = sl472NonDefault();
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        sl472PutU32(b, kSl472ChBase + kSl472Rec * kT + bad.field, bad.value);
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        INFO("field offset " << bad.field << " = " << bad.value);
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r)); // 单项坏值不拒载整块
        const auto& c = d.channels[kT];
        // 被点中的那一项回落构造默认,同一轨其余项保持夹具值(不是整轨回落)。
        CHECK(c.enabled == (bad.field == 0u ? true : s.channels[kT].enabled));
        CHECK(c.participateAutoPan == (bad.field == 4u ? 2u : s.channels[kT].participateAutoPan));
        CHECK(c.priority == (bad.field == 8u ? 5u : s.channels[kT].priority));
        CHECK(c.leadLock == (bad.field == 12u ? false : s.channels[kT].leadLock));
        CHECK(c.leadVolExempt == (bad.field == 16u ? false : s.channels[kT].leadVolExempt));
        CHECK(c.pairId == (bad.field == 20u ? 0u : s.channels[kT].pairId));
        // 计数器逐字段独立:被点中的 == 1,其余 == 0。
        CHECK(r.channelEnabledFallbacks == (bad.field == 0u ? 1u : 0u));
        CHECK(r.channelParticipateFallbacks == (bad.field == 4u ? 1u : 0u));
        CHECK(r.channelPriorityFallbacks == (bad.field == 8u ? 1u : 0u));
        CHECK(r.channelLeadLockFallbacks == (bad.field == 12u ? 1u : 0u));
        CHECK(r.channelLeadVolExemptFallbacks == (bad.field == 16u ? 1u : 0u));
        CHECK(r.channelPairIdFallbacks == (bad.field == 20u ? 1u : 0u));
        CHECK(r.channelLabelFallbacks == 0u);
        // 别的轨与前面几档不受牵连。
        CHECK(d.channels[0].label == "Lead Vox");
        CHECK(d.channels[1].pairId == 3u);
        CHECK(d.vadThresholdDb == -38.0f);
    }
    // 计数按字段在 15 轨上累加:三条轨的 pair_id 都坏 ⇒ pair_id 计 3。
    {
        const scvb::state::OutputState s = sl472NonDefault();
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        for (const std::size_t t : {1u, 2u, 14u})
        {
            sl472PutU32(b, kSl472ChBase + kSl472Rec * t + 20u, 99u);
        }
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
        CHECK(r.channelPairIdFallbacks == 3u);
        CHECK(d.channels[1].pairId == 0u);
        CHECK(d.channels[4].pairId == 7u);
    }
}

TEST_CASE("OutputStateCodec:[SL-472] label 非法 ⇒ 该轨名回落空串并计数,不牵连同轨其余项", "[output][state][sl472]")
{
    // 每个输入只打中一个判据原子。
    struct BadLabel
    {
        const char* what;
        std::uint32_t labelBytes;
        std::vector<std::uint8_t> bytes;
    };
    const std::vector<BadLabel> bads = {
        {"labelBytes > 96", 97u, {0x61}},
        {"lone continuation byte", 2u, {0x61, 0x80}},
        {"invalid lead byte 0xFF", 1u, {0xFF}},
        {"truncated 3-byte sequence", 2u, {0xE4, 0xB8}},
        {"overlong encoding of slash", 2u, {0xC0, 0xAF}},
        {"UTF-16 surrogate U+D800", 3u, {0xED, 0xA0, 0x80}},
        {"above U+10FFFF", 4u, {0xF4, 0x90, 0x80, 0x80}},
        {"embedded NUL", 3u, {0x61, 0x00, 0x62}},
        {"25 code points", 25u, std::vector<std::uint8_t>(25u, 0x78)},
    };
    for (const auto& bad : bads)
    {
        const scvb::state::OutputState s = sl472NonDefault();
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        const std::size_t rec = kSl472ChBase; // 第 1 轨:名字 "Lead Vox"、lead_lock 开、优先级 10
        sl472PutU32(b, rec + 24u, bad.labelBytes);
        for (std::size_t i = 0; i < bad.bytes.size(); ++i)
        {
            b[rec + 28u + i] = bad.bytes[i];
        }
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        INFO("bad label: " << bad.what);
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
        CHECK(d.channels[0].label.empty());
        CHECK(r.channelLabelFallbacks == 1u);
        CHECK(d.channels[0].leadLock); // 同轨其余项照常
        CHECK(d.channels[0].priority == 10u);
        CHECK(d.channels[1].label == kHarmonyL); // 别的轨照常
        CHECK(r.channelPriorityFallbacks == 0u);
    }
    // 边界本身合法:恰好 24 码点 / 96 字节(夹具第 15 轨)与 labelBytes = 0(空名)。
    {
        const scvb::state::OutputState s = sl472NonDefault();
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        scvb::state::OutputState d;
        scvb::state::OutputDecodeReport r;
        REQUIRE(scvb::state::decodeOutputState(b.data(), b.size(), d, &r));
        CHECK(d.channels[14].label == sl472Repeat(kMic, 24));
        CHECK(d.channels[7].label.empty());
        CHECK(r.channelLabelFallbacks == 0u);
    }
}

TEST_CASE("OutputStateCodec:[SL-472] channels 半截(52<remaining<1912)→ 拒载;边界 52 / 1912 可解",
          "[output][state][sl472]")
{
    // 一整档 1860 字节是同一个构建写下去的,「只有前几条记录」不可能是任何真实构建的产物。
    for (const std::size_t tail : {53u, 52u + 124u, 52u + 1859u})
    {
        const scvb::state::OutputState s = sl472NonDefault();
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        b.resize(24u + 2u + tail);
        scvb::state::OutputState d;
        INFO("tail bytes = " << tail);
        CHECK_FALSE(scvb::state::decodeOutputState(b.data(), b.size(), d));
    }
    for (const std::size_t tail : {52u, 1912u})
    {
        const scvb::state::OutputState s = sl472NonDefault();
        std::vector<std::uint8_t> b;
        REQUIRE(scvb::state::encodeOutputState(s, b));
        b.resize(24u + 2u + tail);
        scvb::state::OutputState d;
        INFO("tail bytes = " << tail);
        CHECK(scvb::state::decodeOutputState(b.data(), b.size(), d));
    }
}

TEST_CASE("OutputStateCodec:unknownTail 解码保留 + 编码原样回写", "[output][state]")
{
    scvb::state::OutputState s;
    s.loudnessMode = "rms";
    s.centerSlotPolicy = "lead_exclusive";
    std::vector<std::uint8_t> b;
    REQUIRE(scvb::state::encodeOutputState(s, b));
    // 模拟未来小版本追加:已知字段之后追加 4 字节未知尾部。
    // [SL-411] 起「已知字段」到 segmentation 那一档为止(尾长 28),[SL-416] 起推到 vad/ramp 那一档
    // (尾长 52),[SL-472] 起推到 channels[15] 那一档(尾长 1912),所以这 4 个字节是**第六档**的未知尾部
    // (remaining = 1916)—— 这正是 unknownTail 该生效的形态。
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
    REQUIRE(b.size() == 24u + 5u + 52u + 1860u); // [SL-472] + channels 1860
    // 24 头 + 5 语言 + 13×u32(当前 2 + applied 2 + seg 3* + vad/ramp 6*)
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
    // 模拟「旧版读新」:把容器 abi 抬到 kCurrentAbi+1(相对量)代表未来/更高版本。
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

TEST_CASE("挂起(suspended)与失准分开呈现:bypass 走挂起,不走失准", "[output][session][t37]")
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

    // ④ 停滞坐实(写头自 1300 起没动过,已 >500ms)→ **挂起**态亮起,而不是失准
    //    (统筹裁定丙案:写方停着与读方跟丢是两件事,各有各的位)。
    in.heartbeat(1900);
    out.tick(1900);
    CHECK(out.channelConn(3, 9999).suspended);
    CHECK(out.misalignCountRecent(3) == 0); // 全程都不是失准

    // ⑤ **只是心跳还在、不再产生新的缺口/饿读,不算恢复**(v4 实测 P0-2 的假恢复):
    //    Input 被 bypass 后本轨转 suspended 退出注入集,read() 不再被调用,痕迹自然停止增长 ——
    //    此时若判恢复,状态会在实际仍无声时撤下。写头没动,挂起必须保持。
    in.heartbeat(3200);
    out.tick(3200);
    CHECK(out.channelConn(3, 9999).suspended);

    // ⑥ 数据真的恢复推进(写头前移)→ 挂起立刻撤下(它描述的是「此刻在不在出数据」,
    //    没有恢复窗要等 —— 那是失准才需要的迟滞)。
    scvb::AudioRing::write(in.audioRing().acquire(), 32, buf, 32); // write_head = 64
    in.heartbeat(3300);
    out.tick(3300);
    CHECK_FALSE(out.channelConn(3, 9999).suspended);
    CHECK(out.misalignCountRecent(3) == 0);

    // ⑦ 再次停流 → 重新亮起(不是一次性)。
    //    **要两次失败读**:写方是否在推进得比较两次采样才判得出。第一次失败之后写头没再动,
    //    第二次才能定性成「写方停着」(只有一次的话按「写方在推进、读方跑过头」算真失准 ——
    //    那是另一件事,见 ShmRingMixSource 的成因分流)。
    REQUIRE_FALSE(src.read(128, dst.data(), 16));
    REQUIRE_FALSE(src.read(128, dst.data(), 16));
    in.heartbeat(4900);
    out.tick(4900); // 写头自 3300 起没动过,已 >500ms → 第二次挂起
    CHECK(out.channelConn(3, 9999).suspended);
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

namespace
{
// 确定性测试信号(48kHz 单声道);gain≠1 = 「用户在 Input 前面插了个改过参数的处理器」。
std::vector<float> fpSignal(int samples, float gain)
{
    std::vector<float> v(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i)
    {
        const double t = static_cast<double>(i) / 48000.0;
        const double env = 0.3 + 0.25 * std::sin(2.0 * 3.14159265358979 * 0.7 * t);
        v[static_cast<std::size_t>(i)] = static_cast<float>(gain * env * std::sin(2.0 * 3.14159265358979 * 440.0 * t));
    }
    return v;
}

// 模拟 InputProcessor::drainFpReports:[A] 攒的 fp 由 [M] 转投本 slot 的 ctrl 命令环。
std::uint32_t relayFpReports(scvb::FeatRing& ring, scvb::CtrlPlane& ctrl, scvb::u32 channel)
{
    scvb::u64 values[32] = {};
    const auto n = ring.drainFpReports(values, 32);
    for (std::uint32_t i = 0; i < n; ++i)
    {
        REQUIRE(ctrl.enqueue(channel, scvb::CtrlOp::kFpReport, values[i]));
    }
    return n;
}

// 每拍推进一次音频写头再 tick。evaluateChannels 的在线判据含「最近 500ms 内确有新帧写入」,
// 只在开头写一次的话该轨几拍之后就掉出 connected_mask,pullTick 会整轨跳过。
// Output 自己也要「在被宿主调用」(bumpBlockCounter = processBlock 首行,[J52]):只推 Input 写头、
// Output 的块计数一动不动,正是 KI-6 的停调形状 —— 看门狗 0.5 s 后会闩住全部轨。修 H5 之前这里
// 不推也照样绿,只是因为那时看门狗清 mask 只清一拍。
void fpTickOnline(OutputSession& out, InputSession& in, scvb::u64 nowMs, std::int64_t& audioPos)
{
    float chunk[64] = {};
    scvb::AudioRing::write(in.audioRing().acquire(), audioPos, chunk, 64);
    audioPos += 64;
    out.bumpBlockCounter();
    in.heartbeat(nowMs);
    out.tick(nowMs);
}

// 喂满整段信号(512 一块,模拟宿主块流)。
void fpFeed(scvb::FeatRing& ring, const std::vector<float>& mono)
{
    const int total = static_cast<int>(mono.size());
    for (int off = 0; off < total; off += 512)
    {
        const int n = std::min(512, total - off);
        const float* p = mono.data() + off;
        const float* planar[2] = {p, nullptr};
        ring.processBlock(planar, n);
    }
}
} // namespace

TEST_CASE("SL-177 端到端:采集 → 上游改动 → Output 该轨 stale", "[output][session][ctrl][fingerprint]")
{
    // 用户实测:在 Input 前面加了 EQ 并修改,Output 侧没有任何「素材变了,建议重新采集」的提示。
    // 通路 = Input [A] 算 tile 指纹 → [M] 排水投 ctrl 命令环 → Output [M] 消费并与 FrameStore
    // 基线比对(04 §4.5 / J46)。本例正反各走一遍:同一段音频不得报 stale,换过的音频必须报。
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession in(backend, 1001);
    in.setChannelId(2);
    REQUIRE(in.prepare(48000, 512, 1, 1000) == InputClaimState::kActive);
    in.heartbeat(1100);
    REQUIRE(in.featRing().bound());

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1200) == OutputClaimState::kActive);
    out.setCaptureEnabled(true);

    // 该轨过 [J32] 注入延迟进 connected_mask(pullTick 只拉在线轨)。
    std::int64_t audioPos = 0;
    fpTickOnline(out, in, 1300, audioPos);
    fpTickOnline(out, in, 1600, audioPos);

    // ---- ① 采集:Input 写 4 秒特征,Output 拉进 FrameStore 成为基线 ----
    const std::vector<float> original = fpSignal(48000 * 4, 1.0f);
    in.featRing().setCapturing(true);
    in.featRing().startRun(0);
    fpFeed(in.featRing(), original);
    for (int i = 0; i < 8; ++i) // pullIncremental 每拍最多 256 hop,多拍几次拉满 400 hop
    {
        fpTickOnline(out, in, 1700 + static_cast<scvb::u64>(i) * 40, audioPos);
    }
    REQUIRE(out.frameStore().channel(2).coversFully(scvb::analysis::HopRange{0, 300}));
    // 采集期间不上报 fp(此刻这一秒正在被写成新基线,拿它跟自己比毫无意义)。
    scvb::u64 drained[32] = {};
    CHECK(in.featRing().drainFpReports(drained, 32) == 0);

    scvb::CtrlPlane inputCtrl(backend, 1);
    REQUIRE(inputCtrl.open() == scvb::InitResult::kOk);

    // ---- ② 采集关 + 重播**同一段**音频 → 指纹一致 → 不得 stale(反向验证)----
    out.setCaptureEnabled(false);
    in.featRing().setCapturing(false);
    in.featRing().startRun(0);
    fpFeed(in.featRing(), original);
    const auto sameCount = relayFpReports(in.featRing(), inputCtrl, 2);
    REQUIRE(sameCount >= 3); // 4 秒音频 ⇒ 至少 3 个完整 tile
    out.tick(2100);
    CHECK(out.fpTilesChecked(2) == sameCount);
    CHECK(out.fpTilesMismatched(2) == 0);
    CHECK_FALSE(out.channelStale(2)); // ← 素材没变就绝不能提示

    // ---- ③ 用户在 Input 前面插了 EQ 并改了参数 → 指纹失配 → 该轨 stale ----
    in.featRing().startRun(0);
    fpFeed(in.featRing(), fpSignal(48000 * 4, 0.5f));
    REQUIRE(relayFpReports(in.featRing(), inputCtrl, 2) >= 3);
    out.tick(2200);
    CHECK(out.fpTilesMismatched(2) >= 3); // 连续 3 秒失配才定谳(滞回)
    CHECK(out.channelStale(2)); // ← 修复前这里恒 false(命令环只排空不消费)

    // 只影响该轨,不牵连别的轨。
    CHECK_FALSE(out.channelStale(1));
    CHECK_FALSE(out.channelStale(3));

    // ---- ④ 改回原样重播 → 提示自行撤下(只提示、可自愈,不阻断任何操作)----
    in.featRing().startRun(0);
    fpFeed(in.featRing(), original);
    REQUIRE(relayFpReports(in.featRing(), inputCtrl, 2) >= 3);
    out.tick(2300);
    CHECK(out.fpTilesMismatched(2) == 0);
    CHECK_FALSE(out.channelStale(2));
}

TEST_CASE("SL-177 闭环:照着 ⚠ 重新采集 → 提示撤下", "[output][session][ctrl][fingerprint]")
{
    // ⚠ 的行动号召是「建议重新采集」。重采集期间采集是 ON 的,而采集 ON 期间 Input 一条
    // fp_report 都不发 —— 自愈路径此刻是断的。若不在「拉到新特征」这一跳上清账,用户照做之后
    // ⚠ 原样挂着,只能靠再关一次采集重播一遍才撤下。
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession in(backend, 1001);
    in.setChannelId(2);
    REQUIRE(in.prepare(48000, 512, 1, 1000) == InputClaimState::kActive);
    in.heartbeat(1100);

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1200) == OutputClaimState::kActive);
    out.setCaptureEnabled(true);
    std::int64_t audioPos = 0;
    fpTickOnline(out, in, 1300, audioPos);
    fpTickOnline(out, in, 1600, audioPos);

    // 采集 4 秒建立基线。
    const std::vector<float> original = fpSignal(48000 * 4, 1.0f);
    in.featRing().setCapturing(true);
    in.featRing().startRun(0);
    fpFeed(in.featRing(), original);
    for (int i = 0; i < 8; ++i)
    {
        fpTickOnline(out, in, 1700 + static_cast<scvb::u64>(i) * 40, audioPos);
    }
    REQUIRE(out.frameStore().channel(2).coversFully(scvb::analysis::HopRange{0, 300}));

    scvb::CtrlPlane inputCtrl(backend, 1);
    REQUIRE(inputCtrl.open() == scvb::InitResult::kOk);

    // 关采集 + 上游改了 → stale。
    out.setCaptureEnabled(false);
    in.featRing().setCapturing(false);
    in.featRing().startRun(0);
    fpFeed(in.featRing(), fpSignal(48000 * 4, 0.5f));
    REQUIRE(relayFpReports(in.featRing(), inputCtrl, 2) >= 3);
    out.tick(2100);
    REQUIRE(out.channelStale(2));

    // 用户照做:重新开采集,回到工程开头再播一遍(改过的素材这次被录成新基线)。
    out.setCaptureEnabled(true);
    in.featRing().setCapturing(true);
    in.featRing().startRun(0); // 回卷 seek:读侧下一拍看到 write_hop 回退并重置游标
    fpTickOnline(out, in, 2200, audioPos);
    fpFeed(in.featRing(), fpSignal(48000 * 4, 0.5f));
    for (int i = 0; i < 8; ++i)
    {
        fpTickOnline(out, in, 2240 + static_cast<scvb::u64>(i) * 40, audioPos);
    }
    // Output 一拉到新特征就清账 —— 旧判定所依据的基线正在被改写。
    CHECK(out.fpTilesChecked(2) == 0);
    CHECK_FALSE(out.channelStale(2)); // ← 修复前会一直挂着,只能靠再关一次采集重播才撤下
}

// ===========================================================================
// [SL-254 / J95①] setNonRealtime —— 离线渲染下跳过 [J32] 的 200ms **墙钟** 注入闸。
// 复审 r1【重要】:新增 core 纯逻辑须有 Catch2 用例(CLAUDE.md §7);host harness 是集成级。
// ===========================================================================

TEST_CASE("SL-254:非实时下首次上线即注入(跳过 200ms 墙钟闸)", "[output][session][SL254]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession in(backend, 1001);
    in.setChannelId(3);
    REQUIRE(in.prepare(48000, 512, 1, 1000) == InputClaimState::kActive);
    in.heartbeat(1100);
    float buf[16] = {};
    scvb::AudioRing::write(in.audioRing().acquire(), 0, buf, 16);

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1200) == OutputClaimState::kActive);
    out.setNonRealtime(true); // 宿主宣告离线渲染
    REQUIRE(out.nonRealtime());

    // ★ 与上面那条 [J32] 用例**同一时序**(单次 tick、无 muted 位、0 < 200ms):
    //   实时下 injectMask 必为 0,非实时下必须**当拍**置位 —— 同块交接的 Output 半边。
    out.tick(1300);
    scvb::Registry probe(backend, 1);
    REQUIRE(probe.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE((probe.connectedMask() & (1u << 2)) != 0);
    REQUIRE((out.injectMask() & (1u << 2)) != 0); // 实时形态在这里红(injectMask == 0)
}

TEST_CASE("SL-254:非实时标志可运行期切回,实时闸随即恢复", "[output][session][SL254]")
{
    // 守住「实时路径一字不动」:宿主 bounce 结束后切回实时,200ms 闸必须原样回来。
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession in(backend, 1001);
    in.setChannelId(3);
    REQUIRE(in.prepare(48000, 512, 1, 1000) == InputClaimState::kActive);
    in.heartbeat(1100);
    float buf[16] = {};
    scvb::AudioRing::write(in.audioRing().acquire(), 0, buf, 16);

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1200) == OutputClaimState::kActive);
    REQUIRE_FALSE(out.nonRealtime()); // 默认实时

    out.tick(1300);
    REQUIRE(out.injectMask() == 0); // 实时:0 < 200ms,不注入

    out.setNonRealtime(true);
    out.tick(1340); // 仍 < 200ms,但已宣告非实时 → 当拍注入
    REQUIRE((out.injectMask() & (1u << 2)) != 0);
}

// ===========================================================================
// [SL-482] / [SL-486] audio 段几何换手 —— 写侧改写 + 读侧刷新,两端配套。
//
// 场景是用户在 DAW 里真做得出来的一件事:mono 轨上的 Input 删掉,同一个通道号换成
// stereo 轨上的 Input。段不会在这中间消亡(Output 常驻持着 audioHandles_[idx]),
// 于是新 Input 的 initHeader 走「attach 且 magic 已就绪」那条路,initData 一次不跑。
// 修前:段头 channels 停在 1,Input 的 write() 用 stride=1 把 LR 交错流当连续帧写,
// Output 按 mono 读 —— 总线上是半速交替 L/R 的撕裂噪音,直到 Output 卸载。
// ===========================================================================
TEST_CASE("[SL-482][SL-486] 新 Input 接手存活段:写侧改写几何,读侧跟着刷新快照", "[output][session][ipc]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    constexpr u32 kCh = 3;

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1000) == OutputClaimState::kActive);

    scvb::u64 epochMono = 0;
    {
        // 第一个 Input:mono 轨。
        InputSession in1(backend, 1001);
        in1.setChannelId(kCh);
        REQUIRE(in1.prepare(48000, 512, /*channels=*/1, 1000) == InputClaimState::kActive);
        in1.heartbeat(1010);

        out.tick(1040); // Output attach 到 mono 段
        REQUIRE(out.mixSource(kCh).bound());
        REQUIRE(out.mixSource(kCh).channels() == 1);
        epochMono = out.epoch(kCh);
    } // in1 析构:释放 slot 与段句柄 —— 但 Output 仍持着 audioHandles_,section 不消亡

    // Output 这边**没有**解绑:releaseSegments 只在改组/release/析构时调。
    REQUIRE(out.mixSource(kCh).bound());

    // 第二个 Input:同一个通道号,stereo 轨。
    InputSession in2(backend, 1002);
    in2.setChannelId(kCh);
    REQUIRE(in2.prepare(48000, 512, /*channels=*/2, 1100) == InputClaimState::kActive);
    in2.heartbeat(1110);

    // [SL-482] 写侧:段头几何必须被改写成 stereo,并换代(读方据此丢弃旧代数据)。
    // 删掉 createSegments 里那段「按值比对 → 回写几何」→ 下面三条当场红。
    const scvb::AudioRingBinding* wb = in2.audioRing().acquire();
    REQUIRE(wb != nullptr);
    REQUIRE(wb->bound);
    CHECK(wb->header->channels == 2); // 段头真身
    CHECK(in2.audioRing().geometry().channels == 2); // 写侧快照
    CHECK(out.epoch(kCh) > epochMono); // epoch 升:旧代数据作废

    // [SL-486] 读侧:Output 的几何快照必须在下一拍 [M] 跟上,否则一直用 stride=1 解码
    // stereo 数据。删掉 refreshAudioGeometry 的重绑(或它在 attachAudioRings 里的调用)
    // → 下面这条当场红,而上面 SL-482 那三条**仍绿** —— 这一格证明的是「写侧的修法没有把
    // 读侧一起代偿掉」。⚠ 反方向不可分:删掉写侧的几何回写,读侧这两条也会跟着红,因为
    // 段头几何压根没变、读侧就没有可刷新的东西。两条缺陷本来就是一前一后串在一条链上,
    // 只有「读侧删掉、写侧仍绿」这半边分得开,别把这一格读成双向隔离。
    out.tick(1140);
    CHECK(out.mixSource(kCh).channels() == 2);
    CHECK(out.mixSource(kCh).bound()); // 重绑没有把这一路绑废

    // 反向:几何没变时不得白重绑 —— 再 tick 两拍,快照恒定(也顺带保证 owned_ 不会每拍长一个)。
    out.tick(1180);
    out.tick(1220);
    CHECK(out.mixSource(kCh).channels() == 2);
    CHECK(out.mixSource(kCh).sampleRate() == 48000);
}

// [SL-482] 反向:attach 到几何**相同**的存活段时不得凭空换代。
// 这条钉住的是「按值比对」而不是「attach 就无条件回写」:后者每次重新 attach 都会 epoch+1,
// 读方每次都要重新等写方追上(primed_ 归零、validFrom_ 前移)—— 平白多出一段静音。
// 走法是「换走再换回」,因为只有这条路才会真正再进一次 createSegments(纯块长变化
// 走 prepare 的快路径,连 createSegments 都不进)。
TEST_CASE("[SL-482] 反向:attach 到几何相同的存活段不换代", "[output][session][ipc]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    constexpr u32 kCh = 5;

    InputSession in(backend, 1001);
    in.setChannelId(kCh);
    REQUIRE(in.prepare(48000, 512, /*channels=*/2, 1000) == InputClaimState::kActive);
    const scvb::AudioRingBinding* b0 = in.audioRing().acquire();
    REQUIRE(b0 != nullptr);
    REQUIRE(b0->bound);
    scvb::AudioRingHeader* const h = b0->header; // 段在 in-process 后端里按名字常驻,指针不变
    const scvb::u64 e0 = h->epoch.load();
    REQUIRE(h->channels == 2);

    // 换到别的 channel,再换回来 —— 换回来那次 createSegments 是 attach 到存活的旧段。
    in.setChannelId(6);
    REQUIRE(in.prepare(48000, 512, /*channels=*/2, 1100) == InputClaimState::kActive);
    in.setChannelId(kCh);
    REQUIRE(in.prepare(48000, 512, /*channels=*/2, 1200) == InputClaimState::kActive);

    const scvb::AudioRingBinding* b1 = in.audioRing().acquire();
    REQUIRE(b1 != nullptr);
    REQUIRE(b1->bound);
    REQUIRE(b1->header == h); // 确实 attach 回了同一个段(不是新建了一个)
    CHECK(h->epoch.load() == e0); // 几何没变 → 不换代
    CHECK(h->channels == 2);
    CHECK(h->sample_rate == 48000);
}

// ===========================================================================
// [KI-6] 停摆看门狗闩锁(修 H5)端到端:InProcess 后端 + 3 个 InputSession + OutputSession,
// 虚拟时钟逐 10 ms 推进。宿主停调 Output 7 s(Input 照写、两边心跳都由 [M] 写着)→ Input 不健康满 5 s
// 转直通(从停调起算 5.5 ± 0.5 s);恢复调用后逐轨接回,接回时重走 [J32] 握手,双路叠加不超出 J32 窗。
//
// Input 那一侧的 [M] 逐行照 InputProcessor::timerCallback:isHealthy → StageSwitchStateMachine(5 s 滞回)
// → muted 确认位 = 「目标档是静音」。[A] 的 80 ms 等功率 ramp 记成「目标档转静音后 kStageRampMs 内仍有原声」。
// 拍点相位:Output [M] 在 t%40==0、Input [M] 在 t%40==20(两个 25Hz 定时器互不对齐);每一步先「音频」
// (各 Input 写一块、Output 没被停调就 bumpBlockCounter)再「[M]」,与宿主「先人声轨后总线」同序。
// ===========================================================================
namespace
{
using scvb::u64;
using scvb::input::OutputStageMode;
using scvb::input::StageSwitchStateMachine;

constexpr u64 kKi6StepMs = 10; // 一步 = 一块 = 10 ms(480 帧 @48k)
constexpr int kKi6BlockFrames = 480;
constexpr int kKi6Lanes = 3;
constexpr u64 kKi6ReacquireMs = 200; // CtrlPlane.cpp kReacquireIntervalMs(§4.3-b 让位间隔)

struct Ki6Lane
{
    Ki6Lane(scvb::SegmentBackendInProcess& backend, u32 pid, u32 ch) : session(backend, pid), channel(ch) {}

    InputSession session;
    StageSwitchStateMachine sm;
    u32 channel;
    OutputStageMode target = OutputStageMode::kPassthrough;
    u64 silenceSinceMs = 0; // 目标档最近一次转静音的时刻(ramp 起点)
    std::int64_t pos = 0;

    void writeBlock()
    {
        float buf[kKi6BlockFrames] = {};
        scvb::AudioRing::write(session.audioRing().acquire(), pos, buf, kKi6BlockFrames);
        pos += kKi6BlockFrames;
    }

    void tickM(u64 now)
    {
        const bool healthy = session.isHealthy(now);
        const OutputStageMode t = sm.evaluate(healthy, now);
        if (t == OutputStageMode::kSilence && target != OutputStageMode::kSilence)
        {
            silenceSinceMs = now;
        }
        target = t;
        session.setMuted(t == OutputStageMode::kSilence);
    }

    // 本轨此刻在总线上是否还有 Input 直通出来的原声(直通档,或转静音的 80 ms ramp 还没走完)。
    bool rawAudible(u64 now) const
    {
        if (target == OutputStageMode::kPassthrough)
        {
            return true;
        }
        return static_cast<double>(now - silenceSinceMs) < scvb::input::kStageRampMs;
    }
};

struct Ki6Sample
{
    u64 t = 0;
    bool outputCalled = false;
    bool outputTick = false; // 本步 Output [M] 跑了一拍
    u32 mask = 0;
    u32 inject = 0;
    bool raw[kKi6Lanes] = {};
    bool muted[kKi6Lanes] = {};
    bool healthy[kKi6Lanes] = {};
};

struct Ki6Rig
{
    scvb::SegmentBackendInProcess backend;
    std::vector<std::unique_ptr<Ki6Lane>> lanes;
    OutputSession out{backend, 2001};
    scvb::Registry probe{backend, 1};
    std::vector<Ki6Sample> log;
    u64 now = 1000;

    Ki6Rig()
    {
        for (u32 i = 0; i < static_cast<u32>(kKi6Lanes); ++i)
        {
            lanes.push_back(std::make_unique<Ki6Lane>(backend, 1001 + i, i + 1)); // channel 1..3
            lanes.back()->session.setChannelId(i + 1);
            REQUIRE(lanes.back()->session.prepare(48000, 512, 1, now) == InputClaimState::kActive);
            lanes.back()->session.heartbeat(now);
        }
        REQUIRE(out.prepare(48000, 512, now) == OutputClaimState::kActive);
        REQUIRE(probe.open() == scvb::Registry::ClaimResult::kClaimed);
        out.setTransportPlaying(true);
    }

    // 推进到 untilMs(不含);outputCalled = 宿主这段时间里调不调 Output 的 processBlock。
    void runUntil(u64 untilMs, bool outputCalled)
    {
        for (; now < untilMs; now += kKi6StepMs)
        {
            for (auto& l : lanes)
            {
                l->writeBlock();
            }
            if (outputCalled)
            {
                out.bumpBlockCounter(); // processBlock 首行([J52])
            }
            Ki6Sample s;
            s.t = now;
            s.outputCalled = outputCalled;
            if (now % 250 == 0)
            {
                out.heartbeat(now); // 心跳在 [M]:宿主停调 Output 不停它
                for (auto& l : lanes)
                {
                    l->session.heartbeat(now);
                }
            }
            if (now % 40 == 0)
            {
                out.tick(now);
                s.outputTick = true;
            }
            if (now % 40 == 20)
            {
                for (auto& l : lanes)
                {
                    l->tickM(now);
                }
            }
            s.mask = probe.connectedMask();
            s.inject = out.injectMask();
            for (int i = 0; i < kKi6Lanes; ++i)
            {
                const Ki6Lane& l = *lanes[static_cast<std::size_t>(i)];
                s.raw[i] = l.rawAudible(now);
                s.muted[i] = l.target == OutputStageMode::kSilence;
                s.healthy[i] = l.session.isHealthy(now);
            }
            log.push_back(s);
        }
    }
};

constexpr u32 ki6Bit(int lane)
{
    return 1u << static_cast<u32>(lane);
}
} // namespace

TEST_CASE("[KI-6] Output not called: lanes fall back to raw after 5.5 +/- 0.5 s and rejoin lane by lane",
          "[output][session][watchdog][KI6]")
{
    scvb::SegmentBackendInProcess::resetAll();
    Ki6Rig rig;

    constexpr u64 kStall = 3000; // 宿主从这一刻起不调 Output(Live 停用设备 / FL smart disable 一类)
    constexpr u64 kResume = 10000; // 停调 7 s 后恢复
    constexpr u64 kEnd = 13000;
    rig.runUntil(kStall, /*outputCalled=*/true);

    // 前提:停调前三轨都已接管(在 mask、在注入、Input 在静音档)。
    {
        const Ki6Sample& s = rig.log.back();
        REQUIRE(s.mask == 0x7u);
        REQUIRE(s.inject == 0x7u);
        for (int i = 0; i < kKi6Lanes; ++i)
        {
            REQUIRE(s.muted[i]);
        }
    }

    rig.runUntil(kResume, /*outputCalled=*/false);
    rig.runUntil(kEnd, /*outputCalled=*/true);

    // ---- 停调段 ------------------------------------------------------------
    // 看门狗触发:停调 0.5 s(+ 至多一拍)后 mask 清空。
    u64 tripMs = 0;
    for (const Ki6Sample& s : rig.log)
    {
        if (s.t >= kStall && s.t < kResume && s.outputTick && s.mask == 0)
        {
            tripMs = s.t;
            break;
        }
    }
    REQUIRE(tripMs != 0);
    CHECK(tripMs >= kStall + 500);
    CHECK(tripMs <= kStall + 540);

    // H5 的修法:触发之后整个停调段,Output 的每一拍都**不得**再把位置回去、也不得注入(修前下一拍就置回)。
    int relatchedTicks = 0;
    for (const Ki6Sample& s : rig.log)
    {
        if (s.t >= tripMs && s.t < kResume && s.outputTick && (s.mask != 0 || s.inject != 0))
        {
            ++relatchedTicks;
        }
    }
    CHECK(relatchedTicks == 0);

    for (int i = 0; i < kKi6Lanes; ++i)
    {
        INFO("lane " << i);
        u64 unhealthySince = 0;
        u64 passthroughAt = 0;
        for (const Ki6Sample& s : rig.log)
        {
            if (s.t < kStall || s.t >= kResume)
            {
                continue;
            }
            if (unhealthySince == 0 && !s.healthy[i])
            {
                unhealthySince = s.t;
            }
            if (passthroughAt == 0 && !s.muted[i])
            {
                passthroughAt = s.t;
            }
        }
        REQUIRE(unhealthySince != 0);
        REQUIRE(passthroughAt != 0);
        // Input 不健康满 5 s 才转直通(J12 滞回),从停调起算落在 5.5 ± 0.5 s(LS-11a 同一判据)。
        CHECK(passthroughAt - unhealthySince >= 5000);
        CHECK(passthroughAt >= kStall + 5000);
        CHECK(passthroughAt <= kStall + 6000);
        // 转直通之后一直直通到恢复(没有被下一拍置回的 mask 又拉回静音)。
        int backToSilence = 0;
        for (const Ki6Sample& s : rig.log)
        {
            if (s.t >= passthroughAt && s.t < kResume && s.muted[i])
            {
                ++backToSilence;
            }
        }
        CHECK(backToSilence == 0);
    }

    // ---- 恢复段:逐轨接回 ---------------------------------------------------
    u64 rejoinAt[kKi6Lanes] = {};
    u64 injectAt[kKi6Lanes] = {};
    for (const Ki6Sample& s : rig.log)
    {
        if (s.t < kResume)
        {
            continue;
        }
        for (int i = 0; i < kKi6Lanes; ++i)
        {
            if (rejoinAt[i] == 0 && (s.mask & ki6Bit(i)) != 0)
            {
                rejoinAt[i] = s.t;
            }
            if (injectAt[i] == 0 && (s.inject & ki6Bit(i)) != 0)
            {
                injectAt[i] = s.t;
            }
        }
    }
    for (int i = 0; i < kKi6Lanes; ++i)
    {
        INFO("lane " << i << " rejoin " << rejoinAt[i] << " inject " << injectAt[i]);
        REQUIRE(rejoinAt[i] != 0);
        REQUIRE(injectAt[i] != 0);
        CHECK(rejoinAt[i] <= kResume + 2000); // LS-11a:恢复后 2 s 内接回
        CHECK(injectAt[i] >= rejoinAt[i]); // 不在 mask 里的轨不注入
    }
    // 接回顺序:channel 升序,相邻两轨间隔 ≥ 200 ms(§4.3-b 让位,不得硬切)。
    CHECK(rejoinAt[0] < rejoinAt[1]);
    CHECK(rejoinAt[1] < rejoinAt[2]);
    CHECK(rejoinAt[1] - rejoinAt[0] >= kKi6ReacquireMs);
    CHECK(rejoinAt[2] - rejoinAt[1] >= kKi6ReacquireMs);

    for (int i = 0; i < kKi6Lanes; ++i)
    {
        INFO("lane " << i);
        // [J32] 握手:置位之后要等这一轨 Input 的 muted 确认位、或满 200 ms,才开始注入。
        bool mutedBeforeInject = false;
        for (const Ki6Sample& s : rig.log)
        {
            if (s.t >= rejoinAt[i] && s.t < injectAt[i] && s.muted[i])
            {
                mutedBeforeInject = true;
            }
        }
        CHECK((mutedBeforeInject || injectAt[i] - rejoinAt[i] >= scvb::output::kInjectDelayMs));

        // 双路叠加(Output 在出混音 ∧ 这一轨 Input 还在直通原声)不超出 J32 窗。
        u64 overlapMs = 0;
        for (const Ki6Sample& s : rig.log)
        {
            if (s.t >= kResume && s.outputCalled && (s.inject & ki6Bit(i)) != 0 && s.raw[i])
            {
                overlapMs += kKi6StepMs;
            }
        }
        CHECK(overlapMs <= scvb::output::kInjectDelayMs);
        WARN("KI-6 e2e lane " << i << ": trip +" << (tripMs - kStall) << " ms, rejoin +" << (rejoinAt[i] - kResume)
                              << " ms, inject +" << (injectAt[i] - kResume) << " ms, overlap " << overlapMs << " ms");
    }

    // 收尾:三轨都回到「在 mask、在注入、Input 静音」。
    const Ki6Sample& last = rig.log.back();
    CHECK(last.mask == 0x7u);
    CHECK(last.inject == 0x7u);
    for (int i = 0; i < kKi6Lanes; ++i)
    {
        CHECK(last.muted[i]);
    }
}

// 短停调(看门狗已触发、Input 还没等满 5 s 滞回):恢复时各 Input 仍报 muted、没有原声可叠,让位不必排队 ——
// 三轨在恢复后的同一拍接回。修 H5 之前这种短停调本来就是立即接回的(mask 根本没真断过);hold 若对它们也
// 逐轨排队,排第 k 的轨会白白多静音 (k−1)×200 ms(PR #379 第 1 轮评审)。
TEST_CASE("[KI-6] short Output stall: lanes whose Input is still muted rejoin together, without queueing",
          "[output][session][watchdog][KI6]")
{
    scvb::SegmentBackendInProcess::resetAll();
    Ki6Rig rig;

    constexpr u64 kStall = 3000;
    constexpr u64 kResume = 5000; // 停调 2 s:> 0.5 s(看门狗触发),< 5.5 s(Input 还没切直通)
    constexpr u64 kEnd = 7000;
    rig.runUntil(kStall, /*outputCalled=*/true);
    REQUIRE(rig.log.back().inject == 0x7u);
    rig.runUntil(kResume, /*outputCalled=*/false);
    rig.runUntil(kEnd, /*outputCalled=*/true);

    // 前提:停调期间看门狗确实触发、闩住了(mask 空过,且之后再没回来),Input 一直没切直通。
    u64 tripMs = 0;
    int relatched = 0;
    int passthroughSamples = 0;
    for (const Ki6Sample& s : rig.log)
    {
        if (s.t < kStall || s.t >= kResume)
        {
            continue;
        }
        if (tripMs == 0 && s.outputTick && s.mask == 0)
        {
            tripMs = s.t;
        }
        if (tripMs != 0 && s.outputTick && s.mask != 0)
        {
            ++relatched;
        }
        for (int i = 0; i < kKi6Lanes; ++i)
        {
            passthroughSamples += s.muted[i] ? 0 : 1;
        }
    }
    REQUIRE(tripMs != 0);
    REQUIRE(relatched == 0);
    REQUIRE(passthroughSamples == 0);

    u64 rejoinAt[kKi6Lanes] = {};
    u64 injectAt[kKi6Lanes] = {};
    for (const Ki6Sample& s : rig.log)
    {
        if (s.t < kResume)
        {
            continue;
        }
        for (int i = 0; i < kKi6Lanes; ++i)
        {
            if (rejoinAt[i] == 0 && (s.mask & ki6Bit(i)) != 0)
            {
                rejoinAt[i] = s.t;
            }
            if (injectAt[i] == 0 && (s.inject & ki6Bit(i)) != 0)
            {
                injectAt[i] = s.t;
            }
        }
    }
    for (int i = 0; i < kKi6Lanes; ++i)
    {
        INFO("lane " << i << " rejoin " << rejoinAt[i] << " inject " << injectAt[i]);
        REQUIRE(rejoinAt[i] != 0);
        REQUIRE(injectAt[i] != 0);
        // 恢复后一到两拍内接回(恢复那一拍 evaluateChannels 先跑、看门狗后跑,所以是下一拍),muted 已在 ⇒ 当拍注入。
        CHECK(rejoinAt[i] <= kResume + 80);
        CHECK(injectAt[i] == rejoinAt[i]);
    }
    CHECK(rejoinAt[1] == rejoinAt[0]);
    CHECK(rejoinAt[2] == rejoinAt[0]);
    WARN("KI-6 short stall: trip +" << (tripMs - kStall) << " ms, rejoin +" << (rejoinAt[0] - kResume) << "/+"
                                    << (rejoinAt[1] - kResume) << "/+" << (rejoinAt[2] - kResume) << " ms");

    const Ki6Sample& last = rig.log.back();
    CHECK(last.mask == 0x7u);
    CHECK(last.inject == 0x7u);
}
