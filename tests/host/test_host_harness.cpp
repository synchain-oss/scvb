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
// [SL-231] 另有一个**保留组** kNoWriterGroup:本文件恒不在该组建任何段,viz 的反向断言
// (「邻组没有写方 ⇒ 只读方拿空态」)靠它成立。给新用例分组号时**不要分它**,
// 否则那条反向会变成一次难解释的假红 —— 失败信息指向 viz 装配,真因却是组号撞车。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <string>
#include <utility> // [SL-273] std::make_pair(pan/vol 两路快照)
#include <vector>

#include "support/exclusive_guard.h"

#include "BridgeArgs.h"
#include "analysis/HopMath.h" // [SL-263] 采样点→hop 的唯一换算口径(与实现同源)
#include "BridgeBase.h" // [SL-234] Min/MaxUiScale + clampUiScalePercent(档位边界真源)
#include "InputBridgeLogic.h"
#include "UiDefaultsStore.h" // [SL-208] 缩放档位全局默认
#include "InputProcessor.h"
#include "OutputProcessor.h"
#include "ipc/SegmentBackendWin32.h"
#include "ipc/VizPlane.h"
#include "state/FeaturesCodec.h" // [SL-215] isValidSessionGuid
#include "state/InputStateCodec.h" // [SL-234] Input 侧 CFGS 同一处夹取缺口
#include "state/OutputStateCodec.h"
#include "state/SidecarStore.h" // [SL-233] owner.lock 续租
#include "state/StateCodec.h"
#include "state/StateMigration.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <set> // [SL-231] GestureSpy 的配对校验

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

// [SL-231] 保留组:**恒无写方**,只给 viz 反向断言用。本文件出现过的组号是 4/5/6/7/9,
// 8 空着 —— 勿分配给任何 rig(理由见文件头「组号纪律」)。
constexpr int kNoWriterGroup = 8;
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
// [SL-324] 同机独占守卫已抽成四套共用的 `tests/support/exclusive_guard.h`
//(原 [SL-314] 的 host 专用版在那里,锁名由 `Local\SCVB-host-tests` 改为
// `Local\SCVB-tests-proc`)。改共用的理由:固定组号在四套之间大面积重叠,
// 每套一把锁挡得住同套并发、挡不住跨套件 —— 详见该头的头注。

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
        REQUIRE(out.groupId() == kTestGroup); // [SL-324] 读回断言,见 MonoMultiRig::kGroup 的头注

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

    // [SL-222] 等到**音频真的在流**(本轨电平量得到东西)。
    //
    // `waitUntilInjected` 只保证**控制面**通了:slot claim + 心跳新鲜。它不保证
    // [J32] 的 200ms 注入延迟已经走完,更不保证首块音频已经进环、被 Output 量到 ——
    // 机器负载高时 `meterSnapshot()` 会间歇性地还停在地板上,L-2/L-3 于是以
    // `trackPeak == 0` 假红(既有假红,与代码无关)。写法照 SL-189 那条用例的 settle 循环。
    //
    // 判据用**本轨** trackPeak,而不是 busPeak:总线可能被别的轨先点亮,那对
    // 「这一轨的电平通了没有」不构成证据。
    //
    // ⚠ 它**只等,不断言** —— 等满预算仍是 0 时照样返回 false 让调用方的 CHECK 去红。
    // 真回归(电平恒地板)时判据必须还留在用例里,不能被这个 helper 悄悄搬走。
    bool waitUntilAudioFlowing(int maxRounds = 60)
    {
        for (int i = 0; i < maxRounds; ++i)
        {
            if (out.meterSnapshot().trackPeak[kTestChannel - 1] > 0.0f)
            {
                return true;
            }
            runBlocks(4, 0.5f, /*pumpEveryN=*/2, /*pumpMs=*/8);
        }
        return out.meterSnapshot().trackPeak[kTestChannel - 1] > 0.0f;
    }

    // [SL-222] 反向的同一件事:喂静音,等本轨电平**落回地板**。
    //
    // 为什么也需要它:Input 量到的电平经 IPC 到 Output 有一段管线延迟,「喂 40 块静音」
    // 是个拍脑袋的定长,并不保证那段延迟已经走完。实测**单独跑** `[L2]` 时
    // `quiet.trackPeak` 会稳定停在 0.49999(上一段有声的值)而整套跑就是绿的 ——
    // 差别只是整套里前面那些用例顺带把管线预热了。这与有声那一向是同一族的时序耦合,
    // 一并按 settle 修掉,顺带让这一例**单独跑也成立**(此前只有整套跑才绿)。
    //
    // 同样**只等不断言**:落不回去(真回归 = 液柱冻在上一块的高度)时等满预算返回 false,
    // 判据仍留在用例的 CHECK 上。
    bool waitUntilAudioQuiet(int maxRounds = 60)
    {
        for (int i = 0; i < maxRounds; ++i)
        {
            if (out.meterSnapshot().trackPeak[kTestChannel - 1] == 0.0f)
            {
                return true;
            }
            runBlocks(4, 0.0f, /*pumpEveryN=*/2, /*pumpMs=*/8);
        }
        return out.meterSnapshot().trackPeak[kTestChannel - 1] == 0.0f;
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
    //
    // [SL-222] 先 settle 再取快照:claim + 心跳只说明控制面通了,不代表首块音频已经
    // 进环并被量到,机器负载高时下面的 trackPeak 会间歇性地还是 0(假红)。
    // settle 只等不断言,判据仍是下面那几条 CHECK —— 真回归时它等满预算,CHECK 照样红。
    const bool flowing = r.waitUntilAudioFlowing();
    INFO("waitUntilAudioFlowing=" << flowing); // 假红与真回归在失败输出里一眼可分
    r.runBlocks(40, /*amplitude=*/0.5f);
    const auto loud = r.out.meterSnapshot();
    const std::size_t idx = kTestChannel - 1;
    CHECK(loud.trackPeak[idx] > 0.0f);
    CHECK(loud.trackRms[idx] > 0.0f);
    CHECK(loud.busPeak[0] > 0.0f);
    CHECK(loud.trackPeak[idx] >= loud.trackRms[idx]); // 峰值不小于 RMS

    // 静音输入:测量随之回零(液柱落底,而不是冻在上一块的高度)。
    // [SL-222] 同样先 settle:定长的 40 块并不保证 Input→IPC→Output 这段管线延迟走完。
    //
    // ⚠ 先 REQUIRE(flowing):`waitUntilAudioQuiet` 有一条**平凡通过**路径 —— 这一轨若从来
    // 就没通过(电平恒 0),它第一轮即 return true,下面两条 CHECK 也随之平凡为真。
    // 上半段虽然会先红、INFO 也能分辨,但把前提显式钉住,静音这半边才真的有判据
    // (#156 复审【建议】5)。
    REQUIRE(flowing);
    r.runBlocks(40, /*amplitude=*/0.0f);
    const bool quieted = r.waitUntilAudioQuiet();
    INFO("waitUntilAudioQuiet=" << quieted);
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
    REQUIRE(r.out.groupId() == kOtherGroup); // [SL-324] 读回断言
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
    REQUIRE(r.out.groupId() == kTestGroup); // [SL-324] 读回断言
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
    REQUIRE(r.out.groupId() == 6); // [SL-324] 读回断言
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

    // [SL-193] 段表出来之后再干跑一次 —— §1.5 的三个数这时候必须都说得出话。
    //
    // 修复前 `previewAnalysis()` **从来没给 `intervals` 赋过值**,它一路留在成员初值 0,
    // 桥面把这个 0 原样回给 web;而 web 的「重分析选区」判据当时正是 `intervals > 0`,
    // 于是真机上这颗钮**恒灰**、影响预览行恒报「将影响 0 区段」(用户 v5.4 实测原话:
    // 「为什么重分析选区一直是灰色的?」)。mock 的 affectedOf() 一直算得好好的,
    // web-preview 里永远复现不出来 —— 只有真跑到这里才拦得住。
    //
    // **反向验证**:把 previewAnalysis 里的 `++a.intervals` 删掉,本条即红。
    const auto preview2 = r.out.previewAnalysis(0, 0.0, coveredS);
    CHECK(preview2.tracks >= 1);
    CHECK(preview2.intervals >= 1);
    // 范围过滤真的在生效:取一个**远在采集区之后**的空窗,三个数必须全 0
    // (修复前 manualKept 不按范围过滤,整轨的用户段会漏进来)。
    const auto preview3 = r.out.previewAnalysis(0, coveredS + 60.0, coveredS + 120.0);
    CHECK(preview3.tracks == 0);
    CHECK(preview3.intervals == 0);
    CHECK(preview3.manualKept == 0);
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

// ---------------------------------------------------------------------------
// #148 复审【重要】③:clearManual × locked 三方一致。
//
// 契约 §1.6「`locked=true` 段不受影响,**须先逐段解锁**」/ §5.4「`locked` 段免疫」,
// mock 的 `isProtectedSegment` 也是「先看 locked、再看 clearManual」—— 只有 native 把这两
// 个判据折进同一个 `!clearManual &&` 短路里(#87 接 opts 时漏的),于是「重新识别(含手动段)」
// 把用户显式上的锁一并抹掉。三方里错的是 native,契约与 mock 一字未动。
//
// 反向验证:把 finishAnalysis 的 `isLocked ||` 去掉(退回旧式)后本用例的 ② 必红。
// ---------------------------------------------------------------------------
TEST_CASE("HOST clearManual 不得清掉 locked 段(契约 §1.6/§5.4)", "[host][t37][analyze][SL230]")
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
    const auto lockedCount = [&segsOf]() {
        int n = 0;
        for (const auto& sg : segsOf())
        {
            if (scvb::state::segmentLocked(sg.flags))
            {
                ++n;
            }
        }
        return n;
    };

    // 手动写回 → 单段 user_edited 且 **locked=false**(makeManualConstantSegment 的口径)。
    int replaced = 0;
    int replacedLocked = 0;
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, 40.0f, replaced, replacedLocked));
    REQUIRE(segsOf().size() == 1);
    REQUIRE(lockedCount() == 0);

    // 用户显式挂锁(§5.4 set_locked:只改 locked,不动 origin)。
    scvb::state::SegmentEditArgs lock;
    lock.op = scvb::state::SegmentEditOp::SetLocked;
    lock.segIdx = 0;
    lock.locked = true;
    // ⚠ editSegment 的 track 是 **0 基**(桥面 OutputEditor.cpp 也是 `ch - 1` 再进来),
    // 与同一个类上 1 基的 setTrackManual(ch) 不同口径 —— 传错不会报错,只会静默改到隔壁轨。
    REQUIRE(r.out.editSegment(kTestChannel - 1, lock) == scvb::state::SegmentEditResult::Ok);
    REQUIRE(lockedCount() == 1);

    // ① 普通分析:锁定段当然保留(存量口径,顺带把「锁真的挂上了」再钉一遍)。
    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS, /*clearManual=*/false).ok);
    waitDone();
    CHECK(lockedCount() == 1);

    // ② clearManual=true:origin 那层保护放开,**锁这层不放** —— 段必须还在。
    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS, /*clearManual=*/true).ok);
    waitDone();
    CHECK(lockedCount() == 1); // ← 修复前锁定段被一并抹掉,这里为 0
}

// ===========================================================================
// v5 实测 —— 多轨分析(P0-1)/ 重新识别清手动(P0-3)/ 波形瓦片(P0-4)/ 换组发布(P0-5)
// ===========================================================================

namespace
{

// 多轨机台:一台 Output + n 个 **mono** Input(各占一条 channel)。
//
// 为什么取 mono:立这台机器时按 [J60] 的默认口径(「mono 参与自动 pan / stereo 不参与」),
// 而 Rig 的 Input 走默认 stereo 布局 —— 不改成 mono 这些轨就不参与,验不了「分析真的把轨
// 分到不同声像上」。[J83] 之后**三种检测态一律参与**,本 rig 已不再依赖这一点;保留 mono
// 是因为它就是真机上一条人声/贝斯轨的样子,顺带让 source_channels 有个确定值。
struct MonoMultiRig
{
    // [SL-324] 原为 **9 —— 一个不存在的组**。契约把组号钉死在 G=1..8
    // (`kMaxGroups = 8`,`docs/IPC_CONTRACT.md`「§1 registry:G=1..8」与冻结那节),
    // 而 `Input/OutputSession::setGroupId` 对越界值**不报错、静默回落到默认组 1**:
    //     groupId_ = (g >= 1 && g <= kMaxGroups) ? g : kOutputDefaultGroup;  // 默认 = 1
    // 所以这 24 个用例一直跑在 **g1** 上 —— 而 g1 是 ipc 套件砸得最狠的组
    // (`--group=1` 三十余处 + `resetRegistry(backend, 1)` 六处,后者**重置 registry**),
    // 也是 `scvb_tests` 的 lifecycle 用例和**真 DAW 实例**的默认组。代码、注释、日志
    // 三处都会告诉你它在 g9,只有读回 `groupId()` 才看得见真相 —— 见下面的读回断言。
    //
    // 改用 **g3**:host 套件自己不用 g3(它用 4/5/6/7/8),所以**同一进程内无同组用例**;
    // 跨进程的重叠(ipc 也用 g3)由本卡的共用互斥 `Local\SCVB-tests-proc` 兜住。
    // 拆卸依据:本 rig 的析构对 Output 与每个 Input 都调 `releaseResources()`,
    // 段随最后一个句柄关闭而销毁,所以同一进程内下一个用例拿到的是干净的 g3。
    static constexpr int kGroup = 3;
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
        // [SL-324] **读回断言**:越界会静默回落到默认组,写了什么不等于用了什么。
        REQUIRE(out.groupId() == kGroup);
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
        // [SL-324] Input 侧也读回一次。**为什么不像 Output 那样紧跟 setGroupId 断言**:
        // `ScvbInputAudioProcessor` 没有无副作用的组号访问器,唯一能读到的是
        // `bridgeTickSnapshot()`,而它会**懒打开 ctrl 段** —— 在构造中途调它会改变
        // 本 rig 所测的东西。所以放在三个 Input 都 `prepareToPlay` 之后统一读一次;
        // 越界回落若发生,这里同样会红。
        for (auto& p : ins)
        {
            REQUIRE(p->bridgeTickSnapshot().groupId == kGroup);
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

    // -----------------------------------------------------------------------
    // [SL-273] 每轨**不同波形**(峰值因数各异)的素材。
    //
    // **为什么非要它** —— 这是本卡改的核心缺陷。本机台原本三轨都是
    // `fillSine(amp_i · s(t))`:同一条正弦、同一相位,只缩幅度。于是三轨互为**标量倍**,
    // x_i(t) = c_i · s(t),而三档 z 全都正比于 c_i²:
    //     z_kw   = mean(kw_i)        = c_i² · mean(kw_s)          (K 加权是线性滤波,能量二次)
    //     z_rms  = (mean √kw_i)²     = c_i² · (mean √kw_s)²
    //     z_peak = max(peak_i)²      = c_i² · max(peak_s)²         (峰值是未加权样本峰)
    // 换档相当于给每轨的 z 乘一个系数 r_i = z_mode/z_kw,而上面三行说明
    // **r_i 与 i 无关**(那三个比值把 c_i² 整个约掉了)—— 换档是纯粹的**共模缩放**。
    // 平衡解对「所有 z 同乘一个常数」严格不变(`zHat = z/zSum` 约掉,增益求解同理),
    // 所以理论上换档后产出应当**逐位相同**;实测残留的 maxDiff ≈ 0.0011 是每轨
    // K 加权 IIR 在 float 下各自舍入的产物,**不是信号**。
    // 旧用例把这 0.0011 当成「换档真的传到了」的证据,阈值只好一路调到 1e-6 ——
    // 那是在拿浮点噪声当判据:实现真的断链时它同样可能非零,方向上并不安全。
    //
    // 旧注释里那句「均值 ∝ amp²、峰值 ∝ amp,两种基准给出的相对能量排序不同」是**错的**:
    // `peak_dbfs` 档返回的是 `max(peak)²`,与均值一样 ∝ amp²(见 `BalanceBasis.h` 该分支)。
    // 幅度差**永远**产生不了档间分歧,不管幅度差多大。
    //
    // **改法**:让三轨不再互为标量倍 —— 同样的响歇包络、同样的基频,但**峰值因数**
    // (peak / rms)差一个数量级。峰值因数正是 `max(peak)²` 与 `mean(kw)` 之间的那个比,
    // 它一变,r_i 就真的离散了,换档才有**设计出来的**信号可断言。
    //   · ch1 正弦     —— crest = √2 ≈ 1.41
    //   · ch2 窄脉冲串 —— 64 样本周期里只响 4 个,crest ≈ 5.7(齿音/爆破音的替身)
    //   · ch3 方波     —— crest = 1(压过头的垫底轨的替身)
    // 三路都是 `amp × (值域含在 [-1, 1] 的波形)`,故 **没有任何一路会削顶**
    // (amp 最大 0.5)。方波的样本峰恰好是 amp;正弦在足够多样本上取到 amp;
    // 窄脉冲串取到的是那 4 个开窗样本里的最大值,略低于 amp —— 三者的**样本峰**
    // 都在同一量级,而 `mean(kw)` 被 crest 拉开一个数量级,
    // r_peak = max(peak)²/mean(kw) 的离散度就是这么来的。
    // (所以本机台的离散度是**设计出来的**,不像旧版那样来自舍入残差;
    //  具体数值由用例自己断言,不在这里写死。)
    //
    // ⚠ 只有显式 `variedCrest = true` 的用例吃这份素材。默认档保持原样,是为了让
    // 本文件另外二十余条用例的产出**逐位不变** —— 它们断的是别的面,不该被本卡搅动。
    static void fillVaried(juce::AudioBuffer<float>& buf, float amp, std::int64_t t0, int trackIdx)
    {
        for (int c = 0; c < buf.getNumChannels(); ++c)
        {
            float* d = buf.getWritePointer(c);
            for (int i = 0; i < buf.getNumSamples(); ++i)
            {
                const std::int64_t n = t0 + i;
                const double t = static_cast<double>(n) / kSr;
                const double s = std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * t);
                double v = 0.0;
                if (trackIdx == 1)
                {
                    // 窄脉冲串:64 样本一个周期,只有头 4 个样本出声。
                    // 峰值 **≈** amp(不是恰好 amp:64 样本窗相对 440Hz 的 109.09 样本
                    // 周期在滑动,开窗那几个样本里 |sin| 的上确界实测 0.99999)——
                    // 与本函数头注「略低于 amp」同口径,别把这两句写成一句「就是 amp」。
                    // 均方则降到 ~1/16 ⇒ crest 拉高约 4 倍。
                    v = ((n % 64) < 4) ? s : 0.0;
                }
                else if (trackIdx == 2)
                {
                    // 方波:峰值 = amp、均方 = amp² ⇒ crest = 1(三轨里最低)。
                    v = (s >= 0.0) ? 1.0 : -1.0;
                }
                else
                {
                    v = s; // 正弦:crest = √2
                }
                d[i] = amp * static_cast<float>(v);
            }
        }
    }

    // [SL-273] 每轨波形是否**各不相同**。默认 false = 三轨同相位正弦、只缩幅度
    // (本文件二十余条用例的既有素材,保持逐位不变);置 true 见 `fillVaried` 头注。
    bool variedCrest = false;

    // 每轨给**不同幅度**的素材:平衡层要有可分的能量差,否则三轨完全对称,
    // 「全是 0」与「解本身就是 0」分不开。
    void runBlocks(int blocks, float amplitude, int pumpEveryN = 4, int pumpMs = 8)
    {
        for (int b = 0; b < blocks; ++b)
        {
            outBuf.clear();
            for (int i = 0; i < kCount; ++i)
            {
                const float amp = amplitude * (0.4f + 0.3f * static_cast<float>(i));
                if (variedCrest)
                {
                    fillVaried(inBuf, amp, ph.timeSamples, i);
                }
                else
                {
                    Rig::fillSine(inBuf, amp, ph.timeSamples);
                }
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
    // ⚠ 返回的是覆盖**时长**,不是覆盖的**结束时刻** —— 别拿它当分析窗右端。
    //   本 rig 的采集是在 `waitUntilInjected()` 把播放头推出去之后才打开的,所以覆盖区
    //   **不从 0 起**;`[0, coveredS)` 与真实覆盖区只是部分相交,相交多少取决于机器负载。
    //   要分析窗请用 `coverageWindow()`(见下)。[SL-292]
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

    // [SL-292] 真实覆盖区间 → 分析窗 `[startS, endS)`。
    // `HOST SL263` 因 #171 复审【重要】已经这么取窗,那段口径抽成 helper 放这儿,
    // 免得每个新用例都要重新想一遍「coveredS 是时长不是时刻」——同族判例复发过一次
    // (SL-284 写这台 rig 的新用例时又拿 coveredS 当了右端),抽出来才不会再复发。
    struct Window
    {
        double startS = 0.0;
        double endS = 0.0;
    };
    // ⚠ 窗口取自 **`ch` 这一轨**的覆盖区,不是多轨并集;`MonoMultiRig` 有三轨,默认按 ch1 定窗。
    //   (三轨是同一次 `runBlocks` 一起喂的,覆盖区实质同步,所以按 ch1 定窗对本 rig 成立;
    //    哪天有用例让某一轨单独起停,就得显式传 `ch` 或改成并集,别默认它还成立。)
    // `probeEndS` 只是**探测窗**,与 `capture()` 里写死的 30.0 是两个口径:
    // `capture()` 返回的 `coveredS` 是被那个 30 s 查询窗**截断过**的时长,
    // 而这里默认探到 600 s,为的是别把覆盖区右端截掉。两个数不必相等,但别把它们读成同一个。
    Window coverageWindow(int ch = 1, double probeEndS = 600.0)
    {
        const auto cov = out.coverageOf(ch, 0.0, probeEndS);
        if (cov.ranges.empty())
        {
            // 出声再回 {0,0}:否则调用处只红出 `0.0 > 0.0`,读不出「压根没采到覆盖」——
            // 正是本卡要治的「红错原因」。
            // ⚠ **必须是 `UNSCOPED_INFO` 不能是 `INFO`**:`INFO` 展开成栈上的
            // `Catch::ScopedMessage`,析构即 `popScopedMessage`(见 catch_message.cpp),
            // `return` 一走消息就被弹掉,**到不了调用处**——上一版就是这么写的,实测红出来
            // 仍只有 `0.0 > 0.0`,等于没加。`UNSCOPED_INFO` 留到**下一条断言**为止,
            // 而调用处紧跟着的就是那条 `REQUIRE(win.endS > win.startS)`,窗口正好对上。
            UNSCOPED_INFO("coverageWindow: ch=" << ch << " 在 [0, " << probeEndS << ") 内没有任何覆盖区间");
            return {}; // 调用处用 `REQUIRE(w.endS > w.startS)` 接住:没采到东西就该红在那儿
        }
        const double hopS = ScvbOutputAudioProcessor::featHopSeconds();
        return {static_cast<double>(cov.ranges.front().begin) * hopS,
                static_cast<double>(cov.ranges.back().end) * hopS};
    }

    // [#152 复审【建议】1] 只采**纯静音**:覆盖区有,但一句人声都没有 —— 分析零产出。
    // 返回 {采集起点秒, 该起点之后的覆盖时长}。
    //
    // ⚠ 必须**先在采集关闭下跑够静音**再开采集,冲的是 **Input 的响度状态**,不是环深度:
    // ADR-007 / IPC_CONTRACT §3「仅采集开关 ON 且播放时写」—— 采集 OFF 期间特征环里**根本
    // 不会有帧**;但 `InputProcessor.cpp` 那一步的注释同时写明「K 加权与 hop 累加**在播放中恒跑**」,
    // 于是 `captureArmed_` 翻 true 的那一刻,**K 加权 IIR 与 hop 累加器里还存着 `waitUntilInjected`
    // 那段 0.25f 正弦的能量** —— 头几个 hop 带残余响度,VAD 真检出一小段。
    // 「零产出」这个前提于是随时序摇摆(实测:同一份 C++ 换个构建就翻面,4 连过与 4 连红各出现过)。
    // 240 块 ≈ 2.56 s 静音是给那条 IIR 衰减留的余量(所以这个数冲的是滤波器状态,调它要按衰减想)。
    struct SilentCapture
    {
        double fromS = 0.0; // 开采集那一刻的时间轴位置(绝对秒)
        double coveredS = 0.0; // 自 fromS 起的覆盖时长
    };
    SilentCapture captureSilence()
    {
        runBlocks(240, 0.0f); // 采集**关闭**下空跑:把 K 加权 IIR / hop 累加器里的残余能量衰掉
        pump(400);
        SilentCapture r{};
        r.fromS = static_cast<double>(ph.timeSamples) / kSr;
        out.setCaptureEnabled(true);
        pump(400);
        for (int burst = 0; burst < 6; ++burst)
        {
            runBlocks(100, 0.0f);
        }
        pump(400);
        // 只问**开采集之后**那一段的覆盖:排空段在采集关闭期跑过,不该算进来。
        r.coveredS = out.coverageOf(1, r.fromS, r.fromS + 30.0).coveredS;
        return r;
    }

    bool runAnalysisToCompletion(double endS, bool clearManual) { return runAnalysisIn(0.0, endS, clearManual); }

    // 显式起点版(#152 复审:captureSilence 的覆盖不再从 0 起,区间得跟着走 ——
    // 否则排空块数一调大,startAnalysis 会直接拒受,红在这里而错误指不到真原因)。
    bool runAnalysisIn(double startS, double endS, bool clearManual)
    {
        const auto accepted = out.startAnalysis(0, startS, endS, clearManual);
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

    // [SL-263] 全轨全段的展平快照 —— 换响度档前后对拍用。
    // 挂在本机台上而不是另造一台:本文件 ~25 条用例已经在用它,再造第二台多轨 rig
    // 就是「同一语义两个落点」(#171 复审【重要】;判例见 analysis/HopMath.h 头注)。
    //
    // [SL-273] 原本只有一个 `panVolOf()`,把 pan 与 volDb 交错压进同一条 vector。
    // 那样只答得出「有没有哪一位变了」,答不出「变的是 pan 还是 vol」—— 而这两件事的
    // 性质完全不同(pan 只在指派掉到 level 2 时才动,vol 才是换档的必然信号,
    // 逐字见 tests/core/test_analysis_pipeline.cpp 里 [SL252/SL-273] 那条的头注)。
    // 拆成两个取值器,断言各断各的。
    std::vector<double> flatOf(bool wantPan)
    {
        std::vector<double> v;
        const auto crvs = out.crvsSnapshot();
        const auto& ver = crvs.versions[static_cast<std::size_t>(out.versionActive() - 1)];
        for (int ch = 1; ch <= kCount; ++ch)
        {
            for (const auto& seg : ver.tracks[static_cast<std::size_t>(ch - 1)].segments)
            {
                v.push_back(static_cast<double>(wantPan ? seg.pan : seg.volDb));
            }
        }
        return v;
    }
    std::vector<double> pansOf() { return flatOf(true); }
    std::vector<double> volsOf() { return flatOf(false); }

    // [SL-284] 最近一次落地分析里最坏的平衡回退级(§6.4;1..4,0 = 没跑过平衡)。
    //
    // 存在的理由是**让断言钉住前提,而不是它的推论**:本文件多条「换档/换素材后 pan 该不该动」
    // 的断言,成立前提都是「首趟 `solveBalance` 收敛」——z 只有在 level 2 的 `balHint->zHat`
    // 里才进指派代价。前提断不了时,红出来的是结论层现象(「pan 变了」),
    // 得从结论倒推回原因;有了它就能先断前提、再断推论,红在哪一层一目了然。
    int fallbackLevel() { return out.lastMaxFallbackLevel(); }

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
    REQUIRE(out.groupId() == static_cast<int>(kFromGroup)); // [SL-324] 读回断言
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
        REQUIRE(donor.groupId() == static_cast<int>(kToGroup)); // [SL-324] 读回断言
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
// #96:UICF 读取必须在 CFGS 早退点之前 —— CFGS 缺失/损坏时 master_chart_mode 也要按新
// 工程的 UICF 值生效,不能停在上一个工程的值(否则下次保存写陈旧值)。
// ---------------------------------------------------------------------------
TEST_CASE("HOST UICF:CFGS 缺失/损坏时 master_chart_mode 仍按 UICF 生效(不残留旧值)", "[host][uicf]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    ScvbOutputAudioProcessor out;

    // 场景 1:CFGS 缺失(只有 UICF=distribution)。旧值 trajectory 必须被覆盖成 distribution。
    out.setMasterChartMode("trajectory");
    REQUIRE(out.masterChartMode() == "trajectory");
    {
        scvb::state::StateChunks chunks;
        chunks.abi = scvb::state::kCurrentAbi;
        std::vector<std::uint8_t> uicf;
        REQUIRE(scvb::state::encodeUiConfig(scvb::state::kMasterChartModeDistribution, uicf));
        chunks.set(scvb::state::kFourccUiConfig, std::move(uicf));
        std::vector<std::uint8_t> blob;
        REQUIRE(scvb::state::encodeContainer(chunks, blob));
        out.setStateInformation(blob.data(), static_cast<int>(blob.size()));
        CHECK(out.masterChartMode() == "distribution"); // 修复前:停在 trajectory(陈旧值)
    }

    // 场景 2:CFGS 损坏(UICF=trajectory + 4 字节坏 CFGS)。UICF 仍须生效为 trajectory。
    out.setMasterChartMode("distribution");
    REQUIRE(out.masterChartMode() == "distribution");
    {
        scvb::state::StateChunks chunks;
        chunks.abi = scvb::state::kCurrentAbi;
        std::vector<std::uint8_t> uicf;
        REQUIRE(scvb::state::encodeUiConfig(scvb::state::kMasterChartModeTrajectory, uicf));
        chunks.set(scvb::state::kFourccUiConfig, std::move(uicf));
        chunks.set(scvb::state::kFourccCfgs, std::vector<std::uint8_t>(4, 0xFF)); // 不足 CFGS 头 → decode 失败
        std::vector<std::uint8_t> blob;
        REQUIRE(scvb::state::encodeContainer(chunks, blob));
        out.setStateInformation(blob.data(), static_cast<int>(blob.size()));
        CHECK(out.masterChartMode() == "trajectory"); // 修复前:停在 distribution(旧值)

        // 往返:#96 原始症状是「下次保存写陈旧值」—— 保存(getStateInformation)再载回
        // (setStateInformation)必须仍是 trajectory,证明保存时没有把上一个工程的值写进去。
        juce::MemoryBlock saved;
        out.getStateInformation(saved);
        out.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        CHECK(out.masterChartMode() == "trajectory");
    }
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

// ===========================================================================
// v5.3 A2 实测 / 裁决 [J85]「冻结语义修订」—— 冻结只存参数面 + 冻结位,曲线真身不动
//
// 用户现场:冻结某轨 pan 调了个值,**解冻后 pan 永远停在冻结时的那个值,重新分析也不动**
// —— 一次冻结即永久锁死。链条:
//   ① 冻结中调整走 setTrackManual,它把静态值**整表烘焙**进曲线(单段全时限常值,
//      t1 = 1<<40 哨兵,origin=user_edited);
//   ② 解冻后 DspArbiter 回读曲线(DspArbiter.cpp §2.3 引擎权威分支),读到的仍是那条常值段;
//   ③ 再分析按 ADR-008 不覆盖 origin=user 段 → 那条常值段永远不会被重算掉。
// 三条合起来 = 冻结这个「临时接管」把曲线永久改写了。
//
// 修法:冻结通道**不写曲线**,只写参数面(冻结维度的取值仲裁与打印器读的都是那里)。
// 手动接管(非冻结)与 clearManual 两条通道逐字不变 —— 用户主动「设为手动」仍然写曲线。
// ===========================================================================

namespace
{

// 段的逐字节相等(harness 里比「段表被没被改写」用;不比浮点近似,要的就是「一个字节都没动」)。
bool sameSegments(const std::vector<scvb::state::Segment>& a, const std::vector<scvb::state::Segment>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].t0 != b[i].t0 || a[i].t1 != b[i].t1 || a[i].pan != b[i].pan || a[i].volDb != b[i].volDb ||
            a[i].flags != b[i].flags)
            return false;
    }
    return true;
}

std::vector<scvb::state::Segment> segmentsOfTrack(ScvbOutputAudioProcessor& out, int ch)
{
    const auto crvs = out.crvsSnapshot();
    return crvs.versions[static_cast<std::size_t>(out.versionActive() - 1)]
        .tracks[static_cast<std::size_t>(ch - 1)]
        .segments;
}

// 冻结位一次性写入(gesture 三段式 = UI 的 toggleFreeze 同款)。
void setFreezeBits(ScvbOutputAudioProcessor& out, int ch, int bits)
{
    auto* p = out.getAPVTS().getParameter(scvb::params::freezeId(out.versionActive(), ch));
    REQUIRE(p != nullptr);
    p->beginChangeGesture();
    p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(bits)));
    p->endChangeGesture();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(120);
}

float paramValueOf(ScvbOutputAudioProcessor& out, const juce::String& id)
{
    auto* p = out.getAPVTS().getParameter(id);
    REQUIRE(p != nullptr);
    return p->convertFrom0to1(p->getValue());
}

// 该轨段表是否全是 auto 段(分析产物没被手动值烘焙掉)。
bool allAutoSegments(ScvbOutputAudioProcessor& out, int ch)
{
    const auto segs = segmentsOfTrack(out, ch);
    if (segs.empty())
        return false;
    for (const auto& s : segs)
        if (scvb::state::segmentOrigin(s.flags) != scvb::state::SegmentOrigin::Auto)
            return false;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// [J85] 核心断言:**冻结期间 crvsSnapshot 段表不被改写**。修的就是这一条。
// 顺带守住 J85 ⑤:手动接管(非冻结)那条通道原样写曲线,一个字都没改。
// ---------------------------------------------------------------------------
TEST_CASE("HOST J85:冻结中调整只落参数面,段表逐字节不变", "[host][t37][v53][J85]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const int v = r.out.versionActive();
    int replaced = 0;
    int replacedLocked = 0;

    // —— 通道②(不受本次改动影响):未冻结 = 用户主动接管手动 → 照旧写曲线真身 ——
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, -40.0f, replaced, replacedLocked));
    {
        const auto segs = segmentsOfTrack(r.out, kTestChannel);
        REQUIRE(segs.size() == 1);
        CHECK(segs.front().pan == -40.0f); // 接管通道确实写进了曲线
        CHECK(scvb::state::segmentOrigin(segs.front().flags) == scvb::state::SegmentOrigin::UserEdited);
    }

    // —— 通道①:冻结 pan 维后再调 —— 只许动参数面 ——
    setFreezeBits(r.out, kTestChannel, 1); // bit0 = pan
    const auto before = segmentsOfTrack(r.out, kTestChannel);

    replaced = -1;
    replacedLocked = -1;
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, 70.0f, replaced, replacedLocked));

    // ★ 核心:段表逐字节不变(修复前这里被整表换成 pan=70 的常值段)。
    CHECK(sameSegments(segmentsOfTrack(r.out, kTestChannel), before));
    // 没替换任何段就得如实回 0(UI 的一次性确认条按这两个数报计数)。
    CHECK(replaced == 0);
    CHECK(replacedLocked == 0);
    // 值落在冻结维度真正会被读的那个平面上。
    CHECK(paramValueOf(r.out, scvb::params::panId(v, kTestChannel)) == Catch::Approx(70.0f).margin(0.01));

    // 冻结是**逐维**的:pan 冻着,vol 没冻 → vol 仍走接管通道写曲线,且不冲掉 pan 那一维。
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/false, -9.0f, replaced, replacedLocked));
    {
        const auto segs = segmentsOfTrack(r.out, kTestChannel);
        REQUIRE(segs.size() == 1);
        CHECK(segs.front().volDb == -9.0f);
        CHECK(segs.front().pan == -40.0f); // 段表里的 pan 仍是接管时那个值(T37 D 族口径)
        CHECK(replaced == 1); // 这一路确实替换了 1 段,如实回报
    }
    CHECK(paramValueOf(r.out, scvb::params::volId(v, kTestChannel)) == Catch::Approx(-9.0f).margin(0.01));

    // 冻结通道不建段,值域钳制也得照 §1.16 的 value 域执行(不能靠建段函数顺手夹)。
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, 999.0f, replaced, replacedLocked));
    CHECK(paramValueOf(r.out, scvb::params::panId(v, kTestChannel)) == Catch::Approx(100.0f).margin(0.01));

    // 非有限值绝不许穿进参数面(#106 复审重要4):冻结维度上它就是 DspArbiter 的音频目标值,
    // NaN 进去等于整条总线出 NaN。桥面另有 badArg 拒绝,这里断的是 native 兜底那一层。
    const float nan = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, nan, replaced, replacedLocked));
    const float panAfterNan = paramValueOf(r.out, scvb::params::panId(v, kTestChannel));
    CHECK(std::isfinite(panAfterNan));
    CHECK(panAfterNan == Catch::Approx(0.0f).margin(0.01)); // 回中性值(pan 居中)
    r.runBlocks(20, 0.5f);
    const auto meters = r.out.meterSnapshot();
    CHECK(std::isfinite(meters.busPeak[0])); // 总线没被污染
    CHECK(std::isfinite(meters.busPeak[1]));
}

