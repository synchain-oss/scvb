// SPDX-License-Identifier: GPL-3.0-or-later
// test_seqlock_stress —— [B 线 M04] PlayheadShot / MeterShot 的 seqlock 双线程压测(arm64 内存序)。
//
// 被测的是产品代码本身:写方调 publish()、读方调 read(),两者都不在本文件里复刻。
// 一个写线程以最快速度连续发布第 k 帧(k = 1, 2, 3 …),每一帧的**全部字段**都是 k 的确定函数;
// 一个读线程反复 read()。read() 返回 true 的每一帧都要过三道判据(任一不过即记一次「收下撕裂帧」):
//   ① 自洽:按帧里携带的 k 重算整帧,逐字段相等 —— 半新半旧(k 与 k+1 混在一帧里)当场露馅;
//   ② 括号:read() 前后各读一次 seq(s0 / s1)。单写者下 seq == 2k 当且仅当第 k 帧已发布完,
//      所以 read() 收下的帧号必须落在 [ceil(s0/2), floor(s1/2)] 里。**这一条专门给写侧 fence 长牙**:
//      写侧缺了「奇数 seq 之后的 release fence」时,第 k+1 帧的 pod 可以先于奇数 seq 可见,读方会在
//      seq 仍是 2k 时收下一整帧**自洽**的第 k+1 帧 —— ① 看不出来,② 看得出来(帧号比 seq 允许的新);
//   ③ 单调:后一次收下的帧号不得小于前一次。
// 另把 shot 放在三种相对缓存行的位置上各跑一轮(见 Placement):自然对齐、seq 独占一行末尾而 pod
// 整个落在下一行、pod 跨行。arm64 架构允许不同缓存行的写乱序可见,所以设计上让 seq 与 pod 分属不同的行
// (B 线 M06a 的命令环用例只删写侧 fence 30/30 绿,记录就在同一行里);实测见下。
//
// 能抓到什么、抓不到什么(如实写):
//   · x86-64 是 TSO,写写不重排、读读不重排,fence 在那里只是编译器屏障 —— 本文件在 Windows 上
//     恒绿,删掉任何一道 fence 也绿。它是 arm64 的门禁,在 x86 上只证明判据本身不误报。
//   · arm64 实测(PR #366,GitHub macos-15 runner,PlayheadShot,三种摆放 × 30 轮,每轮 100ms):
//     - 删**写侧** fence(奇数增量改回 release RMW):30 / 30 绿,三条判据全 0 —— 这套压测在这台硬件上
//       **抓不到**写侧缺失,写侧 fence 的依据只是内存模型;
//     - 删**读侧** acquire fence:30 / 30 红,判据 ① ② 都响 —— 它在 arm64 上的牙在读侧。
// 断言纪律([SL-453]):工作线程里不放断言宏,只计数;join 之后在主线程断言。
// 默认不跑(隐藏标签 [.]):单跑由独立 ctest 条目 scvb_seqlock_stress 承担(tests/CMakeLists.txt),
// 不进 scvb_tests 那一条的 30s 预算。手跑:scvb_tests "[stress]"。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <new>
#include <thread>
#include <type_traits>

#include "engine/PlayheadShot.h"
#include "output/MeterShot.h"

