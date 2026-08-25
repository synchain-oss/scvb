// SPDX-License-Identifier: GPL-3.0-or-later
#include "UiDefaultsStore.h"

#include <juce_data_structures/juce_data_structures.h>

#include <memory>

namespace scvb::output::uidefaults
{

namespace
{

constexpr const char* kKeyGuideSeen = "guide_seen_global";
constexpr const char* kKeyTourSeen = "tour_seen_global";
constexpr const char* kKeyUiScale = "ui_scale_percent";

// Windows:%APPDATA%\SCVB\ui-defaults.settings。文件缺失/不可写一律降级为「全默认」——
// 全局默认丢失只会让引导页多弹一次,绝不能让开窗失败(§5.1 降级纪律)。
std::unique_ptr<juce::PropertiesFile> openFile()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "ui-defaults";
    options.folderName = "SCVB";
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
    return (percent >= 33 && percent <= 300) ? percent : 0; // 范围外视为未设置(不可信字节)
}

void setUiScalePercent(int percent)
{
    if (percent < 33 || percent > 300)
        return;
    const auto f = openFile();
    if (f == nullptr)
        return;
    f->setValue(kKeyUiScale, percent);
    f->saveIfNeeded();
}

} // namespace scvb::output::uidefaults