// ---------------------------------------------------------------------------
// [J85] ②preview + ③解冻即回曲线:引擎权威下冻结维度播冻结静态值(= 写入自动化前的
// preview),解冻后立刻回到**曲线采样值**并随时间运动(两个不同 tSec 取到两个不同的值 ——
// 常值段是取不出这个差的,这一条就是「不残留常值段」的听感证据)。
// ---------------------------------------------------------------------------
TEST_CASE("HOST J85:解冻回引擎曲线,输出随时间运动(不残留常值段)", "[host][t37][v53][J85]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setOutputEnabled(true); // 引擎权威(ARMED/PRINT)

    const int v = r.out.versionActive();
    int replaced = 0;
    int replacedLocked = 0;

    // 造一条**随时间变化**的曲线:先接管出一条全时限常值段,再切成两段、两段给相反的 pan。
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, -100.0f, replaced, replacedLocked));
    scvb::state::SegmentEditArgs split;
    split.op = scvb::state::SegmentEditOp::Split;
    split.segIdx = 0;
    split.tSamples = static_cast<std::int64_t>(5.0 * kSr);
    REQUIRE(r.out.editSegment(kTestChannel - 1, split) == scvb::state::SegmentEditResult::Ok);
    for (int i = 0; i < 2; ++i)
    {
        scvb::state::SegmentEditArgs sv;
        sv.op = scvb::state::SegmentEditOp::SetValues;
        sv.segIdx = i;
        sv.hasPan = true;
        sv.pan = (i == 0) ? -100.0f : 100.0f; // 前段硬左、后段硬右
        REQUIRE(r.out.editSegment(kTestChannel - 1, sv) == scvb::state::SegmentEditResult::Ok);
    }
    REQUIRE(segmentsOfTrack(r.out, kTestChannel).size() == 2);

    // 总线 L/R 比:>1 = 偏左,<1 = 偏右(本组只有这一条 Input)。
    const auto balanceAt = [&r](double tSec) {
        r.ph.timeSamples = static_cast<std::int64_t>(tSec * kSr);
        r.runBlocks(40, 0.5f); // 走过切换斜坡
        float l = 0.0f;
        float rr = 0.0f;
        for (int i = 0; i < 12; ++i)
        {
            r.runBlocks(4, 0.5f, /*pumpEveryN=*/2, /*pumpMs=*/4);
            const auto m = r.out.meterSnapshot();
            l = std::max(l, m.busPeak[0]);
            rr = std::max(rr, m.busPeak[1]);
        }
        REQUIRE(l > 0.0f);
        REQUIRE(rr > 0.0f);
        return l / rr;
    };

    // 前置:未冻结时曲线说了算 —— 前段偏左、后段偏右,两个 tSec 取到两个不同的值。
    const float leftEarly = balanceAt(2.0);
    const float rightLate = balanceAt(8.0);
    REQUIRE(leftEarly > 1.2f);
    REQUIRE(rightLate < 0.83f);

    // —— ② 冻结 pan 并调到居中:引擎权威下必须**立刻**播这个冻结静态值(preview 成立)——
    setFreezeBits(r.out, kTestChannel, 1);
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, 0.0f, replaced, replacedLocked));
    CHECK(paramValueOf(r.out, scvb::params::panId(v, kTestChannel)) == Catch::Approx(0.0f).margin(0.01));
    const float frozenEarly = balanceAt(2.0);
    const float frozenLate = balanceAt(8.0);
    CHECK(frozenEarly == Catch::Approx(1.0f).margin(0.15)); // 冻结值居中 → 两声道等量
    CHECK(frozenLate == Catch::Approx(1.0f).margin(0.15)); // 且**与时间无关**(冻结 = 平直线)

    // 居中值单独一条断不出「参数面被按什么值域读」:pan=0 归一化后是 0.5,若 DspArbiter 误把
    // 归一化值当工程值读,0.5 仍然≈居中,断言照样绿(#106 复审建议)。所以再取两个**非居中**
    // 的冻结值走完整 DSP 链,按 equal-power + dual-pan 解析值对拍:
    //   stereo 源、width=100 → 子声像 = clamp(pan∓100);两路子声像增益叠加后
    //   pan=+70 → L/R ≈ 0.8526/1.5225 ≈ 0.56;pan=−70 → 镜像 ≈ 1.79。
    // 若参数被当成归一化值读(+70 → 0.85),L/R 会是 ≈0.99 —— 与 0.56 差着一个数量级的判据。
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, 70.0f, replaced, replacedLocked));
    const float frozenRight = balanceAt(2.0);
    CHECK(frozenRight < 0.75f); // 明显偏右(解析值 0.56)
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, -70.0f, replaced, replacedLocked));
    const float frozenLeft = balanceAt(2.0);
    CHECK(frozenLeft > 1.33f); // 明显偏左(解析值 1.79)
    CHECK(frozenLeft > frozenRight * 2.0f); // 两个冻结静态值真的推动了声像,不是同一个数

    // 回到居中,免得下一段解冻断言把「冻结值恰好也偏左」当成曲线在动。
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, 0.0f, replaced, replacedLocked));

    // —— ③ 解冻:立刻回到曲线采样值,并随时间运动 ——
    setFreezeBits(r.out, kTestChannel, 0);
    const float thawedEarly = balanceAt(2.0);
    const float thawedLate = balanceAt(8.0);
    // 修复前:曲线已被冻结中那次调整整表烘焙成 pan=0 的常值段 → 这两个数都还是 1.0(居中),
    // 而且再也回不来 —— 这正是用户现场的「解冻后 pan 永远停在冻结时的值」。
    CHECK(thawedEarly > 1.2f);
    CHECK(thawedLate < 0.83f);
    CHECK(thawedEarly > thawedLate * 1.5f); // 两个 tSec 取到两个不同的值 = 曲线在动,不是常值

    // 段表也得是原来那两段:冻结那一轮一个字节都没往里写。
    const auto segs = segmentsOfTrack(r.out, kTestChannel);
    REQUIRE(segs.size() == 2);
    CHECK(segs[0].pan == -100.0f);
    CHECK(segs[1].pan == 100.0f);
}

// ---------------------------------------------------------------------------
// [J85] ④(用户现场后半句):解冻之后**再分析**必须能更新该轨曲线。
// 修复前:冻结中调整烘焙出的是 origin=user_edited 段,普通再分析按 ADR-008 不覆盖它 ——
// 于是「重新分析也不动」。修复后段表始终是 auto,再分析照常重算。
// ---------------------------------------------------------------------------
TEST_CASE("HOST J85:冻结→调整→解冻后,再分析可更新该轨曲线", "[host][t37][v53][J85]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));

    constexpr int kCh = 1;
    constexpr float kFrozenPan = -80.0f;
    REQUIRE(allAutoSegments(r.out, kCh)); // 前置:分析产物全是 auto 段

    // 冻结 pan 维,再在冻结态调一个显眼的值(= 用户现场的操作)。
    setFreezeBits(r.out, kCh, 1);
    int replaced = 0;
    int replacedLocked = 0;
    const auto beforeFreeze = segmentsOfTrack(r.out, kCh);
    REQUIRE(r.out.setTrackManual(kCh, /*isPan=*/true, kFrozenPan, replaced, replacedLocked));
    // 分析产物一个字节都不许被烘焙掉(修复前:整表变成单段 user_edited 常值)。
    CHECK(sameSegments(segmentsOfTrack(r.out, kCh), beforeFreeze));
    CHECK(allAutoSegments(r.out, kCh));

    // 解冻 → 再分析(**普通**分析,不带 clearManual —— 这正是修复前走不通的那条路)。
    setFreezeBits(r.out, kCh, 0);
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));

    CHECK(allAutoSegments(r.out, kCh)); // 仍是 auto:没有 user 段挡着重算
    const double after = r.firstPan(kCh);
    REQUIRE(std::isfinite(after));
    // 修复前:段表恒等于那个手动值(user 段被 ADR-008 保留下来),这里永远是 −80。
    CHECK(std::abs(after - static_cast<double>(kFrozenPan)) > 1.0);
}

// ===========================================================================
// v5.4 实测 SL-189 定谳组:「引擎驱动」档到底有没有无视宿主自动化。
//
// 用户现场:「选择跟随引擎模式后,所有声音依旧完全依照自动化来走,而不是根据冻结的数值
// 或者引擎的变化(在手动调整偏离引擎之后)」。
//
// 嫌疑面是「输出开关 → processBlock 的 engineAuthority 实参」这条线断了(恒 false / 被覆盖 /
// 根本没接)。本组不猜,把**同一份宿主自动化**灌进四种配置里对拍 —— 输入逐字节相同,只有
// 档位 / 冻结位不同,所以任何一条的结果都不可能是「凑巧总是读同一个面」:
//   ① 引擎驱动 + 未冻结 → 总线跟**曲线**(自动化写的是曲线的镜像值,跟错了立刻看得出来);
//   ①b 同上,但最后几块**不泵消息循环** —— 打印器的 25Hz Timer 一次都不触发,参数面上
//      就只剩自动化那个值(用例当场读回来对拍),总线仍须偏左 ⇒ 排除「其实是打印器把
//      曲线值又写回参数、DSP 读的还是参数面」这条替代解释;
//   ② 引擎驱动 + 冻结 pan → 总线跟**参数面**(= 宿主自动化。J78 优先级链:宿主自动化 >
//      冻结手动值;打印器对冻结车道只把参数自己的当前值重写成平直线,不采样曲线 —— #68);
//   ③ 跟随宿主(输出开关 OFF)+ 未冻结 → 总线跟**参数面**;
//   ④ 再切回引擎驱动 → 回到曲线(权威能来回切,不是一次性的)。
// ②③ 同时也是 ① 的反向验证:把 engineAuthority 实参改成恒 false,①①b④ 立刻红;改成恒 true,
// ③ 立刻红;把仲裁里的 freeze 分支删掉,② 立刻红。没有哪一个恒定实参能让四条同时绿。
//
// 判据一律取**总线 L/R 峰值比**(真实音频,不看中间量):>1 偏左,<1 偏右。
// 曲线取「全时限常值」而不是分段曲线:本组要断的是「读哪个面」,时间维已由 J85 那组
// 覆盖;不设时间分界就不必跳播放头 —— 大跨度跳播放头会让该轨短暂掉出注入集,量到的静音
// 与「权威读错了」在判据上长得一模一样。
// ===========================================================================
TEST_CASE("HOST SL-189:引擎驱动档下未冻结维度无视宿主自动化,冻结维度让位给它", "[host][t37][v54][SL189]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setOutputEnabled(true); // 引擎驱动(ARMED/PRINT)

    // waitUntilInjected 只保证 claim + 心跳新鲜;[J32] 的 200ms 注入延迟与首块环数据还要再等。
    // 每次改开关 / 冻结位都要在消息线程 runDispatchLoopUntil,那段时间一个音频块都不跑,该轨
    // 会短暂掉出注入集 —— 掉出去时总线是直通静音,和「权威读错了」在电平判据上无法区分。
    // 所以每次状态变更后都先等它真的回来。不等的话整组在**进程里第一个跑**时必红、跟在别的
    // 用例后面又必绿,假绿假红都不要。
    const auto settle = [&r] {
        for (int i = 0; i < 60 && r.out.meterSnapshot().busPeak[0] <= 0.0f; ++i)
        {
            r.runBlocks(4, 0.5f, /*pumpEveryN=*/2, /*pumpMs=*/8);
        }
        REQUIRE(r.out.meterSnapshot().busPeak[0] > 0.0f);
    };
    settle();

    const int v = r.out.versionActive();
    int replaced = 0;
    int replacedLocked = 0;

    // 曲线真身 = 全时限常值(手动接管通道,J85 ⑤:未冻结的 setTrackManual 照旧写曲线)。
    // 它同时把参数面也写成同一个值 —— 下面的「自动化」再把参数顶成镜像值,两个面就彻底岔开了。
    // 取 ∓70 而不是 ∓100:硬左会让右声道恒 0,L/R 比就成了除零,判据反而不成立。
    // stereo 源 + width=100 → 子声像 = clamp(pan∓100),解析比:pan=−70 → L/R≈1.79,+70 → ≈0.56。
    constexpr float kCurvePan = -70.0f; // 曲线:偏左
    constexpr float kAutomatedPan = 70.0f; // 宿主已录自动化:偏右,与曲线镜像
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, kCurvePan, replaced, replacedLocked));
    REQUIRE(segmentsOfTrack(r.out, kTestChannel).size() == 1);
    CHECK(segmentsOfTrack(r.out, kTestChannel).front().pan == kCurvePan);

    auto* panParam = r.out.getAPVTS().getParameter(scvb::params::panId(v, kTestChannel));
    REQUIRE(panParam != nullptr);

    // 模拟宿主自动化 Read 档把已录值顶进参数。包 gesture 的理由同 setTrackManual 头注:
    // 裸写在 Read 档宿主上会被当场顶回去。
    const auto writeAutomation = [panParam, kAutomatedPan] {
        panParam->beginChangeGesture();
        panParam->setValueNotifyingHost(panParam->convertTo0to1(kAutomatedPan));
        panParam->endChangeGesture();
    };

    // 量一段总线 L/R 峰值比。pumpMessages=false 的那档**全程不泵消息循环** —— 打印器的 25Hz
    // Timer 在那几十块里一次都不会触发,参数面上就只剩自动化写进去的值。
    const auto balanceUnderAutomation = [&r, &writeAutomation](bool pumpMessages) {
        writeAutomation();
        if (pumpMessages)
        {
            r.runBlocks(40, 0.5f); // 走过 30ms 切换斜坡 + 10ms 常规斜坡
        }
        else
        {
            r.runBlocks(16, 0.5f, /*pumpEveryN=*/0);
        }

        float l = 0.0f;
        float rr = 0.0f;
        for (int i = 0; i < 12; ++i)
        {
            if (pumpMessages)
            {
                writeAutomation();
                r.runBlocks(4, 0.5f, /*pumpEveryN=*/2, /*pumpMs=*/4);
            }
            else
            {
                r.runBlocks(4, 0.5f, /*pumpEveryN=*/0);
            }
            const auto m = r.out.meterSnapshot();
            l = std::max(l, m.busPeak[0]);
            rr = std::max(rr, m.busPeak[1]);
        }
        REQUIRE(l > 0.0f);
        REQUIRE(rr > 0.0f);
        return l / rr;
    };

    // —— ① 引擎驱动 + 未冻结:自动化说偏右,曲线说偏左,总线必须偏左 ——
    const float engine = balanceUnderAutomation(/*pumpMessages=*/true);
    CHECK(engine > 1.33f); // 用户现场的现象若成立,这一条就是红的(解析值 1.79)

    // —— ①b 排除「打印器把曲线值写回了参数、DSP 读的还是参数面」——
    const float engineNoPump = balanceUnderAutomation(/*pumpMessages=*/false);
    // 这几十块里没有任何 [M] 侧代码跑过,参数面就是自动化那个值 —— 当场读回来钉住。
    CHECK(paramValueOf(r.out, scvb::params::panId(v, kTestChannel)) == Catch::Approx(kAutomatedPan).margin(0.01));
    CHECK(engineNoPump > 1.33f); // 参数偏右、总线偏左 ⇒ 未冻结维度读的确实不是参数面

    // —— ② 引擎驱动 + 冻结 pan:同一份自动化,现在必须**跟得上**(J78 优先级链)——
    setFreezeBits(r.out, kTestChannel, 1); // bit0 = pan
    settle();
    const float frozen = balanceUnderAutomation(/*pumpMessages=*/true);
    CHECK(frozen < 0.75f); // 曲线仍是偏左,总线却偏右 ⇒ 冻结维度读的是参数面(解析值 0.56)
    CHECK(frozen < engine * 0.5f); // 输入一样、只有 freeze 位不同 ⇒ 双源分叉是真的在分叉

    // —— ③ 跟随宿主(输出开关 OFF)+ 未冻结:回到参数面权威 ——
    setFreezeBits(r.out, kTestChannel, 0);
    r.out.setOutputEnabled(false);
    settle();
    const float follow = balanceUnderAutomation(/*pumpMessages=*/true);
    CHECK(follow < 0.75f); // 曲线仍是偏左,总线偏右 ⇒ 开关真的切换了权威
    CHECK(follow < engine * 0.5f);

    // —— ④ 再切回引擎驱动:同一实例、同一份自动化,权威必须能切回去 ——
    r.out.setOutputEnabled(true);
    settle();
    const float backToEngine = balanceUnderAutomation(/*pumpMessages=*/true);
    CHECK(backToEngine > 1.33f);
    CHECK(backToEngine > follow * 2.0f);
}

// ===========================================================================
// [SL-187] setTrackManual 的参数面写入必须带**自写标记**(§3.5 层 2)。
//
// `v{v}_t{t:02d}_pan/_vol` 是打印车道参数,HostEchoListener 挂在 APVTS 上。不置自写位,
// 我们自己这次写入会走到层 1b —— 在引擎权威(ARMED/PRINT)下被当成「宿主自动化正在回写」
// 记进 host echo,`hostEchoActive()` 随即真 600ms,`emitParams` 带 `hostEcho:true`,
// UI 把用户**刚拖完**的那个旋钮灰显掉,提示语还写着「宿主在驱动这条车道」。
// 冻结通道与手动接管通道每一次拖拽都命中(用户 v5.5 必遇)。
//
// 三段断言缺一不可:两条通道各自不得被记 + **对照组**(真正的宿主写入仍然要被记)——
// 少了对照组,把整个 host echo shield 拆掉也能让前两条绿。
// ===========================================================================
TEST_CASE("HOST SL-187:setTrackManual 的自写不得被记成 hostEcho", "[host][t37][v55][SL187]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setOutputEnabled(true); // 引擎权威:层 1b 才是活的(Follow 档不记 host echo)
    r.runBlocks(40, 0.5f);
    Rig::pumpMessages(120); // 等 timerCallback 把 mode 推给打印器

    auto& printer = r.out.getPrinter();
    const int v = r.out.versionActive();
    int replaced = 0;
    int replacedLocked = 0;

    // 前置:此刻没有 host echo(否则下面断言的「没被记」证明不了什么)。
    REQUIRE_FALSE(printer.hostEchoActive());
    const int baseline = printer.hostEchoCount();

    // —— 通道①:冻结中调整 ——
    setFreezeBits(r.out, kTestChannel, 1);
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, 55.0f, replaced, replacedLocked));
    CHECK(printer.hostEchoCount() == baseline); // ★ 自写不进 host echo 计数
    CHECK_FALSE(printer.hostEchoActive()); // ★ UI 灰显的直接判据
    // 值确实落到参数面了(别让「没记 hostEcho」是因为压根没写成)。
    CHECK(paramValueOf(r.out, scvb::params::panId(v, kTestChannel)) == Catch::Approx(55.0f).margin(0.01));

    // —— 通道②:未冻结的手动接管 ——
    setFreezeBits(r.out, kTestChannel, 0);
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/false, -6.0f, replaced, replacedLocked));
    CHECK(printer.hostEchoCount() == baseline);
    CHECK_FALSE(printer.hostEchoActive());
    CHECK(paramValueOf(r.out, scvb::params::volId(v, kTestChannel)) == Catch::Approx(-6.0f).margin(0.01));

    // —— 对照组:**真正的**宿主写入(不带自写标记)必须照常被记 ——
    // 这一条保证上面两条不是「shield 整个坏了」换来的绿。
    auto* volP = r.out.getAPVTS().getParameter(scvb::params::volId(v, kTestChannel));
    REQUIRE(volP != nullptr);
    volP->setValueNotifyingHost(volP->convertTo0to1(-12.0f));
    Rig::pumpMessages(20);
    CHECK(printer.hostEchoCount() > baseline);
    CHECK(printer.hostEchoActive());
}

// ===========================================================================
// [SL-188] PR #111 复审【重要】:J85 的「解冻即回引擎分析曲线」在**跨维度**这条路上不成立。
//
// 手动接管通道走 `track.segments.assign(1, seg)` —— **整表**换成一段常值,另一维的值只从
// 首段继承。于是「分析出 N 段 → 冻 pan 调值 → 拖 vol(未冻结,接管)」之后,pan 那一维的
// 分析曲线连带被拍平,解冻 pan 读到的仍是常值段,普通再分析按 ADR-008 不覆盖 user 段。
// 症状与 v5.3 A2 现场逐字相同,只是入口从 pan 换成了 vol。
//
// 这是 04 §1.5 方案 A(每轨一张段表、两维同段)的**既有设计**,确认条正文也确实写着
// 「替换该轨的**全部**分段结果」,J85 ⑤ 又明令手动接管通道逐字不动 —— 所以本用例**不主张
// 它该被保住**,而是把「当前行为就是会被压平」钉死:哪天有人改了方案 A,这条会红,
// 提醒他同步改掉 PR #106 描述里登记的那条已知残留(而不是默默把语义换掉)。
//
// 已有的 `HOST J85:冻结中调整只落参数面` 那条跨维断言是从**已经被压平**的单段表出发的,
// 恰好盖住了这个洞 —— 本用例从**多段 auto 表**出发,补上那一半。
// ===========================================================================
TEST_CASE("HOST SL-188:多段 auto 表上拖未冻结 vol 会连带压平 pan(方案 A 现行语义)", "[host][t37][v55][SL188]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // 采一段足够长的有声/静音交替素材,再**分两个范围各分析一次** —— 这是本 harness 里
    // 拿到「多段 auto 表」的可靠办法(单次全范围分析通常只产出一段;R6 用例同款手法)。
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

    const auto runAnalysis = [&r](double a, double b, bool clearManual) {
        REQUIRE(r.out.startAnalysis(0, a, b, clearManual).ok);
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

    const double half = coveredS * 0.5;
    runAnalysis(half, coveredS, /*clearManual=*/false);
    runAnalysis(0.0, half, /*clearManual=*/false);

    const auto before = segmentsOfTrack(r.out, kTestChannel);
    REQUIRE(before.size() >= 2); // 前置:确实是**多段** auto 曲线,不是单段
    REQUIRE(allAutoSegments(r.out, kTestChannel));

    // 冻 pan 并调值 —— [J85] 这一步不碰曲线(已由 J85 用例守住,这里只做前置)。
    setFreezeBits(r.out, kTestChannel, 1);
    int replaced = 0;
    int replacedLocked = 0;
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, -80.0f, replaced, replacedLocked));
    REQUIRE(sameSegments(segmentsOfTrack(r.out, kTestChannel), before));

    // 拖 **vol**(该维未冻结 → 走手动接管通道)。
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/false, -6.0f, replaced, replacedLocked));

    // ★ 钉死现行语义:整表被换成单段常值,**pan 那一维的分析曲线一并没了**。
    const auto after = segmentsOfTrack(r.out, kTestChannel);
    CHECK(after.size() == 1);
    CHECK(scvb::state::segmentOrigin(after.front().flags) == scvb::state::SegmentOrigin::UserEdited);
    CHECK(after.front().volDb == -6.0f);
    CHECK(replaced == static_cast<int>(before.size())); // 如实回报替换掉了多少段
    // pan 维:继承自**首段**,而不是原来那条随时间变化的曲线。
    CHECK(after.front().pan == before.front().pan);

    // 于是解冻 pan 之后,曲线上再也取不出时间变化 —— 这正是「入口换成 vol 的同一个环」。
    setFreezeBits(r.out, kTestChannel, 0);
    CHECK(segmentsOfTrack(r.out, kTestChannel).size() == 1);

    // 出口仍在:重新识别(含手动段)能把它清掉,段表回到 auto(与 HOST P0-3 同一条链路)。
    runAnalysis(0.0, coveredS, /*clearManual=*/true);
    CHECK(allAutoSegments(r.out, kTestChannel));
}

// ===========================================================================
// [SL-202] v5.5 实测 P1 回归:冻结一轨 pan → 调整 → 解冻,pan 落**正中**,而且**其他轨也有落正中的**;
// 选中该轨「重新分析选区」无效,只有全量分析才恢复。
//
// 「正中」这个词是本卡最硬的线索:它不是指派算法会给出的值,而是 `DspArbiter` 在
// **曲线指针为空**时的默认(`DspArbiter.cpp` 引擎权威分支:`src.curve != nullptr ? panAt(t) : 0.0f`)。
// 而曲线指针只在一个地方被置空 —— `rebuildAllCurves()` 里 `src.empty() → setCurve(v, t, nullptr)`。
// 所以「pan 正中」等价于「**该轨段表被清空了**」,判据不该看音频(三轨混音的 L/R 比读不准),
// 要直接看 `authority().activeCurves()[t]` 是不是 nullptr —— 那是这条因果链的正下方。
//
// 下面按用户的真实路径逐步复现,每一步都对**全部三轨**的段表与曲线指针拍照对拍,
// 谁在哪一步把段表清掉就当场落到那一步的断言上。
// ===========================================================================
namespace
{
// 该轨当前的活动曲线指针(空 = DspArbiter 会给正中 0)。
const scvb::CurveEvaluator* activeCurveOf(ScvbOutputAudioProcessor& out, int ch)
{
    return out.authority().activeCurves()[static_cast<std::size_t>(ch - 1)];
}

// 全部三轨的「段数 + 曲线在不在」快照,便于一次性对拍。
struct TrackShot
{
    std::size_t segCount = 0;
    bool hasCurve = false;
};

std::array<TrackShot, 3> shotOf(ScvbOutputAudioProcessor& out)
{
    std::array<TrackShot, 3> s{};
    for (int ch = 1; ch <= 3; ++ch)
    {
        s[static_cast<std::size_t>(ch - 1)].segCount = segmentsOfTrack(out, ch).size();
        s[static_cast<std::size_t>(ch - 1)].hasCurve = activeCurveOf(out, ch) != nullptr;
    }
    return s;
}
} // namespace

TEST_CASE("HOST SL-202:冻结→调整→解冻,本轨与他轨曲线都不得被清空", "[host][t37][v55][SL202]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));

    constexpr int kCh = 1;

    // 前置:三轨都有分析产物,曲线都在。
    const auto before = shotOf(r.out);
    for (int ch = 1; ch <= 3; ++ch)
    {
        INFO("track " << ch);
        REQUIRE(before[static_cast<std::size_t>(ch - 1)].segCount > 0);
        REQUIRE(before[static_cast<std::size_t>(ch - 1)].hasCurve);
    }
    const auto beforeSegs = segmentsOfTrack(r.out, kCh);
    const double analysedPan = r.firstPan(kCh);
    REQUIRE(std::isfinite(analysedPan));

    // ① 冻结该轨 pan。冻结本身**不得**触碰任何轨的段表(嫌疑 b:冻结按钮链路里有清段动作)。
    setFreezeBits(r.out, kCh, 1);
    MonoMultiRig::pump(120);
    CHECK(sameSegments(segmentsOfTrack(r.out, kCh), beforeSegs));
    for (int ch = 1; ch <= 3; ++ch)
    {
        INFO("freeze step, track " << ch);
        CHECK(activeCurveOf(r.out, ch) != nullptr);
    }

    // ② 冻结态调整(J85:只落参数面)。同样不得触碰段表。
    int replaced = 0;
    int replacedLocked = 0;
    REQUIRE(r.out.setTrackManual(kCh, /*isPan=*/true, -80.0f, replaced, replacedLocked));
    MonoMultiRig::pump(120);
    CHECK(sameSegments(segmentsOfTrack(r.out, kCh), beforeSegs));

    // ③ 解冻。曲线本来就没被动过,这一步不需要任何恢复动作(J85 ③)。
    setFreezeBits(r.out, kCh, 0);
    MonoMultiRig::pump(200);

    // ★ 核心断言组:解冻后 —— 本轨曲线仍在、段表逐字节不变、pan 回到分析值(不是正中 0)。
    {
        const auto* curve = activeCurveOf(r.out, kCh);
        REQUIRE(curve != nullptr); // ← 空曲线正是「正中」的来源
        CHECK(sameSegments(segmentsOfTrack(r.out, kCh), beforeSegs));
        CHECK(r.firstPan(kCh) == Catch::Approx(analysedPan));
        // 曲线在该段中点采样应等于分析值,而不是 0(正中)。
        const double sr = r.out.sampleRate();
        REQUIRE(sr > 0.0);
        const double midSec =
            (static_cast<double>(beforeSegs.front().t0) + static_cast<double>(beforeSegs.front().t1)) / (2.0 * sr);
        CHECK(curve->panAt(midSec) == Catch::Approx(analysedPan).margin(0.5));
    }

    // ★ 他轨(嫌疑 ②:多轨曲线被清)。
    const auto after = shotOf(r.out);
    for (int ch = 2; ch <= 3; ++ch)
    {
        INFO("after unfreeze, other track " << ch);
        CHECK(after[static_cast<std::size_t>(ch - 1)].hasCurve);
        CHECK(after[static_cast<std::size_t>(ch - 1)].segCount == before[static_cast<std::size_t>(ch - 1)].segCount);
    }
}

// ---------------------------------------------------------------------------
// [SL-202] 嫌疑 a:#112 让「单轨重新识别」真的发得出去之后,
// analyze({tracksMask}, {clearManual:true}) 会不会波及**他轨**或**范围外**的段?
// 这是解冻提示条那颗按钮的真实调用(tab-tracks.js:1230),用户解冻后最可能点的就是它。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL-202:单轨 clearManual 重新识别不得动他轨段表", "[host][t37][v55][SL202]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));

    constexpr int kCh = 1;
    const auto beforeCh2 = segmentsOfTrack(r.out, 2);
    const auto beforeCh3 = segmentsOfTrack(r.out, 3);
    REQUIRE(beforeCh2.size() > 0);
    REQUIRE(beforeCh3.size() > 0);

    // 冻结 + 调整 + 解冻,再走一次「单轨重新识别」(mask 只含该轨,clearManual=true)。
    setFreezeBits(r.out, kCh, 1);
    int replaced = 0;
    int replacedLocked = 0;
    REQUIRE(r.out.setTrackManual(kCh, /*isPan=*/true, -80.0f, replaced, replacedLocked));
    setFreezeBits(r.out, kCh, 0);
    MonoMultiRig::pump(120);

    const auto accepted = r.out.startAnalysis(static_cast<std::uint16_t>(1u << (kCh - 1)), 0.0, coveredS,
                                              /*clearManual=*/true);
    REQUIRE(accepted.ok); // #112 之后单轨重新识别必须受理得了
    for (int waited = 0; waited < 20000; waited += 50)
    {
        MonoMultiRig::pump(50);
        if (!r.out.analysisRunning() && !r.out.runtime().analysisRunning)
        {
            break;
        }
    }
    REQUIRE_FALSE(r.out.analysisRunning());

    // ★ 他轨:段表与曲线一个字节都不许动(局部重分析只对「轨 × 区间」失效,ADR-008 / §4.4)。
    CHECK(sameSegments(segmentsOfTrack(r.out, 2), beforeCh2));
    CHECK(sameSegments(segmentsOfTrack(r.out, 3), beforeCh3));
    CHECK(activeCurveOf(r.out, 2) != nullptr);
    CHECK(activeCurveOf(r.out, 3) != nullptr);
    // 本轨:重算出了 auto 段,曲线在,不是空表。
    CHECK(activeCurveOf(r.out, kCh) != nullptr);
    CHECK(allAutoSegments(r.out, kCh));
}

// ---------------------------------------------------------------------------
// [SL-202] 嫌疑 c:「重新分析选区」在真机上无效,而全量分析能恢复。
// 这里用**子范围**跑一次单轨分析,断言它确实改到了该轨在该范围内的段。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL-202:选区重分析对该轨生效(不是只有全量才行)", "[host][t37][v55][SL202]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 2.0);
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));

    constexpr int kCh = 1;
    const auto before = segmentsOfTrack(r.out, kCh);
    REQUIRE_FALSE(before.empty());

    // 选区 = 后半段;单轨 mask。
    const double half = coveredS * 0.5;
    const auto accepted =
        r.out.startAnalysis(static_cast<std::uint16_t>(1u << (kCh - 1)), half, coveredS, /*clearManual=*/false);
    REQUIRE(accepted.ok); // 受理得了(#112 修的是「不给范围」那一路,这里显式给了范围)
    CHECK(accepted.tracks >= 1); // ★ 影响面里必须有这条轨,否则「选了等于没选」
    for (int waited = 0; waited < 20000; waited += 50)
    {
        MonoMultiRig::pump(50);
        if (!r.out.analysisRunning() && !r.out.runtime().analysisRunning)
        {
            break;
        }
    }
    REQUIRE_FALSE(r.out.analysisRunning());

    // ★ 该轨曲线仍在(选区重分析不得把整轨清空 —— 那正是「变正中」的形态)。
    CHECK(activeCurveOf(r.out, kCh) != nullptr);
    CHECK_FALSE(segmentsOfTrack(r.out, kCh).empty());
    // 范围外的段仍在(局部重分析不碰其他区间)。
    const std::int64_t halfSample = static_cast<std::int64_t>(half * r.out.sampleRate());
    bool keptOutside = false;
    for (const auto& sg : before)
    {
        if (sg.t1 <= halfSample)
        {
            keptOutside = true;
            break;
        }
    }
    if (keptOutside)
    {
        bool stillThere = false;
        for (const auto& sg : segmentsOfTrack(r.out, kCh))
        {
            if (sg.t1 <= halfSample)
            {
                stillThere = true;
                break;
            }
        }
        CHECK(stillThere);
    }
}

// ---------------------------------------------------------------------------
// [SL-202] 唯一能**一次清空全部轨**的代码路径:`setStateInformation` 在「没有 CRVS chunk 或
// 解码失败」时会把 `crvsData_` 整个重置成默认(两个版本 × 15 轨全空段表)。
// 段表一空 → `rebuildAllCurves` 给 `setCurve(v,t,nullptr)` → `DspArbiter` 引擎权威分支拿不到曲线
// → 恒 0 = **正中**。用户报的「其他轨也有落正中的」只有这条路径解释得通(finishAnalysis 结构上
// 清不空:`src.empty()` 直接 continue,非空时 dst = src + kept)。
//
// 所以这条链必须有回归守卫:存档往返一旦丢 CRVS,现象与 SL-202 逐字相同。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL-202:state 往返必须保住段表(丢 CRVS = 全部轨落正中)", "[host][t37][v55][SL202]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));

    // 造一个「有冻结位 + 有手动接管段 + 有分析段」的真实工程态,再往返。
    setFreezeBits(r.out, 1, 1);
    int replaced = 0;
    int replacedLocked = 0;
    REQUIRE(r.out.setTrackManual(1, /*isPan=*/true, -80.0f, replaced, replacedLocked));
    REQUIRE(r.out.setTrackManual(2, /*isPan=*/false, -6.0f, replaced, replacedLocked)); // 未冻结 → 接管通道写曲线

    std::array<std::vector<scvb::state::Segment>, 3> before;
    for (int ch = 1; ch <= 3; ++ch)
    {
        before[static_cast<std::size_t>(ch - 1)] = segmentsOfTrack(r.out, ch);
        REQUIRE_FALSE(before[static_cast<std::size_t>(ch - 1)].empty());
    }

    // 宿主保存 → 重新灌回(工程重开 / 预设切换 / 插件重建都走这一对)。
    juce::MemoryBlock blob;
    r.out.getStateInformation(blob);
    REQUIRE(blob.getSize() > 0);
    r.out.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    MonoMultiRig::pump(200);

    // ★ 三轨段表逐字节还在,曲线指针也重建了(不是 nullptr = 不落正中)。
    for (int ch = 1; ch <= 3; ++ch)
    {
        INFO("round-trip track " << ch);
        CHECK(sameSegments(segmentsOfTrack(r.out, ch), before[static_cast<std::size_t>(ch - 1)]));
        CHECK(activeCurveOf(r.out, ch) != nullptr);
    }
}

