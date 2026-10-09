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
//
// [B 线 M12a] 两平台同一份用例:段后端走 PlatformSegmentBackend,对端经 support/peer_spawn.h 拉起。
// 用例收尾时**先强杀还在 linger 的对端、再 releaseResources()**:Monitor 此刻仍吊着 viz 段,于是它是
// 最后一个离开者,段随它的 unmap 撤掉。反过来(先放 Monitor、再由 PeerGuard 析构强杀对端)在 Windows 上
// 没有区别(内核随句柄关闭回收),在 POSIX 上会留下一个无主的 `/SynchainSCVB.v1.gN.viz` —— 强杀的对端
// 没机会 shm_unlink,而 mac CI 在 ctest 之后用 `scvb_tests "[shm-leftover]"` 查残段,这一条会红在那里。

#include <catch2/catch_test_macros.hpp>

// [SL-324] 同机独占守卫(四套共用一把:Windows `Local\SCVB-tests-proc` / POSIX 按 uid 的 flock)——
// 本套也建段:见该头注的组号重叠表。每个二进制只需在任意一个 TU 里包含它。
#include "support/exclusive_guard.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "MonitorProcessor.h"

#include "ipc/PlatformSegmentBackend.h"
#include "ipc/VizPlane.h"
#include "support/peer_spawn.h"

using scvb::ipctest::peer::killPeer;
using scvb::ipctest::peer::PeerGuard;
using scvb::ipctest::peer::spawnPeer;
using scvb::ipctest::peer::waitPeer;

