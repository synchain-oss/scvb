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

    details_.setComponentID("fallback.details");
    details_.setText(options_.details, juce::dontSendNotification);
    details_.setJustificationType(juce::Justification::centredTop);
    // 11px:诊断行里含完整的 user-data 路径,Input 的 460px 窄盒要折 3-4 行才放得下。
    details_.setFont(
        juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain)));
    details_.setColour(juce::Label::textColourId, juce::Colour(0xff7a7a86));
    // 1.0 = 不许横向压扁,长诊断串按行宽自动折行(Label::paint 走 drawFittedText 多行)。
    details_.setMinimumHorizontalScale(1.0f);
    addChildComponent(details_);
    details_.setVisible(options_.details.isNotEmpty());

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
    if (details_.isVisible())
    {
        details_.setBounds(b.removeFromTop(84)); // ~7 行 11px 等宽,够放折行后的完整 udf 路径
        b.removeFromTop(4);
    }
    b.removeFromTop(10);
    if (options_.showInstall)
    {
        install_.setBounds(b.removeFromTop(38).reduced(24, 0));
        b.removeFromTop(8);
    }
    retry_.setBounds(b.removeFromTop(34).reduced(96, 0));
}

} // namespace scvb::webview
