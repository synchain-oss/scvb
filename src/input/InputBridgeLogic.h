// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// InputBridgeLogic —— T30 Input 桥的纯载荷/判定逻辑(无 WebView2、无 GUI,离线 Catch2 可测)。
// claim 六值映射(§5.2/R3)、srMismatch 推导(§4.1/§5.2)、remoteSetPriority 拒绝判定(§3.4/§5.6)、
// 五个事件与首帧快照的载荷构造(§3.1/§4.1-§4.5,键名逐字照契约)。IPC 数据的采集在
// InputProcessor/InputEditor,本层只做「快照 → 载荷」的纯变换。

#include <juce_core/juce_core.h>

#include "input/InputSession.h"

namespace scvb::input::bridge
{

// --- claim 六值(§5.2;[R3 收口] 与 01 §6.3/05 §1.4 逐字一致)--------------------------
// state + maskBit(本组 connected_mask 本位)+ srMismatch(Output 已报 SR 且 ≠ 本机 SR)映射到六值:
//   unassigned | idle | active | conflict | abiMismatch | srMismatch。
// kUnavailable(I0 段未打开)→ idle;srMismatch 只在 kActive 上成立且优先于 active/idle。
juce::String claimValue(InputClaimState state, bool maskBit, bool srMismatch);

// srMismatch 推导(§4.1):claim 态为 kActive ∧ Output 已报非零 SR ∧ ≠ 本机 SR。
bool srMismatch(InputClaimState state, u32 outputSampleRate, u32 localSampleRate);

// --- remoteSetPriority 拒绝判定(§3.4/§5.6)-----------------------------------------
// 判定顺序(全部不满足 = 投递):channel_id==0 → unassigned;Output 离线 → outputOffline;
// 命令环满 → ringFull(满环仍投递:写方覆盖最旧 + 溢出计数,回执 queued:false 提示重试)。
enum class PriorityReject
{
    kNone,
    kRingFull,
    kOutputOffline,
    kUnassigned
};

PriorityReject priorityRejection(int channelId, bool outputOnline, bool ringFull);
juce::String priorityRejectReason(PriorityReject r); // kNone → ""

// --- 数值参数解析(§0.8.2 类型不符/越界 → 拒绝)------------------------------------
// 非数值(缺参/字符串/对象/null)→ 空 Optional,调用方回 {ok:false, reason:"badArg"};
// 数值(JS number 走 double,截断)且在 int 全域内 → 值;double 越界/NaN → 空(cast 前挡下,
// 处理器 clamp 只处理 int 域内的业务范围)。绝不把非数值静默夹取为 0:0 是 setChannelId「释放」
// 与 remoteSetPriority 的合法业务值。
juce::Optional<int> parseIntArg(const juce::Array<juce::var>& args);

// --- 载荷构造(键名与契约逐字一致;全部返回新 DynamicObject)--------------------------
// §4.1 scvb.state:{channel_id, group_id, claim, abi, abi_remote?, ui:{scale, language}}。
// abi_remote 仅 abiMismatch 且探测到(≠0)时存在(§4.1 字段纪律:探测不到则字段不存在)。
juce::var buildStatePayload(int channelId, int groupId, const juce::String& claim, u32 abi,
                            const juce::Optional<u32>& abiRemote, float uiScale, const juce::String& lang);

// §4.2 scvb.conn:{outputOnline, maskBit, capturing, passthrough, passthroughPending, occupiedMask}。
juce::var buildConnPayload(const InputConnSnapshot& s);

// §4.3 scvb.config 快照。广播区布局仍是占位(CtrlPlane kCtrlBroadcastBytes「T25/params v2.x 填内容」):
// label/priority/lead_lock/pair_id/freeze/channelLabels 一律回退默认值,只回本机实测 source_channels
// 与 OutputSlot.config_seq —— 不自行发明广播布局(那是 params 契约面)。
struct ConfigSnapshot
{
    int sourceChannels = 1; // Input 实测 1|2([J57])
    u32 configSeq = 0; // 本组 OutputSlot.config_seq(变化检测真源)
};
juce::var buildConfigPayload(const ConfigSnapshot& s);

// §4.4 scvb.groups:{groups_online: u8}(bit0=组A … bit7=组H)。
juce::var buildGroupsPayload(int groupsOnline);

// §4.5 scvb.error:{code, ch?, detail, active}。ch ≤0 时字段不存在。
juce::var buildErrorPayload(const juce::String& code, int ch, const juce::var& detail, bool active);

// §3.4 remoteSetPriority 回执:{queued:true} | {queued:false, reason}。
juce::var buildPriorityResponse(bool queued, const juce::String& reason);

// §5.6 拒绝语义 {conflict:true}(Input setChannelId/setGroupId)。
juce::var conflictResponse();

// §3.1 首帧快照 InputSnapshot:{channel_id, group_id, role:"input", conn, config, ui:{scale, language},
//   version:{plugin, abi}}。claim 不经快照回推(唯一通道 = §4.1 scvb.state,§3.1 无 claim 键)。
juce::var buildInputSnapshot(int channelId, int groupId, const InputConnSnapshot& conn, const ConfigSnapshot& config,
                             float uiScale, const juce::String& lang, const juce::String& pluginVersion, u32 abi);

} // namespace scvb::input::bridge
