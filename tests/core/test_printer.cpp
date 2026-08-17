// SPDX-License-Identifier: GPL-3.0-or-later
// test_printer —— T17:AutomationPrinter + GestureGuard + PlayheadShot。
// 覆盖(03 §3.2/§3.3、P-6):五个 gesture 出口后 gestureOpen 全 false;区间外零 gesture(R5);
// deadband 段内常量零写;段边界必写点;ScopedSelfWriteFlag listener 短路;isLaneParam 分类。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <memory>

#include "AutomationPrinter.h"
#include "OutputParams.h"
#include "engine/CurveEvaluator.h"
#include "engine/PlayheadShot.h"

using Catch::Approx;

using scvb::engine::AuthorityMode;
using scvb::engine::PlayheadPod;
using scvb::output::AutomationPrinter;
using scvb::params::collectParamHandles;
using scvb::params::makeOutputLayout;
using scvb::params::ParamHandles;

namespace
{

constexpr double kFs = 48000.0;

// 最小 AudioProcessor:仅用于承载 APVTS(同 test_authority_params.cpp)。
struct PrinterTestProcessor final : juce::AudioProcessor
{
    PrinterTestProcessor()
        : juce::AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "PrinterTest"; }
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

// 参数 listener:计数 setValueNotifyingHost 触发的值变化与 gesture 配对。
struct CountingListener final : juce::AudioProcessorParameter::Listener
{
    int valueChanges = 0;
    int gestureBegins = 0;
    int gestureEnds = 0;
    int openNow = 0;

    void parameterValueChanged(int, float) override { ++valueChanges; }
    void parameterGestureChanged(int, bool gestureIsStarting) override
    {
        if (gestureIsStarting)
        {
            ++gestureBegins;
            ++openNow;
        }
        else
        {
            ++gestureEnds;
            --openNow;
        }
    }
};

// 探测 ScopedSelfWriteFlag:写值回调期间 isSelfWrite() 应为 true(§3.5 层 2 listener 短路)。
struct SelfWriteProbe final : juce::AudioProcessorParameter::Listener
{
    AutomationPrinter* printer = nullptr;
    bool sawSelfWriteTrue = false;

    void parameterValueChanged(int, float) override
    {
        if (printer != nullptr && printer->isSelfWrite())
            sawSelfWriteTrue = true;
    }
    void parameterGestureChanged(int, bool) override {}
};

scvb::CurveEvaluator constCurve(double pan, double volDb)
{
    scvb::CurveEvaluator ev;
    ev.build({scvb::CurveSegment{0.0, 1000.0, pan, volDb}}, scvb::TransitionConfig{});
    return ev;
}

// 双段曲线:跨 t=1.0 的边界,pan 0 → +6(非微小变化,产生真实 ramp)。
scvb::CurveEvaluator boundaryCurve()
{
    scvb::CurveEvaluator ev;
    ev.build({scvb::CurveSegment{0.0, 1.0, 0.0, 0.0}, scvb::CurveSegment{1.0, 10.0, 6.0, 0.0}},
             scvb::TransitionConfig{});
    return ev;
}

// 一次构造:APVTS + handles + printer + shot + 15 轨曲线存储。
struct PrinterFixture
{
    PrinterTestProcessor proc;
    juce::AudioProcessorValueTreeState apvts;
    ParamHandles handles;
    scvb::engine::PlayheadShot shot;
    std::array<scvb::CurveEvaluator, AutomationPrinter::kNumTracks> curves{};
    AutomationPrinter printer;

    PrinterFixture() : apvts(proc, nullptr, "PARAMETERS", makeOutputLayout()), handles(collectParamHandles(apvts))
    {
        printer.setShot(&shot);
        printer.setAnalyzedRange(0.0, 100.0); // 宽区间,便于测试
        printer.bindVersion(1, handles);
    }

    // 仅给轨 0 设置曲线(其余轨曲线为 null → 不打印)。
    void setCurve0(scvb::CurveEvaluator ev)
    {
        curves[0] = std::move(ev);
        std::array<const scvb::CurveEvaluator*, AutomationPrinter::kNumTracks> ptrs{};
        ptrs[0] = &curves[0];
        printer.setCurves(ptrs);
    }

    void publishAtSec(double tSec, double sampleRate = kFs)
    {
        PlayheadPod p;
        p.timeSamples = static_cast<int64_t>(tSec * sampleRate);
        p.sampleRate = sampleRate;
        p.flags = scvb::engine::kPlayheadIsPlaying;
        shot.publish(p);
    }
};

} // namespace

// ============================================================================
// GestureGuard 四出口 + epoch 出口(R12 / P-6):每出口后 gestureOpen 全 false。
// ============================================================================

