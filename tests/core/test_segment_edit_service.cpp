// SPDX-License-Identifier: GPL-3.0-or-later
// test_segment_edit_service —— T29 CRVS 变更事务(结果门控)单测(链接 JUCE UndoManager,离线 Catch2)。
// 覆盖 PR#55 缺陷3:editSegmentTransactional 失败(BadArg/NotAdjacent)不改 CRVS、不新增 undo 步、不触发
// rebuild;成功才压入事务,undo 还原 + 再 rebuild。另覆盖 observer 拒绝的「守卫前置」(见 SERVICE-4,事务层
// 不感知 observer,observer 拒绝由 OutputEditor::isReadOnly 前置,本测试只验证事务层不越权)。

#include <catch2/catch_test_macros.hpp>

#include <juce_data_structures/juce_data_structures.h>

#include <limits>

#include "AnalyzeScopeMath.h"
#include "engine/FreezeBits.h"
#include "SegmentEditService.h"
#include "state/SegmentEdit.h"
#include "state/StateCodec.h"

namespace
{

using scvb::state::CrvsData;
using scvb::state::makeSegmentFlags;
using scvb::state::Segment;
using scvb::state::SegmentEditArgs;
using scvb::state::SegmentEditOp;
using scvb::state::SegmentEditResult;
using scvb::state::SegmentOrigin;

Segment seg(std::int64_t t0, std::int64_t t1, float pan)
{
    Segment s;
    s.t0 = t0;
    s.t1 = t1;
    s.pan = pan;
    s.flags = makeSegmentFlags(SegmentOrigin::Auto, false);
    return s;
}

} // namespace

TEST_CASE("SERVICE-1 不相邻合并失败:不改 CRVS/不新增 undo/不 rebuild", "[segedit][service]")
{
    juce::UndoManager undo;
    CrvsData crvs;
    crvs.versions[0].tracks[0].segments = {seg(0, 1000, 10.0f), seg(1000, 2000, 20.0f), seg(2000, 3000, 30.0f)};
    const std::size_t beforeSize = crvs.versions[0].tracks[0].segments.size();

    int rebuilds = 0;
    SegmentEditArgs args;
    args.op = SegmentEditOp::Merge;
    args.segIdx = 0;
    args.segIdxB = 2; // 不相邻 → NotAdjacent

    const auto r = scvb::output::editSegmentTransactional(undo, crvs, 1, 0, args, [&] { ++rebuilds; });

    REQUIRE(r == SegmentEditResult::NotAdjacent);
    REQUIRE(rebuilds == 0);
    REQUIRE(crvs.versions[0].tracks[0].segments.size() == beforeSize); // 段表未变
    REQUIRE_FALSE(undo.canUndo()); // 无新增 undo 步
}

TEST_CASE("SERVICE-2 越界失败:不新增 undo/不 rebuild", "[segedit][service]")
{
    juce::UndoManager undo;
    CrvsData crvs;
    crvs.versions[0].tracks[0].segments = {seg(0, 1000, 10.0f)};

    int rebuilds = 0;
    SegmentEditArgs args;
    args.op = SegmentEditOp::SetValues;
    args.segIdx = 9; // 越界
    args.hasPan = true;
    args.pan = 42.0f;

    const auto r = scvb::output::editSegmentTransactional(undo, crvs, 1, 0, args, [&] { ++rebuilds; });

    REQUIRE(r == SegmentEditResult::BadArg);
    REQUIRE(rebuilds == 0);
    REQUIRE(crvs.versions[0].tracks[0].segments[0].pan == 10.0f);
    REQUIRE_FALSE(undo.canUndo());
}

TEST_CASE("SERVICE-3 成功压事务:perform/undo 各 rebuild 一次且值往返", "[segedit][service]")
{
    juce::UndoManager undo;
    CrvsData crvs;
    crvs.versions[0].tracks[0].segments = {seg(0, 1000, 10.0f)};

    int rebuilds = 0;
    SegmentEditArgs args;
    args.op = SegmentEditOp::SetValues;
    args.segIdx = 0;
    args.hasPan = true;
    args.pan = 42.0f;

    const auto r = scvb::output::editSegmentTransactional(undo, crvs, 1, 0, args, [&] { ++rebuilds; });

    REQUIRE(r == SegmentEditResult::Ok);
    REQUIRE(rebuilds == 1); // perform 触发一次 rebuild
    REQUIRE(undo.canUndo());
    REQUIRE(crvs.versions[0].tracks[0].segments[0].pan == 42.0f);

    REQUIRE(undo.undo());
    REQUIRE(rebuilds == 2); // undo 再触发一次 rebuild
    REQUIRE(crvs.versions[0].tracks[0].segments[0].pan == 10.0f); // 还原旧值

    REQUIRE(undo.redo());
    REQUIRE(crvs.versions[0].tracks[0].segments[0].pan == 42.0f); // redo 重放
}

