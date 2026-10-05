// SPDX-License-Identifier: GPL-3.0-or-later
// OutputPluginEntry —— Output 插件的两个「宿主入口」定义:createEditor() 与 createPluginFilter()。
//
// 单独成 TU 的理由是**可测性**,不是代码整洁:
//   · createEditor() 是 OutputProcessor.cpp 里唯一一处引用 OutputEditor 的地方,而 OutputEditor
//     经 WebViewHost 拖进 juce_gui_extra + WebView2 loader —— 免 DAW 的宿主 harness 只想跑
//     processBlock / timerCallback / 段表事务,不该被迫链接一整套 WebView。
//   · createPluginFilter() 在 Input/Output 两侧同名,一个进程里同时托管两个插件(C 族广播需要
//     Output 写、Input 读)会撞符号。
// 把这两个定义抽到本 TU 后,tests/host 只编 *Processor.cpp 而不编本文件,两个问题一起消失;
// 插件产物侧行为完全不变(本 TU 在 SCVBOutput 的 target_sources 里)。

#include "OutputEditor.h"
#include "OutputProcessor.h"
#include "PlatformLog.h"

juce::AudioProcessorEditor* ScvbOutputAudioProcessor::createEditor()
{
    return new scvb::output::OutputEditor(*this);
}

// juce_add_plugin 的 VST3/AU wrapper 从这里实例化插件。
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    // [B 线 M07] 平台文件日志 + IPC 诊断 sink(mac:~/Library/Logs/Synchain/SCVB/output.log;Windows 空操作)。
    // 落点理由同 InputPluginEntry.cpp:早于本二进制里任何一个 Processor 的构造;每个二进制只装一次。
    scvb::platformlog::install("output", JucePlugin_VersionString);
    return new ScvbOutputAudioProcessor();
}
