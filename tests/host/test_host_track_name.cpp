// SPDX-License-Identifier: GPL-3.0-or-later
// test_host_track_name —— [J150] 04 §7 步 2「轨道名自动填入 label」的宿主级用例。
//
// 真 ScvbInputAudioProcessor + 真 ScvbOutputAudioProcessor,经**真实** Win32 共享内存段(ctrl 段
// 轨道名区)通信,手工跑消息循环让两侧 25Hz Timer 真实触发。钉的是生产那几跳:
//   宿主 updateTrackProperties → Input timer 写 ctrl 段轨道名区 → Output timer 读(三道门)→
//   runtime_.channels[].label → 广播区(Input 侧可见)/ 存档(PRMS channels_auto_label)。
// 纯函数层(CtrlPlane 写读、归属判据、UTF-8 自保)在 tests/core/test_track_name_ipc.cpp。
//
// 每个用例都带一条**对照轨**:ch4 上的第二个 Input(`ctl`),它的 label 从不被用户改过。判「某条 label
// **没有**跟着改名」时,先等对照轨跟上新名字 —— 证明这段时间里 Output 确实读过轨道名区 —— 再断言
// 被测轨没动;不拿固定睡眠当「已经来得及改了」的证据。
//
// 组号:与 test_host_harness.cpp 的主 Rig 同用 7(同一进程里各用例串行、段随实例析构)。
// 同机独占守卫由 test_host_harness.cpp 引入的 support/exclusive_guard.h 负责(整个可执行文件一把)。

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "InputProcessor.h"
#include "OutputProcessor.h"
#include "state/OutputStateCodec.h"
#include "state/StateCodec.h"
#include "state/StateMigration.h"

namespace
{

constexpr int kGroup = 7;
constexpr int kCh = 3; // 被测轨
constexpr int kCtlCh = 4; // 对照轨
constexpr std::size_t kIdx = static_cast<std::size_t>(kCh - 1);
constexpr std::size_t kCtlIdx = static_cast<std::size_t>(kCtlCh - 1);
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

juce::AudioProcessor::TrackProperties named(const juce::String& name)
{
    juce::AudioProcessor::TrackProperties p;
    p.name = name;
    return p;
}

struct TrackRig
{
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager(Timer 要它)
    std::unique_ptr<ScvbOutputAudioProcessor> out = std::make_unique<ScvbOutputAudioProcessor>();
    ScvbInputAudioProcessor in;
    ScvbInputAudioProcessor ctl;

    TrackRig()
    {
        out->setGroupId(kGroup);
        REQUIRE(out->groupId() == kGroup); // [SL-324] 读回断言:组号越界会静默回落 g1
        in.setGroupId(kGroup);
        ctl.setGroupId(kGroup);
        in.setChannelId(kCh);
        ctl.setChannelId(kCtlCh);
        out->prepareToPlay(kSr, kBlock);
        in.prepareToPlay(kSr, kBlock);
        ctl.prepareToPlay(kSr, kBlock);
        REQUIRE(pumpUntil([this] { return connected(kCh) && connected(kCtlCh); }));
    }

    ~TrackRig()
    {
        if (out != nullptr)
        {
            out->releaseResources();
        }
        in.releaseResources();
        ctl.releaseResources();
    }

    static void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

    template<typename Pred>
    static bool pumpUntil(Pred pred, int maxMs = 3000)
    {
        for (int waited = 0; waited < maxMs; waited += 40)
        {
            if (pred())
            {
                return true;
            }
            pump(40);
        }
        return pred();
    }

    bool connected(int ch)
    {
        const auto c = out->connSnapshot().channels[static_cast<std::size_t>(ch - 1)];
        return c.slotState == scvb::kSlotActive && c.heartbeatAgeMs <= scvb::kStaleDisplayMs;
    }

    juce::String label(std::size_t idx) { return out->channelsSnapshot()[idx].label; }

