// SPDX-License-Identifier: GPL-3.0-or-later
// test_analysis_pipeline —— AnalysisPipeline 编排层单测(离线、合成特征)。
// 分析全链此前从未接线(handleAnalyze 是 T29 占位),v4 实测 P0-1「分析中卡死」即由此而来。
// 本文件只压编排层:喂合成的 kw 序列,断言 VAD → 谷切分 → 全局区间 → 指派 能真的出段。

#include <catch2/catch_test_macros.hpp>

#include <algorithm> // [SL-273] std::max(两路 maxAbsDiff)
#include <cmath>
#include <cstdint>
#include <vector>

#include "analysis/AnalysisPipeline.h"
#include "analysis/HopMath.h" // [SL-262] 采样点→hop 唯一口径
#include "analysis/BalanceBasis.h" // [SL-252] 平衡归一化基准 z(ADR-009 v2.2 澄清 ②)
// [SL-262] **回归守卫**:与 BalanceBasis.h 拉进来的 `analysis/LoudnessMode.h` 同时 include。
// 收敛之前这两个头各有一份同名同命名空间的 `LoudnessMode` / `SegmentLoudness`,本行会让
// 整个 TU 报 C2011。**别删这一行** —— 它不为用例服务,只为「第二份定义再也进不来」服务。
#include "analysis/Loudness.h"

using namespace scvb::analysis;

namespace
{

constexpr double kSr = 48000.0;
constexpr int kHopMs = 10;

// 造一条「有声/静音交替」的 kw_ms 序列(线性 K 加权均方)。
// loudMs/quietMs 各若干轮;有声段 kw = loudKw,静音段 kw ≈ 0。
PipelineTrackFeatures makeAlternating(int rounds, int loudHops, int quietHops, float loudKw)
{
    PipelineTrackFeatures f;
    for (int r = 0; r < rounds; ++r)
    {
        for (int i = 0; i < loudHops; ++i)
        {
            f.kwMs.push_back(loudKw);
            f.peak.push_back(std::sqrt(loudKw));
        }
        for (int i = 0; i < quietHops; ++i)
        {
            f.kwMs.push_back(1e-9f);
            f.peak.push_back(1e-5f);
        }
    }
    f.covered.assign(f.kwMs.size(), 1u);
    f.anyCovered = !f.kwMs.empty();
    return f;
}

PipelineConfig makeConfig(std::size_t numHops, int activeTracks)
{
    PipelineConfig cfg;
    cfg.sampleRate = kSr;
    cfg.hopMs = kHopMs;
    cfg.rangeStartSample = 0;
    cfg.rangeEndSample = static_cast<std::int64_t>(numHops) * static_cast<std::int64_t>(kHopMs * kSr / 1000.0);
    for (int t = 0; t < kPipelineTracks; ++t)
    {
        cfg.tracks[static_cast<std::size_t>(t)].enabled = (t < activeTracks);
    }
    return cfg;
}

} // namespace

TEST_CASE("PIPE-1 单轨有声/静音交替 → 切出多段", "[analysis][pipeline][t37]")
{
    // 每轮 80 hop 有声(0.8s)+ 60 hop 静音(0.6s),共 5 轮。
    auto feat = makeAlternating(5, 80, 60, 0.05f);
    const std::size_t n = feat.kwMs.size();

    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = feat;

    const auto cfg = makeConfig(n, 1);
    const auto res = runAnalysisPipeline(features, cfg);

    CHECK_FALSE(res.cancelled);
    CHECK(res.tracksTouched == 1);
    CHECK(res.intervals > 0);
    REQUIRE_FALSE(res.segments[0].empty()); // ← 核心:真的出段了

    for (std::size_t i = 0; i < res.segments[0].size(); ++i)
    {
        const auto& s = res.segments[0][i];
        CHECK(s.t1Samples > s.t0Samples);
        CHECK(s.pan >= -100.0);
        CHECK(s.pan <= 100.0);
        if (i > 0)
        {
            CHECK(s.t0Samples >= res.segments[0][i - 1].t1Samples);
        }
    }
}

