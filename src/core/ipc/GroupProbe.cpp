// SPDX-License-Identifier: GPL-3.0-or-later
#include "GroupProbe.h"

#include <string>

#include "Registry.h"
#include "SegmentLayout.h"

namespace scvb
{

namespace
{
// 与 Registry.cpp 的 registryFullName 同构:v1 仅 Windows,OS 前缀固定 "Local\"。
std::wstring registryFullName(u32 group)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kRegistry);
}
} // namespace

std::uint8_t probeGroupsOnline(ISegmentBackend& backend, u32 ownGroup, u64 nowMs, bool includeOwnGroup) noexcept
{
    std::uint8_t bits = 0;
    for (u32 g = 1; g <= kMaxGroups; ++g)
    {
        if (g == ownGroup && !includeOwnGroup)
        {
            continue; // 本组位由调用方从本组 registry 填(回退探测路径除外)
        }
        SegmentView view;
        if (backend.openExistingReadOnly(registryFullName(g), view) != InitResult::kOk)
        {
            continue; // 段不存在 → 该组离线(不创建,只读探测;契约 §2.4 FILE_MAP_READ 最小权限)
        }
        // 尺寸校验(B 线 M03:由退役的 RegistryProbe 迁来,那是它比本函数多出的唯一一条判据):
        // 段必须至少容纳 RegistryHeader + 15 个 InputSlot + OutputSlot,才允许读头部与 OutputSlot。
        // 不足 = 旧版本 / 异常残段 → 该组离线。必须排在读 magic 之前 —— 不足 64 字节时连头部都在界外。
        // view.size 由各后端回填真实段大小(Win32 v6 GetFileSizeEx / VirtualQuery;InProcess = 缓冲长度)。
        if (view.base == nullptr || view.size < kOutputSlotOffset + sizeof(OutputSlot))
        {
            backend.unmap(view);
            continue;
        }
        const auto* header = static_cast<const RegistryHeader*>(view.base);
        // 只读 attach 校验(PR#54 R8):checkHeaderReadOnly 取 const 引用、严格只读(绝不写 view/头部),
        // 不经 initHeader(其 allowOverwrite=true 分支会写)—— FILE_MAP_READ 只读映射上任何写都会
        // ACCESS_VIOLATION。magic 未就绪 → kFailed(该组离线)。
        if (backend.checkHeaderReadOnly(view, header->magic, header->abi) != InitResult::kOk)
        {
            backend.unmap(view);
            continue;
        }
        auto* os = reinterpret_cast<OutputSlot*>(static_cast<char*>(view.base) + kOutputSlotOffset);
        if (os->state.load(std::memory_order_acquire) == kSlotActive &&
            !isStaleDisplay(os->heartbeat_ms.load(std::memory_order_acquire), nowMs))
        {
            bits = static_cast<std::uint8_t>(bits | (1u << (g - 1)));
        }
        backend.unmap(view);
    }
    return bits;
}

} // namespace scvb
