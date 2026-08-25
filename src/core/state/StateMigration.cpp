// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/StateMigration.h"

#include <algorithm>

namespace scvb::state
{

// abi 1→2:CFGS 尾扩 loudness_mode/center_slot_policy 两个 u32 枚举序号。abi=1 的 CFGS 无这两个尾字段,
// 但 OutputStateCodec 的 decodeOutputState 按「长度回退」把缺失尾字段回落默认(且保留未知尾部),故此处
// 为 no-op(不重写 CFGS payload)。旧版(abi=1)读到新(abi=2)blob 时走 RejectedNewer → preservedOriginal
// 原样回写,绝不静默降级(CLAUDE.md §7.3 / STATE_SCHEMA)。
bool migrate_1_to_2(StateChunks& chunks) noexcept
{
    (void)chunks;
    return true;
}

StateLoadResult loadState(const std::uint8_t* data, std::size_t size, StateChunks& out)
{
    StateLoadResult res;

    StateHeader hdr;
    if (!parseHeader(data, size, hdr) || hdr.magic != kStateMagic)
    {
        res.status = StateLoadStatus::Corrupt;
        return res;
    }

    if (hdr.abi > kCurrentAbi)
    {
        res.status = StateLoadStatus::RejectedNewer;
        res.preservedOriginal.assign(data, data + size);
        return res;
    }

    StateChunks chunks;
    if (decodeContainer(data, size, chunks) != DecodeStatus::Ok)
    {
        res.status = StateLoadStatus::Corrupt;
        return res;
    }

    if (hdr.abi < kCurrentAbi)
    {
        // 依次执行 kMigrators[abi-1 .. kCurrentAbi-2](03 §6.2);任一失败 → Corrupt。
        // abi=0 是「未版本化」起点(无历史格式),从 abi=1 起才有对应 migrator。
        for (std::uint32_t a = std::max<std::uint32_t>(hdr.abi, 1u); a < kCurrentAbi; ++a)
        {
            const std::size_t idx = static_cast<std::size_t>(a - 1u);
            if (idx >= kMigrators.size() || kMigrators[idx] == nullptr || !kMigrators[idx](chunks))
            {
                res.status = StateLoadStatus::Corrupt;
                return res;
            }
        }
        chunks.abi = kCurrentAbi;
        out = std::move(chunks);
        res.status = StateLoadStatus::Migrated;
        return res;
    }

    out = std::move(chunks);
    res.status = StateLoadStatus::Ok;
    return res;
}

} // namespace scvb::state
