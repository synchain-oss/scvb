// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · 播放头层(rAF 插值 + 降级;可复用件;T33 Wave 1 交付)。
// -----------------------------------------------------------------------------
// 05 §2.3(315):「竖线,rAF 插值平滑;采集中在头部追加绿色进度点」;
// 05 §6.1(672-673):播放头属动态层;降级序列第①档 = 播放头/电平层降 30Hz;
// 空闲(未播放)零 rAF。数据源 = 契约 §2.6 `scvb.playhead`(30 Hz)。
//
// **复用契约(T43 轨迹图)**:本模块零 DOM 查询 —— 写入面由调用方注入
// (`apply(tS, isPlaying)`,T33 里写 .wave-playhead 的位置,T43 写自己的图元)。
// 插值是纯函数(node 可断言):两帧事件之间按到达时间线性外推,倒带/跳转
// (目标比外推值早)直接跟随不回弹。
// =============================================================================

/** 降级档下的写入周期(ms;05 §6.1 ①「降 30Hz」)。 */
export const DEGRADED_PERIOD_MS = 33;

/** 外推上限(ms):事件断流(暂停/失联)时最多再走这么远,防播放头飞出。 */
export const EXTRAPOLATE_CAP_MS = 200;

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

/**
 * rAF 插值(纯函数):以最近一帧事件 `{timeS, isPlaying}` 与其到达时刻为基线,
 * 播放中按墙钟线性外推(封顶 EXTRAPOLATE_CAP_MS),停住则原地。
 * @param {{timeS:number, isPlaying:boolean}|null} ev 最近事件
 * @param {number} evAtMs 事件到达时刻(performance.now() 系)
 * @param {number} nowMs 当前帧时刻
 * @returns {number} 插值后的播放头位置(秒)
 */
export function interpolate(ev, evAtMs, nowMs) {
    if (!ev) return 0;
    const t = num(ev.timeS, 0);
    if (!ev.isPlaying) return t;
    const dt = Math.min(
        Math.max(num(nowMs, 0) - num(evAtMs, 0), 0),
        EXTRAPOLATE_CAP_MS,
    );
    return t + dt / 1000;
}

/**
 * 播放头渲染器。
 * @param {object} opts
 * @param {(tS:number, playing:boolean) => void} opts.apply 每帧写入面
 * @param {() => number} [opts.degradeLevel] 当前降级档(layers.js governor.level)
 * @returns {{push:(ev:object)=>void, stop:()=>void, tick:(nowMs:number)=>void,
 *            valueAt:(nowMs:number)=>number, running:()=>boolean}}
 */
export function createPlayhead(opts) {
    const o = opts || {};
    const apply = typeof o.apply === "function" ? o.apply : () => {};
    const degradeLevel =
        typeof o.degradeLevel === "function" ? o.degradeLevel : () => 0;
    let ev = null;
    let evAtMs = 0;
    let raf = 0;
    let lastWriteMs = -1;

    function valueAt(nowMs) {
        return interpolate(ev, evAtMs, nowMs);
    }

    /** 单帧写入(降级档 ≥1 时限 30Hz —— 两次写入间隔 < 33ms 的帧跳过)。 */
    function tick(nowMs) {
        if (!ev) return; // 首帧事件前无位置可写(调用方的写入面保持初始态)
        if (
            degradeLevel() >= 1 &&
            lastWriteMs >= 0 &&
            nowMs - lastWriteMs < DEGRADED_PERIOD_MS
        ) {
            return;
        }
        lastWriteMs = nowMs;
        apply(valueAt(nowMs), !!(ev && ev.isPlaying));
    }

    function loop(ts) {
        raf = 0;
        tick(ts);
        // 空闲零 rAF:停止播放后写完最后一帧即自停(§6.1)
        if (ev && ev.isPlaying) raf = requestAnimationFrame(loop);
    }

    return {
        /** §2.6 事件入口(30 Hz);播放中自动起 rAF,停下写一帧后自停。 */
        push(next) {
            ev = next || null;
            evAtMs = typeof performance !== "undefined" ? performance.now() : 0;
            if (typeof requestAnimationFrame !== "function") return;
            if (!raf) raf = requestAnimationFrame(loop);
        },
        stop() {
            if (raf && typeof cancelAnimationFrame === "function") {
                cancelAnimationFrame(raf);
            }
            raf = 0;
            ev = null;
            lastWriteMs = -1;
        },
        tick,
        valueAt,
        running: () => raf !== 0,
    };
}
