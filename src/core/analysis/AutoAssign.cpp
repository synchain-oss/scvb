// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/AutoAssign.h"

#include "analysis/Hungarian.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace scvb::analysis
{

namespace
{

// p_target(j) = K>1 ? 1 − (ringRank−1)/(K−1) : 1(02 §5.3):最外圈 1,最内圈/中心 0。
double pTarget(int ringRank, int K)
{
    if (K > 1)
        return 1.0 - static_cast<double>(ringRank - 1) / static_cast<double>(K - 1);
    return 1.0;
}

// [J59] 轨道上限 15;channelIndex ∈ [0,14]。
inline constexpr int kMaxAutoTracks = 15;

// ε 平局确定性(02 §5.3):使「channel 序小者取排序靠前槽(先外后左)」成为唯一最优。
// ε = eps·j·(15 − ch) = 旧 Monge 交叉项(−eps·ch·j)+ 15·eps·j:
//  * 完美匹配(方阵)下 15·eps·j 求和为常数 → 与旧实现逐位等价,不破坏 T12 平局;
//  * 矩形(center_slot_policy 奇数分支 n+1 槽,§5.6)下该项使「补虚拟行代价 0」的空槽
//    落在最高序号槽(最内/右),与 02 §5.5 ASSIGN-11 定稿一致。扰动上界 ≈1e-6·14·15≈2.1e-4,
//    远小于任何真实代价差(w_prio 最小 ~0.01),只在平局时起作用。
double epsilonOf(int channelIndex, int S, int slotIndex, const AutoAssignConfig& cfg)
{
    (void)S; // 保留规格签名;矩形语义由 15·eps·j 项承载。
    return cfg.epsilonBase * static_cast<double>(slotIndex) * static_cast<double>(kMaxAutoTracks - channelIndex);
}

// 上一区间侧:−1=左,0=无/中心,+1=右。
int prevSideOf(const TrackMeta& t)
{
    if (!t.hasPrev)
        return 0;
    if (t.prevPan < 0.0)
        return -1;
    if (t.prevPan > 0.0)
        return +1;
    return 0;
}

int slotSideOf(const Slot& s)
{
    if (s.isCenter)
        return 0;
    return s.pan < 0.0 ? -1 : +1;
}

// 基础代价(w_prio/w_cont/w_side/w_link;不含 ε)。w_link 恒 0(pair 已预合并)。
double baseCost(const TrackMeta& t, const Slot& s, int K, const AutoAssignConfig& cfg)
{
    const double p = t.priority / 10.0;
    double c = cfg.wPrio * std::abs(p - pTarget(s.ringRank, K));
    if (t.hasPrev)
    {
        c += cfg.wCont * std::abs(t.prevPan - s.pan) / 200.0;
        const int ps = prevSideOf(t);
        const int ss = slotSideOf(s);
        if (ps != 0 && ss != 0 && ps != ss)
            c += cfg.wSide;
    }
    return c;
}

// ρ(P) = cos(2θ),θ=(P+100)/200·π/2(mono 点源杠杆,02 §6.1/§5.3 第二趟)。
double rhoOfPan(double pan)
{
    constexpr double kPi = 3.14159265358979323846;
    return std::cos((pan + 100.0) / 200.0 * kPi);
}

// 矩阵项 = 基础代价 + ε + 平衡感知项(进入匈牙利,用于确定性平局/第二趟)。
// balHint 非空时加 wBal · signD0 · ẑ_i · ρ(P(j))(02 §5.3);trackOrig = 轨在 tracks 中的原始下标。
double entryCost(const TrackMeta& t, const Slot& s, int K, int S, int slotIndex, const AutoAssignConfig& cfg,
                 const BalanceAwareHint* balHint, int trackOrig)
{
    double c = baseCost(t, s, K, cfg) + epsilonOf(t.channelIndex, S, slotIndex, cfg);
    if (balHint != nullptr)
        c += cfg.wBal * balHint->signD0 * balHint->zHat[static_cast<std::size_t>(trackOrig)] * rhoOfPan(s.pan);
    return c;
}

// pair 成员左右取(02 §5.3):上一区间已有侧 → 沿用;否则 pair 内 channel 序小者取左。
// 返回 {leftPos, rightPos}(leftPos 取 −r 槽,rightPos 取 +r 槽)。
std::pair<int, int> pairSides(int a, int b, const std::vector<TrackMeta>& tracks)
{
    const int pa = prevSideOf(tracks[static_cast<std::size_t>(a)]);
    const int pb = prevSideOf(tracks[static_cast<std::size_t>(b)]);
    if (pa == -1 && pb == +1)
        return {a, b};
    if (pa == +1 && pb == -1)
        return {b, a};
    return {a, b}; // 默认:低序(a 为 pairs 已按 channel 序排序后的较小者)取左。
}

} // namespace

CenterSlotPolicy sanitizeCenterSlotPolicy(int rawValue, bool* warnedOut)
{
    const bool valid = rawValue == static_cast<int>(CenterSlotPolicy::PriorityQueue) ||
                       rawValue == static_cast<int>(CenterSlotPolicy::LeadExclusive) ||
                       rawValue == static_cast<int>(CenterSlotPolicy::EvenOffset);
    if (warnedOut != nullptr)
        *warnedOut = !valid;
    if (!valid)
        return CenterSlotPolicy::PriorityQueue; // J69 ④:未知/越界 → 默认档。
    return static_cast<CenterSlotPolicy>(rawValue);
}

std::vector<Slot> generateSlots(int n, const AutoAssignConfig& cfg, bool* widthWarningOut, bool hasLeadLock)
{
    std::vector<Slot> slots;
    bool warn = false;
    if (n < 0)
        n = 0;
    if (n > 0)
    {
        const int K = (n + 1) / 2; // ceil(n/2)
        const bool odd = (n % 2) == 1;

        // J69(02 §5.6):lead_exclusive 仅当 C≠∅ 且奇数分支时剔除中心;even_offset 仅奇数且
        // K≥2(有主半径、存在争抢)时把中心拆成微偏对;n=1(K=1)仍居中(§5.4 独唱语义)。
        const bool leadExclusiveActive =
            (cfg.centerSlotPolicy == CenterSlotPolicy::LeadExclusive) && hasLeadLock && odd;
        const bool evenOffsetActive = (cfg.centerSlotPolicy == CenterSlotPolicy::EvenOffset) && odd && K >= 2;

        std::vector<double> radii; // 镜像主半径,外→内。
        if (!odd || leadExclusiveActive)
        {
            // 偶数分支(含 lead_exclusive 剔除中心后的奇数,§5.6):radii = P_base·(K−k+1)/K。
            for (int k = 1; k <= K; ++k)
                radii.push_back(cfg.pBase * static_cast<double>(K - k + 1) / static_cast<double>(K));
        }
        else if (K >= 2)
        {
            // 奇数(priority_queue / even_offset 主半径):radii = P_base·(K−k)/(K−1)。
            for (int k = 1; k <= K - 1; ++k)
                radii.push_back(cfg.pBase * static_cast<double>(K - k) / static_cast<double>(K - 1));
        }

        // 最小间隔保护(§5.2):相邻主半径差(含中心 0 / 微偏半径与最内半径之差)< minSep → 提示。
        double minGap = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i + 1 < radii.size(); ++i)
            minGap = std::min(minGap, radii[i] - radii[i + 1]);
        if (odd)
        {
            if (evenOffsetActive)
                minGap = std::min(minGap, radii.empty() ? std::numeric_limits<double>::infinity()
                                                        : radii.back() - cfg.deltaCenter);
            else if (!leadExclusiveActive)
                minGap = std::min(minGap, radii.empty() ? std::numeric_limits<double>::infinity() : radii.back());
        }
        if (n >= 2 && minGap < cfg.minSep)
            warn = true; // 原始生成已是「max 半径=P_base 的等间距」,收缩语义由等间距不变量承载。

        // 冻结排序:环外→内;同环先左(−)后右(+);中心(或微偏对)最后(02 §5.2/§5.6)。
        int ringRank = 1;
        for (const double r : radii)
        {
            Slot left;
            left.pan = -r;
            left.ringRank = ringRank;
            left.isCenter = false;
            Slot right;
            right.pan = r;
            right.ringRank = ringRank;
            right.isCenter = false;
            slots.push_back(left);
            slots.push_back(right);
            ++ringRank;
        }
        if (odd && !leadExclusiveActive)
        {
            if (evenOffsetActive)
            {
                // even_offset:中心槽拆成一对微偏槽 {−δ_c,+δ_c}(§5.6),与原中心同秩(p_target=0)。
                Slot ml;
                ml.pan = -cfg.deltaCenter;
                ml.ringRank = K;
                ml.isCenter = false;
                Slot mr;
                mr.pan = cfg.deltaCenter;
                mr.ringRank = K;
                mr.isCenter = false;
                slots.push_back(ml);
                slots.push_back(mr);
            }
            else
            {
                Slot center;
                center.pan = 0.0;
                center.ringRank = K;
                center.isCenter = true;
                slots.push_back(center);
            }
        }
    }
    if (widthWarningOut != nullptr)
        *widthWarningOut = warn;
    return slots;
}

