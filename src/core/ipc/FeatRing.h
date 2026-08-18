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
// plain u32(非原子)。写侧/读侧都必须在 attach(bind)时一次性读进本地快照,此后每拍只读快照、
// 绝不回读段头几何字段 —— 回读在 re-prepare 并发改写几何时是数据竞争。特征段恒按固定容量
// kFeatCapacityHops 创建、无运行期扩容;索引 hop % capacity 有界于快照,保住「几何 ≤ 实际映射」
// 不变式(v1..v5 越界类事故)。bind 的 mappedCapacity = 实际映射的 FeatFrame 槽数,必须 >= 段头
// 声明的 capacity_hops,否则拒绝绑定(静默不写/不读)。

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "SegmentLayout.h"
#include "analysis/FeatureExtractor.h"
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

// 特征段写侧(Input):run 协议 + 首 hop 预热。
class FeatRing
{
public:
    FeatRing() = default;

    // 绑定特征段(Input 写侧;header/ring 可写)。mappedCapacity = 实际映射的 FeatFrame 槽数。
    // 见文件头【几何快照纪律】:attach 时快照 header->capacity_hops/hop_ms 进本地,此后不回读段头;
    // mappedCapacity < capacity_hops → 拒绝绑定(bound()==false,processBlock 静默不写)。
    void bind(FeatHeader* header, FeatFrame* ring, u32 mappedCapacity) noexcept;

    // 准备提取器(采样率/声道/最大块;分配缓冲,只应在消息/准备线程调用,不得在音频线程)。
    void prepare(double sampleRate, int channels, int maxBlockSamples);

    // 采集开关布防(04 §1.1)。OFF → processBlock 静默丢弃,不写段、不推进 write_hop。
    void setCapturing(bool on) noexcept { capturing_ = on; }
    bool capturing() const noexcept { return capturing_; }
    bool bound() const noexcept { return bound_; }

    // run 切换协议(04 §3.2):base_hop.store(h0) 先、write_hop.store(h0) 后(顺序不可倒,J33),
    // 再 reset + 首完整 hop 预热(FeatureExtractor 口径)。betweenStores 仅供测试注入交错读取。
    void startRun(int64_t timelineSample, const std::function<void()>& betweenStores = {});

    // 推入平面声道数据;完成的 hop 写入 ring 并推进 write_hop。返回本块写入的 hop 数。
    // 超大块按 prepare 的 maxBlockSamples 分段喂 extractor(离线渲染块长可超 prepare 时的 maxBlock,
    // 一次性喂会越界读 out_)。实时线程零分配(缓冲均在 prepare 分配)。
    int processBlock(const float* const* channels, int numSamples);

    int hopSize() const noexcept { return extractor_.hopSize(); }
    double sampleRate() const noexcept { return extractor_.sampleRate(); }
    int channels() const noexcept { return extractor_.channels(); }
    uint64_t nextHop() const noexcept { return nextHop_; }
    u32 capacityHops() const noexcept { return capacityHops_; } // 几何快照(attach 时读一次)
    u32 hopMs() const noexcept { return hopMs_; }

private:
    FeatHeader* header_ = nullptr;
    FeatFrame* ring_ = nullptr;
    u32 capacityHops_ = 0; // 几何快照:bind 时读 header->capacity_hops,此后只读它(索引模数)
    u32 hopMs_ = 0; // 几何快照:bind 时读 header->hop_ms(=kFeatHopMs,一致性观测)
    bool capturing_ = false;
    bool prepared_ = false;
    bool bound_ = false;

    analysis::FeatureExtractor extractor_;
    std::vector<const float*> shifted_; // prepare 分配,processBlock 复用(零分配)
    std::vector<analysis::FeatFrame> out_; // prepare 分配,processBlock 复用(零分配)

    int maxBlockSamples_ = 0; // prepare 时的最大块长;processBlock 按此分段喂 extractor
    int64_t pendingSkip_ = 0; // run 起始的部分 hop 待丢弃样本数
    uint64_t nextHop_ = 0; // 下一个待写帧的时间线 hop 序号
};

// §3.3 读侧:单 channel 一次 seqlock 增量拉取。返回本拍写入 FrameStore 的 hop 数。
// capacity 必须是 attach 时的几何快照(见 FeatPuller::bind),不得回读段头 capacity_hops。
uint32_t pullIncremental(const FeatHeader& header, const FeatFrame* ring, u32 capacity, FeatPullState& state,
                         analysis::ChannelFrames& store, analysis::HopRange timeGate);

// Output [M] 25Hz 增量拉取驱动(04 §3.3;Analyze 前补拉即再调一次 pullTick)。
// 门控:timeGate = 时间维(global.range 或布防工作选区);selectedMask = 轨维(bit{N-1} 选中轨,
// 0 表示不限轨);activeMask = connected_mask(在线轨,只拉在线轨)。
class FeatPuller
{
public:
    // mappedCapacity = 实际映射的 FeatFrame 槽数;attach 时快照 header->capacity_hops,见文件头纪律。
    void bind(u32 channel, const FeatHeader* header, const FeatFrame* ring, u32 mappedCapacity);
    void pullTick(analysis::FrameStore& store, analysis::HopRange timeGate, u32 selectedMask, u32 activeMask);

    FeatPullState& state(u32 channel);
    void reset();

private:
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
