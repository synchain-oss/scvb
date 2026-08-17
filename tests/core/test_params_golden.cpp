// SPDX-License-Identifier: GPL-3.0-or-later
// test_params_golden —— T15:123 参数布局 golden 冻结 + index 公式 + hash 碰撞 + 离散步进。
// 03 §8 P-1/P-2/P-3:实例化 layout、遍历 getParameters() 与 tests/golden/params_v0.tsv 逐行 diff。

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "OutputParams.h"

namespace
{

// 最小 AudioProcessor:仅用于承载 APVTS 并遍历 getParameters()(03 §8 P-1)。
struct LayoutTestProcessor final : juce::AudioProcessor
{
    LayoutTestProcessor()
        : juce::AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "LayoutTest"; }
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

// 一次构造:一个 layout + 一个 APVTS,测试内遍历。
struct ParamsUnderTest
{
    LayoutTestProcessor proc;
    juce::AudioProcessorValueTreeState apvts;

    ParamsUnderTest() : apvts(proc, nullptr, "PARAMETERS", scvb::params::makeOutputLayout()) {}

    const juce::Array<juce::AudioProcessorParameter*>& params() const { return proc.getParameters(); }
};

std::string goldenPath()
{
    return std::string(SCVB_TEST_GOLDEN_DIR) + "/params_v0.tsv";
}

std::vector<std::string> readGoldenDataLines()
{
    std::ifstream f(goldenPath());
    REQUIRE(f.good());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        lines.push_back(line);
    }
    return lines;
}

