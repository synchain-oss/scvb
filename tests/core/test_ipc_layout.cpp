// SPDX-License-Identifier: GPL-3.0-or-later
// test_ipc_layout —— IPC 段布局冻结校验(T06)。把 tests/golden/ipc-layout.txt 逐行对拍
// SegmentLayout.h 的 sizeof/alignof/offsetof/常量/段名,锁死 abi=1 布局。

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "ipc/CtrlPlane.h"
#include "ipc/Registry.h" // [SL-254] outputSlotAtBase 的冻结偏移对拍
#include "ipc/SegmentLayout.h"
#include "ipc/VizPlane.h"

#ifdef WIN32
#include "ipc/SegmentBackendWin32.h"
#endif

using scvb::u32;
using scvb::u64;

namespace
{
constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

std::string goldenPath()
{
    return std::string(SCVB_TEST_GOLDEN_DIR) + "/ipc-layout.txt";
}

std::string hex32(u32 v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", static_cast<unsigned int>(v));
    return buf;
}

std::string narrow(const std::wstring& w)
{
    std::string s;
    s.reserve(w.size());
    for (const wchar_t c : w)
    {
        s.push_back(static_cast<char>(c));
    }
    return s;
}

std::vector<std::string> split(const std::string& line)
{
    std::istringstream iss(line);
    std::vector<std::string> toks;
    std::string t;
    while (iss >> t)
    {
        toks.push_back(t);
    }
    return toks;
}

std::vector<std::string> readLines(const std::string& path)
{
    std::ifstream f(path);
    REQUIRE(f.good());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '')
        {
            line.pop_back(); // CRLF 容错
        }
        if (line.empty() || line[0] == '#')
        {
            continue; // 注释/空行
        }
        lines.push_back(line);
    }
    return lines;
}

std::size_t structSize(const std::string& s)
{
    if (s == "RegistryHeader")
        return sizeof(scvb::RegistryHeader);
    if (s == "InputSlot")
        return sizeof(scvb::InputSlot);
    if (s == "OutputSlot")
        return sizeof(scvb::OutputSlot);
    if (s == "AudioRingHeader")
        return sizeof(scvb::AudioRingHeader);
    if (s == "FeatHeader")
        return sizeof(scvb::FeatHeader);
    if (s == "FeatFrame")
        return sizeof(scvb::FeatFrame);
    if (s == "OutputGlobalInfo")
        return sizeof(scvb::OutputGlobalInfo);
    if (s == "CtrlRecord")
        return sizeof(scvb::CtrlRecord);
    if (s == "CtrlHeader")
        return sizeof(scvb::CtrlHeader);
    if (s == "CtrlRing")
        return sizeof(scvb::CtrlRing);
    if (s == "CtrlChannelConfig")
        return sizeof(scvb::CtrlChannelConfig);
    if (s == "CtrlBroadcast")
        return sizeof(scvb::CtrlBroadcast);
    if (s == "VizHeader")
        return sizeof(scvb::VizHeader);
    if (s == "VizFrame")
        return sizeof(scvb::VizFrame);
    if (s == "VizTrackColors")
        return sizeof(scvb::VizTrackColors);
    if (s == "VizCoverage")
        return sizeof(scvb::VizCoverage);
    if (s == "VizLanes")
        return sizeof(scvb::VizLanes);
    if (s == "VizTrackState")
        return sizeof(scvb::VizTrackState);
    if (s == "VizTrackLabels")
        return sizeof(scvb::VizTrackLabels);
    return kNotFound;
}

