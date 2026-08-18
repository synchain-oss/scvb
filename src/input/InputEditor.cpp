// SPDX-License-Identifier: GPL-3.0-or-later
#include "InputEditor.h"

#include "InputBridgeApi.h"
#include "InputBridgeLogic.h"
#include "InputProcessor.h"

#include "BridgeBase.h"

#include <utility>

namespace scvb::input
{
namespace
{
using WBC = juce::WebBrowserComponent;

// §3.2/§3.3 参数夹取口径:非数值 → 0(→ setChannelId 0=释放 / setGroupId 0 被 clamp 到 1);
// 范围越界由处理器 clamp(契约返回值只有 {ok}/{conflict:true},无 badArg 分支)。
int intArgOrZero(const juce::Array<juce::var>& args)
{
    if (args.size() > 0 && (args[0].isInt() || args[0].isDouble()))
    {
        return static_cast<int>(args[0]);
    }
    return 0;
}
} // namespace

InputEditor::InputEditor(ScvbInputAudioProcessor& processor)
    : WebViewHost(processor,
                  [&processor, this]() {
                      scvb::webview::WebViewHost::Config c;
                      c.role = "input";
                      c.userDataFolderName = "SCVBInputWV2";
                      c.version = JucePlugin_VersionString;
                      c.lang = processor.bridgeUiLanguage();
                      c.uiScale = static_cast<float>(processor.bridgeUiScalePercent()) / 100.0f;
                      c.channelLimit = 15;
                      c.resourceSource = {}; // web/input 资源由 T36 嵌入;T30 桥不嵌资源
                      c.augmentOptions = [this](juce::WebBrowserComponent::Options& options) {
                          registerNativeFunctions(options);
                      };
                      return c;
                  }()),
      processor_(processor), pluginVersion_(JucePlugin_VersionString)
{
}

void InputEditor::registerNativeFunctions(juce::WebBrowserComponent::Options& options)
{
    options = options
                  .withNativeFunction(juce::Identifier(bridge::kFnSetChannelId),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleSetChannelId(a, std::move(c));
                                      })
                  .withNativeFunction(juce::Identifier(bridge::kFnSetGroupId),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleSetGroupId(a, std::move(c));
                                      })
                  .withNativeFunction(juce::Identifier(bridge::kFnRemoteSetPriority),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleRemoteSetPriority(a, std::move(c));
                                      });
}

juce::var InputEditor::buildSnapshot()
{
    const auto snap = processor_.bridgeTickSnapshot();
    const bool srMis = bridge::srMismatch(snap.claimState, snap.globalInfo.output_sample_rate,
                                          static_cast<scvb::u32>(snap.sampleRate));
    const juce::String claim = bridge::claimValue(snap.claimState, snap.conn.maskBit, srMis);

    // 首帧后以当前 config_seq/claim 为 diff 基线:scvb.config 只在 seq 变化再发、error 只发迁移边沿。
    lastConfigSeq_ = snap.configSeq;
    lastClaim_ = claim;

    scvb::input::InputConnSnapshot conn = snap.conn;
    conn.passthrough = snap.passthrough;
    conn.passthroughPending = !snap.healthy && !snap.passthrough;

    bridge::ConfigSnapshot cfg;
    cfg.sourceChannels = snap.sourceChannels;
    cfg.configSeq = snap.configSeq;

    return bridge::buildInputSnapshot(snap.channelId, snap.groupId, conn, cfg, uiScale(), lang(), pluginVersion_,
                                      snap.localAbi);
}

