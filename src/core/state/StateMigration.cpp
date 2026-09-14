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

// abi 2→3:[SL-278/SL-279] CFGS 再尾扩 applied.{loudness_mode,center_slot_policy}。同样是 no-op ——
// abi=2 的 CFGS 无这两个尾字段,而 decodeOutputState 按「长度回退」令 **applied := 当前值**
// (**不是**回落默认:旧工程视为「已经按它存着的那档分析过」,回落默认会让存了非默认档的工程
// 一打开就误报「需重新分析」)。旧版读到新(abi=3)blob 仍走 RejectedNewer → preservedOriginal
// 原样回写,绝不静默降级。
bool migrate_2_to_3(StateChunks& chunks) noexcept
{
    (void)chunks;
    return true;
}

// abi 3→4:[SL-411] CFGS 尾扩 analysis.segmentation.{mode,sensitivity,min_segment_ms}。同样 no-op ——
// abi=3 的 CFGS 只有到 applied.* 为止的尾部,而 decodeOutputState 按「长度回退」把缺席的
// segmentation 三项回落**规格默认**(valley / 50.0 / 120)且**不计回落**:旧工程确实没存过它们,
// 不是「存了一个不可信的值」。取规格默认也正是旧构建里 `runtime_.segmentation*` 的初值 ——
// 于是「旧工程打开后的行为与它保存时逐字相同」。
// ⚠ 与上一级(applied := 当前值)的取舍不同是**有意的**,别照抄那一级的理由:applied 的语义是
// 「上次分析所用的那一档」,缺席时取当前值才不误报「需重新分析」;而 segmentation 的语义就是
// 「当前设置」本身,旧工程没有「当过一次的设置」这回事。
// 旧版读到新(abi=4)blob 仍走 RejectedNewer → preservedOriginal 原样回写,绝不静默降级。
bool migrate_3_to_4(StateChunks& chunks) noexcept
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
