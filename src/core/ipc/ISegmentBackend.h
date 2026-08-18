// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ISegmentBackend —— 共享内存段后端抽象接口(01 §9:逻辑段名不带 OS 前缀,由 backend 加前缀;
// Win 实现 = SegmentBackendWin32,Posix 实现 = SegmentBackendPosix 归 v2)。
// 只含「映射/打开/解映射/页锁定」四个平台差异点(纯虚)+ 平台无关的 01 §4.0 段初始化协议(非虚共用)。
//
// 释放协议(DeepSeek 复审【重要】1,替换 T06「故意泄漏」):映射视图经 SegmentHandle 引用计数共享;
// 音频线程按块持有租约 lease()(共享计数 leaseCount 原子增减,不触碰系统调用);解映射只在消息
// 线程且 leaseCount 归零后经 backend->unmap 执行。握手/宽限期策略(01 §4.0/§4.2 未钦定释放协议,
// 本卡自设计,见 SegmentHandle 注释)。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
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
    kFailed // 映射/打开失败(含只读方 magic 未就绪,由 [M] 25Hz 重试)
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

// 段映射视图(RAII,move-only)。public 结构名与成员保持 T06 不变;视图的所有权与解映射
// 现由 SegmentHandle 承载(见下),裸 SegmentView 必须提升为 SegmentHandle,否则退回
// 「进程退出统一回收」。
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

    // 仅作 move 语义清理(置空指针,不持 backend 故不在此解映射)。解映射由 SegmentHandle
    // 经 backend->unmap 在消息线程执行。原 S1 v4「故意泄漏」策略已被引用计数 + 握手释放取代。
    void reset();
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
    // 仅 move 语义清理(置空,不持 backend 故不在此解映射)。解映射由 SegmentHandle 经
    // backend->unmap 在消息线程执行;裸 SegmentView 未提升为 SegmentHandle 时退回「进程退出
    // 统一回收」(原 S1 v4 泄漏策略,现由引用计数 + 握手释放取代)。
    base = nullptr;
    mapping = nullptr;
    size = 0;
    created = false;
}

// 段后端抽象接口。所有段操作只发生在持 lifecycleMutex 的非实时线程([M],01 §3.1);
// 音频线程只拿映射后的裸指针(经 SegmentHandle::lease() 租约)做原子读写。
class ISegmentBackend
{
public:
    virtual ~ISegmentBackend() = default;

    // 创建或打开命名共享内存段,写入 view。返回 kOk / kFailed。
    // created=true 时内核已零初始化整段;created=false 时需经 initHeader 判定是否可用。
    virtual InitResult createOrOpen(const std::wstring& name, std::size_t size, SegmentView& view) = 0;

    // 仅打开已存在的命名段(不创建)——Output 侧 attach audio.chN 用。段不存在 → kFailed。
    virtual InitResult openExisting(const std::wstring& name, SegmentView& view) = 0;

    // 只读权限打开已存在段(契约 §2.4 异组探测/数据面 attach 的最小权限口径:Win32 = FILE_MAP_READ)。
    // 默认实现退回 openExisting(后端无权限区分,如 InProcess)。
    virtual InitResult openExistingReadOnly(const std::wstring& name, SegmentView& view)
    {
        return openExisting(name, view);
    }

    // 解映射 + 关闭平台句柄(仅消息线程,与 createOrOpen 对称;供 SegmentHandle::release 调用)。
    // 调用后 view 置空。InProcess 后端只清视图(缓冲由全局 map 持有,进程寿命)。
    virtual void unmap(SegmentView& view) = 0;

    // 创建者写触碰后锁定页(平台相关:Win32 = VirtualLock,失败降级为仅预触碰并记日志;
    // POSIX v2 = mlock)。只读 attach 方不调用(仅创建者/写者锁,DeepSeek 复审【重要】4)。
    virtual void tryLock(const SegmentView& view) { (void)view; }

