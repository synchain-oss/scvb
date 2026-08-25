// SPDX-License-Identifier: GPL-3.0-or-later
// test_viz_plane —— viz 段([T44/J75])的 L0 校验:布局/口径、seqlock 一致性读、只读 attach 的
// 零写入、以及 VizPublisher 的降采样口径(断线 = 无分段覆盖)与 4Hz 分频节拍。
// 跨进程部分(只读 attach / abi 拒连 / 一致性读)另见 tests/ipc/test_ipc_contract.cpp 的 VIZ-1/2。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <string>
#include <memory>
#include <thread>
#include <vector>

#include "ipc/SegmentBackendInProcess.h"
#include "ipc/VizPlane.h"
#include "output/VizPublisher.h"

using scvb::u32;
using scvb::u64;

namespace
{
constexpr double kSr = 48000.0;

// 造一条只在 [t0Sec, t1Sec) 有分段的轨(pan 恒 value)。
scvb::state::Segment makeSeg(double t0Sec, double t1Sec, float pan)
{
    scvb::state::Segment s;
    s.t0 = static_cast<std::int64_t>(t0Sec * kSr);
    s.t1 = static_cast<std::int64_t>(t1Sec * kSr);
    s.pan = pan;
    s.volDb = 0.0f;
    return s;
}

void buildCurve(scvb::CurveEvaluator& ev, const std::vector<scvb::state::Segment>& segs)
{
    std::vector<scvb::CurveSegment> cs;
    cs.reserve(segs.size());
    for (const auto& s : segs)
    {
        scvb::CurveSegment c;
        c.startSec = static_cast<double>(s.t0) / kSr;
        c.endSec = static_cast<double>(s.t1) / kSr;
        c.pan = s.pan;
        cs.push_back(c);
    }
    ev.build(cs, scvb::TransitionConfig{});
}
} // namespace

TEST_CASE("viz 段布局与定点口径", "[viz][layout]")
{
    // 段名带组号,且不存在无组号变体。
    REQUIRE(scvb::segmentLogicalName(1, scvb::SegmentKind::kViz) == L"SynchainSCVB.v1.g1.viz");
    REQUIRE(scvb::segmentLogicalName(8, scvb::SegmentKind::kViz) == L"SynchainSCVB.v1.g8.viz");

    // 定点换算:±100 → ±10000,越界钳制,哨兵不与合法值撞。
    REQUIRE(scvb::vizPackPan(0.0) == 0);
    REQUIRE(scvb::vizPackPan(100.0) == scvb::kVizPanMax);
    REQUIRE(scvb::vizPackPan(-100.0) == -scvb::kVizPanMax);
    REQUIRE(scvb::vizPackPan(1e9) == scvb::kVizPanMax);
    REQUIRE(scvb::vizPackPan(-1e9) == -scvb::kVizPanMax);
    REQUIRE(scvb::vizPackPan(12.345) == 1235); // 四舍五入
    REQUIRE(scvb::vizPanIsNone(scvb::kVizPanNone));
    REQUIRE_FALSE(scvb::vizPanIsNone(-scvb::kVizPanMax));
    REQUIRE(scvb::vizUnpackPan(2500) == 25.0);

    // 通用定点(volDb / widthPct):夹到 int16 且**绝不撞哨兵**(下界留一格给 -32768)。
    REQUIRE(scvb::vizPackFixed(-6.25) == -625);
    REQUIRE(scvb::vizPackFixed(0.0) == 0);
    REQUIRE(scvb::vizPackFixed(100.0) == 10000);
    REQUIRE(scvb::vizPackFixed(-1e9) == -32767);
    REQUIRE(scvb::vizPackFixed(-1e9) != scvb::kVizPanNone);
    REQUIRE(scvb::vizPackFixed(1e9) == 32767);
    REQUIRE(scvb::vizUnpackFixed(-625) == -6.25);

    // UTF-8 安全截断:绝不切出半个多字节序列。
    REQUIRE(scvb::vizUtf8TruncateLen("Lead", 31) == 4);
    const std::string zh = "主唱主唱主唱"; // 6 个汉字 = 18 字节
    REQUIRE(zh.size() == 18);
    REQUIRE(scvb::vizUtf8TruncateLen(zh, 31) == 18); // 不超限:原样
    REQUIRE(scvb::vizUtf8TruncateLen(zh, 4) == 3); // 落在第 2 个汉字中间 → 退到边界
    REQUIRE(scvb::vizUtf8TruncateLen(zh, 6) == 6); // 正好两个汉字边界
    REQUIRE(scvb::vizUtf8TruncateLen(zh, 0) == 0);
}

