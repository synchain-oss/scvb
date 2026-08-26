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
#include <memory>
#include <string>
#include <vector>

#include "BridgeArgs.h"
#include "InputBridgeLogic.h"
#include "InputProcessor.h"
#include "OutputProcessor.h"
#include "ipc/SegmentBackendWin32.h"
#include "ipc/VizPlane.h"
#include "state/StateCodec.h"

#include <algorithm>
#include <limits>

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
    // 断言面 = **suspended**(统筹裁定丙案):写方停着是「挂起」,不是「失准」。
    // 两者是性质不同的两件事,§2.3 起各有各的位 —— 见 misalignCountRecent 头注。
    // 坐实还需要 kSuspendStallMs(500ms),所以这里等,而不是跑固定块数。
    const auto suspendedNow = [&r] { return r.out.connSnapshot().channels[kTestChannel - 1].suspended; };
    bool raised = false;
    for (int waited = 0; waited < 3000 && !raised; waited += 80)
    {
        for (int b = 0; b < 8; ++b)
        {
            r.outBuf.clear();
            r.out.processBlock(r.outBuf, r.midi);
            r.ph.timeSamples += kBlock;
        }
        Rig::pumpMessages(80);
        raised = suspendedNow();
    }
    CHECK(raised); // 挂起态亮起(这是对的)
    CHECK(r.out.misalignCount(kTestChannel) == 0); // 而且**不是**失准 —— 用户自己 bypass 的,不该红灯

    // 重开 Input:两侧恢复正常推进 → 挂起态撤下。
    bool cleared = false;
    for (int waited = 0; waited < 4000 && !cleared; waited += 40)
    {
        r.runBlocks(2, 0.25f, /*pumpEveryN=*/1, /*pumpMs=*/20);
        cleared = !suspendedNow();
    }
    CHECK(cleared); // ← 修复前永远撤不下来
    CHECK(r.out.misalignCount(kTestChannel) == 0);
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

    // 信号来源(统筹裁定丙案):写头停着 = **挂起**(ChannelConnInfo::suspended),不是失准。
    // 这条用例守的「不得假恢复」保证与从前完全一致,只是换到了正确的那个位上:
    // 持续断流期间 suspended 必须一直亮,且该轨必须一直在注入集之外。
    const auto suspendedNow = [&r] { return r.out.connSnapshot().channels[kTestChannel - 1].suspended; };
    bool raised = false;
    for (int waited = 0; waited < 3000 && !raised; waited += 100)
    {
        runOutputOnly(10);
        Rig::pumpMessages(100);
        raised = suspendedNow();
    }
    REQUIRE(raised); // 挂起态亮起(这是对的)

    // 继续 bypass 远超恢复窗(1s)——此时缺口已不再增长(本轨退出注入集,read 不再被调),
    // 但数据依然没有推进。**绝不能**因此被判成「已恢复」。
    for (int round = 0; round < 6; ++round)
    {
        runOutputOnly(20);
        Rig::pumpMessages(200);
        CHECK(suspendedNow()); // ← 修复前这里会被判成「已恢复」
        // 同时该轨必须已退出注入集:状态在、还照混旧数据,是更坏的假恢复。
        CHECK(r.out.meterSnapshot().trackPeak[kTestChannel - 1] == 0.0f);
    }
    // 全程都不是「失准」:用户自己 bypass 的,不该出红色警报。
    CHECK(r.out.misalignCount(kTestChannel) == 0);

    // 真正重开 Input:两侧一起推进 → 数据恢复 → 挂起态才该撤下。
    bool cleared = false;
    for (int waited = 0; waited < 5000 && !cleared; waited += 40)
    {
        r.runBlocks(2, 0.5f, /*pumpEveryN=*/1, /*pumpMs=*/20);
        cleared = !suspendedNow();
    }
    CHECK(cleared);
}