AssignResult assignInterval(const std::vector<TrackMeta>& tracks, const AutoAssignConfig& cfg,
                            const BalanceAwareHint* balHint)
{
    AssignResult result;
    const int numTracks = static_cast<int>(tracks.size());
    result.pans.assign(static_cast<std::size_t>(numTracks), 0.0);
    result.u.assign(static_cast<std::size_t>(numTracks), 0.0);

    // 1. 分类(优先级:leadLock > freeze pan 维 > 非参与 > 自由)。
    // fixed/manual/nonpart 直接落结果,不占槽;只有 free 进入后续指派。
    std::vector<int> freePos;
    bool hasLeadLock = false; // J69:区间内是否存在 lead_lock 轨(C≠∅),lead_exclusive 档用。
    for (int i = 0; i < numTracks; ++i)
    {
        const TrackMeta& t = tracks[static_cast<std::size_t>(i)];
        if (t.leadLock)
        {
            hasLeadLock = true;
            result.pans[static_cast<std::size_t>(i)] = 0.0;
        }
        else if ((t.freeze & 1) != 0) // freeze ∈ {1,3}:pan 维冻结。
        {
            result.pans[static_cast<std::size_t>(i)] = t.currentPan;
        }
        else if (!t.participateInAutoPan)
        {
            result.pans[static_cast<std::size_t>(i)] = t.currentPan;
        }
        else
        {
            freePos.push_back(i);
        }
    }

    // 2. pair 预合并(超级节点):同 pairId 的两个自由成员成对;仅一员自由 → 按普通轨。
    std::map<int, std::vector<int>> byPair;
    std::vector<int> freeSinglePos;
    for (const int pos : freePos)
    {
        if (tracks[static_cast<std::size_t>(pos)].pairId > 0)
            byPair[tracks[static_cast<std::size_t>(pos)].pairId].push_back(pos);
        else
            freeSinglePos.push_back(pos);
    }
    std::vector<std::pair<int, int>> pairs;
    for (auto& kv : byPair)
    {
        auto& members = kv.second;
        std::sort(members.begin(), members.end(), [&](int a, int b) {
            return tracks[static_cast<std::size_t>(a)].channelIndex < tracks[static_cast<std::size_t>(b)].channelIndex;
        });
        std::size_t i = 0;
        for (; i + 1 < members.size(); i += 2)
            pairs.emplace_back(members[i], members[i + 1]);
        if (i < members.size()) // 奇数个成员:末位落单,按普通轨。
            freeSinglePos.push_back(members[i]);
    }
    std::sort(pairs.begin(), pairs.end(), [&](const std::pair<int, int>& a, const std::pair<int, int>& b) {
        return tracks[static_cast<std::size_t>(a.first)].channelIndex <
               tracks[static_cast<std::size_t>(b.first)].channelIndex;
    });

    const int n = static_cast<int>(freeSinglePos.size()) + 2 * static_cast<int>(pairs.size());
    result.slots = generateSlots(n, cfg, &result.widthWarning, hasLeadLock);
    if (n == 0)
        return result; // 全锁 / 全 manual / 全非参与:无槽。

    const int S = static_cast<int>(result.slots.size()); // == n(方阵)或 == n+1(center_slot_policy 奇数分支)。
    const int K = (n + 1) / 2;

    // assignedSlotIdx[pos] = 自由轨分到的槽下标(结果;总代价按 baseCost 口径累加)。
    std::vector<int> assignedSlotIdx(static_cast<std::size_t>(numTracks), -1);

    if (pairs.empty())
    {
        // 无 pair → 标准匈牙利(m×S;center_slot_policy 奇数分支 S=m+1 为矩形,补虚拟行代价 0)。
        const int m = static_cast<int>(freeSinglePos.size());
        std::vector<std::vector<double>> cost(static_cast<std::size_t>(m),
                                              std::vector<double>(static_cast<std::size_t>(S), 0.0));
        for (int i = 0; i < m; ++i)
        {
            const int orig = freeSinglePos[static_cast<std::size_t>(i)];
            const TrackMeta& t = tracks[static_cast<std::size_t>(orig)];
            for (int j = 0; j < S; ++j)
                cost[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] =
                    entryCost(t, result.slots[static_cast<std::size_t>(j)], K, S, j, cfg, balHint, orig);
        }
        const HungarianResult hr = solveHungarian(cost);
        for (int i = 0; i < m; ++i)
            if (hr.rowToCol[static_cast<std::size_t>(i)] >= 0)
                assignedSlotIdx[static_cast<std::size_t>(freeSinglePos[static_cast<std::size_t>(i)])] =
                    hr.rowToCol[static_cast<std::size_t>(i)];
    }
    else
    {
        // 有 pair → 枚举分支法(§5.3):各 pair 独占不同镜像环,删去被占环两槽后跑匈牙利。
        // 镜像环数 = 非中心槽数 / 2(center_slot_policy 奇数分支无中心槽时同样成立)。
        int ringCount = 0;
        for (const Slot& s : result.slots)
            if (!s.isCenter)
                ++ringCount;
        ringCount /= 2;
        const int P = static_cast<int>(pairs.size());

        std::vector<int> bestAssign(static_cast<std::size_t>(P), 0);
        double bestCost = std::numeric_limits<double>::infinity();

        std::vector<int> cur(static_cast<std::size_t>(P), 0);
        std::vector<bool> usedRing(static_cast<std::size_t>(ringCount), false);

        std::function<void(int)> dfs = [&](int depth) {
            if (depth == P)
            {
                double branchCost = 0.0;
                std::vector<bool> occ(static_cast<std::size_t>(S), false);
                for (int pi = 0; pi < P; ++pi)
                {
                    const int ring = cur[static_cast<std::size_t>(pi)];
                    const int li = 2 * ring; // 环 r 的左槽(排序:同环先左后右)。
                    const int ri = 2 * ring + 1;
                    const std::pair<int, int> sd = pairSides(pairs[static_cast<std::size_t>(pi)].first,
                                                             pairs[static_cast<std::size_t>(pi)].second, tracks);
                    branchCost +=
                        entryCost(tracks[static_cast<std::size_t>(sd.first)],
                                  result.slots[static_cast<std::size_t>(li)], K, S, li, cfg, balHint, sd.first);
                    branchCost +=
                        entryCost(tracks[static_cast<std::size_t>(sd.second)],
                                  result.slots[static_cast<std::size_t>(ri)], K, S, ri, cfg, balHint, sd.second);
                    occ[static_cast<std::size_t>(li)] = true;
                    occ[static_cast<std::size_t>(ri)] = true;
                }
                std::vector<int> remSlots;
                for (int j = 0; j < S; ++j)
                    if (!occ[static_cast<std::size_t>(j)])
                        remSlots.push_back(j);
                const int m = static_cast<int>(freeSinglePos.size());
                if (m > 0)
                {
                    std::vector<std::vector<double>> cost(static_cast<std::size_t>(m),
                                                          std::vector<double>(remSlots.size(), 0.0));
                    for (int i = 0; i < m; ++i)
                    {
                        const int orig = freeSinglePos[static_cast<std::size_t>(i)];
                        const TrackMeta& t = tracks[static_cast<std::size_t>(orig)];
                        for (std::size_t jj = 0; jj < remSlots.size(); ++jj)
                            cost[static_cast<std::size_t>(i)][jj] =
                                entryCost(t, result.slots[static_cast<std::size_t>(remSlots[jj])], K, S, remSlots[jj],
                                          cfg, balHint, orig);
                    }
                    branchCost += solveHungarian(cost).totalCost;
                }
                if (branchCost < bestCost)
                {
                    bestCost = branchCost;
                    bestAssign = cur;
                }
                return;
            }
            for (int r = 0; r < ringCount; ++r)
            {
                if (usedRing[static_cast<std::size_t>(r)])
                    continue;
                usedRing[static_cast<std::size_t>(r)] = true;
                cur[static_cast<std::size_t>(depth)] = r;
                dfs(depth + 1);
                usedRing[static_cast<std::size_t>(r)] = false;
            }
        };
        dfs(0);

        // 应用最优分支(平局取环序字典序小者:DFS 依环序升序枚举,严格 < 保留首个)。
        std::vector<bool> occ(static_cast<std::size_t>(S), false);
        for (int pi = 0; pi < P; ++pi)
        {
            const int ring = bestAssign[static_cast<std::size_t>(pi)];
            const int li = 2 * ring;
            const int ri = 2 * ring + 1;
            const std::pair<int, int> sd = pairSides(pairs[static_cast<std::size_t>(pi)].first,
                                                     pairs[static_cast<std::size_t>(pi)].second, tracks);
            assignedSlotIdx[static_cast<std::size_t>(sd.first)] = li;
            assignedSlotIdx[static_cast<std::size_t>(sd.second)] = ri;
            occ[static_cast<std::size_t>(li)] = true;
            occ[static_cast<std::size_t>(ri)] = true;
        }
        std::vector<int> remSlots;
        for (int j = 0; j < S; ++j)
            if (!occ[static_cast<std::size_t>(j)])
                remSlots.push_back(j);
        const int m = static_cast<int>(freeSinglePos.size());
        if (m > 0)
        {
            std::vector<std::vector<double>> cost(static_cast<std::size_t>(m),
                                                  std::vector<double>(remSlots.size(), 0.0));
            for (int i = 0; i < m; ++i)
            {
                const int orig = freeSinglePos[static_cast<std::size_t>(i)];
                const TrackMeta& t = tracks[static_cast<std::size_t>(orig)];
                for (std::size_t jj = 0; jj < remSlots.size(); ++jj)
                    cost[static_cast<std::size_t>(i)][jj] =
                        entryCost(t, result.slots[static_cast<std::size_t>(remSlots[jj])], K, S, remSlots[jj], cfg,
                                  balHint, orig);
            }
            const HungarianResult hr = solveHungarian(cost);
            for (int i = 0; i < m; ++i)
                if (hr.rowToCol[static_cast<std::size_t>(i)] >= 0)
                    assignedSlotIdx[static_cast<std::size_t>(freeSinglePos[static_cast<std::size_t>(i)])] =
                        remSlots[static_cast<std::size_t>(hr.rowToCol[static_cast<std::size_t>(i)])];
        }
    }

    // 填 pans 与 totalCost(base 口径,不含 ε)。
    for (int pos = 0; pos < numTracks; ++pos)
    {
        const int j = assignedSlotIdx[static_cast<std::size_t>(pos)];
        if (j >= 0)
        {
            result.pans[static_cast<std::size_t>(pos)] = result.slots[static_cast<std::size_t>(j)].pan;
            result.totalCost +=
                baseCost(tracks[static_cast<std::size_t>(pos)], result.slots[static_cast<std::size_t>(j)], K, cfg);
        }
    }

    if (n == 1)
        result.reason = "独唱段居中"; // 02 §5.4 退化 1/2。

    return result;
}

