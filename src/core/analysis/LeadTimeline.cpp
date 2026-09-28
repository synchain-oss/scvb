// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/LeadTimeline.h"

#include <algorithm>
#include <iterator>

namespace scvb::analysis
{

bool LeadTimeline::write(std::int64_t t0, std::int64_t t1, int lead)
{
    if (t1 <= t0 || t0 < 0 || lead < 0 || lead > kLeadMaxValue)
    {
        return false;
    }
    // 一次写入最多让段数 +2(把一段从中间劈开再插一段)。到顶就整条丢弃,不做半截写入 ——
    // 半截写入会让「这段谁是主唱」取决于到顶的那一刻,比不记更难解释。
    if (runs_.size() + 2 > kMaxRuns)
    {
        ++dropped_;
        return false;
    }

    // 1) 左侧与 [t0,t1) 相交的那一段:截到 t0;若它越过 t1,右边剩下的那截另立一段。
    auto it = runs_.lower_bound(t0);
    if (it != runs_.begin())
    {
        auto prev = std::prev(it);
        if (prev->second.t1 > t0)
        {
            const Run whole = prev->second;
            prev->second.t1 = t0; // prev->first < t0,截后仍非空
            if (whole.t1 > t1)
            {
                runs_[t1] = Run{whole.t1, whole.lead};
            }
        }
    }
    // 2) 起点落在 [t0,t1) 内的各段:整段被盖掉的删,越过 t1 的留下右截。
    it = runs_.lower_bound(t0);
    while (it != runs_.end() && it->first < t1)
    {
        if (it->second.t1 > t1)
        {
            const Run tail = it->second;
            runs_.erase(it);
            runs_[t1] = tail;
            break;
        }
        it = runs_.erase(it);
    }

    // 3) 与两侧**相接且同值**的段合并(播放是逐块连续写入的,不合并的话一首歌就是几万段)。
    std::int64_t nt0 = t0;
    std::int64_t nt1 = t1;
    if (auto next = runs_.find(t1); next != runs_.end() && next->second.lead == lead)
    {
        nt1 = next->second.t1;
        runs_.erase(next);
    }
    auto after = runs_.lower_bound(t0);
    if (after != runs_.begin())
    {
        auto prev = std::prev(after);
        if (prev->second.t1 == t0 && prev->second.lead == lead)
        {
            nt0 = prev->first;
            runs_.erase(prev);
        }
    }
    runs_[nt0] = Run{nt1, lead};
    return true;
}

std::vector<LeadRun> LeadTimeline::runs() const
{
    std::vector<LeadRun> out;
    out.reserve(runs_.size());
    for (const auto& [t0, r] : runs_)
    {
        out.push_back(LeadRun{t0, r.t1, r.lead});
    }
    return out;
}

std::vector<LeadRun> LeadTimeline::runsOverlapping(std::int64_t t0, std::int64_t t1) const
{
    std::vector<LeadRun> out;
    if (t1 <= t0)
    {
        return out;
    }
    auto it = runs_.lower_bound(t0);
    if (it != runs_.begin())
    {
        auto prev = std::prev(it);
        if (prev->second.t1 > t0)
        {
            it = prev;
        }
    }
    for (; it != runs_.end() && it->first < t1; ++it)
    {
        out.push_back(LeadRun{it->first, it->second.t1, it->second.lead});
    }
    return out;
}

void LeadTimeline::assign(const std::vector<LeadRun>& runs)
{
    runs_.clear();
    for (const auto& r : runs)
    {
        runs_[r.t0] = Run{r.t1, r.lead};
    }
}

int majorityLead(const std::vector<LeadRun>& runs, std::int64_t t0, std::int64_t t1)
{
    if (t1 <= t0)
    {
        return 0;
    }
    std::array<std::int64_t, kLeadMaxValue + 1> covered{};
    for (const auto& r : runs)
    {
        if (r.lead < 0 || r.lead > kLeadMaxValue)
        {
            continue;
        }
        const std::int64_t a = std::max(r.t0, t0);
        const std::int64_t b = std::min(r.t1, t1);
        if (b > a)
        {
            covered[static_cast<std::size_t>(r.lead)] += b - a;
        }
    }
    int best = 0;
    std::int64_t bestCovered = 0;
    for (int v = 0; v <= kLeadMaxValue; ++v)
    {
        // 严格大于:平局保留先遇到的(较小的)值。
        if (covered[static_cast<std::size_t>(v)] > bestCovered)
        {
            bestCovered = covered[static_cast<std::size_t>(v)];
            best = v;
        }
    }
    return best;
}

void LeadRecorder::record(std::int64_t t0, std::int64_t t1, int lead) noexcept
{
    const std::uint32_t w = writePos_.load(std::memory_order_relaxed);
    const std::uint32_t r = readPos_.load(std::memory_order_acquire);
    if (w - r >= kCapacity)
    {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ring_[w & (kCapacity - 1)] = Rec{t0, t1, static_cast<std::int32_t>(lead)};
    writePos_.store(w + 1, std::memory_order_release);
}

std::size_t LeadRecorder::drainInto(LeadTimeline& timeline)
{
    const std::uint32_t w = writePos_.load(std::memory_order_acquire);
    std::uint32_t r = readPos_.load(std::memory_order_relaxed);
    std::size_t n = 0;
    while (r != w)
    {
        const Rec& rec = ring_[r & (kCapacity - 1)];
        (void)timeline.write(rec.t0, rec.t1, rec.lead);
        ++r;
        ++n;
    }
    readPos_.store(r, std::memory_order_release);
    return n;
}

std::size_t LeadRecorder::discard()
{
    const std::uint32_t w = writePos_.load(std::memory_order_acquire);
    const std::uint32_t r = readPos_.load(std::memory_order_relaxed);
    readPos_.store(w, std::memory_order_release);
    return static_cast<std::size_t>(w - r);
}

namespace
{

void putU16(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
}

void putI64(std::vector<std::uint8_t>& out, std::int64_t v)
{
    const auto u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i)
    {
        out.push_back(static_cast<std::uint8_t>((u >> (8 * i)) & 0xFFu));
    }
}

std::uint16_t getU16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t getU32(const std::uint8_t* p)
{
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i)
    {
        v = (v << 8) | p[i];
    }
    return v;
}

std::int64_t getI64(const std::uint8_t* p)
{
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
    {
        v = (v << 8) | p[i];
    }
    return static_cast<std::int64_t>(v);
}

} // namespace

