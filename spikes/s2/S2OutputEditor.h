// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// S2 spike —— 最小编辑器:Output(打印)开关 + 状态/回读显示。无参数控件(123 参数太庞大,
// 手工自动化由用户在 DAW 车道上画;插件窗口只负责触发打印与展示打印器状态)。

#include <juce_audio_processors/juce_audio_processors.h>

#include "S2OutputProcessor.h"

class S2OutputEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit S2OutputEditor(S2OutputProcessor& p);
    ~S2OutputEditor() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;
    void updateStatus();

    S2OutputProcessor& processor_;

    juce::TextButton outputButton_;
    juce::Label headerLabel_;
    juce::Label infoLabel_;
    juce::Label statusLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(S2OutputEditor)
};
