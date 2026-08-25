// SPDX-License-Identifier: GPL-3.0-or-later
// test_host_harness —— 免 DAW 的宿主 harness(T37 三轮验收升级)。
//
// 在一个进程里同时托管 ScvbOutputAudioProcessor 与 ScvbInputAudioProcessor,扮演宿主:
//   · FakePlayHead 提供 transport(播放/停止、时间线跳变、循环区开关、tempo);
//   · 手工推 processBlock(音频线程职责),手工跑消息循环让 25Hz Timer 真实触发([M] 职责);
//   · 两个插件经**真实的** Win32 共享内存段通信(registry / audio 环 / feat 段 / ctrl 段),
//     不是 in-process 模拟 —— 这一层正是真机上出问题的地方。
//
// 为什么必须有这个 harness:T37 三轮的四族 bug 全部是「数据面从未接线」,而每一条都能在这里
// 被机器发现 —— 液柱不动 = meterSnapshot 恒地板;循环区读不到 = playhead 无 loop 字段;
// 优先级不同步 = Input 侧广播区读不到;采集不落账 = coverage 恒 0;写回互冲 = 段表另一维被清。
// 用例名与用户 L 系列清单一一对应,见 SCVB-TestKits/v4-selftest-record.md。
//
// 组号纪律:一律用测试专用组(kTestGroup),避开开发机上可能正在跑的 DAW 活实例(g1/g2),
// 与 scvb_stress 的 v10 约定同口径。段随进程退出销毁。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <vector>

#include "InputBridgeLogic.h"
#include "InputProcessor.h"
#include "OutputProcessor.h"

// createEditor 是虚函数,vtable 需要定义。真实定义在 *PluginEntry.cpp —— 那两个 TU 会把
// WebViewHost/WebView2 拖进来,harness 不编它们(见 OutputPluginEntry.cpp 头注)。
// 这里给一个无头实现补齐链接:本进程从不开编辑器,只跑 processBlock 与 [M] Timer。
juce::AudioProcessorEditor* ScvbOutputAudioProcessor::createEditor()
{
    return nullptr;
}

juce::AudioProcessorEditor* ScvbInputAudioProcessor::createEditor()
{
    return nullptr;
}

namespace
{

// 测试专用组:避开 DAW 活实例常用的 g1/g2。
constexpr int kTestGroup = 7;
constexpr int kTestChannel = 3;
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

// 宿主 playhead 替身。JUCE 只要求实现 getPosition();其余由 PositionInfo 承载。
class FakePlayHead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setTimeInSamples(timeSamples);
        p.setTimeInSeconds(static_cast<double>(timeSamples) / kSr);
        p.setIsPlaying(playing);
        p.setIsLooping(looping);
        if (haveBpm)
        {
            p.setBpm(bpm);
        }
        p.setPpqPosition(static_cast<double>(timeSamples) / kSr * (bpm / 60.0));
        if (haveLoop)
        {
            LoopPoints lp;
            lp.ppqStart = loopStartPpq;
            lp.ppqEnd = loopEndPpq;
            p.setLoopPoints(lp);
        }
        return p;
    }

    std::int64_t timeSamples = 0;
    bool playing = false;
    bool looping = false;
    bool haveLoop = false;
    bool haveBpm = true;
    double bpm = 120.0;
    double loopStartPpq = 0.0;
    double loopEndPpq = 0.0;
};

// 一台「机器」:两个插件 + 一个 playhead + 缓冲。
struct Rig
{
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager(Timer 要它)
    ScvbOutputAudioProcessor out;
    ScvbInputAudioProcessor in;
    FakePlayHead ph;
    juce::AudioBuffer<float> outBuf{2, kBlock};
    juce::AudioBuffer<float> inBuf{2, kBlock};
    juce::MidiBuffer midi;

    Rig()
    {
        out.setGroupId(kTestGroup);
        in.setGroupId(kTestGroup);
        in.setChannelId(kTestChannel);
        out.setPlayHead(&ph);
        in.setPlayHead(&ph);
        out.prepareToPlay(kSr, kBlock);
        in.prepareToPlay(kSr, kBlock);
    }