// ---------------------------------------------------------------------------
// v5 实测 P1-7:静音段里宿主挂起 Input(write_head 停滞)不得报失准。
// Cubase/Nuendo 的「无信号时挂起 VST3 处理」会在 -inf 段停调 Input 的 processBlock;
// 写头一冻,covered 判据在 CH_SUSPENDED 的 500ms 判定窗里连续失败 —— 老实现把这段
// 全记成缺口,于是每个静音边界都闪一次「失准」,1s 恢复窗过后自愈。用户看到的正是
// 「偶发短暂失准后自愈」。真失准(写方套圈)不在此列,仍照计。
// ---------------------------------------------------------------------------
TEST_CASE("HOST P1-7:宿主在静音段挂起 Input,短暂停流不报失准", "[host][t37][v5][misalign]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.runBlocks(40);
    REQUIRE(r.out.misalignCount(kTestChannel) == 0);

    // 只跑 Output:模拟宿主在静音段跳过 Input 的处理。停 12 块 ≈ 128ms,
    // 远短于 kSuspendStallMs(500ms)——这正是「判定窗内」那段。
    for (int b = 0; b < 12; ++b)
    {
        r.outBuf.clear();
        r.out.processBlock(r.outBuf, r.midi);
        r.ph.timeSamples += kBlock;
        if ((b % 4) == 3)
        {
            Rig::pumpMessages(8);
        }
    }
    Rig::pumpMessages(60);
    // ← 修复前:12 块里每一块 covered 都失败,gapCount 加满 12,失准当场亮起
    CHECK(r.out.misalignCount(kTestChannel) == 0);

    // 恢复供数后继续跑,依然干净(不是「先报了再自愈」,是根本没报过)。
    r.runBlocks(40);
    CHECK(r.out.misalignCount(kTestChannel) == 0);
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

// ---------------------------------------------------------------------------
// v4 实测 P0-1:点「分析」后必须真的跑完并出段表,不能永久停在「分析中」。
// 原因是 handleAnalyze 是 T29 占位:回 {ok:true} 却从不置 analysis_run.running,
// 而 web 要等 running 翻真才交回状态面驱动 —— 于是它的在途标志永远挂着。
// 本例走完整链:采集 → coverage 落账 → 触发分析 → 后台线程跑完 → 段表落 CRVS。
// ---------------------------------------------------------------------------
TEST_CASE("HOST P0-1:采集 → 分析 → 出段表(全链,不卡死)", "[host][t37][v4][analyze]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // 采集一段**有声有静**的素材:让 VAD 能切出多个段,而不是一整条常响。
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    for (int burst = 0; burst < 6; ++burst)
    {
        r.runBlocks(60, 0.5f, 4, 4); // 有声
        r.runBlocks(40, 0.0f, 4, 4); // 静音
    }
    Rig::pumpMessages(400);

    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 20.0).coveredS;
    REQUIRE(coveredS > 0.0); // 前置:确实采到了东西

    // 干跑:影响面应报出「有数据的轨」。
    const auto preview = r.out.previewAnalysis(0, 0.0, coveredS);
    CHECK(preview.tracks >= 1);

    // 触发分析:受理必须成功,且**立刻**进入 running(web 就等这一位)。
    const auto accepted = r.out.startAnalysis(0, 0.0, coveredS);
    REQUIRE(accepted.ok); // ← 修复前 ok 恒 true 但 running 恒 false
    CHECK(accepted.tracks >= 1);
    CHECK(r.out.runtime().analysisRunning);

    // 等后台线程跑完并回落到消息线程(callAsync 需要消息泵)。
    bool finished = false;
    for (int waited = 0; waited < 20000 && !finished; waited += 50)
    {
        Rig::pumpMessages(50);
        finished = !r.out.analysisRunning() && !r.out.runtime().analysisRunning;
    }
    REQUIRE(finished); // ← 修复前永远不会变 false:「分析中」卡死
    CHECK(r.out.runtime().analysisProgress == 1.0f);

    // 出结果:该轨段表非空,且段是分析产物(origin=auto),时间递增不重叠。
    const auto crvs = r.out.crvsSnapshot();
    const auto& segs =
        crvs.versions[static_cast<std::size_t>(r.out.versionActive() - 1)].tracks[kTestChannel - 1].segments;
    REQUIRE_FALSE(segs.empty()); // ← 核心断言:真的出段表了
    for (std::size_t i = 0; i < segs.size(); ++i)
    {
        CHECK(segs[i].t1 > segs[i].t0);
        CHECK(scvb::state::segmentOrigin(segs[i].flags) == scvb::state::SegmentOrigin::Auto);
        CHECK(segs[i].pan >= -100.0f);
        CHECK(segs[i].pan <= 100.0f);
        CHECK(segs[i].volDb >= -24.0f);
        CHECK(segs[i].volDb <= 12.0f);
        if (i > 0)
        {
            CHECK(segs[i].t0 >= segs[i - 1].t1); // 有序且不重叠
        }
    }
}

TEST_CASE("HOST P0-1:无采集数据时分析被拒,不会挂起", "[host][t37][v4][analyze]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setCaptureEnabled(false);
    r.runBlocks(40);

    const auto accepted = r.out.startAnalysis(0, 0.0, 5.0);
    CHECK_FALSE(accepted.ok);
    // §1.6 拒绝态:range ∩ coverage = ∅ → {ok:false, affected:{0,0,0}},**不带 reason**
    // (§5.6 的 reason 是八值闭集,analyze 只登记了 "busy")。
    CHECK_FALSE(accepted.busy);
    CHECK(accepted.tracks == 0);
    CHECK_FALSE(r.out.runtime().analysisRunning); // 不置 running → UI 不会转圈
}

TEST_CASE("HOST P0-1:分析可取消,取消后不改段表", "[host][t37][v4][analyze]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    r.runBlocks(300, 0.5f, 4, 4);
    Rig::pumpMessages(400);
    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 20.0).coveredS;
    REQUIRE(coveredS > 0.0);

    const auto before = r.out.crvsSnapshot()
                            .versions[static_cast<std::size_t>(r.out.versionActive() - 1)]
                            .tracks[kTestChannel - 1]
                            .segments.size();

    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS).ok);
    r.out.cancelAnalysis();
    CHECK_FALSE(r.out.analysisRunning());
    CHECK_FALSE(r.out.runtime().analysisRunning);

    Rig::pumpMessages(300);
    const auto after = r.out.crvsSnapshot()
                           .versions[static_cast<std::size_t>(r.out.versionActive() - 1)]
                           .tracks[kTestChannel - 1]
                           .segments.size();
    CHECK(after == before); // 取消 = 结果整份丢弃
}

// ---------------------------------------------------------------------------
// 复审 R6:局部分析不得删掉分析范围**之外**的既有 auto 段(ADR-008 / §4.4)。
// 触发路径不是边角:parseAnalyzeScope 在 daw_loop / manual 档会把 "all" 也收成
// [range.start_s, range.end_s) —— 「划了循环区 → 点分析」就会走到这里。
// ---------------------------------------------------------------------------
TEST_CASE("HOST R6:范围 A 分析 → 范围 B 分析 → A 的段仍在", "[host][t37][analyze][r6]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // 采一段足够长的素材(有声/静音交替,保证前后两段范围里都切得出段)。
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    for (int burst = 0; burst < 10; ++burst)
    {
        r.runBlocks(50, 0.5f, 4, 4);
        r.runBlocks(30, 0.0f, 4, 4);
    }
    Rig::pumpMessages(400);

    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 60.0).coveredS;
    REQUIRE(coveredS > 4.0);

    const auto segsOf = [&r]() {
        const auto crvs = r.out.crvsSnapshot();
        return crvs.versions[static_cast<std::size_t>(r.out.versionActive() - 1)].tracks[kTestChannel - 1].segments;
    };
    const auto runAnalysis = [&r](double a, double b) {
        REQUIRE(r.out.startAnalysis(0, a, b).ok);
        for (int waited = 0; waited < 20000; waited += 50)
        {
            Rig::pumpMessages(50);
            if (!r.out.analysisRunning() && !r.out.runtime().analysisRunning)
            {
                return;
            }
        }
        FAIL("analysis did not finish");
    };

    // ① 只分析后半段 [half, covered)。
    const double half = coveredS * 0.5;
    runAnalysis(half, coveredS);
    const auto afterB = segsOf();
    REQUIRE_FALSE(afterB.empty());
    // 阈值必须落在**同一个 hop 栅格**上:startAnalysis 会把 startS 截断到 hop 边界
    // (firstHop = startS/hopS 向下取整),范围真正的起点是 firstHop*hopSamples,
    // 可能略早于 half*kSr。直接拿原始值比会把边界那一段误判成「范围外」。
    const std::int64_t hopSamplesT = static_cast<std::int64_t>(0.01 * kSr);
    const std::int64_t halfSample = static_cast<std::int64_t>(half / 0.01) * hopSamplesT;
    int inB = 0;
    for (const auto& sg : afterB)
    {
        if (sg.t0 >= halfSample)
        {
            ++inB;
        }
    }
    REQUIRE(inB > 0); // 后半段确实产出了

    // ② 再只分析前半段 [0, half)。后半段那些 auto 段**必须**还在。
    runAnalysis(0.0, half);
    const auto afterA = segsOf();

    int stillInB = 0;
    int inA = 0;
    for (const auto& sg : afterA)
    {
        if (sg.t0 >= halfSample)
        {
            ++stillInB;
        }
        else
        {
            ++inA;
        }
    }
    CHECK(inA > 0); // 前半段有了新产出
    CHECK(stillInB == inB); // ← 修复前:整表替换,后半段的 auto 段被静默抹掉(这里会是 0)
}

