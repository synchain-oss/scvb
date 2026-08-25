// SPDX-License-Identifier: GPL-3.0-or-later
// test_monitor_harness —— SCVB Monitor **全数据链**双进程 harness(统筹加码要求)。
//
// 与 test_monitor_processor.cpp 的分工:那份在**单进程内**验三条铁律(0 参数 / 直通按位相等 /
// 零写入);本份拉起**真的第二个进程**跑真 VizPublisher(Output 侧发布器),用**真的**
// ScvbMonitorAudioProcessor 走完整条链,把「机器能验的」全部锁死,只把纯 GUI 观察留给 DAW:
//
//   Output 发布 viz  →  Monitor attach 读到  →  组切换  →  Output 退出后空态恢复  →  再上线重连
//
// 对端 = tests/tools/scvb_ipc_peer 的 viz-publisher / viz-writer 角色(真共享内存、真进程退出)。
// 编译时定义 SCVB_MONITOR_HEADLESS —— 不实例化 WebView2(真机 GUI 归 gate 8)。

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "MonitorProcessor.h"

#include "ipc/SegmentBackendWin32.h"
#include "ipc/VizPlane.h"
#include "support/peer_spawn.h"

using scvb::ipctest::peer::PeerGuard;
using scvb::ipctest::peer::spawnPeer;
using scvb::ipctest::peer::waitPeer;

namespace
{
const std::wstring kPeer = L"scvb_ipc_peer.exe";

// 驱动 Monitor 的 [M] 直到条件成立或超时。**必须用真实时钟** —— 对端是真进程,按真实的 4Hz
// 发布;若这里推一个跑得更快的虚拟钟,Monitor 的「帧陈旧」判据(2s 没新帧)会被自己的假时间
// 提前触发,测出一堆假阴性。(第一版就是这么写的,MON-CHAIN 立刻红在 vizFresh() 上。)
template<typename Fn>
bool pumpUntil(ScvbMonitorAudioProcessor& p, Fn&& fn, int timeoutMs = 6000)
{
    const std::uint64_t deadline = scvb::steadyNowMs() + static_cast<std::uint64_t>(timeoutMs);
    for (;;)
    {
        p.tickMessageThread(scvb::steadyNowMs());
        if (fn())
        {
            return true;
        }
        if (scvb::steadyNowMs() >= deadline)
        {
            return false;
        }
        ::Sleep(20);
    }
}

// 只推几拍就返回(用于「确认仍是空态」这类不该成立的条件)。
void pumpTicks(ScvbMonitorAudioProcessor& p, int ticks)
{
    for (int i = 0; i < ticks; ++i)
    {
        p.tickMessageThread(scvb::steadyNowMs());
        ::Sleep(60); // > kVizPollIntervalMs/4,确保 attach 重试闸门真的开过
    }
}

bool vizOnline(const ScvbMonitorAudioProcessor& p)
{
    return p.vizState() == ScvbMonitorAudioProcessor::VizState::kOnline;
}
} // namespace