    // 对照:给 ctl 一个新名字,等 Output 把它填进 ch4 —— 之后再多跑几拍,保证被测轨那边若要改也早改了。
    bool settleWithControl(const juce::String& controlName)
    {
        ctl.updateTrackProperties(named(controlName));
        const bool ok = pumpUntil([&] { return label(kCtlIdx) == controlName; });
        pump(200);
        return ok;
    }
};

// 存档里 PRMS / CFGS 两节的就地改写(模拟「本功能之前的工程」与「被旧构建另存过的工程」)。
std::vector<std::uint8_t> rewriteState(const juce::MemoryBlock& blob,
                                       const std::function<void(juce::ValueTree&)>& editPrms,
                                       const std::function<void(scvb::state::OutputState&)>& editCfgs)
{
    scvb::state::StateChunks chunks;
    REQUIRE(scvb::state::loadState(static_cast<const std::uint8_t*>(blob.getData()), blob.getSize(), chunks).status ==
            scvb::state::StateLoadStatus::Ok);
    if (editPrms)
    {
        const scvb::state::Chunk* prms = chunks.find(scvb::state::kFourccPrms);
        REQUIRE(prms != nullptr);
        std::unique_ptr<juce::XmlElement> xml(
            juce::AudioProcessor::getXmlFromBinary(prms->payload.data(), static_cast<int>(prms->payload.size())));
        REQUIRE(xml != nullptr);
        juce::ValueTree tree = juce::ValueTree::fromXml(*xml);
        editPrms(tree);
        std::unique_ptr<juce::XmlElement> back(tree.createXml());
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary(*back, mb);
        const auto* p = static_cast<const std::uint8_t*>(mb.getData());
        chunks.set(scvb::state::kFourccPrms, std::vector<std::uint8_t>(p, p + mb.getSize()));
    }
    if (editCfgs)
    {
        const scvb::state::Chunk* cfgs = chunks.find(scvb::state::kFourccCfgs);
        REQUIRE(cfgs != nullptr);
        scvb::state::OutputState s;
        REQUIRE(scvb::state::decodeOutputState(cfgs->payload.data(), cfgs->payload.size(), s));
        editCfgs(s);
        std::vector<std::uint8_t> payload;
        REQUIRE(scvb::state::encodeOutputState(s, payload));
        chunks.set(scvb::state::kFourccCfgs, std::move(payload));
    }
    std::vector<std::uint8_t> out;
    REQUIRE(scvb::state::encodeContainer(chunks, out));
    return out;
}

} // namespace

TEST_CASE("HOST J150:宿主轨道名自动填进 label,改名跟着改,Input 侧广播看得到", "[host][j150]")
{
    TrackRig r;
    REQUIRE(r.label(kIdx).isEmpty()); // 前提:新实例的 label 是空的(占位「Track 03」由页面画)

    r.in.updateTrackProperties(named("Lead Vox"));
    CHECK(TrackRig::pumpUntil([&] { return r.label(kIdx) == "Lead Vox"; }));
    // 回推给 Input 的广播区(Input 页 / 通道网格读的就是这里)。
    CHECK(TrackRig::pumpUntil(
        [&] { return juce::String::fromUTF8(r.in.bridgeTickSnapshot().broadcast.labels[kIdx]) == "Lead Vox"; }));

    // DAW 里改名:用户没动过这条 label ⇒ 跟着改。
    r.in.updateTrackProperties(named("Lead Vox 2"));
    CHECK(TrackRig::pumpUntil([&] { return r.label(kIdx) == "Lead Vox 2"; }));

    // 宿主这次没带 name(只改了颜色之类):保留上一次的名字,不把 label 清掉。
    r.in.updateTrackProperties(juce::AudioProcessor::TrackProperties{});
    REQUIRE(r.settleWithControl("Ctl A"));
    CHECK(r.label(kIdx) == "Lead Vox 2");
    // ……而且 Input 手里的名字也还在:用户清空 label(回到自动)后,填回来的仍是「Lead Vox 2」。
    // Output 从不因「空名字」清 label,所以只看上一行分不出 Input 有没有把名字丢成空串;这一格才分得出。
    ScvbOutputAudioProcessor::ChannelConfigPatch clear;
    clear.label = juce::String();
    r.out->bridgeApplyChannelConfig(static_cast<int>(kIdx), clear);
    CHECK(TrackRig::pumpUntil([&] { return r.label(kIdx) == "Lead Vox 2"; }));

    // 没有 Input 的轨不受影响。
    CHECK(r.label(0).isEmpty());
}

TEST_CASE("HOST J150:超过 24 码点的轨道名按码点截断(与桥面 setChannelConfig 同上限)", "[host][j150]")
{
    TrackRig r;
    r.in.updateTrackProperties(named("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123")); // 30 个 ASCII
    CHECK(TrackRig::pumpUntil([&] { return r.label(kIdx) == "ABCDEFGHIJKLMNOPQRSTUVWX"; }));

    // 多字节:30 个「声」(U+58F0)⇒ 24 个,不切半个码点。
    juce::String thirty;
    juce::String twentyFour;
    for (int i = 0; i < 30; ++i)
    {
        thirty += juce::String::charToString(static_cast<juce::juce_wchar>(0x58F0));
        if (i < 24)
        {
            twentyFour += juce::String::charToString(static_cast<juce::juce_wchar>(0x58F0));
        }
    }
    r.in.updateTrackProperties(named(thirty));
    CHECK(TrackRig::pumpUntil([&] { return r.label(kIdx) == twentyFour; }));
    CHECK(r.label(kIdx).length() == 24);
}

