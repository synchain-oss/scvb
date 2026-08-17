// SPDX-License-Identifier: GPL-3.0-or-later
// test_authority_params —— T16 JUCE 参数层:OutputAuthority 把 T15 的 ParamHandles(raw atomic)
// 绑定到 DspArbiter;断言 PRINT 态篡改真实 JUCE 参数不改变 DSP 输出、lead_select 覆盖、版本切换重绑。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cmath>

#include "OutputAuthority.h"
#include "OutputParams.h"
#include "engine/CurveEvaluator.h"

using Catch::Approx;

namespace
{

constexpr double kFs = 48000.0;

// 最小 AudioProcessor:仅用于承载 APVTS(同 test_params_golden.cpp)。
struct AuthorityTestProcessor final : juce::AudioProcessor
{
    AuthorityTestProcessor()
        : juce::AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "AuthorityTest"; }
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

// 一次构造:APVTS(123 参数)+ ParamHandles + OutputAuthority。
struct AuthorityParamsFixture
{
    AuthorityTestProcessor proc;
    juce::AudioProcessorValueTreeState apvts;
    scvb::params::ParamHandles handles;
    scvb::output::OutputAuthority auth;

    AuthorityParamsFixture()
        : apvts(proc, nullptr, "PARAMETERS", scvb::params::makeOutputLayout()),
          handles(scvb::params::collectParamHandles(apvts))
    {
        auth.prepare(kFs, handles);
    }
};

} // namespace

TEST_CASE("AUTH-PARAMS-1 PRINT 态篡改真实 JUCE 参数不改变 DSP 输出", "[authority][params]")
{
    AuthorityParamsFixture f;

    // 版本 1 轨 0:曲线真身 pan=+30 / vol=-3。
    auto ev = constCurve(30.0, -3.0);
    f.auth.setCurve(1, 0, &ev);

    // 宿主把 v1_t01_pan/vol 参数「篡改」为 +10/+2(写 raw atomic = APVTS 实时值)。
    f.handles.rawPan[0][0]->store(10.0f);
    f.handles.rawVol[0][0]->store(2.0f);

    const auto t1 = f.auth.processBlock(true, 0.0); // PRINT:引擎权威
    REQUIRE(t1[0].pan == Approx(30.0f).margin(1e-6)); // 曲线值,不是 10
    REQUIRE(t1[0].volDb == Approx(-3.0f).margin(1e-6)); // 曲线值,不是 2

    // 再次篡改:DSP 目标不变。
    f.handles.rawPan[0][0]->store(99.0f);
    f.handles.rawVol[0][0]->store(12.0f);
    const auto t2 = f.auth.processBlock(true, 0.0);
    REQUIRE(t2[0].pan == Approx(30.0f).margin(1e-6));
    REQUIRE(t2[0].volDb == Approx(-3.0f).margin(1e-6));
}

TEST_CASE("AUTH-PARAMS-2 lead_select 经真实 JUCE 参数强制 t03 居中", "[authority][params]")
{
    AuthorityParamsFixture f;

    // 15 轨曲线 distinct pan;先稳定。
    std::array<scvb::CurveEvaluator, 15> curves{};
    for (int t = 0; t < 15; ++t)
    {
        curves[static_cast<std::size_t>(t)] = constCurve(static_cast<double>(t) * 10.0 - 70.0, -3.0);
        f.auth.setCurve(1, t, &curves[static_cast<std::size_t>(t)]);
    }

    (void)f.auth.processBlock(true, 0.0);
    for (int i = 0; i < 8; ++i)
        (void)f.auth.nextSample();

    // 真实参数:lead_select = 3。
    f.handles.rawLeadSelect->store(3.0f);
    const auto targets = f.auth.processBlock(true, 0.0);

    REQUIRE(targets[2].pan == Approx(0.0f).margin(1e-9));
    for (int t = 0; t < 15; ++t)
    {
        if (t == 2)
            continue;
        REQUIRE(targets[static_cast<std::size_t>(t)].pan == Approx(static_cast<double>(t) * 10.0 - 70.0).margin(1e-6));
    }
}

TEST_CASE("AUTH-PARAMS-3 版本切换重绑 raw atomic 与曲线", "[authority][params]")
{
    AuthorityParamsFixture f;

    // 版本 1 轨 0 曲线 +50;版本 2 轨 0 曲线 -50。
    auto ev1 = constCurve(50.0, -1.0);
    auto ev2 = constCurve(-50.0, -2.0);
    f.auth.setCurve(1, 0, &ev1);
    f.auth.setCurve(2, 0, &ev2);

    (void)f.auth.processBlock(true, 0.0);
    REQUIRE(f.auth.arbiter().lastTargets()[0].pan == Approx(50.0f).margin(1e-6));

    f.auth.setVersionActive(2);
    REQUIRE(f.auth.versionActive() == 2);
    (void)f.auth.processBlock(true, 0.0);
    REQUIRE(f.auth.arbiter().lastTargets()[0].pan == Approx(-50.0f).margin(1e-6));

    // FOLLOW 下切回版本 1:读版本 1 的 host 参数。
    f.handles.rawPan[0][0]->store(25.0f); // v1 参数
    f.handles.rawPan[1][0]->store(-25.0f); // v2 参数
    f.auth.setVersionActive(1);
    (void)f.auth.processBlock(false, 0.0);
    REQUIRE(f.auth.arbiter().lastTargets()[0].pan == Approx(25.0f).margin(1e-6));

    f.auth.setVersionActive(2);
    (void)f.auth.processBlock(false, 0.0);
    REQUIRE(f.auth.arbiter().lastTargets()[0].pan == Approx(-25.0f).margin(1e-6));
}

TEST_CASE("AUTH-PARAMS-4 freeze 参数经真实 JUCE 参数冻结维度", "[authority][params]")
{
    AuthorityParamsFixture f;

    auto ev = constCurve(30.0, -3.0);
    f.auth.setCurve(1, 0, &ev);

    f.handles.rawPan[0][0]->store(-40.0f);
    f.handles.rawVol[0][0]->store(5.0f);

    // freeze=1(冻结 pan):引擎权威下 pan 读 host。
    f.handles.rawFrz[0][0]->store(1.0f);
    auto t = f.auth.processBlock(true, 0.0);
    REQUIRE(t[0].pan == Approx(-40.0f).margin(1e-6));
    REQUIRE(t[0].volDb == Approx(-3.0f).margin(1e-6));

    // freeze=3(全冻结):都读 host。
    f.handles.rawFrz[0][0]->store(3.0f);
    t = f.auth.processBlock(true, 0.0);
    REQUIRE(t[0].pan == Approx(-40.0f).margin(1e-6));
    REQUIRE(t[0].volDb == Approx(5.0f).margin(1e-6));
}
