// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OutputUiState —— 首启已读位在 PRMS(APVTS ValueTree)里的读写(T37 真机 bug A-3)。
//
// 为什么放 PRMS 而不是 CFGS:STATE_SCHEMA §三 的 chunk 表本来就把
// `ui{scale, language, active_tab, guide_seen, tour_seen}` 登记在 **PRMS** 名下,
// 而 CFGS 的 OutputStateCodec 是定长枚举式解码(总长必须逐字等于 24+langBytes)——
// 往它尾部追加字段会让任何**旧**构建整块拒载,而拒载路径是一句裸 return:
// group_id / 采集 / 输出 / 活动版本全部静默回默认(违反 §7.3「不得静默丢数据」)。
// 用户手上同时有新旧两个测试包、来回换着开同一个工程,这条路径是真会被踩到的。
//
// ValueTree(XML)两个方向都天生容忍字段增删:
//   • 旧构建读新工程 —— 多出来的两个属性被忽略,其余参数照常加载;
//   • 新构建读旧工程 —— 属性不存在,两位取默认 false。
// 无需升 abi、无需迁移函数;将来补 §1.31 的 ui.active_tab 同样零成本。

#include <juce_data_structures/juce_data_structures.h>

#include <string>

#include "state/FeaturesCodec.h" // isValidSessionGuid(不可信 state 字节的 guid 形状校验)

namespace scvb::output
{

// 属性名带 ui_ 前缀,与 APVTS 自己写在根节点上的参数子节点不同名(APVTS 的参数是**子节点**,
// 根节点属性面归本模块与将来的 ui.* 使用)。
inline const juce::Identifier kUiGuideSeenProp{"ui_guide_seen"};
inline const juce::Identifier kUiTourSeenProp{"ui_tour_seen"};
// 「用户显式选过语言」位:首启语言选择卡的唯一抑制条件。此前它只活在 web 的
// store.session 里(随 WebView 一起销毁),于是每次开窗都重新问一遍(v4 实测 P1-6)。
inline const juce::Identifier kUiLangChosenProp{"ui_lang_chosen"};

struct OutputUiFlags
{
    bool guideSeen = false;
    bool tourSeen = false;
    bool langChosen = false;
};

// 写入 APVTS 快照树的根节点(getStateInformation:copyState() 之后、序列化之前)。
inline void writeUiFlags(juce::ValueTree& apvtsState, const OutputUiFlags& flags)
{
    if (!apvtsState.isValid())
    {
        return;
    }
    apvtsState.setProperty(kUiGuideSeenProp, flags.guideSeen, nullptr);
    apvtsState.setProperty(kUiTourSeenProp, flags.tourSeen, nullptr);
    apvtsState.setProperty(kUiLangChosenProp, flags.langChosen, nullptr);
}

// 从工程里解出的 APVTS 树读回(setStateInformation:replaceState 之前/之后皆可)。
// 属性缺失 = 老工程 / 从未落过盘 ⇒ 两位为 false(= 该走首启)。
inline OutputUiFlags readUiFlags(const juce::ValueTree& apvtsState)
{
    OutputUiFlags flags;
    if (!apvtsState.isValid())
    {
        return flags;
    }
    flags.guideSeen = static_cast<bool>(apvtsState.getProperty(kUiGuideSeenProp, false));
    flags.tourSeen = static_cast<bool>(apvtsState.getProperty(kUiTourSeenProp, false));
    flags.langChosen = static_cast<bool>(apvtsState.getProperty(kUiLangChosenProp, false));
    return flags;
}

// [SL-215] 会话 GUID —— sidecar 文件名隔离的根基(<basename>-<GUID前8>.scvbfeat,04 §5.x)。
// 它此前**根本没有生产落点**:桥面快照里写死一串全零字面量(OutputEditor 的 `session_guid`),
// 设置页于是恒显示 session 00000000-0000-0000-0000-000000000000。generateSessionGuid() 与
// SidecarStore 侧的校验/CoW 全都实现好且有单测,只是没人调用。
//
// 落在 PRMS 根节点属性面上,理由与上面三个 ui_ 位逐字相同:CFGS 是定长枚举式解码,尾部追加
// 字段会让旧构建整块拒载并静默把配置打回默认;ValueTree 增删字段两个方向都容忍,无需升 abi、
// 无需迁移函数,也就不动 STATE_SCHEMA 的冻结布局。
inline const juce::Identifier kSessionGuidProp{"session_guid"};

inline void writeSessionGuid(juce::ValueTree& apvtsState, const juce::String& guid)
{
    if (!apvtsState.isValid())
    {
        return;
    }
    apvtsState.setProperty(kSessionGuidProp, guid, nullptr);
}

// 读回并**校验形状**:state 字节不可信(§7.3),而这个 guid 会被 SidecarStore 拿去拼路径。
// 非 36 字符 dashed UUID 一律当「没有」处理,由调用方重新生成 —— 绝不把畸形串带进文件名。
inline juce::String readSessionGuid(const juce::ValueTree& apvtsState)
{
    if (!apvtsState.isValid())
    {
        return {};
    }
    const juce::String guid = apvtsState.getProperty(kSessionGuidProp, juce::String()).toString();
    return scvb::state::isValidSessionGuid(guid.toStdString()) ? guid : juce::String();
}

} // namespace scvb::output
