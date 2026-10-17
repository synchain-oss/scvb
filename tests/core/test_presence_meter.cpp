// SPDX-License-Identifier: GPL-3.0-or-later
// test_presence_meter —— 在场判据仪器(tests/support/presence_meter.h)的自测。
//
// 仪器是 A 线后面所有判据的尺子:它若什么都判不出来,调度模拟器的每一条场景都会假绿。
// 所以这里不只测「好的判成好的」,更要钉住「每一种坏读出都判得出来」,并且每个设计落点
// 都配一格删除式(删掉它,对应的那条必红):
//
//   落点(presence_meter.h)                      删除方式                     必红的用例 / 断言
//   ① kCarrierHalfStep(f = (n+1/2)·sr/R 的 +1)   置 0 ⇒ R 点 bin 中心频率      「R alias」的 maxRho ≤ −0.99
//   ② kEnvDepth(与 R 不可公度的缓变包络)          置 0 ⇒ 没有包络              「2R alias」的 Present 占比 ≤ 0.3
//   ③ Thresholds::floorAbsent(缺席下限)           置 0                          「missing lane」的 Absent == 帧数
//   ④ classify() 里「有宽带泄漏 ⇒ Partial(Dirty)」  删掉期望静音那一支           「half-frame」的 lane 2 why == Dirty
//
// 场景全部直接按时间线合成 Output 的 L/R(不经任何 Processor),逐帧判定。帧数和起点固定,
// 结果是确定的。运行期文案一律 ASCII(本机 CP936 上中文字面量会触发 C4819)。

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "support/presence_meter.h"

using scvb::testsupport::presence::analyze;
using scvb::testsupport::presence::CombReport;
using scvb::testsupport::presence::kFrame;
using scvb::testsupport::presence::kMaxLanes;
using scvb::testsupport::presence::kRawBusGainL;
using scvb::testsupport::presence::kRawBusGainR;
using scvb::testsupport::presence::kRingR;
using scvb::testsupport::presence::LaneReport;
using scvb::testsupport::presence::laneTone;
using scvb::testsupport::presence::msToDbfs;
using scvb::testsupport::presence::sampleAt;
using scvb::testsupport::presence::Source;
using scvb::testsupport::presence::Span;
using scvb::testsupport::presence::Thresholds;
using scvb::testsupport::presence::toString;
using scvb::testsupport::presence::Verdict;
using scvb::testsupport::presence::Why;

namespace
{

// 一条 lane 在 Output 录音里的样子。
struct LaneRender
{
    int lane = 0; // 这条轨的频点
    int contentLane = -1; // 实际放进来的内容取自哪条轨(-1 = 本轨;跨轨串扰用)
    std::int64_t shift = 0; // 内容晚了多少样本(kRingR = 读到上一圈的旧数据)
    double gainL = 0.7;
    double gainR = 0.7;
    double level = 1.0;
    std::int64_t onFrom = std::numeric_limits<std::int64_t>::min(); // 时间线 [onFrom, onUntil) 内才有声
    std::int64_t onUntil = std::numeric_limits<std::int64_t>::max();
};

LaneRender lane(int id)
{
    LaneRender r;
    r.lane = id;
    return r;
}

LaneRender shifted(int id, std::int64_t shift)
{
    LaneRender r = lane(id);
    r.shift = shift;
    return r;
}

LaneRender panned(int id, double gainL, double gainR)
{
    LaneRender r = lane(id);
    r.gainL = gainL;
    r.gainR = gainR;
    return r;
}

struct Bus
{
    std::int64_t start = 0;
    std::vector<float> left;
    std::vector<float> right;

