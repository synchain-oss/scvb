// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

#include "export/SuggestionExport.h"

// exportSuggestions(scope) 的入参归一(契约 §1.36)。
//
// 单拎成纯函数的理由与同目录 `AnalyzeScopeMath.h` 一脉相承:它埋在 `OutputEditor` 的私有
// 成员里就够不着 —— 那个类要 WebView 才构造得起来,离线 harness 编不进那个 TU,于是「入参
// 归一」这一层只能靠源码正则去看,改回去照样绿。抽成纯函数,`scvb_params_tests` 可以直接调。
//
// §1.36 的默认口径(**整体可省 = 全默认**):
//   · `versions`:`"active"`(默认)| `"all"`;其余字符串 = badArg;
//   · `tracksMask`:默认 `0x7FFF`(全 15 轨);**bit15 保留 0**(§9.2),给了也掩掉;
//     掩完为 0 = 一轨都没选 ⇒ badArg(与 §1.23 `recaptureArm` 的 [J87] 守卫同口径 ——
//     「掩完为 0」不能退化成「不限轨」,那是把空选区当全选);
//   · 时间窗:`startS`/`endS` 两者都给且 `end > start` 才生效;只给一头 = 不限
//     (`SuggestionExport::Scope` 以 `< 0` 表示不限,故这里回 -1)。
//
// 「给没给」由调用方判(桥面 `juce::var` 的 `hasProperty` + 是不是数),本函数只收已解出的
// 值 + 一个 has 标志 —— 与 `analyzeScopeRange` 同款签名风格,免得把 JUCE 类型拖进纯层。

namespace scvb::output
{

struct SuggestionScopeParse
{
    scvb::suggest::Scope scope{};
    bool badArg = false;
};

inline SuggestionScopeParse parseSuggestionScope(bool hasVersions, const std::string& versions, bool hasMask,
                                                 int maskRaw, bool hasStart, double startS, bool hasEnd, double endS,
                                                 int activeVersion)
{
    SuggestionScopeParse r;
    r.scope.activeVersion = activeVersion;

    if (hasVersions)
    {
        if (versions == "all")
        {
            r.scope.allVersions = true;
        }
        else if (versions == "active")
        {
            r.scope.allVersions = false;
        }
        else
        {
            r.badArg = true; // 未知枚举值不「宽容回落」:静默导出一份不是用户要的范围更糟
            return r;
        }
    }

    if (hasMask)
    {
        // bit15 是 §9.2 的保留位,恒 0;先掩再判,免得「只点了保留位」被当成有效选择。
        const std::uint16_t masked = static_cast<std::uint16_t>(static_cast<unsigned>(maskRaw) & 0x7FFFu);
        if (masked == 0)
        {
            r.badArg = true;
            return r;
        }
        r.scope.tracksMask = masked;
    }

    // 时间窗:两头都给且成立才启用;否则整个不限(-1/-1),**不做单边窗**。
    // 单边窗在这里没有合理默认:导出是「把选中的段抄成表」,一头开放会让用户拿到一份
    // 他没框过的范围,而这条路没有 UI 回显能让他发现。
    if (hasStart && hasEnd && endS > startS)
    {
        r.scope.startSec = startS;
        r.scope.endSec = endS;
    }
    else if ((hasStart || hasEnd) && !(hasStart && hasEnd))
    {
        r.scope.startSec = -1.0;
        r.scope.endSec = -1.0;
    }
    else if (hasStart && hasEnd)
    {
        // 两头都给但 end <= start:这是调用方算错了范围,不是「不限」。
        r.badArg = true;
        return r;
    }

    return r;
}

} // namespace scvb::output
