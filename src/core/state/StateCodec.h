// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "analysis/PanCurve.h"

// T19 state 层:TLV 容器 + CRVS 曲线节(03 §6.1)。纯 C++17,无 JUCE(ADR-011)。
//
// 容器(定长头 + TLV 块,little-endian):
//   偏移  字段
//   0     u32 magic = 'SCVB' (0x42564353,小端内存序;与 ipc/SegmentLayout.h kScvbMagic 同源)
//   4     u32 abi   = 1(与 IPC abi 独立计数)
//   8     u32 flags = 0
//   12    u32 chunkCount
//   16..  TLV × N:{ u32 fourcc; u32 sizeBytes; u8 payload[sizeBytes](4 字节对齐,padding 置 0) }
//
// 四个已知 fourcc:PRMS / CFGS / CRVS / FEAT。PRMS/CFGS 是 JUCE ValueTree 二进制、
// FEAT 是 gzip(04 §5.2,归 T21)——三者在 scvb_core(无 JUCE)层面都是**不透明 payload**,
// 本层只负责容器承载与未知块回写(03 §6.1)。CRVS 是自定义紧凑二进制,本层负责其编解码。
// 未知 fourcc 的块在 load 时原样保留、save 时原样回写(前向小版本兼容)。
//
// 【挂账 / 后续接线契约落点】(本卡不改代码,只记账):
// 1. PRMS/CFGS 的 ValueTree 字段级编码(123 参数 + ui{…}、group_id/global/analysis/channels[15]
//    的 source_channels/participate_in_auto_pan 回填与 auto_pan/auto_vol 丢弃)归 Output/JUCE 层
//    的 getStateInformation/setStateInformation 接线卡(本层仅 opaque 承载 + fourcc 常量)。
// 2. 同 abi 内 CRVS 的 minor 高于当前(kCrvsMinorVersion)时,decodeCrvs 只拒解本块,容器级 loadState
//    仍返回 Ok —— Output 接线卡必须把「同 abi 但 CRVS minor 更高」按「等同拒载 + preservedOriginal
//    原样回写 + 提示升级」处理,不得让旧插件抹掉新版曲线数据。
// 3. docs/STATE_SCHEMA.md 目前是 T39a 占位空壳;本 codec 是 wire-format 先行真源,T39a 回填时以本
//    头 + tests/golden/state/abi1.bin 为准交叉校验。
namespace scvb::state
{

// ---- 容器常量(03 §6.1)----
// magic 与仓内 IPC 已冻结常量同源(ipc/SegmentLayout.h kScvbMagic = makeFourCc('S','C','V','B') =
// 0x42564353):小端写盘后前 4 字节字面拼出 "SCVB",与 tests/golden/ipc-layout.txt 的 magic 0x42564353 一致。
inline constexpr std::uint32_t kStateMagic = 0x42564353u; // 'SCVB'(小端内存序,与 SegmentLayout.h 同源)
inline constexpr std::uint32_t kCurrentAbi = 1u;

inline constexpr std::uint32_t kFourccPrms = 0x534D5250u; // 'PRMS'
inline constexpr std::uint32_t kFourccCfgs = 0x53474643u; // 'CFGS'
inline constexpr std::uint32_t kFourccCrvs = 0x53565243u; // 'CRVS'
inline constexpr std::uint32_t kFourccFeat = 0x54414546u; // 'FEAT'

// ---- 校验上限(不可信字节:长度/范围字段先校验再分配/索引,CLAUDE.md §7.3)----
inline constexpr std::uint32_t kMaxChunkCount = 256u;
inline constexpr std::uint16_t kCrvsMinorVersion = 1u;
inline constexpr std::uint16_t kMaxVersionNameBytes = 64u; // 16 码点 × 4 字节 UTF-8 上限
inline constexpr std::uint32_t kMaxSegmentsPerTrack = 1000000u;
inline constexpr std::uint32_t kMaxExcludedRangesPerTrack = 1000000u;
inline constexpr std::uint16_t kMaxPanPointsPerVersion = 4096u;

// [J59] 版本数 4→2、轨道数 10→15。
inline constexpr std::size_t kNumVersions = 2;
inline constexpr std::size_t kNumTracks = 15;
// CRVS 头用 u8 承载版本/轨道数,须在单字节内(防窄化静默截断)。
static_assert(kNumVersions <= 255 && kNumTracks <= 255, "CRVS counts must fit in u8");

// ---- segment flags([J34]:替换 manual_edited;03 §6.1)----
enum class SegmentOrigin : std::uint32_t
{
    Auto = 0,
    UserEdited = 1,
    UserCreated = 2,
};
inline constexpr std::uint32_t kSegmentOriginMask = 0x3u; // bit0-1 = origin
inline constexpr std::uint32_t kSegmentLockedBit = 1u << 2; // bit2 = locked

SegmentOrigin segmentOrigin(std::uint32_t flags) noexcept;
bool segmentLocked(std::uint32_t flags) noexcept;
std::uint32_t makeSegmentFlags(SegmentOrigin origin, bool locked) noexcept;

// ---- CRVS 数据模型(03 §6.1:紧凑二进制,自带 u16 minor 版本;定长记录可顺序读)----
struct Segment
{
    std::int64_t t0 = 0; // samples
    std::int64_t t1 = 0; // samples
    float pan = 0.0f; // f32
    float volDb = 0.0f; // f32
    std::uint32_t flags = 0; // bit0-1 origin,bit2 locked
};

struct ExcludedRange
{
    std::int64_t t0 = 0; // samples
    std::int64_t t1 = 0; // samples
};

struct TrackCurve
{
    std::vector<Segment> segments;
    // excluded_ranges 的唯一编码落点(02 §3.5/SEG-6):作用域 per-version per-track,与 segments 同级。
    std::vector<ExcludedRange> excludedRanges;
};

struct VersionMeta
{
    std::string name; // [J05] 默认 "V1"/"V2",≤16 字符;UTF-8 变长
    std::int32_t copiedFrom = 0; // T18 VersionMeta(0 = 未复制;非 0 = 1-based 来源版本号,勿作数组下标)
    std::int64_t copiedAtMs = 0; // 复制时刻(epoch 毫秒)
};

struct VersionCurve
{
    VersionMeta meta;
    std::array<TrackCurve, kNumTracks> tracks;
    std::vector<scvb::PanCurvePoint> panCurve; // pan 角度域增益曲线,per-version(02 §7 / params-v0 §二)
};

struct CrvsData
{
    std::array<VersionCurve, kNumVersions> versions;
};

// ---- 容器 chunk 与 StateChunks ----
struct Chunk
{
    std::uint32_t fourcc = 0;
    std::vector<std::uint8_t> payload;
};

struct StateChunks
{
    std::uint32_t abi = kCurrentAbi;
    std::uint32_t flags = 0;
    std::vector<Chunk> chunks; // 按出现序;未知 fourcc 原样保留(load)/回写(save)