    Span span() const { return Span{left.data(), right.data(), start, static_cast<std::int64_t>(left.size())}; }
};

Bus render(std::int64_t start, std::int64_t length, const std::vector<LaneRender>& lanes)
{
    Bus b;
    b.start = start;
    b.left.assign(static_cast<std::size_t>(length), 0.0f);
    b.right.assign(static_cast<std::size_t>(length), 0.0f);
    for (const LaneRender& lr : lanes)
    {
        const auto tone = laneTone(lr.contentLane >= 0 ? lr.contentLane : lr.lane);
        for (std::int64_t i = 0; i < length; ++i)
        {
            const std::int64_t t = start + i;
            if (t < lr.onFrom || t >= lr.onUntil)
            {
                continue;
            }
            const double v = lr.level * sampleAt(tone, t - lr.shift);
            b.left[static_cast<std::size_t>(i)] += static_cast<float>(lr.gainL * v);
            b.right[static_cast<std::size_t>(i)] += static_cast<float>(lr.gainR * v);
        }
    }
    return b;
}

// 一个对齐、且已经绕过环好几圈的时间线起点(大 t 也要精确)。
constexpr std::int64_t kT0 = 3 * kRingR + 12345 * static_cast<std::int64_t>(kFrame);

// 没分析过这条轨 ⇒ vector::at 抛 out_of_range,Catch2 记为该用例失败。
const LaneReport& laneOf(const CombReport& rep, int id)
{
    std::size_t idx = 0;
    while (idx < rep.lanes.size() && rep.lanes[idx].lane != id)
    {
        ++idx;
    }
    return rep.lanes.at(idx);
}

double minRho(const LaneReport& lr)
{
    double m = std::numeric_limits<double>::max();
    for (const auto& f : lr.frames)
    {
        m = f.rho < m ? f.rho : m;
    }
    return m;
}

double maxRho(const LaneReport& lr)
{
    double m = -std::numeric_limits<double>::max();
    for (const auto& f : lr.frames)
    {
        m = f.rho > m ? f.rho : m;
    }
    return m;
}

double maxEnergy(const LaneReport& lr)
{
    double m = 0.0;
    for (const auto& f : lr.frames)
    {
        m = f.energy > m ? f.energy : m;
    }
    return m;
}

int countSource(const LaneReport& lr, Source s)
{
    int n = 0;
    for (Source x : lr.sources)
    {
        n += (x == s) ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE("PRESENCE ideal four-lane bus: every lane Present on every frame, control Absent", "[presence]")
{
    const Thresholds th;
    constexpr int kFrames = 128;
    const Bus bus = render(kT0, kFrames * static_cast<std::int64_t>(kFrame), {lane(0), lane(1), lane(2), lane(3)});
    const CombReport rep = analyze(bus.span(), {0, 1, 2, 3});
    REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));
    REQUIRE(rep.lanes.size() == 4u);

    for (const LaneReport& lr : rep.lanes)
    {
        INFO("lane " << lr.lane << " minRho " << minRho(lr) << " gain.mix " << lr.gain.mix);
        CHECK(lr.count(Verdict::Present) == kFrames);
        CHECK(countSource(lr, Source::Mix) == kFrames);
        CHECK(minRho(lr) >= 0.999);
    }
    CHECK(rep.controlNonAbsent(th) == 0);
    CHECK(rep.dirtyFrames(th) == 0);

    // 帧对齐:起点不对齐、且在时间线负半轴(起播前的预备拍)。不完整的头尾帧不分析。
    // span = [-5300, -5300 + 20·1024) ⇒ 完整帧 k = -5..13,共 19 帧。
    const Bus odd = render(-5300, 20 * static_cast<std::int64_t>(kFrame), {lane(0), lane(1)});
    const CombReport rep2 = analyze(odd.span(), {0, 1});
    REQUIRE(rep2.frameStarts.size() == 19u);
    CHECK(rep2.frameStarts.front() == -5 * static_cast<std::int64_t>(kFrame));
    CHECK(rep2.frameStarts.back() == 13 * static_cast<std::int64_t>(kFrame));
    for (const LaneReport& lr : rep2.lanes)
    {
        CHECK(lr.count(Verdict::Present) == 19);
    }
}

TEST_CASE("PRESENCE missing lane is Absent on every frame while its neighbours stay Present", "[presence]")
{
    const Thresholds th;
    constexpr int kFrames = 128;
    // lane 1 一个样本都没写进总线;两侧邻轨照常满幅播放。
    const Bus bus = render(kT0, kFrames * static_cast<std::int64_t>(kFrame), {lane(0), lane(2), lane(3)});
    const CombReport rep = analyze(bus.span(), {0, 1, 2, 3});
    REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));

    const LaneReport& missing = laneOf(rep, 1);
    INFO("lane 1 max energy " << msToDbfs(maxEnergy(missing)) << " dBFS, floor " << msToDbfs(th.floorAbsent)
                              << " dBFS");
    CHECK(missing.count(Verdict::Absent) == kFrames);
    CHECK(countSource(missing, Source::None) == kFrames);
    for (int id : {0, 2, 3})
    {
        CHECK(laneOf(rep, id).count(Verdict::Present) == kFrames);
    }
    CHECK(rep.dirtyFrames(th) == 0);
}

