// SPDX-License-Identifier: GPL-3.0-or-later
// test_mix_source —— ShmRingMixSource 读环语义(covered/换代/失准)+ MixMath DSP 原语单测。

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "output/BusXfade.h"
#include "output/MeterShot.h"
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

TEST_CASE("ShmRingMixSource 冷启动(写方尚未追上)不计失准", "[mix][ring]")
{
    // T37 三轮 A 族回归:刚 attach 的空环 / 起播瞬间 / 宿主先渲染 Output 再渲染 Input,
    // 都表现为「write_head 还没覆盖本块」。这不是失准,是**尚未上线** —— 若计数,所有
    // 注入轨会在同一块同时 +1,UI 立刻报「5 轨检测到时间线缺口」(真机症状 L-6)。
    RingFixture f;
    f.header.write_head_samples.store(0, std::memory_order_release); // 写方一帧未写

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());

    std::vector<float> out(8 * 2, 1.0f);
    for (int i = 0; i < 10; ++i)
    {
        REQUIRE_FALSE(src.read(0, out.data(), 8)); // 该块静音直通
    }
    CHECK(src.gapCount() == 0); // 一次都不计

    // 写方追上 → 首次成功读(primed),此后才进入失准判定。
    f.header.write_head_samples.store(8, std::memory_order_release);
    REQUIRE(src.read(0, out.data(), 8));
    CHECK(src.gapCount() == 0);
}

TEST_CASE("ShmRingMixSource 缺口(primed 后 write_head 落后)→ 失准计数", "[mix][ring]")
{
    RingFixture f;
    f.header.write_head_samples.store(8, std::memory_order_release);

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());

    // 先成功读一次:本代确有可读数据,此后 covered 失败才是真缺口。
    std::vector<float> out(8 * 2, 1.0f);
    REQUIRE(src.read(0, out.data(), 8));
    CHECK(src.gapCount() == 0);

    // 写头停滞而读位置前移 → 真缺口(Input 掉出总线 / 停止推进)。
    REQUIRE_FALSE(src.read(8, out.data(), 8));
    CHECK(src.gapCount() == 1);

    // 写头推进覆盖后恢复。
    f.header.write_head_samples.store(16, std::memory_order_release);
    REQUIRE(src.read(8, out.data(), 8));
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

TEST_CASE("ShmRingMixSource bind/unbind 与 read 并发不崩(原子快照)", "[mix][ring][concurrency]")
{
    RingFixture f(64, 2);
    f.header.write_head_samples.store(64, std::memory_order_release);
    for (u32 i = 0; i < 64; ++i)
    {
        f.data[static_cast<std::size_t>(i) * 2] = static_cast<float>(i);
        f.data[static_cast<std::size_t>(i) * 2 + 1] = static_cast<float>(i);
    }

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());

    std::atomic<bool> stop{false};
    std::vector<float> buf(8 * 2);

    std::thread reader([&] {
        int64_t t0 = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            (void)src.read(t0, buf.data(), 8);
            t0 = (t0 + 8) & 63; // 在 64 帧环内循环(covered)
        }
    });

    // 主线程反复 bind/unbind 抖动;reader 并发 read —— 原子快照下不得空指针解引用/撕裂。
    for (int i = 0; i < 2000; ++i)
    {
        src.unbind();
        src.bind(&f.header, f.data.data());
    }

    stop.store(true, std::memory_order_relaxed);
    reader.join();

    // 重新稳定绑定,确认仍可读(无永久损坏)。
    src.unbind();
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());
    std::vector<float> out(8 * 2);
    REQUIRE(src.read(0, out.data(), 8));
    REQUIRE(out[0] == 0.0f);
    REQUIRE(out[1] == 0.0f);
}