TEST_CASE("MON-CHAIN 全数据链:发布→读到→组切换→退出空态→重连", "[monitor][harness][chain]")
{
    constexpr int kGroupA = 2; // viz-publisher 造真数据的组
    constexpr int kGroupB = 5; // 没有写方的组(空态)

    ScvbMonitorAudioProcessor mon;
    mon.prepareToPlay(48000.0, 256);
    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer midi;
    buf.clear();

    // ---- ① 起点:A 组无写方 → 空态 ----
    REQUIRE(mon.setObservedGroup(kGroupA));
    pumpTicks(mon, 8);
    REQUIRE(mon.vizState() == ScvbMonitorAudioProcessor::VizState::kOffline);
    REQUIRE_FALSE(mon.vizFresh());

    // ---- ② Output(真 VizPublisher)上线 → Monitor attach 并读到降采样数据 ----
    PeerGuard pub;
    int err = 0;
    pub.pi = spawnPeer(kPeer, {"--role=viz-publisher", "--group=2", "--sr=48000", "--linger-ms=9000"}, &err);
    REQUIRE(err == 0);

    REQUIRE(pumpUntil(mon, [&] { return vizOnline(mon) && mon.vizSnapshot().laneRevision > 0; }));
    REQUIRE(mon.vizFresh());

    {
        // 发布器造的场景:轨1 [0,30s) pan=-50;轨2 [60,90s) pan=+40;其余轨无分段。
        const auto& v = mon.vizSnapshot();
        REQUIRE(v.sampleRate == 48000);
        REQUIRE(v.windowSpanSamples == 48000ull * 90); // 跨度量化到 30s 边界
        REQUIRE(v.coveredMask == 0x0003);
        REQUIRE(v.pan[0][0] == scvb::vizPackPan(-50.0));
        REQUIRE(v.covered(0, 0));
        // 轨2 前 60s 断线、末列有覆盖 —— 断线口径穿过整条链没走样。
        REQUIRE_FALSE(v.covered(1, 0));
        REQUIRE(v.covered(1, scvb::kVizColumns - 1));
        REQUIRE(v.pan[1][scvb::kVizColumns - 1] == scvb::vizPackPan(40.0));
        // 无分段轨:整条哨兵 + 零覆盖。
        REQUIRE(v.pan[2][0] == scvb::kVizPanNone);
        REQUIRE_FALSE(v.covered(2, 0));
        // 每轨当前值与轨名(T46 的分布图/图例数据面)。
        REQUIRE(v.panNow[0] == scvb::vizPackPan(-50.0)); // 播放头在 0s,轨1 段内 pan=-50
        REQUIRE(v.widthPct[0] == scvb::vizPackFixed(80.0));
        REQUIRE(v.label[0] == "Lead");
        REQUIRE(v.trackColor[14] == 15);
    }

    // 音频线程同时在跑:整条链不受影响,且 buffer 逐样本不变。
    juce::AudioBuffer<float> ref;
    for (int c = 0; c < buf.getNumChannels(); ++c)
    {
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            buf.setSample(c, i, 0.25f * static_cast<float>((i % 7) - 3));
        }
    }
    ref.makeCopyOf(buf);
    for (int i = 0; i < 100; ++i)
    {
        mon.processBlock(buf, midi);
    }
    for (int c = 0; c < buf.getNumChannels(); ++c)
    {
        REQUIRE(std::memcmp(buf.getReadPointer(c), ref.getReadPointer(c),
                            static_cast<std::size_t>(buf.getNumSamples()) * sizeof(float)) == 0);
    }
    REQUIRE(vizOnline(mon)); // 音频跑了 100 块,链路没掉

    // ---- ③ 组切换到没有写方的 B 组 → 立刻空态,且不残留 A 组车道 ----
    REQUIRE(mon.setObservedGroup(kGroupB));
    REQUIRE(mon.vizSnapshot().pan[0][0] == scvb::kVizPanNone); // 换组即清
    pumpTicks(mon, 8);
    REQUIRE(mon.vizState() == ScvbMonitorAudioProcessor::VizState::kOffline);
    REQUIRE_FALSE(mon.vizFresh());

    // ---- ④ 切回 A 组 → 重新读到(Output 仍在线)----
    REQUIRE(mon.setObservedGroup(kGroupA));
    REQUIRE(pumpUntil(mon, [&] { return vizOnline(mon) && mon.vizSnapshot().laneRevision > 0; }));
    REQUIRE(mon.vizSnapshot().pan[0][0] == scvb::vizPackPan(-50.0));

    // ---- ⑤ Output 进程退出 → Monitor 回空态(不显示僵尸数据)----
    REQUIRE(waitPeer(pub.pi, 30000) == 0);
    REQUIRE(pumpUntil(mon, [&] { return mon.vizState() == ScvbMonitorAudioProcessor::VizState::kOffline; }));
    REQUIRE_FALSE(mon.vizFresh());

    // ---- ⑥ Output 再上线 → Monitor 自动重连,拿到**新一代**段(不是上一代残留)----
    PeerGuard pub2;
    pub2.pi = spawnPeer(kPeer, {"--role=viz-writer", "--group=2", "--sr=44100", "--linger-ms=6000"}, &err);
    REQUIRE(err == 0);
    REQUIRE(pumpUntil(mon, [&] { return vizOnline(mon) && mon.vizSnapshot().sampleRate == 44100; }));
    REQUIRE(mon.vizSnapshot().windowSpanSamples == 44100ull * 120);
    REQUIRE(mon.vizFresh());

    mon.releaseResources();
}

TEST_CASE("MON-CHAIN 写方停摆 → 帧判陈旧(不假装在线)", "[monitor][harness][stale]")
{
    ScvbMonitorAudioProcessor mon;
    mon.prepareToPlay(48000.0, 256);
    REQUIRE(mon.setObservedGroup(4));

    // 写方发一帧就退出;段由本进程的探针句柄吊住,模拟「段还在但写方不再发布」。
    scvb::SegmentBackendWin32 backend;
    scvb::VizPlane keepAlive(backend, 4);
    {
        PeerGuard w;
        int err = 0;
        w.pi = spawnPeer(kPeer, {"--role=viz-writer", "--group=4", "--sr=48000", "--linger-ms=2500"}, &err);
        REQUIRE(err == 0);
        REQUIRE(pumpUntil(mon, [&] { return vizOnline(mon) && mon.vizFresh(); }));
        REQUIRE(keepAlive.attachReadOnly() == scvb::InitResult::kOk); // 吊住段,写方退出后段不消失
        REQUIRE(waitPeer(w.pi, 20000) == 0);
    }

    // 段仍在(attach 成功),但 publish_ms 不再推进 → 超过 2s 判陈旧。
    std::uint64_t t = scvb::steadyNowMs();
    bool wentStale = false;
    for (int i = 0; i < 40 && !wentStale; ++i)
    {
        t += 250;
        mon.tickMessageThread(t);
        wentStale = vizOnline(mon) && !mon.vizFresh();
    }
    REQUIRE(wentStale); // 在线但陈旧 —— UI 显示 stalled,不是假装数据还在更新
    mon.releaseResources();
}
