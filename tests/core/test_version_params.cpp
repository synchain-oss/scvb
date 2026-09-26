// SPDX-License-Identifier: GPL-3.0-or-later
// test_version_params —— T18 JUCE 参数层:OutputAuthority 集成 VersionStore + UndoManager。
// 断言:§5.4 交互语义表 2×2 逐格、version_active 钳制、name 往返(save→load)与重命名撤销。
// 版本复制(03 §5.3)的用例不在这里:本类那份曲线层 copyVersion 零生产调用点,已随 [SL-510] / [J109] 删除;
// 生产路径(processor 的 CRVS 事务)的「零参数写入 / 零 gesture / PRINT 拒绝」见
// tests/host/test_host_harness.cpp 的 `HOST SL-510` 与 `HOST SL-484`,VersionStore 层的复制语义见
// tests/core/test_version.cpp 的 VERSION-COPY-1..6。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "OutputAuthority.h"
#include "OutputParams.h"
#include "engine/AuthorityMode.h"
#include "engine/CurveEvaluator.h"
#include "engine/VersionStore.h"

using Catch::Approx;

namespace
{

constexpr double kFs = 48000.0;

// 最小 AudioProcessor:仅用于承载 APVTS(同 test_authority_params.cpp / test_params_golden.cpp)。
struct VersionTestProcessor final : juce::AudioProcessor
{
    VersionTestProcessor()
        : juce::AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "VersionTest"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};

scvb::CurveEvaluator constCurve(double pan, double volDb)
{
    scvb::CurveEvaluator ev;
    ev.build({scvb::CurveSegment{0.0, 1000.0, pan, volDb}}, scvb::TransitionConfig{});
    return ev;
}

// 一次构造:APVTS(123 参数)+ ParamHandles + OutputAuthority(含 VersionStore/UndoManager)。
struct VersionParamsFixture
{
    VersionTestProcessor proc;
    juce::AudioProcessorValueTreeState apvts;
    scvb::params::ParamHandles handles;
    scvb::output::OutputAuthority auth;

    VersionParamsFixture()
        : apvts(proc, nullptr, "PARAMETERS", scvb::params::makeOutputLayout()),
          handles(scvb::params::collectParamHandles(apvts))
    {
        auth.prepare(kFs, handles);
    }
};

} // namespace

// ============================================================================
// §5.4 交互语义表逐格用例(2 版本 × follow/print 两态;行数变、语义不变)
// 注:「引擎是否写该车道」列是 T17 打印器职责,本卡断言「DSP 是否采用」列。
// ============================================================================

TEST_CASE("VERSION-54-1 Va×FOLLOW:host 参数 → DSP", "[version][interact]")
{
    VersionParamsFixture f;

    auto ev1 = constCurve(30.0, -3.0);
    auto ev2 = constCurve(-50.0, -2.0);
    f.auth.setCurve(1, 0, &ev1);
    f.auth.setCurve(2, 0, &ev2);
    f.handles.rawPan[0][0]->store(10.0f);
    f.handles.rawVol[0][0]->store(1.0f);

    f.auth.setVersionActive(1); // Va=1
    const auto t = f.auth.processBlock(false, 0.0); // FOLLOW
    REQUIRE(t[0].pan == Approx(10.0f).margin(1e-6)); // host 参数,非曲线 30
    REQUIRE(t[0].volDb == Approx(1.0f).margin(1e-6));
}

TEST_CASE("VERSION-54-2 Va×PRINT:DSP 用曲线真身(不经过参数)", "[version][interact]")
{
    VersionParamsFixture f;

    auto ev1 = constCurve(30.0, -3.0);
    f.auth.setCurve(1, 0, &ev1);
    f.handles.rawPan[0][0]->store(10.0f); // 参数被篡改,应被忽略

    f.auth.setVersionActive(1); // Va=1
    const auto t = f.auth.processBlock(true, 0.0); // PRINT(引擎权威)
    REQUIRE(t[0].pan == Approx(30.0f).margin(1e-6)); // 曲线真身,非参数 10
}

