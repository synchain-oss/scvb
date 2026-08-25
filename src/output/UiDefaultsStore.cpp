// SPDX-License-Identifier: GPL-3.0-or-later
#include "UiDefaultsStore.h"

#include <juce_data_structures/juce_data_structures.h>

#include <memory>

#include "BridgeBase.h" // Min/MaxUiScale(缩放档位边界的单一真源)

namespace scvb::output::uidefaults
{

namespace
{

constexpr const char* kKeyGuideSeen = "guide_seen_global";
constexpr const char* kKeyTourSeen = "tour_seen_global";
// 缩放档位**按角色分键**:两插件的档位表不同(Output {0.5…2} / Input {0.33…3}),
// 共用一个键会在 Input 也实现 §3.6 落盘后互相污染(Input 存 300 → Output 构造读回 300,
// 而 300 不在 Output 档位里)。inRange 用的是并集边界,拦不住这种污染。
// 两个 *_seen_global 是 Output 专属(Input 没有引导页/导览),无需分键。
constexpr const char* kKeyUiScale = "ui_scale_percent_output";

// 档位边界真源 = scvb::bridge::Min/MaxUiScale(§1.28/§1.29:C++ 不得二次硬编码)。
bool inRange(int percent)
{
    return percent >= juce::roundToInt(scvb::bridge::plugin::MinUiScale * 100.0f) &&
           percent <= juce::roundToInt(scvb::bridge::plugin::MaxUiScale * 100.0f);
}

// 测试注入的落盘目录(空 = 用默认位置)。只由 setStorageDirForTesting 写,消息线程/单测线程。
juce::File& testDirRef()
{
    static juce::File dir;
    return dir;
}

// Windows:%APPDATA%\Synchain\SCVB\ui-defaults.settings(app data 根照 STATE_SCHEMA §4.3)。
// 文件缺失/不可写一律降级为「全默认」—— 全局默认丢失只会让引导页多弹一次,
// 绝不能让开窗失败(§5.1 降级纪律)。
std::unique_ptr<juce::PropertiesFile> openFile()
{
    const juce::File& testDir = testDirRef();
    if (testDir != juce::File())
    {
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        options.millisecondsBeforeSaving = 0;
        return std::make_unique<juce::PropertiesFile>(testDir.getChildFile("ui-defaults.settings"), options);
    }

    juce::PropertiesFile::Options options;
    options.applicationName = "ui-defaults";
    options.folderName = "Synchain/SCVB"; // = %APPDATA%\Synchain\SCVB(STATE_SCHEMA §4.3 同根)
    options.filenameSuffix = "settings";
    options.osxLibrarySubFolder = "Application Support";
    options.commonToAllUsers = false;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    // 0 = setValue 即刻落盘。默认值 3000 会起一个 juce::Timer 做延迟保存 —— 那既依赖
    // MessageManager 存活,也会在宿主强杀进程时丢掉刚写的「不再显示」。
    options.millisecondsBeforeSaving = 0;
    return std::make_unique<juce::PropertiesFile>(options);
}

bool readBool(const char* key)
{
    const auto f = openFile();
    return f != nullptr && f->getBoolValue(key, false);
}

void writeBool(const char* key, bool value)
{
    const auto f = openFile();
    if (f == nullptr)
        return;
    f->setValue(key, value);
    f->saveIfNeeded(); // millisecondsBeforeSaving=0 下已写过;显式一次,免得改选项时静默丢写
}

} // namespace

bool guideSeenGlobal()
{
    return readBool(kKeyGuideSeen);
}

void setGuideSeenGlobal(bool seen)
{
    writeBool(kKeyGuideSeen, seen);
}

bool tourSeenGlobal()
{
    return readBool(kKeyTourSeen);
}

void setTourSeenGlobal(bool seen)
{
    writeBool(kKeyTourSeen, seen);
}

int uiScalePercent()
{
    const auto f = openFile();
    if (f == nullptr)
        return 0;
    const int percent = f->getIntValue(kKeyUiScale, 0);
    return inRange(percent) ? percent : 0; // 范围外视为未设置(不可信字节)
}

void setUiScalePercent(int percent)
{
    if (!inRange(percent))
        return;
    const auto f = openFile();
    if (f == nullptr)
        return;
    f->setValue(kKeyUiScale, percent);
    f->saveIfNeeded();
}

void setStorageDirForTesting(const juce::File& dir)
{
    testDirRef() = dir;
}

} // namespace scvb::output::uidefaults
