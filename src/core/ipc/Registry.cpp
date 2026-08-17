// SPDX-License-Identifier: GPL-3.0-or-later
#include "Registry.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <chrono>
#endif

namespace scvb
{

namespace
{
// v1 仅 Windows,OS 前缀固定 "Local\"(01 §9:逻辑段名不带 OS 前缀,前缀由 backend 侧全名函数加;
// Win32 见 SegmentBackendWin32::segmentRegistryName;此处为 Registry 的可移植等价实现,InProcess
// 后端用同一字符串作键)。POSIX 前缀归 v2。
std::wstring registryFullName(u32 group)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kRegistry);
}
} // namespace

u64 steadyNowMs() noexcept
{
#ifdef _WIN32
    return static_cast<u64>(::GetTickCount64());
#else
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}

bool isProcessAlive(u32 pid) noexcept
{
    if (pid == 0)
    {
        return false;
    }
#ifdef _WIN32
    const HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr)
    {
        return false;
    }
    DWORD code = 0;
    const bool ok = ::GetExitCodeProcess(h, &code) != 0;
    ::CloseHandle(h);
    return ok && code == STILL_ACTIVE;
#else
    // v1 仅 Windows;非 Windows 保守判「活」(绝不误接管,保护 SPSC)。v2 POSIX 用 kill(pid, 0)。
    (void)pid;
    return true;
#endif
}

Registry::Registry(ISegmentBackend& backend, u32 group, ProcessProbe probe)
    : backend_(backend), group_(group), probe_(std::move(probe))
{
}

Registry::~Registry()
{
    releaseOwnedSlot();
    header_ = nullptr;
    handle_.release(); // 握手释放(消息线程;leaseCount 归零后经 backend->unmap 解映射)
    // pendingReleases_ 的 SegmentHandle 元素随 vector 析构各自 release(进程退出兜底)。
}

