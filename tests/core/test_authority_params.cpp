// SPDX-License-Identifier: GPL-3.0-or-later
// test_authority_params —— T16 JUCE 参数层:OutputAuthority 把 T15 的 ParamHandles(raw atomic)
// 绑定到 DspArbiter;断言 PRINT 态篡改真实 JUCE 参数不改变 DSP 输出、lead_select 覆盖、版本切换重绑。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

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

TEST_CASE("AUTH-PARAMS-10 坏点在进表之前被挡住(接线格,不是零件格)", "[authority][params][pancurve][nan]")
{
    // SL442-NAN-* 测的是守卫**函数**;这一格测的是守卫**被调用了** —— 把 setPanCurve 里那句
    // arePanCurvePointsUsable 摘掉,函数级那几格照样全绿,只有这一格会红。
    AuthorityParamsFixture f;

    scvb::PanCurvePoint good;
    good.angle = 0.0f;
    good.gainDb = -9.0f;
    good.shape = scvb::PanCurveShape::bell;
    good.q = 1.5f;
    good.side = scvb::PanCurveSide::out;
    f.auth.setPanCurve(1, {good});
    const auto* baseline = f.auth.activePanCurveLut().get();
    REQUIRE(baseline != nullptr);
    REQUIRE(static_cast<double>(f.auth.activePanCurveLut()->gainDb(0.0f)) == Approx(-9.0).margin(0.03));

    // 一个 NaN 点混进来(模拟损坏 state / 别的写入方)。
    scvb::PanCurvePoint poisoned = good;
    poisoned.gainDb = std::numeric_limits<float>::quiet_NaN();
    f.auth.setPanCurve(1, {good, poisoned});

    // ① 表没被换掉 —— 保留上一张,而不是烘一张带 NaN 的出来。
    REQUIRE(f.auth.activePanCurveLut().get() == baseline);
    // ② 表里仍然处处有限(真正要守的东西)。
    for (const float pan : {-100.0f, -33.0f, 0.0f, 51.0f, 100.0f})
    {
        REQUIRE(std::isfinite(f.auth.activePanCurveLut()->gainDb(pan)));
    }
    // ③ 音频线程那一侧拿到的也有限 —— 一路到实时链出口都不是 NaN。
    (void)f.auth.processBlock(true, 0.0);
    (void)f.auth.nextSample();
    const auto x = f.auth.arbiter().panCurveXfade();
    for (const float pan : {-100.0f, 0.0f, 100.0f})
    {
        REQUIRE(std::isfinite(scvb::panCurveGainDb(x, pan)));
    }

    // 对照:合法的新点列表照样换得动(守卫没有把正常路径也挡死)。
    scvb::PanCurvePoint other = good;
    other.gainDb = -3.0f;
    f.auth.setPanCurve(1, {other});
    REQUIRE(f.auth.activePanCurveLut().get() != baseline);
    REQUIRE(static_cast<double>(f.auth.activePanCurveLut()->gainDb(0.0f)) == Approx(-3.0).margin(0.03));
}

// ---------------------------------------------------------------------------
// [SL-445] 快照池回收。此前池「进程寿命保活、绝不释放」,每次改曲线留下一份 128 KiB 的 LUT。
// 现在音频线程每块报 oldestHeldSeq,发布方在消息线程上释放 seq 更小的快照。
// 这一组钉三件事:①池有上界(不回收 ⇒ 红);②音频线程**当前那份**不被放(判据取等号 ⇒ 红);
// ③换表淡入窗口开着时,**旧表那份**也不被放(确认值不算窗口 ⇒ 红)。④是真双线程的压力格。
// ---------------------------------------------------------------------------

namespace
{

// 单点 bell,中心 0°、增益 db:在 P=0 处 G 恰为 db,换 db 就换一张表(no-op 守卫不会挡)。
std::vector<scvb::PanCurvePoint> bellAt0(float db)
{
    scvb::PanCurvePoint p;
    p.angle = 0.0f;
    p.gainDb = db;
    p.shape = scvb::PanCurveShape::bell;
    p.q = 1.5f;
    p.side = scvb::PanCurveSide::out;
    return {p};
}

} // namespace

