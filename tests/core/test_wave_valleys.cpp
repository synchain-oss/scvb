// SPDX-License-Identifier: GPL-3.0-or-later
// [J145] 波形瓦片的吸附谷点(`snapValleysSeconds`,契约 §1.27 `requestWaveform.valleys[]`)。
//
// 这一层钉的是「FrameStore → 秒」这一跳:覆盖段逐段检测、谷点取 hop 中心、窗外的丢掉、
// 两侧上下文、条数上限与扫描上限。算法本身(detectSnapValleys)在 test_segmentation.cpp 的 [J145] 组;
// 「采集 → FrameStore → waveformOf 的瓦片」整条链在 host harness 的 `HOST J145` 用例。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "analysis/FrameStore.h"
#include "analysis/Segmentation.h"
#include "analysis/WaveValleys.h"

using Catch::Approx;
using scvb::analysis::FrameStore;
using scvb::analysis::SegmentationParams;
using scvb::analysis::snapValleysSeconds;

namespace
{

constexpr double kHopS = 0.01;
constexpr float kLoudKw = 0.1f; // ≈ −10.7 LUFS
constexpr float kSilentKw = 0.0f; // 数字静音 ⇒ 量化地板 −120 dB

// 往 ch 的 [begin,end) 逐 hop 写同一个 kw(peak 随手给一个非地板值)。
void fillKw(FrameStore& fs, std::uint32_t ch, std::uint64_t begin, std::uint64_t end, float kw)
{
    auto& c = fs.channel(ch);
    c.setReadOnly(false); // 采集 ON 才写得进(write() 的第一道门)
    for (std::uint64_t h = begin; h < end; ++h)
    {
        c.write(h, kw, 0.5f);
    }
}

// 句子 [1000,1300) · 数字静音 [1300,1350) · 句子 [1350,1650)。
// 平滑后谷底平坦区 = [1302,1347] ⇒ rep = round(1324.5) = 1325 ⇒ 时刻 = (1325+0.5)·10ms = 13.255 s;
// 间隙的几何中心 = (13.00 + 13.50)/2 = 13.25 s —— 差半个 hop,是「取 hop 中心」的口径,不是误差。
FrameStore oneGap()
{
    FrameStore fs;
    fillKw(fs, 1, 1000, 1300, kLoudKw);
    fillKw(fs, 1, 1300, 1350, kSilentKw);
    fillKw(fs, 1, 1350, 1650, kLoudKw);
    return fs;
}

} // namespace

TEST_CASE("[J145] 已知静音间隙 ⇒ 谷点落在间隙中心(秒)", "[wavevalleys][J145]")
{
    const FrameStore fs = oneGap();
    const auto v = snapValleysSeconds(fs.channel(1), 10.0, 16.5, kHopS, SegmentationParams{}, 256);
    REQUIRE(v.size() == 1);
    CHECK(v[0] == Approx(13.255).margin(1e-9));
    CHECK(v[0] > 13.0); // 在间隙里
    CHECK(v[0] < 13.5);
}

TEST_CASE("[J145] 窗外的谷不回;窗很窄时靠两侧上下文仍认得出同一个谷", "[wavevalleys][J145]")
{
    const FrameStore fs = oneGap();
    // 谷在 13.255:窗 [13.3,16.5) 不含它 ⇒ 空。
    CHECK(snapValleysSeconds(fs.channel(1), 13.3, 16.5, kHopS, SegmentationParams{}, 256).empty());
    // 窗只有 10 个 hop 宽、整个落在静音里:窗内的 ℓ 是平的,单看窗内**认不出**任何谷。
    // 两侧各 kSnapValleyContextHops 的上下文把两侧句子带进来 ⇒ 与宽窗逐位同一个结果。
    // (删除式:把 kSnapValleyContextHops 改成 0 ⇒ 本条红。)
    const auto narrow = snapValleysSeconds(fs.channel(1), 13.2, 13.3, kHopS, SegmentationParams{}, 1);
    REQUIRE(narrow.size() == 1);
    CHECK(narrow[0] == Approx(13.255).margin(1e-9));
}

TEST_CASE("[J145] 未覆盖的空洞不是静音:不得跨过它连成一个谷", "[wavevalleys][J145]")
{
    FrameStore fs;
    fillKw(fs, 1, 1000, 1300, kLoudKw);
    fillKw(fs, 1, 1350, 1650, kLoudKw); // [1300,1350) 没采过
    CHECK(snapValleysSeconds(fs.channel(1), 10.0, 16.5, kHopS, SegmentationParams{}, 256).empty());
    // 没采过的轨:空。
    CHECK(snapValleysSeconds(fs.channel(2), 10.0, 16.5, kHopS, SegmentationParams{}, 256).empty());
}

TEST_CASE("[J145] 条数上限留最深的,并按时间升序回", "[wavevalleys][J145]")
{
    // 三个间隙,底分别是 −40 / −120(数字静音)/ −60 dB(kw = 1e-4 / 0 / 1e-6)。
    FrameStore fs;
    fillKw(fs, 1, 0, 300, kLoudKw);
    fillKw(fs, 1, 300, 340, 1e-4f); // 浅(≈ −40)
    fillKw(fs, 1, 340, 600, kLoudKw);
    fillKw(fs, 1, 600, 640, kSilentKw); // 最深
    fillKw(fs, 1, 640, 900, kLoudKw);
    fillKw(fs, 1, 900, 940, 1e-6f); // 次深(≈ −60)
    fillKw(fs, 1, 940, 1200, kLoudKw);

    const auto all = snapValleysSeconds(fs.channel(1), 0.0, 12.0, kHopS, SegmentationParams{}, 256);
    REQUIRE(all.size() == 3);
    CHECK(all[0] < all[1]);
    CHECK(all[1] < all[2]);

    const auto top2 = snapValleysSeconds(fs.channel(1), 0.0, 12.0, kHopS, SegmentationParams{}, 2);
    REQUIRE(top2.size() == 2);
    CHECK(top2[0] == Approx(all[1]).margin(1e-9)); // 数字静音那个
    CHECK(top2[1] == Approx(all[2]).margin(1e-9)); // −60 那个;−40 那个被挤掉
}

TEST_CASE("[J145] 已覆盖跨度超扫描上限 ⇒ 本块不给谷点(代价与跨度无关)", "[wavevalleys][J145]")
{
    FrameStore fs;
    // 恰好到上限:照常给。
    const std::uint64_t n = scvb::analysis::kSnapValleyMaxScanHops;
    fillKw(fs, 1, 0, n / 2, kLoudKw);
    fillKw(fs, 1, n / 2, n / 2 + 50, kSilentKw);
    fillKw(fs, 1, n / 2 + 50, n, kLoudKw);
    const double endS = static_cast<double>(n) * kHopS;
    CHECK(snapValleysSeconds(fs.channel(1), 0.0, endS, kHopS, SegmentationParams{}, 256).size() == 1);
    // 再多一个 hop:超上限 ⇒ 空。
    fillKw(fs, 1, n, n + 1, kLoudKw);
    CHECK(snapValleysSeconds(fs.channel(1), 0.0, endS + 1.0, kHopS, SegmentationParams{}, 256).empty());
}
