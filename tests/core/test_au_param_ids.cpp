// SPDX-License-Identifier: GPL-3.0-or-later
// test_au_param_ids —— [B 线 M06b] AU 参数 ID 冻结表(编进 scvb_params_tests;**不**放 tests/golden/)。
//
// 宿主(Logic、LUNA ……)在工程里按 AudioUnitParameterID 记自动化。JUCE 8.0.8 的 AU wrapper
// (juce_audio_plugin_client_AU_1.mm 的 generateAUParameterID,约 :2382-2393)这样算它:
//   ① JUCE_FORCE_USE_LEGACY_PARAM_IDS=0:取 ParamID 字符串的 juce::String::hashCode()
//      (31 乘子、uint32 回绕),而不是参数下标;
//   ② JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS=1:再清掉最高位。
// VST3 wrapper(juce_audio_plugin_client_VST3.cpp 的 generateVSTParamIDForParam)走同一条式子:
// VST3ClientExtensions::convertJuceParameterId(id, JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS)。
//
// 两个宏由根 CMakeLists.txt 的 SCVB_PLUGIN_PARAM_ID_DEFINITIONS 一处给出:三个插件以 PUBLIC 传给各
// 格式 wrapper,本测试目标读同一个变量。本文件钉三件事:
//   1. 两个宏的值 —— static_assert,任何一个翻转,本目标当场编不过;
//   2. 123 个冻结 ParamID(遍历真实 makeOutputLayout() 的 getParameters(),按 index 顺序)现算的
//      AU 参数 ID:两两不同、最高位为 0、逐项等于下面内嵌的冻结表;
//   3. 同一个值等于 JUCE 公开的 convertJuceParameterId —— VST3 与 AU 在同一工程里共用同一套 ID,
//      也就说明钉宏之后 VST3 参数 ID(Windows 已发布的那一套)不变。
//
// 为什么不放 tests/golden/:那个目录属冻结契约(CLAUDE.md §5,改动要变更文档 + 用户批准)。AU 是
// 新格式,AU 三元组与参数 ID 的冻结要等修宪落进 ADR;在那之前这张表是**回归判据**:它一变,宿主
// 工程里存的 AU 自动化就会对不上。
// 表的来源:对 tests/golden/params_v0.tsv 第 2 列逐行按 ①② 离线算出;运行期再用 JUCE 自己的
// String::hashCode 复算对拍 —— 两条独立路径必须一致。升级 .juce-version 的 PR 要重看 ①② 的出处
// 是否仍是这条式子(本文件红,即说明 JUCE 改了散列或 wrapper 的算法)。

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "OutputParams.h"

// 两个宏只能来自构建系统:本目标不链接 juce_audio_plugin_client,拿不到 JUCE 头里的默认值。
// 没定义 = 本目标与三个插件已经不读同一个变量,下面的 static_assert 钉的就不再是插件的口径。
#ifndef JUCE_FORCE_USE_LEGACY_PARAM_IDS
#error "JUCE_FORCE_USE_LEGACY_PARAM_IDS undefined: scvb_params_tests must read SCVB_PLUGIN_PARAM_ID_DEFINITIONS"
#endif
#ifndef JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS
#error \
    "JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS undefined: scvb_params_tests must read SCVB_PLUGIN_PARAM_ID_DEFINITIONS"
#endif

static_assert(JUCE_FORCE_USE_LEGACY_PARAM_IDS == 0,
              "JUCE_FORCE_USE_LEGACY_PARAM_IDS must stay 0: AU/VST3 parameter IDs are hashed from the frozen "
              "ParamID strings; 1 switches to parameter indices and breaks every saved automation lane");
static_assert(JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS == 1,
              "JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS must stay 1: it clears the top bit of every AU/VST3 "
              "parameter ID; 0 changes 64 of the 123 IDs");

namespace
{

// 最小 AudioProcessor:只为承载 APVTS 并遍历 getParameters()(与 test_params_golden.cpp 同一手法)。
struct AuIdTestProcessor final : juce::AudioProcessor
{
    AuIdTestProcessor()
        : juce::AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "AuIdTest"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};

struct LayoutUnderTest
{
    AuIdTestProcessor proc;
    juce::AudioProcessorValueTreeState apvts;

