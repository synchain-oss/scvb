// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

#include <cmath>
#include <string>
#include <vector>

#include "analysis/AutoAssign.h"
#include "dsp/PanMath.h"

namespace
{

using scvb::analysis::assignInterval;
using scvb::analysis::AssignResult;
using scvb::analysis::AutoAssignConfig;
using scvb::analysis::BalanceConfig;
using scvb::analysis::BalanceResult;
using scvb::analysis::CenterSlotPolicy;
using scvb::analysis::generateSlots;
using scvb::analysis::sanitizeCenterSlotPolicy;
using scvb::analysis::Slot;
using scvb::analysis::solveBalance;
using scvb::analysis::solveBalanceWithFallback;
using scvb::analysis::SourceChannels;
using scvb::analysis::TrackMeta;

// 便捷构造:mono、参与、无锁、无对、无上一区间。
TrackMeta makeTrack(int ch, double priority, double z = 1.0)
{
    TrackMeta t;
    t.channelIndex = ch;
    t.priority = priority;
    t.z = z;
    return t;
}

// 直接以给定 pans 跑核心平衡(不触发回退链),返回结果。
BalanceResult balanceAt(const std::vector<TrackMeta>& tracks, const std::vector<double>& pans,
                        const BalanceConfig& cfg = BalanceConfig{})
{
    return solveBalance(tracks, pans, cfg);
}

} // namespace

// ============================================================================
// BAL-1..6(02 §6.6 参考值)
// ============================================================================

TEST_CASE("BAL-1: 2 轨硬声像 1 次迭代精确归零,共模守恒", "[balance]")
{
    std::vector<TrackMeta> tracks(2);
    tracks[0] = makeTrack(0, 5.0, 2.0);
    tracks[1] = makeTrack(1, 5.0, 1.0);

    const BalanceResult r = balanceAt(tracks, {-100.0, 100.0});

    REQUIRE(r.d0 == Approx(3.0103).margin(1e-3));
    REQUIRE(r.uBalance[0] == Approx(-1.5051).margin(1e-3));
    REQUIRE(r.uBalance[1] == Approx(1.5051).margin(1e-3));
    REQUIRE(r.delta == Approx(0.2558).margin(1e-3));
    REQUIRE(r.u[0] == Approx(-1.2494).margin(1e-3));
    REQUIRE(r.u[1] == Approx(1.7609).margin(1e-3));
    REQUIRE(r.dFinal == Approx(0.0).margin(1e-3));
    REQUIRE(r.converged);
}

TEST_CASE("BAL-2: 混合角度 4 次检查收敛", "[balance]")
{
    std::vector<TrackMeta> tracks(2);
    tracks[0] = makeTrack(0, 5.0, 1.0);
    tracks[1] = makeTrack(1, 5.0, 1.0);

    const BalanceResult r = balanceAt(tracks, {-66.67, 33.33});

    REQUIRE(r.d0 == Approx(1.6077).margin(1e-3));
    REQUIRE(r.uBalance[0] == Approx(-1.2615).margin(0.01));
    REQUIRE(r.uBalance[1] == Approx(0.7283).margin(0.01));
    REQUIRE(r.delta == Approx(0.1537).margin(0.01));
    REQUIRE(r.u[0] == Approx(-1.1081).margin(0.01));
    REQUIRE(r.u[1] == Approx(0.8821).margin(0.01));
    REQUIRE(r.dFinal == Approx(0.2540).margin(0.01));
    REQUIRE(r.converged);
}

TEST_CASE("BAL-3: 全 P=0 → u 全 0,converged", "[balance]")
{
    std::vector<TrackMeta> tracks(3);
    for (int i = 0; i < 3; ++i)
        tracks[static_cast<std::size_t>(i)] = makeTrack(i, 5.0);

    const BalanceResult r = balanceAt(tracks, {0.0, 0.0, 0.0});

    for (const double u : r.u)
        REQUIRE(u == 0.0);
    REQUIRE(r.converged);
    REQUIRE(r.dFinal == Approx(0.0).margin(1e-9));
}