TEST_CASE("PRESENCE R alias: a lane read one ring lap stale is anti-phase and judged Wrong", "[presence]")
{
    const Thresholds th;
    constexpr int kFrames = 128;
    // lane 1 读到的是环里上一圈留下的内容:时间线 t 处放的是 s(t - R)。
    const Bus bus =
        render(kT0, kFrames * static_cast<std::int64_t>(kFrame), {lane(0), shifted(1, kRingR), lane(2), lane(3)});
    const CombReport rep = analyze(bus.span(), {0, 1, 2, 3});
    REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));

    const LaneReport& stale = laneOf(rep, 1);
    INFO("lane 1 maxRho " << maxRho(stale) << " minRho " << minRho(stale));
    // 设计接住它的那条:f = (n + 1/2)·sr/R ⇒ 平移 R 相位正好反转。
    CHECK(maxRho(stale) <= -0.99);
    CHECK(stale.count(Verdict::Wrong) == kFrames);
    CHECK(stale.countWhy(Why::Decorrelated) == kFrames);
    for (int id : {0, 2, 3})
    {
        CHECK(laneOf(rep, id).count(Verdict::Present) == kFrames);
    }
    CHECK(rep.dirtyFrames(th) == 0);
}

TEST_CASE("PRESENCE 2R alias: carrier is back in phase, the envelope amplitude check still rejects it", "[presence]")
{
    const Thresholds th;
    // 400 帧 ≈ 14 个包络周期,幅度比在整段里摆过好几轮。
    constexpr int kFrames = 400;
    const Bus bus =
        render(kT0, kFrames * static_cast<std::int64_t>(kFrame), {lane(0), shifted(1, 2 * kRingR), lane(2), lane(3)});
    const CombReport rep = analyze(bus.span(), {0, 1, 2, 3});
    REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));

    const LaneReport& stale = laneOf(rep, 1);
    INFO("lane 1 Present " << stale.count(Verdict::Present) << " / " << kFrames << ", AmpMismatch "
                           << stale.countWhy(Why::AmpMismatch) << ", minRho " << minRho(stale) << ", gain.mix "
                           << stale.gain.mix);
    // 先钉住前提:平移 2R 时载波逐样本同相 —— 只看相关,这一轨会被当成在场。
    CHECK(minRho(stale) >= 0.99);
    // 设计接住它的那条:包络与 R 不可公度 ⇒ 2R 前的包络与期望错开,幅度对不上。
    CHECK(stale.fraction(Verdict::Present) <= 0.3);
    CHECK(stale.countWhy(Why::AmpMismatch) >= kFrames * 6 / 10);
    CHECK(stale.count(Verdict::Wrong) == 0);
    for (int id : {0, 2, 3})
    {
        CHECK(laneOf(rep, id).count(Verdict::Present) == kFrames);
    }
    CHECK(rep.dirtyFrames(th) == 0);
}

