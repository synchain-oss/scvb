// SPDX-License-Identifier: GPL-3.0-or-later
// test_authority —— T16:AuthorityMode 状态机(03 §2.2 每格)+ DSP 双源取值仲裁(§2.3)
// + 统一平滑层(§2.4)+ [J58] lead_select 实时覆盖层。纯核心,无 JUCE(ADR-011)。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#include "dsp/ParamSmoother.h"
#include "engine/AuthorityMode.h"
#include "engine/CurveEvaluator.h"
#include "engine/DspArbiter.h"

using Catch::Approx;

namespace
{

using scvb::engine::AuthorityContext;
using scvb::engine::AuthorityEvent;
using scvb::engine::AuthorityMode;
using scvb::engine::AuthorityStep;
using scvb::engine::DspArbiter;
using scvb::engine::stepAuthority;

constexpr double kFs = 48000.0;

// 常量曲线:单段覆盖 [0,1000),pan/vol 恒定。
scvb::CurveEvaluator constCurve(double pan, double volDb)
{
    scvb::CurveEvaluator ev;
    ev.build({scvb::CurveSegment{0.0, 1000.0, pan, volDb}}, scvb::TransitionConfig{});
    return ev;
}

// 一个 Arbiter + 15 轨 synthetic 来源(raw atomic + 常量曲线)。
struct ArbiterFixture
{
    DspArbiter arbiter;
    std::array<std::atomic<float>, DspArbiter::kNumTracks> rawPan{};
    std::array<std::atomic<float>, DspArbiter::kNumTracks> rawVol{};
    std::array<std::atomic<float>, DspArbiter::kNumTracks> rawTrkW{};
    std::array<std::atomic<float>, DspArbiter::kNumTracks> rawFrz{};
    std::atomic<float> rawLeadSelect{0.0f};
    std::array<scvb::CurveEvaluator, DspArbiter::kNumTracks> curves{};
    std::vector<std::unique_ptr<DspArbiter::Snapshot>> pool; // 进程寿命保活已发布快照

    ArbiterFixture()
    {
        for (int t = 0; t < DspArbiter::kNumTracks; ++t)
        {
            rawPan[static_cast<std::size_t>(t)].store(0.0f);
            rawVol[static_cast<std::size_t>(t)].store(0.0f);
            rawTrkW[static_cast<std::size_t>(t)].store(100.0f);
            rawFrz[static_cast<std::size_t>(t)].store(0.0f);
            curves[static_cast<std::size_t>(t)] = constCurve(static_cast<double>(t) * 10.0 - 70.0, -3.0);
        }
        rawLeadSelect.store(0.0f);
    }

    void bind()
    {
        auto snap = std::make_unique<DspArbiter::Snapshot>();
        for (int t = 0; t < DspArbiter::kNumTracks; ++t)
        {
            snap->sources[static_cast<std::size_t>(t)].rawPan = &rawPan[static_cast<std::size_t>(t)];
            snap->sources[static_cast<std::size_t>(t)].rawVol = &rawVol[static_cast<std::size_t>(t)];
            snap->sources[static_cast<std::size_t>(t)].rawTrkW = &rawTrkW[static_cast<std::size_t>(t)];
            snap->sources[static_cast<std::size_t>(t)].rawFrz = &rawFrz[static_cast<std::size_t>(t)];
            snap->sources[static_cast<std::size_t>(t)].curve = &curves[static_cast<std::size_t>(t)];
        }
        snap->rawLeadSelect = &rawLeadSelect;
        arbiter.publish(snap.get());
        pool.push_back(std::move(snap)); // 进程寿命保活
    }
};

// 前进 n 个样本(丢弃返回值)。
void advance(DspArbiter& a, int n)
{
    for (int i = 0; i < n; ++i)
        (void)a.nextSample();
}

} // namespace

// ============================================================================
// §2.2 事件-转移表:10 行 × 3 列,每格至少一条断言。
// ============================================================================

