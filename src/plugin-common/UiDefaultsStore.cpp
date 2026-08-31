// SPDX-License-Identifier: GPL-3.0-or-later
#include "UiDefaultsStore.h"

#include <juce_data_structures/juce_data_structures.h>

#include <memory>

#include "BridgeBase.h" // Min/MaxUiScale(缩放档位边界的单一真源)

namespace scvb::uidefaults
{

namespace
{

constexpr const char* kKeyGuideSeen = "guide_seen_global";
constexpr const char* kKeyTourSeen = "tour_seen_global";
constexpr const char* kKeyLangChosen = "lang_chosen"; // 用户显式选过语言(跨工程)
constexpr const char* kKeyLang = "lang_global"; // 选中的语言值本身(跨工程)
// 缩放档位**按角色分键**:两插件的档位表不同(Output {0.5…2} / Input {0.33…3}),
// 共用一个键会在 Input 也实现 §3.6 落盘后互相污染(Input 存 300 → Output 构造读回 300,
// 而 300 不在 Output 档位里)。inRange 用的是并集边界,拦不住这种污染。
constexpr const char* kKeyUiScale = "ui_scale_percent_output";

// [SL-258] guide_seen 全局位**同样按角色分键**。原注释写「两个 *_seen_global 是 Output 专属
// (Input 没有引导页/导览),无需分键」—— 那是 [J80]/T48 之前的旧事实,Input 现在有首启轻量
// 引导(契约 §3.8)。契约 §3.1 语义行逐字要求「**Input 与 Output 的全局位各存一份**」:
// 共用一个位会让先装 Output 的用户永远看不到 Input 的引导,**而那正是 J80 立 T48 的全部理由**。
// Output 沿用原键名不动(改名 = 老用户已勾的「不再显示」全部失效),Input 另起后缀键。
// tour 仍是 Output 专属(Input 没有交互式导览),故 kKeyTourSeen 不分键。
constexpr const char* kKeyGuideSeenInput = "guide_seen_global_input";

// UI 语言白名单。真源 = §1.30 的归一集(scvb::bridge::normalizeLang 只吐 {zh,en,fr}),
// 本处**只做落盘侧的复核**,不新增第二套语言表 —— 加语言时两处必须一起改,故写成一个函数,
// 读写共用,不给「只改了写侧」留缝。
bool isSupportedLang(const juce::String& lang)
{
    return lang == "zh" || lang == "en" || lang == "fr";
}

// 档位边界真源 = scvb::bridge::Min/MaxUiScale(§1.28/§1.29:C++ 不得二次硬编码)。
// [SL-234] 换算不再就地展开:夹取即恒等 ⇔ 本来就在区间内,与桥面/加载期共用同一个 clamp。
bool inRange(int percent)
{
    return scvb::bridge::clampUiScalePercent(percent) == percent;
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

bool guideSeenGlobalInput()
{
    return readBool(kKeyGuideSeenInput);
}

void setGuideSeenGlobalInput(bool seen)
{
    writeBool(kKeyGuideSeenInput, seen);
}

bool tourSeenGlobal()
{
    return readBool(kKeyTourSeen);
}

void setTourSeenGlobal(bool seen)
{
    writeBool(kKeyTourSeen, seen);
}

bool langChosenGlobal()
{
    return readBool(kKeyLangChosen);
}

void setLangChosenGlobal(bool chosen)
{
    writeBool(kKeyLangChosen, chosen);
}

juce::String langGlobal()
{
    const auto f = openFile();
    if (f == nullptr)
        return {};
    const juce::String v = f->getValue(kKeyLang, juce::String());
    // 白名单**逐字同 §1.30 的归一集** {zh,en,fr}(scvb::bridge::normalizeLang)。磁盘上是
    // 用户可编辑的 XML,来路不明的值不许进 UI 语言位;但白名单漏一种语言比不做白名单更坏 ——
    // 漏掉 fr 时法文用户的 setLangGlobal 会静默 return,而「选过语言」那个布尔照写不误:
    // 移除插件重加载后语言回落 en,语言起始卡又被 lang_chosen_global 挡住 —— P1-6 在 fr 上原样复现。
    return isSupportedLang(v) ? v : juce::String();
}

void setLangGlobal(const juce::String& lang)
{
    if (!isSupportedLang(lang))
        return;
    const auto f = openFile();
    if (f == nullptr)
        return;
    f->setValue(kKeyLang, lang);
    f->saveIfNeeded();
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

} // namespace scvb::uidefaults
