// SPDX-License-Identifier: GPL-3.0-or-later
// test_segment_edit_service —— T29 CRVS 变更事务(结果门控)单测(链接 JUCE UndoManager,离线 Catch2)。
// 覆盖 PR#55 缺陷3:editSegmentTransactional 失败(BadArg/NotAdjacent)不改 CRVS、不新增 undo 步、不触发
// rebuild;成功才压入事务,undo 还原 + 再 rebuild。另覆盖 observer 拒绝的「守卫前置」(见 SERVICE-4,事务层
// 不感知 observer,observer 拒绝由 OutputEditor::isReadOnly 前置,本测试只验证事务层不越权)。

#include <catch2/catch_test_macros.hpp>

#include <juce_data_structures/juce_data_structures.h>

#include <limits>
#include <vector>

#include "AnalyzeScopeMath.h"
#include "SuggestionScopeArgs.h" // [SL-256] §1.36 入参归一
#include "OutputAuthority.h" // SERVICE-12 第三支:钉住**生产装配点**真的装上了预算
#include "ParamUndo.h" // [SL-536] 参数撤销动作
#include "engine/FreezeBits.h"
#include "output/DistReadback.h" // [SL-548] SERVICE-14:接管产物交给读回判据
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
//
// [J131] / SL-180:另一维不只是「不许回默认」,而是**逐段保留原曲线** —— 此前把段表压成
// 单段、另一维从首段继承,于是拖一下音量卡箍,整条 pan 曲线被压成首段的 pan。
// SERVICE-5/6 用**多段、另一维各不相同**的段表钉这一条(单段夹具区分不出「保留曲线」与
// 「继承首段」)。
// ---------------------------------------------------------------------------

namespace
{
// 三段、pan 与 vol 都随段变化、含一个锁定的手编段 —— 「另一维曲线」的最小夹具。
std::vector<Segment> threeVaryingSegments()
{
    std::vector<Segment> segs(3);
    const float pans[3] = {-60.0f, 10.0f, 80.0f};
    const float vols[3] = {-6.0f, 2.0f, -3.5f};
    for (std::size_t i = 0; i < segs.size(); ++i)
    {
        segs[i].t0 = static_cast<std::int64_t>(i) * 48000 * 4;
        segs[i].t1 = segs[i].t0 + 48000 * 3; // 段间留 1s 空隙
        segs[i].pan = pans[i];
        segs[i].volDb = vols[i];
        segs[i].flags = makeSegmentFlags(SegmentOrigin::Auto, false);
    }
    segs[1].flags = makeSegmentFlags(SegmentOrigin::UserEdited, true); // 手编 + 锁定
    return segs;
}
} // namespace

TEST_CASE("SERVICE-5 makeManualDimSegments:写 pan 逐段保留 vol 曲线", "[segedit][service][t37][SL180]")
{
    const std::vector<Segment> existing = threeVaryingSegments();
    const std::vector<Segment> out = scvb::output::makeManualDimSegments(existing, /*isPan=*/true, 40.0f);

    REQUIRE(out.size() == existing.size()); // 段表不再被压成一段
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        CHECK(out[i].pan == 40.0f); // 被拖维度 = 常值
        CHECK(out[i].volDb == existing[i].volDb); // ← 改造前:三段都是首段的 -6
        CHECK(out[i].t0 == existing[i].t0); // 段边界不动
        CHECK(out[i].t1 == existing[i].t1);
        CHECK(scvb::state::segmentOrigin(out[i].flags) == SegmentOrigin::UserEdited);
        CHECK_FALSE(scvb::state::segmentLocked(out[i].flags)); // 与改造前那条常值段同口径:不上锁
        // [SL-548 / J162] 只给被拖的 pan 置位;夹具原本一个位都没有 ⇒ vol 位不会被顺手补上。
        CHECK((out[i].flags & scvb::state::kSegmentManualMask) == scvb::state::kSegmentManualPanBit);
    }
}

TEST_CASE("SERVICE-6 makeManualDimSegments:写 vol 逐段保留 pan 曲线", "[segedit][service][t37][SL180]")
{
    const std::vector<Segment> existing = threeVaryingSegments();
    const std::vector<Segment> out = scvb::output::makeManualDimSegments(existing, /*isPan=*/false, 3.5f);

    REQUIRE(out.size() == existing.size());
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        CHECK(out[i].volDb == 3.5f);
        CHECK(out[i].pan == existing[i].pan); // ← 改造前:三段都是首段的 -60(整条 pan 曲线被压平)
        CHECK(out[i].t0 == existing[i].t0);
        CHECK(out[i].t1 == existing[i].t1);
        CHECK(scvb::state::segmentOrigin(out[i].flags) == SegmentOrigin::UserEdited);
        CHECK_FALSE(scvb::state::segmentLocked(out[i].flags));
        // [SL-548 / J162] 只给 vol 置位 —— pan 各段值不同,但判据不再看值,只看这一位。
        CHECK((out[i].flags & scvb::state::kSegmentManualMask) == scvb::state::kSegmentManualVolBit);
    }
}

TEST_CASE("SERVICE-7 makeManualDimSegments:空表落单段全时限 + 默认 + 越界钳制", "[segedit][service][t37]")
{
    const std::vector<Segment> empty;

    const std::vector<Segment> a = scvb::output::makeManualDimSegments(empty, /*isPan=*/true, 999.0f);
    REQUIRE(a.size() == 1);
    CHECK(a[0].t0 == 0);
    CHECK(a[0].t1 == (static_cast<std::int64_t>(1) << 40)); // 无末端哨兵(§2.8 openEnded)
    CHECK(a[0].pan == 100.0f); // 钳到 +100
    CHECK(a[0].volDb == 0.0f); // 空表 → vol 默认 0dB
    // [SL-548 / J162] 空表只标被拖的那一维:另一维是默认值,不是用户固定的值。
    CHECK((a[0].flags & scvb::state::kSegmentManualMask) == scvb::state::kSegmentManualPanBit);

    const std::vector<Segment> b = scvb::output::makeManualDimSegments(empty, /*isPan=*/false, -999.0f);
    REQUIRE(b.size() == 1);
    CHECK(b[0].volDb == -24.0f); // 钳到 -24dB
    CHECK(b[0].pan == 0.0f); // 空表 → pan 默认居中
    CHECK((b[0].flags & scvb::state::kSegmentManualMask) == scvb::state::kSegmentManualVolBit);
}

TEST_CASE("SERVICE-8 makeManualDimSegments:交替写两维互不干扰(真机复现序列)", "[segedit][service][t37]")
{
    std::vector<Segment> segs; // 从空表起步

    // 先调音量 → 再调 pan → 再调音量:三步之后两维都应是最后一次写入的值。
    segs = scvb::output::makeManualDimSegments(segs, /*isPan=*/false, -8.0f);
    REQUIRE(segs.size() == 1);
    REQUIRE(segs[0].volDb == -8.0f);

    segs = scvb::output::makeManualDimSegments(segs, /*isPan=*/true, 55.0f);
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].pan == 55.0f);
    CHECK(segs[0].volDb == -8.0f); // 音量没被 pan 冲掉
    // [SL-548 / J162] 非空表保留另一维已有的位:先接管 vol、再接管 pan ⇒ 两位都在。
    CHECK((segs[0].flags & scvb::state::kSegmentManualMask) == scvb::state::kSegmentManualMask);

    segs = scvb::output::makeManualDimSegments(segs, /*isPan=*/false, 2.0f);
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].volDb == 2.0f);
    CHECK(segs[0].pan == 55.0f); // pan 没被音量冲掉
}