namespace
{
const std::string kPeer = scvb::ipctest::peer::kIpcPeerName;

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
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// 只推几拍就返回(用于「确认仍是空态」这类不该成立的条件)。
void pumpTicks(ScvbMonitorAudioProcessor& p, int ticks)
{
    for (int i = 0; i < ticks; ++i)
    {
        p.tickMessageThread(scvb::steadyNowMs());
        // > kVizPollIntervalMs/4,确保 attach 重试闸门真的开过
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
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
        // [SL-361] **无分段轨也要有当前值**,回落到参数值 —— 不再留哨兵。
        // 用户 v5.6.7 实测:同一工程 Output 画 10 根、Monitor 只画 7 根。真因在这里:
        // 发布器原来只在「有分段 ∧ 有曲线」时写 panNow/volDb,否则留 kVizPanNone,
        // Monitor 把那一轨整根跳过;而 Output 那侧走「段回读 → 没段就退参数值」
        // (tab-master.js 的 renderDist),所以两边根数对不上。
        // ← 删掉发布器里那个 else 分支(退回只在有段时写),这一格红。
        REQUIRE(v.panNow[2] == scvb::vizPackPan(-25.0));
        REQUIRE(v.volDb[2] == scvb::vizPackFixed(-6.0, scvb::kVizVolDbMin, scvb::kVizVolDbMax));
        // 有段轨:曲线求值**优先**,回落不许盖掉它(对端给轨1 的参数值是 77 / +9,
        // 与段内值差得远)。只钉上面那两条的话,把两支写反照样全绿。
        REQUIRE(v.panNow[0] != scvb::vizPackPan(77.0));
        REQUIRE(v.volDb[0] != scvb::vizPackFixed(9.0, scvb::kVizVolDbMin, scvb::kVizVolDbMax));
        // 两样都没有的轨(对端不给参数值,默认 NaN)**仍是哨兵** —— 「不知道就别编」:
        // 0 对 pan 是正中、对 vol 是 0 dB,都是合法值,回落成 0 会凭空画出一根居中柱,
        // 比整根不画更难发现。
        REQUIRE(v.panNow[3] == scvb::kVizPanNone);
        REQUIRE(v.volDb[3] == scvb::kVizPanNone);
        // [SL-361 复审第 1 轮] **未连接轨即使有参数值也仍是哨兵。**
        // Monitor 的逐轨闸只有「enabled ∧ 非哨兵」,而 enabled 默认全 true;Output 那侧却按
        // 「已连接轨」过滤。少了这道闸,本卡会把 15 条全喂出去 —— 从「少画 3 根」变成「多画」,
        // 方向反了、幅度更大。对端给轨5(索引 4)参数值 33 / +3 但**不在 connectedMask 里**。
        // ← 去掉发布器里那道 connected 闸,只红这两格。
        REQUIRE(v.panNow[4] == scvb::kVizPanNone);
        REQUIRE(v.volDb[4] == scvb::kVizPanNone);
        REQUIRE(v.widthPct[0] == scvb::vizPackFixed(80.0, scvb::kVizWidthMin, scvb::kVizWidthMax));
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

    killPeer(pub2.pi); // [B 线 M12a] 先杀对端、再放 Monitor:Monitor 是最后一个离开者(见文件头注)
    mon.releaseResources();
}

TEST_CASE("MON-CHAIN 写方停摆 → 帧判陈旧(不假装在线)", "[monitor][harness][stale]")
{
    ScvbMonitorAudioProcessor mon;
    mon.prepareToPlay(48000.0, 256);
    REQUIRE(mon.setObservedGroup(4));

    // 写方发一帧就退出;段由本进程的探针句柄吊住,模拟「段还在但写方不再发布」。
    scvb::PlatformSegmentBackend backend;
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
// [SL-192] 升频之后的**跨进程**校验:30Hz 写方 + 60Hz 读方,帧序单调、零撕裂。
//
// 为什么单开一条而不是把既有 MON-CHAIN 改快:那条验的是「链路通不通 + 生命周期」,
// 一次读到就够了;本条验的是**持续高频读写下的一致性**,要的是成百上千次读。
//
// 撕裂能不能被测出来,取决于对端有没有**跨字段不变式**。对端(viz-publisher)因此逐帧
// 推进播放头 —— 帧头里的 `publish_ms` 与 `playhead_samples` 必须同步前进。若某次读把
// 新旧两帧拼在一起,这两个量就会对不上或倒退。**所有字段恒定的对端根本测不出撕裂**:
// 拼接出来的帧与正确帧逐字节相同。
//
// 速率断言刻意**留宽**(≥15 帧 / 1.5s;本机实测 32):这是真进程 + 真调度,CI 上对端被抢占是常事,
// 把 30Hz 卡死在这里只会换来一条抖动的门禁。**精确的 30Hz 断言在进程内的确定性用例里**
// (test_viz_plane.cpp 的 `[viz][publisher][rate]`,按逻辑时钟数帧,不看墙钟)。
// 这里要守住的是「频率上去之后一致性不塌」,那是墙钟测不坏的部分。
TEST_CASE("MON-CHAIN 30Hz 持续读写:帧序单调、seq 恒偶、零撕裂", "[monitor][harness][rate]")
{
    constexpr int kGroup = 7;
    ScvbMonitorAudioProcessor mon;
    mon.prepareToPlay(48000.0, 256);
    REQUIRE(mon.setObservedGroup(kGroup));

    PeerGuard pub;
    int err = 0;
    pub.pi = spawnPeer(kPeer, {"--role=viz-publisher", "--group=7", "--sr=48000", "--linger-ms=9000"}, &err);
    REQUIRE(err == 0);
    REQUIRE(pumpUntil(mon, [&] { return vizOnline(mon) && mon.vizFresh(); }));

    std::uint64_t lastPublishMs = 0;
    std::int64_t lastPlayhead = -1;
    std::uint32_t lastSeq = 0;
    int distinctFrames = 0;
    int oddSeq = 0;
    int wentBackwards = 0;
    bool first = true;

    // 按生产读方的 60Hz 拍子推 1.5 秒。
    const std::uint64_t until = scvb::steadyNowMs() + 1500;
    while (scvb::steadyNowMs() < until)
    {
        mon.tickMessageThread(scvb::steadyNowMs());
        const auto& v = mon.vizSnapshot();

        // seqlock 的读侧保证:交到调用方手里的 seq 永远是偶数(奇数 = 写方在临界区内,
        // `VizPlane::read()` 会重试而不是把那一帧交出来)。
        if ((v.seq & 1u) != 0u)
        {
            ++oddSeq;
        }

        if (first)
        {
            first = false;
        }
        else if (v.publishMs != lastPublishMs)
        {
            ++distinctFrames;
            // 单调:时刻与播放头都只能前进。任一倒退 = 读到了拼接帧。
            if (v.publishMs < lastPublishMs || v.playheadSamples < lastPlayhead || v.seq < lastSeq)
            {
                ++wentBackwards;
            }
        }
        lastPublishMs = v.publishMs;
        lastPlayhead = v.playheadSamples;
        lastSeq = v.seq;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / 60));
    }

    INFO("distinct frames observed in 1.5s = " << distinctFrames);
    REQUIRE(oddSeq == 0); // 一次都不许把写入中的帧交出去
    REQUIRE(wentBackwards == 0); // 一次都不许倒退 —— 那就是撕裂
    // 旧的 4Hz 在 1.5s 内**最多** 6 帧 —— 15 这条线把「退回 250ms」判得死死的,
    // 同时对调度抖动留了一半余量(本机实测 32)。
    REQUIRE(distinctFrames >= 15);
    REQUIRE(mon.vizFresh());

    killPeer(pub.pi); // [B 线 M12a] 先杀对端、再放 Monitor:Monitor 是最后一个离开者(见文件头注)
    mon.releaseResources();
}

// ============================================================================
// TEMP EXPERIMENT (M12a-fix) -- measure the MON-CHAIN 30Hz distribution; reverted before the PR.
// ============================================================================
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/task_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/resource.h>
#endif

namespace
{
struct XStats
{
    double min = 0, p50 = 0, p90 = 0, max = 0;
};

XStats xstats(std::vector<double> v)
{
    XStats s;
    if (v.empty())
    {
        return s;
    }
    std::sort(v.begin(), v.end());
    s.min = v.front();
    s.max = v.back();
    s.p50 = v[v.size() / 2];
    s.p90 = v[(v.size() * 9) / 10];
    return s;
}

double xmsSince(std::chrono::steady_clock::time_point t0)
{
    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()) /
           1000.0;
}

XStats xsleepStats(int ms, int n)
{
    std::vector<double> d;
    for (int i = 0; i < n; ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        d.push_back(xmsSince(t0));
    }
    return xstats(d);
}

void xprintEnv(const char* tag)
{
#if defined(__APPLE__)
    const unsigned q = static_cast<unsigned>(qos_class_self());
    errno = 0;
    const int bgp = getpriority(PRIO_DARWIN_PROCESS, 0);
    const int e1 = errno;
    errno = 0;
    const int bgt = getpriority(PRIO_DARWIN_THREAD, 0);
    const int e2 = errno;
    const int niceV = getpriority(PRIO_PROCESS, 0);
    task_category_policy_data_t cat{};
    mach_msg_type_number_t cnt = TASK_CATEGORY_POLICY_COUNT;
    boolean_t gd = FALSE;
    const kern_return_t kr =
        task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY, reinterpret_cast<task_policy_t>(&cat), &cnt, &gd);
    std::printf("XENV %s qos=0x%x darwinBgProc=%d(errno %d) darwinBgThread=%d(errno %d) nice=%d taskRole=%d kr=%d "
                "getDefault=%d\n",
                tag, q, bgp, e1, bgt, e2, niceV, static_cast<int>(cat.role), static_cast<int>(kr),
                static_cast<int>(gd));
#else
    std::printf("XENV %s (not apple)\n", tag);
#endif
    const XStats s1 = xsleepStats(1, 40);
    const XStats s16 = xsleepStats(16, 30);
    std::printf("XSLEEP %s sleep_for(1ms): min=%.2f p50=%.2f p90=%.2f max=%.2f | sleep_for(16ms): min=%.2f p50=%.2f "
                "p90=%.2f max=%.2f\n",
                tag, s1.min, s1.p50, s1.p90, s1.max, s16.min, s16.p50, s16.p90, s16.max);
    std::fflush(stdout);
}