TEST_CASE("PIPE-2 多轨 → 全局区间 + 指派,各轨都拿到 pan", "[analysis][pipeline][t37]")
{
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    // 三轨错开发声,保证全局区间里活跃集合会变化。
    features[0] = makeAlternating(4, 100, 60, 0.05f);
    features[1] = makeAlternating(4, 60, 100, 0.04f);
    features[2] = makeAlternating(4, 80, 80, 0.03f);
    const std::size_t n = features[0].kwMs.size();

    auto cfg = makeConfig(n, 3);
    const auto res = runAnalysisPipeline(features, cfg);

    CHECK(res.tracksTouched == 3);
    CHECK(res.intervals > 0);
    for (int t = 0; t < 3; ++t)
    {
        CHECK_FALSE(res.segments[static_cast<std::size_t>(t)].empty());
    }

    // 同一时刻多轨同时发声时,pan 不应全挤在正中(§5 指派的意义所在)。
    bool anyNonZeroPan = false;
    for (int t = 0; t < 3 && !anyNonZeroPan; ++t)
    {
        for (const auto& s : res.segments[static_cast<std::size_t>(t)])
        {
            if (std::abs(s.pan) > 1.0)
            {
                anyNonZeroPan = true;
                break;
            }
        }
    }
    CHECK(anyNonZeroPan);
}

TEST_CASE("PIPE-3 关掉的轨不产段;无覆盖的轨不产段", "[analysis][pipeline][t37]")
{
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = makeAlternating(4, 80, 60, 0.05f);
    features[1] = makeAlternating(4, 80, 60, 0.05f);
    const std::size_t n = features[0].kwMs.size();

    auto cfg = makeConfig(n, 2);
    cfg.tracks[1].enabled = false; // 轨 2 被关

    const auto res = runAnalysisPipeline(features, cfg);
    CHECK_FALSE(res.segments[0].empty());
    CHECK(res.segments[1].empty()); // 关掉的轨:一段都不该有

    // 轨 3 从未有覆盖 → 同样不产段。
    CHECK(res.segments[2].empty());
}

TEST_CASE("PIPE-4 取消:立即返回且标记 cancelled", "[analysis][pipeline][t37]")
{
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = makeAlternating(20, 80, 60, 0.05f);
    const std::size_t n = features[0].kwMs.size();
    const auto cfg = makeConfig(n, 1);

    const auto res = runAnalysisPipeline(features, cfg, {}, [] { return true; });
    CHECK(res.cancelled);
}

TEST_CASE("PIPE-5 非零起点范围(局部分析)照样出段", "[analysis][pipeline][t37]")
{
    // 局部分析(range 不从 0 开始)是最常见的路径之一:划了循环区再点分析。
    // hop 域的绝对/相对下标在这里最容易搞反 —— 搞反的表现就是「分析跑完但零段」。
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = makeAlternating(6, 80, 60, 0.05f);
    const std::size_t n = features[0].kwMs.size();

    auto cfg = makeConfig(n, 1);
    // 关键:把范围整体后移 —— 特征切片仍是 features[0](调用方按范围抠出来的那一段),
    // 但 rangeStartSample 非零,于是 firstHop != 0。
    const std::int64_t hopSamples = static_cast<std::int64_t>(kHopMs * kSr / 1000.0);
    const std::int64_t offsetHops = 4000;
    cfg.rangeStartSample = offsetHops * hopSamples;
    cfg.rangeEndSample = (offsetHops + static_cast<std::int64_t>(n)) * hopSamples;

    const auto res = runAnalysisPipeline(features, cfg);

    CHECK(res.intervals > 0);
    REQUIRE_FALSE(res.segments[0].empty()); // ← 下标搞反时这里是空的

    // 产出的段必须落在给定范围内(而不是从 0 开始)。
    for (const auto& sg : res.segments[0])
    {
        CHECK(sg.t0Samples >= cfg.rangeStartSample);
        CHECK(sg.t1Samples <= cfg.rangeEndSample);
    }
}

