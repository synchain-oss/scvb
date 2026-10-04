// SPDX-License-Identifier: GPL-3.0-or-later
// test_host_sched —— 宿主调度模拟器 SchedRig(tests/host/sched/SchedRig.h)的框架自测。
//
// 本文件里的用例一律打 `[.][sched]`:
//   · `[.]` 隐藏 ⇒ 主条目 `scvb_host_tests`(命令行不带过滤)默认不跑它们;
//   · ctest 条目 `scvb_host_sched_tests` 以 `scvb_host_tests "[sched]"` 显式选中它们。
//
// 自测清单(A-2 验收 ①–⑥,外加 ⓪ 纯函数层):
//   ⓪ TransportMap / LanePlayHead:Cycle 回绕、定位、停起走带、块在循环点处切开、读方偏移、起播怪癖;
//   ① 理想调度(Δ=0、全 live)稳定后 100% Present、misalign=0;
//   ② 旁路一条 lane 的 Input:只有该轨在 Output 上变 Absent,而宿主总线上它的原声仍在 ——
//      证明判定走的是真实 Output 混音路径,不是宿主侧静音(host 静音判据会空转的教训);
//   ③ 原声(只进 L)与 SCVB 混音(居中)在 L/R 判据上分得开;
//   ④ 违反图依赖时不变式检查会触发(标注过的只记账,未标注的另计);
//   ⑤ pace=1 跑 2 s,墙钟在 ±20% 内;
//   ⑥ 纯时间线场景(Cycle + 定位 + 静音区 + 一条领先的预取轨)连跑 3 次,逐帧判定日志逐字相同;
//   ⑦ 事件与停调策略的落账(live 切换三种方式、带在途旧块的定位、停 / 起走带、起播怪癖、无 region、
//      静音即停调 Input、总线静音尾巴停调 Output、整链停调、几何重写、离线):只看调度账,与墙钟无关。
//
// 删除式(PR 里有结果表):总线输入改喂零 ⇒ ③ 红;去掉不变式检查 ⇒ ④ 红。
//
// 运行期文案一律 ASCII(中文字面量在本机 CP936 上会触发 C4819);中文只在注释里。

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

// 同机独占守卫:本二进制已由 test_host_harness.cpp 引入一次;这里再引一次是为了让 sched 用例
// 不依赖「别的 TU 恰好引过」(头注:每进程至多取一次锁,重复引入安全)。只在 Windows 上有这把锁。
#if defined(_WIN32)
#include "support/exclusive_guard.h"
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sched/SchedRig.h"
#include "support/presence_meter.h"

namespace
{

namespace presence = scvb::testsupport::presence;
namespace sched = scvb::testsupport::sched;

std::int64_t sec(double s)
{
    return sched::secondsToSamples(s);
}

// Output 块表里游标 >= c 的那几块(按处理顺序)。
std::vector<sched::OutBlock> outBlocksFrom(const sched::SchedRig& rig, std::int64_t c)
{
    std::vector<sched::OutBlock> out;
    for (const sched::OutBlock& b : rig.outBlocks())
    {
        if (b.cursor >= c)
        {
            out.push_back(b);
        }
    }
    return out;
}

// lane 块日志里游标 >= c 的那几块(按处理顺序;被冲刷后重新渲染的游标会出现两次)。
std::vector<sched::LaneBlock> laneBlocksFrom(const sched::SchedRig& rig, int lane, std::int64_t c)
{
    std::vector<sched::LaneBlock> out;
    for (const sched::LaneBlock& b : rig.laneLog(lane))
    {
        if (b.cursor >= c)
        {
            out.push_back(b);
        }
    }
    return out;
}

} // namespace

TEST_CASE("SCHED scaffold: the presence meter judges an ideal two-lane bus Present inside the host target",
          "[.][sched]")
{
    constexpr int kFrames = 16;
    const std::int64_t len = kFrames * static_cast<std::int64_t>(presence::kFrame);
    const std::int64_t t0 = 7 * static_cast<std::int64_t>(presence::kFrame);

    std::vector<float> left(static_cast<std::size_t>(len), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(len), 0.0f);
    for (int lane = 0; lane < 2; ++lane)
    {
        presence::addLane(lane, t0, left.data(), len, 0.7);
        presence::addLane(lane, t0, right.data(), len, 0.7);
    }
    const presence::CombReport rep = presence::analyze(presence::Span{left.data(), right.data(), t0, len}, {0, 1});
    REQUIRE(rep.frameStarts.size() == static_cast<std::size_t>(kFrames));
    for (const presence::LaneReport& lr : rep.lanes)
    {
        INFO("lane " << lr.lane);
        CHECK(lr.count(presence::Verdict::Present) == kFrames);
    }
}

