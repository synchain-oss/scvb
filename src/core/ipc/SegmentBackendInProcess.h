// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SegmentBackendInProcess —— 进程内共享内存模拟后端(测试用,可移植,无 WIN32 守卫)。
// 命名 → 共享字节缓冲(64 字节对齐),供测试模拟多实例:多个 Registry/CtrlPlane 实例经同一
// 名字共享同一缓冲,语义对齐 Win32 v6(同名已存在时请求大小被忽略、回填真实段大小)。
// T07 快速回归用;跨进程真实行为归 T07b。T20 亦复用。

#include <string>

#include "ISegmentBackend.h"

namespace scvb
{

class SegmentBackendInProcess final : public ISegmentBackend
{
public:
    // 创建或打开模拟段;同名已存在 → 复用既有缓冲(created=false,忽略请求大小)。
    InitResult createOrOpen(const std::wstring& name, std::size_t size, SegmentView& view) override;

    // 仅打开已存在段;不存在 → kFailed。
    InitResult openExisting(const std::wstring& name, SegmentView& view) override;

    // 解映射:只清视图指针,缓冲由全局 map 持有(进程寿命,其它实例仍可能引用)。
    void unmap(SegmentView& view) override;

    // 清空所有模拟段(测试隔离:每个 TEST_CASE 开始时调用,避免跨用例共享残留)。
    static void resetAll();
};

} // namespace scvb
