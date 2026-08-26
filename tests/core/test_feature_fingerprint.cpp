// SPDX-License-Identifier: GPL-3.0-or-later
// test_feature_fingerprint —— 上游改动的过期检测(04 §4.5 fingerprint watchdog,[J46])。
//
// 覆盖三层,每层都有反向用例:
//   ① 量化/累加器:0.5dB 同桶等价、异桶不等价;
//   ② 两端一致性:同一段音频经 [Input FeatRing 采集 OFF 上报] 与 [Output FrameStore 基线重算]
//      得到**同一个** 48 位指纹;改了上游(增益/滤波)则不等 —— 这是整条链的判据本身;
//   ③ 判定:滞回(连续 <3 秒不算)、>10% 门槛(边界两侧)、重新匹配后自愈、无基线不计分母。
//
// 会话级(Input 命令环 → Output stale)的端到端在 test_output_session.cpp。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "analysis/FeatureFingerprint.h"
#include "analysis/FrameStore.h"
#include "ipc/CtrlPlane.h"
#include "ipc/FeatRing.h"

using scvb::kScvbAbi;
using scvb::kScvbMagic;
using scvb::analysis::ChannelFrames;
using scvb::analysis::FingerprintWatch;
using scvb::analysis::HopRange;
using scvb::analysis::kFpTileHops;
using scvb::analysis::TileFingerprint;

namespace
{

struct FeatFixture
{
    scvb::FeatHeader header;
    std::vector<scvb::FeatFrame> ring;