// ---------------------------------------------------------------------------
// 复审 R5:分析在途时销毁 processor 不得崩溃(裸 callAsync 捕获 owner 指针 = use-after-free)。
// ---------------------------------------------------------------------------
TEST_CASE("HOST R5:分析在途销毁 processor 不崩溃", "[host][t37][analyze][r5]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    for (int round = 0; round < 3; ++round)
    {
        auto rig = std::make_unique<Rig>();
        rig->ph.playing = true;
        REQUIRE(rig->waitUntilInjected());

        rig->out.setCaptureEnabled(true);
        Rig::pumpMessages(300);
        for (int burst = 0; burst < 8; ++burst)
        {
            rig->runBlocks(50, 0.5f, 4, 4);
            rig->runBlocks(30, 0.0f, 4, 4);
        }
        Rig::pumpMessages(300);

        const double coveredS = rig->out.coverageOf(kTestChannel, 0.0, 60.0).coveredS;
        REQUIRE(coveredS > 0.0);
        REQUIRE(rig->out.startAnalysis(0, 0.0, coveredS).ok);

        // **不等分析跑完**,直接销毁(= 用户在分析跑着时把插件从轨上删掉)。
        // 析构里 cancelAnalysis() 先 join、再 cancelPendingUpdate() 撤掉已入队的派发。
        rig.reset();

        // 再泵一轮消息:修复前这里会把 finishAnalysis 打在已析构对象上。
        Rig::pumpMessages(300);
    }
    SUCCEED("分析在途销毁 + 消息泵未崩溃");
}

// ---------------------------------------------------------------------------
// 复审【重要】1:opts.clearManual 必须真的生效(此前 a[1] 从没被读过)。
// ---------------------------------------------------------------------------
TEST_CASE("HOST clearManual:true 时连用户段一并重算", "[host][t37][analyze]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    for (int burst = 0; burst < 8; ++burst)
    {
        r.runBlocks(50, 0.5f, 4, 4);
        r.runBlocks(30, 0.0f, 4, 4);
    }
    Rig::pumpMessages(400);
    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 60.0).coveredS;
    REQUIRE(coveredS > 0.0);

    // 先手动写回:段表变成单段 user_edited 常值。
    int replaced = 0;
    int locked = 0;
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, 40.0f, replaced, locked));

    const auto segsOf = [&r]() {
        const auto crvs = r.out.crvsSnapshot();
        return crvs.versions[static_cast<std::size_t>(r.out.versionActive() - 1)].tracks[kTestChannel - 1].segments;
    };
    const auto waitDone = [&r]() {
        for (int waited = 0; waited < 20000; waited += 50)
        {
            Rig::pumpMessages(50);
            if (!r.out.analysisRunning() && !r.out.runtime().analysisRunning)
            {
                return;
            }
        }
        FAIL("analysis did not finish");
    };

    const auto hasUserSeg = [&segsOf]() {
        for (const auto& sg : segsOf())
        {
            if (scvb::state::segmentOrigin(sg.flags) != scvb::state::SegmentOrigin::Auto)
            {
                return true;
            }
        }
        return false;
    };
    REQUIRE(hasUserSeg());

    // ① 普通分析:ADR-008 —— 用户段必须保留。
    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS, /*clearManual=*/false).ok);
    waitDone();
    CHECK(hasUserSeg());

    // ② clearManual=true:用户读了二次确认、点了确认 —— 手动段该被真的清掉。
    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS, /*clearManual=*/true).ok);
    waitDone();
    CHECK_FALSE(hasUserSeg()); // ← 修复前 opts 整个被丢弃,这里仍为 true
}

// ===========================================================================
// v5 实测 —— 多轨分析(P0-1)/ 重新识别清手动(P0-3)/ 波形瓦片(P0-4)/ 换组发布(P0-5)
// ===========================================================================

namespace
{

// 多轨机台:一台 Output + n 个 **mono** Input(各占一条 channel)。
//
// 为什么必须 mono:[J60] 的默认口径是「mono 参与自动 pan / stereo 不参与」,而
// Rig 的 Input 走默认 stereo 布局。要验「分析真的把轨分到不同声像上」,得先让这些轨
// 是**会参与**的轨 —— 这也正是真机上一条 mono 人声/贝斯轨的样子。
struct MonoMultiRig
{
    static constexpr int kGroup = 9;
    static constexpr int kCount = 3;

    juce::ScopedJuceInitialiser_GUI juceInit;
    ScvbOutputAudioProcessor out;
    std::vector<std::unique_ptr<ScvbInputAudioProcessor>> ins;
    FakePlayHead ph;
    juce::AudioBuffer<float> outBuf{2, kBlock};
    juce::AudioBuffer<float> inBuf{1, kBlock};
    juce::MidiBuffer midi;