TEST_CASE("PRESENCE block-offset reads (64 to 4096 samples late) are never Present", "[presence]")
{
    // 读方按块错位(读成了 d 个样本之前的内容):常见块长与其倍数逐个钉住。全梳 16 条 lane
    // 同时错位,每条的相关都必须掉到 rhoPresent 以下 —— 只靠载波相位就拒掉,不依赖幅度核对。
    constexpr int kFrames = 64;
    const Thresholds th;
    std::vector<int> ids;
    for (int id = 0; id < kMaxLanes; ++id)
    {
        ids.push_back(id);
    }
    for (std::int64_t d : {64, 128, 256, 480, 512, 1024, 2048, 3072, 4096})
    {
        std::vector<LaneRender> late;
        for (int id : ids)
        {
            LaneRender r = panned(id, 0.16, 0.16);
            r.shift = d;
            late.push_back(r);
        }
        const Bus bus = render(kT0, kFrames * static_cast<std::int64_t>(kFrame), late);
        const CombReport rep = analyze(bus.span(), ids);
        REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));
        REQUIRE(rep.lanes.size() == static_cast<std::size_t>(kMaxLanes));
        double worst = -1.0;
        for (const LaneReport& lr : rep.lanes)
        {
            INFO("offset " << d << " lane " << lr.lane << " maxRho " << maxRho(lr) << " minRho " << minRho(lr));
            CHECK(lr.count(Verdict::Present) == 0);
            CHECK(maxRho(lr) < th.rhoPresent);
            worst = maxRho(lr) > worst ? maxRho(lr) : worst;
        }
        INFO("offset " << d << " worst maxRho over 16 lanes " << worst);
        CHECK(worst < 0.81);
        CHECK(rep.dirtyFrames(th) == 0);
    }
}

TEST_CASE("PRESENCE half-frame transition: one Partial frame between Absent and Present, no Wrong anywhere",
          "[presence]")
{
    const Thresholds th;
    constexpr int kFrames = 64;
    constexpr int kEdge = 20; // lane 1 在第 20 帧的正中间硬切进来
    LaneRender late = lane(1);
    late.onFrom = kT0 + kEdge * static_cast<std::int64_t>(kFrame) + kFrame / 2;
    // lane 2 期望静音、也确实没写进总线:它紧挨着硬切的 lane 1,硬切那一帧的宽带泄漏会
    // 落到它的频点上。这一帧必须判 Partial(Dirty),不能判成「期望静音却有能量」的 Wrong。
    const auto silentLane2 = [](int id, std::int64_t) { return id != 2; };
    const Bus bus = render(kT0, kFrames * static_cast<std::int64_t>(kFrame), {lane(0), late, lane(3)});
    const CombReport rep = analyze(bus.span(), {0, 1, 2, 3}, silentLane2);
    REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));

    const LaneReport& l1 = laneOf(rep, 1);
    for (int k = 0; k < kFrames; ++k)
    {
        const auto& j = l1.judged[static_cast<std::size_t>(k)];
        INFO("frame " << k << " verdict " << toString(j.verdict) << " why " << toString(j.why) << " ampRatio "
                      << l1.frames[static_cast<std::size_t>(k)].ampRatio);
        if (k < kEdge)
        {
            CHECK(j.verdict == Verdict::Absent);
        }
        else if (k == kEdge)
        {
            CHECK(j.verdict == Verdict::Partial);
        }
        else
        {
            CHECK(j.verdict == Verdict::Present);
        }
    }

    // 硬切只污染它所在的那一帧:probe 只在第 20 帧见到宽带泄漏,别的帧全干净。
    CHECK(rep.dirtyFrames(th) == 1);
    CHECK(rep.noise[static_cast<std::size_t>(kEdge)].probeMax >= th.floorAbsent);

    // 静音邻轨:泄漏确实落到了它的频点上(前提),而判定是 Partial(Dirty),不是 Wrong。
    const LaneReport& l2 = laneOf(rep, 2);
    const auto& edge2 = l2.frames[static_cast<std::size_t>(kEdge)];
    INFO("lane 2 edge-frame energy " << msToDbfs(edge2.energy) << " dBFS, verdict "
                                     << toString(l2.judged[static_cast<std::size_t>(kEdge)].verdict));
    REQUIRE(edge2.energy >= th.floorAbsent);
    CHECK(l2.judged[static_cast<std::size_t>(kEdge)].why == Why::Dirty);
    CHECK(l2.count(Verdict::Absent) == kFrames - 1);

    for (const LaneReport& lr : rep.lanes)
    {
        INFO("lane " << lr.lane);
        CHECK(lr.count(Verdict::Wrong) == 0);
        if (lr.lane == 0 || lr.lane == 3)
        {
            // 在场的邻轨在硬切那一帧可以是 Present 或 Partial(Dirty),其余帧必须 Present。
            CHECK(lr.count(Verdict::Present) >= kFrames - 1);
        }
    }
}

