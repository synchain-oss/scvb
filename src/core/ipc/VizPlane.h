// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// VizPlane —— viz 段(`SynchainSCVB.v1.g{G}.viz`)的布局真源 + 读写门面([T44/J75])。
//
// 定位:**只读监视数据面**。唯一写方 = Output 的消息线程 [M](4Hz 低频发布);唯一读方 =
// SCVB Monitor 的消息线程(只读 attach,FILE_MAP_READ,永不写)。`processBlock`(音频线程 [A])
// **绝不触碰本段任何字节** —— 这是本卡的硬铁律,由 tests/core/test_viz_plane.cpp 的零写入断言
// 与 05 J75 节 C 共同约束。段内无 ctrl 写权、无命令环、无握手,读方掉线/不存在对写方零影响。
//
// abi 策略(挂总 abi,见 docs/contract-changes/20260825-viz-segment.md):
//   VizHeader 前两字段 magic/abi 与既有各段同构,abi 恒 = kScvbAbi。「新增一个段」本身不改任何
//   既有段的布局,故**不触发 abi+1**(旧版 Output 不建 viz 段 → Monitor attach 得 kFailed → 空态,
//   降级优雅);viz 段**自身**将来的布局改动,与其余各段同规:abi+1 且段名 v2(ipc §5)。
//   不设独立 viz_abi —— 独立版本号会造出「registry 认、viz 不认」的半兼容态,正是 J40 要禁的。
//
// 一致性:整帧 seqlock(seq 偶=稳定/奇=写入中)。写方单线程,读方可多,读侧无锁一致读、
// 撕裂即重试(上限 kVizReadRetries),超限返回 false 由调用方沿用上帧 —— 与进程内的
// scvb::engine::PlayheadShot 同款协议。区别:本段跨进程,故载荷字段一律 std::atomic
// (ipc §0「所有跨进程字段 std::atomic」),relaxed 存取在 x86 上即普通 mov,无额外开销。
//
// 降采样口径(冻结语义,读写两侧必须一致):
//   - 窗口 = [window_start_samples, window_start_samples + window_span_samples),均分 column_count 列;
//     第 i 列覆盖 [start + i*span/N, start + (i+1)*span/N)。
//   - 车道值 pan[t][i] = 该轨曲线在**列中心时刻**的求值(点采样,不是列内均值),定点
//     int16 = round(clamp(pan, -100, +100) * kVizPanScale);哨兵 kVizPanNone 表示该轨整条无数据。
//   - 覆盖位图 coverage[t][i] = 该列时间区间与该轨任一 CRVS 分段**有交集**即置 1(保守口径:
//     短于一列的分段不会消失)。断线渲染以位图为准 —— 曲线求值本身会填补空隙(CurveEvaluator
//     的 hold/外推语义),只有分段表才是覆盖真源。

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "ISegmentBackend.h"
#include "SegmentLayout.h"