// ---------------------------------------------------------------------------
// ⓪ 纯函数层:游标 → 时间线。后面所有场景的「时间线位置」都从这里来,它错了全盘错。
// ---------------------------------------------------------------------------
TEST_CASE("SCHED rig 0: transport map wraps cycles, applies seek/stop/start, and blocks are cut at loop points",
          "[.][sched]")
{
    sched::TransportMap m(0, true);
    sched::TransportEvent cyc;
    cyc.at = 0;
    cyc.kind = sched::TransportKind::Cycle;
    cyc.pos = 1000;
    cyc.loopEnd = 3000;
    m.add(cyc);

    // 从 0 放到 loopE 再回 loopB,之后每 2000 个样本回绕一次。
    CHECK(m.at(0).t == 0);
    CHECK(m.at(2999).t == 2999);
    CHECK(m.at(3000).t == 1000);
    CHECK(m.at(4999).t == 2999);
    CHECK(m.at(5000).t == 1000);
    CHECK(m.nextBoundary(0) == 3000);
    CHECK(m.nextBoundary(3000) == 5000);
    CHECK(m.nextBoundary(4999) == 5000);
    // 时间线切点:游标 3500 处 t=1500,走到 t=2000 还要 500。
    CHECK(m.nextBoundary(3500, {2000}) == 4000);

    // 游标 5500 处定位到 10000:越过 loopE 之后不再回绕(Cycle 仍开着)。
    sched::TransportEvent sk;
    sk.at = 5500;
    sk.kind = sched::TransportKind::Seek;
    sk.pos = 10000;
    m.add(sk);
    CHECK(m.at(5499).t == 1499);
    CHECK(m.nextBoundary(5000) == 5500);
    CHECK(m.at(5500).t == 10000);
    CHECK(m.at(6000).t == 10500);
    CHECK(m.at(6000).cycleOn);
    CHECK(m.nextBoundary(5500) == sched::kFar);

    // 游标 7000 停走带:t 冻在 11500;9000 处从 2000 起播,回到 Cycle 里继续回绕。
    sched::TransportEvent stop;
    stop.at = 7000;
    stop.kind = sched::TransportKind::Stop;
    m.add(stop);
    sched::TransportEvent start;
    start.at = 9000;
    start.kind = sched::TransportKind::Start;
    start.pos = 2000;
    start.hasPos = true;
    m.add(start);
    CHECK(m.at(7000).t == 11500);
    CHECK_FALSE(m.at(7000).playing);
    CHECK(m.at(8999).t == 11500);
    CHECK(m.nextBoundary(7000) == 9000);
    CHECK(m.at(9000).playing);
    CHECK(m.at(9999).t == 2999);
    CHECK(m.at(10000).t == 1000);
    CHECK(m.nextBoundary(9000) == 10000);

    // LanePlayHead:块在循环点 / 事件点处切开。
    sched::LanePlayHead h(m, sched::kSchedSr);
    h.setCursor(2500);
    CHECK(h.maxBlock(1024) == 500);
    h.setCursor(3000);
    CHECK(h.maxBlock(1024) == 1024);
    h.setCursor(4500);
    CHECK(h.maxBlock(1024) == 500);
    h.setCursor(5300);
    CHECK(h.maxBlock(1024) == 200);
    h.setCursor(0);
    CHECK(h.maxBlock(256, {100}) == 100);

    // 读方偏移:报给插件的是 map(c + d),块同时在两个视角的不连续点处切开。
    h.setCursor(1000);
    h.setReaderOffset(1800);
    CHECK(h.maxBlock(1024) == 200);
    sched::BlockPos bp = h.plan(200);
    CHECK(bp.bus.t == 1000);
    CHECK(bp.tReported == 2800);
    h.setReaderOffset(0);

    // 起播怪癖:只改接下来几次被调用的块报出的位置,游标与总线时间不动。
    h.setCursor(0);
    h.pushQuirk(sched::Quirk::garbage(540544, 2));
    for (int k = 0; k < 3; ++k)
    {
        sched::BlockPos q = h.plan(256);
        h.arm(q);
        const auto pos = h.getPosition();
        REQUIRE(pos.hasValue());
        REQUIRE(pos->getTimeInSamples().hasValue());
        const std::int64_t expected = k < 2 ? 540544 : 512;
        CHECK(*pos->getTimeInSamples() == expected);
        CHECK(q.quirked == (k < 2));
        CHECK(q.bus.t == 256 * k);
        CHECK(pos->getIsLooping());
        h.advance(256);
    }
    CHECK(sched::Quirk::preroll(-383, 64).reported == std::vector<std::int64_t>{-383, -319, -255, -191, -127, -63});
    CHECK(sched::Quirk::offByOne(-179, 1868).reported == std::vector<std::int64_t>{-179, 1868});
}