TEST_CASE("VERSION-54-3 Vx×FOLLOW:DSP 只读 Va,静默存入 Vx 参数", "[version][interact]")
{
    VersionParamsFixture f;

    auto ev1 = constCurve(30.0, -3.0);
    auto ev2 = constCurve(-50.0, -2.0);
    f.auth.setCurve(1, 0, &ev1);
    f.auth.setCurve(2, 0, &ev2);
    f.handles.rawPan[0][0]->store(10.0f); // Va=1 参数
    f.handles.rawPan[1][0]->store(-40.0f); // Vx=2 参数(被忽略)

    f.auth.setVersionActive(1); // Va=1,Vx=2 非活跃
    const auto t = f.auth.processBlock(false, 0.0); // FOLLOW
    REQUIRE(t[0].pan == Approx(10.0f).margin(1e-6)); // 读 Va 参数,非 Vx 的 -40
}

TEST_CASE("VERSION-54-4 Vx×PRINT:DSP 物理隔离(只读 Va 曲线)", "[version][interact]")
{
    VersionParamsFixture f;

    auto ev1 = constCurve(30.0, -3.0);
    auto ev2 = constCurve(-50.0, -2.0);
    f.auth.setCurve(1, 0, &ev1);
    f.auth.setCurve(2, 0, &ev2);

    f.auth.setVersionActive(1); // Va=1,Vx=2 非活跃
    const auto t = f.auth.processBlock(true, 0.0); // PRINT
    REQUIRE(t[0].pan == Approx(30.0f).margin(1e-6)); // 读 Va 曲线,非 Vx 的 -50
}

// ============================================================================
// version_active 值域 1..2(经 OutputAuthority 钳制 + warning)
// ============================================================================

TEST_CASE("VERSION-ACTIVE-CLAMP OutputAuthority 越界钳制 + warning", "[version][active]")
{
    VersionParamsFixture f;

    f.auth.setVersionActive(99);
    REQUIRE(f.auth.versionActive() == 2); // 钳到 2,非取模
    REQUIRE(f.auth.warningCount() == 1);

    f.auth.setVersionActive(-7);
    REQUIRE(f.auth.versionActive() == 1); // 钳到 1
    REQUIRE(f.auth.warningCount() == 2);
}

// ============================================================================
// name 字段:往返一致(save→load)、超长/空名边界
// ============================================================================

TEST_CASE("VERSION-NAME-ROUNDTRIP save→load 保名", "[version][name]")
{
    VersionParamsFixture f;

    f.auth.setVersionName(1, "Lead");
    f.auth.setVersionName(2, "Double");
    f.auth.setVersionActive(2);

    const juce::ValueTree state = f.auth.toState();

    // 新权威实例 load 后名称与 active 一致。
    VersionParamsFixture g;
    g.auth.fromState(state);

    REQUIRE(g.auth.versionName(1) == "Lead");
    REQUIRE(g.auth.versionName(2) == "Double");
    REQUIRE(g.auth.versionActive() == 2);
}

TEST_CASE("VERSION-RENAME-UNDO 重命名可撤销", "[version][name]")
{
    VersionParamsFixture f;

    REQUIRE(f.auth.setVersionName(1, "Lead") == scvb::engine::SetNameResult::Ok);
    REQUIRE(f.auth.versionName(1) == "Lead");

    REQUIRE(f.auth.undoManager().canUndo());
    REQUIRE(f.auth.undoManager().undo());
    REQUIRE(f.auth.versionName(1) == "V1"); // 撤销回默认名

    REQUIRE(f.auth.undoManager().redo());
    REQUIRE(f.auth.versionName(1) == "Lead"); // 重做恢复
}

TEST_CASE("VERSION-NAME-BOUNDARY 空值回落默认 V{n}、超长截断", "[version][name]")
{
    VersionParamsFixture f;

    // 空值 → 默认 V1。
    REQUIRE(f.auth.setVersionName(1, "") == scvb::engine::SetNameResult::EmptyFellBack);
    REQUIRE(f.auth.versionName(1) == "V1");

    // 超长 → 截断到 16。
    REQUIRE(f.auth.setVersionName(1, "abcdefghijklmnopqrst") == scvb::engine::SetNameResult::Truncated);
    REQUIRE(f.auth.versionName(1) == "abcdefghijklmnop");
}