std::size_t structAlign(const std::string& s)
{
    if (s == "RegistryHeader")
        return alignof(scvb::RegistryHeader);
    if (s == "InputSlot")
        return alignof(scvb::InputSlot);
    if (s == "OutputSlot")
        return alignof(scvb::OutputSlot);
    if (s == "AudioRingHeader")
        return alignof(scvb::AudioRingHeader);
    if (s == "FeatHeader")
        return alignof(scvb::FeatHeader);
    if (s == "FeatFrame")
        return alignof(scvb::FeatFrame);
    if (s == "OutputGlobalInfo")
        return alignof(scvb::OutputGlobalInfo);
    if (s == "CtrlRecord")
        return alignof(scvb::CtrlRecord);
    if (s == "CtrlHeader")
        return alignof(scvb::CtrlHeader);
    if (s == "CtrlRing")
        return alignof(scvb::CtrlRing);
    if (s == "CtrlChannelConfig")
        return alignof(scvb::CtrlChannelConfig);
    if (s == "CtrlBroadcast")
        return alignof(scvb::CtrlBroadcast);
    if (s == "VizHeader")
        return alignof(scvb::VizHeader);
    if (s == "VizFrame")
        return alignof(scvb::VizFrame);
    if (s == "VizTrackColors")
        return alignof(scvb::VizTrackColors);
    if (s == "VizCoverage")
        return alignof(scvb::VizCoverage);
    if (s == "VizLanes")
        return alignof(scvb::VizLanes);
    if (s == "VizTrackState")
        return alignof(scvb::VizTrackState);
    if (s == "VizTrackLabels")
        return alignof(scvb::VizTrackLabels);
    return kNotFound;
}

