// SPDX-License-Identifier: GPL-3.0-or-later
// test_output_stage —— 输出级策略 + RampSwitcher + 滞回状态机单测(01 §5.1 步骤 6 / §8-2,J12/J32)。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "input/OutputStage.h"

using scvb::input::kPassthroughHysteresisMs;
using scvb::input::kStageRampMs;
using scvb::input::OutputStageMode;
using scvb::input::RampSwitcher;
using scvb::input::SilenceStage;
using scvb::input::StageSwitchStateMachine;

namespace
{
std::vector<float> makeSignal(int n)
{
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        v[static_cast<std::size_t>(i)] = 0.25f * static_cast<float>(i) - 0.5f;
    }
    return v;
}
} // namespace

TEST_CASE("常量可配且默认 kStageRampMs=80 / kPassthroughHysteresisMs=5000", "[input][outputstage]")
{
    REQUIRE(kStageRampMs == 80.0);
    REQUIRE(kPassthroughHysteresisMs == 5000.0);

    // 可配:自定义滞回 / 自定义 ramp 时长。
    StageSwitchStateMachine sm(1000.0);
    REQUIRE(sm.evaluate(false, 0) == OutputStageMode::kPassthrough); // 初始直通,不健康保持
    REQUIRE(sm.evaluate(true, 0) == OutputStageMode::kSilence);
    REQUIRE(sm.evaluate(false, 500) == OutputStageMode::kSilence);
    REQUIRE(sm.evaluate(false, 1500) == OutputStageMode::kPassthrough); // 1s 自定义滞回(自 500 起算)

    RampSwitcher rs;
    rs.prepare(48000.0, 40.0); // 40ms 自定义 ramp
    (void)rs;
}

TEST_CASE("SilenceStage 清零 / PassthroughStage 保持", "[input][outputstage]")
{
    const int n = 64;
    auto buf = makeSignal(n);
    float* ch[1] = {buf.data()};

    SilenceStage silence;
    silence.render(ch, 1, n);
    for (int i = 0; i < n; ++i)
    {
        REQUIRE(ch[0][i] == 0.0f);
    }

    auto buf2 = makeSignal(n);
    const auto orig = buf2;
    scvb::input::PassthroughStage passthrough;
    float* ch2[1] = {buf2.data()};
    passthrough.render(ch2, 1, n); // 直通无操作,逐样本一致
    REQUIRE(buf2 == orig);
}

TEST_CASE("无健康 Output 时直通稳态输出==输入逐样本一致", "[input][outputstage]")
{
    RampSwitcher rs;
    rs.prepare(48000.0);
    const int n = 128;
    auto buf = makeSignal(n);
    const auto orig = buf;
    float* ch[1] = {buf.data()};

    // 稳态直通(初始 theta=0):零开销,输出与输入逐样本一致(验收判据)。
    rs.render(ch, 1, n, OutputStageMode::kPassthrough);
    REQUIRE(buf == orig);
    REQUIRE(rs.isSettledPassthrough());
}

TEST_CASE("直通→静音经 80ms 等功率 ramp,稳态清零", "[input][outputstage]")
{
    RampSwitcher rs;
    rs.prepare(48000.0); // 80ms = 3840 样本 @48k
    const int n = 512;
    auto buf = makeSignal(n);
    const auto orig = buf;
    float* ch[1] = {buf.data()};

    // 开始切向静音:首块未完,中间样本已被衰减(非硬切)。
    rs.render(ch, 1, n, OutputStageMode::kSilence);
    REQUIRE_FALSE(rs.isSettledSilence());
    const float out100 = std::abs(buf[100]);
    REQUIRE(out100 > 0.0f); // 未到零
    REQUIRE(out100 < std::abs(orig[100])); // 已被衰减

    // 持续推进超过 80ms(3840 样本)→ 稳态静音;ramp 结束后余下样本清零。
    RampSwitcher rs2;
    rs2.prepare(48000.0);
    auto longBuf = makeSignal(4096);
    float* ch2[1] = {longBuf.data()};
    rs2.render(ch2, 1, 4096, OutputStageMode::kSilence);
    REQUIRE(rs2.isSettledSilence());
    for (int i = 3840; i < 4096; ++i) // 超过 ramp 时长的尾段必为零
    {
        REQUIRE(ch2[0][i] == 0.0f);
    }
}

TEST_CASE("5s 滞回只作用于 静音→直通 方向", "[input][outputstage]")
{
    StageSwitchStateMachine sm;
    REQUIRE(sm.target() == OutputStageMode::kPassthrough);

    // 不健康 → 保持直通(不评估)。
    REQUIRE(sm.evaluate(false, 0) == OutputStageMode::kPassthrough);

    // 健康 → 立即静音(此方向无滞回)。
    REQUIRE(sm.evaluate(true, 0) == OutputStageMode::kSilence);

    // 不健康 4000ms → 仍未满 5000ms 滞回,维持静音。
    REQUIRE(sm.evaluate(false, 4000) == OutputStageMode::kSilence);
    // 4999ms → 仍静音。
    REQUIRE(sm.evaluate(false, 8999) == OutputStageMode::kSilence);
    // 5000ms → 滞回届满,转直通。
    REQUIRE(sm.evaluate(false, 9000) == OutputStageMode::kPassthrough);

    // 恢复健康 → 立即静音(直通→静音无滞回)。
    REQUIRE(sm.evaluate(true, 10000) == OutputStageMode::kSilence);

    // forcePassthrough:改组/释放强制直通。
    sm.forcePassthrough();
    REQUIRE(sm.target() == OutputStageMode::kPassthrough);
}

