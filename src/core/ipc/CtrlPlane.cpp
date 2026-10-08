// SPDX-License-Identifier: GPL-3.0-or-later
#include "CtrlPlane.h"

#include <cstring>

namespace scvb
{

namespace
{
// 调用方统一传 Windows 形态的全名,OS 前缀固定 "Local\"(与 Registry.cpp 的 registryFullName 同构;POSIX 后端内部再映射)。
std::wstring ctrlFullName(u32 group)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kCtrl);
}

// [J150] 严格 UTF-8 判定:拒过长编码 / UTF-16 代理区(U+D800..U+DFFF)/ > U+10FFFF / 截断的续字节。
// 与 OutputStateCodec.cpp 里 label 的解码校验同一口径 —— 那边是工程文件字节,这里是对端进程写的
// 字节,两处都是不可信输入;Output 会把读到的轨道名落进 label、随工程保存、推给 web,半个码点
// 在任何一站都会变成乱码或被存档解码判成坏值。
bool isStrictUtf8(const unsigned char* s, std::size_t n)
{
    std::size_t i = 0;
    while (i < n)
    {
        const unsigned char b0 = s[i];
        if (b0 < 0x80u)
        {
            ++i;
            continue;
        }
        std::size_t len = 0;
        u32 cp = 0;
        u32 minCp = 0;
        if ((b0 & 0xE0u) == 0xC0u)
        {
            len = 2;
            cp = b0 & 0x1Fu;
            minCp = 0x80u;
        }
        else if ((b0 & 0xF0u) == 0xE0u)
        {
            len = 3;
            cp = b0 & 0x0Fu;
            minCp = 0x800u;
        }
        else if ((b0 & 0xF8u) == 0xF0u)
        {
            len = 4;
            cp = b0 & 0x07u;
            minCp = 0x10000u;
        }
        else
        {
            return false; // 孤立续字节或 0xF8.. 前缀
        }
        if (i + len > n)
        {
            return false;
        }
        for (std::size_t k = 1; k < len; ++k)
        {
            const unsigned char b = s[i + k];
            if ((b & 0xC0u) != 0x80u)
            {
                return false;
            }
            cp = (cp << 6) | (b & 0x3Fu);
        }
        if (cp < minCp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
        {
            return false;
        }
        i += len;
    }
    return true;
}

// [J150] 取 s 的 ≤ maxBytes 字节前缀,且不切出半个码点:落点若是续字节(10xxxxxx)就回退到该序列
// 的头字节之前(与 OutputProcessor::publishConfigBroadcast 截 label 的写法同款)。
std::size_t utf8PrefixBytes(const std::string& s, std::size_t maxBytes)
{
    if (s.size() <= maxBytes)
    {
        return s.size();
    }
    std::size_t len = maxBytes;
    while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0u) == 0x80u)
    {
        --len;
    }
    return len;
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
    // 与 changeGroup 一致:handle_ 尚未打开/已 moved-from 时先判 valid(),避免把 moved-from 句柄推进
    // pendingReleases_(releaseHandle() 同模式)。
    if (handle_.valid() && !handle_.release(steadyNowMs()))
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