// ===========================================================================
// [SL-217] `setStateInformation` 的 CRVS 分支**不得静默清空段表**(§7.3 不得静默丢数据)。
//
// 原实现在「无 CRVS chunk」或「CRVS 解码失败」时把 crvsData_ 整个重置成默认 ——
// 两个版本 × 15 轨的段表一次全清,悄无声息。后果不是「少了点东西」:段表一空 →
// rebuildAllCurves 给 setCurve(nullptr) → DspArbiter 引擎权威分支恒 0 → **每条轨的声像都跳正中**,
// 而 UI 上毫无提示。这正是 SL-202 现场「多轨落正中、只有全量分析才恢复」的形态,
// 也是这条链上唯一能**一次清空所有轨**的代码路径。
//
// 宿主给出不含 CRVS 的 blob 是有正当场合的(轨道预设/参数预设只带 PRMS、别的实例的部分状态、
// 旧版本或被裁剪过的工程数据)——「没有信息」不该被读成「删除全部」。
// ===========================================================================
namespace
{
// 从一份完整 blob 里**剥掉** CRVS chunk,模拟「宿主给了一份不含段表的状态」。
// 直接在容器层解开→删→重编,不手搓字节(与生产同一套编解码,免得测的是我自己的假容器)。
std::vector<std::uint8_t> blobWithoutCrvs(const juce::MemoryBlock& src)
{
    scvb::state::StateChunks chunks;
    const auto res = scvb::state::loadState(static_cast<const std::uint8_t*>(src.getData()), src.getSize(), chunks);
    REQUIRE(res.status == scvb::state::StateLoadStatus::Ok);
    REQUIRE(chunks.find(scvb::state::kFourccCrvs) != nullptr); // 前置:原 blob 确实带 CRVS
    chunks.chunks.erase(
        std::remove_if(chunks.chunks.begin(), chunks.chunks.end(),
                       [](const scvb::state::Chunk& c) { return c.fourcc == scvb::state::kFourccCrvs; }),
        chunks.chunks.end());
    std::vector<std::uint8_t> out;
    REQUIRE(scvb::state::encodeContainer(chunks, out));
    return out;
}
} // namespace

TEST_CASE("HOST SL-217:缺 CRVS chunk 的 state 不得清空段表", "[host][t37][v55][SL217]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));

    std::array<std::vector<scvb::state::Segment>, 3> before;
    for (int ch = 1; ch <= 3; ++ch)
    {
        before[static_cast<std::size_t>(ch - 1)] = segmentsOfTrack(r.out, ch);
        REQUIRE_FALSE(before[static_cast<std::size_t>(ch - 1)].empty());
        REQUIRE(activeCurveOf(r.out, ch) != nullptr);
    }

    juce::MemoryBlock full;
    r.out.getStateInformation(full);
    REQUIRE(full.getSize() > 0);
    const auto stripped = blobWithoutCrvs(full);

    // ★ 灌一份**不含 CRVS** 的 state:段表必须原样保留,曲线必须还在。
    r.out.setStateInformation(stripped.data(), static_cast<int>(stripped.size()));
    MonoMultiRig::pump(200);

    for (int ch = 1; ch <= 3; ++ch)
    {
        INFO("track " << ch);
        CHECK(sameSegments(segmentsOfTrack(r.out, ch), before[static_cast<std::size_t>(ch - 1)]));
        CHECK(activeCurveOf(r.out, ch) != nullptr); // ← 空表会让它变 nullptr = 正中
    }
    // 诊断位:如实标记「这一轮没恢复段真身」(供上桥告警/现场判别)。
    CHECK(r.out.hasCrvsNotRestored());
}

TEST_CASE("HOST SL-217:CRVS 解码失败同样不得清空段表", "[host][t37][v55][SL217]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));

    const auto before = segmentsOfTrack(r.out, 1);
    REQUIRE_FALSE(before.empty());

    // 把 CRVS chunk 的 payload 换成一段解不开的垃圾(容器本身仍然合法 —— 走的是
    // 「chunk 在、decodeCrvs 失败」那一支,而不是 Corrupt 整体拒载那一支)。
    juce::MemoryBlock full;
    r.out.getStateInformation(full);
    scvb::state::StateChunks chunks;
    REQUIRE(scvb::state::loadState(static_cast<const std::uint8_t*>(full.getData()), full.getSize(), chunks).status ==
            scvb::state::StateLoadStatus::Ok);
    for (auto& c : chunks.chunks)
    {
        if (c.fourcc == scvb::state::kFourccCrvs)
        {
            c.payload.assign(16, std::uint8_t{0xEE}); // 解不开的载荷
        }
    }
    std::vector<std::uint8_t> broken;
    REQUIRE(scvb::state::encodeContainer(chunks, broken));

    r.out.setStateInformation(broken.data(), static_cast<int>(broken.size()));
    MonoMultiRig::pump(200);

    // ★ 段表保留、曲线还在、诊断位置起。
    CHECK(sameSegments(segmentsOfTrack(r.out, 1), before));
    CHECK(activeCurveOf(r.out, 1) != nullptr);
    CHECK(r.out.hasCrvsNotRestored());
}

TEST_CASE("HOST SL-217:正常往返仍照常恢复段表且不置诊断位", "[host][t37][v55][SL217]")
{
    // 对照组:别让「保留」把正常加载也一并跳过了 —— 完整 blob 必须真的把段表换成 blob 里那一份。
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));

    juce::MemoryBlock full;
    r.out.getStateInformation(full); // 这一份里带着分析结果
    const auto analysed = segmentsOfTrack(r.out, 1);
    REQUIRE_FALSE(analysed.empty());

    // 把段表改掉(手动接管通道 → 单段常值),再灌回那份 blob:必须被换回分析结果。
    int replaced = 0;
    int replacedLocked = 0;
    REQUIRE(r.out.setTrackManual(1, /*isPan=*/true, -70.0f, replaced, replacedLocked));
    REQUIRE_FALSE(sameSegments(segmentsOfTrack(r.out, 1), analysed));

    r.out.setStateInformation(full.getData(), static_cast<int>(full.getSize()));
    MonoMultiRig::pump(200);

    CHECK(sameSegments(segmentsOfTrack(r.out, 1), analysed)); // ★ 正常路径照旧生效
    CHECK_FALSE(r.out.hasCrvsNotRestored()); // 诊断位只在真没恢复时才亮
}

// ---------------------------------------------------------------------------
// [SL-217 / SL-202 追查] 宿主在**冻结 gesture 中途**拍 state 快照,CRVS 是否完整?
//
// 统筹的嫌疑:Cubase 这类宿主可能在参数 gesture 期间回写 preset 快照,若那一刻 CRVS
// 不完整,随后回灌就正好命中「缺 CRVS → 清空」那条路。
// 结论(本例钉死):**不成立**。冻结只写 APVTS 参数,压根不碰 crvsData_;
// getStateInformation 持 lifecycleMutex_ 整体编码段真身,gesture 开着与否与它无关。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL-217:冻结 gesture 中途取 state,CRVS 仍完整", "[host][t37][v55][SL217]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));
    const auto before = segmentsOfTrack(r.out, 1);
    REQUIRE_FALSE(before.empty());

    // 打开一个**未闭合**的 freeze gesture,并在中途取快照(模拟宿主此刻拍 preset)。
    auto* frz = r.out.getAPVTS().getParameter(scvb::params::freezeId(r.out.versionActive(), 1));
    REQUIRE(frz != nullptr);
    frz->beginChangeGesture();
    frz->setValueNotifyingHost(frz->convertTo0to1(1.0f));

    juce::MemoryBlock mid;
    r.out.getStateInformation(mid); // ← gesture 仍开着
    frz->endChangeGesture();
    MonoMultiRig::pump(120);

    // ★ 这份快照里 CRVS chunk 在、且解得开、且段表就是分析结果。
    scvb::state::StateChunks chunks;
    REQUIRE(scvb::state::loadState(static_cast<const std::uint8_t*>(mid.getData()), mid.getSize(), chunks).status ==
            scvb::state::StateLoadStatus::Ok);
    const scvb::state::Chunk* crvs = chunks.find(scvb::state::kFourccCrvs);
    REQUIRE(crvs != nullptr); // ← gesture 中途取的快照**不缺** CRVS
    scvb::state::CrvsData decoded;
    REQUIRE(scvb::state::decodeCrvs(crvs->payload.data(), crvs->payload.size(), decoded));
    CHECK(
        sameSegments(decoded.versions[static_cast<std::size_t>(r.out.versionActive() - 1)].tracks[0].segments, before));

    // 回灌这份快照:段表照常恢复,诊断位不亮。
    r.out.setStateInformation(mid.getData(), static_cast<int>(mid.getSize()));
    MonoMultiRig::pump(200);
    CHECK(sameSegments(segmentsOfTrack(r.out, 1), before));
    CHECK_FALSE(r.out.hasCrvsNotRestored());
}

// ===========================================================================
// [J87] 局部重采集布防的引擎侧实装(04 §4.2;用户 2026-08-27 三裁)。
//
// #114 定谳查出的洞:`recaptureArm` 的五个 runtime 字段在引擎侧**零消费方** —— 布防只把
// badge 点亮,采集通路一个字节都没改,`autoStop` 更是连实现方都没有。而 USER_GUIDE 已经把
// 行为写出去了。本组按三条裁定逐条钉死,判据全部取**引擎侧可观测量**(采集开关真值 /
// coverage / waveformOf 内容),不看 UI:
//   ① 布防即自动打开 01 采集;
//   ② 布防期间采集收窄到「工作选区 × 选中轨掩码」,区外与未选轨的既有特征逐字节不动;
//   ③ 勾了「播放结束自动停止」时,播放头越过选区右边界 → 自动撤防 + 把采集恢复成布防前的值。
// ===========================================================================

namespace
{

// waveformOf 的内容对拍(= 卡里说的「哈希比对」;直接比数组,失败时看得见是哪一列变了)。
// 比 min/max/covered 三列:它们是 FrameStore 里 kw/peak 的直接投影,特征被改写必然反映过来。
bool sameWaveform(const ScvbOutputAudioProcessor::WaveformTile& a, const ScvbOutputAudioProcessor::WaveformTile& b)
{
    if (a.minDb.size() != b.minDb.size() || a.maxDb.size() != b.maxDb.size() || a.covered.size() != b.covered.size())
        return false;
    for (std::size_t i = 0; i < a.minDb.size(); ++i)
    {
        if (a.minDb[i] != b.minDb[i] || a.maxDb[i] != b.maxDb[i] || a.covered[i] != b.covered[i])
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("HOST J87①③:布防自动开采集,撤防恢复布防前的原值", "[host][t37][j87]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    constexpr std::uint16_t kMask = 1u << (kTestChannel - 1);

    // —— ① 布防前采集是关的 → 布防即自动打开(裁定①)——
    r.out.setCaptureEnabled(false);
    REQUIRE_FALSE(r.out.captureEnabled());
    r.out.armRecapture(kMask, 1.0, 2.0, /*autoStop=*/false);
    CHECK(r.out.runtime().recaptureArmed);
    CHECK(r.out.captureEnabled()); // ← 修复前:布防只亮 badge,这里恒 false

    // 中途改选区 = 再布防一次(04 §4.2 ②)。**不能**因此把「布防前采集是关的」这笔账冲掉 ——
    // 冲掉了撤防就再也关不回去。这一条是 armRecapture 里那个 if (!recaptureArmed) 的用例。
    r.out.armRecapture(kMask, 1.5, 3.0, /*autoStop=*/false);
    CHECK(r.out.runtime().recaptureStartS == 1.5);
    CHECK(r.out.captureEnabled());

    // —— ③ 撤防 → 采集恢复成布防前的值(这里是 OFF)——
    r.out.disarmRecapture();
    CHECK_FALSE(r.out.runtime().recaptureArmed);
    CHECK_FALSE(r.out.captureEnabled());

    // —— ③ 反向:布防前采集**本来就开着** → 撤防后必须保持开 ——
    r.out.setCaptureEnabled(true);
    r.out.armRecapture(kMask, 1.0, 2.0, /*autoStop=*/false);
    CHECK(r.out.captureEnabled());
    r.out.disarmRecapture();
    CHECK_FALSE(r.out.runtime().recaptureArmed);
    CHECK(r.out.captureEnabled()); // ← 一律关掉的写法在这里红:用户自己开的采集被我们私自关了

    // 撤防是幂等的:UI 侧「开关关掉」会重复发 recaptureArm(0,0,0),第二发不许再动采集。
    r.out.disarmRecapture();
    CHECK(r.out.captureEnabled());

    // —— 用户在布防期间**自己**拧过采集开关 = 这把闸他接管了,撤防不许再替他动 ——
    // 裁定③恢复的是「布防前的原值」,不是「盖掉用户中途的决定」。
    r.out.setCaptureEnabled(false);
    r.out.armRecapture(kMask, 1.0, 2.0, /*autoStop=*/false);
    REQUIRE(r.out.captureEnabled()); // 我们替他开的
    r.out.setCaptureEnabled(false); // 用户手动关掉
    r.out.setCaptureEnabled(true); // 又自己开回来 —— 从这一刻起是他的决定
    r.out.disarmRecapture();
    CHECK(r.out.captureEnabled()); // ← 不清 autoEnabled 位的写法在这里红:用户开的采集被关掉
}

TEST_CASE("HOST J87②:布防期采集收窄到「选区 × 选中轨」,区外与未选轨逐字节不动", "[host][t37][j87]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // —— 第一遍:全局采集,把 3 条轨的 [0, ~6.4s] 全采一遍 ——
    const double coveredS = r.capture();
    REQUIRE(coveredS > 4.0);

    constexpr int kArmedCh = 2; // 只重采第 2 轨
    constexpr double kSelStartS = 2.0;
    constexpr double kSelEndS = 3.0;
    constexpr int kCols = 128;

    // 三轨各自的「选区内 / 选区外」两段基线内容。
    const auto beforeInside = r.out.waveformOf(kArmedCh, kSelStartS, kSelEndS, kCols);
    const auto beforeOutside = r.out.waveformOf(kArmedCh, kSelEndS, coveredS, kCols);
    const auto beforeCh1 = r.out.waveformOf(1, 0.0, coveredS, kCols);
    const auto beforeCh3 = r.out.waveformOf(3, 0.0, coveredS, kCols);
    REQUIRE(beforeInside.covered.size() == static_cast<std::size_t>(kCols));

    // —— 第二遍:只对「第 2 轨 × [2s,3s)」布防,回到 0 用**明显不同的幅度**重播全程 ——
    // 布防会自动打开采集(裁定①),所以这里不再手动开;这也顺带证明了那条链是通的。
    r.out.setCaptureEnabled(false);
    r.ph.timeSamples = 0;
    r.out.armRecapture(static_cast<std::uint16_t>(1u << (kArmedCh - 1)), kSelStartS, kSelEndS,
                       /*autoStop=*/false);
    REQUIRE(r.out.captureEnabled());
    MonoMultiRig::pump(400);
    for (int burst = 0; burst < 6; ++burst)
    {
        r.runBlocks(60, 0.12f); // 第一遍是 0.5f —— 差一个数量级,特征必然不同
        r.runBlocks(40, 0.0f);
    }
    MonoMultiRig::pump(400);

    // ★ 选区内:被新素材覆盖(否则这一组测的是「什么都没发生」)。
    const auto afterInside = r.out.waveformOf(kArmedCh, kSelStartS, kSelEndS, kCols);
    CHECK_FALSE(sameWaveform(beforeInside, afterInside));

    // ★ 选区外(同一条轨):逐字节不动 —— 04 §4.1「绝不触碰其他区间已有结果」。
    //   修复前:布防不改门控,整条时间线按 global.range(follow=全域)重录,这里必红。
    const auto afterOutside = r.out.waveformOf(kArmedCh, kSelEndS, coveredS, kCols);
    CHECK(sameWaveform(beforeOutside, afterOutside));

    // ★ 未选轨:整轨逐字节不动 —— 布防范围的轨维硬约束(04 §4.2「未选轨的写入被拉取侧丢弃」)。
    //   这一条同时守住 pullTick 的「排空而不是跳过」:若未选轨只是 continue,撤防后那段积压
    //   会被补拉进来,ch1/ch3 就变了。
    CHECK(sameWaveform(beforeCh1, r.out.waveformOf(1, 0.0, coveredS, kCols)));
    CHECK(sameWaveform(beforeCh3, r.out.waveformOf(3, 0.0, coveredS, kCols)));

    // 撤防后再跑一段:未选轨的积压不许在这时候补拉进来(排空的后半段证据)。
    r.out.disarmRecapture();
    MonoMultiRig::pump(400);
    r.runBlocks(40, 0.0f);
    MonoMultiRig::pump(400);
    CHECK(sameWaveform(beforeCh1, r.out.waveformOf(1, 0.0, coveredS, kCols)));
    CHECK(sameWaveform(beforeCh3, r.out.waveformOf(3, 0.0, coveredS, kCols)));
}

TEST_CASE("HOST J87③:越过选区右边界自动撤防 + 恢复采集原值", "[host][t37][j87]")
{
    constexpr std::uint16_t kMask = 1u << (kTestChannel - 1);
    constexpr double kSelStartS = 1.0;
    constexpr double kSelEndS = 2.0;

    // 把播放头从选区左侧一路推过右边界;回报是否还布防。
    const auto playAcross = [](Rig& r) {
        r.ph.timeSamples = static_cast<std::int64_t>(0.5 * kSr);
        for (int i = 0; i < 40 && r.out.runtime().recaptureArmed; ++i)
        {
            r.runBlocks(8, 0.5f, /*pumpEveryN=*/2, /*pumpMs=*/12);
        }
        return r.out.runtime().recaptureArmed;
    };

    SECTION("勾了自动停 + 布防前采集是关的 → 越界后撤防并把采集关回去")
    {
        Rig r;
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());
        r.out.setCaptureEnabled(false);
        r.out.armRecapture(kMask, kSelStartS, kSelEndS, /*autoStop=*/true);
        REQUIRE(r.out.captureEnabled());

        CHECK_FALSE(playAcross(r)); // ← 修复前 autoStop 无实现方,这里恒 true
        CHECK_FALSE(r.out.captureEnabled());
    }

    SECTION("勾了自动停 + 布防前采集本来就开着 → 撤防但采集保持开")
    {
        Rig r;
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());
        r.out.setCaptureEnabled(true);
        r.out.armRecapture(kMask, kSelStartS, kSelEndS, /*autoStop=*/true);

        CHECK_FALSE(playAcross(r));
        CHECK(r.out.captureEnabled()); // 裁定③的括号:恢复原值,不是一律关掉
    }

    SECTION("没勾自动停 → 越界不撤防(门控自己挡住区外记账就够了)")
    {
        Rig r;
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());
        r.out.setCaptureEnabled(false);
        r.out.armRecapture(kMask, kSelStartS, kSelEndS, /*autoStop=*/false);

        CHECK(playAcross(r)); // 仍布防 —— 这一条是上面两节的反向验证
        CHECK(r.out.captureEnabled());
    }

    SECTION("布防时播放头已在选区右侧 → 不当场自撤防(边沿判定,不是电平判定)")
    {
        Rig r;
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());
        r.out.setCaptureEnabled(false);
        r.ph.timeSamples = static_cast<std::int64_t>(5.0 * kSr); // 已经在选区右边
        r.out.armRecapture(kMask, kSelStartS, kSelEndS, /*autoStop=*/true);
        r.runBlocks(20, 0.5f, /*pumpEveryN=*/2, /*pumpMs=*/12);
        // 电平判定("此刻在右侧就撤防")在这里会当场撤防,用户一按布防就自己关掉。
        CHECK(r.out.runtime().recaptureArmed);
    }
}

// ---------------------------------------------------------------------------
// [J87] 排空 vs 跳过(pullTick 里那一支)。
//
// 上一组用「回到 0 重播」构造重采集,那条路上宿主会 startRun 重锚,读游标被 run 切换守卫
// 顺手重置了 —— 于是「跳过」与「排空」两种写法在那里表现一致,测不出差别。真正暴露它的是
// **不回卷、原地往前播**的那条路:布防期间未选轨的读游标停在原地,撤防后 pullIncremental
// 从旧游标接着拉,把布防期写进环里的那一段补进 FrameStore —— 未选轨凭空多出一段覆盖,
// 而按 04 §4.2 那一段本该被丢弃。环容量 2^17 hop ≈ 21.8 分钟,这里两秒的积压离回绕很远,
// 所以「补拉」是必然发生而不是碰运气。
// ---------------------------------------------------------------------------
TEST_CASE("HOST J87②b:布防期未选轨的积压不得在撤防后被补拉", "[host][t37][j87]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    REQUIRE(r.capture() > 4.0); // 先把 [0, t0) 采满,顺带把采集开关留在 ON

    // 布防前采集就是 ON —— 撤防后它必须保持 ON(裁定③),补拉才有机会发生。
    REQUIRE(r.out.captureEnabled());
    const double t0 = static_cast<double>(r.ph.timeSamples) / kSr;
    const double selS = t0 + 0.5;
    const double selE = t0 + 1.5;

    // **不回卷**,原地往前布防第 2 轨的一小段。
    r.out.armRecapture(/*tracksMask=*/1u << 1, selS, selE, /*autoStop=*/false);
    MonoMultiRig::pump(200);
    r.runBlocks(240, 0.3f); // ≈2.56s,整段跨过选区
    MonoMultiRig::pump(400);

    r.out.disarmRecapture();
    MonoMultiRig::pump(400);
    r.runBlocks(60, 0.3f); // 撤防后继续播:这一段是允许被记的
    MonoMultiRig::pump(400);

    // ★ 未选轨在**布防那一段**里必须一片空白。
    //   「跳过而不是排空」的写法在这里红:撤防后那一拍会把 [t0, t0+2.5] 整段补拉进来。
    CHECK(r.out.coverageOf(1, t0 + 0.05, t0 + 2.4).coveredS < 0.05);
    CHECK(r.out.coverageOf(3, t0 + 0.05, t0 + 2.4).coveredS < 0.05);

    // 对照组:选中轨在选区内确实采到了(否则上面两条是「什么都没发生」的假绿)。
    CHECK(r.out.coverageOf(2, selS, selE).coveredS > 0.3);
    // 而选中轨在选区**外**同样空白 —— 时间维门控(与上一组的内容对拍互为佐证)。
    CHECK(r.out.coverageOf(2, selE + 0.1, t0 + 2.4).coveredS < 0.05);
}

// ---------------------------------------------------------------------------
// [SL-210] 同组同 bus 的第二个 Output —— 只读观察者,不是第二个主实例。
//
// 真机症状:同一条总线上挂第二个 Output,总线输出塌成单声道、采集与分析跟着单声道,
// 第一个 Output 正常,删掉第二个立刻恢复,全程无任何提示。
//
// 根因在 claim 层而不在 processBlock:两个实例同在一个 DAW 进程里、pid 相同,而
// Registry::claimOutput 的「同 pid 重认领」分支只比 pid,于是第二个实例也拿到 kActive。
// 两个主实例都替换总线,后挂的那个是全新实例、15 轨 pan 全在默认居中位,它的求和 L==R
// 盖掉了前一个按用户 pan 值铺开的声像 —— 用户听到的「单声道」就是这么来的。
//
// 本用例必须在 harness 里做:两个 processor 真实同进程,pid 天然相同,in-process 单测
// 里要手工传同一个 pid 才复现得出来(tests/core/test_output_session.cpp 的同名用例)。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL-210:同 bus 第二个 Output 进只读观察,不抢主实例", "[host][v56][SL210]")
{
    Rig r; // r.out = 第一个 Output(主实例)
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // 主实例:非只读(观察者横幅不该显示),且确实在注入。
    REQUIRE_FALSE(r.out.connSnapshot().readOnly);

    // 同一条总线上再挂一个 Output:同组、同进程(pid 相同)。
    auto second = std::make_unique<ScvbOutputAudioProcessor>();
    second->setGroupId(kTestGroup);
    REQUIRE(second->groupId() == kTestGroup); // [SL-324] 读回断言
    second->setPlayHead(&r.ph);
    second->prepareToPlay(kSr, kBlock);
    Rig::pumpMessages(300);

    // ① 第二个实例进只读观察 —— readOnly 正是桥面 §1.1 驱动观察者横幅的那一位,
    //    修复前它恒 false,横幅逻辑在也永远不显示(用户报的「无任何提示」)。
    REQUIRE(second->connSnapshot().readOnly);
    // ② 主实例不受影响,没被挤下去。
    REQUIRE_FALSE(r.out.connSnapshot().readOnly);

    // ③ 观察者的音频严格逐样本直通:按位相等,一个样本都不许改。
    //    (这是「零写入」纪律的音频面,对齐 Monitor 三铁律。)
    juce::AudioBuffer<float> probe{2, kBlock};
    Rig::fillSine(probe, 0.31f, r.ph.timeSamples);
    juce::AudioBuffer<float> expected;
    expected.makeCopyOf(probe);

    juce::MidiBuffer midi;
    for (int b = 0; b < 8; ++b)
    {
        second->processBlock(probe, midi);
        for (int c = 0; c < 2; ++c)
        {
            const float* got = probe.getReadPointer(c);
            const float* want = expected.getReadPointer(c);
            for (int i = 0; i < kBlock; ++i)
            {
                // 按位相等,不是 Approx:观察者做的任何增益/淡入淡出都会在这里露出来。
                REQUIRE(got[i] == want[i]);
            }
        }
    }

    // ④ 观察者绝不是单声道源:上面按位相等已经保证 L≠R 的输入原样出去。
    //    再直接断言一次「左右没有被折叠成同一路」,把用户症状钉死在用例里。
    juce::AudioBuffer<float> stereo{2, kBlock};
    for (int i = 0; i < kBlock; ++i)
    {
        stereo.getWritePointer(0)[i] = 0.5f; // L
        stereo.getWritePointer(1)[i] = -0.5f; // R(与 L 反相 —— 折叠成单声道就会归零)
    }
    second->processBlock(stereo, midi);
    for (int i = 0; i < kBlock; ++i)
    {
        REQUIRE(stereo.getReadPointer(0)[i] == 0.5f);
        REQUIRE(stereo.getReadPointer(1)[i] == -0.5f);
    }

    // ⑤ 反向验证:删掉观察者,主实例仍是主实例(slot 没被同 pid 的观察者释放掉)。
    second->releaseResources();
    second.reset();
    Rig::pumpMessages(300);
    REQUIRE_FALSE(r.out.connSnapshot().readOnly);
    r.runBlocks(4, 0.25f, 1, 20);
    REQUIRE(r.injected());
}

// ---------------------------------------------------------------------------
// [SL-215] 会话 GUID:非全零、随工程往返稳定。
// 修复前桥面快照里是一串写死的全零字面量,设置页恒显示
// 「session 00000000-0000-0000-0000-000000000000」;GUID 是 sidecar 文件名隔离的根基,
// 全零等于所有工程共用一个身份。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL-215:会话 GUID 非全零且随 state 往返稳定", "[host][v56][SL215]")
{
    constexpr const char* kAllZero = "00000000-0000-0000-0000-000000000000";

    ScvbOutputAudioProcessor a;
    const juce::String guidA = a.sessionGuid();

    // ① 非全零、形状合法(36 字符 dashed UUID)。
    REQUIRE(guidA.isNotEmpty());
    REQUIRE(guidA != juce::String(kAllZero));
    REQUIRE(scvb::state::isValidSessionGuid(guidA.toStdString()));

    // ② 同一实例内稳定(不是每次读都重生成)。
    REQUIRE(a.sessionGuid() == guidA);

    // ③ 往返:存进 state → 新实例加载 → 拿到同一串。
    juce::MemoryBlock blob;
    a.getStateInformation(blob);
    REQUIRE(blob.getSize() > 0);

    ScvbOutputAudioProcessor b;
    const juce::String guidBFresh = b.sessionGuid();
    REQUIRE(guidBFresh != guidA); // 前置:全新实例本来是另一串,否则往返断言没有意义

    b.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    REQUIRE(b.sessionGuid() == guidA); // 沿用工程里的那一个

    // ④ 二次往返不漂移。
    juce::MemoryBlock blob2;
    b.getStateInformation(blob2);
    ScvbOutputAudioProcessor c;
    c.setStateInformation(blob2.getData(), static_cast<int>(blob2.getSize()));
    REQUIRE(c.sessionGuid() == guidA);

    // ⑤ 反向验证:工程里没存过 GUID(老工程)→ 保留自己生成的那一个,不退回全零。
    ScvbOutputAudioProcessor d;
    const juce::String guidD = d.sessionGuid();
    d.setStateInformation(nullptr, 0); // 空 state = 从未落过盘
    REQUIRE(d.sessionGuid() == guidD);
    REQUIRE(d.sessionGuid() != juce::String(kAllZero));

    // ⑥ 未采集过 → 特征 0 字节且落点是「内嵌」(设置页据此显示「内嵌于工程(0.0 MB)」,
    //    而不是修复前那句凭空的「已保存为外部文件」)。
    REQUIRE(a.featureBytes() == 0);
    REQUIRE_FALSE(a.featuresInSidecar());
}

// ---------------------------------------------------------------------------
// [SL-208] 缩放档位记忆:「保持」过的档位,换实例(= 重开窗 / 重开工程)必须还在。
//
// 用户报「重开不记住上次档位」。这条链路有两段,本用例把两段都钉住:
//   ① 系统级全局默认(UiDefaultsStore)—— 新工程 / 新实例的起始档;
//   ② 工程 state(CFGS.uiScale)—— 同一工程再打开时的档。
// 两段都通了,「记不住」就只可能来自**没走「保持」确认**:Ctrl+- 是 WebView2 自带的浏览器
// 缩放,插件既收不到也存不了(web/output/app.js 里没有任何 Ctrl +/- 处理,档位只经页脚下拉
// 与设置页 select 进 setUiScale,再由「保持」走 commitUiScale 落盘)。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL-208:「保持」过的缩放档位换实例仍在(全局默认 + 工程 state)", "[host][v56][SL208]")
{
    namespace ud = scvb::uidefaults;

    // 独享临时落盘目录,结束即删(含异常路径),不碰开发机上的真实全局默认。
    struct TempStore
    {
        juce::File dir = juce::File::createTempFile("scvb-sl208")
                             .getSiblingFile("scvb-sl208-" + juce::String(juce::Random::getSystemRandom().nextInt64()));
        TempStore()
        {
            dir.createDirectory();
            ud::setStorageDirForTesting(dir);
        }
        ~TempStore()
        {
            ud::setStorageDirForTesting({});
            dir.deleteRecursively();
        }
    } store;

    REQUIRE(ud::uiScalePercent() == 0); // 干净起点:从未「保持」过

    // ① 全局默认:一个实例「保持」125%,下一个**全新实例**构造时就该起在 125%。
    {
        ScvbOutputAudioProcessor a;
        REQUIRE(a.uiScalePercent() == 100); // 未设置过 → 出厂 100

        // persistUiScaleAsDefault() 的两件事(editor 侧走 commitUiScale 时的等价动作)。
        a.bridgeSetUiScalePercent(125);
        ud::setUiScalePercent(125);
        REQUIRE(a.uiScalePercent() == 125);
    }
    {
        ScvbOutputAudioProcessor b; // 换实例 = 重开窗 / 新工程
        REQUIRE(b.uiScalePercent() == 125);
    }

    // ② 工程 state:同一工程存下的档位,加载时压过全局默认(工程 > 全局默认)。
    juce::MemoryBlock blob;
    {
        ScvbOutputAudioProcessor c;
        c.bridgeSetUiScalePercent(200);
        c.getStateInformation(blob);
        REQUIRE(c.uiScalePercent() == 200);
    }
    {
        ScvbOutputAudioProcessor d;
        REQUIRE(d.uiScalePercent() == 125); // 先取到全局默认
        d.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
        REQUIRE(d.uiScalePercent() == 200); // 工程里的档位赢
    }

    // ③ 反向验证:越界档位既不落盘也不被读成「已设置」,不会把用户档位冲掉。
    ud::setUiScalePercent(5000);
    REQUIRE(ud::uiScalePercent() == 125);
}

// ---------------------------------------------------------------------------
// [J87] PR #124 评审补的两道守卫。
//
// ① 工程恢复(setStateInformation)必须复位布防运行时态。布防是纯运行时态(04 §4.2 ③
//    「工作选区不落 state」),此前不复位无害 —— 那几个字段没有消费方;J87 之后它们直接决定
//    记账门控与采集开关,残留的陈旧布防会让新工程一开就按上一个工程的选区收窄采集,
//    甚至在越界那一拍把刚从 state 恢复出来的 capture_enabled 关掉。
// ② `tracksMask` 的保留位:§9.2 是 u16、bit15 保留 0,processor 侧存 `& 0x7FFF`。桥面若拿
//    **未掩码**的值判 noTracks,`0x8000` 一类只点了保留位的入参会通过校验、落进 processor 时
//    变成 0 —— 而 0 在记账侧是「不限轨」,轨维门控整个退化。
// ---------------------------------------------------------------------------
TEST_CASE("HOST J87:工程恢复复位布防运行时态", "[host][t37][j87]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    constexpr std::uint16_t kMask = 1u << (kTestChannel - 1);

    // 先存一份工程态。**注意**:[J91] 之后采集态根本不落盘(`getStateInformation` 恒写 0),
    // 所以这里开不开采集对存下来的 blob 毫无影响 —— 留着这一行只是为了让本用例的
    // 前后对照与改判前逐字可比(见末尾那条 CHECK_FALSE 的说明)。
    r.out.setCaptureEnabled(true);
    juce::MemoryBlock blob;
    r.out.getStateInformation(blob);
    REQUIRE(blob.getSize() > 0);

    // —— 把**十个字段逐个弄脏** ——
    // 这一段不是铺垫,是本用例的一半:只断「载回之后是零」而不先把它们弄脏,那些本来就是
    // 零的字段(autoEnabled / prevPlayhead / range 三个)不复位也照样绿 —— 十条断言里五条
    // 是空的,护栏只有一半真的存在(PR #140 评审重要)。所以每一项都先弄脏、再 REQUIRE
    // 确认脏成功,最后才谈复位。
    r.out.setCaptureEnabled(false); // 让下面那发布防去「替用户开」,autoEnabled 才会是 true
    r.out.runtime().rangeMode = 2; // manual 档
    r.out.runtime().rangeStartS = 3.0;
    r.out.runtime().rangeEndS = 9.0;
    // endS 取得远一点:autoStop=true 时播放头越过右边界会自动撤防,那样就不是「载回 state
    // 才复位」而是被自动撤防顺手清了 —— 会把本用例测的东西掉包。
    r.out.armRecapture(kMask, 1.0, 60.0, /*autoStop=*/true);
    // 播几块让 25Hz tick 把 recapturePrevPlayheadS 填成非负(它只在 tick 里被写)。
    r.ph.timeSamples = static_cast<std::int64_t>(1.2 * kSr);
    r.runBlocks(24, 0.5f, /*pumpEveryN=*/2, /*pumpMs=*/12);

    // 前置:十个字段确实都不是复位值(弄脏成功),否则下面的断言测不出任何东西。
    REQUIRE(r.out.runtime().recaptureArmed);
    REQUIRE(r.out.runtime().recaptureTracksMask == kMask);
    REQUIRE(r.out.runtime().recaptureStartS == 1.0);
    REQUIRE(r.out.runtime().recaptureEndS == 60.0);
    REQUIRE(r.out.runtime().recaptureAutoStop);
    REQUIRE(r.out.runtime().recaptureAutoEnabledCapture);
    REQUIRE(r.out.runtime().recapturePrevPlayheadS >= 0.0);
    REQUIRE(r.out.runtime().rangeMode == 2);
    REQUIRE(r.out.runtime().rangeStartS == 3.0);
    REQUIRE(r.out.runtime().rangeEndS == 9.0);

    // 载回工程 = 换工程 / 撤销加载:两组字段必须整块复位。
    r.out.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));

    // 十个字段逐个回读(评审建议②:别只断一半,剩下的靠「行为面间接覆盖」说不清)。
    CHECK_FALSE(r.out.runtime().recaptureArmed); // ← 不复位的写法在这里红
    CHECK(r.out.runtime().recaptureTracksMask == 0);
    CHECK(r.out.runtime().recaptureStartS == 0.0);
    CHECK(r.out.runtime().recaptureEndS == 0.0);
    CHECK_FALSE(r.out.runtime().recaptureAutoStop);
    CHECK_FALSE(r.out.runtime().recaptureAutoEnabledCapture);
    CHECK(r.out.runtime().recapturePrevPlayheadS < 0.0);
    // range 三字段同属「不随工程走」,一并归零到 §1.8 默认档(统筹指定①)。
    CHECK(r.out.runtime().rangeMode == 0);
    CHECK(r.out.runtime().rangeStartS == 0.0);
    CHECK(r.out.runtime().rangeEndS == 0.0);
    // ⚠ [J91] **本行的期望在 2026-08-30 反转过,这不是回归**。
    //
    // 改判前:采集态随工程走,所以载回「采集 ON」的工程后这里断的是 `CHECK(captureEnabled())`
    // ——「state 里的采集态照常恢复,没被布防残留顺手关掉」。
    // 改判后([J91],用户批准):采集是**录制动作**不是工程设置,`getStateInformation` 恒写 0、
    // `setStateInformation` 一律忽略,重开工程一律为**关**。于是这里必然是 false。
    //
    // 本用例真正守的东西**一个字没变** —— 「陈旧布防不得在载入后顺手改采集开关」。那条不变量
    // 现在由紧随其后的姊妹用例(「陈旧布防不得把恢复出来的采集关掉」)以**加载后手动开采集**
    // 的方式继续守着;这里只剩「加载后恒为关」这一条 J91 的口径断言。
    // 后来人若看到这行觉得别扭想改回 CHECK():先读 docs/contract-changes/20260830-j91-capture-not-persisted.md。
    CHECK_FALSE(r.out.captureEnabled());
}

