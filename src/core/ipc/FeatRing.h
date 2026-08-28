// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// FeatRing —— 特征段读写封装(ipc-contract §3 冻结布局 + 04 §3 语义)。
//
// 写侧(Input):run 协议(startRun 顺序不可倒:base_hop.store(h0) 先、write_hop.store(h0) 后,
// J33/04 §3.2)+ biquad 首 hop 预热(复用 analysis::FeatureExtractor 的 T08 口径)+ 采集 OFF 静默丢弃。
// 读侧(Output):25Hz 增量拉取(seqlock:base→write→base 双读)+ 受限追赶(04 §3.3)。
// 时间线寻址以 FeatHeader.base_hop/write_hop(hop 序号)为唯一真源,不另造时间线概念。
//
// 【几何快照纪律(04 §3 / PR#36 复审,硬性)】FeatHeader 的几何字段 hop_ms/capacity_hops 是
// plain u32(非原子)。写侧 bind() 一次性读进不可变绑定快照 FeatRingBinding,此后 startRun/processBlock
// 只读快照、绝不回读段头几何字段 —— 回读在 re-prepare 并发改写几何时是数据竞争。特征段恒按固定容量
// kFeatCapacityHops 创建、无运行期扩容;索引 hop % capacity 有界于快照,保住「几何 ≤ 实际映射」不变式。
// bind 的 mappedCapacity = 实际映射的 FeatFrame 槽数,必须 >= 段头声明的 capacity_hops,否则拒绝绑定。
//
// 【跨线程发布协议(T16 Snapshot 同款,PR#51 红旗第 2 轮)】绑定与运行态都走「不可变快照 +
// std::atomic<const T*>」发布:
//   - FeatRingBinding(段头/环裸指针 + 几何):消息线程 bind() 构造完整后 release-store;段由调用方
//     (InputSession)经 SegmentHandle 租约/宽限期保活。
//   - FeatRunState(extractor + shifted/out 缓冲 + maxBlock + pendingSkip/nextHop 运行记账):消息线程
//     prepare() 构造全新状态后 release-store;音频线程 startRun/processBlock 每次 acquire-load 一次、
//     整调用复用同一份。旧状态由 owned_ 保活(进程寿命)——播放中加载不同 channel/group 的 preset
//     触发并发 prepare 时,音频线程在途调用仍持旧状态指针,绝不悬垂(无自旋、无锁、无握手)。
//     代价:re-prepare 后下一块切换到新 extractor(滤波态重置)——正常宿主流重配置前已停音频,
//     该语义与 startRun 的 run 边界重置一致。capturing_ 为 atomic(仅音频线程写)。

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "SegmentLayout.h"
#include "analysis/FeatureExtractor.h"
#include "analysis/FeatureFingerprint.h"
#include "analysis/FrameStore.h"

namespace scvb
{

// §3.3 增量拉取的每 channel 状态(Output [M] 25Hz)。
struct FeatPullState
{
    uint64_t lastPulled = 0;
    uint64_t lastBase = 0;
    bool initialized = false;
};

// §3.3 受限追赶:稳态每拍增量 ~4 hop(25Hz);base 未变时每拍最多追 kMaxBurstHops 个 hop(≈2.56s)。
inline constexpr uint64_t kMaxBurstHops = 256;

// 不可变特征段绑定快照(发布后不得修改)。裸指针指向共享内存段,由调用方经 SegmentHandle 保活。
struct FeatRingBinding
{
    FeatHeader* header = nullptr;
    FeatFrame* ring = nullptr;
    u32 capacity = 0; // capacity_hops(几何快照)
    u32 hopMs = 0; // hop_ms(几何快照)
    bool bound = false;
};

// 不可变运行态快照(prepare 发布后不得修改;extractor 内部滤波态由音频线程独占演化 ——
// 每次 processBlock 整调用持同一份状态指针,滤波连续性在状态不切换时保持)。
struct FeatRunState
{
    std::unique_ptr<analysis::FeatureExtractor> extractor = std::make_unique<analysis::FeatureExtractor>();
    std::vector<const float*> shifted; // prepare 分配,processBlock 复用(零分配)
    std::vector<analysis::FeatFrame> out; // prepare 分配,processBlock 复用(零分配)
    int maxBlockSamples = 0; // prepare 时的最大块长;processBlock 按此分段喂 extractor
    int64_t pendingSkip = 0; // run 起始的部分 hop 待丢弃样本数
    uint64_t nextHop = 0; // 下一个待写帧的时间线 hop 序号(**只在采集 ON 时推进**,= 段写位置)