TEST_CASE("AUTH-PARAMS-11 快照池有上界:音频线程在跑时,连改 300 次曲线池不随之增长",
          "[authority][params][pancurve][sl445]")
{
    AuthorityParamsFixture f;
    (void)f.auth.processBlock(true, 0.0);

    // 模拟 Q 滑杆断续拖:每次提交之间音频线程跑过一块(256 样本 ≈ 5 ms,短于 30 ms 淡入窗口,
    // 所以窗口一直在「重开」—— 这是确认值最保守的那种节奏)。
    std::weak_ptr<const scvb::PanCurveLut> firstLut;
    std::size_t maxPool = 0;
    for (int i = 0; i < 300; ++i)
    {
        f.auth.setPanCurve(1, bellAt0(-1.0f - static_cast<float>(i % 11)));
        if (i == 0)
            firstLut = f.auth.activePanCurveLut();
        (void)f.auth.processBlock(true, 0.0);
        for (int n = 0; n < 256; ++n)
            (void)f.auth.nextSample();
        maxPool = std::max(maxPool, f.auth.snapshotPoolSize());
    }
    // 不回收时池 = 发布次数(≥ 300);回收时稳态只留「旧表那份 + 上一块那份 + 刚发的」量级。
    CHECK(maxPool <= 4);
    CHECK(f.auth.snapshotPoolSize() <= 4);
    // 被放掉的不只是 744 B 的快照壳,还有它钉住的那张 128 KiB 的表 —— 这才是 SL-445 要收的内存。
    CHECK(firstLut.expired());
}

TEST_CASE("AUTH-PARAMS-12 音频线程还没跑下一块时,它手上那份快照一律不放", "[authority][params][pancurve][sl445]")
{
    AuthorityParamsFixture f;
    f.auth.setPanCurve(1, bellAt0(-3.0f));
    std::weak_ptr<const scvb::PanCurveLut> held = f.auth.activePanCurveLut();
    (void)f.auth.processBlock(true, 0.0);
    for (int n = 0; n < 4096; ++n)
        (void)f.auth.nextSample(); // 走完「从 G≡0 到这张表」的淡入窗口:此后确认值只看本块那份
    REQUIRE(f.auth.arbiter().panCurveXfadeRemaining() == 0);
    (void)f.auth.processBlock(true, 0.0); // 音频线程此刻持有这份(本块快照 + 本块 LUT 裸指针)
    REQUIRE(f.auth.arbiter().panCurveLut() != nullptr);
    REQUIRE(f.auth.arbiter().panCurveXfadeRemaining() == 0); // 窗口关着 ⇒ 下面钉的是「本块那份」,不是旧表那份

    // 消息线程连发 20 份新表,音频线程一块都没跑:它的确认值还停在「手上那份」。
    // authority 自己对 -3 dB 那张表的引用在第一次换表时就没了,之后它只靠快照活着。
    for (int i = 0; i < 20; ++i)
        f.auth.setPanCurve(1, bellAt0(-4.0f - static_cast<float>(i)));

    // 判据是「严格小于」确认值才放;写成小于等于,手上这份就被放了,下面那次解引用即悬垂。
    CHECK_FALSE(held.expired());
    if (!held.expired()) // 注入态下这里已悬垂:只在判据成立时才去读,不让测试自己踩 UAF
        CHECK(f.auth.arbiter().panCurveLut()->gainDb(0.0f) == Approx(-3.0f).margin(0.03));

    // 对照(活性):音频线程跑过一块、窗口走完,再发一份 ⇒ 这份就该放了。
    // 少了这一格,一个「永远什么都不放」的实现也能让上面两条全绿。
    (void)f.auth.processBlock(true, 0.0);
    for (int n = 0; n < 4096; ++n)
        (void)f.auth.nextSample();
    (void)f.auth.processBlock(true, 0.0);
    f.auth.setPanCurve(1, bellAt0(-30.0f));
    CHECK(held.expired());
}

