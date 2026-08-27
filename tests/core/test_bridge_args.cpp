// SPDX-License-Identifier: GPL-3.0-or-later
// test_bridge_args —— T29 桥面参数提取/白名单助手单测。
// 覆盖:PR#55 缺陷3 严格布尔提取;PR#55 第3轮重要1 归一化→工程值;重要2 gesture 白名单。
// 佐证 JUCE 8.0.8 var::operator bool() = type->toBool(value)(juce_Variant.cpp:567),对 bool 返回真值。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <map>

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

TEST_CASE("BRIDGEARGS-SEGMODE-1 segmentation.mode 白名单(PR#55 第6轮缺陷2)", "[bridgeargs][segmode]")
{
    REQUIRE(scvb::output::isSegmentationMode("vad_only"));
    REQUIRE(scvb::output::isSegmentationMode("valley"));
    REQUIRE_FALSE(scvb::output::isSegmentationMode("energi")); // 拼错拒绝
    REQUIRE_FALSE(scvb::output::isSegmentationMode("energy")); // 旧默认值已废
    REQUIRE_FALSE(scvb::output::isSegmentationMode(""));
}

TEST_CASE("BRIDGEARGS-S2S-1 样本→秒安全换算(PR#55 第6轮缺陷1)", "[bridgeargs][s2s]")
{
    REQUIRE(scvb::output::samplesToSeconds(48000, 48000.0) == Approx(1.0));
    REQUIRE(scvb::output::samplesToSeconds(0, 48000.0) == Approx(0.0));
    REQUIRE(scvb::output::samplesToSeconds(48000, 0.0) == Approx(0.0)); // 零除守卫 → 0.0 哨兵

    // 「无末端」哨兵(1<<40)在这里**照常换算,不做裁剪**。
    //
    // 曾经试过在本函数按值域把超大采样数夹成 0.0 来挡 P0-A —— 那会连带把哨兵夹成 0,
    // 于是手动/冻结段的 t1S=0 < t0S,段在波形页上直接消失、点不中、切不开。
    // 哨兵是**语义**问题,归产生它的 emitSegments 按语义降级(见那里的行注);
    // 本函数只保证「不产 NaN/inf」这一条,不越权替上层做语义判断。
    REQUIRE(scvb::output::samplesToSeconds(scvb::output::kOpenEndedT1, 48000.0) ==
            Approx(22906492.245333).epsilon(1e-9));
    REQUIRE(scvb::output::kOpenEndedT1 == (static_cast<std::int64_t>(1) << 40));
    REQUIRE(scvb::output::samplesToSeconds(48000, -1.0) == Approx(0.0));
}

// ---------------------------------------------------------------------------
// [SL-199] 隐藏期 scvb.params / scvb.segments 吞帧后必须能补发。
//
// `emitParams` 的两层基线里,只有 `lastParamsJson_` 挡住了隐藏期丢帧;`lastParamsValues_`
// 在构建载荷时就推进了,与这一帧发没发出去无关。于是隐藏期某个 id 变了之后:
// 下一拍 `changed == false` → `values` 为空 → `any == false` **提前 return** → 载荷压根不再
// 构建,json 那层保护无从生效,这个变化**永远不会重发**。
// [J85] 之后冻结维度的读回值只在参数面上,后果就是旋钮一直显示旧值。
// `scvb.segments` 是同一个洞,而且连 json 那半层保护都没有。
//
// 补发用的是**闩锁**而不是一次性边沿(#119 复审重要):不可见→可见置位,**确认这一帧真的
// 发出去了才清位**。下面按 25Hz 帧逐拍重演,驱动的是**生产同一份**实现。
// ---------------------------------------------------------------------------
namespace
{
// 一帧 emitParams 的结果:本帧下发了哪些 id → 什么值;以及这一帧究竟有没有到达 UI。
struct ParamsFrame
{
    std::map<juce::String, float> sent;
    bool delivered = false;
};

ParamsFrame emitFrame(std::map<juce::String, float>& baseline, const std::map<juce::String, float>& plane,
                      bool firstFrame, bool visible, bool& wasVisible, bool& pendingFull, bool browserSwallows = false)
{
    // 与生产 emitTick 同序:先置闩锁,再按 `first || pending` 决定要不要强制全量。
    scvb::output::raiseResendLatch(visible, wasVisible, pendingFull);
    const bool forceFull = firstFrame || pendingFull;

    ParamsFrame f;
    for (const auto& kv : plane)
    {
        if (scvb::output::selectParamForEmit(baseline, kv.first, kv.second, forceFull))
        {
            f.sent[kv.first] = kv.second;
        }
    }
    // `browserSwallows` = 复审点名的那一拍:isVisible() 已翻真,但 JUCE 内部判据还没跟上,
    // 载荷被丢。C++ 侧观察到的「已下发」这时就是 false —— 闩锁必须保持。
    f.delivered = visible && !browserSwallows;
    if (!f.delivered)
    {
        f.sent.clear();
    }
    scvb::output::settleResendLatch(f.delivered, pendingFull);
    return f;
}
} // namespace

