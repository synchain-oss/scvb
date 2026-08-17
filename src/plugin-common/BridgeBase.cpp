// SPDX-License-Identifier: GPL-3.0-or-later
#include "BridgeBase.h"

namespace scvb::bridge
{

float clampUiScale(float scale)
{
    return juce::jlimit(plugin::MinUiScale, plugin::MaxUiScale, scale);
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

} // namespace scvb::bridge
