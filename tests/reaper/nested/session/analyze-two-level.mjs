// SPDX-License-Identifier: GPL-3.0-or-later
// The two-level workflow a user would follow, through the plugins' real bridges with their windows open:
//   capture and analyze groups A and B, turn their outputs on, then capture and analyze group H from what they send up.
// Saves the project after each stage and writes what the plugins reported to states/analysis.json:
//   analyzed-l1.rpp         A and B analyzed with outputs on, H not analyzed
//   analyzed-on.rpp         H analyzed with the groups taking part in auto-pan (the default)
//   automated-on.rpp        analyzed-on after a pass in REAPER's Write mode
//   analyzed-off-after.rpp  analyzed-on, then H analyzed again with the groups out of auto-pan
//   analyzed-off-first.rpp  analyzed-l1, with the groups out of auto-pan before H's first analysis
// It fails when the positions or the lanes are not what the two-level workflow should leave; how they sound is for the
// render scenarios.
import { writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { finish, log, openFx, parseArgs, Reaper, sleep } from "./lib.mjs";

const args = parseArgs();
const reaper = new Reaper(args.rpc);
const port = args.port;
const states = resolve(dirname(fileURLToPath(import.meta.url)), "..", "states");
const TEMPLATE = resolve(states, "template-raw.rpp");

const TRACK = { TOP: 0, GA: 1, VA1: 2, VA2: 3, GB: 4, VB1: 5 };
const GROUP = { TOP: 8, GA: 1, GB: 2 };
// REAPER stops by itself at the end of the project; the items last 8 s.
const SECONDS = 7.95;
const WRITE = 3;
const READ = 1;
const PAN_CENTRE = 0.5;
const VOL_ZERO = 24 / 36;

const known = new Set();
async function open(name) {
    const isWanted = async (p) =>
        (await p.call("requestInitialState"))?.group_id === GROUP[name];
    const page = await openFx(
        reaper,
        port,
        TRACK[name],
        0,
        known,
        60000,
        isWanted,
    );
    known.add(page.target.id);
    await page.watch("scvb.segments");
    await page.watch("scvb.error");
    return page;
}

async function openAll() {
    const pages = {};
    for (const name of ["GA", "GB", "TOP"]) pages[name] = await open(name);
    await sleep(1500);
    return pages;
}

async function pass() {
    await reaper.cmd("seek", 0);
    await reaper.cmd("play");
    const t0 = Date.now();
    for (;;) {
        await sleep(100);
        const [pos, state] = (await reaper.cmd("pos")).split("\t");
        if (Number(pos) >= SECONDS || (Number(state) & 1) === 0) break;
        if (Date.now() - t0 > (SECONDS + 20) * 1000)
            throw new Error(`transport stuck at ${pos}`);
    }
    await reaper.cmd("stop");
    await sleep(500);
}

function summary(segments) {
    if (!segments) return null;
    return segments.channels
        .filter((c) => c.segments.length)
        .map((c) => ({
            ch: c.ch,
            stale: c.stale,
            segments: c.segments.map((s) => ({
                t0: +s.t0S.toFixed(3),
                t1: +s.t1S.toFixed(3),
                pan: +s.pan.toFixed(3),
                volDb: +s.volDb.toFixed(3),
            })),
        }));
}

function connected(snap) {
    return snap.conn.channels
        .map((c, i) => ({
            ch: i + 1,
            slot: c.slotState,
            fresh: c.heartbeatFresh,
            misalign: c.misalignCount,
        }))
        .filter((c) => c.slot !== 0);
}

async function analyze(page, name) {
    const before = await page.eval(`window.__t["scvb.segments"].n`);
    log(
        name,
        "connected before analyze",
        connected(await page.call("requestInitialState")),
    );
    const res = await page.call("analyze", "all");
    log(name, "analyze", res);
    if (!res?.ok)
        throw new Error(`${name}: analyze refused: ${JSON.stringify(res)}`);
    const t0 = Date.now();
    for (;;) {
        await sleep(200);
        const snap = await page.call("requestInitialState");
        const n = await page.eval(`window.__t["scvb.segments"].n`);
        if (!snap.analysis_run?.running && n > before) break;
        if (Date.now() - t0 > 60000)
            throw new Error(`${name}: analysis did not finish`);
    }
    return summary(await page.latest("scvb.segments"));
}

async function capture(pages, label) {
    for (const p of pages)
        log(label, "capture on", await p.call("setCaptureEnabled", true));
    await pass();
    for (const p of pages)
        log(label, "capture off", await p.call("setCaptureEnabled", false));
    await sleep(500);
}

async function outputOn(page, name) {
    const res = await page.call("setOutputEnabled", true);
    await page.call("confirmPrintGuard");
    const snap = await page.call("requestInitialState");
    log(
        name,
        "output on",
        res,
        "guard",
        snap.print_guard,
        "enabled",
        String(snap.global.output_enabled),
    );
    return res;
}

// See make-states.mjs: typed labels keep the saved states the same on the harness's learn tracks.
async function pinLabels(page) {
    for (const ch of [1, 2, 3])
        await page.call("setChannelConfig", ch, { label: `S${ch}` });
}

function channels(snap, n) {
    return snap.channels.slice(0, n).map((c) => ({
        label: c.label,
        source: c.source_channels,
        enabled: c.enabled,
        participate: c.participate_in_auto_pan,
    }));
}

// A pass with output on moves the parameters to the segment values, and REAPER's Write mode may only record a change,
// so the lanes go back to their defaults first, as they are on a user's first pass after analyzing.
async function resetLanes(track, tracks) {
    for (const t of tracks) {
        await reaper.cmd("setparam", track, 0, `V1 T0${t} Pan`, PAN_CENTRE);
        await reaper.cmd("setparam", track, 0, `V1 T0${t} Vol`, VOL_ZERO);
    }
}

// The segment table of each track, as [pan, volDb] of its only segment.
const positions = (table) =>
    table.map((t) =>
        t.segments.map((s) => [s.pan, Number(s.volDb.toFixed(1))]),
    );

// Numbers within 0.2, so a tenth of a dB from another machine or analysis build doesn't fail the row.
const same = (got, want) =>
    Array.isArray(want)
        ? Array.isArray(got) &&
          got.length === want.length &&
          got.every((g, i) => same(g, want[i]))
        : typeof want === "number"
          ? typeof got === "number" && Math.abs(got - want) <= 0.2
          : got === want;

function judge(out) {
    const problems = [];
    const expect = (name, got, want) => {
        if (!same(got, want))
            problems.push(
                `${name}: ${JSON.stringify(got)}, expected ${JSON.stringify(want)}`,
            );
    };
    expect(
        "group A positions",
        positions(out.ga).map((t) => t.map((s) => s[0])),
        [[-60], [60]],
    );
    expect(
        "group B positions",
        positions(out.gb).map((t) => t.map((s) => s[0])),
        [[0]],
    );
    expect("H with the groups taking part", positions(out.topOn), [
        [[-60, -1.1]],
        [[60, 1.6]],
    ]);
    expect(
        "H, groups taken out after its analysis",
        positions(out.topOffAfter),
        [[[-60, -1.1]], [[60, 1.6]]],
    );
    expect(
        "H, groups out before its first analysis",
        positions(out.topOffFirst),
        [[[0, 0]], [[0, 0]]],
    );
    // REAPER keeps a lane only for a parameter whose value moved: group B's single track stays at its default.
    const names = (lanes) => lanes.map((l) => l.name).sort();
    expect("lanes on H", names(out.lanes.TOP), [
        "V1 T01 Pan",
        "V1 T01 Vol",
        "V1 T02 Pan",
        "V1 T02 Vol",
    ]);
    expect("lanes on group A", names(out.lanes.GA), [
        "V1 T01 Pan",
        "V1 T02 Pan",
    ]);
    expect("lanes on group B", names(out.lanes.GB), []);
    return problems;
}

async function lanes(track) {
    const text = await reaper.cmd("envs", track, 0);
    return text
        ? text.split("\n").map((l) => {
              const [name, points] = l.split("\t");
              return {
                  name,
                  points: points
                      .split(" ")
                      .map((p) => p.split(":").map(Number)),
              };
          })
        : [];
}

async function reopen(file, pages) {
    for (const p of Object.values(pages)) p.close();
    await reaper.cmd("open", resolve(states, file));
    await sleep(4000);
    known.clear();
    const fresh = await openAll();
    await sleep(1500);
    for (const p of Object.values(fresh)) await p.call("confirmPrintGuard");
    log(
        "reopened",
        file,
        "TOP connected",
        connected(await fresh.TOP.call("requestInitialState")),
    );
    return fresh;
}

async function participateOff(top) {
    for (const ch of [1, 2])
        log(
            "participate off",
            String(ch),
            await top.call("setChannelConfig", ch, {
                participate_in_auto_pan: false,
            }),
        );
}

try {
    await reaper.ready();
    await reaper.cmd("open", TEMPLATE);
    await sleep(3000);
    const out = {};
    let pages = await openAll();

    // Level 1
    await capture([pages.GA, pages.GB], "L1");
    out.ga = await analyze(pages.GA, "GA");
    out.gb = await analyze(pages.GB, "GB");
    log("segments GA", out.ga);
    log("segments GB", out.gb);
    await outputOn(pages.GA, "GA");
    await outputOn(pages.GB, "GB");
    await sleep(500);
    await reaper.cmd("save", resolve(states, "analyzed-l1.rpp"));
    await sleep(500);

    // Level 2: what the groups send up, with their outputs on
    await capture([pages.TOP], "L2");
    out.topChannels = channels(await pages.TOP.call("requestInitialState"), 3);
    log("TOP channels", out.topChannels);
    out.topOn = await analyze(pages.TOP, "TOP on");
    log("segments TOP participate on", out.topOn);
    await outputOn(pages.TOP, "TOP");
    await sleep(500);
    for (const p of Object.values(pages)) await pinLabels(p);
    await sleep(300);
    await reaper.cmd("save", resolve(states, "analyzed-on.rpp"));
    await sleep(500);

    // Write mode: all three Outputs print their lanes in one pass
    await resetLanes(TRACK.TOP, [1, 2]);
    await resetLanes(TRACK.GA, [1, 2]);
    await resetLanes(TRACK.GB, [1]);
    await sleep(300);
    for (const t of [TRACK.TOP, TRACK.GA, TRACK.GB])
        await reaper.cmd("automode", t, WRITE);
    await pass();
    for (const t of [TRACK.TOP, TRACK.GA, TRACK.GB])
        await reaper.cmd("automode", t, READ);
    await sleep(500);
    out.lanes = {};
    for (const name of ["TOP", "GA", "GB"]) {
        out.lanes[name] = await lanes(TRACK[name]);
        log("lanes", name, out.lanes[name]);
    }
    await reaper.cmd("save", resolve(states, "automated-on.rpp"));
    await sleep(500);

    // Groups out of auto-pan after H already has a table of its own
    pages = await reopen("analyzed-on.rpp", pages);
    await capture([pages.TOP], "L2 off after");
    await participateOff(pages.TOP);
    out.topOffAfter = await analyze(pages.TOP, "TOP off after");
    log("segments TOP participate off after", out.topOffAfter);
    await outputOn(pages.TOP, "TOP");
    await sleep(500);
    await reaper.cmd("save", resolve(states, "analyzed-off-after.rpp"));
    await sleep(500);

    // Groups out of auto-pan before H's first analysis
    pages = await reopen("analyzed-l1.rpp", pages);
    await capture([pages.TOP], "L2 off first");
    await participateOff(pages.TOP);
    out.topOffFirst = await analyze(pages.TOP, "TOP off first");
    log("segments TOP participate off first", out.topOffFirst);
    await outputOn(pages.TOP, "TOP");
    await sleep(500);
    for (const p of Object.values(pages)) await pinLabels(p);
    await sleep(300);
    await reaper.cmd("save", resolve(states, "analyzed-off-first.rpp"));
    await sleep(500);

    writeFileSync(
        resolve(states, "analysis.json"),
        JSON.stringify(out, null, 2),
    );
    await reaper.cmd("quit");
    const problems = judge(out);
    if (problems.length) finish(args.outcome, "FAIL", problems.join("; "));
    else finish(args.outcome, "PASS", "two-level analysis done; states saved");
} catch (e) {
    log("error", String(e.stack || e));
    try {
        await reaper.cmd("quit");
    } catch {}
    finish(args.outcome, "INCOMPLETE", String(e.message || e));
}