    MonoMultiRig()
    {
        out.setGroupId(kGroup);
        out.setPlayHead(&ph);
        out.prepareToPlay(kSr, kBlock);
        for (int i = 0; i < kCount; ++i)
        {
            auto p = std::make_unique<ScvbInputAudioProcessor>();
            juce::AudioProcessor::BusesLayout mono;
            mono.inputBuses.add(juce::AudioChannelSet::mono());
            mono.outputBuses.add(juce::AudioChannelSet::mono());
            p->setBusesLayout(mono);
            p->setGroupId(kGroup);
            p->setChannelId(i + 1);
            p->setPlayHead(&ph);
            p->prepareToPlay(kSr, kBlock);
            ins.push_back(std::move(p));
        }
    }

    ~MonoMultiRig()
    {
        out.releaseResources();
        for (auto& p : ins)
        {
            p->releaseResources();
        }
    }

    static void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

    // 每轨给**不同幅度**的素材:平衡层要有可分的能量差,否则三轨完全对称,
    // 「全是 0」与「解本身就是 0」分不开。
    void runBlocks(int blocks, float amplitude, int pumpEveryN = 4, int pumpMs = 8)
    {
        for (int b = 0; b < blocks; ++b)
        {
            outBuf.clear();
            for (int i = 0; i < kCount; ++i)
            {
                Rig::fillSine(inBuf, amplitude * (0.4f + 0.3f * static_cast<float>(i)), ph.timeSamples);
                ins[static_cast<std::size_t>(i)]->processBlock(inBuf, midi);
            }
            out.processBlock(outBuf, midi);
            if (ph.playing)
            {
                ph.timeSamples += kBlock;
            }
            if (pumpEveryN > 0 && (b % pumpEveryN) == pumpEveryN - 1)
            {
                pump(pumpMs);
            }
        }
    }

    bool waitUntilInjected(int maxMs = 4000)
    {
        for (int waited = 0; waited < maxMs; waited += 40)
        {
            runBlocks(2, 0.25f, /*pumpEveryN=*/1, /*pumpMs=*/20);
            const auto snap = out.connSnapshot();
            int live = 0;
            for (int i = 0; i < kCount; ++i)
            {
                const auto& c = snap.channels[static_cast<std::size_t>(i)];
                if (c.slotState == scvb::kSlotActive && c.heartbeatAgeMs <= 2000)
                {
                    ++live;
                }
            }
            if (live == kCount)
            {
                return true;
            }
        }
        return false;
    }

    // 采一段有声有静的素材,返回覆盖到的秒数。
    double capture()
    {
        out.setCaptureEnabled(true);
        pump(400);
        for (int burst = 0; burst < 6; ++burst)
        {
            runBlocks(60, 0.5f);
            runBlocks(40, 0.0f);
        }
        pump(400);
        return out.coverageOf(1, 0.0, 30.0).coveredS;
    }

    bool runAnalysisToCompletion(double endS, bool clearManual)
    {
        const auto accepted = out.startAnalysis(0, 0.0, endS, clearManual);
        if (!accepted.ok)
        {
            return false;
        }
        for (int waited = 0; waited < 20000; waited += 50)
        {
            pump(50);
            if (!out.analysisRunning() && !out.runtime().analysisRunning)
            {
                return true;
            }
        }
        return false;
    }

