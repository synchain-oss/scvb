// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SegmentBackendWin32 —— Windows 命名共享内存段后端(CreateFileMappingW/MapViewOfFile)+
// 01 §4.0 初始化协议(在 ISegmentBackend 内共用)+ 全名包装(加 "Local\" 前缀)。
// 仅消息线程调用;音频线程只拿映射后的裸指针做原子读写。

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ISegmentBackend.h"
#include "SegmentLayout.h"

namespace scvb
{

// 全名包装 = L"Local\" + segmentLogicalName(...)(ipc v1.5 [J66]:全部段名带组号 g{G},G=1..8)。
std::wstring segmentRegistryName(u32 group);
std::wstring segmentAudioName(u32 group, u32 channel); // channel ∈ [1, 15]
std::wstring segmentFeatName(u32 group, u32 channel); // channel ∈ [1, 15]
std::wstring segmentCtrlName(u32 group);

class SegmentBackendWin32 final : public ISegmentBackend
{
public:
    // 创建或打开命名共享内存段,写入 view。返回 kOk / kFailed。
    // created=true 时内核已零初始化整段;created=false 时需经 initHeader 判定是否可用。
    // v6:同名段已存在时请求大小被忽略,必须经 GetFileSizeEx 回填真实段大小到 view.size。
    InitResult createOrOpen(const std::wstring& name, std::size_t size, SegmentView& view) override;

    // 仅打开已存在的命名段(OpenFileMappingW,不创建)——Output 侧 attach audio.chN 用。
    // 段不存在 → kFailed。映射整段;view.size 回填真实段大小(v6,供容量校验)。
    InitResult openExisting(const std::wstring& name, SegmentView& view) override;

    // 只读打开(契约 §2.4 异组探测/数据面 attach 最小权限:OpenFileMappingW(FILE_MAP_READ),
    // PR#54 复审建议2)。
    InitResult openExistingReadOnly(const std::wstring& name, SegmentView& view) override;

    // 解映射 + 关闭句柄(仅消息线程;与 createOrOpen 对称)。
    void unmap(SegmentView& view) override;

    // 创建者写触碰后锁定(01 §4.0 / DeepSeek 复审【重要】4):VirtualLock,先提升工作集配额;
    // 失败降级为仅预触碰并记日志,绝不失败。只读 attach 方不调用。
    void tryLock(const SegmentView& view) override;

private:
    // openExisting / openExistingReadOnly 共用路径(仅 access 权限不同)。
    InitResult openExistingWithAccess(const std::wstring& name, SegmentView& view, DWORD access);
};

} // namespace scvb
