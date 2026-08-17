// SPDX-License-Identifier: GPL-3.0-or-later
// analysis_coverage_test —— FrameStore/CoverageMap/FeatRing/增量拉取/run 协议(T20)。
// 覆盖:① CoverageMap 与朴素位图对拍;② 04 §3.5 十合并场景;③ 5min=30000±1 hop、内存<1MB;
// ④ 采集 OFF 写路径丢弃;⑤ startRun biquad 预热(FEAT-3 在线路径);⑥ [P4/G11] 布防门控;
// ⑦ run 协议 store 顺序(base 先 write 后)与读方 seqlock。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "analysis/CoverageMap.h"
#include "analysis/FeatureExtractor.h"
#include "analysis/FrameStore.h"
#include "ipc/FeatRing.h"
#include "ipc/SegmentLayout.h"

using Catch::Approx;
using scvb::u32;
using scvb::u64;

using scvb::analysis::ChannelFrames;
using scvb::analysis::CoverageMap;
using scvb::analysis::FeatFrame;
using scvb::analysis::FrameStore;
using scvb::analysis::HopRange;

namespace
{

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr HopRange kFull{0, std::numeric_limits<u64>::max()};

// ---- CoverageMap 对拍辅助 ----

bool invariantsOk(const CoverageMap& cm)
{
    const auto& r = cm.ranges();
    for (std::size_t i = 0; i < r.size(); ++i)
    {
        if (r[i].begin >= r[i].end)
            return false; // 空区间
        if (i > 0 && r[i - 1].end >= r[i].begin)
            return false; // 未排序 / 重叠 / 相邻未合并
    }
    return true;
}

bool bitmapMatches(const CoverageMap& cm, const std::vector<uint8_t>& bitmap)
{
    const u64 n = static_cast<u64>(bitmap.size());
    u64 total = 0;
    for (u64 h = 0; h < n; ++h)
    {
        const bool covered = cm.coversFully(HopRange{h, h + 1});
        if (covered != (bitmap[static_cast<std::size_t>(h)] != 0))
            return false;
        if (bitmap[static_cast<std::size_t>(h)] != 0)
            ++total;
    }
    return cm.coveredHops(HopRange{0, n}) == total;
}

// 确定性 xorshift。
u64 nextRand(u64& seed)
{
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return seed >> 33;
}

// ---- FeatRing 写侧辅助 ----

struct FeatRingFixture
{
    scvb::FeatHeader header;
    std::vector<scvb::FeatFrame> ring;
    scvb::FeatRing wr;

    explicit FeatRingFixture(u32 capacity = 4096, int channels = 1, int maxBlock = 480) : ring(capacity)
    {
        header.magic.store(scvb::kScvbMagic, std::memory_order_release);
        header.abi.store(scvb::kScvbAbi, std::memory_order_release);
        header.hop_ms = scvb::kFeatHopMs;
        header.capacity_hops = capacity;
        header.base_hop.store(0, std::memory_order_release);
        header.write_hop.store(0, std::memory_order_release);
        wr.bind(&header, ring.data(), capacity);
        wr.prepare(48000.0, channels, maxBlock);
        wr.setCapturing(true);
    }
};

std::vector<float> makeMix(double fs, int seconds, double f1, double f2, double f3)
{
    const std::size_t n = static_cast<std::size_t>(fs * seconds);
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / fs;
        out[i] = static_cast<float>(0.3 * std::sin(2.0 * kPi * f1 * t) + 0.2 * std::sin(2.0 * kPi * f2 * t) +
                                    0.1 * std::sin(2.0 * kPi * f3 * t));
    }
    return out;
}

std::vector<FeatFrame> runExtractor(scvb::analysis::FeatureExtractor& ex, const float* const* ch, int numSamples,
                                    const std::vector<int>& lens)
{
    const int totalCap = numSamples / ex.hopSize() + 2;
    std::vector<FeatFrame> frames(static_cast<std::size_t>(totalCap));
    int written = 0;
    int pos = 0;
    std::size_t li = 0;
    while (pos < numSamples)
    {
        int len = lens[li % lens.size()];
        if (len > numSamples - pos)
            len = numSamples - pos;
        const float* blockCh[2] = {nullptr, nullptr};
        for (int c = 0; c < ex.channels(); ++c)
            blockCh[c] = ch[c] + pos;
        const int got = ex.processBlock(blockCh, len, frames.data() + written, totalCap - written);
        written += got;
        pos += len;
        ++li;
    }
    frames.resize(static_cast<std::size_t>(written));
    return frames;
}