namespace scvb
{

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

// 降采样列数(固定;改动 = 布局改动 → abi+1 且段名 v2)。
inline constexpr u32 kVizColumns = 1024;
inline constexpr u32 kVizCoverageWords = kVizColumns / 32; // 32
static_assert(kVizColumns % 32 == 0, "列数须 32 整除,位图才无残位");

// pan 定点标度:engineering pan ∈ [-100, +100] × 100 → int16 ∈ [-10000, +10000]。
inline constexpr std::int32_t kVizPanScale = 100;
inline constexpr std::int16_t kVizPanMax = static_cast<std::int16_t>(100 * kVizPanScale);
// 车道 / 当前值哨兵:该轨无数据(无分段 / 曲线缺失 / 轨未启用)。取 INT16_MIN,不与任何合法值撞。
inline constexpr std::int16_t kVizPanNone = -32768;

// volDb(−24..+12 dB)与 widthPct(0..100)共用 kVizPanScale 定点标度。
inline constexpr double kVizVolDbMin = -24.0;
inline constexpr double kVizVolDbMax = 12.0;
inline constexpr double kVizWidthMin = 0.0;
inline constexpr double kVizWidthMax = 100.0;

// 轨名:每轨 32 字节 UTF-8(8 个 u32 字),NUL 补齐;超长按 **UTF-8 字符边界**截断
// —— 绝不切出半个多字节序列(半个汉字到了 web 侧就是一个替换字符)。
inline constexpr u32 kVizLabelWords = 8;
inline constexpr u32 kVizLabelBytes = kVizLabelWords * 4; // 32

// playhead flags(与 scvb::engine::PlayheadFlag 的子集同义,但本段自持定义 —— 段布局不依赖
// 进程内头文件的枚举值)。
enum VizPlayheadFlag : u32
{
    kVizPlaying = 1u << 0, // 宿主正在播放
    kVizLooping = 1u << 1, // 宿主循环开启
    kVizLoopValid = 1u << 2 // loop_start/end_samples 有效(已由 ppq+bpm 换算成功)
};

// 读侧 seqlock 重试上限(超限 = 写方正高频重写或段被重初始化;沿用上帧)。
inline constexpr int kVizReadRetries = 8;

// ---------------------------------------------------------------------------
// 段结构体
// ---------------------------------------------------------------------------

// cacheline 0:magic/abi 先行(ipc §5)+ generation + 几何字段(magic 发布前写定,读方据此
// 校验自己编译期的 kVizColumns/kMaxChannels/kVizPanScale 与段内一致)。
struct alignas(64) VizHeader
{
    std::atomic<u32> magic; // 0   = kScvbMagic(初始化完成标志,release-store 后行)
    std::atomic<u32> abi; // 4   = kScvbAbi(挂总 abi)
    std::atomic<u32> generation; // 8   每次覆盖式重初始化 +1
    u32 column_count; // 12  = kVizColumns(几何,magic 前写定)
    u32 track_count; // 16  = kMaxChannels
    u32 pan_scale; // 20  = kVizPanScale
    u32 _reserved[10]; // 24..64
};

// 帧头 = seqlock 临界区的起点。标量全部在此,车道/位图/轨色在其后,同一个 seq 覆盖。
struct alignas(64) VizFrame
{
    std::atomic<u32> seq; // 0   偶 = 稳定,奇 = 写入中
    std::atomic<u32> playhead_flags; // 4   VizPlayheadFlag 位组合
    std::atomic<u64> publish_ms; // 8   发布时刻(steadyNowMs;读方据此判断写方是否停摆)
    std::atomic<u64> window_start_samples; // 16  降采样窗口起点(v1 恒 0 = 工程起点)
    std::atomic<u64> window_span_samples; // 24  窗口跨度(0 = 无有效窗口)
    std::atomic<std::int64_t> playhead_samples; // 32  -1 = 无时间线
    std::atomic<std::int64_t> loop_start_samples; // 40  -1 = 无效(kVizLoopValid 未置位时)
    std::atomic<std::int64_t> loop_end_samples; // 48  -1 = 无效
    std::atomic<u32> sample_rate; // 56  0 = 未 prepare
    std::atomic<u32> version_active; // 60  1|2(车道数据取自哪个版本)
    std::atomic<u32> playhead_epoch; // 64  时间线跳变代数(定位/循环回跳时 +1)
    std::atomic<u32> track_online_mask; // 68  bit{N-1} = 该轨启用
    std::atomic<u32> track_covered_mask; // 72  bit{N-1} = 该轨有分段(车道非全哨兵)
    std::atomic<u32> track_stereo_mask; // 76  bit{N-1} = 该轨立体声源
    std::atomic<u32> lane_revision; // 80  车道/位图内容版本(仅重算车道时 +1;读方可据此跳过重解析)
    u32 _reserved[11]; // 84..128
};

// 轨色索引:调色板槽位(1..15;0 = 未指定)。v1 恒 = 轨号(web 侧 --track-color-{n} 顺序即轨号),
// 字段先行落段,将来若引入 native 侧重映射,读写两侧无需改布局。
struct alignas(64) VizTrackColors
{
    std::atomic<u32> index[kMaxChannels]; // 0..60
    u32 _pad; // 60..64
};

// 分段 activity 位图:每轨 kVizColumns 位,LSB 优先(列 i → words[i/32] 的 bit(i%32))。
struct alignas(64) VizCoverage
{
    std::atomic<u32> bits[kMaxChannels][kVizCoverageWords]; // 15 × 32 × 4 = 1920
};

// 降采样 pan 车道:每轨 kVizColumns 个 int16 定点值。
struct alignas(64) VizLanes
{
    std::atomic<std::int16_t> pan[kMaxChannels][kVizColumns]; // 15 × 1024 × 2 = 30720
};

// 每轨**当前值**三件套(分布图数据面;轨迹图用车道)。索引 15 保留,只为让每个数组按 32 字节对齐。
// 口径提醒:panNow 是**播放头精确时刻**的求值,不是 pan 车道在播放头所在列的采样 ——
// 后者是列中心点采样,列宽 = span/1024,分布图要的是「此刻」。
struct alignas(64) VizTrackState
{
    std::atomic<std::int16_t> panNow[16]; // 0..32    定点 ×100,−100..+100
    std::atomic<std::int16_t> volDb[16]; // 32..64   定点 ×100,−24..+12 dB
    std::atomic<std::int16_t> widthPct[16]; // 64..96   定点 ×100,0..100
    std::atomic<std::int16_t> _reserved[16]; // 96..128
};

// 轨名(UTF-8,NUL 补齐)。u32 字数组而非 u8 —— 跨进程字段一律 std::atomic(ipc §0),
// 按字存取比 480 个 atomic<u8> 干净,字节序 = 主机序(同机同序,读写两侧共用下面的存取器)。
struct alignas(64) VizTrackLabels
{
    std::atomic<u32> utf8[kMaxChannels][kVizLabelWords]; // 15 × 32 = 480
    u32 _pad[8]; // 480..512
};

// 段内偏移(全部 64 字节对齐)。
inline constexpr std::size_t kVizSegmentSize = 65536; // 64 KB(尾部预留给后续增补)
inline constexpr std::size_t kVizHeaderOffset = 0;
inline constexpr std::size_t kVizFrameOffset = 64;
inline constexpr std::size_t kVizColorsOffset = 192;
inline constexpr std::size_t kVizCoverageOffset = 256;
inline constexpr std::size_t kVizLanesOffset = 2176;
inline constexpr std::size_t kVizTrackStateOffset = 32896;
inline constexpr std::size_t kVizLabelsOffset = 33024;

// ---------------------------------------------------------------------------
// 主机侧快照(非段内布局;写方填、读方取)
// ---------------------------------------------------------------------------

// ≈32 KB,**勿在栈上构造** —— 请作为长寿命对象的成员或 unique_ptr 持有。
struct VizSnapshot
{
    u64 publishMs = 0;
    u64 windowStartSamples = 0;
    u64 windowSpanSamples = 0;
    std::int64_t playheadSamples = -1;
    std::int64_t loopStartSamples = -1;
    std::int64_t loopEndSamples = -1;
    u32 sampleRate = 0;
    u32 versionActive = 1;
    u32 playheadFlags = 0;
    u32 playheadEpoch = 0;
    u32 onlineMask = 0;
    u32 coveredMask = 0;
    u32 stereoMask = 0;
    u32 laneRevision = 0;
    std::array<u32, kMaxChannels> trackColor{};
    std::array<std::array<u32, kVizCoverageWords>, kMaxChannels> coverage{};
    std::array<std::array<std::int16_t, kVizColumns>, kMaxChannels> pan{};
    // 每轨当前值(分布图);哨兵 kVizPanNone = 无数据。
    std::array<std::int16_t, kMaxChannels> panNow{};
    std::array<std::int16_t, kMaxChannels> volDb{};
    std::array<std::int16_t, kMaxChannels> widthPct{};
    // 轨名(已按 UTF-8 边界截断到 ≤kVizLabelBytes-1 字节)。
    std::array<std::string, kMaxChannels> label{};