TEST_CASE("AUTH-PARAMS-13 换表淡入窗口开着时,旧表那份快照不放", "[authority][params][pancurve][xfade][sl445]")
{
    AuthorityParamsFixture f;
    const auto flat = constCurve(0.0, 0.0);

    f.auth.setPanCurve(1, bellAt0(0.0f));
    std::weak_ptr<const scvb::PanCurveLut> oldLut = f.auth.activePanCurveLut();
    (void)f.auth.processBlock(true, 0.0);
    for (int n = 0; n < 4096; ++n)
        (void)f.auth.nextSample(); // 「从 G≡0 到第一张表」的窗口走完
    REQUIRE(f.auth.arbiter().panCurveXfadeRemaining() == 0);

    // 换表 ⇒ 开窗,旧表 = oldLut(音频线程跨块持有它的裸指针)。
    f.auth.setPanCurve(1, bellAt0(-12.0f));
    (void)f.auth.processBlock(true, 0.0);
    REQUIRE(f.auth.arbiter().panCurveXfadeRemaining() > 0);

    // 窗口内再发几份**不换表**的快照(段编辑形态:setCurve 走 rebindSources,LUT 指针不变,
    // 所以窗口不会重开),并让音频线程每份都跑一块 —— 确认值若只看「本块那份」,就会
    // 一路推过持有旧表的那份,把它放掉。
    for (int i = 0; i < 4; ++i)
    {
        f.auth.setCurve(1, 0, &flat);
        (void)f.auth.processBlock(true, 0.0);
        for (int n = 0; n < 64; ++n)
            (void)f.auth.nextSample();
    }
    REQUIRE(f.auth.arbiter().panCurveXfadeRemaining() > 0); // 窗口确实还开着,本格前提成立
    CHECK_FALSE(oldLut.expired());
    const auto x = f.auth.arbiter().panCurveXfade();
    CHECK(x.previous != nullptr);
    // 旧表仍可读且内容没变(0 dB 那张)。注入态下它已悬垂,只在判据成立时才读。
    if (!oldLut.expired() && x.previous != nullptr)
        CHECK(x.previous->gainDb(0.0f) == Approx(0.0f).margin(0.03));

    // 活性对照:窗口关上后下一块把确认值推过去,再发一份 ⇒ 旧表那份放掉。
    for (int n = 0; n < 4096; ++n)
        (void)f.auth.nextSample();
    REQUIRE(f.auth.arbiter().panCurveXfadeRemaining() == 0);
    (void)f.auth.processBlock(true, 0.0);
    f.auth.setCurve(1, 0, &flat);
    CHECK(oldLut.expired());
}

TEST_CASE("AUTH-PARAMS-14 压力:消息线程连发、音频线程并发读,读到的表始终完整且池有上界",
          "[authority][params][pancurve][xfade][sl445]")
{
    // 真双线程。它能证明的是「没崩、读到的值始终在合法集合里、池不涨」;
    // ⚠ 它**证明不了**「时序绝无 use-after-free」:Release 下被放掉的 128 KiB 块多半立刻被下一张
    //   表复用,内容仍是一张合法的表,读不出异常。时序正确性由 AUTH-PARAMS-12/13 两格确定性地钉。
    AuthorityParamsFixture f;
    f.auth.setPanCurve(1, bellAt0(-1.0f));

    std::atomic<bool> stop{false};
    std::atomic<int> badReads{0};
    std::atomic<long> blocks{0};
    std::thread audio([&] {
        while (!stop.load(std::memory_order_acquire))
        {
            (void)f.auth.processBlock(true, 0.0);
            for (int n = 0; n < 32; ++n)
            {
                (void)f.auth.nextSample();
                // 所有提交过的表在 P=0 处都在 [-12, -1] dB;淡入是两张合法表的凸组合,仍在区间内。
                const float g = scvb::panCurveGainDb(f.auth.arbiter().panCurveXfade(), 0.0f);
                if (!std::isfinite(g) || g > -0.9f || g < -12.1f)
                    badReads.fetch_add(1, std::memory_order_relaxed);
            }
            blocks.fetch_add(1, std::memory_order_release);
        }
    });

    // 先等音频线程真正跑完第一块:否则负载高时 400 次发布可能全落在它起跑之前,池涨到 ~400,
    // 下面的上界判据红的是「线程没被调度」而不是回收坏了(#306 复审建议)。
    while (blocks.load(std::memory_order_acquire) == 0)
        std::this_thread::yield();
    std::size_t maxPool = 0;
    for (int i = 0; i < 400; ++i)
    {
        f.auth.setPanCurve(1, bellAt0(-1.0f - static_cast<float>(i % 12)));
        maxPool = std::max(maxPool, f.auth.snapshotPoolSize());
    }
    // 让音频线程再跑 128 块(每块 32 样本,共 4096 > 30 ms 淡入窗口 1440):最后一次换表的窗口
    // 必已关上,确认值推到最后一份 pan 快照;再发一份触发回收 ⇒ 池只剩那份 + 刚发的。
    const long seen = blocks.load(std::memory_order_acquire);
    while (blocks.load(std::memory_order_acquire) < seen + 128)
        std::this_thread::yield();
    f.auth.setCurve(1, 0, nullptr);
    stop.store(true, std::memory_order_release);
    audio.join();

    CHECK(badReads.load() == 0);
    CHECK(blocks.load() > 0);
    // 上界取宽:音频线程可能被调度器饿住一阵,期间的发布照积;但绝不会接近「400 份全留」。
    CHECK(maxPool < 200);
    CHECK(f.auth.snapshotPoolSize() <= 2);
}