TEST_CASE("AUTH-SM-1 EnableOutput(用户开 Output)", "[authority][sm]")
{
    SECTION("FOLLOW → ARMED(未播放,区间外)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Follow, AuthorityEvent::EnableOutput, {false, false});
        REQUIRE(r.next == AuthorityMode::Armed);
        REQUIRE(r.authorityChanged);
        REQUIRE_FALSE(r.beginGesture);
        REQUIRE_FALSE(r.endAllGestures);
        REQUIRE_FALSE(r.handOver);
    }
    SECTION("FOLLOW → PRINT(播放且区间内)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Follow, AuthorityEvent::EnableOutput, {true, true});
        REQUIRE(r.next == AuthorityMode::Print);
        REQUIRE(r.authorityChanged);
        REQUIRE(r.beginGesture);
    }
    SECTION("ARMED → 不变(—)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Armed, AuthorityEvent::EnableOutput, {});
        REQUIRE(r.next == AuthorityMode::Armed);
        REQUIRE_FALSE(r.authorityChanged);
        REQUIRE_FALSE(r.beginGesture);
    }
    SECTION("PRINT → 不变(—)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Print, AuthorityEvent::EnableOutput, {});
        REQUIRE(r.next == AuthorityMode::Print);
        REQUIRE_FALSE(r.authorityChanged);
    }
}

TEST_CASE("AUTH-SM-2 DisableOutput(用户关 Output)", "[authority][sm]")
{
    SECTION("FOLLOW → 不变(—)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Follow, AuthorityEvent::DisableOutput, {});
        REQUIRE(r.next == AuthorityMode::Follow);
        REQUIRE_FALSE(r.authorityChanged);
        REQUIRE_FALSE(r.endAllGestures);
        REQUIRE_FALSE(r.handOver);
    }
    SECTION("ARMED → FOLLOW")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Armed, AuthorityEvent::DisableOutput, {});
        REQUIRE(r.next == AuthorityMode::Follow);
        REQUIRE(r.authorityChanged);
        REQUIRE_FALSE(r.endAllGestures);
        REQUIRE_FALSE(r.handOver);
    }
    SECTION("PRINT → endAllGestures + HANDING_OVER → FOLLOW")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Print, AuthorityEvent::DisableOutput, {});
        REQUIRE(r.next == AuthorityMode::Follow);
        REQUIRE(r.authorityChanged);
        REQUIRE(r.endAllGestures);
        REQUIRE(r.handOver);
    }
}

TEST_CASE("AUTH-SM-3 TransportPlay(区间内)", "[authority][sm]")
{
    const AuthorityContext in{true, true};
    SECTION("FOLLOW → 无(DSP 继续读 host 参数)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Follow, AuthorityEvent::TransportPlay, in);
        REQUIRE(r.next == AuthorityMode::Follow);
        REQUIRE_FALSE(r.authorityChanged);
        REQUIRE_FALSE(r.beginGesture);
    }
    SECTION("ARMED → PRINT + beginGesture")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Armed, AuthorityEvent::TransportPlay, in);
        REQUIRE(r.next == AuthorityMode::Print);
        REQUIRE(r.beginGesture);
        REQUIRE_FALSE(r.authorityChanged);
    }
    SECTION("PRINT → 不变(—)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Print, AuthorityEvent::TransportPlay, in);
        REQUIRE(r.next == AuthorityMode::Print);
        REQUIRE_FALSE(r.beginGesture);
    }
}

TEST_CASE("AUTH-SM-4 TransportStop", "[authority][sm]")
{
    SECTION("FOLLOW → 无")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Follow, AuthorityEvent::TransportStop, {});
        REQUIRE(r.next == AuthorityMode::Follow);
        REQUIRE_FALSE(r.endAllGestures);
    }
    SECTION("ARMED → 无")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Armed, AuthorityEvent::TransportStop, {});
        REQUIRE(r.next == AuthorityMode::Armed);
        REQUIRE_FALSE(r.endAllGestures);
    }
    SECTION("PRINT → endAllGestures → ARMED")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Print, AuthorityEvent::TransportStop, {});
        REQUIRE(r.next == AuthorityMode::Armed);
        REQUIRE(r.endAllGestures);
    }
}

TEST_CASE("AUTH-SM-5a PlayheadEnterRange", "[authority][sm]")
{
    SECTION("FOLLOW → 无")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Follow, AuthorityEvent::PlayheadEnterRange, {true, true});
        REQUIRE(r.next == AuthorityMode::Follow);
        REQUIRE_FALSE(r.beginGesture);
    }
    SECTION("ARMED + playing → PRINT")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Armed, AuthorityEvent::PlayheadEnterRange, {true, true});
        REQUIRE(r.next == AuthorityMode::Print);
        REQUIRE(r.beginGesture);
    }
    SECTION("ARMED + 未播放 → 无")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Armed, AuthorityEvent::PlayheadEnterRange, {false, true});
        REQUIRE(r.next == AuthorityMode::Armed);
        REQUIRE_FALSE(r.beginGesture);
    }
    SECTION("PRINT → 无")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Print, AuthorityEvent::PlayheadEnterRange, {true, true});
        REQUIRE(r.next == AuthorityMode::Print);
        REQUIRE_FALSE(r.beginGesture);
    }
}

