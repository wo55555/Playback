# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- Fixed the replay editor overlay never appearing while another overlay that detours DXGI `Present`, such as RivaTuner Statistics Server, is attached to the client. The overlay is now composited from the BGFX D3D12 submit path, which the replay browser reaches even when the present detour is bypassed.
- Fixed the overlay losing its swap-chain resources after a window resize that the `ResizeBuffers` detour did not observe.

## [0.2.0-mc26.10] - 2026-08-20

### Added

- Added an in-game cinematic camera workflow with camera creation, capture-at-playhead, keyframe editing, sequence binding, live preview, property inspection, and undo/redo.
- Added Smooth, Linear, Ease In, Ease Out, Ease In/Out, Hold, Hermite, and Cubic Bezier camera interpolation.
- Added experimental frame-by-frame H.264 MP4 and PNG-sequence export with configurable tick range, frame rate, resolution, SSAA, warm-up, progress, cancellation, finalization, output validation, and clear-frame retry handling.
- Added D3D11 and D3D12 framebuffer capture paths. D3D12 supports SSAA 1x/2x; D3D11 uses the stable 1x path.
- Added camera-driven observer/chunk tracking and stable-frame warm-up after fast movement or dimension changes.

### Changed

- Flattened the source tree into recording, replay, editor, state, keyframe, exporting, visuals, runtime, packet, and I/O domains.
- Made every recorded dimension change an interpolation boundary. Camera curves never connect across a dimension transition, including when an intermediate dimension has no keyframes.
- Made paused replay use the freely movable observer camera, while active keyframe preview/export uses the sampled timeline camera.
- Reworked replay rendering so preview and export share the camera timeline, entity-pose sampling, observer synchronization, and cross-dimension state machine.
- Unified entity rendering around one sampled pose per frame and temporarily suppressed native movement interpolation during export rendering.
- Reworked chunk recording to preserve native request-mode packets for local worlds while converting inline or cache-enabled server chunks into portable replay data after client decoding.
- Changed export preparation to rely on replay tick, dimension transition, UI stability, and warm-up state instead of requiring every chunk in a fixed camera neighborhood.
- Derived the packaged mod version from the nearest Git release tag so hotfix builds retain the `0.2.0-mc26.10` identifier.
- Set the product version to `0.2.0` and the MC 26.10 release identifier to `v0.2.0-mc26.10`.
- Kept the configuration version in `Config.h` and the recording-file snapshot context version at `1`; no configuration or replay migration layer is provided.
- Replays recorded before this hotfix on affected servers may already be missing portable chunk or custom-entity registry data. Those archives cannot be repaired and must be recorded again; complete `v0.2.0-mc26.10` archives do not require conversion.

### Removed

- Removed compatibility branches for older action payload layouts. Replay archives created by previous Playback releases are incompatible and must be recorded again.

### Fixed

- Fixed preview and export stalls when entering, revisiting, or leaving dimensions.
- Fixed forced-snapshot reader boundaries being considered export-ready before the next replay chunk was applied.
- Fixed camera paths loading or capturing the wrong chunk region during fast movement and cross-dimension export.
- Fixed high-speed player ghosting caused by native and replay render paths observing different interpolated positions.
- Fixed free-camera translation stutter, pause rebound, third-person offsets, mouse grab/release loops, and unnecessary pause-time server teleports.
- Fixed raw camera FOV units, D3D11 supersampling fallback, D3D12 clear-frame recovery, and SSAA values above the stable 2x limit.
- Fixed replay terrain appearing as air on servers that send inline or cache-enabled chunk data.
- Fixed local-world seek failures caused by duplicate chunk columns or missing subchunk sections after server compatibility conversion.
- Fixed resource-pack handshakes discarding actor-registry configuration required to reconstruct custom server entities in newly recorded replays.
- Fixed snapshot application choosing an older LevelChunk record when multiple versions of the same chunk column were present.
- Fixed offline video export stopping when Minecraft lost focus or attempted to open pause or suspend UI.
- Fixed valid exports remaining at frame 0 because a fixed 5x5 camera-neighborhood readiness check could never be satisfied.

## [0.1.2-mc26.10] - 2026-08-03

### Added

- Added replay-browser search, playable/unavailable filters, five sorting modes, grid/details views, import, rename, multi-selection deletion, path copying, and File Explorer integration.
- Added replay thumbnail capture into `icon.png` and thumbnail display in the browser on both D3D11 and D3D12.
- Added in-memory editor groundwork for sequence and world-actor segment operations, camera-track data and keyframes, and undo/redo. Project persistence and video export remain unavailable.
- Added D3D11 rendering support for the native replay UI alongside the existing D3D12 path.

### Changed

- Changed the replay metadata and snapshot stream to require per-chunk dimension/view context, explicit local-player creation, and forced snapshot boundaries. Replays created by `v0.1.1` or earlier are not compatible and must be recorded again after upgrading.
- Kept the internal `Config` version field at its initial value (`1`) and left third-party dependency version declarations unchanged. No configuration or replay migration path is provided.
- Replaced the resource-pack-based replay browser with a native ImGui browser while retaining a lightweight resource-pack button on the main menu.
- Rebuilt the existing replay editor around resizable viewport, details, and timeline panels, zoomable tracks, viewport maximize/restore, and revised transport controls.
- Refactored replay snapshots to carry dimension context and local-player state explicitly, and to force snapshot playback at dimension boundaries.
- Unified command, replay-browser, and editor messages through the English and Simplified Chinese catalogs using LeviLamina's native translation path.
- Bundled the Lucide icon font directly with the mod and reduced the UI resource pack to its main-menu button, bilingual label, and required metadata.
- Consolidated the feature overview in the root README files and replaced the legacy documentation tree with focused installation and source-build guides.

### Fixed

- Restored installed and standalone `playback-ui.mcpack` packaging so the main-menu button is available in complete mod installations and as a separate Release asset.
- Reworked cross-dimension recording and replay handling to close the source chunk at the tick boundary, wait for the target dimension, and apply a dimension-scoped forced snapshot.
- Prevented ordinary forward seeks from reloading snapshots and moving the viewer; snapshot reloads remain for backward seeks, dimension changes, and forced boundaries.
- Improved replay snapshot cleanup and chunk application by refreshing the replay player, isolating chunks by dimension, filtering unsuccessful subchunk responses, and clearing recorded objectives between snapshots.
- Expanded D3D12 command-queue and swap-chain capture to cover additional renderer creation paths.

## [0.1.1-mc26.10] - 2026-07-29

### Added

- Added PDB symbols to release assets to help diagnose crashes.

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

[Unreleased]: https://github.com/wo55555/Playback/compare/v0.2.0-mc26.10...HEAD
[0.2.0-mc26.10]: https://github.com/wo55555/Playback/compare/v0.1.2-mc26.10...v0.2.0-mc26.10
[0.1.2-mc26.10]: https://github.com/wo55555/Playback/compare/v0.1.1-mc26.10...v0.1.2-mc26.10
[0.1.1-mc26.10]: https://github.com/wo55555/Playback/compare/v0.1.0-alpha.2...v0.1.1-mc26.10
[0.1.0-alpha.2]: https://github.com/wo55555/Playback/compare/v0.1.0-alpha.1...v0.1.0-alpha.2
[0.1.0-alpha.1]: https://github.com/wo55555/Playback/releases/tag/v0.1.0-alpha.1
