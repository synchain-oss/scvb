// SPDX-License-Identifier: GPL-3.0-or-later
// test_bridge_args —— T29 桥面参数提取助手单测(PR#55 缺陷3:严格布尔提取)。
// 佐证 JUCE 8.0.8 var::operator bool() 语义 = type->toBool(value)(juce_Variant.cpp:567),对 bool 返回真值、
// 对 void 返回 false —— 与「非 void 即 true」的旧读法不同;strictBool 仍显式分型,拒绝字符串/void。

#include <catch2/catch_test_macros.hpp>

#include <juce_core/juce_core.h>

#include "BridgeArgs.h"

TEST_CASE("BRIDGEARGS-BOOL-1 严格布尔提取(false 语义正确)", "[bridgeargs][bool]")
{
    bool out = true;

    REQUIRE(scvb::output::strictBool(juce::var(false), out));
    REQUIRE_FALSE(out); // JS false → 关闭/未读/解锁

    REQUIRE(scvb::output::strictBool(juce::var(true), out));
    REQUIRE(out);

    REQUIRE(scvb::output::strictBool(juce::var(0), out));
    REQUIRE_FALSE(out); // int 0 → false

    REQUIRE(scvb::output::strictBool(juce::var(1), out));
    REQUIRE(out);
}

TEST_CASE("BRIDGEARGS-BOOL-2 非布尔/void 拒绝(调用方回 badArg)", "[bridgeargs][bool]")
{
    bool out = false;
    REQUIRE_FALSE(scvb::output::strictBool(juce::var("false"), out)); // 字符串拒绝(防 stringToBool 误判)
    REQUIRE_FALSE(scvb::output::strictBool(juce::var(), out)); // void 拒绝
}

TEST_CASE("BRIDGEARGS-BOOL-3 JUCE var::operator bool 语义佐证", "[bridgeargs][bool]")
{
    // 佐证:JUCE 8.0.8 var::operator bool() 对 bool 返回真值、对 void 返回 false。
    REQUIRE_FALSE(static_cast<bool>(juce::var(false)));
    REQUIRE(static_cast<bool>(juce::var(true)));
    REQUIRE_FALSE(static_cast<bool>(juce::var()));
}
