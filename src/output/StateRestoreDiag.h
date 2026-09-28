// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

// [SL-218][SL-219] 「这一次载入有哪几节没恢复」的诊断位图(纯 C++17,无 JUCE)。
//
// 生产者 = `ScvbOutputAudioProcessor::setStateInformation`(每次真的走到「载入」那一步就整份重算,
// 见那里的注释);消费者 = `OutputEditor::emitStateNotRestoredError`,把它变成
// §2.9 / §5.1 的 `scvb.error{code:"stateNotFullyRestored", detail:{missing, rejected}}`。
//
// 位图只收两节:CFGS(配置)与 CRVS(段表)。**CRVS 位是这条 code 的必要条件** ——
// CFGS 缺失或解不开时,`setStateInformation` 在读 CRVS 之前就早退了,段表同样没恢复,
// 所以只要有任何一位,就一定有一个 CRVS 位;横幅那句「段表没能恢复」因此对每一种组合都成立。
// FEAT / UICF / PRMS 不进本位图:FEAT 有自己的读失败处置(`readFeaturesChunk` 的留底与
// `featCodecNewer_` / `featRefUnresolved_`),UICF / PRMS 解不开时退回默认值、不影响段表。
//
// 「missing」= 这一节不在 blob 里;「rejected」= 这一节在,但没被采用 —— 解不开 / 值越界 /
// 由更高 minor 写入,或者(仅 CRVS)因为 CFGS 缺失或解不开、在读到它之前就早退了。
namespace scvb::output
{

inline constexpr std::uint8_t kNotRestoredCrvsMissing = 1u << 0;
inline constexpr std::uint8_t kNotRestoredCrvsRejected = 1u << 1;
inline constexpr std::uint8_t kNotRestoredCfgsMissing = 1u << 2;
inline constexpr std::uint8_t kNotRestoredCfgsRejected = 1u << 3;
inline constexpr std::uint8_t kNotRestoredCrvsAny = kNotRestoredCrvsMissing | kNotRestoredCrvsRejected;

// detail 的两张 fourcc 表(顺序固定:CFGS 在前、CRVS 在后,与 blob 里的读取次序一致)。
struct NotRestoredFourccs
{
    std::vector<const char*> missing;
    std::vector<const char*> rejected;
};

inline NotRestoredFourccs notRestoredFourccs(std::uint8_t mask)
{
    NotRestoredFourccs out;
    if ((mask & kNotRestoredCfgsMissing) != 0)
        out.missing.push_back("CFGS");
    if ((mask & kNotRestoredCfgsRejected) != 0)
        out.rejected.push_back("CFGS");
    if ((mask & kNotRestoredCrvsMissing) != 0)
        out.missing.push_back("CRVS");
    if ((mask & kNotRestoredCrvsRejected) != 0)
        out.rejected.push_back("CRVS");
    return out;
}

} // namespace scvb::output