TEST_CASE("HOST J150:用户改过名就不再跟随轨道名;清空后回到自动", "[host][j150]")
{
    TrackRig r;
    r.in.updateTrackProperties(named("Vox A"));
    REQUIRE(TrackRig::pumpUntil([&] { return r.label(kIdx) == "Vox A"; }));

    // 用户在轨道页上改名 —— 走桥面 setChannelConfig 阶段 2 的同一个持锁口。
    ScvbOutputAudioProcessor::ChannelConfigPatch p;
    p.label = juce::String("My Lead");
    REQUIRE(r.out->bridgeApplyChannelConfig(static_cast<int>(kIdx), p));

    // DAW 改名:用户的名字不被覆盖(对照轨证明这段时间 Output 确实在读轨道名)。
    r.in.updateTrackProperties(named("Vox B"));
    REQUIRE(r.settleWithControl("Ctl B"));
    CHECK(r.label(kIdx) == "My Lead");

    // 用户清空 ⇒ 回到自动,当前轨道名填回来。
    ScvbOutputAudioProcessor::ChannelConfigPatch clear;
    clear.label = juce::String();
    r.out->bridgeApplyChannelConfig(static_cast<int>(kIdx), clear);
    CHECK(TrackRig::pumpUntil([&] { return r.label(kIdx) == "Vox B"; }));

    // 用户把名字改回来恰好等于当前轨道名:与自动填的一样,之后仍跟随(不是「用户命名」)。
    ScvbOutputAudioProcessor::ChannelConfigPatch same;
    same.label = juce::String("Vox B");
    r.out->bridgeApplyChannelConfig(static_cast<int>(kIdx), same);
    r.in.updateTrackProperties(named("Vox C"));
    CHECK(TrackRig::pumpUntil([&] { return r.label(kIdx) == "Vox C"; }));
}

TEST_CASE("HOST J150:Input 断开后保留最后的名字;断开期间清空的 label 不会被旧名字填回", "[host][j150]")
{
    TrackRig r;
    r.in.updateTrackProperties(named("Keep Me"));
    REQUIRE(TrackRig::pumpUntil([&] { return r.label(kIdx) == "Keep Me"; }));

    r.in.setChannelId(0); // Input 释放 slot(= 用户把它改成未分配,或删掉那条轨)
    REQUIRE(TrackRig::pumpUntil([&] { return !r.connected(kCh); }));
    REQUIRE(r.settleWithControl("Ctl C"));
    CHECK(r.label(kIdx) == "Keep Me"); // 断开 ≠ 清名字

    // 断开期间用户清空:轨道名区里那一条还留着旧名字,但该轨已不在线 ⇒ 不采信,保持空。
    ScvbOutputAudioProcessor::ChannelConfigPatch clear;
    clear.label = juce::String();
    r.out->bridgeApplyChannelConfig(static_cast<int>(kIdx), clear);
    REQUIRE(r.settleWithControl("Ctl D"));
    CHECK(r.label(kIdx).isEmpty());
}

TEST_CASE("HOST J150:只读观察的第二个 Output 不改自己的 label", "[host][j150]")
{
    TrackRig r;
    auto second = std::make_unique<ScvbOutputAudioProcessor>();
    second->setGroupId(kGroup);
    REQUIRE(second->groupId() == kGroup);
    second->prepareToPlay(kSr, kBlock);
    REQUIRE(TrackRig::pumpUntil([&] { return second->connSnapshot().readOnly; })); // 前提:确实是观察者

    r.in.updateTrackProperties(named("Observer Test"));
    REQUIRE(TrackRig::pumpUntil([&] { return r.label(kIdx) == "Observer Test"; })); // 主实例照填
    REQUIRE(r.settleWithControl("Ctl E"));
    CHECK(second->channelsSnapshot()[kIdx].label.isEmpty()); // 观察者不动配置
    second->releaseResources();
}

