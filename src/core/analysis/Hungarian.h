// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

namespace scvb::analysis
{

// 最小代价指派结果(HUNG-1/2)。对 rows×cols 代价矩阵求 1-1 指派的最小总代价。
struct HungarianResult
{
    std::vector<int> rowToCol; // rowToCol[r] = 第 r 行分到的列(0 基);未分到(补零虚拟列)为 -1。
    double totalCost = 0.0; // 原矩阵已分派元素之和(虚拟行列代价 0,不计入)。
    bool ok = false; // 是否成功(矩阵非空且各维 >0 且矩形)。

    bool assigned(int row) const
    {
        return row >= 0 && row < static_cast<int>(rowToCol.size()) && rowToCol[static_cast<std::size_t>(row)] >= 0;
    }
};

// 矩形最小代价指派(O(n³),n = max(rows,cols)),02-dsp-spec §5.3。cost 允许 rows != cols:
//   rows < cols → 补零虚拟行(虚拟行吸收多余列;真实行全被指派);
//   rows > cols → 补零虚拟列(多余行未指派,rowToCol 记 -1)。
// 所有 cost 必须有限。空矩阵 / 非矩形矩阵返回 ok=false。
HungarianResult solveHungarian(const std::vector<std::vector<double>>& cost);

} // namespace scvb::analysis