struct XRep
{
    int ticks = 0;
    int distinct = 0;
    int published = 0;
    double pubHz = 0;
    int lag = 0;
};

XRep xrunRep(int rep, const char* tag)
{
    constexpr int kGroup = 7;
    XRep r;
    ScvbMonitorAudioProcessor mon;
    mon.prepareToPlay(48000.0, 256);
    REQUIRE(mon.setObservedGroup(kGroup));

    PeerGuard pub;
    int err = 0;
    pub.pi = spawnPeer(kPeer, {"--role=viz-publisher", "--group=7", "--sr=48000", "--linger-ms=6000"}, &err);
    REQUIRE(err == 0);
    REQUIRE(pumpUntil(mon, [&] { return vizOnline(mon) && mon.vizFresh(); }));

    scvb::PlatformSegmentBackend backend;
    scvb::VizPlane probe(backend, kGroup);
    REQUIRE(probe.attachReadOnly() == scvb::InitResult::kOk);
    auto probeSnap = std::make_unique<scvb::VizSnapshot>();

    std::vector<double> dts;
    std::vector<double> singleIv; // publish interval when exactly one publish happened between two distinct frames
    std::uint64_t lastPublishMs = 0;
    std::uint32_t lastSeq = 0;
    std::int64_t lastPh = -1;
    std::uint32_t firstSeq = 0;
    std::uint64_t firstPms = 0;
    std::int64_t firstPh = 0;
    bool first = true;
    int maxMissed = 0;
    int odd = 0;
    int back = 0;
    auto tPrev = std::chrono::steady_clock::now();
    const std::uint64_t until = scvb::steadyNowMs() + 1500;
    while (scvb::steadyNowMs() < until)
    {
        std::uint32_t probeSeq = 0;
        if (probe.read(*probeSnap))
        {
            probeSeq = probeSnap->seq;
        }
        mon.tickMessageThread(scvb::steadyNowMs());
        ++r.ticks;
        const auto& v = mon.vizSnapshot();
        if ((v.seq & 1u) != 0u)
        {
            ++odd;
        }
        if (v.seq < probeSeq)
        {
            ++r.lag;
        }
        if (first)
        {
            first = false;
            firstSeq = v.seq;
            firstPms = v.publishMs;
            firstPh = v.playheadSamples;
        }
        else if (v.publishMs != lastPublishMs)
        {
            ++r.distinct;
            const int nPub = static_cast<int>((v.seq - lastSeq) / 2u);
            maxMissed = std::max(maxMissed, nPub - 1);
            if (nPub == 1)
            {
                singleIv.push_back(static_cast<double>(v.publishMs - lastPublishMs));
            }
            if (v.publishMs < lastPublishMs || v.playheadSamples < lastPh || v.seq < lastSeq)
            {
                ++back;
            }
        }
        lastPublishMs = v.publishMs;
        lastPh = v.playheadSamples;
        lastSeq = v.seq;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / 60));
        dts.push_back(xmsSince(tPrev));
        tPrev = std::chrono::steady_clock::now();
    }
    const auto& v = mon.vizSnapshot();
    r.published = static_cast<int>((v.seq - firstSeq) / 2u);
    const double spanMs = static_cast<double>(v.publishMs - firstPms);
    r.pubHz = spanMs > 0 ? 1000.0 * r.published / spanMs : 0.0;
    const double driveIters = static_cast<double>(v.playheadSamples - firstPh) / 1600.0;
    const double driveMs = driveIters > 0 ? spanMs / driveIters : 0.0;
    const XStats d = xstats(dts);
    const XStats iv = xstats(singleIv);
    std::printf("XREP %s %02d ticks=%d tickMs[p50=%.1f p90=%.1f max=%.1f] distinct=%d published=%d spanMs=%.0f "
                "pubHz=%.1f ratio=%.2f peerDriveIters=%.0f peerDriveMs=%.2f pubIvSingle[n=%zu p50=%.0f max=%.0f] "
                "maxMissed=%d lag=%d odd=%d back=%d\n",
                tag, rep, r.ticks, d.p50, d.p90, d.max, r.distinct, r.published, spanMs, r.pubHz,
                r.published > 0 ? static_cast<double>(r.distinct) / r.published : 0.0, driveIters, driveMs,
                singleIv.size(), iv.p50, iv.max, maxMissed, r.lag, odd, back);
    std::fflush(stdout);

    killPeer(pub.pi);
    probe.release();
    mon.releaseResources();
    return r;
}