TEST_CASE("AUTH-SM-5b PlayheadLeaveRange", "[authority][sm]")
{
    SECTION("FOLLOW → 无")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Follow, AuthorityEvent::PlayheadLeaveRange, {});
        REQUIRE(r.next == AuthorityMode::Follow);
        REQUIRE_FALSE(r.endAllGestures);
    }
    SECTION("ARMED → 无")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Armed, AuthorityEvent::PlayheadLeaveRange, {});
        REQUIRE(r.next == AuthorityMode::Armed);
        REQUIRE_FALSE(r.endAllGestures);
    }
    SECTION("PRINT → endAllGestures → ARMED(播放继续)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Print, AuthorityEvent::PlayheadLeaveRange, {true, false});
        REQUIRE(r.next == AuthorityMode::Armed);
        REQUIRE(r.endAllGestures);
    }
}

TEST_CASE("AUTH-SM-6 TimelineJump(loop 回跳,epoch+1)", "[authority][sm]")
{
    SECTION("FOLLOW → 无")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Follow, AuthorityEvent::TimelineJump, {true, true});
        REQUIRE(r.next == AuthorityMode::Follow);
        REQUIRE_FALSE(r.endAllGestures);
    }
    SECTION("ARMED → 无")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Armed, AuthorityEvent::TimelineJump, {true, true});
        REQUIRE(r.next == AuthorityMode::Armed);
        REQUIRE_FALSE(r.endAllGestures);
    }
    SECTION("PRINT → endAllGestures + 区间内重新 begin(仍 PRINT)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Print, AuthorityEvent::TimelineJump, {true, true});
        REQUIRE(r.next == AuthorityMode::Print);
        REQUIRE(r.endAllGestures);
        REQUIRE(r.beginGesture);
    }
    SECTION("PRINT → 跳变落区间外则不重新 begin")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Print, AuthorityEvent::TimelineJump, {true, false});
        REQUIRE(r.next == AuthorityMode::Print);
        REQUIRE(r.endAllGestures);
        REQUIRE_FALSE(r.beginGesture);
    }
}

TEST_CASE("AUTH-SM-7 HostParamChanged(host 参数回调)", "[authority][sm]")
{
    SECTION("FOLLOW → 更新 DSP 目标 + UI")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Follow, AuthorityEvent::HostParamChanged, {});
        REQUIRE(r.next == AuthorityMode::Follow);
        REQUIRE(r.hostParamDrivesDsp);
    }
    SECTION("ARMED → 仅 UI(host echo 灰显)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Armed, AuthorityEvent::HostParamChanged, {});
        REQUIRE(r.next == AuthorityMode::Armed);
        REQUIRE_FALSE(r.hostParamDrivesDsp);
    }
    SECTION("PRINT → 仅 UI(host echo 灰显)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Print, AuthorityEvent::HostParamChanged, {});
        REQUIRE(r.next == AuthorityMode::Print);
        REQUIRE_FALSE(r.hostParamDrivesDsp);
    }
}

TEST_CASE("AUTH-SM-8 VersionSwitchRequest", "[authority][sm]")
{
    SECTION("FOLLOW → 允许")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Follow, AuthorityEvent::VersionSwitchRequest, {});
        REQUIRE(r.next == AuthorityMode::Follow);
        REQUIRE(r.versionSwitchAccepted);
    }
    SECTION("ARMED → 允许")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Armed, AuthorityEvent::VersionSwitchRequest, {});
        REQUIRE(r.next == AuthorityMode::Armed);
        REQUIRE(r.versionSwitchAccepted);
    }
    SECTION("PRINT → 拒绝")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Print, AuthorityEvent::VersionSwitchRequest, {});
        REQUIRE(r.next == AuthorityMode::Print);
        REQUIRE_FALSE(r.versionSwitchAccepted);
    }
}