std::size_t fieldOffset(const std::string& s, const std::string& f)
{
    if (s == "RegistryHeader")
    {
        if (f == "magic")
            return offsetof(scvb::RegistryHeader, magic);
        if (f == "abi")
            return offsetof(scvb::RegistryHeader, abi);
        if (f == "generation")
            return offsetof(scvb::RegistryHeader, generation);
        if (f == "_pad")
            return offsetof(scvb::RegistryHeader, _pad);
        if (f == "_reserved")
            return offsetof(scvb::RegistryHeader, _reserved);
        return kNotFound;
    }
    if (s == "InputSlot")
    {
        if (f == "state")
            return offsetof(scvb::InputSlot, state);
        if (f == "pid")
            return offsetof(scvb::InputSlot, pid);
        if (f == "sample_rate")
            return offsetof(scvb::InputSlot, sample_rate);
        if (f == "max_block")
            return offsetof(scvb::InputSlot, max_block);
        if (f == "heartbeat_ms")
            return offsetof(scvb::InputSlot, heartbeat_ms);
        if (f == "capture_write_pos")
            return offsetof(scvb::InputSlot, capture_write_pos);
        if (f == "flags")
            return offsetof(scvb::InputSlot, flags);
        if (f == "_pad")
            return offsetof(scvb::InputSlot, _pad);
        return kNotFound;
    }
    if (s == "OutputSlot")
    {
        if (f == "state")
            return offsetof(scvb::OutputSlot, state);
        if (f == "pid")
            return offsetof(scvb::OutputSlot, pid);
        if (f == "heartbeat_ms")
            return offsetof(scvb::OutputSlot, heartbeat_ms);
        if (f == "connected_mask")
            return offsetof(scvb::OutputSlot, connected_mask);
        if (f == "config_seq")
            return offsetof(scvb::OutputSlot, config_seq);
        if (f == "_pad")
            return offsetof(scvb::OutputSlot, _pad);
        return kNotFound;
    }
    if (s == "AudioRingHeader")
    {
        if (f == "magic")
            return offsetof(scvb::AudioRingHeader, magic);
        if (f == "abi")
            return offsetof(scvb::AudioRingHeader, abi);
        if (f == "sample_rate")
            return offsetof(scvb::AudioRingHeader, sample_rate);
        if (f == "ring_frames")
            return offsetof(scvb::AudioRingHeader, ring_frames);
        if (f == "channels")
            return offsetof(scvb::AudioRingHeader, channels);
        if (f == "_pad")
            return offsetof(scvb::AudioRingHeader, _pad);
        if (f == "write_head_samples")
            return offsetof(scvb::AudioRingHeader, write_head_samples);
        if (f == "epoch")
            return offsetof(scvb::AudioRingHeader, epoch);
        return kNotFound;
    }
    if (s == "FeatHeader")
    {
        if (f == "magic")
            return offsetof(scvb::FeatHeader, magic);
        if (f == "abi")
            return offsetof(scvb::FeatHeader, abi);
        if (f == "hop_ms")
            return offsetof(scvb::FeatHeader, hop_ms);
        if (f == "capacity_hops")
            return offsetof(scvb::FeatHeader, capacity_hops);
        if (f == "base_hop")
            return offsetof(scvb::FeatHeader, base_hop);
        if (f == "write_hop")
            return offsetof(scvb::FeatHeader, write_hop);
        return kNotFound;
    }
    if (s == "FeatFrame")
    {
        if (f == "kw_ms")
            return offsetof(scvb::FeatFrame, kw_ms);
        if (f == "peak")
            return offsetof(scvb::FeatFrame, peak);
        return kNotFound;
    }
    if (s == "OutputGlobalInfo")
    {
        if (f == "capture_enabled")
            return offsetof(scvb::OutputGlobalInfo, capture_enabled);
        if (f == "output_sample_rate")
            return offsetof(scvb::OutputGlobalInfo, output_sample_rate);
        if (f == "flags")
            return offsetof(scvb::OutputGlobalInfo, flags);
        if (f == "_pad")
            return offsetof(scvb::OutputGlobalInfo, _pad);
        if (f == "gap_count")
            return offsetof(scvb::OutputGlobalInfo, gap_count);
        if (f == "overlap_count")
            return offsetof(scvb::OutputGlobalInfo, overlap_count);
        if (f == "epoch_summary")
            return offsetof(scvb::OutputGlobalInfo, epoch_summary);
        return kNotFound;
    }
    if (s == "CtrlRecord")
    {
        if (f == "seq")
            return offsetof(scvb::CtrlRecord, seq);
        if (f == "channel")
            return offsetof(scvb::CtrlRecord, channel);
        if (f == "op")
            return offsetof(scvb::CtrlRecord, op);
        if (f == "value")
            return offsetof(scvb::CtrlRecord, value);
        return kNotFound;
    }
    if (s == "CtrlHeader")
    {
        if (f == "magic")
            return offsetof(scvb::CtrlHeader, magic);
        if (f == "abi")
            return offsetof(scvb::CtrlHeader, abi);
        if (f == "generation")
            return offsetof(scvb::CtrlHeader, generation);
        if (f == "_pad")
            return offsetof(scvb::CtrlHeader, _pad);
        if (f == "_reserved")
            return offsetof(scvb::CtrlHeader, _reserved);
        return kNotFound;
    }
    if (s == "CtrlRing")
    {
        if (f == "write_pos")
            return offsetof(scvb::CtrlRing, write_pos);
        if (f == "read_pos")
            return offsetof(scvb::CtrlRing, read_pos);
        if (f == "overflow_count")
            return offsetof(scvb::CtrlRing, overflow_count);
        if (f == "_pad")
            return offsetof(scvb::CtrlRing, _pad);
        if (f == "_reserved")
            return offsetof(scvb::CtrlRing, _reserved);
        if (f == "records")
            return offsetof(scvb::CtrlRing, records);
        return kNotFound;
    }
    if (s == "CtrlChannelConfig")
    {
        if (f == "priority")
            return offsetof(scvb::CtrlChannelConfig, priority);
        if (f == "flags")
            return offsetof(scvb::CtrlChannelConfig, flags);
        if (f == "pair_id")
            return offsetof(scvb::CtrlChannelConfig, pair_id);
        if (f == "freeze")
            return offsetof(scvb::CtrlChannelConfig, freeze);
        if (f == "source_channels")
            return offsetof(scvb::CtrlChannelConfig, source_channels);
        if (f == "_reserved")
            return offsetof(scvb::CtrlChannelConfig, _reserved);
        return kNotFound;
    }
    if (s == "CtrlBroadcast")
    {
        if (f == "seq")
            return offsetof(scvb::CtrlBroadcast, seq);
        if (f == "config_seq")
            return offsetof(scvb::CtrlBroadcast, config_seq);
        if (f == "lead_select")
            return offsetof(scvb::CtrlBroadcast, lead_select);
        if (f == "_pad")
            return offsetof(scvb::CtrlBroadcast, _pad);
        if (f == "_reserved")
            return offsetof(scvb::CtrlBroadcast, _reserved);
        if (f == "channels")
            return offsetof(scvb::CtrlBroadcast, channels);
        if (f == "labels")
            return offsetof(scvb::CtrlBroadcast, labels);
        if (f == "_tail")
            return offsetof(scvb::CtrlBroadcast, _tail);
        return kNotFound;
    }
    if (s == "VizHeader")
    {
        if (f == "magic")
            return offsetof(scvb::VizHeader, magic);
        if (f == "abi")
            return offsetof(scvb::VizHeader, abi);
        if (f == "generation")
            return offsetof(scvb::VizHeader, generation);
        if (f == "column_count")
            return offsetof(scvb::VizHeader, column_count);
        if (f == "track_count")
            return offsetof(scvb::VizHeader, track_count);
        if (f == "pan_scale")
            return offsetof(scvb::VizHeader, pan_scale);
        if (f == "_reserved")
            return offsetof(scvb::VizHeader, _reserved);
        return kNotFound;
    }
    if (s == "VizFrame")
    {
        if (f == "seq")
            return offsetof(scvb::VizFrame, seq);
        if (f == "playhead_flags")
            return offsetof(scvb::VizFrame, playhead_flags);
        if (f == "publish_ms")
            return offsetof(scvb::VizFrame, publish_ms);
        if (f == "window_start_samples")
            return offsetof(scvb::VizFrame, window_start_samples);
        if (f == "window_span_samples")
            return offsetof(scvb::VizFrame, window_span_samples);
        if (f == "playhead_samples")
            return offsetof(scvb::VizFrame, playhead_samples);
        if (f == "loop_start_samples")
            return offsetof(scvb::VizFrame, loop_start_samples);
        if (f == "loop_end_samples")
            return offsetof(scvb::VizFrame, loop_end_samples);
        if (f == "sample_rate")
            return offsetof(scvb::VizFrame, sample_rate);
        if (f == "version_active")
            return offsetof(scvb::VizFrame, version_active);
        if (f == "playhead_epoch")
            return offsetof(scvb::VizFrame, playhead_epoch);
        if (f == "track_online_mask")
            return offsetof(scvb::VizFrame, track_online_mask);
        if (f == "track_covered_mask")
            return offsetof(scvb::VizFrame, track_covered_mask);
        if (f == "track_stereo_mask")
            return offsetof(scvb::VizFrame, track_stereo_mask);
        if (f == "lane_revision")
            return offsetof(scvb::VizFrame, lane_revision);
        if (f == "track_lead_mask")
            return offsetof(scvb::VizFrame, track_lead_mask);
        if (f == "_reserved")
            return offsetof(scvb::VizFrame, _reserved);
        return kNotFound;
    }
    if (s == "VizTrackColors")
    {
        if (f == "index")
            return offsetof(scvb::VizTrackColors, index);
        if (f == "_pad")
            return offsetof(scvb::VizTrackColors, _pad);
        return kNotFound;
    }
    if (s == "VizCoverage")
    {
        if (f == "bits")
            return offsetof(scvb::VizCoverage, bits);
        return kNotFound;
    }
    if (s == "VizLanes")
    {
        if (f == "pan")
            return offsetof(scvb::VizLanes, pan);
        return kNotFound;
    }
    if (s == "VizTrackState")
    {
        if (f == "panNow")
            return offsetof(scvb::VizTrackState, panNow);
        if (f == "volDb")
            return offsetof(scvb::VizTrackState, volDb);
        if (f == "widthPct")
            return offsetof(scvb::VizTrackState, widthPct);
        if (f == "_reserved")
            return offsetof(scvb::VizTrackState, _reserved);
        return kNotFound;
    }
    if (s == "VizTrackLabels")
    {
        if (f == "utf8")
            return offsetof(scvb::VizTrackLabels, utf8);
        if (f == "_pad")
            return offsetof(scvb::VizTrackLabels, _pad);
        return kNotFound;
    }
    return kNotFound;
}

