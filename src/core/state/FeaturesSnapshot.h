// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// [SL-226] FrameStore ↔ FEAT 数据模型的双向搬运(04 §5.2 的两端接头)。
//
// 为什么单独一层:`FeaturesCodec` 只管「FeaturesData ↔ 字节」,`FrameStore` 只管「内存里的
// 分页特征」,此前**没有任何人把这两头接起来** —— 于是采集到的特征活在内存里、工程存下去
// 一个字节都没有,重开工程泳道全空(SL-226 用户实测)。段表在 CRVS 里照常往返,所以只丢波形
// 不丢分段,两条持久化路径一条接了一条没接。
//
// 放在 scvb_core、不带 JUCE:这段搬运是纯数据变换,该能离线 Catch2 单测(CLAUDE.md §9),
// 不该只在 host harness 里被间接覆盖。

#include <cstdint>

#include "analysis/FrameStore.h"
#include "state/FeaturesCodec.h"

namespace scvb::state
{

// FrameStore → FeaturesData。只导出**已覆盖**的 hop(coverage 之外的页数据是历史残留,
// 不是有效特征,导出去会把空洞填成假数据)。channel 无覆盖则整条不进 channels。
FeaturesData snapshotFeatures(const analysis::FrameStore& store, std::uint32_t sampleRate, std::uint32_t hopMs);

// FeaturesData → FrameStore。逐 channel **先 reset 再回灌**:加载工程是「换一份数据」,
// 不是「并进现有数据」—— 不 reset 的话上一个工程的覆盖会残留成幽灵波形。
// channelId 越界(非 1..maxChannels)的条目静默跳过(不可信字节,§7.3)。
void restoreFeatures(const FeaturesData& data, analysis::FrameStore& store);

} // namespace scvb::state