// 上一条断的是「字段有没有复位」;这一条断的是**那个复位到底挡住了什么**。
// 构造必须同时满足三件事,少一件就测不出来(PR #131 评审重要-2①指出原构造三件全缺):
//   ① 布防**前**采集是 OFF —— 这样 `recaptureAutoEnabledCapture` 才会是 true,
//      也才存在「撤防会去关采集」这个动作;
//   ② 载入的 state 里采集是 **ON** —— 这样「被误关」才有可观测的落差;
//   ③ 播放头必须**真的越过**旧的 endS —— 边沿判定要求上一拍在左、这一拍在右。
// 不复位的实现在这里:载回 state 之后 armed 仍是 true、autoEnabled 仍是 true,
// 播过旧右边界那一拍就把刚恢复出来的 capture_enabled 关掉了。
TEST_CASE("HOST J87:工程恢复后,陈旧布防不得把恢复出来的采集关掉", "[host][t37][j87]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    constexpr std::uint16_t kMask = 1u << (kTestChannel - 1);
    constexpr double kOldEndS = 2.0;

    // 先做一份工程存档(② 的来源)。[J91] 之后采集态不落盘,这份 blob 里没有采集态。
    juce::MemoryBlock blob;
    r.out.getStateInformation(blob);
    REQUIRE(blob.getSize() > 0);

    // ① 布防前采集 OFF → 布防替用户开(autoEnabled = true),且勾了自动停。
    r.out.setCaptureEnabled(false);
    r.out.armRecapture(kMask, 1.0, kOldEndS, /*autoStop=*/true);
    REQUIRE(r.out.captureEnabled());
    REQUIRE(r.out.runtime().recaptureAutoEnabledCapture);

    // ② 载回工程。复位这一步若缺席,布防与 autoEnabled 位都会活下来。
    //
    // ⚠ [J91] **本段的构造手法换过一次,被测不变量一个字没变**。改判前采集态随工程走,
    // 所以这里靠「载回一份采集 ON 的工程」来制造「载入后采集是开的」这个前提;改判后采集
    // 恒不落盘、加载恒为关,那条路没了 —— 改用**用户自己手动开采集**制造同一个落差。
    r.out.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    REQUIRE_FALSE(r.out.captureEnabled()); // [J91] 加载后恒为关

    r.out.setCaptureEnabled(true); // 用户自己把采集打开
    REQUIRE(r.out.captureEnabled());
    // `setCaptureEnabled` 会把 autoEnabled 清零(§1.23「用户接管」语义)。而本用例要制造的
    // 敌对前提恰恰是「陈旧布防仍以为采集是它开的」—— 在**不复位**的实现里,那一位本来就从
    // ① 一路活到现在。所以手动按回去:正确实现里它无害(armed 已复位,越界那一拍什么都不会
    // 触发),不复位的实现里它与那边的真实状态一致。**少了这一行,不复位的实现也不会去关
    // 采集,本用例就测不出任何东西了**(反向验证实跑确认过:注掉七字段复位 ⇒ 本条转红)。
    r.out.runtime().recaptureAutoEnabledCapture = true;

    // ③ 从旧选区左侧一路播过旧的 endS —— 每拍推进 512/48000≈10.7ms,这里给足 4 秒。
    r.ph.timeSamples = static_cast<std::int64_t>(0.5 * kSr);
    for (int i = 0; i < 24; ++i)
    {
        r.runBlocks(16, 0.5f, /*pumpEveryN=*/4, /*pumpMs=*/10);
    }
    REQUIRE(static_cast<double>(r.ph.timeSamples) / kSr > kOldEndS + 1.0); // 前置:真的越过去了

    // ★ 采集必须还开着 —— 不复位的实现在这里红:陈旧布防在越界那一拍把它关了。
    CHECK(r.out.captureEnabled());
    CHECK_FALSE(r.out.runtime().recaptureArmed);
}

// [J87] 排空必须**不受 kMaxBurstHops 限速**(PR #131 修 ②;评审重要-2② 指出原用例测不出来)。
//
// J87②b 的积压只有 ≈2.56s ≈ 256 hop,恰好等于一拍的 burst 上限 —— 空 gate 与 drainOnly
// 在那里表现一致,两种写法都能过。要把二者分开,得让**单拍积压远大于 256 hop**:
// 这里用「跑一大段块但一次都不泵消息循环」制造离线快渲染的形状(不泵 = 25Hz tick 不触发 =
// 读方不拉),写头一口气推出 ~32 秒 ≈ 3200 hop。随后只给几拍的泵:
//   · 受限速的空 gate —— 每拍最多追 256 hop,几拍下来还剩两千多 hop 没排掉,
//     撤防后被补拉进来,未选轨凭空多出十几秒覆盖;
//   · drainOnly —— 一拍就把游标推到写头,撤防后干干净净。
TEST_CASE("HOST J87:排空不受每拍 burst 上限约束(单拍积压远超 256 hop)", "[host][t37][j87]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    REQUIRE(r.capture() > 4.0); // 采集留在 ON:撤防后要靠它把积压补进来才看得见差别
    REQUIRE(r.out.captureEnabled());
    const double t0 = static_cast<double>(r.ph.timeSamples) / kSr;

    // 只对第 2 轨布防一小段;第 1/3 轨在整个布防期都该是空白。
    r.out.armRecapture(/*tracksMask=*/1u << 1, t0 + 0.5, t0 + 1.5, /*autoStop=*/false);
    MonoMultiRig::pump(200);

    // 关键:**一次都不泵**。3000 块 × 512 / 48000 ≈ 32s ≈ 3200 hop 全压成一拍的积压。
    r.runBlocks(3000, 0.3f, /*pumpEveryN=*/0);
    const double t1 = static_cast<double>(r.ph.timeSamples) / kSr;
    REQUIRE(t1 - t0 > 25.0); // 前置:积压确实远超 256 hop(2.56s)

    // 只给几拍 —— 受限速的写法在这几拍里追不完 3200 hop。
    MonoMultiRig::pump(200);
    r.out.disarmRecapture();
    MonoMultiRig::pump(600);
    r.runBlocks(40, 0.0f);
    MonoMultiRig::pump(600);

    // ★ 未选轨在整个布防期一片空白。空 gate + burst 限速的写法在这里红:
    //   残留的两千多 hop 会在撤防后被补拉进来,变成十几秒凭空多出的覆盖。
    CHECK(r.out.coverageOf(1, t0 + 0.05, t1 - 0.5).coveredS < 0.5);
    CHECK(r.out.coverageOf(3, t0 + 0.05, t1 - 0.5).coveredS < 0.5);
    // 对照组:选中轨在选区内确实采到了(否则上面两条是「什么都没发生」的假绿)。
    CHECK(r.out.coverageOf(2, t0 + 0.5, t0 + 1.5).coveredS > 0.3);
}

TEST_CASE("HOST J87:tracksMask 只点保留位不得退化成「不限轨」", "[host][t37][j87]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    REQUIRE(r.capture() > 4.0);
    const double t0 = static_cast<double>(r.ph.timeSamples) / kSr;

    // ⚠ 下面这行是**编译期恒真式**,不触达 handleRecaptureArm —— 它只是把「0x8000 掩完为 0,
    // 与本来就传 0 不可区分」这个前提写在案发现场,**不是守卫**(评审建议③:免得后人当它是)。
    // 桥面那条真正的拒绝路径(recaptureArm(0x8000,…) ⇒ reason:"noTracks")由
    // web-preview/tests/smoke-tab3-interactions.mjs 覆盖 —— handleRecaptureArm 要 WebView 才能
    // 构造,harness 够不着,与 P1-F/SL-190 抽纯函数是同一类结构性限制。
    CHECK((0x8000 & 0x7FFF) == 0);

    // 而合法掩码必须真的收窄:只点第 2 轨,第 1/3 轨在布防期一片空白。
    r.out.armRecapture(/*tracksMask=*/1u << 1, t0 + 0.5, t0 + 1.5, /*autoStop=*/false);
    MonoMultiRig::pump(200);
    r.runBlocks(200, 0.3f);
    MonoMultiRig::pump(400);
    r.out.disarmRecapture();

    CHECK(r.out.coverageOf(2, t0 + 0.5, t0 + 1.5).coveredS > 0.3);
    CHECK(r.out.coverageOf(1, t0 + 0.05, t0 + 2.0).coveredS < 0.05);
    CHECK(r.out.coverageOf(3, t0 + 0.05, t0 + 2.0).coveredS < 0.05);
}

// ===========================================================================
// [SL-225] v5.6 实测 P1 回归:**重采提醒不见了**。
// 用户现场:把上游 EQ 改狠 → 本该出「该轨上游音频与已采集特征不一致,建议重新采集」的
// ⚠(04 §4.5 fingerprint watchdog,#107 实装),v5.5 还在,v5.6 没了。
//
// #107 之后碰过这条链的只有 J87 的三个 PR(#124/#131/#140),所以先立一条**不变量**用例:
// **未布防的常规采集必须完全不受 J87 影响**。这条链很长而此前只有单测(FingerprintWatch /
// TileFingerprint)与 IPC 层用例,**没有任何端到端回归** —— 这正是它能悄悄断掉的原因。
// 本用例把整条链在真 harness 里跑通:
//
//   Input[A] 逐 hop 算 kw → 攒满 100 hop 一个 tile → FNV-1a 打包 → [A] SPSC 队列
//     → Input[M] 25Hz drainFpReports → 本 slot ctrl 命令环
//     → Output[M] drainCtrl 收 kFpReport → FingerprintWatch::onReport
//     → 拿 FrameStore 里**已采集**的 kw 重算同一 tile 的基线 → 比对 → 滞回 3 条 + >10%
//     → channelStale → 桥面 §2.8 channels[].stale
//
// 判据取 `captureStale`(引擎侧真值),不看 UI。
//
// 两个必须踩准的前提,踩不准这条用例会「绿得毫无意义」:
//   ① **fp_report 只在采集 OFF 时发**(FeatRing::accumulateFp:`if (capturing) return;`)——
//      采集 ON 时这一秒的特征正在被写成新基线,拿它跟自己比毫无意义;
//   ② 第二遍必须回到**同一段时间线**,否则 tile 号对不上、基线不存在(baselineTileFingerprint
//      在 coverage 不全时返回 false),那些上报会被「无基线」分支整条跳过,分母都进不去。
// ===========================================================================
TEST_CASE("HOST SL-225:常规采集(未布防)下上游改动仍须翻出 stale", "[host][t37][v56][SL225]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // ★ 不变量:本用例**全程不布防**。J87 的门控只在布防期改口径,常规采集这条路必须原样。
    REQUIRE_FALSE(r.out.runtime().recaptureArmed);

    // —— 第一遍:常规采集,录下「上游改动前」的素材,它就是基线 ——
    r.out.setCaptureEnabled(true);
    r.ph.timeSamples = 0;
    r.runBlocks(760, 0.5f); // ≈8.1s
    Rig::pumpMessages(400);
    REQUIRE(r.out.coverageOf(kTestChannel, 0.0, 7.0).coveredS > 5.0); // 前置:基线确实存在
    REQUIRE_FALSE(r.out.runtime().recaptureArmed);

    // —— 采集 OFF:这是 fp 上报的**开关**(前提①),不是可有可无的一步 ——
    r.out.setCaptureEnabled(false);
    Rig::pumpMessages(200);
    REQUIRE_FALSE(r.out.captureStale(kTestChannel)); // 前置:此刻还没有任何失配定谳

    // —— 第二遍:回到同一段时间线(前提②),喂**明显不同**的素材 = 用户把上游 EQ 改狠了 ——
    // 幅度差一个数量级 ⇒ kw 差远超 0.5dB 的量化桶 ⇒ 每个 tile 的 FNV 都不同。
    r.ph.timeSamples = 0;
    r.runBlocks(760, 0.05f);
    Rig::pumpMessages(600);

    // ★ 上游改动必须被认出来(滞回 3 条 + >10%,8 秒足够攒够)。
    CHECK(r.out.captureStale(kTestChannel));
}

// [SL-225] 上一条(纯常规路径)在 feature/v1 tip 上是**绿**的 —— 那条链本身没断。
// 于是把嫌疑面推到 J87 真正改过的地方:布防/撤防走一遭之后,这条链还在不在。
// 三种走法分别对应用户可能的操作序列;判据同上,取引擎侧 captureStale。
TEST_CASE("HOST SL-225:布防→撤防之后,上游改动仍须翻出 stale", "[host][t37][v56][SL225]")
{
    constexpr std::uint16_t kMask = 1u << (kTestChannel - 1);

    // 采一段基线(采集 ON),回报采到的秒数。
    const auto layBaseline = [](Rig& r) {
        r.out.setCaptureEnabled(true);
        r.ph.timeSamples = 0;
        r.runBlocks(760, 0.5f);
        Rig::pumpMessages(400);
        REQUIRE(r.out.coverageOf(kTestChannel, 0.0, 7.0).coveredS > 5.0);
    };
    // 回到同一段时间线喂不同素材(= 改狠了上游 EQ),再问 stale。
    const auto upstreamChangedThenAsk = [](Rig& r, float amplitude = 0.05f) {
        r.ph.timeSamples = 0;
        r.runBlocks(760, amplitude);
        Rig::pumpMessages(600);
        return r.out.captureStale(kTestChannel);
    };

    SECTION("布防前采集 OFF:撤防把采集关回去 ⇒ fp 上报恢复,stale 照常翻出")
    {
        Rig r;
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());
        layBaseline(r);

        r.out.setCaptureEnabled(false);
        r.out.armRecapture(kMask, 1.0, 2.0, /*autoStop=*/false); // 替用户开了采集
        REQUIRE(r.out.captureEnabled());
        r.out.disarmRecapture(); // 裁定③:恢复成布防前的 OFF
        REQUIRE_FALSE(r.out.captureEnabled());
        Rig::pumpMessages(200);

        CHECK(upstreamChangedThenAsk(r));
    }

    SECTION("布防期间用户自己把采集关掉:撤防不再替他动 ⇒ 采集是 OFF,stale 照常翻出")
    {
        Rig r;
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());
        layBaseline(r);

        r.out.setCaptureEnabled(false);
        r.out.armRecapture(kMask, 1.0, 2.0, /*autoStop=*/false);
        r.out.setCaptureEnabled(false); // 用户接管:布防期间自己关掉
        r.out.disarmRecapture();
        REQUIRE_FALSE(r.out.captureEnabled());
        Rig::pumpMessages(200);

        CHECK(upstreamChangedThenAsk(r));
    }

    SECTION("撤防后采集仍是 ON(布防前就开着):**按 §4.5 本就不该有 stale** —— 采集 ON 期间不上报")
    {
        Rig r;
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());
        layBaseline(r); // 这一步之后采集是 ON

        r.out.armRecapture(kMask, 1.0, 2.0, /*autoStop=*/false);
        r.out.disarmRecapture(); // 布防前本来就开着 ⇒ 保持开(裁定③)
        REQUIRE(r.out.captureEnabled());
        Rig::pumpMessages(200);

        // 这一条**期望为假**,而且不是缺陷:采集 ON 时这一秒的特征正被写成新基线,
        // 拿它跟自己比毫无意义(FeatRing::accumulateFp 的 `if (capturing) return;`)。
        // 钉住它是为了把「采集 ON 所以没提示」与「链断了所以没提示」两件事分开 ——
        // 用户看到的都是「提示不见了」,但只有后者是 bug。
        CHECK_FALSE(upstreamChangedThenAsk(r, 0.05f));

        // ⚠ 但**只有上面那条负向断言是空的**:链整个断掉它也照样绿。所以紧接着在**同一个
        // 配置上**把链跑活一次 —— 只关掉采集,别的都不动,⚠ 必须出现。两条合起来才说明
        // 「刚才没提示是因为采集 ON,不是因为链死了」(PR #146 评审建议:负向断言可空转)。
        //
        // 素材要换第三种:上一段是在**采集 ON** 下播的,它已经把基线重写成 0.05 那一版了,
        // 再拿 0.05 去比会匹配上 —— 那就又成了一条自证其说的假绿。
        r.out.setCaptureEnabled(false);
        Rig::pumpMessages(200);
        CHECK(upstreamChangedThenAsk(r, 0.3f));
    }
}

// [SL-225] 定谳落点:布防替用户开的采集会**被存进工程**。
//
// 前两条用例说明链本身没断。真正能让用户「提示莫名其妙没了」的是这条:
//   ① 用户点「重采集选区」→ [J87] 裁定① 替他打开 01 采集;
//   ② 用户此时保存工程(或宿主自动保存)→ CFGS 里 capture_enabled 落成 1;
//   ③ 重开工程 → 采集是 ON,而**布防位不持久化**(04 §4.2 ③,工作选区不落 state),
//      于是界面上没有任何「正在布防」的线索,用户也从没自己开过采集;
//   ④ 采集 ON 期间 Input 一条 fp_report 都不发(FeatRing::accumulateFp 的 `if (capturing) return;`)
//      —— 于是改多狠的 EQ 都不会再有 ⚠。
//
// 用户看到的就是「重采提醒消失了」,而且查不出原因:采集开关确实开着,但那不是他开的。
// 布防是**临时接管**,它替用户开的那一下不该越过一次保存活到下一个工程会话去。
TEST_CASE("HOST SL-225:布防替用户开的采集不得被存进工程", "[host][t37][v56][SL225]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    constexpr std::uint16_t kMask = 1u << (kTestChannel - 1);

    // 先采一段基线(⚠ 的比对对象),再由用户自己把采集关掉 —— 关着才是他要存进工程的状态。
    r.out.setCaptureEnabled(true);
    r.ph.timeSamples = 0;
    r.runBlocks(760, 0.5f);
    Rig::pumpMessages(400);
    REQUIRE(r.out.coverageOf(kTestChannel, 0.0, 7.0).coveredS > 5.0);

    r.out.setCaptureEnabled(false);
    REQUIRE_FALSE(r.out.captureEnabled());

    // 点「重采集选区」:布防替他把采集打开(裁定①)。
    r.out.armRecapture(kMask, 1.0, 2.0, /*autoStop=*/false);
    REQUIRE(r.out.captureEnabled());
    REQUIRE(r.out.runtime().recaptureAutoEnabledCapture); // 确实是我们开的,不是他

    // 此刻保存工程。
    juce::MemoryBlock blob;
    r.out.getStateInformation(blob);
    REQUIRE(blob.getSize() > 0);

    // 载回这份工程(= 重开工程)。用同一个实例载回,好让 FrameStore 里的基线还在 ——
    // 下面那半条断言要靠它把整条 ⚠ 链跑到底。
    r.out.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));

    // ★ 载回来的采集态必须是**用户自己选的那个**(OFF),不是布防替他开的那个。
    //   修复前:CFGS 里存的是 1,这里回 true —— 而布防位不持久化,用户完全看不出为什么。
    CHECK_FALSE(r.out.captureEnabled());
    // 布防位本来就不持久化,顺带钉住(04 §4.2 ③)。
    CHECK_FALSE(r.out.runtime().recaptureArmed);

    // ★★ 而且要真的把用户看得见的那件事跑通:改狠上游 → ⚠ 必须回来。
    //    这半条才是 SL-225 的正题 —— 上面那条只说明「开关值对了」,这条说明「提醒又出现了」。
    //    修复前:采集载回来是 ON,Input 一条 fp_report 都不发,这里恒 false。
    Rig::pumpMessages(200);
    r.ph.timeSamples = 0;
    r.runBlocks(760, 0.05f);
    Rig::pumpMessages(600);
    CHECK(r.out.captureStale(kTestChannel));
}

// ---------------------------------------------------------------------------
// [SL-226] 采集特征随工程往返 —— 重开工程泳道波形不得消失。
//
// 用户 v5.6 实测:保存 → 重开,**分段标记与分析内容都在,泳道波形全没**。
// 根因是特征持久化整条从未接通:段表走 CRVS(编解码都真接了),波形来自 FrameStore,
// 而 FEAT 节此前没有任何生产写点/读点 —— FrameStore 是纯内存,一关工程就没了。
// 两条持久化路径一条接了一条没接,所以只丢一半。
//
// 本用例走真 processor 的 get/setStateInformation,把「采集 → 存 → 毁内存 → 载 → 出图」
// 整条钉住;搬运层本身的逐 hop 往返在 tests/core/test_features_snapshot.cpp。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL-226:采集特征随工程往返,重开后泳道波形仍在", "[host][v56][SL226]")
{
    juce::MemoryBlock blob;
    double coveredS = 0.0;
    std::vector<int> beforeCovered;
    std::vector<double> beforeMax;
    std::vector<double> beforeMin;

    {
        Rig r;
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());
        r.out.setCaptureEnabled(true);
        Rig::pumpMessages(400);
        r.runBlocks(200, 0.5f);
        Rig::pumpMessages(400);

        coveredS = r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS;
        REQUIRE(coveredS > 0.0); // 前置:确实采到了东西,否则往返断言没有意义

        const auto tile = r.out.waveformOf(kTestChannel, 0.0, coveredS, 64);
        beforeCovered.assign(tile.covered.begin(), tile.covered.end());
        beforeMax.assign(tile.maxDb.begin(), tile.maxDb.end());
        beforeMin.assign(tile.minDb.begin(), tile.minDb.end());
        REQUIRE(std::count(beforeCovered.begin(), beforeCovered.end(), 0) < 64); // 存前有波形

        r.out.getStateInformation(blob);
        REQUIRE(blob.getSize() > 0);
    } // Rig 析构 = 内存里的 FrameStore 连同实例一起没了(等价于关工程)

    // 全新实例 = 重开工程。加载前必须是空的,否则下面的断言证明不了任何事。
    Rig r2;
    REQUIRE(r2.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS == 0.0);

    r2.out.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    Rig::pumpMessages(200);

    // ① 覆盖回来了。
    const double after = r2.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS;
    CHECK(after == Catch::Approx(coveredS));

    // ② 波形逐列与存前对拍 —— 这一条修复前恒红:整条泳道 covered 全 0。
    const auto tile2 = r2.out.waveformOf(kTestChannel, 0.0, coveredS, 64);
    REQUIRE(tile2.covered.size() == beforeCovered.size());
    int coveredCols = 0;
    for (std::size_t i = 0; i < beforeCovered.size(); ++i)
    {
        INFO("col=" << i);
        CHECK(tile2.covered[i] == beforeCovered[i]);
        if (beforeCovered[i] != 0)
        {
            ++coveredCols;
            // 落盘存的就是量化值(int16 dB×100),回灌不再二次量化,所以包络应当原样回来。
            CHECK(tile2.maxDb[i] == Catch::Approx(beforeMax[i]));
            CHECK(tile2.minDb[i] == Catch::Approx(beforeMin[i]));
            CHECK(tile2.maxDb[i] > -160.0); // 不是哨兵
        }
    }
    CHECK(coveredCols > 0);

    // ③ 没采过的远端仍如实回「未覆盖」:回灌不许把空洞填平。
    // (变量名别用 far —— windows.h 把 far/near 定义成宏了。)
    const auto distant = r2.out.waveformOf(kTestChannel, 600.0, 610.0, 16);
    for (const int c : distant.covered)
    {
        CHECK(c == 0);
    }
}

// [SL-226] 反向①:没采集过的工程存下去不带 FEAT,读回来也不该凭空长出波形。
TEST_CASE("HOST SL-226:反向 —— 未采集的工程往返后仍无波形", "[host][v56][SL226]")
{
    juce::MemoryBlock blob;
    {
        Rig r;
        r.out.getStateInformation(blob); // 从没开过采集
        REQUIRE(blob.getSize() > 0);
    }

    Rig r2;
    r2.out.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    Rig::pumpMessages(200);

    CHECK(r2.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS == 0.0);
    const auto tile = r2.out.waveformOf(kTestChannel, 0.0, 10.0, 32);
    for (const int c : tile.covered)
    {
        CHECK(c == 0);
    }
}

// [SL-226] 反向②:特征被清空后再保存,必须把 FEAT 一并删掉 —— 留着上一版会让重开时
// 把已经不存在的波形又捞回来。清空走**改组**这条真实用户路径([J66]:frameStore 按 channel
// 索引存、没有 group 维度,改组必须整店作废)。
TEST_CASE("HOST SL-226:反向 —— 特征清空后保存,重开不得捞回旧波形", "[host][v56][SL226]")
{
    juce::MemoryBlock withFeat;
    juce::MemoryBlock cleared;
    {
        Rig r;
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());
        r.out.setCaptureEnabled(true);
        Rig::pumpMessages(400);
        r.runBlocks(200, 0.5f);
        Rig::pumpMessages(400);
        REQUIRE(r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS > 0.0);
        r.out.getStateInformation(withFeat);

        r.out.setGroupId(6); // 改组 → frameStore 整店作废
        REQUIRE(r.out.groupId() == 6); // [SL-324] 读回断言
        Rig::pumpMessages(300);
        REQUIRE(r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS == 0.0);
        r.out.getStateInformation(cleared);
    }

    // 带 FEAT 的那份照常恢复(前置:证明这两份 blob 确实不同)。
    {
        Rig a;
        a.out.setStateInformation(withFeat.getData(), static_cast<int>(withFeat.getSize()));
        Rig::pumpMessages(200);
        CHECK(a.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS > 0.0);
    }
    // 清空后保存的那份,重开必须仍是空的。
    {
        Rig b;
        b.out.setStateInformation(cleared.getData(), static_cast<int>(cleared.getSize()));
        Rig::pumpMessages(200);
        CHECK(b.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS == 0.0);
    }
}

// [SL-226] 回归①(#147 审查【重要】①):**工程组 ≠ 当前组**时,回灌不得被 changeGroup 抹掉。
// setStateInformation 末尾会在「已 prepared 且组不同」时走 session_.changeGroup(),而它第一件事
// 就是 frameStore_.reset()。本卡初版把回灌排在它前面 —— 同组的用例全绿,换组的工程照样一片空白,
// 而「宿主先 prepareToPlay(默认组)再灌工程 chunk」正是真机常规时序(v5 实测 P0-5 同款)。
TEST_CASE("HOST SL-226:回归 —— 换组加载时波形不被 changeGroup 抹掉", "[host][v56][SL226]")
{
    juce::MemoryBlock blob;
    double coveredS = 0.0;
    {
        Rig r; // Rig 用 kTestGroup(7)
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());
        r.out.setCaptureEnabled(true);
        Rig::pumpMessages(400);
        r.runBlocks(200, 0.5f);
        Rig::pumpMessages(400);
        coveredS = r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS;
        REQUIRE(coveredS > 0.0);
        r.out.getStateInformation(blob); // 工程记的是组 7
    }

    // 新实例**先 prepareToPlay 在另一个组**,再灌工程 chunk —— 于是加载末尾必然走 changeGroup。
    ScvbOutputAudioProcessor out2;
    out2.setGroupId(4); // 与工程里的组 7 不同
    REQUIRE(out2.groupId() == 4); // [SL-324] 读回断言
    FakePlayHead ph;
    out2.setPlayHead(&ph);
    out2.prepareToPlay(kSr, kBlock);
    Rig::pumpMessages(200);
    REQUIRE(out2.coverageOf(kTestChannel, 0.0, 30.0).coveredS == 0.0);

    out2.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    Rig::pumpMessages(300);

    // ← 修复前这里恒 0:回灌完又被 changeGroup 的 frameStore_.reset() 抹掉了。
    CHECK(out2.coverageOf(kTestChannel, 0.0, 30.0).coveredS == Catch::Approx(coveredS));
    const auto tile = out2.waveformOf(kTestChannel, 0.0, coveredS, 32);
    CHECK(std::count(tile.covered.begin(), tile.covered.end(), 0) < 32);

    out2.releaseResources();
}

// [SL-226] 回归②(#147 审查【重要】②):只带 PRMS 的部分 blob(轨道预设/参数预设)不得清空已采特征。
// SL-217(#126)为 CRVS 立的规矩:「没有信息」不能读成「删除全部」。特征这条路同理 ——
// 加载一个参数预设把波形静默清光、且不可撤销,正是 SL-226 症状的镜像。
TEST_CASE("HOST SL-226:回归 —— 只带 PRMS 的预设不得清空已采波形", "[host][v56][SL226]")
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

    // 造一份**只含 PRMS** 的容器(宿主的轨道预设/参数预设就是这个形状:没有 CFGS、没有 FEAT)。
    juce::MemoryBlock full;
    r.out.getStateInformation(full);
    scvb::state::StateChunks loaded;
    REQUIRE(scvb::state::loadState(static_cast<const std::uint8_t*>(full.getData()), full.getSize(), loaded).status ==
            scvb::state::StateLoadStatus::Ok);

    scvb::state::StateChunks presetOnly;
    presetOnly.abi = scvb::state::kCurrentAbi;
    const scvb::state::Chunk* prms = loaded.find(scvb::state::kFourccPrms);
    REQUIRE(prms != nullptr);
    presetOnly.set(scvb::state::kFourccPrms, prms->payload);
    REQUIRE(presetOnly.find(scvb::state::kFourccCfgs) == nullptr); // 前置:确实是部分 blob
    REQUIRE(presetOnly.find(scvb::state::kFourccFeat) == nullptr);

    std::vector<std::uint8_t> presetBlob;
    REQUIRE(scvb::state::encodeContainer(presetOnly, presetBlob));

    r.out.setStateInformation(presetBlob.data(), static_cast<int>(presetBlob.size()));
    Rig::pumpMessages(200);

    // ← 修复前:readFeaturesChunk 排在 CFGS 早退之前,缺 FEAT 即 reset,波形被静默清空。
    CHECK(r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS == Catch::Approx(coveredS));
    const auto tile = r.out.waveformOf(kTestChannel, 0.0, coveredS, 32);
    CHECK(std::count(tile.covered.begin(), tile.covered.end(), 0) < 32);
}

// [SL-226] 回归③(#147 复审【重要】③):**CFGS 损坏 ≠ 部分 blob**。
// 这是一份完整工程,只是配置节坏了 —— 特征仍必须按本工程的 FEAT 处理。不处理的话上一个工程的
// 波形会留在 FrameStore 里冒充本工程的,而 loadedChunks_ 已经换成本工程的了,
// **下次保存就把上一个工程的特征写进这个工程**。
TEST_CASE("HOST SL-226:回归 —— CFGS 损坏的完整工程不得留下上一工程的波形", "[host][v56][SL226]")
{
    // 工程 A:采过一段,存下来(带 FEAT)。
    juce::MemoryBlock blobA;
    {
        Rig r;
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());
        r.out.setCaptureEnabled(true);
        Rig::pumpMessages(400);
        r.runBlocks(200, 0.5f);
        Rig::pumpMessages(400);
        REQUIRE(r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS > 0.0);
        r.out.getStateInformation(blobA);
    }

    // 工程 B:**没有 FEAT**(没采过),但 CFGS 被打坏。
    juce::MemoryBlock blobBFull;
    {
        Rig r;
        r.out.getStateInformation(blobBFull);
    }
    scvb::state::StateChunks b;
    REQUIRE(
        scvb::state::loadState(static_cast<const std::uint8_t*>(blobBFull.getData()), blobBFull.getSize(), b).status ==
        scvb::state::StateLoadStatus::Ok);
    REQUIRE(b.find(scvb::state::kFourccFeat) == nullptr); // 前置:B 确实没有特征
    // 把 CFGS 换成一段长度合法但内容过短的垃圾 → decodeOutputState 必失败。
    b.set(scvb::state::kFourccCfgs, std::vector<std::uint8_t>{0x01, 0x02, 0x03});
    std::vector<std::uint8_t> blobB;
    REQUIRE(scvb::state::encodeContainer(b, blobB));

    // 同一个实例:先载 A(有波形),再载 CFGS 损坏的 B(无波形)。
    Rig r2;
    r2.out.setStateInformation(blobA.getData(), static_cast<int>(blobA.getSize()));
    Rig::pumpMessages(200);
    REQUIRE(r2.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS > 0.0); // A 的波形在

    r2.out.setStateInformation(blobB.data(), static_cast<int>(blobB.size()));
    Rig::pumpMessages(200);

    // ← 修复前:CFGS 早退把回灌一并跳过,A 的波形留在 FrameStore 里冒充 B 的。
    CHECK(r2.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS == 0.0);

    // 而且下次保存不得把 A 的特征写进 B。
    juce::MemoryBlock resaved;
    r2.out.getStateInformation(resaved);
    scvb::state::StateChunks out;
    REQUIRE(
        scvb::state::loadState(static_cast<const std::uint8_t*>(resaved.getData()), resaved.getSize(), out).status ==
        scvb::state::StateLoadStatus::Ok);
    CHECK(out.find(scvb::state::kFourccFeat) == nullptr);
}

// [SL-226] 回归④(#147 三轮复审【重要】):「原样带走」的纪律不能挂在 loadedChunks_ 上。
//
// 三步序列:① 开一份 FEAT 是**高 codecVer**(本构建解不开)的工程 → 置 featCodecNewer_,
// 该节必须原样带走;② 载一个只带 PRMS 的预设 → loadedChunks_ 被整个换成 {PRMS},而这条路
// 不走 readFeaturesChunk、标志位不复位;③ 保存 → 若「原样回写」靠的是「chunks 从 loadedChunks_
// 拷来、什么都不做即可」,此刻 chunks 里根本没有 FEAT —— 那份不认识的字节就永久消失了。
// 修法是自己留底(preservedFeatChunk_)并在早退处**显式写回**。
TEST_CASE("HOST SL-226:回归 —— 高 codecVer 的 FEAT 经 PRMS-only 预设后仍原样带走", "[host][v56][SL226]")
{
    // 造一份 FEAT payload 为「高 codecVer」的工程:拿真实容器,把 FEAT 换成 codecVer=99 的节。
    juce::MemoryBlock baseBlob;
    {
        Rig r;
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());
        r.out.setCaptureEnabled(true);
        Rig::pumpMessages(400);
        r.runBlocks(120, 0.5f);
        Rig::pumpMessages(400);
        REQUIRE(r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS > 0.0);
        r.out.getStateInformation(baseBlob);
    }

    scvb::state::StateChunks base;
    REQUIRE(
        scvb::state::loadState(static_cast<const std::uint8_t*>(baseBlob.getData()), baseBlob.getSize(), base).status ==
        scvb::state::StateLoadStatus::Ok);
    REQUIRE(base.find(scvb::state::kFourccFeat) != nullptr);

    // 高 codecVer 的 FEAT 节:tag + codecVer=99 + 其余头字段(gzip 后放进 chunk)。
    std::vector<std::uint8_t> raw;
    const auto put32 = [&raw](std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            raw.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    };
    const auto put16 = [&raw](std::uint16_t v) {
        for (int i = 0; i < 2; ++i)
            raw.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    };
    put32(scvb::state::kFeatTag);
    put16(99); // codecVer 远高于本构建
    put16(scvb::state::kFeatFlagEmbedded);
    put32(48000u);
    put32(10u);
    raw.push_back(0); // channelCount = 0
    const auto futureFeat = scvb::state::gzipCompress(raw.data(), raw.size());
    REQUIRE_FALSE(futureFeat.empty());

    base.set(scvb::state::kFourccFeat, futureFeat);
    std::vector<std::uint8_t> futureBlob;
    REQUIRE(scvb::state::encodeContainer(base, futureBlob));

    // 只带 PRMS 的预设 blob。
    scvb::state::StateChunks presetOnly;
    presetOnly.abi = scvb::state::kCurrentAbi;
    presetOnly.set(scvb::state::kFourccPrms, base.find(scvb::state::kFourccPrms)->payload);
    std::vector<std::uint8_t> presetBlob;
    REQUIRE(scvb::state::encodeContainer(presetOnly, presetBlob));

    Rig r2;
    // ① 载入高 codecVer 工程。
    r2.out.setStateInformation(futureBlob.data(), static_cast<int>(futureBlob.size()));
    Rig::pumpMessages(200);
    CHECK(r2.out.featureBytes() == static_cast<std::int64_t>(futureFeat.size())); // 解不开也如实报大小

    // ② 载入只带 PRMS 的预设(loadedChunks_ 被换成 {PRMS})。
    r2.out.setStateInformation(presetBlob.data(), static_cast<int>(presetBlob.size()));
    Rig::pumpMessages(200);

    // ③ 保存:那份解不开的 FEAT 必须**逐字节还在**。
    juce::MemoryBlock resaved;
    r2.out.getStateInformation(resaved);
    scvb::state::StateChunks out;
    REQUIRE(
        scvb::state::loadState(static_cast<const std::uint8_t*>(resaved.getData()), resaved.getSize(), out).status ==
        scvb::state::StateLoadStatus::Ok);
    const scvb::state::Chunk* kept = out.find(scvb::state::kFourccFeat);
    REQUIRE(kept != nullptr); // ← 修复前:这里是 nullptr,用户的高版本特征被静默丢弃
    CHECK(kept->payload == futureFeat); // 逐字节相等,不是"重编了一份空的"
}

