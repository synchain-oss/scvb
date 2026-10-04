// SPDX-License-Identifier: GPL-3.0-or-later
#include "SegmentBackendPosix.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h> // flock
#include <sys/mman.h> // shm_open / shm_unlink / mmap / munmap / mlock
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "IpcDiag.h"

namespace scvb
{

// 后端私有的映射记录。SegmentView::mapping 指向它;同一后端实例的所有在用记录串成双向链表,
// 后端析构时据此补做「离开」(见头注)。
struct SegmentBackendPosix::Mapping
{
    int lockFd = -1; // 段锁文件的 fd,持 LOCK_SH 直到离开
    std::string shmName; // POSIX 名 "/<逻辑名>"
    std::string lockDir; // 建立映射时的锁目录:离开时用同一把全局锁
    void* base = nullptr;
    std::size_t size = 0;
    Mapping* prev = nullptr;
    Mapping* next = nullptr;
};

namespace
{
constexpr wchar_t kWinPrefix[] = L"Local\\";
constexpr std::size_t kWinPrefixLength = sizeof(kWinPrefix) / sizeof(kWinPrefix[0]) - 1;

// 全局生命周期锁的有界等待。持锁者只做几次系统调用就放;等满仍拿不到,说明有进程挂在锁里,
// 宁可这一次 kFailed(调用方本来就会周期重试),也不把消息线程无限期卡住。
constexpr auto kLifecycleLockTimeout = std::chrono::milliseconds(2000);
constexpr auto kLifecycleLockPoll = std::chrono::milliseconds(1);

void report(IpcDiagOp op, int error, const std::string& segment, const char* detail) noexcept
{
    IpcDiagEvent e;
    e.op = op;
    e.error = error;
    e.segment = segment.c_str();
    e.detail = detail;
    reportIpcDiag(e);
}

bool isAllowedNameChar(wchar_t c) noexcept
{
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' ||
           c == L'-';
}

int openRetry(const char* path, int flags, int mode) noexcept
{
    int fd = -1;
    do
    {
        fd = ::open(path, flags, mode);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

int flockRetry(int fd, int operation) noexcept
{
    int r = -1;
    do
    {
        r = ::flock(fd, operation);
    } while (r != 0 && errno == EINTR);
    return r;
}

void closeKeepErrno(int fd) noexcept
{
    if (fd >= 0)
    {
        const int saved = errno;
        ::close(fd);
        errno = saved;
    }
}

bool isDirectory(const std::string& path, int& err)
{
    struct stat st
    {
    };
    if (::stat(path.c_str(), &st) != 0)
    {
        err = errno;
        return false;
    }
    if (!S_ISDIR(st.st_mode))
    {
        err = ENOTDIR;
        return false;
    }
    return true;
}

// mkdir -p(新建的每一级 0700;已存在的不动)。失败时 err = errno。
bool ensureDirectory(const std::string& path, int& err)
{
    if (path.empty() || path[0] != '/')
    {
        err = EINVAL;
        return false;
    }
    if (isDirectory(path, err))
    {
        return true; // 常态:锁目录早已建好,不碰上级目录
    }
    std::size_t pos = 1;
    while (pos <= path.size())
    {
        const std::size_t slash = path.find('/', pos);
        const std::size_t end = (slash == std::string::npos) ? path.size() : slash;
        if (end > pos)
        {
            const std::string prefix = path.substr(0, end);
            int statErr = 0;
            if (!isDirectory(prefix, statErr))
            {
                if (::mkdir(prefix.c_str(), 0700) != 0 && errno != EEXIST)
                {
                    err = errno;
                    return false;
                }
            }
        }
        if (slash == std::string::npos)
        {
            break;
        }
        pos = slash + 1;
    }
    return isDirectory(path, err);
}

// 全局 lifecycle.lock 的作用域持有(LOCK_EX,有界等待)。create=false 时文件不存在就不建
// (只读 / 附着路径:锁目录里从没建过任何东西,说明也不可能有段)。
class LifecycleLock
{
public:
    LifecycleLock(const std::string& lockDir, bool create)
    {
        const std::string path = lockDir + "/" + SegmentBackendPosix::kLifecycleLockName;
        const int flags = create ? (O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW) : (O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        fd_ = openRetry(path.c_str(), flags, 0600);
        if (fd_ < 0)
        {
            error_ = errno;
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + kLifecycleLockTimeout;
        for (;;)
        {
            if (flockRetry(fd_, LOCK_EX | LOCK_NB) == 0)
            {
                held_ = true;
                return;
            }
            if (errno != EWOULDBLOCK)
            {
                error_ = errno;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                error_ = ETIMEDOUT;
                break;
            }
            std::this_thread::sleep_for(kLifecycleLockPoll);
        }
        ::close(fd_);
        fd_ = -1;
    }

    ~LifecycleLock()
    {
        if (fd_ >= 0)
        {
            ::close(fd_); // 关 fd 即释放 flock
        }
    }

    LifecycleLock(const LifecycleLock&) = delete;
    LifecycleLock& operator=(const LifecycleLock&) = delete;

    bool held() const noexcept { return held_; }
    int error() const noexcept { return error_; }

private:
    int fd_ = -1;
    int error_ = 0;
    bool held_ = false;
};

// 无主段的属主检查:只清自己(有效 uid)名下的段。POSIX shm 是全机命名空间,锁目录却在各自的 home 里,
// 所以另一个用户正在用的段,在本用户的锁看来也是「没有持有者」;不查就会把别人的活段 unlink 掉。
// 返回 0 = 可以清(段不存在,或属于自己);否则返回要上报的 errno(EACCES 等)。
int orphanRemovalBlocker(const std::string& shm) noexcept
{
    const int fd = ::shm_open(shm.c_str(), O_RDONLY, 0);
    if (fd < 0)
    {
        return errno == ENOENT ? 0 : errno;
    }
    struct stat st
    {
    };
    const int r = ::fstat(fd, &st);
    const int statErr = errno;
    ::close(fd);
    if (r != 0)
    {
        return statErr;
    }
    return st.st_uid == ::geteuid() ? 0 : EACCES;
}

std::string resolveLockDir()
{
    const char* overrideDir = std::getenv(SegmentBackendPosix::kLockDirEnvVar);
    if (overrideDir != nullptr && overrideDir[0] != '\0')
    {
        return std::string(overrideDir);
    }
    return SegmentBackendPosix::defaultLockDir();
}
} // namespace

std::wstring segmentRegistryName(u32 group)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kRegistry);
}

std::wstring segmentAudioName(u32 group, u32 channel)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kAudio, channel);
}

std::wstring segmentFeatName(u32 group, u32 channel)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kFeat, channel);
}