// ---------------------------------------------------------------------------
// ① 理想调度:3 条 live、Δ=0、块长 1024。交接走完后 2 s 里逐帧 Present(SCVB 混音)、零失准。
// ---------------------------------------------------------------------------
TEST_CASE("SCHED rig 1: ideal schedule (all live, delta 0) settles to 100% Present with zero misalign", "[.][sched]")
{
    sched::SchedRig rig(sched::liveLanes(3));
    REQUIRE(rig.settle());

    const std::int64_t c0 = rig.outCursor();
    const std::size_t beat0 = rig.beats().size();
    std::vector<std::uint32_t> gap0;
    for (int lane = 0; lane < rig.laneCount(); ++lane)
    {
        gap0.push_back(rig.output().gapCount(rig.channelOf(lane)));
    }
    rig.runFor(sec(2.0));
    const std::int64_t c1 = rig.outCursor();

    const sched::Analysis a = rig.analyzeOutput(c0, c1);
    CHECK(a.spans == 1);
    CHECK(a.controlNonAbsent == 0); // 稳态没有硬切
    for (int lane = 0; lane < rig.laneCount(); ++lane)
    {
        INFO("lane " << lane);
        REQUIRE(a.total(lane) >= 90);
        CHECK(a.count(lane, presence::Verdict::Present) == a.total(lane));
        CHECK(a.countSource(lane, presence::Verdict::Present, presence::Source::Mix) == a.total(lane));
        CHECK(rig.output().gapCount(rig.channelOf(lane)) == gap0[static_cast<std::size_t>(lane)]);
    }

    // [M] 拍:窗口内每一拍都零失准、无挂起、心跳新鲜。
    REQUIRE(rig.beats().size() > beat0 + 10);
    int badBeats = 0;
    for (std::size_t i = beat0; i < rig.beats().size(); ++i)
    {
        for (const sched::LaneBeat& lb : rig.beats()[i].lanes)
        {
            if (lb.misalign != 0 || lb.suspended || lb.slotState != scvb::kSlotActive)
            {
                ++badBeats;
            }
        }
    }
    CHECK(badBeats == 0);

    CHECK(rig.invariant().checked == rig.cycles());
    CHECK(rig.invariant().declared == 0);
    CHECK(rig.invariant().undeclared == 0);
    CHECK(rig.busUnderflowSamples() == 0);
}

