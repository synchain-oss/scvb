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
#include <vector>

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

// ---------------------------------------------------------------------------
// 广播区(Output→Input 配置只读镜像,契约 §4.3 / ADR-004:配置真源在 Output)。
// 写方 = 本组主 Output 的 [M];读方 = 各 Input 的 [M]。跨进程 seqlock(奇偶协议),读方撕裂即
// 沿用上帧、不自旋 —— 与进程内的 engine::PlayheadShot 同款,只是载荷落在共享段里。
//
// 本区在 T06 冻结时只留了「偏移 64、9344 字节」的预算,内容留白(注释写「T25/params v2.x 填」),
// 于是 Input 侧的 label/priority/lead_lock/pair_id/freeze 一直是 InputBridgeLogic 里的硬编码
// 常量,Output 改什么 Input 都看不见(T37 三轮 C 族「远程只读视图整条断」)。下面的结构就落在
// 那段预算内,不改任何已冻结字段、不越界。
// ---------------------------------------------------------------------------

// 每轨配置镜像(32 字节)。字段与 §2.1 scvb.state.channels[] 一一对位。
struct CtrlChannelConfig
{
    u32 priority; // 0   0..10
    u32 flags; // 4   位定义见下 kCfgFlag*
    u32 pair_id; // 8   0=无配对,1..7
    u32 freeze; // 12  bit0=pan 冻结,bit1=vol 冻结(J65 同一参数两位)
    u32 source_channels; // 16  1|2(Input 实测值,Output 回镜像给全组)
    u32 _reserved[3]; // 20..32
};
static_assert(sizeof(CtrlChannelConfig) == 32, "CtrlChannelConfig 定长 32 字节");

enum CtrlConfigFlag : u32
{
    kCfgFlagEnabled = 1u << 0,
    kCfgFlagLeadLock = 1u << 1,
    kCfgFlagLeadVolExempt = 1u << 2,
    kCfgFlagParticipateAutoPan = 1u << 3,
};

// label 定长字节槽:契约限 ≤24 **字符**,UTF-8 最坏 4 字节/字符 → 96 字节封顶 + 1 位 NUL 余量,
// 取 100 对齐到 4。写方按字节截断到最后一个完整 UTF-8 序列边界(不产生半个码点)。
inline constexpr std::size_t kCtrlLabelBytes = 100;

// 广播区总布局:64 字节头(独占 cacheline 0,seq 与载荷分离)+ 15 轨配置 + 15 条 label。
struct alignas(64) CtrlBroadcast
{
    std::atomic<u32> seq; // 0   seqlock:写前 +1(奇)→ 写载荷 → 写后 +1(偶)
    std::atomic<u32> config_seq; // 4   广播区整体版本号(任一字段变化 +1;Input 据此判「有新配置」)
    u32 lead_select; // 8   0=无,1..15(全局 lead 选择,§2.1)
    u32 _pad; // 12
    u32 _reserved[12]; // 16..64
    CtrlChannelConfig channels[kMaxChannels]; // 64..544(15×32)
    char labels[kMaxChannels][kCtrlLabelBytes]; // 544..2044(15×100,UTF-8 NUL 结尾)
    char _tail[4]; // 2044..2048:显式补齐到 64 的整数倍(与 CtrlHeader/CtrlRing 同惯例)
};
static_assert(offsetof(CtrlBroadcast, channels) == 64, "广播区头独占 cacheline 0");
static_assert(alignof(CtrlBroadcast) == 64);
// 尺寸写死:alignas(64) 的结构若不是 64 整数倍,编译器会隐式补齐(MSVC /W4 C4324),
// 而隐式补齐意味着布局取决于编译器 —— 共享内存结构不接受这种不确定性。
static_assert(sizeof(CtrlBroadcast) == 2048, "CtrlBroadcast 须恰好 32 个 cacheline(无隐式填充)");

// 读写用的普通 POD 快照(无原子,可自由拷贝;与 OutputGlobalInfoSnapshot 同款 API 形状)。
struct CtrlBroadcastSnapshot
{
    u32 config_seq = 0;
    u32 lead_select = 0;
    CtrlChannelConfig channels[kMaxChannels] = {};
    char labels[kMaxChannels][kCtrlLabelBytes] = {};
};

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
inline constexpr std::size_t kCtrlBroadcastBytes = 9344; // 广播区预算(CtrlBroadcast 落在其内,尚有余量)
inline constexpr std::size_t kCtrlGlobalInfoOffset = 9408; // 64 对齐(64+9344)
inline constexpr std::size_t kCtrlRingsOffset = 9664; // 64 对齐(9408+256)
static_assert(kCtrlGlobalInfoOffset % 64 == 0);
static_assert(kCtrlRingsOffset % 64 == 0);
static_assert(kCtrlRingsOffset + kMaxChannels * sizeof(CtrlRing) == kCtrlSegmentSize,
              "ctrl 段 15 命令环须恰好占满预算");