// ---------------------------------------------------------------------------
// [SL-206] 管线必须把 VAD **后验**灌进 PipelineResult —— 它是泳道绿线(§1.27 瓦片 vad 列)
// 唯一的数据源。此前 `runEnergyVad` 的第五参传的是 nullptr,后验算完就地扔掉,
// FrameStore 的 vadP 全仓没有生产者、真机恒 0。
// 这一组是**核心侧**的直接断言(比 host harness 便宜、更贴回归点,CLAUDE.md §7)。
// ---------------------------------------------------------------------------
TEST_CASE("PIPE-6 后验灌进 PipelineResult:参与轨有值、未参与轨留空", "[analysis][pipeline][vad][SL206]")
{
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = makeAlternating(4, 40, 30, 0.02f); // 轨1:参与
    features[1] = makeAlternating(4, 40, 30, 0.02f); // 轨2:有数据但下面会被 enabled=false 关掉
    // 轨3:**enabled 且 kwMs 非空,但 anyCovered=false** —— 管线的跳过判据是
    // `!tc.enabled || !f.anyCovered || f.kwMs.empty()` 三选一,三条得分开钉。
    // ⚠ 修复前这里只写了注释、没有真造出这个形状:`makeConfig(n, 1)` 把轨2/轨3 一起
    // enabled=false 了,于是两条空断言走的都是 `!tc.enabled`,`anyCovered=false` 那支
    // 一次都没被执行过(#151 复审【重要】2)。
    features[2] = makeAlternating(4, 40, 30, 0.02f);
    features[2].covered.assign(features[2].kwMs.size(), 0u); // 一个 hop 都没采到
    features[2].anyCovered = false;

    const std::size_t n = features[0].kwMs.size();
    PipelineConfig cfg = makeConfig(n, /*activeTracks=*/1); // 只有轨1 enabled
    cfg.tracks[2].enabled = true; // ★ 轨3 开着 —— 这样留空只可能是 anyCovered=false 挡的

    const PipelineResult r = runAnalysisPipeline(features, cfg);
    REQUIRE_FALSE(r.cancelled);

    // ★ 参与轨:后验长度 = kwMs 长度,且**不是全零**(全零就等于没接上)。
    const auto& p0 = r.vadPosterior[0];
    REQUIRE(p0.size() == n);
    bool anyNonZero = false;
    for (const float v : p0)
    {
        CHECK(v >= 0.0f);
        CHECK(v <= 1.0f); // 出口值域(EnergyVad 内已 clamp)
        if (v > 0.0f)
        {
            anyNonZero = true;
        }
    }
    CHECK(anyNonZero); // ← 修复前这里恒空/恒零

    // ★ 与能量形状相关:有声段的后验均值显著高于静音段。
    double loudSum = 0.0;
    double quietSum = 0.0;
    int loudN = 0;
    int quietN = 0;
    for (std::size_t k = 0; k < n; ++k)
    {
        if (features[0].kwMs[k] > 1e-6f)
        {
            loudSum += p0[k];
            ++loudN;
        }
        else
        {
            quietSum += p0[k];
            ++quietN;
        }
    }
    REQUIRE(loudN > 0);
    REQUIRE(quietN > 0);
    CHECK(loudSum / loudN > quietSum / quietN + 0.3);

    // ★ 未参与的轨留空(与 segments 同口径:关掉的轨 / 无覆盖的轨都不填)。
    CHECK(r.vadPosterior[1].empty()); // enabled=false 这一支
    // ★ 这一条走的是 anyCovered=false —— 轨3 是 enabled 的、kwMs 也非空,
    //   留空的唯一原因只能是无覆盖。反向验证:把上面那行 anyCovered 改回 true → 本条红。
    CHECK(r.vadPosterior[2].empty());
    CHECK(r.segments[2].empty()); // 同一支也不许产段
}

TEST_CASE("PIPE-7 后验的 firstHop 与局部分析范围一致", "[analysis][pipeline][vad][SL206]")
{
    // 写回 FrameStore 要靠 firstHop 定位绝对 hop —— 它必须等于 rangeStartSample/hopSamples,
    // 否则整条后验会被写到错误的时间位置上(绿线整体平移)。
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = makeAlternating(3, 40, 30, 0.02f);
    const std::size_t n = features[0].kwMs.size();

    PipelineConfig cfg = makeConfig(n, /*activeTracks=*/1);
    const std::int64_t hopSamples = static_cast<std::int64_t>(kHopMs * kSr / 1000.0);
    const std::int64_t startHop = 1234;
    cfg.rangeStartSample = startHop * hopSamples;
    cfg.rangeEndSample = cfg.rangeStartSample + static_cast<std::int64_t>(n) * hopSamples;

    const PipelineResult r = runAnalysisPipeline(features, cfg);
    REQUIRE_FALSE(r.cancelled);
    CHECK(r.firstHop == startHop);
    CHECK(r.vadPosterior[0].size() == n);
}

