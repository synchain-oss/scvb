// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/StateMigration.h"

#include <algorithm>

namespace scvb::state
{

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