TEST_CASE("PRESENCE cross-lane: misrouted content leaves the target Absent, leaked content into a silent lane is "
          "Wrong",
          "[presence]")
{
    const Thresholds th;
    constexpr int kFrames = 96;
    const std::int64_t len = kFrames * static_cast<std::int64_t>(kFrame);

    // ① 串轨:lane 1 的位置上放的是 lane 0 的内容。目标轨缺席,不被邻轨的能量冒充。
    LaneRender misrouted = lane(1);
    misrouted.contentLane = 0;
    const Bus bus1 = render(kT0, len, {lane(0), misrouted, lane(2), lane(3)});
    const CombReport rep1 = analyze(bus1.span(), {0, 1, 2, 3});
    REQUIRE(rep1.frameStarts.size() == static_cast<std::size_t>(kFrames));
    CHECK(laneOf(rep1, 1).count(Verdict::Absent) == kFrames);
    CHECK(laneOf(rep1, 2).count(Verdict::Present) == kFrames);
    CHECK(laneOf(rep1, 3).count(Verdict::Present) == kFrames);

    // ② 期望静音的 lane 1 漏进了自己 -30 dB 的内容 ⇒ Wrong(期望静音却有能量)。
    const auto silentLane1 = [](int id, std::int64_t) { return id != 1; };
    LaneRender leak = lane(1);
    leak.level = 0.0316;
    const Bus bus2 = render(kT0, len, {lane(0), leak, lane(2), lane(3)});
    const CombReport rep2 = analyze(bus2.span(), {0, 1, 2, 3}, silentLane1);
    REQUIRE(rep2.frameStarts.size() == static_cast<std::size_t>(kFrames));
    CHECK(laneOf(rep2, 1).count(Verdict::Wrong) == kFrames);
    CHECK(laneOf(rep2, 1).countWhy(Why::SilentHasEnergy) == kFrames);
    for (int id : {0, 2, 3})
    {
        CHECK(laneOf(rep2, id).count(Verdict::Present) == kFrames);
    }

    // ③ 对照:期望静音且确实没东西 ⇒ Absent(这是对的结果,不是缺陷)。
    const Bus bus3 = render(kT0, len, {lane(0), lane(2), lane(3)});
    const CombReport rep3 = analyze(bus3.span(), {0, 1, 2, 3}, silentLane1);
    CHECK(laneOf(rep3, 1).count(Verdict::Absent) == kFrames);
    CHECK(rep1.dirtyFrames(th) == 0);
    CHECK(rep2.dirtyFrames(th) == 0);
    CHECK(rep3.dirtyFrames(th) == 0);
}

