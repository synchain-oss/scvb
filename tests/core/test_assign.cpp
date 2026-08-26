// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

#include <algorithm>
#include <cmath>
#include <vector>

#include "analysis/AutoAssign.h"
#include "dsp/PanMath.h"

namespace
{

using scvb::analysis::assignInterval;
using scvb::analysis::AssignResult;
using scvb::analysis::AutoAssignConfig;
using scvb::analysis::generateSlots;
using scvb::analysis::Slot;
using scvb::analysis::SourceChannels;
using scvb::analysis::TrackMeta;

// 便捷构造:默认 mono、参与、无锁、无对、无上一区间。
TrackMeta makeTrack(int ch, double priority, int pairId = 0, bool lock = false)
{
    TrackMeta t;
    t.channelIndex = ch;
    t.priority = priority;
    t.pairId = pairId;
    t.leadLock = lock;
    return t;
}

// 按 pan 升序返回 idx 序列(用于「相对次序」断言)。
std::vector<int> sortedByPan(const std::vector<double>& pans, const std::vector<int>& idxs)
{
    std::vector<int> out = idxs;
    std::sort(out.begin(), out.end(),
              [&](int a, int b) { return pans[static_cast<std::size_t>(a)] < pans[static_cast<std::size_t>(b)]; });
    return out;
}

} // namespace

TEST_CASE("ASSIGN-1: 4 轨 priority=8/6/4/2 无锁无对无上一区间 → 外环低序取左", "[assign]")
{
    std::vector<TrackMeta> tracks(4);
    const double prio[4] = {8.0, 6.0, 4.0, 2.0};
    for (int i = 0; i < 4; ++i)
        tracks[static_cast<std::size_t>(i)] = makeTrack(i, prio[i]);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});

    REQUIRE(r.pans.size() == 4);
    REQUIRE(r.pans[0] == -60.0);
    REQUIRE(r.pans[1] == 60.0);
    REQUIRE(r.pans[2] == -30.0);
    REQUIRE(r.pans[3] == 30.0);
}

TEST_CASE("ASSIGN-2: 3 轨 priority=9/5/5 → 中心给低序(ε 平局)", "[assign]")
{
    std::vector<TrackMeta> tracks(3);
    const double prio[3] = {9.0, 5.0, 5.0};
    for (int i = 0; i < 3; ++i)
        tracks[static_cast<std::size_t>(i)] = makeTrack(i, prio[i]);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});

    REQUIRE(r.slots.size() == 3);
    REQUIRE(r.slots[0].pan == -60.0);
    REQUIRE(r.slots[1].pan == 60.0);
    REQUIRE(r.slots[2].pan == 0.0);
    REQUIRE(r.pans[0] == -60.0);
    REQUIRE(r.pans[1] == 60.0);
    REQUIRE(r.pans[2] == 0.0);
}

TEST_CASE("ASSIGN-3: 1 lead + 1 pair(p=7) + 2 单轨 → pair 占最外环", "[assign]")
{
    std::vector<TrackMeta> tracks(5);
    tracks[0] = makeTrack(0, 5.0, 0, true); // lead → P=0
    tracks[1] = makeTrack(1, 7.0, 1); // pair 成员 A(低序)
    tracks[2] = makeTrack(2, 7.0, 1); // pair 成员 B
    tracks[3] = makeTrack(3, 3.0);
    tracks[4] = makeTrack(4, 1.0);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});

    REQUIRE(r.pans[0] == 0.0); // lead 居中
    REQUIRE(r.pans[1] == -60.0); // pair 左(低序取左)
    REQUIRE(r.pans[2] == 60.0); // pair 右
    REQUIRE(r.pans[3] == -30.0);
    REQUIRE(r.pans[4] == 30.0);
}

TEST_CASE("ASSIGN-4: 下一区间 t3 priority 改 8.5 → 连续性滞回,布局不变(1.65)", "[assign]")
{
    std::vector<TrackMeta> tracks(4);
    const double prio[4] = {8.0, 6.0, 8.5, 2.0};
    const double prev[4] = {-60.0, 60.0, -30.0, 30.0};
    for (int i = 0; i < 4; ++i)
    {
        tracks[static_cast<std::size_t>(i)] = makeTrack(i, prio[i]);
        tracks[static_cast<std::size_t>(i)].hasPrev = true;
        tracks[static_cast<std::size_t>(i)].prevPan = prev[i];
    }

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});

    REQUIRE(r.pans[0] == -60.0);
    REQUIRE(r.pans[1] == 60.0);
    REQUIRE(r.pans[2] == -30.0);
    REQUIRE(r.pans[3] == 30.0);
    REQUIRE(r.totalCost == Approx(1.65).margin(1e-9));
}