// [T44] viz 段内区块偏移(golden 的 "offset <key> <n>" 行)。
std::size_t vizOffset(const std::string& key)
{
    if (key == "viz_header")
        return scvb::kVizHeaderOffset;
    if (key == "viz_frame")
        return scvb::kVizFrameOffset;
    if (key == "viz_colors")
        return scvb::kVizColorsOffset;
    if (key == "viz_coverage")
        return scvb::kVizCoverageOffset;
    if (key == "viz_lanes")
        return scvb::kVizLanesOffset;
    if (key == "viz_track_state")
        return scvb::kVizTrackStateOffset;
    if (key == "viz_labels")
        return scvb::kVizLabelsOffset;
    return kNotFound;
}

u32 parseGroup(const std::string& tok)
{
    REQUIRE(tok.size() >= 2);
    REQUIRE(tok[0] == 'g');
    return static_cast<u32>(std::stoul(tok.substr(1)));
}

u32 parseChannel(const std::string& tok)
{
    const std::size_t pos = tok.find("ch");
    REQUIRE(pos != std::string::npos);
    return static_cast<u32>(std::stoul(tok.substr(pos + 2)));
}

void checkNameLine(const std::vector<std::string>& t)
{
    // name g{G} {registry|audio.chN|feat.chN|ctrl} <logical>
    REQUIRE(t.size() == 4);
    const u32 group = parseGroup(t[1]);
    const std::string& kindTok = t[2];
    scvb::SegmentKind kind = scvb::SegmentKind::kRegistry;
    u32 channel = 0;
    if (kindTok == "registry")
    {
        kind = scvb::SegmentKind::kRegistry;
    }
    else if (kindTok == "viz")
    {
        kind = scvb::SegmentKind::kViz;
    }
    else if (kindTok == "ctrl")
    {
        kind = scvb::SegmentKind::kCtrl;
    }
    else if (kindTok.rfind("audio.ch", 0) == 0)
    {
        kind = scvb::SegmentKind::kAudio;
        channel = parseChannel(kindTok);
    }
    else if (kindTok.rfind("feat.ch", 0) == 0)
    {
        kind = scvb::SegmentKind::kFeat;
        channel = parseChannel(kindTok);
    }
    else
    {
        FAIL("未知段名 kind: " << kindTok);
    }
    INFO("segmentLogicalName(" << group << ", " << kindTok << ")");
    REQUIRE(narrow(scvb::segmentLogicalName(group, kind, channel)) == t[3]);
}
} // namespace

