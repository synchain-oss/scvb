// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// host-tempo.js —— [J147] Tab1 手动范围「按小节显示」用的宿主速度模型(纯函数,node 可直接 import)
// =============================================================================
// 数据来源 = 契约 §2.6 `scvb.playhead` 的四个可选字段 `bpm` / `timeSigNum` / `timeSigDen` / `ppq`:
// 宿主 AudioPlayHead 在**当前播放位置**报的速度、拍号、拍位置(四分音符数),`ppq` 与同一帧的
// `timeS` 是同一块的一对。插件读不到宿主的整张速度表 —— VST3 只给「此刻」—— 所以本模块回答的
// 问题是「拿手上这些点,某个秒数落在第几小节第几拍,这个答案靠不靠得住」:
//
//   · **一帧速度都没收到过** ⇒ 没有小节可言,页面按秒显示并明说(`master.rangeSecondsNote`);
//     收到过之后宿主某几帧不再给(比如停带时不报),沿用最后一次读到的值(J147)。
//   · **本次会话没见过速度变化** ⇒ 按最近一次的 bpm 与拍位置线性换算,算精确值。
//   · **本次会话见过速度变化**(bpm 变了;或「拍位置 − 秒 × bpm / 60」这个偏移变了 —— 同一个
//     bpm 下偏移变了,说明两处之间夹着一段别的速度;第一帧偏移就不为 0 同理)⇒ 离开走带经过的
//     地方就是估算值(用最近一次的 bpm 外推,J147「变速用最近 BPM 估算」)。走带经过时每
//     `ANCHOR_BUCKET_S` 秒留一个锚点(宿主报的 秒 ↔ 拍位置),落在锚点 `CALIBRATE_WINDOW_S`
//     之内的端点按那个锚点换算,算「已校准」—— 这就是「播放该区域后校准」。
//   · **本次会话见过拍号变化** ⇒ 小节号取决于整段拍号历史,插件读不到,**播放也校准不了**,
//     一律按估算显示,提示换成 `master.barsMeterNote`(不说「播放后校准」,那句话此时不成立)。
//   · **用户在宿主里改了速度表**(PR #325 复审①②):此前的锚点与「变没变过」的判断都是对着
//     旧速度表攒的,**整份作废、从这一帧重新观察**(拍号变化那一位除外:它说的是小节号的历史,
//     改完速度表也不会因此变准)。不作废的话,落在旧锚点旁边的端点会被判「已校准」,显示的却是
//     按旧速度算的小节号 —— 标成精确的错值。认「改过」的三条(`tempoMapEdited`):
//       ① 停在同一个时刻,宿主却报出了不同的速度(按秒钉住播放头的宿主;同一时刻拍位置变了
//          的那一半由 ③ 认出);
//       ② 拍位置没动、秒位置变了(按拍钉住播放头的宿主:改速度后同一拍落到了别的秒上);
//       ③ 新帧落在某个旧锚点 `CALIBRATE_WINDOW_S` 之内,按那个锚点推出的拍位置与宿主报的对不上,
//          且差值超出「两点之间至多一次变速」能解释的范围(见 `edgeTolQn`)—— 改动在播放头之后、
//          播放头随后经过或停到那里时由这一条认出。
//     看不到的一类照实说:改动落在本次会话**没留下锚点**的地方、而停着那一点的拍位置与速度都
//     没变时,插件无从得知 —— 那里本来也只有按最近速度的外推。
//
// 锚点表只在内存里(不进 state、不过桥),上限 `MAX_ANCHORS` 个,超了先丢最久没更新的。
// 关掉插件窗口再打开 = 新页面 = 从头观察(与 04 §2.3「仅内存缓存」同口径)。
// =============================================================================

/** 锚点桶宽(秒):走带经过时每个桶只留最后一帧。 */
export const ANCHOR_BUCKET_S = 0.25;
/** 端点离最近锚点不超过这么远,就按锚点换算、算「已校准」。 */
export const CALIBRATE_WINDOW_S = 0.25;
/** 锚点表上限(桶数):0.25 s 一桶 × 2 小时时间线。 */
export const MAX_ANCHORS = 28800;
/**
 * 「拍位置 − 秒 × bpm / 60」变化多少(四分音符)算「中间夹着别的速度」。
 * 恒速下它是常数,浮点误差在 1e-9 量级;0.01 拍 ≈ 120 BPM 下 5 ms。
 */
export const OFFSET_TOL_QN = 0.01;
/** 拍线吸附(秒):输入框只到毫秒,落在拍线上的端点经毫秒取整后会差出一丝,别让它掉到前一拍。 */
export const SNAP_S = 0.001;
/** 「停在同一个时刻」的判定宽度(秒):停带时宿主逐帧报的是同一个样本位置,秒值逐位相同。 */
export const SAME_SPOT_S = 1e-6;
/** 「拍位置没动」的判定宽度(四分音符):按拍钉住播放头的宿主改速度后报的是同一个拍位置。 */
export const SAME_PPQ_QN = 1e-6;

