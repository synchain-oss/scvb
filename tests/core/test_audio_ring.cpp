// SPDX-License-Identifier: GPL-3.0-or-later
// test_audio_ring —— AudioRing 写侧单测([J57] 声道三条 + 几何快照纪律,01 §5.1 步骤 4)。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "ipc/AudioRing.h"

using scvb::kScvbAbi;
using scvb::kScvbMagic;
using scvb::u32;

namespace
{
constexpr u32 kRingFrames = 1024; // 2^k 小环,便于断言
constexpr u32 kRingMask = kRingFrames - 1;

struct RingFixture
{
    scvb::AudioRingHeader header;
    std::vector<float> data; // stereo 容量(ring_frames × 2)

    explicit RingFixture(u32 channels) : data(static_cast<std::size_t>(kRingFrames) * 2, 0.0f)
    {
        header.magic.store(kScvbMagic, std::memory_order_release);
        header.abi.store(kScvbAbi, std::memory_order_release);
        header.sample_rate = 48000;
        header.ring_frames = kRingFrames;
        header.channels = channels;
        header.write_head_samples.store(0, std::memory_order_relaxed);
        header.epoch.store(0, std::memory_order_relaxed);
    }
};
} // namespace

TEST_CASE("J57① mono 轨 channels==1 且环内容与输入逐样本一致", "[input][audioring]")
{
    RingFixture f(1);
    scvb::AudioRing ring;
    ring.bind(&f.header, f.data.data());
    REQUIRE(ring.bound());
    REQUIRE(ring.geometry().channels == 1);
    REQUIRE(ring.geometry().ringFrames == kRingFrames);

    const int n = 256;
    std::vector<float> src(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        src[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.003f - 0.4f;
    }
    const int64_t t0 = 100;
    scvb::AudioRing::write(ring.acquire(), t0, src.data(), n);

    for (int i = 0; i < n; ++i)
    {
        REQUIRE(f.data[static_cast<std::size_t>((t0 + i) & kRingMask) * 1] == src[static_cast<std::size_t>(i)]);
    }
    REQUIRE(f.header.write_head_samples.load(std::memory_order_acquire) == static_cast<scvb::u64>(t0 + n));
}

TEST_CASE("J57② stereo 轨 channels==2 interleaved LR 且 L/R 不下混不互换", "[input][audioring]")
{
    RingFixture f(2);
    scvb::AudioRing ring;
    ring.bind(&f.header, f.data.data());
    REQUIRE(ring.bound());
    REQUIRE(ring.geometry().channels == 2);

    // L≠R 的素材,断言 L/R 既未被 (L+R)/2 下混、也未互换。
    const int n = 256;
    std::vector<float> src(static_cast<std::size_t>(n) * 2);
    for (int i = 0; i < n; ++i)
    {
        const float l = static_cast<float>(i) * 0.001f;
        const float r = -0.5f - static_cast<float>(i) * 0.002f;
        src[static_cast<std::size_t>(i) * 2 + 0] = l;
        src[static_cast<std::size_t>(i) * 2 + 1] = r;
    }
    const int64_t t0 = 50;
    scvb::AudioRing::write(ring.acquire(), t0, src.data(), n);

    for (int i = 0; i < n; ++i)
    {
        const float l = src[static_cast<std::size_t>(i) * 2 + 0];
        const float r = src[static_cast<std::size_t>(i) * 2 + 1];
        const std::size_t base = static_cast<std::size_t>((t0 + i) & kRingMask) * 2;
        REQUIRE(f.data[base + 0] == l); // L 未下混、未互换
        REQUIRE(f.data[base + 1] == r); // R 未下混、未互换
    }
    REQUIRE(f.header.write_head_samples.load(std::memory_order_acquire) == static_cast<scvb::u64>(t0 + n));
}

TEST_CASE("J57③ 布局变更 channels 随 prepareToPlay 更新且同次 prepare 恒定", "[input][audioring]")
{
    RingFixture f(1);
    scvb::AudioRing ring;
    ring.bind(&f.header, f.data.data());
    REQUIRE(ring.bound());
    REQUIRE(ring.geometry().channels == 1);

    // 宿主改轨道布局(mono→stereo),prepareToPlay 重建环头并 rebind。
    f.header.channels = 2;
    ring.bind(&f.header, f.data.data());
    REQUIRE(ring.geometry().channels == 2);

    // 同一次 prepare 内恒定:processBlock 只读快照 geometry().channels,不读会变的段头布局。
    // 下面模拟「宿主改布局但尚未 rebind」:段头 channels 被改,快照仍应为旧值。
    f.header.channels = 1;
    const int n = 64;
    std::vector<float> stereo(static_cast<std::size_t>(n) * 2, 1.0f);
    stereo[1] = 2.0f;
    scvb::AudioRing::write(ring.acquire(), 0, stereo.data(), n);
    // 快照 channels==2 → 按 interleaved 写;若误读段头(channels==1)则 L/R 会错位。
    REQUIRE(ring.geometry().channels == 2);
    REQUIRE(f.data[0] == stereo[0]); // L
    REQUIRE(f.data[1] == stereo[1]); // R(未按 1 通道写)
}

TEST_CASE("PR#51 红旗#2 并发发布/读:快照发布与音频线程读不撕裂不悬垂", "[input][audioring]")
{
    RingFixture f(2);
    scvb::AudioRing ring;
    ring.bind(&f.header, f.data.data());
    REQUIRE(ring.bound());

    std::atomic<bool> stop{false};
    std::thread publisher([&ring, &f, &stop] {
        while (!stop.load(std::memory_order_relaxed))
        {
            ring.bind(&f.header, f.data.data()); // 消息线程:反复发布新快照
        }
    });
    std::thread reader([&ring, &stop] {
        std::vector<float> buf(128 * 2, 1.0f);
        while (!stop.load(std::memory_order_relaxed))
        {
            const scvb::AudioRingBinding* b = ring.acquire(); // 音频线程:每块 acquire 一次
            scvb::AudioRing::write(b, 0, buf.data(), 128);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    stop.store(true, std::memory_order_release);
    publisher.join();
    reader.join();

    // 发布线程结束后绑定仍有效(owned_ 保活),几何快照一致。
    const scvb::AudioRingBinding* b = ring.acquire();
    REQUIRE(b != nullptr);
    REQUIRE(b->geo.channels == 2);
    REQUIRE(b->geo.ringFrames == kRingFrames);
}