// ---------------------------------------------------------------------------
// [SL-548 / J162] 非空表上「另一维已有的位」是**逐段**保留的,不是整表统一补齐或统一清空。
// 夹具:整轨接管过 pan,随后第 2 段被 set_values 改过(那一段的位已被清掉)。再接管 vol ⇒
// 第 1、3 段 pan|vol,第 2 段只有 vol —— pan 维因此不再「每段都带位」,读回不再把它当手动常值。
// 删除式:把 `(seg.flags & kSegmentManualMask)` 那一项去掉 ⇒ 第 1、3 段丢 pan 位(红)。
// ---------------------------------------------------------------------------
TEST_CASE("SERVICE-13 makeManualDimSegments:另一维的手动位逐段保留", "[segedit][service][SL548]")
{
    std::vector<Segment> segs = scvb::output::makeManualDimSegments(threeVaryingSegments(), /*isPan=*/true, 30.0f);
    for (const auto& s : segs)
        REQUIRE((s.flags & scvb::state::kSegmentManualMask) == scvb::state::kSegmentManualPanBit); // 前置
    SegmentEditArgs sv;
    sv.op = SegmentEditOp::SetValues;
    sv.segIdx = 1;
    sv.hasPan = true;
    sv.pan = -12.0f;
    REQUIRE(scvb::state::editTrackSegments(segs, sv) == SegmentEditResult::Ok);
    REQUIRE((segs[1].flags & scvb::state::kSegmentManualMask) == 0u); // 前置:set_values 清了第 2 段

    const std::vector<Segment> out = scvb::output::makeManualDimSegments(segs, /*isPan=*/false, -4.0f);
    REQUIRE(out.size() == 3);
    CHECK((out[0].flags & scvb::state::kSegmentManualMask) == scvb::state::kSegmentManualMask);
    CHECK((out[1].flags & scvb::state::kSegmentManualMask) == scvb::state::kSegmentManualVolBit);
    CHECK((out[2].flags & scvb::state::kSegmentManualMask) == scvb::state::kSegmentManualMask);
    CHECK(out[1].pan == -12.0f); // 另一维的值同样逐段保留(SERVICE-6 同款,这里顺带)
}

