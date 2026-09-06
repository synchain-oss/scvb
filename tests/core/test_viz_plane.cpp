// SPDX-License-Identifier: GPL-3.0-or-later
// test_viz_plane —— viz 段([T44/J75])的 L0 校验:布局/口径、seqlock 一致性读、只读 attach 的
// 零写入、以及 VizPublisher 的降采样口径(断线 = 无分段覆盖)与 4Hz 分频节拍。
// 跨进程部分(只读 attach / abi 拒连 / 一致性读)另见 tests/ipc/test_ipc_contract.cpp 的 VIZ-1/2。

#include <limits>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <string>
#include <memory>
#include <thread>
#include <vector>

#include "ipc/SegmentBackendInProcess.h"
#include "ipc/VizPlane.h"
#include "output/DistReadback.h"
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

    // 通用定点:**先按工程量纲夹取再定点**,且绝不撞哨兵。
    const double vLo = scvb::kVizVolDbMin, vHi = scvb::kVizVolDbMax;
    const double wLo = scvb::kVizWidthMin, wHi = scvb::kVizWidthMax;
    REQUIRE(scvb::vizPackFixed(-6.25, vLo, vHi) == -625);
    REQUIRE(scvb::vizPackFixed(0.0, vLo, vHi) == 0);
    REQUIRE(scvb::vizPackFixed(100.0, wLo, wHi) == 10000);
    // 越界按**声明的域**夹,不是按 int16 上限 —— 这正是那四个常量存在的意义。
    REQUIRE(scvb::vizPackFixed(-1e9, vLo, vHi) == -2400); // −24 dB
    REQUIRE(scvb::vizPackFixed(1e9, vLo, vHi) == 1200); // +12 dB
    REQUIRE(scvb::vizPackFixed(-1e9, wLo, wHi) == 0); // width 下界
    REQUIRE(scvb::vizPackFixed(1e9, wLo, wHi) == 10000); // width 上界
    REQUIRE(scvb::vizPackFixed(-1e9, vLo, vHi) != scvb::kVizPanNone);
    // 极宽的域仍夹到 int16 且不撞哨兵(下界留一格给 -32768)。
    REQUIRE(scvb::vizPackFixed(-1e9, -1e9, 1e9) == -32767);
    REQUIRE(scvb::vizPackFixed(1e9, -1e9, 1e9) == 32767);
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
    in->leadMask = 0x0020;
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
    in->volDb[0] = scvb::vizPackFixed(-6.25, scvb::kVizVolDbMin, scvb::kVizVolDbMax);
    in->widthPct[0] = scvb::vizPackFixed(80.0, scvb::kVizWidthMin, scvb::kVizWidthMax);
    // [SL-362] 全局「最大角度」:值域 **0..150**,取 150 —— 一是 >100 才与「回落 100」分得开,
    // 二是它落在**只有全局域才够得着**的那一半(per-track 域是 0..100)。
    in->globalWidthPct = scvb::vizPackFixed(150.0, scvb::kVizGlobalWidthMin, scvb::kVizGlobalWidthMax);
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
    REQUIRE(out->leadMask == 0x0020);
    REQUIRE(out->laneRevision == 3);
    REQUIRE(out->trackColor[14] == 15);
    REQUIRE(out->pan[0][0] == 777);
    REQUIRE(out->pan[0][scvb::kVizColumns - 1] == -777);
    REQUIRE(out->covered(0, 0));
    REQUIRE(out->covered(0, scvb::kVizColumns - 1));
    REQUIRE_FALSE(out->covered(0, 1));
    REQUIRE(out->panNow[0] == scvb::vizPackPan(-12.5));
    REQUIRE(out->volDb[0] == scvb::vizPackFixed(-6.25, scvb::kVizVolDbMin, scvb::kVizVolDbMax));
    REQUIRE(out->widthPct[0] == scvb::vizPackFixed(80.0, scvb::kVizWidthMin, scvb::kVizWidthMax));
    REQUIRE(out->label[0] == "Lead");
    REQUIRE(out->label[1] == "主唱");
    REQUIRE(out->panNow[2] == scvb::kVizPanNone); // 未填 = 哨兵
    // [SL-362] 全局「最大角度」往返。
    //
    // ⚠ 期望值**不用被测代码的同一个表达式算**(复审第 1 轮【重要】):初版写成
    // `vizPackFixed(150.0, kVizWidthMin, kVizWidthMax)`,与被测处逐字同式 ⇒ 它只断言了
    // 「两侧用同一条编码」,**夹取错了照样绿**;而那个表达式的值恰好就是 fixed(100),
    // 于是「取 >100 免得与回落 100 撞车」这个理由**当场落空**——往返值就是 100。
    // 现在两条一起钉:① 解出来的工程量是 150;② packed 是 **15000** 这个裸字面量
    // (夹到 100 时是 10000,当场红)。判据必须**独立于被测物**,否则测的是自洽不是对。
    // 定点数是精确的(150 × 100 = 15000),不需要 Approx —— 用精确比较更硬:
    // Approx 会把「差一点」也放过,而这里任何偏差都意味着标度或夹取出了问题。
    REQUIRE(scvb::vizUnpackFixed(out->globalWidthPct) == 150.0);
    REQUIRE(out->globalWidthPct == 15000);

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

