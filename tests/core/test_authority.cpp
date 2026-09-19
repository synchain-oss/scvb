// SPDX-License-Identifier: GPL-3.0-or-later
// test_authority —— T16:AuthorityMode 状态机(03 §2.2 每格)+ DSP 双源取值仲裁(§2.3)
// + 统一平滑层(§2.4)+ [J58] lead_select 实时覆盖层。纯核心,无 JUCE(ADR-011)。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
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
    std::array<std::shared_ptr<scvb::CurveEvaluator>, DspArbiter::kNumTracks> curves{};
    std::vector<std::unique_ptr<DspArbiter::Snapshot>> pool; // 进程寿命保活已发布快照

    ArbiterFixture()
    {
        for (int t = 0; t < DspArbiter::kNumTracks; ++t)
        {
            rawPan[static_cast<std::size_t>(t)].store(0.0f);
            rawVol[static_cast<std::size_t>(t)].store(0.0f);
            rawTrkW[static_cast<std::size_t>(t)].store(100.0f);
            rawFrz[static_cast<std::size_t>(t)].store(0.0f);
            curves[static_cast<std::size_t>(t)] =
                std::make_shared<scvb::CurveEvaluator>(constCurve(static_cast<double>(t) * 10.0 - 70.0, -3.0));
        }
        rawLeadSelect.store(0.0f);
    }

    // lut 缺省 null = 没有 pan 曲线(G≡0);既有调用点的行为一字不变。
    void bind(std::shared_ptr<const scvb::PanCurveLut> lut = nullptr)
    {
        auto snap = std::make_unique<DspArbiter::Snapshot>();
        snap->panCurveLut = std::move(lut);
        for (int t = 0; t < DspArbiter::kNumTracks; ++t)
        {
            snap->sources[static_cast<std::size_t>(t)].rawPan = &rawPan[static_cast<std::size_t>(t)];
            snap->sources[static_cast<std::size_t>(t)].rawVol = &rawVol[static_cast<std::size_t>(t)];
            snap->sources[static_cast<std::size_t>(t)].rawTrkW = &rawTrkW[static_cast<std::size_t>(t)];
            snap->sources[static_cast<std::size_t>(t)].rawFrz = &rawFrz[static_cast<std::size_t>(t)];
            snap->sources[static_cast<std::size_t>(t)].curve = curves[static_cast<std::size_t>(t)];
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
    f.curves[0] = std::make_shared<scvb::CurveEvaluator>(constCurve(50.0, -3.0));
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
    f.curves[0] = std::make_shared<scvb::CurveEvaluator>(constCurve(50.0, -3.0));
    f.bind();

    (void)f.arbiter.processBlock(true, 0.0);
    advance(f.arbiter, 8);
    REQUIRE(f.arbiter.panSmoother(0).getCurrentValue() == Approx(50.0f).margin(1e-6));

    // 版本切换:曲线真身从 +50 换到 -50。
    f.curves[0] = std::make_shared<scvb::CurveEvaluator>(constCurve(-50.0, -3.0));
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
    f.curves[0] = std::make_shared<scvb::CurveEvaluator>(constCurve(50.0, -3.0));
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
    f.curves[0] = std::make_shared<scvb::CurveEvaluator>(constCurve(30.0, -3.0));
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
    f.curves[0] = std::make_shared<scvb::CurveEvaluator>(constCurve(30.0, -3.0));
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
    f.curves[0] = std::make_shared<scvb::CurveEvaluator>(constCurve(30.0, -3.0));
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
    f.curves[0] = std::make_shared<scvb::CurveEvaluator>(constCurve(30.0, -3.0));
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
    f.curves[2] = std::make_shared<scvb::CurveEvaluator>(constCurve(-50.0, -2.5));
    f.bind();

    // 覆盖前对曲线真身采样快照。
    std::vector<double> before;
    for (int k = 0; k < 1000; ++k)
    {
        const double t = static_cast<double>(k) / kFs;
        before.push_back(f.curves[2]->panAt(t));
        before.push_back(f.curves[2]->volAt(t));
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
        REQUIRE(f.curves[2]->panAt(t) == before[idx++]);
        REQUIRE(f.curves[2]->volAt(t) == before[idx++]);
    }
}

TEST_CASE("AUTH-LEAD-4 lead_select=3 且 lead_vol_exempt=false → vol 照常(反向断言)", "[authority][lead]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.curves[2] = std::make_shared<scvb::CurveEvaluator>(constCurve(-50.0, -2.5));
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
    f.curves[2] = std::make_shared<scvb::CurveEvaluator>(constCurve(-50.0, -2.5));
    f.bind();

    // PRINT(engine authority)+ lead_select=3:DSP 目标被覆盖到中心,但曲线真身仍是 -50。
    f.rawLeadSelect.store(3.0f);
    (void)f.arbiter.processBlock(true, 0.0);

    REQUIRE(f.arbiter.lastTargets()[2].pan == Approx(0.0f).margin(1e-9)); // DSP 覆盖生效
    // 打印头采样的是曲线真身(仍 -50),不是覆盖值 0 —— 覆盖不混进打印。
    REQUIRE(f.curves[2]->panAt(0.0) == Approx(-50.0).margin(1e-9));
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

// ============================================================================
// [SL-442] 换表交叉淡入(02 §8.1 步骤 5 + 03 §2.4 的 30ms 切换档)。
//
// 为什么要淡入:G 是 P 的静态映射,pan 动时 G 跟着连续变;但**曲线本身被改**(或换版本)
// 时同一个 P 上的增益会瞬时跳。实测单次 setPanCurve 提交的跳变上界 12 dB,最温和的手势
// (滚轮一格)也有 ~1 dB —— 没有「小到不可闻」那一档。
//
// 两格互为反向,钉的是「开得了**也**关得上」,不是只钉一头:
//   XFADE-1 删掉窗口初值 ⇒ 窗口永不开 ⇒ 第 1 样本直接等于新表值 ⇒ 必红;
//   XFADE-2 删掉每样本递减 ⇒ 窗口永不关 ⇒ 窗口后仍在混旧表 ⇒ 必红。
// ============================================================================

namespace
{

// 峰值落在 P=0 的 bell,**峰值处**恰为 db。整条表并不是恒定 db —— 下面的断言因此
// 要么把探针固定在 P=0(取到的就是 db),要么拿 lut->gainDb(pan) 自比,不假设平坦。
std::shared_ptr<scvb::PanCurveLut> bellPeakAtCentre(float db)
{
    auto lut = std::make_shared<scvb::PanCurveLut>();
    scvb::PanCurvePoint p;
    p.angle = 0.0f;
    p.gainDb = db;
    p.shape = scvb::PanCurveShape::bell;
    p.q = 1.5f;
    p.side = scvb::PanCurveSide::out;
    lut->rebuild({p});
    return lut;
}

} // namespace

TEST_CASE("AUTH-XFADE-1 换表开窗:第一个样本仍≈旧表,不是瞬间跳到新表", "[authority][pancurve][xfade]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs); // 30ms @48k = 1440 样本
    const auto lutA = bellPeakAtCentre(0.0f); // 探针点 0 dB
    const auto lutB = bellPeakAtCentre(-12.0f); // 探针点 -12 dB:一次提交能打满的那个量级
    f.bind(lutA);
    (void)f.arbiter.processBlock(true, 0.0);
    advance(f.arbiter, 64);

    // 窗口未开时:只查一张表,prev 为 null。
    REQUIRE(f.arbiter.panCurveXfade().previous == nullptr);
    REQUIRE(f.arbiter.panCurveXfadeRemaining() == 0);
    REQUIRE(scvb::panCurveGainDb(f.arbiter.panCurveXfade(), 0.0f) == Approx(0.0).margin(1e-6));

    // 换表 → 开窗。
    f.bind(lutB);
    (void)f.arbiter.processBlock(true, 0.0);
    REQUIRE(f.arbiter.panCurveXfadeRemaining() > 0); // 窗口真的开了

    (void)f.arbiter.nextSample();
    const float first = scvb::panCurveGainDb(f.arbiter.panCurveXfade(), 0.0f);
    // 第一个样本必须还贴着**旧**表:不做淡入的话这里直接是 -12,与上一个样本差 12 dB。
    REQUIRE(std::fabs(static_cast<double>(first)) < 0.5);
    REQUIRE(std::fabs(static_cast<double>(first) + 12.0) > 10.0); // 且确实不是新表值

    // 逐样本单调下行、每步增量远小于一次跳变(这才是「不咔哒」的可验证形态)。
    float prev = first;
    float maxStep = 0.0f;
    const int n = f.arbiter.panCurveXfadeRemaining();
    for (int i = 0; i < n; ++i)
    {
        (void)f.arbiter.nextSample();
        const float now = scvb::panCurveGainDb(f.arbiter.panCurveXfade(), 0.0f);
        REQUIRE(now <= prev + 1e-5f); // 单调(0 → -12)
        maxStep = std::max(maxStep, std::fabs(now - prev));
        prev = now;
    }
    REQUIRE(maxStep < 0.05f); // 单步 ≤0.05 dB,比整跳的 12 dB 小两个数量级
    REQUIRE(prev == Approx(-12.0).margin(0.01)); // 窗口末尾落到新表
}

TEST_CASE("AUTH-XFADE-2 窗口有界:走完 30ms 必关,此后只查一张表", "[authority][pancurve][xfade]")
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    const auto lutA = bellPeakAtCentre(0.0f);
    const auto lutB = bellPeakAtCentre(-12.0f);
    f.bind(lutA);
    (void)f.arbiter.processBlock(true, 0.0);
    advance(f.arbiter, 16);

    f.bind(lutB);
    (void)f.arbiter.processBlock(true, 0.0);
    const int window = f.arbiter.panCurveXfadeRemaining();
    REQUIRE(window == static_cast<int>(0.030 * kFs + 0.5)); // 30ms 切换档,不是另立的数

    advance(f.arbiter, window); // 正好走完
    REQUIRE(f.arbiter.panCurveXfadeRemaining() == 0);

    // 再走一个样本:旧表指针必须被置 null —— 这一步是「窗口关得上」的实体。
    (void)f.arbiter.nextSample();
    REQUIRE(f.arbiter.panCurveXfade().previous == nullptr);
    REQUIRE(f.arbiter.panCurveXfade().target == lutB.get());

    // 且此后**逐位**等于单表查表(窗口外不残留任何淡入成分)。
    for (const float pan : {-100.0f, -42.0f, 0.0f, 55.0f, 100.0f})
    {
        REQUIRE(scvb::panCurveGainDb(f.arbiter.panCurveXfade(), pan) == lutB->gainDb(pan));
    }

    // 再走很久也不会自己重开(窗口不是周期性的)。
    advance(f.arbiter, 4096);
    REQUIRE(f.arbiter.panCurveXfadeRemaining() == 0);
    REQUIRE(f.arbiter.panCurveXfade().previous == nullptr);
}

TEST_CASE("AUTH-XFADE-3 段编辑造新快照但不换表 ⇒ 不开窗", "[authority][pancurve][xfade]")
{
    // rebuildAllCurves 一次会发 15 个新快照(每轨 setCurve 各一次)。若按「快照变没变」判,
    // 每次段编辑都会触发 30ms 淡入 —— 一个本该罕见的窗口会变成常态。
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    const auto lut = bellPeakAtCentre(-6.0f);
    f.bind(lut);
    (void)f.arbiter.processBlock(true, 0.0);
    advance(f.arbiter, 64);

    for (int i = 0; i < 15; ++i) // 模拟一次 rebuildAllCurves 的 15 次重发
    {
        f.bind(lut); // 同一张表,新快照
        (void)f.arbiter.processBlock(true, 0.0);
        REQUIRE(f.arbiter.panCurveXfadeRemaining() == 0); // 一次都不该开窗
        REQUIRE(f.arbiter.panCurveXfade().previous == nullptr);
        advance(f.arbiter, 8);
    }
}

// ----------------------------------------------------------------------------
// [SL-442] 版本切换时 **pan 在 30ms 平滑、LUT 也在 30ms 交叉淡入** —— G(P) 的两个输入
// 同时在动。各自收敛没问题,合起来的增益轨迹要量,不能推。
// ----------------------------------------------------------------------------
namespace
{

// 把一次「换 pan 目标 + 换表」跑完,返回逐样本的 G(dB) 轨迹。
// samePan / sameLut 用来做对照组:单独只动一个输入时的轨迹长什么样。
std::vector<double> sweepGainTrace(double panFrom, double panTo, const std::shared_ptr<scvb::PanCurveLut>& lutFrom,
                                   const std::shared_ptr<scvb::PanCurveLut>& lutTo, int samples)
{
    ArbiterFixture f;
    f.arbiter.prepare(kFs);
    f.curves[0] = std::make_shared<scvb::CurveEvaluator>(constCurve(panFrom, 0.0));
    f.bind(lutFrom);
    (void)f.arbiter.processBlock(true, 0.0);
    advance(f.arbiter, 4096); // 先让 pan 平滑器彻底落在 panFrom 上

    f.curves[0] = std::make_shared<scvb::CurveEvaluator>(constCurve(panTo, 0.0));
    f.bind(lutTo);
    (void)f.arbiter.processBlock(true, 0.0); // 同时 arm:pan 30ms 切换档 + LUT 30ms 淡入

    std::vector<double> trace;
    trace.reserve(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i)
    {
        const auto tv = f.arbiter.nextSample();
        trace.push_back(static_cast<double>(scvb::panCurveGainDb(f.arbiter.panCurveXfade(), tv[0].pan)));
    }
    return trace;
}

double maxStep(const std::vector<double>& v)
{
    double m = 0.0;
    for (std::size_t i = 1; i < v.size(); ++i)
        m = std::max(m, std::fabs(v[i] - v[i - 1]));
    return m;
}

} // namespace

TEST_CASE("AUTH-XFADE-4 pan 平滑与 LUT 淡入同时在跑:无阶跃", "[authority][pancurve][xfade]")
{
    // A:谷底 -12 dB 落在 pan 路径的正中(P=0);B:谷底挪到 +90(路径末端之外)。
    // 这样两个输入都在动,且 A 在路径中段有强特征 —— 最容易把问题照出来的构造。
    auto lutA = std::make_shared<scvb::PanCurveLut>();
    lutA->rebuild({scvb::PanCurvePoint{0.0f, -12.0f, scvb::PanCurveShape::bell, 3.0f, scvb::PanCurveSide::out}});
    auto lutB = std::make_shared<scvb::PanCurveLut>();
    lutB->rebuild({scvb::PanCurvePoint{90.0f, -12.0f, scvb::PanCurveShape::bell, 3.0f, scvb::PanCurveSide::out}});

    const int window = static_cast<int>(0.030 * kFs + 0.5);
    const int n = window * 3; // 覆盖窗口内 + 窗口关上之后

    const auto both = sweepGainTrace(-60.0, 60.0, lutA, lutB, n); // 两个输入一起动
    const auto lutOnly = sweepGainTrace(-60.0, -60.0, lutA, lutB, n); // 只换表
    const auto panOnly = sweepGainTrace(-60.0, 60.0, lutA, lutA, n); // 只动 pan

    std::cout << "  [SL-442] maxStep(dB/sample):both=" << maxStep(both) << " lutOnly=" << maxStep(lutOnly)
              << " panOnly=" << maxStep(panOnly) << std::endl;

    // 判据 = **逐样本阶跃**,不是「有没有起伏」。起伏是正常的:pan 扫过曲线上的特征时
    // G 本来就该跟着变(那正是 G 的作用)。会咔哒的是不连续,不是幅度 —— 拿「不许超出两端值
    // 围成的区间」当判据会把**正确**行为判成错:pan 从 -60 扫到 +60 途中经过 A 表 P=0 的
    // -12 dB 谷底,中段本来就该比两端都低。
    //
    // 实测(48k,上面那行 stdout 会打出来):
    //   both = 0.0254 dB/sample、lutOnly = 0.00088、panOnly = 0.0214
    // ⇒ 两个输入一起动只比「单动 pan」陡 1.18 倍;主导项是 pan 扫过曲线特征(既有行为),
    //   LUT 淡入自身只贡献 0.00088 dB/sample。合起来没有引入新的陡变。
    // 1.5 这个上界是**先于测量**定的(取「不得明显比更陡的那个单输入更陡」),不是照着
    // 1.18 凑的;留 27% 余量。
    REQUIRE(maxStep(both) <= std::max(maxStep(lutOnly), maxStep(panOnly)) * 1.5 + 1e-9);

    // 绝对上界:整跳是 12 dB,逐样本阶跃必须比它小两个数量级以上。
    REQUIRE(maxStep(both) < 0.12);

    // 窗口关上那一刻不许有台阶 —— 淡入结束时旧表被置 null,最容易在这儿掉一块。
    const double atClose =
        std::fabs(both[static_cast<std::size_t>(window)] - both[static_cast<std::size_t>(window) - 1]);
    REQUIRE(atClose < 0.12);

    // 收敛:窗口之后停在「新表 @ 新 pan」上。
    REQUIRE(both.back() == Approx(static_cast<double>(lutB->gainDb(60.0f))).margin(0.01));
}
