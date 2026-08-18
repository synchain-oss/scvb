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

std::uint8_t probeGroupsOnline(ISegmentBackend& backend, u32 ownGroup, u64 nowMs) noexcept
{
    std::uint8_t bits = 0;
    for (u32 g = 1; g <= kMaxGroups; ++g)
    {
        if (g == ownGroup)
        {
            continue; // 本组位由调用方从本组 registry 填
        }
        SegmentView view;
        if (backend.openExistingReadOnly(registryFullName(g), view) != InitResult::kOk)
        {
            continue; // 段不存在 → 该组离线(不创建,只读探测;契约 §2.4 FILE_MAP_READ 最小权限)
        }
        auto* header = static_cast<RegistryHeader*>(view.base);
        // 只读 attach(allowOverwrite=false):非阻塞单次 magic/abi 校验,magic 未就绪 → kFailed(离线)。
        if (backend.initHeader(view, &header->magic, &header->abi, nullptr, kInputSlotOffset,
                               /*initData=*/{}, /*allowOverwrite=*/false) != InitResult::kOk)
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