TEST_CASE("PRINTER-GUARD-1 模式切换(stop/关 Output)后 gestureOpen 全 false", "[printer]")
{
    PrinterFixture f;
    f.setCurve0(constCurve(30.0, -3.0));

    f.printer.setMode(AuthorityMode::Print);
    f.publishAtSec(0.0);
    f.printer.tick();

    REQUIRE(f.printer.anyGestureOpen());
    REQUIRE(f.printer.numGesturesOpen() == 2); // 轨 0 的 pan + vol

    SECTION("stop:PRINT → ARMED")
    {
        f.printer.setMode(AuthorityMode::Armed);
        REQUIRE_FALSE(f.printer.anyGestureOpen());
        REQUIRE(f.printer.numGesturesOpen() == 0);
    }
    SECTION("关 Output:PRINT → FOLLOW")
    {
        f.printer.setMode(AuthorityMode::Follow);
        REQUIRE_FALSE(f.printer.anyGestureOpen());
        REQUIRE(f.printer.numGesturesOpen() == 0);
    }
}

TEST_CASE("PRINTER-GUARD-2 离区间(R5)后 gestureOpen 全 false 且零写入", "[printer]")
{
    PrinterFixture f;
    f.setCurve0(constCurve(30.0, -3.0));
    CountingListener panL;
    f.handles.pan[0][0]->addListener(&panL);

    f.printer.setMode(AuthorityMode::Print);
    f.publishAtSec(200.0); // 区间 [0,100] 之外
    f.printer.tick();

    REQUIRE_FALSE(f.printer.anyGestureOpen());
    REQUIRE(f.printer.numGesturesOpen() == 0);
    REQUIRE(panL.valueChanges == 0);
    REQUIRE(panL.gestureBegins == 0);

    f.handles.pan[0][0]->removeListener(&panL);
}

TEST_CASE("PRINTER-GUARD-3 epoch 跳变(endAllGestures)幂等且可重新 begin", "[printer]")
{
    PrinterFixture f;
    f.setCurve0(constCurve(30.0, -3.0));

    f.printer.setMode(AuthorityMode::Print);
    f.publishAtSec(0.0);
    f.printer.tick();
    REQUIRE(f.printer.anyGestureOpen());

    // epoch 跳变:endAllGestures() 幂等,连续两次不崩溃、不双 end。
    f.printer.endAllGestures();
    f.printer.endAllGestures();
    REQUIRE_FALSE(f.printer.anyGestureOpen());

    // 跳变后仍在区间内 → 下一 tick 重新 begin(不是重复 begin 叠加)。
    f.printer.tick();
    REQUIRE(f.printer.numGesturesOpen() == 2);
}

TEST_CASE("PRINTER-GUARD-4 析构路径闭合全部 gesture", "[printer]")
{
    PrinterTestProcessor proc;
    juce::AudioProcessorValueTreeState apvts(proc, nullptr, "PARAMETERS", makeOutputLayout());
    ParamHandles handles = collectParamHandles(apvts);

    CountingListener panL;
    CountingListener volL;
    handles.pan[0][0]->addListener(&panL);
    handles.vol[0][0]->addListener(&volL);

    scvb::engine::PlayheadShot shot;
    {
        AutomationPrinter printer;
        printer.setShot(&shot);
        printer.setAnalyzedRange(0.0, 100.0);
        printer.bindVersion(1, handles);

        scvb::CurveEvaluator ev = constCurve(30.0, -3.0);
        std::array<const scvb::CurveEvaluator*, AutomationPrinter::kNumTracks> ptrs{};
        ptrs[0] = &ev;
        printer.setCurves(ptrs);

        printer.setMode(AuthorityMode::Print);
        PlayheadPod p;
        p.timeSamples = 0;
        p.sampleRate = kFs;
        p.flags = scvb::engine::kPlayheadIsPlaying;
        shot.publish(p);
        printer.tick();

        REQUIRE(panL.gestureBegins == 1);
        REQUIRE(volL.gestureBegins == 1);
        REQUIRE(printer.numGesturesOpen() == 2);
    } // printer 析构 → GestureGuard 兜底 endAllGestures

    REQUIRE(panL.gestureEnds == 1);
    REQUIRE(volL.gestureEnds == 1);
    REQUIRE(panL.openNow == 0);
    REQUIRE(volL.openNow == 0);

    handles.pan[0][0]->removeListener(&panL);
    handles.vol[0][0]->removeListener(&volL);
}

// ============================================================================
// deadband:段内常量零写;段边界必写点。
// ============================================================================

