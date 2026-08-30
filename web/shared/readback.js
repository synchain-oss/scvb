// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// 未冻结维度的「读回真源」——Tab1 分布图与 Tab2 轨道行共用一份(SL-241)
// =============================================================================
// 抽到 shared 的理由与 `param-id.js` 头注同一条,且是同一个病灶的**第二次**发作:
// SL-211 把「未冻结维度读**曲线段**、不读参数面」这条优先级链修在了 `tab-tracks.js`
// 里,于是验收**只在 Tab2 成立** —— Tab1 的分布图(`tab-master.js` renderDist)始终
// 只读参数面。而 `copyVersion` 契约明写**零参数写入**(03 §5.3;
// `tests/core/test_version_params.cpp` VERSION-COPY-ZERO-1「123 参数逐位不变」),
// 刚复制出来的版本切进去、引擎打印头还没跑过,那 63 个 id 装的就是出厂默认
// (pan 居中 / vol 0dB)。于是用户实测:**复制版本切进去,声像分布图 15 轨齐刷刷居中,
// 一播放又全对**(打印头开始驱动参数面)—— 与 SL-211 修掉的是同一幕,只是换了张图。
//
// SL-229 给分布图补的是**版本闸**(读哪一版的命名空间),不是**真源闸**(读参数面
// 还是读曲线),所以那一卡修不掉这一幕。两处画的是同一个量,判据就该是同一份代码;
// 这一件就是那一份。`tab-tracks.js` 在原处再导出,既有 import 点一字不改
// (手法同 `param-id.js` / `distGeometry`)。
// =============================================================================

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

/**
 * 每轨 freeze 参数 `v{active}_t{ch:02d}_freeze`(契约 §1.12-§1.14:int 0-3,
 * bit0=pan / bit1=vol,两枚开关各改一位)。
 *
 * **解码口径与 native 侧 `scvb::engine::freezeBitsOf`(`src/core/engine/FreezeBits.h`)逐条对齐**:
 * 四舍五入 → 钳到 [0,3];非有限值(NaN)回 0 = 未冻结。旧写法是 `Math.trunc(x) & 3`,
 * 与 native 在两处分叉:小数 1.9 截断成 1(native 进位成 2)、越界 4 被按位截成 0
 * =「两维都没冻」(native 钳成 3 =「两维都冻」,保守的那一边)。freeze 当前是
 * `AudioParameterInt`、值恒为精确整数,所以现在不会真分叉;但「谁被冻结」这件事在
 * native / UI / mock 三侧都要同一个答案,否则 UI 会按未冻结去画一条 native 认为已冻结的轨
 * (#106 终轮复审建议)。**改这里必须同改 `FreezeBits.h` 与 mock 的同名解码**。
 */
export function freezeBits(freeze) {
    const raw = num(freeze, 0);
    const f = Number.isFinite(raw)
        ? Math.min(3, Math.max(0, Math.round(raw)))
        : 0;
    return { pan: (f & 1) === 1, vol: (f & 2) === 2 };
}

/** 从合并后的段表视图里取某轨(契约 §2.8:`channels` 只含受影响轨)。 */
export function segmentsOfCh(segments, ch) {
    const list = (segments && segments.channels) || [];
    for (const c of list) if (c && c.ch === ch) return c;
    return null;
}

/**
 * 「单段全时限 `user_edited` 常值」判定 —— `setTrackManual` **手动接管通道**的产物特征
 * (契约 §1.16 编码 = 04 §1.5 方案 A)。两处用它:
 *   ① **未冻结**维度的读回值(05 §2.2「读回值同样取自该段」)—— [J85] 之后冻结维度改读
 *      参数面,因为冻结通道根本不写曲线,段表里那条常值段只可能是**冻结前**留下的旧值;
 *   ② 解冻提示(该位 1→0 且该轨仍由手动常值驱动)。
 * 命中返回该段本身(调用方要读 pan/volDb),否则 null。
 */
export function manualConstantOf(segChannel) {
    const segs = (segChannel && segChannel.segments) || [];
    if (segs.length !== 1) return null;
    const s = segs[0];
    return s && s.origin === "user_edited" ? s : null;
}

