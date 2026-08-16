// SPDX-License-Identifier: GPL-3.0-or-later
#include "SegmentBackendWin32.h"

#include <cstring>

#include "SegmentLayout.h"

namespace scvb
{

namespace
{
std::wstring prefix(u32 group)
{
    // Local\SynchainSCVB.v1.g{G}.
    return L"Local\\SynchainSCVB.v1.g" + std::to_wstring(group) + L".";
}

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
} // namespace

std::wstring segmentRegistryName(u32 group)
{
    return prefix(group) + L"registry";
}

std::wstring segmentAudioName(u32 group, u32 channel)
{
    return prefix(group) + L"audio.ch" + std::to_wstring(channel);
}

std::wstring segmentCtrlName(u32 group)
{
    return prefix(group) + L"ctrl";
}

SegmentView::SegmentView(SegmentView&& other) noexcept
    : base(other.base), size(other.size), mapping(other.mapping), created(other.created)
{
    other.base = nullptr;
    other.mapping = nullptr;
    other.size = 0;
    other.created = false;
}

SegmentView& SegmentView::operator=(SegmentView&& other) noexcept
{
    if (this != &other)
    {
        reset();
        base = other.base;
        size = other.size;
        mapping = other.mapping;
        created = other.created;
        other.base = nullptr;
        other.mapping = nullptr;
        other.size = 0;
        other.created = false;
    }
    return *this;
}

SegmentView::~SegmentView()
{
    reset();
}

void SegmentView::reset()
{
    // v4 崩溃修复:spike 阶段故意不 UnmapViewOfFile/CloseHandle(泄漏视图)。
    // 实测 Cubase 15 会在未完全挂起处理线程时销毁/重建插件实例;在途
    // writeBlock/readBlock 仍持有旧视图指针,解映射即 0xC0000005。
    // 视图引用计数由内核管理,进程退出统一回收;T06 正式版改为引用计数 + 握手释放。
    base = nullptr;
    mapping = nullptr;
    size = 0;
    created = false;
}

InitResult SegmentBackendWin32::map(const std::wstring& name, std::size_t size, SegmentView& view)
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
    return InitResult::kOk;
}

InitResult SegmentBackendWin32::initHeader(SegmentView& view, std::atomic<u32>* headerMagic,
                                           std::atomic<u32>* headerAbi, std::atomic<u32>* headerGen,
                                           std::size_t dataOffset, const std::function<void()>& initData)
{
    if (view.base == nullptr || headerMagic == nullptr || headerAbi == nullptr)
    {
        return InitResult::kFailed;
    }

    if (view.created)
    {
        // 内核已零初始化;magic 后行 = 初始化完成标志(01 §4.0)。几何字段先写、magic 最后发布。
        headerAbi->store(kScvbAbi, std::memory_order_relaxed);
        if (headerGen != nullptr)
        {
            headerGen->store(1, std::memory_order_relaxed);
        }
        if (initData)
        {
            initData();
        }
        headerMagic->store(kScvbMagic, std::memory_order_release);
        return InitResult::kOk;
    }

    // attach 方:自旋读 magic(50ms × 10 次)。
    for (int i = 0; i < 10; ++i)
    {
        if (headerMagic->load(std::memory_order_acquire) == kScvbMagic)
        {
            return (headerAbi->load(std::memory_order_acquire) == kScvbAbi) ? InitResult::kOk
                                                                            : InitResult::kAbiMismatch;
        }
        ::Sleep(50);
    }

    // 超时仍 magic==0 → 残段/创建者死于初始化:覆盖式重初始化(两个竞争者写相同内容,竞态良性)。
    if (dataOffset < view.size)
    {
        std::memset(static_cast<char*>(view.base) + dataOffset, 0, view.size - dataOffset);
    }
    headerAbi->store(kScvbAbi, std::memory_order_relaxed);
    if (headerGen != nullptr)
    {
        headerGen->fetch_add(1, std::memory_order_relaxed);
    }
    if (initData)
    {
        initData();
    }
    u32 expected = 0;
    if (!headerMagic->compare_exchange_strong(expected, kScvbMagic, std::memory_order_release,
                                              std::memory_order_relaxed))
    {
        // 他人已完成初始化 → 直接 attach(校验 abi)。
        return (headerAbi->load(std::memory_order_acquire) == kScvbAbi) ? InitResult::kOk : InitResult::kAbiMismatch;
    }
    return InitResult::kOk;
}

} // namespace scvb