    // ---- fingerprint watchdog(04 §4.5)的 [A] 侧累加状态 ----------------------------
    // fpHop 与 nextHop 是**两个**计数器,不能合并:nextHop 是「段里下一个待写槽」,采集 OFF
    // 时按冻结语义不推进;fpHop 是「时间线上的 hop 序号」,采集开关无关,恒跟着 run 走。
    uint64_t fpHop = 0;
    analysis::TileFingerprint fpTile; // 当前 tile 的流式 FNV-1a
    uint64_t fpTileIdx = 0; // 当前 tile 序号(= fpHop / kFpTileHops;u64,超 16 位上限的不上报)
    uint32_t fpTileCount = 0; // 当前 tile 已累计的 hop 数
    bool fpTileOpen = false; // 是否正在累加一个**完整**的 tile(run 中途接上的半个 tile 不报)
    // run 起点恰好落在 tile 起点(h0 % 100 == 0)时,本 run 的第一个 tile 带 extractor 预热差,
    // 整个丢掉不报(见 startRun 的说明:不丢的话整秒处的短循环会稳定误报)。
    bool fpSkipFirstTile = false;
};

// 特征段写侧(Input):run 协议 + 首 hop 预热。
class FeatRing
{
public:
    FeatRing() = default;

    // 绑定特征段(Input 写侧;header/ring 可写)。mappedCapacity = 实际映射的 FeatFrame 槽数。
    // 构造不可变绑定快照并 release 发布;mappedCapacity < capacity_hops → 拒绝绑定(bound()==false)。
    void bind(FeatHeader* header, FeatFrame* ring, u32 mappedCapacity);

    // 解绑(消息线程):发布 nullptr(bound()==false,processBlock 静默不写)。
    void unbind() noexcept { binding_.store(nullptr, std::memory_order_release); }

    // 音频线程:acquire 绑定快照(每 block 一次)。
    const FeatRingBinding* acquire() const noexcept { return binding_.load(std::memory_order_acquire); }

    // 准备提取器(消息线程;播放中并发调用安全 —— 构造全新 FeatRunState 后原子发布,旧状态保活)。
    void prepare(double sampleRate, int channels, int maxBlockSamples);

    // 采集开关布防(04 §1.1)。OFF → processBlock 静默丢弃,不写段、不推进 write_hop。
    void setCapturing(bool on) noexcept { capturing_.store(on, std::memory_order_relaxed); }
    bool capturing() const noexcept { return capturing_.load(std::memory_order_relaxed); }
    bool bound() const noexcept
    {
        const FeatRingBinding* b = acquire();
        return b != nullptr && b->bound;
    }

    // run 切换协议(04 §3.2):base_hop.store(h0) 先、write_hop.store(h0) 后(顺序不可倒,J33),
    // 再 reset + 首完整 hop 预热(FeatureExtractor 口径)。betweenStores 仅供测试注入交错读取。
    void startRun(int64_t timelineSample, const std::function<void()>& betweenStores = {});

