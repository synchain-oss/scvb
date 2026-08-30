// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/FrameStore.h"

#include <algorithm> // std::min(setVadPosteriorRange / appendRange 的按页推进)
#include <cassert>
#include <cmath>

namespace scvb::analysis
{

namespace
{
int16_t quantizeDbq(double db) noexcept
{
    if (db <= -120.0)
    {
        return kSilenceDbq;
    }
    const double q = db * 100.0;
    if (q >= 32767.0)
    {
        return 32767;
    }
    if (q <= -32768.0)
    {
        return -32768;
    }
    return static_cast<int16_t>(std::llround(q));
}
} // namespace

int16_t quantizeKwDbq(float kw_ms) noexcept
{
    if (!(kw_ms > 0.0f))
    {
        return kSilenceDbq;
    }
    return quantizeDbq(10.0 * std::log10(static_cast<double>(kw_ms)));
}

int16_t quantizePeakDbq(float peak) noexcept
{
    if (!(peak > 0.0f))
    {
        return kSilenceDbq;
    }
    return quantizeDbq(20.0 * std::log10(static_cast<double>(peak)));
}

uint8_t quantizeVadPosterior(float p) noexcept
{
    // ⚠ 与 SL-206 那版逐 hop 写法的**唯一**差别:那边是 `lround(clamped * 255.0f)`(float 乘),
    // 这里先升 double 再乘。极窄的取整临界值上两者可能差 1 —— 对 UI 热图无实际影响
    // (读侧判据只是 `>127`),但「语义逐字相同」严格说要打这个星号,记在这里。
    // NaN 也走这一支(比较全 false)→ 0,不把畸形值放进泳道。
    if (!(p > 0.0f))
    {
        return 0;
    }
    if (p >= 1.0f)
    {
        return 255;
    }
    return static_cast<uint8_t>(std::lround(static_cast<double>(p) * 255.0));
}

float dequantizeKwMs(int16_t dbq) noexcept
{
    if (dbq <= kSilenceDbq)
    {
        return 0.0f;
    }
    return static_cast<float>(std::pow(10.0, (static_cast<double>(dbq) / 100.0) / 10.0));
}

float dequantizePeak(int16_t dbq) noexcept
{
    if (dbq <= kSilenceDbq)
    {
        return 0.0f;
    }
    return static_cast<float>(std::pow(10.0, (static_cast<double>(dbq) / 100.0) / 20.0));
}

FeatPage* ChannelFrames::pageFor(uint64_t hop, bool create)
{
    const uint64_t pageIdx = hop / FeatPage::kHops;
    auto it = pages_.find(pageIdx);
    if (it != pages_.end())
    {
        return it->second.get();
    }
    if (!create)
    {
        return nullptr;
    }
    auto page = std::make_unique<FeatPage>();
    FeatPage* raw = page.get();
    pages_.emplace(pageIdx, std::move(page));
    return raw;
}

const FeatPage* ChannelFrames::pageForRead(uint64_t hop) const
{
    const uint64_t pageIdx = hop / FeatPage::kHops;
    const auto it = pages_.find(pageIdx);
    return (it == pages_.end()) ? nullptr : it->second.get();
}

void ChannelFrames::write(uint64_t hop, float kw_ms, float peak)
{
    if (readOnly_)
    {
        return; // 采集 OFF:release 静默丢弃(不写页、不记账)
    }
    if (hop < gate_.begin || hop >= gate_.end)
    {
        return; // 越 gate(global.range 或布防工作选区):丢弃不记账
    }

    FeatPage* page = pageFor(hop, /*create=*/true);
    const uint32_t idx = static_cast<uint32_t>(hop % FeatPage::kHops);
    // [SL-206] **新特征进来 = 旧判决作废**:vadP 是分析写的后验,与这一 hop 的 kw/peak 同源。
    // 不清的话,「采集 → 分析 → 清除该区间 → 重采一遍别的音频」之后 kw/peak 是新的、vadP 还是
    // 上一份素材的判决,泳道会照着**旧素材**画绿线,直到用户再分析一次。
    //
    // [SL-240] 但这条只在**这一 hop 的数据真的被换掉**时成立,SL-206 当初是无条件清的:
    //   ① 这一 hop **还没有覆盖** —— 新采到的地方,或 `clearCoverage` 打过洞之后的重采
    //      (上面那句话里的「清除该区间」走的正是 `invalidate()`,打完洞覆盖就没了);
    //   ② **重采集布防期**(`vadInvalidateOnWrite_`)—— 用户明说「这段素材要换」,而
    //      §1.23 规定布防**保留既有覆盖**(门控只挡写入),所以 ① 看不见它。
    // 其余情形 = 采集开着、用户又放了一遍**同一段**音频:kw/peak 被同样的值原样覆写,
    // 判决没有作废的理由。无条件清的后果是用户实测的那一幕 —— 「泳道绿线有时有有时没,
    // 而且**播放到哪消失到哪**」(SL-240):FeatRing 的 hop 是时间线序号,重播同一段就是
    // 拿同样的 hop 号再写一遍,于是播放头走到哪,已分析段的绿线就被抹到哪。
    //
    // 「素材换了但没打洞也没布防」那一档不归这里管:它由 04 §4.5 的指纹守望(`channelStale`
    // ⇒ ⚠ 提示用户重采/重分析)负责,那是**提示**而不是**抹掉显示**。
    // 判据本身 O(log n)(n = 覆盖区间数,实测几十),与已经在做的 `coverage_.add` 同阶。
    if (vadInvalidateOnWrite_ || !coverage_.coversFully(HopRange{hop, hop + 1}))
    {
        page->vadP[idx] = 0; // O(1),就在已经拿到的页与下标上写,不额外找页
    }
    page->kw_dBq[idx] = quantizeKwDbq(kw_ms);
    page->peak_dBq[idx] = quantizePeakDbq(peak);
    coverage_.add(HopRange{hop, hop + 1});
}

void ChannelFrames::restoreHop(uint64_t hop, int16_t kwDbq, int16_t peakDbq, uint8_t vad)
{
    // 见头注:刻意不看 readOnly_/gate_,也刻意不记账。
    FeatPage* page = pageFor(hop, /*create=*/true);
    const uint32_t idx = static_cast<uint32_t>(hop % FeatPage::kHops);
    page->kw_dBq[idx] = kwDbq;
    page->peak_dBq[idx] = peakDbq;
    page->vadP[idx] = vad;
}

void ChannelFrames::appendRange(HopRange r, std::vector<int16_t>& kw, std::vector<int16_t>& peak,
                                std::vector<uint8_t>& vad) const
{
    uint64_t h = r.begin;
    while (h < r.end)
    {
        // 本页能覆盖到哪:min(页尾, 段尾)。
        const uint64_t pageStart = (h / FeatPage::kHops) * FeatPage::kHops;
        const uint64_t pageEnd = pageStart + FeatPage::kHops;
        const uint64_t stop = std::min(pageEnd, r.end);
        const FeatPage* page = pageForRead(h); // 每页一次索引查找

        for (uint64_t x = h; x < stop; ++x)
        {
            const uint32_t idx = static_cast<uint32_t>(x % FeatPage::kHops);
            // 页缺失(覆盖记账说有、页却没分配)按静音地板补,与逐 hop 访问器同口径。
            kw.push_back(page ? page->kw_dBq[idx] : kSilenceDbq);
            peak.push_back(page ? page->peak_dBq[idx] : kSilenceDbq);
            vad.push_back(page ? page->vadP[idx] : uint8_t{0});
        }
        h = stop;
    }
}

int16_t ChannelFrames::kwDbq(uint64_t hop) const
{
    const FeatPage* page = pageForRead(hop);
    return page ? page->kw_dBq[hop % FeatPage::kHops] : kSilenceDbq;
}

int16_t ChannelFrames::peakDbq(uint64_t hop) const
{
    const FeatPage* page = pageForRead(hop);
    return page ? page->peak_dBq[hop % FeatPage::kHops] : kSilenceDbq;
}

uint8_t ChannelFrames::vadP(uint64_t hop) const
{
    const FeatPage* page = pageForRead(hop);
    return page ? page->vadP[hop % FeatPage::kHops] : 0;
}

void ChannelFrames::setVadP(uint64_t hop, uint8_t v)
{
    FeatPage* page = pageFor(hop, /*create=*/true);
    page->vadP[hop % FeatPage::kHops] = v;
}

void ChannelFrames::setVadPosteriorRange(HopRange r, const float* posterior, std::size_t count)
{
    if (posterior == nullptr || r.end <= r.begin || count == 0)
    {
        return;
    }
    // 先按**可读元素个数**夹住右端:调用方传宽了不许变成越界读,只许少写几个 hop。
    if (r.end - r.begin > count)
    {
        r.end = r.begin + count;
    }
    // 只走**已覆盖**的子区间:未覆盖处后验恒 0,写进去只会白建 20KB 的页(见头文件注)。
    // intersect 已把区间裁进 r 内。⚠ 区间数**没有硬上限** —— `CoverageMap` 只保证有序 +
    // 相邻合并,不保证条数;「几十」是现场经验量级(= 连续覆盖段数),不是不变量,别据此
    // 做更强的假设。复杂度结论不受影响:仍是 O(重叠区间数 + 实际覆盖 hop 数),与**跨度**无关。
    for (const HopRange seg : coverage_.intersect(r))
    {
        uint64_t h = seg.begin;
        while (h < seg.end)
        {
            // 本页能覆盖到哪:min(页尾, 段尾)—— 与 appendRange 同一把尺子。
            const uint64_t pageStart = (h / FeatPage::kHops) * FeatPage::kHops;
            const uint64_t stop = std::min(pageStart + FeatPage::kHops, seg.end);
            FeatPage* page = pageFor(h, /*create=*/true); // 每页一次索引查找
            for (uint64_t x = h; x < stop; ++x)
            {
                page->vadP[x % FeatPage::kHops] = quantizeVadPosterior(posterior[x - r.begin]);
            }
            h = stop;
        }
    }
}

float ChannelFrames::kwMs(uint64_t hop) const
{
    return dequantizeKwMs(kwDbq(hop));
}

float ChannelFrames::peak(uint64_t hop) const
{
    return dequantizePeak(peakDbq(hop));
}

ChannelFrames& FrameStore::channel(uint32_t ch)
{
    const uint32_t n = static_cast<uint32_t>(channels_.size());
    if (ch >= 1 && ch <= n)
    {
        return channels_[ch - 1];
    }
    // 越界:debug 断言,release 回落只读哨兵(写被丢弃),绝不静默回落 ch1 污染真实轨数据(PR#42)。
    assert(false && "FrameStore::channel: channel out of range");
    return invalidChannel_;
}

const ChannelFrames& FrameStore::channel(uint32_t ch) const
{
    const uint32_t n = static_cast<uint32_t>(channels_.size());
    if (ch >= 1 && ch <= n)
    {
        return channels_[ch - 1];
    }
    assert(false && "FrameStore::channel: channel out of range");
    return invalidChannel_;
}

void FrameStore::reset()
{
    for (auto& c : channels_)
    {
        c.reset();
    }
}

std::size_t FrameStore::totalPageCount() const noexcept
{
    std::size_t n = 0;
    for (const auto& c : channels_)
    {
        n += c.pageCount();
    }
    return n;
}

std::size_t FrameStore::totalAllocatedBytes() const noexcept
{
    std::size_t n = 0;
    for (const auto& c : channels_)
    {
        n += c.allocatedBytes();
    }
    return n;
}

} // namespace scvb::analysis