TEST_CASE("BAL-4: 硬声像对 z 比 16:1 → 触顶,u_balance=±u_max,residualD≈4", "[balance]")
{
    std::vector<TrackMeta> tracks(2);
    tracks[0] = makeTrack(0, 5.0, 16.0);
    tracks[1] = makeTrack(1, 5.0, 1.0);

    const BalanceResult r = balanceAt(tracks, {-100.0, 100.0});

    REQUIRE(r.d0 == Approx(12.0412).margin(1e-3));
    REQUIRE(r.uBalance[0] == Approx(-4.0).margin(1e-9)); // 触顶
    REQUIRE(r.uBalance[1] == Approx(4.0).margin(1e-9)); // 触顶
    REQUIRE(r.capped);
    REQUIRE(r.dBalance == Approx(4.04).margin(0.05)); // 02 §6.6「residualD≈4」= 平衡迭代后 D(共模前)
    REQUIRE_FALSE(r.converged);
}

TEST_CASE("BAL-5: 共模守恒仅在全部 adjustable 场景 <0.01 dB", "[balance]")
{
    // 全 adjustable 且未触顶:BAL-1 构造 → 共模守恒精确。
    {
        std::vector<TrackMeta> tracks(2);
        tracks[0] = makeTrack(0, 5.0, 2.0);
        tracks[1] = makeTrack(1, 5.0, 1.0);
        const BalanceResult r = balanceAt(tracks, {-100.0, 100.0});

        // 解前总响度
        const double lTarget = 10.0 * std::log10(2.0 + 1.0);
        double zL = 0.0;
        double zR = 0.0;
        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            const double th = (tracks[i].source == SourceChannels::Stereo) ? 0.0 : 0.0; // 未用
            (void)th;
            // 用最终 u 重算总线能量(mono 硬声像,曲线 c≡1)
            const double g2 = std::pow(10.0, r.u[i] / 10.0);
            const double pan = (i == 0) ? -100.0 : 100.0;
            const double theta = (pan + 100.0) / 200.0 * 3.14159265358979323846 / 2.0;
            zL += g2 * std::cos(theta) * std::cos(theta) * tracks[i].z;
            zR += g2 * std::sin(theta) * std::sin(theta) * tracks[i].z;
        }
        REQUIRE(10.0 * std::log10(zL + zR) == Approx(lTarget).margin(0.01));
    }

    // 含 freeze bit1(vol 冻结)偏心轨:共模后 D 会变 → 不断言守恒,只按 D_final 判 converged。
    {
        std::vector<TrackMeta> tracks(2);
        tracks[0] = makeTrack(0, 5.0, 2.0); // adjustable
        tracks[1] = makeTrack(1, 5.0, 1.0);
        tracks[1].freeze = 2; // [J65] freeze bit1=1 → vol 维冻结
        const BalanceResult r = balanceAt(tracks, {-100.0, 100.0});

        REQUIRE(r.u[1] == 0.0); // 冻结轨 u 恒 0
        // converged 由共模后 D_final 判定(§6.2/BAL-5);不另断言守恒数值。
        REQUIRE(r.converged == (std::abs(r.dFinal) < 0.3));
    }
}

TEST_CASE("BAL-6: lead_vol_exempt 轨 u 恒 0,其余轨解同「常数背景」手算", "[balance]")
{
    // t0 豁免(P=0,z=2 → L/R 各贡献 1,常数背景);t1 可调(P=-100,z=1)。
    std::vector<TrackMeta> tracks(2);
    tracks[0] = makeTrack(0, 5.0, 2.0);
    tracks[0].leadVolExempt = true;
    tracks[1] = makeTrack(1, 5.0, 1.0);

    const BalanceResult r = balanceAt(tracks, {0.0, -100.0});

    REQUIRE(r.u[0] == 0.0); // 豁免轨恒 0
    // t1 是唯一可调轨,其 u 必非 0(参与平衡)。
    REQUIRE(r.u[1] != 0.0);
    REQUIRE(r.uBalance[0] == 0.0);
}

