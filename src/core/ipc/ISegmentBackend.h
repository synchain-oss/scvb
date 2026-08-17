// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ISegmentBackend —— 共享内存段后端抽象接口(01 §9:逻辑段名不带 OS 前缀,由 backend 加前缀;
// Win 实现 = SegmentBackendWin32,Posix 实现 = SegmentBackendPosix 归 v2)。
// 只含「映射/打开」两个平台差异点(纯虚)+ 平台无关的 01 §4.0 段初始化协议(非虚共用)。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include "SegmentLayout.h"

namespace scvb
{

// 初始化结果。
enum class InitResult
{
    kOk, // 创建成功或 attach 成功
    kAbiMismatch, // magic 相符但 abi 不符(J40:拒连不崩溃)
    kFailed // 映射/打开失败
};

namespace detail
{
// 逐页写触碰(创建者分支):每页写一字节,强制页提交,消除 [A] 首触软缺页。
// 此刻段内无有效数据(内核已零初始化),写安全(01 §4.0)。
inline void writeTouchPages(void* base, std::size_t size) noexcept
{
    constexpr std::size_t kPageSize = 4096;
    auto* p = static_cast<volatile unsigned char*>(base);
    for (std::size_t off = 0; off < size; off += kPageSize)
    {
        p[off] = 0;
    }
}

// 逐页只读触碰(attach 方/重连路径):每页 volatile 读一字节,严禁任何写 —— 对已被 Input
// 活跃写入的 audio.chN,写触碰会清掉环内最多 ≈8s 音频;「写读一字节」则与 [A] 并发写构成
// 非原子 RMW 丢写。共享内存页在创建时已 commit,attach 方只读触碰即足(01 §4.0)。
inline void readTouchPages(const void* base, std::size_t size) noexcept
{
    constexpr std::size_t kPageSize = 4096;
    const auto* p = static_cast<const volatile unsigned char*>(base);
    for (std::size_t off = 0; off < size; off += kPageSize)
    {
        (void)p[off];
    }
}
} // namespace detail

// 段映射视图(RAII,move-only)。
struct SegmentView
{
    void* base = nullptr; // 映射后的字节指针
    std::size_t size = 0; // 真实段大小(字节;v6:同名段已存在时请求大小被忽略,必须回填真实值)
    void* mapping = nullptr; // 平台映射句柄(Win32: HANDLE;Posix: fd),不透明
    bool created = false; // 本进程是否创建者(ERROR_ALREADY_EXISTS 判定)

    SegmentView() = default;
    SegmentView(SegmentView&& other) noexcept;
    SegmentView& operator=(SegmentView&& other) noexcept;
    SegmentView(const SegmentView&) = delete;
    SegmentView& operator=(const SegmentView&) = delete;
    ~SegmentView();

    // S1 v4 策略:故意不解映射(泄漏视图)。实测 Cubase 15 会在未完全挂起处理线程时
    // 销毁/重建插件实例,在途 writeBlock/readBlock 仍持有旧视图指针,解映射即 0xC0000005。
    // 视图引用计数由内核管理,进程退出统一回收;正式版引用计数 + 握手释放随 T07 Registry
    // 生命周期落地。
    void reset();
};

// 段后端抽象接口。所有段操作只发生在持 lifecycleMutex 的非实时线程([M],01 §3.1);
// 音频线程只拿映射后的裸指针做原子读写。
class ISegmentBackend
{
public:
    virtual ~ISegmentBackend() = default;

    // 创建或打开命名共享内存段,写入 view。返回 kOk / kFailed。
    // created=true 时内核已零初始化整段;created=false 时需经 initHeader 判定是否可用。
    virtual InitResult createOrOpen(const std::wstring& name, std::size_t size, SegmentView& view) = 0;

    // 仅打开已存在的命名段(不创建)——Output 侧 attach audio.chN 用。段不存在 → kFailed。
    virtual InitResult openExisting(const std::wstring& name, SegmentView& view) = 0;

    // 01 §4.0 初始化协议(registry/audio/ctrl 通用;非虚共用)。
    // header 前两字段为 magic/abi,可选 generation;dataOffset 为「清 slot 区」的起始偏移
    // (覆盖式重初始化时从该偏移起清零);initData 为「magic 发布前」必须落盘的几何字段写回。
    //
    //   created=true :写触碰全段 → 写 abi=1、generation=1(若有)、initData() → release-store magic;
    //   created=false:只读触碰(严禁写)→ 自旋读 magic(50ms × 10)。magic ok 且 abi ok → kOk;
    //                 abi 不符 → kAbiMismatch;超时仍 magic==0 → 覆盖式重初始化(清 dataOffset 起、
    //                 generation.fetch_add(1)、initData()、CAS magic 0→'SCVB';失败即他人已完成,直接 attach)。
    InitResult initHeader(SegmentView& view, std::atomic<u32>* headerMagic, std::atomic<u32>* headerAbi,
                          std::atomic<u32>* headerGen, std::size_t dataOffset,
                          const std::function<void()>& initData = {})
    {
        if (view.base == nullptr || headerMagic == nullptr || headerAbi == nullptr)
        {
            return InitResult::kFailed;
        }

        if (view.created)
        {
            // 创建者:magic 后行 = 初始化完成标志(01 §4.0)。写触碰全段 → 几何字段先写 → magic 最后发布。
            detail::writeTouchPages(view.base, view.size);
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

        // attach 方:先只读触碰(严禁写),再自旋读 magic。
        detail::readTouchPages(view.base, view.size);
        for (int i = 0; i < 10; ++i)
        {
            if (headerMagic->load(std::memory_order_acquire) == kScvbMagic)
            {
                return (headerAbi->load(std::memory_order_acquire) == kScvbAbi) ? InitResult::kOk
                                                                                : InitResult::kAbiMismatch;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
            return (headerAbi->load(std::memory_order_acquire) == kScvbAbi) ? InitResult::kOk
                                                                            : InitResult::kAbiMismatch;
        }
        return InitResult::kOk;
    }
};

inline SegmentView::SegmentView(SegmentView&& other) noexcept
    : base(other.base), size(other.size), mapping(other.mapping), created(other.created)
{
    other.base = nullptr;
    other.mapping = nullptr;
    other.size = 0;
    other.created = false;
}

inline SegmentView& SegmentView::operator=(SegmentView&& other) noexcept
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

inline SegmentView::~SegmentView()
{
    reset();
}

inline void SegmentView::reset()
{
    // v4 崩溃修复:故意不 UnmapViewOfFile/CloseHandle(泄漏视图)。视图引用计数由内核管理,
    // 进程退出统一回收;正式版改为引用计数 + 握手释放,随 T07 Registry 生命周期落地。
    base = nullptr;
    mapping = nullptr;
    size = 0;
    created = false;
}

} // namespace scvb
