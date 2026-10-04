// SPDX-License-Identifier: GPL-3.0-or-later
// test_m04_arm64_experiment —— [B 线 M04] PR #366 的 arm64 临时实验用例(**不合入**,做完即 revert)。
//
// 按统筹裁定(选项 A 的执行方式):只在 PR 自身分支上「临时实验 commit → push → 读 macos-build 日志 →
// git revert」,不推新分支、不改任何 workflow。重复次数写在用例里:场景在同一个进程里循环 N 次,
// 每次的计数往 stderr 打一行(崩溃时也留得下),失败的那几次另用 WARN 打详情,最后用 CHECK 判
// 「失败次数 == 0」(CHECK 不用 REQUIRE:各条判据各自出结论,不互相遮挡)。
// 用例打 [.][stress]:只由 ctest 条目 scvb_seqlock_stress(scvb_tests "[stress]",TIMEOUT 临时 240)跑到,
// 不进 scvb_tests 那一条。非空洞条件也写成 CHECK(过 = 每一次都真的测到了)。
// **可见性**:ctest 只在条目失败时贴输出(--output-on-failure),绿的实验在 job 日志里只剩一行「Passed」,
// 连「到底跑没跑、跑了几次」都看不见(E1 第一次 push 就是这样)。所以文件末尾另有一条**故意失败**的
// 「canary」用例:它让条目必红、ctest 必贴整段输出,于是每个变体的 stderr 计数与 WARN 汇总都留在日志里。
// 实验结论看的是**实验用例自己**的 CHECK(Catch2 汇总里那一条是否失败),不看 job 颜色。
//
// 变体由 M04_EXP 选。每个实验 commit 只改这一个数,外加该变体在产品代码上的那一处注入:
//   1 = E1:修后(当前 head)「SegmentHandle 并发 lease/release」新建模 ×40
//   2 = E2:PlayheadShot::publish 写侧改回旧写法(奇数增量 release RMW、无 fence),PlayheadShot 压测 ×30
//   3 = E3:租约握手改回旧内存序(acq_rel / acquire / release / acquire)+ 新建模 ×40
//   4 = E4:保留 seq_cst + 旧建模(无静默点、无持有跨届满)×40
// 1 / 3 / 4 的场景逐字照抄 tests/core/test_ipc_lifecycle.cpp(新建模 = 本 PR head;旧建模 = 51d1386),
// 只把断言换成计数;2 的压测逐字照抄 tests/core/test_seqlock_stress.cpp 的 runRound / PlayheadTraits。
// 运行期字符串只用 ASCII(CP936 本机 C4819)。

#define M04_EXP 1

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#if M04_EXP == 1 || M04_EXP == 3 || M04_EXP == 4
#include <utility>
#include <vector>

#include "ipc/Registry.h"
#include "ipc/SegmentBackendInProcess.h"
#endif

#if M04_EXP == 2
#include <cstddef>
#include <initializer_list>
#include <new>
#include <type_traits>

#include "engine/PlayheadShot.h"
#endif

namespace
{

#if M04_EXP == 1 || M04_EXP == 3 || M04_EXP == 4
using scvb::u64;

struct LeaseResult
{
    u64 createFailures = 0;
    u64 initFailures = 0;
    u64 leases = 0;
    u64 nullBaseLeases = 0; // 原 test_ipc_lifecycle.cpp:1013 那条断言的计数
    u64 heldAcrossGrace = 0; // 只有新建模才有
    u64 cleanupFailures = 0; // 收尾释放的那几条 REQUIRE
};

// publish():逐字照抄原用例的 lambda,只把它提成函数。
scvb::SegmentHandle publishSegment(scvb::SegmentBackendInProcess& backend, std::atomic<u64>& createFailures,
                                   std::atomic<u64>& initFailures)
{
    scvb::SegmentView v;
    if (backend.createOrOpen(L"Local\\SynchainSCVB.v1.g1.registry", scvb::kRegistrySegmentSize, v) !=
        scvb::InitResult::kOk)
    {
        ++createFailures;
        return scvb::SegmentHandle{};
    }
    auto* hdr = static_cast<scvb::RegistryHeader*>(v.base);
    if (backend.initHeader(v, &hdr->magic, &hdr->abi, &hdr->generation, sizeof(scvb::RegistryHeader)) !=
        scvb::InitResult::kOk)
    {
        ++initFailures;
    }
    return scvb::SegmentHandle(std::move(v), &backend);
}

// 收尾:逐字照抄原用例 join 之后的那几条 REQUIRE,换成计数。
void cleanupHandles(std::vector<scvb::SegmentHandle>& pending, scvb::SegmentHandle& handle, LeaseResult& r)
{
    const u64 finalNowMs = 1000000000;
    for (auto& h : pending)
    {
        h.release(finalNowMs);
        if (!h.release(finalNowMs + scvb::SegmentHandle::kReleaseGraceMs))
        {
            ++r.cleanupFailures;
        }
    }
    if (handle.release(finalNowMs))
    {
        ++r.cleanupFailures;
    }
    if (!handle.release(finalNowMs + scvb::SegmentHandle::kReleaseGraceMs))
    {
        ++r.cleanupFailures;
    }
    if (handle.valid())
    {
        ++r.cleanupFailures;
    }
}
#endif

#if M04_EXP == 1 || M04_EXP == 3
// 新建模(本 PR head 的 test_ipc_lifecycle.cpp:「静默点」+「持有跨届满」)。
LeaseResult runLeaseScenario()
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;
    LeaseResult r;
    std::atomic<u64> createFailures{0};
    std::atomic<u64> initFailures{0};