    LayoutUnderTest() : apvts(proc, nullptr, "PARAMETERS", scvb::params::makeOutputLayout()) {}

    // index 顺序的 ParamID(AU wrapper 遍历的也是 getParameters() 这份顺序)。
    std::vector<juce::String> paramIds() const
    {
        std::vector<juce::String> ids;
        for (const auto* p : proc.getParameters())
        {
            const auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*>(p);
            REQUIRE(withId != nullptr);
            ids.push_back(withId->getParameterID());
        }
        return ids;
    }
};

// generateAUParameterID 在本仓钉住的两个宏取值下的等价式:LEGACY=0 ⇒ 不走下标分支,
// STUDIO_ONE=1 ⇒ 清最高位。宏一翻转,上面的 static_assert 先红,这里不必再按宏分支。
std::uint32_t auParameterIdFor(const juce::String& paramId)
{
    auto id = static_cast<std::uint32_t>(paramId.hashCode());
    id &= ~(std::uint32_t{1} << 31);
    return id;
}

struct FrozenAuParamId
{
    const char* paramId;
    std::uint32_t auId;
};

// 冻结表:行序 = tests/golden/params_v0.tsv 的 index 顺序(行尾注释是 index)。
constexpr FrozenAuParamId kFrozenAuParamIds[] = {
    {"width", 0x06BE2DC6u}, // 0
    {"ms_balance", 0x165B1D43u}, // 1
    {"lead_select", 0x5EC7D33Fu}, // 2
    {"v1_t01_pan", 0x549DE10Fu}, // 3
    {"v1_t01_vol", 0x549DF945u}, // 4
    {"v1_t01_width", 0x251009B8u}, // 5
    {"v1_t01_freeze", 0x606DDCE5u}, // 6
    {"v1_t02_pan", 0x54ABF890u}, // 7
    {"v1_t02_vol", 0x54AC10C6u}, // 8
    {"v1_t02_width", 0x59F644F9u}, // 9
    {"v1_t02_freeze", 0x484F09C4u}, // 10
    {"v1_t03_pan", 0x54BA1011u}, // 11
    {"v1_t03_vol", 0x54BA2847u}, // 12
    {"v1_t03_width", 0x0EDC803Au}, // 13
    {"v1_t03_freeze", 0x303036A3u}, // 14
    {"v1_t04_pan", 0x54C82792u}, // 15
    {"v1_t04_vol", 0x54C83FC8u}, // 16
    {"v1_t04_width", 0x43C2BB7Bu}, // 17
    {"v1_t04_freeze", 0x18116382u}, // 18
    {"v1_t05_pan", 0x54D63F13u}, // 19
    {"v1_t05_vol", 0x54D65749u}, // 20
    {"v1_t05_width", 0x78A8F6BCu}, // 21
    {"v1_t05_freeze", 0x7FF29061u}, // 22
    {"v1_t06_pan", 0x54E45694u}, // 23
    {"v1_t06_vol", 0x54E46ECAu}, // 24
    {"v1_t06_width", 0x2D8F31FDu}, // 25
    {"v1_t06_freeze", 0x67D3BD40u}, // 26
    {"v1_t07_pan", 0x54F26E15u}, // 27
    {"v1_t07_vol", 0x54F2864Bu}, // 28
    {"v1_t07_width", 0x62756D3Eu}, // 29
    {"v1_t07_freeze", 0x4FB4EA1Fu}, // 30
    {"v1_t08_pan", 0x55008596u}, // 31
    {"v1_t08_vol", 0x55009DCCu}, // 32
    {"v1_t08_width", 0x175BA87Fu}, // 33
    {"v1_t08_freeze", 0x379616FEu}, // 34
    {"v1_t09_pan", 0x550E9D17u}, // 35
    {"v1_t09_vol", 0x550EB54Du}, // 36
    {"v1_t09_width", 0x4C41E3C0u}, // 37
    {"v1_t09_freeze", 0x1F7743DDu}, // 38
    {"v1_t10_pan", 0x5644A22Du}, // 39
    {"v1_t10_vol", 0x5644BA63u}, // 40
    {"v1_t10_width", 0x580AFB56u}, // 41
    {"v1_t10_freeze", 0x0CD11F07u}, // 42
    {"v1_t11_pan", 0x5652B9AEu}, // 43
    {"v1_t11_vol", 0x5652D1E4u}, // 44
    {"v1_t11_width", 0x0CF13697u}, // 45
    {"v1_t11_freeze", 0x74B24BE6u}, // 46
    {"v1_t12_pan", 0x5660D12Fu}, // 47
    {"v1_t12_vol", 0x5660E965u}, // 48
    {"v1_t12_width", 0x41D771D8u}, // 49
    {"v1_t12_freeze", 0x5C9378C5u}, // 50
    {"v1_t13_pan", 0x566EE8B0u}, // 51
    {"v1_t13_vol", 0x566F00E6u}, // 52
    {"v1_t13_width", 0x76BDAD19u}, // 53
    {"v1_t13_freeze", 0x4474A5A4u}, // 54
    {"v1_t14_pan", 0x567D0031u}, // 55
    {"v1_t14_vol", 0x567D1867u}, // 56
    {"v1_t14_width", 0x2BA3E85Au}, // 57
    {"v1_t14_freeze", 0x2C55D283u}, // 58
    {"v1_t15_pan", 0x568B17B2u}, // 59
    {"v1_t15_vol", 0x568B2FE8u}, // 60
    {"v1_t15_width", 0x608A239Bu}, // 61
    {"v1_t15_freeze", 0x1436FF62u}, // 62
    {"v2_t01_pan", 0x68E25010u}, // 63
    {"v2_t01_vol", 0x68E26846u}, // 64
    {"v2_t01_width", 0x39F4BC79u}, // 65
    {"v2_t01_freeze", 0x681F8244u}, // 66
    {"v2_t02_pan", 0x68F06791u}, // 67
    {"v2_t02_vol", 0x68F07FC7u}, // 68
    {"v2_t02_width", 0x6EDAF7BAu}, // 69
    {"v2_t02_freeze", 0x5000AF23u}, // 70
    {"v2_t03_pan", 0x68FE7F12u}, // 71
    {"v2_t03_vol", 0x68FE9748u}, // 72
    {"v2_t03_width", 0x23C132FBu}, // 73
    {"v2_t03_freeze", 0x37E1DC02u}, // 74
    {"v2_t04_pan", 0x690C9693u}, // 75
    {"v2_t04_vol", 0x690CAEC9u}, // 76
    {"v2_t04_width", 0x58A76E3Cu}, // 77
    {"v2_t04_freeze", 0x1FC308E1u}, // 78
    {"v2_t05_pan", 0x691AAE14u}, // 79
    {"v2_t05_vol", 0x691AC64Au}, // 80
    {"v2_t05_width", 0x0D8DA97Du}, // 81
    {"v2_t05_freeze", 0x07A435C0u}, // 82
    {"v2_t06_pan", 0x6928C595u}, // 83
    {"v2_t06_vol", 0x6928DDCBu}, // 84
    {"v2_t06_width", 0x4273E4BEu}, // 85
    {"v2_t06_freeze", 0x6F85629Fu}, // 86
    {"v2_t07_pan", 0x6936DD16u}, // 87
    {"v2_t07_vol", 0x6936F54Cu}, // 88
    {"v2_t07_width", 0x775A1FFFu}, // 89
    {"v2_t07_freeze", 0x57668F7Eu}, // 90
    {"v2_t08_pan", 0x6944F497u}, // 91
    {"v2_t08_vol", 0x69450CCDu}, // 92
    {"v2_t08_width", 0x2C405B40u}, // 93
    {"v2_t08_freeze", 0x3F47BC5Du}, // 94
    {"v2_t09_pan", 0x69530C18u}, // 95
    {"v2_t09_vol", 0x6953244Eu}, // 96
    {"v2_t09_width", 0x61269681u}, // 97
    {"v2_t09_freeze", 0x2728E93Cu}, // 98
    {"v2_t10_pan", 0x6A89112Eu}, // 99
    {"v2_t10_vol", 0x6A892964u}, // 100
    {"v2_t10_width", 0x6CEFAE17u}, // 101
    {"v2_t10_freeze", 0x1482C466u}, // 102
    {"v2_t11_pan", 0x6A9728AFu}, // 103
    {"v2_t11_vol", 0x6A9740E5u}, // 104
    {"v2_t11_width", 0x21D5E958u}, // 105
    {"v2_t11_freeze", 0x7C63F145u}, // 106
    {"v2_t12_pan", 0x6AA54030u}, // 107
    {"v2_t12_vol", 0x6AA55866u}, // 108
    {"v2_t12_width", 0x56BC2499u}, // 109
    {"v2_t12_freeze", 0x64451E24u}, // 110
    {"v2_t13_pan", 0x6AB357B1u}, // 111
    {"v2_t13_vol", 0x6AB36FE7u}, // 112
    {"v2_t13_width", 0x0BA25FDAu}, // 113
    {"v2_t13_freeze", 0x4C264B03u}, // 114
    {"v2_t14_pan", 0x6AC16F32u}, // 115
    {"v2_t14_vol", 0x6AC18768u}, // 116
    {"v2_t14_width", 0x40889B1Bu}, // 117
    {"v2_t14_freeze", 0x340777E2u}, // 118
    {"v2_t15_pan", 0x6ACF86B3u}, // 119
    {"v2_t15_vol", 0x6ACF9EE9u}, // 120
    {"v2_t15_width", 0x756ED65Cu}, // 121
    {"v2_t15_freeze", 0x1BE8A4C1u}, // 122
};

static_assert(sizeof(kFrozenAuParamIds) / sizeof(kFrozenAuParamIds[0]) ==
                  static_cast<std::size_t>(scvb::params::kNumAutomatable),
              "the frozen AU ID table must list every automatable parameter exactly once");

} // namespace

