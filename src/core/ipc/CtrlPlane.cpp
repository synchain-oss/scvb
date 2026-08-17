// SPDX-License-Identifier: GPL-3.0-or-later
#include "CtrlPlane.h"

namespace scvb
{

namespace
{
// v1 仅 Windows,OS 前缀固定 "Local\"(与 Registry.cpp 的 registryFullName 同构;POSIX 前缀归 v2)。
std::wstring ctrlFullName(u32 group)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kCtrl);
}
} // namespace

// 停摆看门狗阈值(01 §4.2 / §4.3-b)。
inline constexpr u64 kWatchdogStallMs = 500; // 自身 blockCounter 停滞 ≥0.5s
inline constexpr u64 kReacquireIntervalMs = 200; // §4.3-b 让位协议:置位后延迟 ≥200ms(或 muted 回执先到者)

CtrlPlane::CtrlPlane(ISegmentBackend& backend, u32 group) : backend_(backend), group_(group) {}

CtrlPlane::~CtrlPlane()
{
    base_ = nullptr;
    handle_.release(); // 握手释放(消息线程;leaseCount 归零后经 backend->unmap 解映射)
}

InitResult CtrlPlane::open()
{
    base_ = nullptr;
    handle_.release(); // 释放旧句柄(重开路径)
    SegmentView view;
    const auto r = backend_.createOrOpen(ctrlFullName(group_), kCtrlSegmentSize, view);
    if (r != InitResult::kOk)
    {
        return r;
    }
    auto* header = static_cast<CtrlHeader*>(view.base);
    // owner 角色(createOrOpen):allowOverwrite=true(覆盖式重初始化分支仅限创建者)。
    const auto ir = backend_.initHeader(view, &header->magic, &header->abi, &header->generation, kCtrlBroadcastOffset,
                                        /*initData=*/{}, /*allowOverwrite=*/true);
    if (ir != InitResult::kOk)
    {
        backend_.unmap(view);
        return ir;
    }
    handle_ = SegmentHandle(std::move(view), &backend_);
    base_ = static_cast<unsigned char*>(handle_.base());
    return InitResult::kOk;
}

u32 CtrlPlane::generation() const
{
    if (base_ == nullptr)
    {
        return 0;
    }
    return static_cast<CtrlHeader*>(handle_.base())->generation.load(std::memory_order_acquire);
}

CtrlRing* CtrlPlane::ringAt(u32 channel) const
{
    if (base_ == nullptr || channel < 1 || channel > kMaxChannels)
    {
        return nullptr;
    }
    return reinterpret_cast<CtrlRing*>(base_ + kCtrlRingsOffset + (channel - 1) * sizeof(CtrlRing));
}