/** 空模型(页面启动时、以及测试里用)。 */
export function emptyTempo() {
    return {
        latest: null, // {bpm, num, den, ppq|null, timeS}:最近一次带速度的帧(给 bpm 与拍号)
        // 最近一次**带拍位置**的帧:外推的基点。与 `latest` 分开存(PR #325 复审②)——
        // 没有时间线的帧(C++ 侧不发 ppq)不该把基点从「宿主报的拍位置」换成「假设原点在 0 秒」。
        latestPpq: null,
        anchors: new Map(), // 桶号 → {timeS, ppq, bpm}
        tempoVaried: false,
        meterVaried: false,
        lastOffset: null, // 最近一次带 ppq 的帧的「ppq − timeS × bpm / 60」
    };
}

/**
 * 从一帧 `scvb.playhead` 里取速度字段;不全或不合法 ⇒ null(按「这一帧没给」处理)。
 * C++ 侧 `hostTempoOf` 已按同一口径筛过,这里是防御 —— 页面不信任何一帧载荷。
 */
export function readTempoFields(p) {
    if (!p || typeof p !== "object") return null;
    const bpm = p.bpm;
    const num = p.timeSigNum;
    const den = p.timeSigDen;
    if (!(Number.isFinite(bpm) && bpm > 0)) return null;
    if (!(Number.isInteger(num) && num >= 1)) return null;
    if (!(Number.isInteger(den) && den >= 1)) return null;
    const timeS = Number.isFinite(p.timeS) ? p.timeS : 0;
    const ppq = Number.isFinite(p.ppq) ? p.ppq : null;
    return { bpm, num, den, ppq, timeS };
}

function offsetOf(f) {
    return f.ppq - (f.timeS * f.bpm) / 60;
}

function bpmDiffers(a, b) {
    return Math.abs(a - b) > b * 1e-9;
}

/**
 * 锚点 a 推到新帧 f 的时刻,容许的最大偏差(四分音符):两点之间至多一次变速时,按两端速度的
 * 平均斜率外推,误差不超过 `|Δt| × |两端斜率差| / 2`;这里取整倍作余量,再加上 `OFFSET_TOL_QN`。
 * 真实工程里两次锚点之间(≤ 0.25 s)夹两次以上变速的情形不在它的保证范围内 —— 那时会多作废一次
 * (退回估算),方向是保守的。
 */
function edgeTolQn(a, f) {
    return (
        OFFSET_TOL_QN +
        (Math.abs(f.timeS - a.timeS) * Math.abs(f.bpm - a.bpm)) / 60
    );
}

/** 速度表被改过吗(三条判据见文件头)。 */
function tempoMapEdited(m, f) {
    if (f.ppq === null) return false;
    const last = m.latestPpq;
    if (last) {
        const sameTime = Math.abs(f.timeS - last.timeS) <= SAME_SPOT_S;
        const samePpq = Math.abs(f.ppq - last.ppq) <= SAME_PPQ_QN;
        // ① 按秒钉住:同一时刻,速度变了(同一时刻拍位置变了的那一半归 ③ —— 上一帧本身
        //    就是离它最近的锚点,③ 用 0 时差一推就对不上)
        if (sameTime && bpmDiffers(f.bpm, last.bpm)) return true;
        // ② 按拍钉住:同一拍位置,秒位置变了
        if (samePpq && !sameTime) return true;
    }
    // ③ 旁边的旧锚点推不出宿主此刻报的拍位置
    const a = nearestAnchor(m, f.timeS);
    if (a) {
        const pred = a.ppq + ((f.timeS - a.timeS) * (a.bpm + f.bpm)) / 2 / 60;
        if (Math.abs(f.ppq - pred) > edgeTolQn(a, f)) return true;
    }
    return false;
}

/**
 * 喂一帧 `scvb.playhead`。**就地**更新并返回同一个模型(锚点表是 Map,整份拷贝 30 次/秒不划算)。
 * 这一帧没带速度字段 ⇒ 什么都不动(沿用最后一次读到的值)。
 */
