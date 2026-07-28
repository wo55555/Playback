# Playback

[![Discord](https://img.shields.io/badge/Discord-Playback-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.gg/mUhRUD8AM)
[![QQ](https://img.shields.io/badge/QQ-Playback-EB1923?style=for-the-badge&logo=qq&logoColor=white)](https://qm.qq.com/q/ufJatMDcha)

![English](https://img.shields.io/badge/English-inactive?style=for-the-badge)
[![简体中文](https://img.shields.io/badge/简体中文-informational?style=for-the-badge)](README_ZH.md)

Playback is a native [LeviLamina](https://github.com/LiteLDev/LeviLamina) client mod for recording, exporting, and replaying Minecraft Bedrock sessions. Its replay architecture is inspired by the Java Edition [Flashback](https://github.com/Moulberry/Flashback) mod and adapted to the Bedrock client lifecycle.

> [!WARNING]
> `0.1.0-alpha.1` is the first public test release. Keep backups of important worlds and recordings. Replay compatibility is not guaranteed across Minecraft, LeviLamina, or Playback version changes.

## Features

- Captures loaded chunks, block actors, entity movement, player state, time, and selected client-safe game packets.
- Writes replay snapshots and timeline data asynchronously to reduce recording stalls.
- Exports recordings as portable replay archives.
- Opens replays from a native main-menu browser in an isolated local replay world.
- Provides an in-game timeline for play/pause, seeking, speed control, and exiting the replay.
- Localizes commands, the replay editor, and the resource-pack UI in English and Simplified Chinese.

## Compatibility

- Minecraft Bedrock for Windows
- LeviLamina client `26.10.*`

> [!TIP]
> Playback is a client-only mod that can record sessions in both local worlds and multiplayer servers.

## Quick Start

### Install a release

1. Download `Playback-client-windows-x64.zip` and `playback-ui.mcpack` from the GitHub release.
2. Extract the included `playback` directory into the LeviLamina instance's `mods` directory.
3. Open the separately downloaded `playback-ui.mcpack` to import the UI resource pack into Minecraft.
4. In Minecraft, open **Settings > Global Resources > My Packs**, activate **Playback UI**, and restart the client.

> [!IMPORTANT]
> You must install and activate the `playback-ui.mcpack` resource pack, or the Playback replay button will not appear on the main menu.

The Playback button should now appear on the main menu.

### Record

Join a world, open the client command console, and use:

```text
record start
record pause
record stop
```

`record start` begins or resumes recording, `record pause` pauses capture, and `record stop` finishes and exports the replay. Exported `.zip` files are stored in Playback's `data/replays` directory.

### Replay

1. Return to the main menu and select **Playback**.
2. Choose a `.playback` or compatible `.zip` replay from the browser.
3. Wait for the isolated replay world and initial chunks to finish loading.
4. Use the bottom timeline to play, pause, seek, change speed, or jump to either end. Use **File > Exit Replay** to leave.

## Build From Source

Requirements:

- Visual Studio 2022 with the MSVC C++ toolchain
- [xmake](https://xmake.io/)
- Git

Configure and build a clean Release client target:

```powershell
xmake f -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

The packaged mod is written to `bin/playback/`, including the command and replay-editor translations under `bin/playback/lang/`. The build also generates `bin/playback/playback-ui.mcpack` from `resources/`, including the localized main-menu and replay-browser UI.

If prelink reports that `bedrock_runtime_data` cannot be found, refresh the package configuration and rebuild:

```powershell
xmake repo -u
xmake f -c -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

## Commands

| Command | Description |
| --- | --- |
| `playback version` | Show the loaded Playback version. |
| `record start` | Start or resume recording the current world. |
| `record pause` | Pause the active recording. |
| `record stop` | Stop recording and export the replay. |

## Languages

Playback currently includes English (`en_US`) and Simplified Chinese (`zh_CN`) translations. Command and replay-editor translations are stored in `src/lang/`; resource-pack UI translations are stored in `resources/texts/`.

## Development Status and Roadmap

- The recording, export, and replay GUIs are under active development and optimization.
- Multiplayer server recording and replay will receive further debugging; testing and feedback are welcome.
- Planned features include camera movement, video rendering, and video export.

## Known Limitations

- The replay format is still under development and may change during Alpha releases.
- Playback reconstructs recorded client-visible state; it is not a deterministic copy of the original server simulation.
- Pending scheduled ticks and server-owned systems such as villages, raids, and POI state are not currently persisted as authoritative simulation state.
- Compatibility must be checked again after updating Minecraft or LeviLamina.

Please report reproducible problems with logs, versions, and a minimal replay where possible.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the build, formatting, and pull request workflow.

Report security issues privately by following [SECURITY.md](SECURITY.md). Do not open a public issue for a security vulnerability.

## Acknowledgements

Special thanks to the [LeviLamina](https://github.com/LiteLDev/LeviLamina) maintainers and community for providing the native modding platform and tooling that make Playback possible, and to the [Flashback](https://github.com/Moulberry/Flashback) project and its contributors for the replay concepts and architecture that inspired Playback.

## License

Copyright (C) 2026 [wo555](https://github.com/wo55555)

Playback is released under the [GNU Affero General Public License v3.0](LICENSE). Distributed modifications must remain under AGPL-3.0 and provide their corresponding source code. Third-party components retain their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the `licenses/` directory.