void Registry::reapPendingReleases()
{
    // [M] 心跳/轮询周期调用:逐项 release(),成功(租约归零)即移除并解映射。
    for (auto it = pendingReleases_.begin(); it != pendingReleases_.end();)
    {
        if (it->release())
        {
            it = pendingReleases_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

Registry::ClaimResult Registry::open()
{
    header_ = nullptr;
    if (!handle_.release())
    {
        // 租约在途:旧句柄未解映射,压入待回收列表,由 [M] reapPendingReleases() 回收(防泄漏)。
        pendingReleases_.push_back(std::move(handle_));
    }
    SegmentView view;
    const auto r = backend_.createOrOpen(registryFullName(group_), kRegistrySegmentSize, view);
    if (r != InitResult::kOk)
    {
        return ClaimResult::kUnavailable;
    }
    auto* header = static_cast<RegistryHeader*>(view.base);
    // owner 角色(createOrOpen):allowOverwrite=true(覆盖式重初始化分支仅限创建者)。
    const auto ir = backend_.initHeader(view, &header->magic, &header->abi, &header->generation, kInputSlotOffset,
                                        /*initData=*/{}, /*allowOverwrite=*/true);
    if (ir == InitResult::kAbiMismatch)
    {
        const u32 remoteAbi = header->abi.load(std::memory_order_acquire);
        backend_.unmap(view); // 释放刚映射的视图
        header_ = nullptr;
        if (abiMismatchHandler_)
        {
            abiMismatchHandler_(kScvbAbi, remoteAbi);
        }
        return ClaimResult::kAbiMismatch;
    }
    if (ir != InitResult::kOk)
    {
        backend_.unmap(view);
        return ClaimResult::kUnavailable;
    }
    handle_ = SegmentHandle(std::move(view), &backend_);
    header_ = header;
    return ClaimResult::kClaimed;
}

u32 Registry::generation() const
{
    return header_ ? header_->generation.load(std::memory_order_acquire) : 0;
}

u32 Registry::abi() const
{
    return header_ ? header_->abi.load(std::memory_order_acquire) : 0;
}

InputSlot* Registry::inputSlot(u32 channel) const
{
    if (header_ == nullptr || channel == 0 || channel > kMaxChannels)
    {
        return nullptr;
    }
    auto* base = static_cast<char*>(handle_.base());
    return reinterpret_cast<InputSlot*>(base + kInputSlotOffset + (channel - 1) * sizeof(InputSlot));
}

OutputSlot* Registry::outputSlot() const
{
    if (header_ == nullptr)
    {
        return nullptr;
    }
    auto* base = static_cast<char*>(handle_.base());
    return reinterpret_cast<OutputSlot*>(base + kOutputSlotOffset);
}

void Registry::fillInputSlot(InputSlot& s, u32 pid, u32 sampleRate, u32 maxBlock, u64 nowMs)
{
    s.pid = pid;
    s.sample_rate = sampleRate;
    s.max_block = maxBlock;
    s.heartbeat_ms.store(nowMs, std::memory_order_relaxed);
    s.capture_write_pos.store(0, std::memory_order_relaxed);
    s.flags.store(0, std::memory_order_relaxed); // claim 瞬间独占,整字 store 安全
    s.state.store(kSlotActive, std::memory_order_release);
}

Registry::ClaimResult Registry::claimInput(u32 channel, u32 pid, u32 sampleRate, u32 maxBlock, u64 nowMs)
{
    InputSlot* slot = inputSlot(channel);
    if (slot == nullptr)
    {
        return ClaimResult::kUnavailable;
    }
    InputSlot& s = *slot;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const u32 cur = s.state.load(std::memory_order_acquire);
        if (cur == kSlotFree)
        {
            u32 expected = kSlotFree;
            if (s.state.compare_exchange_strong(expected, kSlotClaimed, std::memory_order_acq_rel,
                                                std::memory_order_acquire))
            {
                fillInputSlot(s, pid, sampleRate, maxBlock, nowMs);
                ownedChannel_ = channel;
                ownedPid_ = pid;
                return ClaimResult::kClaimed;
            }
            continue; // 竞争者赢了,重试
        }

        // v10:Active 槽不再「同 pid 刷新接管」(v2 语义废除)。同 pid 刷新会让「复制轨道」带来的
        // 同 channel 复制体直接抢走原实例的槽 → 两个实例同写一个环;与 ipc §1「已被占且心跳新鲜
        // → 冲突」对齐。本实例自己的采样率重认领改走 updateOwnedInputSlot,不经本函数。

        // kSlotClaimed = 进行中(另一 claimer 正填字段,pid=0/hb=0):视为进行中,重试(3 次后 kConflict),
        // 绝不接管(pr-agent 复审:否则第二 claimer 会以 isTakeoverStale(hb==0)&&!probe(pid==0) 误接管,
        // 双实例同认为自己持有同 channel → 双写同一 SPSC 环)。
        if (cur == kSlotClaimed)
        {
            continue;
        }

        // 被占(kSlotActive):接管双条件(J10:心跳陈旧 ≥5000ms 且 pid 探活失败)。
        const u64 hb = s.heartbeat_ms.load(std::memory_order_acquire);
        if (isTakeoverStale(hb, nowMs) && !probe_(s.pid))
        {
            u32 expected = cur;
            if (s.state.compare_exchange_strong(expected, kSlotClaimed, std::memory_order_acq_rel,
                                                std::memory_order_acquire))
            {
                fillInputSlot(s, pid, sampleRate, maxBlock, nowMs);
                ownedChannel_ = channel;
                ownedPid_ = pid;
                return ClaimResult::kClaimed;
            }
            continue; // state 变了,重试
        }
        return ClaimResult::kConflict;
    }
    return ClaimResult::kConflict;
}

Registry::ClaimResult Registry::claimInputAuto(u32 channel, u32 pid, u32 sampleRate, u32 maxBlock, u64 nowMs)
{
    // v8/v10:自动认领(InputProcessor 的 channel_==0 自动选槽循环)与 claimInput 语义一致——
    // 只抢 Free 槽或陈旧+死 pid 接管,绝不「同 pid 刷新」(v2 同 pid 刷新导致 15 实例挤 ch1)。
    return claimInput(channel, pid, sampleRate, maxBlock, nowMs);
}

void Registry::updateOwnedInputSlot(u32 channel, u32 pid, u32 sampleRate, u32 maxBlock)
{
    // v10:本实例对【自己已持有】的 slot 做字段刷新(采样率切换重认领路径)。
    // 与 claimInput 的区别:不做 CAS、不抢任何非本实例的槽;非属主调用是空操作。
    InputSlot* slot = inputSlot(channel);
    if (slot == nullptr)
    {
        return;
    }
    InputSlot& s = *slot;
    if (s.pid == pid && s.state.load(std::memory_order_acquire) == kSlotActive)
    {
        s.sample_rate = sampleRate;
        s.max_block = maxBlock;
    }
}

void Registry::releaseInput(u32 channel, u32 pid)
{
    InputSlot* slot = inputSlot(channel);
    if (slot == nullptr)
    {
        return;
    }
    InputSlot& s = *slot;
    // 仅当本实例确属 owner 时才释放,防误清他人 slot。
    u32 expected = kSlotActive;
    if (s.pid == pid &&
        s.state.compare_exchange_strong(expected, kSlotFree, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        // 释放成功。
    }
}

Registry::ClaimResult Registry::claimOutput(u32 pid, u64 nowMs)
{
    OutputSlot* slot = outputSlot();
    if (slot == nullptr)
    {
        return ClaimResult::kUnavailable;
    }
    OutputSlot& s = *slot;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const u32 cur = s.state.load(std::memory_order_acquire);
        if (cur == kSlotFree)
        {
            u32 expected = kSlotFree;
            if (s.state.compare_exchange_strong(expected, kSlotClaimed, std::memory_order_acq_rel,
                                                std::memory_order_acquire))
            {
                s.pid = pid;
                s.heartbeat_ms.store(nowMs, std::memory_order_relaxed);
                s.connected_mask.store(0, std::memory_order_relaxed);
                s.config_seq.store(0, std::memory_order_relaxed);
                s.state.store(kSlotActive, std::memory_order_release);
                ownsOutput_ = true;
                ownedPid_ = pid;
                return ClaimResult::kClaimed;
            }
            continue;
        }

        // 同 pid 重认领(采样率切换 / 同进程新实例):只刷新 pid/心跳,【不清】 connected_mask。
        // 清 mask 会把 Output 自己的 Input 拓扑判定打掉 → Input 健康判定跟掉 → 总线静音(P0)。
        if (cur == kSlotActive && s.pid == pid)
        {
            s.pid = pid;
            s.heartbeat_ms.store(nowMs, std::memory_order_relaxed);
            ownsOutput_ = true;
            ownedPid_ = pid;
            return ClaimResult::kClaimed;
        }

        // kSlotClaimed = 进行中(pid=0/hb=0):重试,绝不接管(与 claimInput 同因,pr-agent 复审)。
        if (cur == kSlotClaimed)
        {
            continue;
        }

        // kSlotActive 且异 pid:接管双条件(J10)。
        const u64 hb = s.heartbeat_ms.load(std::memory_order_acquire);
        if (isTakeoverStale(hb, nowMs) && !probe_(s.pid))
        {
            u32 expected = cur;
            if (s.state.compare_exchange_strong(expected, kSlotClaimed, std::memory_order_acq_rel,
                                                std::memory_order_acquire))
            {
                s.pid = pid;
                s.heartbeat_ms.store(nowMs, std::memory_order_relaxed);
                s.connected_mask.store(0, std::memory_order_relaxed);
                s.config_seq.store(0, std::memory_order_relaxed);
                s.state.store(kSlotActive, std::memory_order_release);
                ownsOutput_ = true;
                ownedPid_ = pid;
                return ClaimResult::kClaimed;
            }
            continue;
        }
        return ClaimResult::kConflict;
    }
    return ClaimResult::kConflict;
}

void Registry::releaseOutput(u32 pid)
{
    OutputSlot* slot = outputSlot();
    if (slot == nullptr)
    {
        return;
    }
    OutputSlot& s = *slot;
    u32 expected = kSlotActive;
    if (s.pid == pid &&
        s.state.compare_exchange_strong(expected, kSlotFree, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        // 释放成功。
    }
}

void Registry::heartbeatInput(u32 channel, u64 nowMs)
{
    InputSlot* slot = inputSlot(channel);
    if (slot != nullptr && slot->state.load(std::memory_order_acquire) == kSlotActive)
    {
        slot->heartbeat_ms.store(nowMs, std::memory_order_release);
    }
}

void Registry::heartbeatOutput(u64 nowMs)
{
    OutputSlot* slot = outputSlot();
    if (slot != nullptr && slot->state.load(std::memory_order_acquire) == kSlotActive)
    {
        slot->heartbeat_ms.store(nowMs, std::memory_order_release);
    }
}

u32 Registry::connectedMask() const
{
    OutputSlot* s = outputSlot();
    return s ? s->connected_mask.load(std::memory_order_acquire) : 0;
}

void Registry::setConnectedMaskBit(u32 channel)
{
    OutputSlot* s = outputSlot();
    if (s != nullptr && channel >= 1 && channel <= kMaxChannels)
    {
        s->connected_mask.fetch_or(1u << (channel - 1), std::memory_order_release);
    }
}

void Registry::clearConnectedMaskBit(u32 channel)
{
    OutputSlot* s = outputSlot();
    if (s != nullptr && channel >= 1 && channel <= kMaxChannels)
    {
        s->connected_mask.fetch_and(~(1u << (channel - 1)), std::memory_order_release);
    }
}

void Registry::clearConnectedMask()
{
    OutputSlot* s = outputSlot();
    if (s != nullptr)
    {
        s->connected_mask.store(0, std::memory_order_release);
    }
}

u32 Registry::configSeq() const
{
    OutputSlot* s = outputSlot();
    return s ? s->config_seq.load(std::memory_order_acquire) : 0;
}

u32 Registry::bumpConfigSeq()
{
    OutputSlot* s = outputSlot();
    if (s == nullptr)
    {
        return 0;
    }
    return s->config_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool Registry::configChanged(u32& lastSeenSeq) const
{
    const u32 seq = configSeq();
    if (seq != lastSeenSeq)
    {
        lastSeenSeq = seq;
        return true;
    }
    return false;
}

Registry::ClaimResult Registry::changeGroup(u32 newGroup)
{
    if (newGroup < 1 || newGroup > kMaxGroups)
    {
        return ClaimResult::kUnavailable;
    }
    releaseOwnedSlot(); // 释放旧组 slot(state→0)
    header_ = nullptr;
    if (!handle_.release())
    {
        // 租约在途:旧组句柄未解映射,压入待回收列表,由 [M] reapPendingReleases() 回收。
        pendingReleases_.push_back(std::move(handle_));
    }
    group_ = newGroup;
    return open(); // 映射新组 registry + §4.0 初始化
}

void Registry::releaseOwnedSlot() noexcept
{
    if (ownsOutput_)
    {
        releaseOutput(ownedPid_);
        ownsOutput_ = false;
        ownedPid_ = 0;
    }
    else if (ownedChannel_ != 0)
    {
        releaseInput(ownedChannel_, ownedPid_);
        ownedChannel_ = 0;
        ownedPid_ = 0;
    }
}

} // namespace scvb
