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

namespace scvb::output
{

// 属性名带 ui_ 前缀,与 APVTS 自己写在根节点上的参数子节点不同名(APVTS 的参数是**子节点**,
// 根节点属性面归本模块与将来的 ui.* 使用)。
inline const juce::Identifier kUiGuideSeenProp{"ui_guide_seen"};
inline const juce::Identifier kUiTourSeenProp{"ui_tour_seen"};

struct OutputUiFlags
{
    bool guideSeen = false;
    bool tourSeen = false;
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
    return flags;
}

} // namespace scvb::output
