// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// FrameStore —— 特征权威存储(04 §3.1)。按 channel 独立的分页量化存储 + coverage 记账。
// IPC 特征段只是运输(ipc-contract-v0 §3);持久化真身是 Output 侧 FrameStore。分页(4096 hop/页)
// 保证长时间线稀疏覆盖不炸内存。量化即入库:f32 → int16 dB(×100),0.01dB 分辨率。

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <vector>

#include "analysis/CoverageMap.h"

namespace scvb::analysis
{

// 静音地板(04 §3.1):kw_ms<=0 → -120dB = -12000。
inline constexpr int16_t kSilenceDbq = -12000;

// 特征页:4096 hop = 40.96s @10ms,按需分配。5B/hop(kw 2 + peak 2 + vadP 1)。
struct FeatPage
{
    static constexpr uint32_t kHops = 4096;
    std::array<int16_t, kHops> kw_dBq{}; // 10*log10(kw_ms)*100
    std::array<int16_t, kHops> peak_dBq{}; // 20*log10(peak)*100
    std::array<uint8_t, kHops> vadP{}; // VAD 后验 0..255(分析时填,可重算)
};

// 量化/反量化(f32 ↔ int16 dB×100;-120dB 为地板)。
int16_t quantizeKwDbq(float kw_ms) noexcept;
int16_t quantizePeakDbq(float peak) noexcept;
float dequantizeKwMs(int16_t dbq) noexcept;
float dequantizePeak(int16_t dbq) noexcept;

// 单 channel 特征存储(04 §3.1 ChannelFrames)。
class ChannelFrames
{
public:
    void setSampleRate(double sr) noexcept { sampleRate_ = sr; }
    double sampleRate() const noexcept { return sampleRate_; }

    // 采集 OFF → 只读:任何写路径丢弃(release 静默,不记账)。
    void setReadOnly(bool ro) noexcept { readOnly_ = ro; }
    bool readOnly() const noexcept { return readOnly_; }

    // 拉取/记账门控(04 §1.1/§4.2):写入口检查 hop ∈ gate。默认全范围(不门控);
    // 布防期由 Output 换读「工作选区」。
    void setGate(HopRange r) noexcept { gate_ = r; }

    // 整轨作废:清页 + 清覆盖记账。用于 Output **改组**(J66)—— frameStore 按 channel 索引存、
    // 没有 group 维度,不清的话新组的 ch3 会继承旧组 ch3 的 CoverageMap,并把两组的特征并进
    // 同一张表。注意与 invalidate(打洞)不同:那是按区间清覆盖、页留着待覆写。
    void reset()
    {
        pages_.clear();
        coverage_.clear();
    }
    HopRange gate() const noexcept { return gate_; }

    const CoverageMap& coverage() const noexcept { return coverage_; }
    CoverageMap& coverage() noexcept { return coverage_; }

    // 写入口:readOnly 或 hop 越 gate → 静默丢弃(不写页、不记账)。
    void write(uint64_t hop, float kw_ms, float peak);

    // 打洞:coverage_.punch(r)(重采集/清除用;页数据留待后续覆盖)。
    void invalidate(HopRange r) { coverage_.punch(r); }

    bool hasHop(uint64_t hop) const { return coverage_.coversFully(HopRange{hop, hop + 1}); }

    int16_t kwDbq(uint64_t hop) const;
    int16_t peakDbq(uint64_t hop) const;
    uint8_t vadP(uint64_t hop) const;
    void setVadP(uint64_t hop, uint8_t v);
    float kwMs(uint64_t hop) const; // 反量化(供分析流水线)
    float peak(uint64_t hop) const;

    uint64_t coveredHops(HopRange r) const { return coverage_.coveredHops(r); }
    bool coversFully(HopRange r) const { return coverage_.coversFully(r); }

    std::size_t pageCount() const noexcept { return pages_.size(); }
    std::size_t allocatedBytes() const noexcept { return pages_.size() * sizeof(FeatPage); }

private:
    FeatPage* pageFor(uint64_t hop, bool create);
    const FeatPage* pageForRead(uint64_t hop) const;

    double sampleRate_ = 48000.0;
    std::map<uint64_t, std::unique_ptr<FeatPage>> pages_;
    CoverageMap coverage_;
    bool readOnly_ = true; // 采集 OFF/未布防默认只读:Output 布防路径必须先 setReadOnly(false) 才能记账
    HopRange gate_{0, std::numeric_limits<uint64_t>::max()};
};

// 全轨道 FrameStore 容器(04 §3.3 frameStore[ch];ch 为 1..maxChannels)。
class FrameStore
{
public:
    explicit FrameStore(uint32_t maxChannels = 15) : channels_(maxChannels) {}

    ChannelFrames& channel(uint32_t ch);
    const ChannelFrames& channel(uint32_t ch) const;
    uint32_t maxChannels() const noexcept { return static_cast<uint32_t>(channels_.size()); }

    // 全轨作废(改组路径;逐轨 ChannelFrames::reset)。
    void reset();

    std::size_t totalPageCount() const noexcept;
    std::size_t totalAllocatedBytes() const noexcept;

private:
    std::vector<ChannelFrames> channels_;
    ChannelFrames invalidChannel_; // 越界 channel 的只读哨兵:写被丢弃,不污染任何真实轨
};

} // namespace scvb::analysis