void InputEditor::emitTick()
{
    const auto now = scvb::steadyNowMs();
    const auto snap = processor_.bridgeTickSnapshot();

    const bool srMis = bridge::srMismatch(snap.claimState, snap.globalInfo.output_sample_rate,
                                          static_cast<scvb::u32>(snap.sampleRate));
    const juce::String claim = bridge::claimValue(snap.claimState, snap.conn.maskBit, srMis);

    // scvb.state:变化即发(首帧必发;§4.1)。
    juce::Optional<scvb::u32> abiRemote;
    if (claim == "abiMismatch" && snap.remoteAbi != 0)
    {
        abiRemote = snap.remoteAbi; // 探测不到 → 字段不存在(§4.1 字段纪律)
    }
    emitIfChanged(
        bridge::kEvState,
        bridge::buildStatePayload(snap.channelId, snap.groupId, claim, snap.localAbi, abiRemote, uiScale(), lang()),
        lastStateJson_);

    // scvb.conn:~4Hz diff-then-emit(§4.2;滞回窗口 = 不健康且目标仍为静音,J32)。
    if (now - lastConnMs_ >= scvb::kHeartbeatIntervalMs)
    {
        lastConnMs_ = now;
        scvb::input::InputConnSnapshot conn = snap.conn;
        conn.passthrough = snap.passthrough;
        conn.passthroughPending = !snap.healthy && !snap.passthrough;
        emitIfChanged(bridge::kEvConn, bridge::buildConnPayload(conn), lastConnJson_);
    }

    // scvb.config:25Hz 轮询,config_seq 变化才发(§4.3)。
    if (snap.configSeq != lastConfigSeq_)
    {
        lastConfigSeq_ = snap.configSeq;
        bridge::ConfigSnapshot cfg;
        cfg.sourceChannels = snap.sourceChannels;
        cfg.configSeq = snap.configSeq;
        emitIfChanged(bridge::kEvConfig, bridge::buildConfigPayload(cfg), lastConfigJson_);
    }

    // scvb.groups:1Hz diff-then-emit(§4.4)。
    if (now - lastGroupsMs_ >= 1000)
    {
        lastGroupsMs_ = now;
        emitIfChanged(bridge::kEvGroups, bridge::buildGroupsPayload(processor_.bridgeGroupsOnline()), lastGroupsJson_);
    }

    // scvb.error:claim 态迁移边沿(§4.5;conflict/srMismatch 进出,abi 不占 error code)。
    if (claim != lastClaim_)
    {
        const juce::String prev = lastClaim_;
        lastClaim_ = claim;
        emitClaimError(claim, prev, snap.channelId, snap.groupId, juce::roundToInt(snap.sampleRate),
                       static_cast<int>(snap.globalInfo.output_sample_rate));
    }
}

void InputEditor::handleSetLang(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
{
    WebViewHost::handleSetLang(args, std::move(complete)); // 归一化 {zh,en,fr} + 回执 {ok:true}
    processor_.bridgeSetUiLanguage(lang()); // §3.5:落 Input state(实际生效值经 scvb.state 回推)
}

void InputEditor::persistUiScaleAsDefault()
{
    // §3.5:commitUiScale 防呆确认后落 Input state(uiScale 百分数,params-v0 §三)。
    processor_.bridgeSetUiScalePercent(juce::roundToInt(uiScale() * 100.0f));
}

void InputEditor::handleSetChannelId(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
{
    const auto st = processor_.setChannelId(intArgOrZero(args)); // 内部 clamp 0..15;n=0 = 释放
    complete(st == InputClaimState::kConflict ? bridge::conflictResponse() : scvb::bridge::okResponse());
}

void InputEditor::handleSetGroupId(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
{
    const auto st = processor_.setGroupId(intArgOrZero(args)); // 内部 clamp 1..8;同组 = no-op
    complete(st == InputClaimState::kConflict ? bridge::conflictResponse() : scvb::bridge::okResponse());
}

void InputEditor::handleRemoteSetPriority(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
{
    const auto r = processor_.bridgeRemoteSetPriority(intArgOrZero(args)); // 内部 clamp 0..10
    complete(bridge::buildPriorityResponse(r.queued, r.reason));
}

void InputEditor::emitIfChanged(const char* name, const juce::var& payload, juce::String& lastJson)
{
    const juce::String json = juce::JSON::toString(payload);
    if (json == lastJson)
    {
        return;
    }
    lastJson = json;
    webView().emitEventIfBrowserIsVisible(juce::Identifier(name), payload);
}

void InputEditor::emitClaimError(const juce::String& claim, const juce::String& prevClaim, int channelId, int groupId,
                                 int localSr, int outputSr)
{
    if (claim == "conflict" || prevClaim == "conflict")
    {
        auto* detail = new juce::DynamicObject();
        detail->setProperty("groupId", groupId);
        emitIfChanged(bridge::kEvError,
                      bridge::buildErrorPayload("channelConflict", channelId, juce::var(detail), claim == "conflict"),
                      lastErrorJson_);
    }
    if (claim == "srMismatch" || prevClaim == "srMismatch")
    {
        auto* detail = new juce::DynamicObject();
        detail->setProperty("inputSr", localSr);
        detail->setProperty("outputSr", outputSr);
        emitIfChanged(bridge::kEvError,
                      bridge::buildErrorPayload("srMismatch", channelId, juce::var(detail), claim == "srMismatch"),
                      lastErrorJson_);
    }
}

} // namespace scvb::input