TEST_CASE("AUTH-SM-9 EditorClosed", "[authority][sm]")
{
    SECTION("FOLLOW → 无")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Follow, AuthorityEvent::EditorClosed, {});
        REQUIRE(r.next == AuthorityMode::Follow);
    }
    SECTION("ARMED → 无")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Armed, AuthorityEvent::EditorClosed, {});
        REQUIRE(r.next == AuthorityMode::Armed);
    }
    SECTION("PRINT → 继续打印(状态不变)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Print, AuthorityEvent::EditorClosed, {});
        REQUIRE(r.next == AuthorityMode::Print);
        REQUIRE_FALSE(r.endAllGestures);
    }
}

TEST_CASE("AUTH-SM-10 ReleaseResources/析构", "[authority][sm]")
{
    SECTION("FOLLOW → —(无)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Follow, AuthorityEvent::ReleaseResources, {});
        REQUIRE(r.next == AuthorityMode::Follow);
        REQUIRE_FALSE(r.endAllGestures);
    }
    SECTION("ARMED → —(无)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Armed, AuthorityEvent::ReleaseResources, {});
        REQUIRE(r.next == AuthorityMode::Armed);
        REQUIRE_FALSE(r.endAllGestures);
    }
    SECTION("PRINT → GestureGuard 兜底 endAllGestures(R12)")
    {
        const AuthorityStep r = stepAuthority(AuthorityMode::Print, AuthorityEvent::ReleaseResources, {});
        REQUIRE(r.endAllGestures);
    }
}

// ============================================================================
// §2.4 统一平滑层:常规 10ms / 切换 30ms,零跳变。
// ============================================================================

TEST_CASE("AUTH-SMOOTH-1 权威切换 30ms 且零跳变(数值断言)", "[authority][smooth]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.bind();

    // 曲线 pan=+50(轨0),host pan=-50;先 FOLLOW 定在 -50。
    f.rawPan[0].store(-50.0f);
    f.curves[0] = constCurve(50.0, -3.0);
    f.bind();

    (void)f.arbiter.processBlock(false, 0.0);
    advance(f.arbiter, 8); // 已稳定在 -50

    REQUIRE(f.arbiter.panSmoother(0).getCurrentValue() == Approx(-50.0f).margin(1e-6));

    // 开 Output → 权威切换(host → engine),目标 +50,30ms 斜坡。
    (void)f.arbiter.processBlock(true, 0.0);
    REQUIRE(f.arbiter.lastBlockWasSwitch());

    // 零跳变:切换后第一样本仅移动一个 30ms 步(≈100/1440),绝非跳到 +50。
    const auto first = f.arbiter.nextSample();
    REQUIRE(std::fabs(first[0].pan - (-50.0f)) < 1.0f);

    // 10ms(480 样本)后仍在平滑(证明不是 10ms);30ms(1440 样本)后落靶 +50。
    advance(f.arbiter, 480 - 1);
    REQUIRE(f.arbiter.panSmoother(0).isSmoothing());

    advance(f.arbiter, 1440 - 480);
    REQUIRE_FALSE(f.arbiter.panSmoother(0).isSmoothing());
    REQUIRE(f.arbiter.panSmoother(0).getCurrentValue() == Approx(50.0f).margin(1e-4));
}

TEST_CASE("AUTH-SMOOTH-2 常规跟随 10ms(非切换目标变化)", "[authority][smooth]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.bind();

    // FOLLOW 首次块硬置 0。
    f.rawPan[0].store(0.0f);
    (void)f.arbiter.processBlock(false, 0.0);
    advance(f.arbiter, 4);

    // host 自动化把 pan 从 0 拨到 +20:非切换 → 10ms 常规平滑。
    f.rawPan[0].store(20.0f);
    (void)f.arbiter.processBlock(false, 0.0);
    REQUIRE_FALSE(f.arbiter.lastBlockWasSwitch());

    advance(f.arbiter, 240); // 5ms:仍在平滑
    REQUIRE(f.arbiter.panSmoother(0).isSmoothing());

    advance(f.arbiter, 240); // 累计 10ms:落靶
    REQUIRE_FALSE(f.arbiter.panSmoother(0).isSmoothing());
    REQUIRE(f.arbiter.panSmoother(0).getCurrentValue() == Approx(20.0f).margin(1e-4));
}