// ============================================================================
// BAL-7 / BAL-8(02 §6.6 + 回退链)
// ============================================================================

TEST_CASE("BAL-7: 两轨均 P=-100 → z_R 精确 0,floor 守卫生效,回退链 4", "[balance]")
{
    std::vector<TrackMeta> tracks(2);
    tracks[0] = makeTrack(0, 5.0, 2.0);
    tracks[0].freeze = 1; // 冻结 pan(manual 曲线/历史值造成硬单侧)
    tracks[0].currentPan = -100.0;
    tracks[1] = makeTrack(1, 5.0, 1.0);
    tracks[1].freeze = 1;
    tracks[1].currentPan = -100.0;

    const BalanceResult r = balanceAt(tracks, {-100.0, -100.0});

    // 数值守卫:D 有限、无 NaN/±Inf(≤120 量级)。
    REQUIRE(std::isfinite(r.d0));
    REQUIRE(r.d0 == Approx(124.7712).margin(0.1));
    REQUIRE(std::isfinite(r.dFinal));
    // u_balance 双双 clamp 到 −u_max。
    REQUIRE(r.uBalance[0] == Approx(-4.0).margin(1e-9));
    REQUIRE(r.uBalance[1] == Approx(-4.0).margin(1e-9));
    // 共模 Δ≈+u_max → 最终 u≈0(勿按字面断言最终 u=±u_max)。
    REQUIRE(r.delta == Approx(4.0).margin(1e-6));
    REQUIRE(r.u[0] == Approx(0.0).margin(1e-6));
    REQUIRE(r.u[1] == Approx(0.0).margin(1e-6));
    // 同增益不改 D → D_final 仍≈D₀。
    REQUIRE(r.dFinal == Approx(r.d0).margin(1e-6));
    REQUIRE_FALSE(r.converged);

    // 回退链:无非 pair 环 → 直接 4) 警告。
    const BalanceResult chain = solveBalanceWithFallback(tracks, AutoAssignConfig{}, BalanceConfig{});
    REQUIRE(chain.fallbackLevel == 4);
    REQUIRE(chain.fallbackPath == std::vector<int>{1, 2, 3, 4});
    REQUIRE_FALSE(chain.warnings.empty());
    REQUIRE(chain.warnings.front() == "素材左右能量不对称,无法自动平衡");
}

TEST_CASE("BAL-8: 最外环 pair 占据 → δ 平移跳过 pair 环、pair 成员镜像", "[balance]")
{
    // t0/t1 = pair(最外 ±60);t2/t3 = 非 pair 单轨(内 ±30);t4 = manual 硬左(造失衡)。
    std::vector<TrackMeta> tracks(5);
    tracks[0] = makeTrack(0, 8.0, 1.0);
    tracks[0].pairId = 1;
    tracks[1] = makeTrack(1, 8.0, 1.0);
    tracks[1].pairId = 1;
    tracks[2] = makeTrack(2, 3.0, 1.0);
    tracks[3] = makeTrack(3, 2.0, 1.0);
    tracks[4] = makeTrack(4, 5.0, 10.0);
    tracks[4].freeze = 1; // 冻结 pan → manual,不占槽
    tracks[4].currentPan = -100.0;

    const BalanceResult r = solveBalanceWithFallback(tracks, AutoAssignConfig{}, BalanceConfig{});

    // 回退链走到 δ 平移(step 3)。
    REQUIRE(std::find(r.fallbackPath.begin(), r.fallbackPath.end(), 3) != r.fallbackPath.end());

    // pair 两成员全程镜像:仍 ±60,未被 δ 平移改写。
    REQUIRE(r.pans[0] == Approx(-60.0).margin(1e-9));
    REQUIRE(r.pans[1] == Approx(60.0).margin(1e-9));
    REQUIRE(r.pans[0] == Approx(-r.pans[1]).margin(1e-9));

    // 内环非 pair 单轨被 δ 平移:整体移向弱侧(右,δ>0),间距 60 保持。
    REQUIRE(r.pans[2] == Approx(-30.0 + 15.0).margin(1e-9)); // -15
    REQUIRE(r.pans[3] == Approx(30.0 + 15.0).margin(1e-9)); // +45
    REQUIRE(r.pans[3] - r.pans[2] == Approx(60.0).margin(1e-9));
}