TEST_CASE("IpcLayoutFreeze", "[ipc][layout]")
{
    const std::vector<std::string> lines = readLines(goldenPath());
    REQUIRE_FALSE(lines.empty());

    for (const std::string& line : lines)
    {
        INFO("golden line: " << line);
        const std::vector<std::string> t = split(line);
        REQUIRE_FALSE(t.empty());

        if (t[0] == "abi")
        {
            REQUIRE(t.size() == 2);
            REQUIRE(scvb::kScvbAbi == static_cast<u32>(std::stoul(t[1])));
        }
        else if (t[0] == "magic")
        {
            REQUIRE(t.size() == 2);
            REQUIRE(hex32(scvb::kScvbMagic) == t[1]);
        }
        else if (t[0] == "max_channels")
        {
            REQUIRE(t.size() == 2);
            REQUIRE(scvb::kMaxChannels == static_cast<u32>(std::stoul(t[1])));
        }
        else if (t[0] == "max_groups")
        {
            REQUIRE(t.size() == 2);
            REQUIRE(scvb::kMaxGroups == static_cast<u32>(std::stoul(t[1])));
        }
        else if (t[0] == "default_ring_frames")
        {
            REQUIRE(t.size() == 2);
            REQUIRE(scvb::kDefaultRingFrames == static_cast<u32>(std::stoul(t[1])));
        }
        else if (t[0] == "feat_capacity_hops")
        {
            REQUIRE(t.size() == 2);
            REQUIRE(scvb::kFeatCapacityHops == static_cast<u32>(std::stoul(t[1])));
        }
        else if (t[0] == "budget_registry_bytes")
        {
            REQUIRE(t.size() == 2);
            const std::size_t budget = static_cast<std::size_t>(std::stoull(t[1]));
            const std::size_t used =
                sizeof(scvb::RegistryHeader) + 15 * sizeof(scvb::InputSlot) + sizeof(scvb::OutputSlot);
            REQUIRE(used <= budget);
        }
        else if (t[0] == "budget_ctrl_bytes")
        {
            REQUIRE(t.size() == 2);
            const std::size_t budget = static_cast<std::size_t>(std::stoull(t[1]));
            REQUIRE(sizeof(scvb::OutputGlobalInfo) <= budget);
        }
        else if (t[0] == "viz_columns")
        {
            REQUIRE(t.size() == 2);
            REQUIRE(scvb::kVizColumns == static_cast<u32>(std::stoul(t[1])));
        }
        else if (t[0] == "viz_coverage_words")
        {
            REQUIRE(t.size() == 2);
            REQUIRE(scvb::kVizCoverageWords == static_cast<u32>(std::stoul(t[1])));
        }
        else if (t[0] == "viz_pan_scale")
        {
            REQUIRE(t.size() == 2);
            REQUIRE(scvb::kVizPanScale == std::stol(t[1]));
        }
        else if (t[0] == "viz_label_words")
        {
            REQUIRE(t.size() == 2);
            REQUIRE(scvb::kVizLabelWords == static_cast<u32>(std::stoul(t[1])));
        }
        else if (t[0] == "viz_label_bytes")
        {
            REQUIRE(t.size() == 2);
            REQUIRE(scvb::kVizLabelBytes == static_cast<u32>(std::stoul(t[1])));
        }
        else if (t[0] == "viz_pan_none")
        {
            REQUIRE(t.size() == 2);
            REQUIRE(static_cast<long>(scvb::kVizPanNone) == std::stol(t[1]));
        }
        else if (t[0] == "budget_viz_bytes")
        {
            REQUIRE(t.size() == 2);
            const std::size_t budget = static_cast<std::size_t>(std::stoull(t[1]));
            REQUIRE(scvb::kVizSegmentSize == budget);
            REQUIRE(scvb::kVizLanesOffset + sizeof(scvb::VizLanes) <= budget);
        }
        else if (t[0] == "offset")
        {
            REQUIRE(t.size() == 3);
            const std::size_t off = vizOffset(t[1]);
            INFO("offset key: " << t[1]);
            REQUIRE(off != kNotFound);
            REQUIRE(off == static_cast<std::size_t>(std::stoull(t[2])));
        }
        else if (t[0] == "name")
        {
            checkNameLine(t);
        }
        else if (t[0] == "struct")
        {
            REQUIRE(t.size() == 6);
            const std::size_t size = structSize(t[1]);
            const std::size_t align = structAlign(t[1]);
            REQUIRE(size != kNotFound);
            REQUIRE(align != kNotFound);
            REQUIRE(size == static_cast<std::size_t>(std::stoull(t[3])));
            REQUIRE(align == static_cast<std::size_t>(std::stoull(t[5])));
        }
        else if (t[0] == "field")
        {
            REQUIRE(t.size() == 4);
            const std::size_t dot = t[1].find('.');
            REQUIRE(dot != std::string::npos);
            const std::string structName = t[1].substr(0, dot);
            const std::string fieldName = t[1].substr(dot + 1);
            const std::size_t off = fieldOffset(structName, fieldName);
            INFO(structName << "." << fieldName);
            REQUIRE(off != kNotFound);
            REQUIRE(off == static_cast<std::size_t>(std::stoull(t[3])));
        }
        else
        {
            FAIL("未知 golden 行类型: " << t[0]);
        }
    }
}

