// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OutputBridgeApi —— Output 侧 JS↔C++ 桥函数/事件名的唯一 C++ 真源(契约 docs/SCVB_CONTRACT.md §7 manifest)。
// 由 scripts/check-bridge-parity.mjs 的 C 侧比对逐名抽取:每条 constexpr 赋值里的字符串字面量,
// 以 "scvb." 开头判为事件名、其余 lowerCamelCase 判为函数名。本头**只含桥名**,不放任何辅助字符串
// (拒绝态 reason 等一律在 OutputEditor.cpp 内联,避免被 parity 误判为函数名/事件名)。
// 改名 = 改契约,须走 SCVB_CONTRACT.md §9「只增不改」流程并同批更新 bridge.js 与 parity 脚本。

namespace scvb::outputbridge
{

// JS -> C++ 原生函数名(契约 §1,共 36 个;requestInitialState/setLang/setUiScale/commitUiScale
// 四个通用函数由 WebViewHost 装配,其余 32 个由 OutputEditor 注册)。
namespace Fn
{
inline constexpr const char* RequestInitialState = "requestInitialState";
inline constexpr const char* SetCaptureEnabled = "setCaptureEnabled";
inline constexpr const char* SetOutputEnabled = "setOutputEnabled";
inline constexpr const char* SetGroupId = "setGroupId";
inline constexpr const char* PreviewAnalyze = "previewAnalyze";
inline constexpr const char* Analyze = "analyze";
inline constexpr const char* CancelAnalyze = "cancelAnalyze";
inline constexpr const char* SetRange = "setRange";
inline constexpr const char* SetVersionActive = "setVersionActive";
inline constexpr const char* SetVersionName = "setVersionName";
inline constexpr const char* CopyVersion = "copyVersion";
inline constexpr const char* BeginParamGesture = "beginParamGesture";
inline constexpr const char* SetParam = "setParam";
inline constexpr const char* EndParamGesture = "endParamGesture";
inline constexpr const char* SetChannelConfig = "setChannelConfig";
inline constexpr const char* SetTrackManual = "setTrackManual";
inline constexpr const char* SetPanCurve = "setPanCurve";
inline constexpr const char* SetVadParams = "setVadParams";
inline constexpr const char* SetSegmentation = "setSegmentation";
inline constexpr const char* SetTransitionRamp = "setTransitionRamp";
inline constexpr const char* SetAnalysisConfig = "setAnalysisConfig";
inline constexpr const char* EditSegment = "editSegment";
inline constexpr const char* RecaptureArm = "recaptureArm";
inline constexpr const char* ClearCoverage = "clearCoverage";
inline constexpr const char* Undo = "undo";
inline constexpr const char* Redo = "redo";
inline constexpr const char* RequestWaveform = "requestWaveform";
inline constexpr const char* SetUiScale = "setUiScale";
inline constexpr const char* CommitUiScale = "commitUiScale";
inline constexpr const char* SetLang = "setLang";
inline constexpr const char* SetActiveTab = "setActiveTab";
inline constexpr const char* SetGuideSeen = "setGuideSeen";
inline constexpr const char* SetTourSeen = "setTourSeen";
inline constexpr const char* ConfirmPrintGuard = "confirmPrintGuard";
inline constexpr const char* SetMasterChartMode = "setMasterChartMode"; // [J75] T43
inline constexpr const char* ExportSuggestions = "exportSuggestions"; // [J81] T41 建议表 CSV 导出
} // namespace Fn

// C++ -> JS 事件名(契约 §2,共 9 个;由 OutputEditor 在 25Hz timer 内经 diff-then-emit 推送)。
namespace Event
{
inline constexpr const char* State = "scvb.state";
inline constexpr const char* Params = "scvb.params";
inline constexpr const char* Conn = "scvb.conn";
inline constexpr const char* Groups = "scvb.groups";
inline constexpr const char* Meters = "scvb.meters";
inline constexpr const char* Playhead = "scvb.playhead";
inline constexpr const char* CaptureProgress = "scvb.captureProgress";
inline constexpr const char* Segments = "scvb.segments";
inline constexpr const char* Error = "scvb.error";
} // namespace Event

} // namespace scvb::outputbridge
