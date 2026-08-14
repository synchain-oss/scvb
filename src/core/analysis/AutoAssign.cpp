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

// ε 平局确定性(02 §5.3):使「channel 序小者取排序靠前槽(先外后左)」成为唯一最优。
// 注:规格字面公式 ε=1e-6·(i·S+j) 在完美匹配下求和恒等(Σσ(i)=常数),无法区分总代价
// 相等的指派;此处改以 i·j 交叉项构成 Monge 扰动(低序→低序为唯一极小)。扰动上界
// ≈1e-6·14·14≈2e-4,远小于任何真实代价差(w_prio 最小 ~0.01、w_cont 最小 ~0.02),
// 只在平局时起作用,不覆盖真实最优。
double epsilonOf(int channelIndex, int S, int slotIndex, const AutoAssignConfig& cfg)
{
    (void)S; // 保留规格签名;平局方向由交叉项承载。
    return -cfg.epsilonBase * static_cast<double>(channelIndex) * static_cast<double>(slotIndex);
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

// 矩阵项 = 基础代价 + ε(进入匈牙利,用于确定性平局)。
double entryCost(const TrackMeta& t, const Slot& s, int K, int S, int slotIndex, const AutoAssignConfig& cfg)
{
    return baseCost(t, s, K, cfg) + epsilonOf(t.channelIndex, S, slotIndex, cfg);
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

std::vector<Slot> generateSlots(int n, const AutoAssignConfig& cfg, bool* widthWarningOut)
{
    std::vector<Slot> slots;
    bool warn = false;
    if (n < 0)
        n = 0;
    if (n > 0)
    {
        const int K = (n + 1) / 2; // ceil(n/2)
        const bool odd = (n % 2) == 1;

        std::vector<double> radii; // 镜像半径,外→内。
        if (!odd)
        {
            // 偶数:radii = P_base·(K−k+1)/K,k=1..K(02 §5.2)。
            for (int k = 1; k <= K; ++k)
                radii.push_back(cfg.pBase * static_cast<double>(K - k + 1) / static_cast<double>(K));
        }
        else if (K >= 2)
        {
            // 奇数(方案 A):radii = P_base·(K−k)/(K−1),k=1..K−1,等间距含中心(02 §5.2)。
            for (int k = 1; k <= K - 1; ++k)
                radii.push_back(cfg.pBase * static_cast<double>(K - k) / static_cast<double>(K - 1));
        }

        // 最小间隔保护(§5.2):相邻半径差(含中心 0 与最内半径之差)< minSep → 提示。
        double minGap = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i + 1 < radii.size(); ++i)
            minGap = std::min(minGap, radii[i] - radii[i + 1]);
        if (odd)
            minGap = std::min(minGap, radii.empty() ? std::numeric_limits<double>::infinity() : radii.back());
        if (n >= 2 && minGap < cfg.minSep)
            warn = true; // 原始生成已是「max 半径=P_base 的等间距」,收缩语义由等间距不变量承载。

        // 冻结排序:环外→内;同环先左(−)后右(+);中心最后(02 §5.2)。
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
        if (odd)
        {
            Slot center;
            center.pan = 0.0;
            center.ringRank = K;
            center.isCenter = true;
            slots.push_back(center);
        }
    }
    if (widthWarningOut != nullptr)
        *widthWarningOut = warn;
    return slots;
}

AssignResult assignInterval(const std::vector<TrackMeta>& tracks, const AutoAssignConfig& cfg)
{
    AssignResult result;
    const int numTracks = static_cast<int>(tracks.size());
    result.pans.assign(static_cast<std::size_t>(numTracks), 0.0);
    result.u.assign(static_cast<std::size_t>(numTracks), 0.0);

    // 1. 分类(优先级:leadLock > freeze pan 维 > 非参与 > 自由)。
    // fixed/manual/nonpart 直接落结果,不占槽;只有 free 进入后续指派。
    std::vector<int> freePos;
    for (int i = 0; i < numTracks; ++i)
    {
        const TrackMeta& t = tracks[static_cast<std::size_t>(i)];
        if (t.leadLock)
        {
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
    result.slots = generateSlots(n, cfg, &result.widthWarning);
    if (n == 0)
        return result; // 全锁 / 全 manual / 全非参与:无槽。

    const int S = static_cast<int>(result.slots.size()); // == n
    const int K = (n + 1) / 2;

    // assignedSlotIdx[pos] = 自由轨分到的槽下标(结果;总代价按 baseCost 口径累加)。
    std::vector<int> assignedSlotIdx(static_cast<std::size_t>(numTracks), -1);

    if (pairs.empty())
    {
        // 无 pair → 标准匈牙利(方阵 n×n)。
        const int m = static_cast<int>(freeSinglePos.size());
        std::vector<std::vector<double>> cost(static_cast<std::size_t>(m),
                                              std::vector<double>(static_cast<std::size_t>(S), 0.0));
        for (int i = 0; i < m; ++i)
        {
            const TrackMeta& t = tracks[static_cast<std::size_t>(freeSinglePos[static_cast<std::size_t>(i)])];
            for (int j = 0; j < S; ++j)
                cost[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] =
                    entryCost(t, result.slots[static_cast<std::size_t>(j)], K, S, j, cfg);
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
        const int ringCount = (n % 2 == 1) ? (K - 1) : K; // 镜像环数(奇数分支中心不可被 pair 占)。
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
                    branchCost += entryCost(tracks[static_cast<std::size_t>(sd.first)],
                                            result.slots[static_cast<std::size_t>(li)], K, S, li, cfg);
                    branchCost += entryCost(tracks[static_cast<std::size_t>(sd.second)],
                                            result.slots[static_cast<std::size_t>(ri)], K, S, ri, cfg);
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
                        const TrackMeta& t =
                            tracks[static_cast<std::size_t>(freeSinglePos[static_cast<std::size_t>(i)])];
                        for (std::size_t jj = 0; jj < remSlots.size(); ++jj)
                            cost[static_cast<std::size_t>(i)][jj] = entryCost(
                                t, result.slots[static_cast<std::size_t>(remSlots[jj])], K, S, remSlots[jj], cfg);
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
                const TrackMeta& t = tracks[static_cast<std::size_t>(freeSinglePos[static_cast<std::size_t>(i)])];
                for (std::size_t jj = 0; jj < remSlots.size(); ++jj)
                    cost[static_cast<std::size_t>(i)][jj] =
                        entryCost(t, result.slots[static_cast<std::size_t>(remSlots[jj])], K, S, remSlots[jj], cfg);
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

} // namespace scvb::analysis
