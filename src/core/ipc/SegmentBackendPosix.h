// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SegmentBackendPosix —— macOS 的共享内存段后端(B 线 M05):shm_open / mmap + flock 生命周期锁。
// 只在 Apple 上编(src/core/CMakeLists.txt 的 if(APPLE) 块,同时 PUBLIC 定义 SCVB_HAS_POSIX_SHM,
// 让 ipc/PlatformSegmentBackend.h 把三个插件的后端别名到这里)。仅消息线程(非实时线程)调用;
// 音频线程只拿映射后的裸指针做原子读写,与 Win32 后端同形。
//
// ── 名字 ─────────────────────────────────────────────────────────────────────────────────
// 调用方传的仍是 Windows 形态的全名 L"Local\<逻辑名>"(9 个调用点与现有测试一行不改,冻结前缀
// `SynchainSCVB.v1.` 不变)。后端内部把它映射成 POSIX 名 "/<逻辑名>":
//   · 必须以 "Local\" 开头;逻辑名只允许 ASCII 的 [A-Za-z0-9._-],且不能是 "." / ".."
//     (逻辑名同时是锁文件名,所以不放 '/'、'\' 与非 ASCII);
//   · POSIX 名(含开头的 '/')最长 kMaxShmNameLength = 31 字符(xnu 的 PSHMNAMLEN;用户态头里拿不到
//     这个宏,只能写常量)。超长在碰文件系统之前就返回 kFailed,并以 ENAMETOOLONG 上报诊断。
//     最长的真实段名 "/SynchainSCVB.v1.g8.audio.ch15" 是 30 字符。
//
// ── 生命周期(用「持有锁」模拟 Windows 的内核引用计数)──────────────────────────────────────
// POSIX shm 不 unlink 就一直留到重启;Windows 的段在最后一个句柄关闭时自动消失。这里给每个段配一个
// 锁文件 <锁目录>/<逻辑名>.lock,**每个映射**持有它的一把 flock(LOCK_SH),直到 unmap:
//   · 锁目录 = 真实 home(getpwuid,不看可能被容器化的 HOME)下的
//     "Library/Application Support/Synchain/SCVB/ipc"。不放 Caches(会被系统清理),也**不放 TMPDIR**
//     (M01 实测:进程外的 AUHostingService 里 TMPDIR 是服务专属子目录,与宿主进程看到的不是同一处)。
//   · 测试钩子:环境变量 SCVB_IPC_LOCK_DIR 覆盖锁目录(构造后端时读一次)。它在发布版里同样生效,所以
//     **同一组段的所有进程必须看到同一个值** —— 宿主与进程外的 AUHostingService 若只有一边设了它,
//     两边的锁就不是同一把,后来的创建者会把对方的活段当残段清掉。为了让这种错配在日志里看得见,
//     用到覆盖的后端实例在第一次打开段时经 IpcDiag 上报一次 kLockDirOverride(带覆盖路径)。
//   · **所有生命周期转换**(建、附着、离开)都在全局 <锁目录>/lifecycle.lock 的 flock(LOCK_EX) 下做:
//     BSD flock 的升降级不是原子的(先放旧锁再上新锁),只有在这把全局锁下,「EX 探测 → 降为 SH」与
//     「离开时 EX 探测 → unlink」之间才不会插进别人的转换。
//   · 全局锁**有界等待**,等不到就放弃这一次(打开 → kFailed;离开 → 不做 unlink 判定,段留给下一个
//     创建者清理):createOrOpen 2 s(插件加载 / 改组,稀少);openExisting* 100 ms(25Hz 重试路径);
//     离开 200 ms。某个实例一旦等满超时,之后 1 s 内它的所有取锁都只试一次不等待(断路):锁被一个
//     挂住的进程拿着时,卸载时的 N 个映射、25Hz 的重试都不会把等待乘上去。断路窗口内的失败**不上报**,
//     只计数;计数并进下一条同类诊断的 suppressed 字段(窗口过期后的那次真实等待若仍超时,就报这一条),
//     所以锁一直挂着时每个实例每秒至多一条诊断。取锁成功即清零。
//   · createOrOpen:对段锁文件 flock(LOCK_EX|LOCK_NB)。
//       拿到 ⇒ 没有活着的持有者 ⇒ 先 shm_unlink 残段(崩溃留下的、旧尺寸的都在这里清掉;只清本用户
//              名下的),再 shm_open(O_CREAT|O_EXCL|O_RDWR, 0600) + ftruncate(size),created = true;
//       EWOULDBLOCK ⇒ 有活持有者 ⇒ 以 O_RDWR 附着,**绝不 ftruncate**(macOS 上同一个 shm 对象只能
//              ftruncate 一次;尺寸以创建者为准,经 fstat 回填)。
//     之后都降为 LOCK_SH,持有到 unmap。
//   · openExisting / openExistingReadOnly:只附着、绝不创建(ADR-001a)。没有活持有者的段等同于
//     Windows 上「已经不存在」,返回 kFailed(不碰它,留给下一个创建者清理);只读打开用
//     O_RDONLY + PROT_READ,同样持 LOCK_SH。段不存在 / 从没建过时安静地返回 kFailed(25Hz 重试路径,
//     不刷诊断)。
//   · 映射:fstat 回填真实尺寸(macOS 会把 ftruncate 的尺寸按页上取整,真实尺寸可能大于请求值,
//     对容量校验是安全超集),mmap 之后立刻关掉 shm 的 fd;锁 fd 与 POSIX 名放在后端私有的小结构里,
//     SegmentView::mapping 指向它。
//   · unmap:munmap,然后在全局锁下试 flock(LOCK_EX|LOCK_NB),拿到 ⇒ 自己是最后一个离开者 ⇒ shm_unlink;
//     最后关锁 fd。DAW 崩溃时内核会释放它持有的所有 flock,下一个 createOrOpen 就走清理分支。
//   · **视图没经 unmap 就被丢掉**(SegmentHandle 在宽限期届满前析构 —— 插件卸载时的常态):映射保留到
//     进程退出(与 Win32 后端此时泄漏 view / HANDLE 同口径,音频线程可能仍持有的裸指针不会悬空),
//     但锁的那一半在**后端析构**时补做:放 SH、最后离开者 unlink。所以三个 Processor 里后端成员必须
//     先于用它的会话声明(今天就是这样:后析构);一个视图也必须由映射它的那个后端实例 unmap
//     (SegmentHandle 记着后端指针,天然满足)。
//   · 必须用 flock,不能用 fcntl:flock 按「打开的文件描述」计,同一进程里两个实例各自 open 锁文件,
//     彼此照样互斥;fcntl 记录锁按进程计,同进程第二个实例会误以为没有持有者而 unlink 活段。
//
// ── 错误与诊断 ───────────────────────────────────────────────────────────────────────────
// EACCES / EPERM / ENAMETOOLONG 等一律返回 kFailed,并经 ipc/IpcDiag.h 上报(默认空操作,M07 接到
// mac 文件日志)。诊断**一律在放掉全局 lifecycle.lock 之后**才交给 sink:sink 做慢 I/O 也卡不住
// 别的进程的段操作。mlock 尽力而为:失败只上报诊断,不影响返回值。
//
// ── 已知限制 ─────────────────────────────────────────────────────────────────────────────
//   · 多用户:权限 0600。同机另一个用户同时开 DAW 会拿不到段(EACCES;POSIX shm 是全机命名空间,
//     段名没有余量加 uid),也不会清掉对方的段。
//   · App Sandbox 里运行插件的宿主(插件在宿主自己的沙盒进程内):沙盒要求 POSIX shm 名带 App Group
//     前缀(31 字符也放不下),锁目录也在容器之外,这个后端在那类宿主里会 kFailed(EPERM / EACCES)。
//     M01 的 G0 只覆盖了进程外的 AUHostingService(未沙盒、HOME 未容器化);沙盒内宿主的实测未做。
//   · DAW 运行期间手动删掉锁目录(~/Library/Application Support/Synchain/SCVB/ipc 或其上级):活着的
//     持有者仍拿着旧锁文件(已无名字)上的 SH,新来的创建者建出新锁文件、判段「没有持有者」并清掉活段,
//     两边各用一份(裂脑)。只能靠「运行中别删这个目录」规避。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "ISegmentBackend.h"
#include "SegmentLayout.h"

