// SPDX-License-Identifier: GPL-3.0-or-later
#include "BridgeBase.h"

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

bool parseUiScaleArg(const juce::Array<juce::var>& args, float& outScale)
{
    if (args.size() < 1)
        return false;
    const auto& v = args[0];
    if (!(v.isDouble() || v.isInt()))
        return false;
    outScale = clampUiScale(static_cast<float>(static_cast<double>(v)));
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