namespace
{

constexpr double kBalancePi = 3.14159265358979323846;

double clampPanD(double p)
{
    if (p < -100.0)
        return -100.0;
    if (p > 100.0)
        return 100.0;
    return p;
}

// 单轨能量模型系数(02 §6.1):u=0 下 z_L 贡献 = g²·lCoef、z_R 贡献 = g²·rCoef;lever = ρ_i。
// mono:z_iL=z、z_iR=0;stereo:z_iL/z_iR 为双通道能量,两字段均 0 时等分 z/2(v1 近似,§6.1)。
struct TrackEnergy
{
    double lCoef = 0.0;
    double rCoef = 0.0;
    double lever = 0.0;
};

TrackEnergy buildTrackEnergy(const TrackMeta& t, double pan, const std::vector<PanCurvePoint>& curve)
{
    TrackEnergy e;
    const auto thetaOf = [](double p) { return (p + 100.0) / 200.0 * kBalancePi / 2.0; };
    const auto cos2 = [&](double p) {
        const double th = thetaOf(p);
        return std::cos(th) * std::cos(th);
    };
    const auto sin2 = [&](double p) {
        const double th = thetaOf(p);
        return std::sin(th) * std::sin(th);
    };
    const auto cos2theta = [&](double p) { return std::cos(2.0 * thetaOf(p)); };
    const auto gain = [&](double p) { return std::pow(10.0, scvb::evalCurve(curve, p) / 20.0); };

    if (t.source == SourceChannels::Stereo)
    {
        double zL = t.zL;
        double zR = t.zR;
        if (zL == 0.0 && zR == 0.0)
        {
            // v1 等分近似:FeatFrame.kw_ms 为多通道求和口径(ipc v1.4),离线无法分离 L/R(02 §6.1)。
            zL = t.z / 2.0;
            zR = t.z / 2.0;
        }
        const double pL = clampPanD(pan - t.width);
        const double pR = clampPanD(pan + t.width);
        const double cL = gain(pL);
        const double cR = gain(pR);
        const double wL = cL * cL * zL;
        const double wR = cR * cR * zR;
        e.lCoef = wL * cos2(pL) + wR * cos2(pR);
        e.rCoef = wL * sin2(pL) + wR * sin2(pR);
        const double denom = wL + wR;
        e.lever = (denom > 0.0) ? (wL * cos2theta(pL) + wR * cos2theta(pR)) / denom : 0.0;
    }
    else
    {
        const double c = gain(pan);
        const double c2 = c * c;
        const double th = thetaOf(pan);
        const double cs = std::cos(th);
        const double sn = std::sin(th);
        e.lCoef = c2 * cs * cs * t.z;
        e.rCoef = c2 * sn * sn * t.z;
        e.lever = cs * cs - sn * sn; // cos(2θ)
    }
    return e;
}

// D(u=0) 对给定 pans 的快速计算(§6.4 step 3 二分用)。
double computeD0(const std::vector<TrackMeta>& tracks, const std::vector<double>& pans, const BalanceConfig& cfg)
{
    double zL = 0.0;
    double zR = 0.0;
    for (std::size_t i = 0; i < tracks.size(); ++i)
    {
        const TrackEnergy e = buildTrackEnergy(tracks[i], pans[i], cfg.panCurve);
        zL += e.lCoef;
        zR += e.rCoef;
    }
    zL = std::max(zL, 1e-12);
    zR = std::max(zR, 1e-12);
    return 10.0 * std::log10(zL / zR);
}

// 找最外非 pair 镜像环并对它做 δ 平移(§6.4 step 3 / BAL-8)。
// 找到可平移环 → outPans = 平移后的 pans、outDelta = δ,返回 true;无非 pair 环 → false(直接进 4)。
bool applyDeltaShift(const std::vector<TrackMeta>& tracks, const std::vector<double>& pans, double dBalance,
                     const BalanceConfig& cfg, std::vector<double>& outPans, double& outDelta)
{
    const int n = static_cast<int>(tracks.size());
    std::vector<bool> isFree(static_cast<std::size_t>(n), false);
    for (int i = 0; i < n; ++i)
    {
        const TrackMeta& t = tracks[static_cast<std::size_t>(i)];
        isFree[static_cast<std::size_t>(i)] = t.participateInAutoPan && (t.freeze & 1) == 0 && !t.leadLock;
    }

    // 收集镜像环(pan<0 为左,匹配 pan==−leftPan 的右成员;中心 0 不是镜像环)。
    std::vector<int> leftPos;
    std::vector<int> rightPos;
    std::vector<double> radius;
    std::vector<bool> used(static_cast<std::size_t>(n), false);
    for (int i = 0; i < n; ++i)
    {
        if (!isFree[static_cast<std::size_t>(i)] || used[static_cast<std::size_t>(i)])
            continue;
        const double p = pans[static_cast<std::size_t>(i)];
        if (p >= 0.0)
            continue;
        for (int j = 0; j < n; ++j)
        {
            if (i == j || !isFree[static_cast<std::size_t>(j)] || used[static_cast<std::size_t>(j)])
                continue;
            if (pans[static_cast<std::size_t>(j)] == -p)
            {
                leftPos.push_back(i);
                rightPos.push_back(j);
                radius.push_back(-p);
                used[static_cast<std::size_t>(i)] = true;
                used[static_cast<std::size_t>(j)] = true;
                break;
            }
        }
    }

    // 选最外非 pair 环(pair 占据的镜像环是硬约束,跳过;BAL-8)。
    int chosen = -1;
    for (std::size_t k = 0; k < leftPos.size(); ++k)
    {
        const TrackMeta& tl = tracks[static_cast<std::size_t>(leftPos[k])];
        const TrackMeta& tr = tracks[static_cast<std::size_t>(rightPos[k])];
        if (tl.pairId > 0 && tl.pairId == tr.pairId)
            continue;
        if (chosen < 0 || radius[k] > radius[static_cast<std::size_t>(chosen)])
            chosen = static_cast<int>(k);
    }
    if (chosen < 0)
        return false;

    const int li = leftPos[static_cast<std::size_t>(chosen)];
    const int ri = rightPos[static_cast<std::size_t>(chosen)];
    const double r = radius[static_cast<std::size_t>(chosen)];

    // 整体移向弱侧:D>0 → 左响 → 弱侧=右 → δ>0。
    const double lo = (dBalance > 0.0) ? 0.0 : -cfg.deltaMax;
    const double hi = (dBalance > 0.0) ? cfg.deltaMax : 0.0;

    const auto Dof = [&](double delta) {
        std::vector<double> pp = pans;
        pp[static_cast<std::size_t>(li)] = clampPanD(-r + delta);
        pp[static_cast<std::size_t>(ri)] = clampPanD(r + delta);
        return computeD0(tracks, pp, cfg);
    };

    const double dLo = Dof(lo);
    const double dHi = Dof(hi);
    double delta = 0.0;
    if (dLo * dHi > 0.0)
    {
        // 区间内不穿越 0(D(δ) 单调):取 |D| 较小的端点。
        delta = (std::abs(dLo) < std::abs(dHi)) ? lo : hi;
    }
    else
    {
        // 二分求 D(δ)=0(≤ deltaBinaryIters,步进精度 deltaPrecision)。
        double l = lo;
        double h = hi;
        double dl = dLo;
        for (int i = 0; i < cfg.deltaBinaryIters && (h - l) > cfg.deltaPrecision; ++i)
        {
            const double mid = (l + h) / 2.0;
            const double dm = Dof(mid);
            if ((dl > 0.0) == (dm > 0.0))
            {
                l = mid;
                dl = dm;
            }
            else
            {
                h = mid;
            }
        }
        delta = (l + h) / 2.0;
    }

    outPans = pans;
    outPans[static_cast<std::size_t>(li)] = clampPanD(-r + delta);
    outPans[static_cast<std::size_t>(ri)] = clampPanD(r + delta);
    outDelta = delta;
    return true;
}

} // namespace