TEST_CASE("PRESENCE verdicts ignore gain and pan; the L/R energy ratio tells the raw bus from the SCVB mix",
          "[presence]")
{
    constexpr int kFrames = 96;
    const std::int64_t len = kFrames * static_cast<std::int64_t>(kFrame);

    SECTION("per-lane gain from 0 dB down to about -20 dB, pans from centre to strongly one-sided")
    {
        const Bus bus =
            render(kT0, len, {panned(0, 1.0, 1.0), panned(1, 0.45, 0.15), panned(2, 0.02, 0.1), panned(3, 0.07, 0.07)});
        const CombReport rep = analyze(bus.span(), {0, 1, 2, 3});
        REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));
        for (const LaneReport& lr : rep.lanes)
        {
            INFO("lane " << lr.lane << " gain.mix " << lr.gain.mix);
            CHECK(lr.count(Verdict::Present) == kFrames);
            CHECK(countSource(lr, Source::Mix) == kFrames);
        }
    }

    SECTION("raw bus: host sends each lane to L only at gain 0.5")
    {
        std::vector<LaneRender> raw;
        for (int id = 0; id < 4; ++id)
        {
            raw.push_back(panned(id, kRawBusGainL, kRawBusGainR));
        }
        const Bus bus = render(kT0, len, raw);
        const CombReport rep = analyze(bus.span(), {0, 1, 2, 3});
        REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));
        for (const LaneReport& lr : rep.lanes)
        {
            INFO("lane " << lr.lane << " gain.raw " << lr.gain.raw);
            CHECK(lr.count(Verdict::Present) == kFrames);
            CHECK(countSource(lr, Source::Raw) == kFrames);
        }
    }

    SECTION("raw to mix hand-over on a frame boundary stays Present on both sides")
    {
        // 前半段 lane 1 是未平衡原声(只进 L、0.5),后半段换成 SCVB 混音(居中 0.7)。
        const std::int64_t cut = kT0 + (kFrames / 2) * static_cast<std::int64_t>(kFrame);
        LaneRender before = panned(1, kRawBusGainL, kRawBusGainR);
        before.onUntil = cut;
        LaneRender after = lane(1);
        after.onFrom = cut;
        const Bus bus = render(kT0, len, {lane(0), before, after, lane(2)});
        const CombReport rep = analyze(bus.span(), {0, 1, 2});
        REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));
        const LaneReport& l1 = laneOf(rep, 1);
        INFO("gain.raw " << l1.gain.raw << " gain.mix " << l1.gain.mix);
        CHECK(l1.count(Verdict::Present) == kFrames);
        CHECK(countSource(l1, Source::Raw) == kFrames / 2);
        CHECK(countSource(l1, Source::Mix) == kFrames / 2);
        CHECK(l1.sources.front() == Source::Raw);
        CHECK(l1.sources.back() == Source::Mix);
    }
}

TEST_CASE("PRESENCE amplitude options: ampTol < 0 tolerates a gain ramp, fixedGain exposes a constant-factor boost",
          "[presence]")
{
    constexpr int kFrames = 96;
    const std::int64_t len = kFrames * static_cast<std::int64_t>(kFrame);

    SECTION("gain ramp: default tolerance flags it, ampTol < 0 accepts it")
    {
        // lane 0 的增益在整段里从 1.0 线性降到 0.3(被测增益本身随时间变);lane 1 恒定。
        Bus bus = render(kT0, len, {lane(1)});
        const auto tone = laneTone(0);
        for (std::int64_t i = 0; i < len; ++i)
        {
            const double g = 1.0 - 0.7 * static_cast<double>(i) / static_cast<double>(len);
            const double v = 0.7 * g * sampleAt(tone, kT0 + i);
            bus.left[static_cast<std::size_t>(i)] += static_cast<float>(v);
            bus.right[static_cast<std::size_t>(i)] += static_cast<float>(v);
        }
        const CombReport strict = analyze(bus.span(), {0, 1});
        REQUIRE(strict.frameStarts.size() == static_cast<std::size_t>(kFrames));
        // 前提:默认口径下斜坡确实会被当成「幅度不符」。
        CHECK(laneOf(strict, 0).countWhy(Why::AmpMismatch) > 0);

        Thresholds loose;
        loose.ampTol = -1.0;
        const CombReport rep = analyze(bus.span(), {0, 1}, {}, loose);
        REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));
        CHECK(laneOf(rep, 0).count(Verdict::Present) == kFrames);
        CHECK(laneOf(rep, 1).count(Verdict::Present) == kFrames);
    }

    SECTION("constant 2x boost: invisible to self-calibration, caught by fixedGain")
    {
        // lane 0 被常数倍放大(例如原声与混音两条路叠在一起);lane 1 是正常的 0.7 居中。
        const Bus bus = render(kT0, len, {panned(0, 1.4, 1.4), lane(1)});
        const CombReport autoRep = analyze(bus.span(), {0, 1});
        REQUIRE(autoRep.frameStarts.size() == static_cast<std::size_t>(kFrames));
        // 头注「不覆盖的」那一条:自标定把 2 倍当成了参考,照样全帧 Present。
        CHECK(laneOf(autoRep, 0).count(Verdict::Present) == kFrames);

        const scvb::testsupport::presence::GainRef known{0.5, 1.4}; // 原声 0.5;混音 0.7 + 0.7
        const CombReport rep = analyze(bus.span(), {0, 1}, {}, Thresholds{}, &known);
        REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));
        CHECK(laneOf(rep, 0).countWhy(Why::AmpMismatch) == kFrames);
        CHECK(laneOf(rep, 1).count(Verdict::Present) == kFrames);
    }

    SECTION("lanes outside the comb are refused with an empty report")
    {
        const Bus bus = render(kT0, len, {lane(0)});
        const CombReport rep = analyze(bus.span(), {0, kMaxLanes});
        CHECK(rep.frameStarts.empty());
        CHECK(rep.lanes.empty());
        CHECK(analyze(bus.span(), {-1}).lanes.empty());
    }
}

