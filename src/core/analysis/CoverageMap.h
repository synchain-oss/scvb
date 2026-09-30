// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// CoverageMap —— 特征覆盖区间记账(04 §3.4)。半开区间 [begin,end) 的并集 + 打洞。
// 不变量:r_ 有序、互不重叠、互不相邻(相邻即合并)。复杂度 O(log n + k)(n=区间数,
// k=受影响区间数;实际 n 在几十以内)。

#include <cstdint>
#include <vector>

namespace scvb::analysis
{

struct HopRange
{
    uint64_t begin = 0;
    uint64_t end = 0;
};

inline bool operator==(const HopRange& a, const HopRange& b) noexcept
{
    return a.begin == b.begin && a.end == b.end;
}

inline bool operator!=(const HopRange& a, const HopRange& b) noexcept
{
    return !(a == b);
}

class CoverageMap
{
public:
    // 并入区间 x:吸收所有 overlap/邻接区间后合并插入。
    void add(HopRange x);

    // 打洞:与 x 相交的区间裁剪(完全包含→删;跨边界→截断;被包含→一分为二)。
    void punch(HopRange x);

    // 与 x 相交的区间(裁剪到 x 内)。
    std::vector<HopRange> intersect(HopRange x) const;

    // x 是否被完全覆盖。
    bool coversFully(HopRange x) const;

    // x 内被覆盖的 hop 数(UI 覆盖率 % 分子)。
    uint64_t coveredHops(HopRange x) const;

    const std::vector<HopRange>& ranges() const noexcept { return r_; }
    bool empty() const noexcept { return r_.empty(); }
    void clear() noexcept { r_.clear(); }

private:
    std::vector<HopRange> r_;
};

} // namespace scvb::analysis
