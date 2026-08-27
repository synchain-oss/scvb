// SPDX-License-Identifier: GPL-3.0-or-later
// test_viz_publish_cost —— viz 发布链路的**单帧耗时实测**(SL-192,4Hz → 30Hz 升频的证据面)。
//
// 为什么要有这份文件:把发布频率提到 30Hz 是**冻结契约变更**,而「消息线程扛不扛得住」这件事
// 不能靠推断("64KB 的段 × 30Hz ≈ 2MB/s,应该没事")。段是 64KB 不等于每帧写 64KB ——
// `VizPlane::publish()` 稳态下只写 15 个帧头标量 + 15×3 个每轨当前值(约 60 次 relaxed store),
// 整块 64KB(车道 + 位图 + 轨色 + 轨名)**只在 `writeLanes` 那一帧**写。两者差三个数量级,
// 拿哪一个去算预算,结论完全不同。所以这里逐条真测,数据进变更文档。
//
// **本文件全部用例都带 `[.]` 前缀标签 = Catch2 的「隐藏用例」**:`ctest` 默认不跑它们
// (门禁里不该有一条按墙钟时间判红的断言 —— 那是最典型的 CI 抖动源)。手工复现:
//
//     scvb_tests.exe "[vizcost]"
//
// 它只打印测量值、不对耗时下断言;唯一的断言是「测出来的东西确实跑了」(publishCount 等),
// 免得哪天代码被优化掉之后这份数据变成对空气的测量。
//
// 发布频率本身的断言(单位时间帧数 ≥28)在 `test_viz_plane.cpp` 的节拍用例里,那条不看墙钟,
// 只按注入的逻辑时刻数发布次数,故可以进门禁。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ipc/SegmentBackendInProcess.h"
#include "ipc/VizPlane.h"
#include "output/VizPublisher.h"

namespace
{
constexpr double kSr = 48000.0;
constexpr int kWarmup = 200;
constexpr int kIters = 2000;

scvb::state::Segment makeSeg(double t0Sec, double t1Sec, float pan)
{
    scvb::state::Segment s;
    s.t0 = static_cast<std::int64_t>(t0Sec * kSr);
    s.t1 = static_cast<std::int64_t>(t1Sec * kSr);
    s.pan = pan;
    s.volDb = -6.0f;
    return s;
}

void buildCurve(scvb::CurveEvaluator& ev, const std::vector<scvb::state::Segment>& segs)
{
    std::vector<scvb::CurveSegment> cs;
    cs.reserve(segs.size());
    for (const auto& s : segs)
    {
        scvb::CurveSegment c;
        c.startSec = static_cast<double>(s.t0) / kSr;
        c.endSec = static_cast<double>(s.t1) / kSr;
        c.pan = s.pan;
        c.volDb = s.volDb;
        cs.push_back(c);
    }
    ev.build(cs, scvb::TransitionConfig{});
}

// 满配:15 轨 × 每轨 40 段(约 5 分钟时间线),= 真实工程的量级上限。
struct World
{
    std::unique_ptr<scvb::state::CrvsData> crvs = std::make_unique<scvb::state::CrvsData>();
    std::vector<scvb::CurveEvaluator> curves{scvb::state::kNumTracks};

    World()
    {
        for (std::size_t t = 0; t < scvb::state::kNumTracks; ++t)
        {
            auto& segs = crvs->versions[0].tracks[t].segments;
            double at = 1.0 + static_cast<double>(t) * 0.31;
            for (int k = 0; k < 40; ++k)
            {
                const double len = 3.2 + static_cast<double>((t + k) % 5);
                segs.push_back(makeSeg(at, at + len, static_cast<float>(((k * 17 + t * 7) % 201) - 100)));
                at += len + 0.9;
            }
            buildCurve(curves[t], segs);
        }
    }