    ~Rig()
    {
        out.releaseResources();
        in.releaseResources();
    }

    // 跑宿主消息循环 ms 毫秒:真实触发两侧的 25Hz Timer(心跳/attach/判定/广播/特征拉取)。
    static void pumpMessages(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

    // 推 n 个音频块:Input 先于 Output(常见宿主顺序:源轨先算,总线后算)。
    // 每块之间穿插消息泵,让 [M] 侧有机会推进(与真实宿主的两线程并行等价)。
    // 宿主渲染顺序。默认 Input 先(源轨先算、总线后算);outputFirst_ 反过来 ——
    // 「宿主先渲染 Output 再渲染 Input」正是 A-3 primed 门的两大动机场景之一(N2)。
    bool outputFirst_ = false;

    void runBlocks(int blocks, float amplitude = 0.25f, int pumpEveryN = 4, int pumpMs = 8)
    {
        for (int b = 0; b < blocks; ++b)
        {
            fillSine(inBuf, amplitude, ph.timeSamples);
            outBuf.clear();

            if (outputFirst_)
            {
                out.processBlock(outBuf, midi);
                in.processBlock(inBuf, midi);
            }
            else
            {
                in.processBlock(inBuf, midi);
                out.processBlock(outBuf, midi);
            }

            if (ph.playing)
            {
                ph.timeSamples += kBlock;
            }
            if (pumpEveryN > 0 && (b % pumpEveryN) == pumpEveryN - 1)
            {
                pumpMessages(pumpMs);
            }
        }
    }

    static void fillSine(juce::AudioBuffer<float>& buf, float amp, std::int64_t t0)
    {
        for (int c = 0; c < buf.getNumChannels(); ++c)
        {
            float* d = buf.getWritePointer(c);
            for (int i = 0; i < buf.getNumSamples(); ++i)
            {
                const double t = static_cast<double>(t0 + i) / kSr;
                d[i] = amp * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * t));
            }
        }
    }

    // 等到该轨真正进注入集(claim + 心跳 + [J32] 200ms 注入延迟);超时返回 false。
    bool waitUntilInjected(int maxMs = 3000)
    {
        for (int waited = 0; waited < maxMs; waited += 40)
        {
            runBlocks(2, 0.25f, /*pumpEveryN=*/1, /*pumpMs=*/20);
            if (injected())
            {
                return true;
            }
        }
        return false;
    }

    // 该轨已 claim 且心跳新鲜 —— [J32] 200ms 注入延迟由上面的循环用真实时间等过。
    bool injected()
    {
        const auto snap = out.connSnapshot();
        const auto& c = snap.channels[kTestChannel - 1];
        return c.slotState == scvb::kSlotActive && c.heartbeatAgeMs <= 2000;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// L-5 循环区(A 族):宿主给循环区 → playhead 快照带 cycle + tempo,秒换算可得。
// ---------------------------------------------------------------------------
TEST_CASE("HOST L-5:宿主循环区经 playhead 快照可见并可换算成秒", "[host][t37][L5]")
{
    Rig r;

    // ① 无循环区:kCycleValid 不置位 —— 桥面据此判「循环区读不到」。
    r.ph.haveLoop = false;
    r.ph.playing = true;
    r.runBlocks(4);
    auto pod = r.out.playheadSnapshot();
    CHECK((pod.flags & scvb::engine::kPlayheadCycleValid) == 0);

    // ② 划一个 4 拍循环区(120 BPM → 每拍 0.5s,4 拍 = 2.0s)。
    r.ph.haveLoop = true;
    r.ph.looping = true;
    r.ph.loopStartPpq = 4.0;
    r.ph.loopEndPpq = 8.0;
    r.runBlocks(4);
    pod = r.out.playheadSnapshot();
    REQUIRE((pod.flags & scvb::engine::kPlayheadCycleValid) != 0);
    REQUIRE((pod.flags & scvb::engine::kPlayheadTempoValid) != 0);
    REQUIRE(pod.bpm == 120.0);

    // 桥面 hostLoopSeconds 的换算口径:秒 = ppq × 60 / bpm。
    const double secPerBeat = 60.0 / pod.bpm;
    CHECK(pod.loopStartPpq * secPerBeat == 2.0);
    CHECK(pod.loopEndPpq * secPerBeat == 4.0);

    // ③ 取消循环区 → 回到「读不到」,提示才该出现(修复前提示恒亮)。
    r.ph.haveLoop = false;
    r.ph.looping = false;
    r.runBlocks(4);
    CHECK((r.out.playheadSnapshot().flags & scvb::engine::kPlayheadCycleValid) == 0);
}

// ---------------------------------------------------------------------------
// L-2 / L-3 电平(B 族):audio 环 → processBlock 测量 → SPSC → 消息线程可读。
// ---------------------------------------------------------------------------
TEST_CASE("HOST L-2/L-3:有声时电平快照非零,静音后落回地板", "[host][t37][L2][L3]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // 有声:该轨与总线都应量到非零电平(修复前恒为 -60dB 地板 → 线性 0)。
    r.runBlocks(40, /*amplitude=*/0.5f);
    const auto loud = r.out.meterSnapshot();
    const std::size_t idx = kTestChannel - 1;
    CHECK(loud.trackPeak[idx] > 0.0f);
    CHECK(loud.trackRms[idx] > 0.0f);
    CHECK(loud.busPeak[0] > 0.0f);
    CHECK(loud.trackPeak[idx] >= loud.trackRms[idx]); // 峰值不小于 RMS

    // 静音输入:测量随之回零(液柱落底,而不是冻在上一块的高度)。
    r.runBlocks(40, /*amplitude=*/0.0f);
    const auto quiet = r.out.meterSnapshot();
    CHECK(quiet.trackPeak[idx] == 0.0f);
    CHECK(quiet.trackRms[idx] == 0.0f);
}

// ---------------------------------------------------------------------------
// L-6 失准(A 族):起播不误报;bypass 断流 → 报警;恢复 → 警告自行清除。
// ---------------------------------------------------------------------------
TEST_CASE("HOST L-6a:起播不误报失准", "[host][t37][L6]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.runBlocks(60);

    // 冷启动 + 起播期间不得产生失准(修复前所有注入轨会在同一块同时 +1)。
    for (int ch = 1; ch <= 15; ++ch)
    {
        CHECK(r.out.misalignCount(ch) == 0);
    }
}

TEST_CASE("HOST L-6b:bypass 断流 → 失准报警;恢复 → 警告自行清除", "[host][t37][L6]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.runBlocks(40);
    REQUIRE(r.out.misalignCount(kTestChannel) == 0);

    // 模拟「误 bypass 一个 Input」:Input 不再 processBlock(写头停滞),Output 照常读。
    for (int b = 0; b < 40; ++b)
    {
        r.outBuf.clear();
        r.out.processBlock(r.outBuf, r.midi);
        r.ph.timeSamples += kBlock;
        if ((b % 4) == 3)
        {
            Rig::pumpMessages(8);
        }
    }
    CHECK(r.out.misalignCount(kTestChannel) > 0); // 报警亮起(这是对的)

    // 重开 Input:两侧恢复正常推进,连续 >kMisalignRecoverMs(1s)无新缺口后计数归零。
    for (int waited = 0; waited < 4000; waited += 40)
    {
        r.runBlocks(2, 0.25f, /*pumpEveryN=*/1, /*pumpMs=*/20);
        if (r.out.misalignCount(kTestChannel) == 0)
        {
            break;
        }
    }
    CHECK(r.out.misalignCount(kTestChannel) == 0); // ← 修复前永远撤不下来
}

// ---------------------------------------------------------------------------
// L-6 采集链(A 族):Output 开采集 → 广播 → Input 布防写特征 → Output 拉取落账。
// 这条链上原本有两处断点:Input 的 captureArmed_ 没有写点、Output 侧没有读侧。
// ---------------------------------------------------------------------------
TEST_CASE("HOST L-6c:开采集 → 播放 → coverage 真实落账(分析按钮据此解锁)", "[host][t37][L6]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // 采集关:不该有任何覆盖。
    r.out.setCaptureEnabled(false);
    r.runBlocks(40);
    CHECK(r.out.coverageOf(kTestChannel, 0.0, 10.0).coveredS == 0.0);

    // 开采集 → 等广播到 Input(ctrl OutputGlobalInfo.capture_enabled)→ 播放一段。
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    r.runBlocks(400, 0.5f, /*pumpEveryN=*/4, /*pumpMs=*/6);
    Rig::pumpMessages(400);

    const auto cov = r.out.coverageOf(kTestChannel, 0.0, 4.0);
    CHECK(cov.coveredS > 0.0); // ← 修复前恒 0(界面显示「无采集数据」、分析按钮置灰)
    CHECK(cov.pct > 0.0f);
    CHECK(cov.pct <= 100.0f);
    CHECK_FALSE(cov.ranges.empty());

    // clearCoverage 打洞:清掉的量与之前落账的量同量级,清完覆盖归零。
    const double cleared = r.out.clearCoverage(1u << (kTestChannel - 1), 0.0, 4.0);
    CHECK(cleared > 0.0);
    CHECK(r.out.coverageOf(kTestChannel, 0.0, 4.0).coveredS == 0.0);
}

// ---------------------------------------------------------------------------
// L-4 配置双向同步(C 族):Output 改 priority/lead → Input 侧广播区可见;
// Input 远程改 priority → Output 的 state 真的变。
// ---------------------------------------------------------------------------
TEST_CASE("HOST L-4a:Output 改配置 → Input 侧广播区可见", "[host][t37][L4]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    Rig::pumpMessages(300);

    // Output 侧改:优先级 5→6、lead_lock 开、配对组 2、轨道名。
    auto& chCfg = r.out.runtime().channels[kTestChannel - 1];
    chCfg.priority = 6;
    chCfg.leadLock = true;
    chCfg.pairId = 2;
    chCfg.label = "Lead Vox";
    ++r.out.runtime().configSeq; // 桥面 setChannelConfig 的等效动作
    Rig::pumpMessages(300); // 让 Output 的 25Hz Timer 把广播区写出去

    const auto snap = r.in.bridgeTickSnapshot();
    REQUIRE(snap.broadcastValid); // ← 修复前广播区无布局、无人写,恒 false
    const auto& mirrored = snap.broadcast.channels[kTestChannel - 1];
    CHECK(mirrored.priority == 6); // ← 修复前 Input 侧恒 0
    CHECK((mirrored.flags & scvb::kCfgFlagLeadLock) != 0);
    CHECK(mirrored.pair_id == 2);
    CHECK(juce::String::fromUTF8(snap.broadcast.labels[kTestChannel - 1]) == "Lead Vox");
    CHECK(snap.configSeq != 0); // 变化检测真源可用(0 保留给「无广播」)

    // 再改一次 → Input 侧跟着变(「改 5→6 Input 不动」的直接回归)。
    chCfg.priority = 9;
    ++r.out.runtime().configSeq;
    Rig::pumpMessages(300);
    CHECK(r.in.bridgeTickSnapshot().broadcast.channels[kTestChannel - 1].priority == 9);
}

TEST_CASE("HOST L-4b:Input 远程改优先级 → 落到 Output 的 state 并广播回来", "[host][t37][L4]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    Rig::pumpMessages(300);

    const auto res = r.in.bridgeRemoteSetPriority(8);
    CHECK(res.queued); // 命令确实投进了 ctrl 命令环

    Rig::pumpMessages(400); // Output 的 25Hz Timer 消费命令环 → 落 state → 广播
    CHECK(r.out.runtime().channels[kTestChannel - 1].priority == 8); // ← 修复前记录被丢弃

    // 回执路径:改动经广播区回到 Input(UI 的乐观值据此让位)。
    const auto snap = r.in.bridgeTickSnapshot();
    REQUIRE(snap.broadcastValid);
    CHECK(snap.broadcast.channels[kTestChannel - 1].priority == 8);
}

// ---------------------------------------------------------------------------
// 手动写回(D 族):pan 与 vol 两个维度互相独立,且值真实落进段表。
// ---------------------------------------------------------------------------
TEST_CASE("HOST 手动写回:改 vol 不动 pan、改 pan 不动 vol,值真实落段表", "[host][t37][manual]")
{
    Rig r;
    r.ph.playing = true;
    r.runBlocks(4);

    const auto manualSeg = [&r]() {
        const auto crvs = r.out.crvsSnapshot();
        const auto& segs =
            crvs.versions[static_cast<std::size_t>(r.out.versionActive() - 1)].tracks[kTestChannel - 1].segments;
        REQUIRE(segs.size() == 1); // setTrackManual 的产物 = 单段全时限常值
        return segs.front();
    };

    int replaced = 0;
    int locked = 0;

    // ① 先调音量到 -8dB。
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/false, -8.0f, replaced, locked));
    CHECK(manualSeg().volDb == -8.0f);

    // ② 再调 pan 到 +55 —— 音量必须原样保留(修复前会被打回 0dB)。
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, 55.0f, replaced, locked));
    CHECK(manualSeg().pan == 55.0f);
    CHECK(manualSeg().volDb == -8.0f);

    // ③ 再调音量 —— pan 必须原样保留(修复前会被打回居中)。
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/false, 2.0f, replaced, locked));
    CHECK(manualSeg().volDb == 2.0f);
    CHECK(manualSeg().pan == 55.0f);

    // ④ 越界钳制照契约:pan ±100 / vol -24..+12。
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, 999.0f, replaced, locked));
    CHECK(manualSeg().pan == 100.0f);
    CHECK(manualSeg().volDb == 2.0f); // 另一维仍不受影响
}

