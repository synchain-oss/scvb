// SPDX-License-Identifier: GPL-3.0-or-later
// test_lead_timeline —— [SL-216 / J136] 主唱进分析:lead_select 时间线记录 + 管线逐区间并入集合 C。
//
// 三层各钉各的:
//   · LeadTimeline / majorityLead / LeadRecorder / LEAD 编解码 —— 纯数据结构;
//   · runAnalysisPipeline —— 记录里选中的那一轨在该区间居中、不占槽,其余声部按剩下的轨数排槽;
//     与「把那一轨设成 lead_lock」逐位同解(同一条 C 路径,平衡也一样);没有记录且当前值为 0 ⇒ 与改动前同解。
//   · [SL-545 / J143 + J143a] 点分析那一刻的 lead_select(cfg.leadFallback)怎么进来:窗里的记录 ≤ 1 个值 ⇒
//     整窗用它;≥ 2 个值 ⇒ 按记录,没有记录的区间才用它 —— 标签 [sl545]。
// 接线(Output 播放时记录 / 分析取记录 / 随工程存取 / 当前值取参数面)在 tests/host/test_host_harness.cpp 的
// [sl216] 与 [sl545]。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "analysis/AnalysisPipeline.h"
#include "analysis/LeadTimeline.h"

using namespace scvb::analysis;

namespace
{

constexpr double kSr = 48000.0;
constexpr int kHopMs = 10;
constexpr std::int64_t kHopSamples = 480;

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
    cfg.rangeEndSample = static_cast<std::int64_t>(numHops) * kHopSamples;
    for (int t = 0; t < kPipelineTracks; ++t)
    {
        cfg.tracks[static_cast<std::size_t>(t)].enabled = (t < activeTracks);
    }
    return cfg;
}

// 三轨同时发声(同一包络,能量相同)—— 每个区间三轨都活跃,指派结果只由槽位规则决定。
// 轨 1(下标 0)优先级压到 0:优先级高 → 角度大(02 §5.3 p_target),所以最低的那条拿中心槽;
// 这样「选轨 2 当主唱」与「没选」给出的中心轨不同,两种结果可区分。
struct ThreeVoices
{
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    PipelineConfig cfg;
    ThreeVoices()
    {
        for (int t = 0; t < 3; ++t)
        {
            features[static_cast<std::size_t>(t)] = makeAlternating(4, 80, 60, 0.05f);
        }
        cfg = makeConfig(features[0].kwMs.size(), 3);
        cfg.tracks[0].priority = 0.0;
    }
};

