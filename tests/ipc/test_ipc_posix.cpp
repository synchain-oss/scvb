// SPDX-License-Identifier: GPL-3.0-or-later
// test_ipc_posix —— scvb_ipc_tests 里只在 macOS 上编的格([ipc][posix],B 线 M12a)。
//
// test_ipc_contract.cpp / test_ipc_viz.cpp 的 IPC-1..20、VIZ-1..4 两平台同一份;这里补的是 POSIX 后端
// **特有**、Windows 上没有对应物的两件事,两件都用真实段名(冻结前缀 `SynchainSCVB.v1.`)+ 真对端进程:
//
//   IPC-P1  跨进程「最后一个离开者撤段」:对端建段、本进程附着;对端正常退出后段仍在(本进程还持着),
//           本进程离开之后段名在内核里消失(shm_open(O_RDONLY) == ENOENT)。
//   IPC-P2  持有者崩溃:对端是唯一持有者时被 _exit 掉(--die-at),段名**留在内核里**(Windows 上内核随句柄
//           关闭回收,这一态不存在);只附着方把它当「不存在」(kFailed、不碰它);下一个创建者清掉重建
//           (created=true、内容全零),离开后段名消失。
//   IPC-P3  support/exclusive_guard.h 的 POSIX 守卫真的拒绝同机第二个测试进程:本进程持着锁时再起一个
//           本二进制,它必须在跑任何用例之前以 2 退出,并说出「另一份在跑」。
//
// 为什么要 P1/P2:SegmentBackendPosix 把无主残段当「不存在」(openExisting* 返回 kFailed、createOrOpen 先清掉
// 再建),所以「离开时漏了 shm_unlink」对 IPC-1..20 的任何一条断言都**不可见** —— 只会在内核里堆残段,
// 直到 mac CI 在 ctest 之后跑 `scvb_tests "[shm-leftover]"` 才红,而且红在一道门禁步骤上、指不到是谁漏的。
// 这里直接看段名在不在,把它钉成 ipc 套件自己的一格。M05 的 tests/core/test_segment_backend_posix.cpp
// 在进程内(fork / 双实例)验过同一组性质,但用的是带 pid 的测试段名;这两格走的是插件用的真名与真对端。
//
// 运行期文案一律 ASCII(见 support/exclusive_guard.h 头注)。

#if !defined(SCVB_HAS_POSIX_SHM)
#error "test_ipc_posix.cpp needs the POSIX shared-memory backend (SCVB_HAS_POSIX_SHM, defined by scvb_core on Apple)"
#endif

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ipc/PlatformSegmentBackend.h"
#include "ipc_contract_harness.h"
#include "support/peer_spawn.h"

using scvb::ipctest::peer::deleteFile;
using scvb::ipctest::peer::PeerGuard;
using scvb::ipctest::peer::readFile;
using scvb::ipctest::peer::selfExePath;
using scvb::ipctest::peer::spawnPeer;
using scvb::ipctest::peer::tempCsvPath;
using scvb::ipctest::peer::waitPeer;

namespace
{
const std::string kPeer = scvb::ipctest::peer::kIpcPeerName;

// 这两格独占的段:g8 的 audio.ch14(IPC-1..20 用 g8 的只有 IPC-1 的 registry claimer)。
constexpr scvb::u32 kGroup = 8;
constexpr scvb::u32 kChannel = 14;

std::string posixNameOf(const std::wstring& name)
{
    std::string posix;
    const auto st = scvb::SegmentBackendPosix::toPosixName(name, posix);
    REQUIRE(st == scvb::SegmentBackendPosix::NameStatus::kOk);
    return posix;
}

// 段名此刻在内核里存不存在(只读探测,不建、不改;与 M05 的 [shm-leftover] 同一手法)。
bool shmExists(const std::string& posix)
{
    const int fd = ::shm_open(posix.c_str(), O_RDONLY, 0);
    const int err = errno;
    if (fd >= 0)
    {
        ::close(fd);
        return true;
    }
    INFO("shm_open(" << posix << ", O_RDONLY) errno=" << err);
    CHECK(err == ENOENT); // 不是 ENOENT(比如 EACCES)就不能读成「不存在」
    return false;
}

// 前一轮异常退出可能留下这个名字的无主残段:经后端清掉(createOrOpen 的清理分支 + 最后离开者撤段)。
void reclaimIfLeftover(const std::wstring& name, const std::string& posix)
{
    if (!shmExists(posix))
    {
        return;
    }
    scvb::PlatformSegmentBackend backend;
    scvb::SegmentView view;
    REQUIRE(backend.createOrOpen(name, scvb::ipctest::audioSegmentBytes(64), view) == scvb::InitResult::kOk);
    backend.unmap(view);
}

std::vector<std::string> writerArgs(const std::string& extra)
{
    return {"--role=writer",
            "--group=" + std::to_string(kGroup),
            "--ch=" + std::to_string(kChannel),
            "--ring-frames=4096",
            "--blocks=16",
            "--blocksize=64",
            extra};
}
} // namespace

