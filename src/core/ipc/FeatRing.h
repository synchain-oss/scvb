// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// FeatRing —— 特征段读写封装(ipc-contract §3 冻结布局 + 04 §3 语义)。
//
// 写侧(Input):run 协议(startRun 顺序不可倒:base_hop.store(h0) 先、write_hop.store(h0) 后,
// J33/04 §3.2)+ biquad 首 hop 预热(复用 analysis::FeatureExtractor 的 T08 口径)+ 采集 OFF 静默丢弃。
// 读侧(Output):25Hz 增量拉取(seqlock:base→write→base 双读)+ 受限追赶(04 §3.3)。
// 时间线寻址以 FeatHeader.base_hop/write_hop(hop 序号)为唯一真源,不另造时间线概念。

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

    // 绑定特征段(Input 写侧;header/ring 可写)。
    void bind(FeatHeader* header, FeatFrame* ring, u32 capacity) noexcept;

    // 准备提取器(采样率/声道/最大块;分配缓冲,只应在消息/准备线程调用,不得在音频线程)。
    void prepare(double sampleRate, int channels, int maxBlockSamples);

    // 采集开关布防(04 §1.1)。OFF → processBlock 静默丢弃,不写段、不推进 write_hop。
    void setCapturing(bool on) noexcept { capturing_ = on; }
    bool capturing() const noexcept { return capturing_; }

    // run 切换协议(04 §3.2):base_hop.store(h0) 先、write_hop.store(h0) 后(顺序不可倒,J33),
    // 再 reset + 首完整 hop 预热(FeatureExtractor 口径)。betweenStores 仅供测试注入交错读取。
    void startRun(int64_t timelineSample, const std::function<void()>& betweenStores = {});

    // 推入平面声道数据;完成的 hop 写入 ring 并推进 write_hop。返回本块写入的 hop 数。
    // 实时线程零分配(缓冲均在 prepare 分配)。
    int processBlock(const float* const* channels, int numSamples);

    int hopSize() const noexcept { return extractor_.hopSize(); }
    double sampleRate() const noexcept { return extractor_.sampleRate(); }
    int channels() const noexcept { return extractor_.channels(); }
    uint64_t nextHop() const noexcept { return nextHop_; }

private:
    FeatHeader* header_ = nullptr;
    FeatFrame* ring_ = nullptr;
    u32 capacity_ = 0;
    bool capturing_ = false;
    bool prepared_ = false;
    bool bound_ = false;

    analysis::FeatureExtractor extractor_;
    std::vector<const float*> shifted_; // prepare 分配,processBlock 复用(零分配)
    std::vector<analysis::FeatFrame> out_; // prepare 分配,processBlock 复用(零分配)

    int64_t pendingSkip_ = 0; // run 起始的部分 hop 待丢弃样本数
    uint64_t nextHop_ = 0; // 下一个待写帧的时间线 hop 序号
};

// §3.3 读侧:单 channel 一次 seqlock 增量拉取。返回本拍写入 FrameStore 的 hop 数。
uint32_t pullIncremental(const FeatHeader& header, const FeatFrame* ring, u32 capacity, FeatPullState& state,
                         analysis::ChannelFrames& store, analysis::HopRange timeGate);

// Output [M] 25Hz 增量拉取驱动(04 §3.3;Analyze 前补拉即再调一次 pullTick)。
// 门控:timeGate = 时间维(global.range 或布防工作选区);selectedMask = 轨维(bit{N-1} 选中轨,
// 0 表示不限轨);activeMask = connected_mask(在线轨,只拉在线轨)。
class FeatPuller
{
public:
    void bind(u32 channel, const FeatHeader* header, const FeatFrame* ring, u32 capacity);
    void pullTick(analysis::FrameStore& store, analysis::HopRange timeGate, u32 selectedMask, u32 activeMask);

    FeatPullState& state(u32 channel);
    void reset();

private:
    struct Binding
    {
        const FeatHeader* header = nullptr;
        const FeatFrame* ring = nullptr;
        u32 capacity = 0;
        bool bound = false;
    };

    std::array<Binding, kMaxChannels> bindings_{};
    std::array<FeatPullState, kMaxChannels> states_{};
};

} // namespace scvb
