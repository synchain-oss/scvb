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
//     测试可用环境变量 SCVB_IPC_LOCK_DIR 覆盖(在构造后端之前设)。
//   · **所有生命周期转换**(建、附着、离开)都在全局 <锁目录>/lifecycle.lock 的 flock(LOCK_EX) 下做:
//     BSD flock 的升降级不是原子的(先放旧锁再上新锁),只有在这把全局锁下,「EX 探测 → 降为 SH」与
//     「离开时 EX 探测 → unlink」之间才不会插进别人的转换。全局锁有界等待(2 s),等不到就 kFailed。
//   · createOrOpen:对段锁文件 flock(LOCK_EX|LOCK_NB)。
//       拿到 ⇒ 没有活着的持有者 ⇒ 先 shm_unlink 残段(崩溃留下的、旧尺寸的都在这里清掉),
//              再 shm_open(O_CREAT|O_EXCL|O_RDWR, 0600) + ftruncate(size),created = true;
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
// ── 错误 ─────────────────────────────────────────────────────────────────────────────────
// EACCES / EPERM / ENAMETOOLONG 等一律返回 kFailed,并经 ipc/IpcDiag.h 上报(默认空操作,M07 接到
// mac 文件日志)。权限 0600:同机另一个用户同时开 DAW 会拿到 EACCES(POSIX shm 是全机命名空间,段名
// 没有余量加 uid),列为已知限制。mlock 尽力而为:失败只上报诊断,不影响返回值。

#include <cstddef>
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
    // 测试用锁目录覆盖(构造时读一次)。
    static constexpr const char* kLockDirEnvVar = "SCVB_IPC_LOCK_DIR";
    // 全局生命周期锁文件名(在锁目录下)。
    static constexpr const char* kLifecycleLockName = "lifecycle.lock";
    // 锁目录相对真实 home 的路径。
    static constexpr const char* kHomeRelativeLockDir = "Library/Application Support/Synchain/SCVB/ipc";

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

private:
    struct Mapping;
    enum class Mode
    {
        kCreateOrOpen,
        kOpenExisting,
        kOpenReadOnly,
    };

    InitResult openSegment(const std::wstring& name, std::size_t size, SegmentView& view, Mode mode);
    void track(Mapping* m) noexcept;
    void untrack(Mapping* m) noexcept;
    // 「离开」这一半:全局锁下 EX 探测 → 最后离开者 shm_unlink → 关锁 fd。不 munmap。
    static void leave(Mapping& m);

    std::string lockDir_;
    std::mutex liveMutex_; // 保护 live_ 链表(多个线程各自用同一个后端实例时)
    Mapping* live_ = nullptr; // 本实例映射出去、尚未 unmap 的视图(侵入式双向链表,插入/摘除不分配)
};

} // namespace scvb