    // 只读 attach 方的非破坏性 magic/abi 校验(openExistingReadOnly 用,PR#54 R8):
    // 只读触碰 + 非阻塞单次 acquire 读 magic/abi;绝不写 view/头部、绝不覆盖式重初始化。
    // headerMagic/headerAbi 取 const 引用,类型层面禁止写入 —— 消除「非 const 指针传入 initHeader
    // 落在 FILE_MAP_READ 只读映射上写崩溃」的潜在风险(initHeader 的 allowOverwrite=false 分支
    // 仅 readTouchPages + load,无 store/memcpy,核实见 initHeader 实现)。
    // magic 未就绪 → kFailed([M] 25Hz 重试);abi 不符 → kAbiMismatch。
    InitResult checkHeaderReadOnly(const SegmentView& view, const std::atomic<u32>& headerMagic,
                                   const std::atomic<u32>& headerAbi) const
    {
        if (view.base == nullptr)
        {
            return InitResult::kFailed;
        }
        detail::readTouchPages(view.base, view.size);
        if (headerMagic.load(std::memory_order_acquire) == kScvbMagic)
        {
            return (headerAbi.load(std::memory_order_acquire) == kScvbAbi) ? InitResult::kOk : InitResult::kAbiMismatch;
        }
        return InitResult::kFailed;
    }