TEST_CASE("AUTH-SMOOTH-3 版本切换 30ms(新快照发布)", "[authority][smooth]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.curves[0] = constCurve(50.0, -3.0);
    f.bind();

    (void)f.arbiter.processBlock(true, 0.0);
    advance(f.arbiter, 8);
    REQUIRE(f.arbiter.panSmoother(0).getCurrentValue() == Approx(50.0f).margin(1e-6));

    // 版本切换:曲线真身从 +50 换到 -50。
    f.curves[0] = constCurve(-50.0, -3.0);
    f.bind();
    (void)f.arbiter.processBlock(true, 0.0);
    REQUIRE(f.arbiter.lastBlockWasSwitch());

    advance(f.arbiter, 480);
    REQUIRE(f.arbiter.panSmoother(0).isSmoothing());
    advance(f.arbiter, 1440 - 480);
    REQUIRE_FALSE(f.arbiter.panSmoother(0).isSmoothing());
    REQUIRE(f.arbiter.panSmoother(0).getCurrentValue() == Approx(-50.0f).margin(1e-4));
}

TEST_CASE("AUTH-SMOOTH-4 LinearSmoother 直接语义(零跳变/落靶/换档不重置)", "[authority][smooth]")
{
    scvb::dsp::LinearSmoother s;
    s.reset(kFs, 0.010);
    s.setCurrentAndTargetValue(-50.0f);

    s.reset(kFs, 0.030);
    s.setTargetValue(50.0f);

    const float first = s.getNextValue();
    REQUIRE(std::fabs(first - (-50.0f)) < 1.0f); // 零跳变

    int n = 1;
    while (s.isSmoothing())
    {
        s.getNextValue();
        ++n;
    }
    REQUIRE(n == 1440); // 30ms @48k
    REQUIRE(s.getCurrentValue() == Approx(50.0f).margin(1e-4));
}

TEST_CASE("AUTH-SMOOTH-5 跨 block 逐 512 样本重 arm 不截断 30ms 斜坡", "[authority][smooth]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.rawPan[0].store(-50.0f);
    f.curves[0] = constCurve(50.0, -3.0);
    f.bind();

    (void)f.arbiter.processBlock(false, 0.0);
    advance(f.arbiter, 8);

    // 权威切换 → 30ms 斜坡(1440 样本 @48k)。
    (void)f.arbiter.processBlock(true, 0.0);
    REQUIRE(f.arbiter.lastBlockWasSwitch());

    int total = 0;
    while (total < 1440)
    {
        // 模拟逐 block 调用:每 512 样本重 processBlock(目标不变,稳态非切换)。
        if (total > 0 && total % 512 == 0)
        {
            (void)f.arbiter.processBlock(true, 0.0);
            REQUIRE_FALSE(f.arbiter.lastBlockWasSwitch());
        }

        (void)f.arbiter.nextSample();
        ++total;

        // 关键:第 1439 样本必须仍在平滑 —— 若被截断成 10ms 会在 992 样本就落靶。
        if (total == 1439)
            REQUIRE(f.arbiter.panSmoother(0).isSmoothing());
    }

    REQUIRE_FALSE(f.arbiter.panSmoother(0).isSmoothing()); // 1440 落靶
    REQUIRE(f.arbiter.panSmoother(0).getCurrentValue() == Approx(50.0f).margin(1e-4));
}

// ============================================================================
// §2.3 DSP 取值仲裁:双源 + PRINT 不经过参数 + freeze。
// ============================================================================

TEST_CASE("AUTH-ARB-1 PRINT 态音频不经过参数(篡改参数不改变 DSP 输出)", "[authority][arbitrate]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.curves[0] = constCurve(30.0, -3.0);
    // host 参数被「人为篡改」成 +10/+2。
    f.rawPan[0].store(10.0f);
    f.rawVol[0].store(2.0f);
    f.bind();

    const auto t1 = f.arbiter.processBlock(true, 0.0);
    REQUIRE(t1[0].pan == Approx(30.0f).margin(1e-6)); // 曲线值,不是 10
    REQUIRE(t1[0].volDb == Approx(-3.0f).margin(1e-6)); // 曲线值,不是 2

    // 再篡改参数:DSP 目标应完全不变。
    f.rawPan[0].store(99.0f);
    f.rawVol[0].store(12.0f);
    const auto t2 = f.arbiter.processBlock(true, 0.0);
    REQUIRE(t2[0].pan == Approx(30.0f).margin(1e-6));
    REQUIRE(t2[0].volDb == Approx(-3.0f).margin(1e-6));
}

