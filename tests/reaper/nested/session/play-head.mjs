// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the start of normal playback sounds like: press play from the top a few times, with different gaps between,
// and read the master meter from inside REAPER while it plays. The meter is read about every 30 ms, so this sees holes
// and missing voices, not single blocks. It has no threshold of its own: the numbers go into the detail, and --max-hole
// fails the row when a hole in the first two seconds is longer than that many seconds.
//
//   --gaps ms,ms,...   how long to stay stopped before each play (default 3000,1000,7000,1000)
//   --play ms          how long each play lasts (default 3000)
//   --max-hole s       optional
//   --late-seek        stay stopped where playback stopped and move the cursor halfway through the gap, to 2 s and
//                      back to 0 in turn: a click on the timeline just before pressing play
//   --tomaster spec    first move an FX to the master track ("TOP:0")

import { finish, log, parseArgs, Reaper, sleep } from "./lib.mjs";

const args = parseArgs();
const reaper = new Reaper(args.rpc);
const SILENT = 0.001; // -60 dB

function readCycle(samples) {
    const playing = samples.filter((s) => s.playing);
    if (playing.length === 0) return null;
    const t0 = playing[0].t;
    const steadyFrom = playing.filter((s) => s.t - t0 > 1.5).map((s) => s.peak);
    const steady = steadyFrom.length
        ? steadyFrom.sort((a, b) => a - b)[steadyFrom.length >> 1]
        : 0;
    const first = playing.find((s) => s.peak > SILENT);
    let hole = 0;
    let holeAt = 0;
    let start = null;
    let normal = null;
    for (const s of playing) {
        if (s.t - t0 > 2) break;
        if (first && s.t >= first.t) {
            if (s.peak <= SILENT) {
                start ??= s.t;
                if (s.t - start > hole) {
                    hole = s.t - start;
                    holeAt = start - t0;
                }
            } else {
                start = null;
            }
        }
        if (normal === null && s.peak >= steady * 0.7) normal = s.t - t0;
    }
    let lastLow = 0;
    for (const s of playing) {
        if (s.t - t0 > 2) break;
        if (s.peak < steady * 0.5) lastLow = s.t - t0;
    }
    return {
        first: first ? first.t - t0 : -1,
        hole,
        holeAt,
        normal: normal ?? -1,
        lastLow,
        steady,
    };
}

try {
    await reaper.ready();
    const gaps = (args.gaps ?? "3000,1000,7000,1000").split(",").map(Number);
    const playMs = Number(args.play ?? 3000);
    const lateSeek = args["late-seek"] !== undefined;
    if (args.tomaster)
        await reaper.cmd("tomaster", ...args.tomaster.split(":"));
    await reaper.cmd("seek", "0");
    let target = 0;
    const results = [];
    for (const gap of gaps) {
        if (lateSeek) {
            await sleep(gap / 2);
            target = target === 0 ? 2 : 0;
            await reaper.cmd("seek", String(target));
            await sleep(gap / 2);
        } else {
            await sleep(gap);
        }
        await reaper.cmd("peakstart");
        await reaper.cmd("play");
        await sleep(playMs);
        const raw = await reaper.cmd("peakdump");
        await reaper.cmd("stop");
        await sleep(200);
        if (!lateSeek) await reaper.cmd("seek", "0");
        log(`gap ${gap}: ${raw.split("\n").length} samples`);
        const samples = raw
            .split("\n")
            .filter(Boolean)
            .map((l) => {
                const [t, pos, state, l1, r1] = l.split("\t").map(Number);
                return {
                    t,
                    pos,
                    playing: (state & 1) !== 0,
                    peak: Math.max(l1, r1),
                };
            });
        const c = readCycle(samples);
        results.push({ gap, ...c });
    }
    await reaper.cmd("quit");
    const fmt = (v) => (v < 0 ? "none" : v.toFixed(2));
    const detail = results
        .map(
            (r) =>
                `gap ${r.gap / 1000}s: first=${fmt(r.first)} hole=${r.hole.toFixed(2)}@${r.holeAt.toFixed(2)} low-until=${r.lastLow.toFixed(2)}`,
        )
        .join(" | ");
    const worst = Math.max(...results.map((r) => r.hole));
    const fail =
        args["max-hole"] !== undefined && worst > Number(args["max-hole"]);
    finish(args.outcome, fail ? "FAIL" : "PASS", detail);
} catch (e) {
    log("error", String(e.stack || e));
    try {
        await reaper.cmd("quit");
    } catch {}
    finish(args.outcome, "INCOMPLETE", String(e.message || e));
}
