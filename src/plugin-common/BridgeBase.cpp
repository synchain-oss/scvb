// SPDX-License-Identifier: GPL-3.0-or-later
#include "BridgeBase.h"

#include <algorithm>

namespace scvb::bridge
{

float clampUiScale(float scale)
{
    return juce::jlimit(plugin::MinUiScale, plugin::MaxUiScale, scale);
}

std::vector<std::pair<juce::String, juce::var>> buildUiSeedPairs(const UiSeed& seed)
{
    return {
        {juce::String(Init::Version), juce::var(seed.version)},
        {juce::String(Init::Role), juce::var(seed.role)},
        {juce::String(Init::ChannelLimit), juce::var(seed.channelLimit)},
        {juce::String(Init::Lang), juce::var(seed.lang)},
        {juce::String(Init::UiScale), juce::var(seed.uiScale)},
    };
}

juce::var buildUiSnapshot(const UiSeed& seed)
{
    auto* obj = new juce::DynamicObject();
    for (const auto& pair : buildUiSeedPairs(seed))
        obj->setProperty(pair.first, pair.second);
    return juce::var(obj);
}

juce::String normalizeLang(const juce::String& code)
{
    if (code == "en" || code == "fr")
        return code;
    return "zh"; // 未知 code 回退 zh(§1.30)
}

bool parseUiScaleArg(const juce::Array<juce::var>& args, const juce::String& role, float& outScale)
{
    if (args.size() < 1)
        return false;
    const auto& v = args[0];
    if (!(v.isDouble() || v.isInt()))
        return false;

    const double f = static_cast<double>(v);
    // §1.28 拒绝态:f 不在档位表 → badArg。档位唯一真源 = DesignBox.h,double 比较避免精度误判。
    // kInputPresets(10 档)与 kOutputPresets(7 档)是不同类型(std::array 长度不同),须分分支。
    const bool inTable = (role == "input")
                             ? std::find(scvb::design::kInputPresets.begin(), scvb::design::kInputPresets.end(), f) !=
                                   scvb::design::kInputPresets.end()
                             : std::find(scvb::design::kOutputPresets.begin(), scvb::design::kOutputPresets.end(), f) !=
                                   scvb::design::kOutputPresets.end();
    if (!inTable)
        return false;

    outScale = static_cast<float>(f);
    return true;
}

juce::var okResponse()
{
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", true);
    return juce::var(o);
}

juce::var okResponseWith(const juce::String& key, const juce::var& value)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", true);
    o->setProperty(key, value);
    return juce::var(o);
}

juce::var badArgResponse()
{
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", false);
    o->setProperty("reason", "badArg");
    return juce::var(o);
}

} // namespace scvb::bridge
