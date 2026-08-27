// SPDX-License-Identifier: GPL-3.0-or-later
// test_bridge_args —— T29 桥面参数提取/白名单助手单测。
// 覆盖:PR#55 缺陷3 严格布尔提取;PR#55 第3轮重要1 归一化→工程值;重要2 gesture 白名单。
// 佐证 JUCE 8.0.8 var::operator bool() = type->toBool(value)(juce_Variant.cpp:567),对 bool 返回真值。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

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
// [SL-199] 隐藏期 scvb.params 吞帧后必须能补发。
//
// `emitParams` 的两层基线里,只有 `lastParamsJson_` 挡住了隐藏期丢帧;`lastParamsValues_`
// 在构建载荷时就推进了,与这一帧发没发出去无关。于是隐藏期某个 id 变了之后:
// 下一拍 `changed == false` → `values` 为空 → `any == false` **提前 return** → 载荷压根不再
// 构建,json 那层保护无从生效,这个变化**永远不会重发**。
// [J85] 之后冻结维度的读回值只在参数面上,后果就是旋钮一直显示旧值。
//
// 下面这一串按 25Hz 帧逐拍重演该场景,驱动的是**生产同一份**实现
// (`paramsForceFull` / `selectParamForEmit`,`emitParams` 的循环体就是后者)。
// ---------------------------------------------------------------------------
namespace
{
// 一帧 emitParams 的选择结果:本帧要下发哪些 id → 什么值(空 = 该帧不发事件)。
std::map<juce::String, float> emitFrame(std::map<juce::String, float>& baseline,
                                        const std::map<juce::String, float>& plane, bool firstFrame, bool visible,
                                        bool& wasVisible)
{
    // ⚠ takeVisibleEdge **必须无条件调用**(它要记账)—— 写成 `firstFrame || takeVisibleEdge(...)`
    // 会被短路:首帧不记账,于是下一拍才报边沿、白发一次全量。生产 emitTick 同款分两行写。
    const bool edge = scvb::output::takeVisibleEdge(visible, wasVisible);
    const bool forceFull = firstFrame || edge;
    std::map<juce::String, float> sent;
    for (const auto& kv : plane)
    {
        if (scvb::output::selectParamForEmit(baseline, kv.first, kv.second, forceFull))
        {
            sent[kv.first] = kv.second;
        }
    }
    // 真实 emitParams 在 `!any && !forceFull` 时提前 return;不可见时载荷还会被
    // emitEventIfBrowserIsVisible 丢掉 —— 两者对调用方是同一件事:这一帧没到达 UI。
    if (!visible)
    {
        sent.clear();
    }
    return sent;
}
} // namespace

TEST_CASE("BRIDGEARGS-SL199-1 隐藏期改参数 → 恢复可见必达且值正确", "[bridgeargs][params][SL199]")
{
    const juce::String kFrozenPan = "v1_t03_pan";
    std::map<juce::String, float> plane{{kFrozenPan, 10.0f}, {"v1_t03_vol", -3.0f}, {"width", 100.0f}};
    std::map<juce::String, float> baseline;
    bool wasVisible = false;

    // ① 首帧(可见):全量下发,基线建立。
    auto sent = emitFrame(baseline, plane, /*firstFrame=*/true, /*visible=*/true, wasVisible);
    REQUIRE(sent.size() == plane.size());
    REQUIRE(sent.at(kFrozenPan) == 10.0f);

    // ② 可见稳态:值没动 → 不重复发(diff 门照常生效,别为了修吞帧把 25Hz 变成全量广播)。
    sent = emitFrame(baseline, plane, false, /*visible=*/true, wasVisible);
    CHECK(sent.empty());

    // ③ 面板被宿主隐藏/折叠(editor 未销毁)。隐藏期该冻结 pan 被改掉(宿主自动化或手动写入)。
    plane[kFrozenPan] = 70.0f;
    sent = emitFrame(baseline, plane, false, /*visible=*/false, wasVisible);
    CHECK(sent.empty()); // 这一帧到不了 UI —— 但基线已经被推进到 70(这就是那个洞)
    CHECK(baseline.at(kFrozenPan) == 70.0f);

    // ④ 仍隐藏,再走几拍:值没再动,什么都不会发。
    for (int i = 0; i < 3; ++i)
    {
        CHECK(emitFrame(baseline, plane, false, false, wasVisible).empty());
    }

    // ⑤ ★ 恢复可见:必须补发,且值是**隐藏期改成的那个新值**。
    //    修复前:forceFull 恒 false → changed 恒 false(基线早就是 70)→ 一个 id 都不发,
    //    旋钮一直显示 10,直到该参数再变一次。
    sent = emitFrame(baseline, plane, false, /*visible=*/true, wasVisible);
    REQUIRE_FALSE(sent.empty());
    REQUIRE(sent.count(kFrozenPan) == 1);
    CHECK(sent.at(kFrozenPan) == 70.0f);
    CHECK(sent.size() == plane.size()); // 全量:隐藏期被吞的**所有** id 一并补上

    // ⑥ 补发之后回到稳态:不再重复全量(边沿只触发一次)。
    CHECK(emitFrame(baseline, plane, false, true, wasVisible).empty());

    // ⑦ 可见期正常改值:仍走稀疏 diff,只发变的那个。
    plane["v1_t03_vol"] = -9.0f;
    sent = emitFrame(baseline, plane, false, true, wasVisible);
    REQUIRE(sent.size() == 1);
    CHECK(sent.at("v1_t03_vol") == -9.0f);
}