TEST_CASE("BRIDGEARGS-SL199-1 隐藏期改参数 → 恢复可见必达且值正确", "[bridgeargs][params][SL199]")
{
    const juce::String kFrozenPan = "v1_t03_pan";
    std::map<juce::String, float> plane{{kFrozenPan, 10.0f}, {"v1_t03_vol", -3.0f}, {"width", 100.0f}};
    std::map<juce::String, float> baseline;
    bool wasVisible = false;
    bool pendingFull = false;

    // ① 首帧(可见):全量下发,基线建立,闩锁随即清掉。
    auto f = emitFrame(baseline, plane, /*firstFrame=*/true, /*visible=*/true, wasVisible, pendingFull);
    REQUIRE(f.sent.size() == plane.size());
    REQUIRE(f.sent.at(kFrozenPan) == 10.0f);
    CHECK_FALSE(pendingFull);

    // ② 可见稳态:值没动 → 不重复发(diff 门照常生效,别为了修吞帧把 25Hz 变成全量广播)。
    CHECK(emitFrame(baseline, plane, false, true, wasVisible, pendingFull).sent.empty());

    // ③ 面板被宿主隐藏/折叠(editor 未销毁)。隐藏期该冻结 pan 被改掉(宿主自动化或手动写入)。
    plane[kFrozenPan] = 70.0f;
    CHECK(emitFrame(baseline, plane, false, /*visible=*/false, wasVisible, pendingFull).sent.empty());
    CHECK(baseline.at(kFrozenPan) == 70.0f); // 基线已被推进 —— 这就是那个洞

    // ④ 仍隐藏,再走几拍:什么都不会发。
    for (int i = 0; i < 3; ++i)
    {
        CHECK(emitFrame(baseline, plane, false, false, wasVisible, pendingFull).sent.empty());
    }

    // ⑤ ★ 恢复可见,但**这一帧恰好被浏览器侧吞掉**(复审点名的那一拍:isVisible() 已翻真、
    //    JUCE 内部判据还没跟上)。一次性边沿到此就把机会用光了;闩锁必须**保持置位**。
    CHECK(emitFrame(baseline, plane, false, true, wasVisible, pendingFull, /*browserSwallows=*/true).sent.empty());
    CHECK(pendingFull); // ★ 闩锁没被消费掉

    // ⑥ ★ 下一拍仍然补:值是隐藏期改成的那个新值,且全量补齐。
    f = emitFrame(baseline, plane, false, true, wasVisible, pendingFull);
    REQUIRE(f.delivered);
    REQUIRE(f.sent.count(kFrozenPan) == 1);
    CHECK(f.sent.at(kFrozenPan) == 70.0f);
    CHECK(f.sent.size() == plane.size());
    CHECK_FALSE(pendingFull);

    // ⑦ 补发之后回到稳态:不再重复全量。
    CHECK(emitFrame(baseline, plane, false, true, wasVisible, pendingFull).sent.empty());

    // ⑧ 可见期正常改值:仍走稀疏 diff,只发变的那个。
    plane["v1_t03_vol"] = -9.0f;
    f = emitFrame(baseline, plane, false, true, wasVisible, pendingFull);
    REQUIRE(f.sent.size() == 1);
    CHECK(f.sent.at("v1_t03_vol") == -9.0f);
}

