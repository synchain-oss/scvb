// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// VizRateSink —— viz 实得速率汇总行的出口(B 线 M07b)。
//
// 【为什么单独一个出口】汇总行在 Output / Monitor 的 Processor 里产生(`VizRateMeter.h`),落盘的是
// plugin-common 的 PlatformLog。两个 Processor 的 .cpp 都会被编进不带 PlatformLog.cpp 的离线测试目标
// (OutputProcessor.cpp → scvb_host_tests;MonitorProcessor.cpp → scvb_monitor_tests),直接调
// `platformlog::write` 会让那两个目标链接不上。于是照 IpcDiag 的先例在 scvb_core 这一侧留一个槽:
// 默认空 = 什么都不做;`platformlog::installAt()` 装上日志时把槽接到日志,`uninstall()` 时摘掉。
// Windows 上生产路径从不 install(defaultLogFile 为空),槽一直是空的 = 不新增任何日志面。
//
// 【形态】header-only:槽是一个 inline 变量,每个链接它的二进制(每个插件、每个测试可执行文件)各一份,
// 与 PlatformLog 的全局状态同寿同域。这样 PlatformLog.cpp 只依赖本头,不需要链接 scvb_core
// (Windows 上的 scvb_plugin_common_tests 就不链)。
//
// 【线程】槽本身是原子指针,任何线程读写都安全。sink 的调用方今天只有消息线程上的定时器回调;
// sink 实现必须 noexcept(异常不许穿回定时器回调)。文案一律 ASCII。

#include <atomic>
#include <string>

namespace scvb::diag
{

// 收一行汇总(以 NUL 结尾的 ASCII,不含换行)。
using VizRateSink = void (*)(const char* line) noexcept;

namespace detail
{
inline std::atomic<VizRateSink> vizRateSinkSlot{nullptr};
static_assert(std::atomic<VizRateSink>::is_always_lock_free, "the viz-rate sink slot must be lock-free");
} // namespace detail

// 装 / 换 / 摘(nullptr = 回到空操作)。
inline void setVizRateSink(VizRateSink sink) noexcept
{
    detail::vizRateSinkSlot.store(sink, std::memory_order_release);
}

inline VizRateSink vizRateSink() noexcept
{
    return detail::vizRateSinkSlot.load(std::memory_order_acquire);
}

// 交一行给 sink;没装时什么都不做。
inline void reportVizRate(const std::string& line) noexcept
{
    if (const VizRateSink sink = vizRateSink(); sink != nullptr)
    {
        sink(line.c_str());
    }
}

} // namespace scvb::diag
