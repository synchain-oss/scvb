// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// LeadTimeline —— `lead_select` 在时间线上的取值记录([SL-216] / J136)。
//
// 为什么要它:`lead_select` 是宿主可自动化的全局参数(J58「每句换主唱」),插件拿不到宿主的
// 自动化曲线本身,只能在**播放经过时**看到它当下的值。J136 要求主唱居中进入分析的槽位/平衡计算
// (而不只是播放期把那一轨强制拉回中间),分析就得知道「哪一段时间谁是主唱」—— 于是 Output 在
// 走带播放时逐块记下 `lead_select` 的值,分析按区间取这里的记录。
//
// 三件东西:
//   · `LeadTimeline`:样本域的分段常值记录([t0,t1) → 0..15),后写覆盖先写,相邻同值合并。
//     只在消息线程上用(Output 持 lifecycleMutex_ 访问)。
//   · `LeadRecorder`:音频线程 → 消息线程的单生产者单消费者队列,音频线程每块 push 一条,
//     消息线程每拍排干进 `LeadTimeline`。音频线程侧无分配、无锁。
//   · `majorityLead`:分析取值 —— 一个区间里**已记录**的样本中占比最大的那个值。
//
// 纯 C++17、JUCE-free,可在 scvb_tests 里直接测。

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace scvb::analysis
{

inline constexpr int kLeadMaxValue = 15; // lead_select 值域 0..15(0 = 无主唱 / 遵循分析)

// 一段记录:样本半开区间 [t0, t1) 上 lead_select 恒为 `lead`。
struct LeadRun
{
    std::int64_t t0 = 0;
    std::int64_t t1 = 0;
    int lead = 0;
};

class LeadTimeline
{
public:
    // 段数上限。正常用法(一首歌里每句换一次主唱)是几十到几百段;
    // 到顶之后新的写入整条丢弃并计数(见 droppedWrites),已有记录不动。
    static constexpr std::size_t kMaxRuns = 65536;

    // 用 `lead` 覆盖 [t0, t1)。t1 <= t0、t0 < 0、lead 越界 → 忽略(返回 false)。
    // 到段数上限 → 丢弃并计数(返回 false)。
    bool write(std::int64_t t0, std::int64_t t1, int lead);

    void clear() noexcept { runs_.clear(); }
    std::size_t size() const noexcept { return runs_.size(); }
    bool empty() const noexcept { return runs_.empty(); }
    std::uint64_t droppedWrites() const noexcept { return dropped_; }

    // 全部记录,按 t0 升序、互不重叠。
    std::vector<LeadRun> runs() const;
    // 与 [t0, t1) 有交集的记录(不裁剪)。
    std::vector<LeadRun> runsOverlapping(std::int64_t t0, std::int64_t t1) const;

    // 整份替换(state 回灌用)。调用方保证 runs 已通过 decodeLeadChunk 的校验。
    void assign(const std::vector<LeadRun>& runs);

private:
    struct Run
    {
        std::int64_t t1 = 0;
        int lead = 0;
    };
    std::map<std::int64_t, Run> runs_; // key = t0
    std::uint64_t dropped_ = 0;
};

// 区间 [t0, t1) 的主唱:在**已记录**的样本里按覆盖样本数取最多的那个值;
// 平局取较小的值;整段都没有记录 → 0(无主唱)。
// 0 也是一个值(「这段没有选主唱」),参与计数 —— 未记录 ≠ 记录了 0,但两者的结论都是 0。
int majorityLead(const std::vector<LeadRun>& runs, std::int64_t t0, std::int64_t t1);

// 音频线程 → 消息线程的记录队列(SPSC,定长环,满则丢并计数)。
class LeadRecorder
{
public:
    static constexpr std::uint32_t kCapacity = 4096; // 2 的幂

    // 音频线程:记一块。不分配、不加锁。
    void record(std::int64_t t0, std::int64_t t1, int lead) noexcept;

    // 消息线程:排干进 timeline,返回取出的条数。
    std::size_t drainInto(LeadTimeline& timeline);
    // 消息线程:排干并丢弃(载入另一份工程时,队列里是旧工程那次播放的记录)。
    std::size_t discard();

    std::uint32_t droppedRecords() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    struct Rec
    {
        std::int64_t t0 = 0;
        std::int64_t t1 = 0;
        std::int32_t lead = 0;
    };
    static_assert((kCapacity & (kCapacity - 1)) == 0, "kCapacity must be a power of two");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "SPSC 索引必须 lock-free");
    std::array<Rec, kCapacity> ring_{};
    std::atomic<std::uint32_t> writePos_{0};
    std::atomic<std::uint32_t> readPos_{0};
    std::atomic<std::uint32_t> dropped_{0};
};

// ---- state 编解码(LEAD chunk,docs/STATE_SCHEMA.md §三)----
// 载荷(小端):u16 minor | u16 reserved(0) | u32 runCount | runCount × { i64 t0 | i64 t1 | u32 lead }。
inline constexpr std::uint16_t kLeadChunkMinor = 1;
inline constexpr std::size_t kLeadChunkHeaderBytes = 8;
inline constexpr std::size_t kLeadChunkRecordBytes = 20;

enum class LeadDecodeStatus
{
    Ok,
    Malformed, // 长度/顺序/值域不合法 → 整块不用
    NewerMinor, // 更高 minor → 本构建不解,调用方原样保留字节
};

void encodeLeadChunk(const std::vector<LeadRun>& runs, std::vector<std::uint8_t>& out);
LeadDecodeStatus decodeLeadChunk(const std::uint8_t* data, std::size_t size, std::vector<LeadRun>& out);

} // namespace scvb::analysis
