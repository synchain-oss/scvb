// SPDX-License-Identifier: GPL-3.0-or-later
// test_platform_identity —— 进程身份与 pid 探活的平台口径(B 线 M03),编进 scvb_tests,Windows 与 macOS 都跑。
//   · currentProcessId():与操作系统给的 pid 一致、多次取值稳定;SidecarStore 的进程身份与它同一个 pid;
//   · isProcessAlive():对自身为真;pid 0 与超出 pid_t 正数范围的值判死;
//   · POSIX 专属:fork 出的子进程 _exit 并被 waitpid 回收后,其 pid 判死 —— 这是「DAW 崩溃后旧槽能被
//     接管」的前提(接管双条件之一是探活失败)。M03 之前非 Windows 恒判活,这一格在 mac 上必红。
// 运行期文案一律 ASCII(中文字面量在本机 CP936 上触发 MSVC C4819);中文只写注释。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "ipc/Registry.h"
#include "state/SidecarStore.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

TEST_CASE("PLATFORM-ID-1 currentProcessId matches the OS pid and is stable", "[platform][identity]")
{
    const scvb::u32 a = scvb::currentProcessId();
    const scvb::u32 b = scvb::currentProcessId();
    CHECK(a != 0u);
    CHECK(a == b);
#ifdef _WIN32
    CHECK(a == static_cast<scvb::u32>(::GetCurrentProcessId()));
#else
    CHECK(a == static_cast<scvb::u32>(::getpid()));
#endif

    // owner.lock 判「是不是我」用的身份与 IPC 槽里写的 pid 必须是同一个数。
    const scvb::state::ProcessIdentity id = scvb::state::currentProcessIdentity();
    CHECK(id.pid == static_cast<std::uint64_t>(a));
    CHECK_FALSE(id.hostName.empty());
#if defined(_WIN32) || defined(__APPLE__)
    // 进程起始时刻:非 0,且同一进程两次取值相同(否则 owner.lock 会把自己判成别人)。
    CHECK(id.processStartEpochMs != 0u);
    CHECK(scvb::state::currentProcessIdentity().processStartEpochMs == id.processStartEpochMs);
#endif
}

TEST_CASE("PLATFORM-ID-2 isProcessAlive: self is alive, invalid pids are dead", "[platform][identity]")
{
    CHECK(scvb::isProcessAlive(scvb::currentProcessId()));
    CHECK_FALSE(scvb::isProcessAlive(0u));
    // (u32)-1:POSIX 上若直接转 pid_t 交给 kill,会变成 kill(-1, 0)「向所有可发信号的进程」而恒成功。
    CHECK_FALSE(scvb::isProcessAlive(0xFFFFFFFFu));
}

#ifndef _WIN32
TEST_CASE("PLATFORM-ID-3 POSIX: a forked child that exited and was reaped is judged dead",
          "[platform][identity][posix]")
{
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        ::_exit(0); // 子进程:不跑 Catch2 的任何收尾、不跑静态析构,立即退出
    }
    const auto childPid = static_cast<scvb::u32>(child);

    // 回收前 pid 仍被占着(运行中或僵尸),kill(pid, 0) 成功 → 判活。
    // 这一格同时防「探活恒判死」—— 那会让活着的 DAW 被误接管、双写同一条环。
    CHECK(scvb::isProcessAlive(childPid));

    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));

    // 回收后 pid 不再存在 → 判死。M03 之前非 Windows 恒返回「活」,红在这一条上。
    CHECK_FALSE(scvb::isProcessAlive(childPid));
}
#endif