TEST_CASE("BRIDGEARGS-SL199-2 闩锁的置位与清位", "[bridgeargs][params][SL199]")
{
    using scvb::output::raiseResendLatch;
    using scvb::output::settleResendLatch;
    bool wasVisible = false;
    bool pending = false;

    // 首次转可见:置位。
    raiseResendLatch(/*visibleNow=*/true, wasVisible, pending);
    CHECK(pending);
    CHECK(wasVisible);
    // 没发出去 → **不清位**,下一拍继续补。这就是闩锁相对一次性边沿的全部价值。
    settleResendLatch(/*sent=*/false, pending);
    CHECK(pending);
    // 稳态可见不会重复置位;确认发出去了才清。
    raiseResendLatch(true, wasVisible, pending);
    settleResendLatch(true, pending);
    CHECK_FALSE(pending);
    raiseResendLatch(true, wasVisible, pending);
    CHECK_FALSE(pending); // 已清,稳态不再置位

    // 可见 → 不可见:不置位(这一帧本来就发不出去)。
    raiseResendLatch(false, wasVisible, pending);
    CHECK_FALSE(pending);
    CHECK_FALSE(wasVisible);
    raiseResendLatch(false, wasVisible, pending); // 持续隐藏:仍不置位
    CHECK_FALSE(pending);

    // 不可见 → 可见:置位;这次发出去了 → 清位。
    raiseResendLatch(true, wasVisible, pending);
    CHECK(pending);
    settleResendLatch(true, pending);
    CHECK_FALSE(pending);
}

TEST_CASE("BRIDGEARGS-SL199-3 selectParamForEmit 的选择与基线推进", "[bridgeargs][params][SL199]")
{
    std::map<juce::String, float> baseline;

    // 新 id:恒入选(基线里没有)。
    CHECK(scvb::output::selectParamForEmit(baseline, "width", 100.0f, /*forceFull=*/false));
    CHECK(baseline.at("width") == 100.0f);
    // 同值:不入选(25Hz 的稀疏 diff 门)。
    CHECK_FALSE(scvb::output::selectParamForEmit(baseline, "width", 100.0f, false));
    // 变值:入选并推进基线。
    CHECK(scvb::output::selectParamForEmit(baseline, "width", 80.0f, false));
    CHECK(baseline.at("width") == 80.0f);
    // forceFull:同值也入选(闩锁补发靠的就是这条)。
    CHECK(scvb::output::selectParamForEmit(baseline, "width", 80.0f, /*forceFull=*/true));

    // 比较口径 = **逐位精确**,不是「容差内不算变」(#119 复审:float 加宽成 double 之后,
    // approximatelyEqual<double> 的容差比相邻 float 的间距小八个数量级,等价于 !=)。
    // 断言这一点的正确方式是取**相邻的那个 float**:它必须被判成「变了」。
    // (上一版这里传的是完全相同的 80.0f,只是把「同值不发」重复了一遍,一点容差语义都没覆盖。)
    const float kNext = std::nextafter(80.0f, 100.0f);
    REQUIRE(kNext != 80.0f);
    CHECK(scvb::output::selectParamForEmit(baseline, "width", kNext, false)); // 1 ULP 也算「变了」
    CHECK(baseline.at("width") == kNext);
    CHECK_FALSE(scvb::output::selectParamForEmit(baseline, "width", kNext, false)); // 同值仍不发
}

TEST_CASE("BRIDGEARGS-SL199-4 segmentsResendNeeded:六个触发面", "[bridgeargs][segments][SL199]")
{
    using scvb::output::segmentsResendNeeded;
    const bool F = false;

    CHECK(segmentsResendNeeded(/*firstFrame=*/true, F, F, F, F, F)); // 首帧必发(§0.4)
    CHECK_FALSE(segmentsResendNeeded(F, F, F, F, F, F)); // 稳态全假 → 不发
    CHECK(segmentsResendNeeded(F, F, /*analyzed=*/true, F, F, F));
    CHECK(segmentsResendNeeded(F, F, F, /*sampleRateChanged=*/true, F, F));
    CHECK(segmentsResendNeeded(F, F, F, F, /*crvsRevisionChanged=*/true, F));
    CHECK(segmentsResendNeeded(F, F, F, F, F, /*staleMaskChanged=*/true));
    CHECK(segmentsResendNeeded(F, /*pendingFull=*/true, F, F, F, F)); // ★ 新增:闩锁位
}

