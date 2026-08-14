// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/Hungarian.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace scvb::analysis
{

// 自研 Kuhn–Munkres(势法,O(n³),n≤15 [J59])。采用 e-maxx 的经典增广路径实现,
// 先把矩形矩阵补成方阵(虚拟行列代价 0),再跑 n×n 标准匈牙利。
// 结果 totalCost 直接由「最终指派 + 原矩阵元素」累加,不取对偶势 -v[0],
// 从而对整数输入得到精确整数和、对浮点输入避免势累加的舍入漂移。
HungarianResult solveHungarian(const std::vector<std::vector<double>>& cost)
{
    HungarianResult result;
    const int rows = static_cast<int>(cost.size());
    if (rows == 0)
        return result;
    const int cols = static_cast<int>(cost[0].size());
    for (const auto& r : cost)
    {
        if (static_cast<int>(r.size()) != cols)
            return result; // 非矩形:契约要求调用方保证矩形。
    }
    if (cols == 0)
        return result;

    const int n = std::max(rows, cols);

    // a[1..n][1..n],虚拟行/列代价 0。
    std::vector<std::vector<double>> a(static_cast<std::size_t>(n) + 1,
                                       std::vector<double>(static_cast<std::size_t>(n) + 1, 0.0));
    for (int i = 0; i < rows; ++i)
    {
        for (int j = 0; j < cols; ++j)
            a[static_cast<std::size_t>(i) + 1][static_cast<std::size_t>(j) + 1] =
                cost[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
    }

    const double inf = std::numeric_limits<double>::infinity();

    std::vector<double> u(static_cast<std::size_t>(n) + 1, 0.0); // 行势
    std::vector<double> v(static_cast<std::size_t>(n) + 1, 0.0); // 列势
    std::vector<int> p(static_cast<std::size_t>(n) + 1, 0); // p[j] = 列 j 当前匹配的行(0=未匹配)
    std::vector<int> way(static_cast<std::size_t>(n) + 1, 0); // way[j] = 增广路径上前驱列

    for (int i = 1; i <= n; ++i)
    {
        p[0] = i; // 从行 i 出发增广
        int j0 = 0;
        std::vector<double> minv(static_cast<std::size_t>(n) + 1, inf);
        std::vector<bool> used(static_cast<std::size_t>(n) + 1, false);
        do
        {
            used[static_cast<std::size_t>(j0)] = true;
            const int i0 = p[static_cast<std::size_t>(j0)];
            double delta = inf;
            int j1 = -1;
            for (int j = 1; j <= n; ++j)
            {
                if (used[static_cast<std::size_t>(j)])
                    continue;
                const double cur = a[static_cast<std::size_t>(i0)][static_cast<std::size_t>(j)] -
                                   u[static_cast<std::size_t>(i0)] - v[static_cast<std::size_t>(j)];
                if (cur < minv[static_cast<std::size_t>(j)])
                {
                    minv[static_cast<std::size_t>(j)] = cur;
                    way[static_cast<std::size_t>(j)] = j0;
                }
                if (minv[static_cast<std::size_t>(j)] < delta)
                {
                    delta = minv[static_cast<std::size_t>(j)];
                    j1 = j;
                }
            }
            for (int j = 0; j <= n; ++j)
            {
                if (used[static_cast<std::size_t>(j)])
                {
                    u[static_cast<std::size_t>(p[static_cast<std::size_t>(j)])] += delta;
                    v[static_cast<std::size_t>(j)] -= delta;
                }
                else
                {
                    minv[static_cast<std::size_t>(j)] -= delta;
                }
            }
            j0 = j1;
        } while (p[static_cast<std::size_t>(j0)] != 0);

        do
        {
            const int j1 = way[static_cast<std::size_t>(j0)];
            p[static_cast<std::size_t>(j0)] = p[static_cast<std::size_t>(j1)];
            j0 = j1;
        } while (j0 != 0);
    }

    // 反查:colOfRow[行] = 该行分到的列(1 基)。
    std::vector<int> colOfRow(static_cast<std::size_t>(n) + 1, -1);
    for (int j = 1; j <= n; ++j)
    {
        const int row = p[static_cast<std::size_t>(j)];
        if (row >= 1 && row <= n)
            colOfRow[static_cast<std::size_t>(row)] = j;
    }

    result.rowToCol.assign(static_cast<std::size_t>(rows), -1);
    for (int i = 0; i < rows; ++i)
    {
        const int c = colOfRow[static_cast<std::size_t>(i) + 1];
        if (c >= 1 && c <= cols) // c > cols 仅当 rows > cols(虚拟列)→ 保持 -1。
            result.rowToCol[static_cast<std::size_t>(i)] = c - 1;
    }

    result.totalCost = 0.0;
    for (int i = 0; i < rows; ++i)
    {
        const int c = result.rowToCol[static_cast<std::size_t>(i)];
        if (c >= 0)
            result.totalCost += cost[static_cast<std::size_t>(i)][static_cast<std::size_t>(c)];
    }
    result.ok = true;
    return result;
}

} // namespace scvb::analysis