// ---------------------------------------------------------------------------
// ② 旁路 lane 1 的 Input(宿主不调用它、把它的源信号原样送进总线):
//    Output 上只有 lane 1 变 Absent,lane 0/2 照旧 Present;同一窗口里宿主总线上 lane 1 的原声
//    是 Present —— 若判定只看宿主侧(总线)而不经过 Output 的真实混音,这里会判它在场。
// ---------------------------------------------------------------------------
TEST_CASE("SCHED rig 2: bypassing one lane's Input makes only that lane Absent at the Output", "[.][sched]")
{
    sched::SchedRig rig(sched::liveLanes(3));
    REQUIRE(rig.settle());

    const std::int64_t c0 = rig.outCursor();
    rig.runFor(sec(0.5));
    const std::int64_t cb = rig.outCursor();
    rig.bypassInput(1, true);
    rig.runFor(sec(1.5));
    const std::int64_t ce = rig.outCursor();
    const std::int64_t cw = cb + 2 * presence::kFrame; // 跳过切换处的过渡帧

    const sched::Analysis out = rig.analyzeOutput(c0, ce);
    for (int lane = 0; lane < rig.laneCount(); ++lane)
    {
        INFO("before bypass, lane " << lane);
        REQUIRE(out.total(lane, c0, cb) >= 20);
        CHECK(out.count(lane, presence::Verdict::Present, c0, cb) == out.total(lane, c0, cb));
    }
    REQUIRE(out.total(1, cw, ce) >= 60);
    CHECK(out.count(1, presence::Verdict::Absent, cw, ce) == out.total(1, cw, ce));
    for (int lane : {0, 2})
    {
        INFO("during bypass, lane " << lane);
        CHECK(out.countSource(lane, presence::Verdict::Present, presence::Source::Mix, cw, ce) ==
              out.total(lane, cw, ce));
    }

    // 宿主侧:lane 1 的原声一直在总线上;lane 0/2 的 Input 已自静音,总线上没有它们。
    const sched::Analysis bus = rig.analyzeBus(cw, ce);
    REQUIRE(bus.total(1) >= 60);
    CHECK(bus.countSource(1, presence::Verdict::Present, presence::Source::Raw) == bus.total(1));
    CHECK(bus.count(0, presence::Verdict::Absent) == bus.total(0));
    CHECK(bus.count(2, presence::Verdict::Absent) == bus.total(2));

    // 块日志:旁路期间 lane 1 一次都没被调用。
    int called = 0;
    int bypassed = 0;
    for (const sched::LaneBlock& b : rig.laneLog(1))
    {
        if (b.cursor >= cb)
        {
            called += b.reason == sched::CallReason::Called ? 1 : 0;
            bypassed += b.reason == sched::CallReason::Bypassed ? 1 : 0;
        }
    }
    CHECK(called == 0);
    CHECK(bypassed > 0);
}

// ---------------------------------------------------------------------------
// ③ 原声 vs SCVB 混音。冷启动(不泵消息 ⇒ [M] 冻结 ⇒ Input 直通、Output 未注入)时 Output 原样
//    直通总线:每轨都是只在 L 上的原声(Raw);交接走完后每轨都是居中的混音(Mix)。
//    删除式:总线输入改喂零 ⇒ 冷启动段每轨 Absent ⇒ 本用例红。
// ---------------------------------------------------------------------------
TEST_CASE("SCHED rig 3: raw passthrough and the SCVB mix are told apart by the L/R source check", "[.][sched]")
{
    sched::SchedRig rig(sched::liveLanes(3));

    rig.setPumping(false);
    const std::int64_t c0 = rig.outCursor();
    rig.runFor(sec(1.0));
    const std::int64_t c1 = rig.outCursor();

    REQUIRE(rig.settle());
    const std::int64_t c2 = rig.outCursor();
    rig.runFor(sec(1.0));
    const std::int64_t c3 = rig.outCursor();

    const sched::Analysis cold = rig.analyzeOutput(c0, c1);
    const sched::Analysis warm = rig.analyzeOutput(c2, c3);
    for (int lane = 0; lane < rig.laneCount(); ++lane)
    {
        INFO("lane " << lane);
        REQUIRE(cold.total(lane) >= 40);
        REQUIRE(warm.total(lane) >= 40);
        CHECK(cold.countSource(lane, presence::Verdict::Present, presence::Source::Raw) == cold.total(lane));
        CHECK(warm.countSource(lane, presence::Verdict::Present, presence::Source::Mix) == warm.total(lane));
    }

    // 声道能量直接看一眼:原声段 R 恒为 0、L 有声;混音段 L、R 同量级。
    const auto coldMs = rig.outMeanSquare(c0, c1);
    const auto warmMs = rig.outMeanSquare(c2, c3);
    INFO("cold L/R ms " << coldMs.first << " / " << coldMs.second << ", warm L/R ms " << warmMs.first << " / "
                        << warmMs.second);
    CHECK(coldMs.first > 1.0e-4);
    CHECK(coldMs.second == 0.0);
    REQUIRE(warmMs.first > 1.0e-4);
    CHECK(warmMs.second / warmMs.first > 0.5);
    CHECK(warmMs.second / warmMs.first < 2.0);
}