TEST_CASE("viz 段:超长轨名按 UTF-8 边界截断,读回不乱码", "[viz][label]")
{
    scvb::SegmentBackendInProcess backend;
    scvb::VizPlane writer(backend, 9);
    REQUIRE(writer.open() == scvb::InitResult::kOk);
    scvb::VizPlane reader(backend, 9);
    REQUIRE(reader.attachReadOnly() == scvb::InitResult::kOk);

    auto in = std::make_unique<scvb::VizSnapshot>();
    // 11 个汉字 = 33 字节 > 31:必须截到 30 字节(10 个汉字),不能留半个。
    in->label[0] = "主唱主唱主唱主唱主唱主";
    REQUIRE(in->label[0].size() == 33);
    // 恰好 31 字节的 ASCII:满格无截断。
    in->label[1] = std::string(31, 'x');
    // 32 字节 ASCII:截到 31(留 NUL)。
    in->label[2] = std::string(32, 'y');
    in->laneRevision = 1;
    writer.publish(*in, /*writeLanes=*/true);

    auto out = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(reader.read(*out));
    REQUIRE(out->label[0].size() == 30); // 10 个汉字,不是 31
    REQUIRE(out->label[0] == in->label[0].substr(0, 30));
    REQUIRE(out->label[1] == in->label[1]);
    REQUIRE(out->label[2] == std::string(31, 'y'));
    // 空轨名读回仍是空串(段内 NUL 补齐,不会读出垃圾)。
    REQUIRE(out->label[14].empty());
}

TEST_CASE("viz 段:写方发布 → 只读方一致性读", "[viz][ipc]")
{
    scvb::SegmentBackendInProcess backend;
    scvb::VizPlane writer(backend, 3);
    REQUIRE(writer.open() == scvb::InitResult::kOk);
    REQUIRE(writer.isOpen());
    REQUIRE_FALSE(writer.isReadOnly());
    REQUIRE(writer.geometryMatches());

    scvb::VizPlane reader(backend, 3);
    REQUIRE(reader.attachReadOnly() == scvb::InitResult::kOk);
    REQUIRE(reader.isReadOnly());
    REQUIRE(reader.geometryMatches());

    auto out = std::make_unique<scvb::VizSnapshot>();
    auto in = std::make_unique<scvb::VizSnapshot>();
    in->publishMs = 999;
    in->windowSpanSamples = 4800000;
    in->playheadSamples = 12345;
    in->loopStartSamples = 1000;
    in->loopEndSamples = 2000;
    in->sampleRate = 48000;
    in->versionActive = 2;
    in->playheadFlags = scvb::kVizPlaying | scvb::kVizLoopValid;
    in->playheadEpoch = 5;
    in->onlineMask = 0x1234;
    in->coveredMask = 0x0001;
    in->stereoMask = 0x0010;
    in->laneRevision = 3;
    for (u32 t = 0; t < scvb::kMaxChannels; ++t)
    {
        in->trackColor[t] = t + 1;
    }
    in->pan[0][0] = 777;
    in->pan[0][scvb::kVizColumns - 1] = -777;
    in->setCovered(0, 0);
    in->setCovered(0, scvb::kVizColumns - 1);
    in->panNow[0] = scvb::vizPackPan(-12.5);
    in->volDb[0] = scvb::vizPackFixed(-6.25);
    in->widthPct[0] = scvb::vizPackFixed(80.0);
    in->label[0] = "Lead";
    in->label[1] = "主唱"; // 主唱(多字节,验往返不乱码)

    writer.publish(*in, /*writeLanes=*/true);
    REQUIRE(reader.read(*out));
    REQUIRE(out->publishMs == 999);
    REQUIRE(out->windowSpanSamples == 4800000);
    REQUIRE(out->playheadSamples == 12345);
    REQUIRE(out->loopStartSamples == 1000);
    REQUIRE(out->loopEndSamples == 2000);
    REQUIRE(out->sampleRate == 48000);
    REQUIRE(out->versionActive == 2);
    REQUIRE(out->playheadFlags == (scvb::kVizPlaying | scvb::kVizLoopValid));
    REQUIRE(out->playheadEpoch == 5);
    REQUIRE(out->onlineMask == 0x1234);
    REQUIRE(out->stereoMask == 0x0010);
    REQUIRE(out->laneRevision == 3);
    REQUIRE(out->trackColor[14] == 15);
    REQUIRE(out->pan[0][0] == 777);
    REQUIRE(out->pan[0][scvb::kVizColumns - 1] == -777);
    REQUIRE(out->covered(0, 0));
    REQUIRE(out->covered(0, scvb::kVizColumns - 1));
    REQUIRE_FALSE(out->covered(0, 1));
    REQUIRE(out->panNow[0] == scvb::vizPackPan(-12.5));
    REQUIRE(out->volDb[0] == scvb::vizPackFixed(-6.25));
    REQUIRE(out->widthPct[0] == scvb::vizPackFixed(80.0));
    REQUIRE(out->label[0] == "Lead");
    REQUIRE(out->label[1] == "主唱");
    REQUIRE(out->panNow[2] == scvb::kVizPanNone); // 未填 = 哨兵

    // writeLanes=false:只刷帧头,车道内容原样保留(4Hz 刷 playhead / 车道按需重算的分频口径)。
    in->playheadSamples = 54321;
    in->pan[0][0] = 111; // 本次不该落段
    writer.publish(*in, /*writeLanes=*/false);
    REQUIRE(reader.read(*out));
    REQUIRE(out->playheadSamples == 54321);
    REQUIRE(out->pan[0][0] == 777);
}