bool CtrlPlane::isRingFull(u32 channel) const
{
    const CtrlRing* ring = ringAt(channel);
    if (ring == nullptr)
    {
        return false;
    }
    const u32 w = ring->write_pos.load(std::memory_order_relaxed);
    const u32 r = ring->read_pos.load(std::memory_order_acquire);
    return w - r >= kCtrlRingCapacity;
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

void CtrlPlane::release()
{
    base_ = nullptr;
    // 与 changeGroup 释放旧句柄同构:valid() 守卫防 moved-from;租约在途 → pendingReleases_ 延迟回收。
    if (handle_.valid() && !handle_.release(steadyNowMs()))
    {
        pendingReleases_.push_back(std::move(handle_));
    }
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

CtrlBroadcast* CtrlPlane::broadcast() const
{
    if (base_ == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<CtrlBroadcast*>(base_ + kCtrlBroadcastOffset);
}

void CtrlPlane::writeBroadcast(const CtrlBroadcastSnapshot& s)
{
    CtrlBroadcast* b = broadcast();
    if (b == nullptr)
    {
        return; // 段未打开:静默不写(Output 尚未 claim / 已释放)
    }

    // seqlock 写侧:奇数进临界区 → 写载荷 → 偶数发布完成。读方看到奇数或前后不等即重读。
    // 奇数增量取 relaxed + 紧跟一道 release fence:release **store** 只挡「之前的写下沉」,
    // 挡不住「其后的载荷写上浮到奇数 seq 之前」—— 那正是这里要防的方向。载荷 2KB、跨进程、
    // 读写双方分别编译,窗口比进程内的 PlayheadShot 宽两个量级;写法与 PlayheadShot::publish 一致(M04
    // 起两处同款,理由见那里)。
    b->seq.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    b->lead_select = s.lead_select;
    for (u32 i = 0; i < kMaxChannels; ++i)
    {
        b->channels[i] = s.channels[i];
        std::memcpy(b->labels[i], s.labels[i], kCtrlLabelBytes);
        b->labels[i][kCtrlLabelBytes - 1] = '\0'; // 防漏 NUL 让读方越界扫描
    }
    // config_seq 最后写:读方即便在 seqlock 之外瞄一眼它,也不会看到「新版本号 + 旧载荷」。
    b->config_seq.store(s.config_seq, std::memory_order_release);

    b->seq.fetch_add(1, std::memory_order_release);
}

bool CtrlPlane::readBroadcast(CtrlBroadcastSnapshot& out) const
{
    const CtrlBroadcast* b = broadcast();
    if (b == nullptr)
    {
        return false;
    }

    const u32 before = b->seq.load(std::memory_order_acquire);
    if ((before & 1u) != 0u)
    {
        return false; // 写者正在写
    }

    out.config_seq = b->config_seq.load(std::memory_order_acquire);
    out.lead_select = b->lead_select;
    for (u32 i = 0; i < kMaxChannels; ++i)
    {
        out.channels[i] = b->channels[i];
        std::memcpy(out.labels[i], b->labels[i], kCtrlLabelBytes);
        out.labels[i][kCtrlLabelBytes - 1] = '\0'; // 对端可能是旧版本/被截断的字节,读方自保
    }

    // 载荷读取不得越过第二次 seq 读(seqlock 读边界)。
    std::atomic_thread_fence(std::memory_order_acquire);
    return b->seq.load(std::memory_order_relaxed) == before;
}

CtrlTrackName* CtrlPlane::trackNameAt(u32 channel) const
{
    if (base_ == nullptr || channel < 1 || channel > kMaxChannels)
    {
        return nullptr;
    }
    return reinterpret_cast<CtrlTrackName*>(base_ + kCtrlTrackNamesOffset + (channel - 1) * sizeof(CtrlTrackName));
}

void CtrlPlane::writeTrackName(u32 channel, u64 ownerHeartbeatMs, const std::string& utf8)
{
    CtrlTrackName* e = trackNameAt(channel);
    if (e == nullptr)
    {
        return; // 段未打开 / channel 非法:静默不写
    }
    const std::size_t len = utf8PrefixBytes(utf8, kCtrlTrackNameBytes - 1);

    // seqlock 写侧,奇偶协议同 writeBroadcast(奇数进临界区 + release fence 挡住载荷上浮)。
    // 每条单写,所以用 load + store 而不是 fetch_add,顺带把**奇数残值**扶正:上一任写方若死在
    // 临界区里(seq 停在奇数),fetch_add 会把本次写入翻成「偶数 = 稳定」、载荷却正在写 ——
    // `| 1u` 让本次无论从奇从偶出发,临界区内都是奇数、写完都是偶数。
    // u32 回绕到 0(= 从未写过)要 2^31 次写入,本条每拍一次 ≈ 2.7 年不停机,落到 0 的那一拍
    // 读方判「没有轨道名」、保持现状,下一拍即恢复。
    const u32 s = e->seq.load(std::memory_order_relaxed) | 1u;
    e->seq.store(s, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    e->owner_heartbeat_ms = ownerHeartbeatMs;
    std::memcpy(e->utf8, utf8.data(), len);
    std::memset(e->utf8 + len, 0, kCtrlTrackNameBytes - len); // 槽内补 0:NUL 结尾 + 不留上一次的尾巴

    e->seq.store(s + 1u, std::memory_order_release);
}

bool CtrlPlane::readTrackName(u32 channel, u64& ownerHeartbeatMs, std::string& name) const
{
    const CtrlTrackName* e = trackNameAt(channel);
    if (e == nullptr)
    {
        return false;
    }
    const u32 before = e->seq.load(std::memory_order_acquire);
    if (before == 0u || (before & 1u) != 0u)
    {
        return false; // 从未写过 / 写方正在写
    }
    const u64 owner = e->owner_heartbeat_ms;
    char buf[kCtrlTrackNameBytes];
    std::memcpy(buf, e->utf8, kCtrlTrackNameBytes);
    // 载荷读取不得越过第二次 seq 读(seqlock 读边界)。
    std::atomic_thread_fence(std::memory_order_acquire);
    if (e->seq.load(std::memory_order_relaxed) != before)
    {
        return false; // 撕裂:本拍放弃,调用方下一拍再读(不自旋)
    }
    buf[kCtrlTrackNameBytes - 1] = '\0'; // 对端可能是旧版本 / 被截断的字节,读方自保
    const std::size_t len = std::strlen(buf);
    if (!isStrictUtf8(reinterpret_cast<const unsigned char*>(buf), len))
    {
        return false;
    }
    ownerHeartbeatMs = owner;
    name.assign(buf, len);
    return true;
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
    // capture_enabled 最后写 = 「发布完成」旗标:读方握手(见 scvb_ipc_peer runGlobalInfoReader)以
    // capture_enabled != 0 判定就绪。若先写 capture_enabled,读方会在数组字段尚未写完时退出轮询,读到
    // 全零 gap_count(IPC-16 flake:gap_count14 偶发 0)。该次序只保证 0→1 首次发布一致性(读方 acquire
    // 到 capture_enabled=1 时,之前所有字段的写入均已对其可见);稳态 250ms 周期重刷不保证跨 48 字段
    // 原子性(诊断快照,可接受)。
    g->output_sample_rate.store(s.output_sample_rate, std::memory_order_release);
    g->flags.store(s.flags, std::memory_order_release);
    for (u32 i = 0; i < kMaxChannels; ++i)
    {
        g->gap_count[i].store(s.gap_count[i], std::memory_order_release);
        g->overlap_count[i].store(s.overlap_count[i], std::memory_order_release);
        g->epoch_summary[i].store(s.epoch_summary[i], std::memory_order_release);
    }
    g->capture_enabled.store(s.capture_enabled, std::memory_order_release);
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
    // seq 协议(防撕裂读):seq=0 标记「在写」+ release fence → channel/op/value(relaxed)→
    // seq=真值(release),最后 write_pos.store(w+1, release) 发布。消费者见 write_pos 推进即知记录已提交;
    // 读侧对称:s1(acquire)→ 三条 relaxed 读 → acquire fence → s2,s1==s2 才算一致(见 dequeue)。
    // seq 由共享 write_pos 派生(seq = w+1):记录 w 的 seq=w+1,消费者 s1==r+1 判据天然成立,
    // 且生产者重启后新实例续用共享 write_pos,seq 不中断(环不卡死)。
    // 理论边界:w+1 在 u32 回绕点为 0,与「在写」标记(seq=0)撞车;≈5.4 年 @25Hz 才回绕一次,
    // v1 不接受此理论边界(abi+1 增补清单),故不做跳过处理。
    // [B 线 M06a] seqlock 写侧 fence:release **store** 只约束它之前的访问,挡不住后面三条 relaxed
    // 写被提到 seq=0 之前可见(x86 的 TSO 天然不重排,arm64 会)。必须是 store 之后的 release fence。
    rec.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
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
        // [B 线 M06a] seqlock 读侧 fence:acquire **load** 只约束它之后的访问,挡不住上面三条 relaxed
        // 读被推迟到 s2 之后;arm64 上实测会读到同槽下一圈的 value 而 s1==s2(命令环双线程用例 4/30 红)。
        std::atomic_thread_fence(std::memory_order_acquire);
        const u32 s2 = rec.seq.load(std::memory_order_relaxed);
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
