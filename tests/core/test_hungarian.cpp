// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "analysis/Hungarian.h"

namespace
{

using scvb::analysis::HungarianResult;
using scvb::analysis::solveHungarian;

// 暴力枚举 n! 指派求最小代价(HUNG-2 参考真源)。
double bruteForceMin(const std::vector<std::vector<double>>& cost)
{
    const int n = static_cast<int>(cost.size());
    std::vector<int> perm(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        perm[static_cast<std::size_t>(i)] = i;
    double best = std::numeric_limits<double>::infinity();
    do
    {
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
            sum += cost[static_cast<std::size_t>(i)][static_cast<std::size_t>(perm[static_cast<std::size_t>(i)])];
        if (sum < best)
            best = sum;
    } while (std::next_permutation(perm.begin(), perm.end()));
    return best;
}

// 固定 LCG(Numerical Recipes),种子 0xC0FFEE;HUNG-2 用。
uint32_t lcg(uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return state;
}

} // namespace

TEST_CASE("HUNG-1: 3×3 已知矩阵 → 指派 (0→1)(1→0)(2→2),总代价 5", "[hungarian]")
{
    const std::vector<std::vector<double>> cost = {
        {4.0, 1.0, 3.0},
        {2.0, 0.0, 5.0},
        {3.0, 2.0, 2.0},
    };

    const HungarianResult hr = solveHungarian(cost);

    REQUIRE(hr.ok);
    REQUIRE(hr.rowToCol.size() == 3);
    REQUIRE(hr.rowToCol[0] == 1);
    REQUIRE(hr.rowToCol[1] == 0);
    REQUIRE(hr.rowToCol[2] == 2);
    REQUIRE(hr.totalCost == 5.0);
}

TEST_CASE("HUNG-2: n=8 随机矩阵(LCG 0xC0FFEE,100 组)与暴力枚举一致", "[hungarian]")
{
    uint32_t state = 0xC0FFEEu;
    for (int group = 0; group < 100; ++group)
    {
        std::vector<std::vector<double>> cost(8, std::vector<double>(8, 0.0));
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j)
                cost[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = static_cast<double>(lcg(state) % 100u);

        const HungarianResult hr = solveHungarian(cost);
        const double ref = bruteForceMin(cost);

        INFO("group " << group);
        REQUIRE(hr.ok);
        REQUIRE(hr.totalCost == ref);

        // 指派合法:每行恰一列、每列至多一行。
        std::vector<bool> colUsed(8, false);
        for (int i = 0; i < 8; ++i)
        {
            REQUIRE(hr.rowToCol[static_cast<std::size_t>(i)] >= 0);
            const int c = hr.rowToCol[static_cast<std::size_t>(i)];
            REQUIRE_FALSE(colUsed[static_cast<std::size_t>(c)]);
            colUsed[static_cast<std::size_t>(c)] = true;
        }

        // 确定性:同矩阵复跑逐位一致。
        const HungarianResult hr2 = solveHungarian(cost);
        REQUIRE(hr2.rowToCol == hr.rowToCol);
        REQUIRE(hr2.totalCost == hr.totalCost);
    }
}

TEST_CASE("HUNG-rect: 矩形矩阵(rows<cols 补虚拟行 / rows>cols 补虚拟列)", "[hungarian]")
{
    // rows<cols:3 行 5 列 → 3 真实行全被指派到真实列,2 列留空。
    const std::vector<std::vector<double>> wide = {
        {1.0, 5.0, 2.0, 9.0, 3.0},
        {4.0, 1.0, 8.0, 2.0, 6.0},
        {2.0, 3.0, 1.0, 7.0, 4.0},
    };
    const HungarianResult hw = solveHungarian(wide);
    REQUIRE(hw.ok);
    REQUIRE(hw.rowToCol.size() == 3);
    for (const int c : hw.rowToCol)
        REQUIRE(c >= 0);
    REQUIRE(hw.totalCost == 3.0); // 0→0(1)、1→1(1)、2→2(1),最小总代价 3

    // rows>cols:5 行 3 列 → 3 行被指派,2 行记 -1(虚拟列)。
    const std::vector<std::vector<double>> tall = {
        {1.0, 5.0, 2.0}, {4.0, 1.0, 8.0}, {2.0, 3.0, 1.0}, {9.0, 2.0, 7.0}, {3.0, 6.0, 4.0},
    };
    const HungarianResult ht = solveHungarian(tall);
    REQUIRE(ht.ok);
    REQUIRE(ht.rowToCol.size() == 5);
    int assigned = 0;
    for (const int c : ht.rowToCol)
        if (c >= 0)
            ++assigned;
    REQUIRE(assigned == 3);
}
