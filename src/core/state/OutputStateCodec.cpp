// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/OutputStateCodec.h"

#include <algorithm>
#include <cstddef>

namespace scvb::state
{

namespace
{
constexpr std::size_t kHeaderBytes = 24; // 6 个 u32
constexpr std::size_t kEnumBytes = 8; // 2 个 u32(loudness_mode + center_slot_policy)

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

bool readU32(const std::uint8_t* p, std::size_t size, std::uint32_t& out)
{
    if (size < 4)
    {
        return false;
    }
    out = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
          (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    return true;
}

// ---- [J69/U24] 枚举字符串 ↔ 序号(STATE_SCHEMA 口径;未知一律回落默认档)----

const char* loudnessModeString(std::uint32_t ordinal)
{
    switch (ordinal)
    {
    case 1:
        return "rms";
    case 2:
        return "peak_dbfs";
    case 0:
    default:
        return "kw_integrated";
    }
}

std::uint32_t loudnessModeOrdinal(const std::string& s)
{
    if (s == "rms")
        return 1;
    if (s == "peak_dbfs")
        return 2;
    return 0; // "kw_integrated" 或未知 → 默认
}

const char* centerSlotPolicyString(std::uint32_t ordinal)
{
    switch (ordinal)
    {
    case 1:
        return "lead_exclusive";
    case 2:
        return "even_spread";
    case 0:
    default:
        return "priority_queue";
    }
}

std::uint32_t centerSlotPolicyOrdinal(const std::string& s)
{
    if (s == "lead_exclusive")
        return 1;
    if (s == "even_spread")
        return 2;
    return 0; // "priority_queue" 或未知 → 默认
}
} // namespace

bool encodeOutputState(const OutputState& s, std::vector<std::uint8_t>& out)
{
    out.clear();
    const std::size_t langBytes = std::min<std::size_t>(s.uiLanguage.size(), kOutputLanguageMaxBytes);
    try
    {
        out.reserve(kHeaderBytes + langBytes + kEnumBytes + s.unknownTail.size());
    }
    catch (...)
    {
        return false;
    }
    putU32(out, s.groupId);
    putU32(out, s.captureEnabled);
    putU32(out, s.outputEnabled);
    putU32(out, s.versionActive);
    putU32(out, s.uiScale);
    putU32(out, static_cast<std::uint32_t>(langBytes));
    out.insert(out.end(), s.uiLanguage.begin(), s.uiLanguage.begin() + static_cast<std::ptrdiff_t>(langBytes));
    putU32(out, loudnessModeOrdinal(s.loudnessMode));
    putU32(out, centerSlotPolicyOrdinal(s.centerSlotPolicy));
    out.insert(out.end(), s.unknownTail.begin(), s.unknownTail.end()); // 未知尾部原样回写
    return true;
}

bool decodeOutputState(const std::uint8_t* data, std::size_t size, OutputState& out, OutputDecodeReport* report)
{
    if (data == nullptr || size < kHeaderBytes)
    {
        return false;
    }
    std::uint32_t groupId = 0;
    std::uint32_t captureEnabled = 0;
    std::uint32_t outputEnabled = 0;
    std::uint32_t versionActive = 0;
    std::uint32_t uiScale = 0;
    std::uint32_t langBytes = 0;
    if (!readU32(data, size, groupId) || !readU32(data + 4, size - 4, captureEnabled) ||
        !readU32(data + 8, size - 8, outputEnabled) || !readU32(data + 12, size - 12, versionActive) ||
        !readU32(data + 16, size - 16, uiScale) || !readU32(data + 20, size - 20, langBytes))
    {
        return false;
    }
    if (groupId < kOutputGroupIdMin || groupId > kOutputGroupIdMax)
    {
        return false;
    }
    if (captureEnabled > 1 || outputEnabled > 1)
    {
        return false;
    }
    if (versionActive < kOutputVersionMin || versionActive > kOutputVersionMax)
    {
        return false;
    }
    if (langBytes > kOutputLanguageMaxBytes)
    {
        return false; // langBytes 超上限 → 拒载(不可信字节,§7.3;尾字段/未知尾部由下方长度回退逻辑处理)
    }
    const std::size_t base = kHeaderBytes + langBytes;
    if (base > size)
    {
        return false;
    }
    const std::size_t remaining = size - base;
    const bool hasEnums = (remaining >= kEnumBytes);
    if (remaining != 0 && !hasEnums)
    {
        return false; // 0 < remaining < 8:枚举字段被截断 → 拒载(不可信字节)
    }

    // 兼容:旧版(abi=1)payload 无末两个 u32 → 两字段回落默认,不计未知回落。
    std::uint32_t loudnessOrdinal = 0;
    std::uint32_t centerOrdinal = 0;
    if (hasEnums)
    {
        if (!readU32(data + base, remaining, loudnessOrdinal) ||
            !readU32(data + base + 4, remaining - 4, centerOrdinal))
        {
            return false;
        }
    }

    OutputState parsed;
    parsed.groupId = groupId;
    parsed.captureEnabled = captureEnabled;
    parsed.outputEnabled = outputEnabled;
    parsed.versionActive = versionActive;
    parsed.uiScale = uiScale;
    parsed.uiLanguage.assign(reinterpret_cast<const char*>(data + kHeaderBytes), langBytes);

    if (loudnessOrdinal > kOutputLoudnessModeMax)
    {
        if (report != nullptr)
        {
            ++report->loudnessModeFallbacks;
        }
        loudnessOrdinal = 0;
    }
    if (centerOrdinal > kOutputCenterSlotPolicyMax)
    {
        if (report != nullptr)
        {
            ++report->centerSlotPolicyFallbacks;
        }
        centerOrdinal = 0;
    }
    parsed.loudnessMode = loudnessModeString(loudnessOrdinal);
    parsed.centerSlotPolicy = centerSlotPolicyString(centerOrdinal);
    if (hasEnums && remaining > kEnumBytes)
    {
        // 未知尾部(未来小版本追加字段)保留,编码时原样回写,防静默丢字段。
        parsed.unknownTail.assign(data + base + kEnumBytes, data + size);
    }

    out = std::move(parsed);
    return true;
}

bool encodeUiConfig(std::uint32_t masterChartMode, std::vector<std::uint8_t>& out)
{
    out.clear();
    try
    {
        out.reserve(kUiConfigBytes);
    }
    catch (...)
    {
        return false;
    }
    putU32(out, masterChartMode); // [J75] T43 恒写 4 字节(0=distribution | 1=trajectory)
    return true;
}

bool decodeUiConfig(const std::uint8_t* data, std::size_t size, std::uint32_t& out)
{
    out = kMasterChartModeDistribution; // 缺失/非法长度/未知值一律回落默认 distribution
    if (data == nullptr || size != kUiConfigBytes)
    {
        return false; // 长度非法 → 拒载该 chunk(§7.3);调用方回落默认
    }
    std::uint32_t tag = 0;
    if (!readU32(data, size, tag))
    {
        return false;
    }
    out = (tag == kMasterChartModeTrajectory) ? kMasterChartModeTrajectory : kMasterChartModeDistribution;
    return true;
}

} // namespace scvb::state
