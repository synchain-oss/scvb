// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · 电平弹道(rAF)—— Tab2 十五根玻璃管的液柱与峰线。
// -----------------------------------------------------------------------------
// **T32 Wave 2:弹道与渲染循环已实现**(Wave 1 只落了常数与工厂桩)。
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

/** 通道数(契约 §0.2 第 4 条:ch = 1..15,J59)。 */
export const METER_CHANNELS = 15;

/** 一轨的复位态(停止态归零 = 全部落到地板,口径④)。 */
export function restState() {
    return { db: METER_FLOOR_DB, peakDb: METER_FLOOR_DB, peakHeldMs: 0 };
}

function floorDb(v) {
    return v < METER_FLOOR_DB ? METER_FLOOR_DB : v > 0 ? 0 : v;
}

/**
 * 单轨一帧的弹道推进(纯函数,node 侧可断言;rAF 循环逐轨调它)。
 *
 * 四条口径逐条落地(顺序即依赖顺序,换序会算错 peak-hold):
 *   ① 上行**瞬时跟随**:`target >= prev.db` 直接取 target(不做 attack 平滑);
 *   ② 下行 fast-follow:每毫秒最多落 `FALL_DB_PER_S / 1000` dB,且不穿地板;
 *   ③ peak:被本帧超越 ⇒ 顶到新值并把保持计时**清零**;否则累加计时,
 *      超过 `PEAK_HOLD_MS` 之后按 `PEAK_DECAY_DB_PER_S` 衰减;
 *   ④ peak 永不低于当前液柱(峰线画在液柱头部之前是视觉错误),且同样不穿地板。
 *
 * @param {{db:number, peakDb:number, peakHeldMs:number}} prev 上一帧状态
 * @param {number} targetDb 本帧事件值(dB;契约 §2.5 `tracks[].db`)
 * @param {number} dtMs 距上一帧的毫秒数
 * @param {number} [targetPeakDb] 本帧事件峰值(契约 §2.5 `tracks[].peakDb`);
 *        省略时退化为「用液柱值自算峰值」——两种喂法弹道一致(**可选参数,只增不改**)。
 * @returns {{db:number, peakDb:number, peakHeldMs:number}}
 */
export function advance(prev, targetDb, dtMs, targetPeakDb) {
    const base = prev || restState();
    const dt = Number.isFinite(dtMs) && dtMs > 0 ? dtMs : 0;
    const target = floorDb(
        Number.isFinite(targetDb) ? targetDb : METER_FLOOR_DB,
    );
    const peakTarget = floorDb(
        Number.isFinite(targetPeakDb) ? targetPeakDb : target,
    );

    // ①② 液柱
    const prevDb = Number.isFinite(base.db) ? base.db : METER_FLOOR_DB;
    const db =
        target >= prevDb
            ? target
            : Math.max(target, prevDb - (FALL_DB_PER_S * dt) / 1000);

    // ③ 峰值保持 / 衰减
    const prevPeak = Number.isFinite(base.peakDb)
        ? base.peakDb
        : METER_FLOOR_DB;
    let peakDb = prevPeak;
    let peakHeldMs = Number.isFinite(base.peakHeldMs) ? base.peakHeldMs : 0;
    if (peakTarget >= prevPeak) {
        peakDb = peakTarget;
        peakHeldMs = 0;
    } else {
        peakHeldMs += dt;
        if (peakHeldMs > PEAK_HOLD_MS) {
            // 只对「超出保持期的那部分时间」计衰减,否则跨过阈值的那一帧会多衰减一截
            const decayMs = Math.min(dt, peakHeldMs - PEAK_HOLD_MS);
            peakDb = prevPeak - (PEAK_DECAY_DB_PER_S * decayMs) / 1000;
        }
    }

    // ④ 下限:地板 + 不低于液柱
    return {
        db: floorDb(db),
        peakDb: floorDb(Math.max(peakDb, db)),
        peakHeldMs,
    };
}

/**
 * 弹道渲染器工厂。
 *
 * 写入面 = 每帧给 15 行的 `.sc-tube__liquid` / `.sc-tube__peak` 写
 * `--lv` / `--pk` / `data-alert` 三件。**DOM 节点在 attach() 时一次性缓存**——
 * 15 行 × 60 fps 下,任何逐帧 querySelector 或 getBoundingClientRect() 都会把
 * 主线程拖垮(这也是本文件不做任何布局读的原因)。
 *
 * @param {object} opts
 * @param {Element} opts.body   #tracks-body(15 行的容器)
 * @param {() => object} opts.getStore  事件仓(读 scvb.playhead 判停止态)
 * @param {() => boolean} [opts.isActive] Tab2 是否为当前激活页;返回 false 的帧**不写 DOM**
 *        (轨道页不在前台时,每帧给 15 行写属性纯属白烧;省略 = 一律活跃,node 侧不受影响)
 * @returns {{start:()=>void, stop:()=>void, push:(m:object)=>void,
 *           attach:()=>void, tick:(nowMs:number)=>void, stateOf:(ch:number)=>object}}
 */