    // 默认态 = 「全轨无数据」:车道全哨兵、覆盖全 0(而非 pan=0 的中央位置 —— 零值是合法 pan,
    // 拿它当空态会让读侧把没数据的轨画成一条居中的直线)。
    VizSnapshot() { clearLanes(); }

    void clearLanes() noexcept
    {
        for (auto& lane : pan)
        {
            lane.fill(kVizPanNone);
        }
        for (auto& words : coverage)
        {
            words.fill(0u);
        }
        panNow.fill(kVizPanNone);
        volDb.fill(kVizPanNone);
        widthPct.fill(kVizPanNone);
        for (auto& s : label)
        {
            s.clear();
        }
    }

    // 位图取位(列越界返回 false)。
    bool covered(u32 track, u32 column) const noexcept
    {
        if (track >= kMaxChannels || column >= kVizColumns)
        {
            return false;
        }
        return (coverage[track][column / 32] & (1u << (column % 32))) != 0u;
    }
    void setCovered(u32 track, u32 column) noexcept
    {
        if (track < kMaxChannels && column < kVizColumns)
        {
            coverage[track][column / 32] |= (1u << (column % 32));
        }
    }
};

// 定点换算(读写两侧共用,避免口径漂移)。
inline std::int16_t vizPackPan(double pan) noexcept
{
    const double clamped = pan < -100.0 ? -100.0 : (pan > 100.0 ? 100.0 : pan);
    const double scaled = clamped * static_cast<double>(kVizPanScale);
    return static_cast<std::int16_t>(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
}
inline bool vizPanIsNone(std::int16_t v) noexcept
{
    return v == kVizPanNone;
}
inline double vizUnpackPan(std::int16_t v) noexcept
{
    return static_cast<double>(v) / static_cast<double>(kVizPanScale);
}

// 通用定点(volDb / widthPct 共用 kVizPanScale)。夹到 int16 可表示范围,绝不撞哨兵。
inline std::int16_t vizPackFixed(double v) noexcept
{
    const double scaled = v * static_cast<double>(kVizPanScale);
    const double rounded = scaled < 0.0 ? scaled - 0.5 : scaled + 0.5;
    if (rounded <= -32767.0)
    {
        return -32767; // 下界留一格给哨兵 -32768
    }
    if (rounded >= 32767.0)
    {
        return 32767;
    }
    return static_cast<std::int16_t>(rounded);
}
inline double vizUnpackFixed(std::int16_t v) noexcept
{
    return static_cast<double>(v) / static_cast<double>(kVizPanScale);
}

// UTF-8 安全截断:返回不超过 maxBytes 且不切断多字节序列的前缀长度。
// 只认续字节的 10xxxxxx 位模式往回退,不需要完整的 UTF-8 校验 —— 输入来自我们自己的
// juce::String::toStdString(),已是良构 UTF-8。
inline std::size_t vizUtf8TruncateLen(const std::string& s, std::size_t maxBytes) noexcept
{
    if (s.size() <= maxBytes)
    {
        return s.size();
    }
    std::size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u)
    {
        --n; // 落在续字节上:往回退到序列首字节
    }
    return n;
}

// ---------------------------------------------------------------------------
// VizPlane —— 段门面
// ---------------------------------------------------------------------------

class VizPlane
{
public:
    explicit VizPlane(ISegmentBackend& backend, u32 group);
    ~VizPlane();