// ---------------------------------------------------------------------------
// 时间线跳变(A 族):定位/跳转不得被当成失准。
// ---------------------------------------------------------------------------
TEST_CASE("HOST 时间线跳变(定位)不误报失准", "[host][t37][L6]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.runBlocks(40);
    REQUIRE(r.out.misalignCount(kTestChannel) == 0);

    // 宿主定位到远处(epoch 跳变):两侧都会重置代际,追赶期不该计失准。
    r.ph.timeSamples = 48000 * 30;
    r.runBlocks(60);

    for (int waited = 0; waited < 3000; waited += 40)
    {
        r.runBlocks(2, 0.25f, /*pumpEveryN=*/1, /*pumpMs=*/20);
        if (r.out.misalignCount(kTestChannel) == 0)
        {
            break;
        }
    }
    CHECK(r.out.misalignCount(kTestChannel) == 0);
}

// ---------------------------------------------------------------------------
// N2:宿主先渲染 Output 再渲染 Input。
// 这是 A-3 primed 门的两大动机场景之一(另一条是空环冷启动),默认顺序覆盖不到 ——
// Output 读 t0 时 Input 本块还没写,covered 判据必然不成立。primed 门必须对它免疫。
// ---------------------------------------------------------------------------
TEST_CASE("HOST N2:Output-first 渲染顺序不误报失准", "[host][t37][L6][N2]")
{
    Rig r;
    r.outputFirst_ = true;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.runBlocks(80);

    for (int ch = 1; ch <= 15; ++ch)
    {
        CHECK(r.out.misalignCount(ch) == 0);
    }
    // 顺序反转不影响电平链:Output 读到的是上一块的数据,依然是真实音频。
    CHECK(r.out.meterSnapshot().trackPeak[kTestChannel - 1] >= 0.0f);
}

