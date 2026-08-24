// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB · source_channels 口径(Input 只读 J57;契约 §4.3 scvb.config.source_channels)
// -----------------------------------------------------------------------------
// 单一真源:1 = mono / 2 = stereo / 0 或 undefined = 尚未测量。
// renderSource / renderRemoteSummary 与 node 侧 smoke 均消费本函数,口径不得分叉。
// =============================================================================

/** @param {number|undefined} sc source_channels 值
 *  @returns {"mono"|"stereo"|"unmeasured"} */
export function sourceKind(sc) {
    if (sc === 1) return "mono";
    if (sc === 2) return "stereo";
    return "unmeasured";
}