// ---------------------------------------------------------------------------
// [J157 / SL-447] pan 曲线拖动预览(契约 §1.37 `previewPanCurve`)。
// 这一组钉:① 预览只换音频线程那张表,已提交那张不动;松手提交同一组点沿用预览那张(不开淡入窗口);
// ② 限速 ≤ 20 Hz 且被拦下的那份留着待发;③ 回收闸 —— 音频线程停着时拖多久池里也只多一份;
// ④ 音频在跑时连续拖 10 秒(200 份)池有上界;⑤ 占空比:烘表贵时把间隔拉长;⑥ 实测烘表耗时(只报告);
// ⑦ 作废规则(撤回 / 已提交曲线变了 / 换版本 / 旧版本号 / 坏点)。
// ---------------------------------------------------------------------------

namespace
{

using PreviewReq = scvb::output::OutputAuthority::PanCurvePreviewRequest;

// 音频线程跑一块:processBlock(吃到最新快照)+ samples 次 nextSample。
void runAudio(AuthorityParamsFixture& f, int samples)
{
    (void)f.auth.processBlock(true, 0.0);
    for (int n = 0; n < samples; ++n)
        (void)f.auth.nextSample();
}

// 让音频线程「越过」最新发布:跑一块吃到它、走完 30 ms 淡入窗口(48 kHz 下 1440 样本),
// 再跑一块把确认值推到它身上(窗口在 nextSample 里关上,下一块才报新值)。
void ackAudio(AuthorityParamsFixture& f)
{
    runAudio(f, 4096);
    (void)f.auth.processBlock(true, 0.0);
}

// 烘表耗时恒报 0 ms:把「限速」「回收闸」两格与机器快慢解耦(否则慢机上的真实烘表耗时会经
// 占空比把间隔拉长,红在与被测无关的地方)。占空比那一半另有专格(AUTH-PARAMS-19)。
void zeroBakeClock(AuthorityParamsFixture& f)
{
    f.auth.setPanCurvePreviewBakeClock([] { return 0.0; });
}

} // namespace

TEST_CASE("AUTH-PARAMS-15 [J157] 预览只换音频线程那张表,已提交那张不动;松手提交同一组点沿用预览那张",
          "[authority][params][pancurve][j157]")
{
    AuthorityParamsFixture f;
    zeroBakeClock(f);
    const auto flat = constCurve(0.0, 0.0);
    f.auth.setPanCurve(1, bellAt0(-3.0f));
    const auto committed = f.auth.activePanCurveLut();
    ackAudio(f);
    REQUIRE(f.auth.arbiter().panCurveLut() == committed.get());

    REQUIRE(f.auth.requestPanCurvePreview(1, bellAt0(-12.0f)) == PreviewReq::accepted);
    CHECK_FALSE(f.auth.pumpPanCurvePreview(0.0)); // 闸门开着:就地发出,没有待发
    CHECK(f.auth.panCurvePreviewLive());
    const auto preview = f.auth.panCurvePreviewLut();
    REQUIRE(preview != nullptr);
    REQUIRE(preview.get() != committed.get());

    // ① 已提交那张没换:存盘 / 回推 / 分析读的都是已提交的点表,这里是它在 authority 侧的那一份。
    CHECK(f.auth.activePanCurveLut() == committed);

    // ② 音频线程听的是预览那张,而且就是预览点表烘出来的值。
    runAudio(f, 4096);
    CHECK(f.auth.arbiter().panCurveLut() == preview.get());
    CHECK(f.auth.arbiter().panCurveLut()->gainDb(0.0f) == Approx(-12.0f).margin(0.03));

    // ③ 拖动途中别的东西触发重发(段编辑形态:setCurve → rebindSources),预览照样在;
    //    段编辑顺带跑的 rebuildAllCurves → setPanCurve(已提交的点)走 no-op,不许撤预览。
    f.auth.setCurve(1, 0, &flat);
    runAudio(f, 64);
    CHECK(f.auth.arbiter().panCurveLut() == preview.get());
    f.auth.setPanCurve(1, bellAt0(-3.0f));
    CHECK(f.auth.panCurvePreviewLive());
    runAudio(f, 64);
    CHECK(f.auth.arbiter().panCurveLut() == preview.get());

    // ④ 松手提交同一组点 ⇒ 沿用预览那张(指针相同)、预览收掉、音频线程不开淡入窗口 ⇒ 输出逐位不变。
    runAudio(f, 4096);
    REQUIRE(f.auth.arbiter().panCurveXfadeRemaining() == 0);
    f.auth.setPanCurve(1, bellAt0(-12.0f));
    CHECK(f.auth.activePanCurveLut() == preview);
    CHECK_FALSE(f.auth.panCurvePreviewLive());
    (void)f.auth.processBlock(true, 0.0);
    CHECK(f.auth.arbiter().panCurveLut() == preview.get());
    CHECK(f.auth.arbiter().panCurveXfadeRemaining() == 0);
}