// ---------------------------------------------------------------------------
// [SL-252 / J95②a] 平衡归一化基准 z —— ADR-009 v2.2 澄清 ② 的落点。
//
// 修宪把两件事分开:**上报段响度 L_seg**(澄清 ①,不随 mode 变)与**归一化基准 z**
// (澄清 ②,按 mode 选档)。本用例只钉后者,钉三条:
//   ① 默认档**逐位**等于 `meanKw` —— 是 `==` 不是「约等于」。这条是硬要求:把默认档实现成
//      `10^(L/10)` 之类的等价换算,既有工程重分析后 pan/volDb 会发生肉眼不可见但逐位不同的
//      漂移。反向验证:把 KIntegrated 分支改成经 dB 往返,本节必红。
//   ② 三档**真分歧** —— 断链时代三档恒等,正是用户 v5.6.3 实测第 19 条「三个模式出来的结果
//      好像是一样的」;
//   ③ 三档**都是非负线性能量量** —— AutoAssign 要 `zSum += z` / `zHat = z/zSum`,塞 dB
//      (负数)进去 zSum 会变负。这条守的正是修宪「正文第三条继续完整适用」那句话。
//
// 放在本文件而不是 test_loudness.cpp 的原始理由已消失([SL-262] 已把 ODR 债收敛掉):
// 那时 `analysis/Loudness.h` 里另有一份**同名同命名空间**的 `LoudnessMode` /
// `SegmentLoudness`,两头一起 include 会 C2011 重定义。现在两个头**可以同时 include** ——
// 本文件顶部就同时引了它们,这本身就是那条 ODR 收敛的**回归守卫**:谁再引入第二份定义,
// 本 TU 编不过。用例留在这里不动(它测的是流水线层,本来就该在这个文件)。
// ---------------------------------------------------------------------------
TEST_CASE("[SL252] balanceBasisZ:默认档逐位等于 meanKw,三档真分歧且同为线性能量", "[analysis][balance][SL252]")
{
    // 刻意造成三档必然分歧的形状:能量集中在少数 hop(峰值高、均值低)。
    const std::vector<float> kw{0.04f, 0.0004f, 0.0004f, 0.0004f};
    const std::vector<float> peak{0.5f, 0.02f, 0.02f, 0.02f};
    const std::int64_t b = 0, e = 4;

    const double zK = balanceBasisZ(LoudnessMode::KIntegrated, kw, peak, b, e);
    const double zR = balanceBasisZ(LoudnessMode::Rms, kw, peak, b, e);
    const double zP = balanceBasisZ(LoudnessMode::PeakDbfs, kw, peak, b, e);

    // ① 默认档逐位相等(== 而非近似):同一个 meanKw、同一条代码路径。
    CHECK(zK == meanKw(kw, b, e));

    // ② 三档两两不等 —— 断链时代这三个值是同一个数。
    CHECK(zK != zR);
    CHECK(zK != zP);
    CHECK(zR != zP);

    // 数学口径逐档核对(与 ADR-009 v2.2 澄清 ② 的公式逐字对应)
    // 期望值必须由**同一批 float 输入**算出:`0.0004f` 不是精确的 0.0004,拿十进制字面量
    // 当期望会差出 ~2e-10 —— 那是浮点表示,不是实现错。
    double sumKw = 0.0, sumAmp = 0.0;
    for (const float v : kw)
    {
        sumKw += static_cast<double>(v);
        sumAmp += std::sqrt(static_cast<double>(v));
    }
    CHECK(std::abs(zK - sumKw / 4.0) < 1e-15); // mean(kw)
    const double meanAmp = sumAmp / 4.0;
    CHECK(std::abs(zR - meanAmp * meanAmp) < 1e-15); // (mean(√kw))²
    const double maxPeak = static_cast<double>(peak[0]);
    CHECK(std::abs(zP - maxPeak * maxPeak) < 1e-15); // max(peak)²

    // ③ 三档同为**非负线性能量**(AutoAssign 的 zSum / zHat 前提)。
    CHECK(zK >= 0.0);
    CHECK(zR >= 0.0);
    CHECK(zP >= 0.0);

    // 空窗 / 越界窗:三档一致回 0.0,不产出 NaN(下游 zSum 为 0 有既有分支接住)。
    for (const auto m : {LoudnessMode::KIntegrated, LoudnessMode::Rms, LoudnessMode::PeakDbfs})
    {
        CHECK(balanceBasisZ(m, kw, peak, 2, 2) == 0.0);
        CHECK(balanceBasisZ(m, kw, peak, 99, 100) == 0.0);
        CHECK_FALSE(std::isnan(balanceBasisZ(m, kw, peak, -5, 2)));
    }
}

