// SPDX-License-Identifier: GPL-3.0-or-later
#include "Registry.h"

namespace scvb
{

Registry::~Registry()
{
    // SegmentView 析构时 Unmap/CloseHandle。
}

Registry::ClaimResult Registry::open()
{
    const auto r = SegmentBackendWin32::map(segmentRegistryName(group_), kRegistrySegmentSize, view_);
    if (r != InitResult::kOk)
    {
        return ClaimResult::kUnavailable;
    }
    auto* header = static_cast<RegistryHeader*>(view_.base);
    const auto ir =
        SegmentBackendWin32::initHeader(view_, &header->magic, &header->abi, &header->generation, kInputSlotOffset);
    if (ir == InitResult::kAbiMismatch)
    {
        view_.reset();
        return ClaimResult::kAbiMismatch;
    }
    if (ir != InitResult::kOk)
    {
        view_.reset();
        return ClaimResult::kUnavailable;
    }
    header_ = header;
    return ClaimResult::kClaimed;
}

InputSlot* Registry::inputSlot(u32 channel)
{
    if (header_ == nullptr || channel == 0 || channel > kMaxChannels)
    {
        return nullptr;
    }
    auto* base = static_cast<char*>(view_.base);
    return reinterpret_cast<InputSlot*>(base + kInputSlotOffset + (channel - 1) * sizeof(InputSlot));
}

OutputSlot* Registry::outputSlot()
{
    if (header_ == nullptr)
    {
        return nullptr;
    }
    auto* base = static_cast<char*>(view_.base);
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
                return ClaimResult::kClaimed;
            }
            continue; // 竞争者赢了,重试
        }

        // 同 pid 重认领(采样率切换:宿主对所有插件重新 prepareToPlay → doClaim 再走一遍)。
        // slot 被自己占用时永远不满足接管双条件(pid 探活成功),必须在此直接刷新并认领成功,
        // 否则 Input 收到 kConflict 会停心跳、停环,Output 收窄 connected_mask → 总线永久静音(P0)。
        if (cur == kSlotActive && s.pid == pid)
        {
            // 自己拥有 slot,无需 CAS;fillInputSlot 刷新 sample_rate/max_block/心跳。
            // 注意:fillInputSlot 会把 flags 清零 —— spike 里 capture 未启用、muted 位由 Input
            // 100ms 后重新置位,可接受;T06 正式版需保留 kFlagCapturing。
            fillInputSlot(s, pid, sampleRate, maxBlock, nowMs);
            return ClaimResult::kClaimed;
        }

        // 被占:接管双条件(J10:心跳陈旧 ≥5000ms 且 pid 探活失败)。
        const u64 hb = s.heartbeat_ms.load(std::memory_order_acquire);
        if (isTakeoverStale(hb, nowMs) && !isProcessAlive(s.pid))
        {
            u32 expected = cur;
            if (s.state.compare_exchange_strong(expected, kSlotClaimed, std::memory_order_acq_rel,
                                                std::memory_order_acquire))
            {
                fillInputSlot(s, pid, sampleRate, maxBlock, nowMs);
                return ClaimResult::kClaimed;
            }
            continue; // state 变了,重试
        }
        return ClaimResult::kConflict;
    }
    return ClaimResult::kConflict;
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
            return ClaimResult::kClaimed;
        }

        const u64 hb = s.heartbeat_ms.load(std::memory_order_acquire);
        if (isTakeoverStale(hb, nowMs) && !isProcessAlive(s.pid))
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

} // namespace scvb