    // 首个匹配 fourcc 的 chunk;无则 nullptr。
    const Chunk* find(std::uint32_t fourcc) const noexcept;
    // 原位替换首个匹配 chunk 的 payload;无匹配则追加。调用方不应构造重复 fourcc。
    void set(std::uint32_t fourcc, std::vector<std::uint8_t> payload);
    // 删除全部同 fourcc 的 chunk。
    void remove(std::uint32_t fourcc);
};

struct StateHeader
{
    std::uint32_t magic = 0;
    std::uint32_t abi = 0;
    std::uint32_t flags = 0;
    std::uint32_t chunkCount = 0;
};

// 解析头部(只校验头部长度,不判 magic);失败返回 false(数据不足)。
bool parseHeader(const std::uint8_t* data, std::size_t size, StateHeader& out);

enum class DecodeStatus
{
    Ok = 0,
    Corrupt = 1,
};

// 完整容器解码:校验 magic + 每个 chunk 的 size/padding 边界;未知 fourcc 原样保留。
DecodeStatus decodeContainer(const std::uint8_t* data, std::size_t size, StateChunks& out);

// 容器编码(定长头 + TLV,payload 4 字节对齐、padding 置 0)。payload 超 4GB 返回 false。
bool encodeContainer(const StateChunks& chunks, std::vector<std::uint8_t>& out);

// ---- CRVS 子编解码(严格校验,不可信字节)----
bool encodeCrvs(const CrvsData& data, std::vector<std::uint8_t>& out);
bool decodeCrvs(const std::uint8_t* data, std::size_t size, CrvsData& out);

} // namespace scvb::state
