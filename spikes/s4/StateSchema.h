// SPDX-License-Identifier: GPL-3.0-or-later
// S4 spike:spike 自有 state 数据模型与二进制布局常量。
//   ⚠ 这是 spike 自有结构(03 §6.1 / 04 §5.2 建议格式的 spike 化落地),不是冻结契约
//     docs/STATE_SCHEMA.md;不改正式 schema、不写正式迁移。正式 schema 由后续正式任务落地,
//     本目录由 T19 的 PR 删除(J16)。

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scvb::s4
{

// ---- 容器常量(03 §6.1 建议格式)----
inline constexpr std::uint32_t kContainerMagic = 0x53435642u; // 'SCVB'
inline constexpr std::uint32_t kCurrentAbi = 1u;
inline constexpr std::uint32_t kContainerFlagsDefault = 0u;

inline constexpr std::uint32_t kFourccPrms = 0x534D5250u; // 'PRMS'
inline constexpr std::uint32_t kFourccCfgs = 0x53474643u; // 'CFGS'
inline constexpr std::uint32_t kFourccCrvs = 0x53565243u; // 'CRVS'
inline constexpr std::uint32_t kFourccFeat = 0x54414546u; // 'FEAT'

// ---- CRVS(03 §6.1)----
inline constexpr std::uint16_t kCrvsMinorVersion = 1u;

// ---- FEAT(04 §5.2)----
inline constexpr std::uint16_t kFeatCodecVer = 1u;
inline constexpr std::uint16_t kFeatFlagEmbedded = 1u << 0;
inline constexpr std::uint32_t kDefaultSampleRate = 48000u;
inline constexpr std::uint32_t kDefaultHopMs = 10u;

// ---- 持久化分层阈值(ADR-007 冻结:压缩后 >8MB 转 sidecar)----
inline constexpr std::uint64_t kSidecarThresholdBytes = 8ull * 1024u * 1024u;

// ---- 满配口径(J59/J57)----
inline constexpr std::size_t kTrackCount = 15u;
inline constexpr std::size_t kVersionCount = 2u;
inline constexpr std::size_t kParamCount = 123u;

// ---- 数据模型 ----
struct Range
{
    std::uint64_t beginHop = 0; // coverage_ranges(hop 单位,04 §5.2)
    std::uint64_t endHop = 0;
};

struct SampleRange
{
    std::int64_t t0 = 0; // excluded_ranges(sample 单位,03 §6.1)
    std::int64_t t1 = 0;
};

struct CurveSegment
{
    std::int64_t t0 = 0; // samples
    std::int64_t t1 = 0; // samples
    float pan = 0.0f;
    float volDb = 0.0f;
    std::uint32_t flags = 0; // bit0-1 origin, bit2 locked(J34)
};

struct PanCurvePoint
{
    float angle = 0.0f; // -100..+100
    float gainDb = 0.0f;
    std::uint32_t shape = 0; // 0=bell 1=shelf 2=cut(J07)
    float q = 1.0f;
    std::uint32_t side = 0; // 0=out 1=left 2=right(J07)
};

struct PanCurve
{
    std::vector<PanCurvePoint> points;
};

struct TrackCurves
{
    std::vector<CurveSegment> segments;
    std::vector<SampleRange> excludedRanges;
};

struct VersionCurves
{
    std::string name;
    std::vector<TrackCurves> tracks; // kTrackCount
    PanCurve panCurve;
};

struct GlobalConfig
{
    bool captureEnabled = false;
    bool outputEnabled = false;
    std::uint32_t versionActive = 1;
    std::uint32_t rangeMode = 0; // 0=follow 1=daw_loop 2=manual(J04)
    float rangeStartS = 0.0f;
    float rangeEndS = 0.0f;
};

struct AnalysisConfig
{
    float vadThresholdDb = -50.0f;
    float vadHysteresisDb = 6.0f;
    std::uint32_t vadHangoverMs = 400;
    std::uint32_t vadPaddingPreMs = 120;
    std::uint32_t vadPaddingPostMs = 200;
    std::uint32_t segmentationMode = 0;
    std::uint32_t segmentationSensitivity = 0;
    std::uint32_t segmentationMinSegmentMs = 250;
    std::uint32_t transitionRampMs = 80;
};

struct ChannelConfig
{
    bool enabled = true;
    std::string label;
    std::uint32_t sourceChannels = 1; // 1|2(J57)
    bool participateInAutoPan = true; // J60:stereo false / mono true
    std::uint32_t priority = 5;
    bool leadLock = false;
    bool leadVolExempt = false;
    std::uint32_t pairId = 0; // 0=无,1..7
};

struct UnknownChunk
{
    std::uint32_t fourcc = 0; // 未知 fourcc:load 时原样保留,save 时原样回写(03 §6.1 前向兼容)
    std::vector<std::uint8_t> payload;
};

struct ChannelFeatures
{
    std::uint8_t channelId = 0;
    std::vector<Range> coverage;
    std::vector<std::int16_t> kwDbq; // 0.01 dB 量化
    std::vector<std::int16_t> peakDbq; // 0.01 dB 量化
    std::vector<std::uint8_t> vadPosterior; // 0..255
};

struct UiState
{
    float scale = 1.0f;
    std::uint32_t language = 0;
    std::uint32_t activeTab = 0;
    bool guideSeen = false; // J50
    bool tourSeen = false; // J62
};

struct FullState
{
    std::uint32_t abi = kCurrentAbi;
    std::string sessionGuid; // 空=未生成
    std::uint32_t groupId = 1; // J66,1..8
    GlobalConfig global;
    AnalysisConfig analysis;
    std::vector<ChannelConfig> channels; // kTrackCount
    std::vector<VersionCurves> versions; // kVersionCount
    std::vector<ChannelFeatures> features; // kTrackCount
    std::uint32_t sampleRate = kDefaultSampleRate;
    std::uint32_t hopMs = kDefaultHopMs;
    std::array<float, kParamCount> params{}; // PRMS
    UiState ui;
    std::vector<UnknownChunk> unknownChunks; // 未知 fourcc 原样保留/回写
};

} // namespace scvb::s4
