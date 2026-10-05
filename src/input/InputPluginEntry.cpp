// SPDX-License-Identifier: GPL-3.0-or-later
// InputPluginEntry —— Input 插件的两个「宿主入口」定义:createEditor() 与 createPluginFilter()。
// 抽出单独 TU 的理由与 OutputPluginEntry.cpp 完全一致(见其头注):让免 DAW 的宿主 harness
// 能只编 InputProcessor 而不链接 WebView2,且同进程同时托管两个插件时 createPluginFilter 不撞名。

#include "InputEditor.h"
#include "InputProcessor.h"
#include "PlatformLog.h"

juce::AudioProcessorEditor* ScvbInputAudioProcessor::createEditor()
{
    return new scvb::input::InputEditor(*this);
}

// juce_add_plugin 的 VST3 wrapper 从这里实例化插件。
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    // [B 线 M07] 平台文件日志 + IPC 诊断 sink(mac:~/Library/Logs/Synchain/SCVB/input.log;Windows 空操作)。
    // 放在这里是因为它早于本二进制里**任何一个** Processor 的构造 —— 段后端从构造 / prepareToPlay 起就可能
    // 经 IpcDiag 报失败原因;每个二进制只装一次,后续实例再调是空操作。
    scvb::platformlog::install("input", JucePlugin_VersionString);
    return new ScvbInputAudioProcessor();
}