TEST_CASE("BRIDGEARGS-SL199-5 隐藏期段表变化 → 恢复可见 snapshot 必达(补发帧被吞也接着补)",
          "[bridgeargs][segments][SL199]")
{
    // 逐拍重演 emitTick 里那段:三个基线在**发之前**就推进,隐藏期丢掉的那一帧不会自愈。
    // segments 这一路连 params 的 `lastParamsJson_` 半层保护都没有,更依赖闩锁。
    std::uint32_t lastCrvsRev = 0;
    std::uint32_t crvsRev = 0;
    bool wasVisible = false;
    bool pendingFull = false;
    bool pendingAnalyzed = false;

    struct Frame
    {
        bool emitted = false;
        std::uint32_t revAtEmit = 0;
        juce::String reason;
    };

    const auto tick = [&](bool firstFrame, bool visible, bool analyzedNow, bool browserSwallows = false) {
        scvb::output::raiseResendLatch(visible, wasVisible, pendingFull);
        pendingAnalyzed = pendingAnalyzed || analyzedNow; // takeAnalysisDone 取走即清 → 必须闩住
        Frame f;
        if (scvb::output::segmentsResendNeeded(firstFrame, pendingFull, pendingAnalyzed, /*srChanged=*/false,
                                               crvsRev != lastCrvsRev, /*staleChanged=*/false))
        {
            lastCrvsRev = crvsRev; // ← 基线在这里推进(与生产同序:先推进,后 emit)
            f.reason = pendingAnalyzed ? "analyze" : "snapshot";
            f.emitted = visible && !browserSwallows;
            f.revAtEmit = crvsRev;
            scvb::output::settleResendLatch(f.emitted, pendingFull);
            scvb::output::settleResendLatch(f.emitted, pendingAnalyzed);
        }
        return f;
    };

    // ① 首帧(可见)全量快照必发;② 可见稳态不发。
    CHECK(tick(/*firstFrame=*/true, /*visible=*/true, /*analyzedNow=*/false).emitted);
    CHECK_FALSE(tick(false, true, false).emitted);

    // ③ 隐藏期:分析跑完(analyzed 被 takeAnalysisDone 取走)且 CRVS 修订号变了。这一帧被丢掉,
    //    三个基线却已经跟上 —— 洞就在这里。
    crvsRev = 7;
    CHECK_FALSE(tick(false, /*visible=*/false, /*analyzedNow=*/true).emitted);
    CHECK(lastCrvsRev == 7);
    CHECK(pendingAnalyzed); // analyzed 被闩住,没随那一帧一起丢掉

    // ④ 恢复可见,但补发帧自己被浏览器侧吞掉 → 两个闩锁都必须保持。
    CHECK_FALSE(tick(false, /*visible=*/true, false, /*browserSwallows=*/true).emitted);
    CHECK(pendingFull);
    CHECK(pendingAnalyzed);

    // ⑤ ★ 下一拍仍然补:带隐藏期那个新修订号,且 **reason 仍是 "analyze"** ——
    //    落成 "snapshot" 会让 Tab4 的「参数已改、结果陈旧」基线同步与 Tab3 的分析 diff 摘要条
    //    静默失效(它们只认 reason==="analyze")。
    const auto restored = tick(false, true, false);
    REQUIRE(restored.emitted);
    CHECK(restored.revAtEmit == 7);
    CHECK(restored.reason == juce::String("analyze"));
    CHECK_FALSE(pendingFull);
    CHECK_FALSE(pendingAnalyzed);

    // ⑥ 补发后回稳态:不再重复发。
    CHECK_FALSE(tick(false, true, false).emitted);
}