// ---------------------------------------------------------------------------
// I1–I4 换组边界:改组 → 下一拍 → 四条数据面各自复位/重发。
// changeGroup 是这四条新数据面共同的失效边界:广播区短路门、Input 缓存、participate 推导、
// 采集覆盖,四处都可能把**上一组**的状态带进新组。
// ---------------------------------------------------------------------------
TEST_CASE("HOST I1/I2 换组:新组广播区被重写,Input 不再显示上一组配置", "[host][t37][changegroup]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    Rig::pumpMessages(300);

    // 旧组:写一份可辨认的配置。
    auto& cfgA = r.out.runtime().channels[kTestChannel - 1];
    cfgA.priority = 6;
    cfgA.label = "GroupA";
    ++r.out.runtime().configSeq;
    Rig::pumpMessages(300);
    REQUIRE(r.in.bridgeTickSnapshot().broadcast.channels[kTestChannel - 1].priority == 6);

    // 两侧一起改到新组(用户在 UI 上改组的等效动作)。
    constexpr int kOtherGroup = 6;
    r.out.setGroupId(kOtherGroup);
    r.in.setGroupId(kOtherGroup);
    Rig::pumpMessages(200);
    r.runBlocks(20, 0.25f, 1, 20);

    // I1:新组 ctrl 段是全新的一张,广播区必须被重写 —— 短路门只比 config_seq 会让它永不写。
    // 注意 runtime_.configSeq 没变(改组不是配置变更),所以这条正是 I1 的判据。
    for (int waited = 0; waited < 3000; waited += 40)
    {
        r.runBlocks(2, 0.25f, 1, 20);
        const auto sn = r.in.bridgeTickSnapshot();
        if (sn.broadcastValid && sn.broadcast.config_seq != 0)
        {
            break;
        }
    }
    const auto after = r.in.bridgeTickSnapshot();
    CHECK(after.broadcastValid);
    CHECK(after.broadcast.config_seq != 0); // ← I1:新组广播区确实被写过
    CHECK(after.broadcast.channels[kTestChannel - 1].priority == 6); // 新组读到的是 Output 当前实况
    CHECK(juce::String::fromUTF8(after.broadcast.labels[kTestChannel - 1]) == "GroupA");

    // I2:改回旧组后,Input 的缓存不得把新组那份当成旧组的(缓存按组作废)。
    r.out.setGroupId(kTestGroup);
    r.in.setGroupId(kTestGroup);
    Rig::pumpMessages(200);
    r.runBlocks(20, 0.25f, 1, 20);
    CHECK(r.in.bridgeTickSnapshot().broadcastValid == true);
}