// ---------------------------------------------------------------------------
// [SL-252 / J95②a] **流水线级**:loudness_mode 必须真的改变平衡产出。
//
// 上面那条 `[SL252]` 用例守的是纯函数 `balanceBasisZ` 本身,**守不住这次真正断掉的那一跳**
// (#168 复审【重要】):本卡的缺陷不是「函数算错」,是「函数没被调用」——
// `cfg.balance.loudnessMode` 在 `startAnalysis` 里漏赋值。只有纯函数用例的话,下面两种回归
// 照样全绿:① 删掉 `OutputProcessor.cpp` 那行 `cfg.balance.loudnessMode = ...`(= 完全回到
// 断链状态);② 把 `AnalysisPipeline.cpp` 里的 `balanceBasisZ(...)` 换回 `meanKw(...)`。
//
// 本仓对这类「守得住零件、守不住那一跳」的缺口有判例(`OutputEditor.cpp:857`:
// 「HOST R4 用例守的是降级链三函数本身,守不到『这里还在调它』这一跳…否则测试照绿而 bug 回归」)。
// 本用例就补在**用户第 19 条实测所处的观察层**:同一批特征、同一份配置,只换档,看产出变不变。
//
// ---------------------------------------------------------------------------
// [SL-273] **本用例此前名不副实,改的就是这一处。**
//
// 旧版有一个叫 `panOf()` 的取值器,名字写着 pan,身体里却把 `pan` 与 `volDb` 交错
// `push_back` 进同一个 `vector`,再对整条 vector 断言「至少有一位不同」。于是:
//   · 用例名与断言文字都说「换档 → pan 真变」;
//   · 而**真正让它变绿的是 volDb** —— 实测 peak 档下 pan **一位都没动**(见下)。
// 名不副实的用例比没有用例更坏:它让「pan 到底该不该变」这个问题看起来已经有人守着了。
//
// 拆开量之后的**实测**(seg-r3 探针,同一份素材、同一份配置,只换档):
//   ┌────────────┬──────────────────────┬──────────────────────┐
//   │            │ pan                  │ volDb                │
//   ├────────────┼──────────────────────┼──────────────────────┤
//   │ peak vs kw │ 逐位相同(maxDiff 0)│ maxDiff 1.75 dB      │
//   │ rms  vs kw │ 24 位里 8 位不同     │ maxDiff 1.42 dB      │
//   └────────────┴──────────────────────┴──────────────────────┘
//
// **pan 为什么可以不动**(`AutoAssign.cpp` 逐字):第一趟指派的代价 `baseCost()` 只读
// `priority` / `prevPan` / 槽位几何,**一个字都不读 z**;z 只在 `entryCost()` 里经
// `balHint->zHat` 进来,而 `balHint` 只有第二趟(`solveBalanceWithFallback` 的 level 2,
// 首趟 solveBalance 不收敛才走)才非空。所以:
//   · level 1 收敛 ⇒ pan 与 loudness_mode **无关**,换档只动 volDb(peak 那一行);
//   · 落到 level 2 ⇒ zHat 进了指派代价,pan 才可能跟着动(rms 那一行)。
// 换句话说 **volDb 才是「换档真的传到了平衡层」的必然信号,pan 只是偶然信号**。
// 所以下面按两路分开断言,且把必然的那一路(volDb)定为主断言。
//
// ⚠ 不要把 rms 那一行的「pan 变了 8 位」也写成断言:它取决于首趟收不收敛,
// 素材一动就可能翻面 —— 那是**记录**,不是判据。
// ---------------------------------------------------------------------------
TEST_CASE("[SL252/SL-273] 流水线级:换档 → volDb 真变、peak 档 pan 逐位不动;默认档与不设该字段逐位相同",
          "[analysis][pipeline][balance][SL252][SL273]")
{
    // 三轨占空比不同 ⇒ 「均值」与「峰值」给出的相对能量排序不同 ⇒ **平衡增益**分叉。
    // (旧注释这里写的是「指派结果必然分叉」—— [SL-273] 实测证伪:peak 档下 pan 逐位不动,
    //  分叉全在 volDb 上。指派代价不读 z,见用例头注。)
    // ⚠ 易脆性提示(#168 复审):`makeAlternating` 里 `peak = sqrt(kw)`,故 `max(peak)² ≡ max(kw)`,
    // `peak_dbfs` 与 `kw_integrated` 的分歧**完全来自「段内含次峰值 hop」**。将来若 VAD / 谷切分
    // 调参让段正好贴合响区,本用例会变成**假红**(方向安全:不是假绿,但排查成本不低)——
    // 那时该调的是这里的占空比,不是把断言放宽。
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = makeAlternating(4, 120, 40, 0.05f); // 长响短歇:均值高
    features[1] = makeAlternating(4, 30, 130, 0.20f); // 短促强峰:峰值高、均值低
    features[2] = makeAlternating(4, 80, 80, 0.03f);
    const std::size_t n = features[0].kwMs.size();

    // [SL-273] 两个取值器,**各取各的**。合成一条交错 vector 是旧版名不副实的根源。
    const auto pansOf = [](const PipelineResult& r) {
        std::vector<double> v;
        for (int t = 0; t < 3; ++t)
        {
            for (const auto& s : r.segments[static_cast<std::size_t>(t)])
            {
                v.push_back(s.pan);
            }
        }
        return v;
    };
    const auto volsOf = [](const PipelineResult& r) {
        std::vector<double> v;
        for (int t = 0; t < 3; ++t)
        {
            for (const auto& s : r.segments[static_cast<std::size_t>(t)])
            {
                v.push_back(s.volDb);
            }
        }
        return v;
    };
    // 「至少有一位差得过阈值」。阈值 0.1 dB = UI 显示步长:比它小的差用户看不见,
    // 拿来当「换档真的传到了」的证据太弱(实测信号 1.4~1.75 dB,留了一个数量级余量)。
    const auto maxAbsDiff = [](const std::vector<double>& a, const std::vector<double>& b) {
        double m = 0.0;
        for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        {
            m = std::max(m, std::abs(a[i] - b[i]));
        }
        return m;
    };

    // ① 不设该字段(= 修订前的行为)与显式默认档,**pan 与 volDb 都逐位相同**。
    //    这一条钉死「默认档不得走等价换算」——它与 `[SL252]` 纯函数用例的 `==` 互为里外。
    auto cfgDefault = makeConfig(n, 3);
    auto cfgK = makeConfig(n, 3);
    cfgK.balance.loudnessMode = LoudnessMode::KIntegrated;
    const auto rDefault = runAnalysisPipeline(features, cfgDefault);
    const auto rK = runAnalysisPipeline(features, cfgK);
    const auto panDefault = pansOf(rDefault);
    const auto volDefault = volsOf(rDefault);
    const auto panK = pansOf(rK);
    const auto volK = volsOf(rK);
    REQUIRE_FALSE(panDefault.empty());
    REQUIRE(panDefault.size() == panK.size());
    REQUIRE(volDefault.size() == volK.size());
    for (std::size_t i = 0; i < panDefault.size(); ++i)
    {
        CHECK(panDefault[i] == panK[i]); // 逐位,不是近似
        CHECK(volDefault[i] == volK[i]);
    }

    // ② 换到 peak_dbfs 档。**两路分开断,各钉各的**:
    //    · volDb **必须真变** —— 换档传到了平衡层的**必然**信号(z 只影响增益求解);
    //      断链时代这里恒等,那正是用户 v5.6.3 实测第 19 条看到的现象。
    //    · pan **必须逐位不动** —— 首趟指派代价一个字都不读 z(见用例头注)。
    //      这一条是新增的:谁把 z 引进 `baseCost()`,或让本该 level 1 收敛的解掉进
    //      level 2,这里立刻红。旧版把它和 volDb 揉在一条 `anyDiff` 里,等于没守。
    auto cfgP = makeConfig(n, 3);
    cfgP.balance.loudnessMode = LoudnessMode::PeakDbfs;
    const auto rP = runAnalysisPipeline(features, cfgP);
    const auto panP = pansOf(rP);
    const auto volP = volsOf(rP);
    REQUIRE(panP.size() == panK.size());
    REQUIRE(volP.size() == volK.size());
    // ⚠ **volDb 这一行钉住的只有回归 ②**(`AnalysisPipeline.cpp` 里 `balanceBasisZ(...)`
    // 被换回 `meanKw(...)`)。**钉不住回归 ①**(删掉 `OutputProcessor.cpp` 的
    // `cfg.balance.loudnessMode = ...`):本用例自己装配 cfg、**整条路径不经 startAnalysis**,
    // 那行删掉这里照样全绿。那一跳由 host 侧 `HOST SL263` 那条多轨用例接住。
    // 留在仓里被后人读到的是注释、不是 commit message,所以缺口写在这里而不只写在提交说明里。
    INFO("peak vs kw:volDb maxDiff = " << maxAbsDiff(volP, volK));
    CHECK(maxAbsDiff(volP, volK) > 0.1); // 实测 1.75 dB
    for (std::size_t i = 0; i < panP.size(); ++i)
    {
        CHECK(panP[i] == panK[i]);
    }

    // ③ rms 档:同样断 volDb 真变(三档两两分叉在纯函数层已钉,这里钉「传得到」)。
    //    ⚠ **pan 在这一档实测会变**(24 位里 8 位不同,maxDiff 15)—— 首趟 solveBalance
    //    不收敛、落到 level 2 的平衡感知重指派,zHat 于是进了指派代价。这是设计内的,
    //    但它取决于收敛与否、素材一动就可能翻面,所以**只记录、不断言**(写成断言
    //    会得到一条随机翻面的用例)。要断「level 2 这条路还通」得另立一条直接压
    //    `solveBalanceWithFallback` 的用例,那属 AutoAssign 的面,不在本文件。
    auto cfgR = makeConfig(n, 3);
    cfgR.balance.loudnessMode = LoudnessMode::Rms;
    const auto rR = runAnalysisPipeline(features, cfgR);
    const auto volR = volsOf(rR);
    REQUIRE(volR.size() == volK.size());
    INFO("rms vs kw:volDb maxDiff = " << maxAbsDiff(volR, volK));
    CHECK(maxAbsDiff(volR, volK) > 0.1); // 实测 1.42 dB
}