TEST_CASE("PRESENCE zero-control frequencies stay Absent on every glitch-free render (noise floor calibration)",
          "[presence]")
{
    const Thresholds th;
    constexpr int kFrames = 64;
    const std::int64_t len = kFrames * static_cast<std::int64_t>(kFrame);

    // 全梳 16 条同时在场(各压低 -16 dB,和不削波):每条都 Present,串扰为数值噪声级。
    std::vector<LaneRender> full;
    std::vector<int> ids;
    for (int id = 0; id < kMaxLanes; ++id)
    {
        full.push_back(panned(id, 0.16, 0.16));
        ids.push_back(id);
    }

    struct Case
    {
        const char* name;
        std::vector<LaneRender> lanes;
    };
    const std::vector<Case> cases = {
        {"ideal", {lane(0), lane(1), lane(2), lane(3)}},
        {"R alias", {lane(0), shifted(1, kRingR), lane(2), lane(3)}},
        {"2R alias", {lane(0), shifted(1, 2 * kRingR), lane(2), lane(3)}},
        {"gain and pan", {panned(0, 1.0, 1.0), panned(1, 0.45, 0.15), panned(2, 0.02, 0.1), panned(3, 0.07, 0.07)}},
        {"raw bus",
         {panned(0, kRawBusGainL, kRawBusGainR), panned(1, kRawBusGainL, kRawBusGainR),
          panned(2, kRawBusGainL, kRawBusGainR), panned(3, kRawBusGainL, kRawBusGainR)}},
        {"full comb", full},
    };

    double worst = 0.0;
    for (const Case& c : cases)
    {
        const Bus bus = render(kT0, len, c.lanes);
        const bool isFull = (c.lanes.size() == full.size());
        const CombReport rep = analyze(bus.span(), isFull ? ids : std::vector<int>{0, 1, 2, 3});
        REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));
        INFO("case " << c.name << " max probe " << msToDbfs(rep.maxProbe()) << " dBFS, floor "
                     << msToDbfs(th.floorAbsent) << " dBFS");
        CHECK(rep.controlNonAbsent(th) == 0);
        CHECK(rep.dirtyFrames(th) == 0);
        worst = rep.maxProbe() > worst ? rep.maxProbe() : worst;
        if (isFull)
        {
            for (const LaneReport& lr : rep.lanes)
            {
                INFO("full comb lane " << lr.lane);
                CHECK(lr.count(Verdict::Present) == kFrames);
            }
        }
    }
    // 标定:实测噪声底至少比 Absent 下限低 30 dB(均方差 1000 倍)。
    INFO("worst probe " << msToDbfs(worst) << " dBFS vs floor " << msToDbfs(th.floorAbsent) << " dBFS");
    CHECK(worst < th.floorAbsent * 1.0e-3);
}