namespace
{

using Clock = std::chrono::steady_clock;

// 每种位置上的读侧时长。三种位置 × 两种 shot = 6 轮,合计约 0.6s(另见 CMakeLists 的上界说明)。
constexpr auto kRoundDuration = std::chrono::milliseconds(100);
// 非空洞下限:读侧至少要真收下这么多帧、写侧至少要发布这么多帧,否则这一轮等于没测。
constexpr std::uint64_t kMinAccepted = 1000;
constexpr std::uint64_t kMinPublished = 1000;

// shot 相对 128 字节边界的摆放(Apple M 系列的缓存行是 128B;对 64B 行同样成立,128 是 64 的倍数)。
enum class Placement
{
    kNatural, // 对象起点 = 行首:seq 与 pod 开头同一行
    kSeqAloneAtLineEnd, // seq 落在一行最后几个字节,pod 从下一行行首开始
    kPodStraddlesLines, // seq 与 pod 前一段同一行,pod 后一段在下一行
};

const char* placementName(Placement p)
{
    switch (p)
    {
    case Placement::kNatural:
        return "natural";
    case Placement::kSeqAloneAtLineEnd:
        return "seq-alone-at-line-end";
    case Placement::kPodStraddlesLines:
        return "pod-straddles-lines";
    }
    return "?";
}

constexpr std::size_t kLine = 128;

// 对象起点在 128 字节对齐缓冲里的偏移:让 pod 恰好从下一行行首开始 / 跨过行边界。
std::size_t placementOffset(Placement p, std::size_t podOffset)
{
    switch (p)
    {
    case Placement::kNatural:
        return 0;
    case Placement::kSeqAloneAtLineEnd:
        return kLine - podOffset; // pod 起点 = kLine
    case Placement::kPodStraddlesLines:
        return kLine - podOffset - 24; // pod 前 24 字节在第 0 行(与 seq 同行),其余在第 1 行
    }
    return 0;
}

struct RoundResult
{
    std::uint64_t published = 0;
    std::uint64_t accepted = 0; // read() 返回 true 的次数
    std::uint64_t rejected = 0; // read() 返回 false 的次数(撕裂被识别,沿用上帧)
    std::uint64_t badSelf = 0; // 判据 ①
    std::uint64_t badBracket = 0; // 判据 ②
    std::uint64_t badMonotonic = 0; // 判据 ③
};

// 通用压测:Traits 提供 Shot / Pod 类型、make(k)、indexOf(pod)、equal(a, b)。
template<typename Traits>
RoundResult runRound(Placement placement)
{
    using Shot = typename Traits::Shot;
    using Pod = typename Traits::Pod;
    static_assert(alignof(Shot) <= kLine, "shot alignment exceeds the line size used for placement");
    static_assert(std::is_standard_layout_v<Shot>, "offsetof(Shot, pod) needs a standard-layout shot");

    alignas(256) static unsigned char storage[4 * kLine];
    const std::size_t off = placementOffset(placement, offsetof(Shot, pod));
    REQUIRE(off % alignof(Shot) == 0);
    REQUIRE(off + sizeof(Shot) <= sizeof(storage));
    Shot* shot = new (storage + off) Shot{};

    // 主线程先发第 1 帧:线程启动前 seq == 2,读方第一眼就有一帧完整数据可收。
    shot->publish(Traits::make(1));

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> published{1};
    RoundResult r;

    std::thread writer([&] {
        std::uint64_t k = 1;
        while (!stop.load(std::memory_order_relaxed))
        {
            ++k;
            shot->publish(Traits::make(k));
        }
        published.store(k, std::memory_order_relaxed);
    });

    std::thread reader([&] {
        const auto deadline = Clock::now() + kRoundDuration;
        std::uint64_t last = 0;
        std::uint64_t n = 0;
        Pod out{};
        for (;;)
        {
            if ((++n & 1023u) == 0 && Clock::now() >= deadline)
            {
                break;
            }
            const std::uint32_t s0 = shot->seq.load(std::memory_order_acquire);
            const bool ok = shot->read(out);
            const std::uint32_t s1 = shot->seq.load(std::memory_order_acquire);
            if (!ok)
            {
                ++r.rejected;
                continue;
            }
            ++r.accepted;
            const std::uint64_t k = Traits::indexOf(out);
            if (!Traits::equal(out, Traits::make(k)))
            {
                ++r.badSelf;
            }
            // seq 是 u32,本轮最多几千万次发布,不会回绕(回绕需要 2^31 帧)。
            const std::uint64_t lo = (static_cast<std::uint64_t>(s0) + 1u) / 2u;
            const std::uint64_t hi = static_cast<std::uint64_t>(s1) / 2u;
            if (k < lo || k > hi)
            {
                ++r.badBracket;
            }
            if (k < last)
            {
                ++r.badMonotonic;
            }
            last = k;
        }
        stop.store(true, std::memory_order_relaxed);
    });

    reader.join();
    writer.join();
    r.published = published.load(std::memory_order_relaxed);
    shot->~Shot();
    return r;
}

// ---- PlayheadShot:每个字段都是 k 的确定函数(double 全部精确可表示,== 比较即可)。
struct PlayheadTraits
{
    using Shot = scvb::engine::PlayheadShot;
    using Pod = scvb::engine::PlayheadPod;