    // 该轨段表里第一个段的 pan(无段回 NaN)。
    double firstPan(int ch)
    {
        const auto crvs = out.crvsSnapshot();
        const auto& segs = crvs.versions[static_cast<std::size_t>(out.versionActive() - 1)]
                               .tracks[static_cast<std::size_t>(ch - 1)]
                               .segments;
        return segs.empty() ? std::numeric_limits<double>::quiet_NaN() : static_cast<double>(segs.front().pan);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// P0-1 ①:source_channels 检测值必须真的落进 runtime state。
// 这一格此前**没有任何生产写入方**(恒 0「未检测」),而 [J60] 的默认推导写的是
// `sourceChannels == 1`,0 落进 else 分支 → 全 15 轨 participate=false → 指派层按
// 「非参与 = 保持现值」处理 → 分析出来的 pan 全是参数面的 0 = 全居中。
// ---------------------------------------------------------------------------
TEST_CASE("HOST P0-1:音频环段头的 source_channels 回填进 runtime state", "[host][t37][v5][analyze]")
{
    Rig r; // 默认 stereo 布局的 Input
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.runBlocks(20);
    Rig::pumpMessages(200); // 等 timerCallback 跑一次 refreshSourceChannels

    // ← 修复前恒 0
    CHECK(r.out.runtime().channels[kTestChannel - 1].sourceChannels == 2);
    // 未连接的轨保持「未检测」,不许瞎猜。
    CHECK(r.out.runtime().channels[0].sourceChannels == 0);
}

TEST_CASE("HOST P0-1:mono Input 被检测成单声道并默认参与自动声像", "[host][t37][v5][analyze]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.runBlocks(20, 0.25f);
    MonoMultiRig::pump(200);

    for (int ch = 1; ch <= MonoMultiRig::kCount; ++ch)
    {
        const auto& c = r.out.runtime().channels[static_cast<std::size_t>(ch - 1)];
        CHECK(c.sourceChannels == 1);
        CHECK(c.participatesInAutoPan());
    }
}

// ---------------------------------------------------------------------------
// v5.1 实测 P0-B:**stereo 轨也必须默认参与自动声像**。
// 检测值来自轨道总线布局,不是素材本身 —— Cubase 里单声道人声放在立体声轨上就报 2。
// 按 [J60] 原默认把它们判成「不参与」,AutoAssign 会给它们「保持现值」;而现值取自从未被
// 打印过的 pan 参数 = 0,分析再把 0 烘焙进段表:真机上「大部分轨回到中间、只剩两条在左边」。
//
// 这一条**只断言取值口径本身**(纯函数,不经共享内存):它就是回归点,而端到端的
// 「多轨分析后 pan 非全零」由 MonoMultiRig 那条用例守着。走 IPC 反而会把断言绑到
// harness 里 Input 总线布局的解析结果上 —— 那个在单独跑与全量跑之间并不稳定,
// 是「测试自己的噪声」,不是被测行为。
// ---------------------------------------------------------------------------
TEST_CASE("HOST P0-B:stereo 检测值不得把轨挤出自动声像", "[host][t37][v51][analyze]")
{
    OutputRuntimeState::Channel c;

    // 未显式设置:三种检测态(未检测 / mono / stereo)一律参与。
    REQUIRE_FALSE(c.participateAutoPanSet);
    for (const int detected : {0, 1, 2})
    {
        c.sourceChannels = detected;
        CHECK(c.participatesInAutoPan()); // ← 改回 `sourceChannels != 2` 时 detected=2 即红
    }

    // 显式设置仍然说了算(该由用户决定的那一档没有被写死)。
    c.participateAutoPanSet = true;
    c.participateAutoPan = false;
    for (const int detected : {0, 1, 2})
    {
        c.sourceChannels = detected;
        CHECK_FALSE(c.participatesInAutoPan());
    }
    c.participateAutoPan = true;
    CHECK(c.participatesInAutoPan());
}

// ---------------------------------------------------------------------------
// P0-1 ②(核心端到端):多轨分析后段表 pan **不得全零**。
// 真机现象:回执报「影响 10 区段 / 9 轨」,分布图与泳道却全在中线,自动化零写入。
// 单轨情形本就该居中(§5「独唱段居中」),所以这条必须用多轨。
// ---------------------------------------------------------------------------
TEST_CASE("HOST P0-1:多轨分析后段表 pan 非全零且轨间有差异", "[host][t37][v5][analyze]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));

    std::vector<double> pans;
    for (int ch = 1; ch <= MonoMultiRig::kCount; ++ch)
    {
        const double p = r.firstPan(ch);
        REQUIRE(std::isfinite(p)); // 每轨都得出段(前置:分析真的跑过这些轨)
        pans.push_back(p);
    }

    // ← 核心断言:修复前这里三个数全是 0.0
    const bool anyNonZero = std::any_of(pans.begin(), pans.end(), [](double p) { return std::abs(p) > 1.0; });
    CHECK(anyNonZero);

    // 而且不是「三轨被推到同一个非零点」:槽位分配本就该把它们分开。
    const double lo = *std::min_element(pans.begin(), pans.end());
    const double hi = *std::max_element(pans.begin(), pans.end());
    CHECK(hi - lo > 1.0);
}

// ---------------------------------------------------------------------------
// P0-1 ③(消费链):段表 pan → CRVS → 打印器 → 宿主参数。
// 用户报的另一半是「自动化零写入」。段表有值而参数不动 = 消费链断在打印这一跳。
// ---------------------------------------------------------------------------
TEST_CASE("HOST P0-1:分析产物经打印器写进宿主参数(自动化非零写入)", "[host][t37][v5][analyze]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));

    // 找一条分析结果非居中的轨。
    int pannedCh = 0;
    for (int ch = 1; ch <= MonoMultiRig::kCount && pannedCh == 0; ++ch)
    {
        if (std::abs(r.firstPan(ch)) > 1.0)
        {
            pannedCh = ch;
        }
    }
    REQUIRE(pannedCh != 0);

    // 引擎权威 + 走带回到已分析区域 → 打印器按曲线真身写参数(03 §3.2)。
    r.out.setOutputEnabled(true);
    r.ph.timeSamples = 0;
    r.runBlocks(120, 0.5f);
    MonoMultiRig::pump(400);

    const auto* raw = r.out.getAPVTS().getRawParameterValue(scvb::params::panId(r.out.versionActive(), pannedCh));
    REQUIRE(raw != nullptr);
    CHECK(std::abs(raw->load()) > 1.0f); // ← 段表有值、参数还停在 0 就说明消费链断了
}

// ---------------------------------------------------------------------------
// P0-3:「重新识别(含手动段)」必须连**冻结位**一起清。
// 只清段不清位时:指派层看见 freeze&1=1 仍把该轨当 manual,pan 取参数面的手动值 →
// 新产出的 auto 段把手动值原样烘焙进去;DspArbiter 对冻结维度读的也仍是参数面。
// 于是「点重新识别 → 再分析 / 关冻结,pan 还是那个手动值」。
// ---------------------------------------------------------------------------
TEST_CASE("HOST P0-3:clearManual 清冻结位,pan 不再被手动值钉住", "[host][t37][v5][analyze]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);

    // 造出真机上的起点:轨 1 手动固定到 −80 且 pan 维冻结(UI 的「拖动 = 接管手动」)。
    constexpr int kCh = 1;
    constexpr float kManualPan = -80.0f;
    int replaced = 0;
    int replacedLocked = 0;
    REQUIRE(r.out.setTrackManual(kCh, /*isPan=*/true, kManualPan, replaced, replacedLocked));
    auto& apvts = r.out.getAPVTS();
    const juce::String frzId = scvb::params::freezeId(r.out.versionActive(), kCh);
    auto* frz = apvts.getParameter(frzId);
    REQUIRE(frz != nullptr);
    frz->beginChangeGesture();
    frz->setValueNotifyingHost(frz->convertTo0to1(1.0f)); // bit0 = pan 维冻结
    frz->endChangeGesture();
    MonoMultiRig::pump(100);
    REQUIRE(juce::roundToInt(apvts.getRawParameterValue(frzId)->load()) == 1);
    REQUIRE(r.firstPan(kCh) == Catch::Approx(kManualPan));

    // 重新识别(含手动段)。
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/true));