TEST_CASE("AU parameter IDs of the 123 frozen ParamIDs equal the embedded table", "[params][au]")
{
    LayoutUnderTest t;
    const auto ids = t.paramIds();
    REQUIRE(ids.size() == static_cast<std::size_t>(scvb::params::kNumAutomatable));

    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        const auto& row = kFrozenAuParamIds[i];
        INFO("index " << i << " paramId " << ids[i].toStdString());
        CHECK(ids[i].toStdString() == row.paramId);
        CHECK(auParameterIdFor(ids[i]) == row.auId);
    }
}

TEST_CASE("AU parameter IDs are pairwise distinct with the top bit clear", "[params][au]")
{
    LayoutUnderTest t;
    const auto ids = t.paramIds();
    REQUIRE(ids.size() == static_cast<std::size_t>(scvb::params::kNumAutomatable));

    std::set<std::uint32_t> seen;
    for (const auto& id : ids)
    {
        const auto au = auParameterIdFor(id);
        INFO("paramId " << id.toStdString() << " auId " << au);
        // 最高位为 1 的 ID 会被 Studio One 之类的宿主当成负数丢掉(JUCE 加这条宏的原因)。
        CHECK((au & 0x80000000u) == 0u);
        seen.insert(au);
    }
    // 撞了的话 JUCE 的 AU wrapper 只在 Debug 下 jassert,Release 里后一个参数静默顶掉前一个。
    CHECK(seen.size() == ids.size());

    // 冻结表本身也必须满足同样两条:表被手改坏时,不依赖上一格的逐项对拍也能单独照出来。
    std::set<std::uint32_t> tableIds;
    for (const auto& row : kFrozenAuParamIds)
    {
        INFO("table paramId " << row.paramId);
        CHECK((row.auId & 0x80000000u) == 0u);
        tableIds.insert(row.auId);
    }
    CHECK(tableIds.size() == static_cast<std::size_t>(scvb::params::kNumAutomatable));
}

TEST_CASE("AU parameter IDs equal the VST3 parameter IDs JUCE assigns", "[params][au]")
{
    LayoutUnderTest t;
    const auto ids = t.paramIds();
    REQUIRE(ids.size() == static_cast<std::size_t>(scvb::params::kNumAutomatable));

    constexpr bool studioOneCompatible = JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS != 0;
    for (const auto& id : ids)
    {
        INFO("paramId " << id.toStdString());
        CHECK(auParameterIdFor(id) == juce::VST3ClientExtensions::convertJuceParameterId(id, studioOneCompatible));
    }
}
