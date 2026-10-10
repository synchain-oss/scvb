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
