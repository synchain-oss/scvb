// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// 每轨 ParamID 与「读回该用哪一版」(SL-229 复审①:Tab1 与 Tab2 共用一份)
// =============================================================================
// 抽到 shared 的理由:`readbackVersion` 原先只在 tab-tracks.js 里,于是 SL-229 的
// 验收**只在 Tab2 成立** —— Tab1 的分布图(tab-master.js renderDist)自己按
// `s.global.version_active` 拼 `v{v}_t{ch}_pan` 去查,切版本那一帧照样查空、照样
// 闪一排居中柱。两处查的是同一个参数面,判据就该是同一份。
// =============================================================================

// 这两个三行小工具就地带一份:为它们再开一个 shared 件不值,而 tab-tracks 那两个
// 是 export 的、语义逐字相同(`tt` 是契约 §1.12-§1.14 的两位零填充轨号)。
function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

/** 两位零填充轨号(契约 §1.12-§1.14:t = "01".."15")。 */
function tt(ch) {
    return String(ch).padStart(2, "0");
}

/** 每轨 ParamID(契约 §1.12-§1.14:`v{v}_t{t:02d}_{knob}`,t 两位零填充)。 */
export function paramIdOf(versionActive, ch, knob) {
    return `v${Math.trunc(num(versionActive, 1))}_t${tt(ch)}_${knob}`;
}

/**
 * 读回值该用**哪一版的 ParamID 命名空间**(SL-229;纯函数,node 侧可断言)。
 *
 * 病:切版本时 `scvb.state.global.version_active` 与 `scvb.params` 是两个事件。
 * state 一说「现在是 V2」,读回值立刻改查 `v2_t01_pan` —— 可 params 手上还是上一版的
 * 那 63 个 id(§2.2:切版本由 C++ **全量重发**,`full:true`),于是查了个空,
 * `num(undefined, def)` 回落**出厂默认 = 居中**,15 轨齐刷刷跳到中间;
 * 下一帧全量到达才跳回真值。用户实测的「复制版本切进去初始显示不对」就是这一下。
 *
 * 修:**params 真的带着那一版的 id 之前,不切命名空间** —— 暂用 params 手上那一版
 * (= 切出时那一版)。于是「切入瞬间显示的就是源版本切出时的值」,全量帧一到原子翻过去,
 * 中间不会闪一帧默认值。
 *
 * 只探一个代表 id 就够:§2.2 的 63 个 id 是**整批**发的,不存在半批状态。
 */
export function readbackVersion(vals, stateActive, paramsActive) {
    const v = (x) => {
        const n = Math.trunc(num(x, 0));
        return n >= 1 ? n : 0;
    };
    const s = v(stateActive);
    const p = v(paramsActive);
    if (!s) return p || 1;
    const probe = paramIdOf(s, 1, "pan");
    const has = !!vals && Object.prototype.hasOwnProperty.call(vals, probe);
    return has ? s : p || s;
}
