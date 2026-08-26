// SPDX-License-Identifier: GPL-3.0-or-later
// test_monitor_processor —— SCVB Monitor(T45 / J75)三条铁律的断言:
//   ① 0 自动化参数(Output 的 123 冻结参数面一个不动)
//   ② 音频直通:processBlock 输出与输入**逐样本按位相等**(T42 旁路口径,memcmp 而非近似)
//   ③ 对任何共享段零写入(viz 段字节不变、registry 段绝不被 Monitor 创建)
// 编译时定义 SCVB_MONITOR_HEADLESS —— 不实例化 WebView2 编辑器(真机 GUI 归 gate 8)。

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "MonitorProcessor.h"

#include "ipc/SegmentBackendWin32.h"
#include "ipc/VizPlane.h"

namespace
{
// 灌一段「难缠」的样本:正负、极小(非规格化)、极大、整数点、0/-0 —— 任何一次乘法/加法
// 都可能改变位模式,直通若不是「原样不动」就会被 memcmp 抓住。
void fillTricky(juce::AudioBuffer<float>& buf, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.5f, 1.5f);
    const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, 1e-38f, -1e-38f, 3.4e38f, -3.4e38f, 1.1754944e-38f, 5e-45f};
    for (int c = 0; c < buf.getNumChannels(); ++c)
    {
        auto* p = buf.getWritePointer(c);
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            p[i] = (i < static_cast<int>(std::size(specials))) ? specials[i] : dist(rng);
        }
    }
}

// 当前存在哪些组的 registry 段(位图)。用于断言「Monitor 没有**新建**任何一个」——
// 而不是断言「一个都不存在」,后者会被同机的其它 SCVB 进程打红。
scvb::u32 registrySnapshot(scvb::ISegmentBackend& backend)
{
    scvb::u32 mask = 0;
    for (scvb::u32 g = 1; g <= scvb::kMaxGroups; ++g)
    {
        scvb::SegmentView view;
        if (backend.openExistingReadOnly(L"Local\\" + scvb::segmentLogicalName(g, scvb::SegmentKind::kRegistry),
                                         view) == scvb::InitResult::kOk)
        {
            mask |= (1u << (g - 1));
            backend.unmap(view);
        }
    }
    return mask;
}

