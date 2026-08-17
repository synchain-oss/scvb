// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// S1 spike —— Registry:claim/CAS/心跳/双阈值陈旧判定(ipc §1 + v1 [J10])。
// 仅消息线程调用 claim/接管/heartbeat;音频线程只读 slot 的原子字段。

#include <atomic>
#include <cstddef>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "SegmentBackendWin32.h"
#include "SegmentLayout.h"

namespace scvb
{

// 双阈值陈旧判定(ipc v1 [J10])。
inline constexpr u64 kStaleDisplayMs = 2000; // 仅驱动 UI「失联」显示
inline constexpr u64 kTakeoverMs = 5000; // 接管需 ≥5000ms 且 pid 探活失败

// registry 段布局偏移(与 SegmentLayout.h 的 static_assert 对齐)。
inline constexpr std::size_t kRegistrySegmentSize = 4096;
inline constexpr std::size_t kInputSlotOffset = sizeof(RegistryHeader);
inline constexpr std::size_t kOutputSlotOffset = sizeof(RegistryHeader) + kMaxChannels * sizeof(InputSlot);

// 稳态时钟毫秒(GetTickCount64:系统级单调、跨进程同一时钟源——心跳必须用它,
// 不能用 std::chrono::steady_clock 的进程内 epoch)。
inline u64 steadyNowMs()
{
    return static_cast<u64>(::GetTickCount64());
}

// pid 存活探测(OpenProcess + GetExitCodeProcess)。探活失败(句柄拿不到/退出码非 STILL_ACTIVE)
// 一律判「死」——J10 的「pid 存活探测失败」条件。
inline bool isProcessAlive(u32 pid)
{
    if (pid == 0)
    {
        return false;
    }
    const HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr)
    {
        return false;
    }
    DWORD code = 0;
    const bool ok = ::GetExitCodeProcess(h, &code) != 0;
    ::CloseHandle(h);
    return ok && code == STILL_ACTIVE;
}

// v9:Input 通道解析策略(env > state > 已认领保持 > 自动 0)。
// 已认领的自动模式实例跨宿主 re-prepare 必须保持原 channel:若每次被 state 默认值
// 打回 0,auto 路径会改抢最低空闲槽 → 实例迁槽、旧槽心跳陈旧但 state 仍 Active
// (幽灵槽,实测 15 轨 mask 变 32764 缺两位)。
inline u32 resolveInputChannelForClaim(u32 envCh, u32 stateCh, bool claimed, u32 currentCh)
{
    if (envCh >= 1 && envCh <= kMaxChannels)
    {
        return envCh;
    }
    if (stateCh >= 1 && stateCh <= kMaxChannels)
    {
        return stateCh;
    }
    if (claimed && currentCh >= 1 && currentCh <= kMaxChannels)
    {
        return currentCh;
    }
    return 0;
}

// 显示陈旧(>2000ms)。
inline bool isStaleDisplay(u64 heartbeatMs, u64 nowMs)
{
    return heartbeatMs == 0 || nowMs - heartbeatMs > kStaleDisplayMs;
}

// 接管条件之一:心跳陈旧 ≥5000ms(另一条件是 pid 探活失败,由调用方组合)。
inline bool isTakeoverStale(u64 heartbeatMs, u64 nowMs)
{
    return heartbeatMs == 0 || nowMs - heartbeatMs >= kTakeoverMs;
}

class Registry
{
public:
    enum class ClaimResult
    {
        kClaimed, // 本实例取得所有权(state=2)
        kConflict, // 被活跃实例占用(不满足接管双条件)
        kUnavailable, // 段未打开/映射失败
        kAbiMismatch // abi 不符(拒连)
    };

    // 组号 G 为构造参数(ipc v1.5 [J66]);1..8。
    explicit Registry(u32 group) : group_(group) {}
    ~Registry();

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // 映射 + §4.0 初始化(消息线程)。
    ClaimResult open();

    bool isOpen() const { return header_ != nullptr; }
    u32 group() const { return group_; }

    RegistryHeader* header() { return header_; }
    InputSlot* inputSlot(u32 channel); // channel ∈ [1, 15]
    OutputSlot* outputSlot();

    // claim 一个 Input channel(消息线程;CAS 0→1 填字段→2;被占且满足接管双条件则接管)。
    ClaimResult claimInput(u32 channel, u32 pid, u32 sampleRate, u32 maxBlock, u64 nowMs);

    // 自动认领专用(v8):只拿 Free slot(或陈旧+死 pid 接管);**不做同 pid 刷新/接管**。
    // v2 的同 pid 刷新曾让每个新实例都把第一个实例的 slot「认领成功」→ 15 实例全挤 ch1
    // (channel 塌缩,导出只剩单轨内容)。此变体供 InputProcessor 的 channel_==0 自动选槽循环使用。
    ClaimResult claimInputAuto(u32 channel, u32 pid, u32 sampleRate, u32 maxBlock, u64 nowMs);
    // 释放本实例占用的 Input channel(析构路径,消息线程)。
    void releaseInput(u32 channel, u32 pid);

    // Output 占用 OutputSlot(同语义,ADR-002 第二个 Output 只读)。
    ClaimResult claimOutput(u32 pid, u64 nowMs);
    void releaseOutput(u32 pid);

    // 心跳写(消息线程定时器)。
    void heartbeatInput(u32 channel, u64 nowMs);
    void heartbeatOutput(u64 nowMs);

private:
    static void fillInputSlot(InputSlot& s, u32 pid, u32 sampleRate, u32 maxBlock, u64 nowMs);

    u32 group_ = 1;
    SegmentView view_;
    RegistryHeader* header_ = nullptr;
};

} // namespace scvb
