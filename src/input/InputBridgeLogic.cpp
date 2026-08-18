// SPDX-License-Identifier: GPL-3.0-or-later
#include "InputBridgeLogic.h"

#include "ipc/SegmentLayout.h" // kMaxChannels(§4.3 channelLabels 15 张卡)

namespace scvb::input::bridge
{

juce::String claimValue(InputClaimState state, bool maskBit, bool srMismatch)
{
    switch (state)
    {
    case InputClaimState::kUnassigned:
        return "unassigned";
    case InputClaimState::kConflict:
        return "conflict";
    case InputClaimState::kAbiMismatch:
        return "abiMismatch";
    case InputClaimState::kUnavailable:
        return "idle"; // I0 段未打开(§5.2 idle:slot 已声明但 Output 尚未健康读取)
    case InputClaimState::kActive:
        return srMismatch ? "srMismatch" : (maskBit ? "active" : "idle");
    }
    return "idle"; // 防御性:未知枚举值不崩溃、不静默(绝无 half-compat,J40)
}

bool srMismatch(InputClaimState state, u32 outputSampleRate, u32 localSampleRate)
{
    return state == InputClaimState::kActive && outputSampleRate != 0 && outputSampleRate != localSampleRate;
}

PriorityReject priorityRejection(int channelId, bool outputOnline, bool ringFull)
{
    if (channelId == 0)
    {
        return PriorityReject::kUnassigned;
    }
    if (!outputOnline)
    {
        return PriorityReject::kOutputOffline;
    }
    if (ringFull)
    {
        return PriorityReject::kRingFull;
    }
    return PriorityReject::kNone;
}

juce::String priorityRejectReason(PriorityReject r)
{
    switch (r)
    {
    case PriorityReject::kUnassigned:
        return "unassigned";
    case PriorityReject::kOutputOffline:
        return "outputOffline";
    case PriorityReject::kRingFull:
        return "ringFull";
    case PriorityReject::kNone:
        break;
    }
    return {};
}

juce::var buildStatePayload(int channelId, int groupId, const juce::String& claim, u32 abi,
                            const juce::Optional<u32>& abiRemote, float uiScale, const juce::String& lang)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("channel_id", channelId);
    o->setProperty("group_id", groupId);
    o->setProperty("claim", claim);
    o->setProperty("abi", static_cast<int>(abi));
    if (abiRemote.hasValue())
    {
        o->setProperty("abi_remote", static_cast<int>(*abiRemote));
    }
    auto* ui = new juce::DynamicObject();
    ui->setProperty("scale", uiScale);
    ui->setProperty("language", lang);
    o->setProperty("ui", juce::var(ui));
    return juce::var(o);
}

juce::var buildConnPayload(const InputConnSnapshot& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("outputOnline", s.outputOnline);
    o->setProperty("maskBit", s.maskBit);
    o->setProperty("capturing", s.capturing);
    o->setProperty("passthrough", s.passthrough);
    o->setProperty("passthroughPending", s.passthroughPending);
    o->setProperty("occupiedMask", static_cast<int>(s.occupiedMask));
    return juce::var(o);
}

juce::var buildConfigPayload(const ConfigSnapshot& s)
{
    auto* o = new juce::DynamicObject();
    // 广播区占位回退默认值(见头注释);source_channels 为本机实测([J57]),participate 默认值
    // 推导 = mono 参与/stereo 不参与(J60/T32 同口径),config_seq 为唯一变化检测真源。
    o->setProperty("label", juce::String());
    o->setProperty("priority", 0);
    o->setProperty("lead_lock", false);
    o->setProperty("pair_id", 0);
    o->setProperty("freeze", 0);
    o->setProperty("source_channels", s.sourceChannels);
    o->setProperty("participate_in_auto_pan", s.sourceChannels == 1);
    o->setProperty("config_seq", static_cast<int>(s.configSeq));
    juce::Array<juce::var> labels;
    for (u32 i = 0; i < kMaxChannels; ++i)
    {
        labels.add(juce::var(juce::String())); // 15 张卡 label 全空(A-32;广播区未填)
    }
    o->setProperty("channelLabels", labels);
    return juce::var(o);
}

juce::var buildGroupsPayload(int groupsOnline)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("groups_online", groupsOnline);
    return juce::var(o);
}

juce::var buildErrorPayload(const juce::String& code, int ch, const juce::var& detail, bool active)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("code", code);
    if (ch > 0)
    {
        o->setProperty("ch", ch);
    }
    o->setProperty("detail", detail);
    o->setProperty("active", active);
    return juce::var(o);
}

juce::var buildPriorityResponse(bool queued, const juce::String& reason)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("queued", queued);
    if (!queued)
    {
        o->setProperty("reason", reason);
    }
    return juce::var(o);
}

juce::var conflictResponse()
{
    auto* o = new juce::DynamicObject();
    o->setProperty("conflict", true);
    return juce::var(o);
}

juce::var buildInputSnapshot(int channelId, int groupId, const InputConnSnapshot& conn, const ConfigSnapshot& config,
                             float uiScale, const juce::String& lang, const juce::String& pluginVersion, u32 abi)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("channel_id", channelId);
    o->setProperty("group_id", groupId);
    o->setProperty("role", "input");
    o->setProperty("conn", buildConnPayload(conn));
    o->setProperty("config", buildConfigPayload(config));
    auto* ui = new juce::DynamicObject();
    ui->setProperty("scale", uiScale);
    ui->setProperty("language", lang);
    o->setProperty("ui", juce::var(ui));
    auto* version = new juce::DynamicObject();
    version->setProperty("plugin", pluginVersion);
    version->setProperty("abi", static_cast<int>(abi));
    o->setProperty("version", juce::var(version));
    return juce::var(o);
}

} // namespace scvb::input::bridge