TEST_CASE("IPC-P1 POSIX 跨进程:最后一个离开者撤段", "[ipc][posix][lifecycle]")
{
    const std::wstring name = scvb::segmentAudioName(kGroup, kChannel);
    const std::string posix = posixNameOf(name);
    reclaimIfLeftover(name, posix);
    REQUIRE_FALSE(shmExists(posix));

    // 对端建段、写完后 linger 3s;本进程在它活着时附着(只附着,绝不创建)。
    PeerGuard writer;
    int err = 0;
    writer.pi = spawnPeer(kPeer, writerArgs("--linger-ms=3000"), &err);
    REQUIRE(err == 0);

    scvb::PlatformSegmentBackend backend;
    scvb::SegmentView view;
    bool attached = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!attached && std::chrono::steady_clock::now() < deadline)
    {
        attached = backend.openExisting(name, view) == scvb::InitResult::kOk;
        if (!attached)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    REQUIRE(attached);
    CHECK_FALSE(view.created);

    // 对端正常退出(它的后端析构 = 离开,但本进程还持着 → 它不是最后一个,不撤段)。
    REQUIRE(waitPeer(writer.pi, 20000) == 0);
    CHECK(shmExists(posix));

    // 本进程离开:它是最后一个持有者 → 段名必须从内核里消失。
    backend.unmap(view);
    CHECK_FALSE(shmExists(posix));
}

TEST_CASE("IPC-P2 POSIX 持有者崩溃:无主残段只附着方视为不存在,下一个创建者清掉重建", "[ipc][posix][lifecycle]")
{
    const std::wstring name = scvb::segmentAudioName(kGroup, kChannel);
    const std::string posix = posixNameOf(name);
    reclaimIfLeftover(name, posix);
    REQUIRE_FALSE(shmExists(posix));

    // 对端是唯一持有者,写满 4 块后 _exit(7):不跑析构、不撤段(01 §5.2 的崩溃模型)。
    PeerGuard writer;
    int err = 0;
    writer.pi = spawnPeer(kPeer, writerArgs("--die-at=3"), &err);
    REQUIRE(err == 0);
    REQUIRE(waitPeer(writer.pi, 20000) == 7);

    // 内核里留下无主残段(flock 随对端进程一起被内核放掉)。
    CHECK(shmExists(posix));

    scvb::PlatformSegmentBackend backend;
    {
        // 只附着方:无主残段等同「不存在」—— kFailed,且不碰它。
        scvb::SegmentView view;
        CHECK(backend.openExisting(name, view) == scvb::InitResult::kFailed);
        CHECK(backend.openExistingReadOnly(name, view) == scvb::InitResult::kFailed);
        CHECK(shmExists(posix));
    }

    // 下一个创建者:清掉残段、按新尺寸重建,内容全零(对端写过的 write_head 不会复活)。
    scvb::SegmentView view;
    REQUIRE(backend.createOrOpen(name, scvb::ipctest::audioSegmentBytes(4096), view) == scvb::InitResult::kOk);
    CHECK(view.created);
    const auto* hdr = static_cast<const scvb::AudioRingHeader*>(view.base);
    CHECK(hdr->magic.load(std::memory_order_acquire) == 0u);
    CHECK(hdr->write_head_samples.load(std::memory_order_acquire) == 0u);

    // 它也是唯一持有者:离开即撤段。
    backend.unmap(view);
    CHECK_FALSE(shmExists(posix));
}

// IPC-P3 的子进程入口:只在守卫失效时才会真的跑到这里(父进程持着锁,子进程应在 testRunStarting 就退出)。
// 隐藏([.]),默认集与 ctest 都不跑它。
TEST_CASE("IPC-P3 guard child", "[.][ipc][posix][guard]")
{
    SUCCEED("reached only when the exclusive guard let a second test process through");
}

TEST_CASE("IPC-P3 POSIX 独占守卫:同机第二个测试进程被拒", "[ipc][posix][guard]")
{
    const std::string self = selfExePath();
    REQUIRE_FALSE(self.empty());

    // 子进程的 stdout / stderr 落到临时文件,判据要读它说了什么(退出码 2 不唯一:Catch2 自己「一个用例都没跑」
    // 时也返回 2,只看退出码会把「用例名对不上」读成「守卫拒绝了」)。
    const std::string out = tempCsvPath() + ".guard.log";
    posix_spawn_file_actions_t fa;
    REQUIRE(::posix_spawn_file_actions_init(&fa) == 0);
    REQUIRE(::posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600) ==
            0);
    REQUIRE(::posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO) == 0);

    std::string arg0 = self;
    std::string arg1 = "IPC-P3 guard child";
    char* argv[] = {arg0.data(), arg1.data(), nullptr};
    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, self.c_str(), &fa, nullptr, argv, environ);
    ::posix_spawn_file_actions_destroy(&fa);
    REQUIRE(rc == 0);

    PeerGuard child;
    child.pi.pid = pid;
    const int code = waitPeer(child.pi, 30000);
    const std::string log = readFile(out);
    deleteFile(out);

    INFO("child exit code " << code << ", output:\n" << log);
    CHECK(code == 2);
    CHECK(log.find("another SCVB test process is already running") != std::string::npos);
    CHECK(log.find("/tmp/scvb-tests-") != std::string::npos);
}