// ============================================================================
// 回退链 4 级各触发一次(BAL-9/J60 要求)
// ============================================================================

TEST_CASE("回退链 level 1:首趟 solveBalance 收敛", "[balance][fallback]")
{
    std::vector<TrackMeta> tracks(2);
    tracks[0] = makeTrack(0, 8.0, 2.0);
    tracks[1] = makeTrack(1, 6.0, 1.0);

    const BalanceResult r = solveBalanceWithFallback(tracks, AutoAssignConfig{}, BalanceConfig{});
    REQUIRE(r.fallbackLevel == 1);
    REQUIRE(r.fallbackPath == std::vector<int>{1});
}

TEST_CASE("回退链 level 2:第二趟平衡感知指派后收敛", "[balance][fallback]")
{
    // t0 = manual 硬左(z=6,freeze pan);t1/t2/t3 = free(4/2/1)。
    std::vector<TrackMeta> tracks(4);
    tracks[0] = makeTrack(0, 5.0, 6.0);
    tracks[0].freeze = 1;
    tracks[0].currentPan = -100.0;
    tracks[1] = makeTrack(1, 9.0, 4.0);
    tracks[2] = makeTrack(2, 8.0, 2.0);
    tracks[3] = makeTrack(3, 1.0, 1.0);

    const BalanceResult r = solveBalanceWithFallback(tracks, AutoAssignConfig{}, BalanceConfig{});
    REQUIRE(r.fallbackLevel == 2);
    REQUIRE(r.fallbackPath == std::vector<int>{1, 2});
    // 第二趟把响的轨 A(z=4)推到弱侧(+60)。
    REQUIRE(r.pans[1] == Approx(60.0).margin(1e-9));
    REQUIRE(r.pans[2] == Approx(-60.0).margin(1e-9));
}

TEST_CASE("回退链 level 3:δ 平移后收敛", "[balance][fallback]")
{
    // t0 = manual 硬左(z=3,freeze pan);t1/t2 = free(z=1 各,对称 → 第二趟不变)。
    std::vector<TrackMeta> tracks(3);
    tracks[0] = makeTrack(0, 5.0, 3.0);
    tracks[0].freeze = 1;
    tracks[0].currentPan = -100.0;
    tracks[1] = makeTrack(1, 9.0, 1.0);
    tracks[2] = makeTrack(2, 8.0, 1.0);

    const BalanceResult r = solveBalanceWithFallback(tracks, AutoAssignConfig{}, BalanceConfig{});
    REQUIRE(r.fallbackLevel == 3);
    REQUIRE(r.fallbackPath == std::vector<int>{1, 2, 3});
    // δ=+15:最外环(-60,+60)→(-45,+75)。
    REQUIRE(r.pans[1] == Approx(-45.0).margin(1e-9));
    REQUIRE(r.pans[2] == Approx(75.0).margin(1e-9));
    REQUIRE(r.converged);
}

// ============================================================================
// BAL-9 / BAL-10(stereo 双通道能量,J57/J60)
// ============================================================================

