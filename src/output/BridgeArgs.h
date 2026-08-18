// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// BridgeArgs —— 桥面 native function 参数提取助手(消息线程)。纯 JUCE var 工具,可离线单测。
// 与 SegmentEditService.h 同放 src/output/(不落 scvb_core,因依赖 JUCE)。

#include <juce_core/juce_core.h>

namespace scvb::output
{

// 严格布尔提取(PR#55 缺陷3):仅接受 bool / int / int64 数值;isBool → 真值,isInt/isInt64 → !=0。
// 其它(字符串/对象/void)返回 false,调用方回 badArg。JUCE 8.0.8 的 var::operator bool() 实为
// type->toBool(value)(juce_Variant.cpp:567;bool→boolToBool L246、int→intToBool L149、void→defaultToBool
// L100),对 bool 返回真值、对 void 返回 false;此处仍显式分型,避免字符串 "false" 被 stringToBool
// 误判为非空即 true。
inline bool strictBool(const juce::var& v, bool& out)
{
    if (v.isBool())
    {
        out = static_cast<bool>(v);
        return true;
    }
    if (v.isInt() || v.isInt64())
    {
        out = static_cast<juce::int64>(v) != 0;
        return true;
    }
    return false;
}

} // namespace scvb::output