TEST_CASE("HOST J150:自动填的 label 随工程保存,重开后仍跟随改名;用户起的名字重开后仍不跟随", "[host][j150][state]")
{
    juce::MemoryBlock autoBlob; // ch3 = 自动填的「Auto Name」
    juce::MemoryBlock userBlob; // ch3 = 用户起的「User Name」
    {
        TrackRig r;
        r.in.updateTrackProperties(named("Auto Name"));
        REQUIRE(TrackRig::pumpUntil([&] { return r.label(kIdx) == "Auto Name"; }));
        r.out->getStateInformation(autoBlob);

        ScvbOutputAudioProcessor::ChannelConfigPatch p;
        p.label = juce::String("User Name");
        REQUIRE(r.out->bridgeApplyChannelConfig(static_cast<int>(kIdx), p));
        r.out->getStateInformation(userBlob);
    }

    TrackRig r2; // 新实例 = 关掉工程再打开;两个 Input 此时都还没收到轨道名
    const auto load = [&r2](const void* data, std::size_t size) {
        r2.out->setStateInformation(data, static_cast<int>(size));
        TrackRig::pump(100);
    };

    SECTION("自动填的 ⇒ 重开后仍跟随")
    {
        load(autoBlob.getData(), autoBlob.getSize());
        REQUIRE(r2.label(kIdx) == "Auto Name");
        r2.in.updateTrackProperties(named("Renamed"));
        CHECK(TrackRig::pumpUntil([&] { return r2.label(kIdx) == "Renamed"; }));
    }
    SECTION("用户起的 ⇒ 重开后仍不跟随")
    {
        load(userBlob.getData(), userBlob.getSize());
        REQUIRE(r2.label(kIdx) == "User Name");
        r2.in.updateTrackProperties(named("Renamed"));
        REQUIRE(r2.settleWithControl("Ctl F"));
        CHECK(r2.label(kIdx) == "User Name");
    }
    SECTION("本功能之前的工程(PRMS 没有 channels_auto_label)⇒ 非空 label 一律当用户命名")
    {
        const auto old = rewriteState(
            autoBlob, [](juce::ValueTree& t) { t.removeProperty("channels_auto_label", nullptr); }, nullptr);
        load(old.data(), old.size());
        REQUIRE(r2.label(kIdx) == "Auto Name");
        r2.in.updateTrackProperties(named("Renamed"));
        REQUIRE(r2.settleWithControl("Ctl G"));
        CHECK(r2.label(kIdx) == "Auto Name");
    }
    SECTION("被旧构建另存过、期间用户改了名字(PRMS 原样带回、CFGS 的 label 变了)⇒ 仍当用户命名")
    {
        // 旧构建经 APVTS replaceState/copyState 把它不认识的 PRMS 属性原样带回,只改了 CFGS 里的 label。
        const auto roundTripped = rewriteState(
            autoBlob, nullptr, [](scvb::state::OutputState& s) { s.channels[kIdx].label = "Typed In Old"; });
        load(roundTripped.data(), roundTripped.size());
        REQUIRE(r2.label(kIdx) == "Typed In Old");
        r2.in.updateTrackProperties(named("Renamed"));
        REQUIRE(r2.settleWithControl("Ctl H"));
        CHECK(r2.label(kIdx) == "Typed In Old");
    }
}

TEST_CASE("HOST J150:自动填入不进撤销栈;撤销用户改名 ⇒ 回到跟随轨道名", "[host][j150][undo]")
{
    // [SL-536 / J140] 起 setChannelConfig 的 label 进插件撤销栈。「是否跟随」由 label 与上次自动填入值推导,
    // 撤销只还原 label、不碰 autoLabel —— 两边不需要任何同步,这一格钉的就是这件事。
    TrackRig r;
    r.in.updateTrackProperties(named("Vox A"));
    REQUIRE(TrackRig::pumpUntil([&] { return r.label(kIdx) == "Vox A"; }));
    CHECK_FALSE(r.out->undo()); // 自动填入不是用户操作:栈里什么都没有

    ScvbOutputAudioProcessor::ChannelConfigPatch p;
    p.label = juce::String("Mine");
    REQUIRE(r.out->bridgeApplyChannelConfig(static_cast<int>(kIdx), p));
    r.in.updateTrackProperties(named("Vox B"));
    REQUIRE(r.settleWithControl("Ctl U"));
    REQUIRE(r.label(kIdx) == "Mine"); // 前提:用户命名期间不跟随

    // 撤掉「改成 Mine」⇒ label 回到「Vox A」== 上次自动填入值 ⇒ 重新跟随,随即换成当前轨道名。
    CHECK(r.out->undo());
    CHECK(TrackRig::pumpUntil([&] { return r.label(kIdx) == "Vox B"; }));
}