// ---------------------------------------------------------------------------
// ④ 图依赖不变式。lane 0/1 live、lane 2 预取(提前 4096)。
//    对照段不触发;outputFirst 只抓 live 轨;readerOffset=8192 连提前 4096 的预取轨一起抓;
//    不标注的违反另计 undeclared(平时直接 FAIL_CHECK,这里临时关掉以便计数)。
//    删除式:去掉不变式检查 ⇒ declared 恒 0 ⇒ 本用例红。
// ---------------------------------------------------------------------------
TEST_CASE("SCHED rig 4: violating the graph order trips the invariant check", "[.][sched]")
{
    sched::SchedCfg cfg = sched::liveLanes(3);
    cfg.lanes[2].live = false;
    cfg.lanes[2].lead = 4096;
    sched::SchedRig rig(cfg);

    // 对照:按图顺序调度 ⇒ 每周期都检查、一次不触发;预取轨保持提前量,live 轨与 Output 同步。
    rig.runCycles(20);
    CHECK(rig.invariant().checked == 20);
    CHECK(rig.invariant().declared == 0);
    CHECK(rig.invariant().undeclared == 0);
    CHECK(rig.laneCursor(2) >= rig.outCursor() + 4096);
    CHECK(rig.laneCursor(0) == rig.outCursor());
    CHECK(rig.busUnderflowSamples() == 0);

    // 标注过的 outputFirst:10 个周期全部触发,命中的只有两条 live 轨。
    rig.violateGraphOrder(sched::GraphViolation{sched::GraphViolationKind::OutputFirst, 10, 0, true});
    rig.runCycles(10);
    CHECK(rig.invariant().declared == 10);
    CHECK(rig.invariant().undeclared == 0);
    CHECK(rig.invariant().hitsFor(0, true) == 10);
    CHECK(rig.invariant().hitsFor(1, true) == 10);
    CHECK(rig.invariant().hitsFor(2, false) == 0);
    CHECK(rig.busUnderflowSamples() > 0); // 那 10 个周期 live 轨的总线输入还没产生

    // 标注过的 readerOffset=8192:预取轨只提前 4096,同样被抓。
    rig.violateGraphOrder(sched::GraphViolation{sched::GraphViolationKind::ReaderOffset, 10, 8192, true});
    rig.runCycles(10);
    CHECK(rig.invariant().declared == 20);
    CHECK(rig.invariant().hitsFor(2, true) == 10);

    // 不标注:另计 undeclared。
    rig.setFailOnUndeclared(false);
    rig.violateGraphOrder(sched::GraphViolation{sched::GraphViolationKind::OutputFirst, 5, 0, false});
    rig.runCycles(5);
    rig.setFailOnUndeclared(true);
    CHECK(rig.invariant().undeclared == 5);

    // 到期后恢复正常,不再触发。
    rig.runCycles(10);
    CHECK(rig.invariant().checked == 55);
    CHECK(rig.invariant().declared == 20);
    CHECK(rig.invariant().undeclared == 5);
}

// ---------------------------------------------------------------------------
// ⑤ pace=1:2 s 音频的墙钟时长在 ±20% 内(墙钟门限类场景 500ms / 200ms / 5s 的前提)。
// ---------------------------------------------------------------------------
TEST_CASE("SCHED rig 5: pace=1 keeps the wall clock within 20% of audio time", "[.][sched]")
{
    sched::SchedCfg cfg = sched::liveLanes(2);
    cfg.pace = 1.0;
    sched::SchedRig rig(cfg);

    const std::int64_t c0 = rig.outCursor();
    const std::size_t beat0 = rig.beats().size();
    const double w0 = sched::wallMs();
    rig.runFor(sec(2.0));
    const double wall = sched::wallMs() - w0;
    const double audio = static_cast<double>(rig.outCursor() - c0) * 1000.0 / rig.sr();

    INFO("audio " << audio << " ms, wall " << wall << " ms");
    CHECK(wall >= 0.8 * audio);
    CHECK(wall <= 1.2 * audio);
    CHECK(rig.beats().size() > beat0 + 20); // 实时节拍下 [M] 被持续泵到
}