// ---------------------------------------------------------------------------
// [SL-548 / J162] 旧工程兼容形状(单段 user_edited、整表无位 ⇒ 读回两维都算手动)上**再接管另一维**:
// 按卡上 ①「非空表保留另一维**已有**的位」的原文,旧段上没有位可保留 ⇒ 只置被拖那一维 ⇒ 表上有了位,
// 兼容规则不再适用,**原先被兼容规则判成手动的那一维随之不算手动**(输出 OFF 时它的读回改走参数面,
// 与声音同路)。#332 复审【建议】指出变更文档漏登了这一档;按原文落地、在变更文档「兼容性影响」登记,
// 这一格钉住现行行为。若改为「命中兼容形状就把两位一起置上」,这一格应当翻过来,连同变更文档那条一起改。
// ---------------------------------------------------------------------------
TEST_CASE("SERVICE-14 makeManualDimSegments:旧工程兼容形状上再接管,只置被拖那一维", "[segedit][service][SL548]")
{
    Segment legacy;
    legacy.t0 = 0;
    legacy.t1 = static_cast<std::int64_t>(1) << 40;
    legacy.pan = 0.0f;
    legacy.volDb = -8.0f;
    legacy.flags = makeSegmentFlags(SegmentOrigin::UserEdited, false); // 旧构建空表接管 vol 的产物:无位
    const std::vector<Segment> before = {legacy};
    REQUIRE(scvb::output::manualDimOf(before, /*isPan=*/false) != nullptr); // 前置:兼容规则两维都算
    REQUIRE(scvb::output::manualDimOf(before, /*isPan=*/true) != nullptr);

    const std::vector<Segment> after = scvb::output::makeManualDimSegments(before, /*isPan=*/true, 40.0f);
    REQUIRE(after.size() == 1);
    CHECK((after[0].flags & scvb::state::kSegmentManualMask) == scvb::state::kSegmentManualPanBit);
    CHECK(after[0].volDb == -8.0f); // vol 的值原样保留
    CHECK(scvb::output::manualDimOf(after, /*isPan=*/true) == &after.front());
    CHECK(scvb::output::manualDimOf(after, /*isPan=*/false) == nullptr); // 兼容规则让位于显式位
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
    CHECK(scvb::output::makeManualDimSegments(empty, /*isPan=*/true, nan).front().pan == 0.0f);
    CHECK(scvb::output::makeManualDimSegments(empty, /*isPan=*/false, nan).front().volDb == 0.0f);
    // 非空表那一支同样过这把尺子(逐段写入的值不得是 NaN)。
    std::vector<Segment> one(1);
    one[0].t1 = 48000;
    CHECK(scvb::output::makeManualDimSegments(one, /*isPan=*/false, nan).front().volDb == 0.0f);
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

// ---------------------------------------------------------------------------
// [J152] §2.7 captureProgress 的 coveragePct 分母窗口(纯函数)。
// 帧内容与接线由 tests/host/test_host_harness.cpp 的 `HOST J152` 用真 processor 钉;
// 这里只钉窗口算术的四个分支,每条都写着「改成什么就红」。
// ---------------------------------------------------------------------------
TEST_CASE("captureProgressWindow:follow 档停着取 max(播放头, 已采集末端),播放中取播放头", "[output][coverage][J152]")
{
    using scvb::output::captureProgressWindow;

    // ① follow + 停着 + 播放头在 0(重开工程的典型形态):取已采集末端。
    //    ← 去掉 max 只留播放头即红:窗口为空,已采集的覆盖一格都报不出来。
    const auto reopened = captureProgressWindow(/*playing=*/false, 0, 0.0, 0.0, /*playheadS=*/0.0, /*extentS=*/42.0);
    CHECK(reopened.startS == 0.0);
    CHECK(reopened.endS == 42.0);
    CHECK(reopened.valid());

    // ② follow + 停着 + 播放头在已采集末端之后:取播放头(max 的另一半)。
    //    ← 去掉 max 只留已采集末端即红。
    const auto pastExtent = captureProgressWindow(false, 0, 0.0, 0.0, 60.0, 42.0);
    CHECK(pastExtent.endS == 60.0);

    // ③ follow + 播放中:只取播放头(周期帧的既有口径),已采集末端不参与。
    //    ← 播放中也取 max 即红:这里 42 > 10。
    const auto playing = captureProgressWindow(true, 0, 0.0, 0.0, 10.0, 42.0);
    CHECK(playing.startS == 0.0);
    CHECK(playing.endS == 10.0);

    // ④ follow + 停着 + 从未采集 + 播放头在 0:窗口为空(例外帧由调用方按 0% 照发)。
    const auto fresh = captureProgressWindow(false, 0, 0.0, 0.0, 0.0, 0.0);
    CHECK_FALSE(fresh.valid());

    // ⑤ 范围档:照用 global.range,走带与已采集末端都不参与。
    const auto manualStopped = captureProgressWindow(false, 2, 3.0, 9.0, 0.0, 42.0);
    CHECK(manualStopped.startS == 3.0);
    CHECK(manualStopped.endS == 9.0);
    const auto manualPlaying = captureProgressWindow(true, 1, 3.0, 9.0, 5.0, 42.0);
    CHECK(manualPlaying.startS == 3.0);
    CHECK(manualPlaying.endS == 9.0);

    // ⑥ 播放头负值(宿主没给 timeInSamples 时调用方传 0;这里防御负数)夹到 0。
    const auto negative = captureProgressWindow(false, 0, 0.0, 0.0, -5.0, 0.0);
    CHECK_FALSE(negative.valid());
    CHECK(negative.endS == 0.0);
}

// ---------------------------------------------------------------------------
// v5.4 SL-190:对象形 scope 的 startS/endS 缺省口径(§1.6 里这两个字段带 `?`)。
//
// 现场:Tab2 解冻提示条的「重新识别(含手动段)」发的是
// `analyze({tracksMask:1<<(ch-1)}, {clearManual:true})` —— 只给轨掩码、不给范围。
// 真桥 parseAnalyzeScope 当时把两个字段都 `getProperty(...,0.0)` 兜底成 0,于是范围 = [0,0],
// startAnalysis 在 `!(endS > startS)` 处当场回 {ok:false},一段都不重算 ——「点了没什么作用」。
// mock 桥对同一形状取的是 ±∞,web smoke 因此从来没报过 —— 这条用例钉的就是真桥这一侧。
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// [SL-279 复审第 5 轮] `applied.*`(§1.21「上次分析所用口径」)前移的判据本身。
//
// 为什么钉在这里:判据只此一处 —— `AnalyzeRange::wholeTimeline`,由范围推导在**选分支**时
// 置,两个调用方(`parseAnalyzeScope` 的 "all" 档、`tickResegmentDebounce`)读它。判据在
// 纯函数里,scvb_tests 够得着;而两个调用方一个埋在私有成员里、一个要跑完整条流水线。
//
// 它守的是一条**产品语义**:`daw_loop`/`manual` 档下点「分析(全部)」只重算 `global.range`,
// 范围外的段仍是旧口径 —— 前移基线会把「需重新分析」注记灭掉,那是漏报。
// ---------------------------------------------------------------------------
TEST_CASE("analyzeAllRange:wholeTimeline 只在真的覆盖整条时间线时为真", "[output][analyze][sl279]")
{
    using scvb::output::analyzeAllRange;
    using scvb::output::analyzeScopeRange;

    // ① follow 档:范围 = [0, 已采集末端] ⇒ 整条,前移。
    CHECK(analyzeAllRange(0, 0.0, 0.0, 12.5).wholeTimeline);
    // ② follow 档下 range 字段有残值也不看(§1.8:follow 忽略起止)⇒ 仍是整条。
    //    ← 若把判据写成「rangeEndS > rangeStartS 就不算整条」(漏了 rangeMode 那一半),这格红。
    CHECK(analyzeAllRange(0, 3.0, 9.0, 12.5).wholeTimeline);

    // ③ manual 档 + 有效范围:只重算 [3,9) ⇒ **不**前移。
    //    ← 这格就是本轮裁定的执行者:去掉 `rangeMode != 0` 那个条件、恒置 true,只红这里。
    CHECK_FALSE(analyzeAllRange(2, 3.0, 9.0, 12.5).wholeTimeline);
    // ④ daw_loop 档同理(非 0 即「有显式范围」)。
    CHECK_FALSE(analyzeAllRange(1, 1.0, 4.0, 12.5).wholeTimeline);

    // ⑤ 显式档但范围**无效**(倒挂):推导落回 [0, 已采集末端] 那条分支 ⇒ 算整条。
    //    钉的是「wholeTimeline 跟着**实际走的分支**,不是跟着档位字面」——两者在这里分叉。
    const auto bad = analyzeAllRange(2, 9.0, 3.0, 12.5);
    CHECK(bad.startS == 0.0);
    CHECK(bad.endS == 12.5);
    CHECK(bad.wholeTimeline);

    // ⑥ 一帧没采到:范围空、分析会被 §1.6 拒掉;判据仍按分支答(拒了就没人读它)。
    CHECK(analyzeAllRange(0, 0.0, 0.0, 0.0).wholeTimeline);

    // ⑦ 对象形 scope **一律**为假 —— 它必然指名了轨(tracksMask != 0 的守卫),轨维就不全。
    //    ← 删掉 analyzeScopeRange 末尾那句 `r.wholeTimeline = false;` 只红这两格:
    //      它借的是 analyzeAllRange 的返回值,follow 档下会把 true 一路带出去。
    constexpr unsigned int kOneTrack = 1u << 2;
    CHECK_FALSE(analyzeScopeRange(kOneTrack, false, 0.0, false, 0.0, 0, 0.0, 0.0, 12.5).wholeTimeline);
    CHECK_FALSE(analyzeScopeRange(kOneTrack, true, 1.0, true, 5.0, 0, 0.0, 0.0, 12.5).wholeTimeline);
}

TEST_CASE("analyzeScopeRange:对象形 scope 缺省范围 = \"all\" 同款推导,不是 [0,0]", "[output][analyze][v54]")
{
    using scvb::output::analyzeAllRange;
    using scvb::output::analyzeScopeRange;

    constexpr unsigned int kOneTrack = 1u << 2; // 指名第 3 轨(Tab2 单轨重新识别的形状)

    // ① 两个字段都缺:follow 档 → [0, 已采集末端],**范围必须有效**。
    //    ← 把实现改回 `hasStartS=false 时用 0.0` 即红:endS 变 0,valid() 失败,分析照旧被拒。
    const auto omitted = analyzeScopeRange(kOneTrack, false, 0.0, false, 0.0, 0, 0.0, 0.0, 12.5);
    CHECK(omitted.startS == 0.0);
    CHECK(omitted.endS == 12.5);
    CHECK(omitted.valid());
    // 与 "all" 逐字同款 —— 缺省口径就是「未指定」,不是「另一套规则」。
    const auto all = analyzeAllRange(0, 0.0, 0.0, 12.5);
    CHECK(omitted.startS == all.startS);
    CHECK(omitted.endS == all.endS);

    // ② 两个字段都缺 + 显式范围档:跟着 range 走(用户设了范围就尊重它)。
    const auto omittedManual = analyzeScopeRange(kOneTrack, false, 0.0, false, 0.0, 2, 3.0, 9.0, 12.5);
    CHECK(omittedManual.startS == 3.0);
    CHECK(omittedManual.endS == 9.0);

    // ③ 两个字段都在(Tab3 工具条那条链路):逐字照用,**修复前后完全一致**。
    //    这条是反向验证的另一半:若实现改成「一律走 all 推导」,这里立刻红。
    const auto explicitBoth = analyzeScopeRange(kOneTrack, true, 4.0, true, 7.0, 0, 0.0, 0.0, 12.5);
    CHECK(explicitBoth.startS == 4.0);
    CHECK(explicitBoth.endS == 7.0);

    // ④ 显式给的 [0,0] 仍然照用(调用方明确要空范围就是空范围,不替它兜底)。
    const auto explicitEmpty = analyzeScopeRange(kOneTrack, true, 0.0, true, 0.0, 0, 0.0, 0.0, 12.5);
    CHECK_FALSE(explicitEmpty.valid());

    // ⑤ 只给一头:给了的照用,没给的取 all 档同侧端点。
    const auto onlyStart = analyzeScopeRange(kOneTrack, true, 5.0, false, 0.0, 0, 0.0, 0.0, 12.5);
    CHECK(onlyStart.startS == 5.0);
    CHECK(onlyStart.endS == 12.5);
    const auto onlyEnd = analyzeScopeRange(kOneTrack, false, 0.0, true, 5.0, 0, 0.0, 0.0, 12.5);
    CHECK(onlyEnd.startS == 0.0);
    CHECK(onlyEnd.endS == 5.0);

    // ⑥ 缺省 + 一帧都没采到:仍然回空范围(由 §1.6 拒绝态作答),不凭空造一个区间。
    const auto nothingCaptured = analyzeScopeRange(kOneTrack, false, 0.0, false, 0.0, 0, 0.0, 0.0, 0.0);
    CHECK_FALSE(nothingCaptured.valid());

    // ⑦ 【PR #112 评审重要】既不指名轨、又不给范围 → **仍然判空**。
    //    tracksMask=0 在 processor 侧是「不限轨」,放开缺省范围就成了「全 15 轨 × 全时间线」,
    //    配上 clearManual:true 是不可撤销的 origin 全清。要全轨全时间线请显式走 "all"。
    //    ← 把 tracksMask==0 那道守卫删掉即红。
    const auto noMaskNoRange = analyzeScopeRange(0, false, 0.0, false, 0.0, 0, 0.0, 0.0, 12.5);
    CHECK_FALSE(noMaskNoRange.valid());
    const auto noMaskNoRangeManual = analyzeScopeRange(0, false, 0.0, false, 0.0, 2, 3.0, 9.0, 12.5);
    CHECK_FALSE(noMaskNoRangeManual.valid());

    // ⑦b 不指名轨 + 只给一头 → 也判空(安全方向):否则 {tracksMask:0, startS:5} 就成了
    //     「全轨、5s 到时间线末端」的 origin 全清,与守卫用意正相反。
    const auto noMaskOnlyStart = analyzeScopeRange(0, true, 5.0, false, 0.0, 0, 0.0, 0.0, 12.5);
    CHECK_FALSE(noMaskOnlyStart.valid());

    // ⑧ 但「不限轨 + 显式范围」是修复前就成立的形状,必须逐字保留(守卫不许连它一起挡)。
    //    ← 把守卫写成「tracksMask==0 一律判空」即红。
    const auto noMaskExplicitRange = analyzeScopeRange(0, true, 3.0, true, 9.0, 0, 0.0, 0.0, 12.5);
    CHECK(noMaskExplicitRange.startS == 3.0);
    CHECK(noMaskExplicitRange.endS == 9.0);
    CHECK(noMaskExplicitRange.valid());
}

// ---------------------------------------------------------------------------
// [SL-209 复审 S1] 撤销栈容量记账:getSizeInUnits 必须随**段数**增长,否则
// juce::UndoManager 的 30000 units 上限等价于「3 万条事务」—— 而每条事务持有两份整个
// CrvsData 快照(2 版本 × 15 轨全部段),分析回落一次就可能几万段,封顶一次都不会触发。
// ---------------------------------------------------------------------------
TEST_CASE("SERVICE-11 撤销事务的 units 随段数增长(封顶才起得了作用)", "[segedit][service][SL209]")
{
    const auto unitsFor = [](std::size_t segCount) {
        CrvsData before;
        CrvsData after;
        auto& segs = after.versions[0].tracks[0].segments;
        for (std::size_t i = 0; i < segCount; ++i)
        {
            Segment s;
            s.t0 = static_cast<std::int64_t>(i) * 100;
            s.t1 = s.t0 + 100;
            s.flags = makeSegmentFlags(scvb::state::SegmentOrigin::Auto, false);
            segs.push_back(s);
        }
        CrvsData live = before;
        juce::UndoManager undo;
        scvb::output::commitCrvsTransaction(undo, live, "T", [&] { live = after; }, [] {});
        // 事务已 perform;直接构造一个同样内容的 action 取其 units(commitCrvsTransaction
        // 内部 new 出来的那个已交给 UndoManager 持有,拿不到指针)。
        scvb::output::CrvsTransactionAction probe(live, before, after, [] {});
        return probe.getSizeInUnits();
    };

    const int small = unitsFor(0);
    const int mid = unitsFor(100);
    const int big = unitsFor(10000);

    CHECK(small >= 1); // 空快照也得占 1(0 会让它在容量账上「不存在」)
    CHECK(mid > small); // ★ 随段数增长 —— 恒 1 的旧写法在这里就红了
    CHECK(big > mid);
    // 量级合理:1 万段两份快照应当远超 30000 units 的一个零头,让封顶真的够得着。
    CHECK(big > 30000);
    // 口径逐字节:1 万段 × 32B(只在 after 一侧)—— 记错成「近似」会在这里露馅。
    CHECK(big == static_cast<int>(10000 * sizeof(Segment)));
}

// ---------------------------------------------------------------------------
// [#152 复审【重要】①②] 大段数工程下的**撤销深度**。
//
// 复审提出的失效路径是「单条超限事务会被丢 / 整个历史被清空 ⇒ 分析撤销静默失效」。逐行核对
// JUCE 8.0.8 `UndoManager::dropOldTransactionsIfTooLarge` 后:裁剪循环的三个条件是**与**,
// 且 `transactions.size() > minimumTransactionsToKeep` 优先于 units 上限 —— **单条超限事务
// 永远不会被丢**,所以「一次分析 = 一条撤销步」不会失效。真正的后果是**深度**:默认参数
// (30000, 30) 下任何真实工程的单条事务都吃光预算,深度恒 = 30。
//
// 本例钉的就是这一条:2000 段的工程连压 64 条事务,64 次撤销必须全成功。
// ★ 反向验证:把 `configureCrvsUndoBudget(undo)` 那行删掉(= 退回 juce 默认预算),
//   第 31 次起 undo() 就开始返回 false,本例立刻红。
// 同时钉住封顶**确实咬合**(不是把预算调大到形同虚设):连压 700 条后深度必须被裁到 700 以下。
// ---------------------------------------------------------------------------
TEST_CASE("SERVICE-12 大段数工程(2000 段)撤销深度:预算够用 + 封顶真咬合", "[segedit][service][SL209]")
{
    constexpr std::size_t kSegs = 2000; // ≥2000 段:两份快照 = 128,000 units,远超默认 30000

    CrvsData table;
    {
        auto& segs = table.versions[0].tracks[0].segments;
        segs.reserve(kSegs);
        for (std::size_t i = 0; i < kSegs; ++i)
        {
            Segment sg;
            sg.t0 = static_cast<std::int64_t>(i) * 100;
            sg.t1 = sg.t0 + 100;
            sg.flags = makeSegmentFlags(SegmentOrigin::Auto, false);
            segs.push_back(sg);
        }
    }

    // 一条事务的 units:(旧 + 新)× 32B。稳态下新旧同量级,这里旧表已含 kSegs 段。
    const int perTxn = [&] {
        scvb::output::CrvsTransactionAction probe(table, table, table, [] {});
        return probe.getSizeInUnits();
    }();
    REQUIRE(perTxn == static_cast<int>(2 * kSegs * sizeof(Segment))); // = 128,000
    REQUIRE(perTxn > 30000); // 单条就吃光 juce 默认预算 —— 这正是复审说的那个前提

    // 深度 = 预算 / 单条开销(向下取整),这里 ≈ 67,108,864 / 128,000 ≈ 524 步。
    const int expectedDepth = scvb::output::kCrvsUndoBudgetBytes / perTxn;
    REQUIRE(expectedDepth > 64); // 推导自洽:64 MiB 在这一档上买得起远超 64 步

    const auto pushN = [&](juce::UndoManager& undo, CrvsData& live, int n) {
        for (int k = 0; k < n; ++k)
        {
            CrvsData next = live;
            next.versions[0].tracks[0].segments[0].pan = static_cast<float>(k % 100);
            scvb::output::commitCrvsTransaction(undo, live, "T", [&] { live = next; }, [] {});
        }
    };
    const auto countUndos = [](juce::UndoManager& undo) {
        int n = 0;
        while (undo.undo())
            ++n;
        return n;
    };

    SECTION("64 步撤销全成立(默认预算下第 31 步起就撤不动)")
    {
        CrvsData live = table;
        juce::UndoManager undo;
        scvb::output::configureCrvsUndoBudget(undo); // ★ 删掉这行 = 反向验证,本 SECTION 转红
        pushN(undo, live, 64);
        CHECK(countUndos(undo) == 64);
    }

    SECTION("生产装配点:OutputAuthority 出厂即带预算(不是只有 helper 自己带)")
    {
        // ★ 这一支钉的是**接线**:上面两支只证明 configureCrvsUndoBudget 有效,
        //   若 OutputAuthority 的构造忘了调它,生产侧照样是 juce 默认值。
        //   反向验证:删掉 OutputAuthority::OutputAuthority() 里那行 → 这里退回 30 → 红。
        CrvsData live = table;
        scvb::output::OutputAuthority authority;
        pushN(authority.undoManager(), live, 64);
        CHECK(countUndos(authority.undoManager()) == 64);
    }

    SECTION("封顶咬合:连压 700 条后深度被裁,且不低于硬地板")
    {
        CrvsData live = table;
        juce::UndoManager undo;
        scvb::output::configureCrvsUndoBudget(undo);
        pushN(undo, live, 700);
        const int deep = countUndos(undo);
        CHECK(deep < 700); // 预算真的裁了 —— 否则就是「调大到形同虚设」
        CHECK(deep >= scvb::output::kCrvsUndoMinTransactions); // JUCE 的 min 硬地板
        CHECK(deep >= 64); // 且远高于默认预算给的 30
    }
}

// ---------------------------------------------------------------------------
// [SL-399] analysisWindows —— 一次分析**两个**窗:计算窗 [0, extent)、写回窗 = scope 窗。
//
// 定谳(用户 A22):冻结前显示 pan −20 / vol −0.2,手动改之后「恢复自动」变成 −60 / −0.9。
// 根因是**计算窗只有 scope 那一段**:`AnalysisPipeline` 在一次 run 内跨区间携带上一区间的解
// (wCont / wSide),而首区间的锚是 `cfg.tracks[t].currentPan`(用户刚手动改过的值)——
// 于是单段重算解出来的槽位/侧别与全量分析不同。裁定 b″:计算窗 = 整条已采集时间线,
// 写回窗 = scope × 目标轨。
//
// 拒绝判据**只认写回窗**:scope 完全落在已采集时间线之外 ⇒ 写集为空 ⇒ §1.6 拒绝态。
// 拿计算窗判这一条是错的 —— 计算窗恒为整条时间线,拿它判等于永不拒绝。
//
// 反向验证:把 `analysisWindows()` 的 compute 改回 `analyzeHopWindow(startS, endS, hopS)`
// (两窗合一)⇒「计算窗恒为整条时间线」两格红;把 rejects 改成只看 apply.valid()
// ⇒「scope 落在时间线之外 ⇒ 拒绝」一格红。
// ---------------------------------------------------------------------------
TEST_CASE("analysisWindows:计算窗恒为整条时间线,写回窗 = scope;落在时间线外 ⇒ 拒绝", "[output][analyze][SL399]")
{
    using scvb::output::analysisWindows;

    SECTION("计算窗恒 [0, extent),与 scope 无关")
    {
        const auto inScope = analysisWindows(4.0, 8.0, 30.0, 0.01);
        CHECK(inScope.compute.firstHop == 0u);
        CHECK(inScope.compute.lastHop == 3000u); // 30s / 10ms
        CHECK(inScope.apply.firstHop == 400u); // 4.0s / 10ms(向内取整)
        CHECK(inScope.apply.lastHop == 800u);
        CHECK_FALSE(inScope.rejects);

        // scope 换到尾巴上,计算窗一个 hop 都不动 —— 这正是「恢复自动 == 全量分析对该段的值」
        // 那条构造性保证的前提。
        const auto tailScope = analysisWindows(25.0, 30.0, 30.0, 0.01);
        CHECK(tailScope.compute.firstHop == inScope.compute.firstHop);
        CHECK(tailScope.compute.lastHop == inScope.compute.lastHop);
        CHECK(tailScope.apply.firstHop == 2500u);
        CHECK(tailScope.apply.lastHop == 3000u);
    }

    SECTION("两窗重合:整条时间线的 scope ⇒ apply == compute")
    {
        const auto whole = analysisWindows(0.0, 30.0, 30.0, 0.01);
        CHECK(whole.apply.firstHop == whole.compute.firstHop);
        CHECK(whole.apply.lastHop == whole.compute.lastHop);
        CHECK_FALSE(whole.rejects);
    }

    SECTION("scope 落在已采集时间线之外 ⇒ rejects(写集为空)")
    {
        // 只采了 30s,却要重算 [100s, 200s):窗自身合法(analyzeHopWindow 会给出非空窗),
        // 但与时间线**无交** ⇒ 写集为空 ⇒ §1.6 拒绝态。
        const auto outside = analysisWindows(100.0, 200.0, 30.0, 0.01);
        CHECK(outside.apply.valid()); // 窗本身是合法的 —— 拒绝不是因为窗坏了
        CHECK(outside.rejects);

        // 贴着右边界之外(起点 == 时间线末端)同样是空交集:半开区间 [30s, 40s) ∩ [0, 30s) = ∅。
        CHECK(analysisWindows(30.0, 40.0, 30.0, 0.01).rejects);
        // 左边界之外只差一点也拒。
        CHECK(analysisWindows(31.0, 32.0, 30.0, 0.01).rejects);
    }

    SECTION("部分相交仍受理(窗会被后续写回面各自裁)")
    {
        const auto partly = analysisWindows(20.0, 45.0, 30.0, 0.01);
        CHECK_FALSE(partly.rejects);
        CHECK(partly.apply.lastHop == 4500u); // 写回窗按 scope 给,不预先夹到 extent
    }

    SECTION("一帧都没采到 / 空 scope ⇒ 一律拒")
    {
        CHECK(analysisWindows(0.0, 5.0, 0.0, 0.01).rejects); // extent = 0
        CHECK(analysisWindows(5.0, 5.0, 30.0, 0.01).rejects); // endS == startS
        CHECK(analysisWindows(0.0, 5.0, 30.0, 0.0).rejects); // hopS = 0(未 prepare)
    }
}

// ---------------------------------------------------------------------------
// [SL-393] inWriteMask —— `tracksMask` 降级成**写回掩码**之后的唯一判据点。
//
// 计算集(参与指派的轨)现在比写回集宽:只喂一条轨会让引擎判成「独唱」而按到正中
// (AutoAssign.cpp:200-208 / tests/core/test_assign.cpp:120),那正是 SL-393 的病根。
// 于是「哪些轨的段表可以被改写」不再等于「哪些轨参与了计算」,需要一条独立判据。
//
// `mask == 0` 必须是**全轨**(§1.6 的 "all" 与不带 mask 的对象形都落到这里)。这一位
// 判反的方向是**静默不写**:分析跑完、回执 ok、段表纹丝不动,屏幕上与「点了没反应」
// 一模一样。
//
// 反向验证:把 `tracksMask == 0` 那一支删掉(回落成逐位判),SECTION「0 = 全轨」必红。
// ---------------------------------------------------------------------------
TEST_CASE("inWriteMask:0 = 全轨,其余按位;越界一律假", "[output][analyze][SL393]")
{
    using scvb::output::inWriteMask;

    SECTION("0 = 全轨")
    {
        for (int t = 0; t < 15; ++t)
        {
            CHECK(inWriteMask(0, t));
        }
    }

    SECTION("单轨掩码只放行那一位")
    {
        const std::uint16_t only3 = static_cast<std::uint16_t>(1u << 2); // 第 3 轨
        for (int t = 0; t < 15; ++t)
        {
            CHECK(inWriteMask(only3, t) == (t == 2));
        }
    }

    SECTION("多位掩码逐位放行")
    {
        const std::uint16_t m = static_cast<std::uint16_t>((1u << 0) | (1u << 14));
        CHECK(inWriteMask(m, 0));
        CHECK(inWriteMask(m, 14));
        CHECK_FALSE(inWriteMask(m, 1));
        CHECK_FALSE(inWriteMask(m, 13));
    }

    SECTION("越界下标一律假(含 mask=0 —— 全轨也只有 15 条)")
    {
        CHECK_FALSE(inWriteMask(0, -1));
        CHECK_FALSE(inWriteMask(0, 15));
        CHECK_FALSE(inWriteMask(0xFFFFu, 15));
    }
}

// ---------------------------------------------------------------------------
// [SL-242] analyzeHopWindow —— 范围 → hop 窗必须**向内取整**。
//
// 靶子:`firstHop` 修复前是截断(向 0 取整),于是非 hop 对齐的范围起点会把
// `cfg.rangeStartSample` 推到 startS **之前**最多一个 hop。那个数正是
// applyAnalysisSegments 判 `outsideRange` 用的门槛 —— 于是紧贴范围左边、
// `t1 == startS` 的那一段被判成「与范围相交」而删掉,而本次产出只覆盖
// [rangeStart, rangeEnd),补不回它 ⇒ 一整段凭空消失(SL-242 的 native 侧成因)。
//
// 反向验证:把 `analyzeHopWindow` 的 `std::ceil(... - kHopEps)` 改回
// `std::floor(...)`(= 截断),SECTION「非对齐起点」必红。
// ---------------------------------------------------------------------------
TEST_CASE("analyzeHopWindow:范围向内取整,不把窗撑到范围之外", "[output][analyze][SL242]")
{
    using scvb::output::analyzeHopWindow;
    constexpr double kHopS = 0.010; // kFeatHopMs = 10

    SECTION("hop 对齐的范围:逐字照用,一个 hop 都不多不少")
    {
        const auto w = analyzeHopWindow(1.00, 2.00, kHopS);
        REQUIRE(w.valid());
        CHECK(w.firstHop == 100u);
        CHECK(w.lastHop == 200u);
    }

    SECTION("非对齐起点:窗的左沿**不许**落到 startS 之前")
    {
        // 1.234s = 123.4 hop。修复前截断成 123 ⇒ 窗从 1.230s 起,把 [.., 1.234) 那一段
        // 一并卷进「范围内」;向内取整取 124 ⇒ 窗从 1.240s 起,左邻段安全。
        const auto w = analyzeHopWindow(1.234, 2.0, kHopS);
        REQUIRE(w.valid());
        CHECK(w.firstHop == 124u);
        CHECK(static_cast<double>(w.firstHop) * kHopS >= 1.234);
        CHECK(w.lastHop == 200u);
    }

    SECTION("非对齐终点:窗的右沿**不许**落到 endS 之后")
    {
        const auto w = analyzeHopWindow(1.0, 2.005, kHopS);
        REQUIRE(w.valid());
        CHECK(w.lastHop == 200u);
        CHECK(static_cast<double>(w.lastHop) * kHopS <= 2.005);
    }

    SECTION("浮点残差不许吃掉首尾各一个 hop")
    {
        // 「样本 ÷ 采样率」链上算出来的秒值:48000 样本 @48k = 1.0s,但 4.8e-1/1e-2
        // 这类除法在别的路径上会给出 99.99999999997 / 100.00000000003。
        const auto low = analyzeHopWindow(1.0 - 1e-13, 2.0 - 1e-13, kHopS);
        REQUIRE(low.valid());
        CHECK(low.firstHop == 100u);
        CHECK(low.lastHop == 200u);
        const auto high = analyzeHopWindow(1.0 + 1e-13, 2.0 + 1e-13, kHopS);
        REQUIRE(high.valid());
        CHECK(high.firstHop == 100u);
        CHECK(high.lastHop == 200u);
    }

    SECTION("窄于一个 hop / 非法范围:空窗 ⇒ 调用方回 §1.6 拒绝态")
    {
        CHECK_FALSE(analyzeHopWindow(1.2341, 1.2349, kHopS).valid()); // 0.8ms
        CHECK_FALSE(analyzeHopWindow(2.0, 1.0, kHopS).valid()); // 倒序
        CHECK_FALSE(analyzeHopWindow(1.0, 1.0, kHopS).valid()); // 空区间
        CHECK_FALSE(analyzeHopWindow(1.0, 2.0, 0.0).valid()); // hopS 非法
    }

    SECTION("巨大/非有限 endS 一律判空窗,不留 double→uint64 越界与下游溢出")
    {
        // [#161 复审二轮] 上一轮夹在 9e15 等于没夹:那个数根本活不到被用的时候 ——
        // `f.kwMs.assign(numHops)` 要 36 PB,而 `lastHop * hopSamples` 在 sr=192k 下
        // 9e15 × 1920 ≈ 1.73e19 已经溢出 int64。现在按「对下游有意义的量级」判空窗。
        //
        // **够得着的是「有限但巨大」,不是 Infinity**:`JSON.stringify(Infinity)` = "null",
        // 桥面 `givenNumber()` 判假,Infinity 过不了桥(上一轮那个 SECTION 测的恰好是
        // 唯一到不了的那种输入)。这里两种都测,但把可达的那一种放在第一条。
        CHECK_FALSE(analyzeHopWindow(0.0, 1.0e12, kHopS).valid()); // ≈3 万年,JSON 传得动
        CHECK_FALSE(analyzeHopWindow(0.0, std::numeric_limits<double>::infinity(), kHopS).valid());
        CHECK_FALSE(analyzeHopWindow(0.0, std::numeric_limits<double>::quiet_NaN(), kHopS).valid());
        CHECK_FALSE(analyzeHopWindow(std::numeric_limits<double>::quiet_NaN(), 1.0, kHopS).valid());
        // 起点巨大、窗却很窄:窗宽合法但 hop **下标**本身会让下游乘法溢出,同样拒掉。
        CHECK_FALSE(analyzeHopWindow(1.0e12, 1.0e12 + 1.0, kHopS).valid());

        // 边界两侧:恰好到上限收,越过一个 hop 就拒 —— 证明这道闸不是「一律拒大数」。
        // [SL-399 R4(b)] 上限改成读**那个常量**(不再是本行自己的 `1.0e7` 字面量):
        // `startAnalysis` 现在也为同一道分配上限复核 `kMaxHop`,两处各写一份就是第二把尺子。
        constexpr double kMaxHopS = scvb::output::kMaxHop * 0.010; // kMaxHop × hop = 1e5 s ≈ 27.8 小时
        const auto atLimit = analyzeHopWindow(0.0, kMaxHopS, kHopS);
        REQUIRE(atLimit.valid());
        CHECK(atLimit.lastHop == 10000000u);
        CHECK_FALSE(analyzeHopWindow(0.0, kMaxHopS + 0.010, kHopS).valid());
    }

    SECTION("负起点按 0 夹取,不产生回绕的巨大 hop 下标")
    {
        const auto w = analyzeHopWindow(-5.0, 1.0, kHopS);
        REQUIRE(w.valid());
        CHECK(w.firstHop == 0u);
        CHECK(w.lastHop == 100u);
    }
}

// ---------------------------------------------------------------------------
// [SL-256] parseSuggestionScope —— §1.36 `exportSuggestions(scope)` 的入参归一。
//
// 抽成纯函数的理由与同目录 `analyzeHopWindow` / `analyzeScopeRange` 一脉相承:它埋在
// `OutputEditor` 的私有成员里就够不着(那个类要 WebView 才构造得起来,离线 harness 编不进
// 那个 TU),于是这一层只能靠源码正则去看 —— 改回去照样绿。
//
// 反向验证(三条均实跑过):给 `parseSuggestionScope` **加回**「掩完为 0 ⇒ badArg」
// 那条守卫(#163 一轮已按 §1.36 拒绝态行删掉它),「tracksMask」一档必红;
// 把负左端的 `std::max(0.0, startS)` 去掉,「负左端必须归一」一档必红;
// 把未知 `versions` 的 badArg 改成「回落 active」,对应 SECTION 必红。
// ---------------------------------------------------------------------------
TEST_CASE("parseSuggestionScope:§1.36 入参归一与拒绝态", "[output][suggest][SL256]")
{
    using scvb::output::parseSuggestionScope;
    constexpr int kActive = 2;

    SECTION("整体可省 = 全默认(active + 全 15 轨 + 不限时间窗)")
    {
        const auto r = parseSuggestionScope(false, "", false, 0, false, 0.0, false, 0.0, kActive);
        REQUIRE_FALSE(r.badArg);
        CHECK_FALSE(r.scope.allVersions);
        CHECK(r.scope.activeVersion == kActive);
        CHECK(r.scope.tracksMask == 0x7FFFu);
        CHECK(r.scope.startSec < 0.0); // < 0 = 不限(SuggestionExport::Scope 的约定)
        CHECK(r.scope.endSec < 0.0);
    }

    SECTION("versions 三态:active / all / 未知值拒收")
    {
        CHECK_FALSE(parseSuggestionScope(true, "active", false, 0, false, 0, false, 0, kActive).scope.allVersions);
        CHECK(parseSuggestionScope(true, "all", false, 0, false, 0, false, 0, kActive).scope.allVersions);
        // 未知枚举**不宽容回落** —— 静默导出一份不是用户要的范围,比报错更糟。
        CHECK(parseSuggestionScope(true, "ALL", false, 0, false, 0, false, 0, kActive).badArg);
        CHECK(parseSuggestionScope(true, "", false, 0, false, 0, false, 0, kActive).badArg);
    }

    SECTION("tracksMask:bit15 掩掉;掩完为 0 **不拒**,留给 noData")
    {
        const auto one = parseSuggestionScope(false, "", true, 0x0001, false, 0, false, 0, kActive);
        REQUIRE_FALSE(one.badArg);
        CHECK(one.scope.tracksMask == 0x0001u);
        // 给了 bit15 也要被掩掉,不能带进 scope
        const auto withReserved = parseSuggestionScope(false, "", true, 0x8001, false, 0, false, 0, kActive);
        REQUIRE_FALSE(withReserved.badArg);
        CHECK(withReserved.scope.tracksMask == 0x0001u);
        // [#163 复审【红旗】] 掩完为 0 **不是 badArg**:§1.36 拒绝态只有
        // 「versions 不在两值内 / endS ≤ startS」两条,零轨落在「scope 内零段」⇒ noData。
        // mock 与冒烟(「零轨 → noData」)都钉死这一档;曾照 §1.23 [J87] 抄成 badArg 是错的
        // —— 那是写函数的口径,导出是只读的。
        for (const int m : {0x8000, 0})
        {
            const auto zero = parseSuggestionScope(false, "", true, m, false, 0, false, 0, kActive);
            CHECK_FALSE(zero.badArg);
            CHECK(zero.scope.tracksMask == 0u); // 交给 buildRows 得零行 ⇒ noData
        }
    }

    SECTION("时间窗:两头都给逐字照用;倒序/零宽 ⇒ badArg")
    {
        const auto win = parseSuggestionScope(false, "", false, 0, true, 1.5, true, 4.0, kActive);
        REQUIRE_FALSE(win.badArg);
        CHECK(win.scope.startSec == 1.5);
        CHECK(win.scope.endSec == 4.0);
        // §1.36 拒绝态第二条逐字:endS ≤ startS(mock 亦然:`if (!(endS > startS)) return BAD_ARG()`)
        CHECK(parseSuggestionScope(false, "", false, 0, true, 4.0, true, 1.5, kActive).badArg);
        CHECK(parseSuggestionScope(false, "", false, 0, true, 2.0, true, 2.0, kActive).badArg);
    }

    SECTION("[#163 复审【重要】] 只给一头 = **半开窗**,与 mock 的 ±∞ 同口径")
    {
        // 老写法把单边窗退化成「整个不限」,而 mock 用 `isFiniteNumber(x) ? x : ±Infinity`
        // ⇒ `{startS:30}` 在预览页只导 30s 之后的段、在真宿主导全部。同一次点击两侧行集不同,
        // 而这条路没有任何回显能让用户发现。`{startS:30}` 的字面意思就是「从 30 秒起」。
        const auto onlyStart = parseSuggestionScope(false, "", false, 0, true, 30.0, false, 0.0, kActive);
        REQUIRE_FALSE(onlyStart.badArg);
        CHECK(onlyStart.scope.startSec == 30.0);
        CHECK(onlyStart.scope.endSec > 1.0e300); // ≡ +∞:`t0Sec < endSec` 恒真
        // 缺左端取 0.0(段时间恒 ≥ 0,真实段上与 −∞ 等效)
        const auto onlyEnd = parseSuggestionScope(false, "", false, 0, false, 0.0, true, 30.0, kActive);
        REQUIRE_FALSE(onlyEnd.badArg);
        CHECK(onlyEnd.scope.startSec == 0.0);
        CHECK(onlyEnd.scope.endSec == 30.0);
        // 只给右端且 ≤ 0:窗内不可能有段。不拒(§1.36 没这一条),但也不能退回「不限」
        // —— 那会把「只要 0 秒之前的段」变成「导出全部」。退成空窗 ⇒ 零行 ⇒ noData。
        // ⚠ 这一档不能写成 `0/0`:`inWindow` 在 `!(endSec > startSec)` 时**关掉筛选**,
        // `0/0` 反而是「全导」—— 与本意正相反(本轮自查到的)。取远端空窗,让筛选真的生效。
        const auto endZero = parseSuggestionScope(false, "", false, 0, false, 0.0, true, 0.0, kActive);
        REQUIRE_FALSE(endZero.badArg);
        CHECK(endZero.scope.startSec > 1.0e300); // 真实段的 t1Sec 够不到 ⇒ 零行 ⇒ noData
        CHECK(endZero.scope.endSec > endZero.scope.startSec); // 筛选**必须**生效,不能退成「不限」
    }

    SECTION("[#163 二轮【重要】] 负左端必须归一 —— 否则 inWindow 会把筛选整个关掉")
    {
        // `inWindow` 用 `startSec >= 0` **兼作「窗生效」旗标**:负左端逐字抄进 Scope 会让
        // `!(startSec >= 0)` 为真 ⇒ 筛选关掉 ⇒ **全导**,而 mock 的 `t1S > -5 && t0S < 10`
        // 只导 10s 之前的段。§1.36 对 startS 只写 f64(没写非负)⇒ 负值是契约合法入参。
        const auto neg = parseSuggestionScope(false, "", false, 0, true, -5.0, true, 10.0, kActive);
        REQUIRE_FALSE(neg.badArg);
        CHECK(neg.scope.startSec == 0.0); // 钳到 0(≡ −∞:段时间恒 ≥ 0)
        CHECK(neg.scope.endSec == 10.0);
        CHECK(neg.scope.startSec >= 0.0); // ← 这一条就是 inWindow 的「窗生效」判据
        // 只给负左端:钳到 0 + 右端 +∞ ⇒ 仍然是「全导」,与 mock 的 (−5, +∞) 同结果
        const auto negOnly = parseSuggestionScope(false, "", false, 0, true, -5.0, false, 0.0, kActive);
        REQUIRE_FALSE(negOnly.badArg);
        CHECK(negOnly.scope.startSec == 0.0);
        CHECK(negOnly.scope.endSec > 1.0e300);
    }

    SECTION("非有限值按「没给」处理(与 mock 的 isFiniteNumber 同口径)")
    {
        const double inf = std::numeric_limits<double>::infinity();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        // `{startS:0, endS:Infinity}` 不得带着 inf 进 Scope(mock 有 isFiniteNumber 守卫,
        // 老写法没有)。顺带:Infinity 其实过不了桥 —— JSON.stringify(Infinity) 是 "null"。
        const auto infEnd = parseSuggestionScope(false, "", false, 0, true, 0.0, true, inf, kActive);
        REQUIRE_FALSE(infEnd.badArg);
        CHECK(infEnd.scope.startSec == 0.0);
        CHECK(infEnd.scope.endSec > 1.0e300); // 退成「只给左端」的半开窗
        // 两头都非有限 ⇒ 等价于都没给 ⇒ 不限
        const auto bothNan = parseSuggestionScope(false, "", false, 0, true, nan, true, nan, kActive);
        REQUIRE_FALSE(bothNan.badArg);
        CHECK(bothNan.scope.startSec < 0.0);
        CHECK(bothNan.scope.endSec < 0.0);
    }
}

// ---------------------------------------------------------------------------
// [SL-536 / J140] ParamWriteAction:参数改动进插件撤销栈的基件。
// 钉的是四件事,每件都对应一个「做错了宿主会看见」的形态:
//   ① alreadyApplied 的动作压栈时**不写**(UI 的 gesture 已经把值写进去了,再写一次 = 宿主平白
//      多录一个同值 gesture);之后的重做**照写**;
//   ② 撤销写旧值、重做写新值,都经注入的 writer(processor 里那个 writer 包着宿主 gesture);
//   ③ 旧 == 新的一步什么都不写(接管占位没被填上的那一格);
//   ④ undo() 恒回 true —— JUCE 的 ActionSet 只要一个动作回 false 就清空**整条**撤销历史。
// ---------------------------------------------------------------------------
TEST_CASE("PARAM-UNDO-1:ParamWriteAction 的写入时机与撤销 / 重做", "[segedit][service][SL536]")
{
    std::vector<float> writes;
    const auto writer = [&writes](float v) { writes.push_back(v); };

    SECTION("alreadyApplied:压栈不写,撤销写旧值,重做写新值")
    {
        juce::UndoManager um;
        um.beginNewTransaction("p");
        REQUIRE(um.perform(new scvb::output::ParamWriteAction(writer, 0.25f, 0.75f, /*alreadyApplied=*/true)));
        CHECK(writes.empty()); // ①
        REQUIRE(um.undo());
        REQUIRE(writes.size() == 1);
        CHECK(writes[0] == 0.25f); // ② 旧值
        REQUIRE(um.redo());
        REQUIRE(writes.size() == 2);
        CHECK(writes[1] == 0.75f); // ① 之后的重做照写
    }

    SECTION("未 applied:压栈即写新值(接管 / 冻结通道那两路不用它,留给将来的直写调用方)")
    {
        juce::UndoManager um;
        um.beginNewTransaction("p");
        REQUIRE(um.perform(new scvb::output::ParamWriteAction(writer, 0.1f, 0.9f, /*alreadyApplied=*/false)));
        REQUIRE(writes.size() == 1);
        CHECK(writes[0] == 0.9f);
    }

    SECTION("旧 == 新:撤销与重做都不写;setNewValue 填上之后才写(接管占位)")
    {
        juce::UndoManager um;
        um.beginNewTransaction("p");
        auto* a = new scvb::output::ParamWriteAction(writer, 0.0f, 0.0f, true);
        REQUIRE(um.perform(a));
        REQUIRE(um.undo());
        REQUIRE(um.redo());
        CHECK(writes.empty()); // ③
        a->setNewValue(1.0f); // UndoManager 持有 a;栈顶未变,指针有效
        REQUIRE(um.undo());
        REQUIRE(writes.size() == 1);
        CHECK(writes[0] == 0.0f);
    }

    SECTION("undo() 恒 true:同一事务里的另一个动作不会因为它被连带清栈")
    {
        juce::UndoManager um;
        um.beginNewTransaction("p");
        REQUIRE(um.perform(new scvb::output::ParamWriteAction(nullptr, 0.0f, 1.0f, true))); // writer 空也不失败
        REQUIRE(um.undo());
        CHECK(um.canRedo()); // 回 false 的话 JUCE 会 clearUndoHistory ⇒ 连重做都没有
    }

    SECTION("absorb:新值推进、旧值保持(一串连按撤回到这一串之前)")
    {
        scvb::output::ParamWriteAction first(writer, 0.2f, 0.3f, true);
        const scvb::output::ParamWriteAction second(writer, 0.3f, 0.4f, true);
        REQUIRE(first.absorb(second));
        CHECK(first.oldValue() == 0.2f);
        CHECK(first.newValue() == 0.4f);
    }
}