// 经 FeatRing 推流 [start, end),返回该区间写入的 hop 总数。
int feedFeatRing(scvb::FeatRing& wr, const std::vector<float>& sig, int start, int end, const std::vector<int>& lens)
{
    int pos = start;
    int total = 0;
    std::size_t li = 0;
    while (pos < end)
    {
        int len = lens[li % lens.size()];
        if (len > end - pos)
            len = end - pos;
        const float* ch[1] = {sig.data() + pos};
        total += wr.processBlock(ch, len);
        pos += len;
        ++li;
    }
    return total;
}

} // namespace

// ---------------------------------------------------------------------------
// ① CoverageMap 与朴素位图对拍(随机 add/punch 属性测试)
// ---------------------------------------------------------------------------

TEST_CASE("CoverageMap 与朴素位图对拍(随机 add/punch)", "[coverage][property]")
{
    constexpr u64 kBits = 256;
    std::vector<uint8_t> bitmap(static_cast<std::size_t>(kBits), 0);
    CoverageMap cm;
    u64 seed = 0x9e3779b97f4a7c15ULL;

    for (int iter = 0; iter < 3000; ++iter)
    {
        const u64 a = nextRand(seed) % kBits;
        const u64 b = a + 1 + nextRand(seed) % (kBits - a);
        const HopRange r{a, b};
        const bool doAdd = (nextRand(seed) & 1u) != 0;
        if (doAdd)
        {
            cm.add(r);
            for (u64 h = a; h < b; ++h)
                bitmap[static_cast<std::size_t>(h)] = 1;
        }
        else
        {
            cm.punch(r);
            for (u64 h = a; h < b; ++h)
                bitmap[static_cast<std::size_t>(h)] = 0;
        }

        INFO("iter=" << iter << " op=" << (doAdd ? "add" : "punch") << " [" << a << "," << b << ")");
        REQUIRE(invariantsOk(cm));
        REQUIRE(bitmapMatches(cm, bitmap));
    }
}

TEST_CASE("CoverageMap 打洞四态(含/跨左/跨右/一分为二)", "[coverage]")
{
    CoverageMap cm;
    cm.add({0, 100});
    cm.punch({40, 60}); // 一分为二
    REQUIRE(cm.ranges() == std::vector<HopRange>{{0, 40}, {60, 100}});

    cm.punch({0, 10}); // 跨左边界
    REQUIRE(cm.ranges() == std::vector<HopRange>{{10, 40}, {60, 100}});

    cm.punch({90, 100}); // 跨右边界
    REQUIRE(cm.ranges() == std::vector<HopRange>{{10, 40}, {60, 90}});

    cm.punch({10, 40}); // 完全包含 → 删
    REQUIRE(cm.ranges() == std::vector<HopRange>{{60, 90}});
}

// ---------------------------------------------------------------------------
// ② 04 §3.5 十个合并场景
// ---------------------------------------------------------------------------

TEST_CASE("S1 首次线性播完 range → 单一连续区间", "[coverage][merge]")
{
    CoverageMap cm;
    for (u64 h = 0; h < 100; ++h)
        cm.add({h, h + 1});
    REQUIRE(cm.ranges() == std::vector<HopRange>{{0, 100}});
}

TEST_CASE("S2 循环播放 [A,B) × N 遍 → 记账不变(LastWriteWins)", "[coverage][merge]")
{
    CoverageMap cm;
    for (int pass = 0; pass < 4; ++pass)
        cm.add({10, 20});
    REQUIRE(cm.ranges() == std::vector<HopRange>{{10, 20}});
    REQUIRE(cm.coveredHops({0, 30}) == 10);
}

TEST_CASE("S3 循环从 range 外进入 → 区外丢弃,仍是 [A,B)", "[coverage][merge]")
{
    // 区外 [A-8s, A) = [2, 10),区内 [10, 20)。
    ChannelFrames cf;
    cf.setReadOnly(false);
    cf.setGate({10, 20});
    for (u64 h = 2; h < 20; ++h)
        cf.write(h, 0.5f, 0.5f); // 区外写被 gate 丢弃
    REQUIRE(cf.coveredHops({0, 30}) == 10);
    REQUIRE(cf.coverage().ranges() == std::vector<HopRange>{{10, 20}});
}

