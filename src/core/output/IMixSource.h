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
    // 返回 true = 本块有有效数据;false = 缺口(该轨该块静音;dst 内容不可用)。
    // 调用方保证 t0>=0。
    // readerEpoch = 读方**自身**时间线代号(OutputProcessor 的 podEpoch_,§5.2 步骤 2):读方自己的
    // 时间线跳变(定位 / 循环回绕 / 起播)时 +1,停走带静止重读同一个 t0 时不变。
    // 读方靠它分清「我自己跳了」与「我只是中间有几块没读(该轨暂时不在注入集)」—— 后者不能当跳变,
    // 否则轨被移出注入集一段时间就会被误判成跳变(A-5 规则 1)。
    // hostBlockEnd = 读方这一宿主块的尾(时间线位置)。宿主块长超过 prepare 预算(SL-523)时 Output 按段读,
    // 同一宿主块里后面几段读的时候写方早已写完整块:写头相对本段多领先 hostBlockEnd − (t0+n),判「写头相对
    // 读方不超过实测提前量上界」时要把这一截算进去。不分段时 = t0+n。
    virtual bool read(int64_t t0, float* dst, int n, u64 readerEpoch, int64_t hostBlockEnd) noexcept = 0;

    // 宿主块不分段的调用方(单测 / 工具):hostBlockEnd = t0+n。
    bool read(int64_t t0, float* dst, int n, u64 readerEpoch) noexcept { return read(t0, dst, n, readerEpoch, t0 + n); }

    // 不知道自身时间线代号的调用方(单测 / 工具;生产路径 OutputProcessor 不走这里):代号恒为 0,
    // 读方因此永远看不到「自己跳了」,时间线不连续一律按「中间少读了几块」处理(只走保守规则)。
    bool read(int64_t t0, float* dst, int n) noexcept { return read(t0, dst, n, 0, t0 + n); }

    // 失准计数(atomic,供 [M] 聚合到 ctrl 全局小节)。
    virtual u32 gapCount() const noexcept = 0;
    // 当前 write_head / epoch([M] 停摆看门狗与全局小节直读)。
    virtual u64 writeHead() const noexcept = 0;
    virtual u64 epoch() const noexcept = 0;
};

} // namespace scvb::output
