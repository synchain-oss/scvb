// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// HostId —— 桥面 §1.1 快照 `host` 字段的取值([J150] ①;03 §4.2 REAPER / §4.4 Ableton Live
// 的宿主专属界面提示)。纯 JUCE 工具,可离线单测。
//
// 判宿主只走 `juce::PluginHostType`(JUCE 按宿主可执行文件名识别,Windows 侧见
// `juce_PluginHostType.cpp`:文件名含 "reaper" ⇒ Reaper;含 "Live " ⇒ AbletonLive*,不带
// 6..11 版本号的落 `AbletonLiveGeneric`;含 "Cubase" ⇒ SteinbergCubase*)。
//
// 值域是**闭集** `"reaper" | "live" | "cubase" | "other"`,页面只对前两个出提示:
//   · `cubase` 单列、不并进 `other`:Cubase 是主测宿主,页面级冒烟要拿它当「有名有姓、
//     但不该出提示」的反例 —— 并进 `other` 的话,「Cubase 上不出」只是「other 上不出」的
//     同义反复,删掉宿主判定那一格照样绿;
//   · Nuendo / Studio One / FL Studio 等**一律** `other`(本卡不给它们出提示)。
// 新增取值只许放宽(SCVB_CONTRACT §0.1 第 3 条),页面对不认识的值按 `other` 处理。

#include <juce_audio_processors/juce_audio_processors.h>

namespace scvb::output
{

inline const char* hostIdOf(juce::PluginHostType::HostType type)
{
    juce::PluginHostType host;
    host.type = type;
    if (host.isReaper())
    {
        return "reaper";
    }
    if (host.isAbletonLive())
    {
        return "live";
    }
    if (host.isCubase())
    {
        return "cubase";
    }
    return "other";
}

} // namespace scvb::output