// [SL-226] 回归⑤(#147 四轮复审【建议】②):高 codecVer 的工程里**重新采集**,新数据必须存得进去。
// 「原样带走」对**没动过**的工程是对的;但若早退排在 snapshot 之前,用户「开一份读不出来的工程 →
// 泳道空 → 重新采集 → 保存」会把刚采的新数据静默丢掉,重开还是空 —— 正是本卡要治的症状换了个入口。
TEST_CASE("HOST SL-226:回归 —— 高 codecVer 工程里重新采集,新数据不得被静默丢弃", "[host][v56][SL226]")
{
    // 造一份 FEAT 为高 codecVer 的工程(手法同回归④)。
    juce::MemoryBlock baseBlob;
    {
        Rig r;
        r.out.getStateInformation(baseBlob);
    }
    scvb::state::StateChunks base;
    REQUIRE(
        scvb::state::loadState(static_cast<const std::uint8_t*>(baseBlob.getData()), baseBlob.getSize(), base).status ==
        scvb::state::StateLoadStatus::Ok);

    std::vector<std::uint8_t> raw;
    const auto put32 = [&raw](std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            raw.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    };
    const auto put16 = [&raw](std::uint16_t v) {
        for (int i = 0; i < 2; ++i)
            raw.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    };
    put32(scvb::state::kFeatTag);
    put16(99);
    put16(scvb::state::kFeatFlagEmbedded);
    put32(48000u);
    put32(10u);
    raw.push_back(0);
    const auto futureFeat = scvb::state::gzipCompress(raw.data(), raw.size());
    REQUIRE_FALSE(futureFeat.empty());
    base.set(scvb::state::kFourccFeat, futureFeat);
    std::vector<std::uint8_t> futureBlob;
    REQUIRE(scvb::state::encodeContainer(base, futureBlob));

    Rig r;
    r.out.setStateInformation(futureBlob.data(), static_cast<int>(futureBlob.size()));
    Rig::pumpMessages(200);
    REQUIRE(r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS == 0.0); // 解不开 → 泳道空

    // 用户重新采集。
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    r.runBlocks(150, 0.5f);
    Rig::pumpMessages(400);
    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS;
    REQUIRE(coveredS > 0.0);

    juce::MemoryBlock resaved;
    r.out.getStateInformation(resaved);

    scvb::state::StateChunks out;
    REQUIRE(
        scvb::state::loadState(static_cast<const std::uint8_t*>(resaved.getData()), resaved.getSize(), out).status ==
        scvb::state::StateLoadStatus::Ok);
    const scvb::state::Chunk* kept = out.find(scvb::state::kFourccFeat);
    REQUIRE(kept != nullptr);
    CHECK(kept->payload != futureFeat); // ← 修复前:早退在 snapshot 之前,原样带走旧的、丢掉新的

    // 重开:新采的波形必须回得来。
    Rig r2;
    r2.out.setStateInformation(resaved.getData(), static_cast<int>(resaved.getSize()));
    Rig::pumpMessages(200);
    CHECK(r2.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS == Catch::Approx(coveredS));
}

// [SL-206] VAD 后验(vadP)必须有**生产者**。
//
// 定谳(ui-r2):泳道绿线的渲染侧齐全、`EnergyVad` 早就支持 `posteriorOut`、`FrameStore` 早就有
// `setVadP`、`waveformOf` 早就按 `vadP(h) > 127` 给瓦片算 vad 列 —— 唯独中间那一段没人接:
// 管线调 `runEnergyVad` 时第五参传 `nullptr`,后验算完就扔。于是 vadP 全仓**恒 0**,
// 真机绿线一次都没画出来过;而 web-preview 的 mock 自己算了一份,preview 里一直看得见 ——
// 这是「mock 盖住真机」的第三次(用户两次被它误导),所以本组用例断的是**真机数据面**。
// ===========================================================================
TEST_CASE("HOST SL-206:分析后 vadP 非全零,且与能量形状相关", "[host][t37][v55][SL206]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // 采一段**有声有静**交替的素材:后验若真接上了,它必须跟着这个形状走。
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    for (int burst = 0; burst < 8; ++burst)
    {
        r.runBlocks(60, 0.5f, 4, 4); // 有声
        r.runBlocks(40, 0.0f, 4, 4); // 静音
    }
    Rig::pumpMessages(400);

    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS;
    REQUIRE(coveredS > 2.0);

    // 分析前:vadP 恒 0(生产者只有分析这一条路)。
    {
        const auto tile = r.out.waveformOf(kTestChannel, 0.0, coveredS, 64);
        int voicedBefore = 0;
        for (const auto v : tile.vad)
        {
            voicedBefore += v ? 1 : 0;
        }
        REQUIRE(voicedBefore == 0); // 前置:分析前没有任何有声列
    }

    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS).ok);
    for (int waited = 0; waited < 20000; waited += 50)
    {
        Rig::pumpMessages(50);
        if (!r.out.analysisRunning() && !r.out.runtime().analysisRunning)
        {
            break;
        }
    }
    REQUIRE_FALSE(r.out.analysisRunning());

    // ★ 分析后:瓦片的 vad 列**不再全零**(修复前这里恒 0 —— 绿线画不出来的直接判据)。
    constexpr int kCols = 96;
    const auto tile = r.out.waveformOf(kTestChannel, 0.0, coveredS, kCols);
    int voiced = 0;
    int coveredCols = 0;
    double kwVoiced = 0.0;
    double kwSilent = 0.0;
    int nVoiced = 0;
    int nSilent = 0;
    for (int i = 0; i < kCols; ++i)
    {
        const auto k = static_cast<std::size_t>(i);
        if (!tile.covered[k])
        {
            continue;
        }
        ++coveredCols;
        if (tile.vad[k])
        {
            ++voiced;
            kwVoiced += tile.maxDb[k];
            ++nVoiced;
        }
        else
        {
            kwSilent += tile.maxDb[k];
            ++nSilent;
        }
    }
    REQUIRE(coveredCols > 0);
    CHECK(voiced > 0); // ★ 核心:有声列存在
    CHECK(voiced < coveredCols); // 且不是「全判有声」——那等于没判

    // ★ 与能量形状相关:被判有声的列,包络峰值应显著高于被判静音的列。
    REQUIRE(nVoiced > 0);
    REQUIRE(nSilent > 0);
    CHECK(kwVoiced / nVoiced > kwSilent / nSilent + 6.0); // 至少 6dB 的差
}

TEST_CASE("HOST SL-206:后验只写本次范围,范围外的 hop 不被动", "[host][t37][v55][SL206]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

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
    const double half = coveredS * 0.5;

    // 只分析后半段。
    REQUIRE(r.out.startAnalysis(0, half, coveredS).ok);
    for (int waited = 0; waited < 20000; waited += 50)
    {
        Rig::pumpMessages(50);
        if (!r.out.analysisRunning() && !r.out.runtime().analysisRunning)
        {
            break;
        }
    }
    REQUIRE_FALSE(r.out.analysisRunning());

    const auto front = r.out.waveformOf(kTestChannel, 0.0, half, 48);
    const auto back = r.out.waveformOf(kTestChannel, half, coveredS, 48);
    int voicedFront = 0;
    int voicedBack = 0;
    for (std::size_t i = 0; i < 48; ++i)
    {
        voicedFront += front.vad[i] ? 1 : 0;
        voicedBack += back.vad[i] ? 1 : 0;
    }
    CHECK(voicedBack > 0); // 分析过的那一半:有声列出现
    CHECK(voicedFront == 0); // 没分析的那一半:后验没被写过,仍是 0
}

// ---------------------------------------------------------------------------
// [SL-206 复审重要②] 后验一有生产者,就同时激活了一条**以前不可能出现**的陈旧数据路径:
// `ChannelFrames::write()` 只覆写 kw/peak,**不动 vadP**;`invalidate()` 也只打洞、页留着。
// 于是「采集 → 分析 → 清除该区间 → 重采一遍别的音频」之后,kw/peak 是新的、vadP 还是上一份
// 素材的判决 —— 泳道照着**旧素材**画绿线,直到用户再分析一次。
// 修法:write() 里 O(1) 清 vadP(新特征进来 = 旧判决作废)。本例钉死它。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL-206:清覆盖后重采,旧绿线不得残留", "[host][t37][v55][SL206]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // ① 采一段有声有静的素材并分析 → vadP 有值。
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    const std::int64_t t0 = r.ph.timeSamples;
    for (int burst = 0; burst < 8; ++burst)
    {
        r.runBlocks(60, 0.5f, 4, 4);
        r.runBlocks(40, 0.0f, 4, 4);
    }
    Rig::pumpMessages(400);
    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS;
    REQUIRE(coveredS > 2.0);

    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS).ok);
    for (int waited = 0; waited < 20000; waited += 50)
    {
        Rig::pumpMessages(50);
        if (!r.out.analysisRunning() && !r.out.runtime().analysisRunning)
        {
            break;
        }
    }
    REQUIRE_FALSE(r.out.analysisRunning());

    const auto voicedCols = [&r](double a, double b, int cols) {
        const auto tile = r.out.waveformOf(kTestChannel, a, b, cols);
        int v = 0;
        for (int i = 0; i < cols; ++i)
        {
            v += tile.vad[static_cast<std::size_t>(i)] ? 1 : 0;
        }
        return v;
    };
    REQUIRE(voicedCols(0.0, coveredS, 64) > 0); // 前置:绿线确实出来了

    // ② 清除该区间的覆盖,再**重采一段纯静音**(与第一次完全不同的素材)。
    r.out.clearCoverage(static_cast<std::uint16_t>(1u << (kTestChannel - 1)), 0.0, coveredS);
    Rig::pumpMessages(200);
    r.ph.timeSamples = t0; // 回到原处重采
    for (int burst = 0; burst < 8; ++burst)
    {
        r.runBlocks(100, 0.0f, 4, 4); // 全静音
    }
    Rig::pumpMessages(400);

    // 前置:重采**确实落账**了(#151 复审【重要】1)。`waveformOf` 对未覆盖的列一律返回
    // covered=0/vad=0(哨兵纪律),所以下面那个 0 有**两条**通过路径:想断的那条是
    // `write()` 里 `page->vadP[idx]=0` 生效;不想要的那条是重采根本没落账
    // (FeatPuller 的 lastPulled 单调推进,playhead 回跳后这批 hop 会被整批跳过)——
    // 那时该区间自始至终没有覆盖列,断言同样为 0 而 write() 一次都没被调用,
    // 这一例就再也钉不住那行。加这条硬前置把第 2 条路堵死。
    REQUIRE(r.out.coverageOf(kTestChannel, 0.0, coveredS).coveredS > 0.0);

    // ★ 重采之后**没有再分析**:该区间的绿线必须已经作废(而不是照着旧素材继续画)。
    CHECK(voicedCols(0.0, coveredS, 64) == 0);
}

// ===========================================================================
// [SL-209] 分析结果可撤销(用户新需求)。
//
// 分析回落整轮包成**一次** CRVS 事务压进既有 UndoManager(与段编辑同栈):
// Ctrl+Z 恢复分析前的段表,Ctrl+Y 重放分析结果。
//
// 「与『分析不覆盖 user 段』共存」这条是自动成立的:事务快照的是**整个 CrvsData**,
// undo 还原的是「分析前的那一刻」—— 那一刻本来就含着这一轮会被保留的用户段与范围外
// auto 段,不需要另算「实际被替换面」。下面第二例就是拿一条用户段把这一点钉死。
//
// 契约 §1.6「撤销」行已随本卡改判为「是」([J89],2026-08-28 用户批准),§0.9 撤销表左列
// 亦已登记 analyze;变更文档 20260827-sl209-analyze-undoable.md 与改动同 PR(§5)。
// ===========================================================================
TEST_CASE("HOST SL-209:分析可撤销 —— 撤销回分析前,重做回分析结果", "[host][t37][v55][SL209]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);

    constexpr int kCh = 1;
    const auto beforeAnalyze = segmentsOfTrack(r.out, kCh); // 分析前(通常是空表)

    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));
    const auto afterAnalyze = segmentsOfTrack(r.out, kCh);
    REQUIRE_FALSE(afterAnalyze.empty()); // 前置:确实分析出东西了
    REQUIRE_FALSE(sameSegments(afterAnalyze, beforeAnalyze));

    // ★ 撤销:回到分析前的段表(一次分析 = 一条撤销步)。
    REQUIRE(r.out.undo());
    CHECK(sameSegments(segmentsOfTrack(r.out, kCh), beforeAnalyze));
    // 曲线也跟着回退(撤销走的是 commitCrvsTransaction 的 rebuild 回调)。
    // 分析前是空表 → 曲线指针应为空;非空表则应有曲线。
    CHECK((activeCurveOf(r.out, kCh) != nullptr) == !beforeAnalyze.empty());

    // ★ 重做:重放分析结果(逐字节相同)。
    REQUIRE(r.out.redo());
    CHECK(sameSegments(segmentsOfTrack(r.out, kCh), afterAnalyze));
    CHECK(activeCurveOf(r.out, kCh) != nullptr);

    // 一次分析只压**一条**步:再撤一次应当又回到分析前,而不是停在中间态。
    REQUIRE(r.out.undo());
    CHECK(sameSegments(segmentsOfTrack(r.out, kCh), beforeAnalyze));
}

TEST_CASE("HOST SL-209:撤销与「分析不覆盖 user 段」共存", "[host][t37][v55][SL209]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);

    constexpr int kCh = 2;
    // 先造一条用户段(未冻结 → 手动接管通道写曲线真身,origin=user_edited)。
    int replaced = 0;
    int replacedLocked = 0;
    REQUIRE(r.out.setTrackManual(kCh, /*isPan=*/true, -55.0f, replaced, replacedLocked));
    const auto withUserSeg = segmentsOfTrack(r.out, kCh);
    REQUIRE(withUserSeg.size() == 1);
    REQUIRE(scvb::state::segmentOrigin(withUserSeg.front().flags) == scvb::state::SegmentOrigin::UserEdited);

    // 普通分析(不带 clearManual):ADR-008 —— 用户段必须原样保留。
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));
    {
        bool stillHasUser = false;
        for (const auto& sg : segmentsOfTrack(r.out, kCh))
        {
            if (scvb::state::segmentOrigin(sg.flags) == scvb::state::SegmentOrigin::UserEdited)
            {
                stillHasUser = true;
            }
        }
        CHECK(stillHasUser); // 前置:共存语义本身没坏
    }
    const auto afterAnalyze = segmentsOfTrack(r.out, kCh);

    // ★ 撤销:回到**分析前那一刻** —— 那一刻的表里就有这条用户段,所以它当然还在。
    //    这正是「整表快照」口径的好处:不需要另算「这一轮实际替换了哪几段」。
    REQUIRE(r.out.undo());
    CHECK(sameSegments(segmentsOfTrack(r.out, kCh), withUserSeg));

    // ★ 重做:回到分析结果(用户段仍在其中)。
    REQUIRE(r.out.redo());
    CHECK(sameSegments(segmentsOfTrack(r.out, kCh), afterAnalyze));
}

TEST_CASE("HOST SL-209:局部(选区)分析同样可撤销,且不碰范围外的段", "[host][t37][v55][SL209]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

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

    runAnalysis(0.0, coveredS); // 打底:全范围
    const auto base = segmentsOfTrack(r.out, kTestChannel);
    REQUIRE_FALSE(base.empty());

    runAnalysis(coveredS * 0.5, coveredS); // 只重分析后半段
    const auto afterPartial = segmentsOfTrack(r.out, kTestChannel);

    // ★ 撤销局部分析:整表回到打底那一轮的样子(范围外的段本来就没动,撤销后当然一致)。
    REQUIRE(r.out.undo());
    CHECK(sameSegments(segmentsOfTrack(r.out, kTestChannel), base));
    REQUIRE(r.out.redo());
    CHECK(sameSegments(segmentsOfTrack(r.out, kTestChannel), afterPartial));
}

// ---------------------------------------------------------------------------
// [#152 复审【建议】1] **空转分析不得压撤销步、不得清空重做栈**。
//
// `applyAnalysisSegments` 的每轨循环开头是 `if (src.empty()) continue;` —— 15 轨全无产出时
// 它是恒等变换。照压不误的后果:用户按一次 Ctrl+Z 段表纹丝不动,而 juce 的「新事务入栈必清
// redo 栈」语义已经把攒着的重做历史吃掉了 —— 一次什么都没发生的操作毁掉真实的重做面。
// 与本仓既有口径一致:editSegmentTransactional 判失败不进 undo(PR#55 缺陷3)、
// setVersionName 名字未变则短路不产生空事务。
//
// ★ 反向验证:把 finishAnalysis 里的 `if (tableChanged)` 守卫去掉(无条件 commit),
//   下面「redo 仍可用」与「undo 弹回的是编辑前」两条立刻红。
//   ([SL-255] 起该守卫由 `producedAny`(15 轨零产出)升级为 `tableChanged`(段表五个
//   字段全等)—— 后者是前者的真超集:零产出 ⇒ 恒等 ⇒ tableChanged 为假,本例行为不变。)
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL-209:空转分析(零产出)不压撤销步、不吃掉重做栈", "[host][t37][v55][SL209]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    constexpr int kCh = 1;

    // ① 先做一笔**真实**的 CRVS 变更(setTrackManual 走 commitCrvsTransaction,§0.9 左列),
    //    攒出一条货真价实的撤销步。
    int replaced = 0;
    int replacedLocked = 0;
    REQUIRE(r.out.setTrackManual(kCh, /*isPan=*/true, 25.0f, replaced, replacedLocked));
    const auto afterEdit = segmentsOfTrack(r.out, kCh);
    REQUIRE_FALSE(afterEdit.empty());

    // ② 撤销再重做一次 —— 现在重做栈是空的、撤销栈有一条,状态回到 afterEdit。
    REQUIRE(r.out.undo());
    const auto beforeEdit = segmentsOfTrack(r.out, kCh);
    REQUIRE(r.out.redo());
    REQUIRE(sameSegments(segmentsOfTrack(r.out, kCh), afterEdit));

    // ③ 再撤一次,把这条步挪到**重做**侧 —— 这就是空转分析将要吃掉的那份历史。
    REQUIRE(r.out.undo());
    REQUIRE(sameSegments(segmentsOfTrack(r.out, kCh), beforeEdit));

    // ④ 采一段纯静音后分析:覆盖区有(分析受理),但一句都检不出 ⇒ 零产出。
    const auto silent = r.captureSilence();
    REQUIRE(silent.coveredS > 0.0);
    REQUIRE(r.runAnalysisIn(silent.fromS, silent.fromS + silent.coveredS, /*clearManual=*/false));
    // 前置:确实零产出。判据要盖**全 15 轨 × 两版本** —— `producedAny` 看的是整份 result,
    // 只查一轨会让「别的轨检出了一段」偷偷把前提蒙混过去(那时事务照压,本例断言的就不是守卫了)。
    {
        const auto snap = r.out.crvsSnapshot();
        std::size_t total = 0;
        for (const auto& v : snap.versions)
        {
            for (const auto& t : v.tracks)
            {
                total += t.segments.size();
            }
        }
        REQUIRE(total == 0);
    }

    // ★ 重做栈没被吃掉:那条段编辑仍重做得回来。
    CHECK(r.out.redo());
    CHECK(sameSegments(segmentsOfTrack(r.out, kCh), afterEdit));

    // ★ 也没多压一条空步:一次 undo 就该弹回编辑前,而不是先弹掉一条恒等步。
    CHECK(r.out.undo());
    CHECK(sameSegments(segmentsOfTrack(r.out, kCh), beforeEdit));
}

// ===========================================================================
// SL-231 长链端到端护栏(盘点矩阵见 docs/testing/long-chain-e2e-map.md)
//
// 立卡背景:04 §4.5 的指纹链在 v5.6 之前**零端到端护栏**,于是 J87 那个「布防替用户开的
// 采集态泄漏进工程」的洞一路睡到用户手里才被发现(#146 定谳)。盘点六条跨进程/跨线程长链后,
// 只有两处仍是「每一跳都有单测、整条链没人走」——本节各补一条,判据一律落在**链末端的
// 可观测量**上,不看被测物自己的内部计数器。
// ===========================================================================

// ---------------------------------------------------------------------------
// SL-231 ①:viz 发布链 —— 真 Output 的配置/曲线 → viz 段 → 只读方读回。
//
// 既有覆盖为什么不够:viz 段的读写语义有 tests/core/test_viz_plane.cpp 守着,跨进程一致性
// 有 tests/ipc/test_ipc_viz.cpp(VIZ-1/3/4)与 tests/core/test_monitor_harness.cpp 守着 ——
// 但**那三处的写方全是手搓 VizPublishInput 的 peer 或裸 VizPublisher**。真 Output 里
// `publishVizFrame()` 的那段装配(轨名 toStdString、leadMask/stereoMask 逐位、widthPct 取
// 活动版本的参数句柄、versionActive、playheadSnapshot、metaRevision 哈希)**没有任何用例
// 走过**。装配错一位——leadMask 取错字段、label 差一个下标、version 差一——段里就是错值,
// Monitor 画的就是错图,而上面那三处照样全绿。这正是 T37「数据面从未接线」那一族的形状。
//
// 判据落点 = 只读方 `VizPlane::attachReadOnly()` + `read()` 读回的帧,不碰 publisher 内部。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL-231:真 Output 的配置与曲线经 viz 段发布,只读方逐字段读回", "[host][sl231][viz][e2e]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // **先切到版本 2**:`VizPublishInput::versionActive` 与 `VizSnapshot::versionActive` 的结构体
    // 默认值都是 1,而 Rig 恒在版本 1 —— 不切版本的话,把装配里那行 `in.versionActive = ...`
    // 整行删掉,断言照样绿(PR #155 复审【重要】①)。切版本还会走 rebindVersion,
    // 顺带把「切版本 → 车道/句柄重绑」这一跳一起串上。
    r.out.setVersionActive(2);

    // 被观察轨:改配置 —— 这一份 runtime 就是 publishVizFrame 的输入。
    auto& cfg = r.out.runtime().channels[kTestChannel - 1];
    cfg.label = juce::String::fromUTF8("\xE4\xB8\xBB\xE5\x94\xB1 A"); // "主唱 A":走 toStdString 这一跳
    cfg.leadLock = true; // → leadMask 的 bit{ch-1}
    // 注:viz 装配**不读** configSeq(那是 ctrl 广播那一路的变化检测真源),故此处不动它。

    // 对照轨:除了**关掉 enabled** 之外一个字段都不动。没有它,「装配把同一份值填满所有轨」
    // 这类错误照样全绿。关 enabled 是为了让 onlineMask 那一格可证伪 —— Channel::enabled 默认
    // 全 true(`OutputRuntimeState::Channel::enabled` 的成员初始化),15 位恒满的话装配错位根本
    // 抓不到(复审【建议】1)。
    constexpr int kQuietCh = 10;
    REQUIRE(kQuietCh != kTestChannel);
    r.out.runtime().channels[kQuietCh - 1].enabled = false;

    // 给**版本 2** 的 width 一个和默认值不同的取值(默认 100)。装配取的是
    // `handles_.rawTrkW[v-1][ch]` —— **版本下标错一**就会读回版本 1 的 100,
    // 只断言「不是哨兵」抓不住那一位,断到具体数值才抓得住。
    constexpr float kWidthV2 = 42.0f;
    {
        auto* wp = r.out.getAPVTS().getParameter(scvb::params::widthId(2, kTestChannel));
        REQUIRE(wp != nullptr);
        wp->setValueNotifyingHost(wp->convertTo0to1(kWidthV2));
    }

    // 给被观察轨一条曲线真身(落在**刚切过去的版本 2** 上):没有曲线时车道恒哨兵,
    // 那样的断言证明不了装配跑通了。
    int replaced = 0;
    int locked = 0;
    REQUIRE(r.out.setTrackManual(kTestChannel, /*isPan=*/true, 55.0f, replaced, locked));

    // source_channels 的检测是**最终一致**的:refreshSourceChannels 要等音频段头写出来才回填,
    // waitUntilInjected 之后它可能还停在 0(未检测)或 1。这里尽力等它稳定 —— 但**不作硬前提**,
    // 下面的 stereoMask 断言与 runtime 当前值耦合,不管检测到哪一步都成立。
    for (int waited = 0; waited < 3000 && r.out.runtime().channels[kTestChannel - 1].sourceChannels != 2; waited += 60)
    {
        r.runBlocks(4, 0.25f, /*pumpEveryN=*/1, /*pumpMs=*/12);
    }

    // **在最后一轮发布之前**取检测值:放在 read() 之后取的话,万一等待循环超时后它才翻成 2,
    // 「最后一次发布 → read」之间就错开一拍,等式两侧对不上(复审【建议】3)。检测是单向落定的,
    // 所以下面用**单向蕴含**断言:已经检测成 stereo ⇒ 段里那一位必须置起。
    const bool srcIsStereo = (r.out.runtime().channels[kTestChannel - 1].sourceChannels == 2);

    // 让 Output 的 viz timer 真实发布若干帧(30Hz 闸门,400ms 足够十余帧)。
    r.runBlocks(60, 0.25f, /*pumpEveryN=*/2, /*pumpMs=*/12);
    Rig::pumpMessages(400);

    scvb::SegmentBackendWin32 backend;
    scvb::VizPlane probe(backend, static_cast<scvb::u32>(kTestGroup));
    REQUIRE(probe.attachReadOnly() == scvb::InitResult::kOk);

    auto frame = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(probe.read(*frame));

    // ── 帧身份与全局字段 ──
    CHECK(frame->sampleRate == static_cast<scvb::u32>(kSr));
    CHECK(frame->versionActive == 2u); // ← 不切版本的话两侧默认值都是 1,这条就没牙齿了

    // ── 逐轨装配:被观察轨 ──
    const auto bit = [](int ch) { return 1u << (ch - 1); };
    CHECK(frame->label[kTestChannel - 1] == std::string("\xE4\xB8\xBB\xE5\x94\xB1 A"));
    CHECK((frame->leadMask & bit(kTestChannel)) != 0u);

    // 车道:该轨有曲线 ⇒ 至少一列不是哨兵(全哨兵 = 车道这一跳没接上)。
    const auto& lane = frame->pan[kTestChannel - 1];
    const auto realCols = static_cast<std::size_t>(
        std::count_if(lane.begin(), lane.end(), [](std::int16_t v) { return v != scvb::kVizPanNone; }));
    CHECK(realCols > 0);

    // width:断到**具体数值**。句柄没接上 → NaN → 哨兵;版本下标错一 → 读回版本 1 的默认 100。
    // 两种都与 42 不等,一条断言同时挡住(这正是复审【重要】② 点名的那一族)。
    CHECK(frame->widthPct[kTestChannel - 1] ==
          scvb::vizPackFixed(static_cast<double>(kWidthV2), scvb::kVizWidthMin, scvb::kVizWidthMax));
    // stereo 检测值 → stereoMask;enabled 位 → onlineMask(见 VizPublisher::tick 里
    // `s.onlineMask = in.enabledMask` / `s.stereoMask = in.stereoMask` 两行)。
    // stereoMask **不硬写 true**:source_channels 由 refreshSourceChannels 从音频段头最终一致
    // 回填,硬写会把一条护栏变成时序炸弹(实测 —— 单独跑本用例时它停在 1,跟着 [analyze]
    // 一起跑就是 2)。用单向蕴含 + 下面对照轨那一位,两个错位方向都留着牙齿。
    INFO("sourceChannels(ch" << kTestChannel << ") = " << r.out.runtime().channels[kTestChannel - 1].sourceChannels);
    if (srcIsStereo)
    {
        CHECK((frame->stereoMask & bit(kTestChannel)) != 0u);
    }
    else
    {
        // 降级必须**留痕**:单向蕴含在 srcIsStereo 为假时一条断言都不执行,而 INFO 只在同
        // scope 有失败时才打印 —— 不留痕的话,「等待超时、这颗牙没咬上」的那次运行在 ctest
        // 输出里和完整跑过一模一样。这是本卡那条纪律(只认真的断言到的字段)的**运行期**版本。
        WARN("stereo 检测未在等待窗口内落定(sourceChannels != 2):本轮 stereoMask 未被断言");
    }
    CHECK((frame->onlineMask & bit(kTestChannel)) != 0u); // 该轨 enabled

    // ── 反向①:一个字段都没动的对照轨,不得被装配顺手填上 ──
    CHECK(frame->label[kQuietCh - 1].empty());
    CHECK((frame->leadMask & bit(kQuietCh)) == 0u);
    CHECK((frame->stereoMask & bit(kQuietCh)) == 0u); // 没有 Input 的轨不该被检测成 stereo
    CHECK((frame->onlineMask & bit(kQuietCh)) == 0u); // ← 刚把它 enabled 关掉了
    const auto& quietLane = frame->pan[kQuietCh - 1];
    CHECK(std::all_of(quietLane.begin(), quietLane.end(), [](std::int16_t v) { return v == scvb::kVizPanNone; }));

    // ── 播放头这一跳:帧里的 playheadSamples 随走带推进(装配读的是真 playheadSnapshot)──
    const std::int64_t firstPlayhead = frame->playheadSamples;
    r.runBlocks(60, 0.25f, /*pumpEveryN=*/2, /*pumpMs=*/12);
    Rig::pumpMessages(300);
    auto later = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(probe.read(*later));
    CHECK(later->playheadSamples > firstPlayhead);
    // 发布器确实**在持续发帧**,不是两次都读到同一帧(与 MON-CHAIN「写方停摆」同口径)。
    // 注:`seq % 2 == 0` 是 read() 自己的不变量,装配错成什么样它都绿,故不拿它当判据。
    CHECK(later->seq > frame->seq);

    // ── 反向②:段是**按组**开的。邻组没有写方 ⇒ 只读方拿空态,绝不建段(VIZ-2 同口径)。
    //    没有这一条,上面所有断言在「publisher 发错组」时也可能因残段而恰好成立。
    scvb::VizPlane wrongGroup(backend, static_cast<scvb::u32>(kNoWriterGroup));
    CHECK(wrongGroup.attachReadOnly() != scvb::InitResult::kOk);
}

// ---------------------------------------------------------------------------
// SL-231 ②:自动化打印链的末跳 —— **真 processor 的权威仲裁** → 打印器 → 宿主 gesture。
//
// 既有覆盖到哪一步:「gesture 该不该开」的**决策**有 tests/core/test_authority.cpp(对
// AuthorityMode 返回的标志位做的纯结构体单测);「打印器开不开 gesture」有
// tests/core/test_printer.cpp 的 PRINTER-GUARD 系列 —— 那里已经挂了真 JUCE 参数监听器
// (CountingListener::gestureBegins),所以**这一跳不是零覆盖**。
//
// 缺的是把它们串起来的那一段:test_printer.cpp 是直接 `printer.setMode(...)` + `printer.tick()`
// 驱动一个**独立的 AutomationPrinter fixture**,从不经过 ScvbOutputAudioProcessor 自己的
// 权威仲裁(`outputEnabled_ && playing && inRange` → Print/Armed/Follow,
// ScvbOutputAudioProcessor::timerCallback 里那段 `printer_.setMode(mode)` 之前的判定)、
// 车道绑定(bindVersion/setCurves)与 25Hz 真驱动。
// 于是「仲裁算错档 → 打印器根本没进 Print → 宿主自动化车道整条是空的」这一族,
// 两边的单测都照样全绿。
//
// 这一跳漏了会怎样:裸 setValueNotifyingHost 在 Cubase 这类宿主看来是一次没有起止的孤立
// 写入,要么被记成孤立自动化点,要么在 Read 档下当场把值顶回去(那样写入根本不生效)——
// 参数值断言全绿,用户那边自动化车道却是空的。这条真机现象记在
// docs/contract-changes/20260826-j85-freeze-param-plane.md。
//
// 判据落点 = 挂在**真 processor** 上的 juce::AudioProcessorListener 收到的回调,
// 不看打印器自己的 numGesturesOpen()。
// ---------------------------------------------------------------------------
namespace
{

// 宿主替身:只记 gesture 事件,并就地做配对校验。
struct GestureSpy final : juce::AudioProcessorListener
{
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}

    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int index) override
    {
        ++begins;
        if (!open.insert(index).second)
        {
            ++doubleBegin; // 同一参数连开两次 = 不配对(JUCE 侧 debug 也会 jassert)
        }
    }

    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int index) override
    {
        ++ends;
        if (open.erase(index) == 0)
        {
            ++orphanEnd; // 没 begin 就 end
        }
    }

    int begins = 0;
    int ends = 0;
    int doubleBegin = 0;
    int orphanEnd = 0;
    std::set<int> open;
};

} // namespace

TEST_CASE("HOST SL-231:打印器的 gesture 真的到达宿主且 begin/end 成对", "[host][sl231][print][e2e]")
{
    // **spy 必须比 rig 活得久**:~MonoMultiRig → releaseResources() → printer_.endAllGestures()
    // 会回调监听器。spy 声明在 rig 之后的话它先析构,一旦中途抛异常跳过 removeListener,
    // 那次回调就打在已析构对象上(UAF,表现成无法解释的崩溃而不是可读的失败)。
    GestureSpy spy;

    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // 采集**从 0 开始**:打印区间起点 lo = 首个段的起点 ≈ 采集开始时的播放头位置。不归零的话
    // lo ≈ waitUntilInjected 推过的距离,机器越慢它越大;一旦超过正向段跑的 1.28s,
    // 区间判定全程为假、begins 恒 0 —— 那是测试自己的时序没给够,不是被测物坏。
    r.ph.timeSamples = 0;

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);
    REQUIRE(r.runAnalysisToCompletion(coveredS, /*clearManual=*/false));

    // **先关输出再挂监听器**,原因有二:
    //   ① `outputEnabled_` 的**成员初始化值就是 true**(见 ScvbOutputAudioProcessor 的
    //      `bool outputEnabled_ = true;`),分析一出段表、走带一进
    //      区间,打印器当场就进 Print 并把 gesture 全开了 —— 不先清干净,下面「重新 begin」
    //      的计数会因为 gesture 早就开着(begin 是幂等的)而恒为 0;
    //   ② 关输出会走 Follow 分支的 endAllGestures,正好给出一个干净起点。
    r.out.setOutputEnabled(false);
    MonoMultiRig::pump(300);

    r.out.addListener(&spy);

    // ── 反向:输出关着(Follow 档,host 参数是权威)⇒ 仲裁不该给出 Print,一条都不该开 ──
    //    没有这一段,下面的 begins>0 证明不了是**仲裁 + 打印器**开的。
    r.ph.timeSamples = 0;
    r.runBlocks(60, 0.5f);
    MonoMultiRig::pump(300);
    CHECK(spy.begins == 0);

    // ── 正向:开输出 + 走带回到已分析区间 ⇒ 仲裁给出 Print,打印器包 gesture 写参数 ──
    r.out.setOutputEnabled(true);
    r.ph.timeSamples = 0;
    r.runBlocks(120, 0.5f);
    MonoMultiRig::pump(400);

    CHECK(spy.begins > 0); // ← 仲裁没进 Print、或末跳裸写不包 gesture,都停在 0
    CHECK(spy.doubleBegin == 0); // ← 同一参数连开两次(JUCE debug 侧也会 jassert)
    CHECK(spy.orphanEnd == 0); // ← 没 begin 就 end

    // ── 收尾:再关输出 ⇒ endAllGestures 必须把开着的全部闭合,不给宿主留悬空 gesture ──
    const int beginsBeforeClose = spy.begins;
    r.out.setOutputEnabled(false);
    MonoMultiRig::pump(400);

    CHECK(spy.open.empty());
    CHECK(spy.ends == spy.begins);
    CHECK(spy.begins == beginsBeforeClose); // 关输出不该再开新的

    r.out.removeListener(&spy);
}