void encodeLeadChunk(const std::vector<LeadRun>& runs, std::vector<std::uint8_t>& out)
{
    out.clear();
    out.reserve(kLeadChunkHeaderBytes + runs.size() * kLeadChunkRecordBytes);
    putU16(out, kLeadChunkMinor);
    putU16(out, 0);
    putU32(out, static_cast<std::uint32_t>(runs.size()));
    for (const auto& r : runs)
    {
        putI64(out, r.t0);
        putI64(out, r.t1);
        putU32(out, static_cast<std::uint32_t>(r.lead));
    }
}

LeadDecodeStatus decodeLeadChunk(const std::uint8_t* data, std::size_t size, std::vector<LeadRun>& out)
{
    out.clear();
    if (data == nullptr || size < kLeadChunkHeaderBytes)
    {
        return LeadDecodeStatus::Malformed;
    }
    const std::uint16_t minor = getU16(data);
    if (minor > kLeadChunkMinor)
    {
        return LeadDecodeStatus::NewerMinor;
    }
    if (minor == 0)
    {
        return LeadDecodeStatus::Malformed;
    }
    const std::uint32_t count = getU32(data + 4);
    // 长度先于分配校验(不可信字节,CLAUDE.md §7.3)。
    if (count > LeadTimeline::kMaxRuns ||
        size != kLeadChunkHeaderBytes + static_cast<std::size_t>(count) * kLeadChunkRecordBytes)
    {
        return LeadDecodeStatus::Malformed;
    }
    std::vector<LeadRun> runs;
    runs.reserve(count);
    std::int64_t prevT1 = 0;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const std::uint8_t* rec = data + kLeadChunkHeaderBytes + static_cast<std::size_t>(i) * kLeadChunkRecordBytes;
        LeadRun r;
        r.t0 = getI64(rec);
        r.t1 = getI64(rec + 8);
        const std::uint32_t lead = getU32(rec + 16);
        // 升序、互不重叠、非空、值域 0..15 —— 任何一条不满足整块不用(段表是整体,不挑着信)。
        if (r.t0 < 0 || r.t1 <= r.t0 || r.t0 < prevT1 || lead > static_cast<std::uint32_t>(kLeadMaxValue))
        {
            return LeadDecodeStatus::Malformed;
        }
        r.lead = static_cast<int>(lead);
        prevT1 = r.t1;
        runs.push_back(r);
    }
    out = std::move(runs);
    return LeadDecodeStatus::Ok;
}

} // namespace scvb::analysis
