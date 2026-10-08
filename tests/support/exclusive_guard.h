// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// [SL-324] **同机只允许一份 SCVB 测试进程** —— 四套二进制共用同一把命名互斥,
// 在整轮开始前判定,拿不到就整轮不跑。
//
// ## 为什么四套共用一把,而不是各自一把
//
// 固定组号在四套之间**大面积重叠**,八个组没有一个是独占的(SL-324 三通道枚举):
//
//     g1  scvb_tests(test_ipc_lifecycle) / host(MonoMultiRig 经越界回落) / ipc(--group=1 ×37 + resetRegistry ×6)
//     g2  scvb_tests(output/input_session) / monitor(kGroupA) / ipc
//     g3  ipc(kGroup、--group=3、segmentRegistryName(3)) / host(本卡把 MonoMultiRig 迁到这里)
//     g4  monitor(kGroupA) / host
//     g5  monitor(kGroupB) / host(kFromGroup)
//     g6  monitor(kGroup) / host(kOtherGroup、kToGroup) / ipc(kProbeGroup)
//     g7  monitor(kGroup,还 spawn viz-publisher 真建 g7.viz) / host(kTestGroup) / ipc(VIZ-2)
//     g8  host(kNoWriterGroup) / ipc(--group=8 claimer)
//
// 段名 `SynchainSCVB.v1.g{G}.…` 是**全机唯一**的,所以「每套一把锁」挡得住同套并发、
// **挡不住跨套件**:#204 给 host、#206 给 ipc 各加一把之后,ipc 的 VIZ-2 仍会被 host 或
// monitor 打红(实测过:VIZ-2 红时机器上正跑着 host 套件)。一把共用的锁一次性消掉全部重叠。
//
// 代价是**串行化所有测试进程**。这不新增等待:`scripts/with-ipc-lock.ps1`(SL-311)本来就在
// 做同一件事,区别只在于它依赖人记得走包装,而这把锁**不依赖任何人记得**。
//
// ## 为什么判定必须在整轮开始前
//
// Catch2 的 `FAIL` 用 `ResultDisposition::Normal` —— **只中止当前用例,不中止整轮**
// (#204 复审的结论)。守卫若挂在某个 fixture 上,第二个进程仍会跑完整轮、照常建段,
// **它仍在主动加害对方**。只有在 `testRunStarting` 里判、拿不到就 `std::exit`,
// 才能保证**一个段都不建**。
//
// ## 三条有意的边界
//
//   · **0 等待、不排队**:排队会把冲突藏起来,而且与 gates 的 `Local\SCVB-ipc-tests`
//     形成锁序风险(那把是 gates 自己串行 gate 3e 与 6/7/8 用的,与这把语义不同);
//   · **不碰 IPC 段**:`Registry::open()` 是 `createOrOpen` + `allowOverwrite=true`,
//     拿它当探针会创建、甚至覆盖式重初始化别人正在用的 registry —— 比它要治的问题更坏;
//   · **不看槽位**:「槽位活跃且 pid 非我」会被崩溃进程留下的**陈旧槽位**误报。
//
// 不手动释放:内核对象随进程退出自动回收,测试里放反而多一条出错路径。
//
// ## 用法
//
// 每个测试二进制在**任意一个** .cpp 里 `#include "support/exclusive_guard.h"` 即可
// (需要 `${CMAKE_SOURCE_DIR}/tests` 在 include 路径上)。取锁走函数内 static,
// **每进程至多一次** —— 即便某个二进制不小心在两个 TU 里都包含了本头,
// 也不会出现「自己把自己判成占用者」。
//
// 运行期文案一律 **ASCII**:中文字面量在本机 CP936 上会触发 MSVC C4819,而 ADR-011
// 要求 /W4 零 warning(SL-323 实测,归因见 SL-325)。中文只写在注释里。

