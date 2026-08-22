// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · 分层 canvas 骨架 + 帧时监测 + 三档降级(可复用件;T33 Wave 1 交付)。
// -----------------------------------------------------------------------------
// 05 §6.1(672-673)逐字落地:
//   • 分层:静态底层(网格/波形,缓存位图,只在数据或视口变化时重绘)+
//     动态层(曲线/手柄/播放头/选区,rAF);
//   • 60fps 目标与降级:`performance.now()` 滚动平均帧时,连续 >20ms → 降级序列:
//       ① 播放头/电平层降 30Hz
//       ② 波形平移改「先位图平移 blit,停手后异步重取重绘」
//       ③ 光晕/阴影在 canvas 内的模糊效果关闭
//   • 空闲(无交互无播放)时**零 rAF**;
//   • 静态层单帧重绘 <8ms 预算意识(§6.3;超预算走上面的序列)。
//
// **复用契约(T34 曲线编辑器 / T43 轨迹图)**:
//   • `createFrameGovernor()` —— 纯计算(滚动平均 + 档位推进/回落),node 可断言;
//   • `createLayerStack()` —— rAF 驱动骨架:注册动态绘制回调、脏标记静态层;
//     调用方只提供「画什么」,循环、帧时账与降级档位由这里管。
// =============================================================================

/** 帧时滚动平均的触发阈(ms;05 §6.1「连续 >20ms → 降级」)。 */
export const FRAME_BUDGET_MS = 20;

/** 静态层单帧重绘预算(ms;05 §6.3)。 */
export const STATIC_REDRAW_BUDGET_MS = 8;

/** 滚动平均窗口(帧数)。 */
export const FRAME_WINDOW = 30;

/**
 * 降级序列(05 §6.1 三档,顺序即档位;index+1 = 档位号):
 *   1 `halfRatePlayhead` —— 播放头/电平层降 30Hz;
 *   2 `blitPan`          —— 平移期间先位图 blit,停手后异步重取重绘;
 *   3 `noCanvasBlur`     —— canvas 内光晕/阴影模糊关闭。
 */
export const DEGRADE_SEQUENCE = Object.freeze([
    "halfRatePlayhead",
    "blitPan",
    "noCanvasBlur",
]);

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

/**
 * 帧时账 —— 滚动平均 + 三档降级档位(纯计算,与 rAF 解耦)。
 * 档位推进:窗口滑满且平均 >20ms → 升一档并**清窗重测**(不清窗的话同一批慢帧
 * 会把三档一口气升满);回落:平均 <14ms(阈值 × 0.7 滞回)→ 降一档。
 * @returns {{push:(frameMs:number)=>number, level:()=>number,
 *            average:()=>number, modes:()=>string[], reset:()=>void}}
 */
export function createFrameGovernor() {
    const win = [];
    let sum = 0;
    let level = 0;

    function average() {
        return win.length ? sum / win.length : 0;
    }

    function push(frameMs) {
        const ms = num(frameMs, 0);
        win.push(ms);
        sum += ms;
        if (win.length > FRAME_WINDOW) sum -= win.shift();
        if (win.length === FRAME_WINDOW) {
            const avg = average();
            if (avg > FRAME_BUDGET_MS && level < DEGRADE_SEQUENCE.length) {
                level += 1;
                win.length = 0;
                sum = 0;
            } else if (avg < FRAME_BUDGET_MS * 0.7 && level > 0) {
                level -= 1;
                win.length = 0;
                sum = 0;
            }
        }
        return level;
    }

    return {
        push,
        level: () => level,
        average,
        /** 当前生效的降级模式名(档位 N = 序列前 N 项全部生效)。 */
        modes: () => DEGRADE_SEQUENCE.slice(0, level),
        reset: () => {
            win.length = 0;
            sum = 0;
            level = 0;
        },
    };
}

/**
 * 分层骨架:静态层(脏标记重绘)+ 动态层(rAF 逐帧)+ 帧时账。
 *
 * 空闲零 rAF 的口径:`start()` 只在有活跃诉求(交互 / 播放 / 脏静态层)时被调,
 * 动态回调返回 `false` 且静态层不脏时循环**自停**;下一次事件再 `start()`。
 *
 * @param {object} opts
 * @param {(ctx:{level:number, modes:string[]}) => void} [opts.drawStatic]
 *        静态层重绘(仅脏帧调用;耗时计入帧时账,超 8ms 预算由降级序列兜)
 * @param {(nowMs:number, ctx:{level:number, modes:string[]}) => boolean} [opts.drawDynamic]
 *        动态层逐帧回调;返回 true = 还有活跃诉求(继续 rAF),false = 可停
 * @returns {{start:()=>void, stop:()=>void, invalidateStatic:()=>void,
 *            running:()=>boolean, governor:object, tick:(nowMs:number)=>void}}
 */
export function createLayerStack(opts) {
    const o = opts || {};
    const governor = createFrameGovernor();
    let staticDirty = true;
    let raf = 0;
    let lastMs = null;

    /** 单帧推进(导出给 node 侧驱动;rAF 循环也走它)。返回「是否还要下一帧」。 */
    function tick(nowMs) {
        const t0 =
            typeof performance !== "undefined" ? performance.now() : nowMs;
        const frame = { level: governor.level(), modes: governor.modes() };
        if (staticDirty) {
            staticDirty = false;
            if (typeof o.drawStatic === "function") o.drawStatic(frame);
        }
        let live = false;
        if (typeof o.drawDynamic === "function") {
            live = o.drawDynamic(nowMs, frame) === true;
        }
        const t1 =
            typeof performance !== "undefined" ? performance.now() : nowMs;
        // 帧时 = 本帧绘制耗时与帧间隔的较大者(绘制便宜但帧被别人拖长同样要降级)
        const gap = lastMs === null ? 0 : nowMs - lastMs;
        lastMs = nowMs;
        governor.push(Math.max(t1 - t0, gap > 0 ? gap : 0));
        return live || staticDirty;
    }

    function loop(ts) {
        raf = 0;
        if (tick(ts)) {
            raf = requestAnimationFrame(loop);
        } else {
            lastMs = null; // 空闲零 rAF:自停,时间基线清掉
        }
    }

    return {
        start() {
            if (raf || typeof requestAnimationFrame !== "function") return;
            lastMs = null;
            raf = requestAnimationFrame(loop);
        },
        stop() {
            if (raf && typeof cancelAnimationFrame === "function") {
                cancelAnimationFrame(raf);
            }
            raf = 0;
            lastMs = null;
        },
        /** 静态层脏标记(数据/视口变化时调;下一帧重绘一次)。 */
        invalidateStatic() {
            staticDirty = true;
            this.start();
        },
        running: () => raf !== 0,
        governor,
        tick,
    };
}
