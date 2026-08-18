// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · 电平弹道(rAF)—— Tab2 十五根玻璃管的液柱与峰线。
// -----------------------------------------------------------------------------
// **本文件是 T32 Wave 1 的空壳:只落弹道常数与工厂桩,渲染循环归 Wave 2。**
// 之所以 Wave 1 就把它切出来:弹道常数是**跨实现共享的契约级数字**(Bridge 与主站的
// peak-hold.ts 用同一组),先立文件、先立常数,免得 Wave 2 写手在 tab-tracks.js 里
// 就地拍脑袋写 0.18s CSS 过渡当弹道 —— 那是设计稿静态稿的帧间平滑,不是弹道。
//
// 真源:
//   • 契约 docs/SCVB_CONTRACT.md §2.5 `scvb.meters`(30 Hz,每轨 db/peakDb,地板 -60 dB);
//   • 05-ui-spec §2.2「弹道常数与 Bridge 完全一致」+ tokens.css 第 16 组注释(347-348 行);
//   • research/02 §2.7(fast-follow / peak-hold 数值的来处)。
//
// 弹道口径(四条,缺一条都会和 Bridge 表对不上):
//   ① 地板 -60 dB —— 低于地板一律按地板画(未连接/静音轨发的就是地板值);
//   ② 上行**瞬时跟随**(不做 attack 平滑),下行 fast-follow **120 dB/s**;
//   ③ 峰值保持 **2200 ms**,到点后按 **20 dB/s** 衰减;
//   ④ 停止态(scvb.playhead.isPlaying === false)复位归零,不留残影。
// CSS 侧 `.sc-tube__liquid` 的 `transition: width var(--dur-meter) linear`(.18s)保留,
// 作用是 30 Hz 事件之间的**帧间平滑**,与本弹道叠加不冲突(两者一个管取值、一个管补间)。
//
// 渲染面:Tab2 的液柱/峰线是 DOM(玻璃管四层,base.css `.sc-tube*`),不是 canvas ——
// 本文件放在 canvas/ 目录下是因为它与 T33 的波形 canvas 同属「rAF 驱动的绘制层」,
// 写入方式是每帧给 15 个 `.sc-tube` 写 `--lv` / `--pk` / `data-alert` 三个属性。
// =============================================================================

/** 电平地板(契约 §2.5:未连接/静音轨发 -60 dB)。 */
export const METER_FLOOR_DB = -60;

/** 回落速度:fast-follow 120 dB/s(上行不平滑,瞬时跟随)。 */
export const FALL_DB_PER_S = 120;

/** 峰值保持时长(ms)。 */
export const PEAK_HOLD_MS = 2200;

/** 峰值保持到点后的衰减速度(dB/s)。 */
export const PEAK_DECAY_DB_PER_S = 20;

/** 峰线转警戒红的阈值(比例,05 §2.2:peak > .86)。 */
export const PEAK_ALERT_RATIO = 0.86;

/** dB → 0..1 比例(地板 -60 dB 映到 0,0 dB 映到 1)。 */
export function dbToRatio(db) {
    const v = Number.isFinite(db) ? db : METER_FLOOR_DB;
    const r = (v - METER_FLOOR_DB) / -METER_FLOOR_DB;
    return r < 0 ? 0 : r > 1 ? 1 : r;
}

/**
 * 单轨一帧的弹道推进(纯函数,node 侧可断言;Wave 2 的 rAF 循环逐轨调它)。
 * @param {{db:number, peakDb:number, peakHeldMs:number}} prev 上一帧状态
 * @param {number} targetDb 本帧事件值(dB)
 * @param {number} dtMs 距上一帧的毫秒数
 * @returns {{db:number, peakDb:number, peakHeldMs:number}}
 */
export function advance(prev, targetDb, dtMs) {
    // [T32 Wave 2] 实现:①上行取 max(瞬时)②下行按 FALL_DB_PER_S 限速
    // ③ peakDb 被超越则重置 peakHeldMs=0,否则累加;超过 PEAK_HOLD_MS 后按
    // PEAK_DECAY_DB_PER_S 衰减 ④ 全部结果对 METER_FLOOR_DB 取下限。
    void targetDb;
    void dtMs;
    return prev;
}

/**
 * 弹道渲染器工厂(Wave 1 空壳)。
 * @param {object} opts
 * @param {Element} opts.body   #tracks-body(15 行的容器)
 * @param {() => object} opts.getStore  事件仓(读 scvb.meters / scvb.playhead)
 * @returns {{start:()=>void, stop:()=>void, push:(m:object)=>void}}
 */
export function createMeterRenderer(opts) {
    void opts;
    // [T32 Wave 2] start():requestAnimationFrame 循环;push(m):把 30 Hz 的
    // scvb.meters 写进逐轨状态;stop():取消 rAF 并把 15 行的 --lv/--pk 归零
    // (停止态复位,口径④)。**不在这里做 DOM 查询缓存以外的任何布局读**——
    // 15 行 × 60 fps 下,任何 getBoundingClientRect() 都会把主线程拖垮。
    return {
        start() {},
        stop() {},
        push() {},
    };
}