TEST_CASE("AUTH-ARB-2 FOLLOW 态 host 参数是权威", "[authority][arbitrate]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.curves[0] = constCurve(30.0, -3.0);
    f.rawPan[0].store(-40.0f);
    f.rawVol[0].store(5.0f);
    f.bind();

    const auto t = f.arbiter.processBlock(false, 0.0);
    REQUIRE(t[0].pan == Approx(-40.0f).margin(1e-6)); // 读参数,不是曲线 30
    REQUIRE(t[0].volDb == Approx(5.0f).margin(1e-6));
}

TEST_CASE("AUTH-ARB-3 freeze 冻结维度改读 host 参数(引擎权威下优先级最高)", "[authority][arbitrate]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.curves[0] = constCurve(30.0, -3.0);
    f.rawPan[0].store(-40.0f);
    f.rawVol[0].store(5.0f);
    f.bind();

    // freeze=1(冻结 pan):pan 读 host,vol 仍曲线。
    f.rawFrz[0].store(1.0f);
    auto t = f.arbiter.processBlock(true, 0.0);
    REQUIRE(t[0].pan == Approx(-40.0f).margin(1e-6));
    REQUIRE(t[0].volDb == Approx(-3.0f).margin(1e-6));

    // freeze=2(冻结 vol):pan 曲线,vol 读 host。
    f.rawFrz[0].store(2.0f);
    t = f.arbiter.processBlock(true, 0.0);
    REQUIRE(t[0].pan == Approx(30.0f).margin(1e-6));
    REQUIRE(t[0].volDb == Approx(5.0f).margin(1e-6));

    // freeze=3(全冻结):都读 host。
    f.rawFrz[0].store(3.0f);
    t = f.arbiter.processBlock(true, 0.0);
    REQUIRE(t[0].pan == Approx(-40.0f).margin(1e-6));
    REQUIRE(t[0].volDb == Approx(5.0f).margin(1e-6));
}

TEST_CASE("AUTH-ARB-4 lead_select 覆盖优先于 freeze(叠加)", "[authority][arbitrate]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.curves[0] = constCurve(30.0, -3.0);
    f.rawPan[0].store(-40.0f); // 冻结后的 host pan 值
    f.rawFrz[0].store(1.0f); // 冻结 pan
    f.rawLeadSelect.store(1.0f); // lead_select=1 → t01(索引0)
    f.bind();

    // 引擎权威 + freeze pan(→ host -40)+ lead_select 选中 t01:pan 强制居中 0,覆盖 freeze。
    const auto t = f.arbiter.processBlock(true, 0.0);
    REQUIRE(t[0].pan == Approx(0.0f).margin(1e-9)); // lead_select 覆盖优先于 freeze
    REQUIRE(t[0].volDb == Approx(-3.0f).margin(1e-6)); // vol 不受 lead_select / freeze(bit1)影响
}

TEST_CASE("AUTH-ARB-5 lead_select 越界值钳到 0..15", "[authority][arbitrate]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.rawLeadSelect.store(99.0f); // 越界(声明值域 0..15)
    f.bind();

    const auto t = f.arbiter.processBlock(true, 0.0);
    // 99 钳到 15 → t15(索引14)强制居中;若越界原样,99 不匹配任何 t+1 会无覆盖。
    REQUIRE(t[14].pan == Approx(0.0f).margin(1e-9));
    // t00(索引0)保持曲线值 -70(曲线 pan = t*10-70)。
    REQUIRE(t[0].pan == Approx(-70.0f).margin(1e-6));

    // 负越界钳到 0(无覆盖)。
    f.rawLeadSelect.store(-5.0f);
    (void)f.arbiter.processBlock(true, 0.0);
    REQUIRE(f.arbiter.lastTargets()[14].pan == Approx(70.0f).margin(1e-6));
}

// ============================================================================
// [J58] lead_select 五条验收。
// ============================================================================