    static Pod make(std::uint64_t k)
    {
        Pod p;
        p.timeSamples = static_cast<std::int64_t>(k);
        p.ppq = static_cast<double>(k) * 0.25;
        p.bpm = static_cast<double>(k % 997u) + 0.5;
        p.loopStartPpq = static_cast<double>(k) * 2.0;
        p.loopEndPpq = static_cast<double>(k) * 2.0 + 1.0;
        p.sampleRate = 44100.0 + static_cast<double>(k % 3u);
        p.epoch = static_cast<std::uint32_t>(k);
        p.flags = static_cast<std::uint32_t>(k * 2654435761u);
        p.timeSigNum = static_cast<std::int32_t>(k & 0x7fffu);
        p.timeSigDen = static_cast<std::int32_t>((k * 7u) & 0x7fffu);
        return p;
    }
    static std::uint64_t indexOf(const Pod& p) { return static_cast<std::uint64_t>(p.timeSamples); }
    static bool equal(const Pod& a, const Pod& b)
    {
        return a.timeSamples == b.timeSamples && a.ppq == b.ppq && a.bpm == b.bpm && a.loopStartPpq == b.loopStartPpq &&
               a.loopEndPpq == b.loopEndPpq && a.sampleRate == b.sampleRate && a.epoch == b.epoch &&
               a.flags == b.flags && a.timeSigNum == b.timeSigNum && a.timeSigDen == b.timeSigDen;
    }
};

// ---- MeterShot:全是 float。帧号拆成两段 20 位放在 trackRms[0..1](float 精确表示 < 2^24 的整数),
// 其余字段各取 (k * c + i) 的低 20 位 —— 相邻两帧在每个字段上都不同,混帧必露馅。
struct MeterTraits
{
    using Shot = scvb::output::MeterShot;
    using Pod = scvb::output::MeterPod;
    static constexpr std::uint64_t kMask = (1u << 20) - 1u;

    static float f(std::uint64_t v) { return static_cast<float>(v & kMask); }

    static Pod make(std::uint64_t k)
    {
        Pod p;
        p.trackRms[0] = f(k);
        p.trackRms[1] = f(k >> 20);
        for (int i = 2; i < scvb::output::kMeterTracks; ++i)
        {
            p.trackRms[i] = f(k * 3u + static_cast<std::uint64_t>(i));
        }
        for (int i = 0; i < scvb::output::kMeterTracks; ++i)
        {
            p.trackPeak[i] = f(k * 5u + static_cast<std::uint64_t>(i));
        }
        for (int c = 0; c < 2; ++c)
        {
            p.busRms[c] = f(k * 7u + static_cast<std::uint64_t>(c));
            p.busPeak[c] = f(k * 11u + static_cast<std::uint64_t>(c));
        }
        return p;
    }
    static std::uint64_t indexOf(const Pod& p)
    {
        return (static_cast<std::uint64_t>(p.trackRms[1]) << 20) | static_cast<std::uint64_t>(p.trackRms[0]);
    }
    static bool equal(const Pod& a, const Pod& b)
    {
        for (int i = 0; i < scvb::output::kMeterTracks; ++i)
        {
            if (a.trackRms[i] != b.trackRms[i] || a.trackPeak[i] != b.trackPeak[i])
            {
                return false;
            }
        }
        for (int c = 0; c < 2; ++c)
        {
            if (a.busRms[c] != b.busRms[c] || a.busPeak[c] != b.busPeak[c])
            {
                return false;
            }
        }
        return true;
    }
};

template<typename Traits>
void runAllPlacements()
{
    for (const Placement p : {Placement::kNatural, Placement::kSeqAloneAtLineEnd, Placement::kPodStraddlesLines})
    {
        const RoundResult r = runRound<Traits>(p);
        INFO("placement " << placementName(p) << ": published " << r.published << ", accepted " << r.accepted
                          << ", rejected " << r.rejected);
        // 先判「判据被违反」,再判非空洞:三条判据各自独立出结论(CHECK,不互相遮挡)。
        CHECK(r.badSelf == 0u);
        CHECK(r.badBracket == 0u);
        CHECK(r.badMonotonic == 0u);
        CHECK(r.accepted >= kMinAccepted);
        CHECK(r.published >= kMinPublished);
    }
}

} // namespace

TEST_CASE("PlayheadShot seqlock: concurrent publish/read never accepts a torn frame", "[.][arm][stress][seqlock]")
{
    runAllPlacements<PlayheadTraits>();
}

TEST_CASE("MeterShot seqlock: concurrent publish/read never accepts a torn frame", "[.][arm][stress][seqlock]")
{
    runAllPlacements<MeterTraits>();
}