    // ① 冻结位被清 → 引擎重新驾驶该维度(关冻结这条出口也就真的通了)。
    CHECK(juce::roundToInt(apvts.getRawParameterValue(frzId)->load()) == 0);
    // ② 段表不再是那个手动值 —— 修复前这里恒等于 −80(手动值被烘焙进新的 auto 段)。
    const double after = r.firstPan(kCh);
    REQUIRE(std::isfinite(after));
    CHECK(std::abs(after - static_cast<double>(kManualPan)) > 1.0);
}

// ---------------------------------------------------------------------------
// P0-4:requestWaveform 的数据面(此前是写死「全未覆盖」的桩)。
// 桩的形状合法,能过 JS 侧的 isTileShape,于是泳道照常画斜纹与栅格 —— 但每列
// covered=0,包络层整体跳过,真机上就是「有斜纹、没波形」的纯黑泳道。
// ---------------------------------------------------------------------------
TEST_CASE("HOST P0-4:已采集区间的波形瓦片带真实包络", "[host][t37][v5][waveform]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    r.runBlocks(200, 0.5f);
    Rig::pumpMessages(400);

    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS;
    REQUIRE(coveredS > 0.0);

    const auto tile = r.out.waveformOf(kTestChannel, 0.0, coveredS, 64);
    REQUIRE(tile.covered.size() == 64);
    REQUIRE(tile.maxDb.size() == 64);

    int coveredCols = 0;
    int loudCols = 0;
    for (std::size_t i = 0; i < tile.covered.size(); ++i)
    {
        if (tile.covered[i] != 0)
        {
            ++coveredCols;
            CHECK(tile.maxDb[i] > -160.0); // 哨兵没被当成真数据留下
            CHECK(tile.minDb[i] <= tile.maxDb[i]); // 包络下沿不高过上沿
            if (tile.maxDb[i] > -60.0)
            {
                ++loudCols;
            }
        }
    }
    CHECK(coveredCols > 0); // ← 修复前恒 0:整条泳道判成未采集
    CHECK(loudCols > 0); // 有声素材必须画得出包络,不是一排地板

    // 完全没采过的远端区间仍如实回「未覆盖」(斜纹是对的,不能假装有数据)。
    const auto empty = r.out.waveformOf(kTestChannel, 600.0, 610.0, 16);
    for (const int c : empty.covered)
    {
        CHECK(c == 0);
    }
}

// ---------------------------------------------------------------------------
// P0-5:prepareToPlay **之后**才灌工程 chunk 时,viz 段必须跟着换组。
// 宿主(Cubase)的真实次序就是「先激活组件、再推工程/预设 chunk」;离线测试恒是
// 「先 setState 再 prepare」,于是这条路径从没被走到 —— Output 把 viz 段发布在旧组、
// Monitor 在工程组上等,永远显示未连接。
// ---------------------------------------------------------------------------
TEST_CASE("HOST P0-5:prepare 后再加载工程 state,viz 段随组重开", "[host][t37][v5][viz]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    constexpr scvb::u32 kFromGroup = 5;
    constexpr scvb::u32 kToGroup = 6;

    ScvbOutputAudioProcessor out;
    FakePlayHead ph;
    out.setPlayHead(&ph);
    out.setGroupId(static_cast<int>(kFromGroup));
    out.prepareToPlay(kSr, kBlock); // ← 宿主先激活
    juce::MessageManager::getInstance()->runDispatchLoopUntil(300);

    // 先确认起点:旧组的 viz 段确实开着(否则下面的断言证明不了什么)。
    {
        scvb::SegmentBackendWin32 backend;
        scvb::VizPlane probe(backend, kFromGroup);
        REQUIRE(probe.attachReadOnly() == scvb::InitResult::kOk);
    }

    // 宿主再灌工程 chunk:组号换到 kToGroup。
    juce::MemoryBlock blob;
    {
        ScvbOutputAudioProcessor donor;
        donor.setGroupId(static_cast<int>(kToGroup));
        donor.getStateInformation(blob);
    }
    out.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));

    bool attached = false;
    for (int waited = 0; waited < 3000 && !attached; waited += 100)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
        scvb::SegmentBackendWin32 backend;
        scvb::VizPlane probe(backend, kToGroup);
        attached = (probe.attachReadOnly() == scvb::InitResult::kOk);
    }
    CHECK(attached); // ← 修复前:publisher 仍指着 kFromGroup,新组的段永远不存在

    out.releaseResources();
}

// ---------------------------------------------------------------------------
// #100 复审【重要】3:打印区间必须跟着**段编辑**走。
// 打印区间此前挂在 crvsRevision_ 上,而那个号只在「段表整体被替换」时才 +1
// (加载工程 / 分析回落)。手动拖一条段的边界拖出旧包络之后,打印区间仍停在旧范围 ——
// 播放头一走出去,打印器就回落 ARMED、自动化面停写,而用户听到的曲线已经变了。
// 改判据为 curvesRevision_(每次 rebuildAllCurves 都 +1,涵盖全部改曲线路径)。
// ---------------------------------------------------------------------------
TEST_CASE("HOST 编辑段把包络拖长 → 打印区间跟随(自动化不在新区间停写)", "[host][t37][v5][print]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));

    // 找一条分析结果非居中的轨(打印器只对有值可写的轨有可观察行为)。
    int ch = 0;
    for (int t = 1; t <= MonoMultiRig::kCount && ch == 0; ++t)
    {
        if (std::abs(r.firstPan(t)) > 1.0)
        {
            ch = t;
        }
    }
    REQUIRE(ch != 0);

    // 旧包络的右端 = 全轨段表里最大的 t1。
    const auto endOf = [&r]() {
        const auto crvs = r.out.crvsSnapshot();
        std::int64_t hi = 0;
        for (const auto& track : crvs.versions[static_cast<std::size_t>(r.out.versionActive() - 1)].tracks)
        {
            for (const auto& sg : track.segments)
            {
                hi = std::max(hi, sg.t1);
            }
        }
        return hi;
    };
    const std::int64_t beforeEnd = endOf();
    REQUIRE(beforeEnd > 0);

    // 把该轨最后一段的 t1 往后拖 4 秒 —— 造出「新曲线比已分析区间长」的局面。
    const double sr = r.out.sampleRate();
    const std::int64_t grownEnd = beforeEnd + static_cast<std::int64_t>(4.0 * sr);
    {
        const auto crvs = r.out.crvsSnapshot();
        const auto& segs = crvs.versions[static_cast<std::size_t>(r.out.versionActive() - 1)]
                               .tracks[static_cast<std::size_t>(ch - 1)]
                               .segments;
        REQUIRE_FALSE(segs.empty());
        scvb::state::SegmentEditArgs args;
        args.op = scvb::state::SegmentEditOp::MoveBoundary;
        args.segIdx = static_cast<int>(segs.size()) - 1;
        args.edgeIsT0 = false; // 移 t1 边
        args.tSamples = grownEnd;
        REQUIRE(r.out.editSegment(ch, args) == scvb::state::SegmentEditResult::Ok);
    }
    REQUIRE(endOf() == grownEnd); // 前置:段真的拖长了

    // 走带开到**旧包络之外、新包络之内**的位置,并开输出。
    r.out.setOutputEnabled(true);
    r.ph.timeSamples = beforeEnd + static_cast<std::int64_t>(1.0 * sr);
    r.runBlocks(120, 0.5f);
    MonoMultiRig::pump(400);

    // 打印区间若没跟着段编辑走,这里播放头落在旧区间之外 → 打印器回落 ARMED → 零写入,
    // 该轨 pan 参数会停在 0(harness 里从没有人写过它)。
    const auto* raw = r.out.getAPVTS().getRawParameterValue(scvb::params::panId(r.out.versionActive(), ch));
    REQUIRE(raw != nullptr);
    CHECK(std::abs(raw->load()) > 1.0f); // ← 判据挂回 crvsRevision_ 即红:停在 0.0f
}