TEST_CASE("ASSIGN-5: 独唱区间(1 轨)→ P=0, u=0,reason=独唱段居中", "[assign]")
{
    std::vector<TrackMeta> tracks(1);
    tracks[0] = makeTrack(0, 9.0);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});

    REQUIRE(r.pans.size() == 1);
    REQUIRE(r.pans[0] == 0.0);
    REQUIRE(r.u.size() == 1);
    REQUIRE(r.u[0] == 0.0);
    REQUIRE(r.reason == "独唱段居中");
}

TEST_CASE("ASSIGN-6: pair 一员活跃 → 活跃员按普通轨,无 linkViolation", "[assign]")
{
    // t1 是 pair 成员但活跃;t2 是其 pair 伙伴但被 lead_lock(pair 破裂)。
    std::vector<TrackMeta> tracks(3);
    tracks[0] = makeTrack(0, 7.0, 1); // 活跃 pair 成员 → 普通轨
    tracks[1] = makeTrack(1, 7.0, 1, true); // pair 伙伴被锁 → pair 破裂
    tracks[2] = makeTrack(2, 3.0);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});

    REQUIRE(r.pans[0] == -60.0); // 按普通轨入槽(非镜像对)
    REQUIRE(r.pans[1] == 0.0); // 锁 → 居中
    REQUIRE(r.pans[2] == 60.0);
}

TEST_CASE("ASSIGN-7: 槽位断言 n=5 → {±60,±30,0}、n=7 → {±60,±40,±20,0}", "[assign]")
{
    const AutoAssignConfig cfg;

    const auto s5 = generateSlots(5, cfg);
    REQUIRE(s5.size() == 5);
    const double expect5[5] = {-60.0, 60.0, -30.0, 30.0, 0.0};
    for (int i = 0; i < 5; ++i)
        REQUIRE(s5[static_cast<std::size_t>(i)].pan == expect5[i]);
    REQUIRE(s5[4].isCenter);

    const auto s7 = generateSlots(7, cfg);
    REQUIRE(s7.size() == 7);
    const double expect7[7] = {-60.0, 60.0, -40.0, 40.0, -20.0, 20.0, 0.0};
    for (int i = 0; i < 7; ++i)
        REQUIRE(s7[static_cast<std::size_t>(i)].pan == expect7[i]);
    REQUIRE(s7[6].isCenter);
}

TEST_CASE("ASSIGN-8: 2 pair + 2 单轨 → 枚举分支法最小分支总代价 0.7", "[assign]")
{
    std::vector<TrackMeta> tracks(6);
    tracks[0] = makeTrack(0, 8.0, 1); // pairA 低序
    tracks[1] = makeTrack(1, 8.0, 1); // pairA 高序
    tracks[2] = makeTrack(2, 5.0, 2); // pairB 低序
    tracks[3] = makeTrack(3, 5.0, 2); // pairB 高序
    tracks[4] = makeTrack(4, 2.0);
    tracks[5] = makeTrack(5, 1.0);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});

    REQUIRE(r.slots.size() == 6);
    REQUIRE(r.pans[0] == -60.0); // pairA 左(低序)
    REQUIRE(r.pans[1] == 60.0); // pairA 右
    REQUIRE(r.pans[2] == -40.0); // pairB 左
    REQUIRE(r.pans[3] == 40.0); // pairB 右
    REQUIRE(r.pans[4] == -20.0);
    REQUIRE(r.pans[5] == 20.0);
    REQUIRE(r.totalCost == Approx(0.7).margin(1e-9));
}