TEST_CASE("SERVICE-4 通用 commitCrvsTransaction:undo 还原 meta.name", "[segedit][service]")
{
    juce::UndoManager undo;
    CrvsData crvs;
    crvs.versions[0].meta.name = "V1";

    scvb::output::commitCrvsTransaction(undo, crvs, "Rename V1", [&] { crvs.versions[0].meta.name = "Lead"; }, [] {});

    REQUIRE(crvs.versions[0].meta.name == "Lead");
    REQUIRE(undo.canUndo());
    REQUIRE(undo.undo());
    REQUIRE(crvs.versions[0].meta.name == "V1"); // 还原
}

// ---------------------------------------------------------------------------
// T37 三轮 D 族回归:setTrackManual 的两个维度不得互相冲掉。
// 真机症状:「改了音量再改 pan,音量退回默认;先 pan 后音量,pan 回居中」。
// 根因是构造常值段时把**另一维**硬写成默认值,而 UI 的读回值对 pan/vol 读的是同一段。
// ---------------------------------------------------------------------------

TEST_CASE("SERVICE-5 makeManualConstantSegment:写 pan 保留既有 vol", "[segedit][service][t37]")
{
    std::vector<Segment> existing;
    Segment prev;
    prev.t0 = 0;
    prev.t1 = static_cast<std::int64_t>(1) << 40;
    prev.pan = 0.0f;
    prev.volDb = -6.0f; // 用户先调过音量
    prev.flags = makeSegmentFlags(SegmentOrigin::UserEdited, false);
    existing.push_back(prev);

    const Segment seg = scvb::output::makeManualConstantSegment(existing, /*isPan=*/true, 40.0f);

    REQUIRE(seg.pan == 40.0f);
    REQUIRE(seg.volDb == -6.0f); // ← 修复前这里是 0.0f(音量被打回默认)
    REQUIRE(scvb::state::segmentOrigin(seg.flags) == SegmentOrigin::UserEdited);
    REQUIRE(seg.t0 == 0);
}

TEST_CASE("SERVICE-6 makeManualConstantSegment:写 vol 保留既有 pan", "[segedit][service][t37]")
{
    std::vector<Segment> existing;
    Segment prev;
    prev.t0 = 0;
    prev.t1 = static_cast<std::int64_t>(1) << 40;
    prev.pan = -75.0f; // 用户先调过 pan
    prev.volDb = 0.0f;
    prev.flags = makeSegmentFlags(SegmentOrigin::UserEdited, false);
    existing.push_back(prev);

    const Segment seg = scvb::output::makeManualConstantSegment(existing, /*isPan=*/false, 3.5f);

    REQUIRE(seg.volDb == 3.5f);
    REQUIRE(seg.pan == -75.0f); // ← 修复前这里是 0.0f(pan 回居中)
}

TEST_CASE("SERVICE-7 makeManualConstantSegment:空表落默认 + 越界钳制", "[segedit][service][t37]")
{
    const std::vector<Segment> empty;

    const Segment a = scvb::output::makeManualConstantSegment(empty, /*isPan=*/true, 999.0f);
    REQUIRE(a.pan == 100.0f); // 钳到 +100
    REQUIRE(a.volDb == 0.0f); // 空表 → vol 默认 0dB

    const Segment b = scvb::output::makeManualConstantSegment(empty, /*isPan=*/false, -999.0f);
    REQUIRE(b.volDb == -24.0f); // 钳到 -24dB
    REQUIRE(b.pan == 0.0f); // 空表 → pan 默认居中
}