OutputGlobalInfo* CtrlPlane::globalInfo() const
{
    if (base_ == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<OutputGlobalInfo*>(base_ + kCtrlGlobalInfoOffset);
}

void CtrlPlane::refreshGlobalInfo(const OutputGlobalInfoSnapshot& s)
{
    OutputGlobalInfo* g = globalInfo();
    if (g == nullptr)
    {
        return;
    }
    g->capture_enabled.store(s.capture_enabled, std::memory_order_relaxed);
    g->output_sample_rate.store(s.output_sample_rate, std::memory_order_relaxed);
    g->flags.store(s.flags, std::memory_order_relaxed);
    for (u32 i = 0; i < kMaxChannels; ++i)
    {
        g->gap_count[i].store(s.gap_count[i], std::memory_order_relaxed);
        g->overlap_count[i].store(s.overlap_count[i], std::memory_order_relaxed);
        g->epoch_summary[i].store(s.epoch_summary[i], std::memory_order_relaxed);
    }
}

OutputGlobalInfoSnapshot CtrlPlane::readGlobalInfo() const
{
    OutputGlobalInfoSnapshot s;
    const OutputGlobalInfo* g = globalInfo();
    if (g == nullptr)
    {
        return s;
    }
    s.capture_enabled = g->capture_enabled.load(std::memory_order_acquire);
    s.output_sample_rate = g->output_sample_rate.load(std::memory_order_acquire);
    s.flags = g->flags.load(std::memory_order_acquire);
    for (u32 i = 0; i < kMaxChannels; ++i)
    {
        s.gap_count[i] = g->gap_count[i].load(std::memory_order_acquire);
        s.overlap_count[i] = g->overlap_count[i].load(std::memory_order_acquire);
        s.epoch_summary[i] = g->epoch_summary[i].load(std::memory_order_acquire);
    }
    return s;
}

bool CtrlPlane::enqueue(u32 channel, CtrlOp op, u64 value)
{
    CtrlRing* ring = ringAt(channel);
    if (ring == nullptr)
    {
        return false;
    }
    const u32 w = ring->write_pos.load(std::memory_order_relaxed);
    const u32 r = ring->read_pos.load(std::memory_order_acquire);
    if (w - r >= kCtrlRingCapacity)
    {
        // 满环:丢最旧 → 推进 read_pos 一格(消费者未来从下一条读起)+ 计数。
        ring->read_pos.fetch_add(1, std::memory_order_relaxed);
        ring->overflow_count.fetch_add(1, std::memory_order_relaxed);
    }
    const u32 idx = w & (kCtrlRingCapacity - 1);
    ring->records[idx] = CtrlRecord{nextSeq_++, channel, op, value};
    ring->write_pos.store(w + 1, std::memory_order_release);
    return true;
}

bool CtrlPlane::dequeue(u32 channel, CtrlRecord& out)
{
    CtrlRing* ring = ringAt(channel);
    if (ring == nullptr)
    {
        return false;
    }
    const u32 r = ring->read_pos.load(std::memory_order_acquire);
    const u32 w = ring->write_pos.load(std::memory_order_acquire);
    if (r == w)
    {
        return false;
    }
    out = ring->records[r & (kCtrlRingCapacity - 1)];
    ring->read_pos.store(r + 1, std::memory_order_release);
    return true;
}

u32 CtrlPlane::overflowCount(u32 channel) const
{
    CtrlRing* ring = ringAt(channel);
    return ring ? ring->overflow_count.load(std::memory_order_acquire) : 0;
}

bool CtrlPlane::anyOnlineWriteHeadAdvanced() const
{
    if (!writeHeadSource_)
    {
        return false;
    }
    const u32 mask = connectedMaskSource_ ? connectedMaskSource_() : 0;
    for (u32 ch = 1; ch <= kMaxChannels; ++ch)
    {
        if ((mask & (1u << (ch - 1))) == 0)
        {
            continue; // 只看在线轨
        }
        if (writeHeadSource_(ch) > lastWriteHead_[ch - 1])
        {
            return true;
        }
    }
    return false;
}

void CtrlPlane::updateWriteHeadBaseline()
{
    if (!writeHeadSource_)
    {
        return;
    }
    for (u32 ch = 1; ch <= kMaxChannels; ++ch)
    {
        lastWriteHead_[ch - 1] = writeHeadSource_(ch);
    }
}

u32 CtrlPlane::lowestOnlineChannel(u32 mask)
{
    for (u32 ch = 1; ch <= kMaxChannels; ++ch)
    {
        if ((mask & (1u << (ch - 1))) != 0)
        {
            return ch;
        }
    }
    return 0;
}

u32 CtrlPlane::nextOnlineChannel(u32 mask, u32 cur)
{
    for (u32 ch = cur + 1; ch <= kMaxChannels; ++ch)
    {
        if ((mask & (1u << (ch - 1))) != 0)
        {
            return ch;
        }
    }
    return 0;
}

WatchdogResult CtrlPlane::tickWatchdog(u64 nowMs)
{
    WatchdogResult r;
    if (!blockCounterSource_ || !writeHeadSource_)
    {
        return r; // 看门狗未启用(未注入源)
    }

    if (!watchdogInit_)
    {
        watchdogInit_ = true;
        lastBlockValue_ = blockCounterSource_();
        lastBlockAdvanceMs_ = nowMs;
        updateWriteHeadBaseline(); // 初始化各轨 write_head 基线
        return r;
    }

    const u64 block = blockCounterSource_();
    const bool blockAdvanced = (block != lastBlockValue_);
    if (blockAdvanced)
    {
        lastBlockValue_ = block;
        lastBlockAdvanceMs_ = nowMs;
    }

    // 1) 恢复:tripped 状态下 blockCounter 恢复推进 → 按 §4.3-b 让位协议逐轨重置信位(不得硬切)。
    if (tripped_)
    {
        if (blockAdvanced)
        {
            tripped_ = false;
            reacquireNext_ = lowestOnlineChannel(onlineMaskAtTrip_);
            lastReacquireMs_ = nowMs;
            if (reacquireNext_ != 0)
            {
                r.action = WatchdogAction::kReacquireBit;
                r.channel = reacquireNext_;
                reacquireNext_ = nextOnlineChannel(onlineMaskAtTrip_, reacquireNext_);
            }
        }
        return r; // 仍停摆(或已开始重置信位);本 tick 不再检测其它
    }

    // 2) 让位协议重置信位序列推进(每 ≥200ms 一个在线轨,channel 升序)。
    if (reacquireNext_ != 0 && nowMs - lastReacquireMs_ >= kReacquireIntervalMs)
    {
        r.action = WatchdogAction::kReacquireBit;
        r.channel = reacquireNext_;
        reacquireNext_ = nextOnlineChannel(onlineMaskAtTrip_, reacquireNext_);
        lastReacquireMs_ = nowMs;
        return r;
    }

    // 3) 停摆检测:自身 blockCounter 停滞 ≥0.5s ∧ 任一在线轨 write_head 仍推进。
    //    只看自身停滞会把「工程整体停止播放」误判为停摆(那时 write_head 也不推进)。
    const u64 blockStalledMs = nowMs - lastBlockAdvanceMs_;
    if (blockStalledMs >= kWatchdogStallMs && anyOnlineWriteHeadAdvanced())
    {
        tripped_ = true;
        onlineMaskAtTrip_ = connectedMaskSource_ ? connectedMaskSource_() : 0;
        r.action = WatchdogAction::kClearMask;
        return r;
    }

    // 4) 更新 write_head 基线(供下一 tick 的「仍推进」判定)。
    updateWriteHeadBaseline();
    return r;
}

} // namespace scvb
