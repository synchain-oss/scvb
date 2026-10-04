// SPDX-License-Identifier: GPL-3.0-or-later
// test_mix_source_provenance —— 读环「出处」判据的落点:读方交出来的每一块,内容必须来自
// 时间线上的同一位置(不是上一圈留下的旧数据,也不是换代前的残留)。
//
// A 线 A-3 在这里展开读环 oracle 模糊测试与旧读方差分基线。A-1 只做脚手架:文件已登记进
// scvb_tests,放一条最小用例 —— 在**生产环长**(kDefaultRingFrames)的环上写一块 lane 0 的
// 在场判据信号,经真 ShmRingMixSource::read 读回,在场判据判 Present。这条同时证明:
//   · 在场判据的 R 与生产环长是同一个数(静态断言);
//   · 判据能吃真读方交出来的数据(interleaved stereo → L/R)。
// 环的建法沿用 test_mix_source.cpp 的 RingFixture 写法(heap 上的环,非共享内存)。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "output/ShmRingMixSource.h"
#include "support/presence_meter.h"

namespace
{

namespace presence = scvb::testsupport::presence;

static_assert(presence::kRingR == static_cast<std::int64_t>(scvb::kDefaultRingFrames),
              "presence meter R must be the production ring length");

// 与 test_mix_source.cpp 的 RingFixture 同形,只是默认帧数取生产环长。
struct RingFixture
{
    scvb::AudioRingHeader header{};
    std::vector<float> data;

    explicit RingFixture(scvb::u32 frames = scvb::kDefaultRingFrames, scvb::u32 channels = 2)
    {
        header.magic.store(scvb::kScvbMagic, std::memory_order_release);
        header.abi.store(scvb::kScvbAbi, std::memory_order_release);
        header.sample_rate = 48000;
        header.ring_frames = frames;
        header.channels = channels;
        header.write_head_samples.store(0, std::memory_order_release);
        header.epoch.store(0, std::memory_order_release);
        data.assign(static_cast<std::size_t>(frames) * channels, 0.0f);
    }
};

} // namespace

TEST_CASE("PROVENANCE scaffold: a covered read at the production ring length returns the tone written for that "
          "timeline position",
          "[mix][provenance]")
{
    RingFixture f;
    const std::int64_t mask = presence::kRingR - 1;
    // 已经绕过环 3 圈的位置:环槽 = t & mask。
    const std::int64_t t0 = 3 * presence::kRingR + 5 * static_cast<std::int64_t>(presence::kFrame);

    std::vector<float> mono(static_cast<std::size_t>(presence::kFrame), 0.0f);
    presence::addLane(0, t0, mono.data(), presence::kFrame);
    for (int i = 0; i < presence::kFrame; ++i)
    {
        const std::size_t slot = static_cast<std::size_t>((t0 + i) & mask);
        f.data[slot * 2] = mono[static_cast<std::size_t>(i)];
        f.data[slot * 2 + 1] = mono[static_cast<std::size_t>(i)];
    }
    f.header.write_head_samples.store(static_cast<scvb::u64>(t0 + presence::kFrame), std::memory_order_release);

    scvb::output::ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());
    REQUIRE(src.ringFrames() == scvb::kDefaultRingFrames);

    std::vector<float> interleaved(static_cast<std::size_t>(presence::kFrame) * 2, 0.0f);
    REQUIRE(src.read(t0, interleaved.data(), presence::kFrame));

    std::vector<float> left(static_cast<std::size_t>(presence::kFrame));
    std::vector<float> right(static_cast<std::size_t>(presence::kFrame));
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        left[i] = interleaved[i * 2];
        right[i] = interleaved[i * 2 + 1];
    }
    const presence::CombReport rep =
        presence::analyze(presence::Span{left.data(), right.data(), t0, presence::kFrame}, {0});
    REQUIRE(rep.lanes.size() == 1u);
    REQUIRE(rep.frameStarts.size() == 1u);
    CHECK(rep.lanes[0].count(presence::Verdict::Present) == 1);
    CHECK(rep.lanes[0].frames[0].rho >= 0.999);
}