/**
 * 曲线在某一时刻所处的段(**未冻结维度的读回真源**;SL-211,用户 v5.4 实测拍板 2026-08-27)。
 *
 * 为什么需要它:未冻结维度此前的回落是**参数面**。可参数面在非 PRINT 态装的是**宿主**
 * 那一份值 —— 刚复制完版本、切进去还没播放时,那就是出厂默认(pan 居中)。于是用户看到
 * 「全轨声像都在中间」,一播放又全对了(引擎开始驱动参数面)。用户裁定:**切进去就该
 * 显示曲线的起始值**。
 *
 * 口径与 J78 优先级链一致 —— 显示的是**该维度的权威**:
 *   · 冻结维度 → 参数面(宿主自动化 / 冻结手动值当家),不走本函数;
 *   · 未冻结维度 → 引擎分析曲线,即本函数;曲线还不存在(没分析过)才回落参数面。
 *
 * **钳位口径逐条对齐 `CurveEvaluator::valueAt`**(复审终轮③b;src/core/engine 那份是
 * DSP 真身,显示层跟它走才叫「显示权威」):
 *   · 首段之前   → **首段**值(「切进版本还没播放」,t=0 常常就落在这一档);
 *   · 末段之后   → **末段**值(不是首段 —— 原先把「曲线前」与「曲线后」混成一档回落
 *                  segs[0],播放头停在曲线尾端时会显示曲线开头的值,和耳朵对不上);
 *   · 段间空隙   → **前一段**值(引擎那边是「ramp 之前保持前段值」;显示层没有 ramp
 *                  模型,取前段即那一刻的稳态值);
 *   · 段表为空   → null(调用方回落参数面:还没分析过,曲线本就不存在)。
 *
 * ⚠ **已知偏差(不是本函数的 bug,记在这里免得下一个人当 bug 查)**:引擎打印头调的是
 * `CurveEvaluator::panAt`,它在段边界的 80ms 窗口内做 smoothstep 插值(ADR-010);而本
 * 函数返回的是**段常值**。于是段边界那 80ms,显示与写进宿主的值有偏差。显示层不建 ramp
 * 模型是刻意的(取那一刻的稳态值),分布图那边还有 rAF 补间兜着,肉眼几乎看不出。
 */
export function curveSegmentAt(segChannel, tS) {
    const segs = ((segChannel && segChannel.segments) || []).filter(Boolean);
    if (!segs.length) return null;
    const t = num(tS, 0);
    const first = segs[0];
    const last = segs[segs.length - 1];
    // 首段之前
    if (t < num(first.t0S, 0)) return first;
    // 末段之后(t1 <= t0 = 开放尾段,那就没有「之后」)
    const lastT0 = num(last.t0S, 0);
    const lastT1 = num(last.t1S, 0);
    if (lastT1 > lastT0 && t >= lastT1) return last;
    // 段内 / 段间空隙:落在第 i 段内取第 i 段;落在 i 与 i+1 之间的空隙取第 i 段
    let hit = first;
    for (const s of segs) {
        const t0 = num(s.t0S, 0);
        const t1 = num(s.t1S, 0);
        if (t >= t0 && (t < t1 || !(t1 > t0))) return s; // 段内(含开放尾段)
        if (t >= t0) hit = s; // 已越过本段 ⇒ 暂记为「前一段」
    }
    return hit;
}

/**
 * 一条轨的读回真源:一次算出 pan / vol **两维各自该读的段**;`null` = 该维回落参数面。
 *
 * [SL-241] 这条优先级链原先只长在 `rowFromStore` 里,分布图那边压根没有 —— 抽出来之后
 * 两处调同一个函数,再想分叉得先改这里。口径逐条即 J78「显示的是该维度的权威」:
 *   · **冻结**维度 → 参数面(宿主自动化 / 冻结手动值当家),不看段表([J85]);
 *   · 有**手动常值段** → 该段(05 §2.2「读回值同样取自该段」)。**这一档不看输出档** ——
 *     手动接管写的是曲线真身,ON/OFF 两边听到的都是它;
 *   · 否则输出 **ON**(引擎按曲线驱动)→ 播放头所处的曲线段(SL-211 复审终轮③a 裁定);
 *   · 否则输出 **OFF**(跟随宿主)→ 参数面 —— 这一档显示曲线就成了「看着曲线、听着宿主」;
 *   · 段表为空(还没分析过)→ 两维都 `null`,回落参数面。
 *
 * 为什么一次算两维、而不是每维调一次:`frozen` 只决定「用不用」,不影响算出来**是哪一段**。
 * 逐维调会把 `manualConstantOf` + `curveSegmentAt` 全量重算一遍(后者每次还新分配一个
 * `filter` 数组),而调用方是 25Hz 的整页 render × 15 轨(#159 复审【建议】2)。
 *
 * 同时把命中的**手动常值段**一并回出(`manual`)。Tab2 的行上还要一个「手动接管」标,
 * 调用方自己再调一次 `manualConstantOf` 的话不只是白算 —— 标和链有可能分头改到分家。
 * 回出来之后,那个标与这条链**必然**同一个判定(#159 复审第三轮)。
 */
export function readbackSegsOf(segChannel, bits, outputOn, timeS) {
    const manual = manualConstantOf(segChannel);
    const seg = manual || (outputOn ? curveSegmentAt(segChannel, timeS) : null);
    const frozen = bits || {};
    return {
        pan: frozen.pan ? null : seg,
        vol: frozen.vol ? null : seg,
        manual,
    };
}