    // 推入平面声道数据;完成的 hop 写入 ring 并推进 write_hop。返回本块**写入段的** hop 数
    // (采集 OFF 时恒 0)。超大块按运行态的 maxBlockSamples 分段喂 extractor(离线渲染块长可超
    // prepare 时的 maxBlock,一次性喂会越界读 out)。实时线程零分配(缓冲均在 prepare 分配)。
    //
    // 【04 §4.5】K 加权提取器**不受采集开关门控**:采集 OFF 时照样跑同一条代码路径,只是
    // 不写段、不推进 write_hop,产出的 kw 只喂 fingerprint。段的「仅采集 ON 写」冻结语义不变。
    int processBlock(const float* const* channels, int numSamples);

    // [M] 25Hz 排水:取出已完成的 fp_report 载荷(已经过 packFpReport 打包的 u64)。
    // 返回实际取出的条数(≤ max)。04 §4.5:[A] 只写进程内 SPSC,由 [M] 转投跨进程 ctrl 命令环。
    uint32_t drainFpReports(u64* out, uint32_t max) noexcept;

    // 诊断:[A] 侧因队列满而丢弃的 fp_report 条数(1 msg/s/轨 vs 25Hz 排水,正常恒 0)。
    uint32_t fpDropCount() const noexcept { return fpDrop_.load(std::memory_order_relaxed); }

    // 诊断/测试 getter(读当前发布状态;仅单线程诊断语义)。
    int hopSize() const noexcept
    {
        const FeatRunState* s = state_.load(std::memory_order_acquire);
        return s != nullptr ? s->extractor->hopSize() : 0;
    }
    double sampleRate() const noexcept
    {
        const FeatRunState* s = state_.load(std::memory_order_acquire);
        return s != nullptr ? s->extractor->sampleRate() : 0.0;
    }
    int channels() const noexcept
    {
        const FeatRunState* s = state_.load(std::memory_order_acquire);
        return s != nullptr ? s->extractor->channels() : 0;
    }
    uint64_t nextHop() const noexcept
    {
        const FeatRunState* s = state_.load(std::memory_order_acquire);
        return s != nullptr ? s->nextHop : 0;
    }
    u32 capacityHops() const noexcept
    {
        const FeatRingBinding* b = acquire();
        return b != nullptr ? b->capacity : 0;
    }
    u32 hopMs() const noexcept
    {
        const FeatRingBinding* b = acquire();
        return b != nullptr ? b->hopMs : 0;
    }

private:
    // fingerprint 上报的**进程内** SPSC(04 §4.5「[A] 绝不直接写跨进程命令环」)。
    // 生产者 = [A](processBlock),消费者 = [M](drainFpReports)。定容预分配、无锁、无分配;
    // 满环丢**最新**并计数(1 msg/s/轨对 25Hz 排水,只有 [M] 长时间停摆才可能满,丢的是提示不是数据)。
    static constexpr uint32_t kFpQueueCapacity = 64;

    // [A] 累加一个 hop 的 kw 并在 tile 收尾时投递上报;[A] 侧入队(零分配、无锁)。
    void accumulateFp(FeatRunState& s, float kwMs, bool capturing) noexcept;
    void pushFpReport(u64 value) noexcept;

    std::atomic<const FeatRingBinding*> binding_{nullptr}; // 消息线程写 / 音频线程读
    std::vector<std::unique_ptr<FeatRingBinding>> owned_; // 旧绑定保活(进程寿命,T16 同款)
    // 注意:指针发布协议(不可变快照 = 从不原地替换),但指向对象本身由音频线程独占演化
    // (extractor 滤波态 / pendingSkip / nextHop);消息线程只整体替换指针、绝不触碰旧对象字段。
    std::atomic<FeatRunState*> state_{nullptr}; // 消息线程写 / 音频线程读写(经快照)
    std::vector<std::unique_ptr<FeatRunState>> ownedState_; // 旧运行态保活(进程寿命,防 UAF)
    std::atomic<bool> capturing_{false};
    // 上一块看到的采集开关([A] 独占,不跨线程;放 FeatRing 而不是 FeatRunState —— 它要跨过
    // re-prepare 存活,否则播放中换 preset 会伪造出一次 OFF→ON 边沿)。用于 run 中途
    // OFF→ON 的 hop 重锚,见 processBlock。
    bool lastCapturing_ = false;