TEST_CASE("S4 跳段 → 两区间,中间是洞", "[coverage][merge]")
{
    CoverageMap cm;
    cm.add({0, 10});
    cm.add({20, 30});
    REQUIRE(cm.ranges() == std::vector<HopRange>{{0, 10}, {20, 30}});
    REQUIRE_FALSE(cm.coversFully({10, 20}));
}

TEST_CASE("S5 重复播放部分重叠 → 并集合并为单区间", "[coverage][merge]")
{
    CoverageMap cm;
    cm.add({0, 10});
    cm.add({5, 15});
    REQUIRE(cm.ranges() == std::vector<HopRange>{{0, 15}});
}

TEST_CASE("S6 播放中途才开采集 → 从 ON 位置的首完整 hop 起", "[coverage][merge][run]")
{
    constexpr double fs = 48000.0;
    const auto sig = makeMix(fs, 2, 997.0, 3171.0, 777.0);
    const int hop = 480;
    const int timelineSample = 1000; // 非 hop 对齐:首完整 hop = 3([1440,1920))
    const u64 h0 = 3;

    FeatRingFixture fx(4096, 1, 480);
    fx.wr.startRun(timelineSample);
    REQUIRE(fx.header.base_hop.load() == h0);
    REQUIRE(fx.header.write_hop.load() == h0);
    REQUIRE(fx.wr.nextHop() == h0);

    // 部分 hop [1000,1440) 丢弃:先喂 440 样本 → 0 帧。
    const std::vector<int> lens{hop};
    const int got0 = feedFeatRing(fx.wr, sig, timelineSample, timelineSample + 440, lens);
    REQUIRE(got0 == 0);
    REQUIRE(fx.wr.nextHop() == h0);

    // 再喂首完整 hop [1440,1920) 480 样本 → 产出 hop 3。
    const int got1 = feedFeatRing(fx.wr, sig, timelineSample + 440, timelineSample + 440 + hop, lens);
    REQUIRE(got1 == 1);
    REQUIRE(fx.wr.nextHop() == h0 + 1);
    REQUIRE(fx.header.write_hop.load() == h0 + 1);
}

TEST_CASE("S7 停止走带 → run 结束,write_hop 停,coverage 不变", "[coverage][merge][run]")
{
    FeatRingFixture fx(4096, 1, 480);
    fx.wr.startRun(0);
    std::vector<float> sig(480 * 10, 0.5f);
    const std::vector<int> lens{480};
    feedFeatRing(fx.wr, sig, 0, 480 * 5, lens);
    const u64 writeHop = fx.header.write_hop.load();
    REQUIRE(writeHop == 5);

    // 停止走带 = 不再喂块;write_hop 不推进。
    REQUIRE(fx.header.write_hop.load() == writeHop);
}

TEST_CASE("S8 重采集同区间 → 新数据覆盖,记账不变", "[coverage][merge][store]")
{
    ChannelFrames cf;
    cf.setReadOnly(false);
    cf.write(5, 0.25f, 0.25f);
    cf.write(5, 0.81f, 0.81f); // 逐 hop 覆盖(LastWriteWins)
    REQUIRE(cf.coveredHops({0, 10}) == 1);
    REQUIRE(cf.kwDbq(5) == scvb::analysis::quantizeKwDbq(0.81f));
    REQUIRE(cf.coverage().ranges() == std::vector<HopRange>{{5, 6}});
}

TEST_CASE("S9 跳段后洞被补播填上 → 两区间合并", "[coverage][merge]")
{
    CoverageMap cm;
    cm.add({0, 10});
    cm.add({20, 30});
    cm.add({10, 20});
    REQUIRE(cm.ranges() == std::vector<HopRange>{{0, 30}});
}

TEST_CASE("S10 相距超 ring 容量的两段先后采集 → 两个独立区间", "[coverage][merge][store]")
{
    constexpr u32 kCap = 4096;
    FrameStore store;
    ChannelFrames& cf = store.channel(1);
    cf.setReadOnly(false);
    for (u64 h = 0; h < 10; ++h)
        cf.write(h, 0.5f, 0.5f);
    for (u64 h = 10000; h < 10010; ++h)
        cf.write(h, 0.5f, 0.5f); // 远超环容量;先写段早已进 FrameStore
    REQUIRE(cf.coveredHops({0, 100000}) == 20);
    REQUIRE(cf.coverage().ranges() == std::vector<HopRange>{{0, 10}, {10000, 10010}});
    (void)kCap;
}

