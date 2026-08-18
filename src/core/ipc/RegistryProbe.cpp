// SPDX-License-Identifier: GPL-3.0-or-later
// windows.h 的 min/max 宏污染:先禁再包含(与 OutputProcessor.h 同款;SegmentBackendWin32.h 内部 include windows.h)。
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RegistryProbe.h"

#include "Registry.h"
#include "SegmentBackendWin32.h"

namespace scvb
{

bool probeRegistryGroupOnline(u32 group, u64 nowMs)
{
    if (group < 1 || group > kMaxGroups)
        return false;

    // 契约 §2.4:异组只读探测 —— OpenFileMappingW(FILE_MAP_READ),只映射 RegistryHeader + OutputSlot。
    const HANDLE h = ::OpenFileMappingW(FILE_MAP_READ, FALSE, segmentRegistryName(group).c_str());
    if (h == nullptr)
        return false; // 段不存在 → 探测失败(可接受降级)

    void* base = ::MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    if (base == nullptr)
    {
        ::CloseHandle(h);
        return false;
    }

    // 尺寸校验必须用 VirtualQuery 查映射视图(MEMORY_BASIC_INFORMATION.RegionSize)——
    // GetFileSizeEx 只接受文件句柄,对 file-mapping 句柄恒失败(PR#55 缺陷1)。
    MEMORY_BASIC_INFORMATION mbi{};
    const bool sizeOk =
        ::VirtualQuery(base, &mbi, sizeof(mbi)) != 0 && mbi.RegionSize >= kOutputSlotOffset + sizeof(OutputSlot);

    bool online = false;
    if (sizeOk)
    {
        const auto* hdr = static_cast<const RegistryHeader*>(base);
        if (hdr->magic.load(std::memory_order_acquire) == kScvbMagic &&
            hdr->abi.load(std::memory_order_acquire) == kScvbAbi)
        {
            const auto* slot = reinterpret_cast<const OutputSlot*>(static_cast<const char*>(base) + kOutputSlotOffset);
            online = slot->state.load(std::memory_order_acquire) == kSlotActive &&
                     !isStaleDisplay(slot->heartbeat_ms.load(std::memory_order_acquire), nowMs);
        }
    }

    ::UnmapViewOfFile(base);
    ::CloseHandle(h);
    return online;
}

} // namespace scvb