TEST_CASE("OutputGlobalInfo layout matches 01 4.4-a", "[ipc][layout]")
{
    static_assert(sizeof(scvb::OutputGlobalInfo) == 256);
    static_assert(offsetof(scvb::OutputGlobalInfo, capture_enabled) == 0);
    static_assert(offsetof(scvb::OutputGlobalInfo, output_sample_rate) == 4);
    static_assert(offsetof(scvb::OutputGlobalInfo, flags) == 8);
    static_assert(offsetof(scvb::OutputGlobalInfo, gap_count) == 16);
    static_assert(offsetof(scvb::OutputGlobalInfo, overlap_count) == 76);
    static_assert(offsetof(scvb::OutputGlobalInfo, epoch_summary) == 136);
    static_assert(alignof(scvb::OutputGlobalInfo) == 64);

    REQUIRE(sizeof(scvb::OutputGlobalInfo) == 256);
    REQUIRE(alignof(scvb::OutputGlobalInfo) == 64);
    REQUIRE(offsetof(scvb::OutputGlobalInfo, gap_count) == 16);
    REQUIRE(offsetof(scvb::OutputGlobalInfo, overlap_count) == 76);
    REQUIRE(offsetof(scvb::OutputGlobalInfo, epoch_summary) == 136);
}

TEST_CASE("CtrlRecord freezes atomic fields (size 24, offsets)", "[ipc][layout]")
{
    // [J48 先例] 四字段 atomic 化,尺寸/偏移不变(非布局改动);offsetof 对 atomic 成员成立。
    static_assert(std::is_same_v<decltype(scvb::CtrlRecord::seq), std::atomic<u32>>);
    static_assert(std::is_same_v<decltype(scvb::CtrlRecord::channel), std::atomic<u32>>);
    static_assert(std::is_same_v<decltype(scvb::CtrlRecord::op), std::atomic<u32>>);
    static_assert(std::is_same_v<decltype(scvb::CtrlRecord::value), std::atomic<u64>>);
    static_assert(sizeof(scvb::CtrlRecord) == 24);
    static_assert(offsetof(scvb::CtrlRecord, op) == 8);
    static_assert(offsetof(scvb::CtrlRecord, value) == 16);

    REQUIRE(sizeof(scvb::CtrlRecord) == 24);
    REQUIRE(offsetof(scvb::CtrlRecord, value) == 16);
}