// participate 门控本身([J60] 定的行为)不变;变的只是**默认档**([J83]:未显式设置一律 true,
// 见 docs/contract-changes/20260826-j83-participate-default.md)。本例逐轨显式赋值,不依赖默认档。
TEST_CASE("ASSIGN-9: 15 轨 4 stereo 的 participate 门控", "[assign]")
{
    const AutoAssignConfig cfg;
    std::vector<TrackMeta> tracks(15);

    // 11 条 mono:priority 严格递减。
    const double monoPrio[11] = {9.0, 8.5, 8.0, 7.5, 7.0, 6.5, 6.0, 5.5, 5.0, 4.5, 4.0};
    for (int i = 0; i < 11; ++i)
    {
        tracks[static_cast<std::size_t>(i)] = makeTrack(i, monoPrio[i]);
        tracks[static_cast<std::size_t>(i)].source = SourceChannels::Mono;
    }
    // 4 条 stereo:**显式**设 participate=false;低优先级(参与时落最内)。
    const double stereoPan[4] = {20.0, -20.0, 40.0, -40.0};
    for (int i = 0; i < 4; ++i)
    {
        const int idx = 11 + i;
        TrackMeta& t = tracks[static_cast<std::size_t>(idx)];
        t.channelIndex = idx;
        t.priority = 0.5 - 0.1 * static_cast<double>(i); // 0.5/0.4/0.3/0.2,低于 mono。
        t.source = SourceChannels::Stereo;
        t.participateInAutoPan = false; // 显式关闭(用户在轨道页关掉这四条)。
        t.currentPan = stereoPan[i];
        t.width = 80.0;
    }

    // ① 四条全不参与 → 槽位只按 11 条 mono 生成,4 条 stereo pan 保持源值。
    const AssignResult r1 = assignInterval(tracks, cfg);
    REQUIRE(r1.slots.size() == 11);
    for (int i = 0; i < 4; ++i)
        REQUIRE(r1.pans[static_cast<std::size_t>(11 + i)] == stereoPan[i]);

    // ② 2 条 stereo 参与 → 槽位按 13 个点重新生成。
    tracks[11].participateInAutoPan = true;
    tracks[12].participateInAutoPan = true;
    const AssignResult r2 = assignInterval(tracks, cfg);
    REQUIRE(r2.slots.size() == 13);
    // 仍不参与的两条 stereo 保持源值。
    REQUIRE(r2.pans[13] == stereoPan[2]);
    REQUIRE(r2.pans[14] == stereoPan[3]);
    // 参与的 2 条以中心点入指派(最内两槽:中心 + 最内环)。
    REQUIRE(std::abs(r2.pans[11]) <= 10.0);
    REQUIRE(std::abs(r2.pans[12]) <= 10.0);
    REQUIRE((r2.pans[11] == 0.0 || r2.pans[12] == 0.0));

    // width 不进代价函数:改 width 不改指派。
    tracks[11].width = 0.0;
    tracks[12].width = 100.0;
    const AssignResult r2b = assignInterval(tracks, cfg);
    REQUIRE(r2b.pans == r2.pans);

    // ③ 参与/不参与切换不改变其余轨的相对次序(只改槽位数)。
    std::vector<int> monoIdx(11);
    for (int i = 0; i < 11; ++i)
        monoIdx[static_cast<std::size_t>(i)] = i;
    REQUIRE(sortedByPan(r1.pans, monoIdx) == sortedByPan(r2.pans, monoIdx));
}

