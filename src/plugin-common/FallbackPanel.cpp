// SPDX-License-Identifier: GPL-3.0-or-later
#include "FallbackPanel.h"

#include "PlatformWebView.h"

#include <utility>

namespace scvb::webview
{
namespace
{
// [SL-370] 面板底 = shellBackdrop(),它在 SL-370 之后是**浅色** ⇒ 三行标签必须是深墨,
// 否则这块「WebView2 起不来时唯一能告诉用户发生了什么」的面板会白字浅底、一个字看不见。
// 取值取自 SL-370 当时 web/shared/tokens.css 的 --txt-1 / --txt-2(浅色面上的一级/正文色阶);
// **这不是跨文件真源**,tokens 改了这里不会红。这里唯一的硬约束是「与 shellBackdrop() 的
// 对比度 ≥ 4.5」,判据 = tests/webview/test_plugin_common.cpp 的
// "FallbackPanel label colours stay readable on shellBackdrop()"。
// 正文与诊断行同色是**对比度下限逼出来的**,不是漏改:在现在这个底色亮度上,再淡一档
// (如 --txt-3 #6a6a74)只有 3.4:1,过不了那条断言;层级改由字号与等宽字面承担。
constexpr juce::uint32 kTitleTextArgb = 0xff21212a;
constexpr juce::uint32 kBodyTextArgb = 0xff52525c;
} // namespace

FallbackPanel::FallbackPanel(Options options) : options_(std::move(options))
{
    title_.setComponentID("fallback.title");
    title_.setText(options_.title, juce::dontSendNotification);
    title_.setJustificationType(juce::Justification::centred);
    title_.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
    title_.setColour(juce::Label::textColourId, juce::Colour(kTitleTextArgb));
    addAndMakeVisible(title_);

    message_.setComponentID("fallback.message");
    message_.setText(options_.message, juce::dontSendNotification);
    message_.setJustificationType(juce::Justification::centredTop);
    message_.setColour(juce::Label::textColourId, juce::Colour(kBodyTextArgb));
    addAndMakeVisible(message_);

    details_.setComponentID("fallback.details");
    details_.setText(options_.details, juce::dontSendNotification);
    details_.setJustificationType(juce::Justification::centredTop);
    // 11px:诊断行里含完整的 user-data 路径,Input 的 460px 窄盒要折 3-4 行才放得下。
    details_.setFont(
        juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain)));
    details_.setColour(juce::Label::textColourId, juce::Colour(kBodyTextArgb));
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
    // [SL-253] 收编到 `PlatformWebView.h` 的单一真源:此前这里是自己的字面量
    // 0xff18161d,与外层那层差一点点 —— WebView2 挂掉那一瞬会看见一次色阶跳变。
    // [SL-370] 那个真源换成浅色之后本面板跟着变浅,标签色见文件顶部 kTitleTextArgb /
    // kBodyTextArgb;继续共用同一个真源(而不是给本面板留一个自己的深底)是为了保住
    // SL-253 收编的那条性质:兜底面板与外层永远同色,切过来不跳阶。
    g.fillAll(scvb::webview::shellBackdrop());
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
