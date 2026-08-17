// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// CtrlPlane —— ctrl 段(Output→Input 配置广播 + OutputGlobalInfo + Input→Output 命令环)+
// 停摆看门狗([R3/J52])。组内语义(ipc v1.5 [J66]):构造带 group(1..8),所有读写作用于本组
// g{G}.ctrl 段;跨组互不可见。只经消息线程;音频线程不触碰 ctrl 段。
//
// 命令环:每 slot 一条独立 SPSC(共 15 条,[J59]);生产者 = 该 Input 的 [M] 唯一(25Hz 排水),
// 消费者 = Output [M]。op 枚举直接复用 T06 SegmentLayout.h 的 CtrlOp(不得重定义),v1 只实现
// kNone/kSetPriority/kFpReport 三值记录流;value 是 u64([J46])。满环丢最旧 + 计数(10 IPC-13)。
// 字符串型 op 不进 v1(记录只有标量 value,需 abi+1 变长区)。
//
// 停摆看门狗(01 §4.2):[M] 25Hz 检测「自身 blockCounter 停滞 ≥0.5s ∧ 任一在线轨 write_head
// 仍推进」→ 清 connected_mask;blockCounter 恢复推进后按 §4.3-b 让位协议逐轨重置信位(不得硬切)。
// blockCounter 是 Output 进程内成员([A] fetch_add(relaxed)/[M] load(relaxed)),绝不放共享段;
// 本类用注入源(std::function<u64()>)接入,时间可注入(virtual clock),便于单测。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "ISegmentBackend.h"
#include "SegmentLayout.h"

namespace scvb
{

// ---------------------------------------------------------------------------
// ctrl 段布局(16 KB,契约 §4)。T06 只冻结了 OutputGlobalInfo/CtrlRecord 结构与 16KB 预算,
// 本卡定义 ctrl 段头与命令环布局(均在预算内,不改任何已冻结字段)。
// ---------------------------------------------------------------------------

// ctrl 段头(§4.0 registry/ctrl 通用,§5「各 header 前两字段 magic/abi」):magic/abi 先行 +
// generation(覆盖式重初始化 +1)。
struct alignas(64) CtrlHeader
{
    std::atomic<u32> magic; // 0   = kScvbMagic(初始化完成标志)
    std::atomic<u32> abi; // 4   = kScvbAbi
    std::atomic<u32> generation; // 8   覆盖式重初始化 +1
    u32 _pad; // 12
    u32 _reserved[12]; // 16..64 填满 cacheline 0
};
static_assert(sizeof(CtrlHeader) == 64, "CtrlHeader 占满 cacheline 0");
static_assert(offsetof(CtrlHeader, magic) == 0);
static_assert(offsetof(CtrlHeader, abi) == 4);
static_assert(offsetof(CtrlHeader, generation) == 8);
static_assert(alignof(CtrlHeader) == 64);

// 命令环每 slot 容量(2^k;满环丢最旧 + 计数,是安全阀,容量小即可)。
inline constexpr u32 kCtrlRingCapacity = 16;

// 命令环 SPSC(每 slot 一条)。生产者 = 该 Input 的 [M] 唯一,消费者 = Output [M]。
// write_pos/read_pos 单调递增(取模容量);满环 = write_pos - read_pos >= capacity,
// 写方覆盖最旧记录 + overflow_count++(丢最旧 + 计数 + UI 提示「设置未送达」,10 IPC-13)。
struct alignas(64) CtrlRing
{
    std::atomic<u32> write_pos; // 0   生产者写位置(单调递增)
    std::atomic<u32> read_pos; // 4   消费者读位置(单调递增)
    std::atomic<u32> overflow_count; // 8   满环丢最旧计数
    u32 _pad; // 12
    u32 _reserved[12]; // 16..64  SPSC 索引独占 cacheline 0(隔离 records 假共享)
    CtrlRecord records[kCtrlRingCapacity]; // 64..448(16×24=384)
};
static_assert(sizeof(CtrlRing) == 448, "CtrlRing = 64 头 + 16×24 记录");
static_assert(alignof(CtrlRing) == 64);
static_assert(offsetof(CtrlRing, records) == 64);

// ctrl 段预算与偏移(全部 64 字节对齐)。
inline constexpr std::size_t kCtrlSegmentSize = 16384;
inline constexpr std::size_t kCtrlHeaderBytes = sizeof(CtrlHeader); // 64
inline constexpr std::size_t kCtrlBroadcastOffset = 64; // 广播区起点(cacheline 0 之后)
inline constexpr std::size_t kCtrlBroadcastBytes = 9344; // 广播区占位(T25/params v2.x 填内容)
inline constexpr std::size_t kCtrlGlobalInfoOffset = 9408; // 64 对齐(64+9344)
inline constexpr std::size_t kCtrlRingsOffset = 9664; // 64 对齐(9408+256)
static_assert(kCtrlGlobalInfoOffset % 64 == 0);
static_assert(kCtrlRingsOffset % 64 == 0);
static_assert(kCtrlRingsOffset + kMaxChannels * sizeof(CtrlRing) == kCtrlSegmentSize,
              "ctrl 段 15 命令环须恰好占满预算");

// fp_report 载荷打包(J46):value = (u64(tile_idx) << 48) | (hash & 0x0000FFFFFFFFFFFF)。
// tile_idx 高 16 位(≈18.2 小时时间线上限的截断语义);fingerprint 截断低 48 位。
// 单条记录原子,不引入跨记录配对。
inline constexpr u64 kFpReportHashMask = 0x0000FFFFFFFFFFFFull;
inline constexpr u64 kFpReportTileIdxMask = 0xFFFFull;

// 打包:tile_idx > 0xFFFF 时钳制到 0xFFFF(明确定义:超上限截断 tile_idx,而非未定义行为)。
inline u64 packFpReport(u32 tileIdx, u64 hash) noexcept
{
    const u32 clamped = tileIdx > 0xFFFFu ? 0xFFFFu : tileIdx;
    return (static_cast<u64>(clamped) << 48) | (hash & kFpReportHashMask);
}
inline u32 unpackFpReportTileIdx(u64 value) noexcept
{
    return static_cast<u32>((value >> 48) & kFpReportTileIdxMask);
}
inline u64 unpackFpReportHash(u64 value) noexcept
{
    return value & kFpReportHashMask;
}

// 未知 op 判定(读方安全忽略:Output [M] 派发 switch 的 default 分支据此跳过未知 op)。
inline bool isKnownCtrlOp(CtrlOp op) noexcept
{
    return op == CtrlOp::kNone || op == CtrlOp::kSetPriority || op == CtrlOp::kFpReport;
}

// OutputGlobalInfo 快照(Output [M] 250ms 刷新 → Input/验证工具只读;J09)。
struct OutputGlobalInfoSnapshot
{
    u32 capture_enabled = 0;
    u32 output_sample_rate = 0;
    u32 flags = 0;
    u32 gap_count[kMaxChannels] = {};
    u32 overlap_count[kMaxChannels] = {};
    u64 epoch_summary[kMaxChannels] = {};
};

// 停摆看门狗动作(01 §4.2 [R3/J52])。
enum class WatchdogAction
{
    kNone, // 无动作
    kClearMask, // 停摆:清空 connected_mask(Inputs 按 J12 走 5s 滞回转直通)
    kReacquireBit // 让位协议(§4.3-b):逐轨按序重置信位(不得硬切);channel 字段生效
};

struct WatchdogResult
{
    WatchdogAction action = WatchdogAction::kNone;
    u32 channel = 0; // kReacquireBit 时的目标 channel(1..15)
};

class CtrlPlane
{
public:
    // backend + group(1..8)注入。Output 主实例写广播/全局信息、消费命令环;Input 实例
    // 读广播/全局信息、生产命令环。
    explicit CtrlPlane(ISegmentBackend& backend, u32 group);
    ~CtrlPlane();