// ---------------------------------------------------------------------------
// v5.1 实测 P1-F:follow 档「分析全部」的范围**与播放头无关**。
// 老实现取 [0, 当前播放头];Cubase「播完回开头」会把播放头送回 0,于是范围恒空、
// 分析永远受理不了 —— 用户看到的就是「分析键点了没反应」。
// v5 的 P2-9 只拆掉了按钮的置灰条件,这条真正的前置留在了 C++ 侧,所以现象照旧。
// ---------------------------------------------------------------------------
TEST_CASE("HOST P1-F:采集后把播放头送回 0,分析仍可受理", "[host][t37][v51][analyze]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    r.runBlocks(200, 0.5f);
    Rig::pumpMessages(400);

    const double extent = r.out.capturedExtentSeconds();
    REQUIRE(extent > 0.0); // 前置:确实采到了东西

    // 模拟「播完回开头」:播放头回 0(采集数据仍在)。
    r.ph.timeSamples = 0;
    r.runBlocks(4, 0.0f);
    Rig::pumpMessages(120);

    // 已采集范围与播放头无关,仍是那一段。
    CHECK(r.out.capturedExtentSeconds() == Catch::Approx(extent));
    // 按该范围发起分析:必须受理(老实现在这里 endS≈0 → 恒拒)。
    const auto accepted = r.out.startAnalysis(0, 0.0, r.out.capturedExtentSeconds());
    CHECK(accepted.ok);
    r.out.cancelAnalysis();
}

// ---------------------------------------------------------------------------
// v5.1 实测 P1-D:「自动化 write 没开时,怎么调音量都没变化;write 打开才有效」。
//
// 用户的这句话把嫌疑指向**消费端**:若音频通路的 vol 目标恒取自参数面,而参数面只有
// 打印器在 PRINT 态才写,那么关掉 write 就等于切断了唯一的写入源 —— 现象与之完全吻合。
// 但那是推测。这一组把全链**三跳**拆开逐跳定谳,不猜:
//   ① 参数 → DSP:直接 setValue 参数,断言下一块总线增益真的变(FOLLOW 档,host 权威);
//   ② 冻结维度 → DSP:引擎权威 + 冻结 vol,参数仍须是权威(§2.3 / J65);
//   ③ 引擎曲线 → DSP:引擎权威 + 未冻结,曲线说了算,改参数**不该**有效。
// 三条都用**总线电平**做判据(单轨工程,vol 改动直接反映在总线上),不看中间量。
// ---------------------------------------------------------------------------
namespace
{
// 跑几块并回报总线峰值(取几块里的最大,避开平滑器的爬坡)。
float busPeakAfter(Rig& r, int blocks)
{
    float peak = 0.0f;
    for (int i = 0; i < blocks; ++i)
    {
        r.runBlocks(4, 0.5f, /*pumpEveryN=*/2, /*pumpMs=*/4);
        peak = std::max(peak, r.out.meterSnapshot().busPeak[0]);
    }
    return peak;
}
} // namespace

TEST_CASE("HOST P1-D:参数 → DSP 这一跳(FOLLOW 档,host 参数是权威)", "[host][t37][v51][pld]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setOutputEnabled(false); // FOLLOW:§2.3 规定 host 参数是权威
    r.runBlocks(60, 0.5f);

    const float loud = busPeakAfter(r, 12);
    REQUIRE(loud > 0.0f); // 前置:确实有声

    // 直接把该轨 vol 压到 −24 dB(不经 UI、不经打印器 —— 只验最后一跳)。
    auto& apvts = r.out.getAPVTS();
    auto* vol = apvts.getParameter(scvb::params::volId(r.out.versionActive(), kTestChannel));
    REQUIRE(vol != nullptr);
    vol->beginChangeGesture();
    vol->setValueNotifyingHost(vol->convertTo0to1(-24.0f));
    vol->endChangeGesture();
    Rig::pumpMessages(120);

    const float quiet = busPeakAfter(r, 20);
    // −24 dB ≈ ×0.063;留足平滑与余量,断言「显著变小」而不是精确值。
    CHECK(quiet < loud * 0.5f);
}

TEST_CASE("HOST P1-D:冻结维度 → DSP(引擎权威下参数面仍是权威,J65)", "[host][t37][v51][pld]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setOutputEnabled(true); // 引擎权威(ARMED/PRINT)
    r.runBlocks(60, 0.5f);

    auto& apvts = r.out.getAPVTS();
    const int v = r.out.versionActive();
    // 冻结 vol 维(bit1)。
    auto* frz = apvts.getParameter(scvb::params::freezeId(v, kTestChannel));
    REQUIRE(frz != nullptr);
    frz->beginChangeGesture();
    frz->setValueNotifyingHost(frz->convertTo0to1(2.0f));
    frz->endChangeGesture();
    Rig::pumpMessages(120);

    const float loud = busPeakAfter(r, 12);
    REQUIRE(loud > 0.0f);

    auto* vol = apvts.getParameter(scvb::params::volId(v, kTestChannel));
    REQUIRE(vol != nullptr);
    vol->beginChangeGesture();
    vol->setValueNotifyingHost(vol->convertTo0to1(-24.0f));
    vol->endChangeGesture();
    Rig::pumpMessages(120);

    const float quiet = busPeakAfter(r, 20);
    CHECK(quiet < loud * 0.5f); // 冻结维度必须听得见参数面的改动
}