BalanceResult solveBalance(const std::vector<TrackMeta>& tracks, const std::vector<double>& pans,
                           const BalanceConfig& cfg)
{
    BalanceResult res;
    const int n = static_cast<int>(tracks.size());
    res.uBalance.assign(static_cast<std::size_t>(n), 0.0);
    res.u.assign(static_cast<std::size_t>(n), 0.0);
    res.pans = pans;

    std::vector<bool> adjustable(static_cast<std::size_t>(n), false);
    std::vector<TrackEnergy> energy(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        const TrackMeta& t = tracks[static_cast<std::size_t>(i)];
        // [J65] freeze bit1:freeze∈{2,3} → vol 维冻结;[J58] lead_vol_exempt 独立、零联动。
        adjustable[static_cast<std::size_t>(i)] = !((t.freeze == 2 || t.freeze == 3) || t.leadVolExempt);
        energy[static_cast<std::size_t>(i)] = buildTrackEnergy(t, pans[static_cast<std::size_t>(i)], cfg.panCurve);
    }

    const auto compute = [&](const std::vector<double>& u, double& zL, double& zR) {
        zL = 0.0;
        zR = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double g2 = std::pow(10.0, u[static_cast<std::size_t>(i)] / 10.0);
            zL += g2 * energy[static_cast<std::size_t>(i)].lCoef;
            zR += g2 * energy[static_cast<std::size_t>(i)].rCoef;
        }
        // 数值守卫(02 §6.2):floor 1e−12,防 log10 除零/±Inf/NaN 进入迭代与 reason 文案(BAL-7)。
        zL = std::max(zL, 1e-12);
        zR = std::max(zR, 1e-12);
    };

    double zL0 = 0.0;
    double zR0 = 0.0;
    compute(res.u, zL0, zR0);
    const double lTarget = 10.0 * std::log10(zL0 + zR0);
    res.d0 = 10.0 * std::log10(zL0 / zR0);

    double D = res.d0;
    res.iters = 0;
    res.capped = false;
    for (int it = 0; it < cfg.maxIters; ++it)
    {
        ++res.iters;
        if (std::abs(D) < cfg.tol)
            break;
        for (int i = 0; i < n; ++i)
        {
            if (!adjustable[static_cast<std::size_t>(i)])
                continue;
            const double v = res.uBalance[static_cast<std::size_t>(i)] -
                             (D / 2.0) * energy[static_cast<std::size_t>(i)].lever * cfg.eta;
            res.uBalance[static_cast<std::size_t>(i)] = std::clamp(v, -cfg.uMax, cfg.uMax);
            if (res.uBalance[static_cast<std::size_t>(i)] <= -cfg.uMax ||
                res.uBalance[static_cast<std::size_t>(i)] >= cfg.uMax)
                res.capped = true;
        }
        double zL = 0.0;
        double zR = 0.0;
        compute(res.uBalance, zL, zR);
        D = 10.0 * std::log10(zL / zR);
    }
    res.dBalance = D;

    // 共模修正(02 §6.2):恢复区间总响度。仅全可调时严格不影响 D(BAL-5)。
    double zLb = 0.0;
    double zRb = 0.0;
    compute(res.uBalance, zLb, zRb);
    res.delta = lTarget - 10.0 * std::log10(zLb + zRb);

    res.u = res.uBalance;
    for (int i = 0; i < n; ++i)
    {
        if (!adjustable[static_cast<std::size_t>(i)])
            continue;
        res.u[static_cast<std::size_t>(i)] =
            std::clamp(res.uBalance[static_cast<std::size_t>(i)] + res.delta, -cfg.uMax, cfg.uMax);
    }

    // 软窗截断(02 §6.2):u 落 vol_soft_window [−18,+9],超出截断 + warning。
    for (int i = 0; i < n; ++i)
    {
        if (!adjustable[static_cast<std::size_t>(i)])
            continue;
        if (res.u[static_cast<std::size_t>(i)] < cfg.volSoftMin || res.u[static_cast<std::size_t>(i)] > cfg.volSoftMax)
        {
            res.u[static_cast<std::size_t>(i)] =
                std::clamp(res.u[static_cast<std::size_t>(i)], cfg.volSoftMin, cfg.volSoftMax);
            res.warnings.push_back("已达音量修正上限");
        }
    }

    double zLf = 0.0;
    double zRf = 0.0;
    compute(res.u, zLf, zRf);
    res.dFinal = 10.0 * std::log10(zLf / zRf);
    res.converged = std::abs(res.dFinal) < cfg.tol;
    return res;
}