TEST_CASE("viz 段:owner 线程绑在首次 publish,不是 open()", "[viz][ipc][rt]")
{
    // [I2] JUCE/VST3 **不保证** prepareToPlay(→ open())与 timerCallback(→ publish())
    // 在同一条线程上。护栏若绑在 open() 的线程,一旦两者不同,此后每一次 publish 都被挡掉,
    // viz 段静默保持全零 —— 没有任何报错。所以要绑在首次 publish。
    scvb::SegmentBackendInProcess backend;
    scvb::VizPlane w2(backend, 12);
    std::thread opener([&] { REQUIRE(w2.open() == scvb::InitResult::kOk); }); // 在**别的**线程 open
    opener.join();

    auto f = std::make_unique<scvb::VizSnapshot>();
    f->laneRevision = 9;
    f->playheadSamples = 4242;
    w2.publish(*f, true); // 本线程首发 → 本线程成为 owner
    REQUIRE(w2.foreignThreadWrites() == 0);

    scvb::VizPlane r2(backend, 12);
    REQUIRE(r2.attachReadOnly() == scvb::InitResult::kOk);
    auto got = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(r2.read(*got));
    REQUIRE(got->laneRevision == 9); // 真的写进去了,而不是被护栏静默挡掉
    REQUIRE(got->playheadSamples == 4242);
}

