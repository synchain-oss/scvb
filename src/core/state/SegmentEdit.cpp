// SPDX-License-Identifier: GPL-3.0-or-later
#include "SegmentEdit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace scvb::state
{

namespace
{
// 值/边界编辑后置:origin=user_edited 且 locked=true([J34]/[J44])。
std::uint32_t userEditedLockedFlags() noexcept
{
    return makeSegmentFlags(SegmentOrigin::UserEdited, true);
}
} // namespace

SegmentEditResult editTrackSegments(std::vector<Segment>& segments, const SegmentEditArgs& args)
{
    const int n = static_cast<int>(segments.size());
    const bool idxOk = args.segIdx >= 0 && args.segIdx < n;

    switch (args.op)
    {
    case SegmentEditOp::SetLocked: {
        if (!idxOk)
            return SegmentEditResult::BadArg;
        Segment& s = segments[static_cast<std::size_t>(args.segIdx)];
        // set_locked 单纯切换 locked,不改 origin(契约 §5.4)。
        s.flags = makeSegmentFlags(segmentOrigin(s.flags), args.locked);
        return SegmentEditResult::Ok;
    }

    case SegmentEditOp::SetValues: {
        // 两字段均可选、至少给一个(契约 §5.4)。
        if (!idxOk || (!args.hasPan && !args.hasVol))
            return SegmentEditResult::BadArg;
        Segment& s = segments[static_cast<std::size_t>(args.segIdx)];
        if (args.hasPan)
            s.pan = args.pan;
        if (args.hasVol)
            s.volDb = args.volDb;
        s.flags = userEditedLockedFlags();
        return SegmentEditResult::Ok;
    }

    case SegmentEditOp::Split: {
        if (!idxOk)
            return SegmentEditResult::BadArg;
        const Segment& s = segments[static_cast<std::size_t>(args.segIdx)];
        if (args.tSamples <= s.t0 || args.tSamples >= s.t1)
            return SegmentEditResult::BadArg; // 切点必须严格落在段内

        Segment a = s;
        Segment b = s;
        a.t1 = args.tSamples;
        b.t0 = args.tSamples;
        a.flags = userEditedLockedFlags();
        b.flags = userEditedLockedFlags(); // 两子段继承原值(pan/volDb),origin=user_edited+locked

        segments[static_cast<std::size_t>(args.segIdx)] = a;
        segments.insert(segments.begin() + args.segIdx + 1, b);
        return SegmentEditResult::Ok;
    }

    case SegmentEditOp::MoveBoundary: {
        if (!idxOk)
            return SegmentEditResult::BadArg;
        const Segment& s = segments[static_cast<std::size_t>(args.segIdx)];

        if (args.edgeIsT0)
        {
            // 移 t0 边:prev 段随之收缩(prev.t1 = tS)。约束 prev.t0 < tS < s.t1。
            if (args.tSamples >= s.t1)
                return SegmentEditResult::BadArg;
            if (args.segIdx > 0)
            {
                const Segment& prev = segments[static_cast<std::size_t>(args.segIdx - 1)];
                if (args.tSamples <= prev.t0)
                    return SegmentEditResult::BadArg;
            }
            segments[static_cast<std::size_t>(args.segIdx)].t0 = args.tSamples;
            if (args.segIdx > 0)
                segments[static_cast<std::size_t>(args.segIdx - 1)].t1 = args.tSamples;
        }
        else
        {
            // 移 t1 边:next 段随之收缩(next.t0 = tS)。约束 s.t0 < tS < next.t1。
            if (args.tSamples <= s.t0)
                return SegmentEditResult::BadArg;
            if (args.segIdx + 1 < n)
            {
                const Segment& next = segments[static_cast<std::size_t>(args.segIdx + 1)];
                if (args.tSamples >= next.t1)
                    return SegmentEditResult::BadArg;
            }
            segments[static_cast<std::size_t>(args.segIdx)].t1 = args.tSamples;
            if (args.segIdx + 1 < n)
                segments[static_cast<std::size_t>(args.segIdx + 1)].t0 = args.tSamples;
        }
        segments[static_cast<std::size_t>(args.segIdx)].flags = userEditedLockedFlags();
        return SegmentEditResult::Ok;
    }

    case SegmentEditOp::Merge: {
        const int a = args.segIdx;
        const int b = args.segIdxB;
        if (a < 0 || a >= n || b < 0 || b >= n)
            return SegmentEditResult::BadArg;
        if (std::abs(a - b) != 1)
            return SegmentEditResult::NotAdjacent; // 两段不相邻

        const int lo = std::min(a, b);
        const int hi = std::max(a, b);
        const Segment& slo = segments[static_cast<std::size_t>(lo)];
        const Segment& shi = segments[static_cast<std::size_t>(hi)];

        // 合并值按时长加权(契约 §5.4;02/04 定细则)。
        const double lenLo = static_cast<double>(slo.t1 - slo.t0);
        const double lenHi = static_cast<double>(shi.t1 - shi.t0);
        const double total = lenLo + lenHi;

        Segment merged;
        merged.t0 = slo.t0;
        merged.t1 = shi.t1;
        if (total > 0.0)
        {
            merged.pan = static_cast<float>((slo.pan * lenLo + shi.pan * lenHi) / total);
            merged.volDb = static_cast<float>((slo.volDb * lenLo + shi.volDb * lenHi) / total);
        }
        else
        {
            merged.pan = slo.pan;
            merged.volDb = slo.volDb;
        }
        merged.flags = userEditedLockedFlags(); // 合并属边界编辑 → user_edited+locked

        segments[static_cast<std::size_t>(lo)] = merged;
        segments.erase(segments.begin() + hi);
        return SegmentEditResult::Ok;
    }
    }

    return SegmentEditResult::BadArg;
}

} // namespace scvb::state