void xsummary(const char* tag, const std::vector<XRep>& reps)
{
    std::vector<double> dist;
    std::vector<double> hz;
    std::vector<double> pubs;
    int below15 = 0;
    int lagTotal = 0;
    for (const auto& r : reps)
    {
        dist.push_back(r.distinct);
        hz.push_back(r.pubHz);
        pubs.push_back(r.published);
        below15 += r.distinct < 15 ? 1 : 0;
        lagTotal += r.lag;
    }
    const XStats a = xstats(dist);
    const XStats b = xstats(hz);
    const XStats c = xstats(pubs);
    std::printf("XSUM %s reps=%zu distinct[min=%.0f p50=%.0f p90=%.0f max=%.0f] below15=%d published[min=%.0f p50=%.0f "
                "max=%.0f] pubHz[min=%.1f p50=%.1f max=%.1f] lagTotal=%d\n",
                tag, reps.size(), a.min, a.p50, a.p90, a.max, below15, c.min, c.p50, c.max, b.min, b.p50, b.max,
                lagTotal);
    std::fflush(stdout);
}
} // namespace

TEST_CASE("TEMP M12a-fix 30Hz distribution experiment", "[monitor][harness][ratedist]")
{
    xprintEnv("baseline");
    std::vector<XRep> base;
    for (int i = 0; i < 30; ++i)
    {
        base.push_back(xrunRep(i, "base"));
    }
    xsummary("base", base);
    xprintEnv("after-base");

#if defined(__APPLE__)
    const int rcQ = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    std::printf("XSET qos USER_INTERACTIVE rc=%d\n", rcQ);
    xprintEnv("after-qos-ui");
    std::vector<XRep> qos;
    for (int i = 0; i < 5; ++i)
    {
        qos.push_back(xrunRep(i, "qosui"));
    }
    xsummary("qosui", qos);
    errno = 0;
    const int rcB = setpriority(PRIO_DARWIN_PROCESS, 0, 0);
    std::printf("XSET darwin-bg off rc=%d errno=%d\n", rcB, errno);
    xprintEnv("after-bg-off");
    std::vector<XRep> nobg;
    for (int i = 0; i < 5; ++i)
    {
        nobg.push_back(xrunRep(i, "nobg"));
    }
    xsummary("nobg", nobg);
#endif
    FAIL("TEMP experiment: deliberate failure so ctest prints the distribution above");
}