export function createMeterRenderer(opts) {
    const o = opts || {};
    const getStore = o.getStore || (() => ({}));
    const isActive = typeof o.isActive === "function" ? o.isActive : () => true;
    /** 逐轨弹道状态(下标 0 = ch1)。 */
    const tracks = Array.from({ length: METER_CHANNELS }, restState);
    /** 最近一帧 `scvb.meters`(30 Hz 到达;rAF 以 60 fps 在两帧之间继续推弹道)。 */
    let latest = null;
    let nodes = [];
    let raf = 0;
    let lastMs = 0;

    /** 缓存 15 行的液柱/峰线节点(mount 之后、start 之前调一次)。 */
    function attach() {
        const body = o.body || null;
        nodes = [];
        if (!body || typeof body.querySelector !== "function") return;
        for (let ch = 1; ch <= METER_CHANNELS; ch++) {
            const tube = body.querySelector(
                `[data-gb="tracks-row-${ch}-vol-tube"]`,
            );
            nodes.push(
                tube
                    ? {
                          liquid: tube.querySelector(".sc-tube__liquid"),
                          peak: tube.querySelector(".sc-tube__peak"),
                      }
                    : null,
            );
        }
    }

    /** 30 Hz 事件入口(契约 §2.5);只存不画,画在 rAF 里。 */
    function push(m) {
        if (m && Array.isArray(m.tracks)) latest = m.tracks;
    }

    /** 停止态(`scvb.playhead.isPlaying === false`)复位归零,不留残影(口径④)。 */
    function isStopped() {
        const ph = getStore().playhead;
        return !!ph && ph.isPlaying === false;
    }

    function writeRow(i, st) {
        const n = nodes[i];
        if (!n) return;
        const lv = dbToRatio(st.db);
        const pk = dbToRatio(st.peakDb);
        if (n.liquid)
            n.liquid.style.setProperty("--lv", (lv * 100).toFixed(1) + "%");
        if (n.peak) {
            // pk=0 时峰线隐(裁定 C19,照设计稿):地板电平不画一条贴左缘的白线
            const hide = pk <= 0;
            if (n.peak.hidden !== hide) n.peak.hidden = hide;
            n.peak.style.setProperty("--pk", (pk * 100).toFixed(1) + "%");
            const alert = pk > PEAK_ALERT_RATIO ? "1" : "0";
            if (n.peak.getAttribute("data-alert") !== alert) {
                n.peak.setAttribute("data-alert", alert);
            }
        }
    }

    /** 单帧推进(导出到返回对象上,便于 node 侧不起 rAF 也能驱动)。 */
    function tick(nowMs) {
        const dt = lastMs ? nowMs - lastMs : 0;
        lastMs = nowMs;
        // 非激活 tab:液柱不可见,整帧早退(lastMs 已推进,回到前台的第一帧 dt 仍正常)
        if (!isActive()) return;
        const stopped = isStopped();
        for (let i = 0; i < METER_CHANNELS; i++) {
            if (stopped) {
                tracks[i] = restState();
            } else {
                const m = latest ? latest[i] : null;
                tracks[i] = advance(
                    tracks[i],
                    m ? m.db : METER_FLOOR_DB,
                    dt,
                    m ? m.peakDb : undefined,
                );
            }
            writeRow(i, tracks[i]);
        }
    }

    function start() {
        if (raf || typeof requestAnimationFrame !== "function") return;
        if (!nodes.length) attach();
        lastMs = 0;
        const loop = (ts) => {
            raf = requestAnimationFrame(loop);
            tick(ts);
        };
        raf = requestAnimationFrame(loop);
    }

    function stop() {
        if (raf && typeof cancelAnimationFrame === "function") {
            cancelAnimationFrame(raf);
        }
        raf = 0;
        latest = null;
        for (let i = 0; i < METER_CHANNELS; i++) {
            tracks[i] = restState();
            writeRow(i, tracks[i]);
        }
    }

    return {
        start,
        stop,
        push,
        attach,
        tick,
        stateOf: (ch) => tracks[ch - 1],
    };
}
