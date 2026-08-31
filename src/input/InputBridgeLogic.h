// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// InputBridgeLogic —— T30 Input 桥的纯载荷/判定逻辑(无 WebView2、无 GUI,离线 Catch2 可测)。
// claim 六值映射(§5.2/R3)、srMismatch 推导(§4.1/§5.2)、remoteSetPriority 拒绝判定(§3.4/§5.6)、
// 五个事件与首帧快照的载荷构造(§3.1/§4.1-§4.5,键名逐字照契约)。IPC 数据的采集在
// InputProcessor/InputEditor,本层只做「快照 → 载荷」的纯变换。

#include <juce_core/juce_core.h>

#include "input/InputSession.h"
#include "ipc/CtrlPlane.h" // CtrlBroadcastSnapshot(§4.3 配置广播区)

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
// 非活跃态(conflict/abiMismatch/unavailable,不持有 slot)→ unassigned(§5.2 未 claim 任何 slot;
//   SPSC 纪律:非持有者不得写命令环,见 InputProcessor::bridgeRemoteSetPriority);
// 命令环满 → ringFull(满环仍投递:写方覆盖最旧 + 溢出计数,回执 queued:false 提示重试)。
enum class PriorityReject
{
    kNone,
    kRingFull,
    kOutputOffline,
    kUnassigned,
    kNotActive
};

PriorityReject priorityRejection(int channelId, bool outputOnline, bool ringFull, bool active);
juce::String priorityRejectReason(PriorityReject r); // kNone → ""

// --- diff-then-emit 缓存推进(§0.4 变化才发 + 首帧必发;PR#54 R4)------------------
// 判定:json == lastJson → false 不重发;json != lastJson 且 visible → 推进 lastJson 并返回
// true(调用方 emit);json != lastJson 且不可见 → 保持 lastJson 并返回 false —— 隐藏期
// emitEventIfBrowserIsVisible 会丢弃事件,缓存不推进则恢复可见后下一 tick 因 json != lastJson
// 自然重发(避免隐藏期变化永久丢失 → UI 陈旧)。
bool advanceEmitCache(const juce::String& json, juce::String& lastJson, bool visible);

// §4.5 claim 边沿消费判定(PR#54 R5,与 advanceEmitCache 同口径):有 error 边沿(needsError =
// claim/prev 为 conflict/srMismatch)时仅当 visible(事件已实际发出)才消费并推进基线;无 error
// 边沿恒消费。隐藏 + error 边沿 → 不消费 → 基线保持 → 恢复可见后下一 tick 重发。
bool claimEdgeConsumed(bool needsError, bool visible);

// §4.5 error 边沿键检测(PR#54 R6):(claim, channelId, groupId, inputSr, outputSr)任一变化即
// true —— 同 claim 换组(conflict 的 detail.groupId)、srMismatch 的 SR 变化都须重发,否则 UI
// 横幅陈旧。
bool claimErrorEdgeChanged(const juce::String& claim, int channelId, int groupId, int inputSr, int outputSr,
                           const juce::String& lastClaim, int lastChannelId, int lastGroupId, int lastInputSr,
                           int lastOutputSr);

// §4.3 config_seq 基线推进(PR#54 R7,与 advanceEmitCache/claimEdgeConsumed 同口径):config_seq
// 变化即需重发,仅当事件实际发出(emitted)时推进基线并返回 true;隐藏时保持基线返回 false,恢复
// 可见后下一 tick 因 seq 仍 != 基线而重发。
bool advanceConfigSeq(u32 configSeq, bool emitted, u32& lastConfigSeq);

// --- 数值参数解析(§0.8.2 类型不符/越界 → 拒绝)------------------------------------
// 非数值(缺参/字符串/对象/null)→ 空 Optional,调用方回 {ok:false, reason:"badArg"};
// 数值(JS number 走 double,截断)且在 int 全域内 → 值;double 越界/NaN → 空(cast 前挡下,
// 处理器 clamp 只处理 int 域内的业务范围)。绝不把非数值静默夹取为 0:0 是 setChannelId「释放」
// 与 remoteSetPriority 的合法业务值。
juce::Optional<int> parseIntArg(const juce::Array<juce::var>& args);

// --- 载荷构造(键名与契约逐字一致;全部返回新 DynamicObject)--------------------------
// §4.1 scvb.state:{channel_id, group_id, claim, abi, abi_remote?, ui:{scale, language, guide_seen}}。
// abi_remote 仅 abiMismatch 且探测到(≠0)时存在(§4.1 字段纪律:探测不到则字段不存在)。
// [SL-258] `ui.guide_seen` 是 §4.1 载荷行逐字要求的第三个 ui 字段,也是 `setGuideSeen` 写入后的
// 回推路径;它与 §3.1 快照的 ui 子树**字段集必须一致**(§4.1 字段纪律行),两处一起改。
juce::var buildStatePayload(int channelId, int groupId, const juce::String& claim, u32 abi,
                            const juce::Optional<u32>& abiRemote, float uiScale, const juce::String& lang,
                            bool guideSeen);

// §4.2 scvb.conn:{outputOnline, maskBit, capturing, passthrough, passthroughPending, occupiedMask}。
juce::var buildConnPayload(const InputConnSnapshot& s);

// §4.3 scvb.config 快照。数据源 = ctrl 广播区(CtrlBroadcast,Output [M] 写 / Input [M] 读):
// label/priority/lead_lock/pair_id/freeze/channelLabels 全部来自本组主 Output 的实况。
// **不含 lead_vol_exempt**:§4.3 的载荷是逐字冻结的九键,该字段属于 Output 侧的
// scvb.state.channels[](§2.1/§4.1),Input 页没有消费面 —— 与 A-32 否决「其余通道 priority/
// lead/pair 下推」是同一条理由。广播区里仍镜像着它的 flag 位,只是不上 Input 的桥。
// 广播区读不到(Output 离线 / seqlock 撕裂 / 段未打开)时 broadcastValid=false,调用方沿用上帧,
// 载荷退回默认值 —— 此前这些字段是**恒定硬编码**,Output 改什么 Input 都看不见(T37 三轮 C 族)。
struct ConfigSnapshot
{
    int sourceChannels = 1; // Input 实测 1|2([J57]);恒本机真源,不吃广播区
    u32 configSeq = 0; // 广播区 config_seq(变化检测真源)
    bool broadcastValid = false; // 本次是否读到有效广播区
    int channelId = 0; // 本实例 channel(1..15;0=未分配 → 无「本轨」配置可取)
    CtrlBroadcastSnapshot broadcast{}; // 全组 15 轨配置镜像 + label 表
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

// §3.1 首帧快照 InputSnapshot:{channel_id, group_id, role:"input", conn, config,
//   ui:{scale, language, guide_seen}, guide_seen_global, version:{plugin, abi}}。
// claim 不经快照回推(唯一通道 = §4.1 scvb.state,§3.1 无 claim 键)。
// [SL-258] `guide_seen_global` 挂**顶层**而不进 ui:§3.1 语义行明写它「只读、不属工程 state」。
juce::var buildInputSnapshot(int channelId, int groupId, const InputConnSnapshot& conn, const ConfigSnapshot& config,
                             float uiScale, const juce::String& lang, bool guideSeen, bool guideSeenGlobal,
                             const juce::String& pluginVersion, u32 abi);

} // namespace scvb::input::bridge
