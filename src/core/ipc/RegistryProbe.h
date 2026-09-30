// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// RegistryProbe —— 异组只读探测(契约 §2.4 / 01 §4.5)。仅 WIN32(命名共享内存)。
// OpenFileMappingW(FILE_MAP_READ) 打开组 g 的 registry 段,只读 RegistryHeader + OutputSlot,
// 判 magic/abi/state==active/心跳≤2000ms。绝不写异组任何字节、绝不 CAS/claim。
// 探测失败(段不存在/映射失败/尺寸不足/abi 不符)= false(可接受降级,不重试不报错)。

#include <cstdint>

#include "SegmentLayout.h"

namespace scvb
{

// 单组探测:group ∈ [1,8];返回该组 registry 头有「心跳新鲜(≤2000ms)的 OutputSlot」(state==active)。
bool probeRegistryGroupOnline(u32 group, u64 nowMs);

} // namespace scvb