BalanceResult solveBalanceWithFallback(const std::vector<TrackMeta>& tracks, const AutoAssignConfig& assignCfg,
                                       const BalanceConfig& balanceCfg)
{
    std::vector<int> path;

    // level 1:首趟指派 + solveBalance(§6.4 step 1)。
    const AssignResult asn1 = assignInterval(tracks, assignCfg);
    BalanceResult res = solveBalance(tracks, asn1.pans, balanceCfg);
    path.push_back(1);
    if (res.converged)
    {
        res.fallbackLevel = 1;
        res.fallbackPath = path;
        return res;
    }

    // level 2:第二趟平衡感知指派(§5.3)+ 重新 solveBalance(§6.4 step 2)。
    path.push_back(2);
    double zSum = 0.0;
    for (const TrackMeta& t : tracks)
        zSum += t.z;
    BalanceAwareHint hint;
    hint.signD0 = (res.d0 >= 0.0) ? 1.0 : -1.0;
    hint.zHat.assign(tracks.size(), 0.0);
    for (std::size_t i = 0; i < tracks.size(); ++i)
        hint.zHat[i] = (zSum > 0.0) ? tracks[i].z / zSum : 0.0;
    const AssignResult asn2 = assignInterval(tracks, assignCfg, &hint);
    res = solveBalance(tracks, asn2.pans, balanceCfg);
    if (res.converged)
    {
        res.fallbackLevel = 2;
        res.fallbackPath = path;
        return res;
    }

    // level 3:最外非 pair 环 δ 平移(§6.4 step 3;BAL-8 跳过 pair 环)。
    path.push_back(3);
    std::vector<double> shiftedPans;
    double delta = 0.0;
    if (applyDeltaShift(tracks, asn2.pans, res.dBalance, balanceCfg, shiftedPans, delta))
    {
        res = solveBalance(tracks, shiftedPans, balanceCfg);
        if (res.converged)
        {
            res.fallbackLevel = 3;
            res.fallbackPath = path;
            return res;
        }
    }

    // level 4:无杠杆可用 → UI 警告,保留当前解(§6.4 step 4)。
    path.push_back(4);
    res.warnings.push_back("素材左右能量不对称,无法自动平衡");
    res.fallbackLevel = 4;
    res.fallbackPath = path;
    return res;
}

} // namespace scvb::analysis