std::wstring segmentCtrlName(u32 group)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kCtrl);
}

SegmentBackendPosix::NameStatus SegmentBackendPosix::toPosixName(const std::wstring& name, std::string& posixName)
{
    const bool prefixed = name.compare(0, kWinPrefixLength, kWinPrefix) == 0;
    const std::size_t start = prefixed ? kWinPrefixLength : 0;
    posixName.assign(1, '/');
    bool badChar = false;
    for (std::size_t i = start; i < name.size(); ++i)
    {
        const wchar_t c = name[i];
        if (isAllowedNameChar(c))
        {
            posixName.push_back(static_cast<char>(c));
        }
        else
        {
            posixName.push_back('?');
            badChar = true;
        }
    }
    if (!prefixed)
    {
        return NameStatus::kBadPrefix;
    }
    const std::string logical = posixName.substr(1);
    if (badChar || logical.empty() || logical == "." || logical == "..")
    {
        return NameStatus::kBadChar;
    }
    if (posixName.size() > kMaxShmNameLength)
    {
        return NameStatus::kTooLong;
    }
    return NameStatus::kOk;
}

std::string SegmentBackendPosix::defaultLockDir()
{
    // 真实 home 取自密码库(getpwuid_r),不取 HOME:宿主若被容器化,HOME 会指进容器,而同一用户的
    // 别的进程看到的是另一个目录 —— 锁就不再是同一把。密码库查不到时才退回 HOME(必须是绝对路径)。
    std::string home;
    long bufSize = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufSize <= 0)
    {
        bufSize = 16384;
    }
    std::vector<char> buf(static_cast<std::size_t>(bufSize));
    struct passwd pw
    {
    };
    struct passwd* result = nullptr;
    if (::getpwuid_r(::getuid(), &pw, buf.data(), buf.size(), &result) == 0 && result != nullptr &&
        result->pw_dir != nullptr && result->pw_dir[0] == '/')
    {
        home = result->pw_dir;
    }
    else
    {
        const char* envHome = std::getenv("HOME");
        if (envHome != nullptr && envHome[0] == '/')
        {
            home = envHome;
        }
    }
    if (home.empty())
    {
        return {};
    }
    while (home.size() > 1 && home.back() == '/')
    {
        home.pop_back();
    }
    return home + "/" + kHomeRelativeLockDir;
}

