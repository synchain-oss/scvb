// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/FrameStore.h"

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
    // (在 vadP 恒 0 的年代这条路径显不出来;后验一有生产者它就活了 —— 与本卡同批堵上。)
    // O(1),就在已经拿到的页与下标上写,不额外找页。
    page->vadP[idx] = 0;
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
