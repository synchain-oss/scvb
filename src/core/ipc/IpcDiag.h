// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// IpcDiag —— 共享内存段后端的诊断出口(B 线 M05)。
//
// 段后端在「打不开 / 建不出 / 锁不上」时只能向调用方回 InitResult::kFailed,失败的**原因**(哪一步、
// errno 是多少、哪个段)在接口上没有位置。这里给它一个旁路:后端在每个失败点调用 reportIpcDiag(),
// 宿主侧(M07:mac 文件日志)经 setIpcDiagSink() 接一个 sink 把它落下来。
//
// 约定:
//   · **默认空操作**:没有 sink 时 reportIpcDiag() 什么都不做,所以 scvb_core 的单测与没接日志的
//     构建行为不变。
//   · **只在非实时线程上调用**(不保证是消息线程:M07 实测段后端会在 prepareToPlay / releaseResources
//   里被调到,线程由宿主定):reportIpcDiag() 只出现在段后端的 createOrOpen / openExisting /
//     openExistingReadOnly / unmap / tryLock 里,这些按 ISegmentBackend 的约定只在持 lifecycleMutex 的
//     非实时线程调用(01 §3.1);音频线程从不经过这里。sink 在报告者的线程上**同步**调用。
//   · **不在跨进程锁里调用**:POSIX 后端先放掉全局 lifecycle.lock,再把诊断交给 sink,所以 sink 里做文件
//     I/O 卡不住别的进程的段操作;但它仍在调用方(非实时)线程上,应当尽快返回。sink 不得回调段后端。
//   · sink 是进程内(准确说是「每个链接了 scvb_core 的二进制内」)唯一的一个函数指针:三个插件是三个
//     二进制,各有一份,互不影响。设置与读取都是原子的;换 sink 时正在进行的那一次报告可能仍落到旧 sink。
//   · **同一事件可能高频重复,sink 要落盘就得自己限流或去重**:后端只给「全局 lifecycle.lock 超时」
//     限流(断路窗口内略去、计数并进下一条的 suppressed)。别的持续性失败 —— 段对附着者不可写、
//     lifecycle.lock 打不开之类的配置错误 —— 不会自己好,在 25Hz 重试路径上每次都会报一条同样的
//     (op, error, segment)。
//   · 事件里的字符串一律 ASCII,只在回调期间有效;sink 要留存就自己拷贝。
//   · 本诊断不进任何共享内存段、不改任何冻结契约,只是进程内的观测口。

#include <cstdint>

namespace scvb
{

// 失败发生在哪一步。数值只供进程内区分,不持久化、不跨进程。
enum class IpcDiagOp : std::uint32_t
{
    kName, // 段名不合法(缺 "Local\" 前缀 / 含非允许字符)或超出 PSHMNAMLEN
    kLockDir, // 锁目录解析或建立失败
    kLifecycleLock, // 全局 lifecycle.lock 打开或加锁失败(含有界等待超时)
    kSegmentLock, // 段锁文件打开或 flock 失败
    kShmOpen, // shm_open 失败(EACCES / EPERM / 有持有者却找不到段 等)
    kShmTruncate, // 创建者 ftruncate 失败
    kShmStat, // fstat 失败或拿到 0 尺寸
    kShmMap, // mmap 失败
    kShmUnlink, // 清残段或最后离开者 shm_unlink 失败
    kMlock, // 创建者锁页失败(尽力而为,不影响返回值)
    kLockDirOverride, // 不是失败:锁目录来自测试钩子 SCVB_IPC_LOCK_DIR(每个后端实例报一次,error = 0)
};

struct IpcDiagEvent
{
    IpcDiagOp op = IpcDiagOp::kName;
    int error = 0; // errno;0 = 不适用
    const char* segment = ""; // 平台段名(POSIX 为 "/X");从不为 nullptr,可能为空串
    const char* path = ""; // 相关的文件 / 目录路径(锁目录、锁文件);从不为 nullptr,可能为空串
    const char* detail = ""; // 静态 ASCII 描述;从不为 nullptr
    // 本条之前、同一后端实例在断路窗口内略去未报的同类失败次数(目前只用于 kLifecycleLock;0 = 没有)。
    std::uint32_t suppressed = 0;
};

// sink 必须 noexcept:报告点在后端的失败清理路径里,不允许异常穿出。
using IpcDiagSink = void (*)(const IpcDiagEvent& event) noexcept;

// 安装 sink(nullptr = 恢复默认空操作),返回之前的 sink。消息线程调用。
IpcDiagSink setIpcDiagSink(IpcDiagSink sink) noexcept;

// 当前 sink(nullptr = 空操作)。
IpcDiagSink ipcDiagSink() noexcept;

// 后端在失败点调用:有 sink 就同步转给它,没有就什么都不做。
void reportIpcDiag(const IpcDiagEvent& event) noexcept;

// op 的稳定 ASCII 名(日志用);未知值返回 "unknown"。
const char* ipcDiagOpName(IpcDiagOp op) noexcept;

} // namespace scvb
