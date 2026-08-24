**English** | [简体中文](USER_GUIDE.zh-CN.md)

> Status: evolving
> Last updated: 2026-08-24 (for version v0.1.0)
> Source of truth: the Chinese guide. Chinese is the semantic authority for the hard rules; see below.

# SCVB User Guide

SCVB (Synchain Vocal Balancer) is a **pair** of VST3 plugins that automatically balances pan and level across a multi-singer, multi-part vocal arrangement:

- **SCVB Input** sits on every vocal track and captures it;
- **SCVB Output** sits on the vocal bus, where it analyses, balances, sums, and writes the result back as DAW automation.

Both plugins must be **installed together and used as a pair**. Installing only one will not leave you with a dead track (see hard rule 3), but it will not give you any balancing either.

> **How to change the nine hard rules**: the `## 硬约束` section of `docs/USER_GUIDE.zh-CN.md` is the **single source of truth** for all nine. They also appear in this file, in both READMEs' Quick start, and in the plugin UI's three language dictionaries — 7 places in total. **None of them may be transcribed by hand.** To change the wording, edit that section only (translations live in `docs/hard-rules.i18n.json`), then run `node scripts/gen-hard-rules.mjs`; the other 6 places follow the generator. `node scripts/gen-hard-rules.mjs --check` is the gate.

## Hard rules

Breaking any one of these does not give you a slightly worse result — it gives you silence, wrong panning, or failed analysis. The plugin shows all nine on first launch, and you can reopen them any time from the Output **Settings** tab.

<!-- BEGIN GENERATED hard-rules:en -->
> ⚠️ **Must read: SCVB's nine usage rules. Breaking any one of them causes silence, wrong panning, or failed analysis.**
>
> 1. **Vocal tracks must keep their original DAW routing, pointing at the bus that hosts SCVB Output.** Do not re-route a vocal track straight to the master output, and do not bypass the bus. (ADR-002)
> 2. **SCVB Input must sit in the last slot of the vocal track's plugin chain; SCVB Output must sit in the first slot of the bus.** Any other position breaks the processing-order assumption SCVB relies on; for what each host calls that slot, see `docs/DAW_COMPATIBILITY.md`. (ADR-002 / J45)
> 3. **Input mutes its downstream output only while a healthy SCVB Output is detected — this is by design, not a bug.** That mute path is what preserves the "vocal tracks first, bus second" ordering in the DAW's dependency graph, and it still holds under offline rendering and REAPER's anticipative multithreading. **When no healthy Output is detected (not installed, not connected, peer has quit), Input falls back to passthrough automatically**, over an 80 ms ramp with a 5-second hysteresis debounce (the hysteresis applies only to the "mute → passthrough" direction; "passthrough → mute" ramps over 80 ms as soon as health is confirmed), so installing only one of the two plugins will never leave you with a dead track. (ADR-002 / J12 + J32)
> 4. **Host pan must stay centred on both the vocal tracks and the bus.** SCVB pans internally with an equal-power law, independently of the host's pan law; an off-centre host pan stacks on top of it and produces a wrong stereo image. (ADR-010)
> 5. **Each channel id is unique within one group, and a given vocal track may belong to only one group.** When two Inputs in the same group claim the same channel, the late arrival shows a "channel conflict" warning and stays inactive; the same channel number in a different group is a separate, unrelated path. (ADR-002 / J66)
> 6. **Only one Output instance can be active in a group at any one time.** A second instance in the same group drops into read-only observer mode and shows a warning; the eight groups (A–H) are independent bus domains and do not affect one another. (ADR-002 / J66)
> 7. **Stereo vocal tracks stay out of automatic pan assignment by default; switch one in by hand when you want it included.** Mono sources are placed with equal-power pan; stereo sources use a dual-pan + width model (pan = centre of the arc, width = spread) which by default preserves the stereo width you already have, rather than letting automatic assignment overwrite it. (ADR-003 / J57 + J60)
> 8. **SCVB Output reports no additional latency to the DAW.** Alignment is done by timeline addressing; do not try to "correct" it with PDC (plugin delay compensation). (ADR-002)
> 9. **Do not carry on exporting while a "timeline gap / overlap" warning is showing.** Work through the common-pitfalls list in `docs/DAW_COMPATIBILITY.md` to check your routing first: for as long as the warning count refuses to fall back to zero, some track's audio is not being picked up correctly.
<!-- END GENERATED hard-rules:en -->

