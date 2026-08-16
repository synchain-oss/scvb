// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// S1 路由可靠性 spike —— 共享内存段布局(唯一真源镜像:constitution/ipc-contract-v0 v1.5 +
// 01-architecture §4.4 Output 全局信息小节)。J16:验证代码,本文件将来由 T06 平移为
// src/core/ipc/SegmentLayout.h(文件名与字段名保持不变,平移即移植)。
//
// 布局是宪法:任何字段偏移/尺寸改动都会让新旧进程半兼容互踩。以下 static_assert 把
// 「magic/abi 先行 + 每 slot 一条 cacheline + 跨进程字段 lock-free」钉死。
//
// 段名规则(ipc v1.5 [J66],组号 G 为构造参数,1..8):
//   registry : Local\SynchainSCVB.v1.g{G}.registry   (4 KB)
//   audio    : Local\SynchainSCVB.v1.g{G}.audio.ch{N} (N=1..15)
//   ctrl     : Local\SynchainSCVB.v1.g{G}.ctrl        (16 KB)

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace scvb
{

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

// 跨进程共享内存的 ABI 版本(与段名前缀 v1 绑定;布局改动 → abi+1 且段名升 v2)。
constexpr u32 kScvbAbi = 1;
constexpr u32 kMaxChannels = 15; // [J59] 10→15
constexpr u32 kMaxGroups = 8; // [J66]
// 宿主最大块帧数上界(1<<14 = 16384 帧):音频线程固定缓冲容量(Input capBuf_ / Output accum/trackBuf /
// BusXfade mix),全部构造时一次定容、prepareToPlay 不再分配;oversized 块按 ≤ 此值分块处理(v3 崩溃修复)。
constexpr int kMaxHostBlockFrames = 1 << 14;

// 'SCVB' 按内存字节序 'S','C','V','B'(小端主机上内存观感即 "SCVB")。两端同机同字节序,
// 用同一常量比对即可;此处不用实现定义的多字符字面量。
constexpr u32 makeFourCc(char c0, char c1, char c2, char c3) noexcept
{
    return static_cast<u32>(static_cast<unsigned char>(c0)) | (static_cast<u32>(static_cast<unsigned char>(c1)) << 8) |
           (static_cast<u32>(static_cast<unsigned char>(c2)) << 16) |
           (static_cast<u32>(static_cast<unsigned char>(c3)) << 24);
}
constexpr u32 kScvbMagic = makeFourCc('S', 'C', 'V', 'B');

// 等功率交叉曲线的公共常量(OutputStage.h / BusXfade.h 共用)。
inline constexpr double kHalfPi = 1.57079632679489661923;

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
    std::atomic<u32> flags; // 32  bit0 capturing / bit1 muted
    u32 _pad[7]; // 36..64
};

// OutputSlot(×1,同 cacheline 对齐以隔离假共享)。
struct alignas(64) OutputSlot
{
    std::atomic<u32> state; // 0   0=空闲 1=已声明 2=活跃(主实例)
    u32 pid; // 4
    std::atomic<u64> heartbeat_ms; // 8
    std::atomic<u32> connected_mask; // 16  Output 视角:哪些 channel 数据健康(bit{N-1})
    std::atomic<u32> config_seq; // 20  Output 配置版本号(spike 恒 0)
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
//   (01 §5.3「帧=channels 个 float」);时长目标 ≈8s @48k 不变,按 stereo 预算反推 2^k。
struct AudioRingHeader
{
    std::atomic<u32> magic; // 0   = kScvbMagic
    std::atomic<u32> abi; // 4   = kScvbAbi
    u32 sample_rate; // 8
    u32 ring_frames; // 12  帧数(2^k),不是样本数(见上)
    u32 channels; // 16  1=mono 2=stereo(prepareToPlay 写定,运行期不变)
    u32 _pad; // 20  (对齐 8)
    std::atomic<u64> write_head_samples; // 24 时间线绝对样本位置:下一帧将写到的 timeline pos
    std::atomic<u64> epoch; // 32 时间线跳变(定位/循环回跳)时 +1,读方丢弃跨代数据
};
static_assert(sizeof(AudioRingHeader) == 40);

// ---------------------------------------------------------------------------
// §4 ctrl 段 —— Output 全局信息小节(J09;spike 的 ctrl 段只含本小节,置于段首;
// 正式版置于广播区尾部,T06 落地时按 01 §4.4-a 摆放)。
// ---------------------------------------------------------------------------

enum OutputInfoFlag : u32
{
    kOutputEnabled = 1u << 0 // bit0: output_enabled
};

struct alignas(64) OutputGlobalInfo
{
    std::atomic<u32> capture_enabled; // 0   采集开关
    std::atomic<u32> output_sample_rate; // 4   Output 采样率
    std::atomic<u32> flags; // 8   bit0 output_enabled,其余保留
    u32 _pad; // 12
    std::atomic<u32> gap_count[15]; // 16..76   失准(缺口)计数导出
    std::atomic<u32> overlap_count[15]; // 76..136  重叠计数导出(保留,spike 恒 0)
    std::atomic<u64> epoch_summary[15]; // 136..256 各轨当前 epoch
};

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
static_assert(alignof(InputSlot) == 64);

// OutputSlot
static_assert(sizeof(OutputSlot) == 64, "OutputSlot 一条 cacheline");
static_assert(offsetof(OutputSlot, state) == 0);
static_assert(offsetof(OutputSlot, pid) == 4);
static_assert(offsetof(OutputSlot, heartbeat_ms) == 8);
static_assert(offsetof(OutputSlot, connected_mask) == 16);
static_assert(offsetof(OutputSlot, config_seq) == 20);

// AudioRingHeader
static_assert(offsetof(AudioRingHeader, magic) == 0);
static_assert(offsetof(AudioRingHeader, abi) == 4);
static_assert(offsetof(AudioRingHeader, sample_rate) == 8);
static_assert(offsetof(AudioRingHeader, ring_frames) == 12);
static_assert(offsetof(AudioRingHeader, channels) == 16);
static_assert(offsetof(AudioRingHeader, write_head_samples) == 24);
static_assert(offsetof(AudioRingHeader, epoch) == 32);

// OutputGlobalInfo
static_assert(sizeof(OutputGlobalInfo) == 256);
static_assert(offsetof(OutputGlobalInfo, capture_enabled) == 0);
static_assert(offsetof(OutputGlobalInfo, output_sample_rate) == 4);
static_assert(offsetof(OutputGlobalInfo, flags) == 8);
static_assert(offsetof(OutputGlobalInfo, gap_count) == 16);
static_assert(offsetof(OutputGlobalInfo, overlap_count) == 76);
static_assert(offsetof(OutputGlobalInfo, epoch_summary) == 136);

// 段预算(01 §5.3 / 契约 §1/§4)
static_assert(sizeof(RegistryHeader) + 15 * sizeof(InputSlot) + sizeof(OutputSlot) <= 4096, "registry 段必须 ≤ 4 KB");
static_assert(sizeof(OutputGlobalInfo) <= 16384, "ctrl 段必须 ≤ 16 KB");

} // namespace scvb
