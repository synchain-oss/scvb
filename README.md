**English** | [简体中文](README.zh-CN.md)

[![License](https://img.shields.io/github/license/synchain-oss/scvb?style=flat-square)](LICENSE)
[![Build](https://img.shields.io/github/actions/workflow/status/synchain-oss/scvb/build-vst3.yml?branch=dev&style=flat-square&label=build)](../../actions)
[![pluginval](https://img.shields.io/badge/pluginval-strictness%205-brightgreen?style=flat-square)](https://github.com/Tracktion/pluginval)
[![Release](https://img.shields.io/github/v/release/synchain-oss/scvb?style=flat-square)](../../releases/latest)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64%20%C2%B7%20VST3-blue?style=flat-square)](#requirements)

# SCVB — Synchain Vocal Balancer

> Automatic pan and level balancing across a multi-singer vocal arrangement, as a pair of VST3 plugins.

SCVB is an open-source plugin project led by [Synchain](https://synchain.ca) — source and documentation are fully public, and you are welcome to use, modify, and redistribute it under the terms of the [GPL-3.0-or-later](LICENSE). If it saves you some time, come have a look at what else we make at [synchain.ca](https://synchain.ca); and if you like working with it, telling a friend or a colleague about Synchain is the best support we could ask for.

## What it does

Mixing engineers routinely spend hours drawing volume and pan automation across dozens of tracks of lead and backing vocals from different singers: keeping the voices from crowding the same spot in the stereo image, or making sure passages from different singers sit at a consistent perceived loudness. It is time-consuming and demanding work, mostly tedious and highly repetitive — and a meaningful way to aggravate the repetitive strain injuries that come with the job.

SCVB exists to solve exactly that and give you those hours back: it captures every vocal track, analyses them together, and gives each one a pan curve and a level curve, so the parts sit apart from one another instead of competing for the same spot, and so loudness stays close to consistent from passage to passage.

**Two plugins, one system.** **SCVB Input** sits on each vocal track and captures it; **SCVB Output** sits on the vocal bus, where it analyses, balances, sums, and replaces the bus input.

Not sold on what the engine came up with, or want it arranged differently? No problem. Much like the workflow around Waves' Vocal Rider — print first, then tune by hand — you can use the automation-write feature to print the engine's analysis into your host as automation, then fine-tune from there by hand — starting from a finished pass rather than a blank one should still save you a lot of time.

Up to 15 vocal tracks per group, 8 independent groups (A–H), 2 version slots. The automation parameter surface is frozen at 123 declared (124 host-visible); everything else lives in state.

## Screenshots

Screenshots ship with the first tagged release. Until then, the interface is described tab by tab in the [User Guide](docs/USER_GUIDE.md).

## Requirements

- Windows 10 1809+ or Windows 11, x64
- A VST3 host
- WebView2 Evergreen Runtime, for the editor UI (usually already present on Windows)

## Supported DAWs

<!-- 本表转贴自 docs/DAW_COMPATIBILITY.md §4(该节标题即「README 支持等级表(供 T39b 转贴)」)。
     真源在那一节:改等级只改那里,再同步回本表与 README.zh-CN.md 的对等表。 -->

Transcribed from [docs/DAW_COMPATIBILITY.md](docs/DAW_COMPATIBILITY.md) §4, which stays the source of truth for this table. Tier 1 = fully supported, Tier 2 = supported with limitations, Tier 3 = not supported.

| DAW | Version | Support tier | Status and known limits |
|---|---|---|---|
| Cubase | 14 / 15 | **Tier 1 (primary test host)** | S1 routing (realtime / offline / state) verified; automation write pending S2 on real hardware (known risk RD-01); automation hides in the Ins hidden lane; Input must sit in the last slot of the pre-fader section |
| REAPER | 7 (recommended) | **Tier 1 (conditional)** | S1 routing (realtime / offline) verified; may not write automation with the GUI closed (needs "process all notifications"); one project per machine |
| Ableton Live | 12 | **Tier 1 (conditional)** | 128-parameter ceiling (124 as counted here, 4 spare); Re-Enable Automation has to be clicked; S1/S2 pending |
| Studio One | 6 | **Tier 1 (conditional)** | Automation mode must be set to Write/Latch inside the plugin window; Dropout Protection changes the block size; S1/S2 pending |

> The "conditional" attached to Tier 1 will be resolved into a final tier once S2 automation testing runs on real hardware; some rows may drop to Tier 2. FL Studio is not in the v1 support matrix.

## Install

SCVB has no tagged release yet. Once it does, installing is:

1. **Recommended**: get `SCVB-vX.Y.Z-win64.zip` from the download page at **[synchain.ca](https://synchain.ca)**. You will find the install notes, update announcements, and the rest of our tools there too — have a wander, there may be something else that helps.
   You can also download it from this repository's Releases page.
2. verify the downloaded zip against the accompanying `.sha256`. **The authoritative checksum is the SHA-256 in the GitHub Release notes** (produced by CI at build time); the two should match — **if they do not, do not install it, and tell us**;
3. unzip, and copy `SCVB Input.vst3`, `SCVB Output.vst3`, and `SCVB Monitor.vst3` — the whole bundle folder in each case — into `C:\Program Files\Common Files\VST3\`;
4. rescan plugins in your DAW.

**Install both Input and Output.** Those two are a pair and share one version number; a mismatched pair refuses to connect, on purpose.

**SCVB Monitor is optional.** It is a read-only side window for watching pan movement and distribution across a whole group. It passes audio through untouched, exposes **no automation parameters at all**, and only ever reads the shared data — it never claims a slot and never writes to any shared segment, so adding or removing it cannot change what Input and Output do.

Until there is a release, build from source (below).

## Quick start

Create a stereo vocal bus and route every vocal track into it. Put an SCVB Input in the last slot of each vocal track's plugin chain and an SCVB Output in the first slot of the bus. Give each Input a channel id, then capture, analyse, and turn on the output. The [User Guide](docs/USER_GUIDE.md) walks through it in five minutes.

Before you start, read these. Breaking any one of them does not make the result worse — it breaks it:

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

## Build from source

```powershell
git clone https://github.com/synchain-oss/scvb.git
cd scvb
pwsh scripts/build.ps1 -JucePath C:\path\to\JUCE
```

See [CLAUDE.md](CLAUDE.md) §6 for the full toolchain list, and run `pwsh scripts/gates.ps1` for the local quality gates.

## Documentation

- [User Guide](docs/USER_GUIDE.md) — installation, workflow, troubleshooting, FAQ
- [Known issues](docs/KNOWN_ISSUES.md) — accepted v1 limitations
- [Release process](docs/RELEASE.md) — versioning, tags, release notes
- Contracts and architecture live in `docs/`: `PARAMETERS.md`, `IPC_CONTRACT.md`, `STATE_SCHEMA.md`, `SCVB_CONTRACT.md`
- Host-by-host notes live in [docs/DAW_COMPATIBILITY.md](docs/DAW_COMPATIBILITY.md)
- Read-only copies of the constitution documents are in `docs/constitution/`

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) and the [Code of Conduct](CODE_OF_CONDUCT.md). Every commit must be signed off (`git commit -s`). Security reports go through [SECURITY.md](SECURITY.md), not public issues.

The nine hard rules have a **single source of truth**: the `## 硬约束` section of [docs/USER_GUIDE.zh-CN.md](docs/USER_GUIDE.zh-CN.md), with translations in `docs/hard-rules.i18n.json`. Chinese is the semantic authority. Never edit the rules anywhere else — change the source, run `node scripts/gen-hard-rules.mjs`, and let the other six copies follow.

## License

[GPL-3.0-or-later](LICENSE), with the JUCE and VST3 SDK dependencies declared in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Related projects

- [synchain-oss/synchain-bridge](https://github.com/synchain-oss/synchain-bridge) — VST3 plugin bridging DAW audio into the browser
- [synchain-oss/synchain-cli](https://github.com/synchain-oss/synchain-cli) — `@synchain/cli` command-line client
- [synchain.ca](https://synchain.ca) — project website