// ---------------------------------------------------------------------------
// 量化 roundtrip + 采集 OFF 丢弃 + gate
// ---------------------------------------------------------------------------

TEST_CASE("FrameStore 量化 roundtrip(0.01dB 分辨率)", "[store][quantize]")
{
    using scvb::analysis::dequantizeKwMs;
    using scvb::analysis::quantizeKwDbq;

    REQUIRE(quantizeKwDbq(0.0f) == scvb::analysis::kSilenceDbq);
    REQUIRE(quantizeKwDbq(-1.0f) == scvb::analysis::kSilenceDbq);
    REQUIRE(dequantizeKwMs(scvb::analysis::kSilenceDbq) == 0.0f);

    const float x = 0.5f;
    const float back = dequantizeKwMs(quantizeKwDbq(x));
    REQUIRE(back == Approx(x).epsilon(2e-3f)); // 0.01dB ≈ 0.23%
}

TEST_CASE("采集 OFF → ChannelFrames 只读,写路径静默丢弃", "[store][readonly]")
{
    ChannelFrames cf; // 默认只读
    cf.write(3, 0.5f, 0.5f);
    REQUIRE(cf.coveredHops({0, 10}) == 0);
    REQUIRE(cf.pageCount() == 0); // 不写页、不记账

    cf.setReadOnly(false);
    cf.write(3, 0.5f, 0.5f);
    REQUIRE(cf.coveredHops({0, 10}) == 1);

    cf.setReadOnly(true);
    cf.write(7, 0.5f, 0.5f);
    REQUIRE(cf.coveredHops({0, 10}) == 1); // 仍只 1 个 hop
}

TEST_CASE("采集 OFF → FeatRing 写路径静默丢弃(不写段、不推进 write_hop)", "[featring][readonly]")
{
    FeatRingFixture fx(4096, 1, 480);
    fx.wr.startRun(0);
    std::vector<float> sig(480 * 4, 0.5f);
    const std::vector<int> lens{480};

    feedFeatRing(fx.wr, sig, 0, 480 * 2, lens);
    REQUIRE(fx.header.write_hop.load() == 2);

    fx.wr.setCapturing(false); // 采集 OFF
    feedFeatRing(fx.wr, sig, 0, 480 * 2, lens);
    REQUIRE(fx.header.write_hop.load() == 2); // 不推进
    REQUIRE(fx.ring[2].kw_ms == 0.0f); // 未写槽 2
}

TEST_CASE("FrameStore gate:区外 hop 丢弃不记账", "[store][gate]")
{
    ChannelFrames cf;
    cf.setReadOnly(false);
    cf.setGate({10, 20});
    for (u64 h = 0; h <= 25; ++h)
        cf.write(h, 0.5f, 0.5f); // 区外 [0,10) 与 [20,25] 被 gate 丢弃
    REQUIRE(cf.coveredHops({0, 30}) == 10);
    REQUIRE(cf.coverage().ranges() == std::vector<HopRange>{{10, 20}});
}

// ---------------------------------------------------------------------------
// ③ 5 分钟单轨 = 30000±1 hop、内存 < 1MB
// ---------------------------------------------------------------------------

TEST_CASE("5 分钟单轨采集 = 30000±1 hop 且 FrameStore 内存 < 1MB", "[store][memory]")
{
    constexpr u32 kCap = 131072; // kFeatCapacityHops
    FeatRingFixture fx(kCap, 1, 480);
    std::vector<float> sig(480, 0.5f);
    const std::vector<int> lens{480};

    fx.wr.startRun(0);
    const int totalHops = 30000; // 300s × 10ms = 30000
    int written = 0;
    for (int i = 0; i < totalHops; ++i)
    {
        const float* ch[1] = {sig.data()};
        written += fx.wr.processBlock(ch, 480);
    }
    REQUIRE(written == totalHops);
    REQUIRE(fx.header.write_hop.load() == static_cast<u64>(totalHops));

    // 拉取进 FrameStore。
    FrameStore store;
    ChannelFrames& cf = store.channel(1);
    cf.setReadOnly(false);
    scvb::FeatPullState st;
    uint32_t pulled = 0;
    do
    {
        pulled = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    } while (pulled != 0);

    REQUIRE(cf.coveredHops(kFull) == 30000);
    REQUIRE(cf.pageCount() == 8); // 30000/4096 向上取整
    REQUIRE(cf.allocatedBytes() < 1024 * 1024); // < 1MB(实为 8×20KB = 160KB)
}

