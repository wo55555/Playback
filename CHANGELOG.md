# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.1-mc26.20] - 2026-07-29

### Changed

- Adapted to Minecraft 26.20.4

## [0.1.0-alpha.2] - 2026-07-29

### Changed

- Bundled the Playback UI resource pack with the mod so LeviLauncher and Lip installations load it automatically through LeviLamina.
- Kept `playback-ui.mcpack` available as a standalone release asset for manual import.

  > **This release changes installation packaging only. The replay format and replay runtime behavior are unchanged from `0.1.0-alpha.1`.**

## [0.1.0-alpha.1] - 2026-07-27

### Added

- Client-side recording with asynchronous replay storage and export.
- Main-menu replay browser and isolated replay-world loading.
- Replay timeline controls for pause, seek, speed, and exit.
- Chunk snapshots, cached chunk replay, entity movement, and selected game-packet replay.
- English and Simplified Chinese localization for commands, the replay editor, and the resource-pack UI.

  > **This is the first public test release. Replay files and behavior may change before `1.0.0`.**
  > **Playback currently targets Windows x64 and the LeviLamina `26.10.*` client runtime.**

[0.1.1-mc26.20]: https://github.com/ShrBox/Playback/compare/v0.1.0-alpha.2...v0.1.1-mc26.20
[0.1.0-alpha.2]: https://github.com/ShrBox/Playback/compare/v0.1.0-alpha.1...v0.1.0-alpha.2
[0.1.0-alpha.1]: https://github.com/ShrBox/Playback/releases/tag/v0.1.0-alpha.1