TEST_CASE("AUTH-PARAMS-16 [J157] 预览限速 ≤ 20 Hz:50 ms 内再来的留着待发,到点发最后一份(不丢)",
          "[authority][params][pancurve][j157]")
{
    AuthorityParamsFixture f;
    zeroBakeClock(f);
    ackAudio(f);
    REQUIRE(f.auth.requestPanCurvePreview(1, bellAt0(-2.0f)) == PreviewReq::accepted);
    REQUIRE_FALSE(f.auth.pumpPanCurvePreview(1000.0));
    REQUIRE(f.auth.panCurvePreviewStats().publishes == 1);
    ackAudio(f); // 回收闸开着:下面拦住后几份的只可能是限速

    // 一拍里连来三份 ⇒ 合并成最后一份(桥调用只记点表,不烘)。
    for (const float db : {-4.0f, -5.0f, -6.0f})
        REQUIRE(f.auth.requestPanCurvePreview(1, bellAt0(db)) == PreviewReq::accepted);
    CHECK(f.auth.pumpPanCurvePreview(1010.0)); // 10 ms 后:拦住、仍待发
    CHECK(f.auth.pumpPanCurvePreview(1049.0)); // 49 ms:仍拦
    CHECK(f.auth.panCurvePreviewStats().publishes == 1);
    CHECK(f.auth.panCurvePreviewPending());
    CHECK_FALSE(f.auth.pumpPanCurvePreview(1050.0)); // 50 ms:发出
    CHECK(f.auth.panCurvePreviewStats().publishes == 2);
    CHECK(f.auth.panCurvePreviewLut()->gainDb(0.0f) == Approx(-6.0f).margin(0.03)); // 发的是最后一份
    CHECK(f.auth.panCurvePreviewStats().bakes == 2); // 中间两份没烘
}

TEST_CASE("AUTH-PARAMS-17 [J157] 音频线程停着时连拖 10 秒(200 份):至多一份预览未确认,池不随拖动增长",
          "[authority][params][pancurve][j157][sl445]")
{
    AuthorityParamsFixture f;
    zeroBakeClock(f);
    f.auth.setPanCurve(1, bellAt0(-1.0f));
    ackAudio(f);
    const std::size_t poolBefore = f.auth.snapshotPoolSize();

    // 间隔 50 ms(= 20 Hz)的 200 份请求 = 10 s;期间音频线程一块都不跑(只读观察 / 无时间线 /
    // 无注入轨 / 宿主停了音频 —— SL-445 写明的那几种「不回收」都是这个形态)。
    for (int i = 0; i < 200; ++i)
    {
        REQUIRE(f.auth.requestPanCurvePreview(1, bellAt0(-2.0f - static_cast<float>(i % 10))) == PreviewReq::accepted);
        (void)f.auth.pumpPanCurvePreview(static_cast<double>(i) * 50.0);
    }
    // 回收闸没开过 ⇒ 只有第一份发出去了。没有这道闸就是 200 份快照、200 张 128 KiB 的表(约 25 MB)
    // 钉到音频再跑起来 —— 比「只在松手时提交」糟得多。
    CHECK(f.auth.panCurvePreviewStats().publishes == 1);
    CHECK(f.auth.panCurvePreviewStats().bakes == 1);
    CHECK(f.auth.snapshotPoolSize() <= poolBefore + 1);
    CHECK(f.auth.panCurvePreviewPending()); // 最后一份没丢,等着

    // 活性对照:音频线程一跑起来,下一拍就把最后那份发出去(少了它,「什么都不发」的实现也全绿)。
    ackAudio(f);
    CHECK_FALSE(f.auth.pumpPanCurvePreview(200.0 * 50.0));
    CHECK(f.auth.panCurvePreviewStats().publishes == 2);
    CHECK(f.auth.panCurvePreviewLut()->gainDb(0.0f) == Approx(-11.0f).margin(0.03)); // i=199 ⇒ -2-9
}

