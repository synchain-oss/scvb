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

// 值域契约锁定:APVTS getRawParameterValue() 在 JUCE 8 返回**去归一化**实际单位
// (ParameterAdapter::unnormalisedValue = convertFrom0to1(getValue())),不是 0..1 归一化值。
// 用默认值区分:若存归一化,pan 默认 0 会是 0.5、每轨 width 默认 100 会是 1.0、全局 width 会是 0.667。
TEST_CASE("AUTH-PARAMS-5 raw 值是去归一化单位(非 0..1)—— 值域契约锁定", "[authority][params]")
{
    AuthorityParamsFixture f;

    // pan 默认 0(范围 -100..100):去归一化=0,归一化=0.5。
    REQUIRE(*f.handles.rawPan[0][0] == Approx(0.0f).margin(1e-6));
    // vol 默认 0(范围 -24..12):去归一化=0,归一化=24/36=0.6667。
    REQUIRE(*f.handles.rawVol[0][0] == Approx(0.0f).margin(1e-6));
    // 每轨 width 默认 100(范围 0..100):去归一化=100,归一化=1.0。
    REQUIRE(*f.handles.rawTrkW[0][0] == Approx(100.0f).margin(1e-6));
    // 全局 width 默认 100(范围 0..150):去归一化=100,归一化=100/150=0.6667。
    REQUIRE(*f.handles.rawWidth == Approx(100.0f).margin(1e-6));

    // 经 JUCE API 写值后再读,仍为去归一化单位。
    auto* pan = f.handles.pan[0][0];
    pan->setValueNotifyingHost(pan->convertTo0to1(50.0f));
    REQUIRE(*f.handles.rawPan[0][0] == Approx(50.0f).margin(1e-4));

    // lead_select(int 0..15)写 3 → raw=3(去归一化),不是 3/15=0.2。
    // 注意:AudioParameterInt::setValueNotifyingHost(float) 吃归一化值,这里用 operator=(int) 吃实际值。
    *f.handles.leadSelect = 3;
    REQUIRE(*f.handles.rawLeadSelect == Approx(3.0f).margin(1e-4));
}

// ---------------------------------------------------------------------------
// [SL-442] pan 角度域曲线 G 进实时链(02 §8.1 步骤 5)—— 消息线程侧的三格。
// 施加环节(MixMath)的判据在 test_mix_source.cpp / test_output_stage.cpp,这里只钉
// 「点列表 → LUT → 快照 → 音频线程拿得到」这条链,以及 no-op 守卫的指针稳定性。
// ---------------------------------------------------------------------------

namespace
{

// CURVE-2 的点(02 §7.3):shelf A=-6, P0=-45, Q=2, side=out ⇒ G(-45)=-3、G(-95)=-5.8921、G(+5)=-0.1079。
// 挑它是因为三个期望值彼此相距 ≥2.9 dB,远大于 LUT 插值容差 0.03 dB —— 容差不会大过被测量。
std::vector<scvb::PanCurvePoint> curve2Points()
{
    scvb::PanCurvePoint p;
    p.angle = -45.0f;
    p.gainDb = -6.0f;
    p.shape = scvb::PanCurveShape::shelf;
    p.q = 2.0f;
    p.side = scvb::PanCurveSide::out;
    return {p};
}

} // namespace

TEST_CASE("AUTH-PARAMS-6 pan 曲线 G 经快照送达音频线程,且与解析式同源", "[authority][params][pancurve]")
{
    AuthorityParamsFixture f;

    // 没设过 pan_curve:LUT 不存在 ⇒ 音频线程按 G≡0 走(老工程零影响的前半)。
    (void)f.auth.processBlock(true, 0.0);
    REQUIRE(f.auth.activePanCurveLut() == nullptr);
    REQUIRE(f.auth.arbiter().panCurveLut() == nullptr);

    const auto points = curve2Points();
    f.auth.setPanCurve(1, points);

    // ① 消息线程侧:发布出来的 LUT 与 evalCurve 同源(§7「UI 与 DSP 共用本实现」)。
    const auto lut = f.auth.activePanCurveLut();
    REQUIRE(lut != nullptr);
    for (const double pan : {-95.0, -45.0, 5.0, -100.0, 0.0, 100.0, 37.5})
    {
        REQUIRE(static_cast<double>(lut->gainDb(static_cast<float>(pan))) ==
                Approx(scvb::evalCurve(points, pan)).margin(0.03));
    }
    // 被测量本身远大于容差 —— 否则「同值」是靠容差蒙的,不是靠同源。
    REQUIRE(std::fabs(static_cast<double>(lut->gainDb(-95.0f))) > 5.0);
    REQUIRE(static_cast<double>(lut->gainDb(-45.0f)) == Approx(-3.0).margin(0.03));

    // ② 接线格:processBlock 之后音频线程侧拿到的必须**就是**发布出去的那张,不是另一张。
    //    只断「非 null」不够 —— 那样接错版本 / 接到旧表都能蒙混过去。
    (void)f.auth.processBlock(true, 0.0);
    REQUIRE(f.auth.arbiter().panCurveLut() == lut.get());
}