// ---------------------------------------------------------------------------
// ⑥ 确定性:同一纯时间线脚本连跑 3 次(每次全新建台),逐帧判定日志逐字相同。
//    脚本:定位到 S0 → Cycle [B,E)(1 s)放 3 圈多 → 圈内回跳 → 关 Cycle 前跳到 20 s。
//    lane 2 在圈内有一段静音区;lane 3 是提前 4096 的预取轨(跨循环点会提前回绕)。
//    交接(settle)那段依赖墙钟,不入判定;脚本从定位开始,时间线位置与块格都只由脚本决定。
// ---------------------------------------------------------------------------
TEST_CASE("SCHED rig 6: a pure-timeline scenario gives identical per-frame verdict logs over 3 runs", "[.][sched]")
{
    const std::int64_t s0 = sec(5.0);
    const std::int64_t loopB = s0 + sec(0.25);
    const std::int64_t loopE = loopB + sec(1.0);

    const auto once = [&]() -> std::string {
        sched::SchedCfg cfg = sched::liveLanes(4);
        cfg.lanes[2].silent.push_back(sched::Range{loopB + sec(0.40), loopB + sec(0.55)});
        cfg.lanes[3].live = false;
        cfg.lanes[3].lead = 4096;
        sched::SchedRig rig(cfg);
        REQUIRE(rig.settle());

        const std::int64_t c0 = rig.outCursor();
        rig.seek(sched::kNow, s0);
        rig.cycle(loopB, loopE);
        rig.runFor(sec(3.4));
        rig.seek(sched::kNow, loopB + sec(0.5));
        rig.runFor(sec(1.0));
        rig.cycleOff();
        rig.seek(sched::kNow, sec(20.0));
        rig.runFor(sec(0.6));

        const sched::Analysis a = rig.analyzeOutput(c0, rig.outCursor());
        REQUIRE(a.spans >= 6);
        REQUIRE(a.total(0) >= 150);
        // 判据确实分辨出了东西(不是一份全 Present 的平凡日志):静音区那几帧 lane 2 缺席。
        CHECK(a.count(2, presence::Verdict::Absent) > 0);
        CHECK(rig.invariant().undeclared == 0);
        return a.log();
    };

    const std::string first = once();
    const std::string second = once();
    const std::string third = once();
    REQUIRE_FALSE(first.empty());
    CHECK(second == first);
    CHECK(third == first);
}

