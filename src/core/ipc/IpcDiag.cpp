// SPDX-License-Identifier: GPL-3.0-or-later
#include "IpcDiag.h"

#include <atomic>

namespace scvb
{

namespace
{
std::atomic<IpcDiagSink>& sinkSlot() noexcept
{
    static std::atomic<IpcDiagSink> slot{nullptr};
    return slot;
}
} // namespace

IpcDiagSink setIpcDiagSink(IpcDiagSink sink) noexcept
{
    return sinkSlot().exchange(sink, std::memory_order_acq_rel);
}

IpcDiagSink ipcDiagSink() noexcept
{
    return sinkSlot().load(std::memory_order_acquire);
}

void reportIpcDiag(const IpcDiagEvent& event) noexcept
{
    const IpcDiagSink sink = sinkSlot().load(std::memory_order_acquire);
    if (sink != nullptr)
    {
        sink(event);
    }
}

const char* ipcDiagOpName(IpcDiagOp op) noexcept
{
    switch (op)
    {
    case IpcDiagOp::kName:
        return "name";
    case IpcDiagOp::kLockDir:
        return "lock-dir";
    case IpcDiagOp::kLifecycleLock:
        return "lifecycle-lock";
    case IpcDiagOp::kSegmentLock:
        return "segment-lock";
    case IpcDiagOp::kShmOpen:
        return "shm-open";
    case IpcDiagOp::kShmTruncate:
        return "shm-truncate";
    case IpcDiagOp::kShmStat:
        return "shm-stat";
    case IpcDiagOp::kShmMap:
        return "shm-map";
    case IpcDiagOp::kShmUnlink:
        return "shm-unlink";
    case IpcDiagOp::kMlock:
        return "mlock";
    case IpcDiagOp::kLockDirOverride:
        return "lock-dir-override";
    }
    return "unknown";
}

} // namespace scvb