SegmentBackendPosix::SegmentBackendPosix() : lockDir_(resolveLockDir()) {}

SegmentBackendPosix::~SegmentBackendPosix()
{
    // 视图没经 unmap 就被丢掉(SegmentHandle 在宽限期届满前析构):映射留到进程退出,锁这一半在这里补做。
    for (;;)
    {
        Mapping* m = nullptr;
        {
            std::lock_guard<std::mutex> lock(liveMutex_);
            m = live_;
            if (m != nullptr)
            {
                live_ = m->next;
                if (live_ != nullptr)
                {
                    live_->prev = nullptr;
                }
            }
        }
        if (m == nullptr)
        {
            break;
        }
        leave(*m);
        delete m;
    }
}

void SegmentBackendPosix::track(Mapping* m) noexcept
{
    std::lock_guard<std::mutex> lock(liveMutex_);
    m->prev = nullptr;
    m->next = live_;
    if (live_ != nullptr)
    {
        live_->prev = m;
    }
    live_ = m;
}

void SegmentBackendPosix::untrack(Mapping* m) noexcept
{
    std::lock_guard<std::mutex> lock(liveMutex_);
    if (m->prev != nullptr)
    {
        m->prev->next = m->next;
    }
    else if (live_ == m)
    {
        live_ = m->next;
    }
    if (m->next != nullptr)
    {
        m->next->prev = m->prev;
    }
    m->prev = nullptr;
    m->next = nullptr;
}

InitResult SegmentBackendPosix::createOrOpen(const std::wstring& name, std::size_t size, SegmentView& view)
{
    return openSegment(name, size, view, Mode::kCreateOrOpen);
}

InitResult SegmentBackendPosix::openExisting(const std::wstring& name, SegmentView& view)
{
    return openSegment(name, 0, view, Mode::kOpenExisting);
}

InitResult SegmentBackendPosix::openExistingReadOnly(const std::wstring& name, SegmentView& view)
{
    return openSegment(name, 0, view, Mode::kOpenReadOnly);
}