    VizPlane(const VizPlane&) = delete;
    VizPlane& operator=(const VizPlane&) = delete;

    // 写方(Output [M]):创建或打开 + §4.0 初始化(allowOverwrite=true,owner 角色)。
    InitResult open();

    // 读方(Monitor [M]):**只读** attach。段不存在 → kFailed(空态,由 [M] 周期重试);
    // magic 相符 abi 不符 → kAbiMismatch(拒连 + 横幅,J40)。绝不创建、绝不写、绝不重初始化。
    InitResult attachReadOnly();

    bool isOpen() const { return base_ != nullptr; }
    bool isReadOnly() const { return readOnly_; }
    u32 group() const { return group_; }
    u32 generation() const;

    // 几何自检:段内 column_count/track_count/pan_scale 与本进程编译期常量一致。
    // attach 成功后调用;不一致 = 同 abi 下的几何漂移(理论不应发生),读方按拒连处理。
    bool geometryMatches() const;

    // 改组(J66):释放本组 viz 段句柄 → 换新组 → 按原角色重新映射。
    InitResult changeGroup(u32 newGroup);

    // 释放映射(消息线程):**立即** unmap,不走租约握手与宽限期。
    // 为什么不像 Registry/CtrlPlane 那样用 SegmentHandle:那套机制存在的唯一理由是
    // 「音频线程可能仍持有裸指针」。viz 段**没有任何音频线程访问**(RT 零写入/零读取铁律),
    // 全部存取都在同一条消息线程上,所以租约与宽限期在这里不但没用,还有两个实害:
    //   ① 读方 release 后仍把段吊住 —— 写方进程都退出了,段却不消失,读方永远等不到「空态」;
    //   ② 宽限期未满就析构 → SegmentHandle 退回「进程退出统一回收」= 每次换组泄漏一份映射。
    // 见 tests/ipc/test_ipc_viz.cpp 的 VIZ-3(写方退出 → 读方空态 → 再上线重连)。
    void release();