TEST_CASE("planBlock:采集夹取、renderSamples 全块(PR#51 重要#2)", "[input][outputstage]")
{
    using scvb::input::planBlock;
    REQUIRE(planBlock(512, 512).captureSamples == 512);
    REQUIRE(planBlock(512, 512).renderSamples == 512);
    const auto big = planBlock(2048, 512);
    REQUIRE(big.captureSamples == 512);
    REQUIRE(big.renderSamples == 2048);
    REQUIRE(planBlock(0, 512).captureSamples == 0);
    REQUIRE(planBlock(0, 512).renderSamples == 0);
    const auto neg = planBlock(-1, 512);
    REQUIRE(neg.captureSamples == 0);
    REQUIRE(neg.renderSamples == 0);
}

TEST_CASE("大块静音档全块清零(PR#51 重要#2)", "[input][outputstage]")
{
    // 宿主大块(numSamples > preparedMaxBlock):渲染按全块,静音稳态下整块(含尾段)清零,无旧音频泄漏。
    RampSwitcher rs;
    rs.prepare(48000.0);
    const int n = 8192;
    auto buf = makeSignal(n);
    float* ch[1] = {buf.data()};
    rs.render(ch, 1, n, OutputStageMode::kSilence);
    REQUIRE(rs.isSettledSilence());
    for (int i = 3840; i < n; ++i)
    {
        REQUIRE(ch[0][i] == 0.0f); // ramp 结束后的全块尾段必须为零
    }
}

// ===========================================================================
// [SL-254 / J95①] RampSwitcher::snapTo —— 非实时(离线渲染)同块硬切。
// 复审 r1【重要】:新增 core 纯逻辑须有对应 Catch2 用例(CLAUDE.md §7);
// host harness 那几条是集成级,替代不了这里的语义钉死。
// ===========================================================================

TEST_CASE("SL-254:snapTo 立即进稳态(C19 muted 确认位的判据)", "[input][outputstage][SL254]")
{
    RampSwitcher rs;
    rs.prepare(48000.0);
    REQUIRE(rs.isSettledPassthrough()); // 初始档

    // ★ snapTo 之后**不需要再渲染任何样本**就已是稳态 —— [M] 的 muted 确认位(C19)读的就是
    //   这两个判定,theta_ 没设对会静默错位(注入与静音的先后就此错乱)。
    rs.snapTo(OutputStageMode::kSilence);
    REQUIRE(rs.isSettledSilence());
    REQUIRE_FALSE(rs.isSettledPassthrough());

    rs.snapTo(OutputStageMode::kPassthrough);
    REQUIRE(rs.isSettledPassthrough());
    REQUIRE_FALSE(rs.isSettledSilence());
}

TEST_CASE("SL-254:snapTo 后紧接 render 无中间增益(硬切,不是 ramp)", "[input][outputstage][SL254]")
{
    // ★ 这条是 J95① 裁定①「无 ramp」的可执行形态:ramp 也是墙钟量,80ms 在 250x 下会摊成
    //   20s 渐变,等于把「前段静音」换成「前段超长淡入」。硬切 ⇒ 首块就必须是终值。
    const int n = 512;

    {
        RampSwitcher rs;
        rs.prepare(48000.0);
        auto buf = makeSignal(n);
        float* ch[1] = {buf.data()};
        rs.snapTo(OutputStageMode::kSilence);
        rs.render(ch, 1, n, OutputStageMode::kSilence);
        for (int i = 0; i < n; ++i)
        {
            REQUIRE(ch[0][i] == 0.0f); // 整块清零,一个中间增益样本都不许有
        }
    }

    {
        // 反向:静音态硬切回直通,首块即原样(cos(0)=1),不得出现淡入。
        RampSwitcher rs;
        rs.prepare(48000.0);
        rs.snapTo(OutputStageMode::kSilence);
        auto buf = makeSignal(n);
        const auto ref = makeSignal(n);
        float* ch[1] = {buf.data()};
        rs.snapTo(OutputStageMode::kPassthrough);
        rs.render(ch, 1, n, OutputStageMode::kPassthrough);
        for (int i = 0; i < n; ++i)
        {
            REQUIRE(ch[0][i] == ref[static_cast<std::size_t>(i)]);
        }
    }
}

TEST_CASE("SL-254:snapTo 不改变实时 ramp 行为(未调用即仍是 80ms 渐变)", "[input][outputstage][SL254]")
{
    // 守住「实时路径一字不动」:不调 snapTo 时,render 仍走 80ms ramp —— 首块**不**是终值。
    RampSwitcher rs;
    rs.prepare(48000.0);
    const int n = 512; // < 3840(80ms@48k),故首块必落在 ramp 中途
    auto buf = makeSignal(n);
    float* ch[1] = {buf.data()};
    rs.render(ch, 1, n, OutputStageMode::kSilence);
    REQUIRE_FALSE(rs.isSettledSilence()); // 还在渐变途中,没到稳态
}