namespace scvb
{

// 全名包装 = L"Local\" + segmentLogicalName(...),与 SegmentBackendWin32.h 同名同义(前缀映射在后端内部做),
// 让走 PlatformSegmentBackend 的代码在两个平台上写法一致。
std::wstring segmentRegistryName(u32 group);
std::wstring segmentAudioName(u32 group, u32 channel); // channel ∈ [1, 15]
std::wstring segmentFeatName(u32 group, u32 channel); // channel ∈ [1, 15]
std::wstring segmentCtrlName(u32 group);

class SegmentBackendPosix final : public ISegmentBackend
{
public:
    // POSIX 段名(含开头的 '/')的最大长度:xnu 的 PSHMNAMLEN。
    static constexpr std::size_t kMaxShmNameLength = 31;
    // 测试用锁目录覆盖(构造时读一次;见头注「测试钩子」)。
    static constexpr const char* kLockDirEnvVar = "SCVB_IPC_LOCK_DIR";
    // 全局生命周期锁文件名(在锁目录下)。
    static constexpr const char* kLifecycleLockName = "lifecycle.lock";
    // 锁目录相对真实 home 的路径。
    static constexpr const char* kHomeRelativeLockDir = "Library/Application Support/Synchain/SCVB/ipc";

    // 全局锁的有界等待(见头注)。
    static constexpr std::chrono::milliseconds kCreateLockWait{2000};
    static constexpr std::chrono::milliseconds kAttachLockWait{100};
    static constexpr std::chrono::milliseconds kLeaveLockWait{200};
    // 一次等满超时之后,本实例在这段时间内只试一次、不等待。
    static constexpr std::chrono::milliseconds kLockBreakerWindow{1000};

