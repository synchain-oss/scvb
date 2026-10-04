// SPDX-License-Identifier: GPL-3.0-or-later
// test_segment_backend_posix —— POSIX 共享内存后端(B 线 M05)的单测。只在 Apple 上编进 scvb_tests
// (tests/CMakeLists.txt 里那个 if(APPLE) 追加块)。
//
// 隔离:每个用例用自己的临时锁目录(SCVB_IPC_LOCK_DIR)和带 pid 的段名("Local\SCVBt<pid>.…"),
// 不碰真实 home 下的锁目录,也**不建**冻结前缀 SynchainSCVB.v1. 下的真实段名 —— 两个测试进程同机并发
// 时彼此打不坏(tests/support/exclusive_guard.h 的 POSIX 版守卫归 M12a)。用 Registry / CtrlPlane 跑的
// 「两个后端各跑一遍」用例经一个改名层把冻结前缀换成带 pid 的前缀,真正的 shm / flock 仍由
// SegmentBackendPosix 做。唯一碰真实段名的是隐藏格 [.][shm-leftover],它只读探测(shm_open O_RDONLY)。
//
// 运行期文案一律 ASCII(用例名、INFO);中文只写在注释里。

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ipc/CtrlPlane.h"
#include "ipc/IpcDiag.h"
#include "ipc/Registry.h"
#include "ipc/SegmentBackendInProcess.h"
#include "ipc/SegmentBackendPosix.h"
#include "ipc/SegmentLayout.h"

using scvb::InitResult;
using scvb::SegmentBackendPosix;
using scvb::SegmentView;
using scvb::u32;
using scvb::u64;
using NameStatus = scvb::SegmentBackendPosix::NameStatus;

