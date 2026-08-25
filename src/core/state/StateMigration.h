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

// kMigrators[i] 将 abi=i+1 升到 abi=i+2。当前 abi=2:abi=1 的 CFGS 无 loudness_mode/center_slot_policy
// 两个尾字段,但 OutputStateCodec 解码按「长度回退」把缺失尾字段回落默认,故 migrate_1_to_2 为 no-op
// (不重写 CFGS payload)。未来升 abi 时在此追加 migrate_2_to_3 等,并同步 tests/golden/state/abi{N}.bin。
bool migrate_1_to_2(StateChunks& chunks) noexcept;
inline constexpr std::array<MigrateFn, 1> kMigrators{&migrate_1_to_2};

// 完整加载:校验 magic/长度 → abi 判读 →(低版本)迁移链 → 逐 TLV 解析(未知 fourcc 保留)。
StateLoadResult loadState(const std::uint8_t* data, std::size_t size, StateChunks& out);

} // namespace scvb::state