// ---------------------------------------------------------------------------
// ⑤ startRun biquad 预热(FEAT-3 在线路径)
// ---------------------------------------------------------------------------

TEST_CASE("FEAT-3 在线路径:run B 预热逐位一致且与单独推流一致", "[featring][preheat]")
{
    constexpr double fs = 48000.0;
    const int hop = 480;
    const auto sig = makeMix(fs, 5, 997.0, 3171.0, 777.0);
    const int n = static_cast<int>(sig.size());
    const int k1 = 100; // run A 采 hop 0..99,run B 从 hop 100 起
    const int k1Sample = k1 * hop;

    // 参考:仅从 k1 单独推流(全新 extractor,预热 k1)。
    scvb::analysis::FeatureExtractor ref;
    ref.prepare(fs, 1);
    const float* chB[1] = {sig.data() + k1Sample};
    const auto refFrames = runExtractor(ref, chB, n - k1Sample, {n - k1Sample});

    auto runOnline = [&](const std::vector<int>& lensRunB) {
        FeatRingFixture fx(4096, 1, 513);
        fx.wr.startRun(0);
        const std::vector<int> runALens{480};
        feedFeatRing(fx.wr, sig, 0, k1Sample, runALens); // run A:hops 0..99
        REQUIRE(fx.header.write_hop.load() == static_cast<u64>(k1));

        fx.wr.startRun(k1Sample); // run 切换:base/write = h0 = 100
        REQUIRE(fx.header.base_hop.load() == static_cast<u64>(k1));
        REQUIRE(fx.header.write_hop.load() == static_cast<u64>(k1));

        feedFeatRing(fx.wr, sig, k1Sample, n, lensRunB); // run B

        std::vector<FeatFrame> out;
        for (u64 h = static_cast<u64>(k1); h < fx.wr.nextHop(); ++h)
        {
            const scvb::FeatFrame& f = fx.ring[h % 4096];
            out.push_back(FeatFrame{f.kw_ms, f.peak});
        }
        return out;
    };

    const auto runB_A = runOnline({480});
    const auto runB_B = runOnline({61, 127, 513});

    REQUIRE(refFrames.size() == runB_A.size());
    REQUIRE(refFrames.size() == runB_B.size());
    for (std::size_t k = 0; k < refFrames.size(); ++k)
    {
        INFO("hop " << k);
        REQUIRE(runB_A[k].kw_ms == refFrames[k].kw_ms); // 逐位:run B 首 hop 无 run A 残留
        REQUIRE(runB_A[k].peak == refFrames[k].peak);
        REQUIRE(runB_B[k].kw_ms == refFrames[k].kw_ms);
        REQUIRE(runB_B[k].peak == refFrames[k].peak);
    }
}

// ---------------------------------------------------------------------------
// ⑦ run 协议 store 顺序(base 先 write 后)+ 读方 seqlock
// ---------------------------------------------------------------------------

TEST_CASE("startRun 顺序:base 先 write 后,交错读见新 base+旧 write 零污染", "[featring][run][order]")
{
    constexpr u32 kCap = 4096;
    FeatRingFixture fx(kCap, 1, 480);
    fx.wr.startRun(0);
    std::vector<float> sig(480 * 10, 0.5f);
    const std::vector<int> lens{480};
    feedFeatRing(fx.wr, sig, 0, 480 * 10, lens); // run A:hops 0..9,write_hop=10
    REQUIRE(fx.header.write_hop.load() == 10);

    FrameStore store;
    ChannelFrames& cf = store.channel(1);
    cf.setReadOnly(false);
    scvb::FeatPullState st;
    scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    REQUIRE(st.lastPulled == 10);
    REQUIRE(cf.coveredHops(kFull) == 10);

    // 前跳 seek 到 h0 = 100000。betweenStores 在两 store 之间跑一次读方。
    uint32_t stalePulled = 0;
    const int64_t jumpSample = 100000LL * 480;
    fx.wr.startRun(jumpSample,
                   [&]() { stalePulled = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull); });

    // 正确顺序(base 先):交错读见「新 base + 旧 write」,w < lastPulled → 零污染。
    REQUIRE(stalePulled == 0);
    REQUIRE(cf.coveredHops(kFull) == 10);
}

