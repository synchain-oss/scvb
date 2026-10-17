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
// **精确的 30Hz 断言在进程内的确定性用例里**(test_viz_plane.cpp 的 `[viz][publisher][rate]`,
// 按逻辑时钟数帧,不看墙钟)。这里是真进程 + 真调度,速率只设一条判死「退回 250ms / 4Hz」的粗线。
//
// [B 线 M12a-fix] 速率判据不再数「观察方看见几次换帧」。旧判据 `distinctFrames >= 15`(1.5s 窗口)
// 在 mac CI 上偶发红(实得 14)。同一场景在 CI macos-15 arm64 上重复 30 次的实测:
//   · 对端每个窗口真发出去 47-54 帧(33.9-36.8Hz),发布方没有问题;
//   · 观察方 1.5s 里只醒来 15-27 次 —— 本进程 `sleep_for(16ms)` 实得 p50 约 90-110ms、最长 145ms,
//     `sleep_for(1ms)` 实得 p50 约 9ms(测试主线程 QoS = UTILITY)。旧判据的上界就是醒来次数,
//     所以它量的是观察方自己睡得准不准,不是被测的发布率;30 次里实得 14-24,1 次低于 15。
//   · 同一场景在 Windows(本机与 CI 各 30 次)上:观察方每窗口 89-91 拍,看见 45-48 帧,与对端自报的
//     发布帧数相同(60 个窗口里只有 1 个少看了 1 帧)。
// 现在拆成三条,每条只依赖它要判的那一方:
//   ① 发布率 —— 用对端**自报**的帧数:seqlock 每发一帧 seq +2(`VizPlane::publish` 进出临界区各 +1),
//      窗口首末两次读到的 seq 之差 / 2 = 这段时间里对端真发出去的帧数,观察方中间漏看几帧都不影响;
//      除以对端自己的 `publish_ms` 跨度。线仍是旧的「15 帧 / 1.5s」= 10Hz。
//   ② 读方每拍都交出最新帧 —— 用例自己再挂一个只读探针,每拍在 Monitor 之前读一次段;Monitor 这一拍
//      交出的帧不许比探针刚看到的旧。它判的是「读方这一拍有没有去读」,不数看见了几帧。灵敏度有边界:
//      读闸周期为 G 时,只有拍距 < G 的那些拍会落后。mac CI 上各窗口拍距的中位数是 32-125ms、
//      单拍最长约 145ms,要判死的「退回 4Hz」(250ms)读闸在两平台上都远大于拍距;周期接近拍距的
//      读闸(例如 ~100ms)在 mac 上只会让一部分拍子落后,不保证越过 10% 线 —— 本条不是通用的读方速率门。
//   ③ 一致性检查的覆盖 —— 至少跨过 15 次换帧;观察方睡得慢就把窗口拉长(最短 1.5s、最长 6s)。
//      它只保证上面的撕裂判据不是空转,不再承担判速率的职责。
TEST_CASE("MON-CHAIN 30Hz 持续读写:帧序单调、seq 恒偶、零撕裂", "[monitor][harness][rate]")
{
    constexpr int kGroup = 7;
    ScvbMonitorAudioProcessor mon;
    mon.prepareToPlay(48000.0, 256);
    REQUIRE(mon.setObservedGroup(kGroup));

    // linger 要盖住「attach 等待 + 最长 6s 窗口」;用例收尾时会主动杀掉它。
    PeerGuard pub;
    int err = 0;
    pub.pi = spawnPeer(kPeer, {"--role=viz-publisher", "--group=7", "--sr=48000", "--linger-ms=12000"}, &err);
    REQUIRE(err == 0);
    REQUIRE(pumpUntil(mon, [&] { return vizOnline(mon) && mon.vizFresh(); }));

    // ② 的参照探针:与 Monitor 同一种只读 attach(不建段、不写段)。
    scvb::PlatformSegmentBackend probeBackend;
    scvb::VizPlane probe(probeBackend, kGroup);
    REQUIRE(probe.attachReadOnly() == scvb::InitResult::kOk);
    auto probeSnap = std::make_unique<scvb::VizSnapshot>(); // ≈32KB,堆上

    std::uint64_t lastPublishMs = 0;
    std::int64_t lastPlayhead = -1;
    std::uint32_t lastSeq = 0;
    std::uint64_t firstPublishMs = 0;
    std::uint32_t firstSeq = 0;
    int ticks = 0;
    int staleTicks = 0;
    int distinctFrames = 0;
    int oddSeq = 0;
    int wentBackwards = 0;
    bool first = true;

    constexpr std::uint64_t kMinWindowMs = 1500;
    constexpr std::uint64_t kMaxWindowMs = 6000;
    constexpr int kMinDistinctFrames = 15;

    // 按生产读方的 60Hz 拍子推,至少 1.5 秒。
    const std::uint64_t start = scvb::steadyNowMs();
    for (;;)
    {
        const std::uint64_t elapsed = scvb::steadyNowMs() - start;
        if (elapsed >= kMaxWindowMs || (elapsed >= kMinWindowMs && distinctFrames >= kMinDistinctFrames))
        {
            break;
        }

        // 先探针、后 Monitor:探针读到的是「这一拍开始前对端已经发出的最新帧」。
        // 探针这一次读失败(连续撕裂)就不拿这一拍判 ②。
        const bool probeOk = probe.read(*probeSnap);
        const std::uint32_t probeSeq = probeOk ? probeSnap->seq : 0u;

        mon.tickMessageThread(scvb::steadyNowMs());
        ++ticks;
        const auto& v = mon.vizSnapshot();

        // seqlock 的读侧保证:交到调用方手里的 seq 永远是偶数(奇数 = 写方在临界区内,
        // `VizPlane::read()` 会重试而不是把那一帧交出来)。
        if ((v.seq & 1u) != 0u)
        {
            ++oddSeq;
        }
        if (probeOk && v.seq < probeSeq)
        {
            ++staleTicks;
        }

        if (first)
        {
            first = false;
            firstPublishMs = v.publishMs;
            firstSeq = v.seq;
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
    const std::uint64_t windowMs = scvb::steadyNowMs() - start;

    // ① 对端自报的发布帧数与它自己时钟上的跨度。
    const std::uint32_t published = (lastSeq - firstSeq) / 2u;
    const std::uint64_t publisherSpanMs = lastPublishMs - firstPublishMs;
    const double publishedHz =
        publisherSpanMs > 0 ? 1000.0 * static_cast<double>(published) / static_cast<double>(publisherSpanMs) : 0.0;

    INFO("window " << windowMs << "ms, observer ticks " << ticks << ", distinct frames seen " << distinctFrames
                   << ", stale ticks " << staleTicks);
    INFO("publisher self-reported: " << published << " frames over " << publisherSpanMs << "ms = " << publishedHz
                                     << "Hz");
    // 各条都用 CHECK:一条红时其余几条照样出结论,红在哪一条就指认是哪一方。
    CHECK(oddSeq == 0); // 一次都不许把写入中的帧交出去
    CHECK(wentBackwards == 0); // 一次都不许倒退 —— 那就是撕裂
    // ① 旧的 250ms 闸门实得 ≤ 4Hz,10Hz 这条线把它判死;实测 mac 33.9-36.8Hz、Windows 30.3-32.0Hz。
    //    跨度下限保证这个速率至少是在一秒的对端时间上量出来的。
    CHECK(publisherSpanMs >= 1000);
    CHECK(publishedHz >= 10.0);
    // ② 正确的读方只在「连续 kVizReadRetries 次撕裂、沿用上帧」时才会落后一拍,留 10% 给它;
    //    分布实验里两平台共 100 个窗口一次都没有。
    CHECK(staleTicks * 10 <= ticks);
    // ③
    CHECK(distinctFrames >= kMinDistinctFrames);
    REQUIRE(mon.vizFresh());

    killPeer(pub.pi); // [B 线 M12a] 先杀对端、再放 Monitor:Monitor 是最后一个离开者(见文件头注)
    probe.release(); // 探针也在 Monitor 之前松手,同一个理由
    mon.releaseResources();
}
