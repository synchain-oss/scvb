// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/VersionStore.h"

#include <chrono>

namespace scvb::engine
{

namespace
{

// 默认版本名 "V1"/"V2"([J05])。
std::string defaultName(int version)
{
    return "V" + std::to_string(version);
}

// UTF-8 码点计数:非续字节(0xxxxxxx / 11xxxxxx)即一个码点起始。
std::size_t utf8CodePointCount(const std::string& s)
{
    std::size_t n = 0;
    for (const unsigned char c : s)
    {
        if ((c & 0xC0) != 0x80)
            ++n;
    }
    return n;
}

// 截断到 maxChars 个 UTF-8 码点(绝不劈开多字节字符)。
std::string utf8Truncate(const std::string& s, std::size_t maxChars)
{
    std::size_t chars = 0;
    std::size_t cut = 0;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const bool isStart = (c & 0xC0) != 0x80;
        if (isStart)
        {
            if (chars == maxChars)
            {
                cut = i;
                break;
            }
            ++chars;
        }
        cut = i + 1;
    }
    return s.substr(0, cut);
}

std::int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

SetNameResult normalizeVersionName(int version, const std::string& input, std::string& out)
{
    if (version < 1 || version > kNumVersions)
        return SetNameResult::InvalidIndex;

    if (input.empty())
    {
        out = defaultName(version);
        return SetNameResult::EmptyFellBack;
    }

    if (utf8CodePointCount(input) > kMaxVersionNameChars)
    {
        out = utf8Truncate(input, kMaxVersionNameChars);
        return SetNameResult::Truncated;
    }

    out = input;
    return SetNameResult::Ok;
}

VersionStore::VersionStore()
{
    m_versions[0].name = defaultName(1);
    m_versions[1].name = defaultName(2);
}

const std::string& VersionStore::versionName(int version) const
{
    static const std::string kEmpty;
    if (version < 1 || version > kNumVersions)
        return kEmpty;
    return m_versions[static_cast<std::size_t>(version - 1)].name;
}

SetNameResult VersionStore::setVersionName(int version, const std::string& name)
{
    if (version < 1 || version > kNumVersions)
        return SetNameResult::InvalidIndex;

    std::string effective;
    const SetNameResult result = normalizeVersionName(version, name, effective);
    m_versions[static_cast<std::size_t>(version - 1)].name = std::move(effective);
    return result;
}

void VersionStore::setCurve(int version, int track, const CurveEvaluator* curve)
{
    if (version < 1 || version > kNumVersions || track < 0 || track >= kNumTracks)
        return;

    // 不可变契约:新建对象再替换 shared_ptr,绝不原地改写已注入曲线。
    m_versions[static_cast<std::size_t>(version - 1)].curves[static_cast<std::size_t>(track)] =
        (curve != nullptr) ? std::make_shared<CurveEvaluator>(*curve) : nullptr;
}

const CurveEvaluator* VersionStore::curve(int version, int track) const
{
    if (version < 1 || version > kNumVersions || track < 0 || track >= kNumTracks)
        return nullptr;
    return m_versions[static_cast<std::size_t>(version - 1)].curves[static_cast<std::size_t>(track)].get();
}

CopyVersionResult VersionStore::validateCopy(int src, int dst, AuthorityMode mode) const
{
    if (mode == AuthorityMode::Print)
        return CopyVersionResult::RejectedPrint;
    if (src < 1 || src > kNumVersions || dst < 1 || dst > kNumVersions)
        return CopyVersionResult::InvalidIndex;
    if (src == dst)
        return CopyVersionResult::SameVersion;
    return CopyVersionResult::Ok;
}

CopyVersionResult VersionStore::copyVersion(int src, int dst, AuthorityMode mode)
{
    const CopyVersionResult result = validateCopy(src, dst, mode);
    if (result != CopyVersionResult::Ok)
        return result;

    applyCopy(src, dst);
    return CopyVersionResult::Ok;
}

void VersionStore::applyCopy(int src, int dst)
{
    const Version& s = m_versions[static_cast<std::size_t>(src - 1)];
    Version& d = m_versions[static_cast<std::size_t>(dst - 1)];

    for (int t = 0; t < kNumTracks; ++t)
    {
        const auto& srcCurve = s.curves[static_cast<std::size_t>(t)];
        d.curves[static_cast<std::size_t>(t)] =
            (srcCurve != nullptr) ? std::make_shared<CurveEvaluator>(*srcCurve) : nullptr;
    }

    // name 不复制 —— 保留目标版本原名([J05]:name 是用户对「槽位」的命名)。
    d.meta.copiedFrom = src;
    d.meta.copiedAtMs = nowMs();
}

std::array<std::shared_ptr<const CurveEvaluator>, kNumTracks> VersionStore::snapshotCurves(int version) const
{
    std::array<std::shared_ptr<const CurveEvaluator>, kNumTracks> out{};
    if (version < 1 || version > kNumVersions)
        return out;
    out = m_versions[static_cast<std::size_t>(version - 1)].curves;
    return out;
}

void VersionStore::restoreCurves(int version,
                                 const std::array<std::shared_ptr<const CurveEvaluator>, kNumTracks>& curves)
{
    if (version < 1 || version > kNumVersions)
        return;
    m_versions[static_cast<std::size_t>(version - 1)].curves = curves;
}

const VersionMeta& VersionStore::meta(int version) const
{
    static const VersionMeta kEmpty{};
    if (version < 1 || version > kNumVersions)
        return kEmpty;
    return m_versions[static_cast<std::size_t>(version - 1)].meta;
}

void VersionStore::setMeta(int version, const VersionMeta& meta)
{
    if (version < 1 || version > kNumVersions)
        return;
    m_versions[static_cast<std::size_t>(version - 1)].meta = meta;
}

int VersionStore::versionActive() const
{
    return m_versionActive;
}

int VersionStore::setVersionActive(int version)
{
    int effective = version;
    if (effective < 1)
    {
        effective = 1;
        ++m_warningCount;
    }
    else if (effective > kNumVersions)
    {
        effective = kNumVersions;
        ++m_warningCount;
    }

    m_versionActive = effective;
    return effective;
}

int VersionStore::warningCount() const
{
    return m_warningCount;
}

} // namespace scvb::engine