// windows.h 的 min/max 宏会污染 `std::max` / `std::numeric_limits<T>::min()`;先禁再包含
//(与 `src/core/ipc/PlatformSegmentBackend.h` 的 Windows 分支同款)。
//
// ⚠ **这道 `#ifndef NOMINMAX` 是包含顺序相关的,它兜不住所有情况** ——
// 同 TU 里只要有**更早**的 `<windows.h>`,宏污染已经既成事实,这里再 define 也来不及。
// 所以本头的使用约束是:
//   · **把它排在任何会拉入 `<windows.h>` 的头之前**(host 侧就是这么放的:
//     守卫头在 `BridgeArgs.h` 之前);
//   · 若该 TU **自己更早**包含了 `<windows.h>`,则须由**它自己** `#define NOMINMAX`
//     —— `tests/ipc/test_ipc_contract.cpp` 正是这一种(它在文件顶部自定义了 NOMINMAX
//     再包含 windows.h,本头排在其后;那边没出事是**因为它自己定义了**,不是本头兜住的)。
// 这条顺序约束**没有机器保证**:把本头的 include 往下挪一行、或上面某个头开始拉
// windows.h,就会复现 `error C2062: 意外的类型「unknown-type」`,而且报在
// `src/output/BridgeArgs.h` 那种**别人的文件**上,极难联想到是这里引起的(实测过)。
//
// [B 线 M06a] 下面整段实现只在 Windows 上编(命名互斥体 `CreateMutexW`)。非 Windows 分支见文件末尾
// ([B 线 M12a] 起是真守卫:按 uid 分文件的 flock,见那里的注释)。
#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

// [SL-324] 把上面那条顺序约束**变成机器保证**(复审建议):走到这里时,`min`/`max` 只可能
// 由**更早的、未禁宏的** `<windows.h>` 定义 —— 我们自己那次包含已被上面的 NOMINMAX 挡住。
// 与其让它稍后在 `src/output/BridgeArgs.h` 那种**别人的文件**上报 C2062,不如在**这里**报,
// 错误信息直接给出处方。
#if defined(min) || defined(max)
#error \
    "[SL-324] windows.h min/max macros already defined: include support/exclusive_guard.h before any header that pulls in <windows.h>, or #define NOMINMAX in this TU first (see tests/ipc/test_ipc_contract.cpp)."
#endif

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstdio>
#include <cstdlib>

namespace scvb::testsupport
{

// 取锁结果:owned = 本进程独占;handle == nullptr 表示**建不出来**(与「已被占用」是两回事)。
struct ExclusiveAcquire
{
    HANDLE handle = nullptr;
    DWORD lastError = 0;
    bool owned = false;
};

// 每进程至多取一次(函数内 static)。句柄故意不关:活到进程退出,由内核回收。
inline const ExclusiveAcquire& acquireTestsExclusiveOnce()
{
    static const ExclusiveAcquire result = [] {
        ExclusiveAcquire r;
        r.handle = ::CreateMutexW(nullptr, TRUE, L"Local\\SCVB-tests-proc");
        // `GetLastError()` 必须**紧挨着**取:中间插任何一个 Win32 调用都可能把它冲掉。
        r.lastError = ::GetLastError();
        r.owned = (r.handle != nullptr && r.lastError != ERROR_ALREADY_EXISTS);
        return r;
    }();
    return result;
}

struct TestsExclusiveListener : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;