// ---------------------------------------------------------------------------
// [SL-262] 采样点 → hop 的唯一换算口径(`analysis/HopMath.h`)。
//
// 这条用例的存在理由([#169] 复审【重要】②):本卡真正**改变数值输出**的就是这处 hop 窗口
// 尾端的 off-by-one,而它原先只活在 `OutputProcessor::segmentLoudnessLufs` 里 ——
// 那是 `ScvbOutputAudioProcessor` 的成员,`scvb_tests` 够不着,于是「谁把它改回秒往返,
// 测试照绿」。抽成纯函数后就能在这里直接钉死(判例:`AnalyzeScopeMath.h` 头注)。
// ---------------------------------------------------------------------------
TEST_CASE("[SL262] hopWindowFromSamples:hop 边界不得被浮点截断到 k−1", "[analysis][hop][SL262]")
{
    using scvb::analysis::hopSamplesFor;
    using scvb::analysis::hopWindowFromSamples;

    constexpr double kHopSec = 0.01;
    constexpr double kSrLocal = 48000.0;
    const std::int64_t hopSamples = hopSamplesFor(kHopSec, kSrLocal);
    REQUIRE(hopSamples == 480);

    // ① 恰落 hop 边界的右端**必须**得到 k,不能是 k−1。
    //    旧的秒往返写法(`(k*hopSec) / hopSec`)在这些 k 上会截断 —— 实测 1..200000 里 9721 个。
    //    这里逐个复算,并顺带断言「旧写法确实会错」,免得这条用例退化成一句空话。
    // 扫全区间但**只累计、不逐个断言** —— 逐个 REQUIRE 会往套件里灌 60 万条断言,
    // 把「断言总数」这个本来就不该当基线的数字冲得更没意义(#168 复审【建议】D 的同族)。
    std::int64_t intWrong = 0;
    std::int64_t firstWrongK = 0;
    int floatWouldTruncate = 0;
    for (std::int64_t k = 1; k <= 200000; ++k)
    {
        const auto w = hopWindowFromSamples(0, k * hopSamples, kHopSec, kSrLocal);
        if (!w.valid || w.first != 0u || w.last != static_cast<std::uint64_t>(k))
        {
            if (intWrong == 0)
            {
                firstWrongK = k;
            }
            ++intWrong;
        }
        // 旧口径:采样点 → 秒 → hop 的浮点往返
        const double seconds = static_cast<double>(k * hopSamples) / kSrLocal;
        if (static_cast<std::int64_t>(seconds / kHopSec) != k)
        {
            ++floatWouldTruncate;
        }
    }
    INFO("首个出错的 k = " << firstWrongK);
    CHECK(intWrong == 0); // 整型口径:20 万个 hop 边界一个都不许错
    // 旧写法在这段区间里**确实**会错(数量级钉一下,避免将来有人以为这条防的是空气)。
    CHECK(floatWouldTruncate > 1000);

    // ② 不足一个 hop 的窗 ⇒ 不成立(first == last)。
    CHECK_FALSE(hopWindowFromSamples(0, hopSamples - 1, kHopSec, kSrLocal).valid);
    // ③ 空窗 / 倒序 ⇒ 不成立。
    CHECK_FALSE(hopWindowFromSamples(480, 480, kHopSec, kSrLocal).valid);
    CHECK_FALSE(hopWindowFromSamples(960, 480, kHopSec, kSrLocal).valid);
    // ④ 负采样点夹到 0(不越界读)。
    const auto neg = hopWindowFromSamples(-4800, 4800, kHopSec, kSrLocal);
    CHECK(neg.valid);
    CHECK(neg.first == 0u);
    CHECK(neg.last == 10u);
    // ⑤ 采样率非正 ⇒ 按 48000 兜底(与 prepareToPlay 同款,不造第二套)。
    CHECK(hopSamplesFor(kHopSec, 0.0) == 480);
    CHECK(hopSamplesFor(kHopSec, -1.0) == 480);
    // ⑥ hopSeconds 非正 ⇒ 换算不成立。
    CHECK(hopSamplesFor(0.0, kSrLocal) == 0);
    CHECK_FALSE(hopWindowFromSamples(0, 48000, 0.0, kSrLocal).valid);
}