## Five-minute start

### Create a vocal bus

Create a stereo bus in your DAW (group / bus / submix, the name varies by host) and route every vocal track you want balanced into it. **Once that is done, leave the vocal tracks' routing alone** (hard rule 1).

### Install both plugins

Put one SCVB Input in the **last slot** of **every** vocal track's plugin chain, and one SCVB Output in the **first slot** of the bus (hard rule 2). Host-by-host instructions are in `docs/DAW_COMPATIBILITY.md`.

### Assign channels and groups

Open each Input and give it a channel id. Channel ids may not repeat within one group (hard rule 5). The group selector is eight capsules, A–H, defaulting to A; a single song project will normally never need a second group — groups exist for "one DAW project containing several unrelated vocal buses". On the Output side, pick the same group in Tab 1.

### Capture

Turn on **Capture** in the Output, then play back as usual. Capture writes only while the switch is ON **and** the transport is rolling; whatever you play is what gets captured, and replaying a section overwrites the old data for it. SCVB **stores no audio** — only one feature frame every 10 ms (see "Sessions and files").

### Analyse

Once capture covers the whole song, press **Analyse**. Analysis runs voice detection (VAD), splits the material into segments, measures segment loudness, and produces a pan / vol curve per track. Thresholds and segmentation sensitivity can be changed at any time with live preview, **without recapturing** — what is stored is continuous features, not decisions.

### Output

Turn on the **Output** switch. What you now hear on the bus is the balanced result: each track takes its gain/pan from the curves, and the sum replaces the bus input. The first time you flip this switch you get a one-off confirmation bar explaining what happens next.

### Write automation

Set the DAW's automation mode to **Write** or **Latch** and play through once more. SCVB Output prints the current version's curves as host automation at the playback position (15 tracks x pan/vol, 30 lanes in total). When it is done, switch automation back to Read and the plugin follows your DAW automation faithfully — draw whatever you like by hand, and nothing will overwrite it until you write again.

## Interface tour

### The four Output tabs

| Tab | What it is for |
|---|---|
| **Overview** | The capture / analyse / output switches, engine Range, group selection, version chip, global Width and MS Balance, Lead Select, pan and level distribution charts |
| **Tracks** | The 15-row track table: per-track pan / vol / width readouts and controls, levels, lead lock, level exemption, auto-pan participation, freeze, ST marker |
| **Waveform** | Timeline lanes: VAD colouring, segments, the segment inspector (edit pan/vol on a selected segment), selections and partial recapture / re-analysis |
| **Settings** | Usage notes (including the nine hard rules), loudness basis, centre-slot policy, UI scale, language, version number, storage status, diagnostics |

### The Input single page

Input is a single page: channel selection, group selection, connection status, level, passthrough/mute status, in-place gain switch. **The configuration's source of truth lives in the Output** — the values Input shows and edits are read from and written to the Output's state over the control-plane IPC.

### First-run guide and tour

The first time you open an Output you get, in order: the language card, then the **nine hard rules page**, then "Would you like a one-minute look at the interface?", then the interactive tour.

- The **"Don't show again"** checkbox at the bottom of the rules page applies across projects: tick it and new projects will not show it again.
- The tour is a spotlight-style walkthrough. **Left-click anywhere to advance**, it is purely visual and textual with no audio, and it runs on demo data — **nothing is written into your project**.
- In the Settings tab, **"Show guide again"** replays the tour at any time, **"Show all nine"** reopens the rules page, and **"View workflow"** opens the workflow overview card.

## Capture

- **When it writes**: capture switch ON **and** transport rolling. Stop the transport and writing stops.
- **Replay semantics**: data is merged by timeline address, so replaying a range overwrites it with the new data. To redo a section, just play that section again.
- **Features, not audio**: one frame every 10 ms holding K-weighted mean-square + peak + the continuous VAD posterior. Project size stays manageable and thresholds can be re-tuned offline as often as you like.
- **Coverage**: ranges that were never captured show as uncaptured on the waveform page — they are never faked as silence. If you see a hole, play that part again.

## Analysis