    std::array<u64, kFpQueueCapacity> fpSlots_{}; // 载荷槽(索引原子,槽本身单写单读)
    std::atomic<u32> fpWrite_{0}; // [A] 写(单调递增)
    std::atomic<u32> fpRead_{0}; // [M] 读(单调递增)
    std::atomic<u32> fpDrop_{0}; // [A] 满环丢弃计数
    // 进程内 SPSC 的索引:跨线程原子必须 lock-free(§8 实时线程纪律 —— 非 lock-free 的
    // atomic 会退化成内部锁,那是音频线程上的阻塞等待)。
    static_assert(std::atomic<u32>::is_always_lock_free, "fp SPSC 索引必须 lock-free(§8)");
};

// §3.3 读侧:单 channel 一次 seqlock 增量拉取。返回本拍写入 FrameStore 的 hop 数。
// capacity 必须是 attach 时的几何快照(见 FeatPuller::bind),不得回读段头 capacity_hops。
// drainOnly = **排空**([J87] 布防期未选中轨):一帧都不写,但把读游标一路推到写头。
// 它与「传空 timeGate」不等价 —— 空 gate 仍受 kMaxBurstHops 每拍上限约束,离线快速渲染时
// 写头一拍能跑出远超 256 hop,排空追不上,撤防后残留的那段就会被补拉进来盖掉既有特征。
// 排空没有任何写入,不需要限速,直接跳到写头(run 切换 / 回退 / 环回绕三道守卫照常先跑)。
uint32_t pullIncremental(const FeatHeader& header, const FeatFrame* ring, u32 capacity, FeatPullState& state,
                         analysis::ChannelFrames& store, analysis::HopRange timeGate, bool drainOnly = false);

// Output [M] 25Hz 增量拉取驱动(04 §3.3;Analyze 前补拉即再调一次 pullTick)。
// 门控:timeGate = 时间维(global.range 或布防工作选区);selectedMask = 轨维(bit{N-1} 选中轨,
// 0 表示不限轨);activeMask = connected_mask(在线轨,只拉在线轨)。
class FeatPuller
{
public:
    // mappedCapacity = 实际映射的 FeatFrame 槽数;attach 时快照 header->capacity_hops,见文件头纪律。
    void bind(u32 channel, const FeatHeader* header, const FeatFrame* ring, u32 mappedCapacity);
    // 返回本拍**真的拉到新 hop** 的轨位图(bit{N-1})。调用方据此知道哪些轨的基线正在被
    // 改写 —— fingerprint watchdog(04 §4.5)要在那些轨上清掉旧的失配定谳,否则用户照着
    // ⚠ 去重采集之后提示还挂着(采集 ON 期间一条 fp_report 都不发,自愈路径走不到)。
    u32 pullTick(analysis::FrameStore& store, analysis::HopRange timeGate, u32 selectedMask, u32 activeMask);

    FeatPullState& state(u32 channel);
    void reset();

private:
    // 仅 [M] 25Hz 消息线程访问(非音频线程;音频线程侧写路径走 FeatRingBinding 快照,
    // 读路径 pullIncremental 每拍 acquire 段头字段 —— 无跨线程共享,PR#51 重要#1 复核)。
    struct Binding
    {
        const FeatHeader* header = nullptr;
        const FeatFrame* ring = nullptr;
        u32 capacity = 0; // 几何快照:bind 时读 header->capacity_hops,此后只读它(索引模数)
        bool bound = false;
    };

    std::array<Binding, kMaxChannels> bindings_{};
    std::array<FeatPullState, kMaxChannels> states_{};
};

} // namespace scvb