TEST_CASE("BusXfade 直通⇄混音等功率交叉(无别名/无 +3dB 泵感)", "[mix][busxfade]")
{
    scvb::output::BusXfade xf;
    xf.prepare(48000.0, 80.0);
    const int n = 512;

    std::vector<float> in(static_cast<std::size_t>(n), 1.0f); // 直通源 = 单位 DC
    std::vector<float> mix(static_cast<std::size_t>(n), 0.0f); // 自有混音 = 静音(模拟最后一混音块)
    std::vector<float> out(static_cast<std::size_t>(n), 0.0f);
    const float* ip[2] = {in.data(), in.data()};
    const float* mp[2] = {mix.data(), mix.data()};
    float* op[2] = {out.data(), out.data()};

    // 1) 稳态直通:theta=0 → render(false) 零开销,不写输出。
    out.assign(static_cast<std::size_t>(n), 7.0f);
    xf.render(op, ip, mp, n, false);
    REQUIRE(xf.isSettledPassthrough());
    REQUIRE(out[0] == 7.0f);

    // 2) 直通→混音:out = in*cos(θ)+mix*sin(θ)=cos(θ)(in=1,mix=0),单调降且恒 ≤1(无 +3dB 泵感)。
    xf.render(op, ip, mp, n, true);
    REQUIRE_FALSE(xf.isSettledMix());
    float prev = 2.0f;
    for (int i = 0; i < n; ++i)
    {
        const float v = out[static_cast<std::size_t>(i)];
        REQUIRE(v <= 1.0f + 1e-6f);
        REQUIRE(v <= prev + 1e-6f); // cos 单调降
        prev = v;
    }

    // 3) 推至稳态混音:余下样本直接拷贝 mix(此处 mix=0)。
    for (int k = 0; k < 20; ++k)
    {
        xf.render(op, ip, mp, n, true);
    }
    REQUIRE(xf.isSettledMix());
    REQUIRE(out[0] == 0.0f); // mix=0 → 稳态输出 0

    // 4) 混音→直通:out = cos(θ)(θ 从 π/2 降),单调升、恒 ≤1;首样本≈mix(无硬切)。
    xf.render(op, ip, mp, n, false);
    REQUIRE_FALSE(xf.isSettledPassthrough());
    prev = -1.0f;
    for (int i = 0; i < n; ++i)
    {
        const float v = out[static_cast<std::size_t>(i)];
        REQUIRE(v <= 1.0f + 1e-6f);
        REQUIRE(v >= prev - 1e-6f); // cos 单调升
        prev = v;
    }
    REQUIRE(out[0] == Catch::Approx(0.0f).margin(1e-4f)); // 首样本≈mix=0,与上一块稳态混音连续(无阶跃)
}

// ---------------------------------------------------------------------------
// T37 三轮 B 族回归:电平快照通道(scvb.meters 的数据面)。
// 真机症状 L-2/L-3:识别出了轨道数,但玻璃管液柱一直最低、电平表不跳、无峰值条。
// 根因是 emitMeters 把 15 轨与总线全部硬编码在 -60dB 地板(T29 占位),叠加 0.3dB 阈值门
// 后该事件首帧发一次就再不发。web 侧一直是好的,缺的是这条从音频线程上来的数据。
// ---------------------------------------------------------------------------

TEST_CASE("MeterShot:seqlock 往返 + 静音发布回地板(T37 三轮 B 族)", "[meters][t37]")
{
    scvb::output::MeterShot shot;

    // 未发布过:读到全零 = 静音。emitMeters 的 toDb(0) 落 -60dB 地板,液柱在底部。
    scvb::output::MeterPod pod{};
    REQUIRE(shot.read(pod));
    CHECK(pod.trackRms[0] == 0.0f);
    CHECK(pod.busPeak[1] == 0.0f);

    // 发布一帧真实电平(线性幅度;dB 换算刻意留在消息线程)。
    scvb::output::MeterPod tx{};
    tx.trackRms[0] = 0.5f;
    tx.trackPeak[0] = 0.75f;
    tx.trackRms[14] = 0.125f;
    tx.busRms[0] = 0.25f;
    tx.busPeak[1] = 1.0f;
    shot.publish(tx);

    scvb::output::MeterPod rx{};
    REQUIRE(shot.read(rx));
    CHECK(rx.trackRms[0] == 0.5f);
    CHECK(rx.trackPeak[0] == 0.75f);
    CHECK(rx.trackRms[14] == 0.125f);
    CHECK(rx.busRms[0] == 0.25f);
    CHECK(rx.busPeak[1] == 1.0f);

    // publishSilentMeters 的语义:发全零 → 电平表落回地板,而不是冻在上一块的值。
    shot.publish(scvb::output::MeterPod{});
    REQUIRE(shot.read(rx));
    CHECK(rx.trackRms[0] == 0.0f);
    CHECK(rx.trackPeak[0] == 0.0f);
    CHECK(rx.busPeak[1] == 0.0f);

    // 写者在临界区内(seq 为奇)时读方必须报撕裂,由调用方沿用上帧而不是拿到半新半旧的值。
    shot.seq.fetch_add(1, std::memory_order_release);
    CHECK_FALSE(shot.read(rx));
    shot.seq.fetch_add(1, std::memory_order_release);
    CHECK(shot.read(rx));
}
