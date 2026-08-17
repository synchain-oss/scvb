// SPDX-License-Identifier: GPL-3.0-or-later
#include "FallbackPanel.h"

#include <utility>

namespace scvb::webview
{

FallbackPanel::FallbackPanel(Options options) : options_(std::move(options))
{
    title_.setComponentID("fallback.title");
    title_.setText(options_.title, juce::dontSendNotification);
    title_.setJustificationType(juce::Justification::centred);
    title_.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
    title_.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(title_);

    message_.setComponentID("fallback.message");
    message_.setText(options_.message, juce::dontSendNotification);
    message_.setJustificationType(juce::Justification::centredTop);
    message_.setColour(juce::Label::textColourId, juce::Colour(0xffb8b8c4));
    addAndMakeVisible(message_);

    if (options_.showInstall)
    {
        install_.setComponentID("fallback.install");
        install_.setButtonText("Download WebView2 Runtime");
        install_.onClick = [this] {
            if (options_.onInstall)
                options_.onInstall();
        };
        addAndMakeVisible(install_);
    }

    retry_.setComponentID("fallback.retry");
    retry_.setButtonText("Retry");
    retry_.onClick = [this] {
        if (options_.onRetry)
            options_.onRetry();
    };
    addAndMakeVisible(retry_);
}

FallbackPanel::~FallbackPanel() = default;

void FallbackPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff18161d));
}

void FallbackPanel::resized()
{
    auto b = getLocalBounds().reduced(24);
    title_.setBounds(b.removeFromTop(34));
    message_.setBounds(b.removeFromTop(76));
    b.removeFromTop(10);
    if (options_.showInstall)
    {
        install_.setBounds(b.removeFromTop(38).reduced(24, 0));
        b.removeFromTop(8);
    }
    retry_.setBounds(b.removeFromTop(34).reduced(96, 0));
}

} // namespace scvb::webview