TEST_CASE("SERVICE-8 makeManualConstantSegment:交替写两维互不干扰(真机复现序列)", "[segedit][service][t37]")
{
    std::vector<Segment> segs; // 从空表起步

    // 先调音量 → 再调 pan → 再调音量:三步之后两维都应是最后一次写入的值。
    segs.assign(1, scvb::output::makeManualConstantSegment(segs, /*isPan=*/false, -8.0f));
    REQUIRE(segs[0].volDb == -8.0f);

    segs.assign(1, scvb::output::makeManualConstantSegment(segs, /*isPan=*/true, 55.0f));
    REQUIRE(segs[0].pan == 55.0f);
    REQUIRE(segs[0].volDb == -8.0f); // 音量没被 pan 冲掉

    segs.assign(1, scvb::output::makeManualConstantSegment(segs, /*isPan=*/false, 2.0f));
    REQUIRE(segs[0].volDb == 2.0f);
    REQUIRE(segs[0].pan == 55.0f); // pan 没被音量冲掉
}

// ---------------------------------------------------------------------------
// [J85] / #106 复审重要4:clampManualValue 是**两条通道共用的那把尺子** —— 冻结通道不建段,
// 值直接落参数面,而冻结维度的参数面就是 DspArbiter 的音频目标值。所以它必须自己拦住
// 非有限值:std::clamp(NaN, lo, hi) 两次比较全 false,会把 NaN 原样吐回去。
// 四个边界 + NaN/±Inf 全部离线断死(纯函数,不需要 JUCE 宿主)。
// ---------------------------------------------------------------------------
TEST_CASE("SERVICE-9 clampManualValue:四边界 + 非有限值", "[segedit][service][j85]")
{
    // 四个边界:pan ±100 / vol −24..+12(契约 §1.16 的 `value` 域),边界值本身必须原样通过。
    CHECK(scvb::output::clampManualValue(/*isPan=*/true, -100.0f) == -100.0f);
    CHECK(scvb::output::clampManualValue(/*isPan=*/true, 100.0f) == 100.0f);
    CHECK(scvb::output::clampManualValue(/*isPan=*/false, -24.0f) == -24.0f);
    CHECK(scvb::output::clampManualValue(/*isPan=*/false, 12.0f) == 12.0f);

    // 越界:各自夹到自己那一侧(两个维度的域不同,不能共用一套上下限)。
    CHECK(scvb::output::clampManualValue(/*isPan=*/true, 999.0f) == 100.0f);
    CHECK(scvb::output::clampManualValue(/*isPan=*/true, -999.0f) == -100.0f);
    CHECK(scvb::output::clampManualValue(/*isPan=*/false, 999.0f) == 12.0f);
    CHECK(scvb::output::clampManualValue(/*isPan=*/false, -999.0f) == -24.0f);
    // vol 的上限不是 pan 的上限 —— 一套上下限吃两维会让 +100 的 vol 悄悄过关。
    CHECK(scvb::output::clampManualValue(/*isPan=*/false, 100.0f) == 12.0f);

    // 非有限值:一律回中性值 0(pan 居中 / vol 0dB),**绝不许原样穿出去**。
    // 修复前 std::clamp(NaN,…) 返回 NaN → convertTo0to1(NaN) → rawPan/rawVol = NaN →
    // DspArbiter 冻结分支拿它当 pan/增益目标 → 整条总线出 NaN。
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(scvb::output::clampManualValue(/*isPan=*/true, nan) == 0.0f);
    CHECK(scvb::output::clampManualValue(/*isPan=*/false, nan) == 0.0f);
    CHECK(scvb::output::clampManualValue(/*isPan=*/true, inf) == 0.0f);
    CHECK(scvb::output::clampManualValue(/*isPan=*/false, -inf) == 0.0f);
    // 建段通道走的是同一把尺子:NaN 不得落进段表(两维分别验)。
    const std::vector<Segment> empty;
    CHECK(scvb::output::makeManualConstantSegment(empty, /*isPan=*/true, nan).pan == 0.0f);
    CHECK(scvb::output::makeManualConstantSegment(empty, /*isPan=*/false, nan).volDb == 0.0f);
}