static_assert(sizeof(CtrlBroadcast) <= kCtrlBroadcastBytes, "CtrlBroadcast 不得越过广播区预算");

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

    // 映射 + §4.0 初始化(消息线程)。allowOverwrite 恒 true(create-or-open 双方 owner 语义):
    // Input 写命令环、Output 写广播/全局信息,双方都是本组 ctrl 段的合法创建/覆盖者(与 Registry
    // 同口径);真正的只读 attach(allowOverwrite=false)属 T20/T23/T24 的 openExisting(audio/feat 环)
    // 路径,非本卡。故不设角色参数。
    InitResult open();

    bool isOpen() const { return base_ != nullptr; }
    u32 group() const { return group_; }
    u32 generation() const;

    // 满环判定(写方 [M] 在 enqueue 前调用;SPSC 纪律:生产者只读 read_pos,read_pos 仅消费者写)。
    // remoteSetPriority(§3.4)据此回 {queued:false, reason:"ringFull"}(enqueue 仍执行:丢最旧 + 计数)。
    bool isRingFull(u32 channel) const;

    // 改组(J66):释放本组 ctrl 段句柄 → 换新组 → 重新映射 + §4.0 初始化。返回 kOk / kAbiMismatch /
    // kFailed。与 Registry::changeGroup 同构(Output 改组时 registry 与 ctrl 同时换组)。
    InitResult changeGroup(u32 newGroup);

    // 释放本组 ctrl 段句柄(消息线程;channel_id=0 未分配/释放时调用,保持「channel_id=0 不建段」
    // 口径,PR#54 R9)。与 changeGroup 释放旧句柄同构:租约在途则压入 pendingReleases_ 延迟回收。
    void release();

    // 延迟释放回收([M] 心跳/轮询周期调用,文档注明调用点):对 pendingReleases_ 逐项 release(nowMs),
    // 成功(租约归零且宽限期届满)即移除并解映射。pr-agent 复审:open()/重开时 release() 返回 false
    // 的旧句柄压入 pendingReleases_,否则旧 mapping 无人再 release → 永久泄漏。
    void reapPendingReleases(u64 nowMs);
    std::size_t pendingReleaseCount() const { return pendingReleases_.size(); }

    // ---- OutputGlobalInfo(J09)----
    // Output [M] 每 250ms 刷新;Input/验证工具只读。
    void refreshGlobalInfo(const OutputGlobalInfoSnapshot& s);
    OutputGlobalInfoSnapshot readGlobalInfo() const;

    // ---- 命令环(J36/J46)----
    // Input [M] 生产(每 slot 一条 SPSC);满环丢最旧 + overflow_count++。seq = 共享 write_pos 派生
    // (w+1),重启安全(见 CtrlPlane.cpp enqueue)。返回 false = channel 非法/未打开。
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

    // ---- 广播区(Output→Input 配置只读镜像,契约 §4.3)----
    // Output [M] 写(seqlock 奇偶);值变化才调用,不必每拍写。未打开则静默不写。
    void writeBroadcast(const CtrlBroadcastSnapshot& s);
    // Input [M] 读。返回 false = 段未打开 / 本次读撕裂(调用方沿用上帧,不自旋)。
    bool readBroadcast(CtrlBroadcastSnapshot& out) const;

    // 广播区首字节(**含 kCtrlBroadcastOffset 偏移**)。原先返回 base_,即段起点 —— 那是
    // CtrlHeader(magic/abi/generation)的地址,任何按 broadcastBytes() 写入都会砸掉段头
    // 并越界踩进 OutputGlobalInfo。当时无调用方所以没暴露出来。
    void* broadcastBase() const { return base_ != nullptr ? base_ + kCtrlBroadcastOffset : nullptr; }
    static constexpr std::size_t broadcastBytes() { return kCtrlBroadcastBytes; }

private:
    CtrlRing* ringAt(u32 channel) const;
    OutputGlobalInfo* globalInfo() const;
    CtrlBroadcast* broadcast() const;
    bool anyOnlineWriteHeadAdvanced() const;
    void updateWriteHeadBaseline();
    static u32 lowestOnlineChannel(u32 mask);
    static u32 nextOnlineChannel(u32 mask, u32 cur);

    ISegmentBackend& backend_;
    u32 group_ = 1;
    SegmentHandle handle_; // 引用计数句柄:析构经 release() 握手释放(消息线程)
    std::vector<SegmentHandle> pendingReleases_; // 延迟释放(租约在途)的旧句柄,[M] reapPendingReleases 回收
    unsigned char* base_ = nullptr;

    // 命令环记录 seq 由共享 write_pos 派生(seq = w+1),不持进程内计数器——多 channel 各自按
    // 本环 write_pos 派生;生产者崩溃/重启后新实例 w 续用共享 write_pos,seq 不中断、环不卡死。

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