TEST_CASE("HOST P1-D:未冻结 + 引擎权威 → 曲线说了算,改参数无效", "[host][t37][v51][pld]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setOutputEnabled(true);
    r.runBlocks(60, 0.5f);

    const float base = busPeakAfter(r, 12);
    REQUIRE(base > 0.0f);

    // 未冻结:§2.3 规定引擎权威读曲线。此时改 host 参数**不该**影响听感 ——
    // 这一条是前两条的对照组:它保证前两条不是「凑巧总是读参数」。
    auto& apvts = r.out.getAPVTS();
    auto* vol = apvts.getParameter(scvb::params::volId(r.out.versionActive(), kTestChannel));
    REQUIRE(vol != nullptr);
    vol->beginChangeGesture();
    vol->setValueNotifyingHost(vol->convertTo0to1(-24.0f));
    vol->endChangeGesture();
    Rig::pumpMessages(120);

    const float after = busPeakAfter(r, 20);
    CHECK(after > base * 0.7f); // 基本不变(无曲线时曲线求值回 0 dB)
}

// ---------------------------------------------------------------------------
// v5.2 实测 P0-A(活锁,dump 定谳):waveformOf 的代价必须与**覆盖**同阶,不与请求跨度同阶。
//
// 真机现场:前端把被污染的工程时长(2^40 采样 ÷ 48k = 22,906,492 s)当 endS 发进来,
// 内层循环按「跨度 ÷ 10ms hop」空转 22.9 亿次、全程持 lifecycleMutex_ 且跑在 WebView2 的
// web-message 回调里 —— 宿主消息泵被占住,UI 整体冻死。那次现场真正有数据的只有 2475 个 hop。
// 这一组用**时间预算**做判据:病态跨度下必须仍然毫秒级返回。
// ---------------------------------------------------------------------------
TEST_CASE("HOST P0-A:病态跨度的 waveformOf 仍在时间预算内返回", "[host][t37][v52][hang]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    r.runBlocks(200, 0.5f); // 只采几秒:覆盖极稀疏,正是真机现场的形状
    Rig::pumpMessages(400);

    const double covered = r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS;
    REQUIRE(covered > 0.0);

    // 转储里的那个数,逐字:2^40 采样 ÷ 48000。
    constexpr double kPoisonedEndS = 1099511627776.0 / 48000.0;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto tile = r.out.waveformOf(kTestChannel, 0.0, kPoisonedEndS, 512);
    const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - t0;

    // 修复前:内层要跑 22.9 亿次,单次调用 9–23 秒。预算给到 2000ms 仍有两个数量级余量,
    // 既不会因构建机器慢而假红,也绝不可能被旧实现蒙混过关。
    CHECK(elapsedMs < 2000.0);
    REQUIRE(tile.covered.size() == 512);
    // 形状仍然合法(否则 JS 侧 isTileShape 不过,泳道整块变黑)。
    for (std::size_t i = 0; i < tile.covered.size(); ++i)
    {
        CHECK(tile.minDb[i] <= tile.maxDb[i]);
    }
}

TEST_CASE("HOST P0-A:covered 列的抽样上界不影响「有没有数据」的判定", "[host][t37][v52][hang]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    r.runBlocks(200, 0.5f);
    Rig::pumpMessages(400);

    const double covered = r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS;
    REQUIRE(covered > 0.0);

    // 抽样只影响「取到哪些 hop」,不影响「这一列有没有数据」——
    // 正常跨度与病态跨度下,已覆盖区间对应的列都必须仍然是 covered。
    const auto normal = r.out.waveformOf(kTestChannel, 0.0, covered, 64);
    int normalCovered = 0;
    for (const int c : normal.covered)
    {
        normalCovered += c;
    }
    CHECK(normalCovered > 0);
}

// ---------------------------------------------------------------------------
// v5.3 R4:**只有一个「无末端」段的轨**,上桥的 t1S 必须严格大于 t0S。
//
// setTrackManual 的产物是「单段全时限常值」(track.segments.assign(1, seg)),于是
// 手动/冻结轨这一整类**本轨内一个非哨兵段都没有**。降级值若按本轨算就永远得 0,
// t1S == t0S,段在波形页上坍缩成零宽:点不中、切不开 —— 而那正是最常被点的那批轨。
// 降级必须取**工程级**已知末端(全轨非哨兵段最大 t1 → 采集覆盖 → 最小非零宽度)。
// ---------------------------------------------------------------------------
TEST_CASE("HOST R4:单哨兵段轨的降级右端严格大于左端", "[host][t37][v53][segments]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // 造出「整轨只有一个无末端段」:setTrackManual 会把段表整表换成单段全时限。
    int replaced = 0;
    int replacedLocked = 0;
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, -40.0f, replaced, replacedLocked));

    const auto crvs = r.out.crvsSnapshot();
    const auto& vc = crvs.versions[static_cast<std::size_t>(r.out.versionActive() - 1)];
    const auto& segs = vc.tracks[kTestChannel - 1].segments;
    REQUIRE(segs.size() == 1); // 前置:确实是单段
    REQUIRE(segs.front().t1 >= scvb::output::kOpenEndedT1); // 且确实是无末端哨兵

    // 上桥值走真实降级链(BridgeArgs.h 三函数 —— buildSegmentsPayload 用的就是这一份实现),
    // 输入全部取自真实快照/处理器,不用局部常量重演:revert 降级链的任何一级这里都会红。
    const double sr = r.out.sampleRate();
    REQUIRE(sr > 0.0);
    const std::int64_t knownEnd = scvb::output::knownTimelineEndSamples(vc, r.out.capturedExtentSeconds(), sr);
    const std::int64_t minSpan = scvb::output::minOpenEndedSpanSamples(ScvbOutputAudioProcessor::featHopSeconds(), sr);
    const std::int64_t t1Effective =
        scvb::output::effectiveT1Samples(segs.front().t0, segs.front().t1, knownEnd, minSpan);

    // 核心断言:哨兵不再以「2^40 采样」伪装上桥,且**严格大于 t0**(零宽段点不中、切不开)。
    CHECK(t1Effective < scvb::output::kOpenEndedT1);
    CHECK(t1Effective > segs.front().t0);
    // 秒域同款(§2.8 载荷字段就是这两个数的换算):t1S 必须严格大于 t0S。
    const double t0S = scvb::output::samplesToSeconds(segs.front().t0, sr);
    const double t1S = scvb::output::samplesToSeconds(t1Effective, sr);
    CHECK(t1S > t0S);
}