// ---------------------------------------------------------------------------
// [SL-284] `maxFallbackLevel` 取的是**逐区间最坏**,不是末个区间的级。
//
// 为什么单开一条 core 用例(#183 复审【重要】):host 侧那两条钉不住这个语义 ——
//   · `HOST SL263`:两档全程 level 1,max / 末值 / 首值**恒等**;
//   · `HOST SL284`:冻结是全时段的,所有区间一起掉出 level 1,三者仍然恒等。
// 也就是说接线里唯一带判断的那一步(`std::max`)是**突变漏检**的:把它改成
// `result.maxFallbackLevel = br.fallbackLevel;`(末值口径)那两条照样全绿。
// 本条造出「**中间**区间掉级、最后一个区间收敛」的形状,让两种口径给出不同答案:
// max ⇒ >=2(对),末值 ⇒ 1(错)。改成末值 ⇒ 本条立刻红。
//
// 形状怎么造:一条**高能量且 pan 维冻结在硬左**的轨只在中段发声(配方同
// `tests/core/test_balance.cpp` 的「回退链 level 2」),两条弱的自由轨全程发声。
// 于是全局区间按那条轨的进出切成三段,中段被硬左的强轨拽偏、首趟收不进容差,
// 末段只剩两条对称的弱轨 ⇒ 回到 level 1。
// ---------------------------------------------------------------------------
TEST_CASE("[SL-284] maxFallbackLevel 取逐区间最坏值,不被末个收敛区间盖掉", "[analysis][pipeline][SL284]")
{
    // 逐段拼 kw 序列:loud 段给 kw,quiet 段给静音底噪。
    const auto push = [](PipelineTrackFeatures& f, int hops, float kw) {
        for (int i = 0; i < hops; ++i)
        {
            f.kwMs.push_back(kw);
            f.peak.push_back(std::sqrt(kw));
        }
    };
    constexpr int kPhase = 120; // 每段 1.2s,足够 VAD 起段
    constexpr float kQuiet = 1e-9f;

    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    // t0:只在**中段**发声,且能量远高于另两条 —— 它就是把中段拽偏的那条。
    push(features[0], kPhase, kQuiet);
    push(features[0], kPhase, 0.40f);
    push(features[0], kPhase, kQuiet);
    // t1/t2:全程发声、能量弱且彼此相当 —— 末段只剩它们时是对称解,必收敛。
    for (const int t : {1, 2})
    {
        push(features[static_cast<std::size_t>(t)], kPhase * 3, 0.02f);
    }
    for (auto& f : features)
    {
        if (f.kwMs.empty())
        {
            continue;
        }
        f.covered.assign(f.kwMs.size(), 1u);
        f.anyCovered = true;
    }

    auto cfg = makeConfig(features[0].kwMs.size(), 3);
    // t0 = manual 硬左(pan 维冻结)⇒ 它不占槽、pan 保持 −100,中段的 D 无从抵消。
    cfg.tracks[0].freeze = 1;
    cfg.tracks[0].currentPan = -100.0;

    const auto res = runAnalysisPipeline(features, cfg);
    REQUIRE_FALSE(res.cancelled);
    INFO("区间数 = " << res.intervals << ",maxFallbackLevel = " << res.maxFallbackLevel);
    REQUIRE(res.intervals >= 2); // 前置:只有一个区间时 max 与末值恒等,本条就成了空过
    // 改成末值口径 ⇒ 末段收敛 ⇒ 这里读到 1 ⇒ 红。
    CHECK(res.maxFallbackLevel >= 2);
}