    enum class NameStatus
    {
        kOk,
        kBadPrefix, // 不以 "Local\" 开头
        kBadChar, // 逻辑名为空、是 "." / "..",或含 [A-Za-z0-9._-] 以外的字符
        kTooLong, // POSIX 名超过 kMaxShmNameLength
    };

    SegmentBackendPosix();
    ~SegmentBackendPosix() override;
    SegmentBackendPosix(const SegmentBackendPosix&) = delete;
    SegmentBackendPosix& operator=(const SegmentBackendPosix&) = delete;

    InitResult createOrOpen(const std::wstring& name, std::size_t size, SegmentView& view) override;
    InitResult openExisting(const std::wstring& name, SegmentView& view) override;
    InitResult openExistingReadOnly(const std::wstring& name, SegmentView& view) override;
    void unmap(SegmentView& view) override;
    void tryLock(const SegmentView& view) override;

    // L"Local\<逻辑名>" → "/<逻辑名>"。失败时 posixName 仍填一个可打印的近似名(非允许字符换成 '?'),
    // 只供诊断用。
    static NameStatus toPosixName(const std::wstring& name, std::string& posixName);

    // 默认锁目录:<真实 home>/kHomeRelativeLockDir。home 解析不出来时返回空串。
    static std::string defaultLockDir();

    // 本实例使用的锁目录(构造时定下,之后不变;空 = 解析失败,所有打开都会 kFailed)。
    const std::string& lockDir() const noexcept { return lockDir_; }
    // 锁目录是否来自 SCVB_IPC_LOCK_DIR。
    bool lockDirOverridden() const noexcept { return lockDirOverridden_; }

private:
    struct Mapping;
    struct PendingDiag;
    enum class Mode
    {
        kCreateOrOpen,
        kOpenExisting,
        kOpenReadOnly,
    };

    InitResult openSegment(const std::wstring& name, std::size_t size, SegmentView& view, Mode mode);
    // 持全局锁的那一段;失败原因记进 diag,由调用方在放锁之后上报。
    InitResult openLocked(const std::string& shm, std::size_t size, SegmentView& view, Mode mode, PendingDiag& diag);
    void track(Mapping* m) noexcept;
    void untrack(Mapping* m) noexcept;
    // 「离开」这一半:全局锁下 EX 探测 → 最后离开者 shm_unlink → 关锁 fd。不 munmap。
    void leave(Mapping& m);
    // 断路:本实例最近一次「等满超时」之后 kLockBreakerWindow 内只试一次。
    std::chrono::milliseconds lockWait(std::chrono::milliseconds normal) const noexcept;
    // 取全局锁失败时的记账:断路窗口内的超时只计数不上报;其余记进 diag(带上此前略去的次数)。
    void noteLockFailure(int error, const std::string& lockPath, std::chrono::milliseconds wait,
                         PendingDiag& diag) noexcept;

    std::string lockDir_;
    bool lockDirOverridden_ = false;
    std::atomic<bool> overrideReported_{false};
    std::atomic<long long> lastLockTimeoutMs_{-1}; // steady_clock 毫秒;-1 = 从没超时过
    std::atomic<std::uint32_t> suppressedLockFailures_{0}; // 断路窗口内略去、尚未随诊断报出的失败次数
    std::mutex liveMutex_; // 保护 live_ 链表(多个线程各自用同一个后端实例时)
    Mapping* live_ = nullptr; // 本实例映射出去、尚未 unmap 的视图(侵入式双向链表,插入/摘除不分配)
};

} // namespace scvb