- **VAD**: dual-threshold energy detection with hysteresis, hangover, and padding either side; the default errs on the generous side. Thresholds and sensitivity can be dragged with live preview.
- **Segmentation**: energy-valley detection plus a minimum segment length and a breath tolerance.
- **Segment loudness basis**: Settings offers **K-weighted segment integration (default) / RMS / peak dBFS**; changing it requires a re-analysis.
- **Centre-slot policy**: the fallback rule for when several tracks compete for the centre position — **priority queue (default) / lead exclusive / evenly nudged apart**, also a "re-analyse after changing" setting.
- **Dragging thresholds never touches segments you edited**: 300 ms after you release, only segments that are `origin=auto` and unlocked get rewritten; hand-edited or locked segments stay byte-for-byte identical, and the first line of the diff tells you how many were preserved.

## Manual touch-ups

Select a segment on the waveform page and edit its pan / vol in the segment inspector:

- hand-edited segments are marked `origin=user_edited`, newly created ones `origin=user_created`;
- you can additionally mark a segment `locked`;
- **automatic re-analysis will not overwrite either kind** unless you explicitly ask for them to be re-detected.

**Freezing** a dimension on the Tracks page declares "I am taking this dimension over by hand": on write, that dimension is printed into automation as a flat line, and whatever you draw in the DAW afterwards will never be overridden by the engine. The priority chain is: **host automation > frozen manual value > manual touch-up > engine analysis curve**.

## Partial recapture / re-analysis

The unit of invalidation and recomputation is **(track x time range)**, and it **never touches results that already exist in other ranges**. Drag out a selection on the waveform page, then:

- **Recapture**: arms the range (an armed badge appears in three places), rewrites features the next time playback reaches it, and stops automatically at the boundary. If the output switch is on while a range is armed, you get an amber warning.
- **Re-analysis**: re-runs analysis over the selection only; segments and curves elsewhere are preserved exactly.

## Versions

There are **2 version slots**, each holding a complete set of curves plus configuration:

- the version chip lives in the header and is visible from all four tabs; double-click to rename it in place (16 characters or fewer; clearing it falls back to `V{n}`);
- **Copy to...** copies the whole current version into the other slot; this happens inside state, with **zero automation pollution**;
- switching versions is **not** an automation parameter (otherwise a write pass would record the switch into automation itself);
- each version owns its own full set of pan/vol/width automation parameters, and a write pass prints the set belonging to the **current** version.

## Writing the result into your DAW

