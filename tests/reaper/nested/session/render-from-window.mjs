// SPDX-License-Identifier: GPL-3.0-or-later
// A render started inside an open project, the way a user starts one from REAPER's window. The harness's render kind
// runs reaper -renderproject, which starts rendering as soon as the project has loaded, so the plugins' timers have
// barely run; here the project has been open and idle first. In REAPER's window the transport also takes part: --play
// listens for that many milliseconds first, as a user does before exporting.
//
//   --settle <ms>          how long the project stays open and idle first (default 3000)
//   --play <ms>            listen for this long before rendering
//   --ops op:arg:arg,...   REAPER ops to run before the render (setsolo, setmute, setbypass, renderset, ...)
//   --regions n:t0:t1,...  render these project regions, one file each; --compare then names the other run's files
//   --compare <name>       a render of an earlier scenario in this run, without .wav
//   --max-diff-db <dB>     the whole render must stay within this of the reference
//   --max-tail-db <dB>     the same after the first two seconds (real-time renders start with a handover)
//   --min-diff-db <dB>     the render must differ by at least this (negative control)
//   --rms-change L,R       after the first two seconds, each channel's RMS must have moved this many dB (within 0.3)
//   --max-peak-db <dB>     the render must stay below this
//   --min-peak-db <dB>     the render must reach this
import { existsSync } from "node:fs";
import { basename, dirname, join } from "node:path";
import { finish, log, parseArgs, Reaper, sleep } from "./lib.mjs";
import { compareWav, describe, peakDb, readWav } from "./wav.mjs";

const args = parseArgs();
const reaper = new Reaper(args.rpc);

function verdict(wav, compare) {
    const problems = [];
    const peak = peakDb(wav);
    let detail = `peak=${peak === -Infinity ? "-inf" : peak.toFixed(2)}`;
    if (args["max-peak-db"] !== undefined && peak > Number(args["max-peak-db"]))
        problems.push(`peak above ${args["max-peak-db"]}`);
    if (args["min-peak-db"] !== undefined && peak < Number(args["min-peak-db"]))
        problems.push(`peak below ${args["min-peak-db"]}`);
    if (compare) {
        const c = compareWav(wav, readWav(join(args.work, `${compare}.wav`)));
        detail += ` vs ${compare}: ${describe(c)}`;
        if (c.mismatch.length)
            problems.push(`not comparable: ${c.mismatch.join(", ")}`);
        else if (args["max-tail-db"] !== undefined && c.tailFrames === 0)
            problems.push("too short for the tail check");
        if (
            args["max-diff-db"] !== undefined &&
            c.maxDb > Number(args["max-diff-db"])
        )
            problems.push(`differs by more than ${args["max-diff-db"]}`);
        if (
            args["max-tail-db"] !== undefined &&
            c.tailDb > Number(args["max-tail-db"])
        )
            problems.push(`tail differs by more than ${args["max-tail-db"]}`);
        if (
            args["min-diff-db"] !== undefined &&
            c.maxDb < Number(args["min-diff-db"])
        )
            problems.push(`does not differ by ${args["min-diff-db"]}`);
        if (args["rms-change"] !== undefined) {
            const expected = args["rms-change"].split(",").map(Number);
            c.rmsChangeDb.forEach((got, ch) => {
                if (!(Math.abs(got - expected[ch]) <= 0.3))
                    problems.push(
                        `channel ${ch} RMS moved ${got.toFixed(2)} dB, expected ${expected[ch]}`,
                    );
            });
        }
    }
    return {
        status: problems.length ? "FAIL" : "PASS",
        detail: `${detail}${problems.length ? ` | ${problems.join("; ")}` : ""}`,
    };
}

try {
    await reaper.ready();
    for (const op of (args.ops ?? "").split(",").filter(Boolean))
        await reaper.cmd(...op.split(":"));
    const regions = (args.regions ?? "")
        .split(",")
        .filter(Boolean)
        .map((r) => r.split(":"));
    const base = basename(args.wav, ".wav");
    if (regions.length) {
        for (const [name, t0, t1] of regions)
            await reaper.cmd("region", t0, t1, name);
        await reaper.cmd("renderset", "RENDER_BOUNDSFLAG", "3");
        await reaper.cmd("renderpat", `${base}-$region`);
    }
    await sleep(Number(args.settle ?? 3000));
    if (args.play) {
        await reaper.cmd("play");
        await sleep(Number(args.play));
        await reaper.cmd("stop");
        await sleep(500);
        await reaper.cmd("seek", "0");
        await sleep(500);
    }
    log("render starts");
    await reaper.cmd("render");
    log("render returned");
    await sleep(500);
    await reaper.cmd("quit");
    if (regions.length) {
        const results = regions.map(([name]) => {
            const file = join(dirname(args.wav), `${base}-${name}.wav`);
            if (!existsSync(file))
                return { status: "FAIL", detail: `${name}: no file` };
            const v = verdict(
                readWav(file),
                args.compare && `${args.compare}-${name}`,
            );
            return { status: v.status, detail: `${name}: ${v.detail}` };
        });
        finish(
            args.outcome,
            results.some((r) => r.status !== "PASS") ? "FAIL" : "PASS",
            results.map((r) => r.detail).join(" | "),
        );
    } else if (!existsSync(args.wav)) {
        finish(args.outcome, "FAIL", "the render produced no file");
    } else {
        const v = verdict(readWav(args.wav), args.compare);
        finish(args.outcome, v.status, v.detail);
    }
} catch (e) {
    log("error", String(e.stack || e));
    try {
        await reaper.cmd("quit");
    } catch {}
    finish(args.outcome, "INCOMPLETE", String(e.message || e));
}