TEST_CASE("BRIDGEARGS-SL199-2 takeVisibleEdge 的边沿记账", "[bridgeargs][params][SL199]")
{
    bool wasVisible = false;

    // 首次转可见:报边沿(首帧那一拍 emitTick 用的是 `first || edge`,两者叠加也只发一次全量)。
    CHECK(scvb::output::takeVisibleEdge(/*visibleNow=*/true, wasVisible));
    CHECK(wasVisible);
    CHECK_FALSE(scvb::output::takeVisibleEdge(true, wasVisible)); // 稳态可见:不再报

    // 可见 → 不可见:不报(这一帧本来就发不出去)。
    CHECK_FALSE(scvb::output::takeVisibleEdge(false, wasVisible));
    CHECK_FALSE(wasVisible);
    CHECK_FALSE(scvb::output::takeVisibleEdge(false, wasVisible)); // 持续隐藏:仍不报

    // 不可见 → 可见:**这一拍报**,且只报这一拍(take 语义)。
    CHECK(scvb::output::takeVisibleEdge(true, wasVisible));
    CHECK_FALSE(scvb::output::takeVisibleEdge(true, wasVisible));
}

TEST_CASE("BRIDGEARGS-SL199-4 segmentsResendNeeded:恢复可见补发全量快照", "[bridgeargs][segments][SL199]")
{
    using scvb::output::segmentsResendNeeded;
    const bool F = false;

    // 首帧必发(§0.4)。
    CHECK(segmentsResendNeeded(/*firstFrame=*/true, F, F, F, F, F));
    // 稳态:六个条件全假 → 不发(别把 25Hz 变成全量段表广播)。
    CHECK_FALSE(segmentsResendNeeded(F, F, F, F, F, F));
    // 四条既有触发面逐条仍然成立。
    CHECK(segmentsResendNeeded(F, F, /*analyzed=*/true, F, F, F));
    CHECK(segmentsResendNeeded(F, F, F, /*sampleRateChanged=*/true, F, F));
    CHECK(segmentsResendNeeded(F, F, F, F, /*crvsRevisionChanged=*/true, F));
    CHECK(segmentsResendNeeded(F, F, F, F, F, /*staleMaskChanged=*/true));
    // ★ 新增的第五条:不可见→可见的边沿。
    CHECK(segmentsResendNeeded(F, /*becameVisible=*/true, F, F, F, F));
}

TEST_CASE("BRIDGEARGS-SL199-5 隐藏期段表变化 → 恢复可见 snapshot 帧必达", "[bridgeargs][segments][SL199]")
{
    // 逐拍重演 emitTick 里那段:三个基线在**发之前**就推进,所以隐藏期丢掉的那一帧
    // 不会因为「条件还成立」而自愈 —— 只能靠可见性边沿补。
    std::uint32_t lastCrvsRev = 0;
    bool wasVisible = false;

    struct Frame
    {
        bool emitted = false;
        std::uint32_t revAtEmit = 0;
    };
    std::uint32_t crvsRev = 0;
    const auto tick = [&](bool firstFrame, bool visible) {
        const bool edge = scvb::output::takeVisibleEdge(visible, wasVisible);
        Frame f;
        if (scvb::output::segmentsResendNeeded(firstFrame, edge, /*analyzed=*/false, /*srChanged=*/false,
                                               crvsRev != lastCrvsRev, /*staleChanged=*/false))
        {
            lastCrvsRev = crvsRev; // ← 基线在这里推进(与生产同序:先推进,后 emit)
            f.emitted = visible; // 不可见时 emitEventIfBrowserIsVisible 丢掉
            f.revAtEmit = crvsRev;
        }
        return f;
    };

    // ① 首帧(可见):全量快照必发。
    CHECK(tick(/*firstFrame=*/true, /*visible=*/true).emitted);
    // ② 可见稳态:段表没动 → 不发。
    CHECK_FALSE(tick(false, true).emitted);

    // ③ 隐藏期加载了工程 / 分析回落 → CRVS 修订号变了。这一帧被丢掉,而基线已经跟上。
    crvsRev = 7;
    CHECK_FALSE(tick(false, /*visible=*/false).emitted);
    CHECK(lastCrvsRev == 7); // ← 洞:基线推进了,条件不再成立
    // ④ 仍隐藏若干拍:什么都不会发。
    for (int i = 0; i < 3; ++i)
    {
        CHECK_FALSE(tick(false, false).emitted);
    }

    // ⑤ ★ 恢复可见:必须补一帧 snapshot,且带的是隐藏期那个新修订号。
    //    修复前:条件恒假 → 段表陈旧到下一次段编辑 / undo / 切版本。
    const auto restored = tick(false, /*visible=*/true);
    REQUIRE(restored.emitted);
    CHECK(restored.revAtEmit == 7);

    // ⑥ 补发后回稳态:不再重复发(边沿只触发一拍)。
    CHECK_FALSE(tick(false, true).emitted);
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
    // forceFull:同值也入选(可见性边沿补发靠的就是这条)。
    CHECK(scvb::output::selectParamForEmit(baseline, "width", 80.0f, /*forceFull=*/true));
    // 浮点近似:approximatelyEqual 口径,极小抖动不算变(否则 25Hz 会一直发)。
    CHECK_FALSE(scvb::output::selectParamForEmit(baseline, "width", 80.0f, false));
}