namespace
{
// macOS 对 shm_open 名字的长度上限(xnu 的 PSHMNAMLEN = 31,含开头的 '/',不含结尾 NUL)。
// 这个宏在内核私有头里,用户态拿不到,只能写常量;超出即 ENAMETOOLONG。
// 与 SegmentBackendPosix::kMaxShmNameLength 刻意分开写:下面有一格核对两者相等,并由内核再证一次。
constexpr std::size_t kPosixShmNameMax = 31;

// 逻辑段名(不带 OS 前缀)→ POSIX 名:加 '/' 前缀。冻结的逻辑前缀只允许 ASCII。
std::string posixName(const std::wstring& logical)
{
    std::string s = "/";
    for (const wchar_t c : logical)
    {
        REQUIRE(static_cast<unsigned long>(c) < 0x80ul);
        s.push_back(static_cast<char>(c));
    }
    return s;
}

constexpr std::size_t kSeg = 64 * 1024; // 16 KiB 页(arm64)的整倍数:真实尺寸 == 请求尺寸,断言可以写等号
constexpr u64 kT0 = 1000;

std::wstring widen(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

std::string pidTag()
{
    return std::to_string(static_cast<long>(::getpid()));
}

// "Local\SCVBt<pid>.<tag><n>":本进程内每次调用都不同,跨进程因 pid 不同而不同。
std::wstring uniqueName(const char* tag)
{
    static int counter = 0;
    ++counter;
    return L"Local\\" + widen("SCVBt" + pidTag() + "." + tag + std::to_string(counter));
}

// POSIX 名恰好 posixLen 个字符(含 '/')的唯一名字。
std::wstring nameOfPosixLength(std::size_t posixLen)
{
    static int counter = 0;
    ++counter;
    std::string logical = "SCVBt" + pidTag() + ".L" + std::to_string(counter) + ".";
    REQUIRE(logical.size() + 1 <= posixLen);
    logical.append(posixLen - 1 - logical.size(), 'x');
    return L"Local\\" + widen(logical);
}

std::string toPosix(const std::wstring& name)
{
    std::string out;
    REQUIRE(SegmentBackendPosix::toPosixName(name, out) == NameStatus::kOk);
    return out;
}

// 只读探测(绝不建段):存在 → true;ENOENT → false;别的 errno 直接判红。
bool shmExists(const std::string& posix)
{
    const int fd = ::shm_open(posix.c_str(), O_RDONLY, 0);
    if (fd >= 0)
    {
        ::close(fd);
        return true;
    }
    const int err = errno;
    INFO("shm_open(" << posix << ", O_RDONLY) errno=" << err);
    REQUIRE(err == ENOENT);
    return false;
}

struct ShmStat
{
    std::size_t size = 0;
    unsigned mode = 0;
    uid_t uid = 0;
};

ShmStat shmStat(const std::string& posix)
{
    const int fd = ::shm_open(posix.c_str(), O_RDONLY, 0);
    REQUIRE(fd >= 0);
    struct stat st
    {
    };
    const int r = ::fstat(fd, &st);
    ::close(fd);
    REQUIRE(r == 0);
    ShmStat s;
    s.size = static_cast<std::size_t>(st.st_size);
    s.mode = static_cast<unsigned>(st.st_mode) & 0777u;
    s.uid = st.st_uid;
    return s;
}

// 用例结束(含 REQUIRE 中途失败)时兜底 unlink,不让一格的残段污染后面的格。
struct UnlinkOnExit
{
    std::string name;
    ~UnlinkOnExit() { ::shm_unlink(name.c_str()); }
};

// 不经后端、不持任何锁地建一个段:模拟崩溃进程留下的残段。
void createOrphan(const std::string& posix, std::size_t size, unsigned char fill, int mode)
{
    const int fd = ::shm_open(posix.c_str(), O_CREAT | O_EXCL | O_RDWR, mode);
    const int err = errno;
    INFO("orphan " << posix << " errno=" << err);
    REQUIRE(fd >= 0);
    REQUIRE(::ftruncate(fd, static_cast<off_t>(size)) == 0);
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    REQUIRE(p != MAP_FAILED);
    std::memset(p, fill, size);
    ::munmap(p, size);
    ::close(fd);
}

void touchFile(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    REQUIRE(fd >= 0);
    ::close(fd);
}

unsigned char* bytes(const SegmentView& v)
{
    return static_cast<unsigned char*>(v.base);
}

bool allBytesAre(const SegmentView& v, unsigned char value)
{
    const unsigned char* p = bytes(v);
    return std::all_of(p, p + v.size, [value](unsigned char c) { return c == value; });
}

// 映射的保护位(mach_vm_region 读回):只读映射必须没有 VM_PROT_WRITE。
vm_prot_t protectionOf(const void* base)
{
    mach_vm_address_t addr = reinterpret_cast<mach_vm_address_t>(base);
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const kern_return_t kr = ::mach_vm_region(::mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64,
                                              reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    REQUIRE(kr == KERN_SUCCESS);
    REQUIRE(addr == reinterpret_cast<mach_vm_address_t>(base));
    return info.protection;
}

// 环境变量的作用域改写(析构时恢复原值或删掉)。
class EnvVarGuard
{
public:
    explicit EnvVarGuard(const char* name) : name_(name)
    {
        const char* prev = std::getenv(name);
        hadPrev_ = (prev != nullptr);
        if (hadPrev_)
        {
            prev_ = prev;
        }
    }
    ~EnvVarGuard()
    {
        if (hadPrev_)
        {
            ::setenv(name_, prev_.c_str(), 1);
        }
        else
        {
            ::unsetenv(name_);
        }
    }
    EnvVarGuard(const EnvVarGuard&) = delete;
    EnvVarGuard& operator=(const EnvVarGuard&) = delete;

private:
    const char* name_;
    std::string prev_;
    bool hadPrev_ = false;
};

// 每格自己的锁目录:建临时目录并设 SCVB_IPC_LOCK_DIR(后端在**构造时**读它,所以要先于后端构造)。
class TempLockDir
{
public:
    TempLockDir() : env_(SegmentBackendPosix::kLockDirEnvVar)
    {
        std::string tmpl = (std::filesystem::temp_directory_path() / "scvb-m05-XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        REQUIRE(::mkdtemp(buf.data()) != nullptr);
        path_ = buf.data();
        REQUIRE(::setenv(SegmentBackendPosix::kLockDirEnvVar, path_.c_str(), 1) == 0);
    }
    ~TempLockDir()
    {
        ::chmod(path_.c_str(), 0700); // 有的格会把它改成只读
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempLockDir(const TempLockDir&) = delete;
    TempLockDir& operator=(const TempLockDir&) = delete;

    const std::string& path() const { return path_; }
    std::string lockFile(const std::string& posix) const { return path_ + "/" + posix.substr(1) + ".lock"; }

private:
    EnvVarGuard env_;
    std::string path_;
};

// 诊断 sink 录制器(作用域内装上,析构时恢复原 sink)。
struct RecordedDiag
{
    scvb::IpcDiagOp op;
    int error;
    std::string segment;
    std::string detail;
};

std::vector<RecordedDiag>& recordedDiags()
{
    static std::vector<RecordedDiag> v;
    return v;
}

void recordDiag(const scvb::IpcDiagEvent& e) noexcept
{
    try
    {
        recordedDiags().push_back(RecordedDiag{e.op, e.error, e.segment, e.detail});
    }
    catch (...)
    {
    }
}

class DiagRecorder
{
public:
    DiagRecorder()
    {
        recordedDiags().clear();
        prev_ = scvb::setIpcDiagSink(&recordDiag);
    }
    ~DiagRecorder() { scvb::setIpcDiagSink(prev_); }
    DiagRecorder(const DiagRecorder&) = delete;
    DiagRecorder& operator=(const DiagRecorder&) = delete;

    bool has(scvb::IpcDiagOp op, int error, const std::string& segment) const
    {
        const auto& v = recordedDiags();
        return std::any_of(v.begin(), v.end(), [&](const RecordedDiag& d) {
            return d.op == op && d.error == error && d.segment == segment;
        });
    }
    std::size_t count() const { return recordedDiags().size(); }
    std::string dump() const
    {
        std::string s;
        for (const auto& d : recordedDiags())
        {
            s += std::string(scvb::ipcDiagOpName(d.op)) + " errno=" + std::to_string(d.error) + " seg=" + d.segment +
                 " (" + d.detail + ")\n";
        }
        return s;
    }

private:
    scvb::IpcDiagSink prev_ = nullptr;
};
} // namespace

// ---------------------------------------------------------------------------
// 名字
// ---------------------------------------------------------------------------

TEST_CASE("POSIX shm names: every logical segment name fits PSHMNAMLEN with the '/' prefix", "[posix][names]")
{
    std::size_t longest = 0;
    const auto consider = [&](const std::wstring& logical) { longest = std::max(longest, posixName(logical).size()); };

    for (scvb::u32 g = 1; g <= scvb::kMaxGroups; ++g)
    {
        consider(scvb::segmentLogicalName(g, scvb::SegmentKind::kRegistry));
        consider(scvb::segmentLogicalName(g, scvb::SegmentKind::kCtrl));
        consider(scvb::segmentLogicalName(g, scvb::SegmentKind::kViz));
        for (scvb::u32 ch = 1; ch <= scvb::kMaxChannels; ++ch)
        {
            consider(scvb::segmentLogicalName(g, scvb::SegmentKind::kAudio, ch));
            consider(scvb::segmentLogicalName(g, scvb::SegmentKind::kFeat, ch));
        }
    }

    // SegmentLayout.h 段名注释写的上界 "/SynchainSCVB.v1.g8.audio.ch15"(30 字符)确实是最长的那一档
    // (两位数声道的 audio / feat 名都与它等长,不唯一,所以比长度而不比名字)。
    const std::string documentedLongest = posixName(scvb::segmentLogicalName(8, scvb::SegmentKind::kAudio, 15));
    INFO("documented longest: " << documentedLongest << " (" << documentedLongest.size() << " chars)");
    CHECK(documentedLongest.size() == longest);
    REQUIRE(longest <= kPosixShmNameMax);
}

TEST_CASE("POSIX backend maps Local-prefixed names to /name and refuses every other shape", "[posix][names]")
{
    CHECK(SegmentBackendPosix::kMaxShmNameLength == kPosixShmNameMax);

    std::string out;
    CHECK(SegmentBackendPosix::toPosixName(scvb::segmentAudioName(8, 15), out) == NameStatus::kOk);
    CHECK(out == "/SynchainSCVB.v1.g8.audio.ch15");
    CHECK(out.size() == std::size_t{30});
    CHECK(SegmentBackendPosix::toPosixName(scvb::segmentRegistryName(1), out) == NameStatus::kOk);
    CHECK(out == "/SynchainSCVB.v1.g1.registry");

    // 每一个真实段名都能映射,且映射结果就是 '/' + 逻辑名(冻结前缀原样保留)。
    std::size_t mapped = 0;
    for (u32 g = 1; g <= scvb::kMaxGroups; ++g)
    {
        std::vector<std::wstring> logical = {scvb::segmentLogicalName(g, scvb::SegmentKind::kRegistry),
                                             scvb::segmentLogicalName(g, scvb::SegmentKind::kCtrl),
                                             scvb::segmentLogicalName(g, scvb::SegmentKind::kViz)};
        for (u32 ch = 1; ch <= scvb::kMaxChannels; ++ch)
        {
            logical.push_back(scvb::segmentLogicalName(g, scvb::SegmentKind::kAudio, ch));
            logical.push_back(scvb::segmentLogicalName(g, scvb::SegmentKind::kFeat, ch));
        }
        for (const auto& l : logical)
        {
            INFO("logical name index " << mapped);
            CHECK(SegmentBackendPosix::toPosixName(L"Local\\" + l, out) == NameStatus::kOk);
            CHECK(out == posixName(l));
            ++mapped;
        }
    }
    CHECK(mapped == std::size_t{scvb::kMaxGroups} * (3u + 2u * scvb::kMaxChannels));

    // 拒绝:没有 "Local\" 前缀(含已经是 POSIX 形态的名字)、空逻辑名、"." / ".."、路径分隔符、非 ASCII、空白。
    CHECK(SegmentBackendPosix::toPosixName(L"SynchainSCVB.v1.g1.registry", out) == NameStatus::kBadPrefix);
    CHECK(SegmentBackendPosix::toPosixName(L"Global\\SynchainSCVB.v1.g1.registry", out) == NameStatus::kBadPrefix);
    CHECK(SegmentBackendPosix::toPosixName(L"/SynchainSCVB.v1.g1.registry", out) == NameStatus::kBadPrefix);
    CHECK(SegmentBackendPosix::toPosixName(L"Local\\", out) == NameStatus::kBadChar);
    CHECK(SegmentBackendPosix::toPosixName(L"Local\\.", out) == NameStatus::kBadChar);
    CHECK(SegmentBackendPosix::toPosixName(L"Local\\..", out) == NameStatus::kBadChar);
    CHECK(SegmentBackendPosix::toPosixName(L"Local\\a/b", out) == NameStatus::kBadChar);
    CHECK(SegmentBackendPosix::toPosixName(L"Local\\a\\b", out) == NameStatus::kBadChar);
    CHECK(SegmentBackendPosix::toPosixName(L"Local\\a b", out) == NameStatus::kBadChar);
    CHECK(SegmentBackendPosix::toPosixName(L"Local\\café", out) == NameStatus::kBadChar);
    CHECK(out == "/caf?"); // 失败时只给一个可打印的近似名供诊断

    // 长度边界:31 通过,32 超长。
    CHECK(SegmentBackendPosix::toPosixName(nameOfPosixLength(31), out) == NameStatus::kOk);
    CHECK(out.size() == std::size_t{31});
    CHECK(SegmentBackendPosix::toPosixName(nameOfPosixLength(32), out) == NameStatus::kTooLong);
    CHECK(out.size() == std::size_t{32});
}

TEST_CASE("POSIX backend: 30- and 31-char names work end to end; a 32-char name is kFailed with ENAMETOOLONG",
          "[posix][names]")
{
    TempLockDir dir;
    DiagRecorder diag;
    SegmentBackendPosix backend;

    for (const std::size_t len : {std::size_t{30}, std::size_t{31}})
    {
        const auto name = nameOfPosixLength(len);
        const auto posix = toPosix(name);
        UnlinkOnExit guard{posix};
        INFO("posix name " << posix << " (" << posix.size() << " chars)");
        SegmentView v;
        REQUIRE(backend.createOrOpen(name, kSeg, v) == InitResult::kOk);
        CHECK(v.created);
        CHECK(shmExists(posix));
        backend.unmap(v);
        CHECK_FALSE(shmExists(posix));
    }

    const auto tooLong = nameOfPosixLength(32);
    std::string approx;
    REQUIRE(SegmentBackendPosix::toPosixName(tooLong, approx) == NameStatus::kTooLong);
    SegmentView v;
    CHECK(backend.createOrOpen(tooLong, kSeg, v) == InitResult::kFailed);
    CHECK(v.base == nullptr);
    INFO(diag.dump());
    CHECK(diag.has(scvb::IpcDiagOp::kName, ENAMETOOLONG, approx));
    CHECK(backend.openExisting(tooLong, v) == InitResult::kFailed);
    CHECK(backend.openExistingReadOnly(tooLong, v) == InitResult::kFailed);
    // 在碰文件系统之前就拒了:连锁文件都没建。
    CHECK_FALSE(std::filesystem::exists(dir.lockFile(approx)));

    // 常量与内核一致:32 字符的名字 shm_open 本身就报 ENAMETOOLONG(O_RDONLY,不建)。
    const int fd = ::shm_open(approx.c_str(), O_RDONLY, 0);
    const int err = errno;
    CHECK(fd < 0);
    CHECK(err == ENAMETOOLONG);
    if (fd >= 0)
    {
        ::close(fd);
    }
}

// ---------------------------------------------------------------------------
// 建 / 附着 / 尺寸 / 权限
// ---------------------------------------------------------------------------

TEST_CASE("POSIX backend: creator gets created=true; attachers get the real size and never resize",
          "[posix][lifecycle]")
{
    TempLockDir dir;
    SegmentBackendPosix a;
    SegmentBackendPosix b;
    const auto name = uniqueName("cr");
    const auto posix = toPosix(name);
    UnlinkOnExit guard{posix};

    SegmentView va;
    REQUIRE(a.createOrOpen(name, kSeg, va) == InitResult::kOk);
    CHECK(va.created);
    CHECK(va.size == kSeg);
    CHECK(shmStat(posix).size == kSeg);
    bytes(va)[100] = 0xA5;

    // 附着方请求更大的尺寸:拿到的是创建者的真实尺寸,段本身也没被改尺寸。
    SegmentView vb;
    REQUIRE(b.createOrOpen(name, kSeg * 2, vb) == InitResult::kOk);
    CHECK_FALSE(vb.created);
    CHECK(vb.size == kSeg);
    CHECK(shmStat(posix).size == kSeg);
    CHECK(bytes(vb)[100] == 0xA5); // 同一块内存

    // 请求更小的尺寸也一样。
    SegmentView vc;
    REQUIRE(b.createOrOpen(name, 4096, vc) == InitResult::kOk);
    CHECK_FALSE(vc.created);
    CHECK(vc.size == kSeg);

    // openExisting 同样回填真实尺寸。
    SegmentView vd;
    REQUIRE(a.openExisting(name, vd) == InitResult::kOk);
    CHECK_FALSE(vd.created);
    CHECK(vd.size == kSeg);
    CHECK(shmStat(posix).size == kSeg);

    a.unmap(va);
    b.unmap(vb);
    b.unmap(vc);
    a.unmap(vd);
    CHECK_FALSE(shmExists(posix));

    // 非页整倍数的请求:真实尺寸取 fstat(macOS 可能按页上取整),只会 ≥ 请求值。
    const auto odd = uniqueName("odd");
    const auto oddPosix = toPosix(odd);
    UnlinkOnExit oddGuard{oddPosix};
    SegmentView vo;
    REQUIRE(a.createOrOpen(odd, 5000, vo) == InitResult::kOk);
    INFO("requested 5000, view.size " << vo.size << ", page " << ::getpagesize());
    CHECK(vo.size >= std::size_t{5000});
    CHECK(vo.size == shmStat(oddPosix).size);
    a.unmap(vo);
    CHECK_FALSE(shmExists(oddPosix));
}

TEST_CASE("POSIX backend: segment and lock files are 0600 and owned by the current user", "[posix][perm]")
{
    TempLockDir dir;
    SegmentBackendPosix backend;
    const auto name = uniqueName("perm");
    const auto posix = toPosix(name);
    UnlinkOnExit guard{posix};

    SegmentView v;
    REQUIRE(backend.createOrOpen(name, kSeg, v) == InitResult::kOk);
    const ShmStat s = shmStat(posix);
    CHECK(s.mode == 0600u);
    CHECK(s.uid == ::geteuid());

    struct stat st
    {
    };
    REQUIRE(::stat(dir.lockFile(posix).c_str(), &st) == 0);
    CHECK(S_ISREG(st.st_mode));
    CHECK((static_cast<unsigned>(st.st_mode) & 0777u) == 0600u);
    REQUIRE(::stat((dir.path() + "/" + SegmentBackendPosix::kLifecycleLockName).c_str(), &st) == 0);
    CHECK((static_cast<unsigned>(st.st_mode) & 0777u) == 0600u);

    backend.unmap(v);
}

TEST_CASE("POSIX backend: read-only open is PROT_READ, holds the segment, and never creates", "[posix][readonly]")
{
    TempLockDir dir;
    DiagRecorder diag;
    SegmentBackendPosix creator;
    SegmentBackendPosix reader;
    const auto name = uniqueName("ro");
    const auto posix = toPosix(name);
    UnlinkOnExit guard{posix};

    // 段不存在:只读 / 附着打开都 kFailed,不建段、不建锁文件,也不刷诊断(这是 25Hz 重试的常态)。
    SegmentView r;
    CHECK(reader.openExistingReadOnly(name, r) == InitResult::kFailed);
    CHECK(reader.openExisting(name, r) == InitResult::kFailed);
    CHECK_FALSE(shmExists(posix));
    CHECK_FALSE(std::filesystem::exists(dir.lockFile(posix)));
    INFO(diag.dump());
    CHECK(diag.count() == std::size_t{0});

    SegmentView c;
    REQUIRE(creator.createOrOpen(name, kSeg, c) == InitResult::kOk);
    REQUIRE(c.created);
    bytes(c)[0] = 0x3C;

    REQUIRE(reader.openExistingReadOnly(name, r) == InitResult::kOk);
    CHECK_FALSE(r.created);
    CHECK(r.size == kSeg);
    CHECK(bytes(r)[0] == 0x3C);
    CHECK((protectionOf(r.base) & VM_PROT_WRITE) == 0);
    CHECK((protectionOf(r.base) & VM_PROT_READ) != 0);
    CHECK((protectionOf(c.base) & VM_PROT_WRITE) != 0); // 对照:创建者的映射可写

    // 读者持 SH:创建者离开不会 unlink;读者是最后一个离开者时才 unlink。
    creator.unmap(c);
    CHECK(shmExists(posix));
    CHECK(bytes(r)[0] == 0x3C);
    reader.unmap(r);
    CHECK_FALSE(shmExists(posix));
}

// ---------------------------------------------------------------------------
// 生命周期:最后离开者清理 / 残段重建 / 崩溃接管 / 竞争 / 同进程互斥
// ---------------------------------------------------------------------------

TEST_CASE("POSIX backend: a live attacher keeps the segment; the last one out unlinks it", "[posix][lifecycle]")
{
    // 附着方式两种都测:createOrOpen 的附着分支,与 openExisting。
    const bool viaOpenExisting = GENERATE(false, true);
    INFO("attach via " << (viaOpenExisting ? "openExisting" : "createOrOpen"));

    TempLockDir dir;
    SegmentBackendPosix a;
    SegmentBackendPosix b;
    SegmentBackendPosix c;
    const auto name = uniqueName("last");
    const auto posix = toPosix(name);
    UnlinkOnExit guard{posix};

    SegmentView va;
    REQUIRE(a.createOrOpen(name, kSeg, va) == InitResult::kOk);
    REQUIRE(va.created);
    SegmentView vb;
    REQUIRE((viaOpenExisting ? b.openExisting(name, vb) : b.createOrOpen(name, kSeg, vb)) == InitResult::kOk);
    REQUIRE_FALSE(vb.created);
    bytes(vb)[7] = 0x77;

    a.unmap(va); // 创建者先走:附着方还持 SH,段必须还在
    CHECK(shmExists(posix));

    // 新来的实例附着到**同一个**对象,而不是另建一个(没被 unlink 的证据)。
    SegmentView vc;
    REQUIRE(c.createOrOpen(name, kSeg, vc) == InitResult::kOk);
    CHECK_FALSE(vc.created);
    CHECK(bytes(vc)[7] == 0x77);
    c.unmap(vc);
    CHECK(shmExists(posix));

    b.unmap(vb); // 最后一个持有者离开 → unlink
    CHECK_FALSE(shmExists(posix));

    // 之后的创建者从一个全新的段开始。
    SegmentView vd;
    REQUIRE(a.createOrOpen(name, kSeg, vd) == InitResult::kOk);
    CHECK(vd.created);
    CHECK(bytes(vd)[7] == 0);
    a.unmap(vd);
    CHECK_FALSE(shmExists(posix));
}

TEST_CASE("POSIX backend: an orphaned segment of an old size is rebuilt; readers leave it alone", "[posix][stale]")
{
    TempLockDir dir;
    DiagRecorder diag;
    SegmentBackendPosix backend;
    const auto name = uniqueName("stale");
    const auto posix = toPosix(name);
    UnlinkOnExit guard{posix};

    // 崩溃现场:段在(旧尺寸、旧内容),锁目录与锁文件也在,但没有任何持有者。
    constexpr std::size_t kOldSize = 16 * 1024;
    createOrphan(posix, kOldSize, 0xEE, 0600);
    touchFile(dir.path() + "/" + SegmentBackendPosix::kLifecycleLockName);
    touchFile(dir.lockFile(posix));

    // 附着 / 只读路径:没有活持有者 ⇒ 等同 Windows 上段已消失 ⇒ kFailed,且不动残段。
    SegmentView r;
    CHECK(backend.openExisting(name, r) == InitResult::kFailed);
    CHECK(backend.openExistingReadOnly(name, r) == InitResult::kFailed);
    CHECK(shmExists(posix));
    CHECK(shmStat(posix).size == kOldSize);

    // 创建者:清掉残段,按新尺寸重建,内容全零。
    SegmentView v;
    REQUIRE(backend.createOrOpen(name, kSeg, v) == InitResult::kOk);
    CHECK(v.created);
    CHECK(v.size == kSeg);
    CHECK(shmStat(posix).size == kSeg);
    CHECK(allBytesAre(v, 0));
    INFO(diag.dump());
    backend.unmap(v);
    CHECK_FALSE(shmExists(posix));
}

TEST_CASE("POSIX backend: after a child dies without cleanup the parent takes the segment over", "[posix][fork]")
{
    TempLockDir dir; // 子进程继承 SCVB_IPC_LOCK_DIR
    const auto name = uniqueName("fork");
    const auto posix = toPosix(name);
    UnlinkOnExit guard{posix};
    constexpr std::size_t kChildSize = 32 * 1024;

    int ready[2] = {-1, -1};
    int release[2] = {-1, -1};
    REQUIRE(::pipe(ready) == 0);
    REQUIRE(::pipe(release) == 0);

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        // 子进程:建段、写满 0x5A、告诉父进程,然后等父进程一声令下直接 _exit —— 不 unmap、不跑析构,
        // 等同崩溃。绝不回到 Catch2。
        ::close(ready[0]);
        ::close(release[1]);
        auto* backend = new SegmentBackendPosix(); // 故意不释放:_exit 不跑析构
        SegmentView v;
        char status = 'F';
        if (backend->createOrOpen(name, kChildSize, v) == InitResult::kOk && v.created)
        {
            std::memset(v.base, 0x5A, v.size);
            status = 'C';
        }
        ssize_t n = ::write(ready[1], &status, 1);
        char go = 0;
        n = ::read(release[0], &go, 1);
        (void)n;
        ::_exit(0);
    }

    ::close(ready[1]);
    ::close(release[0]);
    char status = 0;
    REQUIRE(::read(ready[0], &status, 1) == 1);
    REQUIRE(status == 'C');

    // 子进程活着:父进程只能附着(created=false),看到子进程的数据与尺寸。
    SegmentBackendPosix parent;
    SegmentView live;
    REQUIRE(parent.createOrOpen(name, kSeg, live) == InitResult::kOk);
    CHECK_FALSE(live.created);
    CHECK(live.size == kChildSize);
    CHECK(bytes(live)[123] == 0x5A);
    parent.unmap(live); // 子进程仍持 SH:不是最后一个,不 unlink
    CHECK(shmExists(posix));

    // 子进程不清理就死掉:内核放掉它的 flock,段本身留下。
    const char go = 'x';
    REQUIRE(::write(release[1], &go, 1) == 1);
    int wstatus = 0;
    REQUIRE(::waitpid(child, &wstatus, 0) == child);
    CHECK(WIFEXITED(wstatus));
    ::close(ready[0]);
    ::close(release[1]);
    CHECK(shmExists(posix));

    // 父进程接管:走清理分支,按自己的尺寸重建,旧数据不在了。
    SegmentView taken;
    REQUIRE(parent.createOrOpen(name, kSeg, taken) == InitResult::kOk);
    CHECK(taken.created);
    CHECK(taken.size == kSeg);
    CHECK(allBytesAre(taken, 0));
    parent.unmap(taken);
    CHECK_FALSE(shmExists(posix));
}

TEST_CASE("POSIX backend: two racing creators -- exactly one gets created=true (repeated)", "[posix][race]")
{
    // 重复次数:每轮两个线程、两个后端实例,在自旋栅栏后同时 createOrOpen 同一个新名字。
    constexpr int kRounds = 200;
    TempLockDir dir;

    int bothCreated = 0;
    int noneCreated = 0;
    int failed = 0;
    int notShared = 0;
    int leftover = 0;
    for (int round = 0; round < kRounds; ++round)
    {
        const auto name = uniqueName("race");
        const auto posix = toPosix(name);
        UnlinkOnExit guard{posix};
        SegmentBackendPosix b1;
        SegmentBackendPosix b2;
        SegmentView v1;
        SegmentView v2;
        InitResult r1 = InitResult::kFailed;
        InitResult r2 = InitResult::kFailed;
        std::atomic<int> arrived{0};
        const auto run = [&arrived, &name](SegmentBackendPosix& b, SegmentView& v, InitResult& r) {
            arrived.fetch_add(1, std::memory_order_acq_rel);
            while (arrived.load(std::memory_order_acquire) < 2)
            {
            }
            r = b.createOrOpen(name, kSeg, v);
        };
        std::thread t1(run, std::ref(b1), std::ref(v1), std::ref(r1));
        std::thread t2(run, std::ref(b2), std::ref(v2), std::ref(r2));
        t1.join();
        t2.join();

        if (r1 != InitResult::kOk || r2 != InitResult::kOk)
        {
            ++failed;
        }
        else
        {
            if (v1.created && v2.created)
            {
                ++bothCreated;
            }
            if (!v1.created && !v2.created)
            {
                ++noneCreated;
            }
            bytes(v1)[11] = static_cast<unsigned char>(round & 0xFF);
            if (bytes(v2)[11] != static_cast<unsigned char>(round & 0xFF))
            {
                ++notShared;
            }
        }
        b1.unmap(v1);
        b2.unmap(v2);
        if (shmExists(posix))
        {
            ++leftover;
        }
    }
    INFO("rounds " << kRounds);
    CHECK(failed == 0);
    CHECK(bothCreated == 0);
    CHECK(noneCreated == 0);
    CHECK(notShared == 0);
    CHECK(leftover == 0);
}

TEST_CASE("POSIX backend: two instances in one process exclude each other (flock is per open file, not fcntl)",
          "[posix][lifecycle]")
{
    TempLockDir dir;
    SegmentBackendPosix one;
    SegmentBackendPosix two;
    const auto name = uniqueName("same");
    const auto posix = toPosix(name);
    UnlinkOnExit guard{posix};

    SegmentView v1;
    REQUIRE(one.createOrOpen(name, kSeg, v1) == InitResult::kOk);
    REQUIRE(v1.created);
    // fcntl 记录锁按进程计:换成它的话,同进程的第二个实例会以为没有持有者,清掉活段再重建。
    SegmentView v2;
    REQUIRE(two.createOrOpen(name, kSeg, v2) == InitResult::kOk);
    CHECK_FALSE(v2.created);
    // 同一个后端实例再映射一次也一样:每个视图持有自己那一把锁。
    SegmentView v3;
    REQUIRE(one.createOrOpen(name, kSeg, v3) == InitResult::kOk);
    CHECK_FALSE(v3.created);

    bytes(v1)[42] = 0x42;
    CHECK(bytes(v2)[42] == 0x42);
    CHECK(bytes(v3)[42] == 0x42);

    one.unmap(v1);
    CHECK(shmExists(posix));
    two.unmap(v2);
    CHECK(shmExists(posix));
    one.unmap(v3);
    CHECK_FALSE(shmExists(posix));
}

TEST_CASE("POSIX backend: views dropped without unmap are released by the backend destructor", "[posix][lifecycle]")
{
    TempLockDir dir;
    const auto name = uniqueName("drop");
    const auto posix = toPosix(name);
    UnlinkOnExit guard{posix};

    SECTION("sole holder: the destructor is the last one out and unlinks")
    {
        {
            SegmentBackendPosix backend;
            SegmentView v1;
            SegmentView v2;
            REQUIRE(backend.createOrOpen(name, kSeg, v1) == InitResult::kOk);
            REQUIRE(backend.createOrOpen(name, kSeg, v2) == InitResult::kOk);
            // 丢掉视图、不 unmap —— 与 SegmentHandle 在宽限期届满前析构同形。
            v1.reset();
            v2.reset();
            CHECK(shmExists(posix));
        }
        CHECK_FALSE(shmExists(posix));
    }

    SECTION("SegmentHandle destroyed inside its grace period while another holder is alive")
    {
        SegmentBackendPosix other;
        SegmentView keep;
        REQUIRE(other.createOrOpen(name, kSeg, keep) == InitResult::kOk);
        {
            SegmentBackendPosix backend;
            SegmentView v;
            REQUIRE(backend.createOrOpen(name, kSeg, v) == InitResult::kOk);
            REQUIRE_FALSE(v.created);
            scvb::SegmentHandle handle(std::move(v), &backend);
            CHECK(handle.valid());
        } // handle 析构:宽限期未满,不 unmap;随后后端析构补做「离开」:还有别人持有 ⇒ 不 unlink
        CHECK(shmExists(posix));
        other.unmap(keep);
        CHECK_FALSE(shmExists(posix));
    }
}

TEST_CASE("POSIX backend: SegmentHandle release after the grace period unmaps and unlinks", "[posix][handle]")
{
    TempLockDir dir;
    SegmentBackendPosix backend;
    const auto name = uniqueName("handle");
    const auto posix = toPosix(name);
    UnlinkOnExit guard{posix};

    SegmentView v;
    REQUIRE(backend.createOrOpen(name, scvb::kRegistrySegmentSize, v) == InitResult::kOk);
    auto* hdr = static_cast<scvb::RegistryHeader*>(v.base);
    REQUIRE(backend.initHeader(v, &hdr->magic, &hdr->abi, &hdr->generation, sizeof(scvb::RegistryHeader)) ==
            InitResult::kOk);
    scvb::SegmentHandle handle(std::move(v), &backend);
    {
        auto lease = handle.lease();
        REQUIRE(static_cast<bool>(lease));
        CHECK_FALSE(handle.release(0)); // 租约在途
    }
    CHECK_FALSE(handle.release(1000)); // 宽限期起点
    CHECK(shmExists(posix));
    CHECK(handle.release(1000 + scvb::SegmentHandle::kReleaseGraceMs)); // 届满 → backend->unmap
    CHECK_FALSE(shmExists(posix));
}

// ---------------------------------------------------------------------------
// 错误与诊断
// ---------------------------------------------------------------------------

TEST_CASE("POSIX backend: EACCES is kFailed and reported to the diagnostics sink", "[posix][diag]")
{
    if (::geteuid() == 0)
    {
        SKIP("root bypasses file permission checks");
    }
    const auto name = uniqueName("acc");
    const auto posix = toPosix(name);
    UnlinkOnExit guard{posix};

    SECTION("lock directory is not writable")
    {
        TempLockDir dir;
        DiagRecorder diag;
        {
            // 先正常建过一次(lifecycle.lock 已在),这样失败点落在段锁文件上。
            SegmentBackendPosix warm;
            SegmentView w;
            const auto warmName = uniqueName("warm");
            REQUIRE(warm.createOrOpen(warmName, kSeg, w) == InitResult::kOk);
            warm.unmap(w);
        }
        REQUIRE(::chmod(dir.path().c_str(), 0500) == 0);
        SegmentBackendPosix backend;
        SegmentView v;
        CHECK(backend.createOrOpen(name, kSeg, v) == InitResult::kFailed);
        CHECK(v.base == nullptr);
        INFO(diag.dump());
        CHECK(diag.has(scvb::IpcDiagOp::kSegmentLock, EACCES, posix));
        CHECK_FALSE(shmExists(posix)); // 什么都没建
    }

    SECTION("segment is not writable for an attacher; the read-only path still opens it")
    {
        TempLockDir dir;
        DiagRecorder diag;
        createOrphan(posix, kSeg, 0x11, 0400); // 属主只读
        // 模拟一个活着的持有者:自己对段锁文件拿 SH。
        const int holder = ::open(dir.lockFile(posix).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        REQUIRE(holder >= 0);
        REQUIRE(::flock(holder, LOCK_SH) == 0);

        SegmentBackendPosix backend;
        SegmentView v;
        CHECK(backend.createOrOpen(name, kSeg, v) == InitResult::kFailed); // 附着要 O_RDWR → EACCES
        INFO(diag.dump());
        CHECK(diag.has(scvb::IpcDiagOp::kShmOpen, EACCES, posix));
        CHECK(shmStat(posix).size == kSeg); // 段原封不动

        // 只读打开只要 O_RDONLY,0400 允许。
        SegmentView r;
        REQUIRE(backend.openExistingReadOnly(name, r) == InitResult::kOk);
        CHECK(bytes(r)[5] == 0x11);
        backend.unmap(r);
        CHECK(shmExists(posix)); // holder 仍持 SH

        ::close(holder);
    }
}

TEST_CASE("IpcDiag: no-op by default, routes to an installed sink, restores the previous one", "[posix][diag]")
{
    const bool noSinkAtStart = (scvb::ipcDiagSink() == nullptr);
    CHECK(noSinkAtStart);
    scvb::IpcDiagEvent e;
    e.op = scvb::IpcDiagOp::kShmOpen;
    e.error = EACCES;
    e.segment = "/SCVBt.diag";
    e.detail = "test";
    scvb::reportIpcDiag(e); // 没有 sink:什么都不做

    {
        DiagRecorder diag;
        scvb::reportIpcDiag(e);
        CHECK(diag.count() == std::size_t{1});
        CHECK(diag.has(scvb::IpcDiagOp::kShmOpen, EACCES, "/SCVBt.diag"));
    }
    const bool noSinkAfterRecorder = (scvb::ipcDiagSink() == nullptr);
    CHECK(noSinkAfterRecorder);

    for (const auto op :
         {scvb::IpcDiagOp::kName, scvb::IpcDiagOp::kLockDir, scvb::IpcDiagOp::kLifecycleLock,
          scvb::IpcDiagOp::kSegmentLock, scvb::IpcDiagOp::kShmOpen, scvb::IpcDiagOp::kShmTruncate,
          scvb::IpcDiagOp::kShmStat, scvb::IpcDiagOp::kShmMap, scvb::IpcDiagOp::kShmUnlink, scvb::IpcDiagOp::kMlock})
    {
        CHECK(std::string(scvb::ipcDiagOpName(op)) != "unknown");
    }
}

TEST_CASE("POSIX backend: the lock directory defaults to the real home, not TMPDIR; the env override wins",
          "[posix][lockdir]")
{
    {
        EnvVarGuard env(SegmentBackendPosix::kLockDirEnvVar);
        ::unsetenv(SegmentBackendPosix::kLockDirEnvVar);
        SegmentBackendPosix backend; // 只读出路径,不建任何东西(建目录只发生在 createOrOpen 里)

        const struct passwd* pw = ::getpwuid(::getuid());
        REQUIRE(pw != nullptr);
        REQUIRE(pw->pw_dir != nullptr);
        const std::string expected = std::string(pw->pw_dir) + "/Library/Application Support/Synchain/SCVB/ipc";
        CHECK(backend.lockDir() == expected);
        CHECK(SegmentBackendPosix::defaultLockDir() == expected);
        const char* tmp = std::getenv("TMPDIR");
        if (tmp != nullptr && tmp[0] != '\0')
        {
            INFO("TMPDIR " << tmp);
            CHECK(backend.lockDir().rfind(tmp, 0) != 0);
        }
    }
    TempLockDir dir;
    SegmentBackendPosix overridden;
    CHECK(overridden.lockDir() == dir.path());
}

// ---------------------------------------------------------------------------
// test_ipc_lifecycle 的后端无关场景:InProcess 与 Posix 各跑一遍。
// Posix 一侧经改名层把冻结前缀 "Local\SynchainSCVB.v1." 换成带 pid 的 "Local\T<pid>s<n>.",真正的
// shm / flock 由 SegmentBackendPosix 做;场景结束、后端析构之后,核对用过的每个段都已撤掉 ——
// 这正是插件卸载的路径(Registry / CtrlPlane 析构时 SegmentHandle 还在宽限期里,不 unmap,
// 「离开」由后端析构补做)。
// ---------------------------------------------------------------------------

namespace
{
class PidScopedPosixBackend final : public scvb::ISegmentBackend
{
public:
    PidScopedPosixBackend()
    {
        static int counter = 0;
        ++counter;
        prefix_ = L"Local\\T" + widen(pidTag()) + L"s" + std::to_wstring(counter) + L".";
    }

    InitResult createOrOpen(const std::wstring& name, std::size_t size, SegmentView& view) override
    {
        return inner_.createOrOpen(scoped(name), size, view);
    }
    InitResult openExisting(const std::wstring& name, SegmentView& view) override
    {
        return inner_.openExisting(scoped(name), view);
    }
    InitResult openExistingReadOnly(const std::wstring& name, SegmentView& view) override
    {
        return inner_.openExistingReadOnly(scoped(name), view);
    }
    void unmap(SegmentView& view) override { inner_.unmap(view); }
    void tryLock(const SegmentView& view) override { inner_.tryLock(view); }

    const std::set<std::string>& touched() const { return touched_; }

private:
    std::wstring scoped(const std::wstring& name)
    {
        static const std::wstring kFrozen = L"Local\\SynchainSCVB.v1.";
        REQUIRE(name.compare(0, kFrozen.size(), kFrozen) == 0);
        std::wstring s = prefix_ + name.substr(kFrozen.size());
        touched_.insert(toPosix(s));
        return s;
    }

    std::wstring prefix_;
    std::set<std::string> touched_;
    SegmentBackendPosix inner_; // 最后声明:构造时 SCVB_IPC_LOCK_DIR 已由外层设好
};

using Scenario = std::function<void(scvb::ISegmentBackend&)>;

void runOnBothBackends(const Scenario& scenario)
{
    SECTION("InProcess")
    {
        scvb::SegmentBackendInProcess::resetAll();
        {
            scvb::SegmentBackendInProcess backend;
            scenario(backend);
        }
        scvb::SegmentBackendInProcess::resetAll();
    }
    SECTION("Posix")
    {
        TempLockDir dir;
        std::set<std::string> touched;
        {
            PidScopedPosixBackend backend;
            scenario(backend);
            touched = backend.touched();
        } // 场景里的 Registry / CtrlPlane 已析构;后端析构补做它们丢下的视图的「离开」
        CHECK_FALSE(touched.empty());
        for (const auto& n : touched)
        {
            INFO("segment " << n);
            CHECK_FALSE(shmExists(n));
        }
    }
}

using Claim = scvb::Registry::ClaimResult;
} // namespace

TEST_CASE("Lifecycle on both backends: 4.3-a cold start, Output first", "[posix][lifecycle][backends]")
{
    runOnBothBackends([](scvb::ISegmentBackend& backend) {
        scvb::Registry out(backend, 1);
        REQUIRE(out.open() == Claim::kClaimed);
        REQUIRE(out.generation() == 1);
        REQUIRE(out.claimOutput(2001, kT0) == Claim::kClaimed);
        REQUIRE(out.outputSlot()->state.load() == scvb::kSlotActive);

        scvb::Registry in(backend, 1);
        REQUIRE(in.open() == Claim::kClaimed);
        REQUIRE(in.generation() == 1); // 附着方不重初始化
        REQUIRE(in.claimInput(3, 1001, 48000, 512, kT0) == Claim::kClaimed);
        REQUIRE(in.inputSlot(3)->state.load() == scvb::kSlotActive);

        out.setConnectedMaskBit(3);
        REQUIRE(out.connectedMask() == (1u << 2));
        REQUIRE(in.connectedMask() == (1u << 2));
    });
}

TEST_CASE("Lifecycle on both backends: 4.3-c same channel in a group -- only one is active",
          "[posix][lifecycle][backends]")
{
    runOnBothBackends([](scvb::ISegmentBackend& backend) {
        scvb::Registry in1(backend, 1);
        REQUIRE(in1.open() == Claim::kClaimed);
        REQUIRE(in1.claimInput(3, 1001, 48000, 512, kT0) == Claim::kClaimed);
        in1.heartbeatInput(3, kT0 + 100);

        scvb::Registry in2(backend, 1);
        REQUIRE(in2.open() == Claim::kClaimed);
        REQUIRE(in2.claimInput(3, 1002, 48000, 512, kT0 + 200) == Claim::kConflict);
        REQUIRE(in1.inputSlot(3)->pid == 1001);
        REQUIRE(in2.inputSlot(3)->pid == 1001); // 两个实例看的是同一块内存
        REQUIRE(in1.inputSlot(3)->state.load() == scvb::kSlotActive);
    });
}

TEST_CASE("Lifecycle on both backends: 4.3-e stale slot of a dead holder is taken over", "[posix][lifecycle][backends]")
{
    runOnBothBackends([](scvb::ISegmentBackend& backend) {
        scvb::Registry in1(backend, 1);
        REQUIRE(in1.open() == Claim::kClaimed);
        REQUIRE(in1.claimInput(3, 1001, 48000, 512, kT0) == Claim::kClaimed);

        scvb::Registry in2(backend, 1, [](u32) { return false; }); // 持有者 pid 已死
        REQUIRE(in2.open() == Claim::kClaimed);
        REQUIRE(in2.claimInput(3, 1002, 48000, 512, kT0 + 5100) == Claim::kClaimed);
        REQUIRE(in2.inputSlot(3)->pid == 1002);
        REQUIRE(in1.inputSlot(3)->pid == 1002);
    });
}

TEST_CASE("Lifecycle on both backends: 4.3-g unloading one plugin frees its slot", "[posix][lifecycle][backends]")
{
    runOnBothBackends([](scvb::ISegmentBackend& backend) {
        scvb::Registry out(backend, 1);
        REQUIRE(out.open() == Claim::kClaimed);
        {
            scvb::Registry in(backend, 1);
            REQUIRE(in.open() == Claim::kClaimed);
            REQUIRE(in.claimInput(3, 1001, 48000, 512, kT0) == Claim::kClaimed);
            REQUIRE(out.inputSlot(3)->state.load() == scvb::kSlotActive);
        } // 析构 = 卸载:释放 slot
        REQUIRE(out.inputSlot(3)->state.load() == scvb::kSlotFree);
    });
}

TEST_CASE("Lifecycle on both backends: J66 groups are isolated and changeGroup moves the claim",
          "[posix][lifecycle][backends]")
{
    runOnBothBackends([](scvb::ISegmentBackend& backend) {
        scvb::Registry g1in(backend, 1);
        REQUIRE(g1in.open() == Claim::kClaimed);
        REQUIRE(g1in.claimInput(3, 1001, 48000, 512, kT0) == Claim::kClaimed);
        scvb::Registry g1out(backend, 1);
        REQUIRE(g1out.open() == Claim::kClaimed);
        REQUIRE(g1out.claimOutput(2001, kT0) == Claim::kClaimed);
        g1out.setConnectedMaskBit(3);

        scvb::Registry g2in(backend, 2);
        REQUIRE(g2in.open() == Claim::kClaimed);
        REQUIRE(g2in.claimInput(3, 1002, 48000, 512, kT0) == Claim::kClaimed); // 同号 channel,零冲突
        scvb::Registry g2out(backend, 2);
        REQUIRE(g2out.open() == Claim::kClaimed);
        REQUIRE(g2out.claimOutput(2002, kT0) == Claim::kClaimed);
        REQUIRE(g1out.connectedMask() == (1u << 2));
        REQUIRE(g2out.connectedMask() == 0);

        // 改组:旧组 slot 归零,新组照常认领。
        scvb::Registry mover(backend, 3);
        REQUIRE(mover.open() == Claim::kClaimed);
        REQUIRE(mover.claimInput(5, 1003, 48000, 512, kT0) == Claim::kClaimed);
        REQUIRE(mover.changeGroup(4) == Claim::kClaimed);
        REQUIRE(mover.group() == 4);
        scvb::Registry probe(backend, 3);
        REQUIRE(probe.open() == Claim::kClaimed);
        REQUIRE(probe.inputSlot(5)->state.load() == scvb::kSlotFree);
        REQUIRE(mover.claimInput(5, 1003, 48000, 512, kT0) == Claim::kClaimed);
    });
}

TEST_CASE("Lifecycle on both backends: J40 abi mismatch refuses to connect", "[posix][lifecycle][backends]")
{
    runOnBothBackends([](scvb::ISegmentBackend& backend) {
        SegmentView view;
        REQUIRE(backend.createOrOpen(L"Local\\SynchainSCVB.v1.g1.registry", scvb::kRegistrySegmentSize, view) ==
                InitResult::kOk);
        auto* hdr = static_cast<scvb::RegistryHeader*>(view.base);
        hdr->magic.store(scvb::kScvbMagic, std::memory_order_release);
        hdr->abi.store(99, std::memory_order_release);

        u32 remote = 0;
        scvb::Registry reg(backend, 1);
        reg.setAbiMismatchHandler([&remote](u32, u32 r) { remote = r; });
        REQUIRE(reg.open() == Claim::kAbiMismatch); // 失败路径里 Registry 自己 unmap 了它的视图
        REQUIRE(remote == 99);
        REQUIRE_FALSE(reg.isOpen());
        backend.unmap(view);
    });
}

TEST_CASE("Lifecycle on both backends: OutputGlobalInfo written by Output is read by Input",
          "[posix][lifecycle][backends]")
{
    runOnBothBackends([](scvb::ISegmentBackend& backend) {
        scvb::CtrlPlane out(backend, 1);
        REQUIRE(out.open() == InitResult::kOk);
        scvb::CtrlPlane in(backend, 1);
        REQUIRE(in.open() == InitResult::kOk);

        scvb::OutputGlobalInfoSnapshot s;
        s.capture_enabled = 1;
        s.output_sample_rate = 48000;
        s.flags = scvb::kOutputEnabled;
        for (u32 i = 0; i < scvb::kMaxChannels; ++i)
        {
            s.gap_count[i] = i + 100;
            s.overlap_count[i] = i + 200;
            s.epoch_summary[i] = i + 300;
        }
        out.refreshGlobalInfo(s);

        const auto got = in.readGlobalInfo();
        REQUIRE(got.capture_enabled == 1);
        REQUIRE(got.output_sample_rate == 48000);
        REQUIRE(got.flags == scvb::kOutputEnabled);
        REQUIRE(got.gap_count[14] == 114);
        REQUIRE(got.overlap_count[14] == 214);
        REQUIRE(got.epoch_summary[14] == 314);
    });
}

TEST_CASE("Lifecycle on both backends: SegmentHandle lease defers release until the grace period ends",
          "[posix][lifecycle][backends]")
{
    runOnBothBackends([](scvb::ISegmentBackend& backend) {
        SegmentView v;
        REQUIRE(backend.createOrOpen(L"Local\\SynchainSCVB.v1.g1.registry", scvb::kRegistrySegmentSize, v) ==
                InitResult::kOk);
        auto* hdr = static_cast<scvb::RegistryHeader*>(v.base);
        REQUIRE(backend.initHeader(v, &hdr->magic, &hdr->abi, &hdr->generation, sizeof(scvb::RegistryHeader)) ==
                InitResult::kOk);
        scvb::SegmentHandle handle(std::move(v), &backend);
        const void* base = handle.base();
        REQUIRE(base != nullptr);
        {
            auto lease = handle.lease();
            REQUIRE(static_cast<bool>(lease));
            REQUIRE(lease.base() == base);
            REQUIRE_FALSE(handle.release(0));
            REQUIRE_FALSE(handle.valid());
        }
        REQUIRE_FALSE(handle.release(1000));
        REQUIRE(handle.release(1000 + scvb::SegmentHandle::kReleaseGraceMs));
        REQUIRE(handle.base() == nullptr);
    });
}

// ---------------------------------------------------------------------------
// 隐藏格:本机有没有 SCVB 段残留(M08 在 auval / pluginval 跑完后用 `scvb_tests "[shm-leftover]"` 调)。
// 逐个只读探测全部 8 组 × (registry / ctrl / viz / audio.ch1..15 / feat.ch1..15) 的真实段名,
// 存在(或因属于别的用户而 EACCES)就记为残留。macOS 没有枚举 POSIX shm 的接口,只能按名字探。
// ---------------------------------------------------------------------------

TEST_CASE("No SCVB shared-memory segment is left on this machine", "[.][shm-leftover]")
{
    std::vector<std::wstring> names;
    for (u32 g = 1; g <= scvb::kMaxGroups; ++g)
    {
        names.push_back(scvb::segmentRegistryName(g));
        names.push_back(scvb::segmentCtrlName(g));
        names.push_back(L"Local\\" + scvb::segmentLogicalName(g, scvb::SegmentKind::kViz));
        for (u32 ch = 1; ch <= scvb::kMaxChannels; ++ch)
        {
            names.push_back(scvb::segmentAudioName(g, ch));
            names.push_back(scvb::segmentFeatName(g, ch));
        }
    }

    std::vector<std::string> present;
    for (const auto& n : names)
    {
        const std::string posix = toPosix(n);
        const int fd = ::shm_open(posix.c_str(), O_RDONLY, 0);
        if (fd >= 0)
        {
            struct stat st
            {
            };
            const std::string size = (::fstat(fd, &st) == 0) ? std::to_string(st.st_size) : std::string("?");
            ::close(fd);
            present.push_back(posix + " (size " + size + ")");
        }
        else
        {
            const int err = errno;
            if (err != ENOENT)
            {
                present.push_back(posix + " (errno " + std::to_string(err) + ")");
            }
        }
    }

    std::string list;
    for (const auto& p : present)
    {
        list += "  " + p + "\n";
    }
    INFO("probed " << names.size() << " segment names; leftovers:\n" << list);
    CHECK(names.size() == std::size_t{scvb::kMaxGroups} * (3u + 2u * scvb::kMaxChannels));
    CHECK(present.empty());
}
