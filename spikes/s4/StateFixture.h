// SPDX-License-Identifier: GPL-3.0-or-later
// S4 spike:满配 state 夹具构造(确定性,固定种子,可跨 commit 复现)。

#pragma once

#include "StateSchema.h"

namespace scvb::s4
{

struct FixtureOptions
{
    std::size_t tracks = kTrackCount;
    int seconds = 300; // 满配口径:5 分钟
    std::uint32_t sampleRate = kDefaultSampleRate;
    std::uint32_t seed = 0xC0FFEEu;
};

// 构造满配 state:2 版曲线 + tracks 轨 × seconds 秒特征(内嵌)。
//   特征口径(J59/J57):每轨一条(mono 单通道 / stereo 多通道 BS.1770 求和,不因 stereo 加倍);
//   5 B/hop(kw i16 + peak i16 + vad u8),hop=10ms。
FullState buildFullState(const FixtureOptions& opts);

} // namespace scvb::s4