    // `TestRunInfo::name` 是 `Catch::StringRef` —— **没有 `c_str()`,也不保证 NUL 结尾**,
    // 所以用 `%.*s` + `size()/data()`,不要 `%s`(那是编译错误,而且即便编过也可能读越界)。
    void testRunStarting(Catch::TestRunInfo const& info) override
    {
        const ExclusiveAcquire& a = acquireTestsExclusiveOnce();
        if (a.owned)
        {
            return;
        }

        // **两种失败要分开说**:处方完全不同,合成一句会把人送错方向。
        if (a.handle == nullptr)
        {
            // 建不出来是**另一件事**:DACL 拒绝 / 句柄耗尽 / 同名非互斥对象占位。
            // 判负是对的(CLAUDE.md §2:建不出来判负,绝不静默继续),但这时**没有**
            // 「另一份在跑」,排队或走包装脚本都救不了。
            std::fprintf(stderr,
                         "[SL-324] %.*s: CreateMutexW(Local"
                         "\\SCVB-tests-proc) failed, GetLastError=%lu."
                         " This is NOT 'another test process is running' -- likely DACL denial,"
                         " handle exhaustion, or the name taken by a non-mutex object."
                         " SCVB test binaries use fixed IPC group ids whose segment names are"
                         " machine-wide, so they must not run without proven exclusivity."
                         "\n",
                         static_cast<int>(info.name.size()), info.name.data(), static_cast<unsigned long>(a.lastError));
        }
        else
        {
            std::fprintf(stderr,
                         "[SL-324] %.*s: another SCVB test process is already running on this"
                         " machine. All four test binaries (scvb_tests, scvb_monitor_tests,"
                         " scvb_host_tests, scvb_ipc_tests) share fixed IPC group ids g1-g8 whose"
                         " segment names are machine-wide, so two of them clobber each other's"
                         " segments -- including across different binaries."
                         "\n  Run it through the wrapper instead:"
                         " pwsh scripts/with-ipc-lock.ps1 -Command 'ctest --test-dir <builddir>"
                         " -C Release'"
                         "\n  (That wrapper takes gates' Local"
                         "\\SCVB-ipc-tests, NOT the mutex refused here -- with both runs behind"
                         " it they are serialised, so this -proc mutex is then free.)"
                         "\n  If nobody is running: look for a leftover scvb_*tests.exe --"
                         " a zombie keeps holding this mutex"
                         " (see the orphan scans around gate 6 in gates.ps1)."
                         "\n",
                         static_cast<int>(info.name.size()), info.name.data());
        }
        std::fflush(stderr);
        // 整轮不跑:再往下走就会建段,那正是要避免的加害。
        std::exit(2);
    }
};

} // namespace scvb::testsupport

CATCH_REGISTER_LISTENER(scvb::testsupport::TestsExclusiveListener);

#else // !_WIN32