    // 01 §4.0 初始化协议(registry/audio/ctrl 通用;非虚共用)。
    // header 前两字段为 magic/abi,可选 generation;dataOffset 为「清 slot 区」的起始偏移
    // (覆盖式重初始化时从该偏移起清零);initData 为「magic 发布前」必须落盘的几何字段写回。
    //
    // allowOverwrite(默认 true = 创建者/owner 角色)决定「覆盖式重初始化分支」是否可用:
    //   created=true :写触碰全段 → tryLock(创建者锁)→ 写 abi=1、generation=1(若有)、initData()
    //                 → release-store magic;
    //   created=false + allowOverwrite=true(owner attach):只读触碰(严禁写)→ 自旋读 magic
    //                 (50ms × 10)。magic ok 且 abi ok → kOk;abi 不符 → kAbiMismatch;超时仍
    //                 magic==0 → 覆盖式重初始化(清 dataOffset 起、generation.fetch_add(1)、
    //                 initData()、CAS magic 0→'SCVB';失败即他人已完成,直接 attach)。
    //   created=false + allowOverwrite=false(只读/attach 方):只读触碰 → 非阻塞单次 magic/abi
    //                 校验(不 Sleep 自旋,[M] 25Hz 重试)。magic 未就绪 → kFailed;绝不 memset、
    //                 绝不重写 ——「覆盖分支仅限创建者角色」(DeepSeek 复审【重要】3)。
    InitResult initHeader(SegmentView& view, std::atomic<u32>* headerMagic, std::atomic<u32>* headerAbi,
                          std::atomic<u32>* headerGen, std::size_t dataOffset,
                          const std::function<void()>& initData = {}, bool allowOverwrite = true)
    {
        if (view.base == nullptr || headerMagic == nullptr || headerAbi == nullptr)
        {
            return InitResult::kFailed;
        }

        if (view.created)
        {
            // 创建者:magic 后行 = 初始化完成标志(01 §4.0)。写触碰 → 锁 → 几何字段 → magic 最后发布。
            detail::writeTouchPages(view.base, view.size);
            tryLock(view); // 创建者:写触碰后再锁(DeepSeek 复审【重要】4)
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

        // attach 方:先只读触碰(严禁写)。
        detail::readTouchPages(view.base, view.size);

        if (!allowOverwrite)
        {
            // 只读/attach 方:非阻塞单次 magic/abi 校验,不 Sleep 自旋;magic 未就绪(创建者仍在
            // 初始化)→ kFailed,由 [M] 25Hz 重试。覆盖式重初始化分支仅限创建者角色。
            if (headerMagic->load(std::memory_order_acquire) == kScvbMagic)
            {
                return (headerAbi->load(std::memory_order_acquire) == kScvbAbi) ? InitResult::kOk
                                                                                : InitResult::kAbiMismatch;
            }
            return InitResult::kFailed;
        }

        // owner/创建者角色的 attach:自旋读 magic(50ms × 10),等待其他实例完成初始化。
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

namespace detail
{
// 段映射控制块:实际持有 SegmentView;经 std::shared_ptr 在消息线程间共享(引用计数),
// 音频线程按块经 SegmentHandle::lease() 增减 leaseCount(原子,无系统调用)。
struct SegmentMapping
{
    SegmentView view; // base/size/mapping/created
    std::atomic<u32> leaseCount{0}; // 音频线程在途租约
    std::atomic<bool> releaseRequested{false}; // 消息线程已请求释放

    explicit SegmentMapping(SegmentView&& v) : view(std::move(v)) {}
};
} // namespace detail

// 稳态时钟毫秒(跨进程单调,01 §3 心跳/看门狗真源;定义在 Registry.cpp)。Windows = GetTickCount64;
// 非 Windows 回退进程内 steady_clock(v1 仅 Windows)。
u64 steadyNowMs() noexcept;

// 引用计数句柄(替换 T06「故意泄漏」策略;经三轮复审定型):
// - 消息线程【独占】std::shared_ptr<detail::SegmentMapping> 保活(音频线程永不触碰 shared_ptr 成员本身,
//   消除 shared_ptr 数据竞争 / UAF)。
// - 音频线程经 std::atomic<detail::SegmentMapping*> implPtr_ 读裸指针并增减 leaseCount(原子,无系统调用)。
// - 解映射只在消息线程,且 leaseCount==0 且宽限期届满后执行。
// 握手/宽限期策略(01 §4.0/§4.2 未钦定释放协议,本卡自设计):
//   release(nowMs):先置 releaseRequested → 摘 implPtr_(store nullptr,阻止新租约)→ unmapIfIdle()。
//   unmapIfIdle:leaseCount==0 时记录宽限期起始,宽限期(≥2 个 [M] 4Hz 心跳 = 500ms,>宿主最大块时长)
//   届满才 backend->unmap + impl_.reset——audio 已读到裸指针的在途块必在宽限期内完成并归还 lease。
//   T23/T24 接入时 release() 调用点 = releaseResources/宿主挂起(音频已停),宽限期是兜底双保险;
//   析构若早于宽限期届满,退回「进程退出统一回收」(罕见边界,进程正在卸载)。
class SegmentHandle
{
public:
    // 宽限期(兜底双保险):500ms = 2 个 4Hz 心跳,>宿主最大块时长。
    static constexpr u64 kReleaseGraceMs = 500;

    SegmentHandle() = default;

    // 接管 SegmentView 所有权(createOrOpen/openExisting + initHeader 之后调用;仅消息线程)。
    SegmentHandle(SegmentView&& view, ISegmentBackend* backend)
        : impl_(std::make_shared<detail::SegmentMapping>(std::move(view))), backend_(backend)
    {
        implPtr_.store(impl_.get(), std::memory_order_release);
    }

    ~SegmentHandle() { release(steadyNowMs()); }

    SegmentHandle(const SegmentHandle&) = delete;
    SegmentHandle& operator=(const SegmentHandle&) = delete;

    // 移动:纯所有权转移,不触发释放。约定:调用方须先 release()/移入 pendingReleases_ 再移动,
    // 否则旧 impl_ 被覆盖即泄漏(Registry/CtrlPlane 均遵守此约定)。
    SegmentHandle(SegmentHandle&& o) noexcept
        : impl_(std::move(o.impl_)), backend_(o.backend_), graceStartMs_(o.graceStartMs_),
          graceStarted_(o.graceStarted_)
    {
        implPtr_.store(impl_.get(), std::memory_order_release);
        o.implPtr_.store(nullptr, std::memory_order_release);
        o.backend_ = nullptr;
        o.graceStartMs_ = 0;
        o.graceStarted_ = false;
    }
    SegmentHandle& operator=(SegmentHandle&& o) noexcept
    {
        if (this != &o)
        {
            impl_ = std::move(o.impl_);
            backend_ = o.backend_;
            graceStartMs_ = o.graceStartMs_;
            graceStarted_ = o.graceStarted_;
            implPtr_.store(impl_.get(), std::memory_order_release);
            o.implPtr_.store(nullptr, std::memory_order_release);
            o.backend_ = nullptr;
            o.graceStartMs_ = 0;
            o.graceStarted_ = false;
        }
        return *this;
    }

    // valid()/base()/size() 只经 implPtr_(atomic 裸指针)读,不触碰 shared_ptr 成员;但【不持租约时
    // 仅消息线程使用】——消息线程是唯一解映射者,不与自己并发。音频线程一律经 Lease::base() 访问
    // (leaseCount≠0 阻止解映射,保证裸指针有效)。
    bool valid() const { return implPtr_.load(std::memory_order_acquire) != nullptr; }
    void* base() const
    {
        auto* p = implPtr_.load(std::memory_order_acquire);
        return p != nullptr ? p->view.base : nullptr;
    }
    std::size_t size() const
    {
        auto* p = implPtr_.load(std::memory_order_acquire);
        return p != nullptr ? p->view.size : 0;
    }

    // 音频线程按块租约:持有期内保证不解映射;leaseCount 原子增减,不触碰系统调用。
    // 块首 lease(),基址经 Lease::base() 访问,块末析构归还。release() 摘指针后不再发放新租约。
    class Lease
    {
    public:
        Lease() = default;
        ~Lease()
        {
            if (p_ != nullptr)
            {
                p_->leaseCount.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
        Lease& operator=(Lease&& o) noexcept
        {
            if (this != &o)
            {
                if (p_ != nullptr)
                {
                    p_->leaseCount.fetch_sub(1, std::memory_order_acq_rel);
                }
                p_ = o.p_;
                o.p_ = nullptr;
            }
            return *this;
        }
        explicit operator bool() const { return p_ != nullptr; }
        void* base() const { return p_ != nullptr ? p_->view.base : nullptr; }

    private:
        friend class SegmentHandle;
        // 构造不增减 leaseCount;租约计数由 SegmentHandle::lease() 在构造前递增。仅持裸指针。
        explicit Lease(detail::SegmentMapping* p) : p_(p) {}
        detail::SegmentMapping* p_ = nullptr;
    };

    // 音频线程按块调用。先 p=implPtr_.load(acquire) 读裸指针(不触碰 shared_ptr);再原子增 leaseCount;
    // 再查 releaseRequested,已置则退还并返回空。与 release() 的「先置请求再摘指针」构成握手。
    Lease lease() const
    {
        detail::SegmentMapping* p = implPtr_.load(std::memory_order_acquire);
        if (p == nullptr)
        {
            return Lease{};
        }
        p->leaseCount.fetch_add(1, std::memory_order_acq_rel);
        if (p->releaseRequested.load(std::memory_order_acquire))
        {
            p->leaseCount.fetch_sub(1, std::memory_order_acq_rel); // 释放已请求,退还租约
            return Lease{};
        }
        return Lease(p);
    }

    // 消息线程:请求释放。返回 true=已解映射(租约归零且宽限期届满),false=仍在释放流程中
    // (租约在途 / 宽限期未满),[M] 心跳周期再调本函数回收。幂等。
    bool release(u64 nowMs)
    {
        if (!impl_)
        {
            return true;
        }
        impl_->releaseRequested.store(true, std::memory_order_release); // 先置释放请求
        implPtr_.store(nullptr, std::memory_order_release); // 再摘指针(阻止新租约)
        return unmapIfIdle(nowMs);
    }

private:
    bool unmapIfIdle(u64 nowMs)
    {
        if (!impl_)
        {
            return true;
        }
        if (impl_->leaseCount.load(std::memory_order_acquire) != 0)
        {
            return false; // 租约在途
        }
        // leaseCount==0:记录宽限期起始(兜底双保险),宽限期届满后才解映射。
        if (!graceStarted_)
        {
            graceStarted_ = true;
            graceStartMs_ = nowMs;
            return false;
        }
        if (nowMs - graceStartMs_ < kReleaseGraceMs)
        {
            return false; // 宽限期未满
        }
        // 宽限期届满:解映射 + 释放保活 shared_ptr(仅消息线程)。
        if (backend_)
        {
            backend_->unmap(impl_->view);
        }
        impl_.reset();
        backend_ = nullptr;
        graceStarted_ = false;
        graceStartMs_ = 0;
        return true;
    }

    std::shared_ptr<detail::SegmentMapping> impl_; // 消息线程独占(保活);音频线程永不触碰
    std::atomic<detail::SegmentMapping*> implPtr_{nullptr}; // 音频线程经此读裸指针
    ISegmentBackend* backend_ = nullptr;
    u64 graceStartMs_ = 0; // 宽限期起始(消息线程)
    bool graceStarted_ = false; // 宽限期是否已开始(消息线程)
};

} // namespace scvb
