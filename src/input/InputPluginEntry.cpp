// SPDX-License-Identifier: GPL-3.0-or-later
// InputPluginEntry —— Input 插件的两个「宿主入口」定义:createEditor() 与 createPluginFilter()。
// 抽出单独 TU 的理由与 OutputPluginEntry.cpp 完全一致(见其头注):让免 DAW 的宿主 harness
// 能只编 InputProcessor 而不链接 WebView2,且同进程同时托管两个插件时 createPluginFilter 不撞名。

#include "InputEditor.h"
#include "InputProcessor.h"

juce::AudioProcessorEditor* ScvbInputAudioProcessor::createEditor()
{
    return new scvb::input::InputEditor(*this);
}

// juce_add_plugin 的 VST3 wrapper 从这里实例化插件。
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ScvbInputAudioProcessor();
}
