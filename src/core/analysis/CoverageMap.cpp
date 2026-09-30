// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/CoverageMap.h"

#include <algorithm>

namespace scvb::analysis
{

namespace
{
// add 用:第一个 end >= x.begin 的区间(含左邻接,便于合并相邻)。
std::vector<HopRange>::iterator lowerMerge(std::vector<HopRange>& r, HopRange x) noexcept
{
    return std::lower_bound(r.begin(), r.end(), x,
                            [](const HopRange& a, const HopRange& b) { return a.end < b.begin; });
}

// punch/intersect 用:第一个 end > x.begin 的区间(仅重叠,不含仅邻接)。
std::vector<HopRange>::iterator lowerOverlap(std::vector<HopRange>& r, HopRange x) noexcept
{
    return std::lower_bound(r.begin(), r.end(), x,
                            [](const HopRange& a, const HopRange& b) { return a.end <= b.begin; });
}

std::vector<HopRange>::const_iterator lowerOverlap(const std::vector<HopRange>& r, HopRange x) noexcept
{
    return std::lower_bound(r.begin(), r.end(), x,
                            [](const HopRange& a, const HopRange& b) { return a.end <= b.begin; });
}
} // namespace

void CoverageMap::add(HopRange x)
{
    if (x.begin >= x.end)
    {
        return;
    }

    auto it = lowerMerge(r_, x);
    if (it == r_.end() || it->begin > x.end)
    {
        r_.insert(it, x);
        return;
    }

    // 吸收所有 overlap/邻接区间,合并为 [lo, hi]。
    uint64_t lo = std::min(it->begin, x.begin);
    uint64_t hi = std::max(it->end, x.end);
    auto jt = std::next(it);
    while (jt != r_.end() && jt->begin <= hi)
    {
        hi = std::max(hi, jt->end);
        ++jt;
    }

    it->begin = lo;
    it->end = hi;
    r_.erase(std::next(it), jt);
}

void CoverageMap::punch(HopRange x)
{
    if (x.begin >= x.end)
    {
        return;
    }

    auto it = lowerOverlap(r_, x);
    while (it != r_.end() && it->begin < x.end)
    {
        const bool leftIn = (it->begin >= x.begin);
        const bool rightIn = (it->end <= x.end);

        if (leftIn && rightIn)
        {
            it = r_.erase(it); // 完全包含 → 删
        }
        else if (!leftIn && rightIn)
        {
            it->end = x.begin; // 跨左边界 → 截断
            ++it;
        }
        else if (leftIn && !rightIn)
        {
            it->begin = x.end; // 跨右边界 → 截断
            ++it;
        }
        else
        {
            // x 完全在区间内 → 一分为二
            const HopRange right{x.end, it->end};
            it->end = x.begin;
            ++it;
            it = r_.insert(it, right);
            ++it;
        }
    }
}

std::vector<HopRange> CoverageMap::intersect(HopRange x) const
{
    std::vector<HopRange> out;
    if (x.begin >= x.end)
    {
        return out;
    }

    auto it = lowerOverlap(r_, x);
    for (; it != r_.end() && it->begin < x.end; ++it)
    {
        const uint64_t lo = std::max(it->begin, x.begin);
        const uint64_t hi = std::min(it->end, x.end);
        if (lo < hi)
        {
            out.push_back(HopRange{lo, hi});
        }
    }
    return out;
}

bool CoverageMap::coversFully(HopRange x) const
{
    if (x.begin >= x.end)
    {
        return true;
    }
    return coveredHops(x) == x.end - x.begin;
}

uint64_t CoverageMap::coveredHops(HopRange x) const
{
    if (x.begin >= x.end)
    {
        return 0;
    }

    uint64_t n = 0;
    auto it = lowerOverlap(r_, x);
    for (; it != r_.end() && it->begin < x.end; ++it)
    {
        const uint64_t lo = std::max(it->begin, x.begin);
        const uint64_t hi = std::min(it->end, x.end);
        if (lo < hi)
        {
            n += hi - lo;
        }
    }
    return n;
}

} // namespace scvb::analysis