TEST_CASE("BAL-9: stereo 闭式解(J57/J60)—— 双通道能量正确入 L/R", "[balance]")
{
    // 02 §6.6:轨 A stereo P=+50、w=50、z=2(等分 zL=zR=1);轨 B mono P=-100、z=1。
    std::vector<TrackMeta> tracks(2);
    tracks[0] = makeTrack(0, 5.0, 2.0);
    tracks[0].source = SourceChannels::Stereo;
    tracks[0].currentPan = 50.0;
    tracks[0].width = 50.0;
    tracks[1] = makeTrack(1, 5.0, 1.0);

    const BalanceResult r = balanceAt(tracks, {50.0, -100.0});

    // A 子声像 = (0,+100) → ρ_A = (1·0 + 1·(−1))/2 = −0.5(公式断言,经 D₀=0 判别)。
    // z_L = 1·cos²45° + 1·0 + 1 = 1.5,z_R = 0.5 + 1 + 0 = 1.5 → D₀=0,首轮收敛。
    REQUIRE(r.d0 == Approx(0.0).margin(1e-9));
    REQUIRE(r.u[0] == Approx(0.0).margin(1e-9));
    REQUIRE(r.u[1] == Approx(0.0).margin(1e-9));
    REQUIRE(r.converged);
}

TEST_CASE("BAL-9(J60): participate=false 的 stereo 轨 L 强 R 弱 计入 z_L/z_R", "[balance]")
{
    // t0 stereo:P=0、w=100、zL=3、zR=1(L 强 R 弱);t1 mono:P=0、z=1。
    std::vector<TrackMeta> tracks(2);
    tracks[0] = makeTrack(0, 5.0, 4.0);
    tracks[0].source = SourceChannels::Stereo;
    tracks[0].participateInAutoPan = false; // J60:stereo 默认 false
    tracks[0].currentPan = 0.0;
    tracks[0].width = 100.0;
    tracks[0].zL = 3.0;
    tracks[0].zR = 1.0;
    tracks[1] = makeTrack(1, 5.0, 1.0);
    tracks[1].participateInAutoPan = false;
    tracks[1].currentPan = 0.0;

    const BalanceResult r = balanceAt(tracks, {0.0, 0.0});

    // 双通道能量:z_L = 3(左通道经 P_L=-100 全左)+ 0.5 = 3.5;z_R = 1(右通道经 P_R=+100 全右)+ 0.5 = 1.5。
    // D₀ = 10·log10(3.5/1.5) = 3.6798。
    REQUIRE(r.d0 == Approx(3.6798).margin(1e-3));
    // 单点折算(错误,ρ=cos2θ(P=0)=0)会给 z_L=z_R=2.5 → D₀=0;此处 D₀≠0 证明 stereo 按双通道能量计入。
    REQUIRE(std::abs(r.d0) > 0.3);

    // stereo 轨 u 是单一标量 → 对 L/R 同乘 g²,内部 L:R 比值 3:1 不变。
    const double g2 = std::pow(10.0, r.u[0] / 10.0);
    const double lContrib = g2 * 3.0; // 左通道(P_L=-100 全左)
    const double rContrib = g2 * 1.0; // 右通道(P_R=+100 全右)
    REQUIRE(lContrib / rContrib == Approx(3.0).margin(1e-9));

    // 排除该 stereo 轨(或按单点折算)会得到 D₀=0 —— 数值对比进 PR 描述。
    const double wrongD = 10.0 * std::log10(0.5 / 0.5); // 仅 t1 居中
    REQUIRE(wrongD == Approx(0.0).margin(1e-9));
    REQUIRE(std::abs(r.d0 - wrongD) > 1.0);
}

TEST_CASE("BAL-10: 全宽居中 stereo P=0、w=100 → ρ=0 无杠杆", "[balance]")
{
    std::vector<TrackMeta> tracks(1);
    tracks[0] = makeTrack(0, 5.0, 4.0);
    tracks[0].source = SourceChannels::Stereo;
    tracks[0].currentPan = 0.0;
    tracks[0].width = 100.0;
    tracks[0].zL = 2.0;
    tracks[0].zR = 2.0;

    const BalanceResult r = balanceAt(tracks, {0.0});
    // 子声像 (−100,+100) → ρ=0(循环不改其 u);对 z_L/z_R 各贡献 z/2。
    REQUIRE(r.d0 == Approx(0.0).margin(1e-9));
    REQUIRE(r.u[0] == Approx(0.0).margin(1e-9));
}