TEST_CASE("AUTH-PARAMS-7 点列表没变则 LUT 对象不重建(换表判据的前提)", "[authority][params][pancurve]")
{
    AuthorityParamsFixture f;

    const auto points = curve2Points();
    f.auth.setPanCurve(1, points);
    const auto* first = f.auth.activePanCurveLut().get();
    REQUIRE(first != nullptr);

    // 逐字段相同的另一份 vector(不是同一个对象)⇒ 仍应 no-op,指针不变。
    // `rebuildAllCurves` 每次段编辑都会走到这儿,这条不成立的话「换表了没有」恒真。
    f.auth.setPanCurve(1, curve2Points());
    REQUIRE(f.auth.activePanCurveLut().get() == first);

    // 只改一个字段(q 2.0 → 5.0,半宽 Δ=100/Q 由 50 收到 20)⇒ 必须换新表。
    // 这是上面那条的可分辨对照:少了它,一个「永远 no-op」的实现也能让上一条全绿。
    auto moved = points;
    moved[0].q = 5.0f;
    f.auth.setPanCurve(1, moved);
    const auto* second = f.auth.activePanCurveLut().get();
    REQUIRE(second != first);

    // 且新表真的换了内容,不只是换了个地址。
    // 探针取 P=-56.6:实测两条曲线在此处相差 1.16 dB(32769 格上扫出的最大差点附近),
    // 是 LUT 插值容差 0.03 dB 的约 39 倍 —— 「取到新值」不可能靠容差蒙混。
    // 反向看:若 setPanCurve 漏了重建,这里读到的还是旧曲线的值,与 moved 差 1.16 dB,
    // 下面那条 margin(0.03) 必红。探针选在差值大处,正是为了让它红得动
    // (第一版我取 P=-95、q 只动到 2.5,两条曲线在那儿仅差 0.068 dB —— 钉不住)。
    constexpr double kProbe = -56.6;
    REQUIRE(std::fabs(scvb::evalCurve(moved, kProbe) - scvb::evalCurve(points, kProbe)) > 1.0);
    REQUIRE(static_cast<double>(f.auth.activePanCurveLut()->gainDb(static_cast<float>(kProbe))) ==
            Approx(scvb::evalCurve(moved, kProbe)).margin(0.03));
}

TEST_CASE("AUTH-PARAMS-8 pan 曲线 per-version 隔离,换版本换表", "[authority][params][pancurve]")
{
    AuthorityParamsFixture f;

    const auto points = curve2Points();

    // 给非活动版本 2 设曲线:活动版本 1 不受影响(pan_curve 是 per-version)。
    f.auth.setPanCurve(2, points);
    REQUIRE(f.auth.versionActive() == 1);
    REQUIRE(f.auth.activePanCurveLut() == nullptr);
    (void)f.auth.processBlock(true, 0.0);
    REQUIRE(f.auth.arbiter().panCurveLut() == nullptr); // v1 没曲线 ⇒ 仍 G≡0

    // 切到版本 2:快照要带上 v2 的表。
    f.auth.setVersionActive(2);
    const auto lut2 = f.auth.activePanCurveLut();
    REQUIRE(lut2 != nullptr);
    (void)f.auth.processBlock(true, 0.0);
    REQUIRE(f.auth.arbiter().panCurveLut() == lut2.get());
    REQUIRE(static_cast<double>(lut2->gainDb(-45.0f)) == Approx(-3.0).margin(0.03));

    // 切回版本 1:表要跟着回到 null,不能把 v2 的表留在实时链上。
    f.auth.setVersionActive(1);
    (void)f.auth.processBlock(true, 0.0);
    REQUIRE(f.auth.arbiter().panCurveLut() == nullptr);
}

TEST_CASE("AUTH-PARAMS-9 换版本走同一条淡入路径(不为版本切换另写一份)", "[authority][params][pancurve][xfade]")
{
    AuthorityParamsFixture f;

    // v1 与 v2 各一条曲线,且在探针点差得很开(0 dB vs -12 dB)。
    auto mk = [](float db) {
        scvb::PanCurvePoint p;
        p.angle = 0.0f;
        p.gainDb = db;
        p.shape = scvb::PanCurveShape::bell;
        p.q = 1.5f;
        p.side = scvb::PanCurveSide::out;
        return std::vector<scvb::PanCurvePoint>{p};
    };
    f.auth.setPanCurve(1, mk(0.0f));
    f.auth.setPanCurve(2, mk(-12.0f));

    (void)f.auth.processBlock(true, 0.0);
    for (int i = 0; i < 4096; ++i)
        (void)f.auth.nextSample(); // 淡入窗口走完,回到稳态
    REQUIRE(f.auth.arbiter().panCurveXfadeRemaining() == 0);

    // 换版本 —— 快照里的 LUT 指针变了 ⇒ 必须开窗。
    // 曲线编辑和版本切换是**同一条**代码路径(都只是「LUT 对象换了」),这一格钉的就是
    // 版本切换确实落在那条路径上,而不是另写了一份、或者干脆没接。
    f.auth.setVersionActive(2);
    (void)f.auth.processBlock(true, 0.0);
    REQUIRE(f.auth.arbiter().panCurveXfadeRemaining() > 0);

    (void)f.auth.nextSample();
    const auto x = f.auth.arbiter().panCurveXfade();
    REQUIRE(x.previous != nullptr); // 旧版本那张还在,正在淡出
    REQUIRE(x.target == f.auth.activePanCurveLut().get()); // 淡向 v2 那张

    // 第一个样本仍贴着 v1(0 dB):没有淡入的话这里直接是 -12,与上一样本差 12 dB。
    REQUIRE(std::fabs(static_cast<double>(scvb::panCurveGainDb(x, 0.0f))) < 0.5);

    // 走完窗口:落到 v2 且旧表撤下。
    for (int i = 0; i < 4096; ++i)
        (void)f.auth.nextSample();
    REQUIRE(f.auth.arbiter().panCurveXfade().previous == nullptr);
    REQUIRE(static_cast<double>(scvb::panCurveGainDb(f.auth.arbiter().panCurveXfade(), 0.0f)) ==
            Approx(-12.0).margin(0.01));
}
