// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// v4 崩溃修复 —— 进程寿命泄漏池。
// 背景(实测 Cubase 15):宿主可在未完全挂起处理线程的情况下销毁/重建插件实例
// (Render-in-Place 撤销、导出结束、ASIO-Guard 切换)。实例析构若同步析构
// AudioRing/Registry 并 UnmapViewOfFile,在途 writeBlock/readBlock 即访问
// 已解映射地址 → 0xC0000005(周日两轮转储均锁定于写环内层拷贝循环)。
// spike 对策:环与 registry 对象交还本池永不析构;共享内存视图永不解除映射
// (SegmentBackendWin32::SegmentView::reset 同步改为泄漏)。占用 ≈ 环容量
// (1<<19 帧 × 4 字节 × 声道数 ≈ 2–4MB/环),单 DAW 会话可接受;进程退出由内核回收。
// T06 正式版改为引用计数 + 宿主握手后释放(不采用本泄漏策略)。

#include <memory>
#include <vector>

#include "AudioRing.h"
#include "Registry.h"

namespace scvb
{
inline std::vector<std::unique_ptr<AudioRing>>& retiredRings()
{
    static auto* pool = new std::vector<std::unique_ptr<AudioRing>>();
    return *pool;
}

inline std::vector<std::unique_ptr<Registry>>& retiredRegistries()
{
    static auto* pool = new std::vector<std::unique_ptr<Registry>>();
    return *pool;
}
} // namespace scvb