export function observeTempo(model, playhead) {
    const m = model || emptyTempo();
    const f = readTempoFields(playhead);
    if (!f) return m;
    const prev = m.latest;
    if (prev && (f.num !== prev.num || f.den !== prev.den))
        m.meterVaried = true;
    if (tempoMapEdited(m, f)) {
        // 旧观察整份作废,这一帧当第一帧重来(下面的偏移判定因此会拿它与 0 比)。
        m.anchors.clear();
        m.tempoVaried = false;
        m.lastOffset = null;
        m.latestPpq = null;
    } else if (prev && bpmDiffers(f.bpm, prev.bpm)) {
        m.tempoVaried = true;
    }
    if (f.ppq !== null) {
        const off = offsetOf(f);
        // 第一帧就与「从 0 秒起恒速」对不上 ⇒ 前面有过别的速度(或宿主的拍位置原点不在 0 秒)。
        // 两种都意味着「离开这里就不能按一条直线推」,一律按估算处理 —— 宁可多标一次估算。
        const ref = m.lastOffset === null ? 0 : m.lastOffset;
        if (Math.abs(off - ref) > OFFSET_TOL_QN) m.tempoVaried = true;
        m.lastOffset = off;
        const key = Math.floor(f.timeS / ANCHOR_BUCKET_S);
        m.anchors.delete(key); // 先删再放:Map 按插入序,这样「最久没更新的」恒在最前
        m.anchors.set(key, { timeS: f.timeS, ppq: f.ppq, bpm: f.bpm });
        while (m.anchors.size > MAX_ANCHORS) {
            m.anchors.delete(m.anchors.keys().next().value);
        }
        m.latestPpq = f;
    }
    m.latest = f;
    return m;
}

/** 有没有可用的速度(没有 ⇒ 页面按秒显示)。 */
export function hasTempo(model) {
    return !!(model && model.latest);
}

function nearestAnchor(m, t) {
    const k = Math.floor(t / ANCHOR_BUCKET_S);
    let best = null;
    let bestD = Infinity;
    for (let i = k - 1; i <= k + 1; i++) {
        const a = m.anchors.get(i);
        if (!a) continue;
        const d = Math.abs(a.timeS - t);
        if (d < bestD) {
            best = a;
            bestD = d;
        }
    }
    return best && bestD <= CALIBRATE_WINDOW_S ? best : null;
}

/**
 * 秒 → 拍位置(四分音符数)。无速度 ⇒ null。
 * @returns {{qn:number, bpm:number, exact:boolean}|null}
 *   `bpm` = 换算这一点所用的速度(±4 小节按它折成秒);`exact` = 这个值靠不靠得住(见文件头)。
 */
export function qnAt(model, t) {
    if (!hasTempo(model)) return null;
    const m = model;
    const tt = Number(t) || 0;
    const near = nearestAnchor(m, tt);
    let qn;
    let bpm;
    if (near) {
        bpm = near.bpm;
        qn = near.ppq + ((tt - near.timeS) * bpm) / 60;
    } else if (m.latestPpq) {
        bpm = m.latestPpq.bpm;
        qn = m.latestPpq.ppq + ((tt - m.latestPpq.timeS) * bpm) / 60;
    } else {
        // 宿主给了速度却没给拍位置:只能假设拍位置原点在 0 秒。
        bpm = m.latest.bpm;
        qn = (tt * bpm) / 60;
    }
    const exact = !m.meterVaried && (!m.tempoVaried || !!near);
    return { qn, bpm, exact };
}

/**
 * 秒 → {bar, beat}(都从 1 起;拍按拍号分母计,6/8 的一拍是八分音符)。无速度 ⇒ null。
 * 拍号取最近一次读到的那个(见文件头「拍号变化」一条)。
 */
export function barBeatAt(model, t) {
    const r = qnAt(model, t);
    if (!r) return null;
    const { num, den } = model.latest;
    const qnPerBeat = 4 / den;
    let beats = r.qn / qnPerBeat;
    const snap = (SNAP_S * r.bpm) / 60 / qnPerBeat;
    const nearest = Math.round(beats);
    if (Math.abs(beats - nearest) <= snap) beats = nearest;
    const idx = Math.floor(beats);
    const bar = Math.floor(idx / num) + 1;
    const beat = idx - (bar - 1) * num + 1;
    return { bar, beat, exact: r.exact };
}

/** 「33.1」这种写法(小节.拍)。 */
export function formatBarBeat(bb) {
    return bb ? bb.bar + "." + bb.beat : "";
}

/**
 * 从 t 起挪 `bars` 个小节后的秒数(负数往前)。无速度 ⇒ null(调用方按秒挪)。
 * 用 t 处换算所用的那个速度折算 —— 恒速工程上是精确值;变速工程上是「按最近 BPM 估算」。
 */
export function stepByBars(model, t, bars) {
    const r = qnAt(model, t);
    if (!r) return null;
    const { num, den } = model.latest;
    const qnPerBar = (num * 4) / den;
    return (Number(t) || 0) + (bars * qnPerBar * 60) / r.bpm;
}