// ===========================================================================
// [SL-233] owner.lock 的 10s 周期刷新(STATE_SCHEMA §4.3)—— 生产接线那一段。
//
// 契约的判活是「pid 存在 ∧ 心跳 < 30s」,而在这张卡之前,写 owner.lock 的地方只有
// 「保存工程」与「copy-on-write」两处。后果:一个开着工程但没再保存的实例,在它上次保存
// 30s 后就被判死 —— 后开的第二个实例读到死锁,于是**不**走 copy-on-write 而直接共享同一份
// sidecar,正是 CoW 要防的那一幕(用户「另存为」出两个工程、两个都开着,重采集互相覆盖)。
//
// SidecarStore 那一层的正反向在 FEAT-SIDECAR-7/8(注入时钟)。本用例只钉**接线**:
// 25Hz tick 到底有没有把这件事做起来 —— 那正是「实现里根本没有周期刷新点」漏掉的一环。
// ===========================================================================
namespace
{
// 独享临时 sidecar 根目录,结束即恢复(含异常路径),不碰开发机上真实的 %APPDATA% 会话目录。
struct TempSidecarRoot
{
    std::filesystem::path path;
    TempSidecarRoot()
    {
        path =
            std::filesystem::temp_directory_path() / ("scvb-sl233-" + std::to_string(scvb::state::epochMsNow()) + "-" +
                                                      std::to_string(juce::Random::getSystemRandom().nextInt(1000000)));
        std::filesystem::create_directories(path);
        ScvbOutputAudioProcessor::setSidecarBaseDirForTesting(path);
    }
    ~TempSidecarRoot()
    {
        ScvbOutputAudioProcessor::setSidecarBaseDirForTesting({});
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// 最小可解的特征夹具(2 通道 × 少量 hop);内容无关紧要,要的是「有一份能过 sha256 的 sidecar」。
scvb::state::FeaturesData makeTinyFeatures()
{
    scvb::state::FeaturesData d;
    d.sampleRate = 48000;
    d.hopMs = 10;
    d.vadPresent = false;
    for (std::uint8_t ch = 1; ch <= 2; ++ch)
    {
        scvb::state::ChannelFeatures c;
        c.channelId = ch;
        constexpr std::uint32_t kHops = 32;
        c.coverage.push_back(scvb::state::HopRange{0, kHops});
        for (std::uint32_t i = 0; i < kHops; ++i)
        {
            c.kwDbq.push_back(static_cast<std::int16_t>(-3000 + static_cast<int>(i)));
            c.peakDbq.push_back(static_cast<std::int16_t>(-2000 + static_cast<int>(i)));
        }
        d.channels.push_back(std::move(c));
    }
    return d;
}

// 把一份完整 blob 的 FEAT chunk 换成「指向 guid 的 sidecar 引用节」,让加载后的实例进入
// sidecar 模式(featuresInSidecar()==true)。与生产同一套编解码,不手搓字节。
std::vector<std::uint8_t> blobWithSidecarRef(const juce::MemoryBlock& src, const std::string& guid,
                                             const std::vector<std::uint8_t>& gz, const scvb::state::FeaturesData& data)
{
    scvb::state::StateChunks chunks;
    const auto res = scvb::state::loadState(static_cast<const std::uint8_t*>(src.getData()), src.getSize(), chunks);
    REQUIRE(res.status == scvb::state::StateLoadStatus::Ok);

    scvb::state::SidecarRef ref;
    ref.sessionGuid = guid;
    ref.sha256 = scvb::state::sha256(gz.data(), gz.size());
    ref.sidecarBytes = static_cast<std::uint64_t>(gz.size());
    auto refSection =
        scvb::state::encodeReference(ref, data.sampleRate, data.hopMs, static_cast<std::uint8_t>(data.channels.size()));
    REQUIRE_FALSE(refSection.empty());
    chunks.set(scvb::state::kFourccFeat, std::move(refSection));

    std::vector<std::uint8_t> out;
    REQUIRE(scvb::state::encodeContainer(chunks, out));
    return out;
}

// 读 owner.lock 的心跳(不存在则返回 0)。
std::uint64_t heartbeatOf(const std::filesystem::path& base, const std::string& guid)
{
    scvb::state::OwnerLock lock;
    if (!scvb::state::SidecarStore(base).readOwnerLock(guid, lock))
    {
        return 0;
    }
    return lock.heartbeatEpochMs;
}
} // namespace

TEST_CASE("HOST SL-233:sidecar 模式下 25Hz tick 周期刷新 owner.lock", "[host][v56][SL233]")
{
    TempSidecarRoot root;
    scvb::state::SidecarStore store(root.path);
    const auto data = makeTinyFeatures();
    const auto gz = scvb::state::encodeFeatures(data);
    REQUIRE_FALSE(gz.empty());

    const auto self = scvb::state::currentProcessIdentity();
    REQUIRE(self.pid != 0u); // 前置:Windows 上拿得到进程身份,否则续租按设计一律拒绝

    // ---- 造一份「本进程持有」的 sidecar;心跳随后倒拨,验证 tick 有没有把它续回来。----
    std::string guid;
    juce::MemoryBlock baseBlob;
    {
        ScvbOutputAudioProcessor seed;
        guid = seed.sessionGuid().toStdString();
        seed.getStateInformation(baseBlob);
    }
    REQUIRE(store.write(guid, gz.data(), gz.size(), static_cast<std::uint32_t>(data.channels.size()),
                        scvb::state::kFeatCodecVer, self));

    const auto backdate = [&](std::int64_t agoMs) {
        scvb::state::OwnerLock lock;
        REQUIRE(store.readOwnerLock(guid, lock));
        lock.heartbeatEpochMs = static_cast<std::uint64_t>(scvb::state::epochMsNow() - agoMs);
        REQUIRE(store.writeOwnerLock(guid, lock));
        return lock.heartbeatEpochMs;
    };

    const std::vector<std::uint8_t> sidecarBlob = blobWithSidecarRef(baseBlob, guid, gz, data);

    // ---- ① 反向验证:**不在** sidecar 模式的实例不去碰 owner.lock。----
    //      关键是这个实例的 sessionGuid_ 必须**就是** guid 且锁**归它自己** —— 否则
    //      「没刷新」也可能只是因为它指着别的 guid、或者闸②「无锁不新建」拦下的,
    //      把 featuresSidecar_ 门控整个删掉用例照样过,反向验证就是空的(PR #154 复审【建议】1)。
    {
        const std::uint64_t before = backdate(20000);
        Rig r;
        // baseBlob 的 PRMS 带着 guid、FEAT 是内嵌(0 字节)→ 同一个 guid,但不在 sidecar 模式。
        r.out.setStateInformation(baseBlob.getData(), static_cast<int>(baseBlob.getSize()));
        REQUIRE(r.out.sessionGuid().toStdString() == guid); // 前置:指的就是这把锁
        REQUIRE_FALSE(r.out.featuresInSidecar()); // 前置:确实不在 sidecar 模式
        scvb::state::OwnerLock owned;
        REQUIRE(store.readOwnerLock(guid, owned));
        REQUIRE(owned.pid == self.pid); // 前置:锁归本进程,唯一拦得住它的只有 sidecar 门控
        Rig::pumpMessages(400); // 远超一拍 40ms;首拍就该做完决定
        CHECK(heartbeatOf(root.path, guid) == before); // 一个字节都没动
    }

    // ---- ② 正向:加载一份走 sidecar 的工程 → tick 把心跳续到「刚刚」。----
    std::uint64_t refreshed = 0;
    {
        const std::uint64_t before = backdate(20000);
        Rig r;
        r.out.setStateInformation(sidecarBlob.data(), static_cast<int>(sidecarBlob.size()));
        REQUIRE(r.out.featuresInSidecar()); // 前置:引用节确实解开了(sha256 过)
        REQUIRE(r.out.sessionGuid().toStdString() == guid);

        Rig::pumpMessages(400);
        refreshed = heartbeatOf(root.path, guid);
        CHECK(refreshed > before); // 续上了

        scvb::state::OwnerLock lock;
        REQUIRE(store.readOwnerLock(guid, lock));
        CHECK(scvb::state::isOwnerLockAlive(lock, scvb::state::epochMsNow()));
        CHECK(lock.pid == self.pid); // 续租只动时间戳,持有者原样
        CHECK(lock.processStartEpochMs == self.processStartEpochMs);

        // 契约效果:此刻第二个实例(别的 pid)来开同一份工程,判活 → 走 copy-on-write。
        scvb::state::ProcessIdentity other;
        other.pid = self.pid + 1u;
        other.processStartEpochMs = self.processStartEpochMs + 1u;
        other.hostName = "other";
        std::string newGuid;
        CHECK(store.copyOnWriteIfNeeded(guid, other, scvb::state::epochMsNow(), newGuid));
        CHECK(newGuid != guid);
    }

    // ---- ③ 反向验证:锁归他人时,tick 绝不把它抢过来(CoW 的判据不能被自己刷掉)。----
    {
        scvb::state::OwnerLock foreign;
        foreign.pid = self.pid + 4242u;
        foreign.processStartEpochMs = self.processStartEpochMs + 4242u;
        foreign.heartbeatEpochMs = static_cast<std::uint64_t>(scvb::state::epochMsNow() - 20000);
        foreign.heartbeatIso8601 = "1970-01-01T00:00:00Z";
        REQUIRE(store.writeOwnerLock(guid, foreign));

        Rig r;
        r.out.setStateInformation(sidecarBlob.data(), static_cast<int>(sidecarBlob.size()));
        REQUIRE(r.out.featuresInSidecar());
        Rig::pumpMessages(400);

        scvb::state::OwnerLock after;
        REQUIRE(store.readOwnerLock(guid, after));
        CHECK(after.pid == foreign.pid); // 持有者没被改
        CHECK(after.heartbeatEpochMs == foreign.heartbeatEpochMs); // 心跳没被刷
        CHECK(after.heartbeatIso8601 == foreign.heartbeatIso8601);
    }

    // ---- ③b 加载期认领:盘上是**上一会话留下的死锁**(最常见的一幕 —— 打开一份昨天存的工程)。
    //      本实例加载后认领它、tick 续上,第二份副本再打开时才判活并走 CoW。
    //      没有这一步,「只有写过 sidecar 的实例才持锁」会让续租的所有权前提永远不成立。----
    {
        scvb::state::OwnerLock dead;
        dead.pid = self.pid + 9999u; // 上一会话的进程,早已退出
        dead.processStartEpochMs = self.processStartEpochMs + 9999u;
        dead.heartbeatEpochMs =
            static_cast<std::uint64_t>(scvb::state::epochMsNow() - scvb::state::kOwnerLockAliveHeartbeatMs - 60000);
        dead.heartbeatIso8601 = "1970-01-01T00:00:00Z";
        REQUIRE(store.writeOwnerLock(guid, dead));
        REQUIRE_FALSE(scvb::state::isOwnerLockAlive(dead, scvb::state::epochMsNow())); // 前置:确实是死锁

        // 反向验证(修复前的行为):死锁摆在那儿,第二实例判死 → 不 CoW,直接共享。
        scvb::state::ProcessIdentity other;
        other.pid = self.pid + 1u;
        other.processStartEpochMs = self.processStartEpochMs + 1u;
        other.hostName = "other";
        std::string shared;
        REQUIRE_FALSE(store.copyOnWriteIfNeeded(guid, other, scvb::state::epochMsNow(), shared));

        Rig r;
        r.out.setStateInformation(sidecarBlob.data(), static_cast<int>(sidecarBlob.size()));
        REQUIRE(r.out.featuresInSidecar());

        scvb::state::OwnerLock claimed;
        REQUIRE(store.readOwnerLock(guid, claimed));
        CHECK(claimed.pid == self.pid); // 加载期就认领下来了(sha256 过之后才认)
        CHECK(scvb::state::isOwnerLockAlive(claimed, scvb::state::epochMsNow()));

        Rig::pumpMessages(400); // tick 继续续租
        scvb::state::OwnerLock afterTick;
        REQUIRE(store.readOwnerLock(guid, afterTick));
        CHECK(afterTick.pid == self.pid);
        CHECK(scvb::state::isOwnerLockAlive(afterTick, scvb::state::epochMsNow()));

        // 正向:此刻第二实例来开同一份工程 → 判活 → 走 CoW(这正是 SL-233 要根治的那一幕)。
        std::string newGuid;
        CHECK(store.copyOnWriteIfNeeded(guid, other, scvb::state::epochMsNow(), newGuid));
        CHECK(newGuid != guid);
    }

    // ---- ④ 实例关掉后不再有人续租 → 30s 后按契约判死(租约不是永久的)。----
    scvb::state::OwnerLock lease;
    lease.pid = self.pid;
    lease.processStartEpochMs = self.processStartEpochMs;
    lease.heartbeatEpochMs = refreshed;
    CHECK_FALSE(scvb::state::isOwnerLockAlive(lease, static_cast<std::int64_t>(refreshed) +
                                                         scvb::state::kOwnerLockAliveHeartbeatMs));
}

// ===========================================================================
// [SL-234] CFGS 的 ui.scale 在**加载期**也要夹取。
//
// §7.3:setStateInformation 处理的是用户工程文件里的不可信字节,范围字段必须先校验再用。
// 此前夹取只在桥面 setUiScale 那一处 —— 手改过 / 被别的工具写坏 / 跨版本的工程带一个
// 0 或几万的 uiScale,会被原样吃进去,再由「设计盒 × 档位」算出一个 0 像素或几万像素的窗口。
// 两个插件同一处缺口,一并钉住。
// ===========================================================================
namespace
{
// 把一份完整 blob 里的 CFGS.uiScale 换成指定值(经生产 codec 解→改→编,不手搓字节)。
std::vector<std::uint8_t> blobWithOutputUiScale(const juce::MemoryBlock& src, std::uint32_t percent)
{
    scvb::state::StateChunks chunks;
    const auto res = scvb::state::loadState(static_cast<const std::uint8_t*>(src.getData()), src.getSize(), chunks);
    REQUIRE(res.status == scvb::state::StateLoadStatus::Ok);
    const auto* cfgs = chunks.find(scvb::state::kFourccCfgs);
    REQUIRE(cfgs != nullptr);

    scvb::state::OutputState s;
    REQUIRE(scvb::state::decodeOutputState(cfgs->payload.data(), cfgs->payload.size(), s));
    s.uiScale = percent;
    std::vector<std::uint8_t> payload;
    REQUIRE(scvb::state::encodeOutputState(s, payload));
    chunks.set(scvb::state::kFourccCfgs, std::move(payload));

    std::vector<std::uint8_t> out;
    REQUIRE(scvb::state::encodeContainer(chunks, out));
    return out;
}

std::vector<std::uint8_t> blobWithInputUiScale(const juce::MemoryBlock& src, std::uint32_t percent)
{
    scvb::state::StateChunks chunks;
    const auto res = scvb::state::loadState(static_cast<const std::uint8_t*>(src.getData()), src.getSize(), chunks);
    REQUIRE(res.status == scvb::state::StateLoadStatus::Ok);
    const auto* cfgs = chunks.find(scvb::state::kFourccCfgs);
    REQUIRE(cfgs != nullptr);

    scvb::state::InputState s;
    REQUIRE(scvb::state::decodeInputState(cfgs->payload.data(), cfgs->payload.size(), s));
    s.uiScale = percent;
    std::vector<std::uint8_t> payload;
    REQUIRE(scvb::state::encodeInputState(s, payload));
    chunks.set(scvb::state::kFourccCfgs, std::move(payload));

    std::vector<std::uint8_t> out;
    REQUIRE(scvb::state::encodeContainer(chunks, out));
    return out;
}
} // namespace

TEST_CASE("HOST SL-234:加载期 CFGS.uiScale 越界值夹到边界", "[host][v56][SL234]")
{
    const int lo = juce::roundToInt(scvb::bridge::plugin::MinUiScale * 100.0f);
    const int hi = juce::roundToInt(scvb::bridge::plugin::MaxUiScale * 100.0f);

    // 不碰开发机真实全局默认:构造期会读它,而本用例要的是「工程里那个值走完加载路径」。
    struct TempStore
    {
        juce::File dir = juce::File::createTempFile("scvb-sl234")
                             .getSiblingFile("scvb-sl234-" + juce::String(juce::Random::getSystemRandom().nextInt64()));
        TempStore()
        {
            dir.createDirectory();
            scvb::uidefaults::setStorageDirForTesting(dir);
        }
        ~TempStore()
        {
            scvb::uidefaults::setStorageDirForTesting({});
            dir.deleteRecursively();
        }
    } store;

    juce::MemoryBlock outBase;
    juce::MemoryBlock inBase;
    {
        ScvbOutputAudioProcessor a;
        a.getStateInformation(outBase);
        ScvbInputAudioProcessor b;
        b.getStateInformation(inBase);
    }

    // ---- ① Output:越界值分别夹到上/下界。0xFFFFFFFF 是**大**值,必须夹到上界 ——
    //      先 static_cast<int> 再夹会把它折成 -1、"恰好"落到下界,那是溢出撞对的。----
    const std::vector<std::pair<std::uint32_t, int>> outOfRange{{0u, lo}, {1u, lo}, {5000u, hi}, {0xFFFFFFFFu, hi}};
    for (const auto& [stored, expect] : outOfRange)
    {
        const auto blob = blobWithOutputUiScale(outBase, stored);
        ScvbOutputAudioProcessor a;
        a.setStateInformation(blob.data(), static_cast<int>(blob.size()));
        CHECK(a.uiScalePercent() == expect);
    }

    // ---- ② 反向验证:区间内的值原样加载(不是「一律夹成边界」把用户档位吃掉)。----
    for (const int ok : {lo, 100, 125, hi})
    {
        const auto blob = blobWithOutputUiScale(outBase, static_cast<std::uint32_t>(ok));
        ScvbOutputAudioProcessor a;
        a.setStateInformation(blob.data(), static_cast<int>(blob.size()));
        CHECK(a.uiScalePercent() == ok);
    }

    // ---- ③ Input 侧同一处缺口(加载期不夹取,只在桥面夹),正反向同款。----
    const std::vector<std::pair<std::uint32_t, int>> inOutOfRange{{0u, lo}, {5000u, hi}, {0xFFFFFFFFu, hi}};
    for (const auto& [stored, expect] : inOutOfRange)
    {
        const auto blob = blobWithInputUiScale(inBase, stored);
        ScvbInputAudioProcessor b;
        b.setStateInformation(blob.data(), static_cast<int>(blob.size()));
        CHECK(b.bridgeUiScalePercent() == expect);
    }
    for (const int ok : {lo, 100, hi})
    {
        const auto blob = blobWithInputUiScale(inBase, static_cast<std::uint32_t>(ok));
        ScvbInputAudioProcessor b;
        b.setStateInformation(blob.data(), static_cast<int>(blob.size()));
        CHECK(b.bridgeUiScalePercent() == ok);
    }
}

// ===========================================================================
// [SL-240] 泳道绿线「有时有有时没,而且**播放到哪消失到哪**」(用户 v5.6.2 实测,
// Cubase 15 Pro)。
//
// 定谳:`FeatRing` 写侧的 hop 是**时间线序号**(`FeatRunState::nextHop`,回卷 seek 起
// 新 run 时按播放头重算),所以「采集开着又放一遍同一段」= 拿同样的 hop 号再写一遍。
// 而 SL-206 在 `ChannelFrames::write()` 里**无条件**清 vadP(「新特征进来 = 旧判决
// 作废」),于是每重播一次,已分析段的绿线就被播放头一路抹过去 —— 「有时有有时没」
// = 分析完之后有没有再播过。
//
// 修法见 `FrameStore.cpp` 里那段注释:只在数据**真被换掉**时作废(该 hop 还没覆盖 /
// 布防重采集期)。下面两例分守两边 —— 一例守「重播不许抹」,一例守「布防重采要抹」。
// 两例都断**真机数据面**(waveformOf 的 vad 列),不碰 mock:vadP 正是 mock 说谎的
// 前科之一(SL-206 头注记的第三次)。
// ===========================================================================
namespace
{
/** 该轨 [a,b) 上被判有声的瓦片列数(泳道绿线的直接判据)。 */
int voicedColsOf(ScvbOutputAudioProcessor& out, double a, double b, int cols)
{
    const auto tile = out.waveformOf(kTestChannel, a, b, cols);
    int v = 0;
    for (int i = 0; i < cols; ++i)
    {
        v += tile.vad[static_cast<std::size_t>(i)] ? 1 : 0;
    }
    return v;
}

/** 采一段有声有静交替的素材(两例共用同一份素材形状)。 */
void captureBursts(Rig& r, int bursts = 8)
{
    for (int i = 0; i < bursts; ++i)
    {
        r.runBlocks(60, 0.5f, 4, 4); // 有声
        r.runBlocks(40, 0.0f, 4, 4); // 静音
    }
}

/** 等分析跑完(与本文件其余分析用例同一等法)。 */
void waitAnalysis(Rig& r)
{
    for (int waited = 0; waited < 20000; waited += 50)
    {
        Rig::pumpMessages(50);
        if (!r.out.analysisRunning() && !r.out.runtime().analysisRunning)
        {
            break;
        }
    }
}
} // namespace

TEST_CASE("HOST SL-240:分析完再放一遍,已分析段的绿线不许被播放抹掉", "[host][t37][v56][SL240]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // ① 采集 → 分析 → 绿线出来。
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    const std::int64_t t0 = r.ph.timeSamples;
    captureBursts(r);
    Rig::pumpMessages(400);

    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS;
    REQUIRE(coveredS > 2.0);
    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS).ok);
    waitAnalysis(r);
    REQUIRE_FALSE(r.out.analysisRunning());

    const double half = coveredS * 0.5;
    const int voicedBefore = voicedColsOf(r.out, 0.0, half, 64);
    REQUIRE(voicedBefore > 0); // 前置:绿线确实画出来了

    // ② 在**尾巴**上打个洞。这个洞与断言区(前半段)不相交,它只有一个用途:
    //    给下面「重播确实落账了」一条**不依赖 vadP** 的硬证据 —— 洞被重新填上,
    //    就说明 FeatPuller 真的把这一段的 hop 又拉了一遍、write() 真的跑过。
    //    没有它的话,「绿线还在」有两条通过路径:想断的那条是 write() 保住了判决,
    //    不想要的那条是重播压根没落账(playhead 回跳后整批被跳过)—— 后者同样绿,
    //    这一例就再也钉不住那几行。同一个坑 SL-206 那例记过一次。
    const double holeStartS = coveredS * 0.9;
    const auto mask = static_cast<std::uint16_t>(1u << (kTestChannel - 1));
    REQUIRE(r.out.clearCoverage(mask, holeStartS, coveredS) > 0.0);
    Rig::pumpMessages(200);
    REQUIRE(r.out.coverageOf(kTestChannel, holeStartS, coveredS).coveredS == Catch::Approx(0.0));

    // ③ 回到原处,**采集仍开着**,把同一段再放一遍(= 用户按下播放)。
    //    不清覆盖、不布防重采 —— 这就是「又听了一遍」而已。
    r.ph.timeSamples = t0;
    captureBursts(r);
    Rig::pumpMessages(400);

    // 硬前置:洞被填回来了 ⇒ 这一段的 write() 确实又跑了一遍。
    REQUIRE(r.out.coverageOf(kTestChannel, holeStartS, coveredS).coveredS > 0.0);

    // ★ 核心:前半段(没打洞、没重采)的绿线必须原样还在。
    //   修复前这里是 0 —— 播放头走到哪,vadP 就被抹到哪。
    CHECK(voicedColsOf(r.out, 0.0, half, 64) == voicedBefore);
}

TEST_CASE("HOST SL-240:布防重采集期重采,旧绿线仍须作废", "[host][t37][v56][SL240]")
{
    // 上一例把「无条件清」摘掉之后,布防重采这一路会跟着丢 —— §1.23 明写布防
    // **保留既有覆盖**(门控只挡写入),所以 write() 里那条「没覆盖才清」看不见它。
    // 这一例守住那条显式支路(OutputProcessor → OutputSession → ChannelFrames)。
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    const std::int64_t t0 = r.ph.timeSamples;
    captureBursts(r);
    Rig::pumpMessages(400);

    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 30.0).coveredS;
    REQUIRE(coveredS > 2.0);
    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS).ok);
    waitAnalysis(r);
    REQUIRE_FALSE(r.out.analysisRunning());
    // 断言窗口整段前移 0.2s,理由见文末那条 CHECK 的注释(回放装置的 run 起点边)。
    constexpr double kEdgeS = 0.2;
    REQUIRE(voicedColsOf(r.out, kEdgeS, coveredS, 64) > 0); // 前置:绿线出来了

    // 布防整段重采(覆盖按契约原样留着),回到原处重放一段**纯静音**。
    //
    // 布防右界给 2s 余量:`coveredS` 是「覆盖了多少秒」而不是「覆盖到几秒」,而布防门是
    // `toHop(endS)` 截断的**半开**区间 —— 正好卡在右界上的那个 hop 会落在门外、旧判决
    // 留着,于是瓦片最后一列仍判有声。本机实测没撞上(唯一的残留在**左**边界,见文末),
    // 这 2s 是不让它在别的机器上变成偶发红。
    const auto mask = static_cast<std::uint16_t>(1u << (kTestChannel - 1));
    r.out.armRecapture(mask, 0.0, coveredS + 2.0, /*autoStop=*/false);
    Rig::pumpMessages(200);
    REQUIRE(r.out.coverageOf(kTestChannel, 0.0, coveredS).coveredS > 0.0); // 布防不清覆盖

    r.ph.timeSamples = t0;
    for (int burst = 0; burst < 8; ++burst)
    {
        r.runBlocks(100, 0.0f, 4, 4);
    }
    Rig::pumpMessages(400);

    // ★ 换了素材:重采到的那一整段绿线必须已经作废(没再分析过,不该照着旧素材继续画)。
    //
    // 窗口从 0.2s 起、不从 0 起:回卷 seek 起的**新 run** 与第一遍「采集开关翻 ON」起的那个
    // run,hop 栅格对齐点不一样(`pendingSkip` 各自吃掉一段不足一 hop 的余数),于是开头
    // 有几个 hop 这一遍压根没被重写,旧判决自然留着。实测**就 1 个 hop**(853 列的瓦片里
    // 第 3 列,≈30ms)。那是**用例这个回放装置**的边,不是产品的边 —— 真机里用户按停再播,
    // 两个 run 的起点本来就不必对齐。kEdgeS = 20 个 hop 的余量,把它整个让出去。
    CHECK(voicedColsOf(r.out, kEdgeS, coveredS, 64) == 0);
}

// ===========================================================================
// [SL-242] 段级「恢复自动」的作用域 —— 只动选中那一段。
//
// 用户实测(v5.6.2 终验 A7,Cubase 15 Pro):点一个**小片段**的「恢复自动」,结果
// 整个大片段被合并、全部回了自动。定谳两条独立成因:
//   ① web 侧:检查器那枚钮发的是**轨级** scope(`{tracksMask}`,不带范围)——
//      整轨的手动段一次清光(修在 tab-wave.js 的 segmentRestoreScope);
//   ② native 侧:范围 → hop 窗**截断**取整,`cfg.rangeStartSample` 会落到 startS
//      之前最多一个 hop(10ms)。applyAnalysisSegments 判 `outsideRange` 读的就是
//      这个数,于是紧贴范围左边、`t1 == startS` 的那一段被判成「与范围相交」而删掉;
//      本次分析只产出 [rangeStart, rangeEnd) 内的段,补不回它 ⇒ **左邻段整段消失**。
//      10ms 的重叠毁掉一个任意长的段,用户看到的就是「大片段被合并」。
//
// 本用例守的是 ②(①在 web 冒烟侧守)。够得着 ② 的前提是段边界**非 hop 对齐** ——
// 那正是用户编辑过的边界(split / move_boundary,§5.4),所以这里先切一刀。
// split 的后置是两子段 locked=true(锁定段本就免疫),故还要显式解锁,把「origin
// 这层保护被 clearManual 放开、锁那层不在」这个真正暴露的形状造出来。
//
// 反向验证:把 AnalyzeScopeMath.h 的 `analyzeHopWindow` 改回截断
// (`std::floor(... / hopS)` 取 firstHop),③ 的左邻段断言必红。
// ===========================================================================
TEST_CASE("HOST [SL-242] 段级 clearManual 不得吃掉左邻段", "[host][t37][analyze][SL242]")
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
    // 「一个字节都没变」的判据:五个字段全等(t0/t1/flags/pan/volDb)。只比 t0/t1 会漏掉
    // 「边界没动但 origin 被清了」,只比 flags 会漏掉「段还在但被挪了」,不比值会漏掉
    // 「段还在、origin 也对,但 pan/vol 被本轮产出改写了」。
    const auto sameSeg = [](const scvb::state::Segment& a, const scvb::state::Segment& b) {
        return a.t0 == b.t0 && a.t1 == b.t1 && a.flags == b.flags && std::abs(a.pan - b.pan) < 1e-6f &&
               std::abs(a.volDb - b.volDb) < 1e-6f;
    };

    // ---- ① 全量分析一遍,拿一张 auto 段表(边界都是 hop 对齐的)。----
    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS, /*clearManual=*/false).ok);
    waitDone();
    REQUIRE(segsOf().size() >= 1);

    // ---- ② 在首段里切一刀,造出「大自动段内的小手动段」。----
    // 切点刻意 **+7 样本**:非 hop 对齐(48k 下一个 hop = 480 样本)。对齐的切点
    // 走不到本用例的靶子 —— 那正是这个缺陷在 DAW 里难现形的原因。
    const auto seed = segsOf();
    const std::int64_t span = seed[0].t1 - seed[0].t0;
    REQUIRE(span > 4800); // 至少 100ms,切完两半都还够一个 hop
    const std::int64_t cut = seed[0].t0 + span / 2 + 7;
    REQUIRE(cut % 480 != 0); // 门槛本身要成立,否则这条用例什么都没证明

    scvb::state::SegmentEditArgs split;
    split.op = scvb::state::SegmentEditOp::Split;
    split.segIdx = 0;
    split.tSamples = cut;
    // ⚠ editSegment 的 track 是 0 基(桥面也是 `ch - 1` 再进来),与 1 基的
    //   setTrackManual(ch) 不同口径 —— 传错不报错,只静默改到隔壁轨。
    REQUIRE(r.out.editSegment(kTestChannel - 1, split) == scvb::state::SegmentEditResult::Ok);

    // 两子段都解锁:split 的后置是 locked=true,而锁定段对 clearManual 免疫(§1.6),
    // 锁着就走不到「origin 保护被放开」那条真正会删段的路。
    for (int idx : {0, 1})
    {
        scvb::state::SegmentEditArgs unlock;
        unlock.op = scvb::state::SegmentEditOp::SetLocked;
        unlock.segIdx = idx;
        unlock.locked = false;
        REQUIRE(r.out.editSegment(kTestChannel - 1, unlock) == scvb::state::SegmentEditResult::Ok);
    }

    const auto before = segsOf();
    REQUIRE(before.size() >= 2);
    REQUIRE(before[0].t1 == cut);
    REQUIRE(before[1].t0 == cut);
    REQUIRE_FALSE(scvb::state::segmentLocked(before[0].flags));
    REQUIRE_FALSE(scvb::state::segmentLocked(before[1].flags));

    // ---- ③ 只对**右边那个小段**做「恢复自动」。----
    const double segStartS = static_cast<double>(before[1].t0) / kSr;
    const double segEndS = static_cast<double>(before[1].t1) / kSr;
    const std::uint16_t mask = static_cast<std::uint16_t>(1u << (kTestChannel - 1));
    REQUIRE(r.out.startAnalysis(mask, segStartS, segEndS, /*clearManual=*/true).ok);
    waitDone();

    const auto after = segsOf();

    // ③-a 左邻段(以及一切完全落在范围外的段)必须**逐字节**还在。
    //      ← 修复前左邻段被整段删掉:hop 窗左沿落在 cut 之前,它被判成「相交」。
    for (const auto& b : before)
    {
        if (!(b.t1 <= before[1].t0 || b.t0 >= before[1].t1))
        {
            continue; // 与范围相交的段本就该被本次重算取代
        }
        const bool survived =
            std::any_of(after.begin(), after.end(), [&](const scvb::state::Segment& x) { return sameSeg(x, b); });
        INFO("范围外的段被动了:t0=" << b.t0 << " t1=" << b.t1);
        CHECK(survived);
    }

    // ③-b 范围内那一段真的回了自动:范围里不许再留 origin≠auto 的段。
    //      (本次产出可能是 0 段 —— VAD 判静音时段表就该空着,故不断言段数。)
    for (const auto& x : after)
    {
        if (x.t1 <= before[1].t0 || x.t0 >= before[1].t1)
        {
            continue;
        }
        CHECK(scvb::state::segmentOrigin(x.flags) == scvb::state::SegmentOrigin::Auto);
    }

    // ③-c 反向:段数不许塌成「整轨一段」。用户报的现象是「整个大片段被合并」,
    //      这条钉的就是那个观感 —— 范围外的段一个不少。
    const std::size_t outsideBefore =
        static_cast<std::size_t>(std::count_if(before.begin(), before.end(), [&](const scvb::state::Segment& b) {
            return b.t1 <= before[1].t0 || b.t0 >= before[1].t1;
        }));
    const std::size_t outsideAfter =
        static_cast<std::size_t>(std::count_if(after.begin(), after.end(), [&](const scvb::state::Segment& x) {
            return x.t1 <= before[1].t0 || x.t0 >= before[1].t1;
        }));
    CHECK(outsideAfter == outsideBefore);
}

// ===========================================================================
// [SL-239] v5.6.2 实测 P1 仍在:#146 合入后,用户「改狠上游 EQ → ⚠ 重采提醒」**依旧
// 不出现**。本组两条把定谳钉住 —— **指纹链一条都没断,断的是「01 采集开着时整条功能
// 是哑的」,而用户的自然流程正好把采集留在 ON**。
//
// 为什么 #146 的 e2e 全绿而用户仍不触发(SL-231 盘点要回答的那个问题):
// `HOST SL-225` 三条用例**每一条在问 stale 之前都显式 `setCaptureEnabled(false)`**,
// 唯一留着 ON 的那个 SECTION 断的是 `CHECK_FALSE` —— 它把「采集 ON 所以没提示」
// **当成正确行为钉住了**。于是那批用例覆盖的是「采集已关」这个前提下的链路健康,
// 而用户的前提恰恰是采集没关(终验清单 A1/B13 全文没有一句「先关掉 01 采集」,
// 产品里也没有任何地方提示要关)。
//
// 另有一处**盲区**同批补上:SL-225 的「重开工程」是**同实例 setStateInformation**,
// FrameStore 的基线全程留在内存里,FEAT→FrameStore 那一跳从没被走过。下面第一条改用
// SL-226 那条真路径(Rig 析构 + 全新实例),把「基线真的随工程回来了」也一并钉住。
// ===========================================================================

// ① 用户场景的整链 e2e:采集 → 存 → **关工程** → 重开 → 改狠上游 → ⚠。
//    判据取 `captureStale`(引擎侧真值,链末端),不看内部计数器。
TEST_CASE("HOST SL-239:工程重开(全新实例)后上游改动仍须翻出 stale", "[host][v562][SL239]")
{
    // 采一段基线,由用户自己把采集关掉(= 他要存进工程的状态),再保存。
    const auto layBaselineAndSave = [](juce::MemoryBlock& blob) {
        Rig r;
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());

        r.out.setCaptureEnabled(true);
        r.ph.timeSamples = 0;
        r.runBlocks(760, 0.5f); // ≈8.1s
        Rig::pumpMessages(400);
        REQUIRE(r.out.coverageOf(kTestChannel, 0.0, 7.0).coveredS > 5.0);

        r.out.setCaptureEnabled(false);
        Rig::pumpMessages(200);
        r.out.getStateInformation(blob);
        REQUIRE(blob.getSize() > 0);
    }; // Rig 在这里析构 = 关工程,内存里的 FrameStore 连同两个实例一起没了

    // 重开工程(全新实例)后回到同一段时间线播一遍,问 stale。
    const auto reopenThenPlay = [](const juce::MemoryBlock& blob, float amplitude) {
        Rig r2;
        REQUIRE(r2.out.coverageOf(kTestChannel, 0.0, 7.0).coveredS == 0.0); // 加载前是空的
        r2.out.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
        Rig::pumpMessages(200);

        REQUIRE_FALSE(r2.out.captureEnabled()); // 采集态 = 用户自己存的 OFF
        // 基线真的随工程回来了(FEAT → FrameStore)。没有这一条,下面断言不出任何东西:
        // 基线缺席时 baselineTileFingerprint 返回 false,上报会走「无基线」分支整条跳过。
        REQUIRE(r2.out.coverageOf(kTestChannel, 0.0, 7.0).coveredS > 5.0);

        r2.ph.playing = true;
        REQUIRE(r2.waitUntilInjected());
        r2.ph.timeSamples = 0;
        r2.runBlocks(760, amplitude);
        Rig::pumpMessages(600);
        return r2.out.captureStale(kTestChannel);
    };

    juce::MemoryBlock blob;
    layBaselineAndSave(blob);

    // ★ 改狠上游 EQ = 同一段时间线上喂差一个数量级的素材 ⇒ ⚠ 必须回来。
    CHECK(reopenThenPlay(blob, 0.05f));

    // ★ 反向:素材一个字节没改(同振幅)⇒ 绝不许报。这一条同时是 FEAT 回灌保真度的判据 ——
    //   落盘存的就是量化后的 int16 dBq,回灌若有任何一位不精确,这里就会整轨误报。
    CHECK_FALSE(reopenThenPlay(blob, 0.5f));
}

