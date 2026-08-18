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
    handle_.release(steadyNowMs()); // 握手释放(消息线程;租约归零且宽限期届满后经 backend->unmap 解映射)
    // pendingReleases_ 的 SegmentHandle 元素随 vector 析构各自 release(进程退出兜底)。
}

void CtrlPlane::reapPendingReleases(u64 nowMs)
{
    // [M] 心跳/轮询周期调用:逐项 release(nowMs),成功(租约归零且宽限期届满)即移除并解映射。
    for (auto it = pendingReleases_.begin(); it != pendingReleases_.end();)
    {
        if (it->release(nowMs))
        {
            it = pendingReleases_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

InitResult CtrlPlane::open()
{
    base_ = nullptr;
    if (!handle_.release(steadyNowMs()))
    {
        // 租约在途/宽限期未满:旧句柄未解映射,压入待回收列表,由 [M] reapPendingReleases() 回收(防泄漏)。
        pendingReleases_.push_back(std::move(handle_));
    }
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

InitResult CtrlPlane::changeGroup(u32 newGroup)
{
    if (newGroup < 1 || newGroup > kMaxGroups)
    {
        return InitResult::kFailed;
    }
    base_ = nullptr;
    // 组变更可能发生在 prepare/open 之前(handle_ 尚未打开或已 moved-from):先判 valid(),与
    // releaseHandle() 同模式,避免把 moved-from 句柄推进 pendingReleases_。
    if (handle_.valid() && !handle_.release(steadyNowMs()))
    {
        pendingReleases_.push_back(std::move(handle_));
    }
    group_ = newGroup;
    return open();
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
    // 写侧 release、读侧 acquire(见 readGlobalInfo)配对:诊断快照无需跨 48 字段的原子一致性,
    // 但单字段须有 happens-before,保证 Input 读到任一字段即见其完整写入。
    g->capture_enabled.store(s.capture_enabled, std::memory_order_release);
    g->output_sample_rate.store(s.output_sample_rate, std::memory_order_release);
    g->flags.store(s.flags, std::memory_order_release);
    for (u32 i = 0; i < kMaxChannels; ++i)
    {
        g->gap_count[i].store(s.gap_count[i], std::memory_order_release);
        g->overlap_count[i].store(s.overlap_count[i], std::memory_order_release);
        g->epoch_summary[i].store(s.epoch_summary[i], std::memory_order_release);
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
        // 满环:丢最旧。SPSC 纪律:生产者【不碰 read_pos】(read_pos 仅消费者写)——由消费者在
        // dequeue 检测 w-r>capacity 时自行追到最旧可用;此处只记溢出计数。
        ring->overflow_count.fetch_add(1, std::memory_order_relaxed);
    }
    const u32 idx = w & (kCtrlRingCapacity - 1);
    CtrlRecord& rec = ring->records[idx];
    // seq 协议(防撕裂读):seq=0(release)标记「在写」→ channel/op/value(relaxed)→ seq=真值(release),
    // 最后 write_pos.store(w+1, release) 发布。消费者见 write_pos 推进即知记录已提交。
    // seq 由共享 write_pos 派生(seq = w+1):记录 w 的 seq=w+1,消费者 s1==r+1 判据天然成立,
    // 且生产者重启后新实例续用共享 write_pos,seq 不中断(环不卡死)。
    // 理论边界:w+1 在 u32 回绕点为 0,与「在写」标记(seq=0)撞车;≈5.4 年 @25Hz 才回绕一次,
    // v1 不接受此理论边界(abi+1 增补清单),故不做跳过处理。
    rec.seq.store(0, std::memory_order_release);
    rec.channel.store(channel, std::memory_order_relaxed);
    rec.op.store(static_cast<u32>(op), std::memory_order_relaxed);
    rec.value.store(value, std::memory_order_relaxed);
    rec.seq.store(w + 1, std::memory_order_release);
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

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        u32 r = ring->read_pos.load(std::memory_order_acquire);
        const u32 w = ring->write_pos.load(std::memory_order_acquire);
        if (r == w)
        {
            return false; // 空
        }

        if (w - r > kCtrlRingCapacity)
        {
            // 生产者已套圈(丢最旧):追到最旧可用,差额吞掉(与 overflow_count 口径自洽)。
            r = w - kCtrlRingCapacity;
            ring->read_pos.store(r, std::memory_order_release);
        }

        CtrlRecord& rec = ring->records[r & (kCtrlRingCapacity - 1)];
        const u32 s1 = rec.seq.load(std::memory_order_acquire);
        if (s1 == 0)
        {
            return false; // 记录在写(seq=0 标记)
        }
        const u32 ch = rec.channel.load(std::memory_order_relaxed);
        const u32 opv = rec.op.load(std::memory_order_relaxed);
        const u64 val = rec.value.load(std::memory_order_relaxed);
        const u32 s2 = rec.seq.load(std::memory_order_acquire);
        if (s1 != s2)
        {
            continue; // 撕裂读(生产者正覆写同槽)→ 重试
        }
        if (s1 != r + 1)
        {
            // 槽被更高序号记录覆写(本快照 w 陈旧、未触发套圈检测)→ 重试:重读 w 并重做套圈跳。
            // 不能消费本记录(它对应 read_pos=s1-1,须在其正确位置读到),否则跳过中间有效记录。
            continue;
        }

        // s1 == r+1:期望记录,一致性快照成功。
        out.seq.store(s1, std::memory_order_relaxed);
        out.channel.store(ch, std::memory_order_relaxed);
        out.op.store(opv, std::memory_order_relaxed);
        out.value.store(val, std::memory_order_relaxed);
        ring->read_pos.store(r + 1, std::memory_order_release);
        return true;
    }
    return false; // 多次撕裂/套圈,放弃(下次 [M] 25Hz 重试)
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