bool allPansAre(const std::vector<AnalysisSegment>& segs, double v)
{
    if (segs.empty())
    {
        return false;
    }
    for (const auto& s : segs)
    {
        if (std::abs(s.pan - v) > 1e-9)
        {
            return false;
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// LeadTimeline
// ---------------------------------------------------------------------------

TEST_CASE("SL216 时间线:连续同值写入合并成一段", "[analysis][lead][sl216]")
{
    LeadTimeline tl;
    for (int b = 0; b < 100; ++b)
    {
        CHECK(tl.write(b * 512, (b + 1) * 512, 3));
    }
    const auto runs = tl.runs();
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].t0 == 0);
    CHECK(runs[0].t1 == 100 * 512);
    CHECK(runs[0].lead == 3);
}

TEST_CASE("SL216 时间线:后写覆盖先写(中间劈开、两端保留)", "[analysis][lead][sl216]")
{
    LeadTimeline tl;
    CHECK(tl.write(0, 1000, 1));
    CHECK(tl.write(400, 600, 2)); // 劈开
    auto runs = tl.runs();
    REQUIRE(runs.size() == 3);
    CHECK((runs[0].t0 == 0 && runs[0].t1 == 400 && runs[0].lead == 1));
    CHECK((runs[1].t0 == 400 && runs[1].t1 == 600 && runs[1].lead == 2));
    CHECK((runs[2].t0 == 600 && runs[2].t1 == 1000 && runs[2].lead == 1));

    // 跨三段覆盖:左截、中删、右截。
    CHECK(tl.write(300, 700, 0));
    runs = tl.runs();
    REQUIRE(runs.size() == 3);
    CHECK((runs[0].t0 == 0 && runs[0].t1 == 300 && runs[0].lead == 1));
    CHECK((runs[1].t0 == 300 && runs[1].t1 == 700 && runs[1].lead == 0));
    CHECK((runs[2].t0 == 700 && runs[2].t1 == 1000 && runs[2].lead == 1));

    // 用原值盖回中间:三段合回一段(两侧相接同值合并)。
    CHECK(tl.write(300, 700, 1));
    runs = tl.runs();
    REQUIRE(runs.size() == 1);
    CHECK((runs[0].t0 == 0 && runs[0].t1 == 1000 && runs[0].lead == 1));
}

TEST_CASE("SL216 时间线:非法写入忽略", "[analysis][lead][sl216]")
{
    LeadTimeline tl;
    CHECK_FALSE(tl.write(100, 100, 1)); // 空
    CHECK_FALSE(tl.write(100, 50, 1)); // 倒序
    CHECK_FALSE(tl.write(-10, 50, 1)); // 负起点
    CHECK_FALSE(tl.write(0, 50, 16)); // 越界
    CHECK_FALSE(tl.write(0, 50, -1));
    CHECK(tl.empty());
}

TEST_CASE("SL216 时间线:runsOverlapping 取与窗相交的段(含跨左边界那段)", "[analysis][lead][sl216]")
{
    LeadTimeline tl;
    tl.write(0, 100, 1);
    tl.write(100, 200, 2);
    tl.write(200, 300, 3);
    const auto r = tl.runsOverlapping(150, 250);
    REQUIRE(r.size() == 2);
    CHECK(r[0].lead == 2);
    CHECK(r[1].lead == 3);
    CHECK(tl.runsOverlapping(300, 400).empty());
}

TEST_CASE("SL216 多数值:按已记录样本数取最多,平局取小,未记录不算", "[analysis][lead][sl216]")
{
    const std::vector<LeadRun> runs{{0, 300, 2}, {300, 1000, 5}};
    CHECK(majorityLead(runs, 0, 1000) == 5);
    CHECK(majorityLead(runs, 0, 400) == 2);
    CHECK(majorityLead(runs, 200, 400) == 2); // 100 vs 100 平局 → 小的
    CHECK(majorityLead(runs, 1000, 2000) == 0); // 全未记录
    // 窗里只有一小截有记录:仍按有记录的那部分定(未记录不稀释)。
    CHECK(majorityLead(runs, 900, 5000) == 5);
    // 0 是一个值,参与计数。
    const std::vector<LeadRun> withZero{{0, 600, 0}, {600, 1000, 4}};
    CHECK(majorityLead(withZero, 0, 1000) == 0);
}

TEST_CASE("SL545 多数值:整段没有记录 → 回落值;有一个已记录样本就按记录(记录的 0 也压过回落值)",
          "[analysis][lead][sl545]")
{
    const std::vector<LeadRun> runs{{0, 300, 2}, {300, 1000, 5}};
    CHECK(majorityLead(runs, 1000, 2000, 7) == 7); // 全未记录 → 回落值
    CHECK(majorityLead({}, 0, 1000, 7) == 7); // 一条记录都没有 → 回落值
    CHECK(majorityLead(runs, 1000, 1000, 7) == 7); // 空区间:没有已记录样本,同一条定义
    // 窗里只有一小截有记录:仍按记录定,回落值不参与计数(记录优先)。
    CHECK(majorityLead(runs, 900, 5000, 7) == 5);
    CHECK(majorityLead(runs, 0, 1000, 7) == 5);
    // 记录了 0 ≠ 没有记录:0 是一个值,压过回落值。
    const std::vector<LeadRun> zeros{{0, 1000, 0}};
    CHECK(majorityLead(zeros, 0, 1000, 7) == 0);
    CHECK(majorityLead(zeros, 1000, 2000, 7) == 7); // 同一份记录,窗挪到记录外 → 回落
}

TEST_CASE("SL545 不同值计数:只数窗里有已记录样本的值;0 也算一个值;没记录的空档与窗外的记录不算",
          "[analysis][lead][sl545]")
{
    const std::vector<LeadRun> runs{{0, 300, 0}, {300, 600, 3}, {800, 1000, 3}, {1000, 1200, 5}};
    CHECK(distinctLeadValues(runs, 0, 1000) == 2); // 0 与 3;600..800 的空档不算;[1000,1200) 贴着窗右端、不相交
    CHECK(distinctLeadValues(runs, 300, 1000) == 1); // 只有 3
    CHECK(distinctLeadValues(runs, 0, 1200) == 3);
    CHECK(distinctLeadValues(runs, 600, 800) == 0); // 窗整个落在空档里
    CHECK(distinctLeadValues(runs, 500, 500) == 0); // 空窗
    CHECK(distinctLeadValues({}, 0, 1000) == 0);
}

TEST_CASE("SL216 记录队列:排干进时间线;满了丢并计数", "[analysis][lead][sl216]")
{
    LeadRecorder rec;
    LeadTimeline tl;
    for (int b = 0; b < 10; ++b)
    {
        rec.record(b * 64, (b + 1) * 64, 7);
    }
    CHECK(rec.drainInto(tl) == 10);
    REQUIRE(tl.size() == 1);
    CHECK(tl.runs()[0].t1 == 640);

    for (std::uint32_t i = 0; i < LeadRecorder::kCapacity + 5; ++i)
    {
        rec.record(0, 1, 1);
    }
    CHECK(rec.droppedRecords() == 5);
    CHECK(rec.discard() == LeadRecorder::kCapacity);
    CHECK(rec.drainInto(tl) == 0);
}

// ---------------------------------------------------------------------------
// LEAD 编解码
// ---------------------------------------------------------------------------

TEST_CASE("SL216 LEAD 往返逐字段一致", "[analysis][lead][sl216][state]")
{
    const std::vector<LeadRun> runs{{0, 48000, 0}, {48000, 96000, 3}, {200000, 300000, 15}};
    std::vector<std::uint8_t> bytes;
    encodeLeadChunk(runs, bytes);
    CHECK(bytes.size() == kLeadChunkHeaderBytes + 3 * kLeadChunkRecordBytes);
    std::vector<LeadRun> back;
    REQUIRE(decodeLeadChunk(bytes.data(), bytes.size(), back) == LeadDecodeStatus::Ok);
    REQUIRE(back.size() == runs.size());
    for (std::size_t i = 0; i < runs.size(); ++i)
    {
        CHECK(back[i].t0 == runs[i].t0);
        CHECK(back[i].t1 == runs[i].t1);
        CHECK(back[i].lead == runs[i].lead);
    }
}

TEST_CASE("SL216 LEAD 坏块整块不用;更高 minor 单列", "[analysis][lead][sl216][state]")
{
    const std::vector<LeadRun> runs{{0, 100, 1}, {100, 200, 2}};
    std::vector<std::uint8_t> good;
    encodeLeadChunk(runs, good);
    std::vector<LeadRun> out;

    SECTION("长度与条数不符")
    {
        auto b = good;
        b.pop_back();
        CHECK(decodeLeadChunk(b.data(), b.size(), out) == LeadDecodeStatus::Malformed);
    }
    SECTION("值越界")
    {
        auto b = good;
        b[kLeadChunkHeaderBytes + kLeadChunkRecordBytes + 16] = 16; // 第二条 lead = 16
        CHECK(decodeLeadChunk(b.data(), b.size(), out) == LeadDecodeStatus::Malformed);
    }
    SECTION("重叠/倒序")
    {
        auto b = good;
        b[kLeadChunkHeaderBytes + kLeadChunkRecordBytes] = 50; // 第二条 t0 = 50 < 前一条 t1
        CHECK(decodeLeadChunk(b.data(), b.size(), out) == LeadDecodeStatus::Malformed);
    }
    SECTION("更高 minor")
    {
        auto b = good;
        b[0] = static_cast<std::uint8_t>(kLeadChunkMinor + 1);
        CHECK(decodeLeadChunk(b.data(), b.size(), out) == LeadDecodeStatus::NewerMinor);
    }
    SECTION("短于头")
    {
        CHECK(decodeLeadChunk(good.data(), 4, out) == LeadDecodeStatus::Malformed);
    }
    CHECK(out.empty());
}

// ---------------------------------------------------------------------------
// 管线:主唱进槽位/平衡计算
// ---------------------------------------------------------------------------

TEST_CASE("SL216 管线:没有记录 → 优先级最低的轨 1 拿中心(前提:可区分)", "[analysis][lead][sl216][pipeline]")
{
    ThreeVoices v;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    CHECK(allPansAre(res.segments[0], 0.0));
    CHECK_FALSE(allPansAre(res.segments[1], 0.0));
}

TEST_CASE("SL216 管线:记录与当前值都是轨 2 → 轨 2 居中,另两轨在两侧对称排开", "[analysis][lead][sl216][pipeline]")
{
    ThreeVoices v;
    v.cfg.leadRuns = {{v.cfg.rangeStartSample, v.cfg.rangeEndSample, 2}};
    // [SL-545 / J143a] 整窗只有一个值的记录不单独说了算(整窗用当前值);这里是「播完没再动旋钮」的常规情形。
    v.cfg.leadFallback = 2;
    const auto res = runAnalysisPipeline(v.features, v.cfg);

    CHECK(allPansAre(res.segments[1], 0.0)); // 主唱:每一段都在正中
    REQUIRE(res.segments[0].size() == res.segments[2].size());
    REQUIRE_FALSE(res.segments[0].empty());
    for (std::size_t i = 0; i < res.segments[0].size(); ++i)
    {
        const double p0 = res.segments[0][i].pan;
        const double p2 = res.segments[2][i].pan;
        // 两条自由轨 → 偶数分支 K=1 → 槽 {−60,+60}(02 §5.2):不再有人占中心,且左右对称。
        CHECK(std::abs(std::abs(p0) - 60.0) < 1e-9);
        CHECK(std::abs(p0 + p2) < 1e-9);
    }
}

TEST_CASE("SL216 管线:lead_select 选中与 lead_lock 走同一条 C 路径(pan 与 vol 逐位同解)",
          "[analysis][lead][sl216][pipeline]")
{
    // 三轨能量不等:平衡要真的出 u,才验得到「平衡也把主唱当居中轨算」。
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = makeAlternating(4, 80, 60, 0.02f);
    features[1] = makeAlternating(4, 80, 60, 0.05f);
    features[2] = makeAlternating(4, 80, 60, 0.09f);
    // 主唱挑一条**没有主唱时不在中心**的轨,否则「选中」与「锁定」两边都是它本来的居中排布,
    // 逐位相等不说明任何事(本条第一版选了轨 3,而轨 3 本来就在中心 —— 删掉管线那一行照样绿)。
    const auto base = runAnalysisPipeline(features, makeConfig(features[0].kwMs.size(), 3));
    int leadIdx = -1;
    for (int t = 0; t < 3 && leadIdx < 0; ++t)
    {
        if (!allPansAre(base.segments[static_cast<std::size_t>(t)], 0.0))
        {
            leadIdx = t;
        }
    }
    REQUIRE(leadIdx >= 0);
    auto cfgSelect = makeConfig(features[0].kwMs.size(), 3);
    cfgSelect.leadRuns = {{cfgSelect.rangeStartSample, cfgSelect.rangeEndSample, leadIdx + 1}};
    cfgSelect.leadFallback = leadIdx + 1; // [SL-545 / J143a] 记录只有一个值时整窗用当前值,两者同值
    auto cfgLock = makeConfig(features[0].kwMs.size(), 3);
    cfgLock.tracks[static_cast<std::size_t>(leadIdx)].leadLock = true;

    const auto a = runAnalysisPipeline(features, cfgSelect);
    const auto b = runAnalysisPipeline(features, cfgLock);
    bool anyU = false;
    for (int t = 0; t < 3; ++t)
    {
        const auto& sa = a.segments[static_cast<std::size_t>(t)];
        const auto& sb = b.segments[static_cast<std::size_t>(t)];
        REQUIRE(sa.size() == sb.size());
        for (std::size_t i = 0; i < sa.size(); ++i)
        {
            CHECK(sa[i].t0Samples == sb[i].t0Samples);
            CHECK(sa[i].t1Samples == sb[i].t1Samples);
            CHECK(sa[i].pan == sb[i].pan);
            CHECK(sa[i].volDb == sb[i].volDb);
            anyU = anyU || std::abs(sa[i].volDb) > 1e-6;
        }
    }
    CHECK(allPansAre(a.segments[static_cast<std::size_t>(leadIdx)], 0.0));
    CHECK(anyU); // 前提:平衡确实动了音量,上面的 vol 逐位相等不是「两边都是 0」
}

TEST_CASE("SL216 管线:记录全是 0 → 与没有记录逐位同解", "[analysis][lead][sl216][pipeline]")
{
    ThreeVoices v;
    const auto base = runAnalysisPipeline(v.features, v.cfg);
    v.cfg.leadRuns = {{v.cfg.rangeStartSample, v.cfg.rangeEndSample, 0}};
    const auto zero = runAnalysisPipeline(v.features, v.cfg);
    for (int t = 0; t < 3; ++t)
    {
        const auto& sa = base.segments[static_cast<std::size_t>(t)];
        const auto& sb = zero.segments[static_cast<std::size_t>(t)];
        REQUIRE(sa.size() == sb.size());
        for (std::size_t i = 0; i < sa.size(); ++i)
        {
            CHECK(sa[i].pan == sb[i].pan);
            CHECK(sa[i].volDb == sb[i].volDb);
        }
    }
}

TEST_CASE("SL216 管线:逐区间换主唱 —— 前半轨 1、后半轨 3", "[analysis][lead][sl216][pipeline]")
{
    ThreeVoices v;
    v.cfg.tracks[0].priority = 5.0; // 本条不需要「默认中心」的前提
    // 4 轮 × 140 hop:前两轮与后两轮的分界恰在第 2 轮静音之后(280 hop)。
    const std::int64_t half = 280 * kHopSamples;
    v.cfg.leadRuns = {{0, half, 1}, {half, v.cfg.rangeEndSample, 3}};
    const auto res = runAnalysisPipeline(v.features, v.cfg);

    int firstHalfChecked = 0;
    int secondHalfChecked = 0;
    for (const auto& s : res.segments[0])
    {
        if (s.t1Samples <= half)
        {
            CHECK(s.pan == 0.0);
            ++firstHalfChecked;
        }
        else if (s.t0Samples >= half)
        {
            CHECK(s.pan != 0.0);
        }
    }
    for (const auto& s : res.segments[2])
    {
        if (s.t0Samples >= half)
        {
            CHECK(s.pan == 0.0);
            ++secondHalfChecked;
        }
        else if (s.t1Samples <= half)
        {
            CHECK(s.pan != 0.0);
        }
    }
    CHECK(firstHalfChecked > 0);
    CHECK(secondHalfChecked > 0);
}

TEST_CASE("SL216 管线:选中的轨在该区间不活跃 → 不影响其余轨", "[analysis][lead][sl216][pipeline]")
{
    ThreeVoices v;
    const auto base = runAnalysisPipeline(v.features, v.cfg);
    v.cfg.leadRuns = {{v.cfg.rangeStartSample, v.cfg.rangeEndSample, 9}}; // 轨 9 没有素材
    v.cfg.leadFallback = 9; // [SL-545 / J143a] 记录只有一个值时整窗用当前值:同值,否则 9 根本不会被取到
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    for (int t = 0; t < 3; ++t)
    {
        const auto& sa = base.segments[static_cast<std::size_t>(t)];
        const auto& sb = res.segments[static_cast<std::size_t>(t)];
        REQUIRE(sa.size() == sb.size());
        for (std::size_t i = 0; i < sa.size(); ++i)
        {
            CHECK(sa[i].pan == sb[i].pan);
        }
    }
    CHECK(res.segments[8].empty());
}

// ---------------------------------------------------------------------------
// [SL-545 / J143 + J143a] 点分析那一刻的 lead_select(cfg.leadFallback)
//
// J143a:计算窗里的记录 ≤ 1 个不同的值(没有记录 / 全是 0 / 全是同一轨)⇒ 不算自动化,整窗用当前值;
// ≥ 2 个值 ⇒ 按记录逐区间取多数值,一个已记录样本都没有的区间才用当前值。
// 本机台只有三条轨有素材(ThreeVoices),所以「当前值」一律取 1..3 里的某一轨 —— 选一条没有素材的轨
// 当当前值,「它居中」在段表里看不出来,与「没有主唱」无法区分。
// ---------------------------------------------------------------------------

namespace
{
// 各段的分界都切在两轮之间的**静音缝**里,这样没有哪个段、哪个区间横跨分界 —— 横跨的区间里多数值
// 怎么取是另一件事(见上面多数值那两条),这里只钉「整段是某个值 / 整段没有记录」的区间。
//   ThreeVoices = 4 轮 × (80 hop 发声 + 60 hop 静音):第 1、2 轮之间的缝约为 hop 240..268(段含前后 padding),
//   第 2、3 轮之间约为 hop 380..408。前提由 noSegmentStraddles 当场核,不靠这段注释。
constexpr std::int64_t kCutA = 254 * kHopSamples;
constexpr std::int64_t kCutB = 394 * kHopSamples;

bool noSegmentStraddles(const PipelineResult& res, std::int64_t cut)
{
    for (int t = 0; t < 3; ++t)
    {
        for (const auto& s : res.segments[static_cast<std::size_t>(t)])
        {
            if (s.t0Samples < cut && s.t1Samples > cut)
            {
                return false;
            }
        }
    }
    return true;
}

// [t0, t1) 里完整落进去的段:pan 全是 0 ⇒ 返回段数(> 0);有一段不是 0 或一段都没有 ⇒ 0 / −1。
// 用段数而不是 bool,让调用处能同时断言「确实查到了段」。
int centeredCountIn(const std::vector<AnalysisSegment>& segs, std::int64_t t0, std::int64_t t1)
{
    int n = 0;
    for (const auto& s : segs)
    {
        if (s.t0Samples >= t0 && s.t1Samples <= t1)
        {
            if (s.pan != 0.0)
            {
                return -1;
            }
            ++n;
        }
    }
    return n;
}

// [t0, t1) 里完整落进去的段,有几段离开了正中。
int offCenterCountIn(const std::vector<AnalysisSegment>& segs, std::int64_t t0, std::int64_t t1)
{
    int n = 0;
    for (const auto& s : segs)
    {
        if (s.t0Samples >= t0 && s.t1Samples <= t1 && s.pan != 0.0)
        {
            ++n;
        }
    }
    return n;
}

void checkSameLayout(const PipelineResult& a, const PipelineResult& b)
{
    for (int t = 0; t < 3; ++t)
    {
        const auto& sa = a.segments[static_cast<std::size_t>(t)];
        const auto& sb = b.segments[static_cast<std::size_t>(t)];
        REQUIRE(sa.size() == sb.size());
        for (std::size_t i = 0; i < sa.size(); ++i)
        {
            INFO("track " << (t + 1) << " seg " << i);
            CHECK(sa[i].t0Samples == sb[i].t0Samples);
            CHECK(sa[i].pan == sb[i].pan);
            CHECK(sa[i].volDb == sb[i].volDb);
        }
    }
}
} // namespace

TEST_CASE("SL545 管线:记录全是 0、当前值 = 3 → 轨 3 居中,另两轨在两侧对称排开(采集后改了 Lead Select 不重播)",
          "[analysis][lead][sl545][pipeline]")
{
    // 用户那条路:采集时 Lead Select = 0(采集就是走带播放,于是整窗记下的都是 0),采完改成 3,
    // 不重播直接点分析。记录只有一个值 ⇒ 不算自动化 ⇒ 整窗按当前值 3。
    ThreeVoices v;
    REQUIRE_FALSE(allPansAre(runAnalysisPipeline(v.features, v.cfg).segments[2], 0.0)); // 前提:轨 3 本来不居中
    v.cfg.leadRuns = {{v.cfg.rangeStartSample, v.cfg.rangeEndSample, 0}};
    v.cfg.leadFallback = 3;
    const auto res = runAnalysisPipeline(v.features, v.cfg);

    CHECK(allPansAre(res.segments[2], 0.0)); // ★
    REQUIRE(res.segments[0].size() == res.segments[1].size());
    REQUIRE_FALSE(res.segments[0].empty());
    for (std::size_t i = 0; i < res.segments[0].size(); ++i)
    {
        const double p0 = res.segments[0][i].pan;
        const double p1 = res.segments[1][i].pan;
        // 两条自由轨 → 偶数分支 K=1 → 槽 {−60,+60}(02 §5.2):中心让给主唱,其余左右对称。
        CHECK(std::abs(std::abs(p0) - 60.0) < 1e-9);
        CHECK(std::abs(p0 + p1) < 1e-9);
    }
}

TEST_CASE("SL545 管线:记录恒为轨 3、当前值 = 2 → 轨 2 居中、轨 3 不居中(用户最后一次的设定就是意图)",
          "[analysis][lead][sl545][pipeline]")
{
    // 播的时候是 3,播完改成 2、不重播:一份恒定的记录不比旋钮多带任何信息 ⇒ 按 2。
    ThreeVoices v;
    v.cfg.leadRuns = {{v.cfg.rangeStartSample, v.cfg.rangeEndSample, 3}};
    v.cfg.leadFallback = 2;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    CHECK(allPansAre(res.segments[1], 0.0)); // ★ 轨 2
    CHECK_FALSE(allPansAre(res.segments[2], 0.0)); // 记录里的 3 没有说了算
}

TEST_CASE("SL545 管线:记录有两个值(A 段 0、B 段 3)、C 段没有记录、当前值 = 2 → A 无主唱、B 轨 3、C 轨 2",
          "[analysis][lead][sl545][pipeline]")
{
    // 真自动化(窗里记下了 0 和 3 两个值)⇒ 有记录的地方按记录,当前值只补没有记录的 C 段。
    // 刻意自动化成 0 的 A 段(合唱句 / 无主唱)不会被当前值盖掉。
    ThreeVoices v;
    v.cfg.leadRuns = {{0, kCutA, 0}, {kCutA, kCutB, 3}};
    v.cfg.leadFallback = 2;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    REQUIRE(noSegmentStraddles(res, kCutA));
    REQUIRE(noSegmentStraddles(res, kCutB));
    const std::int64_t end = v.cfg.rangeEndSample;

    // A:记录了 0 ⇒ 无主唱 ⇒ 优先级最低的轨 1 拿中心(与「没有记录」那条同一个前提),轨 2 不居中。
    CHECK(centeredCountIn(res.segments[0], 0, kCutA) > 0);
    CHECK(offCenterCountIn(res.segments[1], 0, kCutA) > 0);
    // B:记录了 3 ⇒ 轨 3 居中;当前值 2 压不过记录。
    CHECK(centeredCountIn(res.segments[2], kCutA, kCutB) > 0);
    CHECK(offCenterCountIn(res.segments[1], kCutA, kCutB) > 0);
    // C:没有记录 ⇒ 当前值 2。
    CHECK(centeredCountIn(res.segments[1], kCutB, end) > 0);
    CHECK(offCenterCountIn(res.segments[0], kCutB, end) > 0);
    CHECK(offCenterCountIn(res.segments[2], kCutB, end) > 0);
}

TEST_CASE("SL545 管线:记录恒为轨 3、当前值 = 0 → 无主唱,与没有记录逐位同解", "[analysis][lead][sl545][pipeline]")
{
    // 播的时候选了 3,之后把 Lead Select 改回 0(不要主唱):按 0。
    ThreeVoices none;
    const auto base = runAnalysisPipeline(none.features, none.cfg);
    ThreeVoices v;
    v.cfg.leadRuns = {{v.cfg.rangeStartSample, v.cfg.rangeEndSample, 3}};
    v.cfg.leadFallback = 0;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    CHECK_FALSE(allPansAre(res.segments[2], 0.0)); // 前提:轨 3 在「没有主唱」时本来不居中
    checkSameLayout(res, base);
}

TEST_CASE("SL545 管线:没有记录(旧工程)、当前值 = 2 → 与 lead_lock 轨 2 逐位同解", "[analysis][lead][sl545][pipeline]")
{
    // SL-216 之前存的工程:特征在、LEAD 块不在 ⇒ 窗里 0 个值 ⇒ 整窗用当前值。
    // 与 lead_lock 逐位同解 = 当前值走的就是记录那条 C 路径(恒居中、不占槽、平衡把它当居中轨),不是另一套近似。
    ThreeVoices v;
    REQUIRE(v.cfg.leadRuns.empty());
    v.cfg.leadFallback = 2;
    ThreeVoices lock;
    lock.cfg.tracks[1].leadLock = true;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    CHECK(allPansAre(res.segments[1], 0.0));
    checkSameLayout(res, runAnalysisPipeline(lock.features, lock.cfg));
}
