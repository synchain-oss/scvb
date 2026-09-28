// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// [J145] 波形瓦片的吸附谷点(契约 §1.27 `requestWaveform.valleys[]`)。
//
// 数据面 = FrameStore 里这一轨已采集的特征(与瓦片的包络列同一份账);算法 = Segmentation 的
// `detectSnapValleys`(与 S1 谷切分共用 ℓ 口径、平滑、谷底判定与 minDepth 门槛,见其头注)。
// 放在 core 而不是 OutputProcessor 里:这样离线 Catch2 就能直接喂一张 FrameStore 验它,
// OutputProcessor::waveformOf 只负责持锁调用、把结果挂到瓦片上。

#include <cstddef>
#include <cstdint>
#include <vector>

#include "analysis/FrameStore.h"
#include "analysis/Segmentation.h"

namespace scvb::analysis
{

// 单次调用最多扫描多少个**已覆盖** hop(含两侧上下文)。超出 ⇒ 本块不给谷点(空数组)。
// 1 小时 @10ms。只有「一眼看完一小时以上的已采集素材」那种极远缩放才会碰到,那时 6px 的
// 捕获半径已是十几秒,吸附本来就没有意义;而代价上界必须与「请求跨度」无关(P0-A 那一族)。
inline constexpr std::uint64_t kSnapValleyMaxScanHops = 360000;

// 请求窗两侧各多看这么多 hop 作上下文(3 s)。谷的 depth 要向两侧走到「第一个更低点」,
// 贴着窗边截断会把窗边的谷算浅、甚至算没;有了上下文,同一个谷在相邻两块里算出同一个结果。
inline constexpr std::uint64_t kSnapValleyContextHops = 300;

// 返回 [startS, endS) 内的谷点时刻(秒,升序,取谷底 hop 的**中心**时刻 (hop+0.5)·hopS)。
// 只扫**已覆盖**的 hop,各覆盖段分别检测(未覆盖 = 不知道,不是静音,不许跨过它连成一个谷)。
// 条数上限 maxCount:超出时留 depth 最大的 maxCount 个,再按时间升序排回去。
// hopS <= 0、endS <= startS、maxCount == 0 ⇒ 空。
std::vector<double> snapValleysSeconds(const ChannelFrames& frames, double startS, double endS, double hopS,
                                       const SegmentationParams& p, std::size_t maxCount);

} // namespace scvb::analysis