InitResult SegmentBackendPosix::openSegment(const std::wstring& name, std::size_t size, SegmentView& view, Mode mode)
{
    view.reset();
    const bool create = (mode == Mode::kCreateOrOpen);
    if (create && size == 0)
    {
        return InitResult::kFailed;
    }

    std::string shm;
    switch (toPosixName(name, shm))
    {
    case NameStatus::kOk:
        break;
    case NameStatus::kTooLong:
        report(IpcDiagOp::kName, ENAMETOOLONG, shm, "POSIX shm name longer than PSHMNAMLEN (31)");
        return InitResult::kFailed;
    case NameStatus::kBadPrefix:
        report(IpcDiagOp::kName, EINVAL, shm, "segment name must start with Local\\");
        return InitResult::kFailed;
    case NameStatus::kBadChar:
        report(IpcDiagOp::kName, EINVAL, shm, "segment name must be ASCII [A-Za-z0-9._-], not . or ..");
        return InitResult::kFailed;
    }

    if (lockDir_.empty())
    {
        report(IpcDiagOp::kLockDir, ENOENT, shm, "cannot resolve the lock directory (no home directory)");
        return InitResult::kFailed;
    }
    if (create)
    {
        int err = 0;
        if (!ensureDirectory(lockDir_, err))
        {
            report(IpcDiagOp::kLockDir, err, shm, "cannot create the lock directory");
            return InitResult::kFailed;
        }
    }

    // ── 全局生命周期锁:从这里到函数返回,本进程与别的进程都插不进任何转换 ──
    LifecycleLock life(lockDir_, create);
    if (!life.held())
    {
        if (!create && life.error() == ENOENT)
        {
            return InitResult::kFailed; // 锁目录里从没建过东西 ⇒ 不可能有段;安静地等下一轮
        }
        report(IpcDiagOp::kLifecycleLock, life.error(), shm, "cannot acquire lifecycle.lock");
        return InitResult::kFailed;
    }

    const std::string lockPath = lockDir_ + "/" + shm.substr(1) + ".lock";
    const int lockFlags = create ? (O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW) : (O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const int lockFd = openRetry(lockPath.c_str(), lockFlags, 0600);
    if (lockFd < 0)
    {
        const int err = errno;
        if (!create && err == ENOENT)
        {
            return InitResult::kFailed; // 这个段从没被建过
        }
        report(IpcDiagOp::kSegmentLock, err, shm, "cannot open the segment lock file");
        return InitResult::kFailed;
    }

    bool created = false;
    int shmFd = -1;
    if (flockRetry(lockFd, LOCK_EX | LOCK_NB) == 0)
    {
        // 没有活着的持有者。
        if (!create)
        {
            // 附着 / 只读路径:等同 Windows 上段已随最后一个句柄消失。不碰残段,留给下一个创建者清理。
            ::close(lockFd);
            return InitResult::kFailed;
        }
        // 清残段(崩溃留下的、旧尺寸的),再独占新建。只清自己名下的(见 orphanRemovalBlocker)。
        if (const int blocker = orphanRemovalBlocker(shm); blocker != 0)
        {
            ::close(lockFd);
            report(IpcDiagOp::kShmOpen, blocker, shm,
                   "segment exists but is not ours (another user?); not touching it");
            return InitResult::kFailed;
        }
        if (::shm_unlink(shm.c_str()) != 0 && errno != ENOENT)
        {
            const int err = errno;
            ::close(lockFd);
            report(IpcDiagOp::kShmUnlink, err, shm, "cannot remove an orphaned segment before re-creating it");
            return InitResult::kFailed;
        }
        shmFd = ::shm_open(shm.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (shmFd < 0)
        {
            const int err = errno;
            ::close(lockFd);
            report(IpcDiagOp::kShmOpen, err, shm, "shm_open(O_CREAT|O_EXCL) failed");
            return InitResult::kFailed;
        }
        if (::ftruncate(shmFd, static_cast<off_t>(size)) != 0)
        {
            const int err = errno;
            ::close(shmFd);
            ::shm_unlink(shm.c_str());
            ::close(lockFd);
            report(IpcDiagOp::kShmTruncate, err, shm, "ftruncate on a freshly created segment failed");
            return InitResult::kFailed;
        }
        created = true;
        // EX → SH。BSD flock 的转换不是原子的(先放 EX 再上 SH),所以必须在全局锁下做。
        if (flockRetry(lockFd, LOCK_SH | LOCK_NB) != 0)
        {
            const int err = errno;
            ::close(shmFd);
            ::shm_unlink(shm.c_str());
            ::close(lockFd);
            report(IpcDiagOp::kSegmentLock, err, shm, "cannot downgrade the segment lock to shared");
            return InitResult::kFailed;
        }
    }
    else
    {
        const int err = errno;
        if (err != EWOULDBLOCK)
        {
            ::close(lockFd);
            report(IpcDiagOp::kSegmentLock, err, shm, "flock probe on the segment lock file failed");
            return InitResult::kFailed;
        }
        // 有活持有者:附着。先拿 SH(全局锁下只有 SH 持有者,所以不会等),再打开段;绝不 ftruncate。
        if (flockRetry(lockFd, LOCK_SH | LOCK_NB) != 0)
        {
            const int shErr = errno;
            ::close(lockFd);
            report(IpcDiagOp::kSegmentLock, shErr, shm, "cannot take the shared segment lock");
            return InitResult::kFailed;
        }
        const int oflag = (mode == Mode::kOpenReadOnly) ? O_RDONLY : O_RDWR;
        shmFd = ::shm_open(shm.c_str(), oflag, 0);
        if (shmFd < 0)
        {
            const int openErr = errno;
            ::close(lockFd);
            report(IpcDiagOp::kShmOpen, openErr, shm,
                   openErr == ENOENT ? "segment missing although its lock is held" : "shm_open(attach) failed");
            return InitResult::kFailed;
        }
        // [INJECT I3] attacher resizes the segment
        if (create && ::ftruncate(shmFd, static_cast<off_t>(size)) != 0)
        {
            const int truncErr = errno;
            ::close(shmFd);
            ::close(lockFd);
            report(IpcDiagOp::kShmTruncate, truncErr, shm, "[INJECT I3] attacher ftruncate failed");
            return InitResult::kFailed;
        }
    }

    // 失败清理:自己刚建的段要撤掉(此刻只有自己持锁),附着的不动。
    const auto failAfterOpen = [&](IpcDiagOp op, int err, const char* detail) {
        if (shmFd >= 0)
        {
            ::close(shmFd);
        }
        if (created)
        {
            ::shm_unlink(shm.c_str());
        }
        ::close(lockFd);
        report(op, err, shm, detail);
        return InitResult::kFailed;
    };

    struct stat st
    {
    };
    if (::fstat(shmFd, &st) != 0)
    {
        return failAfterOpen(IpcDiagOp::kShmStat, errno, "fstat on the segment failed");
    }
    if (st.st_size <= 0)
    {
        return failAfterOpen(IpcDiagOp::kShmStat, EINVAL, "segment has no size");
    }
    const auto realSize = static_cast<std::size_t>(st.st_size);
    const int prot = (mode == Mode::kOpenReadOnly) ? PROT_READ : (PROT_READ | PROT_WRITE);
    void* base = ::mmap(nullptr, realSize, prot, MAP_SHARED, shmFd, 0);
    if (base == MAP_FAILED)
    {
        return failAfterOpen(IpcDiagOp::kShmMap, errno, "mmap of the segment failed");
    }
    ::close(shmFd); // 映射建立后 shm 的 fd 不再需要
    shmFd = -1;

    auto* m = new (std::nothrow) Mapping();
    if (m == nullptr)
    {
        ::munmap(base, realSize);
        return failAfterOpen(IpcDiagOp::kShmMap, ENOMEM, "out of memory for the mapping record");
    }
    m->lockFd = lockFd;
    m->shmName = shm;
    m->lockDir = lockDir_;
    m->base = base;
    m->size = realSize;
    track(m);

    view.base = base;
    view.size = realSize;
    view.mapping = m;
    view.created = created;
    // 锁页推迟到 initHeader 的 created 分支(写触碰之后再锁,与 Win32 同序);附着方不锁。
    return InitResult::kOk;
}

void SegmentBackendPosix::leave(Mapping& m)
{
    LifecycleLock life(m.lockDir, /*create=*/false);
    if (life.held())
    {
        // SH → EX 的探测(转换不是原子的:先放 SH 再试 EX;失败时 SH 已经放掉,这正是离开要的)。
        if (flockRetry(m.lockFd, LOCK_EX | LOCK_NB) == 0)
        {
            // 最后一个离开者:撤掉名字。别的进程里已有的映射(若有)不受影响。
            if (::shm_unlink(m.shmName.c_str()) != 0 && errno != ENOENT)
            {
                report(IpcDiagOp::kShmUnlink, errno, m.shmName, "last holder could not unlink the segment");
            }
        }
    }
    else
    {
        // 拿不到全局锁就不做 unlink 判定(否则可能与别人的建段交错);段留给下一个创建者清理。
        report(IpcDiagOp::kLifecycleLock, life.error(), m.shmName,
               "leaving without lifecycle.lock; the segment is left for the next creator to clean up");
    }
    // 关锁 fd 即放掉剩下的锁;此刻全局锁(若拿到)仍在手里,整个离开转换落在锁内。
    closeKeepErrno(m.lockFd);
    m.lockFd = -1;
}

void SegmentBackendPosix::unmap(SegmentView& view)
{
    auto* m = static_cast<Mapping*>(view.mapping);
    if (m != nullptr)
    {
        if (m->base != nullptr)
        {
            ::munmap(m->base, m->size);
        }
        untrack(m);
        leave(*m);
        delete m;
    }
    else if (view.base != nullptr && view.size != 0)
    {
        ::munmap(view.base, view.size);
    }
    view.base = nullptr;
    view.mapping = nullptr;
    view.size = 0;
    view.created = false;
}

void SegmentBackendPosix::tryLock(const SegmentView& view)
{
    if (view.base == nullptr || view.size == 0)
    {
        return;
    }
    if (::mlock(view.base, view.size) != 0)
    {
        // 尽力而为:内存已在 initHeader 里写触碰过,锁不住只上报、不失败(01 §4.0,与 Win32 VirtualLock 同口径)。
        const int err = errno;
        const auto* m = static_cast<const Mapping*>(view.mapping);
        report(IpcDiagOp::kMlock, err, m != nullptr ? m->shmName : std::string(), "mlock failed; pages stay pageable");
    }
}

} // namespace scvb