1. Confirm the output switch is ON;
2. set the DAW's automation mode to **Write** or **Latch**;
3. play the range you want written (Range decides the engine's scope: follow / DAW loop / manual range);
4. switch back to Read when it is done.

Things worth knowing:

- The engine prints **30 lanes only** (15 tracks x pan/vol). You may automate width / MS Balance / Lead Select yourself, but the engine does not print them — **the host is always authoritative** for those.
- With the output switch **ON**, the DSP uses engine values (the parameters are just the outward-facing print head); with it **OFF**, the DSP uses the host parameter values.
- Switching versions, copying a version, editing segment values, and turning the output switch off **never** produce host automation events.
- Reopening a project saved with `output_enabled=ON` shows a load-guard banner: until you press "continue engine-driven", the plugin is loaded but silent on the automation side — **not a single gesture goes out**.
- Host-specific pitfalls (Cubase lane placement, REAPER not writing with the GUI closed, Pro Tools recording only the first loop pass, and so on) are in `docs/DAW_COMPATIBILITY.md`.

## Pan curve editor

The x axis is pan angle [-100, +100] and the y axis is gain in dB. There are three point types — **bell / shelf / cut** — each with a Q, and interpolation works the same way as an EQ curve. It describes "the gain correction applied at a given pan position", and pairs with automatic assignment to suppress or lift particular angular regions.

## Target width

Width is a **geometric angle scaling**: the assigned angle is multiplied by a coefficient, and the readout is shown as an angle (`+/-{θ}°`, where `θ = round(width% x 0.6)`, so 0 / 100 / 150% map to 0° / 60° / 90°).

**Why not M/S widening**: above a width of 1, M/S widening flips the polarity of amplitude-panned sources and mono compatibility collapses outright. Geometric angle scaling does not have that problem.

For stereo sources, width is the **spread** in the dual-pan model (pan being the centre of the arc); width=0 collapses the source to mono.

## Sessions and files

- **Segments, ranges, and 2 versions of curves plus configuration** (a few hundred KB) live in the Output's state and travel with the project.
- The **feature stream** is compressed and embedded in state by default; **above 8 MB it automatically moves to a sidecar file** in the system application-data directory, tied to the state by `sessionGUID`. The "Storage status" panel in Settings tells you which of the two you are on.
- Saving the project elsewhere or copying it to another machine does not carry the sidecar along. The plugin then says plainly that the feature file is missing, rather than pretending the data is still there.
- The Input's state holds only a channel id plus UI preferences; **the single source of truth for configuration is always the Output**.

## Troubleshooting

| Symptom | Likely cause | What to do |
|---|---|---|
| **Red pill at the top plus a red "version mismatch" banner** | Only one of the two plugins was updated | SCVB refuses half-compatible connections. Bring Input and Output up to the same version together |
| **The vocals suddenly revert to their raw, unbalanced image** | The host stopped calling the Output (Live device deactivated / FL smart disable) | SCVB has already fallen back to passthrough and recovers in about 5.5 s. **FL Studio users: turn smart disable off for the bus that hosts SCVB Output** — FL suspends plugins based on "input is silent", and the SCVB bus input is silent by design, which makes it unusually easy to suspend by mistake. Host-by-host wording is in `docs/DAW_COMPATIBILITY.md` |
| **One vocal track is silent** | That track's Input is connected to a healthy Output, but the Output never received its data (no channel selected / wrong group / channel conflict) | Check that Input's channel and group; check whether the track shows as online on the Output's Tracks page |
| **Installing Input killed the whole track** | Should not happen | With no healthy Output detected, Input falls back to passthrough automatically (hard rule 3). If a track really is dead, collect the output of "Copy diagnostics" in Settings and open an issue |
| **"Channel conflict" warning** | Two Inputs in the same group claim the same channel | Change the channel id on one of them, or move it to another group |
| **"Group X already has a primary Output; this instance is read-only"** | The group already has an active Output | A group may only have one active Output (hard rule 6). Remove the extra one, or move it to another group |
| **The "timeline gap / overlap" warning count is climbing** | Vocal track routing was changed / some track is not being picked up | **Do not export yet** (hard rule 9). Work through the common-pitfalls list in `docs/DAW_COMPATIBILITY.md` |
| **The whole image is skewed to one side** | Host pan on a vocal track or on the bus is not centred | Return every host pan to centre (hard rule 4) |
| **The exported audio differs from what you heard live** | A routing or ordering problem under offline rendering | Timeline addressing holds under offline rendering too; if it still differs, note your DAW and version and open an issue |
| **The write pass recorded nothing** | Wrong automation mode / a known pitfall in that DAW | Confirm Write or Latch; in REAPER, do not close the plugin GUI; see `docs/DAW_COMPATIBILITY.md` |
| **A track is disabled with a sample-rate mismatch notice** | That track's sample rate differs from the Output's | Use one sample rate throughout the project |

## FAQ

**Can I install only the Output?** No. Without Inputs there is no track data at all.

**What happens if I install only an Input?** That track stays in passthrough and will not go silent (hard rule 3), but you get no balancing either.

**Why can't more parameters be added?** The automation parameter surface is frozen at **123** (124 as the host sees it). Ableton Live's ceiling of 128 leaves only 4 spare, and Logic identifies parameters by index — adding or removing one would scramble the automation in every existing project. New requirements go into state instead.

**Can I go beyond 15 tracks?** Not in v1; 15 tracks per group. If you genuinely need more, use a second group (an independent bus domain), but the two groups are not balanced jointly.

**Is macOS supported?** v1 is Windows x64 / VST3 only.

**Can I edit the analysis result if I don't like it?** Yes, see "Manual touch-ups"; automatic re-analysis will not overwrite what you edited.

**Do I have to recapture after changing the VAD threshold?** No. What is stored is continuous features, so thresholds can be changed at any time with live preview.

## Known limits and the v2 roadmap

The full list is in `docs/KNOWN_ISSUES.md`. The main points:

- 15 tracks per group, 2 version slots;
- one active Output per group at a time;
- the Output reports no additional latency (by design, not a limitation);
- up to 40 ms at the tail of an old run may be missed when runs switch; replaying restores it;
- Input does in-place gain only, not in-place pan (which would double up with the Output's dual-pan);
- v2 directions: a precise-mode VAD (Silero), and more platforms.