TEST_CASE("AUTH-PARAMS-18 [J157] 音频在跑时连续拖 10 秒(200 份预览):每份都发、池有上界、旧预览表逐张释放",
          "[authority][params][pancurve][j157][sl445]")
{
    AuthorityParamsFixture f;
    zeroBakeClock(f);
    f.auth.setPanCurve(1, bellAt0(-1.0f));
    ackAudio(f);

    std::vector<std::weak_ptr<const scvb::PanCurveLut>> seen;
    std::size_t maxPool = 0;
    for (int i = 0; i < 200; ++i)
    {
        REQUIRE(f.auth.requestPanCurvePreview(1, bellAt0(-2.0f - static_cast<float>(i % 10))) == PreviewReq::accepted);
        (void)f.auth.pumpPanCurvePreview(static_cast<double>(i) * 50.0);
        seen.push_back(f.auth.panCurvePreviewLut());
        maxPool = std::max(maxPool, f.auth.snapshotPoolSize());
        // 两份之间的 50 ms 音频(48 kHz × 0.05 = 2400 样本):5 块 × 480。
        for (int b = 0; b < 5; ++b)
            runAudio(f, 480);
    }
    // 每 50 ms 一份都发出去了 —— 音频在跑时回收闸每一拍都开着,没把预览饿死。
    CHECK(f.auth.panCurvePreviewStats().publishes == 200);
    // 池上界 = 音频线程手上那份 + 刚发的那份。
    CHECK(maxPool <= 2);
    std::size_t alive = 0;
    for (const auto& w : seen)
        alive += w.expired() ? 0u : 1u;
    // 活着的预览表 = 发布中的那张 + 池里上一份钉着的那张;其余 198 张(约 25 MB)已放掉。
    CHECK(alive <= 2);
    CHECK(seen.front().expired());
}

TEST_CASE("AUTH-PARAMS-19 [J157] 占空比:烘一张表要 20 ms ⇒ 下一份最早 200 ms 后(消息线程 ≤ 10%)",
          "[authority][params][pancurve][j157]")
{
    AuthorityParamsFixture f;
    ackAudio(f);
    // 注入的烘表计时:每次烘表前后各读一次,差 = bakeCost。
    double bakeCost = 20.0;
    int reads = 0;
    f.auth.setPanCurvePreviewBakeClock([&] { return (reads++ % 2 == 0) ? 0.0 : bakeCost; });

    REQUIRE(f.auth.requestPanCurvePreview(1, bellAt0(-2.0f)) == PreviewReq::accepted);
    REQUIRE_FALSE(f.auth.pumpPanCurvePreview(0.0));
    CHECK(f.auth.panCurvePreviewStats().lastBakeMs == Approx(20.0));
    CHECK(f.auth.panCurvePreviewStats().intervalMs == Approx(200.0));
    ackAudio(f);

    REQUIRE(f.auth.requestPanCurvePreview(1, bellAt0(-3.0f)) == PreviewReq::accepted);
    CHECK(f.auth.pumpPanCurvePreview(199.0)); // 50 ms 的底线早过了,但占空比还没到
    CHECK_FALSE(f.auth.pumpPanCurvePreview(200.0));
    CHECK(f.auth.panCurvePreviewStats().publishes == 2);
    ackAudio(f);

    // 便宜的表(1 ms)⇒ 间隔回到 50 ms 底线,不是一直停在 200。
    bakeCost = 1.0;
    REQUIRE(f.auth.requestPanCurvePreview(1, bellAt0(-4.0f)) == PreviewReq::accepted);
    CHECK(f.auth.pumpPanCurvePreview(399.0));
    CHECK_FALSE(f.auth.pumpPanCurvePreview(400.0));
    CHECK(f.auth.panCurvePreviewStats().intervalMs == Approx(50.0));
    CHECK(f.auth.panCurvePreviewStats().maxBakeMs == Approx(20.0));
}