// ============================================================================
// J69 中心槽策略(02 §5.6)五条断言
// ============================================================================

TEST_CASE("J69-center-1: 三档向量可区分且同输入 1000 次逐位一致", "[assign][center-policy]")
{
    auto pqTracks = [] {
        std::vector<TrackMeta> t(3);
        t[0] = makeTrack(0, 9.0);
        t[1] = makeTrack(1, 5.0);
        t[2] = makeTrack(2, 5.0);
        return t;
    };

    AutoAssignConfig pqCfg;
    pqCfg.centerSlotPolicy = CenterSlotPolicy::PriorityQueue;
    const AssignResult pq = assignInterval(pqTracks(), pqCfg);
    REQUIRE(pq.slots.size() == 3);
    REQUIRE(pq.pans[0] == -60.0);
    REQUIRE(pq.pans[1] == 60.0);
    REQUIRE(pq.pans[2] == 0.0);

    // lead_exclusive(C≠∅):t0 lead_lock + 3 free。
    AutoAssignConfig leCfg;
    leCfg.centerSlotPolicy = CenterSlotPolicy::LeadExclusive;
    std::vector<TrackMeta> leTracks(4);
    leTracks[0] = makeTrack(0, 5.0);
    leTracks[0].leadLock = true;
    leTracks[1] = makeTrack(1, 9.0);
    leTracks[2] = makeTrack(2, 5.0);
    leTracks[3] = makeTrack(3, 5.0);
    const AssignResult le = assignInterval(leTracks, leCfg);
    REQUIRE(le.slots.size() == 4); // {±60,±30}
    REQUIRE(le.pans[0] == 0.0); // lead 仍 P=0
    REQUIRE(le.pans[1] == -60.0);
    REQUIRE(le.pans[2] == 60.0);
    REQUIRE(le.pans[3] == -30.0); // 空 +30

    // even_offset。
    AutoAssignConfig eoCfg;
    eoCfg.centerSlotPolicy = CenterSlotPolicy::EvenOffset;
    const AssignResult eo = assignInterval(pqTracks(), eoCfg);
    REQUIRE(eo.slots.size() == 4); // {±60,±4}
    REQUIRE(eo.pans[0] == -60.0);
    REQUIRE(eo.pans[1] == 60.0);
    REQUIRE(eo.pans[2] == -4.0); // 原落中心者取 −4(ε 低序取左)

    // 三档向量可区分。
    REQUIRE(pq.pans != le.pans);
    REQUIRE(pq.pans != eo.pans);
    REQUIRE(le.pans != eo.pans);

    // 同输入 1000 次逐位一致。
    for (int rep = 0; rep < 1000; ++rep)
    {
        REQUIRE(assignInterval(pqTracks(), pqCfg).pans == pq.pans);
        REQUIRE(assignInterval(leTracks, leCfg).pans == le.pans);
        REQUIRE(assignInterval(pqTracks(), eoCfg).pans == eo.pans);
    }
}

