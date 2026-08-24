// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/OutputStateCodec.h"

#include <algorithm>
#include <cstddef>

namespace scvb::state
{

namespace
{
constexpr std::size_t kHeaderBytes = 24; // 6 个 u32
constexpr std::size_t kChartModeBytes = 4; // [J75] T43 尾字段(u32)

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
        out.reserve(kHeaderBytes + langBytes + kChartModeBytes);
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
    putU32(out, s.masterChartMode); // [J75] T43 尾字段(0=distribution | 1=trajectory)
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
    if (langBytes > kOutputLanguageMaxBytes)
    {
        return false;
    }
    const std::size_t baseSize = kHeaderBytes + langBytes;
    // 旧工程(v1 之前)无尾字段 → baseSize;新版带尾字段 → baseSize + 4。其余长度 → 拒载(不可信字节)。
    if (size != baseSize && size != baseSize + kChartModeBytes)
    {
        return false;
    }
    OutputState parsed;
    parsed.groupId = groupId;
    parsed.captureEnabled = captureEnabled;
    parsed.outputEnabled = outputEnabled;
    parsed.versionActive = versionActive;
    parsed.uiScale = uiScale;
    parsed.uiLanguage.assign(reinterpret_cast<const char*>(data + kHeaderBytes), langBytes);
    // [J75] T43:缺尾字段(旧工程)与未知取值一律回落默认 distribution;abi 不递增。
    parsed.masterChartMode = kMasterChartModeDistribution;
    if (size == baseSize + kChartModeBytes)
    {
        std::uint32_t tag = 0;
        if (!readU32(data + baseSize, kChartModeBytes, tag))
        {
            return false;
        }
        if (tag == kMasterChartModeTrajectory)
        {
            parsed.masterChartMode = kMasterChartModeTrajectory;
        }
    }
    out = std::move(parsed);
    return true;
}

} // namespace scvb::state
