// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// IMixSource —— Output 混音源的每轨读取策略(01 §5.2 读环部分)。
// 抽象出「读一个 channel 的音频环」:ShmRingMixSource 为共享内存实现,测试可注入假源。
// JUCE-free,可离线单测(不引用 AudioProcessor / AudioBuffer)。

#include <cstdint>

namespace scvb::output
{

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// 每轨混音源:把时间线区间 [t0, t0+n) 读进 interleaved 缓冲,并承担 covered 判定 /
// 读中换代弃用 / 失准计数(01 §5.2 读环语义)。
class IMixSource
{
public:
    virtual ~IMixSource() = default;

    // 已绑定到有效环头(channels ∈ {1,2}、ring_frames 为 2^k、magic/abi 相符)。
    virtual bool bound() const noexcept = 0;
    // 声道数(1|2);未绑定 → 0。
    virtual u32 channels() const noexcept = 0;
    // 几何快照(bind 时读一次,此后只读快照)。
    virtual u32 sampleRate() const noexcept = 0;
    virtual u32 ringFrames() const noexcept = 0;

    // 读 [t0, t0+n) 到 interleaved dst(n × channels 个 float)。
    // 返回 true = 本块有有效数据;false = 缺口(该轨该块静音,gapCount +1,dst 不改)。
    // 调用方保证 t0>=0。
    virtual bool read(int64_t t0, float* dst, int n) noexcept = 0;

    // 失准计数(atomic,供 [M] 聚合到 ctrl 全局小节)。
    virtual u32 gapCount() const noexcept = 0;
    // 当前 write_head / epoch([M] 停摆看门狗与全局小节直读)。
    virtual u64 writeHead() const noexcept = 0;
    virtual u64 epoch() const noexcept = 0;
};

} // namespace scvb::output