TEST_CASE("AUTH-PARAMS-20 [J157] 预览烘表的实测耗时(只报告,不判快慢)与由它推出的限速",
          "[authority][params][pancurve][j157][bench]")
{
    // 真计时源。数字随机器与构建档变,**不做快慢断言**(那会在慢机上红在与代码无关的地方);
    // 断的是「限速间隔确实由实测耗时推出、占空比不超过 10%」。读数见 WARN 输出与 PR 描述。
    AuthorityParamsFixture f;
    ackAudio(f);

    const auto mk = [](scvb::PanCurveShape shape, float angle, float q) {
        scvb::PanCurvePoint p;
        p.angle = angle;
        p.gainDb = -6.0f;
        p.shape = shape;
        p.q = q;
        if (shape == scvb::PanCurveShape::bell)
            p.side = scvb::PanCurveSide::out;
        else
            p.side = (angle >= 0.0f) ? scvb::PanCurveSide::right : scvb::PanCurveSide::left;
        return p;
    };
    struct Case
    {
        const char* name;
        std::vector<scvb::PanCurvePoint> points;
    };
    std::vector<Case> cases;
    cases.push_back({"1 bell", {mk(scvb::PanCurveShape::bell, 0.0f, 2.0f)}});
    cases.push_back({"4 mixed",
                     {mk(scvb::PanCurveShape::bell, -60.0f, 2.0f), mk(scvb::PanCurveShape::shelf, -20.0f, 2.0f),
                      mk(scvb::PanCurveShape::cut, 20.0f, 12.0f), mk(scvb::PanCurveShape::bell, 60.0f, 2.0f)}});
    {
        std::vector<scvb::PanCurvePoint> worst; // 已知上限:点数接近 16 **且**多数为 bell
        for (int i = 0; i < 16; ++i)
            worst.push_back(mk(scvb::PanCurveShape::bell, -90.0f + 12.0f * static_cast<float>(i), 2.0f));
        cases.push_back({"16 bell (worst)", worst});
    }

    double t = 0.0;
    for (const auto& c : cases)
    {
        REQUIRE(f.auth.requestPanCurvePreview(1, c.points) == PreviewReq::accepted);
        REQUIRE_FALSE(f.auth.pumpPanCurvePreview(t));
        const auto s = f.auth.panCurvePreviewStats();
        WARN("[J157 bake] " << c.name << ": " << s.lastBakeMs << " ms/preview, interval " << s.intervalMs << " ms ("
                            << 1000.0 / s.intervalMs << " Hz), message-thread duty "
                            << 100.0 * s.lastBakeMs / s.intervalMs << "%");
        CHECK(s.intervalMs == Approx(std::max(50.0, s.lastBakeMs / 0.10)));
        CHECK(s.lastBakeMs / s.intervalMs <= 0.10 + 1e-9);
        ackAudio(f);
        t += 10000.0;
    }
}

