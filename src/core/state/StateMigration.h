// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "state/StateCodec.h"

// T19:state 迁移框架(03 §6.2 / CLAUDE.md §7.3)。
//
// abi 规则(03 §6.2):
//   - 读到高版本 → RejectedNewer,preservedOriginal 保留整个 blob,由 Output 层原样回写,绝不让旧插件
//     重写(毁掉)新版数据;
//   - 读到低版本 → 依次执行 kMigrators[abi-1 .. kCurrentAbi-2] 升格,全部成功 → Migrated;
//   - magic/长度/chunk 边界校验失败 → Corrupt(保持默认态,UI 报错)。
namespace scvb::state
{

enum class StateLoadStatus
{
    Ok = 0,
    Migrated = 1,
    RejectedNewer = 2,
    Corrupt = 3,
};

struct StateLoadResult
{
    StateLoadStatus status = StateLoadStatus::Corrupt;
    std::vector<std::uint8_t> preservedOriginal; // RejectedNewer 时保留原 blob(save 原样回写)
};

// in-place 升格 abi=n → n+1(chunks.abi 由框架在链末设为 kCurrentAbi,迁移函数不得依赖其入参值)。
using MigrateFn = bool (*)(StateChunks& chunks);

// kMigrators[i] 将 abi=i+1 升到 abi=i+2。当前 abi=3,两级都靠 OutputStateCodec 的**长度回退**,
// 故两个迁移函数都是 no-op(不重写 CFGS payload):
//   · abi=1 的 CFGS 无 loudness_mode/center_slot_policy 两个尾字段 → 解码回落默认;
//   · abi=2 的 CFGS 无 applied.{loudness_mode,center_slot_policy} → [SL-279] 解码令 **applied := 当前值**
//     (不是回落默认 —— 理由写在 OutputStateCodec.h 的兼容段:回落默认会让存了非默认档的旧工程
//     一打开就误报「需重新分析」)。
// 再升 abi 时在此追加 migrate_3_to_4 等,并**新增** tests/golden/state/abi{N}.bin ——
// 旧的 abi{N-1}.bin **保留**作迁移基线,不是替换(abi1/abi2 就是这么并存的)。
bool migrate_1_to_2(StateChunks& chunks) noexcept;
bool migrate_2_to_3(StateChunks& chunks) noexcept;
inline constexpr std::array<MigrateFn, 2> kMigrators{&migrate_1_to_2, &migrate_2_to_3};

// 完整加载:校验 magic/长度 → abi 判读 →(低版本)迁移链 → 逐 TLV 解析(未知 fourcc 保留)。
StateLoadResult loadState(const std::uint8_t* data, std::size_t size, StateChunks& out);

} // namespace scvb::state
