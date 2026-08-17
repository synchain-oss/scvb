// SPDX-License-Identifier: GPL-3.0-or-later
#include "SegmentBackendWin32.h"

namespace scvb
{

namespace
{
// 真实段大小:GetFileSizeEx 对 section 句柄返回段大小(精确);失败回退 VirtualQuery
// (64KB 对齐上界,对容量校验是安全超集)。v6 起 view.size 一律填真实值,不再填请求值
// ——v1..v5 根因:同名段已存在时 CreateFileMapping 返回旧段句柄,请求大小被忽略,
// 旧实现却把请求大小当实际大小发布几何 → 小视图 + 大几何 → 写环越界 0xC0000005。
std::size_t realSectionSize(HANDLE h, void* base)
{
    LARGE_INTEGER li{};
    if (::GetFileSizeEx(h, &li) != FALSE && li.QuadPart > 0)
    {
        return static_cast<std::size_t>(li.QuadPart);
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(base, &mbi, sizeof(mbi)) != 0 && mbi.RegionSize > 0)
    {
        return mbi.RegionSize;
    }
    return 0;
}

void logVirtualLockFailure(const std::wstring& name)
{
    // scvb_core 无日志框架(T07 Registry 生命周期落地);此处用 OutputDebugStringW(仅调试输出、
    // 非阻塞、无害),符合 01 §4.0「失败降级为仅预触碰并记日志」。
    const std::wstring msg = L"[SCVB] VirtualLock 失败(降级为仅预触碰): " + name;
    ::OutputDebugStringW(msg.c_str());
}
} // namespace

std::wstring segmentRegistryName(u32 group)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kRegistry);
}

std::wstring segmentAudioName(u32 group, u32 channel)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kAudio, channel);
}

std::wstring segmentFeatName(u32 group, u32 channel)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kFeat, channel);
}

std::wstring segmentCtrlName(u32 group)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kCtrl);
}

InitResult SegmentBackendWin32::createOrOpen(const std::wstring& name, std::size_t size, SegmentView& view)
{
    view.reset();
    if (size == 0)
    {
        return InitResult::kFailed;
    }
    const DWORD high = static_cast<DWORD>(static_cast<unsigned long long>(size) >> 32);
    const DWORD low = static_cast<DWORD>(static_cast<unsigned long long>(size) & 0xFFFFFFFFu);

    HANDLE h = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, high, low, name.c_str());
    if (h == nullptr)
    {
        return InitResult::kFailed;
    }
    const bool created = (::GetLastError() != ERROR_ALREADY_EXISTS);

    void* base = ::MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (base == nullptr)
    {
        ::CloseHandle(h);
        return InitResult::kFailed;
    }

    view.base = base;
    view.size = realSectionSize(h, base); // v6:真实段大小(同名段已存在时可能远小于请求值)
    view.mapping = h;
    view.created = created;

    tryVirtualLock(view); // 01 §4.0:可选锁定,失败降级不失败
    return InitResult::kOk;
}

InitResult SegmentBackendWin32::openExisting(const std::wstring& name, SegmentView& view)
{
    view.reset();
    HANDLE h = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    if (h == nullptr)
    {
        return InitResult::kFailed;
    }
    void* base = ::MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (base == nullptr)
    {
        ::CloseHandle(h);
        return InitResult::kFailed;
    }
    view.base = base;
    view.size = realSectionSize(h, base); // v6:真实段大小(供 AudioRing 容量校验)
    view.mapping = h;
    view.created = false;

    tryVirtualLock(view); // 01 §4.0:可选锁定,失败降级不失败
    return InitResult::kOk;
}

void SegmentBackendWin32::tryVirtualLock(const SegmentView& view)
{
    if (view.base == nullptr || view.size == 0)
    {
        return;
    }

    // 先提升工作集配额再锁定(01 §4.0:audio.chN 可选 VirtualLock)。
    HANDLE proc = ::GetCurrentProcess();
    SIZE_T minWs = 0;
    SIZE_T maxWs = 0;
    if (::GetProcessWorkingSetSize(proc, &minWs, &maxWs) != FALSE)
    {
        const SIZE_T want = maxWs + static_cast<SIZE_T>(view.size);
        ::SetProcessWorkingSetSize(proc, minWs, want);
    }

    if (::VirtualLock(view.base, view.size) == FALSE)
    {
        // 降级:仅预触碰(已在 initHeader 完成),记日志不失败(01 §4.0)。
        logVirtualLockFailure(L"<mapped segment>");
    }
}

} // namespace scvb
