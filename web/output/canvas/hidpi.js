// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · DPR / uiScale 后备存储管理(可复用件;T33 Wave 1 交付)。
// -----------------------------------------------------------------------------
// 05 §6.1(671)逐字:`k = state.uiScale * devicePixelRatio`;
//   `canvas.width = cssW * k; canvas.height = cssH * k; ctx.setTransform(k,0,0,k,0,0)`;
// 缩放档位变化与 dpr 变化(`matchMedia('(resolution)')`)都触发重建后备存储。
// 命中测试用 CSS 坐标(getBoundingClientRect + zoom 校正)—— 见 web/shared/hit.js。
//
// **复用契约(T34 / T43 必用,两侧不得各写一份)**:
//   • `backingScale(uiScale, dpr)` —— 纯函数,node 可断言;
//   • `resizeCanvas(canvas, cssW, cssH, k)` —— 重建后备存储并回 2D ctx(已 setTransform,
//     之后一律按 CSS px 作画);尺寸没变时**不重建**(canvas.width 赋值即清屏);
//   • `observeResolution(cb)` —— dpr 变化监听(缩放显示器 / 拖到另一块屏)。
// =============================================================================

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

/**
 * 后备存储倍率上限。真机 dpr ≤ 3(uiScale ≤ 1.25 ⇒ k ≤ 3.75),k=4 已覆盖全部
 * 交付档位;再往上只会把位图面积按 k² 涨上去 —— 15 泳道共享动态层在 k=12 时是
 * 11148×6120(约 273 MB 后备存储),分配失败后 canvas 静默空掉,整片泳道渲染成
 * 浅色(Wave 3 无头亲验的高倍复现)。05 §6.1 的片段没给上限,这里补一道封顶:
 * 超过 4 的部分按 4 画再由浏览器上采样,**画得糙好过画不出来**。
 */
export const MAX_BACKING_SCALE = 4;

/**
 * 后备存储倍率 k = uiScale × dpr(05 §6.1 逐字)。
 * 两参皆容错:非法值按 1;下夹 0.1(0/负值会让 canvas 尺寸为 0 并抛),
 * 上夹 MAX_BACKING_SCALE。
 */
export function backingScale(uiScale, dpr) {
    const k = num(uiScale, 1) * num(dpr, 1);
    if (!(k > 0.1)) return 0.1;
    return k > MAX_BACKING_SCALE ? MAX_BACKING_SCALE : k;
}

/**
 * 重建后备存储并返回已 setTransform 的 2D 上下文。
 * @param {HTMLCanvasElement} canvas
 * @param {number} cssW CSS px 宽
 * @param {number} cssH CSS px 高
 * @param {number} k backingScale() 的结果
 * @returns {CanvasRenderingContext2D|null} 尺寸非法/取不到 ctx 时 null
 */
export function resizeCanvas(canvas, cssW, cssH, k) {
    if (!canvas || !(cssW > 0) || !(cssH > 0)) return null;
    const w = Math.max(1, Math.round(cssW * k));
    const h = Math.max(1, Math.round(cssH * k));
    // 尺寸未变不重建:给 canvas.width 赋同值也会清空位图(平移 blit 的老底就没了)
    if (canvas.width !== w) canvas.width = w;
    if (canvas.height !== h) canvas.height = h;
    // CSS 尺寸必须**显式写死**:canvas 的 width/height 属性会作为表现层提示映射到
    // CSS width/height,与 `left+right` / `top+bottom` 双向定位互相过约束 —— dpr>1
    // 时 right/bottom 被丢弃,位图按后备像素数当 CSS px 摊开,泳道整体放大 dpr 倍
    // 再被 overflow 裁掉(Wave 3 无头亲验实拍;05 §6.1 的片段没写这两行)。
    const cw = `${cssW}px`;
    const ch = `${cssH}px`;
    if (canvas.style) {
        if (canvas.style.width !== cw) canvas.style.width = cw;
        if (canvas.style.height !== ch) canvas.style.height = ch;
    }
    const ctx = canvas.getContext ? canvas.getContext("2d") : null;
    if (ctx) ctx.setTransform(k, 0, 0, k, 0, 0);
    return ctx;
}

/**
 * dpr 变化监听(05 §6.1:`matchMedia('(resolution)')` 触发重建)。
 * 每次命中后按新 dpr **重挂**媒体查询(resolution 查询是一次性的边界匹配)。
 * @param {() => void} cb 变化回调(调用方在里面重建后备存储并重绘)
 * @returns {() => void} 取消函数(空环境下为 no-op)
 */
export function observeResolution(cb) {
    if (typeof matchMedia !== "function" || typeof cb !== "function") {
        return () => {};
    }
    let mql = null;
    let disposed = false;
    const arm = () => {
        if (disposed) return;
        const dpr = typeof devicePixelRatio === "number" ? devicePixelRatio : 1;
        mql = matchMedia(`(resolution: ${dpr}dppx)`);
        mql.addEventListener("change", onChange, { once: true });
    };
    const onChange = () => {
        cb();
        arm();
    };
    arm();
    return () => {
        disposed = true;
        if (mql) mql.removeEventListener("change", onChange);
    };
}
