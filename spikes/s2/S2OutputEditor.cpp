// SPDX-License-Identifier: GPL-3.0-or-later
#include "S2OutputEditor.h"

S2OutputEditor::S2OutputEditor(S2OutputProcessor& p) : juce::AudioProcessorEditor(p), processor_(p)
{
    headerLabel_.setText("SCVB S2 Automation Spike", juce::dontSendNotification);
    headerLabel_.setFont(juce::Font(16.0f, juce::Font::bold));
    addAndMakeVisible(headerLabel_);

    infoLabel_.setText("123 params (v2.1) · fixed 30s step envelope · print window [0,30s]",
                       juce::dontSendNotification);
    infoLabel_.setFont(juce::Font(12.0f));
    addAndMakeVisible(infoLabel_);

    outputButton_.setClickingTogglesState(true);
    outputButton_.setButtonText("Output: OFF");
    outputButton_.onClick = [this] {
        processor_.setOutputEnabled(outputButton_.getToggleState());
        updateStatus();
    };
    addAndMakeVisible(outputButton_);

    statusLabel_.setFont(juce::Font(13.0f));
    addAndMakeVisible(statusLabel_);

    setSize(640, 150);
    updateStatus();
    startTimerHz(20);
}

S2OutputEditor::~S2OutputEditor()
{
    stopTimer();
}

void S2OutputEditor::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void S2OutputEditor::resized()
{
    auto area = getLocalBounds().reduced(12);
    headerLabel_.setBounds(area.removeFromTop(24));
    infoLabel_.setBounds(area.removeFromTop(20));
    area.removeFromTop(8);

    auto row = area.removeFromTop(32);
    outputButton_.setBounds(row.removeFromLeft(180));
    area.removeFromTop(8);

    statusLabel_.setBounds(area);
}

void S2OutputEditor::timerCallback()
{
    updateStatus();
}

void S2OutputEditor::updateStatus()
{
    const bool on = processor_.isOutputEnabled();
    outputButton_.setToggleState(on, juce::dontSendNotification);
    outputButton_.setButtonText(on ? "Output: ON" : "Output: OFF");

    // 回读显示:v1_t01 的 pan/vol 当前值(FOLLOW 态 = 宿主回放驱动,可目测回读一致性)。
    juce::String pan = "-";
    juce::String vol = "-";
    if (auto* p = processor_.apvts.getParameter("v1_t01_pan"))
        pan = p->getCurrentValueAsText();
    if (auto* p = processor_.apvts.getParameter("v1_t01_vol"))
        vol = p->getCurrentValueAsText();

    const juce::String state = processor_.isPrinting()
                                   ? juce::String::formatted("PRINTING @ %.2f s", processor_.lastPlayheadSeconds())
                                   : juce::String(on ? "ARMED (play within 0-30s)" : "FOLLOW");

    statusLabel_.setText(state + " · v1_t01_pan=" + pan + " · v1_t01_vol=" + vol, juce::dontSendNotification);
}