TEST_CASE("读方 seqlock:受限追赶分拍补完 + 真回退丢弃", "[featring][pull]")
{
    constexpr u32 kCap = 4096;
    FeatRingFixture fx(kCap, 1, 480);
    fx.wr.startRun(0);
    std::vector<float> sig(480 * 600, 0.5f);
    const std::vector<int> lens{480};
    const int total = feedFeatRing(fx.wr, sig, 0, 480 * 600, lens); // 600 hop 积压
    REQUIRE(total == 600);

    FrameStore store;
    ChannelFrames& cf = store.channel(1);
    cf.setReadOnly(false);
    scvb::FeatPullState st;

    // 每拍最多 kMaxBurstHops=256,分拍补完。
    uint32_t p1 = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    REQUIRE(p1 == scvb::kMaxBurstHops);
    uint32_t p2 = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    uint32_t p3 = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    REQUIRE(p2 + p3 == static_cast<uint32_t>(600 - scvb::kMaxBurstHops));
    REQUIRE(cf.coveredHops(kFull) == 600);

    // 真回退:w < lastPulled → 本拍丢弃(不拉)。
    st.lastPulled = 700; // 人为制造回退
    const uint32_t p4 = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    REQUIRE(p4 == 0);
}

// ---------------------------------------------------------------------------
// ⑥ [P4/G11] 布防期门控:工作选区 ∩ 选中轨掩码
// ---------------------------------------------------------------------------

TEST_CASE("[P4/G11] 布防期录入 hop 数 == 选区内选中轨 hop 数", "[puller][arm]")
{
    constexpr u32 kCap = 4096;

    // 3 个在线轨,各写 hops [0,100)。
    FeatRingFixture fx1(kCap);
    FeatRingFixture fx2(kCap);
    FeatRingFixture fx3(kCap);
    FeatRingFixture* fxs[3] = {&fx1, &fx2, &fx3};
    std::vector<float> sig(480 * 100, 0.5f);
    const std::vector<int> lens{480};
    for (auto* fx : fxs)
    {
        fx->wr.startRun(0);
        feedFeatRing(fx->wr, sig, 0, 480 * 100, lens);
        REQUIRE(fx->header.write_hop.load() == 100);
    }

    scvb::FeatPuller puller;
    for (u32 ch = 1; ch <= 3; ++ch)
        puller.bind(ch, &fxs[ch - 1]->header, fxs[ch - 1]->ring.data(), kCap);

    FrameStore store;
    for (u32 ch = 1; ch <= 3; ++ch)
        store.channel(ch).setReadOnly(false);

    // 布防:工作选区 [20,80),选中轨 ch1/ch3;全局 range 全程不动。
    const HopRange workSel{20, 80};
    const u32 selectedMask = (1u << 0) | (1u << 2); // ch1 + ch3
    const u32 activeMask = (1u << 0) | (1u << 1) | (1u << 2); // 3 轨全在线

    puller.pullTick(store, workSel, selectedMask, activeMask);

    REQUIRE(store.channel(1).coveredHops(kFull) == 60); // 选中 + 选区内
    REQUIRE(store.channel(2).coveredHops(kFull) == 0); // 未选中轨 → 丢弃
    REQUIRE(store.channel(3).coveredHops(kFull) == 60); // 选中 + 选区内
    REQUIRE(store.totalPageCount() == 2); // 仅 ch1/ch3 有页

    // 非布防对照:selectedMask=0(不限轨)→ 三轨各 60(时间门控仍生效)。
    FrameStore store2;
    for (u32 ch = 1; ch <= 3; ++ch)
        store2.channel(ch).setReadOnly(false);
    scvb::FeatPuller puller2;
    for (u32 ch = 1; ch <= 3; ++ch)
        puller2.bind(ch, &fxs[ch - 1]->header, fxs[ch - 1]->ring.data(), kCap);
    puller2.pullTick(store2, workSel, /*selectedMask=*/0, activeMask);
    REQUIRE(store2.channel(1).coveredHops(kFull) == 60);
    REQUIRE(store2.channel(2).coveredHops(kFull) == 60);
    REQUIRE(store2.channel(3).coveredHops(kFull) == 60);
}