// ---------------------------------------------------------------------------
// ⑦ 事件与停调策略的落账:每个调度操作在块表 / 块日志里留下的形状与脚本一致。
//    A-4 的场景全靠这些操作搭,它们自己错了,场景的红绿就没有意义。这里只看调度账(谁在哪个游标、
//    以什么时间线位置、因为什么被调或没被调),不看音频判定;不按节拍、不泵消息([M] 冻结),
//    所以与墙钟无关,逐块确定。
// ---------------------------------------------------------------------------
TEST_CASE("SCHED rig 7: transport events, live switches, quirks and stall policies land in the call log as scripted",
          "[.][sched]")
{
    sched::SchedCfg cfg = sched::liveLanes(3);
    cfg.lanes[1].live = false;
    cfg.lanes[1].lead = 4096;
    cfg.pace = 0.0;
    cfg.unpacedPumpMs = 0;
    sched::SchedRig rig(cfg);
    const std::int64_t b = cfg.outBlock;

    // (a) live / 预取切换的三种方式。
    rig.runCycles(4);
    CHECK(rig.laneCursor(1) == rig.outCursor() + 4096);
    rig.setLive(1, true, sched::LiveSwitch::Rewind); // 超前 ⇒ 退回 Output 游标,丢掉已预取的音频
    CHECK(rig.laneCursor(1) == rig.outCursor());
    rig.runCycles(2);
    CHECK(rig.laneCursor(1) == rig.outCursor());
    rig.setLive(1, false, sched::LiveSwitch::Drain); // 游标不动,下一周期成块追上提前量
    CHECK(rig.laneCursor(1) == rig.outCursor());
    rig.runCycles(1);
    CHECK(rig.laneCursor(1) == rig.outCursor() + 4096);
    const std::int64_t ahead = rig.laneCursor(1);
    rig.setLive(1, true, sched::LiveSwitch::Drain); // 超前的 live 轨空等 Output 追上
    rig.runCycles(2);
    CHECK(rig.laneCursor(1) == ahead);
    rig.setLive(1, false, sched::LiveSwitch::Jump); // 游标直接跳到新目标
    CHECK(rig.laneCursor(1) == rig.outCursor() + 4096);

    // (b) 定位 + 2 个在途旧块:Output 先放完两块旧时间线再换位置;超过生效点的预取轨被冲刷回生效点,
    //     在新时间线上重新渲染。
    rig.runCycles(1);
    const std::int64_t a = rig.outCursor();
    const std::int64_t tBefore = rig.map().at(a).t;
    rig.seek(sched::kNow, sec(30.0), 2);
    rig.runCycles(4);
    {
        const std::vector<sched::OutBlock> ob = outBlocksFrom(rig, a);
        REQUIRE(ob.size() == 4u);
        CHECK(ob[0].tBus == tBefore);
        CHECK(ob[1].tBus == tBefore + b);
        CHECK(ob[2].tBus == sec(30.0));
        CHECK(ob[3].tBus == sec(30.0) + b);
        std::int64_t rerendered = -1;
        for (const sched::LaneBlock& lb : laneBlocksFrom(rig, 1, a))
        {
            if (lb.cursor == a + 2 * b)
            {
                rerendered = lb.tBus; // 取最后一次:冲刷后在新时间线上重新渲染的那块
            }
        }
        CHECK(rerendered == sec(30.0));
    }

    // (c) 停走带 / 起走带:停住期间时间线冻结、各轨源信号静音;起走带从冻结处接着放。
    const std::int64_t s = rig.outCursor();
    const std::int64_t tStop = rig.map().at(s).t;
    rig.stop();
    rig.runCycles(3);
    rig.start();
    rig.runCycles(2);
    {
        const std::vector<sched::OutBlock> ob = outBlocksFrom(rig, s);
        REQUIRE(ob.size() == 5u);
        for (int k = 0; k < 3; ++k)
        {
            CHECK_FALSE(ob[static_cast<std::size_t>(k)].playingBus);
            CHECK_FALSE(ob[static_cast<std::size_t>(k)].playingReported);
            CHECK(ob[static_cast<std::size_t>(k)].tBus == tStop);
        }
        CHECK(ob[3].playingBus);
        CHECK(ob[3].tBus == tStop);
        CHECK(ob[4].tBus == tStop + b);
        for (const sched::LaneBlock& lb : laneBlocksFrom(rig, 0, s))
        {
            if (lb.cursor < s + 3 * b)
            {
                CHECK_FALSE(lb.playing);
                CHECK_FALSE(lb.contentActive);
                CHECK(lb.reason == sched::CallReason::Called);
            }
        }
    }

    // (d) 起播怪癖:只改报给插件的位置(Output 两块垃圾值;lane 0 两块 -179 → 1868),总线时间不动。
    const std::int64_t q = rig.outCursor();
    rig.quirk(sched::SchedRig::Target::Output, sched::Quirk::garbage(540544, 2));
    rig.quirk(sched::SchedRig::Target::Lane, sched::Quirk::offByOne(-179, 1868), 0);
    rig.runCycles(3);
    {
        const std::vector<sched::OutBlock> ob = outBlocksFrom(rig, q);
        REQUIRE(ob.size() == 3u);
        CHECK(ob[0].quirked);
        CHECK(ob[0].tReported == 540544);
        CHECK(ob[1].quirked);
        CHECK(ob[1].tReported == 540544);
        CHECK_FALSE(ob[2].quirked);
        CHECK(ob[2].tReported == ob[2].tBus);
        const std::vector<sched::LaneBlock> lb = laneBlocksFrom(rig, 0, q);
        REQUIRE(lb.size() == 3u);
        CHECK(lb[0].tReported == -179);
        CHECK(lb[1].tReported == 1868);
        CHECK(lb[0].quirked);
        CHECK(lb[1].quirked);
        CHECK_FALSE(lb[2].quirked);
        CHECK(lb[2].tReported == lb[2].tBus);
    }

    // (e) 停调策略:lane 0 无 region(宿主不调用);lane 2 源信号静音段 + Cubase 式「静音即停调 Input」。
    //     块在区间边沿处切开,区间内一块都不调用。
    const std::int64_t t0 = sec(60.0);
    rig.noRegion(0, t0 + 3000, t0 + 7000);
    rig.silentRegion(2, t0 + 2000, t0 + 5000);
    rig.inputSilenceSuspend(true);
    const std::int64_t e0 = rig.outCursor();
    rig.seek(sched::kNow, t0);
    rig.runCycles(12);
    rig.inputSilenceSuspend(false);
    {
        const auto tally = [&](int lane, sched::CallReason inside, sched::Range r) {
            std::int64_t insideSamples = 0;
            int startsAtBegin = 0;
            int startsAtEnd = 0;
            for (const sched::LaneBlock& lb : laneBlocksFrom(rig, lane, e0))
            {
                const bool in = r.covers(lb.tBus, lb.tBus + lb.n);
                INFO("lane " << lane << " block t=" << lb.tBus << " n=" << lb.n << " reason "
                             << sched::toString(lb.reason));
                CHECK((in || !r.overlaps(lb.tBus, lb.tBus + lb.n))); // 没有跨边沿的块
                CHECK(lb.reason == (in ? inside : sched::CallReason::Called));
                insideSamples += in ? lb.n : 0;
                startsAtBegin += lb.tBus == r.begin ? 1 : 0;
                startsAtEnd += lb.tBus == r.end ? 1 : 0;
            }
            CHECK(insideSamples == r.end - r.begin);
            CHECK(startsAtBegin == 1);
            CHECK(startsAtEnd == 1);
        };
        tally(0, sched::CallReason::NoRegion, sched::Range{t0 + 3000, t0 + 7000});
        tally(2, sched::CallReason::SilenceSuspended, sched::Range{t0 + 2000, t0 + 5000});
        int noRegionIntervals = 0;
        for (const sched::LaneInterval& iv : rig.processedIntervals(0))
        {
            if (iv.reason == sched::CallReason::NoRegion)
            {
                ++noRegionIntervals;
                CHECK(iv.t0 == t0 + 3000);
                CHECK(iv.t1 == t0 + 7000);
            }
        }
        CHECK(noRegionIntervals == 1);
    }

    // (f) Logic / FL 式:总线输入连续静音超过 T 就停调 Output(总线原样直通)。停走带 ⇒ 各轨静音。
    rig.outputSilenceTail(4096);
    const std::int64_t st = rig.outCursor();
    rig.stop();
    rig.runCycles(8);
    {
        const std::vector<sched::OutBlock> ob = outBlocksFrom(rig, st);
        REQUIRE(ob.size() == 8u);
        for (std::size_t k = 0; k < ob.size(); ++k)
        {
            INFO("block " << k);
            CHECK(ob[k].reason == (k < 4 ? sched::CallReason::Called : sched::CallReason::OutputSilenceTail));
        }
        REQUIRE_FALSE(rig.outputSuspendLog().empty());
        CHECK(rig.outputSuspendLog().back().begin == st + 4 * b);
        CHECK(rig.outputSuspendLog().back().end == st + 8 * b);
    }
    rig.outputSilenceTail(-1);

    // (g) 整链停调(只做表征):所有轨源信号静音超过 T ⇒ Input 与 Output 都不调用。
    rig.chainSuspend(2048, sched::ChainBy::In);
    const std::int64_t cs = rig.outCursor();
    rig.runCycles(5);
    {
        const std::vector<sched::OutBlock> ob = outBlocksFrom(rig, cs);
        REQUIRE(ob.size() == 5u);
        for (std::size_t k = 0; k < ob.size(); ++k)
        {
            INFO("block " << k);
            CHECK(ob[k].reason == (k < 2 ? sched::CallReason::Called : sched::CallReason::ChainSuspended));
        }
        REQUIRE_FALSE(rig.chainSuspendLog().empty());
        CHECK(rig.chainSuspendLog().back().begin == cs + 2 * b);
        CHECK(rig.chainSuspendLog().back().end == cs + 5 * b);
        for (const sched::LaneBlock& lb : laneBlocksFrom(rig, 0, cs + 2 * b))
        {
            CHECK(lb.reason == sched::CallReason::ChainSuspended);
        }
    }
    rig.chainSuspend(-1, sched::ChainBy::In);
    rig.start();
    rig.runCycles(2);

    // (h) 几何重写:循环中途把 lane 2 改成 stereo 重新 prepare,之后照常被调用。
    rig.geometryRewrite(2, true);
    CHECK(rig.input(2).getTotalNumInputChannels() == 2);
    const std::int64_t g = rig.outCursor();
    rig.runCycles(2);
    {
        const std::vector<sched::LaneBlock> lb = laneBlocksFrom(rig, 2, g);
        REQUIRE(lb.size() == 2u);
        CHECK(lb[0].reason == sched::CallReason::Called);
        CHECK(lb[1].reason == sched::CallReason::Called);
    }

    // (i) 离线:所有实例 setNonRealtime(on)。
    rig.offline(true);
    CHECK(rig.output().isNonRealtime());
    for (int lane = 0; lane < rig.laneCount(); ++lane)
    {
        CHECK(rig.input(lane).isNonRealtime());
    }
    rig.offline(false);
    CHECK_FALSE(rig.output().isNonRealtime());

    // 全程:按图顺序调度,不变式每周期都查、一次没破;总线输入没有欠账。
    CHECK(rig.invariant().checked == rig.cycles());
    CHECK(rig.invariant().declared == 0);
    CHECK(rig.invariant().undeclared == 0);
    CHECK(rig.busUnderflowSamples() == 0);
}