TEST_CASE("HOST I3:本组无 Output 广播时,mono 轨 participate 不被误报成 false", "[host][t37][changegroup]")
{
    // Input 自己就是 ctrl 段的创建者:本组没有 Output 时段照样存在、广播区全零、seq=0 是偶数,
    // readBroadcast 会返回 true。若拿全零当实况,mono 轨的 participate_in_auto_pan 会被发成
    // false —— J60 默认应为 true。判据必须是 config_seq != 0 而不是 broadcastValid。
    juce::ScopedJuceInitialiser_GUI juceInit;
    ScvbInputAudioProcessor lone;
    lone.setGroupId(5); // 一个确定没有 Output 的组
    lone.setChannelId(2);
    lone.prepareToPlay(kSr, kBlock);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(300);

    const auto snap = lone.bridgeTickSnapshot();
    // 段被 Input 自己创建出来了,所以「读到了」——但 config_seq=0 说明没有 Output 在广播。
    CHECK(snap.configSeq == 0);

    scvb::input::bridge::ConfigSnapshot cfg;
    cfg.sourceChannels = 1; // mono
    cfg.configSeq = snap.configSeq;
    cfg.broadcastValid = snap.broadcastValid;
    cfg.channelId = 2;
    cfg.broadcast = snap.broadcast;

    const juce::var payload = scvb::input::bridge::buildConfigPayload(cfg);
    auto* o = payload.getDynamicObject();
    REQUIRE(o != nullptr);
    CHECK(static_cast<bool>(o->getProperty("participate_in_auto_pan")) == true); // ← 修复前会是 false
    CHECK(static_cast<int>(o->getProperty("priority")) == 0); // 无广播 → 默认值,不是「真的 0」

    lone.releaseResources();
}