    scvb::SegmentHandle handle = publishSegment(backend, createFailures, initFailures);
    if (createFailures.load() != 0 || initFailures.load() != 0)
    {
        r.createFailures = createFailures.load();
        r.initFailures = initFailures.load();
        return r;
    }
    std::vector<scvb::SegmentHandle> pending;

    std::atomic<bool> stop{false};
    std::atomic<u64> leases{0};
    std::atomic<u64> nullBaseLeases{0};

    constexpr u64 kHoldEvery = 8;
    constexpr u64 kHoldTicks = 7;
    std::atomic<u64> attemptPhase{0};
    std::atomic<u64> messageTicks{0};
    std::atomic<u64> heldAcrossGrace{0};

    std::thread audio([&] {
        u64 granted = 0;
        while (!stop.load(std::memory_order_acquire))
        {
            attemptPhase.fetch_add(1, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            auto lease = handle.lease();
            attemptPhase.fetch_add(1, std::memory_order_seq_cst);
            if (lease)
            {
                if (++granted % kHoldEvery == 0)
                {
                    const u64 start = messageTicks.load(std::memory_order_acquire);
                    while (messageTicks.load(std::memory_order_acquire) < start + kHoldTicks &&
                           !stop.load(std::memory_order_acquire))
                    {
                        std::this_thread::yield();
                    }
                    if (messageTicks.load(std::memory_order_acquire) >= start + kHoldTicks)
                    {
                        ++heldAcrossGrace;
                    }
                }
                if (lease.base() == nullptr)
                {
                    ++nullBaseLeases;
                }
                ++leases;
            }
        }
    });

    std::thread message([&] {
        u64 nowMs = 0;
        for (int i = 0; i < 3000; ++i)
        {
            nowMs += 100;
            if (!handle.release(nowMs))
            {
                pending.push_back(std::move(handle));
            }
            handle = publishSegment(backend, createFailures, initFailures);
            if (createFailures.load() != 0 || initFailures.load() != 0)
            {
                break;
            }
            for (auto it = pending.begin(); it != pending.end();)
            {
                if (it->release(nowMs))
                {
                    it = pending.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
            const u64 phase = attemptPhase.load(std::memory_order_seq_cst);
            if ((phase & 1u) != 0u)
            {
                while (attemptPhase.load(std::memory_order_acquire) == phase)
                {
                    std::this_thread::yield();
                }
            }
            messageTicks.fetch_add(1, std::memory_order_release);
        }
        stop.store(true, std::memory_order_release);
    });

    audio.join();
    message.join();

    r.createFailures = createFailures.load();
    r.initFailures = initFailures.load();
    r.leases = leases.load();
    r.nullBaseLeases = nullBaseLeases.load();
    r.heldAcrossGrace = heldAcrossGrace.load();
    cleanupHandles(pending, handle, r);
    return r;
}
#endif

#if M04_EXP == 4
// 旧建模(51d1386 的 test_ipc_lifecycle.cpp:没有静默点、没有持有跨届满)。
LeaseResult runLeaseScenario()
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;
    LeaseResult r;
    std::atomic<u64> createFailures{0};
    std::atomic<u64> initFailures{0};

    scvb::SegmentHandle handle = publishSegment(backend, createFailures, initFailures);
    if (createFailures.load() != 0 || initFailures.load() != 0)
    {
        r.createFailures = createFailures.load();
        r.initFailures = initFailures.load();
        return r;
    }
    std::vector<scvb::SegmentHandle> pending;

    std::atomic<bool> stop{false};
    std::atomic<u64> leases{0};
    std::atomic<u64> nullBaseLeases{0};

    std::thread audio([&] {
        while (!stop.load(std::memory_order_acquire))
        {
            auto lease = handle.lease();
            if (lease)
            {
                if (lease.base() == nullptr)
                {
                    ++nullBaseLeases;
                }
                ++leases;
            }
        }
    });

    std::thread message([&] {
        u64 nowMs = 0;
        for (int i = 0; i < 3000; ++i)
        {
            nowMs += 100;
            if (!handle.release(nowMs))
            {
                pending.push_back(std::move(handle));
            }
            handle = publishSegment(backend, createFailures, initFailures);
            if (createFailures.load() != 0 || initFailures.load() != 0)
            {
                break;
            }
            for (auto it = pending.begin(); it != pending.end();)
            {
                if (it->release(nowMs))
                {
                    it = pending.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        stop.store(true, std::memory_order_release);
    });

    audio.join();
    message.join();

    r.createFailures = createFailures.load();
    r.initFailures = initFailures.load();
    r.leases = leases.load();
    r.nullBaseLeases = nullBaseLeases.load();
    cleanupHandles(pending, handle, r);
    return r;
}
#endif

#if M04_EXP == 2
// ---- 逐字照抄 tests/core/test_seqlock_stress.cpp(只留 PlayheadShot 需要的部分)。
using Clock = std::chrono::steady_clock;

constexpr auto kRoundDuration = std::chrono::milliseconds(100);
constexpr std::uint64_t kMinAccepted = 1000;
constexpr std::uint64_t kMinPublished = 1000;

enum class Placement
{
    kNatural,
    kSeqAloneAtLineEnd,
    kPodStraddlesLines,
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

std::size_t placementOffset(Placement p, std::size_t podOffset)
{
    switch (p)
    {
    case Placement::kNatural:
        return 0;
    case Placement::kSeqAloneAtLineEnd:
        return kLine - podOffset;
    case Placement::kPodStraddlesLines:
        return kLine - podOffset - 24;
    }
    return 0;
}

struct RoundResult
{
    std::uint64_t published = 0;
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t badSelf = 0;
    std::uint64_t badBracket = 0;
    std::uint64_t badMonotonic = 0;
};

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
#endif

} // namespace

#if M04_EXP == 1 || M04_EXP == 3 || M04_EXP == 4
TEST_CASE("M04 experiment: SegmentHandle concurrent lease/release looped", "[.][stress][m04-exp]")
{
    constexpr int kIterations = 40;
#if M04_EXP == 1
    const char* const label = "E1 head (seq_cst handshake + quiescent/hold model)";
#elif M04_EXP == 3
    const char* const label = "E3 old handshake orders + quiescent/hold model";
#else
    const char* const label = "E4 seq_cst handshake + old model (no quiescent point, no hold)";
#endif
    int itersNullBase = 0; // :1013 那条断言会红的次数
    int itersOtherFailure = 0; // create / init / 收尾释放 任一不对
    int itersNoLease = 0; // leases == 0(那一次等于没测)
    int itersNotHeld = 0; // 新建模:heldAcrossGrace == 0(那一次没测到「持有跨届满」)
    u64 totalNullBase = 0;
    u64 totalLeases = 0;
    u64 totalHeld = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 1; i <= kIterations; ++i)
    {
        const auto ti = std::chrono::steady_clock::now();
        const LeaseResult r = runLeaseScenario();
        const auto us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - ti).count();
        std::fprintf(stderr,
                     "[m04-exp] %s iter %d/%d (%lld us): leases=%llu nullBase=%llu heldAcrossGrace=%llu create=%llu "
                     "init=%llu cleanup=%llu\n",
                     label, i, kIterations, static_cast<long long>(us), static_cast<unsigned long long>(r.leases),
                     static_cast<unsigned long long>(r.nullBaseLeases),
                     static_cast<unsigned long long>(r.heldAcrossGrace),
                     static_cast<unsigned long long>(r.createFailures), static_cast<unsigned long long>(r.initFailures),
                     static_cast<unsigned long long>(r.cleanupFailures));
        totalNullBase += r.nullBaseLeases;
        totalLeases += r.leases;
        totalHeld += r.heldAcrossGrace;
        if (r.nullBaseLeases != 0)
        {
            ++itersNullBase;
            WARN("[m04-exp] " << label << " iter " << i << ": nullBaseLeases = " << r.nullBaseLeases << " (leases "
                              << r.leases << ")");
        }
        if (r.createFailures != 0 || r.initFailures != 0 || r.cleanupFailures != 0)
        {
            ++itersOtherFailure;
        }
        if (r.leases == 0)
        {
            ++itersNoLease;
        }
#if M04_EXP != 4
        if (r.heldAcrossGrace == 0)
        {
            ++itersNotHeld;
        }
#endif
    }
    const auto totalMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    WARN("[m04-exp] " << label << " SUMMARY (" << totalMs << " ms): nullBase-red iterations " << itersNullBase << " / "
                      << kIterations << "; other failures " << itersOtherFailure << "; no-lease iterations "
                      << itersNoLease << "; not-held iterations " << itersNotHeld << "; total nullBase "
                      << totalNullBase << ", total leases " << totalLeases << ", total heldAcrossGrace " << totalHeld);
    CHECK(itersNullBase == 0);
    CHECK(itersOtherFailure == 0);
    CHECK(itersNoLease == 0);
    CHECK(itersNotHeld == 0);
}
#endif

#if M04_EXP == 2
TEST_CASE("M04 experiment: PlayheadShot seqlock stress looped", "[.][stress][m04-exp]")
{
    constexpr int kIterations = 30;
    int itersRed = 0; // 任一摆放上任一判据(自洽 / 括号 / 单调)> 0
    int itersVacuous = 0; // 任一摆放上 accepted / published 低于下限
    std::uint64_t totalSelf[3] = {0, 0, 0};
    std::uint64_t totalBracket[3] = {0, 0, 0};
    std::uint64_t totalMono[3] = {0, 0, 0};
    int redByPlacement[3] = {0, 0, 0};
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 1; i <= kIterations; ++i)
    {
        bool red = false;
        bool vacuous = false;
        int idx = 0;
        for (const Placement p : {Placement::kNatural, Placement::kSeqAloneAtLineEnd, Placement::kPodStraddlesLines})
        {
            const RoundResult r = runRound<PlayheadTraits>(p);
            std::fprintf(stderr,
                         "[m04-exp] E2 iter %d/%d %s: published=%llu accepted=%llu rejected=%llu badSelf=%llu "
                         "badBracket=%llu badMonotonic=%llu\n",
                         i, kIterations, placementName(p), static_cast<unsigned long long>(r.published),
                         static_cast<unsigned long long>(r.accepted), static_cast<unsigned long long>(r.rejected),
                         static_cast<unsigned long long>(r.badSelf), static_cast<unsigned long long>(r.badBracket),
                         static_cast<unsigned long long>(r.badMonotonic));
            totalSelf[idx] += r.badSelf;
            totalBracket[idx] += r.badBracket;
            totalMono[idx] += r.badMonotonic;
            if (r.badSelf != 0 || r.badBracket != 0 || r.badMonotonic != 0)
            {
                red = true;
                ++redByPlacement[idx];
                WARN("[m04-exp] E2 iter " << i << " " << placementName(p) << ": badSelf " << r.badSelf
                                          << ", badBracket " << r.badBracket << ", badMonotonic " << r.badMonotonic
                                          << " (accepted " << r.accepted << ", rejected " << r.rejected << ")");
            }
            if (r.accepted < kMinAccepted || r.published < kMinPublished)
            {
                vacuous = true;
            }
            ++idx;
        }
        if (red)
        {
            ++itersRed;
        }
        if (vacuous)
        {
            ++itersVacuous;
        }
    }
    const auto totalMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    WARN("[m04-exp] E2 SUMMARY (" << totalMs << " ms): red iterations " << itersRed << " / " << kIterations
                                  << "; vacuous iterations " << itersVacuous << "; red rounds natural "
                                  << redByPlacement[0] << ", seq-alone " << redByPlacement[1] << ", straddles "
                                  << redByPlacement[2] << "; badSelf totals " << totalSelf[0] << "/" << totalSelf[1]
                                  << "/" << totalSelf[2] << "; badBracket totals " << totalBracket[0] << "/"
                                  << totalBracket[1] << "/" << totalBracket[2] << "; badMonotonic totals "
                                  << totalMono[0] << "/" << totalMono[1] << "/" << totalMono[2]);
    CHECK(itersRed == 0);
    CHECK(itersVacuous == 0);
}
#endif

// 故意失败的 canary(理由见文件头「可见性」):只为让 ctest 贴出本条目的完整输出。实验结论看上面那条用例。
TEST_CASE("M04 experiment: zz canary (fails on purpose so ctest prints the counts above)", "[.][stress][m04-exp]")
{
    FAIL_CHECK("[m04-exp] canary: intentional failure so that ctest --output-on-failure prints this run; the verdict "
               "is the experiment test case, not this one");
}
