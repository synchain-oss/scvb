// SPDX-License-Identifier: GPL-3.0-or-later
// test_mix_source —— ShmRingMixSource 读环语义(covered/换代/失准)+ MixMath DSP 原语单测。

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
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

// [SL-442] 「没有 pan 曲线」的 G 视图:两个表都为 null ⇒ panCurveGainDb 恒回 0 dB。
// 既有这四条断言的期望值一个都没改 —— 那正是「老工程零影响」的直接证据。
static const scvb::PanCurveXfade kNoCurve{};
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

TEST_CASE("ShmRingMixSource 写头停滞 → 记饿读,不记失准(v5 P1-7)", "[mix][ring]")
{
    RingFixture f;
    f.header.write_head_samples.store(8, std::memory_order_release);

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());

    // 先成功读一次:本代确有可读数据,此后 covered 失败才需要分类。
    std::vector<float> out(8 * 2, 1.0f);
    REQUIRE(src.read(0, out.data(), 8));
    CHECK(src.gapCount() == 0);
    CHECK(src.stallFailCount() == 0);

    // 写头停滞而读位置前移 = **写方没在写**(bypass / 宿主在静音段跳过该轨)。
    // 这归 OutputSession 的 CH_SUSPENDED 判定管,**不是失准** —— 老实现在这里记缺口,
    // 于是每个静音边界都闪一次「失准」再自愈(v5 实测 P1-7)。
    REQUIRE_FALSE(src.read(8, out.data(), 8));
    CHECK(src.gapCount() == 0);
    CHECK(src.stallFailCount() == 1);

    // 停滞持续:饿读继续留痕,失准仍然为 0。
    REQUIRE_FALSE(src.read(16, out.data(), 8));
    CHECK(src.gapCount() == 0);
    CHECK(src.stallFailCount() == 2);

    // 写头推进覆盖后恢复。
    f.header.write_head_samples.store(24, std::memory_order_release);
    REQUIRE(src.read(8, out.data(), 8));
    CHECK(src.gapCount() == 0);
}

TEST_CASE("ShmRingMixSource 写方套圈 → 真失准计数", "[mix][ring]")
{
    RingFixture f;
    f.header.write_head_samples.store(8, std::memory_order_release);

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());

    std::vector<float> out(8 * 2, 1.0f);
    REQUIRE(src.read(0, out.data(), 8));
    REQUIRE(src.gapCount() == 0);

    // 写头远远越过读位置(超一整个环距)= 要读的数据已被覆盖 —— 这才是真失准,照计。
    // 与上一条用例的对照点:两者 covered 都失败,但物理成因相反,不能混判。
    f.header.write_head_samples.store(static_cast<scvb::u64>(f.header.ring_frames) * 4 + 8, std::memory_order_release);
    REQUIRE_FALSE(src.read(8, out.data(), 8));
    CHECK(src.gapCount() == 1);
}

TEST_CASE("ShmRingMixSource epoch 跳变 → 本代数据从此刻起算", "[mix][ring]")
{
    RingFixture f;
    f.header.write_head_samples.store(64, std::memory_order_release);

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());

    std::vector<float> out(8 * 2);
    REQUIRE(src.read(0, out.data(), 8));

    // epoch 跳变、写头保持 64:[8,16) 是上一代已确认写过的区间(写头 64 之下、没被套圈),照读。
    // [A-5] 读法换了:A-5 之前是「本代有效起点 = 当前块起点」,现在是「上一代窗」(设计稿 §3.3 I3)——
    // 读方自己没跳、写头也还没在新一代里动过,本代从哪开始写无从确认,当前窗先不锚;
    // 上一代 [0, 64) 里的数据仍是这些位置的数据,换代不影响它可读。
    f.header.epoch.fetch_add(1, std::memory_order_release);
    REQUIRE(src.read(8, out.data(), 8));
    CHECK(src.gapCount() == 0);
}

// ---------------------------------------------------------------------------
// [A-5] 读方换代处理的竞态单测(设计稿 §3.3 规则 3 / 5)。环里每帧写的是它自己的时间线位置
// (两个声道都写),读出来逐帧核对:返回 true 的块必须每一帧都是本位置(I1)。读方代号一律显式给,
// 1 = 读方时间线没跳过。
// ---------------------------------------------------------------------------

