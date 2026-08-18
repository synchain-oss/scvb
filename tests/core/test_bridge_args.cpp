// SPDX-License-Identifier: GPL-3.0-or-later
// test_bridge_args —— T29 桥面参数提取/白名单助手单测。
// 覆盖:PR#55 缺陷3 严格布尔提取;PR#55 第3轮重要1 归一化→工程值;重要2 gesture 白名单。
// 佐证 JUCE 8.0.8 var::operator bool() = type->toBool(value)(juce_Variant.cpp:567),对 bool 返回真值。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include "BridgeArgs.h"
#include "OutputParams.h"

using Catch::Approx;

namespace
{

// 最小 AudioProcessor 桩,仅用于承载 APVTS(与 test_params_golden.cpp 同款)。
class LayoutTestProcessor final : public juce::AudioProcessor
{
public:
    LayoutTestProcessor()
        : juce::AudioProcessor(BusesProperties()
                                   .withInput("Input", juce::AudioChannelSet::stereo(), true)
                                   .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
    }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "BridgeArgsTest"; }
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

} // namespace

TEST_CASE("BRIDGEARGS-BOOL-1 严格布尔提取(false 语义正确)", "[bridgeargs][bool]")
{
    bool out = true;

    REQUIRE(scvb::output::strictBool(juce::var(false), out));
    REQUIRE_FALSE(out); // JS false → 关闭/未读/解锁

    REQUIRE(scvb::output::strictBool(juce::var(true), out));
    REQUIRE(out);

    REQUIRE(scvb::output::strictBool(juce::var(0), out));
    REQUIRE_FALSE(out); // int 0 → false

    REQUIRE(scvb::output::strictBool(juce::var(1), out));
    REQUIRE(out);
}

TEST_CASE("BRIDGEARGS-BOOL-2 非布尔/void 拒绝(调用方回 badArg)", "[bridgeargs][bool]")
{
    bool out = false;
    REQUIRE_FALSE(scvb::output::strictBool(juce::var("false"), out)); // 字符串拒绝(防 stringToBool 误判)
    REQUIRE_FALSE(scvb::output::strictBool(juce::var(), out)); // void 拒绝
}

TEST_CASE("BRIDGEARGS-BOOL-3 JUCE var::operator bool 语义佐证", "[bridgeargs][bool]")
{
    // 佐证:JUCE 8.0.8 var::operator bool() 对 bool 返回真值、对 void 返回 false。
    REQUIRE_FALSE(static_cast<bool>(juce::var(false)));
    REQUIRE(static_cast<bool>(juce::var(true)));
    REQUIRE_FALSE(static_cast<bool>(juce::var()));
}

TEST_CASE("BRIDGEARGS-PARAM-1 归一化→工程值往返(PR#55 第3轮重要1)", "[bridgeargs][param]")
{
    LayoutTestProcessor proc;
    juce::AudioProcessorValueTreeState apvts(proc, nullptr, "test", scvb::params::makeOutputLayout());

    // width(0..150%):归一化 0.5 → 75
    apvts.getParameter("width")->setValue(0.5f);
    REQUIRE(scvb::output::readParamEngineering(apvts, "width") == Approx(75.0f));

    // ms_balance(-100..+100):归一化 0.5 → 0
    apvts.getParameter("ms_balance")->setValue(0.5f);
    REQUIRE(scvb::output::readParamEngineering(apvts, "ms_balance") == Approx(0.0f));

    // lead_select(0..15):归一化 1.0 → 15
    apvts.getParameter("lead_select")->setValue(1.0f);
    REQUIRE(scvb::output::readParamEngineering(apvts, "lead_select") == Approx(15.0f));

    // v1_t01_vol(-24..+12):归一化 0.0 → -24
    apvts.getParameter("v1_t01_vol")->setValue(0.0f);
    REQUIRE(scvb::output::readParamEngineering(apvts, "v1_t01_vol") == Approx(-24.0f));

    // v1_t01_freeze(0..3):归一化 1.0 → 3
    apvts.getParameter("v1_t01_freeze")->setValue(1.0f);
    REQUIRE(scvb::output::readParamEngineering(apvts, "v1_t01_freeze") == Approx(3.0f));

    // v2_t15_pan(-100..+100):归一化 0.25 → -50
    apvts.getParameter("v2_t15_pan")->setValue(0.25f);
    REQUIRE(scvb::output::readParamEngineering(apvts, "v2_t15_pan") == Approx(-50.0f));
}

TEST_CASE("BRIDGEARGS-GESTURE-1 gesture 白名单(PR#55 第3轮重要2)", "[bridgeargs][gesture]")
{
    // 全局三件 + 激活版本(1)每轨 width/freeze。
    REQUIRE(scvb::output::isGestureParam("width", 1));
    REQUIRE(scvb::output::isGestureParam("ms_balance", 1));
    REQUIRE(scvb::output::isGestureParam("lead_select", 1));
    REQUIRE(scvb::output::isGestureParam("v1_t01_width", 1));
    REQUIRE(scvb::output::isGestureParam("v1_t15_width", 1));
    REQUIRE(scvb::output::isGestureParam("v1_t01_freeze", 1));
    REQUIRE(scvb::output::isGestureParam("v1_t15_freeze", 1));

    // 白名单外:pan/vol(走 setTrackManual 曲线真身)、非激活版本 width/freeze、未知 id。
    REQUIRE_FALSE(scvb::output::isGestureParam("v1_t01_pan", 1));
    REQUIRE_FALSE(scvb::output::isGestureParam("v1_t01_vol", 1));
    REQUIRE_FALSE(scvb::output::isGestureParam("v2_t01_width", 1));
    REQUIRE_FALSE(scvb::output::isGestureParam("v2_t01_freeze", 1));
    REQUIRE_FALSE(scvb::output::isGestureParam("bogus", 1));
}