    // 写方:整帧发布(seqlock 临界区)。writeLanes=false 时只刷帧头标量(playhead/掩码/时刻),
    // 车道/位图/轨色保持上一帧内容 —— 4Hz 刷 playhead、车道按需重算的分频发布口径。
    // 仅消息线程调用;只读 attach 状态下为 no-op(防误写)。
    void publish(const VizSnapshot& s, bool writeLanes);

    // 读方:一致性读。返回 false = 段未打开 / seqlock 连续撕裂 kVizReadRetries 次(沿用上帧)。
    bool read(VizSnapshot& out) const;

    // 「RT 线程零写入」的运行期护栏(不只是注释):open() 记下调用线程 = 段的 owner 线程([M]);
    // publish() 若发现自己跑在别的线程上,**一个字节都不写**并计数。processBlock 误接线时
    // 计数会非零,由 tests/core/test_viz_plane.cpp 的零写入用例钉死。
    u64 foreignThreadWrites() const { return foreignThreadWrites_.load(std::memory_order_relaxed); }

private:
    VizHeader* header() const;
    VizFrame* frame() const;
    VizTrackColors* colors() const;
    VizCoverage* coverageBits() const;
    VizLanes* lanes() const;
    VizTrackState* trackState() const;
    VizTrackLabels* labels() const;
    void releaseHandle();

    ISegmentBackend& backend_;
    u32 group_ = 1;
    bool readOnly_ = false;
    SegmentView view_; // 直接持有映射(无租约、无宽限期;理由见 release() 注释)
    unsigned char* base_ = nullptr;