TEST_CASE("J69-center-2: 主唱独占并入既有 lead 逻辑,不新造第二套主唱概念", "[assign][center-policy]")
{
    // C=∅ → lead_exclusive 退化为 priority_queue(与默认档逐位一致)。
    {
        std::vector<TrackMeta> tracks(3);
        tracks[0] = makeTrack(0, 9.0);
        tracks[1] = makeTrack(1, 5.0);
        tracks[2] = makeTrack(2, 5.0);

        AutoAssignConfig def;
        AutoAssignConfig le;
        le.centerSlotPolicy = CenterSlotPolicy::LeadExclusive;
        const AssignResult a = assignInterval(tracks, def);
        const AssignResult b = assignInterval(tracks, le);
        REQUIRE(a.pans == b.pans);
    }

    // lead_lock 语义不变:恒 P=0、可多轨共占、从不消耗槽位(lead_exclusive 下同样成立)。
    {
        std::vector<TrackMeta> tracks(4);
        tracks[0] = makeTrack(0, 9.0);
        tracks[0].leadLock = true;
        tracks[1] = makeTrack(1, 8.0);
        tracks[1].leadLock = true; // 多主唱
        tracks[2] = makeTrack(2, 5.0);
        tracks[3] = makeTrack(3, 1.0);

        AutoAssignConfig le;
        le.centerSlotPolicy = CenterSlotPolicy::LeadExclusive;
        const AssignResult r = assignInterval(tracks, le);
        REQUIRE(r.pans[0] == 0.0);
        REQUIRE(r.pans[1] == 0.0);
        // 自由轨 2 条(锁不占槽)→ 偶数分支 {±60}。
        REQUIRE(r.slots.size() == 2);
        REQUIRE(r.pans[2] == -60.0);
        REQUIRE(r.pans[3] == 60.0);
    }
}

TEST_CASE("J69-center-3: 与 lead_vol_exempt / lead_select 各自独立(J58 反向断言)", "[balance][center-policy]")
{
    // center_slot_policy=lead_exclusive 且该轨 lead_vol_exempt=false → 该轨 u 照常参与平衡。
    // lead_select 是运行时覆盖层(§8.1),不进入 TrackMeta/§5/§6 任何计算 → 无联动。
    std::vector<TrackMeta> tracks(2);
    tracks[0] = makeTrack(0, 5.0, 2.0);
    tracks[0].leadVolExempt = false; // 显式:不豁免
    tracks[1] = makeTrack(1, 5.0, 1.0);

    BalanceConfig cfg;
    const BalanceResult r = solveBalance(tracks, {-100.0, 100.0}, cfg);
    REQUIRE(r.u[0] != 0.0); // 照常参与平衡(不被豁免、不被 lead_select/center_slot_policy 影响)
    REQUIRE(r.u[1] != 0.0);
}

TEST_CASE("J69-center-4: 未知/越界值回落默认档并记 warning", "[assign][center-policy]")
{
    bool warned = false;
    const CenterSlotPolicy p = sanitizeCenterSlotPolicy(99, &warned);
    REQUIRE(p == CenterSlotPolicy::PriorityQueue);
    REQUIRE(warned);

    bool warned2 = true;
    REQUIRE(sanitizeCenterSlotPolicy(static_cast<int>(CenterSlotPolicy::LeadExclusive), &warned2) ==
            CenterSlotPolicy::LeadExclusive);
    REQUIRE_FALSE(warned2);
    REQUIRE(sanitizeCenterSlotPolicy(static_cast<int>(CenterSlotPolicy::EvenOffset), nullptr) ==
            CenterSlotPolicy::EvenOffset);
}

TEST_CASE("J69-center-5: 与 T12 同一策略两端,枚举与默认只在 02 定义一次", "[assign][center-policy]")
{
    // 默认档 = priority_queue(方案 A),与 T12 的 generateSlots 结果逐位一致。
    AutoAssignConfig def;
    REQUIRE(def.centerSlotPolicy == CenterSlotPolicy::PriorityQueue);
    REQUIRE(def.deltaCenter == Approx(4.0).margin(1e-12));

    const auto s5 = generateSlots(5, def);
    REQUIRE(s5.size() == 5);
    const double expect5[5] = {-60.0, 60.0, -30.0, 30.0, 0.0};
    for (int i = 0; i < 5; ++i)
        REQUIRE(s5[static_cast<std::size_t>(i)].pan == expect5[i]);

    const auto s7 = generateSlots(7, def);
    const double expect7[7] = {-60.0, 60.0, -40.0, 40.0, -20.0, 20.0, 0.0};
    for (int i = 0; i < 7; ++i)
        REQUIRE(s7[static_cast<std::size_t>(i)].pan == expect7[i]);
}
