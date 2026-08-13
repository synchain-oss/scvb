# SCVB — Synchain Vocal Balancer

Multi-track vocal auto pan & level balancing for JUCE 8 / VST3.

## What it does

- **SCVB Input** — sits on each vocal track, captures audio and routes it (via shared memory) to the bus plugin, silencing the track's direct output to preserve DAW routing order.
- **SCVB Output** — sits on the vocal bus, reads each track's timeline window, applies per-track gain/pan, and sums into the bus.
- Automatic segment-level loudness balancing and equal-power pan assignment across up to 15 vocal tracks, 2 versions.
- Frozen automation parameter surface (123 declared / 124 host-visible), with everything else in state.

> **状态**:v1 开发中(仓库骨架阶段,T01)。尚未发布可安装版本。

## Requirements

- Windows 10/11 x64, a VST3 DAW
- WebView2 Evergreen Runtime(编辑器 UI)

## Build from source

```powershell
git clone https://github.com/synchain-oss/scvb.git
cd scvb
pwsh scripts/build.ps1 -JucePath C:\path\to\JUCE
```

See [CLAUDE.md](./CLAUDE.md) §6 for the full toolchain list, and `pwsh scripts/gates.ps1` for local gates.

## Documentation

- 架构与契约:`docs/`(PARAMETERS / IPC_CONTRACT / STATE_SCHEMA;正式内容由 T39a 蒸馏)
- 宪法原文只读副本:`docs/constitution/`

## Contributing

See [CONTRIBUTING.md](./CONTRIBUTING.md). All commits must be signed off(`git commit -s`).

## License

[GPL-3.0](./LICENSE)(with JUCE / VST3 SDK dependencies noted in [THIRD-PARTY-NOTICES.md](./THIRD-PARTY-NOTICES.md)).

## Related projects

- [synchain-oss/synchain-bridge](https://github.com/synchain-oss/synchain-bridge) — VST3 plugin bridging DAW audio into the browser
- [synchain-oss/synchain-cli](https://github.com/synchain-oss/synchain-cli) — `@synchain/cli` command-line client