namespace
{
// 往环里写 [p0, p0+n) 并发布写头 = p0+n(AudioRing::write 的顺序:先数据、后写头)。
void writePositions(RingFixture& f, int64_t p0, int n)
{
    const u32 mask = f.header.ring_frames - 1;
    const u32 ch = f.header.channels;
    for (int i = 0; i < n; ++i)
    {
        const u32 slot = static_cast<u32>(static_cast<u64>(p0 + i)) & mask;
        for (u32 c = 0; c < ch; ++c)
        {
            f.data[static_cast<std::size_t>(slot) * ch + c] = static_cast<float>(p0 + i);
        }
    }
    f.header.write_head_samples.store(static_cast<u64>(p0 + n), std::memory_order_release);
}

// 只写数据、不发布写头:AudioRing::write「先写数据、后发布写头」之间那一刻(写方的一笔还在途)。
void writeUnpublished(RingFixture& f, int64_t p0, int n)
{
    const u32 mask = f.header.ring_frames - 1;
    const u32 ch = f.header.channels;
    for (int i = 0; i < n; ++i)
    {
        const u32 slot = static_cast<u32>(static_cast<u64>(p0 + i)) & mask;
        for (u32 c = 0; c < ch; ++c)
        {
            f.data[static_cast<std::size_t>(slot) * ch + c] = static_cast<float>(p0 + i);
        }
    }
}

// 读出来的每一帧(两个声道)都等于它的时间线位置。
bool holdsPositions(const std::vector<float>& out, int64_t t0, int n)
{
    for (int i = 0; i < n; ++i)
    {
        if (out[static_cast<std::size_t>(i) * 2] != static_cast<float>(t0 + i) ||
            out[static_cast<std::size_t>(i) * 2 + 1] != static_cast<float>(t0 + i))
        {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("ShmRingMixSource A-5 换代后写头未动:上一代窗照读,写方在前方另起一代时不把中间的旧环槽当本代数据",
          "[mix][ring][a5]")
{
    // H4 的读方层面最小形状:写方换代(epoch+1)之后写头先不动,随后写方在读方前方很远处
    // (从停调恢复 / 往前定位)发布新一代的第一段。读方自己没跳。
    RingFixture f(4096, 2);
    f.header.epoch.store(1, std::memory_order_release);
    writePositions(f, 1000, 512); // 上一代 [1000, 1512)

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());
    std::vector<float> out(64 * 2, -1.0f);
    REQUIRE(src.read(1000, out.data(), 64, 1));
    CHECK(holdsPositions(out, 1000, 64));
    REQUIRE(src.read(1064, out.data(), 64, 1));

    // ① 换代、写头未动(1512):上一代已确认写过的区间照读。
    f.header.epoch.fetch_add(1, std::memory_order_release);
    REQUIRE(src.read(1128, out.data(), 64, 1));
    CHECK(holdsPositions(out, 1128, 64));

    // ② 新一代的第一段写在前方 2048 处(写头一动 ⇒ 确认是新一代的头)。上一代窗里的位置照读 ——
    //    新一代 [2048, 2112) 与它们不同槽。
    writePositions(f, 2048, 64);
    for (int64_t t0 = 1192; t0 < 1512; t0 += 64)
    {
        INFO("t0=" << t0);
        REQUIRE(src.read(t0, out.data(), 64, 1));
        CHECK(holdsPositions(out, t0, 64));
    }

    // ③ 读方走到上一代写头 1512:[1512, 2048) 两代都没写过,环槽里是更早的内容。A-5 之前的实现在这里
    //    把有效起点放在换代那一块(1128)、又看写头 2112 已覆盖,于是把它当本代数据交出去(H4)。
    const u32 handover0 = src.handoverLossCount();
    CHECK_FALSE(src.read(1512, out.data(), 64, 1));
    CHECK_FALSE(src.read(1576, out.data(), 64, 1));
    CHECK(src.handoverLossCount() > handover0); // 交接期读不到:记 handoverLoss(只计数,A-6 才告警)
    CHECK(src.gapCount() == 0); // 本代还没在当前窗里读到过数据:不算失准

    // ④ 读方追上新一代确认过的写头之后照常读(保守规则:锚在确认过的写头上)。
    writePositions(f, 2112, 128);
    REQUIRE(src.read(2112, out.data(), 64, 1));
    CHECK(holdsPositions(out, 2112, 64));
    REQUIRE(src.read(2176, out.data(), 64, 1));
    CHECK(holdsPositions(out, 2176, 64));
}

TEST_CASE("ShmRingMixSource A-5 同一代内写头回退(停走带静止重写换短块):照读,但换代后不拿上一代的高头认新头",
          "[mix][ring][a5]")
{
    // 写方协议里同一代写头回退只有一个来源:走带停着、宿主每块给同一个 t0 时写方不换代,照样写
    // [t0, t0+n) 并发布 t0+n —— 这一块比上一块短,写头就往回退(A-3 同步族实测到)。
    // 设计稿规则 5「其余回退」:① 写过的位置仍是这些位置的数据,读方照读(I4,旧实现也照读);
    // ② 但这一代的写头不再单调:换代之后、新一代第一段发布之前,读方看到的旧头可能比它上一次观测到的
    //    更低 —— 规则 3 保守规则前半「新写头低于上一代最后观测到的头 ⇒ 一定是新一代写的」不再成立。
    RingFixture f(4096, 2);
    f.header.epoch.store(1, std::memory_order_release);
    writePositions(f, 1000, 512); // [1000, 1512)

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    std::vector<float> out(64 * 2, -1.0f);
    REQUIRE(src.read(1000, out.data(), 64, 1));
    writePositions(f, 1512, 64); // 写头往前动一次(1576):本代的头确认了
    REQUIRE(src.read(1064, out.data(), 64, 1));

    // 走带停在 1576(写方的下一个位置,连续 ⇒ 不换代):静止重写 [1576, 1640),再来一块更短的 [1576, 1608)。
    writePositions(f, 1576, 64);
    REQUIRE(src.read(1128, out.data(), 64, 1));
    writePositions(f, 1576, 32); // 写头 1640 → 1608:同一代里回退
    // ① 回退之后照读(A-5 初版在这里放弃了锚点,A-3 同步族差分当场红:旧实现读得到的块新实现读不到)。
    REQUIRE(src.read(1192, out.data(), 64, 1));
    CHECK(holdsPositions(out, 1192, 64));

    // 停着的时候往前定位到 3000:写方换代,读方看到「新一代、写头还是旧的 1608」(新一代第一段还没发布)。
    // 读方自己没跳(它要的位置照常往前走)。
    f.header.epoch.fetch_add(1, std::memory_order_release);
    REQUIRE(src.read(1256, out.data(), 64, 1)); // 上一代窗照读
    CHECK(holdsPositions(out, 1256, 64));
    writePositions(f, 3000, 64); // 新一代的第一段在 [3000, 3064)

    // 上一代 [1000, 1640) 照读到底(CHECK 不 REQUIRE:下面 ② 那条要在这几块读不到时照样判)。
    for (int64_t t0 = 1320; t0 < 1640; t0 += 64)
    {
        INFO("t0=" << t0);
        CHECK(src.read(t0, out.data(), 64, 1));
        CHECK(holdsPositions(out, t0, 64));
    }
    // ② [1640, 3000) 两代都没写过。旧头 1608 若被当成新一代的头(1608 < 上一代观测到的最高头 1640),
    //    当前窗就锚在 1608、新一代写头 3064 又「覆盖」到这里,读方会把从没写过的环槽交出去。
    CHECK_FALSE(src.read(1640, out.data(), 64, 1));
    CHECK_FALSE(src.read(1704, out.data(), 64, 1));
    CHECK(src.gapCount() == 0);
}

TEST_CASE("ShmRingMixSource A-5 写头归 0(几何改写)清空所有窗:旧布局的数据换代后不再读", "[mix][ring][a5]")
{
    // 几何改写(rebuildAudioGeometry / attach 到几何不符的旧段)的顺序是 写几何 → 写头归 0 → epoch+1 →
    // 写方按新几何写。声道数没变(只改采样率)时段头 channels 比对拦不住,靠「w=0 清窗」(设计稿规则 5)。
    RingFixture f(4096, 2);
    f.header.epoch.store(1, std::memory_order_release);
    writePositions(f, 1000, 512);

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    std::vector<float> out(64 * 2, -1.0f);
    REQUIRE(src.read(1000, out.data(), 64, 1));
    REQUIRE(src.read(1064, out.data(), 64, 1));

    SECTION("读方先看到同一代里写头归 0,再看到换代")
    {
        f.header.write_head_samples.store(0, std::memory_order_release);
        CHECK_FALSE(src.read(1128, out.data(), 64, 1));
        f.header.epoch.fetch_add(1, std::memory_order_release);
    }
    SECTION("归 0 与换代都在读方两次观测之间发生(读方只看到「新一代、写头 0」)")
    {
        f.header.write_head_samples.store(0, std::memory_order_release);
        f.header.epoch.fetch_add(1, std::memory_order_release);
        CHECK_FALSE(src.read(1128, out.data(), 64, 1));
    }
    // 新几何下的第一段写在别处。上一代 [1000, 1512) 是按旧几何写的,不能再当上一代窗读 ——
    // A-5 之前的实现在这里把有效起点放在本块、又看写头已覆盖,照读不误。
    writePositions(f, 3000, 64);
    CHECK_FALSE(src.read(1192, out.data(), 64, 1));
    CHECK_FALSE(src.read(1256, out.data(), 64, 1));
    CHECK(src.gapCount() == 0);
    CHECK(src.readOkCount() == 2u);
}

TEST_CASE("ShmRingMixSource A-5 推迟锚定:写方先单独循环回绕,读方随后自己跳到同一个循环起点,从落点起读",
          "[mix][ring][a5]")
{
    // H1 的读方层面最小形状(设计稿规则 4):循环 [512, 3072),写方在同一首歌上领先读方 1024 帧,每块 64 帧,
    // 写方先到循环终点、先回绕换代;读方还在上一圈尾段(上一代窗接住),1024 帧之后才自己跳回 512。
    // 读方第一次看到新一代时锚在确认过的写头上(保守规则,那时写方才写到 576);读方随后自己跳到 512 时,
    // 推迟锚定把锚点放回落点 —— 否则 [512, 576) 这一块写方明明写过、读方却读不到(每圈丢一块)。
    constexpr int64_t kB = 512;
    constexpr int64_t kE = 3072;
    constexpr int64_t kLead = 1024;
    RingFixture f(4096, 2);
    f.header.epoch.store(1, std::memory_order_release);
    writePositions(f, 1024, 1984); // 写方领先:读方在 1984 时写头已到 3008

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    std::vector<float> out(64 * 2, -1.0f);
    REQUIRE(src.read(1984, out.data(), 64, 1));
    writePositions(f, 3008, 64); // 写方写到循环终点 3072
    REQUIRE(src.read(2048, out.data(), 64, 1));

    // 读方读上一圈尾段 [2112, 3072);每读一块之前,写方按「读方位置 + 1024」写到新一圈。
    f.header.epoch.fetch_add(1, std::memory_order_release); // 写方回绕:换代
    int64_t wpos = kB;
    for (int64_t t0 = 2112; t0 < kE; t0 += 64)
    {
        const int64_t want = kB + (t0 + kLead - kE); // 写头此刻应在的新一圈位置(= 读方块首 + 1024)
        while (wpos < want)
        {
            writePositions(f, wpos, 64);
            wpos += 64;
        }
        INFO("old lap t0=" << t0);
        CHECK(src.read(t0, out.data(), 64, 1));
        CHECK(holdsPositions(out, t0, 64));
    }

    // 读方自己回绕到 512(读方代号 1 → 2):写方此刻写到 1536。
    writePositions(f, wpos, 64);
    wpos += 64;
    REQUIRE(wpos == kB + kLead);
    CHECK(src.read(kB, out.data(), 64, 2));
    CHECK(holdsPositions(out, kB, 64));
    for (int64_t t0 = kB + 64; t0 < kB + 512; t0 += 64)
    {
        writePositions(f, wpos, 64);
        wpos += 64;
        INFO("new lap t0=" << t0);
        CHECK(src.read(t0, out.data(), 64, 2));
        CHECK(holdsPositions(out, t0, 64));
    }
    CHECK(src.gapCount() == 0);
}

TEST_CASE("ShmRingMixSource A-5 在途保护:写方领先接近一个环长时,不读会被它在途那一笔套圈的槽", "[mix][ring][a5]")
{
    // 写方协议:先写数据、后发布写头。写头 w 之后在途的那一笔会写到 [w, w+步长) 的同槽,也就是 [w-R, w-R+步长)。
    // 旧实现只看 `w - t0 <= R` 并在拷贝之后复核写头,在途那一笔还没发布写头时两道都拦不住。A-5 的当前窗下沿
    // 让开一个写方步长(实测的最大写头前进量)。
    RingFixture f(4096, 2);
    f.header.epoch.store(1, std::memory_order_release);
    writePositions(f, 1000, 4090); // 读方在 1000 时写头已到 5090:领先 4090 帧(R = 4096)

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    std::vector<float> out(64 * 2, -1.0f);
    REQUIRE(src.read(1000, out.data(), 64, 1));
    CHECK(holdsPositions(out, 1000, 64));
    writePositions(f, 5090, 64);
    REQUIRE(src.read(1064, out.data(), 64, 1));
    CHECK(holdsPositions(out, 1064, 64));
    writePositions(f, 5154, 64); // 写头 5218;写方步长 64 从这一块起被量到
    writeUnpublished(f, 5218, 64); // 在途:[5218, 5282) 的数据已写进 [1122, 1186) 这些槽,写头还没发布

    // [1128, 1192) 有 58 帧已被在途那一笔换成 5224.. 的数据。写头 5218 - 1128 = 4090 <= R,旧实现照读。
    CHECK_FALSE(src.read(1128, out.data(), 64, 1));
    CHECK(src.gapCount() == 1); // 判到的就是套圈(写方在推进),照旧计真失准
}

TEST_CASE("ShmRingMixSource A-5 换代后先看到旧写头、随后新一代往回发布:本代最大写头不把上一代的旧头算进来",
          "[mix][ring][a5]")
{
    // 并发宿主上读方可能落在「epoch 已 +1、新一代第一段还没发布」那一刻(设计稿残余风险 R1),这时它看到的
    // 写头还是上一代的(6064)。新一代第一段随后发布在更低处(2000)。本代的最大写头 curHi_ 若把那个旧头也算进来,
    // 下一次换代时上一代窗就会按 6064 开到 [2000, 6064) —— 可其中 [5096, 6064) 的槽早被新一代 [1000, 2000) 换掉了。
    RingFixture f(4096, 2);
    f.header.epoch.store(1, std::memory_order_release);
    writePositions(f, 2000, 4000); // [2000, 6000)

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    std::vector<float> out(64 * 2, -1.0f);
    REQUIRE(src.read(5000, out.data(), 64, 1));
    writePositions(f, 6000, 64); // 写头 6064:本代的头确认了
    REQUIRE(src.read(5064, out.data(), 64, 1));

    // 写方往回跳到 1000:换代,第一段还没发布(读方看到新一代、写头仍是 6064)。上一代窗照读。
    f.header.epoch.fetch_add(1, std::memory_order_release);
    REQUIRE(src.read(5128, out.data(), 64, 1));
    CHECK(holdsPositions(out, 5128, 64));

    // 新一代第一段 [1000, 2000) 发布:与上一代窗 [5096, 6064) 同槽,上一代窗从这里起读不到了(别名判据)。
    writePositions(f, 1000, 1000);
    CHECK_FALSE(src.read(5192, out.data(), 64, 1));

    // 写方又往前跳到 3000(再换一代)并发布第一段。上一代(新一代 [1000, 2000))的最大写头是 2000 ——
    // 若按 6064 降级,[5256, 5320) 会被当成上一代写过的位置交出去,而那些槽里是 1160.. 的数据。
    f.header.epoch.fetch_add(1, std::memory_order_release);
    writePositions(f, 3000, 64);
    CHECK_FALSE(src.read(5256, out.data(), 64, 1));
    CHECK(src.gapCount() == 0);
}

TEST_CASE("ShmRingMixSource A-5 「远超一步」只是暂定:换代后看到的高写头其实是上一代的旧头时,写头一动就撤锚",
          "[mix][ring][a5]")
{
    // 设计稿残余风险 R1 / 头注 R5。上一代每块写 64 帧(读方量到的最大步长 64);读方最后一次观测之后,上一代又
    // 多写了一大笔 [1640, 2040)(读方没看到),随后写方单独往前跳、换代,新一代第一段还没发布 —— 读方看到
    // 「新一代、写头 2040」:比上一代最后观测到的头 1640 多走了 400 > 64,「远超一步」会把它当成新一代的头。
    // 它其实是上一代的旧头;新一代从 3000 开始写。锚若就此下在 2040 且不复核,[2040, 3000) 这些从没写过的槽
    // 会在新一代写头越过它们之后被当成本代数据交出去。
    RingFixture f(4096, 2);
    f.header.epoch.store(1, std::memory_order_release);
    writePositions(f, 1000, 512); // 写方领先 512:读方在 1000 时写头已到 1512

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    std::vector<float> out(64 * 2, -1.0f);
    REQUIRE(src.read(1000, out.data(), 64, 1));
    writePositions(f, 1512, 64);
    REQUIRE(src.read(1064, out.data(), 64, 1));
    writePositions(f, 1576, 64); // 写头 1640;上一代步长 64 从这一块起量到
    REQUIRE(src.read(1128, out.data(), 64, 1));

    writePositions(f, 1640, 400); // 读方没看到的一大笔尾段:写头 2040
    f.header.epoch.fetch_add(1, std::memory_order_release); // 写方单独往前跳:换代,新一代第一段还没发布
    REQUIRE(src.read(1192, out.data(), 64, 1)); // 上一代窗照读
    CHECK(holdsPositions(out, 1192, 64));

    writePositions(f, 3000, 64); // 新一代第一段:写头 2040 → 3064,一步 1024,远超 spanBound(576)⇒ 撤锚
    for (int64_t t0 = 1256; t0 < 1640; t0 += 64)
    {
        INFO("t0=" << t0);
        CHECK(src.read(t0, out.data(), 64, 1));
        CHECK(holdsPositions(out, t0, 64));
    }
    // [1640, 2040) 是上一代读方没观测到的尾段(数据其实是对的,只是读方证明不了),读不到不算错。
    for (int64_t t0 = 1640; t0 < 2040; t0 += 64)
    {
        static_cast<void>(src.read(t0, out.data(), 64, 1));
    }
    // [2040, 3000) 两代都没写过。锚若还在 2040,当前窗 [2040, 3064) 会把它们交出去。
    CHECK_FALSE(src.read(2040, out.data(), 64, 1));
    CHECK_FALSE(src.read(2104, out.data(), 64, 1));
    CHECK(src.gapCount() == 0);
}

// ---------------------------------------------------------------------------
// [A-13] 换代后第一次看到的写头**正好**比上一代最后观测到的头多一个步长(LS-16 在读方这一层的形状)。
// 读方只有 epoch 与写头两个数,同一个读数有好几种成因,分不出来:
//   ① 原地重算:宿主播放中把预取轨已算好的音频作废、从 Output 游标重新渲染到「游标 + 提前量 + 块长」——
//      写方单独往回跳了一个提前量、换代,新一代的头恰好比上一代最后观测到的头多一个写方块(LS-16);
//   ② 位置连续的换代:写方换代(例:重新 prepare 之后第一块)但位置接着上一代写,新一代的头 = 上一代的头 + 一块;
//   ③ R1 并发窗口:上一代在读方最后一次观测之后恰好又写了一块,随后换代,新一代第一段还没发布 —— 读方看到的
//      「多一步」的头其实是**上一代的旧头**,新一代往哪写都有可能(往回、原地接着、往前跳);
//   ④ 跨代(两次观测之间换了不止一代)/ 别名(新一代第一段写到读方还要读的上一代窗的同槽)。
// 判据与 A-3 oracle 同口径:读方返回 true 的每一帧都必须是本位置写下的数据(错帧 = 0),哪种成因都一样。
// 台子:环 2^17(装得下提前量 48000 的跨度),读写块长都是 1024(LS-16 的预取块长);写方领先 L,每个读方块之前
// 把写头推到「读方块首 + L + 块长」(A-3 预取条件同形),读方自己从不跳(读方代号恒 1)。
// ---------------------------------------------------------------------------

namespace
{
constexpr u32 kA13Ring = 1u << 17;
constexpr int kA13Block = 1024;

struct LeadRig
{
    RingFixture f;
    ShmRingMixSource src;
    std::vector<float> out;
    int64_t lead = 0;
    int n = kA13Block;
    int64_t t = 0; // 读方下一块的 t0
    int64_t wPos = 0; // 写方下一笔的位置
    int64_t seenHead = 0; // 读方最近一块读时的写头(测试侧记账,前提断言用)
    u64 seenEpoch = 0;
    int64_t wrongFrames = 0; // 返回 true 的块里不是本位置数据的帧(I1 违例)
    int64_t lostBlocks = 0; // 环里整块都是本位置的数据、读方却没读的块
    int64_t hazardBlocks = 0; // 写头已越过块尾、环里却不全是本位置数据的块(读方若认就是错音)
    std::string firstWrong;

    LeadRig(int64_t leadIn, int64_t t0)
        : f(kA13Ring, 2), out(static_cast<std::size_t>(kA13Block) * 2), lead(leadIn), t(t0), wPos(t0)
    {
        f.header.epoch.store(1, std::memory_order_release);
        src.bind(&f.header, f.data.data());
    }

    int64_t head() const { return static_cast<int64_t>(f.header.write_head_samples.load(std::memory_order_acquire)); }
    void bump() { f.header.epoch.fetch_add(1, std::memory_order_release); }
    // 写方本代接着写到 target(一块一块写,每块发布一次写头)。已经在 target 之上就不写(领先够了先停)。
    void writeTo(int64_t target)
    {
        while (wPos < target)
        {
            writePositions(f, wPos, n);
            wPos += n;
        }
    }
    // 写方换代之后的第一段:从 p 起写 m 帧并发布(此后接着从 p + m 往后写)。
    void jumpWrite(int64_t p, int m)
    {
        writePositions(f, p, m);
        wPos = p + m;
    }
    bool ringHolds(int64_t t0) const
    {
        const u32 mask = kA13Ring - 1;
        for (int i = 0; i < n; ++i)
        {
            const std::size_t slot = static_cast<std::size_t>(static_cast<u32>(static_cast<u64>(t0 + i)) & mask);
            if (f.data[slot * 2] != static_cast<float>(t0 + i) || f.data[slot * 2 + 1] != static_cast<float>(t0 + i))
            {
                return false;
            }
        }
        return true;
    }
    bool read()
    {
        const bool avail = ringHolds(t);
        seenHead = head();
        seenEpoch = f.header.epoch.load(std::memory_order_acquire);
        if (!avail && seenHead >= t + n)
        {
            ++hazardBlocks;
        }
        std::fill(out.begin(), out.end(), -1.0f);
        const bool ok = src.read(t, out.data(), n, 1);
        if (ok)
        {
            for (int i = 0; i < n; ++i)
            {
                if (out[static_cast<std::size_t>(i) * 2] != static_cast<float>(t + i) ||
                    out[static_cast<std::size_t>(i) * 2 + 1] != static_cast<float>(t + i))
                {
                    if (firstWrong.empty())
                    {
                        firstWrong = "t0=" + std::to_string(t) + " frame=" + std::to_string(i) +
                                     " got=" + std::to_string(out[static_cast<std::size_t>(i) * 2]);
                    }
                    ++wrongFrames;
                }
            }
        }
        else if (avail)
        {
            ++lostBlocks;
        }
        t += n;
        return ok;
    }
    // 稳态一块:写方先推到「读方块首 + L + 块长」,读方再读。
    void steady()
    {
        writeTo(t + lead + n);
        read();
    }
    void steadyFor(int blocks)
    {
        for (int k = 0; k < blocks; ++k)
        {
            steady();
        }
    }
    // 事件之后放够:读方越过新一代写头与提前量之后再多读几块。
    void runOut() { steadyFor(static_cast<int>(lead / n) + 8); }
};
} // namespace

TEST_CASE("ShmRingMixSource A-13 写头正好多一步(1)(2):原地重算 / 位置连续的换代都不错音,读方至多丢一个写方块",
          "[mix][ring][a13]")
{
    for (const int64_t lead : {int64_t{4096}, int64_t{48000}})
    {
        for (const bool rerender : {true, false})
        {
            INFO("lead=" << lead << (rerender ? " in-place re-render" : " contiguous new generation"));
            LeadRig r(lead, 200000);
            r.steadyFor(8);
            REQUIRE(r.lostBlocks == 0);
            REQUIRE(r.wrongFrames == 0);
            const int64_t refHi = r.seenHead; // 上一代最后观测到的头
            const int64_t lost0 = r.lostBlocks;
            r.bump();
            if (rerender)
            {
                r.wPos = r.t; // ① 从读方游标起重新渲染(一块一块)
            }
            r.writeTo(r.t + lead + r.n); // ① 渲染到游标 + L + 块长;② 新一代接着上一代的位置写一块
            REQUIRE(r.head() == refHi + r.n); // 这一格要的形状:新一代的头 = 上一代最后观测到的头 + 一个步长
            r.runOut();
            REQUIRE(r.t > refHi + 2 * r.n); // 读方确实读过了 [上一代的头, 新一代的头 + 一块) 那一段
            INFO(r.firstWrong);
            CHECK(r.wrongFrames == 0);
            // 新一代真正从哪写起读方不知道:[上一代最后观测到的头, 新一代第一次看到的头) 那一块读方证明不了,不读
            // (与 LS-12 领先轨句首同一条裁定:至多一个写方块)。A-13 之前这里还要多丢「写头再动一次」的那一块。
            CHECK(r.lostBlocks - lost0 <= 1);
            CHECK(r.src.gapCount() == 0);
        }
    }
}

TEST_CASE("ShmRingMixSource A-13 写头正好多一步(3) R1 并发窗口:那个头是上一代的旧头,新一代往哪写都不错音",
          "[mix][ring][a13]")
{
    // 上一代在读方最后一次观测之后恰好又写了一块(写头 = refHi + 一块),随后写方单独跳变:换代,新一代第一段还没发布
    // (InputProcessor 换代与发布之间,只出现在 Input 与 Output 真正并发的宿主上)。读方这一块看到「新一代、写头 =
    // refHi + 一块」—— 与①②的读数一模一样,但这个头是上一代的。下一块之前新一代第一段发布,落点分几种。
    constexpr int64_t kLead = 4096;
    struct Shape
    {
        const char* name;
        int64_t offset; // 新一代第一段起点 − 那个旧头;kBackToCursor = 回到读方游标重新渲染
    };
    constexpr int64_t kBackToCursor = std::numeric_limits<int64_t>::min();
    const Shape shapes[] = {
        {"back to the Output cursor (in-place re-render)", kBackToCursor},
        {"contiguous with the old head", 0},
        {"forward by half a block", kA13Block / 2},
        {"forward by one block", kA13Block},
        {"forward by the lead", kLead},
        {"forward by twice the lead (beyond the span bound)", 2 * kLead},
    };
    for (const Shape& s : shapes)
    {
        INFO(s.name);
        LeadRig r(kLead, 200000);
        r.steadyFor(8);
        const int64_t refHi = r.seenHead;
        r.writeTo(r.t + kLead + r.n); // 上一代多写的那一块(读方没看到)
        const int64_t oldHead = r.head();
        REQUIRE(oldHead == refHi + r.n);
        r.bump(); // 换代,新一代第一段还没发布
        r.read(); // 读方落在这个窗口里
        REQUIRE(r.seenHead == oldHead);
        REQUIRE(r.seenEpoch == 2u);
        if (s.offset == kBackToCursor)
        {
            r.wPos = r.t;
            r.writeTo(r.t + kLead + r.n);
        }
        else
        {
            r.jumpWrite(oldHead + s.offset, r.n);
        }
        r.runOut();
        if (s.offset > 0)
        {
            // [旧头, 新一代起点) 两代都没写过:读方后来确实读到过写头已越过、环里却不是本位置数据的块(这一格有牙)。
            CHECK(r.hazardBlocks > 0);
        }
        INFO(r.firstWrong);
        CHECK(r.wrongFrames == 0);
        CHECK(r.src.gapCount() == 0);
    }
}

TEST_CASE("ShmRingMixSource A-13 写头正好多一步(3) R1 窗口 + 写方节拍不稳:最大步长两块时新一代往前跳半块也不错音",
          "[mix][ring][a13]")
{
    // 并发宿主上写方的节拍会抖:两次读方观测之间写方偶尔写两块(读方量到的最大步长 = 两块,最小步长 = 一块)。
    // 这时「正好多一步」= 多两块。上一代在读方最后一次观测之后恰好又写了两块、随后换代(R1),新一代往前跳半块、
    // 写一块。按「最大步长」复核分不出它和「新一代的头、写方又推进了 1.5 块」;按「最小步长」复核分得出。
    constexpr int64_t kLead = 4096;
    LeadRig r(kLead, 200000);
    r.steadyFor(4);
    r.writeTo(r.t + kLead + 2 * r.n); // 抖一下:这一块之前写方多写了一块(步长两块)
    r.read();
    r.steadyFor(4); // 之后照常(下一块写方已经够了,不写;再往后每块一块)
    const int64_t refHi = r.seenHead;
    r.writeTo(r.t + kLead + 2 * r.n); // 上一代又写了两块(读方没看到)
    const int64_t oldHead = r.head();
    REQUIRE(oldHead == refHi + 2 * r.n);
    r.bump();
    r.read();
    REQUIRE(r.seenHead == oldHead);
    r.jumpWrite(oldHead + r.n / 2, r.n); // 新一代往前跳半块,写一块
    r.runOut();
    CHECK(r.hazardBlocks > 0);
    INFO(r.firstWrong);
    CHECK(r.wrongFrames == 0);
    CHECK(r.src.gapCount() == 0);
}

TEST_CASE("ShmRingMixSource A-13 写头正好多一步(4) 跨代与别名:两次观测间换了两代 / 新一代写到上一代窗同槽,都不错音",
          "[mix][ring][a13]")
{
    constexpr int64_t kLead = 4096;
    SECTION("two generations between two reads, the second one starting half a block past the old head")
    {
        LeadRig r(kLead, 200000);
        r.steadyFor(8);
        const int64_t refHi = r.seenHead;
        r.bump();
        r.jumpWrite(refHi + 30000, r.n); // 第一代:往前跳到远处写了一块
        r.bump();
        r.jumpWrite(refHi + r.n / 2, r.n / 2); // 第二代:跳回来,从旧头之后半块写到旧头 + 一块
        REQUIRE(r.head() == refHi + r.n);
        r.runOut();
        CHECK(r.hazardBlocks > 0);
        INFO(r.firstWrong);
        CHECK(r.wrongFrames == 0);
    }
    SECTION("R1 window, then another generation before the first publication")
    {
        LeadRig r(kLead, 200000);
        r.steadyFor(8);
        const int64_t refHi = r.seenHead;
        r.writeTo(r.t + kLead + r.n);
        const int64_t oldHead = r.head();
        REQUIRE(oldHead == refHi + r.n);
        r.bump();
        r.read(); // 看到「新一代、旧头 + 一块」
        r.bump(); // 又换一代,往前跳半块写一块
        r.jumpWrite(oldHead + r.n / 2, r.n);
        r.runOut();
        CHECK(r.hazardBlocks > 0);
        INFO(r.firstWrong);
        CHECK(r.wrongFrames == 0);
    }
    SECTION("R1 window, then the first publication aliases slots of the previous-generation window still to be read")
    {
        LeadRig r(kLead, 200000);
        r.steadyFor(8);
        const int64_t refHi = r.seenHead;
        r.writeTo(r.t + kLead + r.n);
        const int64_t oldHead = r.head();
        REQUIRE(oldHead == refHi + r.n);
        r.bump();
        r.read();
        // 新一代往前跳将近一个环长:第一段写在读方再过一块要读的那些位置 + 环长,与它们同槽。
        const int64_t aliased = r.t + r.n;
        r.jumpWrite(aliased + static_cast<int64_t>(kA13Ring), r.n);
        REQUIRE_FALSE(r.ringHolds(aliased)); // 那一块的环槽里现在是下一圈的数据
        r.runOut();
        CHECK(r.hazardBlocks > 0);
        INFO(r.firstWrong);
        CHECK(r.wrongFrames == 0);
    }
}

TEST_CASE("ShmRingMixSource A-13 写头正好多一步(5) 锁步写方单独换代、前后各一块先算 Output:锁步锚恰在暂定头上也不抬",
          "[mix][ring][a13]")
{
    // 同步宿主(先 Input 后 Output,读写同块长 64)上写方单独换代(例:重新 prepare 之后第一块),换代前后各有一块宿主先算
    // 了 Output(N2a 那一形)。读方在新一代第一次看到的写头 = 本块块首 = 上一代最后观测到的头 + 一块:「正好多一步」;
    // 锁步规则把锚下在本块块首,数值上恰好也等于那个暂定头。上一代里宿主在事件点给过一块 32 帧的短块,量到的最小步长
    // 只有半块。下一块恢复先算 Input,写头一次动两块:锁步锚若被当成暂定头的锚按最小步长抬(写头 − 32),这一块的前半截
    // 就被抬出当前窗,环里明明是新一代为本位置写下的数据也读不到。
    RingFixture f(4096, 2);
    f.header.epoch.store(1, std::memory_order_release);
    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    std::vector<float> out(64 * 2, -1.0f);
    int64_t t = 1000;
    const auto inputFirst = [&](int n) {
        writePositions(f, t, n);
        const bool ok = src.read(t, out.data(), n, 1) && holdsPositions(out, t, n);
        t += n;
        return ok;
    };
    for (int k = 0; k < 6; ++k)
    {
        REQUIRE(inputFirst(64));
    }
    REQUIRE(inputFirst(32)); // 事件点上的短块
    for (int k = 0; k < 6; ++k)
    {
        REQUIRE(inputFirst(64));
    }
    // 先算 Output 的第一块(写头还停在块首,读不到),随后 Input 重新 prepare 之后的第一块:换代、写本块。
    CHECK_FALSE(src.read(t, out.data(), 64, 1));
    f.header.epoch.fetch_add(1, std::memory_order_release);
    writePositions(f, t, 64);
    t += 64;
    // 先算 Output 的第二块:看到「新一代、写头 = 本块块首」(这一格要的形状),读不到;随后 Input 写本块。
    REQUIRE(static_cast<int64_t>(f.header.write_head_samples.load(std::memory_order_acquire)) == t);
    CHECK_FALSE(src.read(t, out.data(), 64, 1));
    writePositions(f, t, 64);
    t += 64;
    // 恢复先算 Input:本块整块是新一代为本位置写下的数据。
    CHECK(inputFirst(64));
    CHECK(inputFirst(64));
    CHECK(src.gapCount() == 0);
}

// ---------------------------------------------------------------------------
// [A-16] 写方领先、循环回绕的读方台子(A-14 台子的推广,A-15 实验台 a15x 落库)。[a14] 与 [a16] 两组用例共用它。
//
// 时间线是一张「游标 → 位置」表:起播一段直线,读方游标 seekAt(8 块)处开场定位到 B,同时开 Cycle [B, B + len)。
// 写方按 InputProcessor 的规则换代(报的位置不接着上一块就 epoch+1);读方按 OutputProcessor 的规则换自己的代号
// (位置不接着上一块就 +1)。每个读方块之前,写方先处理到「读方游标 + 提前量 + 读方块长」(SchedRig 的预取条件)。
// 定位之前写方按直线渲染;定位时写方超过定位点的部分冲刷回定位点,从那里重新渲染(SchedRig 同款)。环里每帧写的是
// 它自己的时间线位置(两个声道都写)。在 A-14 的两种突发(① 开场定位对写方是空操作;② 写方停几块又一口补齐)之外,
// A-15 的「读方逐块输入相同、环里不同」的构造也在这里:
//   · 在途写:读方读的那一刻,写方下一笔普通写(inflight = 1)/ 一笔大写(inflight = 2)已写进环、写头还没发布 ——
//     读方这一刻看到的 epoch 与写头与不注入时完全相同;
//   · 新一代在读方看不到的地方先写一段:第 0 圈回绕时新一代先写 [B − startEdit, B),写头随即被下一笔盖过;
//   · 读方块长与写方块长不同(B256:读 256 / 写 1024)。
// 每块两份 oracle:
//   · 环(A-14 口径):读的那一刻环槽里整块是不是本位置的数据 —— 物理上读得到;
//   · 无限环(A-16 口径):写方这一遍(与读方同一圈)是不是已经写好并发布了这些位置 —— 环无限长时读得到。两者之差就是
//     读之前已被新一圈 / 在途写换掉的那些帧。
// ---------------------------------------------------------------------------

namespace
{
namespace a16
{
constexpr u32 kRing = scvb::kDefaultRingFrames; // 2^19 = 生产环长(与 LS-4 同)
constexpr int64_t kR = static_cast<int64_t>(kRing);
constexpr int64_t kLoopB = 141 * 1024; // 144384,与 LS-4 的 kLs4LoopB 同值

struct Cell
{
    std::string name;
    int64_t len = 0; // 圈长
    int rBlock = 1024; // 读方块长
    int wBlock = 1024; // 写方块长
    int64_t lead = 48000; // 提前量
    int laps = 3; // 开场定位之后跑几圈
    bool noopSeek = false; // ① 开场定位目标 = 写方下一块要写的位置(写方不换代);否则比它低一块(写方往回跳一块、换代)
    int stall = 0; // ② 第 0 圈里写方停的读方游标长度(以 1024 帧计,0 = 不停),停完一口补齐
    int64_t startEdit = 0; // 第 0 圈回绕时新一代先写的 [B − startEdit, B)(X3 世界 2)
    // 在途写:1 = 第 0 圈尾段每次读时写方下一笔普通写在途(X1 / B256 世界 2);
    //         2 = 第 bigAt 次尾段读时一笔 bigLen 帧的写在途(X2 世界 2)
    int inflight = 0;
    int64_t bigLen = 0;
    int bigAt = 10;
    bool recordSeen = false; // 记下读方每块看到的输入(世界对比较用)
};

// 稳态写头领先读方块首的量:写方按整块推到「块首 + 提前量 + 读方块长」之上。
int64_t aheadOf(const Cell& c)
{
    return (c.lead + c.rBlock + c.wBlock - 1) / c.wBlock * c.wBlock;
}

// 余量 = 圈长 − k·环长 − 稳态提前量(k = 离圈长最近的整环数)。
int64_t marginOf(const Cell& c)
{
    const int64_t k = (c.len + kR / 2) / kR;
    return c.len - k * kR - aheadOf(c);
}

struct Lap
{
    int64_t blocks = 0; // 读方读的块数
    int64_t ringAvail = 0; // 其中读的那一刻环槽里整块是本位置数据的(A-14 口径)
    int64_t lost = 0; // 环里整块是本位置数据、读方却没读的块
    int64_t lostFrames = 0;
    int64_t firstLost = -1; // 第一个这样的块的时间线位置
    int64_t lostEnd = -1; // 最后一个这样的块的块尾
    int64_t infFrames = 0; // 写方这一遍已写好并发布了的帧(无限环口径)
    int64_t lostInfFrames = 0; // 其中读方没交出的帧
    int64_t rescuedFrames = 0; // 读方交出了、读的那一刻环槽里却已不是本位置数据的帧
};

// 读方一块看到的输入(世界对比较用:同一对世界里读方逐块看到的这三个数必须相同)。
struct Seen
{
    int64_t t0 = 0;
    u64 epoch = 0;
    int64_t head = 0;
    bool operator==(const Seen& o) const { return t0 == o.t0 && epoch == o.epoch && head == o.head; }
};

struct Result
{
    std::vector<Lap> laps; // 按读方自己的跳变切:[0] = 起播(开场定位之前),[1] = 第 0 圈,……
    int64_t wrong = 0; // 返回 true 的块里不是本位置数据的帧(I1 违例)
    std::string firstWrong;
    int64_t writerNextAtSeek = -1; // 开场定位那一刻写方下一块要写的位置(InputProcessor 的 expectedNext_)
    int64_t bumpsAtSeek = 0; // 开场定位让写方换了几代(0 = 空操作)
    int64_t maxStep = 0; // 测试侧记账:第 0 圈里读方相邻两次观测之间、同一代里写头的最大前进量
    int64_t wrapAt = -1; // 写方第 0 圈回绕(换代)之后读方读的第一块的时间线位置
    int injected = 0; // 注入在途写的次数
    int64_t firstInjectedRead = -1; // 第一次注入是第几次读(从 0 起)
    std::vector<Seen> seen; // recordSeen 时才记
    u32 gaps = 0;
};

// rc.1 读方(test_mix_source_provenance.cpp 的 LegacyReader 去掉计数):只作对照打印,不作判据。
class Rc1Reader
{
public:
    void bind(AudioRingHeader* header, float* data) noexcept
    {
        h_ = header;
        d_ = data;
        ring_ = header->ring_frames;
        ch_ = header->channels;
    }
    bool read(int64_t t0, float* dst, int n, u64 /*readerEpoch*/) noexcept
    {
        const u64 e1 = h_->epoch.load(std::memory_order_acquire);
        const u64 w = h_->write_head_samples.load(std::memory_order_acquire);
        if (e1 != lastEpoch_)
        {
            lastEpoch_ = e1;
            validFrom_ = t0;
        }
        const u64 t0u = static_cast<u64>(t0);
        if (t0 < validFrom_ || w < t0u + static_cast<u64>(n) || w - t0u > ring_)
        {
            return false;
        }
        const u32 mask = ring_ - 1;
        for (int i = 0; i < n; ++i)
        {
            const u32 frame = static_cast<u32>(static_cast<u64>(t0 + i)) & mask;
            for (u32 c = 0; c < ch_; ++c)
            {
                dst[static_cast<std::size_t>(i) * ch_ + c] = d_[static_cast<std::size_t>(frame) * ch_ + c];
            }
        }
        const u64 e2 = h_->epoch.load(std::memory_order_acquire);
        const u64 w2 = h_->write_head_samples.load(std::memory_order_acquire);
        return e2 == e1 && w2 <= t0u + ring_;
    }
    u32 gapCount() const noexcept { return 0; }

private:
    AudioRingHeader* h_ = nullptr;
    float* d_ = nullptr;
    u32 ring_ = 0;
    u32 ch_ = 2;
    u64 lastEpoch_ = 0;
    int64_t validFrom_ = 0;
};

template<class Reader>
Result run(const Cell& cell, Reader& src)
{
    const int64_t len = cell.len;
    const int rb = cell.rBlock;
    const int wb = cell.wBlock;
    const int64_t seekAt = 8 * static_cast<int64_t>(std::max(rb, wb)); // 起播 8 块之后开场定位(读方游标)
    const int64_t stallAt = seekAt + 40 * 1024; // ② 第 0 圈里写方停下的读方游标
    const int64_t ahead = aheadOf(cell);
    // 起播位置:定位那一刻写方下一块 = 起播位置 + 定位游标 + 稳态提前量 − 写方块长。① 让它正好等于 B;否则比 B 高一块,
    // 定位对写方是往回跳一块(SchedRig 开场守卫多跑一块之后就是这一形)。
    const int64_t pre0 = kLoopB - seekAt - (ahead - wb) + (cell.noopSeek ? 0 : wb);
    bool seeked = false;
    const auto posAt = [&](int64_t c) { return (!seeked || c < seekAt) ? pre0 + c : kLoopB + (c - seekAt) % len; };
    const auto toEdge = [&](int64_t c) {
        return (!seeked || c < seekAt) ? std::numeric_limits<int64_t>::max() : len - (c - seekAt) % len;
    };
    // 游标落在第几遍:0 = 起播,1 = 第 0 圈,……(写方与读方同一张表;无限环 oracle 按它认「这一遍写好的」)。
    const auto passOf = [&](int64_t c) { return (!seeked || c < seekAt) ? int64_t{0} : 1 + (c - seekAt) / len; };

    RingFixture f(kRing, 2);
    f.header.epoch.store(1, std::memory_order_release);
    src.bind(&f.header, f.data.data());
    std::vector<float> out(static_cast<std::size_t>(rb) * 2);
    std::vector<char> ringOk(static_cast<std::size_t>(rb));
    std::vector<char> infOk(static_cast<std::size_t>(rb));
    // 无限环影子:每个位置最近一次**已发布**的写入属于第几遍(-1 = 没写过)。在途写读方按协议看不见,发布时才记。
    std::vector<signed char> pubPass(static_cast<std::size_t>(kLoopB + len + 4 * wb), -1);
    const auto publish = [&](int64_t p, int64_t m, int64_t pass) {
        std::fill_n(pubPass.begin() + static_cast<std::ptrdiff_t>(p), static_cast<std::size_t>(m),
                    static_cast<signed char>(pass));
    };
    Result res;

    int64_t wC = 0;
    int64_t wExpected = std::numeric_limits<int64_t>::lowest(); // InputProcessor:prepare 之后的哨兵(第一块必换代)
    int64_t bumps = 0;
    int wraps = 0; // 开场定位之后写方的回绕次数
    const auto writeTo = [&](int64_t target) {
        while (wC < target)
        {
            const int m = static_cast<int>(std::min<int64_t>(wb, toEdge(wC)));
            const int64_t p = posAt(wC);
            const int64_t pass = passOf(wC);
            if (p != wExpected)
            {
                f.header.epoch.fetch_add(1, std::memory_order_release);
                ++bumps;
                if (seeked && wC > seekAt && ++wraps == 1 && cell.startEdit > 0)
                {
                    // 新一代真正的起点在 B 之下:先写 [B − startEdit, B),写头随即被下面这一块盖过,读方看不到。
                    writePositions(f, p - cell.startEdit, static_cast<int>(cell.startEdit));
                    publish(p - cell.startEdit, cell.startEdit, pass);
                }
            }
            writePositions(f, p, m);
            publish(p, m, pass);
            wExpected = p + m;
            wC += m;
        }
    };

    const u32 mask = kRing - 1;
    const int64_t stallEnd = stallAt + static_cast<int64_t>(cell.stall) * 1024;
    const int64_t rEnd = seekAt + static_cast<int64_t>(cell.laps) * len;
    int64_t rC = 0;
    int64_t rExpected = std::numeric_limits<int64_t>::lowest();
    u64 rEpoch = 0;
    int64_t lastHead = -1;
    u64 lastEpoch = 0;
    int tailReads = 0;
    int64_t reads = 0;
    while (rC < rEnd)
    {
        if (rC == seekAt)
        {
            // 开场定位 + 开 Cycle。宿主冲刷:写方游标超过定位点的部分作废,从定位点按新时间线重新渲染。
            res.writerNextAtSeek = wExpected;
            seeked = true;
            wC = std::min(wC, seekAt);
            const int64_t before = bumps;
            writeTo(rC + cell.lead + rb);
            res.bumpsAtSeek = bumps - before;
        }
        else if (rC < stallAt || rC >= stallEnd)
        {
            writeTo(rC + cell.lead + rb);
        }
        const int m = static_cast<int>(std::min<int64_t>(rb, toEdge(rC)));
        const int64_t t0 = posAt(rC);
        const int64_t pass = passOf(rC);
        if (t0 != rExpected)
        {
            ++rEpoch; // OutputProcessor 的 podEpoch_:读方自己的时间线跳了
            res.laps.emplace_back();
        }
        rExpected = t0 + m;
        const int64_t head = static_cast<int64_t>(f.header.write_head_samples.load(std::memory_order_acquire));
        const u64 ep = f.header.epoch.load(std::memory_order_acquire);
        bool tail = false;
        if (res.laps.size() == 2u)
        {
            if (ep == lastEpoch && head > lastHead)
            {
                res.maxStep = std::max(res.maxStep, head - lastHead);
            }
            if (res.wrapAt < 0 && ep != lastEpoch && rC > seekAt)
            {
                res.wrapAt = t0;
            }
            tail = res.wrapAt >= 0;
        }
        lastHead = head;
        lastEpoch = ep;
        if (cell.recordSeen)
        {
            res.seen.push_back(Seen{t0, ep, head});
        }

        // 在途写:写方的一笔已写进环、写头还没发布(AudioRing::write 先写数据、后发布写头之间那一刻)。
        bool bigPending = false;
        int64_t bigP = 0;
        int bigM = 0;
        const int64_t wp = posAt(wC);
        bool inject = false;
        if (tail && cell.inflight == 1 && wp == wExpected)
        {
            writeUnpublished(f, wp, static_cast<int>(std::min<int64_t>(wb, toEdge(wC))));
            inject = true;
        }
        if (tail && cell.inflight == 2 && tailReads == cell.bigAt && wp == wExpected)
        {
            bigP = wp;
            bigM = static_cast<int>(std::min<int64_t>(cell.bigLen, toEdge(wC)));
            writeUnpublished(f, bigP, bigM);
            bigPending = true;
            inject = true;
        }
        if (inject)
        {
            ++res.injected;
            res.firstInjectedRead = res.firstInjectedRead < 0 ? reads : res.firstInjectedRead;
        }

        bool avail = true;
        int64_t infAvail = 0;
        for (int i = 0; i < m; ++i)
        {
            const std::size_t slot = static_cast<std::size_t>(static_cast<u32>(static_cast<u64>(t0 + i)) & mask);
            ringOk[static_cast<std::size_t>(i)] = static_cast<char>(f.data[slot * 2] == static_cast<float>(t0 + i) &&
                                                                    f.data[slot * 2 + 1] == static_cast<float>(t0 + i));
            infOk[static_cast<std::size_t>(i)] =
                static_cast<char>(pubPass[static_cast<std::size_t>(t0 + i)] == static_cast<signed char>(pass));
            avail = avail && ringOk[static_cast<std::size_t>(i)] != 0;
            infAvail += infOk[static_cast<std::size_t>(i)] != 0 ? 1 : 0;
        }
        std::fill(out.begin(), out.end(), -1.0f);
        const bool ok = src.read(t0, out.data(), m, rEpoch);
        Lap& lap = res.laps.back();
        ++lap.blocks;
        lap.ringAvail += avail ? 1 : 0;
        lap.infFrames += infAvail;
        if (ok)
        {
            for (int i = 0; i < m; ++i)
            {
                const bool right = out[static_cast<std::size_t>(i) * 2] == static_cast<float>(t0 + i) &&
                                   out[static_cast<std::size_t>(i) * 2 + 1] == static_cast<float>(t0 + i);
                if (!right)
                {
                    if (res.firstWrong.empty())
                    {
                        res.firstWrong = "t0=" + std::to_string(t0) + " frame=" + std::to_string(i) +
                                         " got=" + std::to_string(out[static_cast<std::size_t>(i) * 2]);
                    }
                    ++res.wrong;
                }
                else if (ringOk[static_cast<std::size_t>(i)] == 0)
                {
                    ++lap.rescuedFrames;
                }
            }
        }
        else
        {
            lap.lostInfFrames += infAvail;
            if (avail)
            {
                ++lap.lost;
                lap.lostFrames += m;
                lap.firstLost = lap.firstLost < 0 ? t0 : lap.firstLost;
                lap.lostEnd = t0 + m;
            }
        }
        if (bigPending)
        {
            // 那一笔大写在读之后才完成:发布写头,写方往前走。
            f.header.write_head_samples.store(static_cast<u64>(bigP + bigM), std::memory_order_release);
            publish(bigP, bigM, passOf(wC));
            wExpected = bigP + bigM;
            wC += bigM;
        }
        if (tail)
        {
            ++tailReads;
        }
        ++reads;
        rC += m;
    }
    res.gaps = src.gapCount();
    return res;
}
} // namespace a16
} // namespace

// ---------------------------------------------------------------------------
// [A-14] 同一代里写方一口突发、随后循环回绕:上一圈尾段读不到。这是 A-13 附带调查的读方层面确定性形状 —— Windows CI 上
// LS-4 lead=48000 loop=12s 第 0 圈两条预取轨各丢 47 帧。产品层面改不改读方待裁定,本卡只钉证据,不动 src/。
//
// 机制:写方在**同一代**里、读方相邻两次观测之间一口气写了远大于一块的一笔(突发 S)。读方把它记成一步:本代最大步长
// stepMaxCur_ = S,在途保护 inflightGuard() 跟着变成 S,换代时作为 stepMaxPrev_ 带进下一代。写方领先 A(写头 −
// 读方块首),先于读方循环回绕(换代)。之后读方还在上一圈尾段,这一段由上一代窗读;别名判据要求新一圈写到的位置加在途保护碰
// 不到它的同槽。圈长 len ≥ 环长 R 时,余量是 len − R − A:保护是一块时,余量够一块就照读;保护被突发放大到 S、余量又不到 S
// 时,从写方回绕到读方回绕这一整段(约一个提前量)都不认 —— 不错音、不报警。所以分得出来的是余量落在 [块长, S)
// 这一带(圈长 [R + A + 块长, R + A + S))。余量不到一块的圈(圈长 [R, R + A + 块长))没有突发也读不到尾段,那是另一回事,
// 不归这里。
// 突发的两种来历:
//   ① 开场定位对写方是空操作:定位目标正好是写方下一块要写的位置,写方不换代;宿主照样冲刷,从定位点一口气补渲染到「游标 +
//      提前量 + 块长」。LS-4 那一次就是这样:脚本起点 96256 时,定位目标 144384 正好是预取轨的下一块;
//   ② 宿主的预取停了几块,又一口补齐(读方时间线连续,写方不换代)。
// 判据与 A-3 oracle 同口径:
//   · 安全:返回 true 的每一帧都必须是本位置写下的数据(错帧 = 0),必须绿;
//   · 期望:读时环里整块是本位置数据、读方却没读的块记丢失,每圈至多丢一个写方块。现行读方在带内做不到,打 [!shouldfail]
//     留证据。
// 台子:上面 [A-16] 一节的 a16::run(由 A-14 落地时的 runA14 推广而来,时间线与写方 / 读方规则逐条相同)。
// 这里的格都取生产环长 2^19、提前量 48000、读写块长 1024、循环起点 144384,与 LS-4 那一格逐数相同
// (稳态 A = 49152;12 s 圈余量 576000 − 524288 − 49152 = 2560)。
// ---------------------------------------------------------------------------

namespace
{
constexpr u32 kA14Ring = scvb::kDefaultRingFrames; // 2^19 = 生产环长(与 LS-4 同)
constexpr int64_t kA14R = static_cast<int64_t>(kA14Ring);
constexpr int kA14Block = 1024;
constexpr int64_t kA14Lead = 48000;
// 稳态时写头领先读方块首的量:写方按整块推到「块首 + 提前量 + 块长」之上 ⇒ ceil((L + 块长) / 块长) 块。
constexpr int64_t kA14Ahead = (kA14Lead + kA14Block + kA14Block - 1) / kA14Block * kA14Block;
static_assert(kA14Ahead == 49152, "LS-4 lead=48000 shape: the writer head leads the reader block start by 48 blocks");
constexpr int64_t kA14LoopB = 141 * kA14Block; // 144384,与 LS-4 的 kLs4LoopB 同值
static_assert(kA14LoopB == a16::kLoopB, "the A-14 cells run on the A-16 rig with the same loop start");
constexpr int64_t kA14Len12s = 576000; // LS-4 的 12 s 圈(48 kHz)
static_assert(kA14Len12s - kA14R - kA14Ahead == 2560, "LS-4 12 s loop: margin 2560 over ring + lead");
constexpr int kA14Laps = 3;

struct A14Cell
{
    std::string name;
    int64_t len = 0; // 圈长
    bool writerNoopSeek = false; // ① 开场定位目标 = 写方下一块要写的位置;否则比它低一块(写方往回跳一块、换代)
    int stallBlocks = 0; // ② 第 0 圈里写方停的读方块数(0 = 不停),停完一口补齐:突发 = (停的块数 + 1) 块
};

struct A14Lap
{
    int64_t blocks = 0;
    int64_t availBlocks = 0; // 读时环里整块是本位置数据
    int64_t lostBlocks = 0; // 其中读方没读的
    int64_t lostFrames = 0;
    int64_t firstLost = -1; // 第一个丢失块的时间线位置
    int64_t lostEnd = -1; // 最后一个丢失块的块尾
};

struct A14Result
{
    std::vector<A14Lap> laps; // 按读方自己的跳变切:[0] = 起播(开场定位之前),[1] = 第 0 圈,……
    int64_t wrongFrames = 0; // 返回 true 的块里不是本位置数据的帧(I1 违例)
    std::string firstWrong;
    int64_t writerNextAtSeek = -1; // 开场定位那一刻写方下一块要写的位置(InputProcessor 的 expectedNext_)
    int64_t bumpsAtSeek = 0; // 开场定位让写方换了几代(0 = 空操作)
    int64_t maxSameGenStep = 0; // 测试侧记账:第 0 圈里读方相邻两次观测之间、同一代里写头的最大前进量
    int64_t wrapAt = -1; // 写方第 0 圈回绕(换代)之后读方读的第一块的时间线位置
    u32 gaps = 0;
};

std::string str(const A14Lap& l)
{
    std::string s = "lost " + std::to_string(l.lostBlocks) + "/" + std::to_string(l.availBlocks) + " blocks (" +
                    std::to_string(l.lostFrames) + " frames)";
    if (l.availBlocks != l.blocks)
    {
        s += " of " + std::to_string(l.blocks) + " read";
    }
    if (l.firstLost >= 0)
    {
        s += " in [" + std::to_string(l.firstLost) + "," + std::to_string(l.lostEnd) + ")";
    }
    return s;
}

// A-14 的一格放到 A-16 台子上跑(提前量 48000、读写块长 1024、三圈)。
A14Result runA14(const A14Cell& cell)
{
    a16::Cell c;
    c.name = cell.name;
    c.len = cell.len;
    c.rBlock = kA14Block;
    c.wBlock = kA14Block;
    c.lead = kA14Lead;
    c.laps = kA14Laps;
    c.noopSeek = cell.writerNoopSeek;
    c.stall = cell.stallBlocks;
    ShmRingMixSource src;
    const a16::Result r = a16::run(c, src);
    A14Result res;
    for (const a16::Lap& l : r.laps)
    {
        A14Lap a;
        a.blocks = l.blocks;
        a.availBlocks = l.ringAvail;
        a.lostBlocks = l.lost;
        a.lostFrames = l.lostFrames;
        a.firstLost = l.firstLost;
        a.lostEnd = l.lostEnd;
        res.laps.push_back(a);
    }
    res.wrongFrames = r.wrong;
    res.firstWrong = r.firstWrong;
    res.writerNextAtSeek = r.writerNextAtSeek;
    res.bumpsAtSeek = r.bumpsAtSeek;
    res.maxSameGenStep = r.maxStep;
    res.wrapAt = r.wrapAt;
    res.gaps = r.gaps;
    return res;
}

// 场景确实造出来了(与读方怎么读无关):圈确实回绕了、每圈读方读的时候环里都有本位置的数据、写方先于读方约一个提前量
// 回绕、开场定位对写方是不是空操作、第 0 圈同一代里读方量到的最大一步就是设计的突发。
void requireA14Shape(const A14Cell& c, const A14Result& r, bool hazard = false)
{
    REQUIRE(r.laps.size() == static_cast<std::size_t>(kA14Laps + 1));
    for (std::size_t i = 0; i < r.laps.size(); ++i)
    {
        const A14Lap& lap = r.laps[i];
        REQUIRE(lap.blocks >= 8);
        if (hazard && i > 0)
        {
            REQUIRE(lap.availBlocks < lap.blocks); // 有牙:这一圈里确实有读方要的块已被新一圈换掉(读了就是错音)
        }
        else
        {
            REQUIRE(lap.availBlocks == lap.blocks); // oracle:每一块读的时候环里都是本位置的数据(读得到是可能的)
        }
    }
    REQUIRE(r.wrapAt >= kA14LoopB + c.len - kA14Ahead - kA14Block);
    REQUIRE(r.wrapAt <= kA14LoopB + c.len - kA14Ahead + kA14Block);
    if (c.writerNoopSeek)
    {
        REQUIRE(r.writerNextAtSeek == kA14LoopB); // ①:定位目标就是写方下一块要写的位置……
        REQUIRE(r.bumpsAtSeek == 0); // ……写方不换代
    }
    else
    {
        REQUIRE(r.writerNextAtSeek == kA14LoopB + kA14Block);
        REQUIRE(r.bumpsAtSeek == 1); // 写方往回跳一块、换代
    }
    const int64_t burst = c.writerNoopSeek ? kA14Ahead : (c.stallBlocks > 0 ? (c.stallBlocks + 1) * kA14Block : 0);
    REQUIRE(r.maxSameGenStep == std::max<int64_t>(burst, kA14Block));
}

std::string lapsStr(const A14Result& r)
{
    std::string s;
    for (std::size_t i = 1; i < r.laps.size(); ++i)
    {
        s += " | lap" + std::to_string(i - 1) + " " + str(r.laps[i]);
    }
    return s;
}

// 带内(余量 len − R − A 落在 [块长, 突发) 里):现行读方第 0 圈整段尾巴不认。后两格是带的内沿。
const A14Cell kA14OpeningNoop12s{"(1) opening seek onto the writer's next block, 12 s loop (LS-4)", kA14Len12s, true,
                                 0};
const A14Cell kA14Stall23At12s{"(2) writer stalls 23 blocks then catches up (burst 24 blocks), 12 s loop", kA14Len12s,
                               false, 23};
const A14Cell kA14InBandEdges[] = {
    {"(1) opening seek onto the writer's next block, margin burst - 1024", kA14R + kA14Ahead + kA14Ahead - kA14Block,
     true, 0},
    {"(2) writer stalls 3 blocks then catches up (burst 4 blocks > margin 2560), 12 s loop", kA14Len12s, false, 3},
};
// 带外与对照:每圈至多丢一块。没有突发(SchedRig 开场守卫生效后的那一形)、突发不超过余量、余量正好等于突发。
const A14Cell kA14OutOfBand[] = {
    {"control: opening seek one block below the writer's next block (writer jumps back), 12 s loop", kA14Len12s, false,
     0},
    {"(2) writer stalls 1 block then catches up (burst 2 blocks <= margin 2560), 12 s loop", kA14Len12s, false, 1},
    {"(2) writer stalls 3 blocks then catches up, margin = burst (4096)", kA14R + kA14Ahead + 4 * kA14Block, false, 3},
    {"(2) writer stalls 23 blocks then catches up, margin = burst (24576)", kA14R + kA14Ahead + 24 * kA14Block, false,
     23},
    {"(1) opening seek onto the writer's next block, margin = burst (49152)", kA14R + kA14Ahead + kA14Ahead, true, 0},
};
// 安全判据的牙:圈长不到「环长 + 提前量」(余量 −24576),新一圈在读方读到之前就把上一圈尾段的一部分同槽换掉了 —— 读方要是
// 认这些块就是错音。有没有突发都一样必须不错音(丢多少不判:这一带没有突发也读不到尾段,不归本卡)。
const A14Cell kA14Hazard[] = {
    {"hazard: no burst, margin -24576", kA14R + kA14Ahead - 24 * kA14Block, false, 0},
    {"hazard: (1) opening seek onto the writer's next block, margin -24576", kA14R + kA14Ahead - 24 * kA14Block, true,
     0},
    {"hazard: (2) writer stalls 23 blocks then catches up, margin -24576", kA14R + kA14Ahead - 24 * kA14Block, false,
     23},
};
} // namespace

TEST_CASE("ShmRingMixSource A-14 同一代里写方一口突发后循环回绕:不错音;突发不超过余量或没有突发时每圈至多丢一块",
          "[mix][ring][a14]")
{
    std::vector<A14Cell> inBand = {kA14OpeningNoop12s, kA14Stall23At12s};
    inBand.insert(inBand.end(), std::begin(kA14InBandEdges), std::end(kA14InBandEdges));
    for (const A14Cell& c : inBand)
    {
        INFO(c.name << " len=" << c.len);
        const A14Result r = runA14(c);
        requireA14Shape(c, r);
        INFO(r.firstWrong);
        CHECK(r.wrongFrames == 0); // 安全(I1):带内也必须成立
        // 带内只有第 0 圈受影响:下一次换代带进去的是第 1 圈量到的步长(一块),后面几圈每圈至多丢一块。
        for (std::size_t i = 2; i < r.laps.size(); ++i)
        {
            INFO("lap " << i - 1);
            CHECK(r.laps[i].lostBlocks <= 1);
        }
        WARN(c.name << " (margin " << c.len - kA14R - kA14Ahead << "): wrapAt " << r.wrapAt << " maxSameGenStep "
                    << r.maxSameGenStep << " gaps " << r.gaps << " wrong " << r.wrongFrames << lapsStr(r));
    }
    for (const A14Cell& c : kA14OutOfBand)
    {
        INFO(c.name << " len=" << c.len);
        const A14Result r = runA14(c);
        requireA14Shape(c, r);
        INFO(r.firstWrong);
        CHECK(r.wrongFrames == 0);
        for (std::size_t i = 1; i < r.laps.size(); ++i)
        {
            INFO("lap " << i - 1);
            CHECK(r.laps[i].lostBlocks <= 1);
        }
        WARN(c.name << " (margin " << c.len - kA14R - kA14Ahead << "): wrapAt " << r.wrapAt << " maxSameGenStep "
                    << r.maxSameGenStep << " gaps " << r.gaps << " wrong " << r.wrongFrames << lapsStr(r));
    }
    for (const A14Cell& c : kA14Hazard)
    {
        INFO(c.name << " len=" << c.len);
        const A14Result r = runA14(c);
        requireA14Shape(c, r, true);
        INFO(r.firstWrong);
        CHECK(r.wrongFrames == 0);
        WARN(c.name << ": wrapAt " << r.wrapAt << " maxSameGenStep " << r.maxSameGenStep << " gaps " << r.gaps
                    << " wrong " << r.wrongFrames << lapsStr(r));
    }
}

// 期望(待裁定:改读方,或接受并写进 KI-11):带内第 0 圈也至多丢一个写方块。现行读方把写方回绕之后的那一整段尾巴都不认
// (12 s 圈两格都是 48 块 = 48640 帧,位置 [671744, 720384),即相对循环起点 [527360, 576000);数字由上一条用例的 WARN
// 打印)。这两条失败 = 通过;哪天读方改了、转绿,Catch2 判红,逼着去掉标记。
// 读这两条的「failed as expected」时要同时看上一条用例是绿的:场景前提(同两格同一套 requireA14Shape)在那里 REQUIRE。
// 这里也调一次只为下标安全 —— 它一红同样会被记成 failed as expected,自己分不出是读方丢块还是台子坏了(评审第 1 轮)。
TEST_CASE("ShmRingMixSource A-14 (1) 开场定位对写方是空操作(LS-4 那一形)之后循环回绕:上一圈尾段至多丢一个写方块",
          "[mix][ring][a14][!shouldfail]")
{
    const A14Result r = runA14(kA14OpeningNoop12s);
    requireA14Shape(kA14OpeningNoop12s, r);
    INFO("lap0 " << str(r.laps[1]));
    CHECK(r.laps[1].lostBlocks <= 1);
}

TEST_CASE("ShmRingMixSource A-14 (2) 写方停几块又一口补齐之后循环回绕:上一圈尾段至多丢一个写方块",
          "[mix][ring][a14][!shouldfail]")
{
    const A14Result r = runA14(kA14Stall23At12s);
    requireA14Shape(kA14Stall23At12s, r);
    INFO("lap0 " << str(r.laps[1]));
    CHECK(r.laps[1].lostBlocks <= 1);
}

// ---------------------------------------------------------------------------
// [A-16] 读方层面的永久见证(A-15 实验台 a15x 落库;台子见上面 [A-16] 一节)。
//
// 圈长落在 k·环长前后一个提前量一带(生产环长、提前量 1 s 时约 9.90–11.97 s,4/4 拍 4 小节约 80–97 BPM)时,写方回绕之后
// 读方还要播的上一圈尾段(约一个提前量)走「上一代窗 + 别名判据」这条环路径,现行读方在这一带每圈丢约一个提前量:
//   ③'   圈长 (R − A, R]:数据全在环里,但新一代跨度的下界保守,尾段被判成别名;
//   物理段 (R, R + A):上一圈尾段在读之前就被新一圈换掉了(环装不下「一圈 + 提前量」);
//   ②    余量 [0, 896):在途保护让开下一笔在途写;
//   ①②  突发后第 0 圈:突发把在途保护放大,余量 [0, S) 都读不到。
// A-15 证明这条环路径的判据放宽不得:每一对世界里,读方逐块看到的 (t0, epoch, 写头) 完全相同,只有环里内容不同 —— 任何只看
// 这些量的放宽(在途保护去掉 / 只取本代 / 封顶一块,新一代跨度下界收紧到一块)都会在其中一个世界里交出别处的数据。
// 本节用例在现行读方上必须绿(错帧 0);它们同时是那几种放宽的删除式牙齿(落点与红在哪见 PR 的删除式表)。
//   · 世界对 X1 / X2 / X3 / B256:前提 REQUIRE(圈确实回绕、写方先于读方约一个提前量回绕、世界 2 的在途写确实注入、
//     世界 2 的环里确实有块被换掉、两个世界读方的输入逐块相同),判据 CHECK 错帧 0;
//   · 边界扫描:圈长取 A-15 两套扫描的并集 × 突发 {无 / ① / ②}(读方块长 1024;读方块长 256 只扫没有突发的一组,
//     带内各余量另有 B256 世界对),另加提前量 4096 一组与 2R 附近三格。
// 每圈丢失按无限环口径计帧(写方这一遍已发布、读方却没交出)。现行读方每圈至多丢「稳态提前量 + 一个写方块」——
// 这是现状上界,一并 CHECK;逐格数字由 WARN 打印。rc.1 读方的对照数字只在隐藏用例 [a16-report] 里打印。
// ---------------------------------------------------------------------------

namespace
{
// 现行读方的每圈上界(无限环口径):稳态提前量 + 一个写方块。
int64_t a16CurrentBound(const a16::Cell& c)
{
    return a16::aheadOf(c) + c.wBlock;
}

// 场景确实造出来了(与读方怎么读无关):圈数、每圈都读了块、写方先于读方约一个提前量回绕、开场定位对写方是不是空操作。
void requireA16Shape(const a16::Cell& c, const a16::Result& r)
{
    REQUIRE(r.laps.size() == static_cast<std::size_t>(c.laps + 1));
    for (const a16::Lap& lap : r.laps)
    {
        REQUIRE(lap.blocks >= 8);
    }
    const int64_t ahead = a16::aheadOf(c);
    // 读方在写方回绕之后的第一个块边界上才看到它:差一个写方块(写方按块推进)加一个读方块(读方按块观测)以内。
    REQUIRE(r.wrapAt >= a16::kLoopB + c.len - ahead - c.wBlock - c.rBlock);
    REQUIRE(r.wrapAt <= a16::kLoopB + c.len - ahead + c.wBlock + c.rBlock);
    REQUIRE(r.bumpsAtSeek == (c.noopSeek ? 0 : 1));
}

std::string a16Line(const a16::Cell& c, const a16::Result& r)
{
    std::string s = c.name + " | len " + std::to_string(c.len) + " margin " + std::to_string(a16::marginOf(c)) +
                    " lead " + std::to_string(c.lead) + " rb " + std::to_string(c.rBlock) + " | wrapAt " +
                    std::to_string(r.wrapAt) + " maxStep " + std::to_string(r.maxStep) + " injected " +
                    std::to_string(r.injected) + " gaps " + std::to_string(r.gaps) + " wrong " +
                    std::to_string(r.wrong);
    if (!r.firstWrong.empty())
    {
        s += " first{" + r.firstWrong + "}";
    }
    for (std::size_t i = 1; i < r.laps.size(); ++i)
    {
        const a16::Lap& l = r.laps[i];
        s += " | lap" + std::to_string(i - 1) + " lostInf " + std::to_string(l.lostInfFrames) + "/" +
             std::to_string(l.infFrames) + " ringLost " + std::to_string(l.lost) + "/" + std::to_string(l.ringAvail) +
             " of " + std::to_string(l.blocks) + " rescued " + std::to_string(l.rescuedFrames);
    }
    return s;
}

// 每圈(无限环口径)丢失不超过 bound 帧。
void checkA16Laps(const a16::Result& r, int64_t bound)
{
    for (std::size_t i = 1; i < r.laps.size(); ++i)
    {
        INFO("lap " << i - 1);
        CHECK(r.laps[i].lostInfFrames <= bound);
    }
}

struct A16WorldPair
{
    a16::Cell w1;
    a16::Cell w2;
    bool teeth = true; // 世界 2 的环里确实有读方要的块在读之前被换掉(false = 只是对照)
};

std::vector<A16WorldPair> a16Worlds()
{
    using a16::kR;
    constexpr int64_t kA = 49152; // 提前量 48000、读写块长 1024 时的稳态提前量
    constexpr int64_t kN = 1024;
    std::vector<A16WorldPair> v;
    // X1(②):余量 512,没有突发。世界 2:第 0 圈尾段每次读时写方下一笔普通写在途,正好写进读方这一块的槽。
    {
        A16WorldPair p;
        p.w1 = a16::Cell{"X1 world1: margin 512, nothing in flight", kR + kA + 512};
        p.w2 = p.w1;
        p.w2.name = "X1 world2: margin 512, the writer's next block in flight at every lap-0 tail read";
        p.w2.inflight = 1;
        v.push_back(p);
    }
    // X1 对照:余量 2560,在途那一笔落不进读方这一块的槽。
    {
        A16WorldPair p;
        p.w1 = a16::Cell{"X1 control world1: margin 2560, nothing in flight", kR + kA + 2560};
        p.w2 = p.w1;
        p.w2.name = "X1 control world2: margin 2560, the writer's next block in flight (lands outside the tail)";
        p.w2.inflight = 1;
        p.teeth = false;
        v.push_back(p);
    }
    // X2(①②):12 s 圈。世界 2:第 10 次尾段读时一笔 4096 帧的写在途(突发也可能就是一笔)。
    for (const bool noop : {true, false})
    {
        A16WorldPair p;
        p.w1 = a16::Cell{noop ? "X2 world1: (1) opening seek no-op, 12 s loop"
                              : "X2 world1: (2) writer stalls 23 blocks, 12 s loop",
                         576000};
        p.w1.noopSeek = noop;
        p.w1.stall = noop ? 0 : 23;
        p.w2 = p.w1;
        p.w2.name = p.w1.name + ", one 4096-frame write in flight at the 10th lap-0 tail read";
        p.w2.inflight = 2;
        p.w2.bigLen = 4096;
        v.push_back(p);
    }
    // X3(物理段):余量 −24576。世界 2:新一代实际从 B − 24576 起写(仍在新一代跨度的假设之内)。
    for (const bool noop : {false, true})
    {
        A16WorldPair p;
        p.w1 = a16::Cell{noop ? "X3 world1: (1) opening seek no-op, margin -24576" : "X3 world1: margin -24576",
                         kR + kA - 24 * kN};
        p.w1.noopSeek = noop;
        p.w2 = p.w1;
        p.w2.name = p.w1.name + ", the new generation starts 24576 below B";
        p.w2.startEdit = 24 * kN;
        v.push_back(p);
    }
    // B256:读方块长 256、写方块长 1024。世界 2:下一个写方块在途。余量 >= 1024 时在途那一笔落不进尾段(对照)。
    for (const int64_t m : {int64_t{0}, int64_t{256}, int64_t{512}, int64_t{768}, int64_t{1024}, int64_t{2048}})
    {
        A16WorldPair p;
        p.w1 = a16::Cell{"B256 world1: reader block 256, writer block 1024, margin " + std::to_string(m), 0};
        p.w1.rBlock = 256;
        p.w1.len = kR + a16::aheadOf(p.w1) + m;
        p.w2 = p.w1;
        p.w2.name = p.w1.name + ", the next writer block in flight";
        p.w2.inflight = 1;
        p.teeth = m < kN;
        v.push_back(p);
    }
    for (A16WorldPair& p : v)
    {
        p.w1.recordSeen = true;
        p.w2.recordSeen = true;
    }
    return v;
}

// 边界扫描的格:圈长取 A-15 两套扫描(R + A 一带与 R 之下一带)的并集。
std::vector<a16::Cell> a16Sweep()
{
    using a16::kR;
    constexpr int64_t kN = 1024;
    struct Kind
    {
        const char* name;
        bool noop;
        int stall;
        int64_t burst;
    };
    const Kind kinds[] = {{"no burst", false, 0, kN},
                          {"(1) opening seek no-op, burst 49152", true, 0, 49152},
                          {"(2) writer stalls 23 blocks, burst 24576", false, 23, 24576}};
    std::vector<a16::Cell> v;
    const auto add = [&v](const std::string& name, int64_t len, int rb, int64_t lead, bool noop, int stall) {
        a16::Cell c{name, len};
        c.rBlock = rb;
        c.lead = lead;
        c.laps = 2;
        c.noopSeek = noop;
        c.stall = stall;
        v.push_back(c);
    };
    for (const int rb : {1024, 256})
    {
        for (const Kind& k : kinds)
        {
            if (rb == 256 && (k.noop || k.stall > 0))
            {
                continue; // 读方块长 256 只扫没有突发的一组(带内各余量另有 B256 世界对)
            }
            constexpr int64_t kA = 49152;
            const std::set<int64_t> lens = {kR - kA - 4 * kN,
                                            kR - kA - 2 * kN,
                                            kR - kA - kN,
                                            kR - kA,
                                            kR - kA + kN,
                                            kR - 24 * kN,
                                            kR - 4 * kN,
                                            kR - kN,
                                            kR,
                                            kR + kN,
                                            kR + kA - 24 * kN,
                                            kR + kA - kN,
                                            kR + kA,
                                            kR + kA + 512,
                                            kR + kA + 895,
                                            kR + kA + 896,
                                            kR + kA + kN,
                                            kR + kA + 2 * kN,
                                            kR + kA + 2560,
                                            kR + kA + k.burst - kN,
                                            kR + kA + k.burst,
                                            kR + kA + k.burst + kN};
            for (const int64_t len : lens)
            {
                add(std::string(k.name) + ", rb " + std::to_string(rb), len, rb, 48000, k.noop, k.stall);
            }
        }
        if (rb == 256)
        {
            continue;
        }
        // 提前量 4096(稳态提前量 5120):带宽跟着提前量缩。
        for (const int64_t len :
             {kR - 8 * kN, kR - 6 * kN, kR - 5 * kN, kR - 4 * kN, kR - kN, kR, kR + 4 * kN, kR + 5 * kN, kR + 6 * kN})
        {
            add("lead 4096, no burst, rb " + std::to_string(rb), len, rb, 4096, false, 0);
        }
    }
    // 2R 附近(圈长约 21.8 s):同一机制,只是隔两环。
    for (const int64_t len : {2 * kR - kN, 2 * kR + 49152 - 24 * kN, 2 * kR + 49152 + 512})
    {
        add("2R, no burst, rb 1024", len, 1024, 48000, false, 0);
    }
    return v;
}
} // namespace

TEST_CASE("ShmRingMixSource A-16 世界对 X1 / X2 / X3 / B256:读方逐块输入相同、环里内容不同,两个世界都不错音",
          "[mix][ring][a16]")
{
    for (const A16WorldPair& p : a16Worlds())
    {
        INFO(p.w2.name);
        ShmRingMixSource s1;
        ShmRingMixSource s2;
        const a16::Result r1 = a16::run(p.w1, s1);
        const a16::Result r2 = a16::run(p.w2, s2);
        requireA16Shape(p.w1, r1);
        requireA16Shape(p.w2, r2);
        if (p.w2.inflight > 0)
        {
            REQUIRE(r2.injected > 0); // 在途写确实注入了
        }
        // 两个世界里读方逐块看到的 (t0, epoch, 写头) 相同;X2 到注入那一次读为止(之后那一笔大写发布,写头才分开)。
        const std::size_t upTo =
            p.w2.inflight == 2 ? static_cast<std::size_t>(r2.firstInjectedRead + 1) : r2.seen.size();
        REQUIRE(upTo > 0u);
        REQUIRE(r1.seen.size() >= upTo);
        REQUIRE(r2.seen.size() >= upTo);
        REQUIRE(std::equal(r2.seen.begin(), r2.seen.begin() + static_cast<std::ptrdiff_t>(upTo), r1.seen.begin()));
        if (p.teeth)
        {
            REQUIRE(r2.laps[1].ringAvail < r1.laps[1].ringAvail); // 世界 2 的环里确实有块在读之前被换掉
        }
        INFO("world1 " << a16Line(p.w1, r1));
        INFO("world2 " << a16Line(p.w2, r2));
        CHECK(r1.wrong == 0);
        CHECK(r2.wrong == 0);
        checkA16Laps(r1, a16CurrentBound(p.w1));
        checkA16Laps(r2, a16CurrentBound(p.w2));
        WARN("world1 " << a16Line(p.w1, r1));
        WARN("world2 " << a16Line(p.w2, r2));
    }
}

TEST_CASE("ShmRingMixSource A-16 边界扫描:圈长落在 k 倍环长前后一个提前量一带,现行读方不错音,"
          "每圈至多丢一个提前量加一块",
          "[mix][ring][a16]")
{
    const std::vector<a16::Cell> cells = a16Sweep();
    REQUIRE(cells.size() == 94u);
    for (const a16::Cell& c : cells)
    {
        INFO(c.name << " len=" << c.len);
        ShmRingMixSource src;
        const a16::Result r = a16::run(c, src);
        requireA16Shape(c, r);
        INFO(a16Line(c, r));
        CHECK(r.wrong == 0);
        checkA16Laps(r, a16CurrentBound(c));
        WARN(a16Line(c, r));
    }
}

// rc.1 读方的对照数字(隐藏,ctest 不跑):`scvb_tests "[a16-report]"`。
TEST_CASE("ShmRingMixSource A-16 report: current reader vs rc.1 on every world and sweep cell", "[.][a16-report]")
{
    std::vector<a16::Cell> cells;
    for (const A16WorldPair& p : a16Worlds())
    {
        cells.push_back(p.w1);
        cells.push_back(p.w2);
    }
    const std::vector<a16::Cell> sweep = a16Sweep();
    cells.insert(cells.end(), sweep.begin(), sweep.end());
    for (const a16::Cell& c : cells)
    {
        ShmRingMixSource cur;
        a16::Rc1Reader rc1;
        WARN("NEW " << a16Line(c, a16::run(c, cur)));
        WARN("RC1 " << a16Line(c, a16::run(c, rc1)));
    }
    SUCCEED();
}

// ---------------------------------------------------------------------------
// [A-6] 持续读不到 → 失准(E1b,设计稿 §3.4;ShmRingMixSource 头注「持续读不到」)。块长 480 = 10 ms @ 48 kHz,
// 宽限下限 200 ms = 9600 样本 = 20 块(这些用例里读方一块都没在当前窗里读成功过,或提前量很小,宽限就是这一项)。
// 环取 65536 / 131072:让在途保护与别名判据都碰不到,只看 A-6 这一条。
// ---------------------------------------------------------------------------

namespace
{
constexpr int kA6Block = 480;

int64_t a6Grace()
{
    return 48000 * ShmRingMixSource::kUnreadGraceMs / 1000;
}
} // namespace

TEST_CASE("ShmRingMixSource A-6 (a) 持续读不到:读方从一代开始一直领先写头,宽限内不计,超过宽限后写方推进的每块计失准",
          "[mix][ring][a6]")
{
    // H3 的读方层面最小形状:宿主每块先算 Output 再算 Input(或人声轨没送进装 Output 的总线 / 读方位置超前),
    // Output 要的 [t0, t0+n) 恰是写方这一块还没写的那段。本代永远 primed 不了 —— A-6 之前一次都不计(无声且不报警)。
    REQUIRE(a6Grace() == 20 * kA6Block);
    RingFixture f(1u << 16, 2);
    f.header.epoch.store(1, std::memory_order_release);
    writePositions(f, 0, kA6Block);

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    std::vector<float> out(static_cast<std::size_t>(kA6Block) * 2, -1.0f);
    int64_t pos = kA6Block; // 读方位置
    int64_t wpos = kA6Block; // 写方位置
    const auto outputFirst = [&] {
        const bool ok = src.read(pos, out.data(), kA6Block, 1);
        pos += kA6Block;
        writePositions(f, wpos, kA6Block);
        wpos += kA6Block;
        return ok;
    };

    // 宽限之内(读不到的这一段 ≤ 200 ms 读方时间线):照旧不计 —— 换代之初的短暂领先是正常的(N2a)。
    for (int i = 0; i < 20; ++i)
    {
        CHECK_FALSE(outputFirst());
    }
    CHECK(src.gapCount() == 0u);
    CHECK(src.unprimedFailCount() > 0u); // A-5 的成因分流确实把它分在 (a)
    // 超过宽限:写方在推进的每一块计一笔(「路由失准」横幅的数据源)。
    CHECK_FALSE(outputFirst());
    CHECK(src.gapCount() == 1u);
    CHECK(src.persistentFailCount() == 1u);
    for (int i = 0; i < 9; ++i)
    {
        CHECK_FALSE(outputFirst());
    }
    CHECK(src.gapCount() == 10u);

    // 写方停着(宿主挂起了 Input):照样读不到,但那是挂起、不是失准(P1-7),不计。停住之后的第一次读还看得见
    // 写方上一块的推进(照计);从第二次起写头一动不动。
    CHECK_FALSE(src.read(pos, out.data(), kA6Block, 1));
    pos += kA6Block;
    const u32 g0 = src.gapCount();
    CHECK(g0 == 11u);
    for (int i = 0; i < 5; ++i)
    {
        CHECK_FALSE(src.read(pos, out.data(), kA6Block, 1));
        pos += kA6Block;
    }
    CHECK(src.gapCount() == g0);
    // 写方又动了(仍落后于读方):动的那一块照计。
    writePositions(f, wpos, kA6Block);
    wpos += kA6Block;
    CHECK_FALSE(src.read(pos, out.data(), kA6Block, 1));
    pos += kA6Block;
    CHECK(src.gapCount() == g0 + 1u);

    // 写方追上读方:读到了 ⇒ primed,此后回到旧口径;持续读不到的计数不再增长。
    while (wpos < pos + 2 * kA6Block)
    {
        writePositions(f, wpos, kA6Block);
        wpos += kA6Block;
    }
    REQUIRE(src.read(pos, out.data(), kA6Block, 1));
    CHECK(holdsPositions(out, pos, kA6Block));
    CHECK(src.persistentFailCount() == src.gapCount()); // 这条轨上的缺口全是「持续读不到」那一类
    CHECK(src.persistentFailCount() == g0 + 1u);
}

TEST_CASE("ShmRingMixSource A-6 持续读不到在读方漏读之后重新起算(本轨被移出注入集一段时间又回来)", "[mix][ring][a6]")
{
    // 漏读期间读不读得到无从得知:不能把这段时间线算进宽限,否则一条刚回到注入集的轨只要再有一块读不到就报警。
    RingFixture f(1u << 16, 2);
    f.header.epoch.store(1, std::memory_order_release);
    writePositions(f, 0, kA6Block);
    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    std::vector<float> out(static_cast<std::size_t>(kA6Block) * 2, -1.0f);
    int64_t pos = kA6Block;
    int64_t wpos = kA6Block;
    const auto outputFirst = [&] {
        const bool ok = src.read(pos, out.data(), kA6Block, 1);
        pos += kA6Block;
        writePositions(f, wpos, kA6Block);
        wpos += kA6Block;
        return ok;
    };
    for (int i = 0; i < 15; ++i)
    {
        CHECK_FALSE(outputFirst());
    }
    // 1 s 不读(读方代号不变:不是读方跳变,是中间没读),写方照常写到读方回来时的位置之前。
    for (int i = 0; i < 100; ++i)
    {
        pos += kA6Block;
        writePositions(f, wpos, kA6Block);
        wpos += kA6Block;
    }
    for (int i = 0; i < 15; ++i)
    {
        CHECK_FALSE(outputFirst());
    }
    CHECK(src.gapCount() == 0u); // 回来之后读不到的只有 15 块(150 ms)< 宽限
    for (int i = 0; i < 6; ++i)
    {
        CHECK_FALSE(outputFirst());
    }
    CHECK(src.gapCount() == 1u); // 回来之后满 21 块:照计
}

TEST_CASE("ShmRingMixSource A-6 (b) 读方卡在锚点之下:超过「实测提前量上界 + 宽限」才计失准(H1 的安全网)",
          "[mix][ring][a6]")
{
    // 写方领先读方 1920 帧(读方块尾之外再多 3 块)稳定读一阵,量到提前量上界 1920;随后写方单独往前跳 1 s
    // (宿主跳过了这一轨 1 s 的渲染)。读方没跳:上一代窗接住到上一代写头,往后是「锚点(新一代确认过的写头)
    // 之下、上一代窗也接不住」—— (b)。读方要走完锚点之下这一段才读得到;这一段超过「提前量上界 + 宽限」
    // 还读不到,就不是交接,是真的没有数据。
    constexpr int64_t kAhead = 1920;
    RingFixture f(1u << 17, 2);
    f.header.epoch.store(1, std::memory_order_release);
    writePositions(f, 0, static_cast<int>(kAhead));
    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    std::vector<float> out(static_cast<std::size_t>(kA6Block) * 2, -1.0f);
    int64_t pos = 0;
    int64_t wpos = kAhead;
    const auto leadingBlock = [&] {
        const bool ok = src.read(pos, out.data(), kA6Block, 1);
        const bool right = !ok || holdsPositions(out, pos, kA6Block);
        pos += kA6Block;
        writePositions(f, wpos, kA6Block);
        wpos += kA6Block;
        return ok && right;
    };
    for (int i = 0; i < 20; ++i)
    {
        INFO("lead block " << i);
        REQUIRE(leadingBlock());
    }

    // 写方单独往前跳 1 s:换代,新一代从 wpos + 48000 起写。
    f.header.epoch.fetch_add(1, std::memory_order_release);
    wpos += 48000;
    // 上一代最后观测到的写头 = 上一块读的时候的写头(读方读完之后写方又写的那一块读方没看见:写过,但证明不了)。
    const int64_t prevHi = pos - kA6Block + kAhead;
    const int64_t anchor = wpos + kA6Block; // 新一代第一次被看到的写头(保守规则锚在它上面)
    // 上一代窗接住读方到上一代写头为止。
    while (pos < prevHi)
    {
        INFO("prev window pos " << pos);
        CHECK(leadingBlock());
    }
    // (b):锚点之下、上一代窗接不住。宽限 = 提前量上界 1920 + max(9600, 2·1920 + 480) = 11520 = 24 块。
    const u32 stuck0 = src.stuckBelowAnchorCount();
    for (int i = 0; i < 24; ++i)
    {
        CHECK_FALSE(leadingBlock());
    }
    CHECK(src.stuckBelowAnchorCount() > stuck0); // A-5 的成因分流确实把它分在 (b)
    CHECK(src.gapCount() == 0u); // 宽限之内(只给 (a) 的宽限、不加提前量上界的话,第 21 块就计了)
    CHECK_FALSE(leadingBlock());
    CHECK(src.gapCount() == 1u); // 第 25 块:照计
    // 读方走到锚点之后读到了:不再计。
    while (pos < anchor)
    {
        CHECK_FALSE(leadingBlock());
    }
    const u32 gaps = src.gapCount();
    CHECK(gaps > 1u);
    for (int i = 0; i < 5; ++i)
    {
        CHECK(leadingBlock());
    }
    CHECK(src.gapCount() == gaps);
}

TEST_CASE("ShmRingMixSource A-6 段头 channels 与快照不一致单独计:不算失准,也不续长、不打断「持续读不到」",
          "[mix][ring][a6]")
{
    // 几何改写(mono⇄stereo)之后、[M] 换绑之前(≤ 一拍),段头 channels 与读方快照不一致,整块不读(A-5)。
    // A-5 时这类块记在 handoverLoss 里;A-6 起单独计 layoutMismatch:几何改写的短暂丢失不能凑满「持续读不到」
    // 的宽限(误报),也不能让正在进行的那段连续失败重新起算(掩盖真的读不到)。
    RingFixture f(1u << 16, 2);
    f.header.epoch.store(1, std::memory_order_release);
    std::vector<float> out(static_cast<std::size_t>(kA6Block) * 2, -1.0f);

    SECTION("primed 之后:不计失准,计 layoutMismatch,不进 handoverLoss")
    {
        ShmRingMixSource src;
        src.bind(&f.header, f.data.data());
        int64_t pos = 0;
        for (int i = 0; i < 5; ++i)
        {
            writePositions(f, pos, kA6Block);
            REQUIRE(src.read(pos, out.data(), kA6Block, 1));
            pos += kA6Block;
        }
        const u32 handover0 = src.handoverLossCount();
        f.header.channels = 1; // 写方按新布局改写了段头(快照仍是 2)
        for (int i = 0; i < 4; ++i)
        {
            writePositions(f, pos, kA6Block); // 写方照写(在推进)
            CHECK_FALSE(src.read(pos, out.data(), kA6Block, 1));
            pos += kA6Block;
        }
        CHECK(src.gapCount() == 0u);
        CHECK(src.layoutMismatchCount() == 4u);
        CHECK(src.handoverLossCount() == handover0);
        f.header.channels = 2;
        writePositions(f, pos, kA6Block);
        CHECK(src.read(pos, out.data(), kA6Block, 1));
        CHECK(holdsPositions(out, pos, kA6Block));
    }

    SECTION("primed 之前、一段连续读不到的中途:不续长、不重新起算")
    {
        writePositions(f, 0, kA6Block);
        ShmRingMixSource src;
        src.bind(&f.header, f.data.data());
        int64_t pos = kA6Block;
        int64_t wpos = kA6Block;
        const auto outputFirst = [&] {
            const bool ok = src.read(pos, out.data(), kA6Block, 1);
            pos += kA6Block;
            writePositions(f, wpos, kA6Block);
            wpos += kA6Block;
            return ok;
        };
        for (int i = 0; i < 15; ++i)
        {
            CHECK_FALSE(outputFirst());
        }
        f.header.channels = 1;
        for (int i = 0; i < 4; ++i)
        {
            CHECK_FALSE(outputFirst());
        }
        f.header.channels = 2;
        CHECK(src.layoutMismatchCount() == 4u);
        // 读不到的块:之前 15 块 + 之后 j 块。宽限 20 块 ⇒ 之后第 6 块才计。
        for (int j = 1; j <= 5; ++j)
        {
            CHECK_FALSE(outputFirst());
        }
        CHECK(src.gapCount() == 0u); // 把不一致那 4 块也算进去的话,之后第 2 块就计了
        CHECK_FALSE(outputFirst());
        CHECK(src.gapCount() == 1u); // 不一致那几块让它重新起算的话,要到之后第 21 块
    }
}

TEST_CASE("ShmRingMixSource A-6 冷启动不报警:空环、写方晚到、换代之初短暂领先", "[mix][ring][a6]")
{
    std::vector<float> out(static_cast<std::size_t>(kA6Block) * 2, -1.0f);

    SECTION("刚 attach 的空环(写方一帧没写):读 1 s 都不计")
    {
        RingFixture f(1u << 16, 2);
        ShmRingMixSource src;
        src.bind(&f.header, f.data.data());
        int64_t pos = 0;
        for (int i = 0; i < 100; ++i)
        {
            CHECK_FALSE(src.read(pos, out.data(), kA6Block, 1));
            pos += kA6Block;
        }
        CHECK(src.gapCount() == 0u);
        // 写方随后在读方当前位置开写(先 Input 后 Output):第一块就读到,全程不计。
        for (int i = 0; i < 10; ++i)
        {
            writePositions(f, pos, kA6Block);
            CHECK(src.read(pos, out.data(), kA6Block, 1));
            pos += kA6Block;
        }
        CHECK(src.gapCount() == 0u);
    }

    SECTION("换代之初先算 Output 的那几块(< 宽限)之后恢复先算 Input:不计")
    {
        RingFixture f(1u << 16, 2);
        f.header.epoch.store(1, std::memory_order_release);
        writePositions(f, 0, kA6Block);
        ShmRingMixSource src;
        src.bind(&f.header, f.data.data());
        int64_t pos = kA6Block;
        for (int i = 0; i < 19; ++i) // 19 块 = 190 ms,压在 200 ms 宽限之内
        {
            CHECK_FALSE(src.read(pos, out.data(), kA6Block, 1));
            writePositions(f, pos, kA6Block);
            pos += kA6Block;
        }
        for (int i = 0; i < 20; ++i)
        {
            writePositions(f, pos, kA6Block);
            CHECK(src.read(pos, out.data(), kA6Block, 1));
            pos += kA6Block;
        }
        CHECK(src.gapCount() == 0u);
    }
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
    scvb::output::mixMonoSample(1.0f, 0.0f, 0.0f, 100.0f, 1.0f, kNoCurve, l, r);
    CHECK(l == Catch::Approx(0.70710678f).margin(1e-5));
    CHECK(r == Catch::Approx(0.70710678f).margin(1e-5));

    float hl = 0.0f;
    float hr = 0.0f;
    scvb::output::mixMonoSample(1.0f, -100.0f, 0.0f, 100.0f, 1.0f, kNoCurve, hl, hr); // 硬左
    CHECK(hl == Catch::Approx(1.0f).margin(1e-5));
    CHECK(hr == Catch::Approx(0.0f).margin(1e-5));
}

TEST_CASE("MixMath stereo dual-pan + width", "[mix][math]")
{
    // width=100、pan=0 → L 源 → L、R 源 → R(源宽度原样,不互换)。
    float l = 0.0f;
    float r = 0.0f;
    scvb::output::mixStereoSample(1.0f, 0.5f, 0.0f, 0.0f, 100.0f, 100.0f, 1.0f, kNoCurve, l, r);
    CHECK(l == Catch::Approx(1.0f).margin(1e-5)); // 子声像 P_L=-100:gL_L=1、gL_R=0
    CHECK(r == Catch::Approx(0.5f).margin(1e-5)); // 子声像 P_R=+100:gR_L=0、gR_R=1

    // width=0 → 双子声像重合(塌成 mono)。
    float ml = 0.0f;
    float mr = 0.0f;
    scvb::output::mixStereoSample(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 100.0f, 1.0f, kNoCurve, ml, mr);
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

    // 重新稳定绑定,确认仍可读(无永久损坏)。[A-5] 同一条环重绑 = 代际从头来:读方要等自己的时间线
    // 跳一次(或写头动一次)才重新锚定 —— 重绑多半是写方几何改写,写方领先时读方锚在自己的 t0 会按新步长
    // 读旧布局的数据(A-3 geometry 族)。这里环是静止的、写头不动,所以让读方的时间线跳一次(代号 0 → 1)。
    // 跳到 56:写头 64 只领先它 8 帧,不超过 reader 线程那边量到过的任何提前量(同步规则的上界判据),
    // 结果与两个线程的交错无关。
    src.unbind();
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());
    std::vector<float> out(8 * 2);
    REQUIRE(src.read(56, out.data(), 8, 1));
    REQUIRE(out[0] == 56.0f);
    REQUIRE(out[1] == 56.0f);
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

// =============================================================================
// [SL-442] pan 角度域曲线 G 进实时链(02 §8.1 步骤 5)—— 施加环节的判据。
// 主判据是 §7 那句原话:「UI 与 DSP 共用本实现……杜绝『画的和听的不一致』」。
// 接上 G 之前这句只成立一半(前端有、DSP 没有),下面钉的就是缺的那一半。
// =============================================================================

namespace
{

// bell A=-9, P0=+30, Q=6(Δ=100/6≈16.67)。三个选择都是为了让判据钉得住:
//   A=-9   —— 深槽,「施加 / 不施加」的差别远大于任何容差;
//   P0=+30 —— 不在中心,「查名义角」与「查有效角」才会落到曲线上差很多的两处;
//   Q=6    —— 钟够窄。Q=3(Δ=33.33)时名义角 +60 与有效角 +30 只差 3.87 dB,
//             我第一版就栽在这儿:差值不够大,「这个输入能否分辨两种实现」的前置断言当场红。
//             Q=6 时这个差拉到 8 dB。
std::vector<scvb::PanCurvePoint> sl442Curve()
{
    scvb::PanCurvePoint p;
    p.angle = 30.0f;
    p.gainDb = -9.0f;
    p.shape = scvb::PanCurveShape::bell;
    p.q = 6.0f;
    p.side = scvb::PanCurveSide::out;
    return {p};
}

void monoPair(float pan, float volDb, float globalWidth, const scvb::PanCurveXfade& curve, float& l, float& r)
{
    l = 0.0f;
    r = 0.0f;
    scvb::output::mixMonoSample(1.0f, pan, volDb, globalWidth, 1.0f, curve, l, r);
}

// 实时链实际施加的 dB = 有曲线 / 无曲线 的**能量**比。
// ⚠ 不能只拿单个通道做分母:equal-power 下 pan=+100 时 gL = cos(π/2),float 里是 -0.0f,
// 比值当场没意义(第一版我就是这么写的,被 `bare > 0` 那条前置断言拦下)。
// 而 gL²+gR² ≡ 1(ADR-010),所以能量比恰好就是增益比,与 pan 落在哪儿无关。
double monoAppliedDb(float pan, float globalWidth, const scvb::PanCurveXfade& curve)
{
    float l = 0.0f;
    float r = 0.0f;
    float bl = 0.0f;
    float br = 0.0f;
    monoPair(pan, 0.0f, globalWidth, curve, l, r);
    monoPair(pan, 0.0f, globalWidth, kNoCurve, bl, br);
    const double withG = std::sqrt(static_cast<double>(l) * l + static_cast<double>(r) * r);
    const double bare = std::sqrt(static_cast<double>(bl) * bl + static_cast<double>(br) * br);
    return 20.0 * std::log10(withG / bare);
}

// stereo:**只喂一路源**(另一路给 0),于是输出里只剩那一个子声像的贡献,
// 比值恰好等于该子声像各自的 G 增益。两路一起喂的话,单个输出通道里混着两个子声像,
// 比值是两者的加权和 —— 钉不住「每个子声像各查各的」(第一版我就是这么写的,推理太松)。
double stereoSubImageAppliedDb(bool useLeftSource, float pan, float trkWidth, float globalWidth,
                               const scvb::PanCurveXfade& curve)
{
    const float sL = useLeftSource ? 1.0f : 0.0f;
    const float sR = useLeftSource ? 0.0f : 1.0f;
    float l = 0.0f;
    float r = 0.0f;
    scvb::output::mixStereoSample(sL, sR, pan, 0.0f, trkWidth, globalWidth, 1.0f, curve, l, r);
    float bl = 0.0f;
    float br = 0.0f;
    scvb::output::mixStereoSample(sL, sR, pan, 0.0f, trkWidth, globalWidth, 1.0f, kNoCurve, bl, br);
    // 取两个输出通道的能量比,免得挑到 equal-power 增益恰为 0 的那一端。
    const double withG = std::sqrt(static_cast<double>(l) * l + static_cast<double>(r) * r);
    const double bare = std::sqrt(static_cast<double>(bl) * bl + static_cast<double>(br) * br);
    return 20.0 * std::log10(withG / bare);
}

} // namespace

TEST_CASE("SL442-MIX-1 空曲线 / 未接线时实时链逐位不变(老工程零影响)", "[mix][math][pancurve]")
{
    // 空点列表烘出来的表,和「压根没有表」必须走到同一个结果 —— 而且是**逐位**,不是近似:
    // 增益 = 10^(G/20),G 恰为 0 时因子恰为 1.0f,乘法按位恒等。
    scvb::PanCurveLut empty;
    empty.rebuild({});
    const scvb::PanCurveXfade withEmpty{&empty, nullptr, 1.0f};

    for (const float pan : {-100.0f, -37.5f, 0.0f, 30.0f, 100.0f})
    {
        for (const float vol : {-12.0f, 0.0f, 6.0f})
        {
            for (const float gw : {0.0f, 50.0f, 100.0f, 150.0f})
            {
                float bl = 0.0f;
                float br = 0.0f;
                float cl = 0.0f;
                float cr = 0.0f;
                monoPair(pan, vol, gw, kNoCurve, bl, br);
                monoPair(pan, vol, gw, withEmpty, cl, cr);
                REQUIRE(cl == bl); // 逐位,无容差
                REQUIRE(cr == br);
            }
        }
    }

    // stereo 同理 —— 它的求和式结构最容易在重构里被拆成 a*c+b*c 而丢掉按位恒等。
    for (const float w : {0.0f, 35.0f, 100.0f})
    {
        float bl = 0.0f;
        float br = 0.0f;
        scvb::output::mixStereoSample(0.75f, -0.3f, 20.0f, -4.0f, w, 80.0f, 1.0f, kNoCurve, bl, br);
        float cl = 0.0f;
        float cr = 0.0f;
        scvb::output::mixStereoSample(0.75f, -0.3f, 20.0f, -4.0f, w, 80.0f, 1.0f, withEmpty, cl, cr);
        REQUIRE(cl == bl);
        REQUIRE(cr == br);
    }
}

TEST_CASE("SL442-MIX-2 画的就是听的:实时链施加的增益 == UI 那条曲线", "[mix][math][pancurve]")
{
    const auto points = sl442Curve();
    scvb::PanCurveLut lut;
    lut.rebuild(points);
    const scvb::PanCurveXfade curve{&lut, nullptr, 1.0f};

    // 全局 width=100 ⇒ P_eff == P,此时「实时链施加的 dB」可以直接和解析式对。
    for (const double pan : {-100.0, -30.0, 0.0, 30.0, 63.33, 100.0})
    {
        // 容差 0.05 dB:LUT 插值上限 0.03(02 §7.3 CURVE-4)+ float 往返余量。
        // 被测量跨 -9..0 dB,谷底 -9 dB 是容差的 180 倍 —— 「同值」不可能靠容差蒙到。
        REQUIRE(monoAppliedDb(static_cast<float>(pan), 100.0f, curve) ==
                Catch::Approx(scvb::evalCurve(points, pan)).margin(0.05));
    }
    // 判别量:曲线底部确实压下去了 9 dB,不是压了个 0(全 0 的实现会让上面那圈全绿)。
    REQUIRE(monoAppliedDb(30.0f, 100.0f, curve) == Catch::Approx(-9.0).margin(0.05));
}

TEST_CASE("SL442-MIX-3 G 查在 P_eff 上,不是名义角(width≠100 才分得出)", "[mix][math][pancurve]")
{
    const auto points = sl442Curve(); // 谷底在 P=+30
    scvb::PanCurveLut lut;
    lut.rebuild(points);
    const scvb::PanCurveXfade curve{&lut, nullptr, 1.0f};

    // ---- mono:全局 width=50 ⇒ P_eff = P/2。取名义 P=+60 ⇒ P_eff=+30 = 谷底 ----
    // 查有效角 → 施加 -9 dB(谷底);查名义角 → 施加 evalCurve(+60) ≈ -1 dB。
    const double atNominal = scvb::evalCurve(points, 60.0);
    const double atEffective = scvb::evalCurve(points, 30.0);
    REQUIRE(std::fabs(atNominal - atEffective) > 5.0); // 先确认这个输入真的能分辨两种实现

    const double appliedDb = monoAppliedDb(60.0f, 50.0f, curve);
    REQUIRE(appliedDb == Catch::Approx(atEffective).margin(0.05)); // 是有效角
    REQUIRE(appliedDb != Catch::Approx(atNominal).margin(0.05)); // 且确实不是名义角

    // ---- stereo:两个子声像各查各的 ----
    // pan=0、w_t=60、全局 width=50 ⇒ 名义子声像 (-60,+60) → 缩放后 (-30,+30)。
    // 右子声像正落谷底(-9 dB),左子声像几乎不受影响(≈0 dB)。
    const double dLeftSub = stereoSubImageAppliedDb(true, 0.0f, 60.0f, 50.0f, curve);
    const double dRightSub = stereoSubImageAppliedDb(false, 0.0f, 60.0f, 50.0f, curve);
    REQUIRE(dLeftSub == Catch::Approx(scvb::evalCurve(points, -30.0)).margin(0.05));
    REQUIRE(dRightSub == Catch::Approx(scvb::evalCurve(points, 30.0)).margin(0.05));

    // 可分辨性:拿弧中心查一次再共用的实现,两个子声像会得到**同一个**增益。
    // 这条要求两者差得够开 —— 差不开的话上面两条对那种实现也可能同时成立。
    REQUIRE(std::fabs(dLeftSub - dRightSub) > 5.0);
    // 而共用弧中心的实现会给出 evalCurve(P_eff=0),两边都是这个值 —— 与上面两条都不符。
    const double atArcCentre = scvb::evalCurve(points, 0.0);
    REQUIRE(std::fabs(dRightSub - atArcCentre) > 5.0);
}