bool bitwiseEqual(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
    {
        return false;
    }
    const auto bytes = static_cast<std::size_t>(a.getNumSamples()) * sizeof(float);
    for (int c = 0; c < a.getNumChannels(); ++c)
    {
        if (std::memcmp(a.getReadPointer(c), b.getReadPointer(c), bytes) != 0)
        {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("Monitor:0 自动化参数(123 参数面一个不动)", "[monitor][params]")
{
    ScvbMonitorAudioProcessor p;
    REQUIRE(p.getParameters().isEmpty());
    REQUIRE(p.getParameters().size() == 0);
    // 宿主看到的参数总数为 0;bypass 由 wrapper 提供,不占自动化位。
    REQUIRE(p.getBypassParameter() == nullptr);
    REQUIRE(p.getName() == "SCVB Monitor");
}

TEST_CASE("Monitor:音频直通逐样本按位相等", "[monitor][passthrough][nulltest]")
{
    ScvbMonitorAudioProcessor p;
    p.prepareToPlay(48000.0, 512);

    for (const int channels : {1, 2})
    {
        for (const int n : {1, 64, 512})
        {
            juce::AudioBuffer<float> buf(channels, n);
            fillTricky(buf, static_cast<unsigned>(channels * 1000 + n));
            juce::AudioBuffer<float> expected;
            expected.makeCopyOf(buf);

            juce::MidiBuffer midi;
            p.processBlock(buf, midi);
            INFO("channels=" << channels << " n=" << n);
            REQUIRE(bitwiseEqual(buf, expected)); // memcmp:不是「近似相等」,是按位相等

            p.processBlockBypassed(buf, midi);
            REQUIRE(bitwiseEqual(buf, expected)); // bypass 路径同款
        }
    }

    // 连续 200 块也不能有任何累积改变(平滑器/ramp 一旦被误接线就会在这里露馅)。
    juce::AudioBuffer<float> buf(2, 256);
    fillTricky(buf, 7);
    juce::AudioBuffer<float> expected;
    expected.makeCopyOf(buf);
    juce::MidiBuffer midi;
    for (int i = 0; i < 200; ++i)
    {
        p.processBlock(buf, midi);
    }
    REQUIRE(bitwiseEqual(buf, expected));

    p.releaseResources();
}

TEST_CASE("Monitor:总线布局只接受进出一致的 mono/stereo", "[monitor][buses]")
{
    ScvbMonitorAudioProcessor p;
    juce::AudioProcessor::BusesLayout l;

    l.inputBuses.add(juce::AudioChannelSet::stereo());
    l.outputBuses.add(juce::AudioChannelSet::stereo());
    REQUIRE(p.isBusesLayoutSupported(l));

    l.inputBuses.set(0, juce::AudioChannelSet::mono());
    l.outputBuses.set(0, juce::AudioChannelSet::mono());
    REQUIRE(p.isBusesLayoutSupported(l));

    // 进出不一致 → 拒绝(直通插件不做通道变换,否则「按位相等」无从谈起)。
    l.inputBuses.set(0, juce::AudioChannelSet::mono());
    l.outputBuses.set(0, juce::AudioChannelSet::stereo());
    REQUIRE_FALSE(p.isBusesLayoutSupported(l));

    l.inputBuses.set(0, juce::AudioChannelSet::create5point1());
    l.outputBuses.set(0, juce::AudioChannelSet::create5point1());
    REQUIRE_FALSE(p.isBusesLayoutSupported(l));
}

TEST_CASE("Monitor:对共享段零写入 + 只读读到 viz 数据", "[monitor][ipc][readonly]")
{
    constexpr scvb::u32 kGroup = 6; // 与其它用例错开,避免残留段互踩

    scvb::SegmentBackendWin32 backend;

    // 先确认:该组的 viz 段此刻不存在 —— 后面才能证明「Monitor 没有建段」。
    {
        scvb::VizPlane probe(backend, kGroup);
        REQUIRE(probe.attachReadOnly() == scvb::InitResult::kFailed);
    }

    // ---- ① Monitor 在段不存在时:空态,不崩、不建段 ----
    {
        const scvb::u32 registryBefore = registrySnapshot(backend);
        ScvbMonitorAudioProcessor p;
        REQUIRE(p.setObservedGroup(static_cast<int>(kGroup)));
        p.prepareToPlay(48000.0, 256);
        for (int i = 0; i < 8; ++i)
        {
            p.tickMessageThread(1000 + static_cast<std::uint64_t>(i) * 250);
        }
        REQUIRE(p.vizState() == ScvbMonitorAudioProcessor::VizState::kOffline);
        REQUIRE_FALSE(p.vizFresh());
        p.releaseResources();

        // Monitor 走了一圈之后,段仍然不存在 —— 只读方绝不创建段。
        scvb::VizPlane probe(backend, kGroup);
        REQUIRE(probe.attachReadOnly() == scvb::InitResult::kFailed);

        // registry 段同理:Monitor 的 1Hz 跨组探测只走 openExistingReadOnly,不 claim、不建段。
        // 断言的是**增量**而不是「一个都不存在」:命名段是机器全局的,同机跑着的另一个测试
        // 二进制、残留的 peer 进程、甚至真装了 SCVB 的 DAW,都可能正持着某组 registry ——
        // 拿「全不存在」当判据,测的就成了机器状态而不是 Monitor 的行为(第一版就是这么写的,
        // 一个上一轮残留的 scvb_ipc_peer 就把它打红了)。
        REQUIRE(registrySnapshot(backend) == registryBefore);
    }

    // ---- ② 写方建段 + 发布一帧 ----
    scvb::VizPlane writer(backend, kGroup);
    REQUIRE(writer.open() == scvb::InitResult::kOk);

    auto frame = std::make_unique<scvb::VizSnapshot>();
    frame->publishMs = 5000;
    frame->windowSpanSamples = 48000ull * 90;
    frame->playheadSamples = 24000;
    frame->sampleRate = 48000;
    frame->versionActive = 1;
    frame->onlineMask = 0x0007;
    frame->coveredMask = 0x0001;
    frame->laneRevision = 11;
    for (scvb::u32 t = 0; t < scvb::kMaxChannels; ++t)
    {
        frame->trackColor[t] = t + 1;
    }
    for (scvb::u32 i = 0; i < scvb::kVizColumns; ++i)
    {
        frame->pan[0][i] = scvb::vizPackPan(-33.5);
        frame->setCovered(0, i);
    }
    writer.publish(*frame, /*writeLanes=*/true);

    // 写方视角的基线快照(Monitor 跑完之后逐项比对)。
    auto before = std::make_unique<scvb::VizSnapshot>();
    scvb::VizPlane baseline(backend, kGroup);
    REQUIRE(baseline.attachReadOnly() == scvb::InitResult::kOk);
    REQUIRE(baseline.read(*before));

    // ---- ③ Monitor 只读 attach、读到数据、且一个字节都不写 ----
    {
        ScvbMonitorAudioProcessor p;
        REQUIRE(p.setObservedGroup(static_cast<int>(kGroup)));
        p.prepareToPlay(48000.0, 256);

        // 音频线程照跑 —— processBlock 期间对共享段零读零写。
        juce::AudioBuffer<float> buf(2, 256);
        fillTricky(buf, 42);
        juce::MidiBuffer midi;
        for (int i = 0; i < 50; ++i)
        {
            p.processBlock(buf, midi);
        }

        for (int i = 0; i < 12; ++i)
        {
            p.tickMessageThread(6000 + static_cast<std::uint64_t>(i) * 250);
        }

        REQUIRE(p.vizState() == ScvbMonitorAudioProcessor::VizState::kOnline);
        const auto& v = p.vizSnapshot();
        REQUIRE(v.windowSpanSamples == 48000ull * 90);
        REQUIRE(v.playheadSamples == 24000);
        REQUIRE(v.laneRevision == 11);
        REQUIRE(v.coveredMask == 0x0001);
        REQUIRE(v.trackColor[14] == 15);
        REQUIRE(v.pan[0][0] == scvb::vizPackPan(-33.5));
        REQUIRE(v.covered(0, 0));
        // 未覆盖轨保持哨兵(断线),Monitor 侧口径与发布侧一致。
        REQUIRE(v.pan[1][0] == scvb::kVizPanNone);
        REQUIRE_FALSE(v.covered(1, 0));

        p.releaseResources();
    }

    // 写方视角:段内容一字未变(Monitor 全程只读)。
    auto after = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(baseline.read(*after));
    REQUIRE(after->publishMs == before->publishMs);
    REQUIRE(after->laneRevision == before->laneRevision);
    REQUIRE(after->playheadSamples == before->playheadSamples);
    REQUIRE(after->windowSpanSamples == before->windowSpanSamples);
    REQUIRE(after->onlineMask == before->onlineMask);
    REQUIRE(after->coveredMask == before->coveredMask);
    REQUIRE(std::memcmp(after->pan.data(), before->pan.data(), sizeof(before->pan)) == 0);
    REQUIRE(std::memcmp(after->coverage.data(), before->coverage.data(), sizeof(before->coverage)) == 0);
    // 写方的 owner 线程护栏也没被触发过(Monitor 从没试图写)。
    REQUIRE(writer.foreignThreadWrites() == 0);
}

TEST_CASE("Monitor:组切换只读换段,不 claim、不残留上一组车道", "[monitor][ipc][group]")
{
    constexpr scvb::u32 kGroupA = 4;
    constexpr scvb::u32 kGroupB = 5;
    scvb::SegmentBackendWin32 backend;

    scvb::VizPlane writerA(backend, kGroupA);
    REQUIRE(writerA.open() == scvb::InitResult::kOk);
    auto f = std::make_unique<scvb::VizSnapshot>();
    f->publishMs = 100;
    f->laneRevision = 3;
    f->windowSpanSamples = 12345;
    f->coveredMask = 0x0001;
    for (scvb::u32 i = 0; i < scvb::kVizColumns; ++i)
    {
        f->pan[0][i] = scvb::vizPackPan(77.0);
        f->setCovered(0, i);
    }
    writerA.publish(*f, true);

    ScvbMonitorAudioProcessor p;
    REQUIRE(p.setObservedGroup(static_cast<int>(kGroupA)));
    p.prepareToPlay(48000.0, 256);
    for (int i = 0; i < 8; ++i)
    {
        p.tickMessageThread(200 + static_cast<std::uint64_t>(i) * 250);
    }
    REQUIRE(p.vizState() == ScvbMonitorAudioProcessor::VizState::kOnline);
    REQUIRE(p.vizSnapshot().pan[0][0] == scvb::vizPackPan(77.0));

    // 切到一个没有 Output 在线的组:立刻空态,且**不能**残留 A 组的车道。
    REQUIRE(p.setObservedGroup(static_cast<int>(kGroupB)));
    REQUIRE(p.vizSnapshot().pan[0][0] == scvb::kVizPanNone); // 换组即清,不画别人的曲线
    for (int i = 0; i < 8; ++i)
    {
        p.tickMessageThread(3000 + static_cast<std::uint64_t>(i) * 250);
    }
    REQUIRE(p.vizState() == ScvbMonitorAudioProcessor::VizState::kOffline);
    REQUIRE_FALSE(p.vizFresh());

    // 切回 A 组:重新 attach 成功,数据回来。
    REQUIRE(p.setObservedGroup(static_cast<int>(kGroupA)));
    for (int i = 0; i < 8; ++i)
    {
        p.tickMessageThread(6000 + static_cast<std::uint64_t>(i) * 250);
    }
    REQUIRE(p.vizState() == ScvbMonitorAudioProcessor::VizState::kOnline);
    REQUIRE(p.vizSnapshot().pan[0][0] == scvb::vizPackPan(77.0));

    // 组号越界一律夹取到 1..8(不越界即不换组)。
    REQUIRE(p.setObservedGroup(99));
    REQUIRE(p.groupId() == 8);
    REQUIRE(p.setObservedGroup(-1));
    REQUIRE(p.groupId() == 1);

    p.releaseResources();
}

TEST_CASE("Monitor:state 往返(组/缩放/语言;不可信字节拒载)", "[monitor][state]")
{
    ScvbMonitorAudioProcessor a;
    a.setObservedGroup(7);
    a.setUiScalePercent(150);
    a.setUiLanguage("fr");

    juce::MemoryBlock blob;
    a.getStateInformation(blob);
    REQUIRE(blob.getSize() > 0);

    ScvbMonitorAudioProcessor b;
    b.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    REQUIRE(b.groupId() == 7);
    REQUIRE(b.uiScalePercent() == 150);
    REQUIRE(b.uiLanguage() == "fr");

    // 垃圾字节:拒载 + 保持默认,不崩溃、不半填充。
    ScvbMonitorAudioProcessor c;
    const char junk[] = "not a scvb state chunk at all";
    c.setStateInformation(junk, static_cast<int>(sizeof(junk)));
    REQUIRE(c.groupId() == 1);
    c.setStateInformation(nullptr, 0);
    REQUIRE(c.groupId() == 1);
}

// ---------------------------------------------------------------------------
// 播放头:Monitor **自己的** AudioPlayHead → PlayheadShot → playheadSnapshot()。
//
// 这条链此前零覆盖。文件里原有的三处 playhead 断言全是 **viz 段里** 的
// `playheadSamples` / `playheadFlags` 字段(那是 Output 发布的,Monitor 只读),
// 与本链无关 —— 而轨迹图上那条竖线走的是 25Hz 的 `scvb.playhead`,数据源正是这里
// (见 web/monitor/app.js 的 playheadSeen 头注:Output 停摆时竖线照常走)。
// 也就是说:哪怕 viz 段一切正常,这条链断了竖线照样不出,而且没有任何测试会红。
// ---------------------------------------------------------------------------
namespace
{
// 假宿主播放头。JUCE 的 PositionInfo 全是 Optional,DAW 之间给不给差别很大 ——
// 故意做成「哪些字段有值」可配置,好把「没给」与「给了 0」分开测。
class FakePlayHead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override
    {
        if (!hasPosition)
            return juce::nullopt;
        return info;
    }

    bool hasPosition = true;
    PositionInfo info;
};

// 灌一块音频把 processBlock 跑一遍(publishPlayhead 挂在那条路上)。
void pumpOneBlock(ScvbMonitorAudioProcessor& p, int samples = 256)
{
    juce::AudioBuffer<float> buf(2, samples);
    buf.clear();
    juce::MidiBuffer midi;
    p.processBlock(buf, midi);
}
} // namespace

TEST_CASE("Monitor:自己的 AudioPlayHead 经 processBlock 落进 playheadSnapshot", "[monitor][playhead]")
{
    ScvbMonitorAudioProcessor p;
    p.prepareToPlay(48000.0, 256);

    SECTION("宿主不给播放头 ⇒ timeSamples 保持 -1(调用方据此过滤,不是当成 0 秒)")
    {
        p.setPlayHead(nullptr);
        pumpOneBlock(p);

        const auto pod = p.playheadSnapshot();
        REQUIRE(pod.timeSamples == -1);
        REQUIRE(pod.flags == 0u);
        REQUIRE(pod.sampleRate == 48000.0); // 采样率仍来自 prepareToPlay,与播放头无关
    }

    SECTION("宿主给了播放头但 getPosition() 无值 ⇒ 同样是 -1")
    {
        FakePlayHead ph;
        ph.hasPosition = false;
        p.setPlayHead(&ph);
        pumpOneBlock(p);

        REQUIRE(p.playheadSnapshot().timeSamples == -1);
        p.setPlayHead(nullptr); // 别让处理器持有出栈的假对象
    }

    SECTION("播放中 + 循环 + 有速度 ⇒ 四个字段与四个 flag 位逐个到位")
    {
        FakePlayHead ph;
        ph.info.setTimeInSamples(123456);
        ph.info.setIsPlaying(true);
        ph.info.setIsLooping(true);
        ph.info.setBpm(128.5);
        ph.info.setLoopPoints(juce::AudioPlayHead::LoopPoints{4.0, 20.0});
        p.setPlayHead(&ph);
        pumpOneBlock(p);

        const auto pod = p.playheadSnapshot();
        REQUIRE(pod.timeSamples == 123456);
        REQUIRE(pod.bpm == 128.5);
        REQUIRE(pod.loopStartPpq == 4.0);
        REQUIRE(pod.loopEndPpq == 20.0);
        REQUIRE((pod.flags & scvb::engine::kPlayheadIsPlaying) != 0u);
        REQUIRE((pod.flags & scvb::engine::kPlayheadIsLooping) != 0u);
        REQUIRE((pod.flags & scvb::engine::kPlayheadTempoValid) != 0u);
        REQUIRE((pod.flags & scvb::engine::kPlayheadCycleValid) != 0u);
        p.setPlayHead(nullptr);
    }

    SECTION("停止 + 无速度 + 无循环点 ⇒ 位置照收,三个 flag 位都不置")
    {
        FakePlayHead ph;
        ph.info.setTimeInSamples(9600);
        ph.info.setIsPlaying(false);
        p.setPlayHead(&ph);
        pumpOneBlock(p);

        const auto pod = p.playheadSnapshot();
        REQUIRE(pod.timeSamples == 9600); // 停着也要给位置,否则竖线会跳回原点
        REQUIRE((pod.flags & scvb::engine::kPlayheadIsPlaying) == 0u);
        REQUIRE((pod.flags & scvb::engine::kPlayheadTempoValid) == 0u);
        REQUIRE((pod.flags & scvb::engine::kPlayheadCycleValid) == 0u);
        REQUIRE(pod.bpm == 0.0); // 宿主没给就别编一个出来
        p.setPlayHead(nullptr);
    }

    SECTION("每块都重发:位置前进后快照跟着走(竖线是靠这个动的)")
    {
        FakePlayHead ph;
        ph.info.setIsPlaying(true);
        p.setPlayHead(&ph);

        for (const juce::int64 at : {0LL, 256LL, 512LL, 768LL})
        {
            ph.info.setTimeInSamples(at);
            pumpOneBlock(p);
            REQUIRE(p.playheadSnapshot().timeSamples == at);
        }
        p.setPlayHead(nullptr);
    }
}