    scvb::output::VizPublishInput input() const
    {
        scvb::output::VizPublishInput in;
        in.crvs = crvs.get();
        for (std::size_t t = 0; t < scvb::state::kNumTracks; ++t)
        {
            in.curves[t] = &curves[t];
            in.widthPct[t] = 100.0f;
            in.label[t] = "Lead Vocal Double " + std::to_string(t + 1);
        }
        in.versionActive = 1;
        in.sampleRate = kSr;
        in.crvsRevision = 1;
        in.enabledMask = 0x7FFFu;
        in.stereoMask = 0x0101u;
        in.playhead.sampleRate = kSr;
        in.playhead.timeSamples = static_cast<std::int64_t>(30.0 * kSr);
        return in;
    }
};

struct Stats
{
    std::size_t n = 0;
    double meanUs = 0.0;
    double p50Us = 0.0;
    double p99Us = 0.0;
    double maxUs = 0.0;
};

Stats summarize(std::vector<double>& us)
{
    Stats s;
    if (us.empty())
    {
        return s;
    }
    s.n = us.size();
    double sum = 0.0;
    for (const double v : us)
    {
        sum += v;
    }
    s.meanUs = sum / static_cast<double>(us.size());
    std::sort(us.begin(), us.end());
    s.p50Us = us[us.size() / 2];
    s.p99Us = us[static_cast<std::size_t>(static_cast<double>(us.size()) * 0.99)];
    s.maxUs = us.back();
    return s;
}

// `hzLabel` 只是给读数配一句「按这个频率算占一颗核多少」的换算,不参与测量。
void report(const char* what, Stats s, double hz, const char* hzLabel)
{
    std::printf(
        "  [vizcost] %-34s n=%4zu  p50 %8.3f us  mean %8.3f  p99 %8.3f  max %8.3f  | @%s => %.4f%% of one core\n", what,
        s.n, s.p50Us, s.meanUs, s.p99Us, s.maxUs, hzLabel, s.p50Us * hz / 10000.0);
}

template<typename F>
Stats measure(int iters, F&& body)
{
    std::vector<double> us;
    us.reserve(static_cast<std::size_t>(iters));
    for (int i = 0; i < iters; ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        body(i);
        const auto t1 = std::chrono::steady_clock::now();
        us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    return summarize(us);
}
} // namespace

TEST_CASE("[vizcost] 稳态单帧发布耗时(帧头 + 每轨当前值,不重算车道)", "[.][vizcost]")
{
    World w;
    scvb::SegmentBackendInProcess backend;
    scvb::output::VizPublisher pub(backend, 3);
    REQUIRE(pub.open() == scvb::InitResult::kOk);
    auto in = w.input();

    // 预热:首帧必重算车道,窗口跨度也在这里定下来。
    scvb::u64 now = 0;
    for (int i = 0; i < kWarmup; ++i)
    {
        now += scvb::output::VizPublisher::kPublishIntervalMs;
        pub.tick(now, in);
    }
    const auto lanesAfterWarmup = pub.laneRebuildCount();

    // **必须把车道重算的那些帧剔出去**:逻辑时钟每次推进一个发布周期,2000 次就是几百秒,
    // kLaneRefreshMaxMs(30s)兜底会在窗口内触发十几次。混进来测的就不是稳态路径了 ——
    // 第一版就是这么错的:mean 被那十几帧拉到 5.5us,而 p50 只有 1.2us,差四倍。
    std::vector<double> steadyUs;
    steadyUs.reserve(static_cast<std::size_t>(kIters));
    scvb::u64 rebuilds = 0;
    for (int i = 0; i < kIters; ++i)
    {
        now += scvb::output::VizPublisher::kPublishIntervalMs;
        const auto lanesBefore = pub.laneRebuildCount();
        const auto t0 = std::chrono::steady_clock::now();
        pub.tick(now, in);
        const auto t1 = std::chrono::steady_clock::now();
        if (pub.laneRebuildCount() != lanesBefore)
        {
            ++rebuilds; // 这一帧走的是重算路径,归下一个用例统计
            continue;
        }
        steadyUs.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    auto s = summarize(steadyUs);

    REQUIRE(pub.publishCount() >= static_cast<scvb::u64>(kIters));
    REQUIRE(steadyUs.size() > static_cast<std::size_t>(kIters) / 2); // 绝大多数确实是稳态帧
    report("steady tick() 15 tracks", s, 30.0, "30Hz");
    std::printf("  [vizcost]   (excluded %llu lane-rebuild frames; warmup rebuilds=%llu)\n",
                static_cast<unsigned long long>(rebuilds), static_cast<unsigned long long>(lanesAfterWarmup));
}

TEST_CASE("[vizcost] 车道重算帧的耗时(15x1024 次曲线求值 + 64KB 写)", "[.][vizcost]")
{
    World w;
    scvb::SegmentBackendInProcess backend;
    scvb::output::VizPublisher pub(backend, 4);
    REQUIRE(pub.open() == scvb::InitResult::kOk);
    auto in = w.input();

    scvb::u64 now = 0;
    pub.tick(now, in); // 首帧
    const auto before = pub.laneRebuildCount();

    // 每帧都把 crvsRevision 推进一格 ⇒ 每帧都重算车道(最坏路径)。
    auto s = measure(300, [&](int i) {
        now += scvb::output::VizPublisher::kPublishIntervalMs;
        in.crvsRevision = static_cast<scvb::u32>(2 + i);
        pub.tick(now, in);
    });
    REQUIRE(pub.laneRebuildCount() >= before + 300);
    report("lane-rebuild tick() 15 tracks", s, 1.0 / 30.0, "1/30s fallback");
}

TEST_CASE("[vizcost] 读方整帧读耗时(VizPlane::read,Monitor [M] 每拍一次)", "[.][vizcost]")
{
    World w;
    scvb::SegmentBackendInProcess backend;
    scvb::output::VizPublisher pub(backend, 5);
    REQUIRE(pub.open() == scvb::InitResult::kOk);
    scvb::VizPlane reader(backend, 5);
    REQUIRE(reader.attachReadOnly() == scvb::InitResult::kOk);

    auto in = w.input();
    scvb::u64 now = 0;
    pub.tick(now, in);

    auto out = std::make_unique<scvb::VizSnapshot>();
    int ok = 0;
    auto s = measure(kIters, [&](int) {
        if (reader.read(*out))
        {
            ++ok;
        }
    });
    REQUIRE(ok == kIters);
    report("VizPlane::read() full snapshot", s, 60.0, "60Hz");
}

TEST_CASE("[vizcost] 调用方的输入采集(15 个轨名 toStdString + FNV)", "[.][vizcost]")
{
    // `OutputProcessor::publishVizFrame` 在闸门之后做的事:15 次 std::string 赋值(堆分配)
    // + 逐字节 FNV-1a。升频之后这段从 4Hz 变成 30Hz,必须一并计入预算。
    std::vector<std::string> src;
    for (int i = 0; i < 15; ++i)
    {
        src.push_back("Lead Vocal Double " + std::to_string(i + 1));
    }
    scvb::output::VizPublishInput in;
    std::uint64_t sink = 0;

    auto s = measure(kIters, [&](int) {
        std::uint64_t meta = 1469598103934665603ull ^ 1ull;
        for (std::size_t ch = 0; ch < 15; ++ch)
        {
            in.label[ch] = src[ch];
            for (const char byte : in.label[ch])
            {
                meta = (meta ^ static_cast<unsigned char>(byte)) * 1099511628211ull;
            }
            meta = (meta ^ 0u) * 1099511628211ull;
        }
        sink += meta;
    });
    REQUIRE(sink != 0);
    report("caller input gather (15 labels)", s, 30.0, "30Hz");
}
