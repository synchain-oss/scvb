// SPDX-License-Identifier: GPL-3.0-or-later
// test_audio_ring_stress —— [B 线 M04b] AudioRing::write 相邻两笔之间的写侧顺序(arm64 内存序)双线程压测。
//
// 被测的是产品代码 AudioRing::write:写线程直接调它,不在本文件里复刻。读线程是读环判据的一份**复刻**,
// 只取 ShmRingMixSource(A 线文件,本卡不动)认当前窗的两条:
//   · 可读下沿 cLo = w + guard − R(w = acquire 读到的写头,R = 环长,guard = 在途保护);
//   · 规则 7 复核:拷完之后先 acquire 栅栏,再 relaxed 读写头 w2,要求 w2 + guard ≤ cLo + R,否则整块弃用。
// guard 取一笔 write() 的长度 m,这是写方协议承诺的最紧上界:写头之后至多一笔在写。产品里的
// inflightGuard() 是观测到的最大步长(≥ 一笔),比这里宽;这里取最紧的一档,钉的是写侧承诺本身,
// 不是读方的估计。
//
// 读方每次只读可读区间最下沿的 m 帧 [cLo, cLo + m)。它们与写头之后第 2 笔(时间线位置 [w + m, w + 2m))
// 同槽,第 1 笔(正在写的那一笔,[w, w + m))则恰好被 guard 挡在区间之外:
//   · 写侧顺序成立时,读方看到第 2 笔的任何一个样本,拷完复核就必然读到写头 ≥ w + m,复核不过、整块弃用;
//   · 写侧缺「上一笔写头先于本笔样本」这条顺序时,arm64 允许第 2 笔的样本先于第 1 笔的写头可见。于是读方
//     复核仍读到 w,收下的却是新一圈的数据。
// 每个样本的值编码它的时间线位置(模 4 圈),所以收下的每一块可以逐帧判:
//   ① 新一圈:值是同槽晚一到两圈的位置写下的。这一条专门给写侧 fence 长牙;
//   ② 旧一圈:值是同槽早一圈的位置写下的,即 acquire 读到写头之后仍看到旧数据(读侧失效);
//   ③ 其他:值不属于这个槽,或不是整数(撕开 / 越界)。
// 非空洞,三个下限:收下块数、写方笔数,以及「复核不过、而且拷到了新一圈数据」的块数(raced)。raced 证明
// 读方确实在和写方抢同一批槽:写方真的在读方拷贝的同时写进了这几个槽,复核也确实把它们挡了下来。
// 四格配置覆盖两种摆放与两种节拍(见 kConfigs):
//   · 段内摆放:样本紧跟段头,与真实段相同,写头和环首几帧在同一缓存行;另一种是段头独占缓存行;
//   · 写方连续写,或每 4 笔停一下(模拟一块按段连写几笔、然后等下一块)。
//
// 能抓到什么、抓不到什么(如实写):
//   · x86-64 是 TSO,写写不重排,写侧 fence 在那里只是编译器屏障 —— 本文件在 Windows 上恒绿,删掉
//     AudioRing::write 开头那道 fence 也绿。它在 x86 上只证明判据不误报;判据会不会响,由源码层的
//     反向注入核(写头晚发一笔,直接造出 arm64 上要防的那种可见顺序;结果见本卡 PR 描述的验证表)。
//   · arm64 的实测次数见本卡 PR 描述(macos-15 runner)。M04 的同形实验(seqlock 写侧 release RMW 之后的
//     普通写,删 fence)在那台硬件上 ×30 抓不到(#366);本压测在 arm64 上能不能抓到写侧缺失,以 PR 里的实测为准。
// 断言纪律([SL-453]):工作线程里不放断言宏,只计数;join 之后在主线程断言。
// 默认不跑(隐藏标签 [.]):与 M04 的 seqlock 压测同由独立 ctest 条目 scvb_seqlock_stress 单跑
// (同一个二进制按 `[stress]` 选,见 tests/CMakeLists.txt)。手跑:scvb_tests "[audioring][stress]"。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <thread>
#include <vector>

#include "ipc/AudioRing.h"