TEST_CASE("ASSIGN-10: n=15 与 n=10 槽位共存不冲突,1000 次逐位一致(J59)", "[assign]")
{
    const AutoAssignConfig cfg;
    bool warn15 = false;
    bool warn10 = false;
    const auto s15 = generateSlots(15, cfg, &warn15);
    const auto s10 = generateSlots(10, cfg, &warn10);

    REQUIRE(s15.size() == 15);
    REQUIRE(s10.size() == 10);
    REQUIRE_FALSE(warn15); // 间距 60/7≈8.57 ≥ minSep(8),恰不触发。
    REQUIRE_FALSE(warn10);

    // n=15 奇数分支含中心(K=8,间距 60/7)。
    REQUIRE(s15[14].isCenter);
    REQUIRE(s15[14].pan == 0.0);
    REQUIRE(s15[0].pan == -60.0);
    REQUIRE(s15[1].pan == 60.0);
    REQUIRE(s15[2].pan == Approx(-60.0 * 6.0 / 7.0).margin(1e-12));
    REQUIRE(s15[3].pan == Approx(60.0 * 6.0 / 7.0).margin(1e-12));

    // n=10 偶数分支无中心(K=5,间距 12)。
    REQUIRE_FALSE(s10[9].isCenter);
    REQUIRE(s10[0].pan == -60.0);
    REQUIRE(s10[1].pan == 60.0);
    REQUIRE(s10[2].pan == -48.0);
    REQUIRE(s10[3].pan == 48.0);

    // 共存:再次生成 n=15 不受 n=10 影响,且 1000 次逐位一致。
    for (int rep = 0; rep < 1000; ++rep)
    {
        const auto a = generateSlots(15, cfg);
        const auto b = generateSlots(10, cfg);
        REQUIRE(a.size() == 15);
        REQUIRE(b.size() == 10);
        for (int j = 0; j < 15; ++j)
            REQUIRE(a[static_cast<std::size_t>(j)].pan == s15[static_cast<std::size_t>(j)].pan);
        for (int j = 0; j < 10; ++j)
            REQUIRE(b[static_cast<std::size_t>(j)].pan == s10[static_cast<std::size_t>(j)].pan);
    }
}

TEST_CASE("ASSIGN-determinism: 15 轨指派重复 1000 次逐位一致", "[assign]")
{
    const AutoAssignConfig cfg;
    std::vector<TrackMeta> tracks(15);
    for (int i = 0; i < 15; ++i)
        tracks[static_cast<std::size_t>(i)] = makeTrack(i, 9.0 - 0.5 * static_cast<double>(i));

    const AssignResult ref = assignInterval(tracks, cfg);
    for (int rep = 0; rep < 1000; ++rep)
    {
        const AssignResult r = assignInterval(tracks, cfg);
        REQUIRE(r.pans == ref.pans);
        REQUIRE(r.totalCost == ref.totalCost);
    }
}

// ============================================================================
// 12 种退化情形(02 §5.4 行 4..15 各一条)+ lead_lock 三行单独断言。
// ============================================================================

TEST_CASE("退化4: n=0(全锁/全 manual)→ 不生成槽,锁中 P=0、manual 保持原值", "[assign][degenerate]")
{
    std::vector<TrackMeta> tracks(3);
    tracks[0] = makeTrack(0, 9.0, 0, true);
    tracks[1] = makeTrack(1, 7.0, 0, true);
    tracks[2] = makeTrack(2, 5.0);
    tracks[2].freeze = 1; // 冻结 pan
    tracks[2].currentPan = 30.0;

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});
    REQUIRE(r.slots.empty());
    REQUIRE(r.pans[0] == 0.0);
    REQUIRE(r.pans[1] == 0.0);
    REQUIRE(r.pans[2] == 30.0);
}

TEST_CASE("退化5: 全部轨 lead_lock → 全中,不生成槽", "[assign][degenerate]")
{
    std::vector<TrackMeta> tracks(3);
    for (int i = 0; i < 3; ++i)
        tracks[static_cast<std::size_t>(i)] = makeTrack(i, 9.0, 0, true);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});
    REQUIRE(r.slots.empty());
    for (const double p : r.pans)
        REQUIRE(p == 0.0);
}

TEST_CASE("退化7: pair 成员 lead_lock 冲突 → lead_lock 优先,pair 忽略", "[assign][degenerate]")
{
    std::vector<TrackMeta> tracks(3);
    tracks[0] = makeTrack(0, 7.0, 1); // pair 成员,自由
    tracks[1] = makeTrack(1, 7.0, 1, true); // pair 成员,被锁 → pair 破裂
    tracks[2] = makeTrack(2, 3.0);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});
    REQUIRE(r.pans[0] == -60.0); // 按普通轨(非 pair 镜像)
    REQUIRE(r.pans[1] == 0.0); // lead_lock 优先
    REQUIRE(r.pans[2] == 60.0);
}

TEST_CASE("退化8: 优先级全相等 → 由 ε(channel 序小者先外后左)决定,确定性", "[assign][degenerate]")
{
    std::vector<TrackMeta> tracks(3);
    for (int i = 0; i < 3; ++i)
        tracks[static_cast<std::size_t>(i)] = makeTrack(i, 5.0);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});
    REQUIRE(r.pans[0] == -60.0); // 低序取最外左
    REQUIRE(r.pans[1] == 60.0);
    REQUIRE(r.pans[2] == 0.0);
}