TEST_CASE("PRINTER-DEADBAND-1 段内常量零写(deadband 去重)", "[printer]")
{
    PrinterFixture f;
    f.setCurve0(constCurve(30.0, -3.0));
    CountingListener panL;
    f.handles.pan[0][0]->addListener(&panL);

    f.printer.setMode(AuthorityMode::Print);
    for (int i = 0; i < 5; ++i)
    {
        f.publishAtSec(static_cast<double>(i) * 0.1); // 常量段内不同时刻
        f.printer.tick();
    }

    REQUIRE(panL.valueChanges == 1); // 仅首次写入,后续 deadband 抑制

    f.handles.pan[0][0]->removeListener(&panL);
}

TEST_CASE("PRINTER-DEADBAND-2 段边界必写点(per-tick delta < deadband 仍写)", "[printer]")
{
    PrinterFixture f;
    f.setCurve0(boundaryCurve()); // pan 0 → +6,ramp 窗口 [0.7, 1.3]
    CountingListener panL;
    f.handles.pan[0][0]->addListener(&panL);

    f.printer.setMode(AuthorityMode::Print);

    f.publishAtSec(0.5);
    f.printer.tick(); // 段内首点:pan=0
    const int baseline = panL.valueChanges;
    REQUIRE(baseline == 1);

    // ramp 窗口内 4 个时刻,每 tick delta < deadband(0.5),但 boundary 强制写。
    const double rampTimes[] = {0.72, 0.74, 0.76, 0.78};
    for (const double t : rampTimes)
    {
        f.publishAtSec(t);
        f.printer.tick();
    }

    REQUIRE(panL.valueChanges == baseline + 4); // 边界必写 4 点

    f.handles.pan[0][0]->removeListener(&panL);
}

// ============================================================================
// §3.5 层 2:ScopedSelfWriteFlag listener 短路。
// ============================================================================

TEST_CASE("PRINTER-SELFW-1 ScopedSelfWriteFlag 生效(listener 短路)", "[printer]")
{
    PrinterFixture f;
    f.setCurve0(constCurve(30.0, -3.0));
    SelfWriteProbe probe;
    probe.printer = &f.printer;
    f.handles.pan[0][0]->addListener(&probe);

    f.printer.setMode(AuthorityMode::Print);
    f.publishAtSec(0.0);
    f.printer.tick();

    REQUIRE(probe.sawSelfWriteTrue); // 写值回调期间 isSelfWrite()==true → listener 短路
    REQUIRE_FALSE(f.printer.isSelfWrite()); // 写后清位

    f.handles.pan[0][0]->removeListener(&probe);
}

// ============================================================================
// §3.5 层 1:isLaneParam 分类(host echo 按模式屏蔽只作用于 30 条打印车道)。
// ============================================================================

TEST_CASE("PRINTER-ECHO-1 isLaneParam 分类", "[printer]")
{
    REQUIRE(AutomationPrinter::isLaneParam("v1_t01_pan"));
    REQUIRE(AutomationPrinter::isLaneParam("v1_t15_vol"));
    REQUIRE(AutomationPrinter::isLaneParam("v2_t01_pan"));

    REQUIRE_FALSE(AutomationPrinter::isLaneParam("width"));
    REQUIRE_FALSE(AutomationPrinter::isLaneParam("ms_balance"));
    REQUIRE_FALSE(AutomationPrinter::isLaneParam("lead_select"));
    REQUIRE_FALSE(AutomationPrinter::isLaneParam("v1_t01_width"));
    REQUIRE_FALSE(AutomationPrinter::isLaneParam("v1_t01_freeze"));
}

// ============================================================================
// PlayheadShot SPSC:seq 奇偶协议,写方永不自旋;读方撕裂沿用上帧。
// ============================================================================

TEST_CASE("PRINTER-SHOT-1 PlayheadShot publish/read 往返一致", "[printer]")
{
    scvb::engine::PlayheadShot shot;
    PlayheadPod out;

    PlayheadPod p;
    p.timeSamples = 48000;
    p.ppq = 4.0;
    p.bpm = 120.0;
    p.sampleRate = kFs;
    p.epoch = 3;
    p.flags = scvb::engine::kPlayheadIsPlaying | scvb::engine::kPlayheadTempoValid;

    shot.publish(p);
    REQUIRE(shot.read(out));
    REQUIRE(out.timeSamples == 48000);
    REQUIRE(out.ppq == Approx(4.0));
    REQUIRE(out.bpm == Approx(120.0));
    REQUIRE(out.epoch == 3);
    REQUIRE((out.flags & scvb::engine::kPlayheadIsPlaying) != 0);

    // seq 终值为偶数(发布完成后);写前 +1 奇、写后 +1 偶。
    REQUIRE((shot.seq.load() & 1u) == 0u);
}