// ② 断口本身:**采集 ON 期间整条提示是哑的**,而且机会**一次性消耗**。
//
// 这一条不是在钉「正确行为」,是在钉「用户看不见的那件事真的会发生」—— 它是本卡 web 侧
// 那条提示横幅的存在理由,横幅文案改了就该回来看这里。
//
// ⚠ 三条断言里前两条是 CHECK_FALSE(空转也会绿),所以第三条**必须**留着:同一个 rig 上
// 只把采集关掉、换第三种素材,⚠ 必须出现。三条合起来才说明「刚才没提示是因为采集 ON
// 且基线已被刷新,不是因为链死了」(PR #146 评审立下的负向断言纪律)。
//
// 前两条**没有**配「注入一处断链让它变红」的反向验证,不是漏了,是它不存在:本卡实跑试过
// 把 `accumulateFp` 的 `if (capturing) return;` 整个去掉(= 采集 ON 也照发 fp_report),
// 两条**照样绿** —— 报告到达 Output 时基线已被同一遍采集覆写,比出来的是「自己跟自己一样」。
// 也就是说这两条钉的是一条**结构性属性**,不是某一行代码的行为;能证伪它的只有「在覆写前
// 快照基线」那种设计级改动。第三条正向断言就是它们的防空转装置。
TEST_CASE("HOST SL-239:采集 ON 期间提示是哑的,且机会一次性消耗", "[host][v562][SL239]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const auto replay = [&r](float amplitude) {
        r.ph.timeSamples = 0;
        r.runBlocks(760, amplitude);
        Rig::pumpMessages(600);
        return r.out.captureStale(kTestChannel);
    };

    // 基线:采集 ON 播一遍 0.5 的素材。
    r.out.setCaptureEnabled(true);
    r.ph.timeSamples = 0;
    r.runBlocks(760, 0.5f);
    Rig::pumpMessages(400);
    REQUIRE(r.out.coverageOf(kTestChannel, 0.0, 7.0).coveredS > 5.0);

    // ★① 采集**留在 ON**(用户的自然流程:没有任何地方叫他关)→ 改狠上游再播:
    //     没有 ⚠。按 04 §4.5 这不是缺陷(FeatRing::accumulateFp 的 `if (capturing) return;`
    //     —— 这一秒的特征正被写成新基线,拿它跟自己比毫无意义),但用户看到的就是「提醒没了」。
    CHECK_FALSE(replay(0.05f));

    // ★② 而且**机会已经用掉了**:上面那遍采集 ON 的重播已经把基线刷成改后素材,
    //     此刻再把采集关掉、播**同一段**,⚠ 也不会回来 —— 用户全程没有任何提示,
    //     却已经永久失去了这一次「上游动过」的证据。
    r.out.setCaptureEnabled(false);
    Rig::pumpMessages(200);
    CHECK_FALSE(replay(0.05f));

    // ★③ 正向防空转:同一个 rig、只换第三种素材(基线现在是 0.05 那一版)⇒ ⚠ 必须出现。
    //     没有这一条,上面两条 CHECK_FALSE 在整条链死掉时照样绿。
    CHECK(replay(0.3f));
}

// ===========================================================================
// [SL-247] J91:采集态不落盘,重开工程一律为关。
// [SL-247] J92a:采集 ↔ 跟随引擎手动互斥,**布防豁免**。
//
// 立卡链条:SL-239 定谳「04 §4.5 的上游改动 ⚠ 只在采集 OFF 期间比对,而流程里没有任何
// 地方叫用户把采集关掉」。#146(SL-225)只堵住了「布防替用户开的那一下不许落盘」这**一种**
// 来源;用户自己开着采集保存、或打开一份 v5.6 期间已被污染的旧工程,重开后照样是 ON,
// ⚠ 照样不出现(用户实测两轮)。J91 把口径整个换掉:采集是**录制动作**不是工程设置。
//
// 契约:docs/contract-changes/20260830-j91-capture-not-persisted.md
//       docs/STATE_SCHEMA.md §三 CFGS + [J91] 一条;docs/SCVB_CONTRACT.md §1.2/§1.3/§1.23
// ===========================================================================

// ① J91 正题 = SL-239 探路时实测**红**的那条(E1),现在必须绿:
//    工程里存着「采集 ON」,重开后采集必须是**关**,于是上游改动 ⚠ 照常翻出。
TEST_CASE("HOST SL-247:采集 ON 时存的工程,重开后采集恒为关且 ⚠ 照常翻出", "[host][SL247][j91]")
{
    juce::MemoryBlock blob;
    {
        Rig r;
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());

        r.out.setCaptureEnabled(true);
        r.ph.timeSamples = 0;
        r.runBlocks(760, 0.5f); // ≈8.1s 基线
        Rig::pumpMessages(400);
        REQUIRE(r.out.coverageOf(kTestChannel, 0.0, 7.0).coveredS > 5.0);

        // ★ 采集**留在 ON** 就保存 —— 改判前这份 blob 会把 ON 带进下一个工程会话,
        //   于是重开后 Input 一条 fp_report 都不发,⚠ 永不出现(SL-239 的 E1 实测红)。
        REQUIRE(r.out.captureEnabled());
        r.out.getStateInformation(blob);
        REQUIRE(blob.getSize() > 0);
    } // 析构 = 关工程

    Rig r2; // 全新实例 = 重开工程
    r2.out.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    Rig::pumpMessages(200);

    // ★★ J91:不管存的时候采集是什么状态,载回来一律是**关**。
    CHECK_FALSE(r2.out.captureEnabled());
    // 基线仍随工程回来(J91 只改采集态,不碰 FEAT)—— 没有它下面那条断言不出东西。
    REQUIRE(r2.out.coverageOf(kTestChannel, 0.0, 7.0).coveredS > 5.0);

    r2.ph.playing = true;
    REQUIRE(r2.waitUntilInjected());
    r2.ph.timeSamples = 0;
    r2.runBlocks(760, 0.05f); // 改狠上游 EQ
    Rig::pumpMessages(600);

    // ★★★ 用户看得见的那件事:⚠ 回来了。改判前这里恒 false。
    CHECK(r2.out.captureStale(kTestChannel));
}

// ② J91 的反向:**存的时候采集是关**,载回来当然也是关 —— 挡住「把断言写成恒真」。
//    这一条单独存在的理由:上面那条若把实现写成「加载后恒为关」以外的任何东西
//    (比如「取反」),它照样绿;两条一起才钉住「恒为关」而不是「跟着存的值走或取反」。
TEST_CASE("HOST SL-247:采集 OFF 时存的工程,重开后同样是关(J91 恒为关,不是取反)", "[host][SL247][j91]")
{
    juce::MemoryBlock blob;
    {
        Rig r;
        r.ph.playing = true;
        REQUIRE(r.waitUntilInjected());
        r.out.setCaptureEnabled(false);
        REQUIRE_FALSE(r.out.captureEnabled());
        r.out.getStateInformation(blob);
        REQUIRE(blob.getSize() > 0);
    }
    Rig r2;
    r2.out.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    Rig::pumpMessages(200);
    CHECK_FALSE(r2.out.captureEnabled());
}

// ③ J92a:双向手动互斥 + **布防豁免**。
//
// 判据全取引擎侧真值(`captureEnabled()` / `outputEnabled()`),不看 UI。
// 豁免那一节是本用例的正题 —— 它守的是「点『重采集选区』不会静默关掉用户的输出引擎」,
// 那正是硬互斥若无差别生效时会造成的、用户拍板时不可能预见的连锁。
TEST_CASE("HOST SL-247:采集与跟随引擎手动互斥,布防豁免", "[host][SL247][j92a]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    SECTION("手动开采集 ⇒ 关跟随引擎")
    {
        r.out.setOutputEnabled(true);
        REQUIRE(r.out.outputEnabled());
        r.out.setCaptureEnabled(true);
        CHECK(r.out.captureEnabled());
        CHECK_FALSE(r.out.outputEnabled()); // ★ 互斥
    }

    SECTION("手动开跟随引擎 ⇒ 关采集")
    {
        r.out.setCaptureEnabled(true);
        REQUIRE(r.out.captureEnabled());
        r.out.setOutputEnabled(true);
        CHECK(r.out.outputEnabled());
        CHECK_FALSE(r.out.captureEnabled()); // ★ 反方向互斥
    }

    SECTION("关一个不牵连另一个 —— 互斥只在**开**的那一跳生效")
    {
        r.out.setCaptureEnabled(true);
        REQUIRE(r.out.captureEnabled());
        r.out.setCaptureEnabled(false);
        CHECK_FALSE(r.out.captureEnabled());
        CHECK_FALSE(r.out.outputEnabled()); // 本来就是关的,没被「反向打开」
    }

    SECTION("★ 布防豁免:点「重采集选区」不得关掉用户的输出引擎")
    {
        constexpr std::uint16_t kMask = 1u << (kTestChannel - 1);

        // 用户自己把输出开着、采集关着(布防才会替他开采集,裁定① 的前提)。
        r.out.setOutputEnabled(true);
        REQUIRE(r.out.outputEnabled());
        REQUIRE_FALSE(r.out.captureEnabled());

        r.out.armRecapture(kMask, 1.0, 2.0, /*autoStop=*/false);

        // 布防替他开了采集(§1.23 裁定①)……
        CHECK(r.out.captureEnabled());
        CHECK(r.out.runtime().recaptureAutoEnabledCapture);
        // ★★ ……但**没有**顺手关掉他的输出引擎。无差别互斥的实现在这里红。
        //     豁免不是靠 if,是靠调用点区分:布防走内部 applyCaptureEnabled,
        //     互斥只写在桥面 setCaptureEnabled 里。
        CHECK(r.out.outputEnabled());
    }

    SECTION("布防期用户手动开输出 ⇒ 采集被关、本次重采集作废,而布防位保留")
    {
        constexpr std::uint16_t kMask = 1u << (kTestChannel - 1);
        r.out.setCaptureEnabled(false);
        r.out.armRecapture(kMask, 1.0, 2.0, /*autoStop=*/false);
        REQUIRE(r.out.captureEnabled());
        REQUIRE(r.out.runtime().recaptureArmed);

        r.out.setOutputEnabled(true); // 用户手动开输出 = 接管

        CHECK(r.out.outputEnabled());
        CHECK_FALSE(r.out.captureEnabled()); // 互斥关掉了采集 ⇒ 这次重采集作废
        // ★ 布防位**保留**(不自动撤防):撤了用户就丢了刚拖出来的工作选区且毫无痕迹。
        //   留着,`armed ∧ !capture_enabled` 本身就是桥面出提示的依据(不新增契约字段)。
        CHECK(r.out.runtime().recaptureArmed);
        // 视为用户接管这把闸,撤防不再替他恢复(与 §1.23 裁定③ 同款)。
        CHECK_FALSE(r.out.runtime().recaptureAutoEnabledCapture);
    }
}

// ④ **读侧那一道单独的护栏** —— 没有它,J91 对**老工程**就是空的。
//
// 为什么必须单独立一条:写侧恒 0 之后,凡是本构建自己存出来的 blob,`capture_enabled` 都是 0
// —— 读侧忽不忽略**看不出差别**。实跑验证过:把读侧退回 `captureEnabled_ = s.captureEnabled != 0`,
// 上面 ①② 两条**照样全绿**。也就是说读侧那行生产代码当时没有任何用例守着,后来人「顺手化简」
// 把它删掉,一切照绿,而 v5.6 期间被污染的老工程从此再也好不了 —— 那恰恰是 J91 对用户最直接的好处。
//
// 所以这里**手工造一份 `capture_enabled = 1` 的 CFGS**(绕开 getStateInformation,模拟旧构建
// 存下的工程),断言载入后仍是关。这条用例是「老工程自愈」这个承诺的唯一护栏。
TEST_CASE("HOST SL-247:老工程里 capture_enabled=1 也必须载成关(读侧护栏)", "[host][SL247][j91]")
{
    // 先拿一份合法的整份工程做底(本构建存的,里面 capture_enabled 已是 0)。
    juce::MemoryBlock base;
    {
        Rig r;
        r.out.getStateInformation(base);
        REQUIRE(base.getSize() > 0);
    }

    // 把 CFGS 里的 capture_enabled 手工改成 1 —— 这就是一份「旧构建存的、被污染的工程」。
    scvb::state::StateChunks chunks;
    const auto res = scvb::state::loadState(static_cast<const std::uint8_t*>(base.getData()), base.getSize(), chunks);
    REQUIRE(res.status == scvb::state::StateLoadStatus::Ok);
    const auto* cfgs = chunks.find(scvb::state::kFourccCfgs);
    REQUIRE(cfgs != nullptr);
    scvb::state::OutputState s;
    REQUIRE(scvb::state::decodeOutputState(cfgs->payload.data(), cfgs->payload.size(), s));
    REQUIRE(s.captureEnabled == 0u); // 前置:本构建存的确实是 0(写侧恒 0 那一半的旁证)
    s.captureEnabled = 1u; // ← 手工污染
    std::vector<std::uint8_t> payload;
    REQUIRE(scvb::state::encodeOutputState(s, payload));
    chunks.set(scvb::state::kFourccCfgs, std::move(payload));
    std::vector<std::uint8_t> poisoned;
    REQUIRE(scvb::state::encodeContainer(chunks, poisoned));

    // 前置:这份 blob 里那一位**真的是 1**(否则下面断言是空的)。
    {
        scvb::state::StateChunks back;
        REQUIRE(scvb::state::loadState(poisoned.data(), poisoned.size(), back).status ==
                scvb::state::StateLoadStatus::Ok);
        const auto* c2 = back.find(scvb::state::kFourccCfgs);
        REQUIRE(c2 != nullptr);
        scvb::state::OutputState s2;
        REQUIRE(scvb::state::decodeOutputState(c2->payload.data(), c2->payload.size(), s2));
        REQUIRE(s2.captureEnabled == 1u);
    }

    // ★ 载入这份被污染的工程 —— 采集必须是**关**。读侧退回 `s.captureEnabled != 0` 时这条转红。
    ScvbOutputAudioProcessor out;
    out.setStateInformation(poisoned.data(), static_cast<int>(poisoned.size()));
    CHECK_FALSE(out.captureEnabled());
}

// ⑤ [SL-247] 把 SL-239 的 E2 与 web 横幅 ⑨ **接起来**。
//
// SL-239 的 `HOST SL-239:采集 ON 期间提示是哑的` 钉住了引擎侧那一半(采集 ON ⇒ 没有 ⚠);
// `smoke-output-stale-page.mjs` 的 `connected` 档钉住了 web 侧那一半(⑨ 亮)。中间缺一条:
// **引擎在那一刻真的处于 ⑨ 判据所读的那个状态吗?** 不接这一跳,两边各自绿而中间是假设 ——
// 正是本仓 T37 那一族「每一跳单测都绿、跳与跳之间从没接线」的形状。
//
// harness 编不进桥面/页面,所以这里断的是**桥面读什么、引擎就给什么**:⑨ 判据里
// 引擎侧可观测的三项 —— `capture_enabled` 为真、该轨 `stale` 为假(⚠ 确实没出来)、
// 且不在布防期。第四项(已有段表)与 DOM 渲染归 web 冒烟,已在那边断。
//
// 它有牙齿的地方:若日后有人让「采集 ON 期间也能比对」成立(SL-239 已论证那条路走不通,
// 但不排除有人再试),`captureStale` 会在这一刻变真 —— 那时 ⑨ 的文案就成了假话
// (它说「不比对」),本条会红,提醒同批改 web 判据与文案。
TEST_CASE("HOST SL-247:采集 ON 那一刻,引擎侧确实是横幅 ⑨ 判据读到的状态", "[host][SL247][j91]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // 与 SL-239 的 E2 同一段构造:采集 ON 采一段基线,改狠上游,**采集不关**再播一遍。
    r.out.setCaptureEnabled(true);
    r.ph.timeSamples = 0;
    r.runBlocks(760, 0.5f);
    Rig::pumpMessages(400);
    REQUIRE(r.out.coverageOf(kTestChannel, 0.0, 7.0).coveredS > 5.0);

    r.ph.timeSamples = 0;
    r.runBlocks(760, 0.05f); // 改狠上游,采集**留在 ON**
    Rig::pumpMessages(600);

    // ★ ⑨ 判据里引擎侧可观测的三项,必须同时成立 —— 这就是「⑨ 该亮」的引擎侧根据。
    CHECK(r.out.captureEnabled());
    CHECK_FALSE(r.out.captureStale(kTestChannel));
    CHECK_FALSE(r.out.runtime().recaptureArmed);
}

// ===========================================================================
// [SL-254 / J95①] 离线(非实时)渲染的同块交接。
//
// 立卡:用户 v5.6.3 实测 #48「离线导出前约 10 秒完全静音」,而实时渲染反相可完美抵消。
// 定谳:J32 的注入握手全程按**墙钟**闸控(`steadyNowMs` + [M] 25Hz),与音频时间线解耦 ——
// 「Input 已静音、Output 未注入」那个窗口在离线下按倍速放大。实测折合墙钟恒为一拍 [M]
// ≈40ms,而时间线静音 5x→0.128s / 48x→2.13s / 122x→4.69s(≈250x 即用户的 10s)。
//
// 下面两条**必须成对**:一条防静音、一条防叠加。任一条单独存在,都会被**另一个方向**的
// 错误修法绕过 —— 这正是本卡定谳时先后踩到的两个坑(先以为是 200ms 闸,而只改 Output 侧
// 闸门为样本计会把静音翻成几百块双路叠加)。宪法 ADR.md [J32→ADR-002] 的 J95① 补注
// 已把「不得只改一侧」连同实测数字入册。
//
// 判据都取**引擎侧可观测量**(Input 轨输出 / Output 总线注入),不看内部计数器。
// ===========================================================================

namespace
{
// 以「每泵一次消息循环推多少块」模拟离线渲染倍速:推得越多 = 音频跑在墙钟前面越远。
// 返回 {Input 首个静音块, Output 首个注入块, 两者同时静音的块数}。
struct OfflineHandoverProbe
{
    int firstInputSilent = -1;
    int firstInject = -1;
    int bothSilentBlocks = 0;
    // 「过渡块」= Input 电平严格落在 0 与源电平之间 ⇒ 80ms ramp 正在渐变。
    // 硬切(snapTo)下**不存在**这种块:每块非满即零。这是实时/非实时唯一稳定可分的印记 ——
    // J32 的 200ms 闸会被 muted 确认位抄近路,所以「注入晚 200ms」在 harness 里不可靠。
    int rampBlocks = 0;
};

// 交接所需的**墙钟预算**,以泵次数计(泵数 = totalBlocks / blocksPerPump)。
// 离线那四个调用点共用这一个真源 —— 下次再遇到更慢的 runner 只改这里。
//
// 为什么是 112:本机实测各档交接分别发生在第 11(8 档)/ 19(16 档)/ 17(32 档)泵。
// 原先三档共用 totalBlocks=900,泵数是 112/56/**28**,32 档只剩 1.6× 余量 —— CI runner
// 慢一点就越不过去,整轮**根本没发生交接**(firstInputSilent = -1),已在 CI 上真红过一次。
// 由此可知 CI 需要的泵数 **> 1.6 倍本机**;按同一把尺子,④/⑤ 原先的 37 泵(≈1.9×)同样
// 不安全,而它们的前置断言是**给主断言铺路的 setup** —— 它一挂,真正要测的「Output
// 释放/崩溃后 Input 必须回直通」根本执行不到,变成与被测行为无关的 setup flake。
// 故四处统一到 112 泵(16 档余量 ≈5.9×,32 档 ≈6.6×)。
//
// ⚠ 要加余量就加**泵数**(本常量),**不要**加 pumpMs:倍速比 = 块数/墙钟,
// 加 pumpMs 会把该档的模拟倍速降下去,正好失去「覆盖高倍速」的本意。
constexpr int kHandoverPumps = 112;

// pumpMs:每次泵消息循环的**墙钟**时长。离线用 1(墙钟几乎不走 = 音频跑在墙钟前面);
// 实时用 20(让 J32 的 200ms 墙钟闸真的走得完,否则实时探针量不到注入)。
OfflineHandoverProbe runOfflineHandover(Rig& r, int blocksPerPump, int totalBlocks, int pumpMs = 1)
{
    OfflineHandoverProbe p;
    for (int i = 0; i < totalBlocks; ++i)
    {
        Rig::fillSine(r.inBuf, 0.5f, r.ph.timeSamples);
        r.outBuf.clear();
        r.in.processBlock(r.inBuf, r.midi);
        r.out.processBlock(r.outBuf, r.midi);

        float inPk = 0.0f, outPk = 0.0f;
        for (int c = 0; c < r.inBuf.getNumChannels(); ++c)
            for (int s = 0; s < r.inBuf.getNumSamples(); ++s)
                inPk = std::max(inPk, std::abs(r.inBuf.getReadPointer(c)[s]));
        for (int c = 0; c < r.outBuf.getNumChannels(); ++c)
            for (int s = 0; s < r.outBuf.getNumSamples(); ++s)
                outPk = std::max(outPk, std::abs(r.outBuf.getReadPointer(c)[s]));

        if (p.firstInputSilent < 0 && inPk < 1e-6f)
            p.firstInputSilent = i;
        // 源幅度 0.5:落在 (1e-4, 0.45) 之间 = ramp 中途。硬切不产生这样的块。
        if (inPk > 1e-4f && inPk < 0.45f)
            ++p.rampBlocks;
        if (p.firstInject < 0 && outPk > 1e-6f)
            p.firstInject = i;
        // 「Input 静音 ∧ Output 未注入」= 用户听到的全零。DAW 里直通期总线有声,harness 不
        // 路由,所以必须分开量 —— 合起来量会把直通期误算成静音。
        if (inPk < 1e-6f && outPk < 1e-6f)
            ++p.bothSilentBlocks;

        r.ph.timeSamples += kBlock;
        if ((i % blocksPerPump) == blocksPerPump - 1)
            Rig::pumpMessages(pumpMs);
    }
    return p;
}
} // namespace

// ① 防静音:非实时下,不论音频跑得多快,「Input 静音 ∧ Output 未注入」的重叠窗恒为 0。
//    反向 = 退回墙钟闸(实时路径)⇒ 重叠窗随倍速线性增长。
TEST_CASE("HOST SL-254:非实时渲染不得出现「Input 静音 ∧ Output 未注入」的全零窗", "[host][SL254]")
{
    for (const int blocksPerPump : {8, 16, 32})
    {
        Rig r;
        r.in.setNonRealtime(true); // 宿主宣告离线渲染
        r.out.setNonRealtime(true);
        r.ph.playing = true;

        // 总块数按档位**等比放大**,让每档拿到同样多的泵次数(= 同样的墙钟预算);
        // 取值与理由见 kHandoverPumps 的头注(四个离线调用点共用那一个真源)。
        const auto p = runOfflineHandover(r, blocksPerPump, kHandoverPumps * blocksPerPump);
        INFO("blocksPerPump=" << blocksPerPump << " Input静音@" << p.firstInputSilent << " 注入@" << p.firstInject
                              << " 全零块=" << p.bothSilentBlocks << " ramp块=" << p.rampBlocks);
        // ★ 前置:确实交接过。缺了这两行,「本轮根本没发生交接」会让下面两条 `== 0` **恒真** ⇒
        //   空跑全绿。而 blocksPerPump 越大、泵次数越少、墙钟预算越紧,最该覆盖高倍速的那档
        //   反而最可能一拍 [M] 都没落下来 —— 主防线空跑是最不该出现的假绿。
        REQUIRE(p.firstInputSilent >= 0);
        REQUIRE(p.firstInject >= 0);
        // ★ 核心判据:一块全零都不许有。墙钟闸的实现在这里红,且红的块数随倍速放大。
        CHECK(p.bothSilentBlocks == 0);
        // ★ 与 ③ 成镜像:非实时是**硬切**,不许有 ramp 过渡块(ramp 也是墙钟量,250x 下会摊成 20s
        //   渐变 —— 把「前段静音」换成「前段超长淡入」,同样被倍速放大)。谁把 ramp 放回非实时,这里红。
        CHECK(p.rampBlocks == 0);
    }
}

// ② 防叠加:非实时下,Output 的首个注入块**不得早于** Input 的首个静音块。
//    这一条专守被 J95① 明令禁用的那个修法(只把 Output 的 200ms 闸改成样本计)——
//    那样注入会跑到 Input 静音之前,几百块双路叠加,而 ① 照样绿。
TEST_CASE("HOST SL-254:非实时下注入不得早于 Input 静音(防双路叠加)", "[host][SL254]")
{
    Rig r;
    r.in.setNonRealtime(true);
    r.out.setNonRealtime(true);
    r.ph.playing = true;

    const auto p = runOfflineHandover(r, 16, kHandoverPumps * 16);
    REQUIRE(p.firstInputSilent >= 0); // 前置:确实交接过(否则两条断言都是空的)
    REQUIRE(p.firstInject >= 0);
    INFO("Input静音@" << p.firstInputSilent << " 注入@" << p.firstInject);
    // ★ 同块交接:注入不早于静音(允许同块,不允许更早)。
    CHECK(p.firstInject >= p.firstInputSilent);
}

// ③ 实时路径逐字不变:J32 的 200ms / muted 确认位 / 80ms ramp 一个都没动。
//    没有这一条,上面两条会诱使后来人把非实时那套直接铺到实时路径上。
//
//    【复审 r1 补强】原形态只断言实时**稳态**(最终注入 + meter>0),而把实时也改成同块硬切之后
//    那两条照样全绿 —— 正是本用例声称要防的事。断言必须落在**过渡期**:实时下那个「Input 已静音
//    ∧ Output 未注入」的窗口是墙钟闸的**可观测投影**,它必须**存在且非零**(实时下它被 80ms ramp
//    盖住、听不见,所以是良性的;离线下它就是本卡的病)。①「非实时恒为 0」与 ③「实时必须 >0」
//    构成一对方向相反的钳子:谁把两条路径搞成同一套,必有一条红。
TEST_CASE("HOST SL-254:实时路径仍走 J32 原协议(注入延迟未被顺手删掉)", "[host][SL254]")
{
    Rig r; // 不宣告非实时 = 实时路径
    r.ph.playing = true;

    // ⚠ 探针必须从**尚未交接**的状态起跑:先 waitUntilInjected() 的话交接早在探针之前就完成了,
    //   量到的 firstInject/firstInputSilent 都只是「稳态第一块」,恒等 ⇒ 断言恒假(已踩过)。
    // pumpMs=20 让 J32 的 200ms 墙钟闸在探针窗口内真的走得完。
    const auto p = runOfflineHandover(r, 4, 600, /*pumpMs=*/20);
    INFO("实时:Input静音@" << p.firstInputSilent << " 注入@" << p.firstInject << " 全零块=" << p.bothSilentBlocks
                           << " ramp块=" << p.rampBlocks);
    REQUIRE(p.firstInputSilent >= 0);
    REQUIRE(p.firstInject >= 0);
    // ★ 80ms ramp 仍在:实时下必有「电平介于 0 与源之间」的过渡块。
    //   把实时也改成同块硬切(snapTo)⇒ 每块非满即零 ⇒ rampBlocks 归 0 ⇒ 本条红,而 ①②④ 照样绿。
    //
    //   为什么不断言「注入晚于静音 200ms」:J32 的注入闸是「muted 确认位 **或** 200ms,先到者」,
    //   实时下 Input 一静音就置确认位,Output 下一拍即注入 —— 200ms 那条几乎从不生效,拿它做断言
    //   会恒假(已实测:实时 Input静音@16 注入@16、全零块=0)。ramp 才是实时路径稳定可观测的印记。
    CHECK(p.rampBlocks > 0);
    //   ⚠ 这里**不能**照搬 ② 的「注入不早于静音」:实时下 [M] 的 muted 确认位取的是 stageMachine
    //   的**目标档**(`InputProcessor.cpp` 的 setMuted 注释写明「以目标档近似」),而 [A] 还在走
    //   80ms ramp,所以注入合法地落在 ramp 中途(实测 注入@16 而全静音@20)—— 那段重叠是**渐弱**
    //   的淡出而不是等幅双路,正是 J32 用 ramp 覆盖掉的部分。非实时没有 ramp,故 ② 在那边才成立。

    // 稳态与改动前一致(原断言保留)。
    REQUIRE(r.waitUntilInjected());
    CHECK(r.injected());
    r.runBlocks(60, 0.5f);
    Rig::pumpMessages(200);
    CHECK(r.out.meterSnapshot().trackPeak[kTestChannel - 1] > 0.0f);
}

// ④ 【复审 r1 红旗】非实时下健康前提不被豁免:Output 释放后 Input 必须回直通,不得恒静音。
//
//    `Registry::releaseOutput()` 把 OutputSlot.state 置回 kSlotFree 时**不清 connected_mask**
//    (清 mask 只在 claimOutput 的两条路径)。于是「Output 被旁通/删除/宿主调 releaseResources()」
//    之后 mask 位**残留**。若非实时逐块路径只读 mask 一位,就会把「无 Output」渲染成**整条导出
//    全零** —— 与本卡要修的用户症状同类,且正是 [J12] 立条要消除的事故面。
//    宪法 J95① 段末已明写:非实时豁免的只是时间窗与 ramp,**不豁免健康前提**。
TEST_CASE("HOST SL-254:非实时下 Output 释放后必须回直通(健康前提不被豁免)", "[host][SL254]")
{
    Rig r;
    r.in.setNonRealtime(true);
    r.out.setNonRealtime(true);
    r.ph.playing = true;

    // 先跑到交接完成:Input 确实已被接管静音(否则后面的断言是空的)。
    const auto before = runOfflineHandover(r, 16, kHandoverPumps * 16);
    REQUIRE(before.firstInputSilent >= 0);
    REQUIRE(before.firstInject >= 0);

    // Output 退场:走宿主真实路径 releaseResources() ⇒ session_.release() ⇒ releaseSlot()
    // ⇒ Registry::releaseOutput():state = kSlotFree,而 connected_mask **位残留**。
    r.out.releaseResources();
    // 让 [M] 跑几拍。注意**不是** outputStale_ 在起作用:state 已 kSlotFree,`outputClaimedButStale()`
    // 按设计提前 return false(不重复否决),真正兜住的是 [A] 的**逐块 state 读**。
    // outputStale_ 那条路(自称 kSlotActive 却心跳陈旧)由 ⑤ 覆盖。
    Rig::pumpMessages(80);

    // ★ 此后 Input 必须回到直通:逐块读到的 mask 位还在,但 state 已 kSlotFree、存活位已落。
    float peak = 0.0f;
    for (int i = 0; i < 200; ++i)
    {
        Rig::fillSine(r.inBuf, 0.5f, r.ph.timeSamples);
        r.in.processBlock(r.inBuf, r.midi);
        for (int c = 0; c < r.inBuf.getNumChannels(); ++c)
            for (int s = 0; s < r.inBuf.getNumSamples(); ++s)
                peak = std::max(peak, std::abs(r.inBuf.getReadPointer(c)[s]));
        r.ph.timeSamples += kBlock;
        if ((i % 16) == 15)
            Rig::pumpMessages(1);
    }
    INFO("Output 释放后 Input 峰值=" << peak);
    // 只读 mask 一位的实现在这里红(peak 恒 0 = 整条导出全零)。
    CHECK(peak > 1e-3f);
}

// ⑤ 【复审 r2】崩溃未释放:Output 停心跳但**不**走释放路径(state 仍 kSlotActive、mask 位仍在)。
//
//    这是 `outputStale_` / `InputSession::outputClaimedButStale()` **存在的唯一理由** ——
//    ④ 走的是 releaseResources ⇒ state=kSlotFree,而该函数在 state 非 kSlotActive 时提前
//    return false,所以 ④ 全程 outputStale_==0、把否决位整条删掉也照样绿(与上一轮回归③ 同形态)。
//    没有这一条,那条判据就是零覆盖的死代码。
TEST_CASE("HOST SL-254:非实时下 Output 崩溃未释放(心跳陈旧)也必须回直通", "[host][SL254]")
{
    Rig r;
    r.in.setNonRealtime(true);
    r.out.setNonRealtime(true);
    r.ph.playing = true;

    const auto before = runOfflineHandover(r, 16, kHandoverPumps * 16);
    REQUIRE(before.firstInputSilent >= 0); // 前置:确实交接过
    REQUIRE(before.firstInject >= 0);

    // 模拟「Output 进程挂死」:**不**调 releaseResources(state 仍 kSlotActive、mask 位仍在),
    // 只把 OutputSlot 的心跳**持续拨旧**。Output 的 [M] 仍在跑、每 250ms 会刷回心跳,所以
    // 必须在每次泵消息**之前**重新拨旧 —— 一次性拨旧会被它刷掉,那样测的就不是这条路了。
    scvb::SegmentBackendWin32 backend;
    scvb::Registry probe(backend, kTestGroup);
    REQUIRE(probe.open() == scvb::Registry::ClaimResult::kClaimed);
    scvb::OutputSlot* os = probe.outputSlot();
    REQUIRE(os != nullptr);
    REQUIRE(os->state.load() == scvb::kSlotActive); // 前置:仍自称活着(没走释放路径)
    REQUIRE((probe.connectedMask() & (1u << (kTestChannel - 1))) != 0); // 前置:mask 位仍在

    const auto backdate = [&os] { os->heartbeat_ms.store(1); }; // 1 = 远古,必然 > kStaleDisplayMs

    // settle 循环(照本文件 waitUntilInjected / heartbeatOf 那两处的既有形态):**每轮都重新拨旧**,
    // 每轮泵满 ≥1 拍 [M],等到否决位真的生效为止。
    //
    // ⚠ 不能写成「拨旧一次 + 泵 60ms + 之后只泵 1ms」(复审 r2 抓出的竞态):Output 的 [M] 仍在跑,
    // 4Hz 心跳会把 heartbeat_ms 刷回新鲜。若那 60ms 里恰好轮到它刷、且排在 Input 的 [M] 之前,
    // Input 读到的就是新鲜心跳 ⇒ outputStale_ 停在 0;而后续每 16 块才泵 1ms、累计仅约 12ms,
    // **不足一拍 [M]**,再没有翻盘机会 ⇒ peak 恒 0 ⇒ **假红**(方向是误报,但仍是不稳定用例)。
    // ⚠ 光断言「回了直通」**不够有牙齿**:settle 循环拉长后,Output 的 [M] 有机会把该轨判下线并
    // `clearConnectedMaskBit`,那样 Input 回直通就与否决位无关了 —— 实测过:只断言 wentPassthrough
    // 时,把否决位删掉该用例**照样绿**。所以必须在**观测到直通的那一轮**同时钉住「mask 位仍在 ∧
    // state 仍 active」——「残留 mask 之下仍回直通」才是否决位独有的效果,别的路径都满足不了。
    bool wentPassthrough = false;
    bool maskStillSetAtPassthrough = false;
    bool sawMaskCleared = false;
    for (int waited = 0; waited < 2000 && !wentPassthrough; waited += 40)
    {
        backdate();
        Rig::pumpMessages(40); // ≥ 一拍 [M],保证 Input 有机会评估
        backdate(); // 泵完再拨一次:这一拍里 Output 可能刚把心跳刷回新鲜

        const bool maskSet = (probe.connectedMask() & (1u << (kTestChannel - 1))) != 0;
        const bool stateActive = os->state.load(std::memory_order_acquire) == scvb::kSlotActive;
        if (!maskSet)
        {
            sawMaskCleared = true;
        }

        Rig::fillSine(r.inBuf, 0.5f, r.ph.timeSamples);
        r.in.processBlock(r.inBuf, r.midi);
        r.ph.timeSamples += kBlock;

        if (r.inBuf.getMagnitude(0, r.inBuf.getNumSamples()) > 1e-3f)
        {
            wentPassthrough = true;
            maskStillSetAtPassthrough = maskSet && stateActive;
        }
    }
    INFO("回直通=" << wentPassthrough << " 直通时 mask 仍在=" << maskStillSetAtPassthrough
                   << " 期间曾观测到 mask 被清=" << sawMaskCleared);
    // ★ 删掉 outputStale_ 否决位 ⇒ 两条里必有一条红:mask 仍在时 Input 恒静音(第一条红),
    //   或 Input 因 mask 被清才直通(第二条红)。两条都绿只可能是否决位真的生效了。
    REQUIRE(wentPassthrough);
    REQUIRE(maskStillSetAtPassthrough);
}

