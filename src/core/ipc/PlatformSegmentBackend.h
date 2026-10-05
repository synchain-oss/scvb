// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// PlatformSegmentBackend —— 「这个平台用哪一个共享内存段后端」的唯一接缝(B 线 M03)。
// 三个插件的 Processor(Input / Output / Monitor)只认这个别名,不再各自写死后端类型:
//
//   · _WIN32                 → SegmentBackendWin32(命名共享内存 Local\...,现行实现,行为逐字不变);
//   · 定义了 SCVB_HAS_POSIX_SHM → SegmentBackendPosix(shm_open + flock 生命周期锁,B 线 M05;宏由
//                               src/core/CMakeLists.txt 只在 Apple 上 PUBLIC 定义,宏未定义时本头**不引用**
//                               那个头文件,所以别的平台不需要它的 POSIX 依赖);
//   · 其余平台               → SegmentBackendInProcess(进程内模拟:同一宿主进程里的实例彼此可见,
//                               跨进程不可见 —— 只是让 core 在没有真后端的平台上也能编过,不是可发布形态)。
//
// 选择只发生在预处理期:别名之外不加任何间接层(不做运行期多态切换),音频线程的访问路径与改动前同形。

#if defined(_WIN32)
// windows.h 的 min/max 宏会污染 std::numeric_limits<T>::min() 与 std::max;SegmentBackendWin32.h
// 内部 include windows.h 但不定义 NOMINMAX,所以在这里、紧挨着它先禁掉(原先这一块分别写在三个
// Processor 头的最上面)。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "SegmentBackendWin32.h"
#elif defined(SCVB_HAS_POSIX_SHM)
#include "SegmentBackendPosix.h"
#else
#include "SegmentBackendInProcess.h"
#endif

namespace scvb
{
#if defined(_WIN32)
using PlatformSegmentBackend = SegmentBackendWin32;
#elif defined(SCVB_HAS_POSIX_SHM)
using PlatformSegmentBackend = SegmentBackendPosix;
#else
using PlatformSegmentBackend = SegmentBackendInProcess;
#endif
} // namespace scvb