TEST_CASE("AUTH-LEAD-1 0→3:t03 pan 30ms 平滑到中心,其余 14 轨逐样本不变", "[authority][lead]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.bind(); // 曲线 pan = t*10-70,全轨 distinct

    // lead=0 全遵循分析,先稳定。
    (void)f.arbiter.processBlock(true, 0.0);
    advance(f.arbiter, 8);

    std::array<float, DspArbiter::kNumTracks> settled{};
    {
        const auto v = f.arbiter.nextSample();
        for (int t = 0; t < DspArbiter::kNumTracks; ++t)
            settled[static_cast<std::size_t>(t)] = v[static_cast<std::size_t>(t)].pan;
    }

    // 0 → 3:lead_select 覆盖层,仅 t03(索引2)强制居中。
    f.rawLeadSelect.store(3.0f);
    const auto targets = f.arbiter.processBlock(true, 0.0);
    REQUIRE(targets[2].pan == Approx(0.0f).margin(1e-9));
    for (int t = 0; t < DspArbiter::kNumTracks; ++t)
    {
        if (t != 2)
            REQUIRE(targets[static_cast<std::size_t>(t)].pan ==
                    Approx(settled[static_cast<std::size_t>(t)]).margin(1e-6));
    }

    // 逐样本:其余 14 轨逐样本逐位不变;t03 平滑逼近中心。
    bool sawRamp = false;
    for (int i = 0; i < 1440; ++i)
    {
        const auto v = f.arbiter.nextSample();
        for (int t = 0; t < DspArbiter::kNumTracks; ++t)
        {
            if (t == 2)
            {
                if (std::fabs(v[2].pan - 0.0f) > 1e-4f)
                    sawRamp = true;
                continue;
            }
            REQUIRE(v[static_cast<std::size_t>(t)].pan == settled[static_cast<std::size_t>(t)]);
        }
    }
    REQUIRE(sawRamp);
    REQUIRE(f.arbiter.panSmoother(2).getCurrentValue() == Approx(0.0f).margin(1e-4));
}

TEST_CASE("AUTH-LEAD-2 3→7 直接切换(不回 0)只动这两轨", "[authority][lead]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.bind();

    (void)f.arbiter.processBlock(true, 0.0);
    advance(f.arbiter, 8);

    // 3 → 7 直接切换。
    f.rawLeadSelect.store(3.0f);
    (void)f.arbiter.processBlock(true, 0.0);
    advance(f.arbiter, 1440); // t03 平滑到中心

    std::array<float, DspArbiter::kNumTracks> baseline{};
    {
        const auto v = f.arbiter.nextSample();
        for (int t = 0; t < DspArbiter::kNumTracks; ++t)
            baseline[static_cast<std::size_t>(t)] = v[static_cast<std::size_t>(t)].pan;
    }
    REQUIRE(baseline[2] == Approx(0.0f).margin(1e-4)); // t03 已居中

    // 直接 3→7:仅 t03 回曲线值、t07(索引6)去中心。
    f.rawLeadSelect.store(7.0f);
    const auto targets = f.arbiter.processBlock(true, 0.0);
    REQUIRE(targets[6].pan == Approx(0.0f).margin(1e-9));
    const double t03Curve = static_cast<double>(2) * 10.0 - 70.0; // -50
    REQUIRE(targets[2].pan == Approx(t03Curve).margin(1e-6));

    for (int t = 0; t < DspArbiter::kNumTracks; ++t)
    {
        if (t == 2 || t == 6)
            continue;
        REQUIRE(targets[static_cast<std::size_t>(t)].pan == Approx(baseline[static_cast<std::size_t>(t)]).margin(1e-6));
    }

    // 平滑到新目标(直接切换,不回 0)。
    advance(f.arbiter, 1440);
    REQUIRE(f.arbiter.panSmoother(2).getCurrentValue() == Approx(t03Curve).margin(1e-4));
    REQUIRE(f.arbiter.panSmoother(6).getCurrentValue() == Approx(0.0f).margin(1e-4));
}

TEST_CASE("AUTH-LEAD-3 覆盖期间曲线真身逐字节不变(覆盖不落盘)", "[authority][lead]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.curves[2] = constCurve(-50.0, -2.5);
    f.bind();

    // 覆盖前对曲线真身采样快照。
    std::vector<double> before;
    for (int k = 0; k < 1000; ++k)
    {
        const double t = static_cast<double>(k) / kFs;
        before.push_back(f.curves[2].panAt(t));
        before.push_back(f.curves[2].volAt(t));
    }

    // lead_select=3 覆盖运行(PRINT 态)。
    f.rawLeadSelect.store(3.0f);
    (void)f.arbiter.processBlock(true, 0.0);
    advance(f.arbiter, 1500);

    // 覆盖后曲线真身逐字节不变。
    std::size_t idx = 0;
    for (int k = 0; k < 1000; ++k)
    {
        const double t = static_cast<double>(k) / kFs;
        REQUIRE(f.curves[2].panAt(t) == before[idx++]);
        REQUIRE(f.curves[2].volAt(t) == before[idx++]);
    }
}