    CtrlPlane(const CtrlPlane&) = delete;
    CtrlPlane& operator=(const CtrlPlane&) = delete;

    // 映射 + §4.0 初始化(消息线程)。
    InitResult open();

    bool isOpen() const { return base_ != nullptr; }
    u32 group() const { return group_; }
    u32 generation() const;

    // ---- OutputGlobalInfo(J09)----
    // Output [M] 每 250ms 刷新;Input/验证工具只读。
    void refreshGlobalInfo(const OutputGlobalInfoSnapshot& s);
    OutputGlobalInfoSnapshot readGlobalInfo() const;

    // ---- 命令环(J36/J46)----
    // Input [M] 生产(每 slot 一条 SPSC);满环丢最旧 + overflow_count++。seq 由本实例内部
    // 单调递增计数器维护(单生产者 [M])。返回 false = channel 非法/未打开。
    bool enqueue(u32 channel, CtrlOp op, u64 value);
    // Output [M] 消费;无记录返回 false。
    bool dequeue(u32 channel, CtrlRecord& out);
    u32 overflowCount(u32 channel) const;

    // ---- 停摆看门狗([R3/J52];Output [M] 25Hz)----
    // 注入源:blockCounter([A] fetch_add relaxed 的进程内成员)、各轨 write_head、
    // 当前 connected_mask(读 registry OutputSlot)。
    void setBlockCounterSource(std::function<u64()> source) { blockCounterSource_ = std::move(source); }
    void setWriteHeadSource(std::function<u64(u32 channel)> source) { writeHeadSource_ = std::move(source); }
    void setConnectedMaskSource(std::function<u32()> source) { connectedMaskSource_ = std::move(source); }
    WatchdogResult tickWatchdog(u64 nowMs);

    // ---- 广播区占位(T25/params v2.x;本卡不发明布局)----
    void* broadcastBase() const { return base_; }
    static constexpr std::size_t broadcastBytes() { return kCtrlBroadcastBytes; }

private:
    CtrlRing* ringAt(u32 channel) const;
    OutputGlobalInfo* globalInfo() const;
    bool anyOnlineWriteHeadAdvanced() const;
    void updateWriteHeadBaseline();
    static u32 lowestOnlineChannel(u32 mask);
    static u32 nextOnlineChannel(u32 mask, u32 cur);

    ISegmentBackend& backend_;
    u32 group_ = 1;
    SegmentHandle handle_; // 引用计数句柄:析构经 release() 握手释放(消息线程)
    unsigned char* base_ = nullptr;

    // 命令环生产者 seq 计数(Input [M] 单线程)。
    u32 nextSeq_ = 0;

    // 停摆看门狗状态。
    std::function<u64()> blockCounterSource_;
    std::function<u64(u32 channel)> writeHeadSource_;
    std::function<u32()> connectedMaskSource_;
    bool watchdogInit_ = false;
    u64 lastBlockValue_ = 0;
    u64 lastBlockAdvanceMs_ = 0;
    bool tripped_ = false;
    u32 onlineMaskAtTrip_ = 0; // 停摆时快照的在线轨集合(bit{N-1}),恢复时逐轨重置信位
    u32 reacquireNext_ = 0; // 下一个待重置信位的 channel(1..15;0 = 无)
    u64 lastReacquireMs_ = 0;
    u64 lastWriteHead_[kMaxChannels] = {};
};

} // namespace scvb