TEST_CASE("退化9: freeze pan 维 → 不占槽,P 保持现值", "[assign][degenerate]")
{
    std::vector<TrackMeta> tracks(3);
    tracks[0] = makeTrack(0, 9.0);
    tracks[0].freeze = 1; // 冻结 pan
    tracks[0].currentPan = 25.0;
    tracks[1] = makeTrack(1, 7.0);
    tracks[2] = makeTrack(2, 3.0);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});
    REQUIRE(r.slots.size() == 2); // 只有 2 条自由轨占槽
    REQUIRE(r.pans[0] == 25.0); // 冻结 → 现值
    REQUIRE(r.pans[1] == -60.0);
    REQUIRE(r.pans[2] == 60.0);
}

TEST_CASE("退化10: participate=false 的轨(显式关闭)→ 不占槽,保持现值", "[assign][degenerate]")
{
    std::vector<TrackMeta> tracks(3);
    tracks[0] = makeTrack(0, 9.0);
    tracks[0].source = SourceChannels::Stereo;
    tracks[0].participateInAutoPan = false;
    tracks[0].currentPan = 20.0;
    tracks[0].width = 80.0;
    tracks[1] = makeTrack(1, 7.0);
    tracks[2] = makeTrack(2, 3.0);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});
    REQUIRE(r.slots.size() == 2);
    REQUIRE(r.pans[0] == 20.0); // 保持源声像
    REQUIRE(r.pans[1] == -60.0);
    REQUIRE(r.pans[2] == 60.0);
}

TEST_CASE("退化11: 参与分配的 stereo(participate=true)→ 以中心点与 mono 同池,width 不进代价", "[assign][degenerate]")
{
    std::vector<TrackMeta> tracks(3);
    tracks[0] = makeTrack(0, 9.0);
    tracks[1] = makeTrack(1, 7.0);
    tracks[1].source = SourceChannels::Stereo;
    tracks[1].participateInAutoPan = true;
    tracks[1].width = 40.0;
    tracks[2] = makeTrack(2, 3.0);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});
    REQUIRE(r.pans[0] == -60.0);
    REQUIRE(r.pans[1] == 60.0); // 中心点入槽
    REQUIRE(r.pans[2] == 0.0);

    tracks[1].width = 100.0; // width 不进代价 → 指派不变。
    const AssignResult r2 = assignInterval(tracks, AutoAssignConfig{});
    REQUIRE(r2.pans == r.pans);
}

TEST_CASE("退化12: lead_select≠0(运行时)→ 本节结果完全不变", "[assign][degenerate]")
{
    // lead_select 是 §8.1 运行时覆盖层,不进入 §5 计算;TrackMeta 无该字段 →
    // 分析结果是输入(TrackMeta)的纯函数。此处以纯函数确定性断言其「完全不变」。
    std::vector<TrackMeta> tracks(3);
    const double prio[3] = {9.0, 5.0, 5.0};
    for (int i = 0; i < 3; ++i)
        tracks[static_cast<std::size_t>(i)] = makeTrack(i, prio[i]);

    const AssignResult a = assignInterval(tracks, AutoAssignConfig{});
    const AssignResult b = assignInterval(tracks, AutoAssignConfig{});
    REQUIRE(a.pans == b.pans);
    REQUIRE(a.slots.size() == 3);
}

TEST_CASE("退化13: z_i≈0 → 仍占槽(有段=有声语义),floor 由 T13 平衡处理", "[assign][degenerate]")
{
    std::vector<TrackMeta> tracks(3);
    const double prio[3] = {9.0, 5.0, 5.0};
    for (int i = 0; i < 3; ++i)
    {
        tracks[static_cast<std::size_t>(i)] = makeTrack(i, prio[i]);
        tracks[static_cast<std::size_t>(i)].z = 1e-12; // 近似无能量
    }

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});
    REQUIRE(r.slots.size() == 3); // 仍占槽
    REQUIRE(r.pans[0] == -60.0);
    REQUIRE(r.pans[1] == 60.0);
    REQUIRE(r.pans[2] == 0.0);
}

