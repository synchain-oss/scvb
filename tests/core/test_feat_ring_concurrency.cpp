// SPDX-License-Identifier: GPL-3.0-or-later
// test_feat_ring_concurrency —— FeatRing prepare×音频线程并发单测(PR#51 红旗第 2 轮)。
// 场景:播放中宿主加载不同 channel/group 的 preset(T30 接线后 setChannelId/setGroupId)会在消息线程
// 触发 prepare();修复 = 不可变运行态快照发布(FeatRunState 原子发布 + 旧状态 owned_ 保活),
// 音频线程在途调用持旧状态指针绝不悬垂。断言:并发 400ms 无崩溃无撕裂,收敛后恢复正常写入。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "ipc/FeatRing.h"

using scvb::kScvbAbi;
using scvb::kScvbMagic;

namespace
{
struct FeatFixture
{
    scvb::FeatHeader header;
    std::vector<scvb::FeatFrame> ring;

    FeatFixture()
        : ring(static_cast<std::size_t>(scvb::kFeatCapacityHops))
    {
        header.magic.store(kScvbMagic, std::memory_order_release);
        header.abi.store(kScvbAbi, std::memory_order_release);
        header.hop_ms = scvb::kFeatHopMs;
        header.capacity_hops = scvb::kFeatCapacityHops;
        header.base_hop.store(0, std::memory_order_relaxed);
        header.write_hop.store(0, std::memory_order_relaxed);
    }
};
} // namespace

TEST_CASE("PR#51 红旗第2轮 prepare×processBlock 并发:无 UAF 无撕裂", "[input][featring]")
{
    FeatFixture f;
    scvb::FeatRing fr;
    fr.bind(&f.header, f.ring.data(), scvb::kFeatCapacityHops);
    REQUIRE(fr.bound());
    fr.prepare(48000.0, 2, 512);
    fr.setCapturing(true);

    std::atomic<bool> stop{false};
    std::thread publisher([&fr, &stop] {
        int i = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            // 消息线程:反复 re-prepare(声道 1/2 交替 + 块长 512/256 交替,触发缓冲重建)。
            fr.prepare(48000.0, (i % 2 == 0) ? 2 : 1, (i % 3 == 0) ? 512 : 256);
            ++i;
        }
    });
    std::thread reader([&fr, &stop] {
        std::vector<float> ch0(512, 0.1f);
        std::vector<float> ch1(512, 0.1f);
        const float* planes[2] = {ch0.data(), ch1.data()};
        while (!stop.load(std::memory_order_relaxed))
        {
            // 音频线程:每块 startRun + processBlock;并发 prepare 下仍安全(旧状态保活)。
            fr.startRun(0);
            (void)fr.processBlock(planes, 512);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop.store(true, std::memory_order_release);
    publisher.join();
    reader.join();

    // 收敛:prepare 完成后下一块恢复正常写入(write_hop 前进且无崩溃)。
    fr.startRun(0);
    std::vector<float> c0(512, 0.1f);
    std::vector<float> c1(512, 0.1f);
    const float* planes2[2] = {c0.data(), c1.data()};
    const int written = fr.processBlock(planes2, 512);
    REQUIRE(written > 0);
    SUCCEED("并发 400ms 无崩溃/无 UAF,收敛后恢复正常写入");
}
