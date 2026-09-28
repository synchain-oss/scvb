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

**Two plugins, one system.** **SCVB Input** sits on each vocal track and captures it; **SCVB Output** sits on the vocal bus, where it analyses, balances, sums, and replaces the bus input. A third, optional plugin, **SCVB Monitor**, is a read-only window for watching a whole group's pan and level movement. The interface is described tab by tab in the [User Guide](docs/USER_GUIDE.md).

Not sold on what the engine came up with, or want it arranged differently? No problem. Much like the workflow around Waves' Vocal Rider — print first, then tune by hand — you can use the automation-write feature to print the engine's analysis into your host as automation, then fine-tune from there by hand — starting from a finished pass rather than a blank one should still save you a lot of time.

Up to 15 vocal tracks per group, 8 independent groups (A–H), 2 version slots. The automation parameter surface is frozen at 123 declared (124 host-visible); everything else lives in state.

## Requirements

- Windows 10 1809+ or Windows 11, x64
- A VST3 host
- WebView2 Evergreen Runtime, for the editor UI (usually already present on Windows)

## Supported DAWs

<!-- 本表转贴自 docs/DAW_COMPATIBILITY.md §4(该节标题即「README 支持等级表(供 T39b 转贴)」)。
     真源在那一节:改等级只改那里,再同步回本表与 README.zh-CN.md 的对等表。 -->

Transcribed from [docs/DAW_COMPATIBILITY.md](docs/DAW_COMPATIBILITY.md) §4, which stays the source of truth for this table. Tier 1 = fully supported, Tier 2 = supported with limitations (with a workaround you can apply yourself), Tier 3 = untested or not supported.

| DAW | Version | Support tier | Status and known limits |
|---|---|---|---|
| Cubase | 14 / 15 | **Tier 1 (primary test host)** | Routing (realtime / offline), project save and reopen, and automation write verified on real hardware with the finished plugins (Cubase 15 Pro); routing also verified on Cubase 14 during the routing spike; automation hides in the Ins hidden lane; Input must sit in the last slot of the pre-fader section |
| REAPER | 7 | **Tier 2 (partly verified)** | Routing (realtime / offline) verified during the routing spike only; the finished plugins and automation write have not been tested in REAPER; may not write automation with the GUI closed (needs "process all notifications"); one project per machine |
| Ableton Live | 12 | **Tier 3 (untested)** | Not yet tested on real hardware. Known from the design: 128-parameter ceiling (124 used here, 4 spare); Re-Enable Automation has to be clicked; deactivating the Output device gives about 5.5 s of silence before the vocals fall back to passthrough |
| Studio One | 6 | **Tier 3 (untested)** | Not yet tested on real hardware. Known from the design: automation mode must be set to Write/Latch inside the plugin window; Dropout Protection changes the block size |

> Untested hosts may well work — the plugins are standard VST3 — but nobody has confirmed it on a real machine yet. Tiers move up only after a real-hardware test. FL Studio is not in the v1 support matrix.

## Install

Releases are published on this repository's [Releases page](https://github.com/synchain-oss/scvb/releases). If no release is listed there yet, build from source (below).