TEST_CASE("AUTH-LEAD-4 lead_select=3 且 lead_vol_exempt=false → vol 照常(反向断言)", "[authority][lead]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.curves[2] = constCurve(-50.0, -2.5);
    f.bind();

    // lead=0:vol 目标 = 曲线值。
    (void)f.arbiter.processBlock(true, 0.0);
    REQUIRE(f.arbiter.lastTargets()[2].volDb == Approx(-2.5f).margin(1e-6));

    // lead=3:vol 不受覆盖层影响(不被豁免、不被强制)。
    f.rawLeadSelect.store(3.0f);
    (void)f.arbiter.processBlock(true, 0.0);
    REQUIRE(f.arbiter.lastTargets()[2].volDb == Approx(-2.5f).margin(1e-6));
    // 反向断言:vol 既非 0、也非被 lead_select 改写。
    REQUIRE(std::fabs(f.arbiter.lastTargets()[2].volDb) > 0.1f);
}

TEST_CASE("AUTH-LEAD-5 PRINT 态覆盖层生效但不额外产生 gesture(打印头写曲线值)", "[authority][lead]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.curves[2] = constCurve(-50.0, -2.5);
    f.bind();

    // PRINT(engine authority)+ lead_select=3:DSP 目标被覆盖到中心,但曲线真身仍是 -50。
    f.rawLeadSelect.store(3.0f);
    (void)f.arbiter.processBlock(true, 0.0);

    REQUIRE(f.arbiter.lastTargets()[2].pan == Approx(0.0f).margin(1e-9)); // DSP 覆盖生效
    // 打印头采样的是曲线真身(仍 -50),不是覆盖值 0 —— 覆盖不混进打印。
    REQUIRE(f.curves[2].panAt(0.0) == Approx(-50.0).margin(1e-9));
}

// ============================================================================
// 并发冒烟:消息线程换快照 × 音频线程读,每 block 快照自洽(不撕裂、不崩)。
// ============================================================================

TEST_CASE("AUTH-SMOKE-1 原子快照并发发布/读取不撕裂", "[authority][concurrency]")
{
    constexpr int kIterations = 4000;

    // 冻结值池:值 = 池索引,发布后不再改(避免值级数据竞争干扰快照自洽判定)。
    std::vector<std::atomic<float>> values(static_cast<std::size_t>(kIterations));
    for (int i = 0; i < kIterations; ++i)
        values[static_cast<std::size_t>(i)].store(static_cast<float>(i));

    DspArbiter arbiter;
    arbiter.prepare(kFs);

    std::atomic<bool> start{false};
    std::atomic<bool> torn{false};
    std::vector<std::unique_ptr<DspArbiter::Snapshot>> pool; // 仅发布线程访问

    std::thread publisher([&] {
        while (!start.load(std::memory_order_acquire))
        {
        }
        for (int i = 0; i < kIterations; ++i)
        {
            auto snap = std::make_unique<DspArbiter::Snapshot>();
            for (int t = 0; t < DspArbiter::kNumTracks; ++t)
                snap->sources[static_cast<std::size_t>(t)].rawPan = &values[static_cast<std::size_t>(i)];
            arbiter.publish(snap.get()); // release-store
            pool.push_back(std::move(snap)); // 保活
        }
    });

    std::thread reader([&] {
        while (!start.load(std::memory_order_acquire))
        {
        }
        for (int i = 0; i < kIterations; ++i)
        {
            (void)arbiter.processBlock(false, 0.0); // FOLLOW:target = rawPan 值
            (void)arbiter.nextSample();
            const auto& t = arbiter.lastTargets();
            // 自洽:本块 15 轨须来自同一快照 → pan 全等;若撕裂会混入不同代值。
            for (int k = 1; k < DspArbiter::kNumTracks; ++k)
            {
                if (t[static_cast<std::size_t>(k)].pan != t[0].pan)
                    torn.store(true, std::memory_order_relaxed);
            }
        }
    });

    start.store(true, std::memory_order_release);
    publisher.join();
    reader.join();

    REQUIRE_FALSE(torn.load());
}