// [B 线 M12a] POSIX:与 Windows 分支同一条不变式 ——「同机同用户只允许一份 SCVB 测试进程」,
// 判定同样在 `testRunStarting` 里做、同样 0 等待、拿不到同样 `std::exit(2)`。M12a 起 ipc / monitor
// 两套在 mac 上用**固定组号 + 真实段名**建段(段名经 SegmentBackendPosix 映射成 `/SynchainSCVB.v1.…`,
// POSIX shm 是全机命名空间),头注那张组号重叠表在这里同样成立,所以守卫必须是真的。
//
// 实现 = 对 `/tmp/scvb-tests-<uid>.lock` 取 `flock(LOCK_EX | LOCK_NB)`,fd 活到进程退出:
//   · **flock 而不是 fcntl 记录锁**:flock 按「打开的文件描述」计,进程退出(含被 SIGKILL)时内核必然
//     释放 —— 与 Windows 命名互斥体「进程没了锁就没了」同一个性质;没有 owner 文件、没有孤儿判定。
//   · **O_CLOEXEC**:测试进程会 posix_spawn 对端(scvb_ipc_peer)。不带它,子进程继承这个 fd,
//     而 flock 跟着「打开的文件描述」走 —— 父进程退出后锁仍被一个还活着的对端吊住,下一轮误判占用。
//   · **O_NOFOLLOW**:/tmp 是全用户可写目录,不跟随别人预先摆好的符号链接。
//   · **按 uid 分文件**:POSIX 后端的段权限是 0600,另一个用户的测试进程打不开、也清不掉本用户的段
//     (SegmentBackendPosix.h「已知限制」),不同用户之间没有可打坏的对象;同一个文件给所有用户共用,
//     反而会让一个用户的测试挡住另一个用户。
//   · **锁文件不跟 SCVB_IPC_LOCK_DIR 走**(卡面给过「或放在 SCVB_IPC_LOCK_DIR 下」这个选项,这里有意
//     不选):那个变量只改「段生命周期锁文件」放在哪,**段名本身仍是全机唯一的**。两轮测试若各设了
//     不同的 SCVB_IPC_LOCK_DIR,它们的段照样同名互踩(而且各自的生命周期锁互不相见,彼此会把对方的
//     活段当无主残段清掉)—— 把这把锁也放进那个目录,恰好让这种最坏的组合不再互斥。所以它钉在
//     与段名同一个作用域(全机,按用户)的 /tmp 上。
//   · 不碰 IPC 段、不看槽位 —— 理由与 Windows 分支头注「三条有意的边界」相同。

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace scvb::testsupport
{

// 取锁结果:owned = 本进程独占;fd < 0 表示锁文件**打不开**(与「已被占用」是两回事)。
struct ExclusiveAcquire
{
    int fd = -1;
    int error = 0; // 失败时的 errno(open 或 flock 的)
    bool owned = false;
    char path[64] = {};
};

// 每进程至多取一次(函数内 static)。fd 故意不关:活到进程退出,由内核释放 flock。
inline const ExclusiveAcquire& acquireTestsExclusiveOnce()
{
    static const ExclusiveAcquire result = [] {
        ExclusiveAcquire r;
        std::snprintf(r.path, sizeof(r.path), "/tmp/scvb-tests-%lu.lock", static_cast<unsigned long>(::getuid()));
        r.fd = ::open(r.path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (r.fd < 0)
        {
            r.error = errno;
            return r;
        }
        int rc = 0;
        do
        {
            rc = ::flock(r.fd, LOCK_SH | LOCK_NB); // INJECT-M12a-J3: shared lock -- a second test process is let through (was: LOCK_EX)
        } while (rc != 0 && errno == EINTR);
        if (rc == 0)
        {
            r.owned = true;
        }
        else
        {
            r.error = errno;
        }
        return r;
    }();
    return result;
}

struct TestsExclusiveListener : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;

    // `TestRunInfo::name` 是 `Catch::StringRef`(不保证 NUL 结尾),用 `%.*s`,理由同 Windows 分支。
    void testRunStarting(Catch::TestRunInfo const& info) override
    {
        const ExclusiveAcquire& a = acquireTestsExclusiveOnce();
        if (a.owned)
        {
            return;
        }

        // 两种失败分开说(处方不同),与 Windows 分支同一口径。
        if (a.fd < 0 || a.error != EWOULDBLOCK)
        {
            std::fprintf(stderr,
                         "[SL-324] %.*s: cannot take the test lock %s (errno=%d: %s)."
                         " This is NOT 'another test process is running' -- the lock file could not be"
                         " opened or flock failed for another reason (permissions, a file left there by"
                         " another user, a symlink at that path)."
                         " SCVB test binaries use fixed IPC group ids whose segment names are"
                         " machine-wide, so they must not run without proven exclusivity."
                         "\n",
                         static_cast<int>(info.name.size()), info.name.data(), a.path, a.error, std::strerror(a.error));
        }
        else
        {
            std::fprintf(stderr,
                         "[SL-324] %.*s: another SCVB test process is already running on this"
                         " machine (it holds flock on %s). All four test binaries (scvb_tests,"
                         " scvb_monitor_tests, scvb_host_tests, scvb_ipc_tests) share fixed IPC group"
                         " ids g1-g8 whose shared-memory names are machine-wide, so two of them"
                         " clobber each other's segments -- including across different binaries."
                         "\n  Run them one at a time (ctest without -j)."
                         "\n  If nobody is running: look for a leftover scvb_*tests process --"
                         " it keeps holding this lock until it exits."
                         "\n",
                         static_cast<int>(info.name.size()), info.name.data(), a.path);
        }
        std::fflush(stderr);
        // 整轮不跑:再往下走就会建段,那正是要避免的加害。
        std::exit(2);
    }
};

} // namespace scvb::testsupport

CATCH_REGISTER_LISTENER(scvb::testsupport::TestsExclusiveListener);

#endif // _WIN32