TEST_CASE("HOST I4:换组后不继承上一组的采集覆盖", "[host][t37][changegroup]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // 旧组:采集出一段真实覆盖。
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    r.runBlocks(400, 0.5f, 4, 6);
    Rig::pumpMessages(300);
    REQUIRE(r.out.coverageOf(kTestChannel, 0.0, 4.0).coveredS > 0.0);

    // 改组:frameStore 按 channel 索引存、没有 group 维度 —— 不清的话新组 ch3 会继承旧组 ch3 的
    // CoverageMap,并把两组特征并进同一张表。
    r.out.setGroupId(6);
    Rig::pumpMessages(200);
    CHECK(r.out.coverageOf(kTestChannel, 0.0, 4.0).coveredS == 0.0); // ← 修复前继承旧组覆盖
}

// ---------------------------------------------------------------------------
// N1 布防两个维度(契约 §1 setCaptureEnabled:ON = 对 {enabled 轨} × {global.range} 布防)。
// ---------------------------------------------------------------------------
TEST_CASE("HOST N1:range 外不落账(布防时间维)", "[host][t37][arming]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // 把范围限定在 [2s, 4s):时间线从 0 起跑,前 2 秒不该记账。
    r.out.runtime().rangeMode = 2; // manual
    r.out.runtime().rangeStartS = 2.0;
    r.out.runtime().rangeEndS = 4.0;
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);

    // 只跑到 ~1.5s(< 2s),全程在 range 外。
    r.runBlocks(140, 0.5f, 4, 6);
    Rig::pumpMessages(300);
    CHECK(r.out.coverageOf(kTestChannel, 0.0, 2.0).coveredS == 0.0); // ← 修复前整条时间线都记
}