    FeatFixture() : ring(static_cast<std::size_t>(scvb::kFeatCapacityHops))
    {
        header.magic.store(kScvbMagic, std::memory_order_release);
        header.abi.store(kScvbAbi, std::memory_order_release);
        header.hop_ms = scvb::kFeatHopMs;
        header.capacity_hops = scvb::kFeatCapacityHops;
        header.base_hop.store(0, std::memory_order_relaxed);
        header.write_hop.store(0, std::memory_order_relaxed);
    }
};

// 一秒多一点的确定性测试信号(48kHz)。gain=1 是「原始素材」,gain≠1 模拟上游插了个改过的
// 处理器 —— 电平变了,K 加权 mean-square 就变,指纹必然不同。
std::vector<float> makeSignal(int samples, float gain)
{
    std::vector<float> v(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i)
    {
        const double t = static_cast<double>(i) / 48000.0;
        // 幅度随时间起伏,保证各 hop 的 kw 不是同一个常数(否则「所有 tile 同指纹」会掩盖 bug)。
        const double env = 0.3 + 0.25 * std::sin(2.0 * 3.14159265358979 * 0.7 * t);
        v[static_cast<std::size_t>(i)] = static_cast<float>(gain * env * std::sin(2.0 * 3.14159265358979 * 440.0 * t));
    }
    return v;
}

// 把整段信号喂给 FeatRing(按 512 一块,模拟宿主块流)。
void feed(scvb::FeatRing& fr, const std::vector<float>& mono)
{
    const int block = 512;
    const int total = static_cast<int>(mono.size());
    for (int off = 0; off < total; off += block)
    {
        const int n = std::min(block, total - off);
        const float* ptr = mono.data() + off;
        const float* planar[2] = {ptr, nullptr};
        fr.processBlock(planar, n);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// ① 量化与累加器
// ---------------------------------------------------------------------------
TEST_CASE("fpQuantizeDbq:0.5dB 同桶等价 / 异桶不等价", "[fingerprint][quant]")
{
    // dBq 单位 = 0.01dB。-2000 = -20.00dB;同桶内 ±0.49dB 不改变量化值。
    REQUIRE(scvb::analysis::fpQuantizeDbq(-2000) == scvb::analysis::fpQuantizeDbq(-2049));
    REQUIRE(scvb::analysis::fpQuantizeDbq(-2000) != scvb::analysis::fpQuantizeDbq(-2050));

    TileFingerprint a;
    TileFingerprint b;
    a.pushKwDbq(-2000);
    b.pushKwDbq(-2049);
    REQUIRE(a.value() == b.value()); // 同桶 → 同指纹(0.5dB 余量,反向验证在下一句)

    TileFingerprint c;
    c.pushKwDbq(-2050);
    REQUIRE(a.value() != c.value());
}

TEST_CASE("TileFingerprint:空累加器 = FNV-1a offset;顺序敏感", "[fingerprint][quant]")
{
    TileFingerprint fp;
    REQUIRE(fp.value() == scvb::analysis::kFnv1a64Offset);

    TileFingerprint ab;
    ab.pushKwDbq(-1000);
    ab.pushKwDbq(-3000);
    TileFingerprint ba;
    ba.pushKwDbq(-3000);
    ba.pushKwDbq(-1000);
    REQUIRE(ab.value() != ba.value()); // 顺序不同 → 指纹不同(不是简单求和)
}

// ---------------------------------------------------------------------------
// ② 两端一致性:Input 上报值 == Output 基线重算值
// ---------------------------------------------------------------------------
TEST_CASE("Input 上报指纹 == Output 基线重算(同一段音频)", "[fingerprint][ipc][e2e]")
{
    const std::vector<float> original = makeSignal(48000 * 2, 1.0f);

    // --- Output 侧基线:采集 ON 走真实写段 + pullIncremental 落 FrameStore ---
    FeatFixture cap;
    scvb::FeatRing writer;
    writer.bind(&cap.header, cap.ring.data(), scvb::kFeatCapacityHops);
    REQUIRE(writer.bound());
    writer.prepare(48000.0, 1, 512);
    writer.setCapturing(true);
    writer.startRun(0);
    feed(writer, original);

    ChannelFrames frames;
    frames.setReadOnly(false);
    scvb::FeatPullState pull;
    const HopRange all{0, std::numeric_limits<std::uint64_t>::max()};
    // 首拍是 run 确认拍(返回 0),第二拍起真拉。
    scvb::pullIncremental(cap.header, cap.ring.data(), scvb::kFeatCapacityHops, pull, frames, all);
    for (int i = 0; i < 4; ++i)
    {
        scvb::pullIncremental(cap.header, cap.ring.data(), scvb::kFeatCapacityHops, pull, frames, all);
    }
    REQUIRE(frames.coversFully(HopRange{0, kFpTileHops}));

    std::uint64_t baseline = 0;
    REQUIRE(scvb::analysis::baselineTileFingerprint(frames, 0, baseline));

    // --- Input 侧上报:采集 OFF,同一段音频 ---
    FeatFixture off;
    scvb::FeatRing reporter;
    reporter.bind(&off.header, off.ring.data(), scvb::kFeatCapacityHops);
    reporter.prepare(48000.0, 1, 512);
    reporter.setCapturing(false); // 采集 OFF:不写段,只跑 K 加权喂 fingerprint
    reporter.startRun(0);
    feed(reporter, original);

    // 采集 OFF ⇒ 段一个字节都不该被推进(「仅采集 ON 写」的冻结语义,反向断言)。
    REQUIRE(off.header.write_hop.load() == 0);

    scvb::u64 got[8] = {};
    const auto n = reporter.drainFpReports(got, 8);
    REQUIRE(n >= 1);
    REQUIRE(reporter.fpDropCount() == 0);
    REQUIRE(scvb::unpackFpReportTileIdx(got[0]) == 0);
    REQUIRE(scvb::unpackFpReportHash(got[0]) == (baseline & scvb::kFpReportHashMask));

    // --- 反向:上游改了(增益变化)⇒ 指纹必须对不上 ---
    const std::vector<float> processed = makeSignal(48000 * 2, 0.5f);
    FeatFixture eq;
    scvb::FeatRing changed;
    changed.bind(&eq.header, eq.ring.data(), scvb::kFeatCapacityHops);
    changed.prepare(48000.0, 1, 512);
    changed.setCapturing(false);
    changed.startRun(0);
    feed(changed, processed);

    scvb::u64 got2[8] = {};
    REQUIRE(changed.drainFpReports(got2, 8) >= 1);
    REQUIRE(scvb::unpackFpReportTileIdx(got2[0]) == 0);
    REQUIRE(scvb::unpackFpReportHash(got2[0]) != (baseline & scvb::kFpReportHashMask));
}

TEST_CASE("采集 ON 不上报 fp(此刻正在写新基线)", "[fingerprint][ipc]")
{
    FeatFixture f;
    scvb::FeatRing fr;
    fr.bind(&f.header, f.ring.data(), scvb::kFeatCapacityHops);
    fr.prepare(48000.0, 1, 512);
    fr.setCapturing(true);
    fr.startRun(0);
    feed(fr, makeSignal(48000 * 2, 1.0f));

    REQUIRE(f.header.write_hop.load() > kFpTileHops); // 段照写(既有语义未变)
    scvb::u64 got[8] = {};
    REQUIRE(fr.drainFpReports(got, 8) == 0); // 但一条 fp 都不上报
}

TEST_CASE("采集 OFF→ON(无 run 切换)重锚 hop:特征不再写到错误的时间线位置", "[fingerprint][ipc][featring]")
{
    // 既有缺陷,被 fingerprint watchdog 放大成用户可见的误报:采集开关在播放中途翻回 ON 时,
    // nextHop 还停在上次停采的位置,于是「第 30 秒的音频」被按「第 10 秒」记账,Output 的基线
    // 整体错位 —— 之后关采集重播同一段素材会**整轨失配**,滞回与 10% 两道门都拦不住。
    FeatFixture f;
    scvb::FeatRing fr;
    fr.bind(&f.header, f.ring.data(), scvb::kFeatCapacityHops);
    fr.prepare(48000.0, 1, 512);
    fr.startRun(0);

    // ① 采集 ON,录 1 秒。
    fr.setCapturing(true);
    feed(fr, makeSignal(48000, 1.0f));
    const std::uint64_t w1 = f.header.write_hop.load();
    REQUIRE(w1 > 0);
    REQUIRE(f.header.base_hop.load() == 0);

    // ② 采集 OFF,继续播 2 秒(= 200 hop)。段一个字节都不推进。
    fr.setCapturing(false);
    feed(fr, makeSignal(48000 * 2, 1.0f));
    REQUIRE(f.header.write_hop.load() == w1);

    // ③ 采集重新 ON(**没有** run 切换):重锚到当前时间线位置,而不是从 w1 续写。
    fr.setCapturing(true);
    feed(fr, makeSignal(48000, 1.0f));
    const std::uint64_t base = f.header.base_hop.load();
    CHECK(base > w1); // ← 修复前恒为 0,新帧被按旧 hop 号记账
    CHECK(base >= w1 + 198); // OFF 期约 200 hop(±2 容边界 hop)
    CHECK(base <= w1 + 202);
    CHECK(f.header.write_hop.load() > base); // 重锚后照常写
}

TEST_CASE("run 中途接上的半个 tile 不上报", "[fingerprint][ipc]")
{
    FeatFixture f;
    scvb::FeatRing fr;
    fr.bind(&f.header, f.ring.data(), scvb::kFeatCapacityHops);
    fr.prepare(48000.0, 1, 512);
    fr.setCapturing(false);
    // hop 50 起播:tile 0 只剩后半截,不构成完整 1 秒 → 不得上报 tile 0。
    fr.startRun(50 * 480);
    feed(fr, makeSignal(48000, 1.0f)); // 1 秒:跨到 hop 150,只有 tile 1 无法凑满

    scvb::u64 got[8] = {};
    const auto n = fr.drainFpReports(got, 8);
    for (std::uint32_t i = 0; i < n; ++i)
    {
        REQUIRE(scvb::unpackFpReportTileIdx(got[i]) != 0); // tile 0 永不出现
    }
}

// ---------------------------------------------------------------------------
// ③ 判定:滞回 / 10% 门槛 / 自愈 / 无基线
// ---------------------------------------------------------------------------
namespace
{

// 给 frames 铺 tileCount 个 tile 的基线(kw 随 hop 变化,避免各 tile 同指纹)。
void fillBaseline(ChannelFrames& frames, std::uint32_t tileCount)
{
    frames.setReadOnly(false);
    const std::uint64_t hops = static_cast<std::uint64_t>(tileCount) * kFpTileHops;
    for (std::uint64_t h = 0; h < hops; ++h)
    {
        frames.write(h, 0.01f + 0.0001f * static_cast<float>(h % 137), 0.5f);
    }
}

std::uint64_t matchingValue(const ChannelFrames& frames, std::uint32_t tile)
{
    std::uint64_t base = 0;
    REQUIRE(scvb::analysis::baselineTileFingerprint(frames, tile, base));
    return scvb::packFpReport(tile, base);
}

std::uint64_t mismatchingValue(const ChannelFrames& frames, std::uint32_t tile)
{
    std::uint64_t base = 0;
    REQUIRE(scvb::analysis::baselineTileFingerprint(frames, tile, base));
    return scvb::packFpReport(tile, base ^ 0x5A5Aull);
}

} // namespace

TEST_CASE("指纹一致 → 不 stale(反向基准)", "[fingerprint][watch]")
{
    ChannelFrames frames;
    fillBaseline(frames, 20);
    FingerprintWatch w;
    for (std::uint32_t t = 0; t < 20; ++t)
    {
        w.onReport(1, matchingValue(frames, t), frames);
    }
    REQUIRE(w.tilesChecked(1) == 20);
    REQUIRE(w.tilesMismatched(1) == 0);
    REQUIRE_FALSE(w.stale(1));
}

TEST_CASE("整段失配 → stale=true", "[fingerprint][watch]")
{
    ChannelFrames frames;
    fillBaseline(frames, 20);
    FingerprintWatch w;
    for (std::uint32_t t = 0; t < 20; ++t)
    {
        w.onReport(1, mismatchingValue(frames, t), frames);
    }
    REQUIRE(w.tilesChecked(1) == 20);
    REQUIRE(w.tilesMismatched(1) == 20);
    REQUIRE(w.stale(1));
    REQUIRE_FALSE(w.stale(2)); // 别的轨不受牵连
}

TEST_CASE("滞回:连续 2 秒失配不计数,第 3 秒才定谳", "[fingerprint][watch]")
{
    ChannelFrames frames;
    fillBaseline(frames, 20);

    FingerprintWatch two;
    for (std::uint32_t t = 0; t < 18; ++t)
    {
        two.onReport(1, matchingValue(frames, t), frames);
    }
    two.onReport(1, mismatchingValue(frames, 18), frames);
    two.onReport(1, mismatchingValue(frames, 19), frames);
    REQUIRE(two.tilesMismatched(1) == 0); // 只有 2 秒 → 一条都不计
    REQUIRE_FALSE(two.stale(1));

    // 同样的账本,再来一个相邻失配 tile → 连同前两个一并定谳(3 条,不是 1 条)。
    FingerprintWatch three;
    for (std::uint32_t t = 0; t < 17; ++t)
    {
        three.onReport(1, matchingValue(frames, t), frames);
    }
    three.onReport(1, mismatchingValue(frames, 17), frames);
    three.onReport(1, mismatchingValue(frames, 18), frames);
    three.onReport(1, mismatchingValue(frames, 19), frames);
    REQUIRE(three.tilesMismatched(1) == 3);
}

TEST_CASE("滞回:不相邻的失配不连成一段", "[fingerprint][watch]")
{
    ChannelFrames frames;
    fillBaseline(frames, 20);
    FingerprintWatch w;
    // 失配 tile 之间夹着匹配 tile → 连续段长度永远是 1,永不定谳。
    for (std::uint32_t t = 0; t < 20; ++t)
    {
        w.onReport(1, (t % 2 == 0) ? mismatchingValue(frames, t) : matchingValue(frames, t), frames);
    }
    REQUIRE(w.tilesMismatched(1) == 0);
    REQUIRE_FALSE(w.stale(1));
}

TEST_CASE("10% 门槛:严格大于才 stale(边界两侧)", "[fingerprint][watch]")
{
    ChannelFrames frames;
    fillBaseline(frames, 40);

    // 30 个已比对 + 3 个失配 = 恰好 10% → **不** stale(设计写的是「>10%」)。
    FingerprintWatch at;
    for (std::uint32_t t = 0; t < 3; ++t)
    {
        at.onReport(1, mismatchingValue(frames, t), frames);
    }
    for (std::uint32_t t = 3; t < 30; ++t)
    {
        at.onReport(1, matchingValue(frames, t), frames);
    }
    REQUIRE(at.tilesChecked(1) == 30);
    REQUIRE(at.tilesMismatched(1) == 3);
    REQUIRE_FALSE(at.stale(1));

    // 29 个已比对 + 3 个失配 > 10% → stale。
    FingerprintWatch over;
    for (std::uint32_t t = 0; t < 3; ++t)
    {
        over.onReport(1, mismatchingValue(frames, t), frames);
    }
    for (std::uint32_t t = 3; t < 29; ++t)
    {
        over.onReport(1, matchingValue(frames, t), frames);
    }
    REQUIRE(over.tilesChecked(1) == 29);
    REQUIRE(over.stale(1));
}

TEST_CASE("重新匹配 → 该 tile 的失配定谳撤销(可自愈)", "[fingerprint][watch]")
{
    ChannelFrames frames;
    fillBaseline(frames, 5);
    FingerprintWatch w;
    for (std::uint32_t t = 0; t < 5; ++t)
    {
        w.onReport(1, mismatchingValue(frames, t), frames);
    }
    REQUIRE(w.stale(1));

    // 用户把上游改回去了 / 重新采集了:再播一遍全部匹配 → 提示自行撤下。
    for (std::uint32_t t = 0; t < 5; ++t)
    {
        w.onReport(1, matchingValue(frames, t), frames);
    }
    REQUIRE(w.tilesMismatched(1) == 0);
    REQUIRE_FALSE(w.stale(1));
}

TEST_CASE("无基线的 tile 不比对、不计分母", "[fingerprint][watch]")
{
    ChannelFrames frames;
    fillBaseline(frames, 2); // 只有 tile 0/1 有基线
    FingerprintWatch w;
    for (std::uint32_t t = 2; t < 10; ++t)
    {
        w.onReport(1, scvb::packFpReport(t, 0xDEADBEEFull), frames);
    }
    REQUIRE(w.tilesChecked(1) == 0);
    REQUIRE_FALSE(w.stale(1)); // 没采过的区间绝不冒出「数据过期」
}

TEST_CASE("resetChannel / reset 清账", "[fingerprint][watch]")
{
    ChannelFrames frames;
    fillBaseline(frames, 5);
    FingerprintWatch w;
    for (std::uint32_t t = 0; t < 5; ++t)
    {
        w.onReport(1, mismatchingValue(frames, t), frames);
    }
    REQUIRE(w.stale(1));
    w.resetChannel(1);
    REQUIRE_FALSE(w.stale(1));
    REQUIRE(w.tilesChecked(1) == 0);
}

TEST_CASE("越界 channel 静默忽略,不越界写", "[fingerprint][watch]")
{
    ChannelFrames frames;
    fillBaseline(frames, 2);
    FingerprintWatch w(15);
    w.onReport(0, matchingValue(frames, 0), frames);
    w.onReport(16, matchingValue(frames, 0), frames);
    REQUIRE_FALSE(w.stale(0));
    REQUIRE_FALSE(w.stale(16));
}

TEST_CASE("部分覆盖的 tile 无基线(打洞后不再提示)", "[fingerprint][watch]")
{
    ChannelFrames frames;
    fillBaseline(frames, 2);
    std::uint64_t before = 0;
    REQUIRE(scvb::analysis::baselineTileFingerprint(frames, 0, before));

    // 重采集打洞:tile 0 少了一个 hop → 不再完整覆盖 → 没有基线可比。
    frames.invalidate(HopRange{10, 11});
    std::uint64_t after = 0;
    REQUIRE_FALSE(scvb::analysis::baselineTileFingerprint(frames, 0, after));
}