TEST_CASE("viz 段:只读 attach 方 publish 不写任何字节", "[viz][ipc][readonly]")
{
    scvb::SegmentBackendInProcess backend;
    scvb::VizPlane writer(backend, 4);
    REQUIRE(writer.open() == scvb::InitResult::kOk);

    auto in = std::make_unique<scvb::VizSnapshot>();
    in->laneRevision = 1;
    in->playheadSamples = 42;
    writer.publish(*in, true);

    scvb::VizPlane reader(backend, 4);
    REQUIRE(reader.attachReadOnly() == scvb::InitResult::kOk);

    auto before = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(reader.read(*before));

    // 只读方误调 publish:必须是彻底 no-op。
    auto poison = std::make_unique<scvb::VizSnapshot>();
    poison->playheadSamples = -999;
    poison->laneRevision = 12345;
    reader.publish(*poison, true);

    auto after = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(reader.read(*after));
    REQUIRE(after->playheadSamples == before->playheadSamples);
    REQUIRE(after->laneRevision == before->laneRevision);
}

TEST_CASE("viz 段:非 owner 线程 publish 零写入(RT 铁律护栏)", "[viz][ipc][rt]")
{
    scvb::SegmentBackendInProcess backend;
    scvb::VizPlane writer(backend, 5); // open() 在本线程 → 本线程即 owner([M])
    REQUIRE(writer.open() == scvb::InitResult::kOk);

    auto in = std::make_unique<scvb::VizSnapshot>();
    in->playheadSamples = 7;
    in->laneRevision = 1;
    writer.publish(*in, true);

    scvb::VizPlane reader(backend, 5);
    REQUIRE(reader.attachReadOnly() == scvb::InitResult::kOk);
    auto before = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(reader.read(*before));
    REQUIRE(writer.foreignThreadWrites() == 0);

    // 模拟「processBlock 误接线」:另一条线程调 publish。
    auto poison = std::make_unique<scvb::VizSnapshot>();
    poison->playheadSamples = -12345;
    poison->laneRevision = 99;
    std::thread rt([&] { writer.publish(*poison, true); });
    rt.join();

    REQUIRE(writer.foreignThreadWrites() == 1);
    auto after = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(reader.read(*after));
    REQUIRE(after->playheadSamples == before->playheadSamples);
    REQUIRE(after->laneRevision == before->laneRevision);
}

