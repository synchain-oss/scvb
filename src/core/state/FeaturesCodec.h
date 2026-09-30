// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// T21:FEAT 节编解码(04 §5.2)——SoA 列式 + int16 量化 + gzip(zlib 格式 RFC 1950,miniz)。
// 纯 C++17,无 JUCE(ADR-011)。容器 TLV 由 T19 StateCodec 承载,本层只编/解码 FEAT payload。
//
// 节内布局(压缩前,little-endian;整节经 gzip 后放入容器的 FEAT chunk 或 sidecar 文件):
//   u32 tag = 'FEAT'(与 StateCodec::kFourccFeat 同值,自识别)
//   u16 codecVer = 1
//   u16 flags   # bit0: embedded(1=内嵌,0=引用 sidecar);bit1: vadPresent([J06] 可选缓存)
//   u32 sampleRate
//   u32 hopMs = 10
//   u8  channelCount
//   -- embedded=1 时,每 channel 依次(SoA 列式,只存覆盖 hop,按 range 顺序串联):
//      u8 channelId; u32 rangeCount; rangeCount × {u64 beginHop; u64 endHop}
//      i16 kwDbq[hopCount]; i16 peakDbq[hopCount];(vadPresent 时)u8 vadPosterior[hopCount]
//   -- embedded=0 时,代之以:
//      char sessionGuid[36]; u8 sha256[32]; u64 sidecarBytes
//
// [J06] vadPosterior 为可选缓存:可由 kw_mean_square 按 ADR-008 离线重算,序列化允许省略
// (flags bit1=0 时省略,加载后逐 channel 为空、由消费方按需重算)。

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scvb::state
{

// ---- FEAT 节常量(04 §5.2)----
inline constexpr std::uint32_t kFeatTag = 0x54414546u; // 'FEAT'(= StateCodec::kFourccFeat)
inline constexpr std::uint16_t kFeatCodecVer = 1u;
inline constexpr std::uint16_t kFeatFlagEmbedded = 1u << 0; // bit0:1=内嵌,0=引用 sidecar
inline constexpr std::uint16_t kFeatFlagVadPresent = 1u << 1; // bit1:1=vadPosterior 已编码,0=省略([J06])
inline constexpr std::uint32_t kDefaultSampleRate = 48000u;
inline constexpr std::uint32_t kDefaultHopMs = 10u;

// 压缩炸弹防护(S4-state.md §7 留 T21):gunzip 输出上限。远高于 20min 满配 ≈10MB(特征环上限)。
inline constexpr std::size_t kMaxDecompressedFeatBytes = 64u * 1024u * 1024u;

// 校验上限(不可信字节:长度/范围字段先校验再分配/索引,CLAUDE.md §7.3)。
inline constexpr std::uint32_t kMaxFeatChannels = 15u; // [J59] 15 轨
inline constexpr std::uint32_t kMaxRangesPerChannel = 1000000u;
inline constexpr std::uint64_t kMaxHopsPerChannel = 100000000ull; // ~278h@10ms,防累计越界

// 采样率 / hop 时长取值域(不可信字节:越界拒解,防消费方除零/换算异常)。
inline constexpr std::uint32_t kMinSampleRate = 8000u;
inline constexpr std::uint32_t kMaxSampleRate = 768000u; // 8..768 kHz,覆盖 44.1k/48k/96k/192k
inline constexpr std::uint32_t kMinHopMs = 1u;
inline constexpr std::uint32_t kMaxHopMs = 1000u; // 1..1000 ms(冻结值 10)

// sessionGuid 校验:严格 36 字符 dashed UUID 形状(hex + 8/13/18/23 位连字符)。
// 用于拒绝对不可信 state 字节解码出的 guid 做路径拼接(防目录穿越,CLAUDE.md §7.3)。
bool isValidSessionGuid(std::string_view guid) noexcept;

// ---- 数据模型 ----
struct HopRange
{
    std::uint64_t begin = 0; // 半开 [begin,end),hop 单位(04 §5.2 coverage_ranges)
    std::uint64_t end = 0;
};

inline bool operator==(const HopRange& a, const HopRange& b) noexcept
{
    return a.begin == b.begin && a.end == b.end;
}

inline bool operator!=(const HopRange& a, const HopRange& b) noexcept
{
    return !(a == b);
}

struct ChannelFeatures
{
    std::uint8_t channelId = 0; // 1..15
    std::vector<HopRange> coverage; // 有序、互不重叠、互不相邻(由 CoverageMap 保证)
    std::vector<std::int16_t> kwDbq; // 0.01 dB 量化(10*log10(kw_ms)*100)
    std::vector<std::int16_t> peakDbq; // 0.01 dB 量化(20*log10(peak)*100)
    std::vector<std::uint8_t> vadPosterior; // [J06] 可选缓存 0..255;空 = 省略(可重算)
};

struct FeaturesData
{
    std::uint32_t sampleRate = kDefaultSampleRate;
    std::uint32_t hopMs = kDefaultHopMs;
    bool vadPresent = true; // [J06]:false = 所有 channel 的 vadPosterior 均省略
    std::vector<ChannelFeatures> channels;
};

// embedded=0 时的 sidecar 引用(04 §5.2)。
struct SidecarRef
{
    std::string sessionGuid; // 36 字符 dashed UUID
    std::array<std::uint8_t, 32> sha256{}; // sidecar 文件(gzip 后字节)的 SHA-256
    std::uint64_t sidecarBytes = 0; // sidecar 文件字节数
};

// 解码结果。
struct FeatDecode
{
    bool ok = false; // true = 成功解码出可用特征/引用
    bool codecVerNewer = false; // true = codecVer > 当前:特征按空处理,但**必须**原样保留原始 chunk 字节,
                                // 绝不可重编码空特征(对齐 T19 StateCodec 三态:Ok/Newer/Corrupt)
    bool embedded = true;
    FeaturesData features; // embedded=1 时有效
    SidecarRef ref; // embedded=0 时有效
};

// ---- gzip(zlib 格式 RFC 1950,miniz;零新依赖,ADR-011)----
// 压缩;输入非法/过大或压缩失败返回空。
std::vector<std::uint8_t> gzipCompress(const void* data, std::size_t size);
// 解压;输出超 maxOutBytes(压缩炸弹)或流损坏/截断返回 false。
bool gzipDecompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out,
                    std::size_t maxOutBytes = kMaxDecompressedFeatBytes);

// ---- FEAT payload 编解码(输入/输出均已 gzip)----
// embedded=1:gzip(FeatSection{... channels ...})。输入非法(如 vad 长度与 hopCount 不符)返回空。
std::vector<std::uint8_t> encodeFeatures(const FeaturesData& data);
// embedded=0:gzip(FeatSection{... sessionGuid/sha256/sidecarBytes ...})。guid 非法(非 dashed UUID)返回空。
std::vector<std::uint8_t> encodeReference(const SidecarRef& ref, std::uint32_t sampleRate, std::uint32_t hopMs,
                                          std::uint8_t channelCount);
// 解码 gzip 后的 FEAT payload。
FeatDecode decodeFeatures(const std::uint8_t* gzData, std::size_t size);

} // namespace scvb::state