// ===========================================================================
// [SL-255] 松手档 300ms 防抖重分段 —— [W] 流水线实装(J95③)。
//
// 用户实测:「松手 300ms 应用,图上看不出效果」。定谳:那条流水线在 native **根本不存在**,
// 只存在于 preview 的 mock —— `handleSetVadParams` / `handleSetSegmentation` 的全部函数体是
// 「夹取 → 赋值 → ++configSeq → 返回 ok」,`startAnalysis` 全仓只有「点分析」一个调用点,
// 全仓也从不发 `reason:"vad"/"segmentation"` 的 §2.8 事件(web 有消费者、没生产者)。
// 而契约 §1.18 白纸黑字写着「由 **C++ 侧 300ms 防抖**自动跑完整流水线」—— UI 正是据此
// **刻意不自建定时器**的。于是那条「300ms 后应用…」的倒计时条,等的是一个永远不来的事件。
//
// 本组断真机数据面:段表**真的变了**、reason 落对、用户段没被动。
// ===========================================================================
namespace
{
// [SL-255] 这三例要的是「**分得出多段**」的素材。`captureBursts` 的静音只有 40 块,会被
// VAD 的 hangover(180ms)+ 前后 padding 桥接成一整段 —— 实测整轨只出 **1** 个段,于是
// 「重分段前后段数不同」这种判据既测不出东西、又会随机翻车。静音拉到 200 块。
void captureSpacedBursts(Rig& r, int bursts = 6)
{
    for (int i = 0; i < bursts; ++i)
    {
        r.runBlocks(60, 0.5f, 4, 4); // 有声
        r.runBlocks(200, 0.0f, 4, 4); // 长静音:确保切得开
    }
}
} // namespace

TEST_CASE("HOST SL-255:改 VAD 参数松手 → 300ms 后真的重分段并以 reason:\"vad\" 发出", "[host][t37][v57][SL255]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    // 采一段有声有静的素材并分析出段表。
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    captureSpacedBursts(r);
    Rig::pumpMessages(400);
    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 60.0).coveredS;
    REQUIRE(coveredS > 2.0);
    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS).ok);
    waitAnalysis(r);
    REQUIRE_FALSE(r.out.analysisRunning());

    // 段真身按既有用例的读法:crvsSnapshot() → 当前版本 → 该轨。
    const auto segsOf = [&r]() {
        const auto crvs = r.out.crvsSnapshot();
        return crvs.versions[static_cast<std::size_t>(r.out.versionActive() - 1)].tracks[kTestChannel - 1].segments;
    };
    const auto before = segsOf().size();
    INFO("analyzed segments = " << before);
    REQUIRE(before > 0); // 前置:先得有段表可改
    const auto revBefore = r.out.crvsRevision();

    // ★ 走带停下来:PRINT 态是契约 §1.18 的抑制条件之一,不停的话这一条会被合法地抑制掉。
    r.ph.playing = false;
    Rig::pumpMessages(100);

    // 改 VAD 阈值(等价于用户拖完滑杆松手那一下的整包下发)。
    r.out.runtime().vadThresholdDb = 12.0f; // 与默认差得远,保证分段结果真的不同
    r.out.armResegment(ScvbOutputAudioProcessor::AnalysisDoneReason::Vad);

    // 防抖 300ms(25Hz tick 分辨率 40ms ⇒ 实际 300~340ms),再等流水线跑完。
    Rig::pumpMessages(600);
    waitAnalysis(r);
    REQUIRE_FALSE(r.out.analysisRunning());

    // ★ 核心判据是**机制**,不是「段数变没变」:`takeAnalysisDone()` 回 Vad ⟺ 防抖真的到点
    // ∧ 流水线真的跑完 ∧ reason 一路带到了 editor 那一层。修前这一整条链根本不存在
    // (setter 只写 config 就返回),这里恒为 None。
    //
    // 为什么不断「段数变了」:段数取决于 VAD 阈值与素材的相互作用,同一份素材换个阈值完全
    // 可能仍是同样多段 —— 那是**分段算法**的性质,不是本卡要证的东西。实测第一版就是这么
    // 翻车的(before=1 after=1),而流水线其实跑了。
    const auto doneReason = r.out.takeAnalysisDone();
    CHECK(doneReason == ScvbOutputAudioProcessor::AnalysisDoneReason::Vad);
    // 且 CRVS 真的动过一次(修订号推进 = 段表被重写并重建了曲线)。
    CHECK(r.out.crvsRevision() != revBefore);
}

TEST_CASE("HOST SL-255:重分段不动 locked 段([J34] / §1.18 逐字)", "[host][t37][v57][SL255]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    captureSpacedBursts(r);
    Rig::pumpMessages(400);
    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 60.0).coveredS;
    REQUIRE(coveredS > 2.0);
    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS).ok);
    waitAnalysis(r);

    const auto segsOf = [&r]() {
        const auto crvs = r.out.crvsSnapshot();
        return crvs.versions[static_cast<std::size_t>(r.out.versionActive() - 1)].tracks[kTestChannel - 1].segments;
    };

    // 前置显式化:段表为空时 editSegment 会回 BadArg,而那个报错完全看不出根因
    // (实测栽过一次:`1 == 0`,查了半天才发现是「这一轮根本没分出段」)。
    INFO("analyzed segments = " << segsOf().size());
    REQUIRE_FALSE(segsOf().empty());

    // 把第 0 段锁上并记下它的边界与值。
    {
        scvb::state::SegmentEditArgs a;
        a.op = scvb::state::SegmentEditOp::SetLocked;
        a.segIdx = 0;
        a.locked = true;
        // ⚠ `editSegment` 的 track 是**0 基**的(`editSegmentTransactional` 里
        // `versions[version - 1].tracks[track]` —— 版本 1 基、轨 0 基,同一个表达式里两套口径)。
        // 生产调用点也是 `const int track = ch - 1;`。直接传 1 基的 kTestChannel 会去改**隔壁轨**,
        // 那条轨没段 ⇒ 回 BadArg,而报错只显示 `1 == 0`,根本看不出是索引错了(实测栽过一次)。
        REQUIRE(r.out.editSegment(kTestChannel - 1, a) == scvb::state::SegmentEditResult::Ok);
    }
    const auto locked0 = segsOf().at(0);
    REQUIRE(scvb::state::segmentLocked(locked0.flags));

    r.ph.playing = false;
    Rig::pumpMessages(100);
    r.out.runtime().vadThresholdDb = 12.0f;
    r.out.armResegment(ScvbOutputAudioProcessor::AnalysisDoneReason::Vad);
    Rig::pumpMessages(600);
    waitAnalysis(r);

    // ⚠ 先证「这一轮流水线**真的跑了**」,否则本例是**空绿**的:防抖若压根没起,锁段当然
    // 纹丝不动,断言照样通过 —— 那就成了「什么都没发生」也算数(实测:摘掉防抖后本例仍绿)。
    REQUIRE(r.out.takeAnalysisDone() == ScvbOutputAudioProcessor::AnalysisDoneReason::Vad);

    // ★ 锁着的那一段逐字段不动 —— 契约 §1.18「仅改写 origin=auto 且未 locked 的段」。
    const auto segs = segsOf();
    const auto it =
        std::find_if(segs.begin(), segs.end(), [](const auto& s) { return scvb::state::segmentLocked(s.flags); });
    REQUIRE(it != segs.end()); // 锁段还在
    CHECK(it->t0 == locked0.t0);
    CHECK(it->t1 == locked0.t1);
    CHECK(it->pan == locked0.pan);
    CHECK(it->volDb == locked0.volDb);
}

TEST_CASE("HOST SL-255:PRINT 态与分析进行中一律抑制(§1.18 [J47])", "[host][t37][v57][SL255]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    captureSpacedBursts(r);
    Rig::pumpMessages(400);
    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 60.0).coveredS;
    REQUIRE(coveredS > 2.0);
    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS).ok);
    waitAnalysis(r);
    const auto segsOf = [&r]() {
        const auto crvs = r.out.crvsSnapshot();
        return crvs.versions[static_cast<std::size_t>(r.out.versionActive() - 1)].tracks[kTestChannel - 1].segments;
    };
    const auto before = segsOf().size();

    // ⚠ 前置:先把首轮分析的完成位**消费掉**,并确认它就是「点分析」那一档 ——
    // 否则下面那句「没跑」的断言会被首轮的残留喂饱([SL-255] 复审【建议】:
    // 原版只断「段数不变」,而本组自己的核心例已经证明**段数相同不代表没跑**
    // (实测 before=1 after=1),把抑制逻辑整个删掉该例照绿)。
    REQUIRE(r.out.takeAnalysisDone() == ScvbOutputAudioProcessor::AnalysisDoneReason::Analyze);
    const auto revBefore = r.out.crvsRevision();

    // PRINT 态 = 输出 ON ∧ 播放中 ∧ **走带落在已分析区间内**。
    //
    // ⚠ 第三个条件此前漏了([SL-255] 复审【建议】暴露出来的):采完素材后走带停在整条
    // 时间线的**末尾**,而末尾那 200 块是静音、不属任何段 ⇒ printRange 判 false ⇒ 模式落
    // ARMED 而不是 PRINT,「抑制」这条路根本没被走到。旧版只断「段数不变」,于是这一整
    // 例都是空绿。把走带挪进第一段的中点,PRINT 才真的成立。
    const auto segsNow = segsOf();
    REQUIRE_FALSE(segsNow.empty());
    r.out.setCaptureEnabled(false); // 下面要跑几块静音只为推进走带,别把它写回特征面
    r.ph.timeSamples = segsNow.front().t0 + (segsNow.front().t1 - segsNow.front().t0) / 2;
    r.out.setOutputEnabled(true);
    r.ph.playing = true;
    r.runBlocks(8, 0.0f); // 让处理器采到新的走带位置(playheadSnapshot 只在音频块里更新)
    Rig::pumpMessages(200);

    r.out.runtime().vadThresholdDb = 12.0f;
    r.out.armResegment(ScvbOutputAudioProcessor::AnalysisDoneReason::Vad);
    Rig::pumpMessages(800); // 远超防抖窗
    waitAnalysis(r);

    // ★ 抑制的判据是**机制**,不是「段数不变」:完成位仍为 None ⇒ 这一轮流水线压根没跑;
    //   CRVS 修订号没推进 ⇒ 段表没被重写过。删掉 armResegment / tickResegmentDebounce
    //   里任意一处抑制判据,这两条立刻红。
    CHECK(r.out.takeAnalysisDone() == ScvbOutputAudioProcessor::AnalysisDoneReason::None);
    CHECK(r.out.crvsRevision() == revBefore);
    CHECK(segsOf().size() == before);

    // ★ 反向对照(canary):**只**把 PRINT 这个条件撤掉(停走带),同样一次布防必须跑起来。
    // 没有这一段的话,「抑制成立」与「防抖整条链根本没接上」在本例里长得一模一样 ——
    // 那正是本组核心例的立卡形态(有消费者、没生产者),不能自己再犯一次。
    //
    // ⚠ 改 `r.ph.playing` 之后必须再跑几块音频:处理器读的是**自己缓存的那份走带快照**,
    // 而它只在 processBlock 里更新 —— 只 pumpMessages 的话 timerCallback 求出来的还是
    // 「在播放」,模式仍是 PRINT,这一段就会连同前一段一起「成功」,canary 反而失效。
    r.ph.playing = false;
    r.runBlocks(8, 0.0f);
    Rig::pumpMessages(200);
    r.out.armResegment(ScvbOutputAudioProcessor::AnalysisDoneReason::Vad);
    Rig::pumpMessages(600);
    waitAnalysis(r);
    CHECK(r.out.takeAnalysisDone() == ScvbOutputAudioProcessor::AnalysisDoneReason::Vad);
}

// ---------------------------------------------------------------------------
// [SL-255 复审②] 防抖的对象是**调用流**,不是变化流:重复下发要把到点时刻**往后推**。
//
// 这是把 `armResegment` 从 `if (changed)` 里提出来之后必须钉住的语义 —— mock 的
// `debounceAnalysisPipeline` 对每一次 setVadParams 都无条件 clearTimeout + 重排,
// native 现在同款。若哪天把重排改成「只在第一次布防」,本例的第一段会提前变红。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL-255:防抖窗内重复布防 → 到点时刻从**最后一次**算起", "[host][t37][v57][SL255]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    captureSpacedBursts(r);
    Rig::pumpMessages(400);
    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 60.0).coveredS;
    REQUIRE(coveredS > 2.0);
    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS).ok);
    waitAnalysis(r);
    REQUIRE(r.out.takeAnalysisDone() == ScvbOutputAudioProcessor::AnalysisDoneReason::Analyze);

    r.ph.playing = false; // 离开 PRINT,否则合法抑制
    Rig::pumpMessages(100);

    // t=0 布防;t≈200ms 再布一次(**同值也要重排** —— 这正是复审②要的语义)。
    r.out.armResegment(ScvbOutputAudioProcessor::AnalysisDoneReason::Vad);
    Rig::pumpMessages(200);
    r.out.armResegment(ScvbOutputAudioProcessor::AnalysisDoneReason::Vad);

    // 再等 200ms:距**第一次**布防已 400ms(早过 300ms 窗),距**第二次**才 200ms。
    // 重排生效 ⇒ 此刻必须还没跑。
    Rig::pumpMessages(200);
    CHECK(r.out.takeAnalysisDone() == ScvbOutputAudioProcessor::AnalysisDoneReason::None);

    // 再给足时间 ⇒ 从第二次算起也到点了,必须跑完并落 Vad。
    Rig::pumpMessages(600);
    waitAnalysis(r);
    CHECK(r.out.takeAnalysisDone() == ScvbOutputAudioProcessor::AnalysisDoneReason::Vad);
}

// ---------------------------------------------------------------------------
// [SL-255 复审①] 取消一次重分段,不许污染**下一次**分析的 reason。
//
// 缺陷形态(改前):触发档记在成员 `pendingResegmentReason_` 上,而清位只写在
// `finishAnalysis` 里 —— 取消那条路根本不经过它(handleAsyncUpdate 代号不符就整份丢弃)。
// 于是「拖 VAD 松手 → 取消分析 → 再点分析」,后面那次**点分析**会以 reason:"vad" 发出:
// Tab4 的「结果陈旧」基线认不到(只认 analyze|vad|segmentation 里对应的那个语义面)、
// undo 钮白名单按松手档处理、Tab3 把一次点分析错记成松手档。
//
// 改法:reason 随作业走(PendingAnalysis),代号一丢它跟着丢。
// ★ 反向验证:把 reason 挪回成员、清位只留在 finishAnalysis ⇒ 末尾那条 == Analyze 立刻红。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL-255:取消松手档重分段后,下一次「点分析」的 reason 仍是 analyze", "[host][t37][v57][SL255]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(400);
    captureSpacedBursts(r);
    Rig::pumpMessages(400);
    const double coveredS = r.out.coverageOf(kTestChannel, 0.0, 60.0).coveredS;
    REQUIRE(coveredS > 2.0);

    r.ph.playing = false; // 离开 PRINT
    Rig::pumpMessages(100);

    // 布防松手档,然后**在它正跑着的时候**取消。
    // 抓「正在跑」用 1ms 粒度的泵:finishAnalysis 只从消息循环里跑,抓到 running 就意味着
    // 完成帧还没派发。允许重试几轮,免得被一次调度抖动判成失败。
    bool cancelledInFlight = false;
    for (int attempt = 0; attempt < 8 && !cancelledInFlight; ++attempt)
    {
        r.out.armResegment(ScvbOutputAudioProcessor::AnalysisDoneReason::Vad);
        for (int i = 0; i < 1200; ++i)
        {
            Rig::pumpMessages(1);
            if (r.out.analysisRunning())
            {
                r.out.cancelAnalysis();
                cancelledInFlight = true;
                break;
            }
        }
        if (!cancelledInFlight)
        {
            waitAnalysis(r); // 这一轮跑完了:清干净再试
            (void)r.out.takeAnalysisDone();
        }
    }
    REQUIRE(cancelledInFlight); // 前置:确实在飞行中取消了(否则本例什么都没测)

    Rig::pumpMessages(300);
    waitAnalysis(r);
    // 取消 = 结果整份丢弃 ⇒ 完成位不置(§1.6 取消语义,顺带守住)。
    REQUIRE(r.out.takeAnalysisDone() == ScvbOutputAudioProcessor::AnalysisDoneReason::None);

    // 现在走「点分析」那条路。
    REQUIRE(r.out.startAnalysis(0, 0.0, coveredS).ok);
    waitAnalysis(r);
    // ★ 必须是 Analyze。改前这里拿到的是 Vad(上一轮被取消的 reason 留在成员里没人清)。
    CHECK(r.out.takeAnalysisDone() == ScvbOutputAudioProcessor::AnalysisDoneReason::Analyze);
}

// ---------------------------------------------------------------------------
// [SL-263] 端到端:`analysis.loudness_mode` 必须真的走到平衡产出。
//
// 补的是 SL-252 复审点名、当时**没能钉住**的那一档:删掉 `OutputProcessor.cpp` 里
// `cfg.balance.loudnessMode = parseLoudnessMode(...)` 那一行(= 完全回到断链状态),
// core 侧的流水线用例照样全绿 —— 它们自己装配 `PipelineConfig`,整条路径不经
// `startAnalysis`。判例:`OutputEditor.cpp` 的 HOST R4 注释「否则测试照绿而 bug 回归」。
//
// **必须多轨**:平衡层的 z 只在轨与轨之间才有意义(`zHat = z / zSum`),单轨时 zSum
// 就是它自己 ⇒ zHat ≡ 1,换任何档产出都一样,那样的用例永远绿。
//
// 用**既有的** `MonoMultiRig`(3 轨),不另造机台 —— #171 复审【重要】:本文件早就有它、
// ~25 条用例在用,真正缺的只有「`setAnalysisConfig` 那一跳没人调」与「全轨全段展平快照」
// 两样,已就地补成 `pansOf()` / `volsOf()`。
//
// ---------------------------------------------------------------------------
// [SL-273] **本用例此前测的是浮点噪声,改的就是这一处。**
//
// 旧版的机理注释写着:「三轨幅度不同,而均值 ∝ amp²、峰值 ∝ amp —— 两种基准给出的
// 相对能量排序不同,指派结果因此分叉」。**两句都错**:
//   · `peak_dbfs` 档返回的是 `max(peak)²`(`BalanceBasis.h` 该分支逐字),与均值一样
//     ∝ amp²。三轨互为标量倍时,三档 z 全都正比于同一个 c_i²,换档 = **共模缩放**,
//     而平衡解对共模缩放严格不变 ⇒ 换档本应**逐位不变**。幅度差多大都不产生分歧。
//   · 变的也不是「指派结果」:指派代价 `baseCost()` 一个字都不读 z(见
//     tests/core/test_analysis_pipeline.cpp 里 [SL252/SL-273] 那条的头注)。
// 旧版实测到的 maxDiff ≈ 0.0011 是每轨 K 加权 IIR 在 float 下各自舍入的残差,
// 于是阈值只好一路调到 1e-6 —— 那是拿浮点噪声当判据,断链时它同样可能非零。
//
// 改法:开 `variedCrest`,让三轨**峰值因数**差一个数量级(正弦 / 窄脉冲串 / 方波,
// 见 `MonoMultiRig::fillVaried` 头注)。峰值因数一离散,`max(peak)²` 与 `mean(kw)`
// 的比就真的按轨不同,换档才有**设计出来的**信号。断言随之分成两路:
//   · **volDb 必须真变**,且幅度要够大(阈值按 UI 显示步长 0.1 dB 定,不是按噪声定);
//   · **pan 逐位不动** —— 指派代价不读 z 的直接推论,也是「vol 真变」不是随机抖动的
//     旁证:真要是数值噪声在动,pan 不会一位不差地对上。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL263:换 loudness_mode → 重分析产出真变(钉住 startAnalysis 那一跳)", "[host][analyze][loudness][SL263]")
{
    MonoMultiRig r;
    // [SL-273] 三轨波形各异(峰值因数拉开一个数量级)。**必须在采集之前置位** ——
    // `capture()` 内部就在跑 `runBlocks`,晚一步这份素材就进不了特征环。
    r.variedCrest = true;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());
    REQUIRE(r.capture() > 1.0);

    // 分析窗取**真实覆盖区间**,不能拿 `coveredS`(它是覆盖**时长**)当结束时刻 ——
    // 采集是在播放头已经走出去一段之后才打开的,`[0, coveredS)` 与真实覆盖区只是部分相交,
    // 相交多少取决于机器负载(#171 复审【重要】)。
    // [SL-292] 这七行原本写在这儿,现已抽成 `MonoMultiRig::coverageWindow()` ——
    // 抽出来的当轮就把这里回填掉,否则同一口径两个落点,改 helper 时这边不会跟着走。
    const auto win = r.coverageWindow();
    REQUIRE(win.endS > win.startS);
    const double startS = win.startS;
    const double endS = win.endS;

    // [SL-284] 连 fallback level 一起带回来:下面要**先断前提再断推论**。
    struct Run
    {
        std::vector<double> pans;
        std::vector<double> vols;
        int level = 0;
    };
    const auto analyze = [&r, startS, endS](const char* mode) {
        r.out.setAnalysisConfig(mode, "", /*hasLoudness=*/true, /*hasCenter=*/false);
        REQUIRE(r.runAnalysisIn(startS, endS, /*clearManual=*/true));
        return Run{r.pansOf(), r.volsOf(), r.fallbackLevel()};
    };

    // 用具名 vector 而不是结构化绑定:Catch2 的 INFO/CAPTURE 宏会把表达式塞进
    // 内部对象里,而 C++17 的结构化绑定名在有些捕获形态下不可用 —— 换个写法就没这事。
    const Run runK = analyze("kw_integrated");
    const std::vector<double> panK = runK.pans;
    const std::vector<double> volK = runK.vols;
    REQUIRE_FALSE(panK.empty()); // 没段就什么都测不出来 —— 素材不够时这里先红

    const Run runP = analyze("peak_dbfs");
    const std::vector<double> panP = runP.pans;
    const std::vector<double> volP = runP.vols;
    REQUIRE(panP.size() == panK.size());
    REQUIRE(volP.size() == volK.size());

    // ---- 前提:两次分析的**每个区间**首趟 solveBalance 都收敛(level 1)----------
    //
    // [SL-284] 这条必须在下面 pan 那条**之前**断,而且是 `== 1` 不是 `<= 1`:
    //   · 掉到 level 2 ⇒ `balHint->zHat` 进了指派代价 ⇒ pan **本来就允许跟着 z 动**,
    //     那时下面「pan 逐位不动」失效是**预期**,不是回归 —— 红在这一条才说得清原因;
    //   · 读到 0 ⇒ 这次压根没跑过平衡(没有区间参与),下面两条断言全是空过,
    //     必须显式红,不能把「没跑」当成「收敛」。
    // 换句话说:下面 pan 那条断的是**推论**,这条断的是它赖以成立的**前提**。
    // 前提断了才轮得到推论;前提破了直接在这里说清「首趟未收敛」,不必让人从 pan 差值倒推。
    INFO("kw 档最坏回退级 = " << runK.level << ",peak 档 = " << runP.level
                              << "(1 = 每个区间首趟都收敛;>=2 = 首趟未收敛,z 已进指派代价;"
                                 "0 = 本次没有区间跑过平衡)");
    REQUIRE(runK.level == 1);
    REQUIRE(runP.level == 1);

    // ---- volDb:必须真变,阈值按**用户看得见**定 -----------------------------
    // 旧版这里是 `> 1e-6`。那个数是被机台逼出来的:三轨互为标量倍时换档本该逐位不变,
    // 实测到的 1e-3 只是 float 舍入残差,阈值只能贴着噪声定。素材换成峰值因数各异之后,
    // 信号是**设计出来的**,于是阈值也回到有物理意义的那把尺:0.1 dB = UI 显示步长
    // (`fmtSigned(x, 1)`),比它小的差用户根本读不出来,不配当「换档真的传到了」的证据。
    // **将来这里变红,先查机台素材的峰值因数还在不在,不是把阈值调小。**
    double maxVolDiff = 0.0;
    for (std::size_t i = 0; i < volP.size(); ++i)
    {
        maxVolDiff = std::max(maxVolDiff, std::abs(volP[i] - volK[i]));
    }
    INFO("换档前后 volDb 最大差 = " << maxVolDiff);
    CHECK(maxVolDiff > 0.1); // 删掉 startAnalysis 那行赋值 / 把 balanceBasisZ 换回 meanKw ⇒ 必红

    // ---- pan:逐位不动 --------------------------------------------------------
    // 指派代价不读 z 的直接推论。它同时是上面那条的**对照**:若 volDb 的差来自数值噪声
    // 而非换档,pan 不可能一位不差地对上。
    //
    // 「pan 不动」成立的前提是**首趟 `solveBalance` 收敛**(level 1)—— 只有落到
    // `solveBalanceWithFallback` 的 level 2,`balHint->zHat` 才进 `entryCost`,pan 才会
    // 跟着 z 动。core 侧 `[SL252/SL-273]` 那条正因为这个理由**拒绝**把 rms 档的 pan 写成
    // 断言(「取决于收不收敛、素材一动就可能翻面」);host 侧 peak 档吃的是同一个条件。
    //
    // ⚠ [SL-284] **那个前提现在由上面 `runK.level == 1` / `runP.level == 1` 直接断着**,
    // 不再靠这里的注释提醒人去查。所以本条红 = 前提成立(首趟收敛)**却**出现了 pan 变动,
    // 那就是真回归:有人把 z 引进了 `baseCost()`。
    // **不要放宽成近似比较** —— 放宽等于把这条断言钉的那件事(指派代价不读 z)整个放掉。
    // (改素材/换编译器把解推进 level 2 时,红的是上面那条前提,不是这一条。)
    for (std::size_t i = 0; i < panP.size(); ++i)
    {
        INFO("段 " << i << ":pan " << panK[i] << " -> " << panP[i]
                   << "(前提已断:两档均 level 1 ⇒ 此处变动是 baseCost 读了 z 的真回归)");
        CHECK(panP[i] == panK[i]);
    }
}

// ---------------------------------------------------------------------------
// [SL-284] fallback level 这条前提断言**必须有牙**:它得报得出 ≠1。
//
// 为什么单开一条:上面 `HOST SL263` 现在先断 `level == 1` 再断「pan 逐位不动」。
// 但**如果 `lastMaxFallbackLevel()` 因为哪天接线断了而恒返回 1(或恒返回 0 被写成 <=1),
// 那条前提就永远成立、永远绿** —— 一条恒真的判据比没有判据更坏,因为它看起来在守。
// 所以这里把回退链**真的推到 level ≥ 2**,证明这条通路报得出「首趟没收敛」。
// 这就是那条前提断言的删之即红通路:把 `finishAnalysis` 里那行记账删掉、或把管线里
// `result.maxFallbackLevel` 的赋值删掉 ⇒ 本条立刻红(读到 0 或 1)。
//
// 怎么把它推下去(配方照搬 core 侧 `tests/core/test_balance.cpp` 的「回退链 level 2」):
// **一条高能量轨被冻结在硬左**,剩下的轨没有足够杠杆把 D 拉回容差内 ⇒ 首趟 solveBalance
// 不收敛 ⇒ 进 level 2 的平衡感知重指派。本机台 ch3 幅度最大(`0.4+0.3*i`,i=2 ⇒ 1.0),
// 所以冻结 ch3 造成的不平衡最大。
//
// ⚠ 分析必须走 `clearManual=false`:`true` 会**清掉冻结位**(见 `HOST P0-3`),
// 那样冻结轨在分析时又变回自由轨,不平衡消失、level 掉回 1,本条就成了空过。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL284:高能量轨冻结硬左 → 首趟不收敛,回退级报得出 >=2", "[host][analyze][balance][SL284]")
{
    MonoMultiRig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double coveredS = r.capture();
    REQUIRE(coveredS > 0.0);

    // [SL-292] 分析窗取**真实覆盖区间**,不能拿 `capture()` 的返回值当结束时刻 ——
    // 它是覆盖**时长**,而本 rig 的采集在播放头走出去之后才打开,覆盖区不从 0 起。
    // `[0, coveredS)` 与真实覆盖区只是部分相交,相交多少取决于机器负载:慢机 corner 下
    // 窗口可能几乎不含素材 ⇒ 下面那条基线 `REQUIRE(level == 1)` 会红在 0(压根没跑分析),
    // **红错原因**。与 `HOST SL263` 那段逐字同款(#171 复审【重要】)。
    const auto win = r.coverageWindow();
    REQUIRE(win.endS > win.startS);

    // [SL-292] **把「时长 ≠ 时刻」这件事本身钉成断言**,而不是只写在注释里。
    // 造不出慢机 corner(本机台素材充足,拿错窗口照样能跑出 level 1),所以退一层,
    // 钉住让旧写法出错的那个**前提**:采集在播放头走出去之后才开 ⇒ 覆盖区不从 0 起
    // ⇒ `startS > 0` ⇒ `endS`(时刻)必然大于 `coveredS`(时长)。
    // 这两条一旦不成立,说明 rig 的采集时序变了,那时 `[0, coveredS)` 与真实覆盖区的
    // 关系也跟着变,本条注释与上面的取窗都要重新想一遍 —— 所以它们是判据不是装饰。
    // `startS > 0` 的余量只有**一个 hop 出头**,它依赖这个前提:
    //   `waitUntilInjected()` 每轮至少跑一次 `runBlocks(2, …)` ⇒ 开采集前播放头已走
    //   `2 × kBlock / kSr = 2 × 512 / 48000 ≈ 21.3 ms`,而 `featHopSeconds() = 10 ms`
    //   ⇒ 首个覆盖 hop 索引 ≥ 2 ⇒ `startS ≥ 0.02`。
    // **`kBlock` 若降到 128,2 块只有 5.3 ms < 10 ms,首个覆盖 hop 落在索引 0,这条直接红**
    // —— 而红出来看着像「rig 采集时序变了」,其实只是块长变了。红在这里先看这个前提。
    INFO("覆盖区 = [" << win.startS << ", " << win.endS << ") 秒;coveredS(时长)= " << coveredS);
    REQUIRE(win.startS > 0.0);
    REQUIRE(win.endS > coveredS);

    // ① 基线:不动任何东西,本机台素材首趟就收敛。
    //    这一半同样重要 —— 它证明下面的 >=2 是**冻结造成的**,不是这台机台本来就不收敛。
    REQUIRE(r.runAnalysisIn(win.startS, win.endS, /*clearManual=*/true));
    INFO("基线(无冻结)回退级 = " << r.fallbackLevel());
    REQUIRE(r.fallbackLevel() == 1);

    // ② 把**两条较响的轨**(ch2/ch3)一起手动钉到硬左并冻结 pan 维。
    //
    // 为什么是两条不是一条:本机台幅度是 `amp*(0.4+0.3*i)`,能量比 ≈ 0.16 : 0.49 : 1.0。
    // **只冻 ch3 实测仍收敛(level 1)** —— 它占约 6 成能量,剩下两条自由轨往右摆再加上
    // ±4 dB 的 u 杠杆,足够把 D 拉回 0.3 LU 的容差内。冻住 ch2+ch3 ⇒ 约 9 成能量钉死在
    // 硬左,只剩最弱的 ch1(约 1 成)可动,杠杆不够 ⇒ 首趟必不收敛。
    // 这个数字是**实测出来的**,不是估的:一条不够就是一条不够。
    constexpr float kHardLeft = -100.0f;
    auto& apvts = r.out.getAPVTS();
    for (const int ch : {2, 3})
    {
        int replaced = 0;
        int replacedLocked = 0;
        REQUIRE(r.out.setTrackManual(ch, /*isPan=*/true, kHardLeft, replaced, replacedLocked));
        const juce::String frzId = scvb::params::freezeId(r.out.versionActive(), ch);
        auto* frz = apvts.getParameter(frzId);
        REQUIRE(frz != nullptr);
        frz->beginChangeGesture();
        frz->setValueNotifyingHost(frz->convertTo0to1(1.0f)); // bit0 = pan 维冻结
        frz->endChangeGesture();
        MonoMultiRig::pump(100);
        REQUIRE(juce::roundToInt(apvts.getRawParameterValue(frzId)->load()) == 1);
    }

    // ③ 保留手动/冻结重分析 ⇒ 首趟解被那两条硬左的响轨拽偏,收不进容差。
    REQUIRE(r.runAnalysisIn(win.startS, win.endS, /*clearManual=*/false));

    // ⚠ **先证明冻结真的进了管线**:若冻结没生效,那两条轨会被当自由轨重新指派,
    // 不平衡根本不存在 —— 那样下面的 level 断言即使红也是**红错了原因**。
    // 冻结维度的 pan 必须原样是手动值(见 `HOST P0-3`:冻结维读参数面)。
    for (const int ch : {2, 3})
    {
        INFO("ch" << ch << " 分析后的 pan = " << r.firstPan(ch) << "(应仍是冻结的手动值)");
        REQUIRE(r.firstPan(ch) == Catch::Approx(static_cast<double>(kHardLeft)));
    }
    const int level = r.fallbackLevel();
    INFO("冻结 ch2/ch3 到 " << kHardLeft << " 后的最坏回退级 = " << level
                            << "(1 = 首趟收敛;>=2 = 首趟未收敛,已进平衡感知重指派)");
    // 断 `>= 2` 而不是 `== 2`:落 2/3/4 取决于剩余杠杆够不够,那是回退链自己的分级,
    // 本条要钉的只是「这条通路报得出『首趟没收敛』」。钉死某一级会让它随素材翻面。
    CHECK(level >= 2);
}

// ---------------------------------------------------------------------------
// [SL-263] `segmentLoudnessLufs` 的三个早退分支(SL-257 那一支此前零用例)。
// ---------------------------------------------------------------------------
TEST_CASE("HOST SL263:segmentLoudnessLufs 早退分支回 −120 且不越界", "[host][analyze][loudness][SL263]")
{
    Rig r;
    r.ph.playing = true;
    REQUIRE(r.waitUntilInjected());

    const double kSilent = -120.0; // lufsFromMeanKw(0.0) 的静音替身

    // ① 空窗 / 倒序窗(t1 <= t0)
    CHECK(r.out.segmentLoudnessLufs(kTestChannel, 0, 0) == kSilent);
    CHECK(r.out.segmentLoudnessLufs(kTestChannel, 48000, 0) == kSilent);

    // ② 越界 channel(合法域 1..15)—— 不得越界读 frameStore
    CHECK(r.out.segmentLoudnessLufs(0, 0, 48000) == kSilent);
    CHECK(r.out.segmentLoudnessLufs(16, 0, 48000) == kSilent);
    CHECK(r.out.segmentLoudnessLufs(-1, 0, 48000) == kSilent);

    // ③ 整段未覆盖(远端窗口,没有任何采集数据)⇒ 均值 0 ⇒ 静音替身
    CHECK(r.out.segmentLoudnessLufs(kTestChannel, 48000LL * 3600, 48000LL * 3601) == kSilent);

    // 采一段真素材后,覆盖窗口应当给出**有限且高于静音替身**的值(证明它不是恒回 −120)。
    r.out.setCaptureEnabled(true);
    Rig::pumpMessages(300);
    r.runBlocks(200, 0.5f, 4, 6);
    Rig::pumpMessages(400);
    // ⚠ 窗口必须取**真实覆盖区间**,不能用 `[0, coveredS]`:`coveredS` 是覆盖**时长**,
    // 而播放头在开采集之前就已经走了一段,`[0, 时长]` 未必与实际被覆盖的 hop 区间相交
    // (第一版就是这么写的,拿到 −120 差点误判成实现回归)。
    const auto cov = r.out.coverageOf(kTestChannel, 0.0, 600.0);
    REQUIRE(cov.coveredS > 0.0);
    REQUIRE_FALSE(cov.ranges.empty());
    // 换算走 `analysis/HopMath.h` 的**唯一口径**(#171 复审【重要】):这里原本是
    // `static_cast<int64_t>(featHopSeconds() * kSr)` —— **截断**,而实现侧走 `llround`。
    // 48k/10ms 下两者都是 480 所以当时是绿的,但那等于把 SL-262 刚还掉的债又欠回来
    // (HopMath.h 头注逐字写着它是「采样点 → hop 的唯一换算口径」);哪天乘积落成
    // 479.9999,用例会去问错的 hop 区间、红在一个指不到真因的地方。
    const std::int64_t hopSamples = scvb::analysis::hopSamplesFor(ScvbOutputAudioProcessor::featHopSeconds(), kSr);
    REQUIRE(hopSamples > 0);
    const auto& cr = cov.ranges.front();
    const double real = r.out.segmentLoudnessLufs(kTestChannel, static_cast<std::int64_t>(cr.begin) * hopSamples,
                                                  static_cast<std::int64_t>(cr.end) * hopSamples);
    CHECK(std::isfinite(real));
    CHECK(real > kSilent); // 恒回静音替身(例如整型换算写错成恒 0 窗)时必红
}
