// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/FeaturesSnapshot.h"

namespace scvb::state
{

FeaturesData snapshotFeatures(const analysis::FrameStore& store, std::uint32_t sampleRate, std::uint32_t hopMs)
{
    FeaturesData data;
    data.sampleRate = sampleRate;
    data.hopMs = hopMs;
    data.vadPresent = true; // 我们存的页里 vadP 恒在(分析填过就是真值,没填就是 0)

    const std::uint32_t maxCh = store.maxChannels();
    for (std::uint32_t ch = 1; ch <= maxCh; ++ch)
    {
        const auto& frames = store.channel(ch);
        const auto& ranges = frames.coverage().ranges();
        if (ranges.empty())
        {
            continue; // 该轨没采过:不占 FEAT 体积
        }

        ChannelFeatures cf;
        cf.channelId = static_cast<std::uint8_t>(ch);

        std::uint64_t totalHops = 0;
        for (const auto& r : ranges)
        {
            totalHops += (r.end - r.begin);
        }
        cf.kwDbq.reserve(static_cast<std::size_t>(totalHops));
        cf.peakDbq.reserve(static_cast<std::size_t>(totalHops));
        cf.vadPosterior.reserve(static_cast<std::size_t>(totalHops));

        for (const auto& r : ranges)
        {
            // 列式:按 range 顺序串联(与 FeaturesCodec 头注的节内布局同序)。
            // 按页批量取 —— 逐 hop 三次 map 查找会在保存路径的临界区里烧掉一个量级的时间。
            cf.coverage.push_back(HopRange{r.begin, r.end});
            frames.appendRange(r, cf.kwDbq, cf.peakDbq, cf.vadPosterior);
        }
        data.channels.push_back(std::move(cf));
    }
    return data;
}

void restoreFeatures(const FeaturesData& data, analysis::FrameStore& store)
{
    const std::uint32_t maxCh = store.maxChannels();

    // 先整店清空:加载工程 = 换一份数据。只清 data 里出现过的轨是不够的 —— 上一个工程采过、
    // 这个工程没采的轨会留着旧覆盖,泳道上就是一条属于别的工程的幽灵波形。
    store.reset();

    for (const auto& cf : data.channels)
    {
        const std::uint32_t ch = cf.channelId;
        if (ch < 1 || ch > maxCh)
        {
            continue; // 不可信字节:越界 channelId 静默跳过(§7.3),绝不回落到 ch1
        }
        auto& frames = store.channel(ch);
        frames.setSampleRate(static_cast<double>(data.sampleRate));

        // 逐 range 串联消费列数据。解码侧已校验过长度自洽,这里再守一次:样本不够就停,
        // 宁可少回灌几个 hop,也不越界读。
        std::size_t cursor = 0;
        const std::size_t n = cf.kwDbq.size();
        // decodeFeatures 已保证 peak 与 kw 等长,这一位实际恒真 —— 留作恒真式防御,
        // 因为本函数也直接受 harness/单测构造的 FeaturesData(那条路没有解码器把关)。
        const bool havePeak = cf.peakDbq.size() == n;
        const bool haveVad = cf.vadPosterior.size() == n; // [J06] 省略时为空,按 0 回灌

        for (const auto& r : cf.coverage)
        {
            if (r.end <= r.begin)
            {
                continue;
            }
            const std::uint64_t span = r.end - r.begin;
            if (cursor + static_cast<std::size_t>(span) > n)
            {
                break; // 列数据不足以铺满这一段:停,不写半段也不越界
            }
            for (std::uint64_t h = r.begin; h < r.end; ++h, ++cursor)
            {
                frames.restoreHop(h, cf.kwDbq[cursor], havePeak ? cf.peakDbq[cursor] : analysis::kSilenceDbq,
                                  haveVad ? cf.vadPosterior[cursor] : std::uint8_t{0});
            }
            frames.addCoverage(analysis::HopRange{r.begin, r.end});
        }
    }
}

} // namespace scvb::state