TEST_CASE("AudioRingHeader.channels is u32 at offset 16", "[ipc][layout]")
{
    static_assert(std::is_same_v<decltype(scvb::AudioRingHeader::channels), u32>);
    static_assert(offsetof(scvb::AudioRingHeader, channels) == 16);

    REQUIRE(offsetof(scvb::AudioRingHeader, channels) == 16);
}

TEST_CASE("InputSlot.flags is lock-free atomic<u32>", "[ipc][layout]")
{
    static_assert(std::is_same_v<decltype(scvb::InputSlot::flags), std::atomic<u32>>);
    static_assert(std::atomic<u32>::is_always_lock_free);

    REQUIRE(offsetof(scvb::InputSlot, flags) == 32);
}

TEST_CASE("golden text must not mention blockCounter", "[ipc][layout]")
{
    std::ifstream f(goldenPath());
    REQUIRE(f.good());
    std::ostringstream oss;
    oss << f.rdbuf();
    REQUIRE(oss.str().find("blockCounter") == std::string::npos);
}

#ifdef WIN32
TEST_CASE("SegmentBackendWin32 full names equal Local prefix + logical", "[ipc][names]")
{
    REQUIRE(scvb::segmentRegistryName(1) == L"Local\\SynchainSCVB.v1.g1.registry");
    REQUIRE(scvb::segmentAudioName(8, 15) == L"Local\\SynchainSCVB.v1.g8.audio.ch15");
    REQUIRE(scvb::segmentFeatName(8, 15) == L"Local\\SynchainSCVB.v1.g8.feat.ch15");
    REQUIRE(scvb::segmentCtrlName(8) == L"Local\\SynchainSCVB.v1.g8.ctrl");
}
#endif

// [SL-254 / J95①] Registry::outputSlotAtBase —— 音频线程用的「租约基址 + 冻结偏移」寻址。
// 复审 r1【重要】:新增 core 纯逻辑须有 Catch2 用例(CLAUDE.md §7)。
TEST_CASE("SL-254:outputSlotAtBase 与冻结偏移一致、空基址返回 nullptr", "[ipc][layout][SL254]")
{
    REQUIRE(scvb::Registry::outputSlotAtBase(nullptr) == nullptr);

    // 用一块与段同尺寸的普通缓冲当基址:只验寻址算术,不建段。
    std::vector<char> buf(scvb::kRegistrySegmentSize, 0);
    void* base = buf.data();
    auto* os = scvb::Registry::outputSlotAtBase(base);
    REQUIRE(os != nullptr);

    // ★ 与冻结布局逐字对齐:偏移必须正好是 kOutputSlotOffset,不能是「碰巧能跑」的别的值。
    const auto delta = reinterpret_cast<char*>(os) - static_cast<char*>(base);
    REQUIRE(static_cast<std::size_t>(delta) == scvb::kOutputSlotOffset);
    REQUIRE(scvb::kOutputSlotOffset == sizeof(scvb::RegistryHeader) + scvb::kMaxChannels * sizeof(scvb::InputSlot));
    // 落在段内(尾部不越界)。
    REQUIRE(static_cast<std::size_t>(delta) + sizeof(scvb::OutputSlot) <= scvb::kRegistrySegmentSize);
}