    std::thread::id ownerThread_{}; // open() 的调用线程([M]);publish() 只认这一个
    std::atomic<u64> foreignThreadWrites_{0};
};

// ---------------------------------------------------------------------------
// 布局静态断言(布局是宪法)
// ---------------------------------------------------------------------------

static_assert(std::atomic<std::int16_t>::is_always_lock_free, "atomic<int16_t> 必须 lock-free");
static_assert(std::atomic<std::int64_t>::is_always_lock_free, "atomic<int64_t> 必须 lock-free");
static_assert(sizeof(std::atomic<std::int16_t>) == 2, "atomic<int16_t> 必须 2 字节(段布局依赖)");
static_assert(sizeof(std::atomic<std::int64_t>) == 8, "atomic<int64_t> 必须 8 字节(段布局依赖)");

// VizHeader
static_assert(sizeof(VizHeader) == 64, "VizHeader 必须占满 cacheline 0");
static_assert(offsetof(VizHeader, magic) == 0);
static_assert(offsetof(VizHeader, abi) == 4);
static_assert(offsetof(VizHeader, generation) == 8);
static_assert(offsetof(VizHeader, column_count) == 12);
static_assert(offsetof(VizHeader, track_count) == 16);
static_assert(offsetof(VizHeader, pan_scale) == 20);
static_assert(offsetof(VizHeader, _reserved) == 24);
static_assert(alignof(VizHeader) == 64);

// VizFrame
static_assert(sizeof(VizFrame) == 128, "VizFrame 必须 128 字节");
static_assert(offsetof(VizFrame, seq) == 0);
static_assert(offsetof(VizFrame, playhead_flags) == 4);
static_assert(offsetof(VizFrame, publish_ms) == 8);
static_assert(offsetof(VizFrame, window_start_samples) == 16);
static_assert(offsetof(VizFrame, window_span_samples) == 24);
static_assert(offsetof(VizFrame, playhead_samples) == 32);
static_assert(offsetof(VizFrame, loop_start_samples) == 40);
static_assert(offsetof(VizFrame, loop_end_samples) == 48);
static_assert(offsetof(VizFrame, sample_rate) == 56);
static_assert(offsetof(VizFrame, version_active) == 60);
static_assert(offsetof(VizFrame, playhead_epoch) == 64);
static_assert(offsetof(VizFrame, track_online_mask) == 68);
static_assert(offsetof(VizFrame, track_covered_mask) == 72);
static_assert(offsetof(VizFrame, track_stereo_mask) == 76);
static_assert(offsetof(VizFrame, lane_revision) == 80);
static_assert(offsetof(VizFrame, _reserved) == 84);
static_assert(alignof(VizFrame) == 64);

// VizTrackColors / VizCoverage / VizLanes
static_assert(sizeof(VizTrackColors) == 64, "VizTrackColors 必须 64 字节");
static_assert(offsetof(VizTrackColors, index) == 0);
static_assert(alignof(VizTrackColors) == 64);
static_assert(sizeof(VizCoverage) == 1920, "VizCoverage 必须 1920 字节(15×32×4)");
static_assert(offsetof(VizCoverage, bits) == 0);
static_assert(alignof(VizCoverage) == 64);
static_assert(sizeof(VizLanes) == 30720, "VizLanes 必须 30720 字节(15×1024×2)");
static_assert(offsetof(VizLanes, pan) == 0);
static_assert(alignof(VizLanes) == 64);
static_assert(sizeof(VizTrackState) == 128, "VizTrackState 必须 128 字节");
static_assert(offsetof(VizTrackState, panNow) == 0);
static_assert(offsetof(VizTrackState, volDb) == 32);
static_assert(offsetof(VizTrackState, widthPct) == 64);
static_assert(offsetof(VizTrackState, _reserved) == 96);
static_assert(alignof(VizTrackState) == 64);
static_assert(sizeof(VizTrackLabels) == 512, "VizTrackLabels 必须 512 字节(15×32 + 32 pad)");
static_assert(offsetof(VizTrackLabels, utf8) == 0);
static_assert(offsetof(VizTrackLabels, _pad) == 480);
static_assert(alignof(VizTrackLabels) == 64);

// 段内偏移:全部 64 对齐、顺序紧接、不越预算。
static_assert(kVizFrameOffset % 64 == 0 && kVizFrameOffset == kVizHeaderOffset + sizeof(VizHeader));
static_assert(kVizColorsOffset % 64 == 0 && kVizColorsOffset == kVizFrameOffset + sizeof(VizFrame));
static_assert(kVizCoverageOffset % 64 == 0 && kVizCoverageOffset == kVizColorsOffset + sizeof(VizTrackColors));
static_assert(kVizLanesOffset % 64 == 0 && kVizLanesOffset == kVizCoverageOffset + sizeof(VizCoverage));
static_assert(kVizTrackStateOffset % 64 == 0 && kVizTrackStateOffset == kVizLanesOffset + sizeof(VizLanes));
static_assert(kVizLabelsOffset % 64 == 0 && kVizLabelsOffset == kVizTrackStateOffset + sizeof(VizTrackState));
static_assert(kVizLabelsOffset + sizeof(VizTrackLabels) <= kVizSegmentSize, "viz 段必须 ≤ 64 KB");

} // namespace scvb