TEST_CASE("VizPublisher:发布 → 读侧看到降采样数据与断线", "[viz][publisher]")
{
    scvb::SegmentBackendInProcess backend;
    scvb::output::VizPublisher pub(backend, 6);
    REQUIRE(pub.open() == scvb::InitResult::kOk);

    scvb::VizPlane reader(backend, 6);
    REQUIRE(reader.attachReadOnly() == scvb::InitResult::kOk);

    // 轨1:[0,30) 有段(pan=-50);轨2:[60,90) 有段(pan=+40),前 60s 无覆盖;轨3:无段。
    auto crvs = std::make_unique<scvb::state::CrvsData>();
    crvs->versions[0].tracks[0].segments = {makeSeg(0.0, 30.0, -50.0f)};
    crvs->versions[0].tracks[1].segments = {makeSeg(60.0, 90.0, 40.0f)};

    scvb::CurveEvaluator c0;
    scvb::CurveEvaluator c1;
    buildCurve(c0, crvs->versions[0].tracks[0].segments);
    buildCurve(c1, crvs->versions[0].tracks[1].segments);

    scvb::output::VizPublishInput in;
    in.crvs = crvs.get();
    in.curves[0] = &c0;
    in.curves[1] = &c1;
    in.versionActive = 1;
    in.enabledMask = 0x7FFF;
    in.sampleRate = kSr;
    in.crvsRevision = 1;
    in.playhead.timeSamples = 0;
    in.playhead.sampleRate = kSr;

    REQUIRE(pub.tick(1000, in));
    REQUIRE(pub.laneRebuildCount() == 1);

    auto out = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(reader.read(*out));

    // 窗口:最大分段末端 90s → 量化到 30s 边界 = 90s;起点恒 0。
    REQUIRE(out->windowStartSamples == 0);
    REQUIRE(out->windowSpanSamples == static_cast<u64>(90.0 * kSr));
    REQUIRE(out->sampleRate == 48000);
    REQUIRE(out->onlineMask == 0x7FFF);
    REQUIRE(out->coveredMask == 0x0003); // 只有轨1/轨2 有段

    const double colSec = 90.0 / static_cast<double>(scvb::kVizColumns);
    const auto colOf = [&](double sec) { return static_cast<u32>(sec / colSec); };

    // 轨1:[0,30) 覆盖,30s 之后断线(位图为 0)。
    REQUIRE(out->covered(0, 0));
    REQUIRE(out->covered(0, colOf(29.0)));
    REQUIRE_FALSE(out->covered(0, colOf(45.0)));
    REQUIRE(out->pan[0][0] == scvb::vizPackPan(-50.0));

    // 轨2:前 60s 断线,[60,90) 覆盖。
    REQUIRE_FALSE(out->covered(1, 0));
    REQUIRE_FALSE(out->covered(1, colOf(30.0)));
    REQUIRE(out->covered(1, colOf(75.0)));
    REQUIRE(out->pan[1][colOf(75.0)] == scvb::vizPackPan(40.0));

    // 轨3:无段 → 整条哨兵 + 零覆盖(读侧据此彻底不画)。
    REQUIRE(out->pan[2][0] == scvb::kVizPanNone);
    REQUIRE(out->pan[2][scvb::kVizColumns - 1] == scvb::kVizPanNone);
    REQUIRE_FALSE(out->covered(2, 0));

    // 轨色索引 = 轨号。
    REQUIRE(out->trackColor[0] == 1);
    REQUIRE(out->trackColor[14] == 15);
}

TEST_CASE("VizPublisher:4Hz 分频与车道按需重算", "[viz][publisher][cadence]")
{
    scvb::SegmentBackendInProcess backend;
    scvb::output::VizPublisher pub(backend, 2);
    REQUIRE(pub.open() == scvb::InitResult::kOk);

    auto crvs = std::make_unique<scvb::state::CrvsData>();
    crvs->versions[0].tracks[0].segments = {makeSeg(0.0, 10.0, 0.0f)};
    scvb::CurveEvaluator c0;
    buildCurve(c0, crvs->versions[0].tracks[0].segments);

    scvb::output::VizPublishInput in;
    in.crvs = crvs.get();
    in.curves[0] = &c0;
    in.versionActive = 1;
    in.sampleRate = kSr;
    in.crvsRevision = 1;

    REQUIRE(pub.tick(0, in)); // 首帧必发
    REQUIRE(pub.publishCount() == 1);
    REQUIRE(pub.laneRebuildCount() == 1);

    // 未到 250ms 闸门:不发。
    REQUIRE_FALSE(pub.tick(100, in));
    REQUIRE_FALSE(pub.tick(249, in));
    REQUIRE(pub.publishCount() == 1);

    // 到闸门:发帧头,但 CRVS 未变、窗口未变、距上次重算 <1s → 不重算车道。
    REQUIRE(pub.tick(250, in));
    REQUIRE(pub.publishCount() == 2);
    REQUIRE(pub.laneRebuildCount() == 1);

    // CRVS 修订变化 → 立刻重算车道。
    in.crvsRevision = 2;
    REQUIRE(pub.tick(500, in));
    REQUIRE(pub.laneRebuildCount() == 2);

    // 距上次重算 ≥1s → 兜底重算。
    REQUIRE(pub.tick(1500, in));
    REQUIRE(pub.laneRebuildCount() == 3);
}

