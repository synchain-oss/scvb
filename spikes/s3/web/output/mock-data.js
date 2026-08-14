// SPDX-License-Identifier: GPL-3.0-or-later
// mock-data.js —— S3 spike mock 数据生成器(15 轨 × 5 分钟 + 300 分段 + 2 版曲线 + 123 参数稀疏面)。
// 全部固定种子、确定性(可跨 run 复现)。波形按 requestWaveform 语义下采样:视口 → cols 列 min/max。
// [J59] 13 mono + 2 stereo(V14/V15);[J57/J59/J60] 每轨 width/participate/stereo 标识齐备。

const TRACKS = 15;
const STEREO_TRACKS = 2; // 最后 2 轨(V14/V15)
const DURATION_S = 300; // 5 分钟
const SEGMENTS_PER_TRACK = 20; // 15 × 20 = 300 分段矩形

// 确定性 PRNG(mulberry32)
function mulberry32(seed) {
    let a = seed >>> 0;
    return function () {
        a |= 0;
        a = (a + 0x6d2b79f5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

// 确定性波形:track 在 tSec 的 dB 值([-60, 0])。多正弦叠加 + 慢包络,相位随 track 变化。
function dbAt(ch, tSec) {
    const w1 = Math.sin(tSec * 1.7 + ch * 0.9);
    const w2 = Math.sin(tSec * 5.3 + ch * 1.3);
    const w3 = Math.sin(tSec * 11.1 + ch * 2.1);
    const env = 0.55 + 0.45 * Math.sin(tSec * 0.11 + ch * 0.7);
    const v = -24 + 18 * env * (0.5 * w1 + 0.3 * w2 + 0.2 * w3);
    return Math.max(-60, Math.min(0, v));
}

function makeChannels() {
    const rnd = mulberry32(20260701);
    const names = [
        "Lead Vox",
        "Harmony",
        "BGV High",
        "BGV Low",
        "Adlib",
        "Double",
        "Choir A",
        "Choir B",
    ];
    const channels = [];
    for (let ch = 1; ch <= TRACKS; ch++) {
        const stereo = ch > TRACKS - STEREO_TRACKS;
        channels.push({
            ch,
            label:
                ch <= names.length
                    ? names[ch - 1]
                    : "Track " + String(ch).padStart(2, "0"),
            priority: 1 + Math.floor(rnd() * 15),
            lead: ch === 1,
            lead_vol_exempt: false,
            participate_in_auto_pan: stereo ? false : ch % 3 === 0,
            pair_id: ch % 7 === 0 ? 1 + Math.floor(rnd() * 7) : 0,
            source_channels: stereo ? 2 : 1,
            enabled: true,
            pan: Math.round((rnd() * 2 - 1) * 100),
            width: stereo ? Math.round(rnd() * 100) : 0,
            vol: Math.round((rnd() * 36 - 24) * 10) / 10,
            freeze: 0, // bit0=pan,bit1=vol([J65] 四态)
            auto_pan: true,
            auto_vol: true,
        });
    }
    return channels;
}

function makePanCurve() {
    return [
        { angle: -100, gain_db: 0, shape: "bell", q: 1, side: "out" },
        { angle: -50, gain_db: -3, shape: "bell", q: 1.2, side: "out" },
        { angle: 0, gain_db: 0, shape: "bell", q: 1, side: "out" },
        { angle: 50, gain_db: 2.5, shape: "shelf", q: 0.7, side: "right" },
        { angle: 100, gain_db: 0, shape: "bell", q: 1, side: "out" },
    ];
}

function makeMockData() {
    const channels = makeChannels();
    const versions = [
        { v: 1, name: "V1", pan_curve: makePanCurve() },
        { v: 2, name: "V2", pan_curve: makePanCurve() },
    ];

    // 分段:每轨 20 段,等分 5 分钟,随机 origin(多数 auto,少量 user_edited/locked)。
    const rnd = mulberry32(77);
    const segments = [];
    for (let ch = 1; ch <= TRACKS; ch++) {
        const segLen = DURATION_S / SEGMENTS_PER_TRACK;
        for (let s = 0; s < SEGMENTS_PER_TRACK; s++) {
            const originRoll = rnd();
            const origin =
                originRoll > 0.85
                    ? "user_edited"
                    : originRoll > 0.7
                      ? "locked"
                      : "auto";
            segments.push({
                ch,
                startS: +(s * segLen).toFixed(3),
                endS: +((s + 1) * segLen).toFixed(3),
                origin,
                locked: origin === "locked",
            });
        }
    }

    // 123 参数稀疏面(自动化参数):全局 3 + 每轨 width/freeze(冻结走同一参数,bit0=pan/bit1=vol)。
    const params = { width: 100, ms_balance: 0, lead_select: 0 };
    for (let ch = 1; ch <= TRACKS; ch++) {
        params["v1_t" + String(ch).padStart(2, "0") + "_width"] =
            channels[ch - 1].width;
        params["v1_t" + String(ch).padStart(2, "0") + "_freeze"] =
            channels[ch - 1].freeze;
    }

    const state = {
        group_id: 1,
        version_active: 1,
        global: {
            width: 100,
            ms_balance: 0,
            lead_select: 0,
            range: { mode: "follow", startS: 0, endS: 0 },
            capture_enabled: true,
            output_enabled: false,
        },
        analysis: {
            vad: {
                threshold_db: -40,
                hysteresis_db: 3,
                hangover_ms: 120,
                padding_pre_ms: 10,
                padding_post_ms: 200,
            },
            segmentation: {
                mode: "energy",
                sensitivity: 5,
                min_segment_ms: 80,
            },
            transition_ramp_ms: 80,
        },
        versions,
        channels,
        ui: {
            lang: "zh",
            guide_seen: true,
            tour_seen: true,
            active_tab: "wave",
            ui_scale: 1,
        },
        params,
    };

    function stateSnapshot() {
        return JSON.parse(JSON.stringify(state));
    }
    function initialSnapshot() {
        return { ...stateSnapshot(), config_seq: 1 };
    }
    function paramsSnapshot() {
        // 模拟 host 回吐 + 电平抖动
        const p = { ...state.params };
        p.width = Math.max(
            0,
            Math.min(150, p.width + Math.round(Math.sin(Date.now() / 800) * 2)),
        );
        return { values: p, hostEcho: false };
    }
    function metersSnapshot() {
        const meters = [];
        for (let ch = 1; ch <= TRACKS; ch++) {
            const v = dbAt(ch, (Date.now() / 1000) % DURATION_S);
            meters.push({ db: v, peakDb: Math.min(0, v + 3) });
        }
        return {
            tracks: meters,
            busL: dbAt(1, (Date.now() / 1000) % DURATION_S),
            busR: dbAt(2, (Date.now() / 1000) % DURATION_S),
        };
    }
    function playheadSnapshot() {
        const t = (Date.now() / 1000) % DURATION_S;
        return {
            timeS: t,
            isPlaying: true,
            loopStartS: null,
            loopEndS: null,
            inRange: true,
        };
    }
    function connSnapshot() {
        const tracks = channels.map((c) => ({
            ch: c.ch,
            slotState: c.enabled ? "active" : "idle",
            heartbeatFresh: true,
            capturing: state.global.capture_enabled,
            misalignCount: 0,
            srMismatch: false,
        }));
        return { tracks, outputReadOnly: false };
    }
    function groupsSnapshot() {
        return { groups_online: 1 }; // bit0 = 组 A 在线
    }

    // requestWaveform:把 [startS,endS] 下采样到 cols 列,每列 min/max + vad/covered/stale/passId。
    function waveform(ch, startS, endS, cols) {
        const n = Math.max(1, cols | 0);
        const minDb = new Float32Array(n);
        const maxDb = new Float32Array(n);
        const vad = new Uint8Array(n);
        const covered = new Uint8Array(n);
        const stale = new Uint8Array(n);
        const passId = new Uint8Array(n);
        const span = Math.max(0.0001, endS - startS);
        const samplesPerCol = 4;
        for (let c = 0; c < n; c++) {
            const t0 = startS + (c / n) * span;
            const t1 = startS + ((c + 1) / n) * span;
            let lo = Infinity;
            let hi = -Infinity;
            for (let s = 0; s < samplesPerCol; s++) {
                const t = t0 + ((t1 - t0) * s) / samplesPerCol;
                const v = dbAt(ch, t);
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            minDb[c] = lo;
            maxDb[c] = hi;
            const mid = (t0 + t1) / 2;
            vad[c] = dbAt(ch, mid) > -45 ? 1 : 0;
            covered[c] = mid % 40 < 32 ? 1 : 0;
            stale[c] = mid > DURATION_S - 12 ? 1 : 0;
            passId[c] = 1 + Math.floor((mid / DURATION_S) * 8);
        }
        return { minDb, maxDb, vad, covered, stale, passId };
    }

    return {
        TRACKS,
        DURATION_S,
        segments,
        state,
        initialSnapshot,
        stateSnapshot,
        paramsSnapshot,
        metersSnapshot,
        playheadSnapshot,
        connSnapshot,
        groupsSnapshot,
        waveform,
    };
}

export { makeMockData, TRACKS, DURATION_S, SEGMENTS_PER_TRACK, STEREO_TRACKS };
