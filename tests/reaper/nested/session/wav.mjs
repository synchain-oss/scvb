// SPDX-License-Identifier: GPL-3.0-or-later
// Reads a render and compares it with another, sample by sample. A window-started render is not one of the harness's
// render scenarios, so its verdict is computed here.
import { readFileSync } from "node:fs";

export function readWav(path) {
    const b = readFileSync(path);
    if (
        b.toString("latin1", 0, 4) !== "RIFF" &&
        b.toString("latin1", 0, 4) !== "RF64"
    )
        throw new Error(`${path}: not a WAV file`);
    let fmt = null;
    let data = null;
    for (let p = 12; p + 8 <= b.length;) {
        const id = b.toString("latin1", p, p + 4);
        const size = b.readUInt32LE(p + 4);
        if (id === "fmt ") {
            const tag = b.readUInt16LE(p + 8);
            const channels = b.readUInt16LE(p + 10);
            const rate = b.readUInt32LE(p + 12);
            const bits = b.readUInt16LE(p + 22);
            const sub = tag === 0xfffe ? b.readUInt16LE(p + 32) : tag;
            fmt = { sub, channels, rate, bits };
        } else if (id === "data") {
            data = b.subarray(p + 8, Math.min(b.length, p + 8 + size));
        }
        p += 8 + size + (size & 1);
    }
    if (!fmt || !data) throw new Error(`${path}: no fmt or data chunk`);
    if (fmt.sub !== 3 || fmt.bits !== 32)
        throw new Error(
            `${path}: expected 32-bit float, found format ${fmt.sub} with ${fmt.bits} bits`,
        );
    const frames = Math.floor(data.length / (4 * fmt.channels));
    const channels = Array.from(
        { length: fmt.channels },
        () => new Float32Array(frames),
    );
    for (let f = 0; f < frames; f++) {
        for (let c = 0; c < fmt.channels; c++)
            channels[c][f] = data.readFloatLE((f * fmt.channels + c) * 4);
    }
    return { rate: fmt.rate, frames, channels };
}

export const toDb = (x) => (x > 0 ? 20 * Math.log10(x) : -Infinity);
const fmtDb = (x) => (x === -Infinity ? "-inf" : x.toFixed(2));

export function peakDb(wav) {
    let peak = 0;
    for (const ch of wav.channels)
        for (const s of ch) peak = Math.max(peak, Math.abs(s));
    return toDb(peak);
}

function rmsDb(ch, from, to) {
    let sum = 0;
    for (let i = from; i < to; i++) sum += ch[i] * ch[i];
    return toDb(Math.sqrt(sum / Math.max(1, to - from)));
}

// onset: the first sample that differs by more than onsetDb. settled: the sample after the last one that does.
export function compareWav(a, b, { onsetDb = -60, tailSkipSec = 2 } = {}) {
    const frames = Math.min(a.frames, b.frames);
    const limit = 10 ** (onsetDb / 20);
    const tailFrom = Math.min(frames, Math.round(tailSkipSec * a.rate));
    let max = 0;
    let tail = 0;
    let onset = -1;
    let settled = 0;
    for (let f = 0; f < frames; f++) {
        for (let c = 0; c < a.channels.length; c++) {
            const d = Math.abs(a.channels[c][f] - b.channels[c][f]);
            if (d > max) max = d;
            if (f >= tailFrom && d > tail) tail = d;
            if (d > limit) {
                if (onset < 0) onset = f;
                settled = f + 1;
            }
        }
    }
    return {
        frames: `${a.frames}/${b.frames}`,
        maxDb: toDb(max),
        tailDb: toDb(tail),
        onsetSec: onset < 0 ? null : onset / a.rate,
        settledSec: settled / a.rate,
        rmsChangeDb: a.channels.map(
            (ch, c) =>
                rmsDb(ch, tailFrom, frames) -
                rmsDb(b.channels[c], tailFrom, frames),
        ),
    };
}

export function describe(c) {
    const when =
        c.onsetSec === null
            ? "no difference over -60"
            : `differs from ${c.onsetSec.toFixed(3)} s to ${c.settledSec.toFixed(3)} s`;
    const rms = c.rmsChangeDb.map(fmtDb).join("/");
    return `frames=${c.frames} maxdiff=${fmtDb(c.maxDb)} tail=${fmtDb(c.tailDb)} ${when} rms-change=${rms}`;
}
