// SPDX-License-Identifier: GPL-3.0-or-later
// test_mix_source —— ShmRingMixSource 读环语义(covered/换代/失准)+ MixMath DSP 原语单测。

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdint>
#include <vector>

#include "output/MixMath.h"
#include "output/ShmRingMixSource.h"

using scvb::AudioRingHeader;
using scvb::kScvbAbi;
using scvb::kScvbMagic;
using scvb::u32;
using scvb::u64;
using scvb::output::ShmRingMixSource;

namespace
{
// 建一个已初始化的 2^k 环(heap,非共享内存),返回数据缓冲引用。
struct RingFixture
{
    AudioRingHeader header{};
    std::vector<float> data;

    explicit RingFixture(u32 frames = 64, u32 channels = 2)
    {
        header.magic.store(kScvbMagic, std::memory_order_release);
        header.abi.store(kScvbAbi, std::memory_order_release);
        header.sample_rate = 48000;
        header.ring_frames = frames;
        header.channels = channels;
        header.write_head_samples.store(0, std::memory_order_release);
        header.epoch.store(0, std::memory_order_release);
        data.assign(static_cast<std::size_t>(frames) * channels, 0.0f);
    }
};
} // namespace

TEST_CASE("ShmRingMixSource bind 校验(非法几何拒绝)", "[mix][ring]")
{
    RingFixture f;
    ShmRingMixSource src;

    // channels=3 非法。
    f.header.channels = 3;
    src.bind(&f.header, f.data.data());
    REQUIRE_FALSE(src.bound());

    // ring_frames 非 2^k。
    f.header.channels = 2;
    f.header.ring_frames = 63;
    src.bind(&f.header, f.data.data());
    REQUIRE_FALSE(src.bound());

    // magic 不符。
    f.header.ring_frames = 64;
    f.header.magic.store(0x12345678, std::memory_order_release);
    src.bind(&f.header, f.data.data());
    REQUIRE_FALSE(src.bound());
}

TEST_CASE("ShmRingMixSource stereo interleaved 读取(covered)", "[mix][ring]")
{
    RingFixture f;
    for (u32 i = 0; i < 64; ++i)
    {
        f.data[static_cast<std::size_t>(i) * 2] = static_cast<float>(i);
        f.data[static_cast<std::size_t>(i) * 2 + 1] = static_cast<float>(i * 100);
    }
    f.header.write_head_samples.store(64, std::memory_order_release);

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());
    REQUIRE(src.channels() == 2);
    REQUIRE(src.ringFrames() == 64);

    std::vector<float> out(8 * 2);
    REQUIRE(src.read(0, out.data(), 8));
    for (int i = 0; i < 8; ++i)
    {
        CHECK(out[static_cast<std::size_t>(i) * 2] == static_cast<float>(i));
        CHECK(out[static_cast<std::size_t>(i) * 2 + 1] == static_cast<float>(i * 100));
    }
    CHECK(src.gapCount() == 0);
}

TEST_CASE("ShmRingMixSource 缺口(write_head 落后)→ 失准计数", "[mix][ring]")
{
    RingFixture f;
    f.header.write_head_samples.store(4, std::memory_order_release); // 只写了 4 帧

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());

    std::vector<float> out(8 * 2, 1.0f);
    REQUIRE_FALSE(src.read(0, out.data(), 8)); // 需要 8 帧,只覆盖 4 帧 → 缺口
    CHECK(src.gapCount() == 1);

    // 写头推进覆盖后恢复。
    f.header.write_head_samples.store(8, std::memory_order_release);
    REQUIRE(src.read(0, out.data(), 8));
    CHECK(src.gapCount() == 1); // 恢复读取不再增计数
}

TEST_CASE("ShmRingMixSource epoch 跳变 → 本代数据从此刻起算", "[mix][ring]")
{
    RingFixture f;
    f.header.write_head_samples.store(64, std::memory_order_release);

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());

    std::vector<float> out(8 * 2);
    REQUIRE(src.read(0, out.data(), 8));

    // epoch 跳变后,validFrom = 当前块起点;旧区间(仍被写头覆盖但属旧代)按缺口处理。
    f.header.epoch.fetch_add(1, std::memory_order_release);
    // 写头保持 64:新代区间从 8 起算,[8,16) 被 64 覆盖 → 有效。
    REQUIRE(src.read(8, out.data(), 8));
    CHECK(src.gapCount() == 0);
}

TEST_CASE("MixMath ms_balance 端点语义", "[mix][math]")
{
    SECTION("ms_balance=0 → 矩阵恒等(逐位透传)")
    {
        float l = 0.25f;
        float r = -0.75f;
        scvb::output::applyMsBalance(0.0f, l, r);
        CHECK(l == 0.25f);
        CHECK(r == -0.75f);
    }

    SECTION("ms_balance=-100 → 纯 M(mono)")
    {
        float l = 0.5f;
        float r = -0.5f;
        scvb::output::applyMsBalance(-100.0f, l, r);
        CHECK(l == Catch::Approx(0.0f).margin(1e-6)); // M = (L+R)/2 = 0
        CHECK(r == Catch::Approx(0.0f).margin(1e-6));
    }

    SECTION("ms_balance=+100 → 纯 S(side)")
    {
        float l = 0.5f;
        float r = -0.5f;
        scvb::output::applyMsBalance(100.0f, l, r);
        CHECK(l == Catch::Approx(0.5f).margin(1e-6)); // S = (L-R)/2 = 0.5
        CHECK(r == Catch::Approx(-0.5f).margin(1e-6)); // R' = -S = -0.5
    }
}

TEST_CASE("MixMath mono equal-power pan", "[mix][math]")
{
    float l = 0.0f;
    float r = 0.0f;
    scvb::output::mixMonoSample(1.0f, 0.0f, 0.0f, 100.0f, 1.0f, l, r);
    CHECK(l == Catch::Approx(0.70710678f).margin(1e-5));
    CHECK(r == Catch::Approx(0.70710678f).margin(1e-5));

    float hl = 0.0f;
    float hr = 0.0f;
    scvb::output::mixMonoSample(1.0f, -100.0f, 0.0f, 100.0f, 1.0f, hl, hr); // 硬左
    CHECK(hl == Catch::Approx(1.0f).margin(1e-5));
    CHECK(hr == Catch::Approx(0.0f).margin(1e-5));
}

TEST_CASE("MixMath stereo dual-pan + width", "[mix][math]")
{
    // width=100、pan=0 → L 源 → L、R 源 → R(源宽度原样,不互换)。
    float l = 0.0f;
    float r = 0.0f;
    scvb::output::mixStereoSample(1.0f, 0.5f, 0.0f, 0.0f, 100.0f, 100.0f, 1.0f, l, r);
    CHECK(l == Catch::Approx(1.0f).margin(1e-5)); // 子声像 P_L=-100:gL_L=1、gL_R=0
    CHECK(r == Catch::Approx(0.5f).margin(1e-5)); // 子声像 P_R=+100:gR_L=0、gR_R=1

    // width=0 → 双子声像重合(塌成 mono)。
    float ml = 0.0f;
    float mr = 0.0f;
    scvb::output::mixStereoSample(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 100.0f, 1.0f, ml, mr);
    CHECK(ml == Catch::Approx(0.70710678f).margin(1e-5));
    CHECK(mr == Catch::Approx(0.70710678f).margin(1e-5));
}
