// SPDX-License-Identifier: GPL-3.0-or-later
#include "export/SuggestionExport.h"

#include <cmath>
#include <cstdio>

namespace scvb::suggest
{
namespace
{

// 轨号 1..kNumTracks → tracksMask 位;bit15 保留 0(契约 §9.2),故第 16 位恒不命中。
bool trackSelected(std::uint16_t mask, int track1Based) noexcept
{
    if (track1Based < 1 || track1Based > static_cast<int>(state::kNumTracks))
        return false;
    return (mask & static_cast<std::uint16_t>(1u << (track1Based - 1))) != 0;
}

// 段与窗口有重叠(半开区间 [t0,t1) 与 [start,end)):窗口未生效时恒真。
bool inWindow(double t0Sec, double t1Sec, const Scope& scope) noexcept
{
    if (!(scope.startSec >= 0.0) || !(scope.endSec > scope.startSec))
        return true;
    return t1Sec > scope.startSec && t0Sec < scope.endSec;
}

} // namespace

const char* originName(state::SegmentOrigin origin) noexcept
{
    switch (origin)
    {
    case state::SegmentOrigin::UserEdited:
        return "user_edited";
    case state::SegmentOrigin::UserCreated:
        return "user_created";
    case state::SegmentOrigin::Auto:
    default:
        return "auto";
    }
}

std::string csvField(const std::string& raw)
{
    bool needsQuote = false;
    for (const char c : raw)
    {
        if (c == ',' || c == '"' || c == '\r' || c == '\n')
        {
            needsQuote = true;
            break;
        }
    }
    if (!needsQuote)
        return raw;

    std::string out;
    out.reserve(raw.size() + 2);
    out.push_back('"');
    for (const char c : raw)
    {
        if (c == '"')
            out.push_back('"'); // RFC 4180:内部双引号翻倍
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string fmtFixed(double v, int decimals)
{
    if (!std::isfinite(v))
        v = 0.0; // NaN/inf 不该出现在建议值里;真出现了也不写进 CSV 毒化下游
    if (decimals < 0)
        decimals = 0;
    if (decimals > 9)
        decimals = 9;

    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    std::string s(buf);

    // 「-0.000」这类负零:同一个零在不同轨上写出两种字面值,导表软件与 diff 都会当成两个值。
    bool allZero = true;
    for (std::size_t i = 1; i < s.size(); ++i)
    {
        if (s[i] != '0' && s[i] != '.')
        {
            allZero = false;
            break;
        }
    }
    if (!s.empty() && s[0] == '-' && allZero)
        s.erase(s.begin());
    return s;
}

std::vector<Row> buildRows(const ExportInput& input, const Scope& scope)
{
    std::vector<Row> rows;
    if (input.curves == nullptr)
        return rows;
    if (!(input.sampleRate > 0.0))
        return rows;

    const int firstVersion = scope.allVersions ? 1 : scope.activeVersion;
    const int lastVersion = scope.allVersions ? static_cast<int>(state::kNumVersions) : scope.activeVersion;
    if (firstVersion < 1 || lastVersion > static_cast<int>(state::kNumVersions) || firstVersion > lastVersion)
    {
        return rows;
    }

    for (int v = firstVersion; v <= lastVersion; ++v)
    {
        const state::VersionCurve& version = input.curves->versions[static_cast<std::size_t>(v - 1)];

        for (int t = 1; t <= static_cast<int>(state::kNumTracks); ++t)
        {
            if (!trackSelected(scope.tracksMask, t))
                continue;

            const auto ti = static_cast<std::size_t>(t - 1);
            const TrackMeta& meta = input.tracks[ti];
            const bool stereo = meta.sourceChannels == 2;
            const auto& segments = version.tracks[ti].segments;

            for (std::size_t i = 0; i < segments.size(); ++i)
            {
                const state::Segment& seg = segments[i];
                const double t0Sec = static_cast<double>(seg.t0) / input.sampleRate;
                const double t1Sec = static_cast<double>(seg.t1) / input.sampleRate;
                if (!inWindow(t0Sec, t1Sec, scope))
                    continue;

                Row row;
                row.trackIndex = t;
                row.trackLabel = meta.label;
                row.sourceChannels = stereo ? 2 : 1;
                row.version = v;
                row.versionName = version.meta.name;
                row.segmentIndex = static_cast<int>(i);
                row.t0Sec = t0Sec;
                row.t1Sec = t1Sec;
                row.pan = static_cast<double>(seg.pan);
                row.volDb = static_cast<double>(seg.volDb);
                // mono 轨的 width 参数是 v1 no-op 占位(params v2.0)——列留空,不写 0:
                // 0 在 stereo 轨上是「收成 mono」的有效建议([J57]),两者不可混。
                // 调用方没装这一格(哨兵)时同样留空:「不知道」比「不知道所以写 0」诚实。
                const float rawWidth = input.widthPercent[static_cast<std::size_t>(v - 1)][ti];
                row.hasWidth = stereo && rawWidth >= 0.0f;
                row.width = row.hasWidth ? static_cast<double>(rawWidth) : 0.0;
                row.origin = state::segmentOrigin(seg.flags);
                row.locked = state::segmentLocked(seg.flags);
                rows.push_back(std::move(row));
            }
        }
    }
    return rows;
}

std::string toCsv(const std::vector<Row>& rows)
{
    std::string out;
    // 13 列 × ~12 字节 + BOM/表头,预留一把免得逐行搬家(15×2×40 = 1200 行是常规量级)。
    out.reserve(rows.size() * 96 + 256);

    out.append(kUtf8Bom);
    out.append(kCsvHeader);
    out.append(kCsvNewline);

    for (const Row& r : rows)
    {
        out.append(std::to_string(r.trackIndex));
        out.push_back(',');
        out.append(csvField(r.trackLabel));
        out.push_back(',');
        out.append(std::to_string(r.sourceChannels));
        out.push_back(',');
        out.append(std::to_string(r.version));
        out.push_back(',');
        out.append(csvField(r.versionName));
        out.push_back(',');
        out.append(std::to_string(r.segmentIndex));
        out.push_back(',');
        out.append(fmtFixed(r.t0Sec, kDecimalsSec));
        out.push_back(',');
        out.append(fmtFixed(r.t1Sec, kDecimalsSec));
        out.push_back(',');
        out.append(fmtFixed(r.pan, kDecimalsPan));
        out.push_back(',');
        out.append(fmtFixed(r.volDb, kDecimalsVol));
        out.push_back(',');
        if (r.hasWidth)
            out.append(fmtFixed(r.width, kDecimalsWidth)); // mono:空字段
        out.push_back(',');
        out.append(originName(r.origin));
        out.push_back(',');
        out.append(r.locked ? "true" : "false");
        out.append(kCsvNewline);
    }
    return out;
}

} // namespace scvb::suggest
