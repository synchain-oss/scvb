// SPDX-License-Identifier: GPL-3.0-or-later
// test_lead_timeline —— [SL-216 / J136] 主唱进分析:lead_select 时间线记录 + 管线逐区间并入集合 C。
//
// 三层各钉各的:
//   · LeadTimeline / majorityLead / LeadRecorder / LEAD 编解码 —— 纯数据结构;
//   · runAnalysisPipeline —— 记录里选中的那一轨在该区间居中、不占槽,其余声部按剩下的轨数排槽;
//     与「把那一轨设成 lead_lock」逐位同解(同一条 C 路径,平衡也一样);没有记录 ⇒ 与改动前同解。
// 接线(Output 播放时记录 / 分析取记录 / 随工程存取)在 tests/host/test_host_harness.cpp 的 [sl216]。

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
// 轨 1(下标 0)优先级拉到 10:没有主唱时它拿中心槽(02 §5.3 p_target),
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
        cfg.tracks[0].priority = 10.0;
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

TEST_CASE("SL216 管线:没有记录 → 优先级最高的轨 1 拿中心(前提:可区分)", "[analysis][lead][sl216][pipeline]")
{
    ThreeVoices v;
    const auto res = runAnalysisPipeline(v.features, v.cfg);
    CHECK(allPansAre(res.segments[0], 0.0));
    CHECK_FALSE(allPansAre(res.segments[1], 0.0));
}

TEST_CASE("SL216 管线:记录选中轨 2 → 轨 2 居中,另两轨在两侧对称排开", "[analysis][lead][sl216][pipeline]")
{
    ThreeVoices v;
    v.cfg.leadRuns = {{v.cfg.rangeStartSample, v.cfg.rangeEndSample, 2}};
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

TEST_CASE("SL216 管线:lead_select 记录与 lead_lock 走同一条 C 路径(pan 与 vol 逐位同解)",
          "[analysis][lead][sl216][pipeline]")
{
    // 三轨能量不等:平衡要真的出 u,才验得到「平衡也把主唱当居中轨算」。
    std::array<PipelineTrackFeatures, kPipelineTracks> features;
    features[0] = makeAlternating(4, 80, 60, 0.02f);
    features[1] = makeAlternating(4, 80, 60, 0.05f);
    features[2] = makeAlternating(4, 80, 60, 0.09f);
    auto cfgSelect = makeConfig(features[0].kwMs.size(), 3);
    cfgSelect.leadRuns = {{cfgSelect.rangeStartSample, cfgSelect.rangeEndSample, 3}};
    auto cfgLock = makeConfig(features[0].kwMs.size(), 3);
    cfgLock.tracks[2].leadLock = true;

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
    CHECK(allPansAre(a.segments[2], 0.0));
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
