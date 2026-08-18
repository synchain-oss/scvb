// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GroupProbe —— [M] 1Hz 跨组只读探测(01 §4.5 / J70):openExisting 本组外各组的 registry 段,
// 单次校验 magic/abi 后读 OutputSlot 心跳。只读 attach(绝不创建段、绝不覆盖式重初始化);
// magic 未就绪/段不存在 → 该组判离线(探测失败是可接受降级:UI 绿点全灭、不报错,05 §3/J70)。
// 禁止音频线程调用(openExisting/unmap 是系统调用;心跳位图仅驱动 1Hz UI 绿点)。

#include <cstdint>

#include "ISegmentBackend.h"

namespace scvb
{
// 返回 u8 位图(bit0=组A/g1 … bit7=组H/g8):bit{G-1} 置位 = 组 G 的 OutputSlot 活跃且心跳新鲜
// (≤kStaleDisplayMs)。ownGroup 的位恒为 0(本组位由调用方从本组 registry 填,见 InputSession::
// groupsOnline),其余 7 组逐一探测。
std::uint8_t probeGroupsOnline(ISegmentBackend& backend, u32 ownGroup, u64 nowMs) noexcept;
} // namespace scvb