std::string fmtFloat(float v)
{
    char buf[32];
    if (v == std::floor(v) && std::fabs(v) < 1e6f)
        std::snprintf(buf, sizeof(buf), "%.0f", v);
    else
        std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

const juce::RangedAudioParameter* asRanged(const juce::AudioProcessorParameter* p)
{
    return dynamic_cast<const juce::RangedAudioParameter*>(p);
}

bool isIntParam(const juce::AudioProcessorParameter* p)
{
    return dynamic_cast<const juce::AudioParameterInt*>(p) != nullptr;
}

bool endsWithFreeze(const std::string& id)
{
    const std::string suffix = "_freeze";
    return id.size() >= suffix.size() && id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string paramRow(int index, const juce::RangedAudioParameter* p)
{
    const auto range = p->getNormalisableRange();
    const bool isInt = isIntParam(p);
    // getDefaultValue() 返回 0..1 归一化值,去归一化回原始默认值。
    const float rawDefault = p->convertFrom0to1(p->getDefaultValue());

    std::ostringstream os;
    os << index << '	' << p->getParameterID().toStdString() << '	' << p->getName(1024).toStdString() << '	';

    if (isInt)
        os << static_cast<int>(range.start) << '	' << static_cast<int>(range.end) << '	';
    else
        os << fmtFloat(range.start) << '	' << fmtFloat(range.end) << '	';

    os << fmtFloat(range.skew) << '	';

    if (isInt)
        os << static_cast<int>(rawDefault);
    else
        os << fmtFloat(rawDefault);

    os << '\t' << p->getVersionHint() << '\t' << (isInt ? "int" : "float");

    return os.str();
}

// 复刻 JUCE 8.0.8 VST3 wrapper 的 ParamID hash:String::hashCode()(31 乘子,uint32 回绕)
// 截为非负 32 位(& 0x7fffffff,JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS 默认 =1)。
// 算法随 JUCE 版本复核:JUCE 升级 PR 必须重跑本 hash 断言(03 §8 P-3)。
std::uint32_t vstParamIdHash(const std::string& s)
{
    std::uint32_t h = 0;
    for (const unsigned char c : s)
        h = h * 31u + c;
    return h & 0x7fffffffu;
}

} // namespace

TEST_CASE("params golden 逐行 diff(123 行)", "[params][golden]")
{
    ParamsUnderTest t;
    const auto& params = t.params();
    REQUIRE(params.size() == static_cast<int>(scvb::params::kNumAutomatable));

    const auto golden = readGoldenDataLines();
    REQUIRE(golden.size() == static_cast<std::size_t>(scvb::params::kNumAutomatable));

    for (int i = 0; i < scvb::params::kNumAutomatable; ++i)
    {
        const auto* ranged = asRanged(params[i]);
        REQUIRE(ranged != nullptr);
        INFO("index " << i);
        CHECK(paramRow(i, ranged) == golden[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("golden 表头含双口径注记 123/124 与 Live 128 余 4", "[params][golden]")
{
    std::ifstream f(goldenPath());
    REQUIRE(f.good());
    std::ostringstream oss;
    oss << f.rdbuf();
    const std::string text = oss.str();

    CHECK(text.find("123") != std::string::npos);
    CHECK(text.find("124") != std::string::npos);
    CHECK(text.find("128") != std::string::npos);
    CHECK(text.find("余 4") != std::string::npos);
}

TEST_CASE("index 公式与实际 layout 逐项一致", "[params][index]")
{
    ParamsUnderTest t;
    const auto& params = t.params();
    REQUIRE(params.size() == static_cast<int>(scvb::params::kNumAutomatable));

    const auto idAt = [&params](int i) { return asRanged(params[i])->getParameterID().toStdString(); };

    // index 0..2:全局三件。
    CHECK(idAt(0) == "width");
    CHECK(idAt(1) == "ms_balance");
    CHECK(idAt(2) == "lead_select");

    // 全部 (v,t,k) 与索引公式逐项一致。
    for (int v = 1; v <= scvb::params::kNumVersions; ++v)
    {
        for (int tr = 1; tr <= scvb::params::kNumTracks; ++tr)
        {
            const int base = scvb::params::kTrackParamIndex(v, tr, 0);
            INFO("v" << v << " t" << tr);
            CHECK(idAt(base + 0) == scvb::params::panId(v, tr).toStdString());
            CHECK(idAt(base + 1) == scvb::params::volId(v, tr).toStdString());
            CHECK(idAt(base + 2) == scvb::params::widthId(v, tr).toStdString());
            CHECK(idAt(base + 3) == scvb::params::freezeId(v, tr).toStdString());
        }
    }

    // 末位 = 122。
    CHECK(scvb::params::kTrackParamIndex(2, 15, 3) == 122);
    CHECK(idAt(122) == "v2_t15_freeze");
}

TEST_CASE("123 ParamID + Bypass 的 hash 无碰撞(size==124)", "[params][hash]")
{
    ParamsUnderTest t;
    const auto& params = t.params();
    REQUIRE(params.size() == 123);

    std::set<std::uint32_t> hashes;
    for (const auto* p : params)
    {
        const auto* ranged = asRanged(p);
        REQUIRE(ranged != nullptr);
        hashes.insert(vstParamIdHash(ranged->getParameterID().toStdString()));
    }
    REQUIRE(hashes.size() == 123);

    // wrapper 合成 bypass(显示名 "Bypass")也须无碰撞(03 §8 P-3)。
    hashes.insert(vstParamIdHash("Bypass"));
    REQUIRE(hashes.size() == 124);

    // 补充:实际 wrapper bypass 的 VST3 ParamID 是固定 fourcc 'byps' = 0x62797073(非 hash),
    // 也须与 123 个 hash 无碰撞。
    CHECK(hashes.count(0x62797073u) == 0);
}

TEST_CASE("lead_select 离散步进(16 档)与 string 互逆", "[params][discrete]")
{
    ParamsUnderTest t;
    const auto* lead = t.apvts.getParameter("lead_select"); // RangedAudioParameter*
    REQUIRE(lead != nullptr);
    CHECK(dynamic_cast<const juce::AudioParameterInt*>(lead) != nullptr);
    CHECK(lead->getNormalisableRange().interval == 1.0f); // step 1
    CHECK(lead->getNumSteps() == 16); // 0..15 共 16 档(getNumSteps 经基类公开接口访问)

    for (int v = 0; v <= 15; ++v)
    {
        INFO("lead_select " << v);
        CHECK(scvb::params::leadSelectFromString(scvb::params::leadSelectToString(v, 1024)) == v);
    }
}

TEST_CASE("freeze 离散步进(4 档)与 string 互逆", "[params][discrete]")
{
    ParamsUnderTest t;
    const auto* frz = t.apvts.getParameter("v1_t01_freeze"); // RangedAudioParameter*
    REQUIRE(frz != nullptr);
    CHECK(dynamic_cast<const juce::AudioParameterInt*>(frz) != nullptr);
    CHECK(frz->getNormalisableRange().interval == 1.0f); // step 1
    CHECK(frz->getNumSteps() == 4); // 0..3 共 4 档,[J65]

    for (int v = 0; v <= 3; ++v)
    {
        INFO("freeze " << v);
        CHECK(scvb::params::freezeFromString(scvb::params::freezeToString(v, 1024)) == v);
    }
}

TEST_CASE("全部参数 versionHint==1 且类型 int/float 正确", "[params][version]")
{
    ParamsUnderTest t;
    for (const auto* p : t.params())
    {
        const auto* ranged = asRanged(p);
        REQUIRE(ranged != nullptr);
        const std::string id = ranged->getParameterID().toStdString();
        INFO("param " << id);

        CHECK(ranged->getVersionHint() == 1);

        const bool isInt = isIntParam(p);
        const bool expectInt = (id == "lead_select") || endsWithFreeze(id);
        if (expectInt)
            CHECK(isInt);
        else
            CHECK(!isInt);
    }
}

TEST_CASE("pan/vol/width/ms_balance 的 string 互逆往返", "[params][strings]")
{
    const float msVals[] = {-100.0f, -50.0f, -1.0f, 0.0f, 1.0f, 50.0f, 100.0f};
    for (const float v : msVals)
    {
        INFO("ms_balance " << v);
        CHECK(juce::approximatelyEqual(scvb::params::msBalanceFromString(scvb::params::msBalanceToString(v, 1024)), v));
    }

    const float panVals[] = {-100.0f, -37.5f, -30.0f, -1.0f, 0.0f, 1.0f, 30.0f, 37.5f, 100.0f};
    for (const float v : panVals)
    {
        INFO("pan " << v);
        CHECK(juce::approximatelyEqual(scvb::params::panFromString(scvb::params::panToString(v, 1024)), v));
    }

    const float volVals[] = {-24.0f, -12.5f, -1.0f, 0.0f, 1.0f, 3.2f, 12.0f};
    for (const float v : volVals)
    {
        INFO("vol " << v);
        CHECK(juce::approximatelyEqual(scvb::params::volFromString(scvb::params::volToString(v, 1024)), v));
    }

    const float widthVals[] = {0.0f, 50.0f, 100.0f, 150.0f};
    for (const float v : widthVals)
    {
        INFO("width " << v);
        CHECK(juce::approximatelyEqual(scvb::params::widthFromString(scvb::params::widthToString(v, 1024)), v));
    }

    const float twVals[] = {0.0f, 50.0f, 100.0f};
    for (const float v : twVals)
    {
        INFO("trackWidth " << v);
        CHECK(
            juce::approximatelyEqual(scvb::params::trackWidthFromString(scvb::params::trackWidthToString(v, 1024)), v));
    }

    // 符号钳制:M/L 前缀强制非正、S/R 前缀强制非负(修复 M-100 → +100 的符号反转)。
    CHECK(scvb::params::msBalanceFromString("M-100") <= 0.0f);
    CHECK(scvb::params::msBalanceFromString("S-50") >= 0.0f);
    CHECK(scvb::params::panFromString("L-100") <= 0.0f);
    CHECK(scvb::params::panFromString("R-50") >= 0.0f);
}

TEST_CASE("version_active 不在参数列表里", "[params]")
{
    ParamsUnderTest t;
    for (const auto* p : t.params())
    {
        const auto* ranged = asRanged(p);
        REQUIRE(ranged != nullptr);
        CHECK(ranged->getParameterID().toStdString() != "version_active");
    }
}

TEST_CASE("ParameterGroup 树:v{v} → v{v}_t{tt} → 4 参数", "[params][groups]")
{
    ParamsUnderTest t;
    const auto& root = t.proc.getParameterTree();

    // 全局三件在根组,两个版本组为根的直接子组。
    CHECK(root.getParameters(false).size() == 3);

    const auto versions = root.getSubgroups(false);
    REQUIRE(versions.size() == 2);
    CHECK(versions[0]->getID() == "v1");
    CHECK(versions[1]->getID() == "v2");

    for (int v = 0; v < scvb::params::kNumVersions; ++v)
    {
        const auto tracks = versions[static_cast<std::size_t>(v)]->getSubgroups(false);
        REQUIRE(tracks.size() == static_cast<int>(scvb::params::kNumTracks));

        for (int tr = 0; tr < scvb::params::kNumTracks; ++tr)
        {
            INFO("v" << (v + 1) << " t" << (tr + 1));
            CHECK(tracks[static_cast<std::size_t>(tr)]->getID() == juce::String::formatted("v%d_t%02d", v + 1, tr + 1));
            CHECK(tracks[static_cast<std::size_t>(tr)]->getParameters(false).size() == 4);
        }
    }
}

TEST_CASE("ParamHandles 与布局一一对应", "[params][handles]")
{
    ParamsUnderTest t;
    const auto h = scvb::params::collectParamHandles(t.apvts);

    REQUIRE(h.width != nullptr);
    REQUIRE(h.msBalance != nullptr);
    REQUIRE(h.leadSelect != nullptr);

    for (int v = 0; v < scvb::params::kNumVersions; ++v)
    {
        for (int tr = 0; tr < scvb::params::kNumTracks; ++tr)
        {
            INFO("v" << (v + 1) << " t" << (tr + 1));
            REQUIRE(h.pan[v][tr] != nullptr);
            REQUIRE(h.vol[v][tr] != nullptr);
            REQUIRE(h.trkW[v][tr] != nullptr);
            REQUIRE(h.frz[v][tr] != nullptr);
            CHECK(h.pan[v][tr]->getParameterID() == scvb::params::panId(v + 1, tr + 1));
            CHECK(h.vol[v][tr]->getParameterID() == scvb::params::volId(v + 1, tr + 1));
            CHECK(h.trkW[v][tr]->getParameterID() == scvb::params::widthId(v + 1, tr + 1));
            CHECK(h.frz[v][tr]->getParameterID() == scvb::params::freezeId(v + 1, tr + 1));
        }
    }
}
