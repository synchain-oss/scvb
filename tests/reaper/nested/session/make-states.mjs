// SPDX-License-Identifier: GPL-3.0-or-later
// Sets group and channel on every SCVB instance through its real bridge, one window at a time, and saves the project.
// The saved project is the template the nested configuration takes its per-instance states from.
import { dirname, resolve } from "node:path";
import { mkdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import {
    closeFx,
    finish,
    log,
    openFx,
    parseArgs,
    Reaper,
    sleep,
} from "./lib.mjs";

const args = parseArgs();
const reaper = new Reaper(args.rpc);
const port = args.port;
const states = resolve(dirname(fileURLToPath(import.meta.url)), "..", "states");
const raw = resolve(states, "template-raw.rpp");
const save = resolve(states, "template.rpp");
// states/ is not in git: a fresh clone has no folder for REAPER to save into.
mkdirSync(states, { recursive: true });

// track index, FX index, group (1..8 = A..H), channel (undefined for an Output, "observe" for a Monitor)
const PLAN = [
    [0, 0, 8],
    [1, 0, 1],
    [1, 1, 8, 1],
    [2, 0, 1, 1],
    [3, 0, 1, 2],
    [4, 0, 2],
    [4, 1, 8, 2],
    [5, 0, 2, 1],
    // SPARE: instances only the wrong-wiring and Monitor layouts use
    [6, 0, 8, 3],
    [6, 1, 1, 3],
    [6, 2, 8, "observe"],
];

// Outputs write the labels they learn from their Inputs into their state, so an Output's state changes when the same
// Inputs sit on tracks with other names (the harness learns blocks on tracks of its own). A label a user typed is kept
// as it is, so the states the render tests use carry typed labels; template-raw.rpp keeps the followed ones.
const OUTPUTS = PLAN.filter((row) => row[3] === undefined);
const pinLabels = async (page) => {
    const res = [];
    for (const ch of [1, 2, 3])
        res.push(await page.call("setChannelConfig", ch, { label: `S${ch}` }));
    return res;
};

try {
    await reaper.ready();
    const seen = [];
    for (const [t, f, group, channel] of PLAN) {
        const page = await openFx(reaper, port, t, f);
        const g = await page.call(
            channel === "observe" ? "setObservedGroup" : "setGroupId",
            group,
        );
        const c =
            channel === undefined || channel === "observe"
                ? null
                : await page.call("setChannelId", channel);
        log(
            "track",
            String(t),
            "fx",
            String(f),
            page.target.url.replace(/^.*\/(\w+)\/index.html.*$/, "$1"),
            "group",
            String(group),
            g,
            "channel",
            String(channel ?? "-"),
            c,
        );
        seen.push({ t, f, g, c });
        await sleep(300);
        await closeFx(reaper, page, t, f, port);
    }
    await sleep(500);
    await reaper.cmd("save", raw);
    await sleep(500);

    // Fresh load: every Output now is the only one in its group, so its writes are accepted.
    await reaper.cmd("open", raw);
    await sleep(2500);
    for (const [t, f] of OUTPUTS) {
        const page = await openFx(reaper, port, t, f);
        const res = await pinLabels(page);
        log("track", String(t), "labels", res);
        if (res.some((r) => r?.ok !== true))
            seen.push({ t, f, g: { ok: false, labels: res }, c: null });
        await sleep(300);
        await closeFx(reaper, page, t, f, port);
    }
    await sleep(500);
    await reaper.cmd("save", save);
    await sleep(500);
    await reaper.cmd("quit");
    const bad = seen.filter((s) => s.g?.ok === false || s.c?.ok === false);
    finish(
        args.outcome,
        bad.length ? "FAIL" : "PASS",
        bad.length
            ? `refused: ${JSON.stringify(bad)}`
            : `${seen.length} instances set; project saved`,
    );
} catch (e) {
    log("error", String(e.stack || e));
    try {
        await reaper.cmd("quit");
    } catch {}
    finish(args.outcome, "INCOMPLETE", String(e.message || e));
}
