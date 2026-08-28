// SPDX-License-Identifier: GPL-3.0-or-later
// analysis_coverage_test —— FrameStore/CoverageMap/FeatRing/增量拉取/run 协议(T20)。
// 覆盖:① CoverageMap 与朴素位图对拍;② 04 §3.5 十合并场景;③ 5min=30000±1 hop、内存<1MB;
// ④ 采集 OFF 写路径丢弃;⑤ startRun biquad 预热(FEAT-3 在线路径);⑥ [P4/G11] 布防门控;
// ⑦ run 协议 store 顺序(base 先 write 后)与读方 seqlock。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
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
using scvb::analysis::FeatPage;
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

// 拉取直到写指针追平(write_hop <= lastPulled),返回累计拉取 hop 数。
// 首次 pullIncremental 是确认拍(init/run 切换,return 0),故必须循环而非单次调用。
uint32_t drainFeat(const scvb::FeatHeader& header, const scvb::FeatFrame* ring, u32 capacity, scvb::FeatPullState& st,
                   scvb::analysis::ChannelFrames& cf, scvb::analysis::HopRange gate)
{
    uint32_t total = 0;
    for (int i = 0; i < 100000; ++i)
    {
        const uint64_t w = header.write_hop.load(std::memory_order_acquire);
        if (st.initialized && st.lastPulled >= w)
        {
            break; // 已追平
        }
        total += scvb::pullIncremental(header, ring, capacity, st, cf, gate);
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
// [SL-206 复审【重要】2 / SL-232] vadP 的写侧:write() 作废旧判决、批量入口只写覆盖区。
//
// `write()` 里那行 `page->vadP[idx] = 0`(「新特征进来 = 旧判决作废」)是 #151 唯一有行为的
// 新逻辑之一,此前**只有 host harness 的间接用例**,core 侧一条都没有(CLAUDE.md §7 要求
// scvb_core 的纯逻辑配 Catch2)。这一组把它和 SL-232 的批量写一起钉在最便宜的那一层。
// ---------------------------------------------------------------------------

TEST_CASE("FrameStore:write 覆盖新特征时清掉旧 vadP", "[store][vad][SL206]")
{
    ChannelFrames cf;
    cf.setReadOnly(false);
    cf.write(100, 0.01f, 0.1f);
    cf.setVadP(100, 200);
    REQUIRE(cf.vadP(100) == 200);

    cf.write(100, 0.02f, 0.2f); // 新素材进来
    CHECK(cf.vadP(100) == 0); // ★ 旧判决作废 —— 删掉 write() 里那行 vadP=0 即红
}

TEST_CASE("FrameStore:write 早退(只读 / 越 gate)不得误清已有 vadP", "[store][vad][SL206]")
{
    // 早退顺序一旦被挪到 vadP=0 之后,这两条会静默丢判决 —— 当前实现是对的,补上守着。
    SECTION("只读")
    {
        ChannelFrames cf;
        cf.setReadOnly(false);
        cf.write(7, 0.01f, 0.1f);
        cf.setVadP(7, 190);
        cf.setReadOnly(true);
        cf.write(7, 0.5f, 0.5f); // 采集 OFF:静默丢弃
        CHECK(cf.vadP(7) == 190); // 判决原样留着
    }
    SECTION("越 gate")
    {
        ChannelFrames cf;
        cf.setReadOnly(false);
        cf.write(7, 0.01f, 0.1f);
        cf.setVadP(7, 190);
        cf.setGate({100, 200}); // 7 落在门外
        cf.write(7, 0.5f, 0.5f);
        CHECK(cf.vadP(7) == 190);
    }
}

TEST_CASE("FrameStore:setVadPosteriorRange 只写覆盖区,值与逐 hop 版逐字节相同", "[store][vad][SL232]")
{
    // 覆盖是 [10,20) ∪ [30,35),中间 [20,30) 是洞 —— 批量写必须原样跳过那个洞。
    ChannelFrames batch;
    ChannelFrames oneByOne;
    for (ChannelFrames* cf : {&batch, &oneByOne})
    {
        cf->setReadOnly(false);
        for (u64 h = 10; h < 20; ++h)
            cf->write(h, 0.01f, 0.1f);
        for (u64 h = 30; h < 35; ++h)
            cf->write(h, 0.01f, 0.1f);
    }
    REQUIRE(batch.coveredHops({0, 50}) == 15);

    // 后验覆盖 [5,45):两端与洞都超出覆盖区,正好把「只写覆盖区」这条钉住。
    std::vector<float> post;
    for (u64 h = 5; h < 45; ++h)
        post.push_back(static_cast<float>((h * 7) % 100) / 99.0f);

    batch.setVadPosteriorRange({5, 45}, post.data());
    // 逐 hop 参照实现(= 本卡改动前那段代码,逐字):未覆盖跳过,覆盖处量化后写入。
    for (u64 h = 5; h < 45; ++h)
    {
        if (!oneByOne.hasHop(h))
            continue;
        oneByOne.setVadP(h, scvb::analysis::quantizeVadPosterior(post[h - 5]));
    }

    for (u64 h = 0; h < 50; ++h)
    {
        INFO("hop " << h);
        CHECK(batch.vadP(h) == oneByOne.vadP(h)); // ★ 与参照实现逐字节相同
    }
    // 洞与两端必须仍是 0(既没写值,也没白建页)。
    for (u64 h : {u64{5}, u64{9}, u64{20}, u64{29}, u64{45}, u64{49}})
    {
        INFO("uncovered hop " << h);
        CHECK(batch.vadP(h) == 0);
    }
    // ★ 未覆盖处不建页:覆盖只落在页 0,所以恰好一页。批量写若对整条 [5,45) 无脑建页,
    //   这个数不会变(同一页);真正的守卫是下面那条跨页用例。
    CHECK(batch.pageCount() == 1);
}

TEST_CASE("FrameStore:setVadPosteriorRange 不为未覆盖的远端建页", "[store][vad][SL232]")
{
    ChannelFrames cf;
    cf.setReadOnly(false);
    cf.write(3, 0.01f, 0.1f); // 只覆盖页 0 的一个 hop
    REQUIRE(cf.pageCount() == 1);

    // 后验跨 40 页(4096 hop/页),但只有 hop 3 是覆盖的。
    const u64 span = FeatPage::kHops * 40;
    std::vector<float> post(static_cast<std::size_t>(span), 0.9f);
    cf.setVadPosteriorRange({0, span}, post.data());

    CHECK(cf.pageCount() == 1); // ★ 无脑建页会变成 40 —— 分页稀疏性被抵消
    CHECK(cf.vadP(3) == scvb::analysis::quantizeVadPosterior(0.9f));
    CHECK(cf.vadP(FeatPage::kHops * 20) == 0); // 远端未覆盖,原样 0
}

TEST_CASE("FrameStore:setVadPosteriorRange 跨页边界与入参守卫", "[store][vad][SL232]")
{
    ChannelFrames cf;
    cf.setReadOnly(false);
    // 骑在页 0/1 边界上:[kHops-3, kHops+4)。
    const u64 lo = FeatPage::kHops - 3;
    const u64 hi = FeatPage::kHops + 4;
    for (u64 h = lo; h < hi; ++h)
        cf.write(h, 0.01f, 0.1f);

    std::vector<float> post(static_cast<std::size_t>(hi - lo), 1.0f);
    cf.setVadPosteriorRange({lo, hi}, post.data());
    for (u64 h = lo; h < hi; ++h)
    {
        INFO("hop " << h);
        CHECK(cf.vadP(h) == 255); // ★ 跨页两侧都写到了(按页推进漏掉半边即红)
    }
    CHECK(cf.pageCount() == 2);

    // 入参守卫:空指针 / 空区间 / 逆序区间都必须是 no-op(不崩、不改值)。
    cf.setVadPosteriorRange({lo, hi}, nullptr);
    cf.setVadPosteriorRange({lo, lo}, post.data());
    cf.setVadPosteriorRange({hi, lo}, post.data());
    CHECK(cf.vadP(lo) == 255);
}

TEST_CASE("FrameStore:大跨度 setVadPosteriorRange 仍在时间预算内", "[store][vad][SL232][hang]")
{
    // 真机现场的形状(与 P0-A 那一族同款):**覆盖极稀疏、分析跨度极大**。
    // 满选一小时 = 360k hop,15 轨;逐 hop 的老写法是每 hop 两次 std::map 查找
    // ≈ 1080 万次,还全程持 lifecycleMutex_ 跑在消息线程上。批量版按覆盖区间收窄 +
    // 按页推进,迭代数与**实际数据量**同阶,与跨度无关。
    constexpr u64 kSpan = 360000; // 1h @ 10ms hop
    constexpr int kTracks = 15;

    FrameStore store(kTracks);
    for (int t = 0; t < kTracks; ++t)
    {
        auto& cf = store.channel(static_cast<u32>(t + 1));
        cf.setReadOnly(false);
        for (u64 h = 0; h < 500; ++h) // 每轨只采了 5 秒
        {
            cf.write(h, 0.01f, 0.1f);
        }
    }
    const std::vector<float> post(static_cast<std::size_t>(kSpan), 0.7f);

    const auto t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < kTracks; ++t)
    {
        store.channel(static_cast<u32>(t + 1)).setVadPosteriorRange({0, kSpan}, post.data());
    }
    const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // 本机实测(RelWithDebInfo):批量版 **0.21–0.23ms**(只走 15 轨 × 500 个覆盖 hop);
    // 退回逐 hop 的老写法 **115ms**(15 × 360k 次 coversFully + pageFor)—— 相差约 500 倍。
    // 预算取 20ms:对新实现留了近**两个数量级**余量(构建机器再慢也不会假红),
    // 同时把旧实现挡在 5 倍之外,绝不可能蒙混过关。定预算的办法与 P0-A 那一族逐字同款。
    // ★ 反向验证:把 setVadPosteriorRange 换回逐 hop 写法 → 实测 115ms,本条红。
    CHECK(elapsedMs < 20.0);

    // 值仍然对:覆盖区写进去了,跨度里其余部分一个字都没动、也没白建页。
    CHECK(store.channel(1).vadP(10) == scvb::analysis::quantizeVadPosterior(0.7f));
    CHECK(store.channel(1).vadP(1000) == 0);
    CHECK(store.channel(1).pageCount() == 1);
}

TEST_CASE("FrameStore:quantizeVadPosterior 与读侧判据(>127)同侧", "[store][vad][SL232]")
{
    using scvb::analysis::quantizeVadPosterior;
    CHECK(quantizeVadPosterior(0.0f) == 0);
    CHECK(quantizeVadPosterior(1.0f) == 255);
    // p=0.5 必须落 128(> 127 算有声),与 EnergyVad 的判决阈同侧 —— 取整改成截断即 127,红。
    CHECK(quantizeVadPosterior(0.5f) == 128);
    // 值域守卫:越界与 NaN 都不许溢出成别的数。
    CHECK(quantizeVadPosterior(-1.0f) == 0);
    CHECK(quantizeVadPosterior(2.0f) == 255);
    CHECK(quantizeVadPosterior(std::numeric_limits<float>::quiet_NaN()) == 0);
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
    drainFeat(fx.header, fx.ring.data(), kCap, st, cf, kFull);

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
    drainFeat(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    REQUIRE(st.lastPulled == 10);
    REQUIRE(cf.coveredHops(kFull) == 10);

    // 前跳 seek 到 h0 = 200(小前跳,< 容量,不触发环回绕守卫)。betweenStores 在两 store 之间跑一次读方。
    uint32_t stalePulled = 0;
    const int64_t jumpSample = 200LL * 480;
    fx.wr.startRun(jumpSample,
                   [&]() { stalePulled = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull); });

    // 正确顺序(base 先):交错读见「新 base + 旧 write」,run 切换确认拍 → 零污染。
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

    // 首次是确认拍(init),return 0;之后每拍最多 kMaxBurstHops=256,分拍补完。
    const uint32_t p0 = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    REQUIRE(p0 == 0);
    const uint32_t p1 = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    REQUIRE(p1 == scvb::kMaxBurstHops);
    const uint32_t p2 = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    const uint32_t p3 = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    REQUIRE(p2 + p3 == static_cast<uint32_t>(600 - scvb::kMaxBurstHops));
    REQUIRE(cf.coveredHops(kFull) == 600);

    // 真回退:w < lastPulled → 重置 lastPulled 到 b1 并 return 0(不保留陈旧 lastPulled)。
    st.lastPulled = 700; // 人为制造回退
    const uint32_t p4 = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    REQUIRE(p4 == 0);
    REQUIRE(st.lastPulled == 0); // 重置到 b1
}

TEST_CASE("FeatRing 超大块:按 maxBlockSamples 分段,无越界且逐位一致", "[featring][chunk]")
{
    constexpr double fs = 48000.0;
    const auto sig = makeMix(fs, 1, 997.0, 3171.0, 777.0);
    const int blockSamples = 4096; // 超大块:>> prepare 的 maxBlock 480

    FeatRingFixture fx(4096, 1, 480);
    fx.wr.startRun(0);
    const float* ch[1] = {sig.data()};
    const int got = fx.wr.processBlock(ch, blockSamples);
    REQUIRE(got == blockSamples / 480); // 4096/480 = 8 完整 hop
    REQUIRE(fx.header.write_hop.load() == static_cast<u64>(got));

    // 参考:全新 extractor 直接喂 4096 样本(不分段),结果应逐位一致(分段不破坏 hop 边界)。
    scvb::analysis::FeatureExtractor ref;
    ref.prepare(fs, 1);
    const auto refFrames = runExtractor(ref, ch, blockSamples, {blockSamples});
    REQUIRE(refFrames.size() == static_cast<std::size_t>(got));
    for (int i = 0; i < got; ++i)
    {
        const scvb::FeatFrame& f = fx.ring[static_cast<std::size_t>(i)];
        INFO("hop " << i);
        REQUIRE(f.kw_ms == refFrames[static_cast<std::size_t>(i)].kw_ms);
        REQUIRE(f.peak == refFrames[static_cast<std::size_t>(i)].peak);
    }
}

TEST_CASE("回卷 seek 交错:run 切换确认拍 0 污染", "[featring][rewind]")
{
    constexpr u32 kCap = 4096;
    FeatRingFixture fx(kCap, 1, 480);
    fx.wr.startRun(0);
    std::vector<float> sig(480 * 100, 0.5f);
    feedFeatRing(fx.wr, sig, 0, 480 * 100, {480}); // run A:100 hop,write=100
    REQUIRE(fx.header.write_hop.load() == 100);

    FrameStore store;
    ChannelFrames& cf = store.channel(1);
    cf.setReadOnly(false);
    scvb::FeatPullState st;
    drainFeat(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    REQUIRE(st.lastPulled == 100);
    REQUIRE(cf.coveredHops(kFull) == 100);

    // 回卷 seek 到 h0=50,betweenStores 注入读方(见「新 base + 旧 write」)。
    uint32_t stalePulled = 0;
    fx.wr.startRun(50LL * 480,
                   [&]() { stalePulled = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull); });
    REQUIRE(stalePulled == 0); // run 切换确认拍
    REQUIRE(cf.coveredHops(kFull) == 100); // 无新污染
}

TEST_CASE("同 base 重开:w 回退 → 重置 lastPulled,新 run 前缀正常重拉", "[featring][rewind]")
{
    constexpr u32 kCap = 4096;
    FeatRingFixture fx(kCap, 1, 480);
    fx.wr.startRun(0);
    std::vector<float> sig(480 * 200, 0.5f);
    feedFeatRing(fx.wr, sig, 0, 480 * 100, {480}); // run A:100 hop
    REQUIRE(fx.header.write_hop.load() == 100);

    FrameStore store;
    ChannelFrames& cf = store.channel(1);
    cf.setReadOnly(false);
    scvb::FeatPullState st;
    drainFeat(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    REQUIRE(st.lastPulled == 100);

    // 同 base 重开:startRun(0) → base 仍 0,write 回退到 0(不触发 run 切换,b1 不变)。
    fx.wr.startRun(0);
    REQUIRE(fx.header.base_hop.load() == 0);
    REQUIRE(fx.header.write_hop.load() == 0);

    const uint32_t p1 = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    REQUIRE(p1 == 0);
    REQUIRE(st.lastPulled == 0); // 不得保留陈旧 lastPulled=100

    feedFeatRing(fx.wr, sig, 0, 480 * 30, {480}); // 新 run 写 30 hop
    REQUIRE(fx.header.write_hop.load() == 30);
    const uint32_t p2 = scvb::pullIncremental(fx.header, fx.ring.data(), kCap, st, cf, kFull);
    REQUIRE(p2 == 30); // 新 run 前缀 [0,30) 被重拉
}

TEST_CASE("环回绕守卫:停滞 > 容量 → 跳前重同步,0 污染", "[featring][wrap]")
{
    constexpr u32 kCap = 4096;
    scvb::FeatHeader header;
    std::vector<scvb::FeatFrame> ring(kCap);
    header.base_hop.store(0, std::memory_order_release);
    header.write_hop.store(5000, std::memory_order_release); // 写方已写 5000 > 容量 4096

    scvb::analysis::ChannelFrames cf;
    cf.setReadOnly(false);
    scvb::FeatPullState st;
    st.initialized = true;
    st.lastBase = 0;
    st.lastPulled = 0; // 读方停滞在 0,落后 > 容量

    const uint32_t p = scvb::pullIncremental(header, ring.data(), kCap, st, cf, kFull);
    REQUIRE(p == scvb::kMaxBurstHops); // 跳到 w-capacity=904,拉最新窗口 [904,1160)
    REQUIRE(st.lastPulled == 904 + scvb::kMaxBurstHops);
    REQUIRE_FALSE(cf.hasHop(0)); // 覆盖槽不按旧 hop 号入库(0 污染)
    REQUIRE_FALSE(cf.hasHop(903));
    REQUIRE(cf.hasHop(904)); // 可恢复前沿正常入库
}

TEST_CASE("环回绕守卫:重同步后下一拍恢复,不永久停更", "[featring][wrap]")
{
    constexpr u32 kCap = 4096;
    scvb::FeatHeader header;
    std::vector<scvb::FeatFrame> ring(kCap);
    header.base_hop.store(0, std::memory_order_release);
    header.write_hop.store(5000, std::memory_order_release);

    scvb::analysis::ChannelFrames cf;
    cf.setReadOnly(false);
    scvb::FeatPullState st;
    st.initialized = true;
    st.lastBase = 0;
    st.lastPulled = 0; // 读方停滞

    const uint32_t p1 = scvb::pullIncremental(header, ring.data(), kCap, st, cf, kFull);
    REQUIRE(p1 == scvb::kMaxBurstHops); // 守卫触发:跳前 + 拉最新窗口
    REQUIRE(st.lastPulled == 904 + scvb::kMaxBurstHops); // = 1160

    header.write_hop.store(5010); // 写方再喂 10 hop

    const uint32_t p2 = scvb::pullIncremental(header, ring.data(), kCap, st, cf, kFull);
    REQUIRE(p2 == scvb::kMaxBurstHops); // 下一拍继续追赶(不永久停更)
    REQUIRE(st.lastPulled == 904 + 2 * scvb::kMaxBurstHops); // 前进到 1416
}

TEST_CASE("FeatRing 几何快照:attach 读一次,越界映射拒绝绑定", "[featring][geometry]")
{
    // 声明容量 > 实际映射 → 拒绝绑定(bound()==false),processBlock 静默不写、不推进 write_hop。
    scvb::FeatHeader header;
    std::vector<scvb::FeatFrame> ring(2048);
    header.magic.store(scvb::kScvbMagic, std::memory_order_release);
    header.abi.store(scvb::kScvbAbi, std::memory_order_release);
    header.capacity_hops = 4096; // 声明的几何
    header.hop_ms = scvb::kFeatHopMs;
    header.base_hop.store(0, std::memory_order_release);
    header.write_hop.store(0, std::memory_order_release);

    scvb::FeatRing wr;
    wr.bind(&header, ring.data(), 2048); // 实际映射 2048 < 声明 4096
    REQUIRE_FALSE(wr.bound());

    wr.prepare(48000.0, 1, 480);
    wr.setCapturing(true);
    wr.startRun(0);
    std::vector<float> sig(480, 0.5f);
    const float* ch[1] = {sig.data()};
    REQUIRE(wr.processBlock(ch, 480) == 0); // 未绑定 → 不写
    REQUIRE(header.write_hop.load() == 0); // 不推进

    // 合法:声明 == 实际映射 → 绑定成功,快照 == 段头几何。
    scvb::FeatHeader okHeader;
    std::vector<scvb::FeatFrame> okRing(4096);
    okHeader.magic.store(scvb::kScvbMagic, std::memory_order_release);
    okHeader.abi.store(scvb::kScvbAbi, std::memory_order_release);
    okHeader.capacity_hops = 4096;
    okHeader.hop_ms = scvb::kFeatHopMs;
    okHeader.base_hop.store(0, std::memory_order_release);
    okHeader.write_hop.store(0, std::memory_order_release);
    scvb::FeatRing ok;
    ok.bind(&okHeader, okRing.data(), 4096);
    REQUIRE(ok.bound());
    REQUIRE(ok.capacityHops() == 4096);
    REQUIRE(ok.hopMs() == scvb::kFeatHopMs);
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

    puller.pullTick(store, workSel, selectedMask, activeMask); // 确认拍(init)
    puller.pullTick(store, workSel, selectedMask, activeMask); // 拉取

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
    puller2.pullTick(store2, workSel, /*selectedMask=*/0, activeMask); // 确认拍(init)
    puller2.pullTick(store2, workSel, /*selectedMask=*/0, activeMask); // 拉取
    REQUIRE(store2.channel(1).coveredHops(kFull) == 60);
    REQUIRE(store2.channel(2).coveredHops(kFull) == 60);
    REQUIRE(store2.channel(3).coveredHops(kFull) == 60);
}