// ---------------------------------------------------------------------------
// [J85] / #106 复审建议⑥:freeze 位解码只许有一份口径 —— 音频线程的 DspArbiter::readFrz 与
// 消息线程的 setTrackManual / ctrl 广播 / 分析入参共用 scvb::engine::freezeBitsOf。
// 两边分叉 = 「一边按未冻结去写曲线、另一边按已冻结去读参数面」,正是 J85 要根治的错位。
// ---------------------------------------------------------------------------
TEST_CASE("SERVICE-10 freezeBitsOf:四态 / 四舍五入 / 越界 / 非有限", "[segedit][service][j85]")
{
    using scvb::engine::freezeBitsOf;
    using scvb::engine::freezeHasDim;

    // 四态(J65:bit0=pan / bit1=vol)
    CHECK(freezeBitsOf(0.0f) == 0);
    CHECK(freezeBitsOf(1.0f) == 1);
    CHECK(freezeBitsOf(2.0f) == 2);
    CHECK(freezeBitsOf(3.0f) == 3);

    // 四舍五入(旧 core 侧是截断:1.9 会被解成 1 = 只冻 pan,而 output 侧解成 2 = 只冻 vol)
    CHECK(freezeBitsOf(1.9f) == 2);
    CHECK(freezeBitsOf(0.4f) == 0);
    CHECK(freezeBitsOf(2.5f) == 3);

    // 越界钳制:保守取「多冻一维」,绝不按位截高位(4 & 3 == 0 会解成两维都没冻)
    CHECK(freezeBitsOf(4.0f) == 3);
    CHECK(freezeBitsOf(99.0f) == 3);
    CHECK(freezeBitsOf(-1.0f) == 0);

    // 非有限:回落 0 = 未冻结(与「参数未接线」同款默认),且不得走 float→int 的 UB 路径
    CHECK(freezeBitsOf(std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK(freezeBitsOf(std::numeric_limits<float>::infinity()) == 3);
    CHECK(freezeBitsOf(-std::numeric_limits<float>::infinity()) == 0);

    // 维度取位:isPan → bit0,否则 bit1
    CHECK(freezeHasDim(1, /*isPan=*/true));
    CHECK_FALSE(freezeHasDim(1, /*isPan=*/false));
    CHECK(freezeHasDim(2, /*isPan=*/false));
    CHECK_FALSE(freezeHasDim(2, /*isPan=*/true));
    CHECK(freezeHasDim(3, /*isPan=*/true));
    CHECK(freezeHasDim(3, /*isPan=*/false));
    CHECK_FALSE(freezeHasDim(0, /*isPan=*/true));
    CHECK_FALSE(freezeHasDim(0, /*isPan=*/false));
}

// ---------------------------------------------------------------------------
// v5.1 P1-F:analyze "all" 的范围推导(纯函数;原先埋在 OutputEditor 私有成员里,
// 免 DAW harness 够不着,回归只能绕开被修的那一行 —— 评审 I1)。
// ---------------------------------------------------------------------------
TEST_CASE("analyzeAllRange:follow 档取已采集时间线,与播放头无关", "[output][analyze][v51]")
{
    using scvb::output::analyzeAllRange;

    // follow 档(rangeMode=0):取 [0, 已采集末端]。
    // ← 改回「取当前播放头」即红:用户 Cubase「播完回开头」时播放头是 0,范围恒空。
    const auto follow = analyzeAllRange(0, 0.0, 0.0, 12.5);
    CHECK(follow.startS == 0.0);
    CHECK(follow.endS == 12.5);
    CHECK(follow.valid());

    // follow 档 + 一帧都没采到:回空范围,由 §1.6 拒绝态作答;**不拿播放头兜底**。
    const auto empty = analyzeAllRange(0, 0.0, 0.0, 0.0);
    CHECK_FALSE(empty.valid());
    CHECK(empty.endS == 0.0);

    // follow 档下 range 字段即使有残值也不参与(档位说了算)。
    const auto stale = analyzeAllRange(0, 3.0, 9.0, 12.5);
    CHECK(stale.startS == 0.0);
    CHECK(stale.endS == 12.5);

    // 显式范围档(daw_loop / manual):照用 range,不看已采集末端。
    const auto manual = analyzeAllRange(2, 3.0, 9.0, 12.5);
    CHECK(manual.startS == 3.0);
    CHECK(manual.endS == 9.0);

    // 显式档但范围非法(startS >= endS)→ 回落到已采集时间线,而不是产出一个倒置区间。
    const auto bad = analyzeAllRange(2, 9.0, 3.0, 12.5);
    CHECK(bad.startS == 0.0);
    CHECK(bad.endS == 12.5);
}