TEST_CASE("VizPublisher:无末端分段哨兵不炸窗口跨度", "[viz][publisher][window]")
{
    scvb::SegmentBackendInProcess backend;
    scvb::output::VizPublisher pub(backend, 8);
    REQUIRE(pub.open() == scvb::InitResult::kOk);
    scvb::VizPlane reader(backend, 8);
    REQUIRE(reader.attachReadOnly() == scvb::InitResult::kOk);

    // setTrackManual 的「覆盖到时间线末端」哨兵 t1 = 1<<40。
    auto crvs = std::make_unique<scvb::state::CrvsData>();
    scvb::state::Segment openEnded;
    openEnded.t0 = static_cast<std::int64_t>(10.0 * kSr);
    openEnded.t1 = scvb::output::kVizOpenEndedT1;
    openEnded.pan = 20.0f;
    crvs->versions[0].tracks[0].segments = {openEnded};

    scvb::CurveEvaluator c0;
    {
        std::vector<scvb::CurveSegment> cs;
        scvb::CurveSegment c;
        c.startSec = 10.0;
        c.endSec = 1e9; // 引擎侧的开区间由 CurveEvaluator 自行外推
        c.pan = 20.0;
        cs.push_back(c);
        c0.build(cs, scvb::TransitionConfig{});
    }

    scvb::output::VizPublishInput in;
    in.crvs = crvs.get();
    in.curves[0] = &c0;
    in.versionActive = 1;
    in.sampleRate = kSr;
    in.crvsRevision = 1;
    REQUIRE(pub.tick(0, in));

    auto out = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(reader.read(*out));
    // 哨兵只以 t0(10s)参与跨度 → 落到最小跨度 60s,而不是 1<<40 样本。
    REQUIRE(out->windowSpanSamples == static_cast<u64>(60.0 * kSr));
    // 开区间分段在位图上一路覆盖到窗口末端。
    REQUIRE(out->covered(0, scvb::kVizColumns - 1));
    REQUIRE_FALSE(out->covered(0, 0)); // 10s 之前无覆盖
}

TEST_CASE("VizPublisher:空工程也给一条最小窗口,不产生退化轴", "[viz][publisher][window]")
{
    scvb::SegmentBackendInProcess backend;
    scvb::output::VizPublisher pub(backend, 1);
    REQUIRE(pub.open() == scvb::InitResult::kOk);
    scvb::VizPlane reader(backend, 1);
    REQUIRE(reader.attachReadOnly() == scvb::InitResult::kOk);

    auto crvs = std::make_unique<scvb::state::CrvsData>();
    scvb::output::VizPublishInput in;
    in.crvs = crvs.get();
    in.versionActive = 1;
    in.sampleRate = kSr;
    REQUIRE(pub.tick(0, in));

    auto out = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(reader.read(*out));
    REQUIRE(out->windowSpanSamples == static_cast<u64>(60.0 * kSr));
    REQUIRE(out->coveredMask == 0);
    for (u32 t = 0; t < scvb::kMaxChannels; ++t)
    {
        REQUIRE(out->pan[t][0] == scvb::kVizPanNone);
    }

    // 未 prepare(sampleRate=0):跨度 0,不除零、不崩。
    scvb::output::VizPublisher pub2(backend, 1);
    REQUIRE(pub2.open() == scvb::InitResult::kOk);
    scvb::output::VizPublishInput noSr;
    noSr.crvs = crvs.get();
    noSr.sampleRate = 0.0;
    REQUIRE(pub2.tick(0, noSr));
    REQUIRE(reader.read(*out));
    REQUIRE(out->windowSpanSamples == 0);
}
