// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/OutputStateCodec.h"

#include <algorithm>
#include <cstddef>

namespace scvb::state
{

namespace
{
constexpr std::size_t kHeaderBytes = 24; // 6 个 u32
constexpr std::size_t kTrailerBytes = 8; // 尾部追加段:uiGuideSeen + uiTourSeen(T37)

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
} // namespace

bool encodeOutputState(const OutputState& s, std::vector<std::uint8_t>& out)
{
    out.clear();
    const std::size_t langBytes = std::min<std::size_t>(s.uiLanguage.size(), kOutputLanguageMaxBytes);
    try
    {
        out.reserve(kHeaderBytes + langBytes + kTrailerBytes);
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
    putU32(out, s.uiGuideSeen != 0 ? 1u : 0u);
    putU32(out, s.uiTourSeen != 0 ? 1u : 0u);
    return true;
}

bool decodeOutputState(const std::uint8_t* data, std::size_t size, OutputState& out)
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
    // 两种合法总长:老布局(无尾部)与新布局(带 uiGuideSeen/uiTourSeen)。其余一律拒载。
    const bool hasTrailer =
        (langBytes <= kOutputLanguageMaxBytes) && (kHeaderBytes + langBytes + kTrailerBytes == size);
    if (langBytes > kOutputLanguageMaxBytes || (kHeaderBytes + langBytes != size && !hasTrailer))
    {
        return false; // 长度字段与总长不一致 → 拒载(不可信字节,§7.3;CFGS 无尾字段,严格 baseSize)
    }
    std::uint32_t guideSeen = 0;
    std::uint32_t tourSeen = 0;
    if (hasTrailer)
    {
        const std::size_t off = kHeaderBytes + langBytes;
        if (!readU32(data + off, size - off, guideSeen) || !readU32(data + off + 4, size - off - 4, tourSeen))
        {
            return false;
        }
        if (guideSeen > 1 || tourSeen > 1)
        {
            return false; // 布尔位越界 → 不可信字节
        }
    }
    OutputState parsed;
    parsed.groupId = groupId;
    parsed.captureEnabled = captureEnabled;
    parsed.outputEnabled = outputEnabled;
    parsed.versionActive = versionActive;
    parsed.uiScale = uiScale;
    parsed.uiLanguage.assign(reinterpret_cast<const char*>(data + kHeaderBytes), langBytes);
    parsed.uiGuideSeen = guideSeen;
    parsed.uiTourSeen = tourSeen;
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