TEST_CASE("HOST N1:Output 关掉的轨不写特征(布防轨维)", "[host][t37][arming]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // Output 侧把本轨关掉并广播下去。
    r.out.runtime().channels[kTestChannel - 1].enabled = false;
    ++r.out.runtime().configSeq;
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(500); // 等广播区写出 + Input 读到并据此撤防

    r.runBlocks(400, 0.5f, 4, 6);
    Rig::pumpMessages(300);
    CHECK(r.out.coverageOf(kTestChannel, 0.0, 4.0).coveredS == 0.0); // ← 修复前照写不误

    // 重新启用 → 恢复布防,能采到。
    r.out.runtime().channels[kTestChannel - 1].enabled = true;
    ++r.out.runtime().configSeq;
    Rig::pumpMessages(500);
    r.runBlocks(400, 0.5f, 4, 6);
    Rig::pumpMessages(300);
    CHECK(r.out.coverageOf(kTestChannel, 0.0, 8.0).coveredS > 0.0);
}

// ---------------------------------------------------------------------------
// v4 实测 P0-2:持续 bypass 下「路由失准」不得自行清除。
// 上一轮把清除条件写成「连续 1s 无新缺口」,而 Input 被 bypass 后本轨会转 suspended、
// 退出注入集 → read() 不再被调用 → 缺口自然不再增长 → 警告在实际仍无声时撤下(假恢复)。
// 清除条件必须是「该轨数据真的在推进」。
// ---------------------------------------------------------------------------
TEST_CASE("HOST P0-2:持续 bypass 期间失准警告不得清除", "[host][t37][v4][L6]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.runBlocks(40);
    REQUIRE(r.out.misalignCount(kTestChannel) == 0);

    // Input 被 bypass:不再 processBlock(写头停滞),Output 照常推进。
    const auto runOutputOnly = [&r](int blocks) {
        for (int b = 0; b < blocks; ++b)
        {
            r.outBuf.clear();
            r.out.processBlock(r.outBuf, r.midi);
            r.ph.timeSamples += kBlock;
            if ((b % 4) == 3)
            {
                Rig::pumpMessages(8);
            }
        }
    };

    runOutputOnly(40);
    REQUIRE(r.out.misalignCount(kTestChannel) > 0); // 报警亮起(这是对的)

    // 继续 bypass 远超恢复窗(1s)——此时缺口已不再增长(本轨退出注入集,read 不再被调),
    // 但数据依然没有推进。警告**必须**保持。
    for (int round = 0; round < 6; ++round)
    {
        runOutputOnly(20);
        Rig::pumpMessages(200);
    }
    CHECK(r.out.misalignCount(kTestChannel) > 0); // ← 修复前这里会被判成「已恢复」归零

    // 真正重开 Input:两侧一起推进 → 数据恢复 → 警告才该撤下。
    bool cleared = false;
    for (int waited = 0; waited < 5000 && !cleared; waited += 40)
    {
        r.runBlocks(2, 0.25f, /*pumpEveryN=*/1, /*pumpMs=*/20);
        cleared = (r.out.misalignCount(kTestChannel) == 0);
    }
    CHECK(cleared);
}