TEST_CASE("AUTH-PARAMS-21 [J157] 预览何时作废:撤回 / 已提交曲线变了 / 换版本 / 旧版本号 / 坏点",
          "[authority][params][pancurve][j157]")
{
    AuthorityParamsFixture f;
    zeroBakeClock(f);
    f.auth.setPanCurve(1, bellAt0(-3.0f));
    ackAudio(f);
    const auto committed = f.auth.activePanCurveLut();
    double t = 0.0;
    const auto preview = [&](float db) {
        REQUIRE(f.auth.requestPanCurvePreview(1, bellAt0(db)) == PreviewReq::accepted);
        REQUIRE_FALSE(f.auth.pumpPanCurvePreview(t));
        REQUIRE(f.auth.panCurvePreviewLive());
        t += 1000.0;
        ackAudio(f);
    };

    // ① 撤回 ⇒ 音频回到已提交那张。
    preview(-12.0f);
    REQUIRE(f.auth.arbiter().panCurveLut() == f.auth.panCurvePreviewLut().get());
    f.auth.cancelPanCurvePreview();
    CHECK_FALSE(f.auth.panCurvePreviewLive());
    CHECK_FALSE(f.auth.panCurvePreviewPending());
    runAudio(f, 64);
    CHECK(f.auth.arbiter().panCurveLut() == committed.get());

    // ② 拖动途中已提交曲线变了(撤销 / 重做 / 载入工程的形态)⇒ 这一版的预览作废,音频跟新的已提交曲线。
    preview(-12.0f);
    f.auth.setPanCurve(1, bellAt0(-6.0f));
    CHECK_FALSE(f.auth.panCurvePreviewLive());
    runAudio(f, 4096);
    CHECK(f.auth.arbiter().panCurveLut() == f.auth.activePanCurveLut().get());
    CHECK(f.auth.arbiter().panCurveLut()->gainDb(0.0f) == Approx(-6.0f).margin(0.03));

    // ②b 被限速拦着、还没发出去的那一份也一并作废 —— 否则它会在已提交曲线变了之后才发出去,
    //    把音频拽回拖动中的旧点表,而那时已经没有人在拖。
    //    ⚠ 夹具要做成「**只有待发、没有在发布中的**」:有在发布中的那份时,作废条件的另一半
    //    (live)会顺手把待发一起清掉,这一格就分辨不出「待发」那一半在不在(删除式实测过:
    //    第一版夹具带着 live,去掉「待发」那一半照样全绿)。先撤回再请求,正是这个形态 ——
    //    上一次手势刚撤回、50 ms 内又按下去。
    preview(-12.0f);
    f.auth.cancelPanCurvePreview();
    REQUIRE(f.auth.requestPanCurvePreview(1, bellAt0(-9.0f)) == PreviewReq::accepted);
    REQUIRE(f.auth.pumpPanCurvePreview(t - 990.0)); // 距上一份 10 ms:限速拦住,待发
    REQUIRE_FALSE(f.auth.panCurvePreviewLive()); // 前提:只有待发
    const auto publishesBefore = f.auth.panCurvePreviewStats().publishes;
    f.auth.setPanCurve(1, bellAt0(-5.0f));
    CHECK_FALSE(f.auth.panCurvePreviewPending());
    CHECK_FALSE(f.auth.pumpPanCurvePreview(t)); // 到点也不再发
    CHECK(f.auth.panCurvePreviewStats().publishes == publishesBefore);
    t += 1000.0;
    ackAudio(f);

    // ③ 换版本 ⇒ 作废;切回来不复活(只靠 rebindSources 按版本挑表的话,切回 V1 那一刻它会复活)。
    preview(-12.0f);
    f.auth.setVersionActive(2);
    CHECK_FALSE(f.auth.panCurvePreviewLive());
    f.auth.setVersionActive(1);
    runAudio(f, 4096);
    CHECK(f.auth.arbiter().panCurveLut() == f.auth.activePanCurveLut().get());
    CHECK(f.auth.arbiter().panCurveLut()->gainDb(0.0f) == Approx(-5.0f).margin(0.03));

    // ④ 旧版本号(UI 在「切版本已发出、回声未到」窗口里捕获的)⇒ staleVersion,什么都不记。
    CHECK(f.auth.requestPanCurvePreview(2, bellAt0(-9.0f)) == PreviewReq::staleVersion);
    CHECK_FALSE(f.auth.panCurvePreviewPending());
    // 坏点 ⇒ badPoints(与 setPanCurve 同一道闸),同样什么都不记。
    auto bad = bellAt0(-3.0f);
    bad[0].gainDb = std::numeric_limits<float>::quiet_NaN();
    CHECK(f.auth.requestPanCurvePreview(1, bad) == PreviewReq::badPoints);
    CHECK_FALSE(f.auth.panCurvePreviewPending());

    // ⑤ 拖回已提交的样子 ⇒ 借用已提交那张,不烘。
    const auto bakes = f.auth.panCurvePreviewStats().bakes;
    REQUIRE(f.auth.requestPanCurvePreview(1, bellAt0(-5.0f)) == PreviewReq::accepted);
    REQUIRE_FALSE(f.auth.pumpPanCurvePreview(t));
    CHECK(f.auth.panCurvePreviewStats().bakes == bakes);
    CHECK(f.auth.panCurvePreviewLut() == f.auth.activePanCurveLut());

    // ⑥ 版本层 state 往返(T18 `fromState`)改了激活版本 ⇒ 同 ③ 作废。
    REQUIRE(f.auth.panCurvePreviewLive());
    juce::ValueTree st = f.auth.toState();
    st.setProperty("active", 2, nullptr);
    f.auth.fromState(st);
    CHECK(f.auth.versionActive() == 2);
    CHECK_FALSE(f.auth.panCurvePreviewLive());
}
