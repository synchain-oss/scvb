// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SegmentLayout —— SCVB 共享内存段布局的唯一真源镜像(constitution/ipc-contract-v0 v1.5 +
// 01-architecture §4.4-a)。T06 自 spikes/s1/SegmentLayout.h 平移为正式生产头,平移即移植:
// 文件名、字段名与布局逐字节保持 S1 已验证的路由语义,只做「正式契约 + 加固」。
//
// 布局是宪法:任何字段偏移/尺寸改动都会让新旧进程半兼容互踩(ipc §5:改动 → abi+1 且段名 v2)。
// 下方 static_assert 把「magic/abi 先行 + 每 slot 一条 cacheline + 跨进程字段 lock-free」钉死,
// tests/golden/ipc-layout.txt + IpcLayoutFreeze 再用运行时断言逐行对拍。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace scvb
{

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// 跨进程共享内存的 ABI 版本(与段名前缀 v1 绑定;布局改动 → abi+1 且段名升 v2)。
inline constexpr u32 kScvbAbi = 1;
inline constexpr u32 kMaxChannels = 15; // [J59] InputSlot 10→15;audio/feat 段名 ch{N} N=1..15
inline constexpr u32 kMaxGroups = 8; // [J66] 段名带组号 g{G},G=1..8

// 音频环默认帧数(01 §5.3 / S1 收口定值):1<<19 = 524288 帧。mono 2MB / stereo 4MB,时长 ≈10.9s @48k。
inline constexpr u32 kDefaultRingFrames = 1u << 19;

// 特征环容量(01 §5.3):1<<17 = 131072 hop;10ms hop ≈ 21.8 分钟(≈20 分钟档,契约 §3)。
inline constexpr u32 kFeatCapacityHops = 1u << 17;
inline constexpr u32 kFeatHopMs = 10;

// 'SCVB' 按内存字节序 'S','C','V','B'(小端主机上内存观感即 "SCVB")。两端同机同字节序,
// 用同一常量比对即可;此处不用实现定义的多字符字面量。数值 = 0x42564353(与 S1 spike 的
// kScvbMagic 完全一致,保持对 S1 已验证部署与诊断工具链的二进制兼容)。
constexpr u32 makeFourCc(char c0, char c1, char c2, char c3) noexcept
{
    return static_cast<u32>(static_cast<unsigned char>(c0)) | (static_cast<u32>(static_cast<unsigned char>(c1)) << 8) |
           (static_cast<u32>(static_cast<unsigned char>(c2)) << 16) |
           (static_cast<u32>(static_cast<unsigned char>(c3)) << 24);
}
inline constexpr u32 kScvbMagic = makeFourCc('S', 'C', 'V', 'B'); // 0x42564353(段内前 4 字节字面拼出 "SCVB")

// ---------------------------------------------------------------------------
// §1 Registry 段结构体
// ---------------------------------------------------------------------------

// InputSlot.state 取值。
enum InputSlotState : u32
{
    kSlotFree = 0, // 空闲
    kSlotClaimed = 1, // 已声明(claim 中,瞬时)
    kSlotActive = 2 // 活跃
};

// InputSlot.flags 位定义(ipc v1.3 [J48]:bit0 [A] 写、bit1 [M] 写,一律 fetch_or/fetch_and)。
enum InputSlotFlag : u32
{
    kFlagCapturing = 1u << 0, // 采集开关 ON(capturing)
    kFlagMuted = 1u << 1 // 已完成 直通→静音 ramp 的回执(J32)
};

// cacheline 0(64 字节)。magic/abi 为 atomic<u32>(ipc §0「所有跨进程字段 std::atomic」,
// [J48] 先例:尺寸/偏移不变,非布局改动)——init 协议对 magic 做 CAS/release-store,必须原子。
struct alignas(64) RegistryHeader
{
    std::atomic<u32> magic; // 0   = kScvbMagic(初始化完成标志,release-store 后行)
    std::atomic<u32> abi; // 4   = kScvbAbi
    std::atomic<u32> generation; // 8   每次覆盖式重初始化 +1
    u32 _pad; // 12
    u32 _reserved[12]; // 16..64 填满 cacheline 0
};

// 每 slot 一条 cacheline(64 字节),×15。
struct alignas(64) InputSlot
{
    std::atomic<u32> state; // 0   0=空闲 1=已声明 2=活跃
    u32 pid; // 4
    u32 sample_rate; // 8
    u32 max_block; // 12
    std::atomic<u64> heartbeat_ms; // 16  稳态时钟毫秒(每 ~250ms 刷新)
    std::atomic<u64> capture_write_pos; // 24  特征段已写到的时间线位置(样本)
    std::atomic<u32> flags; // 32  bit0 capturing / bit1 muted([J48] atomic<u32>)
    u32 _pad[7]; // 36..64
};

// OutputSlot(×1,同 cacheline 对齐以隔离假共享)。
struct alignas(64) OutputSlot
{
    std::atomic<u32> state; // 0   0=空闲 1=已声明 2=活跃(主实例)
    u32 pid; // 4
    std::atomic<u64> heartbeat_ms; // 8
    std::atomic<u32> connected_mask; // 16  Output 视角:哪些 channel 数据健康(bit{N-1})
    std::atomic<u32> config_seq; // 20  Output 配置版本号
    u32 _pad[10]; // 24..64
};

// ---------------------------------------------------------------------------
// §2 音频环段结构体(每 channel 一个)
// ---------------------------------------------------------------------------

// ring_frames 口径(一次性定死,读写两侧必须一致,否则 L/R 错位静默 bug):
//   ring_frames = 环内「帧」数(帧 = 一个时间线样本位置 = channels 个 interleaved float)。
//   —— 不是「帧对数」,也不是「样本数」。样本数 = ring_frames * channels。
//   环的 float 容量 = ring_frames * channels;地址 = ((timeline_pos & (ring_frames-1)) * channels + c)。
//   mono(channels=1)与 stereo(channels=2)用同一 ring_frames 值 → 帧数(时长)相同、字节数 ×2
//   (01 §5.3「帧=channels 个 float」;段恒按 stereo 容量创建,创建后读回真实段大小)。
struct AudioRingHeader
{
    std::atomic<u32> magic; // 0   = kScvbMagic
    std::atomic<u32> abi; // 4   = kScvbAbi
    u32 sample_rate; // 8
    u32 ring_frames; // 12  帧数(2^k),不是样本数(见上)
    u32 channels; // 16  1=mono 2=stereo(prepareToPlay 写定,运行期不变;[J57])
    u32 _pad; // 20  (对齐 8)
    std::atomic<u64> write_head_samples; // 24 时间线绝对样本位置:下一帧将写到的 timeline pos
    std::atomic<u64> epoch; // 32 时间线跳变(定位/循环回跳)时 +1,读方丢弃跨代数据
};

// ---------------------------------------------------------------------------
// §3 特征段结构体(每 channel 一个)
// ---------------------------------------------------------------------------

struct FeatHeader
{
    std::atomic<u32> magic; // 0   = kScvbMagic
    std::atomic<u32> abi; // 4   = kScvbAbi
    u32 hop_ms; // 8   = kFeatHopMs(10)
    u32 capacity_hops; // 12  环容量(hop 数,≈20 分钟)
    std::atomic<u64> base_hop; // 16  环起始对应的时间线 hop 序号
    std::atomic<u64> write_hop; // 24  已写到的 hop 序号(时间线寻址)
};

// FeatFrame:K-weighted mean-square + peak([J57] BS.1770 多通道求和口径)。
struct FeatFrame
{
    float kw_ms; // 0
    float peak; // 4
};

// ---------------------------------------------------------------------------
// §4 ctrl 段 —— Output 全局信息小节(J09;01 §4.4-a 逐字一致)
// ---------------------------------------------------------------------------

enum OutputInfoFlag : u32
{
    kOutputEnabled = 1u << 0 // bit0: output_enabled
};

struct alignas(64) OutputGlobalInfo
{
    std::atomic<u32> capture_enabled; // 0   采集开关(Input「已布防」显示依据)
    std::atomic<u32> output_sample_rate; // 4   Output 采样率
    std::atomic<u32> flags; // 8   bit0 output_enabled,其余保留
    u32 _pad; // 12
    std::atomic<u32> gap_count[15]; // 16..76   失准(缺口)计数导出([J59] 15 轨)
    std::atomic<u32> overlap_count[15]; // 76..136  重叠计数导出
    std::atomic<u64> epoch_summary[15]; // 136..256 各轨当前 epoch
};

// ---------------------------------------------------------------------------
// §4 ctrl 命令环记录(契约 v1.2 [J46] value 定型 u64;op 枚举语义归 T07)
// ---------------------------------------------------------------------------

// CtrlOp 枚举值在此定义以支撑 CtrlRecord 的冻结布局(abi=1;op 占 4 字节、value 占 8 字节)。
// 枚举的底层类型断言、三值与 01 §4.4-c 表逐项核对、以及命令环满环/派发语义归 T07;
// T07 直接复用本枚举,不得重定义。
enum class CtrlOp : u32
{
    kNone = 0,
    kSetPriority = 1,
    kFpReport = 2
};

// 命令环记录(每 slot 一条 SPSC,15 条)。[J46]:value 是 u64 不是 u32 ——
// kFpReport 载荷按 value = (u64(tile_idx) << 48) | (hash & 0x0000FFFFFFFFFFFF) 打包
// (tile_idx 高 16 位、fingerprint 截断低 48 位),单条记录原子;生产者 = Input [M] 25Hz 排水。
//
// [J48 先例] 四字段声明为 atomic(seq/channel/op/value),尺寸/偏移不变(非布局改动;golden 不变):
// 命令环是单写单读 SPSC,记录 24 字节非原子时,生产者满环覆写与消费者读同一条记录会撕裂读;
// atomic 化让 seq 可作「在写标记/一致性快照」锚点(见 CtrlPlane::enqueue/dequeue 的 seq 协议)。
struct CtrlRecord
{
    std::atomic<u32> seq; // 0   0 = 记录在写(生产者先写 0 标记),非 0 = 已提交
    std::atomic<u32> channel; // 4
    std::atomic<u32> op; // 8   底层存储 CtrlOp 的 u32 值
    std::atomic<u64> value; // 16
};

// ---------------------------------------------------------------------------
// 段名生成(ipc v1.5 [J66]:全部段名带组号 g{G},G=1..8;逻辑段名不带 OS 前缀,由 backend 加 "Local\")
// ---------------------------------------------------------------------------

enum class SegmentKind
{
    kRegistry,
    kAudio,
    kFeat,
    kCtrl
};

// 逻辑段名(不带 OS 前缀):"SynchainSCVB.v1.g{G}.{registry|audio.ch{N}|feat.ch{N}|ctrl}"。
// G ∈ 1..8;N ∈ 1..15(仅 audio/feat 用 channel)。mac 长度复核:最长
// "/SynchainSCVB.v1.g8.audio.ch15" = 30 字符 ≤ 31(PSHMNAMLEN)。不允许存在无组号的段名常量。
inline std::wstring segmentLogicalName(u32 group, SegmentKind kind, u32 channel = 0)
{
    std::wstring name = L"SynchainSCVB.v1.g" + std::to_wstring(group) + L".";
    switch (kind)
    {
    case SegmentKind::kRegistry:
        name += L"registry";
        break;
    case SegmentKind::kAudio:
        name += L"audio.ch" + std::to_wstring(channel);
        break;
    case SegmentKind::kFeat:
        name += L"feat.ch" + std::to_wstring(channel);
        break;
    case SegmentKind::kCtrl:
        name += L"ctrl";
        break;
    }
    return name;
}

// ---------------------------------------------------------------------------
// blockCounter 归属([R3/J52];仅注释,不进任何结构体):
//   std::atomic<u64> blockCounter 是 Output 进程内成员变量([A] 每块 fetch_add(1, relaxed)、
//   [M] load(relaxed)),用于 [M] 25Hz 停摆看门狗(01 §4.2/§5.2)。它绝不放 RegistryHeader /
//   OutputSlot / ctrl 全局小节,也绝不进 tests/golden/ipc-layout.txt —— 一旦进段即构成布局改动
//   (ipc §5:abi+1 且段名 v2),而看门狗不需要跨进程可见(判据另一半 write_head 已在段里)。
//   看门狗本体归 T07。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 布局静态断言(布局是宪法)
// ---------------------------------------------------------------------------

// 跨进程字段必须 lock-free(否则 std::atomic 内部可能带锁,共享内存里放锁即 UB/死锁)。
static_assert(std::atomic<u32>::is_always_lock_free, "atomic<u32> 必须 lock-free");
static_assert(std::atomic<u64>::is_always_lock_free, "atomic<u64> 必须 lock-free");

// RegistryHeader
static_assert(sizeof(RegistryHeader) == 64, "RegistryHeader 必须占满 cacheline 0");
static_assert(offsetof(RegistryHeader, magic) == 0);
static_assert(offsetof(RegistryHeader, abi) == 4);
static_assert(offsetof(RegistryHeader, generation) == 8);
static_assert(offsetof(RegistryHeader, _pad) == 12);
static_assert(offsetof(RegistryHeader, _reserved) == 16);
static_assert(alignof(RegistryHeader) == 64);

// InputSlot
static_assert(sizeof(InputSlot) == 64, "InputSlot 必须一条 cacheline");
static_assert(offsetof(InputSlot, state) == 0);
static_assert(offsetof(InputSlot, pid) == 4);
static_assert(offsetof(InputSlot, sample_rate) == 8);
static_assert(offsetof(InputSlot, max_block) == 12);
static_assert(offsetof(InputSlot, heartbeat_ms) == 16);
static_assert(offsetof(InputSlot, capture_write_pos) == 24);
static_assert(offsetof(InputSlot, flags) == 32);
static_assert(offsetof(InputSlot, _pad) == 36);
static_assert(alignof(InputSlot) == 64);

// OutputSlot
static_assert(sizeof(OutputSlot) == 64, "OutputSlot 必须一条 cacheline");
static_assert(offsetof(OutputSlot, state) == 0);
static_assert(offsetof(OutputSlot, pid) == 4);
static_assert(offsetof(OutputSlot, heartbeat_ms) == 8);
static_assert(offsetof(OutputSlot, connected_mask) == 16);
static_assert(offsetof(OutputSlot, config_seq) == 20);
static_assert(offsetof(OutputSlot, _pad) == 24);
static_assert(alignof(OutputSlot) == 64);

// AudioRingHeader
static_assert(sizeof(AudioRingHeader) == 40, "AudioRingHeader 必须 40 字节");
static_assert(offsetof(AudioRingHeader, magic) == 0);
static_assert(offsetof(AudioRingHeader, abi) == 4);
static_assert(offsetof(AudioRingHeader, sample_rate) == 8);
static_assert(offsetof(AudioRingHeader, ring_frames) == 12);
static_assert(offsetof(AudioRingHeader, channels) == 16);
static_assert(offsetof(AudioRingHeader, _pad) == 20);
static_assert(offsetof(AudioRingHeader, write_head_samples) == 24);
static_assert(offsetof(AudioRingHeader, epoch) == 32);

// FeatHeader
static_assert(sizeof(FeatHeader) == 32, "FeatHeader 必须 32 字节");
static_assert(offsetof(FeatHeader, magic) == 0);
static_assert(offsetof(FeatHeader, abi) == 4);
static_assert(offsetof(FeatHeader, hop_ms) == 8);
static_assert(offsetof(FeatHeader, capacity_hops) == 12);
static_assert(offsetof(FeatHeader, base_hop) == 16);
static_assert(offsetof(FeatHeader, write_hop) == 24);

// FeatFrame
static_assert(sizeof(FeatFrame) == 8, "FeatFrame 必须 8 字节");
static_assert(offsetof(FeatFrame, kw_ms) == 0);
static_assert(offsetof(FeatFrame, peak) == 4);

// OutputGlobalInfo
static_assert(sizeof(OutputGlobalInfo) == 256, "OutputGlobalInfo 必须 256 字节");
static_assert(offsetof(OutputGlobalInfo, capture_enabled) == 0);
static_assert(offsetof(OutputGlobalInfo, output_sample_rate) == 4);
static_assert(offsetof(OutputGlobalInfo, flags) == 8);
static_assert(offsetof(OutputGlobalInfo, _pad) == 12);
static_assert(offsetof(OutputGlobalInfo, gap_count) == 16);
static_assert(offsetof(OutputGlobalInfo, overlap_count) == 76);
static_assert(offsetof(OutputGlobalInfo, epoch_summary) == 136);
static_assert(alignof(OutputGlobalInfo) == 64);

// CtrlRecord([J46]:value 定型 u64;四字段 atomic 化,尺寸/偏移不变)
static_assert(sizeof(CtrlRecord) == 24, "CtrlRecord 必须 24 字节");
static_assert(offsetof(CtrlRecord, seq) == 0);
static_assert(offsetof(CtrlRecord, channel) == 4);
static_assert(offsetof(CtrlRecord, op) == 8);
static_assert(offsetof(CtrlRecord, value) == 16);
static_assert(decltype(CtrlRecord::seq)::is_always_lock_free, "CtrlRecord.seq 必须 lock-free");
static_assert(decltype(CtrlRecord::channel)::is_always_lock_free, "CtrlRecord.channel 必须 lock-free");
static_assert(decltype(CtrlRecord::op)::is_always_lock_free, "CtrlRecord.op 必须 lock-free");
static_assert(decltype(CtrlRecord::value)::is_always_lock_free, "CtrlRecord.value 必须 lock-free");

// 段预算(01 §5.3 / 契约 §1/§4)
static_assert(sizeof(RegistryHeader) + 15 * sizeof(InputSlot) + sizeof(OutputSlot) <= 4096, "registry 段必须 ≤ 4 KB");
static_assert(sizeof(OutputGlobalInfo) <= 16384, "ctrl 段必须 ≤ 16 KB");

} // namespace scvb