// ---------------------------------------------------------------------------
// v4 实测 P1-5:轨「启用」开关必须真的影响音频。
// ---------------------------------------------------------------------------
TEST_CASE("HOST P1-5:关掉的轨不进混音", "[host][t37][v4][enabled]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setOutputEnabled(true);
    r.runBlocks(60, 0.5f);

    // 启用时:总线有声。
    CHECK(r.out.meterSnapshot().busPeak[0] > 0.0f);

    // 关掉该轨 → 整轨不注入 → 总线电平回零(本组只有这一条 Input)。
    r.out.runtime().channels[kTestChannel - 1].enabled = false;
    ++r.out.runtime().configSeq;
    Rig::pumpMessages(200); // 等 timerCallback 把位图推下去
    r.runBlocks(60, 0.5f);
    const auto off = r.out.meterSnapshot();
    CHECK(off.trackPeak[kTestChannel - 1] == 0.0f); // ← 修复前照混不误
    CHECK(off.busPeak[0] == 0.0f);

    // 重新启用 → 恢复出声。
    r.out.runtime().channels[kTestChannel - 1].enabled = true;
    ++r.out.runtime().configSeq;
    Rig::pumpMessages(200);
    r.runBlocks(60, 0.5f);
    CHECK(r.out.meterSnapshot().busPeak[0] > 0.0f);
}

// ---------------------------------------------------------------------------
// v4 实测 P1-4:手动写回反复多轮都必须生效,且必须落到冻结维度真正读的参数面。
// ---------------------------------------------------------------------------
TEST_CASE("HOST P1-4:手动写回落参数面 + 多轮交替不失效", "[host][t37][v4][manual]")
{
    Rig r;
    r.ph.playing = true;
    r.runBlocks(4);

    auto& apvts = r.out.getAPVTS();
    const int v = r.out.versionActive();
    auto* panP = apvts.getParameter(scvb::params::panId(v, kTestChannel));
    auto* volP = apvts.getParameter(scvb::params::volId(v, kTestChannel));
    REQUIRE(panP != nullptr);
    REQUIRE(volP != nullptr);

    int replaced = 0;
    int locked = 0;

    // 十轮交替写回:每轮两维都必须同时在**段表**与**参数面**上生效。
    for (int round = 1; round <= 10; ++round)
    {
        const float pan = static_cast<float>(round * 7 % 100) - 50.0f;
        const float vol = static_cast<float>(round % 12) - 6.0f;

        REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/false, vol, replaced, locked));
        REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, pan, replaced, locked));
        r.runBlocks(2);

        const auto crvs = r.out.crvsSnapshot();
        const auto& segs = crvs.versions[static_cast<std::size_t>(v - 1)].tracks[kTestChannel - 1].segments;
        REQUIRE(segs.size() == 1);
        CHECK(segs.front().pan == pan);
        CHECK(segs.front().volDb == vol); // 两维互不冲掉(D 族)

        // 参数面同步:冻结维度上引擎只读这里,不同步就等于「改了没反应」。
        CHECK(panP->convertFrom0to1(panP->getValue()) == Catch::Approx(pan).margin(0.01));
        CHECK(volP->convertFrom0to1(volP->getValue()) == Catch::Approx(vol).margin(0.01));
    }
}