namespace
{

using Clock = std::chrono::steady_clock;
using scvb::u32;
using scvb::u64;

constexpr u32 kRing = 1024; // 环长 R(帧,2^k)。小环:读写两侧反复碰同几条缓存行
constexpr u32 kRingMask = kRing - 1;
constexpr u32 kLaps = 4; // 样本值编码「时间线位置 mod (kLaps × R)」:能分出晚 1、2 圈与早 1 圈
constexpr u32 kSpan = kLaps * kRing;
constexpr std::size_t kLine = 128; // Apple M 系列缓存行 128B(对 64B 行同样成立)

// 每格读侧时长。四格合计约 0.6s(上界说明见 tests/CMakeLists.txt 的 scvb_seqlock_stress)。
constexpr auto kRoundDuration = std::chrono::milliseconds(150);
// 非空洞下限:读方至少收下这么多块、写方至少写这么多笔,否则这一格等于没测。
// 收下块数的下限低于 M04 那套:读方拷的正是写方下一笔要写的槽,连续写的几格里大多数块都会被复核挡下
// (本机 x86 实测 ×5:每格 150ms 收下约 5000–220000 块、raced 约 1300–480000,见 PR 描述)。
constexpr std::uint64_t kMinAccepted = 200;
constexpr std::uint64_t kMinWrites = 1000;
constexpr std::uint64_t kMinRaced = 100;

enum class Layout
{
    kSegment, // 样本紧跟段头(sizeof(AudioRingHeader) 之后),与 InputSession 建的真实段相同
    kSeparateLine, // 段头独占缓存行,样本从下下一行开始
};

struct Config
{
    const char* name;
    Layout layout;
    u32 channels;
    int m; // 每笔 write() 的帧数(= 读方的 guard)
    int burst; // 0 = 连续写;> 0 = 每写这么多笔停一下
    int pauseSpins; // 停多久(自旋次数)
};

constexpr Config kConfigs[] = {
    {"segment-mono-m16", Layout::kSegment, 1, 16, 0, 0},
    {"segment-stereo-m64", Layout::kSegment, 2, 64, 0, 0},
    {"separate-mono-m8", Layout::kSeparateLine, 1, 8, 0, 0},
    {"separate-stereo-m256-burst4", Layout::kSeparateLine, 2, 256, 4, 20000},
};

struct RoundResult
{
    std::uint64_t writes = 0; // 写方 write() 笔数(不含预填)
    std::uint64_t accepted = 0; // 复核通过的块
    std::uint64_t rejected = 0; // 复核不过的块(按协议弃用)
    std::uint64_t raced = 0; // 复核不过、且拷到了新一圈数据的块(非空洞:读方确实在和写方抢槽)
    std::uint64_t badNewer = 0; // 判据 ①
    std::uint64_t badOlder = 0; // 判据 ②
    std::uint64_t badOther = 0; // 判据 ③
};

// 时间线位置 p 的第 c 个声道写下的值:精确可表示的小整数(< 2^24)。
float encode(u64 p, u32 c, u32 ch)
{
    return static_cast<float>(static_cast<u32>(p % kSpan) * ch + c);
}

struct Verdict
{
    bool newer = false;
    bool older = false;
    bool other = false;
};

// 逐帧判一块 [lo, lo + n):每个样本属于本圈 / 晚 1–2 圈 / 早 1 圈 / 都不是。
Verdict classify(const std::vector<float>& buf, std::int64_t lo, int n, u32 ch)
{
    Verdict v;
    for (int i = 0; i < n; ++i)
    {
        const u32 want = static_cast<u32>(static_cast<u64>(lo + i) % kSpan);
        for (u32 c = 0; c < ch; ++c)
        {
            const float x = buf[static_cast<std::size_t>(i) * ch + c];
            if (!(x >= 0.0f && x < static_cast<float>(kSpan * ch))) // 先判范围再转整数(越界转换是未定义行为)
            {
                v.other = true;
                continue;
            }
            const u32 q = static_cast<u32>(x);
            if (static_cast<float>(q) != x || q % ch != c)
            {
                v.other = true;
                continue;
            }
            const u32 diff = (q / ch - want) & (kSpan - 1); // 晚了多少帧(模 kSpan)
            if (diff % kRing != 0)
            {
                v.other = true; // 不是同一个槽写下的
            }
            else if (diff / kRing == kLaps - 1)
            {
                v.older = true; // ≡ 早 1 圈
            }
            else if (diff != 0)
            {
                v.newer = true; // 晚 1 或 2 圈
            }
        }
    }
    return v;
}

RoundResult runRound(const Config& cfg)
{
    const u32 ch = cfg.channels;
    const int m = cfg.m;
    const std::int64_t guard = m;
    const int n = m; // 读方每块读的帧数:写头之后第 2 笔的那几个槽
    static_assert(kSpan % 256 == 0, "every write size must divide the encoding span");

    alignas(256) static unsigned char storage[2 * kLine + static_cast<std::size_t>(kRing) * 2 * sizeof(float)];
    auto* hdr = new (storage) scvb::AudioRingHeader{};
    const std::size_t dataOff = cfg.layout == Layout::kSegment ? sizeof(scvb::AudioRingHeader) : 2 * kLine;
    float* data = reinterpret_cast<float*>(storage + dataOff);
    std::fill(data, data + static_cast<std::size_t>(kRing) * 2, -1.0f);
    hdr->magic.store(scvb::kScvbMagic, std::memory_order_relaxed);
    hdr->abi.store(scvb::kScvbAbi, std::memory_order_relaxed);
    hdr->sample_rate = 48000;
    hdr->ring_frames = kRing;
    hdr->channels = ch;
    hdr->write_head_samples.store(0, std::memory_order_relaxed);
    hdr->epoch.store(0, std::memory_order_relaxed);

    scvb::AudioRing ring;
    ring.bind(hdr, data);
    const scvb::AudioRingBinding* b = ring.acquire();

    // 写方的源:时间线位置 mod kSpan 的编码值,预先算好,写线程里只剩 write() 本身。
    std::vector<float> src(static_cast<std::size_t>(kSpan) * ch);
    for (u32 p = 0; p < kSpan; ++p)
    {
        for (u32 c = 0; c < ch; ++c)
        {
            src[static_cast<std::size_t>(p) * ch + c] = encode(p, c, ch);
        }
    }
    const auto srcAt = [&](std::int64_t t) {
        return src.data() + static_cast<std::size_t>(static_cast<u64>(t) % kSpan) * ch;
    };

    // 线程启动前先写满两圈:读方第一眼就有完整的可读区间。
    std::int64_t t0 = 0;
    for (; t0 < 2 * static_cast<std::int64_t>(kRing); t0 += m)
    {
        scvb::AudioRing::write(b, t0, srcAt(t0), m);
    }

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> writes{0};
    RoundResult r;

    std::thread writer([&] {
        std::int64_t t = t0;
        std::uint64_t k = 0;
        const int chunk = cfg.burst > 0 ? cfg.burst : 64;
        while (!stop.load(std::memory_order_relaxed))
        {
            for (int j = 0; j < chunk; ++j)
            {
                scvb::AudioRing::write(b, t, srcAt(t), m);
                t += m;
            }
            k += static_cast<std::uint64_t>(chunk);
            for (int s = 0; s < cfg.pauseSpins && !stop.load(std::memory_order_relaxed); ++s)
            {
            }
        }
        writes.store(k, std::memory_order_relaxed);
    });

    std::thread reader([&] {
        const auto deadline = Clock::now() + kRoundDuration;
        std::vector<float> buf(static_cast<std::size_t>(n) * ch);
        std::uint64_t iter = 0;
        for (;;)
        {
            if ((++iter & 255u) == 0 && Clock::now() >= deadline)
            {
                break;
            }
            const std::int64_t w = static_cast<std::int64_t>(hdr->write_head_samples.load(std::memory_order_acquire));
            const std::int64_t lo = w + guard - static_cast<std::int64_t>(kRing);
            for (int i = 0; i < n; ++i)
            {
                const u32 slot = static_cast<u32>(static_cast<u64>(lo + i)) & kRingMask;
                for (u32 c = 0; c < ch; ++c)
                {
                    buf[static_cast<std::size_t>(i) * ch + c] = data[static_cast<std::size_t>(slot) * ch + c];
                }
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            const std::int64_t w2 = static_cast<std::int64_t>(hdr->write_head_samples.load(std::memory_order_relaxed));
            const bool ok = w2 + guard <= lo + static_cast<std::int64_t>(kRing);
            const Verdict v = classify(buf, lo, n, ch);
            if (!ok)
            {
                ++r.rejected;
                if (v.newer)
                {
                    ++r.raced;
                }
                continue;
            }
            ++r.accepted;
            r.badNewer += v.newer ? 1u : 0u;
            r.badOlder += v.older ? 1u : 0u;
            r.badOther += v.other ? 1u : 0u;
        }
        stop.store(true, std::memory_order_relaxed);
    });

    reader.join();
    writer.join();
    r.writes = writes.load(std::memory_order_relaxed);
    ring.bind(nullptr, nullptr);
    hdr->~AudioRingHeader();
    return r;
}

} // namespace

TEST_CASE("AudioRing write: back-to-back writes never surface a newer lap inside the guarded window",
          "[.][arm][stress][audioring]")
{
    for (const Config& cfg : kConfigs)
    {
        const RoundResult r = runRound(cfg);
        INFO("config " << cfg.name << ": writes " << r.writes << ", accepted " << r.accepted << ", rejected "
                       << r.rejected << ", raced " << r.raced << ", newer " << r.badNewer << ", older " << r.badOlder
                       << ", other " << r.badOther);
        // 先判「判据被违反」,再判非空洞:各条独立出结论(CHECK,不互相遮挡)。
        CHECK(r.badNewer == 0u);
        CHECK(r.badOlder == 0u);
        CHECK(r.badOther == 0u);
        CHECK(r.accepted >= kMinAccepted);
        CHECK(r.writes >= kMinWrites);
        CHECK(r.raced >= kMinRaced);
    }
}