1. From the Releases page, download `SCVB-v<version>-win64.zip` and the matching `.sha256`;
2. verify the zip against the `.sha256`. **The authoritative checksum is the SHA-256 in the GitHub Release notes** (produced by CI at build time); the two should match — **if they do not, do not install it, and tell us**;
3. **SCVB is not code-signed.** Your browser or Windows may warn that the file comes from an unknown publisher; before unzipping, right-click the zip → **Properties** → tick **Unblock** → **OK**. The [User Guide](docs/USER_GUIDE.md#install) has the step-by-step version;
4. unzip, and copy `SCVB Input.vst3`, `SCVB Output.vst3`, and (optionally) `SCVB Monitor.vst3` — the whole bundle folder in each case — into `C:\Program Files\Common Files\VST3\`;
5. rescan plugins in your DAW.

**Install both Input and Output.** Those two are a pair and share one version number. **When you upgrade, upgrade every SCVB plugin together**: if the two sides speak different versions of the shared-memory protocol they refuse to connect, on purpose; and even when they do connect, mixing versions is not supported (with a new Output and an old Input, an offline render can sum the vocals twice).

**SCVB Monitor is optional.** It is a read-only side window for watching pan movement and distribution across a whole group. It passes audio through untouched, exposes **no automation parameters at all**, and only ever reads the shared data — it never claims a slot and never writes to any shared segment, so adding or removing it cannot change what Input and Output do.

## Quick start

Create a stereo vocal bus and route every vocal track into it. Put an SCVB Input in the last slot of each vocal track's plugin chain and an SCVB Output in the first slot of the bus. Give each Input a channel id, then capture, analyse, and turn on the output. The [User Guide](docs/USER_GUIDE.md) walks through it in five minutes.

Before you start, read these. Breaking any one of them does not make the result worse — it breaks it:

<!-- BEGIN GENERATED hard-rules:en -->
> ⚠️ **Must read: SCVB's nine usage rules. Breaking any one of them causes silence, wrong panning, or failed analysis.**
>
> 1. **Vocal tracks must keep their original DAW routing, pointing at the bus that hosts SCVB Output.** Do not re-route a vocal track straight to the master output, and do not bypass the bus. (ADR-002)
> 2. **SCVB Input must sit in the last slot of the vocal track's plugin chain; SCVB Output must sit in the first slot of the bus.** Any other position breaks the processing-order assumption SCVB relies on; for what each host calls that slot, see `https://github.com/synchain-oss/scvb/blob/prod/docs/DAW_COMPATIBILITY.md`. (ADR-002 / J45)
> 3. **Input mutes its downstream output only while a healthy SCVB Output is detected — this is by design, not a bug.** That mute path is what preserves the "vocal tracks first, bus second" ordering in the DAW's dependency graph, and it still holds under offline rendering and REAPER's anticipative multithreading. **When no healthy Output is detected (not installed, not connected, peer has quit), Input falls back to passthrough automatically**, over an 80 ms ramp with a 5-second hysteresis debounce (the hysteresis applies only to the "mute → passthrough" direction; "passthrough → mute" ramps over 80 ms as soon as health is confirmed), so installing only one of the two plugins will never leave you with a dead track. (ADR-002 / J12 + J32)
> 4. **Host pan must stay centred on both the vocal tracks and the bus.** SCVB pans internally with an equal-power law, independently of the host's pan law; an off-centre host pan stacks on top of it and produces a wrong stereo image. (ADR-010)
> 5. **Each channel id is unique within one group, and a given vocal track may belong to only one group.** When two Inputs in the same group claim the same channel, the late arrival shows a "channel conflict" warning and stays inactive; the same channel number in a different group is a separate, unrelated path. (ADR-002 / J66)
> 6. **Only one Output instance can be active in a group at any one time.** A second instance in the same group drops into read-only observer mode and shows a warning; the eight groups (A–H) are independent bus domains and do not affect one another. (ADR-002 / J66)
> 7. **Every track takes part in automatic pan by default; if a stereo track should keep its existing stereo width and position, switch off "participate in auto pan" for that track on the Tracks page.** Mono sources are placed with equal-power pan; stereo sources use a dual-pan + width model (pan = centre of the arc, width = spread), and once participation is switched off the stereo width you already have is preserved rather than overwritten by automatic assignment. (ADR-003 / J57 + J83)
> 8. **SCVB Output reports no additional latency to the DAW.** Alignment is done by timeline addressing; do not try to "correct" it with PDC (plugin delay compensation). (ADR-002)
> 9. **Do not carry on exporting while a "timeline gap / overlap" warning is showing.** Work through the common-pitfalls list in `https://github.com/synchain-oss/scvb/blob/prod/docs/DAW_COMPATIBILITY.md` to check your routing first: for as long as the warning count refuses to fall back to zero, some track's audio is not being picked up correctly.
<!-- END GENERATED hard-rules:en -->

## Privacy

SCVB does not use the network: no update check, no usage statistics, no account. The only web pages it opens are the documentation and WebView2 download links, in your default browser, when you click them. It stores a few cross-project preferences under `%APPDATA%\Synchain\SCVB` and the interface's browser cache under `%LOCALAPPDATA%\Synchain\SCVB`; everything else is saved in your project, apart from files you export yourself. The full list is in the [User Guide, "Privacy and files on disk"](docs/USER_GUIDE.md).

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

The source code in this repository is [GPL-3.0-or-later](LICENSE). The released `.vst3` binaries also contain GPLv3-only and AGPLv3 components, so they are distributed under GPLv3 as a whole (details in THIRD-PARTY-NOTICES.md). Third-party components built into the plugins (JUCE, the VST3 SDK, the libraries JUCE compiles in, the WebView2 loader, fonts) and their licences are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md); full licence texts are in `LICENSES/`.

## Related projects

- [synchain-oss/synchain-bridge](https://github.com/synchain-oss/synchain-bridge) — VST3 plugin bridging DAW audio into the browser
- [synchain-oss/synchain-cli](https://github.com/synchain-oss/synchain-cli) — `@synchain/cli` command-line client
- [synchain.ca](https://synchain.ca) — project website
