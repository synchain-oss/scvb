// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/InputStateCodec.h"

#include <algorithm>
#include <cstring>

namespace scvb::state
{

namespace
{
constexpr std::size_t kHeaderBytes = 16; // 4 个 u32

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

bool encodeInputState(const InputState& s, std::vector<std::uint8_t>& out)
{
    out.clear();
    const std::size_t langBytes = std::min<std::size_t>(s.uiLanguage.size(), kInputLanguageMaxBytes);
    try
    {
        out.reserve(kHeaderBytes + langBytes);
    }
    catch (...)
    {
        return false;
    }
    putU32(out, s.channelId);
    putU32(out, s.groupId);
    putU32(out, s.uiScale);
    putU32(out, static_cast<std::uint32_t>(langBytes));
    out.insert(out.end(), s.uiLanguage.begin(), s.uiLanguage.begin() + static_cast<std::ptrdiff_t>(langBytes));
    return true;
}

bool decodeInputState(const std::uint8_t* data, std::size_t size, InputState& out)
{
    if (data == nullptr || size < kHeaderBytes)
    {
        return false;
    }
    std::uint32_t channelId = 0;
    std::uint32_t groupId = 0;
    std::uint32_t uiScale = 0;
    std::uint32_t langBytes = 0;
    if (!readU32(data, size, channelId) || !readU32(data + 4, size - 4, groupId) ||
        !readU32(data + 8, size - 8, uiScale) || !readU32(data + 12, size - 12, langBytes))
    {
        return false;
    }
    if (channelId > kInputChannelIdMax)
    {
        return false;
    }
    if (groupId < kInputGroupIdMin || groupId > kInputGroupIdMax)
    {
        return false;
    }
    if (langBytes > kInputLanguageMaxBytes || kHeaderBytes + langBytes != size)
    {
        return false; // 长度字段与总长不一致 → 拒载(不可信字节)
    }
    InputState parsed;
    parsed.channelId = channelId;
    parsed.groupId = groupId;
    parsed.uiScale = uiScale;
    parsed.uiLanguage.assign(reinterpret_cast<const char*>(data + kHeaderBytes), langBytes);
    out = std::move(parsed);
    return true;
}

} // namespace scvb::state