TEST_CASE("viz 段:非 owner 线程 publish 零写入(RT 铁律护栏)", "[viz][ipc][rt]")
{
    scvb::SegmentBackendInProcess backend;
    scvb::VizPlane writer(backend, 5);
    REQUIRE(writer.open() == scvb::InitResult::kOk);

    auto in = std::make_unique<scvb::VizSnapshot>();
    in->playheadSamples = 7;
    in->laneRevision = 1;
    writer.publish(*in, true); // 首发在本线程 → 本线程成为 owner

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

TEST_CASE("viz 段:read() 返回 false 时 out 一个字节不改(沿用上帧真的做得到)", "[viz][ipc][read]")
{
    // [I3] 早期版本直接读进 out,撕裂时把调用方的上一帧覆盖成半新半旧的拼接,
    // 而 API 却承诺「沿用上帧」—— 调用方根本没法沿用。
    scvb::SegmentBackendInProcess backend;
    scvb::VizPlane writer(backend, 13);
    REQUIRE(writer.open() == scvb::InitResult::kOk);
    auto f = std::make_unique<scvb::VizSnapshot>();
    f->laneRevision = 5;
    f->playheadSamples = 111;
    f->label[0] = "Keep";
    writer.publish(*f, true);

    scvb::VizPlane reader(backend, 13);
    REQUIRE(reader.attachReadOnly() == scvb::InitResult::kOk);
    auto held = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(reader.read(*held));
    REQUIRE(held->laneRevision == 5);

    // 段未打开的读方:必然返回 false —— 此时 out 必须一个字节都没动。
    scvb::VizPlane closed(backend, 14);
    REQUIRE_FALSE(closed.read(*held));
    REQUIRE(held->laneRevision == 5); // 上一帧原封不动
    REQUIRE(held->playheadSamples == 111);
    REQUIRE(held->label[0] == "Keep");
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

// [SL-362] **旧写方兼容**:本卡之前的 Output 不写 `global_width_plus_one`,覆盖式初始化
// 把那一槽留成 **0**。新读方必须把 0 读成「未提供」(哨兵),**不是**「宽度 0%」——
// 0 在定点编码里是合法宽度(全收拢到中央),读错的表现是分布图把 15 根柱全挤到中线,
// **而那看起来像一张正常的图**,没有任何东西会报错。这一格就是为它写的。
//
// 造「旧写方」的方式:发布一帧**不设** globalWidthPct 的快照(默认即哨兵),写方那边会
// 存 0 —— 与旧写方留下的 0 逐字节同形。
// ← 把编码处的 `+1` 与解码处的 `-1` 同时去掉(往返仍自洽,上面那格照绿),**只红这一格**。
TEST_CASE("VizPlane:[SL-362] 槽为 0 = 写方未提供 ⇒ 解码回哨兵,不是宽度 0", "[viz][plane][sl362]")
{
    scvb::SegmentBackendInProcess backend;
    scvb::VizPlane writer(backend, 3);
    REQUIRE(writer.open() == scvb::InitResult::kOk);
    scvb::VizPlane reader(backend, 3);
    REQUIRE(reader.attachReadOnly() == scvb::InitResult::kOk);

    auto in = std::make_unique<scvb::VizSnapshot>();
    auto out = std::make_unique<scvb::VizSnapshot>();
    // 默认构造即哨兵 —— 明写一次,免得将来有人改了默认值而本格悄悄失去前提。
    REQUIRE(in->globalWidthPct == scvb::kVizPanNone);

    writer.publish(*in, /*writeLanes=*/true);
    REQUIRE(reader.read(*out));
    REQUIRE(out->globalWidthPct == scvb::kVizPanNone);
    // 且**不是** 0:0 是合法宽度,两者混淆正是本格要挡的。
    REQUIRE(out->globalWidthPct != 0);

    // 反向:写一个真的 0%(全收拢)必须**能**往返出来,不被当成「未提供」。
    // 少了这一条,把编码写成「恒存 0」也能让上面两格全绿。
    in->globalWidthPct = scvb::vizPackFixed(0.0, scvb::kVizGlobalWidthMin, scvb::kVizGlobalWidthMax);
    writer.publish(*in, /*writeLanes=*/true);
    REQUIRE(reader.read(*out));
    REQUIRE(out->globalWidthPct == 0); // 真的 0%:packed 就是 0(裸字面量,不经被测编码)
    REQUIRE(out->globalWidthPct != scvb::kVizPanNone);
}

// [SL-362 复审第 5 轮] **打包点**那一格 —— 经真 `VizPublisher::tick` 喂工程量、从段里读回。
//
// 为什么单独要它:上面那两格喂的是**已经打好包的定点值**(`in->globalWidthPct` 是 int16),
// 它们钉住的是 `VizPlane` 的编解码,**钉不到 `VizPublisher` 里那次 `vizPackFixed` 调用**。
// 复核实证:把 `VizPublisher.cpp` 里那行的域换回 `kVizWidthMin/Max`、乃至把整块打包删掉,
// 上面两格**全绿** —— 打包点当时没有任何判据。而红旗恰恰就出在那一行的域上。
//
// 期望值 **15000 是裸字面量**,不经被测编码(150 × kVizPanScale;夹到 100 时是 10000)。
TEST_CASE("VizPublisher:[SL-362] 全局 width 经打包点落段(150 ⇒ 15000)", "[viz][publisher][sl362]")
{
    scvb::SegmentBackendInProcess backend;
    scvb::output::VizPublisher pub(backend, 2);
    REQUIRE(pub.open() == scvb::InitResult::kOk);
    scvb::VizPlane reader(backend, 2);
    REQUIRE(reader.attachReadOnly() == scvb::InitResult::kOk);

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
    in.globalWidthPct = 150.0f; // ← **工程量**,由发布器负责打包

    REQUIRE(pub.tick(0, in));
    auto out = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(reader.read(*out));
    // 150 × 100 = 15000。用 per-track 的 0..100 域会夹成 10000(红旗那个缺陷)。
    REQUIRE(out->globalWidthPct == 15000);
    REQUIRE(scvb::vizUnpackFixed(out->globalWidthPct) == 150.0);

    // 不给(NaN,句柄未就绪)⇒ 段里留哨兵,读方回落 100。
    in.globalWidthPct = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(pub.tick(scvb::output::VizPublisher::kPublishIntervalMs, in));
    REQUIRE(reader.read(*out));
    REQUIRE(out->globalWidthPct == scvb::kVizPanNone);
}

TEST_CASE("VizPublisher:发布分频与车道按需重算", "[viz][publisher][cadence]")
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

    // 未到闸门:不发。**全部按常量表达** —— 这里曾经写的是 100/249/250 三个字面量,
    // SL-192 把闸门从 250ms 改成 33ms 之后它们全体失效(249 反而跨了好几个周期)。
    // 按常量写,以后再改频率这条也不会悄悄变成另一个意思。
    constexpr auto kGate = scvb::output::VizPublisher::kPublishIntervalMs;
    REQUIRE_FALSE(pub.tick(1, in));
    REQUIRE_FALSE(pub.tick(kGate - 1, in));
    REQUIRE(pub.publishCount() == 1);

    // 到闸门:发帧头,但 CRVS 未变、窗口未变、未到兜底 → 不重算车道。
    REQUIRE(pub.tick(kGate, in));
    REQUIRE(pub.publishCount() == 2);
    REQUIRE(pub.laneRebuildCount() == 1);

    // CRVS 修订变化 → 立刻重算车道。
    in.crvsRevision = 2;
    REQUIRE(pub.tick(2 * kGate, in));
    REQUIRE(pub.laneRebuildCount() == 2);

    // 兜底间隔内**不该**重算 —— 车道的四个依赖(CRVS 修订 / 活动版本 / 窗口跨度 /
    // metaRevision)都没变。兜底只为哈希碰撞留后路,不是常态开销。
    const auto midway = scvb::output::VizPublisher::kLaneRefreshMaxMs / 2;
    REQUIRE(pub.tick(midway, in));
    REQUIRE(pub.laneRebuildCount() == 2);

    // 越过兜底间隔 → 重算一次(用常量而不是字面量,改了阈值这条不会悄悄失效)。
    const auto past = midway + scvb::output::VizPublisher::kLaneRefreshMaxMs;
    REQUIRE(pub.tick(past, in));
    REQUIRE(pub.laneRebuildCount() == 3);

    // metaRevision(轨名)变化 → 立刻重算,不必等兜底。
    in.metaRevision = 77;
    REQUIRE(pub.tick(past + kGate, in));
    REQUIRE(pub.laneRebuildCount() == 4);

    // due():发布闸门的对外查询与实际发布行为一致(调用方据此跳过输入采集)。
    REQUIRE_FALSE(pub.due(past + kGate + 1));
    REQUIRE(pub.due(past + kGate + scvb::output::VizPublisher::kPublishIntervalMs));
}

// [SL-192] 发布频率的**数值**断言。用户拍板的是「好歹每秒保证 30Hz」,而频率正是这一卡
// 唯一真正被要求的东西 —— 它必须有一条看得见的闸,不能只靠常量改对了就算数。
//
// **不看墙钟**:按注入的逻辑时刻推一秒,数真的发出去几帧。于是它可以进门禁而不会变成
// CI 抖动源(单帧耗时那种必然要看墙钟的测量归隐藏用例,见 test_viz_publish_cost.cpp)。
namespace
{
// 驱动发布器一个逻辑秒,返回真的发出去几帧。
//
// `jitterMs` 模拟定时器抖动:第 i 拍落在 `round(i * 1000 / hz) + (i % 3) * jitterMs`。
// 这不是装饰 —— **没有抖动就测不出「驱动与闸门同频」的危害**:理想时钟下 30Hz 驱动
// 33ms 闸门每拍都恰好够着,数出来是漂亮的 30 帧,而真机上它会周期性丢帧。
// (本用例第一版就是无抖动的,于是「驱动退回 30Hz」这条反向验证**没能变红** ——
// 一条测不出自己要防的东西的断言,等于没有。)
scvb::u64 publishedInOneSecond(int driverHz, int jitterMs)
{
    scvb::SegmentBackendInProcess backend;
    scvb::output::VizPublisher pub(backend, 6);
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

    scvb::u64 published = 0;
    for (int i = 1; i <= driverHz; ++i)
    {
        const auto nominal = static_cast<scvb::u64>(static_cast<double>(i) * 1000.0 / driverHz);
        const auto nowMs = nominal + static_cast<scvb::u64>((i % 3) * jitterMs);
        if (pub.tick(nowMs, in))
        {
            ++published;
        }
    }
    return published;
}
} // namespace

TEST_CASE("VizPublisher:单位时间发布帧数 >= 28(SL-192 升频)", "[viz][publisher][rate]")
{
    // 生产配置:驱动 = OutputProcessor 的独立 kPublishTimerHz(60Hz)定时器。
    const int driver = scvb::output::VizPublisher::kPublishTimerHz;

    // 无抖动的理想时钟:阈值 ≥28 留余量,上界 ≤34 防「闸门被误改成 0 / 被绕过」
    // (那会让发布跑到驱动频率上去 —— 白烧一倍 CPU,也是错,不能当绿)。
    const auto clean = publishedInOneSecond(driver, 0);
    INFO("clean clock: published = " << clean << " (gate = " << scvb::output::VizPublisher::kPublishIntervalMs
                                     << "ms, driver = " << driver << "Hz)");
    REQUIRE(clean >= 28);
    REQUIRE(clean <= 34);

    // **带抖动**:生产配置必须扛得住。60Hz 驱动 33ms 闸门 = 每两拍一帧,±2ms 抖动
    // 吃不掉那一整拍的余量。
    const auto jittered = publishedInOneSecond(driver, 2);
    INFO("jittered clock: published = " << jittered);
    REQUIRE(jittered >= 28);
}

// 这一条断言的是一个**我们不采用**的配置会坏 —— 把「闸门必须夹在两个可达节拍之间、
// 两头都留余量」这条规则本身钉住(见 VizPublisher.h 的 kPublishIntervalMs 头注)。
//
// SL-192 在读方栽过两次同频异相(MonitorProcessor.cpp / MonitorEditor.cpp 的头注),
// 写方这边差点栽第三次:最初把闸门写成 33ms(= 目标周期本身),两拍 33.33ms 只比它多
// 0.33ms,理想时钟下数出来是漂亮的 30 帧,**一有抖动就塌到 20Hz**。
// 上面那条 >=28 在理想时钟下照样绿 —— 只有带抖动的本条会红。
TEST_CASE("VizPublisher:驱动周期 == 闸门时,抖动会周期性丢帧(故闸门夹在两拍之间)", "[viz][publisher][rate]")
{
    // 「驱动周期恰好等于闸门」的那个频率 —— 危险配置。
    const int gateHz = 1000 / static_cast<int>(scvb::output::VizPublisher::kPublishIntervalMs);
    const int shipped = scvb::output::VizPublisher::kPublishTimerHz;

    // 危险配置:理想时钟下看着很美(每拍都恰好够着闸门)……
    REQUIRE(publishedInOneSecond(gateHz, 0) >= 28);
    // ……一旦有 ±2ms 抖动就周期性丢帧,掉到阈值以下。
    const auto sameFreq = publishedInOneSecond(gateHz, 2);
    INFO("same-frequency driver with jitter: published = " << sameFreq);
    REQUIRE(sameFreq < 28);

    // 出厂配置(60Hz 驱动 + 夹在一拍与两拍之间的闸门)在同样的抖动下毫发无伤。
    REQUIRE(publishedInOneSecond(shipped, 2) >= 28);
}

// ===========================================================================
// [SL-363] 分布图「每轨当前值」的读回口径 —— Monitor 与 Output 同源
// ===========================================================================
// 用户 v5.6.8 真机原话:「柱子位置明显不对……不过在一些地方是对齐的,一些片段不对。」
// 定谳:两侧画的是同一个量,却各走各的链 ——
//   · Output(`web/output/tab-master.js` renderDist → `web/shared/readback.js`)= 播放头
//     **所在段**的段值(空隙里保持前一段)+ 冻结 / 输出档 / 手动段三档回落;
//   · Monitor(本文件测的 `VizPublisher`)原来 = `CurveEvaluator` 在播放头**精确时刻**的求值。
// 于是「段内对齐、空隙与段边界不对」。SL-363 把发布器换成同一条读回链(`DistReadback.h`)。
//
// 每一格都同时钉**新值**与**旧值**:只断「等于段值」的话,把实现改回曲线求值时,段内那些格
// 照样绿 —— 真正分叉的是空隙与 ramp 窗口这两处,所以那两处显式断「不等于曲线给的那个数」,
// 并把曲线给的数**量出来**(那是分叉幅度的实测,不是推断)。
//
// ★ 删除式(本机实测;下面每条后面的数是**实得** FAIL 条数,不是预期):
//   · D1 发布器改回 `curve->panAt/volAt`(整条读回链被短路)      ⇒ 11 条 —— (a)(b)(c)(d)(e) 全族。
//     D1 是「把本卡整个撤掉」,所以它**不是**一条精准注入:它红得比下面四条都宽,
//     真正证明「每一格各钉各的」的是 D2-D5 那四条各自只红自己那一格。
//   · D2 发布器不解 freeze(恒传 0)                              ⇒ 1 条 —— 只有 (c)
//   · D3 发布器恒传 outputEnabled = true                          ⇒ 4 条 —— (d) 两格 + (e) 两格
//     ((e) 跟着红是对的:轨1 有段,输出档一旦被写死成 ON,未连接轨也会拿到段值而不是哨兵)
//   · D4 `readbackSegsOf` 去掉 manual 优先(只留 curveSegmentAt)  ⇒ 2 条 —— 纯函数那格 + (d) 的手动段格
//   · D5 `curveSegmentAt` 空隙改取**后一段**                      ⇒ 6 条 —— 纯函数空隙格 + (a) 四格 + (b) 里轨1 那格;
//     **(b) 的 ramp 两格不红** —— 那两格钉的是「段内取该段」,与空隙分支是两条路
namespace
{
scvb::state::Segment makeSegV(double t0Sec, double t1Sec, float pan, float volDb,
                              scvb::state::SegmentOrigin origin = scvb::state::SegmentOrigin::Auto)
{
    scvb::state::Segment s;
    s.t0 = static_cast<std::int64_t>(t0Sec * kSr);
    s.t1 = static_cast<std::int64_t>(t1Sec * kSr);
    s.pan = pan;
    s.volDb = volDb;
    s.flags = scvb::state::makeSegmentFlags(origin, false);
    return s;
}

std::int64_t atSec(double sec)
{
    return static_cast<std::int64_t>(sec * kSr);
}

// 手动接管常值段的右端:`SegmentEditService.h` 写的是 1<<40 样本。这里按秒给同一个数量级的
// 「远大于任何播放位置」,免得测试里再抄一遍那个哨兵字面量。
constexpr double kFarEndSec = 4294967296.0;
} // namespace

TEST_CASE("DistReadback:段选择口径逐条对齐 web/shared/readback.js", "[viz][publisher][SL-363][readback]")
{
    const std::vector<scvb::state::Segment> gap = {makeSegV(0.0, 10.0, -60.0f, -3.0f),
                                                   makeSegV(20.0, 30.0, 60.0f, 6.0f)};
    const std::vector<scvb::state::Segment> manual = {
        makeSegV(0.0, kFarEndSec, -80.0f, -9.0f, scvb::state::SegmentOrigin::UserEdited)};

    // 首段之前 → 首段;末段之后 → 末段。这两档两侧**本来就一致**(曲线求值也这么回填),
    // 不是病灶 —— 卡面把「段外」列成疑似分叉族,实测下来分叉的只有**段间空隙**。
    REQUIRE(scvb::output::curveSegmentAt(gap, atSec(-1.0))->pan == -60.0f);
    REQUIRE(scvb::output::curveSegmentAt(gap, atSec(99.0))->pan == 60.0f);
    // 段内。
    REQUIRE(scvb::output::curveSegmentAt(gap, atSec(5.0))->pan == -60.0f);
    REQUIRE(scvb::output::curveSegmentAt(gap, atSec(25.0))->pan == 60.0f);
    // **空隙 → 前一段**,整个空隙都是,不因过了中点就切后段(那正是曲线求值的做法)。
    REQUIRE(scvb::output::curveSegmentAt(gap, atSec(10.0))->pan == -60.0f);
    REQUIRE(scvb::output::curveSegmentAt(gap, atSec(16.0))->pan == -60.0f);
    REQUIRE(scvb::output::curveSegmentAt(gap, atSec(19.9))->pan == -60.0f);
    // 空表 → nullptr(调用方回落参数面)。
    const std::vector<scvb::state::Segment> none;
    REQUIRE(scvb::output::curveSegmentAt(none, 0) == nullptr);

    // 手动常值段:判据只看「段数 == 1 ∧ origin == UserEdited」。
    REQUIRE(scvb::output::manualConstantOf(manual) != nullptr);
    REQUIRE(scvb::output::manualConstantOf(gap) == nullptr);
    {
        auto notManual = manual;
        notManual.front().flags = scvb::state::makeSegmentFlags(scvb::state::SegmentOrigin::Auto, false);
        REQUIRE(scvb::output::manualConstantOf(notManual) == nullptr);
    }

    // 优先级链四档。
    const auto onNoFreeze = scvb::output::readbackSegsOf(gap, 0, /*outputOn=*/true, atSec(16.0));
    REQUIRE(onNoFreeze.pan != nullptr);
    REQUIRE(onNoFreeze.pan->pan == -60.0f);
    REQUIRE(onNoFreeze.vol->volDb == -3.0f);
    // 输出 OFF(跟随宿主)⇒ 两维都回落参数面。
    const auto off = scvb::output::readbackSegsOf(gap, 0, /*outputOn=*/false, atSec(16.0));
    REQUIRE(off.pan == nullptr);
    REQUIRE(off.vol == nullptr);
    // 手动常值段**不看输出档**。
    const auto manOff = scvb::output::readbackSegsOf(manual, 0, /*outputOn=*/false, atSec(500.0));
    REQUIRE(manOff.pan != nullptr);
    REQUIRE(manOff.pan->pan == -80.0f);
    REQUIRE(manOff.manual == manOff.pan);
    // 冻结**逐维**:冻 pan 只让 pan 回落,vol 仍读段(写成「一冻全冻」会在这里红)。
    const auto frzPan = scvb::output::readbackSegsOf(manual, 1, /*outputOn=*/true, atSec(500.0));
    REQUIRE(frzPan.pan == nullptr);
    REQUIRE(frzPan.vol != nullptr);
    REQUIRE(frzPan.vol->volDb == -9.0f);
    const auto frzVol = scvb::output::readbackSegsOf(manual, 2, /*outputOn=*/true, atSec(500.0));
    REQUIRE(frzVol.pan != nullptr);
    REQUIRE(frzVol.vol == nullptr);
    const auto frzBoth = scvb::output::readbackSegsOf(manual, 3, /*outputOn=*/true, atSec(500.0));
    REQUIRE(frzBoth.pan == nullptr);
    REQUIRE(frzBoth.vol == nullptr);
}

TEST_CASE("VizPublisher:panNow/volDb 走段读回,不再是曲线求值", "[viz][publisher][SL-363]")
{
    scvb::SegmentBackendInProcess backend;
    scvb::output::VizPublisher pub(backend, 3);
    REQUIRE(pub.open() == scvb::InitResult::kOk);
    scvb::VizPlane reader(backend, 3);
    REQUIRE(reader.attachReadOnly() == scvb::InitResult::kOk);

    auto crvs = std::make_unique<scvb::state::CrvsData>();
    // 轨1:**空隙**族 —— [0,10) 与 [20,30),空隙 [10,20)。
    crvs->versions[0].tracks[0].segments = {makeSegV(0.0, 10.0, -60.0f, -3.0f), makeSegV(20.0, 30.0, 60.0f, 6.0f)};
    // 轨2:**相邻段**族(gap = 0)—— 边界 10s 上有一条 ramp。
    crvs->versions[0].tracks[1].segments = {makeSegV(0.0, 10.0, -60.0f, 0.0f), makeSegV(10.0, 20.0, 60.0f, 0.0f)};
    // 轨3:手动接管常值段(单段 UserEdited)。
    crvs->versions[0].tracks[2].segments = {
        makeSegV(0.0, kFarEndSec, -80.0f, -9.0f, scvb::state::SegmentOrigin::UserEdited)};

    std::vector<scvb::CurveEvaluator> curves(3);
    for (std::size_t t = 0; t < curves.size(); ++t)
    {
        std::vector<scvb::CurveSegment> cs;
        for (const auto& s : crvs->versions[0].tracks[t].segments)
        {
            scvb::CurveSegment c;
            c.startSec = static_cast<double>(s.t0) / kSr;
            c.endSec = static_cast<double>(s.t1) / kSr;
            c.pan = s.pan;
            c.volDb = s.volDb;
            cs.push_back(c);
        }
        curves[t].build(cs, scvb::TransitionConfig{});
    }

    scvb::output::VizPublishInput in;
    in.crvs = crvs.get();
    for (std::size_t t = 0; t < curves.size(); ++t)
    {
        in.curves[t] = &curves[t];
    }
    in.versionActive = 1;
    in.enabledMask = 0x7FFF;
    in.connectedMask = 0x7FFF;
    in.sampleRate = kSr;
    in.crvsRevision = 1;
    in.panParam.fill(33.0f); // 参数面刻意与任何段值都不同 —— 回落一旦误触发就看得见
    in.volDbParam.fill(11.0f);

    auto out = std::make_unique<scvb::VizSnapshot>();
    constexpr auto kGate = scvb::output::VizPublisher::kPublishIntervalMs;
    scvb::u64 now = 0;
    const auto publishAt = [&](double headSec) {
        in.playhead.timeSamples = atSec(headSec);
        REQUIRE(pub.tick(now, in));
        now += kGate;
        REQUIRE(reader.read(*out));
    };

    // ---- (a) [gap] 空隙:播放头 16s 落在 [10,20) 里 ------------------------
    publishAt(16.0);
    // 曲线求值在这里给的是**后一段**(ramp 中心 = 空隙中点 15s,80ms 窗口在 16s 之前就走完了)。
    // 把它量出来:这是「两侧差多少」的实测,也是下面那条反向断言的依据。
    INFO("curve panAt(16) = " << curves[0].panAt(16.0));
    CHECK(curves[0].panAt(16.0) == 60.0);
    CHECK(out->panNow[0] == scvb::vizPackPan(-60.0)); // 新口径:保持前一段
    CHECK(out->panNow[0] != scvb::vizPackPan(60.0)); // ← D1 / D5 在这里红
    CHECK(out->volDb[0] == scvb::vizPackFixed(-3.0, scvb::kVizVolDbMin, scvb::kVizVolDbMax));
    CHECK(out->volDb[0] != scvb::vizPackFixed(6.0, scvb::kVizVolDbMin, scvb::kVizVolDbMax));
    // 车道**不跟着改** —— 轨迹图画的就是曲线本身,那条线该有 ramp。
    CHECK(out->pan[0][0] == scvb::vizPackPan(-60.0));
    CHECK(out->coveredMask == 0x0007);

    // ---- (b) [ramp] 过渡斜坡中点:播放头 10s 落在轨2 的 ramp 窗口正中 -------
    publishAt(10.0);
    // gap=0 那一支的 T_eff 由限速反推:|ΔP| = 120 ⇒ 1.5 × 120/15 = 12s,夹到 6s 上限,
    // 再被重叠防护夹到 0.9 × 10s = 9s ⇒ 6s。窗口 [7,13],中点求值 = 两段中值 0。
    // **不是「80ms 一闪」**:这一族在真机上能连着好几秒对不上。
    INFO("curve panAt(10) = " << curves[1].panAt(10.0));
    CHECK(curves[1].panAt(10.0) == 0.0);
    CHECK(out->panNow[1] == scvb::vizPackPan(60.0)); // 新口径:段内 ⇒ 该段的段值
    CHECK(out->panNow[1] != scvb::vizPackPan(0.0)); // ← D1 在这里也红
    // 同一帧里轨1 仍在空隙(10s 是 [10,20) 的左端)⇒ 前一段。
    CHECK(out->panNow[0] == scvb::vizPackPan(-60.0));
    // [manual] 手动常值段:与播放头无关,恒是它自己。
    CHECK(out->panNow[2] == scvb::vizPackPan(-80.0));
    CHECK(out->volDb[2] == scvb::vizPackFixed(-9.0, scvb::kVizVolDbMin, scvb::kVizVolDbMax));

    // ---- (c) [freeze] 冻结维度读参数面,**逐维独立** -----------------------
    in.freezeParam[2] = 1.0f; // 只冻 pan
    publishAt(10.0);
    CHECK(out->panNow[2] == scvb::vizPackPan(33.0)); // ← D2 在这里红(冻结位没解出来)
    CHECK(out->volDb[2] == scvb::vizPackFixed(-9.0, scvb::kVizVolDbMin, scvb::kVizVolDbMax)); // vol 仍读段
    in.freezeParam[2] = 0.0f;

    // ---- (d) [outputoff] 输出 OFF ⇒ 读参数面;手动段那一档**不受影响** -----
    in.outputEnabled = false;
    publishAt(16.0);
    CHECK(out->panNow[0] == scvb::vizPackPan(33.0)); // ← D3 在这里红
    CHECK(out->volDb[0] == scvb::vizPackFixed(11.0, scvb::kVizVolDbMin, scvb::kVizVolDbMax));
    CHECK(out->panNow[2] == scvb::vizPackPan(-80.0)); // ← D4 在这里红(手动段被降级成普通段)
    in.outputEnabled = true;

    // ---- (e) 未连接轨:回落被闸住,仍是哨兵(SL-361 那道闸没被本卡挪走)-----
    in.connectedMask = 0;
    in.outputEnabled = false;
    publishAt(16.0);
    CHECK(out->panNow[0] == scvb::kVizPanNone);
    CHECK(out->volDb[0] == scvb::kVizPanNone);
}