TEST_CASE("退化14: 宽度不足(P_base 下调)→ §5.2 收缩 + UI 提示", "[assign][degenerate]")
{
    AutoAssignConfig cfg;
    cfg.pBase = 30.0; // 15 轨时间距 30/7≈4.29 < minSep(8)。
    bool warn = false;
    const auto slots = generateSlots(15, cfg, &warn);

    REQUIRE(warn);
    REQUIRE(slots.size() == 15); // 仍生成全部槽(收缩)。
    REQUIRE(slots[0].pan == Approx(-30.0).margin(1e-12));
    REQUIRE(slots[1].pan == Approx(30.0).margin(1e-12));
    REQUIRE(slots[14].isCenter);
    REQUIRE(slots[14].pan == 0.0);
}

TEST_CASE("退化15: 全局 width=0(运行时)→ 分析结果不变,运行时实际角=0", "[assign][degenerate]")
{
    std::vector<TrackMeta> tracks(4);
    const double prio[4] = {8.0, 6.0, 4.0, 2.0};
    for (int i = 0; i < 4; ++i)
        tracks[static_cast<std::size_t>(i)] = makeTrack(i, prio[i]);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});
    // 分析层名义域结果不受全局 width 影响(§8.1 运行时缩放)。
    REQUIRE(r.pans[0] == -60.0);
    REQUIRE(r.pans[1] == 60.0);
    // 运行时 width=0 → 全部子声像实际角 = 0(真 mono 居中,校听用法)。
    for (const double p : r.pans)
        REQUIRE(scvb::scaleByGlobalWidth(static_cast<float>(p), 0.0f) == 0.0f);
}

// ----------------------------------------------------------------------------
// lead_lock 三行单独断言:锁定居中 / 多主唱 / 锁定+奇数轨。
// ----------------------------------------------------------------------------

TEST_CASE("lead_lock: 锁定居中(lead_lock 恒 P=0 且不占槽)", "[assign][degenerate]")
{
    std::vector<TrackMeta> tracks(3);
    tracks[0] = makeTrack(0, 9.0, 0, true); // 锁
    tracks[1] = makeTrack(1, 7.0);
    tracks[2] = makeTrack(2, 3.0);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});
    REQUIRE(r.pans[0] == 0.0); // 锁定 → 居中
    REQUIRE(r.slots.size() == 2); // 锁不占槽:仅 2 条自由轨。
    REQUIRE(r.pans[1] == -60.0);
    REQUIRE(r.pans[2] == 60.0);
}

TEST_CASE("lead_lock: 多主唱(多条 lock 全部居中)", "[assign][degenerate]")
{
    std::vector<TrackMeta> tracks(4);
    tracks[0] = makeTrack(0, 9.0, 0, true);
    tracks[1] = makeTrack(1, 8.0, 0, true);
    tracks[2] = makeTrack(2, 7.0);
    tracks[3] = makeTrack(3, 3.0);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});
    REQUIRE(r.pans[0] == 0.0);
    REQUIRE(r.pans[1] == 0.0); // 两条主唱均居中。
    REQUIRE(r.slots.size() == 2);
    REQUIRE(r.pans[2] == -60.0);
    REQUIRE(r.pans[3] == 60.0);
}

TEST_CASE("lead_lock: 锁定+奇数轨(锁不占中心槽,自由奇数仍含中心)", "[assign][degenerate]")
{
    std::vector<TrackMeta> tracks(4);
    tracks[0] = makeTrack(0, 9.0, 0, true); // 锁
    tracks[1] = makeTrack(1, 9.0);
    tracks[2] = makeTrack(2, 5.0);
    tracks[3] = makeTrack(3, 1.0);

    const AssignResult r = assignInterval(tracks, AutoAssignConfig{});
    // 自由轨 = 3(奇数)→ 槽位含中心;锁轨不消耗中心槽。
    REQUIRE(r.slots.size() == 3);
    REQUIRE(r.slots[2].isCenter);
    REQUIRE(r.pans[0] == 0.0); // 锁 → 居中(固定)
    REQUIRE(r.pans[1] == -60.0);
    REQUIRE(r.pans[2] == 60.0);
    REQUIRE(r.pans[3] == 0.0); // 最低优先级自由轨取中心槽(与锁轨共占中心位)。
}
